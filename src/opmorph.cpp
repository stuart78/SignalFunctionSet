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
		// APPENDED for the 2026-08 panel, which draws a CV jack under every one
		// of the four pots rather than only under SPEED and SHAPE. They go on
		// the end, after the reserved slot, because inputs serialise by index
		// and putting them beside their siblings would repatch saved patches.
		SPREAD_CV_INPUT, MODE_CV_INPUT,
		INPUTS_LEN
	};

	// How the point travels the field. Borrowed from Gravity, which already
	// solved "make a dot wander somewhere a person would not have drawn".
	// Nothing bounces: the field is a torus, so everything wraps.
	// MOVE_LINEAR is appended, not slotted in front where it reads best: the
	// mode is a stored param index and inserting would re-point saved patches.
	enum MoveMode { MOVE_DRIFT, MOVE_TURTLE, MOVE_WALK, MOVE_CIRCLE,
	                MOVE_LINEAR, MOVE_COUNT };
	enum OutputId { OUTPUTS_LEN };
	enum LightId { CONNECTED_LIGHT, LIGHTS_LEN };

	// Each slot is an algorithm index by default; a slot whose matrix has been
	// edited keeps its own weights and reports algo = -1.
	int   slotAlgo[OPM_SLOTS];
	float slotW[OPM_SLOTS][6][7];
	int   slotFbSrc[OPM_SLOTS], slotFbDst[OPM_SLOTS];

	float posX = 0.f, posY = 0.f;        // the continuous path, always running
	float outX = 0.f, outY = 0.f;        // what actually drives the blend
	int   editSlot = 0;
	int   stepIdx = 0;                   // clock walks the field slot by slot
	bool  stepping = false;
	dsp::SchmittTrigger clockTrig, resetTrig;

	// Where it has been. Sampled on a timer rather than per frame, so the trail
	// is the same length whatever the frame rate, and lives on the module so it
	// survives the display being rebuilt.
	static const int TRAIL_N = 192;
	float trailX[TRAIL_N] = {}, trailY[TRAIL_N] = {};
	int   trailHead = 0, trailFill = 0;
	float trailTimer = 0.f;

	// travel state
	float heading = (float)M_PI * 0.25f;
	float cmdTime = 0.f, cmdDur = 0.f, moveRate = 0.f, turnRate = 0.f, turnAccel = 0.f;
	float circPhase = 0.f, circCx = 2.f, circCy = 2.f;
	int   lastMode = -1;
	bool  everClocked = false;

	// A mask over the matrix, independent of wherever the point is standing:
	// switching a node off mutes that path across the whole field, so you can
	// take one modulation edge out of every algorithm at once. Structurally
	// empty cells (a source cannot modulate itself or anything before it) are
	// not togglable.
	bool cellOn[6][7];

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
		             {"Drift", "Turtle", "Random walk", "Circle", "Linear"});
		getParamQuantity(MODE_PARAM)->snapEnabled = true;
		configParam(SPREAD_PARAM, 0.f, 2.f, 1.f, "Depth", "x");
		// STEP does not replace the movement, it QUANTIZES it: the path keeps
		// running underneath and the clock snaps the output to the nearest slot,
		// so whatever shape the turtle or the walk is drawing comes out on the
		// beat. Linear is the exception -- there a clock means one slot forward.
		configSwitch(STEP_PARAM, 0.f, 1.f, 0.f, "Clock quantizes the movement", {"Off", "On"});
		configInput(SPEED_CV_INPUT, "Speed CV (±5V)");
		configInput(SHAPE_CV_INPUT, "Shape CV (±5V)");
		configInput(SPREAD_CV_INPUT, "Depth CV (±5V)");
		configInput(MODE_CV_INPUT, "Movement CV (1V per mode)");
		configInput(CLOCK_INPUT, "Clock — steps to the next slot");
		configInput(RESET_INPUT, "Return to the first slot");
		// A spread of algorithms rather than 1..16: the field is nicer to travel
		// when neighbours differ, and these run stacks -> pairs -> parallel.
		static const int SEED[OPM_SLOTS] = {
			0, 1, 4, 6,  7, 9, 13, 15,  17, 18, 20, 21,  24, 27, 29, 31
		};
		for (int i = 0; i < OPM_SLOTS; i++) loadSlot(i, SEED[i]);
		for (int i = 0; i < 6; i++) for (int j = 0; j < 7; j++) cellOn[i][j] = true;
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
			trailHead = trailFill = 0; everClocked = false;
		}
		stepping = params[STEP_PARAM].getValue() > 0.5f;
		bool clocked = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		if (stepping && clocked) stepIdx = (stepIdx + 1) % OPM_SLOTS;

		float speed = clamp(params[SPEED_PARAM].getValue()
		                  + inputs[SPEED_CV_INPUT].getVoltage() / 5.f, 0.f, 1.f) * 4.f;
		float shape = clamp(params[SHAPE_PARAM].getValue()
		                  + inputs[SHAPE_CV_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		// 1V per mode, added to the knob, so a sequencer can step the movement
		// the same way ROOT and SCALE are stepped elsewhere in the plugin.
		int mode = clamp((int)std::round(params[MODE_PARAM].getValue()
		                               + inputs[MODE_CV_INPUT].getVoltage()),
		                 0, MOVE_COUNT - 1);
		if (mode != lastMode) { cmdTime = cmdDur = 0.f; lastMode = mode; }

		if (mode == MOVE_LINEAR && stepping) {
			// A plain sequencer: one slot per clock, along the row, on to the next
			// row, and round to the beginning.
			posX = (float)(stepIdx % OPM_COLS);
			posY = (float)(stepIdx / OPM_COLS);
		} else {
			travel(mode, speed, shape, args.sampleTime);
		}

		if (stepping) {
			// Snap on the clock and hold. The path underneath never stops, so the
			// figure it is drawing arrives quantized rather than being replaced by
			// a different, duller one.
			if (clocked || !everClocked) {          // snap once on arrival, then on clocks
				outX = wrapf(std::round(posX), (float)OPM_COLS);
				outY = wrapf(std::round(posY), (float)OPM_ROWS);
				everClocked = true;
			}
		} else {
			outX = posX; outY = posY;
			everClocked = false;               // so re-arming STEP snaps immediately
		}

		// Bilinear over the four nearest slots, with wraparound indexing — this
		// is what makes travelling off one edge continuous with the other.
		int x0 = (int)outX, y0 = (int)outY;
		float fx = outX - x0, fy = outY - y0;
		int x1 = (x0 + 1) % OPM_COLS, y1 = (y0 + 1) % OPM_ROWS;
		int s00 = y0 * OPM_COLS + x0, s10 = y0 * OPM_COLS + x1;
		int s01 = y1 * OPM_COLS + x0, s11 = y1 * OPM_COLS + x1;
		float a = (1.f - fx) * (1.f - fy), b = fx * (1.f - fy);
		float c = (1.f - fx) * fy,          d = fx * fy;

		float depth = clamp(params[SPREAD_PARAM].getValue()
		                  + inputs[SPREAD_CV_INPUT].getVoltage() / 5.f * 2.f, 0.f, 2.f);
		float w[6][7];
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 7; j++) {
				float v = a * slotW[s00][i][j] + b * slotW[s10][i][j]
				        + c * slotW[s01][i][j] + d * slotW[s11][i][j];
				// DEPTH scales the FM columns only. Scaling the output column too
				// would just be a volume knob, and there is one of those already.
				w[i][j] = (j < 6) ? v * depth : v;
				if (!cellOn[i][j]) w[i][j] = 0.f;
				dispW[i][j] = w[i][j];
			}
		// The loop follows whichever corner is nearest, since a fractional
		// feedback edge is not a thing the delayed path can express.
		int near = (a >= b && a >= c && a >= d) ? s00
		         : (b >= c && b >= d) ? s10 : (c >= d ? s01 : s11);

		bool motherHere = leftExpander.module && leftExpander.module->model == modelOperator;
		dispActive = motherHere;
		lights[CONNECTED_LIGHT].setBrightness(motherHere ? 1.f : 0.f);
		trailTimer += args.sampleTime;
		if (trailTimer >= 0.008f) {                 // ~1.5s of trail over 192 points
			trailTimer = 0.f;
			trailX[trailHead] = outX; trailY[trailHead] = outY;
			trailHead = (trailHead + 1) % TRAIL_N;
			if (trailFill < TRAIL_N) trailFill++;
		}

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
		json_t* mask = json_array();
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 7; j++) json_array_append_new(mask, json_boolean(cellOn[i][j]));
		json_object_set_new(root, "cellOn", mask);
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* slots = json_object_get(root, "slots"))
			for (int i = 0; i < OPM_SLOTS && i < (int)json_array_size(slots); i++)
				loadSlot(i, (int)json_integer_value(json_array_get(slots, i)));
		if (json_t* j = json_object_get(root, "editSlot")) editSlot = (int)json_integer_value(j);
		if (json_t* mask = json_object_get(root, "cellOn"))
			for (int i = 0; i < 6; i++)
				for (int j = 0; j < 7; j++) {
					json_t* v = json_array_get(mask, i * 7 + j);
					if (v) cellOn[i][j] = json_boolean_value(v);
				}
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

	// ── the split, in ONE place ─────────────────────────────────────────────
	// These four numbers were written out four times over -- in both hit tests
	// and both draw paths -- with the same two literals each time. That is how
	// a click comes to land on a different cell from the one it is drawn over.
	//
	// THE FIELD IS A FIXED HEIGHT AND THE MATRIX TAKES THE REST. It used to be
	// 46% of the screen, so a taller screen grew both of them in proportion --
	// and the 2026-08 art made the screen taller specifically to give the
	// matrix more room. Stated as a height rather than a share, the extra goes
	// where the art meant it to go. The clamp keeps it sane if the screen ever
	// shrinks instead.
	// The screen is the whole interface here -- sixteen slots and forty-two
	// routing cells, none of which can carry a label at this size -- so what a
	// thing IS gets said on hover rather than printed.
	ui::Tooltip* tip = nullptr;
	std::string hoverStr;
	std::string hoverAt(Vec p);

	static constexpr float PAD = 4.f;
	// A NAME GUTTER AND A HEADER, as Sigma's matrix has. Without them nothing on
	// screen said the rows were operators, the columns were their destinations,
	// or what the orange column was -- so the colour was carrying a meaning it
	// had no way to explain, and the only reader who could decode it was one who
	// already knew. They come out of the matrix's own area, so the cells and the
	// hit test shrink together.
	static constexpr float GUT = 9.f;      // row names, down the left
	static constexpr float HDR = 8.f;      // column names, along the top
	float bodyW()  const { return box.size.x - 2 * PAD; }
	float fieldH() const { return std::min(mm2px(30.4f), box.size.y * 0.5f); }
	float matX()   const { return PAD + GUT; }
	float matW()   const { return bodyW() - GUT; }
	float matY()   const { return PAD * 2 + fieldH() + HDR; }
	float matH()   const { return box.size.y - fieldH() - 3 * PAD - HDR; }

	// The trail, oldest faintest. A segment is DROPPED where the point crossed
	// a seam: on a torus a wrap is a jump of nearly the whole field, and joining
	// those two ends draws a line straight back across everything the point did
	// not travel.
	void drawTrail(NVGcontext* vg, float x, float y, float cw, float ch) {
		if (!module || module->trailFill < 2) return;
		int n = module->trailFill;
		int oldest = (module->trailHead - n + OpMorph::TRAIL_N) % OpMorph::TRAIL_N;
		float px = 0.f, py = 0.f;
		bool have = false;
		for (int k = 0; k < n; k++) {
			int i = (oldest + k) % OpMorph::TRAIL_N;
			float tx = x + module->trailX[i] * cw + cw / 2;
			float ty = y + module->trailY[i] * ch + ch / 2;
			if (have && std::fabs(tx - px) < cw * OPM_COLS * 0.5f
			         && std::fabs(ty - py) < ch * OPM_ROWS * 0.5f) {
				nvgBeginPath(vg);
				nvgMoveTo(vg, px, py);
				nvgLineTo(vg, tx, ty);
				nvgStrokeColor(vg, nvgTransRGBA(OPM_HOT, (int)(12 + 90.f * k / n)));
				nvgStrokeWidth(vg, 1.3f);
				nvgStroke(vg);
			}
			px = tx; py = ty; have = true;
		}
	}

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
		drawTrail(vg, x, y, cw, ch);
		float gx = x + px * cw + cw / 2, gy = y + py * ch + ch / 2;
		nvgBeginPath(vg);
		nvgCircle(vg, gx, gy, 4.2f);
		nvgFillColor(vg, nvgTransRGBA(OPM_HOT, 60)); nvgFill(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, gx, gy, 2.6f);
		nvgFillColor(vg, OPM_HOT); nvgFill(vg);
		nvgRestore(vg);
	}

	// The matrix is drawn the way Sigma draws its mod matrix, and for the same
	// two reasons.
	//
	// A LATTICE, ruled all the way across, rather than forty-two separate
	// tiles. Drawn one at a time with a gap between them, the block read as a
	// scatter of little meters and you could not see that the OUT column was a
	// column. One wash of field with dark rules cut through it in both
	// directions makes the seven destinations line up as columns and the six
	// operators as rows, which is what you are reading when you look for a
	// route.
	//
	// A DOT THAT RIDES A LINE, not a bar that grows from one. A bar has AREA,
	// and area reads as quantity even when there is almost none of it -- a cell
	// at 5% still drew a visible slab, so a sparse algorithm looked busy. A dot
	// sitting ON the baseline is plainly off, and its distance from that line is
	// the amount.
	void drawMatrix(NVGcontext* vg, float x, float y, float w, float h, const float ww[6][7]) {
		float cw = w / 7.f, ch = h / 6.f;
		auto usableAt = [](int i, int j) { return (j == 6) || (j > i); };

		nvgBeginPath(vg);
		nvgRect(vg, x, y, w, h);
		nvgFillColor(vg, OPM_DIM);
		nvgFill(vg);

		// the structurally empty half, painted back over the wash: no operator
		// can modulate itself or anything above it, so those cells are not
		// controls that happen to be at zero
		nvgFillColor(vg, nvgRGB(0x22, 0x22, 0x38));
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 7; j++)
				if (!usableAt(i, j)) {
					nvgBeginPath(vg);
					nvgRect(vg, x + j * cw, y + i * ch, cw, ch);
					nvgFill(vg);
				}

		// CENTRELINES, through the middle of each cell, and drawn before the
		// rules so the rules cut them. They sat near the cell's floor at first,
		// on the argument that these values are unipolar and so have no need of
		// the lower half. That was reasoning about the numbers rather than about
		// the picture: a line a couple of pixels above the row rule reads as a
		// doubled rule, the row looks ragged, and the matrix stops being a
		// lattice. A line down the middle of a cell is the one place it cannot
		// be confused with the cell's own edge.
		float inset = std::min(2.6f, ch * 0.22f);
		nvgFillColor(vg, OPM_MID);
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 7; j++) {
				if (!usableAt(i, j)) continue;
				nvgBeginPath(vg);
				nvgRect(vg, x + j * cw, y + i * ch + ch * 0.5f - 0.5f, cw, 1.f);
				nvgFill(vg);
			}

		nvgFillColor(vg, OPM_BG);
		for (int j = 0; j <= 7; j++) {          // verticals, both edges in
			nvgBeginPath(vg);
			nvgRect(vg, x + j * cw - 0.5f, y, 1.f, h);
			nvgFill(vg);
		}
		for (int i = 1; i < 6; i++) {           // horizontals, between rows
			nvgBeginPath(vg);
			nvgRect(vg, x, y + i * ch - 0.5f, w, 1.f);
			nvgFill(vg);
		}

		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 7; j++) {
				if (!usableAt(i, j)) continue;
				float cx = x + j * cw, cy = y + i * ch;
				float base = cy + ch * 0.5f;
				bool on = !module || module->cellOn[i][j];
				// A node switched off keeps a mark, so you can see it is a path
				// you muted rather than one the field is simply not using.
				if (!on) {
					nvgBeginPath(vg);
					nvgMoveTo(vg, cx + 3.f, cy + ch * 0.5f);
					nvgLineTo(vg, cx + cw - 3.f, cy + ch * 0.5f);
					nvgStrokeColor(vg, OPM_MID);
					nvgStrokeWidth(vg, 1.f);
					nvgStroke(vg);
					continue;
				}
				float v = clamp(ww ? ww[i][j] : 0.f, 0.f, 1.f);
				// Unipolar, so the dot only ever rises: half the travel Sigma
				// has, for a value that can only go one way.
				float travel = ch * 0.5f - inset;
				float dy = base - v * travel;
				bool lit = v > 0.004f;
				// the output column is a level; the rest is FM index
				NVGcolor col = (j == 6) ? OPM_HOT : OPM_BLUE;
				if (lit) {                      // a stem back to the baseline, so
					nvgBeginPath(vg);           // the eye can read the distance
					nvgMoveTo(vg, cx + cw * 0.5f, base);
					nvgLineTo(vg, cx + cw * 0.5f, dy);
					nvgStrokeColor(vg, nvgTransRGBA(col, 150));
					nvgStrokeWidth(vg, 1.f);
					nvgStroke(vg);
				}
				nvgBeginPath(vg);
				nvgCircle(vg, cx + cw * 0.5f, dy, 2.f);
				nvgFillColor(vg, lit ? col : OPM_MID);
				nvgFill(vg);
			}

		// Rows are the SOURCE operator, columns the DESTINATION -- w is [src][dst]
		// and the last column is the output bus. OUT is named rather than merely
		// coloured differently, because a colour cannot say what it means.
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgFillColor(vg, OPM_MID);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
			for (int j = 0; j < 7; j++)
				nvgText(vg, x + j * cw + cw * 0.5f, y - 1.5f,
				        j == 6 ? "OUT" : string::f("%d", j + 1).c_str(), NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			for (int i = 0; i < 6; i++)
				nvgText(vg, x - 2.5f, y + i * ch + ch * 0.5f,
				        string::f("%d", i + 1).c_str(), NULL);
		}
	}

	// Which matrix cell is under a point, packed as i*7+j, or -1.
	int cellAt(Vec p) {
		float mx = matX(), w = matW(), my = matY(), mh = matH();
		if (p.x < mx || p.x > mx + w || p.y < my || p.y > my + mh) return -1;
		int j = (int)((p.x - mx) / (w / 7.f));
		int i = (int)((p.y - my) / (mh / 6.f));
		if (i < 0 || i > 5 || j < 0 || j > 6) return -1;
		if (!(j == 6 || j > i)) return -1;            // structurally empty
		return i * 7 + j;
	}

	// Which slot is under a point, or -1 outside the field.
	int slotAt(Vec p) {
		float w = bodyW(), fh = fieldH();
		if (p.x < PAD || p.x > PAD + w || p.y < PAD || p.y > PAD + fh) return -1;
		int c = (int)((p.x - PAD) / (w / OPM_COLS));
		int r = (int)((p.y - PAD) / (fh / OPM_ROWS));
		if (c < 0 || c >= OPM_COLS || r < 0 || r >= OPM_ROWS) return -1;
		return r * OPM_COLS + c;
	}

	// Click a slot to select it, drag up/down or scroll to change its algorithm.
	// Doing this from the context menu was four clicks deep for one number.
	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			int s = slotAt(e.pos);
			if (s >= 0) { module->editSlot = s; e.consume(this); return; }
			int c = cellAt(e.pos);
			if (c >= 0) {
				module->cellOn[c / 7][c % 7] = !module->cellOn[c / 7][c % 7];
				e.consume(this); return;
			}
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

	// Driven from the pointer position rather than from onHover/onLeave, and
	// that is deliberate. This widget is a plain Widget and does not CONSUME the
	// hover event -- it must not, or a click on the screen's empty space would
	// stop falling through to the module and you could no longer drag the module
	// by its screen. But a widget that never consumes hover never becomes the
	// hovered widget, so it is never sent onLeave either: the readout would
	// simply keep saying whatever it last said, with the tooltip stranded on
	// screen after the pointer had gone. Testing containment each frame has no
	// such hole, and it keeps the number live during a drag for free -- dragging
	// a slot walks its algorithm, and during an edit is exactly when you want to
	// see the value.
	void step() override {
		// DIVIDE BY THE ZOOM. getAbsoluteOffset() transforms a LOCAL vector into
		// absolute coordinates, and ZoomWidget scales it on the way, so
		//     absolute(p) = offset0 + zoom * p_local
		// and recovering p_local needs both the subtraction and the division.
		// With only the subtraction the error is (zoom - 1) * p_local: zero at
		// the widget's top-left corner and growing with distance from it. Zoomed
		// in, the readout therefore worked near the corner, named the wrong cell
		// further out, and stopped appearing at all once the inflated point fell
		// outside the box -- which reads as "it works for a bit and then stops".
		float z = getAbsoluteZoom();
		if (z <= 0.f) z = 1.f;
		Vec p = APP->scene->mousePos.minus(getAbsoluteOffset(Vec(0, 0))).div(z);
		hoverStr = box.zeroPos().contains(p) ? hoverAt(p) : std::string();
		bool want = !hoverStr.empty();
		if (want && !tip) { tip = new ui::Tooltip; APP->scene->addChild(tip); }
		else if (!want && tip) {
			APP->scene->removeChild(tip); delete tip; tip = nullptr;
		}
		if (tip) {
			tip->text = hoverStr;
			tip->box.pos = APP->scene->mousePos.plus(Vec(15, 15));
		}
		Widget::step();
	}
	~OpMorphDisplay() override {
		// The tooltip is parented to the SCENE, not to this widget, so it does
		// not go away when the widget does -- a module deleted with the pointer
		// over its screen would otherwise leave the readout floating there.
		if (tip) { APP->scene->removeChild(tip); delete tip; tip = nullptr; }
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, OPM_BG); nvgFill(vg);

		float w = bodyW(), fh = fieldH();
		if (!module) { drawPreview(vg, PAD, w, fh); return; }
		selected = module->editSlot;
		drawField(vg, PAD, PAD, w, fh, module->slotAlgo,
		          module->outX, module->outY, true);
		drawMatrix(vg, matX(), matY(), matW(), matH(), module->dispW);
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
		drawMatrix(vg, matX(), matY(), matW(), matH(), dm);
	}
};

// What the pointer is over, already formatted. The hit tests are the SAME ones
// a click uses -- slotAt() and cellAt() -- so the readout cannot describe a cell
// other than the one a click would land on. Computing the indices a second time
// from the same geometry is a standing invitation for the tooltip to name one
// cell while the click edits its neighbour.
std::string OpMorphDisplay::hoverAt(Vec p) {
	if (!module) return "";
	int sl = slotAt(p);
	if (sl >= 0) {
		int a = module->slotAlgo[clamp(sl, 0, OPM_SLOTS - 1)];
		std::string what = (a < 0) ? std::string("edited")
		                           : string::f("algorithm %d", a + 1);
		// Which slot the field is actually sitting on is the one thing the
		// numbers on screen cannot say, since the dot is between four of them.
		const char* mark = (sl == module->editSlot) ? "   (editing)" : "";
		return string::f("Slot %d,%d   %s%s",
		                 sl % OPM_COLS + 1, sl / OPM_COLS + 1, what.c_str(), mark);
	}
	int c = cellAt(p);
	if (c >= 0) {
		int i = c / 7, j = c % 7;
		float v = clamp(module->dispW[i][j], 0.f, 1.f);
		if (!module->cellOn[i][j])
			return j == 6 ? string::f("OP %d -> OUT   muted", i + 1)
			              : string::f("OP %d -> OP %d   muted", i + 1, j + 1);
		// The output column is a LEVEL and the rest is an FM INDEX. They are
		// different quantities that happen to share a scale, and the tooltip is
		// the only place on this screen that can say so.
		return j == 6 ? string::f("OP %d -> OUT   level %.0f%%", i + 1, v * 100.f)
		              : string::f("OP %d -> OP %d   index %.0f%%", i + 1, j + 1, v * 100.f);
	}
	return "";
}

// ── the 2026-08 grid, transcribed from res/opmorph.svg ─────────────────────
// The screen grew 5mm taller and every macro gained a pot with a CV jack under
// it; STEP stayed a switch, drawn as a rounded rect rather than a circle.
static const float SCR_X = 5.08f, SCR_Y = 10.16f, SCR_W = 50.96f, SCR_H = 71.11f;
static const float KX[4] = {7.75f, 19.17f, 30.60f, 42.03f};   // pots
static const float KY = 93.58f;
static const float JX[4] = {7.58f, 19.00f, 30.43f, 41.86f};   // their CV
static const float JY = 105.26f;
static const float STEP_X = 53.33f, STEP_Y = 93.88f;
static const float BX[2] = {7.66f, 19.00f};                   // clock, reset
static const float BY = 121.26f;

struct OpMorphWidget : ModuleWidget {
	OpMorphWidget(OpMorph* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/opmorph.svg")));

		static const int KP[4] = {OpMorph::SPEED_PARAM, OpMorph::SHAPE_PARAM,
		                          OpMorph::SPREAD_PARAM, OpMorph::MODE_PARAM};
		static const int KI[4] = {OpMorph::SPEED_CV_INPUT, OpMorph::SHAPE_CV_INPUT,
		                          OpMorph::SPREAD_CV_INPUT, OpMorph::MODE_CV_INPUT};

		// NO PanelLabels, and no title. This panel's artwork carries its own
		// text as outlined paths and Rack renders those, so a runtime label
		// layer prints every word twice about half a millimetre off -- which
		// reads as a blurry panel rather than as an obvious duplicate. Do not
		// "fix" the missing labels by adding them back.

		OpMorphDisplay* disp = new OpMorphDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(SCR_X, SCR_Y));
		disp->box.size = mm2px(Vec(SCR_W, SCR_H));
		addChild(disp);

		// A pot per macro with its CV directly underneath, in the same order,
		// so a cable hangs under the thing it modulates.
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(KX[i], KY)), module, KP[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JX[i], JY)), module, KI[i]));
		}
		addParam(createParamCentered<CKSS>(mm2px(Vec(STEP_X, STEP_Y)), module, OpMorph::STEP_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BX[0], BY)), module, OpMorph::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BX[1], BY)), module, OpMorph::RESET_INPUT));
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
