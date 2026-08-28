#include "plugin.hpp"
#include "scale-bus.hpp"
#include "scales.hpp"
#include <cmath>


// ─── Triadex Muse: faithful recreation ───────────────────────────────────────
//
// Original by Edward Fredkin and Marvin Minsky, 1972 (US Pat. 3,610,801).
// Eight sliders — 4 THEME and 4 INTERVAL — each selecting one of 40 "taps":
//
//   idx  label   what
//   0    OFF     constant 0
//   1    ON      constant 1
//   2    C 1/2   raw clock pulse (toggles every clock)
//   3    C1      \
//   4    C2      |  binary counter bits (periods 2, 4, 8, 16 clocks)
//   5    C4      |
//   6    C8      /
//   7    C3      \  mod-12 counter taps (periods 6 and 12 clocks)
//   8    C6      /  — adds triplet/ternary rhythms alongside the binary taps
//   9..  B1..B31 31-bit shift register, B1 = newest bit
//   39   B31
//
// The four THEME slider bits are XNOR'd together to form the next bit shifted
// into B1 on each clock pulse. (XNOR, not XOR — makes all-zeros a fixed point.)
//
// The four INTERVAL slider bits form a 4-bit value:
//   bits 0..2 (A,B,C) = index into a 7-note diatonic scale (or other)
//   bit 3     (D)     = octave-up bit
// Output = root + scale[idx] + 12*octave, emitted as 1V/oct.
//
// SFS-specific additions:
//   - external clock only (no internal tempo)
//   - small scale selector (Major faithful default, plus a few alternates)
//   - Random button + Random CV in: re-rolls all 8 slider positions
//   - till.com-style label column + live LED state to the right of the sliders


static const int N_TAPS = 40;
static const int SR_BITS = 31;

// Tap index constants
enum TapIdx {
	TAP_OFF = 0,
	TAP_ON,
	TAP_C_HALF,
	TAP_C1, TAP_C2, TAP_C4, TAP_C8,   // binary counter
	TAP_C3, TAP_C6,                    // mod-12 counter
	TAP_B1                              // = 9; B1..B31 = 9..39
};

static const char* TAP_LABELS[N_TAPS] = {
	"OFF", "ON", "C \xc2\xbd",  // "C ½"
	"C1", "C2", "C4", "C8",
	"C3", "C6",
	"B1",  "B2",  "B3",  "B4",  "B5",  "B6",  "B7",  "B8",
	"B9",  "B10", "B11", "B12", "B13", "B14", "B15", "B16",
	"B17", "B18", "B19", "B20", "B21", "B22", "B23", "B24",
	"B25", "B26", "B27", "B28", "B29", "B30", "B31"
};


// ─── Scale table ─────────────────────────────────────────────────────────────
// Scales come from the shared canonical list (src/scales.hpp) so SCALE CV
// values are interchangeable across Note, Fugue, and Muse. Muse's 3-bit pitch
// index reads the flattened 8-slot museSemis[] table (bit D adds +12 on top);
// it shows museName (== longName except index 0 = "Chromatic-ish").
using MuseScale = sfs::Scale;
static const sfs::Scale* const MUSE_SCALES = sfs::SCALES;
static const int NUM_MUSE_SCALES = sfs::NUM_SCALES;


// ─── Core DSP state (separated for clarity / testability) ────────────────────

struct MuseCore {
	uint8_t  binCounter  = 0;   // 4-bit: C1, C2, C4, C8 = bits 0..3
	uint8_t  mod12Counter = 0;  // 0..11: C3 = (n%6)>=3, C6 = n>=6
	uint32_t sr          = 0;   // 31-bit SR, bit 0 = B1 (newest)
	bool     clockHigh   = false;  // for C 1/2

	int theme[4]    = {0, 0, 0, 0};
	int interval[4] = {0, 0, 0, 0};

	bool tap(int n) const {
		switch (n) {
			case TAP_OFF:    return false;
			case TAP_ON:     return true;
			case TAP_C_HALF: return clockHigh;
			case TAP_C1:     return (binCounter >> 0) & 1;
			case TAP_C2:     return (binCounter >> 1) & 1;
			case TAP_C4:     return (binCounter >> 2) & 1;
			case TAP_C8:     return (binCounter >> 3) & 1;
			case TAP_C3:     return (mod12Counter % 6) >= 3;
			case TAP_C6:     return mod12Counter >= 6;
			default: {
				int b = n - TAP_B1;  // 0..30
				if (b < 0 || b >= SR_BITS) return false;
				return (sr >> b) & 1;
			}
		}
	}

	void clockTick() {
		// XNOR of the four theme taps -> next B1
		int s = 0;
		for (int i = 0; i < 4; i++) s += tap(theme[i]) ? 1 : 0;
		bool fb = !(s & 1);   // XNOR = NOT(XOR)

		// Shift register: shift left, new bit into B1 (= bit 0)
		uint32_t mask = (1u << SR_BITS) - 1u;
		sr = ((sr << 1) | (fb ? 1u : 0u)) & mask;

		// Counters
		binCounter   = (binCounter + 1) & 0x0F;
		mod12Counter = (mod12Counter + 1) % 12;
		clockHigh    = !clockHigh;
	}

	void clearState() {
		binCounter   = 0;
		mod12Counter = 0;
		sr           = 0;
		clockHigh    = false;
	}

	// 4-bit pitch address. Bits 0..2 (interval A,B,C) -> scale index; bit 3 (D)
	// -> octave-up flag.
	int pitchAddr() const {
		return (tap(interval[0]) ? 1 : 0)
		     | (tap(interval[1]) ? 2 : 0)
		     | (tap(interval[2]) ? 4 : 0)
		     | (tap(interval[3]) ? 8 : 0);
	}
};


// ─── Triadex Muse manual presets ────────────────────────────────────────────
// Slider settings from the original Triadex Muse owner's manual (1972), as
// transcribed via dbRackSequencer's TME presets. Each preset sets all 8
// sliders to a specific tap index 0..39. Theme is XNOR-symmetric so the
// per-position order doesn't affect the sonic result; we map left-to-right
// (T1..T4 ← TME W..Z) for visual consistency.

struct MusePreset {
	const char* name;
	uint8_t interval[4];  // A, B, C, D (TME ids 0..3)
	uint8_t theme[4];     // T1..T4    (TME ids 4..7)
};

static const MusePreset MUSE_PRESETS[] = {
	{"240-Note Pattern",  { 9, 10,  5,  6}, { 0,  0, 11, 12}},
	{"Al's Surprise",     { 9, 13, 15,  2}, { 6,  9, 15, 19}},
	{"Birds",             { 9, 10, 11,  5}, {38, 39, 39, 39}},
	{"Christmas Bells",   {39, 38, 37, 36}, {36, 37, 38, 39}},
	{"Dorian Muse",       { 0,  9, 11,  6}, { 9, 24,  0,  0}},
	{"Federal Row",       {22, 13, 20, 10}, {29, 32,  4,  0}},
	{"Flat Baroque",      { 3, 23,  9,  2}, {38, 37, 32,  0}},
	{"Marvin's Yodel",    { 9, 25, 17, 33}, {24,  0, 23,  3}},
	{"Meditation",        { 9, 39, 22,  0}, { 0,  0, 24, 39}},
	{"Mesopotamia",       { 4, 13, 17,  0}, { 6, 17, 32,  5}},
	{"Michael's Tune",    {15, 16, 13,  0}, { 0, 12, 31,  0}},
	{"Muser's Waltz",     {18, 16, 15,  0}, { 1,  5,  9, 10}},
	{"Polka",             { 9, 21, 19,  2}, { 6, 19, 15,  9}},
	{"Rhyming Couplets",  { 9, 10,  5,  6}, { 0,  0, 39,  5}},
	{"Ron's Rhapsody",    {14, 17, 14,  2}, {39,  5,  0,  6}},
	{"Swiss Yodeler",     {16,  3, 24,  0}, {30, 29, 24,  0}},
	{"The Crazy Cuckoo",  { 3,  9, 39,  6}, { 0,  0,  9, 39}},
};
static const int NUM_MUSE_PRESETS = sizeof(MUSE_PRESETS) / sizeof(MUSE_PRESETS[0]);


struct Muse;


// ─── Custom ParamQuantity: ROOT shown as a note name (C4, C#4, …) ───────────
// Value is offset semitones from C4 (V/oct convention: 0V = C4). Range is
// −24…+24 so the tooltip says e.g. "Root: F3" rather than "Root: -7 semitones".

struct RootParamQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int semis = (int)std::round(getValue());
		int midi  = 60 + semis;   // C4 = MIDI 60
		static const char* names[12] = {
			"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
		};
		int n   = ((midi % 12) + 12) % 12;
		int oct = (midi / 12) - 1;
		return string::f("%s%d", names[n], oct);
	}
};


// ─── Expander link message ──────────────────────────────────────────────────
// Master Muse writes its live state into its rightExpander producerMessage on
// every sample; the adjacent right-neighbor Muse reads it as a consumer and
// (when in slave mode) copies the state into its own core BEFORE reading
// pitchAddr, so it always plays from the master's bit stream. Slaves also
// forward the master's state through to THEIR right neighbor, so a daisy
// chain Master → Slave1 → Slave2 → … works.

struct MuseLinkMessage {
	bool     valid = false;            // true if upstream actually authored this
	uint32_t sr = 0;                   // master's 31-bit shift register
	uint8_t  binCounter = 0;           // 4-bit counter (C1..C8)
	uint8_t  mod12Counter = 0;         // mod-12 counter (C3, C6)
	bool     clockHigh = false;        // C 1/2 value at master
	bool     clockTickedThisFrame = false;  // fires on the sample the master ticked
	uint8_t  fbBit = 0;                // master's last theme XNOR feedback bit
	uint16_t hopsFromMaster = 0;       // 0 = direct master, 1+ = chained slave
};


// ─── Display: per-slider columns + live LED state + calculated note ──────────

struct MuseDisplay : Widget {
	Muse* module = nullptr;
	std::shared_ptr<Font> font;

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawPreview(const DrawArgs& args);    // module==NULL fallback
	void draw(const DrawArgs& args) override { Widget::draw(args); }
};


// ─── Scope: scrolling pitch history (stacked traces) ────────────────────────

struct MuseScope : Widget {
	Muse* module = nullptr;
	std::shared_ptr<Font> font;

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawPreview(const DrawArgs& args);    // module==NULL fallback
	void draw(const DrawArgs& args) override { Widget::draw(args); }
};


// ─── Module ──────────────────────────────────────────────────────────────────

struct Muse : Module {
	enum ParamId {
		THEME_PARAM_0, THEME_PARAM_1, THEME_PARAM_2, THEME_PARAM_3,
		INTERVAL_PARAM_0, INTERVAL_PARAM_1, INTERVAL_PARAM_2, INTERVAL_PARAM_3,
		ROOT_PARAM,
		RUN_PARAM,
		RESET_PARAM,
		RAND_PARAM,
		SCALE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		RUN_INPUT,
		RAND_INPUT,
		// Per-slider CV (added after the original 4 inputs so existing patches
		// keep their cables on the right jacks). ±5V mapped to ±20 taps.
		INTERVAL_CV_0, INTERVAL_CV_1, INTERVAL_CV_2, INTERVAL_CV_3,
		THEME_CV_0,    THEME_CV_1,    THEME_CV_2,    THEME_CV_3,
		ROOT_CV_INPUT,
		SCALE_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VOCT_OUTPUT,
		GATE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		RUN_LIGHT,
		LIGHTS_LEN
	};

	MuseCore core;

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger resetButtonTrigger;
	dsp::SchmittTrigger randTrigger;
	dsp::SchmittTrigger randButtonTrigger;
	dsp::PulseGenerator gatePulse;

	// 50%-duty gate: the gate stays high for half the measured clock interval,
	// so it actually sustains a synth voice (instead of a 1ms trigger).
	int  samplesSinceTick = 0;
	int  tickInterval = 0;
	bool haveTickInterval = false;
	int  gateRemaining = 0;

	bool running = true;
	bool gateOnChangeOnly = false;
	int  lastPitchAddr = -1;

	// --- Expander linking ---
	bool linkingEnabled = true;        // user can disable in context menu
	bool followingThisFrame = false;      // true if we read a valid master message this frame
	int  hopsFromMasterCached = 0;     // for display
	uint8_t lastMasterFb = 0;          // master's last fb bit (for scope when slaved)

	// --- V/OCT output scaling ---
	// 0 = standard 1V/oct (pitch use). 1/2/3 = scaled modulation modes that
	// map pitchAddr 0..15 linearly to 0..1V / 0..2V / 0..5V; in these modes
	// SCALE and ROOT do not affect the output (it's a modulation source, not
	// a pitch CV).
	enum CvScaleMode {
		CV_SCALE_VOCT = 0,
		CV_SCALE_1V   = 1,
		CV_SCALE_2V   = 2,
		CV_SCALE_5V   = 3,
	};
	int cvScaleMode = CV_SCALE_VOCT;

	// Random scope: 0 = both, 1 = theme only, 2 = interval only
	int  randomScope = 0;

	// Cached "last clock pulse fired" indicator for display
	float clockFlash = 0.f;

	// Scope ring buffer — last N clock-tick pitch addresses and feedback bits.
	// scopeHead points at the next slot to write; ordering is "oldest" at
	// (scopeHead) modulo length.
	static const int SCOPE_LEN = 128;
	uint8_t scopePitch[SCOPE_LEN] = {0};   // 0..15
	uint8_t scopeFb[SCOPE_LEN]    = {0};   // 0 or 1 (theme XNOR result fed into B1)
	int     scopeHead = 0;
	int     scopeFilled = 0;

	~Muse() {
		delete (MuseLinkMessage*)rightExpander.producerMessage;
		delete (MuseLinkMessage*)rightExpander.consumerMessage;
	}

	Muse() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// Expander message buffers (this Muse → its right-neighbor Muse)
		rightExpander.producerMessage = new MuseLinkMessage();
		rightExpander.consumerMessage = new MuseLinkMessage();

		for (int i = 0; i < 4; i++) {
			configParam(THEME_PARAM_0 + i, 0.f, (float)(N_TAPS - 1), 0.f,
				string::f("Theme %d tap", i + 1));
			paramQuantities[THEME_PARAM_0 + i]->snapEnabled = true;
		}
		for (int i = 0; i < 4; i++) {
			const char* nm[4] = {"A", "B", "C", "D"};
			configParam(INTERVAL_PARAM_0 + i, 0.f, (float)(N_TAPS - 1), 0.f,
				string::f("Interval %s tap", nm[i]));
			paramQuantities[INTERVAL_PARAM_0 + i]->snapEnabled = true;
		}

		configParam<RootParamQuantity>(ROOT_PARAM, -24.f, 24.f, 0.f, "Root");
		paramQuantities[ROOT_PARAM]->snapEnabled = true;

		// Scale selector — snap knob with scale names as values
		{
			std::vector<std::string> scaleNames;
			for (int i = 0; i < NUM_MUSE_SCALES; i++)
				scaleNames.push_back(MUSE_SCALES[i].museName);
			configSwitch(SCALE_PARAM, 0.f, (float)(NUM_MUSE_SCALES - 1), 0.f,
				"Scale", scaleNames);
		}

		configButton(RUN_PARAM, "Run / Stop");
		configButton(RESET_PARAM, "Reset");
		configButton(RAND_PARAM, "Randomize sliders");

		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset trigger");
		configInput(RUN_INPUT, "Run gate");
		configInput(RAND_INPUT, "Randomize trigger");
		for (int i = 0; i < 4; i++) {
			const char* nm[4] = {"A", "B", "C", "D"};
			configInput(INTERVAL_CV_0 + i, string::f("Interval %s CV", nm[i]));
			configInput(THEME_CV_0 + i,    string::f("Theme %d CV",    i + 1));
		}
		configInput(ROOT_CV_INPUT,  "Root CV (1V/oct)");
		configInput(SCALE_CV_INPUT, "Scale CV");

		configOutput(VOCT_OUTPUT, "Pitch (V/oct)");
		configOutput(GATE_OUTPUT, "Gate");
	}

	void onReset() override {
		core.clearState();
		running = true;
		lastPitchAddr = -1;
		clockFlash = 0.f;
		for (int i = 0; i < SCOPE_LEN; i++) { scopePitch[i] = 0; scopeFb[i] = 0; }
		scopeHead = 0;
		scopeFilled = 0;
	}

	// Push current slider params (+ per-slider CV offsets) into the core.
	// CV: ±5V → ±20 taps (4 taps/V) — a full swing covers half the range, so
	// a static CV nudges the tap by a few rows; modulating an LFO across the
	// full ±5V can scrub through every tap.
	static constexpr float CV_TAPS_PER_VOLT = 4.f;

	void syncSlidersToCore() {
		for (int i = 0; i < 4; i++) {
			float iv = params[INTERVAL_PARAM_0 + i].getValue();
			if (inputs[INTERVAL_CV_0 + i].isConnected())
				iv += inputs[INTERVAL_CV_0 + i].getVoltage() * CV_TAPS_PER_VOLT;
			core.interval[i] = clamp((int)std::round(iv), 0, N_TAPS - 1);

			float tv = params[THEME_PARAM_0 + i].getValue();
			if (inputs[THEME_CV_0 + i].isConnected())
				tv += inputs[THEME_CV_0 + i].getVoltage() * CV_TAPS_PER_VOLT;
			core.theme[i] = clamp((int)std::round(tv), 0, N_TAPS - 1);
		}
	}

	void doRandomize() {
		// random::u32() % 40 — uses Rack's seedable RNG so seeds stay deterministic
		// across patch loads if the user wants reproducibility.
		bool both = (randomScope == 0);
		bool theme = (randomScope == 0 || randomScope == 1);
		bool itvl  = (randomScope == 0 || randomScope == 2);
		(void)both;
		for (int i = 0; i < 4; i++) {
			if (theme) {
				int v = (int)(random::uniform() * N_TAPS);
				if (v >= N_TAPS) v = N_TAPS - 1;
				params[THEME_PARAM_0 + i].setValue((float)v);
			}
			if (itvl) {
				int v = (int)(random::uniform() * N_TAPS);
				if (v >= N_TAPS) v = N_TAPS - 1;
				params[INTERVAL_PARAM_0 + i].setValue((float)v);
			}
		}
	}

	// True when this Muse is currently slaved to a left-neighbor Muse.
	bool isFollowing() const { return followingThisFrame; }

	// Apply one of the Triadex manual presets — sets all 8 sliders.
	void loadPreset(int idx) {
		if (idx < 0 || idx >= NUM_MUSE_PRESETS) return;
		const MusePreset& p = MUSE_PRESETS[idx];
		for (int i = 0; i < 4; i++) {
			params[INTERVAL_PARAM_0 + i].setValue((float)p.interval[i]);
			params[THEME_PARAM_0    + i].setValue((float)p.theme[i]);
		}
		// Fresh start: clear SR so the pattern unfolds from a known seed.
		core.clearState();
		lastPitchAddr = -1;
	}

	// Muse reads eight flattened degrees rather than a scale, so a bus scale has
	// to be flattened the same way: degree i, continuing past the end of the
	// scale by repeating at its PERIOD. That is exactly the rule the canonical
	// museSemis[] tables were built with, so nothing moves for the nineteen.
	sfs::BusScale busScale;
	float museDegree(int i) const {
		if (!busScale.extended) return sfs::SCALES[busScale.index].museSemis[i & 7];
		return busScale.degree(i & 7);
	}
	int currentScaleIdx() {
		float v = params[SCALE_PARAM].getValue();
		if (inputs[SCALE_CV_INPUT].isConnected()) {
			// ~1V per scale step; small CV nudges, big CV scrubs through the list.
			v += inputs[SCALE_CV_INPUT].getVoltage();
		}
		return clamp((int)std::round(v), 0, NUM_MUSE_SCALES - 1);
	}

	float voctForPitchAddr(int addr) {
		int idx3 = addr & 0x7;
		int oct  = (addr >> 3) & 0x1;
		float rootSemis = params[ROOT_PARAM].getValue();
		if (inputs[ROOT_CV_INPUT].isConnected())
			rootSemis += inputs[ROOT_CV_INPUT].getVoltage() * 12.f;  // 1V/oct
		float semis = museDegree(idx3) + busScale.period * (float)oct + rootSemis;
		return semis / 12.f;
	}

	void process(const ProcessArgs& args) override {
		busScale = sfs::busResolve(inputs[SCALE_CV_INPUT],
		                           (int)std::round(params[SCALE_PARAM].getValue()));

		// --- Expander linking: read left-neighbor master's state (if any) ---
		followingThisFrame = false;
		uint32_t recvSr = 0;
		uint8_t  recvBinC = 0, recvMod12C = 0;
		bool     recvClockHigh = false;
		bool     recvTicked = false;
		uint8_t  recvFb = 0;
		uint16_t recvHops = 0;
		if (linkingEnabled && leftExpander.module && leftExpander.module->model == modelMuse) {
			MuseLinkMessage* msg = (MuseLinkMessage*)leftExpander.module->rightExpander.consumerMessage;
			if (msg && msg->valid) {
				followingThisFrame = true;
				recvSr         = msg->sr;
				recvBinC       = msg->binCounter;
				recvMod12C     = msg->mod12Counter;
				recvClockHigh  = msg->clockHigh;
				recvTicked     = msg->clockTickedThisFrame;
				recvFb         = msg->fbBit;
				recvHops       = msg->hopsFromMaster;
			}
		}
		hopsFromMasterCached = followingThisFrame ? (recvHops + 1) : 0;

		// --- Run latch & gate override ---
		if (params[RUN_PARAM].getValue() > 0.f) {
			params[RUN_PARAM].setValue(0.f);
			running = !running;
		}
		bool effectiveRunning = running;
		if (inputs[RUN_INPUT].isConnected()) {
			effectiveRunning = inputs[RUN_INPUT].getVoltage() >= 1.f;
		}
		lights[RUN_LIGHT].setBrightness(effectiveRunning ? 1.f : 0.f);

		// --- Reset (button or CV) --- still respected per-Muse so each slave
		// can reset its own scope/lastPitchAddr; doesn't clear master state
		// (would only matter when standalone anyway).
		bool resetEdge = false;
		if (resetButtonTrigger.process(params[RESET_PARAM].getValue())) resetEdge = true;
		if (inputs[RESET_INPUT].isConnected()
			&& resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			resetEdge = true;
		}
		if (resetEdge) {
			if (!followingThisFrame) core.clearState();
			lastPitchAddr = -1;
			gateRemaining = 0;
			samplesSinceTick = 0;
			haveTickInterval = false;
		}

		// --- Random (button or CV) ---
		bool randEdge = false;
		if (randButtonTrigger.process(params[RAND_PARAM].getValue())) randEdge = true;
		if (inputs[RAND_INPUT].isConnected()
			&& randTrigger.process(inputs[RAND_INPUT].getVoltage(), 0.1f, 1.f)) {
			randEdge = true;
		}
		if (randEdge) doRandomize();

		// --- Slider params (+ per-slider CVs) → core. When slaved, Theme is
		// inert (its bits won't reach B1) but we still update core.theme for
		// display consistency — the dim overlay communicates "ignored."
		syncSlidersToCore();

		// --- Advance machine ---
		// Time since the last clock tick, used to size the 50%-duty gate.
		samplesSinceTick = std::min(samplesSinceTick + 1, (int)(args.sampleRate * 10.f));
		bool didTickThisFrame = false;
		uint8_t myFbBit = 0;

		if (followingThisFrame) {
			// Slave path: copy master's state into our core BEFORE reading
			// pitchAddr so the Interval taps sample the master's bit stream.
			core.sr           = recvSr;
			core.binCounter   = recvBinC;
			core.mod12Counter = recvMod12C;
			core.clockHigh    = recvClockHigh;
			myFbBit           = recvFb;
			didTickThisFrame  = recvTicked;  // forwarded regardless of our RUN

			if (effectiveRunning && recvTicked) {
				clockFlash = 1.f;
				int addr = core.pitchAddr();
				scopePitch[scopeHead] = (uint8_t)(addr & 0x0F);
				scopeFb[scopeHead]    = myFbBit;
				scopeHead = (scopeHead + 1) % SCOPE_LEN;
				if (scopeFilled < SCOPE_LEN) scopeFilled++;

				bool emitGate = !gateOnChangeOnly || (addr != lastPitchAddr);
				lastPitchAddr = addr;
				if (emitGate) {
					int ivl = haveTickInterval ? samplesSinceTick : (int)(args.sampleRate * 0.05f);
					gateRemaining = std::max(1, ivl / 2);
				}
			}
		} else {
			// Standalone / master path: drive from our own CLOCK input
			if (effectiveRunning && inputs[CLOCK_INPUT].isConnected()) {
				if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
					// Capture the feedback bit ABOUT to be shifted in (Theme XNOR),
					// for the scope and for forwarding to slaves.
					int fbSum = 0;
					for (int i = 0; i < 4; i++) fbSum += core.tap(core.theme[i]) ? 1 : 0;
					myFbBit = !(fbSum & 1) ? 1 : 0;

					core.clockTick();
					didTickThisFrame = true;
					clockFlash = 1.f;

					int addr = core.pitchAddr();
					scopePitch[scopeHead] = (uint8_t)(addr & 0x0F);
					scopeFb[scopeHead]    = myFbBit;
					scopeHead = (scopeHead + 1) % SCOPE_LEN;
					if (scopeFilled < SCOPE_LEN) scopeFilled++;

					bool emitGate = !gateOnChangeOnly || (addr != lastPitchAddr);
					lastPitchAddr = addr;
					if (emitGate) {
						int ivl = haveTickInterval ? samplesSinceTick : (int)(args.sampleRate * 0.05f);
						gateRemaining = std::max(1, ivl / 2);
					}
				}
			}
		}

		// Capture the clock interval and reset the counter on each tick.
		if (didTickThisFrame) {
			tickInterval = samplesSinceTick;
			samplesSinceTick = 0;
			haveTickInterval = true;
		}

		// --- Forward state to right-neighbor Muse via expander ---
		if (rightExpander.module && rightExpander.module->model == modelMuse) {
			MuseLinkMessage* tx = (MuseLinkMessage*)rightExpander.producerMessage;
			if (tx) {
				tx->valid                = true;
				tx->sr                   = core.sr;
				tx->binCounter           = core.binCounter;
				tx->mod12Counter         = core.mod12Counter;
				tx->clockHigh            = core.clockHigh;
				tx->clockTickedThisFrame = didTickThisFrame;
				tx->fbBit                = myFbBit;
				tx->hopsFromMaster       = (uint16_t)hopsFromMasterCached;
				rightExpander.requestMessageFlip();
			}
		}

		// Decay the clock-flash indicator
		clockFlash = std::max(0.f, clockFlash - args.sampleTime / 0.10f);

		// --- Outputs ---
		// V/oct: standard 1V/oct with scale + root applied (VCO use).
		// 1V/2V/5V: same scale-quantized pitch but rescaled so the natural
		// 2-octave Muse range fills the chosen target voltage. ROOT is ignored
		// in scaled modes (modulation use — output should stay within the
		// stated voltage range no matter what ROOT is set to).
		float cvOut;
		int addrNow = core.pitchAddr();
		if (cvScaleMode == CV_SCALE_VOCT) {
			cvOut = voctForPitchAddr(addrNow);
		} else {
			(void)0;
			int idx3 = addrNow & 0x7;
			int oct  = (addrNow >> 3) & 0x1;
			float pitchSemis = museDegree(idx3) + busScale.period * (float)oct;
			// The scale's full Muse range (idx3=7, D=1) = top degree + a period.
			float scaleMaxSemis = museDegree(7) + busScale.period;
			if (scaleMaxSemis < 0.001f) scaleMaxSemis = 24.f;  // safety
			float targetV;
			switch (cvScaleMode) {
				case CV_SCALE_1V: targetV = 1.f; break;
				case CV_SCALE_2V: targetV = 2.f; break;
				default:          targetV = 5.f; break;   // CV_SCALE_5V
			}
			cvOut = (pitchSemis / scaleMaxSemis) * targetV;
		}
		outputs[VOCT_OUTPUT].setVoltage(cvOut);
		bool gateHi = gateRemaining > 0;
		if (gateRemaining > 0) gateRemaining--;
		outputs[GATE_OUTPUT].setVoltage(gateHi ? 10.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "running", json_boolean(running));
		json_object_set_new(rootJ, "gateOnChangeOnly", json_boolean(gateOnChangeOnly));
		json_object_set_new(rootJ, "randomScope", json_integer(randomScope));
		json_object_set_new(rootJ, "linkingEnabled", json_boolean(linkingEnabled));
		json_object_set_new(rootJ, "cvScaleMode", json_integer(cvScaleMode));
		// Persist SR + counter state so reload picks up mid-sequence
		json_object_set_new(rootJ, "sr", json_integer((json_int_t)core.sr));
		json_object_set_new(rootJ, "binCounter", json_integer(core.binCounter));
		json_object_set_new(rootJ, "mod12Counter", json_integer(core.mod12Counter));
		json_object_set_new(rootJ, "clockHigh", json_boolean(core.clockHigh));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		if (json_t* j = json_object_get(rootJ, "running"))          running = json_boolean_value(j);
		// Backward compat: pre-SCALE_PARAM patches stored scaleIdx as a JSON int.
		if (json_t* j = json_object_get(rootJ, "scaleIdx")) {
			int v = clamp((int)json_integer_value(j), 0, NUM_MUSE_SCALES - 1);
			params[SCALE_PARAM].setValue((float)v);
		}
		if (json_t* j = json_object_get(rootJ, "gateOnChangeOnly")) gateOnChangeOnly = json_boolean_value(j);
		if (json_t* j = json_object_get(rootJ, "randomScope"))      randomScope = clamp((int)json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(rootJ, "linkingEnabled"))   linkingEnabled = json_boolean_value(j);
		if (json_t* j = json_object_get(rootJ, "cvScaleMode"))      cvScaleMode = clamp((int)json_integer_value(j), 0, 3);
		if (json_t* j = json_object_get(rootJ, "sr"))               core.sr = (uint32_t)json_integer_value(j);
		if (json_t* j = json_object_get(rootJ, "binCounter"))       core.binCounter = (uint8_t)json_integer_value(j) & 0x0F;
		if (json_t* j = json_object_get(rootJ, "mod12Counter"))     core.mod12Counter = (uint8_t)(json_integer_value(j) % 12);
		if (json_t* j = json_object_get(rootJ, "clockHigh"))        core.clockHigh = json_boolean_value(j);
	}
};


// ─── Custom full-height slider ───────────────────────────────────────────────

struct MuseSlider : app::SvgSlider {
	bool isTheme = false;   // dimmed when this Muse is in slave mode
	MuseSlider() {
		setBackgroundSvg(Svg::load(asset::plugin(pluginInstance, "res/muse-slider-bg.svg")));
		setHandleSvg(Svg::load(asset::plugin(pluginInstance, "res/muse-slider-handle.svg")));
		// Background SVG is 17 wide × 261 tall (SVG units), with a 40-tick rail
		// at y = 3 .. 258. Handle is 17 wide × 6 tall, anchored on its center,
		// and we align its centers with the first and last tick.
		//
		// Faithful layout puts value 0 (= OFF) at the TOP and value 39 (= B31)
		// at the BOTTOM, matching the original Muse and till.com. Because
		// Knob::onDragMove hard-codes "drag up = value increases", we invert
		// the drag direction by setting speed = -1 so dragging DOWN increases
		// value — which visually moves the handle down through the tap list
		// (the handle follows the finger; up = lower tap index = OFF/ON/C½...).
		setHandlePosCentered(
			math::Vec(17.f / 2.f,   3.f),    // value MIN aligns with tick 0 (top)
			math::Vec(17.f / 2.f, 258.f)     // value MAX aligns with tick 39 (bottom)
		);
		speed = -1.f;
	}

	void draw(const DrawArgs& args) override {
		bool dim = false;
		if (isTheme) {
			Muse* m = (getParamQuantity()) ? dynamic_cast<Muse*>(getParamQuantity()->module) : nullptr;
			if (m && m->isFollowing()) dim = true;
		}
		if (dim) nvgGlobalAlpha(args.vg, 0.35f);
		app::SvgSlider::draw(args);
		if (dim) nvgGlobalAlpha(args.vg, 1.f);
	}
};


// ─── Display drawLayer ───────────────────────────────────────────────────────

void MuseDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) {
		Widget::drawLayer(args, layer);
		return;
	}
	if (!module) {
		drawPreview(args);
		return;
	}

	// Palette (matches Meter/Beat)
	const NVGcolor COL_BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_TEXT_BRIGHT = nvgRGBA(0xFF, 0xFF, 0xFF, 0xFF);
	const NVGcolor COL_TEXT_DIM    = nvgRGBA(0x70, 0x70, 0x80, 0xFF);
	const NVGcolor COL_TEXT_MID    = nvgRGBA(0xB0, 0xB0, 0xC0, 0xFF);
	const NVGcolor COL_LED_OFF     = nvgRGBA(0x18, 0x18, 0x24, 0xFF);
	const NVGcolor COL_DIV         = nvgRGBA(0x2A, 0x2A, 0x40, 0xFF);

	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	float w = box.size.x;
	float h = box.size.y;

	// --- Vertical zones (widget-local) -----------------------------------
	// Top strip: column headers (A B C D | T1 T2 T3 T4 | TAP | ●)
	// Middle: 40 tap rows
	// Bottom: calculated note + addr indicator
	const float headerH = 9.f;
	const float statusH = 16.f;
	const float gridTop = headerH;
	const float gridBot = h - statusH;
	const float gridH   = gridBot - gridTop;
	const float rowH    = gridH / (float)N_TAPS;

	// --- Horizontal zones (widget-local) ---------------------------------
	// 8 slider columns at left, then label column, then LED column.
	// Interval A/B/C/D on the left half of slider area, Theme T1..T4 right.
	const float sliderColX[8] = {
		w * 0.05f,   // A
		w * 0.10f,   // B
		w * 0.15f,   // C
		w * 0.20f,   // D
		w * 0.30f,   // T1
		w * 0.35f,   // T2
		w * 0.40f,   // T3
		w * 0.45f,   // T4
	};
	const float labelRightX = w * 0.78f;
	const float ledX        = w * 0.92f;
	const float ledR        = std::min(rowH * 0.34f, 2.0f);

	// Read all 8 slider tap positions once — using core's effective values so
	// the dots reflect any per-slider CV offset, not just the raw knob.
	int sliderTap[8];
	for (int i = 0; i < 4; i++) sliderTap[i]   = module->core.interval[i];
	for (int i = 0; i < 4; i++) sliderTap[i+4] = module->core.theme[i];

	const bool slaved = module->isFollowing();

	// --- Header strip: column letters ------------------------------------
	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 6.5f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		const char* hdr[8] = {"A","B","C","D","T1","T2","T3","T4"};
		for (int s = 0; s < 8; s++) {
			// Tint interval headers orange-ish, theme headers blue-ish, both dim
			nvgFillColor(args.vg,
				(s < 4)
					? nvgRGBA(0xC8, 0x6E, 0x3F, 0xC0)
					: nvgRGBA(0x4A, 0x9A, 0xCD, 0xC0));
			nvgText(args.vg, sliderColX[s], headerH * 0.5f, hdr[s], NULL);
		}
		// "TAP" and "·" header
		nvgFillColor(args.vg, COL_TEXT_DIM);
		nvgText(args.vg, labelRightX - 14.f, headerH * 0.5f, "TAP", NULL);
		nvgText(args.vg, ledX,                headerH * 0.5f, "·",   NULL);
	}

	// Subtle divider between header and grid
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, w * 0.02f, headerH);
	nvgLineTo(args.vg, w * 0.98f, headerH);
	nvgStrokeColor(args.vg, COL_DIV);
	nvgStrokeWidth(args.vg, 0.6f);
	nvgStroke(args.vg);

	// Subtle divider between Interval and Theme slider groups
	float dividerX = (sliderColX[3] + sliderColX[4]) * 0.5f;
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, dividerX, headerH + 1.f);
	nvgLineTo(args.vg, dividerX, gridBot - 1.f);
	nvgStrokeColor(args.vg, COL_DIV);
	nvgStrokeWidth(args.vg, 0.4f);
	nvgStroke(args.vg);

	// --- Tap rows --------------------------------------------------------
	if (font && font->handle >= 0) {
		nvgFontSize(args.vg, std::min(rowH * 0.95f, 7.5f));
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	}

	for (int i = 0; i < N_TAPS; i++) {
		float cy = gridTop + (i + 0.5f) * rowH;
		bool bitOn = module->core.tap(i);

		// Is any slider on this row? Used to brighten label.
		bool selected = false;
		for (int s = 0; s < 8; s++) if (sliderTap[s] == i) { selected = true; break; }

		// Per-slider position dot. Theme dots (s>=4) dim when slaved, since
		// they're not actually driving the SR — only the master's Theme is.
		float dotR = std::min(rowH * 0.36f, 1.7f);
		for (int s = 0; s < 8; s++) {
			if (sliderTap[s] == i) {
				NVGcolor col = (s < 4) ? COL_ORANGE : COL_BLUE;
				if (s >= 4 && slaved) col.a = 0.30f;
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, sliderColX[s], cy, dotR);
				nvgFillColor(args.vg, col);
				nvgFill(args.vg);
			}
		}

		// Row label
		NVGcolor labelCol = selected ? COL_TEXT_BRIGHT : COL_TEXT_DIM;
		if (font && font->handle >= 0) {
			nvgFillColor(args.vg, labelCol);
			nvgText(args.vg, labelRightX, cy, TAP_LABELS[i], NULL);
		}

		// LED state lamp — C½ row tints orange briefly on clock tick
		NVGcolor litCol = COL_TEXT_BRIGHT;
		if (i == TAP_C_HALF && module->clockFlash > 0.01f) {
			float t = clamp(module->clockFlash, 0.f, 1.f);
			int r = (int)(0xFF + (0xEC - 0xFF) * t);
			int g = (int)(0xFF + (0x65 - 0xFF) * t);
			int b = (int)(0xFF + (0x2E - 0xFF) * t);
			litCol = nvgRGBA(r, g, b, 0xFF);
		}
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, ledX, cy, ledR);
		nvgFillColor(args.vg, bitOn ? litCol : COL_LED_OFF);
		nvgFill(args.vg);
	}

	// --- Status bar at bottom: calculated note ---------------------------
	// Divider above status
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, w * 0.02f, gridBot);
	nvgLineTo(args.vg, w * 0.98f, gridBot);
	nvgStrokeColor(args.vg, COL_DIV);
	nvgStrokeWidth(args.vg, 0.6f);
	nvgStroke(args.vg);

	int addr = module->core.pitchAddr();
	int idx3 = addr & 0x7;
	int oct  = (addr >> 3) & 0x1;

	float voct = module->voctForPitchAddr(addr);
	// V/oct 0V = C4 by VCV convention. MIDI note = 60 + 12*voct.
	int midi = (int)std::round(60.f + 12.f * voct);
	static const char* noteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
	int nIdx = ((midi % 12) + 12) % 12;
	int nOct = (midi / 12) - 1;
	(void)COL_TEXT_MID;

	float statusY = gridBot + statusH * 0.5f;

	if (font && font->handle >= 0) {
		// Big note name (left half of status bar)
		nvgFontSize(args.vg, 11.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_BRIGHT);
		std::string noteStr = string::f("%s%d", noteNames[nIdx], nOct);
		nvgText(args.vg, 4.f, statusY, noteStr.c_str(), NULL);

		// Link badge (between note name and addr breakdown)
		if (slaved) {
			nvgFontSize(args.vg, 6.0f);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, COL_ORANGE);
			std::string lab = string::f("\xe2\x86\x90 FOLLOW %d",
				module->hopsFromMasterCached);
			nvgText(args.vg, w * 0.32f, statusY, lab.c_str(), NULL);
		} else if (module->rightExpander.module
			&& module->rightExpander.module->model == modelMuse
			&& module->linkingEnabled) {
			nvgFontSize(args.vg, 6.0f);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, COL_BLUE);
			nvgText(args.vg, w * 0.32f, statusY, "MASTER \xe2\x86\x92", NULL);
		}

		// Addr breakdown (right half)
		nvgFontSize(args.vg, 6.f);
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		// Show 4-bit binary address (D C B A) + octave flag + scale index
		// e.g. "D=0 CBA=5 (idx 5, oct 0)"
		std::string addrStr = string::f("DCBA=%d%d%d%d  oct%d  idx%d",
			(addr >> 3) & 1, (addr >> 2) & 1, (addr >> 1) & 1, addr & 1, oct, idx3);
		nvgText(args.vg, w - 4.f, statusY, addrStr.c_str(), NULL);
	}

	// Also dim the Theme column headers when slaved
	if (slaved) {
		// Paint a translucent dark wash over the Theme column header (T1..T4)
		float x0 = sliderColX[4] - rowH * 0.5f;
		float x1 = sliderColX[7] + rowH * 0.5f;
		nvgBeginPath(args.vg);
		nvgRect(args.vg, x0, 0.f, x1 - x0, headerH);
		nvgFillColor(args.vg, nvgRGBA(0x1a, 0x1a, 0x2e, 0xb0));
		nvgFill(args.vg);
	}

	Widget::drawLayer(args, layer);
}


// ─── MuseDisplay browser preview (module == NULL) ────────────────────────────
// Static slider/tap table with representative positions + lit LEDs.
void MuseDisplay::drawPreview(const DrawArgs& args) {
	const NVGcolor COL_BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_TEXT_BRIGHT = nvgRGBA(0xFF, 0xFF, 0xFF, 0xFF);
	const NVGcolor COL_TEXT_DIM    = nvgRGBA(0x70, 0x70, 0x80, 0xFF);
	const NVGcolor COL_LED_OFF     = nvgRGBA(0x18, 0x18, 0x24, 0xFF);
	const NVGcolor COL_DIV         = nvgRGBA(0x2A, 0x2A, 0x40, 0xFF);

	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	float w = box.size.x;
	float h = box.size.y;

	const float headerH = 9.f;
	const float statusH = 16.f;
	const float gridTop = headerH;
	const float gridBot = h - statusH;
	const float gridH   = gridBot - gridTop;
	const float rowH    = gridH / (float)N_TAPS;

	const float sliderColX[8] = {
		w * 0.05f, w * 0.10f, w * 0.15f, w * 0.20f,
		w * 0.30f, w * 0.35f, w * 0.40f, w * 0.45f,
	};
	const float labelRightX = w * 0.78f;
	const float ledX        = w * 0.92f;
	const float ledR        = std::min(rowH * 0.34f, 2.0f);

	// Preview slider positions: spread across the tap list so it looks active.
	const int sliderTap[8] = {2, 5, 7, 11, 9, 13, 4, 8};

	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 6.5f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		const char* hdr[8] = {"A","B","C","D","T1","T2","T3","T4"};
		for (int s = 0; s < 8; s++) {
			nvgFillColor(args.vg, (s < 4)
				? nvgRGBA(0xC8, 0x6E, 0x3F, 0xC0)
				: nvgRGBA(0x4A, 0x9A, 0xCD, 0xC0));
			nvgText(args.vg, sliderColX[s], headerH * 0.5f, hdr[s], NULL);
		}
		nvgFillColor(args.vg, COL_TEXT_DIM);
		nvgText(args.vg, labelRightX - 14.f, headerH * 0.5f, "TAP", NULL);
		nvgText(args.vg, ledX,                headerH * 0.5f, "·",   NULL);
	}

	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, w * 0.02f, headerH);
	nvgLineTo(args.vg, w * 0.98f, headerH);
	nvgStrokeColor(args.vg, COL_DIV); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

	float dividerX = (sliderColX[3] + sliderColX[4]) * 0.5f;
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, dividerX, headerH + 1.f);
	nvgLineTo(args.vg, dividerX, gridBot - 1.f);
	nvgStrokeColor(args.vg, COL_DIV); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

	if (font && font->handle >= 0) {
		nvgFontSize(args.vg, std::min(rowH * 0.95f, 7.5f));
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	}

	// Hardcoded LED bit pattern (a few rows lit to look "active")
	const bool litBit[N_TAPS] = {
		false, true, true, false, true, false, true, true,    // 0..7
		false, true, false, true, false, true, true, false,   // 8..15
		true, false, false, true, true, false, true, false,   // 16..23
		false, true, true, false, true, true, false, true,    // 24..31
		false, true, false, true, true, false, false, true    // 32..39
	};

	for (int i = 0; i < N_TAPS; i++) {
		float cy = gridTop + (i + 0.5f) * rowH;
		bool selected = false;
		for (int s = 0; s < 8; s++) if (sliderTap[s] == i) { selected = true; break; }
		float dotR = std::min(rowH * 0.36f, 1.7f);
		for (int s = 0; s < 8; s++) {
			if (sliderTap[s] == i) {
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, sliderColX[s], cy, dotR);
				nvgFillColor(args.vg, (s < 4) ? COL_ORANGE : COL_BLUE); nvgFill(args.vg);
			}
		}
		NVGcolor labelCol = selected ? COL_TEXT_BRIGHT : COL_TEXT_DIM;
		if (font && font->handle >= 0) {
			nvgFillColor(args.vg, labelCol);
			nvgText(args.vg, labelRightX, cy, TAP_LABELS[i], NULL);
		}
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, ledX, cy, ledR);
		nvgFillColor(args.vg, litBit[i] ? COL_TEXT_BRIGHT : COL_LED_OFF);
		nvgFill(args.vg);
	}

	// Status bar
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, w * 0.02f, gridBot);
	nvgLineTo(args.vg, w * 0.98f, gridBot);
	nvgStrokeColor(args.vg, COL_DIV); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

	if (font && font->handle >= 0) {
		float statusY = gridBot + statusH * 0.5f;
		nvgFontSize(args.vg, 11.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_BRIGHT);
		nvgText(args.vg, 4.f, statusY, "E4", NULL);
		nvgFontSize(args.vg, 6.f);
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		nvgText(args.vg, w - 4.f, statusY, "DCBA=0011  oct0  idx3", NULL);
	}
}


// ─── Scope drawLayer ─────────────────────────────────────────────────────────

void MuseScope::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) {
		Widget::drawLayer(args, layer);
		return;
	}
	if (!module) {
		drawPreview(args);
		return;
	}

	const NVGcolor COL_BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_BLUE_DIM    = nvgRGBA(0x00, 0x97, 0xDE, 0x40);
	const NVGcolor COL_ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_TEXT_DIM    = nvgRGBA(0x70, 0x70, 0x80, 0xFF);
	const NVGcolor COL_DIV         = nvgRGBA(0x2A, 0x2A, 0x40, 0xFF);

	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	float w = box.size.x;
	float h = box.size.y;

	// Two stacked traces (cascading vertically):
	//   Top (taller, 70% of h):  PITCH trajectory — addr 0..15 → step line
	//   Bottom (30%):            Theme feedback bit — 0/1 bar trace
	const float labW = 14.f;        // narrow strip on the left for labels
	const float plotX0 = labW;
	const float plotW  = w - labW - 2.f;
	const float traceGap = 2.f;
	const float topH = (h - traceGap) * 0.72f;
	const float botH = (h - traceGap) * 0.28f;
	const float topY0 = 1.f;
	const float topY1 = topY0 + topH;
	const float botY0 = topY1 + traceGap;
	const float botY1 = botY0 + botH;

	// Labels on the left strip
	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 5.5f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		nvgText(args.vg, 2.f, topY0 + topH * 0.5f, "PITCH", NULL);
		nvgText(args.vg, 2.f, botY0 + botH * 0.5f, "B1", NULL);
	}

	// Vertical divider between label strip and plots
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, labW - 0.5f, 1.f);
	nvgLineTo(args.vg, labW - 0.5f, h - 1.f);
	nvgStrokeColor(args.vg, COL_DIV);
	nvgStrokeWidth(args.vg, 0.5f);
	nvgStroke(args.vg);

	// Horizontal grid lines on the pitch plot (octave divisions at addr=8)
	{
		float midY = topY0 + topH * 0.5f;  // addr 8 boundary roughly
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, plotX0, midY);
		nvgLineTo(args.vg, plotX0 + plotW, midY);
		nvgStrokeColor(args.vg, COL_DIV);
		nvgStrokeWidth(args.vg, 0.4f);
		nvgStroke(args.vg);
	}

	int N = Muse::SCOPE_LEN;
	int filled = module->scopeFilled;
	if (filled <= 0) {
		Widget::drawLayer(args, layer);
		return;
	}

	// Show the most recent SHOWN samples (not the whole buffer). The shorter
	// window makes individual pulses on the B1 trace clearly visible at this
	// scope width, and the pitch trace stays time-correlated with it.
	const int SHOWN = 32;
	int shown = std::min(filled, SHOWN);
	float colW = plotW / (float)SHOWN;

	// Pitch trace: step line. Read newest-on-right, oldest-on-left.
	// scopeHead points at NEXT write slot; (scopeHead-1) is newest.
	int newest = (module->scopeHead - 1 + N) % N;
	int firstShown = SHOWN - shown;  // # of empty columns on the left while warming up

	// Pitch trace (step line)
	nvgBeginPath(args.vg);
	bool first = true;
	for (int col = firstShown; col < SHOWN; col++) {
		int back = SHOWN - 1 - col;   // 0 = newest sample, SHOWN-1 = oldest shown
		int idx  = (newest - back + N) % N;
		int v    = module->scopePitch[idx];
		float xL = plotX0 + col * colW;
		float xR = xL + colW;
		// Map 0..15 to topY1 (bottom) .. topY0 (top)
		float y  = topY1 - ((float)v / 15.f) * topH;
		if (first) {
			nvgMoveTo(args.vg, xL, y);
			first = false;
		} else {
			nvgLineTo(args.vg, xL, y);
		}
		nvgLineTo(args.vg, xR, y);
	}
	nvgStrokeColor(args.vg, COL_ORANGE);
	nvgStrokeWidth(args.vg, 1.0f);
	nvgStroke(args.vg);

	// B1 (Theme feedback) trace: filled bars where bit=1, faint baseline at 0
	const float barInset = std::min(colW * 0.18f, 0.6f);  // small gap between bars
	const float barW = std::max(colW - 2.f * barInset, 0.8f);
	for (int col = firstShown; col < SHOWN; col++) {
		int back = SHOWN - 1 - col;
		int idx  = (newest - back + N) % N;
		int b    = module->scopeFb[idx];
		float xL = plotX0 + col * colW + barInset;
		float barH = (botH - 2.f);
		if (b) {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, xL, botY1 - 1.f - barH, barW, barH);
			nvgFillColor(args.vg, COL_BLUE);
			nvgFill(args.vg);
		} else {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, xL, botY1 - 1.5f, barW, 0.6f);
			nvgFillColor(args.vg, COL_BLUE_DIM);
			nvgFill(args.vg);
		}
	}

	Widget::drawLayer(args, layer);
}


// ─── MuseScope browser preview (module == NULL) ──────────────────────────────
void MuseScope::drawPreview(const DrawArgs& args) {
	const NVGcolor COL_ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_TEXT_DIM    = nvgRGBA(0x70, 0x70, 0x80, 0xFF);
	const NVGcolor COL_DIV         = nvgRGBA(0x2A, 0x2A, 0x40, 0xFF);

	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	float w = box.size.x;
	float h = box.size.y;
	const float labW = 14.f;
	const float plotX0 = labW;
	const float plotW  = w - labW - 2.f;
	const float traceGap = 2.f;
	const float topH = (h - traceGap) * 0.72f;
	const float botH = (h - traceGap) * 0.28f;
	const float topY0 = 1.f;
	const float topY1 = topY0 + topH;
	const float botY0 = topY1 + traceGap;
	const float botY1 = botY0 + botH;

	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 5.5f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		nvgText(args.vg, 2.f, topY0 + topH * 0.5f, "PITCH", NULL);
		nvgText(args.vg, 2.f, botY0 + botH * 0.5f, "B1", NULL);
	}

	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, labW - 0.5f, 1.f);
	nvgLineTo(args.vg, labW - 0.5f, h - 1.f);
	nvgStrokeColor(args.vg, COL_DIV); nvgStrokeWidth(args.vg, 0.5f); nvgStroke(args.vg);

	float midY = topY0 + topH * 0.5f;
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, plotX0, midY);
	nvgLineTo(args.vg, plotX0 + plotW, midY);
	nvgStrokeColor(args.vg, COL_DIV); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

	// Synthesize a 32-step "trajectory" of pitch addresses and B1 bits
	const int SHOWN = 32;
	const int pitch[SHOWN] = {5,7,8,5,10,7,12,5, 9,5,8,7,11,5,7,9, 6,8,10,7,5,9,8,11, 7,5,10,8,7,9,6,8};
	const int b1[SHOWN]    = {1,0,1,1,0,1,0,1,  1,1,0,1,0,1,1,0,  1,0,1,1,0,1,0,1,  0,1,1,0,1,0,1,1};
	float colW = plotW / (float)SHOWN;

	// Pitch step line
	nvgBeginPath(args.vg);
	for (int col = 0; col < SHOWN; col++) {
		float xL = plotX0 + col * colW;
		float xR = xL + colW;
		float y = topY1 - ((float)pitch[col] / 15.f) * topH;
		if (col == 0) nvgMoveTo(args.vg, xL, y);
		else nvgLineTo(args.vg, xL, y);
		nvgLineTo(args.vg, xR, y);
	}
	nvgStrokeColor(args.vg, COL_ORANGE); nvgStrokeWidth(args.vg, 1.0f); nvgStroke(args.vg);

	// B1 bars
	const float barInset = std::min(colW * 0.18f, 0.6f);
	const float barW = std::max(colW - 2.f * barInset, 0.8f);
	for (int col = 0; col < SHOWN; col++) {
		if (!b1[col]) continue;
		float xL = plotX0 + col * colW + barInset;
		float barH = (botH - 2.f);
		nvgBeginPath(args.vg);
		nvgRect(args.vg, xL, botY1 - 1.f - barH, barW, barH);
		nvgFillColor(args.vg, COL_BLUE); nvgFill(args.vg);
	}
}


// ─── Widget ──────────────────────────────────────────────────────────────────

struct MuseWidget : ModuleWidget {
	MuseWidget(Muse* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/muse.svg")));

		// 30HP = 152.4mm wide × 128.5mm tall.
		// Layout: left column (x=0..20) for general controls + outputs;
		// slider region in the middle; display + scope on the right.

		// Slider centers (X). 4 INTERVAL (left) + gap + 4 THEME (right),
		// matching the original Triadex Muse layout. Shifted right by 18mm
		// from the old 26HP design to make room for the left control column.
		// Slider columns (mm) read from the panel's green reference grid:
		//   INTERVAL: 10.16 / 20.32 / 30.48 / 40.64
		//   THEME:    55.88 / 66.04 / 76.20 / 86.36
		const float itvlX0     = 10.16f;
		const float themeX0    = 55.88f;
		const float sliderPitch = 10.16f;

		// Slider geometry. The BG art (res/muse-slider-bg.svg) is built to match
		// the panel's green grid exactly: 261px tall, with tap 0 at y=3 and
		// tap 39 at y=258 (a 255px detent span = grid 43.18..287.98 in panel
		// units). The handle travel below uses those same in-art coordinates, so
		// the box is self-consistent and the detents land on the grid no matter
		// where the box is placed. Box is centered on the grid center (mm).
		const float sliderY  = 58.413f;             // grid center in mm
		const float hMinY    = 3.f;                 // tap 0 (value MIN, top)
		const float hMaxY    = 258.f;               // tap 39 (value MAX, bottom)
		auto setSliderTravel = [&](MuseSlider* s) {
			s->setHandlePosCentered(math::Vec(17.f / 2.f, hMinY),
			                        math::Vec(17.f / 2.f, hMaxY));
			s->speed = -1.f;
		};

		// THEME sliders (right group) — tagged isTheme so they dim when slaved.
		for (int i = 0; i < 4; i++) {
			float x = themeX0 + i * sliderPitch;
			MuseSlider* s = createParamCentered<MuseSlider>(mm2px(Vec(x, sliderY)),
				module, Muse::THEME_PARAM_0 + i);
			s->isTheme = true;
			setSliderTravel(s);
			addParam(s);
		}
		// INTERVAL sliders (left group)
		for (int i = 0; i < 4; i++) {
			float x = itvlX0 + i * sliderPitch;
			MuseSlider* s = createParamCentered<MuseSlider>(mm2px(Vec(x, sliderY)),
				module, Muse::INTERVAL_PARAM_0 + i);
			setSliderTravel(s);
			addParam(s);
		}

		// Display (DisplayFrame rect: x264 y43.69 w153.07 h198.43 -> mm).
		MuseDisplay* display = new MuseDisplay();
		display->module = module;
		display->box.pos  = mm2px(Vec(93.13f, 15.41f));
		display->box.size = mm2px(Vec(54.0f, 70.0f));
		addChild(display);

		// Scope (ScopeFrame rect: x264 y247.79 w153.07 h39.69 -> mm).
		MuseScope* scope = new MuseScope();
		scope->module = module;
		scope->box.pos  = mm2px(Vec(93.13f, 87.41f));
		scope->box.size = mm2px(Vec(54.0f, 14.0f));
		addChild(scope);

		// 8 CV input jacks directly below each slider column (y=106.67mm).
		const float cvJackY = 106.67f;
		for (int i = 0; i < 4; i++) {
			float xI = itvlX0  + i * sliderPitch;
			float xT = themeX0 + i * sliderPitch;
			addInput(createInputCentered<PJ301MPort>(
				mm2px(Vec(xI, cvJackY)), module, Muse::INTERVAL_CV_0 + i));
			addInput(createInputCentered<PJ301MPort>(
				mm2px(Vec(xT, cvJackY)), module, Muse::THEME_CV_0 + i));
		}

		// --- Bottom global I/O row (y=121.93mm), left -> right ---
		//   Clock | Run(btn)+cv | Scale(pot)+cv | Root(pot)+cv | Reset(btn)+cv | Random(btn)+cv
		const float rowY = 121.93f;
		// CLOCK input
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, rowY)), module, Muse::CLOCK_INPUT));
		// RUN latch + CV
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(20.32f, rowY)), module, Muse::RUN_PARAM, Muse::RUN_LIGHT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(30.48f, rowY)), module, Muse::RUN_INPUT));
		// SCALE pot (small) + CV
		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(40.64f, rowY)), module, Muse::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(50.80f, rowY)), module, Muse::SCALE_CV_INPUT));
		// ROOT pot (small) + CV
		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(60.96f, rowY)), module, Muse::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(71.12f, rowY)), module, Muse::ROOT_CV_INPUT));
		// RESET button + CV
		addParam(createParamCentered<VCVButton>(
			mm2px(Vec(81.28f, rowY)), module, Muse::RESET_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(91.44f, rowY)), module, Muse::RESET_INPUT));
		// RANDOM button + CV
		addParam(createParamCentered<VCVButton>(
			mm2px(Vec(101.60f, rowY)), module, Muse::RAND_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(111.76f, rowY)), module, Muse::RAND_INPUT));

		// V/OCT and GATE outputs on the dark plate (bottom-right).
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(132.08f, rowY)), module, Muse::VOCT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(142.24f, rowY)), module, Muse::GATE_OUTPUT));
	}

	// Overlay translucent washes on inert panel elements when this Muse is
	// following a left-neighbor. The sliders themselves dim via
	// MuseSlider::draw; here we cover the CLOCK jack and the 4 Theme CV jacks
	// so the user sees they're not driving anything in follow mode.
	void draw(const DrawArgs& args) override {
		ModuleWidget::draw(args);
		Muse* m = dynamic_cast<Muse*>(module);
		if (!m || !m->isFollowing()) return;

		const NVGcolor wash = nvgRGBA(0xf0, 0xf0, 0xf0, 0xb0);
		auto dimJack = [&](float xMm, float yMm) {
			Vec c  = mm2px(Vec(xMm, yMm));
			float r = mm2px(4.5f);  // jack radius-ish
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, r);
			nvgFillColor(args.vg, wash);
			nvgFill(args.vg);
		};
		// CLOCK input (bottom global row)
		dimJack(10.16f, 121.93f);
		// 4 Theme CV jacks (under right-hand sliders)
		for (int i = 0; i < 4; i++) {
			dimJack(55.88f + i * 10.16f, 106.67f);
		}
	}

	void appendContextMenu(Menu* menu) override {
		Muse* module = dynamic_cast<Muse*>(this->module);
		assert(module);

		// Triadex manual presets (slider snapshots from the original 1972 manual)
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Presets (from Triadex manual)", "",
			[=](Menu* sub) {
				for (int i = 0; i < NUM_MUSE_PRESETS; i++) {
					int idx = i;
					sub->addChild(createMenuItem(
						MUSE_PRESETS[i].name, "",
						[=]() { module->loadPreset(idx); }
					));
				}
			}));

		// V/OCT output scaling. V/oct (default) is the standard 1V/oct pitch
		// CV (with SCALE + ROOT applied); 1V / 2V / 5V are scale-quantized
		// modes that rescale the Muse's natural range to that voltage span
		// and ignore ROOT (useful as a modulation source).
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Output range"));
		menu->addChild(createCheckMenuItem(
			"V/oct", "",
			[=]() { return module->cvScaleMode == Muse::CV_SCALE_VOCT; },
			[=]() { module->cvScaleMode = Muse::CV_SCALE_VOCT; }
		));
		menu->addChild(createCheckMenuItem(
			"1V", "",
			[=]() { return module->cvScaleMode == Muse::CV_SCALE_1V; },
			[=]() { module->cvScaleMode = Muse::CV_SCALE_1V; }
		));
		menu->addChild(createCheckMenuItem(
			"2V", "",
			[=]() { return module->cvScaleMode == Muse::CV_SCALE_2V; },
			[=]() { module->cvScaleMode = Muse::CV_SCALE_2V; }
		));
		menu->addChild(createCheckMenuItem(
			"5V", "",
			[=]() { return module->cvScaleMode == Muse::CV_SCALE_5V; },
			[=]() { module->cvScaleMode = Muse::CV_SCALE_5V; }
		));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Link"));
		menu->addChild(createBoolPtrMenuItem(
			"Allow expander linking (adjacent Muse → follow)", "",
			&module->linkingEnabled));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Gate"));
		menu->addChild(createCheckMenuItem(
			"Every clock pulse", "",
			[=]() { return !module->gateOnChangeOnly; },
			[=]() { module->gateOnChangeOnly = false; }
		));
		menu->addChild(createCheckMenuItem(
			"Only when pitch changes", "",
			[=]() { return module->gateOnChangeOnly; },
			[=]() { module->gateOnChangeOnly = true; }
		));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Randomize"));
		menu->addChild(createCheckMenuItem(
			"All 8 sliders", "",
			[=]() { return module->randomScope == 0; },
			[=]() { module->randomScope = 0; }
		));
		menu->addChild(createCheckMenuItem(
			"Theme only", "",
			[=]() { return module->randomScope == 1; },
			[=]() { module->randomScope = 1; }
		));
		menu->addChild(createCheckMenuItem(
			"Interval only", "",
			[=]() { return module->randomScope == 2; },
			[=]() { module->randomScope = 2; }
		));
		menu->addChild(createMenuItem(
			"Randomize now", "",
			[=]() { module->doRandomize(); }
		));
	}
};


Model* modelMuse = createModel<Muse, MuseWidget>("Muse");
