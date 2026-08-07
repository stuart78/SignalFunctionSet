#include "plugin.hpp"
#include "panel-style.hpp"
#include "scales.hpp"
#include "waveguide.hpp"
#include <cmath>
#include <cstring>
#include <atomic>
#include <string>

// =============================================================================
// Loom — an eight-string resonator.
//
// Eight vertical strings stretched on one frame. Each is a digital waveguide:
// a delay loop whose length sets the pitch, a lowpass in the loop that eats the
// treble first (as a real string does), a chain of allpasses that make the
// partials stretch sharp (stiffness), and a comb on the output tap that puts
// the notch where the string was plucked.
//
// What makes it ONE instrument rather than eight voices is the bridge. Every
// string's output sums into a shared bridge bus, and a fraction of that bus is
// fed back into every OTHER string's loop — so striking one string sets its
// undamped neighbours ringing. That is the COUPLE control, and it is the whole
// difference between a loom and eight separate strings on eight separate
// frames. The body is a bank of modal resonances hung on the same bus.
//
// The display IS the instrument: in the PLAY tab the mouse strums across the
// strings, and mouse Y picks where along the string you caught it. The other
// tabs edit one attribute per string by height, the same idiom Beat uses.
// =============================================================================

static const int LOOM_N    = 8;
static const int LOOM_BUF  = 16384;          // ≥ 1.6× the longest loop we allow
static const int LOOM_MASK = LOOM_BUF - 1;
static const int LOOM_AP   = 4;              // dispersion sections per string
// Set from measurement, not arithmetic: this puts an ordinary pluck near 4V
// and a full strum near 5V. Eight strings bowed at once still runs into the
// rail, which is correct — that is eight self-oscillators, not eight notes.
static const float LOOM_OUT_GAIN = 12.f;

// Eight bowed strings have a crest factor a single pluck never approaches, and
// the output used to end in a hard clamp — which is the harshest thing an
// overload can do. This is linear to ±6 V and then bends, asymptotic to ±10, so
// the worst case compresses instead of tearing.
static inline float loomSoftClip(float x) {
	const float T = 6.f, K = 4.f;
	float a = std::fabs(x);
	if (a <= T) return x;
	return std::copysign(T + K * (1.f - K / (K + a - T)), x);
}
// Bow shaping. SAT is where the friction curve softly saturates (it peaks near
// 2.5, so this is active most of the cycle and its shape IS the timbre).
static const float LOOM_BOW_SAT   = 0.55f;
static const float LOOM_BOW_DRIVE = 0.12f;
// How hard the string's own velocity pushes back into the friction curve. This
// is the harshness control, and it is not obvious: high values drive the limit
// cycle CHAOTIC, filling the gaps between the partials with broadband noise at
// only -11 dB. What sounded like a bright bow was a noisy one. Dropping it from
// 12 to 1.5 takes the inter-harmonic floor to -50 dB — a clean periodic tone.
static const float LOOM_BOW_FEEDBACK = 1.5f;
static const float LOOM_BOW_HAIR     = 0.07f;
static const float LOOM_BOW_TARGET   = 0.06f;  // amplitude the regulator holds
static const float LOOM_BOW_NORM     = 0.10f;  // scale the friction curve is solved at  // string amplitude the bow holds
static const int   LOOM_BOW_OS       = 4;    // sub-steps per sample for the friction curve
static const float LOOM_DAMP_FLOOR   = 10.f; // the loop always keeps this many partials
// ^ DAMP is an absolute cutoff, which is wrong for a bank spanning octaves: at
// the dark end a high string was filtered below its own second partial, and
// with so few partials left the pluck-position comb's null (near the 4.5th)
// removed most of what remained — so a dark, high, bowed string made almost no
// sound. Ten partials is what it takes for the comb to stop gutting it.
static const float LOOM_WIND_DRIVE = 0.020f;
// A bridge transmits lows far better than highs; this is the corner.
static const float LOOM_BRIDGE_HZ = 1200.f;
// Ceiling on the fraction a string trades with the bridge. Real bridges pass a
// few percent — the first version passed 35%, which cost 89% of the sustain and
// bought no extra sympathetic ring at all.
static const float LOOM_COUPLE_MAX = 0.06f;

// Strings are tuned, not quantized — a harp is tuned to a chord. The range
// covers five octaves so the bank can span an instrument rather than a chord.
static const float LOOM_TUNE_MIN = -24.f;
static const float LOOM_TUNE_MAX =  36.f;

// EXCITE is a continuous axis, not a switch: two ways of striking, then two
// ways of sustaining, so it sweeps from a soft hit through a hard one into
// friction and finally into wind. The four are NODES on that axis — dial one
// exactly and you get it undiluted, which is what keeps the pluck and hammer
// tones intact rather than smeared into their neighbours.
enum ExciterId { EX_HAMMER, EX_PLUCK, EX_BOW, EX_AEOLIAN, EX_COUNT };
static const char* EX_NAME[EX_COUNT]  = {"HAMMER", "PLUCK", "BOW", "WIND"};
static const char* EX_SHORT[EX_COUNT] = {"HAM", "PLK", "BOW", "WND"};

static inline void loomExciteWeights(float e, float w[EX_COUNT]) {
	float p = clamp(e, 0.f, 1.f) * (float)(EX_COUNT - 1);
	int i0 = clamp((int)p, 0, EX_COUNT - 2);
	float f = p - (float)i0;
	for (int k = 0; k < EX_COUNT; k++) w[k] = 0.f;
	w[i0] = 1.f - f;
	w[i0 + 1] = f;
}

// ── register patterns ──────────────────────────────────────────────────────
// A 4-bit value stepped by two operators in turn and allowed to overflow: an
// adder wrapping in a 16-step register. The TOP three bits pick the string.
//
// The top bits and not the low ones, which is not the obvious choice. `value &
// 7` discards exactly the bit that makes the second half of a cycle differ from
// the first, so every additive chain collapses to an 8-step order played twice
// -- of 3364 chains, NONE survived that mapping. The top three bits keep the
// contour and give real 16- and 32-step orders.
//
// Only + and - reach the full range. Multiply mod 16 is invertible only for odd
// v, and 0 is its fixed point, so *odd never leaves; *even and / discard bits
// outright, so those orbits fold onto themselves. "Fold" (+3,*5) is the one
// that gets round all sixteen anyway, and the four re-plucks its folding leaves
// behind are exactly what makes it sound unlike the others.
struct LoomReg { const char* name; int n; uint8_t s[32]; };
static const LoomReg LOOM_REGS[] = {
	{"Ladder",    16, {0, 1, 3, 4, 6, 7, 1, 2, 4, 5, 7, 0, 2, 3, 5, 6}},            // +3,+3
	{"Wide wrap", 16, {0, 3, 7, 2, 6, 1, 5, 0, 4, 7, 3, 6, 2, 5, 1, 4}},            // +7,+7
	{"Zigzag",    16, {0, 5, 7, 4, 6, 3, 5, 2, 4, 1, 3, 0, 2, 7, 1, 6}},            // +11,+3
	{"Ramp pair", 16, {0, 6, 1, 7, 2, 0, 3, 1, 4, 2, 5, 3, 6, 4, 7, 5}},            // +13,+5
	{"Skip two",  16, {0, 1, 5, 6, 2, 3, 7, 0, 4, 5, 1, 2, 6, 7, 3, 4}},            // +3,+7
	{"Fold",      32, {0, 1, 7, 1, 5, 6, 0, 2, 2, 3, 1, 3, 7, 0, 2, 4,              // +3,*5
	                   4, 5, 3, 5, 1, 2, 4, 6, 6, 7, 5, 7, 3, 4, 6, 0}},
};
static const int LOOM_NREGS = (int)(sizeof(LOOM_REGS) / sizeof(LOOM_REGS[0]));

// APPEND ONLY. PATTERN_PARAM stores an index, so inserting here re-points every
// saved patch's pattern at a different one.
enum PatternId {
	PAT_UP, PAT_DOWN, PAT_UPDOWN, PAT_DOWNUP, PAT_CONVERGE, PAT_DIVERGE,
	PAT_THUMB, PAT_PAIRS, PAT_SKIP, PAT_RANDOM, PAT_WALK, PAT_STRUM,
	PAT_REG,                                    // the register patterns follow
	PAT_COUNT = PAT_REG + LOOM_NREGS
};
static const char* PAT_NAME[PAT_REG] = {
	"Up", "Down", "Up-down", "Down-up", "Converge", "Diverge",
	"Thumb", "Pairs", "Skip 3", "Random", "Walk", "Strum"
};

struct LoomTuning { const char* name; float semis[LOOM_N]; };
static const LoomTuning LOOM_TUNINGS[] = {
	{"Major pentatonic", {0,  2,  4,  7,  9, 12, 14, 16}},
	{"Minor pentatonic", {0,  3,  5,  7, 10, 12, 15, 17}},
	{"Major scale",      {0,  2,  4,  5,  7,  9, 11, 12}},
	{"Natural minor",    {0,  2,  3,  5,  7,  8, 10, 12}},
	{"Dorian",           {0,  2,  3,  5,  7,  9, 10, 12}},
	{"Major triad",      {0,  4,  7, 12, 16, 19, 24, 28}},
	{"Minor seventh",    {0,  3,  7, 10, 12, 15, 19, 22}},
	{"Fourths & fifths", {0,  5,  7, 12, 17, 19, 24, 29}},
	{"Octaves",          {0,  0, 12, 12, 24, 24, 36, 36}},
	{"Unison",           {0,  0,  0,  0,  0,  0,  0,  0}},
	{"Harmonic series",  {0, 12, 19, 24, 28, 31, 34, 36}},
	{"Whole tone",       {0,  2,  4,  6,  8, 10, 12, 14}},
	{"Chromatic",        {0,  1,  2,  3,  4,  5,  6,  7}},
};
static const int LOOM_NTUNINGS = (int)(sizeof(LOOM_TUNINGS) / sizeof(LOOM_TUNINGS[0]));

static const char* LOOM_NOTES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static std::string loomNoteName(float semisFromC4) {
	int s = (int)std::round(semisFromC4);
	int oct = 4 + (int)std::floor(s / 12.f);
	int pc = ((s % 12) + 12) % 12;
	return std::string(LOOM_NOTES[pc]) + std::to_string(oct);
}

// The delay line, the exact phase-delay maths and the filter now live in
// waveguide.hpp, shared with Slide. The loop BODY stays here — see that file.
typedef sfs::SVF LoomSVF;
static inline float loomAllpassDelay(float a, float w) { return sfs::allpassDelay(a, w); }
static inline float loomOnePoleDelay(float c, float w) { return sfs::onePoleDelay(c, w); }

// ── one string ──────────────────────────────────────────────────────────────
struct LoomString {
	sfs::DelayLine<LOOM_BUF> dl;

	float lp = 0.f;                         // loop lowpass (treble loss)
	float apX[LOOM_AP] = {}, apY[LOOM_AP] = {};
	float dcX = 0.f, dcY = 0.f;             // DC blocker, INSIDE the loop

	// excitation
	float burst = 0.f, burstLen = 1.f, burstAmp = 0.f;
	float burstMix = 0.f;                   // 1 = pure hammer shape, 0 = pure pick
	float excLp = 0.f;
	float bowLp = 0.f, bowRamp = 0.f;       // bow contact width, and its attack
	float bowEnv = 0.f, bowGain = 1.f;      // what the string is doing, and the pressure answer
	float vPrev = 0.f;                      // for oversampling the friction curve
	float dampC = 0.f;                      // loop lowpass, floored at this string's pitch
	float gustPhase = 0.f, gust = 0.f;      // wind does not blow at a constant rate
	LoomSVF aeo;                            // the narrow band the wind actually drives
	float brLp = 0.f;                       // what this string sends to the bridge
	float velocity = 1.f;
	float pickPos = 0.25f;                  // position this excitation caught it at
	float sustainTimer = 0.f;               // a bare trigger still bows, briefly
	bool  gateHeld = false;

	// strum scheduling
	float pending = -1.f, pendVel = 1.f, pendPos = -1.f;

	float dTarget = 0.f, dSm = 0.f;         // delay-line length, and its glide
	float loopSig = 0.f;                    // what the string itself is doing
	float out = 0.f;                        // what the pickup hears
	float amp = 0.f, flash = 0.f;           // display only
	float wobble = 0.f;

	void clear() {
		dl.clear();
		lp = 0.f; dcX = dcY = 0.f; excLp = 0.f;
		std::memset(apX, 0, sizeof(apX));
		std::memset(apY, 0, sizeof(apY));
		burst = 0.f; sustainTimer = 0.f; gateHeld = false;
		pending = -1.f; loopSig = out = amp = flash = 0.f;
		dTarget = dSm = 0.f;
		bowLp = bowRamp = gustPhase = brLp = vPrev = 0.f;
		bowEnv = 0.f; bowGain = 1.f;
		dampC = 0.f;
		aeo.clear();
	}

	float tap(float d) const { return dl.tap(d); }
};

struct Loom : Module {
	enum ParamId {
		BODY_PARAM, COUPLE_PARAM, DECAY_PARAM, DAMP_PARAM, PICK_PARAM, SPREAD_PARAM,
		ROOT_PARAM, OCT_PARAM,
		PATTERN_PARAM, DENSITY_PARAM,
		AUTO_PARAM, RESET_PARAM,
		// ── appended for the 2026-08 panel; do not reorder ──────────────────
		SCALE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		BODY_CV_INPUT, COUPLE_CV_INPUT, DECAY_CV_INPUT, DAMP_CV_INPUT,
		PICK_CV_INPUT, SPREAD_CV_INPUT,
		ROOT_CV_INPUT, OCT_CV_INPUT,
		VOCT_INPUT, GATE_INPUT, VEL_INPUT,
		CLOCK_INPUT, RESET_INPUT, PATTERN_CV_INPUT, DENSITY_CV_INPUT,
		ENUMS(STRING_GATE_INPUT, LOOM_N),
		// ── appended for the 2026-08 panel; do not reorder ──────────────────
		SCALE_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		MIX_L_OUTPUT, MIX_R_OUTPUT,
		ENUMS(STRING_OUTPUT, LOOM_N),
		OUTPUTS_LEN
	};
	enum LightId { AUTO_LIGHT, LIGHTS_LEN };

	// ── per-string attributes: screen-edited, so plain state + JSON (as Beat) ──
	float tune[LOOM_N]     = {};
	float decayOff[LOOM_N] = {};    // 0..1 → ×0.25 .. ×4 of the global decay
	float stiff[LOOM_N]    = {};
	float pickPos[LOOM_N]  = {};    // 0.02 .. 0.5 along the string
	float excite[LOOM_N]   = {};   // 0..1 along the exciter axis
	float level[LOOM_N]    = {};
	bool  enabled[LOOM_N]  = {};

	LoomString str[LOOM_N];
	// A string's delay line is 64kB, so the GUI never memsets one itself — it
	// asks, and the audio thread does it between samples.
	bool clearReq[LOOM_N] = {};

	// body: a soundboard's first modes, plus a broad air resonance
	static const int NBODY = 5;
	LoomSVF body[NBODY];
	float bodyFreq[NBODY] = {104.f, 217.f, 396.f, 731.f, 2380.f};
	float bodyQ[NBODY]    = {  7.f,   9.f,  10.f,   8.f,    3.f};
	float bodyW[NBODY]    = {1.00f, 0.78f, 0.62f, 0.45f, 0.30f};
	float bodySr = 0.f;
	float coupleLp = 0.f, coupleDcX = 0.f, coupleDcY = 0.f;

	// ── UI / options ───────────────────────────────────────────────────────────
	int   tab = 0;                  // 0 = PLAY, then TUNE DECAY STIFF POS EXCITE LEVEL
	int   quantScale = -1;          // -1 = free tuning; else index into sfs::SCALES
	float stereoWidth = 0.6f;
	int   mouseMode = 0;            // 0 = hover strums, 1 = click-drag only
	float internalHz = 6.f;         // auto-play fallback when CLOCK is unpatched

	// ── play state ─────────────────────────────────────────────────────────────
	dsp::SchmittTrigger gateTrig, clockTrig, resetTrig, resetBtnTrig;
	dsp::SchmittTrigger strTrig[LOOM_N];
	int   autoIdx = 0, autoWalk = 0;
	int   geomCount = 0;
	float intPhase = 0.f;

	Loom() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(BODY_PARAM,   0.f, 1.f, 0.45f, "Body resonance", "%", 0.f, 100.f);
		configParam(COUPLE_PARAM, 0.f, 1.f, 0.35f, "Sympathetic coupling", "%", 0.f, 100.f);
		configParam(DECAY_PARAM,  0.15f, 20.f, 3.5f, "Decay", " s");
		configParam(DAMP_PARAM,   0.f, 1.f, 0.55f, "Damping (treble loss)", "%", 0.f, 100.f);
		configParam(PICK_PARAM,   0.f, 1.f, 0.5f, "Pick hardness", "%", 0.f, 100.f);
		configParam(SPREAD_PARAM, -1.f, 1.f, 0.35f, "Strum spread (− up / + down)", "%", 0.f, 100.f);

		configParam(ROOT_PARAM, -12.f, 12.f, 0.f, "Root", " semitones");
		getParamQuantity(ROOT_PARAM)->snapEnabled = true;
		configParam(OCT_PARAM, -4.f, 3.f, -2.f, "Octave");
		getParamQuantity(OCT_PARAM)->snapEnabled = true;

		{
			std::vector<std::string> patNames;
			for (int i = 0; i < PAT_REG; i++) patNames.push_back(PAT_NAME[i]);
			for (int i = 0; i < LOOM_NREGS; i++) patNames.push_back(LOOM_REGS[i].name);
			configSwitch(PATTERN_PARAM, 0.f, (float)(PAT_COUNT - 1), 0.f, "Auto pattern", patNames);
		}
		configParam(DENSITY_PARAM, 0.f, 1.f, 1.f, "Auto density", "%", 0.f, 100.f);
		{
			// SCALE was context-menu only. The panel gives it a pot and CV, which
			// is what puts Loom on the ROOT/SCALE bus the rest of the set shares
			// -- one key change now reaches it like anything else. TUNING stays
			// in the menu: it reloads all eight string pitches at once, which is
			// a setup choice rather than something to sweep.
			std::vector<std::string> scaleNames{"Off (free tuning)"};
			for (int i = 0; i < sfs::NUM_SCALES; i++) scaleNames.push_back(sfs::SCALES[i].longName);
			configSwitch(SCALE_PARAM, 0.f, (float)sfs::NUM_SCALES, 0.f, "Scale", scaleNames);
			getParamQuantity(SCALE_PARAM)->snapEnabled = true;
		}
		configSwitch(AUTO_PARAM, 0.f, 1.f, 0.f, "Auto play", {"Off", "On"});
		configButton(RESET_PARAM, "Reset auto pattern");

		configInput(BODY_CV_INPUT,   "Body CV (±5V)");
		configInput(COUPLE_CV_INPUT, "Coupling CV (±5V)");
		configInput(DECAY_CV_INPUT,  "Decay CV (±5V)");
		configInput(DAMP_CV_INPUT,   "Damping CV (±5V)");
		configInput(PICK_CV_INPUT,   "Pick hardness CV (±5V)");
		configInput(SPREAD_CV_INPUT, "Strum spread CV (±5V)");
		configInput(ROOT_CV_INPUT,   "Root CV (1V/oct, semitone-quantized)");
		configInput(OCT_CV_INPUT,    "Octave CV (1V per octave)");
		configInput(VOCT_INPUT,      "V/oct — transposes the whole loom");
		configInput(GATE_INPUT,      "Gate — strums every enabled string");
		configInput(VEL_INPUT,       "Velocity (0–10V, polyphonic: channel N → string N)");
		configInput(CLOCK_INPUT,     "Clock — advances the auto pattern");
		configInput(RESET_INPUT,     "Reset the auto pattern");
		configInput(PATTERN_CV_INPUT, "Auto pattern CV (1V per pattern)");
		configInput(SCALE_CV_INPUT,  "Scale CV (1V per scale; 0V = free tuning)");
		configInput(DENSITY_CV_INPUT, "Auto density CV (±5V)");

		configOutput(MIX_L_OUTPUT, "Mix left");
		configOutput(MIX_R_OUTPUT, "Mix right");
		for (int i = 0; i < LOOM_N; i++) {
			configInput(STRING_GATE_INPUT + i, string::f("String %d gate", i + 1));
			configOutput(STRING_OUTPUT + i, string::f("String %d audio", i + 1));
		}

		resetStrings();
	}

	void resetStrings() {
		applyTuning(0);
		for (int i = 0; i < LOOM_N; i++) {
			decayOff[i] = 0.5f;
			stiff[i]    = 0.12f;
			pickPos[i]  = 0.22f;
			excite[i]   = 1.f / 3.f;       // the PLUCK node
			level[i]    = 0.8f;
			enabled[i]  = true;
			str[i].clear();
		}
	}

	// SCALE: 0 = free tuning, 1..N = an sfs::SCALES index, so the pot's first
	// position is "off" and the rest line up with the shared scale list.
	void readKeyControls() {
		int sc = (int)std::round(params[SCALE_PARAM].getValue()
		                        + inputs[SCALE_CV_INPUT].getVoltage());
		quantScale = clamp(sc, 0, sfs::NUM_SCALES) - 1;      // 0 -> -1 (off)
	}

	void applyTuning(int t) {
		t = clamp(t, 0, LOOM_NTUNINGS - 1);
		for (int i = 0; i < LOOM_N; i++)
			tune[i] = LOOM_TUNINGS[t].semis[i];
	}

	void onReset() override {
		resetStrings();
		tab = 0; quantScale = -1; stereoWidth = 0.6f; mouseMode = 0; internalHz = 6.f;
		autoIdx = autoWalk = 0; intPhase = 0.f;
		for (int k = 0; k < NBODY; k++) body[k].clear();
		coupleLp = coupleDcX = coupleDcY = 0.f;
	}

	void onSampleRateChange() override {
		bodySr = 0.f;                        // forces the body bank to re-tune
		for (int i = 0; i < LOOM_N; i++) str[i].clear();
	}

	// A string is TUNED, not quantized — but the menu can snap it to a scale.
	float tuneOf(int i) const {
		if (quantScale < 0 || quantScale >= sfs::NUM_SCALES) return tune[i];
		const sfs::Scale& sc = sfs::SCALES[quantScale];
		float best = tune[i], bestD = 1e9f;
		for (int oct = -3; oct <= 4; oct++) {
			for (int d = 0; d < sc.size; d++) {
				float cand = sc.intervals[d] + 12.f * oct;
				float dist = std::fabs(cand - tune[i]);
				if (dist < bestD) { bestD = dist; best = cand; }
			}
		}
		return best;
	}

	// ── excitation ─────────────────────────────────────────────────────────────
	void pluck(int i, float vel, float posOverride) {
		if (i < 0 || i >= LOOM_N || !enabled[i]) return;
		LoomString& s = str[i];
		s.velocity = clamp(vel, 0.03f, 1.f);
		s.pickPos  = posOverride > 0.f ? clamp(posOverride, 0.02f, 0.5f) : pickPos[i];

		float pick = paramCV(PICK_PARAM, PICK_CV_INPUT, 0.f, 1.f);
		float sr = APP->engine->getSampleRate();
		float w[EX_COUNT];
		loomExciteWeights(excite[i], w);
		float strike = w[EX_HAMMER] + w[EX_PLUCK];     // how much of a hit this is

		if (strike > 1e-4f) {
			float hw = w[EX_HAMMER] / strike;          // the hammer's share of it
			float lenH = sr * (0.0090f - 0.0068f * pick);   // a wide, soft contact
			float lenP = sr * (0.0026f - 0.0022f * pick);   // a pick is nearly a click
			s.burstLen = std::max(hw * lenH + (1.f - hw) * lenP, 3.f);
			s.burst    = s.burstLen;
			s.burstMix = hw;
			// A hammer's energy sits at and below the fundamental, where the loop
			// is nearly lossless, while a pick's is spread up into the band the
			// loop filter eats — so matched amplitudes are nowhere near matched
			// loudness, and the crossfade has to interpolate the LEVELS too.
			s.burstAmp = s.velocity * (hw * 0.14f + (1.f - hw) * 0.7f) * strike;
			s.excLp    = 0.f;
		}
		// Bow and wind are sustained exciters. A bare trigger with no held gate
		// would otherwise do nothing at all, so it buys a short stroke.
		if (strike < 0.9999f) s.sustainTimer = 0.55f;
		s.flash = 1.f;
	}

	void strum(float vel, float posOverride) {
		float spread = paramCV(SPREAD_PARAM, SPREAD_CV_INPUT, -1.f, 1.f);
		float total = std::fabs(spread) * 0.28f;              // seconds nut-to-nut
		for (int i = 0; i < LOOM_N; i++) {
			if (!enabled[i]) continue;
			int k = (spread >= 0.f) ? i : (LOOM_N - 1 - i);
			str[i].pending = total * (float)k / (float)(LOOM_N - 1);
			str[i].pendVel = vel * velFor(i);
			str[i].pendPos = posOverride;
			if (str[i].pending <= 0.f) {
				str[i].pending = -1.f;
				pluck(i, str[i].pendVel, posOverride);
			}
		}
	}

	float velFor(int i) {
		if (!inputs[VEL_INPUT].isConnected()) return 1.f;
		return clamp(inputs[VEL_INPUT].getPolyVoltage(i) / 10.f, 0.03f, 1.f);
	}

	float paramCV(int p, int in, float lo, float hi) {
		float v = params[p].getValue();
		if (in >= 0 && inputs[in].isConnected())
			v += inputs[in].getVoltage() / 5.f * (hi - lo) * 0.5f;
		return clamp(v, lo, hi);
	}

	// ── auto play ──────────────────────────────────────────────────────────────
	void autoStep() {
		int list[LOOM_N], n = 0;
		for (int i = 0; i < LOOM_N; i++)
			if (enabled[i]) list[n++] = i;
		if (n == 0) return;

		int pat = (int)std::round(params[PATTERN_PARAM].getValue()
		                          + inputs[PATTERN_CV_INPUT].getVoltage());
		pat = clamp(pat, 0, PAT_COUNT - 1);

		float density = paramCV(DENSITY_PARAM, DENSITY_CV_INPUT, 0.f, 1.f);
		float vel = clamp(0.82f + 0.14f * (2.f * random::uniform() - 1.f), 0.1f, 1.f);

		if (pat == PAT_STRUM) {
			if (random::uniform() <= density) strum(vel, -1.f);
			return;
		}

		int sel = 0;
		int period = (n > 1) ? 2 * n - 2 : 1;
		if (pat >= PAT_REG) {
			// The table is written for eight strings; % n folds it onto however
			// many are actually enabled, the same way every other pattern here
			// indexes into list[] rather than into the strings themselves.
			const LoomReg& r = LOOM_REGS[clamp(pat - PAT_REG, 0, LOOM_NREGS - 1)];
			sel = r.s[autoIdx % r.n] % n;
			autoIdx = (autoIdx + 1) & 0xFFFFF;
			if (random::uniform() <= density) pluck(list[clamp(sel, 0, n - 1)], vel, -1.f);
			return;
		}
		switch (pat) {
			case PAT_UP:       sel = autoIdx % n; break;
			case PAT_DOWN:     sel = n - 1 - (autoIdx % n); break;
			case PAT_UPDOWN:   { int p = autoIdx % period; sel = (p < n) ? p : period - p; } break;
			case PAT_DOWNUP:   { int p = autoIdx % period; int q = (p < n) ? p : period - p;
			                     sel = n - 1 - q; } break;
			case PAT_CONVERGE: { int p = autoIdx % n; sel = (p % 2 == 0) ? p / 2 : n - 1 - p / 2; } break;
			case PAT_DIVERGE:  { int p = autoIdx % n; int c = n / 2;
			                     sel = (p % 2 == 0) ? c + p / 2 : c - 1 - p / 2; } break;
			case PAT_THUMB:    sel = (autoIdx % 2 == 0) ? 0
			                       : 1 + ((autoIdx / 2) % std::max(1, n - 1)); break;
			case PAT_PAIRS:    { int p = autoIdx % n;
			                     sel = (p % 2 == 0) ? p / 2 : (p / 2 + n / 2) % n; } break;
			case PAT_SKIP:     sel = (autoIdx * 3) % n; break;
			case PAT_RANDOM:   sel = (int)(random::uniform() * n); break;
			case PAT_WALK:     autoWalk += (random::uniform() < 0.5f) ? -1 : 1;
			                   if (autoWalk < 0) autoWalk = std::min(1, n - 1);
			                   if (autoWalk >= n) autoWalk = std::max(0, n - 2);
			                   sel = autoWalk; break;
		}
		autoIdx = (autoIdx + 1) & 0xFFFFF;
		sel = clamp(sel, 0, n - 1);
		if (random::uniform() <= density) pluck(list[sel], vel, -1.f);
	}

	void process(const ProcessArgs& args) override {
		const float sr = args.sampleRate;
		readKeyControls();

		if (bodySr != sr) {
			for (int k = 0; k < NBODY; k++) body[k].set(bodyFreq[k], bodyQ[k], sr);
			bodySr = sr;
		}

		// ── global controls ────────────────────────────────────────────────────
		float bodyAmt = paramCV(BODY_PARAM,   BODY_CV_INPUT,   0.f, 1.f);
		float coupAmt = paramCV(COUPLE_PARAM, COUPLE_CV_INPUT, 0.f, 1.f);
		float dampAmt = paramCV(DAMP_PARAM,   DAMP_CV_INPUT,   0.f, 1.f);
		float pick    = paramCV(PICK_PARAM,   PICK_CV_INPUT,   0.f, 1.f);
		float decaySec = params[DECAY_PARAM].getValue();
		if (inputs[DECAY_CV_INPUT].isConnected())
			decaySec *= std::pow(2.f, inputs[DECAY_CV_INPUT].getVoltage() / 5.f);
		decaySec = clamp(decaySec, 0.05f, 40.f);

		// Loop lowpass: 0 = dark and short-lived treble, 1 = wire-bright.
		float dampHz = 420.f * std::pow(11500.f / 420.f, dampAmt);
		float excHz  = 700.f * std::pow(11000.f / 700.f, pick);
		float excC   = clamp(1.f - std::exp(-2.f * (float)M_PI * excHz / sr), 0.01f, 1.f);
		// PICK is the exciter's brightness everywhere: for the bow it is the
		// contact width, for the wind it is which partial the gusts sit on.
		float bowHz  = 900.f * std::pow(3200.f / 900.f, pick);
		float bowC   = clamp(1.f - std::exp(-2.f * (float)M_PI * bowHz / sr), 0.01f, 1.f);
		float bowAtk = 1.f - std::exp(-args.sampleTime / 0.045f);   // ~45ms to speak
		float bowEnvC = 1.f - std::exp(-args.sampleTime / 0.030f);
		float bowAgcC = args.sampleTime / 0.12f;                    // pressure follows slowly
		float brC    = clamp(1.f - std::exp(-2.f * (float)M_PI * LOOM_BRIDGE_HZ / sr), 0.01f, 1.f);
		float windBand = 3.f + 5.f * pick;      // which partial the wind excites

		// ── pitch ──────────────────────────────────────────────────────────────
		float rootSemis = params[ROOT_PARAM].getValue();
		if (inputs[ROOT_CV_INPUT].isConnected())
			rootSemis += std::round(inputs[ROOT_CV_INPUT].getVoltage() * 12.f);
		float oct = params[OCT_PARAM].getValue();
		if (inputs[OCT_CV_INPUT].isConnected()) oct += inputs[OCT_CV_INPUT].getVoltage();
		float basePitch = oct + rootSemis / 12.f + inputs[VOCT_INPUT].getVoltage();

		// ── triggers ───────────────────────────────────────────────────────────
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)
		    || resetBtnTrig.process(params[RESET_PARAM].getValue() > 0.5f ? 10.f : 0.f, 0.1f, 1.f)) {
			autoIdx = 0; autoWalk = 0; intPhase = 0.f;
		}

		bool globalGate = inputs[GATE_INPUT].getVoltage() >= 1.f;
		if (gateTrig.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f))
			strum(1.f, -1.f);

		bool autoOn = params[AUTO_PARAM].getValue() > 0.5f;
		lights[AUTO_LIGHT].setBrightness(autoOn ? 1.f : 0.f);
		if (autoOn) {
			bool tick = false;
			if (inputs[CLOCK_INPUT].isConnected()) {
				tick = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
			}
			else {
				intPhase += args.sampleTime * internalHz;
				if (intPhase >= 1.f) { intPhase -= 1.f; tick = true; }
			}
			if (tick) autoStep();
		}

		// Mouse plucks land on the GUI thread; drain them here so a strum never
		// tries to touch the delay lines mid-sample.
		int mqr = mouseQueueRead.load(std::memory_order_relaxed);
		while (mqr != mouseQueueWrite.load(std::memory_order_acquire)) {
			MouseHit h = mouseQueue[mqr];
			mqr = (mqr + 1) % MOUSE_Q;
			mouseQueueRead.store(mqr, std::memory_order_release);
			pluck(h.string, h.vel, h.pos);
		}

		// ── per-string voice ───────────────────────────────────────────────────
		float bridge = 0.f;
		float couplePrev = coupleBus;               // last sample's bus: the delay
		                                            // is what keeps this stable
		float mixL = 0.f, mixR = 0.f;
		// What the bridge feels is the string's MOTION, not what the pickup
		// hears: the pickup signal has been through the pluck-position comb,
		// which peaks at +6 dB, and putting that gain inside the coupling
		// feedback is what made every large setting howl.
		float motion = 0.f;
		geomCount++;

		for (int i = 0; i < LOOM_N; i++) {
			LoomString& s = str[i];
			if (clearReq[i]) { s.clear(); clearReq[i] = false; }

			// per-string gate
			float gv = inputs[STRING_GATE_INPUT + i].getVoltage();
			if (strTrig[i].process(gv, 0.1f, 1.f)) pluck(i, velFor(i), -1.f);
			s.gateHeld = enabled[i] && (globalGate || gv >= 1.f);
			if (s.sustainTimer > 0.f) s.sustainTimer -= args.sampleTime;

			// scheduled strum arrival
			if (s.pending >= 0.f) {
				s.pending -= args.sampleTime;
				if (s.pending <= 0.f) { s.pending = -1.f; pluck(i, s.pendVel, s.pendPos); }
			}

			if (!enabled[i]) {
				s.out = 0.f;
				s.amp *= 0.90f;
				s.flash *= 0.88f;
				outputs[STRING_OUTPUT + i].setVoltage(0.f);
				continue;
			}

			// ── geometry of the loop ───────────────────────────────────────────
			float semis = basePitch * 12.f + tuneOf(i);
			float freq = dsp::FREQ_C4 * std::pow(2.f, semis / 12.f);
			freq = clamp(freq, 20.f, std::min(8000.f, sr * 0.24f));

			float b = stiff[i] * 0.42f;
			float apC = -b;
			// Recomputing the exact filter delay costs three atan2s, so it runs
			// every 32 samples with the strings staggered, and `d` glides to it.
			if (((geomCount + i) & 31) == 0 || s.dSm <= 0.f) {
				float w = 2.f * (float)M_PI * freq / sr;
				// DAMP is an absolute cutoff, which is wrong for a bank of strings
				// spanning octaves: at the dark end a 262 Hz string was filtered
				// BELOW its own second partial, so it could barely sustain and the
				// exciter's own noise dominated it. Flooring the cutoff at a
				// multiple of the string's OWN pitch fixes the top of the range
				// and leaves everything at or above the default untouched.
				// Each string sits in its own eddy, so they must not gust together.
				s.gustPhase += 32.f / sr;
				float ph = s.gustPhase + 1.37f * (float)i;
				s.gust = clamp(0.5f + 0.5f * std::sin(2.f * (float)M_PI * 0.19f * ph)
				                          * std::sin(2.f * (float)M_PI * 0.067f * ph)
				               + 0.18f * std::sin(2.f * (float)M_PI * 0.041f * ph),
				               0.f, 1.f);
				float dHz = std::max(dampHz, freq * LOOM_DAMP_FLOOR);
				s.dampC = clamp(1.f - std::exp(-2.f * (float)M_PI * dHz / sr), 0.02f, 0.999f);
				s.dTarget = clamp(sr / freq
				                  - LOOM_AP * loomAllpassDelay(apC, w)
				                  - loomOnePoleDelay(s.dampC, w),
				                  8.f, (float)LOOM_BUF / 1.62f);
				if (s.dSm <= 0.f) s.dSm = s.dTarget;
				// Vortex shedding frequency rises with wind speed (Strouhal), so
				// the band the wind drives MOVES as the gust swells — and the harp
				// climbs and falls between the string's partials. That wandering
				// is the whole character of an aeolian harp; a fixed band just
				// sounds like a differently-filtered bow.
				float band = windBand * (0.45f + 1.10f * s.gust);
				s.aeo.set(freq * band, 9.f, sr);
			}
			s.dSm += (s.dTarget - s.dSm) * 0.004f;
			float d = s.dSm;

			// Loop gain for a T60: the round trip happens `freq` times a second.
			float t60 = decaySec * std::pow(4.f, (decayOff[i] - 0.5f) * 2.f);
			float g = std::exp(-6.907755f / (freq * std::max(t60, 0.02f)));
			g = std::min(g, 0.99995f);

			// ── read ───────────────────────────────────────────────────────────
			float v = s.tap(d);
			s.loopSig = v;
			// The pluck-position notch, taken on the OUTPUT tap so the loop stays
			// a plain delay: y = x(t-d) − x(t-d-pos·d) is the same comb a pick at
			// `pos` along the string puts in the spectrum.
			float combD = std::min(d * (1.f + s.pickPos), (float)LOOM_BUF - 4.f);
			s.out = v - s.tap(combD);

			// ── excitation ─────────────────────────────────────────────────────
			float exc = 0.f;
			bool sustaining = s.gateHeld || s.sustainTimer > 0.f;
			float exw[EX_COUNT];
			loomExciteWeights(excite[i], exw);

			if (s.burst > 0.f) {
				float t = 1.f - s.burst / s.burstLen;
				float win = 0.5f - 0.5f * std::cos(2.f * (float)M_PI * t);
				// A hammer imparts a velocity pulse, which is bipolar — a plain
				// one-sided bump is a DC step, and a delay loop passes DC at very
				// nearly unity, so it came out roughly ten times louder than a
				// pluck and rang on the fundamental for seconds.
				// The three branches are not just an optimisation: at the pure
				// nodes they draw exactly as many random numbers as the
				// single-exciter version did, so a pluck is bit-for-bit the
				// pluck that was already right.
				float raw;
				if (s.burstMix <= 0.f)
					raw = win * (2.f * random::uniform() - 1.f);
				else if (s.burstMix >= 1.f)
					raw = win * (std::sin(2.f * (float)M_PI * t)
					             + 0.12f * (2.f * random::uniform() - 1.f));
				else {
					float ham = win * (std::sin(2.f * (float)M_PI * t)
					                   + 0.12f * (2.f * random::uniform() - 1.f));
					float plk = win * (2.f * random::uniform() - 1.f);
					raw = s.burstMix * ham + (1.f - s.burstMix) * plk;
				}
				s.excLp += excC * (raw - s.excLp);
				exc = s.excLp * s.burstAmp;
				s.burst -= 1.f;
			}

			if (sustaining && exw[EX_BOW] > 1e-4f) {
				// Smith's friction curve. The string sticks to the bow until the
				// relative velocity is large enough to slip, and it is that
				// stick-slip alternation — not the steady force — that sustains
				// the tone. The four constants were swept against the real loop:
				// a shallow curve or a weak drive settles at a stable DC
				// operating point and makes no sound at all, and the wrong
				// balance locks the string into its sub-octave.
				s.bowRamp += (1.f - s.bowRamp) * bowAtk;    // bows do not start instantly
				// Bow speed sets how LOUD, not whether it speaks: the drive stays
				// above the threshold at every velocity and the level is scaled
				// after the loop instead. A drive that scaled with velocity left
				// gentle bowing silent.
				// The hair noise is not decoration. Without it the string locks
				// into its sub-octave at the top of the range — a real bifurcation,
				// insensitive to drive level — and real bow hair is not uniform.
				// A friction model's equilibrium amplitude is not controlled by
				// anything: measured at steady state it was silent on some strings
				// and pinned to the output clamp on others, and it took as long as
				// six seconds to get there. A player does not accept that — they
				// lean on the bow until the note sits where they want it. So the
				// bow watches the string and answers with pressure. This is what
				// makes the level the same on every string, at every DAMP and
				// DECAY, instead of only near the settings it was tuned at.
				s.bowEnv += (std::fabs(v) - s.bowEnv) * bowEnvC;
				float target = LOOM_BOW_TARGET * (0.25f + 0.75f * s.velocity);
				s.bowGain = clamp(s.bowGain + (target - s.bowEnv) * bowAgcC,
				                  0.02f, 24.f);
				// The regulator drives FORCE, not bow speed. The friction curve is
				// non-monotonic in speed — it peaks and then falls away — so a
				// controller pushing on speed can push straight past the peak and
				// lose the string. Force scales the curve's output and is
				// monotonic, which is what makes the loop behave.
				float drive = (0.30f + 0.20f * s.velocity) * s.bowRamp
				            + LOOM_BOW_HAIR * (2.f * random::uniform() - 1.f);
				// The slip transition is a corner, and at 500 Hz there are only
				// ~90 samples in a period to resolve it — so the curve throws
				// harmonics far above Nyquist and they fold back down as noise.
				// That, not brightness, is what made a high bowed string harsh:
				// at 192 kHz the very same note measures 25 dB cleaner. Nothing
				// downstream can fix it, because the aliases are already inside
				// the band, so the curve is evaluated OVERSAMPLED and averaged.
				float ff = 0.f;
				for (int o = 1; o <= LOOM_BOW_OS; o++) {
					float vo = s.vPrev + (v - s.vPrev) * ((float)o / (float)LOOM_BOW_OS);
					// Normalising by the string's own envelope is what makes the
					// bow behave the same on every string. Otherwise the friction
					// curve sees a signal whose scale depends on pitch, decay and
					// damping, and its operating point — hence the whole character
					// — drifts with them. This is the difference between a bow
					// that is right where it was tuned and one that is right
					// everywhere.
					float vn = vo * LOOM_BOW_NORM / std::max(s.bowEnv, 1e-4f);
					float vd = drive - vn * LOOM_BOW_FEEDBACK;
					float a = std::fabs(vd) + 0.35f;
					float a2 = a * a;
					float f1 = vd / (a2 * a2);
					// A soft knee, not a clamp. The curve peaks near 2.5 and the
					// old hard limit at 0.9 was active most of the cycle, so the
					// limit cycle was squared off — and a square wave IS harshness.
					ff += LOOM_BOW_SAT * f1 / (LOOM_BOW_SAT + std::fabs(f1));
				}
				ff *= 1.f / (float)LOOM_BOW_OS;
				// Finite bow width: the force is a spatial average over the hair
				// in contact, which is a lowpass. Without it the string is driven
				// broadband and screams.
				s.bowLp += bowC * (ff - s.bowLp);
				exc += s.bowLp * LOOM_BOW_DRIVE * s.bowGain * exw[EX_BOW];
			}
			else s.bowRamp *= 0.9995f;

			if (sustaining && exw[EX_AEOLIAN] > 1e-4f) {
				// Wind, not hiss. An aeolian harp is played by vortices shedding
				// off the string, which drive a NARROW band — and the instrument
				// sings on the string's upper partials, which is why it sounds
				// like a flute rather than a bowed string. Feeding it broadband
				// noise, as the first version did, is just noise through a comb.
				float n = 2.f * random::uniform() - 1.f;
				// Wind swells and drops away; the square makes the lulls real
				// lulls rather than a constant breeze at varying loudness.
				exc += s.aeo.bandpass(n) * LOOM_WIND_DRIVE * s.velocity
				       * (0.12f + 0.88f * s.gust * s.gust) * exw[EX_AEOLIAN];
			}

			// ── the loop ───────────────────────────────────────────────────────
			s.lp += s.dampC * (v - s.lp);
			float x = g * s.lp;
			for (int k = 0; k < LOOM_AP; k++) {
				float y = apC * x + s.apX[k] - apC * s.apY[k];
				s.apX[k] = x; s.apY[k] = y; x = y;
			}
			// DC blocker sits INSIDE the loop — on the input it would do nothing,
			// because it is the recirculation that accumulates the offset.
			float dy = x - s.dcX + 0.99985f * s.dcY;
			s.dcX = x; s.dcY = dy; x = dy;

			// Sympathetic bleed through the bridge. This MIXES rather than adds:
			// what a string takes from the bridge it gives up out of its own
			// loop. That is what a real bridge does — energy leaving a string is
			// energy that string no longer has — and it is also the only thing
			// keeping eight resonators with a loop gain of 0.999 from howling
			// the moment they are joined. Injecting without the matching loss
			// has no stable setting: it is inaudible until it runs away.
			// What this string sends to the bridge. A real bridge passes lows
			// far better than highs, and making the transmission band-limited is
			// what stops COUPLE reading as a plain dampener: the string gives up
			// its low end and keeps its brightness.
			s.brLp += brC * (v - s.brLp);
			// Sympathetic exchange through the bridge. Subtracting exactly what
			// this string contributed and adding back the bank's mean means the
			// in-phase mode is preserved (gain 1) while everything else is damped
			// only in the transmitted band — so it is unconditionally stable
			// without costing the sustain across the whole spectrum.
			float k = coupAmt * coupAmt * LOOM_COUPLE_MAX;
			x += k * (couplePrev - s.brLp);
			x += exc;

			if (x > 1.6f) x = 1.6f; else if (x < -1.6f) x = -1.6f;
			s.vPrev = v;
			s.dl.write(x);

			// ── mix ────────────────────────────────────────────────────────────
			float y = s.out * level[i];
			bridge += y;
			motion += s.brLp;

			float p = ((float)i / (float)(LOOM_N - 1) * 2.f - 1.f) * stereoWidth;
			float th = (p + 1.f) * (float)M_PI_4;
			mixL += y * std::cos(th);
			mixR += y * std::sin(th);

			outputs[STRING_OUTPUT + i].setVoltage(loomSoftClip(y * LOOM_OUT_GAIN));

			// display envelope: fast attack, slow release
			float a = std::fabs(s.out);
			s.amp += (a > s.amp ? 0.02f : 0.0009f) * (a - s.amp);
			s.flash *= 0.9994f;
			s.wobble += args.sampleTime;
		}

		// ── bridge, body, coupling bus ─────────────────────────────────────────
		float bodyOut = 0.f;
		for (int k = 0; k < NBODY; k++)
			bodyOut += body[k].bandpass(bridge) * bodyW[k];
		bodyOut *= 0.42f;

		// The coupling medium is the BRIDGE, not the body. That is how a real
		// instrument works — strings talk through the bridge whether or not the
		// box behind it resonates — and it is also what makes the feedback safe:
		// the body's modes have a gain of Q at resonance, so routing them back
		// into eight high-Q strings guarantees a howl at the body's own pitches.
		// The bus carries the MEAN string motion, so a string's own share of it
		// is 1/8 and cannot outweigh the (1 − k) it gave up to be there.
		// Already band-limited per string, so the bus only needs the DC guard.
		float cin = motion / (float)LOOM_N;
		float cdy = cin - coupleDcX + 0.9993f * coupleDcY;
		coupleDcX = cin; coupleDcY = cdy;
		coupleBus = clamp(cdy, -3.f, 3.f);

		float wetL = mixL + bodyOut * bodyAmt;
		float wetR = mixR + bodyOut * bodyAmt;
		outputs[MIX_L_OUTPUT].setVoltage(loomSoftClip(wetL * LOOM_OUT_GAIN));
		outputs[MIX_R_OUTPUT].setVoltage(loomSoftClip(wetR * LOOM_OUT_GAIN));
	}

	float coupleBus = 0.f;

	// ── mouse plucks: GUI thread → audio thread ───────────────────────────────
	struct MouseHit { int string; float vel; float pos; };
	static const int MOUSE_Q = 32;
	MouseHit mouseQueue[MOUSE_Q] = {};
	// Single producer (the GUI), single consumer (the audio thread). The release
	// on the write index is what guarantees the audio thread sees a fully
	// written MouseHit and not half of one.
	std::atomic<int> mouseQueueRead{0}, mouseQueueWrite{0};

	void mousePluck(int i, float vel, float pos) {
		int w = mouseQueueWrite.load(std::memory_order_relaxed);
		int nw = (w + 1) % MOUSE_Q;
		if (nw == mouseQueueRead.load(std::memory_order_acquire))
			return;                                  // full: drop, never block audio
		mouseQueue[w] = {i, vel, pos};
		mouseQueueWrite.store(nw, std::memory_order_release);
	}

	// ── persistence ────────────────────────────────────────────────────────────
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "tab", json_integer(tab));
		json_object_set_new(root, "quantScale", json_integer(quantScale));
		json_object_set_new(root, "stereoWidth", json_real(stereoWidth));
		json_object_set_new(root, "mouseMode", json_integer(mouseMode));
		json_object_set_new(root, "internalHz", json_real(internalHz));

		json_t* sa = json_array();
		for (int i = 0; i < LOOM_N; i++) {
			json_t* s = json_object();
			json_object_set_new(s, "tune", json_real(tune[i]));
			json_object_set_new(s, "decay", json_real(decayOff[i]));
			json_object_set_new(s, "stiff", json_real(stiff[i]));
			json_object_set_new(s, "pos", json_real(pickPos[i]));
			json_object_set_new(s, "excite", json_real(excite[i]));
			json_object_set_new(s, "level", json_real(level[i]));
			json_object_set_new(s, "enabled", json_boolean(enabled[i]));
			json_array_append_new(sa, s);
		}
		json_object_set_new(root, "strings", sa);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "tab")) tab = clamp((int)json_integer_value(j), 0, 6);
		if (json_t* j = json_object_get(root, "quantScale"))
			quantScale = clamp((int)json_integer_value(j), -1, sfs::NUM_SCALES - 1);
		if (json_t* j = json_object_get(root, "stereoWidth")) stereoWidth = json_number_value(j);
		if (json_t* j = json_object_get(root, "mouseMode")) mouseMode = (int)json_integer_value(j);
		if (json_t* j = json_object_get(root, "internalHz")) internalHz = json_number_value(j);

		json_t* sa = json_object_get(root, "strings");
		if (!sa) return;
		for (int i = 0; i < LOOM_N && i < (int)json_array_size(sa); i++) {
			json_t* s = json_array_get(sa, i);
			if (json_t* j = json_object_get(s, "tune"))
				tune[i] = clamp((float)json_number_value(j), LOOM_TUNE_MIN, LOOM_TUNE_MAX);
			if (json_t* j = json_object_get(s, "decay")) decayOff[i] = json_number_value(j);
			if (json_t* j = json_object_get(s, "stiff")) stiff[i] = json_number_value(j);
			if (json_t* j = json_object_get(s, "pos")) pickPos[i] = json_number_value(j);
			if (json_t* j = json_object_get(s, "excite"))
				excite[i] = clamp((float)json_number_value(j), 0.f, 1.f);
			if (json_t* j = json_object_get(s, "level")) level[i] = json_number_value(j);
			if (json_t* j = json_object_get(s, "enabled")) enabled[i] = json_boolean_value(j);
		}
	}
};


// =============================================================================
// Display — the instrument itself.
// =============================================================================

static const int LOOM_NTABS = 7;
static const char* LOOM_TABS[LOOM_NTABS] =
	{"PLAY", "TUNE", "DECAY", "STIFF", "POS", "EXCITE", "LEVEL"};

struct LoomLayout {
	float w = 0.f, h = 0.f;
	float tabY = 0.f, tabH = 0.f, tabX0 = 0.f, tabW = 0.f;
	float nutY = 0.f, bridgeY = 0.f, dotY = 0.f, footY = 0.f;
	// The eight strings sit over the eight jack columns on the panel below: the
	// display is 20 cells wide with the columns at cells 3, 5 ... 17.
	float sx(int i) const { return w * (3.f + 2.f * (float)i) / 20.f; }
	float vy(float v) const { return bridgeY - clamp(v, 0.f, 1.f) * (bridgeY - nutY); }
};

struct LoomDisplay : OpaqueWidget {
	Loom* module = nullptr;
	std::shared_ptr<Font> font;

	int   dragString = -1;
	Vec   dragPos;
	bool  dragIsPlay = false;
	float phase = 0.f;

	LoomLayout layout() const {
		LoomLayout L;
		L.w = box.size.x; L.h = box.size.y;
		L.tabY = L.h * 0.022f;  L.tabH = L.h * 0.098f;
		L.tabX0 = L.w * 0.030f; L.tabW = (L.w * 0.940f) / (float)LOOM_NTABS;
		L.nutY = L.h * 0.180f;  L.bridgeY = L.h * 0.800f;
		L.dotY = L.h * 0.858f;  L.footY = L.h * 0.945f;
		return L;
	}

	int tabHit(const LoomLayout& L, float x) const {
		int t = (int)std::floor((x - L.tabX0) / L.tabW);
		return (t >= 0 && t < LOOM_NTABS) ? t : -1;
	}

	int stringHit(const LoomLayout& L, float x) const {
		for (int i = 0; i < LOOM_N; i++)
			if (std::fabs(x - L.sx(i)) < L.w * 0.055f) return i;
		return -1;
	}

	// ── attribute read/write ──────────────────────────────────────────────────
	float attrOf(int i) const {
		if (!module) return 0.5f;
		switch (module->tab) {
			case 1: return (module->tune[i] - LOOM_TUNE_MIN) / (LOOM_TUNE_MAX - LOOM_TUNE_MIN);
			case 2: return module->decayOff[i];
			case 3: return module->stiff[i];
			case 4: return (module->pickPos[i] - 0.02f) / 0.48f;
			case 5: return module->excite[i];
			case 6: return module->level[i];
		}
		return 0.f;
	}

	void setAttr(int i, float v) {
		if (!module) return;
		v = clamp(v, 0.f, 1.f);
		switch (module->tab) {
			case 1: module->tune[i] = std::round(LOOM_TUNE_MIN
			            + v * (LOOM_TUNE_MAX - LOOM_TUNE_MIN)); break;
			case 2: module->decayOff[i] = v; break;
			case 3: module->stiff[i] = v; break;
			case 4: module->pickPos[i] = 0.02f + v * 0.48f; break;
			case 5: module->excite[i] = v; break;
			case 6: module->level[i] = v; break;
		}
	}

	std::string footText(int i) const {
		if (!module) return "";
		switch (module->tab) {
			case 2: return string::f("%.0f%%", module->decayOff[i] * 100.f);
			case 3: return string::f("%.0f%%", module->stiff[i] * 100.f);
			case 4: return string::f("%.0f%%", module->pickPos[i] * 200.f);
			case 5: {
				float w[EX_COUNT];
				loomExciteWeights(module->excite[i], w);
				int a = -1, b = -1;
				for (int k = 0; k < EX_COUNT; k++)
					if (w[k] > 0.f) { if (a < 0) a = k; else b = k; }
				if (a < 0) return "";
				if (b < 0 || w[b] < 0.10f) return EX_SHORT[a];
				if (w[a] < 0.10f) return EX_SHORT[b];
				return std::string(EX_SHORT[a]) + "\u00b7" + EX_SHORT[b];
			}
			case 6: return string::f("%.0f%%", module->level[i] * 100.f);
		}
		// PLAY and TUNE both want to know what note the string is
		float semis = module->params[Loom::OCT_PARAM].getValue() * 12.f
		            + module->params[Loom::ROOT_PARAM].getValue()
		            + module->tuneOf(i);
		return loomNoteName(semis);
	}

	// ── interaction ───────────────────────────────────────────────────────────
	// Nothing here consumes an event it has no use for: a click that hits no
	// string falls through to the panel, and onHoverScroll is deliberately NOT
	// overridden so the wheel always reaches the rack.
	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) {
			OpaqueWidget::onButton(e);
			return;
		}
		LoomLayout L = layout();
		if (e.pos.y < L.tabY + L.tabH) {
			int t = tabHit(L, e.pos.x);
			if (t >= 0) { module->tab = t; e.consume(this); }
			return;
		}
		// the enable dots, just under the bridge
		if (std::fabs(e.pos.y - L.dotY) < L.h * 0.035f) {
			int s = stringHit(L, e.pos.x);
			if (s >= 0) {
				module->enabled[s] = !module->enabled[s];
				module->clearReq[s] = true;
				e.consume(this);
			}
			return;
		}
		if (e.pos.y < L.nutY - L.h * 0.02f || e.pos.y > L.bridgeY + L.h * 0.02f) return;

		int s = stringHit(L, e.pos.x);
		if (s < 0) return;
		e.consume(this);
		dragPos = e.pos;
		dragString = s;
		dragIsPlay = (module->tab == 0);
		if (dragIsPlay) module->mousePluck(s, 0.8f, pickFromY(L, e.pos.y));
		else            setAttr(s, valueFromY(L, e.pos.y));
	}

	float valueFromY(const LoomLayout& L, float y) const {
		return clamp((L.bridgeY - y) / (L.bridgeY - L.nutY), 0.f, 1.f);
	}

	// Where along the string the mouse caught it. Near either end is a hard,
	// nasal attack; the middle is mellow — which is exactly how a real string
	// behaves, so the gesture teaches the control.
	float pickFromY(const LoomLayout& L, float y) const {
		float t = clamp((y - L.nutY) / std::max(L.bridgeY - L.nutY, 1.f), 0.f, 1.f);
		return 0.03f + 0.47f * (1.f - std::fabs(1.f - 2.f * t));
	}

	void crossStrings(const LoomLayout& L, Vec pos, Vec delta) {
		if (!module) return;
		if (pos.y < L.nutY - 4.f || pos.y > L.bridgeY + 4.f) return;
		float speed = std::fabs(delta.x);
		if (speed < 0.35f) return;
		float x1 = pos.x, x0 = pos.x - delta.x;
		float lo = std::min(x0, x1), hi = std::max(x0, x1);
		float vel = clamp(0.22f + speed / 20.f, 0.12f, 1.f);
		float pk = pickFromY(L, pos.y);
		for (int i = 0; i < LOOM_N; i++) {
			float sx = L.sx(i);
			if (sx > lo && sx <= hi) module->mousePluck(i, vel, pk);
		}
	}

	void onHover(const HoverEvent& e) override {
		OpaqueWidget::onHover(e);            // consumed so we keep receiving moves
		if (!module || module->tab != 0 || module->mouseMode != 0) return;
		crossStrings(layout(), e.pos, e.mouseDelta);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || dragString < 0) return;
		float zoom = std::max(getAbsoluteZoom(), 0.01f);
		Vec d = e.mouseDelta.div(zoom);
		dragPos = dragPos.plus(d);
		LoomLayout L = layout();
		if (dragIsPlay) crossStrings(L, dragPos, d);
		else {
			// drag paints sideways and adjusts vertically at the same time
			int s = stringHit(L, dragPos.x);
			if (s >= 0) dragString = s;
			setAttr(dragString, valueFromY(L, dragPos.y));
		}
	}

	void onDragEnd(const DragEndEvent& e) override { dragString = -1; }

	void step() override {
		phase += 0.28f;
		if (phase > 1e6f) phase = 0.f;
		OpaqueWidget::step();
	}

	// ── drawing ───────────────────────────────────────────────────────────────
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!font || font->handle < 0) font = sfs::panelFont();
		if (!font || font->handle < 0) return;

		nvgSave(args.vg);
		nvgScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		if (!module) drawPreview(args);
		else         drawLive(args);
		nvgRestore(args.vg);
	}

	void drawTabs(const DrawArgs& args, const LoomLayout& L, int sel) {
		NVGcontext* vg = args.vg;
		for (int t = 0; t < LOOM_NTABS; t++) {
			float x = L.tabX0 + t * L.tabW;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x + 1.f, L.tabY, L.tabW - 2.f, L.tabH, 2.f);
			nvgFillColor(vg, t == sel ? sfs::SCREEN_DEEP : nvgRGB(0x24, 0x24, 0x3C));
			nvgFill(vg);

			sfs::screenFont(vg, font);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, t == sel ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM);
			nvgText(vg, x + L.tabW * 0.5f, L.tabY + L.tabH * 0.55f, LOOM_TABS[t], NULL);
		}
	}

	void drawFrame(const DrawArgs& args, const LoomLayout& L) {
		NVGcontext* vg = args.vg;
		float x0 = L.sx(0) - L.w * 0.055f, x1 = L.sx(LOOM_N - 1) + L.w * 0.055f;
		for (int k = 0; k < 2; k++) {
			float y = k ? L.bridgeY : L.nutY;
			nvgBeginPath(vg);
			nvgMoveTo(vg, x0, y);
			nvgLineTo(vg, x1, y);
			nvgStrokeColor(vg, sfs::SCREEN_LINE);
			nvgStrokeWidth(vg, k ? 2.2f : 1.4f);      // the bridge is the heavy one
			nvgStroke(vg);
		}
	}

	// One string, drawn as it is actually behaving: displacement peaks at the
	// middle and goes to zero at both ends, because that is where it is clamped.
	void drawString(const DrawArgs& args, const LoomLayout& L, int i,
	                float amp, float flash, bool on, float thickness, float ph) {
		NVGcontext* vg = args.vg;
		float x = L.sx(i);
		float span = L.bridgeY - L.nutY;
		float A = std::min(amp, 1.f) * L.w * 0.020f;

		NVGcolor col = on ? sfs::SCREEN_BLUE : sfs::SCREEN_PURP;
		if (on && flash > 0.01f)
			col = nvgLerpRGBA(col, sfs::SCREEN_HOT, clamp(flash, 0.f, 1.f));

		nvgBeginPath(vg);
		if (A < 0.25f) {
			nvgMoveTo(vg, x, L.nutY);
			nvgLineTo(vg, x, L.bridgeY);
		}
		else {
			const int SEG = 22;
			for (int k = 0; k <= SEG; k++) {
				float t = (float)k / (float)SEG;
				float env = std::sin((float)M_PI * t);
				float dx = A * env * std::sin(ph + t * (float)M_PI * 2.f);
				float y = L.nutY + t * span;
				if (k == 0) nvgMoveTo(vg, x + dx, y); else nvgLineTo(vg, x + dx, y);
			}
		}
		nvgStrokeColor(vg, col);
		nvgStrokeWidth(vg, thickness);
		nvgStroke(vg);

		// the envelope the string is sweeping out, so loud strings read as loud
		if (A > 0.6f) {
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, L.nutY);
			nvgQuadTo(vg, x + A * 1.7f, L.nutY + span * 0.5f, x, L.bridgeY);
			nvgQuadTo(vg, x - A * 1.7f, L.nutY + span * 0.5f, x, L.nutY);
			nvgFillColor(vg, nvgTransRGBA(col, 34));
			nvgFill(vg);
		}
	}

	void drawEnableDot(const DrawArgs& args, const LoomLayout& L, int i, bool on) {
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		nvgCircle(vg, L.sx(i), L.dotY, L.h * 0.016f);
		if (on) { nvgFillColor(vg, sfs::SCREEN_BLUE); nvgFill(vg); }
		else {
			nvgStrokeColor(vg, sfs::SCREEN_PMID);
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
		}
	}

	void drawMarker(const DrawArgs& args, const LoomLayout& L, int i, float v, bool on) {
		NVGcontext* vg = args.vg;
		float x = L.sx(i), y = L.vy(v), hw = L.w * 0.038f;
		nvgBeginPath(vg);
		nvgRect(vg, x - hw, y, hw * 2.f, L.bridgeY - y);
		nvgFillColor(vg, nvgTransRGBA(on ? sfs::SCREEN_DEEP : sfs::SCREEN_PURP, 130));
		nvgFill(vg);

		nvgBeginPath(vg);
		nvgMoveTo(vg, x - hw, y);
		nvgLineTo(vg, x + hw, y);
		nvgStrokeColor(vg, on ? sfs::SCREEN_HOT : sfs::SCREEN_PMID);
		nvgStrokeWidth(vg, 2.f);
		nvgStroke(vg);
	}

	void drawFoot(const DrawArgs& args, const LoomLayout& L, int i,
	              const std::string& t, bool on) {
		NVGcontext* vg = args.vg;
		sfs::screenFont(vg, font);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, on ? sfs::SCREEN_TEXT : sfs::SCREEN_PMID);
		nvgText(vg, L.sx(i), L.footY, t.c_str(), NULL);
	}

	// In EXCITE the string's height picks one of four bands, so the bands have
	// to be named or the gesture is a guess.
	void drawExciteLegend(const DrawArgs& args, const LoomLayout& L) {
		NVGcontext* vg = args.vg;
		float span = L.bridgeY - L.nutY;
		nvgFontFaceId(vg, font->handle);
		nvgFontSize(vg, mm2px(sfs::TYPE_SCREEN_SMALL));
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		for (int e = 0; e < EX_COUNT; e++) {
			float y = L.bridgeY - span * (float)e / (float)(EX_COUNT - 1);
			nvgBeginPath(vg);
			nvgMoveTo(vg, L.w * 0.030f, y + L.h * 0.035f);
			nvgLineTo(vg, L.w * 0.125f, y + L.h * 0.035f);
			nvgStrokeColor(vg, sfs::SCREEN_PURP);
			nvgStrokeWidth(vg, 0.8f);
			nvgStroke(vg);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, L.w * 0.030f, y, EX_NAME[e], NULL);
		}
	}

	void drawLive(const DrawArgs& args) {
		LoomLayout L = layout();
		drawTabs(args, L, module->tab);
		drawFrame(args, L);
		if (module->tab == 5) drawExciteLegend(args, L);

		for (int i = 0; i < LOOM_N; i++) {
			bool on = module->enabled[i];
			// low strings are visibly fatter, the way a real course is strung
			float pitchN = clamp((module->tuneOf(i) - LOOM_TUNE_MIN)
			                     / (LOOM_TUNE_MAX - LOOM_TUNE_MIN), 0.f, 1.f);
			float th = 0.9f + 2.3f * (1.f - pitchN);

			if (module->tab > 0) drawMarker(args, L, i, attrOf(i), on);
			drawString(args, L, i, module->str[i].amp * 3.2f,
			           module->str[i].flash, on, th, phase * (0.9f + 0.13f * i));
			drawEnableDot(args, L, i, on);
			drawFoot(args, L, i, footText(i), on);
		}
	}

	// Browser thumbnail: no module, so a representative loom — mid-strum, with
	// the low strings still ringing from the pass that already went by.
	void drawPreview(const DrawArgs& args) {
		LoomLayout L = layout();
		drawTabs(args, L, 0);
		drawFrame(args, L);
		static const float amp[LOOM_N]   = {0.95f, 0.80f, 0.62f, 0.44f, 0.26f, 0.12f, 0.f, 0.f};
		static const float flash[LOOM_N] = {0.f, 0.f, 0.f, 0.2f, 0.6f, 1.f, 0.f, 0.f};
		static const char* note[LOOM_N]  = {"C2","D2","E2","G2","A2","C3","D3","E3"};
		static const bool  on[LOOM_N]    = {true,true,true,true,true,true,true,false};
		for (int i = 0; i < LOOM_N; i++) {
			float th = 0.9f + 2.3f * (1.f - (float)i / (float)(LOOM_N - 1)) * 0.55f;
			drawString(args, L, i, amp[i], flash[i], on[i], th, 0.7f * i);
			drawEnableDot(args, L, i, on[i]);
			drawFoot(args, L, i, note[i], on[i]);
		}
	}
};


// =============================================================================
// Panel — 28HP. Left column is the instrument, the plate under the display is
// the eight strings, the bottom row is the player.
// =============================================================================

struct LoomWidget : ModuleWidget {
	LoomWidget(Loom* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/loom.svg")));
		using sfs::hp;

		// NO sfs::PanelLabels HERE, DELIBERATELY. res/loom.svg is the designer's
		// own file, published by `figma_panel_template.py --publish loom`, and it
		// already carries every label, every connector line and the logo, set in
		// their type at their weight. Drawing labels over it at runtime would
		// replace all of that with panel-style.hpp's defaults -- right while a
		// layout is still being worked out in code, wrong once a finished face
		// exists. This constructor places components and nothing else.
		const float colA = hp(2), colB = hp(4.5f), colC = hp(7);
		const float str0 = hp(8.5f), strStep = hp(2.5f);
		const float gateY = hp(13), outY = hp(16);
		const float knobY = hp(21), botY = hp(24);

		LoomDisplay* disp = new LoomDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(hp(7.25f), hp(2)));
		disp->box.size = mm2px(Vec(hp(20), hp(9)));
		addChild(disp);

		// ── left: four rows of two pot-over-jack pairs ─────────────────────────
		const float rowA = hp(4), rowAj = hp(6);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, rowA)), module, Loom::BODY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, rowAj)), module, Loom::BODY_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, rowA)), module, Loom::COUPLE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, rowAj)), module, Loom::COUPLE_CV_INPUT));

		const float rowB = hp(9), rowBj = hp(11);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, rowB)), module, Loom::DECAY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, rowBj)), module, Loom::DECAY_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, rowB)), module, Loom::DAMP_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, rowBj)), module, Loom::DAMP_CV_INPUT));

		const float rowC = hp(14), rowCj = hp(16);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, rowC)), module, Loom::PICK_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, rowCj)), module, Loom::PICK_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, rowC)), module, Loom::SPREAD_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, rowCj)), module, Loom::SPREAD_CV_INPUT));

		const float rowD = hp(19), rowDj = hp(21);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, rowD)), module, Loom::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, rowDj)), module, Loom::ROOT_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, rowD)), module, Loom::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, rowDj)), module, Loom::SCALE_CV_INPUT));

		// ── the eight strings: a gate in and an audio out each ─────────────────
		for (int i = 0; i < LOOM_N; i++) {
			float x = str0 + strStep * i;
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, gateY)), module, Loom::STRING_GATE_INPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, outY)), module, Loom::STRING_OUTPUT + i));
		}

		// ── the auto-player, two rows of pot + jack, each ending in a button ────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(9.5f), knobY)), module, Loom::PATTERN_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(11.75f), knobY)), module, Loom::PATTERN_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(14.5f), knobY)), module, Loom::DENSITY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(16.75f), knobY)), module, Loom::DENSITY_CV_INPUT));

		// ── bottom row ─────────────────────────────────────────────────────────
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, botY)), module, Loom::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, botY)), module, Loom::GATE_INPUT));
		// VEL stands above V/OCT with no pot of its own, so no pipe joins them --
		// it is the poly velocity that goes with the poly gate, not a modulator.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colC, hp(21))), module, Loom::VEL_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colC, botY)), module, Loom::VOCT_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(9.5f), botY)), module, Loom::OCT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(11.75f), botY)), module, Loom::OCT_CV_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(hp(14.5f), botY)), module, Loom::RESET_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(16.75f), botY)), module, Loom::RESET_INPUT));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(hp(20), botY)), module, Loom::AUTO_PARAM, Loom::AUTO_LIGHT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(23.5f), botY)), module, Loom::MIX_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(26), botY)), module, Loom::MIX_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Loom* m = dynamic_cast<Loom*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);

		menu->addChild(createSubmenuItem("Tuning", "", [=](Menu* sub) {
			for (int t = 0; t < LOOM_NTUNINGS; t++)
				sub->addChild(createMenuItem(LOOM_TUNINGS[t].name, "",
					[=]() { m->applyTuning(t); }));
		}));
		// "Quantize tuning" used to sit here too. It is the SCALE pot now, and
		// leaving the menu copy in would give one value two sources that disagree.

		menu->addChild(createIndexPtrSubmenuItem("Mouse strum",
			{"Hover over the strings", "Click and drag only"}, &m->mouseMode));

		menu->addChild(createSubmenuItem("Stereo width", string::f("%.0f%%", m->stereoWidth * 100.f),
			[=](Menu* sub) {
				sub->addChild(new MenuSeparator);
				struct WidthQuantity : Quantity {
					Loom* m;
					void setValue(float v) override { m->stereoWidth = clamp(v, 0.f, 1.f); }
					float getValue() override { return m->stereoWidth; }
					float getDefaultValue() override { return 0.6f; }
					std::string getLabel() override { return "Stereo width"; }
					std::string getUnit() override { return "%"; }
					float getDisplayValue() override { return m->stereoWidth * 100.f; }
					void setDisplayValue(float v) override { setValue(v / 100.f); }
				};
				WidthQuantity* q = new WidthQuantity;
				q->m = m;
				Slider* sl = new Slider;
				sl->quantity = q;
				sl->box.size.x = 180.f;
				sub->addChild(sl);
			}));

		menu->addChild(createSubmenuItem("Auto rate (no clock patched)",
			string::f("%.1f Hz", m->internalHz), [=](Menu* sub) {
				struct RateQuantity : Quantity {
					Loom* m;
					void setValue(float v) override { m->internalHz = clamp(v, 0.5f, 24.f); }
					float getValue() override { return m->internalHz; }
					float getDefaultValue() override { return 6.f; }
					float getMinValue() override { return 0.5f; }
					float getMaxValue() override { return 24.f; }
					std::string getLabel() override { return "Rate"; }
					std::string getUnit() override { return " Hz"; }
				};
				RateQuantity* q = new RateQuantity;
				q->m = m;
				Slider* sl = new Slider;
				sl->quantity = q;
				sl->box.size.x = 180.f;
				sub->addChild(sl);
			}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Enable all strings", "",
			[=]() { for (int i = 0; i < LOOM_N; i++) m->enabled[i] = true; }));
		menu->addChild(createMenuItem("Silence all strings", "",
			[=]() { for (int i = 0; i < LOOM_N; i++) m->clearReq[i] = true; }));
	}
};

Model* modelLoom = createModel<Loom, LoomWidget>("Loom");
