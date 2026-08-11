#include "plugin.hpp"
#include <cmath>
#include <cstring>
#include <vector>

// =============================================================================
// WAVE — Polaroid wavetable voice
//
// One LIVE shape is sculpted continuously by 6 macro params (Peaks, Bias,
// Transition, Skew, Width, Fold) plus optional CV. A WANDER knob layers
// independent random modulation onto each shape param — internal motion that
// makes the live shape evolve without any external CV.
//
// SNAP captures the current live shape into the wavetable. The wavetable is a
// FIFO of up to 8 snapshots: oldest gets overwritten when full. Audio output
// reads the snapshots, *not* the live shape — the live shape is only audible
// once you snap it. Until a snapshot exists, the module is silent.
//
// WT VELOCITY scans the playback position through the snapshots. Bipolar with
// center detent: positive scans forward (oldest → newest), negative reverse,
// 0 = locked. Position wraps end-to-beginning continuously. ACCEL slews
// velocity changes for smooth speed transitions.
//
// All snapshots are pre-rendered with an FFT-bandlimited mipmap so playback
// is alias-free at any pitch.
// =============================================================================

static const int  NUM_SLOTS    = 8;
static const int  TABLE_SIZE   = 2048;
static const int  NUM_LEVELS   = 11;
static const int  MINI_SIZE    = 64;
static const int  CURVE_LUT_SIZE = 256;

// Live preview throttle — the live thumbnail re-renders for the display only.
// 60Hz is plenty (visual refresh).
static const int  LIVE_PREVIEW_DEFER = 800;   // ~60Hz @ 48k


// ---------- Bookmark ----------

struct Bookmark {
	float peaks      = 1.f;
	float bias       = 0.f;
	float transition = 0.6f;
	float skew       = 0.f;
	float width      = 0.5f;
	float fold       = 0.f;
};


// ---------- Tables for one slot (mipmap + thumbnail) ----------

struct SlotTables {
	float levels[NUM_LEVELS][TABLE_SIZE] = {};
	float mini[MINI_SIZE] = {};
	bool  ready = false;
};


// =============================================================================
// Module
// =============================================================================

struct Wave : Module {
	enum ParamId {
		PEAKS_PARAM,
		BIAS_PARAM,
		TRANSITION_PARAM,
		SKEW_PARAM,
		WIDTH_PARAM,
		FOLD_PARAM,
		WANDER_PARAM,
		WT_VEL_PARAM,
		ACCEL_PARAM,
		SNAP_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		FM_INPUT,
		SYNC_INPUT,
		WT_VEL_CV_INPUT,
		SNAP_INPUT,
		RESET_INPUT,
		WANDER_CV_INPUT,
		PEAKS_CV_INPUT,
		BIAS_CV_INPUT,
		TRANSITION_CV_INPUT,
		SKEW_CV_INPUT,
		WIDTH_CV_INPUT,
		FOLD_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	// --- Snapshot wavetable ---
	Bookmark bookmarks[NUM_SLOTS];     // stored shapes (storage indices, arbitrary)
	SlotTables tables[NUM_SLOTS];      // mipmaps for stored shapes
	int  captureOrder[NUM_SLOTS] = {}; // playback order: index 0 = oldest
	int  snapCount = 0;                // 0..NUM_SLOTS

	// --- Live shape (always reflects knobs + CV + WANDER) ---
	Bookmark liveBookmark;
	float liveMini[MINI_SIZE] = {};    // display thumbnail of live shape
	int  livePreviewDefer = 0;
	bool liveDirty = true;

	// --- WANDER state (6 sample-and-hold + smoothstep interp sources) ---
	// Each source runs at its own base rate (mutually prime-ish so they don't
	// sync). On each segment boundary it picks a new random target in [-1,+1];
	// `current` interpolates from prev → target via smoothstep over the segment.
	struct WanderSrc {
		float prev = 0.f;
		float target = 0.f;
		float current = 0.f;
		float phase = 0.f;     // 0..1 within current segment
	};
	WanderSrc wander[6];
	const float wanderBaseRate[6] = {0.31f, 0.47f, 0.71f, 1.13f, 1.7f, 2.1f};

	// --- Playback ---
	double phase = 0.0;
	double playPosition = 0.0;        // float index in captureOrder[0..snapCount)
	float currentVelocity = 0.f;      // slewed actual velocity
	dsp::SchmittTrigger syncTrigger;
	dsp::SchmittTrigger snapTrigger;
	dsp::SchmittTrigger snapBtnTrigger;
	dsp::SchmittTrigger resetTrigger;

	// --- FFT ---
	dsp::RealFFT* fft = nullptr;
	std::vector<float> fftSpectrum;

	// --- Param quantities ---
	struct PeaksQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			return string::f("%d", (int)std::round(getValue()));
		}
	};
	struct BipolarPctQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			return string::f("%+.0f%%", getValue() * 100.f);
		}
	};
	struct UnipolarPctQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			return string::f("%.0f%%", getValue() * 100.f);
		}
	};
	struct AccelQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float t = getValue();
			if (t < 0.005f) return "Instant";
			if (t < 1.f) return string::f("%.0f ms", t * 1000.f);
			return string::f("%.2f s", t);
		}
	};
	struct VelocityQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float v = getValue();
			if (std::fabs(v) < 0.01f) return "Stopped";
			float secPerSweep = (float)NUM_SLOTS / (4.f * std::fabs(v));
			return string::f("%s %.2fs/sweep", v > 0 ? "FWD" : "REV", secPerSweep);
		}
	};

	Wave() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<PeaksQuantity>(PEAKS_PARAM, 1.f, 8.f, 1.f, "Peaks");
		paramQuantities[PEAKS_PARAM]->snapEnabled = true;
		configParam<BipolarPctQuantity>(BIAS_PARAM, -1.f, 1.f, 0.f, "Bias");
		configParam<BipolarPctQuantity>(TRANSITION_PARAM, -1.f, 1.f, 0.6f, "Transition (log ↔ exp)");
		configParam<BipolarPctQuantity>(SKEW_PARAM, -1.f, 1.f, 0.f, "Skew");
		configParam<UnipolarPctQuantity>(WIDTH_PARAM, 0.f, 1.f, 0.5f, "Width");
		configParam<UnipolarPctQuantity>(FOLD_PARAM, 0.f, 1.f, 0.f, "Fold");
		configParam<UnipolarPctQuantity>(WANDER_PARAM, 0.f, 1.f, 0.f, "Wander (internal random modulation)");
		configParam<VelocityQuantity>(WT_VEL_PARAM, -1.f, 1.f, 0.f, "Wavetable scan velocity");
		paramQuantities[WT_VEL_PARAM]->snapEnabled = false;
		configParam<AccelQuantity>(ACCEL_PARAM, 0.f, 5.f, 0.f, "Velocity accel (slew)");
		configButton(SNAP_PARAM, "Snap (capture live shape into wavetable)");

		configInput(VOCT_INPUT,        "V/Oct");
		configInput(FM_INPUT,          "FM (linear, ~100 Hz/V)");
		configInput(SYNC_INPUT,        "Sync (rising edge resets phase)");
		configInput(WT_VEL_CV_INPUT,   "Velocity CV (±5V → ±1.0)");
		configInput(SNAP_INPUT,        "Snap trigger");
		configInput(RESET_INPUT,       "Reset (jump scan position to oldest)");
		configInput(WANDER_CV_INPUT,   "Wander CV");
		configInput(PEAKS_CV_INPUT,      "Peaks CV");
		configInput(BIAS_CV_INPUT,       "Bias CV");
		configInput(TRANSITION_CV_INPUT, "Transition CV");
		configInput(SKEW_CV_INPUT,       "Skew CV");
		configInput(WIDTH_CV_INPUT,      "Width CV");
		configInput(FOLD_CV_INPUT,       "Fold CV");

		configOutput(AUDIO_OUTPUT, "Audio");

		fft = new dsp::RealFFT(TABLE_SIZE);
		fftSpectrum.resize(TABLE_SIZE * 2);

		snapCount = 0;
		for (int s = 0; s < NUM_SLOTS; s++) captureOrder[s] = s;

		// Initial snap of the default live shape so the module makes sound on
		// first patch — without this it's silent until the user hits SNAP.
		liveBookmark = Bookmark{};
		snapNow();
	}

	~Wave() {
		delete fft;
	}

	void onReset() override {
		snapCount = 0;
		for (int s = 0; s < NUM_SLOTS; s++) {
			captureOrder[s] = s;
			tables[s].ready = false;
		}
		phase = 0.0;
		playPosition = 0.0;
		currentVelocity = 0.f;
		for (int i = 0; i < 6; i++) {
			wander[i].prev = 0.f;
			wander[i].target = 0.f;
			wander[i].current = 0.f;
			wander[i].phase = 0.f;
		}
		liveDirty = true;
		livePreviewDefer = 0;
		// Re-snap default shape so audio resumes immediately after Initialize.
		liveBookmark = Bookmark{};
		snapNow();
	}

	// --------------------- Shape renderer ---------------------

	static void buildTransitionLUT(float transition, float* lut) {
		float a = std::fabs(transition);
		float k = 1.f + a * 4.f;
		bool exp = (transition >= 0.f);
		for (int i = 0; i <= CURVE_LUT_SIZE; i++) {
			float u = (float)i / (float)CURVE_LUT_SIZE;
			lut[i] = exp ? std::pow(u, k) : (1.f - std::pow(1.f - u, k));
		}
	}
	static void buildWidthLUT(float width, float* lut) {
		float widthExp = 1.f + (1.f - clamp(width, 0.f, 1.f)) * 5.f;
		for (int i = 0; i <= CURVE_LUT_SIZE; i++) {
			float u = (float)i / (float)CURVE_LUT_SIZE;
			lut[i] = std::pow(u, widthExp);
		}
	}
	static void buildBiasLUT(float bias, float* lut) {
		float a = std::fabs(bias);
		float k = 1.f + a * 4.f;
		if (a < 1e-4f) {
			for (int i = 0; i <= CURVE_LUT_SIZE; i++) {
				lut[i] = (float)i / (float)CURVE_LUT_SIZE;
			}
			return;
		}
		bool pos = (bias > 0.f);
		for (int i = 0; i <= CURVE_LUT_SIZE; i++) {
			float t = (float)i / (float)CURVE_LUT_SIZE;
			lut[i] = pos ? std::pow(t, k) : (1.f - std::pow(1.f - t, k));
		}
	}
	static inline float lutLookup(const float* lut, float u) {
		if (u <= 0.f) return lut[0];
		if (u >= 1.f) return lut[CURVE_LUT_SIZE];
		float fp = u * (float)CURVE_LUT_SIZE;
		int i0 = (int)fp;
		float f  = fp - (float)i0;
		return lut[i0] + (lut[i0 + 1] - lut[i0]) * f;
	}

	void renderShape(const Bookmark& bm, float* out) {
		int peaks = clamp((int)std::round(bm.peaks), 1, 8);
		int nHalf = 2 * peaks;
		float crest = clamp(0.5f + 0.5f * bm.skew, 0.05f, 0.95f);

		float biasLUT[CURVE_LUT_SIZE + 1];
		float transLUT[CURVE_LUT_SIZE + 1];
		float widthLUT[CURVE_LUT_SIZE + 1];
		buildBiasLUT(bm.bias, biasLUT);
		buildTransitionLUT(bm.transition, transLUT);
		buildWidthLUT(bm.width, widthLUT);

		float invCrest = 1.f / crest;
		float invOneMinusCrest = 1.f / (1.f - crest);
		float fnHalf = (float)nHalf;
		float invTable = 1.f / (float)TABLE_SIZE;

		for (int i = 0; i < TABLE_SIZE; i++) {
			float t = (float)i * invTable;
			float w = lutLookup(biasLUT, t);
			float scaled = w * fnHalf;
			int seg = (int)scaled;
			if (seg >= nHalf) seg = nHalf - 1;
			float local = scaled - (float)seg;

			float u = (local < crest)
				? (local * invCrest)
				: ((1.f - local) * invOneMinusCrest);
			float e = lutLookup(transLUT, u);
			e = lutLookup(widthLUT, e);
			float sign = (seg & 1) ? -1.f : 1.f;
			out[i] = sign * e;
		}

		// DC-remove + peak-normalize
		double sum = 0.0;
		for (int i = 0; i < TABLE_SIZE; i++) sum += out[i];
		float mean = (float)(sum / TABLE_SIZE);
		float peak = 0.f;
		for (int i = 0; i < TABLE_SIZE; i++) {
			out[i] -= mean;
			float a = std::fabs(out[i]);
			if (a > peak) peak = a;
		}
		if (peak > 1e-6f) {
			float g = 1.f / peak;
			for (int i = 0; i < TABLE_SIZE; i++) out[i] *= g;
		}

		if (bm.fold > 1e-4f) {
			float drive = (1.f + bm.fold * 4.f) * (float)M_PI * 0.5f;
			for (int i = 0; i < TABLE_SIZE; i++) {
				out[i] = std::sin(out[i] * drive);
			}
		}
	}

	// Build mini thumbnail from a base table.
	static void thumbFromTable(const float* table, float* mini) {
		int binSize = TABLE_SIZE / MINI_SIZE;
		for (int i = 0; i < MINI_SIZE; i++) {
			float maxAbs = 0.f, signedPeak = 0.f;
			for (int j = 0; j < binSize; j++) {
				float v = table[i * binSize + j];
				if (std::fabs(v) > maxAbs) { maxAbs = std::fabs(v); signedPeak = v; }
			}
			mini[i] = signedPeak;
		}
	}

	// Build full mipmap into slot s.
	void buildMipmap(int s, const Bookmark& bm) {
		renderShape(bm, tables[s].levels[0]);
		fft->rfft(tables[s].levels[0], fftSpectrum.data());

		const int N = TABLE_SIZE;
		std::vector<float> work(N * 2);

		for (int k = 1; k < NUM_LEVELS; k++) {
			int maxBin = (N / 2) >> k;
			if (maxBin < 1) maxBin = 1;
			std::memcpy(work.data(), fftSpectrum.data(), sizeof(float) * N);
			if (maxBin < N / 2) work[1] = 0.f;
			for (int b = maxBin + 1; b < N / 2; b++) {
				work[2 * b]     = 0.f;
				work[2 * b + 1] = 0.f;
			}
			fft->irfft(work.data(), tables[s].levels[k]);
			float a = 1.f / (float)N;
			for (int i = 0; i < N; i++) tables[s].levels[k][i] *= a;
		}

		thumbFromTable(tables[s].levels[0], tables[s].mini);
		tables[s].ready = true;
	}

	// --------------------- SNAP — capture liveBookmark to wavetable ---------------------
	void snapNow() {
		int targetSlot;
		if (snapCount < NUM_SLOTS) {
			// Find a slot index not yet in captureOrder[0..snapCount-1].
			bool used[NUM_SLOTS] = {};
			for (int i = 0; i < snapCount; i++) used[captureOrder[i]] = true;
			targetSlot = 0;
			for (int s = 0; s < NUM_SLOTS; s++) if (!used[s]) { targetSlot = s; break; }
			captureOrder[snapCount] = targetSlot;
			snapCount++;
		} else {
			// Full — overwrite oldest (captureOrder[0]) and rotate.
			targetSlot = captureOrder[0];
			for (int i = 0; i < NUM_SLOTS - 1; i++) {
				captureOrder[i] = captureOrder[i + 1];
			}
			captureOrder[NUM_SLOTS - 1] = targetSlot;
		}
		bookmarks[targetSlot] = liveBookmark;
		buildMipmap(targetSlot, liveBookmark);
	}

	// --------------------- Process ---------------------

	void process(const ProcessArgs& args) override {
		// 1. Build the live bookmark from knobs + CV.
		Bookmark base;
		base.peaks      = params[PEAKS_PARAM].getValue();
		base.bias       = params[BIAS_PARAM].getValue();
		base.transition = params[TRANSITION_PARAM].getValue();
		base.skew       = params[SKEW_PARAM].getValue();
		base.width      = params[WIDTH_PARAM].getValue();
		base.fold       = params[FOLD_PARAM].getValue();
		auto cv = [&](int input, float scale) -> float {
			return inputs[input].isConnected() ? inputs[input].getVoltage() * scale : 0.f;
		};
		base.peaks      = clamp(base.peaks      + cv(PEAKS_CV_INPUT,      0.7f),  1.f, 8.f);
		base.bias       = clamp(base.bias       + cv(BIAS_CV_INPUT,       0.2f), -1.f, 1.f);
		base.transition = clamp(base.transition + cv(TRANSITION_CV_INPUT, 0.2f), -1.f, 1.f);
		base.skew       = clamp(base.skew       + cv(SKEW_CV_INPUT,       0.2f), -1.f, 1.f);
		base.width      = clamp(base.width      + cv(WIDTH_CV_INPUT,      0.1f),  0.f, 1.f);
		base.fold       = clamp(base.fold       + cv(FOLD_CV_INPUT,       0.1f),  0.f, 1.f);

		// 2. WANDER — internal random modulation. Each of 6 params gets its own
		// S&H+smoothstep source running at a unique base rate. WANDER scales
		// both depth (excursion amplitude) and rate (how fast the sources cycle
		// to new targets). 0 = no motion. 1 = wild & fast.
		float wAmt = clamp(params[WANDER_PARAM].getValue()
			+ cv(WANDER_CV_INPUT, 0.1f), 0.f, 1.f);
		Bookmark live = base;
		if (wAmt > 1e-4f) {
			// Rate scaling: 0.5x → 8.5x base rate as WANDER goes 0→1.
			// At wander=1, source 5 (base 2.1Hz) fires at ~17Hz — fast enough
			// to feel chaotic without being audio-rate.
			float rateScale = 0.5f + wAmt * 8.f;
			for (int i = 0; i < 6; i++) {
				float rate = wanderBaseRate[i] * rateScale;
				wander[i].phase += args.sampleTime * rate;
				while (wander[i].phase >= 1.f) {
					wander[i].phase -= 1.f;
					wander[i].prev = wander[i].target;
					wander[i].target = random::uniform() * 2.f - 1.f;
				}
				// Smoothstep interp: 3t² - 2t³ — eliminates corners at hold boundaries
				float t = wander[i].phase;
				float st = t * t * (3.f - 2.f * t);
				wander[i].current = wander[i].prev + (wander[i].target - wander[i].prev) * st;
			}
			live.peaks      = clamp(live.peaks      + wander[0].current * wAmt * 4.f,  1.f, 8.f);
			live.bias       = clamp(live.bias       + wander[1].current * wAmt * 0.7f, -1.f, 1.f);
			live.transition = clamp(live.transition + wander[2].current * wAmt * 0.7f, -1.f, 1.f);
			live.skew       = clamp(live.skew       + wander[3].current * wAmt * 0.7f, -1.f, 1.f);
			live.width      = clamp(live.width      + wander[4].current * wAmt * 0.4f,  0.f, 1.f);
			live.fold       = clamp(live.fold       + wander[5].current * wAmt * 0.4f,  0.f, 1.f);
			liveDirty = true;
		}
		// Mark live dirty if anything actually changed.
		if (liveBookmark.peaks != live.peaks
			|| liveBookmark.bias != live.bias
			|| liveBookmark.transition != live.transition
			|| liveBookmark.skew != live.skew
			|| liveBookmark.width != live.width
			|| liveBookmark.fold != live.fold) {
			liveDirty = true;
		}
		liveBookmark = live;

		// 3. Live preview thumbnail — for display only, throttled to ~60Hz.
		if (livePreviewDefer > 0) livePreviewDefer--;
		if (liveDirty && livePreviewDefer == 0) {
			float buf[TABLE_SIZE];
			renderShape(liveBookmark, buf);
			thumbFromTable(buf, liveMini);
			liveDirty = false;
			livePreviewDefer = LIVE_PREVIEW_DEFER;
		}

		// 4. SNAP — button or rising-edge CV trigger.
		bool snapEdge = false;
		if (snapTrigger.process(inputs[SNAP_INPUT].getVoltage(), 0.1f, 1.f)) snapEdge = true;
		if (snapBtnTrigger.process(params[SNAP_PARAM].getValue(), 0.1f, 1.f)) snapEdge = true;
		if (snapEdge) snapNow();

		// 5. RESET — jump WT POSITION back to 0.
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			playPosition = 0.0;
		}

		// 6. WT VELOCITY: bipolar, slewed via ACCEL.
		float velTarget = clamp(params[WT_VEL_PARAM].getValue()
			+ cv(WT_VEL_CV_INPUT, 0.2f), -1.f, 1.f);
		float accel = params[ACCEL_PARAM].getValue();    // 0..5 sec
		if (accel < 1e-4f) {
			currentVelocity = velTarget;
		} else {
			float maxStep = (2.f / accel) * args.sampleTime;  // full ±1 swing in `accel` sec
			float diff = velTarget - currentVelocity;
			if (diff > maxStep)       currentVelocity += maxStep;
			else if (diff < -maxStep) currentVelocity -= maxStep;
			else                      currentVelocity  = velTarget;
		}

		// 7. Advance WT POSITION; wrap within filled subset.
		// Velocity 1.0 = 4 slots/sec → full sweep of 8 in 2 sec.
		const float SCAN_RATE = 4.f;
		if (snapCount > 1) {
			playPosition += (double)(currentVelocity * SCAN_RATE) * args.sampleTime;
			double snap = (double)snapCount;
			while (playPosition >= snap) playPosition -= snap;
			while (playPosition < 0.0)   playPosition += snap;
		} else if (snapCount == 1) {
			playPosition = 0.0;
		}

		// 8. Frequency: V/Oct + linear FM
		float voct = inputs[VOCT_INPUT].getVoltage();
		float fmIn = inputs[FM_INPUT].isConnected() ? inputs[FM_INPUT].getVoltage() : 0.f;
		float baseFreq = 261.6256f * std::pow(2.f, voct);
		float freq = baseFreq + fmIn * 100.f;
		if (freq < 0.f) freq = 0.f;
		if (freq > args.sampleRate * 0.5f) freq = args.sampleRate * 0.5f;

		// 9. Sync
		if (syncTrigger.process(inputs[SYNC_INPUT].getVoltage(), 0.1f, 1.f)) {
			phase = 0.0;
		}

		// 10. Advance phase
		phase += (double)freq / (double)args.sampleRate;
		while (phase >= 1.0) phase -= 1.0;
		while (phase < 0.0)  phase += 1.0;

		// 11. Mipmap level pick
		int level = 0;
		if (freq > 1.f) {
			float ratio = (float)(TABLE_SIZE / 2) * freq / (args.sampleRate * 0.5f);
			if (ratio > 1.f) {
				level = (int)std::ceil(std::log2(ratio));
				if (level < 0) level = 0;
				if (level >= NUM_LEVELS) level = NUM_LEVELS - 1;
			}
		}

		// 12. Audio output: silent if no snapshots; else interp neighbors in
		// captureOrder space.
		float sample = 0.f;
		if (snapCount >= 1) {
			int loPos = (int)std::floor(playPosition);
			if (loPos < 0) loPos = 0;
			if (loPos >= snapCount) loPos = snapCount - 1;
			int hiPos = (snapCount > 1) ? ((loPos + 1) % snapCount) : loPos;
			float frac = (float)(playPosition - (double)loPos);

			int loSlot = captureOrder[loPos];
			int hiSlot = captureOrder[hiPos];
			if (tables[loSlot].ready && tables[hiSlot].ready) {
				float a = sampleTable(tables[loSlot].levels[level], (float)phase);
				float b = sampleTable(tables[hiSlot].levels[level], (float)phase);
				sample = a + (b - a) * frac;
			}
		}

		outputs[AUDIO_OUTPUT].setVoltage(sample * 5.f);
	}

	static inline float sampleTable(const float* tbl, float phase) {
		float fp = phase * TABLE_SIZE;
		int   i0 = (int)fp;
		if (i0 < 0) i0 = 0;
		if (i0 >= TABLE_SIZE) i0 = TABLE_SIZE - 1;
		int   i1 = (i0 + 1) % TABLE_SIZE;
		float f  = fp - (float)i0;
		return tbl[i0] + (tbl[i1] - tbl[i0]) * f;
	}

	// --------------------- Persistence ---------------------

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		// Save snapshots in capture order so reload preserves the FIFO.
		for (int i = 0; i < snapCount; i++) {
			int s = captureOrder[i];
			json_t* o = json_object();
			json_object_set_new(o, "peaks",      json_real(bookmarks[s].peaks));
			json_object_set_new(o, "bias",       json_real(bookmarks[s].bias));
			json_object_set_new(o, "transition", json_real(bookmarks[s].transition));
			json_object_set_new(o, "skew",       json_real(bookmarks[s].skew));
			json_object_set_new(o, "width",      json_real(bookmarks[s].width));
			json_object_set_new(o, "fold",       json_real(bookmarks[s].fold));
			json_array_append_new(arr, o);
		}
		json_object_set_new(root, "snapshots", arr);
		return root;
	}

	void dataFromJson(json_t* root) override {
		snapCount = 0;
		for (int s = 0; s < NUM_SLOTS; s++) {
			tables[s].ready = false;
			captureOrder[s] = s;
		}
		if (json_t* arr = json_object_get(root, "snapshots")) {
			size_t n = json_array_size(arr);
			for (size_t i = 0; i < n && i < NUM_SLOTS; i++) {
				json_t* o = json_array_get(arr, i);
				if (!o) continue;
				Bookmark bm;
				if (auto* j = json_object_get(o, "peaks"))      bm.peaks      = json_number_value(j);
				if (auto* j = json_object_get(o, "bias"))       bm.bias       = json_number_value(j);
				if (auto* j = json_object_get(o, "transition")) bm.transition = json_number_value(j);
				if (auto* j = json_object_get(o, "skew"))       bm.skew       = json_number_value(j);
				if (auto* j = json_object_get(o, "width"))      bm.width      = json_number_value(j);
				if (auto* j = json_object_get(o, "fold"))       bm.fold       = json_number_value(j);
				bookmarks[(int)i] = bm;
				captureOrder[(int)i] = (int)i;
				buildMipmap((int)i, bm);
				snapCount++;
			}
		}
	}

	void clearAll() {
		snapCount = 0;
		for (int s = 0; s < NUM_SLOTS; s++) {
			captureOrder[s] = s;
			tables[s].ready = false;
		}
		playPosition = 0.0;
	}
};


// =============================================================================
// Display widget
// =============================================================================

struct WaveDisplay : OpaqueWidget {
	Wave* module = nullptr;
	std::shared_ptr<Font> font;

	float stripY = 0.f, stripH = 0.f, waveH = 0.f, cellW = 0.f;

	void computeLayout() {
		float w = box.size.x;
		float h = box.size.y;
		stripH = std::max(14.f, h * 0.30f);
		stripY = h - stripH;
		waveH  = stripY - 2.f;
		cellW  = (w - 2.f) / (float)NUM_SLOTS;
	}

	// Browser-preview render (module == NULL): draws live shape (2-peak wave) +
	// 3 filled slot thumbnails so the VCV Library screenshot conveys what WAVE
	// is. Static, all-hardcoded.
	void drawPreview(const DrawArgs& args) {
		computeLayout();
		const NVGcolor BG          = nvgRGBA(0x0a, 0x0a, 0x18, 0xFF);
		const NVGcolor BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
		const NVGcolor BLUE_FAINT  = nvgRGBA(0x00, 0x97, 0xDE, 0x60);
		const NVGcolor ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
		const NVGcolor ORANGE_FAINT= nvgRGBA(0xEC, 0x65, 0x2E, 0x40);
		const NVGcolor PURPLE      = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
		const NVGcolor PURPLE_DIM  = nvgRGBA(0x35, 0x35, 0x4D, 0x80);
		const NVGcolor EMPTY       = nvgRGBA(0x14, 0x14, 0x28, 0xFF);

		float w = box.size.x, h = box.size.y;
		nvgBeginPath(args.vg); nvgRect(args.vg, 0, 0, w, h);
		nvgFillColor(args.vg, BG); nvgFill(args.vg);

		float waveTop = 2.f, waveBot = waveTop + waveH - 2.f;
		float midY = (waveTop + waveBot) * 0.5f;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 2, midY); nvgLineTo(args.vg, w - 2, midY);
		nvgStrokeColor(args.vg, PURPLE_DIM); nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);

		// Live shape: 2-peak wave (sine of 2 cycles, slight skew)
		float drawW = w - 4.f;
		nvgBeginPath(args.vg);
		for (int i = 0; i < MINI_SIZE; i++) {
			float t = (float)i / (float)(MINI_SIZE - 1);
			float v = std::sin(t * 4.f * (float)M_PI) * 0.85f;
			float x = 2.f + drawW * t;
			float y = midY - v * (waveH * 0.45f);
			if (i == 0) nvgMoveTo(args.vg, x, y);
			else nvgLineTo(args.vg, x, y);
		}
		nvgStrokeColor(args.vg, BLUE); nvgStrokeWidth(args.vg, 1.4f);
		nvgStroke(args.vg);

		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 8.f);
			nvgFillColor(args.vg, BLUE_FAINT);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgText(args.vg, 4.f, 3.f, "LIVE", nullptr);
			nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgFillColor(args.vg, ORANGE_FAINT);
			nvgText(args.vg, w - 4.f, 3.f, "3/8", nullptr);
		}

		// Slot strip: 3 filled (slot 1 = playing in orange), 5 empty
		const int previewSnapCount = 3;
		const int previewPlayPos   = 1;
		for (int pos = 0; pos < NUM_SLOTS; pos++) {
			float cx = 1.f + pos * cellW;
			float cy = stripY, cw = cellW - 1.f, ch = stripH - 2.f;
			bool filled = pos < previewSnapCount;
			NVGcolor fill = (pos == previewPlayPos) ? ORANGE_FAINT : EMPTY;
			nvgBeginPath(args.vg); nvgRect(args.vg, cx, cy, cw, ch);
			nvgFillColor(args.vg, fill); nvgFill(args.vg);
			if (filled) {
				// 3 different small waveforms — varied shape per slot
				float midCellY = cy + ch * 0.5f;
				nvgBeginPath(args.vg);
				for (int i = 0; i < MINI_SIZE; i++) {
					float t = (float)i / (float)(MINI_SIZE - 1);
					float v = 0.f;
					if (pos == 0)      v = std::sin(t * 2.f * (float)M_PI) * 0.8f;            // sine
					else if (pos == 1) v = std::sin(t * 4.f * (float)M_PI) * 0.85f;           // 2-peak
					else if (pos == 2) v = (t < 0.5f ? 2.f * t - 0.5f : 2.f * (1.f - t) - 0.5f) * 1.6f; // triangle
					float x = cx + 1.f + ((cw - 2.f) * t);
					float y = midCellY - v * (ch * 0.40f);
					if (i == 0) nvgMoveTo(args.vg, x, y);
					else nvgLineTo(args.vg, x, y);
				}
				NVGcolor stroke = (pos == previewPlayPos) ? ORANGE : PURPLE;
				nvgStrokeColor(args.vg, stroke); nvgStrokeWidth(args.vg, 1.0f);
				nvgStroke(args.vg);
			} else {
				nvgBeginPath(args.vg);
				nvgRect(args.vg, cx + 0.5f, cy + 0.5f, cw - 1.f, ch - 1.f);
				nvgStrokeColor(args.vg, PURPLE_DIM); nvgStrokeWidth(args.vg, 0.5f);
				nvgStroke(args.vg);
			}
		}

		// Position pointer
		float ppx = 1.f + cellW * ((float)previewPlayPos + 0.5f);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, ppx, stripY - 4.f);
		nvgLineTo(args.vg, ppx - 2.5f, stripY - 1.f);
		nvgLineTo(args.vg, ppx + 2.5f, stripY - 1.f);
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, ORANGE); nvgFill(args.vg);

		if (!font || font->handle < 0) {
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			OpaqueWidget::drawLayer(args, layer);
			return;
		}
		if (!module) {
			drawPreview(args);
			return;
		}
		computeLayout();

		const NVGcolor BG          = nvgRGBA(0x0a, 0x0a, 0x18, 0xFF);
		const NVGcolor BLUE        = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
		const NVGcolor BLUE_FAINT  = nvgRGBA(0x00, 0x97, 0xDE, 0x60);
		const NVGcolor ORANGE      = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
		const NVGcolor ORANGE_FAINT= nvgRGBA(0xEC, 0x65, 0x2E, 0x40);
		const NVGcolor PURPLE      = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
		const NVGcolor PURPLE_DIM  = nvgRGBA(0x35, 0x35, 0x4D, 0x80);
		const NVGcolor EMPTY       = nvgRGBA(0x14, 0x14, 0x28, 0xFF);

		float w = box.size.x;
		float h = box.size.y;

		// Background
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, w, h);
		nvgFillColor(args.vg, BG);
		nvgFill(args.vg);

		// --- Top: live shape preview ---
		float waveTop = 2.f;
		float waveBot = waveTop + waveH - 2.f;
		float midY = (waveTop + waveBot) * 0.5f;

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 2, midY);
		nvgLineTo(args.vg, w - 2, midY);
		nvgStrokeColor(args.vg, PURPLE_DIM);
		nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);

		// Live shape — drawn in blue (to distinguish from snapshot playback in orange)
		float drawW = w - 4.f;
		nvgBeginPath(args.vg);
		for (int i = 0; i < MINI_SIZE; i++) {
			float v = module->liveMini[i];
			float x = 2.f + (drawW * (float)i) / (float)(MINI_SIZE - 1);
			float y = midY - v * (waveH * 0.45f);
			if (i == 0) nvgMoveTo(args.vg, x, y);
			else nvgLineTo(args.vg, x, y);
		}
		nvgStrokeColor(args.vg, BLUE);
		nvgStrokeWidth(args.vg, 1.4f);
		nvgStroke(args.vg);

		// "LIVE" label top-left
		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 8.f);
			nvgFillColor(args.vg, BLUE_FAINT);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgText(args.vg, 4.f, 3.f, "LIVE", nullptr);

			// Snap count top-right
			char buf[16];
			snprintf(buf, sizeof(buf), "%d/8", module->snapCount);
			nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgFillColor(args.vg, ORANGE_FAINT);
			nvgText(args.vg, w - 4.f, 3.f, buf, nullptr);
		}

		// --- Bottom: snapshot strip ---
		// Render only filled cells (snapCount of them, left to right oldest→newest).
		// Empty cells are dim outlines.
		double pp = module->playPosition;
		int playSlotPos = (module->snapCount > 0)
			? clamp((int)std::round(pp), 0, module->snapCount - 1) : -1;

		for (int pos = 0; pos < NUM_SLOTS; pos++) {
			float cx = 1.f + pos * cellW;
			float cy = stripY;
			float cw = cellW - 1.f;
			float ch = stripH - 2.f;

			bool filled = pos < module->snapCount;

			// Cell background
			NVGcolor fill = (pos == playSlotPos) ? ORANGE_FAINT : EMPTY;
			nvgBeginPath(args.vg);
			nvgRect(args.vg, cx, cy, cw, ch);
			nvgFillColor(args.vg, fill);
			nvgFill(args.vg);

			if (filled) {
				int slot = module->captureOrder[pos];
				if (module->tables[slot].ready) {
					float midCellY = cy + ch * 0.5f;
					nvgBeginPath(args.vg);
					for (int i = 0; i < MINI_SIZE; i++) {
						float v = module->tables[slot].mini[i];
						float x = cx + 1.f + ((cw - 2.f) * (float)i) / (float)(MINI_SIZE - 1);
						float y = midCellY - v * (ch * 0.40f);
						if (i == 0) nvgMoveTo(args.vg, x, y);
						else nvgLineTo(args.vg, x, y);
					}
					NVGcolor stroke = (pos == playSlotPos) ? ORANGE : PURPLE;
					nvgStrokeColor(args.vg, stroke);
					nvgStrokeWidth(args.vg, 1.0f);
					nvgStroke(args.vg);
				}
			} else {
				// Empty: dim outline
				nvgBeginPath(args.vg);
				nvgRect(args.vg, cx + 0.5f, cy + 0.5f, cw - 1.f, ch - 1.f);
				nvgStrokeColor(args.vg, PURPLE_DIM);
				nvgStrokeWidth(args.vg, 0.5f);
				nvgStroke(args.vg);
			}
		}

		// Position pointer above strip (only meaningful when snapshots exist)
		if (module->snapCount > 0) {
			// Map playPosition (0..snapCount) → x within filled cells
			float frac = (float)(pp / (double)module->snapCount);
			float px = 1.f + frac * (cellW * (float)module->snapCount);
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, px, stripY - 4.f);
			nvgLineTo(args.vg, px - 2.5f, stripY - 1.f);
			nvgLineTo(args.vg, px + 2.5f, stripY - 1.f);
			nvgClosePath(args.vg);
			nvgFillColor(args.vg, ORANGE);
			nvgFill(args.vg);
		}

		if (!font || font->handle < 0) {
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		}
	}
};


// =============================================================================
// Module widget
// =============================================================================

struct WaveWidget : ModuleWidget {
	WaveWidget(Wave* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/wave.svg")));

		// Display
		WaveDisplay* display = new WaveDisplay();
		display->module = module;
		display->box.pos  = mm2px(Vec(3.5f, 12.f));
		display->box.size = mm2px(Vec(74.3f, 32.f));
		addChild(display);

		// Shape knobs (3 cols)
		const float xL = 14.f, xC = 40.64f, xR = 67.28f;

		// Row 1: Peaks, Bias, Transition (knob + CV)
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(xL, 52.f)), module, Wave::PEAKS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(xC, 52.f)), module, Wave::BIAS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(xR, 52.f)), module, Wave::TRANSITION_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xL, 64.f)), module, Wave::PEAKS_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xC, 64.f)), module, Wave::BIAS_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, 64.f)), module, Wave::TRANSITION_CV_INPUT));

		// Row 2: Skew, Width, Fold
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(xL, 78.f)), module, Wave::SKEW_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(xC, 78.f)), module, Wave::WIDTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(xR, 78.f)), module, Wave::FOLD_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xL, 90.f)), module, Wave::SKEW_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xC, 90.f)), module, Wave::WIDTH_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, 90.f)), module, Wave::FOLD_CV_INPUT));

		// Macro row at y=104: WANDER pair, WT VEL pair, ACCEL trimpot, SNAP button + CV
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.f, 104.f)), module, Wave::WANDER_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.f, 104.f)), module, Wave::WANDER_CV_INPUT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(33.f, 104.f)), module, Wave::WT_VEL_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.f, 104.f)), module, Wave::WT_VEL_CV_INPUT));

		addParam(createParamCentered<Trimpot>(mm2px(Vec(54.f, 104.f)), module, Wave::ACCEL_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(64.f, 104.f)), module, Wave::SNAP_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(73.f, 104.f)), module, Wave::SNAP_INPUT));

		// I/O row at y=120: RESET, V/Oct, FM, Sync, AUDIO out
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.f, 120.f)),  module, Wave::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.f, 120.f)), module, Wave::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(45.f, 120.f)), module, Wave::FM_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.f, 120.f)), module, Wave::SYNC_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(73.f, 120.f)), module, Wave::AUDIO_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Wave* w = dynamic_cast<Wave*>(module);
		if (!w) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Snap now", "", [=]() { w->snapNow(); }));
		menu->addChild(createMenuItem("Clear all snapshots", "", [=]() { w->clearAll(); }));
	}
};


Model* modelWave = createModel<Wave, WaveWidget>("Wave");
