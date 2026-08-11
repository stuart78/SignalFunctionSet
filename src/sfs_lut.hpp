#pragma once
// Shared sine / cosine / Hann lookup tables for SFS modules.
// Initialized once at static-init time and reused across modules.
//
// Each table covers a full period (phase 0..1) with linear interpolation
// between samples. 1024 entries gives a worst-case interpolation error
// well below 16-bit audio noise floor for the sine/cosine tables.

#include <cmath>

namespace sfs_lut {

constexpr int SIZE = 1024;

struct Tables {
	float sine[SIZE + 1];   // sin(phase * 2π) for phase ∈ [0,1]
	float cosine[SIZE + 1]; // cos(phase * 2π) for phase ∈ [0,1]
	float hann[SIZE + 1];   // 0.5 * (1 - cos(phase * 2π))
	Tables() {
		for (int i = 0; i <= SIZE; i++) {
			float phase = (float)i / (float)SIZE;
			float angle = phase * 2.f * (float)M_PI;
			sine[i]   = std::sin(angle);
			cosine[i] = std::cos(angle);
			hann[i]   = 0.5f * (1.f - cosine[i]);
		}
	}
};

inline const Tables& tables() {
	static const Tables t;
	return t;
}

// Wrap phase into [0,1) and look up with linear interpolation.
inline float lookup(const float* lut, float phase) {
	// Wrap (cheap modulo for small overshoots; assume |phase| not huge)
	phase -= (int)phase;
	if (phase < 0.f) phase += 1.f;
	float idxF = phase * (float)SIZE;
	int idx = (int)idxF;
	float frac = idxF - (float)idx;
	return lut[idx] + (lut[idx + 1] - lut[idx]) * frac;
}

inline float sine(float phase)   { return lookup(tables().sine,   phase); }
inline float cosine(float phase) { return lookup(tables().cosine, phase); }


// ---- Fast pow2 ----------------------------------------------------------
// std::pow(2, x) shows up in the per-sample hot path of nearly every module
// (V/Oct conversion). It dispatches to a generic exp(x * ln 2) which is
// expensive on the MetaModule's Cortex-A7 — Intone alone calls it 7 times
// per sample.
//
// Strategy: 2^x = 2^floor(x) * 2^frac(x). The integer part is a free
// scalbn/ldexp (single-instruction exponent shift). The fractional part is
// in [0,1); we LUT-interpolate it from POW2_SIZE+1 precomputed values of
// 2^(i/POW2_SIZE).
//
// At 1024 entries the worst-case linear-interpolation error vs std::pow is
// well below 0.001 cents, far below any audible threshold.
constexpr int POW2_SIZE = 1024;

struct Pow2Table {
	float frac[POW2_SIZE + 1];  // 2^(i/POW2_SIZE) for i in [0, POW2_SIZE]
	Pow2Table() {
		for (int i = 0; i <= POW2_SIZE; i++) {
			frac[i] = std::pow(2.f, (float)i / (float)POW2_SIZE);
		}
	}
};

inline const Pow2Table& pow2Table() {
	static const Pow2Table t;
	return t;
}

inline float pow2(float x) {
	// Split into integer and fractional parts.
	// Use floor (not truncation) so negative x with a fractional component
	// gets the right integer part: e.g. -1.25 -> integer -2, frac 0.75.
	float floored = std::floor(x);
	int ipart = (int)floored;
	float frac = x - floored; // always in [0, 1)
	// Look up 2^frac with linear interpolation
	float idxF = frac * (float)POW2_SIZE;
	int idx = (int)idxF;
	float t = idxF - (float)idx;
	const float* lut = pow2Table().frac;
	float fracPart = lut[idx] + (lut[idx + 1] - lut[idx]) * t;
	// Multiply by 2^ipart via exponent manipulation
	return std::ldexp(fracPart, ipart);
}

// Fast a^x for a > 0: a^x = 2^(x * log2(a)). Caller can precompute log2(a).
inline float powFromLog2(float log2a, float x) {
	return pow2(log2a * x);
}

inline float hann(float phase) {
	// Hann is non-periodic in our usage (phase ∈ [0,1] over a grain's
	// lifetime), so don't wrap — clamp instead.
	if (phase < 0.f) phase = 0.f;
	else if (phase > 1.f) phase = 1.f;
	float idxF = phase * (float)SIZE;
	int idx = (int)idxF;
	// The table has SIZE+1 entries so the interpolation always has a right-hand
	// neighbour -- but at phase == 1.0 exactly, idx IS SIZE and idx+1 runs off
	// the end. frac is zero there so the value is multiplied away, which is why
	// this never showed up as a wrong number; it is still a read past the array,
	// and had the byte after it ever held an inf, inf * 0 would have put a NaN
	// into the signal. Found by cppcheck (issue #11).
	if (idx >= SIZE) return tables().hann[SIZE];
	float frac = idxF - (float)idx;
	return tables().hann[idx] + (tables().hann[idx + 1] - tables().hann[idx]) * frac;
}

} // namespace sfs_lut
