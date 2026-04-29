#include "plugin.hpp"
#include <cmath>


static const int NUM_PATTERNS = 8;
static const int MAX_STEPS = 16;


// Forward declaration
struct Beat;


struct BeatDisplay : OpaqueWidget {
	Beat* module = nullptr;
	std::shared_ptr<Font> font;

	// Cached layout rects (in widget-local pixel coords). Recomputed each draw.
	rack::math::Rect tabRect[4];          // STEPS / VEL / ACC / PROB
	rack::math::Rect stepGridRect;
	rack::math::Rect lengthDotRect[MAX_STEPS];  // 16 small dots
	rack::math::Rect repeatsRect[8];      // 8 cells: 1..MAX_REPEATS
	rack::math::Rect patternRect[NUM_PATTERNS];

	// Drag state
	enum DragKind {
		DRAG_NONE = 0,
		DRAG_STEP_PAINT,    // STEPS mode: paint cells with paintState
		DRAG_VEL,           // VEL mode: vertical drag adjusts velocity
		DRAG_ACC_PAINT,     // ACC mode: paint accents with paintState
		DRAG_LENGTH,        // Drag across length dots
		DRAG_PATTERN,       // Drag across pattern selector
		DRAG_REPEATS        // Drag across repeats bar
	};
	DragKind dragKind = DRAG_NONE;
	bool paintState = false;        // For STEP/ACC paint
	int dragStep = -1;              // For VEL mode: which step is being dragged
	float dragStartY = 0.f;
	float dragStartVel = 0.f;
	rack::math::Vec dragPos;        // Tracked widget-local cursor during drag

	void computeLayout();
	rack::math::Rect cellRectForStep(int step);
	int hitTestStep(rack::math::Vec p);
	int hitTestPattern(rack::math::Vec p);
	int hitTestTab(rack::math::Vec p);
	int hitTestLength(rack::math::Vec p);

	void onButton(const ButtonEvent& e) override;
	void onDoubleClick(const DoubleClickEvent& e) override;
	void onDragStart(const DragStartEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
	void onHoverScroll(const HoverScrollEvent& e) override;

	void drawLayer(const DrawArgs& args, int layer) override;
	void draw(const DrawArgs& args) override {
		// Background is drawn via SVG; only emissive layer matters here.
		OpaqueWidget::draw(args);
	}
};


struct Beat : Module {
	enum ParamId { PARAMS_LEN };
	enum InputId {
		CLOCK_INPUT,
		BAR_INPUT,
		RESET_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		GATE_OUTPUT,
		VELOCITY_OUTPUT,
		ACCENT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	enum EditMode {
		MODE_STEPS = 0,
		MODE_VEL,
		MODE_ACC,
		MODE_PROB,
		NUM_MODES
	};

	static const int MAX_REPEATS = 8;

	struct Pattern {
		bool steps[MAX_STEPS];
		float velocities[MAX_STEPS];
		bool accents[MAX_STEPS];
		float probabilities[MAX_STEPS];   // 0..1, chance the step actually fires
		int length;
		int repeats;       // Number of bars to play before advancing (1-8)
		bool active;

		Pattern() {
			for (int i = 0; i < MAX_STEPS; i++) {
				steps[i] = false;
				velocities[i] = 1.f;
				accents[i] = false;
				probabilities[i] = 1.f;
			}
			length = 16;
			repeats = 1;
			active = false;
		}
	};

	Pattern patterns[NUM_PATTERNS];
	int editPattern = 0;
	int playPattern = 0;
	int playStep = 0;
	int editMode = MODE_STEPS;
	int currentBar = 1;          // Which bar of the loop we are in (1..reps)
	float currentVelocity = 1.f;

	// True between Reset (or fresh start) and the next BAR-aligned downbeat.
	// Makes the first audible step 0 land on a real downbeat:
	//   - If BAR is connected, only a BAR pulse consumes the flag (CLOCKs
	//     in between are silently absorbed). This keeps step 0 musically
	//     aligned even if Reset lands mid-bar.
	//   - If BAR is not connected, the next CLOCK consumes the flag.
	// Without this, Reset fired step 0 off-clock, then the next CLOCK
	// advanced to step 1 and fired it; subsequently the BAR pulse re-fired
	// step 0 — the user heard step 0 "early" before the real bar 2 downbeat.
	// Default true at construction so a fresh Beat also waits for the
	// first real downbeat instead of skipping step 0 on the first CLOCK.
	bool firstClockPending = true;

	// When true, pattern only advances on a BAR pulse (default — most musical
	// behavior). When false, pattern wrap also advances when BAR isn't patched.
	bool advanceOnBarOnly = true;

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger barTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::PulseGenerator gatePulse;
	dsp::PulseGenerator accentPulse;

	// Bar/clock coincidence handling. CLOCK and BAR from Meter SHOULD arrive
	// on the same sample, but in practice can be off by a few samples. Two
	// guards keep step 0 from double-firing on bar boundaries:
	//   - pendingClockSamples: defer a CLOCK fire by N samples; if a BAR
	//     arrives within that window, it cancels the pending clock (so the
	//     BAR's advanceBar handles step 0 alone).
	//   - barSuppressionSamples: after a BAR fires, drop any CLOCK that
	//     arrives within the window (it's the same musical event).
	int pendingClockSamples = -1;
	int barSuppressionSamples = 0;
	static const int CLOCK_DEFER_SAMPLES = 24;    // ~0.5ms at 48kHz
	static const int BAR_SUPPRESS_SAMPLES = 96;   // ~2ms at 48kHz

	Beat() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configInput(CLOCK_INPUT, "Clock (step advance)");
		configInput(BAR_INPUT, "Bar (pattern advance)");
		configInput(RESET_INPUT, "Reset");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(VELOCITY_OUTPUT, "Velocity (0-10V)");
		configOutput(ACCENT_OUTPUT, "Accent");
		// Default: all patterns active. An "active empty" pattern is the
		// silent option; right-click a cell to skip it from the rotation.
		for (int p = 0; p < NUM_PATTERNS; p++) patterns[p].active = true;
		currentBar = 1;
	}

	void onReset() override {
		for (int p = 0; p < NUM_PATTERNS; p++) {
			patterns[p] = Pattern();
		}
		for (int p = 0; p < NUM_PATTERNS; p++) patterns[p].active = true;
		editPattern = 0;
		playPattern = 0;
		playStep = 0;
		editMode = MODE_STEPS;
		currentBar = 1;
		currentVelocity = 1.f;
		advanceOnBarOnly = true;
	}

	int firstActivePattern() {
		for (int i = 0; i < NUM_PATTERNS; i++) {
			if (patterns[i].active) return i;
		}
		return 0;
	}

	int nextActivePattern(int from) {
		for (int i = 1; i <= NUM_PATTERNS; i++) {
			int idx = (from + i) % NUM_PATTERNS;
			if (patterns[idx].active) return idx;
		}
		return from;
	}

	void fireStepIfActive() {
		const Pattern& p = patterns[playPattern];
		if (playStep < 0 || playStep >= p.length) return;
		if (!p.steps[playStep]) return;
		// Probability check — fires only if random draw is below the set
		// probability. p=1 always fires, p=0 never fires.
		if (random::uniform() >= p.probabilities[playStep]) return;
		gatePulse.trigger(0.001f);
		currentVelocity = clamp(p.velocities[playStep], 0.f, 1.f);
		if (p.accents[playStep]) accentPulse.trigger(0.001f);
	}

	void doReset() {
		playPattern = firstActivePattern();
		playStep = 0;
		currentBar = 1;
		// Park before step 0; the next CLOCK or BAR pulse will fire it.
		// Also clear any in-flight deferred clock so a stale CLOCK from
		// just before Reset doesn't fire after the reset state is set.
		firstClockPending = true;
		pendingClockSamples = -1;
		barSuppressionSamples = 0;
	}

	void process(const ProcessArgs& args) override {
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			doReset();
		}

		bool barConnected = inputs[BAR_INPUT].isConnected();

		auto advanceBar = [&]() {
			if (firstClockPending) {
				// First bar after Reset — sit on step 0 instead of
				// incrementing past it.
				firstClockPending = false;
				playStep = 0;
				fireStepIfActive();
				return;
			}
			currentBar++;
			if (currentBar > patterns[playPattern].repeats) {
				playPattern = nextActivePattern(playPattern);
				currentBar = 1;
			}
			playStep = 0;
			fireStepIfActive();
		};

		auto advanceStep = [&]() {
			if (firstClockPending) {
				// Waiting for downbeat. If BAR is patched, silently absorb
				// this CLOCK — only a BAR pulse should fire step 0, so it
				// lands on the actual downbeat. If BAR isn't patched, fire
				// step 0 on this CLOCK.
				if (barConnected) return;
				firstClockPending = false;
				playStep = 0;
				fireStepIfActive();
				return;
			}
			int len = patterns[playPattern].length;
			if (len < 1) len = 1;
			int nextStep = playStep + 1;
			if (nextStep >= len && !barConnected && !advanceOnBarOnly) {
				// Legacy fallback: when BAR isn't patched and the user has
				// disabled "advance only on BAR", treat pattern wrap as the
				// bar boundary so the module is usable with just a clock.
				advanceBar();
			} else {
				playStep = nextStep % len;
				fireStepIfActive();
			}
		};

		bool barFired = barConnected
			&& barTrigger.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f);
		if (barFired) {
			// Cancel any deferred clock — the BAR is the canonical event.
			pendingClockSamples = -1;
			barSuppressionSamples = BAR_SUPPRESS_SAMPLES;
			advanceBar();
		}

		bool clockFired = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		if (clockFired && !barFired) {
			if (barConnected) {
				if (barSuppressionSamples > 0) {
					// CLOCK in post-BAR window — same musical event, drop.
				} else {
					// Defer the CLOCK fire so a BAR arriving in the next
					// few samples can cancel it.
					pendingClockSamples = CLOCK_DEFER_SAMPLES;
				}
			} else {
				// No BAR cable — fire CLOCK immediately.
				advanceStep();
			}
		}
		if (pendingClockSamples > 0) {
			pendingClockSamples--;
			if (pendingClockSamples == 0) {
				advanceStep();
				pendingClockSamples = -1;
			}
		}
		if (barSuppressionSamples > 0) barSuppressionSamples--;

		bool gateHi = gatePulse.process(args.sampleTime);
		bool accHi = accentPulse.process(args.sampleTime);

		outputs[GATE_OUTPUT].setVoltage(gateHi ? 10.f : 0.f);
		outputs[VELOCITY_OUTPUT].setVoltage(currentVelocity * 10.f);
		outputs[ACCENT_OUTPUT].setVoltage(accHi ? 10.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "editPattern", json_integer(editPattern));
		json_object_set_new(root, "editMode", json_integer(editMode));
		json_object_set_new(root, "playPattern", json_integer(playPattern));
		json_object_set_new(root, "playStep", json_integer(playStep));
		json_object_set_new(root, "currentBar", json_integer(currentBar));
		json_object_set_new(root, "advanceOnBarOnly", json_boolean(advanceOnBarOnly));

		json_t* patArray = json_array();
		for (int p = 0; p < NUM_PATTERNS; p++) {
			json_t* patObj = json_object();
			json_object_set_new(patObj, "active", json_boolean(patterns[p].active));
			json_object_set_new(patObj, "length", json_integer(patterns[p].length));
			json_object_set_new(patObj, "repeats", json_integer(patterns[p].repeats));
			json_t* stepsArr = json_array();
			json_t* velsArr = json_array();
			json_t* accsArr = json_array();
			json_t* probsArr = json_array();
			for (int s = 0; s < MAX_STEPS; s++) {
				json_array_append_new(stepsArr, json_boolean(patterns[p].steps[s]));
				json_array_append_new(velsArr, json_real(patterns[p].velocities[s]));
				json_array_append_new(accsArr, json_boolean(patterns[p].accents[s]));
				json_array_append_new(probsArr, json_real(patterns[p].probabilities[s]));
			}
			json_object_set_new(patObj, "steps", stepsArr);
			json_object_set_new(patObj, "velocities", velsArr);
			json_object_set_new(patObj, "accents", accsArr);
			json_object_set_new(patObj, "probabilities", probsArr);
			json_array_append_new(patArray, patObj);
		}
		json_object_set_new(root, "patterns", patArray);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "editPattern"))
			editPattern = clamp((int)json_integer_value(j), 0, NUM_PATTERNS - 1);
		if (json_t* j = json_object_get(root, "editMode"))
			editMode = clamp((int)json_integer_value(j), 0, NUM_MODES - 1);
		if (json_t* j = json_object_get(root, "playPattern"))
			playPattern = clamp((int)json_integer_value(j), 0, NUM_PATTERNS - 1);
		if (json_t* j = json_object_get(root, "playStep"))
			playStep = clamp((int)json_integer_value(j), 0, MAX_STEPS - 1);
		if (json_t* j = json_object_get(root, "currentBar"))
			currentBar = clamp((int)json_integer_value(j), 1, MAX_REPEATS);
		if (json_t* j = json_object_get(root, "advanceOnBarOnly"))
			advanceOnBarOnly = json_boolean_value(j);

		json_t* patArray = json_object_get(root, "patterns");
		if (patArray && json_is_array(patArray)) {
			for (int p = 0; p < NUM_PATTERNS; p++) {
				json_t* patObj = json_array_get(patArray, p);
				if (!patObj) continue;
				if (json_t* j = json_object_get(patObj, "active"))
					patterns[p].active = json_boolean_value(j);
				if (json_t* j = json_object_get(patObj, "length"))
					patterns[p].length = clamp((int)json_integer_value(j), 1, MAX_STEPS);
				if (json_t* j = json_object_get(patObj, "repeats"))
					patterns[p].repeats = clamp((int)json_integer_value(j), 1, MAX_REPEATS);
				if (json_t* arr = json_object_get(patObj, "steps")) {
					for (int s = 0; s < MAX_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].steps[s] = json_boolean_value(v);
					}
				}
				if (json_t* arr = json_object_get(patObj, "velocities")) {
					for (int s = 0; s < MAX_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].velocities[s] = clamp((float)json_real_value(v), 0.f, 1.f);
					}
				}
				if (json_t* arr = json_object_get(patObj, "accents")) {
					for (int s = 0; s < MAX_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].accents[s] = json_boolean_value(v);
					}
				}
				if (json_t* arr = json_object_get(patObj, "probabilities")) {
					for (int s = 0; s < MAX_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].probabilities[s] = clamp((float)json_real_value(v), 0.f, 1.f);
					}
				}
			}
		}
	}
};


// --- BeatDisplay implementation ---

// Mockup uses a 174 × 155 unit display (= 46 mm × 41 mm). Convert mockup
// units → widget pixels by multiplying by `s = w / 174`.

void BeatDisplay::computeLayout() {
	float w = box.size.x;
	float s = w / 174.f;

	// 4 mode tabs (STEPS / VEL / ACC / PROB) of (38 x 18) at x = 7, 47, 87, 127, y = 8
	for (int i = 0; i < 4; i++) {
		tabRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 40.f) * s, 8.f * s),
			rack::math::Vec(38.f * s, 18.f * s));
	}

	// Step grid: 2 rows × 8 cols of (18 x 18) cells, x = 7, 27, 47, ..., 147
	stepGridRect = rack::math::Rect(
		rack::math::Vec(7.f * s, 35.f * s),
		rack::math::Vec(158.f * s, 38.f * s));

	// Length dots: 16 cells of (8 x 8) at x = 7, 17, ..., 157, y = 75
	for (int i = 0; i < MAX_STEPS; i++) {
		lengthDotRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 10.f) * s, 75.f * s),
			rack::math::Vec(8.f * s, 8.f * s));
	}

	// Pattern selector: 8 cells of (18 x 18) at x = 7, 27, ..., 147, y = 111
	for (int i = 0; i < NUM_PATTERNS; i++) {
		patternRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 20.f) * s, 111.f * s),
			rack::math::Vec(18.f * s, 18.f * s));
	}

	// Repeats bar: 8 cells of (18 x 8) at x = 7, 27, ..., 147, y = 137
	for (int i = 0; i < 8; i++) {
		repeatsRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 20.f) * s, 137.f * s),
			rack::math::Vec(18.f * s, 8.f * s));
	}
}

rack::math::Rect BeatDisplay::cellRectForStep(int step) {
	float w = box.size.x;
	float s = w / 174.f;
	int row = step / 8;
	int col = step % 8;
	return rack::math::Rect(
		rack::math::Vec((7.f + col * 20.f) * s, (35.f + row * 20.f) * s),
		rack::math::Vec(18.f * s, 18.f * s));
}

int BeatDisplay::hitTestStep(rack::math::Vec p) {
	for (int i = 0; i < MAX_STEPS; i++) {
		if (cellRectForStep(i).contains(p)) return i;
	}
	return -1;
}

int BeatDisplay::hitTestPattern(rack::math::Vec p) {
	for (int i = 0; i < NUM_PATTERNS; i++) {
		if (patternRect[i].contains(p)) return i;
	}
	return -1;
}

int BeatDisplay::hitTestTab(rack::math::Vec p) {
	for (int i = 0; i < 4; i++) {
		if (tabRect[i].contains(p)) return i;
	}
	return -1;
}

// Returns 0..15 if a length dot was hit, else -1.
int BeatDisplay::hitTestLength(rack::math::Vec p) {
	for (int i = 0; i < MAX_STEPS; i++) {
		if (lengthDotRect[i].contains(p)) return i;
	}
	return -1;
}

static int hitTestRepeats(BeatDisplay* d, rack::math::Vec p) {
	for (int i = 0; i < 8; i++) {
		if (d->repeatsRect[i].contains(p)) return i;
	}
	return -1;
}

void BeatDisplay::onButton(const ButtonEvent& e) {
	if (!module) {
		OpaqueWidget::onButton(e);
		return;
	}
	if (e.action != GLFW_PRESS) {
		OpaqueWidget::onButton(e);
		return;
	}
	computeLayout();
	rack::math::Vec p = e.pos;

	if (e.button != GLFW_MOUSE_BUTTON_LEFT) {
		OpaqueWidget::onButton(e);
		return;
	}

	// Track cursor so onDragMove can hit-test absolute positions.
	dragPos = p;

	// Tabs: switch edit mode
	int tabIdx = hitTestTab(p);
	if (tabIdx >= 0) {
		module->editMode = tabIdx;
		e.consume(this);
		return;
	}

	// Pattern selector: select for edit (drag across cells to scrub)
	int patIdx = hitTestPattern(p);
	if (patIdx >= 0) {
		module->editPattern = patIdx;
		dragKind = DRAG_PATTERN;
		e.consume(this);
		return;
	}

	// Repeats bar: click cell N → set edit pattern's repeats to N+1
	// (drag across cells to scrub)
	int repIdx = hitTestRepeats(this, p);
	if (repIdx >= 0) {
		int newReps = repIdx + 1;
		module->patterns[module->editPattern].repeats = newReps;
		if (module->editPattern == module->playPattern
			&& module->currentBar > newReps) {
			module->currentBar = newReps;
		}
		dragKind = DRAG_REPEATS;
		e.consume(this);
		return;
	}

	// Length dot: click cell N → set length to N+1 (drag to scrub)
	int lenIdx = hitTestLength(p);
	if (lenIdx >= 0) {
		module->patterns[module->editPattern].length = lenIdx + 1;
		dragKind = DRAG_LENGTH;
		e.consume(this);
		return;
	}

	// Step grid: behavior depends on mode
	int step = hitTestStep(p);
	if (step >= 0) {
		Beat::Pattern& pat = module->patterns[module->editPattern];
		if (step >= pat.length) {
			// Click beyond current length: extend length to include this step
			pat.length = step + 1;
		}
		switch (module->editMode) {
			case Beat::MODE_STEPS:
				pat.steps[step] = !pat.steps[step];
				// Drag from here paints subsequent cells with this new state
				dragKind = DRAG_STEP_PAINT;
				paintState = pat.steps[step];
				break;
			case Beat::MODE_VEL: {
				// Click sets velocity based on Y within cell; drag continues
				dragStep = step;
				dragKind = DRAG_VEL;
				rack::math::Rect cr = cellRectForStep(step);
				float relY = (p.y - cr.pos.y) / cr.size.y;
				float vel = clamp(1.f - relY, 0.f, 1.f);
				pat.velocities[step] = vel;
				if (!pat.steps[step]) pat.steps[step] = true;
				dragStartY = p.y;
				dragStartVel = vel;
				break;
			}
			case Beat::MODE_ACC:
				pat.accents[step] = !pat.accents[step];
				if (pat.accents[step] && !pat.steps[step]) pat.steps[step] = true;
				dragKind = DRAG_ACC_PAINT;
				paintState = pat.accents[step];
				break;
			case Beat::MODE_PROB: {
				// Same vertical-drag behavior as VEL (reuses DRAG_VEL kind)
				dragStep = step;
				dragKind = DRAG_VEL;
				rack::math::Rect cr = cellRectForStep(step);
				float relY = (p.y - cr.pos.y) / cr.size.y;
				float val = clamp(1.f - relY, 0.f, 1.f);
				pat.probabilities[step] = val;
				if (!pat.steps[step]) pat.steps[step] = true;
				dragStartY = p.y;
				dragStartVel = val;
				break;
			}
		}
		e.consume(this);
		return;
	}

	OpaqueWidget::onButton(e);
}

void BeatDisplay::onDoubleClick(const DoubleClickEvent& e) {
	if (!module) return;
	// VCV's DoubleClickEvent has no position; reuse dragPos which onButton
	// updates on every press, so it has the most recent click location.
	int patIdx = hitTestPattern(dragPos);
	if (patIdx >= 0) {
		module->patterns[patIdx].active = !module->patterns[patIdx].active;
		// If we deactivated the playing pattern, jump to next active
		if (!module->patterns[module->playPattern].active) {
			module->playPattern = module->nextActivePattern(module->playPattern);
			module->playStep = 0;
		}
		e.consume(this);
		return;
	}
	OpaqueWidget::onDoubleClick(e);
}

void BeatDisplay::onDragStart(const DragStartEvent& e) {
	OpaqueWidget::onDragStart(e);
}

void BeatDisplay::onDragMove(const DragMoveEvent& e) {
	if (!module) return;
	float zoom = getAbsoluteZoom();
	if (zoom <= 0.f) zoom = 1.f;
	rack::math::Vec delta = e.mouseDelta.div(zoom);
	dragPos = dragPos.plus(delta);

	switch (dragKind) {
		case DRAG_VEL: {
			if (dragStep >= 0) {
				rack::math::Rect cr = cellRectForStep(dragStep);
				float deltaVal = -delta.y / cr.size.y;
				Beat::Pattern& pat = module->patterns[module->editPattern];
				// Dispatches between velocity / probability based on mode
				float* target = (module->editMode == Beat::MODE_PROB)
					? &pat.probabilities[dragStep]
					: &pat.velocities[dragStep];
				*target = clamp(*target + deltaVal, 0.f, 1.f);
			}
			break;
		}
		case DRAG_STEP_PAINT: {
			int step = hitTestStep(dragPos);
			if (step >= 0) {
				Beat::Pattern& pat = module->patterns[module->editPattern];
				if (step < pat.length) {
					pat.steps[step] = paintState;
				}
			}
			break;
		}
		case DRAG_ACC_PAINT: {
			int step = hitTestStep(dragPos);
			if (step >= 0) {
				Beat::Pattern& pat = module->patterns[module->editPattern];
				if (step < pat.length) {
					pat.accents[step] = paintState;
					if (paintState && !pat.steps[step]) pat.steps[step] = true;
				}
			}
			break;
		}
		case DRAG_LENGTH: {
			int idx = hitTestLength(dragPos);
			if (idx >= 0) {
				module->patterns[module->editPattern].length = idx + 1;
			}
			break;
		}
		case DRAG_PATTERN: {
			int idx = hitTestPattern(dragPos);
			if (idx >= 0) {
				module->editPattern = idx;
			}
			break;
		}
		case DRAG_REPEATS: {
			int idx = hitTestRepeats(this, dragPos);
			if (idx >= 0) {
				int newReps = idx + 1;
				module->patterns[module->editPattern].repeats = newReps;
				if (module->editPattern == module->playPattern
					&& module->currentBar > newReps) {
					module->currentBar = newReps;
				}
			}
			break;
		}
		case DRAG_NONE:
		default:
			break;
	}

	OpaqueWidget::onDragMove(e);
}

void BeatDisplay::onDragEnd(const DragEndEvent& e) {
	dragKind = DRAG_NONE;
	dragStep = -1;
	OpaqueWidget::onDragEnd(e);
}

void BeatDisplay::onHoverScroll(const HoverScrollEvent& e) {
	if (!module) return;
	computeLayout();
	int patIdx = hitTestPattern(e.pos);
	if (patIdx >= 0) {
		int delta = (e.scrollDelta.y > 0.f) ? 1 : -1;
		int& r = module->patterns[patIdx].repeats;
		r = clamp(r + delta, 1, Beat::MAX_REPEATS);
		if (patIdx == module->playPattern && module->currentBar > r) {
			module->currentBar = r;
		}
		e.consume(this);
		return;
	}
	OpaqueWidget::onHoverScroll(e);
}


void BeatDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1 || !module) {
		OpaqueWidget::drawLayer(args, layer);
		return;
	}

	computeLayout();

	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	// --- Mockup palette ---
	const NVGcolor COL_BLUE       = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);  // primary active
	const NVGcolor COL_BLUE_DARK  = nvgRGBA(0x0D, 0x59, 0x86, 0xFF);  // selected mode tab bg
	const NVGcolor COL_BLUE_LINE  = nvgRGBA(0x0D, 0x59, 0x88, 0xFF);  // connector rails
	const NVGcolor COL_PURPLE     = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);  // inactive cell
	const NVGcolor COL_PURPLE_MID = nvgRGBA(0x4A, 0x4A, 0x66, 0xFF);  // beat-group accent / dim active
	const NVGcolor COL_PURPLE_DK  = nvgRGBA(0x1A, 0x1A, 0x32, 0xFF);  // out of length / pattern indicator
	const NVGcolor COL_ORANGE     = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);  // currently playing
	const NVGcolor COL_TEXT_DIM   = nvgRGBA(0x80, 0x80, 0x80, 0xFF);
	const NVGcolor COL_TEXT_BRIGHT = nvgRGBA(0xFF, 0xFF, 0xFF, 0xFF);
	const NVGcolor COL_HINT       = nvgRGBA(0xFF, 0xFF, 0xFF, 26);    // ~10% white

	const Beat::Pattern& editPat = module->patterns[module->editPattern];
	bool isPlayingPattern = (module->editPattern == module->playPattern);

	float w = box.size.x;
	float s = w / 174.f;  // mockup-unit → pixel scale, used for stroke widths etc.

	// --- Mode tabs (4): STEPS / VEL / ACC / PROB ---
	const char* tabLabels[4] = { "STEPS", "VEL", "ACC", "PROB" };
	for (int i = 0; i < 4; i++) {
		bool active = (module->editMode == i);
		NVGcolor bg = active ? COL_BLUE_DARK : COL_PURPLE;
		NVGcolor fg = active ? COL_TEXT_BRIGHT : COL_TEXT_DIM;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, tabRect[i].pos.x, tabRect[i].pos.y,
			tabRect[i].size.x, tabRect[i].size.y, 2.f * s);
		nvgFillColor(args.vg, bg);
		nvgFill(args.vg);
		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 9.f * s);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, fg);
			nvgText(args.vg,
				tabRect[i].pos.x + tabRect[i].size.x * 0.5f,
				tabRect[i].pos.y + tabRect[i].size.y * 0.5f,
				tabLabels[i], NULL);
		}
	}

	// --- Step grid ---
	for (int idx = 0; idx < MAX_STEPS; idx++) {
		rack::math::Rect cr = cellRectForStep(idx);
		bool inLen = (idx < editPat.length);
		bool stepOn = editPat.steps[idx];
		bool isCurrent = isPlayingPattern && (idx == module->playStep);
		bool beatStart = (idx % 4 == 0);

		// Base cell color
		NVGcolor cellBg;
		if (!inLen) cellBg = COL_PURPLE_DK;
		else if (stepOn) cellBg = isCurrent ? COL_ORANGE : COL_BLUE;
		else cellBg = beatStart ? COL_PURPLE_MID : COL_PURPLE;

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, cr.pos.x, cr.pos.y,
			cr.size.x, cr.size.y, 2.f * s);
		nvgFillColor(args.vg, cellBg);
		nvgFill(args.vg);

		if (!inLen) continue;

		// Velocity overlay (bottom-up white). Bright in VEL mode, faint hint
		// in STEPS / ACC modes. Skipped entirely in PROB mode (the PROB
		// overlay below takes over).
		if (stepOn && module->editMode != Beat::MODE_PROB) {
			float v = clamp(editPat.velocities[idx], 0.f, 1.f);
			float overlayH = cr.size.y * v;
			NVGcolor velColor = (module->editMode == Beat::MODE_VEL)
				? nvgRGBA(255, 255, 255, 153)   // 60% white in VEL mode
				: nvgRGBA(255, 255, 255, 26);   // ~10% white as hint elsewhere
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg,
				cr.pos.x, cr.pos.y + cr.size.y - overlayH,
				cr.size.x, overlayH, 2.f * s);
			nvgFillColor(args.vg, velColor);
			nvgFill(args.vg);
		}

		// Probability overlay (bottom-up white) — only in PROB mode.
		if (stepOn && module->editMode == Beat::MODE_PROB) {
			float prob = clamp(editPat.probabilities[idx], 0.f, 1.f);
			float overlayH = cr.size.y * prob;
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg,
				cr.pos.x, cr.pos.y + cr.size.y - overlayH,
				cr.size.x, overlayH, 2.f * s);
			nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 153));
			nvgFill(args.vg);
		}

		// Accent ring (unfilled white circle). Full opacity in ACC mode,
		// 20% as a hint in STEPS / VEL modes.
		if (stepOn && editPat.accents[idx]) {
			float r = std::min(cr.size.x, cr.size.y) * 0.39f;  // r=3.5 of 9 mockup units
			NVGcolor accColor = (module->editMode == Beat::MODE_ACC)
				? COL_TEXT_BRIGHT
				: COL_HINT;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg,
				cr.pos.x + cr.size.x * 0.5f,
				cr.pos.y + cr.size.y * 0.5f, r);
			nvgStrokeColor(args.vg, accColor);
			nvgStrokeWidth(args.vg, 1.f * s);
			nvgStroke(args.vg);
		}

		// Playhead on the current step (when it's not already audibly active).
		if (isCurrent && !stepOn) {
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg,
				cr.pos.x + 0.5f, cr.pos.y + 0.5f,
				cr.size.x - 1.f, cr.size.y - 1.f, 2.f * s);
			nvgStrokeColor(args.vg, COL_ORANGE);
			nvgStrokeWidth(args.vg, 1.f);
			nvgStroke(args.vg);
		}
	}

	// --- Length dots (16) ---
	for (int i = 0; i < MAX_STEPS; i++) {
		const rack::math::Rect& dr = lengthDotRect[i];
		bool inLen = (i < editPat.length);
		NVGcolor c = inLen ? COL_BLUE : COL_PURPLE;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, dr.pos.x, dr.pos.y,
			dr.size.x, dr.size.y, 2.f * s);
		nvgFillColor(args.vg, c);
		nvgFill(args.vg);
	}

	// --- Pattern selector ---
	for (int p = 0; p < NUM_PATTERNS; p++) {
		const rack::math::Rect& pr = patternRect[p];
		bool active = module->patterns[p].active;
		bool isEdit = (p == module->editPattern);
		bool isPlay = (p == module->playPattern);

		NVGcolor bg;
		if (!active) bg = COL_PURPLE_DK;
		else if (isPlay) bg = COL_ORANGE;
		else if (isEdit) bg = COL_BLUE;
		else bg = COL_PURPLE_MID;

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, pr.pos.x, pr.pos.y, pr.size.x, pr.size.y, 2.f * s);
		nvgFillColor(args.vg, bg);
		nvgFill(args.vg);

		// Edit-pattern highlight outline (thinner, dimmer than before)
		if (isEdit && !isPlay) {
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg,
				pr.pos.x + 0.5f, pr.pos.y + 0.5f,
				pr.size.x - 1.f, pr.size.y - 1.f, 2.f * s);
			nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 160));
			nvgStrokeWidth(args.vg, 0.6f);
			nvgStroke(args.vg);
		}

		// Pattern number — slightly above center to leave room for the dots
		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 9.f * s);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			NVGcolor tc = active ? COL_TEXT_BRIGHT : COL_TEXT_DIM;
			nvgFillColor(args.vg, tc);
			std::string lbl = string::f("%d", p + 1);
			nvgText(args.vg,
				pr.pos.x + pr.size.x * 0.5f,
				pr.pos.y + pr.size.y * 0.42f,
				lbl.c_str(), NULL);
		}

		// Repeat-count dots: one dot per loop, centered horizontally.
		// Dim = total loop count marker, bright = current playhead position
		// (only on the playing pattern).
		{
			int reps = module->patterns[p].repeats;
			float dotR = 0.7f * s;
			float dotSpacing = 1.9f * s;
			float totalW = (reps - 1) * dotSpacing;
			float startX = pr.pos.x + pr.size.x * 0.5f - totalW * 0.5f;
			float dotY = pr.pos.y + pr.size.y - 3.f * s;

			int playheadIdx = isPlay ? clamp(module->currentBar - 1, 0, reps - 1) : -1;
			NVGcolor dimCol    = nvgRGBA(255, 255, 255, 90);
			NVGcolor brightCol = COL_TEXT_BRIGHT;

			for (int d = 0; d < reps; d++) {
				bool isPlayhead = (d == playheadIdx);
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, startX + d * dotSpacing, dotY, dotR);
				nvgFillColor(args.vg, isPlayhead ? brightCol : dimCol);
				nvgFill(args.vg);
			}
		}
	}

	// --- Repeats bar ---
	{
		int editReps = editPat.repeats;
		int currentBarIdx = isPlayingPattern
			? clamp(module->currentBar - 1, 0, editReps - 1)
			: -1;

		for (int i = 0; i < 8; i++) {
			const rack::math::Rect& rr = repeatsRect[i];
			bool inRange = (i < editReps);
			bool isCurrent = (i == currentBarIdx);
			NVGcolor c = isCurrent ? COL_ORANGE
				: inRange ? COL_BLUE
				: COL_PURPLE;
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, rr.pos.x, rr.pos.y, rr.size.x, rr.size.y, 2.f * s);
			nvgFillColor(args.vg, c);
			nvgFill(args.vg);
		}
	}

	// --- Active-state connector rails ---
	// Coordinates from the mockup are panel-absolute; the display starts at
	// panel y=45, so relative y = mockup y - 45.
	// Top rail mockup y=77   → rel y=32   (between mode tabs and step grid)
	// Bottom rail mockup y=179.5 → rel y=134.5 (between pattern selector and repeats)
	{
		nvgStrokeColor(args.vg, COL_BLUE_LINE);
		nvgStrokeWidth(args.vg, 1.f);

		// Top rail
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 7.f * s, 32.f * s);
		nvgLineTo(args.vg, 165.f * s, 32.f * s);
		nvgStroke(args.vg);

		// Stem: from active mode tab center down to (just past) the rail
		float topStemX = (7.f + module->editMode * 40.f + 19.f) * s;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, topStemX, 28.f * s);
		nvgLineTo(args.vg, topStemX, 32.5f * s);
		nvgStroke(args.vg);

		// Bottom rail
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 7.f * s, 134.5f * s);
		nvgLineTo(args.vg, 165.f * s, 134.5f * s);
		nvgStroke(args.vg);

		// Stem: from edit-pattern center down to (just past) the rail
		float botStemX = (7.f + module->editPattern * 20.f + 9.f) * s;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, botStemX, 130.5f * s);
		nvgLineTo(args.vg, botStemX, 135.f * s);
		nvgStroke(args.vg);
	}

	// --- "PATTERN" section label between length dots and pattern selector
	// (mockup baseline y=148 → rel y=103) ---
	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 9.f * s);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFillColor(args.vg, COL_TEXT_BRIGHT);
		nvgText(args.vg, 8.f * s, 103.f * s, "PATTERN", NULL);
	}

	OpaqueWidget::drawLayer(args, layer);
}


// --- Widget ---

struct BeatWidget : ModuleWidget {
	BeatWidget(Beat* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/beat.svg")));


		// Display: x=2.4, y=12.2, w=46, h=41 (per updated Beat SVG)
		BeatDisplay* display = new BeatDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(2.4f, 12.2f));
		display->box.size = mm2px(Vec(46.f, 41.f));
		addChild(display);

		// Inputs (LEFT column at x=10.16mm) — top→bottom: RESET, BAR, CLOCK
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, 91.45f)),  module, Beat::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, 106.68f)), module, Beat::BAR_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, 121.92f)), module, Beat::CLOCK_INPUT));

		// Outputs (RIGHT column at x=40.64mm on dark plate) — top→bottom: VEL, ACC, GATE
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(40.64f, 91.45f)),  module, Beat::VELOCITY_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(40.64f, 106.68f)), module, Beat::ACCENT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(40.64f, 121.92f)), module, Beat::GATE_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Beat* module = dynamic_cast<Beat*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem(
			"Advance only on bar trigger", "",
			&module->advanceOnBarOnly));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Patterns"));
		menu->addChild(createMenuItem("Randomize current pattern steps", "",
			[=]() {
				Beat::Pattern& p = module->patterns[module->editPattern];
				for (int i = 0; i < MAX_STEPS; i++) {
					p.steps[i] = (random::uniform() < 0.5f);
				}
			}));
		menu->addChild(createMenuItem("Clear current pattern", "",
			[=]() {
				Beat::Pattern& p = module->patterns[module->editPattern];
				for (int i = 0; i < MAX_STEPS; i++) {
					p.steps[i] = false;
					p.accents[i] = false;
					p.velocities[i] = 1.f;
					p.probabilities[i] = 1.f;
				}
			}));
		menu->addChild(createMenuItem("Clear all patterns", "",
			[=]() {
				for (int p = 0; p < NUM_PATTERNS; p++) {
					module->patterns[p] = Beat::Pattern();
					module->patterns[p].active = true;
				}
			}));
	}
};


Model* modelBeat = createModel<Beat, BeatWidget>("Beat");
