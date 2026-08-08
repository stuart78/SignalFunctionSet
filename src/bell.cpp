#include "plugin.hpp"
#include "bell_voice.h"
#include <osdialog.h>
#include <vector>
#include <string>
#include <cstring>
#include <initializer_list>

// ---------------------------------------------------------------------------
// Bell — compact DX7 cartridge-player voice built on the vendored msfa engine.
// Loads multiple .syx banks, selects bank + voice (knob/CV/Back-Fwd), and plays
// polyphonically from V/OCT + GATE (+ velocity). Timbre macros come next.
// ---------------------------------------------------------------------------

static const char BELL_SYX_FILTERS[] = "DX7 SysEx (.syx):syx;All files (*.*):*";
static const int  MAX_BANKS = 16;

// --- tiny base64 for persisting bank data in the patch JSON ---
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64encode(const uint8_t* d, int n) {
	std::string o;
	for (int i = 0; i < n; i += 3) {
		int b = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
		o += B64[(b >> 18) & 63];
		o += B64[(b >> 12) & 63];
		o += (i + 1 < n) ? B64[(b >> 6) & 63] : '=';
		o += (i + 2 < n) ? B64[b & 63] : '=';
	}
	return o;
}
static std::vector<uint8_t> b64decode(const std::string& s) {
	int t[256]; for (int i = 0; i < 256; i++) t[i] = -1;
	for (int i = 0; i < 64; i++) t[(uint8_t)B64[i]] = i;
	std::vector<uint8_t> o; int val = 0, bits = -8;
	for (char c : s) {
		if (t[(uint8_t)c] < 0) continue;
		val = (val << 6) | t[(uint8_t)c]; bits += 6;
		if (bits >= 0) { o.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
	}
	return o;
}

// DX7 algorithm block-diagram layout, ported from Dexed's AlgoDisplay.cpp
// (GPLv3). Per op: id (DX7 OP 1-6), grid x, grid y, link style, feedback style.
static const struct AlgoOp { uint8_t id, x, y, link, fb; } kAlgo[32][6] = {
	{ {6,3,0,0,1}, {5,3,1,0,0}, {4,3,2,0,0}, {3,3,3,2,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 1
	{ {6,3,0,0,0}, {5,3,1,0,0}, {4,3,2,0,0}, {3,3,3,2,0}, {2,2,2,0,1}, {1,2,3,1,0} }, // 2
	{ {6,3,1,0,1}, {5,3,2,0,0}, {4,3,3,2,0}, {3,2,1,0,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 3
	{ {6,3,1,0,2}, {5,3,2,0,0}, {4,3,3,2,0}, {3,2,1,0,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 4
	{ {6,4,2,0,1}, {5,4,3,2,0}, {4,3,2,0,0}, {3,3,3,1,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 5
	{ {6,4,2,0,3}, {5,4,3,2,0}, {4,3,2,0,0}, {3,3,3,1,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 6
	{ {6,4,1,0,1}, {5,4,2,7,0}, {4,3,2,0,0}, {3,3,3,2,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 7
	{ {6,4,1,0,0}, {5,4,2,7,0}, {4,3,2,0,4}, {3,3,3,2,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 8
	{ {6,4,1,0,0}, {5,4,2,7,0}, {4,3,2,0,0}, {3,3,3,2,0}, {2,2,2,0,1}, {1,2,3,1,0} }, // 9
	{ {6,2,2,0,0}, {5,1,2,1,0}, {4,2,3,1,0}, {3,3,1,0,1}, {2,3,2,0,0}, {1,3,3,2,0} }, // 10
	{ {6,2,2,0,1}, {5,1,2,1,0}, {4,2,3,1,0}, {3,3,1,0,0}, {2,3,2,0,0}, {1,3,3,2,0} }, // 11
	{ {6,3,2,7,0}, {5,2,2,0,0}, {4,1,2,1,0}, {3,2,3,6,0}, {2,4,2,0,1}, {1,4,3,2,0} }, // 12
	{ {6,3,2,7,1}, {5,2,2,0,0}, {4,1,2,1,0}, {3,2,3,6,0}, {2,4,2,0,0}, {1,4,3,2,0} }, // 13
	{ {6,4,1,7,1}, {5,3,1,0,0}, {4,3,2,0,0}, {3,3,3,2,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 14
	{ {6,4,1,7,0}, {5,3,1,0,0}, {4,3,2,0,0}, {3,3,3,2,0}, {2,2,2,0,4}, {1,2,3,1,0} }, // 15
	{ {6,4,1,0,1}, {5,4,2,7,0}, {4,3,1,0,0}, {3,3,2,0,0}, {2,2,2,1,0}, {1,3,3,0,0} }, // 16
	{ {6,4,1,0,0}, {5,4,2,7,0}, {4,3,1,0,0}, {3,3,2,0,0}, {2,2,2,1,4}, {1,3,3,0,0} }, // 17
	{ {6,4,0,0,0}, {5,4,1,0,0}, {4,4,2,7,0}, {3,3,2,0,4}, {2,2,2,1,0}, {1,3,3,0,0} }, // 18
	{ {6,3,2,3,1}, {5,4,3,2,0}, {4,3,3,1,0}, {3,2,1,0,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 19
	{ {6,4,2,0,0}, {5,3,2,1,0}, {4,4,3,2,0}, {3,1,2,3,1}, {2,2,3,6,0}, {1,1,3,1,0} }, // 20
	{ {6,3,2,3,0}, {5,4,3,2,0}, {4,3,3,1,0}, {3,1,2,3,1}, {2,2,3,1,0}, {1,1,3,1,0} }, // 21
	{ {6,3,2,4,1}, {5,4,3,2,0}, {4,3,3,1,0}, {3,2,3,1,0}, {2,1,2,0,0}, {1,1,3,1,0} }, // 22
	{ {6,3,2,3,1}, {5,4,3,2,0}, {4,3,3,1,0}, {3,2,2,0,0}, {2,2,3,1,0}, {1,1,3,1,0} }, // 23
	{ {6,3,2,4,1}, {5,4,3,2,0}, {4,3,3,1,0}, {3,2,3,1,0}, {2,1,3,1,0}, {1,0,3,1,0} }, // 24
	{ {6,3,2,3,1}, {5,4,3,2,0}, {4,3,3,1,0}, {3,2,3,1,0}, {2,1,3,1,0}, {1,0,3,1,0} }, // 25
	{ {6,4,2,0,1}, {5,3,2,1,0}, {4,4,3,2,0}, {3,2,2,0,0}, {2,2,3,6,0}, {1,1,3,1,0} }, // 26
	{ {6,4,2,0,0}, {5,3,2,1,0}, {4,4,3,2,0}, {3,2,2,0,1}, {2,2,3,6,0}, {1,1,3,1,0} }, // 27
	{ {6,4,3,2,0}, {5,3,1,0,1}, {4,3,2,0,0}, {3,3,3,1,0}, {2,2,2,0,0}, {1,2,3,1,0} }, // 28
	{ {6,4,2,0,1}, {5,4,3,2,0}, {4,3,2,0,0}, {3,3,3,1,0}, {2,2,3,1,0}, {1,1,3,1,0} }, // 29
	{ {6,4,3,2,0}, {5,3,1,0,1}, {4,3,2,0,0}, {3,3,3,1,0}, {2,2,3,1,0}, {1,1,3,1,0} }, // 30
	{ {6,4,2,0,1}, {5,4,3,2,0}, {4,3,3,1,0}, {3,2,3,1,0}, {2,1,3,1,0}, {1,0,3,1,0} }, // 31
	{ {6,5,3,2,1}, {5,4,3,1,0}, {4,3,3,1,0}, {3,2,3,1,0}, {2,1,3,1,0}, {1,0,3,1,0} }, // 32
};

struct Bell : Module {
	enum ParamId {
		VOICE_PARAM, BANK_PARAM, VOICE_PREV_PARAM, VOICE_NEXT_PARAM,
		TUNE_PARAM, BRIGHTNESS_PARAM, FEEDBACK_PARAM,
		// ── appended; Rack serialises by index, so these stay at the end ────
		MORPH_PARAM, ALGOB_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT, GATE_INPUT, VEL_INPUT, VOICE_CV_INPUT, BANK_CV_INPUT,
		TUNE_CV_INPUT, BRIGHTNESS_CV_INPUT, FEEDBACK_CV_INPUT,
		MORPH_CV_INPUT, ALGOB_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId { AUDIO_OUTPUT, VCO_OUTPUT, ENV_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	struct LoadedBank {
		std::string name;
		uint8_t data[4096];
		int voiceCount = 32;   // selectable voices (DX7 .syx = 32; Eno default = 4)
	};

	BellEngine engine;
	std::vector<LoadedBank> banks;
	int   curBank = 0;
	int   blockPos = 0;
	float curSR = 0.f;
	bool  gateHigh[BellEngine::MAX_CH] = {};      // current gate level (per-sample)
	bool  noteOnState[BellEngine::MAX_CH] = {};   // engine note currently keyed down
	bool  trigPending[BellEngine::MAX_CH] = {};   // rising edge seen since last block
	int   trigNote[BellEngine::MAX_CH] = {};
	int   trigVel[BellEngine::MAX_CH] = {};
	int   lastNote[BellEngine::MAX_CH] = {};
	int   lastVel[BellEngine::MAX_CH] = {};
	float tuneOctCached = 0.f;
	dsp::SchmittTrigger prevTrig, nextTrig;

	// Operator on/off (msfa index order: [0]=OP6 … [5]=OP1).
	bool opEnabled[6] = {true, true, true, true, true, true};

	// display mirrors (GUI thread reads)
	int  dispVoice = 0, dispBank = 0, dispAlgo = 0, dispCarrier = 0;
	float dispMorph = 0.f;              // display: how far toward algoB
	int   dispAlgoB = 0;
	char dispName[11] = {0};
	int  screenTab = 0;   // 0 = Operators, 1 = Envelope
	float outLevelDb = 0.f;   // user output level trim (context menu), -12..+12 dB
	// Bank / Voice CV scaling (context menu). 0 = 0-10V across the range,
	// 1 = 0.1V per step, 2 = notes (1V/oct, one semitone per step, C4/0V = index 0).
	int bankCvMode = 0, voiceCvMode = 0;

	// Envelope display: static carrier-EG shape (recomputed on voice change) +
	// a live amplitude follower of the audio output.
	static const int ENV_N = 120, SCOPE_N = 120, SCOPE_DECIM = 256;
	float envCurve[ENV_N] = {};
	bool  envValid = false;
	int   envShownVoice = -1, envShownBank = -1, envShownOps = -1;
	float scope[SCOPE_N] = {};
	int   scopeHead = 0;
	float scopeAcc = 0.f;
	int   scopeCnt = 0;

	// Per-channel envelope follower (ENV output): one-pole on the rectified audio
	// voltage, fast attack / slower release.
	float envFollow[BellEngine::MAX_CH] = {};
	float envAtkCoeff = 0.05f, envRelCoeff = 0.001f;
	// DC blocker, per channel. Some DX7 patches put a real offset on the output
	// (Violin 3 of the built-in bank sits at 2.9% of peak), and because the offset
	// is gated by the amplitude envelope it arrives and leaves with every note --
	// a woofer push rather than a sound. Cheap insurance for arbitrary cartridges,
	// which cannot be tested here. 5Hz, so the lowest playable note is untouched.
	float dcX[BellEngine::MAX_CH] = {}, dcY[BellEngine::MAX_CH] = {};
	float dcVX[BellEngine::MAX_CH] = {}, dcVY[BellEngine::MAX_CH] = {};
	float dcR = 0.99935f;

	// Toggle by DX7 op id (1-6); maps to engine index 6-id.
	void toggleOp(int dx7id) {
		int i = 6 - dx7id;
		if (i >= 0 && i < 6) opEnabled[i] = !opEnabled[i];
	}

	Bell() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(VOICE_PARAM, 0.f, 31.f, 0.f, "Voice", "", 0.f, 1.f, 1.f);
		paramQuantities[VOICE_PARAM]->snapEnabled = true;
		configParam(BANK_PARAM, 0.f, (float)(MAX_BANKS - 1), 0.f, "Bank");
		paramQuantities[BANK_PARAM]->snapEnabled = true;
		configButton(VOICE_PREV_PARAM, "Previous voice");
		configButton(VOICE_NEXT_PARAM, "Next voice");
		configParam(TUNE_PARAM, -12.f, 12.f, 0.f, "Tune", " semitones");
		configParam(BRIGHTNESS_PARAM, -1.f, 1.f, 0.f, "Brightness", "%", 0.f, 100.f);
		configParam(FEEDBACK_PARAM, -7.f, 7.f, 0.f, "Feedback offset");
		paramQuantities[FEEDBACK_PARAM]->snapEnabled = true;
		configInput(VOCT_INPUT, "V/oct (poly)");
		configInput(GATE_INPUT, "Gate (poly)");
		configInput(VEL_INPUT, "Velocity (poly)");
		configInput(VOICE_CV_INPUT, "Voice/patch select CV (scale set in right-click menu)");
		configInput(BANK_CV_INPUT, "Bank select CV (scale set in right-click menu)");
		configInput(TUNE_CV_INPUT, "Tune CV (1V/oct)");
		configInput(BRIGHTNESS_CV_INPUT, "Brightness CV (±5V)");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.f, "Algorithm morph", "%", 0.f, 100.f);
		configParam(ALGOB_PARAM, 0.f, 31.f, 0.f, "Morph target algorithm", "", 0.f, 1.f, 1.f);
		getParamQuantity(ALGOB_PARAM)->snapEnabled = true;
		configInput(MORPH_CV_INPUT, "Algorithm morph CV (0–10V)");
		configInput(ALGOB_CV_INPUT, "Morph target algorithm CV (0.32V per algorithm)");
		configInput(FEEDBACK_CV_INPUT, "Feedback CV (±5V)");
		configOutput(AUDIO_OUTPUT, "VCA — voice audio with its internal envelope (poly)");
		configOutput(VCO_OUTPUT, "VCO — raw tone, no internal envelope (poly)");
		configOutput(ENV_OUTPUT, "Envelope follower — 0-10V tracking the audio amplitude (poly)");

		// Seed with the engine's built-in default bank (Brian Eno DX7 patches).
		LoadedBank def;
		def.name = "Eno '87";
		def.voiceCount = 4;        // only four published patches
		engine.getBank(def.data);
		banks.push_back(def);
		engine.getVoiceName(dispName);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		curSR = e.sampleRate;
		engine.setSampleRate(curSR);
		engine.getVoiceName(dispName);
	}

	// CV → index offset. mode 0 = 0-10V across `span`, 1 = 0.1V/step, 2 = 1V/oct notes.
	static int cvIndex(float v, int mode, int span) {
		if (mode == 1) return (int) std::round(v * 10.f);   // 0.1V per step
		if (mode == 2) return (int) std::round(v * 12.f);   // one semitone per step (0V/C4 = 0)
		return (int) std::round(v / 10.f * (float) std::max(1, span - 1));
	}
	int selectedBank() {
		int b = (int) std::round(params[BANK_PARAM].getValue());
		if (inputs[BANK_CV_INPUT].isConnected())
			b += cvIndex(inputs[BANK_CV_INPUT].getVoltage(), bankCvMode, (int) banks.size());
		return clamp(b, 0, (int) banks.size() - 1);
	}
	int currentVoiceCount() const {
		if (curBank >= 0 && curBank < (int) banks.size())
			return clamp(banks[curBank].voiceCount, 1, 32);
		return 32;
	}
	int selectedVoice() {
		int v = (int) std::round(params[VOICE_PARAM].getValue());
		if (inputs[VOICE_CV_INPUT].isConnected())
			v += cvIndex(inputs[VOICE_CV_INPUT].getVoltage(), voiceCvMode, 32);
		return clamp(v, 0, currentVoiceCount() - 1);
	}
	void bumpVoice(int d) {
		int n = currentVoiceCount();
		int v = (int) std::round(params[VOICE_PARAM].getValue());
		if (v >= n) v = n - 1;
		v = ((v + d) % n + n) % n;
		params[VOICE_PARAM].setValue((float) v);
	}

	void process(const ProcessArgs& args) override {
		if (args.sampleRate != curSR) {
			curSR = args.sampleRate;
			engine.setSampleRate(curSR);
			envAtkCoeff = 1.f - std::exp(-1.f / (0.0008f * curSR));  // ~0.8ms attack
			envRelCoeff = 1.f - std::exp(-1.f / (0.040f * curSR));   // ~40ms release
			dcR = std::exp(-2.f * (float)M_PI * 5.f / (float)curSR);  // 5Hz DC blocker
		}

		// Back/Fwd voice buttons (every sample so taps aren't missed).
		if (nextTrig.process(params[VOICE_NEXT_PARAM].getValue())) bumpVoice(+1);
		if (prevTrig.process(params[VOICE_PREV_PARAM].getValue())) bumpVoice(-1);

		int chans = std::max(1, std::max(inputs[VOCT_INPUT].getChannels(),
		                                 inputs[GATE_INPUT].getChannels()));
		if (chans > BellEngine::MAX_CH) chans = BellEngine::MAX_CH;
		outputs[AUDIO_OUTPUT].setChannels(chans);
		// VCO is a standard always-on oscillator: it sounds whenever its jack is
		// patched, tracking V/oct (and droning at the base pitch if V/oct is open).
		bool vcoOn = outputs[VCO_OUTPUT].isConnected();
		outputs[VCO_OUTPUT].setChannels(vcoOn ? chans : 0);

		// Gate edges are captured EVERY sample (the engine only renders/consumes
		// at block boundaries). A 16th-note clock pulse can be shorter than one
		// 64-sample block, so a once-per-block check would miss it — that was the
		// dropped-note bug. The rising edge latches trigPending + the note/vel at
		// that instant; the block boundary then triggers it.
		for (int ch = 0; ch < chans; ch++) {
			float g = inputs[GATE_INPUT].getVoltage(ch);
			if (!gateHigh[ch] && g >= 1.f) {
				gateHigh[ch] = true;
				float voct = inputs[VOCT_INPUT].getVoltage(ch) + tuneOctCached;
				trigNote[ch] = clamp((int) std::round(voct * 12.f + 60.f), 0, 127);
				trigVel[ch] = inputs[VEL_INPUT].isConnected()
					? clamp((int)(inputs[VEL_INPUT].getVoltage(ch) / 10.f * 127.f), 1, 127)
					: 100;
				trigPending[ch] = true;
			} else if (gateHigh[ch] && g <= 0.1f) {
				gateHigh[ch] = false;
			}
		}

		if (blockPos == 0) {
			bool patchChanged = false;
			int b = selectedBank();
			if (b != curBank && b >= 0 && b < (int) banks.size()) {
				curBank = b;
				engine.setBankRaw(banks[b].data);
				patchChanged = true;
			}
			int v = selectedVoice();
			if (v != engine.voice()) { engine.setVoice(v); patchChanged = true; }
			engine.getVoiceName(dispName);
			dispVoice = engine.voice();
			dispBank = curBank;
			dispAlgo = engine.algorithm();
			dispCarrier = engine.carrierMask();

			// Recompute the static carrier-EG shape when the voice/bank changes.
			if (engine.voice() != envShownVoice || curBank != envShownBank) {
				engine.renderEnvShape(envCurve, ENV_N);
				envValid = true;
				envShownVoice = engine.voice();
				envShownBank = curBank;
			}

			// Operator on/off.
			for (int i = 0; i < 6; i++) engine.setOpEnabled(i, opEnabled[i]);

			// Timbre macros (global).
			float brightness = clamp(params[BRIGHTNESS_PARAM].getValue()
				+ inputs[BRIGHTNESS_CV_INPUT].getVoltage() / 5.f, -1.f, 1.f);
			engine.setBrightness(brightness);
			// Routing morph: 0 leaves every voice on the stock engine path, so a
			// patch that does not use it is bit-identical to before.
			float morph = clamp(params[MORPH_PARAM].getValue()
				+ inputs[MORPH_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			int algoB = clamp((int) std::round(params[ALGOB_PARAM].getValue()
				+ inputs[ALGOB_CV_INPUT].getVoltage() * 3.2f), 0, 31);
			engine.setMorph(algoB, morph);
			dispMorph = morph; dispAlgoB = algoB;

			int fbOff = clamp((int) std::round(params[FEEDBACK_PARAM].getValue()
				+ inputs[FEEDBACK_CV_INPUT].getVoltage() / 5.f * 7.f), -7, 7);
			engine.setFeedbackOffset(fbOff);

			// Tune (octaves), added to V/oct: knob is semitones, CV is 1V/oct.
			float tuneOct = params[TUNE_PARAM].getValue() / 12.f
				+ inputs[TUNE_CV_INPUT].getVoltage();
			tuneOctCached = tuneOct;   // used by the per-sample edge capture above

			for (int ch = 0; ch < chans; ch++) {
				float voct = inputs[VOCT_INPUT].getVoltage(ch) + tuneOct;
				bool justTrig = false;
				if (trigPending[ch]) {
					engine.noteOn(ch, trigNote[ch], trigVel[ch]);
					lastNote[ch] = trigNote[ch]; lastVel[ch] = trigVel[ch];
					noteOnState[ch] = true; justTrig = true;
					trigPending[ch] = false;
				}
				// Release only when the gate is low AND we didn't just (re)trigger
				// this block — so a pulse shorter than a block still gets one full
				// block of key-down and pings.
				if (!gateHigh[ch] && !justTrig && noteOnState[ch]) {
					engine.noteOff(ch);
					noteOnState[ch] = false;
				}
				// Continuous pitch: offset = played pitch minus the latched
				// integer note, in logfreq units. Tracks glide + tune live.
				float offV = voct - (lastNote[ch] - 60) / 12.f;
				engine.setPitchOffset(ch, BellEngine::octavesToLogfreq(offV));

				// VCO out: a continuous oscillator that tracks V/oct regardless
				// of gate (so an external ADSR can shape it).
				if (vcoOn) {
					int vnote = clamp((int) std::round(voct * 12.f + 60.f), 0, 127);
					engine.setVcoNote(ch, vnote);
					engine.setVcoPitchOffset(ch,
						BellEngine::octavesToLogfreq(voct - (vnote - 60) / 12.f));
				}
			}
			for (int ch = chans; ch < BellEngine::MAX_CH; ch++) {
				if (noteOnState[ch]) { engine.noteOff(ch); noteOnState[ch] = false; }
				gateHigh[ch] = false; trigPending[ch] = false;
			}

			// A voice/bank change latches per note at note-on, so held notes
			// keep their original patch. Re-trigger any held note so the new
			// voice is audible immediately (re-attacks the envelope).
			if (patchChanged) {
				for (int ch = 0; ch < chans; ch++)
					if (noteOnState[ch]) engine.noteOn(ch, lastNote[ch], lastVel[ch]);
			}

			engine.renderBlock(chans);
			if (vcoOn) engine.renderVcoBlock(chans);   // reuses this block's LFO
		}

		bool envOn = outputs[ENV_OUTPUT].isConnected();
		outputs[ENV_OUTPUT].setChannels(envOn ? chans : 0);
		// Follower gain toward a useful 0-10V range. The audio path now carries the
		// bulk of the makeup gain (below), so this is modest.
		const float ENV_GAIN = 2.f;
		const float outGain = std::pow(10.f, outLevelDb / 20.f);   // user OUTPUT LEVEL trim
		float blkEnv = 0.f;
		for (int ch = 0; ch < chans; ch++) {
			float n = engine.sample(ch, blockPos);
			if (!std::isfinite(n)) n = 0.f;   // never emit NaN/Inf
			// Before the gain, so the tanh below saturates symmetrically rather
			// than being pushed off centre by a patch's own offset.
			float dy = n - dcX[ch] + dcR * dcY[ch];
			dcX[ch] = n; dcY[ch] = dy; n = dy;
			// Makeup gain: DX7 patches peak well below the engine's clip point, so at
			// unity they were far quieter than a ±5V VCV oscillator. Drive into a tanh
			// so a typical patch reaches ~±5V and hot multi-carrier patches saturate
			// gracefully toward ±10V (no hard clip). OUTPUT LEVEL trims the drive.
			float v = 10.f * std::tanh(n * (1.7f * outGain));
			outputs[AUDIO_OUTPUT].setVoltage(v, ch);
			// Envelope follower (computed always so the display works unpatched).
			float rect = std::fabs(v) * ENV_GAIN;
			float c = rect > envFollow[ch] ? envAtkCoeff : envRelCoeff;
			envFollow[ch] += (rect - envFollow[ch]) * c;
			float env = clamp(envFollow[ch], 0.f, 10.f);
			if (envOn) outputs[ENV_OUTPUT].setVoltage(env, ch);
			blkEnv = std::max(blkEnv, env);
		}
		// Live follower trace for the envelope display (0..1 maps to 0..10V).
		scopeAcc = std::max(scopeAcc, blkEnv * 0.1f);
		if (++scopeCnt >= SCOPE_DECIM) {
			scope[scopeHead] = scopeAcc;
			scopeHead = (scopeHead + 1) % SCOPE_N;
			scopeAcc = 0.f; scopeCnt = 0;
		}
		if (vcoOn) {
			for (int ch = 0; ch < chans; ch++) {
				float s = engine.vcoSample(ch, blockPos);
				if (!std::isfinite(s)) s = 0.f;
				float vy = s - dcVX[ch] + dcR * dcVY[ch];
				dcVX[ch] = s; dcVY[ch] = vy; s = vy;
				outputs[VCO_OUTPUT].setVoltage(clamp(s * 16.f * outGain, -10.f, 10.f), ch);   // match audio makeup
			}
		}
		blockPos = (blockPos + 1) % BellEngine::BLOCK;
	}

	void addBank(const std::string& path) {
		FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) return;
		std::fseek(f, 0, SEEK_END);
		long sz = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (sz > 0 && sz < 1 << 20) {
			std::vector<uint8_t> raw(sz);
			if (std::fread(raw.data(), 1, sz, f) == (size_t) sz) {
				// Reuse the engine's parser by loading then reading back the
				// normalized 4096-byte bank.
				if (engine.loadBank(raw.data(), (int) sz)) {
					LoadedBank lb;
					lb.name = system::getStem(path);
					engine.getBank(lb.data);
					if ((int) banks.size() >= MAX_BANKS) banks.erase(banks.begin());
					banks.push_back(lb);
					curBank = (int) banks.size() - 1;
					params[BANK_PARAM].setValue((float) curBank);
					engine.setBankRaw(banks[curBank].data);
					engine.getVoiceName(dispName);
				}
			}
		}
		std::fclose(f);
	}
	void removeBank(int idx) {
		if (idx < 0 || idx >= (int) banks.size() || banks.size() <= 1) return;
		banks.erase(banks.begin() + idx);
		curBank = clamp(curBank, 0, (int) banks.size() - 1);
		params[BANK_PARAM].setValue((float) curBank);
		engine.setBankRaw(banks[curBank].data);
		engine.getVoiceName(dispName);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		for (auto& b : banks) {
			json_t* o = json_object();
			json_object_set_new(o, "name", json_string(b.name.c_str()));
			json_object_set_new(o, "data", json_string(b64encode(b.data, 4096).c_str()));
			json_object_set_new(o, "voiceCount", json_integer(b.voiceCount));
			json_array_append_new(arr, o);
		}
		json_object_set_new(root, "banks", arr);
		json_object_set_new(root, "curBank", json_integer(curBank));
		json_t* ops = json_array();
		for (int i = 0; i < 6; i++) json_array_append_new(ops, json_boolean(opEnabled[i]));
		json_object_set_new(root, "opEnabled", ops);
		json_object_set_new(root, "screenTab", json_integer(screenTab));
		json_object_set_new(root, "outLevelDb", json_real(outLevelDb));
		json_object_set_new(root, "bankCvMode", json_integer(bankCvMode));
		json_object_set_new(root, "voiceCvMode", json_integer(voiceCvMode));
		return root;
	}
	void dataFromJson(json_t* root) override {
		json_t* arr = json_object_get(root, "banks");
		if (arr && json_array_size(arr) > 0) {
			banks.clear();
			size_t n = json_array_size(arr);
			for (size_t i = 0; i < n && banks.size() < MAX_BANKS; i++) {
				json_t* o = json_array_get(arr, i);
				LoadedBank lb;
				json_t* nm = json_object_get(o, "name");
				lb.name = nm ? json_string_value(nm) : "Bank";
				json_t* d = json_object_get(o, "data");
				std::vector<uint8_t> raw = d ? b64decode(json_string_value(d)) : std::vector<uint8_t>();
				std::memset(lb.data, 0, 4096);
				if (raw.size() >= 4096) std::memcpy(lb.data, raw.data(), 4096);
				json_t* vc = json_object_get(o, "voiceCount");
				lb.voiceCount = vc ? clamp((int) json_integer_value(vc), 1, 32) : 32;
				banks.push_back(lb);
			}
		} else if (json_t* legacy = json_object_get(root, "bank")) {
			// Legacy milestone-1 format: a single bank under "bank"/"cartName".
			std::vector<uint8_t> raw = b64decode(json_string_value(legacy));
			if (raw.size() >= 4096) {
				banks.clear();
				LoadedBank lb;
				json_t* nm = json_object_get(root, "cartName");
				lb.name = nm ? json_string_value(nm) : "Bank";
				std::memcpy(lb.data, raw.data(), 4096);
				banks.push_back(lb);
			}
		}
		json_t* cb = json_object_get(root, "curBank");
		curBank = cb ? clamp((int) json_integer_value(cb), 0, (int) banks.size() - 1) : 0;
		if (!banks.empty()) engine.setBankRaw(banks[curBank].data);
		json_t* ops = json_object_get(root, "opEnabled");
		if (ops && json_array_size(ops) >= 6)
			for (int i = 0; i < 6; i++) opEnabled[i] = json_boolean_value(json_array_get(ops, i));
		if (json_t* st = json_object_get(root, "screenTab"))
			screenTab = clamp((int) json_integer_value(st), 0, 1);
		if (json_t* ol = json_object_get(root, "outLevelDb"))
			outLevelDb = clamp((float) json_number_value(ol), -24.f, 12.f);
		if (json_t* j = json_object_get(root, "bankCvMode"))  bankCvMode  = clamp((int) json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(root, "voiceCvMode")) voiceCvMode = clamp((int) json_integer_value(j), 0, 2);
		engine.getVoiceName(dispName);
	}
};

// --- Display: SFS style, rendered in the design's native 176x157 space (see
//     Design Files/DX interface.svg) scaled to the widget. Bank strip, two
//     text lines, fixed 6x4 algorithm grid; operators are clickable to toggle. ---
// Tabbed screen: OPERATORS (algorithm grid) | ENVELOPE (two stacked plots).
// Design width 174 + tab geometry matched to Note so the top is pixel-identical;
// the screen is made tall enough that the operator grid keeps its size.
static const float DSGN_W = 174.f, DSGN_H = 159.f;
static inline float cellGX(int c) { return 12.567f + 26.f * c; }   // 6x4 grid
static inline float cellGY(int r) { return 56.f + 26.f * r; }
static inline float tabX(int i) { return 7.f + i * 80.f; }         // 2 tabs, w=78, gap=2
static const float TAB_Y = 8.f, TAB_W = 78.f, TAB_H = 18.f;        // matches Note's tabs
// Generic A/D shape for the browser thumbnail (module == NULL).
static inline float previewEnv(int i, int n) {
	float t = (n > 1) ? (float) i / (n - 1) : 0.f;
	if (t < 0.07f) return t / 0.07f;                       // fast attack
	float d = (t - 0.07f) / 0.93f;
	return 0.05f + 0.95f * std::exp(-3.2f * d);            // exp decay
}
// Bank strip spans the same width as the 6-column op grid (left of col0 to
// right of col5 = 160.567); 16 cells of width 8 evenly distributed.
static inline float bankX(int i) { return 12.567f + (160.567f - 12.567f - 8.f) / 15.f * i; }

struct BellDisplay : OpaqueWidget {
	Bell* module = nullptr;
	std::shared_ptr<Font> font;
	// transform from design space -> widget, cached for hit-testing.
	float tOx = 0, tOy = 0, tS = 1.f; int hitAlgo = 0;

	void drawScreen(NVGcontext* vg) {
		float w = box.size.x, h = box.size.y;
		float s = std::min(w / DSGN_W, h / DSGN_H);
		float ox = (w - DSGN_W * s) * 0.5f, oy = (h - DSGN_H * s) * 0.5f;
		tOx = ox; tOy = oy; tS = s;
		auto X = [&](float v) { return ox + s * v; };
		auto Y = [&](float v) { return oy + s * v; };
		auto roundRect = [&](float x, float y, float wd, float ht, float r) {
			nvgBeginPath(vg); nvgRoundedRect(vg, X(x), Y(y), s * wd, s * ht, s * r);
		};

		int algo = module ? module->dispAlgo : 0;
		int carrierMask = module ? module->dispCarrier : 0x28;  // algo 1 carriers in preview
		hitAlgo = algo;

		// --- tabs: OPERATORS | ENVELOPE (Beat/Note style) ---
		int tab = module ? module->screenTab : 0;
		if (font && font->handle >= 0) nvgFontFaceId(vg, font->handle);
		{
			const char* tl[2] = { "OPERATORS", "ENVELOPE" };
			for (int i = 0; i < 2; i++) {
				roundRect(tabX(i), TAB_Y, TAB_W, TAB_H, 2);
				nvgFillColor(vg, (tab == i) ? nvgRGB(0x0D, 0x59, 0x86) : nvgRGB(0x35, 0x35, 0x4D));
				nvgFill(vg);
				if (font && font->handle >= 0) {
					nvgFontSize(vg, s * 9.f);          // match Note's tab font height
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgFillColor(vg, (tab == i) ? nvgRGB(0xff, 0xff, 0xff) : nvgRGB(0x80, 0x80, 0x80));
					nvgText(vg, X(tabX(i) + TAB_W * 0.5f), Y(TAB_Y + TAB_H * 0.5f), tl[i], NULL);
				}
			}
			// Connector rail + active-tab stem (matches Note: rail y=32, stem 28→32.5).
			nvgStrokeColor(vg, nvgRGB(0x0D, 0x59, 0x88));
			nvgStrokeWidth(vg, std::max(1.f, s));
			nvgBeginPath(vg);
			nvgMoveTo(vg, X(7.f), Y(32.f)); nvgLineTo(vg, X(165.f), Y(32.f));
			nvgStroke(vg);
			float stemX = tabX(tab) + TAB_W * 0.5f;
			nvgBeginPath(vg);
			nvgMoveTo(vg, X(stemX), Y(28.f)); nvgLineTo(vg, X(stemX), Y(32.5f));
			nvgStroke(vg);
		}
		int nb = module ? (int) module->banks.size() : 1;
		int selB = module ? module->dispBank : 0;

		// --- two text lines ---
		if (font && font->handle >= 0) {
			nvgFontFaceId(vg, font->handle);
			const char* bankName = (module && selB >= 0 && selB < nb) ? module->banks[selB].name.c_str() : "BELL";
			const char* vname = module ? module->dispName : "E.PIANO 1";
			int vnum = module ? module->dispVoice : 0;
			nvgFontSize(vg, s * 9.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
			nvgText(vg, X(13.4f), Y(35.f), bankName, NULL);
			char num[8]; std::snprintf(num, sizeof(num), "%d ", vnum + 1);
			nvgFillColor(vg, nvgRGB(0xEC, 0x65, 0x2E));
			float nx = nvgText(vg, X(13.4f), Y(45.f), num, NULL);
			nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
			nvgText(vg, nx, Y(45.f), vname, NULL);
			// algorithm number, right-aligned on the voice row
			char alg[12]; std::snprintf(alg, sizeof(alg), "ALG %d", algo + 1);
			nvgFontSize(vg, s * 8.f);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgFillColor(vg, nvgRGBA(0xa0, 0xa8, 0xc0, 0xcc));
			nvgText(vg, X(160.567f), Y(46.f), alg, NULL);
		}

	  if (tab == 0) {
		// --- empty 6x4 grid ---
		for (int r = 0; r < 4; r++) for (int c = 0; c < 6; c++) {
			roundRect(cellGX(c), cellGY(r), 18, 18, 2);
			nvgFillColor(vg, nvgRGB(0x35, 0x35, 0x4D)); nvgFill(vg);
		}

		const AlgoOp* a = kAlgo[clamp(algo, 0, 31)];
		auto on = [&](int id) { return module ? module->opEnabled[6 - id] : true; };
		auto carrier = [&](int id) { return ((carrierMask >> (6 - id)) & 1) != 0; };

		// --- connections (drawn under the cells) ---
		// Connected runs are stroked as single polylines with miter joins +
		// square caps so corners meet crisply (no gaps).
		nvgStrokeWidth(vg, std::max(0.8f, s * 1.2f));
		nvgLineJoin(vg, NVG_MITER);
		nvgLineCap(vg, NVG_SQUARE);
		auto poly = [&](std::initializer_list<Vec> pts) {
			nvgBeginPath(vg); bool first = true;
			for (const Vec& p : pts) {
				if (first) { nvgMoveTo(vg, X(p.x), Y(p.y)); first = false; }
				else nvgLineTo(vg, X(p.x), Y(p.y));
			}
			nvgStroke(vg);
		};
		for (int i = 0; i < 6; i++) {
			int id = a[i].id, lk = a[i].link, fb = a[i].fb;
			float gx = cellGX(a[i].x), gy = cellGY(a[i].y), cx = gx + 9;
			float cellBot = gy + 18, nextTop = gy + 26, gapMid = gy + 22, nextCx = cx + 26, prevCx = cx - 26;
			nvgStrokeColor(vg, on(id) ? nvgRGB(0x00, 0x97, 0xDE) : nvgRGBA(0x00, 0x97, 0xDE, 0x40));
			if (!carrier(id)) {
				switch (lk) {
					case 0: case 2: poly({Vec(cx,cellBot), Vec(cx,nextTop)}); break;
					case 1: poly({Vec(cx,cellBot), Vec(cx,gapMid), Vec(nextCx,gapMid), Vec(nextCx,nextTop)}); break;
					case 3: poly({Vec(cx,cellBot), Vec(cx,nextTop)});
					        poly({Vec(cx,gapMid), Vec(nextCx,gapMid), Vec(nextCx,nextTop)}); break;
					case 4: poly({Vec(cx,cellBot), Vec(cx,nextTop)});
					        poly({Vec(prevCx,nextTop), Vec(prevCx,gapMid), Vec(nextCx,gapMid), Vec(nextCx,nextTop)}); break;
					case 6: poly({Vec(cx,cellBot), Vec(cx,gapMid), Vec(cx+52,gapMid), Vec(cx+52,nextTop)}); break;
					case 7: poly({Vec(cx,cellBot), Vec(cx,gapMid), Vec(prevCx,gapMid), Vec(prevCx,nextTop)}); break;
				}
			}
			if (fb)
				poly({Vec(cx,gy+22), Vec(cx+14,gy+22), Vec(cx+14,gy-5), Vec(cx,gy-5), Vec(cx,gy+0.5f)});
		}
		// output bus for carriers (drawn in carrier orange)
		{
			float minCx = 1e9f, maxCx = -1e9f, busY = -1e9f; int n = 0;
			float sx[6], sy[6]; bool son[6];
			for (int i = 0; i < 6; i++) {
				if (!carrier(a[i].id)) continue;
				float cx = cellGX(a[i].x) + 9, bot = cellGY(a[i].y) + 18;
				sx[n] = cx; sy[n] = bot; son[n] = on(a[i].id); n++;
				minCx = std::min(minCx, cx); maxCx = std::max(maxCx, cx); busY = std::max(busY, bot);
			}
			busY += 4.5f;
			for (int i = 0; i < n; i++) {
				nvgStrokeColor(vg, son[i] ? nvgRGB(0xEC, 0x65, 0x2E) : nvgRGBA(0xEC, 0x65, 0x2E, 0x40));
				poly({Vec(sx[i], sy[i]), Vec(sx[i], busY)});
			}
			if (n > 1) { nvgStrokeColor(vg, nvgRGB(0xEC, 0x65, 0x2E)); poly({Vec(minCx,busY), Vec(maxCx,busY)}); }
		}

		// --- operator cells (on top) ---
		for (int i = 0; i < 6; i++) {
			int id = a[i].id;
			float gx = cellGX(a[i].x), gy = cellGY(a[i].y);
			bool o = on(id), car = carrier(id);
			NVGcolor fill = o ? (car ? nvgRGB(0xEC, 0x65, 0x2E) : nvgRGB(0x33, 0xAC, 0xE5))
			                  : (car ? nvgRGB(0x4A, 0x4A, 0x66) : nvgRGB(0x35, 0x35, 0x4D));
			roundRect(gx, gy, 18, 18, 2); nvgFillColor(vg, fill); nvgFill(vg);
			// Outline: modulators get a blue outline (dimmed when off). Carriers
			// have NO blue outline (orange fill stands alone); an off carrier
			// keeps a dim orange outline so the box still reads. The 18x18 fill
			// keeps the box size identical either way.
			bool drawStroke = true; NVGcolor stk = nvgRGB(0x00, 0x97, 0xDE);
			if (car) {
				if (o) drawStroke = false;
				else stk = nvgRGBA(0xEC, 0x65, 0x2E, 0x55);
			} else {
				stk = o ? nvgRGB(0x00, 0x97, 0xDE) : nvgRGBA(0x00, 0x97, 0xDE, 0x40);
			}
			if (drawStroke) {
				roundRect(gx + 0.5f, gy + 0.5f, 17, 17, 1.5f);
				nvgStrokeColor(vg, stk);
				nvgStrokeWidth(vg, std::max(0.7f, s)); nvgStroke(vg);
			}
			if (font && font->handle >= 0) {
				nvgFontSize(vg, s * 11.f);
				nvgFillColor(vg, o ? nvgRGB(0xff, 0xff, 0xff) : nvgRGBA(0xff, 0xff, 0xff, 0x88));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				char nm[2] = {(char)('0' + id), 0};
				nvgText(vg, X(gx + 9), Y(gy + 9), nm, NULL);
			}
		}

	  } else {
		// --- envelope tab: two separate (non-overlapping) plots ---
		//   top: static carrier EG (blue)   bottom: live output follower (orange)
		const float ex0 = 12.567f, ex1 = 160.567f;
		const float t1 = 57.f, b1 = 104.f;    // EG plot (top half)
		const float t2 = 110.f, b2 = 156.f;   // live follower (bottom half)
		auto baseline = [&](float yB) {
			nvgBeginPath(vg); nvgMoveTo(vg, X(ex0), Y(yB)); nvgLineTo(vg, X(ex1), Y(yB));
			nvgStrokeColor(vg, nvgRGB(0x35, 0x35, 0x4D));
			nvgStrokeWidth(vg, std::max(0.6f, s)); nvgStroke(vg);
		};
		baseline(b1); baseline(b2);

		// static carrier EG (blue)
		const int N = Bell::ENV_N;
		bool haveEnv = module && module->envValid;
		nvgBeginPath(vg);
		for (int i = 0; i < N; i++) {
			float v = clamp(haveEnv ? module->envCurve[i] : previewEnv(i, N), 0.f, 1.f);
			float x = X(ex0 + (ex1 - ex0) * (float) i / (N - 1));
			float y = Y(b1 - v * (b1 - t1));
			if (i == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
		}
		nvgStrokeColor(vg, nvgRGB(0x00, 0x97, 0xDE));
		nvgStrokeWidth(vg, std::max(0.8f, s)); nvgStroke(vg);

		// live amplitude follower (orange; oldest sample at scopeHead)
		if (module) {
			const int M = Bell::SCOPE_N;
			nvgBeginPath(vg);
			for (int j = 0; j < M; j++) {
				int idx = (module->scopeHead + j) % M;
				float v = clamp(module->scope[idx], 0.f, 1.f);
				float x = X(ex0 + (ex1 - ex0) * (float) j / (M - 1));
				float y = Y(b2 - v * (b2 - t2));
				if (j == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
			}
			nvgStrokeColor(vg, nvgRGB(0xEC, 0x65, 0x2E));
			nvgStrokeWidth(vg, std::max(0.8f, s * 1.2f)); nvgStroke(vg);
		}
	  }
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 4.f);
		nvgFillColor(vg, nvgRGB(0x1A, 0x1A, 0x2E)); nvgFill(vg);
		nvgStrokeColor(vg, nvgRGB(0x40, 0x40, 0x60)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		nvgIntersectScissor(vg, 1, 1, w - 2, h - 2);
		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		drawScreen(vg);
		OpaqueWidget::drawLayer(args, layer);
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module && tS > 0.f) {
			float dx = (e.pos.x - tOx) / tS, dy = (e.pos.y - tOy) / tS;
			// Tab switch.
			if (dy >= TAB_Y && dy <= TAB_Y + TAB_H) {
				for (int i = 0; i < 2; i++) {
					float tx = tabX(i);
					if (dx >= tx && dx <= tx + TAB_W) { module->screenTab = i; e.consume(this); return; }
				}
			}
			// Operator on/off (only on the Operators tab).
			if (module->screenTab == 0) {
				const AlgoOp* a = kAlgo[clamp(hitAlgo, 0, 31)];
				for (int i = 0; i < 6; i++) {
					float gx = cellGX(a[i].x), gy = cellGY(a[i].y);
					if (dx >= gx && dx <= gx + 18 && dy >= gy && dy <= gy + 18) { module->toggleOp(a[i].id); e.consume(this); return; }
				}
			}
		}
		OpaqueWidget::onButton(e);
	}
};

struct BellWidget : ModuleWidget {
	BellWidget(Bell* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/operator.svg")));

		// No virtual screws — see CLAUDE.md (SFS panels omit them by design).

		// Positions synced to res/operator.svg reticules (viewBox units / 2.83465).
		BellDisplay* disp = new BellDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(2.4f, 12.f));
		disp->box.size = mm2px(Vec(46.f, 42.03f));
		addChild(disp);

		// Voice row: BANK | PREV | VOICE | NEXT, with BANK / VOICE CV below.
		addParam(createParamCentered<Trimpot>(mm2px(Vec(7.62f, 64.14f)), module, Bell::BANK_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(22.01f, 64.14f)), module, Bell::VOICE_PREV_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(29.63f, 64.14f)), module, Bell::VOICE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(37.25f, 64.14f)), module, Bell::VOICE_NEXT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.62f, 77.69f)), module, Bell::BANK_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.63f, 77.69f)), module, Bell::VOICE_CV_INPUT));

		// Timbre: FEEDBACK | TUNE | BRIGHTNESS, with CV below.
		addParam(createParamCentered<Trimpot>(mm2px(Vec(7.62f, 91.23f)), module, Bell::FEEDBACK_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(18.63f, 91.23f)), module, Bell::TUNE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(29.63f, 91.23f)), module, Bell::BRIGHTNESS_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.62f, 106.47f)), module, Bell::FEEDBACK_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.63f, 106.47f)), module, Bell::TUNE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.63f, 106.47f)), module, Bell::BRIGHTNESS_CV_INPUT));

		// I/O row: V/OCT | GATE | VEL.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.62f, 121.71f)), module, Bell::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.63f, 121.71f)), module, Bell::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.63f, 121.71f)), module, Bell::VEL_INPUT));

		// Output column: ENV | VCO | AUDIO.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(43.18f, 91.23f)), module, Bell::ENV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(43.18f, 106.47f)), module, Bell::VCO_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(43.18f, 121.71f)), module, Bell::AUDIO_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Bell* m = dynamic_cast<Bell*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Algorithm morph"));
		// The routing of two algorithms, blended. At 0 the voice runs the stock
		// engine path and is bit-identical to before; part-way, an operator can
		// modulate the one below it AND sound as its own carrier, which is a
		// structure no DX7 algorithm has a number for.
		//
		// Here rather than on the panel because Operator is 10HP and full, and
		// res/operator.svg is a designed face -- two knobs and two CV jacks is a
		// panel decision, not one to squeeze in from the code side. The params
		// and their CV inputs exist, so the jacks are a placement away.
		{
			struct MorphSlider : ui::Slider {
				MorphSlider(Quantity* q) { quantity = q; box.size.x = 200.f; }
			};
			menu->addChild(new MorphSlider(m->getParamQuantity(Bell::MORPH_PARAM)));
			std::vector<std::string> algs;
			for (int i = 0; i < 32; i++) algs.push_back(string::f("Algorithm %d", i + 1));
			menu->addChild(createIndexSubmenuItem("Morph toward", algs,
				[=]() { return (int) std::round(m->params[Bell::ALGOB_PARAM].getValue()); },
				[=](int i) { m->params[Bell::ALGOB_PARAM].setValue((float) i); }));
			menu->addChild(createMenuLabel(
				string::f("   this patch is algorithm %d", m->dispAlgo + 1)));
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Banks"));
		menu->addChild(createMenuItem("Load bank (.syx)…", "", [m]() {
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL,
				osdialog_filters_parse(BELL_SYX_FILTERS));
			if (path) { m->addBank(path); std::free(path); }
		}));
		// Loaded banks (click to select, with a Remove sub-action).
		for (int i = 0; i < (int) m->banks.size(); i++) {
			std::string label = string::f("%d: %s", i + 1, m->banks[i].name.c_str());
			menu->addChild(createCheckMenuItem(label, "",
				[m, i]() { return m->curBank == i; },
				[m, i]() { m->params[Bell::BANK_PARAM].setValue((float) i); }));
		}
		if (m->banks.size() > 1) {
			menu->addChild(createMenuItem("Remove current bank", "",
				[m]() { m->removeBank(m->curBank); }));
		}

		menu->addChild(new MenuSeparator);
		static const float LVL[] = {-24.f, -18.f, -12.f, -9.f, -6.f, -3.f, 0.f, 3.f, 6.f, 9.f, 12.f};
		static const int NLVL = (int)(sizeof(LVL) / sizeof(LVL[0]));
		std::vector<std::string> lvlLabels;
		for (int i = 0; i < NLVL; i++) lvlLabels.push_back(string::f("%+g dB", LVL[i]));
		menu->addChild(createIndexSubmenuItem("Output level", lvlLabels,
			[m]() { int c = 6; for (int i = 0; i < NLVL; i++) if (std::fabs(LVL[i] - m->outLevelDb) < 0.01f) c = i; return c; },
			[m](int i) { m->outLevelDb = LVL[i]; }));

		// CV input scaling — lets a sequencer address banks/voices by low voltage or by
		// note. e.g. Notes mode: C4 = first, C#4 = second… (play drum kits with one Operator).
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Bank CV",
			{"0–10V (full range)", "0–1.6V (0.1V / bank)", "Notes (1V/oct, C4 = 1st)"},
			[m]() { return m->bankCvMode; }, [m](int i) { m->bankCvMode = i; }));
		menu->addChild(createIndexSubmenuItem("Voice CV",
			{"0–10V (full range)", "0–3.2V (0.1V / voice)", "Notes (1V/oct, C4 = 1st)"},
			[m]() { return m->voiceCvMode; }, [m](int i) { m->voiceCvMode = i; }));
	}
};

Model* modelOperator = createModel<Bell, BellWidget>("Operator");
