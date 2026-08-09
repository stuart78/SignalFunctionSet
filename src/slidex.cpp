// =============================================================================
// SLIDE X — expander for Slide. The eight strings, one jack each.
//
// Slide used to carry these on a polyphonic output. A poly cable is the right
// answer when the eight channels are one voice being played polyphonically; it
// is the wrong one here, because these are eight strings of a single instrument
// that a player wants to send to eight different places -- separate amps, a
// per-string filter, a mixer with its own pan. Splitting a poly cable back out
// to do that costs a module and a row of cables anyway, so the jacks belong on
// the instrument.
//
// The audio arrives over the expander bus one sample late. That is inaudible,
// and it is the price of not making Slide 8HP wider.
// =============================================================================

#include "plugin.hpp"
#include "slide-messages.hpp"
#include "panel-style.hpp"

struct SlideX : Module {
	enum ParamId  { PARAMS_LEN };
	enum InputId  { INPUTS_LEN };
	enum OutputId { ENUMS(STRING_OUTPUT, SlideExpanderMessage::NCH), OUTPUTS_LEN };
	enum LightId  { ENUMS(STRING_LIGHT, SlideExpanderMessage::NCH), LIGHTS_LEN };

	float env[SlideExpanderMessage::NCH] = {};   // for the activity lights
	bool  connected = false;

	SlideX() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < SlideExpanderMessage::NCH; i++)
			configOutput(STRING_OUTPUT + i, string::f("String %d", i + 1));
	}

	void process(const ProcessArgs& args) override {
		SlideExpanderMessage msg;
		connected = leftExpander.module && leftExpander.module->model == modelSlide;
		if (connected) {
			auto* m = (SlideExpanderMessage*) leftExpander.module->rightExpander.consumerMessage;
			if (m) msg = *m;
		}
		// A follower with no mother outputs silence rather than whatever was in
		// the buffer when it was unplugged.
		const float decay = args.sampleTime / 0.08f;
		for (int i = 0; i < SlideExpanderMessage::NCH; i++) {
			float v = (connected && msg.active) ? msg.string[i] : 0.f;
			outputs[STRING_OUTPUT + i].setVoltage(v);
			float a = std::fabs(v) * 0.15f;
			env[i] = (a > env[i]) ? a : env[i] - env[i] * decay;
			lights[STRING_LIGHT + i].setBrightness(clamp(env[i], 0.f, 1.f));
		}
	}
};

struct SlideXWidget : ModuleWidget {
	SlideXWidget(SlideX* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/slidex.svg")));
		using sfs::hp;

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(hp(0.75f), hp(1.6f), "STR");

		// One column of eight, on the same 2.5HP pitch the strings use on Slide's
		// own rows, so the two read as one instrument side by side.
		const float x = hp(3), y0 = hp(4.5f), dy = hp(2.5f);
		for (int i = 0; i < SlideExpanderMessage::NCH; i++) {
			float y = y0 + dy * i;
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, y)), module,
			                                           SlideX::STRING_OUTPUT + i));
			addChild(createLightCentered<SmallLight<GreenLight>>(
				mm2px(Vec(hp(5.1f), y)), module, SlideX::STRING_LIGHT + i));
			lbl->add(hp(1.1f), y, string::f("%d", i + 1), sfs::PanelLabels::LABEL);
		}
	}
};

Model* modelSlideX = createModel<SlideX, SlideXWidget>("SlideX");
