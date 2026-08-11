#include "plugin.hpp"
#include "op_env_engine.h"
#include <osdialog.h>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// OP ENV — standalone DX7 operator envelope. Pick a voice (same banks as
// Operator) to load its carrier EG, offset each of the 8 EG attributes
// (R1-4 / L1-4) via trimpot + CV, fire it with GATE, and get a 0-10V envelope.
// ---------------------------------------------------------------------------

static const char OPENV_SYX_FILTERS[] = "DX7 SysEx (.syx):syx;All files (*.*):*";
static const int  OPENV_MAX_BANKS = 16;

// --- tiny base64 for persisting bank data (matches Operator) ---
static const char* OE_B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string oeB64encode(const uint8_t* d, int n) {
	std::string o;
	for (int i = 0; i < n; i += 3) {
		int b = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
		o += OE_B64[(b >> 18) & 63];
		o += OE_B64[(b >> 12) & 63];
		o += (i + 1 < n) ? OE_B64[(b >> 6) & 63] : '=';
		o += (i + 2 < n) ? OE_B64[b & 63] : '=';
	}
	return o;
}
static std::vector<uint8_t> oeB64decode(const std::string& s) {
	int t[256]; for (int i = 0; i < 256; i++) t[i] = -1;
	for (int i = 0; i < 64; i++) t[(uint8_t) OE_B64[i]] = i;
	std::vector<uint8_t> o; int val = 0, bits = -8;
	for (char c : s) {
		if (t[(uint8_t) c] < 0) continue;
		val = (val << 6) | t[(uint8_t) c]; bits += 6;
		if (bits >= 0) { o.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
	}
	return o;
}

struct OpEnv : Module {
	enum ParamId {
		VOICE_PARAM, BANK_PARAM,
		R1_PARAM, R2_PARAM, R3_PARAM, R4_PARAM,
		L1_PARAM, L2_PARAM, L3_PARAM, L4_PARAM,
		OUTLVL_PARAM,
		LFO_RATE_PARAM, LFO_DELAY_PARAM, LFO_DEPTH_PARAM, AM_SENS_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		GATE_INPUT, VOICE_CV_INPUT, BANK_CV_INPUT,
		R1_CV_INPUT, R2_CV_INPUT, R3_CV_INPUT, R4_CV_INPUT,
		L1_CV_INPUT, L2_CV_INPUT, L3_CV_INPUT, L4_CV_INPUT,
		VOCT_INPUT, OUTLVL_CV_INPUT, LFO_RATE_CV_INPUT, LFO_DEPTH_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId { ENV_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	struct LoadedBank { std::string name; uint8_t data[4096] = {}; int voiceCount = 32; };

	OpEnvEngine engine;
	std::vector<LoadedBank> banks;
	int   curBank = 0;
	int   blockPos = 0;
	float curSR = 0.f;
	bool  gateHigh = false;

	float prevLevel = 0.f, curLevel = 0.f;   // block-to-block interpolation
	int   lfoWave = -1;                      // LFO wave override (-1 = from voice)
	bool  releaseToZero = true;              // gate-off returns to 0V (vs DX7 L4)

	// display mirrors
	int  dispVoice = 0, dispBank = 0;
	char dispName[11] = {0};

	static const int ENV_N = 120, SCOPE_N = 120, SCOPE_DECIM = 256;
	float envCurve[ENV_N] = {};
	bool  envValid = false;
	int   shapeSig = -1;
	float levelAmps[4] = {1.f, 0.5f, 0.78f, 0.f};   // L1..L4 amplitudes (display guides)
	float releaseFrac = 0.7f;                        // key-off x along the curve
	float scope[SCOPE_N] = {};
	int   scopeHead = 0;
	float scopeAcc = 0.f;
	int   scopeCnt = 0;

	OpEnv() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(VOICE_PARAM, 0.f, 31.f, 0.f, "Voice", "", 0.f, 1.f, 1.f);
		paramQuantities[VOICE_PARAM]->snapEnabled = true;
		configParam(BANK_PARAM, 0.f, (float)(OPENV_MAX_BANKS - 1), 0.f, "Bank");
		paramQuantities[BANK_PARAM]->snapEnabled = true;
		const char* rn[4] = {"Rate 1 offset", "Rate 2 offset", "Rate 3 offset", "Rate 4 offset"};
		const char* ln[4] = {"Level 1 offset", "Level 2 offset", "Level 3 offset", "Level 4 offset"};
		for (int i = 0; i < 4; i++) {
			configParam(R1_PARAM + i, -99.f, 99.f, 0.f, rn[i]);
			paramQuantities[R1_PARAM + i]->snapEnabled = true;
			configParam(L1_PARAM + i, -99.f, 99.f, 0.f, ln[i]);
			paramQuantities[L1_PARAM + i]->snapEnabled = true;
		}
		configParam(OUTLVL_PARAM, 0.f, 100.f, 100.f, "Output level", "%");
		configParam(LFO_RATE_PARAM, -99.f, 99.f, 0.f, "LFO rate offset");
		paramQuantities[LFO_RATE_PARAM]->snapEnabled = true;
		configParam(LFO_DELAY_PARAM, -99.f, 99.f, 0.f, "LFO delay offset");
		paramQuantities[LFO_DELAY_PARAM]->snapEnabled = true;
		configParam(LFO_DEPTH_PARAM, -99.f, 99.f, 0.f, "LFO depth offset (AM)");
		paramQuantities[LFO_DEPTH_PARAM]->snapEnabled = true;
		configParam(AM_SENS_PARAM, -3.f, 3.f, 0.f, "AM sensitivity offset");
		paramQuantities[AM_SENS_PARAM]->snapEnabled = true;

		configInput(GATE_INPUT, "Gate");
		configInput(VOICE_CV_INPUT, "Voice select CV (0-10V)");
		configInput(BANK_CV_INPUT, "Bank select CV (1V/bank)");
		for (int i = 0; i < 4; i++) {
			configInput(R1_CV_INPUT + i, string::f("Rate %d CV (±5V)", i + 1));
			configInput(L1_CV_INPUT + i, string::f("Level %d CV (±5V)", i + 1));
		}
		configInput(VOCT_INPUT, "V/oct (rate scaling — faster envelope on higher notes)");
		configInput(OUTLVL_CV_INPUT, "Output level CV (0-10V)");
		configInput(LFO_RATE_CV_INPUT, "LFO rate CV (±5V)");
		configInput(LFO_DEPTH_CV_INPUT, "LFO depth CV (±5V)");
		configOutput(ENV_OUTPUT, "Envelope (0-10V)");

		LoadedBank def;
		def.name = "Eno '87";
		def.voiceCount = 4;
		engine.getBank(def.data);
		banks.push_back(def);
		engine.getVoiceName(dispName);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		curSR = e.sampleRate;
		engine.setSampleRate(curSR);
	}

	int selectedBank() {
		int b = (int) std::round(params[BANK_PARAM].getValue());
		if (inputs[BANK_CV_INPUT].isConnected())
			b += (int) std::round(inputs[BANK_CV_INPUT].getVoltage());
		return clamp(b, 0, (int) banks.size() - 1);
	}
	int currentVoiceCount() const {
		if (curBank >= 0 && curBank < (int) banks.size())
			return clamp(banks[curBank].voiceCount, 1, 32);
		return 32;
	}
	int selectedVoice() {
		int v = (int) std::round(params[VOICE_PARAM].getValue());
		if (inputs[VOICE_CV_INPUT].isConnected())
			v += (int) std::round(inputs[VOICE_CV_INPUT].getVoltage() / 10.f * 31.f);
		return clamp(v, 0, currentVoiceCount() - 1);
	}
	void process(const ProcessArgs& args) override {
		if (args.sampleRate != curSR) { curSR = args.sampleRate; engine.setSampleRate(curSR); }

		// Gate edges every sample (the engine consumes them immediately).
		float g = inputs[GATE_INPUT].getVoltage();
		if (!gateHigh && g >= 1.f)       { gateHigh = true;  engine.gate(true); }
		else if (gateHigh && g <= 0.1f)  { gateHigh = false; engine.gate(false); }

		if (blockPos == 0) {
			int b = selectedBank();
			if (b != curBank && b >= 0 && b < (int) banks.size()) {
				curBank = b;
				engine.setBankRaw(banks[b].data);
			}
			int v = selectedVoice();
			if (v != engine.voice()) engine.setVoice(v);
			engine.getVoiceName(dispName);
			dispVoice = engine.voice();
			dispBank = curBank;

			int rOff[4], lOff[4];
			for (int i = 0; i < 4; i++) {
				rOff[i] = clamp((int) std::round(params[R1_PARAM + i].getValue()
					+ inputs[R1_CV_INPUT + i].getVoltage() / 5.f * 99.f), -99, 99);
				lOff[i] = clamp((int) std::round(params[L1_PARAM + i].getValue()
					+ inputs[L1_CV_INPUT + i].getVoltage() / 5.f * 99.f), -99, 99);
			}
			engine.setOffsets(rOff, lOff);

			// V/oct → rate scaling.
			bool voctOn = inputs[VOCT_INPUT].isConnected();
			int note = voctOn
				? clamp((int) std::round(inputs[VOCT_INPUT].getVoltage() * 12.f + 60.f), 0, 127) : 60;
			engine.setNote(note, voctOn);

			// LFO (tremolo) offsets.
			int lfoRateOff  = clamp((int) std::round(params[LFO_RATE_PARAM].getValue()
				+ inputs[LFO_RATE_CV_INPUT].getVoltage() / 5.f * 99.f), -99, 99);
			int lfoDepthOff = clamp((int) std::round(params[LFO_DEPTH_PARAM].getValue()
				+ inputs[LFO_DEPTH_CV_INPUT].getVoltage() / 5.f * 99.f), -99, 99);
			int lfoDelayOff = clamp((int) std::round(params[LFO_DELAY_PARAM].getValue()), -99, 99);
			int amSensOff   = clamp((int) std::round(params[AM_SENS_PARAM].getValue()), -3, 3);
			engine.setLfo(lfoRateOff, lfoDelayOff, lfoDepthOff, amSensOff, lfoWave);
			engine.setReleaseToZero(releaseToZero);

			// Recompute the static EG shape only when it changed (EG-only, no LFO).
			int sig = engine.voice() * 131 + curBank + (releaseToZero ? 1 : 0) * 977;
			for (int i = 0; i < 4; i++) sig = sig * 199 + rOff[i] * 7 + lOff[i];
			if (sig != shapeSig) {
				engine.renderEnvShape(envCurve, ENV_N);
				engine.getLevelAmps(levelAmps);
				releaseFrac = engine.releaseFraction();
				envValid = true;
				shapeSig = sig;
			}

			prevLevel = curLevel;
			engine.renderBlock();
			curLevel = engine.level();
		}

		float t = (float) blockPos / (float) OpEnvEngine::BLOCK;
		float lv = prevLevel + (curLevel - prevLevel) * t;
		float outAmt = clamp(params[OUTLVL_PARAM].getValue() / 100.f
			+ inputs[OUTLVL_CV_INPUT].getVoltage() / 10.f, 0.f, 2.f);
		outputs[ENV_OUTPUT].setVoltage(clamp(lv * outAmt * 10.f, 0.f, 10.f));

		scopeAcc = std::max(scopeAcc, lv);
		if (++scopeCnt >= SCOPE_DECIM) {
			scope[scopeHead] = scopeAcc;
			scopeHead = (scopeHead + 1) % SCOPE_N;
			scopeAcc = 0.f; scopeCnt = 0;
		}
		blockPos = (blockPos + 1) % OpEnvEngine::BLOCK;
	}

	void addBank(const std::string& path) {
		FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) return;
		std::fseek(f, 0, SEEK_END);
		long sz = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (sz > 0 && sz < 1 << 20) {
			std::vector<uint8_t> raw(sz);
			if (std::fread(raw.data(), 1, sz, f) == (size_t) sz) {
				if (engine.loadBank(raw.data(), (int) sz)) {
					LoadedBank lb;
					lb.name = system::getStem(path);
					engine.getBank(lb.data);
					if ((int) banks.size() >= OPENV_MAX_BANKS) banks.erase(banks.begin());
					banks.push_back(lb);
					curBank = (int) banks.size() - 1;
					params[BANK_PARAM].setValue((float) curBank);
					engine.setBankRaw(banks[curBank].data);
					engine.getVoiceName(dispName);
				}
			}
		}
		std::fclose(f);
	}
	void removeBank(int idx) {
		if (idx < 0 || idx >= (int) banks.size() || banks.size() <= 1) return;
		banks.erase(banks.begin() + idx);
		curBank = clamp(curBank, 0, (int) banks.size() - 1);
		params[BANK_PARAM].setValue((float) curBank);
		engine.setBankRaw(banks[curBank].data);
		engine.getVoiceName(dispName);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		for (auto& b : banks) {
			json_t* o = json_object();
			json_object_set_new(o, "name", json_string(b.name.c_str()));
			json_object_set_new(o, "data", json_string(oeB64encode(b.data, 4096).c_str()));
			json_object_set_new(o, "voiceCount", json_integer(b.voiceCount));
			json_array_append_new(arr, o);
		}
		json_object_set_new(root, "banks", arr);
		json_object_set_new(root, "curBank", json_integer(curBank));
		json_object_set_new(root, "lfoWave", json_integer(lfoWave));
		json_object_set_new(root, "releaseToZero", json_boolean(releaseToZero));
		return root;
	}
	void dataFromJson(json_t* root) override {
		json_t* arr = json_object_get(root, "banks");
		if (arr && json_array_size(arr) > 0) {
			banks.clear();
			size_t n = json_array_size(arr);
			for (size_t i = 0; i < n && banks.size() < OPENV_MAX_BANKS; i++) {
				json_t* o = json_array_get(arr, i);
				LoadedBank lb;
				json_t* nm = json_object_get(o, "name");
				lb.name = nm ? json_string_value(nm) : "Bank";
				json_t* d = json_object_get(o, "data");
				std::vector<uint8_t> raw = d ? oeB64decode(json_string_value(d)) : std::vector<uint8_t>();
				std::memset(lb.data, 0, 4096);
				if (raw.size() >= 4096) std::memcpy(lb.data, raw.data(), 4096);
				json_t* vc = json_object_get(o, "voiceCount");
				lb.voiceCount = vc ? clamp((int) json_integer_value(vc), 1, 32) : 32;
				banks.push_back(lb);
			}
		}
		json_t* cb = json_object_get(root, "curBank");
		curBank = cb ? clamp((int) json_integer_value(cb), 0, (int) banks.size() - 1) : 0;
		if (!banks.empty()) engine.setBankRaw(banks[curBank].data);
		json_t* lw = json_object_get(root, "lfoWave");
		if (lw) lfoWave = clamp((int) json_integer_value(lw), -1, 5);

		json_t* rz = json_object_get(root, "releaseToZero");
		if (rz) releaseToZero = json_boolean_value(rz);
		engine.getVoiceName(dispName);
	}
};

// --- Display: voice header + static EG (blue) and live output (orange) plots ---
static const float OE_W = 186.f, OE_H = 60.f;   // wide/short, matches the screen box
static inline float oePreviewEnv(int i, int n) {
	float t = (n > 1) ? (float) i / (n - 1) : 0.f;
	if (t < 0.07f) return t / 0.07f;
	float d = (t - 0.07f) / 0.93f;
	return 0.05f + 0.95f * std::exp(-3.2f * d);
}

struct OpEnvDisplay : OpaqueWidget {
	OpEnv* module = nullptr;
	std::shared_ptr<Font> font;

	void drawScreen(NVGcontext* vg) {
		float w = box.size.x, h = box.size.y;
		float s = std::min(w / OE_W, h / OE_H);
		float ox = (w - OE_W * s) * 0.5f, oy = (h - OE_H * s) * 0.5f;
		auto X = [&](float v) { return ox + s * v; };
		auto Y = [&](float v) { return oy + s * v; };

		int nb = module ? (int) module->banks.size() : 1;
		int selB = module ? module->dispBank : 0;
		if (font && font->handle >= 0) {
			nvgFontFaceId(vg, font->handle);
			const char* bankName = (module && selB >= 0 && selB < nb) ? module->banks[selB].name.c_str() : "OP ENV";
			const char* vname = module ? module->dispName : "E.PIANO 1";
			int vnum = module ? module->dispVoice : 0;
			nvgFontSize(vg, s * 9.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			char num[8]; std::snprintf(num, sizeof(num), "%d ", vnum + 1);
			nvgFillColor(vg, nvgRGB(0xEC, 0x65, 0x2E));
			float nx = nvgText(vg, X(8.f), Y(3.f), num, NULL);
			nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
			nvgText(vg, nx, Y(3.f), vname, NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgFillColor(vg, nvgRGBA(0xa0, 0xa8, 0xc0, 0xcc));
			nvgText(vg, X(178.f), Y(3.f), bankName, NULL);
		}

		// Single plot region. Background = the dim 4-stage DX7 reference diagram;
		// foreground = the actual envelope (blue) and live output (orange).
		const float px0 = 8.f, px1 = 178.f, pTop = 16.f, pBot = 57.f;
		auto PX = [&](float fx) { return X(px0 + (px1 - px0) * fx); };
		auto PY = [&](float a) { a = clamp(a, 0.f, 1.f); return Y(pBot - (pBot - pTop) * a); };
		nvgLineJoin(vg, NVG_MITER); nvgLineCap(vg, NVG_SQUARE);

		// Data-driven backdrop: guides at this voice's actual L1..L4 amplitudes,
		// key-off marker at its actual release point.
		float aL1 = 1.f, aL2 = 0.5f, aL3 = 0.78f, aL4 = 0.f, relX = 0.7f;
		if (module) {
			aL1 = module->levelAmps[0]; aL2 = module->levelAmps[1];
			aL3 = module->levelAmps[2]; aL4 = module->levelAmps[3];
			relX = module->releaseFrac;
		}
		NVGcolor dimLine = nvgRGB(0x2b, 0x2b, 0x42);
		NVGcolor dimText = nvgRGBA(0x7a, 0x82, 0xa0, 0xbb);

		nvgStrokeColor(vg, dimLine); nvgStrokeWidth(vg, std::max(0.5f, s * 0.7f));
		for (float gl : {aL1, aL2, aL3, aL4}) {
			nvgBeginPath(vg); nvgMoveTo(vg, PX(0), PY(gl)); nvgLineTo(vg, PX(1), PY(gl)); nvgStroke(vg);
		}
		for (float xv : {0.f, relX}) {
			nvgBeginPath(vg); nvgMoveTo(vg, PX(xv), PY(1)); nvgLineTo(vg, PX(xv), PY(0)); nvgStroke(vg);
		}

		if (font && font->handle >= 0) {
			nvgFontSize(vg, s * 7.f);
			nvgFillColor(vg, dimText);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			// Label L1..L4, skipping ones that sit on top of each other.
			struct Lbl { const char* t; float a; };
			Lbl ls[4] = { {"L1", aL1}, {"L2", aL2}, {"L3", aL3}, {"L4", aL4} };
			for (int i = 0; i < 4; i++) {
				bool clash = false;
				for (int j = 0; j < i; j++) if (std::fabs(ls[i].a - ls[j].a) < 0.06f) clash = true;
				if (!clash) nvgText(vg, PX(0) - 1.5f, PY(ls[i].a), ls[i].t, NULL);
			}
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
			nvgText(vg, PX(0), PY(1) + 1, "on", NULL);
			nvgText(vg, PX(relX), PY(1) + 1, "off", NULL);
		}

		// actual envelope (blue) over the diagram
		const int N = OpEnv::ENV_N;
		bool haveEnv = module && module->envValid;
		nvgBeginPath(vg);
		for (int i = 0; i < N; i++) {
			float v = clamp(haveEnv ? module->envCurve[i] : oePreviewEnv(i, N), 0.f, 1.f);
			float x = PX((float) i / (N - 1));
			if (i == 0) nvgMoveTo(vg, x, PY(v)); else nvgLineTo(vg, x, PY(v));
		}
		nvgStrokeColor(vg, nvgRGB(0x00, 0x97, 0xDE));
		nvgStrokeWidth(vg, std::max(0.9f, s * 1.1f)); nvgStroke(vg);

		// live output (orange)
		if (module) {
			const int M = OpEnv::SCOPE_N;
			nvgBeginPath(vg);
			for (int j = 0; j < M; j++) {
				int idx = (module->scopeHead + j) % M;
				float v = clamp(module->scope[idx], 0.f, 1.f);
				float x = PX((float) j / (M - 1));
				if (j == 0) nvgMoveTo(vg, x, PY(v)); else nvgLineTo(vg, x, PY(v));
			}
			nvgStrokeColor(vg, nvgRGB(0xEC, 0x65, 0x2E));
			nvgStrokeWidth(vg, std::max(0.8f, s * 1.2f)); nvgStroke(vg);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 4.f);
		nvgFillColor(vg, nvgRGB(0x1A, 0x1A, 0x2E)); nvgFill(vg);
		nvgStrokeColor(vg, nvgRGB(0x40, 0x40, 0x60)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		nvgIntersectScissor(vg, 1, 1, w - 2, h - 2);
		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		drawScreen(vg);
		OpaqueWidget::drawLayer(args, layer);
	}
};

struct OpEnvWidget : ModuleWidget {
	OpEnvWidget(OpEnv* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/op_env.svg")));

		// No virtual screws — see CLAUDE.md (SFS panels omit them by design).

		OpEnvDisplay* disp = new OpEnvDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(4.233f, 12.33f));   // matches res/op_env.svg screen rect
		disp->box.size = mm2px(Vec(93.f, 30.f));
		addChild(disp);

		// Layout synced to res/op_env.svg reticules: trimpots (blue, top row of
		// each block) ABOVE the CV jacks (red, bottom row). LEVELS block up top,
		// RATES below, with VOICE/BANK as the 5th column.
		const float colN[4] = {25.4f, 40.64f, 55.88f, 71.12f};   // L1-4 / R1-4
		const float col5 = 91.44f;                                // VOICE / BANK

		// LEVELS: trimpot @ 55.88, CV @ 66.04.
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(colN[i], 55.88f)), module, OpEnv::L1_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colN[i], 66.04f)), module, OpEnv::L1_CV_INPUT + i));
		}
		addParam(createParamCentered<Trimpot>(mm2px(Vec(col5, 55.88f)), module, OpEnv::VOICE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(col5, 66.04f)), module, OpEnv::VOICE_CV_INPUT));

		// RATES: trimpot @ 81.28, CV @ 91.44. BANK is mirrored (jack above, pot
		// below) so the VOICE/BANK jacks sit adjacent in the middle.
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(colN[i], 81.28f)), module, OpEnv::R1_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colN[i], 91.44f)), module, OpEnv::R1_CV_INPUT + i));
		}
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(col5, 81.28f)), module, OpEnv::BANK_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(col5, 91.44f)), module, OpEnv::BANK_PARAM));

		// Bottom: trimpots on the upper row (106.68), jacks on the lower (121.92).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 106.68f)), module, OpEnv::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 121.92f)), module, OpEnv::GATE_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(25.4f, 106.68f)), module, OpEnv::LFO_RATE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4f, 121.92f)), module, OpEnv::LFO_RATE_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(40.64f, 106.68f)), module, OpEnv::LFO_DEPTH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.64f, 121.92f)), module, OpEnv::LFO_DEPTH_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(55.88f, 106.68f)), module, OpEnv::AM_SENS_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(55.88f, 121.92f)), module, OpEnv::LFO_DELAY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(71.12f, 106.68f)), module, OpEnv::OUTLVL_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(71.12f, 121.92f)), module, OpEnv::OUTLVL_CV_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(91.44f, 121.92f)), module, OpEnv::ENV_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		OpEnv* m = dynamic_cast<OpEnv*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Release to 0V", "", &m->releaseToZero));
		menu->addChild(createMenuLabel("(off = authentic DX7 L4 release level)"));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Banks"));
		menu->addChild(createMenuItem("Load bank (.syx)…", "", [m]() {
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL,
				osdialog_filters_parse(OPENV_SYX_FILTERS));
			if (path) { m->addBank(path); std::free(path); }
		}));
		for (int i = 0; i < (int) m->banks.size(); i++) {
			std::string label = string::f("%d: %s", i + 1, m->banks[i].name.c_str());
			menu->addChild(createCheckMenuItem(label, "",
				[m, i]() { return m->curBank == i; },
				[m, i]() { m->params[OpEnv::BANK_PARAM].setValue((float) i); }));
		}
		if (m->banks.size() > 1) {
			menu->addChild(createMenuItem("Remove current bank", "",
				[m]() { m->removeBank(m->curBank); }));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("LFO waveform"));
		struct WaveOpt { const char* name; int v; };
		static const WaveOpt waves[] = {
			{"From voice", -1}, {"Triangle", 0}, {"Saw down", 1}, {"Saw up", 2},
			{"Square", 3}, {"Sine", 4}, {"Sample & hold", 5},
		};
		for (const WaveOpt& wv : waves) {
			int val = wv.v;
			menu->addChild(createCheckMenuItem(wv.name, "",
				[m, val]() { return m->lfoWave == val; },
				[m, val]() { m->lfoWave = val; }));
		}
	}
};

Model* modelOpEnv = createModel<OpEnv, OpEnvWidget>("OpEnv");
