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
	// Indices are stable across the reworkings: an absolute X/Y became a
	// heading and a speed, and then a speed and a shape, all in place.
	enum ParamId {
		SPEED_PARAM, SHAPE_PARAM, SPREAD_PARAM, STEP_PARAM,
		MODE_PARAM,               // appended
		PARAMS_LEN
	};
	enum InputId {
		SPEED_CV_INPUT, SHAPE_CV_INPUT, CLOCK_INPUT, RESET_INPUT,
		PARAMS_UNUSED_INPUT,      // reserved; keeps later appends honest
		INPUTS_LEN
	};

	// How the point travels the field. Borrowed from Gravity, which already
	// solved "make a dot wander somewhere a person would not have drawn".
	// Nothing bounces: the field is a torus, so everything wraps.
	enum MoveMode { MOVE_DRIFT, MOVE_TURTLE, MOVE_WALK, MOVE_CIRCLE, MOVE_COUNT };
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

	// travel state
	float heading = (float)M_PI * 0.25f;
	float cmdTime = 0.f, cmdDur = 0.f, moveRate = 0.f, turnRate = 0.f, turnAccel = 0.f;
	float circPhase = 0.f, circCx = 2.f, circCy = 2.f;
	int   lastMode = -1;

	// display
	float dispW[6][7] = {};
	bool  dispActive = false;

	OpMorph() {
		// The WRITER owns the buffers. Writing into the mother's own outgoing
		// buffer instead puts two modules on one allocation and makes the result
		// depend on which of them Rack happens to process first.
		leftExpander.producerMessage = new OpMorphMessage();
		leftExpander.consumerMessage = new OpMorphMessage();
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// SPEED is always speed; SHAPE is whatever gives that mode its character.
		configParam(SPEED_PARAM, 0.f, 1.f, 0.15f, "Speed", " slots/s", 0.f, 4.f);
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.35f, "Shape — heading / turniness / wander / radius");
		configSwitch(MODE_PARAM, 0.f, (float)(MOVE_COUNT - 1), (float)MOVE_TURTLE, "Movement",
		             {"Drift", "Turtle", "Random walk", "Circle"});
		getParamQuantity(MODE_PARAM)->snapEnabled = true;
		configParam(SPREAD_PARAM, 0.f, 2.f, 1.f, "Depth", "x");
		configSwitch(STEP_PARAM, 0.f, 1.f, 0.f, "Clock steps the field", {"Off", "On"});
		configInput(SPEED_CV_INPUT, "Speed CV (±5V)");
		configInput(SHAPE_CV_INPUT, "Shape CV (±5V)");
		configInput(CLOCK_INPUT, "Clock — steps to the next slot");
		configInput(RESET_INPUT, "Return to the first slot");
		// A spread of algorithms rather than 1..16: the field is nicer to travel
		// when neighbours differ, and these run stacks -> pairs -> parallel.
		static const int SEED[OPM_SLOTS] = {
			0, 1, 4, 6,  7, 9, 13, 15,  17, 18, 20, 21,  24, 27, 29, 31
		};
		for (int i = 0; i < OPM_SLOTS; i++) loadSlot(i, SEED[i]);
	}

	~OpMorph() {
		delete (OpMorphMessage*) leftExpander.producerMessage;
		delete (OpMorphMessage*) leftExpander.consumerMessage;
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

	void advance(float dx, float dy) {
		posX = wrapf(posX + dx, (float)OPM_COLS);
		posY = wrapf(posY + dy, (float)OPM_ROWS);
	}

	// A LOGO turtle, after Gravity's: hold a command for a while, then pick
	// another. Straight and gentle veers are common, corners and about-faces
	// rare, and SHAPE tilts the dice toward the rare ones while shortening how
	// long each command is held.
	void newTurtleCmd(float speed, float shape) {
		float u = 1.f - shape;
		cmdDur = 0.25f + 3.5f * u * u;            // 0.25 .. 3.75 s
		cmdTime = 0.f;
		float V = 0.15f + speed;                  // slots/s
		float W = (40.f + 260.f * shape) * (float)M_PI / 180.f;
		turnAccel = 0.f;
		float r = random::uniform();
		float wild = 0.15f + 0.55f * shape;       // chance of something unusual
		if (r > wild)            { moveRate = V;        turnRate = 0.f; }
		else if (r > wild * 0.6f){ moveRate = V;        turnRate = (random::uniform() < .5f ? 1 : -1) * W * .35f; }
		else if (r > wild * 0.3f){ moveRate = V * .85f; turnRate = (random::uniform() < .5f ? 1 : -1) * W; }
		else if (r > wild * 0.1f){ moveRate = V * .25f; turnRate = (random::uniform() < .5f ? 1 : -1) * W * 2.6f; }
		else {                                     // a spiral, tightening as it goes
			float d = (random::uniform() < .5f) ? 1.f : -1.f;
			moveRate = V; turnRate = d * W * .25f; turnAccel = d * W * 1.4f;
		}
	}

	void travel(int mode, float speed, float shape, float dt) {
		switch (mode) {
			case MOVE_DRIFT: {                     // a straight line, forever
				float th = shape * 2.f * (float)M_PI;
				advance(std::cos(th) * speed * dt, std::sin(th) * speed * dt);
				break;
			}
			case MOVE_TURTLE: {
				cmdTime += dt;
				if (cmdTime >= cmdDur) newTurtleCmd(speed, shape);
				turnRate += turnAccel * dt;
				heading  += turnRate * dt;
				advance(std::sin(heading) * moveRate * dt, std::cos(heading) * moveRate * dt);
				break;
			}
			case MOVE_WALK: {
				// Brownian in HEADING rather than in position: jittering the
				// position directly just vibrates in place, while jittering the
				// direction actually goes somewhere.
				//
				// Scaled by sqrt(dt), not dt. A random step per sample accumulates
				// as sqrt(n), so scaling it linearly makes the wander depend on the
				// host's sample rate -- the same patch would drift differently at
				// 96k than at 48k, and far too slowly at either.
				heading += (2.f * random::uniform() - 1.f) * shape * 6.f * std::sqrt(dt);
				advance(std::sin(heading) * speed * dt, std::cos(heading) * speed * dt);
				break;
			}
			case MOVE_CIRCLE: {
				float rad = 0.25f + shape * 1.75f;
				circPhase += speed * dt / std::max(rad, 0.25f);
				if (circPhase > 2.f * (float)M_PI) circPhase -= 2.f * (float)M_PI;
				posX = wrapf(circCx + std::cos(circPhase) * rad, (float)OPM_COLS);
				posY = wrapf(circCy + std::sin(circPhase) * rad, (float)OPM_ROWS);
				break;
			}
		}
	}

	void process(const ProcessArgs& args) override {
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			stepIdx = 0; posX = posY = 0.f;
			heading = (float)M_PI * 0.25f; circPhase = 0.f; cmdTime = cmdDur = 0.f;
		}
		stepping = params[STEP_PARAM].getValue() > 0.5f;
		if (stepping && clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
			stepIdx = (stepIdx + 1) % OPM_SLOTS;

		if (stepping) {
			posX = (float)(stepIdx % OPM_COLS);
			posY = (float)(stepIdx / OPM_COLS);
		} else {
			float speed = clamp(params[SPEED_PARAM].getValue()
			                  + inputs[SPEED_CV_INPUT].getVoltage() / 5.f, 0.f, 1.f) * 4.f;
			float shape = clamp(params[SHAPE_PARAM].getValue()
			                  + inputs[SHAPE_CV_INPUT].getVoltage() / 5.f, 0.f, 1.f);
			int mode = clamp((int)std::round(params[MODE_PARAM].getValue()), 0, MOVE_COUNT - 1);
			if (mode != lastMode) { cmdTime = cmdDur = 0.f; lastMode = mode; }
			travel(mode, speed, shape, args.sampleTime);
		}

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
		OpMorphMessage* msg = (OpMorphMessage*) leftExpander.producerMessage;
		if (msg) {
			msg->active = motherHere;
			std::memcpy(msg->w, w, sizeof(w));
			msg->fbSrc = slotFbSrc[near];
			msg->fbDst = slotFbDst[near];
			leftExpander.requestMessageFlip();
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

	int selected = -1;
	void drawField(NVGcontext* vg, float x, float y, float w, float h,
	               const int* algo, float px, float py, bool live) {
		float cw = w / OPM_COLS, ch = h / OPM_ROWS;
		for (int r = 0; r < OPM_ROWS; r++)
			for (int c = 0; c < OPM_COLS; c++) {
				float cx = x + c * cw, cy = y + r * ch;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, cx + 1.f, cy + 1.f, cw - 2.f, ch - 2.f, 2.f);
				nvgFillColor(vg, OPM_DIM); nvgFill(vg);
				if (r * OPM_COLS + c == selected) {
					nvgStrokeColor(vg, OPM_BLUE); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
				}
				if (font && font->handle >= 0 && algo) {
					sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
					nvgFillColor(vg, OPM_TEXT);
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgText(vg, cx + cw / 2, cy + ch / 2,
					        string::f("%d", algo[r * OPM_COLS + c] + 1).c_str(), NULL);
				}
			}
		// The travelling point. ONE dot, and scissored to the field: the wrapped
		// copies used to be drawn nine times with a loose bounds test, which put
		// stray dots outside the screen area entirely.
		if (!live) return;
		nvgSave(vg);
		nvgScissor(vg, x, y, w, h);
		float gx = x + px * cw + cw / 2, gy = y + py * ch + ch / 2;
		nvgBeginPath(vg);
		nvgCircle(vg, gx, gy, 4.2f);
		nvgFillColor(vg, nvgTransRGBA(OPM_HOT, 60)); nvgFill(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, gx, gy, 2.6f);
		nvgFillColor(vg, OPM_HOT); nvgFill(vg);
		nvgRestore(vg);
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

	// Which slot is under a point, or -1 outside the field.
	int slotAt(Vec p) {
		float pad = 4.f, w = box.size.x - 2 * pad, fieldH = box.size.y * 0.46f;
		if (p.x < pad || p.x > pad + w || p.y < pad || p.y > pad + fieldH) return -1;
		int c = (int)((p.x - pad) / (w / OPM_COLS));
		int r = (int)((p.y - pad) / (fieldH / OPM_ROWS));
		if (c < 0 || c >= OPM_COLS || r < 0 || r >= OPM_ROWS) return -1;
		return r * OPM_COLS + c;
	}

	// Click a slot to select it, drag up/down or scroll to change its algorithm.
	// Doing this from the context menu was four clicks deep for one number.
	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			int s = slotAt(e.pos);
			if (s >= 0) { module->editSlot = s; e.consume(this); return; }
		}
		Widget::onButton(e);
	}
	void onDragStart(const DragStartEvent& e) override { dragAccum = 0.f; }
	void onDragMove(const DragMoveEvent& e) override {
		if (!module) return;
		dragAccum -= e.mouseDelta.y / APP->scene->rackScroll->getZoom();
		while (dragAccum >= 6.f) { bump(+1); dragAccum -= 6.f; }
		while (dragAccum <= -6.f) { bump(-1); dragAccum += 6.f; }
	}
	void onHoverScroll(const HoverScrollEvent& e) override {
		if (!module) return;
		int s = slotAt(e.pos);
		if (s < 0) return;
		module->editSlot = s;
		bump(e.scrollDelta.y > 0.f ? +1 : -1);
		e.consume(this);
	}
	void bump(int d) {
		int i = clamp(module->editSlot, 0, OPM_SLOTS - 1);
		module->loadSlot(i, ((module->slotAlgo[i] + d) % 32 + 32) % 32);
	}
	float dragAccum = 0.f;

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::panelFont();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, OPM_BG); nvgFill(vg);

		float pad = 4.f, w = box.size.x - 2 * pad;
		float fieldH = box.size.y * 0.46f;
		if (!module) { drawPreview(vg, pad, w, fieldH); return; }
		selected = module->editSlot;
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
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, hp(17))), module, OpMorph::SPEED_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA, hp(19))), module, OpMorph::SPEED_CV_INPUT));
		lbl->pairDown(colA, hp(17), hp(19), "SPEED");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colB, hp(17))), module, OpMorph::SHAPE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB, hp(19))), module, OpMorph::SHAPE_CV_INPUT));
		lbl->pairDown(colB, hp(17), hp(19), "SHAPE");

		addParam(createParamCentered<Trimpot>(mm2px(Vec(colA, hp(22))), module, OpMorph::SPREAD_PARAM));
		lbl->trim(colA, hp(22), "DEPTH");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(6), hp(22))), module, OpMorph::MODE_PARAM));
		lbl->trim(hp(6), hp(22), "MOVE");
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
