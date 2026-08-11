#include "plugin.hpp"
#include <osdialog.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef METAMODULE
#include "filesystem/async_filebrowser.hh"
#include "filesystem/helpers.hh"
#include "patch/patch_file.hh"
// MetaModule's newlib doesn't provide <mutex>. The Phase module uses
// std::mutex / std::unique_lock to coordinate cue-edit access between
// the GUI thread and the audio thread. On MetaModule, the audio engine
// pauses during patch load and GUI callbacks, so no-op stubs are safe.
namespace std {
struct mutex { void lock() {} void unlock() {} bool try_lock() { return true; } };
template<typename T> struct lock_guard { lock_guard(T&) {} };
struct try_to_lock_t {};
inline constexpr try_to_lock_t try_to_lock{};
template<typename T> struct unique_lock {
	unique_lock(T&) {}
	unique_lock(T&, try_to_lock_t) {}
	bool owns_lock() const { return true; }
};
}
#else
#include <mutex>
#endif

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"


// Waveform display resolution
static const int WAVEFORM_POINTS = 512;

// WAV file filter for open dialog
static const char PHASE_WAV_FILTERS[] = "WAV file (.wav):wav;All files (*.*):*";

#ifdef METAMODULE
// On MetaModule we halve sample-buffer memory by storing samples as int16
// instead of float. 16-bit at 48kHz is still transparent quality for looper
// work; the memory saving matters because MetaModule shares ~300MB across all
// plugins. We also cap max sample/record length harder than desktop.
using PhaseSampleT = int16_t;
static const size_t MAX_SAMPLE_LENGTH = 48000 * 60 * 3;  //  3 min per loop
static const size_t MAX_REC_LENGTH    = 48000 * 30;      // 30 s per record
static inline float sampleToFloat(int16_t s) { return (float)s * (1.f / 32768.f); }
static inline int16_t floatToSample(float f) {
	if (f >=  1.f) return  32767;
	if (f <= -1.f) return -32768;
	return (int16_t)(f * 32768.f);
}
#else
// VCV (desktop): float storage, generous limits.
using PhaseSampleT = float;
static const size_t MAX_SAMPLE_LENGTH = 48000 * 60 * 10; // 10 min per loop
static const size_t MAX_REC_LENGTH    = 48000 * 60;      // 60 s per record
static inline float sampleToFloat(float s) { return s; }
static inline float floatToSample(float f) { return f; }
#endif


// --- Base64 helpers (used to embed audio buffers in patch JSON) ---
// Standard base64 alphabet, no line wrapping.
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)data[i] << 16;
		if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
		if (i + 2 < len) v |= (uint32_t)data[i + 2];
		out.push_back(B64_CHARS[(v >> 18) & 0x3F]);
		out.push_back(B64_CHARS[(v >> 12) & 0x3F]);
		out.push_back(i + 1 < len ? B64_CHARS[(v >> 6) & 0x3F] : '=');
		out.push_back(i + 2 < len ? B64_CHARS[v & 0x3F] : '=');
	}
	return out;
}

static std::vector<uint8_t> base64Decode(const std::string& s) {
	static int8_t lookup[256];
	static bool initLookup = false;
	if (!initLookup) {
		for (int i = 0; i < 256; i++) lookup[i] = -1;
		for (int i = 0; i < 64; i++) lookup[(uint8_t)B64_CHARS[i]] = (int8_t)i;
		initLookup = true;
	}
	std::vector<uint8_t> out;
	out.reserve((s.size() / 4) * 3);
	uint32_t buf = 0;
	int bits = 0;
	for (char c : s) {
		if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
		int8_t v = lookup[(uint8_t)c];
		if (v < 0) continue;
		buf = (buf << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((uint8_t)((buf >> bits) & 0xFF));
		}
	}
	return out;
}


struct SampleData {
	std::vector<PhaseSampleT> samples;   // int16 on MM, float on VCV
	size_t length = 0;
	std::string filePath;
	std::string fileName;
	std::vector<size_t> transients;
	std::vector<float> waveformMini; // peak amplitude per display column
	// Atomic so the audio thread can safely check it without a lock.
	// Writers (load/clear/finalize) set this to false BEFORE mutating
	// `samples`/`length` and back to true after, so the audio thread skips
	// reads of sd.samples whenever the buffer is in flux.
	std::atomic<bool> loaded{false};
	bool hasCuePoints = false; // true if transients came from WAV cue points
	bool cuesEdited = false;   // true once cues were manually edited; persist them
	// Loop region (normalized 0-1)
	float loopStart = 0.f;
	float loopEnd = 1.f;
};


struct LoopState {
	double playhead = 0.0;
	bool sleeping = false;
	float sleepRemaining = 0.f;
	dsp::SchmittTrigger clockTrigger;
	// VCA mode: anti-click envelope
	float envelope = 1.f;       // current envelope level (0-1)
	bool ramping = false;       // true when in a fade-out/fade-in transition
	double jumpTarget = -1.0;   // where to jump after fade-out completes
	// Rotate mode: accumulated rotation offset in samples
	double rotationOffset = 0.0;
};


// Live-recording state per buffer.
// State machine:
//   IDLE → ARMING (1ms fade-in) → ACTIVE → FINISHING (1ms fade-out) → IDLE
// During ARMING/ACTIVE/FINISHING the loop's normal playback is suppressed
// and a dry monitor passthrough is sent to the output instead.
// On reaching IDLE after FINISHING, pendingFinalize is set and the widget
// step() finalizes the recording on the GUI thread (copy into samples,
// run waveform/transient analysis).
struct RecState {
	enum Phase { IDLE, ARMING, ACTIVE, FINISHING };
	Phase phase = IDLE;
	bool armed = false;             // panel-button latch state
	std::atomic<size_t> writePos{0}; // current write index into recBuffer (atomic for GUI reads)
	float envelope = 0.f;           // anti-click fade envelope
	std::vector<PhaseSampleT> recBuffer;   // pre-allocated, MAX_REC_LENGTH
	std::atomic<bool> pendingFinalize{false};
	size_t recordedLength = 0;      // captured at FINISHING completion
};


// Forward declaration
struct Phase;

struct PhaseWaveformDisplay : Widget {
	Phase* module = nullptr;

	// Handle dragging state
	enum DragTarget {
		NONE,
		LOOP_START_A,
		LOOP_END_A,
		LOOP_START_B,
		LOOP_END_B,
		CUE_A,
		CUE_B
	};
	DragTarget dragTarget = NONE;
	int dragCueIdx = -1;   // index into the dragged sample's transients
	Vec lastButtonPos;     // last left-press position (onDoubleClick carries none)

	PhaseWaveformDisplay() {}

	void drawWaveform(const DrawArgs& args, const std::vector<float>& mini, float x, float y, float w, float h,
	                  float loopStart, float loopEnd, NVGcolor color,
	                  float rotationNorm = 0.f) {
		if (mini.empty()) return;
		int n = (int)mini.size();

		// Dim region outside loop
		NVGcolor dimColor = nvgRGBA(10, 10, 20, 200);
		if (loopStart > 0.f) {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x, y, loopStart * w, h);
			nvgFillColor(args.vg, dimColor);
			nvgFill(args.vg);
		}
		if (loopEnd < 1.f) {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x + loopEnd * w, y, (1.f - loopEnd) * w, h);
			nvgFillColor(args.vg, dimColor);
			nvgFill(args.vg);
		}

		// Compute rotation offset in waveform-mini indices
		// rotationNorm is the rotation as a fraction of the total sample
		// We only rotate the portion within the loop region
		int loopStartIdx = (int)(loopStart * n);
		int loopEndIdx = (int)(loopEnd * n);
		int loopLen = loopEndIdx - loopStartIdx;
		int rotOffset = 0;
		if (loopLen > 0 && rotationNorm != 0.f) {
			rotOffset = (int)(rotationNorm * n) % loopLen;
			if (rotOffset < 0) rotOffset += loopLen;
		}

		nvgBeginPath(args.vg);
		float midY = y + h * 0.5f;

		for (int i = 0; i < n; i++) {
			// Apply rotation only within the loop region
			int srcIdx = i;
			if (i >= loopStartIdx && i < loopEndIdx && loopLen > 0) {
				int relIdx = i - loopStartIdx;
				srcIdx = loopStartIdx + ((relIdx + rotOffset) % loopLen);
			}
			float px = x + (float)i / (float)n * w;
			float amp = mini[srcIdx] * h * 0.45f;
			if (i == 0) {
				nvgMoveTo(args.vg, px, midY - amp);
			} else {
				nvgLineTo(args.vg, px, midY - amp);
			}
		}
		// Draw back along bottom
		for (int i = n - 1; i >= 0; i--) {
			int srcIdx = i;
			if (i >= loopStartIdx && i < loopEndIdx && loopLen > 0) {
				int relIdx = i - loopStartIdx;
				srcIdx = loopStartIdx + ((relIdx + rotOffset) % loopLen);
			}
			float px = x + (float)i / (float)n * w;
			float amp = mini[srcIdx] * h * 0.45f;
			nvgLineTo(args.vg, px, midY + amp);
		}
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, color);
		nvgFill(args.vg);
	}

	void drawTransients(const DrawArgs& args, const std::vector<size_t>& transients, size_t sampleLength, float x, float y, float w, float h, NVGcolor color) {
		if (transients.empty() || sampleLength == 0) return;

		nvgStrokeColor(args.vg, color);
		nvgStrokeWidth(args.vg, 0.75f);

		for (size_t t : transients) {
			float px = x + (float)t / (float)sampleLength * w;
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, px, y + 1.f);
			nvgLineTo(args.vg, px, y + h - 1.f);
			nvgStroke(args.vg);
		}
	}

	void drawPlayhead(const DrawArgs& args, double playhead, size_t sampleLength, float x, float y, float w, float h, NVGcolor color) {
		if (sampleLength == 0) return;

		float px = x + (float)(playhead / (double)sampleLength) * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, px, y);
		nvgLineTo(args.vg, px, y + h);
		nvgStrokeColor(args.vg, color);
		nvgStrokeWidth(args.vg, 1.5f);
		nvgStroke(args.vg);
	}

	void drawHandle(const DrawArgs& args, float normPos, float x, float y, float w, float h, NVGcolor color, bool isStart) {
		float px = x + normPos * w;
		// Clamp to stay within display bounds
		if (px < x) px = x;
		if (px > x + w) px = x + w;

		float tickLen = 5.f;    // horizontal tick length at top and bottom
		float lineW = 1.f;      // vertical line width

		// Draw as a single filled path (no transparency stacking)
		nvgBeginPath(args.vg);

		// Build bracket shape as a filled polygon
		// Vertical bar + top tick + bottom tick
		float barLeft = isStart ? px : px - lineW;

		// Vertical bar
		nvgRect(args.vg, barLeft, y + 1.f, lineW, h - 2.f);

		// Top horizontal tick
		float tickLeft = isStart ? px : px - tickLen;
		float tickRight = isStart ? px + tickLen : px;
		nvgRect(args.vg, tickLeft, y + 1.f, tickRight - tickLeft, lineW);

		// Bottom horizontal tick
		nvgRect(args.vg, tickLeft, y + h - 1.f - lineW, tickRight - tickLeft, lineW);

		nvgFillColor(args.vg, color);
		nvgFill(args.vg);
	}

	DragTarget hitTestHandle(Vec pos);
	int hitTestCue(Vec pos, bool& isB);   // nearest cue index within hit radius, or -1
	void onButton(const ButtonEvent& e) override;
	void onDoubleClick(const DoubleClickEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;

	void drawLayer(const DrawArgs& args, int layer) override;

	void draw(const DrawArgs& args) override {
		Widget::draw(args);
	}
};


struct Phase : Module {
	enum ParamId {
		SLEEP_A_PARAM,
		SPEED_A_PARAM,
		PAN_A_PARAM,
		SLEEP_B_PARAM,
		SPEED_B_PARAM,
		PAN_B_PARAM,
		PLAY_PARAM,
		MODE_A_PARAM,  // 0 = sleep, 1 = rotate
		MODE_B_PARAM,
		SYNC_PARAM,
		REC_A_PARAM,
		REC_B_PARAM,
		REC_LINK_PARAM,  // link switch: when on, REC A and REC B trigger together
		PARAMS_LEN
	};
	enum InputId {
		SLEEP_A_INPUT,
		SPEED_A_INPUT,
		PAN_A_INPUT,
		CLOCK_A_INPUT,
		SLEEP_B_INPUT,
		SPEED_B_INPUT,
		PAN_B_INPUT,
		CLOCK_B_INPUT,
		SYNC_INPUT,
		PLAY_INPUT,
		START_A_INPUT,
		END_A_INPUT,
		START_B_INPUT,
		END_B_INPUT,
		REC_A_INPUT,        // audio in for recording
		REC_A_GATE_INPUT,   // record gate
		REC_B_INPUT,        // audio in for recording (normalled from REC_A_INPUT)
		REC_B_GATE_INPUT,   // record gate (normalled from REC_A_GATE_INPUT)
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		PLAY_LIGHT,
		REC_A_LIGHT,
		REC_B_LIGHT,
		REC_LINK_LIGHT,
		LIGHTS_LEN
	};

	SampleData sampleA;
	SampleData sampleB;
	LoopState loopA;
	LoopState loopB;
	RecState recA;
	RecState recB;
	dsp::SchmittTrigger syncTrigger;
	bool sampleBExplicitlyLoaded = false;
	bool playing = false;

	// Transient detection sensitivity (0.0 = most sensitive, 1.0 = least sensitive)
	float transientSensitivity = 0.7f;
	// Minimum gap between transients in ms
	float transientMinGapMs = 100.f;
	// VCA mode: anti-click envelope on jumps
	bool vcaMode = true;
	// Recording mode: false = replace (start fresh), true = append to existing audio
	bool recordAppend = false;
	// Link button latch: when true, REC A and REC B trigger together
	bool recLink = false;
	// Whether to embed recorded buffers in the patch JSON (default on).
	// File-loaded samples are always referenced by path, never embedded.
	bool persistRecordedBuffers = true;

	Phase() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(SLEEP_A_PARAM, -500.f, 500.f, 0.f, "Drift A", " ms");
		configParam(SPEED_A_PARAM, -4.f, 4.f, 1.f, "Speed A", "x");
		configParam(PAN_A_PARAM, -1.f, 1.f, 0.f, "Pan A");

		configParam(SLEEP_B_PARAM, -500.f, 500.f, 0.f, "Drift B", " ms");
		configParam(SPEED_B_PARAM, -4.f, 4.f, 1.f, "Speed B", "x");
		configParam(PAN_B_PARAM, -1.f, 1.f, 0.f, "Pan B");

		configButton(PLAY_PARAM, "Play / Stop");
		configSwitch(MODE_A_PARAM, 0.f, 1.f, 1.f, "Mode A", {"Rotate", "Sleep"});
		configSwitch(MODE_B_PARAM, 0.f, 1.f, 1.f, "Mode B", {"Rotate", "Sleep"});
		configButton(SYNC_PARAM, "Sync (reset both loops)");

		configInput(SLEEP_A_INPUT, "Drift A CV");
		configInput(SPEED_A_INPUT, "Speed A CV");
		configInput(PAN_A_INPUT, "Pan A CV");
		configInput(CLOCK_A_INPUT, "Clock A (transient jump)");

		configInput(SLEEP_B_INPUT, "Drift B CV");
		configInput(SPEED_B_INPUT, "Speed B CV");
		configInput(PAN_B_INPUT, "Pan B CV");
		configInput(CLOCK_B_INPUT, "Clock B (transient jump)");

		configInput(SYNC_INPUT, "Sync (reset both loops)");
		configInput(PLAY_INPUT, "Play gate (high = play)");

		configInput(START_A_INPUT, "Loop Start A (0-10V = 0-100%)");
		configInput(END_A_INPUT, "Loop Length A (0-10V = 0-100%)");
		configInput(START_B_INPUT, "Loop Start B (0-10V = 0-100%)");
		configInput(END_B_INPUT, "Loop Length B (0-10V = 0-100%)");

		configButton(REC_A_PARAM, "Record A (latch)");
		configButton(REC_B_PARAM, "Record B (latch)");
		configButton(REC_LINK_PARAM, "Link record buttons (one click triggers both)");
		configInput(REC_A_INPUT, "Record A audio");
		configInput(REC_A_GATE_INPUT, "Record A gate (high = recording)");
		configInput(REC_B_INPUT, "Record B audio (normalled from Record A audio)");
		configInput(REC_B_GATE_INPUT, "Record B gate (normalled from Record A gate)");

		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");

		// Pre-allocate the record buffers once at construction.
		// This avoids any allocation on the audio thread when recording starts.
		recA.recBuffer.resize(MAX_REC_LENGTH, 0.f);
		recB.recBuffer.resize(MAX_REC_LENGTH, 0.f);
	}

	void onReset() override {
		// Reset loop regions
		sampleA.loopStart = 0.f;
		sampleA.loopEnd = 1.f;
		sampleB.loopStart = 0.f;
		sampleB.loopEnd = 1.f;

		// Reset playheads
		loopA.playhead = 0.0;
		loopA.sleeping = false;
		loopA.sleepRemaining = 0.f;
		loopA.envelope = 1.f;
		loopA.ramping = false;
		loopA.jumpTarget = -1.0;
		loopA.rotationOffset = 0.0;

		loopB.playhead = 0.0;
		loopB.sleeping = false;
		loopB.sleepRemaining = 0.f;
		loopB.envelope = 1.f;
		loopB.ramping = false;
		loopB.jumpTarget = -1.0;
		loopB.rotationOffset = 0.0;

		// Reset recording state
		recA.phase = RecState::IDLE;
		recA.armed = false;
		recA.writePos = 0;
		recA.envelope = 0.f;
		recA.pendingFinalize = false;
		recA.recordedLength = 0;

		recB.phase = RecState::IDLE;
		recB.armed = false;
		recB.writePos = 0;
		recB.envelope = 0.f;
		recB.pendingFinalize = false;
		recB.recordedLength = 0;

		// Reset state
		playing = false;
		transientSensitivity = 0.7f;
		transientMinGapMs = 100.f;
		vcaMode = true;
		recordAppend = false;
		recLink = false;
		persistRecordedBuffers = true;
	}

	// --- Sample loading ---

	void computeWaveformMini(SampleData& sd) {
		sd.waveformMini.resize(WAVEFORM_POINTS, 0.f);
		if (sd.length == 0) return;

		for (int i = 0; i < WAVEFORM_POINTS; i++) {
			size_t start = (size_t)((double)i / WAVEFORM_POINTS * sd.length);
			size_t end = (size_t)((double)(i + 1) / WAVEFORM_POINTS * sd.length);
			if (end > sd.length) end = sd.length;
			float peak = 0.f;
			for (size_t j = start; j < end; j++) {
				float v = std::fabs(sampleToFloat(sd.samples[j]));
				if (v > peak) peak = v;
			}
			sd.waveformMini[i] = peak;
		}
	}

	void detectTransients(SampleData& sd) {
		sd.transients.clear();
		if (sd.length < 1024) return;

		// Adaptive onset detection using spectral flux with high-frequency emphasis
		// Uses multiple window sizes for better detection across different content types

		// Parameters derived from sensitivity (0 = most sensitive, 1 = least)
		// Map sensitivity to threshold: 0.0 -> 1.0 (sensitive), 1.0 -> 12.0 (only big transients)
		float threshold = 1.0f + transientSensitivity * 11.0f;
		int minGapSamples = (int)(transientMinGapMs * 48.f); // ms to samples at 48kHz

		// Window size balances temporal resolution vs noise rejection
		int windowSize = 1024;
		int hopSize = 256;

		int numFrames = ((int)sd.length - windowSize) / hopSize + 1;
		if (numFrames <= 2) return;

		// Compute energy with high-frequency emphasis (simple differencing as high-pass)
		std::vector<float> energy(numFrames, 0.f);
		for (int i = 0; i < numFrames; i++) {
			size_t offset = (size_t)(i * hopSize);
			float sum = 0.f;
			float hfSum = 0.f;
			for (int j = 0; j < windowSize && offset + j < sd.length; j++) {
				float v = sampleToFloat(sd.samples[offset + j]);
				sum += v * v;
				// High-frequency energy via sample differencing
				if (j > 0) {
					float diff = v - sampleToFloat(sd.samples[offset + j - 1]);
					hfSum += diff * diff;
				}
			}
			// Blend broadband and high-frequency energy (HF helps detect transients in noise)
			energy[i] = std::sqrt(sum / windowSize) + 0.5f * std::sqrt(hfSum / windowSize);
		}

		// Compute onset detection function: half-wave rectified first-order difference
		std::vector<float> odf(numFrames, 0.f);
		for (int i = 1; i < numFrames; i++) {
			float diff = energy[i] - energy[i - 1];
			odf[i] = std::max(0.f, diff);
		}

		// Adaptive threshold: local context window for comparison
		int medianWindow = 30; // wider context for better noise rejection
		size_t lastTransient = 0;

		for (int i = 1; i < numFrames; i++) {
			if (odf[i] <= 0.f) continue;

			// Compute local statistics (mean and peak of ODF in context window)
			float localSum = 0.f;
			float localMax = 0.f;
			int count = 0;
			for (int j = std::max(0, i - medianWindow); j <= std::min(numFrames - 1, i + medianWindow); j++) {
				localSum += odf[j];
				if (odf[j] > localMax) localMax = odf[j];
				count++;
			}
			float localMean = localSum / (float)count;

			// Onset if exceeds threshold * local mean AND is a local peak
			bool isPeak = true;
			for (int j = std::max(1, i - 2); j <= std::min(numFrames - 1, i + 2); j++) {
				if (j != i && odf[j] > odf[i]) {
					isPeak = false;
					break;
				}
			}

			// Require both relative threshold AND absolute minimum energy
		// The absolute floor prevents noise in quiet passages from triggering
		if (isPeak && odf[i] > localMean * threshold && energy[i] > 0.005f) {
				size_t pos = (size_t)(i * hopSize);
				if (sd.transients.empty() || (pos - lastTransient >= (size_t)minGapSamples)) {
					sd.transients.push_back(pos);
					lastTransient = pos;
				}
			}
		}
	}

	void clearSample(SampleData& sd) {
		// Mark as unloaded FIRST so the audio thread skips reads of sd.samples
		// while we mutate it. Wait one audio buffer's worth (~5ms) for the
		// audio thread to see the flag before we reallocate.
		sd.loaded.store(false);
#ifndef METAMODULE
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
#endif
		sd.samples.clear();
		sd.length = 0;
		sd.filePath.clear();
		sd.fileName.clear();
		sd.transients.clear();
		sd.waveformMini.clear();
		sd.loopStart = 0.f;
		sd.loopEnd = 1.f;
		sd.hasCuePoints = false;
	}

	void loadSample(SampleData& sd, const std::string& path) {
		drwav wav;
		// Open with metadata to read cue points
		if (!drwav_init_file_with_metadata(&wav, path.c_str(), 0, NULL))
			return;

		size_t totalFrames = wav.totalPCMFrameCount;
		uint32_t channels = wav.channels;
		uint32_t sampleRate = wav.sampleRate;
		uint32_t bitsPerSample = wav.bitsPerSample;

		if (totalFrames == 0) {
			drwav_uninit(&wav);
			return;
		}

		// Extract cue points from metadata before reading audio
		std::vector<size_t> cuePositions;
		for (drwav_uint32 m = 0; m < wav.metadataCount; m++) {
			if (wav.pMetadata[m].type == drwav_metadata_type_cue) {
				drwav_cue* cue = &wav.pMetadata[m].data.cue;
				for (drwav_uint32 c = 0; c < cue->cuePointCount; c++) {
					// dr_wav's sampleByteOffset is the raw dwSampleOffset, a SAMPLE-FRAME
					// offset (despite the name) — use it directly so markers match other apps
					uint32_t bytesPerFrame = channels * (bitsPerSample / 8);
					if (bytesPerFrame > 0) {
						size_t framePos = (size_t)cue->pCuePoints[c].sampleByteOffset;
						if (framePos < totalFrames) {
							cuePositions.push_back(framePos);
						}
					}
				}
			}
		}
		// Sort cue positions
		std::sort(cuePositions.begin(), cuePositions.end());

		// Read interleaved samples
		std::vector<float> raw(totalFrames * channels);
		drwav_read_pcm_frames_f32(&wav, totalFrames, raw.data());
		drwav_uninit(&wav);

		// Mix down to mono
		std::vector<float> mono(totalFrames);
		for (size_t i = 0; i < totalFrames; i++) {
			float sum = 0.f;
			for (uint32_t ch = 0; ch < channels; ch++) {
				sum += raw[i * channels + ch];
			}
			mono[i] = sum / (float)channels;
		}

		// Resample to 48kHz if needed
		double resampleRatio = 1.0;
		if (sampleRate != 48000 && sampleRate > 0) {
			resampleRatio = 48000.0 / (double)sampleRate;
			size_t newLength = (size_t)(totalFrames * resampleRatio);
			if (newLength > MAX_SAMPLE_LENGTH) newLength = MAX_SAMPLE_LENGTH;

			std::vector<float> resampled(newLength);
			for (size_t i = 0; i < newLength; i++) {
				double srcPos = (double)i / resampleRatio;
				size_t idx = (size_t)srcPos;
				float frac = (float)(srcPos - idx);
				if (idx + 1 < totalFrames) {
					resampled[i] = mono[idx] * (1.f - frac) + mono[idx + 1] * frac;
				} else if (idx < totalFrames) {
					resampled[i] = mono[idx];
				}
			}
			mono = std::move(resampled);

			// Adjust cue positions for resampling
			for (size_t& pos : cuePositions) {
				pos = (size_t)((double)pos * resampleRatio);
			}
		}

		// Enforce max length
		if (mono.size() > MAX_SAMPLE_LENGTH) {
			mono.resize(MAX_SAMPLE_LENGTH);
		}

		// Mark as unloaded so the audio thread skips reads of sd.samples,
		// then wait one audio buffer's worth (~5ms) before reallocating.
		sd.loaded.store(false);
#ifndef METAMODULE
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
#endif

#ifdef METAMODULE
		// Convert decoded float samples to int16 storage.
		sd.samples.resize(mono.size());
		for (size_t i = 0; i < mono.size(); i++) sd.samples[i] = floatToSample(mono[i]);
#else
		sd.samples = std::move(mono);
#endif
		sd.length = sd.samples.size();
		sd.filePath = path;
		sd.fileName = system::getFilename(path);
		sd.loopStart = 0.f;
		sd.loopEnd = 1.f;

		computeWaveformMini(sd);

		// Use cue points if present, otherwise auto-detect transients
		if (!cuePositions.empty()) {
			// Filter out any cue positions beyond sample length
			sd.transients.clear();
			for (size_t pos : cuePositions) {
				if (pos < sd.length) {
					sd.transients.push_back(pos);
				}
			}
			sd.hasCuePoints = true;
		} else {
			sd.hasCuePoints = false;
			detectTransients(sd);
		}

		// All metadata is now in place; let the audio thread read it again.
		sd.loaded.store(true);
	}

	void loadSampleFromPath(const std::string& path, bool isB) {
		if (isB) {
			loadSample(sampleB, path);
			sampleBExplicitlyLoaded = true;
		} else {
			loadSample(sampleA, path);
			if (!sampleBExplicitlyLoaded) {
				loadSample(sampleB, path);
			}
		}
	}

#ifdef METAMODULE
	void loadSampleDialog(bool isB) {
		std::string dir = "";
		if (isB && sampleB.loaded) {
			dir = system::getDirectory(sampleB.filePath);
		} else if (!isB && sampleA.loaded) {
			dir = system::getDirectory(sampleA.filePath);
		}
		osdialog_filters* filters = osdialog_filters_parse(PHASE_WAV_FILTERS);
		async_osdialog_file(OSDIALOG_OPEN,
			dir.empty() ? NULL : dir.c_str(),
			NULL,
			filters,
			[this, isB, filters](char* pathC) {
				if (pathC) {
					std::string path = pathC;
					std::free(pathC);
					loadSampleFromPath(path, isB);
				}
				osdialog_filters_free(filters);
			}
		);
	}
#else
	void loadSampleDialog(bool isB) {
		osdialog_filters* filters = osdialog_filters_parse(PHASE_WAV_FILTERS);
		DEFER({ osdialog_filters_free(filters); });

		std::string dir = "";
		if (isB && sampleB.loaded) {
			dir = system::getDirectory(sampleB.filePath);
		} else if (!isB && sampleA.loaded) {
			dir = system::getDirectory(sampleA.filePath);
		}

		char* pathC = osdialog_file(OSDIALOG_OPEN, dir.empty() ? NULL : dir.c_str(), NULL, filters);
		if (!pathC) return;

		std::string path = pathC;
		std::free(pathC);

		loadSampleFromPath(path, isB);
	}
#endif

	// --- Live recording ---

	// Returns true if the loop is currently recording (any phase except IDLE).
	// While recording, the loop's normal playback is suppressed and a dry
	// monitor passthrough is sent to the output instead.
	bool isRecording(const RecState& rec) const {
		return rec.phase != RecState::IDLE;
	}

	// Run one sample of the recording state machine for a single buffer.
	// `audioInput` is the already-normalled audio sample (in volts, ±5V scale).
	// `wantRecording` is the desired recording state from button + gate logic.
	// If `appendMode` is true and the buffer already holds a sample, the new
	// recording is appended to the end of the existing audio rather than
	// replacing it.
	void processRecording(RecState& rec, SampleData& sd, float audioInput, bool wantRecording,
	                      bool appendMode, float sampleTime) {
		// 1ms ramp = 48 samples at 48kHz
		float rampRate = sampleTime / 0.001f;

		// State transitions
		if (wantRecording && rec.phase == RecState::IDLE) {
			rec.phase = RecState::ARMING;
			rec.envelope = 0.f;

			// Set initial writePos based on mode + existing content
			size_t startPos = 0;
			if (appendMode && sd.loaded && sd.length > 0) {
				// Copy existing audio into the front of recBuffer so we
				// continue writing past it. Truncated to MAX_REC_LENGTH.
				size_t copyLen = std::min(sd.length, rec.recBuffer.size());
				std::copy(sd.samples.begin(), sd.samples.begin() + copyLen,
				          rec.recBuffer.begin());
				startPos = copyLen;
			}
			rec.writePos.store(startPos);
		}
		else if (!wantRecording && rec.phase == RecState::ACTIVE) {
			rec.phase = RecState::FINISHING;
		}

		if (rec.phase == RecState::IDLE) return;

		// Convert from ±5V audio scale to ±1.0 sample value, apply fade envelope
		float sampleValue = (audioInput / 5.f) * rec.envelope;

		// Write to record buffer (float → PhaseSampleT, which is int16 on MM)
		size_t pos = rec.writePos.load();
		if (pos < rec.recBuffer.size()) {
			rec.recBuffer[pos] = floatToSample(sampleValue);
			rec.writePos.store(pos + 1);
		} else {
			// Buffer full, force end. Skip to FINISHING.
			rec.phase = RecState::FINISHING;
		}

		// Update envelope
		if (rec.phase == RecState::ARMING) {
			rec.envelope += rampRate;
			if (rec.envelope >= 1.f) {
				rec.envelope = 1.f;
				rec.phase = RecState::ACTIVE;
			}
		}
		else if (rec.phase == RecState::FINISHING) {
			rec.envelope -= rampRate;
			if (rec.envelope <= 0.f) {
				rec.envelope = 0.f;
				// Hand off to the GUI thread for finalization.
				rec.recordedLength = rec.writePos.load();
				rec.pendingFinalize.store(true);
				rec.phase = RecState::IDLE;
				rec.armed = false; // reset latch so the LED reflects state
			}
		}
	}

	// Finalize a completed recording: copy the captured audio out of the
	// pre-allocated record buffer into the playable sample, run waveform and
	// transient analysis, and reset the loop region.
	// Called from PhaseWidget::step() on the GUI thread to avoid audio glitches.
	void finalizeRecording(SampleData& sd, RecState& rec) {
		if (rec.recordedLength == 0) {
			rec.recordedLength = 0;
			return;
		}

		// Mark as unloaded so the audio thread skips reads of sd.samples,
		// then wait one audio buffer's worth (~5ms) before reallocating.
		sd.loaded.store(false);
#ifndef METAMODULE
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
#endif

		sd.samples.assign(rec.recBuffer.begin(),
		                  rec.recBuffer.begin() + rec.recordedLength);
		sd.length = rec.recordedLength;
		sd.filePath.clear();
		sd.fileName = "Recorded";
		sd.loopStart = 0.f;
		sd.loopEnd = 1.f;
		sd.hasCuePoints = false;

		computeWaveformMini(sd);
		detectTransients(sd);

		// Now safe for audio thread to read again
		sd.loaded.store(true);

		rec.recordedLength = 0;
	}

	// Adopt an existing in-memory float buffer as a sample. Used for restoring
	// recorded buffers from the patch JSON. Runs on the GUI thread.
	void adoptSampleBuffer(SampleData& sd, std::vector<float>&& buf, bool fromRecording) {
		sd.loaded.store(false);
#ifndef METAMODULE
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
#endif
#ifdef METAMODULE
		// Convert incoming float buffer to int16 storage.
		size_t n = std::min(buf.size(), MAX_SAMPLE_LENGTH);
		sd.samples.resize(n);
		for (size_t i = 0; i < n; i++) sd.samples[i] = floatToSample(buf[i]);
#else
		sd.samples = std::move(buf);
		if (sd.samples.size() > MAX_SAMPLE_LENGTH) sd.samples.resize(MAX_SAMPLE_LENGTH);
#endif
		sd.length = sd.samples.size();
		sd.loopStart = 0.f;
		sd.loopEnd = 1.f;
		sd.hasCuePoints = false;
		if (fromRecording) {
			sd.filePath.clear();
			sd.fileName = "Recorded";
		}
		computeWaveformMini(sd);
		detectTransients(sd);
		sd.loaded.store(true);
	}

	// Save both buffers as a single stereo WAV: A on the left channel, B on
	// the right. The shorter buffer is zero-padded so they line up.
	bool saveStereoWav(const std::string& path) {
		size_t length = std::max(sampleA.length, sampleB.length);
		if (length == 0) return false;

		drwav_data_format format = {};
		format.container = drwav_container_riff;
		format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
		format.channels = 2;
		format.sampleRate = 48000;
		format.bitsPerSample = 32;

		drwav wav;
		if (!drwav_init_file_write(&wav, path.c_str(), &format, NULL))
			return false;

		// Interleave A and B in chunks to keep memory use bounded.
		const size_t CHUNK = 4096;
		std::vector<float> interleaved(CHUNK * 2);
		for (size_t i = 0; i < length; i += CHUNK) {
			size_t n = std::min(CHUNK, length - i);
			for (size_t j = 0; j < n; j++) {
				size_t idx = i + j;
				interleaved[j * 2]     = (idx < sampleA.length) ? sampleToFloat(sampleA.samples[idx]) : 0.f;
				interleaved[j * 2 + 1] = (idx < sampleB.length) ? sampleToFloat(sampleB.samples[idx]) : 0.f;
			}
			drwav_write_pcm_frames(&wav, n, interleaved.data());
		}
		drwav_uninit(&wav);
		return true;
	}

	static std::string ensureWavExt(std::string path) {
		if (path.size() < 4 ||
		    !(path[path.size()-4]=='.' && path[path.size()-3]=='w' &&
		      path[path.size()-2]=='a' && path[path.size()-1]=='v')) {
			path += ".wav";
		}
		return path;
	}

#ifdef METAMODULE
	void saveStereoWavDialog() {
		osdialog_filters* filters = osdialog_filters_parse(PHASE_WAV_FILTERS);
		async_osdialog_file(OSDIALOG_SAVE, NULL, "phase-buffers.wav", filters,
			[this, filters](char* pathC) {
				if (pathC) {
					std::string path = ensureWavExt(pathC);
					std::free(pathC);
					saveStereoWav(path);
				}
				osdialog_filters_free(filters);
			}
		);
	}
#else
	void saveStereoWavDialog() {
		osdialog_filters* filters = osdialog_filters_parse(PHASE_WAV_FILTERS);
		DEFER({ osdialog_filters_free(filters); });
		char* pathC = osdialog_file(OSDIALOG_SAVE, NULL, "phase-buffers.wav", filters);
		if (!pathC) return;
		std::string path = ensureWavExt(pathC);
		std::free(pathC);
		saveStereoWav(path);
	}
#endif

	void redetectTransients() {
		if (sampleA.loaded) {
			detectTransients(sampleA);
			sampleA.hasCuePoints = false; // override cues with auto-detection
		}
		if (sampleB.loaded) {
			detectTransients(sampleB);
			sampleB.hasCuePoints = false;
		}
	}

	// --- JSON persistence ---

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		// Persist samples in one of two ways:
		//  - File-loaded: store the file path. Reload from disk on patch open.
		//  - Recorded: embed the raw float bytes as base64. Bigger patch files,
		//    but the recording survives across sessions without leaving stray
		//    files on the user's disk.
		auto persistBuffer = [&](const SampleData& sd, const char* pathKey, const char* dataKey) {
			if (!sd.loaded || sd.length == 0) return;
			if (!sd.filePath.empty()) {
				json_object_set_new(rootJ, pathKey, json_string(sd.filePath.c_str()));
			} else if (persistRecordedBuffers) {
#ifdef METAMODULE
				// Convert int16 → float bytes so the patch JSON format stays
				// cross-compatible with VCV Rack. Otherwise a patch recorded
				// on MetaModule wouldn't load in VCV and vice-versa.
				std::vector<float> tmp(sd.length);
				for (size_t i = 0; i < sd.length; i++) tmp[i] = sampleToFloat(sd.samples[i]);
				size_t numBytes = tmp.size() * sizeof(float);
				std::string b64 = base64Encode(reinterpret_cast<const uint8_t*>(tmp.data()), numBytes);
#else
				size_t numBytes = sd.length * sizeof(float);
				std::string b64 = base64Encode(reinterpret_cast<const uint8_t*>(sd.samples.data()), numBytes);
#endif
				json_object_set_new(rootJ, dataKey, json_string(b64.c_str()));
			}
		};
		persistBuffer(sampleA, "sampleAPath", "sampleAData");
		persistBuffer(sampleB, "sampleBPath", "sampleBData");
		json_object_set_new(rootJ, "sampleBExplicit", json_boolean(sampleBExplicitlyLoaded));
		json_object_set_new(rootJ, "playing", json_boolean(playing));
		json_object_set_new(rootJ, "transientSensitivity", json_real(transientSensitivity));
		json_object_set_new(rootJ, "transientMinGapMs", json_real(transientMinGapMs));
		json_object_set_new(rootJ, "vcaMode", json_boolean(vcaMode));
		json_object_set_new(rootJ, "recordAppend", json_boolean(recordAppend));
		json_object_set_new(rootJ, "recLink", json_boolean(recLink));
		json_object_set_new(rootJ, "persistRecordedBuffers", json_boolean(persistRecordedBuffers));

		// Loop regions
		json_object_set_new(rootJ, "loopStartA", json_real(sampleA.loopStart));
		json_object_set_new(rootJ, "loopEndA", json_real(sampleA.loopEnd));
		json_object_set_new(rootJ, "loopStartB", json_real(sampleB.loopStart));
		json_object_set_new(rootJ, "loopEndB", json_real(sampleB.loopEnd));

		// Manually-edited cue points (override the file-derived transients on
		// reload). Only persisted once the user has edited them.
		auto saveCues = [&](SampleData& sd, const char* flagKey, const char* arrKey) {
			if (!sd.cuesEdited) return;
			json_object_set_new(rootJ, flagKey, json_boolean(true));
			json_t* arr = json_array();
			std::lock_guard<std::mutex> lk(cueMutex);
			for (size_t t : sd.transients)
				json_array_append_new(arr, json_integer((json_int_t)t));
			json_object_set_new(rootJ, arrKey, arr);
		};
		saveCues(sampleA, "cuesEditedA", "cuesA");
		saveCues(sampleB, "cuesEditedB", "cuesB");

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* pathAJ = json_object_get(rootJ, "sampleAPath");
		json_t* pathBJ = json_object_get(rootJ, "sampleBPath");
		json_t* explicitJ = json_object_get(rootJ, "sampleBExplicit");
		json_t* playingJ = json_object_get(rootJ, "playing");
		json_t* sensJ = json_object_get(rootJ, "transientSensitivity");
		json_t* gapJ = json_object_get(rootJ, "transientMinGapMs");

		if (explicitJ)
			sampleBExplicitlyLoaded = json_boolean_value(explicitJ);
		if (playingJ)
			playing = json_boolean_value(playingJ);
		if (sensJ)
			transientSensitivity = (float)json_real_value(sensJ);
		if (gapJ)
			transientMinGapMs = (float)json_real_value(gapJ);
		json_t* vcaJ = json_object_get(rootJ, "vcaMode");
		if (vcaJ)
			vcaMode = json_boolean_value(vcaJ);
		json_t* appendJ = json_object_get(rootJ, "recordAppend");
		if (appendJ)
			recordAppend = json_boolean_value(appendJ);
		json_t* linkJ = json_object_get(rootJ, "recLink");
		if (linkJ)
			recLink = json_boolean_value(linkJ);
		json_t* persistJ = json_object_get(rootJ, "persistRecordedBuffers");
		if (persistJ)
			persistRecordedBuffers = json_boolean_value(persistJ);

		// Restore embedded recorded-buffer data (preferred over path: a file
		// load would clobber the recorded audio if both somehow appeared).
		auto restoreEmbedded = [&](SampleData& sd, const char* dataKey) -> bool {
			json_t* dataJ = json_object_get(rootJ, dataKey);
			if (!dataJ) return false;
			std::string b64 = json_string_value(dataJ);
			std::vector<uint8_t> bytes = base64Decode(b64);
			size_t numFloats = bytes.size() / sizeof(float);
			if (numFloats == 0) return false;
			std::vector<float> samples(numFloats);
			std::memcpy(samples.data(), bytes.data(), numFloats * sizeof(float));
			adoptSampleBuffer(sd, std::move(samples), true);
			return true;
		};
		bool aRestored = restoreEmbedded(sampleA, "sampleAData");
		bool bRestored = restoreEmbedded(sampleB, "sampleBData");

		if (!aRestored && pathAJ) {
			std::string path = json_string_value(pathAJ);
#ifdef METAMODULE
			path = MetaModule::Filesystem::translate_path_to_local(path, MetaModule::Patch::get_dir());
#endif
			loadSample(sampleA, path);
		}

		if (!bRestored) {
			if (pathBJ) {
				std::string path = json_string_value(pathBJ);
#ifdef METAMODULE
				path = MetaModule::Filesystem::translate_path_to_local(path, MetaModule::Patch::get_dir());
#endif
				loadSample(sampleB, path);
			} else if (sampleA.loaded && !sampleBExplicitlyLoaded && !sampleA.filePath.empty()) {
				loadSample(sampleB, sampleA.filePath);
			}
		}

		// Restore loop regions
		json_t* lsA = json_object_get(rootJ, "loopStartA");
		json_t* leA = json_object_get(rootJ, "loopEndA");
		json_t* lsB = json_object_get(rootJ, "loopStartB");
		json_t* leB = json_object_get(rootJ, "loopEndB");
		if (lsA) sampleA.loopStart = (float)json_real_value(lsA);
		if (leA) sampleA.loopEnd = (float)json_real_value(leA);
		if (lsB) sampleB.loopStart = (float)json_real_value(lsB);
		if (leB) sampleB.loopEnd = (float)json_real_value(leB);

		// Restore manually-edited cues, overriding the file-derived transients
		// that loadSample() just produced above.
		auto restoreCues = [&](SampleData& sd, const char* flagKey, const char* arrKey) {
			json_t* flagJ = json_object_get(rootJ, flagKey);
			if (!flagJ || !json_boolean_value(flagJ)) return;
			json_t* arr = json_object_get(rootJ, arrKey);
			std::lock_guard<std::mutex> lk(cueMutex);
			sd.transients.clear();
			if (arr) {
				size_t n = json_array_size(arr);
				for (size_t i = 0; i < n; i++) {
					size_t t = (size_t)json_integer_value(json_array_get(arr, i));
					if (t < sd.length) sd.transients.push_back(t);
				}
			}
			std::sort(sd.transients.begin(), sd.transients.end());
			sd.cuesEdited = true;
			sd.hasCuePoints = !sd.transients.empty();
		};
		restoreCues(sampleA, "cuesEditedA", "cuesA");
		restoreCues(sampleB, "cuesEditedB", "cuesB");
	}

	// --- DSP ---

	float processLoop(LoopState& loop, SampleData& sd,
	                   float sleepMs, float speed, float pan,
	                   bool rotateMode, float sampleTime,
	                   float& leftOut, float& rightOut,
	                   bool recording = false, float monitorAudio = 0.f) {
		// While recording, suppress loop playback and pass through the dry
		// monitor signal at the buffer's current pan position. This lets the
		// user hear what they're tracking without any feedback risk.
		if (recording) {
			float panNorm = (pan + 1.f) * 0.5f;
			float leftGain = std::cos(panNorm * M_PI * 0.5f);
			float rightGain = std::sin(panNorm * M_PI * 0.5f);
			leftOut += monitorAudio * leftGain;
			rightOut += monitorAudio * rightGain;
			return monitorAudio;
		}

		if (!sd.loaded || sd.length == 0) return 0.f;

		// VCA mode envelope: 1ms ramp = 48 samples at 48kHz
		float rampRate = sampleTime / 0.001f; // 0->1 in 1ms

		// Process envelope ramping
		if (vcaMode && loop.ramping) {
			if (loop.jumpTarget >= 0.0) {
				// Fading out before jump
				loop.envelope -= rampRate;
				if (loop.envelope <= 0.f) {
					loop.envelope = 0.f;
					// Execute the jump
					loop.playhead = loop.jumpTarget;
					loop.sleeping = false;
					loop.sleepRemaining = 0.f;
					loop.jumpTarget = -1.0; // clear target, now fade in
				}
			} else {
				// Fading back in after jump
				loop.envelope += rampRate;
				if (loop.envelope >= 1.f) {
					loop.envelope = 1.f;
					loop.ramping = false;
				}
			}
		}

		// Compute actual loop region in samples
		size_t regionStart = (size_t)(sd.loopStart * sd.length);
		size_t regionEnd = (size_t)(sd.loopEnd * sd.length);
		if (regionEnd <= regionStart) regionEnd = regionStart + 1;
		if (regionEnd > sd.length) regionEnd = sd.length;
		size_t regionLength = regionEnd - regionStart;

		float sample = 0.f;

		if (rotateMode) {
			// --- ROTATE MODE ---
			// Continuous drift: the playhead advances normally within the
			// loop region, but the actual read position is offset by a
			// continuously growing rotationOffset. This means the content
			// gradually shifts relative to the loop boundaries — like two
			// tape machines running at slightly different speeds.
			//
			// rotationOffset accumulates driftPerSample each sample tick.
			// driftPerSample = sleepMs_in_samples / regionLength
			// e.g. 10ms sleep, 5s loop: 480/240000 = 0.002 extra per sample

			// Clamp playhead into region
			if (loop.playhead < (double)regionStart)
				loop.playhead = (double)regionStart;
			if (loop.playhead >= (double)regionEnd)
				loop.playhead = (double)regionStart;

			// Compute read position: playhead + rotationOffset, wrapped in region
			double relPlay = loop.playhead - (double)regionStart + loop.rotationOffset;
			double readRel = std::fmod(relPlay, (double)regionLength);
			if (readRel < 0.0) readRel += (double)regionLength;
			double readPos = (double)regionStart + readRel;

			// Read sample with linear interpolation
			size_t idx = (size_t)readPos;
			float frac = (float)(readPos - (double)idx);
			if (idx < sd.length) {
				float s0 = sampleToFloat(sd.samples[idx]);
				size_t idx1 = idx + 1;
				if (idx1 >= regionEnd) idx1 = regionStart;
				float s1 = (idx1 < sd.length) ? sampleToFloat(sd.samples[idx1]) : s0;
				sample = s0 + (s1 - s0) * frac;
			}

			// Advance playhead at base speed (signed, so negative speed
			// reverses the playhead direction in rotate mode too).
			loop.playhead += (double)speed;

			// Wrap playhead at loop boundary in either direction.
			if (loop.playhead >= (double)regionEnd) {
				loop.playhead = (double)regionStart + (loop.playhead - (double)regionEnd);
			} else if (loop.playhead < (double)regionStart) {
				loop.playhead = (double)regionEnd - ((double)regionStart - loop.playhead);
			}

			// Accumulate drift continuously (no discrete jumps)
			// Positive sleep = drift forward, negative = drift backward
			if (regionLength > 0 && sleepMs != 0.f) {
				double sleepSamples = (double)sleepMs * 48.0;
				double driftPerSample = sleepSamples / (double)regionLength;
				loop.rotationOffset += driftPerSample;
				// Wrap offset within region length
				loop.rotationOffset = std::fmod(loop.rotationOffset, (double)regionLength);
				if (loop.rotationOffset < 0.0) loop.rotationOffset += (double)regionLength;
			}

		} else {
			// --- SLEEP MODE ---
			if (loop.sleeping) {
				loop.sleepRemaining -= sampleTime;
				if (loop.sleepRemaining <= 0.f) {
					loop.sleeping = false;
					loop.playhead = (speed >= 0.f) ? (double)regionStart : (double)(regionEnd - 1);
					if (vcaMode) {
						loop.envelope = 0.f;
						loop.ramping = true;
						loop.jumpTarget = -1.0;
					}
				}
			} else {
				// Clamp playhead into region
				if (loop.playhead < (double)regionStart)
					loop.playhead = (double)regionStart;
				if (loop.playhead >= (double)regionEnd)
					loop.playhead = (double)(regionEnd - 1);

				// Read sample with linear interpolation
				size_t idx = (size_t)loop.playhead;
				float frac = (float)(loop.playhead - (double)idx);

				if (idx < sd.length) {
					float s0 = sampleToFloat(sd.samples[idx]);
					float s1 = (idx + 1 < sd.length) ? sampleToFloat(sd.samples[idx + 1]) : s0;
					sample = s0 + (s1 - s0) * frac;
				}

				// Advance playhead
				loop.playhead += (double)speed;

				// Compute effective loop boundary
				// Positive sleep: full region, then add silence
				// Negative sleep: shorten region by |sleepMs| samples, no silence
				// Zero: full region, no silence
				size_t effectiveEnd = regionEnd;
				size_t effectiveStart = regionStart;
				if (sleepMs < 0.f) {
					size_t cutSamples = (size_t)(std::fabs(sleepMs) * 48.f);
					if (cutSamples < regionLength - 1) {
						if (speed >= 0.f)
							effectiveEnd = regionEnd - cutSamples;
						else
							effectiveStart = regionStart + cutSamples;
					}
				}

				// Check for loop boundary (forward or reverse)
				bool loopWrapped = false;
				double wrapTarget = 0.0;

				if (speed >= 0.f && loop.playhead >= (double)effectiveEnd) {
					loopWrapped = true;
					wrapTarget = (double)effectiveStart;
				} else if (speed < 0.f && loop.playhead < (double)effectiveStart) {
					loopWrapped = true;
					wrapTarget = (double)(effectiveEnd - 1);
				}

				if (loopWrapped) {
					if (sleepMs > 0.f) {
						// Positive sleep: add silence after loop
						loop.sleeping = true;
						loop.sleepRemaining = sleepMs / 1000.f;
					} else if (vcaMode) {
						scheduleJump(loop, wrapTarget);
					} else {
						loop.playhead = wrapTarget;
					}
				}
			}
		}

		// Apply VCA envelope
		float env = vcaMode ? loop.envelope : 1.f;

		// Equal-power panning: pan [-1, +1] mapped to [0, 1]
		float panNorm = (pan + 1.f) * 0.5f;
		float leftGain = std::cos(panNorm * M_PI * 0.5f);
		float rightGain = std::sin(panNorm * M_PI * 0.5f);

		// Scale to VCV audio level (±5V)
		float output = sample * 5.f * env;
		leftOut += output * leftGain;
		rightOut += output * rightGain;

		return sample;
	}

	// Schedule a jump: in VCA mode, fade out first; otherwise instant
	void scheduleJump(LoopState& loop, double target) {
		if (vcaMode) {
			loop.jumpTarget = target;
			loop.ramping = true;
			// envelope will ramp down, then jump, then ramp up
		} else {
			loop.playhead = target;
			loop.sleeping = false;
			loop.sleepRemaining = 0.f;
		}
	}

	// Guards sd.transients against concurrent edits (UI thread) while the
	// audio thread reads them on clock edges. UI edits take a full lock; the
	// audio reader takes a try-lock and simply skips the jump if it's busy.
	std::mutex cueMutex;

	// --- Manual cue editing (UI thread) ---
	void addCue(SampleData& sd, size_t frame) {
		if (!sd.loaded || sd.length == 0) return;
		if (frame >= sd.length) frame = sd.length - 1;
		std::lock_guard<std::mutex> lk(cueMutex);
		sd.transients.push_back(frame);
		std::sort(sd.transients.begin(), sd.transients.end());
		sd.cuesEdited = true;
		sd.hasCuePoints = true;
	}
	void removeCue(SampleData& sd, int idx) {
		std::lock_guard<std::mutex> lk(cueMutex);
		if (idx >= 0 && idx < (int)sd.transients.size())
			sd.transients.erase(sd.transients.begin() + idx);
		sd.cuesEdited = true;
	}
	// Move one cue by index without re-sorting (called continuously during a
	// drag; sortCues() is called once on drag end to restore order).
	void moveCue(SampleData& sd, int idx, size_t frame) {
		if (sd.length == 0) return;
		if (frame >= sd.length) frame = sd.length - 1;
		std::lock_guard<std::mutex> lk(cueMutex);
		if (idx >= 0 && idx < (int)sd.transients.size())
			sd.transients[idx] = frame;
	}
	void sortCues(SampleData& sd) {
		std::lock_guard<std::mutex> lk(cueMutex);
		std::sort(sd.transients.begin(), sd.transients.end());
		sd.cuesEdited = true;
	}
	void clearCues(SampleData& sd) {
		std::lock_guard<std::mutex> lk(cueMutex);
		sd.transients.clear();
		sd.cuesEdited = true;
		sd.hasCuePoints = false;
	}

	double findNextTransient(const LoopState& loop, const SampleData& sd) {
		// Skip the jump rather than block audio if the UI is mid-edit.
		std::unique_lock<std::mutex> lk(cueMutex, std::try_to_lock);
		if (!lk.owns_lock()) return -1.0;
		if (sd.transients.empty()) return -1.0;

		size_t currentPos = (size_t)loop.playhead;
		size_t regionStart = (size_t)(sd.loopStart * sd.length);
		size_t regionEnd = (size_t)(sd.loopEnd * sd.length);

		// Find next transient after current position within loop region
		for (size_t t : sd.transients) {
			if (t > currentPos && t >= regionStart && t < regionEnd) {
				return (double)t;
			}
		}

		// Wrap: find first transient in loop region
		for (size_t t : sd.transients) {
			if (t >= regionStart && t < regionEnd) {
				return (double)t;
			}
		}

		return -1.0;
	}

	void jumpToNextTransient(LoopState& loop, const SampleData& sd) {
		double target = findNextTransient(loop, sd);
		if (target >= 0.0) {
			scheduleJump(loop, target);
		}
	}

	void process(const ProcessArgs& args) override {
		// Play button toggle
		if (params[PLAY_PARAM].getValue() > 0.f) {
			params[PLAY_PARAM].setValue(0.f);
			playing = !playing;
		}

		// Play gate CV: if connected, gate overrides button state
		bool isPlaying = playing;
		if (inputs[PLAY_INPUT].isConnected()) {
			isPlaying = inputs[PLAY_INPUT].getVoltage() >= 1.f;
		}

		// Update play LED
		lights[PLAY_LIGHT].setBrightness(isPlaying ? 1.f : 0.f);

		// --- Recording control (runs regardless of play state) ---
		// Recording must work whether or not the module is playing back.
		// LINK button is a latch: each click toggles the link state.
		if (params[REC_LINK_PARAM].getValue() > 0.f) {
			params[REC_LINK_PARAM].setValue(0.f);
			recLink = !recLink;
		}
		// Toggle armed state on momentary button press. When LINK is on,
		// pressing either REC button toggles both at once.
		if (params[REC_A_PARAM].getValue() > 0.f) {
			params[REC_A_PARAM].setValue(0.f);
			recA.armed = !recA.armed;
			if (recLink) recB.armed = recA.armed;
		}
		if (params[REC_B_PARAM].getValue() > 0.f) {
			params[REC_B_PARAM].setValue(0.f);
			recB.armed = !recB.armed;
			if (recLink) recA.armed = recB.armed;
		}

		// Audio in: REC B INPUT normalled from REC A INPUT (cascade)
		float recAudioA = inputs[REC_A_INPUT].getVoltage();
		float recAudioB = inputs[REC_B_INPUT].getNormalVoltage(recAudioA);

		// Gate logic: record if button is armed OR gate input is high.
		// Either source can independently drive recording. REC B GATE is
		// normalled from REC A GATE so a single gate cable cascades to both.
		bool wantRecA = recA.armed;
		if (inputs[REC_A_GATE_INPUT].isConnected()) {
			wantRecA = wantRecA || (inputs[REC_A_GATE_INPUT].getVoltage() >= 1.f);
		}
		bool wantRecB = recB.armed;
		if (inputs[REC_B_GATE_INPUT].isConnected()) {
			wantRecB = wantRecB || (inputs[REC_B_GATE_INPUT].getVoltage() >= 1.f);
		} else if (inputs[REC_A_GATE_INPUT].isConnected()) {
			// Normalled from REC A GATE
			wantRecB = wantRecB || (inputs[REC_A_GATE_INPUT].getVoltage() >= 1.f);
		}

		processRecording(recA, sampleA, recAudioA, wantRecA, recordAppend, args.sampleTime);
		processRecording(recB, sampleB, recAudioB, wantRecB, recordAppend, args.sampleTime);

		lights[REC_A_LIGHT].setBrightness(isRecording(recA) ? 1.f : (recA.armed ? 0.5f : 0.f));
		lights[REC_B_LIGHT].setBrightness(isRecording(recB) ? 1.f : (recB.armed ? 0.5f : 0.f));
		lights[REC_LINK_LIGHT].setBrightness(recLink ? 1.f : 0.f);

		// If not playing AND not recording on either buffer, output silence and skip the rest
		if (!isPlaying && !isRecording(recA) && !isRecording(recB)) {
			outputs[LEFT_OUTPUT].setVoltage(0.f);
			outputs[RIGHT_OUTPUT].setVoltage(0.f);
			return;
		}

		// Sync trigger - reset both loops (button or CV)
		bool syncTriggered = syncTrigger.process(inputs[SYNC_INPUT].getVoltage(), 0.1f, 1.f);
		if (params[SYNC_PARAM].getValue() > 0.f) {
			params[SYNC_PARAM].setValue(0.f);
			syncTriggered = true;
		}
		if (syncTriggered) {
			double startA = (double)(size_t)(sampleA.loopStart * sampleA.length);
			double startB = (double)(size_t)(sampleB.loopStart * sampleB.length);
			scheduleJump(loopA, startA);
			scheduleJump(loopB, startB);
		}

		// Clock triggers - jump to transient
		if (loopA.clockTrigger.process(inputs[CLOCK_A_INPUT].getVoltage(), 0.1f, 1.f)) {
			jumpToNextTransient(loopA, sampleA);
		}
		if (loopB.clockTrigger.process(inputs[CLOCK_B_INPUT].getVoltage(), 0.1f, 1.f)) {
			jumpToNextTransient(loopB, sampleB);
		}

		// Read parameters with CV modulation

		// Sleep A: -500 to +500ms, CV at 50ms/V
		float sleepA = params[SLEEP_A_PARAM].getValue();
		if (inputs[SLEEP_A_INPUT].isConnected())
			sleepA += inputs[SLEEP_A_INPUT].getVoltage() * 50.f;
		sleepA = clamp(sleepA, -500.f, 500.f);

		// Speed A: -4x to +4x, CV at 1.0x/V (so ±4V CV = ±4× exactly)
		float speedA = params[SPEED_A_PARAM].getValue();
		if (inputs[SPEED_A_INPUT].isConnected())
			speedA += inputs[SPEED_A_INPUT].getVoltage() * 1.0f;
		speedA = clamp(speedA, -4.f, 4.f);

		// Pan A: -1 to +1, CV at 0.2/V
		float panA = params[PAN_A_PARAM].getValue();
		if (inputs[PAN_A_INPUT].isConnected())
			panA += inputs[PAN_A_INPUT].getVoltage() * 0.2f;
		panA = clamp(panA, -1.f, 1.f);

		// Sleep B: -500 to +500ms, CV at 50ms/V
		float sleepB = params[SLEEP_B_PARAM].getValue();
		if (inputs[SLEEP_B_INPUT].isConnected())
			sleepB += inputs[SLEEP_B_INPUT].getVoltage() * 50.f;
		sleepB = clamp(sleepB, -500.f, 500.f);

		// Speed B: -4x to +4x, CV at 1.0x/V
		float speedB = params[SPEED_B_PARAM].getValue();
		if (inputs[SPEED_B_INPUT].isConnected())
			speedB += inputs[SPEED_B_INPUT].getVoltage() * 1.0f;
		speedB = clamp(speedB, -4.f, 4.f);

		// Pan B
		float panB = params[PAN_B_PARAM].getValue();
		if (inputs[PAN_B_INPUT].isConnected())
			panB += inputs[PAN_B_INPUT].getVoltage() * 0.2f;
		panB = clamp(panB, -1.f, 1.f);

		// Loop Start CV: 0-10V = 0-100% of sample
		// Loop Length CV: 0-10V = 0-100% of remaining sample (from start)
		if (inputs[START_A_INPUT].isConnected()) {
			float start = clamp(inputs[START_A_INPUT].getVoltage() / 10.f, 0.f, 0.99f);
			sampleA.loopStart = start;
			if (!inputs[END_A_INPUT].isConnected())
				sampleA.loopEnd = clamp(sampleA.loopEnd, start + 0.01f, 1.f);
		}
		if (inputs[END_A_INPUT].isConnected()) {
			float lengthNorm = clamp(inputs[END_A_INPUT].getVoltage() / 10.f, 0.01f, 1.f);
			float remaining = 1.f - sampleA.loopStart;
			sampleA.loopEnd = clamp(sampleA.loopStart + remaining * lengthNorm, sampleA.loopStart + 0.01f, 1.f);
		}
		if (inputs[START_B_INPUT].isConnected()) {
			float start = clamp(inputs[START_B_INPUT].getVoltage() / 10.f, 0.f, 0.99f);
			sampleB.loopStart = start;
			if (!inputs[END_B_INPUT].isConnected())
				sampleB.loopEnd = clamp(sampleB.loopEnd, start + 0.01f, 1.f);
		}
		if (inputs[END_B_INPUT].isConnected()) {
			float lengthNorm = clamp(inputs[END_B_INPUT].getVoltage() / 10.f, 0.01f, 1.f);
			float remaining = 1.f - sampleB.loopStart;
			sampleB.loopEnd = clamp(sampleB.loopStart + remaining * lengthNorm, sampleB.loopStart + 0.01f, 1.f);
		}

		// Process both loops
		float leftOut = 0.f;
		float rightOut = 0.f;

		bool rotateModeA = params[MODE_A_PARAM].getValue() < 0.5f;
		bool rotateModeB = params[MODE_B_PARAM].getValue() < 0.5f;

		// While recording, the monitor audio is the post-fade input value
		float monitorA = recAudioA * recA.envelope;
		float monitorB = recAudioB * recB.envelope;

		// If not playing but recording, run the loop function only for monitor
		// passthrough on the recording buffer. Don't advance/play the other buffer.
		if (isPlaying || isRecording(recA))
			processLoop(loopA, sampleA, sleepA, speedA, panA, rotateModeA, args.sampleTime, leftOut, rightOut,
			            isRecording(recA), monitorA);
		if (isPlaying || isRecording(recB))
			processLoop(loopB, sampleB, sleepB, speedB, panB, rotateModeB, args.sampleTime, leftOut, rightOut,
			            isRecording(recB), monitorB);

		// Clamp output
		outputs[LEFT_OUTPUT].setVoltage(clamp(leftOut, -10.f, 10.f));
		outputs[RIGHT_OUTPUT].setVoltage(clamp(rightOut, -10.f, 10.f));
	}
};


// --- Waveform display interaction (needs Phase to be fully defined) ---

PhaseWaveformDisplay::DragTarget PhaseWaveformDisplay::hitTestHandle(Vec pos) {
	if (!module) return NONE;

	float w = box.size.x;
	float halfH = box.size.y * 0.5f;
	float hitRadius = 6.f;

	// Test Sample A handles (top half)
	if (module->sampleA.loaded && pos.y < halfH) {
		float startPx = module->sampleA.loopStart * w;
		float endPx = module->sampleA.loopEnd * w;
		if (std::fabs(pos.x - startPx) < hitRadius) return LOOP_START_A;
		if (std::fabs(pos.x - endPx) < hitRadius) return LOOP_END_A;
	}

	// Test Sample B handles (bottom half)
	if (module->sampleB.loaded && pos.y >= halfH) {
		float startPx = module->sampleB.loopStart * w;
		float endPx = module->sampleB.loopEnd * w;
		if (std::fabs(pos.x - startPx) < hitRadius) return LOOP_START_B;
		if (std::fabs(pos.x - endPx) < hitRadius) return LOOP_END_B;
	}

	return NONE;
}

int PhaseWaveformDisplay::hitTestCue(Vec pos, bool& isB) {
	if (!module) return -1;
	float w = box.size.x;
	float halfH = box.size.y * 0.5f;
	float hitRadius = 4.f;
	isB = pos.y >= halfH;
	SampleData& sd = isB ? module->sampleB : module->sampleA;
	if (!sd.loaded || sd.length == 0) return -1;
	int best = -1;
	float bestDist = hitRadius;
	for (int i = 0; i < (int)sd.transients.size(); i++) {
		float px = (float)sd.transients[i] / (float)sd.length * w;
		float d = std::fabs(pos.x - px);
		if (d < bestDist) { bestDist = d; best = i; }
	}
	return best;
}

void PhaseWaveformDisplay::onButton(const ButtonEvent& e) {
	if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
		lastButtonPos = e.pos;
		// Loop handles take priority, then existing cues (for dragging).
		dragTarget = hitTestHandle(e.pos);
		dragCueIdx = -1;
		if (dragTarget == NONE) {
			bool isB = false;
			int cue = hitTestCue(e.pos, isB);
			if (cue >= 0) {
				dragTarget = isB ? CUE_B : CUE_A;
				dragCueIdx = cue;
			}
		}
		// Consume all left presses on the display so we also receive the
		// double-click that follows (add/remove cue). Right-click is left
		// untouched so VCV's context menu still opens.
		APP->event->setSelectedWidget(this);
		e.consume(this);
	}
	Widget::onButton(e);
}

void PhaseWaveformDisplay::onDoubleClick(const DoubleClickEvent& e) {
	if (!module) return;
	// Don't drop a cue when double-clicking a loop handle.
	if (hitTestHandle(lastButtonPos) != NONE) return;
	float w = box.size.x;
	float halfH = box.size.y * 0.5f;
	bool isB = lastButtonPos.y >= halfH;
	SampleData& sd = isB ? module->sampleB : module->sampleA;
	if (!sd.loaded || sd.length == 0) return;

	bool hitIsB = false;
	int cue = hitTestCue(lastButtonPos, hitIsB);
	if (cue >= 0 && hitIsB == isB) {
		module->removeCue(sd, cue);          // double-click a cue → remove it
	} else {
		float norm = clamp(lastButtonPos.x / w, 0.f, 1.f);
		module->addCue(sd, (size_t)(norm * sd.length));   // double-click empty → add
	}
}

void PhaseWaveformDisplay::onDragMove(const DragMoveEvent& e) {
	if (!module || dragTarget == NONE) return;

	float w = box.size.x;
	// Convert mouse delta to normalized position change, accounting for zoom
	float zoom = getAbsoluteZoom();
	float delta = e.mouseDelta.x / (w * zoom);

	switch (dragTarget) {
		case LOOP_START_A:
			module->sampleA.loopStart = clamp(module->sampleA.loopStart + delta, 0.f, module->sampleA.loopEnd - 0.01f);
			break;
		case LOOP_END_A:
			module->sampleA.loopEnd = clamp(module->sampleA.loopEnd + delta, module->sampleA.loopStart + 0.01f, 1.f);
			break;
		case LOOP_START_B:
			module->sampleB.loopStart = clamp(module->sampleB.loopStart + delta, 0.f, module->sampleB.loopEnd - 0.01f);
			break;
		case LOOP_END_B:
			module->sampleB.loopEnd = clamp(module->sampleB.loopEnd + delta, module->sampleB.loopStart + 0.01f, 1.f);
			break;
		case CUE_A: {
			SampleData& sd = module->sampleA;
			if (sd.length == 0 || dragCueIdx < 0 || dragCueIdx >= (int)sd.transients.size()) break;
			float cur = (float)sd.transients[dragCueIdx] / (float)sd.length;
			float n = clamp(cur + delta, 0.f, 1.f);
			module->moveCue(sd, dragCueIdx, (size_t)(n * sd.length));
			break;
		}
		case CUE_B: {
			SampleData& sd = module->sampleB;
			if (sd.length == 0 || dragCueIdx < 0 || dragCueIdx >= (int)sd.transients.size()) break;
			float cur = (float)sd.transients[dragCueIdx] / (float)sd.length;
			float n = clamp(cur + delta, 0.f, 1.f);
			module->moveCue(sd, dragCueIdx, (size_t)(n * sd.length));
			break;
		}
		default:
			break;
	}
}

void PhaseWaveformDisplay::onDragEnd(const DragEndEvent& e) {
	// Re-sort after a cue drag so findNextTransient() stays in order.
	if (dragTarget == CUE_A) module->sortCues(module->sampleA);
	else if (dragTarget == CUE_B) module->sortCues(module->sampleB);
	dragTarget = NONE;
	dragCueIdx = -1;
}


// --- Waveform display drawLayer implementation ---

// Build a peak-amplitude mini waveform from the live record buffer.
// Used to render the in-progress recording on the display. Stride-samples
// within each bin so it stays cheap to compute every frame.
static std::vector<float> buildLiveRecMini(const std::vector<PhaseSampleT>& buf, size_t writePos) {
	std::vector<float> mini(WAVEFORM_POINTS, 0.f);
	if (writePos == 0) return mini;
	for (int i = 0; i < WAVEFORM_POINTS; i++) {
		size_t binStart = (size_t)((double)i / WAVEFORM_POINTS * writePos);
		size_t binEnd = (size_t)((double)(i + 1) / WAVEFORM_POINTS * writePos);
		if (binEnd > writePos) binEnd = writePos;
		if (binEnd <= binStart) continue;
		size_t step = std::max((size_t)1, (binEnd - binStart) / 16);
		float peak = 0.f;
		for (size_t j = binStart; j < binEnd; j += step) {
			float v = std::fabs(sampleToFloat(buf[j]));
			if (v > peak) peak = v;
		}
		mini[i] = peak;
	}
	return mini;
}

void PhaseWaveformDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) {
		Widget::drawLayer(args, layer);
		return;
	}
	if (!module) {
		// Browser-preview render: synthesize two stylized mini waveforms (top =
		// blue, bottom = orange) with loop handles drawn at sensible positions
		// so the VCV Library screenshot shows what Phase looks like loaded.
		float w = box.size.x;
		float h = box.size.y;
		float halfH = h * 0.5f;
		NVGcolor colorA = nvgRGBA(100, 180, 255, 200);
		NVGcolor colorB = nvgRGBA(255, 140, 80, 200);
		NVGcolor handleColorA = nvgRGBA(200, 220, 255, 255);
		NVGcolor handleColorB = nvgRGBA(255, 200, 160, 255);

		// Synthesize amplitude envelopes: A = decaying drum-like, B = sustained pad-like
		const int N = 128;
		std::vector<float> miniA(N), miniB(N);
		for (int i = 0; i < N; i++) {
			float t = (float)i / (float)(N - 1);
			// A: percussive shape (fast attack, exp decay), with sub-detail
			miniA[i] = std::exp(-t * 4.f) * (0.4f + 0.5f * std::sin(t * 40.f));
			// B: sustained shape, slow envelope
			miniB[i] = (0.6f + 0.3f * std::sin(t * 18.f)) * std::sin(t * (float)M_PI);
		}
		drawWaveform(args, miniA, 0, 0,     w, halfH, 0.1f, 0.85f, colorA, 0.f);
		drawWaveform(args, miniB, 0, halfH, w, halfH, 0.05f, 0.9f, colorB, 0.f);
		// Loop handles
		drawHandle(args, 0.1f,  0, 0,     w, halfH, handleColorA, true);
		drawHandle(args, 0.85f, 0, 0,     w, halfH, handleColorA, false);
		drawHandle(args, 0.05f, 0, halfH, w, halfH, handleColorB, true);
		drawHandle(args, 0.9f,  0, halfH, w, halfH, handleColorB, false);
		// Playheads (mid-loop on each)
		NVGcolor playColor = nvgRGBA(255, 255, 255, 220);
		float pxA = 0.45f * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, pxA, 0); nvgLineTo(args.vg, pxA, halfH);
		nvgStrokeColor(args.vg, playColor); nvgStrokeWidth(args.vg, 1.5f); nvgStroke(args.vg);
		float pxB = 0.6f * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, pxB, halfH); nvgLineTo(args.vg, pxB, h);
		nvgStrokeColor(args.vg, playColor); nvgStrokeWidth(args.vg, 1.5f); nvgStroke(args.vg);
		return;
	}

	float w = box.size.x;
	float h = box.size.y;
	float halfH = h * 0.5f;

	NVGcolor colorA = nvgRGBA(100, 180, 255, 200);
	NVGcolor colorB = nvgRGBA(255, 140, 80, 200);
	NVGcolor transientColorA = nvgRGBA(150, 210, 255, 100);
	NVGcolor transientColorB = nvgRGBA(255, 180, 120, 100);
	NVGcolor playheadColor = nvgRGBA(255, 255, 255, 220);
	NVGcolor handleColorA = nvgRGBA(200, 220, 255, 255);
	NVGcolor handleColorB = nvgRGBA(255, 200, 160, 255);

	NVGcolor originColor = nvgRGBA(255, 255, 255, 50);
	// Bright red for live recording display
	NVGcolor recordingColor = nvgRGBA(255, 60, 60, 220);
	NVGcolor recordHeadColor = nvgRGBA(255, 80, 80, 255);

	// --- Live recording display takes precedence over loaded waveform ---
	bool recordingA = module->recA.phase != RecState::IDLE;
	bool recordingB = module->recB.phase != RecState::IDLE;

	if (recordingA) {
		size_t pos = module->recA.writePos.load();
		std::vector<float> liveMini = buildLiveRecMini(module->recA.recBuffer, pos);
		drawWaveform(args, liveMini, 0, 0, w, halfH, 0.f, 1.f, recordingColor, 0.f);
		// Recording playhead at current position (normalized to recBuffer capacity)
		float headNorm = (float)pos / (float)module->recA.recBuffer.size();
		float headPx = headNorm * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, headPx, 0);
		nvgLineTo(args.vg, headPx, halfH);
		nvgStrokeColor(args.vg, recordHeadColor);
		nvgStrokeWidth(args.vg, 1.5f);
		nvgStroke(args.vg);
	}

	if (recordingB) {
		size_t pos = module->recB.writePos.load();
		std::vector<float> liveMini = buildLiveRecMini(module->recB.recBuffer, pos);
		drawWaveform(args, liveMini, 0, halfH, w, halfH, 0.f, 1.f, recordingColor, 0.f);
		float headNorm = (float)pos / (float)module->recB.recBuffer.size();
		float headPx = headNorm * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, headPx, halfH);
		nvgLineTo(args.vg, headPx, h);
		nvgStrokeColor(args.vg, recordHeadColor);
		nvgStrokeWidth(args.vg, 1.5f);
		nvgStroke(args.vg);
	}

	// Draw waveform A (top half) — only if not recording
	if (!recordingA && module->sampleA.loaded) {
		// Compute rotation as normalized fraction of total sample
		float rotNormA = 0.f;
		if (module->params[Phase::MODE_A_PARAM].getValue() < 0.5f && module->sampleA.length > 0) {
			rotNormA = (float)(module->loopA.rotationOffset / (double)module->sampleA.length);
		}
		drawWaveform(args, module->sampleA.waveformMini, 0, 0, w, halfH,
		             module->sampleA.loopStart, module->sampleA.loopEnd, colorA, rotNormA);
		drawTransients(args, module->sampleA.transients, module->sampleA.length, 0, 0, w, halfH, transientColorA);

		// Origin line: in rotate mode, shows where the original loop start
		// appears in the rotated waveform display. The display shifts content
		// forward by rotOffset, so the original start appears at (regionLength - rotOffset).
		float originNorm = module->sampleA.loopStart;
		if (module->params[Phase::MODE_A_PARAM].getValue() < 0.5f && module->loopA.rotationOffset != 0.0) {
			size_t regionStart = (size_t)(module->sampleA.loopStart * module->sampleA.length);
			size_t regionEnd = (size_t)(module->sampleA.loopEnd * module->sampleA.length);
			size_t regionLength = regionEnd - regionStart;
			if (regionLength > 0) {
				double rotFraction = std::fmod(module->loopA.rotationOffset, (double)regionLength) / (double)regionLength;
				if (rotFraction < 0.0) rotFraction += 1.0;
				// Invert: original start is at (1 - rotFraction) in the display
				double originFraction = 1.0 - rotFraction;
				if (originFraction >= 1.0) originFraction -= 1.0;
				float regionWidth = module->sampleA.loopEnd - module->sampleA.loopStart;
				originNorm = module->sampleA.loopStart + (float)originFraction * regionWidth;
			}
		}
		float originPx = originNorm * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, originPx, 0);
		nvgLineTo(args.vg, originPx, halfH);
		nvgStrokeColor(args.vg, originColor);
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStroke(args.vg);

		drawPlayhead(args, module->loopA.playhead, module->sampleA.length, 0, 0, w, halfH, playheadColor);
		// Loop handles
		drawHandle(args, module->sampleA.loopStart, 0, 0, w, halfH, handleColorA, true);
		drawHandle(args, module->sampleA.loopEnd, 0, 0, w, halfH, handleColorA, false);
	}

	// Draw waveform B (bottom half) — only if not recording
	if (!recordingB && module->sampleB.loaded) {
		float rotNormB = 0.f;
		if (module->params[Phase::MODE_B_PARAM].getValue() < 0.5f && module->sampleB.length > 0) {
			rotNormB = (float)(module->loopB.rotationOffset / (double)module->sampleB.length);
		}
		drawWaveform(args, module->sampleB.waveformMini, 0, halfH, w, halfH,
		             module->sampleB.loopStart, module->sampleB.loopEnd, colorB, rotNormB);
		drawTransients(args, module->sampleB.transients, module->sampleB.length, 0, halfH, w, halfH, transientColorB);

		// Origin line for B
		float originNorm = module->sampleB.loopStart;
		if (module->params[Phase::MODE_B_PARAM].getValue() < 0.5f && module->loopB.rotationOffset != 0.0) {
			size_t regionStart = (size_t)(module->sampleB.loopStart * module->sampleB.length);
			size_t regionEnd = (size_t)(module->sampleB.loopEnd * module->sampleB.length);
			size_t regionLength = regionEnd - regionStart;
			if (regionLength > 0) {
				double rotFraction = std::fmod(module->loopB.rotationOffset, (double)regionLength) / (double)regionLength;
				if (rotFraction < 0.0) rotFraction += 1.0;
				double originFraction = 1.0 - rotFraction;
				if (originFraction >= 1.0) originFraction -= 1.0;
				float regionWidth = module->sampleB.loopEnd - module->sampleB.loopStart;
				originNorm = module->sampleB.loopStart + (float)originFraction * regionWidth;
			}
		}
		float originPx = originNorm * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, originPx, halfH);
		nvgLineTo(args.vg, originPx, h);
		nvgStrokeColor(args.vg, originColor);
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStroke(args.vg);

		drawPlayhead(args, module->loopB.playhead, module->sampleB.length, 0, halfH, w, halfH, playheadColor);
		// Loop handles
		drawHandle(args, module->sampleB.loopStart, 0, halfH, w, halfH, handleColorB, true);
		drawHandle(args, module->sampleB.loopEnd, 0, halfH, w, halfH, handleColorB, false);
	}

	Widget::drawLayer(args, layer);
}


// --- Widget ---

struct PhaseWidget : ModuleWidget {
	// GUI-thread tick: finalize any pending recordings off the audio thread.
	// The audio thread sets pendingFinalize when a recording completes (after
	// fade-out); we do the heavy lifting (vector copy, waveform mini, transient
	// detection) here so it doesn't glitch playback.
	void step() override {
		ModuleWidget::step();
		Phase* phase = dynamic_cast<Phase*>(this->module);
		if (!phase) return;
		if (phase->recA.pendingFinalize.load()) {
			phase->finalizeRecording(phase->sampleA, phase->recA);
			phase->recA.pendingFinalize.store(false);
		}
		if (phase->recB.pendingFinalize.load()) {
			phase->finalizeRecording(phase->sampleB, phase->recB);
			phase->recB.pendingFinalize.store(false);
			// Recording into B counts as an explicit load — don't let a
			// subsequent Sample A file load cascade-overwrite it.
			phase->sampleBExplicitlyLoaded = true;
		}
	}

	PhaseWidget(Phase* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/phase.svg")));


		// Waveform display
		PhaseWaveformDisplay* display = new PhaseWaveformDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(2.54f, 14.f));
		display->box.size = mm2px(Vec(91.44f, 24.f));
		addChild(display);

		// --- Loop A controls ---
		// Left side: CLK, START, LEN jacks (Y=53.34mm)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 53.34f)), module, Phase::CLOCK_A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 53.34f)), module, Phase::START_A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.48f, 53.34f)), module, Phase::END_A_INPUT));

		// Mode switch A (Y=53.34mm)
		addParam(createParamCentered<CKSS>(mm2px(Vec(43.18f, 53.34f)), module, Phase::MODE_A_PARAM));

		// Knobs: Drift, Speed, Pan (Y=53.34mm)
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(55.88f, 53.34f)), module, Phase::SLEEP_A_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(73.66f, 53.34f)), module, Phase::SPEED_A_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(88.9f, 53.34f)), module, Phase::PAN_A_PARAM));

		// CV inputs (Y=63.5mm)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.88f, 63.5f)), module, Phase::SLEEP_A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(73.66f, 63.5f)), module, Phase::SPEED_A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(88.9f, 63.5f)), module, Phase::PAN_A_INPUT));

		// --- Loop B controls ---
		// Left side: CLK, START, LEN jacks (Y=81.28mm)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 81.28f)), module, Phase::CLOCK_B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 81.28f)), module, Phase::START_B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.48f, 81.28f)), module, Phase::END_B_INPUT));

		// Mode switch B (Y=81.28mm)
		addParam(createParamCentered<CKSS>(mm2px(Vec(43.18f, 81.28f)), module, Phase::MODE_B_PARAM));

		// Knobs: Drift, Speed, Pan (Y=81.28mm)
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(55.88f, 81.28f)), module, Phase::SLEEP_B_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(73.66f, 81.28f)), module, Phase::SPEED_B_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(88.9f, 81.28f)), module, Phase::PAN_B_PARAM));

		// CV inputs (Y=91.44mm)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.88f, 91.44f)), module, Phase::SLEEP_B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(73.66f, 91.44f)), module, Phase::SPEED_B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(88.9f, 91.44f)), module, Phase::PAN_B_INPUT));

		// --- Bottom section ---
		// Play button + Sync button (Y=106.68mm)
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(10.16f, 106.68f)), module, Phase::PLAY_PARAM, Phase::PLAY_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(20.32f, 106.68f)), module, Phase::SYNC_PARAM));

		// Play gate + Sync CV (Y=116.84mm)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 116.84f)), module, Phase::PLAY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 116.84f)), module, Phase::SYNC_INPUT));

		// Record + link buttons (Y=106.68mm — same row as PLAY/SYNC).
		// Buttons sit above the record jack row: REC A over GATE A (left),
		// LINK over the pair of audio ins (middle), REC B over GATE B (right).
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
			mm2px(Vec(35.56f, 106.68f)), module, Phase::REC_A_PARAM, Phase::REC_A_LIGHT));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<YellowLight>>>(
			mm2px(Vec(50.79f, 106.68f)), module, Phase::REC_LINK_PARAM, Phase::REC_LINK_LIGHT));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
			mm2px(Vec(66.03f, 106.68f)), module, Phase::REC_B_PARAM, Phase::REC_B_LIGHT));

		// Record jacks (Y=116.84mm). New layout: gates on the outside,
		// audio inputs sandwiched in the middle.
		// Order (left → right): GATE A · A IN · B IN · GATE B
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.56f, 116.84f)), module, Phase::REC_A_GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(45.72f, 116.84f)), module, Phase::REC_A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.88f, 116.84f)), module, Phase::REC_B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.13f, 116.84f)), module, Phase::REC_B_GATE_INPUT));

		// Stereo outputs (Y=116.84mm)
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(78.74f, 116.84f)), module, Phase::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(88.9f, 116.84f)), module, Phase::RIGHT_OUTPUT));
	}

	// Drag-and-drop WAV files onto the module to load samples. Dropping two
	// files loads A and B; dropping one loads A (cascading to B), unless it's
	// dropped on the lower half of the waveform display, which targets B.
	void onPathDrop(const PathDropEvent& e) override {
		Phase* phase = dynamic_cast<Phase*>(this->module);
		if (!phase) { ModuleWidget::onPathDrop(e); return; }

		std::vector<std::string> wavs;
		for (const std::string& p : e.paths)
			if (string::endsWith(string::lowercase(p), ".wav")) wavs.push_back(p);
		if (wavs.empty()) { ModuleWidget::onPathDrop(e); return; }

		if (wavs.size() >= 2) {
			phase->loadSample(phase->sampleA, wavs[0]);
			phase->loadSample(phase->sampleB, wavs[1]);
			phase->sampleBExplicitlyLoaded = true;
			e.consume(this);
			return;
		}

		// Single file: lower half of the display targets B, otherwise A.
		Rect disp;
		disp.pos  = mm2px(Vec(2.54f, 14.f));
		disp.size = mm2px(Vec(91.44f, 24.f));
		bool toB = disp.contains(e.pos) && e.pos.y > disp.pos.y + disp.size.y * 0.5f;
		if (toB) {
			phase->loadSample(phase->sampleB, wavs[0]);
			phase->sampleBExplicitlyLoaded = true;
		} else {
			phase->loadSample(phase->sampleA, wavs[0]);
			if (!phase->sampleBExplicitlyLoaded)
				phase->loadSample(phase->sampleB, wavs[0]);
		}
		e.consume(this);
	}

	void appendContextMenu(Menu* menu) override {
		Phase* module = dynamic_cast<Phase*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Samples"));

		// Load Sample A
		std::string labelA = "Load Sample A";
		if (module->sampleA.loaded)
			labelA += " (" + module->sampleA.fileName + ")";
		menu->addChild(createMenuItem(labelA, "",
			[=]() { module->loadSampleDialog(false); }
		));

		// Load Sample B
		std::string labelB = "Load Sample B";
		if (module->sampleB.loaded)
			labelB += " (" + module->sampleB.fileName + ")";
		menu->addChild(createMenuItem(labelB, "",
			[=]() { module->loadSampleDialog(true); }
		));

		// Clear samples
		if (module->sampleA.loaded) {
			menu->addChild(createMenuItem("Clear Sample A", "",
				[=]() { module->clearSample(module->sampleA); }
			));
		}
		if (module->sampleB.loaded) {
			menu->addChild(createMenuItem("Clear Sample B", "",
				[=]() {
					module->clearSample(module->sampleB);
					module->sampleBExplicitlyLoaded = false;
				}
			));
		}
		if (module->sampleA.loaded || module->sampleB.loaded) {
			menu->addChild(createMenuItem("Clear A&B", "",
				[=]() {
					module->clearSample(module->sampleA);
					module->clearSample(module->sampleB);
					module->sampleBExplicitlyLoaded = false;
				}
			));
		}

		// Cue points (manual editing)
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Cue Points"));
		menu->addChild(createMenuLabel("(double-click waveform to add/remove, drag to move)"));
		if (module->sampleA.loaded)
			menu->addChild(createMenuItem("Clear cue points A", "",
				[=]() { module->clearCues(module->sampleA); }));
		if (module->sampleB.loaded)
			menu->addChild(createMenuItem("Clear cue points B", "",
				[=]() { module->clearCues(module->sampleB); }));

		// Transient detection settings
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Transient Detection"));

		menu->addChild(createMenuItem("Re-detect Transients", "",
			[=]() { module->redetectTransients(); }
		));

		// Sensitivity presets
		menu->addChild(createCheckMenuItem("High Sensitivity", "",
			[=]() { return module->transientSensitivity < 0.3f; },
			[=]() { module->transientSensitivity = 0.15f; module->redetectTransients(); }
		));
		menu->addChild(createCheckMenuItem("Medium Sensitivity (default)", "",
			[=]() { return module->transientSensitivity >= 0.3f && module->transientSensitivity <= 0.8f; },
			[=]() { module->transientSensitivity = 0.7f; module->redetectTransients(); }
		));
		menu->addChild(createCheckMenuItem("Low Sensitivity", "",
			[=]() { return module->transientSensitivity > 0.8f; },
			[=]() { module->transientSensitivity = 0.95f; module->redetectTransients(); }
		));

		// Min gap presets
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Min Transient Gap"));

		menu->addChild(createCheckMenuItem("10ms (fast)", "",
			[=]() { return module->transientMinGapMs < 20.f; },
			[=]() { module->transientMinGapMs = 10.f; module->redetectTransients(); }
		));
		menu->addChild(createCheckMenuItem("50ms (medium)", "",
			[=]() { return module->transientMinGapMs >= 20.f && module->transientMinGapMs <= 75.f; },
			[=]() { module->transientMinGapMs = 50.f; module->redetectTransients(); }
		));
		menu->addChild(createCheckMenuItem("100ms (slow)", "",
			[=]() { return module->transientMinGapMs > 75.f; },
			[=]() { module->transientMinGapMs = 100.f; module->redetectTransients(); }
		));

		// VCA mode
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("VCA Mode (anti-click)", "",
			&module->vcaMode));

		// Recording mode
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Recording"));
		menu->addChild(createCheckMenuItem("Replace existing audio", "",
			[=]() { return !module->recordAppend; },
			[=]() { module->recordAppend = false; }
		));
		menu->addChild(createCheckMenuItem("Append to existing audio", "",
			[=]() { return module->recordAppend; },
			[=]() { module->recordAppend = true; }
		));
		menu->addChild(createBoolPtrMenuItem("Save recordings with patch", "",
			&module->persistRecordedBuffers));

		// Export
		menu->addChild(new MenuSeparator);
		bool hasContent = (module->sampleA.loaded && module->sampleA.length > 0)
		               || (module->sampleB.loaded && module->sampleB.length > 0);
		MenuItem* saveItem = createMenuItem("Save buffers as stereo WAV…", "",
			[=]() { module->saveStereoWavDialog(); });
		saveItem->disabled = !hasContent;
		menu->addChild(saveItem);
	}
};


Model* modelPhase = createModel<Phase, PhaseWidget>("Phase");
