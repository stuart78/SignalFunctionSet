#include "plugin.hpp"
#include "panel-style.hpp"
#include "dr_wav.h"        // implementation lives in phase.cpp; headers only here
#include "pitchtrack.hpp"  // the detector Band proved, shared
#include <osdialog.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>

// =============================================================================
// Spool -- four short tapes, one transport, and no rewind.
//
// After the Mellotron, and departing from it in the one place that matters. A
// Mellotron's tapes are pulled across the head while a key is held and are
// SPRUNG BACK when it is released, which is why a note can only last eight
// seconds and why every press starts from the same place. Here the tape stays
// where it stopped. Press again and it carries on from there, so a repeated
// stab walks through the loop instead of replaying the same instant, and the
// phrase you get out is a function of how you played rather than of where the
// file happens to begin.
//
// That one change is what makes RESET necessary rather than a convenience: if
// the tape never returns to the start, something has to be able to send it
// there.
//
// PITCH IS VARISPEED, NOT TRANSPOSITION. V/OCT changes how fast the tape runs,
// so it moves the pitch AND the loop's period together, exactly as it does on a
// machine with a capstan. An octave down is twice the loop length. Nothing here
// time-stretches, because a Mellotron cannot.
//
// ONE TRANSPORT, FOUR TAPES. WOW and FLUTTER are shared, not per loop, because
// they are the motor: one capstan, one set of speed errors, imposed on
// everything it drives. Four independent wobbles sound like four machines, and
// the instrument is one machine. (Wheel makes the same argument about its one
// rosined wheel bowing six strings.)
//
// THE RAMP IS A SPEED, NOT A FADE. A tape machine's start is the capstan
// getting up to speed, so the ramp scales the playback RATE, which glides the
// pitch up into place and back down on stop. At 0 it is instant; at 20 ms every
// note has a tiny sag on the way in. It is also what stops the transport
// clicking, because a tape resumed mid-waveform from a standstill is a step.
//
// THE TRANSPORT FOLLOWS THE ENVELOPE, NOT THE GATE. If it stopped when the gate
// fell, a long release would be releasing a FROZEN sample -- a held DC value
// fading out, which is not a tape stopping, it is a click with a slow tail. So
// the tape rolls while the envelope is above zero and only then spins down.
// =============================================================================

static const int SP_N = 4;                  // tapes
static const int SP_POLY = 8;               // playheads per tape
static const float SP_MAXSEC = 5.f;         // "short loops", as specified

// Exponential seconds, printed the way you would say them. A knob whose tooltip
// reads "0.34" tells you where the knob is, which you can already see.
struct LpTimeQuantity : ParamQuantity {
	float lo = 0.001f, hi = 4.f;
	std::string getDisplayValueString() override {
		float t = lo * std::pow(hi / lo, clamp(getValue(), 0.f, 1.f));
		if (t < 0.01f)  return string::f("%.2f ms", t * 1000.f);
		if (t < 1.f)    return string::f("%.0f ms", t * 1000.f);
		return string::f("%.2f s", t);
	}
};

struct Spool : Module {
	enum ParamId {
		ENUMS(LEVEL_PARAM, SP_N),
		ENUMS(REC_PARAM, SP_N),
		ATTACK_PARAM, RELEASE_PARAM, RAMP_PARAM,
		WOW_PARAM, FLUTTER_PARAM, SAT_PARAM,
		ROOT_PARAM, PMODE_PARAM, SAG_PARAM, REWIND_PARAM, DETECT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(GATE_INPUT, SP_N),
		ENUMS(VOCT_INPUT, SP_N),
		ENUMS(RECIN_INPUT, SP_N),
		RESET_INPUT, ROOT_INPUT,
		ATTACK_CV_INPUT, RELEASE_CV_INPUT, RAMP_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(OUT_OUTPUT, SP_N),
		MIX_L_OUTPUT, MIX_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { ENUMS(REC_LIGHT, SP_N), LIGHTS_LEN };

	enum Stage { ST_IDLE, ST_ATT, ST_SUS, ST_REL };

	// ── one playhead ────────────────────────────────────────────────────────
	// A tape can have several heads on it at once. Retriggering used to restart
	// the ONE envelope, which cut the note that was still sounding off at the
	// knee -- the new note did not layer over the old one, it replaced it. Each
	// gate edge now takes a fresh head, and the head it displaces goes on
	// playing to the end of its own release.
	//
	// A head is bound to the GATE CHANNEL that started it, so a polyphonic gate
	// gives a chord on one tape and a mono cable behaves exactly as before.
	struct Head {
		double pos = 0.0;
		float env = 0.f, relFrom = 0.f, ramp = 0.f, declick = 1.f;
		bool gated = false;            // is the key still down
		float voct = 0.f;              // the pitch this head was struck at
		int stage = ST_IDLE;
		int chan = -1;
		uint32_t age = 0;
	};

	struct Tape {
		std::vector<float> buf;
		int   len = 0;                 // samples actually in it
		double pos = 0.0;              // WHERE THE TAPE IS. Survives a stop --
		                               // this is the whole module.
		Head head[SP_POLY];
		int primary = -1;              // whose position the bookmark follows
		// A 1 ms FADE FROM ZERO AFTER EVERY JUMP, per head. The loop seam is a step: the
		// last sample of the tape and the first are unrelated unless the material
		// happens to be perfectly cyclic, and nothing guarantees that. At 1x a
		// five-second tape crosses that seam once every five seconds and the tick
		// hides in the music; at 8x it crosses every 600 ms and it is a rhythm.
		// That is why this only showed up at high speeds -- the discontinuity was
		// always there, it just became frequent enough to hear.
		//
		// The cost is a 1 ms dip at each crossing, which is inaudible against a
		// click that is not. A seam CROSSFADE would avoid even that, but it has
		// to read the tape twice and blend two positions, and it changes what the
		// loop contains; a declick only changes when you hear it.
		bool  recording = false;
		int   recPos = 0;
		std::string name;
		std::string srcPath;           // where it came from, if it came from a file
		// Bumped whenever the audio changes, so a save only rewrites a tape that
		// has actually been re-recorded or reloaded.
		int rev = 0, savedRev = -1;
		// A 256-point peak overview, so the display never scans the buffer.
		float mini[256] = {};
		// WHAT THIS TAPE IS ACTUALLY SOUNDING, measured off the tape itself.
		sfs::PitchTracker det;
		float detHz = 0.f;
		bool  detOk = false;
	};
	Tape tape[SP_N];

	dsp::SchmittTrigger gateTrig[SP_N][SP_POLY], resetTrig[SP_N];
	dsp::BooleanTrigger recBtn[SP_N];
	float wowPh = 0.f, wowPh2 = 0.f, flutPh = 0.f, flutPh2 = 0.f;
	int detCounter = 0;
	float sagEnv = 0.f;
	uint32_t headAge = 0;
	std::atomic<bool> measureReq[SP_N];
	float satLp = 0.f;
	uint32_t rng = 0x9E3779B9u;

	// display
	float dispPos[SP_N][SP_POLY] = {}, dispEnv[SP_N][SP_POLY] = {};

	Spool() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < SP_N; i++) {
			configParam(LEVEL_PARAM + i, 0.f, 1.f, 0.8f,
			            string::f("Tape %d level", i + 1), "%", 0.f, 100.f);
			configSwitch(REC_PARAM + i, 0.f, 1.f, 0.f,
			             string::f("Record into tape %d", i + 1), {"Off", "Recording"});
			configInput(GATE_INPUT + i,
			            string::f("Tape %d gate (poly: a channel each, for chords on one tape)", i + 1));
			configInput(VOCT_INPUT + i,
			            string::f("Tape %d V/oct (varispeed; poly, per gate channel)", i + 1));
			configInput(RECIN_INPUT + i, string::f("Tape %d record in", i + 1));
			configOutput(OUT_OUTPUT + i, string::f("Tape %d", i + 1));
			tape[i].buf.assign((size_t)(SP_MAXSEC * 96000.f), 0.f);
			measureReq[i] = false;
		}
		// Shared, because there is one envelope shape for the instrument the way
		// there is one set of tape heads.
		// Every time control reads as a TIME. "0.25" tells you where the knob is,
		// which the knob already does.
		{
			auto* qa = configParam<LpTimeQuantity>(ATTACK_PARAM, 0.f, 1.f, 0.08f, "Attack");
			qa->lo = 0.001f; qa->hi = 4.f;
			// 290 ms, not 32. At 32 ms a note was gone within a thirty-second of
			// a second of the gate falling, so a mono gate retriggered at any
			// musical rate NEVER had two heads sounding at once and the module
			// looked monophonic however many heads it had. Measured: at 32 ms a
			// retrigger every 200 ms peaks at ONE simultaneous head; at 290 ms it
			// is two, and at 500 ms three. Polyphony you cannot hear is not a
			// feature, it is a default.
			auto* qr = configParam<LpTimeQuantity>(RELEASE_PARAM, 0.f, 1.f, 0.55f, "Release");
			qr->lo = 0.005f; qr->hi = 8.f;
		}
		// Linear, so the built-in unit and multiplier do it: 0 to 60 ms.
		configParam(RAMP_PARAM, 0.f, 1.f, 0.25f, "Transport ramp", " ms", 0.f, 60.f);
		configParam(WOW_PARAM, 0.f, 1.f, 0.18f, "Wow", "%", 0.f, 100.f);
		configParam(FLUTTER_PARAM, 0.f, 1.f, 0.15f, "Flutter", "%", 0.f, 100.f);
		configParam(SAT_PARAM, 0.f, 1.f, 0.3f, "Tape saturation", "%", 0.f, 100.f);
		// THE PLUGIN'S ROOT, not a transpose in semitones. Twelve named notes
		// with C at zero, exactly as Note and Chance declare it, so the knob
		// reads "D#" rather than "+3" and a ROOT CV crossing from Key or Arrange
		// lands on the same note it means everywhere else.
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root note",
			{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		// WHAT V/OCT MEANS. Relative is a tape machine's varispeed: 0V plays the
		// tape at the speed it was recorded, and the CV pushes it either way from
		// there. Absolute makes V/OCT a NOTE: the tape is sped up or slowed until
		// what it is sounding matches what was asked for, which only means
		// anything because the pitch is measured rather than assumed.
		configSwitch(PMODE_PARAM, 0.f, 1.f, 0.f, "V/OCT means",
		             {"Relative (varispeed from natural)", "Absolute (retune to the note)"});
		// MOTOR SAG. On a real Mellotron every held key presses another pinch
		// roller onto the SAME capstan, so more notes means more drag and the
		// motor slows: a big chord goes flat, and how flat depends on how hard
		// you are playing. It is the clearest consequence of one transport
		// driving everything, and it is why a Mellotron chord sounds different
		// from three Mellotron notes.
		//
		// A menu slider rather than an eighth trimpot: the shared row is full,
		// and this is set once for a patch and then left, which is the same
		// argument Chance makes for GATE LEN and Sigma for its touch response.
		configParam(SAG_PARAM, 0.f, 1.f, 0.35f, "Motor sag under load", "%", 0.f, 100.f);
		// The authentic behaviour, offered rather than imposed. A Mellotron's
		// spring drags the tape back the moment the key comes up, which is why
		// every press of a key sounds the same instant of tape. Not rewinding is
		// this module's whole departure from that, so it stays the default.
		configSwitch(REWIND_PARAM, 0.f, 1.f, 0.f, "New note starts",
		             {"Where the tape stopped", "At the beginning"});
		// HOW OFTEN THE TAPE'S PITCH IS MEASURED, and the default is ONCE.
		//
		// A tape of a sung or bowed note has vibrato, drift and human wander in
		// it, and that IS the character -- a Mellotron sounds like itself partly
		// because every note is a separate, imperfect performance. Correcting the
		// pitch continuously flattens exactly that out when it works, and beats
		// against it when it does not: measured, the tracking version updated at
		// 5 Hz, which is BELOW vibrato's 5-7 Hz, so it could not follow vibrato
		// and instead sampled it at an arbitrary phase each time and wandered by
		// however deep the vibrato was.
		//
		// Measured once, the tape has one pitch, absolute mode is a fixed ratio,
		// the performance keeps its life, and there is nothing to step or alias.
		configSwitch(DETECT_PARAM, 0.f, 1.f, 0.f, "Pitch detection",
		             {"Once, when the tape is loaded", "Continuous (tracks, and flattens vibrato)"});
		configInput(ROOT_INPUT, "Root CV (1V/oct, semitone-quantized)");
		configInput(RESET_INPUT, "Reset (poly: one channel per tape; mono rewinds all)");
		// +-5 V covers the knob's whole travel, as Slice's DEPTH does. The
		// envelope is shared by every tape, so these are one CV each and not
		// four: it is one machine's amplifier, not four.
		configInput(ATTACK_CV_INPUT, "Attack CV (+-5V = full range)");
		configInput(RELEASE_CV_INPUT, "Release CV (+-5V = full range)");
		configInput(RAMP_CV_INPUT, "Transport ramp CV (+-5V = full range)");
		configOutput(MIX_L_OUTPUT, "Mix L");
		configOutput(MIX_R_OUTPUT, "Mix R");
	}

	float frand() {
		rng = rng * 1664525u + 1013904223u;
		return (float)(rng >> 8) * (1.f / 8388608.f) - 1.f;
	}
	static float knobTime(float k, float lo, float hi) { return lo * std::pow(hi / lo, k); }

	int maxLen(float sr) const { return (int)std::min((float)tape[0].buf.size(), SP_MAXSEC * sr); }

	void buildMini(Tape& t) {
		for (int m = 0; m < 256; m++) {
			if (t.len <= 0) { t.mini[m] = 0.f; continue; }
			int a = (int)((int64_t)m * t.len / 256);
			int b = (int)((int64_t)(m + 1) * t.len / 256);
			if (b <= a) b = a + 1;
			if (b > t.len) b = t.len;
			float pk = 0.f;
			for (int s = a; s < b; s++) pk = std::max(pk, std::fabs(t.buf[s]));
			t.mini[m] = pk;
		}
	}

	// Hermite rather than linear. Varispeed runs the read pointer at any
	// fractional rate over a two-octave range, and linear interpolation is a
	// lowpass whose corner moves with that rate -- so a tape played slowly comes
	// back duller than the same tape played fast, for no reason a tape machine
	// would recognise.
	float readAt(const Tape& t, double p) const {
		if (t.len <= 0) return 0.f;
		int i1 = (int)p;
		float f = (float)(p - (double)i1);
		int i0 = (i1 - 1 + t.len) % t.len, i2 = (i1 + 1) % t.len, i3 = (i1 + 2) % t.len;
		i1 %= t.len;
		float y0 = t.buf[i0], y1 = t.buf[i1], y2 = t.buf[i2], y3 = t.buf[i3];
		float c0 = y1;
		float c1 = 0.5f * (y2 - y0);
		float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
		float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
		return ((c3 * f + c2) * f + c1) * f + c0;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		int cap = maxLen(sr);

		// CLAMPED BEFORE knobTime, NOT AFTER. knobTime is exponential over the
		// knob position, so a position outside 0..1 does not overshoot the range,
		// it runs off it -- 1.5 on RELEASE is 90 seconds, and a modulated
		// envelope would silently stop coming back.
		float A = knobTime(clamp(params[ATTACK_PARAM].getValue()
		                       + inputs[ATTACK_CV_INPUT].getVoltage() * 0.2f, 0.f, 1.f), 0.001f, 4.f);
		float R = knobTime(clamp(params[RELEASE_PARAM].getValue()
		                       + inputs[RELEASE_CV_INPUT].getVoltage() * 0.2f, 0.f, 1.f), 0.005f, 8.f);
		// 0 to 60 ms. Zero is a legitimate setting and has to be exactly zero,
		// not "very short": a transport that always takes a few milliseconds
		// cannot make a hard cut.
		float rampSec = clamp(params[RAMP_PARAM].getValue()
		                    + inputs[RAMP_CV_INPUT].getVoltage() * 0.2f, 0.f, 1.f) * 0.06f;
		float wowAmt = params[WOW_PARAM].getValue();
		float flutAmt = params[FLUTTER_PARAM].getValue();
		float sat = params[SAT_PARAM].getValue();

		// PER TAPE, ON ONE JACK. getPolyVoltage() hands channel 0 to every reader,
		// so a mono cable still rewinds all four exactly as it did, and a
		// polyphonic one addresses them separately. Four separate jacks would
		// have cost a row the panel does not have, and the trigger has to be per
		// tape as well as the voltage: one shared Schmitt is consumed by whoever
		// reads it first and the other three never see the edge. (Key's TRIG
		// learned this the same way.)
		for (int i = 0; i < SP_N; i++) {
			if (resetTrig[i].process(inputs[RESET_INPUT].getPolyVoltage(i), 0.1f, 1.f)) {
				tape[i].pos = 0.0;
				// Every head on that tape, not just the bookmark: a jump is a
				// discontinuity for whoever is reading, and they all are.
				for (int h = 0; h < SP_POLY; h++) {
					tape[i].head[h].pos = 0.0;
					tape[i].head[h].declick = 0.f;
				}
			}
		}

		bool absolute = params[PMODE_PARAM].getValue() > 0.5f;
		// Read the way Note reads it: the CV is whole semitones, added to the
		// knob and wrapped into one octave, so a key change moves the tapes to
		// the nearest voicing of the new key rather than transposing them away.
		int rootNote = (int)std::round(params[ROOT_PARAM].getValue());
		if (inputs[ROOT_INPUT].isConnected())
			rootNote += (int)std::round(inputs[ROOT_INPUT].getVoltage() * 12.f);
		rootNote = ((rootNote % 12) + 12) % 12;
		float rootSemis = (float)rootNote;

		// ── tracking, only if asked for ─────────────────────────────────────
		// Measured OFF THE TAPE, never off the output: absolute mode changes the
		// rate to correct the pitch, so a detector watching the output would see
		// its own correction and chase its tail. The tape is ground truth.
		//
		// Four times faster than it was (25 ms per tape, 40 Hz) because 5 Hz sat
		// below vibrato and aliased against it, and SMOOTHED, because an
		// unsmoothed estimate is a step in playback rate -- the same class of
		// discontinuity the declick and the sag smoothing exist to remove.
		//
		// It reads at the PRIMARY head rather than at the bookmark. With one
		// playhead those were the same place; with eight they are not, and
		// measuring where nobody is reading was retuning every head by the pitch
		// of a moment none of them was playing.
		if (params[DETECT_PARAM].getValue() > 0.5f) {
			detCounter++;
			int period = std::max(1, (int)(sr * 0.00625f));   // each tape every 25 ms
			if ((detCounter % period) == 0) {
				int which = (detCounter / period) % SP_N;
				Tape& t = tape[which];
				int at = (t.primary >= 0) ? (int)t.head[t.primary].pos : (int)t.pos;
				if (t.len >= sfs::PitchTracker::FFT_N && !t.recording) {
					t.det.analyseSpan(t.buf.data(), t.len, at, sr);
					if (t.det.valid && t.det.f0 > 20.f && t.det.f0 < 4000.f) {
						t.detHz = (t.detOk) ? t.detHz + (t.det.f0 - t.detHz) * 0.25f
						                    : t.det.f0;
						t.detOk = true;
					}
				}
			}
		}

		// ── the motor ───────────────────────────────────────────────────────
		// One capstan for all four tapes. Wow is the slow error of a rotating
		// part, flutter the fast one; two flutter partials that are not
		// harmonically related, or it reads as vibrato rather than as a fault.
		// SCALED TO BE AUDIBLE, which the first numbers were not. +/-1% of speed
		// is 17 cents at full WOW, and 0.35% is 6 cents at full FLUTTER -- real
		// figures for a well-kept machine, and a control whose entire travel is
		// worth a sixth of a semitone has nothing to say. A Mellotron is not a
		// well-kept machine; its wow is the reason it sounds like itself. These
		// run to 4% and 1.6%, which is 68 and 27 cents, and the knobs now cross
		// from "barely there" to "broken" over their range.
		//
		// WOW is TWO slow components, not one. A single sine is a vibrato, and
		// the ear latches onto anything periodic enough to predict; two rates
		// that do not divide each other never quite repeat.
		wowPh  += 1.10f / sr; if (wowPh  >= 1.f) wowPh  -= 1.f;
		wowPh2 += 0.37f / sr; if (wowPh2 >= 1.f) wowPh2 -= 1.f;
		flutPh += 17.3f / sr; if (flutPh >= 1.f) flutPh -= 1.f;
		flutPh2 += 25.7f / sr; if (flutPh2 >= 1.f) flutPh2 -= 1.f;
		float wow = (std::sin(2.f * (float)M_PI * wowPh) * 0.65f
		           + std::sin(2.f * (float)M_PI * wowPh2) * 0.35f) * wowAmt * 0.040f;
		float flut = (std::sin(2.f * (float)M_PI * flutPh) * 0.6f
		            + std::sin(2.f * (float)M_PI * flutPh2) * 0.4f) * flutAmt * 0.016f;
		// ── the motor under load ────────────────────────────────────────────
		// Counted from what is SOUNDING, not from what is gated, so a chord that
		// is still releasing is still dragging on the capstan. One note is the
		// reference and does not sag: what the ear notices is chords going flat
		// against single notes, not the whole instrument sitting low.
		//
		// Smoothed over about 80 ms, because a motor has inertia. Stepping the
		// speed the instant a key lands would be a pitch discontinuity on every
		// note -- the very click the declick exists to prevent, arriving by
		// another route.
		int sounding = 0;
		for (int i = 0; i < SP_N; i++)
			for (int h = 0; h < SP_POLY; h++)
				if (tape[i].head[h].stage != ST_IDLE) sounding++;
		// Eight heads is "leaning on it". Counting against four tapes was right
		// when a tape could only sound once; now one tape can hold a chord, and
		// the load is how many notes are down, not how many tapes are in use.
		float sagTarget = (sounding > 1)
		                ? std::min(1.f, (float)(sounding - 1) / 7.f) : 0.f;
		sagEnv += (sagTarget - sagEnv) * (1.f - std::exp(-1.f / (0.08f * sr)));
		float sag = 1.f - sagEnv * params[SAG_PARAM].getValue() * 0.030f;

		float speedErr = (1.f + wow + flut) * sag;

		float mixL = 0.f, mixR = 0.f;
		int soundingHeads = 0;
		for (int i = 0; i < SP_N; i++) {
			Tape& t = tape[i];

			// ── record ──────────────────────────────────────────────────────
			if (recBtn[i].process(params[REC_PARAM + i].getValue() > 0.5f)) {
				t.recording = true;
				t.recPos = 0;
				t.len = 0;
			} else if (t.recording && params[REC_PARAM + i].getValue() <= 0.5f) {
				t.recording = false;
				t.len = std::max(t.recPos, 1);
				t.pos = 0.0;
				t.name = "recorded";
				t.srcPath.clear();
				t.rev++;
				buildMini(t);
				// Eight FFTs is a spike no audio thread should take, so the
				// measurement is handed to the GUI thread -- the same rule Record
				// follows for writing its WAVs.
				measureReq[i] = true;
			}
			if (t.recording) {
				// Recording runs at nominal speed whatever V/OCT says. A capstan
				// records at the speed it records at; varispeed belongs to
				// playback, and letting it move the write pointer would store a
				// tape that is already the wrong length.
				// 0.1, so ±10 V lands at ±1.0 -- THE SAME FULL SCALE A LOADED WAV
				// USES. At 0.2 a recorded tape sat twice as high as a loaded one:
				// twice as loud through the same LEVEL knob, and drawn at twice
				// the height, which is why a recorded lane spilled over its
				// neighbours on screen.
				if (t.recPos < cap) t.buf[t.recPos++] = inputs[RECIN_INPUT + i].getVoltage() * 0.1f;
				if (t.recPos >= cap) {
					t.recording = false;
					t.len = cap;
					t.pos = 0.0;
					t.srcPath.clear();
					t.rev++;
					params[REC_PARAM + i].setValue(0.f);
					buildMini(t);
					measureReq[i] = true;
				}
			}
			lights[REC_LIGHT + i].setBrightness(t.recording ? 1.f : 0.f);

			// ── gates, one head per note ────────────────────────────────────
			int nch = std::max(1, inputs[GATE_INPUT + i].getChannels());
			nch = std::min(nch, SP_POLY);
			for (int c = 0; c < nch; c++) {
				float gv = inputs[GATE_INPUT + i].getPolyVoltage(c);
				if (gateTrig[i][c].process(gv, 0.1f, 1.f)) {
					// A free head, or the QUIETEST one. Stealing the oldest takes
					// whichever note has been ringing longest, which on a five
					// second tape is as likely to be the loudest thing playing.
					int slot = -1; float worst = 1e9f;
					for (int h = 0; h < SP_POLY; h++) {
						if (t.head[h].stage == ST_IDLE) { slot = h; break; }
						if (t.head[h].env < worst) { worst = t.head[h].env; slot = h; }
					}
					// A STOLEN HEAD KEEPS ITS LEVEL and re-attacks from there, so
					// taking a voice away from a note still sounding is a rise
					// rather than a drop to zero and back. A free head is already
					// at zero. (This line used to test the stage AFTER setting it,
					// so it could never have reset anything -- it read as if it
					// did something it did not.)
					Head& hd = t.head[slot];
					hd.stage = ST_ATT;
					hd.chan = c;
					hd.age = ++headAge;
					hd.ramp = 0.f;
					hd.declick = 0.f;
					hd.gated = true;
					hd.voct = inputs[VOCT_INPUT + i].getPolyVoltage(c);
					// WHERE A NOTE STARTS IS DECIDED HERE, AT NOTE-ON, and it is
					// the only place that decides it. It used to be arranged the
					// other way round -- the head always took the bookmark, and
					// "at the beginning" was implemented by zeroing the bookmark
					// when a note finished releasing. That could not work once
					// several heads were in the air: only the head that happened
					// to hold `primary` ran that line, and `primary` is always
					// the NEWEST head, so the note that finished was almost never
					// the one allowed to rewind. From the keyboard the option
					// simply did nothing, which is what you heard.
					hd.pos = (params[REWIND_PARAM].getValue() > 0.5f) ? 0.0 : t.pos;
					// `primary` now means only "the head the display and the
					// pitch detector should read", which is the newest one.
					t.primary = slot;
				}
				// EACH PLAY COMPLETES ITS OWN ENVELOPE. The gate is a hold, and a
				// head follows the A/R of the gate that started it -- what a new
				// gate must not do is cut the last play off at the knee, and it
				// does not: it takes a head of its own and the one already
				// sounding is left to finish.
				//
				// This was briefly written as "a head ignores its gate until it
				// has been all the way round the tape", which made every trigger,
				// however short, a five second play. That is a different
				// instrument, and not this one.
				if (gv < 1.f) {
					for (int h = 0; h < SP_POLY; h++) {
						Head& hd = t.head[h];
						if (hd.chan != c || hd.stage == ST_IDLE) continue;
						// UNGATED THE MOMENT THE KEY LIFTS. This is what stops the
						// TAPE, and it is separate from what stops the SOUND: the
						// release tail below still sounds, but the tape is no
						// longer running under it.
						hd.gated = false;
						if (hd.stage == ST_ATT || hd.stage == ST_SUS) {
							hd.stage = ST_REL;
							hd.relFrom = std::max(hd.env, 1e-4f);
						}
					}
				}
			}

			// ── the heads ───────────────────────────────────────────────────
			float out = 0.f;
			int liveHere = 0;
			float dens = 0.f;
			// The tape is dragged by the NEWEST head whose key is still down --
			// not by `primary`, which is the newest head of any kind and is there
			// for the display and the detector. With a poly cable the newest note
			// can be let go while an older one is still held, and the tape should
			// keep running for as long as ANY key is down.
			uint32_t dragAge = 0; double dragPos = 0.0; bool dragging = false;
			for (int h = 0; h < SP_POLY; h++) {
				Head& hd = t.head[h];
				if (hd.stage == ST_IDLE && hd.ramp <= 1e-5f) continue;

				switch (hd.stage) {
					case ST_ATT:
						hd.env += args.sampleTime / std::max(A, 1e-4f);
						if (hd.env >= 1.f) { hd.env = 1.f; hd.stage = ST_SUS; }
						break;
					case ST_SUS: hd.env = 1.f; break;
					case ST_REL:
						hd.env -= args.sampleTime * hd.relFrom / std::max(R, 1e-4f);
						if (hd.env <= 0.f) {
							hd.env = 0.f;
							hd.stage = ST_IDLE;
							if (t.primary == h) t.primary = -1;
						}
						break;
					default: hd.env = 0.f; break;
				}
				if (hd.stage != ST_IDLE) liveHere++;

				// The transport rolls while there is still sound to make, and
				// only then spins down, so a long release is a tape still moving
				// rather than a held sample being faded.
				bool rolling = (hd.stage != ST_IDLE);
				float rampStep = (rampSec <= 1e-6f) ? 1.f : args.sampleTime / rampSec;
				hd.ramp += (rolling ? rampStep : -rampStep);
				hd.ramp = clamp(hd.ramp, 0.f, 1.f);

				if (t.len <= 0 || t.recording || hd.ramp <= 1e-5f) continue;

				// EACH HEAD READS AT THE PITCH IT WAS STRUCK AT, and it has to be
				// held on the head rather than read live, or a mono V/OCT + gate
				// pair -- the ordinary way to play this -- moves EVERY head that
				// is still sounding to whatever the last note asked for. The tape
				// then ran at one speed for all of them, which is not several
				// plays, it is one play stacked up.
				//
				// It is refreshed while the KEY IS DOWN, so bending a held note
				// still bends it, and frozen the instant the key lifts, so the
				// lap a head finishes afterwards keeps the pitch you let go at
				// even while the next note is being played at another.
				if (hd.gated) hd.voct = inputs[VOCT_INPUT + i].getPolyVoltage(std::max(hd.chan, 0));
				float want = hd.voct + rootSemis / 12.f;
				float base;
				if (absolute && t.detOk) {
					// Speed the tape until what it is sounding IS the note asked
					// for. C4 at 0V, as everywhere else in Rack.
					base = (dsp::FREQ_C4 * std::pow(2.f, want)) / t.detHz;
				} else {
					// Relative, and also the fallback when nothing was detected:
					// a percussive or noisy tape has no pitch to correct to, and
					// guessing one would send the transport somewhere arbitrary.
					base = std::pow(2.f, want);
				}
				base = clamp(base, 0.05f, 20.f);
				float rate = base * speedErr * hd.ramp;

				hd.declick += args.sampleTime / 0.001f;      // 1 ms
				if (hd.declick > 1.f) hd.declick = 1.f;
				// THE RAMP GATES THE LEVEL AS WELL AS THE SPEED. It scales the
				// rate, so a tape starting from rest reads at almost zero speed
				// for the first milliseconds -- the same sample over and over,
				// which is DC, and it bubbles. A capstan not yet up to speed also
				// has poor head contact, so following it with the level is both
				// the fix and the more honest picture.
				out += readAt(t, hd.pos) * hd.env * hd.declick * hd.ramp;
				dens += hd.env * hd.ramp;

				hd.pos += (double)rate;
				bool wrapped = false;
				while (hd.pos >= (double)t.len) { hd.pos -= (double)t.len; wrapped = true; }
				while (hd.pos < 0.0)            { hd.pos += (double)t.len; wrapped = true; }
				if (wrapped) hd.declick = 0.f;

				// THE BOOKMARK FOLLOWS THE TAPE, AND THE TAPE ONLY MOVES WHILE A
				// KEY IS DOWN. That distinction is the whole of the offset
				// between one play and the next, and it is what one-full-pass
				// broke: the bookmark used to follow the sounding head, which was
				// right while a play ENDED where the gate fell, and became wrong
				// the moment a play always ran a whole lap -- a lap ends exactly
				// where it began, at any speed, so the bookmark came back to the
				// same instant of tape every single time and every new head
				// entered on top of the last one in perfect unison. Eight heads
				// playing one sound is not eight plays, which is what you heard.
				//
				// Held for 200 ms, the tape moves 200 ms and the next note enters
				// 200 ms further on. The lap the head finishes afterwards is a
				// tail, like a release, and the tape is not still running for it.
				if (hd.gated && hd.age >= dragAge) { dragAge = hd.age; dragPos = hd.pos; dragging = true; }
				dispPos[i][h] = (float)(hd.pos / (double)t.len);
				dispEnv[i][h] = hd.env;
			}
			if (dragging) t.pos = dragPos;
			for (int h = 0; h < SP_POLY; h++)
				if (t.head[h].stage == ST_IDLE && t.head[h].ramp <= 1e-5f) dispEnv[i][h] = 0.f;
			soundingHeads += liveHere;
			(void)soundingHeads;

			// DIVIDED BY THE DENSITY. Now that a pass always completes, heads
			// pile up: measured, a trigger every 200 ms on a five second tape
			// fills all eight, and the summed envelope reaches EIGHT times one
			// head. Summed raw that is a wall against the saturator.
			//
			// The density is the SUM OF THE ENVELOPES, not the head COUNT, for
			// two reasons. A count steps the moment a head appears or dies, and
			// a step in gain across a sounding lane is a click -- 8 heads to 7
			// is 0.6 dB in one sample. And a head that has only just been
			// allocated counts as a whole head while contributing nothing, so a
			// new note would duck everything already sounding before it could be
			// heard. Summed envelopes grow in with the attack and fade out with
			// the release, so the correction moves exactly as the level does.
			//
			// sqrt, not N: two heads at different points of the tape play
			// different audio and add as power. Two triggered close together are
			// nearly the same audio and do add to double, so this under-corrects
			// in that one case -- which is the case where you asked for it.
			if (dens > 1.f) out /= std::sqrt(dens);
			out *= params[LEVEL_PARAM + i].getValue() * 5.f;
			outputs[OUT_OUTPUT + i].setVoltage(out);
			// Four tapes across the field, so a chord opens out rather than
			// arriving in one place.
			float pan = ((float)i / (float)(SP_N - 1) * 2.f - 1.f) * 0.6f;
			float th = (pan + 1.f) * (float)M_PI_4;
			mixL += out * std::cos(th);
			mixR += out * std::sin(th);
		}

		// ── tape saturation ─────────────────────────────────────────────────
		// tanh for the curve, and a lowpass that CLOSES as it is driven: tape
		// loses top end when it is hit hard, and saturation without that reads
		// as distortion rather than as tape. Normalised by tanh(drive) so the
		// knob does not double as a volume control.
		if (sat > 1e-4f) {
			float drive = 1.f + sat * 6.f;
			float nrm = 1.f / std::tanh(drive);
			mixL = std::tanh(mixL * 0.2f * drive) * nrm * 5.f;
			mixR = std::tanh(mixR * 0.2f * drive) * nrm * 5.f;
			float fc = 18000.f - 12000.f * sat;
			float a = clamp(1.f - std::exp(-2.f * (float)M_PI * fc / sr), 0.f, 1.f);
			satLp += (mixL - satLp) * a;
			mixL = satLp;
		}
		outputs[MIX_L_OUTPUT].setVoltage(clamp(mixL, -10.f, 10.f));
		outputs[MIX_R_OUTPUT].setVoltage(clamp(mixR, -10.f, 10.f));
	}

	// ── the tape's own pitch, measured once ─────────────────────────────────
	// A MEDIAN OF EIGHT WINDOWS SPREAD ALONG THE TAPE, not one reading. A single
	// window can land on a breath, a consonant, a bow change or a gap, and be
	// confidently wrong; the median throws those away without needing to know
	// what they were. Windows that find nothing are dropped rather than counted
	// as zero, which would drag the answer down in exactly the cases where the
	// tape is hardest to read.
	void measureTape(int i) {
		Tape& t = tape[i];
		t.detOk = false;
		if (t.len < sfs::PitchTracker::FFT_N) return;
		float sr = APP->engine->getSampleRate();
		std::vector<float> found;
		for (int k = 0; k < 8; k++) {
			int at = (int)((int64_t)k * t.len / 8);
			t.det.analyseSpan(t.buf.data(), t.len, at, sr);
			if (t.det.valid && t.det.f0 > 20.f && t.det.f0 < 4000.f)
				found.push_back(t.det.f0);
		}
		if (found.size() < 3) return;      // too few honest readings to trust one
		std::sort(found.begin(), found.end());
		t.detHz = found[found.size() / 2];
		t.detOk = true;
	}

	// ── persistence ─────────────────────────────────────────────────────────
	// A path is not enough. Half the tapes here are RECORDED, and a recording has
	// no file behind it -- saving only paths would mean anything you played into
	// the module vanished when Rack closed, which is what was happening. So the
	// audio itself is written into the patch's own storage directory, which Rack
	// carries with the patch, copies on Save As and deletes with the module.
	//
	// Sixteen bit, mono: a five-second tape is 480 kB, where float would be a
	// megabyte each and four of those is a patch nobody wants to email.
	std::string tapeFile(int i) {
		std::string dir = createPatchStorageDirectory();
		if (dir.empty()) return "";
		return system::join(dir, string::f("tape%d.wav", i + 1));
	}

	void writeTape(int i) {
		Tape& t = tape[i];
		if (t.len <= 0) return;
		std::string path = tapeFile(i);
		if (path.empty()) return;
		drwav_data_format fmt;
		fmt.container = drwav_container_riff;
		fmt.format = DR_WAVE_FORMAT_PCM;
		fmt.channels = 1;
		fmt.sampleRate = (drwav_uint32)APP->engine->getSampleRate();
		fmt.bitsPerSample = 16;
		drwav w;
		if (!drwav_init_file_write(&w, path.c_str(), &fmt, NULL)) return;
		std::vector<int16_t> pcm((size_t)t.len);
		for (int n = 0; n < t.len; n++)
			pcm[n] = (int16_t)clamp(t.buf[n] * 32767.f, -32768.f, 32767.f);
		drwav_write_pcm_frames(&w, (drwav_uint64)t.len, pcm.data());
		drwav_uninit(&w);
		t.savedRev = t.rev;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		for (int i = 0; i < SP_N; i++) {
			Tape& t = tape[i];
			// Only a tape that has actually changed is rewritten. dataToJson runs
			// on more than a deliberate save -- duplicating a module calls it too
			// -- and four megabytes of WAV per call would be felt.
			if (t.len > 0 && t.rev != t.savedRev) writeTape(i);
			json_t* o = json_object();
			json_object_set_new(o, "len", json_integer(t.len));
			// WHERE THE TAPE IS STANDING. The one piece of state this module is
			// actually about, so it survives a save as much as the audio does.
			json_object_set_new(o, "pos", json_real(t.pos));
			json_object_set_new(o, "name", json_string(t.name.c_str()));
			json_object_set_new(o, "srcPath", json_string(t.srcPath.c_str()));
			json_array_append_new(arr, o);
		}
		json_object_set_new(root, "tapes", arr);
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* arr = json_object_get(root, "tapes");
		if (!arr) return;
		std::string dir = getPatchStorageDirectory();
		for (int i = 0; i < SP_N && i < (int)json_array_size(arr); i++) {
			json_t* o = json_array_get(arr, i);
			if (!o) continue;
			Tape& t = tape[i];
			std::string wav = dir.empty() ? "" : system::join(dir, string::f("tape%d.wav", i + 1));
			if (!wav.empty() && system::isFile(wav)) {
				loadTape(i, wav);                       // the audio as it was
			} else if (json_t* j = json_object_get(o, "srcPath")) {
				// Fall back to the original file. A patch that was hand-edited or
				// moved without its storage folder still comes back if the source
				// is where it was.
				std::string sp = json_string_value(j);
				if (!sp.empty() && system::isFile(sp)) loadTape(i, sp);
			}
			if (json_t* j = json_object_get(o, "name")) t.name = json_string_value(j);
			if (json_t* j = json_object_get(o, "srcPath")) t.srcPath = json_string_value(j);
			// Restored AFTER the load, because loadTape() rewinds to zero.
			if (json_t* j = json_object_get(o, "pos")) {
				double p = json_real_value(j);
				double hi = (double)t.len - 1.0;
				if (t.len > 0) t.pos = (p < 0.0) ? 0.0 : (p > hi ? hi : p);
			}
			t.savedRev = t.rev;
		}
	}

	void loadTape(int i, const std::string& path) {
		drwav wav;
		if (!drwav_init_file(&wav, path.c_str(), NULL)) return;
		size_t frames = wav.totalPCMFrameCount;
		uint32_t ch = wav.channels;
		if (frames == 0 || ch == 0) { drwav_uninit(&wav); return; }
		std::vector<float> raw(frames * ch);
		drwav_read_pcm_frames_f32(&wav, frames, raw.data());
		uint32_t fileSr = wav.sampleRate;
		drwav_uninit(&wav);

		float sr = APP->engine->getSampleRate();
		double ratio = (double)sr / (double)fileSr;
		int cap = maxLen(sr);
		int outN = (int)std::min((double)cap, (double)frames * ratio);
		Tape& t = tape[i];
		for (int n = 0; n < outN; n++) {
			double src = (double)n / ratio;
			size_t s0 = (size_t)src;
			float f = (float)(src - (double)s0);
			size_t s1 = std::min(s0 + 1, frames - 1);
			float a = 0.f, b = 0.f;
			for (uint32_t c = 0; c < ch; c++) { a += raw[s0 * ch + c]; b += raw[s1 * ch + c]; }
			t.buf[n] = (a + (b - a) * f) / (float)ch;
		}
		t.len = std::max(outN, 1);
		t.pos = 0.0;
		t.name = system::getStem(path);
		t.srcPath = path;
		t.rev++;
		buildMini(t);
		measureTape(i);
	}
};

// =============================================================================
// Display -- four lanes, one per tape, each showing where its tape is standing.
// =============================================================================
struct SpoolDisplay : Widget {
	Spool* module = nullptr;
	std::shared_ptr<Font> font;

	// THE FOUR TAPES ARE LETTERED AND COLOURED ON THE PANEL, so the screen's
	// four lanes carry the same four colours: without that the panel names A, B,
	// C and D and the screen shows four identical blue rows, and nothing joins
	// them up. Read out of res/spool.svg rather than chosen here -- these ARE the
	// marks under the screen. Two of them are already the display palette's blue
	// and orange, which is why the playhead below stops being orange.
	static const NVGcolor* laneColour() {
		static const NVGcolor C[SP_N] = {
			nvgRGB(0x00, 0x97, 0xDE),   // A
			nvgRGB(0x3F, 0xBF, 0x6F),   // B
			nvgRGB(0xEC, 0x65, 0x2E),   // C
			nvgRGB(0x9B, 0x6B, 0xD6),   // D
		};
		return C;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, sfs::SCREEN_BG);
		nvgFill(vg);

		float laneH = box.size.y / (float)SP_N;
		for (int i = 0; i < SP_N; i++) {
			float y0 = i * laneH, mid = y0 + laneH * 0.5f;
			float x0 = 4.f, w = box.size.x - 8.f;

			const NVGcolor lc = laneColour()[i];

			nvgBeginPath(vg);
			nvgRect(vg, x0, mid - 0.5f, w, 1.f);
			nvgFillColor(vg, nvgTransRGBA(lc, 70));
			nvgFill(vg);

			bool live = module && module->tape[i].len > 0;
			// The waveform as a filled envelope, the way Phase and Slice draw
			// one: a column of separate bars reads as a bar chart.
			nvgBeginPath(vg);
			nvgMoveTo(vg, x0, mid);
			for (int m = 0; m < 256; m++) {
				float pk = live ? clamp(module->tape[i].mini[m], 0.f, 1.f)
				                : 0.35f * std::fabs(std::sin(m * 0.05f + i));
				nvgLineTo(vg, x0 + w * (float)m / 255.f, mid - pk * laneH * 0.42f);
			}
			for (int m = 255; m >= 0; m--) {
				// CLAMPED, so a hot tape can never draw outside its own lane. A
				// buffer is not guaranteed to sit inside +/-1: anything recorded
				// above full scale used to spill over its neighbours and the
				// four lanes stopped being four lanes.
				float pk = live ? clamp(module->tape[i].mini[m], 0.f, 1.f)
				                : 0.35f * std::fabs(std::sin(m * 0.05f + i));
				nvgLineTo(vg, x0 + w * (float)m / 255.f, mid + pk * laneH * 0.42f);
			}
			nvgClosePath(vg);
			nvgFillColor(vg, live ? nvgTransRGBA(lc, 0xE0)
			                      : nvgRGBA(0x35, 0x35, 0x4D, 0xB0));
			nvgFill(vg);

			// WHERE THE TAPE IS STANDING. The point of the module is that this
			// does not go back to the left edge when the gate falls, so it is
			// drawn whether the tape is rolling or not -- a playhead that only
			// appeared while sounding would hide the one thing worth seeing.
			if (module) {
				// A MARK PER HEAD. Drawing only one would say the tape has one
				// place, which stopped being true the moment a lane could hold a
				// chord -- and which head it drew would be arbitrary.
				bool any = false;
				for (int h = 0; h < SP_POLY; h++) {
					float e = module->dispEnv[i][h];
					if (e <= 0.005f) continue;
					any = true;
					float px = x0 + w * clamp(module->dispPos[i][h], 0.f, 1.f);
					nvgBeginPath(vg);
					nvgRect(vg, px - 0.5f, y0 + 2.f, 1.f, laneH - 4.f);
					// WHITE, NOT SCREEN_HOT. Tape C's colour is exactly
					// SCREEN_HOT, so an orange playhead vanished into its own
					// waveform on one lane in four. A mark that has to read over
					// any of the four cannot be one of them.
					nvgFillColor(vg, nvgRGBA(0xFF, 0xFF, 0xFF,
					                         (int)(90 + 165 * clamp(e, 0.f, 1.f))));
					nvgFill(vg);
				}
				// The bookmark: where the NEXT note will start. Dim, and always
				// drawn, because a tape standing still is the thing this module
				// is about and a silent lane would otherwise show nothing at all.
				if (!any && module->tape[i].len > 0) {
					// It has to answer the same question the note-on does, or it
					// is drawing a fact about a variable rather than about the
					// instrument: with "at the beginning" set, the bookmark still
					// holds the last stopping point but nothing will ever start
					// there.
					double bm = (module->params[Spool::REWIND_PARAM].getValue() > 0.5f)
					          ? 0.0 : module->tape[i].pos;
					float px = x0 + w * clamp((float)(bm
					                        / (double)module->tape[i].len), 0.f, 1.f);
					nvgBeginPath(vg);
					nvgRect(vg, px - 0.5f, y0 + 2.f, 1.f, laneH - 4.f);
					nvgFillColor(vg, nvgRGBA(0xFF, 0xFF, 0xFF, 80));
					nvgFill(vg);
				}
				if (module->tape[i].recording) {
					nvgBeginPath(vg);
					nvgCircle(vg, x0 + w - 5.f, y0 + 5.f, 2.4f);
					nvgFillColor(vg, sfs::SCREEN_HOT);
					nvgFill(vg);
				}
			}
			if (font && font->handle >= 0) {
				sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
				// The letter in the lane's own colour, the name after it in
				// grey: the panel calls these tapes A-D, so the screen must not
				// call them 1-4.
				nvgFillColor(vg, lc);
				float lx = nvgText(vg, x0 + 2.f, y0 + 2.f,
				                   string::f("%c", 'A' + i).c_str(), NULL);
				std::string nm = module ? module->tape[i].name : "";
				if (!nm.empty()) {
					nvgFillColor(vg, sfs::SCREEN_DIM);
					nvgText(vg, lx + 4.f, y0 + 2.f, nm.c_str(), NULL);
				}
				// The detected pitch, as a note AND a frequency. A number alone
				// does not tell you whether the tape is in tune with the patch,
				// which is the question absolute mode exists to answer.
				if (module) {
					nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
					bool abs_ = module->params[Spool::PMODE_PARAM].getValue() > 0.5f;
					if (module->tape[i].detOk) {
						float hz = module->tape[i].detHz;
						float semi = 12.f * std::log2(hz / dsp::FREQ_C4);
						static const char* NN[12] = {"C","C#","D","D#","E","F",
						                             "F#","G","G#","A","A#","B"};
						int n = (int)std::round(semi);
						int pc = ((n % 12) + 12) % 12;
						int oct = 4 + (int)std::floor((float)n / 12.f);
						float cents = (semi - (float)n) * 100.f;
						nvgFillColor(vg, abs_ ? sfs::SCREEN_BLUE : sfs::SCREEN_DIM);
						nvgText(vg, x0 + w - 2.f, y0 + 2.f,
						        string::f("%s%d %+.0fc  %.0f Hz", NN[pc], oct, cents, hz).c_str(), NULL);
					} else if (abs_ && module->tape[i].len > 0) {
						// Absolute mode with nothing to tune TO is the one state
						// worth calling out, because the tape quietly falls back
						// to varispeed and would otherwise look broken.
						nvgFillColor(vg, sfs::SCREEN_HOT);
						nvgText(vg, x0 + w - 2.f, y0 + 2.f, "no pitch", NULL);
					}
				}
			}
		}
	}
};

// =============================================================================
// Widget -- 22HP
// =============================================================================
// EVERY NUMBER HERE IS READ OUT OF res/spool.svg, not chosen. The art is the
// specification: the designer's guide circles are the control centres, and the
// widget's job is to agree with them.
//
// Four tape blocks of two columns each, then two global rows under a rule.
static const float SP_SUB[8] = {12.06f, 23.49f, 37.46f, 48.89f,
                                63.02f, 74.45f, 88.25f, 99.68f};
// The global trim row has its own spacing -- SAT sits out at 76.79 rather than
// on a tape column, so it cannot share SP_SUB.
// ROOT  ABS  ATTACK  RELEASE  RAMP  SAT  WOW  FLUTTER
static const float SP_GLOB[8] = {12.03f, 23.45f, 37.43f, 48.86f,
                                 60.28f, 76.79f, 88.22f, 99.65f};
// ROOT  RESET  ATK CV  REL CV  RAMP CV  MIX L  MIX R
static const float SP_FOOT[7] = {12.06f, 23.49f, 37.46f, 48.89f,
                                 60.31f, 89.52f, 100.95f};
static const float SP_Y_PLAY = 61.58f;   // GATE / V/OCT
static const float SP_Y_IO   = 76.82f;   // IN / OUT
static const float SP_Y_SET  = 90.76f;   // REC / LEVEL
static const float SP_Y_GLOB = 107.26f;
static const float SP_Y_FOOT = 121.26f;
// The mix pair sits 1.27mm HIGHER than the rest of the foot row, because it is
// on a plate and the plate lifts it. Averaging the row into one number put all
// seven jacks 0.36-0.91mm off their guides.
static const float SP_Y_MIX  = 119.99f;

struct SpoolParamSlider : ui::Slider {
	SpoolParamSlider(Module* m, int paramId) {
		quantity = m->paramQuantities[paramId];
		box.size.x = 200.f;
	}
};

struct SpoolWidget : ModuleWidget {
	Spool* mod = nullptr;

	// Measuring a freshly recorded tape happens HERE, not in process(): eight
	// FFTs in one sample is a dropout, and Record and Play both defer their
	// heavy work to the GUI thread for the same reason.
	void step() override {
		if (mod) {
			for (int i = 0; i < SP_N; i++)
				if (mod->measureReq[i].exchange(false)) mod->measureTape(i);
		}
		ModuleWidget::step();
	}

	SpoolWidget(Spool* module) {
		mod = module;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/spool.svg")));

		// NO sfs::PanelLabels HERE, DELIBERATELY. res/spool.svg is the
		// designer's Figma export and carries its own labels as OUTLINED PATHS
		// (33 of them), which Rack does render -- the half-remembered rule that
		// "Rack ignores SVG text" is only true of <text> elements. Adding the
		// runtime labels back prints every one of them twice, half a millimetre
		// apart, which reads as a blurry panel rather than as an obvious fault.
		// Place components only.

		SpoolDisplay* disp = new SpoolDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(5.08f, 10.16f));
		disp->box.size = mm2px(Vec(101.58f, 35.55f));
		addChild(disp);

		// The global trim row, in the ART'S order: ROOT, [ABS switch], ATTACK,
		// RELEASE, RAMP, SAT, WOW, FLUTTER. Slot 1 is the switch and is placed
		// below.
		addParam(createParamCentered<Trimpot>(mm2px(Vec(SP_GLOB[0], SP_Y_GLOB)), module, Spool::ROOT_PARAM));
		const int shP[6] = {Spool::ATTACK_PARAM, Spool::RELEASE_PARAM, Spool::RAMP_PARAM,
		                    Spool::SAT_PARAM, Spool::WOW_PARAM, Spool::FLUTTER_PARAM};
		for (int k = 0; k < 6; k++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(SP_GLOB[2 + k], SP_Y_GLOB)), module, shP[k]));

		// Per tape, top to bottom: what you PLAY it with, what goes in and what
		// comes out, then how it is set up. Each tape's OUT sits on its own dark
		// plate in the art rather than the outputs sharing one, because a tape's
		// column is kept intact and a shared plate would cover four inputs too.
		// REC is the LEFT of the pair and LEVEL the right, which is the reverse
		// of the pre-2026-09 panel -- the art joins them with a line, so they
		// read as one setup block.
		for (int i = 0; i < SP_N; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SP_SUB[2 * i], SP_Y_PLAY)), module, Spool::GATE_INPUT + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SP_SUB[2 * i + 1], SP_Y_PLAY)), module, Spool::VOCT_INPUT + i));

			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SP_SUB[2 * i], SP_Y_IO)), module, Spool::RECIN_INPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(SP_SUB[2 * i + 1], SP_Y_IO)), module, Spool::OUT_OUTPUT + i));

			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
				mm2px(Vec(SP_SUB[2 * i], SP_Y_SET)), module, Spool::REC_PARAM + i, Spool::REC_LIGHT + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(SP_SUB[2 * i + 1], SP_Y_SET)), module, Spool::LEVEL_PARAM + i));
		}

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SP_FOOT[0], SP_Y_FOOT)), module, Spool::ROOT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SP_FOOT[1], SP_Y_FOOT)), module, Spool::RESET_INPUT));
		// On the panel rather than in the menu: it changes what every V/OCT cable
		// on the module MEANS, and a setting that reinterprets your patch cables
		// should not be two clicks deep.
		addParam(createParamCentered<CKSS>(mm2px(Vec(SP_GLOB[1], SP_Y_GLOB)), module, Spool::PMODE_PARAM));
		// Each shaping CV sits directly UNDER the trimpot it modulates, which is
		// the house convention and needs no label to say what it does. The art
		// puts them there; the earlier code had them bunched to one side because
		// the old trim row left no room beneath it.
		const int cvI[3] = {Spool::ATTACK_CV_INPUT, Spool::RELEASE_CV_INPUT, Spool::RAMP_CV_INPUT};
		for (int k = 0; k < 3; k++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(SP_FOOT[2 + k], SP_Y_FOOT)), module, cvI[k]));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(SP_FOOT[5], SP_Y_MIX)), module, Spool::MIX_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(SP_FOOT[6], SP_Y_MIX)), module, Spool::MIX_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Spool* m = dynamic_cast<Spool*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		for (int i = 0; i < SP_N; i++) {
			int idx = i;
			menu->addChild(createMenuItem(
				string::f("Load tape %d…", i + 1),
				m->tape[i].len ? m->tape[i].name : "empty",
				[=]() {
					char* p = osdialog_file(OSDIALOG_OPEN, NULL, NULL, NULL);
					if (!p) return;
					m->loadTape(idx, std::string(p));
					std::free(p);
				}));
		}
		menu->addChild(createMenuItem("Rewind all tapes", "", [=]() {
			for (int i = 0; i < SP_N; i++) {
				m->tape[i].pos = 0.0;
				for (int h = 0; h < SP_POLY; h++) {
					m->tape[i].head[h].pos = 0.0;
					m->tape[i].head[h].declick = 0.f;
				}
			}
		}));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("More keys held means more drag on one capstan,"));
		menu->addChild(createMenuLabel("so a chord goes flat. One note is the reference."));
		menu->addChild(new SpoolParamSlider(m, Spool::SAG_PARAM));
		menu->addChild(new MenuSeparator);
		// Driven as a PARAM, not a plain member, so it saves with the patch for
		// free and shows up in the right-click parameter list like everything else.
		menu->addChild(createIndexSubmenuItem("New note starts",
			{"Where the tape stopped", "At the beginning (as a Mellotron does)"},
			[=]() { return (int)std::round(m->params[Spool::REWIND_PARAM].getValue()); },
			[=](int v) { m->params[Spool::REWIND_PARAM].setValue((float)clamp(v, 0, 1)); }));
	}
};

Model* modelSpool = createModel<Spool, SpoolWidget>("Spool");
