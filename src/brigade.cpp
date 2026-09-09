#include "plugin.hpp"
#include "panel-style.hpp"
#include "waveguide.hpp"     // sfs::SVF
#include <cmath>

// =============================================================================
// Brigade -- a bucket brigade whose buckets are frequency bands.
//
// A BBD moves charge one bucket along on every clock tick, and what gives it
// its character is not the shifting but the LOSSES: every transfer costs a
// little level and blurs the signal a little, so what arrives at the end of the
// chain is softer and vaguer than what went in. An array index that moves is
// just a rotation; a bucket brigade is a rotation that degrades.
//
// Here the buckets are bands rather than time slots, so content does not travel
// along a delay line, it travels UP THE SPECTRUM. Energy that entered at 200 Hz
// appears at 260 Hz a tick later, at 340 Hz the tick after, fading as it climbs.
// The two axes move together, exactly as they do in a real BBD: the further
// content has travelled, the older it is.
//
// WHY THIS AND NOT A MOVING FILTER. Helix, next door, moves NOTCHES -- removal
// -- in a pattern that is self-similar under an octave shift, and self-similar
// is another way of saying the spectral centre of mass never moves. Measured, a
// full cycle of it drifts the centroid 0.36 semitones, against 39.5 for a gong
// whose partials genuinely die at different rates. This moves CONTENT, so the
// spectrum really is rearranged and the centroid really does travel. It is also
// stepped, because a clock is stepped, and the step is most of the sound.
//
// WHAT IT IS UNDERNEATH is a vocoder with a rotated band mapping, which is not
// new -- Eventide and Kyma both have a spectral shift. The clocked, lossy,
// feedback-wrapped brigade around it is the part that has a character of its
// own, and all of that character lives in TRAIL and BLUR.
// =============================================================================

static const int BG_BANDS = 24;
static const float BG_LO = 40.f, BG_HI = 16000.f;

struct Brigade : Module {
	enum ParamId {
		RATE_PARAM, STRIDE_PARAM, TRAIL_PARAM, BLUR_PARAM,
		WIDTH_PARAM, MIX_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INL_INPUT, INR_INPUT, CLOCK_INPUT,
		RATE_INPUT, STRIDE_INPUT, TRAIL_INPUT, BLUR_INPUT, WIDTH_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUTL_OUTPUT, OUTR_OUTPUT, TICK_OUTPUT, OUTPUTS_LEN };
	enum LightId { TICK_LIGHT, LIGHTS_LEN };

	sfs::SVF bp[2][BG_BANDS];
	float env[BG_BANDS] = {};     // what each band is doing NOW
	float slow[BG_BANDS] = {};    // its own slow average, so we can see a RISE
	float pend[BG_BANDS] = {};    // the rise, peak-held until the next tick
	float bandHz[BG_BANDS] = {};

	// ── the charge packets ──────────────────────────────────────────────────
	// The bank used to be 24 oscillators at fixed frequencies, with the buckets
	// moving AMPLITUDE between them. That is a crossfade, not a movement: the
	// set of pitches never changes, so however the levels are shuffled the
	// result sits in one place. Nothing rises, because nothing's frequency ever
	// rises.
	//
	// So a bucket's contents are now a VOICE that carries its own frequency.
	// It is born where the input put it, and every tick it steps up by STRIDE
	// bands and loses TRAIL, until it is too quiet to keep. That is what a
	// charge packet in a brigade actually is, and it is audible as a thing
	// climbing rather than as a bank of tones trading places.
	//
	// BLUR now has a real job: it is the glide between steps. At zero the voice
	// jumps, which is the stepped sound; wound up it swoops, and the swoop is
	// the movement that was missing.
	struct Voice {
		float hz = 0.f, target = 0.f, amp = 0.f, phase = 0.f;
		bool on = false;
	};
	static const int BG_VOICES = 64;
	Voice voice[BG_VOICES];
	int nextVoice = 0;
	float ratio = 1.f;            // one band, as a frequency ratio

	float clockPhase = 0.f;
	dsp::SchmittTrigger clockTrig;
	dsp::PulseGenerator tickPulse;
	uint32_t rng = 0x2545F491u;

	float dispMax = 0.1f;
	float dispEnv[BG_BANDS] = {};
	float dispTick = 0.f;

	Brigade() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// DEFAULTS THAT ACTUALLY TRAVEL. At the old 0.45 the clock was 2.45 Hz --
		// one minor third every 408 ms -- and TRAIL 0.6 killed a packet after
		// about six steps, so a hit climbed 2.3 octaves over two and a half
		// seconds and read as barely moving. At 8 Hz with TRAIL 0.85 the same hit
		// crosses 6.4 octaves, which is the whole band. The mechanism was never
		// the problem; the numbers in front of it were.
		configParam(RATE_PARAM, 0.f, 1.f, 0.68f, "Clock rate");
		// Bipolar and snapped. The sign is the direction, so climbing and
		// falling are one control rather than two modules, and zero is a
		// legitimate setting: the bank still plays, nothing travels.
		configParam(STRIDE_PARAM, -6.f, 6.f, 1.f, "Stride", " bands");
		getParamQuantity(STRIDE_PARAM)->snapEnabled = true;
		configParam(TRAIL_PARAM, 0.f, 0.98f, 0.85f, "Trail", "%", 0.f, 100.f);
		configParam(BLUR_PARAM, 0.f, 1.f, 0.35f, "Blur per transfer");
		configParam(WIDTH_PARAM, 0.f, 1.f, 0.4f, "Band width");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Mix", "%", 0.f, 100.f);

		configInput(INL_INPUT, "Left / mono audio");
		configInput(INR_INPUT, "Right audio");
		configInput(CLOCK_INPUT, "Clock (overrides the internal rate)");
		configInput(RATE_INPUT, "Rate CV");
		configInput(STRIDE_INPUT, "Stride CV (1V per band)");
		configInput(TRAIL_INPUT, "Trail CV");
		configInput(BLUR_INPUT, "Blur CV");
		configInput(WIDTH_INPUT, "Band width CV");
		configOutput(OUTL_OUTPUT, "Left");
		configOutput(OUTR_OUTPUT, "Right");
		configOutput(TICK_OUTPUT, "Tick");
		configBypass(INL_INPUT, OUTL_OUTPUT);
		configBypass(INR_INPUT, OUTR_OUTPUT);

		// Log spacing, so one bucket is one INTERVAL wherever you are in the
		// spectrum. Twenty-four bands from 40 Hz to 16 kHz is a shade under a
		// minor third each, which makes STRIDE an interval control: one band is
		// a third, three is a little over an octave.
		ratio = std::pow(BG_HI / BG_LO, 1.f / (float)(BG_BANDS - 1));
		for (int k = 0; k < BG_BANDS; k++)
			bandHz[k] = BG_LO * std::pow(ratio, (float)k);
	}

	float frand() {
		rng = rng * 1664525u + 1013904223u;
		return (float)(rng >> 8) * (1.f / 8388608.f) - 1.f;
	}

	float pv(int p, int in, float lo, float hi) {
		float v = params[p].getValue();
		if (inputs[in].isConnected()) v += inputs[in].getVoltage() * 0.1f * (hi - lo);
		return clamp(v, lo, hi);
	}

	// One clock tick: every packet moves STRIDE bands along and loses TRAIL,
	// and whatever the input has newly produced is loaded in as fresh packets.
	void advance(int stride, float trail) {
		// what has been standing there climbs
		for (int i = 0; i < BG_VOICES; i++) {
			Voice& v = voice[i];
			if (!v.on) continue;
			if (stride != 0) v.target *= std::pow(ratio, (float)stride);
			v.amp *= trail;
			// A packet that has run off the end of the chain is gone. It used to
			// wrap around, which was the endless-climb idea from the module next
			// door carried somewhere it does not belong: a brigade has an end.
			if (v.amp < 1e-4f || v.target > 20000.f || v.target < 20.f) v.on = false;
		}
		// and the input loads new ones
		if (stride != 0) {
			for (int k = 0; k < BG_BANDS; k++) {
				if (pend[k] < 1e-3f) { pend[k] = 0.f; continue; }
				// Take a free voice, or the QUIETEST one. Round-robin stole
				// whichever came next, which is as likely to be the loudest
				// packet in flight as the faintest.
				int slot = -1; float worst = 1e9f;
				for (int i = 0; i < BG_VOICES; i++) {
					if (!voice[i].on) { slot = i; break; }
					if (voice[i].amp < worst) { worst = voice[i].amp; slot = i; }
				}
				Voice& v = voice[slot];
				v.on = true;
				v.amp = pend[k];
				v.hz = v.target = bandHz[k];
				// A RANDOM START PHASE. Every packet used to start at zero, so
				// all the ones spawned on a tick began in phase and summed
				// coherently -- N times one packet, where incoherent partials
				// would give sqrt(N).
				v.phase = 0.5f * (frand() + 1.f);
				pend[k] = 0.f;
			}
		}
		dispTick = 1.f;
		tickPulse.trigger(1e-3f);
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float inL = inputs[INL_INPUT].getVoltage();
		float inR = inputs[INR_INPUT].isConnected() ? inputs[INR_INPUT].getVoltage() : inL;

		float trail = pv(TRAIL_PARAM, TRAIL_INPUT, 0.f, 0.98f);
		float blur  = pv(BLUR_PARAM, BLUR_INPUT, 0.f, 1.f);
		float wKnob = pv(WIDTH_PARAM, WIDTH_INPUT, 0.f, 1.f);
		float mix   = params[MIX_PARAM].getValue();
		int stride = (int)std::round(clamp(params[STRIDE_PARAM].getValue()
			+ inputs[STRIDE_INPUT].getVoltage(), -(float)(BG_BANDS - 1), (float)(BG_BANDS - 1)));

		// ── the clock ───────────────────────────────────────────────────────
		if (inputs[CLOCK_INPUT].isConnected()) {
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
				advance(stride, trail);
		} else {
			float hz = 0.25f * std::pow(160.f, pv(RATE_PARAM, RATE_INPUT, 0.f, 1.f));
			clockPhase += hz / sr;
			if (clockPhase >= 1.f) { clockPhase -= 1.f; advance(stride, trail); }
		}

		// ── analysis: where is the input gaining energy right now ───────────
		float Q = 1.2f * std::pow(10.f, wKnob);
		float aUp = 1.f - std::exp(-1.f / (0.002f * sr));
		float aDn = 1.f - std::exp(-1.f / (0.060f * sr));
		float aSlow = 1.f - std::exp(-1.f / (0.30f * sr));   // what counts as "standing"
		float frameMax = 0.f;
		for (int k = 0; k < BG_BANDS; k++) {
			float f = bandHz[k];
			if (f >= sr * 0.45f) { dispEnv[k] = 0.f; continue; }
			bp[0][k].set(f, Q, sr);
			bp[1][k].set(f, Q, sr);
			float mag = std::fabs(0.5f * (bp[0][k].bandpass(inL) + bp[1][k].bandpass(inR)));
			env[k] += (mag - env[k]) * (mag > env[k] ? aUp : aDn);
			slow[k] += (env[k] - slow[k]) * aSlow;
			float rise = env[k] - slow[k];
			if (rise > pend[k]) pend[k] = rise;
			dispEnv[k] = env[k];
			if (env[k] > frameMax) frameMax = env[k];
		}

		// ── synthesis: the packets, each at its own travelling frequency ────
		// BLUR is the glide. At zero a packet arrives at the next band in one
		// sample, which is the stepped sound; wound up it swoops there, and the
		// swoop is what reads as movement. It is also what keeps a step from
		// clicking, so its floor is short rather than zero.
		float ga = 1.f - std::exp(-1.f / ((0.0015f + 0.35f * blur) * sr));
		float wet = 0.f;
		int live = 0;
		for (int i = 0; i < BG_VOICES; i++) {
			Voice& v = voice[i];
			if (!v.on) continue;
			v.hz += (v.target - v.hz) * ga;
			if (v.hz >= sr * 0.45f) { v.on = false; continue; }
			v.phase += v.hz / sr;
			v.phase -= std::floor(v.phase);
			wet += v.amp * std::sin(2.f * (float)M_PI * v.phase);
			live++;
			if (v.amp > frameMax) frameMax = v.amp;
		}
		// DIVIDE BY THE DENSITY. Dozens of packets can be in flight at once, and
		// summed raw they were 20.7 dB above the dry signal in RMS -- so at MIX
		// 10% the wet was still LOUDER than the source it was supposed to be
		// sitting behind. Incoherent partials add as sqrt(N), so that is what
		// this takes back out; without it MIX has no usable lower half.
		if (live > 1) wet /= std::sqrt((float)live);
		dispMax += (frameMax - dispMax) * (frameMax > dispMax ? 0.3f : 0.0005f);
		if (dispMax < 0.02f) dispMax = 0.02f;
		// x2 rather than x3, chosen so a full MIX sits about level with the dry
		// instead of on top of the clipper. The old figure was arbitrary, and
		// what it mostly did was pin the output at the soft clip, which is why
		// the wet had the RMS of a sustained tone against a percussive source.
		wet = sfs::softClip(wet * 2.f);

		// ADDED to the source, not crossfaded with it. Only what has travelled
		// is in the packets, so the shimmer happens on top of the input rather
		// than replacing it, and MIX is how much of it there is.
		outputs[OUTL_OUTPUT].setVoltage(inL + wet * mix);
		outputs[OUTR_OUTPUT].setVoltage(inR + wet * mix);
		outputs[TICK_OUTPUT].setVoltage(tickPulse.process(args.sampleTime) ? 10.f : 0.f);
		dispTick = std::max(0.f, dispTick - args.sampleTime / 0.08f);
		lights[TICK_LIGHT].setBrightness(dispTick);
	}
};

// =============================================================================
// Display -- the buckets, as a row of levels with content climbing through them.
// =============================================================================
struct BrigadeDisplay : Widget {
	Brigade* module = nullptr;
	std::shared_ptr<Font> font;

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, sfs::SCREEN_BG);
		nvgFill(vg);

		float x0 = 5.f, w = box.size.x - 10.f;
		float baseY = box.size.y - 10.f, topY = 12.f;
		float cw = w / (float)BG_BANDS;

		// The input's own spectrum: an outline, because it is what went IN and is
		// not what you are hearing.
		float sc = module ? 1.f / std::max(module->dispMax, 0.02f) : 2.f;
		for (int k = 0; k < BG_BANDS; k++) {
			float e = module ? module->dispEnv[k] : 0.25f * std::exp(-0.16f * k);
			float x = x0 + k * cw;
			float he = clamp(e * sc, 0.f, 1.f) * (baseY - topY);
			nvgBeginPath(vg);
			nvgRect(vg, x + 1.f, baseY - he, cw - 2.f, he);
			nvgStrokeColor(vg, nvgTransRGBA(sfs::SCREEN_PMID, 150));
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
		}
		// The packets, each drawn where its own frequency actually is. Drawing
		// the BAND levels was drawing the wrong thing once the packets started
		// carrying their own pitch: what climbs is a voice, not a bar.
		auto fx = [&](float hz) {
			return x0 + w * clamp(std::log2(clamp(hz, BG_LO, BG_HI) / BG_LO)
			                      / std::log2(BG_HI / BG_LO), 0.f, 1.f);
		};
		if (module) {
			for (int i = 0; i < Brigade::BG_VOICES; i++) {
				const Brigade::Voice& v = module->voice[i];
				if (!v.on || v.amp < 1e-4f) continue;
				float x = fx(v.hz);
				float h = clamp(v.amp * sc, 0.f, 1.f) * (baseY - topY);
				nvgBeginPath(vg);
				nvgMoveTo(vg, x, baseY);
				nvgLineTo(vg, x, baseY - h);
				nvgStrokeColor(vg, sfs::SCREEN_HOT);
				nvgStrokeWidth(vg, 1.6f);
				nvgStroke(vg);
				nvgBeginPath(vg);
				nvgCircle(vg, x, baseY - h, 2.f);
				nvgFillColor(vg, sfs::SCREEN_HOT);
				nvgFill(vg);
			}
		} else {
			const float demo[5] = {180.f, 420.f, 900.f, 2100.f, 5200.f};
			for (int i = 0; i < 5; i++) {
				float x = fx(demo[i]), h = (baseY - topY) * (0.85f - 0.15f * i);
				nvgBeginPath(vg); nvgMoveTo(vg, x, baseY); nvgLineTo(vg, x, baseY - h);
				nvgStrokeColor(vg, sfs::SCREEN_HOT); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);
				nvgBeginPath(vg); nvgCircle(vg, x, baseY - h, 2.f);
				nvgFillColor(vg, sfs::SCREEN_HOT); nvgFill(vg);
			}
		}
		nvgBeginPath(vg);
		nvgRect(vg, x0, baseY, w, 1.f);
		nvgFillColor(vg, sfs::SCREEN_PURP);
		nvgFill(vg);

		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, x0, 2.f, "40 Hz", NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgText(vg, x0 + w, 2.f, "16k", NULL);
			if (module) {
				int st = (int)std::round(module->params[Brigade::STRIDE_PARAM].getValue());
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
				nvgFillColor(vg, module->dispTick > 0.02f ? sfs::SCREEN_HOT : sfs::SCREEN_DIM);
				nvgText(vg, x0 + w * 0.5f, 2.f,
				        string::f("%+d", st).c_str(), NULL);
			}
		}
	}
};

// =============================================================================
// Widget -- 16HP, the same shape as Helix next door.
// =============================================================================
static const float BG_X[6] = {9.5f, 22.3f, 35.1f, 47.9f, 60.7f, 73.5f};
static const float BG_KY = 76.0f, BG_JY = 92.0f, BG_TY = 110.0f;

struct BrigadeWidget : ModuleWidget {
	BrigadeWidget(Brigade* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/brigade.svg")));

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(6.f, 9.f, "BRIGADE");

		BrigadeDisplay* disp = new BrigadeDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(4.f, 14.f));
		disp->box.size = mm2px(Vec(73.28f, 52.f));
		addChild(disp);

		struct K { int p; int in; const char* t; };
		const K row[6] = {
			{Brigade::RATE_PARAM,   Brigade::RATE_INPUT,   "RATE"},
			{Brigade::STRIDE_PARAM, Brigade::STRIDE_INPUT, "STRIDE"},
			{Brigade::TRAIL_PARAM,  Brigade::TRAIL_INPUT,  "TRAIL"},
			{Brigade::BLUR_PARAM,   Brigade::BLUR_INPUT,   "BLUR"},
			{Brigade::WIDTH_PARAM,  Brigade::WIDTH_INPUT,  "WIDTH"},
			{Brigade::MIX_PARAM,    -1,                    "MIX"},
		};
		for (int i = 0; i < 6; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(BG_X[i], BG_KY)), module, row[i].p));
			if (row[i].in >= 0) {
				addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BG_X[i], BG_JY)), module, row[i].in));
				lbl->pairDown(BG_X[i], BG_KY, BG_JY, row[i].t);
			} else {
				lbl->trim(BG_X[i], BG_KY, row[i].t);
			}
		}
		addChild(createLightCentered<MediumLight<GreenLight>>(
			mm2px(Vec(BG_X[5], BG_JY)), module, Brigade::TICK_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BG_X[0], BG_TY)), module, Brigade::INL_INPUT));
		lbl->jack(BG_X[0], BG_TY, "IN L");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BG_X[1], BG_TY)), module, Brigade::INR_INPUT));
		lbl->jack(BG_X[1], BG_TY, "IN R");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BG_X[2], BG_TY)), module, Brigade::CLOCK_INPUT));
		lbl->jack(BG_X[2], BG_TY, "CLOCK");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BG_X[3], BG_TY)), module, Brigade::TICK_OUTPUT));
		lbl->jackOnPlate(BG_X[3], BG_TY, "TICK");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BG_X[4], BG_TY)), module, Brigade::OUTL_OUTPUT));
		lbl->jackOnPlate(BG_X[4], BG_TY, "OUT L");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BG_X[5], BG_TY)), module, Brigade::OUTR_OUTPUT));
		lbl->jackOnPlate(BG_X[5], BG_TY, "OUT R");
	}
};

Model* modelBrigade = createModel<Brigade, BrigadeWidget>("Brigade");
