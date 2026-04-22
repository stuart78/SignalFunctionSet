#include "plugin.hpp"
#include <cmath>


// Maximum number of overlapping rise contributions tracked at once.
// Each PING queues one rise; multiple in-flight rises stack additively.
static const int MAX_RISES = 32;

// Scope ring buffer: SCOPE_HALF past samples drawn left of center, SCOPE_HALF
// future samples simulated forward and drawn right of center. SCOPE_DT is the
// time between scope samples, so total visible window = 2 * SCOPE_HALF * SCOPE_DT.
static const int   SCOPE_HALF = 60;          // 60 past + 60 future
static const float SCOPE_DT   = 0.01f;       // 10ms per scope sample → 1.2s window


struct Swell;


struct SwellDisplay : Widget {
	Swell* module = nullptr;
	std::shared_ptr<Font> font;

	void drawLayer(const DrawArgs& args, int layer) override;
	void draw(const DrawArgs& args) override { Widget::draw(args); }
};


struct Swell : Module {
	enum ParamId {
		DELTA_PARAM,
		RISE_PARAM,
		DECAY_PARAM,
		CURVE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		PING_INPUT,
		RESET_INPUT,
		DELTA_CV_INPUT,
		RISE_CV_INPUT,
		DECAY_CV_INPUT,
		CURVE_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		CV_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	struct Rise {
		float deltaTotal = 0.f;   // total V we want this rise to contribute
		float deltaAdded = 0.f;   // V actually added so far
		float riseTime  = 0.05f;  // total rise time in seconds
		bool  active    = false;
	};
	Rise rises[MAX_RISES];
	int  riseCursor = 0;

	float V = 0.f;             // current envelope voltage 0..10

	dsp::SchmittTrigger pingTrigger;
	dsp::SchmittTrigger resetTrigger;

	// Scope state — past samples + cached params so the display can simulate
	// the future trace without touching live audio state.
	float scopePast[SCOPE_HALF] = {};
	int   scopeWriteIdx = 0;        // next slot to write
	float scopeAccum    = 0.f;      // seconds since last scope sample
	// Snapshot of params for display-side future simulation (atomic-ish: written
	// once per process call, read by GUI thread).
	float dispCurve     = 0.5f;
	float dispRiseTime  = 0.05f;
	float dispDecayTime = 1.f;

	// --- Custom param quantities so tooltips show meaningful values ---
	struct DeltaQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			return string::f("%.2f V", getValue() * 10.f);
		}
	};
	struct RiseQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float t = 0.001f * std::pow(2000.f, getValue()); // 1ms..2s
			if (t < 0.1f) return string::f("%.0f ms", t * 1000.f);
			return string::f("%.2f s", t);
		}
	};
	struct DecayQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float t = 0.01f * std::pow(1000.f, getValue()); // 10ms..10s
			if (t < 0.1f) return string::f("%.0f ms", t * 1000.f);
			return string::f("%.2f s", t);
		}
	};
	struct CurveQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float c = getValue();
			if (c < 0.05f) return "Linear";
			if (c > 0.95f) return "Exponential";
			return string::f("%.0f%% exp", c * 100.f);
		}
	};

	Swell() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<DeltaQuantity>(DELTA_PARAM, 0.f, 1.f, 0.5f,
			"Delta voltage per ping");
		configParam<RiseQuantity>(RISE_PARAM, 0.f, 1.f, 0.10f,
			"Rise time");
		configParam<DecayQuantity>(DECAY_PARAM, 0.f, 1.f, 0.50f,
			"Fall time");
		configParam<CurveQuantity>(CURVE_PARAM, 0.f, 1.f, 0.5f,
			"Curve shape (linear → exponential)");
		configInput(PING_INPUT,    "Ping (rising edge adds Delta)");
		configInput(RESET_INPUT,   "Reset (zeros envelope)");
		configInput(DELTA_CV_INPUT,"Delta CV (±5V → ±50%)");
		configInput(RISE_CV_INPUT, "Rise CV (±5V → ±50%)");
		configInput(DECAY_CV_INPUT,"Fall CV (±5V → ±50%)");
		configInput(CURVE_CV_INPUT,"Curve CV (±5V → ±50%)");
		configOutput(CV_OUTPUT,    "Envelope CV (0-10V, soft-saturated)");
	}

	void onReset() override {
		V = 0.f;
		for (int i = 0; i < MAX_RISES; i++) rises[i].active = false;
		riseCursor = 0;
	}

	// Helper: read a 0..1 param and clamp(param + cv*0.1, 0, 1).
	static float readParamCV(Param& p, Input& cv) {
		float v = p.getValue();
		if (cv.isConnected()) v += cv.getVoltage() * 0.1f;
		return clamp(v, 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		// --- Read params + CV ---
		float deltaN = readParamCV(params[DELTA_PARAM], inputs[DELTA_CV_INPUT]);
		float riseN  = readParamCV(params[RISE_PARAM],  inputs[RISE_CV_INPUT]);
		float decN   = readParamCV(params[DECAY_PARAM], inputs[DECAY_CV_INPUT]);
		float curve  = readParamCV(params[CURVE_PARAM], inputs[CURVE_CV_INPUT]);

		float delta     = deltaN * 10.f;
		float riseTime  = std::max(0.001f * std::pow(2000.f, riseN), 0.001f);
		float decayTime = std::max(0.01f  * std::pow(1000.f, decN),  0.001f);

		dispCurve     = curve;
		dispRiseTime  = riseTime;
		dispDecayTime = decayTime;

		// --- Reset ---
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			V = 0.f;
			for (int i = 0; i < MAX_RISES; i++) rises[i].active = false;
		}

		// --- Ping: queue a new rise contribution ---
		if (pingTrigger.process(inputs[PING_INPUT].getVoltage(), 0.1f, 1.f)) {
			// Find a free slot, prefer the next-available cursor position.
			int idx = -1;
			for (int i = 0; i < MAX_RISES; i++) {
				int t = (riseCursor + i) % MAX_RISES;
				if (!rises[t].active) { idx = t; break; }
			}
			if (idx < 0) idx = riseCursor;   // all slots busy — overwrite cursor
			rises[idx].deltaTotal = delta;
			rises[idx].deltaAdded = 0.f;
			rises[idx].riseTime   = riseTime;
			rises[idx].active     = true;
			riseCursor = (idx + 1) % MAX_RISES;
		}

		// --- Apply active rise contributions ---
		// Each rise spreads its `deltaTotal` over `riseTime`. The `curve` param
		// blends between a constant rate (linear) and a remaining*time-constant
		// rate (exponential settle, which adds faster early, slower late).
		// Soft-saturation scales each addition by (10-V)/10 so V asymptotes
		// smoothly at the 10V ceiling.
		bool anyRiseActive = false;
		for (int i = 0; i < MAX_RISES; i++) {
			if (!rises[i].active) continue;
			anyRiseActive = true;
			float remaining = rises[i].deltaTotal - rises[i].deltaAdded;
			if (remaining <= 0.f) { rises[i].active = false; continue; }
			float linRate = rises[i].deltaTotal / rises[i].riseTime;
			float expRate = remaining * 3.f / rises[i].riseTime;  // ~95% in riseTime
			float rate    = (1.f - curve) * linRate + curve * expRate;
			float dV      = rate * args.sampleTime;
			if (dV > remaining) dV = remaining;
			rises[i].deltaAdded += dV;
			float headroom = std::max(0.f, 10.f - V);
			V += dV * (headroom / 10.f);
			if (rises[i].deltaAdded >= rises[i].deltaTotal - 0.0001f
				|| rises[i].deltaAdded >= 10.f) {
				rises[i].active = false;
			}
		}

		// --- Continuous decay back toward 0 ---
		// Paused while any rise is in flight so the rise actually reaches its
		// delta (otherwise decay eats voltage as fast as the rise adds it).
		if (!anyRiseActive && V > 0.f) {
			float linRate = 10.f / decayTime;       // V drops by full scale in decayTime
			float expRate = V    / decayTime;       // tau-based exponential bleed
			float rate    = (1.f - curve) * linRate + curve * expRate;
			V = std::max(0.f, V - rate * args.sampleTime);
		}

		// Hard ceiling just in case (soft-sat should keep us under)
		if (V > 10.f) V = 10.f;

		outputs[CV_OUTPUT].setVoltage(V);

		// --- Scope sampling ---
		scopeAccum += args.sampleTime;
		if (scopeAccum >= SCOPE_DT) {
			scopeAccum -= SCOPE_DT;
			scopePast[scopeWriteIdx] = V;
			scopeWriteIdx = (scopeWriteIdx + 1) % SCOPE_HALF;
		}
	}
};


// --- SwellDisplay: scope view — past trace + future projection ---

void SwellDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1 || !module) {
		Widget::drawLayer(args, layer);
		return;
	}

	float w = box.size.x;
	float h = box.size.y;

	if (!font || font->handle < 0) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}

	// Palette (matches Beat / Note / Meter)
	const NVGcolor COL_BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
	const NVGcolor COL_BLUE_FAINT  = nvgRGBA(0x00, 0x97, 0xDE, 0x60);
	const NVGcolor COL_PURPLE      = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
	const NVGcolor COL_PURPLE_DIM  = nvgRGBA(0x35, 0x35, 0x4D, 0x80);
	const NVGcolor COL_ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
	const NVGcolor COL_TEXT_DIM    = nvgRGBA(0xC0, 0xC0, 0xD0, 0xC0);

	const int N = SCOPE_HALF;          // samples each side
	const int TOTAL = 2 * N;
	float pad = 2.f;
	float plotL = pad;
	float plotR = w - pad;
	float plotT = pad;
	float plotB = h - pad;
	float plotW = plotR - plotL;
	float plotH = plotB - plotT;

	auto xAt = [&](int i) {           // i in [0, TOTAL]
		return plotL + plotW * ((float)i / (float)TOTAL);
	};
	auto yAt = [&](float v) {
		float frac = clamp(v / 10.f, 0.f, 1.f);
		return plotB - frac * plotH;
	};

	// --- Gridlines: 0V, 5V, 10V ---
	for (int t = 0; t <= 10; t += 5) {
		float gy = yAt((float)t);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, plotL, gy);
		nvgLineTo(args.vg, plotR, gy);
		nvgStrokeColor(args.vg, COL_PURPLE_DIM);
		nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);
	}

	// --- Past trace (solid blue) ---
	// scopePast is a ring buffer; the OLDEST sample is at scopeWriteIdx,
	// the NEWEST at (scopeWriteIdx - 1) mod N. Draw oldest → newest left → center.
	nvgBeginPath(args.vg);
	for (int i = 0; i < N; i++) {
		int idx = (module->scopeWriteIdx + i) % N;
		float v = module->scopePast[idx];
		float x = xAt(i);
		float y = yAt(v);
		if (i == 0) nvgMoveTo(args.vg, x, y);
		else nvgLineTo(args.vg, x, y);
	}
	// Connect through center to current V
	nvgLineTo(args.vg, xAt(N), yAt(module->V));
	nvgStrokeColor(args.vg, COL_BLUE);
	nvgStrokeWidth(args.vg, 1.2f);
	nvgStroke(args.vg);

	// --- Future projection (semi-transparent) ---
	// Simulate forward from the current state. Copy rises so we don't touch
	// audio-thread state. Step in SCOPE_DT chunks.
	{
		Swell::Rise simRises[MAX_RISES];
		for (int i = 0; i < MAX_RISES; i++) simRises[i] = module->rises[i];
		float simV     = module->V;
		float curve    = module->dispCurve;
		float decTime  = module->dispDecayTime;
		float dt       = SCOPE_DT;

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, xAt(N), yAt(simV));
		for (int s = 1; s <= N; s++) {
			// Apply rise contributions over dt
			bool anyRise = false;
			for (int i = 0; i < MAX_RISES; i++) {
				if (!simRises[i].active) continue;
				anyRise = true;
				float remaining = simRises[i].deltaTotal - simRises[i].deltaAdded;
				if (remaining <= 0.f) { simRises[i].active = false; continue; }
				float linRate = simRises[i].deltaTotal / simRises[i].riseTime;
				float expRate = remaining * 3.f / simRises[i].riseTime;
				float rate    = (1.f - curve) * linRate + curve * expRate;
				float dV      = rate * dt;
				if (dV > remaining) dV = remaining;
				simRises[i].deltaAdded += dV;
				float headroom = std::max(0.f, 10.f - simV);
				simV += dV * (headroom / 10.f);
				if (simRises[i].deltaAdded >= simRises[i].deltaTotal - 0.0001f
					|| simRises[i].deltaAdded >= 10.f) {
					simRises[i].active = false;
				}
			}
			if (!anyRise && simV > 0.f) {
				float linRate = 10.f / decTime;
				float expRate = simV  / decTime;
				float rate    = (1.f - curve) * linRate + curve * expRate;
				simV = std::max(0.f, simV - rate * dt);
			}
			if (simV > 10.f) simV = 10.f;
			nvgLineTo(args.vg, xAt(N + s), yAt(simV));
		}
		nvgStrokeColor(args.vg, COL_BLUE_FAINT);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
	}

	// --- Center NOW marker ---
	float cx = xAt(N);
	nvgBeginPath(args.vg);
	nvgMoveTo(args.vg, cx, plotT);
	nvgLineTo(args.vg, cx, plotB);
	nvgStrokeColor(args.vg, COL_PURPLE);
	nvgStrokeWidth(args.vg, 0.6f);
	nvgStroke(args.vg);

	// Current voltage dot at center
	nvgBeginPath(args.vg);
	nvgCircle(args.vg, cx, yAt(module->V), 1.6f);
	nvgFillColor(args.vg, COL_ORANGE);
	nvgFill(args.vg);

	// --- Numeric voltage readout (top-right corner) ---
	if (font && font->handle >= 0) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 9.f);
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
		nvgFillColor(args.vg, COL_TEXT_DIM);
		std::string lbl = string::f("%.2fV", clamp(module->V, 0.f, 10.f));
		nvgText(args.vg, plotR - 2.f, plotT + 1.f, lbl.c_str(), NULL);
	}

	Widget::drawLayer(args, layer);
}


// --- Widget ---

struct SwellWidget : ModuleWidget {
	SwellWidget(Swell* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/swell.svg")));

		// Display (top of panel)
		SwellDisplay* display = new SwellDisplay();
		display->module = module;
		display->box.pos  = mm2px(Vec(2.4f, 12.f));
		display->box.size = mm2px(Vec(25.7f, 18.f));
		addChild(display);

		// Knob + CV pairs at y = 45.72, 60.96, 76.2, 91.44 (xL=7.62, xR=22.86)
		const float xL = 7.62f;
		const float xR = 22.86f;

		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(xL, 45.72f)), module, Swell::DELTA_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(xR, 45.72f)), module, Swell::DELTA_CV_INPUT));

		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(xL, 60.96f)), module, Swell::RISE_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(xR, 60.96f)), module, Swell::RISE_CV_INPUT));

		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(xL, 76.2f)), module, Swell::DECAY_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(xR, 76.2f)), module, Swell::DECAY_CV_INPUT));

		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(xL, 91.44f)), module, Swell::CURVE_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(xR, 91.44f)), module, Swell::CURVE_CV_INPUT));

		// RESET (right column, alone)
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(xR, 106.68f)), module, Swell::RESET_INPUT));

		// Bottom row: PING (left) + OUT (right, on dark plate)
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(xL, 121.92f)), module, Swell::PING_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(xR, 121.92f)), module, Swell::CV_OUTPUT));
	}
};


Model* modelSwell = createModel<Swell, SwellWidget>("Swell");
