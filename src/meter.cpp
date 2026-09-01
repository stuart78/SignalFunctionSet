#include "plugin.hpp"
#include "meter-messages.hpp"
#include "pulse-width.hpp"
#include <cmath>


// Powers-of-2 denominators selectable via configSwitch
static const int DENOM_VALUES[] = { 1, 2, 4, 8, 16, 32 };
static const int NUM_DENOMS = 6;
static const int DENOM_DEFAULT_INDEX = 2; // = 4

// PPQN options for external clock
static const int PPQN_OPTIONS[] = { 1, 2, 4, 8, 12, 16, 24 };
static const int NUM_PPQN_OPTIONS = 7;

// Number of subdivision outputs
static const int NUM_OUTPUTS = 6;

// Subdivision identifiers (matches output array order)
enum SubdivisionId {
	SUB_BAR = 0,
	SUB_QUARTER,
	SUB_EIGHTH,
	SUB_SIXTEENTH,
	SUB_QTRIP,
	SUB_ETRIP
};

static const char* SUB_LABELS[NUM_OUTPUTS] = { "BAR", "Q", "8th", "16th", "QT", "8T" };

// Forward declaration
struct Meter;

struct MeterDisplay : Widget {
	Meter* module = nullptr;
	std::shared_ptr<Font> font;

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawPreview(const DrawArgs& args);   // module==NULL fallback

	void draw(const DrawArgs& args) override {
		Widget::draw(args);
	}
};


struct Meter : Module {
	enum ParamId {
		BPM_PARAM,
		NUMERATOR_PARAM,
		DENOMINATOR_PARAM,
		RUN_PARAM,
		RESET_PARAM,
		SWING_PARAM_0,
		SWING_PARAM_1,
		SWING_PARAM_2,
		SWING_PARAM_3,
		SWING_PARAM_4,
		SWING_PARAM_5,
		PARAMS_LEN
	};
	enum InputId {
		BPM_INPUT,
		NUMERATOR_INPUT,
		DENOMINATOR_INPUT,
		RUN_INPUT,
		EXT_CLOCK_INPUT,
		SWING_CV_0,
		SWING_CV_1,
		SWING_CV_2,
		SWING_CV_3,
		SWING_CV_4,
		SWING_CV_5,
		RESET_INPUT,        // appended — keeps existing patch cable indices valid
		INPUTS_LEN
	};
	enum OutputId {
		// Swung outputs (BAR has no swing — its output is always on the grid).
		BAR_OUTPUT,
		QUARTER_OUTPUT,
		EIGHTH_OUTPUT,
		SIXTEENTH_OUTPUT,
		QUARTER_TRIPLET_OUTPUT,
		EIGHTH_TRIPLET_OUTPUT,
		RESET_OUTPUT,
		// Grid (un-swung) outputs for the 5 swingable subdivisions.
		// Appended after RESET_OUTPUT so existing patches keep their cables.
		QUARTER_GRID_OUTPUT,
		EIGHTH_GRID_OUTPUT,
		SIXTEENTH_GRID_OUTPUT,
		QUARTER_TRIPLET_GRID_OUTPUT,
		EIGHTH_TRIPLET_GRID_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		RUN_LIGHT,
		LIGHTS_LEN
	};

	// --- Phase accumulators per subdivision (in samples since last on-beat) ---
	// We use sample-counting instead of float phases to allow swing offset
	float samplesSinceQuarter = 0.f;
	float samplesSinceEighth = 0.f;
	float samplesSinceSixteenth = 0.f;
	float samplesSinceQTrip = 0.f;
	float samplesSinceETrip = 0.f;

	// Pulse counter for each subdivision (used for swing on-beat/off-beat tracking)
	int pulseCountQuarter = 0;
	int pulseCountEighth = 0;
	int pulseCountSixteenth = 0;
	int pulseCountQTrip = 0;
	int pulseCountETrip = 0;

	// --- Bar tracking ---
	int sixteenthCount = 0;
	int sixteenthsPerBar = 16;

	// --- Time signature state ---
	int activeNumerator = 4;
	int activeDenominator = 4;
	int pendingNumerator = 4;
	int pendingDenominator = 4;
	bool hasPendingChange = false;

	// --- Pulse generators ---
	dsp::PulseGenerator pulses[NUM_OUTPUTS];

	// --- Triggers ---
	dsp::PulseGenerator resetOutPulse;
	dsp::SchmittTrigger resetButtonTrigger;
	dsp::SchmittTrigger resetInputTrigger;
	dsp::SchmittTrigger extClockTrigger;

	// --- Run state ---
	bool running = true;

	// --- Encoder-safe pulse width (index into sfs::PULSE_WIDTHS) ---
	int pulseWidthIdx = 0;   // 0 == 1 ms (legacy default)
	// Period of each subdivision in samples, refreshed every process() call.
	float basePeriods[NUM_OUTPUTS] = {};
	float sampleTimeCached = 1.f / 44100.f;

	// --- External clock measurement ---
	int samplesSinceLastExtPulse = 0;
	// 24 PPQN, the MIDI standard, because that is what a host sends and host
	// sync is what this input is for. It defaulted to 4, which matches nothing
	// a DAW emits: a fresh Meter fed MIDI clock measured an impossible tempo on
	// every tick and fell back to its knob. Saved patches carry their own value,
	// so this moves nothing that already works.
	int extClockPpqnIndex = 6;
	float measuredBpm = 120.f;
	float measuredBpmRaw = 120.f;
	// The smoothing runs on the PERIOD, not on the tempo. Averaging BPM
	// biases the result high whenever the tick intervals are uneven — mean(1/T)
	// is not 1/mean(T) — and host MIDI clock quantized to the audio block is
	// exactly that: alternating long and short intervals around the true one.
	// With 512-sample blocks the tempo bias alone walked the clock a whole
	// eighth note off the host inside eight seconds.
	float measuredSamplesPerQuarter = 0.f;
	int measurementCount = 0;
	// The clock is ticking steadily but the tempo it implies is impossible, so
	// the PPQN setting does not match what the host is sending. Kept as state
	// because the panel has to be able to SAY so: silently running on the knob
	// with the sync light flashing is the one failure a user cannot diagnose.
	bool extPpqnSuspect = false;
	// Consecutive plausible intervals whose implied tempo is impossible. A
	// COUNT, not a single reading: the gap across a stopped transport is also
	// an interval at an impossible tempo, and it happens once, where a PPQN
	// mismatch happens on every tick. Four ticks is 83 ms at 24 PPQN.
	int extBadTicks = 0;
	bool extClockHasMeasurement = false;
	// True once a tick has been seen that the NEXT tick can be measured
	// against. Cleared whenever the clock goes quiet, so the silence across a
	// stopped transport is never mistaken for one very long tick.
	bool extPulseValid = false;
	bool extPulseThisSample = false;
	// Set by a reset/downbeat: the next tick snaps the phase outright instead
	// of easing toward it.
	bool extLockPending = true;
	// Position of the next incoming tick within the quarter note, counted from
	// the tick the lock snapped to. MIDI clock is a counted stream and says
	// nothing on its own about which tick is a beat, so counting is the only
	// way to know. Aligning to the NEAREST tick instead needs no count, but at
	// 24 PPQN the ticks are 20 ms apart, so a host whose clock jitters by a
	// fair share of that can walk the lock onto the neighbouring tick — and
	// nearest-tick alignment is then perfectly content, one whole tick out.
	int extTickIdx = 0;
	// The first tick after the clock has been silent — the tick a host emits
	// as its transport starts. It is the only tick that says "the master just
	// came back", which is what separates a Stop from a Start.
	float samplesSinceGapTick = 1e9f;
	// Filtered estimate of how far the grid sits from where the count says it
	// should be, in samples. The correction is a share of this, never of a
	// single raw reading.
	float extPhaseOffset = 0.f;
	// Position within the quarter note, in samples, used only as the lock's
	// reference. It is kept separately from the quarter grid accumulator
	// because the bar wrap force-realigns the grid accumulators, and in an odd
	// meter the bar does not land on a quarter — the reference would jump
	// mid-quarter and the lock would yank the clock once a bar.
	float extRefPhase = 0.f;

	// A downbeat is owed. doReset() arms it rather than firing on the spot,
	// because a reset usually arrives while the transport is stopped (a DAW
	// sends Stop, then Start) and the downbeat belongs to the sample playback
	// actually resumes on.
	bool pendingDownbeat = true;
	// Running samples since the last downbeat. One MIDI Stop drives both the
	// Reset cable and whatever holds the RUN gate, so Meter is still running
	// when the reset lands and would spend beat 1 there — a sample or two
	// before the transport actually stops, and a whole take before anyone
	// hears it. If the clock stops before even a sixteenth of that beat has
	// played, nothing was played, and the downbeat is owed again.
	float samplesSinceDownbeat = 0.f;
	bool prevEffectiveRunning = false;

	// --- Display state ---
	int displayedSixteenth = 0;
	float displayedBpm = 120.f;
	bool extClockConnected = false;
	int barsSinceReset = 0;       // Increments on each bar wrap; cleared on Reset
	float syncFlash = 0.f;        // Brightness of the sync indicator (decays)

	// --- Meter X expander bus ---
	bool  msgBar = false;         // a BAR pulse fired this sample (for the expander)
	bool  msgPpqn = false;        // a 24-PPQN pulse fired this sample
	float samplesSince24 = 0.f;   // 24-PPQN accumulator (straight, un-swung)
	void writeExpander(bool running) {
		if (!(rightExpander.module && rightExpander.module->model == modelMeterExpander)) return;
		auto* m = (MeterExpanderMessage*)rightExpander.producerMessage;
		if (!m) return;
		m->running = running;
		m->ppqn24 = msgPpqn;
		static const int NB[8] = {1, 2, 4, 8, 16, 32, 64, 128};
		for (int k = 0; k < 8; k++) m->bar[k] = msgBar && (barsSinceReset % NB[k] == 0);
		// Continuous bar position: whole bars since reset + fraction through the
		// current bar (sixteenth count + sub-sixteenth). Drives the expander's
		// per-output cycle pie charts. Frozen while stopped (accumulators idle).
		float frac = 0.f;
		if (sixteenthsPerBar > 0) {
			float sixteenthLen = lastSamplesPerQuarter / 4.f;
			float sub = (sixteenthLen > 0.f) ? clamp(samplesSinceSixteenth / sixteenthLen, 0.f, 1.f) : 0.f;
			frac = ((float)sixteenthCount + sub) / (float)sixteenthsPerBar;
		}
		m->barPos = (float)barsSinceReset + frac;
		m->quarterSec = lastSamplesPerQuarter * sampleTimeCached;
		m->barSec     = basePeriods[SUB_BAR] * sampleTimeCached;
		rightExpander.requestMessageFlip();
	}

	// --- Grid (un-swung) phase trackers ---
	// 5 entries for the swingable subdivisions: Q, E, S, QT, ET (no BAR
	// straight — BAR has no swing, so BAR_OUTPUT is already on the grid).
	float samplesSinceGrid[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
	dsp::PulseGenerator pulses_grid[5];

	// Cached samplesPerQuarter from previous process call. When BPM changes,
	// every accumulator gets scaled by (new / old) so the *phase fraction*
	// (accumulator / basePeriod) is preserved — otherwise a sudden BPM jump
	// would fire some subdivisions early and delay others, breaking their
	// relative alignment. This is the bug that surfaced after rapid BPM /
	// time-sig sweeps: subdivisions drift out of phase with the bar.
	float lastSamplesPerQuarter = 0.f;

	// Swing values: pending = what the user has dialed in (knob+CV); active =
	// what the DSP is currently using. Pending → active transfer happens on
	// bar boundaries to avoid mid-period accumulator glitches when swing
	// changes (recomputing swingAdjustedPeriod mid-bar would either fire a
	// pulse early or swallow one).
	float pendingSwing[NUM_OUTPUTS] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	float activeSwing[NUM_OUTPUTS]  = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	bool firstProcess = true;

	// Display mirror of activeSwing
	float displayedSwing[NUM_OUTPUTS] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };

	// Per-output pulse flash for the indicator ticks. flashIdx = which tick
	// (0-indexed within the bar) most recently fired; flash = brightness 0..1
	// that decays over ~100ms; pulseInBar = running counter of pulses fired
	// in the current bar (reset on bar boundary).
	float pulseFlash[NUM_OUTPUTS]  = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	int pulseFlashIdx[NUM_OUTPUTS] = { 0, 0, 0, 0, 0, 0 };
	int pulseInBar[NUM_OUTPUTS]    = { 0, 0, 0, 0, 0, 0 };

	// --- Context menu options ---
	bool applyTimeSigImmediately = false;
	bool resetOnPlay = false;
	bool bpmCvAbsolute = false;   // BPM CV as absolute 0.01V/BPM (e.g. from Arrange) vs additive offset

	// Set by MeterWidget::step() when the NUM/DEN cable originates from a Fill
	// module, whose CV is absolute (0.5V/count, 1V/denom index) rather than an
	// additive offset. Not serialized — re-detected from the cable graph.
	bool numCvFromFill = false;
	bool denCvFromFill = false;

	~Meter() {
		delete (MeterExpanderMessage*)rightExpander.producerMessage;
		delete (MeterExpanderMessage*)rightExpander.consumerMessage;
	}

	Meter() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		rightExpander.producerMessage = new MeterExpanderMessage();
		rightExpander.consumerMessage = new MeterExpanderMessage();

		configParam(BPM_PARAM, 30.f, 300.f, 120.f, "BPM", " bpm");
		configParam(NUMERATOR_PARAM, 1.f, 16.f, 4.f, "Numerator");
		paramQuantities[NUMERATOR_PARAM]->snapEnabled = true;
		configSwitch(DENOMINATOR_PARAM, 0.f, (float)(NUM_DENOMS - 1), (float)DENOM_DEFAULT_INDEX,
			"Denominator", {"1", "2", "4", "8", "16", "32"});

		configButton(RUN_PARAM, "Run / Stop");
		configButton(RESET_PARAM, "Reset");

		// Per-output swing params (no per-output enable anymore)
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			configParam(SWING_PARAM_0 + i, -0.5f, 0.5f, 0.f,
				string::f("%s swing", SUB_LABELS[i]), "%", 0.f, 100.f);
		}

		configInput(BPM_INPUT, "BPM CV (additive ~27 BPM/V, or absolute 0.01V/BPM via menu)");
		configInput(NUMERATOR_INPUT, "Numerator CV");
		configInput(DENOMINATOR_INPUT, "Denominator CV");
		configInput(RUN_INPUT, "Run gate");
		configInput(EXT_CLOCK_INPUT, "External clock");
		configInput(RESET_INPUT, "Reset (resets bar/position, forwards to Reset OUT)");

		for (int i = 0; i < NUM_OUTPUTS; i++) {
			configInput(SWING_CV_0 + i, string::f("%s swing CV", SUB_LABELS[i]));
		}

		configOutput(BAR_OUTPUT,                       "Bar");
		configOutput(QUARTER_OUTPUT,                   "Quarter note (swung)");
		configOutput(EIGHTH_OUTPUT,                    "Eighth note (swung)");
		configOutput(SIXTEENTH_OUTPUT,                 "Sixteenth note (swung)");
		configOutput(QUARTER_TRIPLET_OUTPUT,           "Quarter triplet (swung)");
		configOutput(EIGHTH_TRIPLET_OUTPUT,            "Eighth triplet (swung)");
		configOutput(RESET_OUTPUT,                     "Reset (fires on Reset button or Reset IN)");
		configOutput(QUARTER_GRID_OUTPUT,          "Quarter note (grid, no swing)");
		configOutput(EIGHTH_GRID_OUTPUT,           "Eighth note (grid, no swing)");
		configOutput(SIXTEENTH_GRID_OUTPUT,        "Sixteenth note (grid, no swing)");
		configOutput(QUARTER_TRIPLET_GRID_OUTPUT,  "Quarter triplet (grid)");
		configOutput(EIGHTH_TRIPLET_GRID_OUTPUT,   "Eighth triplet (grid)");
	}

	void onReset() override {
		samplesSinceQuarter = samplesSinceEighth = samplesSinceSixteenth = 0.f;
		samplesSinceQTrip = samplesSinceETrip = 0.f;
		pulseCountQuarter = pulseCountEighth = pulseCountSixteenth = 0;
		pulseCountQTrip = pulseCountETrip = 0;
		sixteenthCount = 0;
		activeNumerator = pendingNumerator = 4;
		activeDenominator = pendingDenominator = 4;
		sixteenthsPerBar = 16;
		hasPendingChange = false;
		running = true;
		samplesSinceLastExtPulse = 0;
		extClockHasMeasurement = false;
		extPulseValid = false;
		measuredSamplesPerQuarter = 0.f;
		measurementCount = 0;
		extPpqnSuspect = false;
		extBadTicks = 0;
		extLockPending = true;
		extPhaseOffset = 0.f;
		extRefPhase = 0.f;
		extTickIdx = 0;
		pendingDownbeat = true;
		samplesSinceDownbeat = 0.f;
		prevEffectiveRunning = false;
		measuredBpm = measuredBpmRaw = 120.f;
		displayedSixteenth = 0;
		barsSinceReset = 0;
		syncFlash = 0.f;
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			activeSwing[i] = pendingSwing[i];
			pulseFlash[i] = 0.f;
			pulseFlashIdx[i] = 0;
			pulseInBar[i] = 0;
		}
		for (int i = 0; i < 5; i++) samplesSinceGrid[i] = 0.f;
		firstProcess = true;
	}

	void recomputeSixteenthsPerBar() {
		sixteenthsPerBar = activeNumerator * 16 / activeDenominator;
		if (sixteenthsPerBar < 1) sixteenthsPerBar = 1;
		if (sixteenthCount >= sixteenthsPerBar) sixteenthCount = 0;
	}

	void doReset() {
		samplesSinceQuarter = samplesSinceEighth = samplesSinceSixteenth = 0.f;
		samplesSinceQTrip = samplesSinceETrip = 0.f;
		pulseCountQuarter = pulseCountEighth = pulseCountSixteenth = 0;
		pulseCountQTrip = pulseCountETrip = 0;
		sixteenthCount = 0;
		// Reset grid (un-swung) accumulators too so they stay phase-locked
		// with the bar. Without this, pressing Reset mid-bar leaves the grid
		// outputs at their pre-reset phase; 16 sixteenths later when the bar
		// wraps, the bar-wrap force-trigger fires an EXTRA grid pulse out of
		// phase with the natural ones — downstream Beat/Note hear it as a
		// double-CLOCK on the bar boundary ("early on 2nd loop").
		for (int i = 0; i < 5; i++) samplesSinceGrid[i] = 0.f;
		lastSamplesPerQuarter = 0.f;
		if (hasPendingChange) {
			activeNumerator = pendingNumerator;
			activeDenominator = pendingDenominator;
			recomputeSixteenthsPerBar();
			hasPendingChange = false;
		}
		// Apply any pending swing on reset so the first bar plays correctly
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			activeSwing[i] = pendingSwing[i];
		}
		// Arm the downbeat rather than firing it here. Firing on the spot is
		// wrong for the transport case, which is the common one under a DAW:
		// the host sends Stop (-> Reset) and Start (-> Run) seconds apart, and
		// a pulse fired at Stop is a pulse that does not land on beat 1 of the
		// take. fireDownbeat() runs on the first sample Meter is actually
		// running, so the downbeat lands where playback begins.
		pendingDownbeat = true;
		extLockPending = true;
		displayedSixteenth = 0;
		barsSinceReset = 0;
		msgBar = true;             // reset is a bar downbeat for the expander
		samplesSince24 = 0.f;      // re-lock the 24-PPQN clock
	}

	// Beat 1: every subdivision fires together, swung AND grid. The grid set
	// used to be left out here on the grounds that it would double-clock a
	// module patched from a grid output with no reset cable — but BAR fires on
	// this same sample (Beat and Note collapse that coincidence), the natural
	// bar wrap below force-fires the grid set in exactly this way, and leaving
	// it out is why a grid-clocked voice was silent on beat 1 and only spoke
	// one subdivision later.
	void fireDownbeat(float sampleTime) {
		samplesSinceQuarter = samplesSinceEighth = samplesSinceSixteenth = 0.f;
		samplesSinceQTrip = samplesSinceETrip = 0.f;
		pulseCountQuarter = pulseCountEighth = pulseCountSixteenth = 0;
		pulseCountQTrip = pulseCountETrip = 0;
		sixteenthCount = 0;
		for (int i = 0; i < 5; i++) samplesSinceGrid[i] = 0.f;
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			pulses[i].trigger(sfs::pulseWidthSec(pulseWidthIdx,
			                                     basePeriods[i] * sampleTime));
			pulseFlashIdx[i] = 0;
			pulseInBar[i] = 1;
			pulseFlash[i] = 1.f;
		}
		const float gridBase[5] = {
			basePeriods[SUB_QUARTER], basePeriods[SUB_EIGHTH],
			basePeriods[SUB_SIXTEENTH], basePeriods[SUB_QTRIP],
			basePeriods[SUB_ETRIP]
		};
		for (int i = 0; i < 5; i++) {
			pulses_grid[i].trigger(sfs::pulseWidthSec(pulseWidthIdx,
			                                          gridBase[i] * sampleTime));
		}
		displayedSixteenth = 0;
		msgBar = true;
		samplesSince24 = 0.f;
		samplesSinceDownbeat = 0.f;
		// A downbeat is the moment the tick count has to be re-anchored: the
		// next tick is the beat, whatever the previous count believed.
		extLockPending = true;
		extPhaseOffset = 0.f;
		extRefPhase = 0.f;
	}

	// Returns swing-adjusted target sample count for the next pulse.
	// pulseCount: total pulses fired since last reset (used to determine on/off-beat)
	// basePeriod: samples between successive pulses with no swing
	// swingAmount: -0.5 to +0.5 range
	// Returns: samples for the next pulse to fire
	float swingAdjustedPeriod(int pulseCount, float basePeriod, float swingAmount) {
		if (std::fabs(swingAmount) < 0.001f) return basePeriod;
		// Off-beat pulses (odd index after the trigger) get displaced.
		// At swing=+0.5, off-beat is delayed by half a period (pure triplet feel).
		// At swing=-0.5, off-beat fires half a period early.
		// Pairs of (on-beat → off-beat) take basePeriod*2 total time regardless.
		// pulseCount is the number of pulses already fired since reset.
		// The NEXT pulse to fire is pulse (pulseCount+1).
		// Off-beats are pulses 1, 3, 5... (odd index). The period LEADING to an
		// off-beat (i.e. when pulseCount is even) gets (1 + swing) so positive
		// swing delays the off-beat (standard shuffle convention).
		bool nextIsOffBeat = (pulseCount % 2) == 0;
		if (nextIsOffBeat) {
			return basePeriod * (1.f + swingAmount);
		} else {
			return basePeriod * (1.f - swingAmount);
		}
	}

	void process(const ProcessArgs& args) override {
		msgBar = false; msgPpqn = false;   // per-sample expander flags
		// --- Run button latch + gate override ---
		if (params[RUN_PARAM].getValue() > 0.f) {
			params[RUN_PARAM].setValue(0.f);
			running = !running;
			if (running && resetOnPlay) doReset();
		}
		bool effectiveRunning = running;
		if (inputs[RUN_INPUT].isConnected()) {
			effectiveRunning = inputs[RUN_INPUT].getVoltage() >= 1.f;
		}
		lights[RUN_LIGHT].setBrightness(effectiveRunning ? 1.f : 0.f);

		// "Reset on play" used to be honoured only by the RUN button, which is
		// the one place a DAW rig never touches: under a host, Run arrives as
		// a gate. Apply it to the gate's rising edge as well, and forward the
		// reset downstream — Meter is the master, so a bar it restarts is a
		// bar its sequencers have to restart with it.
		if (effectiveRunning && !prevEffectiveRunning && resetOnPlay
			&& inputs[RUN_INPUT].isConnected()) {
			doReset();
			resetOutPulse.trigger(sfs::pulseWidthSec(pulseWidthIdx));
		}
		prevEffectiveRunning = effectiveRunning;

		// --- Reset (button or Reset IN; Reset OUT forwards to downstream
		//     modules like Beat) ---
		bool resetBtn = resetButtonTrigger.process(params[RESET_PARAM].getValue());
		bool resetIn = inputs[RESET_INPUT].isConnected()
			&& resetInputTrigger.process(inputs[RESET_INPUT].getVoltage());
		if (resetBtn || resetIn) {
			doReset();
			// RESET stays a trigger whatever the gate setting says -- a reset with a
			// duty cycle is not a thing, and a downstream module wants the edge.
			resetOutPulse.trigger(sfs::pulseWidthSec(pulseWidthIdx));
		}

		// --- Read CV-modulated parameters ---
		float bpmKnob = params[BPM_PARAM].getValue();
		if (inputs[BPM_INPUT].isConnected()) {
			if (bpmCvAbsolute) bpmKnob = inputs[BPM_INPUT].getVoltage() * 100.f;   // 0.01V/BPM absolute (Arrange)
			else               bpmKnob += inputs[BPM_INPUT].getVoltage() * 27.f;   // additive offset
		}
		bpmKnob = clamp(bpmKnob, 30.f, 300.f);

		int numKnob = (int)std::round(params[NUMERATOR_PARAM].getValue());
		if (inputs[NUMERATOR_INPUT].isConnected()) {
			if (numCvFromFill) numKnob = (int)std::round(inputs[NUMERATOR_INPUT].getVoltage() * 2.f);   // 0.5V/count (Fill)
			else               numKnob += (int)std::round(inputs[NUMERATOR_INPUT].getVoltage() * 1.5f);
		}
		numKnob = clamp(numKnob, 1, 16);

		int denIdx = (int)std::round(params[DENOMINATOR_PARAM].getValue());
		if (inputs[DENOMINATOR_INPUT].isConnected()) {
			if (denCvFromFill) denIdx = (int)std::round(inputs[DENOMINATOR_INPUT].getVoltage());        // 1V/index (Fill)
			else               denIdx += (int)std::round(inputs[DENOMINATOR_INPUT].getVoltage() * 0.5f);
		}
		denIdx = clamp(denIdx, 0, NUM_DENOMS - 1);
		int denValue = DENOM_VALUES[denIdx];

		// --- External clock processing ---
		extClockConnected = inputs[EXT_CLOCK_INPUT].isConnected();
		extPulseThisSample = false;
		if (extClockConnected) {
			if (extClockTrigger.process(inputs[EXT_CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				bool measured = false;
				if (extPulseValid && samplesSinceLastExtPulse > 0) {
					int ppqn = PPQN_OPTIONS[extClockPpqnIndex];
					float samplesPerQuarter = (float)samplesSinceLastExtPulse * (float)ppqn;
					float bpm = 60.f * args.sampleRate / samplesPerQuarter;
					// IS THIS A TICK INTERVAL AT ALL? Judge the INTERVAL, not
					// the tempo it implies.
					//
					// The question being asked is whether this gap is a tick or
					// the silence across a stopped transport (the first tick
					// after Start measured against the last tick before Stop),
					// or a double-trigger. Neither of those depends on the PPQN
					// setting — but a tempo window does, and testing the tempo
					// made the PPQN setting decide whether ANY tick was ever
					// accepted. With the menu on its old default of 4 and a host
					// sending MIDI clock's 24, every genuine tick implied 720
					// BPM, every interval was thrown away, extClockHasMeasurement
					// never became true, and Meter ran at its BPM KNOB while the
					// sync light flashed happily. Slower than the host, drifting
					// for ever, and nothing on the panel said why.
					//
					// Two seconds is 30 BPM at 1 PPQN, the slowest tick any
					// setting can legitimately produce; two milliseconds is
					// faster than any of them can go.
					float gapSec = (float)samplesSinceLastExtPulse / args.sampleRate;
					bool isTick = gapSec >= 0.002f && gapSec <= 2.f;
					// A steady tick whose tempo is impossible means the PPQN is
					// wrong, and that is worth reporting rather than absorbing.
					if (isTick && (bpm < 30.f || bpm > 300.f)) extBadTicks++;
					else if (isTick)                           extBadTicks = 0;
					extPpqnSuspect = extBadTicks >= 4;
					if (isTick && bpm >= 30.f && bpm <= 300.f) {
						measuredBpmRaw = bpm;
						// Smooth per TICK, not per sample. The old per-sample
						// filter reached the raw value in ~10 samples, so it
						// passed every bit of host MIDI jitter straight into
						// the tempo.
						if (!extClockHasMeasurement) {
							measuredSamplesPerQuarter = samplesPerQuarter;
							measurementCount = 1;
							extClockHasMeasurement = true;
						} else {
							// A running mean that settles into a slow EWMA: the
							// early ticks carry full weight so the tempo is
							// right within a few ticks of Start, and the gain
							// then falls to 0.04 so the estimate stops riding
							// the host's block quantization. A fixed 0.15 left
							// about 1% of wobble on it, and that wobble goes
							// out in the gate widths and in Meter X's bar
							// clock even though the grid itself stays locked.
							measurementCount++;
							float g = std::max(0.04f, 1.f / (float)measurementCount);
							measuredSamplesPerQuarter +=
								(samplesPerQuarter - measuredSamplesPerQuarter) * g;
						}
						measuredBpm = clamp(60.f * args.sampleRate / measuredSamplesPerQuarter,
						                    30.f, 300.f);
						measured = true;
					}
				}
				// No usable interval behind it: this tick opens the clock,
				// rather than continuing it.
				if (!measured) samplesSinceGapTick = 0.f;
				samplesSinceLastExtPulse = 0;
				extPulseValid = true;
				extPulseThisSample = true;
				syncFlash = 1.f;  // Light up the sync indicator
			}
			// Four seconds of silence is below the 30 BPM floor by any PPQN:
			// the clock is gone, so stop counting (the counter would overflow
			// after a few hours) and require a fresh pair of ticks.
			if (samplesSinceLastExtPulse > (int)(args.sampleRate * 4.f)) {
				extPulseValid = false;
			} else {
				samplesSinceLastExtPulse++;
			}
			if (samplesSinceGapTick < 1e9f) samplesSinceGapTick += 1.f;
		} else {
			samplesSinceLastExtPulse = 0;
			extClockHasMeasurement = false;
			extPulseValid = false;
			measuredSamplesPerQuarter = 0.f;
			measurementCount = 0;
			extPpqnSuspect = false;
			extBadTicks = 0;
		}

		float effectiveBpm = (extClockConnected && extClockHasMeasurement) ? measuredBpm : bpmKnob;
		displayedBpm = effectiveBpm;

		// --- Pending time sig change ---
		if (numKnob != pendingNumerator || denValue != pendingDenominator) {
			pendingNumerator = numKnob;
			pendingDenominator = denValue;
			if (numKnob == activeNumerator && denValue == activeDenominator) {
				hasPendingChange = false;
			} else if (applyTimeSigImmediately) {
				activeNumerator = pendingNumerator;
				activeDenominator = pendingDenominator;
				recomputeSixteenthsPerBar();
				hasPendingChange = false;
			} else {
				hasPendingChange = true;
			}
		}

		// --- Read pending swing from knobs+CV every sample (even when stopped
		//     so the indicator/ghost shows the latest setting). The DSP only
		//     uses activeSwing, which is committed on bar boundaries. ---
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			if (i == SUB_BAR) {
				pendingSwing[i] = 0.f;
				activeSwing[i] = 0.f;
				displayedSwing[i] = 0.f;
				continue;
			}
			float s = params[SWING_PARAM_0 + i].getValue();
			if (inputs[SWING_CV_0 + i].isConnected())
				s += inputs[SWING_CV_0 + i].getVoltage() * 0.1f;
			pendingSwing[i] = clamp(s, -0.5f, 0.5f);
			// Display reflects the user's knob position (pending) rather than
			// the currently-active swing — the trimpot and ghost indicator
			// stay visually responsive even while playback waits for the bar
			// boundary to adopt the new value.
			displayedSwing[i] = pendingSwing[i];
		}

		// On the very first process call after construction, commit pending
		// → active so initial knob position takes effect immediately rather
		// than waiting for the first bar boundary.
		if (firstProcess) {
			for (int i = 0; i < NUM_OUTPUTS; i++) activeSwing[i] = pendingSwing[i];
			firstProcess = false;
		}

		// --- Reset output: still drive even when stopped so a Reset button
		//     press downstream-resets connected modules without playback. ---
		bool resetHi = resetOutPulse.process(args.sampleTime);
		outputs[RESET_OUTPUT].setVoltage(resetHi ? 10.f : 0.f);

		// --- If not running, freeze subdivision outputs ---
		if (!effectiveRunning) {
			for (int i = 0; i < NUM_OUTPUTS; i++) {
				outputs[BAR_OUTPUT + i].setVoltage(0.f);
			}
			// The grid outputs need clearing too. They were left alone here,
			// so one stopped mid-pulse stayed high for the whole stop — and a
			// gate already high has no edge left to give when the downbeat
			// fires on restart. With the pulse width set as a share of the
			// step rather than 1 ms, that is not a rare coincidence.
			const int gridOutIdsStopped[5] = {
				QUARTER_GRID_OUTPUT, EIGHTH_GRID_OUTPUT, SIXTEENTH_GRID_OUTPUT,
				QUARTER_TRIPLET_GRID_OUTPUT, EIGHTH_TRIPLET_GRID_OUTPUT
			};
			for (int i = 0; i < 5; i++) outputs[gridOutIdsStopped[i]].setVoltage(0.f);
			// If the clock stopped before a sixteenth of the last downbeat had
			// played, nothing was played: the downbeat is owed again, and this
			// time it lands where playback resumes.
			if (samplesSinceDownbeat < basePeriods[SUB_SIXTEENTH]) {
				pendingDownbeat = true;
			}
			writeExpander(false);   // RUN gate low, no clock (msgBar may still be set by a Reset)
			return;
		}

		// --- Compute per-subdivision base periods (samples per pulse, no swing) ---
		float samplesPerQuarter = 60.f * args.sampleRate / effectiveBpm;

		// --- 24 PPQN clock (straight, un-swung) for the expander ---
		float period24 = samplesPerQuarter / 24.f;
		samplesSince24 += 1.f;
		if (period24 > 0.f && samplesSince24 >= period24) {
			samplesSince24 -= period24;
			msgPpqn = true;
		}

		// --- BPM-change rescaling ---
		// When samplesPerQuarter changes (BPM knob, BPM CV, ext clock LPF
		// settling, etc.), scale every accumulator by the same ratio so each
		// subdivision's phase fraction is preserved across the change. Without
		// this, a sudden BPM jump can push one accumulator past its new
		// threshold (firing immediately) while leaving another below its
		// threshold — the subdivisions then drift out of phase with each
		// other and with the bar. Skipping the very first frame avoids a
		// divide-by-zero / huge-ratio glitch on startup.
		if (lastSamplesPerQuarter > 0.f
			&& std::fabs(samplesPerQuarter - lastSamplesPerQuarter) > 0.001f) {
			float ratio = samplesPerQuarter / lastSamplesPerQuarter;
			samplesSinceQuarter   *= ratio;
			samplesSinceEighth    *= ratio;
			samplesSinceSixteenth *= ratio;
			samplesSinceQTrip     *= ratio;
			samplesSinceETrip     *= ratio;
			for (int i = 0; i < 5; i++) samplesSinceGrid[i] *= ratio;
			extRefPhase *= ratio;
		}
		lastSamplesPerQuarter = samplesPerQuarter;
		// Member, not a local: fireDownbeat() needs the periods too, and a
		// duty-cycle gate needs a period to be a fraction of.
		basePeriods[SUB_BAR] = samplesPerQuarter * (float)sixteenthsPerBar / 4.f;
		basePeriods[SUB_QUARTER] = samplesPerQuarter;
		basePeriods[SUB_EIGHTH] = samplesPerQuarter / 2.f;
		basePeriods[SUB_SIXTEENTH] = samplesPerQuarter / 4.f;
		basePeriods[SUB_QTRIP] = samplesPerQuarter / 3.f;
		basePeriods[SUB_ETRIP] = samplesPerQuarter / 6.f;
		sampleTimeCached = args.sampleTime;

		// --- Owed downbeat (armed by Reset, or by a fresh module) ---
		// Fired here, on the first running sample, rather than inside
		// doReset(): under a DAW the reset arrives with the transport stopped
		// and beat 1 belongs to where playback starts.
		//
		// When slaved to an external clock, beat 1 is where the MASTER says it
		// is, and a master that has stopped ticking is not saying anything —
		// so an owed downbeat waits for the clock to come back. That is what
		// makes this independent of how Run/Stop happen to be wired: the host
		// sends Stop, Meter takes the reset while the RUN gate is still high
		// (or never drops at all, if the gate is held by a flipflop cleared by
		// Continue rather than Stop), and without this the downbeat is spent
		// there — a whole take before anyone could hear it.
		//
		// A clock that IS ticking does not delay anything: a mid-run Reset
		// fires on the spot, as it always did.
		float tickPeriod = samplesPerQuarter / (float)PPQN_OPTIONS[extClockPpqnIndex];
		bool clockLive = !extClockConnected
			|| (extPulseValid && (float)samplesSinceLastExtPulse <= 2.f * tickPeriod);

		// A master clock that has gone quiet is a stop, whether or not the RUN
		// gate agrees. The gate cannot be relied on: the host's Stop and its
		// last tick arrive together, so at the instant of the Reset the clock
		// still looks alive and the downbeat fires — and if the gate is held by
		// something Stop does not clear, no stop ever arrives to re-arm it. So
		// when the ticks dry up, check the same thing the stop check does: if
		// less than a sixteenth has played since that downbeat, nothing was
		// played into it and it is owed again.
		if (extClockConnected && !clockLive
			&& samplesSinceDownbeat < basePeriods[SUB_SIXTEENTH]) {
			pendingDownbeat = true;
		}

		// Fire on a tick, or within one tick of the clock coming back. NOT
		// merely because the clock is alive: the Reset that a host sends with
		// its Stop arrives while the clock still looks alive, and firing there
		// put a stray 1-sample trigger on every output — a drum hit every time
		// you press stop. Waiting costs at most one tick (20.8 ms at 24 PPQN)
		// and lands the downbeat exactly on the master's grid.
		bool clockJustResumed = samplesSinceGapTick <= tickPeriod;
		if (pendingDownbeat
			&& (!extClockConnected || extPulseThisSample || clockJustResumed)) {
			pendingDownbeat = false;
			fireDownbeat(args.sampleTime);
			// fireDownbeat() arms the lock so the NEXT tick becomes the beat.
			// That is right only when no tick has just gone by. Under a DAW the
			// tick almost always lands a sample or two BEFORE the Run gate
			// rises, and waiting for the next one then declares the wrong tick
			// the beat: measured, it dragged everything after beat 1 a full
			// tick (20.8 ms at 24 PPQN / 120 BPM) off the host, leaving beat 1
			// alone in the right place. If a tick is closer behind us than the
			// next one is ahead, anchor to it instead.
			if (extClockConnected && extPulseValid && !extPulseThisSample
				&& (float)samplesSinceLastExtPulse < tickPeriod) {
				// The beat was at that tick, `back` samples ago. Every
				// accumulator has to be told so, not just the lock's
				// reference: zeroing them here says "the beat is NOW", the
				// lock then sees no error to correct, and the whole grid
				// simply runs `back` samples behind the host for ever.
				// A tick period is smaller than every subdivision, so `back`
				// needs no wrapping.
				float back = (float)samplesSinceLastExtPulse;
				samplesSinceQuarter = samplesSinceEighth = samplesSinceSixteenth = back;
				samplesSinceQTrip = samplesSinceETrip = back;
				for (int i = 0; i < 5; i++) samplesSinceGrid[i] = back;
				extRefPhase = back;
				extTickIdx = 1;            // the tick just passed was index 0
				extPhaseOffset = 0.f;
				extLockPending = false;
			}
		}

		// --- Advance per-subdivision sample counters ---
		samplesSinceQuarter += 1.f;
		samplesSinceEighth += 1.f;
		samplesSinceSixteenth += 1.f;
		samplesSinceQTrip += 1.f;
		samplesSinceETrip += 1.f;
		// Grid (un-swung) phase trackers tick the same way
		for (int i = 0; i < 5; i++) samplesSinceGrid[i] += 1.f;
		samplesSinceDownbeat += 1.f;
		// ...and so does the external-clock lock's own reference
		extRefPhase += 1.f;
		if (extRefPhase >= samplesPerQuarter) extRefPhase -= samplesPerQuarter;

		// --- Phase-lock to the external clock ---
		// Measuring the tick interval sets the RATE and nothing else, which
		// leaves the phase wherever the accumulators happened to be when
		// playback started. Meter then runs at the host's tempo but a fixed
		// distance off the host's grid — the flam a DAW user hears between
		// Rack and the tracks in the timeline. On each tick, pull every
		// accumulator by the SAME delta, so the subdivisions keep their
		// alignment with each other and with the bar.
		if (extPulseThisSample) {
			float tickPeriod = samplesPerQuarter / (float)PPQN_OPTIONS[extClockPpqnIndex];
			// The lock-in snap deliberately does NOT wait for a rate
			// measurement: a measurement takes two ticks, and deferring the
			// snap to the second one declares the wrong tick to be the beat —
			// it put every Meter output exactly one tick behind the host.
			if (tickPeriod > 2.f && (extLockPending || extClockHasMeasurement)) {
				float err, alpha;
				if (extLockPending) {
					// The first tick after a downbeat IS the beat — a host
					// sends Start and its first clock together — so this tick
					// says where beat 1 actually falls, and the grid snaps
					// onto it. Nearest-tick alignment cannot establish this:
					// at 24 PPQN it will settle just as happily onto any of
					// the twelve ticks inside an eighth note, leaving Meter
					// locked to the host's tempo but on the wrong tick.
					err = -extRefPhase;
					float q = samplesPerQuarter;
					while (err < -q * 0.5f) err += q;
					while (err >  q * 0.5f) err -= q;
					alpha = 1.f;
					extPhaseOffset = 0.f;
					extTickIdx = 0;        // this tick is the beat; count from here
				} else {
					// Thereafter the count says where this tick belongs, so
					// the error wraps against the QUARTER rather than against
					// one tick: a whole beat of margin, which host jitter of a
					// few hundred samples cannot cross.
					float raw = (float)extTickIdx * tickPeriod - extRefPhase;
					float q = samplesPerQuarter;
					while (raw < -q * 0.5f) raw += q;
					while (raw >  q * 0.5f) raw -= q;
					// Correct a share of a FILTERED estimate, never of a
					// single raw reading: host MIDI clock arrives quantized to
					// the audio block, and chasing each tick hands that jitter
					// straight to the outputs.
					extPhaseOffset += (raw - extPhaseOffset) * 0.15f;
					err = extPhaseOffset;
					alpha = 0.3f;
				}
				float delta = err * alpha;
				samplesSinceQuarter   += delta;
				samplesSinceEighth    += delta;
				samplesSinceSixteenth += delta;
				samplesSinceQTrip     += delta;
				samplesSinceETrip     += delta;
				for (int i = 0; i < 5; i++) samplesSinceGrid[i] += delta;
				extRefPhase += delta;
				if (extRefPhase < 0.f) extRefPhase += samplesPerQuarter;
				if (extRefPhase >= samplesPerQuarter) extRefPhase -= samplesPerQuarter;
				extPhaseOffset -= delta;   // that much of the offset is now taken out
				extTickIdx = (extTickIdx + 1) % PPQN_OPTIONS[extClockPpqnIndex];
				extLockPending = false;
			}
		}

		// --- Fire grid pulses at exact basePeriod intervals (no swing) ---
		// Index: 0=Q, 1=E, 2=S, 3=QT, 4=ET. Reset on bar boundary below.
		const float gridBase[5] = {
			basePeriods[SUB_QUARTER],
			basePeriods[SUB_EIGHTH],
			basePeriods[SUB_SIXTEENTH],
			basePeriods[SUB_QTRIP],
			basePeriods[SUB_ETRIP]
		};
		for (int i = 0; i < 5; i++) {
			if (samplesSinceGrid[i] >= gridBase[i]) {
				samplesSinceGrid[i] -= gridBase[i];
				pulses_grid[i].trigger(sfs::pulseWidthSec(pulseWidthIdx,
					gridBase[i] * args.sampleTime));
			}
		}

		// Decay pulse flash (~100ms back to dim)
		float flashDecay = args.sampleTime / 0.10f;
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			pulseFlash[i] = std::max(0.f, pulseFlash[i] - flashDecay);
		}
		// Decay sync indicator at the same rate
		syncFlash = std::max(0.f, syncFlash - flashDecay);

		// Track which outputs fired this sample so we can fix up their flash
		// indices if the bar wraps later in the same call.
		bool firedThisSample[NUM_OUTPUTS] = {false, false, false, false, false, false};

		// Helper to fire a pulse and update its flash state
		// `nextPeriod` is the gap to this output's NEXT pulse, in samples, and is
		// what a duty-cycle gate is a fraction of. It has to be the next one and
		// not the one just served: swing makes them differ by up to 3x, and a
		// gate scaled from the long interval would still be high when the short
		// one ended.
		auto firePulse = [&](int outIdx, float nextPeriod) {
			pulses[outIdx].trigger(sfs::pulseWidthSec(pulseWidthIdx,
			                                          nextPeriod * args.sampleTime));
			if (outIdx == SUB_BAR) msgBar = true;   // notify the expander
			pulseFlashIdx[outIdx] = pulseInBar[outIdx];
			pulseInBar[outIdx]++;
			pulseFlash[outIdx] = 1.f;
			firedThisSample[outIdx] = true;
		};

		// --- Check each subdivision for pulse fire (using activeSwing) ---
		// Quarter
		float qTarget = swingAdjustedPeriod(pulseCountQuarter, basePeriods[SUB_QUARTER], activeSwing[SUB_QUARTER]);
		if (samplesSinceQuarter >= qTarget) {
			samplesSinceQuarter -= qTarget;
			pulseCountQuarter++;
			firePulse(SUB_QUARTER, swingAdjustedPeriod(pulseCountQuarter, basePeriods[SUB_QUARTER], activeSwing[SUB_QUARTER]));
		}

		// Eighth
		float eTarget = swingAdjustedPeriod(pulseCountEighth, basePeriods[SUB_EIGHTH], activeSwing[SUB_EIGHTH]);
		if (samplesSinceEighth >= eTarget) {
			samplesSinceEighth -= eTarget;
			pulseCountEighth++;
			firePulse(SUB_EIGHTH, swingAdjustedPeriod(pulseCountEighth, basePeriods[SUB_EIGHTH], activeSwing[SUB_EIGHTH]));
		}

		// Quarter triplet
		float qtTarget = swingAdjustedPeriod(pulseCountQTrip, basePeriods[SUB_QTRIP], activeSwing[SUB_QTRIP]);
		if (samplesSinceQTrip >= qtTarget) {
			samplesSinceQTrip -= qtTarget;
			pulseCountQTrip++;
			firePulse(SUB_QTRIP, swingAdjustedPeriod(pulseCountQTrip, basePeriods[SUB_QTRIP], activeSwing[SUB_QTRIP]));
		}

		// Eighth triplet
		float etTarget = swingAdjustedPeriod(pulseCountETrip, basePeriods[SUB_ETRIP], activeSwing[SUB_ETRIP]);
		if (samplesSinceETrip >= etTarget) {
			samplesSinceETrip -= etTarget;
			pulseCountETrip++;
			firePulse(SUB_ETRIP, swingAdjustedPeriod(pulseCountETrip, basePeriods[SUB_ETRIP], activeSwing[SUB_ETRIP]));
		}

		// Sixteenth (drives bar tracking)
		float sTarget = swingAdjustedPeriod(pulseCountSixteenth, basePeriods[SUB_SIXTEENTH], activeSwing[SUB_SIXTEENTH]);
		if (samplesSinceSixteenth >= sTarget) {
			samplesSinceSixteenth -= sTarget;
			pulseCountSixteenth++;
			firePulse(SUB_SIXTEENTH, swingAdjustedPeriod(pulseCountSixteenth, basePeriods[SUB_SIXTEENTH], activeSwing[SUB_SIXTEENTH]));

			sixteenthCount++;
			if (sixteenthCount >= sixteenthsPerBar) {
				sixteenthCount = 0;
				barsSinceReset++;
				if (hasPendingChange) {
					activeNumerator = pendingNumerator;
					activeDenominator = pendingDenominator;
					recomputeSixteenthsPerBar();
					hasPendingChange = false;
				}
				// Reset per-bar pulse counters. For outputs that fired on
				// this very sample (the bar-boundary downbeat shared with
				// QUARTER/EIGHTH/SIXTEENTH etc.), correct their flashIdx
				// to point at tick 0 of the NEW bar rather than the last
				// tick of the OLD bar.
				for (int i = 0; i < NUM_OUTPUTS; i++) {
					if (firedThisSample[i]) {
						pulseFlashIdx[i] = 0;
						pulseInBar[i] = 1;
					} else {
						pulseInBar[i] = 0;
					}
				}
				firePulse(SUB_BAR, basePeriods[SUB_BAR]);
				// Reset triplet phases on bar boundary
				samplesSinceQTrip = 0.f;
				samplesSinceETrip = 0.f;
				pulseCountQTrip = 0;
				pulseCountETrip = 0;
				// Realign grid pulses with the bar boundary (also fires
				// the downbeat straight pulses, mirroring the swung set).
				for (int i = 0; i < 5; i++) {
					samplesSinceGrid[i] = 0.f;
					pulses_grid[i].trigger(sfs::pulseWidthSec(pulseWidthIdx,
						gridBase[i] * args.sampleTime));
				}
				// Commit pending swing → active for the new bar. Doing it
				// only on bar boundaries prevents mid-period accumulator
				// glitches that can swallow or misplace pulses.
				for (int i = 0; i < NUM_OUTPUTS; i++) {
					activeSwing[i] = pendingSwing[i];
				}
			}
			displayedSixteenth = sixteenthCount;
		}

		// --- Emit swung gate outputs ---
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			bool pulseHigh = pulses[i].process(args.sampleTime);
			outputs[BAR_OUTPUT + i].setVoltage(pulseHigh ? 10.f : 0.f);
		}

		// --- Emit grid (un-swung) gate outputs ---
		const int gridOutIds[5] = {
			QUARTER_GRID_OUTPUT,
			EIGHTH_GRID_OUTPUT,
			SIXTEENTH_GRID_OUTPUT,
			QUARTER_TRIPLET_GRID_OUTPUT,
			EIGHTH_TRIPLET_GRID_OUTPUT
		};
		for (int i = 0; i < 5; i++) {
			bool hi = pulses_grid[i].process(args.sampleTime);
			outputs[gridOutIds[i]].setVoltage(hi ? 10.f : 0.f);
		}

		writeExpander(true);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "running", json_boolean(running));
		json_object_set_new(rootJ, "extClockPpqnIndex", json_integer(extClockPpqnIndex));
		json_object_set_new(rootJ, "applyTimeSigImmediately", json_boolean(applyTimeSigImmediately));
		json_object_set_new(rootJ, "resetOnPlay", json_boolean(resetOnPlay));
		json_object_set_new(rootJ, "bpmCvAbsolute", json_boolean(bpmCvAbsolute));
		json_object_set_new(rootJ, "barsSinceReset", json_integer(barsSinceReset));
		json_object_set_new(rootJ, "pulseWidthIdx", json_integer(pulseWidthIdx));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* runJ = json_object_get(rootJ, "running");
		if (runJ) running = json_boolean_value(runJ);
		json_t* ppqnJ = json_object_get(rootJ, "extClockPpqnIndex");
		if (ppqnJ) extClockPpqnIndex = clamp((int)json_integer_value(ppqnJ), 0, NUM_PPQN_OPTIONS - 1);
		json_t* immJ = json_object_get(rootJ, "applyTimeSigImmediately");
		if (immJ) applyTimeSigImmediately = json_boolean_value(immJ);
		json_t* ropJ = json_object_get(rootJ, "resetOnPlay");
		if (ropJ) resetOnPlay = json_boolean_value(ropJ);
		json_t* bcaJ = json_object_get(rootJ, "bpmCvAbsolute");
		if (bcaJ) bpmCvAbsolute = json_boolean_value(bcaJ);
		json_t* bsrJ = json_object_get(rootJ, "barsSinceReset");
		if (bsrJ) barsSinceReset = (int)json_integer_value(bsrJ);
		json_t* pwJ = json_object_get(rootJ, "pulseWidthIdx");
		if (pwJ) pulseWidthIdx = clamp((int)json_integer_value(pwJ), 0, sfs::NUM_PULSE_WIDTHS - 1);
	}
};


// --- Display drawLayer ---

void MeterDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) {
		Widget::drawLayer(args, layer);
		return;
	}
	if (!module) {
		drawPreview(args);
		return;
	}

	float w = box.size.x;
	float h = box.size.y;

	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	// --- Shared palette (matches Beat) ---
	const NVGcolor COL_BLUE         = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_BLUE_DIM     = nvgRGBA(0x00, 0x97, 0xDE, 0x70);
	const NVGcolor COL_PURPLE       = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
	const NVGcolor COL_PURPLE_MID   = nvgRGBA(0x4A, 0x4A, 0x66, 0xFF);
	const NVGcolor COL_ORANGE       = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_TEXT_BRIGHT  = nvgRGBA(0xFF, 0xFF, 0xFF, 0xFF);
	const NVGcolor COL_TEXT_DIM     = nvgRGBA(0x80, 0x80, 0x80, 0xFF);

	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);

		float topY = h * 0.22f;

		// --- BPM (left) ---
		nvgFontSize(args.vg, 8.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		// A clock that is ticking but cannot be believed says so HERE, where the
		// tempo is read, rather than being absorbed into a knob reading that
		// looks perfectly normal. This is the whole difference between "Meter
		// drifts against my DAW" and "Meter is set to the wrong PPQN".
		bool suspect = module->extClockConnected && module->extPpqnSuspect;
		std::string bpmStr = suspect
			? string::f("%.1f BPM  PPQN?", module->displayedBpm)
			: string::f("%.1f BPM", module->displayedBpm);
		nvgFillColor(args.vg, suspect ? nvgRGB(0xEC, 0x65, 0x2E) : COL_TEXT_DIM);
		nvgText(args.vg, 5.f, topY, bpmStr.c_str(), NULL);

		// --- Sync indicator light (just to the right of BPM, only when ext clock connected) ---
		if (module->extClockConnected) {
			float flashA = clamp(module->syncFlash, 0.f, 1.f);
			int alpha = (int)(60 + 195 * flashA);
			NVGcolor lightCol = nvgRGBA(0xEC, 0x65, 0x2E, (uint8_t)alpha);
			float bounds[4];
			nvgTextBounds(args.vg, 5.f, topY, bpmStr.c_str(), NULL, bounds);
			float lightX = bounds[2] + 4.f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, lightX, topY, 1.8f);
			nvgFillColor(args.vg, lightCol);
			nvgFill(args.vg);
		}

		// --- Time signature (center, big) ---
		nvgFontSize(args.vg, 14.f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_BRIGHT);
		std::string ts = string::f("%d/%d", module->activeNumerator, module->activeDenominator);
		float tsX = module->hasPendingChange ? w * 0.46f : w * 0.5f;
		nvgText(args.vg, tsX, topY, ts.c_str(), NULL);

		if (module->hasPendingChange) {
			nvgFontSize(args.vg, 9.f);
			nvgFillColor(args.vg, COL_TEXT_DIM);
			std::string pend = string::f("%d/%d",
				module->pendingNumerator, module->pendingDenominator);
			nvgText(args.vg, w * 0.62f, topY, pend.c_str(), NULL);
			nvgFontSize(args.vg, 7.f);
			nvgText(args.vg, w * 0.55f, topY, ">", NULL);
		}

		// --- BAR counter (right) ---
		nvgFontSize(args.vg, 8.f);
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		std::string barsStr = string::f("BAR %d", module->barsSinceReset + 1);
		nvgText(args.vg, w - 5.f, topY, barsStr.c_str(), NULL);
	}

	// --- Position tracker: scaled to actual sixteenths_per_bar ---
	int cells = module->sixteenthsPerBar;
	if (cells < 1) cells = 1;
	int beatBoundary = 16 / module->activeDenominator;
	if (beatBoundary < 1) beatBoundary = 1;

	float trackerY = h * 0.78f;
	float trackerH = h * 0.18f;
	float trackerW = w - 6.f;

	// Uniform cell spacing — beat boundaries are indicated via color only.
	float cellSpacing = trackerW / (float)cells;
	float cellW = cellSpacing * 0.85f;

	auto xForSixteenth = [&](float pos) -> float {
		return 3.f + pos * cellSpacing + cellSpacing * 0.5f;
	};

	// --- Per-output hit indicators (6 thin rows above tracker) ---
	float indTop = h * 0.42f;
	float indBottom = trackerY - 1.f;
	float rowH = (indBottom - indTop) / (float)NUM_OUTPUTS;

	// Spacing between hits, in sixteenth-note units, per output
	float hitSpacing[NUM_OUTPUTS] = {
		(float)cells,    // BAR: one hit per bar
		4.f,             // QUARTER
		2.f,             // EIGHTH
		1.f,             // SIXTEENTH
		4.f / 3.f,       // QUARTER TRIPLET
		2.f / 3.f        // EIGHTH TRIPLET
	};

	for (int out = 0; out < NUM_OUTPUTS; out++) {
		float yRow = indTop + (out + 0.5f) * rowH;
		bool enabled = true;   // outputs are always live now

		// Faint baseline
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 3.f, yRow);
		nvgLineTo(args.vg, 3.f + trackerW, yRow);
		nvgStrokeColor(args.vg, nvgRGBA(0x35, 0x35, 0x4D, 0x80));
		nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);

		NVGcolor baseColor = enabled ? COL_BLUE : COL_PURPLE;

		// Per-output flash: when a pulse fires, that tick lights orange and
		// decays back to the base blue color.
		float flash = module->pulseFlash[out];
		int flashIdx = module->pulseFlashIdx[out];

		auto tickColorFor = [&](int tickIdx) -> NVGcolor {
			if (!enabled || flash <= 0.f || tickIdx != flashIdx) return baseColor;
			float t = clamp(flash, 0.f, 1.f);
			// Lerp blue → orange
			int r = (int)(0x00 + (0xEC - 0x00) * t);
			int g = (int)(0x97 + (0x65 - 0x97) * t);
			int b = (int)(0xDE + (0x2E - 0xDE) * t);
			return nvgRGBA(r, g, b, 0xFF);
		};

		float baseSpacing = hitSpacing[out];
		float swingAmt = module->displayedSwing[out];
		float tickH = std::max(rowH * 0.75f, 1.5f);
		float tickW = 1.4f;

		// BAR: just one tick at downbeat (swing has no meaning for once-per-bar)
		if (out == SUB_BAR) {
			float x = xForSixteenth(0.f);
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x - tickW * 0.5f, yRow - tickH * 0.5f, tickW, tickH);
			nvgFillColor(args.vg, tickColorFor(0));
			nvgFill(args.vg);
			continue;
		}

		// Walk pulses with swing applied (matches swingAdjustedPeriod in DSP).
		float pos = 0.f;
		int pulseN = 0;
		int safety = 0;
		bool hasSwing = std::fabs(swingAmt) > 0.001f;
		NVGcolor ghostColor = enabled
			? nvgRGBA(0x00, 0x97, 0xDE, 70)
			: nvgRGBA(0x35, 0x35, 0x4D, 60);
		NVGcolor lineColor = enabled
			? nvgRGBA(0x00, 0x97, 0xDE, 110)
			: nvgRGBA(0x35, 0x35, 0x4D, 80);

		while (pos < (float)cells - 0.0001f && safety < 256) {
			float basePos = (float)pulseN * baseSpacing;
			float xActual = xForSixteenth(pos);

			// Ghost + connector for swung off-beat pulses
			if (hasSwing && std::fabs(basePos - pos) > 0.01f
				&& basePos < (float)cells - 0.0001f) {
				float xBase = xForSixteenth(basePos);

				// Connector line at row baseline
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, xBase, yRow);
				nvgLineTo(args.vg, xActual, yRow);
				nvgStrokeColor(args.vg, lineColor);
				nvgStrokeWidth(args.vg, 0.7f);
				nvgStroke(args.vg);

				// Ghost tick at original position
				float ghostH = tickH * 0.7f;
				nvgBeginPath(args.vg);
				nvgRect(args.vg, xBase - tickW * 0.5f, yRow - ghostH * 0.5f,
					tickW, ghostH);
				nvgFillColor(args.vg, ghostColor);
				nvgFill(args.vg);
			}

			// Actual (swung) tick on top
			nvgBeginPath(args.vg);
			nvgRect(args.vg, xActual - tickW * 0.5f, yRow - tickH * 0.5f, tickW, tickH);
			nvgFillColor(args.vg, tickColorFor(pulseN));
			nvgFill(args.vg);

			float period = ((pulseN % 2) == 0)
				? baseSpacing * (1.f + swingAmt)
				: baseSpacing * (1.f - swingAmt);
			pos += period;
			pulseN++;
			safety++;
		}
	}

	for (int i = 0; i < cells; i++) {
		float cx = 3.f + i * cellSpacing + (cellSpacing - cellW) * 0.5f;
		bool active = (i == module->displayedSixteenth);
		bool beat = (i % beatBoundary == 0);

		NVGcolor c;
		if (active)     c = COL_ORANGE;
		else if (beat)  c = COL_PURPLE_MID;
		else            c = COL_PURPLE;

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, cx, trackerY, cellW, trackerH, 1.f);
		nvgFillColor(args.vg, c);
		nvgFill(args.vg);
	}

	Widget::drawLayer(args, layer);
}


// --- Browser-preview render (module == NULL) ---
// Shows "120.0 BPM", "4/4", "BAR 1", and a simple position tracker.
void MeterDisplay::drawPreview(const DrawArgs& args) {
	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}
	const NVGcolor COL_BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_PURPLE      = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
	const NVGcolor COL_PURPLE_MID  = nvgRGBA(0x4A, 0x4A, 0x66, 0xFF);
	const NVGcolor COL_ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_TEXT_BRIGHT = nvgRGBA(0xFF, 0xFF, 0xFF, 0xFF);
	const NVGcolor COL_TEXT_DIM    = nvgRGBA(0x80, 0x80, 0x80, 0xFF);

	float w = box.size.x;
	float h = box.size.y;

	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		float topY = h * 0.22f;

		nvgFontSize(args.vg, 8.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		nvgText(args.vg, 5.f, topY, "120.0 BPM", NULL);

		nvgFontSize(args.vg, 14.f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_BRIGHT);
		nvgText(args.vg, w * 0.5f, topY, "4/4", NULL);

		nvgFontSize(args.vg, 8.f);
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		nvgText(args.vg, w - 5.f, topY, "BAR 1", NULL);
	}

	// Simple per-output indicator rows (one tick per subdivision)
	const int cells = 16;
	float trackerY = h * 0.78f;
	float trackerH = h * 0.18f;
	float trackerW = w - 6.f;
	float cellSpacing = trackerW / (float)cells;
	float cellW = cellSpacing * 0.85f;

	float indTop = h * 0.42f;
	float indBottom = trackerY - 1.f;
	float rowH = (indBottom - indTop) / 6.f;
	int hitsPerRow[6] = {1, 4, 8, 16, 12, 24};   // BAR, Q, 8th, 16th, QT, 8T

	for (int row = 0; row < 6; row++) {
		float yRow = indTop + (row + 0.5f) * rowH;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 3.f, yRow);
		nvgLineTo(args.vg, 3.f + trackerW, yRow);
		nvgStrokeColor(args.vg, nvgRGBA(0x35, 0x35, 0x4D, 0x80));
		nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);
		int hits = hitsPerRow[row];
		float tickH = std::max(rowH * 0.75f, 1.5f);
		float tickW = 1.4f;
		for (int i = 0; i < hits; i++) {
			float xPos = 3.f + (i + 0.5f) * trackerW / (float)hits;
			NVGcolor c = (i == 0 && row == 0) ? COL_ORANGE : COL_BLUE;
			nvgBeginPath(args.vg);
			nvgRect(args.vg, xPos - tickW * 0.5f, yRow - tickH * 0.5f, tickW, tickH);
			nvgFillColor(args.vg, c); nvgFill(args.vg);
		}
	}

	// Position tracker: cell 0 highlighted, beat boundaries mid-purple
	for (int i = 0; i < cells; i++) {
		float cx = 3.f + i * cellSpacing + (cellSpacing - cellW) * 0.5f;
		NVGcolor c = (i == 0) ? COL_ORANGE
			: (i % 4 == 0) ? COL_PURPLE_MID
			: COL_PURPLE;
		nvgBeginPath(args.vg);
		nvgRect(args.vg, cx, trackerY, cellW, trackerH);
		nvgFillColor(args.vg, c); nvgFill(args.vg);
	}
}


// --- Widget ---

struct MeterWidget : ModuleWidget {
	MeterWidget(Meter* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/meter.svg")));


		// 18HP = 91.44mm
		// Display top, full width
		MeterDisplay* display = new MeterDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(3.0f, 12.0f));
		display->box.size = mm2px(Vec(85.44f, 26.0f));
		addChild(display);

		// --- LEFT COLUMN: clock/transport controls (positions per Meter SVG) ---
		// BPM knob (smaller — RoundBlackKnob, was RoundHugeBlackKnob)
		addParam(createParamCentered<RoundBlackKnob>(
			mm2px(Vec(10.06f, 50.79f)), module, Meter::BPM_PARAM));
		// BPM CV jack (right of BPM knob)
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(20.22f, 50.79f)), module, Meter::BPM_INPUT));

		// SYNC / EXT clock input
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, 71.11f)), module, Meter::EXT_CLOCK_INPUT));

		// NUM + DEN knobs and their CV jacks
		addParam(createParamCentered<RoundBlackKnob>(
			mm2px(Vec(10.16f, 88.89f)), module, Meter::NUMERATOR_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(
			mm2px(Vec(20.32f, 88.89f)), module, Meter::DENOMINATOR_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, 101.59f)), module, Meter::NUMERATOR_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(20.32f, 101.59f)), module, Meter::DENOMINATOR_INPUT));

		// Bottom row (y=121.92): RUN button + RUN gate, RST button, RESET out
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(10.01f, 121.92f)), module, Meter::RUN_PARAM, Meter::RUN_LIGHT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(20.32f, 121.92f)), module, Meter::RUN_INPUT));
		addParam(createParamCentered<VCVButton>(
			mm2px(Vec(50.79f, 121.92f)), module, Meter::RESET_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(60.95f, 121.92f)), module, Meter::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(81.27f, 121.92f)), module, Meter::RESET_OUTPUT));

		// --- RIGHT COLUMN: 5 swing rows + BAR row ---
		// Per row: [trimpot] [swing CV] [swung] [grid]
		const float xTrimpot = 50.79f;
		const float xCV      = 60.95f;
		const float xSwung   = 71.11f;
		const float xGrid    = 81.27f;

		// 5 swingable subdivisions (Q, E, S, QT, ET) top-to-bottom
		const float swingRowYs[5] = { 60.95f, 71.11f, 81.27f, 91.43f, 101.59f };
		const int swingSubIds[5] = {
			SUB_QUARTER, SUB_EIGHTH, SUB_SIXTEENTH, SUB_QTRIP, SUB_ETRIP
		};
		const int gridOutIds[5] = {
			Meter::QUARTER_GRID_OUTPUT,
			Meter::EIGHTH_GRID_OUTPUT,
			Meter::SIXTEENTH_GRID_OUTPUT,
			Meter::QUARTER_TRIPLET_GRID_OUTPUT,
			Meter::EIGHTH_TRIPLET_GRID_OUTPUT
		};

		for (int i = 0; i < 5; i++) {
			int sub = swingSubIds[i];
			float y = swingRowYs[i];
			addParam(createParamCentered<Trimpot>(
				mm2px(Vec(xTrimpot, y)), module, Meter::SWING_PARAM_0 + sub));
			addInput(createInputCentered<PJ301MPort>(
				mm2px(Vec(xCV, y)), module, Meter::SWING_CV_0 + sub));
			addOutput(createOutputCentered<PJ301MPort>(
				mm2px(Vec(xSwung, y)), module, Meter::BAR_OUTPUT + sub));
			addOutput(createOutputCentered<PJ301MPort>(
				mm2px(Vec(xGrid, y)), module, gridOutIds[i]));
		}

		// BAR row (no swing): single output jack in the grid column
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(81.27f, 111.75f)), module, Meter::BAR_OUTPUT));
	}

	// Fill sends absolute time-signature CV; everything else is treated as an
	// additive offset. Detected from the cable graph on the UI thread — the
	// engine-side events (onPortChange, process) run under the engine lock and
	// can't safely query cables.
	// Matched by slug rather than the modelFill pointer so a dev-build Meter
	// (plugin slug "SignalFunctionSet-dev") still recognizes a stable-build
	// Fill and vice versa.
	bool inputComesFromFill(int portId) {
		for (CableWidget* cw : APP->scene->rack->getCablesOnPort(getInput(portId))) {
			if (!cw->cable || !cw->cable->outputModule)
				continue;
			Model* srcModel = cw->cable->outputModule->model;
			if (srcModel->slug == "Fill" && srcModel->plugin->slug.rfind("SignalFunctionSet", 0) == 0)
				return true;
		}
		return false;
	}

	void step() override {
		ModuleWidget::step();
		if (Meter* m = dynamic_cast<Meter*>(module)) {
			m->numCvFromFill = inputComesFromFill(Meter::NUMERATOR_INPUT);
			m->denCvFromFill = inputComesFromFill(Meter::DENOMINATOR_INPUT);
		}
	}

	void appendContextMenu(Menu* menu) override {
		Meter* module = dynamic_cast<Meter*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("External Clock PPQN"));
		for (int i = 0; i < NUM_PPQN_OPTIONS; i++) {
			int ppqn = PPQN_OPTIONS[i];
			int idx = i;
			menu->addChild(createCheckMenuItem(
				string::f("%d PPQN", ppqn), "",
				[=]() { return module->extClockPpqnIndex == idx; },
				[=]() { module->extClockPpqnIndex = idx; }
			));
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Behavior"));
		menu->addChild(createBoolPtrMenuItem(
			"Apply time signature changes immediately", "",
			&module->applyTimeSigImmediately));
		menu->addChild(createBoolPtrMenuItem(
			"Reset on play", "",
			&module->resetOnPlay));
		menu->addChild(createBoolPtrMenuItem(
			"BPM CV absolute (0.01V/BPM — for Arrange)", "",
			&module->bpmCvAbsolute));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Outputs"));
		// true == offer the duty-cycle gates: every output here has a known period.
		sfs::addPulseWidthMenu(menu, &module->pulseWidthIdx, "Gate/trigger width", true);

		if (module->extClockConnected && module->extClockHasMeasurement) {
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuLabel(
				string::f("Detected: %.1f BPM", module->displayedBpm)));
		} else if (module->extClockConnected && module->extPpqnSuspect) {
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuLabel("Clock is ticking, but not at this PPQN."));
			menu->addChild(createMenuLabel("MIDI clock is 24 PPQN. Meter is running"));
			menu->addChild(createMenuLabel("on its BPM knob until this matches."));
		}
	}
};


Model* modelMeter = createModel<Meter, MeterWidget>("Meter");
