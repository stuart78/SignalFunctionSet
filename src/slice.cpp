#include "plugin.hpp"
#include "panel-style.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

// =============================================================================
// Slice — cut a stereo stream into a grid and do something to each piece.
//
// Thirty seconds of audio go into a circular buffer. A grid of slices runs over
// it, 10ms to a second each, either free-running or measured from a CLOCK. When
// a slice comes round, Slice rolls against seven weights and either passes it
// through or does one thing to it.
//
// THE MODULE IS ZERO-LATENCY WHEN IT PASSES THROUGH, and that shapes the rest.
// A slice that is played straight is read from the write head itself, sample
// for sample. But you cannot reverse a slice you are still recording, so the
// transforms that need a finished slice reach back one slot and work on THAT.
// Reverse plays the previous slice backwards; pitch plays it slowed. The
// alternative is to delay everything by one slice so each transform can act on
// "this" one, and at a one-second slice that is a second of latency on a live
// input — unusable in the chain this belongs in.
//
// REPEAT is the exception and gets to stay live: it plays the first 1/N of the
// slice as it arrives, then loops the fragment it has just captured.
//
// THE EDGE FADE IS NOT A GRAIN WINDOW. Each slice fades in and out over a few
// milliseconds and runs at unity in between, so with DEPTH at zero the module
// is transparent and can sit on a bus. Enveloping the whole slice instead —
// which is what a Hann window means in granular synthesis — would amplitude-
// modulate the signal at the slice rate whether or not anything was being
// transformed, and that is a tremolo, not a slicer. The whole-slice form is on
// the menu for when that IS what you want.
//
// The choices are SEEDED. A shuffle that re-rolls forever is texture; one that
// repeats every eight slices is a part you can write against, and that is the
// difference between this and a random-buffer effect. PATTERN sets the length.
// =============================================================================

static const int   SLICE_NXF     = 7;        // transforms, not counting passthrough
static const float SLICE_BUFSEC  = 30.f;
static const float SLICE_MINLEN  = 0.010f;   // 10ms
static const float SLICE_MAXLEN  = 1.000f;

// The transforms, in panel order. APPEND ONLY: the weights are params and the
// screen rows are drawn from this, so inserting one re-points a saved patch.
enum SliceXf {
	XF_CUT,        // silence
	XF_SWAP,       // left and right exchanged
	XF_DELAY,      // a slice from further back, in order
	XF_SHUFFLE,    // a slice from anywhere in range
	XF_REVERSE,    // the previous slice, backwards
	XF_REPEAT,     // the first 1/N of this slice, looped
	XF_PITCH,      // the previous slice, at half or double rate
};
static const char* SLICE_XFNAME[SLICE_NXF] =
	{"CUT", "SWAP", "DELAY", "SHUF", "REV", "REPEAT", "PITCH"};

// A PATTERN is a fixed sequence of what to do to each slice, and you pick one.
//
// The first cut of this module rolled seven weights per slice, which sounds
// like a good idea and is not: everything came out as the same undifferentiated
// scatter, and no setting was a thing you could learn, play against or come
// back to. A pattern is repeatable by construction. RANDOM is one of the
// entries rather than the whole design, so the scatter is still there when you
// want it, as a choice among twelve rather than as the only behaviour.
//
// -1 is "leave this slice alone", and the straight slices matter as much as the
// altered ones: they are what makes the alteration land.
struct SlicePattern { const char* name; int len; int8_t slot[8]; };
static const SlicePattern SLICE_PATS[] = {
	{"Straight",  1, {-1}},
	{"Gate 2",    2, {-1, XF_CUT}},
	{"Gate 4",    4, {-1, -1, -1, XF_CUT}},
	{"Ping-pong", 2, {-1, XF_SWAP}},
	{"Reverse 2", 2, {-1, XF_REVERSE}},
	{"Echo",      4, {-1, -1, XF_DELAY, -1}},
	{"Stutter",   4, {-1, -1, -1, XF_REPEAT}},
	{"Dive",      4, {-1, -1, -1, XF_PITCH}},
	{"Scatter",   8, {XF_SHUFFLE, -1, XF_SHUFFLE, -1, -1, XF_SHUFFLE, -1, -1}},
	{"Tumble",    8, {-1, XF_REVERSE, -1, XF_DELAY, -1, XF_REPEAT, -1, XF_SWAP}},
	{"Glitch",    8, {-1, XF_REPEAT, XF_CUT, -1, XF_REVERSE, -1, XF_REPEAT, XF_CUT}},
	{"Random",   -1, {-1}},        // len -1: rolled per slice, from the seed
};
static const int SLICE_NPAT = (int)(sizeof(SLICE_PATS) / sizeof(SLICE_PATS[0]));

enum SliceShape { SH_HANN, SH_BELL, SH_SQUARE, SH_TRI, SH_COUNT };
static const char* SLICE_SHAPENAME[SH_COUNT] = {"Hann", "Bell", "Square", "Triangle"};

// The fade curve, over t in 0..1 rising into the slice. Square has no fade at
// all, which is the point of choosing it: the click IS the sound.
static inline float sliceFade(int shape, float t) {
	t = clamp(t, 0.f, 1.f);
	switch (shape) {
		case SH_SQUARE: return 1.f;
		case SH_TRI:    return t;
		case SH_BELL:   return t * t * (3.f - 2.f * t);      // smoothstep
		default:        return 0.5f * (1.f - std::cos(t * (float)M_PI));
	}
}

struct Slice;

struct SliceDisplay : OpaqueWidget {
	Slice* module = nullptr;
	std::shared_ptr<Font> font;
	void drawLayer(const DrawArgs& args, int layer) override;
	void drawPreview(const DrawArgs& args);
	void drawLive(const DrawArgs& args);
	void onButton(const ButtonEvent& e) override;
	int rowAt(Vec p) const;
};

struct Slice : Module {
	// Slice has never shipped, so this enum could be rebuilt rather than
	// appended to. Once it does ship, that stops being true.
	enum ParamId {
		PATTERN_PARAM, LENGTH_PARAM, DEPTH_PARAM, RANGE_PARAM,
		DIV_PARAM, SHAPE_PARAM, LINK_PARAM,
		FREEZE_PARAM, RESEED_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		L_INPUT, R_INPUT, CLOCK_INPUT, BAR_INPUT, RESET_INPUT,
		FREEZE_INPUT, RESEED_INPUT,
		PATTERN_INPUT, DEPTH_INPUT, LENGTH_INPUT,
		INPUTS_LEN
	};
	enum OutputId { L_OUTPUT, R_OUTPUT, SLICE_OUTPUT, XF_OUTPUT, OUTPUTS_LEN };
	enum LightId  { FREEZE_LIGHT, LIGHTS_LEN };

	// ── the buffer ───────────────────────────────────────────────────────────
	std::vector<float> bufL, bufR;
	long  bufN = 0;                   // capacity, samples
	long  wr = 0;                     // write head
	bool  primed = false;             // the buffer has been filled once
	long  written = 0;                // samples written since load (caps reads)

	// ── the grid ─────────────────────────────────────────────────────────────
	double slicePos = 0.0;            // samples into the current slice
	float  sliceLen = 4800.f;         // current slice length, samples
	long   sliceIdx = 0;              // slices since reset
	float  clockLen = 0.f;            // measured clock interval, samples
	float  sinceClock = 0.f;
	float  barLen = 0.f;              // measured bar, samples
	float  sinceBar = 0.f;

	// ── the slot in progress ─────────────────────────────────────────────────
	int    xf = -1;                   // SliceXf, or -1 for passthrough
	double rdPos = 0.0;               // read head into the buffer (absolute, may be fractional)
	double rdRate = 1.0;
	long   repLen = 0;                // repeat: fragment length
	long   repStart = 0;
	bool   swapCh = false;

	// ── options ──────────────────────────────────────────────────────────────
	bool  wholeWindow = false;        // envelope the whole slice, granular-style
	float fadeMs = 4.f;
	bool  freeze = false;

	// ── seeded choice ────────────────────────────────────────────────────────
	uint32_t seed = 0x5c1ce;
	// A tiny explicit PRNG rather than random::uniform(), because the whole
	// point is that the same pattern comes round again: it has to depend on the
	// slice index and the seed and on nothing else, not on how many times
	// anything happened to be called.
	static uint32_t hash32(uint32_t x) {
		x ^= x >> 16; x *= 0x7feb352dU;
		x ^= x >> 15; x *= 0x846ca68bU;
		x ^= x >> 16; return x;
	}
	float rollAt(long idx, int stream) {
		return (float)(hash32((uint32_t)(idx * 2654435761u) ^ hash32(seed + stream * 977u))
		               & 0xFFFFFF) / (float)0xFFFFFF;
	}

	dsp::SchmittTrigger clockTrig, barTrig, resetTrig, freezeTrig, freezeBtn, reseedTrig, reseedBtn;
	dsp::PulseGenerator slicePulse;

	// display mirrors
	float dispEnv = 0.f;
	int   dispXf = -1;
	float dispRdFrac = 0.f, dispWrFrac = 0.f;
	float dispPeak[128] = {};

	Slice() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		std::vector<std::string> pn;
		for (int i = 0; i < SLICE_NPAT; i++) pn.push_back(SLICE_PATS[i].name);
		configSwitch(PATTERN_PARAM, 0.f, (float)(SLICE_NPAT - 1), 0.f, "Pattern", pn);
		configParam(LENGTH_PARAM, std::log2(SLICE_MINLEN), std::log2(SLICE_MAXLEN),
		            std::log2(0.125f), "Slice length", " s", 2.f);
		configParam(DEPTH_PARAM, 0.f, 1.f, 1.f, "Depth", "%", 0.f, 100.f);
		configParam(RANGE_PARAM, 0.f, 1.f, 0.25f, "Reach back", "%", 0.f, 100.f);
		configSwitch(DIV_PARAM, 0.f, 5.f, 0.f, "Clock division",
		             {"x1", "x2", "x4", "/2", "/4", "/8"});
		configSwitch(SHAPE_PARAM, 0.f, (float)(SH_COUNT - 1), 0.f, "Edge shape",
		             {SLICE_SHAPENAME[0], SLICE_SHAPENAME[1],
		              SLICE_SHAPENAME[2], SLICE_SHAPENAME[3]});
		configSwitch(LINK_PARAM, 0.f, 1.f, 1.f, "Channels", {"Independent", "Paired"});
		configButton(FREEZE_PARAM, "Freeze the buffer");
		configButton(RESEED_PARAM, "Reseed (Random pattern only)");

		configInput(L_INPUT, "Left audio");
		configInput(R_INPUT, "Right audio (normalled from left)");
		configInput(CLOCK_INPUT, "Clock — sets the slice length");
		configInput(BAR_INPUT, "Bar — quantizes how far back DELAY and SHUFFLE reach");
		configInput(RESET_INPUT, "Reset the grid and the pattern");
		configInput(FREEZE_INPUT, "Freeze gate");
		configInput(RESEED_INPUT, "Reseed trigger");
		configInput(DEPTH_INPUT, "Depth CV (±5V)");
		configInput(LENGTH_INPUT, "Slice length CV (±5V)");
		configInput(PATTERN_INPUT, "Pattern CV (1V per pattern)");
		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");
		configOutput(SLICE_OUTPUT, "Trigger on every slice boundary");
		configOutput(XF_OUTPUT, "Gate — high while a slice is being altered");
		configBypass(L_INPUT, L_OUTPUT);
		configBypass(R_INPUT, R_OUTPUT);
		onSampleRateChange();
	}

	void onSampleRateChange() override {
		bufN = (long)(SLICE_BUFSEC * APP->engine->getSampleRate());
		bufL.assign((size_t)bufN, 0.f);
		bufR.assign((size_t)bufN, 0.f);
		wr = 0; written = 0; primed = false;
	}

	void onReset() override {
		std::fill(bufL.begin(), bufL.end(), 0.f);
		std::fill(bufR.begin(), bufR.end(), 0.f);
		wr = 0; written = 0; primed = false; freeze = false;
		slicePos = 0.0; sliceIdx = 0; xf = -1;
	}

	inline float readL(double p) const {
		long i0 = (long)std::floor(p);
		float f = (float)(p - (double)i0);
		long a = ((i0 % bufN) + bufN) % bufN, b = (a + 1) % bufN;
		return bufL[(size_t)a] * (1.f - f) + bufL[(size_t)b] * f;
	}
	inline float readR(double p) const {
		long i0 = (long)std::floor(p);
		float f = (float)(p - (double)i0);
		long a = ((i0 % bufN) + bufN) % bufN, b = (a + 1) % bufN;
		return bufR[(size_t)a] * (1.f - f) + bufR[(size_t)b] * f;
	}

	// How far back the transforms may reach, in samples: RANGE across the
	// buffer, but snapped to whole bars when a BAR clock is present, because
	// "one bar ago" lands and "1.37 seconds ago" does not.
	float reachSamples() {
		float r = clamp(params[RANGE_PARAM].getValue(), 0.f, 1.f);
		float maxBack = std::min((float)bufN, (float)written) - sliceLen * 2.f;
		if (maxBack < sliceLen) return sliceLen;
		float want = sliceLen + r * (maxBack - sliceLen);
		if (barLen > sliceLen) {
			float bars = std::max(1.f, std::round(want / barLen));
			want = std::min(bars * barLen, maxBack);
		}
		return want;
	}

	int patIndex() {
		int p = (int)std::round(params[PATTERN_PARAM].getValue()
		                        + inputs[PATTERN_INPUT].getVoltage());
		return clamp(p, 0, SLICE_NPAT - 1);
	}

	// Choose what happens to slot `idx`, and set the read head up for it.
	void beginSlice(long idx) {
		xf = -1; swapCh = false; rdRate = 1.0; repLen = 0;

		const SlicePattern& P = SLICE_PATS[patIndex()];
		if (P.len > 0) {
			// The whole point: slot N of the pattern, every time round.
			xf = P.slot[idx % P.len];
		} else {
			// RANDOM is a pattern like the others, not a mode. Still seeded, so
			// a take can be reproduced from a reset and RESEED means something.
			float r = rollAt(idx, 0);
			xf = (r < 0.45f) ? -1 : (int)(rollAt(idx, 1) * (float)SLICE_NXF) % SLICE_NXF;
		}
		if (xf < 0) return;

		double now = (double)wr;
		float reach = reachSamples();
		switch (xf) {
			case XF_SWAP:  swapCh = true; break;
			case XF_CUT:   break;
			case XF_REVERSE:
				// Start at the END of the previous slot and walk backwards.
				rdPos = now - 1.0; rdRate = -1.0; break;
			case XF_DELAY: {
				// A whole number of slices back, so the grid still lines up.
				float slots = std::max(1.f, std::floor(reach / std::max(sliceLen, 1.f)));
				rdPos = now - (double)(slots * sliceLen); break;
			}
			case XF_SHUFFLE: {
				float slots = std::max(1.f, std::floor(reach / std::max(sliceLen, 1.f)));
				float pick = 1.f + std::floor(rollAt(idx, 2) * slots);
				rdPos = now - (double)(pick * sliceLen); break;
			}
			case XF_REPEAT: {
				static const int DIVS[4] = {2, 3, 4, 8};
				int d = DIVS[(int)(rollAt(idx, 3) * 4.f) & 3];
				repLen = std::max(64L, (long)(sliceLen / (float)d));
				repStart = wr;
				rdPos = now; break;                    // live until the fragment fills
			}
			case XF_PITCH: {
				rdRate = (rollAt(idx, 4) < 0.5f) ? 0.5 : 2.0;
				rdPos = now - (double)sliceLen; break;
			}
		}
	}

	void process(const ProcessArgs& args) override {
		const float sr = args.sampleRate;
		if (bufN <= 0) return;

		// ── clocks ───────────────────────────────────────────────────────────
		sinceClock += 1.f; sinceBar += 1.f;
		if (inputs[CLOCK_INPUT].isConnected()
		    && clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (sinceClock > 4.f && sinceClock < sr * 8.f) clockLen = sinceClock;
			sinceClock = 0.f;
		}
		if (inputs[BAR_INPUT].isConnected()
		    && barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (sinceBar > 4.f && sinceBar < sr * 60.f) barLen = sinceBar;
			sinceBar = 0.f;
		}
		if (!inputs[BAR_INPUT].isConnected()) barLen = 0.f;

		bool rst = resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		if (rst) { sliceIdx = 0; slicePos = 0.0; xf = -1; }

		if (freezeBtn.process(params[FREEZE_PARAM].getValue() > 0.5f)) freeze = !freeze;
		if (inputs[FREEZE_INPUT].isConnected())
			freeze = inputs[FREEZE_INPUT].getVoltage() >= 1.f;
		lights[FREEZE_LIGHT].setBrightness(freeze ? 1.f : 0.f);

		if (reseedBtn.process(params[RESEED_PARAM].getValue() > 0.5f)
		    || reseedTrig.process(inputs[RESEED_INPUT].getVoltage(), 0.1f, 1.f))
			seed = hash32(seed ^ (uint32_t)wr);

		// ── slice length ─────────────────────────────────────────────────────
		static const float DIVMUL[6] = {1.f, 0.5f, 0.25f, 2.f, 4.f, 8.f};
		float want;
		if (clockLen > 0.f && inputs[CLOCK_INPUT].isConnected()) {
			want = clockLen * DIVMUL[(int)std::round(params[DIV_PARAM].getValue())];
		} else {
			float lv = clamp(params[LENGTH_PARAM].getValue()
			                 + inputs[LENGTH_INPUT].getVoltage() * 0.4f,
			                 std::log2(SLICE_MINLEN), std::log2(SLICE_MAXLEN));
			want = std::pow(2.f, lv) * sr;
		}
		// Only adopt a new length at a boundary: changing it mid-slice moves the
		// finish line while the read head is running at it.
		if (slicePos <= 0.0) sliceLen = clamp(want, SLICE_MINLEN * sr, SLICE_MAXLEN * sr);

		// ── write ────────────────────────────────────────────────────────────
		float inL = inputs[L_INPUT].getVoltage();
		float inR = inputs[R_INPUT].isConnected() ? inputs[R_INPUT].getVoltage() : inL;
		if (!freeze) {
			bufL[(size_t)wr] = inL;
			bufR[(size_t)wr] = inR;
			wr = (wr + 1) % bufN;
			if (written < bufN) written++;
			else primed = true;
		}

		// ── slice boundary ───────────────────────────────────────────────────
		if (slicePos <= 0.0) {
			beginSlice(sliceIdx);
			slicePulse.trigger(0.001f);
		}

		// ── read ─────────────────────────────────────────────────────────────
		float outL, outR;
		bool live = (xf < 0 || xf == XF_SWAP);
		if (freeze && live) {
			// Frozen, a passthrough slice has nothing live to pass: loop the
			// buffer instead, so freeze turns the module into a player.
			rdPos += 1.0;
			outL = readL(rdPos); outR = readR(rdPos);
		} else if (live) {
			long back = ((wr - 1) % bufN + bufN) % bufN;
			outL = bufL[(size_t)back]; outR = bufR[(size_t)back];
			rdPos = (double)wr;
		} else if (xf == XF_CUT) {
			outL = outR = 0.f;
		} else if (xf == XF_REPEAT) {
			// Live until the fragment is captured, then loop it.
			long into = (long)slicePos;
			if (into < repLen) { rdPos = (double)((repStart + into) % bufN); }
			else               { rdPos = (double)((repStart + (into % repLen)) % bufN); }
			outL = readL(rdPos); outR = readR(rdPos);
		} else {
			outL = readL(rdPos); outR = readR(rdPos);
			rdPos += rdRate;
		}
		if (swapCh) std::swap(outL, outR);

		// ── envelope ─────────────────────────────────────────────────────────
		int shape = (int)std::round(params[SHAPE_PARAM].getValue());
		float env;
		if (wholeWindow) {
			float t = (float)(slicePos / (double)sliceLen);
			env = sliceFade(shape, std::min(t, 1.f - t) * 2.f);
		} else {
			float fade = std::min(fadeMs * 0.001f * sr, sliceLen * 0.49f);
			float a = (float)slicePos, b = sliceLen - (float)slicePos;
			env = std::min(sliceFade(shape, a / fade), sliceFade(shape, b / fade));
		}
		dispEnv = env; dispXf = xf;

		// DEPTH crossfades the altered slice against the straight signal. With a
		// pattern doing the choosing, "how strong" is the only thing left for it
		// to mean, and it is the same knob whichever transform is running.
		float mix = clamp(params[DEPTH_PARAM].getValue()
		                  + inputs[DEPTH_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		float wetL = outL * env, wetR = outR * env;
		// LINK off decorrelates the two channels by giving the right one its own
		// roll; the transform is the same, its source is not.
		if (!linked() && (xf == XF_SHUFFLE || xf == XF_DELAY)) {
			double alt = rdPos - (double)(std::floor(rollAt(sliceIdx, 5) * 4.f) * sliceLen);
			wetR = readR(alt) * env;
		}
		outputs[L_OUTPUT].setVoltage(inL * (1.f - mix) + wetL * mix);
		outputs[R_OUTPUT].setVoltage(inR * (1.f - mix) + wetR * mix);
		outputs[SLICE_OUTPUT].setVoltage(slicePulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[XF_OUTPUT].setVoltage(xf >= 0 ? 10.f : 0.f);

		// ── advance ──────────────────────────────────────────────────────────
		slicePos += 1.0;
		if (slicePos >= (double)sliceLen) { slicePos = 0.0; sliceIdx++; }

		// ── display ──────────────────────────────────────────────────────────
		dispWrFrac = (float)wr / (float)bufN;
		dispRdFrac = (float)(((long)rdPos % bufN + bufN) % bufN) / (float)bufN;
		int bin = (int)(dispWrFrac * 128.f) & 127;
		float a = std::fabs(inL) * 0.2f;
		dispPeak[bin] = std::max(dispPeak[bin] * 0.999f, a);
	}

	bool linked() { return params[LINK_PARAM].getValue() > 0.5f; }

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "seed", json_integer((json_int_t)seed));
		json_object_set_new(r, "wholeWindow", json_boolean(wholeWindow));
		json_object_set_new(r, "fadeMs", json_real(fadeMs));
		json_object_set_new(r, "freeze", json_boolean(freeze));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "seed"))        seed = (uint32_t)json_integer_value(j);
		if (json_t* j = json_object_get(r, "wholeWindow")) wholeWindow = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "fadeMs"))      fadeMs = clamp((float)json_number_value(j), 0.5f, 25.f);
		if (json_t* j = json_object_get(r, "freeze"))      freeze = json_boolean_value(j);
	}
};

// =============================================================================
// Display — the buffer, the grid, and where this slice came from.
// =============================================================================

// Screen coordinates, in the design's own units (see panel-design.md).
static const float SD_W = 480.f;
static const float SD_M = 14.f;
static const float SD_BUFY = 20.f, SD_BUFH = 62.f;
static const float SD_ROWY = 108.f, SD_ROWH = 13.f;

void SliceDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	if (!font || font->handle < 0) font = sfs::screenFontFace();
	if (!font || font->handle < 0) return;
	nvgSave(args.vg);
	nvgScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
	if (!module) drawPreview(args);
	else         drawLive(args);
	nvgRestore(args.vg);
}

int SliceDisplay::rowAt(Vec p) const { (void)p; return -1; }
void SliceDisplay::onButton(const ButtonEvent& e) { OpaqueWidget::onButton(e); }

// The pattern, as the row of slices it is: one cell per slot, named, with the
// one playing lit. You can see the shape of the figure and where you are in it,
// which is the thing a bank of probability bars could never show.
static void slicePatRow(NVGcontext* vg, std::shared_ptr<Font> font, float s,
                        const SlicePattern& P, int playSlot, bool random) {
	int n = random ? 8 : P.len;
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s;
	float cw = (x1 - x0) / (float)n, y = SD_ROWY * s, h = SD_ROWH * 2.2f * s;
	for (int i = 0; i < n; i++) {
		int xf = random ? -2 : P.slot[i];
		bool live = (i == playSlot);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x0 + cw * (float)i + 1.f, y, cw - 2.f, h, 2.f);
		nvgFillColor(vg, live ? sfs::SCREEN_HOT
		                : xf >= 0 ? sfs::SCREEN_DEEP : nvgRGB(0x23, 0x23, 0x3C));
		nvgFill(vg);
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, live ? sfs::SCREEN_BG
		                : xf >= 0 ? sfs::SCREEN_TEXT : sfs::SCREEN_PMID);
		nvgText(vg, x0 + cw * ((float)i + 0.5f), y + h * 0.5f,
		        xf == -2 ? "?" : xf >= 0 ? SLICE_XFNAME[xf] : "\u00b7", NULL);
	}
}

void SliceDisplay::drawLive(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	Slice* m = module;
	float s = box.size.x / SD_W;

	// The buffer, as a ring flattened into a bar: the write head, and a mark
	// where the slice being played was fetched from. The line between them is
	// the module's whole idea in one picture.
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s, y = SD_BUFY * s, h = SD_BUFH * s;
	nvgBeginPath(vg); nvgRect(vg, x0, y, x1 - x0, h);
	nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20)); nvgFill(vg);
	for (int i = 0; i < 128; i++) {
		float a = clamp(m->dispPeak[i], 0.f, 1.f);
		float bx = x0 + (x1 - x0) * ((float)i / 128.f);
		float bh = h * 0.46f * a;
		nvgBeginPath(vg);
		nvgRect(vg, bx, y + h * 0.5f - bh, std::max((x1 - x0) / 128.f - 1.f, 1.f), bh * 2.f);
		nvgFillColor(vg, sfs::SCREEN_DEEP); nvgFill(vg);
	}
	float wx = x0 + (x1 - x0) * m->dispWrFrac;
	float rx = x0 + (x1 - x0) * m->dispRdFrac;
	if (m->dispXf >= 0) {
		nvgBeginPath(vg);
		nvgMoveTo(vg, rx, y + h * 0.5f); nvgLineTo(vg, wx, y + h * 0.5f);
		nvgStrokeColor(vg, nvgRGBA(0xEC, 0x65, 0x2E, 140));
		nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		nvgBeginPath(vg); nvgCircle(vg, rx, y + h * 0.5f, 2.4f * s);
		nvgFillColor(vg, sfs::SCREEN_HOT); nvgFill(vg);
	}
	nvgBeginPath(vg); nvgMoveTo(vg, wx, y); nvgLineTo(vg, wx, y + h);
	nvgStrokeColor(vg, m->freeze ? sfs::SCREEN_HOT : sfs::SCREEN_TEXT);
	nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);

	sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, m->freeze ? sfs::SCREEN_HOT : sfs::SCREEN_DIM);
	float ms = m->sliceLen / APP->engine->getSampleRate() * 1000.f;
	nvgText(vg, x0, (SD_BUFY - 8.f) * s,
	        m->freeze ? "FROZEN" : string::f("%.0f ms", ms).c_str(), NULL);

	const SlicePattern& P = SLICE_PATS[m->patIndex()];
	bool rnd = (P.len <= 0);
	int slot = rnd ? (int)(m->sliceIdx % 8) : (int)(m->sliceIdx % P.len);
	sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, sfs::SCREEN_TEXT);
	nvgText(vg, x1, (SD_BUFY - 8.f) * s, P.name, NULL);
	slicePatRow(vg, font, s, P, slot, rnd);
}

void SliceDisplay::drawPreview(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	float s = box.size.x / SD_W;
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s, y = SD_BUFY * s, h = SD_BUFH * s;
	nvgBeginPath(vg); nvgRect(vg, x0, y, x1 - x0, h);
	nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20)); nvgFill(vg);
	for (int i = 0; i < 128; i++) {
		float a = 0.25f + 0.7f * std::fabs(std::sin((float)i * 0.31f))
		                * std::fabs(std::sin((float)i * 0.07f));
		float bx = x0 + (x1 - x0) * ((float)i / 128.f), bh = h * 0.46f * a;
		nvgBeginPath(vg);
		nvgRect(vg, bx, y + h * 0.5f - bh, std::max((x1 - x0) / 128.f - 1.f, 1.f), bh * 2.f);
		nvgFillColor(vg, sfs::SCREEN_DEEP); nvgFill(vg);
	}
	float wx = x0 + (x1 - x0) * 0.72f, rx = x0 + (x1 - x0) * 0.31f;
	nvgBeginPath(vg); nvgMoveTo(vg, rx, y + h * 0.5f); nvgLineTo(vg, wx, y + h * 0.5f);
	nvgStrokeColor(vg, nvgRGBA(0xEC, 0x65, 0x2E, 140)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	nvgBeginPath(vg); nvgCircle(vg, rx, y + h * 0.5f, 2.4f * s);
	nvgFillColor(vg, sfs::SCREEN_HOT); nvgFill(vg);
	nvgBeginPath(vg); nvgMoveTo(vg, wx, y); nvgLineTo(vg, wx, y + h);
	nvgStrokeColor(vg, sfs::SCREEN_TEXT); nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);

	sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, sfs::SCREEN_DIM);
	nvgText(vg, x0, (SD_BUFY - 8.f) * s, "125 ms", NULL);

	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, sfs::SCREEN_TEXT);
	nvgText(vg, x1, (SD_BUFY - 8.f) * s, "Tumble", NULL);
	slicePatRow(vg, font, s, SLICE_PATS[9], 3, false);
}

// =============================================================================
// Panel — 22HP. Screen on top, controls in the middle, every jack at the foot.
//
// This one does NOT use the pot-over-jack pairs the rest of the plugin is laid
// out with. Slice has ten inputs and only three of them modulate a knob, so
// pairing would scatter the audio and clock jacks among the controls and leave
// the pairs that do exist looking accidental. Banding it instead — read the
// screen, set the sound, patch the edges — suits a module you set up once and
// then play from one or two cables.
// =============================================================================

struct SliceWidget : ModuleWidget {
	SliceWidget(Slice* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/slice.svg")));
		using sfs::hp;

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(hp(1), hp(1.6f), "SLICE");

		SliceDisplay* disp = new SliceDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(hp(1), hp(2.4f)));
		disp->box.size = mm2px(Vec(hp(19), hp(6)));
		addChild(disp);

		// ── controls ─────────────────────────────────────────────────────────
		const float kx[4] = {hp(3), hp(8), hp(14), hp(19)};
		const float ky1 = hp(11), ky2 = hp(14);
		// PATTERN is the module's one big decision, so it gets the first knob.
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(kx[0], ky1)), module, Slice::PATTERN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[1], ky1)), module, Slice::LENGTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[2], ky1)), module, Slice::DEPTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[3], ky1)), module, Slice::RANGE_PARAM));
		lbl->knob(kx[0], ky1, "PATTERN");
		lbl->knob(kx[1], ky1, "LENGTH");
		lbl->knob(kx[2], ky1, "DEPTH");
		lbl->knob(kx[3], ky1, "REACH");

		addParam(createParamCentered<Trimpot>(mm2px(Vec(kx[1], ky2)), module, Slice::DIV_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(kx[2], ky2)), module, Slice::SHAPE_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(kx[3], ky2)), module, Slice::LINK_PARAM));
		lbl->trim(kx[1], ky2, "DIV");
		lbl->trim(kx[2], ky2, "SHAPE");
		lbl->trim(kx[3], ky2, "LINK");

		const float by = hp(17.4f);
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(hp(6.5f), by)), module, Slice::FREEZE_PARAM, Slice::FREEZE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(hp(15.5f), by)), module, Slice::RESEED_PARAM));
		lbl->trim(hp(6.5f), by, "FREEZE");
		lbl->trim(hp(15.5f), by, "RESEED");

		// ── jacks, all of them, along the foot ───────────────────────────────
		const float jy1 = hp(20.6f), jy2 = hp(23.1f);
		const float jx0 = hp(1.8f), jdx = hp(2.62f);
		struct J { int id; bool out; const char* name; };
		static const J ROW1[7] = {
			{Slice::L_INPUT, false, "L"}, {Slice::R_INPUT, false, "R"},
			{Slice::CLOCK_INPUT, false, "CLK"}, {Slice::BAR_INPUT, false, "BAR"},
			{Slice::RESET_INPUT, false, "RST"}, {Slice::FREEZE_INPUT, false, "FRZ"},
			{Slice::RESEED_INPUT, false, "SEED"},
		};
		static const J ROW2[7] = {
			{Slice::PATTERN_INPUT, false, "PAT"}, {Slice::DEPTH_INPUT, false, "DPTH"},
			{Slice::LENGTH_INPUT, false, "LEN"},
			{Slice::L_OUTPUT, true, "L"}, {Slice::R_OUTPUT, true, "R"},
			{Slice::SLICE_OUTPUT, true, "TRIG"}, {Slice::XF_OUTPUT, true, "GATE"},
		};
		for (int i = 0; i < 7; i++) {
			float x = jx0 + jdx * (float)i;
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, jy1)), module, ROW1[i].id));
			lbl->jack(x, jy1, ROW1[i].name);
		}
		for (int i = 0; i < 7; i++) {
			float x = jx0 + jdx * (float)i;
			if (ROW2[i].out)
				addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, jy2)), module, ROW2[i].id));
			else
				addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, jy2)), module, ROW2[i].id));
			if (ROW2[i].out) lbl->jackOnPlate(x, jy2, ROW2[i].name);
			else             lbl->jack(x, jy2, ROW2[i].name);
		}
	}

	void appendContextMenu(Menu* menu) override {
		Slice* m = dynamic_cast<Slice*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);
		// The granular reading of "window shape": envelope the whole slice
		// rather than just its edges. Nothing passes through clean then, which
		// is the point -- it becomes a gater with a shape control.
		menu->addChild(createBoolPtrMenuItem("Envelope the whole slice", "", &m->wholeWindow));
		menu->addChild(createSubmenuItem("Edge fade", string::f("%.0f ms", m->fadeMs),
			[=](Menu* sub) {
				static const float MS[5] = {1.f, 2.f, 4.f, 8.f, 16.f};
				for (int i = 0; i < 5; i++) {
					float v = MS[i];
					sub->addChild(createCheckMenuItem(string::f("%.0f ms", v), "",
						[=]() { return std::fabs(m->fadeMs - v) < 0.01f; },
						[=]() { m->fadeMs = v; }));
				}
			}));
	}
};

Model* modelSlice = createModel<Slice, SliceWidget>("Slice");
