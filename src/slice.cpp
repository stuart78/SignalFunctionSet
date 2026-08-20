#include "plugin.hpp"
#include "panel-style.hpp"
#include <algorithm>
#include <atomic>
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
// How much audio to keep. 60s is the default because REACH counts up to 32
// bars, and at 120 BPM that is 64 seconds -- with a 30s buffer two thirds of
// that knob's travel was unreachable at ordinary tempos. 120s costs 44 MB at
// 48k and 176 MB at 192k, per instance, which is why it is a choice.
static const float SLICE_BUFOPT[] = {30.f, 60.f, 120.f};
static const int   SLICE_NBUFOPT = 3;
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
// RATIO says how many slices go by between firings. Every combination exists,
// each is a figure you can learn, and the old patterns are all still in there
// as pairs -- Gate 4 is CUT every 4, Stutter is REPEAT every 4, Tumble is MIXED
// every 2.
static const int SLICE_MIXED = SLICE_NXF;         // one past the last real effect
static const char* SLICE_EFFNAME[SLICE_NXF + 1] =
	{"CUT", "SWAP", "DELAY", "SHUFFLE", "REVERSE", "REPEAT", "PITCH", "MIXED"};

// How many slices pass between firings. Not a plain 1..16 count: the useful
// rates are sparse at the top end and a knob that spends half its travel
// between 12 and 16 is wasted.
static const int SLICE_RATIO[] = {1, 2, 3, 4, 6, 8, 12, 16};
static const int SLICE_NRATIO = (int)(sizeof(SLICE_RATIO) / sizeof(SLICE_RATIO[0]));

// Slice length as a multiple of the clock interval. "/2" means the slice spans
// two clocks (half the rate); "x2" means two slices per clock. x1 sits dead
// centre so the knob reads the way a clock divider is expected to.
static const char* SLICE_DIVNAME[] = {"/8", "/4", "/2", "x1", "x2", "x4", "x8"};
static const float SLICE_DIVMUL[]  = {8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f, 0.125f};
static const int   SLICE_NDIV = (int)(sizeof(SLICE_DIVMUL) / sizeof(SLICE_DIVMUL[0]));

// THREE curves, because as windows there are only three.
//
// This control has been wrong three times, each time one level deeper.
// Square returned 1.0 unconditionally, so a "shape" silently switched the
// envelope off -- a hidden mode. Its replacement was inaudible: Gaussian(0.4),
// Hann and smoothstep sat within 0.01 of each other. Respacing them by gain
// ratio did not help either, because a 27 dB ratio between two silences is
// still silence.
//
// The actual mistake was a category error. A fade is an EDGE TAPER, and the ear
// hears how long a taper is, not how it curves. A window function is the WHOLE
// ARC, and the family name only describes something audible when the window
// spans the slice -- which is what WINDOW at its top now does.
//
// And once they ARE windows, what separates them is WIDTH, not pedigree.
// Blackman, Hann, Sinc and Log are all wide smooth arcs; measured across a
// slice they keep 70 / 80 / 91 / 97 percent of it above -20 dB, which is four
// names for one window. Gaussian is distinct only because sigma is narrowed to
// 0.22. So the set is three genuinely different widths and nothing that merely
// sounds like a fourth:
//
//    Gaussian  47% of the slice loud   a blip in the middle of its slot
//    Hann      80%                     the classic arc
//    Log       97%                     nearly the whole slice, edges taken off
//
// Weakest gap 17 points, against 5.8 for the pair that was cut.
enum SliceShape { SH_GAUSS, SH_HANN, SH_LOG, SH_COUNT };
static const char* SLICE_SHAPENAME[SH_COUNT] = {"Gaussian", "Hann", "Log"};

// The curve, over t in 0..1 rising into the slice. Each really reaches 0 at t=0
// and 1 at t=1: a curve that starts at 0.04 is a step of 0.04, which is the
// whole problem these exist to avoid.
static inline float sliceFade(int shape, float t) {
	t = clamp(t, 0.f, 1.f);
	switch (shape) {
		case SH_GAUSS: {
			// Sigma 0.22, not the textbook 0.4 -- at 0.4 it lands on top of Hann.
			// Narrowed, it concentrates the slice into its middle half, which is
			// the one thing here the ear reads instantly. A Gaussian never truly
			// reaches zero, so it is shifted and rescaled until it does.
			const float S = 0.22f;
			const float E = std::exp(-0.5f / (S * S));      // its value at t = 0
			float x = 1.f - t;
			return (std::exp(-0.5f * x * x / (S * S)) - E) / (1.f - E);
		}
		case SH_LOG:
			// A cubic attack: as a taper it is nearly immediate without being a
			// step, and as a window it is close to rectangular with the corners
			// taken off. This is the honest version of what Square was for.
			return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
		default:
			return 0.5f * (1.f - std::cos(t * (float)M_PI));   // Hann
	}
}

struct Slice;

// REACH counts steps, or bars when a BAR clock is patched, and the unit changes
// under the user's feet. A fixed unit string in the tooltip would be wrong half
// the time, and a bare percentage was wrong all of it: as a fraction of a
// thirty-second buffer the knob's whole lower half rounded to the same one or
// two slices and no position on it named anything.
struct SliceReachQuantity : ParamQuantity {
	std::string getUnit() override;
	std::string getDescription() override;
};

// WINDOW reads as the fade TIME it is actually applying, not as a percentage.
// A percentage said nothing about whether the setting was usable, and the
// bottom of the knob was a millisecond -- a quarter of one cycle at 261 Hz,
// where no fade curve is distinguishable from a hard edge.
struct SliceWindowQuantity : ParamQuantity {
	std::string getDisplayValueString() override;
};

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
		EFFECT_PARAM, RATIO_PARAM, LENGTH_PARAM, DEPTH_PARAM,
		RANGE_PARAM, DIV_PARAM, SHAPE_PARAM, LINK_PARAM,
		FREEZE_PARAM, RESEED_PARAM, WINDOW_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		L_INPUT, R_INPUT, CLOCK_INPUT, BAR_INPUT, RESET_INPUT,
		FREEZE_INPUT, RESEED_INPUT,
		EFFECT_INPUT, RATIO_INPUT, DEPTH_INPUT, LENGTH_INPUT, RANGE_INPUT,
		INPUTS_LEN
	};
	enum OutputId { L_OUTPUT, R_OUTPUT, SLICE_OUTPUT, XF_OUTPUT, OUTPUTS_LEN };
	enum LightId  { FREEZE_LIGHT, LIGHTS_LEN };

	// ── the buffer ───────────────────────────────────────────────────────────
	std::vector<float> bufL, bufR;
	long  bufN = 0;                   // capacity, samples
	int   bufSecIdx = 1;              // index into SLICE_BUFOPT; 1 == 60s

	// Changing the buffer size means allocating, and allocating on the audio
	// thread is a dropout. So the UI thread builds the new vectors and the audio
	// thread takes them with a std::vector::swap, which is a pointer exchange
	// and allocates nothing. The old memory rides back out in the pending
	// vectors and is freed by the widget, off the audio thread.
	std::vector<float> pendL, pendR;
	long pendN = 0;
	std::atomic<bool> pendReady{false}, pendDone{false};

	void requestBuffer(int idx) {
		// A handover already in flight owns pendL/pendR until the audio thread
		// has taken them; writing into them now would be writing into a vector
		// that is about to be swapped in under us.
		if (pendReady.load()) return;
		bufSecIdx = clamp(idx, 0, SLICE_NBUFOPT - 1);
		long n = (long)(SLICE_BUFOPT[bufSecIdx] * APP->engine->getSampleRate());
		if (n == bufN || n <= 0) return;
		pendL.assign((size_t)n, 0.f);
		pendR.assign((size_t)n, 0.f);
		pendN = n;
		pendDone = false;
		pendReady = true;
	}
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
	int    prevXf = -1;               // what the slice before this one did
	// Whether each END of this slice has a splice to hide. They are separate
	// questions: the start jumps if this slice or the one before it was
	// transformed, the end jumps if this slice or the NEXT one is.
	bool   spliceIn = false, spliceOut = false;
	double rdPos = 0.0;               // read head into the buffer (absolute, may be fractional)
	double rdRate = 1.0;
	long   repLen = 0;                // repeat: fragment length
	long   repStart = 0;
	bool   swapCh = false;

	// ── options ──────────────────────────────────────────────────────────────
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

	dsp::SchmittTrigger clockTrig, barTrig, resetTrig, reseedTrig, reseedBtn;
	dsp::PulseGenerator slicePulse;

	// display mirrors
	float dispEnv = 0.f;
	int   dispXf = -1;
	float  dispBack = 0.f;              // read head's distance behind now, samples
	// The fade lengths actually in force this sample. On the screen because
	// inferring them from the code turned out to be unreliable: SHAPE, WINDOW,
	// which effect is running and what its neighbours are doing all feed in, and
	// a number on the panel settles in one glance what reading the source did
	// not settle in an afternoon.
	float  dispFadeIn = 0.f, dispFadeOut = 0.f;

	// FIVE SECONDS OF HISTORY, NOW AT THE RIGHT. One peak per column per trace,
	// scrolling, the way a chart recorder works -- not one slice swept in place.
	// A single slice showed the transform in detail and told you nothing about
	// the figure it was part of; five seconds shows both, because at 125ms a
	// slice is still thirteen columns wide.
	//
	// Two traces per channel: what arrived, and WHAT LEAVES THE JACK. The second
	// one has to be the real output, post-DEPTH -- an earlier version drew the
	// pre-mix wet signal, and in CUT at full depth that meant a fat orange
	// waveform on screen against total silence in the room. A display that shows
	// a signal you cannot hear is worse than no display.
	static const int SCOPE = 512;
	static constexpr float SCOPE_SEC = 5.f;
	float scDry[2][SCOPE] = {}, scOut[2][SCOPE] = {};
	int   scHead = 0;                 // newest column
	float scAcc = 0.f;                // samples into the current column

	Slice() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		std::vector<std::string> en;
		for (int i = 0; i <= SLICE_NXF; i++) en.push_back(SLICE_EFFNAME[i]);
		configSwitch(EFFECT_PARAM, 0.f, (float)SLICE_NXF, 0.f, "Effect", en);
		std::vector<std::string> ev;
		// "1 in 4" says what the setting does; "4" leaves you to guess whether it
		// is a count, a divisor or a percentage.
		for (int i = 0; i < SLICE_NRATIO; i++)
			ev.push_back(SLICE_RATIO[i] == 1 ? "every slice"
			                                 : string::f("1 in %d slices", SLICE_RATIO[i]));
		configSwitch(RATIO_PARAM, 0.f, (float)(SLICE_NRATIO - 1), 3.f, "Ratio", ev);
		// LENGTH only decides anything when there is no clock: patch CLOCK and
		// the slice comes from the clock interval times DIV instead, and this
		// knob does nothing. The screen says which of the two is in charge.
		configParam(LENGTH_PARAM, std::log2(SLICE_MINLEN), std::log2(SLICE_MAXLEN),
		            std::log2(0.125f), "Slice length (only when CLOCK is unpatched)",
		            " s", 2.f);
		configParam(DEPTH_PARAM, 0.f, 1.f, 1.f, "Depth", "%", 0.f, 100.f);
		configParam<SliceReachQuantity>(RANGE_PARAM, 1.f, (float)REACH_MAX, 8.f,
		                                "Reach back");
		getParamQuantity(RANGE_PARAM)->snapEnabled = true;
		// x1 in the CENTRE of the knob, divide to the left, multiply to the
		// right, so the control reads like every other clock divider.
		configSwitch(DIV_PARAM, 0.f, (float)(SLICE_NDIV - 1), 3.f, "Clock rate",
		             {SLICE_DIVNAME[0], SLICE_DIVNAME[1], SLICE_DIVNAME[2],
		              SLICE_DIVNAME[3], SLICE_DIVNAME[4], SLICE_DIVNAME[5],
		              SLICE_DIVNAME[6]});
		configSwitch(SHAPE_PARAM, 0.f, (float)(SH_COUNT - 1), (float)SH_HANN,
		             "Window shape",
		             {SLICE_SHAPENAME[0], SLICE_SHAPENAME[1], SLICE_SHAPENAME[2]});
		configSwitch(LINK_PARAM, 0.f, 1.f, 1.f, "Channels", {"Independent", "Paired"});
		// WINDOW is the whole envelope story on one knob. At 0 the fade is a
		// millisecond at each end and the slice passes at unity between, so
		// nothing happens to untransformed audio. At 1 the two fades meet in the
		// middle and every slice swells and falls -- a grain window, which
		// amplitude-modulates at the slice rate whether or not anything is being
		// transformed. That pulse is the sound most people want from a slicer,
		// so it is the default, and 0 is there when you need it transparent.
		configParam<SliceWindowQuantity>(WINDOW_PARAM, 0.f, 1.f, 1.f, "Window");
		configSwitch(FREEZE_PARAM, 0.f, 1.f, 0.f, "Freeze the buffer", {"Running", "Frozen"});
		// Not just MIXED. The seed feeds every choice the module makes: which earlier
		// slice SHUFFLE grabs, how far DELAY reaches, whether PITCH goes down or up,
		// the right channel's own picks when LINK is off, and where MIXED starts.
		configButton(RESEED_PARAM, "Reseed: roll a new set of choices");

		configInput(L_INPUT, "Left audio");
		configInput(R_INPUT, "Right audio (normalled from left)");
		configInput(CLOCK_INPUT, "Clock (sets the slice length)");
		configInput(BAR_INPUT, "Bar (quantizes how far back DELAY and SHUFFLE reach)");
		configInput(RESET_INPUT, "Reset the grid and the pattern");
		configInput(FREEZE_INPUT, "Freeze gate");
		configInput(RESEED_INPUT, "Reseed trigger (new choices for SHUFFLE, DELAY, PITCH, MIXED)");
		configInput(DEPTH_INPUT, "Depth CV (±5V)");
		configInput(LENGTH_INPUT, "Slice length CV (±5V)");
		configInput(EFFECT_INPUT, "Effect CV (1V per effect)");
		configInput(RATIO_INPUT, "Ratio CV (1V per step)");
		configInput(RANGE_INPUT, "Reach CV (0.1V per step or bar)");
		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");
		configOutput(SLICE_OUTPUT, "Trigger on every slice boundary");
		configOutput(XF_OUTPUT, "Gate, high while a slice is being altered");
		configBypass(L_INPUT, L_OUTPUT);
		configBypass(R_INPUT, R_OUTPUT);
		onSampleRateChange();
	}

	void onSampleRateChange() override {
		// Called with the engine in a safe state, so this one can allocate
		// directly rather than going through the swap.
		pendReady = false; pendDone = false;
		pendL.clear(); pendR.clear();
		bufN = (long)(SLICE_BUFOPT[clamp(bufSecIdx, 0, SLICE_NBUFOPT - 1)]
		              * APP->engine->getSampleRate());
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
	// One range, 1..32, and only the UNIT changes: it is a count of steps, or a
	// count of bars once BAR is patched. Two ranges would mean three quarters of
	// the travel doing nothing in bar mode.
	static const int REACH_MAX = 32;
	bool reachInBars() { return barLen > 0.f && inputs[BAR_INPUT].isConnected(); }
	int reachCount() {
		float v = params[RANGE_PARAM].getValue()
		        + inputs[RANGE_INPUT].getVoltage() * (float)REACH_MAX * 0.1f;
		return clamp((int)std::round(v), 1, REACH_MAX);
	}
	// What the buffer can actually deliver. Thirty-two bars at 60 BPM is 128
	// seconds and the buffer holds thirty, so the request has to be capped --
	// and the screen shows both numbers when it is, rather than printing a reach
	// that is not happening.
	int reachFit() {
		float unit = reachInBars() ? barLen : std::max(sliceLen, 1.f);
		float have = std::min((float)bufN, (float)written) - sliceLen * 2.f;
		if (unit <= 0.f || have < unit) return 1;
		return clamp((int)std::floor(have / unit), 1, reachCount());
	}
	// ...and in samples, for the read head.
	float reachSamples() {
		float unit = reachInBars() ? barLen : std::max(sliceLen, 1.f);
		return std::max(unit * (float)reachFit(), sliceLen);
	}

	int effIndex() {
		int p = (int)std::round(params[EFFECT_PARAM].getValue()
		                        + inputs[EFFECT_INPUT].getVoltage());
		return clamp(p, 0, SLICE_NXF);
	}
	int ratioN() {
		int p = (int)std::round(params[RATIO_PARAM].getValue()
		                        + inputs[RATIO_INPUT].getVoltage());
		return SLICE_RATIO[clamp(p, 0, SLICE_NRATIO - 1)];
	}

	// Choose what happens to slot `idx`, and set the read head up for it.
	void beginSlice(long idx) {
		prevXf = xf;
		xf = -1; swapCh = false; rdRate = 1.0; repLen = 0;

		int n = ratioN();
		// The LAST slice of each group is the one that fires, so the effect
		// lands on the approach to the downbeat rather than on it. The rule is a
		// pure function of the index, which means the NEXT slice can be asked
		// about before it has happened -- and the END of this slice needs a fade
		// if the next one is going to jump away from it.
		bool fires     = (n <= 1) || ((idx % n)       == (long)(n - 1));
		bool nextFires = (n <= 1) || (((idx + 1) % n) == (long)(n - 1));
		spliceIn  = (prevXf >= 0) || fires;
		spliceOut = fires || nextFires;
		if (!fires) return;
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
		// Take a new buffer if one is waiting. swap() exchanges pointers, so
		// nothing is allocated or freed here.
		if (pendReady.load()) {
			bufL.swap(pendL); bufR.swap(pendR);
			bufN = pendN; wr = 0; written = 0; primed = false;
			slicePos = 0.0; xf = -1;
			pendReady = false; pendDone = true;
		}
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

		// The panel control is a VCVLightLatch, which already holds 0 or 1 -- so
		// its value IS the state. Running it through a SchmittTrigger and
		// toggling on top took two clicks to turn freeze off: the first press
		// gave a rising edge and flipped it, the release gave a falling edge and
		// did nothing, and only the next press flipped it back.
		freeze = params[FREEZE_PARAM].getValue() > 0.5f;
		params[LINK_PARAM].setValue(linkIdx ? 1.f : 0.f);
		if (inputs[FREEZE_INPUT].isConnected())
			freeze = inputs[FREEZE_INPUT].getVoltage() >= 1.f;
		lights[FREEZE_LIGHT].setBrightness(freeze ? 1.f : 0.f);

		if (reseedBtn.process(params[RESEED_PARAM].getValue() > 0.5f)
		    || reseedTrig.process(inputs[RESEED_INPUT].getVoltage(), 0.1f, 1.f))
			seed = hash32(seed ^ (uint32_t)wr);

		// ── slice length ─────────────────────────────────────────────────────
		float want;
		if (clockLen > 0.f && inputs[CLOCK_INPUT].isConnected()) {
			int d = clamp((int)std::round(params[DIV_PARAM].getValue()), 0, SLICE_NDIV - 1);
			want = clockLen * SLICE_DIVMUL[d];
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
				// A raised cosine, NOT the SHAPE curve: the repeat splice is
				// hiding a discontinuity inside a slice, which is a different
				// job from shaping its edges, and it should not change character
				// when the edge shape does.
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
		// One envelope, one control. The fade length runs exponentially from a
		// millisecond up to half the slice; at the top the two fades meet in the
		// middle and the edge fade IS the whole-slice window.
		int shape = (int)std::round(params[SHAPE_PARAM].getValue());
		float wparam = clamp(params[WINDOW_PARAM].getValue(), 0.f, 1.f);
		// WINDOW IS HOW MUCH OF THE SLICE IS WINDOW, expressed as a fade time:
		// 5 ms at the bottom, half the slice at the top.
		//
		// It ran to a fixed 200 ms ceiling until it turned out that ceiling was
		// quietly holding two different ideas apart. A fade is an EDGE TAPER, and
		// the curve of an edge taper is nearly inaudible -- the ear hears how long
		// it is, not what shape it is. A window function is the WHOLE ARC, nought
		// to one and back, and choosing between Hann and Gaussian only describes
		// something real when the window spans the thing it is windowing.
		//
		// With the ceiling in place, a 940 ms slice at WINDOW maximum still kept a
		// 540 ms plateau: the two half-curves never met, so every setting was a
		// Tukey window with a short taper and SHAPE only ever got to bend the
		// taper. Five window functions, none of which was allowed to be a window.
		// That is why they all sounded the same.
		//
		// Clamped to half the slice, the top of the knob is a true window and the
		// five shapes separate completely: measured across the whole slice they
		// run 47 / 70 / 80 / 91 / 97 percent of it above -20 dB, and 5 dB apart in
		// energy. That is a difference in how LONG each piece sounds, which is a
		// first-order thing to hear, where taper curvature is a third-order one.
		//
		// It used to run from one millisecond, which was a mistake twice over. A
		// 1 ms ramp on a 261 Hz tone is a quarter of one cycle, so no curve is
		// distinguishable from any other down there, so the shape control had
		// nothing to say at the bottom of its range. And the bottom of the knob
		// was simply an unusable setting that looked like a legitimate one.
		//
		// There used to be a longer floor for edges that gate to silence and a
		// shorter one for edges that splice between two pieces of audio. That
		// distinction is real, but 5 ms is long enough for the splice case and
		// short enough for the gate case, so one number does both and there is
		// no conditional left to get wrong.
		//
		// Past half the slice the two fades would overlap and the slice would
		// never reach full level, which is a volume drop rather than a window.
		float half = std::max(sliceLen * 0.5f, 1.f);
		float fLo  = std::min(0.005f * sr, half);
		float fHi  = std::max(half, fLo);
		float fade = fLo * std::pow(fHi / fLo, wparam);

		// An edge with nothing to hide still gets nothing: a run of untransformed
		// slices is read from the write head sample for sample and is already
		// continuous, so fading it only punches a hole in it.
		float fIn  = spliceIn  ? fade : fade * wparam;
		float fOut = spliceOut ? fade : fade * wparam;
		float ea = (float)slicePos, eb = sliceLen - (float)slicePos;
		float env = std::min(fIn  < 1.f ? 1.f : sliceFade(shape, ea / fIn),
		                     fOut < 1.f ? 1.f : sliceFade(shape, eb / fOut));
		dispEnv = env; dispXf = xf;
		dispFadeIn = fIn; dispFadeOut = fOut;

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
		float finalL = inL * (1.f - mix) + wetL * mix;
		float finalR = inR * (1.f - mix) + wetR * mix;
		outputs[L_OUTPUT].setVoltage(finalL);
		outputs[R_OUTPUT].setVoltage(finalR);
		outputs[SLICE_OUTPUT].setVoltage(slicePulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[XF_OUTPUT].setVoltage(xf >= 0 ? 10.f : 0.f);

		scDry[0][scHead] = std::max(scDry[0][scHead], std::fabs(inL));
		scDry[1][scHead] = std::max(scDry[1][scHead], std::fabs(inR));
		scOut[0][scHead] = std::max(scOut[0][scHead], std::fabs(finalL));
		scOut[1][scHead] = std::max(scOut[1][scHead], std::fabs(finalR));
		scAcc += 1.f;
		if (scAcc >= SCOPE_SEC * sr / (float)SCOPE) {
			scAcc = 0.f;
			scHead = (scHead + 1) % SCOPE;
			scDry[0][scHead] = scDry[1][scHead] = 0.f;
			scOut[0][scHead] = scOut[1][scHead] = 0.f;
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

	// The menu edits an int; the param is what the DSP and the patch use.
	int  linkIdx = 1;
	bool linked() { return params[LINK_PARAM].getValue() > 0.5f; }
	// Whether the slice length is coming from the clock or from the knob.
	bool clockedLength() { return clockLen > 0.f && inputs[CLOCK_INPUT].isConnected(); }

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "seed", json_integer((json_int_t)seed));
		json_object_set_new(r, "freeze", json_boolean(freeze));
		json_object_set_new(r, "spliceMode", json_integer(spliceMode));
		json_object_set_new(r, "bufSecIdx", json_integer(bufSecIdx));
		json_object_set_new(r, "linkIdx", json_integer(linkIdx));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "seed"))        seed = (uint32_t)json_integer_value(j);
		if (json_t* j = json_object_get(r, "freeze"))      freeze = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "spliceMode")) spliceMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(r, "linkIdx")) linkIdx = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(r, "bufSecIdx"))
			requestBuffer((int)json_integer_value(j));
	}
};

// =============================================================================
// Display — the buffer, the grid, and where this slice came from.
// =============================================================================

// Screen coordinates, in the design's own units (see panel-design.md).
static const float SD_W = 480.f;
static const float SD_M = 14.f;
static const float SD_BUFY = 20.f;
// The VERTICAL layout is derived from the box, not fixed. These were three
// constants totalling 142 virtual units, which filled a screen 30 mm tall; the
// 2026-08 panel made it 56 and the drawing simply sat in the top half with a
// band of empty navy under it. Everything below is a share of whatever height
// there actually is, so the next panel change cannot reintroduce that.
struct SliceRows { float bufY, bufH, p1Y, p2Y, paneH; };
static inline SliceRows sliceRows(float sh) {     // sh = box height in units
	SliceRows r;
	float top = SD_BUFY + 6.f, bot = sh - 6.f, avail = std::max(bot - top, 40.f);
	float gap = avail * 0.025f;
	r.bufY = top;
	r.bufH = avail * 0.45f;                       // the buffer view is the headline
	r.paneH = avail * 0.25f;
	r.p1Y = r.bufY + r.bufH + gap;
	r.p2Y = r.p1Y + r.paneH + gap;
	return r;
}

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

// FIVE SECONDS, NOW AT THE RIGHT, one pane per channel.
//
// Each pane carries both signals for its channel, and they are drawn
// DIFFERENTLY on purpose:
//
//   BLUE, FILLED    what leaves the jack. This is the sound in the room.
//   ORANGE, OUTLINE what came in. Where the two coincide the outline sits on
//                   the edge of the fill and vanishes; where they part, the
//                   orange is left tracing what the slice would have been.
//
// Both were filled at first, and CUT at full depth then showed a fat orange
// waveform against complete silence. A display that draws a signal you cannot
// hear as though you can is worse than no display, so only the audible one is
// solid now, and the source it departed from is a line.
//
// Drawn as a filled envelope, the way Phase draws a sample: trace the peak along
// the top, back along the bottom, close and fill. A column of separate bars is
// cheaper and reads as a bar chart rather than as a waveform.
static void sliceFilled(NVGcontext* vg, const float* v, int n, float x0, float x1,
                        float mid, float h, NVGcolor col, bool fill = true) {
	if (n < 2) return;
	nvgBeginPath(vg);
	for (int i = 0; i < n; i++) {
		float px = x0 + (x1 - x0) * ((float)i / (float)(n - 1));
		float a = clamp(v[i] * 0.2f, 0.f, 1.f) * h * 0.46f;
		if (i == 0) nvgMoveTo(vg, px, mid - a); else nvgLineTo(vg, px, mid - a);
	}
	for (int i = n - 1; i >= 0; i--) {
		float px = x0 + (x1 - x0) * ((float)i / (float)(n - 1));
		float a = clamp(v[i] * 0.2f, 0.f, 1.f) * h * 0.46f;
		nvgLineTo(vg, px, mid + a);
	}
	nvgClosePath(vg);
	if (fill) { nvgFillColor(vg, col); nvgFill(vg); }
	else      { nvgStrokeColor(vg, col); nvgStrokeWidth(vg, 1.f); nvgStroke(vg); }
}

static void sliceTrace(NVGcontext* vg, const float* v, int head, float x0, float x1,
                       float mid, float h, NVGcolor col, bool fill) {
	float lin[Slice::SCOPE];
	for (int i = 0; i < Slice::SCOPE; i++)          // oldest first, newest at x1
		lin[i] = v[(head + 1 + i) % Slice::SCOPE];
	sliceFilled(vg, lin, Slice::SCOPE, x0, x1, mid, h, col, fill);
}

static void slicePane(NVGcontext* vg, std::shared_ptr<Font> font, float s, float uy,
                      float uh, const float* dry, const float* out, int head,
                      const char* label) {
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s;
	float y = uy * s, h = uh * s, mid = y + h * 0.5f;
	nvgBeginPath(vg); nvgRect(vg, x0, y, x1 - x0, h);
	nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20)); nvgFill(vg);
	nvgBeginPath(vg); nvgMoveTo(vg, x0, mid); nvgLineTo(vg, x1, mid);
	nvgStrokeColor(vg, nvgRGBA(0x8A, 0x8A, 0xA5, 40)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

	sliceTrace(vg, out, head, x0, x1, mid, h, sfs::SCREEN_BLUE, true);
	sliceTrace(vg, dry, head, x0, x1, mid, h, sfs::SCREEN_HOT, false);

	sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
	nvgFillColor(vg, sfs::SCREEN_DIM);
	nvgText(vg, x0 + 3.f * s, y + 2.f * s, label, NULL);
}

void SliceDisplay::drawLive(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	Slice* m = module;
	float s = box.size.x / SD_W;
	SliceRows R = sliceRows(box.size.y / s);
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s, y = R.bufY * s, h = R.bufH * s;

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
	const int NB = 240;
	// bufN and the vectors are swapped a moment apart on the audio thread, so
	// the drawing takes whichever is smaller and cannot run off the end.
	long n = std::min(m->bufN, (long)std::min(m->bufL.size(), m->bufR.size()));
	if (n < 2) return;
	long step = std::max(1L, (long)(win / (float)NB));
	float pk[NB];
	for (int i = 0; i < NB; i++) {
		long start = m->wr - (long)win + (long)((float)i / NB * win);
		float p = 0.f;
		for (long k = 0; k < step; k += std::max(1L, step / 16)) {
			long a = ((start + k) % n + n) % n;
			p = std::max(p, std::max(std::fabs(m->bufL[(size_t)a]),
			                         std::fabs(m->bufR[(size_t)a])));
		}
		pk[i] = p;
	}
	sliceFilled(vg, pk, NB, x0, x1, y + h * 0.5f, h, sfs::SCREEN_DEEP);

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
	float fi = m->dispFadeIn / APP->engine->getSampleRate() * 1000.f;
	float fo = m->dispFadeOut / APP->engine->getSampleRate() * 1000.f;
	nvgText(vg, x0, (SD_BUFY - 8.f) * s,
	        m->freeze ? "FROZEN"
	                  : string::f("%.0f ms %s   fade %.1f/%.1f ms", ms,
	                              m->clockedLength() ? "CLK" : "LEN", fi, fo).c_str(), NULL);
	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, sfs::SCREEN_TEXT);
	int want = m->reachCount(), got = m->reachFit();
	const char* unit = m->reachInBars() ? (got == 1 ? "bar" : "bars")
	                                    : (got == 1 ? "step" : "steps");
	std::string reach = (got == want) ? string::f("%d %s", got, unit)
	                                  : string::f("%d/%d %s", got, want, unit);
	nvgText(vg, x1, (SD_BUFY - 8.f) * s,
	        string::f("%s /%d  \u2190%s", SLICE_EFFNAME[m->effIndex()], m->ratioN(),
	                  reach.c_str()).c_str(), NULL);

	slicePane(vg, font, s, R.p1Y, R.paneH, m->scDry[0], m->scOut[0], m->scHead, "L");
	slicePane(vg, font, s, R.p2Y, R.paneH, m->scDry[1], m->scOut[1], m->scHead, "R");
}

void SliceDisplay::drawPreview(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	float s = box.size.x / SD_W;
	SliceRows R = sliceRows(box.size.y / s);
	float x0 = SD_M * s, x1 = (SD_W - SD_M) * s, y = R.bufY * s, h = R.bufH * s;
	nvgBeginPath(vg); nvgRect(vg, x0, y, x1 - x0, h);
	nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20)); nvgFill(vg);
	float pk[240];
	for (int i = 0; i < 240; i++)
		pk[i] = (1.2f + 3.4f * std::fabs(std::sin((float)i * 0.155f))
		                     * std::fabs(std::sin((float)i * 0.035f)));
	sliceFilled(vg, pk, 240, x0, x1, y + h * 0.5f, h, sfs::SCREEN_DEEP);
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
	nvgText(vg, x1, (SD_BUFY - 8.f) * s, "CUT /4  \u21904 steps", NULL);

	// Five seconds of CUT every 4: the output drops out for a slice at a time
	// and the orange outline is left showing what was removed.
	static float dry[Slice::SCOPE], out[Slice::SCOPE];
	for (int i = 0; i < Slice::SCOPE; i++) {
		float t = (float)i / (float)Slice::SCOPE;
		dry[i] = (0.6f + 0.4f * std::sin(t * 31.f))
		       * (0.55f + 0.45f * std::sin(t * 7.3f)) * 4.2f;
		int slot = i / 13, into = i % 13;                 // ~125ms slices
		float e = clamp(std::min((float)into, 12.f - (float)into) * 0.9f, 0.f, 1.f);
		out[i] = (slot % 4 == 3) ? 0.f : dry[i] * e;
	}
	slicePane(vg, font, s, R.p1Y, R.paneH, dry, out, Slice::SCOPE - 1, "L");
	slicePane(vg, font, s, R.p2Y, R.paneH, dry, out, Slice::SCOPE - 1, "R");
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

		// NO PanelLabels HERE, and no title. This panel's artwork carries its own
		// text as outlined paths -- which Rack does render, unlike <text> -- so
		// drawing them again in Figtree printed every label on the panel twice,
		// half a millimetre out. Positions below are read from the guides in
		// design/slice.svg, which is now the source of the layout.
		SliceDisplay* disp = new SliceDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(5.08f, 10.16f));
		disp->box.size = mm2px(Vec(101.60f, 56.22f));
		addChild(disp);

		// ── the controls, one row ────────────────────────────────────────────
		const float CY = 81.28f, TY = 80.39f;
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(8.47f,  CY)), module, Slice::EFFECT_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(23.71f, CY)), module, Slice::RATIO_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(38.95f, CY)), module, Slice::SHAPE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(54.99f, TY)), module, Slice::DEPTH_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(66.84f, TY)), module, Slice::LENGTH_PARAM));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(78.70f, TY)), module, Slice::FREEZE_PARAM, Slice::FREEZE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(90.55f, TY)), module, Slice::RESEED_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(102.32f, TY)), module, Slice::RANGE_PARAM));

		// ── CV row: each jack under the control it feeds ─────────────────────
		const float JY1 = 102.40f;
		struct J { float x; int id; };
		static const J CV[9] = {
			{  7.58f, Slice::EFFECT_INPUT }, { 19.43f, Slice::RATIO_INPUT  },
			{ 31.28f, Slice::CLOCK_INPUT  }, { 43.14f, Slice::BAR_INPUT    },
			{ 54.99f, Slice::DEPTH_INPUT  }, { 66.84f, Slice::LENGTH_INPUT },
			{ 78.70f, Slice::FREEZE_INPUT }, { 90.55f, Slice::RESEED_INPUT },
			{102.40f, Slice::RANGE_INPUT  },
		};
		for (int i = 0; i < 9; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CV[i].x, JY1)), module, CV[i].id));

		// ── audio in, two trims, reset, then the outputs on their plate ──────
		const float JY2 = 121.03f;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.58f,  JY2)), module, Slice::L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.43f, JY2)), module, Slice::R_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(31.28f, JY2)), module, Slice::DIV_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(43.05f, JY2)), module, Slice::WINDOW_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(54.91f, JY2)), module, Slice::RESET_INPUT));
		// TRIG, GATE, L, R -- the order the plate is lettered, which is not the
		// order the enum is in. Getting this backwards puts the audio out under
		// the label that says TRIG, and nothing about it looks wrong on screen.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(66.84f,  JY2)), module, Slice::SLICE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(78.70f,  JY2)), module, Slice::XF_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(90.47f,  JY2)), module, Slice::L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(102.40f, JY2)), module, Slice::R_OUTPUT));
	}

	// The audio thread hands the old buffer back by swapping it into the pending
	// vectors; freeing tens of megabytes is not its job, so it happens here.
	void step() override {
		Slice* m = dynamic_cast<Slice*>(this->module);
		if (m && m->pendDone.load()) {
			m->pendDone = false;
			m->pendL.clear(); m->pendL.shrink_to_fit();
			m->pendR.clear(); m->pendR.shrink_to_fit();
		}
		ModuleWidget::step();
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
		menu->addChild(createSubmenuItem("Buffer length",
			string::f("%.0f s", SLICE_BUFOPT[clamp(m->bufSecIdx, 0, SLICE_NBUFOPT - 1)]),
			[=](Menu* sub) {
				for (int i = 0; i < SLICE_NBUFOPT; i++) {
					float sec = SLICE_BUFOPT[i];
					float mb = sec * APP->engine->getSampleRate() * 2.f * 4.f / 1048576.f;
					sub->addChild(createCheckMenuItem(
						string::f("%.0f s", sec), string::f("%.0f MB", mb),
						[=]() { return m->bufSecIdx == i; },
						[=]() { m->requestBuffer(i); }));
				}
			}));
		menu->addChild(createIndexPtrSubmenuItem("Repeat splice",
			{"Clean", "Dirty"}, &m->spliceMode));
		// LINK came off the panel to make room for WINDOW. It is a set-once
		// decision about the stereo image, not something played.
		menu->addChild(createIndexPtrSubmenuItem("Channels",
			{"Independent", "Paired"}, &m->linkIdx));
	}
};

std::string SliceWindowQuantity::getDisplayValueString() {
	Slice* m = dynamic_cast<Slice*>(module);
	if (!m) return "";
	float sr = APP->engine->getSampleRate();
	float half = std::max(m->sliceLen * 0.5f, 1.f);
	float lo = std::min(0.005f * sr, half);
	float hi = std::max(half, lo);
	float f = lo * std::pow(hi / lo, clamp(getValue(), 0.f, 1.f));
	return string::f("%.1f ms", f / sr * 1000.f);
}

std::string SliceReachQuantity::getUnit() {
	Slice* m = dynamic_cast<Slice*>(module);
	bool bars = m && m->reachInBars();
	bool one = std::round(getValue()) == 1.f;
	return bars ? (one ? " bar" : " bars") : (one ? " step" : " steps");
}
std::string SliceReachQuantity::getDescription() {
	Slice* m = dynamic_cast<Slice*>(module);
	if (m && m->reachInBars())
		return "How far back DELAY and SHUFFLE fetch from, in whole bars. Patched "
		       "to BAR, the reach lands on a bar line, which is what makes a "
		       "delayed slice arrive somewhere musical.";
	return "How far back DELAY and SHUFFLE fetch from, in whole slices. Patch a "
	       "BAR clock and this counts bars instead.";
}

Model* modelSlice = createModel<Slice, SliceWidget>("Slice");
