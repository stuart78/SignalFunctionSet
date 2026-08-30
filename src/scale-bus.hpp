#pragma once
// =============================================================================
// The scale bus — how a whole scale travels between modules.
//
// A SCALE CV of 1V per scale can only name one of the nineteen entries in
// `scales.hpp`. It cannot say "Pelog", let alone "Pelog, three cents wide", and
// it certainly cannot say "Bohlen-Pierce, thirteen degrees repeating at 3/1".
// So an index alone cannot carry a custom mask or a Scala file, and any module
// downstream of Key was quietly hearing the nearest canonical match instead of
// the key that was actually set.
//
// The answer is that SCALE OUT is POLYPHONIC, and the extra channels carry the
// scale itself:
//
//   ch 0          scale index, 1V per scale — the nearest canonical scale when
//                 the real one cannot be named by an index
//   ch 1          period, in volts (12 semitones = 1V)
//   ch 2 .. n+1   the n degrees as 1V/oct offsets from the root; ch 2 is 0
//
// THE EXTENSION IS INVISIBLE TO ANYONE WHO DOES NOT WANT IT. Rack's
// `getVoltage()` returns channel 0 whatever the channel count, so a module that
// only understands an index keeps working unchanged and costs nothing. The
// index stays as a lossy summary for whoever only wants a summary, and the real
// scale rides right behind it for whoever can use it.
//
// This lived inside key.cpp until Note needed it too. A wire format with one
// implementation is a private detail; a wire format with two copy-pasted
// implementations is a bug waiting for someone to fix only one of them. Hence
// this header: ONE encoder, ONE validated decoder, shared by every module that
// speaks the convention.
//
// See docs/conventions/scales.md.
// =============================================================================

#include "plugin.hpp"
#include "scales.hpp"
#include <cmath>
#include <string>

namespace sfs {

// 16 channels less the index and the period. Bohlen-Pierce needs 13, so this is
// not as generous as it looks.
static const int BUS_MAXDEG = 14;

// A scale reduced to what a consumer actually needs: the degrees, and how far
// apart the repeats are. NOT a 12-bit pitch mask -- the harmonic series, Pelog
// and Slendro all carry fractional intervals before Scala is even involved, and
// a mask cannot hold any of them.
struct BusScale {
	// Named to match sfs::Scale, so a consumer swaps only where the scale comes
	// from and every `sc.intervals[k]` / `sc.size` below it keeps working.
	float intervals[BUS_MAXDEG] = {};
	int   size = 0;
	float period = 12.f;          // semitones per repeat
	int   index = 0;              // the channel-0 summary
	bool  extended = false;       // came off the bus rather than the list
	// Degrees the source had that did not fit. A 31-note Scala scale arrives
	// as 14 degrees and `dropped` = 17. Without it a consumer cannot tell a
	// scale that genuinely has fourteen degrees from one cut down to fourteen,
	// and so cannot say the one thing the reader needs to know.
	int   dropped = 0;

	// Degree `k`, continuing past the end by repeating at the period, so a
	// caller can ask for a row above the scale without a special case.
	float degree(int k) const {
		if (size <= 0) return 0.f;
		int rep = (int)std::floor((float)k / (float)size);
		int within = k - rep * size;
		return intervals[within] + (float)rep * period;
	}
	// Does the index actually NAME this scale, or is it only the nearest
	// canonical neighbour? `extended` cannot answer that: it says the scale
	// came off the wire, and a plain Major relayed by Key comes off the wire
	// too. Naming that "Scala" is just as wrong as naming Bohlen-Pierce
	// "Chromatic" — which is the bug this replaces. Compare the degrees.
	bool canonical() const {
		if (dropped > 0) return false;
		if (std::fabs(period - 12.f) > 1e-3f) return false;
		if (index < 0 || index >= NUM_SCALES) return false;
		const Scale& c = SCALES[index];
		if (c.size != size) return false;
		for (int k = 0; k < size; k++)
			if (std::fabs(intervals[k] - c.intervals[k]) > 0.01f) return false;   // a cent
		return true;
	}
	bool truncated() const { return dropped > 0; }
	bool octave()    const { return std::fabs(period - 12.f) < 1e-3f; }

	const char* longName()  const { return canonical() ? SCALES[index].longName  : "Scala"; }
	const char* shortName() const { return canonical() ? SCALES[index].shortName : "Scala"; }

	// For a status cell with room for about seven characters. A scale an index
	// cannot name is described by its shape instead of misnamed: "13 deg", or
	// "13/31" when the wire could not carry all of it.
	std::string label() const {
		if (canonical()) return SCALES[index].shortName;
		if (dropped > 0) return string::f("%d/%d", size, size + dropped);
		return string::f("%d deg", size);
	}

	// One line, for a readout or a menu. Says what the scale IS, and what was
	// lost getting here.
	std::string describe() const {
		if (canonical()) return SCALES[index].longName;
		std::string s = (dropped > 0)
			? string::f("%d of %d degrees", size, size + dropped)
			: string::f("%d degrees", size);
		if (!octave()) s += string::f(", period %.2f st", period);
		if (dropped > 0) s += string::f("  (cut to the %d-degree bus limit)", BUS_MAXDEG);
		return s;
	}
};

// The canonical list, in the same shape.
inline void busScaleFromIndex(int idx, BusScale& out) {
	idx = clamp(idx, 0, NUM_SCALES - 1);
	const Scale& sc = SCALES[idx];
	out.size = std::min(sc.size, BUS_MAXDEG);
	for (int k = 0; k < out.size; k++) out.intervals[k] = sc.intervals[k];
	// Every canonical scale repeats at the octave, INCLUDING the harmonic
	// series, whose degrees simply climb past one.
	out.period = 12.f;
	out.index = idx;
	out.extended = false;
}

// Read an extended scale off a poly SCALE input.
//
// VALIDATED RATHER THAN ASSUMED. A sixteen-channel pitch cable patched here by
// accident has exactly the shape of a scale bus and would otherwise be read as
// one, producing nonsense that is very hard to trace back to a wrong cable. So
// anything that does not look like a scale is rejected, and the caller falls
// back to plain 1V-per-scale behaviour.
inline bool busScaleFromInput(Input& in, BusScale& out) {
	int ch = in.getChannels();
	if (ch < 3) return false;
	float per = in.getVoltage(1) * 12.f;
	if (per < 1.f || per > 48.f) return false;
	int n = std::min(ch - 2, BUS_MAXDEG);
	float prev = -1e9f;
	for (int k = 0; k < n; k++) {
		float d = in.getVoltage(2 + k) * 12.f;
		if (d <= prev + 1e-4f) return false;            // must ascend strictly
		if (d < -0.01f || d >= per + 0.01f) return false;
		out.intervals[k] = d;
		prev = d;
	}
	if (std::fabs(out.intervals[0]) > 0.01f) return false;     // degree 0 is the root
	out.size = n;
	out.period = per;
	// Channel 0 is "1V per scale", so its INTEGER part is the whole payload for
	// anyone reading an index -- they all round it. That leaves the fraction
	// spare, and it carries how many degrees were dropped. A count rather than
	// a flag, because "13 of 31" tells the reader something and "13, and some
	// were lost" does not. Invisible to an index-only consumer by the
	// definition of the format, and it costs no channel: there is none to
	// spare when a fourteen-degree scale already fills all sixteen.
	float v0 = in.getVoltage(0);
	float iv = std::round(v0);
	out.index = clamp((int)iv, 0, NUM_SCALES - 1);
	out.dropped = clamp((int)std::round((v0 - iv) * 128.f), 0, 127);
	out.extended = true;
	return true;
}

// The one call a consumer makes: take the full scale if one is on the wire,
// otherwise the knob plus the 1V-per-scale index, exactly as before.
inline BusScale busResolve(Input& in, int knobIndex) {
	BusScale s;
	if (in.isConnected() && busScaleFromInput(in, s)) return s;
	int idx = knobIndex;
	if (in.isConnected()) idx += (int)std::round(in.getVoltage());
	busScaleFromIndex(idx, s);
	return s;
}

// Put a scale back on the wire, so a module can pass the key along instead of
// being where it stops. A module that relays only the index is a dead end for
// every scale an index cannot name.
inline void busScaleToOutput(Output& out, const BusScale& s) {
	int n = std::min(s.size, BUS_MAXDEG);
	int dropped = clamp(s.dropped + (s.size - n), 0, 127);
	if (n <= 0) { out.setChannels(1); out.setVoltage((float)s.index); return; }
	out.setChannels(n + 2);
	out.setVoltage((float)s.index + (float)dropped / 128.f, 0);
	out.setVoltage(s.period / 12.f, 1);
	for (int k = 0; k < n; k++) out.setVoltage(s.intervals[k] / 12.f, 2 + k);
}

// Snap `semis` (relative to the root, any register) onto the scale.
inline float busSnap(float semis, const BusScale& s) {
	if (s.size <= 0 || s.period <= 0.01f) return semis;
	float rep = std::floor(semis / s.period);
	float within = semis - rep * s.period;
	float best = s.intervals[0]; float bd = 1e9f;
	for (int k = 0; k <= s.size; k++) {
		// The wrap candidate is the first degree of the NEXT period. Without
		// it, anything above the last degree snaps back down to it instead of
		// up to the root.
		float c = (k < s.size) ? s.intervals[k] : s.intervals[0] + s.period;
		float d = std::fabs(within - c);
		if (d < bd) { bd = d; best = c; }
	}
	return rep * s.period + best;
}

}  // namespace sfs
