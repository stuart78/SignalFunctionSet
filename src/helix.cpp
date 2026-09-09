#include "plugin.hpp"
#include "panel-style.hpp"
#include "waveguide.hpp"     // sfs::SVF
#include "pitchtrack.hpp"
#include <cmath>

// =============================================================================
// Helix -- a barber-pole phaser whose notches sit on the harmonic series.
//
// WHY THE NOTCHES ARE LOCKED TO HARMONICS. An ordinary phaser places its
// notches in Hz, knowing nothing about the source. On a sparse tone they spend
// most of the sweep sitting BETWEEN partials, doing nothing, and then abruptly
// cross one. That is most of why phasers work on dense material -- guitars,
// pads, drums -- and disappoint on a low sustained note. Notches locked to the
// harmonic grid can never fall in a gap, so the effect is always doing
// something.
//
// WHY IT CAN RISE FOR EVER. A Shepard illusion needs the moving pattern to be
// self-similar under the shift, and needs a STATIONARY amplitude window to hide
// where features are born and where they die. Those two requirements normally
// fight harmonic locking, because a barber pole wants GEOMETRIC spacing and
// harmonics are LINEAR. They stop fighting on one particular spacing: partial
// 2n is exactly an octave above partial n, so a set of notches at harmonics
// k, 2k, 4k, 8k ... is geometrically spaced AND harmonically locked at the same
// time. Sweep k from 1 to 2 and the set becomes 2, 4, 8, 16: every notch lands
// where its neighbour used to be, one is born at the bottom and one dies at the
// top, and the cycle is seamless.
//
// SNAP IS PER NOTCH, NOT ON k. The obvious way to keep every notch on a real
// partial is to quantize k, and it does not work: k only travels from 1 to 2,
// so there is exactly ONE integer in its whole range. Rounding each notch's own
// harmonic index instead gives a different number of steps at each height --
// the lowest notch steps 1, 2 while the seventh steps through 64, 65 ... 128 --
// which is not a compromise but the harmonic series being honest. Ratios are
// far apart down low and dense up high, so that is what landing on partials
// actually looks like.
//
// THE NOTCHES ARE WIDE BY DEFAULT. Measured on Sigma's Psychedelic Gong, an
// amplitude gradient moving across the spectrum drags the spectral centroid
// -39.5 semitones in five seconds while no partial changes frequency at all.
// That is the effect this module is for, and it comes from a broad soft tilt
// rather than from a narrow null. A phaser's notches are narrow because allpass
// cancellation makes them narrow, not because narrow is better.
// =============================================================================

static const int HX_NOTCH = 8;      // notches per channel, an octave apart
static const float HX_C4 = 261.6256f;

struct Helix : Module {
	enum ParamId {
		RATE_PARAM, DEPTH_PARAM, WIDTH_PARAM, SPAN_PARAM,
		TUNE_PARAM, MIX_PARAM, SNAP_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INL_INPUT, INR_INPUT, VOCT_INPUT,
		RATE_INPUT, DEPTH_INPUT, WIDTH_INPUT, SPAN_INPUT,
		SYNC_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUTL_OUTPUT, OUTR_OUTPUT, PHASE_OUTPUT, OUTPUTS_LEN };
	enum LightId { SNAP_LIGHT, LIGHTS_LEN };

	// ── one notch, and why it is two filters ────────────────────────────────
	// SNAP moves a notch from one whole harmonic to the next in a single
	// sample, which retunes a filter that has STATE, and that is the click.
	// Measured on a 100 Hz harmonic source climbing at 0.05 oct/s, comparing
	// each sample with the one exactly a source period earlier (so the signal
	// cancels and only CHANGE shows): as written it spiked 1.33 V against a
	// typical 0.0027 V. With SNAP off the same test spikes 0.009 V, which is
	// how we know the steps are the whole cause.
	//
	// So the tuning being left behind stays alive in a second filter and the
	// two are crossfaded. The old one is perfectly smooth throughout; the new
	// one's settling transient is faded in over milliseconds instead of
	// arriving in one sample. That takes the spike to 0.60 V.
	//
	// The rest is not an artifact and does not want removing: moving a deep
	// notch from harmonic 4 to harmonic 5 really does un-notch one partial and
	// notch another, and that is the sound of a stepped mode being stepped.
	struct Notch {
		sfs::SVF a, b;
		float fcB = 0.f, xf = 1.f;
		int lastH = -1;
		float process(float x, float fc, int h, float d, float Q, float sr, float xfA) {
			if (lastH < 0) { lastH = h; fcB = fc; }
			if (h != lastH) { b = a; xf = 0.f; lastH = h; }
			xf += (1.f - xf) * xfA;
			a.set(fc, Q, sr);
			float wet;
			if (xf < 0.999f) {
				b.set(fcB, Q, sr);
				wet = (1.f - xf) * b.bandpass(x) + xf * a.bandpass(x);
			} else {
				fcB = fc;
				wet = a.bandpass(x);
			}
			return x - d * wet;
		}
	};
	Notch nt[2][HX_NOTCH];
	float lastK = -1.f;
	sfs::PitchTracker tracker;
	int analyseCounter = 0;

	// Where the stack sits, as a position in OCTAVES. k = 2^phase, and the
	// pattern repeats exactly when phase passes 1, which is what makes the rise
	// endless rather than a sweep that has to fly back.
	float phase = 0.f;
	bool followPitch = true;
	bool snapOn = true;
	bool stereoLink = true;
	// HOW MUCH OF THE STEP TO HIDE. Smoothing the snap steps away was measured
	// as a win (the worst transient fell from 1.33 V to 0.60 V) and was wrong as
	// a musical judgement: the step IS the stepped mode. Smoothed, a
	// harmonic-locked notch bank is just a filter moving around, because the
	// only thing distinguishing it from a filter was that it arrived somewhere
	// suddenly. Slice learned this first and kept it as Clean vs Dirty on its
	// repeat splice, for the same reason: the click is most of what makes a
	// stutter sound like a stutter rather than a loop.
	// 0 = none (the step is the sound), 1 = 2 ms, 2 = 10 ms.
	int stepSmooth = 0;

	// display
	float dispF0 = 110.f, dispHarm[HX_NOTCH] = {}, dispDepth[HX_NOTCH] = {};
	float dispPhase = 0.f, dispSR = 48000.f;
	bool  dispDetected = false;

	dsp::SchmittTrigger syncTrig;

	Helix() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Bipolar and through zero: the centre is STOPPED, and the two halves
		// are the two directions. A rise-only control would make the falling
		// version a second module.
		configParam(RATE_PARAM, -1.f, 1.f, 0.18f, "Rate", " oct/s", 0.f, 2.f);
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.8f, "Depth", "%", 0.f, 100.f);
		configParam(WIDTH_PARAM, 0.f, 1.f, 0.65f, "Notch width");
		configParam(SPAN_PARAM, 0.f, 1.f, 0.5f, "Window span", " oct", 0.f, 6.f);
		configParam(TUNE_PARAM, -4.f, 4.f, 0.f, "Fundamental", " oct");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Mix", "%", 0.f, 100.f);
		configSwitch(SNAP_PARAM, 0.f, 1.f, 1.f, "Snap notches to whole harmonics",
		             {"Continuous", "Snapped"});

		configInput(INL_INPUT, "Left / mono audio");
		configInput(INR_INPUT, "Right audio");
		configInput(VOCT_INPUT, "Fundamental V/oct");
		configInput(RATE_INPUT, "Rate CV");
		configInput(DEPTH_INPUT, "Depth CV");
		configInput(WIDTH_INPUT, "Width CV");
		configInput(SPAN_INPUT, "Window span CV");
		configInput(SYNC_INPUT, "Sync (restarts the climb)");
		configOutput(OUTL_OUTPUT, "Left");
		configOutput(OUTR_OUTPUT, "Right");
		configOutput(PHASE_OUTPUT, "Climb phase (0-10V ramp, one per octave)");
		configBypass(INL_INPUT, OUTL_OUTPUT);
		configBypass(INR_INPUT, OUTR_OUTPUT);
	}

	float pv(int p, int in, float lo, float hi) {
		float v = params[p].getValue();
		if (inputs[in].isConnected()) v += inputs[in].getVoltage() * 0.1f * (hi - lo);
		return clamp(v, lo, hi);
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		dispSR = sr;

		float inL = inputs[INL_INPUT].getVoltage();
		float inR = inputs[INR_INPUT].isConnected() ? inputs[INR_INPUT].getVoltage() : inL;

		// ── the fundamental ─────────────────────────────────────────────────
		tracker.push(0.5f * (inL + inR));
		// Four times a second. The window is 2048 samples of history whatever
		// the rate, so analysing more often costs CPU and tells you nothing new.
		if (++analyseCounter >= (int)(sr / 4.f)) {
			analyseCounter = 0;
			tracker.analyse(sr);
		}
		float manualF0 = HX_C4 * std::pow(2.f, params[TUNE_PARAM].getValue()
		                                     + inputs[VOCT_INPUT].getVoltage());
		bool detected = followPitch && !inputs[VOCT_INPUT].isConnected()
		                && tracker.valid && tracker.f0 > 1.f;
		float f0 = clamp(detected ? tracker.f0 : manualF0, 8.f, sr * 0.45f);
		dispF0 = f0;
		dispDetected = detected;

		// ── the climb ───────────────────────────────────────────────────────
		float rate = pv(RATE_PARAM, RATE_INPUT, -1.f, 1.f) * 2.f;   // octaves/sec
		if (inputs[SYNC_INPUT].isConnected()
			&& syncTrig.process(inputs[SYNC_INPUT].getVoltage(), 0.1f, 1.f))
			phase = 0.f;
		phase += rate / sr;
		phase -= std::floor(phase);           // wraps both ways
		dispPhase = phase;

		float depth = pv(DEPTH_PARAM, DEPTH_INPUT, 0.f, 1.f);
		float wKnob = pv(WIDTH_PARAM, WIDTH_INPUT, 0.f, 1.f);
		float span  = pv(SPAN_PARAM, SPAN_INPUT, 0.f, 1.f) * 6.f + 0.5f;   // octaves
		float mix   = params[MIX_PARAM].getValue();
		snapOn = params[SNAP_PARAM].getValue() > 0.5f;
		lights[SNAP_LIGHT].setBrightness(snapOn ? 1.f : 0.f);

		// WIDTH is the Q of each notch, and it runs the useful way round: at the
		// bottom it is a broad tilt (the gong gradient), at the top a narrow
		// null (the classic phaser tooth).
		float Q = 0.6f * std::pow(24.f, wKnob);

		// The stationary window. Everything about the illusion lives here: it
		// does NOT move with the notches, which is what hides their birth and
		// death. Centred where the ear is most sensitive to spectral change.
		const float winCentreHz = 1200.f;

		float k = std::pow(2.f, phase);

		// AT THE WRAP EVERY FILTER IS ASKED TO BECOME ITS NEIGHBOUR. k falls from
		// 2 to 1, so notch i drops to where notch i-1 was standing a sample ago.
		// The RESPONSE is continuous, which is exactly what makes the climb
		// seamless, but each filter's STATE is suddenly an octave wrong. Hand the
		// states down the stack and the filters are as continuous as the response
		// they compute. lastH has to travel with them: without it every notch
		// compares itself against its neighbour's history, decides it has
		// stepped, and starts a crossfade it does not need -- which measured
		// LOUDER than the click it was there to remove.
		if (lastK > 0.f && k < lastK - 0.5f) {
			for (int c = 0; c < 2; c++) {
				for (int i = HX_NOTCH - 1; i > 0; i--) nt[c][i] = nt[c][i - 1];
				nt[c][0] = Notch();
			}
		}
		lastK = k;

		static const float SMOOTH_MS[3] = {0.f, 2.f, 10.f};
		float sm = SMOOTH_MS[clamp(stepSmooth, 0, 2)];
		float xfA = (sm <= 0.f) ? 1.f : 1.f - std::exp(-1.f / (sm * 0.001f * sr));
		float wetL = inL, wetR = inR;
		for (int i = 0; i < HX_NOTCH; i++) {
			float h = k * std::pow(2.f, (float)i);
			if (snapOn) h = std::max(1.f, std::round(h));
			float fc = clamp(h * f0, 8.f, sr * 0.45f);
			dispHarm[i] = h;

			// raised cosine in log frequency, zero at both edges
			float x = std::log2(fc / winCentreHz) / span;
			float amp = (std::fabs(x) >= 1.f)
			          ? 0.f : 0.5f * (1.f + std::cos((float)M_PI * x));
			float d = depth * amp;
			dispDepth[i] = d;
			// The filters run even when the notch is silent. Skipping them left
			// their state frozen from whenever the window last let them through,
			// so a notch coming back to life spoke with stale state.
			// The crossfade exists for STEPS, so in continuous mode there is
			// nothing to trigger it: a changing index every sample would restart
			// the fade every sample and never let it finish. Toggling SNAP is
			// itself a step, and gets one crossfade, which is what you want.
			int hi = snapOn ? (int)h : 0;
			wetL = nt[0][i].process(wetL, fc, hi, d, Q, sr, xfA);
			if (stereoLink) {
				wetR = nt[1][i].process(wetR, fc, hi, d, Q, sr, xfA);
			} else {
				// Unlinked, the right channel runs half an octave behind, so the
				// pair opens into a moving stereo image instead of one mono
				// gesture heard twice.
				float h2 = k * std::pow(2.f, (float)i + 0.5f);
				if (snapOn) h2 = std::max(1.f, std::round(h2));
				float fc2 = clamp(h2 * f0, 8.f, sr * 0.45f);
				int hi2 = snapOn ? (int)h2 : 0;
				wetR = nt[1][i].process(wetR, fc2, hi2, d, Q, sr, xfA);
			}
		}

		outputs[OUTL_OUTPUT].setVoltage(inL + (wetL - inL) * mix);
		outputs[OUTR_OUTPUT].setVoltage(inR + (wetR - inR) * mix);
		outputs[PHASE_OUTPUT].setVoltage(phase * 10.f);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "followPitch", json_boolean(followPitch));
		json_object_set_new(root, "stereoLink", json_boolean(stereoLink));
		json_object_set_new(root, "stepSmooth", json_integer(stepSmooth));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "followPitch")) followPitch = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "stereoLink")) stereoLink = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "stepSmooth")) stepSmooth = (int)json_integer_value(j);
	}
};

// =============================================================================
// Display -- the harmonic series, the stationary window, and the notches
// climbing through it.
// =============================================================================
struct HelixDisplay : Widget {
	Helix* module = nullptr;
	std::shared_ptr<Font> font;

	// One shared mapping from frequency to x, so the window curve, the notches
	// and the harmonic ticks cannot disagree about where a frequency is.
	float lo() const { return 40.f; }
	float hi() const { return 16000.f; }
	float fx(float f) const {
		float a = std::log2(clamp(f, lo(), hi()) / lo()), b = std::log2(hi() / lo());
		return 4.f + (box.size.x - 8.f) * a / b;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, sfs::SCREEN_BG);
		nvgFill(vg);
		if (!module) { drawPreview(vg); return; }

		float f0 = module->dispF0;
		float baseY = box.size.y - 12.f, topY = 14.f;

		// harmonic ticks: where a notch is ALLOWED to land when SNAP is on
		for (int n = 1; n <= 128; n++) {
			float f = n * f0;
			if (f < lo() || f > hi()) continue;
			float x = fx(f);
			nvgBeginPath(vg);
			nvgRect(vg, x, baseY - 2.f, 1.f, 4.f);
			nvgFillColor(vg, n == 1 ? sfs::SCREEN_BLUE : sfs::SCREEN_PURP);
			nvgFill(vg);
		}
		nvgBeginPath(vg);
		nvgRect(vg, 4.f, baseY, box.size.x - 8.f, 1.f);
		nvgFillColor(vg, sfs::SCREEN_PURP);
		nvgFill(vg);

		// the stationary window, drawn as the ceiling the notches reach toward.
		// It is the shape that never moves, so it is drawn as a fixed outline
		// rather than as anything that could be mistaken for signal.
		nvgBeginPath(vg);
		for (int i = 0; i <= 96; i++) {
			float t = (float)i / 96.f;
			float f = lo() * std::pow(hi() / lo(), t);
			float span = clamp(module->params[Helix::SPAN_PARAM].getValue(), 0.f, 1.f) * 6.f + 0.5f;
			float x2 = std::log2(f / 1200.f) / span;
			float a = (std::fabs(x2) >= 1.f) ? 0.f : 0.5f * (1.f + std::cos((float)M_PI * x2));
			float xx = 4.f + (box.size.x - 8.f) * t, yy = baseY - (baseY - topY) * a;
			if (i == 0) nvgMoveTo(vg, xx, yy); else nvgLineTo(vg, xx, yy);
		}
		nvgStrokeColor(vg, nvgTransRGBA(sfs::SCREEN_PMID, 170));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);

		// the notches themselves, hanging from the window down to the axis
		for (int i = 0; i < HX_NOTCH; i++) {
			float d = module->dispDepth[i];
			if (d < 1e-3f) continue;
			float f = module->dispHarm[i] * f0;
			if (f < lo() || f > hi()) continue;
			float x = fx(f), y = baseY - (baseY - topY) * d;
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, baseY);
			nvgLineTo(vg, x, y);
			nvgStrokeColor(vg, sfs::SCREEN_HOT);
			nvgStrokeWidth(vg, 1.6f);
			nvgStroke(vg);
			nvgBeginPath(vg);
			nvgCircle(vg, x, y, 2.2f);
			nvgFillColor(vg, sfs::SCREEN_HOT);
			nvgFill(vg);
		}

		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, module->dispDetected ? sfs::SCREEN_BLUE : sfs::SCREEN_DIM);
			nvgText(vg, 5.f, 3.f, string::f("%.1f Hz %s", f0,
			        module->dispDetected ? "detected" : "V/oct").c_str(), NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, box.size.x - 5.f, 3.f,
			        module->snapOn ? "SNAP" : "FREE", NULL);
		}
	}

	// The browser thumbnail renders with module == NULL. Without this it is a
	// dark slab that says nothing about what the module does.
	void drawPreview(NVGcontext* vg) {
		float baseY = box.size.y - 12.f, topY = 14.f;
		for (int n = 1; n <= 128; n++) {
			float f = n * 110.f;
			if (f < lo() || f > hi()) continue;
			float x = fx(f);
			nvgBeginPath(vg);
			nvgRect(vg, x, baseY - 2.f, 1.f, 4.f);
			nvgFillColor(vg, n == 1 ? sfs::SCREEN_BLUE : sfs::SCREEN_PURP);
			nvgFill(vg);
		}
		nvgBeginPath(vg);
		for (int i = 0; i <= 96; i++) {
			float t = (float)i / 96.f;
			float f = lo() * std::pow(hi() / lo(), t);
			float x2 = std::log2(f / 1200.f) / 3.5f;
			float a = (std::fabs(x2) >= 1.f) ? 0.f : 0.5f * (1.f + std::cos((float)M_PI * x2));
			float xx = 4.f + (box.size.x - 8.f) * t, yy = baseY - (baseY - topY) * a;
			if (i == 0) nvgMoveTo(vg, xx, yy); else nvgLineTo(vg, xx, yy);
		}
		nvgStrokeColor(vg, nvgTransRGBA(sfs::SCREEN_PMID, 170));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
		const float demo[5] = {3.f, 6.f, 12.f, 24.f, 48.f};
		for (int i = 0; i < 5; i++) {
			float f = demo[i] * 110.f;
			float x2 = std::log2(f / 1200.f) / 3.5f;
			float a = (std::fabs(x2) >= 1.f) ? 0.f : 0.5f * (1.f + std::cos((float)M_PI * x2));
			if (a < 1e-3f) continue;
			float x = fx(f), y = baseY - (baseY - topY) * a * 0.85f;
			nvgBeginPath(vg); nvgMoveTo(vg, x, baseY); nvgLineTo(vg, x, y);
			nvgStrokeColor(vg, sfs::SCREEN_HOT); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);
			nvgBeginPath(vg); nvgCircle(vg, x, y, 2.2f);
			nvgFillColor(vg, sfs::SCREEN_HOT); nvgFill(vg);
		}
	}
};

// =============================================================================
// Widget -- 16HP, laid out from code, so the panel carries no text of its own
// and the labels are drawn at runtime.
// =============================================================================
static const float HX_X[6] = {9.5f, 22.3f, 35.1f, 47.9f, 60.7f, 73.5f};
static const float HX_KY = 76.0f;    // the controls
static const float HX_JY = 92.0f;    // their CV
static const float HX_TY = 110.0f;   // the transport row

struct HelixWidget : ModuleWidget {
	HelixWidget(Helix* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/helix.svg")));

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(6.f, 9.f, "HELIX");

		HelixDisplay* disp = new HelixDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(4.f, 14.f));
		disp->box.size = mm2px(Vec(73.28f, 52.f));
		addChild(disp);

		struct K { int p; int in; const char* t; };
		const K row[6] = {
			{Helix::RATE_PARAM,  Helix::RATE_INPUT,  "RATE"},
			{Helix::DEPTH_PARAM, Helix::DEPTH_INPUT, "DEPTH"},
			{Helix::WIDTH_PARAM, Helix::WIDTH_INPUT, "WIDTH"},
			{Helix::SPAN_PARAM,  Helix::SPAN_INPUT,  "SPAN"},
			{Helix::TUNE_PARAM,  Helix::VOCT_INPUT,  "TUNE"},
			{Helix::MIX_PARAM,   -1,                 "MIX"},
		};
		for (int i = 0; i < 6; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(HX_X[i], HX_KY)), module, row[i].p));
			if (row[i].in >= 0) {
				addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HX_X[i], HX_JY)), module, row[i].in));
				lbl->pairDown(HX_X[i], HX_KY, HX_JY, row[i].t);
			} else {
				lbl->trim(HX_X[i], HX_KY, row[i].t);
			}
		}
		// SNAP sits where MIX's CV jack would be: it is the module's identity,
		// not a menu option, and it wants to be reachable while playing.
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(HX_X[5], HX_JY)), module, Helix::SNAP_PARAM, Helix::SNAP_LIGHT));
		lbl->trim(HX_X[5], HX_JY, "SNAP");

		// The transport row: what is played in and out, and nothing else.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HX_X[0], HX_TY)), module, Helix::INL_INPUT));
		lbl->jack(HX_X[0], HX_TY, "IN L");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HX_X[1], HX_TY)), module, Helix::INR_INPUT));
		lbl->jack(HX_X[1], HX_TY, "IN R");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HX_X[2], HX_TY)), module, Helix::SYNC_INPUT));
		lbl->jack(HX_X[2], HX_TY, "SYNC");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(HX_X[3], HX_TY)), module, Helix::PHASE_OUTPUT));
		lbl->jackOnPlate(HX_X[3], HX_TY, "PHASE");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(HX_X[4], HX_TY)), module, Helix::OUTL_OUTPUT));
		lbl->jackOnPlate(HX_X[4], HX_TY, "OUT L");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(HX_X[5], HX_TY)), module, Helix::OUTR_OUTPUT));
		lbl->jackOnPlate(HX_X[5], HX_TY, "OUT R");
	}

	void appendContextMenu(Menu* menu) override {
		Helix* m = dynamic_cast<Helix*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Follow the input's pitch", "", &m->followPitch));
		menu->addChild(createMenuLabel("(a V/OCT cable overrides it)"));
		menu->addChild(createIndexPtrSubmenuItem("Step smoothing",
			{"None (the step is the sound)", "2 ms", "10 ms"}, &m->stepSmooth));
		menu->addChild(createBoolPtrMenuItem("Link the channels", "", &m->stereoLink));
		menu->addChild(createMenuLabel("(unlinked, the right runs half an octave behind)"));
	}
};

Model* modelHelix = createModel<Helix, HelixWidget>("Helix");
