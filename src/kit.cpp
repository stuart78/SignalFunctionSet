#include "plugin.hpp"
#include "panel-style.hpp"
#include "membrane.hpp"
#include "waveguide.hpp"   // softClip
#include <cmath>

// Kit -- a struck membrane, modelled mode by mode.
//
// The DSP lives in membrane.hpp so it can be measured without Rack in the way,
// which is how the mallet's 38 ms contact times and 296-bounce chatter were
// found. This file is the instrument around it: the parameters, the strike, and
// a head you can play with the mouse.

// ── tooltips that say something ─────────────────────────────────────────────
// "Size 45%" tells you nothing you could act on. A drum has a diameter, a head
// has a pitch, a beater has a weight and a contact time -- and every one of
// those is already implied by the knob, so printing the percentage instead is
// throwing information away. Each of these reads the module and reports the
// quantity the control actually sets.
struct KitSizeQ : ParamQuantity {
	std::string getDisplayValueString() override {
		// The range spans roughly a 6-inch splash to a 22-inch kick.
		float in = 6.f * std::pow(22.f / 6.f, clamp(getValue(), 0.f, 1.f));
		return string::f("%.0f in (%.0f cm)", in, in * 2.54f);
	}
};
struct KitTensionQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float st = (clamp(getValue(), 0.f, 1.f) - 0.5f) * 1.4f * 12.f;
		return string::f("%+.1f semitones", st);
	}
};
struct KitDecayQ : ParamQuantity {
	std::string getDisplayValueString() override {
		// Depends on SIZE as well, so read it rather than pretending it does not.
		float sz = 0.45f;
		if (module) sz = clamp(module->params[0].getValue(), 0.f, 1.f);
		float sec = 0.08f * std::pow(90.f, clamp(getValue(), 0.f, 1.f)) * (0.6f + 0.8f * sz);
		return sec < 1.f ? string::f("%.0f ms", sec * 1000.f) : string::f("%.2f s", sec);
	}
};
struct KitExciteQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float h = clamp(getValue(), 0.f, 1.f);
		// tau ~ pi*sqrt(m/k), the same numbers strike() uses
		float m = 0.05f - 0.035f * h, k = 1.0e8f * std::pow(0.1f, h);
		float ms = (float)M_PI * std::sqrt(m / k) * 1000.f * 6.f;   // ~ the measured span
		const char* n = h < 0.2f ? "soft felt" : h < 0.45f ? "felt mallet"
		              : h < 0.7f ? "hard mallet" : h < 0.9f ? "wood stick" : "hard stick";
		return string::f("%s, %.1f ms contact", n, ms);
	}
};
struct KitWeightQ : ParamQuantity {
	std::string getDisplayValueString() override {
		return string::f("%.0f g", clamp(getValue(), 0.3f, 2.f) * 35.f);
	}
};
struct KitMaterialQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.f, 1.f);
		const char* n = v < 0.12f ? "drum head" : v < 0.35f ? "stiff head"
		              : v < 0.6f ? "thin plate" : v < 0.85f ? "plate" : "gong";
		return string::f("%s (%.0f%%)", n, v * 100.f);
	}
};
struct KitAirQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.f, 1.f);
		const char* n = v < 0.1f ? "open, no shell" : v < 0.4f ? "shallow shell"
		              : v < 0.75f ? "deep shell" : "sealed kettle (pitched)";
		return string::f("%s (%.0f%%)", n, v * 100.f);
	}
};
struct KitToneQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.f, 1.f);
		const char* n = v < 0.2f ? "partials ring on" : v < 0.45f ? "bright"
		              : v < 0.7f ? "natural" : v < 0.9f ? "dark" : "partials die at once";
		return string::f("%s (%.0f%%)", n, v * 100.f);
	}
};
struct KitBendQ : ParamQuantity {
	std::string getDisplayValueString() override {
		return string::f("%.1f semitones at full strike", clamp(getValue(), 0.f, 1.f) * 4.2f);
	}
};
struct KitResoQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.6f, 1.6f);
		return string::f("%+.1f semitones vs the batter head", 12.f * std::log2(v));
	}
};

// Presets, written as what the instrument IS -- 110 Hz, 1.1 s -- and converted
// to knob positions by inverting the module's own mappings, so they cannot drift
// from what process() actually does with the numbers.
struct KitPreset {
	const char* name;
	float size, tension, material, air, decay, tone, couple, reso,
	      excite, muffle, bend, snare, snareTune, strikeY;
};
static const KitPreset KIT_PRESETS[] = {
	{"Tom",          0.510f, 0.50f, 0.00f, 0.00f, 0.581f, 0.50f, 0.40f, 1.12f, 0.75f, 0.00f, 0.71f, 0.00f, 0.52f, 0.46f},
	{"Floor tom",    0.654f, 0.50f, 0.00f, 0.00f, 0.640f, 0.45f, 0.45f, 1.08f, 0.70f, 0.05f, 0.86f, 0.00f, 0.52f, 0.41f},
	{"Timpani",      0.699f, 0.50f, 0.00f, 1.00f, 0.773f, 0.35f, 0.30f, 1.00f, 0.30f, 0.00f, 0.43f, 0.00f, 0.52f, 0.74f},
	{"Kick",         0.856f, 0.50f, 0.00f, 0.00f, 0.352f, 0.90f, 0.55f, 0.85f, 0.85f, 0.55f, 1.00f, 0.00f, 0.52f, 0.29f},
	{"Snare",        0.282f, 0.50f, 0.00f, 0.00f, 0.370f, 0.80f, 0.50f, 1.30f, 0.90f, 0.00f, 0.57f, 0.80f, 0.22f, 0.52f},
	{"Brush snare",  0.282f, 0.50f, 0.00f, 0.00f, 0.400f, 0.70f, 0.50f, 1.30f, 0.15f, 0.00f, 0.57f, 0.60f, 0.12f, 0.72f},
	{"Gong",         0.799f, 0.50f, 1.00f, 0.00f, 0.689f, 0.20f, 0.20f, 1.00f, 0.50f, 0.00f, 0.29f, 0.00f, 0.52f, 0.52f},
	{"Steel pan",    0.185f, 0.50f, 0.70f, 0.00f, 0.756f, 0.30f, 0.25f, 1.00f, 0.60f, 0.00f, 0.29f, 0.00f, 0.52f, 0.64f},
	{"Frame drum",   0.441f, 0.50f, 0.00f, 0.20f, 0.523f, 0.60f, 0.25f, 1.00f, 0.45f, 0.20f, 1.00f, 0.00f, 0.52f, 0.82f},
	{"Tabla",        0.221f, 0.50f, 0.15f, 0.50f, 0.658f, 0.55f, 0.30f, 1.00f, 0.80f, 0.35f, 1.00f, 0.00f, 0.52f, 0.70f},
};
static const int KIT_NPRESET = (int)(sizeof(KIT_PRESETS) / sizeof(KIT_PRESETS[0]));

struct Kit : Module {
	enum ParamId {
		SIZE_PARAM, TENSION_PARAM, STIFF_PARAM, AIR_PARAM,
		DECAY_PARAM, TONE_PARAM, COUPLE_PARAM, RESO_PARAM,
		EXCITE_PARAM, WEIGHT_PARAM, BEND_PARAM,
		STRIKEX_PARAM, STRIKEY_PARAM,
		MUFFLE_PARAM, MUFFLEANG_PARAM,
		SNARE_PARAM, SNARETHR_PARAM,
		LEVEL_PARAM, STRIKE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		GATE_INPUT, VEL_INPUT, VOCT_INPUT,
		STRIKEX_INPUT, STRIKEY_INPUT,
		SIZE_INPUT, TENSION_INPUT, STIFF_INPUT, AIR_INPUT, MUFFLE_INPUT,
		// APPENDED for the 2026-08 panel, which gives every voice control a CV
		// in. New entries go on the END: inputs serialise by index, so slotting
		// these in beside their siblings above would repatch every saved Kit.
		DECAY_INPUT, TONE_INPUT, EXCITE_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_OUTPUT, HEAD_OUTPUT, SNARE_OUTPUT, OUTPUTS_LEN };
	enum LightId { STRIKE_LIGHT, LIGHTS_LEN };

	sfs::Drum drum;
	dsp::SchmittTrigger gateTrig, strikeBtn;
	int ctl = 0;
	float lastVel = 0.6f;
	float uiFlash = 0.f;
	// Set by the display when you play the head with the mouse. The strike is
	// consumed in process(), never fired from the UI thread.
	float pendVel = 0.f, pendX = 0.f, pendY = 0.f;
	bool  pendHit = false;
	// Mirrors for the display, which must not reach into the audio thread.
	float dispR = 0.55f, dispA = 0.f, dispEnergy = 0.f;
	// The screen must draw what the ENGINE is using, not what the knobs say. CV
	// is summed into locals in process() and never written back to the params,
	// so a display reading params[] shows the knob and silently ignores every
	// patched cable -- which is exactly how it behaved.
	float dispSize = 0.45f, dispTens = 0.5f, dispAir = 0.f;
	float dispExcite = 0.7f, dispWires = 0.f, dispMuffle = 0.f, dispCouple = 0.4f;
	float dispStiff = 0.3f;
	float modeVis[sfs::Drum::NM] = {0.f};
	int   headView = 1;                  // 0 = flat rings, 1 = 3D surface

	Kit() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// SIZE and TENSION both move the pitch, and for an ideal membrane that
		// is ALL they do -- the mode ratios are scale-invariant, so the two are
		// the same control. They only separate through stiffness, the cavity and
		// damping, which is why those exist. SIZE is the slow one: a big drum is
		// low and dark, and it stays dark when you tighten it.
		configParam<KitSizeQ>(SIZE_PARAM, 0.f, 1.f, 0.45f, "Drum size");
		configParam<KitTensionQ>(TENSION_PARAM, 0.f, 1.f, 0.5f, "Head tension");
		configParam<KitMaterialQ>(STIFF_PARAM, 0.f, 1.f, 0.f, "Material");
		configParam<KitAirQ>(AIR_PARAM, 0.f, 1.f, 0.f, "Air cavity");
		configParam<KitDecayQ>(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay");
		configParam<KitToneQ>(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone (how fast partials die)");
		configParam(COUPLE_PARAM, 0.f, 1.f, 0.4f, "Coupling between the two heads", "%", 0.f, 100.f);
		configParam<KitResoQ>(RESO_PARAM, 0.6f, 1.6f, 1.1f, "Resonant head tuning");
		configParam<KitExciteQ>(EXCITE_PARAM, 0.f, 1.f, 0.7f, "Beater");
		configParam<KitWeightQ>(WEIGHT_PARAM, 0.3f, 2.f, 1.f, "Beater weight");
		configParam<KitBendQ>(BEND_PARAM, 0.f, 1.f, 0.25f, "Bend (pitch drop after the hit)");
		configParam(STRIKEX_PARAM, -1.f, 1.f, 0.f, "Strike X");
		configParam(STRIKEY_PARAM, -1.f, 1.f, 0.42f, "Strike Y");
		configParam(MUFFLE_PARAM, 0.f, 1.f, 0.f, "Muffle (a hand on the head)", "%", 0.f, 100.f);
		configParam(MUFFLEANG_PARAM, -1.f, 1.f, 0.f, "Muffle angle");
		configParam(SNARE_PARAM, 0.f, 1.f, 0.f, "Snare wires", "%", 0.f, 100.f);
		configParam(SNARETHR_PARAM, 0.f, 1.f, 0.3f, "Wire tightness (loose buzz to tight snap)", "%", 0.f, 100.f);
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level", "x");
		configButton(STRIKE_PARAM, "Strike");

		configInput(GATE_INPUT, "Gate or trigger");
		configInput(VEL_INPUT, "Velocity (0-10V)");
		configInput(VOCT_INPUT, "1V/oct");
		configInput(STRIKEX_INPUT, "Strike X CV (+/-5V)");
		configInput(STRIKEY_INPUT, "Strike Y CV (+/-5V)");
		configInput(SIZE_INPUT, "Size CV (+/-5V)");
		configInput(TENSION_INPUT, "Tension CV (+/-5V)");
		configInput(STIFF_INPUT, "Material CV (+/-5V)");
		configInput(AIR_INPUT, "Air CV (+/-5V)");
		configInput(MUFFLE_INPUT, "Muffle CV (+/-5V)");
		configInput(DECAY_INPUT, "Decay CV (+/-5V)");
		configInput(TONE_INPUT, "Tone CV (+/-5V)");
		configInput(EXCITE_INPUT, "Beater CV (+/-5V)");
		configOutput(OUT_OUTPUT, "Mix");
		configOutput(HEAD_OUTPUT, "Head only");
		configOutput(SNARE_OUTPUT, "Wires only");
	}

	inline float pv(int p, int in, float lo = 0.f, float hi = 1.f) {
		float v = params[p].getValue();
		if (inputs[in].isConnected()) v += inputs[in].getVoltage() * 0.2f * (hi - lo);
		return clamp(v, lo, hi);
	}

	void process(const ProcessArgs& args) override {
		drum.sr = args.sampleRate;

		// Control rate. The mode layout involves a pow and two sqrts per mode
		// and nothing in it needs to be sample-accurate.
		if (--ctl <= 0) {
			ctl = 32;
			float size = pv(SIZE_PARAM, SIZE_INPUT);
			float tens = pv(TENSION_PARAM, TENSION_INPUT);
			// One pitch, from both. Size spans roughly a 22" kick to a 6" splash
			// and tension is a fifth either way on top of it.
			float f0 = 34.f * std::pow(11.f, 1.f - size) * std::pow(2.f, (tens - 0.5f) * 1.4f);
			if (inputs[VOCT_INPUT].isConnected())
				f0 *= std::pow(2.f, inputs[VOCT_INPUT].getVoltage());
			drum.f0 = clamp(f0, 12.f, 4000.f);

			drum.stiff    = pv(STIFF_PARAM, STIFF_INPUT);
			drum.air      = pv(AIR_PARAM, AIR_INPUT);
			drum.couple   = params[COUPLE_PARAM].getValue();
			drum.resoTune = params[RESO_PARAM].getValue();
			// A big drum rings longer than a small one at the same tension, so
			// decay leans on size as well as on its own knob.
			float dk = pv(DECAY_PARAM, DECAY_INPUT);
			drum.decay = 0.08f * std::pow(90.f, dk) * (0.6f + 0.8f * size);
			drum.tone  = pv(TONE_PARAM, TONE_INPUT) * 1.3f;
			drum.muffle    = pv(MUFFLE_PARAM, MUFFLE_INPUT);
			drum.muffleAng = params[MUFFLEANG_PARAM].getValue() * (float)M_PI;
			drum.bend      = params[BEND_PARAM].getValue() * 0.35f;
			drum.snareAmt  = params[SNARE_PARAM].getValue();
			drum.snareThr  = 0.04f + params[SNARETHR_PARAM].getValue() * 0.5f;
			drum.snareTight = 0.06f + (1.f - params[SNARETHR_PARAM].getValue()) * 0.3f;

			float x = clamp(params[STRIKEX_PARAM].getValue()
			                + inputs[STRIKEX_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
			float y = clamp(params[STRIKEY_PARAM].getValue()
			                + inputs[STRIKEY_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
			float r = std::min(1.f, std::sqrt(x * x + y * y));
			drum.strikeR   = r * 0.97f;
			drum.strikeAng = std::atan2(y, x);
			dispR = r; dispA = drum.strikeAng;
			dispSize = size; dispTens = tens; dispAir = drum.air;
			dispMuffle = drum.muffle; dispCouple = drum.couple;
			dispStiff = drum.stiff;
			dispExcite = pv(EXCITE_PARAM, EXCITE_INPUT);
			dispWires  = clamp(params[SNARE_PARAM].getValue(), 0.f, 1.f);
			// modes first: updateStrike()'s excitation tilt reads ratio[], which
			// updateModes() computes. The other order used last frame's layout.
			drum.updateModes();
			drum.updateStrike();
			dispEnergy = drum.energy;
			for (int k = 0; k < sfs::Drum::NM; k++)
				modeVis[k] = std::fabs(drum.lo[k].value()) * drum.outGain;
		}

		// A strike from the gate, the button, or the display.
		bool fire = false;
		float vel = lastVel;
		if (gateTrig.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f)) {
			vel = inputs[VEL_INPUT].isConnected()
			    ? clamp(inputs[VEL_INPUT].getVoltage() * 0.1f, 0.02f, 1.f) : 0.7f;
			fire = true;
		}
		if (strikeBtn.process(params[STRIKE_PARAM].getValue() > 0.5f)) { vel = 0.7f; fire = true; }
		if (pendHit) {
			pendHit = false; vel = pendVel;
			// A mouse strike sets its own place on the head, and the knobs
			// follow it, so the panel never disagrees with what you just played.
			params[STRIKEX_PARAM].setValue(pendX);
			params[STRIKEY_PARAM].setValue(pendY);
			ctl = 0;
			fire = true;
		}
		if (fire) {
			lastVel = vel;
			float hard = pv(EXCITE_PARAM, EXCITE_INPUT);
			// Velocity is a real approach speed, so everything downstream --
			// contact time, brightness, how far the pitch bends -- follows from
			// the collision rather than from a curve drawn over the top.
			drum.strike(0.4f + vel * 9.f, hard, params[WEIGHT_PARAM].getValue());
			uiFlash = 1.f;
		}

		float head, snare;
		drum.process(head, snare);
		float lvl = params[LEVEL_PARAM].getValue();
		float mix = (head + snare) * lvl;
		outputs[OUT_OUTPUT].setVoltage(sfs::softClip(mix * 5.f));
		outputs[HEAD_OUTPUT].setVoltage(sfs::softClip(head * lvl * 5.f));
		outputs[SNARE_OUTPUT].setVoltage(sfs::softClip(snare * lvl * 5.f));

		uiFlash -= uiFlash * 6.f * args.sampleTime;
		lights[STRIKE_LIGHT].setBrightness(uiFlash);
	}

	void loadPreset(int i) {
		if (i < 0 || i >= KIT_NPRESET) return;
		const KitPreset& p = KIT_PRESETS[i];
		params[SIZE_PARAM].setValue(p.size);
		params[TENSION_PARAM].setValue(p.tension);
		params[STIFF_PARAM].setValue(p.material);
		params[AIR_PARAM].setValue(p.air);
		params[DECAY_PARAM].setValue(p.decay);
		params[TONE_PARAM].setValue(p.tone);
		params[COUPLE_PARAM].setValue(p.couple);
		params[RESO_PARAM].setValue(p.reso);
		params[EXCITE_PARAM].setValue(p.excite);
		params[MUFFLE_PARAM].setValue(p.muffle);
		params[BEND_PARAM].setValue(p.bend);
		params[SNARE_PARAM].setValue(p.snare);
		params[SNARETHR_PARAM].setValue(p.snareTune);
		// Straight up the head: the radius is the tone control, the angle only
		// matters against the muffle.
		params[STRIKEX_PARAM].setValue(0.f);
		params[STRIKEY_PARAM].setValue(p.strikeY);
		ctl = 0;                       // re-solve the layout on the next sample
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "headView", json_integer(headView));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "headView"))
			headView = clamp((int)json_integer_value(j), 0, 1);
	}

	void onReset() override { drum.clear(); }
	void onSampleRateChange() override { drum.sr = APP->engine->getSampleRate(); drum.clear(); }
};

// ── the head ────────────────────────────────────────────────────────────────
// A drum seen from above. It is not a picture of the module's settings, it is
// the drum: click it to play it, and where you click is where it is struck.
struct KitDisplay : OpaqueWidget {
	Kit* module = nullptr;
	std::shared_ptr<Font> font;
	Vec dragFrom;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) return;
		// Load the face here, not in the constructor: the window may not exist
		// yet when the widget is built. Leaving it unloaded is a null deref the
		// first time anything draws text -- f->handle on a null shared_ptr, which
		// faults at address 0x8 and takes Rack down the moment Kit is placed.
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		nvgScissor(args.vg, RECT_ARGS(Rect(Vec(0, 0), box.size)));
		if (!module) drawPreview(args); else drawLive(args);
		nvgResetScissor(args.vg);
	}

	// MATERIAL, drawn. Every other macro had a picture and this one did not,
	// which made it the knob you turned without knowing whether anything had
	// happened. A head's material shows in its SURFACE: skin and mylar are matte
	// and warm, and the stiffer the material gets the more it behaves like
	// metal -- colder, and with a tight specular highlight instead of a soft
	// sheen. So stiffness moves the fill from warm to blue-steel and pulls the
	// highlight in. It is a tint and one gradient, deliberately: the mode rings
	// are drawn on top of this and are the thing you are meant to be reading.
	float stiffAmt() const { return module ? clamp(module->dispStiff, 0.f, 1.f) : 0.3f; }

	void head(const DrawArgs& args, float cx, float cy, float rad) {
		float st = stiffAmt();
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, rad);
		nvgFillColor(args.vg, nvgRGBf(0.20f - 0.07f * st,
		                              0.16f - 0.02f * st,
		                              0.17f + 0.09f * st));
		nvgFill(args.vg);
		// the sheen: broad and faint on skin, tight and bright on metal
		{
			float hx = cx - rad * 0.34f, hy = cy - rad * 0.34f;
			NVGpaint g = nvgRadialGradient(args.vg, hx, hy,
			                               rad * (0.02f + 0.30f * (1.f - st)),
			                               rad * (0.55f + 0.45f * (1.f - st)),
			                               nvgRGBAf(0.62f, 0.72f, 0.95f,
			                                        0.05f + 0.20f * st),
			                               nvgRGBAf(0.62f, 0.72f, 0.95f, 0.f));
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, rad);
			nvgFillPaint(args.vg, g);
			nvgFill(args.vg);
		}
		nvgStrokeColor(args.vg, sfs::SCREEN_LINE);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		// the hoop
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, rad * 0.93f);
		nvgStrokeColor(args.vg, sfs::SCREEN_PURP);
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);
	}

	// The standing wave actually on the head right now: sum over modes of
	// amplitude * J_m(j_mn r) * cos(m theta). Drawn as rings rather than a full
	// raster because the radial part is what the strike controls, and a ring
	// reads as a drum head where a heat map reads as a physics demo.
	// Where the head sits in a wide screen: centred vertically, tucked left, so
	// the remaining width is a usable panel rather than padding.
	float headRad() const { return box.size.y * 0.5f - mm2px(1.2f); }
	// CENTRED. It used to be tucked against the left edge to leave a column for
	// the spectrum; with the scope moved into a corner there is nothing to make
	// room for, and a drum head off to one side of a wide screen reads as a
	// mistake rather than as a layout.
	float headCx()  const { return box.size.x * 0.5f; }

	// The head as a surface rather than a plan. The 2D view can only show the
	// RADIAL part of a mode, because a flat ring has one value; the whole point
	// of a drum is that the modes have angular shape too -- (m,1) has m nodal
	// diameters -- and that is invisible until you tilt it and give it height.
	//
	// The head is drawn as wide as the screen allows rather than as wide as the
	// screen is TALL. Tilted at 0.4 the disc's vertical extent is only 0.8 of
	// its width, so fitting it to the height wasted most of the radius it could
	// have had.
	static constexpr float TILT = 0.40f;

	// SIZE is drawn, not just heard: a 22-inch kick should look like one next to
	// a 6-inch splash. The range is kept off zero so a small drum is still a
	// drum rather than a dot.
	float sizeScale() const {
		return 0.52f + 0.48f * (module ? clamp(module->dispSize, 0.f, 1.f) : 0.55f);
	}
	// EVERY drum here has two heads and a shell between them -- COUPLE and RESO
	// only mean anything because it does -- so the shell is always drawn, and its
	// depth simply follows the drum's size the way a real one does.
	//
	// It used to follow AIR, which was wrong twice: AIR is the CAVITY morph, the
	// thing that pulls the modes into a kettledrum's harmonic series, and a kick
	// wants none of that while having the deepest shell of anything here. So the
	// kick rendered with no shell at all. AIR now closes the BOTTOM instead,
	// which is what a kettle actually is: one head over a sealed bowl.
	float shellDepth(float rad) const { return rad * 0.30f; }
	float airAmt() const {
		return module ? clamp(module->dispAir, 0.f, 1.f) : 0.35f;
	}
	float head3Rad() const {
		// Centred on the WHOLE screen, so the room either side has to clear the
		// spectrum on the right -- otherwise "centred" and "does not overlap"
		// cannot both be true.
		float wAvail = box.size.x - 2.f * mm2px(3.f);
		float byH = (box.size.y - mm2px(5.f)) / (2.f * TILT + 0.34f + 0.34f);
		return std::min(wAvail * 0.5f, byH) * sizeScale();
	}
	// The scope is a CORNER READOUT now, not a column. As a full-height column
	// it took a fifth of the screen to say what the head already says, and it
	// pushed the head off centre to do it. Small and out of the way it still
	// answers the one question the head cannot: which partials are sounding.
	float scopeW() const { return std::min(mm2px(24.f), box.size.x * 0.28f); }
	float scopeH() const { return std::min(mm2px(9.f), box.size.y * 0.24f); }
	void drawScope(const DrawArgs& args) {
		float x1 = box.size.x - mm2px(2.f), x0 = x1 - scopeW();
		float y1 = box.size.y - mm2px(2.f), y0 = y1 - scopeH();
		drawSpectrum(args, x0, y0, x1, y1);
	}
	// The object is rise + tilted disc + shell, so its centre is not the box's.
	// Both the surface and the strike mark must agree about this or the mark
	// floats off the head.
	float head3Cy() const {
		float rad = head3Rad(), hgt = rad * 0.34f;
		float shell = shellDepth(rad);
		return box.size.y * 0.5f - (shell - hgt) * 0.5f;
	}
	float head3Cx() const { return box.size.x * 0.5f; }

	void drawHead3D(const DrawArgs& args) {
		const int RINGS = 12, SECT = 36, ARCS = 6;
		const sfs::MembraneShapes& sh = sfs::membraneShapes();
		float rad = head3Rad(), cx = head3Cx();
		float hgt = rad * 0.34f;
		// The shell. Tied to AIR, which IS the cavity, so the picture says
		// something rather than being a constant box.
		float shell = shellDepth(rad), airv = airAmt();
		float cy = head3Cy();
		float sa = module ? module->dispA : 0.f;

		static float ang[sfs::Drum::NM][SECT + 1];
		for (int k = 0; k < sfs::Drum::NM; k++) {
			int mm = sfs::MEMBRANE_MODES[k].m;
			for (int j = 0; j <= SECT; j++)
				ang[k][j] = std::cos(mm * (2.f * (float)M_PI * (float)j / SECT - sa));
		}
		float z[RINGS + 1][SECT + 1];
		float zmax = 1e-6f;
		for (int i = 0; i <= RINGS; i++) {
			float u = (float)i / RINGS;
			for (int j = 0; j <= SECT; j++) {
				float v = 0.f;
				for (int k = 0; k < sfs::Drum::NM; k++) {
					float a = module ? module->modeVis[k] : 0.55f * std::exp(-k * 0.30f);
					if (a < 1e-4f) continue;
					v += a * sh.at(k, u) * ang[k][j];
				}
				z[i][j] = v;
				zmax = std::max(zmax, std::fabs(v));
			}
		}
		float zs = hgt / std::max(zmax, 0.35f);

		// A taut head pulls its grid out toward the rim; a slack one lets it
		// gather in the middle. Same rings, redistributed -- which is what
		// tension does to a real head's response, and it reads instantly.
		float tens = module ? clamp(module->dispTens, 0.f, 1.f) : 0.5f;
		float warp = 1.f - 0.55f * (tens - 0.5f);        // <1 pushes rings outward
		auto PX = [&](int i, int j, float& X, float& Y) {
			float u = std::pow((float)i / RINGS, warp);
			float th = 2.f * (float)M_PI * (float)j / SECT;
			X = cx + std::cos(th) * u * rad;
			Y = cy + std::sin(th) * u * rad * TILT - z[i][j] * zs;
		};
		// Excited areas warm toward the plugin's orange. Depth cue on top: the
		// far half of the surface sits back a little.
		auto col = [&](float amp, float th) {
			float hot = clamp(amp, 0.f, 1.f);
			float far = 0.72f + 0.28f * (0.5f + 0.5f * std::sin(th));
			return nvgRGBAf(0.00f + 0.92f * hot, 0.59f - 0.19f * hot, 0.87f - 0.69f * hot,
			                (0.30f + 0.60f * hot) * far);
		};

		nvgLineCap(args.vg, NVG_ROUND);

		// ── the shell, drawn first so the head sits on top of it ──────────────
		if (shell >= 1.f) {
			nvgBeginPath(args.vg);
			for (int j = 0; j <= SECT; j++) {
				float th = 2.f * (float)M_PI * (float)j / SECT;
				float X = cx + std::cos(th) * rad, Y = cy + std::sin(th) * rad * TILT + shell;
				if (j == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
			}
			nvgStrokeColor(args.vg, nvgRGBAf(0.21f, 0.21f, 0.30f, 0.95f));
			nvgStrokeWidth(args.vg, 1.2f);
			nvgStroke(args.vg);
			for (int j = 0; j <= SECT; j += 3) {
				if (shell < 1.f) break;
				float th = 2.f * (float)M_PI * (float)j / SECT;
				if (std::sin(th) < -0.15f) continue;          // the back wall is hidden
				float X = cx + std::cos(th) * rad, Y0 = cy + std::sin(th) * rad * TILT;
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, X, Y0); nvgLineTo(args.vg, X, Y0 + shell);
				nvgStrokeColor(args.vg, nvgRGBAf(0.21f, 0.21f, 0.30f, 0.85f));
				nvgStrokeWidth(args.vg, 0.9f);
				nvgStroke(args.vg);
			}
		}

		// AIR closes the bottom. At zero it is an open shell with a resonant head
		// across it, which is a tom or a kick; wound up it bellies into a sealed
		// bowl, which is a kettledrum -- and a kettledrum is exactly what the
		// harmonic series AIR imposes belongs to. Same knob, same story, twice.
		if (airv > 0.02f) {
			int NB = 5;
			for (int b = 1; b <= NB; b++) {
				float t = (float)b / NB;                    // 0 at the rim, 1 at the pole
				float rr = rad * std::sqrt(std::max(0.f, 1.f - t * t));
				float yy = cy + shell + airv * rad * 0.55f * t;
				nvgBeginPath(args.vg);
				for (int j = 0; j <= SECT; j++) {
					float th = 2.f * (float)M_PI * (float)j / SECT;
					float X = cx + std::cos(th) * rr, Y = yy + std::sin(th) * rr * TILT;
					if (j == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
				}
				nvgStrokeColor(args.vg, nvgRGBAf(0.21f, 0.21f, 0.30f, 0.30f + 0.55f * airv));
				nvgStrokeWidth(args.vg, 0.8f);
				nvgStroke(args.vg);
			}
		}
		// The resonant head and its wires only exist while the bottom is open.
		if (airv < 0.85f) drawWires(args, cx, cy, rad, shell);

		// ── the surface: rings in arcs so colour is local, then spokes ────────
		for (int i = RINGS; i >= 1; i--) {
			int per = SECT / ARCS;
			for (int a = 0; a < ARCS; a++) {
				int j0 = a * per, j1 = j0 + per;
				float amp = 0.f;
				for (int j = j0; j <= j1; j++) amp = std::max(amp, std::fabs(z[i][j]) * zs / hgt);
				nvgBeginPath(args.vg);
				for (int j = j0; j <= j1; j++) {
					float X, Y; PX(i, j, X, Y);
					if (j == j0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
				}
				float thm = 2.f * (float)M_PI * (float)(j0 + per / 2) / SECT;
				nvgStrokeColor(args.vg, i == RINGS ? sfs::SCREEN_LINE : col(amp, thm));
				nvgStrokeWidth(args.vg, i == RINGS ? 1.5f : 0.9f + amp * 1.6f);
				nvgStroke(args.vg);
			}
		}
		for (int j = 0; j < SECT; j += 2) {
			float amp = 0.f;
			for (int i = 0; i <= RINGS; i++) amp = std::max(amp, std::fabs(z[i][j]) * zs / hgt);
			nvgBeginPath(args.vg);
			for (int i = 0; i <= RINGS; i++) {
				float X, Y; PX(i, j, X, Y);
				if (i == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
			}
			float th = 2.f * (float)M_PI * (float)j / SECT;
			NVGcolor c = col(amp * 0.8f, th);
			c.a *= 0.65f;
			nvgStrokeColor(args.vg, c);
			nvgStrokeWidth(args.vg, 0.7f);
			nvgStroke(args.vg);
		}
	}

	void drawLive(const DrawArgs& args) {
		float cy = box.size.y * 0.5f;
		float rad = headRad(), cx = headCx();
		if (module && module->headView == 1) {
			drawHead3D(args);
			drawScope(args);
			drawStrikeMark(args, head3Cx(), head3Cy(), head3Rad());
			drawReadout(args, head3Cx());
			return;
		}
		head(args, cx, cy, rad);
		drawScope(args);

		const sfs::MembraneShapes& sh = sfs::membraneShapes();
		const int RINGS = 22;
		for (int i = RINGS; i >= 1; i--) {
			float u = (float)i / RINGS;
			float amp = 0.f;
			for (int k = 0; k < sfs::Drum::NM; k++)
				amp += module->modeVis[k] * sh.at(k, u);
			amp = clamp(std::fabs(amp) * 0.5f, 0.f, 1.f);
			if (amp < 0.004f) continue;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, rad * u * 0.93f);
			nvgStrokeColor(args.vg, nvgRGBAf(0.0f, 0.59f, 0.87f, amp * 0.85f));
			nvgStrokeWidth(args.vg, 1.f + amp * 2.5f);
			nvgStroke(args.vg);
		}

		// where the muffle sits
		float mu = module->dispMuffle;
		if (mu > 0.01f) {
			float ma = module->params[Kit::MUFFLEANG_PARAM].getValue() * (float)M_PI;
			float mr = rad * 0.82f * 0.93f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx + std::cos(ma) * mr, cy + std::sin(ma) * mr,
			          mm2px(1.1f + mu * 2.2f));
			nvgFillColor(args.vg, nvgRGBAf(0.55f, 0.55f, 0.62f, 0.25f + mu * 0.5f));
			nvgFill(args.vg);
		}

		drawStrikeMark(args, cx, cy, rad);
		drawReadout(args, cx);
	}

	// Both views mark the strike the same way; in 3D it is flattened onto the
	// tilted plane so it still sits where you clicked.
	// The beater, drawn as what it is. A felt mallet is broad and its edge is
	// soft; a stick is small and hard. Those are the same two things EXCITER
	// changes in the sound -- contact area and contact time -- so the icon and
	// the tone move together instead of the icon being decoration.
	void drawStrikeMark(const DrawArgs& args, float cx, float cy, float rad) {
		if (!module) return;
		bool three = module->headView == 1;
		float tilt = three ? TILT : 0.93f;
		float sr = module->dispR * (three ? 1.f : 0.97f), sa = module->dispA;
		float sx = cx + std::cos(sa) * sr * rad * (three ? 1.f : 0.93f);
		float sy = cy + std::sin(sa) * sr * rad * tilt;
		float f = clamp(module->uiFlash, 0.f, 1.f);
		float hard = clamp(module->dispExcite, 0.f, 1.f);
		float r = mm2px(2.6f - 1.5f * hard) + f * mm2px(1.8f);
		// soft beater: a wide halo and no rim. stick: tight, with a hard edge.
		NVGpaint g = nvgRadialGradient(args.vg, sx, sy, r * (0.15f + 0.70f * hard), r,
		                               nvgRGBAf(0.93f, 0.40f, 0.18f, 0.45f + f * 0.55f),
		                               nvgRGBAf(0.93f, 0.40f, 0.18f, 0.f));
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, sx, sy, r);
		nvgFillPaint(args.vg, g);
		nvgFill(args.vg);
		if (hard > 0.35f) {
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, sx, sy, r * 0.55f);
			nvgStrokeColor(args.vg, nvgRGBAf(0.97f, 0.55f, 0.30f,
			                                 (hard - 0.35f) / 0.65f * (0.5f + f * 0.5f)));
			nvgStrokeWidth(args.vg, 0.8f + hard);
			nvgStroke(args.vg);
		}
	}

	// The wires, on the bottom head. The first version spread seven of them
	// evenly across the whole diameter, which is not what a snare looks like at
	// all -- it read as a set of unrelated horizontal lines drawn through the
	// drum. Real snare wires are about twenty strands bunched into a BAND a
	// couple of inches wide, running parallel from a strainer on one side to a
	// butt plate on the other, so nearly all of them are close to full length
	// and they sit together rather than dividing the head up.
	void drawWires(const DrawArgs& args, float cx, float cy, float rad, float shell) {
		// THREE SPRINGS, NOT A BAND OF STRANDS. Sixteen straight lines under the
		// shell merged into a grey slab at this size and read as shading rather
		// than as hardware. A snare wire IS a coiled spring, and three of them
		// drawn as springs say "snare" at a glance where sixteen lines said
		// "smudge".
		//
		// They arrive one at a time so the knob's whole travel does something:
		// the centre wire fades in over the first third and is solid at 33%,
		// the top over the second and solid at 66%, the bottom over the last.
		// A single opacity ramp on all three would have made 0-100% one gesture
		// with nothing to see in the middle of it.
		float w = module ? clamp(module->dispWires, 0.f, 1.f) : 0.85f;
		if (w < 0.005f) return;
		const float A[3] = {clamp(w / 0.33f, 0.f, 1.f),               // centre
		                    clamp((w - 0.33f) / 0.33f, 0.f, 1.f),     // top
		                    clamp((w - 0.66f) / 0.34f, 0.f, 1.f)};    // bottom
		const float O[3] = {0.f, -1.f, 1.f};                          // across the band

		// TIGHT is the coil pitch. The parameter already means how tightly the
		// wires are strained, and a strained spring has its coils closer
		// together -- so the control gets a picture for free and an honest one.
		float tight = module ? clamp(module->params[Kit::SNARETHR_PARAM].getValue(),
		                             0.f, 1.f) : 0.5f;
		// Tight enough to read as ONE snare unit strung under the bottom head.
		// Spread wide, the three springs stopped being a set of wires and
		// became three separate objects draped across the shell.
		float band = rad * 0.13f;
		float yb = cy + shell;
		float halfEnd = 0.f;
		for (int i = 0; i < 3; i++) {
			if (A[i] < 0.01f) continue;
			float o = O[i];
			float rr = o * band / std::max(rad, 1.f);
			float half = std::sqrt(std::max(0.f, 1.f - rr * rr)) * rad;
			halfEnd = std::max(halfEnd, half);
			float yy = yb + o * band * TILT;
			// The coil: a sine across the wire's own axis. Its wavelength is set
			// in millimetres rather than as a fixed number of turns, so a big
			// drum shows more coils than a small one instead of the same spring
			// stretched to fit.
			float lam = mm2px(4.4f - 2.2f * tight);
			float amp = mm2px(1.05f);
			int steps = clamp((int)(2.f * half / (lam / 8.f)), 24, 420);
			nvgBeginPath(args.vg);
			for (int k = 0; k <= steps; k++) {
				float t = (float)k / steps;
				float x = cx - half + t * 2.f * half;
				float ph = (x - (cx - half)) / std::max(lam, 1.f) * 2.f * (float)M_PI;
				float y = yy + std::sin(ph) * amp;
				if (k == 0) nvgMoveTo(args.vg, x, y);
				else        nvgLineTo(args.vg, x, y);
			}
			nvgStrokeColor(args.vg, nvgRGBAf(0.86f, 0.87f, 0.92f, 0.10f + 0.42f * A[i]));
			nvgStrokeWidth(args.vg, 0.7f);
			nvgStroke(args.vg);
			// the straight core the coil is wound on, which is what keeps a
			// sine from reading as a squiggle
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, cx - half, yy);
			nvgLineTo(args.vg, cx + half, yy);
			nvgStrokeColor(args.vg, nvgRGBAf(0.72f, 0.74f, 0.82f, 0.18f * A[i]));
			nvgStrokeWidth(args.vg, 0.5f);
			nvgStroke(args.vg);
		}
		if (halfEnd <= 0.f) return;
		// strainer and butt plate: the wires are held at both ends, and without
		// them the springs just stop in mid air.
		float aMax = std::max(A[0], std::max(A[1], A[2]));
		for (int sgn = -1; sgn <= 1; sgn += 2) {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, cx + sgn * halfEnd - (sgn < 0 ? 0.f : mm2px(1.2f)),
			        yb - band * TILT - mm2px(0.6f), mm2px(1.2f),
			        2.f * band * TILT + mm2px(1.2f));
			nvgFillColor(args.vg, nvgRGBAf(0.72f, 0.74f, 0.80f, 0.20f + 0.40f * aMax));
			nvgFill(args.vg);
		}
	}
	void drawReadout(const DrawArgs& args, float cx) {
		if (!module) return;
		if (!font || font->handle < 0) return;
		sfs::screenFont(args.vg, font, sfs::TYPE_SCREEN);
		nvgFillColor(args.vg, sfs::SCREEN_DIM);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
		nvgText(args.vg, mm2px(1.4f), mm2px(1.2f),
		        string::f("%.0f Hz", module->drum.f0).c_str(), NULL);
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
		float pct = module->dispR * 100.f;
		nvgText(args.vg, box.size.x - mm2px(1.4f), mm2px(1.2f),
		        string::f("%.0f%% out", pct).c_str(), NULL);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
		nvgFillColor(args.vg, sfs::SCREEN_PMID);
		nvgText(args.vg, cx, box.size.y - mm2px(1.f), "CLICK THE HEAD TO PLAY", NULL);
	}

	// The browser thumbnail. Without this the module is a dark slab in the
	// library and says nothing about what it is.
	// One bar per mode, tallest at the fundamental: the 1/omega excitation tilt
	// and the frequency-dependent damping are both visible here and nowhere else.
	void drawSpectrum(const DrawArgs& args, float x0, float y0, float x1, float y1) {
		if (x1 - x0 < mm2px(8.f)) return;
		float h = y1 - y0;
		float w = (x1 - x0) / (float)sfs::Drum::NM;
		for (int k = 0; k < sfs::Drum::NM; k++) {
			float a = module ? clamp(module->modeVis[k] * 2.2f, 0.f, 1.f)
			                 : 0.9f * std::exp(-k * 0.12f);
			float bh = 1.f + a * h;
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x0 + k * w, y1 - bh, std::max(w - 0.8f, 0.8f), bh);
			nvgFillColor(args.vg, a > 0.02f
			             ? nvgRGBAf(0.0f, 0.59f, 0.87f, 0.35f + a * 0.65f)
			             : sfs::SCREEN_PURP);
			nvgFill(args.vg);
		}
	}

	// The browser thumbnail shows the DEFAULT view, which is 3D. drawHead3D
	// already copes with module == NULL by standing in a plausible mode mix, so
	// the preview is the same code rather than a second drawing to keep in step.
	void drawPreview(const DrawArgs& args) {
		drawHead3D(args);
		drawScope(args);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, head3Cx() + head3Rad() * 0.42f,
		          head3Cy() - head3Rad() * 0.30f * TILT, mm2px(1.6f));
		nvgFillColor(args.vg, nvgRGBAf(0.93f, 0.40f, 0.18f, 0.9f));
		nvgFill(args.vg);
	}

	void hit(Vec p, float vel) {
		if (!module) return;
		// THE 3D VIEW HAS ITS OWN GEOMETRY and this was still inverting the flat
		// one: a different centre, a different radius, and no tilt at all. So a
		// click right of centre mapped past the rim, clamped, and landed on the
		// edge -- or missed the head entirely and played nothing. The projection
		// is X = cx + x*r and Y = cy + y*r*TILT, so undoing it means dividing the
		// vertical by the tilt as well.
		float cx, cy, rad, sq;
		if (module->headView == 1) {
			cx = head3Cx(); cy = head3Cy(); rad = head3Rad(); sq = TILT;
		} else {
			cx = headCx(); cy = box.size.y * 0.5f; rad = headRad() * 0.93f; sq = 1.f;
		}
		float x = (p.x - cx) / rad, y = (p.y - cy) / (rad * sq);
		float r = std::sqrt(x * x + y * y);
		if (r > 1.f) { x /= r; y /= r; }
		module->pendX = clamp(x, -1.f, 1.f);
		module->pendY = clamp(y, -1.f, 1.f);
		module->pendVel = vel;
		module->pendHit = true;
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			// Velocity from where you land: the rim is where you play rimshots,
			// and it saves a modifier key.
			hit(e.pos, 0.85f);
			dragFrom = e.pos;
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	// Dragging across the head keeps striking, which is how you get a roll out
	// of a mouse.
	void onDragHover(const DragHoverEvent& e) override {
		if (e.origin == this) {
			Vec d = e.pos.minus(dragFrom);
			if (std::sqrt(d.x * d.x + d.y * d.y) > mm2px(2.2f)) {
				hit(e.pos, 0.45f);
				dragFrom = e.pos;
			}
		}
		OpaqueWidget::onDragHover(e);
	}
};

struct KitWidget : ModuleWidget {
	void appendContextMenu(Menu* menu) override {
		Kit* m = dynamic_cast<Kit*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);
		// These are the instruments the engine was measured against while it was
		// being built, so they are also the shortest route to hearing whether a
		// change broke something.
		menu->addChild(createIndexPtrSubmenuItem("Head view",
			{"Flat", "3D"}, &m->headView));
		menu->addChild(createSubmenuItem("Instruments", "", [=](Menu* sub) {
			for (int i = 0; i < KIT_NPRESET; i++) {
				int idx = i;
				sub->addChild(createMenuItem(KIT_PRESETS[i].name, "",
				                             [=]() { m->loadPreset(idx); }));
			}
		}));
	}

	KitWidget(Kit* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/kit.svg")));
		using sfs::hp;

		// NO PanelLabels, and no title. This panel's artwork carries its own text
		// as outlined paths -- which Rack DOES render, unlike <text> -- so
		// drawing them again in Figtree printed every label twice, half a
		// millimetre out. Slice learned this first; see its widget. The grid
		// below is read from the art, which is now the source of the layout.
		KitDisplay* disp = new KitDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(5.08f, 10.16f));
		disp->box.size = mm2px(Vec(101.58f, 45.71f));
		addChild(disp);

		// ── the 2026-08 grid: eight columns, four rows ─────────────────────
		// Transcribed from the panel art, which is authored at 11.813 units/mm
		// (1320 x 1518 for 22HP). Every control is a trimpot: sixteen of them
		// on one pitch reads as one instrument, where four sizes of knob read
		// as four separate ideas competing for the same panel.
		static const float KX[8] = {10.96f, 23.74f, 36.53f, 49.31f,
		                            62.09f, 74.87f, 87.66f, 100.44f};
		static const float Y_VOICE = 70.26f;   // what the drum IS
		static const float Y_CHAR  = 85.50f;   // how it is played and dressed
		static const float Y_CV    = 102.43f;  // one CV in per voice control
		static const float Y_PERF  = 119.36f;  // the transport row

		struct K { int p; const char* t; };
		// Row 1 and row 3 are the SAME EIGHT in the same order, so a cable
		// hangs directly under the control it modulates and the pairing needs
		// no label to explain it.
		const K voice[8] = {
			{Kit::SIZE_PARAM, "SIZE"},   {Kit::TENSION_PARAM, "TENSION"},
			{Kit::STIFF_PARAM, "MATERIAL"}, {Kit::AIR_PARAM, "AIR"},
			{Kit::DECAY_PARAM, "DECAY"}, {Kit::TONE_PARAM, "TONE"},
			{Kit::EXCITE_PARAM, "EXCITER"}, {Kit::MUFFLE_PARAM, "MUFFLE"}};
		for (int i = 0; i < 8; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(KX[i], Y_VOICE)), module, voice[i].p));
		}

		// HIT stays a BUTTON. It is a momentary strike, not a value, and the
		// only reason it sits in a row of trimpots is that VCVButton is 6.10mm
		// against Trimpot's 6.05 -- near enough to hold the grid.
		const K chr[7] = {
			{Kit::COUPLE_PARAM, "COUPLE"}, {Kit::RESO_PARAM, "RESO"},
			{Kit::BEND_PARAM, "BEND"},     {Kit::WEIGHT_PARAM, "WEIGHT"},
			{Kit::SNARE_PARAM, "WIRES"},   {Kit::SNARETHR_PARAM, "TIGHT"},
			{Kit::LEVEL_PARAM, "LEVEL"}};
		for (int i = 0; i < 7; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(KX[i], Y_CHAR)), module, chr[i].p));
		}
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(KX[7], Y_CHAR)), module, Kit::STRIKE_PARAM, Kit::STRIKE_LIGHT));

		struct J { int id; const char* t; };
		const J cv[8] = {
			{Kit::SIZE_INPUT, "SIZE"},   {Kit::TENSION_INPUT, "TENSION"},
			{Kit::STIFF_INPUT, "MAT"},   {Kit::AIR_INPUT, "AIR"},
			{Kit::DECAY_INPUT, "DECAY"}, {Kit::TONE_INPUT, "TONE"},
			{Kit::EXCITE_INPUT, "EXCITER"}, {Kit::MUFFLE_INPUT, "MUFFLE"}};
		for (int i = 0; i < 8; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(KX[i], Y_CV)), module, cv[i].id));
		}

		// ── THE TRANSPORT ROW (house style) ────────────────────────────────
		// The bottom row is performance data in and out, and nothing else:
		// GATE, V/OCT, VEL first and in that order, then whatever else the
		// instrument is played with, then its outputs on a plate at the right.
		// Per-parameter CV belongs above, beside its control. See
		// docs/conventions/panel-design.md.
		const J perf[5] = {{Kit::GATE_INPUT, "GATE"}, {Kit::VOCT_INPUT, "V/OCT"},
		                   {Kit::VEL_INPUT, "VEL"},   {Kit::STRIKEX_INPUT, "X"},
		                   {Kit::STRIKEY_INPUT, "Y"}};
		for (int i = 0; i < 5; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(KX[i], Y_PERF)), module, perf[i].id));
		}
		const J outs[3] = {{Kit::HEAD_OUTPUT, "HEAD"}, {Kit::SNARE_OUTPUT, "WIRES"},
		                   {Kit::OUT_OUTPUT, "MIX"}};
		for (int i = 0; i < 3; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(KX[5 + i], Y_PERF)),
			                                           module, outs[i].id));
		}
	}
};

Model* modelKit = createModel<Kit, KitWidget>("Kit");
