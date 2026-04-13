#include "plugin.hpp"
#include "fugue-messages.hpp"

static const int SLEEP_VALUES[] = {0, 1, 2, 4, 5, 8, 16, 32, 48, 64};
static const int NUM_SLEEP_VALUES = 10;
static const float RANGE_VALUES[] = {1.f, 2.f, 5.f};

struct FugueX : Module {

	enum ParamId {
		RAND_SEQ_BUTTON_PARAM,
		SAMPLE_HOLD_PARAM,
		STEPS_A_PARAM,
		STEPS_B_PARAM,
		STEPS_C_PARAM,
		RANGE_A_PARAM,
		RANGE_B_PARAM,
		RANGE_C_PARAM,
		SLEEP_A_PARAM,
		SLEEP_B_PARAM,
		SLEEP_C_PARAM,
		PROB_A_PARAM,
		PROB_B_PARAM,
		PROB_C_PARAM,
		PARAMS_LEN
	};

	enum InputId {
		RAND_SEQ_INPUT,
		STEPS_A_INPUT,
		STEPS_B_INPUT,
		STEPS_C_INPUT,
		RANGE_A_INPUT,
		RANGE_B_INPUT,
		RANGE_C_INPUT,
		SLEEP_A_INPUT,
		SLEEP_B_INPUT,
		SLEEP_C_INPUT,
		PROB_A_INPUT,
		PROB_B_INPUT,
		PROB_C_INPUT,
		INPUTS_LEN
	};

	enum OutputId {
		MAX_OUTPUT,
		MID_OUTPUT,
		MIN_OUTPUT,
		GATE_A_OUTPUT_0,
		GATE_B_OUTPUT_0 = GATE_A_OUTPUT_0 + FUGUE_NUM_STEPS,
		GATE_C_OUTPUT_0 = GATE_B_OUTPUT_0 + FUGUE_NUM_STEPS,
		OUTPUTS_LEN = GATE_C_OUTPUT_0 + FUGUE_NUM_STEPS
	};

	enum LightId {
		SAMPLE_HOLD_LIGHT,
		// LED matrix: 8 step columns x 3 rows (red)
		STEP_LED_0,   // step 0 voice A, step 0 voice B, step 0 voice C, step 1 voice A, ...
		// Layout: STEP_LED_0 + step * FUGUE_NUM_VOICES + voice
		SLEEP_LED_0 = STEP_LED_0 + FUGUE_NUM_STEPS * FUGUE_NUM_VOICES,  // 3 amber sleep LEDs
		LIGHTS_LEN = SLEEP_LED_0 + FUGUE_NUM_VOICES
	};

	dsp::SchmittTrigger randSeqTrigger;
	dsp::SchmittTrigger randSeqButtonTrigger;
	bool randomizeRequested = false;
	dsp::PulseGenerator triggerPulses[FUGUE_NUM_VOICES][FUGUE_NUM_STEPS];

	~FugueX() {
		delete (ExpanderToFugueMessage*)leftExpander.producerMessage;
		delete (ExpanderToFugueMessage*)leftExpander.consumerMessage;
	}

	FugueX() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		leftExpander.producerMessage = new ExpanderToFugueMessage();
		leftExpander.consumerMessage = new ExpanderToFugueMessage();

		configParam(RAND_SEQ_BUTTON_PARAM, 0.f, 1.f, 0.f, "Randomize Sequence");
		configInput(RAND_SEQ_INPUT, "Randomize Sequence Trigger");
		configSwitch(SAMPLE_HOLD_PARAM, 0.f, 1.f, 0.f, "Sample & Hold Mode", {"Off", "On"});

		const char* voiceNames[] = {"A", "B", "C"};
		for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
			configSwitch(STEPS_A_PARAM + v, 1.f, 8.f, 8.f,
				string::f("Steps %s", voiceNames[v]),
				{"1", "2", "3", "4", "5", "6", "7", "8"});
			configSwitch(RANGE_A_PARAM + v, 0.f, 2.f, 0.f,
				string::f("Range %s", voiceNames[v]),
				{"1V", "2V", "5V"});
			configSwitch(SLEEP_A_PARAM + v, 0.f, 9.f, 0.f,
				string::f("Sleep %s", voiceNames[v]),
				{"0", "1", "2", "4", "5", "8", "16", "32", "48", "64"});
			configParam(PROB_A_PARAM + v, 0.f, 1.f, 1.f,
				string::f("Probability %s", voiceNames[v]), "%", 0.f, 100.f);

			configInput(STEPS_A_INPUT + v, string::f("Steps %s CV", voiceNames[v]));
			configInput(RANGE_A_INPUT + v, string::f("Range %s CV", voiceNames[v]));
			configInput(SLEEP_A_INPUT + v, string::f("Sleep %s CV", voiceNames[v]));
			configInput(PROB_A_INPUT + v, string::f("Probability %s CV", voiceNames[v]));
		}

		configOutput(MAX_OUTPUT, "Max CV");
		configOutput(MID_OUTPUT, "Mid CV");
		configOutput(MIN_OUTPUT, "Min CV");

		for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
			for (int s = 0; s < FUGUE_NUM_STEPS; s++) {
				int outIdx = (v == 0) ? GATE_A_OUTPUT_0 + s :
				             (v == 1) ? GATE_B_OUTPUT_0 + s :
				                        GATE_C_OUTPUT_0 + s;
				configOutput(outIdx, string::f("Gate %s Step %d", voiceNames[v], s + 1));
			}
		}
	}

	void process(const ProcessArgs& args) override {
		bool hasFugue = (leftExpander.module && leftExpander.module->model == modelFugue);

		// ── Read Fugue state ──
		FugueToExpanderMessage fugueState = {};
		if (hasFugue) {
			FugueToExpanderMessage* rxMsg =
				(FugueToExpanderMessage*)leftExpander.module->rightExpander.consumerMessage;
			if (rxMsg) {
				fugueState = *rxMsg;
			}
		}

		// ── Randomize trigger ──
		bool randBtn = randSeqButtonTrigger.process(params[RAND_SEQ_BUTTON_PARAM].getValue());
		bool randTrig = randSeqTrigger.process(inputs[RAND_SEQ_INPUT].getVoltage(), 0.1f, 1.f);
		randomizeRequested = (randBtn || randTrig);

		// ── S&H light ──
		bool sampleHold = params[SAMPLE_HOLD_PARAM].getValue() > 0.5f;
		lights[SAMPLE_HOLD_LIGHT].setBrightness(sampleHold ? 1.f : 0.f);

		// ── Build override message for Fugue ──
		ExpanderToFugueMessage* txMsg = (ExpanderToFugueMessage*)leftExpander.producerMessage;
		if (txMsg) {
			txMsg->connected = true;
			txMsg->randomizeRequested = randomizeRequested;
			txMsg->sampleHoldEnabled = sampleHold;

			for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
				int steps = (int)std::round(params[STEPS_A_PARAM + v].getValue());
				if (inputs[STEPS_A_INPUT + v].isConnected()) {
					steps += (int)std::round(inputs[STEPS_A_INPUT + v].getVoltage());
					steps = clamp(steps, 1, 8);
				}
				txMsg->voices[v].stepsOverride = steps;

				int rangeIdx = (int)std::round(params[RANGE_A_PARAM + v].getValue());
				if (inputs[RANGE_A_INPUT + v].isConnected()) {
					rangeIdx += (int)std::round(inputs[RANGE_A_INPUT + v].getVoltage());
					rangeIdx = clamp(rangeIdx, 0, 2);
				}
				txMsg->voices[v].rangeOverride = RANGE_VALUES[rangeIdx];

				int sleepIdx = (int)std::round(params[SLEEP_A_PARAM + v].getValue());
				if (inputs[SLEEP_A_INPUT + v].isConnected()) {
					sleepIdx += (int)std::round(inputs[SLEEP_A_INPUT + v].getVoltage());
					sleepIdx = clamp(sleepIdx, 0, NUM_SLEEP_VALUES - 1);
				}
				txMsg->voices[v].sleepDivision = SLEEP_VALUES[sleepIdx];

				float prob = params[PROB_A_PARAM + v].getValue();
				if (inputs[PROB_A_INPUT + v].isConnected()) {
					prob += inputs[PROB_A_INPUT + v].getVoltage() / 5.f;
				}
				txMsg->voices[v].probability = clamp(prob, 0.f, 1.f);
			}

			leftExpander.requestMessageFlip();
		}

		// ── Min / Mid / Max outputs ──
		if (hasFugue) {
			float v0 = fugueState.voices[0].currentVoltage;
			float v1 = fugueState.voices[1].currentVoltage;
			float v2 = fugueState.voices[2].currentVoltage;
			float minV = std::min({v0, v1, v2});
			float maxV = std::max({v0, v1, v2});
			float midV = v0 + v1 + v2 - minV - maxV;
			outputs[MAX_OUTPUT].setVoltage(maxV);
			outputs[MID_OUTPUT].setVoltage(midV);
			outputs[MIN_OUTPUT].setVoltage(minV);
		} else {
			outputs[MAX_OUTPUT].setVoltage(0.f);
			outputs[MID_OUTPUT].setVoltage(0.f);
			outputs[MIN_OUTPUT].setVoltage(0.f);
		}

		// ── Per-step trigger outputs ──
		for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
			int gateBaseOutput = (v == 0) ? GATE_A_OUTPUT_0 :
			                     (v == 1) ? GATE_B_OUTPUT_0 :
			                                GATE_C_OUTPUT_0;

			if (hasFugue && fugueState.voices[v].clockRose && fugueState.voices[v].gateOn) {
				int step = fugueState.voices[v].currentStep;
				if (step >= 0 && step < FUGUE_NUM_STEPS) {
					triggerPulses[v][step].trigger(1e-3f);
				}
			}

			for (int s = 0; s < FUGUE_NUM_STEPS; s++) {
				bool pulse = triggerPulses[v][s].process(args.sampleTime);
				outputs[gateBaseOutput + s].setVoltage(pulse ? 10.f : 0.f);
			}
		}

		// ── LED matrix: step indicators (cols 1-8, red) ──
		for (int step = 0; step < FUGUE_NUM_STEPS; step++) {
			for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
				int lightIdx = STEP_LED_0 + step * FUGUE_NUM_VOICES + v;
				if (hasFugue) {
					bool isCurrentStep = (fugueState.voices[v].currentStep == step);
					bool gateOn = fugueState.voices[v].gateOn;
					bool isSleeping = fugueState.voices[v].sleeping;
					// Show current step brightly if gate is on and not sleeping
					if (isCurrentStep && !isSleeping) {
						lights[lightIdx].setBrightness(gateOn ? 1.f : 0.3f);
					} else {
						lights[lightIdx].setBrightness(0.f);
					}
				} else {
					lights[lightIdx].setBrightness(0.f);
				}
			}
		}

		// ── LED matrix: sleep indicators (col 9, amber) ──
		for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
			if (hasFugue && fugueState.voices[v].sleeping) {
				int div = fugueState.voices[v].sleepDivision;
				int counter = fugueState.voices[v].sleepCounter;
				// Brightness increases towards wake: 1.0 at wake, dim at start of sleep
				float progress = (div > 0) ? 1.f - (float)counter / (float)div : 1.f;
				// Minimum brightness 0.15 so it's always visible when sleeping
				float brightness = 0.15f + progress * 0.85f;
				// Flash on clock pulse
				if (fugueState.voices[v].clockRose) {
					brightness = 1.f;
				}
				lights[SLEEP_LED_0 + v].setBrightness(brightness);
			} else {
				lights[SLEEP_LED_0 + v].setBrightness(0.f);
			}
		}
	}
};

// ─── Widget ──────────────────────────────────────────────────────────────────

struct FugueXWidget : ModuleWidget {
	FugueXWidget(FugueX* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/fugue-expander.svg")));

		// Screws
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// ══════════════════════════════════════════════════════════════════════
		// LAYOUT CONSTANTS (mm) — matched to SVG reticules
		// Module is 24HP = 121.92mm wide
		// ══════════════════════════════════════════════════════════════════════

		// ── Top controls (Y = 27.94mm) ──
		const float topRowY = 27.94f;

		// ── LED matrix (9 cols x 3 rows) ──
		// Cols 1-8: step indicators (red), Col 9: sleep (amber)
		const float ledStartX = 71.12f;
		const float ledSpacing = 5.08f;
		const float ledRowYs[] = {17.78f, 22.86f, 27.94f};  // A, B, C

		// ── Per-voice parameter grid ──
		const float stpKnobX = 10.16f;   const float stpCvX = 20.32f;
		const float rngKnobX = 40.64f;   const float rngCvX = 50.80f;
		const float slpKnobX = 71.24f;   const float slpCvX = 81.28f;
		const float prbKnobX = 101.60f;  const float prbCvX = 111.76f;

		const float voiceAY = 45.72f;
		const float voiceBY = 60.96f;
		const float voiceCY = 76.20f;

		// ── Bottom section ──
		const float gateStartX = 10.16f;
		const float gateSpacing = 10.16f;
		const float gateAY = 96.52f;
		const float gateBY = 106.68f;
		const float gateCY = 116.83f;
		const float minMidMaxX = 111.76f;

		// ══════════════════════════════════════════════════════════════════════
		// TOP SECTION
		// ══════════════════════════════════════════════════════════════════════

		// Rand Seq button @ (10.16, 27.94), jack @ (20.32, 27.94)
		addParam(createParamCentered<VCVButton>(mm2px(Vec(10.16f, topRowY)), module, FugueX::RAND_SEQ_BUTTON_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, topRowY)), module, FugueX::RAND_SEQ_INPUT));

		// S&H toggle @ (40.64, 27.94)
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
			mm2px(Vec(40.64f, topRowY)), module,
			FugueX::SAMPLE_HOLD_PARAM, FugueX::SAMPLE_HOLD_LIGHT));

		// ══════════════════════════════════════════════════════════════════════
		// LED MATRIX (top-right): 9 cols x 3 rows
		// Cols 1-8: red step indicators, Col 9: amber sleep indicators
		// ══════════════════════════════════════════════════════════════════════

		for (int step = 0; step < FUGUE_NUM_STEPS; step++) {
			float x = ledStartX + step * ledSpacing;
			for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
				int lightIdx = FugueX::STEP_LED_0 + step * FUGUE_NUM_VOICES + v;
				addChild(createLightCentered<SmallLight<RedLight>>(
					mm2px(Vec(x, ledRowYs[v])), module, lightIdx));
			}
		}

		// Sleep LEDs (col 9, amber)
		float sleepLedX = ledStartX + 8 * ledSpacing;  // 111.76mm
		for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
			addChild(createLightCentered<SmallLight<YellowLight>>(
				mm2px(Vec(sleepLedX, ledRowYs[v])), module, FugueX::SLEEP_LED_0 + v));
		}

		// ══════════════════════════════════════════════════════════════════════
		// PER-VOICE PARAMETERS (3 rows x 4 columns, knob + CV side-by-side)
		// ══════════════════════════════════════════════════════════════════════

		const float voiceYs[] = {voiceAY, voiceBY, voiceCY};

		for (int v = 0; v < FUGUE_NUM_VOICES; v++) {
			float y = voiceYs[v];

			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(stpKnobX, y)), module, FugueX::STEPS_A_PARAM + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(stpCvX, y)), module, FugueX::STEPS_A_INPUT + v));

			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rngKnobX, y)), module, FugueX::RANGE_A_PARAM + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rngCvX, y)), module, FugueX::RANGE_A_INPUT + v));

			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(slpKnobX, y)), module, FugueX::SLEEP_A_PARAM + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(slpCvX, y)), module, FugueX::SLEEP_A_INPUT + v));

			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(prbKnobX, y)), module, FugueX::PROB_A_PARAM + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(prbCvX, y)), module, FugueX::PROB_A_INPUT + v));
		}

		// ══════════════════════════════════════════════════════════════════════
		// BOTTOM SECTION: Per-step triggers + Max/Mid/Min
		// ══════════════════════════════════════════════════════════════════════

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(minMidMaxX, gateAY)), module, FugueX::MAX_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(minMidMaxX, gateBY)), module, FugueX::MID_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(minMidMaxX, gateCY)), module, FugueX::MIN_OUTPUT));

		for (int s = 0; s < FUGUE_NUM_STEPS; s++) {
			float x = gateStartX + s * gateSpacing;
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, gateAY)), module, FugueX::GATE_A_OUTPUT_0 + s));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, gateBY)), module, FugueX::GATE_B_OUTPUT_0 + s));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, gateCY)), module, FugueX::GATE_C_OUTPUT_0 + s));
		}
	}
};

Model* modelFugueX = createModel<FugueX, FugueXWidget>("FugueX");
