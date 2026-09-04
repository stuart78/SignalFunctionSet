#include "plugin.hpp"
#include "dr_wav.h"        // implementation lives in phase.cpp; headers only here
#include "dr_flac.h"       // implementation lives in dr_flac.cpp; headers only here
extern "C" {
#include "miniz.h"         // implementation is src/miniz.c — .dslibrary is a zip
}
#include "scales.hpp"      // canonical sfs::SCALES (shared) — for the In-Key grid
#include <osdialog.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

// ─── Play — polyphonic multisample (SFZ) player ──────────────────────────────
// Loads SFZ instruments (e.g. those written by Record, or simple third-party
// SFZ) and plays them polyphonically: V/OCT + GATE + VELOCITY (poly) pick a
// region by key & velocity, pitch-shift the sample, and mix to stereo. Several
// instruments can be loaded and switched by CV.

static const int PLAY_MAX_VOICES = 16;
static const char* PLAY_NOTES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// Push-style isomorphic pad grid — shared helpers (GRID_COLS/ROWS, gridNoteAt, …)
#include "pushgrid.hpp"

struct SfzRegion {
	std::string sample;
	int   lokey = 0, hikey = 127, keycenter = 60, lovel = 0, hivel = 127;
	float tuneCents = 0.f, volumeDb = 0.f;
	// A loop that runs through the release and a loop that stops at note-off are
	// different instruments: the first fades out inside the loop, the second
	// leaves it at loop_end and plays the recorded tail -- the string ringing
	// off, the room, the key noise. Collapsing them into one "looping" flag threw
	// that tail away on every sustain-looped library.
	enum { LOOP_NONE = 0, LOOP_CONTINUOUS = 1, LOOP_SUSTAIN = 2 };
	int   loopMode = LOOP_NONE;
	long  loopStart = -1, loopEnd = -1;
	// SFZ `offset` / DecentSampler `start`+`end`: the region is a WINDOW into the
	// file, not the whole file. Libraries that record several takes into one pass
	// and slice them apart afterwards (Marxophone, the Cassette Tape Organ) carry
	// their trims here, so ignoring them plays the wrong audio, or the count-in.
	long  offsetFrames = 0;          // playback starts here, not at 0
	long  endFrames = 0;             // 0 = play to the end of the file
	float pan = 0.f;                 // -1 hard left … +1 hard right
	// DecentSampler pitchKeyTrack="0" (SFZ pitch_keytrack=0): a region that plays
	// at its recorded pitch whatever key triggers it — drums, sound effects.
	bool  keyTrack = true;
	int   seqLength = 1, seqPos = 1; // round-robin
	int   swLast = -1;               // SFZ `sw_last` (keyswitch articulation), -1 = none
	// SFZ `amp_veltrack` as 0..1 (negative = inverted). <0 means the file didn't say —
	// see playVelGain(): we only apply velocity tracking when it's actually stated.
	float velTrack = -2.f;
	// Loaded audio, stored as int16 rather than float: these libraries run to
	// gigabytes (Salamander is 1.8 GB of 24-bit WAV) and float32 doubles that.
	std::vector<int16_t> L, R;
	long  frames = 0; float srcRate = 48000.f;
	bool  loaded = false;            // written last by the loader thread — see startLoader()
	float volGain = 1.f;
	// Amp envelope (SFZ ampeg / DecentSampler): attack·decay·release in seconds, sustain 0..1.
	float egAttack = 0.f, egDecay = 0.f, egSustain = 1.f, egRelease = 0.f;
};

// Velocity → gain. SFZ's amp_veltrack blends between "velocity does nothing" (0)
// and a square law, vel²/127² (100); negative values invert the curve. When the
// instrument doesn't state it we keep Play's historical linear response, because
// Record bakes velocity into the samples it writes.
static inline float playVelGain(int vel, float velTrack) {
	float x = clamp(vel / 127.f, 0.f, 1.f);
	if (velTrack < -1.f) return x;                    // opcode absent → linear
	float a = std::fabs(velTrack);
	if (velTrack < 0.f) x = 1.f - x;
	return clamp((1.f - a) + a * x * x, 0.f, 1.f);
}

struct Instrument {
	std::string name, dir, srcPath;
	// True on the first instrument of a load. One .dsbundle can yield several
	// instruments off one user action, and the patch stores what the user picked
	// — so only the head of each group is written back, or reloading a 6-preset
	// bundle would ask for it six times and come back with 36 instruments.
	bool srcHead = true;
	std::vector<SfzRegion> regions;
};

// ─── SFZ parsing ─────────────────────────────────────────────────────────────
static void sfzStripComments(std::string& s) {
	for (size_t i = 0; i + 1 < s.size(); i++)
		if (s[i] == '/' && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') s[i++] = ' '; }
}
// SFZ mandates '\' as the sample-path separator (the format grew up on Windows),
// and plenty of libraries — Salamander among them — write it that way. Nothing
// else on macOS/Linux treats a backslash as a separator, so translate on load.
static void sfzFixSeparators(std::string& s) {
	for (char& c : s) if (c == '\\') c = '/';
}
static int sfzKeyNum(const std::string& v) {
	if (v.empty()) return -1;
	if (isdigit((unsigned char)v[0]) || v[0] == '-') return atoi(v.c_str());
	// note name like c4, f#3, db2
	int step[7] = {9, 11, 0, 2, 4, 5, 7};   // a b c d e f g
	int c = tolower(v[0]); if (c < 'a' || c > 'g') return -1;
	int semi = step[c - 'a']; size_t i = 1;
	if (i < v.size() && (v[i] == '#' || v[i] == 's')) { semi++; i++; }
	else if (i < v.size() && v[i] == 'b') { semi--; i++; }
	int oct = (i < v.size()) ? atoi(v.c_str() + i) : 4;
	return clamp(semi + (oct + 1) * 12, 0, 127);   // c4 = 60
}

static bool parseSfz(const std::string& path, Instrument& inst) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::string text(n, 0); if (std::fread(&text[0], 1, n, f) != (size_t)n) {} std::fclose(f);
	sfzStripComments(text);

	inst.dir = system::getDirectory(path);
	inst.name = system::getStem(path);
	std::string defaultPath;
	std::map<std::string, std::string> gGlobal, gGroup, gRegion;
	bool inRegion = false;
	int  swDefault = -1;   // file-wide `sw_default` — the keyswitch that's active at load

	auto geti = [](std::map<std::string, std::string>& m, const char* k, int d) {
		auto it = m.find(k); return it == m.end() ? d : atoi(it->second.c_str()); };
	auto getf = [](std::map<std::string, std::string>& m, const char* k, float d) {
		auto it = m.find(k); return it == m.end() ? d : (float)atof(it->second.c_str()); };

	auto flush = [&]() {
		if (!inRegion) return;
		std::map<std::string, std::string> m = gGlobal;
		for (auto& kv : gGroup) m[kv.first] = kv.second;
		for (auto& kv : gRegion) m[kv.first] = kv.second;
		if (m.count("sw_default")) swDefault = sfzKeyNum(m["sw_default"]);   // capture before the filters below
		// Regions Play has no state to drive are dropped rather than mapped as
		// ordinary notes — otherwise they overlap the sustains and get picked by
		// the round-robin, so every other note-on plays a release resonance or a
		// pedal noise instead of the note.
		std::string trig = m.count("trigger") ? m["trigger"] : "attack";
		if (trig != "attack") return;                                  // release / first / legato
		for (auto& kv : m)                                             // CC-gated (pedal noises)
			if (kv.first.compare(0, 6, "on_loc") == 0 || kv.first.compare(0, 6, "on_hic") == 0) return;

		SfzRegion r;
		if (m.count("sample")) r.sample = m["sample"];
		if (m.count("sw_last")) r.swLast = sfzKeyNum(m["sw_last"]);
		if (m.count("key")) { int k = sfzKeyNum(m["key"]); r.lokey = r.hikey = r.keycenter = k; }
		if (m.count("lokey")) r.lokey = sfzKeyNum(m["lokey"]);
		if (m.count("hikey")) r.hikey = sfzKeyNum(m["hikey"]);
		if (m.count("pitch_keycenter")) r.keycenter = sfzKeyNum(m["pitch_keycenter"]);
		r.lovel = geti(m, "lovel", 0); r.hivel = geti(m, "hivel", 127);
		r.tuneCents = getf(m, "tune", 0.f);
		r.volumeDb = getf(m, "volume", 0.f) + getf(m, "global_volume", 0.f);
		r.volGain = std::pow(10.f, r.volumeDb / 20.f);
		if (m.count("amp_veltrack")) r.velTrack = clamp(getf(m, "amp_veltrack", 100.f) / 100.f, -1.f, 1.f);
		r.offsetFrames = std::max(0L, (long)geti(m, "offset", 0));
		r.endFrames    = std::max(0L, (long)geti(m, "end", 0));
		r.pan          = clamp(getf(m, "pan", 0.f) / 100.f, -1.f, 1.f);
		if (m.count("pitch_keytrack")) r.keyTrack = (geti(m, "pitch_keytrack", 100) != 0);
		r.egAttack  = getf(m, "ampeg_attack", 0.f);
		r.egDecay   = getf(m, "ampeg_decay", 0.f);
		r.egSustain = clamp(getf(m, "ampeg_sustain", 100.f) / 100.f, 0.f, 1.f);
		r.egRelease = getf(m, "ampeg_release", 0.f);
		std::string lm = m.count("loop_mode") ? m["loop_mode"] : "";
		r.loopMode = (lm == "loop_sustain")    ? SfzRegion::LOOP_SUSTAIN
		           : (lm == "loop_continuous") ? SfzRegion::LOOP_CONTINUOUS
		                                       : SfzRegion::LOOP_NONE;
		r.loopStart = m.count("loop_start") ? atol(m["loop_start"].c_str()) : -1;
		r.loopEnd   = m.count("loop_end")   ? atol(m["loop_end"].c_str())   : -1;
		r.seqLength = geti(m, "seq_length", 1); r.seqPos = geti(m, "seq_position", 1);
		if (!r.sample.empty()) inst.regions.push_back(std::move(r));
	};

	// tokenize on whitespace. A token WITHOUT '=' (and not a <header>) is a
	// continuation of the previous opcode's value — this is how SFZ allows spaces
	// in a value, most importantly sample paths (e.g. `sample=My Long Pad.wav`).
	size_t i = 0; int ctx = 0;   // 1 global, 2 group, 3 region, 4 control
	std::string* curVal = nullptr;
	while (i < text.size()) {
		while (i < text.size() && isspace((unsigned char)text[i])) i++;
		if (i >= text.size()) break;
		size_t j = i; while (j < text.size() && !isspace((unsigned char)text[j])) j++;
		std::string tok = text.substr(i, j - i); i = j;
		if (tok[0] == '<') {
			curVal = nullptr;
			if (tok == "<region>") { flush(); gRegion.clear(); inRegion = true; ctx = 3; }
			else if (tok == "<group>") { flush(); inRegion = false; gGroup.clear(); ctx = 2; }
			else if (tok == "<global>") { flush(); inRegion = false; gGlobal.clear(); ctx = 1; }
			else { flush(); inRegion = false; ctx = 4; }
		} else {
			size_t eq = tok.find('=');
			if (eq == std::string::npos) {   // continuation (value contained a space)
				if (curVal) { *curVal += ' '; *curVal += tok; }
				continue;
			}
			std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
			curVal = nullptr;
			if (ctx == 4 && k == "default_path") { defaultPath = v; curVal = &defaultPath; }
			else if (ctx == 1) { gGlobal[k] = v; curVal = &gGlobal[k]; }
			else if (ctx == 2) { gGroup[k] = v; curVal = &gGroup[k]; }
			else if (ctx == 3) { gRegion[k] = v; curVal = &gRegion[k]; }
		}
	}
	flush();
	if (!defaultPath.empty()) {
		sfzFixSeparators(defaultPath);
		if (defaultPath.back() != '/') defaultPath += '/';
		for (auto& r : inst.regions) r.sample = defaultPath + r.sample;
	}
	for (auto& r : inst.regions) sfzFixSeparators(r.sample);
	// Keyswitched libraries stack every articulation on the same keys. With no
	// keyswitch state to track, keep only the one the file says starts active.
	if (swDefault >= 0) {
		inst.regions.erase(std::remove_if(inst.regions.begin(), inst.regions.end(),
			[&](const SfzRegion& r) { return r.swLast >= 0 && r.swLast != swDefault; }), inst.regions.end());
	}
	return !inst.regions.empty();
}

// ─── DecentSampler (.dspreset) parsing ───────────────────────────────────────
// XML: <DecentSampler><groups ...><group ...><sample path= rootNote= loNote= .../>.
// Attributes cascade groups→group→sample; tuning and volume accumulate across
// levels (DecentSampler convention), everything else overrides. Mapped onto the
// same SfzRegion so the voice engine is shared.
static std::map<std::string, std::string> dsAttrs(const std::string& s) {
	std::map<std::string, std::string> m;
	size_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && !(isalpha((unsigned char)s[i]) || s[i] == '_')) i++;
		if (i >= s.size()) break;
		size_t j = i;
		while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '-' || s[j] == ':')) j++;
		std::string key = s.substr(i, j - i);
		size_t k = j; while (k < s.size() && isspace((unsigned char)s[k])) k++;
		if (k >= s.size() || s[k] != '=') { i = j + 1; continue; }
		k++; while (k < s.size() && isspace((unsigned char)s[k])) k++;
		if (k >= s.size()) break;
		char q = s[k];
		if (q == '"' || q == '\'') {
			k++; size_t e = k; while (e < s.size() && s[e] != q) e++;
			m[key] = s.substr(k, e - k); i = e + 1;
		} else {
			size_t e = k; while (e < s.size() && !isspace((unsigned char)s[e]) && s[e] != '/' && s[e] != '>') e++;
			m[key] = s.substr(k, e - k); i = e;
		}
	}
	return m;
}
static float dsVolToDb(const std::string& v) {
	if (v.empty()) return 0.f;
	size_t n = v.size();
	if (n >= 2 && (v[n - 1] == 'B' || v[n - 1] == 'b') && (v[n - 2] == 'd' || v[n - 2] == 'D'))
		return (float)atof(v.c_str());           // "-6dB"
	float g = (float)atof(v.c_str());             // linear gain
	return g > 0.f ? 20.f * std::log10(g) : 0.f;
}

static bool parseDecentSampler(const std::string& path, Instrument& inst) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::string text(n, 0); if (std::fread(&text[0], 1, n, f) != (size_t)n) {} std::fclose(f);
	for (size_t p = text.find("<!--"); p != std::string::npos; p = text.find("<!--")) {
		size_t e = text.find("-->", p);
		if (e == std::string::npos) { text.erase(p); break; }
		text.erase(p, e + 3 - p);
	}
	inst.dir = system::getDirectory(path);
	inst.name = system::getStem(path);

	std::map<std::string, std::string> gGlobal, gGroup;
	size_t i = 0;
	while ((i = text.find('<', i)) != std::string::npos) {
		if (text.compare(i, 2, "<?") == 0 || text.compare(i, 2, "<!") == 0) {
			size_t e = text.find('>', i); if (e == std::string::npos) break; i = e + 1; continue;
		}
		size_t end = text.find('>', i); if (end == std::string::npos) break;
		std::string tag = text.substr(i + 1, end - i - 1); i = end + 1;
		size_t ns = 0; while (ns < tag.size() && isspace((unsigned char)tag[ns])) ns++;
		bool closing = (ns < tag.size() && tag[ns] == '/'); if (closing) ns++;
		size_t ne = ns; while (ne < tag.size() && !isspace((unsigned char)tag[ne]) && tag[ne] != '/' && tag[ne] != '>') ne++;
		std::string name = tag.substr(ns, ne - ns), body = tag.substr(ne);

		if (closing) { if (name == "group") gGroup.clear(); continue; }
		if (name == "groups") { gGlobal = dsAttrs(body); continue; }
		if (name == "group") { gGroup = dsAttrs(body); continue; }
		if (name != "sample") continue;

		std::map<std::string, std::string> sa = dsAttrs(body);
		auto pick = [&](const char* k) -> std::string {
			auto it = sa.find(k); if (it != sa.end()) return it->second;
			it = gGroup.find(k); if (it != gGroup.end()) return it->second;
			it = gGlobal.find(k); if (it != gGlobal.end()) return it->second;
			return std::string();
		};
		std::string sp = pick("path"); if (sp.empty()) continue;
		{ std::string t = pick("trigger"); if (!t.empty() && t != "attack") continue; }   // as in parseSfz()
		sfzFixSeparators(sp);
		SfzRegion r;
		r.sample = sp;
		int root = -1;
		{ std::string s = pick("rootNote"); if (s.empty()) s = pick("rootKey"); if (s.empty()) s = pick("pitchKeyCenter"); if (!s.empty()) root = sfzKeyNum(s); }
		std::string loS = pick("loNote"); if (loS.empty()) loS = pick("loKey");
		std::string hiS = pick("hiNote"); if (hiS.empty()) hiS = pick("hiKey");
		r.lokey = !loS.empty() ? sfzKeyNum(loS) : (root >= 0 ? root : 0);
		r.hikey = !hiS.empty() ? sfzKeyNum(hiS) : (root >= 0 ? root : 127);
		r.keycenter = root >= 0 ? root : r.lokey;
		{ std::string s = pick("loVel"); r.lovel = s.empty() ? 0 : atoi(s.c_str()); }
		{ std::string s = pick("hiVel"); r.hivel = s.empty() ? 127 : atoi(s.c_str()); }
		float tuning = 0.f, volDb = 0.f;
		std::map<std::string, std::string>* levels[3] = { &gGlobal, &gGroup, &sa };
		for (auto* mp : levels) {
			auto t = mp->find("tuning"); if (t != mp->end()) tuning += (float)atof(t->second.c_str());
			auto v = mp->find("volume"); if (v != mp->end()) volDb += dsVolToDb(v->second);
		}
		r.tuneCents = tuning * 100.f;
		r.volumeDb = volDb; r.volGain = std::pow(10.f, volDb / 20.f);
		// DecentSampler states looping with loopEnabled, and names the kind in
		// loopMode. A file may carry loopMode="sustain" with no loopEnabled at
		// all, so naming the mode counts as enabling it.
		{
			std::string en = pick("loopEnabled"), md = string::lowercase(pick("loopMode"));
			bool on = (en == "true" || en == "1") || md == "sustain" || md == "forward";
			r.loopMode = !on ? SfzRegion::LOOP_NONE
			           : (md == "sustain") ? SfzRegion::LOOP_SUSTAIN
			                               : SfzRegion::LOOP_CONTINUOUS;
		}
		{ std::string s = pick("loopStart"); if (!s.empty()) r.loopStart = atol(s.c_str()); }
		{ std::string s = pick("loopEnd"); if (!s.empty()) r.loopEnd = atol(s.c_str()); }
		{ std::string s = pick("seqLength"); r.seqLength = s.empty() ? 1 : atoi(s.c_str()); }
		{ std::string s = pick("seqPosition"); r.seqPos = s.empty() ? 1 : atoi(s.c_str()); }
		// start/end are written as floats by several editors ("83859.000"), so
		// they are parsed as such and then truncated — atol() would stop at the
		// point on some, and reading them as ints is how a trim silently becomes
		// "play the whole file".
		{ std::string s = pick("start"); if (!s.empty()) r.offsetFrames = std::max(0.0, atof(s.c_str())); }
		{ std::string s = pick("end");   if (!s.empty()) r.endFrames    = std::max(0.0, atof(s.c_str())); }
		{ std::string s = pick("pan");   if (!s.empty()) r.pan = clamp((float)atof(s.c_str()) / 100.f, -1.f, 1.f); }
		{ std::string s = pick("pitchKeyTrack"); if (!s.empty()) r.keyTrack = (atof(s.c_str()) != 0.0); }
		// DecentSampler states velocity→amplitude as 0..1 and defaults to 1;
		// Play's velTrack is the same idea on SFZ's 0..100 scale. A library that
		// says ampVelTrack="0" has baked its dynamics into the layers already,
		// and scaling them again by velocity is what makes it play quiet.
		{ std::string s = pick("ampVelTrack"); if (!s.empty()) r.velTrack = clamp((float)atof(s.c_str()), -1.f, 1.f); }
		// Amp envelope (DecentSampler: seconds; sustain 0..1)
		{ std::string s = pick("attack");  if (!s.empty()) r.egAttack  = (float)atof(s.c_str()); }
		{ std::string s = pick("decay");   if (!s.empty()) r.egDecay   = (float)atof(s.c_str()); }
		{ std::string s = pick("sustain"); if (!s.empty()) r.egSustain = clamp((float)atof(s.c_str()), 0.f, 1.f); }
		{ std::string s = pick("release"); if (!s.empty()) r.egRelease = (float)atof(s.c_str()); }
		inst.regions.push_back(std::move(r));
	}
	return !inst.regions.empty();
}

// A .dsbundle is a folder holding a .dspreset (+ a Samples/ folder) — and often
// SEVERAL .dspresets, one per articulation (Marxophone ships 6, Phase8 ships 25).
// Returns all of them, sorted, because `system::getEntries` hands back directory
// order: picking "the first one" meant the bundle loaded whichever patch the
// filesystem happened to name first, and said nothing about the others.
static std::vector<std::string> findDspresetsIn(const std::string& dir, int depth = 2);
static std::string findDspresetIn(const std::string& dir) {
	std::vector<std::string> v = findDspresetsIn(dir);
	return v.empty() ? std::string() : v[0];
}

// A .dslibrary is a ZIP of a .dspreset plus its samples — the format DecentSampler
// packs are actually distributed in. Rack's own unarchiver runs `tar --zstd` and
// nothing else, and no libarchive or zlib symbol is exported from libRack, so a
// plugin that wants to read a zip has to carry the decompressor: hence miniz,
// vendored beside dr_wav and dr_flac for the same reason.
//
// Entry names come out of an untrusted archive, so they are validated rather
// than trusted: an absolute path or a ".." segment would let an archive write
// anywhere the user can write, and unzipping a sample pack is not a thing that
// should be able to do that.
static bool dsSafeEntryName(const std::string& n) {
	if (n.empty() || n[0] == '/' || n[0] == '\\') return false;
	if (n.size() > 1 && n[1] == ':') return false;              // C:\…
	size_t i = 0;
	while (i < n.size()) {
		size_t j = n.find_first_of("/\\", i);
		std::string seg = n.substr(i, (j == std::string::npos ? n.size() : j) - i);
		if (seg == "..") return false;
		if (j == std::string::npos) break;
		i = j + 1;
	}
	return true;
}

static bool unzipToDirectory(const std::string& zipPath, const std::string& dir, std::string& err) {
	mz_zip_archive z;
	std::memset(&z, 0, sizeof(z));
	if (!mz_zip_reader_init_file(&z, zipPath.c_str(), 0)) {
		err = "not a readable zip";
		WARN("Play: %s is not a readable zip archive", zipPath.c_str());
		return false;
	}
	mz_uint n = mz_zip_reader_get_num_files(&z);
	int wrote = 0, skipped = 0;
	for (mz_uint i = 0; i < n; i++) {
		mz_zip_archive_file_stat st;
		if (!mz_zip_reader_file_stat(&z, i, &st)) { skipped++; continue; }
		std::string name = st.m_filename;
		for (char& c : name) if (c == '\\') c = '/';
		if (!dsSafeEntryName(name)) {
			WARN("Play: refusing archive entry \"%s\" — it points outside the unpack folder", st.m_filename);
			skipped++; continue;
		}
		std::string out = system::join(dir, name);
		if (mz_zip_reader_is_file_a_directory(&z, i)) { system::createDirectories(out); continue; }
		// __MACOSX/ resource forks and .DS_Store ride along in most of these
		// archives; they are not samples and writing them out is just litter.
		if (name.compare(0, 9, "__MACOSX/") == 0 || system::getFilename(name) == ".DS_Store") continue;
		system::createDirectories(system::getDirectory(out));
		if (mz_zip_reader_extract_to_file(&z, i, out.c_str(), 0)) wrote++;
		else { WARN("Play: could not write \"%s\"", out.c_str()); skipped++; }
	}
	mz_zip_reader_end(&z);
	if (!wrote) { err = "archive was empty"; return false; }
	if (skipped) WARN("Play: %d of %u archive entries skipped", skipped, (unsigned)n);
	return true;
}

static std::string unpackDslibrary(const std::string& path, std::string& err) {
	std::string dir = asset::user("SignalFunctionSet/dslibrary/" + system::getStem(path));
	if (!findDspresetIn(dir).empty()) return dir;               // already unpacked
	system::createDirectories(dir);
	if (!unzipToDirectory(path, dir, err)) { system::removeRecursively(dir); return ""; }
	if (findDspresetIn(dir).empty()) {
		// It unpacked, but there is no instrument in it.
		WARN("Play: no .dspreset inside \"%s\"", path.c_str());
		err = "no .dspreset inside";
		system::removeRecursively(dir);
		return "";
	}
	return dir;
}

static std::vector<std::string> findDspresetsIn(const std::string& dir, int depth) {
	std::vector<std::string> out;
	if (!system::isDirectory(dir)) return out;
	for (const std::string& e : system::getEntries(dir))
		if (string::lowercase(system::getExtension(e)) == ".dspreset") out.push_back(e);
	if (out.empty() && depth > 0) {                          // then look further in
		for (const std::string& e : system::getEntries(dir)) {
			if (!system::isDirectory(e)) continue;
			std::vector<std::string> sub = findDspresetsIn(e, depth - 1);
			out.insert(out.end(), sub.begin(), sub.end());
			if (!out.empty()) break;                         // one folder's worth
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

// Decode one region's file to interleaved int16. WAV and FLAC are both common in
// SFZ libraries (VCSL ships FLAC throughout); we pick by extension and fall back
// to the other decoder, since a mislabelled extension is easier to hit than a
// genuinely broken file. Caller frees with std::free().
static int16_t* playDecodeFile(const std::string& p, unsigned int& ch, unsigned int& sr, uint64_t& frames) {
	std::string ext = string::lowercase(system::getExtension(p));
	bool flacFirst = (ext == ".flac");
	for (int attempt = 0; attempt < 2; attempt++) {
		bool tryFlac = (attempt == 0) ? flacFirst : !flacFirst;
		if (tryFlac) {
			drflac_uint64 n = 0;
			drflac_int16* d = drflac_open_file_and_read_pcm_frames_s16(p.c_str(), &ch, &sr, &n, NULL);
			if (d && ch > 0 && n > 0) { frames = n; return (int16_t*)d; }
			if (d) drflac_free(d, NULL);
		} else {
			drwav_uint64 n = 0;
			drwav_int16* d = drwav_open_file_and_read_pcm_frames_s16(p.c_str(), &ch, &sr, &n, NULL);
			if (d && ch > 0 && n > 0) { frames = n; return (int16_t*)d; }
			if (d) drwav_free(d, NULL);
		}
	}
	return nullptr;
}

static void loadRegionAudio(SfzRegion& r, const std::string& dir) {
	std::string p = system::join(dir, r.sample);
	unsigned int ch = 0, sr = 0; uint64_t frames = 0;
	int16_t* data = playDecodeFile(p, ch, sr, frames);
	if (!data) return;
	r.srcRate = (float)sr; r.frames = (long)frames;
	r.L.resize(frames); r.R.resize(frames);
	for (uint64_t i = 0; i < frames; i++) {
		r.L[i] = data[i * ch];
		r.R[i] = (ch > 1) ? data[i * ch + 1] : data[i * ch];
	}
	std::free(data);
	if (r.endFrames > 0 && r.endFrames < r.frames) r.frames = r.endFrames;
	if (r.loopEnd <= 0 || r.loopEnd > r.frames) r.loopEnd = r.frames;
	if (r.loopStart < 0) r.loopStart = 0;
	if (r.offsetFrames >= r.frames) r.offsetFrames = 0;
	// Publish the audio before the flag: the engine thread reads `loaded` first
	// and only then touches L/R, so this pairs with the acquire in process().
	std::atomic_thread_fence(std::memory_order_release);
	r.loaded = true;
}

// ─── Module ──────────────────────────────────────────────────────────────────
struct Play;

// The INSTR knob used to span a fixed 0..15 whatever was loaded, so with three
// instruments thirteen of its sixteen positions did nothing and the tooltip read
// "Instrument: 7" — a number that named nothing. `maxValue` now follows the list
// (see Play::syncInstrParam) and the tooltip says which instrument that is.
struct PlayInstrQuantity : ParamQuantity {
	std::string getDisplayValueString() override;
	void setDisplayValueString(std::string s) override;
};
struct PlayDisplay : OpaqueWidget {
	Play* module = nullptr;
	std::shared_ptr<Font> font;
	bool dragging = false;
	Vec dragPos;

	// The second tab used to be an 88-key map of the loaded instrument. It was a
	// picture of something the grid already shows in colour, on a surface you
	// could not play as well as the grid — while the thing you actually come to
	// this module to do, put an instrument in it, was buried in the right-click
	// menu. It is a list and a LOAD button now.
	int   listScroll = 0;         // first visible row
	int   hoverRow = -1;          // row under the cursor, or -1
	bool  hoverLoad = false;
	bool  loadReq = false;        // set by the button, acted on in step()
	int   removeReq = -1;

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawTabs(NVGcontext* vg, int view);
	void drawList(NVGcontext* vg);
	void drawGrid(NVGcontext* vg, const uint8_t* mapped, const uint8_t* root, const uint8_t* playing,
	              int layout, int gRoot, int gScale, int gBase);

	// Geometry (px, widget-local) — shared by draw + hit-test
	void tabRects(Rect out[2]) const {
		float tw = (box.size.x - 12.f - 4.f) * 0.5f;
		out[0] = Rect(Vec(6.f, 4.f), Vec(tw, 15.f));
		out[1] = Rect(Vec(6.f + tw + 4.f, 4.f), Vec(tw, 15.f));
	}
	void gridGeom(float& x0, float& y0, float& pad) const {
		float top = 32.f, marg = 6.f;
		float aw = box.size.x - 2.f * marg, ah = box.size.y - 6.f - top;
		pad = std::min(aw / GRID_COLS, ah / GRID_ROWS);
		x0 = (box.size.x - pad * GRID_COLS) * 0.5f;
		y0 = top + std::max(0.f, (ah - pad * GRID_ROWS) * 0.5f);
	}
	// LOAD sits at the foot, out of the list's way: it is a thing you reach for
	// once and then stop looking at, so it does not need the height a row does.
	Rect loadRect() const { return Rect(Vec(6.f, box.size.y - 20.f), Vec(box.size.x - 12.f, 14.f)); }
	static constexpr float ROW_H = 17.f, LIST_TOP = 34.f;
	int   rowsVisible() const { return std::max(1, (int)((box.size.y - 24.f - LIST_TOP) / ROW_H)); }
	int   hitTab(Vec p) const { Rect r[2]; tabRects(r); for (int i = 0; i < 2; i++) if (r[i].contains(p)) return i; return -1; }
	int   hitPad(Vec p) const;    // grid pad under cursor → MIDI note or -1
	int   rowAt(Vec p) const;     // instrument row under cursor, or -1
	// The remove cross only appears on the row the cursor is over: a delete
	// target sitting permanently on every row is a delete waiting to be misclicked.
	Rect  removeRect(int visRow) const {
		return Rect(Vec(box.size.x - 22.f, LIST_TOP + visRow * ROW_H + 2.f), Vec(15.f, ROW_H - 4.f));
	}

	void step() override;
	void onButton(const ButtonEvent& e) override;
	void onHover(const HoverEvent& e) override;
	void onLeave(const LeaveEvent& e) override;
	void onHoverScroll(const HoverScrollEvent& e) override;
	void onDragStart(const DragStartEvent& e) override { OpaqueWidget::onDragStart(e); }
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
};

struct Play : Module {
	enum ParamId { INSTR_PARAM, LEVEL_PARAM, PARAMS_LEN };
	enum InputId { VOCT_INPUT, GATE_INPUT, VEL_INPUT, INSTR_CV_INPUT, LEVEL_CV_INPUT, INPUTS_LEN };
	enum OutputId { L_OUTPUT, R_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	struct Voice {
		bool active = false, held = false;
		int chan = -1, instr = -1, reg = -1, note = 60;
		double pos = 0, ratio = 1; float amp = 1.f, env = 0.f;
		int   envStage = 0;                                 // 0 attack, 1 decay, 2 sustain, 3 release
		float cA = 1.f, cD = 1.f, cR = 1.f, susL = 1.f;     // one-pole coefficients resolved at note-on
	};
	Voice voices[PLAY_MAX_VOICES];
	std::vector<Instrument> instruments;
	int curInstrument = 0;
	int rrCounter = 0;                       // rotates through round-robin takes
	bool oneShot = false;                   // true = play samples through, ignoring gate-off (drums)
	// Amp-envelope mode: Off = fast anti-click AR (legacy); SFZ = the instrument's own ampeg
	// (falls back to Off's shape if the region defines none); Default = one fixed smooth ADSR.
	enum EnvMode { ENV_OFF, ENV_SFZ, ENV_DEFAULT };
	int envMode = ENV_OFF;
	bool suspended = false;                 // true while the instrument list is being restructured
	bool gateWas[PLAY_MAX_VOICES] = {};

	// Sample decoding runs on its own thread — a full piano library is gigabytes,
	// and doing that inline would freeze Rack's UI for the duration. Regions are
	// silent until their `loaded` flag flips, so the instrument fades in as it
	// decodes. Every structural change to `instruments` joins the loader first,
	// which is what keeps the captured Instrument& valid.
	std::thread loader;
	std::atomic<bool> loaderRun{false}, loaderCancel{false};
	std::atomic<int>  loadDone{0}, loadTotal{0};

	~Play() { joinLoader(); }

	void joinLoader() {
		if (loader.joinable()) { loaderCancel = true; loader.join(); }
		loaderCancel = false; loaderRun = false;
	}

	// Loads EVERY region that is not loaded yet, across all instruments. It used
	// to take a single instrument index, and since every structural change joins
	// (which cancels) the loader first, loading a second instrument abandoned the
	// first one's remaining regions for good — they stayed silent forever. Worse
	// on patch load, where instruments are added in a loop, so all but the last
	// were cancelled part-way. Scanning for unloaded regions makes a cancel a
	// pause rather than an abort.
	void startLoader() {
		int tot = 0;
		for (auto& in : instruments)
			for (auto& r : in.regions) if (!r.loaded) tot++;
		if (tot == 0) return;
		loadDone = 0;
		loadTotal = tot;
		loaderRun = true;
		loader = std::thread([this]() {
			int failed = 0;
			for (auto& in : instruments) {
				for (auto& r : in.regions) {
					if (loaderCancel) { loaderRun = false; return; }
					if (r.loaded) continue;
					loadRegionAudio(r, in.dir);
					if (!r.loaded && ++failed <= 8)
						WARN("Play: could not load sample \"%s\"", system::join(in.dir, r.sample).c_str());
					loadDone++;
				}
			}
			if (failed) WARN("Play: %d samples failed to load", failed);
			loaderRun = false;
		});
	}

	// Keyboard surface view (tab-switched on the display) + Push grid config
	// 0 = instrument list, 1 = pad grid. A NEW module opens on the list, because
	// a new Play has nothing in it and the list is where you put something; the
	// choice is persisted, so anyone who moves to the grid stays there.
	int kbView = 0;
	int gridLayout = 0;    // 0 = chromatic 4ths, 1 = in-key
	int gridRoot = 0;      // 0..11 (C..B) — key root / highlight
	int gridScale = 1;     // sfs::SCALES index (default Major)
	int gridBase = 36;     // MIDI note of the bottom-left pad (C2)
	// Mouse audition: widget writes uiNote (-1 = none), audio thread edge-detects
	int uiNote = -1, uiNotePrev = -1;

	// display mirrors
	char dispName[48] = {0}, dispInfo[48] = {0}, dispErr[64] = {0};
	uint8_t dispMapped[128] = {0}, dispRoot[128] = {0}, dispPlaying[128] = {0};
	int dispCount = 0, statusDiv = 0;

	Play() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Configured wide and narrowed later, never the other way round:
		// Module::paramsFromJson() restores through ParamQuantity, which CLAMPS to
		// the range in force at that moment — and it runs BEFORE dataFromJson(),
		// where the instruments are actually loaded. Starting narrow would clip a
		// saved knob position down to 1 on every patch load.
		configParam<PlayInstrQuantity>(INSTR_PARAM, 0.f, 15.f, 0.f, "Instrument");
		getParamQuantity(INSTR_PARAM)->snapEnabled = true;
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level");
		configInput(VOCT_INPUT, "V/oct (poly)");
		configInput(GATE_INPUT, "Gate (poly)");
		configInput(VEL_INPUT, "Velocity (poly)");
		configInput(INSTR_CV_INPUT, "Instrument select CV");
		configInput(LEVEL_CV_INPUT, "Level CV (VCA)");
		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");
		refreshDisplay();
	}

	SfzRegion* findRegion(int inst, int note, int vel) {
		if (inst < 0 || inst >= (int)instruments.size()) return nullptr;
		for (auto& r : instruments[inst].regions)
			if (r.loaded && note >= r.lokey && note <= r.hikey && vel >= r.lovel && vel <= r.hivel) return &r;
		return nullptr;
	}

	void noteOn(int chan, int note, int vel) {
		if (curInstrument < 0 || curInstrument >= (int)instruments.size()) return;
		auto& regs = instruments[curInstrument].regions;
		// collect every region matching this key+velocity (>1 = round-robin takes)
		std::vector<int> matches;
		for (int i = 0; i < (int)regs.size(); i++) {
			SfzRegion& r = regs[i];
			if (r.loaded && note >= r.lokey && note <= r.hikey && vel >= r.lovel && vel <= r.hivel) matches.push_back(i);
		}
		// A library whose velocity layers do not reach 127 is common — Phase8's
		// MidVel patches stop at 98, and Play's own default velocity is 100, so
		// the instrument loaded, mapped the whole keyboard, and made no sound at
		// all. Nothing on screen could have told you why. Fall back to the
		// nearest layer on that key rather than to silence: a note the user asked
		// for, played slightly outside its recorded dynamic, beats nothing.
		if (matches.empty()) {
			int best = 1 << 30;
			for (int i = 0; i < (int)regs.size(); i++) {
				SfzRegion& r = regs[i];
				if (!r.loaded || note < r.lokey || note > r.hikey) continue;
				int d = (vel < r.lovel) ? (r.lovel - vel) : (vel > r.hivel ? vel - r.hivel : 0);
				if (d < best) { best = d; matches.clear(); matches.push_back(i); }
				else if (d == best) matches.push_back(i);
			}
		}
		if (matches.empty()) return;
		std::atomic_thread_fence(std::memory_order_acquire);   // pairs with loadRegionAudio()'s release
		std::sort(matches.begin(), matches.end(), [&](int a, int b) { return regs[a].seqPos < regs[b].seqPos; });
		int regIdx = matches[(matches.size() > 1) ? (rrCounter++ % (int)matches.size()) : 0];
		SfzRegion* r = &regs[regIdx];
		int slot = -1;
		for (int i = 0; i < PLAY_MAX_VOICES; i++) if (!voices[i].active) { slot = i; break; }
		if (slot < 0) { double best = -1; for (int i = 0; i < PLAY_MAX_VOICES; i++) if (voices[i].pos > best) { best = voices[i].pos; slot = i; } }
		Voice& v = voices[slot];
		v.active = true; v.held = true; v.chan = chan; v.instr = curInstrument; v.reg = regIdx; v.note = note;
		v.pos = (double)r->offsetFrames; v.amp = playVelGain(vel, r->velTrack);
		v.envStage = 0;                                                   // attack from wherever env is (smooth on steal)
		double semis = (r->keyTrack ? (note - r->keycenter) : 0) + r->tuneCents / 100.0;
		v.ratio = std::pow(2.0, semis / 12.0) * (r->srcRate / APP->engine->getSampleRate());
		resolveEnv(v, *r, (float)APP->engine->getSampleRate());
	}

	// Resolve a voice's ADSR (one-pole coefficients) from the current envelope mode + region.
	void resolveEnv(Voice& v, const SfzRegion& r, float sr) {
		float A, D, S, R;
		bool sfzHasEnv = (r.egAttack > 0.f || r.egDecay > 0.f || r.egRelease > 0.f || r.egSustain < 1.f);
		switch (envMode) {
			default:
			case ENV_OFF:     A = 0.002f; D = 0.f; S = 1.f; R = 0.030f; break;
			case ENV_SFZ:     if (sfzHasEnv) { A = r.egAttack; D = r.egDecay; S = r.egSustain; R = r.egRelease; }
			                  else           { A = 0.002f; D = 0.f; S = 1.f; R = 0.030f; }   // no ampeg → Off shape
			                  break;
			case ENV_DEFAULT: A = 0.005f; D = 0.f; S = 1.f; R = 0.200f; break;
		}
		A = std::max(A, 0.001f); R = std::max(R, 0.002f);   // anti-click floors so A/R=0 doesn't click
		auto coef = [&](float t) { return 1.f - std::exp(-1.f / (std::max(t, 1e-5f) * sr)); };
		v.cA = coef(A); v.cD = (D > 1e-5f) ? coef(D) : 1.f; v.cR = coef(R); v.susL = clamp(S, 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		float sel = params[INSTR_PARAM].getValue();
		if (inputs[INSTR_CV_INPUT].isConnected()) sel += inputs[INSTR_CV_INPUT].getVoltage() / 10.f * 15.f;
		curInstrument = clamp((int)std::round(sel), 0, std::max(0, (int)instruments.size() - 1));

		if (suspended || instruments.empty()) {
			outputs[L_OUTPUT].setVoltage(0.f); outputs[R_OUTPUT].setVoltage(0.f);
			return;
		}

		int nch = std::max(1, std::max(inputs[VOCT_INPUT].getChannels(), inputs[GATE_INPUT].getChannels()));
		for (int c = 0; c < nch; c++) {
			bool g = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
			if (g && !gateWas[c]) {
				// getPolyVoltage → a mono V/oct or velocity cable applies to every voice
				int note = clamp((int)std::round(inputs[VOCT_INPUT].getPolyVoltage(c) * 12.f + 60.f), 0, 127);
				int vel  = inputs[VEL_INPUT].isConnected()
					? clamp((int)std::round(inputs[VEL_INPUT].getPolyVoltage(c) * 12.7f), 1, 127) : 100;
				noteOn(c, note, vel);
			} else if (!g && gateWas[c]) {
				for (auto& v : voices) if (v.active && v.chan == c) v.held = false;
			}
			gateWas[c] = g;
		}

		// Mouse audition from the on-screen keyboard / grid (reserved channel)
		if (uiNote != uiNotePrev) {
			for (auto& v : voices) if (v.active && v.chan == GRID_UI_CHAN) v.held = false;
			if (uiNote >= 0) noteOn(GRID_UI_CHAN, uiNote, 100);
			uiNotePrev = uiNote;
		}

		float outL = 0.f, outR = 0.f;
		for (auto& v : voices) {
			if (!v.active) continue;
			Instrument& in = instruments[v.instr];
			if (v.reg < 0 || v.reg >= (int)in.regions.size()) { v.active = false; continue; }
			SfzRegion& r = in.regions[v.reg];
			long i0 = (long)v.pos;
			if (i0 < 0 || i0 >= r.frames - 1) { v.active = false; continue; }
			float f = (float)(v.pos - i0);
			const float S16 = 1.f / 32768.f;
			float sl = (r.L[i0] * (1.f - f) + r.L[i0 + 1] * f) * S16;
			float sr = (r.R[i0] * (1.f - f) + r.R[i0 + 1] * f) * S16;
			// Per-voice ADSR (coefficients resolved at note-on). Gate-off starts the
			// release; one-shot mode ignores gate-off and plays the sample through.
			if (!v.held && !oneShot && v.envStage < 3) v.envStage = 3;
			switch (v.envStage) {
				case 0: v.env += (1.f - v.env) * v.cA;    if (v.env >= 0.999f) { v.env = 1.f; v.envStage = 1; } break;
				case 1: v.env += (v.susL - v.env) * v.cD; if (std::fabs(v.env - v.susL) < 0.001f) { v.env = v.susL; v.envStage = 2; } break;
				case 2: v.env = v.susL; break;
				case 3: v.env += (0.f - v.env) * v.cR; break;
			}
			if (v.envStage == 3 && v.env < 0.0008f) { v.active = false; continue; }
			float ggain = v.amp * v.env * r.volGain;
			// Equal-power, as everywhere else in the plugin. A region pan of 0
			// leaves both gains at 1/√2·√2 = 1, so an unpanned instrument is
			// bit-identical to what it was before panning existed.
			float th = (r.pan + 1.f) * 0.7853981634f;      // (pan+1)·π/4
			outL += sl * ggain * std::cos(th) * 1.4142135624f;
			outR += sr * ggain * std::sin(th) * 1.4142135624f;
			v.pos += v.ratio;
			// A continuous loop keeps wrapping through the release, so the note
			// fades out inside the loop. A SUSTAIN loop only wraps while the note
			// is held: at note-off it runs on past loop_end into the recorded
			// tail, under the release envelope. That is what loop_mode=loop_sustain
			// and DecentSampler's loopMode="sustain" mean, and Play used to treat
			// both kinds the same -- so every sustain-looped library lost its tail.
			// The break happens AT loop_end, not at note-off: the voice finishes
			// the pass it is in, exactly as the spec reads.
			bool wrapping = (r.loopMode == SfzRegion::LOOP_CONTINUOUS)
			              || (r.loopMode == SfzRegion::LOOP_SUSTAIN && v.held);
			if (wrapping && r.loopEnd > r.loopStart && v.pos >= r.loopEnd)
				v.pos -= (r.loopEnd - r.loopStart);
		}
		float lvl = params[LEVEL_PARAM].getValue();
		if (inputs[LEVEL_CV_INPUT].isConnected())          // CV acts as an output VCA
			lvl *= std::max(0.f, inputs[LEVEL_CV_INPUT].getVoltage() * 0.1f);
		outputs[L_OUTPUT].setVoltage(clamp(outL * lvl * 5.f, -10.f, 10.f));
		outputs[R_OUTPUT].setVoltage(clamp(outR * lvl * 5.f, -10.f, 10.f));

		if (++statusDiv >= 256) { statusDiv = 0; refreshDisplay(); }
	}

	void refreshDisplay() {
		std::memset(dispMapped, 0, sizeof(dispMapped));
		std::memset(dispRoot, 0, sizeof(dispRoot));
		std::memset(dispPlaying, 0, sizeof(dispPlaying));
		if (curInstrument >= 0 && curInstrument < (int)instruments.size()) {
			Instrument& in = instruments[curInstrument];
			for (auto& r : in.regions) {
				for (int k = r.lokey; k <= r.hikey && k < 128; k++) if (k >= 0) dispMapped[k] = 1;
				if (r.keycenter >= 0 && r.keycenter < 128) dispRoot[r.keycenter] = 1;
			}
			snprintf(dispName, sizeof(dispName), "%d/%d %s", curInstrument + 1, (int)instruments.size(), in.name.c_str());
			int loaded = 0, tot = (int)in.regions.size();
			for (auto& r : in.regions) if (r.loaded) loaded++;
			if (loaderRun.load())          snprintf(dispInfo, sizeof(dispInfo), "loading %d/%d…", loadDone.load(), loadTotal.load());
			else if (loaded == 0 && tot)   snprintf(dispInfo, sizeof(dispInfo), "no samples found");
			else if (loaded < tot)         snprintf(dispInfo, sizeof(dispInfo), "%d/%d samples missing", tot - loaded, tot);
			else                           snprintf(dispInfo, sizeof(dispInfo), "%d regions", tot);
		} else { snprintf(dispName, sizeof(dispName), "no instrument"); snprintf(dispInfo, sizeof(dispInfo), "load .sfz"); }
		int cnt = 0;
		for (auto& v : voices) if (v.active) { cnt++; if (v.note >= 0 && v.note < 128) dispPlaying[v.note] = 1; }
		dispCount = cnt;
	}

	// A load can fail for half a dozen reasons and every one of them used to end
	// in a log line the user never sees — the module simply carried on showing
	// the instrument it already had. `dispErr` is what the display reads back;
	// it is written here, on the UI thread, and nowhere else.
	void setLoadError(const std::string& e) { snprintf(dispErr, sizeof(dispErr), "%s", e.c_str()); }

	// Fit the INSTR knob's travel to the instruments that exist. Floored at 1
	// rather than 0 because ParamQuantity documents maxValue as "must be greater
	// than minValue", and a zero-width range would leave the knob widget dividing
	// by it. A shrink also pulls the current value in, so a patch that comes back
	// with fewer instruments than it saved does not leave the knob past its stop.
	void syncInstrParam() {
		ParamQuantity* pq = getParamQuantity(INSTR_PARAM);
		if (!pq) return;
		pq->maxValue = (float)clamp((int)instruments.size() - 1, 1, 15);
		if (pq->getValue() > pq->maxValue) pq->setImmediateValue(pq->maxValue);
	}

	void loadInstrument(const std::string& path, bool startNow = true) {
		joinLoader();                         // no structural change while the loader holds a reference
		suspended = true;
		for (auto& v : voices) v.active = false;
		setLoadError("");
		std::string ext = string::lowercase(system::getExtension(path));
		// A .dsbundle is a folder holding a .dspreset + samples — find the presets
		// inside and load them; sample paths resolve relative to the bundle.
		std::string root = path;                  // where samples resolve from
		if (ext == ".dslibrary") {
			std::string err;
			root = unpackDslibrary(path, err);
			if (root.empty()) { setLoadError(err); suspended = false; if (startNow) startLoader(); syncInstrParam(); refreshDisplay(); return; }
		}
		bool bundle = (ext == ".dsbundle") || (ext == ".dslibrary") || system::isDirectory(root);
		std::vector<std::string> presets;
		if (bundle) presets = findDspresetsIn(root);
		else        presets.push_back(path);
		if (presets.empty()) {
			WARN("Play: no .dspreset inside \"%s\"", path.c_str());
			setLoadError("no .dspreset inside");
			suspended = false; if (startNow) startLoader(); syncInstrParam(); refreshDisplay(); return;
		}

		int added = 0;
		for (const std::string& presetPath : presets) {
			Instrument in;
			std::string pext = string::lowercase(system::getExtension(presetPath));
			bool ok = (pext == ".dspreset") ? parseDecentSampler(presetPath, in) : parseSfz(presetPath, in);
			if (!ok) { WARN("Play: failed to parse \"%s\"", presetPath.c_str()); continue; }
			in.srcPath = path;                    // persist what the user picked (bundle or file)
			in.srcHead = (added == 0);
			// One instrument per preset, named by the preset — a bundle's patches
			// are what the INSTR knob is for, and naming them all after the bundle
			// would make six identical menu entries.
			if (bundle) in.name = (presets.size() > 1) ? system::getStem(presetPath)
			                                           : system::getStem(path);
			instruments.push_back(std::move(in));
			added++;
		}
		if (!added) setLoadError("could not read " + system::getFilename(path));
		suspended = false;                        // regions are still unloaded, so still silent
		if (startNow) startLoader();              // a failed load must not strand the others
		syncInstrParam();
		refreshDisplay();
	}
	void removeInstrument(int idx) {
		if (idx < 0 || idx >= (int)instruments.size()) return;
		joinLoader();
		suspended = true;
		for (auto& v : voices) v.active = false;
		// Only the head of a load group carries its path into the patch, so
		// removing the head of a multi-preset bundle would drop the rest of it on
		// the next reload. Hand the flag to the next one from the same load.
		if (instruments[idx].srcHead && idx + 1 < (int)instruments.size()
		    && instruments[idx + 1].srcPath == instruments[idx].srcPath)
			instruments[idx + 1].srcHead = true;
		instruments.erase(instruments.begin() + idx);
		suspended = false;
		startLoader();                        // anything still unloaded carries on
		syncInstrParam();
		refreshDisplay();
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		for (auto& in : instruments)
			if (in.srcHead) json_array_append_new(arr, json_string(in.srcPath.c_str()));
		json_object_set_new(root, "sfzPaths", arr);
		json_object_set_new(root, "oneShot", json_boolean(oneShot));
		json_object_set_new(root, "envMode", json_integer(envMode));
		json_object_set_new(root, "kbView", json_integer(kbView));
		json_object_set_new(root, "gridLayout", json_integer(gridLayout));
		json_object_set_new(root, "gridRoot", json_integer(gridRoot));
		json_object_set_new(root, "gridScale", json_integer(gridScale));
		json_object_set_new(root, "gridBase", json_integer(gridBase));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "oneShot")) oneShot = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "envMode")) envMode = clamp((int)json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(root, "kbView")) kbView = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(root, "gridLayout")) gridLayout = clamp((int)json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(root, "gridRoot")) gridRoot = clamp((int)json_integer_value(j), 0, 11);
		if (json_t* j = json_object_get(root, "gridScale")) gridScale = clamp((int)json_integer_value(j), 0, sfs::NUM_SCALES - 1);
		if (json_t* j = json_object_get(root, "gridBase")) gridBase = clamp((int)json_integer_value(j), 0, 115);
		json_t* arr = json_object_get(root, "sfzPaths");
		if (!arr) return;
		joinLoader();
		instruments.clear();
		size_t n = json_array_size(arr);
		// hold the loader off until every instrument is in place, then load them
		// all in one pass — starting per instrument cancels the previous one
		for (size_t i = 0; i < n; i++) { json_t* p = json_array_get(arr, i); if (p) loadInstrument(json_string_value(p), false); }
		startLoader();
		syncInstrParam();
	}
};

// The knob reads back as the instrument it selects, not as an index into a list
// the user cannot see. Reports the KNOB's own choice rather than what is sounding:
// with INSTR CV patched those differ, and a knob's tooltip should describe the
// knob. Clamped, because a shrunk list can leave the stored value past the end
// for as long as it takes syncInstrParam() to pull it in.
std::string PlayInstrQuantity::getDisplayValueString() {
	Play* m = dynamic_cast<Play*>(module);
	if (!m || m->instruments.empty()) return "none loaded";
	int n = (int)m->instruments.size();
	int i = clamp((int)std::round(getValue()), 0, n - 1);
	return string::f("%d/%d %s", i + 1, n, m->instruments[i].name.c_str());
}

// Right-clicking a knob in Rack opens a text field over that same string, so the
// number it accepts has to be the number it prints: 1-based, not the 0-based
// index the base class would parse back out of "3/6 Kalimba".
void PlayInstrQuantity::setDisplayValueString(std::string s) {
	int v = atoi(s.c_str());
	if (v >= 1) setImmediateValue((float)(v - 1));
}

// ─── Display ─────────────────────────────────────────────────────────────────
// Tabs, left→right: INSTRUMENTS (view 0) then GRID (view 1). The list leads
// because it is the first thing you need — there is nothing to play until
// something is loaded — and it is where a new module opens.
static const int PLAY_TAB_VIEW[2] = { 0, 1 };
static const char* PLAY_TAB_LABEL[2] = { "INSTRUMENTS", "GRID" };
void PlayDisplay::drawTabs(NVGcontext* vg, int view) {
	Rect r[2]; tabRects(r);
	for (int i = 0; i < 2; i++) {
		bool active = (view == PLAY_TAB_VIEW[i]);
		nvgBeginPath(vg); nvgRoundedRect(vg, r[i].pos.x, r[i].pos.y, r[i].size.x, r[i].size.y, 2.f);
		nvgFillColor(vg, active ? nvgRGB(0x0d, 0x59, 0x86) : nvgRGB(0x2a, 0x2a, 0x3e)); nvgFill(vg);
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, active ? nvgRGB(0xe6, 0xe6, 0xf0) : nvgRGB(0x6a, 0x6a, 0x88));
			nvgText(vg, r[i].pos.x + r[i].size.x * 0.5f, r[i].pos.y + r[i].size.y * 0.5f + 0.5f, PLAY_TAB_LABEL[i], NULL);
		}
	}
}

void PlayDisplay::drawGrid(NVGcontext* vg, const uint8_t* mapped, const uint8_t* root, const uint8_t* playing,
                           int layout, int gRoot, int gScale, int gBase) {
	float x0, y0, pad; gridGeom(x0, y0, pad);
	float ps = pad * 0.88f, off = (pad - ps) * 0.5f;
	for (int rr = 0; rr < GRID_ROWS; rr++) {           // rr = display row (0 = top)
		int row = GRID_ROWS - 1 - rr;                  // musical row (0 = bottom)
		for (int col = 0; col < GRID_COLS; col++) {
			int n = gridNoteAt(layout, gRoot, gScale, gBase, row, col);
			float px = x0 + col * pad + off, py = y0 + rr * pad + off;
			bool inScale = (n >= 0) && gridNoteInScale(n, gRoot, gScale);
			bool isKeyRoot = (n >= 0) && (((n - gRoot) % 12 + 12) % 12 == 0);
			NVGcolor c;
			if (n < 0)              c = nvgRGB(0x14, 0x14, 0x22);
			else if (playing[n])    c = nvgRGB(0x00, 0xc8, 0xff);
			else if (root[n])       c = nvgRGB(0x00, 0x97, 0xde);
			else if (mapped[n])     c = nvgRGB(0x25, 0x4c, 0x66);
			else if (layout == 2)   c = nvgRGB(0x40, 0x40, 0x54);
			else                    c = inScale ? nvgRGB(0x3a, 0x3a, 0x4a) : nvgRGB(0x20, 0x20, 0x2c);
			// chromatic grid: darken accidental (black-key) columns across every tier
			// so they read as a piano-roll overlay even inside the lit/mapped region
			if (layout == 2 && n >= 0 && !playing[n] && gridIsAccidental(n))
				c = nvgRGBf(c.r * 0.55f, c.g * 0.55f, c.b * 0.55f);
			nvgBeginPath(vg); nvgRoundedRect(vg, px, py, ps, ps, 2.f);
			nvgFillColor(vg, c); nvgFill(vg);
			if (isKeyRoot) {   // key-root pads: bright ring + label
				nvgBeginPath(vg); nvgRoundedRect(vg, px + 0.5f, py + 0.5f, ps - 1.f, ps - 1.f, 2.f);
				nvgStrokeColor(vg, nvgRGBA(0xff, 0xd0, 0x60, 0xcc)); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
				if (font && ps > 10.f) {
					nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 6.5f);
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0xdd));
					nvgText(vg, px + ps * 0.5f, py + ps * 0.5f, string::f("%s%d", PLAY_NOTES[((n % 12) + 12) % 12], n / 12 - 1).c_str(), NULL);
				}
			}
		}
	}
}

void PlayDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	NVGcontext* vg = args.vg; const float w = box.size.x, h = box.size.y;
	nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
	nvgFillColor(vg, nvgRGB(0x1a, 0x1a, 0x2e)); nvgFill(vg);

	const char* name = "Piano"; const char* info = "12 regions"; const char* err = "";
	uint8_t mapped[128] = {0}, root[128] = {0}, playing[128] = {0}; int cnt = 0;
	int view = 1, layout = 0, gRoot = 0, gScale = 1, gBase = 36;
	if (module) { name = module->dispName; info = module->dispInfo; err = module->dispErr;
		std::memcpy(mapped, module->dispMapped, 128); std::memcpy(root, module->dispRoot, 128);
		std::memcpy(playing, module->dispPlaying, 128); cnt = module->dispCount;
		view = module->kbView; layout = module->gridLayout; gRoot = module->gridRoot;
		gScale = module->gridScale; gBase = module->gridBase; }
	else { for (int n = 48; n <= 72; n++) mapped[n] = 1; for (int n = 48; n <= 72; n += 3) root[n] = 1; layout = 2; }

	drawTabs(vg, view);
	if (font) {
		nvgFontFaceId(vg, font->handle);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFontSize(vg, 10.f); nvgFillColor(vg, nvgRGB(0xc8, 0xc8, 0xe0));
		nvgSave(vg); nvgIntersectScissor(vg, 6, 20, w - 6 - 100.f, 14);
		nvgText(vg, 6, 30, name, NULL);
		nvgRestore(vg);
		// The right-hand slot carries the load progress while there IS load
		// progress and the voice count otherwise. `info` used to draw only on the
		// piano tab, so on the grid — the default view — a library still decoding
		// looked exactly like one that had finished and gone silent.
		bool loading = module && module->loaderRun.load();
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
		nvgFillColor(vg, loading ? nvgRGB(0xec, 0x65, 0x2e) : nvgRGB(0x00, 0xc8, 0xff));
		nvgText(vg, w - 6, 30, loading ? info : string::f("%d voices", cnt).c_str(), NULL);
	}

	if (view == 1) drawGrid(vg, mapped, root, playing, layout, gRoot, gScale, gBase);
	else           drawList(vg);

	// The load error goes over whichever view is showing, in the plugin's orange
	// — a message that only appeared on one of two tabs would still be a module
	// that failed without saying so. Directly under the header: the foot belongs
	// to the LOAD button now, and covering that is covering the thing you need to
	// reach after a failed load.
	if (font && err && err[0]) {
		nvgBeginPath(vg); nvgRoundedRect(vg, 3, 33, w - 6, 15, 2.f);
		nvgFillColor(vg, nvgRGBA(0x2a, 0x14, 0x0c, 0xf0)); nvgFill(vg);
		nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFillColor(vg, nvgRGB(0xec, 0x65, 0x2e));
		nvgText(vg, 8, 44, err, NULL);
	}
}

// One row per loaded instrument, and a LOAD button you cannot miss. Reads
// `module->instruments` directly rather than through the dispXxx snapshot:
// those exist because refreshDisplay() runs on the AUDIO thread, whereas this
// and every load run on the GUI thread, so they cannot race each other. The
// loader thread only fills regions that already exist — it never resizes the
// vectors, and every structural change joins it first.
void PlayDisplay::drawList(NVGcontext* vg) {
	const float w = box.size.x;
	const Rect lb = loadRect();
	const int n = module ? (int)module->instruments.size() : 0;
	const float top = LIST_TOP;
	const int vis = rowsVisible();

	auto drawLoad = [&]() {
		nvgBeginPath(vg); nvgRoundedRect(vg, lb.pos.x, lb.pos.y, lb.size.x, lb.size.y, 3.f);
		nvgFillColor(vg, hoverLoad ? nvgRGB(0x12, 0x74, 0xad) : nvgRGB(0x0d, 0x59, 0x86)); nvgFill(vg);
		nvgStrokeColor(vg, nvgRGB(0x00, 0x97, 0xde)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		if (!font) return;
		nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0xe6, 0xe6, 0xf0));
		nvgText(vg, lb.pos.x + lb.size.x * 0.5f, lb.pos.y + lb.size.y * 0.5f + 0.5f, "+  LOAD INSTRUMENT", NULL);
	};

	if (n == 0) {
		if (font) {
			// Centred in the list area rather than pinned under the header — an
			// empty pane with its message in the top corner reads as broken.
			float mid = (top + lb.pos.y) * 0.5f;
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, nvgRGB(0x6a, 0x6a, 0x88));
			nvgText(vg, w * 0.5f, mid - 8.f, "no instruments loaded", NULL);
			nvgFontSize(vg, 8.f);
			nvgFillColor(vg, nvgRGB(0x4a, 0x4a, 0x66));
			nvgText(vg, w * 0.5f, mid + 8.f, ".sfz  .dspreset  .dsbundle  .dslibrary", NULL);
		}
		drawLoad();
		return;
	}

	int first = clamp(listScroll, 0, std::max(0, n - vis));
	for (int v = 0; v < vis && first + v < n; v++) {
		int i = first + v;
		const Instrument& in = module->instruments[i];
		float y = top + v * ROW_H;
		bool sel = (i == module->curInstrument);
		bool hov = (i == hoverRow);
		if (sel || hov) {
			nvgBeginPath(vg); nvgRoundedRect(vg, 3, y, w - 6, ROW_H - 2.f, 2.f);
			nvgFillColor(vg, sel ? nvgRGB(0x0d, 0x59, 0x86) : nvgRGB(0x2a, 0x2a, 0x3e)); nvgFill(vg);
		}
		if (!font) continue;
		nvgFontFaceId(vg, font->handle);
		float mid = y + (ROW_H - 2.f) * 0.5f + 0.5f;

		nvgFontSize(vg, 8.f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sel ? nvgRGB(0xa0, 0xd8, 0xf4) : nvgRGB(0x5a, 0x5a, 0x78));
		nvgText(vg, 8, mid, string::f("%d", i + 1).c_str(), NULL);

		// How far along this instrument is, per instrument — the header only ever
		// spoke for the selected one, so a library still decoding in the
		// background looked identical to one that had failed.
		int tot = (int)in.regions.size(), loaded = 0;
		for (const auto& r : in.regions) if (r.loaded) loaded++;
		std::string status = (loaded == tot) ? string::f("%d", tot)
		                   : string::f("%d/%d", loaded, tot);
		nvgFontSize(vg, 8.f);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		// Every colour on a row has to clear the row's OWN background: the dim
		// grey that reads as secondary against #1a1a2e disappears into the
		// selected row's blue, so the selected row gets its own pair.
		nvgFillColor(vg, (loaded != tot) ? nvgRGB(0xec, 0x65, 0x2e)
		                 : sel           ? nvgRGB(0xa0, 0xd8, 0xf4)
		                                 : nvgRGB(0x5a, 0x5a, 0x78));
		nvgText(vg, w - (hov ? 26.f : 10.f), mid, status.c_str(), NULL);
		// 8px/char at this size covers "431/431"; the name column stops short of it.

		// Name, clipped to its column rather than measured and truncated — the
		// column is the truth about how much room there is.
		nvgSave(vg);
		nvgIntersectScissor(vg, 22, y, w - 22 - (hov ? 66.f : 52.f), ROW_H);
		nvgFontSize(vg, 9.5f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sel ? nvgRGB(0xe6, 0xe6, 0xf0) : nvgRGB(0xa8, 0xa8, 0xc4));
		nvgText(vg, 22, mid, in.name.c_str(), NULL);
		nvgRestore(vg);

		if (hov) {
			Rect rr = removeRect(v);
			nvgBeginPath(vg); nvgRoundedRect(vg, rr.pos.x, rr.pos.y, rr.size.x, rr.size.y, 2.f);
			nvgFillColor(vg, nvgRGB(0x3a, 0x22, 0x22)); nvgFill(vg);
			float cx = rr.pos.x + rr.size.x * 0.5f, cy = rr.pos.y + rr.size.y * 0.5f, a = 2.8f;
			nvgBeginPath(vg);
			nvgMoveTo(vg, cx - a, cy - a); nvgLineTo(vg, cx + a, cy + a);
			nvgMoveTo(vg, cx + a, cy - a); nvgLineTo(vg, cx - a, cy + a);
			nvgStrokeColor(vg, nvgRGB(0xec, 0x65, 0x2e)); nvgStrokeWidth(vg, 1.3f);
			nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
		}
	}

	// Scroll marker — only when there is something off the ends.
	if (n > vis) {
		float trackH = vis * ROW_H;
		float kh = std::max(10.f, trackH * vis / n);
		float ky = top + (trackH - kh) * (float)first / (float)(n - vis);
		nvgBeginPath(vg); nvgRoundedRect(vg, w - 4.5f, ky, 2.f, kh, 1.f);
		nvgFillColor(vg, nvgRGB(0x4a, 0x4a, 0x66)); nvgFill(vg);
	}
	drawLoad();
}

int PlayDisplay::hitPad(Vec p) const {
	if (!module) return -1;
	float x0, y0, pad; gridGeom(x0, y0, pad);
	if (p.x < x0 || p.y < y0) return -1;
	int col = (int)((p.x - x0) / pad), rr = (int)((p.y - y0) / pad);
	if (col < 0 || col >= GRID_COLS || rr < 0 || rr >= GRID_ROWS) return -1;
	return gridNoteAt(module->gridLayout, module->gridRoot, module->gridScale, module->gridBase, GRID_ROWS - 1 - rr, col);
}

int PlayDisplay::rowAt(Vec p) const {
	if (!module || module->instruments.empty()) return -1;
	float top = LIST_TOP;
	if (p.y < top || p.x < 3 || p.x > box.size.x - 3) return -1;
	int v = (int)((p.y - top) / ROW_H);
	if (v < 0 || v >= rowsVisible()) return -1;
	int n = (int)module->instruments.size();
	int i = clamp(listScroll, 0, std::max(0, n - rowsVisible())) + v;
	return (i < n) ? i : -1;
}

// A file dialog opened from inside an event handler eats the mouse-release that
// belongs to the click that opened it, and Rack is left thinking the button is
// still down. Record already defers its folder prompt to the widget's step() for
// the same reason; this follows it.
void PlayDisplay::step() {
	OpaqueWidget::step();
	if (!module) return;
	if (removeReq >= 0) { int i = removeReq; removeReq = -1; module->removeInstrument(i); }
	// Follow the selection: INSTR is a knob and a CV as well as a list, so the
	// row that is playing has to come to the cursor rather than wait for it.
	int n = (int)module->instruments.size(), vis = rowsVisible();
	listScroll = clamp(listScroll, 0, std::max(0, n - vis));
	if (module->curInstrument < listScroll) listScroll = module->curInstrument;
	else if (module->curInstrument >= listScroll + vis) listScroll = module->curInstrument - vis + 1;
	if (loadReq) {
		loadReq = false;
		char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL,
			osdialog_filters_parse("Instrument:sfz,dspreset,dsbundle,dslibrary"));
		if (path) { module->loadInstrument(path); std::free(path); }
	}
}

void PlayDisplay::onHover(const HoverEvent& e) {
	OpaqueWidget::onHover(e);
	if (!module || module->kbView != 0) { hoverRow = -1; hoverLoad = false; return; }
	hoverLoad = loadRect().contains(e.pos);
	hoverRow  = rowAt(e.pos);
}
void PlayDisplay::onLeave(const LeaveEvent& e) {
	hoverRow = -1; hoverLoad = false;
	OpaqueWidget::onLeave(e);
}
void PlayDisplay::onHoverScroll(const HoverScrollEvent& e) {
	if (!module || module->kbView != 0) { OpaqueWidget::onHoverScroll(e); return; }
	int n = (int)module->instruments.size(), vis = rowsVisible();
	if (n <= vis) { OpaqueWidget::onHoverScroll(e); return; }
	listScroll = clamp(listScroll - (int)std::round(e.scrollDelta.y / 20.f), 0, n - vis);
	e.consume(this);
}

void PlayDisplay::onButton(const ButtonEvent& e) {
	if (!module) { OpaqueWidget::onButton(e); return; }
	if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
		Vec p = e.pos;
		int t = hitTab(p);
		if (t >= 0) { module->kbView = PLAY_TAB_VIEW[t]; e.consume(this); return; }
		if (module->kbView == 0) {
			if (loadRect().contains(p)) { loadReq = true; e.consume(this); return; }
			int i = rowAt(p);
			if (i >= 0) {
				int vis = rowsVisible(), n = (int)module->instruments.size();
				int v = i - clamp(listScroll, 0, std::max(0, n - vis));
				// The cross is only drawn on the hovered row, so it is only a
				// target on the hovered row.
				if (i == hoverRow && removeRect(v).contains(p)) { removeReq = i; e.consume(this); return; }
				module->params[Play::INSTR_PARAM].setValue((float)i);
				e.consume(this); return;
			}
			OpaqueWidget::onButton(e); return;
		}
		int n = hitPad(p);
		if (n >= 0) { module->uiNote = n; dragging = true; dragPos = p; e.consume(this); return; }
	}
	if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_RELEASE && dragging) {
		module->uiNote = -1; dragging = false; e.consume(this); return;
	}
	OpaqueWidget::onButton(e);
}
void PlayDisplay::onDragMove(const DragMoveEvent& e) {
	if (!module || !dragging) { OpaqueWidget::onDragMove(e); return; }
	float zoom = getAbsoluteZoom(); if (zoom <= 0.f) zoom = 1.f;
	dragPos = dragPos.plus(e.mouseDelta.div(zoom));
	int n = hitPad(dragPos);
	if (n >= 0) module->uiNote = n;
}
void PlayDisplay::onDragEnd(const DragEndEvent& e) {
	if (module && dragging) { module->uiNote = -1; dragging = false; }
	OpaqueWidget::onDragEnd(e);
}

// ─── Widget ──────────────────────────────────────────────────────────────────
struct PlayWidget : ModuleWidget {
	PlayWidget(Play* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/play.svg")));
		// No virtual screws — see CLAUDE.md.

		PlayDisplay* disp = new PlayDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(4.f, 12.f));
		disp->box.size = mm2px(Vec(73.f, 57.f));
		addChild(disp);

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 81.18f)), module, Play::INSTR_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 101.50f)), module, Play::LEVEL_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, 81.18f)), module, Play::INSTR_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, 101.50f)), module, Play::LEVEL_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 121.82f)), module, Play::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, 121.82f)), module, Play::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.64f, 121.82f)), module, Play::VEL_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(55.88f, 122.13f)), module, Play::L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(71.12f, 121.82f)), module, Play::R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Play* m = dynamic_cast<Play*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Load instrument (.sfz / .dspreset / .dsbundle / .dslibrary)…", "", [m]() {
			char* p = osdialog_file(OSDIALOG_OPEN, NULL, NULL, osdialog_filters_parse("Instrument:sfz,dspreset,dsbundle,dslibrary"));
			if (p) { m->loadInstrument(p); std::free(p); }
		}));
		// Fallback for when .dsbundle is a plain folder (not a registered package):
		// pick the bundle folder directly.
		menu->addChild(createMenuItem("Load DecentSampler bundle (folder)…", "", [m]() {
			char* p = osdialog_file(OSDIALOG_OPEN_DIR, NULL, NULL, NULL);
			if (p) { m->loadInstrument(p); std::free(p); }
		}));
		for (int i = 0; i < (int)m->instruments.size(); i++) {
			std::string label = string::f("%d: %s", i + 1, m->instruments[i].name.c_str());
			menu->addChild(createCheckMenuItem(label, "",
				[m, i]() { return m->curInstrument == i; },
				[m, i]() { m->params[Play::INSTR_PARAM].setValue((float)i); }));
		}
		if (!m->instruments.empty())
			menu->addChild(createMenuItem("Remove current instrument", "", [m]() { m->removeInstrument(m->curInstrument); }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("One-shot (play through, ignore gate-off)", "", &m->oneShot));
		menu->addChild(createIndexPtrSubmenuItem("Amp envelope",
			{"Off (fast release)", "Use envelopes (SFZ)", "Default ADSR (smooth)"},
			&m->envMode));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Grid view"));
		menu->addChild(createIndexSubmenuItem("Layout", {"Chromatic (4ths)", "In-Key (scale)", "Chromatic grid (C0)"},
			[m]() { return m->gridLayout; }, [m](int i) { m->gridLayout = i; }));
		static const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
		std::vector<std::string> roots(NOTE_NAMES, NOTE_NAMES + 12);
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

Model* modelPlay = createModel<Play, PlayWidget>("Play");
