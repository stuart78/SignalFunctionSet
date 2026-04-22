#include "plugin.hpp"
#include "fugue-messages.hpp"

// ─── Scale Tables ────────────────────────────────────────────────────────────
// Order mirrors Note's scale list so SCALE CV values are interchangeable
// between modules. Two extras (Melodic Minor, Locrian) are appended to
// preserve them from older Fugue versions.

struct ScaleInfo {
	const float* intervals;
	int size;
};

// 12-TET scales (integer semitones, expressed as floats)
static const float SCALE_CHROMATIC[]   = {0,1,2,3,4,5,6,7,8,9,10,11};
static const float SCALE_MAJOR[]       = {0,2,4,5,7,9,11};
static const float SCALE_NAT_MINOR[]   = {0,2,3,5,7,8,10};
static const float SCALE_PENTA_MAJ[]   = {0,2,4,7,9};
static const float SCALE_PENTA_MIN[]   = {0,3,5,7,10};
static const float SCALE_BLUES[]       = {0,3,5,6,7,10};
static const float SCALE_WHOLE[]       = {0,2,4,6,8,10};
// Just-intonation harmonics 1..12 (non-12-TET, log2(n)*12)
static const float SCALE_HARMONIC[]    = {
	0.f, 12.f, 19.0196f, 24.f, 27.8631f, 31.0196f,
	33.6883f, 36.f, 38.0392f, 39.8632f, 41.5126f, 43.0196f
};
static const float SCALE_DORIAN[]      = {0,2,3,5,7,9,10};
static const float SCALE_PHRYGIAN[]    = {0,1,3,5,7,8,10};
static const float SCALE_LYDIAN[]      = {0,2,4,6,7,9,11};
static const float SCALE_MIXOLYDIAN[]  = {0,2,4,5,7,9,10};
static const float SCALE_HARM_MINOR[]  = {0,2,3,5,7,8,11};
static const float SCALE_HIJAZ[]       = {0,1,4,5,7,8,10};
static const float SCALE_HIRAJOSHI[]   = {0,2,3,7,8};
// Pelog: Surakarta-style approximation in cents/12 (non-12-TET)
static const float SCALE_PELOG[]       = {0.f, 1.2f, 2.7f, 5.4f, 7.0f, 8.0f, 10.4f};
// Slendro: 5 equal divisions of the octave (non-12-TET)
static const float SCALE_SLENDRO[]     = {0.f, 2.4f, 4.8f, 7.2f, 9.6f};
// Preserved-from-old-Fugue extras
static const float SCALE_MELO_MINOR[]  = {0,2,3,5,7,9,11};
static const float SCALE_LOCRIAN[]     = {0,1,3,5,6,8,10};

static const ScaleInfo SCALES[] = {
	{SCALE_CHROMATIC, 12},   //  0  Chromatic
	{SCALE_MAJOR,      7},   //  1  Major
	{SCALE_NAT_MINOR,  7},   //  2  Minor
	{SCALE_PENTA_MAJ,  5},   //  3  Pentatonic Major
	{SCALE_PENTA_MIN,  5},   //  4  Pentatonic Minor
	{SCALE_BLUES,      6},   //  5  Blues
	{SCALE_WHOLE,      6},   //  6  Whole tone
	{SCALE_HARMONIC,  12},   //  7  Harmonic series
	{SCALE_DORIAN,     7},   //  8  Dorian
	{SCALE_PHRYGIAN,   7},   //  9  Phrygian
	{SCALE_LYDIAN,     7},   // 10  Lydian
	{SCALE_MIXOLYDIAN, 7},   // 11  Mixolydian
	{SCALE_HARM_MINOR, 7},   // 12  Harmonic Minor
	{SCALE_HIJAZ,      7},   // 13  Hijaz
	{SCALE_HIRAJOSHI,  5},   // 14  Hirajoshi
	{SCALE_PELOG,      7},   // 15  Pelog
	{SCALE_SLENDRO,    5},   // 16  Slendro
	{SCALE_MELO_MINOR, 7},   // 17  Melodic Minor (Fugue extra)
	{SCALE_LOCRIAN,    7},   // 18  Locrian (Fugue extra)
};
static const int NUM_SCALES_FUGUE = sizeof(SCALES) / sizeof(SCALES[0]);

static const int NUM_STEPS = 8;
static const int NUM_VOICES = 3;
static const int CHROMATIC_SCALE_INDEX = 0;

// ─── Harmonic Deviation Tier Tables ──────────────────────────────────────────

// Diatonic tiers: offsets in scale degrees from the base note
struct DeviationTier {
	int offsets[6];
	int count;
};

static const DeviationTier DIATONIC_TIERS[] = {
	{{0},          1},  // Tier 0: Unison
	{{2, 4},       2},  // Tier 1: 3rd, 5th (chord tones)
	{{6, 1, 3},    3},  // Tier 2: 7th, 9th, 11th (extensions)
	{{5},          1},  // Tier 3: 6th (remaining)
};
static const int NUM_DIATONIC_TIERS = 4;

static const DeviationTier PENTATONIC_TIERS[] = {
	{{0},          1},  // Tier 0: Unison
	{{1, 2},       2},  // Tier 1: 2nd, 3rd degree
	{{3, 4},       2},  // Tier 2: 4th, 5th degree
};
static const int NUM_PENTATONIC_TIERS = 3;

// Chromatic tiers: intervals in semitones
static const int CHROM_TIER_0[] = {0};              // Unison
static const int CHROM_TIER_1[] = {7, 5};           // Perfect consonance (P5, P4)
static const int CHROM_TIER_2[] = {4, 3, 9, 8};     // Imperfect consonance (M3, m3, M6, m6)
static const int CHROM_TIER_3[] = {2, 10};           // Mild dissonance (M2, m7)
static const int CHROM_TIER_4[] = {1, 11, 6};        // Sharp dissonance (m2, M7, tritone)

struct ChromTierInfo {
	const int* intervals;
	int count;
};

static const ChromTierInfo CHROM_TIERS[] = {
	{CHROM_TIER_0, 1},
	{CHROM_TIER_1, 2},
	{CHROM_TIER_2, 4},
	{CHROM_TIER_3, 2},
	{CHROM_TIER_4, 3},
};

// ─── RNG Helper ──────────────────────────────────────────────────────────────

static inline uint32_t xorshift32(uint32_t& state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

static inline float randFloat(uint32_t& state) {
	return (float)(xorshift32(state) & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

// ─── Interval Consonance Scoring ─────────────────────────────────────────────

static float intervalConsonance(int semitones) {
	semitones = ((semitones % 12) + 12) % 12;
	static const float scores[] = {
		1.0f,   // 0: Unison
		0.1f,   // 1: m2
		0.3f,   // 2: M2
		0.65f,  // 3: m3
		0.7f,   // 4: M3
		0.85f,  // 5: P4
		0.15f,  // 6: tritone
		0.9f,   // 7: P5
		0.55f,  // 8: m6
		0.6f,   // 9: M6
		0.25f,  // 10: m7
		0.1f,   // 11: M7
	};
	return scores[semitones];
}

// ─── Custom ParamQuantity for fader note display ────────────────────────────

struct Fugue;

struct FaderParamQuantity : ParamQuantity {
	std::string getDisplayValueString() override;
};

// ─── Module ──────────────────────────────────────────────────────────────────

struct Fugue : Module {

	enum ParamId {
		FADER_PARAM_0,
		FADER_PARAM_1,
		FADER_PARAM_2,
		FADER_PARAM_3,
		FADER_PARAM_4,
		FADER_PARAM_5,
		FADER_PARAM_6,
		FADER_PARAM_7,
		ROOT_PARAM,
		SCALE_PARAM,
		STEPS_PARAM,
		SLEW_PARAM,
		WANDER_A_PARAM,
		WANDER_B_PARAM,
		WANDER_C_PARAM,
		RESET_BUTTON_PARAM,
		GATE_TOGGLE_PARAM_0,   // 24 toggles: step0_A, step0_B, step0_C, step1_A, ...
		PARAMS_LEN = GATE_TOGGLE_PARAM_0 + NUM_STEPS * NUM_VOICES
	};

	enum InputId {
		CLOCK_A_INPUT,
		CLOCK_B_INPUT,
		CLOCK_C_INPUT,
		RESET_INPUT,
		ROOT_CV_INPUT,
		SCALE_CV_INPUT,
		STEPS_CV_INPUT,
		SLEW_CV_INPUT,
		WANDER_A_INPUT,
		WANDER_B_INPUT,
		WANDER_C_INPUT,
		INPUTS_LEN
	};

	enum OutputId {
		CV_A_OUTPUT,
		CV_B_OUTPUT,
		CV_C_OUTPUT,
		GATE_A_OUTPUT,
		GATE_B_OUTPUT,
		GATE_C_OUTPUT,
		OUTPUTS_LEN
	};

	enum LightId {
		GATE_LIGHT_0,         // 24 gate toggle LEDs
		STEP_A_LIGHT_0 = GATE_LIGHT_0 + NUM_STEPS * NUM_VOICES,
		STEP_B_LIGHT_0 = STEP_A_LIGHT_0 + NUM_STEPS,
		STEP_C_LIGHT_0 = STEP_B_LIGHT_0 + NUM_STEPS,
		LIGHTS_LEN = STEP_C_LIGHT_0 + NUM_STEPS
	};

	// ─── Per-voice state ─────────────────────────────────────────────────────

	struct VoiceState {
		int currentStep = 0;
		float clockPeriod = 0.5f;
		float clockTimer = 0.f;
		bool clockHigh = false;
		uint32_t stepCounter = 0;
		dsp::SchmittTrigger clockTrigger;

		float currentVoltage = 0.f;
		float targetVoltage = 0.f;
		float slewRate = 0.f;
	};

	VoiceState voices[NUM_VOICES];
	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger resetButtonTrigger;
	float faderRangeVolts = 1.f;
	bool harmonicLock = true;

	// ─── Expander state ─────────────────────────────────────────────────────
	int sleepCounter[NUM_VOICES] = {};       // clocks remaining in sleep
	bool sleeping[NUM_VOICES] = {};          // voice is in sleep state
	bool sampleHoldEnabled = false;
	bool sampleHoldHolding[NUM_VOICES] = {}; // currently holding CV
	bool probGateSuppress[NUM_VOICES] = {};  // gate suppressed by probability
	uint32_t probRng = 12345;

	// ─── Constructor ─────────────────────────────────────────────────────────

	~Fugue() {
		delete (FugueToExpanderMessage*)rightExpander.producerMessage;
		delete (FugueToExpanderMessage*)rightExpander.consumerMessage;
	}

	Fugue() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// Expander message buffers (Fugue → FugueX)
		rightExpander.producerMessage = new FugueToExpanderMessage();
		rightExpander.consumerMessage = new FugueToExpanderMessage();

		// 8 pitch faders (custom ParamQuantity shows quantized note name)
		for (int i = 0; i < NUM_STEPS; i++) {
			configParam<FaderParamQuantity>(FADER_PARAM_0 + i, 0.f, 1.f, 0.f,
				string::f("Step %d Pitch", i + 1));
		}

		// Root note (snapped)
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root Note",
			{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});

		// Scale (snapped) — order matches Note module's SCALES array so
		// SCALE CV values are interchangeable between modules.
		configSwitch(SCALE_PARAM, 0.f, (float)(NUM_SCALES_FUGUE - 1), 1.f, "Scale", {
			"Chromatic", "Major", "Minor",
			"Pentatonic Major", "Pentatonic Minor",
			"Blues", "Whole tone", "Harmonic series",
			"Dorian", "Phrygian", "Lydian", "Mixolydian",
			"Harmonic Minor", "Hijaz (Arabic)",
			"Hirajoshi (Japanese)",
			"Pelog (Gamelan, 7-tone)",
			"Slendro (Gamelan, 5-equal)",
			"Melodic Minor", "Locrian"
		});

		// Steps (snapped)
		configSwitch(STEPS_PARAM, 1.f, 8.f, 8.f, "Steps",
			{"1", "2", "3", "4", "5", "6", "7", "8"});

		// Slew
		configParam(SLEW_PARAM, 0.f, 1.f, 0.f, "Slew", "%", 0.f, 100.f);

		// Wander sliders (0=faithful, 1=wanders more; default 0 = no wandering)
		configParam(WANDER_A_PARAM, 0.f, 1.f, 0.f, "Wander A", "%", 0.f, 100.f);
		configParam(WANDER_B_PARAM, 0.f, 1.f, 0.f, "Wander B", "%", 0.f, 100.f);
		configParam(WANDER_C_PARAM, 0.f, 1.f, 0.f, "Wander C", "%", 0.f, 100.f);

		// Reset button (momentary)
		configParam(RESET_BUTTON_PARAM, 0.f, 1.f, 0.f, "Reset");

		// 24 gate toggle buttons (default: A all on, B and C all off)
		const char* voiceNames[] = {"A", "B", "C"};
		for (int step = 0; step < NUM_STEPS; step++) {
			for (int v = 0; v < NUM_VOICES; v++) {
				int idx = step * NUM_VOICES + v;
				float defaultVal = 1.f;
				configSwitch(GATE_TOGGLE_PARAM_0 + idx, 0.f, 1.f, defaultVal,
					string::f("Gate %s Step %d", voiceNames[v], step + 1),
					{"Off", "On"});
			}
		}

		// Inputs
		configInput(CLOCK_A_INPUT, "Clock A");
		configInput(CLOCK_B_INPUT, "Clock B (normalled to A)");
		configInput(CLOCK_C_INPUT, "Clock C (normalled to B)");
		configInput(RESET_INPUT, "Reset");
		configInput(ROOT_CV_INPUT, "Root Note CV");
		configInput(SCALE_CV_INPUT, "Scale CV");
		configInput(STEPS_CV_INPUT, "Steps CV");
		configInput(SLEW_CV_INPUT, "Slew CV");
		configInput(WANDER_A_INPUT, "Wander A CV");
		configInput(WANDER_B_INPUT, "Wander B CV");
		configInput(WANDER_C_INPUT, "Wander C CV");

		// Outputs
		configOutput(CV_A_OUTPUT, "CV A");
		configOutput(CV_B_OUTPUT, "CV B");
		configOutput(CV_C_OUTPUT, "CV C");
		configOutput(GATE_A_OUTPUT, "Gate A");
		configOutput(GATE_B_OUTPUT, "Gate B");
		configOutput(GATE_C_OUTPUT, "Gate C");
	}

	// ─── JSON Persistence ────────────────────────────────────────────────────

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "faderRange", json_real(faderRangeVolts));
		json_object_set_new(rootJ, "harmonicLock", json_boolean(harmonicLock));
		// Bump on any change to SCALE ordering so dataFromJson can migrate
		// older saved scaleParam values.
		json_object_set_new(rootJ, "schemaVersion", json_integer(2));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* j = json_object_get(rootJ, "faderRange");
		if (j) faderRangeVolts = json_number_value(j);
		json_t* hlJ = json_object_get(rootJ, "harmonicLock");
		if (hlJ) harmonicLock = json_boolean_value(hlJ);

		// Schema v1 → v2: SCALE ordering changed to match Note. Remap the
		// saved scale param so the patch sounds the same as before.
		json_t* schemaJ = json_object_get(rootJ, "schemaVersion");
		int schema = schemaJ ? (int)json_integer_value(schemaJ) : 1;
		if (schema < 2) {
			// old → new index map
			static const int REMAP[12] = {
				1,  // 0 Major          → 1
				2,  // 1 Natural Minor  → 2
				12, // 2 Harmonic Minor → 12
				17, // 3 Melodic Minor  → 17
				8,  // 4 Dorian         → 8
				9,  // 5 Phrygian       → 9
				10, // 6 Lydian         → 10
				11, // 7 Mixolydian     → 11
				18, // 8 Locrian        → 18
				3,  // 9 Penta Major    → 3
				4,  // 10 Penta Minor   → 4
				0,  // 11 Chromatic     → 0
			};
			int oldVal = (int)std::round(params[SCALE_PARAM].getValue());
			if (oldVal >= 0 && oldVal < 12) {
				params[SCALE_PARAM].setValue((float)REMAP[oldVal]);
			}
		}
	}

	// ─── Scale Quantization ──────────────────────────────────────────────────

	float faderToVoltage(float faderValue, int rootNote, int scaleIndex, float faderRange) {
		float rawVoltage = faderValue * faderRange;
		const ScaleInfo& scale = SCALES[scaleIndex];

		float bestVoltage = 0.f;
		float bestDist = 999.f;

		int maxSemitones = (int)(faderRange * 12.f) + 12;
		int maxOctaves = maxSemitones / 12 + 2;

		for (int oct = 0; oct <= maxOctaves; oct++) {
			for (int d = 0; d < scale.size; d++) {
				float semitone = (float)(oct * 12) + scale.intervals[d];
				float noteVoltage = semitone / 12.f;

				if (noteVoltage > faderRange + 0.05f) break;
				if (noteVoltage < -0.05f) continue;

				float dist = std::abs(noteVoltage - rawVoltage);
				if (dist < bestDist) {
					bestDist = dist;
					bestVoltage = noteVoltage;
				}
			}
		}

		return bestVoltage + (float)rootNote / 12.f;
	}

	// ─── Harmonic Deviation ──────────────────────────────────────────────────

	float selectDeviationNote(float baseVoltage, float stability, int rootNote,
	                          int scaleIndex, float faderRange, uint32_t seed) {
		uint32_t rng = seed;

		// At full stability, always return the base note
		if (stability >= 0.999f) return baseVoltage;

		float tierRoll = randFloat(rng);

		if (scaleIndex == CHROMATIC_SCALE_INDEX) {
			// ── Chromatic mode: interval-based hierarchy ──
			float p0 = stability + (1.f - stability) * 0.05f;
			float p1 = p0 + (1.f - stability) * 0.15f;
			float p2 = p1 + (1.f - stability) * 0.35f;
			float p3 = p2 + (1.f - stability) * 0.30f;

			int selectedTier;
			if (tierRoll < p0) selectedTier = 0;
			else if (tierRoll < p1) selectedTier = 1;
			else if (tierRoll < p2) selectedTier = 2;
			else if (tierRoll < p3) selectedTier = 3;
			else selectedTier = 4;

			const ChromTierInfo& tier = CHROM_TIERS[selectedTier];
			int idx = (int)(randFloat(rng) * tier.count) % tier.count;
			int semiOffset = tier.intervals[idx];

			// Random direction
			if (randFloat(rng) < 0.5f) semiOffset = -semiOffset;

			float dev = baseVoltage + (float)semiOffset / 12.f;
			return clamp(dev, baseVoltage - faderRange, baseVoltage + faderRange);
		}
		else {
			// ── Diatonic / Pentatonic mode: scale-degree-based ──
			const ScaleInfo& scale = SCALES[scaleIndex];
			bool isPenta = (scale.size == 5);
			const DeviationTier* tiers = isPenta ? PENTATONIC_TIERS : DIATONIC_TIERS;
			int numTiers = isPenta ? NUM_PENTATONIC_TIERS : NUM_DIATONIC_TIERS;
			// Build cumulative probability thresholds
			float p[5];
			if (isPenta) {
				p[0] = stability + (1.f - stability) * 0.10f;
				p[1] = p[0] + (1.f - stability) * 0.45f;
				p[2] = 1.0f;
			} else {
				p[0] = stability + (1.f - stability) * 0.05f;
				p[1] = p[0] + (1.f - stability) * 0.30f;
				p[2] = p[1] + (1.f - stability) * 0.30f;
				p[3] = p[2] + (1.f - stability) * 0.20f;
				p[4] = 1.0f;
			}

			int maxTier = isPenta ? 2 : 4;
			int selectedTier = maxTier;
			for (int t = 0; t <= maxTier; t++) {
				if (tierRoll < p[t]) {
					selectedTier = t;
					break;
				}
			}

			if (selectedTier < numTiers) {
				// Scale-degree-based deviation
				const DeviationTier& tier = tiers[selectedTier];
				int idx = (int)(randFloat(rng) * tier.count) % tier.count;
				int scaleDegreeOffset = tier.offsets[idx];

				// Find the base note's scale degree
				float baseSemiFromRoot = (baseVoltage - (float)rootNote / 12.f) * 12.f;
				int baseSemiNorm = ((int)std::round(baseSemiFromRoot)) % 12;
				if (baseSemiNorm < 0) baseSemiNorm += 12;
				int baseOctave = (int)std::floor(baseSemiFromRoot / 12.f);

				// Find scale degree closest to baseSemiNorm. Floats are used
				// so non-12-TET scales (Pelog, Slendro, Harmonic) work too.
				int baseDegree = 0;
				float bestDiff = 999.f;
				for (int d = 0; d < scale.size; d++) {
					float diff = std::fabs(scale.intervals[d] - (float)baseSemiNorm);
					if (diff < bestDiff) {
						bestDiff = diff;
						baseDegree = d;
					}
				}

				// Calculate target degree (up or down)
				bool goDown = (randFloat(rng) < 0.4f);
				int targetDegree, targetOctave;

				if (goDown) {
					int raw = baseDegree - scaleDegreeOffset;
					if (raw < 0) {
						targetOctave = baseOctave - 1;
						targetDegree = ((raw % scale.size) + scale.size) % scale.size;
					} else {
						targetOctave = baseOctave;
						targetDegree = raw;
					}
				} else {
					int raw = baseDegree + scaleDegreeOffset;
					targetOctave = baseOctave + raw / scale.size;
					targetDegree = raw % scale.size;
				}

				float targetSemi = (float)(targetOctave * 12) + scale.intervals[targetDegree];
				float dev = (float)rootNote / 12.f + targetSemi / 12.f;
				return clamp(dev, baseVoltage - faderRange, baseVoltage + faderRange);
			}
			else {
				// Chromatic neighbor tier (diatonic only)
				int semiOffset = (randFloat(rng) < 0.5f) ? 1 : -1;
				float dev = baseVoltage + (float)semiOffset / 12.f;
				return clamp(dev, baseVoltage - faderRange, baseVoltage + faderRange);
			}
		}
	}

	// ─── Clock Normalling ────────────────────────────────────────────────────

	float getClockVoltage(int voiceIdx) {
		switch (voiceIdx) {
			case 0: return inputs[CLOCK_A_INPUT].getVoltage();
			case 1:
				if (inputs[CLOCK_B_INPUT].isConnected())
					return inputs[CLOCK_B_INPUT].getVoltage();
				return inputs[CLOCK_A_INPUT].getVoltage();
			case 2:
				if (inputs[CLOCK_C_INPUT].isConnected())
					return inputs[CLOCK_C_INPUT].getVoltage();
				if (inputs[CLOCK_B_INPUT].isConnected())
					return inputs[CLOCK_B_INPUT].getVoltage();
				return inputs[CLOCK_A_INPUT].getVoltage();
			default: return 0.f;
		}
	}

	// ─── Adaptive Slew ──────────────────────────────────────────────────────

	void calculateSlewRate(int voiceIdx, int numSteps, float slewPercent) {
		VoiceState& voice = voices[voiceIdx];

		if (slewPercent < 0.001f) {
			voice.slewRate = 0.f;
			return;
		}

		// Find steps to next active gate for this voice
		int stepsToNext = 0;
		for (int i = 1; i <= numSteps; i++) {
			int checkStep = (voice.currentStep + i) % numSteps;
			int toggleIdx = checkStep * NUM_VOICES + voiceIdx;
			if (params[GATE_TOGGLE_PARAM_0 + toggleIdx].getValue() > 0.5f) {
				stepsToNext = i;
				break;
			}
		}
		if (stepsToNext == 0) stepsToNext = numSteps;

		float timeAvailable = stepsToNext * voice.clockPeriod;
		float slewTime = std::max(slewPercent * timeAvailable, 0.001f);
		float voltageDiff = std::abs(voice.targetVoltage - voice.currentVoltage);

		if (voltageDiff < 0.0001f) {
			voice.slewRate = 0.f;
		} else {
			voice.slewRate = voltageDiff / slewTime;
		}
	}

	void processSlew(int voiceIdx, float sampleTime) {
		VoiceState& voice = voices[voiceIdx];

		if (voice.slewRate <= 0.f) {
			voice.currentVoltage = voice.targetVoltage;
		} else {
			float diff = voice.targetVoltage - voice.currentVoltage;
			float maxStep = voice.slewRate * sampleTime;
			if (std::abs(diff) <= maxStep) {
				voice.currentVoltage = voice.targetVoltage;
			} else {
				voice.currentVoltage += (diff > 0.f ? maxStep : -maxStep);
			}
		}
	}

	// ─── Consonance Scoring ─────────────────────────────────────────────────

	float scoreConsonance(float candidateVolt, int voiceIdx) {
		float score = 0.f;
		int count = 0;
		for (int v = 0; v < NUM_VOICES; v++) {
			if (v == voiceIdx) continue;
			int interval = (int)std::round((candidateVolt - voices[v].targetVoltage) * 12.f);
			score += intervalConsonance(interval);
			count++;
		}
		return count > 0 ? score / (float)count : 1.f;
	}

	// ─── Step Advance ────────────────────────────────────────────────────────

	void onVoiceStepAdvance(int voiceIdx) {
		onVoiceStepAdvanceWithRange(voiceIdx, faderRangeVolts);
	}

	void onVoiceStepAdvanceWithRange(int voiceIdx, float rangeVolts) {
		VoiceState& voice = voices[voiceIdx];

		// Read root with CV (1V = 1 semitone, wraps 0-11)
		int rootNote = (int)std::round(params[ROOT_PARAM].getValue());
		if (inputs[ROOT_CV_INPUT].isConnected()) {
			rootNote += (int)std::round(inputs[ROOT_CV_INPUT].getVoltage());
		}
		rootNote = ((rootNote % 12) + 12) % 12;

		// Read scale with CV (1V = 1 scale index, clamped 0-11)
		int scaleIndex = (int)std::round(params[SCALE_PARAM].getValue());
		if (inputs[SCALE_CV_INPUT].isConnected()) {
			scaleIndex += (int)std::round(inputs[SCALE_CV_INPUT].getVoltage());
		}
		scaleIndex = clamp(scaleIndex, 0, 11);

		int numSteps = (int)std::round(params[STEPS_PARAM].getValue());

		// Read slew with CV (±5V = ±100%)
		float slewPercent = params[SLEW_PARAM].getValue();
		if (inputs[SLEW_CV_INPUT].isConnected()) {
			slewPercent += inputs[SLEW_CV_INPUT].getVoltage() / 5.f;
		}
		slewPercent = clamp(slewPercent, 0.f, 1.f);

		// Get base voltage from current step's fader
		float faderValue = params[FADER_PARAM_0 + voice.currentStep].getValue();
		float baseVolt = faderToVoltage(faderValue, rootNote, scaleIndex, rangeVolts);

		// Read wander with CV (0=faithful, 1=wanders; invert for internal stability)
		float instability = params[WANDER_A_PARAM + voiceIdx].getValue();
		if (inputs[WANDER_A_INPUT + voiceIdx].isConnected()) {
			instability += inputs[WANDER_A_INPUT + voiceIdx].getVoltage() / 5.f;
		}
		instability = clamp(instability, 0.f, 1.f);
		float stability = 1.f - instability;

		// Generate seed from step counter, voice, and step position
		uint32_t seed = voice.stepCounter * 2654435761u
		              + voiceIdx * 340573321u
		              + voice.currentStep * 1234577u;
		// Ensure seed is non-zero for xorshift
		if (seed == 0) seed = 1;

		// Harmonic Lock: each voice deviates from its own step's fader note but
		// biases toward intervals consonant with the other voices. Generate 3
		// candidates and pick the one that scores best against the other voices.
		if (harmonicLock) {
			float bestVolt = baseVolt;
			float bestScore = -1.f;
			for (int c = 0; c < 3; c++) {
				uint32_t candidateSeed = seed + c * 7919u;
				if (candidateSeed == 0) candidateSeed = 1;
				float candidate = selectDeviationNote(
					baseVolt, stability, rootNote, scaleIndex, rangeVolts, candidateSeed);
				float score = scoreConsonance(candidate, voiceIdx);
				if (score > bestScore) {
					bestScore = score;
					bestVolt = candidate;
				}
			}
			voice.targetVoltage = bestVolt;
		} else {
			voice.targetVoltage = selectDeviationNote(
				baseVolt, stability, rootNote, scaleIndex, rangeVolts, seed);
		}

		// Calculate adaptive slew
		calculateSlewRate(voiceIdx, numSteps, slewPercent);
	}

	// ─── Process ─────────────────────────────────────────────────────────────

	void process(const ProcessArgs& args) override {
		int numSteps = (int)std::round(params[STEPS_PARAM].getValue());
		if (inputs[STEPS_CV_INPUT].isConnected()) {
			numSteps += (int)std::round(inputs[STEPS_CV_INPUT].getVoltage());
			numSteps = clamp(numSteps, 1, 8);
		}

		// ── Read expander overrides ──
		ExpanderToFugueMessage expanderMsg = {};
		expanderMsg.sampleHoldEnabled = false;
		for (int v = 0; v < NUM_VOICES; v++) {
			expanderMsg.voices[v].stepsOverride = -1;
			expanderMsg.voices[v].rangeOverride = -1.f;
			expanderMsg.voices[v].sleepDivision = 1;
			expanderMsg.voices[v].probability = 1.f;
		}

		if (rightExpander.module && rightExpander.module->model == modelFugueX) {
			ExpanderToFugueMessage* rxMsg =
				(ExpanderToFugueMessage*)rightExpander.module->leftExpander.consumerMessage;
			if (rxMsg && rxMsg->connected) {
				expanderMsg = *rxMsg;
			}
		}

		sampleHoldEnabled = expanderMsg.sampleHoldEnabled;

		// ── Handle randomize request ──
		if (expanderMsg.randomizeRequested) {
			for (int i = 0; i < NUM_STEPS; i++) {
				params[FADER_PARAM_0 + i].setValue(random::uniform());
			}
		}

		// ── Reset (input or button) ──
		bool resetTriggered = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		bool resetBtnTriggered = resetButtonTrigger.process(params[RESET_BUTTON_PARAM].getValue());
		if (resetTriggered || resetBtnTriggered) {
			for (int v = 0; v < NUM_VOICES; v++) {
				voices[v].currentStep = 0;
				voices[v].stepCounter = 0;
				sleeping[v] = false;
				sleepCounter[v] = 0;
				sampleHoldHolding[v] = false;
				onVoiceStepAdvance(v);
			}
		}

		// ── Prepare expander output message ──
		FugueToExpanderMessage* txMsg = (FugueToExpanderMessage*)rightExpander.producerMessage;

		// ── Per-voice clock processing ──
		for (int v = 0; v < NUM_VOICES; v++) {
			VoiceState& voice = voices[v];
			float clockVolt = getClockVoltage(v);

			// Per-voice step count (capped at global)
			int voiceSteps = numSteps;
			if (expanderMsg.voices[v].stepsOverride > 0) {
				voiceSteps = std::min(expanderMsg.voices[v].stepsOverride, numSteps);
			}

			// Per-voice fader range override
			float voiceRange = faderRangeVolts;
			if (expanderMsg.voices[v].rangeOverride > 0.f) {
				voiceRange = expanderMsg.voices[v].rangeOverride;
			}

			voice.clockHigh = (clockVolt >= 1.0f);
			voice.clockTimer += args.sampleTime;

			bool clockRose = false;
			if (voice.clockTrigger.process(clockVolt, 0.1f, 1.f)) {
				clockRose = true;
				if (voice.clockTimer > 0.001f) {
					voice.clockPeriod = voice.clockTimer;
				}
				voice.clockTimer = 0.f;

				// ── Sleep logic ──
				int sleepDiv = expanderMsg.voices[v].sleepDivision;
				if (sleeping[v]) {
					sleepCounter[v]--;
					if (sleepCounter[v] <= 0) {
						sleeping[v] = false;
					}
					// Don't advance step while sleeping
				} else {
					voice.stepCounter++;
					voice.currentStep++;
					if (voice.currentStep >= voiceSteps) {
						voice.currentStep = 0;
						// Start sleeping at end of cycle
						if (sleepDiv > 0) {
							sleeping[v] = true;
							sleepCounter[v] = sleepDiv;
						}
					}

					// ── Probability ──
					float prob = expanderMsg.voices[v].probability;
					if (prob < 1.f) {
						float roll = (float)(xorshift32(probRng) & 0x7FFFFFFF) / (float)0x7FFFFFFF;
						probGateSuppress[v] = (roll >= prob);
					} else {
						probGateSuppress[v] = false;
					}

					onVoiceStepAdvanceWithRange(v, voiceRange);
				}
			}

			// ── Slew / S&H ──
			if (sampleHoldEnabled) {
				// In S&H mode, only update voltage when gate fires
				int toggleIdx = voice.currentStep * NUM_VOICES + v;
				bool toggleOn = params[GATE_TOGGLE_PARAM_0 + toggleIdx].getValue() > 0.5f;
				if (clockRose && toggleOn && !sleeping[v] && !probGateSuppress[v]) {
					voice.currentVoltage = voice.targetVoltage;
					sampleHoldHolding[v] = true;
				}
			} else {
				processSlew(v, args.sampleTime);
			}

			// ── CV output ──
			outputs[CV_A_OUTPUT + v].setVoltage(voice.currentVoltage);

			// ── Gate output ──
			int toggleIdx = voice.currentStep * NUM_VOICES + v;
			bool toggleOn = params[GATE_TOGGLE_PARAM_0 + toggleIdx].getValue() > 0.5f;
			bool gateActive = voice.clockHigh && toggleOn && !sleeping[v] && !probGateSuppress[v];
			outputs[GATE_A_OUTPUT + v].setVoltage(gateActive ? 10.f : 0.f);

			// ── Fill expander message ──
			if (txMsg) {
				txMsg->voices[v].currentStep = voice.currentStep;
				txMsg->voices[v].clockHigh = voice.clockHigh;
				txMsg->voices[v].clockRose = clockRose;
				txMsg->voices[v].currentVoltage = voice.currentVoltage;
				txMsg->voices[v].gateOn = gateActive;
				txMsg->voices[v].sleeping = sleeping[v];
				txMsg->voices[v].sleepCounter = sleepCounter[v];
				txMsg->voices[v].sleepDivision = expanderMsg.voices[v].sleepDivision;
			}
		}

		// ── Send expander message ──
		if (txMsg) {
			txMsg->numSteps = numSteps;
			rightExpander.requestMessageFlip();
		}

		// ── Update lights ──
		for (int step = 0; step < NUM_STEPS; step++) {
			for (int v = 0; v < NUM_VOICES; v++) {
				int idx = step * NUM_VOICES + v;
				float brightness = params[GATE_TOGGLE_PARAM_0 + idx].getValue();
				if (step >= numSteps) brightness *= 0.15f;
				lights[GATE_LIGHT_0 + idx].setBrightness(brightness);
			}
		}

		for (int step = 0; step < NUM_STEPS; step++) {
			for (int v = 0; v < NUM_VOICES; v++) {
				int toggleIdx = step * NUM_VOICES + v;
				bool gateOn = params[GATE_TOGGLE_PARAM_0 + toggleIdx].getValue() > 0.5f;
				int lightBase = (v == 0) ? STEP_A_LIGHT_0 : (v == 1) ? STEP_B_LIGHT_0 : STEP_C_LIGHT_0;
				lights[lightBase + step].setBrightness(
					(step == voices[v].currentStep && step < numSteps && gateOn) ? 1.f : 0.f);
			}
		}
	}
};

// ─── FaderParamQuantity Implementation ───────────────────────────────────────

std::string FaderParamQuantity::getDisplayValueString() {
	Fugue* m = dynamic_cast<Fugue*>(this->module);
	if (!m) return ParamQuantity::getDisplayValueString();

	int rootNote = (int)std::round(m->params[Fugue::ROOT_PARAM].getValue());
	int scaleIndex = (int)std::round(m->params[Fugue::SCALE_PARAM].getValue());
	float faderValue = getValue();
	float voltage = m->faderToVoltage(faderValue, rootNote, scaleIndex, m->faderRangeVolts);

	// Convert voltage to note name (0V = C4 in 1V/oct standard)
	static const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	int totalSemitones = (int)std::round(voltage * 12.f);
	int octave = 4 + (int)std::floor((float)totalSemitones / 12.f);
	int noteIdx = ((totalSemitones % 12) + 12) % 12;

	return string::f("%s%d", noteNames[noteIdx], octave);
}

// ─── Custom Horizontal Wander Slider (SvgSlider) ─────────────────────────────

struct WanderSlider : app::SvgSlider {
	WanderSlider() {
		horizontal = true;
		setBackgroundSvg(Svg::load(asset::plugin(pluginInstance, "res/slider-horiz-bg.svg")));
		setHandleSvg(Svg::load(asset::plugin(pluginInstance, "res/slider-horiz-handle.svg")));
		setHandlePosCentered(
			math::Vec(8.1f, 8.12f),     // min: left
			math::Vec(89.34f, 8.12f)    // max: right
		);
	}
};

// ─── Widget ──────────────────────────────────────────────────────────────────

struct FugueWidget : ModuleWidget {
	FugueWidget(Fugue* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/fugue.svg")));


		// ══════════════════════════════════════════════════════════════════════
		// LAYOUT CONSTANTS (mm)
		// ══════════════════════════════════════════════════════════════════════

		// ── Control section (top-left) ──
		const float knobCol1X = 10.16f;
		const float knobCol2X = 22.86f;

		const float knobRow1Y = 22.9f;    // Root, Scale
		const float cvRow1Y = 33.87f;     // Root CV, Scale CV
		const float knobRow2Y = 50.62f;   // Steps, Slew
		const float cvRow2Y = 61.63f;     // Steps CV, Slew CV

		const float resetY = 77.23f;      // Reset jack + button

		// ── Sequencer area (top-right) ──
		const float faderStartX = 35.56f;
		const float faderSpacing = 8.15f;

		const float stepLedY_A = 19.05f;
		const float stepLedY_B = 22.05f;
		const float stepLedY_C = 25.04f;

		const float faderY = 43.03f;      // center Y for sliders

		const float gateRowY_A = 63.05f;
		const float gateRowY_B = 70.03f;
		const float gateRowY_C = 77.06f;

		// ── Voice rows (bottom) ──
		const float voiceA_Y = 99.63f;
		const float voiceB_Y = 108.42f;
		const float voiceC_Y = 117.22f;

		const float clockX = 10.16f;
		const float wanderCvX = 22.86f;       // next to clock
		const float wanderSliderX = 50.60f;   // center of 33mm slider
		const float gateOutX = 79.94f;
		const float cvOutX = 89.60f;

		// ══════════════════════════════════════════════════════════════════════
		// CONTROL SECTION (top-left)
		// ══════════════════════════════════════════════════════════════════════

		// ── Knob row 1: Root, Scale ──
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(knobCol1X, knobRow1Y)), module, Fugue::ROOT_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(knobCol2X, knobRow1Y)), module, Fugue::SCALE_PARAM));

		// ── CV row 1: Root CV, Scale CV ──
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(knobCol1X, cvRow1Y)), module, Fugue::ROOT_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(knobCol2X, cvRow1Y)), module, Fugue::SCALE_CV_INPUT));

		// ── Knob row 2: Steps, Slew ──
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(knobCol1X, knobRow2Y)), module, Fugue::STEPS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(knobCol2X, knobRow2Y)), module, Fugue::SLEW_PARAM));

		// ── CV row 2: Steps CV, Slew CV ──
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(knobCol1X, cvRow2Y)), module, Fugue::STEPS_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(knobCol2X, cvRow2Y)), module, Fugue::SLEW_CV_INPUT));

		// ── Reset: jack + momentary button ──
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(knobCol1X, resetY)), module, Fugue::RESET_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(knobCol2X, resetY)), module, Fugue::RESET_BUTTON_PARAM));

		// ══════════════════════════════════════════════════════════════════════
		// SEQUENCER AREA (top-right)
		// ══════════════════════════════════════════════════════════════════════

		for (int i = 0; i < NUM_STEPS; i++) {
			float x = faderStartX + i * faderSpacing;

			// ── Step indicator LEDs (above faders) — all red ──
			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(x, stepLedY_A)), module, Fugue::STEP_A_LIGHT_0 + i));
			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(x, stepLedY_B)), module, Fugue::STEP_B_LIGHT_0 + i));
			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(x, stepLedY_C)), module, Fugue::STEP_C_LIGHT_0 + i));

			// ── Pitch fader ──
			addParam(createParamCentered<VCVSlider>(mm2px(Vec(x, faderY)), module, Fugue::FADER_PARAM_0 + i));

			// ── Gate toggle buttons (3 rows, all red) ──
			int idxA = i * NUM_VOICES + 0;
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
				mm2px(Vec(x, gateRowY_A)), module,
				Fugue::GATE_TOGGLE_PARAM_0 + idxA, Fugue::GATE_LIGHT_0 + idxA));

			int idxB = i * NUM_VOICES + 1;
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
				mm2px(Vec(x, gateRowY_B)), module,
				Fugue::GATE_TOGGLE_PARAM_0 + idxB, Fugue::GATE_LIGHT_0 + idxB));

			int idxC = i * NUM_VOICES + 2;
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
				mm2px(Vec(x, gateRowY_C)), module,
				Fugue::GATE_TOGGLE_PARAM_0 + idxC, Fugue::GATE_LIGHT_0 + idxC));
		}

		// ══════════════════════════════════════════════════════════════════════
		// VOICE ROWS (bottom: Clock → Wander CV → Wander Slider → Gate Out → CV Out)
		// ══════════════════════════════════════════════════════════════════════

		const float voiceYs[] = {voiceA_Y, voiceB_Y, voiceC_Y};
		const int clockInputs[] = {Fugue::CLOCK_A_INPUT, Fugue::CLOCK_B_INPUT, Fugue::CLOCK_C_INPUT};
		const int wanderParams[] = {Fugue::WANDER_A_PARAM, Fugue::WANDER_B_PARAM, Fugue::WANDER_C_PARAM};
		const int wanderInputs[] = {Fugue::WANDER_A_INPUT, Fugue::WANDER_B_INPUT, Fugue::WANDER_C_INPUT};
		const int gateOutputs[] = {Fugue::GATE_A_OUTPUT, Fugue::GATE_B_OUTPUT, Fugue::GATE_C_OUTPUT};
		const int cvOutputs[] = {Fugue::CV_A_OUTPUT, Fugue::CV_B_OUTPUT, Fugue::CV_C_OUTPUT};

		for (int v = 0; v < NUM_VOICES; v++) {
			float y = voiceYs[v];

			// Clock input
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(clockX, y)), module, clockInputs[v]));

			// Wander CV input (next to clock)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(wanderCvX, y)), module, wanderInputs[v]));

			// Wander horizontal slider
			addParam(createParamCentered<WanderSlider>(mm2px(Vec(wanderSliderX, y)), module, wanderParams[v]));

			// Gate output
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(gateOutX, y)), module, gateOutputs[v]));

			// CV output
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(cvOutX, y)), module, cvOutputs[v]));
		}
	}

	// ── Context Menu ──
	void appendContextMenu(Menu* menu) override {
		Fugue* module = dynamic_cast<Fugue*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Fader Range"));

		menu->addChild(createCheckMenuItem("1V (1 octave)", "",
			[=]() { return module->faderRangeVolts == 1.f; },
			[=]() { module->faderRangeVolts = 1.f; }
		));
		menu->addChild(createCheckMenuItem("2V (2 octaves)", "",
			[=]() { return module->faderRangeVolts == 2.f; },
			[=]() { module->faderRangeVolts = 2.f; }
		));
		menu->addChild(createCheckMenuItem("5V (5 octaves)", "",
			[=]() { return module->faderRangeVolts == 5.f; },
			[=]() { module->faderRangeVolts = 5.f; }
		));

		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Harmonic Lock", "",
			&module->harmonicLock));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Randomize Sequence", "",
			[=]() {
				for (int i = 0; i < NUM_STEPS; i++) {
					module->params[Fugue::FADER_PARAM_0 + i].setValue(random::uniform());
				}
			}
		));
	}
};

Model* modelFugue = createModel<Fugue, FugueWidget>("Fugue");
