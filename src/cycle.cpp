#include "plugin.hpp"
#include <cmath>

// ─── Cycle: bar-synced quad LFO ──────────────────────────────────────────────
//
// Four LFO channels (A–D), each with its own SHAPE and bipolar SCALE, all
// phase-locked to a musical bar. Pairs with Meter: patch Meter's BAR into BAR
// and the bank locks to the downbeat. Unpatched, it free-runs (FREQUENCY in Hz).
//
// Timing is driven entirely by BAR: it measures the bar duration and hard-aligns
// the cycle to the bar grid every downbeat (no clock-pulse counting, so it can't
// be confused by sub-sample drift between a clock and the bar). CLOCK is optional:
// it sets the step grid for the staircase / stepped-random shapes (one step per
// pulse) and draws beat tick marks on the display.
//
//  • FREQUENCY  — Hz when free; ratchets to a bar division when locked
//  • PHASE      — global spread fanning A/B/C/D across the cycle
//  • SHAPE A–D  — morph sine → triangle → saw → square (50% duty)
//  • SCALE A–D  — bipolar depth (negative inverts)
//  • STABILITY  — amplitude wander (1 = locked); BAR keeps timing on the grid

static const int N_CH = 4;

static inline float xfade(float a, float b, float t) { return a + (b - a) * t; }

// sine → triangle → saw → square → staircase → stepped-random → (back to) sine.
// SHAPE is a CLOSED RING: six equal morph segments, so sweeping past random
// morphs smoothly back into sine. The shape value wraps (mod 1) rather than
// clamping, which is what lets CV and link offsets rotate all the way around
// the timbre loop. The staircase has nSteps equal steps of 1/n each (ascending);
// a negative SCALE inverts it. randVal is the shared per-step random value.
static float lfoShape(float ph, float shape, int nSteps, float randVal) {
	ph -= std::floor(ph);
	float sine = std::sin(2.f * (float) M_PI * ph);
	float tri  = (ph < 0.5f) ? (4.f * ph - 1.f) : (3.f - 4.f * ph);
	float saw  = 2.f * ph - 1.f;
	float sq   = (ph < 0.5f) ? 1.f : -1.f;
	if (nSteps < 1) nSteps = 1;
	float stair = (std::floor(ph * nSteps) / (float) nSteps) * 2.f - 1.f;   // 0,1/n,…,(n-1)/n
	float sw = shape - std::floor(shape);                                  // wrap into [0,1)
	float s = sw * 6.f;
	if (s < 1.f) return xfade(sine, tri, s);
	if (s < 2.f) return xfade(tri, saw, s - 1.f);
	if (s < 3.f) return xfade(saw, sq, s - 2.f);
	if (s < 4.f) return xfade(sq, stair, s - 3.f);
	if (s < 5.f) return xfade(stair, randVal, s - 4.f);
	return xfade(randVal, sine, s - 5.f);                                  // ring closes
}

struct Cycle;
struct FreqQuantity : ParamQuantity {
	std::string getDisplayValueString() override;   // defined after Cycle
};


struct Cycle : Module {
	enum ParamId {
		FREQ_PARAM, PHASE_PARAM, STAB_PARAM, RESET_PARAM,
		ENUMS(SHAPE_PARAM, N_CH),
		ENUMS(SCALE_PARAM, N_CH),
		ENUMS(SHAPE_LINK_PARAM, N_CH - 1),   // sync shape across adjacent channels
		ENUMS(SCALE_LINK_PARAM, N_CH - 1),   // sync scale across adjacent channels
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT, BAR_INPUT, RESET_INPUT,
		FREQ_INPUT, PHASE_INPUT, STAB_INPUT,
		ENUMS(SHAPE_INPUT, N_CH),
		ENUMS(SCALE_INPUT, N_CH),
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(UNI_OUTPUT, N_CH),
		ENUMS(BI_OUTPUT, N_CH),
		EOC_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SYNC_LIGHT,
		ENUMS(SHAPE_LINK_LIGHT, N_CH - 1),
		ENUMS(SCALE_LINK_LIGHT, N_CH - 1),
		LIGHTS_LEN
	};

	static const int N_DIV = 10;
	const float divBars[N_DIV] = {64.f, 32.f, 16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f, 0.125f};

	dsp::SchmittTrigger barTrig, clockTrig, resetTrig, resetBtnTrig;
	dsp::PulseGenerator eocPulse;
	double phase = 0.0, freePhase = 0.0, prevPhase = 0.0;
	float dispOffset[N_CH] = {};
	float  secSinceBar = 0.f, barSec = 0.5f;
	bool   barValid = false;
	bool   sawBar = false;           // a bar pulse has started the stopwatch (first interval isn't a real duration)
	int    barCounter = 0;
	float  secSinceBeat = 0.f, beatSec = 0.25f;
	bool   beatValid = false;
	bool   sawBeat = false;

	float stabPhase[N_CH] = {}, stabTarget[N_CH] = {}, stabVal[N_CH] = {};

	// Shared stepped-random voltages, regenerated each cycle (all 4 channels read this).
	static const int MAX_STEPS = 64;
	float randSteps[MAX_STEPS] = {};
	void fillRand() { for (int k = 0; k < MAX_STEPS; k++) randSteps[k] = 2.f * random::uniform() - 1.f; }

	// Display
	float dispPhase = 0.f, dispFreqHz = 1.f, dispSpread = 0.f, dispStab = 1.f;
	bool  dispClocked = false;
	float dispBarsPerCycle = 1.f;
	int   dispBarInCycle = 0, dispDivIdx = 3, dispNSteps = 4;
	float dispBeatFrac = 0.f;        // beat spacing in phase (for display ticks)
	float dispShape[N_CH] = {}, dispAmp[N_CH] = {}, dispScale[N_CH] = {};

	Cycle() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<FreqQuantity>(FREQ_PARAM, 0.f, (float)(N_DIV - 1), 6.f, "Frequency");   // default 1 bar
		configParam(PHASE_PARAM, 0.f, 1.f, 0.f, "Phase spread", "%", 0.f, 100.f);
		configParam(STAB_PARAM, 0.f, 1.f, 1.f, "Stability", "%", 0.f, 100.f);
		configButton(RESET_PARAM, "Reset (restart cycle)");
		for (int i = 0; i < N_CH - 1; i++) {
			configSwitch(SHAPE_LINK_PARAM + i, 0.f, 1.f, 1.f,
				string::f("Link %c-%c shape", 'A' + i, 'A' + i + 1), {"Off", "On"});
			configSwitch(SCALE_LINK_PARAM + i, 0.f, 1.f, 1.f,
				string::f("Link %c-%c scale", 'A' + i, 'A' + i + 1), {"Off", "On"});
		}
		for (int i = 0; i < N_CH; i++) {
			char c = 'A' + i;
			configParam(SHAPE_PARAM + i, 0.f, 1.f, 0.f, string::f("%c shape", c));
			configParam(SCALE_PARAM + i, -1.f, 1.f, 1.f, string::f("%c scale (bipolar)", c), "%", 0.f, 100.f);
			configInput(SHAPE_INPUT + i, string::f("%c shape CV", c));
			configInput(SCALE_INPUT + i, string::f("%c scale CV", c));
			configOutput(UNI_OUTPUT + i, string::f("%c unipolar (0-5V)", c));
			configOutput(BI_OUTPUT + i, string::f("%c bipolar (±5V)", c));
		}
		configInput(CLOCK_INPUT, "Clock (optional — sets the staircase / stepped-random step grid: one step per pulse)");
		configInput(BAR_INPUT, "Bar (downbeat sync)");
		configInput(RESET_INPUT, "Reset (restart cycle)");
		configOutput(EOC_OUTPUT, "End of cycle (trigger)");
		configInput(FREQ_INPUT, "Frequency CV");
		configInput(PHASE_INPUT, "Phase spread CV");
		configInput(STAB_INPUT, "Stability CV");
		fillRand();
	}

	int divIndex() {
		float fval = clamp(params[FREQ_PARAM].getValue()
			+ inputs[FREQ_INPUT].getVoltage() / 10.f * (N_DIV - 1), 0.f, (float)(N_DIV - 1));
		return clamp((int) std::round(fval), 0, N_DIV - 1);
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		bool barConn = inputs[BAR_INPUT].isConnected();
		secSinceBar += dt;
		secSinceBeat += dt;

		// RESET (button or trigger): restart the cycle from the top.
		bool reset = resetBtnTrig.process(params[RESET_PARAM].getValue());
		if (inputs[RESET_INPUT].isConnected())
			reset |= resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		if (reset) {
			// barCounter = -1 so the next downbeat (barCounter++ → 0) is the
			// cycle start (phase 0), not the second division.
			phase = 0.0; freePhase = 0.0; barCounter = -1; secSinceBar = 0.f;
			// A reset can land mid-bar; don't let the partial interval to the next
			// downbeat be measured as a bar. Keep the existing barSec (stay locked).
			sawBar = false;
			fillRand();
		}

		// Optional clock — measures the beat interval, used to set the
		// stepped-shape step grid (and draw display beat ticks).
		if (inputs[CLOCK_INPUT].isConnected()
			&& clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			// The first pulse only starts the stopwatch — the elapsed time to it is
			// not a beat duration. From the second pulse on, snap to a genuine
			// interval (catches tempo changes); lightly de-jitter a steady one.
			if (sawBeat && secSinceBeat > 1e-4f) {
				if (!beatValid || std::fabs(secSinceBeat - beatSec) > beatSec * 0.1f) beatSec = secSinceBeat;
				else beatSec += (secSinceBeat - beatSec) * 0.25f;
				beatValid = true;
			}
			sawBeat = true;
			secSinceBeat = 0.f;
		}

		bool clocked = barConn && barValid;
		if (paramQuantities[FREQ_PARAM]) paramQuantities[FREQ_PARAM]->snapEnabled = clocked;

		int divIdx = divIndex();
		float barsPerCycle = divBars[divIdx];
		// Stepped-shape (staircase / stepped-random) step count.
		// When CLOCK is patched (alongside BAR), the step grid follows the
		// clock: one step per clock pulse, so the count = clocks-per-cycle.
		// Because phase advances uniformly and is bar-aligned, the step
		// boundaries (pw = k/nSteps) land exactly on the clock beats — no
		// per-pulse counting needed. Otherwise fall back to the bar division.
		bool clockGrid = clocked && inputs[CLOCK_INPUT].isConnected()
			&& beatValid && beatSec > 1e-4f;
		int nSteps;
		if (clockGrid) {
			float cycleSec = std::max(1e-4f, barSec * barsPerCycle);
			nSteps = clamp((int) std::round(cycleSec / beatSec), 2, 64);
		} else {
			nSteps = clamp((int) std::round(barsPerCycle >= 1.f ? barsPerCycle : 1.f / barsPerCycle), 2, 64);
		}

		// BAR: measure duration, hard-align the cycle to the bar grid.
		if (barConn && barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f)) {
			// The first pulse only starts the stopwatch — the time to it is a phase
			// offset, not a bar duration, so measuring it (as before) locked Cycle to
			// a wrong tempo that took several bars to converge. From the second pulse
			// on, snap to a genuine interval (locks in one bar, tracks tempo changes);
			// lightly de-jitter a steady clock.
			if (sawBar && secSinceBar > 1e-4f) {
				if (!barValid || std::fabs(secSinceBar - barSec) > barSec * 0.1f) barSec = secSinceBar;
				else barSec += (secSinceBar - barSec) * 0.25f;
				barValid = true;
			}
			sawBar = true;
			secSinceBar = 0.f;
			barCounter++;
			if (barsPerCycle >= 1.f) {
				int K = (int) std::round(barsPerCycle);
				phase = (double)(((barCounter % K) + K) % K) / (double) K;
			} else {
				phase = 0.0;
			}
		}

		// Advance phase
		float freqHz;
		if (clocked) {
			float cycleSec = std::max(1e-4f, barSec * barsPerCycle);
			freqHz = 1.f / cycleSec;
			phase += (double)(freqHz * dt);
			if (phase >= 1.0) phase -= std::floor(phase);
			freePhase = phase;
		} else {
			float fval = clamp(params[FREQ_PARAM].getValue()
				+ inputs[FREQ_INPUT].getVoltage() / 10.f * (N_DIV - 1), 0.f, (float)(N_DIV - 1));
			freqHz = 0.02f * std::pow(1000.f, fval / (N_DIV - 1));   // 0.02–20 Hz
			freePhase += (double)(freqHz * dt);
			if (freePhase >= 1.0) freePhase -= std::floor(freePhase);
			phase = freePhase;
		}
		dispFreqHz = freqHz;

		// Controls + stability wander
		float spread = clamp(params[PHASE_PARAM].getValue() + inputs[PHASE_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float stab = clamp(params[STAB_PARAM].getValue() + inputs[STAB_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float stabAmt = 1.f - stab;

		// Raw per-channel controls.
		float rawShape[N_CH], rawScale[N_CH];
		for (int i = 0; i < N_CH; i++) {
			// SHAPE is circular — keep it unwrapped here (pot + CV); the wrap is
			// applied after the link offset so CV rotates around the ring.
			rawShape[i] = params[SHAPE_PARAM + i].getValue() + inputs[SHAPE_INPUT + i].getVoltage() / 10.f;
			rawScale[i] = clamp(params[SCALE_PARAM + i].getValue() + inputs[SCALE_INPUT + i].getVoltage() / 10.f, -1.f, 1.f);
		}
		// LINK toggles sync adjacent channels' shape / scale: a linked channel
		// follows the leftmost channel of its contiguous group.
		int shLead[N_CH], scLead[N_CH]; shLead[0] = 0; scLead[0] = 0;
		for (int i = 1; i < N_CH; i++) {
			shLead[i] = (params[SHAPE_LINK_PARAM + i - 1].getValue() > 0.5f) ? shLead[i - 1] : i;
			scLead[i] = (params[SCALE_LINK_PARAM + i - 1].getValue() > 0.5f) ? scLead[i - 1] : i;
		}
		for (int i = 0; i < N_CH - 1; i++) {
			lights[SHAPE_LINK_LIGHT + i].setBrightness(params[SHAPE_LINK_PARAM + i].getValue() > 0.5f ? 1.f : 0.f);
			lights[SCALE_LINK_LIGHT + i].setBrightness(params[SCALE_LINK_PARAM + i].getValue() > 0.5f ? 1.f : 0.f);
		}
		for (int i = 0; i < N_CH; i++) {
			float rate = 0.5f + 0.13f * i;
			stabPhase[i] += dt * rate;
			if (stabPhase[i] >= 1.f) { stabPhase[i] -= 1.f; stabTarget[i] = 2.f * random::uniform() - 1.f; }
			stabVal[i] += (stabTarget[i] - stabVal[i]) * clamp(dt * 8.f, 0.f, 1.f);
		}

		for (int i = 0; i < N_CH; i++) {
			// When linked, a follower's own pot acts as an offset from the group
			// leader, measured from the pot's default: at the default position it
			// matches the leader exactly, turning it deviates from there.
			// SHAPE is a ring, so its offset WRAPS (rotates around the loop);
			// SCALE is bounded amplitude, so its offset clamps.
			float shapeRaw = (shLead[i] == i) ? rawShape[i]
				: rawShape[shLead[i]] + rawShape[i];        // follower pot = circular offset
			float shape = shapeRaw - std::floor(shapeRaw);  // wrap into [0,1)
			float scale = (scLead[i] == i) ? rawScale[i]
				: clamp(rawScale[scLead[i]] + (rawScale[i] - 1.f), -1.f, 1.f);
			float ampF = clamp(1.f + stabAmt * stabVal[i] * 0.6f, 0.2f, 1.6f);
			float off = i * 0.25f * spread;
			dispShape[i] = shape; dispAmp[i] = ampF; dispScale[i] = scale; dispOffset[i] = off;
			float ph = (float) phase + off;
			float pw = ph - std::floor(ph);
			float randVal = randSteps[clamp((int)(pw * nSteps), 0, nSteps - 1)];
			float w = lfoShape(ph, shape, nSteps, randVal) * ampF;
			float ws = clamp(w * scale, -1.f, 1.f);
			outputs[BI_OUTPUT + i].setVoltage(ws * 5.f);
			outputs[UNI_OUTPUT + i].setVoltage((ws * 0.5f + 0.5f) * 5.f);
		}

		// Display
		dispPhase = (float) phase; dispClocked = clocked;
		dispBarsPerCycle = barsPerCycle; dispDivIdx = divIdx; dispNSteps = nSteps;
		dispSpread = spread; dispStab = stab;
		if (clocked && barsPerCycle >= 1.f) {
			int K = (int) std::round(barsPerCycle);
			dispBarInCycle = clamp((int) std::floor((float) phase * K), 0, K - 1);
		}
		dispBeatFrac = (clocked && beatValid && barSec > 1e-4f)
			? clamp(beatSec / (barSec * barsPerCycle), 0.f, 1.f) : 0.f;
		lights[SYNC_LIGHT].setBrightness(clocked ? 1.f : 0.f);

		// End of cycle: a big backward jump in phase = a wrap (cycle completed).
		// Skip on manual reset so RESET doesn't emit an EOC.
		if (!reset && phase + 0.5 < prevPhase) { eocPulse.trigger(1e-3f); fillRand(); }
		prevPhase = phase;
		outputs[EOC_OUTPUT].setVoltage(eocPulse.process(dt) ? 10.f : 0.f);
	}
};

std::string FreqQuantity::getDisplayValueString() {
	Cycle* m = dynamic_cast<Cycle*>(module);
	if (!m) return ParamQuantity::getDisplayValueString();
	if (m->dispClocked) {
		float b = m->divBars[clamp((int) std::round(getValue()), 0, Cycle::N_DIV - 1)];
		if (b >= 1.f) return string::f("%d bar%s / cycle", (int) b, (b > 1.f) ? "s" : "");
		return string::f("1/%d bar / cycle", (int) std::round(1.f / b));
	}
	return string::f("%.2f Hz (free)", m->dispFreqHz);
}


// ─── Display ─────────────────────────────────────────────────────────────────
struct CycleDisplay : OpaqueWidget {
	Cycle* module = nullptr;
	std::shared_ptr<Font> font;
	static const int SEG = 128;

	NVGcolor chanCol(int i) {
		switch (i) {
			case 0: return nvgRGB(0x00, 0x97, 0xde);
			case 1: return nvgRGB(0x1f, 0xbc, 0x17);
			case 2: return nvgRGB(0xdd, 0x64, 0x00);
			default: return nvgRGB(0x69, 0x2f, 0xbc);
		}
	}

	void drawContent(NVGcontext* vg, bool preview) {
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
		nvgFillColor(vg, nvgRGB(0x1a, 0x1a, 0x2e)); nvgFill(vg);
		nvgStrokeColor(vg, nvgRGB(0x35, 0x35, 0x4d)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		nvgIntersectScissor(vg, 1.f, 1.f, w - 2.f, h - 2.f);

		float padL = 18.f, padR = 20.f, padT = 5.f, padB = 12.f;
		float plotW = w - padL - padR, plotH = h - padT - padB;
		float midY = padT + plotH * 0.5f;

		// bar/beat division gridlines (when locked)
		if (!preview && module->dispClocked && module->dispBarsPerCycle >= 1.f) {
			int K = (int) std::round(module->dispBarsPerCycle);
			for (int k = 1; k < K; k++) {
				float x = padL + (float) k / K * plotW;
				nvgBeginPath(vg); nvgMoveTo(vg, x, padT); nvgLineTo(vg, x, padT + plotH);
				nvgStrokeColor(vg, nvgRGBA(0x35, 0x35, 0x4d, 0xff)); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			}
		}
		float bf = preview ? 0.f : module->dispBeatFrac;
		if (bf > 0.02f) {
			// Counted, not accumulated: repeatedly adding a float drifts, and at
			// small bf the last tick lands somewhere other than where the maths
			// says it should.
			for (int bi = 1; (float)bi * bf < 0.999f; bi++) {
				float x = padL + (float)bi * bf * plotW;
				nvgBeginPath(vg); nvgMoveTo(vg, x, padT + plotH - 3.f); nvgLineTo(vg, x, padT + plotH);
				nvgStrokeColor(vg, nvgRGBA(0x50, 0x5a, 0x70, 0xcc)); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
			}
		}

		nvgBeginPath(vg); nvgMoveTo(vg, padL, midY); nvgLineTo(vg, w - padR, midY);
		nvgStrokeColor(vg, nvgRGBA(0x35, 0x35, 0x4d, 0xaa)); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);

		for (int i = 0; i < N_CH; i++) {
			float shape = preview ? (i * 0.33f) : module->dispShape[i];
			float scale = preview ? 0.9f : module->dispScale[i];
			float amp   = preview ? 1.f : module->dispAmp[i];
			float off   = preview ? (i * 0.25f * 0.6f) : module->dispOffset[i];
			int   n     = preview ? 4 : module->dispNSteps;
			if (std::fabs(scale) < 0.01f) continue;
			nvgBeginPath(vg);
			for (int s = 0; s <= SEG; s++) {
				float t = (float) s / SEG;
				float ph = t + off;
				float pw = ph - std::floor(ph);
				// Bounded against the ARRAY, not just against n. n comes from
				// another thread and is clamped to 64 a hundred lines away in a
				// different function, so nothing here can see that it is safe --
				// which is precisely what the analyser objected to.
				int ridx = clamp((int)(pw * n), 0, std::min(n, Cycle::MAX_STEPS) - 1);
				float rv = preview ? std::sin(ridx * 2.4f) : module->randSteps[ridx];
				float v = clamp(lfoShape(ph, shape, n, rv) * amp * scale, -1.f, 1.f);
				float x = padL + t * plotW;
				float y = midY - v * (plotH * 0.5f);
				if (s == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
			}
			NVGcolor c = chanCol(i);
			nvgStrokeColor(vg, nvgRGBA((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 0xcc));
			nvgStrokeWidth(vg, 1.3f); nvgStroke(vg);
		}

		float cph = preview ? 0.32f : module->dispPhase;
		float cx = padL + cph * plotW;
		nvgBeginPath(vg); nvgMoveTo(vg, cx, padT); nvgLineTo(vg, cx, padT + plotH);
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0xcc)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font && font->handle >= 0) {
			nvgFontFaceId(vg, font->handle);

			// Vertical-axis voltage scale: bipolar (±5V) on the left, unipolar
			// (0-5V) on the right. Top = max, midline = 0, bottom = min.
			float botY = padT + plotH;
			nvgFontSize(vg, 5.f);
			nvgFillColor(vg, nvgRGBA(0x80, 0x90, 0xb0, 0xcc));
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgText(vg, padL - 3.f, padT, "+5", NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgText(vg, padL - 3.f, midY, "0", NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
			nvgText(vg, padL - 3.f, botY, "-5", NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgText(vg, w - padR + 3.f, padT, "5", NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(vg, w - padR + 3.f, midY, "2.5", NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			nvgText(vg, w - padR + 3.f, botY, "0", NULL);
			// Column headers
			nvgFontSize(vg, 4.f);
			nvgFillColor(vg, nvgRGBA(0x60, 0x6c, 0x88, 0xcc));
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
			nvgText(vg, padL - 3.f, padT - 0.5f, "BI", NULL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			nvgText(vg, w - padR + 3.f, padT - 0.5f, "UNI", NULL);

			nvgFontSize(vg, 8.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			bool clocked = preview ? true : module->dispClocked;
			float bpc = preview ? 2.f : module->dispBarsPerCycle;
			if (clocked) {
				nvgFillColor(vg, nvgRGBA(0xf0, 0xc0, 0x60, 0xd0));
				if (bpc >= 1.f) {
					int K = (int) std::round(bpc);
					int e = (preview ? 0 : module->dispBarInCycle) + 1;
					nvgText(vg, padL + 2.f, h - 2.f, string::f("BAR %d/%d", e, K).c_str(), NULL);
					nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
					nvgFillColor(vg, nvgRGBA(0x80, 0x90, 0xb0, 0xc0));
					nvgText(vg, w - padR - 2.f, h - 2.f, string::f("%d left", K - e + 1).c_str(), NULL);
				} else {
					nvgText(vg, padL + 2.f, h - 2.f, string::f("%dx / bar", (int) std::round(1.f / bpc)).c_str(), NULL);
				}
			} else {
				nvgFillColor(vg, nvgRGBA(0x80, 0x90, 0xb0, 0xd0));
				nvgText(vg, padL + 2.f, h - 2.f, string::f("FREE %.2f Hz", preview ? 0.5f : module->dispFreqHz).c_str(), NULL);
			}
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		drawContent(args.vg, module == nullptr);
		OpaqueWidget::drawLayer(args, layer);
	}
};


struct CycleLabels : Widget {
	std::shared_ptr<Font> font;
	struct L { Vec p; std::string t; float sz; };
	std::vector<L> labels;
	void add(float x, float y, const std::string& t, float sz = 6.f) { labels.push_back({Vec(x, y), t, sz}); }
	void draw(const DrawArgs& args) override {
		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font || font->handle < 0) return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFillColor(args.vg, nvgRGB(0x40, 0x40, 0x40));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		for (auto& l : labels) { nvgFontSize(args.vg, l.sz); nvgText(args.vg, mm2px(l.p.x), mm2px(l.p.y), l.t.c_str(), NULL); }
		Widget::draw(args);
	}
};


struct CycleWidget : ModuleWidget {
	CycleWidget(Cycle* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/cycle.svg")));

		CycleDisplay* disp = new CycleDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(5.08f, 11.85f));
		disp->box.size = mm2px(Vec(121.92f, 29.63f));
		addChild(disp);

		// Globals — left-most column. Knobs top→bottom: PHASE, STAB, FREQ,
		// each paired with its CV jack to the right.
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 60.95f)), module, Cycle::PHASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 81.27f)), module, Cycle::STAB_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 101.59f)), module, Cycle::FREQ_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4f, 60.95f)), module, Cycle::PHASE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4f, 81.27f)), module, Cycle::STAB_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4f, 101.59f)), module, Cycle::FREQ_INPUT));
		// Bottom-left cluster: BAR | SYNC light | CLOCK (the beat/step grid).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 121.91f)), module, Cycle::BAR_INPUT));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(17.78f, 121.91f)), module, Cycle::SYNC_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4f, 121.91f)), module, Cycle::CLOCK_INPUT));
		// Bottom-right: RESET button + RESET CV + EOC output.
		addParam(createParamCentered<VCVButton>(mm2px(Vec(81.27f, 121.91f)), module, Cycle::RESET_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(96.52f, 121.91f)), module, Cycle::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(121.92f, 121.91f)), module, Cycle::EOC_OUTPUT));

		// Channels as rows (A–D top→bottom): SHAPE pot/CV, SCALE pot/CV, then
		// UNI + BI outputs grouped on the right. Link latches sit between rows,
		// aligned to the SHAPE / SCALE columns. Panel labels live in the SVG.
		const float rowY[N_CH] = {60.95f, 76.19f, 91.43f, 106.67f};
		const float xShp = 50.8f, xShpCv = 66.03f, xScl = 81.27f, xSclCv = 96.52f, xUni = 111.76f, xBi = 121.92f;
		const float xShpLink = 58.42f, xSclLink = 88.9f;
		for (int i = 0; i < N_CH - 1; i++) {
			float my = (rowY[i] + rowY[i + 1]) * 0.5f;
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
				mm2px(Vec(xShpLink, my)), module, Cycle::SHAPE_LINK_PARAM + i, Cycle::SHAPE_LINK_LIGHT + i));
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
				mm2px(Vec(xSclLink, my)), module, Cycle::SCALE_LINK_PARAM + i, Cycle::SCALE_LINK_LIGHT + i));
		}
		for (int i = 0; i < N_CH; i++) {
			float y = rowY[i];
			addParam(createParamCentered<Trimpot>(mm2px(Vec(xShp, y)), module, Cycle::SHAPE_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xShpCv, y)), module, Cycle::SHAPE_INPUT + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(xScl, y)), module, Cycle::SCALE_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xSclCv, y)), module, Cycle::SCALE_INPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xUni, y)), module, Cycle::UNI_OUTPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xBi, y)), module, Cycle::BI_OUTPUT + i));
		}
	}
};


Model* modelCycle = createModel<Cycle, CycleWidget>("Cycle");
