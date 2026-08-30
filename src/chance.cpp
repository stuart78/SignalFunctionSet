#include "plugin.hpp"
#include "scale-bus.hpp"
#include "scales.hpp"
#include "chance-markov.hpp"
#include "sfs_lut.hpp"
#include <cmath>

// ─── Chance — a garden of forking paths ──────────────────────────────────────
// A generative melodic sequencer. Set Key / Root and a Start..End window. A
// seeded walk builds a core skeleton melody (shaped by Gravity = up/down bias
// and Drift = move size). BRANCH is the per-step chance that a step strays from
// the core to a neighbouring note — bounded and NON-cascading, so the core
// always shows through. Everything (strays, rests, holds, octaves) is DERIVED
// FROM THE PATTERN SEED, so each of the 8 pattern slots is a fixed, repeatable
// melody: its repeats replay identically and every knob's effect is audible.
// Variety comes from the pattern rotation and Randomize (= a new seed = a new
// pattern). HARMONY out is a diatonic 2nd voice above/below the main line.

static const int NUM_NODES = 8;
static const int NUM_SCALES = sfs::NUM_SCALES;

static const float CHANCE_DISP_X0 = 5.f, CHANCE_DISP_W = 122.f;

// deterministic hash → [0,1) (defines the stable core)
static inline uint32_t hashU(uint32_t seed, int node, int salt) {
	uint32_t h = seed + (uint32_t)node * 0x9E3779B9u + (uint32_t)salt * 0x85EBCA77u;
	h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
	return h;
}
static inline float hashF(uint32_t seed, int node, int salt) {
	return (float)(hashU(seed, node, salt) & 0xFFFFFF) / (float)0x1000000;
}
static inline int ifloordiv(int a, int b) {
	int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q;
}
static inline int reflectPos(int x, int m) {
	if (m <= 0) return 0;
	while (x > m)  x = 2 * m - x;
	while (x < -m) x = -2 * m - x;
	return x;
}
// Markov-weighted next position. From `fromPos` (absolute degree, octave folded by
// reflection), enumerate candidates within a drift-bounded window; weight each by
// the scale's transition table (tonal tendency) × a stepwise falloff (drift widens)
// × a gravity up/down bias, then pick with rnd01 (seeded or live).
static int chancePickNext(int fromPos, int scaleIdx, int size, int maxPos,
                          float drift, float gravity, float rnd01) {
	if (size < 1) return fromPos;
	int maxStep = 1 + (int)std::lround(clamp(drift, 0.f, 1.f) * (size - 1));
	int fromDeg = ((fromPos % size) + size) % size;
	float cum[48]; int cand[48]; int nc = 0; float total = 0.f;
	for (int dp = -maxStep; dp <= maxStep && nc < 48; dp++) {
		if (dp == 0) continue;                       // must move (REPEAT handles staying)
		int np = reflectPos(fromPos + dp, maxPos);
		int toDeg = ((np % size) + size) % size;
		float wt = chancemk::weight(scaleIdx, fromDeg, toDeg, size);
		float fall = std::exp(-(float)(std::abs(dp) - 1) * (1.3f - drift));   // drift↑ flattens
		float gb = (dp > 0) ? (0.5f + 0.5f * gravity) : (0.5f - 0.5f * gravity);
		gb = clamp(gb, 0.05f, 1.f);
		float w = wt * fall * gb;
		if (w < 0.f) w = 0.f;
		total += w; cum[nc] = total; cand[nc] = np; nc++;
	}
	if (total <= 0.f || nc == 0) return fromPos;
	float pick = clamp(rnd01, 0.f, 0.999999f) * total;
	for (int i = 0; i < nc; i++) if (pick <= cum[i]) return cand[i];
	return cand[nc - 1];
}

struct Chance;

// ─── Display ─────────────────────────────────────────────────────────────────
struct ChanceDisplay : OpaqueWidget {
	Chance* module = nullptr;
	std::shared_ptr<Font> font;
	void drawLayer(const DrawArgs& args, int layer) override;
	void drawScene(const DrawArgs& args, const int* core, const int* play, const int* base,
	               const bool* rest, const int* harm, bool harmOn, const int* seq, int seqLen,
	               int curNode, int maxPos, bool running, int root, int scaleIdx);
	void drawReseedIcon(NVGcontext* vg, rack::math::Rect r, bool on);
	void drawKeyReadout(NVGcontext* vg, int root, const sfs::BusScale& sc);
	void drawBank(const DrawArgs& args);
	void drawControls(const DrawArgs& args);
	void drawPreview(const DrawArgs& args);

	// Layout mapped from Stuart's screen SVG (content x∈[14,414], y∈[0,178] — the screen
	// was shortened 2026-07-13; the pattern bank became thin RECTS to give the space back,
	// so the walk stays large). Top→bottom: key readout · pattern bank (thin P1-8 rects +
	// repeat boxes) · inverted-T rail · step-gate row (S1-8) · walk (C-axis).
	float SX(float x) const { return (x - 14.f) / 400.f * box.size.x; }
	float SY(float y) const { return y / 178.f * box.size.y; }
	float SW(float w) const { return w / 400.f * box.size.x; }
	float SH(float h) const { return h / 178.f * box.size.y; }
	float colCX(int i) const { return SX(41.f + i * 49.f); }     // node / cell centre column
	rack::math::Rect patCellRect(int i) const { return rack::math::Rect(Vec(SX(18+i*49), SY(19)), Vec(SW(47), SH(28))); }
	rack::math::Rect patModeRect(int i) const { return rack::math::Rect(Vec(SX(18+i*49+34), SY(20.5f)), Vec(SW(11), SH(11))); }
	rack::math::Rect patWaveRect(int i) const { return rack::math::Rect(Vec(SX(18+i*49+3), SY(32)), Vec(SW(41), SH(13))); }
	rack::math::Rect patRepRect(int i) const { return rack::math::Rect(Vec(SX(18+i*49), SY(49)), Vec(SW(47), SH(5))); }
	rack::math::Rect gateRect(int i) const { return rack::math::Rect(Vec(SX(18+i*49), SY(62)), Vec(SW(47), SH(18))); }
	float walkTop() const { return SY(85); }
	float walkBot() const { return SY(173); }
	int hitPatCell(Vec p) const {
		for (int i = 0; i < NUM_NODES; i++) if (patCellRect(i).contains(p)) return i;
		return -1;
	}
	void setRepFromX(int i, float x);
	void onButton(const ButtonEvent& e) override;
	void onDoubleClick(const DoubleClickEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
	void onHoverScroll(const HoverScrollEvent& e) override;
	int   dragRepCell = -1;    // repeat-row being scrubbed
	float dragMouseX = 0.f;
	Vec   dragPos;             // last press position (for onDoubleClick)
};

// ─── Module ──────────────────────────────────────────────────────────────────
struct Chance : Module {
	enum ParamId {
		PATTERN_PARAM,                             // 8 pattern active latches
		PATREP_PARAM = PATTERN_PARAM + NUM_NODES,  // 8 per-pattern repeat counts (1-8)
		KEY_PARAM = PATREP_PARAM + NUM_NODES,
		ROOT_PARAM, START_PARAM, END_PARAM, GRAVITY_PARAM, DRIFT_PARAM, BRANCH_PARAM,
		GATE_PARAM, RESTS_PARAM, EVEN_REST_PARAM, OCTAVE_PARAM, REPEAT_PARAM, GLIDE_PARAM,
		RANDOMIZE_PARAM, RESET_PARAM,
		HARMONY_PARAM,           // diatonic interval for the second voice
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT, RESET_INPUT, RANDOMIZE_INPUT, ROOT_CV_INPUT,
		GRAVITY_CV_INPUT, DRIFT_CV_INPUT, BRANCH_CV_INPUT, SCALE_CV_INPUT,
		RESTS_CV_INPUT, EVEN_REST_CV_INPUT, OCT_CV_INPUT, REPEAT_CV_INPUT,
		// Appended — never insert mid-enum, cables serialise by index.
		START_CV_INPUT, END_CV_INPUT, INPUTS_LEN
	};
	enum OutputId { CV_OUTPUT, GATE_OUTPUT, HARMONY_OUTPUT, HARM_GATE_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		NODE_LIGHT,
		PATTERN_LIGHT = NODE_LIGHT + NUM_NODES,
		LIGHTS_LEN = PATTERN_LIGHT + NUM_NODES
	};

	uint32_t patternSeed[NUM_NODES] = {};   // per-pattern generative core seed
	bool patternReseed[NUM_NODES] = {};     // per-pattern: regenerate a new seed on each play
	// Per-run variation: when true (default), the shaping rolls (rests, holds, octave
	// leaps, ratchets) re-roll each cycle from a fresh salt, so repeats of a fixed
	// pattern vary in articulation while the melody stays the same. When false, every
	// decision comes from patternSeed and repeats replay bit-identically (legacy).
	bool perRunVary = true;
	uint32_t cycleSalt = 0;                 // refreshed each cycle when perRunVary
	int  patternGate[NUM_NODES][NUM_NODES]; // [pattern][step] gate: 0 off, 1 gate, 2 tie
	int  patShape[NUM_NODES][NUM_NODES] = {}; // cached played contour per pattern (micro-waveform)
	int  shapeDiv = 0;                       // throttles the patShape refresh
	int playPat = 0, curRepeat = 1;         // pattern rotation state
	int editPat = 0;                         // which pattern the on-screen gate row edits
	int rangeOctaves = 1;

	int patRepeats(int p) { return clamp((int)std::round(params[PATREP_PARAM + p].getValue()), 1, 8); }
	bool patActive(int p) { return params[PATTERN_PARAM + p].getValue() > 0.5f; }
	int firstActivePattern() { for (int i = 0; i < NUM_NODES; i++) if (patActive(i)) return i; return 0; }
	int nextActivePattern(int from) {
		for (int i = 1; i <= NUM_NODES; i++) { int idx = (from + i) % NUM_NODES; if (patActive(idx)) return idx; }
		return from;   // only this one active → stay
	}
	// Rotation lands on pattern p → a new "play": reseed its core if it's a reseed pattern.
	void enterPattern(int p) {
		playPat = p; curRepeat = 1;
		if (patternReseed[p]) patternSeed[p] = random::u32();
	}
	// Per-pattern, per-step gate: 0 = off (rest), 1 = gate, 2 = tie (hold previous note).
	int gateMode(int pat, int step) { return clamp(patternGate[pat][step], 0, 2); }
	// Which pattern the main walk shows/computes: the PLAYING one while running, the FOCUSED
	// (clicked) one while stopped — so clicking a thumb loads that pattern into the big window.
	int walkPat() { return started ? playPat : editPat; }

	int curScale = 0, curRoot = 0, maxPosDeg = 7;
	// The scale in force: the canonical entry named by curScale, or the whole
	// thing read off a polyphonic SCALE input. curScale stays as the index
	// because the Markov tables are keyed on it -- those describe a musical
	// STYLE, which a Scala file has no opinion about.
	sfs::BusScale scale;
	float curGateFrac = 0.5f;

	// per node-index (so the display aligns with the columns)
	int  coreNote[NUM_NODES] = {};   // stable seeded skeleton
	int  playNote[NUM_NODES] = {};   // what plays this cycle (core or a stray), octave leaps applied
	int  baseNote[NUM_NODES] = {};   // playNote WITHOUT the octave overlay — the display's base contour
	int  harmNote[NUM_NODES] = {};   // the complement
	bool pathRest[NUM_NODES] = {};
	bool pathTie[NUM_NODES] = {};     // this step is a tie (hold previous note, no retrigger)
	int  pathDur[NUM_NODES] = {};
	int  pathRatchet[NUM_NODES] = {}; // sub-hits for this step: 1 = single, 2/3 = ratchet burst
	int  seq[NUM_NODES] = {};
	int  seqLen = NUM_NODES;
	int  previewLen = NUM_NODES;      // step count of the cached micro-waveforms

	int  curStep = 0, curNodeIdx = 0;
	bool started = false;
	int  nodeClocksRemaining = 1;
	float clockPeriod = 0.5f;
	int  samplesSinceClock = 0;

	bool gateOpen = false; float gateTimer = 0.f;
	int  ratchetLeft = 0;             // sub-hits still to fire after the current burst
	float ratchetSlot = 0.f, ratchetGate = 0.f, ratchetNext = 0.f;   // burst spacing / on-time / countdown
	float targetCV = 0.f, targetHarmony = 0.f, currentCV = 0.f, harmonyCV = 0.f;
	bool dispRunning = false;
	int  previewDiv = 0;       // throttles the stopped-state preview recompute
	// Set by the UI (gate-box click) so an edit lands on the very next sample instead of
	// waiting for the next cycle. The audio thread owns the recompute; the UI only flags.
	bool recomputePending = false;
	bool startOnRoot = true;   // ground each cycle on the tonic (context menu)
	bool harmonyOn = false;    // harmony interval currently enabled

	dsp::SchmittTrigger clockTrig, resetTrigIn, resetTrigBtn, randTrigIn, randTrigBtn;

	Chance() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < NUM_NODES; i++) {
			configParam(PATTERN_PARAM + i, 0.f, 1.f, i == 0 ? 1.f : 0.f, string::f("Pattern %d active", i + 1));
			configParam(PATREP_PARAM + i, 1.f, 8.f, 1.f, string::f("Pattern %d repeats", i + 1));
			getParamQuantity(PATREP_PARAM + i)->snapEnabled = true;
		}
		std::vector<std::string> scaleNames;
		for (int i = 0; i < NUM_SCALES; i++) scaleNames.push_back(sfs::SCALES[i].longName);
		configSwitch(KEY_PARAM, 0.f, (float)(NUM_SCALES - 1), 0.f, "Scale", scaleNames);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root note",
			{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"});
		configSwitch(START_PARAM, 0.f, (float)(NUM_NODES - 1), 0.f, "Start step",
			{"1","2","3","4","5","6","7","8"});
		configSwitch(END_PARAM, 0.f, (float)(NUM_NODES - 1), (float)(NUM_NODES - 1), "End step",
			{"1","2","3","4","5","6","7","8"});
		configParam(GRAVITY_PARAM, -1.f, 1.f, 0.f, "Gravity (down – up)");
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.3f, "Drift (move size)", "%", 0.f, 100.f);
		configParam(BRANCH_PARAM, 0.f, 1.f, 0.25f, "Branch (stray from core)", "%", 0.f, 100.f);
		configParam(GATE_PARAM, 0.f, 1.f, 0.5f, "Gate length", "%", 0.f, 100.f);
		configParam(RESTS_PARAM, 0.f, 1.f, 0.f, "Odd-note rests", "%", 0.f, 100.f);
		configParam(EVEN_REST_PARAM, 0.f, 1.f, 0.f, "Even-note rests", "%", 0.f, 100.f);
		configParam(OCTAVE_PARAM, 0.f, 1.f, 0.f, "Octave leaps", "%", 0.f, 100.f);
		configParam(REPEAT_PARAM, 0.f, 1.f, 0.f, "Ratchet (2-3 bursts within a step)", "%", 0.f, 100.f);
		configParam(GLIDE_PARAM, 0.f, 1.f, 0.f, "Glide");
		configButton(RANDOMIZE_PARAM, "Randomize (new core)");
		configButton(RESET_PARAM, "Reset");
		configSwitch(HARMONY_PARAM, 0.f, 7.f, 0.f, "Harmony (2nd voice)",
			{"Off", "3rd up", "5th up", "Octave up", "3rd down", "5th down", "Octave down", "Varied"});
		getParamQuantity(HARMONY_PARAM)->snapEnabled = true;
		for (int p = 0; p < NUM_NODES; p++)
			for (int s = 0; s < NUM_NODES; s++) patternGate[p][s] = 1;   // default: every step gates

		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");
		configInput(RANDOMIZE_INPUT, "Randomize trigger");
		configInput(ROOT_CV_INPUT, "Root CV (1V/oct, semitone-quantized) — e.g. Note or Phrase root out");
		configInput(SCALE_CV_INPUT, "Scale-select CV (1V per scale) — e.g. Note or Phrase scale out");
		configInput(GRAVITY_CV_INPUT, "Gravity CV");
		configInput(DRIFT_CV_INPUT, "Drift CV");
		configInput(BRANCH_CV_INPUT, "Branch CV");
		configInput(RESTS_CV_INPUT, "Odd-note rests CV");
		configInput(EVEN_REST_CV_INPUT, "Even-note rests CV");
		configInput(OCT_CV_INPUT, "Octave leaps CV");
		configInput(REPEAT_CV_INPUT, "Ratchet CV");
		configInput(START_CV_INPUT, "Start step CV (1V/step, offsets the trimpot)");
		configInput(END_CV_INPUT, "End step CV (1V/step, offsets the trimpot) — end before start plays the window backwards");
		configOutput(CV_OUTPUT, "CV (1V/oct)");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(HARMONY_OUTPUT, "Harmony CV — the 2nd voice (diatonic interval)");
		configOutput(HARM_GATE_OUTPUT, "Harmony gate");

		for (int i = 0; i < NUM_NODES; i++) patternSeed[i] = random::u32();
	}

	float voltsFromPos(int pos) {
		const sfs::BusScale& sc = scale;
		int sz = std::max(sc.size, 1);
		int oct = ifloordiv(pos, sz);
		int deg = pos - oct * sz;
		// The PERIOD, not twelve, and the degree unrounded. Rounding to a whole
		// semitone here quietly flattened Pelog and Slendro onto 12-TET long
		// before Scala existed as an option.
		float semis = (float)oct * sc.period + sc.intervals[deg];
		return (float)curRoot / 12.f + semis / 12.f;
	}

	// The play window: trimpot + CV at 1V/step. END before START is not an error —
	// it plays the window backwards, which is the whole point of having both.
	void windowIdx(int& sIdx, int& eIdx) {
		sIdx = clamp((int)std::round(params[START_PARAM].getValue() + inputs[START_CV_INPUT].getVoltage()), 0, NUM_NODES - 1);
		eIdx = clamp((int)std::round(params[END_PARAM].getValue()   + inputs[END_CV_INPUT].getVoltage()),   0, NUM_NODES - 1);
	}

	// Build the window order + the deterministic core skeleton (stable per seed).
	void computeCore() {
		const sfs::BusScale& sc = scale;
		int sz = sc.size;
		maxPosDeg = sz * rangeOctaves;

		int sIdx, eIdx; windowIdx(sIdx, eIdx);
		int dir = (eIdx >= sIdx) ? 1 : -1;
		seqLen = std::abs(eIdx - sIdx) + 1;
		for (int k = 0; k < seqLen; k++) seq[k] = sIdx + dir * k;

		float gGrav = clamp(params[GRAVITY_PARAM].getValue() + inputs[GRAVITY_CV_INPUT].getVoltage() / 5.f, -1.f, 1.f);
		float gDrift = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);

		int pos = 0;
		for (int k = 0; k < seqLen; k++) {
			int ni = seq[k];
			if (k == 0 && startOnRoot) { coreNote[ni] = 0; pos = 0; continue; }  // ground on tonic
			pos = chancePickNext(pos, curScale, sz, maxPosDeg, gDrift, gGrav, hashF(patternSeed[walkPat()], ni, 2));
			coreNote[ni] = pos;
		}
	}

	// Reproduce the actual PLAYED contour for pattern `pat`, step-ordered over the current
	// window, so each cell's micro-waveform matches what that pattern really plays.
	// MUST mirror computeCore + computeCycle's pitch logic (kept in sync by hand).
	int buildPlayedPreview(int pat, int* out) {
		uint32_t seed = patternSeed[pat];
		const sfs::BusScale& sc = scale;
		int sz = sc.size, mp = sz * rangeOctaves;
		float gGrav = clamp(params[GRAVITY_PARAM].getValue() + inputs[GRAVITY_CV_INPUT].getVoltage() / 5.f, -1.f, 1.f);
		float gDrift = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gBranch = clamp(params[BRANCH_PARAM].getValue() + inputs[BRANCH_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gOct = clamp(params[OCTAVE_PARAM].getValue() + inputs[OCT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		int sIdx, eIdx; windowIdx(sIdx, eIdx);
		int dir = (eIdx >= sIdx) ? 1 : -1, len = std::abs(eIdx - sIdx) + 1;
		int sq[NUM_NODES]; for (int k = 0; k < len; k++) sq[k] = sIdx + dir * k;
		int cr[NUM_NODES] = {}, pos = 0;
		for (int k = 0; k < len; k++) {
			int ni = sq[k];
			if (k == 0 && startOnRoot) { cr[ni] = 0; pos = 0; continue; }
			pos = chancePickNext(pos, curScale, sz, mp, gDrift, gGrav, hashF(seed, ni, 2));
			cr[ni] = pos;
		}
		// Mirror computeCycle: chain the walk on the octave-FREE note so the octave
		// overlay doesn't perturb the contour (prevMel), while ties hold the output (prevOut).
		int prevMel = startOnRoot ? 0 : cr[sq[0]];
		int prevOut = prevMel;
		for (int k = 0; k < len; k++) {
			int ni = sq[k], core = cr[ni];
			int stray = chancePickNext(prevMel, curScale, sz, mp, gDrift, gGrav, hashF(seed, ni, 10));
			int play = (hashF(seed, ni, 11) < gBranch) ? stray : core;
			if (k == 0 && startOnRoot) play = 0;
			// (ratchet — hashF salt 12 — affects retriggering, not pitch, so it's not applied here)
			int o, mel;
			if (gateMode(pat, ni) == 2) { o = prevOut; mel = prevMel; }   // tie holds the previous note
			else {
				mel = play; o = play;
				if (hashF(seed, ni, 13) < gOct) o = play + (hashF(seed, ni, 14) < 0.5f ? sz : -sz);
			}
			out[k] = o; prevMel = mel; prevOut = o;
		}
		return len;
	}

	// Harmony (2nd voice) — mode 0 = Off.
	bool harmonyEnabled() { return (int)std::round(params[HARMONY_PARAM].getValue()) != 0; }
	// Degree offset for node ni. Fixed modes ignore ni; "Varied" picks a seeded
	// consonant interval per step so the 2nd voice weaves instead of running parallel.
	int harmonyOffsetFor(int ni) {
		int sz = std::max(scale.size, 1);
		switch ((int)std::round(params[HARMONY_PARAM].getValue())) {
			case 1: return  2;    // 3rd up
			case 2: return  4;    // 5th up
			case 3: return  sz;   // octave up
			case 4: return -2;    // 3rd down
			case 5: return -4;    // 5th down
			case 6: return -sz;   // octave down
			case 7: {             // Varied — weaving consonances (3rd/5th/6th/octave, some contrary)
				int opts[8] = { 2, 4, 5, sz, -2, 5, 2, -4 };
				int idx = clamp((int)(hashF(patternSeed[playPat], ni, 20) * 8.f), 0, 7);
				return opts[idx];
			}
			default: return 0;    // Off
		}
	}

	// Each cycle: keep the stable core, then decide live, per-step strays.
	void computeCycle() {
		computeCore();
		const sfs::BusScale& sc = scale;
		int sz = sc.size;
		float gGrav = clamp(params[GRAVITY_PARAM].getValue() + inputs[GRAVITY_CV_INPUT].getVoltage() / 5.f, -1.f, 1.f);
		float gDrift = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gBranch = clamp(params[BRANCH_PARAM].getValue() + inputs[BRANCH_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		// Rests are split by note position: the Odd control governs the 1st/3rd/5th/…
		// notes of the cycle, the Even control the 2nd/4th/6th/… — for predictable
		// rhythmic variation (e.g. rest every off-beat).
		float gRestOdd  = clamp(params[RESTS_PARAM].getValue() + inputs[RESTS_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gRestEven = clamp(params[EVEN_REST_PARAM].getValue() + inputs[EVEN_REST_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gOct   = clamp(params[OCTAVE_PARAM].getValue() + inputs[OCT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gRep   = clamp(params[REPEAT_PARAM].getValue() + inputs[REPEAT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);

		uint32_t sd = patternSeed[walkPat()];
		// Melody (core + stray/branch) uses `sd` so the pattern keeps its identity.
		// The shaping rolls use `shSd`: in per-run mode a fresh per-cycle salt is mixed
		// in so rests/holds/octave-leaps/ratchets vary every cycle; in fixed mode
		// shSd == sd, so repeats replay bit-identically (legacy behaviour).
		uint32_t shSd = perRunVary ? (sd ^ cycleSalt) : sd;
		// Two running "previous note" values. chancePickNext keys off ABSOLUTE position,
		// so if the octave overlay fed the walk, per-run octave variation would cascade
		// into different note choices and regenerate the whole melody. So chain the walk
		// on the octave-FREE melodic note (prevMel — depends only on `sd`, hence fixed
		// per pattern); ties hold the actual OUTPUT note (prevOut, octave included).
		// In legacy fixed mode the two track together, so octave feeds the walk as before.
		int prevMel = startOnRoot ? 0 : coreNote[seq[0]];
		int prevOut = prevMel;
		for (int k = 0; k < seqLen; k++) {
			int ni = seq[k];
			int core = coreNote[ni];
			// Melody: Markov detour from the previous melodic note. Seeded from `sd`, so
			// the note skeleton is the pattern's fixed identity — never varies per run.
			int stray = chancePickNext(prevMel, curScale, sz, maxPosDeg, gDrift, gGrav, hashF(sd, ni, 10));
			bool deviate = (hashF(sd, ni, 11) < gBranch);
			int play = deviate ? stray : core;
			if (k == 0 && startOnRoot) play = 0;                 // ground each cycle on the root

			int gm = gateMode(walkPat(), ni);  // 0 off, 1 gate, 2 tie
			int out, mel;
			if (gm == 2) {
				out = prevOut; mel = prevMel;    // tie: hold previous note, don't advance the walk
			} else {
				mel = play;
				out = play;
				// Octave leap = articulation overlay (shSd), not part of the melody.
				if (hashF(shSd, ni, 13) < gOct)
					out = play + (hashF(shSd, ni, 14) < 0.5f ? sz : -sz);
				if (!perRunVary) mel = out;      // legacy: octave fed the Markov chain
			}
			prevMel = mel; prevOut = out;

			playNote[ni] = out;
			baseNote[ni] = mel;                          // octave-free contour (for the display)
			harmNote[ni] = out + harmonyOffsetFor(ni);   // 2nd voice (for output + display)
			pathTie[ni]  = (gm == 2);
			// Rest roll uses the Odd/Even control by the note's position in the cycle
			// (k is 0-based, so k%2==0 is the 1st/3rd/5th… note).
			float gRest = (k % 2 == 0) ? gRestOdd : gRestEven;
			pathRest[ni] = (gm == 0) || (hashF(shSd, ni, 15) < gRest);
			pathDur[ni]  = 1;                            // Hold removed (replaced by Even rests)
			// Ratchet: the note keeps its own pitch but retriggers 2-3 times inside the step
			// (a burst), instead of the old "hold the previous note" behaviour.
			pathRatchet[ni] = (hashF(shSd, ni, 12) < gRep) ? (2 + (int)(hashF(shSd, ni, 18) * 2.f)) : 1;
		}
	}

	void fireStep(int k) {
		curNodeIdx = seq[k];
		int playPos = playNote[curNodeIdx];
		targetCV = voltsFromPos(playPos);
		harmonyOn = harmonyEnabled();                    // 2nd voice on?
		targetHarmony = voltsFromPos(harmNote[curNodeIdx]);
		ratchetLeft = 0;
		if (pathTie[curNodeIdx]) {
			// tie: keep the gate high through the whole step, no retrigger (legato hold)
			gateOpen = true; gateTimer = clockPeriod + std::max(0.001f, curGateFrac * clockPeriod);
		} else if (!pathRest[curNodeIdx]) {
			int rat = pathRatchet[curNodeIdx];
			if (rat > 1) {
				// Ratchet: rat evenly-spaced bursts across the step, each an independent gate
				// (same pitch) — a rapid retrigger contained within this step's time.
				ratchetSlot = clockPeriod / (float)rat;
				ratchetGate = std::max(0.001f, curGateFrac * ratchetSlot);
				gateOpen = true; gateTimer = ratchetGate;
				ratchetNext = ratchetSlot; ratchetLeft = rat - 1;
			} else {
				gateOpen = true; gateTimer = std::max(0.001f, curGateFrac * clockPeriod);
			}
		}
	}

	void onClock() {
		if (!started) {
			started = true; enterPattern(firstActivePattern()); curStep = 0;
			if (perRunVary) cycleSalt = random::u32();
			computeCycle(); fireStep(0); nodeClocksRemaining = pathDur[seq[0]]; return;
		}
		if (nodeClocksRemaining > 1) { nodeClocksRemaining--; return; }
		curStep++;
		if (curStep >= seqLen) {
			curStep = 0;
			// advance repeat count, then hop to the next active pattern when done
			if (++curRepeat > patRepeats(playPat)) enterPattern(nextActivePattern(playPat));
			if (perRunVary) cycleSalt = random::u32();   // new run → re-roll shaping
			computeCycle();
		}
		fireStep(curStep);
		nodeClocksRemaining = pathDur[seq[curStep]];
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;

		int scaleCv = inputs[SCALE_CV_INPUT].isConnected() ? (int)std::round(inputs[SCALE_CV_INPUT].getVoltage()) : 0;
		curScale = clamp((int)std::round(params[KEY_PARAM].getValue()) + scaleCv, 0, NUM_SCALES - 1);
		scale = sfs::busResolve(inputs[SCALE_CV_INPUT],
		                        (int)std::round(params[KEY_PARAM].getValue()));
		if (scale.extended) curScale = scale.index;
		int rootKnob = (int)std::round(params[ROOT_PARAM].getValue());
		int rootCv = inputs[ROOT_CV_INPUT].isConnected() ? (int)std::round(inputs[ROOT_CV_INPUT].getVoltage() * 12.f) : 0;
		curRoot = ((rootKnob + rootCv) % 12 + 12) % 12;
		curGateFrac = rescale(clamp(params[GATE_PARAM].getValue(), 0.f, 1.f), 0.f, 1.f, 0.05f, 0.95f);

		// A step's gate was toggled on-screen — recompute now so it takes effect immediately
		// rather than at the next cycle. Every decision is seeded, so this is deterministic:
		// the melody is unchanged, only the gate-derived rest/tie/pitch-chain fields update.
		if (recomputePending) { recomputePending = false; computeCycle(); }

		// Refresh each pattern's micro-waveform (throttled — cheap, 8×8 nodes).
		if (shapeDiv-- <= 0) {
			shapeDiv = 512;
			for (int p = 0; p < NUM_NODES; p++) previewLen = buildPlayedPreview(p, patShape[p]);
		}

		if (randTrigBtn.process(params[RANDOMIZE_PARAM].getValue())
		    || randTrigIn.process(inputs[RANDOMIZE_INPUT].getVoltage())) {
			patternSeed[playPat] = random::u32(); computeCycle();   // reseed the playing pattern
		}
		if (resetTrigBtn.process(params[RESET_PARAM].getValue())
		    || resetTrigIn.process(inputs[RESET_INPUT].getVoltage())) {
			curStep = 0; started = false; nodeClocksRemaining = 1;
			playPat = firstActivePattern(); curRepeat = 1;
			ratchetLeft = 0; gateOpen = false;
		}

		// External clock only — Chance advances a step per rising edge of CLOCK.
		bool clk = false;
		if (inputs[CLOCK_INPUT].isConnected()) {
			samplesSinceClock++;
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage())) {
				clk = true;
				float per = samplesSinceClock * dt;
				if (per > 0.0005f && per < 10.f) clockPeriod = per;
				samplesSinceClock = 0;
			}
		}
		dispRunning = started;

		if (clk) onClock();
		if (gateOpen) { gateTimer -= dt; if (gateTimer <= 0.f) gateOpen = false; }
		// Ratchet: retrigger the same note at each sub-slot until the burst is spent.
		if (ratchetLeft > 0) {
			ratchetNext -= dt;
			if (ratchetNext <= 0.f) {
				gateOpen = true; gateTimer = ratchetGate;
				ratchetNext += ratchetSlot; ratchetLeft--;
			}
		}

		float glide = clamp(params[GLIDE_PARAM].getValue(), 0.f, 1.f);
		if (glide < 1e-4f) { currentCV = targetCV; harmonyCV = targetHarmony; }
		else {
			float tau = glide * glide * 0.5f;
			// exp(-x) = pow2(-x * log2(e))
			static constexpr float LOG2_E = 1.4426950f;
			float c = 1.f - sfs_lut::pow2(-dt / std::max(tau, 1e-4f) * LOG2_E);
			currentCV += (targetCV - currentCV) * c;
			harmonyCV += (targetHarmony - harmonyCV) * c;
		}

		outputs[CV_OUTPUT].setVoltage(currentCV);
		outputs[HARMONY_OUTPUT].setVoltage(harmonyCV);
		outputs[GATE_OUTPUT].setVoltage(gateOpen ? 10.f : 0.f);
		outputs[HARM_GATE_OUTPUT].setVoltage((harmonyOn && gateOpen) ? 10.f : 0.f);

		for (int i = 0; i < NUM_NODES; i++) {
			float b = (i == curNodeIdx && started) ? (gateOpen ? 1.f : 0.4f) : 0.f;
			lights[NODE_LIGHT + i].setBrightnessSmooth(b, dt);
			// Pattern LED: bright on the playing slot, dim on other active slots, off if inactive
			float p = !patActive(i) ? 0.f : (i == playPat && started) ? 1.f : 0.35f;
			lights[PATTERN_LIGHT + i].setBrightnessSmooth(p, dt);
		}
		// While stopped, refresh the preview periodically so knob edits show live
		// (cheap — 8 nodes; deterministic so the preview is stable between edits).
		if (!started) { if (previewDiv-- <= 0) { previewDiv = 256; computeCycle(); } }
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* seeds = json_array();
		for (int i = 0; i < NUM_NODES; i++) json_array_append_new(seeds, json_integer((json_int_t)patternSeed[i]));
		json_object_set_new(root, "patternSeeds", seeds);
		json_t* reseed = json_array();
		for (int i = 0; i < NUM_NODES; i++) json_array_append_new(reseed, json_boolean(patternReseed[i]));
		json_object_set_new(root, "patternReseed", reseed);
		json_t* gates = json_array();
		for (int p = 0; p < NUM_NODES; p++)
			for (int s = 0; s < NUM_NODES; s++) json_array_append_new(gates, json_integer(patternGate[p][s]));
		json_object_set_new(root, "patternGate", gates);
		json_object_set_new(root, "playPat", json_integer(playPat));
		json_object_set_new(root, "editPat", json_integer(editPat));
		json_object_set_new(root, "rangeOctaves", json_integer(rangeOctaves));
		json_object_set_new(root, "startOnRoot", json_boolean(startOnRoot));
		json_object_set_new(root, "perRunVary", json_boolean(perRunVary));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* a = json_object_get(root, "patternSeeds"))
			for (int i = 0; i < NUM_NODES; i++) if (json_t* v = json_array_get(a, i)) patternSeed[i] = (uint32_t)json_integer_value(v);
		if (json_t* a = json_object_get(root, "patternReseed"))
			for (int i = 0; i < NUM_NODES; i++) if (json_t* v = json_array_get(a, i)) patternReseed[i] = json_boolean_value(v);
		if (json_t* a = json_object_get(root, "patternGate"))
			for (int p = 0; p < NUM_NODES; p++)
				for (int s = 0; s < NUM_NODES; s++)
					if (json_t* v = json_array_get(a, p * NUM_NODES + s)) patternGate[p][s] = clamp((int)json_integer_value(v), 0, 2);
		if (json_t* j = json_object_get(root, "editPat")) editPat = clamp((int)json_integer_value(j), 0, NUM_NODES - 1);
		if (json_t* j = json_object_get(root, "playPat")) playPat = clamp((int)json_integer_value(j), 0, NUM_NODES - 1);
		if (json_t* j = json_object_get(root, "rangeOctaves")) rangeOctaves = clamp((int)json_integer_value(j), 1, 2);
		if (json_t* j = json_object_get(root, "startOnRoot")) startOnRoot = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "perRunVary")) perRunVary = json_boolean_value(j);
	}
};

// ─── Display impl ────────────────────────────────────────────────────────────
void ChanceDisplay::drawScene(const DrawArgs& args, const int* core, const int* play, const int* base,
                              const bool* rest, const int* harm, bool harmOn, const int* seq,
                              int seqLen, int curNode, int maxPos, bool running, int root, int scaleIdx) {
	NVGcontext* vg = args.vg;
	const float w = box.size.x, h = box.size.y;
	nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
	nvgFillColor(vg, nvgRGB(0x1a, 0x1a, 0x2e)); nvgFill(vg);
	if (maxPos < 1 || seqLen < 1) return;

	const float top = walkTop(), bot = walkBot(), pad = SH(3);
	Chance* mScale = dynamic_cast<Chance*>(module);
	sfs::BusScale busFallback; sfs::busScaleFromIndex(clamp(scaleIdx, 0, NUM_SCALES - 1), busFallback);
	const sfs::BusScale& scRes = mScale ? mScale->scale : busFallback;
	int szOct = scRes.size; if (szOct < 1) szOct = 1;
	// Auto-fit to the BASE contour (core + base melody + base harmony) — NOT the octave
	// leaps — so the base sequence keeps a stable scale regardless of per-run leaps. `harm`
	// and `play` carry the octave overlay; strip it (play-base) for the harmony's base.
	int lo = base[seq[0]], hi = base[seq[0]];
	for (int k = 0; k < seqLen; k++) {
		int ni = seq[k];
		int hb = harm[ni] - (play[ni] - base[ni]);
		if (core[ni] < lo) lo = core[ni]; if (core[ni] > hi) hi = core[ni];
		if (base[ni] < lo) lo = base[ni]; if (base[ni] > hi) hi = base[ni];
		if (harmOn) { if (hb < lo) lo = hb; if (hb > hi) hi = hb; }
	}
	if (hi <= lo) hi = lo + 1;
	// Reserve one octave of head/foot room so leap lines (base ± an octave) have space and
	// never squeeze the base contour.
	int mapLo = lo - szOct, mapHi = hi + szOct;
	auto X = [&](int ni) { return colCX(ni); };
	auto Y = [&](int p) { return bot - pad - (bot - top - 2 * pad) * (float)(p - mapLo) / (float)(mapHi - mapLo); };

	// vertical column gridlines
	for (int i = 0; i < NUM_NODES; i++) {
		nvgBeginPath(vg); nvgMoveTo(vg, X(i), top); nvgLineTo(vg, X(i), bot);
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x18)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	}
	// octave axis: a faint line + note-name label at each root octave in the visible range
	if (font) {
		static const char* NN[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
		const sfs::BusScale& sc = scRes;
		int sz = sc.size > 0 ? sc.size : 1;
		nvgFontFaceId(vg, font->handle); nvgFontSize(vg, SH(11));
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		for (int oct = (int)std::floor((float)lo / sz) - 1; oct <= (int)std::ceil((float)hi / sz) + 1; oct++) {
			int pos = oct * sz;
			if (pos < lo || pos > hi) continue;
			int midi = 60 + root
			         + (int)std::lround((float)oct * sc.period + sc.intervals[0]);
			float yy = Y(pos);
			nvgBeginPath(vg); nvgMoveTo(vg, SX(52), yy); nvgLineTo(vg, X(NUM_NODES - 1), yy);
			nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x0e)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			nvgFillColor(vg, nvgRGB(0xcf, 0xcf, 0xe0));
			nvgText(vg, SX(20), yy, (std::string(NN[((midi % 12) + 12) % 12]) + std::to_string(midi / 12 - 1)).c_str(), NULL);
		}
	}

	// harmony (2nd voice) — teal, under the main lines
	if (harmOn) {
		nvgStrokeColor(vg, nvgRGBA(0x3f, 0xc9, 0x9a, 0xe0)); nvgStrokeWidth(vg, 2.f);
		nvgBeginPath(vg);
		for (int k = 0; k < seqLen; k++) { int ni = seq[k]; float x = X(ni), y = Y(harm[ni] - (play[ni] - base[ni])); if (k == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y); }
		nvgStroke(vg);
	}
	// core skeleton (darker blue)
	nvgStrokeColor(vg, nvgRGB(0x0d, 0x59, 0x86)); nvgStrokeWidth(vg, 2.f);
	nvgBeginPath(vg);
	for (int k = 0; k < seqLen; k++) { float x = X(seq[k]), y = Y(core[seq[k]]); if (k == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y); }
	nvgStroke(vg);
	// octave leaps — a thin vertical line from the base contour to the actual (leaped)
	// pitch, drawn into the reserved head/foot room. The base line stays put; the jump is
	// shown as an annotation rather than moving the sequence.
	for (int k = 0; k < seqLen; k++) {
		int ni = seq[k];
		if (play[ni] == base[ni]) continue;
		float x = X(ni), yb = Y(base[ni]), yp = Y(play[ni]);
		nvgBeginPath(vg); nvgMoveTo(vg, x, yb); nvgLineTo(vg, x, yp);
		nvgStrokeColor(vg, nvgRGBA(0x00, 0x97, 0xde, 0x99)); nvgStrokeWidth(vg, 1.3f); nvgStroke(vg);
		nvgBeginPath(vg); nvgCircle(vg, x, yp, SW(2.2));   // small marker at the leaped pitch
		nvgFillColor(vg, nvgRGB(0x00, 0x97, 0xde)); nvgFill(vg);
	}
	// played path (bright blue) — the base contour (octave leaps annotated above)
	nvgStrokeColor(vg, nvgRGB(0x00, 0x97, 0xde)); nvgStrokeWidth(vg, 2.f);
	nvgBeginPath(vg);
	for (int k = 0; k < seqLen; k++) { float x = X(seq[k]), y = Y(base[seq[k]]); if (k == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y); }
	nvgStroke(vg);
	for (int k = 0; k < seqLen; k++) {
		int ni = seq[k];
		bool on = running && (ni == curNode);
		if (rest[ni]) {   // gate off → hollow circle on the base contour
			nvgBeginPath(vg); nvgCircle(vg, X(ni), Y(base[ni]), SW(4.5));
			nvgFillColor(vg, nvgRGB(0x1a, 0x1a, 0x2e)); nvgFill(vg);
			nvgStrokeColor(vg, on ? nvgRGB(0xec, 0x65, 0x2e) : nvgRGB(0x00, 0x97, 0xde)); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);
		} else if (on) {  // current step (gate on) → filled playhead
			nvgBeginPath(vg); nvgCircle(vg, X(ni), Y(base[ni]), SW(5));
			nvgFillColor(vg, nvgRGB(0xec, 0x65, 0x2e)); nvgFill(vg);
		}
	}
}

// Recycle/reseed glyph: two opposing arcs, each with a small tangential arrowhead.
// The "off" colour is solid (not translucent) so the arc/arrowhead overlaps don't
// double up in alpha — it reads as a uniform dim glyph.
void ChanceDisplay::drawReseedIcon(NVGcontext* vg, rack::math::Rect r, bool on) {
	float cx = r.pos.x + r.size.x * 0.5f, cy = r.pos.y + r.size.y * 0.5f;
	float rad = std::min(r.size.x, r.size.y) * 0.30f;
	NVGcolor col = on ? nvgRGB(0xec, 0x65, 0x2e) : nvgRGB(0x55, 0x55, 0x6a);
	nvgStrokeColor(vg, col); nvgStrokeWidth(vg, std::max(1.f, rad * 0.26f)); nvgLineCap(vg, NVG_ROUND);
	const float a0 = 0.5f, a1 = M_PI - 0.35f;   // ~130° arcs, 180° apart
	nvgBeginPath(vg); nvgArc(vg, cx, cy, rad, a0, a1, NVG_CW); nvgStroke(vg);
	nvgBeginPath(vg); nvgArc(vg, cx, cy, rad, a0 + M_PI, a1 + M_PI, NVG_CW); nvgStroke(vg);
	const float ah = rad * 0.55f;               // arrowhead size
	auto arrow = [&](float ang) {
		float ex = cx + rad * std::cos(ang), ey = cy + rad * std::sin(ang);
		float tx = -std::sin(ang), ty = std::cos(ang);   // CW tangent (direction of travel)
		float nx = std::cos(ang), ny = std::sin(ang);    // radial
		nvgBeginPath(vg);
		nvgMoveTo(vg, ex + tx * ah, ey + ty * ah);       // tip
		nvgLineTo(vg, ex + nx * ah * 0.65f, ey + ny * ah * 0.65f);
		nvgLineTo(vg, ex - nx * ah * 0.65f, ey - ny * ah * 0.65f);
		nvgClosePath(vg); nvgFillColor(vg, col); nvgFill(vg);
	};
	arrow(a1); arrow(a1 + M_PI);
}

void ChanceDisplay::drawPreview(const DrawArgs& args) {
	const int core[8] = {1, 3, 2, 4, 3, 5, 4, 6};
	const int play[8] = {1, 3, 5, 4, 3, 2, 4, 6};   // strays at a couple of steps
	const bool rest[8] = {false, false, false, false, true, false, false, false};
	const int harm[8] = {3, 7, 8, 6, 5, 4, 6, 8};   // a weaving 2nd voice
	const int seq[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	drawScene(args, core, play, play, rest, harm, true, seq, 8, 2, 8, true, 0, 1);
	sfs::BusScale prev; sfs::busScaleFromIndex(1, prev);
	drawKeyReadout(args.vg, 0, prev);
}

// Top-left key/scale readout: note name (white) + the scale (orange).
//
// The scale is the one ACTUALLY in force, from the bus, not the canonical name
// of the channel-0 index. Once the key comes off the wire that index is only
// ever a nearest neighbour, so reading its name here announced "Chromatic" over
// a Bohlen-Pierce scale -- confidently, in the one place a player looks to see
// what key they are in.
void ChanceDisplay::drawKeyReadout(NVGcontext* vg, int root, const sfs::BusScale& sc) {
	if (!font) return;
	static const char* NN[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
	std::string note = NN[((root % 12) + 12) % 12];
	std::string scale = sc.canonical()
		? std::string(sc.longName())
		: (sc.truncated() ? string::f("%d of %d deg", sc.size, sc.size + sc.dropped)
		                  : string::f("%d deg", sc.size));
	// A period that is not an octave is the other thing an index cannot say,
	// and it changes how every row above the first reads.
	if (!sc.canonical() && !sc.octave()) scale += string::f("  %.2f st", sc.period);
	nvgFontFaceId(vg, font->handle); nvgFontSize(vg, SH(12));
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	float y = SY(9);   // sit in the top band, clear of the divider (16.5) + pattern rects (19)
	nvgFillColor(vg, nvgRGB(0xe6, 0xe6, 0xf0));
	float adv = nvgText(vg, SX(18), y, note.c_str(), NULL);
	// Red rather than orange when the wire could not carry the whole scale.
	nvgFillColor(vg, sc.truncated() ? nvgRGB(0xe0, 0x3a, 0x2a) : nvgRGB(0xec, 0x65, 0x2e));
	nvgText(vg, adv + SW(6), y, scale.c_str(), NULL);
}

void ChanceDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	if (!module) { drawPreview(args); drawBank(args); drawControls(args); return; }
	Chance* m = dynamic_cast<Chance*>(module);
	if (!m) { drawPreview(args); drawBank(args); drawControls(args); return; }
	drawScene(args, m->coreNote, m->playNote, m->baseNote, m->pathRest, m->harmNote, m->harmonyEnabled(),
	          m->seq, m->seqLen, m->curNodeIdx, m->maxPosDeg, m->dispRunning, m->curRoot, m->curScale);
	drawKeyReadout(args.vg, m->curRoot, m->scale);
	drawBank(args);
	drawControls(args);
}

// On-screen pattern bank: per cell a micro-waveform, a mode chip (normal ↔ reseed),
// and a row of 8 repeat boxes (click box N = N repeats).
void ChanceDisplay::drawBank(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	static const int DEMO[NUM_NODES][NUM_NODES] = {
		{0,1,2,1,3,2,4,3}, {0,2,1,3,2,4,3,5}, {0,1,0,2,1,3,2,4}, {0,3,1,4,2,5,3,6},
		{0,1,2,3,4,3,2,1}, {0,2,4,2,0,2,4,2}, {0,1,3,2,4,3,5,4}, {0,4,3,2,1,2,3,4},
	};
	bool active[NUM_NODES]; int reps[NUM_NODES]; bool reseed[NUM_NODES]; const int* shape[NUM_NODES];
	int playPat = 0, curRep = 1; bool running = false;
	if (module) {
		for (int i = 0; i < NUM_NODES; i++) {
			active[i] = module->patActive(i); reps[i] = module->patRepeats(i);
			reseed[i] = module->patternReseed[i]; shape[i] = module->patShape[i];
		}
		playPat = module->playPat; curRep = module->curRepeat; running = module->dispRunning;
	} else {
		for (int i = 0; i < NUM_NODES; i++) { active[i] = (i < 3); reps[i] = (i == 2 ? 3 : 1); reseed[i] = (i == 1); shape[i] = DEMO[i]; }
		running = true;
	}

	int editPat = module ? module->editPat : 0;
	// top divider (under the key readout)
	nvgBeginPath(vg); nvgMoveTo(vg, SX(18), SY(16.5)); nvgLineTo(vg, SX(408), SY(16.5));
	nvgStrokeColor(vg, nvgRGB(0x0d, 0x59, 0x88)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

	for (int i = 0; i < NUM_NODES; i++) {
		rack::math::Rect r = patCellRect(i);
		bool play = (i == playPat) && running;
		bool focus = (i == editPat);
		NVGcolor bg = focus ? nvgRGB(0x00, 0x97, 0xde) : active[i] ? nvgRGB(0x35, 0x35, 0x4d) : nvgRGBA(0x35, 0x35, 0x4d, 0x33);
		nvgBeginPath(vg); nvgRoundedRect(vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 2.f);
		nvgFillColor(vg, bg); nvgFill(vg);
		if (play) {   // orange border = currently playing
			nvgBeginPath(vg); nvgRoundedRect(vg, r.pos.x + 0.6f, r.pos.y + 0.6f, r.size.x - 1.2f, r.size.y - 1.2f, 2.f);
			nvgStrokeColor(vg, nvgRGB(0xec, 0x65, 0x2e)); nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
		}
		// header: pattern number "P#" (top-left)
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, SH(12));
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, focus ? nvgRGB(0xff, 0xff, 0xff) : active[i] ? nvgRGB(0xe6, 0xe6, 0xf0) : nvgRGBA(0xe6, 0xe6, 0xf0, 0x55));
			nvgText(vg, r.pos.x + SW(4), r.pos.y + SH(3), string::f("P%d", i + 1).c_str(), NULL);
		}
		// reseed recycle icon (top-right)
		drawReseedIcon(vg, patModeRect(i), reseed[i]);
		// micro-waveform: this pattern's actual played contour (white when focused)
		{
			rack::math::Rect wr = patWaveRect(i);
			const int* sh = shape[i];
			int n = module ? module->previewLen : NUM_NODES; if (n < 2) n = 2; if (n > NUM_NODES) n = NUM_NODES;
			int lo = sh[0], hi = sh[0];
			for (int k = 1; k < n; k++) { if (sh[k] < lo) lo = sh[k]; if (sh[k] > hi) hi = sh[k]; }
			if (hi <= lo) hi = lo + 1;
			nvgBeginPath(vg);
			for (int k = 0; k < n; k++) {
				float x = wr.pos.x + wr.size.x * (float)k / (float)(n - 1);
				float y = wr.pos.y + wr.size.y - wr.size.y * (float)(sh[k] - lo) / (float)(hi - lo);
				if (k == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
			}
			nvgStrokeColor(vg, focus ? nvgRGB(0xff, 0xff, 0xff) : active[i] ? nvgRGB(0x00, 0x97, 0xde) : nvgRGBA(0x00, 0x97, 0xde, 0x55));
			nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
		}
		// repeat boxes (1..8) directly below the rect; current repeat lit orange on the playing slot
		{
			rack::math::Rect rr = patRepRect(i);
			float bw = rr.size.x / 8.f;
			for (int d = 0; d < 8; d++) {
				bool on = d < reps[i], cur = play && (d < curRep);
				NVGcolor c = cur ? nvgRGB(0xec, 0x65, 0x2e)
				           : on  ? (active[i] ? nvgRGB(0x00, 0x97, 0xde) : nvgRGBA(0x00, 0x97, 0xde, 0x55))
				                 : nvgRGB(0x35, 0x35, 0x4d);
				nvgBeginPath(vg); nvgRoundedRect(vg, rr.pos.x + d * bw + 0.5f, rr.pos.y, bw - 1.f, rr.size.y, 1.f);
				nvgFillColor(vg, c); nvgFill(vg);
			}
		}
	}

	// Inverted-T tab connector: a rail under the bank + a stem down to the focused pattern.
	nvgLineCap(vg, NVG_BUTT);
	nvgBeginPath(vg); nvgMoveTo(vg, SX(18), SY(58)); nvgLineTo(vg, SX(408), SY(58));
	nvgStrokeColor(vg, nvgRGB(0x0d, 0x59, 0x88)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	// stem: centred on the focused cell, starting below the repeat row, ending exactly at the rail
	rack::math::Rect fc = patCellRect(editPat);
	float stemX = fc.pos.x + fc.size.x * 0.5f;
	nvgBeginPath(vg); nvgMoveTo(vg, stemX, SY(55.5f)); nvgLineTo(vg, stemX, SY(58));
	nvgStrokeColor(vg, nvgRGB(0x0d, 0x59, 0x88)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
}

// ─── Controls band: per-step gate row (G/D/B sliders moved to panel trimpots) ─
void ChanceDisplay::drawControls(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	int gm[NUM_NODES]; int ep = 0;
	if (module) {
		ep = module->editPat;
		for (int i = 0; i < NUM_NODES; i++) gm[i] = module->gateMode(ep, i);
	} else {
		int demo[NUM_NODES] = {1,1,2,1,0,1,2,1};
		for (int i = 0; i < NUM_NODES; i++) gm[i] = demo[i];
	}
	// ── per-step gate row S1-S8 (focused pattern): off = dim, gate = blue, tie = blue + a
	//    blue connector bar bridging to the previous step (matches the screen SVG). ──
	for (int i = 0; i < NUM_NODES; i++) {
		rack::math::Rect g = gateRect(i);
		nvgBeginPath(vg); nvgRoundedRect(vg, g.pos.x, g.pos.y, g.size.x, g.size.y, 2.f);
		nvgFillColor(vg, gm[i] == 0 ? nvgRGB(0x35, 0x35, 0x4d) : nvgRGB(0x00, 0x97, 0xde)); nvgFill(vg);
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, SH(11));
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, gm[i] == 0 ? nvgRGB(0x8a, 0x8a, 0xaa) : nvgRGB(0xff, 0xff, 0xff));
			nvgText(vg, g.pos.x + SW(4), g.pos.y + g.size.y * 0.5f, string::f("S%d", i + 1).c_str(), NULL);
		}
	}
	// tie connectors on top: a blue bar filling the gap between a tie step and the previous one
	for (int i = 1; i < NUM_NODES; i++) {
		if (gm[i] != 2) continue;
		rack::math::Rect g = gateRect(i), gp = gateRect(i - 1);
		float x0 = gp.pos.x + gp.size.x, cyb = g.pos.y + g.size.y * 0.5f;
		nvgBeginPath(vg); nvgRect(vg, x0, cyb - SH(4), g.pos.x - x0, SH(8));
		nvgFillColor(vg, nvgRGB(0x00, 0x97, 0xde)); nvgFill(vg);
	}
}

void ChanceDisplay::setRepFromX(int i, float x) {
	rack::math::Rect r = patRepRect(i);
	int idx = clamp((int)std::floor((x - r.pos.x) / (r.size.x / 8.f)), 0, 7);
	module->params[Chance::PATREP_PARAM + i].setValue((float)(idx + 1));
}
// Click: mode chip → reseed; repeat row → repeats; cell body → focus for editing (dbl-click
// toggles active); gate box → enable/disable (shift-click ties) for the focused pattern.
void ChanceDisplay::onButton(const ButtonEvent& e) {
	if (module && e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
		dragPos = e.pos;   // remembered for onDoubleClick (which has no position)
		for (int i = 0; i < NUM_NODES; i++) {
			if (patModeRect(i).contains(e.pos)) {
				module->patternReseed[i] = !module->patternReseed[i]; e.consume(this); return;
			}
			if (patRepRect(i).contains(e.pos)) {
				dragRepCell = i; dragMouseX = e.pos.x; setRepFromX(i, e.pos.x); e.consume(this); return;
			}
			if (patCellRect(i).contains(e.pos)) {
				module->editPat = i; e.consume(this); return;   // focus this pattern for gate editing
			}
		}
		for (int i = 0; i < NUM_NODES; i++) {
			if (gateRect(i).contains(e.pos)) {
				int& g = module->patternGate[module->editPat][i];
				if (e.mods & GLFW_MOD_SHIFT) g = (g == 2) ? 1 : 2;   // shift-click: tie / untie
				else                         g = (g == 0) ? 1 : 0;   // click: enable / disable
				module->recomputePending = true;   // apply now, not at the next cycle
				e.consume(this); return;
			}
		}
	}
	OpaqueWidget::onButton(e);
}
void ChanceDisplay::onDoubleClick(const DoubleClickEvent& e) {
	if (module) {
		int i = hitPatCell(dragPos);   // DoubleClickEvent carries no position; reuse last press
		if (i >= 0) {
			auto& p = module->params[Chance::PATTERN_PARAM + i];
			p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);   // toggle active
			e.consume(this); return;
		}
	}
	OpaqueWidget::onDoubleClick(e);
}
void ChanceDisplay::onDragMove(const DragMoveEvent& e) {
	if (module && dragRepCell >= 0) { dragMouseX += e.mouseDelta.x; setRepFromX(dragRepCell, dragMouseX); e.consume(this); return; }
	OpaqueWidget::onDragMove(e);
}
void ChanceDisplay::onDragEnd(const DragEndEvent& e) {
	dragRepCell = -1;
	OpaqueWidget::onDragEnd(e);
}
void ChanceDisplay::onHoverScroll(const HoverScrollEvent& e) {
	if (module) {
		int i = hitPatCell(e.pos);
		if (i >= 0) {   // scroll over a cell = adjust its repeat count too
			auto& p = module->params[Chance::PATREP_PARAM + i];
			p.setValue(clamp(p.getValue() + (e.scrollDelta.y > 0.f ? 1.f : -1.f), 1.f, 8.f));
			e.consume(this); return;
		}
	}
	OpaqueWidget::onHoverScroll(e);
}

// ─── Widget ──────────────────────────────────────────────────────────────────
// Menu slider over a module param — for controls that earn their keep but don't
// earn panel space. `quantity` is borrowed, not owned (see ui/Slider.hpp).
struct ChanceParamSlider : ui::Slider {
	ChanceParamSlider(Quantity* q) { quantity = q; box.size.x = 200.f; }
};

struct ChanceWidget : ModuleWidget {
	ChanceWidget(Chance* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/chance.svg")));
		// No virtual screws — see CLAUDE.md (SFS panels omit them by design).

		ChanceDisplay* disp = new ChanceDisplay();
		disp->module = module;
		// On-screen screen: key readout · pattern bank P1-8 (thin rects + reseed icon + repeat
		// boxes) · inverted-T tab rail · step-gate row S1-8 · walk (C-axis). Shortened
		// 2026-07-13 to match Stuart's art (screen now 122 × 54.3mm).
		disp->box.pos = mm2px(Vec(CHANCE_DISP_X0, 12.f));
		disp->box.size = mm2px(Vec(CHANCE_DISP_W, 54.3f));
		addChild(disp);

		// Panel controls laid out to Stuart's res/chance.svg — 9 columns at 13.86mm pitch,
		// three control-row Y bands below the screen.
		static const float CX[9] = {10.16f, 24.02f, 37.89f, 51.75f, 65.62f, 79.48f, 93.34f, 107.21f, 121.07f};

		// Row A (y=76.31) — trimpots: START END | GRAV DRIFT BRANCH ODD-REST EVEN-REST LEAP RATCHET
		const int rowA[9] = {Chance::START_PARAM, Chance::END_PARAM, Chance::GRAVITY_PARAM,
			Chance::DRIFT_PARAM, Chance::BRANCH_PARAM, Chance::RESTS_PARAM, Chance::EVEN_REST_PARAM,
			Chance::OCTAVE_PARAM, Chance::REPEAT_PARAM};
		for (int i = 0; i < 9; i++) addParam(createParamCentered<Trimpot>(mm2px(Vec(CX[i], 76.31f)), module, rowA[i]));

		// Row B (y=90) — one CV input per row-A trimpot, sharing its column label.
		// (GATE LEN + GLIDE used to sit in cols 0-1; they moved to the context menu
		// so START/END could have CV like every other sequence control.)
		const int rowBcv[9] = {Chance::START_CV_INPUT, Chance::END_CV_INPUT,
			Chance::GRAVITY_CV_INPUT, Chance::DRIFT_CV_INPUT, Chance::BRANCH_CV_INPUT,
			Chance::RESTS_CV_INPUT, Chance::EVEN_REST_CV_INPUT, Chance::OCT_CV_INPUT, Chance::REPEAT_CV_INPUT};
		for (int i = 0; i < 9; i++) addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CX[i], 90.f)), module, rowBcv[i]));

		// Row C (y=106.67) — KEY ROOT trims · RND RST buttons · CV GATE outs (dark inset)
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CX[1], 106.67f)), module, Chance::KEY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CX[2], 106.67f)), module, Chance::ROOT_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(CX[3], 106.67f)), module, Chance::RANDOMIZE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(CX[4], 106.67f)), module, Chance::RESET_PARAM));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CX[7], 106.67f)), module, Chance::CV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CX[8], 106.67f)), module, Chance::GATE_OUTPUT));

		// Row D (y=121.91) — CLK SCALEcv ROOTcv RNDg RSTg · HARMONY trim · H CV H GATE outs
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CX[0], 121.91f)), module, Chance::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CX[1], 121.91f)), module, Chance::SCALE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CX[2], 121.91f)), module, Chance::ROOT_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CX[3], 121.91f)), module, Chance::RANDOMIZE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CX[4], 121.91f)), module, Chance::RESET_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CX[6], 121.91f)), module, Chance::HARMONY_PARAM));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CX[7], 121.91f)), module, Chance::HARMONY_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CX[8], 121.91f)), module, Chance::HARM_GATE_OUTPUT));
		// (EOC output removed 2026-07-13 — the panel has 4 output holes: CV/GATE/H CV/H GATE.)
	}

	void appendContextMenu(Menu* menu) override {
		Chance* m = dynamic_cast<Chance*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Pattern cell: click = edit · dbl-click = on/off · boxes = repeats · R = reseed"));
		menu->addChild(createMenuLabel("Gate row (focused pattern): click = on/off · shift-click = tie"));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Gate length"));
		menu->addChild(new ChanceParamSlider(m->getParamQuantity(Chance::GATE_PARAM)));
		menu->addChild(createMenuLabel("Glide"));
		menu->addChild(new ChanceParamSlider(m->getParamQuantity(Chance::GLIDE_PARAM)));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Start each cycle on root", "", &m->startOnRoot));
		menu->addChild(createBoolPtrMenuItem("Vary shaping each run (rests/holds/octaves/ratchets)", "", &m->perRunVary));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Walk range"));
		menu->addChild(createCheckMenuItem("±1 octave", "",
			[m]() { return m->rangeOctaves == 1; }, [m]() { m->rangeOctaves = 1; }));
		menu->addChild(createCheckMenuItem("±2 octaves", "",
			[m]() { return m->rangeOctaves == 2; }, [m]() { m->rangeOctaves = 2; }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("New core for current pattern", "",
			[m]() { m->patternSeed[m->playPat] = random::u32(); m->computeCycle(); }));
		menu->addChild(createMenuItem("New cores for all patterns", "",
			[m]() { for (int i = 0; i < NUM_NODES; i++) m->patternSeed[i] = random::u32(); m->computeCycle(); }));
	}
};

Model* modelChance = createModel<Chance, ChanceWidget>("Chance");
