#include "plugin.hpp"
#include "dr_wav.h"        // implementation lives in phase.cpp; headers only here
#include "pushgrid.hpp"    // shared Push-style pad grid (GRID_COLS, gridNoteAt, …)
#include <osdialog.h>
#include <vector>
#include <string>
#include <cmath>

// ─── Record — multisample auto-sampler ───────────────────────────────────────
// Drives an external voice with V/OCT + GATE + VELOCITY and captures its stereo
// output across a note range and velocity layers, then writes WAVs + an .sfz.
// Audition plays the sweep without recording. Waits for silence between notes so
// one note's release never bleeds into the next.

static const int REC_SPACINGS[6] = {1, 2, 3, 4, 6, 12};
static const int REC_MAX_CAPS = 256;
static const int KB_LO = 21, KB_HI = 108;      // A0 … C8 (88 keys)
static const float SILENCE_THRESH = 0.002f;    // ≈ −54 dBFS
static const char* REC_NOTE_NAMES[12] =
	{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static inline bool recIsWhite(int n) {
	int s = ((n % 12) + 12) % 12;
	return s == 0 || s == 2 || s == 4 || s == 5 || s == 7 || s == 9 || s == 11;
}
static inline int recWhiteBefore(int n) {       // white keys in [KB_LO, n)
	int c = 0; for (int m = KB_LO; m < n; m++) if (recIsWhite(m)) c++; return c;
}

// Show the Start-note value as a note name, e.g. "C2 (36)".
struct NoteParamQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int m = clamp((int)std::round(getValue()), 0, 127);
		return string::f("%s%d (%d)", REC_NOTE_NAMES[((m % 12) + 12) % 12], m / 12 - 1, m);
	}
};

struct RecCap {
	int  midi = 60, lokey = 0, hikey = 127, lovel = 1, hivel = 127, repVel = 100, rr = 0;
	std::vector<float> data;   // interleaved stereo, ±1
	size_t frames = 0, trimStart = 0, trimEnd = 0;
	long loopStart = -1, loopEnd = -1;   // in written-WAV frames (0-based)
};

struct Record;

struct RecordDisplay : OpaqueWidget {
	Record* module = nullptr;
	std::shared_ptr<Font> font;
	bool dragging = false;
	Vec dragPos;

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawScope(NVGcontext* vg, float x, float y, float w, float h,
	               const float* mn, const float* mx, int n);
	void drawKeys(NVGcontext* vg, float x, float y, float w, float h,
	              const uint8_t* sampled, int cur, int rlo, int rhi);
	void drawTabs(NVGcontext* vg, int view);
	void drawGrid(NVGcontext* vg, const uint8_t* sampled, int cur, int rlo, int rhi,
	              int layout, int gRoot, int gScale, int gBase);

	// Geometry (px, widget-local) — shared by draw + hit-test
	void tabRects(Rect out[2]) const {
		float tw = (box.size.x - 12.f - 4.f) * 0.5f;
		out[0] = Rect(Vec(6.f, 4.f), Vec(tw, 15.f));
		out[1] = Rect(Vec(6.f + tw + 4.f, 4.f), Vec(tw, 15.f));
	}
	void gridGeom(float& x0, float& y0, float& pad) const {
		float top = 40.f, marg = 6.f;   // below tabs + name/status + progress
		float aw = box.size.x - 2.f * marg, ah = box.size.y - 6.f - top;
		pad = std::min(aw / GRID_COLS, ah / GRID_ROWS);
		x0 = (box.size.x - pad * GRID_COLS) * 0.5f;
		y0 = top + std::max(0.f, (ah - pad * GRID_ROWS) * 0.5f);
	}
	int hitTab(Vec p) const { Rect r[2]; tabRects(r); for (int i = 0; i < 2; i++) if (r[i].contains(p)) return i; return -1; }
	int hitPad(Vec p) const;
	int pianoNoteAt(Vec p) const;

	void onButton(const ButtonEvent& e) override;
	void onDragStart(const DragStartEvent& e) override { OpaqueWidget::onDragStart(e); }
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
};

struct Record : Module {
	enum ParamId { START_PARAM, OCTAVES_PARAM, SPACING_PARAM, VELLAYERS_PARAM,
	               SUSTAIN_PARAM, TAIL_PARAM, RECORD_PARAM, AUDITION_PARAM, PARAMS_LEN };
	enum InputId { L_INPUT, R_INPUT, INPUTS_LEN };
	enum OutputId { VOCT_OUTPUT, GATE_OUTPUT, VEL_OUTPUT, DONE_OUTPUT, OUTPUTS_LEN };
	enum LightId { RECORD_LIGHT, AUDITION_LIGHT, LIGHTS_LEN };
	enum State { IDLE, RECORDING, AUDITIONING, WRITE_PENDING, WRITING, DONE_ST, ERR, CALIBRATING };

	static const int SCOPE_N = 256;

	State state = IDLE;
	std::vector<RecCap> caps;
	int   capIdx = 0, phase = 0, phaseSamp = 0;   // phase 0 sustain, 1 tail, 2 wait-silence
	int   sustainSamps = 0, tailSamps = 0, silenceSamps = 0, maxWaitSamps = 0, minGapSamps = 0, silenceCount = 0;
	float sr = 48000.f;

	// options (persisted)
	std::string destDir, instrument = "Instrument", lastOut;
	bool  normalize = true, autoTrim = true, loopInstrument = false, triggerMode = false, waitSilence = true;
	int   gapMs = 250;            // minimum gap between notes
	int   channels = 2, bitDepth = 24;   // file options: 1/2 ch, 16/24/32-bit
	int   rrCount = 1;            // round-robin takes per note+velocity (1..4)
	int   latencySamps = 0;       // measured gate→audio round-trip (persisted)
	int   lastCount = 0;

	// latency calibration
	std::vector<float> calBuf; int calSamp = 0, calDur = 0;

	// runtime
	dsp::SchmittTrigger recTrig, audTrig;
	dsp::PulseGenerator donePulse, gatePulse;
	bool  noteStart = false;
	int   startReq = 0;           // consumed by the widget (folder dialog on GUI thread)

	// Keyboard surface view (tab-switched on the display) + Push grid config
	int kbView = 1;        // 0 = piano, 1 = grid (grid is the default)
	int gridLayout = 0;    // 0 = chromatic 4ths, 1 = in-key, 2 = chromatic grid (C0)
	int gridRoot = 0;      // 0..11 (C..B)
	int gridScale = 1;     // sfs::SCALES index (default Major)
	int gridBase = 36;     // MIDI note of the bottom-left pad (C2)
	int uiNote = -1;       // mouse audition (widget writes; process emits V/OCT+GATE)

	// display mirrors
	char  dispName[48] = {0}, dispStatus[48] = {0}, dispVel[16] = {0};
	int   kbLo = 36, kbHi = 84, kbCur = -1;
	uint8_t kbSampled[128] = {0};
	float dispProg = 0.f;
	float scopeMin[SCOPE_N] = {0}, scopeMax[SCOPE_N] = {0};
	int   scopePos = 0, scopeAcc = 0, scopeD = 1; float binMin = 1.f, binMax = -1.f;
	int   statusDiv = 0;

	void resetScope() {
		std::memset(scopeMin, 0, sizeof(scopeMin)); std::memset(scopeMax, 0, sizeof(scopeMax));
		scopePos = 0; scopeAcc = 0; binMin = 1.f; binMax = -1.f;
	}

	Record() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<NoteParamQuantity>(START_PARAM, 0.f, 127.f, 36.f, "Start note");
		getParamQuantity(START_PARAM)->snapEnabled = true;
		configParam(OCTAVES_PARAM, 1.f, 8.f, 4.f, "Octaves");
		getParamQuantity(OCTAVES_PARAM)->snapEnabled = true;
		configSwitch(SPACING_PARAM, 0.f, 5.f, 2.f, "Sample spacing",
			{"Every semitone","Every 2","Every 3","Every 4","Every 6","Every octave"});
		configParam(VELLAYERS_PARAM, 1.f, 4.f, 1.f, "Velocity layers");
		getParamQuantity(VELLAYERS_PARAM)->snapEnabled = true;
		configParam(SUSTAIN_PARAM, 0.1f, 10.f, 2.f, "Sustain (gate held)", " s");
		configParam(TAIL_PARAM, 0.f, 10.f, 1.f, "Release tail", " s");
		configButton(RECORD_PARAM, "Record");
		configButton(AUDITION_PARAM, "Audition (play, no capture)");
		configInput(L_INPUT, "Audio L");
		configInput(R_INPUT, "Audio R");
		configOutput(VOCT_OUTPUT, "V/oct");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(VEL_OUTPUT, "Velocity");
		configOutput(DONE_OUTPUT, "Done trigger");
		refreshStatus();
	}

	void buildPlan() {
		caps.clear();
		int start = clamp((int)std::round(params[START_PARAM].getValue()), 0, 127);
		int octs  = clamp((int)std::round(params[OCTAVES_PARAM].getValue()), 1, 8);
		int sp    = REC_SPACINGS[clamp((int)std::round(params[SPACING_PARAM].getValue()), 0, 5)];
		int L     = clamp((int)std::round(params[VELLAYERS_PARAM].getValue()), 1, 4);
		int end   = clamp(start + octs * 12, 0, 127);
		std::vector<int> roots;
		for (int n = start; n <= end; n += sp) roots.push_back(n);
		int lovel[4], hivel[4], rep[4];
		for (int j = 0; j < L; j++) {
			hivel[j] = (j == L - 1) ? 127 : (int)std::round(127.0 * (j + 1) / L);
			lovel[j] = (j == 0) ? 1 : hivel[j - 1] + 1;
			rep[j]   = clamp((lovel[j] + hivel[j]) / 2, 1, 127);
		}
		int R = clamp(rrCount, 1, 4);
		for (size_t i = 0; i < roots.size(); i++) {
			int lo = (i == 0) ? 0 : (roots[i - 1] + roots[i]) / 2 + 1;
			int hi = (i == roots.size() - 1) ? 127 : (roots[i] + roots[i + 1]) / 2;
			for (int j = 0; j < L; j++)
				for (int rr = 0; rr < R; rr++) {   // rr innermost: capture takes back-to-back
					if ((int)caps.size() >= REC_MAX_CAPS) return;
					RecCap c; c.midi = roots[i]; c.lokey = lo; c.hikey = hi;
					c.lovel = lovel[j]; c.hivel = hivel[j]; c.repVel = rep[j]; c.rr = rr;
					caps.push_back(std::move(c));
				}
		}
	}

	void startPlan(bool capture) {
		buildPlan();
		if (caps.empty()) { state = ERR; refreshStatus(); return; }
		capIdx = 0; phase = 0; phaseSamp = 0; noteStart = true; silenceCount = 0;
		sustainSamps = (int)(clamp(params[SUSTAIN_PARAM].getValue(), 0.1f, 10.f) * sr);
		tailSamps    = (int)(clamp(params[TAIL_PARAM].getValue(), 0.f, 10.f) * sr);
		silenceSamps = (int)(0.050f * sr);       // require 50 ms of true silence
		minGapSamps  = (int)(clamp(gapMs, 0, 5000) / 1000.f * sr);   // …and at least this gap
		maxWaitSamps = (int)(4.0f * sr);         // never hang more than 4 s
		long span = (long)sustainSamps + tailSamps + minGapSamps + silenceSamps + (long)(0.1f * sr);
		scopeD = std::max(1, (int)(span / SCOPE_N));   // one full note+gap fills the scope
		resetScope();
		if (capture) { caps[0].data.reserve((size_t)(sustainSamps + tailSamps) * 2 + 64); state = RECORDING; }
		else state = AUDITIONING;
	}

	void advanceNote() {
		capIdx++; phase = 0; phaseSamp = 0; noteStart = true; silenceCount = 0;
		if (state == RECORDING) caps[capIdx].data.reserve((size_t)(sustainSamps + tailSamps) * 2 + 64);
	}

	// Fire a test note and measure how long until the sound arrives back.
	void startCalibrate() {
		calDur = (int)(0.6f * sr);
		calBuf.assign(calDur, 0.f);
		calSamp = 0;
		state = CALIBRATING;
		snprintf(dispStatus, sizeof(dispStatus), "calibrating...");
	}
	void doCalibrate() {
		outputs[VOCT_OUTPUT].setVoltage(0.f);        // C4
		outputs[GATE_OUTPUT].setVoltage(10.f);
		outputs[VEL_OUTPUT].setVoltage(100.f / 127.f * 10.f);
		float lv = inputs[L_INPUT].getVoltage() / 10.f;
		float rv = inputs[R_INPUT].isConnected() ? inputs[R_INPUT].getVoltage() / 10.f : lv;
		if (calSamp < calDur) calBuf[calSamp] = std::max(std::fabs(lv), std::fabs(rv));
		calSamp++;
		if (calSamp >= calDur) {
			int onset = 0;
			for (int i = 0; i < calDur; i++) if (calBuf[i] > 0.01f) { onset = i; break; }
			latencySamps = onset;
			state = IDLE; refreshStatus();
		}
	}

	void process(const ProcessArgs& args) override {
		sr = args.sampleRate;
		if (state == CALIBRATING) {
			doCalibrate();
			outputs[DONE_OUTPUT].setVoltage(0.f);
			return;
		}
		if (recTrig.process(params[RECORD_PARAM].getValue())) {
			if (state == RECORDING) state = IDLE;
			else if (state != WRITE_PENDING && state != WRITING) startReq = 1;
		}
		if (audTrig.process(params[AUDITION_PARAM].getValue())) {
			if (state == AUDITIONING) state = IDLE;
			else if (state != WRITE_PENDING && state != WRITING) startReq = 2;
		}

		float voct = 0.f, gate = 0.f, vel = 0.f;
		bool advancing = (state == RECORDING || state == AUDITIONING);
		bool gp = gatePulse.process(args.sampleTime);
		if (advancing && capIdx < (int)caps.size()) {
			RecCap& c = caps[capIdx];
			voct = (c.midi - 60) / 12.f;
			vel  = c.repVel / 127.f * 10.f;
			kbCur = c.midi;
			if (noteStart) { gatePulse.trigger(0.001f); noteStart = false; resetScope(); }

			float lv = clamp(inputs[L_INPUT].getVoltage() / 10.f, -1.f, 1.f);
			float rv = clamp((inputs[R_INPUT].isConnected() ? inputs[R_INPUT].getVoltage()
				: inputs[L_INPUT].getVoltage()) / 10.f, -1.f, 1.f);
			float amp = std::max(std::fabs(lv), std::fabs(rv));

			// scope: fill left→right once across this note's whole window (note + gap)
			binMin = std::min(binMin, lv); binMax = std::max(binMax, lv);
			if (++scopeAcc >= scopeD) {
				if (scopePos < SCOPE_N) { scopeMin[scopePos] = binMin; scopeMax[scopePos] = binMax; scopePos++; }
				binMin = 1.f; binMax = -1.f; scopeAcc = 0;
			}

			if (phase <= 1 && state == RECORDING) { c.data.push_back(lv); c.data.push_back(rv); c.frames++; }
			phaseSamp++;
			if (phase == 0 && phaseSamp >= sustainSamps) { phase = 1; phaseSamp = 0; }
			else if (phase == 1 && phaseSamp >= tailSamps) {
				bool last = (capIdx == (int)caps.size() - 1);
				if (last) { state = (state == RECORDING) ? WRITE_PENDING : IDLE; kbCur = -1; }
				else if (waitSilence) { phase = 2; phaseSamp = 0; silenceCount = 0; }
				else advanceNote();
			}
			else if (phase == 2) {
				if (amp < SILENCE_THRESH) silenceCount++; else silenceCount = 0;
				if ((phaseSamp >= minGapSamps && silenceCount >= silenceSamps) || phaseSamp >= maxWaitSamps)
					advanceNote();
			}
			gate = triggerMode ? (gp ? 10.f : 0.f) : (phase == 0 ? 10.f : 0.f);
			// progress includes how far through the current note we are, and
			// reaches 1.0 as the final note's tail completes.
			float noteEl = (phase == 0) ? (float)phaseSamp
			             : (phase == 1) ? (float)(sustainSamps + phaseSamp)
			             : (float)(sustainSamps + tailSamps);
			float noteFrac = clamp(noteEl / (float)std::max(1, sustainSamps + tailSamps), 0.f, 1.f);
			dispProg = clamp((capIdx + noteFrac) / (float)caps.size(), 0.f, 1.f);
		} else kbCur = -1;

		// Mouse audition from the on-screen grid / keyboard: emit V/OCT + GATE to
		// play the external voice, but only when the sampler isn't busy.
		if (uiNote >= 0 && (state == IDLE || state == DONE_ST || state == ERR)) {
			voct = (uiNote - 60) / 12.f;
			gate = 10.f;
			vel  = 100.f / 127.f * 10.f;
			kbCur = uiNote;
		}

		outputs[VOCT_OUTPUT].setVoltage(voct);
		outputs[GATE_OUTPUT].setVoltage(gate);
		outputs[VEL_OUTPUT].setVoltage(vel);
		outputs[DONE_OUTPUT].setVoltage(donePulse.process(args.sampleTime) ? 10.f : 0.f);
		lights[RECORD_LIGHT].setBrightness(state == RECORDING ? 1.f : (state == WRITING ? 0.5f : 0.f));
		lights[AUDITION_LIGHT].setBrightness(state == AUDITIONING ? 1.f : 0.f);
		if (++statusDiv >= 512) { statusDiv = 0; refreshStatus(); }
	}

	void refreshStatus() {
		int start = clamp((int)std::round(params[START_PARAM].getValue()), 0, 127);
		int octs  = clamp((int)std::round(params[OCTAVES_PARAM].getValue()), 1, 8);
		int sp    = REC_SPACINGS[clamp((int)std::round(params[SPACING_PARAM].getValue()), 0, 5)];
		int L     = clamp((int)std::round(params[VELLAYERS_PARAM].getValue()), 1, 4);
		kbLo = start; kbHi = clamp(start + octs * 12, 0, 127);
		std::memset(kbSampled, 0, sizeof(kbSampled));
		for (int n = start; n <= kbHi; n += sp) if (n >= 0 && n < 128) kbSampled[n] = 1;
		snprintf(dispName, sizeof(dispName), "%s", instrument.c_str());
		if (rrCount > 1) snprintf(dispVel, sizeof(dispVel), "x%dv x%dr", L, rrCount);
		else snprintf(dispVel, sizeof(dispVel), "x%d vel", L);
		switch (state) {
			case CALIBRATING:   snprintf(dispStatus, sizeof(dispStatus), "calibrating..."); break;
			case IDLE:          snprintf(dispStatus, sizeof(dispStatus), "%s", destDir.empty() ? "set folder" : "ready"); break;
			case RECORDING:     snprintf(dispStatus, sizeof(dispStatus), phase == 2 ? "REC %d/%d — wait" : "REC %d/%d", capIdx + 1, (int)caps.size()); break;
			case AUDITIONING:   snprintf(dispStatus, sizeof(dispStatus), "audition %d/%d", capIdx + 1, (int)caps.size()); break;
			case WRITE_PENDING:
			case WRITING:       snprintf(dispStatus, sizeof(dispStatus), "writing..."); break;
			case DONE_ST:       snprintf(dispStatus, sizeof(dispStatus), "done \xe2\x9c\x93 %d files", lastCount); break;
			case ERR:           snprintf(dispStatus, sizeof(dispStatus), "no notes"); break;
		}
	}

	void analyze(RecCap& c) {
		const float th = 0.0008f;
		size_t n = c.frames, s = 0, e = n;
		size_t pre = (size_t)(0.002f * sr);
		if (latencySamps > 0) {                  // calibrated: start at the measured onset
			s = ((size_t)latencySamps > pre) ? latencySamps - pre : 0;
			if (s >= n) s = 0;
		} else if (autoTrim) {                   // else threshold onset
			for (size_t i = 0; i < n; i++)
				if (std::fabs(c.data[i * 2]) > th || std::fabs(c.data[i * 2 + 1]) > th) { s = i; break; }
			s = (s > pre) ? s - pre : 0;
		}
		if (autoTrim) {
			for (size_t i = n; i > s; i--)
				if (std::fabs(c.data[(i - 1) * 2]) > th || std::fabs(c.data[(i - 1) * 2 + 1]) > th) { e = i; break; }
		}
		c.trimStart = s; c.trimEnd = (e > s) ? e : s + 1;
	}

	// Find a seamless sustain loop: both ends on positive-going zero crossings,
	// end chosen to best match the waveform just after the start (min seam error).
	void findLoop(RecCap& c) {
		long L = (long)(c.trimEnd - c.trimStart);
		c.loopStart = 0; c.loopEnd = L;
		if (L < 4000) return;
		const float* d = &c.data[c.trimStart * 2];
		auto mono = [&](long i) { return d[i * 2]; };      // L channel
		long a = L * 40 / 100;
		for (long i = L * 38 / 100; i < L * 55 / 100; i++) if (mono(i - 1) <= 0.f && mono(i) > 0.f) { a = i; break; }
		const long W = 256;
		long bestB = -1; double bestErr = 1e30;
		for (long b = L * 68 / 100; b < L - W - 1 && b < L * 93 / 100; b++) {
			if (!(mono(b - 1) <= 0.f && mono(b) > 0.f)) continue;
			double err = 0;
			for (long k = 0; k < W; k++) { double e = mono(a + k) - mono(b + k); err += e * e; }
			if (err < bestErr) { bestErr = err; bestB = b; }
		}
		if (bestB > a + W) { c.loopStart = a; c.loopEnd = bestB; }
	}

	// Write one WAV honoring the channels/bitDepth options (src is interleaved stereo ±1).
	bool writeWav(const std::string& path, const float* src, size_t frames) {
		int ch = clamp(channels, 1, 2);
		std::vector<float> f(frames * ch);
		if (ch == 2) for (size_t i = 0; i < frames * 2; i++) f[i] = src[i];
		else for (size_t i = 0; i < frames; i++) f[i] = 0.5f * (src[i * 2] + src[i * 2 + 1]);

		drwav_data_format fmt; fmt.container = drwav_container_riff; fmt.channels = ch; fmt.sampleRate = (drwav_uint32)sr;
		drwav wav;
		if (bitDepth == 32) {
			fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT; fmt.bitsPerSample = 32;
			if (!drwav_init_file_write(&wav, path.c_str(), &fmt, NULL)) return false;
			drwav_write_pcm_frames(&wav, frames, f.data());
		} else if (bitDepth == 16) {
			fmt.format = DR_WAVE_FORMAT_PCM; fmt.bitsPerSample = 16;
			std::vector<drwav_int16> s16(f.size());
			drwav_f32_to_s16(s16.data(), f.data(), f.size());
			if (!drwav_init_file_write(&wav, path.c_str(), &fmt, NULL)) return false;
			drwav_write_pcm_frames(&wav, frames, s16.data());
		} else {                                  // 24-bit: pack the top 3 bytes of s32
			fmt.format = DR_WAVE_FORMAT_PCM; fmt.bitsPerSample = 24;
			std::vector<drwav_int32> s32(f.size());
			drwav_f32_to_s32(s32.data(), f.data(), f.size());
			std::vector<uint8_t> b(f.size() * 3);
			for (size_t i = 0; i < f.size(); i++) {
				int32_t v = s32[i];
				b[i * 3 + 0] = (v >> 8) & 0xFF; b[i * 3 + 1] = (v >> 16) & 0xFF; b[i * 3 + 2] = (v >> 24) & 0xFF;
			}
			if (!drwav_init_file_write(&wav, path.c_str(), &fmt, NULL)) return false;
			drwav_write_pcm_frames(&wav, frames, b.data());
		}
		drwav_uninit(&wav);
		return true;
	}

	void writeFiles() {                          // GUI thread
		std::string dir = destDir;               // write straight into the chosen folder
		system::createDirectories(dir);
		int written = 0;
		float peak = 1e-6f;
		for (auto& c : caps) { analyze(c);
			for (size_t i = c.trimStart * 2; i < c.trimEnd * 2; i++) peak = std::max(peak, std::fabs(c.data[i])); }
		float scale = normalize ? (0.985f / peak) : 1.f;
		std::string sfz = "// Generated by Signal Function Set — Record\n<control>\n<global>\n";
		// amp_veltrack=0: each layer was recorded *at* its velocity, so the dynamics
		// are already in the audio — a player must not scale by velocity again.
		sfz += string::f("<group>\namp_veltrack=0\nloop_mode=%s\n", loopInstrument ? "loop_continuous" : "one_shot");
		int R = clamp(rrCount, 1, 4);
		for (auto& c : caps) {
			std::string fn = (R > 1) ? string::f("%03d_v%03d_r%d.wav", c.midi, c.repVel, c.rr + 1)
			                         : string::f("%03d_v%03d.wav", c.midi, c.repVel);
			if (loopInstrument) findLoop(c);
			size_t frames = c.trimEnd - c.trimStart;
			std::vector<float> buf(frames * 2);
			for (size_t i = 0; i < frames * 2; i++) buf[i] = clamp(c.data[c.trimStart * 2 + i] * scale, -1.f, 1.f);
			if (writeWav(system::join(dir, fn), buf.data(), frames)) written++;
			sfz += string::f("<region> sample=%s lokey=%d hikey=%d pitch_keycenter=%d lovel=%d hivel=%d",
				fn.c_str(), c.lokey, c.hikey, c.midi, c.lovel, c.hivel);
			if (R > 1) sfz += string::f(" seq_length=%d seq_position=%d", R, c.rr + 1);
			if (loopInstrument) sfz += string::f(" loop_start=%ld loop_end=%ld", c.loopStart, c.loopEnd);
			sfz += "\n";
		}
		std::string sfzPath = system::join(dir, instrument + ".sfz");
		if (FILE* f = std::fopen(sfzPath.c_str(), "wb")) { std::fwrite(sfz.data(), 1, sfz.size(), f); std::fclose(f); }
		for (auto& c : caps) { c.data.clear(); c.data.shrink_to_fit(); }
		caps.clear(); lastOut = dir; lastCount = written; donePulse.trigger(1e-3f);
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "destDir", json_string(destDir.c_str()));
		json_object_set_new(r, "instrument", json_string(instrument.c_str()));
		json_object_set_new(r, "normalize", json_boolean(normalize));
		json_object_set_new(r, "autoTrim", json_boolean(autoTrim));
		json_object_set_new(r, "loopInstrument", json_boolean(loopInstrument));
		json_object_set_new(r, "triggerMode", json_boolean(triggerMode));
		json_object_set_new(r, "waitSilence", json_boolean(waitSilence));
		json_object_set_new(r, "gapMs", json_integer(gapMs));
		json_object_set_new(r, "channels", json_integer(channels));
		json_object_set_new(r, "bitDepth", json_integer(bitDepth));
		json_object_set_new(r, "rrCount", json_integer(rrCount));
		json_object_set_new(r, "latencySamps", json_integer(latencySamps));
		json_object_set_new(r, "kbView", json_integer(kbView));
		json_object_set_new(r, "gridLayout", json_integer(gridLayout));
		json_object_set_new(r, "gridRoot", json_integer(gridRoot));
		json_object_set_new(r, "gridScale", json_integer(gridScale));
		json_object_set_new(r, "gridBase", json_integer(gridBase));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "destDir")) destDir = json_string_value(j);
		if (json_t* j = json_object_get(r, "instrument")) instrument = json_string_value(j);
		if (json_t* j = json_object_get(r, "normalize")) normalize = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "autoTrim")) autoTrim = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "loopInstrument")) loopInstrument = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "triggerMode")) triggerMode = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "waitSilence")) waitSilence = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "gapMs")) gapMs = clamp((int)json_integer_value(j), 0, 5000);
		if (json_t* j = json_object_get(r, "channels")) channels = clamp((int)json_integer_value(j), 1, 2);
		if (json_t* j = json_object_get(r, "bitDepth")) bitDepth = (int)json_integer_value(j);
		if (json_t* j = json_object_get(r, "rrCount")) rrCount = clamp((int)json_integer_value(j), 1, 4);
		if (json_t* j = json_object_get(r, "latencySamps")) latencySamps = std::max(0, (int)json_integer_value(j));
		if (json_t* j = json_object_get(r, "kbView")) kbView = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(r, "gridLayout")) gridLayout = clamp((int)json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(r, "gridRoot")) gridRoot = clamp((int)json_integer_value(j), 0, 11);
		if (json_t* j = json_object_get(r, "gridScale")) gridScale = clamp((int)json_integer_value(j), 0, sfs::NUM_SCALES - 1);
		if (json_t* j = json_object_get(r, "gridBase")) gridBase = clamp((int)json_integer_value(j), 0, 115);
	}
};

// ─── Display ─────────────────────────────────────────────────────────────────
void RecordDisplay::drawScope(NVGcontext* vg, float x, float y, float w, float h,
                              const float* mn, const float* mx, int n) {
	nvgSave(vg); nvgScissor(vg, x, y, w, h);
	const float mid = y + h / 2.f, amp = h / 2.f - 1.f;
	// centre line
	nvgBeginPath(vg); nvgMoveTo(vg, x, mid); nvgLineTo(vg, x + w, mid);
	nvgStrokeColor(vg, nvgRGBA(0x8a, 0x8a, 0xb0, 0x40)); nvgStrokeWidth(vg, 0.5f); nvgStroke(vg);

	auto HI = [&](int i) {
		if (mx) return mx[i];
		float e = 0.62f * std::max(0.f, 1.f - i / (float)(n * 0.72f));   // preview envelope
		return e * (0.55f + 0.45f * std::fabs(std::sin(i * 0.22f)));
	};
	auto LO = [&](int i) { return mn ? mn[i] : -HI(i); };
	auto PX = [&](int i) { return x + w * i / (float)(n - 1); };

	// filled envelope (max on top, min on bottom) — the classic waveform look
	nvgBeginPath(vg);
	nvgMoveTo(vg, PX(0), mid - HI(0) * amp);
	for (int i = 1; i < n; i++) nvgLineTo(vg, PX(i), mid - HI(i) * amp);
	for (int i = n - 1; i >= 0; i--) nvgLineTo(vg, PX(i), mid - LO(i) * amp);
	nvgFillColor(vg, nvgRGBA(0x00, 0x97, 0xde, 0xdd)); nvgFill(vg);
	nvgRestore(vg);
}

void RecordDisplay::drawKeys(NVGcontext* vg, float x, float y, float w, float h,
                             const uint8_t* sampled, int cur, int rlo, int rhi) {
	int NW = recWhiteBefore(KB_HI + 1);
	float ww = w / NW;
	NVGcolor cyan = nvgRGB(0x00, 0xc8, 0xff), blue = nvgRGB(0x00, 0x97, 0xde);
	// selection-area band
	float bx0 = x + recWhiteBefore(rlo) * ww;
	float bx1 = x + (recWhiteBefore(rhi) + 1) * ww;
	nvgBeginPath(vg); nvgRect(vg, bx0, y - 2, bx1 - bx0, h + 4);
	nvgFillColor(vg, nvgRGBA(0x00, 0x97, 0xde, 0x22)); nvgFill(vg);
	// white keys — flat top, rounded bottom, with a gap so adjacent keys stay distinct
	NVGcolor gap = nvgRGB(0x1a, 0x1a, 0x2e);
	int k = 0;
	for (int m = KB_LO; m <= KB_HI; m++) {
		if (!recIsWhite(m)) continue;
		float x0 = x + k * ww;
		NVGcolor col = (m == cur) ? cyan : (sampled[m] ? blue : nvgRGB(0x3a, 0x3a, 0x4a));
		nvgBeginPath(vg); nvgRoundedRectVarying(vg, x0 + 0.6f, y, ww - 1.2f, h, 0.f, 0.f, 1.4f, 1.4f);
		nvgFillColor(vg, col); nvgFill(vg);
		k++;
	}
	// black keys — dark gap around each so it reads even when neighbours are selected
	float bh = h * 0.62f, bw = ww * 0.6f;
	for (int m = KB_LO; m <= KB_HI; m++) {
		if (recIsWhite(m)) continue;
		float bx = x + recWhiteBefore(m) * ww - bw * 0.5f;
		nvgBeginPath(vg); nvgRoundedRectVarying(vg, bx - 0.9f, y, bw + 1.8f, bh + 0.9f, 0.f, 0.f, 1.6f, 1.6f);
		nvgFillColor(vg, gap); nvgFill(vg);
		NVGcolor col = (m == cur) ? cyan : (sampled[m] ? blue : nvgRGB(0x20, 0x20, 0x2c));
		nvgBeginPath(vg); nvgRoundedRectVarying(vg, bx, y, bw, bh, 0.f, 0.f, 1.2f, 1.2f);
		nvgFillColor(vg, col); nvgFill(vg);
	}
	// octave labels under C keys
	if (font) {
		nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 6.5f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
		nvgFillColor(vg, nvgRGB(0x8a, 0x8a, 0xb0));
		for (int m = KB_LO; m <= KB_HI; m++) {
			if (m % 12 != 0) continue;
			float cx = x + recWhiteBefore(m) * ww + ww * 0.5f;
			nvgText(vg, cx, y + h + 1, string::f("C%d", m / 12 - 1).c_str(), NULL);
		}
	}
}

// Tabs, left→right: GRID (view 1) then PIANO (view 0).
static const int REC_TAB_VIEW[2] = { 1, 0 };
static const char* REC_TAB_LABEL[2] = { "GRID", "PIANO" };
void RecordDisplay::drawTabs(NVGcontext* vg, int view) {
	Rect r[2]; tabRects(r);
	for (int i = 0; i < 2; i++) {
		bool active = (view == REC_TAB_VIEW[i]);
		nvgBeginPath(vg); nvgRoundedRect(vg, r[i].pos.x, r[i].pos.y, r[i].size.x, r[i].size.y, 2.f);
		nvgFillColor(vg, active ? nvgRGB(0x0d, 0x59, 0x86) : nvgRGB(0x2a, 0x2a, 0x3e)); nvgFill(vg);
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, active ? nvgRGB(0xe6, 0xe6, 0xf0) : nvgRGB(0x6a, 0x6a, 0x88));
			nvgText(vg, r[i].pos.x + r[i].size.x * 0.5f, r[i].pos.y + r[i].size.y * 0.5f + 0.5f, REC_TAB_LABEL[i], NULL);
		}
	}
}

void RecordDisplay::drawGrid(NVGcontext* vg, const uint8_t* sampled, int cur, int rlo, int rhi,
                             int layout, int gRoot, int gScale, int gBase) {
	float x0, y0, pad; gridGeom(x0, y0, pad);
	float ps = pad * 0.88f, off = (pad - ps) * 0.5f;
	for (int rr = 0; rr < GRID_ROWS; rr++) {
		int row = GRID_ROWS - 1 - rr;
		for (int col = 0; col < GRID_COLS; col++) {
			int n = gridNoteAt(layout, gRoot, gScale, gBase, row, col);
			float px = x0 + col * pad + off, py = y0 + rr * pad + off;
			bool inScale = (n >= 0) && gridNoteInScale(n, gRoot, gScale);
			bool isKeyRoot = (n >= 0) && (((n - gRoot) % 12 + 12) % 12 == 0);
			bool inRange = (n >= rlo && n <= rhi);
			NVGcolor c;
			if (n < 0)              c = nvgRGB(0x14, 0x14, 0x22);
			else if (n == cur)      c = nvgRGB(0x00, 0xc8, 0xff);   // current / auditioning
			else if (sampled[n])    c = nvgRGB(0x00, 0x97, 0xde);   // captured
			else if (inRange)       c = nvgRGB(0x25, 0x4c, 0x66);   // will be sampled
			else if (layout == 2)   c = nvgRGB(0x40, 0x40, 0x54);
			else                    c = inScale ? nvgRGB(0x3a, 0x3a, 0x4a) : nvgRGB(0x20, 0x20, 0x2c);
			// chromatic grid: darken accidental (black-key) columns across every tier
			// so they read as a piano-roll overlay even inside the lit/sampled region
			if (layout == 2 && n >= 0 && n != cur && gridIsAccidental(n))
				c = nvgRGBf(c.r * 0.55f, c.g * 0.55f, c.b * 0.55f);
			nvgBeginPath(vg); nvgRoundedRect(vg, px, py, ps, ps, 2.f); nvgFillColor(vg, c); nvgFill(vg);
			if (isKeyRoot) {
				nvgBeginPath(vg); nvgRoundedRect(vg, px + 0.5f, py + 0.5f, ps - 1.f, ps - 1.f, 2.f);
				nvgStrokeColor(vg, nvgRGBA(0xff, 0xd0, 0x60, 0xcc)); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
				if (font && ps > 10.f) {
					nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 6.5f);
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0xdd));
					nvgText(vg, px + ps * 0.5f, py + ps * 0.5f, string::f("%s%d", REC_NOTE_NAMES[((n % 12) + 12) % 12], n / 12 - 1).c_str(), NULL);
				}
			}
		}
	}
}

void RecordDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	NVGcontext* vg = args.vg;
	const float w = box.size.x, h = box.size.y;
	nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
	nvgFillColor(vg, nvgRGB(0x1a, 0x1a, 0x2e)); nvgFill(vg);

	const char *name = "Instrument", *status = "ready";
	int lo = 48, hi = 84, cur = 60; float prog = 0.f;
	uint8_t prevS[128] = {0};
	const uint8_t* sampled = prevS;
	const float *mn = nullptr, *mx = nullptr;
	int view = 1, layout = 0, gRoot = 0, gScale = 1, gBase = 36;
	if (module) {
		name = module->dispName; status = module->dispStatus;
		lo = module->kbLo; hi = module->kbHi; cur = module->kbCur; prog = module->dispProg;
		sampled = module->kbSampled; mn = module->scopeMin; mx = module->scopeMax;
		view = module->kbView; layout = module->gridLayout; gRoot = module->gridRoot;
		gScale = module->gridScale; gBase = module->gridBase;
	} else { for (int n = 48; n <= 84; n += 3) prevS[n] = 1; layout = 2; lo = 48; hi = 84; }

	drawTabs(vg, view);
	if (font) {
		nvgFontFaceId(vg, font->handle);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFontSize(vg, 10.f); nvgFillColor(vg, nvgRGB(0xc8, 0xc8, 0xe0)); nvgText(vg, 6, 31, name, NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
		nvgFontSize(vg, 8.f); nvgFillColor(vg, nvgRGB(0xec, 0x65, 0x2e)); nvgText(vg, w - 6, 31, status, NULL);
	}
	// progress
	nvgBeginPath(vg); nvgRoundedRect(vg, 6, 35, w - 12, 2.5f, 1.f);
	nvgFillColor(vg, nvgRGB(0x35, 0x35, 0x4d)); nvgFill(vg);
	nvgBeginPath(vg); nvgRoundedRect(vg, 6, 35, (w - 12) * clamp(prog, 0.f, 1.f), 2.5f, 1.f);
	nvgFillColor(vg, nvgRGB(0x00, 0x97, 0xde)); nvgFill(vg);

	if (view == 1) {
		drawGrid(vg, sampled, cur, lo, hi, layout, gRoot, gScale, gBase);
	} else {
		// keyboard sits above the bottom edge with clearance for the octave labels
		float kbY = h - 36;
		drawScope(vg, 6, 42, w - 12, kbY - 42 - 4, mn, mx, Record::SCOPE_N);
		drawKeys(vg, 6, kbY, w - 12, 24, sampled, cur, lo, hi);
	}
}

int RecordDisplay::hitPad(Vec p) const {
	if (!module) return -1;
	float x0, y0, pad; gridGeom(x0, y0, pad);
	if (p.x < x0 || p.y < y0) return -1;
	int col = (int)((p.x - x0) / pad), rr = (int)((p.y - y0) / pad);
	if (col < 0 || col >= GRID_COLS || rr < 0 || rr >= GRID_ROWS) return -1;
	return gridNoteAt(module->gridLayout, module->gridRoot, module->gridScale, module->gridBase, GRID_ROWS - 1 - rr, col);
}

int RecordDisplay::pianoNoteAt(Vec p) const {
	float x = 6, y = box.size.y - 30, w = box.size.x - 12, h = 26;
	if (p.y < y || p.y > y + h) return -1;
	int NW = recWhiteBefore(KB_HI + 1); float ww = w / NW;
	float bh = h * 0.62f, bw = ww * 0.6f;
	if (p.y <= y + bh) {
		for (int m = KB_LO; m <= KB_HI; m++) {
			if (recIsWhite(m)) continue;
			float bx = x + recWhiteBefore(m) * ww - bw * 0.5f;
			if (p.x >= bx && p.x <= bx + bw) return m;
		}
	}
	int k = (int)((p.x - x) / ww); if (k < 0) return -1;
	int cnt = 0;
	for (int m = KB_LO; m <= KB_HI; m++) { if (!recIsWhite(m)) continue; if (cnt == k) return m; cnt++; }
	return -1;
}

void RecordDisplay::onButton(const ButtonEvent& e) {
	if (!module) { OpaqueWidget::onButton(e); return; }
	if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
		Vec p = e.pos;
		int t = hitTab(p);
		if (t >= 0) { module->kbView = REC_TAB_VIEW[t]; e.consume(this); return; }
		int n = (module->kbView == 1) ? hitPad(p) : pianoNoteAt(p);
		if (n >= 0) { module->uiNote = n; dragging = true; dragPos = p; e.consume(this); return; }
	}
	if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_RELEASE && dragging) {
		module->uiNote = -1; dragging = false; e.consume(this); return;
	}
	OpaqueWidget::onButton(e);
}
void RecordDisplay::onDragMove(const DragMoveEvent& e) {
	if (!module || !dragging) { OpaqueWidget::onDragMove(e); return; }
	float zoom = getAbsoluteZoom(); if (zoom <= 0.f) zoom = 1.f;
	dragPos = dragPos.plus(e.mouseDelta.div(zoom));
	int n = (module->kbView == 1) ? hitPad(dragPos) : pianoNoteAt(dragPos);
	if (n >= 0) module->uiNote = n;
}
void RecordDisplay::onDragEnd(const DragEndEvent& e) {
	if (module && dragging) { module->uiNote = -1; dragging = false; }
	OpaqueWidget::onDragEnd(e);
}

// ─── Widget ──────────────────────────────────────────────────────────────────
struct RecordWidget : ModuleWidget {
	RecordWidget(Record* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/record.svg")));
		// No virtual screws — see CLAUDE.md.

		RecordDisplay* disp = new RecordDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(4.f, 12.f));
		disp->box.size = mm2px(Vec(73.f, 57.f));
		addChild(disp);

		// Knobs: 2 columns (x=10.16, 30.48) × 3 rows (y=79.83, 100.81, 122.16)
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 79.83f)), module, Record::START_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48f, 79.83f)), module, Record::SPACING_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 100.81f)), module, Record::OCTAVES_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48f, 100.81f)), module, Record::VELLAYERS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 122.16f)), module, Record::TAIL_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48f, 122.16f)), module, Record::SUSTAIN_PARAM));

		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(55.88f, 78.14f)), module, Record::AUDITION_PARAM, Record::AUDITION_LIGHT));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedLight>>>(
			mm2px(Vec(71.12f, 78.14f)), module, Record::RECORD_PARAM, Record::RECORD_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.88f, 90.84f)), module, Record::L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(71.12f, 90.84f)), module, Record::R_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(55.88f, 106.92f)), module, Record::VEL_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(70.27f, 106.92f)), module, Record::DONE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(55.88f, 122.16f)), module, Record::VOCT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(70.27f, 122.16f)), module, Record::GATE_OUTPUT));
	}

	void step() override {
		ModuleWidget::step();
		Record* m = dynamic_cast<Record*>(module);
		if (!m) return;
		if (m->startReq == 1) {
			m->startReq = 0;
			if (m->destDir.empty()) {
				char* dir = osdialog_file(OSDIALOG_OPEN_DIR, NULL, NULL, NULL);
				if (dir) { m->destDir = dir; std::free(dir); } else return;
			}
			m->startPlan(true);
		} else if (m->startReq == 2) { m->startReq = 0; m->startPlan(false); }
		else if (m->startReq == 3) { m->startReq = 0; m->startCalibrate(); }
		if (m->state == Record::WRITE_PENDING) { m->state = Record::WRITING; m->writeFiles(); m->state = Record::DONE_ST; }
	}

	void appendContextMenu(Menu* menu) override {
		Record* m = dynamic_cast<Record*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Set destination folder…", m->destDir,
			[m]() { char* d = osdialog_file(OSDIALOG_OPEN_DIR, NULL, NULL, NULL); if (d) { m->destDir = d; std::free(d); } }));
		menu->addChild(createMenuItem("Instrument name…", m->instrument,
			[m]() { char* s = osdialog_prompt(OSDIALOG_INFO, "Instrument name", m->instrument.c_str());
			        if (s) { if (*s) m->instrument = s; std::free(s); } }));
		if (!m->lastOut.empty())
			menu->addChild(createMenuLabel(string::f("Last: %d files → %s", m->lastCount, m->lastOut.c_str())));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Gate output"));
		menu->addChild(createCheckMenuItem("Gate (held)", "", [m]() { return !m->triggerMode; }, [m]() { m->triggerMode = false; }));
		menu->addChild(createCheckMenuItem("Trigger (pulse)", "", [m]() { return m->triggerMode; }, [m]() { m->triggerMode = true; }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Wait for silence between notes", "", &m->waitSilence));
		menu->addChild(createSubmenuItem("Min gap between notes", string::f("%d ms", m->gapMs),
			[m](Menu* sub) {
				for (int g : {50, 100, 250, 500, 1000, 2000})
					sub->addChild(createCheckMenuItem(string::f("%d ms", g), "",
						[m, g]() { return m->gapMs == g; }, [m, g]() { m->gapMs = g; }));
			}));
		menu->addChild(createBoolPtrMenuItem("Normalize", "", &m->normalize));
		menu->addChild(createBoolPtrMenuItem("Auto-trim silence", "", &m->autoTrim));
		menu->addChild(createBoolPtrMenuItem("Loop this instrument", "", &m->loopInstrument));

		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Round-robin takes", string::f("%d", m->rrCount),
			[m](Menu* sub) {
				for (int n = 1; n <= 4; n++)
					sub->addChild(createCheckMenuItem(string::f("%d", n), "",
						[m, n]() { return m->rrCount == n; }, [m, n]() { m->rrCount = n; }));
			}));
		menu->addChild(createSubmenuItem("Channels", m->channels == 1 ? "Mono" : "Stereo",
			[m](Menu* sub) {
				sub->addChild(createCheckMenuItem("Mono", "", [m]() { return m->channels == 1; }, [m]() { m->channels = 1; }));
				sub->addChild(createCheckMenuItem("Stereo", "", [m]() { return m->channels == 2; }, [m]() { m->channels = 2; }));
			}));
		menu->addChild(createSubmenuItem("Bit depth", string::f("%d-bit", m->bitDepth),
			[m](Menu* sub) {
				for (int b : {16, 24, 32})
					sub->addChild(createCheckMenuItem(string::f("%d-bit%s", b, b == 32 ? " float" : ""), "",
						[m, b]() { return m->bitDepth == b; }, [m, b]() { m->bitDepth = b; }));
			}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Calibrate latency", m->latencySamps > 0
				? string::f("%.1f ms", m->latencySamps * 1000.f / std::max(1.f, m->sr)) : "not set",
			[m]() { m->startReq = 3; }));
		if (m->latencySamps > 0)
			menu->addChild(createMenuItem("Clear latency", "", [m]() { m->latencySamps = 0; }));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Grid view"));
		menu->addChild(createIndexSubmenuItem("Layout", {"Chromatic (4ths)", "In-Key (scale)", "Chromatic grid (C0)"},
			[m]() { return m->gridLayout; }, [m](int i) { m->gridLayout = i; }));
		std::vector<std::string> roots(REC_NOTE_NAMES, REC_NOTE_NAMES + 12);
		menu->addChild(createIndexSubmenuItem("Root", roots,
			[m]() { return m->gridRoot; }, [m](int i) { m->gridRoot = i; }));
		std::vector<std::string> scales;
		for (int i = 0; i < sfs::NUM_SCALES; i++) scales.push_back(sfs::SCALES[i].longName);
		menu->addChild(createIndexSubmenuItem("Scale (In-Key)", scales,
			[m]() { return m->gridScale; }, [m](int i) { m->gridScale = i; }));
		menu->addChild(createMenuItem("Octave up", "", [m]() { if (m->gridBase <= 103) m->gridBase += 12; }));
		menu->addChild(createMenuItem("Octave down", "", [m]() { if (m->gridBase >= 12) m->gridBase -= 12; }));
	}
};

Model* modelRecord = createModel<Record, RecordWidget>("Record");
