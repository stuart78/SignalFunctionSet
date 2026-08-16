#include "plugin.hpp"
#include "panel-style.hpp"
#include "membrane.hpp"
#include "waveguide.hpp"   // softClip
#include <cmath>

// Skin -- a struck membrane, modelled mode by mode.
//
// The DSP lives in membrane.hpp so it can be measured without Rack in the way,
// which is how the mallet's 38 ms contact times and 296-bounce chatter were
// found. This file is the instrument around it: the parameters, the strike, and
// a head you can play with the mouse.

// Presets, written as what the instrument IS -- 110 Hz, 1.1 s -- and converted
// to knob positions by inverting the module's own mappings, so they cannot drift
// from what process() actually does with the numbers.
struct SkinPreset {
	const char* name;
	float size, tension, material, air, decay, tone, couple, reso,
	      excite, muffle, bend, snare, snareTune, strikeY;
};
static const SkinPreset SKIN_PRESETS[] = {
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
static const int SKIN_NPRESET = (int)(sizeof(SKIN_PRESETS) / sizeof(SKIN_PRESETS[0]));

struct Skin : Module {
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
	float modeVis[sfs::Drum::NM] = {0.f};
	int   headView = 1;                  // 0 = flat rings, 1 = 3D surface

	Skin() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// SIZE and TENSION both move the pitch, and for an ideal membrane that
		// is ALL they do -- the mode ratios are scale-invariant, so the two are
		// the same control. They only separate through stiffness, the cavity and
		// damping, which is why those exist. SIZE is the slow one: a big drum is
		// low and dark, and it stays dark when you tighten it.
		configParam(SIZE_PARAM, 0.f, 1.f, 0.45f, "Size", "%", 0.f, 100.f);
		configParam(TENSION_PARAM, 0.f, 1.f, 0.5f, "Tension", "%", 0.f, 100.f);
		configParam(STIFF_PARAM, 0.f, 1.f, 0.f, "Material", "%", 0.f, 100.f);
		configParam(AIR_PARAM, 0.f, 1.f, 0.f, "Air (cavity)", "%", 0.f, 100.f);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay", "%", 0.f, 100.f);
		configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone", "%", 0.f, 100.f);
		configParam(COUPLE_PARAM, 0.f, 1.f, 0.4f, "Head coupling", "%", 0.f, 100.f);
		configParam(RESO_PARAM, 0.6f, 1.6f, 1.1f, "Resonant head tune", "x");
		configParam(EXCITE_PARAM, 0.f, 1.f, 0.7f, "Exciter (soft to stick)", "%", 0.f, 100.f);
		configParam(WEIGHT_PARAM, 0.3f, 2.f, 1.f, "Beater weight", "x");
		configParam(BEND_PARAM, 0.f, 1.f, 0.25f, "Bend (tension modulation)", "%", 0.f, 100.f);
		configParam(STRIKEX_PARAM, -1.f, 1.f, 0.f, "Strike X");
		configParam(STRIKEY_PARAM, -1.f, 1.f, 0.42f, "Strike Y");
		configParam(MUFFLE_PARAM, 0.f, 1.f, 0.f, "Muffle", "%", 0.f, 100.f);
		configParam(MUFFLEANG_PARAM, -1.f, 1.f, 0.f, "Muffle angle");
		configParam(SNARE_PARAM, 0.f, 1.f, 0.f, "Wires", "%", 0.f, 100.f);
		configParam(SNARETHR_PARAM, 0.f, 1.f, 0.3f, "Wire tightness", "%", 0.f, 100.f);
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
			float dk = params[DECAY_PARAM].getValue();
			drum.decay = 0.08f * std::pow(90.f, dk) * (0.6f + 0.8f * size);
			drum.tone  = params[TONE_PARAM].getValue() * 1.3f;
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
			float hard = clamp(params[EXCITE_PARAM].getValue(), 0.f, 1.f);
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
		if (i < 0 || i >= SKIN_NPRESET) return;
		const SkinPreset& p = SKIN_PRESETS[i];
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
struct SkinDisplay : OpaqueWidget {
	Skin* module = nullptr;
	std::shared_ptr<Font> font;
	Vec dragFrom;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) return;
		// Load the face here, not in the constructor: the window may not exist
		// yet when the widget is built. Leaving it unloaded is a null deref the
		// first time anything draws text -- f->handle on a null shared_ptr, which
		// faults at address 0x8 and takes Rack down the moment Skin is placed.
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		nvgScissor(args.vg, RECT_ARGS(Rect(Vec(0, 0), box.size)));
		if (!module) drawPreview(args); else drawLive(args);
		nvgResetScissor(args.vg);
	}

	void head(const DrawArgs& args, float cx, float cy, float rad) {
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, rad);
		nvgFillColor(args.vg, nvgRGB(0x22, 0x22, 0x3E));
		nvgFill(args.vg);
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
	float headCx()  const { return mm2px(1.2f) + headRad(); }

	// The head as a surface rather than a plan. The 2D view can only show the
	// RADIAL part of a mode, because a flat ring has one value; the whole point
	// of a drum is that the modes have angular shape too -- (m,1) has m nodal
	// diameters -- and that is invisible until you tilt it and give it height.
	void drawHead3D(const DrawArgs& args, float cx, float cy, float rad) {
		const int RINGS = 11, SECT = 30;
		const sfs::MembraneShapes& sh = sfs::membraneShapes();
		const float TILT = 0.40f;               // cosine of the viewing angle
		float hgt = rad * 0.42f;
		float sa = module ? module->dispA : 0.f;

		// Angular factor per (mode, sector). A strike orients each doublet along
		// itself, so the angle is measured from where the head was last hit.
		static float ang[sfs::Drum::NM][SECT + 1];
		for (int k = 0; k < sfs::Drum::NM; k++) {
			int mm = sfs::MEMBRANE_MODES[k].m;
			for (int j = 0; j <= SECT; j++) {
				float th = 2.f * (float)M_PI * (float)j / SECT;
				ang[k][j] = std::cos(mm * (th - sa));
			}
		}
		float z[RINGS + 1][SECT + 1];
		float zmax = 1e-6f;
		for (int i = 0; i <= RINGS; i++) {
			float u = (float)i / RINGS;
			for (int j = 0; j <= SECT; j++) {
				float v = 0.f;
				for (int k = 0; k < sfs::Drum::NM; k++) {
					float a = module ? module->modeVis[k]
					                 : 0.55f * std::exp(-k * 0.30f);
					if (a < 1e-4f) continue;
					v += a * sh.at(k, u) * ang[k][j];
				}
				z[i][j] = v;
				zmax = std::max(zmax, std::fabs(v));
			}
		}
		float zs = hgt / std::max(zmax, 0.35f);      // never blow a quiet head up

		auto px = [&](int i, int j, float& X, float& Y) {
			float u = (float)i / RINGS, th = 2.f * (float)M_PI * (float)j / SECT;
			X = cx + std::cos(th) * u * rad;
			Y = cy + std::sin(th) * u * rad * TILT - z[i][j] * zs;
		};
		// rim first, then the rings inward, then the spokes: painter's order, so
		// the near edge of the surface covers the far one.
		nvgLineCap(args.vg, NVG_ROUND);
		for (int i = RINGS; i >= 1; i--) {
			nvgBeginPath(args.vg);
			for (int j = 0; j <= SECT; j++) {
				float X, Y; px(i, j, X, Y);
				if (j == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
			}
			float lit = clamp(std::fabs(z[i][SECT / 4]) * zs / std::max(hgt, 1.f), 0.f, 1.f);
			nvgStrokeColor(args.vg, i == RINGS
			               ? sfs::SCREEN_LINE
			               : nvgRGBAf(0.0f, 0.59f, 0.87f, 0.28f + 0.62f * lit));
			nvgStrokeWidth(args.vg, i == RINGS ? 1.4f : 0.9f);
			nvgStroke(args.vg);
		}
		for (int j = 0; j < SECT; j += 2) {
			nvgBeginPath(args.vg);
			for (int i = 0; i <= RINGS; i++) {
				float X, Y; px(i, j, X, Y);
				if (i == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
			}
			nvgStrokeColor(args.vg, nvgRGBAf(0.0f, 0.59f, 0.87f, 0.20f));
			nvgStrokeWidth(args.vg, 0.7f);
			nvgStroke(args.vg);
		}
	}

	void drawLive(const DrawArgs& args) {
		float cy = box.size.y * 0.5f;
		float rad = headRad(), cx = headCx();
		if (module && module->headView == 1) {
			drawHead3D(args, cx, cy, rad);
			drawSpectrum(args, cx + rad + mm2px(4.f), box.size.x - mm2px(2.f));
			drawStrikeMark(args, cx, cy, rad);
			drawReadout(args, cx);
			return;
		}
		head(args, cx, cy, rad);
		drawSpectrum(args, cx + rad + mm2px(4.f), box.size.x - mm2px(2.f));

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
		float mu = module->params[Skin::MUFFLE_PARAM].getValue();
		if (mu > 0.01f) {
			float ma = module->params[Skin::MUFFLEANG_PARAM].getValue() * (float)M_PI;
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
	void drawStrikeMark(const DrawArgs& args, float cx, float cy, float rad) {
		if (!module) return;
		bool three = module->headView == 1;
		float tilt = three ? 0.40f : 0.93f;
		float sr = module->dispR * (three ? 1.f : 0.97f), sa = module->dispA;
		float sx = cx + std::cos(sa) * sr * rad * (three ? 1.f : 0.93f);
		float sy = cy + std::sin(sa) * sr * rad * tilt;
		float f = clamp(module->uiFlash, 0.f, 1.f);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, sx, sy, mm2px(1.3f) + f * mm2px(2.2f));
		nvgFillColor(args.vg, nvgRGBAf(0.93f, 0.40f, 0.18f, 0.35f + f * 0.65f));
		nvgFill(args.vg);
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
	void drawSpectrum(const DrawArgs& args, float x0, float x1) {
		if (x1 - x0 < mm2px(8.f)) return;
		float y1 = box.size.y - mm2px(4.f), h = box.size.y - mm2px(9.f);
		float w = (x1 - x0) / (float)sfs::Drum::NM;
		for (int k = 0; k < sfs::Drum::NM; k++) {
			float a = module ? clamp(module->modeVis[k] * 2.2f, 0.f, 1.f)
			                 : 0.9f * std::exp(-k * 0.12f);
			float bh = 1.5f + a * h;
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x0 + k * w, y1 - bh, std::max(w - 1.2f, 1.f), bh);
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
		float cy = box.size.y * 0.5f;
		float rad = headRad(), cx = headCx();
		drawHead3D(args, cx, cy, rad);
		drawSpectrum(args, cx + rad + mm2px(4.f), box.size.x - mm2px(2.f));
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx + rad * 0.42f, cy - rad * 0.30f * 0.40f, mm2px(1.6f));
		nvgFillColor(args.vg, nvgRGBAf(0.93f, 0.40f, 0.18f, 0.9f));
		nvgFill(args.vg);
		return;
		for (int i = 1; i <= 7; i++) {
			float u = i / 7.f;
			float a = 0.55f * std::fabs(std::cos(u * 4.2f)) * (1.f - u * 0.5f);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, rad * u * 0.93f);
			nvgStrokeColor(args.vg, nvgRGBAf(0.0f, 0.59f, 0.87f, a));
			nvgStrokeWidth(args.vg, 1.f + a * 2.2f);
			nvgStroke(args.vg);
		}
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx + rad * 0.42f, cy - rad * 0.30f, mm2px(1.6f));
		nvgFillColor(args.vg, nvgRGBAf(0.93f, 0.40f, 0.18f, 0.9f));
		nvgFill(args.vg);
	}

	void hit(Vec p, float vel) {
		if (!module) return;
		float cy = box.size.y * 0.5f;
		float rad = headRad(), cx = headCx();
		float x = (p.x - cx) / (rad * 0.93f), y = (p.y - cy) / (rad * 0.93f);
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

struct SkinWidget : ModuleWidget {
	void appendContextMenu(Menu* menu) override {
		Skin* m = dynamic_cast<Skin*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);
		// These are the instruments the engine was measured against while it was
		// being built, so they are also the shortest route to hearing whether a
		// change broke something.
		menu->addChild(createIndexPtrSubmenuItem("Head view",
			{"Flat", "3D"}, &m->headView));
		menu->addChild(createSubmenuItem("Instruments", "", [=](Menu* sub) {
			for (int i = 0; i < SKIN_NPRESET; i++) {
				int idx = i;
				sub->addChild(createMenuItem(SKIN_PRESETS[i].name, "",
				                             [=]() { m->loadPreset(idx); }));
			}
		}));
	}

	SkinWidget(Skin* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/skin.svg")));
		using sfs::hp;

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(hp(1), hp(1.6f), "SKIN");

		SkinDisplay* disp = new SkinDisplay();
		disp->module = module;
		// Full width. The head is a circle so its size is set by the height; the
		// width that buys goes to the mode spectrum beside it, which is the one
		// thing about this engine you cannot see from the head alone.
		disp->box.pos  = mm2px(Vec(3.f, 11.f));
		disp->box.size = mm2px(Vec(105.76f, 48.f));
		addChild(disp);

		// Vertical positions are millimetres on a 128.5 mm panel. Only the
		// horizontal grid is in HP.
		const float cx[4] = {13.97f, 41.91f, 69.85f, 97.79f};
		const float KY1 = 71.5f, KY2 = 87.5f, TY = 98.f, JY1 = 110.f, JY2 = 122.f;

		struct K { int p; const char* t; };
		const K big[4] = {{Skin::SIZE_PARAM, "SIZE"}, {Skin::TENSION_PARAM, "TENSION"},
		                  {Skin::STIFF_PARAM, "MATERIAL"}, {Skin::AIR_PARAM, "AIR"}};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(cx[i], KY1)), module, big[i].p));
			lbl->knobLarge(cx[i], KY1, big[i].t);
		}
		const K small[4] = {{Skin::DECAY_PARAM, "DECAY"}, {Skin::TONE_PARAM, "TONE"},
		                    {Skin::EXCITE_PARAM, "EXCITER"}, {Skin::MUFFLE_PARAM, "MUFFLE"}};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx[i], KY2)), module, small[i].p));
			lbl->knob(cx[i], KY2, small[i].t);
		}

		const float tx[7] = {7.98f, 23.95f, 39.91f, 55.88f, 71.84f, 87.81f, 103.78f};
		const K trims[7] = {{Skin::COUPLE_PARAM, "COUPLE"}, {Skin::RESO_PARAM, "RESO"},
		                    {Skin::BEND_PARAM, "BEND"},     {Skin::WEIGHT_PARAM, "WEIGHT"},
		                    {Skin::SNARE_PARAM, "WIRES"},   {Skin::SNARETHR_PARAM, "TIGHT"},
		                    {Skin::LEVEL_PARAM, "LEVEL"}};
		for (int i = 0; i < 7; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(tx[i], TY)), module, trims[i].p));
			lbl->trim(tx[i], TY, trims[i].t);
		}

		struct J { int id; const char* t; };
		const J in1[5] = {{Skin::GATE_INPUT, "GATE"}, {Skin::VEL_INPUT, "VEL"},
		                  {Skin::VOCT_INPUT, "V/OCT"}, {Skin::STRIKEX_INPUT, "X"},
		                  {Skin::STRIKEY_INPUT, "Y"}};
		for (int i = 0; i < 5; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(tx[i], JY1)), module, in1[i].id));
			lbl->jack(tx[i], JY1, in1[i].t);
		}
		addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(
			mm2px(Vec(tx[5], JY1)), module, Skin::STRIKE_PARAM, Skin::STRIKE_LIGHT));
		lbl->jack(tx[5], JY1, "HIT");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(tx[6], JY1)), module, Skin::SNARE_OUTPUT));
		lbl->jack(tx[6], JY1, "WIRES");

		const J in2[5] = {{Skin::SIZE_INPUT, "SIZE"}, {Skin::TENSION_INPUT, "TEN"},
		                  {Skin::STIFF_INPUT, "MAT"}, {Skin::AIR_INPUT, "AIR"},
		                  {Skin::MUFFLE_INPUT, "MUFF"}};
		for (int i = 0; i < 5; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(tx[i], JY2)), module, in2[i].id));
			lbl->jack(tx[i], JY2, in2[i].t);
		}
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(tx[5], JY2)), module, Skin::HEAD_OUTPUT));
		lbl->jack(tx[5], JY2, "HEAD");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(tx[6], JY2)), module, Skin::OUT_OUTPUT));
		lbl->jack(tx[6], JY2, "OUT");
	}
};

Model* modelSkin = createModel<Skin, SkinWidget>("Skin");
