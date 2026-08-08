// =============================================================================
// OP MORPH — expander for Operator. The DX7 operator routing as a matrix, and
// a field of them you travel across.
//
// A DX7 algorithm is a stack machine over three buses, which is a fast way to
// evaluate something simpler: a table of "how much of operator i is added to
// operator j's PHASE", plus "how much of operator i reaches the output". Every
// one of the 32 evaluates in operator order 0->5 with each modulator strictly
// before what it modulates, so the table is strictly upper triangular — 15
// modulation weights and 6 output weights. See src/msfa/fm_matrix.cc.
//
// The two column groups are both VCAs and feel nothing alike. Columns 0..5 feed
// a PHASE input, so they are FM index: turning one up grows sidebands, it does
// not get louder. Column 6 feeds the output bus and is a plain level. An
// operator with both is modulating its neighbour AND audible in its own right,
// which no DX7 algorithm does.
//
// SLOTS AND THE FIELD. 16 slots on a 4x4 grid, each holding one algorithm, and
// an X/Y position that bilinearly blends the four nearest. Both axes WRAP.
// That is why the field is a torus and not a square with four corners: on a
// square, x=0 and x=1 hold different routings, so wrapping past the edge jumps
// between two structures mid-note — a routing discontinuity, which is a click.
// Tiling means travelling in one direction forever passes through every column
// and interpolates back into the first with no seam anywhere.
//
// Only 11 of the 15 possible modulation edges appear in the 32 algorithms;
// (0,4) (0,5) (1,4) and (2,4) are structures Yamaha never shipped, and no blend
// of stock algorithms can reach them because a blend only produces edges one of
// its endpoints already has. They are reachable by editing a slot's matrix.
// =============================================================================

#include "plugin.hpp"
#include "opmorph-messages.hpp"
#include "bell_voice.h"
#include "panel-style.hpp"
#include <cmath>

static const int OPM_COLS = 4, OPM_ROWS = 4;
static const int OPM_SLOTS = OPM_COLS * OPM_ROWS;

struct OpMorph : Module {
	enum ParamId {
		X_PARAM, Y_PARAM, SPREAD_PARAM, STEP_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		X_CV_INPUT, Y_CV_INPUT, CLOCK_INPUT, RESET_INPUT,
		PARAMS_UNUSED_INPUT,      // reserved; keeps later appends honest
		INPUTS_LEN
	};
	enum OutputId { OUTPUTS_LEN };
	enum LightId { CONNECTED_LIGHT, LIGHTS_LEN };

	// Each slot is an algorithm index by default; a slot whose matrix has been
	// edited keeps its own weights and reports algo = -1.
	int   slotAlgo[OPM_SLOTS];
	float slotW[OPM_SLOTS][6][7];
	int   slotFbSrc[OPM_SLOTS], slotFbDst[OPM_SLOTS];

	float posX = 0.f, posY = 0.f;        // in slot units, wrapped to [0, COLS)
	int   editSlot = 0;
	int   stepIdx = 0;                   // clock walks the field slot by slot
	bool  stepping = false;
	dsp::SchmittTrigger clockTrig, resetTrig;

	// display
	float dispW[6][7] = {};
	bool  dispActive = false;

	OpMorph() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(X_PARAM, 0.f, (float)OPM_COLS, 0.f, "X position", " slots");
		configParam(Y_PARAM, 0.f, (float)OPM_ROWS, 0.f, "Y position", " slots");
		configParam(SPREAD_PARAM, 0.f, 2.f, 1.f, "Depth", "x");
		configSwitch(STEP_PARAM, 0.f, 1.f, 0.f, "Clock steps the field", {"Off", "On"});
		configInput(X_CV_INPUT, "X CV (1V per slot, wraps)");
		configInput(Y_CV_INPUT, "Y CV (1V per slot, wraps)");
		configInput(CLOCK_INPUT, "Clock — steps to the next slot");
		configInput(RESET_INPUT, "Reset to the first slot");
		// A spread of algorithms rather than 1..16: the field is nicer to travel
		// when neighbours differ, and these run stacks -> pairs -> parallel.
		static const int SEED[OPM_SLOTS] = {
			0, 1, 4, 6,  7, 9, 13, 15,  17, 18, 20, 21,  24, 27, 29, 31
		};
		for (int i = 0; i < OPM_SLOTS; i++) loadSlot(i, SEED[i]);
	}

	void loadSlot(int i, int algo) {
		slotAlgo[i] = clamp(algo, 0, 31);
		bellAlgorithmWeights(slotAlgo[i], slotW[i], &slotFbSrc[i], &slotFbDst[i]);
	}

	// Wrap into [0, n). Rack params and CV can both leave the range, and a value
	// that merely clamps would stall at the edge instead of coming round.
	static float wrapf(float v, float n) {
		v = std::fmod(v, n);
		return v < 0.f ? v + n : v;
	}

	void process(const ProcessArgs& args) override {
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) stepIdx = 0;
		stepping = params[STEP_PARAM].getValue() > 0.5f;
		if (stepping && clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
			stepIdx = (stepIdx + 1) % OPM_SLOTS;

		float x, y;
		if (stepping) {
			x = (float)(stepIdx % OPM_COLS);
			y = (float)(stepIdx / OPM_COLS);
		} else {
			x = params[X_PARAM].getValue() + inputs[X_CV_INPUT].getVoltage();
			y = params[Y_PARAM].getValue() + inputs[Y_CV_INPUT].getVoltage();
		}
		posX = wrapf(x, (float)OPM_COLS);
		posY = wrapf(y, (float)OPM_ROWS);

		// Bilinear over the four nearest slots, with wraparound indexing — this
		// is what makes travelling off one edge continuous with the other.
		int x0 = (int)posX, y0 = (int)posY;
		float fx = posX - x0, fy = posY - y0;
		int x1 = (x0 + 1) % OPM_COLS, y1 = (y0 + 1) % OPM_ROWS;
		int s00 = y0 * OPM_COLS + x0, s10 = y0 * OPM_COLS + x1;
		int s01 = y1 * OPM_COLS + x0, s11 = y1 * OPM_COLS + x1;
		float a = (1.f - fx) * (1.f - fy), b = fx * (1.f - fy);
		float c = (1.f - fx) * fy,          d = fx * fy;

		float depth = params[SPREAD_PARAM].getValue();
		float w[6][7];
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 7; j++) {
				float v = a * slotW[s00][i][j] + b * slotW[s10][i][j]
				        + c * slotW[s01][i][j] + d * slotW[s11][i][j];
				// DEPTH scales the FM columns only. Scaling the output column too
				// would just be a volume knob, and there is one of those already.
				w[i][j] = (j < 6) ? v * depth : v;
				dispW[i][j] = w[i][j];
			}
		// The loop follows whichever corner is nearest, since a fractional
		// feedback edge is not a thing the delayed path can express.
		int near = (a >= b && a >= c && a >= d) ? s00
		         : (b >= c && b >= d) ? s10 : (c >= d ? s01 : s11);

		bool motherHere = leftExpander.module && leftExpander.module->model == modelOperator;
		dispActive = motherHere;
		lights[CONNECTED_LIGHT].setBrightness(motherHere ? 1.f : 0.f);
		if (motherHere) {
			OpMorphMessage* msg = (OpMorphMessage*) leftExpander.module->rightExpander.producerMessage;
			if (msg) {
				msg->active = true;
				std::memcpy(msg->w, w, sizeof(w));
				msg->fbSrc = slotFbSrc[near];
				msg->fbDst = slotFbDst[near];
				leftExpander.module->rightExpander.requestMessageFlip();
			}
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* slots = json_array();
		for (int i = 0; i < OPM_SLOTS; i++) json_array_append_new(slots, json_integer(slotAlgo[i]));
		json_object_set_new(root, "slots", slots);
		json_object_set_new(root, "editSlot", json_integer(editSlot));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* slots = json_object_get(root, "slots"))
			for (int i = 0; i < OPM_SLOTS && i < (int)json_array_size(slots); i++)
				loadSlot(i, (int)json_integer_value(json_array_get(slots, i)));
		if (json_t* j = json_object_get(root, "editSlot")) editSlot = (int)json_integer_value(j);
	}
};

// ── display: the field on top, the live matrix underneath ───────────────────
static const NVGcolor OPM_BG    = nvgRGB(0x1a, 0x1a, 0x32);
static const NVGcolor OPM_DIM   = nvgRGB(0x35, 0x35, 0x4d);
static const NVGcolor OPM_MID   = nvgRGB(0x4a, 0x4a, 0x66);
static const NVGcolor OPM_BLUE  = nvgRGB(0x00, 0x97, 0xde);
static const NVGcolor OPM_HOT   = nvgRGB(0xec, 0x65, 0x2e);
static const NVGcolor OPM_TEXT  = nvgRGB(0xe8, 0xe8, 0xf0);

struct OpMorphDisplay : Widget {
	OpMorph* module = nullptr;
	std::shared_ptr<Font> font;

	void drawField(NVGcontext* vg, float x, float y, float w, float h,
	               const int* algo, float px, float py, bool live) {
		float cw = w / OPM_COLS, ch = h / OPM_ROWS;
		for (int r = 0; r < OPM_ROWS; r++)
			for (int c = 0; c < OPM_COLS; c++) {
				float cx = x + c * cw, cy = y + r * ch;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, cx + 1.f, cy + 1.f, cw - 2.f, ch - 2.f, 2.f);
				nvgFillColor(vg, OPM_DIM); nvgFill(vg);
				if (font && font->handle >= 0 && algo) {
					sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
					nvgFillColor(vg, OPM_TEXT);
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgText(vg, cx + cw / 2, cy + ch / 2,
					        string::f("%d", algo[r * OPM_COLS + c] + 1).c_str(), NULL);
				}
			}
		// the travelling point, drawn wrapped so it never disappears at a seam
		if (!live) return;
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++) {
				float gx = x + (px + dx * OPM_COLS) * cw + cw / 2;
				float gy = y + (py + dy * OPM_ROWS) * ch + ch / 2;
				if (gx < x - cw || gx > x + w + cw || gy < y - ch || gy > y + h + ch) continue;
				nvgBeginPath(vg);
				nvgCircle(vg, gx, gy, 3.2f);
				nvgFillColor(vg, OPM_HOT); nvgFill(vg);
			}
	}

	void drawMatrix(NVGcontext* vg, float x, float y, float w, float h, const float ww[6][7]) {
		float cw = w / 7.f, ch = h / 6.f;
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 7; j++) {
				float cx = x + j * cw, cy = y + i * ch;
				bool usable = (j == 6) || (j > i);       // the rest is structurally empty
				float v = clamp(ww ? ww[i][j] : 0.f, 0.f, 1.f);
				nvgBeginPath(vg);
				nvgRoundedRect(vg, cx + 0.8f, cy + 0.8f, cw - 1.6f, ch - 1.6f, 1.5f);
				nvgFillColor(vg, usable ? OPM_DIM : nvgRGB(0x22, 0x22, 0x38));
				nvgFill(vg);
				if (usable && v > 0.004f) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, cx + 0.8f, cy + 0.8f, cw - 1.6f, (ch - 1.6f) * v, 1.5f);
					// the output column is a level; the rest is FM index
					nvgFillColor(vg, j == 6 ? OPM_HOT : OPM_BLUE);
					nvgFill(vg);
				}
			}
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::panelFont();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, OPM_BG); nvgFill(vg);

		float pad = 4.f, w = box.size.x - 2 * pad;
		float fieldH = box.size.y * 0.46f;
		if (!module) { drawPreview(vg, pad, w, fieldH); return; }
		drawField(vg, pad, pad, w, fieldH, module->slotAlgo,
		          module->posX, module->posY, true);
		drawMatrix(vg, pad, pad * 2 + fieldH, w, box.size.y - fieldH - 3 * pad, module->dispW);
		if (!module->dispActive && font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgFillColor(vg, OPM_MID);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
			nvgText(vg, box.size.x / 2, box.size.y - 2.f, "PLACE RIGHT OF OPERATOR", NULL);
		}
	}

	// The browser thumbnail renders with module == NULL; without this it is an
	// empty slab that says nothing about what the module does.
	void drawPreview(NVGcontext* vg, float pad, float w, float fieldH) {
		static const int demo[OPM_SLOTS] = {1,2,5,7, 8,10,14,16, 18,19,21,22, 25,28,30,32};
		drawField(vg, pad, pad, w, fieldH, demo, 1.6f, 2.3f, true);
		static float dm[6][7] = {
			{0,1,0,0,0,0, 0}, {0,0,.65f,0,0,0, .35f}, {0,0,0,1,0,0, 0},
			{0,0,0,0,0,0, 1}, {0,0,0,0,0,.8f, .2f},   {0,0,0,0,0,0, 1}};
		drawMatrix(vg, pad, pad * 2 + fieldH, w, box.size.y - fieldH - 3 * pad, dm);
	}
};

struct OpMorphWidget : ModuleWidget {
	OpMorphWidget(OpMorph* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/opmorph.svg")));
		using sfs::hp;

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(hp(1), hp(1.6f), "MORPH");

		OpMorphDisplay* disp = new OpMorphDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(hp(0.75f), hp(2)));
		disp->box.size = mm2px(Vec(hp(10.5f), hp(13)));
		addChild(disp);

		const float colA = hp(2.75f), colB = hp(9);
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, hp(17))), module, OpMorph::X_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, hp(19))), module, OpMorph::X_CV_INPUT));
		lbl->pairDown(colA, hp(17), hp(19), "X");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, hp(17))), module, OpMorph::Y_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, hp(19))), module, OpMorph::Y_CV_INPUT));
		lbl->pairDown(colB, hp(17), hp(19), "Y");

		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, hp(22))), module, OpMorph::SPREAD_PARAM));
		lbl->trim(colA, hp(22), "DEPTH");
		addParam(createParamCentered<CKSS>(mm2px(Vec(colB, hp(22))), module, OpMorph::STEP_PARAM));
		lbl->trim(colB, hp(22), "STEP");

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, hp(25))), module, OpMorph::CLOCK_INPUT));
		lbl->jack(colA, hp(25), "CLOCK");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, hp(25))), module, OpMorph::RESET_INPUT));
		lbl->jack(colB, hp(25), "RESET");
	}

	void appendContextMenu(Menu* menu) override {
		OpMorph* m = dynamic_cast<OpMorph*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Slots — each holds one algorithm"));
		std::vector<std::string> algs;
		for (int i = 0; i < 32; i++) algs.push_back(string::f("Algorithm %d", i + 1));
		for (int r = 0; r < OPM_ROWS; r++) {
			menu->addChild(createSubmenuItem(string::f("Row %d", r + 1), "", [=](Menu* sub) {
				for (int c = 0; c < OPM_COLS; c++) {
					int i = r * OPM_COLS + c;
					sub->addChild(createIndexSubmenuItem(string::f("Slot %d,%d", c + 1, r + 1), algs,
						[=]() { return m->slotAlgo[i]; },
						[=](int a) { m->loadSlot(i, a); }));
				}
			}));
		}
	}
};

Model* modelOpMorph = createModel<OpMorph, OpMorphWidget>("OpMorph");
