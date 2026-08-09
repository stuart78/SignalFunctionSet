#include "plugin.hpp"
#include "panel-style.hpp"
#include "waveguide.hpp"
#include "scales.hpp"
#include "slide-messages.hpp"
#include <cmath>
#include <string>
#include <vector>

// =============================================================================
// Slide — an electric lap steel.
//
// Eight waveguide strings stopped by a steel bar rather than by frets. The bar
// is the instrument: a single rigid object lying across every string at once,
// so it moves them all by the same ratio and the tuning's intervals survive
// intact. That is why lap steel is played in 6th and 7th tunings — a straight
// bar is already a chord.
//
// What makes it read as a SLIDE rather than as a pitch bend, in order of how
// much each contributes:
//
//   1. The glide is RATE-based, not time-based. A hand moves along the neck at
//      roughly constant speed, so a twelfth takes twice as long as a fifth.
//      Nearly every synth portamento is constant-time-per-interval, and that is
//      most of why synth glides do not sound like slides.
//   2. (Was SCRAPE, and it is gone. Two models — bandpassed noise, then an
//      impulse train at the winding-crossing rate — and neither sounded like a
//      bar on a string. The physics was right and the result was still wrong.)
//   3. The pickup is at a FIXED point in space while the speaking length
//      changes, so its position as a fraction of the string grows as the bar
//      goes up the neck and the tone hollows out. Loom's comb is a fixed
//      fraction, which is right for a fretted instrument and wrong for this.
//   4. The bar is a lossy, mass-loaded termination — it rests on the string
//      rather than clamping it against wood, so it returns less treble than a
//      fret does.
//   5. Vibrato is a rocking motion: wide, and centred on the pitch rather than
//      bending up to it.
//
// SLANT angles the bar across the strings. It is the technique that gets major,
// minor and dominant voicings out of one tuning without retuning, and it is the
// reason this is its own module rather than a glide mode on Loom.
// =============================================================================

static const int SLIDE_NCH  = 8;
static const int SLIDE_BUF  = 16384;
static const int SLIDE_AP   = 4;
static const int SLIDE_FRETS = 24;      // how far up the neck the bar travels

struct SlideTuning { const char* name; float semis[SLIDE_NCH]; };
// Lap steel lives in 6th and 7th tunings, because a straight bar has to give a
// usable chord — that is the whole premise of the instrument.
static const SlideTuning SLIDE_TUNINGS[] = {
	{"C6",            {0,  4,  7,  9, 12, 16, 19, 21}},
	{"C6 add 9",      {0,  2,  4,  7,  9, 12, 16, 19}},
	{"E7",            {0,  4,  7, 10, 12, 16, 19, 22}},
	{"E13",           {0,  4,  7, 10, 14, 16, 19, 24}},
	{"A6",            {0,  3,  4,  7,  9, 12, 16, 19}},
	{"Open major",    {0,  7, 12, 16, 19, 24, 28, 31}},
	{"Open minor",    {0,  7, 12, 15, 19, 24, 27, 31}},
	{"Dobro G",       {0,  7, 12, 16, 19, 24, 28, 31}},
	{"Fourths",       {0,  5, 10, 15, 20, 25, 30, 35}},
	{"Unison",        {0,  0,  0,  0,  0,  0,  0,  0}},
};
static const int SLIDE_NTUNINGS = (int)(sizeof(SLIDE_TUNINGS) / sizeof(SLIDE_TUNINGS[0]));

// Fingerpicking rolls, as string orders. A roll is a repeating finger pattern,
// not a scale run — which is why these are short and lopsided.
// Each step carries its own weight, because fingerpicking dynamics are not
// random — the thumb is a heavier finger than the index or the middle, and the
// stroke that starts the roll lands hardest. Jitter alone humanises the timing
// of the accents but not their SHAPE, and the shape is what makes a roll sound
// picked rather than sequenced.
struct SlideRoll { const char* name; int n; int s[32]; float v[32]; };
static const SlideRoll SLIDE_ROLLS[] = {
	{"Forward roll",   6, {0, 4, 7, 1, 4, 7},
	                      {1.00f, .68f, .74f, .86f, .66f, .72f}},
	{"Backward roll",  6, {7, 4, 0, 7, 4, 1},
	                      {.80f, .70f, 1.00f, .74f, .68f, .90f}},
	{"Alternating",    8, {0, 5, 1, 6, 2, 7, 3, 6},
	                      {1.00f, .66f, .92f, .68f, .88f, .64f, .86f, .66f}},
	{"Thumb & index",  8, {0, 6, 1, 6, 2, 7, 3, 7},
	                      {1.00f, .64f, .92f, .62f, .88f, .66f, .84f, .62f}},
	{"Inside out",     8, {3, 4, 2, 5, 1, 6, 0, 7},
	                      {.90f, .70f, .85f, .68f, .90f, .66f, 1.00f, .64f}},
	{"Climb",          8, {0, 1, 2, 3, 4, 5, 6, 7},
	                      {1.00f, .78f, .74f, .76f, .72f, .74f, .70f, .76f}},
	{"Fall",           8, {7, 6, 5, 4, 3, 2, 1, 0},
	                      {.72f, .70f, .74f, .72f, .78f, .80f, .86f, 1.00f}},
	{"Pinch",          4, {0, 7, 0, 7}, {1.00f, .80f, .95f, .76f}},
	{"Random",         1, {0}, {1.00f}},
	{"Strum",          1, {0}, {1.00f}},

	// ── register rolls ─────────────────────────────────────────────────────
	// APPENDED, and they must stay at the end: PATTERN_PARAM stores an index,
	// so inserting above re-points every saved patch at a different roll.
	//
	// A 4-bit value stepped by two operators in turn and allowed to overflow,
	// its TOP three bits picking the string. The top bits and not the low ones:
	// `value & 7` throws away exactly the bit that makes a cycle's second half
	// differ from its first, so every additive chain collapses to an 8-step
	// order played twice -- of 3364 chains, none survived that mapping.
	//
	// Only + and - get round all sixteen values. Multiply mod 16 is invertible
	// only for odd v, whose fixed point is 0, and divide discards bits, so those
	// chains fold onto themselves. "Fold" (+3,*5) reaches the full range in
	// spite of that, and the four re-plucks its folding leaves are what make it
	// sound unlike the rest -- a picked pattern, not a run.
	//
	// The accents follow the same rule as the rolls above: the thumb is heavier
	// than the fingers, so bass strings land harder, and the stroke that starts
	// the cycle hardest.
	{"Ladder",        16, {0, 1, 3, 4, 6, 7, 1, 2, 4, 5, 7, 0, 2, 3, 5, 6},
	                      {1.00f, .76f, .75f, .64f, .67f, .53f, .84f, .72f,
	                       .76f, .61f, .58f, .80f, .85f, .68f, .67f, .57f}},
	{"Wide wrap",     16, {0, 3, 7, 2, 6, 1, 5, 0, 4, 7, 3, 6, 2, 5, 1, 4},
	                      {1.00f, .68f, .58f, .72f, .67f, .76f, .67f, .80f,
	                       .76f, .53f, .75f, .57f, .85f, .61f, .84f, .64f}},
	{"Zigzag",        16, {0, 5, 7, 4, 6, 3, 5, 2, 4, 1, 3, 0, 2, 7, 1, 6},
	                      {1.00f, .61f, .58f, .64f, .67f, .68f, .67f, .72f,
	                       .76f, .76f, .75f, .80f, .85f, .53f, .84f, .57f}},
	{"Ramp pair",     16, {0, 6, 1, 7, 2, 0, 3, 1, 4, 2, 5, 3, 6, 4, 7, 5},
	                      {1.00f, .57f, .84f, .53f, .85f, .80f, .75f, .76f,
	                       .76f, .72f, .67f, .68f, .67f, .64f, .58f, .61f}},
	{"Skip two",      16, {0, 1, 5, 6, 2, 3, 7, 0, 4, 5, 1, 2, 6, 7, 3, 4},
	                      {1.00f, .76f, .67f, .57f, .85f, .68f, .58f, .80f,
	                       .76f, .61f, .84f, .72f, .67f, .53f, .75f, .64f}},
	{"Fold",          32, {0, 1, 7, 1, 5, 6, 0, 2, 2, 3, 1, 3, 7, 0, 2, 4,
	                       4, 5, 3, 5, 1, 2, 4, 6, 6, 7, 5, 7, 3, 4, 6, 0},
	                      {1.00f, .76f, .58f, .76f, .71f, .57f, .88f, .72f,
	                       .85f, .68f, .84f, .68f, .62f, .80f, .79f, .64f,
	                       .76f, .61f, .75f, .61f, .89f, .72f, .71f, .57f,
	                       .67f, .53f, .67f, .53f, .80f, .64f, .62f, .80f}},
};

// The thumb is heavier than the fingers, so the bass strings come out louder
// whatever the pattern — this is what Random and Strum use, having no shape of
// their own to follow.
static inline float slideThumbAccent(int stringIdx) {
	return 1.f - 0.34f * ((float)stringIdx / (float)(SLIDE_NCH - 1));
}
static const int SLIDE_NROLLS = (int)(sizeof(SLIDE_ROLLS) / sizeof(SLIDE_ROLLS[0]));

// Where a fret sits along the neck. Real frets are spaced by 2^(-n/12), and
// drawing them evenly would make the display lie about where the bar is.
static inline float slideFretX(float semis) {
	const float endF = 1.f - std::pow(2.f, -(float)SLIDE_FRETS / 12.f);
	return (1.f - std::pow(2.f, -semis / 12.f)) / endF;
}

struct SlideString {
	sfs::DelayLine<SLIDE_BUF> dl;
	float lp = 0.f;
	float apX[SLIDE_AP] = {}, apY[SLIDE_AP] = {};
	float dcX = 0.f, dcY = 0.f;

	float burst = 0.f, burstLen = 1.f, burstAmp = 0.f;
	float excLp = 0.f;
	float velocity = 1.f;
	float dTarget = 0.f, dSm = 0.f;
	float dampC = 0.f;
	float brLp = 0.f;                 // what this string sends to the bridge
	float pending = -1.f, pendVel = 1.f;

	float swell = 1.f;                // the pedal, per note: 0 at the pick
	float live = 0.f;                 // 1 just after picking, decaying — a string
	                                  // the hand is not on is a damped string
	float out = 0.f;                  // what the pickup sees from this string
	float amp = 0.f, flash = 0.f;     // display only

	void clear() {
		dl.clear();
		lp = 0.f; dcX = dcY = 0.f; excLp = 0.f;
		std::memset(apX, 0, sizeof(apX));
		std::memset(apY, 0, sizeof(apY));
		burst = 0.f; pending = -1.f;
		dTarget = dSm = 0.f; dampC = 0.f; brLp = 0.f;
		out = amp = flash = live = 0.f; swell = 1.f;
	}
};

struct Slide : Module {
	// APPEND ONLY. Rack serialises params by INDEX, so inserting one into the
	// middle silently renumbers every param after it and a saved patch loads its
	// values into the wrong controls. BLOCK, SWELL, COUPLE, VIBRATE and DYN were
	// each added mid-enum, which between them shifted ROOT through RESET by five
	// places: a patch's AUTO landed on DYN, AUTO itself fell back to its default
	// of off, and the pitch controls read whatever happened to be five slots
	// along. Everything below the line stays where it is; new params go at the
	// bottom, however untidy that looks.
	enum ParamId {
		BAR_PARAM, SLANT_PARAM, GLIDE_PARAM, VIB_PARAM,
		SCRAPE_PARAM,          // RETIRED — see below; kept so indices do not move
		DECAY_PARAM, DAMP_PARAM, PICK_PARAM,
		PICKUP_PARAM, TONE_PARAM, DRIVE_PARAM,
		ROOT_PARAM, OCT_PARAM,
		PATTERN_PARAM, DENSITY_PARAM,
		AUTO_PARAM, RESET_PARAM,
		// ── appended after the first release; do not reorder ────────────────
		BLOCK_PARAM, SWELL_PARAM, COUPLE_PARAM, VIBRATE_PARAM, DYN_PARAM,
		// ── appended for the 2026-08 panel ──────────────────────────────────
		SCALE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		BAR_CV_INPUT, SLANT_CV_INPUT,
		DECAY_CV_INPUT, DAMP_CV_INPUT, PICK_CV_INPUT, TONE_CV_INPUT,
		VOCT_INPUT, GATE_INPUT, VEL_INPUT,
		CLOCK_INPUT, RESET_INPUT, PATTERN_CV_INPUT, DENSITY_CV_INPUT,
		VOL_INPUT,
		// ── appended for the 2026-08 panel ──────────────────────────────────
		ROOT_CV_INPUT, SCALE_CV_INPUT,
		INPUTS_LEN
	};
	// POLY_OUTPUT is RETIRED. Per-string audio comes out of Slide X now, one
	// jack each. The slot STAYS, because Rack serialises outputs by index and
	// deleting it would re-point every cable in a saved patch onto its
	// neighbour -- the enum is append-only, and retiring in place is how you
	// remove something from it.
	enum OutputId { MIX_L_OUTPUT, MIX_R_OUTPUT, POLY_OUTPUT,
	                EVEN_OUTPUT, ODD_OUTPUT, OUTPUTS_LEN };
	enum LightId { AUTO_LIGHT, LIGHTS_LEN };

	SlideString str[SLIDE_NCH];
	float tune[SLIDE_NCH] = {};

	int curScale = -1;                   // -1 = free; else an index into sfs::SCALES

	// Where the auto player has walked the bar to, as an offset from the BAR
	// knob. A roll on a stationary bar is an arpeggio, not a steel: a player
	// moves between phrases, and it is the MOVING that the instrument is for.
	// Only the destination is chosen here -- GLIDE still decides how the bar
	// gets there, so one control governs every slide in the module.
	float autoBarOff = 0.f;
	bool  autoMovesBar = true;

	// Snap where the bar is GOING, never where it is: the glide has to travel
	// through the notes in between or it stops being a slide. And snap the bar
	// rather than each string, because the bar is one rigid object -- moving
	// strings independently would break the tuning's intervals, which is the
	// whole reason a lap steel is tuned to a chord.
	float snapBar(float semis) const {
		if (curScale < 0 || curScale >= sfs::NUM_SCALES) return semis;
		const sfs::Scale& sc = sfs::SCALES[curScale];
		float best = semis, bestD = 1e9f;
		for (int oct = -2; oct <= 3; oct++)
			for (int d = 0; d < sc.size; d++) {
				float cand = sc.intervals[d] + 12.f * oct;
				float dist = std::fabs(cand - semis);
				if (dist < bestD) { bestD = dist; best = cand; }
			}
		return best;
	}

	// ── the bar ───────────────────────────────────────────────────────────────
	float barSm = 0.f, barPrev = 0.f;    // where the bar IS, and where it was
	float barVel = 0.f;                  // it has to get up to speed and slow down
	float moveDist = 0.f;
	float tremPhase = 0.f, tremPhase2 = 0.f;

	float slantSm = 0.f;
	float vibPhase = 0.f, vibDrift = 0.f;
	float barMotion = 0.f;               // 0 at rest, 1 while the bar is moving

	// ── the pickup ────────────────────────────────────────────────────────────
	sfs::SVF coil, honk;
	float coilSr = 0.f, coilHz = 0.f;
	int   pickupType = 0;             // 0 = modern single coil, 1 = horseshoe
	float coupleBus = 0.f, coupleDcX = 0.f, coupleDcY = 0.f;

	// CHORD: V/OCT transposes the whole instrument and the BAR knob stops it —
	// good for pads and rolls. MELODY: V/OCT is the note you want, and the module
	// does what a player does — picks the string that needs the least bar travel
	// and slides the bar to it. A melodic line then comes out as a series of
	// slides at hand speed, which is the instrument's whole voice.
	int   playMode = 1;                  // 0 = chord, 1 = melody
	int   melodyString = 0;
	int   voiceString[SLIDE_NCH] = {};   // poly: which string plays each note
	int   nVoices = 0;
	int   solveCount = 0;
	float lastNote[SLIDE_NCH] = {};
	float lastSolvedBar = 0.f;
	float stereoWidth = 0.35f;
	int   mouseMode = 0;                 // 0 = hover strums, 1 = click-drag only

	dsp::SchmittTrigger gateTrig, clockTrig, resetTrig, resetBtn;
	dsp::SchmittTrigger polyGate[SLIDE_NCH];
	int   rollIdx = 0, rollWalk = 0;
	float intPhase = 0.f;
	float internalHz = 7.f;

	// display
	float shownBar[SLIDE_NCH] = {};

	Slide() {
		// The mother owns these because it is the one WRITING them.
		rightExpander.producerMessage = new SlideExpanderMessage();
		rightExpander.consumerMessage = new SlideExpanderMessage();

		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// Defaults to the seventh rather than the nut: in melody mode this is the
		// home position the hand works around, and at the nut there is no room to
		// reach anything below the open strings.
		configParam(BAR_PARAM, 0.f, (float)SLIDE_FRETS, 7.f, "Bar position (melody: home position)",
		            " semitones");
		configParam(SLANT_PARAM, -6.f, 6.f, 0.f, "Bar slant", " semitones across the strings");
		configParam(GLIDE_PARAM, 0.f, 1.f, 0.35f, "Glide (bar travel rate)", "%", 0.f, 100.f);
		configParam(VIB_PARAM, 0.f, 1.f, 0.f, "Vibrato depth (bar rocking)", "%", 0.f, 100.f);
		// Steel vibrato is a rocking wrist, and players differ as much in how
		// fast they rock as in how far — a slow wide rock and a fast narrow one
		// are different expressions, not the same one louder.
		configParam(VIBRATE_PARAM, 2.f, 9.f, 5.2f, "Vibrato speed", " Hz");
		// Retired. Two models were tried — bandpassed noise, then an impulse
		// train at the winding-crossing rate — and neither sounded like a bar on
		// a string: the first was hiss, the second a record scratch. The physics
		// is right (a wound string IS a grating, and the rate really does track
		// bar speed) and it still did not work, which is worth remembering
		// before anyone reaches for it again. It stays in the enum because
		// deleting a param renumbers every one after it and breaks saved patches.
		configParam(SCRAPE_PARAM, 0.f, 1.f, 0.f, "Scrape (retired)", "%", 0.f, 100.f);

		configParam(DECAY_PARAM, 0.2f, 20.f, 4.f, "Decay", " s");
		configParam(DAMP_PARAM, 0.f, 1.f, 0.6f, "Damping (treble loss)", "%", 0.f, 100.f);
		configParam(PICK_PARAM, 0.f, 1.f, 0.7f, "Pick hardness", "%", 0.f, 100.f);

		configParam(PICKUP_PARAM, 0.04f, 0.34f, 0.14f, "Pickup position (from the bridge)",
		            "%", 0.f, 100.f);
		configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone (coil resonance)", "%", 0.f, 100.f);
		configParam(DRIVE_PARAM, 0.f, 1.f, 0.2f, "Amp drive", "%", 0.f, 100.f);
		// Lap steel is not strummed: the picking hand damps every string it is
		// not sounding, constantly. Without that an eight-string open tuning is
		// a wash — so this is the technique, not a refinement, and it defaults on.
		configParam(BLOCK_PARAM, 0.f, 1.f, 0.75f, "Blocking (hand damping the strings you are not playing)",
		            "%", 0.f, 100.f);
		// A steel player's foot is on a volume pedal the whole time: pick with it
		// down, then swell in PAST the attack so the pick is never heard. That
		// missing transient is what makes the instrument cry, and it is the one
		// thing a slide model can get right that a portamento cannot fake.
		configParam(SWELL_PARAM, 0.f, 1.f, 0.f, "Swell (volume pedal past the pick attack)",
		            "%", 0.f, 100.f);
		configParam(COUPLE_PARAM, 0.f, 1.f, 0.3f, "Sympathetic coupling through the bridge",
		            "%", 0.f, 100.f);

		configParam(ROOT_PARAM, -12.f, 12.f, 0.f, "Root", " semitones");
		getParamQuantity(ROOT_PARAM)->snapEnabled = true;
		{
			// A bar is not a fret, so nothing here is quantized by construction --
			// which is the point of a steel and also why an auto-played roll can
			// wander out of key. SCALE snaps the BAR's offset, not each string:
			// the bar is one rigid object and moves them all by the same ratio,
			// so snapping per string would break the tuning's intervals, which is
			// the one thing the instrument exists to preserve.
			std::vector<std::string> names{"Off (free)"};
			for (int i = 0; i < sfs::NUM_SCALES; i++) names.push_back(sfs::SCALES[i].longName);
			configSwitch(SCALE_PARAM, 0.f, (float)sfs::NUM_SCALES, 0.f, "Scale", names);
			getParamQuantity(SCALE_PARAM)->snapEnabled = true;
		}
		configParam(OCT_PARAM, -4.f, 2.f, -2.f, "Octave");
		getParamQuantity(OCT_PARAM)->snapEnabled = true;

		std::vector<std::string> rollNames;
		for (int i = 0; i < SLIDE_NROLLS; i++) rollNames.push_back(SLIDE_ROLLS[i].name);
		configSwitch(PATTERN_PARAM, 0.f, (float)(SLIDE_NROLLS - 1), 0.f, "Roll", rollNames);
		getParamQuantity(PATTERN_PARAM)->snapEnabled = true;
		configParam(DENSITY_PARAM, 0.f, 1.f, 1.f, "Roll density", "%", 0.f, 100.f);
		// At zero every stroke is the same, which is what a sequencer sounds
		// like. Turning it up brings in the pattern's own accents first and the
		// note-to-note wobble second.
		configParam(DYN_PARAM, 0.f, 1.f, 0.6f, "Dynamics (pick accent and variation)",
		            "%", 0.f, 100.f);
		configSwitch(AUTO_PARAM, 0.f, 1.f, 0.f, "Auto roll", {"Off", "On"});
		configButton(RESET_PARAM, "Reset the roll");

		configInput(BAR_CV_INPUT,   "Bar position CV (1V per octave of travel)");
		configInput(SLANT_CV_INPUT, "Slant CV (±5V)");
		configInput(DECAY_CV_INPUT, "Decay CV (±5V)");
		configInput(DAMP_CV_INPUT,  "Damping CV (±5V)");
		configInput(PICK_CV_INPUT,  "Pick hardness CV (±5V)");
		configInput(TONE_CV_INPUT,  "Tone CV (±5V)");
		configInput(VOCT_INPUT,     "V/oct — transposes the whole instrument");
		configInput(GATE_INPUT,     "Gate (polyphonic: channel N picks string N; mono picks all)");
		configInput(VEL_INPUT,      "Velocity (0–10V, polyphonic)");
		configInput(CLOCK_INPUT,    "Clock — advances the roll");
		configInput(RESET_INPUT,    "Reset the roll");
		configInput(PATTERN_CV_INPUT, "Roll CV (±5V sweeps all 16)");
		configInput(DENSITY_CV_INPUT, "Roll density CV (±5V)");
		configInput(VOL_INPUT, "Volume pedal (0–10V) — the real answer to swells; overrides SWELL");
		configInput(ROOT_CV_INPUT,  "Root CV (1V/oct, semitone-quantized)");
		configInput(SCALE_CV_INPUT, "Scale CV (1V per scale; 0V = free)");

		configOutput(MIX_L_OUTPUT, "Mix left");
		configOutput(MIX_R_OUTPUT, "Mix right");
		configOutput(POLY_OUTPUT,  "(retired — per-string audio is on the Slide X expander)");
		configOutput(EVEN_OUTPUT, "Even strings (2, 4, 6, 8) summed");
		configOutput(ODD_OUTPUT,  "Odd strings (1, 3, 5, 7) summed");

		applyTuning(0);
	}

	void applyTuning(int t) {
		t = clamp(t, 0, SLIDE_NTUNINGS - 1);
		for (int i = 0; i < SLIDE_NCH; i++) tune[i] = SLIDE_TUNINGS[t].semis[i];
	}

	~Slide() {
		delete (SlideExpanderMessage*) rightExpander.producerMessage;
		delete (SlideExpanderMessage*) rightExpander.consumerMessage;
	}

	void onReset() override {
		applyTuning(0);
		for (int i = 0; i < SLIDE_NCH; i++) str[i].clear();
		barSm = barPrev = slantSm = 0.f;
		stereoWidth = 0.35f; mouseMode = 0; internalHz = 7.f;
		rollIdx = rollWalk = 0;
	}

	void onSampleRateChange() override {
		coilSr = 0.f;
		for (int i = 0; i < SLIDE_NCH; i++) str[i].clear();
	}

	float paramCV(int p, int in, float lo, float hi) {
		float v = params[p].getValue();
		if (in >= 0 && inputs[in].isConnected())
			v += inputs[in].getVoltage() / 5.f * (hi - lo) * 0.5f;
		return clamp(v, lo, hi);
	}

	// Where the bar crosses string i. A slant spreads the stop across the
	// strings — the low string flat of centre, the high string sharp of it.
	float barFor(int i) const {
		float spread = (SLIDE_NCH > 1)
		             ? ((float)i / (float)(SLIDE_NCH - 1) * 2.f - 1.f) : 0.f;
		return std::max(barSm + slantSm * spread, 0.f);
	}

	void pick(int i, float vel) {
		if (i < 0 || i >= SLIDE_NCH) return;
		SlideString& s = str[i];
		s.velocity = clamp(vel, 0.03f, 1.f);
		float pk = paramCV(PICK_PARAM, PICK_CV_INPUT, 0.f, 1.f);
		float sr = APP->engine->getSampleRate();
		// Fingerpicks and a thumbpick: harder and shorter than flesh, which is
		// why lap steel has that bright attack even through a warm amp.
		s.burstLen = std::max(sr * (0.0028f - 0.0022f * pk), 3.f);
		s.burst = s.burstLen;
		s.burstAmp = s.velocity * 0.7f;
		s.excLp = 0.f;
		s.flash = 1.f;
		s.live = 1.f;
		s.swell = 1.f - clamp(params[SWELL_PARAM].getValue(), 0.f, 1.f);
	}

	void strum(float vel) {
		// A strum across eight strings is a roll of the hand, not a chord stab —
		// and the hand lightens as it crosses, so the eight notes are not equal.
		float dyn = clamp(params[DYN_PARAM].getValue(), 0.f, 1.f);
		for (int i = 0; i < SLIDE_NCH; i++) {
			str[i].pending = 0.012f * (float)i;
			float a = slideThumbAccent(i);
			float h = 1.f + dyn * 0.16f * (2.f * random::uniform() - 1.f);
			str[i].pendVel = clamp(vel * (1.f + dyn * (a - 1.f)) * h, 0.05f, 1.f);
		}
	}

	void rollStep() {
		// ±5V sweeps the WHOLE list. At 1V per pattern -- the convention for
		// ROOT and SCALE, which have to mean the same thing across modules --
		// reaching the last of sixteen needs 15V, so an ordinary LFO only ever
		// found the ends. Nothing outside this module reads a roll number, so
		// there is no convention to keep here.
		int pat = clamp((int)std::round(params[PATTERN_PARAM].getValue()
		                + inputs[PATTERN_CV_INPUT].getVoltage() * 0.1f * SLIDE_NROLLS),
		                0, SLIDE_NROLLS - 1);
		float density = paramCV(DENSITY_PARAM, DENSITY_CV_INPUT, 0.f, 1.f);
		float dyn = clamp(params[DYN_PARAM].getValue(), 0.f, 1.f);

		if (std::string(SLIDE_ROLLS[pat].name) == "Strum") {
			if (random::uniform() <= density) strum(0.92f);
			rollIdx++;
			return;
		}
		int sel; float accent;
		if (std::string(SLIDE_ROLLS[pat].name) == "Random") {
			sel = (int)(random::uniform() * SLIDE_NCH);
			accent = slideThumbAccent(clamp(sel, 0, SLIDE_NCH - 1));
		}
		else {
			const SlideRoll& r = SLIDE_ROLLS[pat];
			int step = rollIdx % r.n;
			sel = r.s[step];
			accent = r.v[step];
		}
		// Two layers, and the order matters: the pattern's own accent is the
		// SHAPE and the jitter is only the wobble on top of it. Jitter alone
		// makes every stroke a different random size, which is not how a hand
		// plays — it is how a random number generator plays.
		float vel = 0.92f * (1.f + dyn * (accent - 1.f))
		          * (1.f + dyn * 0.15f * (2.f * random::uniform() - 1.f));
		// One move per completed cycle of the roll, so a phrase gets played
		// somewhere before the hand goes anywhere else.
		const SlideRoll& rr = SLIDE_ROLLS[pat];
		if (autoMovesBar && rr.n > 0 && (rollIdx % rr.n) == rr.n - 1) {
			float step = (random::uniform() < 0.5f ? -1.f : 1.f)
			           * (1.f + std::floor(random::uniform() * 3.f));   // 1..3 semitones
			// Pulled back toward the knob, or a free random walk parks itself
			// against a limit and stays there.
			autoBarOff = clamp((autoBarOff + step) * 0.78f, -7.f, 7.f);
		}
		rollIdx = (rollIdx + 1) & 0xFFFFF;
		if (random::uniform() <= density)
			pick(clamp(sel, 0, SLIDE_NCH - 1), clamp(vel, 0.05f, 1.f));
	}

	void process(const ProcessArgs& args) override {
		const float sr = args.sampleRate;

		// ── the bar ───────────────────────────────────────────────────────────
		float barTarget = params[BAR_PARAM].getValue();
		if (inputs[BAR_CV_INPUT].isConnected())
			barTarget += inputs[BAR_CV_INPUT].getVoltage() * 12.f;
		curScale = clamp((int)std::round(params[SCALE_PARAM].getValue()
		                 + inputs[SCALE_CV_INPUT].getVoltage()), 0, sfs::NUM_SCALES) - 1;
		if (params[AUTO_PARAM].getValue() > 0.5f && autoMovesBar) barTarget += autoBarOff;
		barTarget = snapBar(barTarget);

		if (playMode == 1 && inputs[VOCT_INPUT].isConnected()) {
			int nv = std::max(1, inputs[VOCT_INPUT].getChannels());
			nv = std::min(nv, SLIDE_NCH);
			bool voicesChanged = (nv != nVoices);
			// The BAR knob is the home position the hand works around, not a
			// stop: a note below every open string cannot be played at all going
			// UP the neck, so leaving home at the nut left only the lowest string
			// reachable and the module played everything on it. That, and not the
			// cost function, was the "always one string" problem.
			float home = barTarget;
			float loT = tune[0], hiT = tune[0];
			for (int i = 1; i < SLIDE_NCH; i++) {
				loT = std::min(loT, tune[i]);
				hiT = std::max(hiT, tune[i]);
			}
			float note[SLIDE_NCH];
			bool notesChanged = false;
			for (int j = 0; j < nv; j++) {
				// home is where the hand LIKES to sit, and it belongs in the cost
				// function only. Adding it to the note made the BAR knob a
				// transpose, which also ate the top of the instrument's range:
				// with a scale as wide as the harmonic series the highest note
				// pushed past what the neck can reach and folded an octave DOWN,
				// so a rising line ended lower than it started.
				float n = inputs[VOCT_INPUT].getVoltage(j) * 12.f;
				// A bar only ever RAISES a string's pitch, so a note below every
				// open string cannot be reached at all. A player takes it an
				// octave up rather than dropping it — and the alternative here
				// was worse than either: the old fallback picked an arbitrary
				// string and left the bar where it was, so the note came out at
				// some unrelated pitch with nothing to say that it had.
				int guard = 0;
				while (n < loT - 0.01f && guard++ < 12) n += 12.f;
				while (n > hiT + (float)SLIDE_FRETS + 0.01f && guard++ < 24) n -= 12.f;
				note[j] = n;
				if (std::fabs(n - lastNote[j]) > 1e-4f) notesChanged = true;
				lastNote[j] = n;
			}

			// Solving this every sample is pointless — the notes change at note
			// rate — and it is the only O(strings x voices x candidates) thing here.
			// Solving every sample is wasteful, but solving only every 64 was a
			// bug: a gate arriving in between was handed the PREVIOUS note's
			// string, so the wrong string got picked and the right one stayed
			// silent. Notes change at note rate — so solve when they change.
			if (notesChanged || voicesChanged || (solveCount++ & 63) == 0) {
				// A bar is ONE position serving every string at once, so a chord
				// is not eight independent choices: it is one bar position that
				// best fits all the notes. Candidates are the positions that put
				// some note exactly under the bar on some string.
				float bestBar = barSm; float bestErr = 1e9f;
				for (int j = 0; j < nv; j++) {
					for (int i = 0; i < SLIDE_NCH; i++) {
						float cand = note[j] - tune[i];
						if (cand < -0.01f || cand > (float)SLIDE_FRETS + 0.01f) continue;
						float err = 0.f;
						for (int k = 0; k < nv; k++) {
							float b = 1e9f;
							for (int m = 0; m < SLIDE_NCH; m++)
								b = std::min(b, std::fabs(note[k] - tune[m] - cand));
							err += b;
						}
						// Two tie-breaks, and the second is the one that matters.
						// Minimising bar travel alone is a TIE for most passing
						// notes — up two frets on this string costs exactly what
						// down two frets on the string tuned a third above costs —
						// so it always took the same string and walked the neck.
						// Pulling back toward the home position breaks those ties
						// toward crossing strings, and travels LESS doing it: a
						// major scale goes from one string and 12 semitones of
						// travel to five strings and 8.
						err += 0.10f * std::fabs(cand - barSm)
						     + 0.05f * std::fabs(cand - home);
						if (err < bestErr) { bestErr = err; bestBar = cand; }
					}
				}
				barTarget = bestBar;

				// Now hand the notes out, one string each — the bar is already
				// placed, so this is exactly the choice a player makes.
				bool taken[SLIDE_NCH] = {};
				for (int j = 0; j < nv; j++) {
					int best = -1; float bc = 1e9f;
					for (int i = 0; i < SLIDE_NCH; i++) {
						if (taken[i]) continue;
						float need = note[j] - tune[i];
						if (need < -0.01f || need > (float)SLIDE_FRETS + 0.01f) continue;
						float c = std::fabs(need - bestBar);
						if (c < bc) { bc = c; best = i; }
					}
					if (best < 0) best = clamp(j, 0, SLIDE_NCH - 1);
					taken[best] = true;
					voiceString[j] = best;
				}
				melodyString = voiceString[0];
			}
			else barTarget = lastSolvedBar;                    // hold between solves
			lastSolvedBar = barTarget;
			nVoices = nv;
		}
		barTarget = clamp(barTarget, 0.f, (float)SLIDE_FRETS);

		// Rate-based, not time-based: the hand travels the neck at a roughly
		// constant speed, so a wide move simply takes longer. Constant time per
		// interval is what makes a synth portamento sound like a synth.
		float glide = clamp(params[GLIDE_PARAM].getValue(), 0.f, 1.f);
		float rate = 400.f * std::pow(0.006f, glide);      // semitones per second
		barPrev = barSm;
		float dBar = barTarget - barSm;
		// A hand accelerates away and eases into the note. Moving at exactly the
		// target speed from the first sample to the last, and arriving exactly on
		// pitch, is most of what makes a glide read as a pitch bend rather than
		// as somebody's arm.
		// An S-curve, and the shape has to scale with the DISTANCE or it is not
		// one: a fixed deceleration window makes a short move all ease and a long
		// move a hard ramp with a nub on the end. Ease over a fixed FRACTION of
		// the move instead, so a semitone and a twelfth feel like the same hand.
		// The move's full length, so the ease can be a fraction of it. It only
		// ever grows during a move, because |dBar| shrinks as the bar arrives.
		if (std::fabs(dBar) > moveDist) moveDist = std::fabs(dBar);
		if (std::fabs(dBar) < 1e-4f) moveDist = 0.f;
		float prog = (moveDist > 1e-4f)
		           ? clamp(1.f - std::fabs(dBar) / moveDist, 0.f, 1.f) : 1.f;
		// smoothstep up over the first 18% and down over the last 18%, with a
		// floor so the bar always arrives.
		float upR = clamp(prog / 0.18f, 0.f, 1.f);
		float dnR = clamp((1.f - prog) / 0.18f, 0.f, 1.f);
		float ease = std::min(upR * upR * (3.f - 2.f * upR),
		                      dnR * dnR * (3.f - 2.f * dnR));
		float want = rate * (0.10f + 0.90f * ease);
		barVel += (want - barVel) * (1.f - std::exp(-args.sampleTime / 0.018f));
		float step = barVel * args.sampleTime;
		if (std::fabs(dBar) <= step) { barSm = barTarget; barVel = 0.f; moveDist = 0.f; }
		else barSm += (dBar > 0.f) ? step : -step;

		float slantTarget = paramCV(SLANT_PARAM, SLANT_CV_INPUT, -6.f, 6.f);
		slantSm += (slantTarget - slantSm) * (1.f - std::exp(-args.sampleTime / 0.02f));

		// Rocking the bar: wide, and centred on the note rather than bending up
		// to it, which is what separates slide vibrato from a fretted one.
		float vibDepth = clamp(params[VIB_PARAM].getValue(), 0.f, 1.f);
		vibDrift += (2.f * random::uniform() - 1.f) * 0.0002f;
		vibDrift = clamp(vibDrift, -0.15f, 0.15f);
		vibPhase += args.sampleTime * (params[VIBRATE_PARAM].getValue() + vibDrift * 6.f);
		if (vibPhase >= 1.f) vibPhase -= 1.f;
		float vib = vibDepth * 0.7f * std::sin(2.f * (float)M_PI * vibPhase);

		// Nobody holds a steel bar perfectly still. A few cents of wander, always
		// present and a little wider while the bar is travelling, is the
		// difference between a hand and a control voltage — and it costs nothing.
		tremPhase  += args.sampleTime * 3.1f;  if (tremPhase  >= 1.f) tremPhase  -= 1.f;
		tremPhase2 += args.sampleTime * 7.7f;  if (tremPhase2 >= 1.f) tremPhase2 -= 1.f;
		float trem = (std::sin(2.f * (float)M_PI * tremPhase) * 0.030f
		            + std::sin(2.f * (float)M_PI * tremPhase2) * 0.014f)
		           * (1.f + 1.6f * barMotion);
		vib += trem;

		// ── how fast the bar is travelling ────────────────────────────────────
		// All that survives of SCRAPE. It still widens the hand tremor while the
		// bar is on the move, which is worth having on its own.
		float speed = std::fabs(barSm - barPrev) * sr;    // semitones per second
		float target = clamp(speed / 16.f, 0.f, 1.f);
		barMotion += (target - barMotion) * (1.f - std::exp(-args.sampleTime / 0.004f));

		// ── tone controls ─────────────────────────────────────────────────────
		float dampAmt = paramCV(DAMP_PARAM, DAMP_CV_INPUT, 0.f, 1.f);
		float pickHard = paramCV(PICK_PARAM, PICK_CV_INPUT, 0.f, 1.f);
		float tone    = paramCV(TONE_PARAM, TONE_CV_INPUT, 0.f, 1.f);
		float pickupPos = clamp(params[PICKUP_PARAM].getValue(), 0.02f, 0.4f);
		float drive   = clamp(params[DRIVE_PARAM].getValue(), 0.f, 1.f);
		float decaySec = params[DECAY_PARAM].getValue();
		if (inputs[DECAY_CV_INPUT].isConnected())
			decaySec *= std::pow(2.f, inputs[DECAY_CV_INPUT].getVoltage() / 5.f);
		decaySec = clamp(decaySec, 0.05f, 40.f);

		// Ported from Loom, where it is measured and stable: a string gives up the
		// low end a real bridge transmits and keeps its brightness, the in-phase
		// mode has gain 1, and the ceiling is low because past it the ring stops
		// growing and only the sustain suffers. Slide had NO coupling at all,
		// which is why it had no halo.
		float coupAmt = clamp(params[COUPLE_PARAM].getValue(), 0.f, 1.f);
		float brC = clamp(1.f - std::exp(-2.f * (float)M_PI * 1200.f / sr), 0.01f, 1.f);
		float couplePrev = coupleBus;
		float motion = 0.f;
		float swellAmt = clamp(params[SWELL_PARAM].getValue(), 0.f, 1.f);
		float swellRate = 1.f - std::exp(-args.sampleTime / (0.035f + 0.42f * swellAmt));

		float blockAmt = clamp(params[BLOCK_PARAM].getValue(), 0.f, 1.f);
		float liveDecay = std::exp(-args.sampleTime / 2.5f);
		float dampHz = 500.f * std::pow(12000.f / 500.f, dampAmt);
		float excHz = 900.f * std::pow(11000.f / 900.f, pickHard);
		float excC = clamp(1.f - std::exp(-2.f * (float)M_PI * excHz / sr), 0.01f, 1.f);

		// A magnetic pickup is a resonant lowpass: the coil's inductance against
		// the cable capacitance puts a peak a couple of kHz up, and that peak is
		// most of what a pickup sounds like.
		float wantHz = 1600.f * std::pow(6200.f / 1600.f, tone);
		if (coilSr != sr || std::fabs(wantHz - coilHz) > 1.f) {
			coil.set(wantHz, 1.4f, sr);
			// A horseshoe's character is not a brighter peak, it is a broad
			// midrange lift — bark and honk. A single resonant lowpass cannot
			// make that however far you move its corner, so it takes a second,
			// much wider band underneath.
			honk.set(1150.f, 0.75f, sr);
			coilHz = wantHz; coilSr = sr;
		}

		// ── pitch ─────────────────────────────────────────────────────────────
		float rootSemis = params[ROOT_PARAM].getValue();
		if (inputs[ROOT_CV_INPUT].isConnected())
			rootSemis += std::round(inputs[ROOT_CV_INPUT].getVoltage() * 12.f);
		float basePitch = params[OCT_PARAM].getValue() + rootSemis / 12.f;
		// In melody mode V/OCT has already been spent placing the bar; adding it
		// here as well would transpose the note a second time.
		if (!(playMode == 1 && inputs[VOCT_INPUT].isConnected()))
			basePitch += inputs[VOCT_INPUT].getVoltage();

		// ── triggers ──────────────────────────────────────────────────────────
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)
		    || resetBtn.process(params[RESET_PARAM].getValue() > 0.5f ? 10.f : 0.f, 0.1f, 1.f))
			{ rollIdx = 0; rollWalk = 0; intPhase = 0.f; }

		int gch = inputs[GATE_INPUT].getChannels();
		if (gch <= 1) {
			if (gateTrig.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f)) {
				float gv = inputs[VEL_INPUT].isConnected()
				         ? clamp(inputs[VEL_INPUT].getVoltage() / 10.f, 0.03f, 1.f) : 1.f;
				if (playMode == 1 && inputs[VOCT_INPUT].isConnected()) pick(melodyString, gv);
				else strum(gv);
			}
		}
		else {
			for (int i = 0; i < SLIDE_NCH && i < gch; i++)
				if (polyGate[i].process(inputs[GATE_INPUT].getVoltage(i), 0.1f, 1.f)) {
					float gv = inputs[VEL_INPUT].isConnected()
					         ? clamp(inputs[VEL_INPUT].getPolyVoltage(i) / 10.f, 0.03f, 1.f) : 1.f;
					// Poly gate channel N is the Nth NOTE, not the Nth string —
					// which string it lands on is the solver's business.
					bool melodic = (playMode == 1 && inputs[VOCT_INPUT].isConnected());
					pick(melodic ? voiceString[std::min(i, SLIDE_NCH - 1)] : i, gv);
				}
		}

		bool autoOn = params[AUTO_PARAM].getValue() > 0.5f;
		lights[AUTO_LIGHT].setBrightness(autoOn ? 1.f : 0.f);
		if (autoOn) {
			bool tick = false;
			if (inputs[CLOCK_INPUT].isConnected())
				tick = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
			else {
				intPhase += args.sampleTime * internalHz;
				if (intPhase >= 1.f) { intPhase -= 1.f; tick = true; }
			}
			if (tick) rollStep();
		}

		while (mouseRead != mouseWrite) {
			MouseHit h = mouseQ[mouseRead];
			mouseRead = (mouseRead + 1) % MOUSE_Q;
			pick(h.string, h.vel);
		}

		// ── the strings ───────────────────────────────────────────────────────
		float bus = 0.f;
		float mixL = 0.f, mixR = 0.f, sumEven = 0.f, sumOdd = 0.f;
		SlideExpanderMessage* xmsg = nullptr;
		if (rightExpander.module && rightExpander.module->model == modelSlideX)
			xmsg = (SlideExpanderMessage*) rightExpander.producerMessage;
		if (xmsg) xmsg->active = true;
		outputs[POLY_OUTPUT].setChannels(0);        // retired; Slide X carries these

		for (int i = 0; i < SLIDE_NCH; i++) {
			SlideString& s = str[i];

			if (s.pending >= 0.f) {
				s.pending -= args.sampleTime;
				if (s.pending <= 0.f) { s.pending = -1.f; pick(i, s.pendVel); }
			}

			float stop = barFor(i) + vib;
			shownBar[i] = stop;
			float semis = basePitch * 12.f + tune[i] + stop;
			float freq = clamp(dsp::FREQ_C4 * std::pow(2.f, semis / 12.f),
			                   20.f, std::min(8000.f, sr * 0.24f));

			// The bar is a lossy, mass-loaded stop rather than a fret clamping
			// the string against wood, so it hands back less treble — and more
			// so the harder it is pressed, which is what the top of DAMP means
			// on this instrument.
			float dHz = std::max(dampHz, freq * 10.f) * (1.f - 0.25f * clamp(stop / 12.f, 0.f, 1.f));
			s.dampC = clamp(1.f - std::exp(-2.f * (float)M_PI * dHz / sr), 0.02f, 0.999f);

			float b = 0.10f * 0.42f;               // a steel string has some stiffness
			float apC = -b;
			float w = 2.f * (float)M_PI * freq / sr;
			s.dTarget = clamp(sr / freq
			                  - SLIDE_AP * sfs::allpassDelay(apC, w)
			                  - sfs::onePoleDelay(s.dampC, w),
			                  8.f, (float)SLIDE_BUF / 1.62f);
			// The delay follows the bar every sample. That is not de-zippering:
			// a string whose speaking length is physically changing while it
			// rings is exactly what a slide IS, and a waveguide gives it for
			// free — the energy already circulating carries through the move.
			if (s.dSm <= 0.f) s.dSm = s.dTarget;
			s.dSm += (s.dTarget - s.dSm) * 0.25f;
			float d = s.dSm;

			// A blocked string still sounds when picked — it just stops ringing
			// almost at once, which is what a palm on the strings does.
			s.live *= liveDecay;
			float openness = 1.f - blockAmt * (1.f - s.live);
			float t60 = std::max(decaySec * (0.03f + 0.97f * openness), 0.02f);
			float g = std::min(std::exp(-6.907755f / (freq * t60)), 0.99995f);

			// Cubic, because this delay is MOVING — see waveguide.hpp.
			float v = s.dl.tapCubic(d);

			// The pickup sits at a FIXED distance from the bridge while the
			// speaking length changes, so its position as a FRACTION of the
			// string grows as the bar goes up — and the comb notch walks down
			// the harmonic series with it. This is the up-the-neck tone change.
			float puFrac = clamp(pickupPos * std::pow(2.f, stop / 12.f), 0.02f, 0.48f);
			float combD = std::min(d * (1.f + puFrac), (float)SLIDE_BUF - 4.f);
			s.out = v - s.dl.tapCubic(combD);

			float exc = 0.f;
			if (s.burst > 0.f) {
				float t = 1.f - s.burst / s.burstLen;
				float win = 0.5f - 0.5f * std::cos(2.f * (float)M_PI * t);
				s.excLp += excC * (win * (2.f * random::uniform() - 1.f) - s.excLp);
				exc = s.excLp * s.burstAmp;
				s.burst -= 1.f;
			}


			s.lp += s.dampC * (v - s.lp);
			float x = g * s.lp;
			for (int k = 0; k < SLIDE_AP; k++) {
				float y = apC * x + s.apX[k] - apC * s.apY[k];
				s.apX[k] = x; s.apY[k] = y; x = y;
			}
			float dy = x - s.dcX + 0.99985f * s.dcY;
			s.dcX = x; s.dcY = dy; x = dy;
			s.brLp += brC * (v - s.brLp);
			x += exc + coupAmt * coupAmt * 0.06f * (couplePrev - s.brLp);
			if (x > 1.6f) x = 1.6f; else if (x < -1.6f) x = -1.6f;
			s.dl.write(x);
			motion += s.brLp;

			// The pedal is already down when the note is picked and comes up
			// after it, so the attack simply never reaches the amp.
			s.swell += (1.f - s.swell) * swellRate;
			s.out *= s.swell;

			bus += s.out;
			float p = ((float)i / (float)(SLIDE_NCH - 1) * 2.f - 1.f) * stereoWidth;
			float th = (p + 1.f) * (float)M_PI_4;
			mixL += s.out * std::cos(th);
			mixR += s.out * std::sin(th);
			if (xmsg) xmsg->string[i] = sfs::softClip(s.out * 10.f);
			// String 1 is index 0, so the ODD strings are the even indices.
			(i % 2 ? sumEven : sumOdd) += s.out;

			float a = std::fabs(s.out);
			s.amp += (a > s.amp ? 0.02f : 0.0009f) * (a - s.amp);
			s.flash *= 0.9994f;
		}
		(void)bus;
		{
			float cin = motion / (float)SLIDE_NCH;
			float cdy = cin - coupleDcX + 0.9993f * coupleDcY;
			coupleDcX = cin; coupleDcY = cdy;
			coupleBus = clamp(cdy, -3.f, 3.f);
		}

		// ── the pickup and the amp ────────────────────────────────────────────
		float loL, bpL, loR, bpR;
		coil.process(mixL, loL, bpL);
		// One coil, so the right channel is the same filter's answer to a signal
		// that only differs by panning; running a second instance would drift.
		loR = loL + (mixR - mixL) * 0.5f;
		bpR = bpL;
		float hk = (pickupType == 1) ? honk.bandpass(mixL) * 0.55f : 0.f;
		float yL = loL + 1.3f * bpL + hk;
		float yR = loR + 1.3f * bpR + hk;

		// The pedal itself, when one is patched: this is what a player's foot is
		// actually doing, and it beats any envelope baked into the module.
		if (inputs[VOL_INPUT].isConnected()) {
			float vg = clamp(inputs[VOL_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			yL *= vg; yR *= vg;
		}

		float dr = 1.f + drive * 8.f;
		yL = std::tanh(yL * dr) / std::sqrt(dr);
		yR = std::tanh(yR * dr) / std::sqrt(dr);

		if (xmsg) rightExpander.requestMessageFlip();
		outputs[EVEN_OUTPUT].setVoltage(sfs::softClip(sumEven * 10.f));
		outputs[ODD_OUTPUT].setVoltage(sfs::softClip(sumOdd * 10.f));
		outputs[MIX_L_OUTPUT].setVoltage(sfs::softClip(yL * 11.f));
		outputs[MIX_R_OUTPUT].setVoltage(sfs::softClip(yR * 11.f));
	}

	// ── mouse picks: GUI thread → audio thread ────────────────────────────────
	struct MouseHit { int string; float vel; };
	static const int MOUSE_Q = 32;
	MouseHit mouseQ[MOUSE_Q] = {};
	volatile int mouseRead = 0, mouseWrite = 0;
	void mousePick(int i, float vel) {
		int nw = (mouseWrite + 1) % MOUSE_Q;
		if (nw == mouseRead) return;
		mouseQ[mouseWrite] = {i, vel};
		mouseWrite = nw;
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "playMode", json_integer(playMode));
		json_object_set_new(r, "autoMovesBar", json_boolean(autoMovesBar));
		json_object_set_new(r, "pickupType", json_integer(pickupType));
		json_object_set_new(r, "stereoWidth", json_real(stereoWidth));
		json_object_set_new(r, "mouseMode", json_integer(mouseMode));
		json_object_set_new(r, "internalHz", json_real(internalHz));
		json_t* t = json_array();
		for (int i = 0; i < SLIDE_NCH; i++) json_array_append_new(t, json_real(tune[i]));
		json_object_set_new(r, "tune", t);
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "playMode")) playMode = (int)json_integer_value(j);
		if (json_t* j = json_object_get(r, "autoMovesBar")) autoMovesBar = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "pickupType")) pickupType = (int)json_integer_value(j);
		if (json_t* j = json_object_get(r, "stereoWidth")) stereoWidth = json_number_value(j);
		if (json_t* j = json_object_get(r, "mouseMode")) mouseMode = (int)json_integer_value(j);
		if (json_t* j = json_object_get(r, "internalHz")) internalHz = json_number_value(j);
		if (json_t* t = json_object_get(r, "tune"))
			for (int i = 0; i < SLIDE_NCH && i < (int)json_array_size(t); i++)
				tune[i] = (float)json_number_value(json_array_get(t, i));
	}
};


// =============================================================================
// Display — the fretboard, lying flat, with the bar across it.
// =============================================================================

struct SlideDisplay : OpaqueWidget {
	Slide* module = nullptr;
	std::shared_ptr<Font> font;
	bool  draggingBar = false;
	Vec   dragPos;

	struct Lay {
		float w, h, x0, x1, y0, y1, rowH;
		float sy(int i) const { return y0 + rowH * ((float)i + 0.5f); }
		float fx(float semis) const { return x0 + slideFretX(semis) * (x1 - x0); }
	};

	Lay layout() const {
		Lay L;
		L.w = box.size.x; L.h = box.size.y;
		L.x0 = L.w * 0.035f; L.x1 = L.w * 0.985f;
		L.y0 = L.h * 0.10f;  L.y1 = L.h * 0.94f;
		L.rowH = (L.y1 - L.y0) / (float)SLIDE_NCH;
		return L;
	}

	int stringHit(const Lay& L, float y) const {
		int i = (int)std::floor((y - L.y0) / L.rowH);
		return (i >= 0 && i < SLIDE_NCH) ? i : -1;
	}

	// x back to semitones — the inverse of the fret spacing, so dragging the bar
	// lands where the eye says it should.
	float semisAt(const Lay& L, float x) const {
		float f = clamp((x - L.x0) / std::max(L.x1 - L.x0, 1.f), 0.f, 1.f);
		const float endF = 1.f - std::pow(2.f, -(float)SLIDE_FRETS / 12.f);
		float r = 1.f - f * endF;
		return clamp(-12.f * std::log2(std::max(r, 1e-4f)), 0.f, (float)SLIDE_FRETS);
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) {
			OpaqueWidget::onButton(e);
			return;
		}
		Lay L = layout();
		if (e.pos.y < L.y0 || e.pos.y > L.y1) return;   // a miss falls through
		e.consume(this);
		draggingBar = true;
		dragPos = e.pos;
		module->params[Slide::BAR_PARAM].setValue(semisAt(L, e.pos.x));
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || !draggingBar) return;
		float zoom = std::max(getAbsoluteZoom(), 0.01f);
		dragPos = dragPos.plus(e.mouseDelta.div(zoom));
		module->params[Slide::BAR_PARAM].setValue(semisAt(layout(), dragPos.x));
	}
	void onDragEnd(const DragEndEvent& e) override { draggingBar = false; }

	// Hover strums, exactly as Loom does: crossing a string picks it, and how
	// fast you cross sets how hard.
	void onHover(const HoverEvent& e) override {
		OpaqueWidget::onHover(e);
		if (!module || module->mouseMode != 0 || draggingBar) return;
		Lay L = layout();
		float speed = std::fabs(e.mouseDelta.y);
		if (speed < 0.35f) return;
		float y1 = e.pos.y, y0 = e.pos.y - e.mouseDelta.y;
		float lo = std::min(y0, y1), hi = std::max(y0, y1);
		float vel = clamp(0.22f + speed / 20.f, 0.12f, 1.f);
		for (int i = 0; i < SLIDE_NCH; i++) {
			float sy = L.sy(i);
			if (sy > lo && sy <= hi) module->mousePick(i, vel);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!font || font->handle < 0) font = sfs::panelFont();
		if (!font || font->handle < 0) return;
		nvgSave(args.vg);
		nvgScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		if (!module) drawPreview(args);
		else         drawLive(args);
		drawRollName(args);
		nvgRestore(args.vg);
	}

	// Which roll is selected, named. Sixteen of them are reachable by knob and
	// by CV and none of them were written down anywhere you could see while
	// playing -- so the six register patterns might as well not have existed.
	void drawRollName(const DrawArgs& args) {
		int pat = 0;
		if (module) {
			pat = (int)std::round(module->params[Slide::PATTERN_PARAM].getValue()
			      + module->inputs[Slide::PATTERN_CV_INPUT].getVoltage() * 0.1f * SLIDE_NROLLS);
		}
		pat = clamp(pat, 0, SLIDE_NROLLS - 1);
		bool on = module && module->params[Slide::AUTO_PARAM].getValue() > 0.5f;
		NVGcontext* vg = args.vg;
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgFillColor(vg, on ? nvgRGB(0xec, 0x65, 0x2e) : nvgRGB(0x6a, 0x6a, 0x86));
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
		nvgText(vg, box.size.x * 0.985f, box.size.y * 0.012f,
		        string::f("%d  %s", pat + 1, SLIDE_ROLLS[pat].name).c_str(), NULL);
	}

	void drawNeck(const DrawArgs& args, const Lay& L) {
		NVGcontext* vg = args.vg;
		static const int INLAY[] = {3, 5, 7, 9, 15, 17, 19, 21};
		for (size_t k = 0; k < sizeof(INLAY) / sizeof(INLAY[0]); k++) {
			float x = L.fx((float)INLAY[k] - 0.5f);
			nvgBeginPath(vg);
			nvgCircle(vg, x, (L.y0 + L.y1) * 0.5f, L.h * 0.022f);
			nvgFillColor(vg, nvgRGB(0x3A, 0x3A, 0x58));
			nvgFill(vg);
		}
		float x12 = L.fx(11.5f);                     // the double dot at the twelfth
		for (int s = -1; s <= 1; s += 2) {
			nvgBeginPath(vg);
			nvgCircle(vg, x12, (L.y0 + L.y1) * 0.5f + (float)s * L.rowH * 1.6f, L.h * 0.022f);
			nvgFillColor(vg, nvgRGB(0x3A, 0x3A, 0x58));
			nvgFill(vg);
		}
		for (int n = 0; n <= SLIDE_FRETS; n++) {
			float x = L.fx((float)n);
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, L.y0);
			nvgLineTo(vg, x, L.y1);
			nvgStrokeColor(vg, n == 0 ? sfs::SCREEN_DIM : nvgRGBA(0x8A, 0x8A, 0xA5, 55));
			nvgStrokeWidth(vg, n == 0 ? 1.8f : 0.8f);
			nvgStroke(vg);
		}
	}

	void drawStrings(const DrawArgs& args, const Lay& L, const float* amp,
	                 const float* flash, const float* stop) {
		NVGcontext* vg = args.vg;
		for (int i = 0; i < SLIDE_NCH; i++) {
			float y = L.sy(i);
			float th = 0.8f + 1.8f * (1.f - (float)i / (float)(SLIDE_NCH - 1));
			float xb = clamp(L.fx(stop[i]), L.x0, L.x1);

			// Behind the bar the string is dead — that is what a bar DOES, and
			// drawing the whole length vibrating hides the one thing the display
			// exists to show.
			nvgBeginPath(vg);
			nvgMoveTo(vg, L.x0, y);
			nvgLineTo(vg, xb, y);
			nvgStrokeColor(vg, sfs::SCREEN_PURP);
			nvgStrokeWidth(vg, th);
			nvgStroke(vg);

			NVGcolor col = sfs::SCREEN_BLUE;
			if (flash[i] > 0.01f)
				col = nvgLerpRGBA(col, sfs::SCREEN_HOT, clamp(flash[i], 0.f, 1.f));
			float A = std::min(amp[i], 1.f) * L.rowH * 0.42f;
			nvgBeginPath(vg);
			if (A < 0.25f || L.x1 - xb < 2.f) { nvgMoveTo(vg, xb, y); nvgLineTo(vg, L.x1, y); }
			else {
				const int SEG = 26;
				for (int k = 0; k <= SEG; k++) {
					float t = (float)k / (float)SEG;
					float x = xb + t * (L.x1 - xb);
					float dy = A * std::sin((float)M_PI * t)
					         * std::sin(t * (float)M_PI * 3.f + (float)i);
					if (k == 0) nvgMoveTo(vg, x, y + dy); else nvgLineTo(vg, x, y + dy);
				}
			}
			nvgStrokeColor(vg, col);
			nvgStrokeWidth(vg, th);
			nvgStroke(vg);
		}
	}

	// The bar itself: one line across the strings, and its ANGLE is the slant.
	void drawBar(const DrawArgs& args, const Lay& L, const float* stop) {
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		for (int i = 0; i < SLIDE_NCH; i++) {
			float x = L.fx(stop[i]);
			if (i == 0) nvgMoveTo(vg, x, L.sy(i) - L.rowH * 0.5f);
			nvgLineTo(vg, x, L.sy(i) + L.rowH * 0.5f);
		}
		nvgStrokeColor(vg, nvgRGBA(0xE8, 0xE8, 0xF0, 235));
		nvgStrokeWidth(vg, 3.4f);
		nvgStroke(vg);

		nvgBeginPath(vg);
		for (int i = 0; i < SLIDE_NCH; i++) {
			float x = L.fx(stop[i]);
			if (i == 0) nvgMoveTo(vg, x, L.sy(i) - L.rowH * 0.5f);
			nvgLineTo(vg, x, L.sy(i) + L.rowH * 0.5f);
		}
		nvgStrokeColor(vg, nvgRGBA(0xFF, 0xFF, 0xFF, 120));
		nvgStrokeWidth(vg, 1.2f);
		nvgStroke(vg);
	}

	void drawLive(const DrawArgs& args) {
		Lay L = layout();
		drawNeck(args, L);
		float amp[SLIDE_NCH], flash[SLIDE_NCH], stop[SLIDE_NCH];
		for (int i = 0; i < SLIDE_NCH; i++) {
			amp[i] = module->str[i].amp * 3.2f;
			flash[i] = module->str[i].flash;
			stop[i] = module->shownBar[i];
		}
		drawStrings(args, L, amp, flash, stop);
		drawBar(args, L, stop);
	}

	void drawPreview(const DrawArgs& args) {
		Lay L = layout();
		drawNeck(args, L);
		static const float amp[SLIDE_NCH]   = {0.9f, 0.7f, 0.5f, 0.3f, 0.15f, 0.f, 0.f, 0.f};
		static const float flash[SLIDE_NCH] = {0.f, 0.f, 0.2f, 0.6f, 1.f, 0.f, 0.f, 0.f};
		float stop[SLIDE_NCH];
		for (int i = 0; i < SLIDE_NCH; i++)          // a forward slant across the neck
			stop[i] = 7.f + 1.5f * ((float)i / (float)(SLIDE_NCH - 1) * 2.f - 1.f);
		drawStrings(args, L, amp, flash, stop);
		drawBar(args, L, stop);
	}
};


// =============================================================================
// Panel — 26HP.
// =============================================================================

struct SlideWidget : ModuleWidget {
	SlideWidget(Slide* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/slide.svg")));
		using sfs::hp;

		// NO sfs::PanelLabels HERE, DELIBERATELY. res/slide.svg is the designer's
		// own file, published by `figma_panel_template.py --publish slide`, and it
		// carries every label, connector line and the logo at their weights.
		// Drawing labels over it at runtime would replace all of that with
		// panel-style.hpp's defaults. This places components and nothing else.
		SlideDisplay* disp = new SlideDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(hp(0.75f), hp(2)));
		disp->box.size = mm2px(Vec(hp(26.5f), hp(10.5f)));
		addChild(disp);

		// ── the bar ────────────────────────────────────────────────────────────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(2), hp(15))), module, Slide::BAR_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(2), hp(17))), module, Slide::BAR_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(4), hp(17))), module, Slide::GLIDE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(6), hp(15))), module, Slide::SLANT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(6), hp(17))), module, Slide::SLANT_CV_INPUT));

		// ── vibrato and the hand ───────────────────────────────────────────────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(9), hp(15))), module, Slide::VIB_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(13), hp(15))), module, Slide::VIBRATE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(9), hp(18))), module, Slide::DYN_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(11), hp(18))), module, Slide::COUPLE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(13), hp(18))), module, Slide::BLOCK_PARAM));

		// ── string, pickup and amp ─────────────────────────────────────────────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(16), hp(14))), module, Slide::PICK_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(18), hp(14))), module, Slide::PICK_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(20), hp(14))), module, Slide::DAMP_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(22), hp(14))), module, Slide::DAMP_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(24), hp(14))), module, Slide::SWELL_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(26), hp(14))), module, Slide::VOL_INPUT));

		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(16), hp(17))), module, Slide::PICKUP_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(18), hp(17))), module, Slide::DRIVE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(20), hp(17))), module, Slide::TONE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(22), hp(17))), module, Slide::TONE_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(24), hp(17))), module, Slide::DECAY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(26), hp(17))), module, Slide::DECAY_CV_INPUT));

		// ── playing in ─────────────────────────────────────────────────────────
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(2), hp(21))), module, Slide::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(4.5f), hp(21))), module, Slide::VEL_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(2), hp(24))), module, Slide::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(4.5f), hp(24))), module, Slide::VOCT_INPUT));

		// ── the key ────────────────────────────────────────────────────────────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(8), hp(22))), module, Slide::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(8), hp(24))), module, Slide::ROOT_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(9.5f), hp(23))), module, Slide::OCT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(11), hp(22))), module, Slide::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(11), hp(24))), module, Slide::SCALE_CV_INPUT));

		// ── the auto player ────────────────────────────────────────────────────
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(hp(14), hp(22))), module, Slide::AUTO_PARAM, Slide::AUTO_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(hp(14), hp(24))), module, Slide::RESET_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(16), hp(22))), module, Slide::PATTERN_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(16), hp(24))), module, Slide::PATTERN_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(19), hp(22))), module, Slide::DENSITY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(19), hp(24))), module, Slide::DENSITY_CV_INPUT));

		// ── out ────────────────────────────────────────────────────────────────
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(23.375f), hp(21))), module, Slide::EVEN_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(26), hp(21))), module, Slide::ODD_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(23.375f), hp(24))), module, Slide::MIX_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(26), hp(24))), module, Slide::MIX_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Slide* m = dynamic_cast<Slide*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Auto roll moves the bar", "", &m->autoMovesBar));
		menu->addChild(createSubmenuItem("Tuning", "", [=](Menu* sub) {
			for (int t = 0; t < SLIDE_NTUNINGS; t++)
				sub->addChild(createMenuItem(SLIDE_TUNINGS[t].name, "",
					[=]() { m->applyTuning(t); }));
		}));
		menu->addChild(createIndexPtrSubmenuItem("V/oct",
			{"Transposes the whole instrument", "Places the bar and picks the string"},
			&m->playMode));
		menu->addChild(createIndexPtrSubmenuItem("Pickup",
			{"Modern single coil", "Horseshoe (bark and midrange honk)"}, &m->pickupType));
		menu->addChild(createIndexPtrSubmenuItem("Mouse",
			{"Hover strums the strings", "Click and drag only"}, &m->mouseMode));
	}
};

Model* modelSlide = createModel<Slide, SlideWidget>("Slide");
