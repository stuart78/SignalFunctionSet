// =============================================================================
// Wheel — a drone instrument after the hurdy-gurdy (vielle à roue).
//
// Design and reasoning: docs/wheel-design.md. The short version, because it
// governs nearly every decision below:
//
//   ONE rosined wheel bows every string at once. So nothing here is independent
//   of anything else. The wheel is not round — it is slightly eccentric, its
//   rosin sits unevenly, and it has a seam where it was joined — and every
//   revolution therefore imposes the SAME ripple of level and brightness on
//   every string together. Five oscillators with five LFOs sound like an organ;
//   five oscillators sharing one slightly irregular modulator sound like one
//   object being played by one person. Common-mode modulation is the tell.
//
//   The trompette crosses a bridge with one loose foot. Accelerate the crank
//   and the foot lifts and rattles: the buzz is the rhythm section, and it is
//   the difference between a hurdy-gurdy and a bagpipe. It fires on wheel
//   ACCELERATION, not on a clock, so the mechanism is real rather than a
//   shortcut — and a coup also speeds the wheel, so every drone swells with it.
//
//   Hurdy-gurdy rhythm is counted in strokes per turn of the wheel ("quatre
//   coups par tour"). So CRANK is the tempo, COUPS is the subdivision, and the
//   pattern is a ring of slots around the rim.
//
// Six voices: five drones, of which voice 1 is locked to the root, plus the
// trompette. Oscillators rather than waveguides — Loom already owns bowed-string
// physical modelling in this plugin, and the character here comes from the
// wheel, not from the string.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "scales.hpp"
#include "scale-bus.hpp"
#include "waveguide.hpp"
#include <cmath>
#include <cstring>

static const int WH_V        = 6;    // five drones and the trompette
static const int WH_TRP      = 5;    // its index
static const int WH_MAXCOUPS = 8;    // strokes per turn
static const int WH_MAXDEG   = 14;   // the scale bus's own limit

// 0V is C3, not C4. This is a drone instrument: a hurdy-gurdy's bourdons sit
// around G2–C3, and defaulting an octave higher would put every patch in the
// wrong register before a control was touched.
static const float WH_BASE_HZ = 130.8128f;

// Where round the wheel the seam is. Named because the DSP and the display both
// have to draw it in the same place, and a number written twice is a number that
// drifts. It sits at 0.38 rather than 0.72 for a measured reason: at 0.72 the
// seam landed squarely in the eccentricity wave's own trough and merely made it
// deeper, so the turn had one big dropout in it. Moved onto the rising side it
// is a distinct notch, and the revolution has two features instead of one.
static const float WH_SEAM = 0.38f;

// ── the just-intonation table ────────────────────────────────────────────────
// Drones are tuned by ear to beatless octaves and fifths, which means just
// intonation. It matters more than it sounds, because it is also what makes the
// PHASE control work: a 12-TET fifth is 2^(7/12) = 1.4983, not 1.5, so the third
// harmonic of the root and the second of the fifth beat at about 0.9 Hz at C4.
// Relative phase rotates through a full cycle roughly once a second, and a
// static phase offset is then inaudible — it only picks where in that rotation
// you happen to start. Snapped to 3/2 the phase holds still and becomes a real
// timbral control.
static const float WH_JUST[12] = {
	0.f,      1.1173f,  2.0391f,  3.1564f,  3.8631f,  4.9804f,
	5.8251f,  7.0196f,  8.1369f,  8.8436f,  9.9609f, 10.8827f
};

static const char* WH_NOTE[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// ROOT and SCALE are indices, and an index shown as a number tells the reader
// nothing. Both read out as what they actually select.
struct WheelRootQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		return WH_NOTE[clamp((int)std::round(getValue()), 0, 11)];
	}
};
struct WheelScaleQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		return sfs::SCALES[clamp((int)std::round(getValue()), 0, sfs::NUM_SCALES - 1)].longName;
	}
};

static inline float wheelJustify(float semis) {
	float oct = std::floor(semis / 12.f);
	int pc = (int)std::round(semis - oct * 12.f);
	while (pc >= 12) { pc -= 12; oct += 1.f; }
	while (pc <  0)  { pc += 12; oct -= 1.f; }
	return oct * 12.f + WH_JUST[pc];
}

// How hard each stroke of the turn is: the metric hierarchy, which is what a
// wrist is actually doing. A stroke landing on a coarser grid is a heavier one.
//
// This is not decoration. Without a spread of strengths DOG has nothing to
// discriminate between and can only be a mute switch. But the spread has to be
// SMOOTH, and the first ladder was not: at eight coups it ran
// 1.00 · 0.52 · 0.70 · 0.52 · 0.82 · 0.52 · 0.70 · 0.52, so four of the eight
// strokes were identical and there was a 0.18 gap above them. DOG fell from
// "all eight" to "four" almost immediately and then sat on four for two thirds
// of its travel — measured, and the reason eight coups sounded like one.
//
// gcd(k, n) says how coarse a grid stroke k lands on, for ANY n rather than for
// the powers of two the hand-written rules covered: at n = 6 it correctly makes
// stroke 3 the half and strokes 2 and 4 the thirds, which the old `k*4 == n`
// tests missed entirely.
static inline int wheelGcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static inline float wheelAccent(int k, int n) {
	// The jitter at the call site is doing real work as well as humanising: it
	// turns each threshold crossing into a band where a stroke fires SOMETIMES,
	// so DOG sweeps the density of the buzz continuously rather than stepping.
	if (k <= 0 || n <= 1) return 1.f;                       // the downbeat
	float g = (float)wheelGcd(k, n);
	return 0.62f + 0.38f * (std::log2(g) / std::log2((float)n));
}

// TEMPER is inert on anything that is not twelve-tone equal temperament, the
// same way Key's free-sub-scale option is. Snapping Pelog toward just ratios
// only makes it not Pelog: its intervals ARE the tuning, and there is no
// intended ratio to snap toward.
static inline bool wheelIs12TET(const sfs::BusScale& sc) {
	if (std::fabs(sc.period - 12.f) > 0.02f) return false;
	for (int k = 0; k < sc.size; k++)
		if (std::fabs(sc.intervals[k] - std::round(sc.intervals[k])) > 0.01f) return false;
	return true;
}

// ── the oscillator ───────────────────────────────────────────────────────────
// Sawtooth-anchored, because Helmholtz motion IS a sawtooth and morphing toward
// square reads as more bow pressure. Band-limited: six naive saws droning at
// 130 Hz alias audibly, and a drone gives the ear all the time it needs to hear
// it.
static inline float wheelBlep(double t, double dt) {
	if (t < dt)          { t /= dt;            return (float)(t + t - t * t - 1.0); }
	if (t > 1.0 - dt)    { t = (t - 1.0) / dt; return (float)(t * t + t + t + 1.0); }
	return 0.f;
}

// ── a human press ────────────────────────────────────────────────────────────
// A one-pole slew starts at its MAXIMUM velocity, which is the one thing a hand
// cannot do: it has mass. Reaching movements follow a minimum-jerk profile
// (Flash & Hogan, 1985) — zero velocity and zero acceleration at both ends,
// s(t) = 10t³ − 15t⁴ + 6t⁵. That alone is most of the difference between a
// pressed string and a faded one.
//
// Two further things a finger does and an exponential does not:
//
//   * it OVERSHOOTS its resting depth and settles back, and only when it moves
//     fast — a slow deliberate press has no overshoot at all, because overshoot
//     is mass rather than intent. On a bowed string that momentary extra
//     pressure is the bite at the start of the note.
//   * it is never twice the same. A little jitter on the timing and on the
//     overshoot keeps repeated notes from being identical.
//
// Releasing is slower than pressing: lifting off a turning wheel is a release,
// not a stop.
struct WheelPress {
	float pos = 0.f, from = 0.f, to = 0.f, tgt = -1.f;
	float t = 1.f, rate = 40.f;
	bool  settling = false;

	static float minJerk(float x) { return x * x * x * (10.f + x * (-15.f + 6.f * x)); }

	void aim(float target, float dur, bool allowOvershoot) {
		if (std::fabs(target - tgt) < 1e-4f) return;
		tgt = target;
		float j = random::uniform() * 2.f - 1.f;
		bool pressing = target > pos;
		float d = std::max(dur * (pressing ? 1.f : 1.25f) * (1.f + j * 0.12f), 2e-4f);
		float over = (pressing && allowOvershoot)
		           ? 0.22f * std::exp(-d / 0.15f) * (1.f + j * 0.3f) : 0.f;
		from = pos;
		to = target + over * (target - pos);
		settling = over > 1e-3f;
		t = 0.f;
		rate = 1.f / d;
	}

	float process(float dt) {
		if (t < 1.f) {
			t = std::min(1.f, t + dt * rate);
			pos = from + (to - from) * minJerk(t);
			if (t >= 1.f && settling) {          // the finger relaxes onto its depth
				settling = false;
				from = pos; to = tgt; t = 0.f; rate *= 1.6f;
			}
		}
		return pos;
	}
};

struct WheelOsc {
	double phase = 0.0;

	void advance(double dt) { phase += dt; if (phase >= 1.0) phase -= std::floor(phase); }

	// `offset` is the PHASE control: a read offset rather than a reset, so it can
	// be turned while the voice sounds without a discontinuity.
	// shape 0 = sine, 1/3 = triangle, 2/3 = sawtooth, 1 = square.
	float value(float shape, double dt, float offset) const {
		double p = phase + (double)offset;
		p -= std::floor(p);
		float sine = std::sin(2.f * (float)M_PI * (float)p);
		float tri  = 4.f * std::fabs((float)p - 0.5f) - 1.f;
		float saw  = (float)(2.0 * p - 1.0) - wheelBlep(p, dt);
		double q = p + 0.5; q -= std::floor(q);
		float sqr  = (p < 0.5 ? 1.f : -1.f) + wheelBlep(p, dt) - wheelBlep(q, dt);
		float s = clamp(shape, 0.f, 1.f) * 3.f;
		int seg = (int)s; if (seg > 2) seg = 2;
		float f = s - (float)seg;
		const float* tbl[4] = {&sine, &tri, &saw, &sqr};
		return *tbl[seg] * (1.f - f) + *tbl[seg + 1] * f;
	}
};

struct Wheel : Module {
	enum ParamId {
		ROOT_PARAM, SCALE_PARAM, CRANK_PARAM, COUPS_PARAM,
		PRESS_PARAM, RIPPLE_PARAM, DOG_PARAM, TEMPER_PARAM,
		SPREAD_PARAM, DETUNE_PARAM, ROSIN_PARAM, BODY_PARAM, WIDTH_PARAM, SWELL_PARAM,
		DECAY_PARAM,                                  // the dog's, the trompette's alone
		PITCH_PARAM,                                  // x6 — voice 0 is its OCTAVE
		LEVEL_PARAM  = PITCH_PARAM  + WH_V,           // x6
		// Voice 0 has no OCT (its slider is the octave) and no PHASE (it is the
		// reference every other phase is measured from), and the trompette has no
		// PHASE either — the buzz masks it, and its fourth trim is DECAY. Those
		// three slots stay allocated: params serialise by index, and reclaiming
		// one later would scramble every saved patch.
		OCT_PARAM    = LEVEL_PARAM  + WH_V,           // x6
		WAVE_PARAM   = OCT_PARAM    + WH_V,           // x6
		PHASE_PARAM  = WAVE_PARAM   + WH_V,           // x6
		PRESSV_PARAM = PHASE_PARAM  + WH_V,           // x6
		ON_PARAM     = PRESSV_PARAM + WH_V,           // x6
		PARAMS_LEN   = ON_PARAM     + WH_V
	};
	enum InputId {
		ROOT_INPUT, SCALE_INPUT, CRANK_INPUT, COUPS_INPUT,
		PRESS_INPUT, RIPPLE_INPUT, DOG_INPUT, TEMPER_INPUT,
		COUP_INPUT, CLOCK_INPUT,
		// No per-voice 1V/oct. A hurdy-gurdy's drones are set by where the string
		// is tuned and then left there; the degree offset is the authentic
		// control and a pitch CV per voice only invites the instrument to stop
		// being one instrument. ROOT and SCALE move all six together, as a key
		// change does.
		GATE_INPUT,                                   // x6
		WAVE_INPUT   = GATE_INPUT  + WH_V,            // x6
		PHASE_INPUT  = WAVE_INPUT  + WH_V,            // x6 — DECAY on the trompette
		PRESSV_INPUT = PHASE_INPUT + WH_V,            // x6
		// Scale-degree CV, 1V PER DEGREE — not 1V/oct. A degree offset stays in
		// the key whatever the key is, which is the authentic control here and
		// the reason the per-voice 1V/oct was dropped: moving a drone by a
		// semitone is not something a hurdy-gurdy can do, moving it to the next
		// degree of the scale is. On voice 1, which is locked to the root, it
		// steps OCTAVES instead, matching what its own slider does.
		DEG_INPUT    = PRESSV_INPUT + WH_V,           // x6
		LEVELV_INPUT = DEG_INPUT + WH_V,              // x6
		INPUTS_LEN   = LEVELV_INPUT + WH_V
	};
	enum OutputId {
		LEFT_OUTPUT, RIGHT_OUTPUT, POLY_OUTPUT, WHEEL_OUTPUT, COUP_OUTPUT, OUTPUTS_LEN
	};
	enum LightId {
		ON_LIGHT, COUP_LIGHT = ON_LIGHT + WH_V, LIGHTS_LEN
	};

	// ── the wheel ────────────────────────────────────────────────────────────
	double wheelPhase = 0.0;
	float  speed      = 1.6f;     // revolutions per second, as it actually turns
	float  speedBase  = 1.6f;     // ...before the coup impulse
	float  impulse    = 0.f;      // the wrist stroke, decaying
	float  impulseTgt = 0.f;
	float  wander     = 0.f;      // slow rosin/humidity drift
	float  wanderRnd  = 0.f;
	int    wanderCtr  = 0;

	// coups
	int    coups      = 4;
	uint8_t coupMask  = 0xFF;     // which slots of the turn get a stroke
	int    coupSlot   = 0;        // the slot the wheel is in
	float  buzzEnv    = 0.f;
	double rattle     = 0.0;
	float  clickEnv   = 0.f;
	float  dogHold    = 0.f;      // retrigger guard, seconds
	float  dogFlash   = 0.f;      // display only
	// What the dog is comparing, published so the display can SHOW the comparison
	// rather than leaving the player to infer it from what they hear.
	float  dogThresh  = 0.44f;
	float  strokeGain = 1.f;
	// Which slots actually BUZZED, decaying. The ring used to highlight whichever
	// slot the wheel was passing, which lit slots that never fired the dog and
	// made the pattern look like it was sliding round the circle. A slot lights
	// when it buzzes, and only then.
	float  slotFlash[WH_MAXCOUPS] = {};
	dsp::PulseGenerator coupPulse;
	dsp::SchmittTrigger coupTrig, clockTrig;

	// external clock: one revolution per pulse
	float  clockSamples = 0.f;
	float  clockPeriod  = 0.f;    // seconds per revolution, 0 = not measuring

	// ── voices ───────────────────────────────────────────────────────────────
	WheelOsc osc[WH_V];
	float freq[WH_V]    = {};
	WheelPress gateC[WH_V];       // one contour: the gate, normalled to the latch
	float pressSm[WH_V] = {};     // CV smoothing only; the contour carries the shape
	float levelSm[WH_V] = {};
	float lp[WH_V]      = {};     // one-pole tone
	float rippleV[WH_V] = {};     // display
	float voiceAct[WH_V] = {};    // is this string engaged, 0..1 through the swell
	float detuneOf[WH_V]= {};     // fixed per-voice detune shape
	float phaseOf[WH_V] = {};     // fixed per-voice position round the wheel
	float panOf[WH_V]   = {0.f, -0.7f, 0.7f, -1.f, 1.f, 0.f};

	sfs::BusScale scale;
	bool  temperLive = true;      // is TEMPER meaningful in this key?
	// How far TEMPER is ACTUALLY moving anything, in cents. Published because
	// the control is honest but looks broken without it: on a traditional drone
	// voicing — roots, octaves and fifths, which is what a hurdy-gurdy is — equal
	// temperament is already within 2 cents of just, so there is nothing for
	// TEMPER to do and no way to tell that from a control that does not work.
	// It bites on thirds and sixths, by 14 to 16 cents.
	float temperCents = 0.f;
	int   rootSemis  = 0;
	int   refreshCtr = 0;

	sfs::SVF body[3], rosinBP, buzzHP;
	float dcL = 0.f, dcR = 0.f, dcInL = 0.f, dcInR = 0.f;

	// options
	bool envToPress = true;       // ENV presses the string onto the wheel
	float sr = 48000.f;

	Wheel() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam<WheelRootQuantity>(ROOT_PARAM, 0.f, 11.f, 0.f, "Root");
		paramQuantities[ROOT_PARAM]->snapEnabled = true;
		configParam<WheelScaleQuantity>(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 1.f, "Scale");
		paramQuantities[SCALE_PARAM]->snapEnabled = true;
		configParam(CRANK_PARAM, 0.3f, 6.f, 1.6f, "Crank", " rev/s");
		configParam(COUPS_PARAM, 1.f, (float)WH_MAXCOUPS, 4.f, "Coups per turn");
		paramQuantities[COUPS_PARAM]->snapEnabled = true;
		configParam(PRESS_PARAM,  0.f, 1.f, 0.65f, "Wheel pressure", "%", 0.f, 100.f);
		configParam(RIPPLE_PARAM, 0.f, 1.f, 0.45f, "Ripple", "%", 0.f, 100.f);
		configParam(DOG_PARAM,    0.f, 1.f, 0.f, "Dog", "%", 0.f, 100.f);
		configParam(TEMPER_PARAM, 0.f, 1.f, 1.f, "Temper (0 equal, 1 just)", "%", 0.f, 100.f);
		configParam(SPREAD_PARAM, 0.f, 1.f, 0.15f, "Wheel spread", "%", 0.f, 100.f);
		configParam(DETUNE_PARAM, 0.f, 1.f, 0.08f, "Detune", "%", 0.f, 100.f);
		configParam(ROSIN_PARAM,  0.f, 1.f, 0.3f, "Rosin", "%", 0.f, 100.f);
		configParam(BODY_PARAM,   0.f, 1.f, 0.5f, "Body", "%", 0.f, 100.f);
		configParam(WIDTH_PARAM,  0.f, 1.f, 0.6f, "Stereo width", "%", 0.f, 100.f);
		configParam(DECAY_PARAM,  0.f, 1.f, 0.4f, "Buzz decay", "%", 0.f, 100.f);
		// How long a GATE takes to press its string onto the wheel. A hard gate
		// on a bowed string is the one articulation the instrument cannot make:
		// the wheel takes a moment to grip. The number is literal now — the
		// contour completes in exactly this time — where a one-pole's "time" was
		// a 63% constant and 90% took 2.3 times as long. 8ms to 1.2s.
		configParam(SWELL_PARAM, 0.f, 1.f, 0.25f, "Gate press time", " ms", 150.f, 8.f);

		static const char* vname[WH_V] = {"1", "2", "3", "4", "5", "Trompette"};
		for (int v = 0; v < WH_V; v++) {
			bool trp = (v == WH_TRP);
			std::string n = std::string("Voice ") + vname[v] + " ";
			if (v == 0) {
				configParam(PITCH_PARAM, -3.f, 2.f, 0.f, "Voice 1 octave");
				paramQuantities[PITCH_PARAM]->snapEnabled = true;
			}
			else {
				configParam(PITCH_PARAM + v, 0.f, (float)(WH_MAXDEG - 1),
				            v == 1 ? 4.f : v == 2 ? 0.f : v == 3 ? 2.f : 0.f,
				            n + "degree");
				paramQuantities[PITCH_PARAM + v]->snapEnabled = true;
				configParam(OCT_PARAM + v, -3.f, 2.f,
				            v == WH_TRP ? 1.f : (v == 3 ? 1.f : 0.f), n + "octave");
				paramQuantities[OCT_PARAM + v]->snapEnabled = true;
				if (v != WH_TRP)
					configParam(PHASE_PARAM + v, 0.f, 1.f, 0.f, n + "phase", "°", 0.f, 360.f);
			}
			configParam(LEVEL_PARAM + v, 0.f, 1.f, v == WH_TRP ? 0.55f : 0.7f,
			            n + "level", "%", 0.f, 100.f);
			configParam(WAVE_PARAM + v, 0.f, 1.f, trp ? 0.8f : 0.66f, n + "wave");
			configParam(PRESSV_PARAM + v, 0.f, 1.f, 0.75f, n + "pressure", "%", 0.f, 100.f);
			configSwitch(ON_PARAM + v, 0.f, 1.f, 1.f, n + "on the wheel", {"Lifted", "On"});
			configInput(GATE_INPUT + v, n + "gate");
			configInput(WAVE_INPUT + v, n + "wave CV");
			configInput(PHASE_INPUT + v, n + (trp ? "decay CV" : "phase CV"));
			configInput(PRESSV_INPUT + v, n + "pressure CV");
			configInput(DEG_INPUT + v, n + (v == 0 ? "octave CV (1V/octave)"
			                                      : "degree CV (1V per degree)"));
			configInput(LEVELV_INPUT + v, n + "level CV");
		}

		configInput(ROOT_INPUT,  "Root CV (1V/oct)");
		configInput(SCALE_INPUT, "Scale CV (1V per scale)");
		configInput(CRANK_INPUT, "Crank CV");
		configInput(COUPS_INPUT, "Coups CV");
		configInput(PRESS_INPUT, "Pressure CV");
		configInput(RIPPLE_INPUT,"Ripple CV");
		configInput(DOG_INPUT,   "Dog CV");
		configInput(TEMPER_INPUT,"Temper CV");
		configInput(COUP_INPUT,  "Coup trigger");
		configInput(CLOCK_INPUT, "Clock (one revolution per pulse)");

		configOutput(LEFT_OUTPUT,  "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		configOutput(POLY_OUTPUT,  "Voices (6ch poly, 6 = trompette)");
		configOutput(WHEEL_OUTPUT, "Wheel ripple CV");
		configOutput(COUP_OUTPUT,  "Coup trigger");

		// Fixed per-voice shapes: a position round the wheel and a detune
		// direction. Not random per instance -- two Wheels set the same way
		// should sound the same.
		static const float pos[WH_V]  = {0.f, 0.31f, -0.22f, 0.47f, -0.41f, 0.13f};
		static const float det[WH_V]  = {0.f, 0.6f, -0.9f, 1.f, -0.45f, 0.25f};
		for (int v = 0; v < WH_V; v++) { phaseOf[v] = pos[v]; detuneOf[v] = det[v]; }

		onSampleRateChange();
		sfs::busScaleFromIndex(1, scale);
		refreshPitches();
	}

	void onSampleRateChange() override {
		sr = APP->engine->getSampleRate();
		// A lute-back body: three resonances, measured off nothing in particular
		// but sitting where a small wooden box with a soundboard puts them.
		body[0].set(232.f, 4.5f, sr);
		body[1].set(486.f, 5.5f, sr);
		body[2].set(1420.f, 3.2f, sr);
		rosinBP.set(3200.f, 0.9f, sr);
		buzzHP.set(900.f, 0.8f, sr);
	}

	// ── the ripple ───────────────────────────────────────────────────────────
	// A FIXED periodic profile, not noise -- that is the whole point of it. The
	// same shape every revolution: eccentricity as the first harmonic, uneven
	// rosin as the second and third, and a narrow dip where the wheel was joined.
	// The slow wander on top is rosin wearing and humidity, not the ripple.
	float rippleAt(double ph) const {
		double p = ph - std::floor(ph);
		float a = std::sin(2.f * (float)M_PI * (float)p);
		float b = 0.34f * std::sin(4.f * (float)M_PI * ((float)p + 0.18f));
		float c = 0.17f * std::sin(6.f * (float)M_PI * ((float)p + 0.62f));
		// the seam: a narrow loss of grip at one angle, once per turn
		float d = (float)p - WH_SEAM;
		if (d >  0.5f) d -= 1.f;
		if (d < -0.5f) d += 1.f;
		float seam = -0.72f * std::exp(-(d * d) / (2.f * 0.024f * 0.024f));
		// +0.035 takes the mean back to zero. The seam is a one-sided event, so
		// without it RIPPLE quietly turns the whole instrument down as it is
		// raised — a level change wearing a modulation's clothes. Measured with
		// tools/wheel-dog-harness.py mode 2, which prints the mean for exactly
		// this reason. The clamp is wide enough never to flatten the seam: it
		// used to, and a clipped dip is a plateau of silence once per turn.
		return clamp((a + b + c) * 0.62f + seam + 0.035f, -1.6f, 1.6f);
	}

	void refreshPitches() {
		scale = sfs::busResolve(inputs[SCALE_INPUT],
		                        (int)std::round(params[SCALE_PARAM].getValue()));
		temperLive = wheelIs12TET(scale);

		float rs = params[ROOT_PARAM].getValue();
		if (inputs[ROOT_INPUT].isConnected()) rs += inputs[ROOT_INPUT].getVoltage() * 12.f;
		rootSemis = (int)std::round(rs);

		float temper = 0.f;
		if (temperLive) {
			temper = params[TEMPER_PARAM].getValue();
			if (inputs[TEMPER_INPUT].isConnected())
				temper += inputs[TEMPER_INPUT].getVoltage() * 0.2f;
			temper = clamp(temper, 0.f, 1.f);
		}
		float detune = params[DETUNE_PARAM].getValue();
		float maxShift = 0.f;

		for (int v = 0; v < WH_V; v++) {
			float semis;
			float dcv = inputs[DEG_INPUT + v].isConnected()
			          ? std::round(inputs[DEG_INPUT + v].getVoltage()) : 0.f;
			if (v == 0) {
				semis = (std::round(params[PITCH_PARAM].getValue()) + dcv) * scale.period;
			}
			else {
				int deg = (int)std::round(params[PITCH_PARAM + v].getValue() + dcv);
				int oct = (int)std::round(params[OCT_PARAM + v].getValue());
				semis = scale.degree(deg) + (float)oct * scale.period;
			}
			if (temper > 0.f) {
				float shift = (wheelJustify(semis) - semis) * temper;
				semis += shift;
				maxShift = std::max(maxShift, std::fabs(shift));
			}
			semis += detuneOf[v] * detune * 0.18f;          // ±0.18 st at full
			float volts = ((float)rootSemis + semis) / 12.f;
			freq[v] = clamp(WH_BASE_HZ * std::pow(2.f, volts), 8.f, sr * 0.45f);
		}
		temperCents = maxShift * 100.f;
	}

	void process(const ProcessArgs& args) override {
		const float dt = args.sampleTime;
		if (--refreshCtr <= 0) { refreshCtr = 32; refreshPitches(); }

		// ── crank speed ──────────────────────────────────────────────────────
		float crank = params[CRANK_PARAM].getValue();
		if (inputs[CRANK_INPUT].isConnected()) crank += inputs[CRANK_INPUT].getVoltage() * 0.5f;

		// A clock pulse is one revolution. Measured rather than reset, and the
		// phase is PULLED toward zero rather than snapped: a hard reset on every
		// pulse puts a step in the ripple, which is exactly the click the ripple
		// exists to avoid.
		bool clocked = inputs[CLOCK_INPUT].isConnected();
		if (clocked) {
			clockSamples += 1.f;
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				float per = clockSamples * dt;
				if (per > 0.02f && per < 8.f) clockPeriod = per;
				clockSamples = 0.f;
				// pull, do not snap
				double err = wheelPhase - std::floor(wheelPhase);
				if (err > 0.5) err -= 1.0;
				wheelPhase -= err * 0.25;
			}
			if (clockPeriod > 0.f) crank = 1.f / clockPeriod;
		}
		else { clockPeriod = 0.f; clockSamples = 0.f; }

		// slow wander: rosin wearing, humidity. Re-aimed a few times a second and
		// slewed, so it is drift rather than noise.
		if (--wanderCtr <= 0) { wanderCtr = (int)(sr * 0.25f); wanderRnd = random::normal() * 0.05f; }
		wander += (wanderRnd - wander) * dt * 3.f;

		speedBase = clamp(crank * (1.f + wander), 0.05f, 24.f);

		// ── coups par tour ───────────────────────────────────────────────────
		int nc = (int)std::round(params[COUPS_PARAM].getValue()
		                        + (inputs[COUPS_INPUT].isConnected()
		                           ? inputs[COUPS_INPUT].getVoltage() : 0.f));
		coups = clamp(nc, 1, WH_MAXCOUPS);

		double prevPhase = wheelPhase;
		wheelPhase += (double)(speedBase * dt);
		if (wheelPhase >= 1.0) wheelPhase -= std::floor(wheelPhase);

		int slot = (int)((wheelPhase - std::floor(wheelPhase)) * (double)coups) % coups;
		int prevSlot = (int)((prevPhase - std::floor(prevPhase)) * (double)coups) % coups;

		float stroke = 0.f;
		if (slot != prevSlot) {
			coupSlot = slot;
			if (coupMask & (1 << slot))
				stroke = wheelAccent(slot, coups) * (1.f + random::normal() * 0.09f);
		}
		// A patched coup is deliberate, so it is the hardest stroke available and
		// clears any setting of DOG.
		if (coupTrig.process(inputs[COUP_INPUT].getVoltage(), 0.1f, 1.f)) stroke = 1.2f;
		if (stroke > 0.f) impulseTgt = std::max(impulseTgt, stroke);

		// The stroke, as wheel speed: a fast rise and a slower fall. The swell on
		// all five drones comes off this one envelope, because on the instrument
		// it all comes off one wrist.
		if (impulseTgt > impulse) impulse += (impulseTgt - impulse) * dt * 170.f;
		else                      impulse += (impulseTgt - impulse) * dt * 26.f;
		impulseTgt *= std::exp(-dt * 34.f);
		speed = speedBase * (1.f + impulse * 0.55f);

		// ── the dog ──────────────────────────────────────────────────────────
		// The design note says the foot lifts on wheel ACCELERATION, and that is
		// the mechanism — but it is not the number to test. Measured against
		// tools/wheel-dog-harness.py, a threshold on d(speed)/dt was unusable:
		// 60% of the DOG knob was dead, the weak strokes of the turn never fired
		// at any setting, and cranking FASTER silenced the dog altogether,
		// because closely-spaced strokes land on a speed envelope that has not
		// come back down and so accelerate it less. The knob was measuring the
		// envelope's shape and the stroke spacing, not the stroke.
		//
		// Acceleration is what the stroke DOES; the stroke's own strength is what
		// the foot actually meets, and it is the same physical quantity read
		// where it is still clean. Crank speed keeps its part — a faster wheel
		// makes the foot readier, and makes the buzz louder — without being able
		// to take the threshold away with it.
		float dogK = params[DOG_PARAM].getValue();
		if (inputs[DOG_INPUT].isConnected()) dogK += inputs[DOG_INPUT].getVoltage() * 0.1f;
		dogK = clamp(dogK, 0.f, 1.f);
		// The floor sits BELOW the weakest stroke the accent ladder can produce,
		// even after the -9% jitter and at the slowest crank (0.62 x 0.87 x 0.91 =
		// 0.49). It used to sit just above it, so at the default the weak strokes
		// dropped out at random: the pattern on the ring was not the pattern you
		// heard, which reads as erratic rather than as human. DOG now defaults to
		// 0, meaning "play the ring exactly", and thins by accent as it is raised.
		float thresh = 0.44f + 0.68f * dogK;            // how tight the foot is
		strokeGain = 0.85f + 0.15f * clamp(speedBase * 0.5f, 0.f, 2.f);
		dogThresh = thresh;
		float strength = stroke * strokeGain;
		if (dogHold > 0.f) dogHold -= dt;
		if (strength > thresh && dogHold <= 0.f) {
			// Never longer than the gap between strokes. A fixed 20ms swallowed
			// every other coup at eight per turn on a fast crank, where they
			// arrive 21ms apart.
			dogHold = std::min(0.02f, 0.4f / std::max(1.f, (float)coups * speedBase));
			buzzEnv = clamp(0.45f + 0.55f * strength, 0.f, 1.f);
			rattle = 0.0;
			// The foot strikes the soundboard AS the stroke lands. clickEnv was
			// only set where the rattle phase wraps, so the first click arrived a
			// whole rattle period late — 29ms at the slow end — and every coup
			// began with a soft edge instead of a strike. That is most of what
			// made the timing feel unreliable rather than merely varied.
			clickEnv = buzzEnv;
			coupPulse.trigger(1e-3f);
			dogFlash = 1.f;
			slotFlash[clamp(coupSlot, 0, WH_MAXCOUPS - 1)] = 1.f;
		}
		float decayS = 0.04f + 0.11f * params[DECAY_PARAM].getValue();
		buzzEnv  *= std::exp(-dt / decayS);
		clickEnv *= std::exp(-dt / 0.0022f);
		dogFlash *= std::exp(-dt / 0.09f);
		float slotDecay = std::exp(-dt / 0.14f);
		for (int k = 0; k < WH_MAXCOUPS; k++) slotFlash[k] *= slotDecay;

		float rattleHz = 21.f + 38.f * buzzEnv;
		double rPrev = rattle;
		rattle += (double)(rattleHz * dt);
		if (rattle >= 1.0) rattle -= std::floor(rattle);
		if (rattle < rPrev) clickEnv = buzzEnv;
		float rattleGate = (rattle < 0.42) ? 1.f : 0.f;

		// ── the voices ───────────────────────────────────────────────────────
		float gPress  = params[PRESS_PARAM].getValue();
		if (inputs[PRESS_INPUT].isConnected()) gPress += inputs[PRESS_INPUT].getVoltage() * 0.1f;
		gPress = clamp(gPress, 0.f, 1.f);
		float depth   = clamp(params[RIPPLE_PARAM].getValue()
		                      + (inputs[RIPPLE_INPUT].isConnected()
		                         ? inputs[RIPPLE_INPUT].getVoltage() * 0.1f : 0.f), 0.f, 1.f);
		float spread  = params[SPREAD_PARAM].getValue();
		float rosinK  = params[ROSIN_PARAM].getValue();
		float speedN  = clamp(speed / 3.f, 0.f, 2.f);
		float white   = random::normal() * 0.35f;
		float rosin   = rosinBP.bandpass(white);

		float mixL = 0.f, mixR = 0.f;
		float polyV[WH_V];

		// How long the wheel takes to grip under a gate. The SAME curve the
		// tooltip prints: 8ms * 150^swell. An earlier version used a different
		// one in the DSP, so the readout said 28ms while the string took 83.
		float swellDur = 0.008f * std::pow(150.f, params[SWELL_PARAM].getValue());

		for (int v = 0; v < WH_V; v++) {
			// press: the string on the wheel. The GATE input drives THIS rather
			// than a VCA — pressing a string onto a wheel gives an attack timbre,
			// which a VCA cannot — and it is a gate rather than an envelope
			// because on the instrument there IS no envelope: the string is
			// against the wheel or it is not, and the shape of the onset is the
			// wheel's, not the player's. SWELL is that shape.
			// THE GATE IS NORMALLED TO THE BUTTON. Unpatched, the button says
			// whether the string is on the wheel and defaults to on; patched, the
			// gate says, high is on. One signal either way, so one contour — and
			// it takes SWELL in both cases, because putting a string on the wheel
			// is the same act however it was asked for.
			float lat  = params[ON_PARAM + v].getValue() > 0.5f ? 1.f : 0.f;
			float gate = inputs[GATE_INPUT + v].isConnected()
			           ? (inputs[GATE_INPUT + v].getVoltage() >= 1.f ? 1.f : 0.f)
			           : lat;
			gateC[v].aim(gate, swellDur, true);
			float gc = gateC[v].process(dt);
			voiceAct[v] = clamp(gc, 0.f, 1.f);

			// The LED follows the BUTTON, not the gate. The button is a control,
			// and a control that changes what it shows when something is patched
			// over it stops being readable as a control.
			lights[ON_LIGHT + v].setBrightnessSmooth(lat, args.sampleTime);

			float pv = params[PRESSV_PARAM + v].getValue();
			if (inputs[PRESSV_INPUT + v].isConnected())
				pv = clamp(pv + inputs[PRESSV_INPUT + v].getVoltage() * 0.1f, 0.f, 1.f);
			float pressT = pv * (0.3f + 0.7f * gPress);
			float lv = params[LEVEL_PARAM + v].getValue();
			if (inputs[LEVELV_INPUT + v].isConnected())
				lv = clamp(lv + inputs[LEVELV_INPUT + v].getVoltage() * 0.1f, 0.f, 1.f);
			float vcaT   = lv;
			if (envToPress) pressT *= gc; else vcaT *= gc;
			// One pole at 200/s on top, for CV smoothing only — the contour above
			// carries the shape, and a slew here would only round its corners off.
			pressSm[v] += (pressT - pressSm[v]) * dt * 200.f;
			levelSm[v] += (vcaT   - levelSm[v]) * dt * 200.f;

			float r = rippleAt(wheelPhase + (double)(phaseOf[v] * spread));
			rippleV[v] = r;
			float press = pressSm[v];

			float wv = params[WAVE_PARAM + v].getValue();
			if (inputs[WAVE_INPUT + v].isConnected())
				wv = clamp(wv + inputs[WAVE_INPUT + v].getVoltage() * 0.1f, 0.f, 1.f);
			float phOff = 0.f;
			if (v != 0 && v != WH_TRP) {
				phOff = params[PHASE_PARAM + v].getValue();
				if (inputs[PHASE_INPUT + v].isConnected())
					phOff += inputs[PHASE_INPUT + v].getVoltage() * 0.1f;
			}
			osc[v].advance((double)(freq[v] * dt));
			float raw = osc[v].value(wv, (double)(freq[v] * dt), phOff);

			// Tone is PITCH-RELATIVE, never an absolute cutoff. Loom learned this
			// the hard way: an absolute corner that is right for a low string
			// filters a high one below its own second partial.
			//
			// The ripple's hold on brightness is 0.9, not the 0.35 it started at.
			// Brightness is the more audible half of what wheel grip actually
			// varies — a bow pressing harder gets brighter before it gets louder
			// — and at 0.35 the whole thing was a faint tremolo.
			float bright = 0.18f + 0.82f * press;
			bright *= 0.55f + 0.45f * speedN;
			bright *= clamp(1.f + 0.9f * depth * r, 0.05f, 3.f);
			bright += 0.5f * buzzEnv * impulse;                  // the coup swells everything
			float cut = clamp(freq[v] * (1.8f + 34.f * bright), 60.f, sr * 0.45f);
			float a = 1.f - std::exp(-2.f * (float)M_PI * cut * dt);
			lp[v] += (raw - lp[v]) * a;

			// A string barely touching the wheel is not merely quieter: it slips,
			// and gives more rosin than tone. The rosin follows the ripple too —
			// grip is heard as texture before it is heard as level.
			float slip = 1.f - press;
			float tone = lp[v] * (0.25f + 0.75f * press);
			tone += rosin * rosinK * press * (0.10f + 0.55f * slip) * (0.4f + speedN)
			              * clamp(1.f - 0.8f * depth * r, 0.f, 2.f);

			// RIPPLE's grip on level is 1.15, not the 0.55 it started at. At 0.55
			// the DEFAULT setting was a 1.5 dB wobble and the maximum was 3.5 dB,
			// which on a sustained drone is close to nothing; the control had a
			// full range of travel and almost no audible consequence over it.
			float rip = clamp(1.f + depth * r * 1.15f, 0.f, 3.f);
			// The buzz is not subject to the wheel's grip. The coup slots and the
			// seam are BOTH fixed to the wheel, so a given stroke always lands at
			// the same point of the ripple: measured at RIPPLE=1, stroke 3 of four
			// sits at -13dB every single turn. That is not a musical variation,
			// it is one stroke of the pattern permanently missing. And it is not
			// physical either — the rattle is the bridge foot against the
			// soundboard, driven by the crank stroke, not by how well the wheel
			// happens to be gripping at that instant.
			if (v == WH_TRP) rip += (1.f - rip) * buzzEnv;
			float amp = levelSm[v] * rip
			          * std::pow(press + 1e-4f, 0.55f)
			          * (1.f + 0.35f * impulse);         // every voice swells on a coup

			if (v == WH_TRP) {
				// The buzz IS the string being interrupted: the loose foot chops it
				// at the rattle rate. Plus the foot itself striking the soundboard.
				//
				// And it is LOUD. The chien is the accent of the whole instrument,
				// not a texture on one string of six — the first version chopped
				// the trompette correctly and then left it at its drone level,
				// where it was firing perfectly and could not be heard. The surge,
				// the drive and the click are all there to make the coup an event.
				float chop = 1.f - buzzEnv * (1.f - rattleGate);
				tone *= chop;
				tone = std::tanh(tone * (1.f + 4.5f * buzzEnv));
				float click = buzzHP.bandpass(white * clickEnv * 3.2f);
				tone += click * 1.4f;
				amp *= 1.f + 2.2f * buzzEnv;
			}

			float outv = tone * amp * 3.2f;
			polyV[v] = outv;
			float pan = clamp(panOf[v] * params[WIDTH_PARAM].getValue(), -1.f, 1.f);
			float th = (pan + 1.f) * 0.25f * (float)M_PI;
			mixL += outv * std::cos(th);
			mixR += outv * std::sin(th);
		}

		// ── body ─────────────────────────────────────────────────────────────
		float bodyK = params[BODY_PARAM].getValue();
		if (bodyK > 0.f) {
			float mono = (mixL + mixR) * 0.5f;
			float res = body[0].bandpass(mono) * 0.9f
			          + body[1].bandpass(mono) * 0.7f
			          + body[2].bandpass(mono) * 0.45f;
			mixL += res * bodyK * 0.5f;
			mixR += res * bodyK * 0.5f;
		}

		// DC blockers: the wave morph is not symmetric at every setting, and six
		// summed voices carry any offset straight to the output stage.
		float oL = mixL - dcInL + 0.9995f * dcL; dcInL = mixL; dcL = oL;
		float oR = mixR - dcInR + 0.9995f * dcR; dcInR = mixR; dcR = oR;

		outputs[LEFT_OUTPUT ].setVoltage(sfs::softClip(oL));
		outputs[RIGHT_OUTPUT].setVoltage(sfs::softClip(oR));
		outputs[POLY_OUTPUT].setChannels(WH_V);
		for (int v = 0; v < WH_V; v++)
			outputs[POLY_OUTPUT].setVoltage(sfs::softClip(polyV[v]), v);
		outputs[WHEEL_OUTPUT].setVoltage(clamp(rippleAt(wheelPhase) * 5.f, -10.f, 10.f));
		outputs[COUP_OUTPUT].setVoltage(coupPulse.process(dt) ? 10.f : 0.f);
		lights[COUP_LIGHT].setBrightnessSmooth(dogFlash, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "coupMask", json_integer(coupMask));
		json_object_set_new(root, "envToPress", json_boolean(envToPress));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "coupMask")) coupMask = (uint8_t)json_integer_value(j);
		if (json_t* j = json_object_get(root, "envToPress")) envToPress = json_boolean_value(j);
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// The display IS the wheel.
//
// Two rings, both STATIC, because both are things you click: the coup slots on
// the rim, and the six strings just outside it. Only the disc inside them turns
// — spokes, the seam mark, the hub and the crank handle — with a playhead line
// sweeping to the rim. Drawing the clickable marks on the rotating part would
// have made every target a moving one.
// ─────────────────────────────────────────────────────────────────────────────

// The wheel is drawn ON the faceplate — no screen, no dark slab. That inverts
// every colour decision: the display palette is built to glow out of a #1a1a32
// ground and simply disappears on #f0f0f0. These are the panel's own inks.
static const NVGcolor W_LINE = nvgRGB(0xB4, 0xB4, 0xB4);   // structure
static const NVGcolor W_DIM  = nvgRGB(0xD0, 0xD0, 0xD0);   // inactive
static const NVGcolor W_SOFT = nvgRGB(0x7C, 0x7C, 0x7C);   // secondary
static const NVGcolor W_INK  = nvgRGB(0x2E, 0x2E, 0x2E);   // primary text
static const NVGcolor W_LIVE = nvgRGB(0x3C, 0x3C, 0x3C);   // armed, sounding
static const NVGcolor W_HOT  = nvgRGB(0xE8, 0x64, 0x1F);   // a buzz

struct WheelDisplay : OpaqueWidget {
	Wheel* module = nullptr;
	std::shared_ptr<Font> font;

	// Everything the picture needs, gathered in one place so drawPreview() can
	// hand the SAME body a hardcoded scene instead of a second copy of the
	// drawing code. See docs/conventions/browser-preview-pattern.md.
	struct State {
		double ph = 0.18;
		int    coups = 4;
		uint8_t mask = 0xFF;
		float  flash = 0.f, speed = 1.6f;
		float  thresh = 0.62f, gain = 1.f;
		float  slotFlash[WH_MAXCOUPS] = {0.9f, 0.f, 0.35f, 0.f, 0.f, 0.f, 0.f, 0.f};
		bool   on[WH_V]      = {true, true, true, false, true, true};
		float  press[WH_V]   = {0.85f, 0.7f, 0.55f, 0.f, 0.75f, 0.6f};
		float  ripple[WH_V]  = {0.5f, 0.2f, -0.3f, 0.f, 0.4f, -0.1f};
		std::string key = "C MAJ", temper = "JUST";
	};

	// Where the strings sit, as a multiple of the rim. Named because the draw
	// and the hit test both need it and a number written twice drifts.
	static constexpr float STRING_R = 1.26f;

	// ── the mechanics, and the camera ────────────────────────────────────────
	// THE WHEEL'S PLANE IS PERPENDICULAR TO THE STRINGS. Its axle is PARALLEL to
	// them — it is the crank shaft, running up the instrument's long axis to the
	// handle at the tail.
	//
	// This is not a stylistic point, it is the only orientation that makes a
	// sound. Bowing needs the contact surface to move ACROSS the string; the
	// rim's velocity lies in the wheel's plane, so if that plane contained the
	// string direction the rim would be rubbing along the string's length, which
	// is not bowing and would not speak.
	//
	//   X = along the instrument = the strings = the AXLE (+X is toward the tail,
	//       which is nearest the camera and lands on the LEFT of the picture)
	//   Y = up, with the soundboard at YSB
	//   Z = across the instrument = how the strings are spread
	//
	// Only the top of the wheel is drawn, because only the top of the wheel is
	// visible: the rest is inside the body, and it emerges through a slot in the
	// soundboard. That is both what the instrument looks like and what buys the
	// room — showing the whole disc spent most of the picture's height on a part
	// nobody can see, and squeezed the strings into what was left.
	static constexpr float YAW     = 0.8378f;   // 48 degrees
	static constexpr float PITCH   = 0.6632f;   // 38 degrees, looking DOWN on it
	static constexpr float CAMD    = 3.4f;
	static constexpr float CAMF    = 2.38f;
	static constexpr float WHEEL_T = 0.16f;     // thickness, in radii
	static constexpr float YSB     = 0.52f;     // the soundboard, in radii
	static constexpr float ZSPAN   = 0.26f;     // the strings reach +-this
	static constexpr float XNEAR   = 2.2f;      // toward the tail  (screen LEFT)
	static constexpr float XFARE   = 4.2f;      // toward the pegbox (screen RIGHT)

	struct P3 { float x, y, d; };

	static P3 cam(float wx, float wy, float wz) {
		float sy = std::sin(YAW),   cy = std::cos(YAW);
		float sp = std::sin(PITCH), cp = std::cos(PITCH);
		float x1 =  wx * cy + wz * sy;
		float z1 = -wx * sy + wz * cy;
		// + and - this way round is the camera looking DOWN on the wheel; the
		// leading minus on x mirrors it, so the tail sits on the left.
		float y2 =  wy * cp + z1 * sp;
		float z2 = -wy * sp + z1 * cp;
		float zc = std::max(z2 + CAMD, 0.3f);
		float k = CAMF / zc;
		return { -x1 * k, -y2 * k, zc };
	}
	static float arcHalf() { return std::acos(clamp(YSB, -0.99f, 0.99f)); }

	// Framed on the VISIBLE arc, not the whole disc.
	void frame(float& ox, float& oy, float& sc) const {
		float a0 = arcHalf();
		float lo = 1e9f, hi = -1e9f, lx = 1e9f, hx = -1e9f;
		for (int i = 0; i <= 24; i++) {
			float a = -a0 + 2.f * a0 * i / 24.f;
			for (float x : {-WHEEL_T * 0.5f, WHEEL_T * 0.5f}) {
				P3 p = cam(x, std::cos(a), std::sin(a));
				lo = std::min(lo, p.y); hi = std::max(hi, p.y);
				lx = std::min(lx, p.x); hx = std::max(hx, p.x);
			}
		}
		sc = std::min(box.size.x * 0.52f / std::max(hx - lx, 1e-3f),
		              box.size.y * 0.37f / std::max(hi - lo, 1e-3f));
		ox = box.size.x * 0.47f - (lx + hx) * 0.5f * sc;
		oy = box.size.y * 0.55f - (lo + hi) * 0.5f * sc;
	}

	// THE STRINGS ARE NOT EVENLY SPACED. The outer ones sit further apart than
	// the inner ones, as they do on the instrument — the melody strings run close
	// together down the middle and the drones sit out at the edges. An even comb
	// is the one thing that immediately looks synthetic.
	static float stringZ(int v) {
		float t = -1.f + 2.f * (float)v / (float)(WH_V - 1);
		float m = std::pow(std::fabs(t), 1.35f);
		return ZSPAN * (t < 0.f ? -m : m);
	}
	// It rests ON the tread, which is curved, so the outer strings sit a shade
	// lower. And the strings are PARALLEL: no fan is needed or wanted, because
	// the perspective already spreads the near end and converges the far one,
	// which is what the eye expects looking up the instrument from the tail.
	//
	// A LIFTED STRING IS NOT DRAWN LIFTED. It was, and in a perspective view a
	// vertical offset is the one displacement the projection is worst at showing:
	// it competes with the depth the whole picture is built on, so a raised
	// string just looked like a string somewhere else. Off is light grey. One
	// channel, unambiguous, and it survives any camera angle.
	static float stringY(int v) {
		float z = stringZ(v);
		return std::sqrt(std::max(1e-4f, 1.f - z * z));
	}
	static NVGcolor mixc(NVGcolor a, NVGcolor b, float t) {
		t = clamp(t, 0.f, 1.f);
		return nvgRGBAf(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
		                a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
	}
	static float depthInk(float d) { return clamp(1.f - (d - CAMD) * 0.30f, 0.35f, 1.f); }

	void seg3(NVGcontext* vg, float ax, float ay, float az, float bx, float by, float bz,
	          float ox, float oy, float sc, NVGcolor col, float wmm) const {
		P3 a = cam(ax, ay, az), b = cam(bx, by, bz);
		nvgBeginPath(vg);
		nvgMoveTo(vg, ox + a.x * sc, oy + a.y * sc);
		nvgLineTo(vg, ox + b.x * sc, oy + b.y * sc);
		nvgStrokeColor(vg, col);
		nvgStrokeWidth(vg, mm2px(wmm));
		nvgStroke(vg);
	}

	void draw(const DrawArgs& args) override {
		if (!module) { drawPreview(args); OpaqueWidget::draw(args); return; }
		State s;
		s.ph    = module->wheelPhase;
		s.coups = module->coups;
		s.mask  = module->coupMask;
		s.flash = module->dogFlash;
		s.speed  = module->speed;
		s.thresh = module->dogThresh;
		s.gain   = module->strokeGain;
		for (int k = 0; k < WH_MAXCOUPS; k++) s.slotFlash[k] = module->slotFlash[k];
		for (int v = 0; v < WH_V; v++) {
			s.on[v]     = module->params[Wheel::ON_PARAM + v].getValue() > 0.5f;
			s.press[v]  = clamp(module->pressSm[v], 0.f, 1.f);
			s.ripple[v] = module->rippleV[v];
		}
		s.key = std::string(WH_NOTE[((module->rootSemis % 12) + 12) % 12]) + " "
		      + module->scale.label();
		// Says what TEMPER is DOING, not what it is set to. "JUST 0c" on a voicing
		// of octaves and fifths is the truth and is the answer to "why does this
		// knob seem to do nothing".
		s.temper = !module->temperLive ? "SCALE"
		         : module->params[Wheel::TEMPER_PARAM].getValue() < 0.02f ? "EQUAL"
		         : string::f("JUST %.0fc", module->temperCents);
		body(args, s);
		OpaqueWidget::draw(args);
	}
	void drawPreview(const DrawArgs& args) { body(args, State()); }

	void body(const DrawArgs& args, const State& st) {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		float w = box.size.x, h = box.size.y;
		float ox, oy, sc;
		frame(ox, oy, sc);
		float ph = (float)st.ph;

		nvgSave(vg);
		nvgScissor(vg, 0, 0, w, h);          // the strings run off the edges

		// ── readout, along the top ───────────────────────────────────────────
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		float ty = h * 0.085f;
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, W_INK);
		nvgText(vg, mm2px(1.2f), ty, st.key.c_str(), NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, W_SOFT);
		nvgText(vg, w - mm2px(1.2f), ty, st.temper.c_str(), NULL);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, W_SOFT);
		nvgText(vg, w * 0.47f, ty, string::f("%.2f/s", st.speed).c_str(), NULL);

		// ── the soundboard, and the wheel emerging through it ───────────────
		float a0 = arcHalf(), zc0 = std::sin(a0);
		float xn = -WHEEL_T * 0.5f, xf = WHEEL_T * 0.5f;
		// which face is toward us
		bool nearIsN = cam(xn, 1.f, 0.f).d < cam(xf, 1.f, 0.f).d;
		float xNearFace = nearIsN ? xn : xf, xFarFace = nearIsN ? xf : xn;

		// the slot: a rectangle in the soundboard, the wheel standing in it
		auto slotPt = [&](int i, float& px, float& py) {
			const float sx[4] = {-1.f, 1.f, 1.f, -1.f};
			const float sz[4] = {-1.f, -1.f, 1.f, 1.f};
			P3 p = cam(sx[i] * (WHEEL_T * 0.5f + 0.05f), YSB, sz[i] * (zc0 + 0.06f));
			px = ox + p.x * sc; py = oy + p.y * sc;
		};
		nvgBeginPath(vg);
		for (int i = 0; i < 4; i++) {
			float px, py; slotPt(i, px, py);
			if (i == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
		}
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGBA(0x9A, 0x9A, 0xA6, 0x26));
		nvgFill(vg);
		nvgStrokeColor(vg, W_DIM);
		nvgStrokeWidth(vg, mm2px(0.2f));
		nvgStroke(vg);

		// the tread — the band between the two rims, and the surface the strings
		// actually sit on
		nvgBeginPath(vg);
		for (int i = 0; i <= 32; i++) {
			float a2 = -a0 + 2.f * a0 * i / 32.f;
			P3 p = cam(xFarFace, std::cos(a2), std::sin(a2));
			float px = ox + p.x * sc, py = oy + p.y * sc;
			if (i == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
		}
		for (int i = 32; i >= 0; i--) {
			float a2 = -a0 + 2.f * a0 * i / 32.f;
			P3 p = cam(xNearFace, std::cos(a2), std::sin(a2));
			nvgLineTo(vg, ox + p.x * sc, oy + p.y * sc);
		}
		nvgClosePath(vg);
		nvgFillColor(vg, mixc(nvgRGB(0xBE, 0xBE, 0xC6), W_HOT, st.flash * 0.35f));
		nvgFill(vg);

		// the face toward us, filled: the wheel is a solid body, not a cage
		nvgBeginPath(vg);
		for (int i = 0; i <= 32; i++) {
			float a2 = -a0 + 2.f * a0 * i / 32.f;
			P3 p = cam(xNearFace, std::cos(a2), std::sin(a2));
			float px = ox + p.x * sc, py = oy + p.y * sc;
			if (i == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
		}
		{ P3 e = cam(xNearFace, YSB, -zc0); nvgLineTo(vg, ox + e.x * sc, oy + e.y * sc); }
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGB(0xD6, 0xD6, 0xDC));
		nvgFill(vg);
		nvgStrokeColor(vg, W_LINE);
		nvgStrokeWidth(vg, mm2px(0.22f));
		nvgStroke(vg);

		// what turns: radial marks on the face, and the seam across the tread.
		// Each is clipped where it meets the soundboard, so they rise out of the
		// slot and sink back into it.
		for (int k = 0; k < 10; k++) {
			float a2 = 2.f * (float)M_PI * (ph + k / 10.f);
			float ca = std::cos(a2), sa = std::sin(a2);
			if (ca <= YSB) continue;                       // below the soundboard
			float t0 = YSB / ca;
			seg3(vg, xNearFace, ca * t0, sa * t0, xNearFace, ca * 0.97f, sa * 0.97f,
			     ox, oy, sc, W_DIM, 0.18f);
		}
		{
			float a2 = 2.f * (float)M_PI * (ph + WH_SEAM);
			float ca = std::cos(a2), sa = std::sin(a2);
			if (ca > YSB) {
				seg3(vg, xn, ca, sa, xf, ca, sa, ox, oy, sc, W_SOFT, 0.34f);
				float t0 = YSB / ca;
				seg3(vg, xNearFace, ca * t0, sa * t0, xNearFace, ca * 0.97f, sa * 0.97f,
				     ox, oy, sc, W_SOFT, 0.26f);
			}
		}
		// the near lip of the slot, drawn OVER the wheel, so it is emerging from
		// the soundboard rather than sitting on top of it
		{
			float ax2, ay2, bx2, by2;
			slotPt(nearIsN ? 3 : 0, ax2, ay2);
			slotPt(nearIsN ? 2 : 1, bx2, by2);
			nvgBeginPath(vg);
			nvgMoveTo(vg, ax2, ay2); nvgLineTo(vg, bx2, by2);
			nvgStrokeColor(vg, W_SOFT);
			nvgStrokeWidth(vg, mm2px(0.3f));
			nvgStroke(vg);
		}

		// ── the strings ──────────────────────────────────────────────────────
		// Parallel, resting on the tread. ON IS DARK, OFF IS LIGHT GREY — the
		// string stays exactly where it is and only its weight changes. Lifting it
		// instead was the obvious idea and the wrong one: in a perspective view a
		// vertical offset competes with the depth the whole picture is built on,
		// so a raised string read as a string somewhere else rather than as a
		// string that had been taken off the wheel.
		const int SEG = 34;
		float xLo = XNEAR, xHi = -XFARE;             // near (left) to far (right)
		for (int v = 0; v < WH_V; v++) {
			bool on  = st.on[v];
			float pr = st.press[v];
			float lit = on ? clamp(pr * (0.55f + 0.45f * st.ripple[v]), 0.f, 1.f) : 0.f;
			float z = stringZ(v), y = stringY(v);
			NVGcolor col = on ? mixc(W_SOFT, W_INK, 0.35f + 0.65f * lit) : W_DIM;
			if (v == WH_TRP) col = mixc(col, W_HOT, st.flash * 0.8f);
			for (int i = 0; i < SEG; i++) {
				float xa = xLo + (xHi - xLo) * i / SEG;
				float xb = xLo + (xHi - xLo) * (i + 1) / SEG;
				P3 pa = cam(xa, y, z), pb = cam(xb, y, z);
				float sx = ox + pa.x * sc, sy2 = oy + pa.y * sc;
				// The edge fade is the load-bearing half: a fade by distance along
				// the string is symmetric in world space and is not on screen,
				// because the perspective throws one end off the edge while it is
				// still at full strength.
				float m = mm2px(6.f);
				float ae = clamp(std::min(std::min(sx, w - sx), std::min(sy2, h - sy2)) / m,
				                 0.f, 1.f);
				nvgBeginPath(vg);
				nvgMoveTo(vg, sx, sy2);
				nvgLineTo(vg, ox + pb.x * sc, oy + pb.y * sc);
				float dk = on ? depthInk(pa.d) : std::max(depthInk(pa.d), 0.85f);
				nvgStrokeColor(vg, nvgRGBAf(col.r, col.g, col.b, ae * dk));
				nvgStrokeWidth(vg, mm2px(on ? 0.30f + 0.16f * lit : 0.26f));
				nvgStroke(vg);
			}
			if (on) {                                 // where the wheel drives it
				P3 c = cam(0.f, y, z);
				nvgBeginPath(vg);
				nvgCircle(vg, ox + c.x * sc, oy + c.y * sc, mm2px(0.28f + 0.26f * lit));
				nvgFillColor(vg, mixc(W_INK, W_HOT, lit));
				nvgFill(vg);
			}
			if (v == WH_TRP) {                        // one anchor for the order
				P3 e = cam(xLo, y, z);
				sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, on ? W_INK : W_DIM);
				float lx2 = clamp(ox + e.x * sc + mm2px(0.6f), mm2px(0.8f), w - mm2px(9.f));
				float ly2 = clamp(oy + e.y * sc, h * 0.17f, h * 0.80f);
				nvgText(vg, lx2, ly2, "TRP", NULL);
			}
		}

		// ── the strokes: one bar per slot, against the dog's threshold ───────
		// This is the whole coup mechanism in one picture, and it used to be a row
		// of identical ticks that showed none of it.
		//
		// A turn of the wheel is divided into COUPS slots. Each is a wrist stroke,
		// and its STRENGTH is its position in the bar: the downbeat hardest, the
		// half-turn next, and so on down — that is the bar's height. DOG is a
		// threshold on that strength — the line across. A stroke buzzes if its bar
		// clears the line, so raising DOG walks the line up through the bars and
		// takes the weak strokes out one tier at a time. Cranking faster grows
		// every bar, which is why a hard crank makes the dog speak more readily.
		// The whisker on each bar is the stroke-to-stroke jitter: where it
		// straddles the line, that stroke fires some turns and not others.
		//
		// Clicking a bar mutes the slot entirely — that is the pattern; DOG is
		// what thins whatever pattern is left.
		float by = h * 0.965f, bx0 = mm2px(4.f), bx1 = w - mm2px(4.f);
		float unit = mm2px(5.6f) / 1.25f;             // strength -> height
		nvgBeginPath(vg);
		nvgMoveTo(vg, bx0, by); nvgLineTo(vg, bx1, by);
		nvgStrokeColor(vg, W_LINE);
		nvgStrokeWidth(vg, mm2px(0.18f));
		nvgStroke(vg);

		for (int k = 0; k < st.coups; k++) {
			bool on = (st.mask & (1 << k)) != 0;
			float f = clamp(st.slotFlash[k], 0.f, 1.f);
			float x = bx0 + (bx1 - bx0) * ((float)k + 0.5f) / (float)st.coups;
			float str = wheelAccent(k, st.coups) * st.gain;
			float top = by - str * unit;
			if (!on) {                                 // muted: the slot is there,
				nvgBeginPath(vg);                      // it just takes no stroke
				nvgMoveTo(vg, x, by); nvgLineTo(vg, x, by - mm2px(0.9f));
				nvgStrokeColor(vg, W_DIM);
				nvgStrokeWidth(vg, mm2px(0.5f));
				nvgStroke(vg);
				continue;
			}
			// the jitter band — where it crosses the line, the stroke is a maybe
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, by - str * 0.91f * unit);
			nvgLineTo(vg, x, by - str * 1.09f * unit);
			nvgStrokeColor(vg, nvgRGBA(0x7C, 0x7C, 0x7C, 0x50));
			nvgStrokeWidth(vg, mm2px(1.5f));
			nvgStroke(vg);

			nvgBeginPath(vg);
			nvgMoveTo(vg, x, by); nvgLineTo(vg, x, top);
			nvgStrokeColor(vg, mixc(str > st.thresh ? W_LIVE : W_DIM, W_HOT, f));
			nvgStrokeWidth(vg, mm2px(0.85f + 0.5f * f));
			nvgStroke(vg);
		}
		// the threshold. Everything above it speaks.
		float ty2 = by - st.thresh * unit;
		// Counted, not accumulated. A float loop counter drifts and clang-tidy
		// flags it, which is fair: the dash count is an integer quantity.
		nvgBeginPath(vg);
		float pitch = mm2px(1.6f);
		int dashes = (int)((bx1 - bx0) / pitch);
		for (int i = 0; i <= dashes; i++) {
			float dx = bx0 + (float)i * pitch;
			nvgMoveTo(vg, dx, ty2);
			nvgLineTo(vg, std::min(dx + mm2px(0.9f), bx1), ty2);
		}
		nvgStrokeColor(vg, W_HOT);
		nvgStrokeWidth(vg, mm2px(0.22f));
		nvgStroke(vg);
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, W_HOT);
		nvgText(vg, bx1 + mm2px(0.5f), ty2, "DOG", NULL);

		{                                              // where the turn is
			float x = bx0 + (bx1 - bx0) * (ph - std::floor(ph));
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, by + mm2px(0.4f));
			nvgLineTo(vg, x - mm2px(0.6f), by + mm2px(1.4f));
			nvgLineTo(vg, x + mm2px(0.6f), by + mm2px(1.4f));
			nvgClosePath(vg);
			nvgFillColor(vg, W_HOT);
			nvgFill(vg);
		}
		nvgRestore(vg);
	}

	void onButton(const event::Button& e) override {
		if (!module || e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT) {
			OpaqueWidget::onButton(e);
			return;
		}
		float w = box.size.x, h = box.size.y, ox, oy, sc;
		frame(ox, oy, sc);

		// the stroke strip
		float by = h * 0.965f, bx0 = mm2px(4.f), bx1 = w - mm2px(4.f);
		if (e.pos.y > by - mm2px(7.f) && e.pos.y < by + mm2px(2.f)
		    && e.pos.x > bx0 - mm2px(2.f) && e.pos.x < bx1 + mm2px(2.f)) {
			int k = (int)std::floor((e.pos.x - bx0) / (bx1 - bx0) * (float)module->coups);
			if (k >= 0 && k < module->coups) {
				module->coupMask ^= (uint8_t)(1 << k);
				e.consume(this);
				return;
			}
		}
		// a string: nearest one whose drawn line the click is close to
		int best = -1; float bd = mm2px(1.1f);
		for (int v = 0; v < WH_V; v++) {
			float y = stringY(v), z = stringZ(v);
			for (int i = 0; i <= 14; i++) {                // sample along the string
				float x = XNEAR + (-XFARE - XNEAR) * i / 14.f;
				P3 p = cam(x, y, z);
				float d = std::hypot(e.pos.x - (ox + p.x * sc), e.pos.y - (oy + p.y * sc));
				if (d < bd) { bd = d; best = v; }
			}
		}
		if (best >= 0) {
			float on = module->params[Wheel::ON_PARAM + best].getValue();
			module->params[Wheel::ON_PARAM + best].setValue(on > 0.5f ? 0.f : 1.f);
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

struct WheelWidget : ModuleWidget {
	WheelWidget(Wheel* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/wheel.svg")));

		// NO sfs::PanelLabels HERE, DELIBERATELY. res/wheel.svg is the designer's
		// own Figma export and it carries every label as an outlined path. Rack
		// ignores <text> but it renders outlines, so drawing labels at runtime on
		// top of it prints all of them twice, half a millimetre apart, which
		// reads as a blurry panel rather than as an obvious duplicate. The panel
		// is the source of the layout now; these positions were transcribed FROM
		// its guide circles, not the other way round.
		//
		// The guides are colour-coded by what they take:
		//     pink  #FFDFDF, 8.89mm  ->  PJ301MPort        (51)
		//     blue  #C3E8FE, 6.35mm  ->  Trimpot           (32)
		//     green #C3FED5, 6.35mm  ->  VCVLightLatch     (6)
		//     grey stroke, 7.62 x 26.67mm -> VCVSlider     (12)

		WheelDisplay* disp = new WheelDisplay;
		disp->module = module;
		disp->box.pos  = mm2px(Vec(3.81f, 11.f));
		disp->box.size = mm2px(Vec(58.42f, 45.5f));
		addChild(disp);

		// ── left: the instrument as a whole ──────────────────────────────────
		// The eight globals are TRIMPOTS on this panel, not RoundBlackKnobs — the
		// art draws them at 6.35mm, which is a trimpot's guide.
		const float LX[4] = {21.53f, 32.96f, 44.39f, 55.82f};
		const float LT1 = 67.25f, LJ1 = 77.35f, LT2 = 91.40f, LJ2 = 101.60f;
		const float LSIDE = 10.70f, OUTY = 120.60f;
		const float OUTX[5] = {10.13f, 21.53f, 32.96f, 44.39f, 55.82f};

		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(LX[i], LT1)), module,
			         Wheel::ROOT_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(LX[i], LJ1)), module,
			         Wheel::ROOT_INPUT + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(LX[i], LT2)), module,
			         Wheel::PRESS_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(LX[i], LJ2)), module,
			         Wheel::PRESS_INPUT + i));
		}
		// CLOCK above, COUP below — the art's order, which is the reverse of the
		// code's previous layout.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(LSIDE, LT1)), module, Wheel::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(LSIDE, LT2)), module, Wheel::COUP_INPUT));
		for (int i = 0; i < 5; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUTX[i], OUTY)), module,
			          Wheel::LEFT_OUTPUT + i));

		// ── right: six voice columns, three sub-columns each ─────────────────
		// The latch sits on the CENTRE sub-column; everything else pairs on the
		// outer two.
		const float VL0 = 78.74f, VDX = 22.86f, VOFF = 5.08f;
		const float VLATCH = 19.f, VSLIDE = 39.96f;
		const float VT1 = 64.70f, VT2 = 78.70f;
		const float VJ1 = 92.70f, VJ2 = 106.60f, VJ3 = 120.60f;

		for (int v = 0; v < WH_V; v++) {
			float xl = VL0 + VDX * v, xc = xl + VOFF, xr = xl + 2 * VOFF;
			bool trp = (v == WH_TRP);

			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
			         mm2px(Vec(xc, VLATCH)), module, Wheel::ON_PARAM + v, Wheel::ON_LIGHT + v));

			addParam(createParamCentered<VCVSlider>(mm2px(Vec(xl, VSLIDE)), module,
			         Wheel::PITCH_PARAM + v));
			addParam(createParamCentered<VCVSlider>(mm2px(Vec(xr, VSLIDE)), module,
			         Wheel::LEVEL_PARAM + v));

			// The rows, read off the art's own labels (they sit 7.7mm ABOVE the
			// control they name, and there are exactly five label rows for five
			// control rows, which is what settles it — read as "below" the first
			// row would have no control and the last control no label):
			//   trim A   PHASE (DECAY on the trompette)  |  PRESS
			//   trim B   OCTAVE                          |  WAVE
			//   jack A   DEG (OCT on the root)           |  LVL
			//   jack B   PHASE / DECAY                   |  PRESS
			//   jack C   GATE                            |  WAVE
			//
			// Voice 1 is locked to the root: no PHASE (it is the reference every
			// other phase is measured from) and no OCTAVE trim (its fader IS the
			// octave).
			if (trp)
				addParam(createParamCentered<Trimpot>(mm2px(Vec(xl, VT1)), module,
				         Wheel::DECAY_PARAM));
			else if (v != 0)
				addParam(createParamCentered<Trimpot>(mm2px(Vec(xl, VT1)), module,
				         Wheel::PHASE_PARAM + v));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(xr, VT1)), module,
			         Wheel::PRESSV_PARAM + v));

			if (v != 0)
				addParam(createParamCentered<Trimpot>(mm2px(Vec(xl, VT2)), module,
				         Wheel::OCT_PARAM + v));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(xr, VT2)), module,
			         Wheel::WAVE_PARAM + v));

			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xl, VJ1)), module,
			         Wheel::DEG_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xr, VJ1)), module,
			         Wheel::LEVELV_INPUT + v));
			if (v != 0)
				addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xl, VJ2)), module,
				         Wheel::PHASE_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xr, VJ2)), module,
			         Wheel::PRESSV_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xl, VJ3)), module,
			         Wheel::GATE_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xr, VJ3)), module,
			         Wheel::WAVE_INPUT + v));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Wheel* m = dynamic_cast<Wheel*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Gate presses the string", "", &m->envToPress));
		menu->addChild(createMenuLabel("  (off: the gate is a plain VCA)"));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Gate press time"));
		struct Sl : ui::Slider { Sl(Quantity* q) { quantity = q; box.size.x = 200.f; } };
		menu->addChild(new Sl(m->getParamQuantity(Wheel::SWELL_PARAM)));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Wheel"));
		menu->addChild(new Sl(m->getParamQuantity(Wheel::SPREAD_PARAM)));
		menu->addChild(new Sl(m->getParamQuantity(Wheel::DETUNE_PARAM)));
		menu->addChild(new Sl(m->getParamQuantity(Wheel::ROSIN_PARAM)));
		menu->addChild(new Sl(m->getParamQuantity(Wheel::BODY_PARAM)));
		menu->addChild(new Sl(m->getParamQuantity(Wheel::WIDTH_PARAM)));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("All strings on the wheel", "", [m]() {
			for (int v = 0; v < WH_V; v++) m->params[Wheel::ON_PARAM + v].setValue(1.f);
		}));
		menu->addChild(createMenuItem("Every stroke of the turn", "", [m]() {
			m->coupMask = 0xFF;
		}));
	}
};

Model* modelWheel = createModel<Wheel, WheelWidget>("Wheel");
