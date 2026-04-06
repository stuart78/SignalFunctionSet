#include "plugin.hpp"
#include <osdialog.h>
#include <thread>
#include <algorithm>
#include <cmath>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"


// Waveform display resolution
static const int WAVEFORM_POINTS = 512;

// WAV file filter for open dialog
static const char PHASE_WAV_FILTERS[] = "WAV file (.wav):wav;All files (*.*):*";

// Max sample length: 10 minutes at 48kHz
static const size_t MAX_SAMPLE_LENGTH = 48000 * 60 * 10;


struct SampleData {
	std::vector<float> samples;
	size_t length = 0;
	std::string filePath;
	std::string fileName;
	std::vector<size_t> transients;
	std::vector<float> waveformMini; // peak amplitude per display column
	bool loaded = false;
	bool hasCuePoints = false; // true if transients came from WAV cue points
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
		LOOP_END_B
	};
	DragTarget dragTarget = NONE;

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
		// Bracket direction: start handle ticks go right, end handle ticks go left
		float dir = isStart ? 1.f : -1.f;

		// Draw as a single filled path (no transparency stacking)
		nvgBeginPath(args.vg);

		// Build bracket shape as a filled polygon
		// Vertical bar + top tick + bottom tick
		float barLeft = isStart ? px : px - lineW;
		float barRight = isStart ? px + lineW : px;

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
	void onButton(const ButtonEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;

	void drawLayer(const DrawArgs& args, int layer) override;

	void draw(const DrawArgs& args) override {
		// Background
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(26, 26, 46));
		nvgFill(args.vg);

		// Border
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgStrokeColor(args.vg, nvgRGB(64, 64, 96));
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStroke(args.vg);

		// Horizontal divider
		float midY = box.size.y * 0.5f;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0, midY);
		nvgLineTo(args.vg, box.size.x, midY);
		nvgStrokeColor(args.vg, nvgRGB(64, 64, 96));
		nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);

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
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		PLAY_LIGHT,
		LIGHTS_LEN
	};

	SampleData sampleA;
	SampleData sampleB;
	LoopState loopA;
	LoopState loopB;
	dsp::SchmittTrigger syncTrigger;
	bool sampleBExplicitlyLoaded = false;
	bool playing = false;

	// Transient detection sensitivity (0.0 = most sensitive, 1.0 = least sensitive)
	float transientSensitivity = 0.7f;
	// Minimum gap between transients in ms
	float transientMinGapMs = 100.f;
	// VCA mode: anti-click envelope on jumps
	bool vcaMode = true;

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

		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
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

		// Reset state
		playing = false;
		transientSensitivity = 0.7f;
		transientMinGapMs = 100.f;
		vcaMode = true;
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
				float v = std::fabs(sd.samples[j]);
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
				float v = sd.samples[offset + j];
				sum += v * v;
				// High-frequency energy via sample differencing
				if (j > 0) {
					float diff = v - sd.samples[offset + j - 1];
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
		std::this_thread::sleep_for(std::chrono::duration<double>(100e-6));
		sd.samples.clear();
		sd.length = 0;
		sd.filePath.clear();
		sd.fileName.clear();
		sd.transients.clear();
		sd.waveformMini.clear();
		sd.loaded = false;
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
					// sampleByteOffset is byte offset into audio data
					// Convert to frame position
					uint32_t bytesPerFrame = channels * (bitsPerSample / 8);
					if (bytesPerFrame > 0) {
						size_t framePos = (size_t)(cue->pCuePoints[c].sampleByteOffset / bytesPerFrame);
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

		// Brief sleep to let DSP thread finish (following Fundamental pattern)
		std::this_thread::sleep_for(std::chrono::duration<double>(100e-6));

		sd.samples = std::move(mono);
		sd.length = sd.samples.size();
		sd.filePath = path;
		sd.fileName = system::getFilename(path);
		sd.loaded = true;
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
	}

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

		if (isB) {
			loadSample(sampleB, path);
			sampleBExplicitlyLoaded = true;
		} else {
			loadSample(sampleA, path);
			// Cascade to B if B not explicitly loaded
			if (!sampleBExplicitlyLoaded) {
				loadSample(sampleB, path);
			}
		}
	}

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

		if (sampleA.loaded)
			json_object_set_new(rootJ, "sampleAPath", json_string(sampleA.filePath.c_str()));
		if (sampleB.loaded)
			json_object_set_new(rootJ, "sampleBPath", json_string(sampleB.filePath.c_str()));
		json_object_set_new(rootJ, "sampleBExplicit", json_boolean(sampleBExplicitlyLoaded));
		json_object_set_new(rootJ, "playing", json_boolean(playing));
		json_object_set_new(rootJ, "transientSensitivity", json_real(transientSensitivity));
		json_object_set_new(rootJ, "transientMinGapMs", json_real(transientMinGapMs));
		json_object_set_new(rootJ, "vcaMode", json_boolean(vcaMode));

		// Loop regions
		json_object_set_new(rootJ, "loopStartA", json_real(sampleA.loopStart));
		json_object_set_new(rootJ, "loopEndA", json_real(sampleA.loopEnd));
		json_object_set_new(rootJ, "loopStartB", json_real(sampleB.loopStart));
		json_object_set_new(rootJ, "loopEndB", json_real(sampleB.loopEnd));

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

		if (pathAJ) {
			std::string path = json_string_value(pathAJ);
			loadSample(sampleA, path);
		}

		if (pathBJ) {
			std::string path = json_string_value(pathBJ);
			loadSample(sampleB, path);
		} else if (sampleA.loaded && !sampleBExplicitlyLoaded) {
			loadSample(sampleB, sampleA.filePath);
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
	}

	// --- DSP ---

	float processLoop(LoopState& loop, SampleData& sd,
	                   float sleepMs, float speed, float pan,
	                   bool rotateMode, float sampleTime,
	                   float& leftOut, float& rightOut) {
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
				float s0 = sd.samples[idx];
				size_t idx1 = idx + 1;
				if (idx1 >= regionEnd) idx1 = regionStart;
				float s1 = (idx1 < sd.length) ? sd.samples[idx1] : s0;
				sample = s0 + (s1 - s0) * frac;
			}

			// Advance playhead at base speed
			loop.playhead += std::fabs((double)speed);

			// Wrap playhead at loop boundary
			if (loop.playhead >= (double)regionEnd) {
				loop.playhead = (double)regionStart + (loop.playhead - (double)regionEnd);
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
					float s0 = sd.samples[idx];
					float s1 = (idx + 1 < sd.length) ? sd.samples[idx + 1] : sd.samples[idx];
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

	double findNextTransient(const LoopState& loop, const SampleData& sd) {
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

		// If not playing, output silence
		if (!isPlaying) {
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

		// Speed A: -4x to +4x, CV at 0.8x/V
		float speedA = params[SPEED_A_PARAM].getValue();
		if (inputs[SPEED_A_INPUT].isConnected())
			speedA += inputs[SPEED_A_INPUT].getVoltage() * 0.8f;
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

		// Speed B: -4x to +4x, CV at 0.8x/V
		float speedB = params[SPEED_B_PARAM].getValue();
		if (inputs[SPEED_B_INPUT].isConnected())
			speedB += inputs[SPEED_B_INPUT].getVoltage() * 0.8f;
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

		processLoop(loopA, sampleA, sleepA, speedA, panA, rotateModeA, args.sampleTime, leftOut, rightOut);
		processLoop(loopB, sampleB, sleepB, speedB, panB, rotateModeB, args.sampleTime, leftOut, rightOut);

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

void PhaseWaveformDisplay::onButton(const ButtonEvent& e) {
	if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
		dragTarget = hitTestHandle(e.pos);
		if (dragTarget != NONE) {
			APP->event->setSelectedWidget(this);
			e.consume(this);
		}
	}
	Widget::onButton(e);
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
		default:
			break;
	}
}

void PhaseWaveformDisplay::onDragEnd(const DragEndEvent& e) {
	dragTarget = NONE;
}


// --- Waveform display drawLayer implementation ---

void PhaseWaveformDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1 || !module) {
		Widget::drawLayer(args, layer);
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

	// Draw waveform A (top half)
	if (module->sampleA.loaded) {
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

	// Draw waveform B (bottom half)
	if (module->sampleB.loaded) {
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
	PhaseWidget(Phase* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/phase.svg")));

		// Screws
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

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

		// Stereo outputs (Y=116.84mm)
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(78.74f, 116.84f)), module, Phase::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(88.9f, 116.84f)), module, Phase::RIGHT_OUTPUT));
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
	}
};


Model* modelPhase = createModel<Phase, PhaseWidget>("Phase");
