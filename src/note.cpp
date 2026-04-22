#include "plugin.hpp"
#include <cmath>


static const int N_PATTERNS = 8;
static const int N_STEPS    = 8;
// Matrix rows: max scale size (12) + 1 extra row at the top showing root +1oct
static const int N_ROWS     = 13;
static const int N_REPEATS  = 8;


// --- Scale definitions ---
// Each scale lists semitones above the root for each row index. Harmonic
// series uses just-intonation log2(n)*12 for harmonic n=1..12.
struct ScaleDef {
	const char* name;
	int size;
	float semitones[16];
};

static const ScaleDef SCALES[] = {
	{"Chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
	{"Major",     7,  {0, 2, 4, 5, 7, 9, 11}},
	{"Minor",     7,  {0, 2, 3, 5, 7, 8, 10}},
	{"Penta+",    5,  {0, 2, 4, 7, 9}},
	{"Penta-",    5,  {0, 3, 5, 7, 10}},
	{"Blues",     6,  {0, 3, 5, 6, 7, 10}},
	{"Whole",     6,  {0, 2, 4, 6, 8, 10}},
	{"Harmonic", 12, {
		0.f,
		12.f,
		19.0196f,
		24.f,
		27.8631f,
		31.0196f,
		33.6883f,
		36.f,
		38.0392f,
		39.8632f,
		41.5126f,
		43.0196f
	}},
	// --- Modes ---
	{"Dorian",    7, {0, 2, 3, 5, 7, 9, 10}},
	{"Phrygian",  7, {0, 1, 3, 5, 7, 8, 10}},
	{"Lydian",    7, {0, 2, 4, 6, 7, 9, 11}},
	{"Mixolyd",   7, {0, 2, 4, 5, 7, 9, 10}},
	// --- Western exotic ---
	{"HarmMin",   7, {0, 2, 3, 5, 7, 8, 11}},
	// --- World ---
	{"Hijaz",     7, {0, 1, 4, 5, 7, 8, 10}},
	{"Hirajoshi", 5, {0, 2, 3, 7, 8}},
	// Pelog: non-12-TET approximation of a Surakarta-style tuning so it's
	// distinct from Phrygian. Cents: 0, 120, 270, 540, 700, 800, 1040.
	{"Pelog",     7, {0.f, 1.2f, 2.7f, 5.4f, 7.0f, 8.0f, 10.4f}},
	// Slendro: 5 equal divisions of the octave (non-12-TET, ~2.4 semis each).
	{"Slendro",   5, {0.f, 2.4f, 4.8f, 7.2f, 9.6f}},
};
static const int NUM_SCALES = sizeof(SCALES) / sizeof(SCALES[0]);

static const char* NOTE_NAMES[12] = {
	"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};


struct Note;


struct NoteDisplay : OpaqueWidget {
	Note* module = nullptr;
	std::shared_ptr<Font> font;

	// Cached layout rects in widget pixels (recomputed each draw)
	rack::math::Rect rootRect;
	rack::math::Rect scaleRect;
	rack::math::Rect octRect;
	rack::math::Rect tabRect[4];
	rack::math::Rect matrixRect;
	rack::math::Rect lengthDotRect[N_STEPS];
	rack::math::Rect patternRect[N_PATTERNS];
	rack::math::Rect repeatsRect[N_REPEATS];

	// Drag state
	enum DragKind {
		DRAG_NONE = 0,
		DRAG_PITCH_PAINT,    // STEPS mode: paint same pitch into other steps
		DRAG_VEL,            // VEL/PROB mode: vertical drag changes value
		DRAG_ACC_PAINT,      // ACC mode: paint accents
		DRAG_LENGTH,
		DRAG_PATTERN,
		DRAG_REPEATS
	};
	DragKind dragKind = DRAG_NONE;
	int dragRow = -1;        // For STEPS paint: row to paint
	int dragStep = -1;       // For VEL/PROB drag: which column we're editing
	bool paintAccent = false;
	rack::math::Vec dragPos;

	void computeLayout();
	rack::math::Rect cellRectFor(int col, int row);
	int hitTestMatrixCol(rack::math::Vec p);
	int hitTestMatrixRow(rack::math::Vec p);
	int hitTestPattern(rack::math::Vec p);
	int hitTestTab(rack::math::Vec p);
	int hitTestLength(rack::math::Vec p);
	int hitTestRepeats(rack::math::Vec p);
	int hitTestStatus(rack::math::Vec p);   // 0=root, 1=scale, 2=oct, -1=miss

	void onButton(const ButtonEvent& e) override;
	void onDoubleClick(const DoubleClickEvent& e) override;
	void onDragStart(const DragStartEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
	void onHoverScroll(const HoverScrollEvent& e) override;

	void drawLayer(const DrawArgs& args, int layer) override;
	void draw(const DrawArgs& args) override { OpaqueWidget::draw(args); }
};


struct Note : Module {
	enum ParamId {
		ROOT_PARAM, SCALE_PARAM, OCT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT, BAR_INPUT, RESET_INPUT,
		ROOT_INPUT, SCALE_INPUT, OCT_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		GATE_OUTPUT, VOCT_OUTPUT, VELOCITY_OUTPUT, ACCENT_OUTPUT,
		ROOT_OUTPUT, SCALE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	enum EditMode {
		MODE_STEPS = 0, MODE_VEL, MODE_ACC, MODE_PROB,
		NUM_MODES
	};

	struct Pattern {
		int   pitches[N_STEPS];        // -1 = rest, else 0..(scale-1)
		float velocities[N_STEPS];
		bool  accents[N_STEPS];
		float probabilities[N_STEPS];
		int   length;                  // 1..N_STEPS
		int   repeats;                 // 1..N_REPEATS
		bool  active;
		Pattern() {
			for (int i = 0; i < N_STEPS; i++) {
				pitches[i] = -1;
				velocities[i] = 1.f;
				accents[i] = false;
				probabilities[i] = 1.f;
			}
			length = N_STEPS;
			repeats = 1;
			active = false;
		}
	};

	Pattern patterns[N_PATTERNS];
	int editPattern = 0;
	int playPattern = 0;
	int playStep = 0;
	int currentBar = 1;
	int editMode = MODE_STEPS;

	int rootNote = 0;          // 0..11 semitones (C=0)
	int scaleIndex = 0;        // 0..NUM_SCALES-1
	int octaveShift = 0;       // -2..+2

	float currentVelocity = 1.f;
	float currentVoct = 0.f;
	bool advanceOnBarOnly = true;

	dsp::SchmittTrigger clockTrigger, barTrigger, resetTrigger;
	dsp::PulseGenerator gatePulse, accentPulse;

	// Bar/clock coincidence handling (mirrors Beat)
	int pendingClockSamples = -1;
	int barSuppressionSamples = 0;
	static const int CLOCK_DEFER_SAMPLES = 24;
	static const int BAR_SUPPRESS_SAMPLES = 96;

	struct OctaveQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			return string::f("%+d", (int)std::round(getValue()));
		}
	};

	Note() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// Named values for ROOT (note names) and SCALE (scale names)
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root note", {
			"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
		});
		configSwitch(SCALE_PARAM, 0.f, (float)(NUM_SCALES - 1), 0.f, "Scale", {
			"Chromatic", "Major", "Minor",
			"Pentatonic Major", "Pentatonic Minor",
			"Blues", "Whole tone", "Harmonic series",
			"Dorian", "Phrygian", "Lydian", "Mixolydian",
			"Harmonic Minor", "Hijaz (Arabic)",
			"Hirajoshi (Japanese)",
			"Pelog (Gamelan, 7-tone)",
			"Slendro (Gamelan, 5-equal)"
		});
		configParam<OctaveQuantity>(OCT_PARAM, -4.f, 4.f, 0.f, "Octave shift");
		paramQuantities[OCT_PARAM]->snapEnabled = true;
		configInput(CLOCK_INPUT, "Clock (step advance)");
		configInput(BAR_INPUT,   "Bar (pattern advance)");
		configInput(RESET_INPUT, "Reset");
		configInput(ROOT_INPUT,  "Root note CV (1V/oct, semitone-quantized)");
		configInput(SCALE_INPUT, "Scale select CV (1V/scale)");
		configInput(OCT_INPUT,   "Octave shift CV (1V/oct)");
		configOutput(GATE_OUTPUT,     "Gate");
		configOutput(VOCT_OUTPUT,     "1V/octave pitch");
		configOutput(VELOCITY_OUTPUT, "Velocity (0-10V)");
		configOutput(ACCENT_OUTPUT,   "Accent");
		configOutput(ROOT_OUTPUT,     "Root (relays current root)");
		configOutput(SCALE_OUTPUT,    "Scale (relays current scale index)");
		// Default: all 8 patterns active so cycling works out of the box
		for (int p = 0; p < N_PATTERNS; p++) patterns[p].active = true;
	}

	void onReset() override {
		for (int p = 0; p < N_PATTERNS; p++) patterns[p] = Pattern();
		for (int p = 0; p < N_PATTERNS; p++) patterns[p].active = true;
		editPattern = 0;
		playPattern = 0;
		playStep = 0;
		currentBar = 1;
		editMode = MODE_STEPS;
		rootNote = 0;
		scaleIndex = 0;
		octaveShift = 0;
		currentVelocity = 1.f;
		currentVoct = 0.f;
		advanceOnBarOnly = true;
		pendingClockSamples = -1;
		barSuppressionSamples = 0;
		params[ROOT_PARAM].setValue(0.f);
		params[SCALE_PARAM].setValue(0.f);
		params[OCT_PARAM].setValue(0.f);
	}

	int firstActivePattern() {
		for (int i = 0; i < N_PATTERNS; i++) if (patterns[i].active) return i;
		return 0;
	}

	int nextActivePattern(int from) {
		for (int i = 1; i <= N_PATTERNS; i++) {
			int idx = (from + i) % N_PATTERNS;
			if (patterns[idx].active) return idx;
		}
		return from;
	}

	int currentScaleSize() const { return SCALES[scaleIndex].size; }
	// Number of selectable rows per scale = scale size + 1 (extra octave row at top)
	int currentRowCount() const { return SCALES[scaleIndex].size + 1; }

	// V/oct (relative to C0=0) for a given matrix row in the current scale.
	// The extra "octave" row (row == scaleSize) returns root + 12 semis.
	float voctForRow(int row) const {
		int sz = currentScaleSize();
		if (row < 0 || row > sz) return 0.f;
		float semis = (row == sz) ? 12.f : SCALES[scaleIndex].semitones[row];
		semis += (float)rootNote;
		semis += (float)octaveShift * 12.f;
		return semis / 12.f;
	}

	void fireStepIfActive() {
		const Pattern& p = patterns[playPattern];
		if (playStep < 0 || playStep >= p.length) return;
		int pitch = p.pitches[playStep];
		if (pitch < 0) return;                              // rest
		if (pitch >= currentRowCount()) return;             // out of scale (incl. octave row)
		if (random::uniform() >= p.probabilities[playStep]) return;
		gatePulse.trigger(0.001f);
		currentVelocity = clamp(p.velocities[playStep], 0.f, 1.f);
		currentVoct = voctForRow(pitch);
		if (p.accents[playStep]) accentPulse.trigger(0.001f);
	}

	void doReset() {
		playPattern = firstActivePattern();
		playStep = 0;
		currentBar = 1;
		fireStepIfActive();
	}

	void process(const ProcessArgs& args) override {
		// ROOT, SCALE, OCT come from trimpots, optionally summed with CV.
		int rootK = (int)std::round(params[ROOT_PARAM].getValue());
		int scaleK = (int)std::round(params[SCALE_PARAM].getValue());
		int octK   = (int)std::round(params[OCT_PARAM].getValue());

		int rootCV = inputs[ROOT_INPUT].isConnected()
			? (int)std::round(inputs[ROOT_INPUT].getVoltage() * 12.f) : 0;
		int scaleCV = inputs[SCALE_INPUT].isConnected()
			? (int)std::round(inputs[SCALE_INPUT].getVoltage()) : 0;
		int octCV = inputs[OCT_INPUT].isConnected()
			? (int)std::round(inputs[OCT_INPUT].getVoltage()) : 0;

		rootNote    = (((rootK + rootCV) % 12) + 12) % 12;
		scaleIndex  = clamp(scaleK + scaleCV, 0, NUM_SCALES - 1);
		octaveShift = clamp(octK + octCV, -4, 4);

		// Reset
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			doReset();
		}

		bool barConnected = inputs[BAR_INPUT].isConnected();

		auto advanceBar = [&]() {
			currentBar++;
			if (currentBar > patterns[playPattern].repeats) {
				playPattern = nextActivePattern(playPattern);
				currentBar = 1;
			}
			playStep = 0;
			fireStepIfActive();
		};

		auto advanceStep = [&]() {
			int len = patterns[playPattern].length;
			if (len < 1) len = 1;
			int nextStep = playStep + 1;
			if (nextStep >= len && !barConnected && !advanceOnBarOnly) {
				advanceBar();
			} else {
				playStep = nextStep % len;
				fireStepIfActive();
			}
		};

		bool barFired = barConnected
			&& barTrigger.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f);
		if (barFired) {
			pendingClockSamples = -1;
			barSuppressionSamples = BAR_SUPPRESS_SAMPLES;
			advanceBar();
		}

		bool clockFired = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		if (clockFired && !barFired) {
			if (barConnected) {
				if (barSuppressionSamples > 0) {
					// CLOCK in post-BAR window — same musical event, drop
				} else {
					pendingClockSamples = CLOCK_DEFER_SAMPLES;
				}
			} else {
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
		bool accHi  = accentPulse.process(args.sampleTime);

		outputs[GATE_OUTPUT].setVoltage(gateHi ? 10.f : 0.f);
		outputs[VELOCITY_OUTPUT].setVoltage(currentVelocity * 10.f);
		outputs[ACCENT_OUTPUT].setVoltage(accHi ? 10.f : 0.f);
		outputs[VOCT_OUTPUT].setVoltage(currentVoct);

		// Relays (for chaining)
		outputs[ROOT_OUTPUT].setVoltage((float)rootNote / 12.f);
		outputs[SCALE_OUTPUT].setVoltage((float)scaleIndex);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "editPattern", json_integer(editPattern));
		json_object_set_new(root, "editMode", json_integer(editMode));
		json_object_set_new(root, "playPattern", json_integer(playPattern));
		json_object_set_new(root, "playStep", json_integer(playStep));
		json_object_set_new(root, "currentBar", json_integer(currentBar));
		json_object_set_new(root, "rootNote", json_integer(rootNote));
		json_object_set_new(root, "scaleIndex", json_integer(scaleIndex));
		json_object_set_new(root, "octaveShift", json_integer(octaveShift));
		json_object_set_new(root, "advanceOnBarOnly", json_boolean(advanceOnBarOnly));

		json_t* patArray = json_array();
		for (int p = 0; p < N_PATTERNS; p++) {
			json_t* po = json_object();
			json_object_set_new(po, "active", json_boolean(patterns[p].active));
			json_object_set_new(po, "length", json_integer(patterns[p].length));
			json_object_set_new(po, "repeats", json_integer(patterns[p].repeats));
			json_t* pitchArr = json_array();
			json_t* velArr = json_array();
			json_t* accArr = json_array();
			json_t* probArr = json_array();
			for (int s = 0; s < N_STEPS; s++) {
				json_array_append_new(pitchArr, json_integer(patterns[p].pitches[s]));
				json_array_append_new(velArr,  json_real(patterns[p].velocities[s]));
				json_array_append_new(accArr,  json_boolean(patterns[p].accents[s]));
				json_array_append_new(probArr, json_real(patterns[p].probabilities[s]));
			}
			json_object_set_new(po, "pitches", pitchArr);
			json_object_set_new(po, "velocities", velArr);
			json_object_set_new(po, "accents", accArr);
			json_object_set_new(po, "probabilities", probArr);
			json_array_append_new(patArray, po);
		}
		json_object_set_new(root, "patterns", patArray);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "editPattern"))
			editPattern = clamp((int)json_integer_value(j), 0, N_PATTERNS - 1);
		if (json_t* j = json_object_get(root, "editMode"))
			editMode = clamp((int)json_integer_value(j), 0, NUM_MODES - 1);
		if (json_t* j = json_object_get(root, "playPattern"))
			playPattern = clamp((int)json_integer_value(j), 0, N_PATTERNS - 1);
		if (json_t* j = json_object_get(root, "playStep"))
			playStep = clamp((int)json_integer_value(j), 0, N_STEPS - 1);
		if (json_t* j = json_object_get(root, "currentBar"))
			currentBar = clamp((int)json_integer_value(j), 1, N_REPEATS);
		if (json_t* j = json_object_get(root, "rootNote"))
			rootNote = clamp((int)json_integer_value(j), 0, 11);
		if (json_t* j = json_object_get(root, "scaleIndex"))
			scaleIndex = clamp((int)json_integer_value(j), 0, NUM_SCALES - 1);
		if (json_t* j = json_object_get(root, "octaveShift"))
			octaveShift = clamp((int)json_integer_value(j), -4, 4);
		if (json_t* j = json_object_get(root, "advanceOnBarOnly"))
			advanceOnBarOnly = json_boolean_value(j);

		json_t* patArray = json_object_get(root, "patterns");
		if (patArray && json_is_array(patArray)) {
			for (int p = 0; p < N_PATTERNS; p++) {
				json_t* po = json_array_get(patArray, p);
				if (!po) continue;
				if (json_t* j = json_object_get(po, "active"))
					patterns[p].active = json_boolean_value(j);
				if (json_t* j = json_object_get(po, "length"))
					patterns[p].length = clamp((int)json_integer_value(j), 1, N_STEPS);
				if (json_t* j = json_object_get(po, "repeats"))
					patterns[p].repeats = clamp((int)json_integer_value(j), 1, N_REPEATS);
				if (json_t* arr = json_object_get(po, "pitches")) {
					for (int s = 0; s < N_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].pitches[s] = clamp((int)json_integer_value(v), -1, N_ROWS - 1);
					}
				}
				if (json_t* arr = json_object_get(po, "velocities")) {
					for (int s = 0; s < N_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].velocities[s] = clamp((float)json_real_value(v), 0.f, 1.f);
					}
				}
				if (json_t* arr = json_object_get(po, "accents")) {
					for (int s = 0; s < N_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].accents[s] = json_boolean_value(v);
					}
				}
				if (json_t* arr = json_object_get(po, "probabilities")) {
					for (int s = 0; s < N_STEPS; s++) {
						if (json_t* v = json_array_get(arr, s))
							patterns[p].probabilities[s] = clamp((float)json_real_value(v), 0.f, 1.f);
					}
				}
			}
		}
	}
};


// --- NoteDisplay implementation ---

// Mockup units: 174 wide × 227 tall (= 46mm × 60mm). All layout is computed
// in those units and scaled to widget pixels by s = w / 174.
// Tab row matches Beat (y=8..26) for visual alignment.

void NoteDisplay::computeLayout() {
	float w = box.size.x;
	float s = w / 174.f;

	// Mode tabs (4 × 38×18 at x=7+i*40, y=8) — same as Beat
	for (int i = 0; i < 4; i++) {
		tabRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 40.f) * s, 8.f * s),
			rack::math::Vec(38.f * s, 18.f * s));
	}

	// Matrix: 13 rows × 8 cols. Cells 18 wide × 9 tall, at x=7+c*20, y=35+r*9
	// Top rail at y=32; matrix starts y=35.
	matrixRect = rack::math::Rect(
		rack::math::Vec(7.f * s, 35.f * s),
		rack::math::Vec(158.f * s, 117.f * s));   // 13 rows × 9

	// Length dots (8 × 18×8 at x=7+i*20, y=156) — wider to align with pattern columns
	for (int i = 0; i < N_STEPS; i++) {
		lengthDotRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 20.f) * s, 156.f * s),
			rack::math::Vec(18.f * s, 8.f * s));
	}

	// PATTERN label + status row. Cells extend up from the PATTERN baseline
	// so all text labels share the same baseline (y=184).
	rootRect  = rack::math::Rect(rack::math::Vec(72.f * s, 172.f * s),
		rack::math::Vec(22.f * s, 14.f * s));
	scaleRect = rack::math::Rect(rack::math::Vec(96.f * s, 172.f * s),
		rack::math::Vec(48.f * s, 14.f * s));
	octRect   = rack::math::Rect(rack::math::Vec(146.f * s, 172.f * s),
		rack::math::Vec(22.f * s, 14.f * s));

	// Pattern selector (8 × 18×18 at x=7+i*20, y=190)
	for (int i = 0; i < N_PATTERNS; i++) {
		patternRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 20.f) * s, 190.f * s),
			rack::math::Vec(18.f * s, 18.f * s));
	}

	// Repeats bar (8 × 18×8 at x=7+i*20, y=216)
	for (int i = 0; i < N_REPEATS; i++) {
		repeatsRect[i] = rack::math::Rect(
			rack::math::Vec((7.f + i * 20.f) * s, 216.f * s),
			rack::math::Vec(18.f * s, 8.f * s));
	}
}

rack::math::Rect NoteDisplay::cellRectFor(int col, int row) {
	// Visual row 0 = top of matrix; pitch row 0 (= root) = bottom of matrix.
	float w = box.size.x;
	float s = w / 174.f;
	int displayRow = (N_ROWS - 1) - row;
	return rack::math::Rect(
		rack::math::Vec((7.f + col * 20.f) * s, (35.f + displayRow * 9.f) * s),
		rack::math::Vec(18.f * s, 9.f * s));
}

int NoteDisplay::hitTestMatrixCol(rack::math::Vec p) {
	if (!matrixRect.contains(p)) return -1;
	float relX = p.x - matrixRect.pos.x;
	int col = (int)(relX / (matrixRect.size.x / (float)N_STEPS));
	return clamp(col, 0, N_STEPS - 1);
}

int NoteDisplay::hitTestMatrixRow(rack::math::Vec p) {
	if (!matrixRect.contains(p)) return -1;
	float relY = p.y - matrixRect.pos.y;
	int displayRow = (int)(relY / (matrixRect.size.y / (float)N_ROWS));
	displayRow = clamp(displayRow, 0, N_ROWS - 1);
	return (N_ROWS - 1) - displayRow;  // flip back to pitch row
}

int NoteDisplay::hitTestPattern(rack::math::Vec p) {
	for (int i = 0; i < N_PATTERNS; i++)
		if (patternRect[i].contains(p)) return i;
	return -1;
}

int NoteDisplay::hitTestTab(rack::math::Vec p) {
	for (int i = 0; i < 4; i++)
		if (tabRect[i].contains(p)) return i;
	return -1;
}

int NoteDisplay::hitTestLength(rack::math::Vec p) {
	for (int i = 0; i < N_STEPS; i++)
		if (lengthDotRect[i].contains(p)) return i;
	return -1;
}

int NoteDisplay::hitTestRepeats(rack::math::Vec p) {
	for (int i = 0; i < N_REPEATS; i++)
		if (repeatsRect[i].contains(p)) return i;
	return -1;
}

int NoteDisplay::hitTestStatus(rack::math::Vec p) {
	if (rootRect.contains(p))  return 0;
	if (scaleRect.contains(p)) return 1;
	if (octRect.contains(p))   return 2;
	return -1;
}


void NoteDisplay::onButton(const ButtonEvent& e) {
	if (!module) { OpaqueWidget::onButton(e); return; }
	if (e.action != GLFW_PRESS) { OpaqueWidget::onButton(e); return; }
	if (e.button != GLFW_MOUSE_BUTTON_LEFT) { OpaqueWidget::onButton(e); return; }

	computeLayout();
	rack::math::Vec p = e.pos;
	dragPos = p;

	// (Status cells are now display-only; ROOT/SCALE/OCT are edited via the
	// panel trimpots and CV inputs.)

	// Tabs
	int tabIdx = hitTestTab(p);
	if (tabIdx >= 0) {
		module->editMode = tabIdx;
		e.consume(this);
		return;
	}

	// Pattern selector (drag scrubs)
	int patIdx = hitTestPattern(p);
	if (patIdx >= 0) {
		module->editPattern = patIdx;
		dragKind = DRAG_PATTERN;
		e.consume(this);
		return;
	}

	// Repeats
	int repIdx = hitTestRepeats(p);
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

	// Length dots
	int lenIdx = hitTestLength(p);
	if (lenIdx >= 0) {
		module->patterns[module->editPattern].length = lenIdx + 1;
		dragKind = DRAG_LENGTH;
		e.consume(this);
		return;
	}

	// Matrix
	int col = hitTestMatrixCol(p);
	int row = hitTestMatrixRow(p);
	if (col >= 0) {
		Note::Pattern& pat = module->patterns[module->editPattern];
		bool needsRow = (module->editMode == Note::MODE_STEPS
		              || module->editMode == Note::MODE_ACC);
		// STEPS / ACC require an in-scale row; VEL / PROB act per column
		if (needsRow && (row < 0 || row >= module->currentRowCount())) {
			OpaqueWidget::onButton(e);
			return;
		}
		switch (module->editMode) {
			case Note::MODE_STEPS: {
				if (pat.pitches[col] == row) {
					pat.pitches[col] = -1;
					dragRow = -1;
				} else {
					pat.pitches[col] = row;
					dragRow = row;
				}
				dragKind = DRAG_PITCH_PAINT;
				break;
			}
			case Note::MODE_ACC: {
				pat.accents[col] = !pat.accents[col];
				if (pat.accents[col] && pat.pitches[col] < 0) pat.pitches[col] = row;
				dragKind = DRAG_ACC_PAINT;
				paintAccent = pat.accents[col];
				break;
			}
			case Note::MODE_VEL: {
				// Column fader — just set velocity by Y; pitch is unchanged
				dragStep = col;
				dragKind = DRAG_VEL;
				float relY = (p.y - matrixRect.pos.y) / matrixRect.size.y;
				pat.velocities[col] = clamp(1.f - relY, 0.f, 1.f);
				break;
			}
			case Note::MODE_PROB: {
				dragStep = col;
				dragKind = DRAG_VEL;
				float relY = (p.y - matrixRect.pos.y) / matrixRect.size.y;
				pat.probabilities[col] = clamp(1.f - relY, 0.f, 1.f);
				break;
			}
		}
		e.consume(this);
		return;
	}

	OpaqueWidget::onButton(e);
}

void NoteDisplay::onDoubleClick(const DoubleClickEvent& e) {
	if (!module) return;
	int patIdx = hitTestPattern(dragPos);
	if (patIdx >= 0) {
		module->patterns[patIdx].active = !module->patterns[patIdx].active;
		if (!module->patterns[module->playPattern].active) {
			module->playPattern = module->nextActivePattern(module->playPattern);
			module->playStep = 0;
		}
		e.consume(this);
		return;
	}
	OpaqueWidget::onDoubleClick(e);
}

void NoteDisplay::onDragStart(const DragStartEvent& e) {
	OpaqueWidget::onDragStart(e);
}

void NoteDisplay::onDragMove(const DragMoveEvent& e) {
	if (!module) return;
	float zoom = getAbsoluteZoom();
	if (zoom <= 0.f) zoom = 1.f;
	rack::math::Vec delta = e.mouseDelta.div(zoom);
	dragPos = dragPos.plus(delta);

	switch (dragKind) {
		case DRAG_VEL: {
			if (dragStep >= 0 && matrixRect.size.y > 0.f) {
				float deltaVal = -delta.y / matrixRect.size.y;
				Note::Pattern& pat = module->patterns[module->editPattern];
				float* target = (module->editMode == Note::MODE_PROB)
					? &pat.probabilities[dragStep]
					: &pat.velocities[dragStep];
				*target = clamp(*target + deltaVal, 0.f, 1.f);
			}
			break;
		}
		case DRAG_PITCH_PAINT: {
			int col = hitTestMatrixCol(dragPos);
			if (col >= 0 && col < module->patterns[module->editPattern].length) {
				module->patterns[module->editPattern].pitches[col] = dragRow;
			}
			break;
		}
		case DRAG_ACC_PAINT: {
			int col = hitTestMatrixCol(dragPos);
			if (col >= 0 && col < module->patterns[module->editPattern].length) {
				Note::Pattern& pat = module->patterns[module->editPattern];
				pat.accents[col] = paintAccent;
				if (paintAccent && pat.pitches[col] < 0) {
					int row = hitTestMatrixRow(dragPos);
					if (row >= 0) pat.pitches[col] = row;
				}
			}
			break;
		}
		case DRAG_LENGTH: {
			int idx = hitTestLength(dragPos);
			if (idx >= 0)
				module->patterns[module->editPattern].length = idx + 1;
			break;
		}
		case DRAG_PATTERN: {
			int idx = hitTestPattern(dragPos);
			if (idx >= 0) module->editPattern = idx;
			break;
		}
		case DRAG_REPEATS: {
			int idx = hitTestRepeats(dragPos);
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
		default: break;
	}
	OpaqueWidget::onDragMove(e);
}

void NoteDisplay::onDragEnd(const DragEndEvent& e) {
	dragKind = DRAG_NONE;
	dragStep = -1;
	dragRow = -1;
	OpaqueWidget::onDragEnd(e);
}

void NoteDisplay::onHoverScroll(const HoverScrollEvent& e) {
	if (!module) return;
	computeLayout();
	int delta = (e.scrollDelta.y > 0.f) ? 1 : -1;

	int patIdx = hitTestPattern(e.pos);
	if (patIdx >= 0) {
		int& r = module->patterns[patIdx].repeats;
		r = clamp(r + delta, 1, N_REPEATS);
		if (patIdx == module->playPattern && module->currentBar > r) {
			module->currentBar = r;
		}
		e.consume(this);
		return;
	}

	OpaqueWidget::onHoverScroll(e);
}


void NoteDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1 || !module) {
		OpaqueWidget::drawLayer(args, layer);
		return;
	}
	computeLayout();
	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	// Palette (matches Beat / Meter)
	const NVGcolor COL_BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_BLUE_DARK   = nvgRGBA(0x0D, 0x59, 0x86, 0xFF);
	const NVGcolor COL_BLUE_LINE   = nvgRGBA(0x0D, 0x59, 0x88, 0xFF);
	const NVGcolor COL_PURPLE      = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
	const NVGcolor COL_PURPLE_MID  = nvgRGBA(0x4A, 0x4A, 0x66, 0xFF);
	const NVGcolor COL_PURPLE_DK   = nvgRGBA(0x1A, 0x1A, 0x32, 0xFF);
	const NVGcolor COL_ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_TEXT_BRIGHT = nvgRGBA(0xFF, 0xFF, 0xFF, 0xFF);
	const NVGcolor COL_TEXT_DIM    = nvgRGBA(0x80, 0x80, 0x80, 0xFF);

	float w = box.size.x;
	float s = w / 174.f;

	const Note::Pattern& editPat = module->patterns[module->editPattern];
	bool isPlayingPattern = (module->editPattern == module->playPattern);

	// --- Mode tabs ---
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

	// --- Connector rails ---
	{
		nvgStrokeColor(args.vg, COL_BLUE_LINE);
		nvgStrokeWidth(args.vg, 1.f);
		// Top rail (between tabs and matrix) — matrix starts y=35
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 7.f * s, 32.f * s);
		nvgLineTo(args.vg, 165.f * s, 32.f * s);
		nvgStroke(args.vg);
		float topStemX = (7.f + module->editMode * 40.f + 19.f) * s;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, topStemX, 28.f * s);
		nvgLineTo(args.vg, topStemX, 32.5f * s);
		nvgStroke(args.vg);
		// Bottom rail (between pattern selector and repeats bar)
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 7.f * s, 213.f * s);
		nvgLineTo(args.vg, 165.f * s, 213.f * s);
		nvgStroke(args.vg);
		float botStemX = (7.f + module->editPattern * 20.f + 9.f) * s;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, botStemX, 209.f * s);
		nvgLineTo(args.vg, botStemX, 213.5f * s);
		nvgStroke(args.vg);
	}

	// --- Matrix (13 rows × 8 cols) ---
	// Layout per pitch row: row 0 = root (bottom), row scaleSize = root+1oct
	// (top of scale), rows above = out of scale.
	int scaleSize = module->currentScaleSize();
	bool stepsOrAcc = (module->editMode == Note::MODE_STEPS
	                || module->editMode == Note::MODE_ACC);
	bool valueMode  = (module->editMode == Note::MODE_VEL
	                || module->editMode == Note::MODE_PROB);

	for (int col = 0; col < N_STEPS; col++) {
		bool inLen = (col < editPat.length);
		bool isCurrentCol = isPlayingPattern && (col == module->playStep);
		int litRow = editPat.pitches[col];

		float colX = (7.f + col * 20.f) * s;
		float colW = 18.f * s;
		float colTop = 35.f * s;
		float colH = 117.f * s;

		if (valueMode) {
			// --- Column-bar view for VEL / PROB ---
			// Background column
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, colX, colTop, colW, colH, 2.f * s);
			nvgFillColor(args.vg, inLen ? COL_PURPLE : COL_PURPLE_DK);
			nvgFill(args.vg);

			// Value bar from bottom up
			if (inLen) {
				float val = (module->editMode == Note::MODE_PROB)
					? clamp(editPat.probabilities[col], 0.f, 1.f)
					: clamp(editPat.velocities[col], 0.f, 1.f);
				float barH = colH * val;
				NVGcolor barCol;
				if (litRow < 0) {
					// Rest step: shows stored value but muted (no audible note)
					barCol = COL_PURPLE_MID;
				} else if (isCurrentCol) {
					barCol = COL_ORANGE;
				} else {
					barCol = COL_BLUE;
				}
				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, colX, colTop + colH - barH,
					colW, barH, 2.f * s);
				nvgFillColor(args.vg, barCol);
				nvgFill(args.vg);
			}

			// Pitch indicator: thin white horizontal mark at the lit row's y
			if (inLen && litRow >= 0) {
				rack::math::Rect cr = cellRectFor(col, litRow);
				float markY = cr.pos.y + cr.size.y * 0.5f;
				nvgBeginPath(args.vg);
				nvgRect(args.vg, colX + 1.f, markY - 0.5f, colW - 2.f, 1.f);
				nvgFillColor(args.vg, COL_TEXT_BRIGHT);
				nvgFill(args.vg);
			}
		}
		else if (stepsOrAcc) {
			// --- Pitch matrix view ---
			for (int row = 0; row < N_ROWS; row++) {
				rack::math::Rect cr = cellRectFor(col, row);
				bool isOctaveRow = (row == scaleSize);
				bool inScale = (row < scaleSize);
				bool isLit = (litRow == row);

				NVGcolor cellBg;
				if (!inLen)         cellBg = COL_PURPLE_DK;
				else if (isLit)     cellBg = isCurrentCol ? COL_ORANGE : COL_BLUE;
				else if (row == 0 || isOctaveRow) cellBg = COL_PURPLE_MID;
				else if (inScale)   cellBg = COL_PURPLE;
				else                cellBg = COL_PURPLE_DK;

				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, cr.pos.x + 0.5f, cr.pos.y + 0.5f,
					cr.size.x - 1.f, cr.size.y - 1.f, 1.f * s);
				nvgFillColor(args.vg, cellBg);
				nvgFill(args.vg);
			}
		}

		// Accent ring for active step in this column
		if (inLen && litRow >= 0 && editPat.accents[col]) {
			rack::math::Rect cr = cellRectFor(col, litRow);
			NVGcolor accColor = (module->editMode == Note::MODE_ACC)
				? COL_TEXT_BRIGHT
				: nvgRGBA(255, 255, 255, 60);
			float r = std::min(cr.size.x, cr.size.y) * 0.45f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg,
				cr.pos.x + cr.size.x * 0.5f,
				cr.pos.y + cr.size.y * 0.5f, r);
			nvgStrokeColor(args.vg, accColor);
			nvgStrokeWidth(args.vg, 1.f);
			nvgStroke(args.vg);
		}

		// Playhead column outline when playing column has no lit pitch
		if (isCurrentCol && inLen && litRow < 0) {
			float colX = (7.f + col * 20.f) * s;
			float colTop = 35.f * s;
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, colX + 0.5f, colTop + 0.5f,
				18.f * s - 1.f, 117.f * s - 1.f, 1.5f * s);
			nvgStrokeColor(args.vg, COL_ORANGE);
			nvgStrokeWidth(args.vg, 1.f);
			nvgStroke(args.vg);
		}
	}

	// --- Length dots ---
	for (int i = 0; i < N_STEPS; i++) {
		const rack::math::Rect& dr = lengthDotRect[i];
		bool inLen = (i < editPat.length);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, dr.pos.x, dr.pos.y, dr.size.x, dr.size.y, 2.f * s);
		nvgFillColor(args.vg, inLen ? COL_BLUE : COL_PURPLE);
		nvgFill(args.vg);
	}

	// --- "PATTERN" label + ROOT / SCALE / OCT status cells ---
	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 9.f * s);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFillColor(args.vg, COL_TEXT_BRIGHT);
		nvgText(args.vg, 8.f * s, 184.f * s, "PATTERN", NULL);
	}
	{
		const rack::math::Rect* statRects[3] = { &rootRect, &scaleRect, &octRect };
		std::string statValues[3] = {
			std::string(NOTE_NAMES[module->rootNote]),
			std::string(SCALES[module->scaleIndex].name),
			string::f("%+d", module->octaveShift)
		};
		for (int i = 0; i < 3; i++) {
			const rack::math::Rect& r = *statRects[i];
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 1.5f * s);
			nvgFillColor(args.vg, COL_PURPLE_DK);
			nvgFill(args.vg);
			if (font && font->handle >= 0) {
				nvgFontFaceId(args.vg, font->handle);
				nvgFontSize(args.vg, 9.f * s);   // match PATTERN font size
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
				nvgFillColor(args.vg, COL_TEXT_BRIGHT);
				nvgText(args.vg,
					r.pos.x + r.size.x * 0.5f,
					184.f * s,                    // shared baseline with PATTERN
					statValues[i].c_str(), NULL);
			}
		}
	}

	// --- Pattern selector ---
	for (int p = 0; p < N_PATTERNS; p++) {
		const rack::math::Rect& pr = patternRect[p];
		bool active = module->patterns[p].active;
		bool isEdit = (p == module->editPattern);
		bool isPlay = (p == module->playPattern);

		NVGcolor bg;
		if (!active)      bg = COL_PURPLE_DK;
		else if (isPlay)  bg = COL_ORANGE;
		else if (isEdit)  bg = COL_BLUE;
		else              bg = COL_PURPLE_MID;

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, pr.pos.x, pr.pos.y, pr.size.x, pr.size.y, 2.f * s);
		nvgFillColor(args.vg, bg);
		nvgFill(args.vg);

		if (isEdit && !isPlay) {
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg,
				pr.pos.x + 0.5f, pr.pos.y + 0.5f,
				pr.size.x - 1.f, pr.size.y - 1.f, 2.f * s);
			nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 160));
			nvgStrokeWidth(args.vg, 0.6f);
			nvgStroke(args.vg);
		}

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

		// Repeat-count dots: dim total + bright playhead (matches Beat)
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
		for (int i = 0; i < N_REPEATS; i++) {
			const rack::math::Rect& rr = repeatsRect[i];
			bool inRange = (i < editReps);
			bool isCurrent = (i == currentBarIdx);
			NVGcolor c = isCurrent ? COL_ORANGE : (inRange ? COL_BLUE : COL_PURPLE);
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, rr.pos.x, rr.pos.y, rr.size.x, rr.size.y, 2.f * s);
			nvgFillColor(args.vg, c);
			nvgFill(args.vg);
		}
	}

	OpaqueWidget::drawLayer(args, layer);
}


// --- Widget ---

struct NoteWidget : ModuleWidget {
	NoteWidget(Note* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/note.svg")));


		// Display: x=2.4, y=12, w=46, h=60
		NoteDisplay* display = new NoteDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(2.4f, 12.f));
		display->box.size = mm2px(Vec(46.f, 60.f));
		addChild(display);

		// Trimpots row (y=81.30): ROOT, SCALE, OCT
		// at x=23.70, 33.86, 44.03 (above their CV inputs)
		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(23.70f, 81.30f)), module, Note::ROOT_PARAM));
		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(33.86f, 81.30f)), module, Note::SCALE_PARAM));
		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(44.03f, 81.30f)), module, Note::OCT_PARAM));

		// Inputs row 1 (y=91.45): RESET (x=6.77), ROOT CV, SCALE CV, OCT CV
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(6.77f,  91.45f)), module, Note::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(23.70f, 91.45f)), module, Note::ROOT_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(33.86f, 91.45f)), module, Note::SCALE_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(44.03f, 91.45f)), module, Note::OCT_INPUT));

		// Row 2 (y=106.68): BAR (left), then on dark plate: ROOT OUT, VEL, ACC
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(6.77f,  106.68f)), module, Note::BAR_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(23.70f, 106.68f)), module, Note::ROOT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(33.86f, 106.68f)), module, Note::VELOCITY_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(44.03f, 106.68f)), module, Note::ACCENT_OUTPUT));

		// Row 3 (y=121.92): CLOCK (left), then on dark plate: SCALE OUT, GATE, V/OCT
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(6.77f,  121.92f)), module, Note::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(23.70f, 121.92f)), module, Note::SCALE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(33.86f, 121.92f)), module, Note::GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(44.03f, 121.92f)), module, Note::VOCT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Note* module = dynamic_cast<Note*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem(
			"Advance only on bar trigger", "",
			&module->advanceOnBarOnly));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Patterns"));
		menu->addChild(createMenuItem("Randomize current pattern (notes only)", "",
			[=]() {
				Note::Pattern& p = module->patterns[module->editPattern];
				int sz = module->currentScaleSize();
				for (int i = 0; i < N_STEPS; i++) {
					if (random::uniform() < 0.6f) {
						p.pitches[i] = (int)(random::uniform() * sz);
					} else {
						p.pitches[i] = -1;  // rest
					}
				}
			}));
		menu->addChild(createMenuItem("Clear current pattern", "",
			[=]() {
				Note::Pattern& p = module->patterns[module->editPattern];
				for (int i = 0; i < N_STEPS; i++) {
					p.pitches[i] = -1;
					p.velocities[i] = 1.f;
					p.accents[i] = false;
					p.probabilities[i] = 1.f;
				}
			}));
		menu->addChild(createMenuItem("Clear all patterns", "",
			[=]() {
				for (int p = 0; p < N_PATTERNS; p++) {
					module->patterns[p] = Note::Pattern();
					module->patterns[p].active = true;
				}
			}));
	}
};


Model* modelNote = createModel<Note, NoteWidget>("Note");
