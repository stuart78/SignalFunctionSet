// =============================================================================
// TOUCH — the hand, not the instrument.
//
// A melody arrives as V/OCT and a gate. Touch decides, per note, HOW that note
// is reached — picked fresh, slid into, slid into and re-struck, hammered,
// pedalled, or left out — and re-emits the melody with that decision baked in.
// Patch its V/OCT and TRIG into Slide (or Loom, or anything with a pitch and an
// exciter) and the line is played rather than merely sequenced.
//
// WHY THIS IS A MODULE AND NOT A KNOB ON SLIDE
// -------------------------------------------
// Slide already decides which STRING a note lands on. That is a question about
// the instrument. This is a question about the player, it needs a note stream
// and a tempo to answer, and the answer is the same whichever instrument is on
// the other end — so it is upstream of the voice, and it is monophonic in, one
// decision out.
//
// THE MODEL
// ---------
// src/slide-articulator.hpp: a first-order Markov chain over articulations
// supplies phrasing inertia, combined in the LOG domain with context features
// (interval size and direction, beat strength, phrase position, how dead the
// previous note is, scale degree, and whether the slide physically fits in the
// time available). Hard physical constraints are applied last, as -inf, so no
// knob can buy an impossible articulation. src/slide-styles.hpp holds ten
// traditions. Everything is logits, so MORPH and temperature are one operation.
//
// WHAT THE HARNESS ESTABLISHED, AND WHAT IT DID NOT
// -------------------------------------------------
// tools/slide-articulation-harness.cpp compiles those two headers directly.
// Read it before trusting any number in them. In short:
//
//  * The style table's absolute slide fractions are a CALIBRATION TARGET, not a
//    result — every preset says "calibrated legato trim +N" in its own comment,
//    and the figures move by up to 30 points with the test melody. They are not
//    on the panel and there is no readout of them, deliberately.
//  * What DOES survive a change of source material is the style ORDERING:
//    Spearman +0.93 to +0.99 across pentatonic, diatonic and chromatic. So MORPH
//    is a real control and the numbers behind it are not.
//  * BIAS is the honest knob: 12.8% -> 65.9% slide fraction, monotone, and what
//    is physically LEGAL stays flat at ~97% across its whole travel. It moves
//    the choice, never the physics.
//  * TEMPERATURE is NOT a commitment control and is therefore in the menu, not
//    on the panel. Raising it barely touches PLUCK (48% -> 40%); what it
//    actually does is convert committed slides into hedged ones and into rests
//    (SHIFT 1.4% -> 17%, REST 0% -> 17%). "Hedge and drop out" is a real thing
//    to want occasionally, but it is not what a front-panel knob should claim.
//  * ACCENT is a hard constraint, not a weight. See NoteRequest::forceAttack.
//
// CLOCK
// -----
// Optional, and it does two jobs, neither of which is "tell me the tempo" —
// the note stream already says that, and inter-onset interval is what the
// physics actually depends on:
//   1. It supplies beat strength, so downbeats get attacks and the grid is
//      audible in the phrasing.
//   2. It gives the glide a DEADLINE. A player departs before the beat in order
//      to land on it; a purely reactive model starts gliding when the note
//      changes and so always arrives a glide-time late. With a clock, the glide
//      is held to the next grid line. This is the one thing here that cannot be
//      done without it.
//
// STILL TO DO — the expander bus. Slide's leftExpander is unused, and the two
// things Touch cannot say over a cable are the per-note glide RATE (Slide's
// GLIDE is knob-only) and the per-string pedal offset. Both want the bus. Until
// then Touch is a standalone processor, which has the compensating virtue of
// working with Loom and with anything else that takes a pitch and a trigger.
// =============================================================================

#include "plugin.hpp"
#include "panel-style.hpp"
#include "slide-styles.hpp"

using namespace sfs::slide;

struct Touch : Module {
	enum ParamId  { STYLEA_PARAM, STYLEB_PARAM, MORPH_PARAM, BIAS_PARAM,
	                TEMP_PARAM,        // menu-only; see appendContextMenu
	                PARAMS_LEN };
	enum InputId  { VOCT_INPUT, GATE_INPUT, ACCENT_INPUT, VEL_INPUT,
	                CLOCK_INPUT, BAR_INPUT, RESET_INPUT, ROOT_INPUT,
	                MORPH_CV_INPUT, BIAS_CV_INPUT, INPUTS_LEN };
	enum OutputId { VOCT_OUTPUT, TRIG_OUTPUT, GATE_OUTPUT, VEL_OUTPUT,
	                SLIDE_OUTPUT, PEDAL_OUTPUT, OUTPUTS_LEN };
	enum LightId  { ENUMS(ART_LIGHT, ART_COUNT), LIGHTS_LEN };

	Articulator art;

	dsp::SchmittTrigger gateTrig, accentTrig, clockTrig, barTrig, resetTrig;

	// --- note timing ---------------------------------------------------------
	// ioiSec is documented as the time to the NEXT note, which no live patch can
	// know. The previous interval is the causal estimate, and it is also what a
	// player is actually using: they feel the tempo they are already in.
	float sinceNote = 0.f, ioiEst = 0.25f;
	float lastNoteVoct = 0.f;      // pitch of the note we last decided on
	bool  havePrev  = false;

	// --- clock ---------------------------------------------------------------
	float sinceClock = 0.f, clockPeriod = 0.f;
	int   clocksSinceBar = 0, clocksPerBar = 0, clockCount = 0;
	bool  pendingPhraseStart = true;

	// --- accent --------------------------------------------------------------
	// An accent pulse and its gate rarely land on the same sample, so the flag is
	// held briefly rather than sampled. 3ms covers a 1ms trigger arriving either
	// side of the gate without ever reaching the next note.
	float accentHold = 0.f;

	float lightDecay[ART_COUNT] = {};
	int   seed = 1;

	Touch() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(STYLEA_PARAM, 0.f, (float)(STYLE_COUNT - 1), 0.f, "Style A");
		configParam(STYLEB_PARAM, 0.f, (float)(STYLE_COUNT - 1),
		            (float)(STYLE_COUNT - 1), "Style B");
		getParamQuantity(STYLEA_PARAM)->snapEnabled = true;
		getParamQuantity(STYLEB_PARAM)->snapEnabled = true;
		// The styles are ordered by measured legato content, so MORPH gets
		// smoother the whole way across. That ordering is the property of the
		// style table that survives a change of source material; the absolute
		// slide fractions behind it do not, which is why none are shown.
		configParam(MORPH_PARAM, 0.f, 1.f, 0.f, "Morph A to B", "%", 0.f, 100.f);
		// +-3 logits. Measured range 12.8% to 65.9% slide, and it cannot buy an
		// articulation the physics forbids -- the hard constraints are applied
		// after it, so what is legal stays flat across the whole travel.
		configParam(BIAS_PARAM, -3.f, 3.f, 0.f, "Slide bias");
		// A real param so it automates and serialises like everything else, but it
		// lives in the menu rather than on the panel -- see appendContextMenu.
		configParam(TEMP_PARAM, 0.05f, 4.f, 1.f, "Temperature");

		configInput(VOCT_INPUT,   "V/OCT (the melody)");
		configInput(GATE_INPUT,   "Gate");
		configInput(ACCENT_INPUT, "Accent (forces a fresh attack)");
		configInput(VEL_INPUT,    "Velocity (0-10V)");
		configInput(CLOCK_INPUT,  "Clock (beat strength, and a deadline for the glide)");
		configInput(BAR_INPUT,    "Bar (phrase start)");
		configInput(RESET_INPUT,  "Reset");
		configInput(ROOT_INPUT,   "Root (1V/oct, for the scale-degree tests)");
		configInput(MORPH_CV_INPUT, "Morph CV (+-5V)");
		configInput(BIAS_CV_INPUT,  "Bias CV (+-5V)");

		configOutput(VOCT_OUTPUT,  "V/OCT (trajectory and vibrato baked in)");
		configOutput(TRIG_OUTPUT,  "Trigger (fires ONLY on a fresh attack)");
		configOutput(GATE_OUTPUT,  "Gate (sustains across a slide)");
		configOutput(VEL_OUTPUT,   "Velocity");
		configOutput(SLIDE_OUTPUT, "Slide (0-10V bump while a glide is in flight)");
		configOutput(PEDAL_OUTPUT, "Pedal V/OCT (moves only on a pedal event)");

		art.setStyles(&getStyle(0), &getStyle(STYLE_COUNT - 1));
		art.setSeed((uint32_t)seed);
	}

	void onReset() override {
		art.reset();
		havePrev = false; sinceNote = 0.f; ioiEst = 0.25f; lastNoteVoct = 0.f;
		clockPeriod = 0.f; clocksPerBar = 0; clocksSinceBar = 0; clockCount = 0;
		pendingPhraseStart = true; accentHold = 0.f;
	}

	float paramCV(int p, int in, float lo, float hi) {
		float v = params[p].getValue();
		if (inputs[in].isConnected())
			v += inputs[in].getVoltage() / 5.f * (hi - lo) * 0.5f;
		return clamp(v, lo, hi);
	}

	void process(const ProcessArgs& args) override {
		const float dt = args.sampleTime;

		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) onReset();

		// ── styles ────────────────────────────────────────────────────────────
		const int ia = clamp((int)std::round(params[STYLEA_PARAM].getValue()), 0, STYLE_COUNT - 1);
		const int ib = clamp((int)std::round(params[STYLEB_PARAM].getValue()), 0, STYLE_COUNT - 1);
		art.setStyles(&getStyle(ia), &getStyle(ib));
		art.setMorph(paramCV(MORPH_PARAM, MORPH_CV_INPUT, 0.f, 1.f));
		art.setSlideBias(paramCV(BIAS_PARAM, BIAS_CV_INPUT, -3.f, 3.f));
		art.setTemperature(params[TEMP_PARAM].getValue());

		// ── clock ─────────────────────────────────────────────────────────────
		sinceClock += dt;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			// One-pole toward the measured period: a jittery clock should not
			// make the glide deadline jump about.
			const float p = sinceClock;
			clockPeriod = (clockPeriod > 0.f) ? clockPeriod + (p - clockPeriod) * 0.25f : p;
			sinceClock = 0.f;
			clocksSinceBar++; clockCount++;
		}
		if (barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f)) {
			// Measured rather than configured, so Touch does not need to be told
			// the time signature -- the same trick Fill uses.
			if (clocksSinceBar > 0) clocksPerBar = clocksSinceBar;
			clocksSinceBar = 0;
			pendingPhraseStart = true;
		}

		// ── accent ────────────────────────────────────────────────────────────
		if (accentTrig.process(inputs[ACCENT_INPUT].getVoltage(), 0.1f, 1.f))
			accentHold = 0.003f;
		if (accentHold > 0.f) accentHold -= dt;

		// ── is this a new note? ───────────────────────────────────────────────
		// A gate edge is one. So is a pitch change under a held gate, because a
		// legato sequencer never drops the gate between notes and that is exactly
		// the case this module exists to articulate.
		sinceNote += dt;
		const float voct = inputs[VOCT_INPUT].getVoltage();
		const bool gateConn = inputs[GATE_INPUT].isConnected();
		const bool gateHigh = inputs[GATE_INPUT].getVoltage() > 1.f;
		bool newNote = gateTrig.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f);
		// Compared against the last NOTE's pitch rather than the last sample's,
		// so a slewed or glided input still resolves to one note per destination
		// instead of either firing continuously or never firing at all.
		if (!newNote && havePrev && (gateHigh || !gateConn)
		    && std::fabs(voct - lastNoteVoct) > 0.04f)
			newNote = true;                       // ~half a semitone
		// With no gate patched there is no edge to start on, so the first note has
		// to be taken on trust. Without this the pitch-change path never gets a
		// previous note to compare against and the module stays silent forever.
		if (!havePrev && !gateConn && inputs[VOCT_INPUT].isConnected())
			newNote = true;

		if (newNote) {
			NoteRequest req;
			req.pitchVolts = voct;
			req.rootVolts  = inputs[ROOT_INPUT].getVoltage();

			if (havePrev) ioiEst = clamp(sinceNote, 0.01f, 8.f);
			req.ioiSec = ioiEst;

			// Beat strength from the grid when there is one. Without a clock every
			// note is equally strong, which is the honest answer -- and the model
			// centres this feature on 0.45, so 0.5 is very nearly neutral.
			req.beatStrength = 0.5f;
			if (inputs[CLOCK_INPUT].isConnected() && clocksPerBar > 0) {
				const int c = clocksSinceBar;
				const int q = std::max(clocksPerBar / 4, 1);
				const int e = std::max(clocksPerBar / 8, 1);
				req.beatStrength = (c == 0) ? 1.00f
				                 : (c % q == 0) ? 0.70f
				                 : (c % e == 0) ? 0.45f : 0.20f;
			}

			req.phraseStart = pendingPhraseStart || !havePrev;
			req.accent = inputs[VEL_INPUT].isConnected()
			           ? clamp(inputs[VEL_INPUT].getVoltage() / 10.f, 0.f, 1.f) : 0.5f;

			// An accent means a right-hand event. Not a nudge toward one: the
			// silent articulations are struck out and PLUCK versus SHIFT is left
			// to the model, which is the part that is genuinely a judgement.
			req.forceAttack = (accentHold > 0.f)
			               || inputs[ACCENT_INPUT].getVoltage() > 1.f;

			// The deadline. Never one that cannot be met -- a glide asked to
			// finish in under 25ms is a jump, so take the tick after instead.
			if (inputs[CLOCK_INPUT].isConnected() && clockPeriod > 1e-4f) {
				float t = clockPeriod - sinceClock;
				while (t < 0.025f) t += clockPeriod;
				req.arriveBySec = t;
			}

			Decision d = art.decide(req);
			lightDecay[clamp(d.artic, 0, ART_COUNT - 1)] = 1.f;

			sinceNote = 0.f;
			havePrev = true;
			lastNoteVoct = voct;
			pendingPhraseStart = false;
		}

		Articulator::Out o = art.process(dt);
		outputs[VOCT_OUTPUT].setVoltage(o.pitchVolts);
		outputs[TRIG_OUTPUT].setVoltage(o.trig ? 10.f : 0.f);
		outputs[GATE_OUTPUT].setVoltage(o.gate ? 10.f : 0.f);
		outputs[VEL_OUTPUT].setVoltage(o.velocity * 10.f);
		outputs[SLIDE_OUTPUT].setVoltage(o.slideCv * 10.f);
		outputs[PEDAL_OUTPUT].setVoltage(o.pedalVolts);

		const float k = dt / 0.12f;
		for (int i = 0; i < ART_COUNT; i++) {
			lightDecay[i] = std::max(0.f, lightDecay[i] - lightDecay[i] * k - k * 0.02f);
			lights[ART_LIGHT + i].setBrightness(lightDecay[i]);
		}
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "seed", json_integer(seed));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "seed")) { seed = (int)json_integer_value(j);
		                                              art.setSeed((uint32_t)seed); }
	}
};

// ── panel ────────────────────────────────────────────────────────────────────
static const float X_KNOB_L = 20.32f, X_KNOB_R = 60.96f;
static const float X_IN[4]  = {13.64f, 31.64f, 49.64f, 67.64f};
static const float X_OUT[6] = {10.5f, 22.6f, 34.7f, 46.8f, 58.9f, 71.0f};
static const float Y_STYLE = 32.f, Y_MB = 54.f;
static const float Y_IN1 = 74.f, Y_IN2 = 88.f, Y_LIGHT = 101.f, Y_OUT = 115.5f;

struct TouchMenuSlider : ui::Slider {
	explicit TouchMenuSlider(engine::ParamQuantity* q) { quantity = q; }
};

struct TouchWidget : ModuleWidget {
	TouchWidget(Touch* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/touch.svg")));

		auto* lab = new sfs::PanelLabels();
		lab->title(4.5f, 11.f, "TOUCH");

		addParam(createParamCentered<RoundLargeBlackKnob>(
			mm2px(Vec(X_KNOB_L, Y_STYLE)), module, Touch::STYLEA_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(
			mm2px(Vec(X_KNOB_R, Y_STYLE)), module, Touch::STYLEB_PARAM));
		lab->knobLarge(X_KNOB_L, Y_STYLE, "STYLE A");
		lab->knobLarge(X_KNOB_R, Y_STYLE, "STYLE B");

		// pot and its CV two cells apart on the same row, joined by a hairline
		addParam(createParamCentered<RoundBlackKnob>(
			mm2px(Vec(X_KNOB_L - 5.08f, Y_MB)), module, Touch::MORPH_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(X_KNOB_L + 5.08f, Y_MB)), module, Touch::MORPH_CV_INPUT));
		lab->pair(X_KNOB_L - 5.08f, Y_MB, "MORPH");

		addParam(createParamCentered<RoundBlackKnob>(
			mm2px(Vec(X_KNOB_R - 5.08f, Y_MB)), module, Touch::BIAS_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(X_KNOB_R + 5.08f, Y_MB)), module, Touch::BIAS_CV_INPUT));
		lab->pair(X_KNOB_R - 5.08f, Y_MB, "BIAS");

		const char* inLab1[4] = {"V/OCT", "GATE", "ACCENT", "VEL"};
		const int   inId1[4]  = {Touch::VOCT_INPUT, Touch::GATE_INPUT,
		                         Touch::ACCENT_INPUT, Touch::VEL_INPUT};
		const char* inLab2[4] = {"CLOCK", "BAR", "RESET", "ROOT"};
		const int   inId2[4]  = {Touch::CLOCK_INPUT, Touch::BAR_INPUT,
		                         Touch::RESET_INPUT, Touch::ROOT_INPUT};
		for (int i = 0; i < 4; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(X_IN[i], Y_IN1)), module, inId1[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(X_IN[i], Y_IN2)), module, inId2[i]));
			lab->jackOnPlate(X_IN[i], Y_IN1, inLab1[i]);
			lab->jackOnPlate(X_IN[i], Y_IN2, inLab2[i]);
		}

		// What the player just did. Six lights is the cheapest possible display
		// and it answers the only question the module raises while you watch it.
		const char* artLab[ART_COUNT] = {"PLUCK", "SLIDE", "SHIFT", "HAM", "PED", "REST"};
		for (int i = 0; i < ART_COUNT; i++) {
			addChild(createLightCentered<SmallLight<GreenLight>>(
				mm2px(Vec(X_OUT[i], Y_LIGHT)), module, Touch::ART_LIGHT + i));
			lab->note(X_OUT[i], Y_LIGHT - 2.8f, artLab[i]);
		}

		const char* outLab[6] = {"V/OCT", "TRIG", "GATE", "VEL", "SLIDE", "PEDAL"};
		const int   outId[6]  = {Touch::VOCT_OUTPUT, Touch::TRIG_OUTPUT, Touch::GATE_OUTPUT,
		                         Touch::VEL_OUTPUT, Touch::SLIDE_OUTPUT, Touch::PEDAL_OUTPUT};
		for (int i = 0; i < 6; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(X_OUT[i], Y_OUT)), module, outId[i]));
			lab->jackOnPlate(X_OUT[i], Y_OUT, outLab[i]);
		}

		addChild(lab);
	}

	void appendContextMenu(Menu* menu) override {
		Touch* m = dynamic_cast<Touch*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		// Deliberately NOT on the panel. Measured, this does not trade legato for
		// attack -- PLUCK barely moves across its whole range. What it does is
		// convert committed slides into hedged ones and into rests. Useful, but
		// not what a knob labelled with it would promise.
		menu->addChild(createMenuLabel("Temperature — hedging, not commitment"));
		{
			auto* sl = new TouchMenuSlider(m->paramQuantities[Touch::TEMP_PARAM]);
			sl->box.size.x = 200.f;
			menu->addChild(sl);
		}
		menu->addChild(createMenuItem("Re-seed", "", [m]() {
			m->seed = (int)(random::u32() & 0x7FFFFFFF);
			m->art.setSeed((uint32_t)m->seed);
		}));
	}
};

Model* modelTouch = createModel<Touch, TouchWidget>("Touch");
