/*
 * SFS addition. See fm_matrix.h.
 */
#include <cstring>

#include "synth.h"
#include "fm_matrix.h"
#include "fm_op_kernel.h"

// FmCore's flag bits. The table itself comes from fm_core.h -- read, never copied.
enum {
	OUT_BUS_ONE = 1 << 0, OUT_BUS_TWO = 1 << 1, OUT_BUS_ADD = 1 << 2,
	IN_BUS_ONE  = 1 << 4, IN_BUS_TWO  = 1 << 5,
	FB_IN = 1 << 6, FB_OUT = 1 << 7
};

// FmCore skips an operator whose gain never reaches this, and marks the bus it
// would have written as empty. Reproduced here or the endpoints do not match:
// a skipped operator contributes EXACTLY zero, while a computed one at the same
// gain contributes something very small but non-zero.
static const int kLevelThresh = 1120;

void fmMatrixFromAlgorithm(int algorithm, FmMatrix& m) {
	std::memset(&m, 0, sizeof(m));
	m.fbSrc = m.fbDst = -1;
	if (algorithm < 0 || algorithm > 31) algorithm = 0;
	const FmAlgorithm& alg = algorithms[algorithm];

	// Run the bus machine symbolically: whatever is standing in a bus when an
	// operator reads it is exactly the set of operators that modulate it.
	int bus[3][6], busN[3] = {0, 0, 0};
	for (int i = 0; i < 6; i++) {
		int flags  = alg.ops[i];
		int inbus  = (flags >> 4) & 3;
		int outbus = flags & 3;
		bool add   = (flags & OUT_BUS_ADD) != 0;
		if (inbus)
			for (int k = 0; k < busN[inbus]; k++) m.w[bus[inbus][k]][i] = FmMatrix::ONE;
		if (flags & FB_IN)  m.fbDst = i;
		if (flags & FB_OUT) m.fbSrc = i;
		if ((flags & 7) == OUT_BUS_ADD) {          // writes the output bus
			m.w[i][6] = FmMatrix::ONE;
		} else if (outbus) {
			if (!add) busN[outbus] = 0;            // replace rather than accumulate
			bus[outbus][busN[outbus]++] = i;
		}
	}
	// FmCore only implements feedback when both flags land on the SAME operator
	// ("todo: more than one op in a feedback loop"), so algorithms 4 and 6 --
	// the two-operator loops -- run without theirs. Matched here deliberately:
	// this is meant to reproduce the engine, not to improve on it behind its back.
	if (m.fbSrc != m.fbDst) m.fbSrc = m.fbDst = -1;
}

void fmMatrixBlend(const FmMatrix& a, const FmMatrix& b, int32_t t, FmMatrix& m) {
	if (t <= 0) { m = a; return; }                 // exact endpoints, so a morph
	if (t >= FmMatrix::ONE) { m = b; return; }     // parked at either end is
	for (int i = 0; i < 6; i++)                    // bit-identical to the engine
		for (int j = 0; j < 7; j++)
			m.w[i][j] = (int32_t)(((int64_t)a.w[i][j] * (FmMatrix::ONE - t)
			                     + (int64_t)b.w[i][j] * t) >> 15);
	// A blend runs BOTH feedback loops at partial depth; carrying only one of
	// them would make the morph jump when the other patch's loop took over.
	m.fbSrc = a.fbSrc; m.fbDst = a.fbDst;
	if (m.fbDst < 0) { m.fbSrc = b.fbSrc; m.fbDst = b.fbDst; }
}

// Scale a buffer by a Q15 weight and accumulate. ONE and 0 take integer paths,
// which is what keeps a single-algorithm matrix bit-exact against FmCore.
static inline void accum(int32_t* dst, const int32_t* src, int32_t w, bool& any) {
	if (w == 0) return;
	if (w == FmMatrix::ONE) {
		if (any) for (int i = 0; i < N; i++) dst[i] += src[i];
		else     for (int i = 0; i < N; i++) dst[i]  = src[i];
	} else {
		if (any) for (int i = 0; i < N; i++) dst[i] += (int32_t)(((int64_t)src[i] * w) >> 15);
		else     for (int i = 0; i < N; i++) dst[i]  = (int32_t)(((int64_t)src[i] * w) >> 15);
	}
	any = true;
}

void FmMatrixCore::compute(int32_t* output, FmOpParams* params, const FmMatrix& m,
                           int32_t* fb_buf, int feedback_shift) {
	bool live[6];
	for (int op = 0; op < 6; op++) {
		FmOpParams& param = params[op];
		int32_t gain1 = param.gain[0], gain2 = param.gain[1];
		int32_t* out = opbuf_[op].get();

		// Below threshold: contribute exactly zero, as FmCore does by not
		// writing the bus at all.
		if (gain1 < kLevelThresh && gain2 < kLevelThresh) {
			std::memset(out, 0, N * sizeof(int32_t));
			live[op] = false;
			param.phase += param.freq << LG_N;
			continue;
		}
		live[op] = true;

		bool any = false;
		int32_t* in = inbuf_.get();
		for (int src = 0; src < op; src++)
			if (live[src]) accum(in, opbuf_[src].get(), m.w[src][op], any);

		bool kernelWroteFb = false;
		if (!any) {
			if (m.fbDst == op && feedback_shift < 16) {
				// The engine's own feedback path: recomputed per sample, and it
				// maintains fb_buf itself.
				FmOpKernel::compute_fb(out, param.phase, param.freq,
				                       gain1, gain2, fb_buf, feedback_shift, false);
				kernelWroteFb = true;
			} else {
				FmOpKernel::compute_pure(out, param.phase, param.freq,
				                         gain1, gain2, false);
			}
		} else {
			// An operator that is modulated AND is the feedback destination:
			// unreachable in FmCore, so it only ever arises part-way through a
			// blend. compute_fb takes no input buffer, so the loop is folded in
			// as one value held across the block rather than recomputed per
			// sample. That is an approximation, and it is confined to blended
			// states -- both endpoints take the exact path above.
			if (m.fbDst == op && feedback_shift < 16) {
				int32_t fb = (fb_buf[0] + fb_buf[1]) >> (feedback_shift + 1);
				for (int i = 0; i < N; i++) in[i] += fb;
			}
			FmOpKernel::compute(out, in, param.phase, param.freq, gain1, gain2, false);
		}
		// Close the loop for every case the kernel did not close itself.
		if (m.fbSrc == op && !kernelWroteFb) { fb_buf[0] = out[N - 2]; fb_buf[1] = out[N - 1]; }
		param.phase += param.freq << LG_N;
	}

	bool any = false;
	for (int op = 0; op < 6; op++)
		if (live[op]) accum(output, opbuf_[op].get(), m.w[op][6], any);
	if (!any) std::memset(output, 0, N * sizeof(int32_t));
}
