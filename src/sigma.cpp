#include "plugin.hpp"
#include "panel-style.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// =============================================================================
// Sigma — a tone SUMMED from sixteen partials you can move independently.
//
// Named for the summation sign, because that is the whole operation: there is
// no oscillator here making a waveform and no filter taking anything away. The
// output is nothing but a sum of sines, and every control is a statement about
// what goes into that sum.
//
// After the Crumar GDS (1980), which commercialised Hal Alles' Bell Labs
// machine. The GDS gave every oscillator a sixteen-stage amplitude envelope AND
// a sixteen-stage frequency envelope, plus two complete envelope sets
// interpolated by velocity. That is 512 breakpoints for sixteen partials, and it
// is why the thing needed a $30,000 computer bolted to it to be programmable at
// all. A Eurorack panel has no answer to that, so this takes the behaviour and
// leaves the mechanism.
//
// TWO NUMBERS PER PARTIAL, NOT AN ENVELOPE: depth and RATE. Depth alone is not
// enough and that is the thing to get right -- a shared envelope scaled only in
// level gives a spectral fade, bright to dark, but every partial still ends at
// the same instant. What makes a struck tone sound struck is that high partials
// die SOONER, which needs the envelope to run at a different rate per partial.
// It is the same thing Kit's modal damping and Loom's in-loop lowpass do.
//
// THE PANEL IS SPREADS; THE SCREEN IS THE EXCEPTIONS. Every macro turned out to
// be the same shape of thing: a curve across the partial index. TILT is a spread
// of level, STRETCH of pitch, WIDTH of pan, ENV RATE of envelope rate, ENV
// SPREAD of envelope start time, and each LFO's spread is the same idea in
// phase. So the panel is one gesture -- how does this attribute vary from the
// fundamental to the sixteenth partial -- repeated per attribute, and the screen
// holds the per-partial deviations from those curves.
//
// VELOCITY MORPHS THE SPECTRUM RATHER THAN SCALING IT. Two complete level sets,
// soft and loud, crossfaded. A quiet note is a different timbre, not a quieter
// one. It is the least-copied idea in the GDS and the cheapest to steal.
//
// See docs/sigma-design.md.
// =============================================================================

// The ARRAY size; the count actually sounding is a menu choice. 64 x 16 voices
// is 1024 oscillators, which the benchmark puts near 5% of one core -- so the
// ceiling is editability and Nyquist, not CPU.
static const int SG_MAXP   = 64;
static const int SG_VOICES = 16;
static const int SG_NCOUNT = 3;
static const int SG_COUNTS[SG_NCOUNT] = {16, 32, 64};

// A table sine, because 16 partials x 16 voices is 256 oscillators and
// std::sin() 256 times a sample is not the same proposition as a lookup.
// Measured at 1.6% of one core for the full 256 with linear interpolation.
static const int SG_TBL = 4096;
static float sgSinTbl[SG_TBL + 1];
static bool  prTblReady = false;
static void prInitTable() {
	if (prTblReady) return;
	for (int i = 0; i <= SG_TBL; i++)
		sgSinTbl[i] = std::sin(2.0 * M_PI * (double)i / (double)SG_TBL);
	prTblReady = true;
}
static inline float sgSin(float ph) {              // ph in [0,1)
	float f = ph * SG_TBL;
	int k = (int)f;
	float fr = f - (float)k;
	return sgSinTbl[k] + (sgSinTbl[k + 1] - sgSinTbl[k]) * fr;
}

// A MOD MATRIX rather than one target per LFO. Three sources by five
// destinations is fifteen numbers, which is small enough to draw and edit on
// screen and large enough that an LFO can do two things at once -- which is
// most of what makes three of them feel like more than three.
enum SgModDest { SG_MOD_LEVEL, SG_MOD_PITCH, SG_MOD_PAN, SG_MOD_TILT,
                 SG_MOD_STRETCH, SG_MOD_CUT, SG_MOD_N };
static const char* SG_MODNAME[SG_MOD_N] = {"LVL", "PIT", "PAN", "TLT", "STR", "CUT"};
// Four sources: the three LFOs, and the envelope. An envelope that can only
// drive amplitude is half an envelope -- the reason it is worth a row is that
// it is the one source that knows where it is in the NOTE.
static const int SG_MODSRC = 4;
static const char* SG_SRCNAME[SG_MODSRC] = {"LFO1", "LFO2", "LFO3", "ENV"};

// Sixteen voices sum, and a hard clamp is the harshest thing an overload can
// do. Measured single-voice peaks run to 6.9V on Bell, and incoherent voices
// grow as sqrt(N) -- so a THREE-NOTE CHORD on Bell reached the old +/-10V clamp
// and tore. Linear to +/-6V and then bending, asymptotic to +/-10, the worst
// case compresses instead. Same curve as Loom, which learned this first with
// eight bowed strings.
// A fixed pseudo-random position per partial. A HASH, not a random draw: the
// stereo image has to be the same every time the patch loads, and the same for
// every voice, or a chord smears instead of placing itself.
static inline float sgScatter(int p) {
	uint32_t h = (uint32_t)p * 2654435761u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
	return (float)((h >> 8) & 0xFFFF) / 32767.5f - 1.f;
}

static inline float sgSoftClip(float x) {
	const float T = 6.f, K = 4.f;
	float a = std::fabs(x);
	if (a <= T) return x;
	return std::copysign(T + K * (1.f - K / (K + a - T)), x);
}
static const int SG_NPRESET = 21;

// The ADSR sliders read in MILLISECONDS. What is stored is a 0..1 position on
// an exponential map, and "0.373" says nothing about an envelope -- you cannot
// tell a 4ms attack from a 400ms one, which is the whole difference between a
// struck tone and a bowed one. The map lives here rather than in the display so
// the tooltip, the typed-in value and the engine all read the same curve.
struct SgTimeQuantity : ParamQuantity {
	float lo = 0.001f, hi = 8.f;
	float seconds() { return lo * std::pow(hi / lo, getValue()); }
	std::string getDisplayValueString() override {
		float ms = seconds() * 1000.f;
		// A decimal below 10ms, where the difference between 1.0 and 1.4 is
		// most of a cycle at the bottom of the range; whole milliseconds above,
		// where a tenth of one is noise.
		return ms < 10.f ? string::f("%.1f ms", ms) : string::f("%.0f ms", ms);
	}
	void setDisplayValueString(std::string t) override {
		float ms = 0.f;
		if (std::sscanf(t.c_str(), "%f", &ms) != 1) return;
		float sec = clamp(ms * 0.001f, lo, hi);
		setValue(std::log(sec / lo) / std::log(hi / lo));
	}
};

struct Sigma : Module {
	enum ParamId {
		TILT_PARAM, ODDEVEN_PARAM, STRETCH_PARAM, WIDTH_PARAM, MORPH_PARAM,
		ENVRATE_PARAM, ENVSPREAD_PARAM,
		ATTACK_PARAM, DECAY_PARAM, SUSTAIN_PARAM, RELEASE_PARAM,
		LFORATE_PARAM,                                  // 3
		// RETIRED. The matrix cell IS the depth, per destination; a master
		// depth on top of it was two controls arguing over one number.
		LFODEPTH_PARAM  = LFORATE_PARAM + 3,            // 3, unused
		LFOSPREAD_PARAM = LFODEPTH_PARAM + 3,           // 3
		CUTOFF_PARAM    = LFOSPREAD_PARAM + 3,
		RESO_PARAM,
		// APPENDED 2026-08, after the Synergy manual. Params serialise by index,
		// so these go on the end however much they belong beside MORPH.
		AMPSENS_PARAM, AMPCENTER_PARAM,   // velocity -> loudness
		MORPHSENS_PARAM,                  // velocity -> timbre, MORPH is its centre
		LFODELAY_PARAM,                   // 3
		LFORAND_PARAM = LFODELAY_PARAM + 3,   // 3
		PARAMS_LEN = LFORAND_PARAM + 3
	};
	enum InputId {
		VOCT_INPUT, GATE_INPUT, VEL_INPUT, VCA_INPUT,
		TILT_INPUT, ODDEVEN_INPUT, STRETCH_INPUT, WIDTH_INPUT, MORPH_INPUT,
		ENVRATE_INPUT, ENVSPREAD_INPUT, CUTOFF_INPUT,
		LFOSYNC_INPUT,                                  // 3
		// APPENDED for the 2026-08 panel, which draws a jack under RES. Inputs
		// serialise by index, so it goes on the end rather than beside CUTOFF.
		RESO_INPUT = LFOSYNC_INPUT + 3,
		INPUTS_LEN
	};
	enum OutputId { L_OUTPUT, R_OUTPUT, OUTPUTS_LEN };
	enum LightId  { LIGHTS_LEN };

	// ── per-partial state, the screen's six tabs ────────────────────────────
	float pLevel[SG_MAXP]  = {};   // the loud spectrum
	float pSoft[SG_MAXP]   = {};   // the quiet spectrum; velocity morphs
	float pPitch[SG_MAXP]  = {};   // cents, a trim on top of STRETCH
	float pPan[SG_MAXP]    = {};   // -1..1
	float pDepth[SG_MAXP]  = {};   // how much envelope this partial takes
	float pRate[SG_MAXP]   = {};   // 0..1 -> 0.25x .. 4x

	// ── voices ──────────────────────────────────────────────────────────────
	enum Stage { ST_IDLE, ST_ATT, ST_DEC, ST_SUS, ST_REL };
	struct Voice {
		bool  on = false;
		float vel = 1.f, pitch = 0.f;
		float phase[SG_MAXP] = {};
		// A partial's envelope is its own, because rate and start time differ.
		float env[SG_MAXP] = {};
		int   stage[SG_MAXP] = {};
		float wait[SG_MAXP] = {};      // ENV SPREAD's start delay
		float relFrom[SG_MAXP] = {};
		// The highest this partial's envelope has reached since the note began.
		// An INVERTED partial needs it: "one minus the envelope" is 1 at the
		// end of a note and also 1 before it has started, and the second of
		// those is not a bloom, it is a blast.
		float envMax[SG_MAXP] = {};
		float mEnv = 0.f; int mStage = ST_IDLE; float mRelFrom = 0.f;
		float ampGain = 1.f;      // velocity -> level, latched at note-on
		// The LFOs run PER VOICE. A periodic vibrato locks every voice together
		// (the Synergy: a new note "immediately starts tracking" the one
		// already sounding), where a random one must not, or every note in a
		// chord wobbles identically and the whole point is lost.
		float lfoPh[3] = {};
		float rndCur[3] = {}, rndNext[3] = {};
		uint32_t rng = 1u;
		float age = 0.f;          // seconds since this note was struck
	};
	Voice voice[SG_VOICES];

	// Which voice each poly channel is currently driving, or -1. A channel owns
	// a voice only while its gate is HIGH; once released the voice keeps ringing
	// on its own and the channel is free to strike another. That is what lets a
	// MONO gate line overlap: sixteen voices, one channel feeding them in turn.
	// -1 rather than 0, and set in the constructor as well as onReset, because
	// 0 is a VALID voice index -- a default-initialised array would claim every
	// channel already owns voice 0.
	int chanVoice[SG_VOICES];
	// FIRST AVAILABLE takes any idle voice, so a repeated note keeps landing on
	// voice 0 and each strike cuts short the tail of the last. ROLLING takes the
	// NEXT voice every time regardless, which maximises the time before a voice
	// is reused -- on the Gong that is the difference between a ringing pile and
	// a stutter. Both are the Synergy's, and its reason for having both is the
	// same one.
	enum Alloc { ALLOC_FIRST, ALLOC_ROLL };
	int allocMode = ALLOC_FIRST;
	// What the PITCH tab snaps to. Free is how it behaved; the other three
	// exist because a partial retuned to 4x or 10x wants to land ON the ratio,
	// and hunting +1200.0 cents by dragging a bar is not something anyone
	// should have to do. The stored value is cents/2400 either way -- this only
	// quantizes what a DRAG can produce.
	enum PitchSnap { PSNAP_FREE, PSNAP_CENT, PSNAP_SEMI, PSNAP_OCT };
	int pitchSnap = PSNAP_FREE;
	float snapPitch(float v) const {
		float cents = v * 2400.f;
		switch (pitchSnap) {
			case PSNAP_CENT: cents = std::round(cents); break;
			case PSNAP_SEMI: cents = std::round(cents / 100.f) * 100.f; break;
			case PSNAP_OCT:  cents = std::round(cents / 1200.f) * 1200.f; break;
			default: break;
		}
		return clamp(cents / 2400.f, -1.f, 1.f);
	}
	int rollNext = 0;
	float lfoPhase[3] = {};      // the shared periodic phase a locked voice tracks
	// A deterministic per-voice generator. random::uniform() would make the
	// same patch sound different on every load, and worse, would not be the
	// same twice under the harness.
	static inline float vrand(uint32_t& st) {
		st = st * 1664525u + 1013904223u;
		return (float)((st >> 8) & 0xFFFFFF) / (float)0xFFFFFF * 2.f - 1.f;
	}
	dsp::SchmittTrigger lfoSyncTrig[3];
	float mod[SG_MODSRC][SG_MOD_N] = {};   // the matrix, bipolar

	// ── what the screen shows ───────────────────────────────────────────────
	// The stored per-partial arrays are what you drew; these are what the
	// engine actually did with them once the macros, the envelope and the LFOs
	// had their say. Seeing only the first is like editing a mixer with the
	// faders hidden.
	int   nPartials = 16;
	int   dispTab = 0;
	float dispMorph = 1.f;
	float liveAmp[SG_MAXP] = {};   // amplitude in force, per partial
	float liveEnv[SG_MAXP] = {};
	float livePan[SG_MAXP] = {};
	float liveCents[SG_MAXP] = {};
	bool  liveOn = false;

	// The knob is exponential over 0.02..30Hz; showing 0.30 told you nothing.
	struct LfoHzQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float hz = 0.02f * std::pow(30.f / 0.02f, getValue());
			return hz < 1.f ? string::f("%.2f Hz", hz)
			     : hz < 10.f ? string::f("%.2f Hz", hz) : string::f("%.1f Hz", hz);
		}
	};

	Sigma() {
		for (int v = 0; v < SG_VOICES; v++) chanVoice[v] = -1;
		prInitTable();
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// TILT defaults to 0.4, which is n^-1 -- a force impulse gives a mode
		// initial velocity, so amplitude goes as 1/omega. Kit learned that the
		// hard way from eigendrum; a flat spectrum is not a neutral default, it
		// is a buzzer.
		// Flat, because LEVEL now carries the 1/n^2 slope itself.
		configParam(TILT_PARAM, -1.f, 1.f, 0.f, "Tilt (spectral slope)");
		// -1 is odd-only, which with the 1/n^2 levels IS a triangle. Sweeping
		// up from here opens the even harmonics and the tone fills out.
		configParam(ODDEVEN_PARAM, -1.f, 1.f, -1.f, "Odd / even balance");
		configParam(STRETCH_PARAM, 0.f, 1.f, 0.f, "Stretch (inharmonicity)");
		// BIPOLAR. Right fans the partials out in order, low to high; left
		// SCATTERS them. Two genuinely different stereo images, and the fan was
		// the only one available -- which is right for a bloom that should open
		// outward and wrong for anything meant to be a body of sound rather
		// than a diagram of one.
		configParam(WIDTH_PARAM, -1.f, 1.f, 0.35f, "Stereo width (- scatter, + fan)");
		// BIPOLAR, and it has to be. Velocity is 1.0 when VEL is unpatched, so a
		// 0..1 morph that only ADDS to it sat pinned at the loud spectrum
		// forever: SOFT was unreachable and MORPH itself did nothing until you
		// patched a velocity below 10V. A control that is inert out of the box
		// is not a control.
		configParam(MORPH_PARAM, -1.f, 1.f, 0.f, "Morph (soft <-> loud), summed with velocity");
		configParam(ENVRATE_PARAM, 0.f, 1.f, 0.35f, "Envelope rate spread (highs die sooner)");
		configParam(ENVSPREAD_PARAM, -1.f, 1.f, 0.f, "Envelope time spread (bloom / onset)");

		// The three ranges are the ones the engine uses, stated once here and
		// read back by knobTime() below -- if they ever disagree the tooltip
		// becomes a lie, which is worse than no tooltip.
		auto* qa = configParam<SgTimeQuantity>(ATTACK_PARAM,  0.f, 1.f, 0.02f, "Attack");
		qa->lo = 0.001f; qa->hi = 8.f;
		auto* qd = configParam<SgTimeQuantity>(DECAY_PARAM,   0.f, 1.f, 0.35f, "Decay");
		qd->lo = 0.005f; qd->hi = 12.f;
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.5f,  "Sustain", "%", 0.f, 100.f);
		auto* qr = configParam<SgTimeQuantity>(RELEASE_PARAM, 0.f, 1.f, 0.3f,  "Release");
		qr->lo = 0.005f; qr->hi = 16.f;

		for (int i = 0; i < 3; i++) {
			configParam<LfoHzQuantity>(LFORATE_PARAM + i, 0.f, 1.f, 0.3f,
			                           string::f("LFO %d rate", i + 1));
			configParam(LFODEPTH_PARAM + i,  0.f, 1.f, 0.5f, string::f("LFO %d depth", i + 1));
			configParam(LFOSPREAD_PARAM + i, 0.f, 1.f, 0.3f,
			            string::f("LFO %d spread, low partial to high", i + 1));
			configInput(LFOSYNC_INPUT + i, string::f("LFO %d sync", i + 1));
		}

		configInput(VOCT_INPUT, "V/oct (poly)");
		configInput(GATE_INPUT, "Gate (poly)");
		configInput(VEL_INPUT, "Velocity (poly)");
		configInput(VCA_INPUT, "VCA");
		configInput(TILT_INPUT, "Tilt CV");
		configInput(ODDEVEN_INPUT, "Odd / even CV");
		configInput(STRETCH_INPUT, "Stretch CV");
		configInput(WIDTH_INPUT, "Width CV");
		configInput(MORPH_INPUT, "Morph CV");
		configInput(ENVRATE_INPUT, "Envelope rate spread CV");
		configInput(ENVSPREAD_INPUT, "Envelope time spread CV");
		configInput(CUTOFF_INPUT, "Cutoff CV (1V/oct)");
		// A SPECTRAL filter, not a time-domain one: the gain of a second-order
		// lowpass evaluated at each partial's own frequency. In an additive
		// voice that is exact -- no phase smear, no aliasing, no oversampling
		// -- and it costs one evaluation per partial that was already looping.
		configParam(CUTOFF_PARAM, 0.f, 1.f, 1.f, "Cutoff");
		configParam(RESO_PARAM, 0.f, 1.f, 0.f, "Resonance");
		configInput(RESO_INPUT, "Resonance CV");

		// ── touch response, after the Synergy's four knobs ──────────────────
		// SENSITIVITY is how much of the range velocity commands; CENTRE is
		// where that range sits. The pair is the Synergy's idea and it is worth
		// copying exactly, because with sensitivity at zero the centre stops
		// being an offset and becomes a plain level control -- one knob doing
		// two jobs with no mode to switch.
		//
		// Amplitude and timbre are kept INDEPENDENT, which the manual is
		// emphatic about: you can have a voice whose loudness barely moves while
		// its whole range of timbres is available, or the reverse.
		configParam(AMPSENS_PARAM, 0.f, 1.f, 0.6f, "Velocity to level", "%", 0.f, 100.f);
		configParam(AMPCENTER_PARAM, 0.f, 1.f, 0.8f, "Level centre", "%", 0.f, 100.f);
		// 0.5 is the pre-2026-08 behaviour exactly: velocity spanning the whole
		// morph, linearly. Below that it commands less of it; above, the blend
		// hardens toward a SWITCH between the two spectra.
		configParam(MORPHSENS_PARAM, 0.f, 1.f, 0.5f, "Velocity to timbre", "%", 0.f, 100.f);
		for (int i = 0; i < 3; i++) {
			configParam(LFODELAY_PARAM + i, 0.f, 1.f, 0.f,
			            string::f("LFO %d delay", i + 1), " s", 0.f, 3.f);
			configParam(LFORAND_PARAM + i, 0.f, 1.f, 0.f,
			            string::f("LFO %d random", i + 1), "%", 0.f, 100.f);
		}
		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");

		initSpectrum();
	}

	// One definition of "default", used by the constructor, by Initialize, and
	// by a double-click on a header. Three copies of a default is three chances
	// for a reset to put you somewhere a fresh module never starts.
	static float defaultFor(int tab, int p) {
		switch (tab) {
			// A TRIANGLE, which is a usable sound the moment you place the
			// module. Odd harmonics at 1/n^2 is the triangle's own spectrum;
			// the evens are present in the ARRAY at the same law and silenced
			// by ODD/EVEN instead, so the control can bring them back. Zeroing
			// them here would have made a default that no control could undo.
			case 0: { float n = (float)p + 1.f; return 1.f / (n * n); }
			case 1: return 1.f / (1.f + 0.55f * (float)p);       // SOFT, darker
			case 2: return 0.f;                                  // PITCH
			case 3: return 1.f;                                  // DEPTH
			default: return 0.5f;                                // RATE, = 1x
		}
	}
	void resetTab(int tab) {
		float* a[5] = {pLevel, pSoft, pPitch, pDepth, pRate};
		if (tab < 0 || tab > 4) return;
		for (int p = 0; p < SG_MAXP; p++) a[tab][p] = defaultFor(tab, p);
	}
	void initSpectrum() {
		for (int p = 0; p < SG_MAXP; p++) {
			// The soft spectrum starts DARKER than the loud one, which is what
			// velocity does on any real instrument: play quietly and you lose
			// the top of the spectrum, not just level.
			pLevel[p] = defaultFor(0, p);
			pSoft[p]  = defaultFor(1, p);
			pPitch[p] = defaultFor(2, p);
			pDepth[p] = defaultFor(3, p);
			pRate[p]  = defaultFor(4, p);
			pPan[p]   = 0.f;
		}
	}
	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		initSpectrum();
		for (int v = 0; v < SG_VOICES; v++) { voice[v] = Voice(); chanVoice[v] = -1; }
		for (int i = 0; i < 3; i++) lfoPhase[i] = 0.f;
		for (int r = 0; r < SG_MODSRC; r++)
			for (int d = 0; d < SG_MOD_N; d++) mod[r][d] = 0.f;
	}

	static float knobTime(float k, float lo, float hi) {   // exponential seconds
		return lo * std::pow(hi / lo, k);
	}
	// The inverses, so a preset can ask for "5 ms", "0.3 Hz" or "900 Hz"
	// rather than for 0.179, 0.373 and 0.491. A preset written in knob
	// positions cannot be checked against anything.
	static float timeKnob(float sec, float lo, float hi) {
		return clamp(std::log(sec / lo) / std::log(hi / lo), 0.f, 1.f);
	}
	static float lfoKnob(float hz) { return timeKnob(hz, 0.02f, 30.f); }

	// ── the Synergy's touch response, both axes ────────────────────────────
	// SENSITIVITY is how much of the range velocity commands; CENTRE is where
	// that range sits. With sensitivity at zero the centre is simply the value,
	// which is why the pair needs no separate on/off.
	static float touch(float vel, float sens, float centre) {
		return clamp(centre + sens * (vel - 0.5f), 0.f, 1.f);
	}

	// Timbre is deliberately NOT the same curve. On the Synergy the widest
	// spread between the two timbres is at the MIDDLE of the sensitivity range,
	// and past the middle the control stops blending and starts SWITCHING --
	// slow playing gives one spectrum, fast playing the other, with less and
	// less in between. Storing two complete spectra and only ever crossfading
	// them wastes what having two is for; a hard switch is a thing a crossfade
	// cannot do at all.
	static float touchMorph(float vel, float sens, float centre01) {
		float span = std::min(sens, 0.5f) * 2.f;          // 0..1 over the lower half
		float hard = std::max(0.f, sens - 0.5f) * 2.f;    // 0..1 over the upper half
		float x = clamp(vel, 0.f, 1.f) * 2.f - 1.f;       // -1..1
		if (hard > 0.f) {
			float k = 1.f + hard * 15.f;
			float y = std::tanh(x * k) / std::tanh(k);
			x = x + (y - x) * hard;                       // linear -> step
		}
		return clamp(centre01 + span * 0.5f * x, 0.f, 1.f);
	}
	static float cutKnob(float hz) {                       // fc = 30 * 2^(k*10)
		return clamp(std::log2(std::max(hz, 30.f) / 30.f) / 10.f, 0.f, 1.f);
	}

	// Four starting points, each of which is a different ARGUMENT for what
	// additive is good at rather than four variations on one.
	void loadPreset(int which) {
		initSpectrum();
		for (int r = 0; r < SG_MODSRC; r++)
			for (int d = 0; d < SG_MOD_N; d++) mod[r][d] = 0.f;
		params[STRETCH_PARAM].setValue(0.f);
		params[MORPH_PARAM].setValue(0.f);
		params[CUTOFF_PARAM].setValue(1.f);
		params[RESO_PARAM].setValue(0.f);
		params[ENVSPREAD_PARAM].setValue(0.f);
		params[TILT_PARAM].setValue(0.f);
		params[WIDTH_PARAM].setValue(0.3f);
		// Two presets raise the partial count. Reset it here so loading one of
		// them and then loading another does not leave 64 partials behind on a
		// preset written for 16.
		nPartials = 16;

		switch (which) {
			case 0:   // RAMP -- every harmonic at 1/n, which is a sawtooth
				for (int p = 0; p < SG_MAXP; p++) pLevel[p] = 1.f / (float)(p + 1);
				params[ODDEVEN_PARAM].setValue(0.f);
				params[ENVRATE_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.005f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.3f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.8f);
				params[RELEASE_PARAM].setValue(timeKnob(0.4f, 0.005f, 16.f));
				break;

			case 1:   // SQUARE -- the same 1/n, odd harmonics only
				for (int p = 0; p < SG_MAXP; p++) pLevel[p] = 1.f / (float)(p + 1);
				params[ODDEVEN_PARAM].setValue(-1.f);
				params[ENVRATE_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.004f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.3f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.85f);
				params[RELEASE_PARAM].setValue(timeKnob(0.3f, 0.005f, 16.f));
				break;

			case 14: {  // CS-80 -- lush brass. Slow swell, wide, and drifting:
				// the CS-80's signature is not a waveform, it is that nothing
				// in it sits still, so the two LFOs matter more than the levels.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / (float)(p + 1);
					pSoft[p]  = 1.f / std::pow((float)(p + 1), 1.9f);   // far darker quiet
					pRate[p]  = 0.5f;
				}
				params[ODDEVEN_PARAM].setValue(0.f);
				params[STRETCH_PARAM].setValue(0.06f);      // a little unrest
				params[WIDTH_PARAM].setValue(0.75f);
				params[ENVRATE_PARAM].setValue(0.2f);
				params[ATTACK_PARAM].setValue(timeKnob(0.22f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(1.2f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.75f);
				params[RELEASE_PARAM].setValue(timeKnob(0.9f, 0.005f, 16.f));
				params[MORPH_PARAM].setValue(-0.35f);       // so SOFT is audible
				mod[0][SG_MOD_PITCH] = 0.10f;               // vibrato
				mod[1][SG_MOD_LEVEL] = 0.22f;               // spread shimmer
				mod[3][SG_MOD_CUT]   = 0.35f;               // envelope opens it
				params[CUTOFF_PARAM].setValue(0.52f);
				params[LFORATE_PARAM + 0].setValue(0.42f);
				params[LFOSPREAD_PARAM + 0].setValue(0.f);  // vibrato moves as one
				params[LFORATE_PARAM + 1].setValue(0.22f);
				params[LFOSPREAD_PARAM + 1].setValue(0.6f);
				break;
			}

			case 5: {  // MARIMBA -- a tuned bar is UNDERCUT so its first
				// overtone lands two octaves up, at 4x rather than the 2x a
				// harmonic series would give, and the next near 10x. That is
				// the whole sound, and it is why this preset is really a use of
				// the PITCH tab rather than of the LEVEL tab.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 0.f; pPitch[p] = 0.f;
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				pLevel[0] = 1.f;                                   // fundamental
				pLevel[1] = 0.32f; pPitch[1] = 0.5f;                    // 2x -> 4x
				// The 10x was at 0.16 and it is INHARMONIC, so it read as metal
				// rather than as wood. On a real bar the second overtone is
				// audible in the strike and gone almost at once; it belongs in
				// the attack, not in the tone.
				pLevel[2] = 0.06f; pPitch[2] = std::log2(10.f / 3.f) / 2.f;  // 3x -> 10x
				// and the two overtones die far sooner than the fundamental,
				// which is most of what separates a struck bar from a bell
				pRate[1] = 0.74f; pRate[2] = 0.88f;
				params[ODDEVEN_PARAM].setValue(0.f);
				params[ENVRATE_PARAM].setValue(0.68f);
				params[ATTACK_PARAM].setValue(timeKnob(0.002f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.45f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.f);               // struck, not held
				params[RELEASE_PARAM].setValue(timeKnob(0.25f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.2f);
				break;
			}

			case 6: {  // BELL -- the PARTIALS OF A BELL, placed one by one.
				// STRETCH was the wrong tool: it bends the whole series smoothly
				// and that is a gong, not a bell. A bell is tuned, and what it
				// is tuned TO is a specific set -- hum, prime, TIERCE, quint,
				// nominal. The tierce sits a minor third above the prime, and
				// that one interval is why a bell sounds like a bell and why
				// bells have a minor tonality. A smooth stretch has no minor
				// third anywhere in it, which is exactly why the old one came
				// out thin and tinny.
				//
				// Ratios 0.5 / 1.0 / 1.2 / 1.5 / 2.0 / 2.5 / 3.0 / 4.0, reached
				// by detuning harmonic n down to the ratio wanted.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 0.f; pSoft[p] = 0.f; pPitch[p] = 0.f;
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				// The RATIOS ARE RIGHT and stay untouched. An attempt to temper
				// them toward the harmonic series was measured and thrown away:
				// interpolating each partial toward harmonic n moved the whole
				// set 420-594 cents sharp and stretched prime-to-tierce from
				// 316 cents to 871, which does not reduce the detuning, it
				// destroys the minor third the bell is built on.
				//
				// "Too detuned" was a BALANCE problem. The tierce was the
				// loudest partial in the set, so the minor third was the pitch
				// you heard rather than a colour on the prime. Prime leads now
				// and the tierce sits under it, which is how a bell you would
				// call in tune is actually voiced.
				static const float BR[8] = {0.5f, 1.f, 1.2f, 1.5f, 2.f, 2.5f, 3.f, 4.f};
				// Rebalanced AND re-levelled. Dropping the tierce to fix the
				// balance took the whole preset 2.4x quieter than its
				// neighbours, which is its own kind of wrong; everything is
				// scaled back up with the prime pinned at the top, so the prime
				// still leads and the preset sits with the others.
				static const float BL[8] = {0.40f, 1.f, 0.74f, 0.51f, 0.84f,
				                            0.27f, 0.22f, 0.15f};
				for (int p = 0; p < 8; p++) {
					float n = (float)(p + 1);
					pLevel[p] = BL[p];
					pSoft[p]  = BL[p] * (p < 3 ? 0.9f : 0.35f);   // quiet = fewer highs
					pPitch[p] = std::log2(BR[p] / n) / 2.f;
					// the upper partials are the strike, the low ones the ring
					pRate[p]  = clamp(0.5f + 0.055f * (float)p, 0.f, 1.f);
				}
				params[STRETCH_PARAM].setValue(0.f);       // the pitches ARE the bell
				params[ENVRATE_PARAM].setValue(0.35f);
				params[ATTACK_PARAM].setValue(timeKnob(0.002f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(7.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.f);
				params[RELEASE_PARAM].setValue(timeKnob(6.f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.45f);
				break;
			}

			case 11: {  // BOWED -- ENV SPREAD's NEGATIVE half, which is the part
				// that reads as broken when you sweep it with nothing else set
				// up. A bow does not start a note, it works one up: the upper
				// partials speak first and the fundamental arrives underneath
				// them. At -0.5 the fundamental is 125 ms late, which is a
				// bowed onset rather than a delay.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / (float)(p + 1);
					pSoft[p]  = 1.f / std::pow((float)(p + 1), 1.5f);
					pRate[p]  = 0.5f;
					pDepth[p] = 1.f;
				}
				params[ENVSPREAD_PARAM].setValue(-0.5f);
				params[ENVRATE_PARAM].setValue(0.f);    // nothing dies; it is bowed
				params[ATTACK_PARAM].setValue(timeKnob(0.12f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.5f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.9f);
				params[RELEASE_PARAM].setValue(timeKnob(0.7f, 0.005f, 16.f));
				mod[0][SG_MOD_PITCH] = 0.07f;
				// A bow leaves the string ringing; it does not stop it. And the
				// vibrato now waits, which is the single biggest thing separating
				// a played vibrato from an applied one.
				params[LFODELAY_PARAM + 0].setValue(0.25f);   // 0.75s
				params[LFORATE_PARAM + 0].setValue(lfoKnob(5.2f));
				params[LFOSPREAD_PARAM + 0].setValue(0.f);   // one vibrato, not sixteen
				params[WIDTH_PARAM].setValue(0.35f);
				break;
			}

			case 16: {  // BLOOM -- ENV SPREAD's positive half AND negative DEPTH
				// together, the only preset that uses the inverse envelope. The
				// upper half of the spectrum both starts late and follows the
				// envelope BACKWARDS, so those partials arrive as the lower ones
				// are leaving. That crossing is the thing additive can do that a
				// filter sweep only imitates.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / std::pow((float)(p + 1), 0.9f);
					pSoft[p]  = 1.f / std::pow((float)(p + 1), 1.4f);
					pRate[p]  = 0.35f;                       // slow, all of them
					pDepth[p] = (p >= 6) ? -0.7f : 1.f;      // upper half inverted
				}
				params[ENVSPREAD_PARAM].setValue(0.75f);
				params[ENVRATE_PARAM].setValue(0.1f);
				params[ATTACK_PARAM].setValue(timeKnob(0.9f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(3.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.6f);
				params[RELEASE_PARAM].setValue(timeKnob(2.5f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.85f);
				break;
			}

			case 3: {  // E-PIANO -- the SOFT/LEVEL morph on its own, with MORPH
				// left at zero so VELOCITY has the whole say. Played softly this
				// is very nearly a sine; played hard the tine bark at partials
				// 4-6 comes in. That is a different spectrum, not a louder one,
				// which is the argument for storing two.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / std::pow((float)(p + 1), 1.3f);
					pSoft[p]  = (p == 0) ? 1.f : ((p == 2) ? 0.10f : 0.f);
					pRate[p]  = clamp(0.5f + 0.035f * (float)p, 0.f, 1.f);
					pDepth[p] = 1.f;
				}
				pLevel[3] = 0.55f; pLevel[4] = 0.42f; pLevel[5] = 0.30f;  // the bark
				params[MORPH_PARAM].setValue(0.f);       // velocity, and only velocity
				params[ENVRATE_PARAM].setValue(0.5f);
				params[ATTACK_PARAM].setValue(timeKnob(0.003f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(1.6f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.14f);
				params[RELEASE_PARAM].setValue(timeKnob(0.85f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.3f);
				break;
			}

			case 18: {  // ROTOR -- PAN spread. LFO2 sweeps pan with its spread
				// wide open, so the partials are at different points of the
				// same circle and the spectrum turns rather than the sound
				// sliding side to side. LFO1 works level in the opposite
				// direction to keep whatever is arriving in front.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / std::pow((float)(p + 1), 1.1f);
					pSoft[p]  = 1.f / std::pow((float)(p + 1), 1.6f);
					pRate[p]  = 0.5f;
					pDepth[p] = 1.f;
				}
				params[ENVRATE_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.05f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.6f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.85f);
				params[RELEASE_PARAM].setValue(timeKnob(0.6f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(1.f);
				params[CUTOFF_PARAM].setValue(cutKnob(2600.f));
				mod[1][SG_MOD_PAN]   =  0.85f;
				mod[0][SG_MOD_LEVEL] = -0.30f;
				params[LFORATE_PARAM + 1].setValue(lfoKnob(0.8f));
				params[LFOSPREAD_PARAM + 1].setValue(1.f);   // the whole circle
				params[LFORATE_PARAM + 0].setValue(lfoKnob(0.8f));
				params[LFOSPREAD_PARAM + 0].setValue(0.55f);
				break;
			}

			case 10: {  // PSYCHEDELIC GONG -- 64 partials, and the only preset
				// MODULATES inharmonicity rather than setting it. A struck gong
				// does not hold still: its partials wander as the plate's modes
				// trade energy, and LFO3 on STRETCH is the cheapest honest
				// version of that. Also the first thing here to run the big
				// partial count, where the Nyquist fade, the spectral filter and
				// the display's column averaging all meet at scale.
				//
				// Note that stretch this high spends partials: at C4 only 27 of
				// the 64 are still under Nyquist, 38 at C3 and 54 at C2. That is
				// the law being honest rather than a fault -- but it does mean
				// this preset wants to be played low, and it is the reason the
				// fade at the top of the range had to be a fade.
				nPartials = 64;
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / std::pow((float)(p + 1), 0.6f);
					pSoft[p]  = 1.f / std::pow((float)(p + 1), 1.2f);
					pRate[p]  = clamp(0.45f + 0.012f * (float)p, 0.f, 1.f);
					pDepth[p] = 1.f;
				}
				params[STRETCH_PARAM].setValue(0.85f);
				params[ENVSPREAD_PARAM].setValue(0.4f);
				params[ENVRATE_PARAM].setValue(0.45f);
				params[ATTACK_PARAM].setValue(timeKnob(0.004f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(10.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.f);
				params[RELEASE_PARAM].setValue(timeKnob(8.f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.9f);
				params[TILT_PARAM].setValue(0.15f);
				mod[2][SG_MOD_STRETCH] = 0.20f;
				params[LFORATE_PARAM + 2].setValue(lfoKnob(0.09f));
				params[LFOSPREAD_PARAM + 2].setValue(0.4f);
				break;
			}

			case 13: {  // VOWEL -- formants drawn straight into the spectrum. An
				// "ah" sits at roughly 730 / 1090 / 2440 Hz, which against a
				// 261.6 Hz fundamental is partials 2.8, 4.2 and 9.3. Peaks are
				// therefore placed by FREQUENCY and not by index, and the
				// partial count has to be high enough to reach the third one.
				//
				// This is deliberately the same job Intone does properly, with
				// FOF grains. If it sounds close, these two modules are
				// competing and one of them should stop.
				nPartials = 32;
				static const float FRQ[3] = {730.f, 1090.f, 2440.f};
				static const float AMP[3] = {1.f, 0.5f, 0.18f};
				static const float BW[3]  = {0.9f, 1.1f, 1.8f};   // in partials
				for (int p = 0; p < SG_MAXP; p++) {
					float n = (float)(p + 1);
					float v = 0.02f / n;                  // a little glottal floor
					for (int k = 0; k < 3; k++) {
						float d = (n - FRQ[k] / 261.6f) / BW[k];
						v += AMP[k] * std::exp(-d * d);
					}
					pLevel[p] = clamp(v, 0.f, 1.f);
					pSoft[p]  = pLevel[p] * (0.4f + 0.6f / n);
					pRate[p]  = 0.5f; pDepth[p] = 1.f;
				}
				params[ENVRATE_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.05f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.3f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.9f);
				params[RELEASE_PARAM].setValue(timeKnob(0.2f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.25f);
				break;
			}

			case 19: {  // ACID -- the only preset where the spectral filter IS
				// the instrument. A saw, the cutoff parked low, resonance up,
				// and the envelope driving CUT hard from the matrix. Worth
				// contrasting with a real ladder: this filter is evaluated per
				// partial rather than run as a difference equation, so it cannot
				// self-oscillate and it cannot distort -- what it can do is be
				// exactly the shape it claims to be.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / (float)(p + 1);
					pSoft[p]  = 1.f / (float)(p + 1);
					pRate[p]  = 0.5f; pDepth[p] = 1.f;
				}
				params[ENVRATE_PARAM].setValue(0.f);
				// TILT is POSITIVE FOR DARKER: lv *= n^(-tilt*2.5), so a positive
				// value pulls the high partials down. A small amount takes the
				// glare off the top without touching the filter.
				params[TILT_PARAM].setValue(0.18f);
				params[CUTOFF_PARAM].setValue(cutKnob(170.f));
				params[RESO_PARAM].setValue(0.85f);
				mod[3][SG_MOD_CUT] = 0.78f;
				params[ATTACK_PARAM].setValue(timeKnob(0.002f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.19f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.05f);
				params[RELEASE_PARAM].setValue(timeKnob(0.15f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.f);
				break;
			}

			case 20: {  // ENSEMBLE -- PITCH as fine detune, the counterpart to
				// Marimba's coarse retuning. Each partial is a few cents off its
				// harmonic, by a fixed irrational-stride pattern rather than by
				// random(), so the preset is the same every time it is loaded.
				// LFO1 works pitch with its spread wide, which is what keeps the
				// detuning from settling into one steady chorus.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 1.f / (float)(p + 1);
					pSoft[p]  = 1.f / std::pow((float)(p + 1), 1.4f);
					pRate[p]  = 0.5f; pDepth[p] = 1.f;
					// +/- 9 cents. pPitch spans +/- 2400, so this is tiny by
					// design -- the tab is coarse enough for a marimba bar and
					// still has to hold this.
					pPitch[p] = (9.f / 2400.f) * std::sin((float)p * 2.3999632f);
					pPan[p]   = 0.55f * std::sin((float)p * 1.1f + 0.7f);
				}
				params[ENVRATE_PARAM].setValue(0.05f);
				params[ATTACK_PARAM].setValue(timeKnob(0.25f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(1.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.8f);
				params[RELEASE_PARAM].setValue(timeKnob(1.2f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.6f);
				mod[0][SG_MOD_PITCH] = 0.05f;
				params[LFORATE_PARAM + 0].setValue(lfoKnob(0.35f));
				params[LFOSPREAD_PARAM + 0].setValue(1.f);
				break;
			}

			case 4: {  // VIBRAPHONE -- the MOTOR. A vibraphone's tremolo is
				// not an effect on the tone, it is discs spinning in the
				// resonator tubes under the bars, and it is the one classic
				// additive sound Sigma could not make: nothing else here uses
				// an LFO on LEVEL as the point of the patch.
				//
				// Aluminium bars are undercut like a marimba's, so the same
				// 1 : 4 : 10 -- but far purer, and they ring for seconds.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 0.f; pSoft[p] = 0.f; pPitch[p] = 0.f;
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				pLevel[0] = 1.f;    pSoft[0] = 1.f;
				pLevel[1] = 0.22f;  pSoft[1] = 0.10f;  pPitch[1] = 0.5f;
				pLevel[2] = 0.05f;  pSoft[2] = 0.02f;  pPitch[2] = std::log2(10.f / 3.f) / 2.f;
				pRate[1] = 0.66f; pRate[2] = 0.78f;
				params[ENVRATE_PARAM].setValue(0.30f);
				params[ATTACK_PARAM].setValue(timeKnob(0.003f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(4.5f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.f);
				params[RELEASE_PARAM].setValue(timeKnob(3.5f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.25f);
				mod[0][SG_MOD_LEVEL] = 0.38f;                  // the discs
				params[LFORATE_PARAM + 0].setValue(lfoKnob(5.2f));
				params[LFOSPREAD_PARAM + 0].setValue(0.f);     // one motor, not sixteen
				break;
			}

			case 7: {  // TUBULAR BELLS -- a different bell entirely. Where a cast
				// bell is tuned to hum/prime/tierce, a hanging tube rings at
				// 1 : 2.76 : 5.40 : 8.93, and those upper three are near enough
				// to 2:4:6 that the ear invents a fundamental an octave BELOW
				// the tube's own -- which is why chimes sound lower than they
				// measure. Nothing else here puts the PITCH tab to this use.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 0.f; pSoft[p] = 0.f; pPitch[p] = 0.f;
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				static const float TB[5] = {1.f, 2.76f, 5.40f, 8.93f, 13.34f};
				static const float TL[5] = {0.55f, 1.f, 0.62f, 0.34f, 0.16f};
				for (int p = 0; p < 5; p++) {
					pLevel[p] = TL[p];
					pSoft[p]  = TL[p] * (p < 2 ? 0.85f : 0.3f);
					pPitch[p] = std::log2(TB[p] / (float)(p + 1)) / 2.f;
					pRate[p]  = clamp(0.5f + 0.06f * (float)p, 0.f, 1.f);
				}
				params[ENVRATE_PARAM].setValue(0.30f);
				params[ATTACK_PARAM].setValue(timeKnob(0.0015f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(9.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.f);
				params[RELEASE_PARAM].setValue(timeKnob(7.f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.4f);
				break;
			}

			case 12: {  // CHOIR -- the case where per-voice RANDOM matters most.
				// A chord whose vibratos all lock reads as one machine; the
				// same chord with each voice drifting on its own reads as
				// people. That is the Synergy's aperiodic vibrato, and a choir
				// is what it is for.
				nPartials = 32;
				static const float FQ[3] = {730.f, 1150.f, 2600.f};   // "ah"
				static const float AM[3] = {1.f, 0.55f, 0.16f};
				static const float BW[3] = {1.3f, 1.6f, 2.4f};
				for (int p = 0; p < SG_MAXP; p++) {
					float n = (float)(p + 1);
					float v = 0.05f / n;
					for (int k = 0; k < 3; k++) {
						float d = (n - FQ[k] / 261.6f) / BW[k];
						v += AM[k] * std::exp(-d * d);
					}
					pLevel[p] = clamp(v, 0.f, 1.f);
					pSoft[p]  = pLevel[p] * (0.35f + 0.65f / n);
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				params[ENVRATE_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.28f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.8f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.88f);
				params[RELEASE_PARAM].setValue(timeKnob(0.6f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(-0.55f);          // a body, not a fan
				mod[0][SG_MOD_PITCH] = 0.045f;
				params[LFORATE_PARAM + 0].setValue(lfoKnob(4.6f));
				params[LFODELAY_PARAM + 0].setValue(0.22f);
				params[LFORAND_PARAM + 0].setValue(0.75f);     // each singer alone
				mod[1][SG_MOD_LEVEL] = 0.12f;
				params[LFORATE_PARAM + 1].setValue(lfoKnob(0.6f));
				params[LFORAND_PARAM + 1].setValue(0.9f);
				break;
			}

			case 15: {  // TOUCH SWITCH -- two instruments, chosen by how hard you
				// play. MORPH SENS past halfway stops blending and starts
				// switching, so slow gives one spectrum and fast gives the
				// other with very little in between. It is the Synergy's
				// headline trick and it only means anything if the two spectra
				// are genuinely different, so they are: a near-sine and a
				// bright odd-harmonic reed.
				for (int p = 0; p < SG_MAXP; p++) {
					float n = (float)(p + 1);
					pSoft[p]  = (p == 0) ? 1.f : ((p == 2) ? 0.06f : 0.f);
					pLevel[p] = (p & 1) ? 0.f : 1.f / std::pow(n, 0.85f);
					pRate[p] = 0.5f; pDepth[p] = 1.f; pPitch[p] = 0.f;
				}
				params[MORPHSENS_PARAM].setValue(1.f);         // a switch
				params[MORPH_PARAM].setValue(0.f);             // centred on the threshold
				params[AMPSENS_PARAM].setValue(0.25f);         // level barely moves...
				params[AMPCENTER_PARAM].setValue(0.85f);       // ...so the TIMBRE is the news
				params[ENVRATE_PARAM].setValue(0.1f);
				params[ATTACK_PARAM].setValue(timeKnob(0.02f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.5f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.8f);
				params[RELEASE_PARAM].setValue(timeKnob(0.3f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.3f);
				break;
			}

			case 17: {  // CLOUD -- WIDTH fully NEGATIVE. The fan opens outward in
				// order and you hear it as a shape; the scatter puts every
				// partial somewhere of its own and you hear a width you are
				// inside. Nothing else uses it, and a slow 64-partial bloom is
				// what it is for.
				nPartials = 64;
				for (int p = 0; p < SG_MAXP; p++) {
					float n = (float)(p + 1);
					pLevel[p] = 1.f / std::pow(n, 0.85f);
					pSoft[p]  = 1.f / std::pow(n, 1.5f);
					pRate[p]  = 0.3f;
					pDepth[p] = (p >= 30) ? -0.45f : 1.f;
					pPitch[p] = 0.f;
				}
				params[STRETCH_PARAM].setValue(0.12f);
				params[ENVSPREAD_PARAM].setValue(0.8f);
				params[ENVRATE_PARAM].setValue(0.08f);
				params[ATTACK_PARAM].setValue(timeKnob(1.6f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(6.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.55f);
				params[RELEASE_PARAM].setValue(timeKnob(4.f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(-1.f);
				params[CUTOFF_PARAM].setValue(cutKnob(6000.f));
				params[TILT_PARAM].setValue(0.12f);
				mod[2][SG_MOD_LEVEL] = 0.15f;
				params[LFORATE_PARAM + 2].setValue(lfoKnob(0.09f));
				params[LFOSPREAD_PARAM + 2].setValue(1.f);
				params[LFORAND_PARAM + 2].setValue(0.5f);
				break;
			}

			case 2: {  // DRAWBAR ORGAN -- the plainest thing in the set, and the
				// reference the others are heard against. A Hammond is additive
				// synthesis with nine sliders and no envelope at all, so this
				// has no spread, no rate spread, and NO VELOCITY: amplitude
				// sensitivity at zero, which turns the centre into a plain
				// level control. That is the Synergy's pair doing its second
				// job, and an organ is the case that needs it.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 0.f; pSoft[p] = 0.f; pPitch[p] = 0.f;
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				// 8' 4' 2-2/3' 2' 1-3/5' 1-1/3' 1'  -- harmonics 1 2 3 4 5 6 8
				static const int  DH[7] = {0, 1, 2, 3, 4, 5, 7};
				static const float DL[7] = {1.f, 0.8f, 0.55f, 0.45f, 0.2f, 0.16f, 0.28f};
				for (int k = 0; k < 7; k++) { pLevel[DH[k]] = DL[k]; pSoft[DH[k]] = DL[k]; }
				params[ENVRATE_PARAM].setValue(0.f);
				params[ENVSPREAD_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.006f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.02f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(1.f);
				params[RELEASE_PARAM].setValue(timeKnob(0.02f, 0.005f, 16.f));
				params[AMPSENS_PARAM].setValue(0.f);           // no touch at all
				params[AMPCENTER_PARAM].setValue(0.8f);        // so this IS the level
				params[MORPHSENS_PARAM].setValue(0.f);
				params[WIDTH_PARAM].setValue(0.15f);
				break;
			}

			case 8: {  // SINGING BOWL -- Bell's partials with Bowed's envelope.
				// Both already exist and their intersection is neither: a bowl
				// has no strike at all, it swells from nothing and keeps going
				// long after you stop. The beating between the low partials is
				// the sound, so the rate spread stays near zero -- if the highs
				// die first there is nothing left to beat against.
				for (int p = 0; p < SG_MAXP; p++) {
					pLevel[p] = 0.f; pSoft[p] = 0.f; pPitch[p] = 0.f;
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				static const float SBR[6] = {1.f, 2.32f, 4.25f, 6.63f, 9.38f, 12.2f};
				static const float SBL[6] = {1.f, 0.5f, 0.3f, 0.16f, 0.09f, 0.05f};
				for (int p = 0; p < 6; p++) {
					pLevel[p] = SBL[p];
					pSoft[p]  = SBL[p] * 0.6f;
					pPitch[p] = std::log2(SBR[p] / (float)(p + 1)) / 2.f;
				}
				params[ENVRATE_PARAM].setValue(0.06f);
				params[ENVSPREAD_PARAM].setValue(-0.3f);
				params[ATTACK_PARAM].setValue(timeKnob(1.2f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(8.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.55f);
				params[RELEASE_PARAM].setValue(timeKnob(9.f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.5f);
				mod[2][SG_MOD_LEVEL] = 0.10f;
				params[LFORATE_PARAM + 2].setValue(lfoKnob(0.18f));
				params[LFOSPREAD_PARAM + 2].setValue(0.7f);
				params[LFORAND_PARAM + 2].setValue(0.35f);
				break;
			}

			case 9: {  // GONG
				// The trippy Gong slides downward through its whole tail, and it
				// is worth being precise about why, because NO PARTIAL MOVES.
				// ENV RATE makes the top of the spectrum die up to eleven times
				// faster than the bottom; with the partials stretched as far as
				// they are, the centre of mass of what is left slides down, and
				// the ear hears a centre of mass. It is a real effect and it is
				// not a gong.
				//
				// A struck tam-tam does the OPPOSITE. Energy migrates UPWARD
				// over the first seconds -- the shimmer that builds after the
				// strike rather than arriving with it -- because the plate is
				// driven hard enough to couple its modes nonlinearly. Sigma
				// cannot model that coupling, but it can state its outcome:
				// negative DEPTH on the upper partials, so they follow the
				// envelope INVERTED and bloom in as the fundamental falls.
				//
				// That both removes the downward slide and puts the real
				// behaviour in its place, which is a better trade than simply
				// turning ENV RATE down.
				nPartials = 64;
				for (int p = 0; p < SG_MAXP; p++) {
					float n = (float)(p + 1);
					pLevel[p] = 1.f / std::pow(n, 0.75f);
					pSoft[p]  = 1.f / std::pow(n, 1.35f);
					pRate[p]  = 0.5f;
					// the top third arrives late and backwards -- the shimmer
					pDepth[p] = (p >= 40) ? -0.55f : ((p >= 24) ? -0.25f : 1.f);
				}
				// Half the old stretch. At 0.85 only 27 of the 64 partials were
				// under Nyquist at C4; at 0.45 that is 45, so the spectrum the
				// preset asks for is closer to the one it gets.
				params[STRETCH_PARAM].setValue(0.45f);
				// The spread is what tilted the tail. Small, not zero: a plate
				// does lose its top eventually, just not eleven times over.
				params[ENVRATE_PARAM].setValue(0.12f);
				params[ENVSPREAD_PARAM].setValue(0.55f);   // the bloom takes time
				params[ATTACK_PARAM].setValue(timeKnob(0.006f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(11.f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.f);
				params[RELEASE_PARAM].setValue(timeKnob(9.f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.8f);
				// THE SQUISH WAS THIS. A random LFO on STRETCH moves every
				// partial's pitch at once and by a different amount each --
				// which is not a plate breathing, it is the whole spectrum
				// sliding around underneath itself. Slower, a third the depth,
				// and periodic rather than random, so it drifts instead of
				// wobbling.
				mod[2][SG_MOD_STRETCH] = 0.035f;
				params[LFORATE_PARAM + 2].setValue(lfoKnob(0.035f));
				params[LFORAND_PARAM + 2].setValue(0.f);
				// And the top needed taking off. 64 stretched partials put real
				// energy above 10kHz where the ear is least forgiving, and the
				// bloom brings MORE of it in as the note decays rather than
				// less -- so a preset that grows its own top end has to have a
				// ceiling.
				params[CUTOFF_PARAM].setValue(cutKnob(5200.f));
				params[TILT_PARAM].setValue(0.22f);
				break;
			}
		}
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;

		auto mac = [&](int pid, int iid, float lo, float hi) {
			float v = params[pid].getValue();
			if (inputs[iid].isConnected()) v += inputs[iid].getVoltage() * 0.1f * (hi - lo);
			return clamp(v, lo, hi);
		};
		float tilt    = mac(TILT_PARAM, TILT_INPUT, -1.f, 1.f);
		float oddEven = mac(ODDEVEN_PARAM, ODDEVEN_INPUT, -1.f, 1.f);
		float stretch = mac(STRETCH_PARAM, STRETCH_INPUT, 0.f, 1.f);
		float width   = mac(WIDTH_PARAM, WIDTH_INPUT, -1.f, 1.f);
		float morphK  = mac(MORPH_PARAM, MORPH_INPUT, -1.f, 1.f);
		float envRate = mac(ENVRATE_PARAM, ENVRATE_INPUT, 0.f, 1.f);
		float envSpr  = mac(ENVSPREAD_PARAM, ENVSPREAD_INPUT, -1.f, 1.f);
		dispMorph = morphK;

		float A = knobTime(params[ATTACK_PARAM].getValue(),  0.001f, 8.f);
		float D = knobTime(params[DECAY_PARAM].getValue(),   0.005f, 12.f);
		float S = params[SUSTAIN_PARAM].getValue();
		float R = knobTime(params[RELEASE_PARAM].getValue(), 0.005f, 16.f);

		// ── LFOs ────────────────────────────────────────────────────────────
		float lfoSpread[3], lfoFlat[3], lfoHz[3], lfoRand[3], lfoDelay[3];
		for (int i = 0; i < 3; i++) {
			if (lfoSyncTrig[i].process(inputs[LFOSYNC_INPUT + i].getVoltage(), 0.1f, 2.f))
				lfoPhase[i] = 0.f;
			lfoHz[i] = knobTime(params[LFORATE_PARAM + i].getValue(), 0.02f, 30.f);
			lfoPhase[i] += lfoHz[i] * dt;
			lfoPhase[i] -= std::floor(lfoPhase[i]);
			lfoSpread[i] = params[LFOSPREAD_PARAM + i].getValue();
			lfoRand[i]  = params[LFORAND_PARAM + i].getValue();
			lfoDelay[i] = params[LFODELAY_PARAM + i].getValue() * 3.f;   // seconds
			// TILT, STRETCH and CUT are whole-spectrum controls, so they take
			// the LFO flat. Spread only means something for a destination that
			// exists once per partial.
			lfoFlat[i]   = sgSin(lfoPhase[i]);
		}
		float cutK = params[CUTOFF_PARAM].getValue();
		if (inputs[CUTOFF_INPUT].isConnected()) cutK += inputs[CUTOFF_INPUT].getVoltage() * 0.1f;
		float reso = clamp(params[RESO_PARAM].getValue()
		                 + inputs[RESO_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float baseTilt = tilt, baseStretch = stretch;

		// STRETCH by the real law: f_n = n*f0*sqrt(1 + B*n^2), which is piano
		// string stiffness. B ~ 1e-4 for a piano, far more for a bell, so one
		// knob runs from pure harmonic through piano to gong. Resolved per
		// voice, below.

		int nch = std::max(inputs[GATE_INPUT].getChannels(), 1);
		nch = std::min(nch, SG_VOICES);

		// The screen follows the LOWEST sounding voice rather than the newest:
		// a display that jumps to whichever note was struck last is unreadable
		// while a chord is held.
		int dispVoice = -1;
		for (int v = 0; v < SG_VOICES; v++)
			if (voice[v].mStage != ST_IDLE) { dispVoice = v; break; }
		liveOn = (dispVoice >= 0);
		if (!liveOn) for (int p = 0; p < nPartials; p++) { liveAmp[p] = 0.f; liveEnv[p] = 0.f; }

		float outL = 0.f, outR = 0.f;
		float nyq = args.sampleRate * 0.5f;
		float fadeLo = nyq * 0.72f;                 // start fading an octave down

		// ── gate pass: channels strike voices ──────────────────────────────
		// Allocation, NOT a fixed channel->voice mapping. Sigma is a struck
		// voice with releases measured in seconds -- the Gong preset rings for
		// ten -- so a mono gate line pinned to voice 0 meant every note cut off
		// the one before it and the module could not be polyphonic at all
		// unless you fed it a poly cable. A note-on now takes an idle voice if
		// there is one, and otherwise steals the quietest, which is the one
		// nobody will miss.
		for (int c = 0; c < nch; c++) {
			bool gate = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
			int v = chanVoice[c];
			if (gate && v < 0) {
				int pick = -1;
				if (allocMode == ALLOC_ROLL) {
					// step to the next voice that no channel is holding
					for (int k = 0; k < SG_VOICES; k++) {
						int cand = (rollNext + k) % SG_VOICES;
						if (!voice[cand].on) { pick = cand; break; }
					}
					rollNext = (pick + 1) % SG_VOICES;
				}
				for (int k = 0; pick < 0 && k < SG_VOICES; k++)
					if (voice[k].mStage == ST_IDLE) { pick = k; break; }
				if (pick < 0) {
					float q = 1e9f;
					for (int k = 0; k < SG_VOICES; k++)
						if (!voice[k].on && voice[k].mEnv < q) { q = voice[k].mEnv; pick = k; }
					if (pick < 0) {                 // every voice still held
						for (int k = 0; k < SG_VOICES; k++)
							if (voice[k].mEnv < q) { q = voice[k].mEnv; pick = k; }
					}
				}
				chanVoice[c] = v = pick;
				Voice& V = voice[v];
				V.on = true;
				// LATCHED AT NOTE-ON, both of them. Reading V/OCT every sample
				// meant a voice that had been released still tracked the input,
				// so a mono sequencer moving to the next note dragged the tone
				// still ringing along with it -- audible as a pitch bend on
				// every note, and worst on the presets that ring longest.
				// getPolyVoltage, not getVoltage: a mono V/OCT feeding a poly
				// gate would otherwise read 0V on every channel above the first
				// and put the whole chord on middle C.
				V.pitch = inputs[VOCT_INPUT].getPolyVoltage(c);
				V.vel = inputs[VEL_INPUT].isConnected()
				      ? clamp(inputs[VEL_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f) : 1.f;
				// Latched with the pitch, and for the same reason: a note's
				// loudness is decided when it is struck. Reading it live would
				// make a held chord swell as the next note's velocity arrived.
				V.ampGain = touch(V.vel, params[AMPSENS_PARAM].getValue(),
				                  params[AMPCENTER_PARAM].getValue());
				V.age = 0.f;
				// A PERIODIC LFO tracks the one already sounding; a RANDOM one
				// does not. Straight from the Synergy, and it is the difference
				// between a vibraphone (every bar shimmering together) and a
				// section of players (each drifting on their own).
				V.rng = 0x9E3779B9u ^ (uint32_t)(v * 2654435761u) ^ 1u;
				for (int i = 0; i < 3; i++) {
					float rnd = params[LFORAND_PARAM + i].getValue();
					V.lfoPh[i] = rnd < 0.001f ? lfoPhase[i] : std::fabs(vrand(V.rng)) ;
					V.rndCur[i] = vrand(V.rng); V.rndNext[i] = vrand(V.rng);
				}
				for (int p = 0; p < nPartials; p++) {
					// ATTACK FROM WHERE THE ENVELOPE ALREADY IS, not from zero.
					// `g = V.mEnv` IS the VCA, so zeroing it on a note-on threw
					// the output to silence in a single sample whenever the
					// voice was still sounding. Measured across the presets the
					// step was 0.55 to 0.94 of full scale on thirteen of
					// fourteen. That was the clicking.
					V.stage[p] = ST_ATT;
					V.envMax[p] = V.env[p];   // the bloom restarts with the note
					// ENV SPREAD: positive delays the HIGH partials so the tone
					// blooms upward; negative delays the LOW ones so the highs
					// speak first and the fundamental builds underneath, which
					// is what a bowed or blown onset actually does.
					float f = (float)p / (float)(nPartials - 1);
					// 0.25s, not 0.5s. Negative spread delays the LOW partials
					// and the low partials carry nearly all the energy, so at
					// half a second the note simply appeared not to start.
					V.wait[p] = std::fabs(envSpr) * 0.25f * (envSpr >= 0.f ? f : 1.f - f);
				}
				V.mStage = ST_ATT;
			} else if (!gate && v >= 0) {
				Voice& V = voice[v];
				V.on = false;
				for (int p = 0; p < nPartials; p++) {
					V.stage[p] = ST_REL; V.relFrom[p] = std::max(V.env[p], 1e-4f);
				}
				V.mStage = ST_REL; V.mRelFrom = std::max(V.mEnv, 1e-4f);
				chanVoice[c] = -1;      // the voice rings on; the channel is free
			}
		}
		for (int c = nch; c < SG_VOICES; c++) chanVoice[c] = -1;

		// ── synthesis pass: EVERY voice, not just the patched channels ──────
		// A released voice is still sounding, so the loop that renders them
		// cannot be bounded by the cable's channel count.
		for (int c = 0; c < SG_VOICES; c++) {
			Voice& V = voice[c];
			if (V.mStage == ST_IDLE && !V.on) continue;

			float f0 = 261.6256f * std::pow(2.f, V.pitch);
			// MORPH is the CENTRE of the timbre range; MORPH SENS is how much
			// of it velocity commands. At sens 0.5 with morph at zero this is
			// exactly the old `vel + morphK`.
			float mSens = params[MORPHSENS_PARAM].getValue();
			float morph = touchMorph(V.vel, mSens, clamp(0.5f + morphK * 0.5f, 0.f, 1.f));

			// The whole-spectrum controls are resolved PER VOICE, because the
			// envelope is per voice: a held chord whose tilt followed whichever
			// note was struck last would be one voice modulating the others.
			// ── this voice's own LFOs ──────────────────────────────────────
			V.age += dt;
			float vLfo[3], vLfoSpread[3];
			for (int i = 0; i < 3; i++) {
				V.lfoPh[i] += lfoHz[i] * dt;
				if (V.lfoPh[i] >= 1.f) {
					V.lfoPh[i] -= std::floor(V.lfoPh[i]);
					// a fresh target once per cycle, so RANDOM moves at the
					// rate the knob says rather than at the sample rate
					V.rndCur[i] = V.rndNext[i];
					V.rndNext[i] = vrand(V.rng);
				}
				float per = sgSin(V.lfoPh[i]);
				// A random LFO is a walk between held targets, not noise: the
				// Synergy calls it "aperiodic vibrato", and what makes it read
				// as vibrato at all is that it still moves at the vibrato rate.
				float rnd = V.rndCur[i] + (V.rndNext[i] - V.rndCur[i]) * V.lfoPh[i];
				float v = per + (rnd - per) * lfoRand[i];
				// DELAY: nothing, then a fade in over a third of a second. An
				// abrupt start is what makes vibrato sound applied rather than
				// played, and it is the one thing the LFOs had no answer for.
				float d = lfoDelay[i];
				float ramp = d <= 0.f ? 1.f
				           : clamp((V.age - d) / 0.3f, 0.f, 1.f);
				vLfo[i] = v * ramp;
				vLfoSpread[i] = lfoSpread[i];
			}
			(void)vLfoSpread;
			float mSrc[SG_MODSRC] = {vLfo[0], vLfo[1], vLfo[2], V.mEnv};
			float tilt = baseTilt, stretch = baseStretch, cutM = 0.f;
			for (int i = 0; i < SG_MODSRC; i++) {
				tilt    += mSrc[i] * mod[i][SG_MOD_TILT];
				stretch += mSrc[i] * mod[i][SG_MOD_STRETCH];
				cutM    += mSrc[i] * mod[i][SG_MOD_CUT];
			}
			tilt = clamp(tilt, -1.f, 1.f);
			stretch = clamp(stretch, 0.f, 1.f);
			float B = stretch * stretch * 0.02f;
			// Cutoff runs 30Hz to well past Nyquist so the top of the knob is
			// genuinely open rather than "nearly open".
			float fc = 30.f * std::pow(2.f, clamp(cutK, 0.f, 1.f) * 10.f + cutM * 4.f);
			float Q  = 0.5f + reso * 8.f;

			// the master envelope: unscaled, drives the VCA and frees the voice
			advance(V.mEnv, V.mStage, V.mRelFrom, 1.f, 1.f, dt, A, D, S, R);
			if (V.mStage == ST_IDLE) { V.mEnv = 0.f; continue; }

			float vL = 0.f, vR = 0.f;
			for (int p = 0; p < nPartials; p++) {
				int n = p + 1;

				// per-partial envelope, at its own rate and after its own wait
				float rateOwn = 0.25f * std::pow(16.f, pRate[p]);   // 0.25x..4x
				float rateDie = rateOwn * (1.f + envRate * (float)p * 0.35f);
				if (V.wait[p] > 0.f) { V.wait[p] -= dt; }
				else advance(V.env[p], V.stage[p], V.relFrom[p],
				             rateOwn, rateDie, dt, A, D, S, R);

				// ── level ───────────────────────────────────────────────────
				float lv = pSoft[p] + (pLevel[p] - pSoft[p]) * morph;
				lv *= std::pow((float)n, -tilt * 2.5f);
				lv *= (n % 2) ? (1.f - std::max(0.f, oddEven))     // odd
				              : (1.f - std::max(0.f, -oddEven));   // even
				// DEPTH is BIPOLAR. Above zero the partial follows its envelope
				// as before. Below zero it follows the INVERSE, so against the
				// master envelope it blooms in the middle of the note instead
				// of at the front of it -- partials arriving as others leave,
				// which is the spectral evolution additive is actually for and
				// which a 0..1 control cannot ask for at all.
				float d = pDepth[p];
				float envAmt;
				if (d >= 0.f) {
					envAmt = 1.f - d + d * V.env[p];
				} else {
					// BLOOM FROM SILENCE, not from full. `1 + d*env` is at its
					// maximum when the envelope is at zero -- which is the end
					// of the note, as intended, but ALSO the beginning of it.
					// The master envelope opens in a few milliseconds, so for
					// those milliseconds the entire voice was its inverted
					// partials at full level, and on Gong those are the high
					// inharmonic ones. That was the squelch on the transient:
					// not a click, a burst of the wrong partials.
					//
					// Measuring the distance the envelope has FALLEN from its
					// own peak is zero before the note and zero at the peak,
					// and only opens as the note decays. Which is what a bloom
					// is.
					if (V.env[p] > V.envMax[p]) V.envMax[p] = V.env[p];
					envAmt = -d * (V.envMax[p] - V.env[p]);
				}
				lv *= envAmt;

				// ── pitch ───────────────────────────────────────────────────
				float ratio = (float)n * std::sqrt(1.f + B * (float)(n * n));
				// +/- TWO OCTAVES. The tab's -1..1 was being used as cents
				// directly, so the whole control spanned two cents and could
				// not retune a partial to anything. Two octaves is what the
				// real modal ratios need: a marimba bar is undercut so its
				// first overtone lands at 4x rather than 2x (+1200 cents) and
				// its second near 10x (+2084), and at one octave that second
				// one was reachable by a preset but not by hand.
				float cents = pPitch[p] * 2400.f;

				// ── pan ─────────────────────────────────────────────────────
				// WIDTH GENERATES the spread; pPan is the deviation from it.
				// It used to be pPan * width, and every pPan defaults to zero,
				// so the knob was multiplying nothing and did nothing at all
				// until you had drawn a pan curve by hand. Partials fan
				// alternately left and right, further out as they climb, which
				// is the same "panel is a spread, screen is the exceptions"
				// rule as every other macro.
				// Right: an ordered fan, alternating sides and widening with the
				// partial index, so the spectrum opens outward. Left: a fixed
				// scatter, each partial at its own place. The fan is a shape you
				// hear move; the scatter is a width you sit inside.
				float place = (width >= 0.f)
				            ? ((p & 1) ? 1.f : -1.f)
				              * (float)p / (float)std::max(nPartials - 1, 1)
				            : sgScatter(p);
				float pan = clamp(place * std::fabs(width) + pPan[p], -1.f, 1.f);

				// ── LFOs, spread across the partial index ───────────────────
				for (int i = 0; i < SG_MODSRC; i++) {
					// The three LFOs are spread across the partial index; the
					// envelope is not, because a note does not start at a
					// different time for each partial -- ENV SPREAD already
					// owns that idea and owning it twice would fight.
					float m;
					if (i < 3) {
						float ph = V.lfoPh[i] + lfoSpread[i] * (float)p / (float)nPartials;
						ph -= std::floor(ph);
						m = sgSin(ph);
					} else m = V.mEnv;
					lv    *= clamp(1.f + m * mod[i][SG_MOD_LEVEL], 0.f, 2.f);
					cents += m * mod[i][SG_MOD_PITCH] * 50.f;
					pan    = clamp(pan + m * mod[i][SG_MOD_PAN], -1.f, 1.f);
				}

				float f = f0 * ratio * std::pow(2.f, cents / 1200.f);
				// The second-order lowpass magnitude, evaluated at this
				// partial's own frequency. Resonance is a real peak at fc, not
				// a fake one -- the maths is the same as the filter's.
				{
					float w = f / std::max(fc, 1.f);
					float w2 = w * w;
					float den = std::sqrt((1.f - w2) * (1.f - w2) + w2 / (Q * Q));
					lv *= 1.f / std::max(den, 0.02f);
				}
				// FADE across Nyquist, never cut: sixteen partials on a 1kHz
				// note already puts the top one at 16k, and a pitch sweep walks
				// them through the ceiling. A hard mute clicks.
				bool over = (f >= nyq);
				if (!over && f > fadeLo) lv *= (nyq - f) / (nyq - fadeLo);

				// PUBLISHED BEFORE THE SKIP. The `continue` for a partial past
				// Nyquist used to jump this, so those partials kept whatever
				// they last reported and their dots froze on the pan display --
				// and the ones past Nyquist are always the high ones, which is
				// why only part of the picture stuck.
				if (c == dispVoice) {
					liveAmp[p]   = over ? 0.f : lv;
					liveEnv[p]   = V.env[p];
					livePan[p]   = pan;
					liveCents[p] = 1200.f * std::log2(std::max(ratio / (float)n, 1e-6f)) + cents;
				}
				if (over) continue;
				V.phase[p] += f * dt;
				V.phase[p] -= std::floor(V.phase[p]);
				float s = sgSin(V.phase[p]) * lv;

				float pl = std::sqrt(0.5f * (1.f - pan));   // equal power
				float pr = std::sqrt(0.5f * (1.f + pan));
				vL += s * pl; vR += s * pr;
			}

			// VELOCITY REACHES THE LEVEL AT LAST. Until now V.vel fed the morph
			// and nothing else, so a pianissimo note was exactly as loud as a
			// fortissimo one -- it simply had a different spectrum. That is a
			// defensible purist position but it was never a decision, and there
			// was no way to ask for the ordinary behaviour.
			float g = V.mEnv * V.ampGain;
			outL += vL * g; outR += vR * g;
		}

		float vca = inputs[VCA_INPUT].isConnected()
		          ? clamp(inputs[VCA_INPUT].getVoltage() * 0.1f, 0.f, 1.f) : 1.f;
		// Sixteen partials summed is a much bigger number than one oscillator,
		// and the tilt default already tapers them, so this is a fixed trim
		// rather than a normaliser -- a normaliser would pump with the spectrum.
		float trim = 1.6f * vca;
		float fl = sgSoftClip(outL * trim);
		float fr = sgSoftClip(outR * trim);
		outputs[L_OUTPUT].setVoltage(fl);
		outputs[R_OUTPUT].setVoltage(fr);
	}

	// One linear ADSR segment. Linear rather than exponential for v1: with the
	// per-partial rate spread doing the perceptual work, curve shape is a
	// second-order question -- see the design doc.
	// TWO RATES, because the ENV RATE spread is a claim about DYING. Its whole
	// description is "highs die sooner", and running it on the attack as well
	// meant the top of a 64-partial Gong opened in 92 microseconds -- 43x faster
	// than the master envelope, and far below one cycle of anything it was
	// playing. An envelope segment shorter than a cycle is a step no matter how
	// smooth the maths is, and a step is a click.
	//
	// A partial's OWN rate (the RATE tab) still scales everything, because that
	// is a per-partial envelope speed and a user who sets it fast means all of
	// it. Only the macro spread is confined to decay and release.
	static void advance(float& e, int& st, float& relFrom,
	                    float rateAtt, float rateDie, float dt,
	                    float A, float D, float S, float R) {
		switch (st) {
			case ST_ATT:
				e += dt * rateAtt / std::max(A, 1e-4f);
				if (e >= 1.f) { e = 1.f; st = ST_DEC; }
				break;
			case ST_DEC:
				e -= dt * rateDie * (1.f - S) / std::max(D, 1e-4f);
				if (e <= S) { e = S; st = ST_SUS; }
				break;
			case ST_SUS: e = S; break;
			case ST_REL:
				e -= dt * rateDie * relFrom / std::max(R, 1e-4f);
				if (e <= 0.f) { e = 0.f; st = ST_IDLE; }
				break;
			default: e = 0.f; break;
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		const char* keys[6] = {"level", "soft", "pitch", "pan", "depth", "rate"};
		float* arrs[6] = {pLevel, pSoft, pPitch, pPan, pDepth, pRate};
		for (int k = 0; k < 6; k++) {
			json_t* a = json_array();
			for (int p = 0; p < nPartials; p++) json_array_append_new(a, json_real(arrs[k][p]));
			json_object_set_new(root, keys[k], a);
		}
		json_t* mm = json_array();
		for (int r = 0; r < SG_MODSRC; r++)
			for (int d = 0; d < SG_MOD_N; d++) json_array_append_new(mm, json_real(mod[r][d]));
		json_object_set_new(root, "mod", mm);
		json_object_set_new(root, "tab", json_integer(dispTab));
		json_object_set_new(root, "nPartials", json_integer(nPartials));
		json_object_set_new(root, "allocMode", json_integer(allocMode));
		json_object_set_new(root, "pitchSnap", json_integer(pitchSnap));
		return root;
	}
	void dataFromJson(json_t* root) override {
		const char* keys[6] = {"level", "soft", "pitch", "pan", "depth", "rate"};
		float* arrs[6] = {pLevel, pSoft, pPitch, pPan, pDepth, pRate};
		for (int k = 0; k < 6; k++) {
			json_t* a = json_object_get(root, keys[k]);
			if (!a) continue;
			for (int p = 0; p < nPartials && p < (int)json_array_size(a); p++)
				arrs[k][p] = (float)json_real_value(json_array_get(a, p));
		}
		if (json_t* mm = json_object_get(root, "mod"))
			for (int i = 0; i < SG_MODSRC; i++)
				for (int d = 0; d < SG_MOD_N; d++) {
					int k = i * SG_MOD_N + d;
					if (k < (int)json_array_size(mm))
						mod[i][d] = (float)json_real_value(json_array_get(mm, k));
				}
		if (json_t* j = json_object_get(root, "tab"))
			dispTab = clamp((int)json_integer_value(j), 0, 4);
		if (json_t* j = json_object_get(root, "nPartials"))
			nPartials = clamp((int)json_integer_value(j), 1, SG_MAXP);
		if (json_t* j = json_object_get(root, "allocMode"))
			allocMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(root, "pitchSnap"))
			pitchSnap = clamp((int)json_integer_value(j), 0, 3);
	}
};

// =============================================================================
// Display — a spectroscope, not a bar chart.
//
// Left two thirds: the spectrum the engine is ACTUALLY producing, drawn as
// filled bars, with the stored per-partial curves laid over it as coloured
// polylines. The tab picks which curve you can drag; the others stay visible
// and dim, because the whole point of an additive voice is how the attributes
// relate to each other and you cannot see a relationship one attribute at a
// time.
//
// Right third, stacked: the output waveform, the stereo placement of each
// partial, and the modulation matrix. Pan gets its own block rather than a tab
// because it is the one attribute that is about SPACE, and a bar chart of it
// tells you nothing a picture of the stereo field would not tell you better.
// =============================================================================

static const float SG_DESIGN_W = 605.f;      // 160mm * 3.783, per screen-style
static const int   SG_NTAB = 5;
// LOUD and SOFT, not LEVEL and SOFT. They are one control in two halves: the
// spectrum you hear at full velocity and the spectrum you hear at none, with
// velocity crossing between them. Calling the first "LEVEL" made it read as
// "the levels" and the second as a softness amount, so the pairing -- the whole
// idea -- was invisible. The MORPH bar along the foot is already labelled SOFT
// at one end and LOUD at the other; now the tabs use the same two words.
static const char* SG_TABNAME[SG_NTAB] = {"LOUD", "SOFT", "PITCH", "DEPTH", "RATE"};
static const bool  SG_TABBIP[SG_NTAB]  = {false, false, true, true, false};
static const NVGcolor SG_TABCOL[SG_NTAB] = {
	nvgRGB(0x00, 0x97, 0xDE),   // LEVEL  blue
	nvgRGB(0x9B, 0x6B, 0xD6),   // SOFT   purple
	nvgRGB(0x3F, 0xBF, 0x6F),   // PITCH  green
	nvgRGB(0xEC, 0x65, 0x2E),   // DEPTH  orange
	nvgRGB(0xD8, 0xB4, 0x3A),   // RATE   yellow
};

struct SigmaDisplay : OpaqueWidget {
	Sigma* module = nullptr;
	std::shared_ptr<Font> font;
	int np() const { return module ? module->nPartials : 16; }
	enum Drag { DRAG_NONE, DRAG_BARS, DRAG_MATRIX, DRAG_PAN };
	Drag dragKind = DRAG_NONE;
	int dragRow = 0, dragCol = 0;
	Vec dragPos;
	Vec dragPrev;                    // where the last apply landed
	bool dragHas = false;

	// ── layout, in design units ─────────────────────────────────────────────
	float uH() const { return box.size.y / (box.size.x / SG_DESIGN_W); }
	float splitX() const { return SG_DESIGN_W * 0.655f; }
	float tabsH() const { return 20.f; }
	float specY() const { return tabsH() + 5.f; }
	float specH() const { return uH() - specY() - 14.f; }
	float rx() const { return splitX() + 6.f; }
	float rw() const { return SG_DESIGN_W - rx() - 4.f; }
	// TWO blocks, PAN over MOD. There was a third above them showing one cycle
	// of the output, and it was the wrong thing to spend a quarter of the
	// column on: a single cycle of a sum of sines is a picture of the spectrum
	// you are already looking at, drawn worse. The matrix is the block you
	// actually reach into, so it takes the whole of what the scope gave up and
	// pan moves to the top.
	float blkFrac(int i) const { return (i == 0) ? 0.28f : 0.72f; }
	float blkY(int i) const {
		float y = 4.f, t = uH() - 8.f;
		for (int k = 0; k < i; k++) y += t * blkFrac(k);
		return y;
	}
	float blkH(int i = 1) const { return (uH() - 8.f) * blkFrac(i); }

	float* tabArray(int t) const {
		if (!module) return nullptr;
		switch (t) {
			case 0: return module->pLevel; case 1: return module->pSoft;
			case 2: return module->pPitch; case 3: return module->pDepth;
			default: return module->pRate;
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawPreview(const DrawArgs& args, float s);
	void onButton(const ButtonEvent& e) override;
	void onDoubleClick(const DoubleClickEvent& e) override;
	Vec lastPress;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override { dragKind = DRAG_NONE; OpaqueWidget::onDragEnd(e); }
	void apply(Vec p);
	void applyAt(Vec p);
	// tabs explain themselves on hover; five one-word names cannot
	ui::Tooltip* tip = nullptr;
	// What the pointer is over, already formatted. Held as a string rather than
	// as a (region, index) pair because every region says something different
	// and the drawing code has no use for it.
	std::string hoverStr;
	std::string hoverAt(float ux, float uy);

	// The hit tests, in ONE place. Editing and reading-out used to compute the
	// same indices from the same geometry in two functions, which is a standing
	// invitation for the tooltip to name one partial while the drag edits its
	// neighbour. Sharing them means the readout cannot describe a control other
	// than the one a click would land on.
	int barIndexAt(float ux) const {
		return clamp((int)(ux / (splitX() / (float)np())), 0, np() - 1);
	}
	float barValueAt(float uy) const {
		return clamp(1.f - (uy - specY()) / std::max(specH(), 1.f), 0.f, 1.f);
	}
	int panIndexAt(float uy) const {
		float h = blkH(0) - 12.f, y0 = blkY(0) + 9.f;
		return clamp((int)((uy - y0) / std::max(h / np(), 0.5f)), 0, np() - 1);
	}
	void matrixCellAt(float ux, float uy, int& r, int& c, float& t) const {
		float h = blkH(1) - 12.f, y0 = blkY(1) + 9.f, lw = 14.f;
		float cw = (rw() - lw) / (float)SG_MOD_N, ch = h / (float)SG_MODSRC;
		r = clamp((int)((uy - y0) / std::max(ch, 0.5f)), 0, SG_MODSRC - 1);
		c = clamp((int)((ux - rx() - lw) / std::max(cw, 0.5f)), 0, SG_MOD_N - 1);
		// vertical drag from the cell's own centre, so a click lands where you
		// clicked rather than snapping to whatever the pointer's row implies
		t = clamp(1.f - (uy - (y0 + r * ch)) / std::max(ch, 0.5f), 0.f, 1.f);
	}
	void onHover(const HoverEvent& e) override;
	void onLeave(const LeaveEvent& e) override;
	void step() override;
};

void SigmaDisplay::onButton(const ButtonEvent& e) {
	if (!module || e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT) {
		OpaqueWidget::onButton(e); return;
	}
	lastPress = e.pos;
	float s = box.size.x / SG_DESIGN_W;
	float ux = e.pos.x / s, uy = e.pos.y / s;
	if (ux < splitX()) {
		if (uy < tabsH()) {
			module->dispTab = clamp((int)(ux / (splitX() / SG_NTAB)), 0, SG_NTAB - 1);
			e.consume(this); return;
		}
		dragKind = DRAG_BARS;
	} else {
		if (uy >= blkY(1))      dragKind = DRAG_MATRIX;
		else if (uy >= blkY(0)) dragKind = DRAG_PAN;
		else { OpaqueWidget::onButton(e); return; }
	}
	dragPos = e.pos;
	dragHas = false;                 // a click has no trail to fill in
	apply(dragPos);
	e.consume(this);
}
// A header is the obvious place to ask for "put this back". DoubleClickEvent
// carries no position, so the last press is remembered instead.
void SigmaDisplay::onDoubleClick(const DoubleClickEvent& e) {
	if (!module) return;
	float s = box.size.x / SG_DESIGN_W;
	float ux = lastPress.x / s, uy = lastPress.y / s;
	if (ux < splitX()) {
		if (uy < tabsH()) module->resetTab(clamp((int)(ux / (splitX() / SG_NTAB)), 0, SG_NTAB - 1));
	} else if (uy < blkY(1)) {
		for (int p = 0; p < SG_MAXP; p++) module->pPan[p] = 0.f;
	} else {
		for (int r = 0; r < SG_MODSRC; r++)
			for (int c = 0; c < SG_MOD_N; c++) module->mod[r][c] = 0.f;
	}
	e.consume(this);
}

void SigmaDisplay::onDragMove(const DragMoveEvent& e) {
	if (dragKind == DRAG_NONE) { OpaqueWidget::onDragMove(e); return; }
	float z = getAbsoluteZoom();
	if (z > 0.f) dragPos = dragPos.plus(e.mouseDelta.div(z));
	apply(dragPos);
}
void SigmaDisplay::apply(Vec p) {
	// Walk from wherever the last apply landed to here, one step per column, so
	// nothing between them is skipped. Mouse events arrive per FRAME, so a fast
	// drag jumps several partials at once and every one it flew over was left
	// untouched -- the same hole Trace's brush had, one control surface up.
	if (dragHas && dragKind == DRAG_BARS) {
		float s0 = box.size.x / SG_DESIGN_W;
		float colPx = (splitX() / (float)np()) * s0;
		float dx = p.x - dragPrev.x;
		int steps = (int)std::min(std::fabs(dx) / std::max(colPx, 1.f), 128.f);
		for (int k = 1; k <= steps; k++) {
			float t = (float)k / (float)(steps + 1);
			applyAt(dragPrev.plus(p.minus(dragPrev).mult(t)));
		}
	}
	dragPrev = p; dragHas = true;
	applyAt(p);
}

void SigmaDisplay::applyAt(Vec p) {
	float s = box.size.x / SG_DESIGN_W;
	float ux = p.x / s, uy = p.y / s;
	if (dragKind == DRAG_BARS) {
		float* arr = tabArray(module->dispTab);
		if (!arr) return;
		float t = barValueAt(uy);
		float v = SG_TABBIP[module->dispTab] ? (t * 2.f - 1.f) : t;
		if (module->dispTab == 2) v = module->snapPitch(v);   // PITCH latches
		arr[barIndexAt(ux)] = v;
	} else if (dragKind == DRAG_PAN) {
		module->pPan[panIndexAt(uy)] = clamp((ux - rx()) / rw() * 2.f - 1.f, -1.f, 1.f);
	} else if (dragKind == DRAG_MATRIX) {
		int r, c; float t;
		matrixCellAt(ux, uy, r, c, t);
		module->mod[r][c] = t * 2.f - 1.f;
	}
}

static const char* SG_TABHELP[SG_NTAB] = {
	"LOUD - the spectrum at FULL velocity. One of a pair: velocity crossfades "
	"between SOFT and this, so a hard note is a different timbre and not just a "
	"bigger one. With nothing patched to VEL you are hearing this curve.",
	"SOFT - the same partials at ZERO velocity, drawn separately. Play quietly "
	"on a real instrument and you lose the top of the spectrum, not just level; "
	"that is what the second curve is for. Storing both is the whole reason "
	"MORPH and MORPH SENS exist.",
	"PITCH - cents per partial, a trim on top of STRETCH, +/- two octaves. The "
	"context menu can latch a drag to cents, semitones or octaves, which is how "
	"you land exactly on 4x or 10x.",
	"DEPTH - how much of the envelope each partial takes. The zero line is drawn "
	"across the graph: above it the partial follows the envelope, below it "
	"follows the INVERSE and blooms in as the others fall away.",
	"RATE - envelope speed per partial. Faster highs is what makes a struck tone sound struck.",
};

// Everything on this screen is editable, and until now only the tabs said what
// they were. A bar you can drag but cannot read is a control you have to
// discover by ear -- so every region reports the value under the pointer, in
// the unit that region actually means: cents for PITCH, a rate multiplier for
// RATE, per cent for the levels.
//
std::string SigmaDisplay::hoverAt(float ux, float uy) {
	if (!module) return "";
	if (uy < tabsH() && ux < splitX()) {
		int t = clamp((int)(ux / (splitX() / SG_NTAB)), 0, SG_NTAB - 1);
		return SG_TABHELP[t];
	}
	if (ux < splitX() && uy >= specY() && uy <= specY() + specH()) {
		int tb = clamp(module->dispTab, 0, SG_NTAB - 1);
		int i = barIndexAt(ux);
		const float* arr = nullptr;
		switch (tb) {
			case 0: arr = module->pLevel; break;
			case 1: arr = module->pSoft;  break;
			case 2: arr = module->pPitch; break;
			case 3: arr = module->pDepth; break;
			default: arr = module->pRate; break;
		}
		float v = arr[i];
		std::string val;
		if (tb == 2) {
			// cents, the unit the tab is actually in
			val = string::f("%+.0f cents", v * 2400.f);
		} else if (tb == 3) {
			val = v >= 0.f ? string::f("%.0f%%", v * 100.f)
			               : string::f("%.0f%% inverted -- blooms as the rest fall",
			                           -v * 100.f);
		} else if (tb == 4) {
			val = string::f("%.2fx", 0.25f * std::pow(16.f, clamp(v, 0.f, 1.f)));
		} else {
			val = string::f("%.0f%%", v * 100.f);
		}
		// The live amplitude is what you are hearing, which is not the same as
		// what you drew once the macros and the envelope have had their say.
		std::string live;
		if ((tb == 0 || tb == 1) && module->liveOn && i < SG_MAXP)
			live = string::f("   (sounding %.0f%%)", clamp(module->liveAmp[i], 0.f, 1.f) * 100.f);
		return string::f("Partial %d   %s  %s%s", i + 1, SG_TABNAME[tb],
		                 val.c_str(), live.c_str());
	}
	if (ux >= rx()) {
		if (uy >= blkY(0) && uy < blkY(0) + blkH(0)) {
			int i = panIndexAt(uy);
			float pan = clamp(module->pPan[i], -1.f, 1.f);
			const char* side = pan < -0.02f ? "L" : (pan > 0.02f ? "R" : "centre");
			return std::fabs(pan) < 0.02f
			     ? string::f("Partial %d   PAN  centre", i + 1)
			     : string::f("Partial %d   PAN  %.0f%% %s", i + 1,
			                 std::fabs(pan) * 100.f, side);
		}
		if (uy >= blkY(1) && uy < blkY(1) + blkH(1)) {
			int r, c; float t;
			matrixCellAt(ux, uy, r, c, t);
			static const char* DEST[SG_MOD_N] = {"level", "pitch", "pan",
			                                     "tilt", "stretch", "cutoff"};
			float v = module->mod[r][c];
			return std::fabs(v) < 0.005f
			     ? string::f("%s -> %s   off", SG_SRCNAME[r], DEST[c])
			     : string::f("%s -> %s   %+.0f%%", SG_SRCNAME[r], DEST[c], v * 100.f);
		}
	}
	return "";
}

void SigmaDisplay::onHover(const HoverEvent& e) {
	float s = box.size.x / SG_DESIGN_W;
	hoverStr = hoverAt(e.pos.x / s, e.pos.y / s);
	OpaqueWidget::onHover(e);
}
void SigmaDisplay::onLeave(const LeaveEvent& e) {
	hoverStr.clear();
	OpaqueWidget::onLeave(e);
}
void SigmaDisplay::step() {
	// While a drag is in progress the value under the pointer is changing, so
	// keep the readout live rather than leaving it at whatever it said when the
	// drag started -- during an edit is exactly when you want to see the number.
	if (dragKind != DRAG_NONE) {
		// The zoom divisor is not optional. getAbsoluteOffset() maps a LOCAL
		// vector to absolute coordinates and ZoomWidget scales it on the way, so
		// the inverse needs the division too. Without it the drag readout named
		// a partial further and further from the real one the further the
		// pointer was from this widget's top-left corner.
		float s = box.size.x / SG_DESIGN_W;
		float z = getAbsoluteZoom();
		if (z <= 0.f) z = 1.f;
		Vec p = APP->scene->mousePos.minus(getAbsoluteOffset(Vec(0, 0))).div(z);
		hoverStr = hoverAt(p.x / s, p.y / s);
	}
	bool want = !hoverStr.empty();
	if (want && !tip) { tip = new ui::Tooltip; APP->scene->addChild(tip); }
	else if (!want && tip) {
		APP->scene->removeChild(tip); delete tip; tip = nullptr;
	}
	if (tip) {
		tip->text = hoverStr;
		tip->box.pos = APP->scene->mousePos.plus(Vec(15, 15));
	}
	OpaqueWidget::step();
}

void SigmaDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	NVGcontext* vg = args.vg;
	float s = box.size.x / SG_DESIGN_W;
	if (!font || font->handle < 0) font = sfs::screenFontFace();

	nvgBeginPath(vg);
	nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, mm2px(1.f));
	nvgFillColor(vg, sfs::SCREEN_BG);
	nvgFill(vg);
	if (!module) { drawPreview(args, s); OpaqueWidget::drawLayer(args, layer); return; }

	nvgSave(vg);
	nvgScissor(vg, 0, 0, box.size.x, box.size.y);

	// ── tabs ────────────────────────────────────────────────────────────────
	float tw = splitX() / (float)SG_NTAB;
	for (int t = 0; t < SG_NTAB; t++) {
		bool sel = (module->dispTab == t);
		nvgBeginPath(vg);
		nvgRect(vg, (t * tw + 1.f) * s, 1.f * s, (tw - 2.f) * s, (tabsH() - 3.f) * s);
		nvgFillColor(vg, sel ? nvgTransRGBA(SG_TABCOL[t], 110) : sfs::SCREEN_PURP);
		nvgFill(vg);
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sel ? SG_TABCOL[t] : sfs::SCREEN_DIM);
			nvgText(vg, (t * tw + tw * 0.5f) * s, (tabsH() * 0.5f) * s, SG_TABNAME[t], NULL);
		}
	}

	// MORPH, along the foot. Unlabelled: the tabs it crossfades between are
	// named SOFT and LOUD a few millimetres above it, and repeating those two
	// words at the ends of the bar said the same thing twice. The marker's
	// colour still names the end it is nearest.
	{
		float mw = splitX() * 0.34f, mx = (splitX() - mw) * 0.5f;
		float my = uH() - 6.f;
		nvgBeginPath(vg);
		nvgRect(vg, mx * s, my * s, mw * s, 1.5f * s);
		nvgFillColor(vg, sfs::SCREEN_PURP);
		nvgFill(vg);
		float t = clamp(module->dispMorph, 0.f, 1.f);
		nvgBeginPath(vg);
		nvgRect(vg, (mx + (mw - 3.f) * t) * s, (my - 2.f) * s, 3.f * s, 5.5f * s);
		nvgFillColor(vg, t < 0.5f ? SG_TABCOL[1] : SG_TABCOL[0]);
		nvgFill(vg);
	}

	float y0 = specY(), h = specH();
	float colW = splitX() / (float)np();

	// ── the live spectrum, filled ───────────────────────────────────────────
	// What the engine is doing right now, which is the thing the stored curves
	// only describe indirectly once TILT, the envelope and the LFOs are through
	// with them.
	// AN AREA, not a bar per partial. Sixty-four bars two pixels wide stopped
	// being a spectrum and became a comb; and bars imply the partials are
	// separate buckets when what they trace is one curve across the index --
	// the same curve every macro in this module bends. Drawn the way Phase and
	// Slice draw a waveform: a filled envelope with its outline on top.
	{
		nvgBeginPath(vg);
		nvgMoveTo(vg, (colW * 0.5f) * s, (y0 + h) * s);
		for (int i = 0; i < np(); i++) {
			float a = clamp(module->liveAmp[i], 0.f, 1.f);
			float bh = std::sqrt(a) * h;          // sqrt, so quiet partials show
			nvgLineTo(vg, (i * colW + colW * 0.5f) * s, (y0 + h - bh) * s);
		}
		nvgLineTo(vg, ((np() - 1) * colW + colW * 0.5f) * s, (y0 + h) * s);
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGBA(0x0D, 0x59, 0x86, 0xCC));
		nvgFill(vg);
		nvgBeginPath(vg);
		for (int i = 0; i < np(); i++) {
			float a = clamp(module->liveAmp[i], 0.f, 1.f);
			float bh = std::sqrt(a) * h;
			float xx = (i * colW + colW * 0.5f) * s, yy = (y0 + h - bh) * s;
			if (i == 0) nvgMoveTo(vg, xx, yy); else nvgLineTo(vg, xx, yy);
		}
		nvgStrokeColor(vg, nvgRGBA(0x2A, 0x8F, 0xC8, 0xFF));
		nvgStrokeWidth(vg, 1.2f * s);
		nvgStroke(vg);
	}

	// THE ZERO LINE, drawn only when the selected tab is bipolar. DEPTH and
	// PITCH both cross zero and both mean something different either side of
	// it -- below zero DEPTH follows the envelope INVERTED -- and there was
	// nothing on screen saying where the crossing was. Labelled at the left so
	// it cannot be mistaken for a grid line.
	if (SG_TABBIP[clamp(module->dispTab, 0, SG_NTAB - 1)]) {
		float zy = y0 + h * 0.5f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, 0.f, zy * s);
		nvgLineTo(vg, splitX() * s, zy * s);
		nvgStrokeColor(vg, sfs::SCREEN_PMID);
		nvgStrokeWidth(vg, 1.f * s);
		nvgStroke(vg);
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, 2.f * s, (zy - 1.f) * s,
			        module->dispTab == 3 ? "0  (below: inverted)" : "0", NULL);
		}
	}

	// ── the stored curves, over the top ─────────────────────────────────────
	for (int t = 0; t < SG_NTAB; t++) {
		const float* arr = tabArray(t);
		bool sel = (module->dispTab == t);
		bool bip = SG_TABBIP[t];
		nvgBeginPath(vg);
		for (int i = 0; i < np(); i++) {
			float v = arr[i];
			float yy = bip ? (y0 + h * 0.5f - clamp(v, -1.f, 1.f) * h * 0.5f)
			               : (y0 + h - clamp(v, 0.f, 1.f) * h);
			float xx = i * colW + colW * 0.5f;
			if (i == 0) nvgMoveTo(vg, xx * s, yy * s);
			else        nvgLineTo(vg, xx * s, yy * s);
		}
		nvgStrokeColor(vg, nvgTransRGBA(SG_TABCOL[t], sel ? 255 : 70));
		nvgStrokeWidth(vg, sel ? 2.0f : 1.0f);
		nvgStroke(vg);
		if (!sel) continue;
		for (int i = 0; i < np(); i++) {          // handles on the live one
			float v = arr[i];
			float yy = bip ? (y0 + h * 0.5f - clamp(v, -1.f, 1.f) * h * 0.5f)
			               : (y0 + h - clamp(v, 0.f, 1.f) * h);
			nvgBeginPath(vg);
			nvgCircle(vg, (i * colW + colW * 0.5f) * s, yy * s, 2.2f * s);
			nvgFillColor(vg, SG_TABCOL[t]);
			nvgFill(vg);
		}
	}

	if (font && font->handle >= 0) {
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		for (int i = 0; i < np(); i += 3)
			nvgText(vg, (i * colW + colW * 0.5f) * s, (y0 + h + 4.f) * s,
			        string::f("%d", i + 1).c_str(), NULL);
	}

	// No divider. The blocks on the right carry their own headings and their
	// own edges; a full-height rule between them and the spectrum drew a
	// boundary the eye did not need.

	auto blockLabel = [&](int i, const char* t) {
		if (!font || font->handle < 0) return;
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		nvgText(vg, rx() * s, (blkY(i) + 5.f) * s, t, NULL);
	};

	// ── block 0: stereo placement ───────────────────────────────────────────
	blockLabel(0, "PAN");
	{
		float by = blkY(0) + 9.f, bh = blkH(0) - 12.f;
		float rowH = bh / (float)np();
		nvgBeginPath(vg);
		nvgRect(vg, (rx() + rw() * 0.5f - 0.5f) * s, by * s, 1.f * s, bh * s);
		nvgFillColor(vg, sfs::SCREEN_PMID);
		nvgFill(vg);
		for (int i = 0; i < np(); i++) {
			float pan = module->liveOn ? module->livePan[i] : module->pPan[i];
			float xx = rx() + rw() * (0.5f + clamp(pan, -1.f, 1.f) * 0.5f);
			float yy = by + (i + 0.5f) * rowH;
			float a = clamp(module->liveAmp[i], 0.f, 1.f);
			nvgBeginPath(vg);
			nvgCircle(vg, xx * s, yy * s, (1.2f + 2.2f * std::sqrt(a)) * s);
			nvgFillColor(vg, nvgTransRGBA(SG_TABCOL[0], (int)(70 + 185 * std::sqrt(a))));
			nvgFill(vg);
		}
	}

	// ── block 1: the mod matrix ─────────────────────────────────────────────
	blockLabel(1, "MOD");
	{
		float by = blkY(1) + 9.f, bh = blkH(1) - 12.f;
		float lw = 14.f;                                   // the LFO name gutter
		float fx = rx() + lw, fw = rw() - lw;
		float cw = fw / (float)SG_MOD_N, ch = bh / (float)SG_MODSRC;
		// A LATTICE, ruled all the way across, rather than twenty-four separate
		// tiles. The cells were drawn one at a time with a gap between them, so
		// each zero line stopped at its own cell's edge and the block read as a
		// scatter of little meters. Ruling the field in one piece -- one wash,
		// then dark lines cut through it in both directions -- makes the six
		// destinations line up as columns and the four sources as rows, which
		// is what you are actually reading when you look for a route.
		nvgBeginPath(vg);
		nvgRect(vg, fx * s, by * s, fw * s, bh * s);
		nvgFillColor(vg, sfs::SCREEN_PURP);
		nvgFill(vg);
		// the zero lines first, full width, so the dark rules cut them
		for (int r = 0; r < SG_MODSRC; r++) {
			nvgBeginPath(vg);
			nvgRect(vg, fx * s, (by + r * ch + ch * 0.5f - 0.5f) * s, fw * s, 1.f * s);
			nvgFillColor(vg, sfs::SCREEN_PMID);
			nvgFill(vg);
		}
		nvgFillColor(vg, sfs::SCREEN_BG);
		for (int c = 0; c <= SG_MOD_N; c++) {       // verticals, both edges in
			nvgBeginPath(vg);
			nvgRect(vg, (fx + c * cw - 0.5f) * s, by * s, 1.f * s, bh * s);
			nvgFill(vg);
		}
		for (int r = 1; r < SG_MODSRC; r++) {       // horizontals, between rows
			nvgBeginPath(vg);
			nvgRect(vg, fx * s, (by + r * ch - 0.5f) * s, fw * s, 1.f * s);
			nvgFill(vg);
		}
		for (int r = 0; r < SG_MODSRC; r++)
			for (int c = 0; c < SG_MOD_N; c++) {
				float x = fx + c * cw, y = by + r * ch;
				float mid = y + ch * 0.5f;
				// A DOT THAT RIDES THE LINE, not a bar that grows from it. A bar
				// has area, and area reads as quantity even when it is zero-ish
				// -- a cell at 5% still drew a visible slab, so an untouched
				// matrix looked busy. A dot sitting ON the zero line is plainly
				// off, and its distance from that line is the amount.
				float v = clamp(module->mod[r][c], -1.f, 1.f);
				float travel = ch * 0.5f - 2.6f;
				float cy = mid - v * travel;
				bool on = std::fabs(v) > 0.005f;
				if (on) {                       // a stem back to zero, so the
					nvgBeginPath(vg);           // eye can read the distance
					nvgMoveTo(vg, (x + cw * 0.5f) * s, mid * s);
					nvgLineTo(vg, (x + cw * 0.5f) * s, cy * s);
					nvgStrokeColor(vg, nvgTransRGBA(v > 0.f ? sfs::SCREEN_BLUE
					                                        : sfs::SCREEN_HOT, 150));
					nvgStrokeWidth(vg, 1.f * s);
					nvgStroke(vg);
				}
				nvgBeginPath(vg);
				nvgCircle(vg, (x + cw * 0.5f) * s, cy * s, 2.f * s);
				nvgFillColor(vg, on ? (v > 0.f ? sfs::SCREEN_BLUE : sfs::SCREEN_HOT)
				                    : sfs::SCREEN_PMID);
				nvgFill(vg);
			}
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			for (int c = 0; c < SG_MOD_N; c++)
				nvgText(vg, (fx + c * cw + cw * 0.5f) * s, (by - 4.f) * s,
				        SG_MODNAME[c], NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			for (int r = 0; r < SG_MODSRC; r++)
				nvgText(vg, (rx() + 1.f) * s, (by + r * ch + ch * 0.5f) * s,
				        SG_SRCNAME[r], NULL);
		}
	}

	nvgRestore(vg);
	OpaqueWidget::drawLayer(args, layer);
}

void SigmaDisplay::drawPreview(const DrawArgs& args, float s) {
	NVGcontext* vg = args.vg;
	if (!font || font->handle < 0) font = sfs::screenFontFace();
	float tw = splitX() / (float)SG_NTAB;
	for (int t = 0; t < SG_NTAB; t++) {
		nvgBeginPath(vg);
		nvgRect(vg, (t * tw + 1.f) * s, 1.f * s, (tw - 2.f) * s, (tabsH() - 3.f) * s);
		nvgFillColor(vg, t == 0 ? nvgTransRGBA(SG_TABCOL[0], 110) : sfs::SCREEN_PURP);
		nvgFill(vg);
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, t == 0 ? SG_TABCOL[0] : sfs::SCREEN_DIM);
			nvgText(vg, (t * tw + tw * 0.5f) * s, (tabsH() * 0.5f) * s, SG_TABNAME[t], NULL);
		}
	}
	float y0 = specY(), h = specH(), colW = splitX() / (float)np();
	// The same AREA the live view draws. Two copies of a drawing is how a
	// browser thumbnail comes to show a module that no longer exists -- this one
	// was still bars for an hour after the live one stopped being.
	{
		auto amp = [&](int i) {
			float a = 1.f / (1.f + 0.5f * i) * (0.75f + 0.25f * std::sin(i * 1.9f));
			return std::sqrt(clamp(a, 0.f, 1.f)) * h;
		};
		nvgBeginPath(vg);
		nvgMoveTo(vg, (colW * 0.5f) * s, (y0 + h) * s);
		for (int i = 0; i < np(); i++)
			nvgLineTo(vg, (i * colW + colW * 0.5f) * s, (y0 + h - amp(i)) * s);
		nvgLineTo(vg, ((np() - 1) * colW + colW * 0.5f) * s, (y0 + h) * s);
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGBA(0x0D, 0x59, 0x86, 0xCC));
		nvgFill(vg);
		nvgBeginPath(vg);
		for (int i = 0; i < np(); i++) {
			float xx = (i * colW + colW * 0.5f) * s, yy = (y0 + h - amp(i)) * s;
			if (i == 0) nvgMoveTo(vg, xx, yy); else nvgLineTo(vg, xx, yy);
		}
		nvgStrokeColor(vg, nvgRGBA(0x2A, 0x8F, 0xC8, 0xFF));
		nvgStrokeWidth(vg, 1.2f * s);
		nvgStroke(vg);
	}
	for (int t = 0; t < SG_NTAB; t++) {
		nvgBeginPath(vg);
		for (int i = 0; i < np(); i++) {
			float v = (t == 2) ? 0.15f * std::sin(i * 0.8f)
			                   : 1.f / (1.f + (0.15f + 0.1f * t) * i);
			bool bip = SG_TABBIP[t];
			float yy = bip ? (y0 + h * 0.5f - v * h * 0.5f) : (y0 + h - v * h);
			float xx = i * colW + colW * 0.5f;
			if (i == 0) nvgMoveTo(vg, xx * s, yy * s); else nvgLineTo(vg, xx * s, yy * s);
		}
		nvgStrokeColor(vg, nvgTransRGBA(SG_TABCOL[t], t == 0 ? 255 : 70));
		nvgStrokeWidth(vg, t == 0 ? 2.f : 1.f);
		nvgStroke(vg);
	}
	// no divider here either

	if (font && font->handle >= 0) {
		static const char* BN[2] = {"PAN", "MOD"};
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		for (int i = 0; i < 2; i++)
			nvgText(vg, rx() * s, (blkY(i) + 5.f) * s, BN[i], NULL);
	}
	{
		float by = blkY(0) + 9.f, bh = blkH(0) - 12.f, rowH = bh / np();
		nvgBeginPath(vg);
		nvgRect(vg, (rx() + rw() * 0.5f - 0.5f) * s, by * s, 1.f * s, bh * s);
		nvgFillColor(vg, sfs::SCREEN_PMID);
		nvgFill(vg);
		for (int i = 0; i < np(); i++) {
			float pan = 0.7f * std::sin(i * 0.9f);
			nvgBeginPath(vg);
			nvgCircle(vg, (rx() + rw() * (0.5f + pan * 0.5f)) * s,
			          (by + (i + 0.5f) * rowH) * s, 2.f * s);
			nvgFillColor(vg, nvgTransRGBA(SG_TABCOL[0], 190));
			nvgFill(vg);
		}
	}
	{
		// The same LATTICE the live view rules. Kept in step by hand, which is
		// the standing hazard of having two copies of one drawing.
		float by = blkY(1) + 9.f, bh = blkH(1) - 12.f, lw = 14.f;
		float fx = rx() + lw, fw = rw() - lw;
		float cw = fw / (float)SG_MOD_N, ch = bh / (float)SG_MODSRC;
		nvgBeginPath(vg);
		nvgRect(vg, fx * s, by * s, fw * s, bh * s);
		nvgFillColor(vg, sfs::SCREEN_PURP);
		nvgFill(vg);
		for (int r = 0; r < SG_MODSRC; r++) {
			nvgBeginPath(vg);
			nvgRect(vg, fx * s, (by + r * ch + ch * 0.5f - 0.5f) * s, fw * s, 1.f * s);
			nvgFillColor(vg, sfs::SCREEN_PMID);
			nvgFill(vg);
		}
		nvgFillColor(vg, sfs::SCREEN_BG);
		for (int c = 0; c <= SG_MOD_N; c++) {
			nvgBeginPath(vg);
			nvgRect(vg, (fx + c * cw - 0.5f) * s, by * s, 1.f * s, bh * s);
			nvgFill(vg);
		}
		for (int r = 1; r < SG_MODSRC; r++) {
			nvgBeginPath(vg);
			nvgRect(vg, fx * s, (by + r * ch - 0.5f) * s, fw * s, 1.f * s);
			nvgFill(vg);
		}
		for (int r = 0; r < SG_MODSRC; r++)
			for (int c = 0; c < SG_MOD_N; c++) {
				nvgBeginPath(vg);
				nvgCircle(vg, (fx + c * cw + cw * 0.5f) * s,
				          (by + r * ch + ch * 0.5f) * s, 2.f * s);
				nvgFillColor(vg, sfs::SCREEN_PMID);
				nvgFill(vg);
			}
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			for (int c = 0; c < SG_MOD_N; c++)
				nvgText(vg, (fx + c * cw + cw * 0.5f) * s, (by - 4.f) * s,
				        SG_MODNAME[c], NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			for (int r = 0; r < SG_MODSRC; r++)
				nvgText(vg, (rx() + 1.f) * s, (by + r * ch + ch * 0.5f) * s,
				        SG_SRCNAME[r], NULL);
		}
	}
}

// =============================================================================
// Widget
// =============================================================================

// The macros sit to the right of the envelope, which keeps the two things you
// reach for while playing -- the spectrum's shape and its shape in time --
// side by side rather than on separate rows.
// ── the 2026-08 grid, transcribed from res/sigma.svg ───────────────────────
// EVERY POT IS A TRIMPOT now, and several pairings went HORIZONTAL: ENV RATE
// and ENV SPREAD each sit as trimpot-then-jack along the bottom rather than
// stacked, which is what let CUTOFF and RES move out to their own column on
// the right. The macro order changed too -- WIDTH and STRETCH swapped.
static const float SG_SX[4] = {8.89f, 17.78f, 26.67f, 35.55f};   // A D S R
static const float SG_SY = 94.60f;

// TILT ODD/EVN WIDTH STRETCH MORPH, trimpot over its CV.
static const float SG_MX[5] = {47.79f, 59.21f, 70.64f, 82.07f, 93.50f};
static const float SG_MJX[5] = {47.62f, 59.04f, 70.47f, 81.90f, 93.33f};
static const float SG_MKY = 86.70f, SG_MJY = 98.40f;

// CUTOFF and RES, the same pairing, off on the right.
static const float SG_FX[2] = {149.37f, 160.79f};
static const float SG_FJX[2] = {149.20f, 160.62f};

// The LFO block: three columns, three rows -- RATE, SPREAD, then SYNC.
static const float SG_LRX[3] = {108.73f, 120.16f, 131.59f};
static const float SG_LSX[3] = {108.56f, 119.99f, 131.42f};
static const float SG_LYX[3] = {108.40f, 119.82f, 131.25f};
static const float SG_LRY = 91.80f, SG_LSY = 105.80f, SG_LYY = 121.10f;

// The bottom row. The two horizontal pairs put their trimpot 11.7mm to the
// LEFT of the jack it belongs to, which is why they need their own constants
// rather than sharing the jack row's.
static const float SG_JY = 121.30f;
static const float SG_JX[3] = {9.52f, 22.22f, 34.92f};      // GATE V/OCT VEL
static const float SG_HPX[2] = {47.36f, 70.22f};            // ENV RATE / SPREAD pots
static const float SG_HJX[2] = {59.04f, 81.90f};            // and their jacks
static const float SG_HPY = 121.10f;
static const float SG_VCAX = 93.33f;
static const float SG_OUTX[2] = {149.20f, 161.89f};         // L and R, on the plate

// A menu row that edits a param directly, so the tooltip's units and range are
// the param's own and there is no second copy of either to drift.
struct ParamSlider : ui::Slider {
	ParamSlider(Module* m, int paramId) {
		quantity = m->paramQuantities[paramId];
		box.size.x = 200.f;
	}
};

struct SigmaWidget : ModuleWidget {
	SigmaWidget(Sigma* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/sigma.svg")));

		// NO PanelLabels, and no title. The 2026-08 artwork carries its own text
		// as outlined paths, which Rack DOES render -- it ignores only <text> --
		// so drawing them again in Figtree printed every label twice, half a
		// millimetre out. Slice, Kit and Trace carry the same note.
		SigmaDisplay* disp = new SigmaDisplay;
		disp->module = module;
		disp->box.pos  = mm2px(Vec(5.08f, 10.16f));
		disp->box.size = mm2px(Vec(162.53f, 65.52f));
		addChild(disp);

		// ── ADSR ───────────────────────────────────────────────────────────
		for (int i = 0; i < 4; i++)
			addParam(createParamCentered<VCVSlider>(mm2px(Vec(SG_SX[i], SG_SY)),
			                                        module, Sigma::ATTACK_PARAM + i));

		// ── five macros, trimpot over CV. WIDTH and STRETCH are swapped from
		//    the old panel, so the order here is the ART's, not the enum's.
		static const int MP[5] = {Sigma::TILT_PARAM, Sigma::ODDEVEN_PARAM,
			Sigma::WIDTH_PARAM, Sigma::STRETCH_PARAM, Sigma::MORPH_PARAM};
		static const int MI[5] = {Sigma::TILT_INPUT, Sigma::ODDEVEN_INPUT,
			Sigma::WIDTH_INPUT, Sigma::STRETCH_INPUT, Sigma::MORPH_INPUT};
		for (int i = 0; i < 5; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(SG_MX[i], SG_MKY)), module, MP[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_MJX[i], SG_MJY)), module, MI[i]));
		}

		// ── the filter, in its own column ──────────────────────────────────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(SG_FX[0], SG_MKY)), module, Sigma::CUTOFF_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_FJX[0], SG_MJY)), module, Sigma::CUTOFF_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(SG_FX[1], SG_MKY)), module, Sigma::RESO_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_FJX[1], SG_MJY)), module, Sigma::RESO_INPUT));

		// ── the LFO block: three columns, RATE / SPREAD / SYNC ─────────────
		for (int i = 0; i < 3; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(SG_LRX[i], SG_LRY)), module,
			                                      Sigma::LFORATE_PARAM + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(SG_LSX[i], SG_LSY)), module,
			                                      Sigma::LFOSPREAD_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_LYX[i], SG_LYY)), module,
			                                         Sigma::LFOSYNC_INPUT + i));
		}

		// ── the bottom row ─────────────────────────────────────────────────
		static const int JI[3] = {Sigma::GATE_INPUT, Sigma::VOCT_INPUT, Sigma::VEL_INPUT};
		for (int i = 0; i < 3; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_JX[i], SG_JY)), module, JI[i]));
		// The two HORIZONTAL pairs: the trimpot sits beside its jack rather than
		// above it, which is what freed the height the filter column needed.
		addParam(createParamCentered<Trimpot>(mm2px(Vec(SG_HPX[0], SG_HPY)), module, Sigma::ENVRATE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_HJX[0], SG_JY)), module, Sigma::ENVRATE_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(SG_HPX[1], SG_HPY)), module, Sigma::ENVSPREAD_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_HJX[1], SG_JY)), module, Sigma::ENVSPREAD_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SG_VCAX, SG_JY)), module, Sigma::VCA_INPUT));

		// The plate's two jacks. NOTE: the art labels them GATE and V/OCT, which
		// cannot be right -- Sigma's only outputs are the stereo pair, and both
		// of those labels already appear on the input row. Placed as L and R.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(SG_OUTX[0], SG_JY)), module, Sigma::L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(SG_OUTX[1], SG_JY)), module, Sigma::R_OUTPUT));

	}

	void appendContextMenu(Menu* menu) override {
		Sigma* m = dynamic_cast<Sigma*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Preset", "", [=](Menu* sub) {
			// ONE list. It was split in two because the second batch arrived
			// later, which is a fact about how it was built rather than about
			// how it is used -- and it left the two gongs eight rows apart.
			static const char* PN[SG_NPRESET] = {
				"Ramp", "Square", "Drawbar Organ", "E-piano", "Vibraphone",
				"Marimba", "Bell", "Tubular Bells", "Singing Bowl", "Gong",
				"Psychedelic Gong", "Bowed", "Choir", "Vowel", "CS-80",
				"Touch Switch", "Bloom", "Cloud", "Rotor", "Acid",
				"Ensemble"};
			for (int i = 0; i < SG_NPRESET; i++)
				sub->addChild(createMenuItem(PN[i], "", [=]() { m->loadPreset(i); }));
		}));
		menu->addChild(createIndexSubmenuItem("Partials", {"16", "32", "64"},
			[=]() {
				for (int i = 0; i < SG_NCOUNT; i++) if (SG_COUNTS[i] == m->nPartials) return i;
				return 0;
			},
			[=](int i) { m->nPartials = SG_COUNTS[clamp(i, 0, SG_NCOUNT - 1)]; }));
		menu->addChild(createIndexPtrSubmenuItem("Voice allocation",
			{"First available", "Rolling"}, &m->allocMode));
		menu->addChild(createIndexPtrSubmenuItem("PITCH tab snaps to",
			{"Free", "Cents", "Semitones", "Octaves"}, &m->pitchSnap));

		// Menu sliders rather than panel knobs, the way Chance keeps GATE LEN
		// and GLIDE off its panel: these are set once for a patch and then left,
		// and Sigma's panel is already eight macros wide.
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Touch response", "", [=](Menu* sub) {
			sub->addChild(createMenuLabel("Velocity to level, and where its range sits"));
			sub->addChild(new ParamSlider(m, Sigma::AMPSENS_PARAM));
			sub->addChild(new ParamSlider(m, Sigma::AMPCENTER_PARAM));
			sub->addChild(createMenuLabel("Velocity to timbre. MORPH is its centre;"));
			sub->addChild(createMenuLabel("past halfway the blend hardens to a switch."));
			sub->addChild(new ParamSlider(m, Sigma::MORPHSENS_PARAM));
		}));
		menu->addChild(createSubmenuItem("LFO delay and random", "", [=](Menu* sub) {
			sub->addChild(createMenuLabel("RANDOM also frees each voice's phase:"));
			sub->addChild(createMenuLabel("periodic locks together, aperiodic drifts apart."));
			for (int i = 0; i < 3; i++) {
				sub->addChild(createMenuLabel(string::f("LFO %d", i + 1)));
				sub->addChild(new ParamSlider(m, Sigma::LFODELAY_PARAM + i));
				sub->addChild(new ParamSlider(m, Sigma::LFORAND_PARAM + i));
			}
		}));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Reset spectrum", "", [=]() { m->initSpectrum(); }));
		menu->addChild(createMenuItem("Reset modulation", "", [=]() {
			for (int r = 0; r < SG_MODSRC; r++)
				for (int c = 0; c < SG_MOD_N; c++) m->mod[r][c] = 0.f;
		}));
	}
};

Model* modelSigma = createModel<Sigma, SigmaWidget>("Sigma");
