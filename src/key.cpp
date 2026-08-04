#include "plugin.hpp"
#include "panel-style.hpp"
#include "scales.hpp"
#include <cmath>
#include <string>
#include <vector>

// =============================================================================
// Key — a quantizer that takes its key from the patch.
//
// An ordinary quantizer is an island: you set its scale by hand and then keep
// it in step with everything else yourself. This one reads ROOT and SCALE in
// the plugin's own convention (1V/oct semitone-quantized, 1V per scale), so
// Arrange, Note, Chance, Muse, Fugue and Loom can all be moved through a key
// change from one place and this follows.
//
// Four channels, each polyphonic, all sharing one key — so a whole voice group
// stays in agreement rather than four quantizers drifting apart.
//
// NOTE ON THE SCALE TABLE: four of the canonical scales (Harmonic series,
// Pelog, Slendro) have FRACTIONAL semitone intervals — they are genuinely not
// 12-TET. A quantizer built on the usual 12-bit pitch mask would silently round
// them to the chromatic grid and throw away the thing that makes them
// themselves. So the quantizer works against the real float intervals, and only
// the on-screen keyboard rounds (it has just twelve keys to draw on).
// =============================================================================

static const int KEY_NCH = 4;             // channels
static const int KEY_MAXPOLY = 16;

static const char* KEY_NOTES[12] =
	{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Which of the twelve semitones each white / black key is.
static const int KEY_WHITE[7] = {0, 2, 4, 5, 7, 9, 11};
static const int KEY_BLACK[5] = {1, 3, 6, 8, 10};
// Black key n sits after white key KEY_BLACK_AFTER[n].
static const int KEY_BLACK_AFTER[5] = {0, 1, 3, 4, 5};

enum KeyRound { KR_NEAREST, KR_DOWN, KR_UP, KR_COUNT };
static const char* KR_NAME[KR_COUNT] = {"Nearest", "Down", "Up"};

struct Key : Module {
	enum ParamId {
		ROOT_PARAM, SCALE_PARAM,
		ENUMS(OFFSET_PARAM, KEY_NCH),
		PARAMS_LEN
	};
	enum InputId {
		ROOT_INPUT, SCALE_INPUT, TRIG_INPUT,
		ENUMS(IN_INPUT, KEY_NCH),
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(OUT_OUTPUT, KEY_NCH),
		ENUMS(CHG_OUTPUT, KEY_NCH),
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	// ── the key in force this sample ──────────────────────────────────────────
	int   rootNote = 0;                 // 0..11
	int   scaleIndex = 1;               // index into sfs::SCALES
	bool  customMask = false;           // the keyboard has been edited by hand
	uint16_t mask = 0;                  // 12-bit, only meaningful when customMask

	// The intervals actually quantized against, ascending within an octave.
	float ivals[16] = {};
	int   nIvals = 0;
	int   keyGen = 0;                   // bumped whenever the key changes

	// ── options ───────────────────────────────────────────────────────────────
	bool  offsetInDegrees = true;       // else semitones
	int   roundMode = KR_NEAREST;
	float hysteresisCents = 12.f;

	// ── per-channel state ─────────────────────────────────────────────────────
	float held[KEY_NCH][KEY_MAXPOLY] = {};
	bool  hasHeld[KEY_NCH][KEY_MAXPOLY] = {};
	int   lastGen[KEY_NCH][KEY_MAXPOLY] = {};
	dsp::PulseGenerator chgPulse[KEY_NCH][KEY_MAXPOLY];
	dsp::SchmittTrigger trigIn;

	// what the display shows as sounding
	float shownVolts[KEY_NCH] = {};
	bool  shownActive[KEY_NCH] = {};

	Key() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		std::vector<std::string> noteNames;
		for (int i = 0; i < 12; i++) noteNames.push_back(KEY_NOTES[i]);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", noteNames);

		std::vector<std::string> scaleNames;
		for (int i = 0; i < sfs::NUM_SCALES; i++)
			scaleNames.push_back(sfs::SCALES[i].longName);
		configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 1.f, "Scale", scaleNames);

		for (int c = 0; c < KEY_NCH; c++) {
			configParam(OFFSET_PARAM + c, -12.f, 12.f, 0.f,
			            string::f("Channel %d offset", c + 1));
			getParamQuantity(OFFSET_PARAM + c)->snapEnabled = true;
			configInput(IN_INPUT + c, string::f("Channel %d pitch (1V/oct, poly)", c + 1));
			configOutput(OUT_OUTPUT + c, string::f("Channel %d quantized pitch", c + 1));
			configOutput(CHG_OUTPUT + c, string::f("Channel %d note-change trigger", c + 1));
			configBypass(IN_INPUT + c, OUT_OUTPUT + c);
		}

		configInput(ROOT_INPUT,  "Root CV (1V/oct, semitone-quantized)");
		configInput(SCALE_INPUT, "Scale CV (1V per scale)");
		configInput(TRIG_INPUT,  "Sample & hold trigger — when patched, notes update only on a trigger");

		rebuildIntervals();
	}

	void onReset() override {
		customMask = false;
		offsetInDegrees = true;
		roundMode = KR_NEAREST;
		hysteresisCents = 12.f;
		for (int c = 0; c < KEY_NCH; c++)
			for (int p = 0; p < KEY_MAXPOLY; p++) hasHeld[c][p] = false;
		rebuildIntervals();
	}

	// The 12-key mask the on-screen keyboard shows. Microtonal scales are rounded
	// HERE and nowhere else — see the note at the top of the file.
	uint16_t presetMask(int si) const {
		const sfs::Scale& sc = sfs::SCALES[clamp(si, 0, sfs::NUM_SCALES - 1)];
		uint16_t m = 0;
		for (int d = 0; d < sc.size; d++) {
			int s = ((int)std::lround(sc.intervals[d]) % 12 + 12) % 12;
			m |= (uint16_t)(1u << s);
		}
		return m;
	}

	void rebuildIntervals() {
		nIvals = 0;
		if (customMask) {
			for (int s = 0; s < 12 && nIvals < 16; s++)
				if (mask & (1u << s)) ivals[nIvals++] = (float)s;
		}
		else {
			const sfs::Scale& sc = sfs::SCALES[clamp(scaleIndex, 0, sfs::NUM_SCALES - 1)];
			for (int d = 0; d < sc.size && nIvals < 16; d++) {
				float v = std::fmod(sc.intervals[d], 12.f);
				if (v < 0.f) v += 12.f;
				ivals[nIvals++] = v;
			}
			// The harmonic series spans several octaves, so folding it can leave
			// duplicates; sort and de-duplicate rather than quantize to a list
			// with two identical entries.
			for (int a = 1; a < nIvals; a++) {
				float t = ivals[a];
				int b = a - 1;
				while (b >= 0 && ivals[b] > t) { ivals[b + 1] = ivals[b]; b--; }
				ivals[b + 1] = t;
			}
			int w = 0;
			for (int a = 0; a < nIvals; a++)
				if (a == 0 || std::fabs(ivals[a] - ivals[w - 1]) > 0.01f) ivals[w++] = ivals[a];
			nIvals = w;
		}
		if (nIvals == 0) { ivals[0] = 0.f; nIvals = 1; }  // an empty mask would mute
		keyGen++;
	}

	// Snap `semis` (relative to the root, any octave) onto the scale.
	float snap(float semis) const {
		float oct = std::floor(semis / 12.f);
		float within = semis - oct * 12.f;
		float best = ivals[0];
		float bestD = 1e9f;
		// The wrap candidate is the first degree of the NEXT octave — without it
		// anything above the last degree would fall back to the bottom of this one.
		for (int k = 0; k <= nIvals; k++) {
			float c = (k < nIvals) ? ivals[k] : ivals[0] + 12.f;
			float d = within - c;
			if (roundMode == KR_DOWN && d < -1e-4f) continue;
			if (roundMode == KR_UP   && d >  1e-4f) continue;
			float ad = std::fabs(d);
			if (ad < bestD) { bestD = ad; best = c; }
		}
		if (bestD > 1e8f) {                       // directional rounding ran off the end
			best = (roundMode == KR_DOWN) ? ivals[nIvals - 1] - 12.f : ivals[0] + 12.f;
		}
		return oct * 12.f + best;
	}

	// Move `semis` by n scale degrees, keeping it on the scale.
	float shiftDegrees(float semis, int n) const {
		if (n == 0) return semis;
		float oct = std::floor(semis / 12.f);
		float within = semis - oct * 12.f;
		int idx = 0;
		float bd = 1e9f;
		for (int k = 0; k < nIvals; k++) {
			float d = std::fabs(within - ivals[k]);
			if (d < bd) { bd = d; idx = k; }
		}
		int t = idx + n;
		int octShift = (int)std::floor((float)t / (float)nIvals);
		int wrapped = t - octShift * nIvals;
		return (oct + (float)octShift) * 12.f + ivals[wrapped];
	}

	void process(const ProcessArgs& args) override {
		// ── the key ───────────────────────────────────────────────────────────
		int rootK = (int)std::round(params[ROOT_PARAM].getValue());
		int rootCV = inputs[ROOT_INPUT].isConnected()
			? (int)std::round(inputs[ROOT_INPUT].getVoltage() * 12.f) : 0;
		int newRoot = (((rootK + rootCV) % 12) + 12) % 12;

		int scaleK = (int)std::round(params[SCALE_PARAM].getValue());
		int scaleCV = inputs[SCALE_INPUT].isConnected()
			? (int)std::round(inputs[SCALE_INPUT].getVoltage()) : 0;
		int newScale = clamp(scaleK + scaleCV, 0, sfs::NUM_SCALES - 1);

		if (newRoot != rootNote) { rootNote = newRoot; keyGen++; }
		if (newScale != scaleIndex) {
			scaleIndex = newScale;
			// Selecting a scale is an explicit instruction, so it wins over a
			// hand-edited keyboard rather than being silently ignored.
			customMask = false;
			rebuildIntervals();
		}

		bool sampleNow = true;
		if (inputs[TRIG_INPUT].isConnected())
			sampleNow = trigIn.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);

		float hystSemis = hysteresisCents / 100.f;

		for (int c = 0; c < KEY_NCH; c++) {
			int nch = std::max(1, inputs[IN_INPUT + c].getChannels());
			nch = std::min(nch, KEY_MAXPOLY);
			outputs[OUT_OUTPUT + c].setChannels(nch);
			outputs[CHG_OUTPUT + c].setChannels(nch);
			shownActive[c] = inputs[IN_INPUT + c].isConnected();

			int off = (int)std::round(params[OFFSET_PARAM + c].getValue());

			for (int p = 0; p < nch; p++) {
				float in = inputs[IN_INPUT + c].getVoltage(p);
				float outV;

				if (sampleNow || !hasHeld[c][p] || lastGen[c][p] != keyGen) {
					float semis = in * 12.f - (float)rootNote;
					if (!offsetInDegrees) semis += (float)off;
					float q = snap(semis);
					if (offsetInDegrees) q = shiftDegrees(q, off);

					// Hysteresis stops a pitch sitting exactly on a boundary from
					// chattering between two notes. It has to be abandoned when
					// the key changes, or the old note would hold on even though
					// it is no longer in the scale.
					if (hasHeld[c][p] && lastGen[c][p] == keyGen && hystSemis > 0.f) {
						float prev = held[c][p] * 12.f - (float)rootNote;
						float inS = in * 12.f - (float)rootNote;
						if (!offsetInDegrees) inS += (float)off;
						if (std::fabs(inS - prev) < std::fabs(inS - q) + hystSemis)
							q = prev;
					}
					outV = ((float)rootNote + q) / 12.f;

					if (!hasHeld[c][p] || std::fabs(outV - held[c][p]) > 1e-6f) {
						if (hasHeld[c][p]) chgPulse[c][p].trigger(1e-3f);
						held[c][p] = outV;
					}
					hasHeld[c][p] = true;
					lastGen[c][p] = keyGen;
				}
				outV = held[c][p];

				outputs[OUT_OUTPUT + c].setVoltage(outV, p);
				outputs[CHG_OUTPUT + c].setVoltage(
					chgPulse[c][p].process(args.sampleTime) ? 10.f : 0.f, p);
				if (p == 0) shownVolts[c] = outV;
			}
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "customMask", json_boolean(customMask));
		json_object_set_new(root, "mask", json_integer(mask));
		json_object_set_new(root, "offsetInDegrees", json_boolean(offsetInDegrees));
		json_object_set_new(root, "roundMode", json_integer(roundMode));
		json_object_set_new(root, "hysteresisCents", json_real(hysteresisCents));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "customMask")) customMask = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "mask")) mask = (uint16_t)json_integer_value(j);
		if (json_t* j = json_object_get(root, "offsetInDegrees"))
			offsetInDegrees = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "roundMode"))
			roundMode = clamp((int)json_integer_value(j), 0, KR_COUNT - 1);
		if (json_t* j = json_object_get(root, "hysteresisCents"))
			hysteresisCents = clamp((float)json_number_value(j), 0.f, 50.f);
		rebuildIntervals();
	}
};


// =============================================================================
// Display — the key, as a keyboard.
// =============================================================================

struct KeyDisplay : OpaqueWidget {
	Key* module = nullptr;
	std::shared_ptr<Font> font;

	struct Lay {
		float w, h;
		float kbY, kbH, wkw, bkw, bkh;
		float readY, footY;
	};

	Lay layout() const {
		Lay L;
		L.w = box.size.x; L.h = box.size.y;
		L.readY = L.h * 0.115f;
		L.kbY = L.h * 0.24f;  L.kbH = L.h * 0.46f;
		L.wkw = L.w / 7.f;
		L.bkw = L.wkw * 0.62f;
		L.bkh = L.kbH * 0.62f;
		L.footY = L.h * 0.86f;
		return L;
	}

	// Black keys are drawn on top, so they must be tested first.
	int keyHit(const Lay& L, Vec p) const {
		if (p.y < L.kbY || p.y > L.kbY + L.kbH) return -1;
		if (p.y <= L.kbY + L.bkh) {
			for (int b = 0; b < 5; b++) {
				float x = (float)(KEY_BLACK_AFTER[b] + 1) * L.wkw - L.bkw * 0.5f;
				if (p.x >= x && p.x <= x + L.bkw) return KEY_BLACK[b];
			}
		}
		int wi = (int)std::floor(p.x / L.wkw);
		if (wi < 0 || wi > 6) return -1;
		return KEY_WHITE[wi];
	}

	uint16_t activeMask() const {
		if (!module) return 0b101010110101;
		return module->customMask ? module->mask : module->presetMask(module->scaleIndex);
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) {
			OpaqueWidget::onButton(e);
			return;
		}
		int s = keyHit(layout(), e.pos);
		if (s < 0) return;                       // a miss falls through to the panel
		e.consume(this);
		if (!module->customMask) {               // first edit forks off the preset
			module->mask = module->presetMask(module->scaleIndex);
			module->customMask = true;
		}
		module->mask ^= (uint16_t)(1u << s);
		module->rebuildIntervals();
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!font || font->handle < 0) font = sfs::panelFont();
		if (!font || font->handle < 0) return;
		nvgSave(args.vg);
		nvgScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		if (!module) drawPreview(args);
		else         drawLive(args);
		nvgRestore(args.vg);
	}

	void text(const DrawArgs& args, float x, float y, float size,
	          NVGcolor col, const std::string& t, int align = NVG_ALIGN_CENTER) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, size);
		nvgTextLetterSpacing(args.vg, 0.3f);
		nvgTextAlign(args.vg, align | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, col);
		nvgText(args.vg, x, y, t.c_str(), NULL);
	}

	void drawKeyboard(const DrawArgs& args, const Lay& L, uint16_t m, int root,
	                  const bool* lit) {
		NVGcontext* vg = args.vg;
		// whites
		for (int i = 0; i < 7; i++) {
			int s = KEY_WHITE[i];
			bool in = (m >> s) & 1;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, i * L.wkw + 0.6f, L.kbY, L.wkw - 1.2f, L.kbH, 1.6f);
			nvgFillColor(vg, lit[s] ? sfs::SCREEN_HOT
			                : in    ? sfs::SCREEN_BLUE : nvgRGB(0x2B, 0x2B, 0x44));
			nvgFill(vg);
			if (s == root) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, i * L.wkw + 0.6f, L.kbY, L.wkw - 1.2f, L.kbH, 1.6f);
				nvgStrokeColor(vg, sfs::SCREEN_TEXT);
				nvgStrokeWidth(vg, 1.4f);
				nvgStroke(vg);
			}
		}
		// blacks on top
		for (int b = 0; b < 5; b++) {
			int s = KEY_BLACK[b];
			bool in = (m >> s) & 1;
			float x = (float)(KEY_BLACK_AFTER[b] + 1) * L.wkw - L.bkw * 0.5f;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x, L.kbY, L.bkw, L.bkh, 1.4f);
			nvgFillColor(vg, lit[s] ? sfs::SCREEN_HOT
			                : in    ? sfs::SCREEN_DEEP : nvgRGB(0x16, 0x16, 0x26));
			nvgFill(vg);
			if (s == root) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x, L.kbY, L.bkw, L.bkh, 1.4f);
				nvgStrokeColor(vg, sfs::SCREEN_TEXT);
				nvgStrokeWidth(vg, 1.2f);
				nvgStroke(vg);
			}
		}
	}

	void drawLive(const DrawArgs& args) {
		Lay L = layout();
		uint16_t m = activeMask();

		bool lit[12] = {};
		for (int c = 0; c < KEY_NCH; c++) {
			if (!module->shownActive[c]) continue;
			int s = (int)std::lround(module->shownVolts[c] * 12.f);
			lit[((s % 12) + 12) % 12] = true;
		}

		std::string name = std::string(KEY_NOTES[module->rootNote]) + "  "
		    + (module->customMask ? std::string("CUSTOM")
		                          : std::string(sfs::SCALES[module->scaleIndex].shortName));
		text(args, L.w * 0.5f, L.readY, L.h * 0.15f, sfs::SCREEN_TEXT, name);

		drawKeyboard(args, L, m, module->rootNote, lit);

		// what each channel is putting out
		float cw = L.w / (float)KEY_NCH;
		for (int c = 0; c < KEY_NCH; c++) {
			std::string t = "–";
			if (module->shownActive[c]) {
				int s = (int)std::lround(module->shownVolts[c] * 12.f);
				int pc = ((s % 12) + 12) % 12;
				t = std::string(KEY_NOTES[pc]) + std::to_string(4 + (int)std::floor(s / 12.f));
			}
			text(args, cw * ((float)c + 0.5f), L.footY, L.h * 0.125f,
			     module->shownActive[c] ? sfs::SCREEN_BLUE : sfs::SCREEN_PMID, t);
		}
	}

	void drawPreview(const DrawArgs& args) {
		Lay L = layout();
		uint16_t m = 0b101010110101;                 // D Dorian
		bool lit[12] = {};
		lit[2] = lit[5] = lit[9] = true;
		text(args, L.w * 0.5f, L.readY, L.h * 0.15f, sfs::SCREEN_TEXT, "D  Dorian");
		drawKeyboard(args, L, m, 2, lit);
		static const char* n[KEY_NCH] = {"D3", "F3", "A3", "–"};
		float cw = L.w / (float)KEY_NCH;
		for (int c = 0; c < KEY_NCH; c++)
			text(args, cw * ((float)c + 0.5f), L.footY, L.h * 0.125f,
			     c < 3 ? sfs::SCREEN_BLUE : sfs::SCREEN_PMID, n[c]);
	}
};


// =============================================================================
// Panel — 10HP.
// =============================================================================

struct KeyWidget : ModuleWidget {
	KeyWidget(Key* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/key.svg")));
		using sfs::hp;

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(hp(1), hp(1.6f), "KEY");

		KeyDisplay* disp = new KeyDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(hp(0.8f), hp(2.4f)));
		disp->box.size = mm2px(Vec(hp(8.4f), hp(6.2f)));
		addChild(disp);

		// ── the key ────────────────────────────────────────────────────────────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(2), hp(11))), module, Key::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(4), hp(11))), module, Key::ROOT_INPUT));
		lbl->pair(hp(2), hp(11), "ROOT");

		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(6), hp(11))), module, Key::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(8), hp(11))), module, Key::SCALE_INPUT));
		lbl->pair(hp(6), hp(11), "SCALE");

		// ── four channels, one key ─────────────────────────────────────────────
		// These sit ON the plate: plate ink is light, so above it on the light
		// faceplate they would be invisible.
		lbl->jackOnPlate(hp(2), hp(14.6f), "IN");
		lbl->jackOnPlate(hp(4), hp(14.6f), "OFF");
		lbl->jackOnPlate(hp(6), hp(14.6f), "CHG");
		lbl->jackOnPlate(hp(8), hp(14.6f), "OUT");
		for (int c = 0; c < KEY_NCH; c++) {
			float y = hp(14.6f + 2.f * c);
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(2), y)), module, Key::IN_INPUT + c));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(4), y)), module, Key::OFFSET_PARAM + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(6), y)), module, Key::CHG_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(8), y)), module, Key::OUT_OUTPUT + c));
		}

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(2), hp(23.2f))), module, Key::TRIG_INPUT));
		lbl->jack(hp(2), hp(23.2f), "TRIG");
	}

	void appendContextMenu(Menu* menu) override {
		Key* m = dynamic_cast<Key*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);

		// Degrees is the default because it is the scale-aware answer: +2 moves
		// two steps UP THE SCALE and stays in key, where +2 semitones does not.
		menu->addChild(createBoolPtrMenuItem("Offset in scale degrees", "",
			&m->offsetInDegrees));

		menu->addChild(createIndexPtrSubmenuItem("Rounding",
			{KR_NAME[0], KR_NAME[1], KR_NAME[2]}, &m->roundMode));

		menu->addChild(createSubmenuItem("Hysteresis",
			string::f("%.0f cents", m->hysteresisCents), [=](Menu* sub) {
				struct HystQuantity : Quantity {
					Key* m;
					void setValue(float v) override { m->hysteresisCents = clamp(v, 0.f, 50.f); }
					float getValue() override { return m->hysteresisCents; }
					float getDefaultValue() override { return 12.f; }
					float getMinValue() override { return 0.f; }
					float getMaxValue() override { return 50.f; }
					std::string getLabel() override { return "Hysteresis"; }
					std::string getUnit() override { return " cents"; }
				};
				HystQuantity* q = new HystQuantity;
				q->m = m;
				Slider* sl = new Slider;
				sl->quantity = q;
				sl->box.size.x = 180.f;
				sub->addChild(sl);
			}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Revert keyboard to the selected scale",
			m->customMask ? "" : "already", [=]() {
				m->customMask = false;
				m->rebuildIntervals();
			}));
	}
};

Model* modelKey = createModel<Key, KeyWidget>("Key");
