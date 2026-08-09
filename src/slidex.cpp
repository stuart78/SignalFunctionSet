// =============================================================================
// SLIDE X — expander for Slide. The eight strings: a gate in and an out each.
//
// Slide used to carry these on a polyphonic output. A poly cable is the right
// answer when the eight channels are one voice being played polyphonically; it
// is the wrong one here, because these are eight strings of a single instrument
// that a player wants to send to eight different places -- separate amps, a
// per-string filter, a mixer with its own pan. Splitting a poly cable back out
// to do that costs a module and a row of cables anyway, so the jacks belong on
// the instrument.
//
// The gates address STRINGS. That is not what Slide's own poly GATE does --
// there, channel N is the Nth note and which string it lands on is the bar
// solver's business, because on a real steel you do not choose. Here a jack
// labelled 3 plays string 3, which is the thing a patch cannot otherwise ask
// for. VEL is polyphonic: channel N is the velocity for string N.
//
// Everything crosses the bus one sample late. Inaudible, and it is the price of
// not making Slide 8HP wider.
// =============================================================================

#include "plugin.hpp"
#include "slide-messages.hpp"
#include "panel-style.hpp"

struct SlideX : Module {
	enum ParamId  { PARAMS_LEN };
	enum InputId  { ENUMS(GATE_INPUT, SlideXMessage::NCH), VEL_INPUT, INPUTS_LEN };
	enum OutputId { ENUMS(STRING_OUTPUT, SlideExpanderMessage::NCH), OUTPUTS_LEN };
	enum LightId  { ENUMS(STRING_LIGHT, SlideExpanderMessage::NCH), LIGHTS_LEN };

	float env[SlideExpanderMessage::NCH] = {};   // for the activity lights
	bool  connected = false;

	SlideX() {
		// This module WRITES its leftExpander pair, so it owns them.
		leftExpander.producerMessage = new SlideXMessage();
		leftExpander.consumerMessage = new SlideXMessage();
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < SlideExpanderMessage::NCH; i++) {
			configOutput(STRING_OUTPUT + i, string::f("String %d", i + 1));
			configInput(GATE_INPUT + i, string::f("String %d gate", i + 1));
		}
		configInput(VEL_INPUT, "Velocity (polyphonic — channel N is string N)");
	}

	~SlideX() {
		delete (SlideXMessage*) leftExpander.producerMessage;
		delete (SlideXMessage*) leftExpander.consumerMessage;
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

		// Gates back the other way. Sent every sample whether or not a mother is
		// attached, because active is what says whether to listen -- leaving
		// stale volts in the buffer would fire a string on reconnection.
		auto* out = (SlideXMessage*) leftExpander.producerMessage;
		if (out) {
			out->active = connected;
			for (int i = 0; i < SlideXMessage::NCH; i++) {
				out->gate[i] = inputs[GATE_INPUT + i].getVoltage();
				out->vel[i] = inputs[VEL_INPUT].isConnected()
				            ? clamp(inputs[VEL_INPUT].getPolyVoltage(i) / 10.f, 0.03f, 1.f)
				            : 1.f;
			}
			leftExpander.requestMessageFlip();
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

		// Eight rows on the same 2.5HP pitch the strings use on Slide's own rows,
		// so the two read as one instrument side by side. Gate in, then out, with
		// the string number between them and the activity light on the out.
		const float xg = hp(2.25f), xo = hp(5.75f), y0 = hp(4), dy = hp(2.5f);
		for (int i = 0; i < SlideXMessage::NCH; i++) {
			float y = y0 + dy * i;
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xg, y)), module,
			                                         SlideX::GATE_INPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xo, y)), module,
			                                           SlideX::STRING_OUTPUT + i));
			addChild(createLightCentered<SmallLight<GreenLight>>(
				mm2px(Vec(hp(4), y)), module, SlideX::STRING_LIGHT + i));
			lbl->add(hp(4), y - 2.4f, string::f("%d", i + 1), sfs::PanelLabels::LABEL);
		}
		lbl->add(xg, y0 - sfs::LABEL_GAP_JACK, "GATE");
		lbl->add(xo, y0 - sfs::LABEL_GAP_JACK, "OUT");

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xg, hp(24))), module, SlideX::VEL_INPUT));
		lbl->jack(xg, hp(24), "VEL");
	}
};

Model* modelSlideX = createModel<SlideX, SlideXWidget>("SlideX");
