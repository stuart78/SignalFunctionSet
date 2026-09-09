#include "plugin.hpp"
#include "scale-bus.hpp"
#include "panel-style.hpp"
#include "scales.hpp"
#include <osdialog.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// =============================================================================
// Key — a quantizer that takes its key from the patch.
//
// An ordinary quantizer is an island: you set its scale by hand and then keep
// it in step with everything else yourself. This one reads ROOT and SCALE in
// the plugin's own convention (1V/oct semitone-quantized, 1V per scale), so
// Arrange, Note, Chance, Muse, Fugue and Loom can all be moved through a key
// change from one place and this follows.
//
// THREE SUB-SCALES filter the selected scale. They are masks over the parent's
// DEGREE INDICES, never over absolute pitches — so "degrees 1, 3, 5" is a triad
// in Major and is still a triad after the scale changes to Minor or the root
// moves. That is the whole point: a sub-scale is a role within the key, not a
// set of notes. Each channel picks the full scale or one of the three.
//
// SCALES ARE NOT NECESSARILY 12-TET, NOR NECESSARILY OCTAVE-REPEATING. Three of
// the canonical scales (Harmonic series, Pelog, Slendro) carry fractional
// semitone intervals, and a loaded Scala file can have any number of degrees
// repeating at any period — Bohlen-Pierce repeats at 3/1, not 2/1. So the
// quantizer works in "semitones within one period" against real float
// intervals. The twelve-bit pitch mask that quantizers are usually built on can
// represent none of that, which is why there isn't one here.
// =============================================================================

static const int KEY_NCH     = 4;      // channels
static const int KEY_MAXPOLY = 16;
static const int KEY_NSUB    = 3;      // sub-scales
static const int KEY_MAXDEG  = 64;     // Scala scales run well past twelve
// The sub-scale rows reach as far as the SCALE does. This was 24, which was
// invisible on every canonical scale (the longest is twelve) and silently
// truncated a Scala file: a 53-degree scale quantized correctly, because the
// quantizer works off parent.iv[] up to KEY_MAXDEG, but its sub-scales could
// only see the first 24 degrees and the strip only drew those. The report that
// reached us was "Scala scales are capped at 24", which was the visible half of
// it.
static const int KEY_EDITDEG = KEY_MAXDEG;

static const char* KEY_NOTES[12] =
	{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
// What a channel's SUB setting is called. The screen has room for one glyph, and
// the sub-scale rows are numbered 1-3 in the design, so the short form matches
// the rows; the long form is what the parameter tooltip says.
static const char* KEY_SUBNAME[KEY_NSUB + 1]  = {"M", "1", "2", "3"};
static const char* KEY_SUBLONG[KEY_NSUB + 1]  = {"Full scale", "Sub-scale 1",
                                                 "Sub-scale 2", "Sub-scale 3"};

enum KeyRound { KR_NEAREST, KR_DOWN, KR_UP, KR_COUNT };

// OFFSET means scale degrees or semitones depending on a menu setting, so a
// fixed unit string in the tooltip would be wrong half the time — and a tooltip
// that reads "Channel 1 offset" with no unit at all leaves the reader guessing
// (it is exactly the thing that got asked).
struct Key;
struct KeyOffsetQuantity : ParamQuantity {
	std::string getUnit() override;
	std::string getDescription() override;
};

// A scale, reduced to what the quantizer actually needs.
struct KeyScale {
	float iv[KEY_MAXDEG] = {};
	int   n;
	float period;                        // semitones per repeat
	KeyScale() : n(0), period(12.f) {}
};

// Snap `semis` (relative to the root, any register) onto a scale.
static float keySnap(float semis, const KeyScale& s, int mode) {
	if (s.n <= 0 || s.period <= 0.01f) return semis;
	float rep = std::floor(semis / s.period);
	float within = semis - rep * s.period;
	float best = s.iv[0], bestD = 1e9f;
	// The wrap candidate is the first degree of the NEXT period — without it
	// anything above the last degree falls back to the bottom of this one.
	for (int k = 0; k <= s.n; k++) {
		float c = (k < s.n) ? s.iv[k] : s.iv[0] + s.period;
		float d = within - c;
		if (mode == KR_DOWN && d < -1e-4f) continue;
		if (mode == KR_UP   && d >  1e-4f) continue;
		float ad = std::fabs(d);
		if (ad < bestD) { bestD = ad; best = c; }
	}
	if (bestD > 1e8f)                     // directional rounding ran off the end
		best = (mode == KR_DOWN) ? s.iv[s.n - 1] - s.period : s.iv[0] + s.period;
	return rep * s.period + best;
}

// ── the scale bus ────────────────────────────────────────────────────────────
// SCALE OUT is POLYPHONIC, and channel 0 is the plain 1V-per-scale index that
// every existing module already reads: Note, Fugue, MetaFugue, Muse and Chime
// all call getVoltage(), which returns channel 0 whatever the channel count. So
// the extra channels are invisible to them and cost them nothing.
//
//   ch 0          scale index, 1V per scale — the NEAREST canonical scale when
//                 the key is a custom mask or a Scala file, because an index
//                 simply cannot say "Pelog", let alone "Pelog, 3 cents wide"
//   ch 1          period, in volts (12 semitones = 1V)
//   ch 2 .. n+1   the n degrees as 1V/oct offsets from the root; ch 2 is 0
//
// That is the answer to microtonal and custom scales travelling between
// modules: the index stays as a lossy summary for whoever only wants a summary,
// and the real scale rides right behind it for whoever can use it. Patch this
// into another Key's SCALE input and the whole key crosses intact — Scala,
// non-octave period and all.
// The wire format lives in scale-bus.hpp now, so Key and every consumer read
// and write ONE definition of it. Key keeps its own richer KeyScale internally
// (Scala files run well past fourteen degrees, where the bus stops at
// sfs::BUS_MAXDEG), and the bus is the lossy but shared window onto it.

static bool keySameScale(const KeyScale& a, const KeyScale& b) {
	if (a.n != b.n || std::fabs(a.period - b.period) > 1e-4f) return false;
	for (int k = 0; k < a.n; k++)
		if (std::fabs(a.iv[k] - b.iv[k]) > 1e-4f) return false;
	return true;
}

// Move by n scale degrees, staying on the scale.
static float keyShiftDegrees(float semis, const KeyScale& s, int n) {
	if (n == 0 || s.n <= 0) return semis;
	float rep = std::floor(semis / s.period);
	float within = semis - rep * s.period;
	int idx = 0; float bd = 1e9f;
	for (int k = 0; k < s.n; k++) {
		float d = std::fabs(within - s.iv[k]);
		if (d < bd) { bd = d; idx = k; }
	}
	int t = idx + n;
	int repShift = (int)std::floor((float)t / (float)s.n);
	int wrapped = t - repShift * s.n;
	return (rep + (float)repShift) * s.period + s.iv[wrapped];
}

// ── Scala (.scl) ─────────────────────────────────────────────────────────────
// The format: `!` starts a comment, the first data line is a description, the
// second is the degree count, then that many pitches — either cents (any line
// containing a '.') or a ratio (`3/2`, or a bare integer). 1/1 is implicit and
// is NOT listed, and the LAST entry is the period, which is why it is pulled off
// the end rather than kept as a degree.
static bool keyLoadScala(const std::string& path, KeyScale& out, std::string& name) {
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f) return false;
	char line[1024];
	int stage = 0;                       // 0 = want description, 1 = want count, 2 = pitches
	int want = 0;
	std::vector<float> semis;
	semis.push_back(0.f);                // the implicit 1/1
	name.clear();

	while (std::fgets(line, sizeof(line), f)) {
		std::string t(line);
		size_t a = t.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) continue;
		t = t.substr(a);
		size_t b = t.find_last_not_of(" \t\r\n");
		t.resize(b + 1);
		if (t.empty() || t[0] == '!') continue;

		if (stage == 0) { name = t; stage = 1; continue; }
		if (stage == 1) { want = std::atoi(t.c_str()); stage = 2; continue; }

		float v;
		if (t.find('.') != std::string::npos) {
			v = (float)std::atof(t.c_str()) / 100.f;             // cents → semitones
		}
		else {
			size_t sl = t.find('/');
			double num = (sl == std::string::npos) ? std::atof(t.c_str())
			                                       : std::atof(t.substr(0, sl).c_str());
			double den = (sl == std::string::npos) ? 1.0 : std::atof(t.substr(sl + 1).c_str());
			if (den == 0.0 || num <= 0.0) continue;
			v = (float)(12.0 * std::log2(num / den));
		}
		semis.push_back(v);
		if ((int)semis.size() > want) break;                     // count reached
	}
	std::fclose(f);

	if (want <= 0 || (int)semis.size() < 2) return false;
	out.period = semis.back();                                   // the last entry IS the period
	semis.pop_back();
	if (out.period <= 0.01f) return false;
	std::sort(semis.begin(), semis.end());
	out.n = 0;
	for (size_t i = 0; i < semis.size() && out.n < KEY_MAXDEG; i++) {
		if (out.n > 0 && std::fabs(semis[i] - out.iv[out.n - 1]) < 0.001f) continue;
		out.iv[out.n++] = semis[i];
	}
	return out.n > 0;
}


struct Key : Module {
	enum ParamId {
		ROOT_PARAM, SCALE_PARAM,
		ENUMS(OFFSET_PARAM, KEY_NCH),
		ENUMS(SUB_PARAM, KEY_NCH),
		PARAMS_LEN
	};
	enum InputId {
		ROOT_INPUT, SCALE_INPUT, TRIG_INPUT,
		ENUMS(IN_INPUT, KEY_NCH),
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(OUT_OUTPUT, KEY_NCH),
		ENUMS(CHG_OUTPUT, KEY_NCH),
		ROOT_OUTPUT, SCALE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	// Index NUM_SCALES selects a loaded Scala file; 0..NUM_SCALES-1 stay exactly
	// the canonical list, so SCALE CV is still interchangeable across the plugin.
	static const int SCALA_INDEX = sfs::NUM_SCALES;

	// ── the key in force ──────────────────────────────────────────────────────
	int   rootNote = 0;
	int   scaleIndex = 1;
	bool  customMask = false;            // the keyboard has been edited by hand
	uint16_t mask = 0;

	KeyScale parent;                     // the selected scale
	KeyScale sub[KEY_NSUB];              // parent filtered by each sub-scale mask
	// 64 bits, because KEY_MAXDEG is 64. As a uint32_t it could not represent a
	// degree past 31 whatever KEY_EDITDEG said, so raising the cap alone would
	// have moved the truncation rather than removed it.
	uint64_t subMask[KEY_NSUB] = {0, 0, 0};    // over parent DEGREE indices
	uint16_t subChrom[KEY_NSUB] = {0, 0, 0};   // free mode: SEMITONES from the root
	int   keyGen = 0;                    // bumped whenever anything about the key moves

	// ── Scala ─────────────────────────────────────────────────────────────────
	KeyScale scala;
	std::string scalaName, scalaPath;
	bool  scalaLoaded = false;

	// ── an incoming scale bus, when one is patched ────────────────────────────
	KeyScale busScale;
	bool  busActive = false;

	// ── options ───────────────────────────────────────────────────────────────
	bool  offsetInDegrees = true;
	int   roundMode = KR_NEAREST;
	float hysteresisCents = 12.f;
	bool  freeSub = false;               // sub-scales may reach outside the key

	// Whether the key can be drawn, and edited, on twelve keys: the period is an
	// octave and every degree lands on a semitone. Pelog, Slendro, the harmonic
	// series and most Scala files fail this, and for them a chromatic pick has no
	// meaning -- there is no chromatic to pick from.
	bool chromaticKey() const {
		if (std::fabs(parent.period - 12.f) > 0.02f) return false;
		for (int k = 0; k < parent.n; k++)
			if (std::fabs(parent.iv[k] - std::round(parent.iv[k])) > 0.02f) return false;
		return true;
	}

	// ── per-channel state ─────────────────────────────────────────────────────
	float held[KEY_NCH][KEY_MAXPOLY] = {};
	bool  hasHeld[KEY_NCH][KEY_MAXPOLY] = {};
	int   lastGen[KEY_NCH][KEY_MAXPOLY] = {};
	dsp::PulseGenerator chgPulse[KEY_NCH][KEY_MAXPOLY];
	// ONE TRIGGER PER CHANNEL. A mono cable still drives all four, because
	// getPolyVoltage() hands channel 0 to every reader, so every existing patch
	// behaves exactly as it did. A polyphonic cable addresses them separately.
	dsp::SchmittTrigger trigIn[KEY_NCH];
	float shownVolts[KEY_NCH] = {};
	bool  shownActive[KEY_NCH] = {};

	Key() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		std::vector<std::string> noteNames;
		for (int i = 0; i < 12; i++) noteNames.push_back(KEY_NOTES[i]);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", noteNames);

		std::vector<std::string> scaleNames;
		for (int i = 0; i < sfs::NUM_SCALES; i++)
			scaleNames.push_back(sfs::SCALES[i].longName);
		scaleNames.push_back("Scala file");
		configSwitch(SCALE_PARAM, 0.f, (float)SCALA_INDEX, 1.f, "Scale", scaleNames);

		for (int c = 0; c < KEY_NCH; c++) {
			configParam<KeyOffsetQuantity>(OFFSET_PARAM + c, -12.f, 12.f, 0.f,
			            string::f("Channel %d offset", c + 1));
			getParamQuantity(OFFSET_PARAM + c)->snapEnabled = true;
			configSwitch(SUB_PARAM + c, 0.f, (float)KEY_NSUB, 0.f,
			             string::f("Channel %d scale", c + 1),
			             {KEY_SUBLONG[0], KEY_SUBLONG[1], KEY_SUBLONG[2], KEY_SUBLONG[3]});
			configInput(IN_INPUT + c, string::f(
				"Channel %d pitch (1V/oct, poly; normals from the channel to its left)", c + 1));
			configOutput(OUT_OUTPUT + c, string::f("Channel %d quantized pitch", c + 1));
			// RETIRED: the note-change triggers lost their jacks in the panel
			// redesign. The enum slots stay, because outputs serialise by index and
			// deleting these would re-point every cable patched to ROOT/SCALE OUT.
			configOutput(CHG_OUTPUT + c, "(retired)");
			configBypass(IN_INPUT + c, OUT_OUTPUT + c);
		}

		configInput(ROOT_INPUT,  "Root CV (1V/oct, semitone-quantized)");
		configInput(SCALE_INPUT, "Scale CV (1V per scale)");
		configInput(TRIG_INPUT,  "Sample & hold trigger (poly: channel N triggers Key channel N, last channel repeated) — when patched, notes update only on a trigger");
		configOutput(ROOT_OUTPUT,  "Root CV (1V/oct) — drives any module's ROOT input");
		configOutput(SCALE_OUTPUT, "Scale CV (1V per scale on channel 0; the full scale, "
		                           "including microtonal and Scala, on the further channels)");

		defaultSubs();
		rebuild();
	}

	// Seeded with three roles you actually reach for, expressed as degrees so
	// they stay themselves through any change of scale.
	void defaultSubs() {
		subMask[0] = (1ull << 0) | (1ull << 2) | (1ull << 4);              // triad
		subMask[1] = (1ull << 0) | (1ull << 3) | (1ull << 4);              // root, 4th, 5th
		subMask[2] = (1ull << 0) | (1ull << 2) | (1ull << 4) | (1ull << 6);  // seventh chord
	}

	void onReset() override {
		customMask = false;
		offsetInDegrees = true;
		roundMode = KR_NEAREST;
		hysteresisCents = 12.f;
		defaultSubs();
		for (int c = 0; c < KEY_NCH; c++)
			for (int p = 0; p < KEY_MAXPOLY; p++) hasHeld[c][p] = false;
		rebuild();
	}

	bool scaleIsScala() const { return scaleIndex >= SCALA_INDEX; }

	// Read an extended scale off SCALE IN. Validated rather than assumed: a
	// sixteen-channel pitch cable patched here by accident would otherwise be
	// read as a scale and produce nonsense, so anything that does not look like
	// a scale falls back to plain 1V-per-scale index behaviour.
	bool readScaleBus(KeyScale& out) {
		sfs::BusScale b;
		if (!sfs::busScaleFromInput(inputs[SCALE_INPUT], b)) return false;
		out.n = std::min(b.size, KEY_MAXDEG);
		for (int k = 0; k < out.n; k++) out.iv[k] = b.intervals[k];
		out.period = b.period;
		return true;
	}

	// An index cannot express a custom mask or a Scala scale, so channel 0 of
	// the bus carries whichever canonical scale shares the most pitch classes
	// with it. Downstream modules stay musically close instead of jumping to
	// something unrelated.
	int nearestCanonicalIndex() const {
		uint16_t m = keyboardMask();
		int best = 0, bestScore = -1000;
		for (int i = 0; i < sfs::NUM_SCALES; i++) {
			const sfs::Scale& sc = sfs::SCALES[i];
			uint16_t c = 0;
			for (int d = 0; d < sc.size; d++)
				c |= (uint16_t)(1u << ((((int)std::lround(sc.intervals[d])) % 12 + 12) % 12));
			int score = 2 * __builtin_popcount((unsigned)(m & c))
			              - __builtin_popcount((unsigned)(m ^ c));
			if (score > bestScore) { bestScore = score; best = i; }
		}
		return best;
	}

	// The twelve-key mask the on-screen keyboard shows. Microtonal degrees are
	// rounded HERE and nowhere else — see the note at the top of the file.
	uint16_t keyboardMask() const {
		if (customMask) return mask;
		uint16_t m = 0;
		for (int k = 0; k < parent.n; k++) {
			int s = ((int)std::lround(parent.iv[k]) % 12 + 12) % 12;
			m |= (uint16_t)(1u << s);
		}
		return m;
	}

	void rebuild() {
		parent = KeyScale();
		if (busActive) {
			// A patched bus is the authority: it carries a real scale, which the
			// knob's index cannot.
			parent = busScale;
		}
		else if (customMask) {
			for (int s = 0; s < 12 && parent.n < KEY_MAXDEG; s++)
				if (mask & (1u << s)) parent.iv[parent.n++] = (float)s;
			parent.period = 12.f;
		}
		else if (scaleIsScala() && scalaLoaded) {
			parent = scala;
		}
		else {
			int si = clamp(scaleIndex, 0, sfs::NUM_SCALES - 1);
			const sfs::Scale& sc = sfs::SCALES[si];
			parent.period = 12.f;
			for (int d = 0; d < sc.size && parent.n < KEY_MAXDEG; d++) {
				float v = std::fmod(sc.intervals[d], 12.f);
				if (v < 0.f) v += 12.f;
				parent.iv[parent.n++] = v;
			}
			// The harmonic series spans octaves, so folding it leaves duplicates.
			std::sort(parent.iv, parent.iv + parent.n);
			int w = 0;
			for (int a = 0; a < parent.n; a++)
				if (a == 0 || std::fabs(parent.iv[a] - parent.iv[w - 1]) > 0.01f)
					parent.iv[w++] = parent.iv[a];
			parent.n = w;
		}
		if (parent.n == 0) { parent.iv[0] = 0.f; parent.n = 1; }   // never mute

		// A sub-scale is built from TWO stores, and they are different kinds of
		// thing on purpose.
		//
		// subMask holds DEGREE INDICES, which is what makes a sub-scale a role in
		// the key rather than a set of notes: {0,2,4} is a triad in Major (C E G),
		// still a triad in Minor (C D# G), and D# G A# in E flat.
		//
		// subChrom holds SEMITONE OFFSETS from the root, and exists only for the
		// notes free mode can reach -- the ones outside the parent scale, which
		// have no degree index to be stored as. They follow the ROOT, because a
		// flat 5th should still be a flat 5th when you transpose, but they do not
		// follow a SCALE change, because there is nothing for them to follow: the
		// note was chosen for being outside the scale in the first place.
		for (int k = 0; k < KEY_NSUB; k++) {
			sub[k] = KeyScale();
			sub[k].period = parent.period;
			for (int d = 0; d < parent.n && d < KEY_EDITDEG; d++)
				if (subMask[k] & (1ull << d)) sub[k].iv[sub[k].n++] = parent.iv[d];
			if (freeSub && chromaticKey()) {
				for (int s = 0; s < 12 && sub[k].n < KEY_MAXDEG; s++) {
					if (!((subChrom[k] >> s) & 1)) continue;
					float v = (float)s;
					bool dup = false;                      // it may also be a degree
					for (int a = 0; a < sub[k].n; a++)
						if (std::fabs(sub[k].iv[a] - v) < 0.01f) dup = true;
					if (!dup) sub[k].iv[sub[k].n++] = v;
				}
				std::sort(sub[k].iv, sub[k].iv + sub[k].n);
			}
			// An empty sub-scale falls back to the parent rather than going silent.
			if (sub[k].n == 0) sub[k] = parent;
		}
		keyGen++;
	}

	const KeyScale& scaleFor(int c) {
		int s = (int)std::round(params[SUB_PARAM + c].getValue());
		return (s >= 1 && s <= KEY_NSUB) ? sub[s - 1] : parent;
	}

	void process(const ProcessArgs& args) override {
		int rootK = (int)std::round(params[ROOT_PARAM].getValue());
		int rootCV = inputs[ROOT_INPUT].isConnected()
			? (int)std::round(inputs[ROOT_INPUT].getVoltage() * 12.f) : 0;
		int newRoot = (((rootK + rootCV) % 12) + 12) % 12;

		int scaleK = (int)std::round(params[SCALE_PARAM].getValue());
		int scaleCV = inputs[SCALE_INPUT].isConnected()
			? (int)std::round(inputs[SCALE_INPUT].getVoltage()) : 0;
		int newScale = clamp(scaleK + scaleCV, 0, SCALA_INDEX);

		// An incoming bus overrides the index entirely — it says more than an
		// index can, so letting the knob fight it would only be confusing.
		KeyScale bs;
		bool bus = readScaleBus(bs);
		if (bus != busActive || (bus && !keySameScale(bs, busScale))) {
			busActive = bus;
			busScale = bs;
			if (bus) customMask = false;
			rebuild();
		}

		if (newRoot != rootNote) { rootNote = newRoot; keyGen++; }
		if (newScale != scaleIndex) {
			scaleIndex = newScale;
			// Selecting a scale is an explicit instruction, so it wins over a
			// hand-edited keyboard rather than being silently ignored.
			customMask = false;
			rebuild();
		}

		bool trigPatched = inputs[TRIG_INPUT].isConnected();
		float hystSemis = hysteresisCents / 100.f;

		// ── the ins normal left to right ───────────────────────────────────────
		// One cable in IN 1 feeds all four channels; a cable in IN 3 BREAKS the
		// chain there, so 1-2 follow the first and 3-4 follow the second. That is
		// the ordinary mult-in-the-panel behaviour, and it is what makes the poly
		// TRIG worth having: one pitch source, four channels sampling it at four
		// different moments, four sub-scales and four offsets off the same line.
		// Without it, three of the four channels had no signal to sample and the
		// per-channel trigger looked dead when it was working perfectly.
		int src[KEY_NCH];
		{
			int last = -1;
			for (int c = 0; c < KEY_NCH; c++) {
				if (inputs[IN_INPUT + c].isConnected()) last = c;
				src[c] = last;                      // -1 until the first cable
			}
		}
		// A trigger cable narrower than four channels REPEATS ITS LAST CHANNEL
		// rather than reading zeros off the end. getPolyVoltage only does that
		// for a mono cable; on a 2-channel cable it reads voltages[2] and [3],
		// which the engine holds at 0 V, so channels 3 and 4 would never see an
		// edge again and would sit frozen on whatever they sampled first --
		// silently, and for ever.
		int trigCh = std::max(1, inputs[TRIG_INPUT].getChannels());

		for (int c = 0; c < KEY_NCH; c++) {
			// Each channel samples on its own trigger. The Schmitt has to be
			// per channel as well as the voltage: one shared trigger would be
			// consumed by whichever channel looked at it first, and the other
			// three would never see the edge.
			bool sampleNow = !trigPatched
				|| trigIn[c].process(inputs[TRIG_INPUT].getVoltage(std::min(c, trigCh - 1)),
				                     0.1f, 1.f);
			const KeyScale& sc = scaleFor(c);
			// Polyphony comes from whichever cable is feeding this channel, so a
			// normalled channel is as polyphonic as the source it is reading.
			Input& in4 = inputs[IN_INPUT + (src[c] < 0 ? c : src[c])];
			int nch = std::max(1, in4.getChannels());
			nch = std::min(nch, KEY_MAXPOLY);
			outputs[OUT_OUTPUT + c].setChannels(nch);
			outputs[CHG_OUTPUT + c].setChannels(nch);
			shownActive[c] = (src[c] >= 0);

			int off = (int)std::round(params[OFFSET_PARAM + c].getValue());
			// A sub-scale change must invalidate held notes exactly as a key
			// change does, or a channel keeps a note its new scale does not have.
			int gen = keyGen * 8 + (int)std::round(params[SUB_PARAM + c].getValue());

			for (int p = 0; p < nch; p++) {
				float in = (src[c] >= 0) ? in4.getVoltage(p) : 0.f;

				if (sampleNow || !hasHeld[c][p] || lastGen[c][p] != gen) {
					float semis = in * 12.f - (float)rootNote;
					if (!offsetInDegrees) semis += (float)off;
					float q = keySnap(semis, sc, roundMode);
					if (offsetInDegrees) q = keyShiftDegrees(q, sc, off);

					// Hysteresis stops a pitch sitting exactly on a boundary from
					// chattering between two notes. It must be abandoned when the
					// key changes, or the old note would hold on even though it is
					// no longer in the scale.
					if (hasHeld[c][p] && lastGen[c][p] == gen && hystSemis > 0.f) {
						float prev = held[c][p] * 12.f - (float)rootNote;
						float inS = in * 12.f - (float)rootNote;
						if (!offsetInDegrees) inS += (float)off;
						if (std::fabs(inS - prev) < std::fabs(inS - q) + hystSemis)
							q = prev;
					}
					float outV = ((float)rootNote + q) / 12.f;

					if (!hasHeld[c][p] || std::fabs(outV - held[c][p]) > 1e-6f) {
						if (hasHeld[c][p]) chgPulse[c][p].trigger(1e-3f);
						held[c][p] = outV;
					}
					hasHeld[c][p] = true;
					lastGen[c][p] = gen;
				}

				outputs[OUT_OUTPUT + c].setVoltage(held[c][p], p);
				outputs[CHG_OUTPUT + c].setVoltage(
					chgPulse[c][p].process(args.sampleTime) ? 10.f : 0.f, p);
				if (p == 0) shownVolts[c] = held[c][p];
			}
		}

		// ── the key, on its way out ────────────────────────────────────────────
		outputs[ROOT_OUTPUT].setChannels(1);
		outputs[ROOT_OUTPUT].setVoltage((float)rootNote / 12.f);

		// Channel 0 is exactly what a 1V-per-scale consumer expects; a canonical
		// scale sends its own index rather than a nearest-match, so the round
		// trip through another module is lossless where it can be.
		int idx = (busActive || customMask || scaleIsScala())
		        ? nearestCanonicalIndex() : clamp(scaleIndex, 0, sfs::NUM_SCALES - 1);
		sfs::BusScale b;
		b.size = std::min(parent.n, sfs::BUS_MAXDEG);
		for (int k = 0; k < b.size; k++) b.intervals[k] = parent.iv[k];
		b.period = parent.period;
		b.index = idx;
		// Key holds up to KEY_MAXDEG degrees and the bus holds fourteen, so a
		// long Scala scale leaves some behind. Say how many, or every module
		// downstream shows a fourteen-degree scale as if that were the whole
		// of it.
		b.dropped = std::max(0, parent.n - b.size);
		sfs::busScaleToOutput(outputs[SCALE_OUTPUT], b);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "customMask", json_boolean(customMask));
		json_object_set_new(root, "mask", json_integer(mask));
		json_object_set_new(root, "offsetInDegrees", json_boolean(offsetInDegrees));
		json_object_set_new(root, "roundMode", json_integer(roundMode));
		json_object_set_new(root, "hysteresisCents", json_real(hysteresisCents));
		json_t* sm = json_array();
		// json_integer is int64, so a 64-bit mask round-trips bit-exactly even
		// when the top bit is set and it reads back as negative.
		for (int k = 0; k < KEY_NSUB; k++) json_array_append_new(sm, json_integer((json_int_t)subMask[k]));
		json_object_set_new(root, "subMask", sm);
		json_t* sc = json_array();
		for (int k = 0; k < KEY_NSUB; k++) json_array_append_new(sc, json_integer(subChrom[k]));
		json_object_set_new(root, "subChrom", sc);
		json_object_set_new(root, "freeSub", json_boolean(freeSub));

		// The PARSED scale is saved as well as the path: a patch opened on another
		// machine, or after the .scl has moved, still sounds right.
		json_object_set_new(root, "scalaLoaded", json_boolean(scalaLoaded));
		json_object_set_new(root, "scalaPath", json_string(scalaPath.c_str()));
		json_object_set_new(root, "scalaName", json_string(scalaName.c_str()));
		json_object_set_new(root, "scalaPeriod", json_real(scala.period));
		json_t* iv = json_array();
		for (int k = 0; k < scala.n; k++) json_array_append_new(iv, json_real(scala.iv[k]));
		json_object_set_new(root, "scalaIvals", iv);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "customMask")) customMask = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "mask")) mask = (uint16_t)json_integer_value(j);
		if (json_t* j = json_object_get(root, "offsetInDegrees"))
			offsetInDegrees = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "roundMode"))
			roundMode = clamp((int)json_integer_value(j), 0, KR_COUNT - 1);
		if (json_t* j = json_object_get(root, "hysteresisCents"))
			hysteresisCents = clamp((float)json_number_value(j), 0.f, 50.f);
		if (json_t* fs = json_object_get(root, "freeSub")) freeSub = json_boolean_value(fs);
		if (json_t* sc = json_object_get(root, "subChrom"))
			for (int k = 0; k < KEY_NSUB && k < (int)json_array_size(sc); k++)
				subChrom[k] = (uint16_t)json_integer_value(json_array_get(sc, k));
		if (json_t* sm = json_object_get(root, "subMask"))
			for (int k = 0; k < KEY_NSUB && k < (int)json_array_size(sm); k++)
				subMask[k] = (uint64_t)json_integer_value(json_array_get(sm, k));

		if (json_t* j = json_object_get(root, "scalaLoaded")) scalaLoaded = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "scalaPath")) scalaPath = json_string_value(j);
		if (json_t* j = json_object_get(root, "scalaName")) scalaName = json_string_value(j);
		if (json_t* j = json_object_get(root, "scalaPeriod")) scala.period = json_number_value(j);
		if (json_t* iv = json_object_get(root, "scalaIvals")) {
			scala.n = 0;
			for (int k = 0; k < (int)json_array_size(iv) && scala.n < KEY_MAXDEG; k++)
				scala.iv[scala.n++] = (float)json_number_value(json_array_get(iv, k));
		}
		rebuild();
	}
};


// =============================================================================
// Display — the key as a keyboard, the scale as regions, the sub-scales as rows.
// =============================================================================

struct KeyDisplay : OpaqueWidget {
	Key* module = nullptr;
	std::shared_ptr<Font> font;

	// The screen is drawn in the DESIGN's own coordinates and scaled once, rather
	// than in fractions of the widget. design/Key/*.svg are 760 x 480 and the
	// panel's screen is 64.35 x 40.64mm, the same ratio to three places, so every
	// number below is the number in the design file and can be checked against it.
	static constexpr float DW = 760.f, DH = 480.f;
	static constexpr float MARGIN = 36.f, RIGHT = 724.f;
	static constexpr float RULE_HEAD = 37.5f, RULE_SUB = 224.5f, RULE_FOOT = 424.5f;
	static constexpr float KEY_X0 = 71.f, KEY_DX = 51.5f, KEY_R = 35.f;
	static constexpr float ROW_BLACK = 84.5f, ROW_WHITE = 174.5f;
	static constexpr float STRIP_Y = 56.f, STRIP_H = 140.f;
	static constexpr float SUB_Y0 = 259.f, SUB_DY = 61.75f, SUB_R = 21.f;
	static constexpr float SUB_LABEL_X = 22.f;
	static constexpr float HEAD_Y = 18.f, FOOT_Y = 450.f;

	float sc() const { return box.size.x / DW; }
	float X(float u) const { return u * sc(); }

	// Twelve pitch classes on THIRTEEN slots. The gap is the black key that is
	// not there between E and F, and keeping it is what makes the row read as a
	// keyboard rather than as twelve anonymous dots.
	static int slotOf(int pc) { return pc + (pc >= 5 ? 1 : 0); }
	static float keyU(int pc) { return KEY_X0 + KEY_DX * (float)slotOf(pc); }
	static bool isBlack(int pc) {
		return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
	}

	// Sub-row cell centres. On a chromatic key they sit under their own note on
	// the keyboard; otherwise the scale has no chromatic to align to, so the row
	// is just its own degrees, spread across the same span.
	float subCellU(int i, int n, bool chromatic) const {
		if (chromatic) return keyU(i);
		if (n <= 1) return (MARGIN + RIGHT) * 0.5f;
		float span = RIGHT - MARGIN - 2.f * SUB_R;
		return MARGIN + SUB_R + span * (float)i / (float)(n - 1);
	}
	float subCellR(int n, bool chromatic) const {
		if (chromatic || n <= 12) return SUB_R;
		float pitch = (RIGHT - MARGIN) / (float)n;          // keep them from touching
		return std::min(SUB_R, pitch * 0.42f);
	}

	// How many cells a sub row shows, and whether they are chromatic.
	int subCellCount(bool chromatic) const {
		if (chromatic) return 12;
		return module ? std::min(module->parent.n, KEY_EDITDEG) : 7;
	}

	// ── hit testing ──────────────────────────────────────────────────────────
	int keyHit(Vec p) const {
		for (int pc = 0; pc < 12; pc++) {
			float cx = X(keyU(pc)), cy = X(isBlack(pc) ? ROW_BLACK : ROW_WHITE);
			float dx = p.x - cx, dy = p.y - cy, r = X(KEY_R);
			if (dx * dx + dy * dy <= r * r) return pc;
		}
		return -1;
	}

	void subHit(Vec p, bool chromatic, int& row, int& cell) const {
		row = cell = -1;
		int n = subCellCount(chromatic);
		float r = X(subCellR(n, chromatic));
		for (int k = 0; k < KEY_NSUB; k++) {
			float cy = X(SUB_Y0 + SUB_DY * (float)k);
			if (std::fabs(p.y - cy) > r * 1.15f) continue;
			for (int i = 0; i < n; i++) {
				float cx = X(subCellU(i, n, chromatic));
				if (std::fabs(p.x - cx) <= r * 1.15f) { row = k; cell = i; return; }
			}
			return;
		}
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) {
			OpaqueWidget::onButton(e);
			return;
		}
		bool chromatic = module->chromaticKey();

		int row, cell;
		subHit(e.pos, chromatic, row, cell);
		if (row >= 0) {
			e.consume(this);
			if (!chromatic) {                        // cell IS the degree index
				module->subMask[row] ^= (1ull << cell);
				module->rebuild();
				return;
			}
			// A chromatic cell is a pitch class. If the parent scale has a degree
			// there it toggles that DEGREE, which is what keeps a sub-scale a role
			// in the key. If it does not, the cell is outside the key and only free
			// mode may have it, stored as a semitone offset from the root.
			int sfr = ((cell - module->rootNote) % 12 + 12) % 12;
			int deg = -1;
			for (int d = 0; d < module->parent.n && d < KEY_EDITDEG; d++)
				if (std::fabs(module->parent.iv[d] - (float)sfr) < 0.02f) deg = d;
			if (deg >= 0) module->subMask[row] ^= (1ull << deg);
			else if (module->freeSub) module->subChrom[row] ^= (uint16_t)(1u << sfr);
			else return;                              // out of key, and not allowed
			module->rebuild();
			return;
		}

		if (!chromatic) return;                       // no keyboard to click
		int s = keyHit(e.pos);
		if (s < 0) return;                            // a miss falls through
		e.consume(this);
		if (!module->customMask) {                    // the first edit forks the preset
			module->mask = module->keyboardMask();
			module->customMask = true;
		}
		module->mask ^= (uint16_t)(1u << s);
		module->rebuild();
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		if (!font || font->handle < 0) return;
		nvgSave(args.vg);
		nvgScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		if (!module) drawPreview(args);
		else         drawLive(args);
		nvgRestore(args.vg);
	}

	// Every string on this screen goes through here, at the plugin's one screen
	// size — see panel-style.hpp. Sizes proportional to the display's own height
	// are what made this module's text a different size from Note's.
	void text(const DrawArgs& args, float x, float y, NVGcolor col,
	          const std::string& t, float mm = sfs::TYPE_SCREEN,
	          int align = NVG_ALIGN_CENTER) {
		sfs::screenFont(args.vg, font, mm);
		nvgTextAlign(args.vg, align | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, col);
		nvgText(args.vg, x, y, t.c_str(), NULL);
	}

	void rule(const DrawArgs& args, float u) {
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, X(MARGIN), X(u));
		nvgLineTo(args.vg, X(RIGHT), X(u));
		nvgStrokeColor(args.vg, sfs::SCREEN_LINE);
		nvgStrokeWidth(args.vg, std::max(X(2.f), 1.f));
		nvgStroke(args.vg);
	}

	void dot(const DrawArgs& args, float ux, float uy, float ur, NVGcolor c) {
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, X(ux), X(uy), X(ur));
		nvgFillColor(args.vg, c);
		nvgFill(args.vg);
	}

	void header(const DrawArgs& args, const std::string& rootName,
	            const std::string& scaleName) {
		sfs::screenFont(args.vg, font, sfs::TYPE_SCREEN);
		float w = nvgTextBounds(args.vg, 0, 0, rootName.c_str(), NULL, NULL);
		text(args, X(MARGIN), X(HEAD_Y), sfs::SCREEN_TEXT, rootName,
		     sfs::TYPE_SCREEN, NVG_ALIGN_LEFT);
		text(args, X(MARGIN) + w + X(14.f), X(HEAD_Y), sfs::SCREEN_HOT, scaleName,
		     sfs::TYPE_SCREEN, NVG_ALIGN_LEFT);
		rule(args, RULE_HEAD);
	}

	// ── the western state: a keyboard ────────────────────────────────────────
	// Drawn from C whatever the root is, because a keyboard that starts somewhere
	// else is not a keyboard any more — you read it by its shape. The root is
	// marked where it falls instead.
	void drawKeyboard(const DrawArgs& args, uint16_t mask, int root, const bool* lit) {
		for (int pc = 0; pc < 12; pc++) {
			bool in = (mask >> pc) & 1;
			NVGcolor c = lit[pc] ? sfs::SCREEN_HOT
			           : in      ? sfs::SCREEN_BLUE : sfs::SCREEN_DEEP;
			dot(args, keyU(pc), isBlack(pc) ? ROW_BLACK : ROW_WHITE, KEY_R, c);
			if (pc == root)
				dot(args, keyU(pc), isBlack(pc) ? ROW_BLACK : ROW_WHITE,
				    KEY_R * 0.29f, sfs::SCREEN_TEXT);
		}
	}

	// ── the arbitrary state: the scale as it really is ───────────────────────
	// x is LINEAR in pitch across one period, so equal pitch spans are equal
	// widths and the gaps between lines are the snap regions. This is the only
	// honest picture of Pelog's 1.20-semitone second degree, or of a Scala file
	// that repeats at 19.02 semitones instead of 12.
	void drawStrip(const DrawArgs& args, const KeyScale& s,
	               const float* sounding, int nSounding) {
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		nvgRect(vg, X(MARGIN), X(STRIP_Y), X(RIGHT - MARGIN), X(STRIP_H));
		nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20));
		nvgFill(vg);

		float per = std::max(s.period, 0.01f);
		float span = RIGHT - MARGIN;
		// n degrees plus the closing line at the period, which is the first degree
		// of the next repeat and is what shows the last region's width.
		for (int k = 0; k <= s.n; k++) {
			float v = (k < s.n) ? s.iv[k] : per;
			float u = MARGIN + span * v / per;
			nvgBeginPath(vg);
			nvgMoveTo(vg, X(u), X(STRIP_Y));
			nvgLineTo(vg, X(u), X(STRIP_Y + STRIP_H));
			nvgStrokeColor(vg, sfs::SCREEN_BLUE);
			nvgStrokeWidth(vg, std::max(X(3.f), 1.f));
			nvgStroke(vg);
		}
		for (int i = 0; i < nSounding; i++) {
			float within = sounding[i] - per * std::floor(sounding[i] / per);
			dot(args, MARGIN + span * within / per, STRIP_Y + STRIP_H * 0.5f,
			    12.f, sfs::SCREEN_TEXT);
		}
	}

	// ── the sub-scale rows ───────────────────────────────────────────────────
	// `inParent` / `on` / `lit` are per cell. `outside` marks a cell that is ON
	// but is not in the parent scale, i.e. a free-mode pick: it gets a ring, so
	// an accidental is visible as an accidental rather than looking like a
	// degree.
	void drawSubRows(const DrawArgs& args, int n, bool chromatic, const bool* used,
	                 const bool* on, const bool* inParent, const bool* lit,
	                 const bool* outside) {
		float r = subCellR(n, chromatic);
		for (int k = 0; k < KEY_NSUB; k++) {
			float cy = SUB_Y0 + SUB_DY * (float)k;
			text(args, X(SUB_LABEL_X), X(cy),
			     used[k] ? sfs::SCREEN_TEXT : sfs::SCREEN_PMID, std::to_string(k + 1));
			for (int i = 0; i < n; i++) {
				int f = k * KEY_EDITDEG + i;
				NVGcolor c = lit[f]      ? sfs::SCREEN_HOT
				           : on[f]       ? (used[k] ? sfs::SCREEN_BLUE : sfs::SCREEN_DEEP)
				           : inParent[f] ? sfs::SCREEN_PURP
				                         : nvgRGB(0x23, 0x23, 0x3C);
				dot(args, subCellU(i, n, chromatic), cy, r, c);
				if (outside[f]) {
					nvgBeginPath(args.vg);
					nvgCircle(args.vg, X(subCellU(i, n, chromatic)), X(cy), X(r) - 1.f);
					nvgStrokeColor(args.vg, sfs::SCREEN_TEXT);
					nvgStrokeWidth(args.vg, std::max(X(2.5f), 1.f));
					nvgStroke(args.vg);
				}
			}
		}
	}

	// Which degree of a scale a sounding note is. The output is always snapped
	// to a degree, so the tolerance only has to survive float error.
	static int degreeIndexOf(const KeyScale& sc, float semisFromRoot) {
		if (sc.n <= 0 || sc.period <= 0.01f) return -1;
		float within = semisFromRoot - sc.period * std::floor(semisFromRoot / sc.period);
		int best = -1; float bd = 1e9f;
		for (int k = 0; k < sc.n; k++) {
			float d = std::fabs(within - sc.iv[k]);
			if (d < bd) { bd = d; best = k; }
		}
		return (bd < 0.05f) ? best : -1;
	}

	void footer(const DrawArgs& args, const char* const* subLabel,
	            const std::string* note, const bool* active) {
		rule(args, RULE_FOOT);
		float cw = (RIGHT - MARGIN) / (float)KEY_NCH;
		for (int c = 0; c < KEY_NCH; c++) {
			float cx = MARGIN + cw * ((float)c + 0.5f);
			sfs::screenFont(args.vg, font, sfs::TYPE_SCREEN);
			std::string l = subLabel[c], nn = note[c];
			float lw = nvgTextBounds(args.vg, 0, 0, l.c_str(), NULL, NULL);
			float nw = nvgTextBounds(args.vg, 0, 0, nn.c_str(), NULL, NULL);
			float gap = X(10.f);
			float x0 = X(cx) - (lw + gap + nw) * 0.5f;
			text(args, x0, X(FOOT_Y),
			     active[c] ? sfs::SCREEN_TEXT : sfs::SCREEN_PMID, l,
			     sfs::TYPE_SCREEN, NVG_ALIGN_LEFT);
			text(args, x0 + lw + gap, X(FOOT_Y),
			     active[c] ? sfs::SCREEN_HOT : sfs::SCREEN_PMID, nn,
			     sfs::TYPE_SCREEN, NVG_ALIGN_LEFT);
			if (c) {                                  // cell separators
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, X(MARGIN + cw * (float)c), X(RULE_FOOT + 1.f));
				nvgLineTo(args.vg, X(MARGIN + cw * (float)c), X(RULE_FOOT + 52.f));
				nvgStrokeColor(args.vg, sfs::SCREEN_LINE);
				nvgStrokeWidth(args.vg, std::max(X(2.f), 1.f));
				nvgStroke(args.vg);
			}
		}
	}

	void drawLive(const DrawArgs& args) {
		Key* m = module;
		bool chromatic = m->chromaticKey();

		bool lit[12] = {};
		float sounding[KEY_NCH];
		int nSound = 0;
		for (int c = 0; c < KEY_NCH; c++) {
			if (!m->shownActive[c]) continue;
			sounding[nSound++] = m->shownVolts[c] * 12.f - (float)m->rootNote;
			int s = (int)std::lround(m->shownVolts[c] * 12.f);
			lit[((s % 12) + 12) % 12] = true;
		}

		std::string sname = m->busActive ? string::f("BUS %d", m->parent.n)
		    : m->customMask ? std::string("Custom")
		    : m->scaleIsScala() ? (m->scalaLoaded ? m->scalaName : std::string("No Scala file"))
		    : std::string(sfs::SCALES[clamp(m->scaleIndex, 0, sfs::NUM_SCALES - 1)].shortName);
		if (sname.size() > 22) sname.resize(22);
		header(args, KEY_NOTES[m->rootNote], sname);

		if (chromatic) drawKeyboard(args, m->keyboardMask(), m->rootNote, lit);
		else           drawStrip(args, m->parent, sounding, nSound);
		rule(args, RULE_SUB);

		// Which sub-scales are actually driving a channel, and what each is
		// sounding — the rows showed structure but no activity, which made them
		// the one part of the screen you could not read while playing.
		bool used[KEY_NSUB] = {}, subLive[KEY_NSUB] = {};
		int  litDeg[KEY_NSUB] = {-1, -1, -1};
		int  litPc[KEY_NSUB]  = {-1, -1, -1};
		for (int c = 0; c < KEY_NCH; c++) {
			int s = (int)std::round(m->params[Key::SUB_PARAM + c].getValue());
			if (s < 1 || s > KEY_NSUB) continue;
			used[s - 1] = true;
			if (!m->shownActive[c]) continue;
			subLive[s - 1] = true;
			float semi = m->shownVolts[c] * 12.f - (float)m->rootNote;
			litDeg[s - 1] = degreeIndexOf(m->parent, semi);
			litPc[s - 1]  = (((int)std::lround(m->shownVolts[c] * 12.f) % 12) + 12) % 12;
		}

		int n = subCellCount(chromatic);
		bool on[KEY_NSUB * KEY_EDITDEG] = {}, inP[KEY_NSUB * KEY_EDITDEG] = {};
		bool litC[KEY_NSUB * KEY_EDITDEG] = {}, outC[KEY_NSUB * KEY_EDITDEG] = {};
		for (int k = 0; k < KEY_NSUB; k++) {
			for (int i = 0; i < n; i++) {
				int f = k * KEY_EDITDEG + i;
				if (!chromatic) {
					on[f]   = (m->subMask[k] >> i) & 1;
					inP[f]  = true;                      // every cell IS a degree here
					litC[f] = subLive[k] && litDeg[k] == i;
					continue;
				}
				int sfr = ((i - m->rootNote) % 12 + 12) % 12;
				int deg = -1;
				for (int d = 0; d < m->parent.n && d < KEY_EDITDEG; d++)
					if (std::fabs(m->parent.iv[d] - (float)sfr) < 0.02f) deg = d;
				inP[f]  = (deg >= 0);
				on[f]   = (deg >= 0) ? (((m->subMask[k] >> deg) & 1) != 0)
				                     : (m->freeSub && ((m->subChrom[k] >> sfr) & 1) != 0);
				outC[f] = on[f] && deg < 0;
				litC[f] = subLive[k] && litPc[k] == i;
			}
		}
		drawSubRows(args, n, chromatic, used, on, inP, litC, outC);

		const char* sl[KEY_NCH];
		std::string note[KEY_NCH];
		bool act[KEY_NCH];
		for (int c = 0; c < KEY_NCH; c++) {
			int sIdx = (int)std::round(m->params[Key::SUB_PARAM + c].getValue());
			sl[c] = KEY_SUBNAME[clamp(sIdx, 0, KEY_NSUB)];
			act[c] = m->shownActive[c];
			if (act[c]) {
				int s = (int)std::lround(m->shownVolts[c] * 12.f);
				note[c] = std::string(KEY_NOTES[((s % 12) + 12) % 12])
				        + std::to_string(4 + (int)std::floor(s / 12.f));
			} else note[c] = "–";
		}
		footer(args, sl, note, act);
	}

	// The thumbnail shows Pelog, because the region strip is the reason this
	// module looks different from every other quantizer.
	void drawPreview(const DrawArgs& args) {
		KeyScale pel;
		static const float PEL[7] = {0.f, 1.2f, 2.7f, 5.4f, 7.0f, 8.0f, 10.4f};
		for (int i = 0; i < 7; i++) pel.iv[i] = PEL[i];
		pel.n = 7; pel.period = 12.f;

		header(args, "C", "Pelog");
		float sounding[2] = {0.f, 5.4f};
		drawStrip(args, pel, sounding, 2);
		rule(args, RULE_SUB);

		static const bool used[KEY_NSUB] = {true, false, false};
		bool on[KEY_NSUB * KEY_EDITDEG] = {}, inP[KEY_NSUB * KEY_EDITDEG] = {};
		bool litC[KEY_NSUB * KEY_EDITDEG] = {}, outC[KEY_NSUB * KEY_EDITDEG] = {};
		static const uint32_t sm[KEY_NSUB] = {0b0010101, 0b0011001, 0b1010101};
		for (int k = 0; k < KEY_NSUB; k++)
			for (int i = 0; i < 7; i++) {
				on[k * KEY_EDITDEG + i] = (sm[k] >> i) & 1;
				inP[k * KEY_EDITDEG + i] = true;
			}
		litC[0] = true;                                  // sub 1 sounding its root
		drawSubRows(args, 7, false, used, on, inP, litC, outC);

		static const char* sl[KEY_NCH] = {"1", "M", "M", "M"};
		std::string note[KEY_NCH] = {"C3", "F3", "–", "–"};
		bool act[KEY_NCH] = {true, true, false, false};
		footer(args, sl, note, act);
	}
};


// =============================================================================
// Panel — 14HP.
// =============================================================================

struct KeyWidget : ModuleWidget {
	KeyWidget(Key* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/key.svg")));
		using sfs::hp;

		// NO sfs::PanelLabels. res/key.svg is the designer's own file, published
		// by `figma_panel_template.py --publish key`, and it carries the title,
		// the row labels and the logo.
		//
		// Positions are in MILLIMETRES read straight out of that file rather than
		// on the hp() grid: the four channel columns sit on an 11.855mm pitch,
		// which is 2.334HP, and the rows are distributed down the panel rather
		// than grid-snapped.
		const float col[KEY_NCH] = {16.04f, 27.90f, 39.75f, 51.60f};
		const float yIn = 58.50f, ySub = 70.40f, yOff = 82.30f, yOut = 94.10f;
		// The bottom row is on its OWN spacing, not the channel columns': the key
		// is not a fifth channel, and the art puts ROOT / SCALE / TRIG together on
		// the left with the two outputs on their own plate to the right.
		const float yPot = 110.60f, yJack = 121.00f;
		const float kRoot = 6.90f, kScale = 18.75f, kTrig = 30.61f;
		const float kRootOut = 48.22f, kScaleOut = 60.07f;

		KeyDisplay* disp = new KeyDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(3.387f, 11.854f));
		disp->box.size = mm2px(Vec(64.35f, 40.64f));
		addChild(disp);

		for (int c = 0; c < KEY_NCH; c++) {
			addInput (createInputCentered <PJ301MPort>(mm2px(Vec(col[c], yIn)),  module, Key::IN_INPUT + c));
			addParam (createParamCentered <Trimpot>   (mm2px(Vec(col[c], ySub)), module, Key::SUB_PARAM + c));
			addParam (createParamCentered <Trimpot>   (mm2px(Vec(col[c], yOff)), module, Key::OFFSET_PARAM + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(col[c], yOut)), module, Key::OUT_OUTPUT + c));
		}

		// ── the key, in and out ────────────────────────────────────────────────
		addParam (createParamCentered <Trimpot>   (mm2px(Vec(kRoot,  yPot)),  module, Key::ROOT_PARAM));
		addInput (createInputCentered <PJ301MPort>(mm2px(Vec(kRoot,  yJack)), module, Key::ROOT_INPUT));
		addParam (createParamCentered <Trimpot>   (mm2px(Vec(kScale, yPot)),  module, Key::SCALE_PARAM));
		addInput (createInputCentered <PJ301MPort>(mm2px(Vec(kScale, yJack)), module, Key::SCALE_INPUT));
		addInput (createInputCentered <PJ301MPort>(mm2px(Vec(kTrig,  yJack)), module, Key::TRIG_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(kRootOut,  yJack)), module, Key::ROOT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(kScaleOut, yJack)), module, Key::SCALE_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Key* m = dynamic_cast<Key*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);

		menu->addChild(createMenuItem("Load Scala file (.scl)…",
			m->scalaLoaded ? m->scalaName : "", [=]() {
				osdialog_filters* f = osdialog_filters_parse("Scala scale:scl");
				char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, f);
				osdialog_filters_free(f);
				if (!path) return;
				KeyScale s;
				std::string name;
				if (keyLoadScala(path, s, name)) {
					m->scala = s;
					m->scalaName = name.empty() ? std::string("Scala") : name;
					m->scalaPath = path;
					m->scalaLoaded = true;
					// Loading a file is a request to hear it.
					m->params[Key::SCALE_PARAM].setValue((float)Key::SCALA_INDEX);
					m->customMask = false;
					m->rebuild();
				}
				else {
					osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK,
						"Could not read that as a Scala (.scl) file.");
				}
				std::free(path);
			}));
		if (m->scalaLoaded) {
			menu->addChild(createMenuLabel(string::f("   %d degrees, period %.1f cents",
				m->scala.n, m->scala.period * 100.f)));
			// Key quantises from all of them; the SCALE bus cannot carry all of
			// them. Better said here, at the point the file is loaded, than
			// discovered later as a downstream module quietly playing a
			// different scale.
			if (m->scala.n > sfs::BUS_MAXDEG)
				menu->addChild(createMenuLabel(string::f(
					"   SCALE out carries the first %d — %d will not travel",
					sfs::BUS_MAXDEG, m->scala.n - sfs::BUS_MAXDEG)));
		}

		menu->addChild(new MenuSeparator);
		// Degrees is the default because it is the scale-aware answer: +2 moves
		// two steps UP THE SCALE and stays in key, where +2 semitones does not.
		menu->addChild(createBoolPtrMenuItem("Offset in scale degrees", "",
			&m->offsetInDegrees));
		menu->addChild(createIndexPtrSubmenuItem("Rounding",
			{"Nearest", "Down", "Up"}, &m->roundMode));

		// Off by default, because the whole idea of a sub-scale is that it cannot
		// take you out of the key. Turned on, the dark cells in a sub row become
		// clickable and a sub-scale can carry an accidental — a flat 5th under a
		// walking line, a passing tone the rest of the patch never sees. Those
		// picks are stored as semitones from the root, so they transpose with ROOT
		// but do not follow a SCALE change: there is no degree for them to follow.
		// A ring around the cell marks one, so an accidental reads as an accidental.
		menu->addChild(createBoolPtrMenuItem("Sub-scales may leave the key", "",
			&m->freeSub));
		if (!m->chromaticKey())
			menu->addChild(createMenuLabel("   (this scale is not 12-tone; rows show its degrees)"));

		menu->addChild(createSubmenuItem("Hysteresis",
			string::f("%.0f cents", m->hysteresisCents), [=](Menu* sub) {
				struct HystQuantity : Quantity {
					Key* m;
					void setValue(float v) override { m->hysteresisCents = clamp(v, 0.f, 50.f); }
					float getValue() override { return m->hysteresisCents; }
					float getDefaultValue() override { return 12.f; }
					float getMinValue() override { return 0.f; }
					float getMaxValue() override { return 50.f; }
					std::string getLabel() override { return "Hysteresis"; }
					std::string getUnit() override { return " cents"; }
				};
				HystQuantity* q = new HystQuantity;
				q->m = m;
				Slider* sl = new Slider;
				sl->quantity = q;
				sl->box.size.x = 180.f;
				sub->addChild(sl);
			}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Revert keyboard to the selected scale", "", [=]() {
			m->customMask = false;
			m->rebuild();
		}));
		menu->addChild(createMenuItem("Reset sub-scales", "", [=]() {
			m->defaultSubs();
			m->rebuild();
		}));
		menu->addChild(createMenuItem("Sub-scales: every degree", "", [=]() {
			for (int k = 0; k < KEY_NSUB; k++) m->subMask[k] = 0xFFFFFFFFu;
			m->rebuild();
		}));
	}
};

std::string KeyOffsetQuantity::getUnit() {
	Key* m = dynamic_cast<Key*>(module);
	return (m && m->offsetInDegrees) ? " scale degrees" : " semitones";
}
std::string KeyOffsetQuantity::getDescription() {
	Key* m = dynamic_cast<Key*>(module);
	if (m && m->offsetInDegrees)
		return "Moves the quantized note this many steps along the channel's own "
		       "scale, so it stays in key. On a sub-scale the steps are that "
		       "sub-scale's degrees. Switch to semitones in the menu.";
	return "Shifts the incoming pitch this many semitones BEFORE quantizing, so "
	       "the scale decides where it lands — a step may not change the note at "
	       "all. Switch to scale degrees in the menu.";
}

Model* modelKey = createModel<Key, KeyWidget>("Key");
