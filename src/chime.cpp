// =============================================================================
// Chime — 8-note resonating drone machine.
//
// Inspired by a xylophone whose resonator tubes rotate on an axis beneath the
// bars: each tube's coupling (and so the note's loudness) peaks as it swings
// through center. Eight channels, each with a note (scale degree) and a
// semi-free bidirectional "rotation" LFO. Amplitude follows center proximity;
// the LFO is also available at a CV jack for filter cutoff etc.
//
// Excitation (switch):
//   BOW    — continuous bar-partial drone, purely swelled by the tube window.
//   STRIKE — the bar is struck each time its tube passes center (two crossings
//            per rotation, like the real mechanism); the ring then fades as
//            the tube turns away.
//
// LFO relationships (RELATE + SPREAD):
//   RAMP    — rates fan smoothly slow→fast across channels
//   STEPPED — integer rate ratios (periodic realignment, phasing patterns)
//   RANDOM  — seeded random ratios (RESEED button / trigger)
// DRIFT adds a slow per-channel random rate wobble so nothing locks perfectly.
//
// Notes: ROOT + SCALE (canonical sfs::SCALES — Note/Arrange CV convention) with
// per-channel scale degree assigned on the display (drag a column; scroll).
// =============================================================================

#include "plugin.hpp"
#include "scales.hpp"
#include "panel-style.hpp"
#include <algorithm>
#include <cmath>

static const int CHIME_NCH = 8;
static const int CHIME_NPART = 3;          // bar partials per voice
static const int CHIME_NDEG = 16;          // degree steps reachable per channel

// Bar-mode partial ratios/levels (xylophone-ish inharmonic stack).
static const float PART_RATIO[CHIME_NPART] = {1.f, 3.932f, 9.538f};
static const float PART_AMP[CHIME_NPART]   = {1.f, 0.40f, 0.15f};
static const float PART_DECAY[CHIME_NPART] = {1.f, 0.45f, 0.22f};   // × DECAY knob

struct Chime : Module {
	// NOTE: Rack serialises params/ports POSITIONALLY. Only ever APPEND to these
	// enums — inserting in the middle silently shifts every later value (and every
	// cable) in already-saved patches.
	enum ParamId {
		RATE_PARAM, SPREAD_PARAM, DRIFT_PARAM, RELATE_PARAM,
		EXCITE_PARAM, DECAY_PARAM, ROOT_PARAM, SCALE_PARAM, RESEED_PARAM,
		ENUMS(DEGREE_PARAM, CHIME_NCH),
		ENUMS(ENABLE_PARAM, CHIME_NCH),          // retired (weight silences a note) — slot kept
		ENUMS(WEIGHT_PARAM, CHIME_NCH),
		SHAPE_PARAM, OCT_PARAM,
		ENUMS(ATTEN_PARAM, CHIME_NCH),
		PARAMS_LEN
	};
	enum InputId {
		RATE_INPUT, SPREAD_INPUT, DRIFT_INPUT, ROOT_INPUT, SCALE_INPUT, RESEED_INPUT,
		CLOCK_INPUT,
		ENUMS(ENABLE_INPUT, CHIME_NCH),          // retired — slot kept
		EXCITE_INPUT, OCT_INPUT,
		// ── appended for the 2026-08 panel; do not reorder ──────────────────
		RELATE_INPUT, SHAPE_INPUT, DECAY_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(LFO_OUTPUT, CHIME_NCH),
		ENUMS(AUDIO_OUTPUT, CHIME_NCH),
		MIX_L_OUTPUT, MIX_R_OUTPUT,
		VOCT_OUTPUT, GATE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { ENUMS(ENABLE_LIGHT, CHIME_NCH), LIGHTS_LEN };

	// per-channel LFO ("tube rotation")
	float phase[CHIME_NCH] = {};            // 0..1
	float tri[CHIME_NCH] = {};              // bidirectional -1..+1
	float window[CHIME_NCH] = {};           // center proximity 0..1 (display + amp)
	float randMul[CHIME_NCH];               // RANDOM-mode rate exponents 0..1
	float wob[CHIME_NCH] = {}, wobTarget[CHIME_NCH] = {};
	float wobTimer[CHIME_NCH] = {};

	// per-channel voice
	float partPhase[CHIME_NCH][CHIME_NPART] = {};
	float partEnv[CHIME_NCH][CHIME_NPART] = {};   // STRIKE ring envelope
	float strikeT[CHIME_NCH] = {};               // attack window after a crossing (s)
	float freq[CHIME_NCH] = {};                  // target pitch from root/scale/degree/octave
	float freqLatched[CHIME_NCH] = {};           // pitch a sounding note keeps until it next restarts
	float freqSm[CHIME_NCH] = {};                // smoothed (declicked) frequency
	float barFlash[CHIME_NCH] = {};              // display: bar lights when struck
	int   curOct = 0;
	float winSm[CHIME_NCH] = {};                 // smoothed tube window
	float dispLevel[CHIME_NCH] = {};             // what this voice is actually sounding (display)
	float lastTri[CHIME_NCH] = {};

	dsp::SchmittTrigger reseedTrig, reseedBtnTrig;
	dsp::ClockDivider ctrlDiv;             // control-rate updates (freq, rates)
	float rateEff[CHIME_NCH] = {};
	int   curRelate = 0;
	float ripple[CHIME_NCH] = {};           // RIPPLE-mode excitation energy per tube
	float rippleCoupling = 0.f;
	bool  lastStruck[CHIME_NCH] = {};       // did the most recent pass sound? (gate + weight)

	// clock sync: rotations lock to measured clock tempo
	dsp::SchmittTrigger clockTrig;
	float clkInterval = 0.f;               // seconds between clock edges (smoothed)
	float clkSince = 1e6f;                 // seconds since last edge

	Chime() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, std::log2(0.02f), std::log2(2.f), std::log2(0.15f), "Rotation rate", " Hz", 2.f);
		configParam(SPREAD_PARAM, 0.f, 1.f, 0.35f, "Rate spread (Ripple: coupling)", "%", 0.f, 100.f);
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.15f, "Drift (semi-free running)", "%", 0.f, 100.f);
		configSwitch(RELATE_PARAM, 0.f, 3.f, 0.f, "Relate",
			{"Ramp (slow → fast)", "Stepped (integer ratios)", "Random (seeded)", "Ripple (strikes excite neighbours)"});
		getParamQuantity(RELATE_PARAM)->snapEnabled = true;
		configParam(SHAPE_PARAM, -1.f, 1.f, 0.f, "Rotation curve (− exponential · 0 linear · + logarithmic)");
		configParam(OCT_PARAM, -3.f, 3.f, 0.f, "Octave");
		getParamQuantity(OCT_PARAM)->snapEnabled = true;
		configParam(EXCITE_PARAM, 0.f, 1.f, 0.f, "Excitation (bow ← → strike)", "%", 0.f, 100.f);
		configParam(DECAY_PARAM, 0.3f, 8.f, 2.5f, "Ring / bloom time", " s");
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root",
			{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		getParamQuantity(ROOT_PARAM)->snapEnabled = true;
		{
			std::vector<std::string> names;
			for (int i = 0; i < sfs::NUM_SCALES; i++) names.push_back(sfs::SCALES[i].longName);
			configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 0.f, "Scale", names);
		}
		getParamQuantity(SCALE_PARAM)->snapEnabled = true;
		configButton(RESEED_PARAM, "Reseed random rates");
		for (int c = 0; c < CHIME_NCH; c++) {
			configParam(DEGREE_PARAM + c, 0.f, (float)(CHIME_NDEG - 1), (float)c, string::f("Note %d scale degree", c + 1));
			getParamQuantity(DEGREE_PARAM + c)->snapEnabled = true;
			configParam(WEIGHT_PARAM + c, 0.f, 1.f, 1.f, string::f("Note %d weight (strike likelihood / bow level)", c + 1), "%", 0.f, 100.f);
			// Panel-labelled FREQ. NOTE THE POLARITY: this is the arc WIDTH, and a
			// narrower arc is crossed sooner, so turning it DOWN strikes more often.
			configParam(ATTEN_PARAM + c, 0.1f, 1.f, 1.f, string::f("Note %d arc width (narrower → strikes more often)", c + 1), "%", 0.f, 100.f);
			configOutput(LFO_OUTPUT + c, string::f("Note %d tube LFO (±5V)", c + 1));
			configOutput(AUDIO_OUTPUT + c, string::f("Note %d audio", c + 1));
		}
		configInput(RATE_INPUT, "Rotation rate CV (±5V)");
		configInput(SPREAD_INPUT, "Rate spread CV (±5V)");
		configInput(DRIFT_INPUT, "Drift CV (±5V)");
		configInput(ROOT_INPUT, "Root CV (1V/oct, semitone-quantized)");
		configInput(SCALE_INPUT, "Scale CV (1V per scale)");
		configInput(RESEED_INPUT, "Reseed trigger");
		configInput(CLOCK_INPUT, "Clock (syncs rotations; RATE knob picks 32/16/8/4/2/1 clocks per rotation)");
		configInput(EXCITE_INPUT, "Excitation CV (0–10V, bow → strike)");
		configInput(OCT_INPUT, "Octave CV (1V per octave)");
		configInput(RELATE_INPUT, "Relate CV (0–10V across the four modes)");
		configInput(SHAPE_INPUT, "Rotation curve CV (±5V)");
		configInput(DECAY_INPUT, "Ring / bloom time CV (1 s/V)");
		configOutput(MIX_L_OUTPUT, "Mix left");
		configOutput(MIX_R_OUTPUT, "Mix right");
		configOutput(VOCT_OUTPUT, "V/oct (polyphonic, 8 channels)");
		configOutput(GATE_OUTPUT, "Gate while a note blooms (polyphonic, 8 channels)");
		ctrlDiv.setDivision(64);
		reseed();
		// stagger initial phases so the machine doesn't start in unison
		for (int c = 0; c < CHIME_NCH; c++) phase[c] = (float)c / CHIME_NCH;
	}

	void reseed() {
		for (int c = 0; c < CHIME_NCH; c++) randMul[c] = random::uniform();
	}

	void onReset() override {
		for (int c = 0; c < CHIME_NCH; c++) {
			phase[c] = (float)c / CHIME_NCH;
			wob[c] = wobTarget[c] = 0.f; wobTimer[c] = 0.f;
			for (int p = 0; p < CHIME_NPART; p++) { partEnv[c][p] = 0.f; partPhase[c][p] = 0.f; }
		}
		reseed();
	}

	int curScale() {
		float sv = params[SCALE_PARAM].getValue();
		if (inputs[SCALE_INPUT].isConnected()) sv += inputs[SCALE_INPUT].getVoltage();
		return clamp((int)std::round(sv), 0, sfs::NUM_SCALES - 1);
	}
	int curRoot() {
		float rv = params[ROOT_PARAM].getValue();
		if (inputs[ROOT_INPUT].isConnected()) rv += inputs[ROOT_INPUT].getVoltage() * 12.f;
		int r = (int)std::round(rv);
		return ((r % 12) + 12) % 12;
	}

	// degree index → semitones above root-C3
	float degreeSemis(int deg, int scaleIdx) {
		const sfs::Scale& sc = sfs::SCALES[scaleIdx];
		int oct = deg / sc.size, step = deg % sc.size;
		return sc.intervals[step] + 12.f * oct;
	}

	void process(const ProcessArgs& args) override {
		if (reseedBtnTrig.process(params[RESEED_PARAM].getValue()) ||
		    reseedTrig.process(inputs[RESEED_INPUT].getVoltage(), 0.1f, 1.f))
			reseed();

		float exciteX = clamp(params[EXCITE_PARAM].getValue() + inputs[EXCITE_INPUT].getVoltage() / 10.f, 0.f, 1.f);   // 0 bow → 1 strike
		float shapeC = clamp(params[SHAPE_PARAM].getValue() + inputs[SHAPE_INPUT].getVoltage() / 5.f, -1.f, 1.f);
		float shapeP = std::exp2(shapeC * 2.f);                           // 0.25 exp ‥ 1 linear ‥ 4 log
		float decayK = clamp(params[DECAY_PARAM].getValue() + inputs[DECAY_INPUT].getVoltage(), 0.3f, 8.f);

		// clock measurement
		clkSince += args.sampleTime;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (clkSince < 4.f) clkInterval = (clkInterval > 0.f) ? clkInterval * 0.7f + clkSince * 0.3f : clkSince;
			clkSince = 0.f;
		}
		bool clocked = inputs[CLOCK_INPUT].isConnected() && clkInterval > 0.f &&
		               clkSince < std::max(4.f * clkInterval, 2.f);

		// control-rate: rates, wobble, frequencies
		if (ctrlDiv.process()) {
			float dt = args.sampleTime * ctrlDiv.getDivision();
			float rate;
			if (clocked) {
				// RATE knob picks a musical division: clocks per full rotation.
				// Even divisions → center crossings (strikes) land on clock beats.
				static const float DIVS[6] = {32.f, 16.f, 8.f, 4.f, 2.f, 1.f};
				float lo = std::log2(0.02f), hi = std::log2(2.f);
				float k = clamp((params[RATE_PARAM].getValue() + inputs[RATE_INPUT].getVoltage() / 2.5f - lo) / (hi - lo), 0.f, 0.999f);
				rate = 1.f / (clkInterval * DIVS[(int)(k * 6.f)]);
			} else {
				rate = std::exp2(params[RATE_PARAM].getValue() + inputs[RATE_INPUT].getVoltage() / 2.5f);
			}
			float spread = clamp(params[SPREAD_PARAM].getValue() + inputs[SPREAD_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			float drift = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			int relate = clamp((int)std::round(params[RELATE_PARAM].getValue()
			                                  + inputs[RELATE_INPUT].getVoltage() * 0.3f), 0, 3);
			curRelate = relate;
			rippleCoupling = spread;
			float maxR = 1.f + spread * 7.f;
			int sci = curScale(); int root = curRoot();
			curOct = clamp((int)std::round(params[OCT_PARAM].getValue() + inputs[OCT_INPUT].getVoltage()), -4, 4);
			for (int c = 0; c < CHIME_NCH; c++) {
				float x = (CHIME_NCH > 1) ? (float)c / (CHIME_NCH - 1) : 0.f;
				float mult;
				switch (relate) {
					case 1:  mult = std::max(1.f, std::round(std::pow(maxR, x))); break;   // stepped
					case 2:  mult = std::pow(maxR, randMul[c]); break;                     // random
					case 3:  mult = 1.f + 3.f * ripple[c]; break;                          // ripple: excitation spins the tube
					default: mult = std::pow(maxR, x); break;                              // ramp
				}
				// slow per-channel wobble: semi-free running
				wobTimer[c] -= dt;
				if (wobTimer[c] <= 0.f) { wobTimer[c] = 2.f + 3.f * random::uniform(); wobTarget[c] = random::uniform() * 2.f - 1.f; }
				wob[c] += (wobTarget[c] - wob[c]) * std::min(1.f, dt / 2.f);
				rateEff[c] = rate * mult * std::exp2(drift * wob[c]);
				int deg = clamp((int)std::round(params[DEGREE_PARAM + c].getValue()), 0, CHIME_NDEG - 1);
				freq[c] = 130.81f * std::exp2((root + degreeSemis(deg, sci)) / 12.f + curOct);
			}
		}

		// declick time constants
		const float kWin  = std::min(1.f, args.sampleTime / 0.004f);   // window smoother
		const float kAtk  = std::min(1.f, args.sampleTime / 0.0008f);  // strike attack rise

		float mixL = 0.f, mixR = 0.f;
		for (int c = 0; c < CHIME_NCH; c++) {
			// Tube rotation LFO. ATTEN shrinks the arc; because the tube keeps the
			// same angular speed, a shorter arc is traversed sooner — so the swing
			// narrows AND the note strikes more often, and never falls fully silent.
			float att = clamp(params[ATTEN_PARAM + c].getValue(), 0.1f, 1.f);
			phase[c] += rateEff[c] / att * args.sampleTime;
			if (phase[c] >= 1.f) phase[c] -= 1.f;
			float t = 1.f - 4.f * std::fabs(phase[c] - 0.5f);        // -1..+1..-1
			// Curve shaping (symmetric about center, so crossing TIMES are unchanged —
			// clock sync and strike rhythm hold). Negative = exponential: the tube
			// dwells at the extremes and whips through center (short bright blooms).
			// Positive = logarithmic: it lingers near center (long swells, brief gaps).
			if (shapeP != 1.f) {
				float a = std::pow(std::fabs(t), shapeP);
				t = (t < 0.f) ? -a : a;
			}
			t *= att;                                                // shape first, then shrink the arc
			tri[c] = t;
			float w = 1.f - std::fabs(t);                            // center proximity
			w *= w;                                                  // sharper bloom
			winSm[c] += (w - winSm[c]) * kWin;                       // rounds off the kink at center
			window[c] = winSm[c];

			// center crossing (twice per rotation, like the real tubes)
			bool crossed = (lastTri[c] < 0.f) != (t < 0.f);
			lastTri[c] = t;
			float wgt = params[WEIGHT_PARAM + c].getValue();
			if (crossed && curRelate == 3) {                         // ripple: excite the neighbours
				if (c > 0)            ripple[c - 1] = std::min(1.5f, ripple[c - 1] + rippleCoupling * 0.6f * params[WEIGHT_PARAM + c - 1].getValue());
				if (c < CHIME_NCH - 1) ripple[c + 1] = std::min(1.5f, ripple[c + 1] + rippleCoupling * 0.6f * params[WEIGHT_PARAM + c + 1].getValue());
			}
			ripple[c] -= ripple[c] * args.sampleTime / 0.6f;         // excitation dies down
			// weight = strike likelihood: the hammer only lands wgt of the passes
			bool struckNow = false;
			if (crossed) {
				lastStruck[c] = random::uniform() < wgt;
				if (lastStruck[c]) { strikeT[c] = 0.0025f; struckNow = true; }   // attack window — rise, don't jump
			}
			bool attacking = strikeT[c] > 0.f;
			if (attacking) strikeT[c] -= args.sampleTime;
			if (struckNow) barFlash[c] = 1.f;
			barFlash[c] -= barFlash[c] * args.sampleTime / 0.12f;

			// Pitch LATCHES at note boundaries — a strike (the note is restarting) or
			// the quiet point at the tube's extremes — and SNAPS there rather than
			// gliding: at a strike the attack masks it, and at the quiet point there's
			// nothing to hear. Gliding instead would bend the sounding note, which is
			// exactly what a root/scale/octave change must never do.
			if (struckNow || winSm[c] < 0.03f || wgt <= 0.001f || freqLatched[c] <= 0.f) {
				freqLatched[c] = freq[c];
				freqSm[c] = freq[c];
			}

			// voice: bar partials, bow↔strike interpolated per partial
			float v = 0.f;
			for (int p = 0; p < CHIME_NPART; p++) {
				partPhase[c][p] += freqSm[c] * PART_RATIO[p] * args.sampleTime;
				if (partPhase[c][p] >= 1.f) partPhase[c][p] -= 1.f;
				if (attacking) partEnv[c][p] += (1.f - partEnv[c][p]) * kAtk;   // continuous retrigger
				else partEnv[c][p] *= std::exp(-args.sampleTime / (decayK * PART_DECAY[p] * 0.25f));
				float env = (1.f - exciteX) + exciteX * partEnv[c][p];          // bow bed → struck ring
				v += PART_AMP[p] * env * std::sin(2.f * M_PI * partPhase[c][p]);
			}
			v *= winSm[c];                                           // the rotating tube's coupling
			v *= (1.f - exciteX) * wgt + exciteX;                    // bow end: weight = level
			float out = v * 3.5f;                                    // per-channel level
			outputs[AUDIO_OUTPUT + c].setVoltage(out);
			outputs[LFO_OUTPUT + c].setVoltage(t * 5.f);             // attenuated swing

			// what this voice is really sounding — drives the display, so a strike
			// skipped by weight never lights up
			float lvl = winSm[c] * ((1.f - exciteX) * wgt + exciteX)
			          * ((1.f - exciteX) + exciteX * partEnv[c][0]);
			dispLevel[c] += (clamp(lvl, 0.f, 1.f) - dispLevel[c]) * kWin;

			// poly CV/gate: Chime as a generative sequencer
			outputs[VOCT_OUTPUT].setVoltage(std::log2(std::max(freqSm[c], 8.f) / 261.63f), c);
			bool gate = winSm[c] > 0.2f && wgt > 0.001f && (exciteX < 0.5f || lastStruck[c]);
			outputs[GATE_OUTPUT].setVoltage(gate ? 10.f : 0.f, c);

			float pan = (CHIME_NCH > 1) ? (float)c / (CHIME_NCH - 1) : 0.5f;   // 0..1 L→R
			mixL += out * std::cos(pan * M_PI_2);
			mixR += out * std::sin(pan * M_PI_2);
		}
		outputs[VOCT_OUTPUT].setChannels(CHIME_NCH);
		outputs[GATE_OUTPUT].setChannels(CHIME_NCH);
		float mixScale = 0.5f;                                       // 8 voices headroom
		outputs[MIX_L_OUTPUT].setVoltage(mixL * mixScale);
		outputs[MIX_R_OUTPUT].setVoltage(mixR * mixScale);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		for (int c = 0; c < CHIME_NCH; c++) json_array_append_new(arr, json_real(randMul[c]));
		json_object_set_new(root, "randMul", arr);
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* arr = json_object_get(root, "randMul"))
			for (int c = 0; c < CHIME_NCH && c < (int)json_array_size(arr); c++)
				randMul[c] = (float)json_real_value(json_array_get(arr, c));
	}
};

// ── display ──────────────────────────────────────────────────────────────────
// Eight columns. Each shows its degree label and the rotating tube seen from
// the side: a line rocking about a center pivot, glowing as it passes through
// horizontal (max resonance). Drag a column vertically to change its degree.

static const NVGcolor CHBG        = nvgRGB(0x1A, 0x1A, 0x32);
static const NVGcolor CHBLUE      = nvgRGB(0x00, 0x97, 0xDE);
static const NVGcolor CHPURPLE    = nvgRGB(0x35, 0x35, 0x4D);
static const NVGcolor CHPURPLE_MID= nvgRGB(0x4A, 0x4A, 0x66);
static const NVGcolor CHORANGE    = nvgRGB(0xEC, 0x65, 0x2E);
static const NVGcolor CHTEXT      = nvgRGB(0xE8, 0xE8, 0xF0);
static const NVGcolor CHTEXT_DIM  = nvgRGB(0x8A, 0x8A, 0xA5);

struct ChimeDisplay : Widget {
	Chime* module = nullptr;
	std::shared_ptr<Font> font;
	int dragCh = -1;
	float dragStartY = 0.f, dragStartDeg = 0.f;

	// semitones above the engine's base note (130.81Hz = C3)
	int noteSemis(int deg, int scaleIdx, int root, int octShift) {
		const sfs::Scale& sc = sfs::SCALES[scaleIdx];
		int oct = deg / sc.size, step = deg % sc.size;
		return root + (int)sc.intervals[step] + 12 * (oct + octShift);
	}
	// actual sounding note for a degree, e.g. root A + dorian degree 3 → "C4"
	std::string noteLabel(int semis) {
		static const char* NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		int pc = ((semis % 12) + 12) % 12;
		int octNum = 3 + (int)std::floor(semis / 12.f);
		return std::string(NN[pc]) + std::to_string(octNum);
	}
	// a resonator is sized by its note: an octave up is a shorter, narrower tube
	// (compressed from the true 1/f so eight notes still fit one lane)
	float noteSize(int semis) { return clamp(1.1f - 0.26f * (semis / 12.f), 0.48f, 1.1f); }

	// One note: the bar (key) up top, its resonator tube rotating on an axle below,
	// and the weight bar at the foot. The tube is drawn in fake 3D — it turns about
	// the axle, so its bore opens toward you as it swings through center (loudest)
	// and closes to an edge as it turns away (silent).
	// `level` is what the voice is audibly doing, not merely where the tube is —
	// a pass whose strike was skipped by weight stays cold.
	void drawColumn(NVGcontext* vg, float x, float w, float h, float t, float level,
	                const std::string& label, bool preview, float weight,
	                float flash = 0.f, float sizeF = 1.f) {
		bool dim = preview;
		float win = level;
		float cx = x + w / 2;
		float laneTop = 26.f, laneH = h - laneTop - 14.f, cy = laneTop + laneH * 0.5f;

		nvgBeginPath(vg); nvgRoundedRect(vg, x + 1.5f, laneTop, w - 3.f, laneH, 3.f);
		nvgFillColor(vg, CHPURPLE); nvgFill(vg);

		// the bar (key) — sized with its note, outlined, fills while the strike flashes
		NVGcolor barCol = dim ? CHPURPLE_MID : nvgLerpRGBA(CHBLUE, CHORANGE, flash);
		float barW = (w - 9.f) * clamp(sizeF, 0.5f, 1.f);
		nvgBeginPath(vg); nvgRoundedRect(vg, cx - barW / 2, 15.5f, barW, 4.f, 1.5f);
		if (flash > 0.01f && !dim) { nvgFillColor(vg, nvgRGBAf(0.92f, 0.4f, 0.18f, flash)); nvgFill(vg); }
		nvgStrokeColor(vg, barCol); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

		// weight bar: strike likelihood / bow level
		float wy = h - 10.f;
		nvgBeginPath(vg); nvgRoundedRect(vg, x + 3.f, wy, w - 6.f, 6.f, 2.f);
		nvgFillColor(vg, CHPURPLE); nvgFill(vg);
		if (weight > 0.01f) {
			nvgBeginPath(vg); nvgRoundedRect(vg, x + 3.f, wy, (w - 6.f) * weight, 6.f, 2.f);
			nvgFillColor(vg, dim ? CHPURPLE_MID : CHBLUE); nvgFill(vg);
		}
		// --- the tube, seen from above ---
		// We look down on the row. Each tube swings like a pendulum about the axle
		// through its middle, ±90° — so it lies flat and full-length at the ends of
		// the swing, and at the centre it points straight up at us and collapses to
		// a bare circle. That circle is the strike. Only the mouth end lights.
		nvgBeginPath(vg); nvgMoveTo(vg, x + 4.f, cy); nvgLineTo(vg, x + w - 4.f, cy);
		nvgStrokeColor(vg, CHPURPLE_MID); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

		float th = t * 1.5708f;                               // ±90°: never further
		float sn = std::sin(th), ap = std::fabs(std::cos(th));// ap 1 = square-on to us
		float Lh = laneH * 0.31f * sizeF;                     // half-length, sized by note
		float R  = w * 0.15f * sizeF;
		float mouthY = cy - Lh * sn, tailY = cy + Lh * sn;    // ends sweep about the axle
		float Rm = R * (1.f + 0.18f * ap);                    // mouth is the nearer end
		float Rt = R * (1.f - 0.18f * ap);

		float hot = std::max(win, flash);                     // resonance / strike
		NVGcolor bodyCol  = dim ? CHPURPLE_MID : nvgRGBA(0x00, 0x97, 0xDE, 0xC8);
		NVGcolor mouthCol = dim ? CHPURPLE_MID : nvgLerpRGBA(CHBLUE, CHORANGE, hot);

		nvgStrokeColor(vg, bodyCol); nvgStrokeWidth(vg, 1.2f);
		nvgBeginPath(vg); nvgEllipse(vg, cx, tailY, Rt, std::max(0.5f, Rt * ap));  // closed end
		nvgStroke(vg);
		if (std::fabs(mouthY - tailY) > 0.5f) {               // barrel
			nvgBeginPath(vg);
			nvgMoveTo(vg, cx - Rt, tailY); nvgLineTo(vg, cx - Rm, mouthY);
			nvgMoveTo(vg, cx + Rt, tailY); nvgLineTo(vg, cx + Rm, mouthY);
			nvgStroke(vg);
		}
		nvgBeginPath(vg); nvgEllipse(vg, cx, mouthY, Rm, std::max(0.5f, Rm * ap));// open mouth
		nvgFillColor(vg, nvgRGBAf(0.02f, 0.03f, 0.07f, 0.55f));                   // the bore: see-through dark
		nvgFill(vg);
		nvgStrokeColor(vg, mouthCol); nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
		if (win > 0.08f && !dim) {                            // resonance rings out of the bore
			for (int k = 1; k <= 2; k++) {
				float rr = Rm * (1.f + 0.6f * k + 0.8f * win);
				nvgBeginPath(vg); nvgEllipse(vg, cx, mouthY, rr, std::max(0.5f, rr * ap));
				nvgStrokeColor(vg, nvgRGBAf(0.92f, 0.4f, 0.18f, (0.45f / k) * win));
				nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			}
		}
		nvgBeginPath(vg); nvgCircle(vg, cx, cy, 1.2f);        // axle pin
		nvgFillColor(vg, dim ? CHPURPLE_MID : CHTEXT_DIM); nvgFill(vg);

		// note name
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 10.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, dim ? CHTEXT_DIM : CHTEXT);
			nvgText(vg, cx, 8.f, label.c_str(), NULL);
		}
	}

	void draw(const DrawArgs& args) override {
		if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
		nvgFillColor(vg, CHBG); nvgFill(vg);
		float colW = w / CHIME_NCH;
		if (!module) { drawPreview(vg, colW, h); return; }
		int sci = module->curScale(); int root = module->curRoot();
		for (int c = 0; c < CHIME_NCH; c++) {
			int deg = clamp((int)std::round(module->params[Chime::DEGREE_PARAM + c].getValue()), 0, CHIME_NDEG - 1);
			int semis = noteSemis(deg, sci, root, module->curOct);
			drawColumn(vg, c * colW, colW, h, module->tri[c],
			           module->dispLevel[c], noteLabel(semis), false,
			           module->params[Chime::WEIGHT_PARAM + c].getValue(),
			           module->barFlash[c], noteSize(semis));
		}
	}

	// browser thumbnail: staggered tube angles, one at resonance
	void drawPreview(NVGcontext* vg, float colW, float h) {
		static const float t[CHIME_NCH] = {-0.9f, -0.55f, -0.2f, 0.05f, 0.35f, 0.6f, 0.85f, -0.4f};
		static const char* lbl[CHIME_NCH] = {"C3", "D3", "E3", "G3", "A3", "C4", "E4", "G4"};
		static const int   sem[CHIME_NCH] = {0, 2, 4, 7, 9, 12, 16, 19};
		static const float wt[CHIME_NCH] = {1.f, 0.8f, 1.f, 0.6f, 1.f, 0.45f, 0.9f, 0.7f};
		for (int c = 0; c < CHIME_NCH; c++) {
			float win = 1.f - std::fabs(t[c]); win *= win;
			drawColumn(vg, c * colW, colW, h, t[c], win, lbl[c], true, wt[c],
			           0.f, noteSize(sem[c]));
		}
	}

	float totalDrag = 0.f;
	bool dragWeight = false;
	float dragStartW = 0.f;

	void onButton(const event::Button& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			float colW = box.size.x / CHIME_NCH;
			dragCh = clamp((int)(e.pos.x / colW), 0, CHIME_NCH - 1);
			dragWeight = e.pos.y > box.size.y - 14.f;              // bottom strip = weight
			totalDrag = 0.f;
			if (dragWeight) {
				dragStartW = clamp((e.pos.x - dragCh * colW - 3.f) / (colW - 6.f), 0.f, 1.f);
				module->params[Chime::WEIGHT_PARAM + dragCh].setValue(dragStartW);
			} else {
				dragStartDeg = module->params[Chime::DEGREE_PARAM + dragCh].getValue();
			}
			e.consume(this);
			return;
		}
		Widget::onButton(e);
	}
	void onDragMove(const event::DragMove& e) override {
		if (!module || dragCh < 0) return;
		if (dragWeight) {
			float colW = box.size.x / CHIME_NCH;
			totalDrag += e.mouseDelta.x / getAbsoluteZoom();
			float w = clamp(dragStartW + totalDrag / (colW - 6.f), 0.f, 1.f);
			module->params[Chime::WEIGHT_PARAM + dragCh].setValue(w);
		} else {
			totalDrag += e.mouseDelta.y / getAbsoluteZoom();     // 12px per degree, up = higher
			float newDeg = clamp(dragStartDeg - totalDrag / 12.f, 0.f, (float)(CHIME_NDEG - 1));
			module->params[Chime::DEGREE_PARAM + dragCh].setValue(std::round(newDeg));
		}
	}
	void onDragEnd(const event::DragEnd& e) override { dragCh = -1; dragWeight = false; Widget::onDragEnd(e); }
	void onHoverScroll(const event::HoverScroll& e) override {
		if (!module) { Widget::onHoverScroll(e); return; }
		int c = clamp((int)(e.pos.x / (box.size.x / CHIME_NCH)), 0, CHIME_NCH - 1);
		float d = module->params[Chime::DEGREE_PARAM + c].getValue() + ((e.scrollDelta.y > 0.f) ? 1.f : -1.f);
		module->params[Chime::DEGREE_PARAM + c].setValue(clamp(d, 0.f, (float)(CHIME_NDEG - 1)));
		e.consume(this);
	}
};

// Live key readout above the display. Reads the RESOLVED root/scale, so it stays
// truthful when ROOT/SCALE CV is patched and the knobs no longer tell the story.
struct ChimeKeyReadout : Widget {
	Chime* module = nullptr;
	std::shared_ptr<Font> font;
	void draw(const DrawArgs& args) override {
		if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font || font->handle < 0) return;
		static const char* NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		std::string txt = "— —";
		if (module) {
			int sci = clamp(module->curScale(), 0, sfs::NUM_SCALES - 1);
			txt = std::string(NN[module->curRoot()]) + "  " + sfs::SCALES[sci].shortName;
			for (char& ch : txt) ch = (char)std::toupper((unsigned char)ch);
			if (module->curOct) txt += string::f("   OCT %+d", module->curOct);
		}
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 9.f);
		nvgFillColor(args.vg, nvgRGB(0x40, 0x40, 0x40));
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, 0, box.size.y / 2, txt.c_str(), NULL);
	}
};

// Horizontal bow↔strike blend slider (the SDK only ships vertical sliders).
// Track and pointer are the designer's artwork rather than drawn shapes: both
// carry shadows, and nanosvg implements no SVG filters, so they arrive as PNG
// at 8× Rack's own pixel scale. Sizes below are that artwork divided back down.
struct ChimeExciteSlider : app::SliderKnob {
	ChimeExciteSlider() {
		horizontal = true;
		box.size = mm2px(Vec(45.72f, 5.08f));       // hp(9) x hp(1)
	}
	// Loaded per frame and deliberately NOT held as a member: Rack destroys the
	// Window before the Scene, so an Image kept alive in a widget deletes its
	// texture through a dead GL context on quit. Its own header says so.
	static void blit(NVGcontext* vg, const char* path, float x, float y, float w, float h) {
		std::shared_ptr<Image> img = APP->window->loadImage(asset::plugin(pluginInstance, path));
		if (!img || img->handle < 0) return;
		NVGpaint paint = nvgImagePattern(vg, x, y, w, h, 0.f, img->handle, 1.f);
		nvgBeginPath(vg);
		nvgRect(vg, x, y, w, h);
		nvgFillPaint(vg, paint);
		nvgFill(vg);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		float v = 0.f;
		if (ParamQuantity* pq = getParamQuantity()) v = clamp(pq->getScaledValue(), 0.f, 1.f);
		blit(vg, "res/chime-slider.png", 0.f, 0.f, w, h);
		float pw = 85.f / 8.f, ph = 116.f / 8.f;     // the pointer PNG, back at 1×
		blit(vg, "res/chime-slider-knob.png", (w - pw) * v, (h - ph) * 0.5f, pw, ph);
	}
};

struct ChimeWidget : ModuleWidget {
	ChimeWidget(Chime* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/chime.svg")));
		using sfs::hp;

		// Everything sits on the quarter-HP grid, in both axes. The control pairs
		// run DOWN their column rather than across a row: label, pot, then the
		// jack that modulates it, joined by a pipe -- so the jack needs no label.
		const float colA = hp(2), colB = hp(4.5f), colC = hp(7);
		// The eight notes, and the three rows they own.
		const float note0 = hp(8.5f), noteStep = hp(2.5f);
		const float freqY = hp(12.5f), lfoY = hp(15.5f), audioY = hp(18);
		const float botY = hp(23.5f);

		// Added before any component so the connecting hairlines draw underneath
		// them rather than across their faces.
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(hp(1), hp(1.6f), "CHIME");

		ChimeDisplay* disp = new ChimeDisplay();
		disp->module = module;
		// 20 cells wide starting at hp(7.25) puts the display's eight internal
		// columns over the eight control columns at hp(8.5) .. hp(26).
		disp->box.pos  = mm2px(Vec(hp(7.25f), hp(2)));
		disp->box.size = mm2px(Vec(hp(20), hp(9)));
		addChild(disp);

		ChimeKeyReadout* key = new ChimeKeyReadout();
		key->module = module;
		key->box.pos  = mm2px(Vec(hp(7.25f), hp(1.1f)));
		key->box.size = mm2px(Vec(hp(12), hp(1.2f)));
		addChild(key);

		// ── left: three rows of two pot-over-jack pairs ─────────────────────────
		// Written out rather than looped over a table: tools/panel_reticules.py
		// reads these positions straight out of the source to place the panel
		// art, and it can evaluate an expression but not a struct member.
		const float rowA = hp(4), rowAj = hp(6);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, rowA)), module, Chime::RATE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, rowAj)), module, Chime::RATE_INPUT));
		lbl->pairDown(colA, rowA, rowAj, "RATE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, rowA)), module, Chime::SPREAD_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, rowAj)), module, Chime::SPREAD_INPUT));
		lbl->pairDown(colB, rowA, rowAj, "SPREAD");

		const float rowB = hp(9), rowBj = hp(11);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, rowB)), module, Chime::RELATE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, rowBj)), module, Chime::RELATE_INPUT));
		lbl->pairDown(colA, rowB, rowBj, "RELATE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, rowB)), module, Chime::SHAPE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, rowBj)), module, Chime::SHAPE_INPUT));
		lbl->pairDown(colB, rowB, rowBj, "CURVE");

		const float rowC = hp(14), rowCj = hp(16);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, rowC)), module, Chime::DECAY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, rowCj)), module, Chime::DECAY_INPUT));
		lbl->pairDown(colA, rowC, rowCj, "DECAY");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, rowC)), module, Chime::DRIFT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, rowCj)), module, Chime::DRIFT_INPUT));
		lbl->pairDown(colB, rowC, rowCj, "DRIFT");

		// Octave takes CV only, and the reseed button stands alone -- neither has a
		// partner, so neither gets a pipe.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, hp(19.5f))), module, Chime::OCT_INPUT));
		lbl->jack(colA, hp(19.5f), "OCT");
		addParam(createParamCentered<VCVButton>(mm2px(Vec(colB, hp(19.5f))), module, Chime::RESEED_PARAM));
		lbl->trim(colB, hp(19.5f), "SEED");

		// ── the key ─────────────────────────────────────────────────────────────
		// ROOT and SCALE sit in the band under the note plate rather than in the
		// left column, because the left column has no room left for a pair. It
		// reads: they are what names the pitches on the screen directly above,
		// and they take the note columns' own x positions so the two line up.
		// Pairs run ACROSS here -- the band is one row tall.
		const float keyY = hp(21);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(note0, keyY)), module, Chime::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(note0 + noteStep, keyY)), module, Chime::ROOT_INPUT));
		lbl->trim(note0, keyY, "ROOT");
		lbl->link(note0, keyY, note0 + noteStep, keyY);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(note0 + noteStep * 2.f, keyY)), module, Chime::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(note0 + noteStep * 3.f, keyY)), module, Chime::SCALE_INPUT));
		lbl->trim(note0 + noteStep * 2.f, keyY, "SCALE");
		lbl->link(note0 + noteStep * 2.f, keyY, note0 + noteStep * 3.f, keyY);

		// ── the eight notes: FREQ on the faceplate, the two out rows on the plate ─
		for (int c = 0; c < CHIME_NCH; c++) {
			float x = note0 + noteStep * c;
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, freqY)), module, Chime::ATTEN_PARAM + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, lfoY)), module, Chime::LFO_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, audioY)), module, Chime::AUDIO_OUTPUT + c));
		}
		// One label for a row of eight, centred between columns 4 and 5, with a
		// tick over each column. FREQ threads a rail through its ticks; the two
		// rows on the plate leave the rail out, so their ticks read as pointers
		// into the plate rather than as a bus running across it.
		auto rowLabel = [&](float y, const char* text, bool rail, bool onPlate) {
			const float mid = note0 + noteStep * 3.5f;
			if (rail) {
				lbl->link(note0, y, mid - hp(0.75f), y, onPlate);
				lbl->link(mid + hp(0.75f), y, note0 + noteStep * 7.f, y, onPlate);
			}
			for (int c = 0; c < CHIME_NCH; c++) {
				float x = note0 + noteStep * c;
				lbl->link(x, y - hp(0.25f), x, y + hp(0.25f), onPlate);
			}
			lbl->add(mid, y, text, onPlate ? sfs::PanelLabels::ON_PLATE
			                               : sfs::PanelLabels::LABEL);
		};
		rowLabel(hp(11.5f),  "FREQ",    true,  false);
		rowLabel(hp(14),     "LFO OUT", false, true);
		rowLabel(hp(16.75f), "AUDIO",   false, true);

		// ── bottom: one row, so every label on it shares a baseline ─────────────
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, botY)), module, Chime::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colB, botY)), module, Chime::GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colC, botY)), module, Chime::VOCT_OUTPUT));
		lbl->jack(colA, botY, "CLOCK");
		lbl->jack(colB, botY, "GATE");
		lbl->jack(colC, botY, "V/OCT");

		// The slider spans hp(10)..hp(19); its labels sit at the ends of the track,
		// on the same baseline as the rest of the row, which is why they are placed
		// by hand rather than through note().
		addParam(createParamCentered<ChimeExciteSlider>(mm2px(Vec(hp(14.5f), botY)), module, Chime::EXCITE_PARAM));
		lbl->add(hp(10), botY - sfs::LABEL_GAP_JACK, "BOW", sfs::PanelLabels::LABEL, NVG_ALIGN_LEFT);
		lbl->add(hp(19), botY - sfs::LABEL_GAP_JACK, "STRIKE", sfs::PanelLabels::LABEL, NVG_ALIGN_RIGHT);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(21), botY)), module, Chime::EXCITE_INPUT));
		lbl->jack(hp(21), botY, "EXCITER");
		lbl->link(hp(19), botY, hp(21), botY);          // slider → its CV jack

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(23.5f), botY)), module, Chime::MIX_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(26), botY)), module, Chime::MIX_R_OUTPUT));
		lbl->jackOnPlate(hp(23.5f), botY, "L");
		lbl->jackOnPlate(hp(26), botY, "R");
	}
};

Model* modelChime = createModel<Chime, ChimeWidget>("Chime");
