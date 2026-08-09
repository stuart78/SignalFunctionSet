#include "plugin.hpp"
#include "meter-messages.hpp"
#include "pulse-width.hpp"
#include <cmath>

// ─── Meter X — clock/bar expander for Meter ──────────────────────────────────
// Sits to the RIGHT of a Meter. Reads Meter's clock state over the expander bus
// and emits: 24 PPQN, a RUN gate (high while running), and Bar / 2 / 4 / 8 / 16 /
// 32-bar triggers. Each output has an LED that lights on the action.

struct MeterX : Module {
	enum ParamId { PARAMS_LEN };
	enum InputId { INPUTS_LEN };
	enum OutputId {
		PPQN24_OUTPUT,
		RUN_OUTPUT,
		ENUMS(BAR_OUTPUT, 8),      // 1, 2, 4, 8, 16, 32, 64, 128-bar
		OUTPUTS_LEN
	};
	enum LightId {
		PPQN24_LIGHT,
		RUN_LIGHT,
		ENUMS(BAR_LIGHT, 8),
		LIGHTS_LEN
	};

	dsp::PulseGenerator ppqnPulse, barPulse[8];
	float flash[10] = {};          // LED flash: [0]=ppqn, [1]=run(unused), [2..9]=bars
	float barPos = 0.f;            // continuous bars-since-reset (drives the cycle pies)
	int pulseWidthIdx = 0;         // encoder-safe pulse width (index into sfs::PULSE_WIDTHS)

	MeterX() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(PPQN24_OUTPUT, "24 PPQN clock");
		configOutput(RUN_OUTPUT, "Run gate (high while running)");
		static const char* barNames[8] = {"Bar", "2 bar", "4 bar", "8 bar", "16 bar", "32 bar", "64 bar", "128 bar"};
		for (int k = 0; k < 8; k++) configOutput(BAR_OUTPUT + k, barNames[k]);
	}

	void process(const ProcessArgs& args) override {
		MeterExpanderMessage msg;
		if (leftExpander.module && leftExpander.module->model == modelMeter) {
			auto* m = (MeterExpanderMessage*)leftExpander.module->rightExpander.consumerMessage;
			if (m) msg = *m;
		}
		const float dt = args.sampleTime;
		const float decay = dt / 0.08f;   // ~80ms LED flash
		barPos = msg.barPos;              // cycle position for the pie charts

		// RUN gate (steady while running)
		outputs[RUN_OUTPUT].setVoltage(msg.running ? 10.f : 0.f);
		lights[RUN_LIGHT].setBrightness(msg.running ? 1.f : 0.f);

		// 24 PPQN. Each output is scaled by ITS OWN period, so "50% gate" is half
		// a 24th of a quarter here and half of 128 bars eight jacks down --
		// otherwise one setting would mean eight unrelated lengths.
		if (msg.ppqn24)
			{ ppqnPulse.trigger(sfs::pulseWidthSec(pulseWidthIdx, msg.quarterSec / 24.f)); flash[0] = 1.f; }
		outputs[PPQN24_OUTPUT].setVoltage(ppqnPulse.process(dt) ? 10.f : 0.f);
		flash[0] = std::max(0.f, flash[0] - decay);
		lights[PPQN24_LIGHT].setBrightness(flash[0]);

		// Bar divisions
		static const int NB[8] = {1, 2, 4, 8, 16, 32, 64, 128};
		for (int k = 0; k < 8; k++) {
			if (msg.bar[k]) {
				barPulse[k].trigger(sfs::pulseWidthSec(pulseWidthIdx, msg.barSec * NB[k]));
				flash[2 + k] = 1.f;
			}
			outputs[BAR_OUTPUT + k].setVoltage(barPulse[k].process(dt) ? 10.f : 0.f);
			flash[2 + k] = std::max(0.f, flash[2 + k] - decay);
			lights[BAR_LIGHT + k].setBrightness(flash[2 + k]);
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "pulseWidthIdx", json_integer(pulseWidthIdx));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* pwJ = json_object_get(rootJ, "pulseWidthIdx");
		if (pwJ) pulseWidthIdx = clamp((int)json_integer_value(pwJ), 0, sfs::NUM_PULSE_WIDTHS - 1);
	}
};

static const float MX_YS[10] = {20.f, 31.f, 42.f, 53.f, 64.f, 75.f, 86.f, 97.f, 108.f, 119.f};
// Cycle length (in bars) each row's pie tracks. PPQN/RUN spin once per bar.
static const float MX_PERIOD[10] = {1, 1, 1, 2, 4, 8, 16, 32, 64, 128};

// Column x-centres (mm). Panel is 8HP; label(left) · pie · jack · LED-right-of-jack.
static const float MX_X_PIE  = 18.f;
static const float MX_X_JACK = 27.f;
static const float MX_X_LED  = 35.f;

// Labels are baked into res/meterx.svg as vector outlines (Stuart's final art), so
// there's no code-drawn label layer — drawing one here would double every label.

// Per-output cycle indicators: a small pie that fills clockwise as we advance
// through that output's cycle (e.g. the 4-bar output fills over four bars, then
// resets when it fires). Drawn where the LED used to sit.
struct MXPies : Widget {
	MeterX* module = nullptr;
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float R = mm2px(Vec(2.1f, 0.f)).x;
		for (int i = 2; i < 10; i++) {   // bar outputs only — PPQN/RUN have no cycle
			Vec c = mm2px(Vec(MX_X_PIE, MX_YS[i]));
			float period = MX_PERIOD[i];
			float frac = module ? (period > 0.f ? std::fmod(module->barPos, period) / period : 0.f)
			                    : (0.12f + 0.10f * (i - 2));   // browser preview: staggered demo
			if (frac < 0.f) frac += 1.f;
			// light disc (the empty remainder of the cycle) — no border
			nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, R);
			nvgFillColor(vg, nvgRGB(0xd8, 0xd8, 0xd0)); nvgFill(vg);
			// elapsed wedge, clockwise from 12 o'clock
			if (frac > 0.001f) {
				float a0 = -M_PI / 2.f, a1 = a0 + frac * 2.f * M_PI;
				nvgBeginPath(vg);
				nvgMoveTo(vg, c.x, c.y);
				nvgLineTo(vg, c.x + R * std::cos(a0), c.y + R * std::sin(a0));
				nvgArc(vg, c.x, c.y, R, a0, a1, NVG_CW);
				nvgClosePath(vg);
				nvgFillColor(vg, nvgRGB(0xe0, 0x80, 0x3f)); nvgFill(vg);
			}
		}
	}
};

struct MeterXWidget : ModuleWidget {
	MeterXWidget(MeterX* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/meterx.svg")));
		// No virtual screws — see CLAUDE.md. Labels come from the SVG art (not code).
		MXPies* pies = new MXPies(); pies->module = module; pies->box.size = box.size; addChild(pies);

		const float* ys = MX_YS;
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(MX_X_JACK, ys[0])), module, MeterX::PPQN24_OUTPUT));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(MX_X_LED, ys[0])), module, MeterX::PPQN24_LIGHT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(MX_X_JACK, ys[1])), module, MeterX::RUN_OUTPUT));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(MX_X_LED, ys[1])), module, MeterX::RUN_LIGHT));

		for (int k = 0; k < 8; k++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(MX_X_JACK, ys[2 + k])), module, MeterX::BAR_OUTPUT + k));
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(MX_X_LED, ys[2 + k])), module, MeterX::BAR_LIGHT + k));
		}
	}

	void appendContextMenu(Menu* menu) override {
		MeterX* module = dynamic_cast<MeterX*>(this->module);
		if (!module) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Outputs"));
		// true == offer the duty-cycle gates: every output here has a known period.
		sfs::addPulseWidthMenu(menu, &module->pulseWidthIdx, "Gate/trigger width", true);
	}
};

Model* modelMeterExpander = createModel<MeterX, MeterXWidget>("MeterX");
