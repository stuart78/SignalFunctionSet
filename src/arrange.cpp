#include "plugin.hpp"
#include "scales.hpp"
#include <cmath>
#include <cstring>

// ─── Arrange — song-form / arrangement sequencer (linear, 4 channels) ──────────
// One horizontal chain of 8 phrases (bar-length sections), advancing linearly
// (next active phrase, wrapping). Each phrase carries a bar length (4×4 grid,
// 1-16), an enable, a root + scale + BPM (trimpots under the display), and four
// CHANNEL enables (colored bars under the grid). The 4 channels are per-instrument
// clock buses: each has a clock division of the master CLOCK input and its own
// BAR / CLOCK / RESET / EOC outputs, all silenced on phrases where that channel
// is disabled — so instruments drop in and out per phrase. LED link dots
// between the trimpot columns cascade scale/root/BPM, each attribute linking
// INDEPENDENTLY on its own row (default linked): a linked phrase inherits the
// value of its group's leftmost phrase exactly (its own trimpot is ignored);
// break the link and the phrase is independent. Driven by a bar-rate pulse on BAR in
// (Meter's BAR). Master outputs: phrase index (1V/phrase), BPM (0.01V/BPM →
// Meter), root + scale CV (Note convention). Bar/clock reach instruments via
// the channel outs (Meter remains the master source).

static const int NUM_PHRASES  = 8;
static const int MAX_BARS     = 16;
static const int NUM_CHANNELS = 4;
static const int NUM_LINKS    = NUM_PHRASES - 1;

static const int DIV_VALUES[8] = {1, 2, 3, 4, 6, 8, 12, 16};

// Channel identity colors (from the guide SVG) + solid darker "off" variants.
static const NVGcolor CH_COLOR[NUM_CHANNELS] = {
	{{{0x00/255.f, 0x97/255.f, 0xDE/255.f, 1.f}}},   // CH1 blue
	{{{0x00/255.f, 0xC3/255.f, 0x00/255.f, 1.f}}},   // CH2 green
	{{{0xFF/255.f, 0x4B/255.f, 0x00/255.f, 1.f}}},   // CH3 orange-red
	{{{0x7B/255.f, 0x25/255.f, 0xCB/255.f, 1.f}}},   // CH4 purple
};
static const NVGcolor CH_COLOR_OFF[NUM_CHANNELS] = {
	{{{0x0D/255.f, 0x59/255.f, 0x86/255.f, 1.f}}},
	{{{0x06/255.f, 0x4E/255.f, 0x06/255.f, 1.f}}},
	{{{0x66/255.f, 0x1E/255.f, 0x00/255.f, 1.f}}},
	{{{0x33/255.f, 0x10/255.f, 0x54/255.f, 1.f}}},
};

struct Arrange : Module {
	enum ParamId {
		RESET_PARAM,
		ENUMS(SCALE_PARAM, NUM_PHRASES),
		ENUMS(ROOT_PARAM,  NUM_PHRASES),
		ENUMS(BPM_PARAM,   NUM_PHRASES),
		ENUMS(DIV_PARAM,   NUM_CHANNELS),
		ENUMS(LINK_SCALE_PARAM, NUM_LINKS),   // scale / root / BPM link independently
		ENUMS(LINK_ROOT_PARAM,  NUM_LINKS),
		ENUMS(LINK_BPM_PARAM,   NUM_LINKS),
		PARAMS_LEN
	};
	enum InputId { BAR_INPUT, CLOCK_INPUT, RESET_INPUT, INPUTS_LEN };
	enum OutputId {
		PHRASE_OUTPUT, BPM_OUTPUT, ROOT_OUTPUT, SCALE_OUTPUT,
		ENUMS(CH_BAR_OUTPUT,   NUM_CHANNELS),
		ENUMS(CH_CLOCK_OUTPUT, NUM_CHANNELS),
		ENUMS(CH_RESET_OUTPUT, NUM_CHANNELS),
		ENUMS(CH_EOC_OUTPUT,   NUM_CHANNELS),
		ENUMS(PGATE_OUTPUT,    NUM_PHRASES),   // per-phrase gate: high while that phrase plays
		OUTPUTS_LEN
	};
	enum LightId { RUN_LIGHT, LIGHTS_LEN };   // link buttons draw their own state (LinkDotButton)

	// per-phrase arrangement data (bars/active/chanOn as arrays+JSON; root/scale/bpm are params)
	int  bars[NUM_PHRASES];
	bool active[NUM_PHRASES];
	bool chanOn[NUM_PHRASES][NUM_CHANNELS];   // channel enabled during this phrase

	int  curPhrase = 0, barInPhrase = 0, barsSinceReset = 0, editPhrase = 0;
	bool started = false;

	dsp::SchmittTrigger barTrig, clockTrig, resetTrigIn, resetTrigBtn;
	dsp::PulseGenerator chBar[NUM_CHANNELS], chClk[NUM_CHANNELS], chRst[NUM_CHANNELS], chEoc[NUM_CHANNELS];
	int clkCount[NUM_CHANNELS] = {};

	Arrange() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(RESET_PARAM, "Reset");
		std::vector<std::string> scaleNames, rootNames = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
		for (int i = 0; i < sfs::NUM_SCALES; i++) scaleNames.push_back(sfs::SCALES[i].longName);
		for (int i = 0; i < NUM_PHRASES; i++) {
			configSwitch(SCALE_PARAM + i, 0.f, (float)(sfs::NUM_SCALES - 1), 1.f, string::f("P%d scale", i + 1), scaleNames);
			configSwitch(ROOT_PARAM + i, 0.f, 11.f, 0.f, string::f("P%d root", i + 1), rootNames);
			configParam(BPM_PARAM + i, 30.f, 300.f, 120.f, string::f("P%d BPM", i + 1), " BPM");
			getParamQuantity(SCALE_PARAM + i)->snapEnabled = true;
			getParamQuantity(ROOT_PARAM + i)->snapEnabled = true;
			getParamQuantity(BPM_PARAM + i)->snapEnabled = true;
		}
		for (int i = 0; i < NUM_LINKS; i++) {   // scale/root/BPM link independently; default LINKED
			configSwitch(LINK_SCALE_PARAM + i, 0.f, 1.f, 1.f, string::f("Link P%d → P%d scale (P%d inherits the group leader's scale)", i + 1, i + 2, i + 2), {"Independent", "Linked"});
			configSwitch(LINK_ROOT_PARAM + i,  0.f, 1.f, 1.f, string::f("Link P%d → P%d root (P%d inherits the group leader's root)", i + 1, i + 2, i + 2), {"Independent", "Linked"});
			configSwitch(LINK_BPM_PARAM + i,   0.f, 1.f, 1.f, string::f("Link P%d → P%d BPM (P%d inherits the group leader's BPM)", i + 1, i + 2, i + 2), {"Independent", "Linked"});
		}
		for (int c = 0; c < NUM_CHANNELS; c++) {
			configSwitch(DIV_PARAM + c, 0.f, 7.f, 0.f, string::f("Channel %d clock division", c + 1),
				{"÷1", "÷2", "÷3", "÷4", "÷6", "÷8", "÷12", "÷16"});
			configOutput(CH_BAR_OUTPUT + c,   string::f("Channel %d bar (muted on phrases where the channel is off)", c + 1));
			configOutput(CH_CLOCK_OUTPUT + c, string::f("Channel %d clock (master clock ÷ division; muted when the channel is off)", c + 1));
			configOutput(CH_RESET_OUTPUT + c, string::f("Channel %d reset (fires on master reset and when the channel re-enables)", c + 1));
			configOutput(CH_EOC_OUTPUT + c,   string::f("Channel %d end of cycle (arrangement wrap)", c + 1));
		}
		configInput(BAR_INPUT, "Bar (one pulse per bar — e.g. Meter's BAR out; advances the arrangement)");
		configInput(CLOCK_INPUT, "Clock (master clock — divided per channel)");
		configInput(RESET_INPUT, "Reset");
		configOutput(PHRASE_OUTPUT, "Phrase index (1V/phrase, phrase 1 = 0V)");
		configOutput(BPM_OUTPUT, "BPM CV (0.01V per BPM) — into Meter");
		configOutput(ROOT_OUTPUT, "Root note CV of the current phrase (1V/oct, semitone-quantized) — into Note/Chance root CV");
		configOutput(SCALE_OUTPUT, "Scale-select CV of the current phrase (1V per scale) — into Note/Chance scale CV");
		for (int i = 0; i < NUM_PHRASES; i++)
			configOutput(PGATE_OUTPUT + i, string::f("Phrase %d gate (high while phrase %d is playing)", i + 1, i + 1));
		for (int i = 0; i < NUM_PHRASES; i++) {
			bars[i] = 4; active[i] = true;
			for (int c = 0; c < NUM_CHANNELS; c++) chanOn[i][c] = true;
		}
	}

	int  rootOf(int i)  { return clamp((int)std::round(params[ROOT_PARAM + i].getValue()), 0, 11); }
	int  scaleOf(int i) { return clamp((int)std::round(params[SCALE_PARAM + i].getValue()), 0, sfs::NUM_SCALES - 1); }
	int  bpmOf(int i)   { return clamp((int)std::round(params[BPM_PARAM + i].getValue()), 30, 300); }
	int  divOf(int c)   { return DIV_VALUES[clamp((int)std::round(params[DIV_PARAM + c].getValue()), 0, 7)]; }
	// Cascade — scale/root/BPM each link INDEPENDENTLY (default linked): a linked
	// phrase follows the LEFTMOST phrase of its contiguous group for that attribute,
	// inheriting its value EXACTLY (the follower's own trimpot is ignored while
	// linked — no offsets; unlike Cycle, an arrangement link means "same key/tempo").
	// Break the link and the phrase's own trimpot applies again.
	bool linkOn(int base, int i) { return i > 0 && params[base + i - 1].getValue() > 0.5f; }
	int  headOf(int base, int i) { int j = i; while (linkOn(base, j)) j--; return j; }
	int  effRoot(int i)  { return rootOf(headOf(LINK_ROOT_PARAM, i)); }
	int  effScale(int i) { return scaleOf(headOf(LINK_SCALE_PARAM, i)); }
	int  effBpm(int i)   { return bpmOf(headOf(LINK_BPM_PARAM, i)); }
	int  firstActive() { for (int i = 0; i < NUM_PHRASES; i++) if (active[i]) return i; return 0; }
	int  nextActive(int from) { for (int k = 1; k <= NUM_PHRASES; k++) { int j = (from + k) % NUM_PHRASES; if (active[j]) return j; } return from; }
	void doReset() {
		curPhrase = firstActive(); barInPhrase = 0; barsSinceReset = 0; started = false;
		for (int c = 0; c < NUM_CHANNELS; c++) { clkCount[c] = 0; chRst[c].trigger(1e-3f); }
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		if (resetTrigBtn.process(params[RESET_PARAM].getValue())
		    || resetTrigIn.process(inputs[RESET_INPUT].getVoltage())) doReset();

		// BAR edge — advances the arrangement (processed before CLOCK so a coincident
		// downbeat clock lands on the freshly-reset division counters).
		bool bar = barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f);
		if (bar) {
			if (!started) { started = true; curPhrase = firstActive(); barInPhrase = 0; barsSinceReset = 1; }
			else {
				barsSinceReset++; barInPhrase++;
				if (barInPhrase >= clamp(bars[curPhrase], 1, MAX_BARS)) {
					int old = curPhrase, nxt = nextActive(curPhrase);
					if (nxt <= old) {   // wrapped — the arrangement completed a full cycle
						for (int c = 0; c < NUM_CHANNELS; c++) if (chanOn[old][c]) chEoc[c].trigger(1e-3f);
					}
					curPhrase = nxt; barInPhrase = 0;
					// channel comes back in on this phrase → resync its downstream rig
					for (int c = 0; c < NUM_CHANNELS; c++)
						if (!chanOn[old][c] && chanOn[curPhrase][c]) chRst[c].trigger(1e-3f);
				}
			}
			for (int c = 0; c < NUM_CHANNELS; c++) {
				clkCount[c] = 0;   // divisions re-align to the bar
				if (chanOn[curPhrase][c]) chBar[c].trigger(1e-3f);
			}
		}

		// CLOCK edge — per-channel division (counters run even while a channel is off
		// so a re-enabled channel stays phase-locked to the master).
		// Gate the OUTPUT on `started`: before the first BAR (e.g. just after a
		// reset while Meter keeps running) the division phase is anchored to the
		// reset moment, not the bar. Emitting those pre-downbeat clocks makes Note
		// play off-grid until the next BAR snaps clkCount back — the "takes a
		// measure or two to click" bug. Counters still advance so a channel
		// re-enabled mid-arrangement stays phase-locked.
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			for (int c = 0; c < NUM_CHANNELS; c++) {
				if (started && clkCount[c] % divOf(c) == 0 && chanOn[curPhrase][c]) chClk[c].trigger(1e-3f);
				clkCount[c]++;
			}
		}

		outputs[PHRASE_OUTPUT].setVoltage(clamp((float)curPhrase, 0.f, 10.f));
		outputs[BPM_OUTPUT].setVoltage(clamp(effBpm(curPhrase) * 0.01f, 0.f, 10.f));
		// Root/Scale of the playing phrase — same convention as Note (1V/oct root, 1V/scale) so
		// Phrase's ROOT/SCALE out drive Note's or Chance's root/scale CV interchangeably.
		outputs[ROOT_OUTPUT].setVoltage((float)effRoot(curPhrase) / 12.f);
		outputs[SCALE_OUTPUT].setVoltage((float)effScale(curPhrase));
		for (int c = 0; c < NUM_CHANNELS; c++) {
			outputs[CH_BAR_OUTPUT + c].setVoltage(chBar[c].process(dt) ? 10.f : 0.f);
			outputs[CH_CLOCK_OUTPUT + c].setVoltage(chClk[c].process(dt) ? 10.f : 0.f);
			outputs[CH_RESET_OUTPUT + c].setVoltage(chRst[c].process(dt) ? 10.f : 0.f);
			outputs[CH_EOC_OUTPUT + c].setVoltage(chEoc[c].process(dt) ? 10.f : 0.f);
		}
		// per-phrase gates: the playing phrase's out sits high for its whole duration
		for (int i = 0; i < NUM_PHRASES; i++)
			outputs[PGATE_OUTPUT + i].setVoltage((started && i == curPhrase) ? 10.f : 0.f);
		lights[RUN_LIGHT].setBrightness(started ? 1.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* ba = json_array(), *ac = json_array(), *ch = json_array();
		for (int i = 0; i < NUM_PHRASES; i++) {
			json_array_append_new(ba, json_integer(bars[i]));
			json_array_append_new(ac, json_boolean(active[i]));
			for (int c = 0; c < NUM_CHANNELS; c++) json_array_append_new(ch, json_boolean(chanOn[i][c]));
		}
		json_object_set_new(root, "bars", ba);   json_object_set_new(root, "active", ac);
		json_object_set_new(root, "chanOn", ch);
		json_object_set_new(root, "editPhrase", json_integer(editPhrase));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* a = json_object_get(root, "bars")) for (int i=0;i<NUM_PHRASES;i++) if (json_t* v=json_array_get(a,i)) bars[i]=clamp((int)json_integer_value(v),1,MAX_BARS);
		if (json_t* a = json_object_get(root, "active")) for (int i=0;i<NUM_PHRASES;i++) if (json_t* v=json_array_get(a,i)) active[i]=json_boolean_value(v);
		if (json_t* a = json_object_get(root, "chanOn")) for (int i=0;i<NUM_PHRASES;i++) for (int c=0;c<NUM_CHANNELS;c++) if (json_t* v=json_array_get(a,i*NUM_CHANNELS+c)) chanOn[i][c]=json_boolean_value(v);
		if (json_t* j = json_object_get(root, "editPhrase")) editPhrase = clamp((int)json_integer_value(j), 0, NUM_PHRASES - 1);
	}
};

// ─── Display ─────────────────────────────────────────────────────────────────
// Guide design space 400 × 157 (SVG/Phrase v3.svg), elements sized up ~12%.
// s = w/400. See docs/conventions/screen-style.md.
static const float DESIGN_W = 400.f;
static const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

struct ArrangeDisplay : OpaqueWidget {
	Arrange* module = nullptr;
	std::shared_ptr<Font> font;

	float S() const { return box.size.x / DESIGN_W; }
	float uc(float u) const { return u * S(); }
	rack::math::Rect cellRect(int i) const { return rack::math::Rect(Vec(uc(14 + i*48), uc(18)), Vec(uc(36), uc(36))); }
	rack::math::Rect gridCell(int i, int k) const { int c=k%4, r=k/4; return rack::math::Rect(Vec(uc(15 + i*48 + c*9), uc(68 + r*9)), Vec(uc(7), uc(7))); }
	rack::math::Rect chanRect(int i, int c) const { return rack::math::Rect(Vec(uc(15 + i*48), uc(106 + c*11)), Vec(uc(34), uc(9))); }

	void txt(NVGcontext* vg, float x, float y, float sz, NVGcolor col, int align, const char* t) {
		if (!font) return; nvgFontFaceId(vg, font->handle); nvgFontSize(vg, sz); nvgTextAlign(vg, align); nvgFillColor(vg, col); nvgText(vg, x, y, t, NULL);
	}
	void drawTri(NVGcontext* vg, Vec c, float h, NVGcolor col) {   // right-pointing
		nvgBeginPath(vg); nvgMoveTo(vg, c.x + h, c.y); nvgLineTo(vg, c.x - h*0.85f, c.y + h*0.85f); nvgLineTo(vg, c.x - h*0.85f, c.y - h*0.85f); nvgClosePath(vg);
		nvgFillColor(vg, col); nvgFill(vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		NVGcontext* vg = args.vg; float s = S();
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 4.f*s); nvgFillColor(vg, nvgRGB(0x1a,0x1a,0x2e)); nvgFill(vg);

		int bars[NUM_PHRASES], root[NUM_PHRASES], scl[NUM_PHRASES], bpm[NUM_PHRASES];
		bool active[NUM_PHRASES], chan[NUM_PHRASES][NUM_CHANNELS];
		bool inhR[NUM_PHRASES], inhS[NUM_PHRASES], inhB[NUM_PHRASES];   // per-attribute follower flags
		int cur=0, ep=0, barIn=0, totBars=0; bool running=false;
		if (module) {
			for (int i=0;i<NUM_PHRASES;i++){ bars[i]=module->bars[i]; active[i]=module->active[i];
				root[i]=module->effRoot(i); scl[i]=module->effScale(i); bpm[i]=module->effBpm(i);
				inhR[i]=module->headOf(Arrange::LINK_ROOT_PARAM,i)!=i;   // following via link → rendered dimmer
				inhS[i]=module->headOf(Arrange::LINK_SCALE_PARAM,i)!=i;
				inhB[i]=module->headOf(Arrange::LINK_BPM_PARAM,i)!=i;
				for (int c=0;c<NUM_CHANNELS;c++) chan[i][c]=module->chanOn[i][c]; }
			cur=module->curPhrase; ep=module->editPhrase; barIn=module->barInPhrase; totBars=module->barsSinceReset; running=module->started;
		} else {
			int db[8]={4,8,4,16,4,8,4,4};
			for (int i=0;i<NUM_PHRASES;i++){ bars[i]=db[i]; active[i]=true; root[i]=0; scl[i]=1; bpm[i]=120;
				inhR[i]=inhS[i]=inhB[i]=(i>0);   // default state: everything linked, following P1
				for (int c=0;c<NUM_CHANNELS;c++) chan[i][c]=true; }
			root[1]=1; scl[1]=2; inhR[1]=inhS[1]=false;             // demo: P2 root+scale unlinked (C# Minor)
			chan[1][3]=false; chan[3][1]=false; chan[3][2]=false;   // demo: some channels dropped out
			cur=0; ep=0; barIn=0; totBars=0; running=true;
		}

		txt(vg, uc(14), uc(9), 11.f*s, nvgRGB(0xec,0x65,0x2e), NVG_ALIGN_LEFT|NVG_ALIGN_MIDDLE, string::f("Bar %d.%d", totBars, barIn+1).c_str());

		// linear next arrows between adjacent cells (triangle only, no line)
		for (int i = 0; i < NUM_PHRASES - 1; i++)
			drawTri(vg, Vec(uc(56 + i*48), uc(36)), 5.f*s, nvgRGB(0x00,0x97,0xde));

		// phrase cells
		for (int i = 0; i < NUM_PHRASES; i++) {
			rack::math::Rect rc = cellRect(i);
			bool play = (i==cur) && running, focus = (i==ep);
			NVGcolor bg = !active[i] ? nvgRGB(0x1f,0x1f,0x34) : focus ? nvgRGB(0x00,0x97,0xde) : nvgRGB(0x35,0x35,0x4d);
			nvgBeginPath(vg); nvgRoundedRect(vg, rc.pos.x, rc.pos.y, rc.size.x, rc.size.y, 2.f*s); nvgFillColor(vg, bg); nvgFill(vg);
			if (play) { nvgBeginPath(vg); nvgRoundedRect(vg, rc.pos.x+0.7f*s, rc.pos.y+0.7f*s, rc.size.x-1.4f*s, rc.size.y-1.4f*s, 2.f*s); nvgStrokeColor(vg, nvgRGB(0xec,0x65,0x2e)); nvgStrokeWidth(vg, 1.5f*s); nvgStroke(vg); }
			NVGcolor tcCol = focus ? nvgRGB(0xff,0xff,0xff) : !active[i] ? nvgRGB(0x6e,0x6e,0x8a) : nvgRGB(0xe6,0xe6,0xf0);
			// each attribute dims independently when it follows a link (or the phrase is off)
			NVGcolor sCol  = (!active[i] || inhS[i]) ? nvgRGB(0x6e,0x6e,0x8a) : tcCol;
			NVGcolor rCol  = (!active[i] || inhR[i]) ? nvgRGB(0x8a,0x52,0x3a) : nvgRGB(0xec,0x65,0x2e);
			NVGcolor bCol  = (!active[i] || inhB[i]) ? nvgRGB(0x8a,0x52,0x3a) : nvgRGB(0xec,0x65,0x2e);
			txt(vg, rc.pos.x+5.5f*s, rc.pos.y+13*s, 12.f*s, tcCol, NVG_ALIGN_LEFT|NVG_ALIGN_MIDDLE, string::f("P%d", i+1).c_str());
			txt(vg, rc.pos.x+rc.size.x-4.5f*s, rc.pos.y+12*s, 9.f*s, rCol, NVG_ALIGN_RIGHT|NVG_ALIGN_MIDDLE, NOTE_NAMES[clamp(root[i],0,11)]);
			// scale name, truncated so it stays inside the cell ("Chromatic" → "Chroma")
			char scName[8]; const char* scFull = sfs::SCALES[clamp(scl[i],0,sfs::NUM_SCALES-1)].shortName;
			// Bounded by construction. The previous version was safe -- strlen <= 7
			// plus a NUL is exactly 8 -- but it took a branch and an unbounded
			// strcpy to be safe, and neither the reader nor the analyser can see
			// that at a glance.
			std::snprintf(scName, sizeof(scName), "%.*s",
			              std::strlen(scFull) > 7 ? 6 : 7, scFull);
			txt(vg, rc.pos.x+rc.size.x*0.5f, rc.pos.y+rc.size.y-9*s, 9.f*s, sCol, NVG_ALIGN_CENTER|NVG_ALIGN_MIDDLE, scName);
			// per-phrase BPM, centered under the cell (bright orange on the playing phrase)
			txt(vg, rc.pos.x+rc.size.x*0.5f, uc(60.5f), 8.f*s, (i==cur) ? nvgRGB(0xec,0x65,0x2e) : bCol, NVG_ALIGN_CENTER|NVG_ALIGN_MIDDLE, string::f("%d BPM", bpm[i]).c_str());
		}

		// 4×4 bar grids
		for (int i = 0; i < NUM_PHRASES; i++)
			for (int k = 0; k < MAX_BARS; k++) {
				rack::math::Rect g = gridCell(i,k);
				bool on = k < clamp(bars[i],1,MAX_BARS), here = running && (i==cur) && (k==barIn);
				nvgBeginPath(vg); nvgRoundedRect(vg, g.pos.x, g.pos.y, g.size.x, g.size.y, 1.f*s);
				nvgFillColor(vg, here ? nvgRGB(0xec,0x65,0x2e) : on ? nvgRGB(0x00,0x97,0xde) : nvgRGB(0x35,0x35,0x4d)); nvgFill(vg);
			}

		// per-phrase channel enable bars (solid darker color when off — no transparency)
		for (int i = 0; i < NUM_PHRASES; i++)
			for (int c = 0; c < NUM_CHANNELS; c++) {
				rack::math::Rect b = chanRect(i,c);
				nvgBeginPath(vg); nvgRoundedRect(vg, b.pos.x, b.pos.y, b.size.x, b.size.y, 1.5f*s);
				nvgFillColor(vg, chan[i][c] ? CH_COLOR[c] : CH_COLOR_OFF[c]); nvgFill(vg);
			}
	}

	// ── interaction ──
	void onButton(const ButtonEvent& e) override {
		if (!module) { OpaqueWidget::onButton(e); return; }
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			for (int i = 0; i < NUM_PHRASES; i++)
				if (cellRect(i).contains(e.pos)) { phraseMenu(i); e.consume(this); return; }
		}
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			lastPos = e.pos;
			for (int i = 0; i < NUM_PHRASES; i++) {
				for (int k = 0; k < MAX_BARS; k++) if (gridCell(i,k).contains(e.pos)) { module->bars[i] = k+1; module->editPhrase = i; e.consume(this); return; }
				for (int c = 0; c < NUM_CHANNELS; c++) if (chanRect(i,c).contains(e.pos)) { module->chanOn[i][c] = !module->chanOn[i][c]; e.consume(this); return; }
				if (cellRect(i).contains(e.pos)) { module->editPhrase = i; e.consume(this); return; }
			}
		}
		OpaqueWidget::onButton(e);
	}
	void onDoubleClick(const DoubleClickEvent& e) override {
		if (module) for (int i = 0; i < NUM_PHRASES; i++) if (cellRect(i).contains(lastPos)) { module->active[i] = !module->active[i]; e.consume(this); return; }
		OpaqueWidget::onDoubleClick(e);
	}
	void phraseMenu(int i) {
		Menu* m = createMenu();
		m->addChild(createMenuLabel(string::f("Phrase %d", i + 1)));
		m->addChild(createSubmenuItem("Root", NOTE_NAMES[module->rootOf(i)], [=](Menu* sub){
			for (int r = 0; r < 12; r++) sub->addChild(createCheckMenuItem(NOTE_NAMES[r], "",
				[=](){ return module->rootOf(i) == r; },
				[=](){ module->params[Arrange::ROOT_PARAM + i].setValue((float)r); }));
		}));
		m->addChild(createSubmenuItem("Scale", sfs::SCALES[module->scaleOf(i)].longName, [=](Menu* sub){
			for (int sc = 0; sc < sfs::NUM_SCALES; sc++) sub->addChild(createCheckMenuItem(sfs::SCALES[sc].longName, "",
				[=](){ return module->scaleOf(i) == sc; },
				[=](){ module->params[Arrange::SCALE_PARAM + i].setValue((float)sc); }));
		}));
	}
	Vec lastPos;
};

// ─── Widget ──────────────────────────────────────────────────────────────────
// Tiny custom-drawn latch dot for the 21 link buttons — every stock Rack button
// is trimpot-sized or bigger, so this draws its own 3.4mm dot: bright when
// linked (self-illuminating on layer 1), dark when independent.
struct LinkDotButton : app::Switch {
	LinkDotButton() { momentary = false; box.size = mm2px(Vec(4.2f, 4.2f)); }
	bool isOn() {
		ParamQuantity* pq = getParamQuantity();
		return pq ? pq->getValue() > 0.5f : true;   // browser preview shows the default (linked)
	}
	void draw(const DrawArgs& args) override {
		float cx = box.size.x * 0.5f, r = mm2px(1.7f);
		nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cx, r);
		nvgFillColor(args.vg, isOn() ? nvgRGB(0xf2,0xf2,0xf2) : nvgRGB(0x3a,0x3a,0x44)); nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x2a,0x2a,0x33)); nvgStrokeWidth(args.vg, 1.0f); nvgStroke(args.vg);
	}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1 && isOn()) {
			float cx = box.size.x * 0.5f;
			nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cx, mm2px(1.35f));
			nvgFillColor(args.vg, nvgRGB(0xff,0xff,0xff)); nvgFill(args.vg);
		}
		app::Switch::drawLayer(args, layer);
	}
};

struct ArrangeWidget : ModuleWidget {
	ArrangeWidget(Arrange* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/arrange.svg")));
		// No virtual screws — see CLAUDE.md.

		ArrangeDisplay* disp = new ArrangeDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(4.f, 12.f));
		disp->box.size = mm2px(Vec(124.f, 48.7f));
		addChild(disp);

		// 3 trimpot rows (Scale / Root / BPM) — one under each phrase column — with an
		// LED link dot between columns on EACH row, and the per-phrase GATE out below.
		// Positions from Stuart's res/arrange.svg art.
		for (int i = 0; i < NUM_PHRASES; i++) {
			float x = 13.92f + i * 14.88f;
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, 70.f)), module, Arrange::SCALE_PARAM + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, 82.f)), module, Arrange::ROOT_PARAM + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, 94.f)), module, Arrange::BPM_PARAM + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 106.68f)), module, Arrange::PGATE_OUTPUT + i));
			if (i < NUM_LINKS) {
				float lx = x + 7.44f;
				addParam(createParamCentered<LinkDotButton>(mm2px(Vec(lx, 70.f)), module, Arrange::LINK_SCALE_PARAM + i));
				addParam(createParamCentered<LinkDotButton>(mm2px(Vec(lx, 82.f)), module, Arrange::LINK_ROOT_PARAM + i));
				addParam(createParamCentered<LinkDotButton>(mm2px(Vec(lx, 94.f)), module, Arrange::LINK_BPM_PARAM + i));
			}
		}

		// 4 channel columns (dark inset): DIV on top, then CLOCK (right under its divider),
		// BAR, RESET, EOC; the master outs sit at the bottom of the inset.
		for (int c = 0; c < NUM_CHANNELS; c++) {
			float x = 134.62f + c * 10.16f;
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, 25.4f)), module, Arrange::DIV_PARAM + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 40.64f)), module, Arrange::CH_CLOCK_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 60.96f)), module, Arrange::CH_BAR_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 81.28f)), module, Arrange::CH_RESET_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 99.9f)), module, Arrange::CH_EOC_OUTPUT + c));
		}
		const float yb = 121.92f;
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(134.62f, yb)), module, Arrange::BPM_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(144.78f, yb)), module, Arrange::ROOT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(154.94f, yb)), module, Arrange::SCALE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(165.1f, yb)),  module, Arrange::PHRASE_OUTPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.92f, yb)), module, Arrange::BAR_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(28.8f, yb)),  module, Arrange::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.68f, yb)), module, Arrange::RESET_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(58.56f, yb)),  module, Arrange::RESET_PARAM));
	}

	void appendContextMenu(Menu* menu) override {
		Arrange* m = dynamic_cast<Arrange*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Trimpots = scale / root / BPM · LEDs link each row independently (linked = inherit)"));
		menu->addChild(createMenuLabel("Grid = bars · colored bars = channel enables · dbl-click phrase = on/off"));
	}
};

Model* modelArrange = createModel<Arrange, ArrangeWidget>("Arrange");
