#include "plugin.hpp"
#include "panel-style.hpp"
#include "scales.hpp"
#include <osdialog.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// =============================================================================
// Key — a quantizer that takes its key from the patch.
//
// An ordinary quantizer is an island: you set its scale by hand and then keep
// it in step with everything else yourself. This one reads ROOT and SCALE in
// the plugin's own convention (1V/oct semitone-quantized, 1V per scale), so
// Arrange, Note, Chance, Muse, Fugue and Loom can all be moved through a key
// change from one place and this follows.
//
// THREE SUB-SCALES filter the selected scale. They are masks over the parent's
// DEGREE INDICES, never over absolute pitches — so "degrees 1, 3, 5" is a triad
// in Major and is still a triad after the scale changes to Minor or the root
// moves. That is the whole point: a sub-scale is a role within the key, not a
// set of notes. Each channel picks the full scale or one of the three.
//
// SCALES ARE NOT NECESSARILY 12-TET, NOR NECESSARILY OCTAVE-REPEATING. Three of
// the canonical scales (Harmonic series, Pelog, Slendro) carry fractional
// semitone intervals, and a loaded Scala file can have any number of degrees
// repeating at any period — Bohlen-Pierce repeats at 3/1, not 2/1. So the
// quantizer works in "semitones within one period" against real float
// intervals. The twelve-bit pitch mask that quantizers are usually built on can
// represent none of that, which is why there isn't one here.
// =============================================================================

static const int KEY_NCH     = 4;      // channels
static const int KEY_MAXPOLY = 16;
static const int KEY_NSUB    = 3;      // sub-scales
static const int KEY_MAXDEG  = 64;     // Scala scales run well past twelve
static const int KEY_EDITDEG = 24;     // degrees the sub-scale rows can reach

static const char* KEY_NOTES[12] =
	{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* KEY_SUBNAME[KEY_NSUB + 1] = {"FULL", "A", "B", "C"};

// Which of the twelve semitones each white / black key is.
static const int KEY_WHITE[7] = {0, 2, 4, 5, 7, 9, 11};
static const int KEY_BLACK[5] = {1, 3, 6, 8, 10};
// Black key n sits after white key KEY_BLACK_AFTER[n].
static const int KEY_BLACK_AFTER[5] = {0, 1, 3, 4, 5};

enum KeyRound { KR_NEAREST, KR_DOWN, KR_UP, KR_COUNT };

// A scale, reduced to what the quantizer actually needs.
struct KeyScale {
	float iv[KEY_MAXDEG];
	int   n;
	float period;                        // semitones per repeat
	KeyScale() : n(0), period(12.f) {}
};

// Snap `semis` (relative to the root, any register) onto a scale.
static float keySnap(float semis, const KeyScale& s, int mode) {
	if (s.n <= 0 || s.period <= 0.01f) return semis;
	float rep = std::floor(semis / s.period);
	float within = semis - rep * s.period;
	float best = s.iv[0], bestD = 1e9f;
	// The wrap candidate is the first degree of the NEXT period — without it
	// anything above the last degree falls back to the bottom of this one.
	for (int k = 0; k <= s.n; k++) {
		float c = (k < s.n) ? s.iv[k] : s.iv[0] + s.period;
		float d = within - c;
		if (mode == KR_DOWN && d < -1e-4f) continue;
		if (mode == KR_UP   && d >  1e-4f) continue;
		float ad = std::fabs(d);
		if (ad < bestD) { bestD = ad; best = c; }
	}
	if (bestD > 1e8f)                     // directional rounding ran off the end
		best = (mode == KR_DOWN) ? s.iv[s.n - 1] - s.period : s.iv[0] + s.period;
	return rep * s.period + best;
}

// Move by n scale degrees, staying on the scale.
static float keyShiftDegrees(float semis, const KeyScale& s, int n) {
	if (n == 0 || s.n <= 0) return semis;
	float rep = std::floor(semis / s.period);
	float within = semis - rep * s.period;
	int idx = 0; float bd = 1e9f;
	for (int k = 0; k < s.n; k++) {
		float d = std::fabs(within - s.iv[k]);
		if (d < bd) { bd = d; idx = k; }
	}
	int t = idx + n;
	int repShift = (int)std::floor((float)t / (float)s.n);
	int wrapped = t - repShift * s.n;
	return (rep + (float)repShift) * s.period + s.iv[wrapped];
}

// ── Scala (.scl) ─────────────────────────────────────────────────────────────
// The format: `!` starts a comment, the first data line is a description, the
// second is the degree count, then that many pitches — either cents (any line
// containing a '.') or a ratio (`3/2`, or a bare integer). 1/1 is implicit and
// is NOT listed, and the LAST entry is the period, which is why it is pulled off
// the end rather than kept as a degree.
static bool keyLoadScala(const std::string& path, KeyScale& out, std::string& name) {
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f) return false;
	char line[1024];
	int stage = 0;                       // 0 = want description, 1 = want count, 2 = pitches
	int want = 0;
	std::vector<float> semis;
	semis.push_back(0.f);                // the implicit 1/1
	name.clear();

	while (std::fgets(line, sizeof(line), f)) {
		std::string t(line);
		size_t a = t.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) continue;
		t = t.substr(a);
		size_t b = t.find_last_not_of(" \t\r\n");
		t = t.substr(0, b + 1);
		if (t.empty() || t[0] == '!') continue;

		if (stage == 0) { name = t; stage = 1; continue; }
		if (stage == 1) { want = std::atoi(t.c_str()); stage = 2; continue; }

		float v;
		if (t.find('.') != std::string::npos) {
			v = (float)std::atof(t.c_str()) / 100.f;             // cents → semitones
		}
		else {
			size_t sl = t.find('/');
			double num = (sl == std::string::npos) ? std::atof(t.c_str())
			                                       : std::atof(t.substr(0, sl).c_str());
			double den = (sl == std::string::npos) ? 1.0 : std::atof(t.substr(sl + 1).c_str());
			if (den == 0.0 || num <= 0.0) continue;
			v = (float)(12.0 * std::log2(num / den));
		}
		semis.push_back(v);
		if ((int)semis.size() > want) break;                     // count reached
	}
	std::fclose(f);

	if (want <= 0 || (int)semis.size() < 2) return false;
	out.period = semis.back();                                   // the last entry IS the period
	semis.pop_back();
	if (out.period <= 0.01f) return false;
	std::sort(semis.begin(), semis.end());
	out.n = 0;
	for (size_t i = 0; i < semis.size() && out.n < KEY_MAXDEG; i++) {
		if (out.n > 0 && std::fabs(semis[i] - out.iv[out.n - 1]) < 0.001f) continue;
		out.iv[out.n++] = semis[i];
	}
	return out.n > 0;
}


struct Key : Module {
	enum ParamId {
		ROOT_PARAM, SCALE_PARAM,
		ENUMS(OFFSET_PARAM, KEY_NCH),
		ENUMS(SUB_PARAM, KEY_NCH),
		PARAMS_LEN
	};
	enum InputId {
		ROOT_INPUT, SCALE_INPUT, TRIG_INPUT,
		ENUMS(IN_INPUT, KEY_NCH),
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(OUT_OUTPUT, KEY_NCH),
		ENUMS(CHG_OUTPUT, KEY_NCH),
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	// Index NUM_SCALES selects a loaded Scala file; 0..NUM_SCALES-1 stay exactly
	// the canonical list, so SCALE CV is still interchangeable across the plugin.
	static const int SCALA_INDEX = sfs::NUM_SCALES;

	// ── the key in force ──────────────────────────────────────────────────────
	int   rootNote = 0;
	int   scaleIndex = 1;
	bool  customMask = false;            // the keyboard has been edited by hand
	uint16_t mask = 0;

	KeyScale parent;                     // the selected scale
	KeyScale sub[KEY_NSUB];              // parent filtered by each sub-scale mask
	uint32_t subMask[KEY_NSUB] = {0, 0, 0};
	int   keyGen = 0;                    // bumped whenever anything about the key moves

	// ── Scala ─────────────────────────────────────────────────────────────────
	KeyScale scala;
	std::string scalaName, scalaPath;
	bool  scalaLoaded = false;

	// ── options ───────────────────────────────────────────────────────────────
	bool  offsetInDegrees = true;
	int   roundMode = KR_NEAREST;
	float hysteresisCents = 12.f;

	// ── per-channel state ─────────────────────────────────────────────────────
	float held[KEY_NCH][KEY_MAXPOLY] = {};
	bool  hasHeld[KEY_NCH][KEY_MAXPOLY] = {};
	int   lastGen[KEY_NCH][KEY_MAXPOLY] = {};
	dsp::PulseGenerator chgPulse[KEY_NCH][KEY_MAXPOLY];
	dsp::SchmittTrigger trigIn;
	float shownVolts[KEY_NCH] = {};
	bool  shownActive[KEY_NCH] = {};

	Key() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		std::vector<std::string> noteNames;
		for (int i = 0; i < 12; i++) noteNames.push_back(KEY_NOTES[i]);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", noteNames);

		std::vector<std::string> scaleNames;
		for (int i = 0; i < sfs::NUM_SCALES; i++)
			scaleNames.push_back(sfs::SCALES[i].longName);
		scaleNames.push_back("Scala file");
		configSwitch(SCALE_PARAM, 0.f, (float)SCALA_INDEX, 1.f, "Scale", scaleNames);

		for (int c = 0; c < KEY_NCH; c++) {
			configParam(OFFSET_PARAM + c, -12.f, 12.f, 0.f,
			            string::f("Channel %d offset", c + 1));
			getParamQuantity(OFFSET_PARAM + c)->snapEnabled = true;
			configSwitch(SUB_PARAM + c, 0.f, (float)KEY_NSUB, 0.f,
			             string::f("Channel %d scale", c + 1),
			             {"Full scale", "Sub A", "Sub B", "Sub C"});
			configInput(IN_INPUT + c, string::f("Channel %d pitch (1V/oct, poly)", c + 1));
			configOutput(OUT_OUTPUT + c, string::f("Channel %d quantized pitch", c + 1));
			configOutput(CHG_OUTPUT + c, string::f("Channel %d note-change trigger", c + 1));
			configBypass(IN_INPUT + c, OUT_OUTPUT + c);
		}

		configInput(ROOT_INPUT,  "Root CV (1V/oct, semitone-quantized)");
		configInput(SCALE_INPUT, "Scale CV (1V per scale)");
		configInput(TRIG_INPUT,  "Sample & hold trigger — when patched, notes update only on a trigger");

		defaultSubs();
		rebuild();
	}

	// Seeded with three roles you actually reach for, expressed as degrees so
	// they stay themselves through any change of scale.
	void defaultSubs() {
		subMask[0] = (1u << 0) | (1u << 2) | (1u << 4);              // triad
		subMask[1] = (1u << 0) | (1u << 3) | (1u << 4);              // root, 4th, 5th
		subMask[2] = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6);  // seventh chord
	}

	void onReset() override {
		customMask = false;
		offsetInDegrees = true;
		roundMode = KR_NEAREST;
		hysteresisCents = 12.f;
		defaultSubs();
		for (int c = 0; c < KEY_NCH; c++)
			for (int p = 0; p < KEY_MAXPOLY; p++) hasHeld[c][p] = false;
		rebuild();
	}

	bool scaleIsScala() const { return scaleIndex >= SCALA_INDEX; }

	// The twelve-key mask the on-screen keyboard shows. Microtonal degrees are
	// rounded HERE and nowhere else — see the note at the top of the file.
	uint16_t keyboardMask() const {
		if (customMask) return mask;
		uint16_t m = 0;
		for (int k = 0; k < parent.n; k++) {
			int s = ((int)std::lround(parent.iv[k]) % 12 + 12) % 12;
			m |= (uint16_t)(1u << s);
		}
		return m;
	}

	void rebuild() {
		parent = KeyScale();
		if (customMask) {
			for (int s = 0; s < 12 && parent.n < KEY_MAXDEG; s++)
				if (mask & (1u << s)) parent.iv[parent.n++] = (float)s;
			parent.period = 12.f;
		}
		else if (scaleIsScala() && scalaLoaded) {
			parent = scala;
		}
		else {
			int si = clamp(scaleIndex, 0, sfs::NUM_SCALES - 1);
			const sfs::Scale& sc = sfs::SCALES[si];
			parent.period = 12.f;
			for (int d = 0; d < sc.size && parent.n < KEY_MAXDEG; d++) {
				float v = std::fmod(sc.intervals[d], 12.f);
				if (v < 0.f) v += 12.f;
				parent.iv[parent.n++] = v;
			}
			// The harmonic series spans octaves, so folding it leaves duplicates.
			std::sort(parent.iv, parent.iv + parent.n);
			int w = 0;
			for (int a = 0; a < parent.n; a++)
				if (a == 0 || std::fabs(parent.iv[a] - parent.iv[w - 1]) > 0.01f)
					parent.iv[w++] = parent.iv[a];
			parent.n = w;
		}
		if (parent.n == 0) { parent.iv[0] = 0.f; parent.n = 1; }   // never mute

		for (int k = 0; k < KEY_NSUB; k++) {
			sub[k] = KeyScale();
			sub[k].period = parent.period;
			for (int d = 0; d < parent.n && d < KEY_EDITDEG; d++)
				if (subMask[k] & (1u << d)) sub[k].iv[sub[k].n++] = parent.iv[d];
			// An empty sub-scale falls back to the parent rather than going silent.
			if (sub[k].n == 0) sub[k] = parent;
		}
		keyGen++;
	}

	const KeyScale& scaleFor(int c) {
		int s = (int)std::round(params[SUB_PARAM + c].getValue());
		return (s >= 1 && s <= KEY_NSUB) ? sub[s - 1] : parent;
	}

	void process(const ProcessArgs& args) override {
		int rootK = (int)std::round(params[ROOT_PARAM].getValue());
		int rootCV = inputs[ROOT_INPUT].isConnected()
			? (int)std::round(inputs[ROOT_INPUT].getVoltage() * 12.f) : 0;
		int newRoot = (((rootK + rootCV) % 12) + 12) % 12;

		int scaleK = (int)std::round(params[SCALE_PARAM].getValue());
		int scaleCV = inputs[SCALE_INPUT].isConnected()
			? (int)std::round(inputs[SCALE_INPUT].getVoltage()) : 0;
		int newScale = clamp(scaleK + scaleCV, 0, SCALA_INDEX);

		if (newRoot != rootNote) { rootNote = newRoot; keyGen++; }
		if (newScale != scaleIndex) {
			scaleIndex = newScale;
			// Selecting a scale is an explicit instruction, so it wins over a
			// hand-edited keyboard rather than being silently ignored.
			customMask = false;
			rebuild();
		}

		bool sampleNow = true;
		if (inputs[TRIG_INPUT].isConnected())
			sampleNow = trigIn.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);

		float hystSemis = hysteresisCents / 100.f;

		for (int c = 0; c < KEY_NCH; c++) {
			const KeyScale& sc = scaleFor(c);
			int nch = std::max(1, inputs[IN_INPUT + c].getChannels());
			nch = std::min(nch, KEY_MAXPOLY);
			outputs[OUT_OUTPUT + c].setChannels(nch);
			outputs[CHG_OUTPUT + c].setChannels(nch);
			shownActive[c] = inputs[IN_INPUT + c].isConnected();

			int off = (int)std::round(params[OFFSET_PARAM + c].getValue());
			// A sub-scale change must invalidate held notes exactly as a key
			// change does, or a channel keeps a note its new scale does not have.
			int gen = keyGen * 8 + (int)std::round(params[SUB_PARAM + c].getValue());

			for (int p = 0; p < nch; p++) {
				float in = inputs[IN_INPUT + c].getVoltage(p);

				if (sampleNow || !hasHeld[c][p] || lastGen[c][p] != gen) {
					float semis = in * 12.f - (float)rootNote;
					if (!offsetInDegrees) semis += (float)off;
					float q = keySnap(semis, sc, roundMode);
					if (offsetInDegrees) q = keyShiftDegrees(q, sc, off);

					// Hysteresis stops a pitch sitting exactly on a boundary from
					// chattering between two notes. It must be abandoned when the
					// key changes, or the old note would hold on even though it is
					// no longer in the scale.
					if (hasHeld[c][p] && lastGen[c][p] == gen && hystSemis > 0.f) {
						float prev = held[c][p] * 12.f - (float)rootNote;
						float inS = in * 12.f - (float)rootNote;
						if (!offsetInDegrees) inS += (float)off;
						if (std::fabs(inS - prev) < std::fabs(inS - q) + hystSemis)
							q = prev;
					}
					float outV = ((float)rootNote + q) / 12.f;

					if (!hasHeld[c][p] || std::fabs(outV - held[c][p]) > 1e-6f) {
						if (hasHeld[c][p]) chgPulse[c][p].trigger(1e-3f);
						held[c][p] = outV;
					}
					hasHeld[c][p] = true;
					lastGen[c][p] = gen;
				}

				outputs[OUT_OUTPUT + c].setVoltage(held[c][p], p);
				outputs[CHG_OUTPUT + c].setVoltage(
					chgPulse[c][p].process(args.sampleTime) ? 10.f : 0.f, p);
				if (p == 0) shownVolts[c] = held[c][p];
			}
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "customMask", json_boolean(customMask));
		json_object_set_new(root, "mask", json_integer(mask));
		json_object_set_new(root, "offsetInDegrees", json_boolean(offsetInDegrees));
		json_object_set_new(root, "roundMode", json_integer(roundMode));
		json_object_set_new(root, "hysteresisCents", json_real(hysteresisCents));
		json_t* sm = json_array();
		for (int k = 0; k < KEY_NSUB; k++) json_array_append_new(sm, json_integer(subMask[k]));
		json_object_set_new(root, "subMask", sm);

		// The PARSED scale is saved as well as the path: a patch opened on another
		// machine, or after the .scl has moved, still sounds right.
		json_object_set_new(root, "scalaLoaded", json_boolean(scalaLoaded));
		json_object_set_new(root, "scalaPath", json_string(scalaPath.c_str()));
		json_object_set_new(root, "scalaName", json_string(scalaName.c_str()));
		json_object_set_new(root, "scalaPeriod", json_real(scala.period));
		json_t* iv = json_array();
		for (int k = 0; k < scala.n; k++) json_array_append_new(iv, json_real(scala.iv[k]));
		json_object_set_new(root, "scalaIvals", iv);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "customMask")) customMask = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "mask")) mask = (uint16_t)json_integer_value(j);
		if (json_t* j = json_object_get(root, "offsetInDegrees"))
			offsetInDegrees = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "roundMode"))
			roundMode = clamp((int)json_integer_value(j), 0, KR_COUNT - 1);
		if (json_t* j = json_object_get(root, "hysteresisCents"))
			hysteresisCents = clamp((float)json_number_value(j), 0.f, 50.f);
		if (json_t* sm = json_object_get(root, "subMask"))
			for (int k = 0; k < KEY_NSUB && k < (int)json_array_size(sm); k++)
				subMask[k] = (uint32_t)json_integer_value(json_array_get(sm, k));

		if (json_t* j = json_object_get(root, "scalaLoaded")) scalaLoaded = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "scalaPath")) scalaPath = json_string_value(j);
		if (json_t* j = json_object_get(root, "scalaName")) scalaName = json_string_value(j);
		if (json_t* j = json_object_get(root, "scalaPeriod")) scala.period = json_number_value(j);
		if (json_t* iv = json_object_get(root, "scalaIvals")) {
			scala.n = 0;
			for (int k = 0; k < (int)json_array_size(iv) && scala.n < KEY_MAXDEG; k++)
				scala.iv[scala.n++] = (float)json_number_value(json_array_get(iv, k));
		}
		rebuild();
	}
};


// =============================================================================
// Display — the key as a keyboard, the scale as regions, the sub-scales as rows.
// =============================================================================

struct KeyDisplay : OpaqueWidget {
	Key* module = nullptr;
	std::shared_ptr<Font> font;

	struct Lay {
		float w, h;
		float readY;
		float kbY, kbH, wkw, bkw, bkh;
		float stripY, stripH;
		float subY, subH, subGap, subX;
		float footY;
	};

	Lay layout() const {
		Lay L;
		L.w = box.size.x; L.h = box.size.y;
		L.readY  = L.h * 0.055f;
		L.kbY    = L.h * 0.105f;  L.kbH = L.h * 0.235f;
		L.wkw    = L.w / 7.f;
		L.bkw    = L.wkw * 0.62f; L.bkh = L.kbH * 0.62f;
		L.stripY = L.h * 0.375f;  L.stripH = L.h * 0.110f;
		L.subY   = L.h * 0.545f;  L.subH = L.h * 0.085f;  L.subGap = L.h * 0.030f;
		L.subX   = L.w * 0.115f;                          // room for the A/B/C label
		L.footY  = L.h * 0.945f;
		return L;
	}

	int keyHit(const Lay& L, Vec p) const {
		if (p.y < L.kbY || p.y > L.kbY + L.kbH) return -1;
		if (p.y <= L.kbY + L.bkh) {
			for (int b = 0; b < 5; b++) {
				float x = (float)(KEY_BLACK_AFTER[b] + 1) * L.wkw - L.bkw * 0.5f;
				if (p.x >= x && p.x <= x + L.bkw) return KEY_BLACK[b];
			}
		}
		int wi = (int)std::floor(p.x / L.wkw);
		if (wi < 0 || wi > 6) return -1;
		return KEY_WHITE[wi];
	}

	// Which sub-scale row and which degree cell, or (-1, -1).
	void subHit(const Lay& L, Vec p, int& row, int& deg, int nDeg) const {
		row = deg = -1;
		for (int k = 0; k < KEY_NSUB; k++) {
			float y = L.subY + (float)k * (L.subH + L.subGap);
			if (p.y < y || p.y > y + L.subH) continue;
			if (p.x < L.subX) return;
			int nd = std::min(nDeg, KEY_EDITDEG);
			float cw = (L.w - L.subX) / (float)std::max(nd, 1);
			int d = (int)std::floor((p.x - L.subX) / cw);
			if (d < 0 || d >= nd) return;
			row = k; deg = d;
			return;
		}
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) {
			OpaqueWidget::onButton(e);
			return;
		}
		Lay L = layout();

		int row, deg;
		subHit(L, e.pos, row, deg, module->parent.n);
		if (row >= 0) {
			e.consume(this);
			module->subMask[row] ^= (1u << deg);
			module->rebuild();
			return;
		}

		int s = keyHit(L, e.pos);
		if (s < 0) return;                        // a miss falls through to the panel
		e.consume(this);
		if (!module->customMask) {                // the first edit forks off the preset
			module->mask = module->keyboardMask();
			module->customMask = true;
		}
		module->mask ^= (uint16_t)(1u << s);
		module->rebuild();
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!font || font->handle < 0) font = sfs::panelFont();
		if (!font || font->handle < 0) return;
		nvgSave(args.vg);
		nvgScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		if (!module) drawPreview(args);
		else         drawLive(args);
		nvgRestore(args.vg);
	}

	void text(const DrawArgs& args, float x, float y, float size,
	          NVGcolor col, const std::string& t, int align = NVG_ALIGN_CENTER) {
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, size);
		nvgTextLetterSpacing(args.vg, 0.2f);
		nvgTextAlign(args.vg, align | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, col);
		nvgText(args.vg, x, y, t.c_str(), NULL);
	}

	void drawKeyboard(const DrawArgs& args, const Lay& L, uint16_t m, int root,
	                  const bool* lit, bool approximate) {
		NVGcontext* vg = args.vg;
		// A microtonal scale cannot be told the truth on twelve keys, so the
		// keyboard steps back and the region strip below becomes the real display.
		float aOn = approximate ? 0.55f : 1.f;
		for (int i = 0; i < 7; i++) {
			int s = KEY_WHITE[i];
			bool in = (m >> s) & 1;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, i * L.wkw + 0.6f, L.kbY, L.wkw - 1.2f, L.kbH, 1.6f);
			NVGcolor c = lit[s] ? sfs::SCREEN_HOT
			           : in     ? sfs::SCREEN_BLUE : nvgRGB(0x2B, 0x2B, 0x44);
			if (in && !lit[s]) c = nvgTransRGBAf(c, aOn);
			nvgFillColor(vg, c);
			nvgFill(vg);
			if (s == root) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, i * L.wkw + 0.6f, L.kbY, L.wkw - 1.2f, L.kbH, 1.6f);
				nvgStrokeColor(vg, sfs::SCREEN_TEXT);
				nvgStrokeWidth(vg, 1.4f);
				nvgStroke(vg);
			}
		}
		for (int b = 0; b < 5; b++) {
			int s = KEY_BLACK[b];
			bool in = (m >> s) & 1;
			float x = (float)(KEY_BLACK_AFTER[b] + 1) * L.wkw - L.bkw * 0.5f;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x, L.kbY, L.bkw, L.bkh, 1.4f);
			NVGcolor c = lit[s] ? sfs::SCREEN_HOT
			           : in     ? sfs::SCREEN_DEEP : nvgRGB(0x16, 0x16, 0x26);
			if (in && !lit[s]) c = nvgTransRGBAf(c, aOn);
			nvgFillColor(vg, c);
			nvgFill(vg);
			if (s == root) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x, L.kbY, L.bkw, L.bkh, 1.4f);
				nvgStrokeColor(vg, sfs::SCREEN_TEXT);
				nvgStrokeWidth(vg, 1.2f);
				nvgStroke(vg);
			}
		}
	}

	// The region strip. x is LINEAR in pitch across one period, so equal pitch
	// spans are equal widths — which is the point: you can see that Pelog's second
	// degree sits at 1.20 semitones, just above C#, and see how wide the band of
	// input pitches that lands on it actually is. The keyboard above can only
	// round; this cannot.
	void drawRegions(const DrawArgs& args, const Lay& L, const KeyScale& sc,
	                 int root, const float* sounding, int nSounding) {
		NVGcontext* vg = args.vg;
		float per = std::max(sc.period, 0.01f);
		float y0 = L.stripY, hh = L.stripH;

		nvgBeginPath(vg);
		nvgRect(vg, 0.f, y0, L.w, hh);
		nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x20));
		nvgFill(vg);

		// Alternating snap regions: the boundaries are the midpoints between
		// neighbouring degrees, so a band is literally the set of input pitches
		// that land on that degree.
		for (int k = 0; k < sc.n; k++) {
			float prev = (k == 0) ? sc.iv[sc.n - 1] - per : sc.iv[k - 1];
			float next = (k == sc.n - 1) ? sc.iv[0] + per : sc.iv[k + 1];
			float lo = 0.5f * (prev + sc.iv[k]);
			float hi = 0.5f * (sc.iv[k] + next);
			float xa = std::max(lo / per * L.w, 0.f);
			float xb = std::min(hi / per * L.w, L.w);
			if (xb <= xa) continue;
			nvgBeginPath(vg);
			nvgRect(vg, xa, y0, xb - xa, hh);
			nvgFillColor(vg, (k % 2) ? nvgRGB(0x1D, 0x2E, 0x3E) : nvgRGB(0x15, 0x22, 0x30));
			nvgFill(vg);
		}

		// the twelve keys, as a ruler behind the degrees
		for (int s = 0; s <= (int)std::ceil(per); s++) {
			float x = (float)s / per * L.w;
			if (x > L.w) break;
			bool natural = false;
			for (int i = 0; i < 7; i++) if (KEY_WHITE[i] == (((s + root) % 12) + 12) % 12) natural = true;
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, y0);
			nvgLineTo(vg, x, y0 + hh * (natural ? 1.f : 0.5f));
			nvgStrokeColor(vg, nvgRGBA(0x8A, 0x8A, 0xA5, natural ? 110 : 55));
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
		}

		// the degrees themselves
		for (int k = 0; k < sc.n; k++) {
			float x = sc.iv[k] / per * L.w;
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, y0 + 1.f);
			nvgLineTo(vg, x, y0 + hh - 1.f);
			nvgStrokeColor(vg, sfs::SCREEN_BLUE);
			nvgStrokeWidth(vg, 1.7f);
			nvgStroke(vg);
		}

		// where the channels currently sit
		for (int i = 0; i < nSounding; i++) {
			float within = sounding[i] - per * std::floor(sounding[i] / per);
			float x = within / per * L.w;
			nvgBeginPath(vg);
			nvgCircle(vg, x, y0 + hh * 0.5f, 2.1f);
			nvgFillColor(vg, sfs::SCREEN_HOT);
			nvgFill(vg);
		}
	}

	void drawSubRows(const DrawArgs& args, const Lay& L, int nDeg,
	                 const uint32_t* masks, const bool* used) {
		NVGcontext* vg = args.vg;
		int nd = std::min(nDeg, KEY_EDITDEG);
		float cw = (L.w - L.subX) / (float)std::max(nd, 1);
		for (int k = 0; k < KEY_NSUB; k++) {
			float y = L.subY + (float)k * (L.subH + L.subGap);
			text(args, L.subX * 0.5f, y + L.subH * 0.5f, L.subH * 0.86f,
			     used[k] ? sfs::SCREEN_TEXT : sfs::SCREEN_PMID, KEY_SUBNAME[k + 1]);
			for (int d = 0; d < nd; d++) {
				bool on = (masks[k] >> d) & 1;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, L.subX + (float)d * cw + 0.7f, y,
				               std::max(cw - 1.4f, 1.f), L.subH, 1.2f);
				nvgFillColor(vg, on ? (used[k] ? sfs::SCREEN_BLUE : sfs::SCREEN_DEEP)
				                    : nvgRGB(0x23, 0x23, 0x38));
				nvgFill(vg);
			}
		}
	}

	void drawLive(const DrawArgs& args) {
		Lay L = layout();
		Key* m = module;

		bool lit[12] = {};
		float sounding[KEY_NCH];
		int nSound = 0;
		for (int c = 0; c < KEY_NCH; c++) {
			if (!m->shownActive[c]) continue;
			sounding[nSound++] = m->shownVolts[c] * 12.f - (float)m->rootNote;
			int s = (int)std::lround(m->shownVolts[c] * 12.f);
			lit[((s % 12) + 12) % 12] = true;
		}

		bool micro = std::fabs(m->parent.period - 12.f) > 0.02f;
		for (int k = 0; k < m->parent.n; k++)
			if (std::fabs(m->parent.iv[k] - std::round(m->parent.iv[k])) > 0.02f) micro = true;

		std::string sname = m->customMask ? std::string("CUSTOM")
		    : m->scaleIsScala() ? (m->scalaLoaded ? m->scalaName : std::string("NO SCALA FILE"))
		    : std::string(sfs::SCALES[clamp(m->scaleIndex, 0, sfs::NUM_SCALES - 1)].shortName);
		if (sname.size() > 20) sname = sname.substr(0, 20);
		text(args, L.w * 0.5f, L.readY, L.h * 0.075f, sfs::SCREEN_TEXT,
		     std::string(KEY_NOTES[m->rootNote]) + "  " + sname);

		drawKeyboard(args, L, m->keyboardMask(), m->rootNote, lit, micro);
		drawRegions(args, L, m->parent, m->rootNote, sounding, nSound);

		bool used[KEY_NSUB] = {};
		for (int c = 0; c < KEY_NCH; c++) {
			int s = (int)std::round(m->params[Key::SUB_PARAM + c].getValue());
			if (s >= 1 && s <= KEY_NSUB) used[s - 1] = true;
		}
		drawSubRows(args, L, m->parent.n, m->subMask, used);

		float cw = L.w / (float)KEY_NCH;
		for (int c = 0; c < KEY_NCH; c++) {
			int sIdx = (int)std::round(m->params[Key::SUB_PARAM + c].getValue());
			std::string t = "–";
			if (m->shownActive[c]) {
				int s = (int)std::lround(m->shownVolts[c] * 12.f);
				int pc = ((s % 12) + 12) % 12;
				t = std::string(KEY_NOTES[pc]) + std::to_string(4 + (int)std::floor(s / 12.f));
				if (sIdx > 0) t += " " + std::string(KEY_SUBNAME[sIdx]);
			}
			text(args, cw * ((float)c + 0.5f), L.footY, L.h * 0.062f,
			     m->shownActive[c] ? sfs::SCREEN_BLUE : sfs::SCREEN_PMID, t);
		}
	}

	// The thumbnail shows Pelog, because the region strip is the reason this
	// module looks different from every other quantizer.
	void drawPreview(const DrawArgs& args) {
		Lay L = layout();
		KeyScale pel;
		static const float PEL[7] = {0.f, 1.2f, 2.7f, 5.4f, 7.0f, 8.0f, 10.4f};
		for (int i = 0; i < 7; i++) pel.iv[i] = PEL[i];
		pel.n = 7; pel.period = 12.f;

		bool lit[12] = {}; lit[0] = lit[5] = true;
		float sounding[2] = {0.f, 5.4f};
		// Derived, not hand-written: a literal here drifts from the intervals
		// above the moment either is touched, and mine already had.
		uint16_t pm = 0;
		for (int i = 0; i < 7; i++)
			pm |= (uint16_t)(1u << ((((int)std::lround(PEL[i])) % 12 + 12) % 12));
		text(args, L.w * 0.5f, L.readY, L.h * 0.075f, sfs::SCREEN_TEXT, "C  Pelog");
		drawKeyboard(args, L, pm, 0, lit, true);
		drawRegions(args, L, pel, 0, sounding, 2);

		static const uint32_t sm[KEY_NSUB] = {0b0010101, 0b0011001, 0b1010101};
		static const bool used[KEY_NSUB] = {true, false, false};
		drawSubRows(args, L, 7, sm, used);

		static const char* n[KEY_NCH] = {"C3 A", "F3", "–", "–"};
		float fw = L.w / (float)KEY_NCH;
		for (int c = 0; c < KEY_NCH; c++)
			text(args, fw * ((float)c + 0.5f), L.footY, L.h * 0.062f,
			     c < 2 ? sfs::SCREEN_BLUE : sfs::SCREEN_PMID, n[c]);
	}
};


// =============================================================================
// Panel — 12HP.
// =============================================================================

struct KeyWidget : ModuleWidget {
	KeyWidget(Key* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/key.svg")));
		using sfs::hp;

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(hp(1), hp(1.6f), "KEY");

		KeyDisplay* disp = new KeyDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(hp(0.8f), hp(2.4f)));
		disp->box.size = mm2px(Vec(hp(10.4f), hp(11.0f)));
		addChild(disp);

		// ── the key ────────────────────────────────────────────────────────────
		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(2), hp(15))), module, Key::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(4), hp(15))), module, Key::ROOT_INPUT));
		lbl->pair(hp(2), hp(15), "ROOT");

		addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(6), hp(15))), module, Key::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(8), hp(15))), module, Key::SCALE_INPUT));
		lbl->pair(hp(6), hp(15), "SCALE");

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(10), hp(15))), module, Key::TRIG_INPUT));
		lbl->jack(hp(10), hp(15), "TRIG");

		// ── four channels, one key ─────────────────────────────────────────────
		// These sit ON the plate: plate ink is light, so above it on the light
		// faceplate they would be invisible.
		lbl->jackOnPlate(hp(2),  hp(17.8f), "IN");
		lbl->jackOnPlate(hp(4),  hp(17.8f), "SUB");
		lbl->jackOnPlate(hp(6),  hp(17.8f), "OFF");
		lbl->jackOnPlate(hp(8),  hp(17.8f), "CHG");
		lbl->jackOnPlate(hp(10), hp(17.8f), "OUT");
		for (int c = 0; c < KEY_NCH; c++) {
			float y = hp(17.8f + 2.f * c);
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(hp(2), y)), module, Key::IN_INPUT + c));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(4), y)), module, Key::SUB_PARAM + c));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(hp(6), y)), module, Key::OFFSET_PARAM + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(8), y)), module, Key::CHG_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(hp(10), y)), module, Key::OUT_OUTPUT + c));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Key* m = dynamic_cast<Key*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);

		menu->addChild(createMenuItem("Load Scala file (.scl)…",
			m->scalaLoaded ? m->scalaName : "", [=]() {
				osdialog_filters* f = osdialog_filters_parse("Scala scale:scl");
				char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, f);
				osdialog_filters_free(f);
				if (!path) return;
				KeyScale s;
				std::string name;
				if (keyLoadScala(path, s, name)) {
					m->scala = s;
					m->scalaName = name.empty() ? std::string("Scala") : name;
					m->scalaPath = path;
					m->scalaLoaded = true;
					// Loading a file is a request to hear it.
					m->params[Key::SCALE_PARAM].setValue((float)Key::SCALA_INDEX);
					m->customMask = false;
					m->rebuild();
				}
				else {
					osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK,
						"Could not read that as a Scala (.scl) file.");
				}
				std::free(path);
			}));
		if (m->scalaLoaded)
			menu->addChild(createMenuLabel(string::f("   %d degrees, period %.1f cents",
				m->scala.n, m->scala.period * 100.f)));

		menu->addChild(new MenuSeparator);
		// Degrees is the default because it is the scale-aware answer: +2 moves
		// two steps UP THE SCALE and stays in key, where +2 semitones does not.
		menu->addChild(createBoolPtrMenuItem("Offset in scale degrees", "",
			&m->offsetInDegrees));
		menu->addChild(createIndexPtrSubmenuItem("Rounding",
			{"Nearest", "Down", "Up"}, &m->roundMode));

		menu->addChild(createSubmenuItem("Hysteresis",
			string::f("%.0f cents", m->hysteresisCents), [=](Menu* sub) {
				struct HystQuantity : Quantity {
					Key* m;
					void setValue(float v) override { m->hysteresisCents = clamp(v, 0.f, 50.f); }
					float getValue() override { return m->hysteresisCents; }
					float getDefaultValue() override { return 12.f; }
					float getMinValue() override { return 0.f; }
					float getMaxValue() override { return 50.f; }
					std::string getLabel() override { return "Hysteresis"; }
					std::string getUnit() override { return " cents"; }
				};
				HystQuantity* q = new HystQuantity;
				q->m = m;
				Slider* sl = new Slider;
				sl->quantity = q;
				sl->box.size.x = 180.f;
				sub->addChild(sl);
			}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Revert keyboard to the selected scale", "", [=]() {
			m->customMask = false;
			m->rebuild();
		}));
		menu->addChild(createMenuItem("Reset sub-scales", "", [=]() {
			m->defaultSubs();
			m->rebuild();
		}));
		menu->addChild(createMenuItem("Sub-scales: every degree", "", [=]() {
			for (int k = 0; k < KEY_NSUB; k++) m->subMask[k] = 0xFFFFFFFFu;
			m->rebuild();
		}));
	}
};

Model* modelKey = createModel<Key, KeyWidget>("Key");
