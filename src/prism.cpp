#include "plugin.hpp"
#include "panel-style.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// =============================================================================
// Prism — a tone split into sixteen partials you can move independently.
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
// See docs/prism-design.md.
// =============================================================================

// The ARRAY size; the count actually sounding is a menu choice. 64 x 16 voices
// is 1024 oscillators, which the benchmark puts near 5% of one core -- so the
// ceiling is editability and Nyquist, not CPU.
static const int PR_MAXP   = 64;
static const int PR_VOICES = 16;
static const int PR_NCOUNT = 3;
static const int PR_COUNTS[PR_NCOUNT] = {16, 32, 64};

// A table sine, because 16 partials x 16 voices is 256 oscillators and
// std::sin() 256 times a sample is not the same proposition as a lookup.
// Measured at 1.6% of one core for the full 256 with linear interpolation.
static const int PR_TBL = 4096;
static float prSinTbl[PR_TBL + 1];
static bool  prTblReady = false;
static void prInitTable() {
	if (prTblReady) return;
	for (int i = 0; i <= PR_TBL; i++)
		prSinTbl[i] = std::sin(2.0 * M_PI * (double)i / (double)PR_TBL);
	prTblReady = true;
}
static inline float prSin(float ph) {              // ph in [0,1)
	float f = ph * PR_TBL;
	int k = (int)f;
	float fr = f - (float)k;
	return prSinTbl[k] + (prSinTbl[k + 1] - prSinTbl[k]) * fr;
}

// A MOD MATRIX rather than one target per LFO. Three sources by five
// destinations is fifteen numbers, which is small enough to draw and edit on
// screen and large enough that an LFO can do two things at once -- which is
// most of what makes three of them feel like more than three.
enum PrModDest { PR_MOD_LEVEL, PR_MOD_PITCH, PR_MOD_PAN, PR_MOD_TILT,
                 PR_MOD_STRETCH, PR_MOD_CUT, PR_MOD_N };
static const char* PR_MODNAME[PR_MOD_N] = {"LVL", "PIT", "PAN", "TLT", "STR", "CUT"};
// Four sources: the three LFOs, and the envelope. An envelope that can only
// drive amplitude is half an envelope -- the reason it is worth a row is that
// it is the one source that knows where it is in the NOTE.
static const int PR_MODSRC = 4;
static const char* PR_SRCNAME[PR_MODSRC] = {"LFO1", "LFO2", "LFO3", "ENV"};
static const int PR_SCOPE = 512;

struct Prism : Module {
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
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT, GATE_INPUT, VEL_INPUT, VCA_INPUT,
		TILT_INPUT, ODDEVEN_INPUT, STRETCH_INPUT, WIDTH_INPUT, MORPH_INPUT,
		ENVRATE_INPUT, ENVSPREAD_INPUT, CUTOFF_INPUT,
		LFOSYNC_INPUT,                                  // 3
		INPUTS_LEN = LFOSYNC_INPUT + 3
	};
	enum OutputId { L_OUTPUT, R_OUTPUT, OUTPUTS_LEN };
	enum LightId  { LIGHTS_LEN };

	// ── per-partial state, the screen's six tabs ────────────────────────────
	float pLevel[PR_MAXP]  = {};   // the loud spectrum
	float pSoft[PR_MAXP]   = {};   // the quiet spectrum; velocity morphs
	float pPitch[PR_MAXP]  = {};   // cents, a trim on top of STRETCH
	float pPan[PR_MAXP]    = {};   // -1..1
	float pDepth[PR_MAXP]  = {};   // how much envelope this partial takes
	float pRate[PR_MAXP]   = {};   // 0..1 -> 0.25x .. 4x

	// ── voices ──────────────────────────────────────────────────────────────
	enum Stage { ST_IDLE, ST_ATT, ST_DEC, ST_SUS, ST_REL };
	struct Voice {
		bool  on = false;
		float vel = 1.f, pitch = 0.f;
		float phase[PR_MAXP] = {};
		// A partial's envelope is its own, because rate and start time differ.
		float env[PR_MAXP] = {};
		int   stage[PR_MAXP] = {};
		float wait[PR_MAXP] = {};      // ENV SPREAD's start delay
		float relFrom[PR_MAXP] = {};
		float mEnv = 0.f; int mStage = ST_IDLE; float mRelFrom = 0.f;
	};
	Voice voice[PR_VOICES];

	float lfoPhase[3] = {};
	dsp::SchmittTrigger lfoSyncTrig[3];
	float mod[PR_MODSRC][PR_MOD_N] = {};   // the matrix, bipolar

	// ── what the screen shows ───────────────────────────────────────────────
	// The stored per-partial arrays are what you drew; these are what the
	// engine actually did with them once the macros, the envelope and the LFOs
	// had their say. Seeing only the first is like editing a mixer with the
	// faders hidden.
	int   nPartials = 16;
	int   dispTab = 0;
	float dispMorph = 1.f;
	float dispF0 = 261.6f;             // for a scope that shows ONE cycle
	float liveAmp[PR_MAXP] = {};   // amplitude in force, per partial
	float liveEnv[PR_MAXP] = {};
	float livePan[PR_MAXP] = {};
	float liveCents[PR_MAXP] = {};
	bool  liveOn = false;
	float scope[PR_SCOPE] = {};
	int   scopeIdx = 0; bool scopeFill = false; float scopePrev = 0.f;

	// The knob is exponential over 0.02..30Hz; showing 0.30 told you nothing.
	struct LfoHzQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float hz = 0.02f * std::pow(30.f / 0.02f, getValue());
			return hz < 1.f ? string::f("%.2f Hz", hz)
			     : hz < 10.f ? string::f("%.2f Hz", hz) : string::f("%.1f Hz", hz);
		}
	};

	Prism() {
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
		configParam(WIDTH_PARAM, 0.f, 1.f, 0.5f, "Stereo width");
		// BIPOLAR, and it has to be. Velocity is 1.0 when VEL is unpatched, so a
		// 0..1 morph that only ADDS to it sat pinned at the loud spectrum
		// forever: SOFT was unreachable and MORPH itself did nothing until you
		// patched a velocity below 10V. A control that is inert out of the box
		// is not a control.
		configParam(MORPH_PARAM, -1.f, 1.f, 0.f, "Morph (soft <-> loud), summed with velocity");
		configParam(ENVRATE_PARAM, 0.f, 1.f, 0.35f, "Envelope rate spread (highs die sooner)");
		configParam(ENVSPREAD_PARAM, -1.f, 1.f, 0.f, "Envelope time spread (bloom / onset)");

		configParam(ATTACK_PARAM,  0.f, 1.f, 0.02f, "Attack");
		configParam(DECAY_PARAM,   0.f, 1.f, 0.35f, "Decay");
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.5f,  "Sustain", "%", 0.f, 100.f);
		configParam(RELEASE_PARAM, 0.f, 1.f, 0.3f,  "Release");

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
		for (int p = 0; p < PR_MAXP; p++) a[tab][p] = defaultFor(tab, p);
	}
	void initSpectrum() {
		for (int p = 0; p < PR_MAXP; p++) {
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
		for (int v = 0; v < PR_VOICES; v++) voice[v] = Voice();
		for (int i = 0; i < 3; i++) lfoPhase[i] = 0.f;
		for (int r = 0; r < PR_MODSRC; r++)
			for (int d = 0; d < PR_MOD_N; d++) mod[r][d] = 0.f;
	}

	static float knobTime(float k, float lo, float hi) {   // exponential seconds
		return lo * std::pow(hi / lo, k);
	}
	// the inverse, so a preset can ask for "5ms" rather than "0.179"
	static float timeKnob(float sec, float lo, float hi) {
		return clamp(std::log(sec / lo) / std::log(hi / lo), 0.f, 1.f);
	}

	// Four starting points, each of which is a different ARGUMENT for what
	// additive is good at rather than four variations on one.
	void loadPreset(int which) {
		initSpectrum();
		for (int r = 0; r < PR_MODSRC; r++)
			for (int d = 0; d < PR_MOD_N; d++) mod[r][d] = 0.f;
		params[STRETCH_PARAM].setValue(0.f);
		params[MORPH_PARAM].setValue(0.f);
		params[CUTOFF_PARAM].setValue(1.f);
		params[RESO_PARAM].setValue(0.f);
		params[ENVSPREAD_PARAM].setValue(0.f);
		params[TILT_PARAM].setValue(0.f);
		params[WIDTH_PARAM].setValue(0.3f);

		switch (which) {
			case 0:   // RAMP -- every harmonic at 1/n, which is a sawtooth
				for (int p = 0; p < PR_MAXP; p++) pLevel[p] = 1.f / (float)(p + 1);
				params[ODDEVEN_PARAM].setValue(0.f);
				params[ENVRATE_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.005f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.3f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.8f);
				params[RELEASE_PARAM].setValue(timeKnob(0.4f, 0.005f, 16.f));
				break;

			case 1:   // SQUARE -- the same 1/n, odd harmonics only
				for (int p = 0; p < PR_MAXP; p++) pLevel[p] = 1.f / (float)(p + 1);
				params[ODDEVEN_PARAM].setValue(-1.f);
				params[ENVRATE_PARAM].setValue(0.f);
				params[ATTACK_PARAM].setValue(timeKnob(0.004f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.3f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.85f);
				params[RELEASE_PARAM].setValue(timeKnob(0.3f, 0.005f, 16.f));
				break;

			case 2: {  // CS-80 -- lush brass. Slow swell, wide, and drifting:
				// the CS-80's signature is not a waveform, it is that nothing
				// in it sits still, so the two LFOs matter more than the levels.
				for (int p = 0; p < PR_MAXP; p++) {
					pLevel[p] = 1.f / (float)(p + 1);
					pSoft[p]  = 1.f / std::pow((float)(p + 1), 1.9f);   // far darker quiet
					pRate[p]  = 0.5f;
				}
				params[ODDEVEN_PARAM].setValue(0.f);
				params[STRETCH_PARAM].setValue(0.06f);      // a little unrest
				params[WIDTH_PARAM].setValue(0.75f);
				params[ENVRATE_PARAM].setValue(0.2f);
				params[ATTACK_PARAM].setValue(timeKnob(0.35f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(1.2f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.75f);
				params[RELEASE_PARAM].setValue(timeKnob(0.9f, 0.005f, 16.f));
				params[MORPH_PARAM].setValue(-0.35f);       // so SOFT is audible
				mod[0][PR_MOD_PITCH] = 0.10f;               // vibrato
				mod[1][PR_MOD_LEVEL] = 0.22f;               // spread shimmer
				mod[3][PR_MOD_CUT]   = 0.35f;               // envelope opens it
				params[CUTOFF_PARAM].setValue(0.52f);
				params[LFORATE_PARAM + 0].setValue(0.42f);
				params[LFOSPREAD_PARAM + 0].setValue(0.f);  // vibrato moves as one
				params[LFORATE_PARAM + 1].setValue(0.22f);
				params[LFOSPREAD_PARAM + 1].setValue(0.6f);
				break;
			}

			case 3: {  // MARIMBA -- a tuned bar is UNDERCUT so its first
				// overtone lands two octaves up, at 4x rather than the 2x a
				// harmonic series would give, and the next near 10x. That is
				// the whole sound, and it is why this preset is really a use of
				// the PITCH tab rather than of the LEVEL tab.
				for (int p = 0; p < PR_MAXP; p++) {
					pLevel[p] = 0.f; pPitch[p] = 0.f;
					pRate[p] = 0.5f; pDepth[p] = 1.f;
				}
				pLevel[0] = 1.f;                                   // fundamental
				pLevel[1] = 0.45f; pPitch[1] = 0.5f;                    // 2x -> 4x
				pLevel[2] = 0.16f; pPitch[2] = std::log2(10.f / 3.f) / 2.f;  // 3x -> 10x
				pLevel[5] = 0.05f;                                 // a little air
				for (int p = 0; p < PR_MAXP; p++)
					pRate[p] = clamp(0.5f + 0.03f * (float)p, 0.f, 1.f);
				params[ODDEVEN_PARAM].setValue(0.f);
				params[ENVRATE_PARAM].setValue(0.55f);
				params[ATTACK_PARAM].setValue(timeKnob(0.002f, 0.001f, 8.f));
				params[DECAY_PARAM].setValue(timeKnob(0.45f, 0.005f, 12.f));
				params[SUSTAIN_PARAM].setValue(0.f);               // struck, not held
				params[RELEASE_PARAM].setValue(timeKnob(0.25f, 0.005f, 16.f));
				params[WIDTH_PARAM].setValue(0.2f);
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
		float width   = mac(WIDTH_PARAM, WIDTH_INPUT, 0.f, 1.f);
		float morphK  = mac(MORPH_PARAM, MORPH_INPUT, -1.f, 1.f);
		float envRate = mac(ENVRATE_PARAM, ENVRATE_INPUT, 0.f, 1.f);
		float envSpr  = mac(ENVSPREAD_PARAM, ENVSPREAD_INPUT, -1.f, 1.f);
		dispMorph = morphK;

		float A = knobTime(params[ATTACK_PARAM].getValue(),  0.001f, 8.f);
		float D = knobTime(params[DECAY_PARAM].getValue(),   0.005f, 12.f);
		float S = params[SUSTAIN_PARAM].getValue();
		float R = knobTime(params[RELEASE_PARAM].getValue(), 0.005f, 16.f);

		// ── LFOs ────────────────────────────────────────────────────────────
		float lfoSpread[3], lfoFlat[3];
		for (int i = 0; i < 3; i++) {
			if (lfoSyncTrig[i].process(inputs[LFOSYNC_INPUT + i].getVoltage(), 0.1f, 2.f))
				lfoPhase[i] = 0.f;
			float hz = knobTime(params[LFORATE_PARAM + i].getValue(), 0.02f, 30.f);
			lfoPhase[i] += hz * dt;
			lfoPhase[i] -= std::floor(lfoPhase[i]);
			lfoSpread[i] = params[LFOSPREAD_PARAM + i].getValue();
			// TILT, STRETCH and CUT are whole-spectrum controls, so they take
			// the LFO flat. Spread only means something for a destination that
			// exists once per partial.
			lfoFlat[i]   = prSin(lfoPhase[i]);
		}
		float cutK = params[CUTOFF_PARAM].getValue();
		if (inputs[CUTOFF_INPUT].isConnected()) cutK += inputs[CUTOFF_INPUT].getVoltage() * 0.1f;
		float reso = params[RESO_PARAM].getValue();
		float baseTilt = tilt, baseStretch = stretch;

		// STRETCH by the real law: f_n = n*f0*sqrt(1 + B*n^2), which is piano
		// string stiffness. B ~ 1e-4 for a piano, far more for a bell, so one
		// knob runs from pure harmonic through piano to gong. Resolved per
		// voice, below.

		int nch = std::max(inputs[GATE_INPUT].getChannels(), 1);
		nch = std::min(nch, PR_VOICES);

		// The screen follows the LOWEST sounding voice rather than the newest:
		// a display that jumps to whichever note was struck last is unreadable
		// while a chord is held.
		int dispVoice = -1;
		for (int c = 0; c < nch; c++)
			if (voice[c].mStage != ST_IDLE) { dispVoice = c; break; }
		liveOn = (dispVoice >= 0);
		if (!liveOn) for (int p = 0; p < nPartials; p++) { liveAmp[p] = 0.f; liveEnv[p] = 0.f; }

		float outL = 0.f, outR = 0.f;
		float nyq = args.sampleRate * 0.5f;
		float fadeLo = nyq * 0.72f;                 // start fading an octave down

		for (int c = 0; c < nch; c++) {
			Voice& V = voice[c];
			bool gate = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
			if (gate && !V.on) {
				V.on = true;
				V.vel = inputs[VEL_INPUT].isConnected()
				      ? clamp(inputs[VEL_INPUT].getVoltage(c) * 0.1f, 0.f, 1.f) : 1.f;
				for (int p = 0; p < nPartials; p++) {
					V.stage[p] = ST_ATT; V.env[p] = 0.f;
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
				V.mStage = ST_ATT; V.mEnv = 0.f;
			} else if (!gate && V.on) {
				V.on = false;
				for (int p = 0; p < nPartials; p++) {
					V.stage[p] = ST_REL; V.relFrom[p] = std::max(V.env[p], 1e-4f);
				}
				V.mStage = ST_REL; V.mRelFrom = std::max(V.mEnv, 1e-4f);
			}
			if (V.mStage == ST_IDLE && !V.on) continue;

			V.pitch = inputs[VOCT_INPUT].getVoltage(c);
			float f0 = 261.6256f * std::pow(2.f, V.pitch);
			if (c == dispVoice) dispF0 = f0;
			float morph = clamp(V.vel + morphK, 0.f, 1.f);

			// The whole-spectrum controls are resolved PER VOICE, because the
			// envelope is per voice: a held chord whose tilt followed whichever
			// note was struck last would be one voice modulating the others.
			float mSrc[PR_MODSRC] = {lfoFlat[0], lfoFlat[1], lfoFlat[2], V.mEnv};
			float tilt = baseTilt, stretch = baseStretch, cutM = 0.f;
			for (int i = 0; i < PR_MODSRC; i++) {
				tilt    += mSrc[i] * mod[i][PR_MOD_TILT];
				stretch += mSrc[i] * mod[i][PR_MOD_STRETCH];
				cutM    += mSrc[i] * mod[i][PR_MOD_CUT];
			}
			tilt = clamp(tilt, -1.f, 1.f);
			stretch = clamp(stretch, 0.f, 1.f);
			float B = stretch * stretch * 0.02f;
			// Cutoff runs 30Hz to well past Nyquist so the top of the knob is
			// genuinely open rather than "nearly open".
			float fc = 30.f * std::pow(2.f, clamp(cutK, 0.f, 1.f) * 10.f + cutM * 4.f);
			float Q  = 0.5f + reso * 8.f;

			// the master envelope: unscaled, drives the VCA and frees the voice
			advance(V.mEnv, V.mStage, V.mRelFrom, 1.f, dt, A, D, S, R);
			if (V.mStage == ST_IDLE) { V.mEnv = 0.f; continue; }

			float vL = 0.f, vR = 0.f;
			for (int p = 0; p < nPartials; p++) {
				int n = p + 1;

				// per-partial envelope, at its own rate and after its own wait
				float rate = 0.25f * std::pow(16.f, pRate[p]);      // 0.25x..4x
				rate *= 1.f + envRate * (float)p * 0.35f;            // the spread
				if (V.wait[p] > 0.f) { V.wait[p] -= dt; }
				else advance(V.env[p], V.stage[p], V.relFrom[p], rate, dt, A, D, S, R);

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
				float envAmt = (d >= 0.f) ? (1.f - d + d * V.env[p])
				                          : (1.f + d * V.env[p]);
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
				float fan = ((p & 1) ? 1.f : -1.f)
				          * (float)p / (float)std::max(nPartials - 1, 1);
				float pan = clamp(fan * width + pPan[p], -1.f, 1.f);

				// ── LFOs, spread across the partial index ───────────────────
				for (int i = 0; i < PR_MODSRC; i++) {
					// The three LFOs are spread across the partial index; the
					// envelope is not, because a note does not start at a
					// different time for each partial -- ENV SPREAD already
					// owns that idea and owning it twice would fight.
					float m;
					if (i < 3) {
						float ph = lfoPhase[i] + lfoSpread[i] * (float)p / (float)nPartials;
						ph -= std::floor(ph);
						m = prSin(ph);
					} else m = V.mEnv;
					lv    *= clamp(1.f + m * mod[i][PR_MOD_LEVEL], 0.f, 2.f);
					cents += m * mod[i][PR_MOD_PITCH] * 50.f;
					pan    = clamp(pan + m * mod[i][PR_MOD_PAN], -1.f, 1.f);
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
				float s = prSin(V.phase[p]) * lv;

				float pl = std::sqrt(0.5f * (1.f - pan));   // equal power
				float pr = std::sqrt(0.5f * (1.f + pan));
				vL += s * pl; vR += s * pr;
			}

			float g = V.mEnv;
			outL += vL * g; outR += vR * g;
		}

		float vca = inputs[VCA_INPUT].isConnected()
		          ? clamp(inputs[VCA_INPUT].getVoltage() * 0.1f, 0.f, 1.f) : 1.f;
		// Sixteen partials summed is a much bigger number than one oscillator,
		// and the tilt default already tapers them, so this is a fixed trim
		// rather than a normaliser -- a normaliser would pump with the spectrum.
		float trim = 1.6f * vca;
		float fl = clamp(outL * trim, -10.f, 10.f);
		float fr = clamp(outR * trim, -10.f, 10.f);
		// Captured on a RISING ZERO CROSSING and then held: a free-running ring
		// slides sideways at whatever the pitch happens to be, which reads as
		// the module being unstable when it is the display that is.
		if (!scopeFill) {
			if (scopePrev <= 0.f && fl > 0.f) { scopeFill = true; scopeIdx = 0; }
		}
		if (scopeFill) {
			scope[scopeIdx++] = fl;
			if (scopeIdx >= PR_SCOPE) { scopeIdx = 0; scopeFill = false; }
		}
		scopePrev = fl;
		outputs[L_OUTPUT].setVoltage(fl);
		outputs[R_OUTPUT].setVoltage(fr);
	}

	// One linear ADSR segment. Linear rather than exponential for v1: with the
	// per-partial rate spread doing the perceptual work, curve shape is a
	// second-order question -- see the design doc.
	static void advance(float& e, int& st, float& relFrom, float rate, float dt,
	                    float A, float D, float S, float R) {
		switch (st) {
			case ST_ATT:
				e += dt * rate / std::max(A, 1e-4f);
				if (e >= 1.f) { e = 1.f; st = ST_DEC; }
				break;
			case ST_DEC:
				e -= dt * rate * (1.f - S) / std::max(D, 1e-4f);
				if (e <= S) { e = S; st = ST_SUS; }
				break;
			case ST_SUS: e = S; break;
			case ST_REL:
				e -= dt * rate * relFrom / std::max(R, 1e-4f);
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
		for (int r = 0; r < PR_MODSRC; r++)
			for (int d = 0; d < PR_MOD_N; d++) json_array_append_new(mm, json_real(mod[r][d]));
		json_object_set_new(root, "mod", mm);
		json_object_set_new(root, "tab", json_integer(dispTab));
		json_object_set_new(root, "nPartials", json_integer(nPartials));
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
			for (int i = 0; i < PR_MODSRC; i++)
				for (int d = 0; d < PR_MOD_N; d++) {
					int k = i * PR_MOD_N + d;
					if (k < (int)json_array_size(mm))
						mod[i][d] = (float)json_real_value(json_array_get(mm, k));
				}
		if (json_t* j = json_object_get(root, "tab"))
			dispTab = clamp((int)json_integer_value(j), 0, 4);
		if (json_t* j = json_object_get(root, "nPartials"))
			nPartials = clamp((int)json_integer_value(j), 1, PR_MAXP);
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

static const float PR_DESIGN_W = 605.f;      // 160mm * 3.783, per screen-style
static const int   PR_NTAB = 5;
static const char* PR_TABNAME[PR_NTAB] = {"LEVEL", "SOFT", "PITCH", "DEPTH", "RATE"};
static const bool  PR_TABBIP[PR_NTAB]  = {false, false, true, true, false};
static const NVGcolor PR_TABCOL[PR_NTAB] = {
	nvgRGB(0x00, 0x97, 0xDE),   // LEVEL  blue
	nvgRGB(0x9B, 0x6B, 0xD6),   // SOFT   purple
	nvgRGB(0x3F, 0xBF, 0x6F),   // PITCH  green
	nvgRGB(0xEC, 0x65, 0x2E),   // DEPTH  orange
	nvgRGB(0xD8, 0xB4, 0x3A),   // RATE   yellow
};

struct PrismDisplay : OpaqueWidget {
	Prism* module = nullptr;
	std::shared_ptr<Font> font;
	int np() const { return module ? module->nPartials : 16; }
	enum Drag { DRAG_NONE, DRAG_BARS, DRAG_MATRIX, DRAG_PAN };
	Drag dragKind = DRAG_NONE;
	int dragRow = 0, dragCol = 0;
	Vec dragPos;
	Vec dragPrev;                    // where the last apply landed
	bool dragHas = false;

	// ── layout, in design units ─────────────────────────────────────────────
	float uH() const { return box.size.y / (box.size.x / PR_DESIGN_W); }
	float splitX() const { return PR_DESIGN_W * 0.655f; }
	float tabsH() const { return 20.f; }
	float specY() const { return tabsH() + 5.f; }
	float specH() const { return uH() - specY() - 14.f; }
	float rx() const { return splitX() + 6.f; }
	float rw() const { return PR_DESIGN_W - rx() - 4.f; }
	// Not equal thirds. One cycle of a waveform needs almost no height, and the
	// matrix is the block you actually edit, so it takes what the scope gives up.
	float blkFrac(int i) const { return (i == 0) ? 0.22f : (i == 1) ? 0.33f : 0.45f; }
	float blkY(int i) const {
		float y = 4.f, t = uH() - 8.f;
		for (int k = 0; k < i; k++) y += t * blkFrac(k);
		return y;
	}
	float blkH(int i = 2) const { return (uH() - 8.f) * blkFrac(i); }

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
	int hoverTab = -1;
	void onHover(const HoverEvent& e) override;
	void onLeave(const LeaveEvent& e) override;
	void step() override;
};

void PrismDisplay::onButton(const ButtonEvent& e) {
	if (!module || e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT) {
		OpaqueWidget::onButton(e); return;
	}
	lastPress = e.pos;
	float s = box.size.x / PR_DESIGN_W;
	float ux = e.pos.x / s, uy = e.pos.y / s;
	if (ux < splitX()) {
		if (uy < tabsH()) {
			module->dispTab = clamp((int)(ux / (splitX() / PR_NTAB)), 0, PR_NTAB - 1);
			e.consume(this); return;
		}
		dragKind = DRAG_BARS;
	} else {
		float b1 = blkY(1), b2 = blkY(2);
		if (uy >= b2)      dragKind = DRAG_MATRIX;
		else if (uy >= b1) dragKind = DRAG_PAN;
		else { OpaqueWidget::onButton(e); return; }   // the scope is not editable
	}
	dragPos = e.pos;
	dragHas = false;                 // a click has no trail to fill in
	apply(dragPos);
	e.consume(this);
}
// A header is the obvious place to ask for "put this back". DoubleClickEvent
// carries no position, so the last press is remembered instead.
void PrismDisplay::onDoubleClick(const DoubleClickEvent& e) {
	if (!module) return;
	float s = box.size.x / PR_DESIGN_W;
	float ux = lastPress.x / s, uy = lastPress.y / s;
	if (ux < splitX()) {
		if (uy < tabsH()) module->resetTab(clamp((int)(ux / (splitX() / PR_NTAB)), 0, PR_NTAB - 1));
	} else if (uy < blkY(1)) {
		return;                                     // the scope heads nothing
	} else if (uy < blkY(2)) {
		for (int p = 0; p < PR_MAXP; p++) module->pPan[p] = 0.f;
	} else {
		for (int r = 0; r < PR_MODSRC; r++)
			for (int c = 0; c < PR_MOD_N; c++) module->mod[r][c] = 0.f;
	}
	e.consume(this);
}

void PrismDisplay::onDragMove(const DragMoveEvent& e) {
	if (dragKind == DRAG_NONE) { OpaqueWidget::onDragMove(e); return; }
	float z = getAbsoluteZoom();
	if (z > 0.f) dragPos = dragPos.plus(e.mouseDelta.div(z));
	apply(dragPos);
}
void PrismDisplay::apply(Vec p) {
	// Walk from wherever the last apply landed to here, one step per column, so
	// nothing between them is skipped. Mouse events arrive per FRAME, so a fast
	// drag jumps several partials at once and every one it flew over was left
	// untouched -- the same hole Trace's brush had, one control surface up.
	if (dragHas && dragKind == DRAG_BARS) {
		float s0 = box.size.x / PR_DESIGN_W;
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

void PrismDisplay::applyAt(Vec p) {
	float s = box.size.x / PR_DESIGN_W;
	float ux = p.x / s, uy = p.y / s;
	if (dragKind == DRAG_BARS) {
		float* arr = tabArray(module->dispTab);
		if (!arr) return;
		float colW = splitX() / (float)np();
		int i = clamp((int)(ux / colW), 0, np() - 1);
		float t = clamp(1.f - (uy - specY()) / std::max(specH(), 1.f), 0.f, 1.f);
		arr[i] = PR_TABBIP[module->dispTab] ? (t * 2.f - 1.f) : t;
	} else if (dragKind == DRAG_PAN) {
		float h = blkH(1) - 12.f, y0 = blkY(1) + 9.f;
		int i = clamp((int)((uy - y0) / std::max(h / np(), 0.5f)), 0, np() - 1);
		module->pPan[i] = clamp((ux - rx()) / rw() * 2.f - 1.f, -1.f, 1.f);
	} else if (dragKind == DRAG_MATRIX) {
		float h = blkH(2) - 12.f, y0 = blkY(2) + 9.f;
		float lw = 14.f;
		float cw = (rw() - lw) / (float)PR_MOD_N, ch = h / (float)PR_MODSRC;
		int r = clamp((int)((uy - y0) / std::max(ch, 0.5f)), 0, PR_MODSRC - 1);
		int c = clamp((int)((ux - rx() - lw) / std::max(cw, 0.5f)), 0, PR_MOD_N - 1);
		// vertical drag from the cell's own centre, so a click lands where you
		// clicked rather than snapping to whatever the pointer's row implies
		float t = clamp(1.f - (uy - (y0 + r * ch)) / std::max(ch, 0.5f), 0.f, 1.f);
		module->mod[r][c] = t * 2.f - 1.f;
	}
}

static const char* PR_TABHELP[PR_NTAB] = {
	"LEVEL - the loud spectrum. Velocity morphs toward it.",
	"SOFT - the quiet spectrum. A quiet note is a different timbre, not a smaller one.",
	"PITCH - cents per partial, a trim on top of STRETCH.",
	"DEPTH - how much envelope each partial takes. Below zero it follows the inverse and blooms as the others fall.",
	"RATE - envelope speed per partial. Faster highs is what makes a struck tone sound struck.",
};

void PrismDisplay::onHover(const HoverEvent& e) {
	float s = box.size.x / PR_DESIGN_W;
	float ux = e.pos.x / s, uy = e.pos.y / s;
	hoverTab = (uy < tabsH() && ux < splitX())
	         ? clamp((int)(ux / (splitX() / PR_NTAB)), 0, PR_NTAB - 1) : -1;
	OpaqueWidget::onHover(e);
}
void PrismDisplay::onLeave(const LeaveEvent& e) {
	hoverTab = -1;
	OpaqueWidget::onLeave(e);
}
void PrismDisplay::step() {
	if (hoverTab >= 0 && !tip) { tip = new ui::Tooltip; APP->scene->addChild(tip); }
	else if (hoverTab < 0 && tip) {
		APP->scene->removeChild(tip); delete tip; tip = nullptr;
	}
	if (tip) {
		tip->text = PR_TABHELP[clamp(hoverTab, 0, PR_NTAB - 1)];
		tip->box.pos = APP->scene->mousePos.plus(Vec(15, 15));
	}
	OpaqueWidget::step();
}

void PrismDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	NVGcontext* vg = args.vg;
	float s = box.size.x / PR_DESIGN_W;
	if (!font || font->handle < 0) font = sfs::screenFontFace();

	nvgBeginPath(vg);
	nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, mm2px(1.f));
	nvgFillColor(vg, sfs::SCREEN_BG);
	nvgFill(vg);
	if (!module) { drawPreview(args, s); OpaqueWidget::drawLayer(args, layer); return; }

	nvgSave(vg);
	nvgScissor(vg, 0, 0, box.size.x, box.size.y);

	// ── tabs ────────────────────────────────────────────────────────────────
	float tw = splitX() / (float)PR_NTAB;
	for (int t = 0; t < PR_NTAB; t++) {
		bool sel = (module->dispTab == t);
		nvgBeginPath(vg);
		nvgRect(vg, (t * tw + 1.f) * s, 1.f * s, (tw - 2.f) * s, (tabsH() - 3.f) * s);
		nvgFillColor(vg, sel ? nvgTransRGBA(PR_TABCOL[t], 110) : sfs::SCREEN_PURP);
		nvgFill(vg);
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sel ? PR_TABCOL[t] : sfs::SCREEN_DIM);
			nvgText(vg, (t * tw + tw * 0.5f) * s, (tabsH() * 0.5f) * s, PR_TABNAME[t], NULL);
		}
	}

	// MORPH, along the foot and named at both ends. It was a bare bar tucked
	// under the tabs: a meter with no units and no endpoints tells you a number
	// is moving, not what the number means. SOFT on the left, LOUD on the
	// right, and the marker sits between the two curves it is crossfading.
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
		nvgFillColor(vg, t < 0.5f ? PR_TABCOL[1] : PR_TABCOL[0]);
		nvgFill(vg);
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgText(vg, (mx - 3.f) * s, (my + 1.f) * s, "SOFT", NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(vg, (mx + mw + 3.f) * s, (my + 1.f) * s, "LOUD", NULL);
		}
	}

	float y0 = specY(), h = specH();
	float colW = splitX() / (float)np();

	// ── the live spectrum, filled ───────────────────────────────────────────
	// What the engine is doing right now, which is the thing the stored curves
	// only describe indirectly once TILT, the envelope and the LFOs are through
	// with them.
	for (int i = 0; i < np(); i++) {
		float a = clamp(module->liveAmp[i], 0.f, 1.f);
		float bh = std::sqrt(a) * h;              // sqrt, so quiet partials show
		nvgBeginPath(vg);
		nvgRect(vg, (i * colW + 1.f) * s, (y0 + h - bh) * s, (colW - 2.f) * s, bh * s);
		nvgFillColor(vg, nvgRGBA(0x0D, 0x59, 0x86, 0xCC));
		nvgFill(vg);
	}

	// ── the stored curves, over the top ─────────────────────────────────────
	for (int t = 0; t < PR_NTAB; t++) {
		const float* arr = tabArray(t);
		bool sel = (module->dispTab == t);
		bool bip = PR_TABBIP[t];
		nvgBeginPath(vg);
		for (int i = 0; i < np(); i++) {
			float v = arr[i];
			float yy = bip ? (y0 + h * 0.5f - clamp(v, -1.f, 1.f) * h * 0.5f)
			               : (y0 + h - clamp(v, 0.f, 1.f) * h);
			float xx = i * colW + colW * 0.5f;
			if (i == 0) nvgMoveTo(vg, xx * s, yy * s);
			else        nvgLineTo(vg, xx * s, yy * s);
		}
		nvgStrokeColor(vg, nvgTransRGBA(PR_TABCOL[t], sel ? 255 : 70));
		nvgStrokeWidth(vg, sel ? 2.0f : 1.0f);
		nvgStroke(vg);
		if (!sel) continue;
		for (int i = 0; i < np(); i++) {          // handles on the live one
			float v = arr[i];
			float yy = bip ? (y0 + h * 0.5f - clamp(v, -1.f, 1.f) * h * 0.5f)
			               : (y0 + h - clamp(v, 0.f, 1.f) * h);
			nvgBeginPath(vg);
			nvgCircle(vg, (i * colW + colW * 0.5f) * s, yy * s, 2.2f * s);
			nvgFillColor(vg, PR_TABCOL[t]);
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

	// ── divider ─────────────────────────────────────────────────────────────
	nvgBeginPath(vg);
	nvgRect(vg, (splitX() + 1.f) * s, 2.f * s, 1.f * s, (uH() - 4.f) * s);
	nvgFillColor(vg, sfs::SCREEN_LINE);
	nvgFill(vg);

	auto blockLabel = [&](int i, const char* t) {
		if (!font || font->handle < 0) return;
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		nvgText(vg, rx() * s, (blkY(i) + 5.f) * s, t, NULL);
	};

	// ── block 0: the waveform ───────────────────────────────────────────────
	blockLabel(0, "WAVE");
	{
		float by = blkY(0) + 9.f, bh = blkH(0) - 12.f;
		// ONE CYCLE, from the pitch of the voice the screen is following. A
		// window of fixed length shows a different number of cycles at every
		// note, so the shape you are trying to read changes with what you play.
		float per = APP->engine->getSampleRate() / std::max(module->dispF0, 20.f);
		int n = clamp((int)per, 8, PR_SCOPE);
		nvgBeginPath(vg);
		for (int i = 0; i < n; i++) {
			float xx = rx() + rw() * (float)i / (float)(n - 1);
			float yy = by + bh * 0.5f - clamp(module->scope[i] * 0.1f, -1.f, 1.f) * bh * 0.5f;
			if (i == 0) nvgMoveTo(vg, xx * s, yy * s); else nvgLineTo(vg, xx * s, yy * s);
		}
		nvgStrokeColor(vg, sfs::SCREEN_BLUE);
		nvgStrokeWidth(vg, 1.2f);
		nvgStroke(vg);
	}

	// ── block 1: stereo placement ───────────────────────────────────────────
	blockLabel(1, "PAN");
	{
		float by = blkY(1) + 9.f, bh = blkH(1) - 12.f;
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
			nvgFillColor(vg, nvgTransRGBA(PR_TABCOL[0], (int)(70 + 185 * std::sqrt(a))));
			nvgFill(vg);
		}
	}

	// ── block 2: the mod matrix ─────────────────────────────────────────────
	blockLabel(2, "MOD");
	{
		float by = blkY(2) + 9.f, bh = blkH(2) - 12.f;
		float lw = 14.f;                                   // the LFO name gutter
		float cw = (rw() - lw) / (float)PR_MOD_N, ch = bh / (float)PR_MODSRC;
		for (int r = 0; r < PR_MODSRC; r++)
			for (int c = 0; c < PR_MOD_N; c++) {
				float x = rx() + lw + c * cw, y = by + r * ch;
				nvgBeginPath(vg);
				nvgRect(vg, (x + 0.5f) * s, (y + 0.5f) * s, (cw - 1.f) * s, (ch - 1.f) * s);
				nvgFillColor(vg, sfs::SCREEN_PURP);
				nvgFill(vg);
				// FROM THE CENTRE, both ways. Filling from the floor in two
				// colours makes you read the colour to know the sign, and two
				// colours at a glance is exactly what a bipolar value should
				// not need -- the direction of travel says it instead.
				float mid = y + ch * 0.5f;
				nvgBeginPath(vg);
				nvgRect(vg, (x + 1.f) * s, (mid - 0.5f) * s, (cw - 2.f) * s, 1.f * s);
				nvgFillColor(vg, sfs::SCREEN_PMID);
				nvgFill(vg);
				float v = clamp(module->mod[r][c], -1.f, 1.f);
				if (std::fabs(v) > 0.01f) {
					float fh = std::fabs(v) * (ch * 0.5f - 1.5f);
					nvgBeginPath(vg);
					nvgRect(vg, (x + 1.f) * s, (v > 0.f ? mid - fh : mid) * s,
					        (cw - 2.f) * s, fh * s);
					nvgFillColor(vg, v > 0.f ? sfs::SCREEN_BLUE : sfs::SCREEN_HOT);
					nvgFill(vg);
				}
			}
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			for (int c = 0; c < PR_MOD_N; c++)
				nvgText(vg, (rx() + lw + c * cw + cw * 0.5f) * s, (by - 4.f) * s,
				        PR_MODNAME[c], NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			for (int r = 0; r < PR_MODSRC; r++)
				nvgText(vg, (rx() + 1.f) * s, (by + r * ch + ch * 0.5f) * s,
				        PR_SRCNAME[r], NULL);
		}
	}

	nvgRestore(vg);
	OpaqueWidget::drawLayer(args, layer);
}

void PrismDisplay::drawPreview(const DrawArgs& args, float s) {
	NVGcontext* vg = args.vg;
	if (!font || font->handle < 0) font = sfs::screenFontFace();
	float tw = splitX() / (float)PR_NTAB;
	for (int t = 0; t < PR_NTAB; t++) {
		nvgBeginPath(vg);
		nvgRect(vg, (t * tw + 1.f) * s, 1.f * s, (tw - 2.f) * s, (tabsH() - 3.f) * s);
		nvgFillColor(vg, t == 0 ? nvgTransRGBA(PR_TABCOL[0], 110) : sfs::SCREEN_PURP);
		nvgFill(vg);
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, t == 0 ? PR_TABCOL[0] : sfs::SCREEN_DIM);
			nvgText(vg, (t * tw + tw * 0.5f) * s, (tabsH() * 0.5f) * s, PR_TABNAME[t], NULL);
		}
	}
	float y0 = specY(), h = specH(), colW = splitX() / (float)np();
	for (int i = 0; i < np(); i++) {
		float a = 1.f / (1.f + 0.5f * i) * (0.75f + 0.25f * std::sin(i * 1.9f));
		float bh = std::sqrt(clamp(a, 0.f, 1.f)) * h;
		nvgBeginPath(vg);
		nvgRect(vg, (i * colW + 1.f) * s, (y0 + h - bh) * s, (colW - 2.f) * s, bh * s);
		nvgFillColor(vg, nvgRGBA(0x0D, 0x59, 0x86, 0xCC));
		nvgFill(vg);
	}
	for (int t = 0; t < PR_NTAB; t++) {
		nvgBeginPath(vg);
		for (int i = 0; i < np(); i++) {
			float v = (t == 2) ? 0.15f * std::sin(i * 0.8f)
			                   : 1.f / (1.f + (0.15f + 0.1f * t) * i);
			bool bip = PR_TABBIP[t];
			float yy = bip ? (y0 + h * 0.5f - v * h * 0.5f) : (y0 + h - v * h);
			float xx = i * colW + colW * 0.5f;
			if (i == 0) nvgMoveTo(vg, xx * s, yy * s); else nvgLineTo(vg, xx * s, yy * s);
		}
		nvgStrokeColor(vg, nvgTransRGBA(PR_TABCOL[t], t == 0 ? 255 : 70));
		nvgStrokeWidth(vg, t == 0 ? 2.f : 1.f);
		nvgStroke(vg);
	}
	nvgBeginPath(vg);
	nvgRect(vg, (splitX() + 1.f) * s, 2.f * s, 1.f * s, (uH() - 4.f) * s);
	nvgFillColor(vg, sfs::SCREEN_LINE);
	nvgFill(vg);
	if (font && font->handle >= 0) {
		static const char* BN[3] = {"WAVE", "PAN", "MOD"};
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		for (int i = 0; i < 3; i++)
			nvgText(vg, rx() * s, (blkY(i) + 5.f) * s, BN[i], NULL);
	}
	{
		float by = blkY(0) + 9.f, bh = blkH(0) - 12.f;
		nvgBeginPath(vg);
		for (int i = 0; i < 96; i++) {
			float ph = (float)i / 95.f;
			float yy = by + bh * 0.5f - std::sin(ph * 6.2831853f * 2.f)
			                            * (0.6f - 0.3f * ph) * bh * 0.5f;
			float xx = rx() + rw() * ph;
			if (i == 0) nvgMoveTo(vg, xx * s, yy * s); else nvgLineTo(vg, xx * s, yy * s);
		}
		nvgStrokeColor(vg, sfs::SCREEN_BLUE);
		nvgStrokeWidth(vg, 1.2f);
		nvgStroke(vg);
	}
	{
		float by = blkY(1) + 9.f, bh = blkH(1) - 12.f, rowH = bh / np();
		nvgBeginPath(vg);
		nvgRect(vg, (rx() + rw() * 0.5f - 0.5f) * s, by * s, 1.f * s, bh * s);
		nvgFillColor(vg, sfs::SCREEN_PMID);
		nvgFill(vg);
		for (int i = 0; i < np(); i++) {
			float pan = 0.7f * std::sin(i * 0.9f);
			nvgBeginPath(vg);
			nvgCircle(vg, (rx() + rw() * (0.5f + pan * 0.5f)) * s,
			          (by + (i + 0.5f) * rowH) * s, 2.f * s);
			nvgFillColor(vg, nvgTransRGBA(PR_TABCOL[0], 190));
			nvgFill(vg);
		}
	}
	{
		float by = blkY(2) + 9.f, bh = blkH(2) - 12.f;
		float cw = rw() / (float)PR_MOD_N, ch = bh / (float)PR_MODSRC;
		for (int r = 0; r < PR_MODSRC; r++)
			for (int c = 0; c < PR_MOD_N; c++) {
				nvgBeginPath(vg);
				nvgRect(vg, (rx() + c * cw + 0.5f) * s, (by + r * ch + 0.5f) * s,
				        (cw - 1.f) * s, (ch - 1.f) * s);
				nvgFillColor(vg, sfs::SCREEN_PURP);
				nvgFill(vg);
			}
	}
}

// =============================================================================
// Widget
// =============================================================================

// The macros sit to the right of the envelope, which keeps the two things you
// reach for while playing -- the spectrum's shape and its shape in time --
// side by side rather than on separate rows.
static const float PR_MX[8] = {54.f, 68.f, 82.f, 96.f, 110.f, 124.f, 138.f, 152.f};
static const float PR_MKY = 88.f, PR_MJY = 102.f;
static const float PR_SX[4] = {10.f, 20.f, 30.f, 40.f};   // A D S R sliders
static const float PR_SY = 95.f;
static const float PR_JY = 119.f;                          // the one jack row
static const float PR_JX[4] = {10.f, 21.f, 32.f, 43.f};    // V/OCT GATE VEL VCA
// Six now, not nine: RATE and SPREAD per LFO. Depth left with the matrix.
// SYNC, RATE, SPREAD per LFO.
static const float PR_LSY[3] = {66.f, 96.f, 126.f};
static const float PR_LX[6]  = {76.f, 85.f, 106.f, 115.f, 136.f, 145.f};
static const float PR_RESOX  = 54.f;

struct PrismWidget : ModuleWidget {
	PrismWidget(Prism* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/prism.svg")));

		PrismDisplay* disp = new PrismDisplay;
		disp->module = module;
		disp->box.pos = mm2px(Vec(6.f, 6.f));
		disp->box.size = mm2px(Vec(160.f, 70.f));
		addChild(disp);

		static const int MP[8] = {Prism::TILT_PARAM, Prism::ODDEVEN_PARAM,
			Prism::STRETCH_PARAM, Prism::WIDTH_PARAM, Prism::MORPH_PARAM,
			Prism::ENVRATE_PARAM, Prism::ENVSPREAD_PARAM, Prism::CUTOFF_PARAM};
		static const int MI[8] = {Prism::TILT_INPUT, Prism::ODDEVEN_INPUT,
			Prism::STRETCH_INPUT, Prism::WIDTH_INPUT, Prism::MORPH_INPUT,
			Prism::ENVRATE_INPUT, Prism::ENVSPREAD_INPUT, Prism::CUTOFF_INPUT};
		for (int i = 0; i < 8; i++) {
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(PR_MX[i], PR_MKY)), module, MP[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PR_MX[i], PR_MJY)), module, MI[i]));
		}

		// ADSR as real sliders rather than trimpots: an envelope is a SHAPE and
		// four sliders draw it, where four knobs make you read four numbers.
		for (int i = 0; i < 4; i++)
			addParam(createParamCentered<VCVSlider>(mm2px(Vec(PR_SX[i], PR_SY)), module,
			                                        Prism::ATTACK_PARAM + i));
		for (int i = 0; i < 3; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(PR_LX[i * 2 + 0], PR_JY)), module,
			                                      Prism::LFORATE_PARAM + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(PR_LX[i * 2 + 1], PR_JY)), module,
			                                      Prism::LFOSPREAD_PARAM + i));
		}
		addParam(createParamCentered<Trimpot>(mm2px(Vec(PR_RESOX, PR_JY)), module, Prism::RESO_PARAM));
		for (int i = 0; i < 3; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PR_LSY[i], PR_JY)), module,
			                                         Prism::LFOSYNC_INPUT + i));
		static const int JI[4] = {Prism::VOCT_INPUT, Prism::GATE_INPUT,
		                          Prism::VEL_INPUT, Prism::VCA_INPUT};
		for (int i = 0; i < 4; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PR_JX[i], PR_JY)), module, JI[i]));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(157.f, PR_JY)), module, Prism::L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(167.f, PR_JY)), module, Prism::R_OUTPUT));

		sfs::PanelLabels* lab = new sfs::PanelLabels;
		lab->title(6.f, 124.f, "PRISM");
		// abbreviated because at 15mm pitch "ENV SPREAD" is wider than its knob
		static const char* MN[8] = {"TILT", "ODD/EVN", "STRETCH", "WIDTH",
		                            "MORPH", "E.RATE", "E.SPRD", "CUTOFF"};
		for (int i = 0; i < 8; i++) lab->knob(PR_MX[i], PR_MKY, MN[i]);
		static const char* EN[4] = {"A", "D", "S", "R"};
		for (int i = 0; i < 4; i++) lab->add(PR_SX[i], PR_SY - 16.f, EN[i]);
		static const char* JN[4] = {"V/OCT", "GATE", "VEL", "VCA"};
		for (int i = 0; i < 4; i++) lab->jack(PR_JX[i], PR_JY, JN[i]);
		// R is rate, S is spread across the partials -- low partial to high.
		static const char* LN[2] = {"R", "S"};
		for (int i = 0; i < 6; i++) lab->note(PR_LX[i], PR_JY - 5.f, LN[i % 2]);
		for (int i = 0; i < 3; i++) {
			lab->note(PR_LSY[i], PR_JY - 5.f, "SYN");
			lab->add((PR_LSY[i] + PR_LX[i * 2 + 1]) * 0.5f, PR_JY - 10.f,
			         string::f("LFO %d", i + 1));
		}
		lab->trim(PR_RESOX, PR_JY, "RES");
		lab->jack(157.f, PR_JY, "L");
		lab->jack(167.f, PR_JY, "R");
		addChild(lab);
	}

	void appendContextMenu(Menu* menu) override {
		Prism* m = dynamic_cast<Prism*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Preset", "", [=](Menu* sub) {
			static const char* PN[4] = {"Ramp (default)", "Square",
			                            "CS-80", "Marimba"};
			for (int i = 0; i < 4; i++)
				sub->addChild(createMenuItem(PN[i], "", [=]() { m->loadPreset(i); }));
		}));
		menu->addChild(createIndexSubmenuItem("Partials", {"16", "32", "64"},
			[=]() {
				for (int i = 0; i < PR_NCOUNT; i++) if (PR_COUNTS[i] == m->nPartials) return i;
				return 0;
			},
			[=](int i) { m->nPartials = PR_COUNTS[clamp(i, 0, PR_NCOUNT - 1)]; }));
		menu->addChild(createMenuItem("Reset spectrum", "", [=]() { m->initSpectrum(); }));
		menu->addChild(createMenuItem("Reset modulation", "", [=]() {
			for (int r = 0; r < PR_MODSRC; r++)
				for (int c = 0; c < PR_MOD_N; c++) m->mod[r][c] = 0.f;
		}));
	}
};

Model* modelPrism = createModel<Prism, PrismWidget>("Prism");
