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
// Each string has its own VEL jack and an attenuator beside it. The attenuator
// is not only an attenuator: with nothing patched it IS the string's velocity,
// so the eight trimpots are a picking-balance control on their own -- quieter
// bass, a leaning-on inner voice -- and once a jack is patched the same trimpot
// scales it. Defaulting to full means an unpatched module behaves as before.
//
// Everything crosses the bus one sample late. Inaudible, and it is the price of
// not making Slide 8HP wider.
// =============================================================================

#include "plugin.hpp"
#include "slide-messages.hpp"
#include "panel-style.hpp"

struct SlideX : Module {
	enum ParamId  { ENUMS(VELATT_PARAM, SlideXMessage::NCH), PARAMS_LEN };
	// VEL_INPUT was one polyphonic jack. RETIRED in place rather than removed --
	// the enum is append-only, and deleting it would re-point every later index.
	enum InputId  { ENUMS(GATE_INPUT, SlideXMessage::NCH), VEL_INPUT,
	                ENUMS(VELS_INPUT, SlideXMessage::NCH), INPUTS_LEN };
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
			configInput(VELS_INPUT + i, string::f("String %d velocity (0–10V)", i + 1));
			configParam(VELATT_PARAM + i, 0.f, 1.f, 1.f,
			            string::f("String %d velocity / attenuator", i + 1), "%", 0.f, 100.f);
		}
		configInput(VEL_INPUT, "(retired — each string has its own velocity jack)");
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
				float v = inputs[VELS_INPUT + i].isConnected()
				        ? inputs[VELS_INPUT + i].getVoltage() / 10.f : 1.f;
				out->vel[i] = clamp(v * params[VELATT_PARAM + i].getValue(), 0.03f, 1.f);
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
		const float xn = hp(0.95f), xg = hp(2.5f), xv = hp(5), xa = hp(7.25f),
		            xo = hp(9.75f), xl = hp(11.2f);
		const float y0 = hp(4), dy = hp(2.5f);
		for (int i = 0; i < SlideXMessage::NCH; i++) {
			float y = y0 + dy * i;
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xg, y)), module,
			                                         SlideX::GATE_INPUT + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xv, y)), module,
			                                         SlideX::VELS_INPUT + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(xa, y)), module,
			                                      SlideX::VELATT_PARAM + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xo, y)), module,
			                                           SlideX::STRING_OUTPUT + i));
			addChild(createLightCentered<SmallLight<GreenLight>>(
				mm2px(Vec(xl, y)), module, SlideX::STRING_LIGHT + i));
			lbl->add(xn, y, string::f("%d", i + 1), sfs::PanelLabels::LABEL);
		}
		lbl->add(xg, y0 - sfs::LABEL_GAP_JACK, "GATE");
		lbl->add(xv, y0 - sfs::LABEL_GAP_JACK, "VEL");
		lbl->add(xa, y0 - sfs::LABEL_GAP_TRIM, "LVL");
		lbl->add(xo, y0 - sfs::LABEL_GAP_JACK, "OUT");
	}
};

Model* modelSlideX = createModel<SlideX, SlideXWidget>("SlideX");
