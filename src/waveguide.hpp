#pragma once
// =============================================================================
// Shared parts of a waveguide string.
//
// A vibrating string, as a delay loop: the delay length is the pitch, an in-loop
// lowpass takes the treble first (as a real string does), a chain of allpasses
// stretches the partials sharp (stiffness), and a DC blocker sits INSIDE the
// loop — on the input it would do nothing, because it is the recirculation that
// accumulates the offset.
//
// WHAT IS DELIBERATELY NOT HERE: the loop body itself. It is eight lines, and in
// Loom the bridge coupling is injected part-way through it — so folding it into
// a shared step() would either change the order of two float additions (which is
// not associative, and Loom's tuning was verified bit-exact) or need a signature
// contorted enough to be worse than the eight lines it replaced. Excitation is
// out for the same reason: a bow and a pick have nothing in common.
//
// So this is the delay line, the exact phase-delay maths, a filter, and a
// limiter — the parts that are the same everywhere and will stay that way.
// =============================================================================

#include <cmath>
#include <cstring>

namespace sfs {

// The loop's filters delay the signal as well as shaping it, and that delay is
// part of the pitch. Estimating it at DC leaves the string measurably sharp —
// and sharper the higher it is tuned, because the error is a fixed number of
// samples against a shrinking period. So both are evaluated exactly, at the
// string's own fundamental, and subtracted from the delay line.
static inline float allpassDelay(float a, float w) {
	float cw = std::cos(w), sw = std::sin(w);
	float argN = std::atan2(-sw, a + cw);
	float argD = std::atan2(-a * sw, 1.f + a * cw);
	return -(argN - argD) / w;
}
static inline float onePoleDelay(float c, float w) {
	float b = 1.f - c;
	return std::atan2(b * std::sin(w), 1.f - b * std::cos(w)) / w;
}

// A two-pole state-variable filter — body resonances, pickup peaks, narrow
// noise bands.
struct SVF {
	float ic1 = 0.f, ic2 = 0.f;
	float a1 = 0.f, a2 = 0.f, a3 = 0.f;
	void set(float freq, float q, float sr) {
		float f = freq < 20.f ? 20.f : (freq > sr * 0.45f ? sr * 0.45f : freq);
		float g = std::tan((float)M_PI * f / sr);
		float k = 1.f / (q < 0.5f ? 0.5f : q);
		a1 = 1.f / (1.f + g * (g + k));
		a2 = g * a1;
		a3 = g * a2;
	}
	float bandpass(float v0) {
		float v3 = v0 - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		return v1;
	}
	// Both outputs from one pass. A resonant lowpass — which is what a magnetic
	// pickup is — needs the lowpass and the bandpass of the SAME state, and
	// calling bandpass() then lowpass() would advance the filter twice.
	void process(float v0, float& lo, float& band) {
		float v3 = v0 - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		band = v1; lo = v2;
	}
	void clear() { ic1 = ic2 = 0.f; }
};

// BUF must be a power of two and at least 1.6× the longest loop in samples —
// the pluck-position comb reads a second tap past the loop length.
template <int BUF>
struct DelayLine {
	enum { MASK = BUF - 1 };
	float buf[BUF];
	int   wi;

	DelayLine() { clear(); }
	void clear() { std::memset(buf, 0, sizeof(buf)); wi = 0; }

	float tap(float d) const {
		float rp = (float)wi - d;
		while (rp < 0.f) rp += (float)BUF;
		int i0 = (int)rp;
		float fr = rp - (float)i0;
		return buf[i0 & MASK] * (1.f - fr) + buf[(i0 + 1) & MASK] * fr;
	}

	// The 1e-20 is the anti-denormal: a loop this quiet costs real CPU on x86
	// once the samples go subnormal.
	void write(float x) { buf[wi] = x + 1e-20f; wi = (wi + 1) & MASK; }
};

// Linear to ±T, then bending, asymptotic to ±(T+K). Several sustained voices
// summed have a crest factor a single pluck never approaches, and a hard clamp
// is the harshest thing an overload can do.
static inline float softClip(float x, float T = 6.f, float K = 4.f) {
	float a = std::fabs(x);
	if (a <= T) return x;
	return std::copysign(T + K * (1.f - K / (K + a - T)), x);
}

}  // namespace sfs
