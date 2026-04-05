#include "plugin.hpp"
#include <cmath>


static const int NUM_OVERTONES = 8;   // 8 switchable overtones (harmonics 2-9)
static const int DISPLAY_POINTS = 256;

// Overtone amplitude falloff: 1/n for harmonic n (overtone i maps to harmonic i+2)
static const float OVERTONE_AMP[NUM_OVERTONES] = {
	1.0f/2.0f, 1.0f/3.0f, 1.0f/4.0f, 1.0f/5.0f, 1.0f/6.0f, 1.0f/7.0f, 1.0f/8.0f, 1.0f/9.0f
};
// Harmonic number for each overtone switch (2 through 9)
static const int OVERTONE_HARMONIC[NUM_OVERTONES] = { 2, 3, 4, 5, 6, 7, 8, 9 };


// Forward declaration
struct Overtone;

struct OvertoneDisplay : Widget {
	Overtone* module = nullptr;

	OvertoneDisplay() {}

	void drawLayer(const DrawArgs& args, int layer) override;

	void draw(const DrawArgs& args) override {
		// Background
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(26, 26, 46));
		nvgFill(args.vg);

		// Border
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgStrokeColor(args.vg, nvgRGB(64, 64, 96));
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStroke(args.vg);

		// Zero line
		float midY = box.size.y * 0.5f;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0, midY);
		nvgLineTo(args.vg, box.size.x, midY);
		nvgStrokeColor(args.vg, nvgRGB(50, 50, 70));
		nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);

		Widget::draw(args);
	}
};


struct Overtone : Module {
	enum ParamId {
		HARMONIC_1_PARAM,
		HARMONIC_2_PARAM,
		HARMONIC_3_PARAM,
		HARMONIC_4_PARAM,
		HARMONIC_5_PARAM,
		HARMONIC_6_PARAM,
		HARMONIC_7_PARAM,
		HARMONIC_8_PARAM,
		EVENODD_PARAM,
		FREQ_PARAM,
		FINE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		MASK_INPUT,
		FILTER_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		// Indicator LEDs (show actual active state)
		INDICATOR_1_LIGHT,
		INDICATOR_2_LIGHT,
		INDICATOR_3_LIGHT,
		INDICATOR_4_LIGHT,
		INDICATOR_5_LIGHT,
		INDICATOR_6_LIGHT,
		INDICATOR_7_LIGHT,
		INDICATOR_8_LIGHT,
		LIGHTS_LEN
	};

	float phase = 0.f;
	float prevPhase = 0.f;

	// Per-harmonic zero-crossing gating
	// target = what the switches/CV want; active = what's actually sounding
	// Transition from active→target happens only at the harmonic's zero crossing
	bool harmonicTarget[NUM_OVERTONES] = {true, true, true, true, true, true, true, true};
	bool harmonicActive[NUM_OVERTONES] = {true, true, true, true, true, true, true, true};
	float prevHarmonicSample[NUM_OVERTONES] = {};

	// Display buffers
	float displayBuffer[DISPLAY_POINTS] = {};
	float displayFundamental[DISPLAY_POINTS] = {};
	float displayHarmonics[NUM_OVERTONES][DISPLAY_POINTS] = {};
	bool displayDirty = true;
	bool activeHarmonics[NUM_OVERTONES] = {};

	// Mask CV mode
	bool maskModeBinary = true; // true = binary, false = sweep

	Overtone() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		for (int i = 0; i < NUM_OVERTONES; i++) {
			configSwitch(HARMONIC_1_PARAM + i, 0.f, 1.f, 1.f,
				string::f("Harmonic %d", OVERTONE_HARMONIC[i]), {"Off", "On"});
		}

		configSwitch(EVENODD_PARAM, 0.f, 2.f, 0.f, "Harmonic Filter", {"All", "Odd Only", "Even Only"});

		// Frequency: log2 scale, 0 = C4 (261.63 Hz)
		configParam(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);
		// Fine tune: ±1 semitone
		configParam(FINE_PARAM, -1.f / 12.f, 1.f / 12.f, 0.f, "Fine Tune", " cents", 0.f, 1200.f);

		configInput(VOCT_INPUT, "V/Oct");
		configInput(MASK_INPUT, "Harmonic Mask");
		configInput(FILTER_INPUT, "Filter CV (0V=All, 3.3V=Odd, 6.6V=Even)");

		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void updateDisplayBuffer() {
		// Compute normalization: fundamental (1.0) + active overtones
		float normSum = 1.f; // fundamental always contributes
		for (int h = 0; h < NUM_OVERTONES; h++) {
			if (activeHarmonics[h])
				normSum += OVERTONE_AMP[h];
		}

		for (int p = 0; p < DISPLAY_POINTS; p++) {
			float t = (float)p / (float)DISPLAY_POINTS; // 0-1 one cycle
			// Fundamental always present
			float fundVal = std::sin(t * 2.f * M_PI);
			displayFundamental[p] = fundVal;
			float composite = fundVal;

			for (int h = 0; h < NUM_OVERTONES; h++) {
				int n = OVERTONE_HARMONIC[h]; // harmonic number 2-9
				float val = OVERTONE_AMP[h] * std::sin(t * n * 2.f * M_PI);
				displayHarmonics[h][p] = val;
				if (activeHarmonics[h]) {
					composite += val;
				}
			}

			displayBuffer[p] = composite / normSum;
		}

		displayDirty = false;
	}

	void process(const ProcessArgs& args) override {
		// Compute frequency
		float pitch = params[FREQ_PARAM].getValue() + params[FINE_PARAM].getValue();
		if (inputs[VOCT_INPUT].isConnected())
			pitch += inputs[VOCT_INPUT].getVoltage();
		float freq = dsp::FREQ_C4 * std::pow(2.f, pitch);
		freq = clamp(freq, 8.f, 20000.f);

		// Phase accumulation
		phase += freq * args.sampleTime;
		if (phase >= 1.f)
			phase -= 1.f;

		// Determine active harmonics
		bool newActive[NUM_OVERTONES];

		if (inputs[MASK_INPUT].isConnected()) {
			float maskV = clamp(inputs[MASK_INPUT].getVoltage(), 0.f, 10.f);

			if (maskModeBinary) {
				// Binary mode: voltage maps to 8-bit pattern
				int mask = (int)(maskV / 10.f * 255.f);
				for (int i = 0; i < NUM_OVERTONES; i++) {
					newActive[i] = (mask >> i) & 1;
				}
			} else {
				// Sweep mode: voltage maps to number of harmonics from bottom
				int count = (int)(maskV / 10.f * 8.f + 0.5f);
				count = clamp(count, 0, NUM_OVERTONES);
				for (int i = 0; i < NUM_OVERTONES; i++) {
					newActive[i] = (i < count);
				}
			}
		} else {
			// Read toggle switches
			for (int i = 0; i < NUM_OVERTONES; i++) {
				newActive[i] = params[HARMONIC_1_PARAM + i].getValue() > 0.5f;
			}
		}

		// Apply even/odd filter based on actual harmonic number
		int evenOdd = (int)params[EVENODD_PARAM].getValue();
		if (inputs[FILTER_INPUT].isConnected()) {
			// 0-3.3V = All, 3.3-6.6V = Odd, 6.6-10V = Even
			float fv = clamp(inputs[FILTER_INPUT].getVoltage(), 0.f, 10.f);
			if (fv < 3.33f) evenOdd = 0;
			else if (fv < 6.66f) evenOdd = 1;
			else evenOdd = 2;
		}
		if (evenOdd == 1) {
			// Odd harmonics only: mute even harmonics (2, 4, 6, 8)
			for (int i = 0; i < NUM_OVERTONES; i++) {
				if (OVERTONE_HARMONIC[i] % 2 == 0) newActive[i] = false;
			}
		} else if (evenOdd == 2) {
			// Even harmonics only: mute odd harmonics (3, 5, 7, 9)
			for (int i = 0; i < NUM_OVERTONES; i++) {
				if (OVERTONE_HARMONIC[i] % 2 != 0) newActive[i] = false;
			}
		}

		// Update harmonic targets
		for (int i = 0; i < NUM_OVERTONES; i++) {
			harmonicTarget[i] = newActive[i];
		}

		// Additive synthesis with zero-crossing gating
		// Fundamental (H1) is always present
		float fundamental = std::sin(phase * 2.f * (float)M_PI);
		float out = fundamental;      // amplitude 1.0 for fundamental
		float normSum = 1.f;           // fundamental contributes 1.0
		bool changed = false;

		for (int h = 0; h < NUM_OVERTONES; h++) {
			int n = OVERTONE_HARMONIC[h]; // harmonic number 2-9
			float harmonicSample = OVERTONE_AMP[h] * std::sin(phase * n * 2.f * (float)M_PI);

			// Check for zero crossing: sign change between previous and current sample
			if (harmonicActive[h] != harmonicTarget[h]) {
				bool zeroCrossing = (prevHarmonicSample[h] <= 0.f && harmonicSample >= 0.f) ||
				                    (prevHarmonicSample[h] >= 0.f && harmonicSample <= 0.f);
				if (zeroCrossing) {
					harmonicActive[h] = harmonicTarget[h];
					changed = true;
				}
			}

			prevHarmonicSample[h] = harmonicSample;

			if (harmonicActive[h]) {
				out += harmonicSample;
				normSum += OVERTONE_AMP[h];
			}

			// Track for display
			activeHarmonics[h] = harmonicActive[h];
		}
		if (changed) displayDirty = true;

		// Update indicator LEDs (show actual active state after filtering/CV)
		for (int i = 0; i < NUM_OVERTONES; i++) {
			lights[INDICATOR_1_LIGHT + i].setBrightness(harmonicActive[i] ? 1.f : 0.f);
		}

		// Normalize to ±5V
		out = (out / normSum) * 5.f;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -10.f, 10.f));
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "maskModeBinary", json_boolean(maskModeBinary));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "maskModeBinary");
		if (modeJ)
			maskModeBinary = json_boolean_value(modeJ);
	}
};


// --- Display drawLayer ---

void OvertoneDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1 || !module) {
		Widget::drawLayer(args, layer);
		return;
	}

	// Update display buffer if needed
	if (module->displayDirty)
		module->updateDisplayBuffer();

	float w = box.size.x;
	float h = box.size.y;
	float midY = h * 0.5f;

	// Harmonic trace colors (subtle, per-harmonic hue)
	NVGcolor harmonicColors[NUM_OVERTONES] = {
		nvgRGBA(100, 180, 255, 40),  // H1 blue
		nvgRGBA(255, 140, 80, 40),   // H2 orange
		nvgRGBA(100, 255, 140, 40),  // H3 green
		nvgRGBA(255, 100, 180, 40),  // H4 pink
		nvgRGBA(180, 140, 255, 40),  // H5 purple
		nvgRGBA(255, 220, 80, 40),   // H6 yellow
		nvgRGBA(80, 220, 220, 40),   // H7 cyan
		nvgRGBA(220, 180, 140, 40),  // H8 tan
	};

	// Draw fundamental trace (always present, faint)
	nvgBeginPath(args.vg);
	for (int i = 0; i < DISPLAY_POINTS; i++) {
		float px = (float)i / (float)DISPLAY_POINTS * w;
		float val = module->displayFundamental[i];
		float py = midY - val * midY * 0.9f;
		if (i == 0)
			nvgMoveTo(args.vg, px, py);
		else
			nvgLineTo(args.vg, px, py);
	}
	nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 35));
	nvgStrokeWidth(args.vg, 1.0f);
	nvgStroke(args.vg);

	// Draw individual overtone traces (faint)
	for (int harm = 0; harm < NUM_OVERTONES; harm++) {
		if (!module->activeHarmonics[harm]) continue;

		nvgBeginPath(args.vg);
		for (int i = 0; i < DISPLAY_POINTS; i++) {
			float px = (float)i / (float)DISPLAY_POINTS * w;
			float val = module->displayHarmonics[harm][i];
			float py = midY - val * midY * 0.9f;
			if (i == 0)
				nvgMoveTo(args.vg, px, py);
			else
				nvgLineTo(args.vg, px, py);
		}
		nvgStrokeColor(args.vg, harmonicColors[harm]);
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStroke(args.vg);
	}

	// Draw composite waveform (bright)
	nvgBeginPath(args.vg);
	for (int i = 0; i < DISPLAY_POINTS; i++) {
		float px = (float)i / (float)DISPLAY_POINTS * w;
		float val = module->displayBuffer[i];
		float py = midY - val * midY * 0.9f;
		if (i == 0)
			nvgMoveTo(args.vg, px, py);
		else
			nvgLineTo(args.vg, px, py);
	}
	nvgStrokeColor(args.vg, nvgRGBA(100, 180, 255, 220));
	nvgStrokeWidth(args.vg, 1.5f);
	nvgStroke(args.vg);

	Widget::drawLayer(args, layer);
}


// --- Widget ---

struct OvertoneWidget : ModuleWidget {
	OvertoneWidget(Overtone* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/overtone.svg")));

		// Screws
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Waveform display (matches Phase: same Y, same margins from panel edge)
		OvertoneDisplay* display = new OvertoneDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(5.8f, 14.f));
		display->box.size = mm2px(Vec(39.2f, 24.f));
		addChild(display);

		// 8 overtone CKSS toggles + indicator LEDs, aligned to screen width
		// Screen: X=5.8mm to X=45.0mm (width 39.2mm)
		// 8 positions evenly spaced within screen bounds
		float screenLeft = 5.8f;
		float screenWidth = 39.2f;
		float toggleSpacing = screenWidth / (float)NUM_OVERTONES;

		for (int i = 0; i < NUM_OVERTONES; i++) {
			float xPos = screenLeft + toggleSpacing * (i + 0.5f);

			// Red LED indicator between display and toggle (Y=40mm)
			addChild(createLightCentered<SmallLight<RedLight>>(
				mm2px(Vec(xPos, 40.f)), module, Overtone::INDICATOR_1_LIGHT + i));

			// CKSS toggle switch (Y=46mm)
			addParam(createParamCentered<CKSS>(
				mm2px(Vec(xPos, 46.f)), module, Overtone::HARMONIC_1_PARAM + i));
		}

		// Even/Odd 3-state toggle + Filter CV (Y=60mm)
		addParam(createParamCentered<CKSSThree>(
			mm2px(Vec(15.24f, 60.f)), module, Overtone::EVENODD_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.56f, 60.f)), module, Overtone::FILTER_INPUT));

		// Frequency and Fine knobs (Y=78mm)
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.24f, 78.f)), module, Overtone::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(35.56f, 78.f)), module, Overtone::FINE_PARAM));

		// V/Oct input (Y=95mm)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4f, 95.f)), module, Overtone::VOCT_INPUT));

		// Bottom row (Y=110mm)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24f, 110.f)), module, Overtone::MASK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(35.56f, 110.f)), module, Overtone::AUDIO_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Overtone* module = dynamic_cast<Overtone*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Mask CV Mode"));

		menu->addChild(createCheckMenuItem("Binary (8-bit pattern)", "",
			[=]() { return module->maskModeBinary; },
			[=]() { module->maskModeBinary = true; }
		));
		menu->addChild(createCheckMenuItem("Sweep (0-8 harmonics)", "",
			[=]() { return !module->maskModeBinary; },
			[=]() { module->maskModeBinary = false; }
		));
	}
};


Model* modelOvertone = createModel<Overtone, OvertoneWidget>("Overtone");
