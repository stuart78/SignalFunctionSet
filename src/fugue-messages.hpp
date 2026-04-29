#pragma once

static const int FUGUE_NUM_STEPS = 8;
static const int FUGUE_NUM_VOICES = 3;

// Fugue → FugueX: voice state for display and trigger generation
struct FugueToExpanderMessage {
	int numSteps;
	struct VoiceInfo {
		int currentStep;
		bool clockHigh;
		bool clockRose;      // true on the sample the clock triggered
		float currentVoltage;
		bool gateOn;          // gate toggle state for current step
		bool sleeping;        // voice is in sleep state
		int sleepCounter;     // clocks remaining in sleep
		int sleepDivision;    // total sleep clocks (for brightness calc)
	} voices[FUGUE_NUM_VOICES];
};

// FugueX → Fugue: control overrides
struct ExpanderToFugueMessage {
	bool connected;
	bool randomizeRequested;
	bool sampleHoldEnabled;
	struct VoiceOverride {
		int stepsOverride;    // -1 = no override, else 1-8 (capped at global)
		float rangeOverride;  // -1 = no override, else 1.f/2.f/5.f
		int sleepDivision;    // 0 = no sleep, else number of clocks to sleep after cycle
		float probability;    // 0-1, gate fire probability (1.0 = always)
	} voices[FUGUE_NUM_VOICES];
};
