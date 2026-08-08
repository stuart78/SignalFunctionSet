/*
 * SFS addition: a matrix formulation of the DX7 operator routing, so two
 * algorithms can be blended into structures the DX7 has no number for.
 *
 * An algorithm in FmCore is a stack machine over three buses. That is a fast
 * evaluation of something simpler: a table of "how much does operator i
 * modulate operator j", plus "how much of operator i reaches the output".
 * Every one of the 32 algorithms evaluates in operator order 0->5 with each
 * modulator strictly BEFORE what it modulates, so that table is strictly upper
 * triangular -- 15 modulation weights and 6 output weights, 21 numbers, and a
 * convex blend of two of them is still upper triangular and still evaluable in
 * the same order. That is what makes morphing well defined for every pair.
 *
 * Feedback is the one edge that runs backwards, so it is not in the matrix; it
 * is a delayed path, exactly as FmCore treats it.
 *
 * Weights are Q15. 0 and ONE are given exact integer paths, so a matrix built
 * from a single algorithm reproduces FmCore sample for sample -- the morph is
 * transparent at its endpoints or it is not worth having.
 */
#ifndef __FM_MATRIX_H
#define __FM_MATRIX_H

#include "synth.h"
#include "aligned_buf.h"
#include "fm_core.h"

struct FmMatrix {
	enum { ONE = 1 << 15 };            // Q15 unity
	int32_t w[6][7];                   // [src][dst]; column 6 is the output
	int     fbSrc, fbDst;              // the delayed feedback edge, -1 if none
};

// Fill `m` from one of the 32 DX7 algorithms (0..31).
void fmMatrixFromAlgorithm(int algorithm, FmMatrix& m);

// m = (1-t)*a + t*b, with t in Q15. Endpoints are returned exactly.
void fmMatrixBlend(const FmMatrix& a, const FmMatrix& b, int32_t t, FmMatrix& m);

class FmMatrixCore {
public:
	void compute(int32_t* output, FmOpParams* params, const FmMatrix& m,
	             int32_t* fb_buf, int feedback_shift);
private:
	AlignedBuf<int32_t, N> opbuf_[6];  // each operator's own output this block
	AlignedBuf<int32_t, N> inbuf_;     // the weighted sum feeding one operator
};

#endif  // __FM_MATRIX_H
