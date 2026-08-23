#include "plugin.hpp"
#include "panel-style.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// Trace — a paper loop, four brushes, one read head per lane.
//
// Part Oramics (which read drawn shapes off moving film), part chart recorder
// (whose pens sit at different points along the drum, which is why the read
// offsets here are per lane rather than global).
//
// TWO THINGS CARRY IT, and neither is "a curve editor".
//
// 1. THE TRANSPORT IS UNCOUPLED FROM THE BRUSH. Every draw-a-shape module is a
//    static editor: you see the whole loop, edit it as an object, and playback
//    is a cursor sweeping your artwork. Here the paper moves whether you are
//    drawing or not, which makes it an instrument. It also gives three
//    behaviours for free, decided by nothing but where the mouse is: left of
//    NOW you are editing what just played, right of it you are composing the
//    future with lead time, on it you are performing.
//
// 2. INK IS A SECOND DIMENSION. The brush lays down weight as well as position,
//    so every lane carries two signals. Ink accumulates over repeated passes
//    and pools where the line sits still, which means the thickness track is
//    automatically "how settled is this line" — sustains heavy, transitions
//    light — without anyone drawing it.
//
// Events come from the SHAPE, not the value: each lane classifies its slope as
// up, down or flat and fires on the transitions, so a scribble makes notes
// where it turns around and no quantization is involved.
//
// THE BRUSH CANNOT BE DOWN WHILE THE PAPER RUNS BACKWARDS. Stated as a state
// rule rather than "a direction change lifts the brush", because the state
// version already answers what happens if you press the mouse while reversed
// (nothing is written) where an edge rule has to answer that separately and
// gets it wrong. A brush writing onto paper moving the other way retraces over
// what it has just laid down, so the stroke eats itself; solving that means
// deciding whether reverse writing blends with or erases what is under it, and
// there is no reason to decide that before the module exists.
//
// See docs/trace-design.md.
// =============================================================================

static const int TR_LANES = 4;

// The paper is a physical LENGTH, not a duration: LENGTH sets how many cells go
// round and SPEED sets how fast they pass the head, so speeding up the
// transport plays the drawing faster the way tape does rather than resampling
// it. Resolution in time therefore varies with speed, which is correct.
static const double TR_CELLS_PER_SEC = 1000.0;   // free-running
static const double TR_CELLS_PER_BAR = 2000.0;   // clocked
static const int    TR_MAXCELLS      = 64000;    // 32 bars, or 64 seconds
static const int    TR_MINCELLS      = 64;

// LENGTH is one knob with the unit swapping under it, as Slice's REACH is: one
// range, because two would leave most of the travel dead in whichever mode was
// not being used.
static const float TR_LENBARS[]  = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
static const int   TR_NLENBARS   = 10;

// Slew, clocked. Snapped to bar fractions so "one bar" is a position on the
// knob rather than a number you have to hunt for.
static const float TR_SLEWBARS[] = {0.f, 1/16.f, 1/8.f, 1/4.f, 1/3.f, 1/2.f,
                                    1.f, 2.f, 4.f, 8.f};
static const int   TR_NSLEWBARS  = 10;

// What a lane's TRIG jack fires on. One jack per lane with the condition
// selected per lane, which absorbs what would otherwise have been a separate
// gate output for quantized lanes.
enum TrCond {
	TC_ANY,        // any orientation change
	TC_UP,         // turns up
	TC_DOWN,       // turns down
	TC_FLAT,       // goes flat
	TC_UNFLAT,     // leaves flat
	TC_STEP,       // the quantized value changed
	TC_GATE_UP,    // gate, high while rising
	TC_GATE_DOWN,  // gate, high while falling
	TC_GATE_FLAT,  // gate, high while flat
	TC_NCOND
};
static const char* TR_CONDNAME[TC_NCOND] = {
	"Any change", "Turns up", "Turns down", "Goes flat", "Leaves flat",
	"Step change", "Gate: rising", "Gate: falling", "Gate: flat"
};
static inline bool trCondIsGate(int c) { return c >= TC_GATE_UP; }

static const char* TR_QUANTNAME[] = {"Off", "Semitones", "2 steps", "3 steps",
	"4 steps", "5 steps", "6 steps", "8 steps", "12 steps", "16 steps"};
static const int   TR_QUANTSTEPS[] = {0, -1, 2, 3, 4, 5, 6, 8, 12, 16};
static const int   TR_NQUANT = 10;

static inline int trWrap(int i, int n) { i %= n; return i < 0 ? i + n : i; }

// How many cells a stroke takes to join what it lands on and blend back into
// what it lifts off. In CELLS rather than milliseconds because the paper is the
// domain, so it is the physical width of the brush's entry rather than a time.
static const int TR_EDGE = 24;

// ── base64, for the paper ────────────────────────────────────────────────────
// The paper IS the patch's content, so it has to be saved, and a JSON array of
// integers is both enormous and slow to parse. Stored at a quarter of the
// paper's cell resolution and interpolated back on load: 250Hz is far above
// anything a hand-drawn CV line contains, and it takes 720KB down to 180.
static const char* TR_B64 =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string trEncode(const std::vector<uint8_t>& d) {
	std::string o;
	o.reserve((d.size() + 2) / 3 * 4);
	for (size_t i = 0; i < d.size(); i += 3) {
		uint32_t n = (uint32_t)d[i] << 16;
		if (i + 1 < d.size()) n |= (uint32_t)d[i + 1] << 8;
		if (i + 2 < d.size()) n |= (uint32_t)d[i + 2];
		o += TR_B64[(n >> 18) & 63];
		o += TR_B64[(n >> 12) & 63];
		o += (i + 1 < d.size()) ? TR_B64[(n >> 6) & 63] : '=';
		o += (i + 2 < d.size()) ? TR_B64[n & 63] : '=';
	}
	return o;
}

static std::vector<uint8_t> trDecode(const std::string& s) {
	int8_t rev[256];
	std::fill(rev, rev + 256, (int8_t)-1);
	for (int i = 0; i < 64; i++) rev[(unsigned char)TR_B64[i]] = (int8_t)i;
	std::vector<uint8_t> o;
	o.reserve(s.size() / 4 * 3);
	uint32_t n = 0; int bits = 0;
	for (char ch : s) {
		int8_t v = rev[(unsigned char)ch];
		if (v < 0) continue;
		n = (n << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) { bits -= 8; o.push_back((uint8_t)((n >> bits) & 0xFF)); }
	}
	return o;
}

// ─────────────────────────────────────────────────────────────────────────────

struct Trace : Module {
	enum ParamId {
		SPEED_PARAM, LENGTH_PARAM, SLEW_PARAM,
		INK_PARAM, LEAK_PARAM, FLAT_PARAM,
		RUN_PARAM, DIR_PARAM, RESET_PARAM,
		BRUSH_PARAM,                          // 4 lane selects, then erase
		PARAMS_LEN = BRUSH_PARAM + 5
	};
	enum InputId {
		LANE_INPUT,                           // 4
		WRITE_INPUT = LANE_INPUT + TR_LANES,
		CLOCK_INPUT, BAR_INPUT,
		SPEED_INPUT, SLEW_INPUT, INK_INPUT,
		RUN_INPUT, DIR_INPUT, RESET_INPUT,
		OFFSET_INPUT,                         // 4, one per lane
		THICK_INPUT = OFFSET_INPUT + TR_LANES,// 4, one per lane
		INPUTS_LEN = THICK_INPUT + TR_LANES
	};
	enum OutputId {
		VALUE_OUTPUT,                                  // 4
		INK_OUTPUT  = VALUE_OUTPUT + TR_LANES,         // 4
		TRIG_OUTPUT = INK_OUTPUT + TR_LANES,           // 4
		LOOP_OUTPUT = TRIG_OUTPUT + TR_LANES,
		OUTPUTS_LEN
	};
	enum LightId {
		RUN_LIGHT, DIR_LIGHT,
		BRUSH_LIGHT,                                   // 5
		LIGHTS_LEN = BRUSH_LIGHT + 5
	};

	// ── the paper ───────────────────────────────────────────────────────────
	std::vector<float> pv[TR_LANES];   // value, volts
	std::vector<float> pk[TR_LANES];   // ink, 0..1
	int cells = 4000;
	double paperPos = 0.0;

	struct Lane {
		// brush
		float target = 0.f, pos = 0.f;
		bool  active = false, holding = false;
		double woff = 0.0;                 // write offset from NOW, in cells
		double lastCell = 0.0; bool hasLast = false;
		int   lastIdx = -1;                // last cell written, for per-cell work
		float writeFrom = 0.f;
		int   edgeIn = 0, edgeOut = -1;    // stroke taper, in cells; -1 = not tapering
		float edgeTo = 0.f, edgeInk = 0.f; // what the paper already held here
		// head
		float offset = 0.f;                // 0..1 of the paper, the knob
		float offsetEff = 0.f;             // after CV: what the head actually reads
		int   lastReadIdx = -1;
		bool  drawn = false;               // has anything ever been written here
		// settings
		int   range = 0;                   // 0 bipolar +/-5V, 1 unipolar 0..10V
		int   readMode = 0;                // 0 smooth (interpolate), 1 stepped
		int   quant = 0;                   // index into TR_QUANTSTEPS
		int   cond  = TC_ANY;
		// state
		int   orient = 0;
		int   lastQ = -100000;
		float outVal = 0.f, outInk = 0.f;
		float trigLight = 0.f;
		dsp::PulseGenerator trig;
	};
	Lane lane[TR_LANES];

	// ── transport ───────────────────────────────────────────────────────────
	bool running = true;
	bool dirFlip = false;
	dsp::SchmittTrigger clockTrig, barTrig, resetTrigIn, resetBtn, dirBtn;
	dsp::SchmittTrigger brushBtn[5];
	dsp::PulseGenerator loopPulse;
	double samplesPerBar = 0.0, samplesPerClock = 0.0;
	double sinceBar = 0.0, sinceClock = 0.0;
	bool haveBar = false, haveClock = false;
	bool barSeen = false, clockSeen = false;   // an interval needs two edges
	int clocksPerBar = 16;
	int barCount = 0;                          // which bar of the loop we are in

	// ── brush selection ─────────────────────────────────────────────────────
	int  brushLane = 0;
	bool eraseMode = false;
	int  scanCell = -1;              // the crossing scan's last visited cell

	// ── UI handshake ────────────────────────────────────────────────────────
	// The widget owns the mouse, the module owns the paper. One brush, so one
	// set of these rather than one per lane.
	// The brush's position has to cross as ONE value. It was four independent
	// relaxed atomics, which is a data race by construction: the audio thread
	// could observe any mixture of old and new. `down` arriving ahead of the
	// coordinates activates the brush at the PREVIOUS stroke's position and
	// writes a cell there; a new value with a stale offset writes the right
	// voltage in the wrong place. Both are single-cell spikes, both sporadic,
	// and neither is reproducible on purpose -- which is exactly how they
	// presented. A single-threaded harness cannot see this at all.
	//
	// Seqlock: odd generation means an update is in flight, so the reader
	// keeps the last good one rather than reading a torn pair.
	struct UiBrush { float val = 0.f; float off = 0.f; bool down = false; };
	std::atomic<uint32_t> uiGen{0};
	UiBrush uiSlot;
	UiBrush uiCur;                       // last consistent read, audio side
	uint32_t uiCurGen = 0;

	void uiPublish(const UiBrush& b) {                       // UI thread
		uint32_t g = uiGen.load(std::memory_order_relaxed);
		uiGen.store(g + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		uiSlot = b;
		std::atomic_thread_fence(std::memory_order_release);
		uiGen.store(g + 2, std::memory_order_release);
	}
	bool uiRead(UiBrush& out, uint32_t& gen) {               // audio thread
		uint32_t g1 = uiGen.load(std::memory_order_acquire);
		if (g1 & 1u) return false;
		out = uiSlot;
		std::atomic_thread_fence(std::memory_order_acquire);
		if (uiGen.load(std::memory_order_relaxed) != g1) return false;
		gen = g1;
		return true;
	}

	// GESTURE RECONSTRUCTION. The mouse is sampled once per DISPLAY FRAME, so
	// the raw target is a staircase: held flat for a frame, then stepped. At
	// 30fps that is a 33ms plateau followed by a vertical edge, which is
	// exactly what the drawn line looked like — blocky, and nothing to do with
	// the paper's own resolution. Anything faster than the frame rate is not
	// information, so the honest fix is to reconstruct the gesture by
	// interpolating between mouse samples rather than holding and jumping.
	// This is not slew: SLEW is then applied on top of the reconstruction.
	uint32_t uiLastSeq = 0;
	double uiRampLen = 1.0, uiRampPos = 1.0, uiSinceSeq = 0.0;
	float  uiValFrom = 0.f, uiValTo = 0.f;
	double uiOffFrom = 0.0, uiOffTo = 0.0;
	bool   uiWasDown = false;

	// ── display mirrors ─────────────────────────────────────────────────────
	// Read by the widget, so the screen reflects CV as well as the knobs.
	float dispSpeed = 1.f, dispSlew = 0.f, dispFlat = 0.05f, dispInk = 0.5f;
	bool  dispClocked = false;
	bool  dispRev = false;
	int   dispBars = 4;
	float dispLenSec = 4.f;

	struct SpeedQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float v = getValue();
			if (std::fabs(v) < 0.02f) return "stopped";
			return string::f("%s%.2fx", v < 0 ? "reverse " : "", std::fabs(v));
		}
	};
	struct LengthQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			Trace* t = dynamic_cast<Trace*>(module);
			float v = getValue();
			if (t && t->dispClocked) {
				int i = clamp((int)std::round(v * (TR_NLENBARS - 1)), 0, TR_NLENBARS - 1);
				int b = (int)TR_LENBARS[i];
				return string::f("%d bar%s", b, b == 1 ? "" : "s");
			}
			float s = 1.f + v * v * 59.f;
			return s < 10.f ? string::f("%.2f s", s) : string::f("%.1f s", s);
		}
	};
	// Rate limited, not time limited. "Arrives by the end of the bar" is
	// ambiguous, and constant-time-per-interval is the common synth portamento
	// and the wrong answer here for the same reason it is wrong in Slide: a
	// hand crossing a distance moves at roughly constant speed. So this is the
	// time to cross the FULL range, and a partial move takes proportionally
	// less.
	struct SlewQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			Trace* t = dynamic_cast<Trace*>(module);
			float v = getValue();
			if (t && t->dispClocked) {
				int i = clamp((int)std::round(v * (TR_NSLEWBARS - 1)), 0, TR_NSLEWBARS - 1);
				float b = TR_SLEWBARS[i];
				if (b <= 0.f) return "instant";
				if (b < 1.f)  return string::f("1/%d bar", (int)std::round(1.f / b));
				return string::f("%g bars", b);
			}
			float s = v * v * 30.f;
			if (s < 0.002f) return "instant";
			return s < 1.f ? string::f("%.0f ms", s * 1000.f) : string::f("%.2f s", s);
		}
	};
	// The threshold that decides whether a drawn line is a handful of events or
	// a swarm of them, so it is on the panel rather than in a menu.
	struct FlatQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float mv = getValue() * getValue() * 2000.f;
			return mv < 1.f ? "any motion" : string::f("%.0f mV", mv);
		}
	};
	// How much ink a READ head lifts each time it passes a cell. Stated as the
	// number of passes it takes to clear a full cell, which is what you are
	// actually choosing.
	struct LeakQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float v = getValue();
			if (v < 0.005f) return "never fades";
			float n = 1.f / (v * v * 0.5f);
			return n < 10.f ? string::f("%.1f passes to clear", n)
			                : string::f("%.0f passes to clear", n);
		}
	};

	Trace() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<SpeedQuantity>(SPEED_PARAM, -4.f, 4.f, 1.f, "Speed");
		configParam<LengthQuantity>(LENGTH_PARAM, 0.f, 1.f, 0.226f, "Paper length");
		configParam<SlewQuantity>(SLEW_PARAM, 0.f, 1.f, 0.f, "Slew");
		configParam(INK_PARAM, 0.f, 1.f, 0.5f, "Ink laid per pass", "%", 0.f, 100.f);
		configParam<LeakQuantity>(LEAK_PARAM, 0.f, 1.f, 0.2f, "Ink lifted per pass");
		configParam<FlatQuantity>(FLAT_PARAM, 0.f, 1.f, 0.16f, "Flat threshold");

		configSwitch(RUN_PARAM, 0.f, 1.f, 1.f, "Run", {"Stopped", "Running"});
		configButton(DIR_PARAM, "Reverse");
		configButton(RESET_PARAM, "Reset");
		for (int i = 0; i < TR_LANES; i++)
			configButton(BRUSH_PARAM + i, string::f("Brush: lane %d", i + 1));
		configButton(BRUSH_PARAM + 4, "Brush: erase");

		for (int i = 0; i < TR_LANES; i++) {
			configInput(LANE_INPUT + i, string::f("Lane %d draw CV", i + 1));
			configInput(OFFSET_INPUT + i, string::f("Lane %d head offset CV", i + 1));
			configInput(THICK_INPUT + i, string::f("Lane %d thickness / velocity", i + 1));
			configOutput(VALUE_OUTPUT + i, string::f("Lane %d value", i + 1));
			configOutput(INK_OUTPUT + i,   string::f("Lane %d ink", i + 1));
			configOutput(TRIG_OUTPUT + i,  string::f("Lane %d trigger", i + 1));
		}
		configInput(WRITE_INPUT, "Write gate");
		configInput(CLOCK_INPUT, "Clock");
		configInput(BAR_INPUT, "Bar");
		configInput(SPEED_INPUT, "Speed CV");
		configInput(SLEW_INPUT, "Slew CV");
		configInput(INK_INPUT, "Ink CV");
		configInput(RUN_INPUT, "Run gate");
		configInput(DIR_INPUT, "Reverse gate");
		configInput(RESET_INPUT, "Reset");
		configOutput(LOOP_OUTPUT, "Loop");

		for (int i = 0; i < TR_LANES; i++) {
			pv[i].assign(TR_MAXCELLS, 0.f);
			pk[i].assign(TR_MAXCELLS, 0.f);
			lane[i].offset = 0.f;          // all four read at NOW until moved
		}
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		for (int i = 0; i < TR_LANES; i++) {
			std::fill(pv[i].begin(), pv[i].end(), 0.f);
			std::fill(pk[i].begin(), pk[i].end(), 0.f);
			lane[i] = Lane();
		}
		paperPos = 0.0;
		running = true; dirFlip = false;
		brushLane = 0; eraseMode = false; scanCell = -1;
		haveBar = haveClock = false;
		barSeen = clockSeen = false;
		barCount = 0;
	}

	// Read a value off the paper with wrap and linear interpolation.
	float paperAt(int L, double c) {
		double f = std::floor(c);
		int i0 = trWrap((int)f, cells);
		int i1 = trWrap(i0 + 1, cells);
		float t = (float)(c - f);
		return pv[L][i0] + (pv[L][i1] - pv[L][i0]) * t;
	}

	float quantize(const Lane& L, float v) const {
		int q = TR_QUANTSTEPS[clamp(L.quant, 0, TR_NQUANT - 1)];
		if (q == 0) return v;
		if (q < 0) return std::round(v * 12.f) / 12.f;          // semitones
		float lo = L.range ? 0.f : -5.f, hi = L.range ? 10.f : 5.f;
		float t = clamp((v - lo) / (hi - lo), 0.f, 1.f);
		return lo + std::round(t * (q - 1)) / (float)(q - 1) * (hi - lo);
	}

	void process(const ProcessArgs& args) override {
		// ── buttons ─────────────────────────────────────────────────────────
		if (dirBtn.process(params[DIR_PARAM].getValue(), 0.1f, 0.9f))
			dirFlip = !dirFlip;
		running = params[RUN_PARAM].getValue() > 0.5f;
		if (inputs[RUN_INPUT].isConnected())
			running = inputs[RUN_INPUT].getVoltage() >= 1.f;

		for (int i = 0; i < 5; i++) {
			if (!brushBtn[i].process(params[BRUSH_PARAM + i].getValue(), 0.1f, 0.9f))
				continue;
			if (i < TR_LANES) brushLane = i;
			else eraseMode = !eraseMode;
		}

		bool doReset = resetBtn.process(params[RESET_PARAM].getValue(), 0.1f, 0.9f);
		if (resetTrigIn.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f))
			doReset = true;

		// ── clock and bar ───────────────────────────────────────────────────
		// AN INTERVAL NEEDS TWO EDGES. `sinceBar` counts from the moment the
		// module is created and is only reset by a BAR, so treating the FIRST
		// edge as a measurement measures how long the module sat there before
		// anyone patched it. Thirty seconds of patching became a thirty-second
		// bar; the paper then crawled at a fifteenth of the right speed, and
		// the phase lock below rounded it to the nearest bar boundary, which
		// was always zero. That is what made BAR behave like a reset, and the
		// 0.3 smoothing took ten bars to climb out of it, so it looked
		// permanent rather than transient.
		sinceBar += 1.0; sinceClock += 1.0;
		if (inputs[CLOCK_INPUT].isConnected() &&
		    clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f)) {
			if (clockSeen && sinceClock > 4.0 && sinceClock < args.sampleRate * 8.0) {
				// Take a large change whole. Smoothing towards a new tempo is
				// right for jitter and wrong for a tempo change.
				if (!haveClock || sinceClock > samplesPerClock * 1.5
				               || sinceClock < samplesPerClock * 0.67)
					samplesPerClock = sinceClock;
				else
					samplesPerClock += (sinceClock - samplesPerClock) * 0.3;
				haveClock = true;
			}
			clockSeen = true;
			sinceClock = 0.0;
		}
		bool barEdge = false;
		if (inputs[BAR_INPUT].isConnected() &&
		    barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 2.f)) {
			if (barSeen && sinceBar > 4.0 && sinceBar < args.sampleRate * 30.0) {
				if (!haveBar || sinceBar > samplesPerBar * 1.5
				             || sinceBar < samplesPerBar * 0.67)
					samplesPerBar = sinceBar;
				else
					samplesPerBar += (sinceBar - samplesPerBar) * 0.3;
				haveBar = true;
				barEdge = true;
			}
			barSeen = true;
			sinceBar = 0.0;
		}
		if (!inputs[BAR_INPUT].isConnected())   { haveBar = false;   barSeen = false; }
		if (!inputs[CLOCK_INPUT].isConnected()) { haveClock = false; clockSeen = false; }

		double spb = 0.0;
		if (haveBar) spb = samplesPerBar;
		else if (haveClock) spb = samplesPerClock * clocksPerBar;
		bool clocked = spb > 0.0;
		dispClocked = clocked;

		// ── paper length ────────────────────────────────────────────────────
		float lenKnob = params[LENGTH_PARAM].getValue();
		int bars = 4;
		float lenSec = 4.f;
		int wantCells;
		if (clocked) {
			int i = clamp((int)std::round(lenKnob * (TR_NLENBARS - 1)), 0, TR_NLENBARS - 1);
			bars = (int)TR_LENBARS[i];
			wantCells = (int)(bars * TR_CELLS_PER_BAR);
		} else {
			lenSec = 1.f + lenKnob * lenKnob * 59.f;
			wantCells = (int)(lenSec * TR_CELLS_PER_SEC);
		}
		dispBars = bars; dispLenSec = lenSec;
		int newCells = clamp(wantCells, TR_MINCELLS, TR_MAXCELLS);
		if (newCells != cells) {
			// Shortening truncates and lengthening reveals what was already
			// there, which is what a tape loop does. No resampling: the drawing
			// lives in cells, and moving it would be a different module.
			cells = newCells;
			if (paperPos >= cells) paperPos = std::fmod(paperPos, (double)cells);
			for (int i = 0; i < TR_LANES; i++) {
				lane[i].hasLast = false;
				lane[i].lastReadIdx = -1;
			}
		}

		// ── transport ───────────────────────────────────────────────────────
		float speed = params[SPEED_PARAM].getValue();
		// 0.8/V, so +/-5V covers the knob's whole +/-4 range. At 0.4/V it took
		// +/-10V to reach the ends, and -- with the knob at its default of 1 --
		// about -2.5V landed exactly on zero, which is a stopped transport
		// sitting in the middle of the CV's travel.
		if (inputs[SPEED_INPUT].isConnected())
			speed += inputs[SPEED_INPUT].getVoltage() * 0.8f;
		speed = clamp(speed, -8.f, 8.f);
		dispSpeed = speed;

		bool reversed = dirFlip;
		if (inputs[DIR_INPUT].isConnected() && inputs[DIR_INPUT].getVoltage() >= 1.f)
			reversed = !reversed;
		if (speed < 0.f) { reversed = !reversed; }
		double mag = std::fabs((double)speed);
		dispRev = reversed;

		double cps;
		if (clocked) cps = (double)cells / (bars * spb);
		else         cps = TR_CELLS_PER_SEC / args.sampleRate;
		cps *= mag;
		if (reversed) cps = -cps;
		if (!running) cps = 0.0;

		if (doReset) {
			paperPos = 0.0;
			barCount = 0;              // the next BAR starts the loop again
			for (int i = 0; i < TR_LANES; i++) {
				lane[i].hasLast = false;
				lane[i].lastReadIdx = -1;
			}
			loopPulse.trigger(1e-3f);
		}

		double prevPaper = paperPos;
		paperPos += cps;
		if (paperPos >= cells) { paperPos -= cells; loopPulse.trigger(1e-3f); }
		else if (paperPos < 0.0) { paperPos += cells; loopPulse.trigger(1e-3f); }
		(void)prevPaper;

		// Phase lock, by COUNTING bars rather than rounding to the nearest
		// boundary. Rounding decides where the paper is from where it already
		// got to, so a rate estimate that is even slightly out drags it to the
		// same boundary every bar instead of moving it on; counting says which
		// bar of the loop this is and puts the paper there, which is right
		// whatever the estimate is doing.
		if (barEdge && clocked && running) {
			if (barCount >= bars) barCount = 0;
			paperPos = barCount * ((double)cells / bars);
			barCount = (barCount + 1) % std::max(bars, 1);
		}

		// ── slew, ink, flat ─────────────────────────────────────────────────
		float slewKnob = params[SLEW_PARAM].getValue();
		if (inputs[SLEW_INPUT].isConnected())
			slewKnob = clamp(slewKnob + inputs[SLEW_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float slewSec;
		if (clocked) {
			int i = clamp((int)std::round(slewKnob * (TR_NSLEWBARS - 1)), 0, TR_NSLEWBARS - 1);
			slewSec = TR_SLEWBARS[i] * (float)(spb / args.sampleRate);
		} else {
			slewSec = slewKnob * slewKnob * 30.f;
		}
		dispSlew = slewSec;

		float inkAmt = params[INK_PARAM].getValue();
		if (inputs[INK_INPUT].isConnected())
			inkAmt = clamp(inkAmt + inputs[INK_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		dispInk = inkAmt;

		float flatV = params[FLAT_PARAM].getValue();
		flatV = flatV * flatV * 2.f;
		dispFlat = flatV;

		float leakKnob = params[LEAK_PARAM].getValue();
		float inkWear = leakKnob * leakKnob * 0.5f;     // lifted per read pass

		// Reverse used to forbid writing, on the theory that a brush working
		// against the paper retraces over what it just laid down. In practice
		// the stroke stays coherent -- it is simply drawn backwards -- and the
		// span handling and the edge tapers cope with it, so the restriction
		// was buying nothing and costing a legitimate gesture.
		bool writeOK = true;
		bool writeGate = !inputs[WRITE_INPUT].isConnected()
		              || inputs[WRITE_INPUT].getVoltage() >= 1.f;

		{
			UiBrush ub; uint32_t g;
			if (uiRead(ub, g)) { uiCur = ub; uiCurGen = g; }
		}
		bool mouseDown = uiCur.down;
		{
			uint32_t seq = uiCurGen;
			float rawVal = uiCur.val;
			float rawOff = uiCur.off;
			if (mouseDown && !uiWasDown) {
				// a fresh press starts where it is, with nothing to ramp from
				uiValFrom = uiValTo = rawVal;
				uiOffFrom = uiOffTo = rawOff;
				uiRampPos = uiRampLen = 1.0;
				uiSinceSeq = 0.0;
				uiLastSeq = seq;
			} else if (seq != uiLastSeq) {
				// RETARGET FROM WHERE THE BRUSH ACTUALLY IS, never from the
				// previous target. Frame times are not regular -- Rack's vary
				// by a factor of two under load -- so the ramp is often still
				// in flight when the next mouse sample lands. Starting the new
				// one at the old TARGET teleports the brush to a position it
				// never reached, once per frame, and those jumps are the
				// evenly spaced spikes. Compute the current position with the
				// OLD length before overwriting it.
				double tNow = std::min(uiRampPos / std::max(uiRampLen, 1.0), 1.0);
				uiValFrom += (uiValTo - uiValFrom) * (float)tNow;
				uiOffFrom += (uiOffTo - uiOffFrom) * tNow;
				uiValTo = rawVal;
				uiOffTo = rawOff;
				// One frame of samples, measured rather than assumed, so it
				// tracks whatever rate the display is actually running at.
				// Capped short: past about a frame this stops reconstructing a
				// gesture and just lags behind one.
				uiRampLen = std::max(1.0, std::min(uiSinceSeq, (double)args.sampleRate * 0.05));
				uiRampPos = 0.0;
				uiLastSeq = seq;
				uiSinceSeq = 0.0;
			}
			uiWasDown = mouseDown;
			uiSinceSeq += 1.0;
			uiRampPos += 1.0;
		}
		float t = (float)std::min(uiRampPos / std::max(uiRampLen, 1.0), 1.0);
		float mouseVal = uiValFrom + (uiValTo - uiValFrom) * t;
		double mouseOff = uiOffFrom + (uiOffTo - uiOffFrom) * t;

		// The slope window scales with the flat threshold rather than being an
		// independent control: a wide window and a low threshold are the same
		// idea stated twice.
		double slopeW = (double)cells * (0.002 + 0.02 * std::sqrt(flatV / 2.f));
		slopeW = std::max(2.0, std::min(slopeW, cells * 0.2));

		for (int i = 0; i < TR_LANES; i++) {
			Lane& L = lane[i];
			float lo = L.range ? 0.f : -5.f, hi = L.range ? 10.f : 5.f;

			// ── where the brush is being told to go ─────────────────────────
			bool hold = false;
			if (mouseDown && brushLane == i) {
				L.target = clamp(mouseVal, lo, hi);
				L.woff = mouseOff;
				hold = true;
			} else if (inputs[LANE_INPUT + i].isConnected() && writeGate) {
				L.target = clamp(inputs[LANE_INPUT + i].getVoltage(), lo, hi);
				L.woff = 0.0;
				hold = true;
			}
			if (hold) {
				// A press that lands while the last stroke is still tapering
				// out is a NEW stroke, not a continuation. Treating it as one
				// left `lastCell` pointing at the old position, so the fill
				// drew a line all the way across the paper to the new one.
				if (!L.active || L.edgeOut >= 0) {
					L.active = true; L.edgeIn = TR_EDGE; L.hasLast = false;
				}
				L.holding = true;
				L.edgeOut = -1;
			} else {
				L.holding = false;
			}

			// ── slew ───────────────────────────────────────────────────────
			float from = L.pos;
			float step = (slewSec <= 0.0005f) ? 1e9f
			           : (hi - lo) * args.sampleTime / slewSec;
			float d = L.target - L.pos;
			L.pos += clamp(d, -step, step);

			// The brush stays down after release until it reaches its target,
			// so a long stroke finishes. Without this a lane touched once
			// would keep painting a flat line over the whole loop. Once it has
			// arrived, it tapers out rather than lifting on the spot.
			if (!L.holding && L.active && L.edgeOut < 0
			 && std::fabs(L.target - L.pos) < 1e-4f)
				L.edgeOut = TR_EDGE;
			if (!L.holding && L.edgeOut == 0) {
				L.active = false;
				L.hasLast = false;
				L.edgeOut = -1;
			}

			// ── write ──────────────────────────────────────────────────────
			// Erase is a property of the STROKE, not of the moment: gating it
			// on `holding` let the exit taper escape into the drawing path.
			bool erasingHere = eraseMode && brushLane == i;
			if (L.active && writeOK) {
				double cur = paperPos + L.woff;
				if (!L.hasLast) { L.lastCell = cur; L.writeFrom = from; L.hasLast = true; }
				// SHORTEST WAY ROUND. paperPos wraps at the seam, so a brush
				// held near the edge of the window saw `cur` jump by a whole
				// paper length once per revolution and repainted the ENTIRE
				// loop with a ramp on that one sample. That was the spike, and
				// it landed in the outputs because it was really written.
				double span = cur - L.lastCell;
				span -= std::round(span / (double)cells) * (double)cells;
				// A mouse dragged fast moves the write point by more than one
				// cell per frame, so the stroke has to be filled in rather
				// than dotted.
				//
				// CEIL, not truncate. With `(int)|span|` the walk takes steps
				// of span/n which are LONGER than a cell whenever the span is
				// not an exact integer, so it skips: a span of 1.9 writes
				// cells 100 and 102 and leaves 101 holding whatever was drawn
				// there before. Those survivors are one cell wide and can sit
				// anywhere in range, which is why they read as thin deep
				// spikes, and why they cluster — one fast gesture skips
				// several in a row.
				int n = (int)std::min(std::ceil(std::fabs(span)), 4096.0);
				for (int k = 0; k <= n; k++) {
					double t = (n == 0) ? 1.0 : (double)k / n;
					int idx = trWrap((int)std::floor(L.lastCell + span * t), cells);
					float v = L.writeFrom + (L.pos - L.writeFrom) * (float)t;
					// Per-CELL work, not per-sample: at 48 samples to a cell
					// the latter would be 48x too heavy, and the tapers would
					// be over before the brush had moved.
					bool newCell = (idx != L.lastIdx);
					// STROKE EDGES. A brush that lands and lifts at full value
					// leaves a step at each end, and a click that never became
					// a drag leaves a rectangular notch a few tens of
					// milliseconds wide — which reads as a spike in the output
					// and is what it was. So the stroke is joined to whatever
					// it lands on and blended back into whatever it lifts off.
					// This is deliberately NOT the SLEW curve, for the reason
					// Slice keeps its splice fade separate from SHAPE: hiding
					// a discontinuity is a different job from shaping a gesture.
					//
					// What is already on this cell has to be captured ONCE, as
					// the brush enters it. Reading it live reads back what this
					// same stroke wrote a sample ago, since 48 samples land on
					// every cell, so the blend converges on the brush's own
					// value instead of on the paper. That collapsed a 24-cell
					// taper into three and left the step it was meant to hide.
					if (newCell) { L.edgeTo = pv[i][idx]; L.edgeInk = pk[i][idx]; }
					// ONE weight for both ends: 0 where the stroke meets the
					// paper, 1 in its body. Entry and exit used to be separate
					// branches, and erase only special-cased the branch where
					// the brush was HELD -- so the exit taper fell through to
					// the drawing path and painted the brush's value at the
					// end of every erase stroke. That is where the leftover
					// spikes came from.
					float w = 1.f;
					if (L.edgeIn > 0) {
						w = 1.f - (float)L.edgeIn / (float)TR_EDGE;
						if (newCell) L.edgeIn--;
					} else if (L.edgeOut >= 0) {
						w = (float)L.edgeOut / (float)TR_EDGE;
						if (newCell && L.edgeOut > 0) L.edgeOut--;
					}
					if (erasingHere) v = 0.f;
					v = L.edgeTo + (v - L.edgeTo) * w;
					pv[i][idx] = v;
					L.drawn = true;
					if (erasingHere) {
						pk[i][idx] = L.edgeInk * (1.f - w);
					} else if (inputs[THICK_INPUT + i].isConnected()) {
						// An explicit thickness SETS the ink; the INK knob
						// accumulates it. Recording a velocity contour wants
						// the contour itself, not a running total of it.
						float tgt = clamp(inputs[THICK_INPUT + i].getVoltage() * 0.1f,
						                  0.f, 1.f);
						pk[i][idx] = L.edgeInk + (tgt - L.edgeInk) * w;
					} else if (newCell) {
						// A flat amount per cell. Dwell weighting used to
						// shape this -- a brush dragged sideways lays down
						// less per unit area -- but it only ever tracked how
						// steady your hand was. Crossings do the shaping now.
						pk[i][idx] = clamp(pk[i][idx] + inkAmt * 0.35f * w, 0.f, 1.f);
					}
					if (newCell) L.lastIdx = idx;
				}
				L.lastCell = cur;
				L.writeFrom = L.pos;
			} else {
				L.hasLast = false;
				L.lastIdx = -1;
			}

			// ── read ───────────────────────────────────────────────────────
			// INTERPOLATE BACKWARDS, from the cell the head has just left to
			// the one it is on. Reading FORWARD reads a cell the brush has not
			// reached yet, and when the head and the brush are on the same
			// cell -- which is the default, and is always the case while a CV
			// is being recorded -- that cell still holds the PREVIOUS
			// revolution. The output then ramps from the value just written
			// toward a stale one and snaps back, once per cell: a 1kHz
			// sawtooth riding on the signal, which reads as a thick fuzzy
			// band on a scope. Backwards, both cells are always written.
			// The offset WRAPS rather than clamping, because the paper is a
			// loop: a head driven past the end comes round the front, which is
			// the whole point of being able to sweep one.
			float off = L.offset;
			if (inputs[OFFSET_INPUT + i].isConnected())
				off += inputs[OFFSET_INPUT + i].getVoltage() * 0.1f;   // 10V = one loop
			off -= std::floor(off);
			L.offsetEff = off;

			double rc = paperPos + (double)off * cells;
			double rf = std::floor(rc);
			int r1 = trWrap((int)rf, cells);         // the cell the head is on
			int r0 = trWrap(r1 - 1, cells);          // the one it just left
			float rt = (float)(rc - rf);

			// Leak is applied as the head crosses a cell, which is exactly
			// once per revolution per cell and costs nothing, where sweeping
			// the whole ring on the wrap would spike one sample's work.
			if (inkWear > 0.f && r1 != L.lastReadIdx) {
				pk[i][r1] = std::max(0.f, pk[i][r1] - inkWear);
				L.lastReadIdx = r1;
			}

			// The paper has grain: cells are a millisecond apart at 1x.
			// Interpolating between them is right for a drawn curve and wrong
			// for a gate, which comes out as a 0.8ms ramp rather than an edge
			// -- measured, and nothing to do with SLEW, which really is
			// instant at zero. So it is a per-lane choice rather than a
			// constant.
			float raw, ink;
			if (L.readMode) {
				raw = pv[i][r1];
				ink = pk[i][r1];
			} else {
				raw = pv[i][r0] + (pv[i][r1] - pv[i][r0]) * rt;
				ink = pk[i][r0] + (pk[i][r1] - pk[i][r0]) * rt;
			}
			float val = clamp(quantize(L, raw), lo, hi);

			// ── orientation ────────────────────────────────────────────────
			// A CENTRED difference, because the future is already drawn on the
			// paper so lookahead is free, and it puts the event on the turn
			// rather than a window late. A causal difference is the obvious
			// implementation and the worse one.
			float slope = paperAt(i, rc + slopeW) - paperAt(i, rc - slopeW);
			if (reversed) slope = -slope;

			int o = L.orient, no = o;
			// Hysteresis is not optional: no hand-drawn line is ever exactly
			// flat, and a bare threshold chatters at the boundary and emits
			// hundreds of triggers a second on recorded CV.
			if (o == 0) {
				if (slope >  flatV * 1.6f) no =  1;
				else if (slope < -flatV * 1.6f) no = -1;
			} else {
				if (std::fabs(slope) < flatV) no = 0;
				else no = (slope > 0.f) ? 1 : -1;
			}

			int qi = (int)std::round(val * 1000.f);
			bool stepped = (TR_QUANTSTEPS[clamp(L.quant, 0, TR_NQUANT - 1)] != 0)
			            && (qi != L.lastQ) && (L.lastQ != -100000);
			L.lastQ = qi;

			bool fire = false;
			switch (L.cond) {
				case TC_ANY:    fire = (no != o); break;
				case TC_UP:     fire = (no ==  1 && o !=  1); break;
				case TC_DOWN:   fire = (no == -1 && o != -1); break;
				case TC_FLAT:   fire = (no ==  0 && o !=  0); break;
				case TC_UNFLAT: fire = (no !=  0 && o ==  0); break;
				case TC_STEP:   fire = stepped; break;
				default: break;
			}
			L.orient = no;
			if (fire) L.trig.trigger(1e-3f);

			float trigOut;
			if (trCondIsGate(L.cond)) {
				bool high = (L.cond == TC_GATE_UP   && no ==  1)
				         || (L.cond == TC_GATE_DOWN && no == -1)
				         || (L.cond == TC_GATE_FLAT && no ==  0);
				trigOut = high ? 10.f : 0.f;
			} else {
				trigOut = L.trig.process(args.sampleTime) ? 10.f : 0.f;
			}
			L.trigLight = std::max(L.trigLight - args.sampleTime * 6.f,
			                       trigOut > 1.f ? 1.f : 0.f);

			L.outVal = val;
			L.outInk = ink * 10.f;
			outputs[VALUE_OUTPUT + i].setVoltage(val);
			outputs[INK_OUTPUT + i].setVoltage(L.outInk);
			outputs[TRIG_OUTPUT + i].setVoltage(trigOut);
		}

		// ── ink pools where the lines cross ─────────────────────────────────
		// A crossing is a property of the PAPER, not of any one head, so it is
		// scanned once per cell as the transport passes over it rather than
		// per lane. Six pairs at a thousand cells a second is nothing, and it
		// means ink gathers at the intersections and thins out everywhere
		// else, which is the shape the read heads then wear away.
		{
			int sc = trWrap((int)std::floor(paperPos), cells);
			if (sc != scanCell) {
				int prev = (scanCell >= 0) ? scanCell : trWrap(sc - 1, cells);
				for (int a = 0; a < TR_LANES; a++) {
					if (!lane[a].drawn) continue;
					for (int b = a + 1; b < TR_LANES; b++) {
						// An EMPTY lane is not a line. Two undrawn lanes both
						// sit at 0V, so every lane swinging through zero
						// "crossed" both of them: ink pooled onto lanes nobody
						// had touched, which is the row of blobs on the centre
						// line, and the lane that actually moved got the
						// deposit two or three times over.
						if (!lane[b].drawn) continue;
						float d0 = pv[a][prev] - pv[b][prev];
						float d1 = pv[a][sc]   - pv[b][sc];
						if (d0 * d1 > 0.f) continue;          // no crossing
						if (d0 == 0.f && d1 == 0.f) continue; // two flat lines
						// A POOL, not a point. Depositing into the single cell
						// where the lines meet puts all the ink inside one
						// screen column at most lengths, so the stroke grew a
						// one-column barb and its width jumped as the paper
						// scrolled that cell across a column boundary. Ink
						// that pools should spread, which is also what the
						// word means.
						int rad = (int)std::max(24.0, cells / 200.0);
						for (int d = -rad; d <= rad; d++) {
							float t = (float)d / (float)rad;
							float fall = 0.5f * (1.f + std::cos(t * (float)M_PI));
							float add = inkAmt * 0.10f * fall;
							int c = trWrap(sc + d, cells);
							pk[a][c] = clamp(pk[a][c] + add, 0.f, 1.f);
							pk[b][c] = clamp(pk[b][c] + add, 0.f, 1.f);
						}
					}
				}
				scanCell = sc;
			}
		}

		outputs[LOOP_OUTPUT].setVoltage(loopPulse.process(args.sampleTime) ? 10.f : 0.f);

		lights[RUN_LIGHT].setBrightness(running ? 1.f : 0.f);
		lights[DIR_LIGHT].setBrightness(reversed ? 1.f : 0.f);
		for (int i = 0; i < TR_LANES; i++)
			lights[BRUSH_LIGHT + i].setBrightness(brushLane == i ? 1.f : 0.f);
		lights[BRUSH_LIGHT + 4].setBrightness(eraseMode ? 1.f : 0.f);
	}

	// ── persistence ─────────────────────────────────────────────────────────
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "cells", json_integer(cells));
		json_object_set_new(root, "running", json_boolean(running));
		json_object_set_new(root, "dirFlip", json_boolean(dirFlip));
		json_object_set_new(root, "brushLane", json_integer(brushLane));
		json_object_set_new(root, "eraseMode", json_boolean(eraseMode));
		json_object_set_new(root, "clocksPerBar", json_integer(clocksPerBar));

		json_t* ls = json_array();
		for (int i = 0; i < TR_LANES; i++) {
			json_t* o = json_object();
			json_object_set_new(o, "offset", json_real(lane[i].offset));
			json_object_set_new(o, "range", json_integer(lane[i].range));
			json_object_set_new(o, "readMode", json_integer(lane[i].readMode));
			json_object_set_new(o, "drawn", json_boolean(lane[i].drawn));
			json_object_set_new(o, "quant", json_integer(lane[i].quant));
			json_object_set_new(o, "cond", json_integer(lane[i].cond));
			json_array_append_new(ls, o);
		}
		json_object_set_new(root, "lanes", ls);

		// Stride 4: 250Hz is well above anything a hand-drawn CV line holds,
		// and it is the difference between a 180KB patch and a 720KB one.
		int n = (cells + 3) / 4;
		json_t* paper = json_array();
		for (int i = 0; i < TR_LANES; i++) {
			std::vector<uint8_t> buf;
			buf.reserve((size_t)n * 3);
			for (int k = 0; k < n; k++) {
				int idx = std::min(k * 4, cells - 1);
				int16_t q = (int16_t)clamp(pv[i][idx] * 3200.f, -32000.f, 32000.f);
				buf.push_back((uint8_t)(q & 0xFF));
				buf.push_back((uint8_t)((q >> 8) & 0xFF));
				buf.push_back((uint8_t)clamp(pk[i][idx] * 255.f, 0.f, 255.f));
			}
			json_array_append_new(paper, json_string(trEncode(buf).c_str()));
		}
		json_object_set_new(root, "paper", paper);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "cells"))
			cells = clamp((int)json_integer_value(j), TR_MINCELLS, TR_MAXCELLS);
		if (json_t* j = json_object_get(root, "running")) running = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "dirFlip")) dirFlip = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "brushLane"))
			brushLane = clamp((int)json_integer_value(j), 0, TR_LANES - 1);
		if (json_t* j = json_object_get(root, "eraseMode")) eraseMode = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "clocksPerBar"))
			clocksPerBar = clamp((int)json_integer_value(j), 1, 96);

		if (json_t* ls = json_object_get(root, "lanes")) {
			for (int i = 0; i < TR_LANES && i < (int)json_array_size(ls); i++) {
				json_t* o = json_array_get(ls, i);
				if (json_t* j = json_object_get(o, "offset"))
					lane[i].offset = clamp((float)json_real_value(j), 0.f, 1.f);
				if (json_t* j = json_object_get(o, "range"))
					lane[i].range = clamp((int)json_integer_value(j), 0, 1);
				if (json_t* j = json_object_get(o, "readMode"))
					lane[i].readMode = clamp((int)json_integer_value(j), 0, 1);
				if (json_t* j = json_object_get(o, "drawn"))
					lane[i].drawn = json_boolean_value(j);
				if (json_t* j = json_object_get(o, "quant"))
					lane[i].quant = clamp((int)json_integer_value(j), 0, TR_NQUANT - 1);
				if (json_t* j = json_object_get(o, "cond"))
					lane[i].cond = clamp((int)json_integer_value(j), 0, TC_NCOND - 1);
			}
		}

		json_t* paper = json_object_get(root, "paper");
		if (!paper) return;
		int n = (cells + 3) / 4;
		for (int i = 0; i < TR_LANES && i < (int)json_array_size(paper); i++) {
			const char* s = json_string_value(json_array_get(paper, i));
			if (!s) continue;
			std::vector<uint8_t> buf = trDecode(s);
			int have = (int)(buf.size() / 3);
			if (have <= 0) continue;
			for (int k = 0; k < n; k++) {
				int a = std::min(k, have - 1), b = std::min(k + 1, have - 1);
				float va = (float)(int16_t)(buf[a * 3] | (buf[a * 3 + 1] << 8)) / 3200.f;
				float vb = (float)(int16_t)(buf[b * 3] | (buf[b * 3 + 1] << 8)) / 3200.f;
				float ka = buf[a * 3 + 2] / 255.f, kb = buf[b * 3 + 2] / 255.f;
				for (int f = 0; f < 4; f++) {
					int idx = k * 4 + f;
					if (idx >= cells) break;
					float t = f / 4.f;
					pv[i][idx] = va + (vb - va) * t;
					pk[i][idx] = ka + (kb - ka) * t;
				}
			}
		}
	}
};

// =============================================================================
// Display
// =============================================================================

static const float TR_DESIGN_W = 363.f;      // 96mm * 3.783, per screen-style
// ONE x axis, shared by the pane and the lane strips, so a strip's handle sits
// directly beneath its own triangle on the pane. They used to have separate
// mappings -- offset 0 was the far left on a strip and dead centre on the pane
// -- so the two drawings of the same number never lined up. Gutters either side
// carry the lane number and the trigger/orientation indicators.
static const float TR_GX0 = 10.f;
static const float TR_GW  = TR_DESIGN_W - 26.f;
static const NVGcolor TR_LANECOL[TR_LANES] = {
	nvgRGB(0x00, 0x97, 0xDE),   // blue
	nvgRGB(0x3F, 0xBF, 0x6F),   // green
	nvgRGB(0xEC, 0x65, 0x2E),   // orange
	nvgRGB(0x9B, 0x6B, 0xD6),   // purple
};

// The brush buttons light in their own lane's colour, so the selector says
// which line you are about to draw on without needing numbers printed under it.
template <int I, typename TBase = GrayModuleLightWidget>
struct TTrLaneLight : TBase {
	TTrLaneLight() { this->addBaseColor(TR_LANECOL[I]); }
};
typedef TTrLaneLight<0> TrLaneLight0;
typedef TTrLaneLight<1> TrLaneLight1;
typedef TTrLaneLight<2> TrLaneLight2;
typedef TTrLaneLight<3> TrLaneLight3;

// The window IS the loop, with NOW centred and the paper scrolling under it, so
// you always see everything and the seam travels across the screen the way a
// real paper loop's join does.
struct TraceDisplay : OpaqueWidget {
	Trace* module = nullptr;
	std::shared_ptr<Font> font;

	enum DragKind { DRAG_NONE, DRAG_PAPER, DRAG_OFFSET };
	DragKind dragKind = DRAG_NONE;
	int dragLane = 0;
	Vec dragPos;

	// layout, in design units
	float hdrH() const { return 16.f; }
	float stripH() const { return 11.f; }
	float paneY() const { return hdrH() + 3.f; }
	float paneH() const {
		float total = box.size.y / (box.size.x / TR_DESIGN_W);
		return total - paneY() - TR_LANES * stripH() - 4.f;
	}
	float stripY(int i) const { return paneY() + paneH() + 4.f + i * stripH(); }

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawPreview(const DrawArgs& args, float s);
	void onButton(const ButtonEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
	void apply(Vec p);
};

void TraceDisplay::onButton(const ButtonEvent& e) {
	if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
		float s = box.size.x / TR_DESIGN_W;
		float u = e.pos.y / s;
		dragKind = DRAG_NONE;
		for (int i = 0; i < TR_LANES; i++) {
			if (u >= stripY(i) && u < stripY(i) + stripH()) {
				dragKind = DRAG_OFFSET; dragLane = i; break;
			}
		}
		if (dragKind == DRAG_NONE && u >= paneY() && u < paneY() + paneH())
			dragKind = DRAG_PAPER;
		if (dragKind != DRAG_NONE) {
			dragPos = e.pos;
			apply(dragPos);
			e.consume(this);
			return;
		}
	}
	OpaqueWidget::onButton(e);
}

void TraceDisplay::onDragMove(const DragMoveEvent& e) {
	if (dragKind == DRAG_NONE) { OpaqueWidget::onDragMove(e); return; }
	float z = getAbsoluteZoom();
	if (z > 0.f) dragPos = dragPos.plus(e.mouseDelta.div(z));
	apply(dragPos);
}

void TraceDisplay::onDragEnd(const DragEndEvent& e) {
	if (module) {
		Trace::UiBrush b = module->uiSlot;
		b.down = false;
		module->uiPublish(b);
	}
	dragKind = DRAG_NONE;
	OpaqueWidget::onDragEnd(e);
}

void TraceDisplay::apply(Vec p) {
	if (!module) return;
	float s = box.size.x / TR_DESIGN_W;
	float ux = p.x / s, uy = p.y / s;
	if (dragKind == DRAG_OFFSET) {
		// Same mapping as the pane, so the handle lands under the pointer AND
		// under its triangle. NOW is the centre, and it wraps.
		float fx = clamp((ux - TR_GX0) / TR_GW, 0.f, 1.f) - 0.5f;
		if (fx < 0.f) fx += 1.f;
		module->lane[dragLane].offset = fx;
		return;
	}
	if (dragKind != DRAG_PAPER) return;
	int L = clamp(module->brushLane, 0, TR_LANES - 1);
	float lo = module->lane[L].range ? 0.f : -5.f;
	float hi = module->lane[L].range ? 10.f : 5.f;
	float t = clamp((uy - paneY()) / std::max(paneH(), 1.f), 0.f, 1.f);
	float fx = clamp((ux - TR_GX0) / TR_GW, 0.f, 1.f);
	Trace::UiBrush b;
	b.val  = hi - t * (hi - lo);
	b.off  = (fx - 0.5f) * module->cells;
	b.down = true;
	module->uiPublish(b);
}

void TraceDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	NVGcontext* vg = args.vg;
	float s = box.size.x / TR_DESIGN_W;
	if (!font || font->handle < 0) font = sfs::screenFontFace();

	nvgBeginPath(vg);
	nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, mm2px(1.f));
	nvgFillColor(vg, sfs::SCREEN_BG);
	nvgFill(vg);

	if (!module) { drawPreview(args, s); OpaqueWidget::drawLayer(args, layer); return; }

	nvgSave(vg);
	nvgScissor(vg, 0, 0, box.size.x, box.size.y);

	int cells = module->cells;
	double pp = module->paperPos;
	float px0 = TR_GX0, pxW = TR_GW;
	float py = paneY(), ph = paneH();

	// ── header ──────────────────────────────────────────────────────────────
	if (font && font->handle >= 0) {
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		std::string len = module->dispClocked
			? string::f("CLK  %d BAR%s", module->dispBars, module->dispBars == 1 ? "" : "S")
			: string::f("LEN  %.1fs", module->dispLenSec);
		nvgText(vg, 4.f * s, hdrH() * 0.5f * s, len.c_str(), NULL);

		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_TEXT);
		// The readout printed the ABSOLUTE speed, so running backwards looked
		// exactly like running forwards -- and a bipolar CV swinging through
		// zero looked like the transport had simply stopped somewhere.
		float sp = module->dispSpeed;
		std::string spd = std::fabs(sp) < 0.02f ? std::string("STOP")
		                : string::f("%s %.2fx", module->dispRev ? "<<" : ">>",
		                            std::fabs(sp));
		nvgText(vg, TR_DESIGN_W * 0.5f * s, hdrH() * 0.5f * s, spd.c_str(), NULL);

		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		std::string rhs = module->eraseMode
			? string::f("ERASE %d", module->brushLane + 1)
			: string::f("BRUSH %d", module->brushLane + 1);
		nvgText(vg, (TR_DESIGN_W - 4.f) * s, hdrH() * 0.5f * s, rhs.c_str(), NULL);
	}

	// ── pane frame and centre rule ──────────────────────────────────────────
	nvgBeginPath(vg);
	nvgRect(vg, px0 * s, py * s, pxW * s, ph * s);
	nvgStrokeColor(vg, nvgRGB(0x40, 0x40, 0x60));
	nvgStrokeWidth(vg, 1.f);
	nvgStroke(vg);

	nvgBeginPath(vg);
	nvgMoveTo(vg, px0 * s, (py + ph * 0.5f) * s);
	nvgLineTo(vg, (px0 + pxW) * s, (py + ph * 0.5f) * s);
	nvgStrokeColor(vg, sfs::SCREEN_PURP);
	nvgStrokeWidth(vg, 1.f);
	nvgStroke(vg);

	// ── the seam ────────────────────────────────────────────────────────────
	// Where the paper joins. It travels, which is what says this is a loop of
	// paper rather than a window onto a buffer.
	{
		double rel = -pp;                       // cell 0 relative to NOW
		while (rel < -cells * 0.5) rel += cells;
		while (rel >  cells * 0.5) rel -= cells;
		float x = px0 + (0.5f + (float)(rel / cells)) * pxW;
		if (x >= px0 && x <= px0 + pxW) {
			nvgBeginPath(vg);
			nvgMoveTo(vg, x * s, py * s);
			nvgLineTo(vg, x * s, (py + ph) * s);
			nvgStrokeColor(vg, sfs::SCREEN_PMID);
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
		}
	}

	// ── the lanes, as ribbons whose width is ink ────────────────────────────
	// PEAKS PER COLUMN, not one cell per column. The window holds the whole
	// loop, so at four seconds each pixel already spans ten cells and at sixty
	// it spans a hundred and fifty. Point-sampling one of them is aliasing, and
	// aliasing on a hand-drawn line looks like the line itself is stepped.
	// Reading the min and max instead is the same envelope Phase draws.
	// The ribbon is a POLYLINE OFFSET ALONG ITS OWN NORMAL, not a pair of
	// vertical offsets. Widening vertically by sqrt(1+m^2) does give a steep
	// run the right perpendicular thickness while it stays a line, but at a
	// STEP -- the two ends of every stroke -- the slope term saturates and
	// throws a tall spike above and below the edge. A vertical line needs its
	// width added horizontally, which only a real normal can do: there the
	// normal is horizontal, so the offset is too, and the step just comes out
	// as a thick vertical stroke.
	static const int TR_MAXCOLS = 512;
	int cols = (int)clamp(pxW * s, 24.f, (float)TR_MAXCOLS);
	float cxA[TR_MAXCOLS + 1], cyA[TR_MAXCOLS + 1], hwA[TR_MAXCOLS + 1];
	float txA[TR_MAXCOLS + 1], tyA[TR_MAXCOLS + 1];
	float bxA[TR_MAXCOLS + 1], byA[TR_MAXCOLS + 1];

	for (int i = 0; i < TR_LANES; i++) {
		Trace::Lane& L = module->lane[i];
		float lo = L.range ? 0.f : -5.f, hi = L.range ? 10.f : 5.f;

		double per = (double)cells / cols;              // cells to a column
		// A FIXED number of samples per column, evenly spaced. Walking
		// `for (c = c0; c < c0 + per; c += stride)` yields fourteen samples on
		// one frame and fifteen on the next, purely from where the column's
		// sub-cell start happens to fall -- so the average jumped as the paper
		// scrolled even though the paper under it had not changed. A filter
		// whose tap count depends on sub-pixel phase is not a filter.
		int nSamp = (int)std::min(32.0, std::max(1.0, std::ceil(per)));
		// WHERE THE WIGGLE CAME FROM. Two things, both here, and both of them
		// manufacture movement out of a paper that is not moving.
		//
		// The centre was the midpoint of the column's MIN and MAX. An extreme
		// enters or leaves the window the instant the column slides by a
		// fraction of a cell, so the midpoint hops even though nothing under
		// it changed. The MEAN of the same samples moves smoothly, because
		// every sample only ever contributes its 1/N.
		//
		// And each sample read its NEAREST cell. Sample points slide
		// continuously while the cell they land in changes in steps, and with
		// the sample spacing close to one cell the two beat against each
		// other. Interpolating removes the staircase entirely.
		//
		// Measured on a hand-drawn line held still: 0.77 design units of
		// shimmer peak to peak and 0.30 per frame, down to 0.14 and 0.025.
		for (int k = 0; k <= cols; k++) {
			double c0 = pp + ((double)k / cols - 0.5) * cells;
			float vSum = 0.f, inkSum = 0.f;
			for (int j = 0; j < nSamp; j++) {
				double c = c0 + ((double)j + 0.5) * per / nSamp;
				double f = std::floor(c);
				int a = trWrap((int)f, cells), b = trWrap(a + 1, cells);
				float t = (float)(c - f);
				vSum   += module->pv[i][a] + (module->pv[i][b] - module->pv[i][a]) * t;
				inkSum += module->pk[i][a] + (module->pk[i][b] - module->pk[i][a]) * t;
			}
			float mid = vSum / nSamp;
			cxA[k] = px0 + (float)k / cols * pxW;
			cyA[k] = py + ph * (1.f - clamp((mid - lo) / (hi - lo), 0.f, 1.f));
			hwA[k] = 0.6f + (inkSum / nSamp) * 3.4f;
		}
		// Smooth the width and the path across a few columns, and take the
		// tangent over a wider span. A brush is a round tip dragged along a
		// line: its width does not step from column to column and its
		// direction does not swing by ninety degrees between neighbours.
		{
			float tmp[TR_MAXCOLS + 1];
			for (int k = 0; k <= cols; k++) {
				float acc = 0.f; int n = 0;
				for (int d = -2; d <= 2; d++) {
					int kk = k + d; if (kk < 0 || kk > cols) continue;
					acc += hwA[kk]; n++;
				}
				tmp[k] = acc / n;
			}
			for (int k = 0; k <= cols; k++) hwA[k] = tmp[k];
			for (int k = 0; k <= cols; k++) {
				int ka = std::max(k - 1, 0), kb = std::min(k + 1, cols);
				tmp[k] = (cyA[ka] + cyA[k] + cyA[kb]) / 3.f;
			}
			for (int k = 0; k <= cols; k++) cyA[k] = tmp[k];
		}
		for (int k = 0; k <= cols; k++) {
			int ka = std::max(k - 2, 0), kb = std::min(k + 2, cols);
			float dx = cxA[kb] - cxA[ka], dy = cyA[kb] - cyA[ka];
			float len = std::sqrt(dx * dx + dy * dy);
			float nx = 0.f, ny = -1.f;                  // flat line: straight up
			if (len > 1e-5f) { nx = -dy / len; ny = dx / len; }
			txA[k] = cxA[k] + nx * hwA[k];  tyA[k] = cyA[k] + ny * hwA[k];
			bxA[k] = cxA[k] - nx * hwA[k];  byA[k] = cyA[k] - ny * hwA[k];
		}

		nvgBeginPath(vg);
		for (int k = 0; k <= cols; k++) {
			if (k == 0) nvgMoveTo(vg, txA[k] * s, tyA[k] * s);
			else        nvgLineTo(vg, txA[k] * s, tyA[k] * s);
		}
		for (int k = cols; k >= 0; k--)
			nvgLineTo(vg, bxA[k] * s, byA[k] * s);
		nvgClosePath(vg);
		nvgFillColor(vg, nvgTransRGBA(TR_LANECOL[i], 220));
		nvgFill(vg);
	}

	// ── the brush ghost ─────────────────────────────────────────────────────
	// What the cursor is asking for, alongside what the brush has managed. At a
	// bar of slew the lag is enormous, and without this it reads as a bug.
	if (module->uiCur.down) {
		int L = clamp(module->brushLane, 0, TR_LANES - 1);
		float lo = module->lane[L].range ? 0.f : -5.f;
		float hi = module->lane[L].range ? 10.f : 5.f;
		float tv = module->uiCur.val;
		float off = module->uiCur.off;
		float fx = clamp(0.5f + off / std::max(cells, 1), 0.f, 1.f);
		float gx = px0 + fx * pxW;
		float gy = py + ph * (1.f - clamp((tv - lo) / (hi - lo), 0.f, 1.f));
		nvgBeginPath(vg);
		nvgCircle(vg, gx * s, gy * s, 3.f * s);
		nvgStrokeColor(vg, nvgTransRGBA(TR_LANECOL[L], 200));
		nvgStrokeWidth(vg, 1.2f);
		nvgStroke(vg);
		// the brush's actual position, and the gap it still has to close
		float by = py + ph * (1.f - clamp((module->lane[L].pos - lo) / (hi - lo), 0.f, 1.f));
		nvgBeginPath(vg);
		nvgMoveTo(vg, gx * s, gy * s);
		nvgLineTo(vg, gx * s, by * s);
		nvgStrokeColor(vg, nvgTransRGBA(TR_LANECOL[L], 90));
		nvgStroke(vg);
	}

	// ── NOW ─────────────────────────────────────────────────────────────────
	{
		float x = px0 + 0.5f * pxW;
		nvgBeginPath(vg);
		nvgMoveTo(vg, x * s, (py - 2.f) * s);
		nvgLineTo(vg, x * s, (py + ph + 2.f) * s);
		nvgStrokeColor(vg, sfs::SCREEN_HOT);
		nvgStrokeWidth(vg, 1.5f);
		nvgStroke(vg);
	}

	// ── the read heads, one line each ───────────────────────────────────────
	// Drawn for every lane including those sitting on NOW, so the picture is
	// always four heads rather than however many happen to be off zero.
	for (int i = 0; i < TR_LANES; i++) {
		float fx = 0.5f + module->lane[i].offsetEff;
		if (fx > 1.f) fx -= 1.f;
		float x = px0 + fx * pxW;
		nvgBeginPath(vg);
		nvgMoveTo(vg, x * s, py * s);
		nvgLineTo(vg, x * s, (py + ph) * s);
		nvgStrokeColor(vg, nvgTransRGBA(TR_LANECOL[i], 110));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
		// a solid foot, so it reads as a head rather than a grid line
		nvgBeginPath(vg);
		nvgMoveTo(vg, x * s, (py + ph) * s);
		nvgLineTo(vg, (x - 2.5f) * s, (py + ph + 3.5f) * s);
		nvgLineTo(vg, (x + 2.5f) * s, (py + ph + 3.5f) * s);
		nvgClosePath(vg);
		nvgFillColor(vg, TR_LANECOL[i]);
		nvgFill(vg);
	}

	// ── lane strips: the same x axis as the pane above ──────────────────────
	for (int i = 0; i < TR_LANES; i++) {
		Trace::Lane& L = module->lane[i];
		float y = stripY(i), h = stripH() - 2.f;

		nvgBeginPath(vg);
		nvgRect(vg, px0 * s, (y + h * 0.35f) * s, pxW * s, h * 0.3f * s);
		nvgFillColor(vg, sfs::SCREEN_PURP);
		nvgFill(vg);

		// NOW, so it is obvious where a zero offset lands
		nvgBeginPath(vg);
		nvgRect(vg, (px0 + pxW * 0.5f - 0.5f) * s, (y + h * 0.2f) * s,
		        1.f * s, h * 0.6f * s);
		nvgFillColor(vg, sfs::SCREEN_PMID);
		nvgFill(vg);

		// the handle, directly beneath this lane's triangle on the pane
		float fx = 0.5f + L.offsetEff;
		if (fx > 1.f) fx -= 1.f;
		float hx = px0 + fx * pxW;
		nvgBeginPath(vg);
		nvgRect(vg, (hx - 1.5f) * s, y * s, 3.f * s, h * s);
		nvgFillColor(vg, TR_LANECOL[i]);
		nvgFill(vg);

		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, TR_LANECOL[i]);
			nvgText(vg, 1.f * s, (y + h * 0.5f) * s, string::f("%d", i + 1).c_str(), NULL);
		}

		float tl = clamp(L.trigLight, 0.f, 1.f);
		nvgBeginPath(vg);
		nvgCircle(vg, (TR_DESIGN_W - 11.f) * s, (y + h * 0.5f) * s, 2.6f * s);
		nvgFillColor(vg, nvgTransRGBA(TR_LANECOL[i], (int)(40 + tl * 215)));
		nvgFill(vg);

		if (font && font->handle >= 0) {
			const char* g = L.orient > 0 ? "/" : L.orient < 0 ? "\\" : "-";
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, (TR_DESIGN_W - 4.f) * s, (y + h * 0.5f) * s, g, NULL);
		}
	}

	nvgRestore(vg);
	OpaqueWidget::drawLayer(args, layer);
}

void TraceDisplay::drawPreview(const DrawArgs& args, float s) {
	NVGcontext* vg = args.vg;
	float px0 = TR_GX0, pxW = TR_GW;
	float py = paneY(), ph = paneH();
	if (!font || font->handle < 0) font = sfs::screenFontFace();

	nvgBeginPath(vg);
	nvgRect(vg, px0 * s, py * s, pxW * s, ph * s);
	nvgStrokeColor(vg, nvgRGB(0x40, 0x40, 0x60));
	nvgStrokeWidth(vg, 1.f);
	nvgStroke(vg);

	if (font && font->handle >= 0) {
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		nvgText(vg, 4.f * s, hdrH() * 0.5f * s, "LEN  4.0s", NULL);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_TEXT);
		nvgText(vg, TR_DESIGN_W * 0.5f * s, hdrH() * 0.5f * s, "1.00x", NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		nvgText(vg, (TR_DESIGN_W - 4.f) * s, hdrH() * 0.5f * s, "BRUSH 1", NULL);
	}

	// Offset along the normal, exactly as the live ribbon does, or the
	// thumbnail advertises a stroke that pinches on its steep sections.
	const int cols = 120;
	float cx[cols + 1], cy[cols + 1], hw[cols + 1];
	for (int i = 0; i < TR_LANES; i++) {
		for (int k = 0; k <= cols; k++) {
			float fx = (float)k / cols, a = fx * 6.2831853f;
			float v = std::sin(a * (1 + i) + i * 1.1f) * (0.32f - i * 0.04f);
			float ink = 0.25f + 0.35f * (0.5f + 0.5f * std::sin(a * 2.f + i));
			cx[k] = px0 + fx * pxW;
			cy[k] = py + ph * (0.5f - v);
			hw[k] = 0.6f + ink * 3.4f;
		}
		nvgBeginPath(vg);
		for (int pass = 0; pass < 2; pass++) {
			for (int k = 0; k <= cols; k++) {
				int kk = pass ? (cols - k) : k;
				int ka = std::max(kk - 1, 0), kb = std::min(kk + 1, cols);
				float dx = cx[kb] - cx[ka], dy = cy[kb] - cy[ka];
				float len = std::sqrt(dx * dx + dy * dy);
				float nx = 0.f, ny = -1.f;
				if (len > 1e-5f) { nx = -dy / len; ny = dx / len; }
				float sgn = pass ? -1.f : 1.f;
				float ox = cx[kk] + nx * hw[kk] * sgn;
				float oy = cy[kk] + ny * hw[kk] * sgn;
				if (pass == 0 && k == 0) nvgMoveTo(vg, ox * s, oy * s);
				else nvgLineTo(vg, ox * s, oy * s);
			}
		}
		nvgClosePath(vg);
		nvgFillColor(vg, nvgTransRGBA(TR_LANECOL[i], 220));
		nvgFill(vg);
	}

	nvgBeginPath(vg);
	nvgMoveTo(vg, (px0 + pxW * 0.5f) * s, (py - 2.f) * s);
	nvgLineTo(vg, (px0 + pxW * 0.5f) * s, (py + ph + 2.f) * s);
	nvgStrokeColor(vg, sfs::SCREEN_HOT);
	nvgStrokeWidth(vg, 1.5f);
	nvgStroke(vg);

	for (int i = 0; i < TR_LANES; i++) {
		float y = stripY(i), h = stripH() - 2.f;
		nvgBeginPath(vg);
		nvgRect(vg, px0 * s, (y + h * 0.35f) * s, pxW * s, h * 0.3f * s);
		nvgFillColor(vg, sfs::SCREEN_PURP);
		nvgFill(vg);
		float hx = px0 + (0.5f + i * 0.13f) * pxW;
		nvgBeginPath(vg);
		nvgRect(vg, (hx - 1.5f) * s, y * s, 3.f * s, h * s);
		nvgFillColor(vg, TR_LANECOL[i]);
		nvgFill(vg);
	}
	// the read heads, matching the live display
	for (int i = 0; i < TR_LANES; i++) {
		float x = px0 + (0.5f + i * 0.13f) * pxW;
		nvgBeginPath(vg);
		nvgMoveTo(vg, x * s, py * s);
		nvgLineTo(vg, x * s, (py + ph) * s);
		nvgStrokeColor(vg, nvgTransRGBA(TR_LANECOL[i], 110));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgMoveTo(vg, x * s, (py + ph) * s);
		nvgLineTo(vg, (x - 2.5f) * s, (py + ph + 3.5f) * s);
		nvgLineTo(vg, (x + 2.5f) * s, (py + ph + 3.5f) * s);
		nvgClosePath(vg);
		nvgFillColor(vg, TR_LANECOL[i]);
		nvgFill(vg);
	}
}

// =============================================================================
// Widget
// =============================================================================

// Panel geometry. Lane rows run across the module: the lane's draw CV on the
// left, its three outputs on the right, so the panel states the shape the
// module actually has.
// Three input columns mirroring the three output columns: the lane rows carry
// that lane's draw CV and its head-offset CV, and the globals take column C.
static const float TR_INX1 =   8.40f, TR_INX2 =  20.40f, TR_INX3 = 32.40f;
static const float TR_OUTX1 = 140.60f, TR_OUTX2 = 152.60f, TR_OUTX3 = 164.60f;
static const float TR_ROW[5] = {20.00f, 34.00f, 48.00f, 62.00f, 76.00f};
static const float TR_KY = 100.00f;     // control row
static const float TR_JY = 116.00f;     // the jacks under it
static const float TR_KX[6] = {50.00f, 63.00f, 76.00f, 89.00f, 102.00f, 115.00f};
static const float TR_BRX = 128.00f;    // first brush button
static const float TR_BRP = 8.60f;      // brush button pitch

struct TraceWidget : ModuleWidget {
	TraceWidget(Trace* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/trace.svg")));

		TraceDisplay* disp = new TraceDisplay;
		disp->module = module;
		disp->box.pos = mm2px(Vec(38.50f, 7.50f));
		disp->box.size = mm2px(Vec(96.00f, 77.00f));
		addChild(disp);

		// inputs, left
		for (int i = 0; i < TR_LANES; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_INX1, TR_ROW[i])),
			                                         module, Trace::LANE_INPUT + i));
		for (int i = 0; i < TR_LANES; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_INX2, TR_ROW[i])),
			                                         module, Trace::OFFSET_INPUT + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_INX3, TR_ROW[i])),
			                                         module, Trace::THICK_INPUT + i));
		}
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_INX1, TR_ROW[4])), module, Trace::WRITE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_INX2, TR_ROW[4])), module, Trace::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_INX3, TR_ROW[4])), module, Trace::BAR_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_KX[0], TR_JY)), module, Trace::SPEED_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_KX[2], TR_JY)), module, Trace::SLEW_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TR_KX[3], TR_JY)), module, Trace::INK_INPUT));

		// outputs, right: one row per lane, value / ink / trigger
		for (int i = 0; i < TR_LANES; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(TR_OUTX1, TR_ROW[i])),
			                                           module, Trace::VALUE_OUTPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(TR_OUTX2, TR_ROW[i])),
			                                           module, Trace::INK_OUTPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(TR_OUTX3, TR_ROW[i])),
			                                           module, Trace::TRIG_OUTPUT + i));
		}
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(TR_OUTX3, TR_ROW[4])),
		                                           module, Trace::LOOP_OUTPUT));

		// transport
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
			mm2px(Vec(10.00f, TR_KY)), module, Trace::RUN_PARAM, Trace::RUN_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(
			mm2px(Vec(22.00f, TR_KY)), module, Trace::DIR_PARAM, Trace::DIR_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(34.00f, TR_KY)), module, Trace::RESET_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.00f, TR_JY)), module, Trace::RUN_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.00f, TR_JY)), module, Trace::DIR_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(34.00f, TR_JY)), module, Trace::RESET_INPUT));

		// controls
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TR_KX[0], TR_KY)), module, Trace::SPEED_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TR_KX[1], TR_KY)), module, Trace::LENGTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TR_KX[2], TR_KY)), module, Trace::SLEW_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TR_KX[3], TR_KY)), module, Trace::INK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TR_KX[4], TR_KY)), module, Trace::LEAK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(TR_KX[5], TR_KY)), module, Trace::FLAT_PARAM));

		// Brush selector. ERASE is RED, not another white one: it latches, and
		// a latched erase looks exactly like drawing that does not work.
		addParam(createLightParamCentered<VCVLightBezel<TrLaneLight0>>(
			mm2px(Vec(TR_BRX + 0 * TR_BRP, TR_KY)), module,
			Trace::BRUSH_PARAM + 0, Trace::BRUSH_LIGHT + 0));
		addParam(createLightParamCentered<VCVLightBezel<TrLaneLight1>>(
			mm2px(Vec(TR_BRX + 1 * TR_BRP, TR_KY)), module,
			Trace::BRUSH_PARAM + 1, Trace::BRUSH_LIGHT + 1));
		addParam(createLightParamCentered<VCVLightBezel<TrLaneLight2>>(
			mm2px(Vec(TR_BRX + 2 * TR_BRP, TR_KY)), module,
			Trace::BRUSH_PARAM + 2, Trace::BRUSH_LIGHT + 2));
		addParam(createLightParamCentered<VCVLightBezel<TrLaneLight3>>(
			mm2px(Vec(TR_BRX + 3 * TR_BRP, TR_KY)), module,
			Trace::BRUSH_PARAM + 3, Trace::BRUSH_LIGHT + 3));
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(
			mm2px(Vec(TR_BRX + 4 * TR_BRP, TR_KY)), module,
			Trace::BRUSH_PARAM + 4, Trace::BRUSH_LIGHT + 4));

		sfs::PanelLabels* lab = new sfs::PanelLabels;
		lab->title(6.f, 122.f, "TRACE");
		for (int i = 0; i < TR_LANES; i++) {
			lab->jack(TR_INX1, TR_ROW[i], string::f("L%d", i + 1));
			lab->jack(TR_INX2, TR_ROW[i], string::f("OF%d", i + 1));
			lab->jack(TR_INX3, TR_ROW[i], string::f("TH%d", i + 1));
		}
		// The output rows are the lanes, top to bottom, in the same order as
		// the screen's numbered strips — so the columns get the labels and the
		// rows do not need repeating four times.
		lab->jackOnPlate(TR_OUTX1, TR_ROW[0], "VAL");
		lab->jack(TR_INX1, TR_ROW[4], "WRT");
		lab->jack(TR_INX2, TR_ROW[4], "CLK");
		lab->jack(TR_INX3, TR_ROW[4], "BAR");
		// the three that modulate a knob now sit under it, joined by a line
		lab->link(TR_KX[0], TR_KY, TR_KX[0], TR_JY);
		lab->link(TR_KX[2], TR_KY, TR_KX[2], TR_JY);
		lab->link(TR_KX[3], TR_KY, TR_KX[3], TR_JY);
		lab->jackOnPlate(TR_OUTX2, TR_ROW[0], "INK");
		lab->jackOnPlate(TR_OUTX3, TR_ROW[0], "TRG");
		lab->jackOnPlate(TR_OUTX3, TR_ROW[4], "LOOP");
		lab->jack(10.00f, TR_KY, "RUN");
		lab->jack(22.00f, TR_KY, "DIR");
		lab->jack(34.00f, TR_KY, "RST");
		static const char* KN[6] = {"SPEED", "LENGTH", "SLEW", "INK", "LEAK", "FLAT"};
		for (int i = 0; i < 6; i++) lab->knob(TR_KX[i], TR_KY, KN[i]);
		lab->add(TR_BRX + 2.f * TR_BRP, TR_KY - 6.6f, "BRUSH");
		lab->note(TR_BRX + 4.f * TR_BRP, TR_KY + 7.0f, "ERA");
		addChild(lab);
	}

	void appendContextMenu(Menu* menu) override {
		Trace* m = dynamic_cast<Trace*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);

		for (int i = 0; i < TR_LANES; i++) {
			menu->addChild(createSubmenuItem(string::f("Lane %d", i + 1), "",
				[=](Menu* sub) {
					sub->addChild(createIndexPtrSubmenuItem("Range",
						{"Bipolar +/-5V", "Unipolar 0-10V"}, &m->lane[i].range));
					sub->addChild(createIndexPtrSubmenuItem("Read",
						{"Smooth", "Stepped (sharp edges)"}, &m->lane[i].readMode));
					std::vector<std::string> qs;
					for (int q = 0; q < TR_NQUANT; q++) qs.push_back(TR_QUANTNAME[q]);
					sub->addChild(createIndexPtrSubmenuItem("Quantize", qs, &m->lane[i].quant));
					std::vector<std::string> cs;
					for (int c = 0; c < TC_NCOND; c++) cs.push_back(TR_CONDNAME[c]);
					sub->addChild(createIndexPtrSubmenuItem("Trigger on", cs, &m->lane[i].cond));
					sub->addChild(new MenuSeparator);
					sub->addChild(createMenuItem("Clear lane", "", [=]() {
						std::fill(m->pv[i].begin(), m->pv[i].end(), 0.f);
						std::fill(m->pk[i].begin(), m->pk[i].end(), 0.f);
						m->lane[i].drawn = false;
					}));
					// Copying a shape across lanes and then offsetting their
					// heads is how you get phase-shifted reads of one gesture,
					// so this is load-bearing rather than a convenience.
					for (int j = 0; j < TR_LANES; j++) {
						if (j == i) continue;
						sub->addChild(createMenuItem(string::f("Copy to lane %d", j + 1), "", [=]() {
							m->pv[j] = m->pv[i];
							m->pk[j] = m->pk[i];
							m->lane[j].drawn = m->lane[i].drawn;
						}));
					}
				}));
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Clocks per bar",
			{"1", "2", "4", "8", "12", "16", "24", "32"},
			[=]() {
				static const int v[] = {1, 2, 4, 8, 12, 16, 24, 32};
				for (int i = 0; i < 8; i++) if (v[i] == m->clocksPerBar) return i;
				return 5;
			},
			[=](int i) {
				static const int v[] = {1, 2, 4, 8, 12, 16, 24, 32};
				m->clocksPerBar = v[clamp(i, 0, 7)];
			}));
		menu->addChild(createMenuItem("Clear all paper", "", [=]() {
			for (int i = 0; i < TR_LANES; i++) {
				std::fill(m->pv[i].begin(), m->pv[i].end(), 0.f);
				std::fill(m->pk[i].begin(), m->pk[i].end(), 0.f);
				m->lane[i].drawn = false;
			}
		}));
	}
};

Model* modelTrace = createModel<Trace, TraceWidget>("Trace");
