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
// WHAT happens and HOW OFTEN it happens are two knobs, not one.
//
// The first cut rolled seven weights per slice, which was undifferentiated
// mush. The second replaced it with twelve named patterns, which was
// repeatable but welded the two questions together: "Gate 2" and "Gate 4" were
// separate entries for one effect at two rates, and everything interesting was
// stuck at every-4. Twelve entries could not cover seven effects times eight
// rates, and the ones it did cover were an arbitrary sample of them.
//
// So: EFFECT picks one of the seven, or MIXED to rotate through them, and
// EVERY says how many slices go by between firings. Every combination exists,
// each is a figure you can learn, and the old patterns are all still in there
// as pairs -- Gate 4 is CUT every 4, Stutter is REPEAT every 4, Tumble is MIXED
// every 2.
static const int SLICE_MIXED = SLICE_NXF;         // one past the last real effect
static const char* SLICE_EFFNAME[SLICE_NXF + 1] =
	{"CUT", "SWAP", "DELAY", "SHUFFLE", "REVERSE", "REPEAT", "PITCH", "MIXED"};

// How many slices pass between firings. Not a plain 1..16 count: the useful
// rates are sparse at the top end and a knob that spends half its travel
// between 12 and 16 is wasted.
static const int SLICE_EVERY[] = {1, 2, 3, 4, 6, 8, 12, 16};
static const int SLICE_NEVERY = (int)(sizeof(SLICE_EVERY) / sizeof(SLICE_EVERY[0]));

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
		EFFECT_PARAM, EVERY_PARAM, LENGTH_PARAM, DEPTH_PARAM,
		RANGE_PARAM, DIV_PARAM, SHAPE_PARAM, LINK_PARAM,
		FREEZE_PARAM, RESEED_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		L_INPUT, R_INPUT, CLOCK_INPUT, BAR_INPUT, RESET_INPUT,
		FREEZE_INPUT, RESEED_INPUT,
		EFFECT_INPUT, EVERY_INPUT, DEPTH_INPUT, LENGTH_INPUT, RANGE_INPUT,
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
	// REPEAT is the one transform that splices inside a slice: every time it
	// wraps back to the start of its fragment the waveform jumps, and unless the
	// fragment happens to begin and end near zero that jump is a click. CLEAN
	// puts a very short fade either side of the wrap; DIRTY leaves it, because
	// the click is a perfectly good sound and it is most of what makes a stutter
	// sound like a stutter rather than like a loop.
	int   spliceMode = 0;             // 0 = clean, 1 = dirty
	static constexpr float SPLICE_MS = 1.5f;
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
	float  dispBack = 0.f;              // read head's distance behind now, samples

	// Two traces of the slice in progress, at column resolution: what came in,
	// and what the transform made of it. Written in place and swept, the way a
	// scope sweeps, so the columns ahead of the cursor still hold the previous
	// slice and the picture is never half empty.
	static const int SCOPE = 256;
	float scDryMin[SCOPE] = {}, scDryMax[SCOPE] = {};
	float scWetMin[SCOPE] = {}, scWetMax[SCOPE] = {};
	float scEnv[SCOPE] = {};
	int   scCol = -1;

	Slice() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		std::vector<std::string> en;
		for (int i = 0; i <= SLICE_NXF; i++) en.push_back(SLICE_EFFNAME[i]);
		configSwitch(EFFECT_PARAM, 0.f, (float)SLICE_NXF, 0.f, "Effect", en);
		std::vector<std::string> ev;
		for (int i = 0; i < SLICE_NEVERY; i++)
			ev.push_back(SLICE_EVERY[i] == 1 ? "Every slice"
			                                 : string::f("Every %d", SLICE_EVERY[i]));
		configSwitch(EVERY_PARAM, 0.f, (float)(SLICE_NEVERY - 1), 3.f, "Rate", ev);
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
		configInput(EFFECT_INPUT, "Effect CV (1V per effect)");
		configInput(EVERY_INPUT, "Rate CV (1V per step)");
		configInput(RANGE_INPUT, "Reach CV (1V per step or bar)");
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

	// REACH is a COUNT, not a fraction of the buffer. As a percentage of thirty
	// seconds it was unreadable and unusable: the knob's whole lower half
	// rounded to the same one or two slices, and no position on it corresponded
	// to anything you could name. In steps, or in bars when a BAR clock is
	// there, every position is a number you can say out loud.
	static const int REACH_MAXSTEP = 32;
	static const int REACH_MAXBAR  = 8;
	bool reachInBars() { return barLen > 0.f && inputs[BAR_INPUT].isConnected(); }
	int reachCount() {
		float v = clamp(params[RANGE_PARAM].getValue()
		                + inputs[RANGE_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		int hi = reachInBars() ? REACH_MAXBAR : REACH_MAXSTEP;
		return clamp(1 + (int)std::floor(v * (float)hi), 1, hi);
	}
	// ...and in samples, for the read head.
	float reachSamples() {
		float unit = reachInBars() ? barLen : std::max(sliceLen, 1.f);
		float back = unit * (float)reachCount();
		float have = std::min((float)bufN, (float)written) - sliceLen * 2.f;
		return clamp(back, sliceLen, std::max(have, sliceLen));
	}

	int effIndex() {
		int p = (int)std::round(params[EFFECT_PARAM].getValue()
		                        + inputs[EFFECT_INPUT].getVoltage());
		return clamp(p, 0, SLICE_NXF);
	}
	int everyN() {
		int p = (int)std::round(params[EVERY_PARAM].getValue()
		                        + inputs[EVERY_INPUT].getVoltage());
		return SLICE_EVERY[clamp(p, 0, SLICE_NEVERY - 1)];
	}

	// Choose what happens to slot `idx`, and set the read head up for it.
	void beginSlice(long idx) {
		xf = -1; swapCh = false; rdRate = 1.0; repLen = 0;

		int n = everyN();
		// The LAST slice of each group is the one that fires, so the effect
		// lands on the approach to the downbeat rather than on it.
		if (n > 1 && (idx % n) != (long)(n - 1)) return;
		int e = effIndex();
		if (e == SLICE_MIXED) {
			// MIXED walks the seven in order rather than rolling: still a
			// figure, just a longer one. RESEED rotates where it starts.
			xf = (int)(((idx / n) + (long)(seed % SLICE_NXF)) % SLICE_NXF);
		} else {
			xf = e;
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
			case XF_DELAY:
				// Exactly REACH back: N steps, or N bars when there is a bar.
				rdPos = now - (double)reach; break;
			case XF_SHUFFLE: {
				// Somewhere inside REACH, on the slice grid so it still lands.
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
			long p = (into < repLen) ? into : (into % repLen);
			rdPos = (double)((repStart + p) % bufN);
			outL = readL(rdPos); outR = readR(rdPos);
			if (spliceMode == 0 && repLen > 8) {
				// A raised cosine, NOT the SHAPE curve. SHAPE is allowed to be
				// Square, which means "no fade" and is chosen for the click at
				// the slice edges -- but a clean splice has to stay clean
				// whatever the edges are doing.
				float f = std::min(SPLICE_MS * 0.001f * sr, (float)repLen * 0.25f);
				float a = std::min((float)p / f, 1.f);
				float b = std::min((float)(repLen - p) / f, 1.f);
				float g = 0.5f * (1.f - std::cos(std::min(a, b) * (float)M_PI));
				outL *= g; outR *= g;
			}
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

		// The two traces. dry is what arrived, wet is what the transform made of
		// it WITH its envelope but BEFORE the DEPTH crossfade -- so the altered
		// window is visible as itself even at low depth, rather than fading out
		// of the picture along with the effect.
		int col = clamp((int)(slicePos / (double)sliceLen * (double)SCOPE), 0, SCOPE - 1);
		float dry = (inL + inR) * 0.5f, wet = (wetL + wetR) * 0.5f;
		if (col != scCol) {
			scCol = col;
			scDryMin[col] = scDryMax[col] = dry;
			scWetMin[col] = scWetMax[col] = wet;
			scEnv[col] = env;
		} else {
			scDryMin[col] = std::min(scDryMin[col], dry);
			scDryMax[col] = std::max(scDryMax[col], dry);
			scWetMin[col] = std::min(scWetMin[col], wet);
			scWetMax[col] = std::max(scWetMax[col], wet);
			scEnv[col]    = std::max(scEnv[col], env);
		}

		// ── advance ──────────────────────────────────────────────────────────
		slicePos += 1.0;
		if (slicePos >= (double)sliceLen) { slicePos = 0.0; sliceIdx++; }

		// ── display ──────────────────────────────────────────────────────────
		// How far behind the write head the current slice is reading, in
		// samples. The screen is drawn from THIS rather than from an absolute
		// buffer position: now is always the right-hand edge.
		double back = (double)wr - rdPos;
		while (back < 0.0) back += (double)bufN;
		dispBack = (float)back;

	}

	bool linked() { return params[LINK_PARAM].getValue() > 0.5f; }

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "seed", json_integer((json_int_t)seed));
		json_object_set_new(r, "wholeWindow", json_boolean(wholeWindow));
		json_object_set_new(r, "fadeMs", json_real(fadeMs));
		json_object_set_new(r, "freeze", json_boolean(freeze));
		json_object_set_new(r, "spliceMode", json_integer(spliceMode));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "seed"))        seed = (uint32_t)json_integer_value(j);
		if (json_t* j = json_object_get(r, "wholeWindow")) wholeWindow = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "fadeMs"))      fadeMs = clamp((float)json_number_value(j), 0.5f, 25.f);
		if (json_t* j = json_object_get(r, "freeze"))      freeze = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "spliceMode")) spliceMode = clamp((int)json_integer_value(j), 0, 1);
	}
};

// =============================================================================
// Display — the buffer, the grid, and where this slice came from.
// =============================================================================

// Screen coordinates, in the design's own units (see panel-design.md).
static const float SD_W = 480.f;
static const float SD_M = 14.f;
static const float SD_BUFY = 20.f, SD_BUFH = 50.f;
// The two slice panes: what came in, and what went out.
static const float SD_PANE1 = 78.f, SD_PANE2 = 112.f, SD_PANEH = 30.f;

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

// ONE SLICE, TWICE: the window that arrived and the window that left.
//
// This replaced a row of labelled cells. A cell could tell you that slice 4 was
// reversed; it could not show you that the reversal is exact, where the edge
// fades sit, that a repeat is looping the first quarter, or that a pitched
// slice runs out halfway and holds. Drawn as the actual samples, all of that is
// just visible.
//
// The traces are swept in place rather than scrolled, so the columns to the
// right of the cursor still hold the previous slice: the pane is always full.
static void slicePane(NVGcontext* vg, std::shared_ptr<Font> font, float s,
                      float uy, const float* mn, const float* mx, const float* env,
                      int cursor, const char* label, NVGcolor col, bool showEnv) {
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s;
	float y = uy * s, h = SD_PANEH * s, mid = y + h * 0.5f;
	nvgBeginPath(vg); nvgRect(vg, x0, y, x1 - x0, h);
	nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20)); nvgFill(vg);

	// zero line
	nvgBeginPath(vg); nvgMoveTo(vg, x0, mid); nvgLineTo(vg, x1, mid);
	nvgStrokeColor(vg, nvgRGBA(0x8A, 0x8A, 0xA5, 45)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

	// the envelope the slice is being shaped by, as an outline behind the trace
	if (showEnv) {
		nvgBeginPath(vg);
		for (int i = 0; i < Slice::SCOPE; i++) {
			float ex = x0 + (x1 - x0) * ((float)i / (float)Slice::SCOPE);
			float ey = mid - h * 0.46f * clamp(env[i], 0.f, 1.f);
			if (i == 0) nvgMoveTo(vg, ex, ey); else nvgLineTo(vg, ex, ey);
		}
		for (int i = Slice::SCOPE - 1; i >= 0; i--) {
			float ex = x0 + (x1 - x0) * ((float)i / (float)Slice::SCOPE);
			float ey = mid + h * 0.46f * clamp(env[i], 0.f, 1.f);
			nvgLineTo(vg, ex, ey);
		}
		nvgClosePath(vg);
		nvgStrokeColor(vg, nvgRGBA(0x8A, 0x8A, 0xA5, 70));
		nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	}

	// min/max per column: a real waveform, not an envelope follower
	const float VSCALE = 0.46f / 5.f;              // ±5V fills the pane
	nvgBeginPath(vg);
	for (int i = 0; i < Slice::SCOPE; i++) {
		float cx = x0 + (x1 - x0) * ((float)i / (float)Slice::SCOPE);
		float a = mid - h * clamp(mx[i] * VSCALE, -0.46f, 0.46f);
		float b = mid - h * clamp(mn[i] * VSCALE, -0.46f, 0.46f);
		if (b - a < 1.f) b = a + 1.f;
		nvgMoveTo(vg, cx, a); nvgLineTo(vg, cx, b);
	}
	nvgStrokeColor(vg, col); nvgStrokeWidth(vg, std::max((x1 - x0) / Slice::SCOPE, 1.f));
	nvgStroke(vg);

	// the sweep
	if (cursor >= 0) {
		float cx = x0 + (x1 - x0) * ((float)cursor / (float)Slice::SCOPE);
		nvgBeginPath(vg); nvgMoveTo(vg, cx, y); nvgLineTo(vg, cx, y + h);
		nvgStrokeColor(vg, nvgRGBA(0xE8, 0xE8, 0xF0, 120));
		nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	}

	sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
	nvgFillColor(vg, sfs::SCREEN_DIM);
	nvgText(vg, x0 + 3.f * s, y + 2.f * s, label, NULL);
}

void SliceDisplay::drawLive(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	Slice* m = module;
	float s = box.size.x / SD_W;
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s, y = SD_BUFY * s, h = SD_BUFH * s;

	// NOW IS THE RIGHT-HAND EDGE, always. The first version drew the whole
	// thirty seconds with the write head sweeping across it, which told you
	// where in a buffer you happened to be -- a fact about the implementation,
	// not about the sound. What you actually want to see is the recent past
	// scrolling in from the left and how far back the current slice reached,
	// so the window is exactly REACH wide and pinned to the present.
	float win = std::max(m->reachSamples(), m->sliceLen * 4.f);
	nvgBeginPath(vg); nvgRect(vg, x0, y, x1 - x0, h);
	nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20)); nvgFill(vg);

	// Peaks read straight out of the buffer at whatever resolution the window
	// needs, rather than from a fixed set of bins that would be far too coarse
	// at a one-second window and far too fine at eight bars.
	const int NB = 120;
	long step = std::max(1L, (long)(win / (float)NB));
	for (int i = 0; i < NB; i++) {
		long start = m->wr - (long)win + (long)((float)i / NB * win);
		float pk = 0.f;
		for (long k = 0; k < step; k += std::max(1L, step / 12)) {
			long a = ((start + k) % m->bufN + m->bufN) % m->bufN;
			pk = std::max(pk, std::fabs(m->bufL[(size_t)a]));
		}
		float bh = h * 0.46f * clamp(pk * 0.2f, 0.f, 1.f);
		float bx = x0 + (x1 - x0) * ((float)i / NB);
		nvgBeginPath(vg);
		nvgRect(vg, bx, y + h * 0.5f - bh, std::max((x1 - x0) / NB - 1.f, 1.f), bh * 2.f);
		nvgFillColor(vg, sfs::SCREEN_DEEP); nvgFill(vg);
	}

	// The slice grid, so the window reads as steps rather than as seconds.
	int grid = (int)std::min(64.f, win / std::max(m->sliceLen, 1.f));
	for (int i = 1; i <= grid; i++) {
		float gx = x1 - (x1 - x0) * ((float)i * m->sliceLen / win);
		if (gx < x0) break;
		nvgBeginPath(vg); nvgMoveTo(vg, gx, y + h * 0.72f); nvgLineTo(vg, gx, y + h);
		nvgStrokeColor(vg, nvgRGBA(0x8A, 0x8A, 0xA5, 60));
		nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	}

	if (m->dispXf >= 0 && m->dispBack > 1.f) {
		float rx = x1 - (x1 - x0) * clamp(m->dispBack / win, 0.f, 1.f);
		nvgBeginPath(vg); nvgMoveTo(vg, rx, y + h * 0.5f); nvgLineTo(vg, x1, y + h * 0.5f);
		nvgStrokeColor(vg, nvgRGBA(0xEC, 0x65, 0x2E, 150));
		nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		nvgBeginPath(vg); nvgCircle(vg, rx, y + h * 0.5f, 2.4f * s);
		nvgFillColor(vg, sfs::SCREEN_HOT); nvgFill(vg);
	}
	nvgBeginPath(vg); nvgMoveTo(vg, x1, y); nvgLineTo(vg, x1, y + h);
	nvgStrokeColor(vg, m->freeze ? sfs::SCREEN_HOT : sfs::SCREEN_TEXT);
	nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);

	sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, m->freeze ? sfs::SCREEN_HOT : sfs::SCREEN_DIM);
	float ms = m->sliceLen / APP->engine->getSampleRate() * 1000.f;
	nvgText(vg, x0, (SD_BUFY - 8.f) * s,
	        m->freeze ? "FROZEN" : string::f("%.0f ms", ms).c_str(), NULL);
	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, sfs::SCREEN_TEXT);
	int rc = m->reachCount();
	nvgText(vg, x1, (SD_BUFY - 8.f) * s,
	        string::f("%s /%d  \u2190%d %s", SLICE_EFFNAME[m->effIndex()], m->everyN(),
	                  rc, m->reachInBars() ? (rc == 1 ? "bar" : "bars")
	                                       : (rc == 1 ? "step" : "steps")).c_str(), NULL);

	slicePane(vg, font, s, SD_PANE1, m->scDryMin, m->scDryMax, m->scEnv,
	          m->scCol, "IN", sfs::SCREEN_DEEP, false);
	slicePane(vg, font, s, SD_PANE2, m->scWetMin, m->scWetMax, m->scEnv,
	          m->scCol, m->dispXf >= 0 ? SLICE_EFFNAME[m->dispXf] : "OUT",
	          m->dispXf >= 0 ? sfs::SCREEN_HOT : sfs::SCREEN_BLUE, true);
}

void SliceDisplay::drawPreview(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	float s = box.size.x / SD_W;
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s, y = SD_BUFY * s, h = SD_BUFH * s;
	nvgBeginPath(vg); nvgRect(vg, x0, y, x1 - x0, h);
	nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20)); nvgFill(vg);
	for (int i = 0; i < 120; i++) {
		float a = 0.25f + 0.7f * std::fabs(std::sin((float)i * 0.31f))
		                * std::fabs(std::sin((float)i * 0.07f));
		float bx = x0 + (x1 - x0) * ((float)i / 120.f), bh = h * 0.46f * a;
		nvgBeginPath(vg);
		nvgRect(vg, bx, y + h * 0.5f - bh, std::max((x1 - x0) / 120.f - 1.f, 1.f), bh * 2.f);
		nvgFillColor(vg, sfs::SCREEN_DEEP); nvgFill(vg);
	}
	for (int i = 1; i <= 8; i++) {
		float gx = x1 - (x1 - x0) * ((float)i / 8.f);
		nvgBeginPath(vg); nvgMoveTo(vg, gx, y + h * 0.72f); nvgLineTo(vg, gx, y + h);
		nvgStrokeColor(vg, nvgRGBA(0x8A, 0x8A, 0xA5, 60)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	}
	float rx = x1 - (x1 - x0) * 0.62f;
	nvgBeginPath(vg); nvgMoveTo(vg, rx, y + h * 0.5f); nvgLineTo(vg, x1, y + h * 0.5f);
	nvgStrokeColor(vg, nvgRGBA(0xEC, 0x65, 0x2E, 150)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
	nvgBeginPath(vg); nvgCircle(vg, rx, y + h * 0.5f, 2.4f * s);
	nvgFillColor(vg, sfs::SCREEN_HOT); nvgFill(vg);
	nvgBeginPath(vg); nvgMoveTo(vg, x1, y); nvgLineTo(vg, x1, y + h);
	nvgStrokeColor(vg, sfs::SCREEN_TEXT); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);

	sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, sfs::SCREEN_DIM);
	nvgText(vg, x0, (SD_BUFY - 8.f) * s, "125 ms", NULL);
	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, sfs::SCREEN_TEXT);
	nvgText(vg, x1, (SD_BUFY - 8.f) * s, "REVERSE /4  \u21904 steps", NULL);

	// A slice reversed, drawn as it would really look: the same waveform,
	// mirrored, inside its edge fades.
	static float dmn[Slice::SCOPE], dmx[Slice::SCOPE];
	static float wmn[Slice::SCOPE], wmx[Slice::SCOPE], wev[Slice::SCOPE];
	for (int i = 0; i < Slice::SCOPE; i++) {
		float t = (float)i / (float)Slice::SCOPE;
		float a = std::sin(t * 47.f) * (0.25f + 0.75f * std::exp(-t * 2.6f)) * 4.f;
		dmx[i] = std::fabs(a); dmn[i] = -std::fabs(a) * 0.85f;
		int j = Slice::SCOPE - 1 - i;                       // reversed
		float e = clamp(std::min(t, 1.f - t) * 14.f, 0.f, 1.f);
		float b = std::sin((float)j / Slice::SCOPE * 47.f)
		        * (0.25f + 0.75f * std::exp(-(float)j / Slice::SCOPE * 2.6f)) * 4.f;
		wmx[i] = std::fabs(b) * e; wmn[i] = -std::fabs(b) * 0.85f * e;
		wev[i] = e;
	}
	slicePane(vg, font, s, SD_PANE1, dmn, dmx, wev, 168, "IN", sfs::SCREEN_DEEP, false);
	slicePane(vg, font, s, SD_PANE2, wmn, wmx, wev, 168, "REVERSE", sfs::SCREEN_HOT, true);
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
		// WHAT and HOW OFTEN, side by side, because they are the two questions.
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(kx[0], ky1)), module, Slice::EFFECT_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(kx[1], ky1)), module, Slice::EVERY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[2], ky1)), module, Slice::LENGTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[3], ky1)), module, Slice::DEPTH_PARAM));
		// A large knob is 12.7mm across, so the standard label gap puts the text
		// inside it. These two get their own.
		lbl->add(kx[0], ky1 - 8.4f, "EFFECT");
		lbl->add(kx[1], ky1 - 8.4f, "EVERY");
		lbl->knob(kx[2], ky1, "LENGTH");
		lbl->knob(kx[3], ky1, "DEPTH");

		addParam(createParamCentered<Trimpot>(mm2px(Vec(kx[0], ky2)), module, Slice::RANGE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(kx[1], ky2)), module, Slice::DIV_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(kx[2], ky2)), module, Slice::SHAPE_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(kx[3], ky2)), module, Slice::LINK_PARAM));
		lbl->trim(kx[0], ky2, "REACH");
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
		const float jx0 = hp(1.9f), jdx = hp(2.6f);
		struct J { int id; bool out; const char* name; };
		static const J ROW1[8] = {
			{Slice::L_INPUT, false, "L"}, {Slice::R_INPUT, false, "R"},
			{Slice::CLOCK_INPUT, false, "CLK"}, {Slice::BAR_INPUT, false, "BAR"},
			{Slice::RESET_INPUT, false, "RST"}, {Slice::FREEZE_INPUT, false, "FRZ"},
			{Slice::RESEED_INPUT, false, "SEED"}, {Slice::RANGE_INPUT, false, "RCH"},
		};
		static const J ROW2[8] = {
			{Slice::EFFECT_INPUT, false, "EFF"}, {Slice::EVERY_INPUT, false, "EVRY"},
			{Slice::DEPTH_INPUT, false, "DPTH"}, {Slice::LENGTH_INPUT, false, "LEN"},
			{Slice::L_OUTPUT, true, "L"}, {Slice::R_OUTPUT, true, "R"},
			{Slice::SLICE_OUTPUT, true, "TRIG"}, {Slice::XF_OUTPUT, true, "GATE"},
		};
		for (int i = 0; i < 8; i++) {
			float x = jx0 + jdx * (float)i;
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, jy1)), module, ROW1[i].id));
			lbl->jack(x, jy1, ROW1[i].name);
		}
		for (int i = 0; i < 8; i++) {
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
		// REPEAT is the only transform that splices inside a slice, so this is
		// the only place a click can come from that the edge fades do not cover.
		menu->addChild(createIndexPtrSubmenuItem("Repeat splice",
			{"Clean", "Dirty"}, &m->spliceMode));
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
