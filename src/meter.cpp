#include "plugin.hpp"
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
	dsp::SchmittTrigger extClockTrigger;

	// --- Run state ---
	bool running = true;

	// --- External clock measurement ---
	int samplesSinceLastExtPulse = 0;
	int extClockPpqnIndex = 2;
	float measuredBpm = 120.f;
	float measuredBpmRaw = 120.f;
	bool extClockHasMeasurement = false;

	// --- Display state ---
	int displayedSixteenth = 0;
	float displayedBpm = 120.f;
	bool extClockConnected = false;
	int barsSinceReset = 0;       // Increments on each bar wrap; cleared on Reset
	float syncFlash = 0.f;        // Brightness of the sync indicator (decays)

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

	Meter() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

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

		configInput(BPM_INPUT, "BPM CV");
		configInput(NUMERATOR_INPUT, "Numerator CV");
		configInput(DENOMINATOR_INPUT, "Denominator CV");
		configInput(RUN_INPUT, "Run gate");
		configInput(EXT_CLOCK_INPUT, "External clock");

		for (int i = 0; i < NUM_OUTPUTS; i++) {
			configInput(SWING_CV_0 + i, string::f("%s swing CV", SUB_LABELS[i]));
		}

		configOutput(BAR_OUTPUT,                       "Bar");
		configOutput(QUARTER_OUTPUT,                   "Quarter note (swung)");
		configOutput(EIGHTH_OUTPUT,                    "Eighth note (swung)");
		configOutput(SIXTEENTH_OUTPUT,                 "Sixteenth note (swung)");
		configOutput(QUARTER_TRIPLET_OUTPUT,           "Quarter triplet (swung)");
		configOutput(EIGHTH_TRIPLET_OUTPUT,            "Eighth triplet (swung)");
		configOutput(RESET_OUTPUT,                     "Reset (fires when Reset button pressed)");
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
		// Fire all swung pulses on reset (downbeat). Grid pulses are NOT
		// fired here — they'll fire naturally one basePeriod after Reset
		// since their accumulators were just reset to 0. Force-firing grid
		// here would inject an extra CLOCK into modules clocked from a grid
		// output without a Reset cable.
		for (int i = 0; i < NUM_OUTPUTS; i++) {
			pulses[i].trigger(0.001f);
		}
		displayedSixteenth = 0;
		barsSinceReset = 0;
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

		// --- Reset (button only — Meter is the master clock; Reset OUT
		//     forwards to downstream modules like Beat) ---
		bool resetBtn = resetButtonTrigger.process(params[RESET_PARAM].getValue());
		if (resetBtn) {
			doReset();
			resetOutPulse.trigger(0.001f);
		}

		// --- Read CV-modulated parameters ---
		float bpmKnob = params[BPM_PARAM].getValue();
		if (inputs[BPM_INPUT].isConnected())
			bpmKnob += inputs[BPM_INPUT].getVoltage() * 27.f;
		bpmKnob = clamp(bpmKnob, 30.f, 300.f);

		int numKnob = (int)std::round(params[NUMERATOR_PARAM].getValue());
		if (inputs[NUMERATOR_INPUT].isConnected())
			numKnob += (int)std::round(inputs[NUMERATOR_INPUT].getVoltage() * 1.5f);
		numKnob = clamp(numKnob, 1, 16);

		int denIdx = (int)std::round(params[DENOMINATOR_PARAM].getValue());
		if (inputs[DENOMINATOR_INPUT].isConnected())
			denIdx += (int)std::round(inputs[DENOMINATOR_INPUT].getVoltage() * 0.5f);
		denIdx = clamp(denIdx, 0, NUM_DENOMS - 1);
		int denValue = DENOM_VALUES[denIdx];

		// --- External clock processing ---
		extClockConnected = inputs[EXT_CLOCK_INPUT].isConnected();
		if (extClockConnected) {
			if (extClockTrigger.process(inputs[EXT_CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (samplesSinceLastExtPulse > 0) {
					int ppqn = PPQN_OPTIONS[extClockPpqnIndex];
					float samplesPerQuarter = (float)samplesSinceLastExtPulse * (float)ppqn;
					float bpm = 60.f * args.sampleRate / samplesPerQuarter;
					measuredBpmRaw = clamp(bpm, 30.f, 300.f);
					extClockHasMeasurement = true;
				}
				samplesSinceLastExtPulse = 0;
				syncFlash = 1.f;  // Light up the sync indicator
			}
			samplesSinceLastExtPulse++;
			if (extClockHasMeasurement) {
				measuredBpm += (measuredBpmRaw - measuredBpm) * 0.1f;
			}
		} else {
			samplesSinceLastExtPulse = 0;
			extClockHasMeasurement = false;
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
			return;
		}

		// --- Compute per-subdivision base periods (samples per pulse, no swing) ---
		float samplesPerQuarter = 60.f * args.sampleRate / effectiveBpm;

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
		}
		lastSamplesPerQuarter = samplesPerQuarter;
		float basePeriods[NUM_OUTPUTS];
		basePeriods[SUB_BAR] = samplesPerQuarter * (float)sixteenthsPerBar / 4.f; // not used directly
		basePeriods[SUB_QUARTER] = samplesPerQuarter;
		basePeriods[SUB_EIGHTH] = samplesPerQuarter / 2.f;
		basePeriods[SUB_SIXTEENTH] = samplesPerQuarter / 4.f;
		basePeriods[SUB_QTRIP] = samplesPerQuarter / 3.f;
		basePeriods[SUB_ETRIP] = samplesPerQuarter / 6.f;

		// --- Advance per-subdivision sample counters ---
		samplesSinceQuarter += 1.f;
		samplesSinceEighth += 1.f;
		samplesSinceSixteenth += 1.f;
		samplesSinceQTrip += 1.f;
		samplesSinceETrip += 1.f;
		// Grid (un-swung) phase trackers tick the same way
		for (int i = 0; i < 5; i++) samplesSinceGrid[i] += 1.f;

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
				pulses_grid[i].trigger(0.001f);
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
		auto firePulse = [&](int outIdx) {
			pulses[outIdx].trigger(0.001f);
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
			firePulse(SUB_QUARTER);
		}

		// Eighth
		float eTarget = swingAdjustedPeriod(pulseCountEighth, basePeriods[SUB_EIGHTH], activeSwing[SUB_EIGHTH]);
		if (samplesSinceEighth >= eTarget) {
			samplesSinceEighth -= eTarget;
			pulseCountEighth++;
			firePulse(SUB_EIGHTH);
		}

		// Quarter triplet
		float qtTarget = swingAdjustedPeriod(pulseCountQTrip, basePeriods[SUB_QTRIP], activeSwing[SUB_QTRIP]);
		if (samplesSinceQTrip >= qtTarget) {
			samplesSinceQTrip -= qtTarget;
			pulseCountQTrip++;
			firePulse(SUB_QTRIP);
		}

		// Eighth triplet
		float etTarget = swingAdjustedPeriod(pulseCountETrip, basePeriods[SUB_ETRIP], activeSwing[SUB_ETRIP]);
		if (samplesSinceETrip >= etTarget) {
			samplesSinceETrip -= etTarget;
			pulseCountETrip++;
			firePulse(SUB_ETRIP);
		}

		// Sixteenth (drives bar tracking)
		float sTarget = swingAdjustedPeriod(pulseCountSixteenth, basePeriods[SUB_SIXTEENTH], activeSwing[SUB_SIXTEENTH]);
		if (samplesSinceSixteenth >= sTarget) {
			samplesSinceSixteenth -= sTarget;
			pulseCountSixteenth++;
			firePulse(SUB_SIXTEENTH);

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
				firePulse(SUB_BAR);
				// Reset triplet phases on bar boundary
				samplesSinceQTrip = 0.f;
				samplesSinceETrip = 0.f;
				pulseCountQTrip = 0;
				pulseCountETrip = 0;
				// Realign grid pulses with the bar boundary (also fires
				// the downbeat straight pulses, mirroring the swung set).
				for (int i = 0; i < 5; i++) {
					samplesSinceGrid[i] = 0.f;
					pulses_grid[i].trigger(0.001f);
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
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "running", json_boolean(running));
		json_object_set_new(rootJ, "extClockPpqnIndex", json_integer(extClockPpqnIndex));
		json_object_set_new(rootJ, "applyTimeSigImmediately", json_boolean(applyTimeSigImmediately));
		json_object_set_new(rootJ, "resetOnPlay", json_boolean(resetOnPlay));
		json_object_set_new(rootJ, "barsSinceReset", json_integer(barsSinceReset));
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
		json_t* bsrJ = json_object_get(rootJ, "barsSinceReset");
		if (bsrJ) barsSinceReset = (int)json_integer_value(bsrJ);
	}
};


// --- Display drawLayer ---

void MeterDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1 || !module) {
		Widget::drawLayer(args, layer);
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
		std::string bpmStr = string::f("%.1f BPM", module->displayedBpm);
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

		if (module->extClockConnected && module->extClockHasMeasurement) {
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuLabel(
				string::f("Detected: %.1f BPM", module->displayedBpm)));
		}
	}
};


Model* modelMeter = createModel<Meter, MeterWidget>("Meter");
