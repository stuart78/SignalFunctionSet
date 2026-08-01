#include "plugin.hpp"
#include "pulse-width.hpp"
#include "drum-taste.hpp"
#include <osdialog.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// ─── FILL — auto-playing 8-channel drum sequencer ────────────────────────────
// Loads the SFS drum-pattern library (res/patterns/drum-patterns-v1.json): 8 role
// lanes, patterns organised into sets (sparse/main/lift tiers + a fill list). A
// single internal PRESSURE value builds each bar and vents as a fill: pressure picks
// the tier (groove thickens) and, on discharge, plays a fill — a build-and-release
// arc, not random noise. Playback is PHASE-BASED (position within the bar), so a
// pattern can be any resolution (4th–64th), a triplet grid (12/8), or a 2-bar clave
// from one BAR-aligned clock. Per-channel GATE/VEL/ACCENT out + a FILL gate.
//
// v0.2: library loader + phase playback + tier select + generated/library fills +
// deferred (bar-aligned) set switching. Ratchet (`r`) and swing are parsed but not
// yet played; the on-screen browser is the next step.

static const int FILL_NCH       = 8;    // role lanes: kick snare chh ohh lo hi cp bell
static const int FILL_MAX_STEPS = 256;  // beats·stepsPerBeat·bars ceiling: 64ths over 2 bars,
                                        // or 64th-triplets over one (4·24·1 = 96)

// ── Library data model ───────────────────────────────────────────────────────
struct LibPattern {
	std::string id, name, meter;
	int meterNum = 4, meterDen = 4;            // parsed from the meter string ("12/8" → 12, 8)
	int beats = 4, spb = 4, bars = 1, steps = 16;
	float swing = 0.f;
	float vel[FILL_NCH][FILL_MAX_STEPS] = {};
	bool  acc[FILL_NCH][FILL_MAX_STEPS] = {};
	float prob[FILL_NCH][FILL_MAX_STEPS];      // 1 = always (default)
	int   rat[FILL_NCH][FILL_MAX_STEPS];       // 1 = single hit (default) — parsed, not yet played
	LibPattern() { for (int c = 0; c < FILL_NCH; c++) for (int s = 0; s < FILL_MAX_STEPS; s++) { prob[c][s] = 1.f; rat[c][s] = 1; } }
};
struct LibSet {
	std::string id, name, family;
	int sparse = -1, main = -1, lift = -1;     // pattern indices; sparse/lift fall back to main
	std::vector<int> fills;
	float bpm = 120.f;
	float vary = 0.4f;                         // identity strength: how far the variation layer may bend this set
	sfs::Taste taste;                          // how this style ornaments (see drum-taste.hpp)
	int   axis = 0;                            // browser axis (AX_GENRE / AX_REGION / AX_USER)
};

// Browser axes. GENRE/REGION come from the set's "axis" field; USER is forced on
// anything loaded from the user patterns folder; FAV is virtual, rebuilt from the
// persisted favorites id set.
static const int AX_GENRE = 0, AX_REGION = 1, AX_USER = 2, AX_FAV = 3, FILL_NAXIS = 4;

struct Library {
	std::vector<std::string> lanes;
	std::vector<LibPattern> pats;
	std::vector<LibSet> sets;
	std::map<std::string, int> patById;
	// banks = families, built after load (first-seen order), split by browser axis
	std::vector<std::string> families[FILL_NAXIS];
	std::vector<std::vector<int>> famSets[FILL_NAXIS];   // set indices per family, per axis
};

static inline int jgeti(json_t* o, const char* k, int d) {
	json_t* v = json_object_get(o, k); return (v && json_is_number(v)) ? (int)json_number_value(v) : d;
}
static inline double jgetf(json_t* o, const char* k, double d) {
	json_t* v = json_object_get(o, k); return (v && json_is_number(v)) ? json_number_value(v) : d;
}
static inline std::string jgets(json_t* o, const char* k) {
	json_t* v = json_object_get(o, k); return (v && json_is_string(v)) ? json_string_value(v) : std::string();
}

static void parseRow(json_t* laneObj, int c, int steps, LibPattern& p) {
	auto row = [&](const char* key) -> const char* {
		json_t* v = json_object_get(laneObj, key); return (v && json_is_string(v)) ? json_string_value(v) : nullptr;
	};
	const char* v = row("v"); const char* a = row("a"); const char* pr = row("p"); const char* r = row("r");
	for (int s = 0; s < steps && s < FILL_MAX_STEPS; s++) {
		if (v && v[s]) { char ch = v[s]; p.vel[c][s] = (ch >= '1' && ch <= '9') ? (float)(ch - '0') / 9.f : 0.f; }
		if (a && a[s]) p.acc[c][s] = (a[s] == 'A');
		if (pr && pr[s]) { char ch = pr[s]; p.prob[c][s] = (ch >= '1' && ch <= '9') ? (float)(ch - '0') / 9.f : 1.f; }
		if (r && r[s]) { char ch = r[s]; p.rat[c][s] = (ch >= '2' && ch <= '8') ? (ch - '0') : 1; }
	}
}

static void loadLibrary(const std::string& path, Library& lib, int forceAxis = -1) {
	json_error_t err;
	json_t* root = json_load_file(path.c_str(), 0, &err);
	if (!root) { WARN("FILL: could not load pattern library %s: %s", path.c_str(), err.text); return; }

	if (json_t* lanes = json_object_get(root, "lanes")) {
		size_t n = json_array_size(lanes);
		for (size_t i = 0; i < n; i++) lib.lanes.push_back(json_string_value(json_array_get(lanes, i)));
	}
	auto laneIdx = [&](const std::string& name) -> int {
		for (int i = 0; i < (int)lib.lanes.size() && i < FILL_NCH; i++) if (lib.lanes[i] == name) return i;
		return -1;
	};

	if (json_t* pats = json_object_get(root, "patterns")) {
		size_t n = json_array_size(pats);
		for (size_t i = 0; i < n; i++) {
			json_t* pj = json_array_get(pats, i);
			LibPattern lp;
			lp.id = jgets(pj, "id"); lp.name = jgets(pj, "name");
			lp.meter = jgets(pj, "meter"); if (lp.meter.empty()) lp.meter = "4/4";
			{ int n = 4, d = 4; if (std::sscanf(lp.meter.c_str(), "%d/%d", &n, &d) == 2) { lp.meterNum = clamp(n, 1, 16); lp.meterDen = clamp(d, 1, 32); } }
			lp.beats = jgeti(pj, "beats", 4); lp.spb = jgeti(pj, "stepsPerBeat", 4); lp.bars = jgeti(pj, "bars", 1);
			lp.swing = (float)jgetf(pj, "swing", 0.0);
			lp.steps = clamp(lp.beats * lp.spb * lp.bars, 1, FILL_MAX_STEPS);
			if (json_t* grid = json_object_get(pj, "grid")) {
				const char* key; json_t* laneObj;
				json_object_foreach(grid, key, laneObj) {
					int c = laneIdx(key);
					if (c >= 0) parseRow(laneObj, c, lp.steps, lp);
				}
			}
			if (lib.patById.count(lp.id)) continue;   // duplicate id (file loaded twice) — first wins
			lib.patById[lp.id] = (int)lib.pats.size();
			lib.pats.push_back(std::move(lp));
		}
	}
	auto idOf = [&](const std::string& id) -> int {
		auto it = lib.patById.find(id); return it == lib.patById.end() ? -1 : it->second;
	};
	if (json_t* sets = json_object_get(root, "sets")) {
		size_t n = json_array_size(sets);
		for (size_t i = 0; i < n; i++) {
			json_t* sj = json_array_get(sets, i);
			LibSet s;
			s.id = jgets(sj, "id"); s.name = jgets(sj, "name"); s.family = jgets(sj, "family");
			s.bpm = (float)jgetf(sj, "bpm", 120.0);
			s.vary = clamp((float)jgetf(sj, "vary", 0.4), 0.f, 1.f);
			s.axis = (forceAxis >= 0) ? forceAxis : ((jgets(sj, "axis") == "region") ? AX_REGION : AX_GENRE);
			if (json_t* roles = json_object_get(sj, "roles")) {
				s.main = idOf(jgets(roles, "main"));
				std::string sp = jgets(roles, "sparse"); s.sparse = sp.empty() ? s.main : idOf(sp);
				std::string lf = jgets(roles, "lift");   s.lift   = lf.empty() ? s.main : idOf(lf);
				if (s.sparse < 0) s.sparse = s.main;
				if (s.lift < 0) s.lift = s.main;
				if (json_t* fj = json_object_get(roles, "fill")) {
					if (json_is_array(fj)) {
						size_t fn = json_array_size(fj);
						for (size_t k = 0; k < fn; k++) { int idx = idOf(json_string_value(json_array_get(fj, k))); if (idx >= 0) s.fills.push_back(idx); }
					} else if (json_is_string(fj)) { int idx = idOf(json_string_value(fj)); if (idx >= 0) s.fills.push_back(idx); }
				}
			}
			{
				std::string sub;
				if (s.id.compare(0, s.family.size(), s.family) == 0
				    && s.id.size() > s.family.size() + 1 && s.id[s.family.size()] == '.') {
					sub = s.id.substr(s.family.size() + 1);
					size_t dot = sub.find('.');
					if (dot != std::string::npos) sub = sub.substr(0, dot);
				}
				s.taste = sfs::tasteFor(s.family, sub);
				if (!json_object_get(sj, "vary")) s.vary = s.taste.vary;   // research default
			}
			bool dupId = false;                       // duplicate set ids break favorites (matched by id)
			for (const LibSet& ex : lib.sets) if (ex.id == s.id) { dupId = true; break; }
			if (s.main >= 0 && !dupId) lib.sets.push_back(std::move(s));
		}
	}
	json_decref(root);
	INFO("FILL: loaded %s → %d patterns, %d sets total", path.c_str(), (int)lib.pats.size(), (int)lib.sets.size());
}

// Group sets into banks (families) per browser axis, first-seen order. Called once
// after ALL files load.
static void famInsert(Library& lib, int a, int i) {
	std::string fam = lib.sets[i].family.empty() ? std::string("misc") : lib.sets[i].family;
	int f = -1;
	for (int k = 0; k < (int)lib.families[a].size(); k++) if (lib.families[a][k] == fam) { f = k; break; }
	if (f < 0) { f = (int)lib.families[a].size(); lib.families[a].push_back(fam); lib.famSets[a].push_back(std::vector<int>()); }
	lib.famSets[a][f].push_back(i);
}

static void buildFamilies(Library& lib) {
	for (int a = 0; a < AX_FAV; a++) { lib.families[a].clear(); lib.famSets[a].clear(); }
	for (int i = 0; i < (int)lib.sets.size(); i++)
		famInsert(lib, clamp(lib.sets[i].axis, 0, AX_USER), i);
}

// Favorites: virtual axis, rebuilt whenever the persisted id set changes.
static void buildFavAxis(Library& lib, const std::set<std::string>& favs) {
	lib.families[AX_FAV].clear(); lib.famSets[AX_FAV].clear();
	for (int i = 0; i < (int)lib.sets.size(); i++)
		if (favs.count(lib.sets[i].id)) famInsert(lib, AX_FAV, i);
}

// ── drum-patterns.com import ─────────────────────────────────────────────────
// Their exported .txt: a "Tempo: / Time: / Swing:" header, then one or more
// "[n] Name" sections of lane rows ("CH --X--X--…"). AC and GH are global
// accent / ghost modifier rows; Swing is in site levels (1 level ≈ 6%).

static int dpLaneIdx(const std::string& v) {
	// site voice code → FILL role lane (canonical kick snare chh ohh lo hi cp bell)
	static const struct { const char* code; int lane; } M[] = {
		{"BD", 0}, {"SD", 1}, {"CH", 2}, {"OH", 3},
		{"LT", 4}, {"MT", 4}, {"LC", 4}, {"MC", 4},
		{"HT", 5}, {"HC", 5},
		{"CP", 6}, {"CL", 6}, {"RS", 6}, {"MA", 6},
		{"CY", 7}, {"CB", 7},
	};
	for (const auto& m : M) if (v == m.code) return m.lane;
	return -1;
}

static std::string dpSlug(const std::string& s) {
	std::string out;
	for (char c : s) {
		if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
		else if (!out.empty() && out.back() != '-') out += '-';
	}
	while (!out.empty() && out.back() == '-') out.pop_back();
	return out.empty() ? std::string("import") : out;
}

static bool loadDrumPatternsTxt(const std::string& path, Library& lib) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::string text; char buf[4096]; size_t r;
	while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
	std::fclose(f);

	float bpm = 120.f, swing = 0.f;
	int tsN = 4, tsD = 4;
	struct Sec { std::string name; std::string rows[FILL_NCH]; std::string acc, gho; };
	std::vector<Sec> secs;

	size_t p = 0;
	while (p < text.size()) {
		size_t nl = text.find('\n', p);
		std::string line = text.substr(p, (nl == std::string::npos ? text.size() : nl) - p);
		p = (nl == std::string::npos) ? text.size() : nl + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if (line.empty()) continue;
		if (line.compare(0, 6, "Tempo:") == 0) { bpm = clamp((float)atof(line.c_str() + 6), 20.f, 400.f); continue; }
		if (line.compare(0, 5, "Time:") == 0) { int n, d; if (std::sscanf(line.c_str() + 5, "%d/%d", &n, &d) == 2) { tsN = clamp(n, 1, 16); tsD = clamp(d, 1, 32); } continue; }
		if (line.compare(0, 6, "Swing:") == 0) { swing = clamp((float)atof(line.c_str() + 6) * 0.06f, 0.f, 1.f); continue; }
		if (line[0] == '[') {                          // "[2] Name" → new section
			Sec s; size_t br = line.find(']');
			if (br != std::string::npos) { s.name = line.substr(br + 1); while (!s.name.empty() && s.name[0] == ' ') s.name.erase(0, 1); }
			secs.push_back(s); continue;
		}
		if (line.size() >= 3 && std::isupper((unsigned char)line[0]) && std::isupper((unsigned char)line[1])) {
			std::string code = line.substr(0, 2), row;
			for (size_t i = 2; i < line.size(); i++) if (line[i] != ' ' && line[i] != '\t') row += line[i];
			if (row.empty()) continue;
			if (secs.empty()) secs.push_back(Sec());
			Sec& s = secs.back();
			if (code == "AC") { s.acc = row; continue; }
			if (code == "GH") { s.gho = row; continue; }
			int c = dpLaneIdx(code);
			if (c < 0) continue;                       // FX etc. → ignored
			std::string& dst = s.rows[c];
			if (dst.size() < row.size()) dst.resize(row.size(), '-');
			for (size_t i = 0; i < row.size(); i++) if (row[i] != '-') dst[i] = row[i];   // merge folded lanes
		}
	}

	std::string stem = system::getStem(path);
	std::string slug = dpSlug(stem);
	std::string setId = "dp." + slug;
	for (const LibSet& ex : lib.sets) if (ex.id == setId) return false;   // already loaded

	std::vector<int> patIdx;
	for (int si = 0; si < (int)secs.size(); si++) {
		Sec& s = secs[si];
		int steps = 0;
		for (int c = 0; c < FILL_NCH; c++) steps = std::max(steps, (int)s.rows[c].size());
		if (steps <= 0) continue;
		steps = std::min(steps, FILL_MAX_STEPS);
		LibPattern lp;
		lp.id = string::f("dp.%s.%d", slug.c_str(), si + 1);
		lp.name = s.name.empty() ? stem : s.name;
		lp.meterNum = tsN; lp.meterDen = tsD; lp.meter = string::f("%d/%d", tsN, tsD);
		lp.beats = tsN;
		if (steps % (tsN * 4) == 0) { lp.spb = 4; lp.bars = steps / (tsN * 4); }
		else { lp.spb = std::max(1, steps / tsN); lp.bars = 1; }
		lp.steps = steps;
		lp.swing = swing;
		for (int c = 0; c < FILL_NCH; c++)
			for (int st = 0; st < (int)s.rows[c].size() && st < steps; st++) {
				if (s.rows[c][st] == '-' || s.rows[c][st] == '.') continue;
				bool ac = st < (int)s.acc.size() && s.acc[st] != '-';
				bool gh = st < (int)s.gho.size() && s.gho[st] != '-';
				float v = ac ? 1.f : gh ? 0.33f : 0.78f;     // accent 9/9 · ghost 3/9 · plain 7/9
				lp.vel[c][st] = std::max(lp.vel[c][st], v);
				if (ac) lp.acc[c][st] = true;
			}
		lib.patById[lp.id] = (int)lib.pats.size();
		patIdx.push_back((int)lib.pats.size());
		lib.pats.push_back(lp);
	}
	if (patIdx.empty()) return false;

	LibSet set;
	set.id = setId;
	set.name = secs[0].name.empty() ? stem : secs[0].name;
	set.family = "drum-patterns";
	set.axis = AX_USER;
	set.bpm = bpm; set.vary = 0.35f;
	set.sparse = set.lift = set.main = patIdx[0];
	if (patIdx.size() > 1) set.lift = patIdx[1];       // [2] → lift, [3+] → fills
	for (size_t i = 2; i < patIdx.size(); i++) set.fills.push_back(patIdx[i]);
	lib.sets.push_back(set);
	INFO("FILL: imported %s → %d section(s)", path.c_str(), (int)patIdx.size());
	return true;
}

// Reduce a drum-patterns.com pattern PAGE back to the export .txt format — the
// grid is plain text inside <code class="pattern"> spans, and related-pattern
// sidebars are excluded by requiring a preceding link back to this page's slug.
static std::string dpHtmlToTxt(const std::string& html, const std::string& slug) {
	std::string out, name;
	size_t tp = html.find("og:title\" content=\"");
	if (tp != std::string::npos) {
		tp += 19; size_t te = html.find('"', tp);
		if (te != std::string::npos) name = html.substr(tp, te - tp);
		size_t suf = name.find(" Drum Pattern");
		if (suf != std::string::npos) name = name.substr(0, suf);
	}
	size_t mp = html.find("BPM<");                     // meta line: ">135BPM</a>&nbsp; 6% swung (1)"
	if (mp != std::string::npos) {
		size_t d = mp; while (d > 0 && std::isdigit((unsigned char)html[d - 1])) d--;
		if (d < mp) out += "Tempo: " + html.substr(d, mp - d) + "\n";
	}
	size_t sp = html.find("% swung");
	if (sp != std::string::npos) {
		size_t d = sp; while (d > 0 && std::isdigit((unsigned char)html[d - 1])) d--;
		if (d < sp) out += string::f("Swing: %.2f\n", (float)atof(html.substr(d, sp - d).c_str()) / 6.f);
	}
	out += "Time: 4/4\n";                              // not exposed on the page; grid length disambiguates
	int nsec = 0;
	size_t pos = 0;
	while ((pos = html.find("<code class=\"pattern", pos)) != std::string::npos) {
		size_t end = html.find("</code>", pos);
		if (end == std::string::npos) break;
		size_t back = pos > 800 ? pos - 800 : 0;
		if (html.substr(back, pos - back).find("/" + slug + "/") == std::string::npos) { pos = end; continue; }
		out += string::f("\n[%d] %s\n", ++nsec, name.c_str());
		std::string t; bool inTag = false;
		for (size_t i = pos; i < end; i++) {
			if (html.compare(i, 6, "</div>") == 0) { t += '\n'; i += 5; inTag = false; continue; }
			char ch = html[i];
			if (ch == '<') inTag = true;
			else if (ch == '>') inTag = false;
			else if (!inTag && ch != '\n' && ch != '\r') t += ch;
		}
		size_t e2;                                     // scrub stray entities
		while ((e2 = t.find("&nbsp;")) != std::string::npos) t.replace(e2, 6, " ");
		out += t + "\n";
		pos = end;
	}
	return nsec ? out : std::string();
}

// Load the full library: bundled res/patterns first (canonical file leads so its
// first set stays the default), then the user folder — JSON banks and imported
// drum-patterns.com .txt — onto the USER tab.
static void loadAllLibraries(Library& lib) {
	std::string dir = asset::plugin(pluginInstance, "res/patterns");
	std::string canon = system::join(dir, "drum-patterns-v2.json");
	loadLibrary(canon, lib);
	std::vector<std::string> entries = system::getEntries(dir);
	std::sort(entries.begin(), entries.end());
	for (const std::string& e : entries) {
		if (system::getExtension(e) != ".json") continue;
		if (e == canon) continue;
		loadLibrary(e, lib);
	}
	std::string udir = asset::user("SignalFunctionSet/patterns");
	system::createDirectories(udir);
	std::vector<std::string> uentries = system::getEntries(udir);
	std::sort(uentries.begin(), uentries.end());
	for (const std::string& e : uentries) {
		std::string ext = string::lowercase(system::getExtension(e));
		if (ext == ".json") loadLibrary(e, lib, AX_USER);
		else if (ext == ".txt") loadDrumPatternsTxt(e, lib);
	}
	buildFamilies(lib);
}

// Bank display name: proper names/acronyms special-cased, else title case.
static std::string famDisplay(const std::string& fam) {
	if (fam == "usa")    return "United States";
	if (fam == "mena")   return "MENA";
	if (fam == "rnb")    return "R&B";
	if (fam == "hiphop") return "Hip-Hop";
	if (fam == "indian") return "India";
	if (fam == "drum-patterns") return "drum-patterns.com";
	std::string nm = fam;
	for (size_t p = 0; p < nm.size(); p++)
		if (p == 0 || nm[p - 1] == ' ') nm[p] = (char)std::toupper((unsigned char)nm[p]);
	return nm;
}

// Small deterministic hash for the seeded variation / fill layers.
static inline uint32_t fhash(uint32_t a, uint32_t b) {
	uint32_t h = a * 2654435761u ^ b * 2246822519u; h ^= h >> 15; h *= 2654435761u; h ^= h >> 13; return h;
}
static inline float fhashF(uint32_t a, uint32_t b) { return (fhash(a, b) & 0xFFFFFF) / (float)0x1000000; }
static inline uint32_t strHash(const std::string& s) { uint32_t h = 2166136261u; for (char c : s) { h ^= (uint8_t)c; h *= 16777619u; } return h; }

struct Fill : Module {
	enum ParamId { ACCUM_PARAM, DISCHARGE_PARAM, TIER_PARAM, PHRASE_PARAM, SYNC_PARAM, SET_PARAM, EXTRAS_PARAM, ENUMS(SWING_PARAM, FILL_NCH), RESET_PARAM, PARAMS_LEN };
	enum InputId { CLOCK_INPUT, BAR_INPUT, RESET_INPUT, RESEED_INPUT, ACCUM_CV_INPUT, DISCHARGE_CV_INPUT, TIER_CV_INPUT, SET_CV_INPUT, EXTRAS_CV_INPUT, NEXT_INPUT, INPUTS_LEN };
	enum OutputId { ENUMS(GATE_OUTPUT, FILL_NCH), ENUMS(VEL_OUTPUT, FILL_NCH), ENUMS(ACC_OUTPUT, FILL_NCH), FILL_OUTPUT, SET_OUTPUT, NUM_OUTPUT, DEN_OUTPUT,
		BPM_OUTPUT, OUTPUTS_LEN };
	enum LightId { SYNC_LIGHT, FILL_LIGHT, LIGHTS_LEN };

	Library lib;
	std::mutex libMutex;                     // guards lib against live rescans (imports)
	int curSet = 0, pendingSet = 0;

	// ── pattern queue ───────────────────────────────────────────────────────
	// A playlist of sets. While it holds anything it OVERRIDES the SET knob and
	// CV, because two things steering the same choice is worse than either. It
	// wraps at the end rather than stopping.
	struct QueueItem { int set; int reps; };   // plain: C++11 aggregate init
	std::vector<QueueItem> queue;
	int  queuePos = 0;              // entry currently playing
	int  queueDone = 0;             // cycles completed within it
	int  advanceMode = 0;           // 0 = by repeat count, 1 = NEXT jack only
	bool queueStep = false;         // NEXT fired: move on at the next cycle
	int  queueJumpTo = -1;          // play pressed on a row: take THAT one next cycle
	dsp::SchmittTrigger nextTrig, resetBtnTrig;

	void queueAdd(int set) { queue.push_back({set, 1}); }
	void queueAdvance() {
		if (queue.empty()) return;
		queueDone = 0;
		queuePos = (queuePos + 1) % (int)queue.size();
	}

	// Rebuild the library from disk after an import (GUI thread). The audio
	// thread try-locks and skips its process body while the swap is in flight.
	void rescanLibrary() {
		Library nl;
		loadAllLibraries(nl);
		std::string keepId = (!lib.sets.empty()) ? lib.sets[clamp(curSet, 0, (int)lib.sets.size() - 1)].id : "";
		std::lock_guard<std::mutex> lk(libMutex);
		lib = std::move(nl);
		buildFavAxis(lib, favIds);
		int idx = 0;
		for (int i = 0; i < (int)lib.sets.size(); i++) if (lib.sets[i].id == keepId) { idx = i; break; }
		curSet = pendingSet = idx;
		if (ParamQuantity* pq = getParamQuantity(SET_PARAM)) pq->maxValue = (float)std::max(0, (int)lib.sets.size() - 1);
		params[SET_PARAM].setValue((float)idx);
		resolveCycle();
	}

	// pressure engine
	float pressure = 0.f;
	float peakPressure = 0.f;                // max since the last tier latch (tier reads this)
	int   curTier = 0;                       // 0 sparse · 1 main · 2 lift
	uint32_t reseedBase = 0x1234567u;
	int   barsSinceReset = 0;
	int   barsSinceFill = 0;                 // display: bars elapsed since the last fill
	bool  fillActive = false;
	// Fills off: the pressure arc still runs and still drives the tier, so the
	// groove keeps rising and settling across the phrase — it just never breaks.
	// Venting silently at the moment a fill would have fired is what preserves
	// that; without it pressure saturates and the tier sticks at the top.
	bool  playFills = true;
	float fillFlash = 0.f;

	// clock-driven playback: steps fire ON incoming CLOCK edges, so playback freezes the
	// instant the clock stops and locks from the first pulse (no tempo estimation / warm-up).
	bool   started = false;
	int    cycleBar = 0, lastStep = -1;
	int    clockCount = 0, clocksPerBar = 16;   // clocks in the current bar; measured over the previous
	int    barSuppress = 0;                      // ignore the CLOCK coincident with a BAR (downbeat)

	// resolved working cycle
	float curVel[FILL_NCH][FILL_MAX_STEPS] = {};
	bool  curAcc[FILL_NCH][FILL_MAX_STEPS] = {};
	bool  curGen[FILL_NCH][FILL_MAX_STEPS] = {};   // true = engine-added (variation / generated fill), for the display
	float curProb[FILL_NCH][FILL_MAX_STEPS];
	int   curRat[FILL_NCH][FILL_MAX_STEPS];        // per-step retrigger count (the format's `r` row)
	float curPatSwing = 0.f;                        // the pattern's authored swing (format: 0..1, 1 = triplet feel)
	int   curSteps = 16, curBars = 1, curSpb = 4, curStepsPerBar = 16;
	int   curBeats = 4;                          // pulses per bar of the playing pattern
	int   clockRes = 0;                          // 0 = measure; else clocks per beat
	int   lastTierBias = 0;                         // tier is phrase-latched; offset changes break the latch
	int   tierMode = 0;      // 0 = rise through the phrase, 1 = latch one tier per phrase
	int   tierBalance = 1;   // 0 even thirds · 1 main-heavy · 2 slow build

	// swung-hit + ratchet schedulers (per channel), timed off the measured clock interval
	float clkMeasure = 0.f, clkInterval = 0.f;      // samples since last CLOCK / between CLOCKs
	float swDelay[FILL_NCH] = {};                   // countdown to a delayed (swung) hit; 0 = none
	float pendVel[FILL_NCH] = {}; bool pendAcc[FILL_NCH] = {}; int pendRat[FILL_NCH] = {};
	int   ratLeft[FILL_NCH] = {}; float ratPeriod[FILL_NCH] = {}, ratTimer[FILL_NCH] = {};

	dsp::SchmittTrigger clockTrig, barTrig, resetTrig, reseedTrig;
	dsp::PulseGenerator gatePulse[FILL_NCH], accPulse[FILL_NCH];
	float velHold[FILL_NCH] = {};
	int pulseWidthIdx = 0;
	int screenTab = 0;                        // 0 = active pattern · 1..4 = Genre / Region / User / Favs browser
	int browseBank[FILL_NAXIS] = {};          // bank (family) shown per browser axis
	std::set<std::string> favIds;             // favorite set ids — shared across instances + sessions

	// Favorites persist plugin-wide in the Rack user folder (survives patches/sessions).
	static std::string favPath() { return asset::user("SignalFunctionSet/fill-favorites.json"); }

	void loadFavs() {
		favIds.clear();
		json_error_t err;
		json_t* root = json_load_file(favPath().c_str(), 0, &err);
		if (!root) return;
		if (json_t* arr = json_object_get(root, "favorites")) {
			size_t n = json_array_size(arr);
			for (size_t i = 0; i < n; i++)
				if (const char* s = json_string_value(json_array_get(arr, i))) favIds.insert(s);
		}
		json_decref(root);
	}

	void toggleFav(const std::string& id) {
		loadFavs();                            // pick up edits from other FILL instances first
		if (!favIds.insert(id).second) favIds.erase(id);
		system::createDirectories(asset::user("SignalFunctionSet"));
		json_t* root = json_object();
		json_t* arr = json_array();
		for (const std::string& s : favIds) json_array_append_new(arr, json_string(s.c_str()));
		json_object_set_new(root, "favorites", arr);
		json_dump_file(root, favPath().c_str(), JSON_INDENT(1));
		json_decref(root);
		buildFavAxis(lib, favIds);
	}

	Fill() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(ACCUM_PARAM, 0.f, 1.f, 0.2f, "Accumulate (pressure built per bar)", "%", 0.f, 100.f);
		configParam(DISCHARGE_PARAM, 0.f, 1.f, 0.5f, "Discharge (pressure vented per fill)", "%", 0.f, 100.f);
		configParam(TIER_PARAM, -2.f, 2.f, 0.f, "Tier offset (0 = follow pressure)");
		getParamQuantity(TIER_PARAM)->snapEnabled = true;
		configSwitch(PHRASE_PARAM, 0.f, 2.f, 1.f, "Fill phrase", {"4 bars", "8 bars", "16 bars"});
		getParamQuantity(PHRASE_PARAM)->snapEnabled = true;
		configSwitch(SYNC_PARAM, 0.f, 1.f, 1.f, "Sync fills to phrase", {"Free (russian roulette)", "Synced"});
		configParam(EXTRAS_PARAM, 0.f, 8.f, 4.f, "Extra triggers (max engine-added notes per bar; 0 = variation off)");
		getParamQuantity(EXTRAS_PARAM)->snapEnabled = true;
		configInput(CLOCK_INPUT, "Clock — steps fire on its edges (required)");
		configInput(BAR_INPUT, "Bar (downbeat; also sets how many clocks make a bar)");
		configInput(RESET_INPUT, "Reset");
		configInput(NEXT_INPUT, "Next queue entry (advances at the next cycle)");
		configButton(RESET_PARAM, "Reset");
		configInput(RESEED_INPUT, "Reseed variation");
		configInput(ACCUM_CV_INPUT, "Accumulate CV");
		configInput(DISCHARGE_CV_INPUT, "Discharge CV");
		configInput(TIER_CV_INPUT, "Tier offset CV (±5V)");
		configInput(SET_CV_INPUT, "Pattern set CV");
		configInput(EXTRAS_CV_INPUT, "Extra triggers CV (0-10V → 0-8)");
		static const char* chn[FILL_NCH] = {"Kick", "Snare", "Closed hat", "Open hat", "Low perc", "High perc", "Clap/rim", "Bell"};
		for (int c = 0; c < FILL_NCH; c++) {
			configOutput(GATE_OUTPUT + c, string::f("%s gate", chn[c]));
			configOutput(VEL_OUTPUT + c, string::f("%s velocity", chn[c]));
			configOutput(ACC_OUTPUT + c, string::f("%s accent", chn[c]));
			configParam(SWING_PARAM + c, 0.f, 0.5f, 0.f, string::f("%s swing (off-beats delayed)", chn[c]), "%", 0.f, 100.f);
		}
		configOutput(FILL_OUTPUT, "Fill gate (high during a fill)");
		configOutput(SET_OUTPUT, "Pattern set (1V per set)");
		configOutput(NUM_OUTPUT, "Time-signature numerator (0.5V/count — Meter's 'Time signature CV absolute')");
		configOutput(DEN_OUTPUT, "Time-signature denominator (1V per denom index: 1,2,4,8,16,32)");
		configOutput(BPM_OUTPUT, "Set tempo (0.01V/BPM — Meter's \"BPM CV absolute\")");
		for (int c = 0; c < FILL_NCH; c++) for (int s = 0; s < FILL_MAX_STEPS; s++) { curProb[c][s] = 1.f; curRat[c][s] = 1; }

		loadAllLibraries(lib);
		loadFavs();
		buildFavAxis(lib, favIds);
		int nSets = std::max(1, (int)lib.sets.size());
		configParam(SET_PARAM, 0.f, (float)(nSets - 1), 0.f, "Pattern set");
		getParamQuantity(SET_PARAM)->snapEnabled = true;
		resolveCycle();
	}

	void onReset() override {
		pressure = 0.f; peakPressure = 0.f; curTier = 0; barsSinceReset = 0; barsSinceFill = 0; fillActive = false;
		queuePos = 0; queueDone = 0; queueStep = false; queueJumpTo = -1;
		started = false; cycleBar = 0; lastStep = -1; clockCount = 0; clocksPerBar = 16; barSuppress = 0;
		for (int c = 0; c < FILL_NCH; c++) { swDelay[c] = 0.f; ratLeft[c] = 0; ratTimer[c] = ratPeriod[c] = 0.f; }
		resolveCycle();
	}

	int phraseLen() { static const int P[3] = {4, 8, 16}; return P[(int)clamp(std::round(params[PHRASE_PARAM].getValue()), 0.f, 2.f)]; }

	const LibPattern* pat(int idx) { return (idx >= 0 && idx < (int)lib.pats.size()) ? &lib.pats[idx] : nullptr; }

	// The clock rate per BEAT does not change when the pattern does; the number of
	// beats in a bar does. Carrying the measured clocks-per-beat across a switch
	// makes the first bar of a new meter correct, instead of mapping it against
	// the old bar length until the next measurement lands.
	void rescaleBarForMeter(int oldBeats) {
		if (clockRes > 0) { clocksPerBar = clockRes * curBeats; return; }
		if (oldBeats > 0 && curBeats != oldBeats && clocksPerBar > 0)
			clocksPerBar = std::max(1, (int)std::lround((double)clocksPerBar * curBeats / oldBeats));
	}

	void copyPattern(const LibPattern& p) {
		for (int c = 0; c < FILL_NCH; c++) for (int s = 0; s < FILL_MAX_STEPS; s++) {
			curVel[c][s] = (s < p.steps) ? p.vel[c][s] : 0.f;
			curAcc[c][s] = (s < p.steps) ? p.acc[c][s] : false;
			curGen[c][s] = false;                             // authored content
			curProb[c][s] = (s < p.steps) ? p.prob[c][s] : 1.f;
			curRat[c][s] = (s < p.steps) ? p.rat[c][s] : 1;
		}
		curSteps = p.steps; curBars = std::max(1, p.bars); curSpb = std::max(1, p.spb);
		curBeats = std::max(1, p.beats);
		curStepsPerBar = std::max(1, curSteps / curBars);
		curPatSwing = clamp(p.swing, 0.f, 1.f);
	}

	// Generated fill fallback (when a set has no fill patterns): a roll accelerating to
	// the downbeat, seeded per fill. Roles: 0 kick, 1 snare, 3 open hat, 4/5 perc.
	// Typed generated fill, built ON the groove's own grid so odd meters and
	// triplet grids keep their length. Four flavours — roll, stutter, drop,
	// cascade — chosen and shaped by a per-fill seed, sized by pressure.
	void buildGeneratedFill(const sfs::Taste& taste, float S, const LibPattern* base) {
		if (base) copyPattern(*base);
		else {
			for (int c = 0; c < FILL_NCH; c++) for (int s = 0; s < FILL_MAX_STEPS; s++) { curVel[c][s] = 0.f; curAcc[c][s] = false; curGen[c][s] = true; curProb[c][s] = 1.f; curRat[c][s] = 1; }
			curSteps = 16; curBars = 1; curSpb = 4; curStepsPerBar = 16; curPatSwing = 0.f;
			curVel[0][0] = 0.9f; curAcc[0][0] = true;
		}
		uint32_t seed = reseedBase ^ (uint32_t)(barsSinceReset * 2654435761u);
		int barEnd = curBars * curStepsPerBar;                      // fill shapes the final bar
		int len = clamp((int)std::round(curStepsPerBar * (0.25f + 0.5f * S)), curSpb, curStepsPerBar);
		int start = barEnd - len;
		// clear the tail (kick stays — it anchors the fill)
		auto clearTail = [&]() {
			for (int c = 1; c < FILL_NCH; c++)
				for (int st = start; st < barEnd; st++) { curVel[c][st] = 0.f; curAcc[c][st] = false; curRat[c][st] = 1; }
		};
		// Pick only from the shapes this style actually plays. A tom cascade is a
		// rock and disco idiom that breaks the frame in MENA, Afro-Cuban, reggae,
		// jazz and funk; a stutter tail reads as electronic almost everywhere else;
		// and drop/silence is wrong in processional and dance-functional styles
		// where the groove is not allowed to stop.
		int allowed[4], nAllowed = 0;
		static const sfs::FillShape MAP[4] = {sfs::F_ROLL, sfs::F_STUTTER, sfs::F_DROP, sfs::F_CASCADE};
		for (int i = 0; i < 4; i++) if (sfs::allows(taste, MAP[i])) allowed[nAllowed++] = i;
		if (!nAllowed) { allowed[nAllowed++] = 0; }                 // a roll is the safe default
		switch (allowed[fhash(seed, 17) % (uint32_t)nAllowed]) {
			case 0: {                                               // ROLL — accelerating run into the downbeat
				clearTail();
				for (int i = 0; i < len; i++) {
					int st = start + i;
					if (fhashF(seed, st * 13 + 3) < 0.12f * (1.f - S)) continue;
					float ramp = 0.4f + 0.6f * (len > 1 ? (float)i / (len - 1) : 1.f);
					int ch = (fhashF(seed, st * 13 + 5) < 0.3f + 0.3f * S) ? 4 : 1;
					curVel[ch][st] = clamp(ramp * (0.75f + 0.35f * fhashF(seed, st * 13 + 7)), 0.2f, 1.f);
					curGen[ch][st] = true;
					if (i == len - 1) curAcc[ch][st] = true;
				}
				if (fhashF(seed, 42) < 0.6f) { curVel[3][barEnd - 1] = 0.8f; curGen[3][barEnd - 1] = true; }
				break;
			}
			case 1: {                                               // STUTTER — groove kept, tail hits ratcheted
				int hits = 0;
				for (int c = 1; c < FILL_NCH; c++)
					for (int st = start; st < barEnd; st++)
						if (curVel[c][st] > 0.f) {
							curRat[c][st] = 2 + (int)(fhash(seed, c * 31 + st) % 3u);
							curVel[c][st] = clamp(curVel[c][st] * 1.15f, 0.f, 1.f);
							curGen[c][st] = true; hits++;
						}
				if (!hits) { curVel[1][start] = 0.85f; curRat[1][start] = 4; curGen[1][start] = true; }
				break;
			}
			case 2: {                                               // DROP — space as tension, slam the last step
				clearTail();
				curVel[1][barEnd - 1] = 1.f; curAcc[1][barEnd - 1] = true; curGen[1][barEnd - 1] = true;
				curVel[6][barEnd - 1] = 0.8f; curGen[6][barEnd - 1] = true;
				break;
			}
			default: {                                              // CASCADE — hi→lo tom run down the tail
				clearTail();
				for (int i = 0; i < len; i++) {
					int st = start + i;
					if (fhashF(seed, st * 11 + 1) < 0.1f) continue;
					int ch = (i * 2 < len) ? 5 : 4;
					curVel[ch][st] = clamp(0.5f + 0.5f * (len > 1 ? (float)i / (len - 1) : 1.f), 0.3f, 1.f);
					curGen[ch][st] = true;
				}
				curVel[0][barEnd - 1] = std::max(curVel[0][barEnd - 1], 0.9f);
				curAcc[0][barEnd - 1] = true; curGen[0][barEnd - 1] = true;
				break;
			}
		}
	}

	// Seeded per-fill mutation so one authored fill doesn't play identically every
	// phrase. Ratchets go ONLY on the lane the style ratchets: trap and drill on
	// the closed hat, crunk on the snare, footwork on the KICK. A blanket "ratchet
	// anything but the kick" gets all three wrong, and gives every other style far
	// more retriggers than it should ever have.
	void mutateFill(const sfs::Taste& taste, uint32_t seed, float S) {
		for (int c = 0; c < FILL_NCH; c++) {
			if (sfs::frozen(taste, c)) continue;
			for (int st = 0; st < curSteps; st++) {
				if (curVel[c][st] > 0.f) {
					bool canRatchet = (c == taste.ratchetLane) && taste.ratchet > 0.01f;
					if (canRatchet && curRat[c][st] == 1
					    && fhashF(seed, c * 977 + st * 13) < taste.ratchet * (0.35f + 0.65f * S)) {
						curRat[c][st] = 2 + (int)(fhash(seed, c * 31 + st) % (taste.ratchet > 0.6f ? 3u : 2u));
						curGen[c][st] = true;
					}
					curVel[c][st] = clamp(curVel[c][st] * (0.92f + 0.2f * fhashF(seed, c * 511 + st)), 0.15f, 1.f);
				} else if ((taste.addedLanes & sfs::laneBit(c))
				           && fhashF(seed, c * 313 + st * 7) < 0.06f * taste.added * (0.5f + S)) {
					curVel[c][st] = 0.4f; curGen[c][st] = true;
				}
			}
		}
	}

	// Constructed tiers for sets that only author a main groove (user imports, GM
	// singles): sparse thins to the backbone, lift densifies. Seeded by the SET id
	// so the derived section is stable — it's a section, not a fill. Scaled by the
	// set's vary (and skipped entirely at vary 0, so timelines stay pure).
	void deriveSparse(uint32_t seed, float vary) {
		float keepP = clamp(0.5f - vary * 0.35f, 0.05f, 0.5f);      // how much colour survives
		for (int c = 1; c < FILL_NCH; c++)
			for (int st = 0; st < curSteps; st++) {
				if (curVel[c][st] == 0.f) continue;
				bool keep;
				if (c == 1) keep = curVel[1][st] > 0.55f;           // real snare hits, no ghosts
				else if (c == 2 || c == 3) keep = (st % curSpb) == 0;   // timekeeper on beats only
				else keep = curAcc[c][st] || fhashF(seed, c * 131 + st) < keepP;
				if (!keep) { curVel[c][st] = 0.f; curAcc[c][st] = false; curRat[c][st] = 1; }
			}
	}

	void deriveLift(uint32_t seed, float vary) {
		float k = clamp(vary * 2.f, 0.3f, 1.f);
		int hat = 0; for (int st = 0; st < curSteps; st++) if (curVel[2][st] > 0.f) hat++;
		int stride = (hat * 2 >= curSteps) ? 1 : std::max(1, curSpb / 2);   // densify one level
		for (int st = 0; st < curSteps; st++) {
			if (curVel[2][st] == 0.f && (st % stride) == 0) { curVel[2][st] = 0.5f; curGen[2][st] = true; }
			bool off = curSpb > 1 && (st % curSpb) == curSpb / 2;
			if (off && curVel[3][st] == 0.f && fhashF(seed, st * 7 + 1) < 0.5f * k) { curVel[3][st] = 0.55f; curGen[3][st] = true; }
			if (curVel[1][st] == 0.f && (st % curSpb) && fhashF(seed, st * 7 + 2) < 0.18f * k) { curVel[1][st] = 0.4f; curGen[1][st] = true; }
			if (curVel[6][st] == 0.f && curVel[1][st] > 0.7f && fhashF(seed, st * 7 + 3) < 0.5f * k) { curVel[6][st] = 0.6f; curGen[6][st] = true; }
		}
		for (int c = 0; c < FILL_NCH; c++)                          // push the energy up a notch
			for (int st = 0; st < curSteps; st++)
				if (curVel[c][st] > 0.f) curVel[c][st] = clamp(curVel[c][st] * 1.1f, 0.f, 1.f);
	}

	// Seeded embellishment on the base tier, scaled by pressure × the SET's `vary`
	// (identity strength): motorik at 0.1 stays nearly incorruptible, funk at 0.7
	// breathes. Seeded by phrase position (form, not jitter); the kick lane is never
	// touched. Engine additions are flagged in curGen so the display can show them.
	// Added notes follow the style's taste: only the lanes that style actually
	// ornaments, never a frozen one, and never the kick unless the style says so
	// (in funk and trap the kick pattern IS the hook).
	void applyVariation(const sfs::Taste& taste, float vary) {
		int maxExtra = clamp((int)std::round(params[EXTRAS_PARAM].getValue() + inputs[EXTRAS_CV_INPUT].getVoltage() / 10.f * 8.f), 0, 8)
		               * std::max(1, curBars);
		if (maxExtra <= 0 || taste.added <= 0.f) return;
		float intensity = pressure * clamp(vary, 0.f, 1.f) * taste.added;
		uint32_t sd = reseedBase ^ (uint32_t)(barsSinceReset * 2654435761u) ^ ((uint32_t)curTier << 20);
		int added = 0;
		for (int st = 0; st < curSteps && added < maxExtra; st++) {
			for (int c = 0; c < FILL_NCH && added < maxExtra; c++) {
				if (!(taste.addedLanes & sfs::laneBit(c))) continue;   // not this style's business
				if (sfs::frozen(taste, c)) continue;                   // timeline: never touched
				if (curVel[c][st] > 0.f) continue;
				float p = intensity * ((c == sfs::L_CHH) ? 0.22f : (c == sfs::L_SNARE) ? 0.12f : 0.14f);
				if (fhashF(sd, c * 977 + st * 13) >= p) continue;
				curVel[c][st] = (c == sfs::L_SNARE) ? 0.35f : 0.45f;   // added snares are ghosts
				curGen[c][st] = true;
				added++;
			}
		}
	}

	void resolveCycle() {
		if (lib.sets.empty()) { curSteps = 16; curBars = 1; curStepsPerBar = 16; fillActive = false;
			for (int c = 0; c < FILL_NCH; c++) for (int s = 0; s < FILL_MAX_STEPS; s++) curVel[c][s] = 0.f; return; }
		LibSet& set = lib.sets[clamp(curSet, 0, (int)lib.sets.size() - 1)];
		bool syncOn = params[SYNC_PARAM].getValue() > 0.5f;

		// Tier is PHRASE-LATCHED: it re-evaluates only at phrase starts (synced), after
		// a discharge (free mode), or when the TIER offset moves — never per-bar. A
		// phrase sits on one tier while pressure builds underneath, so the groove keeps
		// its identity instead of hopping sparse/main/lift every bar.
		float tcv = inputs[TIER_CV_INPUT].getVoltage() / 5.f * 2.f;
		int bias = (int)std::round(params[TIER_PARAM].getValue() + tcv);
		bool phraseStart = ((barsSinceReset - 1) % phraseLen()) == 0;
		bool afterFill = fillActive;   // the cycle just ended was a fill
		// Where the tiers sit. Even thirds put everything in lift within a few bars,
		// because pressure saturates at 1.0 and stays there; main-heavy keeps the
		// basic groove for roughly the first half of a phrase.
		static const float T0[3] = {0.33f, 0.12f, 0.20f};
		static const float T1[3] = {0.67f, 0.56f, 0.72f};
		int tb = clamp(tierBalance, 0, 2);

		if (tierMode == 0) {
			// Rise through the phrase: position leads, pressure colours it. On an
			// 8-bar phrase with the default balance that is about four bars of the
			// basic groove, three of lift, and the last bar venting as a fill.
			int pl = std::max(1, phraseLen());
			float prog = (float)(barsSinceReset % pl) / (float)pl;
			float lvl = 0.65f * prog + 0.35f * peakPressure;
			int pTier = (lvl < T0[tb]) ? 0 : (lvl < T1[tb]) ? 1 : 2;
			curTier = clamp(pTier + bias, 0, 2);
			lastTierBias = bias;
		} else if (!started || bias != lastTierBias || (syncOn ? phraseStart : afterFill)) {
			// Latched: one tier for the whole phrase, taken from the phrase's PEAK
			// pressure (what the gauge showed), not the post-discharge remainder.
			int pTier = (peakPressure < T0[tb]) ? 0 : (peakPressure < T1[tb]) ? 1 : 2;
			curTier = clamp(pTier + bias, 0, 2);
			lastTierBias = bias;
			peakPressure = pressure;               // start tracking the new phrase
		}

		bool fillNow = syncOn ? (barsSinceReset > 0 && (barsSinceReset % phraseLen()) == 0)
		                      : (random::uniform() < pressure);
		if (fillNow && !playFills) {
			float discharge = clamp(params[DISCHARGE_PARAM].getValue()
			                        + inputs[DISCHARGE_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			pressure = clamp(pressure - discharge * pressure, 0.f, 1.f);
			barsSinceFill = 0;                 // the arc still turned over
			fillNow = false;
		}

		if (fillNow) {
			// 70% a library fill (seeded pick + seeded mutation so one authored fill
			// still varies phrase to phrase), 30% a typed generated fill — always
			// generated when the set authors none.
			uint32_t fseed = reseedBase ^ (uint32_t)(barsSinceReset * 2654435761u);
			const LibPattern* fp = set.fills.empty() ? nullptr
			                     : pat(set.fills[fhash(reseedBase, barsSinceReset) % set.fills.size()]);
			int prevBeats = curBeats;
			if (fp && fhashF(fseed, 5) < 0.7f) { copyPattern(*fp); mutateFill(set.taste, fseed, pressure); }
			else buildGeneratedFill(set.taste, pressure, pat(set.main));
			rescaleBarForMeter(prevBeats);
			fillActive = true; fillFlash = 1.f; barsSinceFill = 0;
			float discharge = clamp(params[DISCHARGE_PARAM].getValue() + inputs[DISCHARGE_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			pressure = clamp(pressure - discharge * pressure, 0.f, 1.f);
		} else {
			int idx = (curTier == 0) ? set.sparse : (curTier == 2) ? set.lift : set.main;
			const LibPattern* p = pat(idx); if (!p) p = pat(set.main);
			int prevBeats = curBeats;
			if (p) copyPattern(*p);
			rescaleBarForMeter(prevBeats);
			// sets that only author a main groove get constructed outer tiers
			if (p && set.vary > 0.f) {
				uint32_t sid = strHash(set.id);
				if (curTier == 0 && set.sparse == set.main) deriveSparse(sid, set.vary);
				if (curTier == 2 && set.lift == set.main) deriveLift(sid, set.vary);
			}
			applyVariation(set.taste, set.vary);
			fillActive = false;
		}
	}

	void applyPendingSet() {
		int n = (int)lib.sets.size();
		if (n <= 0) return;
		if (!queue.empty()) {
			queuePos = clamp(queuePos, 0, (int)queue.size() - 1);
			pendingSet = clamp(queue[queuePos].set, 0, n - 1);
		} else {
			float sel = params[SET_PARAM].getValue() + inputs[SET_CV_INPUT].getVoltage() / 10.f * (n - 1);
			pendingSet = clamp((int)std::round(sel), 0, n - 1);
		}
		curSet = pendingSet;   // apply at the cycle boundary (called from onBar)
	}

	void onBar() {
		barsSinceReset++;
		barsSinceFill++;                     // reset to 0 when a fill cycle resolves
		float accum = clamp(params[ACCUM_PARAM].getValue() + inputs[ACCUM_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		pressure = clamp(pressure + accum, 0.f, 1.f);
		peakPressure = std::max(peakPressure, pressure);   // the tier latches from this, pre-vent
		if (!started || cycleBar + 1 >= curBars) {   // start a new pattern cycle
			cycleBar = 0; lastStep = -1;
			// The queue moves between cycles, never mid-pattern. A NEXT trigger is
			// held until this point for the same reason the set switch is.
			if (!queue.empty() && started) {
				if (queueJumpTo >= 0) {
					queuePos = clamp(queueJumpTo, 0, (int)queue.size() - 1);
					queueDone = 0; queueJumpTo = -1; queueStep = false;
				}
				else if (queueStep) { queueAdvance(); queueStep = false; }
				else if (advanceMode == 0 && ++queueDone >= std::max(1, queue[queuePos].reps))
					queueAdvance();
			}
			applyPendingSet();
			resolveCycle();
		} else {                                     // continue a multi-bar cycle
			cycleBar++;
			lastStep = cycleBar * curStepsPerBar - 1;
		}
	}

	// Map a clock index within the bar to a pattern step and fire it (once).
	void fireForClock(int stepInBar) {
		int g = cycleBar * curStepsPerBar + stepInBar;
		if (g >= curSteps) g = curSteps - 1;
		if (g != lastStep) { fireStep(g); lastStep = g; }
	}

	// One pattern step's duration in samples (from the measured clock interval).
	float stepDurSamples() {
		if (clkInterval <= 1.f) return 0.f;
		return clkInterval * (float)clocksPerBar / std::max(1, curStepsPerBar);
	}

	// Fire a hit now: gate + vel + accent, and arm the ratchet burst if the step asks.
	void fireHit(int c, float v, bool acc, int rat) {
		float pw = sfs::pulseWidthSec(pulseWidthIdx);
		gatePulse[c].trigger(pw);
		velHold[c] = v;
		if (acc) accPulse[c].trigger(pw);
		float sd = stepDurSamples();
		if (rat > 1 && sd > 8.f) { ratLeft[c] = rat - 1; ratPeriod[c] = sd / rat; ratTimer[c] = ratPeriod[c]; }
	}

	void fireStep(int gstep) {
		if (gstep < 0 || gstep >= curSteps) return;
		int stepInBar = gstep % std::max(1, curStepsPerBar);
		bool offbeat = (stepInBar % 2) == 1 && (curSpb % 3) != 0;   // triplet grids don't swing
		float sd = stepDurSamples();
		for (int c = 0; c < FILL_NCH; c++) {
			float v = curVel[c][gstep];
			if (v <= 0.f) continue;
			if (curProb[c][gstep] < 1.f && random::uniform() >= curProb[c][gstep]) continue;   // probability
			// Per-channel swing: the pattern's authored swing (format 0..1 → up to triplet
			// feel) plus this channel's trimpot, as a fraction of the step PAIR. Off-beats
			// fire late via the scheduler; on-beats always land on the grid.
			float sw = clamp(curPatSwing / 6.f + params[SWING_PARAM + c].getValue(), 0.f, 0.5f);
			float delay = (offbeat && sw > 0.005f && sd > 8.f) ? std::min(sw * 2.f * sd, sd * 0.95f) : 0.f;
			if (delay < 1.f) fireHit(c, v, curAcc[c][gstep], curRat[c][gstep]);
			else { swDelay[c] = delay; pendVel[c] = v; pendAcc[c] = curAcc[c][gstep]; pendRat[c] = curRat[c][gstep]; }
		}
	}

	void process(const ProcessArgs& args) override {
		std::unique_lock<std::mutex> lk(libMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;         // library rebuild in flight (rare, user import)
		float dt = args.sampleTime;
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) onReset();
		if (resetBtnTrig.process(params[RESET_PARAM].getValue())) onReset();
		if (nextTrig.process(inputs[NEXT_INPUT].getVoltage(), 0.1f, 1.f)) queueStep = true;
		if (reseedTrig.process(inputs[RESEED_INPUT].getVoltage(), 0.1f, 1.f)) reseedBase = random::u32();

		bool barConn = inputs[BAR_INPUT].isConnected();
		bool bar = barConn && barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f);
		bool clk = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);   // always consume the edge
		clkMeasure += 1.f;
		if (clk) { if (clkMeasure > 1.f) clkInterval = clkMeasure; clkMeasure = 0.f; }   // for swing/ratchet timing

		if (bar) {
			if (started && clockCount > 0) clocksPerBar = clockCount;   // measure over the bar just ended
			onBar();                        // downbeat: resolve cycle / advance bar
			clockCount = 1;                 // the downbeat counts as clock 0 of the new bar
			started = true;
			fireForClock(0);
			barSuppress = (int)(args.sampleRate * 0.002f);   // swallow the coincident downbeat CLOCK
		}

		if (barSuppress > 0) {
			barSuppress--;
		} else if (clk) {
			if (!started) {                 // free-run start on the first clock (BAR unpatched)
				onBar(); clockCount = 1; started = true; fireForClock(0);
			} else {
				clockCount++;
				if (!barConn && clockCount > clocksPerBar) {   // internal bar wrap when BAR isn't patched
					onBar(); clockCount = 1; fireForClock(0);
				} else {
					int stepInBar = (int)((long)(clockCount - 1) * curStepsPerBar / std::max(1, clocksPerBar));
					fireForClock(stepInBar);
				}
			}
		}

		// scheduled swung hits + ratchet sub-hits
		for (int c = 0; c < FILL_NCH; c++) {
			if (swDelay[c] > 0.f) {
				swDelay[c] -= 1.f;
				if (swDelay[c] <= 0.f) { swDelay[c] = 0.f; fireHit(c, pendVel[c], pendAcc[c], pendRat[c]); }
			}
			if (ratLeft[c] > 0) {
				ratTimer[c] -= 1.f;
				if (ratTimer[c] <= 0.f) { gatePulse[c].trigger(sfs::pulseWidthSec(pulseWidthIdx)); ratTimer[c] += ratPeriod[c]; ratLeft[c]--; }
			}
		}

		for (int c = 0; c < FILL_NCH; c++) {
			outputs[GATE_OUTPUT + c].setVoltage(gatePulse[c].process(dt) ? 10.f : 0.f);
			outputs[VEL_OUTPUT + c].setVoltage(velHold[c] * 10.f);
			outputs[ACC_OUTPUT + c].setVoltage(accPulse[c].process(dt) ? 10.f : 0.f);
		}
		outputs[FILL_OUTPUT].setVoltage(fillActive ? 10.f : 0.f);
		outputs[SET_OUTPUT].setVoltage(clamp((float)curSet, 0.f, 10.f));   // 1V per set
		if (!lib.sets.empty()) {                                          // 0.01V per BPM
			float bpm = lib.sets[clamp(curSet, 0, (int)lib.sets.size() - 1)].bpm;
			outputs[BPM_OUTPUT].setVoltage(clamp(bpm * 0.01f, 0.f, 10.f));
		}
		// Time signature of the active set's main pattern → Meter's absolute NUM/DEN CV.
		{
			int num = 4, den = 4;
			if (!lib.sets.empty()) {
				const LibPattern* mp = pat(lib.sets[clamp(curSet, 0, (int)lib.sets.size() - 1)].main);
				if (mp) { num = mp->meterNum; den = mp->meterDen; }
			}
			int denIdx = 2;   // {1,2,4,8,16,32} → index
			for (int i = 0, v = 1; i < 6; i++, v *= 2) if (den == v) { denIdx = i; break; }
			outputs[NUM_OUTPUT].setVoltage(num * 0.5f);
			outputs[DEN_OUTPUT].setVoltage((float)denIdx);
		}
		lights[SYNC_LIGHT].setBrightness(params[SYNC_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		fillFlash = std::max(0.f, fillFlash - dt / 0.15f);
		lights[FILL_LIGHT].setBrightness(fillFlash);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "setIdx", json_integer(curSet));
		json_object_set_new(root, "clockRes", json_integer(clockRes));
		json_object_set_new(root, "tierMode", json_integer(tierMode));
		json_object_set_new(root, "playFills", json_boolean(playFills));
		json_object_set_new(root, "advanceMode", json_integer(advanceMode));
		json_object_set_new(root, "queuePos", json_integer(queuePos));
		{
			json_t* q = json_array();
			for (auto& it : queue) {
				json_t* o = json_object();
				json_object_set_new(o, "set", json_integer(it.set));
				json_object_set_new(o, "reps", json_integer(it.reps));
				json_array_append_new(q, o);
			}
			json_object_set_new(root, "queue", q);
		}
		json_object_set_new(root, "tierBalance", json_integer(tierBalance));
		json_object_set_new(root, "pulseWidthIdx", json_integer(pulseWidthIdx));
		json_object_set_new(root, "screenTab", json_integer(screenTab));
		json_object_set_new(root, "browseBankG", json_integer(browseBank[0]));
		json_object_set_new(root, "browseBankR", json_integer(browseBank[1]));
		json_object_set_new(root, "browseBankU", json_integer(browseBank[2]));
		json_object_set_new(root, "browseBankF", json_integer(browseBank[3]));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "clockRes")) clockRes = clamp((int)json_integer_value(j), 0, 48);
		if (json_t* j = json_object_get(root, "tierMode")) tierMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(root, "playFills")) playFills = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "advanceMode")) advanceMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* q = json_object_get(root, "queue")) {
			queue.clear();
			size_t n = json_array_size(q);
			for (size_t i = 0; i < n; i++) {
				json_t* o = json_array_get(q, i);
				QueueItem it = {0, 1};
				if (json_t* v = json_object_get(o, "set")) it.set = (int)json_integer_value(v);
				if (json_t* v = json_object_get(o, "reps")) it.reps = clamp((int)json_integer_value(v), 1, 64);
				queue.push_back(it);
			}
		}
		if (json_t* j = json_object_get(root, "queuePos")) queuePos = (int)json_integer_value(j);
		if (json_t* j = json_object_get(root, "tierBalance")) tierBalance = clamp((int)json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(root, "setIdx")) { curSet = pendingSet = clamp((int)json_integer_value(j), 0, std::max(0, (int)lib.sets.size() - 1)); }
		if (json_t* j = json_object_get(root, "pulseWidthIdx")) pulseWidthIdx = clamp((int)json_integer_value(j), 0, sfs::NUM_PULSE_WIDTHS - 1);
		if (json_t* j = json_object_get(root, "screenTab")) screenTab = clamp((int)json_integer_value(j), 0, FILL_NAXIS + 1);
		if (json_t* j = json_object_get(root, "browseBankG")) browseBank[0] = clamp((int)json_integer_value(j), 0, std::max(0, (int)lib.families[0].size() - 1));
		if (json_t* j = json_object_get(root, "browseBankR")) browseBank[1] = clamp((int)json_integer_value(j), 0, std::max(0, (int)lib.families[1].size() - 1));
		if (json_t* j = json_object_get(root, "browseBankU")) browseBank[2] = clamp((int)json_integer_value(j), 0, std::max(0, (int)lib.families[2].size() - 1));
		if (json_t* j = json_object_get(root, "browseBankF")) browseBank[3] = clamp((int)json_integer_value(j), 0, std::max(0, (int)lib.families[3].size() - 1));
	}
};

// ─── Display ─────────────────────────────────────────────────────────────────
static const NVGcolor FILL_CHCOL[FILL_NCH] = {
	nvgRGB(0xec, 0x65, 0x2e), nvgRGB(0xe0, 0xa0, 0x30), nvgRGB(0xd0, 0xd0, 0x50), nvgRGB(0x40, 0xc0, 0xc0),
	nvgRGB(0x90, 0x60, 0xc0), nvgRGB(0x5a, 0x9b, 0xd4), nvgRGB(0x3f, 0xc9, 0x9a), nvgRGB(0xc0, 0x5a, 0x8a),
};

static const char* FILL_ROWLBL[FILL_NCH] = {"KIK", "SNR", "CHH", "OHH", "LO", "HI", "CLP", "BEL"};

// Shared SFS display palette (matches Note/Beat).
static const NVGcolor FBG         = nvgRGB(0x1A, 0x1A, 0x32);
static const NVGcolor FBLUE       = nvgRGB(0x00, 0x97, 0xDE);
static const NVGcolor FBLUE_DARK  = nvgRGB(0x0D, 0x59, 0x86);
static const NVGcolor FBLUE_LINE  = nvgRGB(0x0D, 0x59, 0x88);
static const NVGcolor FPURPLE     = nvgRGB(0x35, 0x35, 0x4D);
static const NVGcolor FPURPLE_MID = nvgRGB(0x4A, 0x4A, 0x66);
static const NVGcolor FORANGE     = nvgRGB(0xEC, 0x65, 0x2E);
static const NVGcolor FTEXT       = nvgRGB(0xFF, 0xFF, 0xFF);
static const NVGcolor FTEXT_DIM   = nvgRGB(0x80, 0x80, 0x80);

struct FillDisplay : Widget {
	Fill* module = nullptr;
	std::shared_ptr<Font> font;
	// Note draws its display in a 174-unit space across 46mm (1 unit = 46/174 mm ≈ 0.264mm).
	// FILL's display is 104mm wide = 393.4 of those SAME units — so the 9-unit fonts,
	// 2-unit radii, 18-unit tabs and 7-unit margins below render at exactly Note's size.
	float u() const { return box.size.x / 393.4f; }
	// Note's tab geometry: tabs y=8 h=18 · rail y=32 · stem 28→32.5 · content y=35.

	void demoVel(float demo[FILL_NCH][FILL_MAX_STEPS]) {
		const char* v[5] = {"9.......9.9.....", "....9.......9...", "8.6.8.6.8.6.8.6.", "..............7.", "..5.......5....."};
		for (int c = 0; c < 5; c++) for (int s = 0; s < 16; s++) { char ch = v[c][s]; demo[c][s] = (ch >= '1' && ch <= '9') ? (ch - '0') / 9.f : 0.f; }
	}

	void drawTabs(NVGcontext* vg, int tab) {
		const float U = u(), w = box.size.x;
		const char* names[6] = {"PATTERN", "GENRE", "REGION", "USER", "FAVS", "QUEUE"};
		for (int i = 0; i < 6; i++) {
			float x = (7.f + i * 52.f) * U;                       // 50-unit tabs, 2-unit gap (Note: 38+2)
			nvgBeginPath(vg); nvgRoundedRect(vg, x, 8.f * U, 50.f * U, 18.f * U, 2.f * U);
			nvgFillColor(vg, i == tab ? FBLUE_DARK : FPURPLE); nvgFill(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);   // Note's exact font size
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, i == tab ? FTEXT : FTEXT_DIM);
				nvgText(vg, x + 25.f * U, 17.f * U, names[i], NULL);
			}
		}
		// connector rail + stem from the active tab — Note's exact geometry
		nvgStrokeColor(vg, FBLUE_LINE); nvgStrokeWidth(vg, 1.f);
		nvgBeginPath(vg); nvgMoveTo(vg, 7.f * U, 32.f * U); nvgLineTo(vg, w - 7.f * U, 32.f * U); nvgStroke(vg);
		float stemX = (7.f + tab * 52.f + 25.f) * U;
		nvgBeginPath(vg); nvgMoveTo(vg, stemX, 28.f * U); nvgLineTo(vg, stemX, 32.5f * U); nvgStroke(vg);
	}

	void drawPatternTab(NVGcontext* vg) {
		const float U = u(), w = box.size.x, h = box.size.y;
		const float W = w / U, H = h / U;                 // display size in Note units (~393 × 197)
		float demo[FILL_NCH][FILL_MAX_STEPS] = {};
		static const bool demoGen[FILL_NCH][FILL_MAX_STEPS] = {};
		const float (*vel)[FILL_MAX_STEPS] = module ? module->curVel : demo;
		const bool (*gen)[FILL_MAX_STEPS] = module ? module->curGen : demoGen;
		int steps = module ? module->curSteps : 16, spb = module ? module->curSpb : 4;
		int curStep = module ? module->lastStep : 4;
		float pressure = module ? module->pressure : 0.55f, flash = module ? module->fillFlash : 0.f;
		if (!module) { demoVel(demo); }
		if (steps < 1) steps = 16;

		static const char* TIERN[3] = {"SPARSE", "MAIN", "LIFT"};
		std::string label = !module ? "Motorik — MAIN"
		    : module->lib.sets.empty() ? "no library"
		    : module->lib.sets[clamp(module->curSet, 0, (int)module->lib.sets.size() - 1)].name
		      + (module->fillActive ? " — FILL" : std::string(" — ") + TIERN[clamp(module->curTier, 0, 2)]);
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, FTEXT); nvgText(vg, 25.f * U, 37.f * U, label.c_str(), NULL);
		}
		// grid: row-label gutter left, pressure meter right, all in Note units
		const float lblW = 18.f, pbW = 6.f, pbX = W - 7.f - pbW;
		const float gxu = 7.f + lblW, gyu = 51.f, gwu = pbX - 4.f - gxu, ghu = H - gyu - 5.f;

		// bars-since-last-fill pips (top right): one slot per phrase bar, filled as the
		// phrase elapses; all pips go orange while the fill itself is playing.
		if (module) {
			int n = clamp(module->phraseLen(), 1, 16);
			int done = std::min(module->barsSinceFill, n);
			float px0 = pbX - 2.f - n * 5.f;
			for (int i = 0; i < n; i++) {
				float x = (px0 + i * 5.f) * U;
				nvgBeginPath(vg); nvgRoundedRect(vg, x, 37.f * U, 4.f * U, 4.f * U, 1.f * U);
				nvgFillColor(vg, module->fillActive ? FORANGE : (i < done ? FBLUE : FPURPLE));
				nvgFill(vg);
			}
		}
		float gx = gxu * U, gy = gyu * U, cw = (gwu / steps) * U, rh = (ghu / FILL_NCH) * U;
		float rad = 2.f * U;
		for (int c = 0; c < FILL_NCH; c++) {
			float y = gy + c * rh;
			if (font) {   // row label (channel role), colour-coded — Note font size
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);
				nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, FILL_CHCOL[c]); nvgText(vg, gx - 3.f * U, y + rh / 2, FILL_ROWLBL[c], NULL);
			}
			for (int st = 0; st < steps; st++) {
				float x = gx + st * cw;
				bool beat = (spb > 0 && st % spb == 0);
				nvgBeginPath(vg); nvgRoundedRect(vg, x + 0.5f, y + 0.5f, cw - 1, rh - 1, rad);
				nvgFillColor(vg, beat ? FPURPLE_MID : FPURPLE); nvgFill(vg);
				float v = vel[c][st];
				if (v > 0.f) {
					NVGcolor col = FILL_CHCOL[c];
					nvgBeginPath(vg); nvgRoundedRect(vg, x + 0.5f, y + 0.5f, cw - 1, rh - 1, rad);
					if (gen[c][st]) {   // engine-added (variation / generated fill) → hollow
						nvgStrokeColor(vg, nvgRGBAf(col.r, col.g, col.b, 0.9f));
						nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
					} else {            // authored → solid
						nvgFillColor(vg, nvgRGBAf(col.r, col.g, col.b, 0.3f + 0.65f * v)); nvgFill(vg);
					}
				}
			}
		}
		float gh = ghu * U;
		if (curStep >= 0 && curStep < steps) {
			float x = gx + curStep * cw;
			nvgBeginPath(vg); nvgRoundedRect(vg, x, gy, cw, gh, rad);
			nvgStrokeColor(vg, FORANGE); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		}
		// pressure meter (right), same cell styling
		float pbx = pbX * U, pbw = pbW * U;
		nvgBeginPath(vg); nvgRoundedRect(vg, pbx, gy, pbw, gh, rad); nvgFillColor(vg, FPURPLE); nvgFill(vg);
		float fillH = gh * clamp(pressure, 0.f, 1.f);
		nvgBeginPath(vg); nvgRoundedRect(vg, pbx, gy + gh - fillH, pbw, fillH, rad); nvgFillColor(vg, FORANGE); nvgFill(vg);
		for (int t = 1; t < 3; t++) { float y = gy + gh - gh * (t / 3.f);
			nvgBeginPath(vg); nvgMoveTo(vg, pbx, y); nvgLineTo(vg, pbx + pbw, y);
			nvgStrokeColor(vg, FPURPLE_MID); nvgStrokeWidth(vg, 1.f); nvgStroke(vg); }
		float pk = clamp(module ? module->peakPressure : 0.f, 0.f, 1.f);
		if (pk > 0.01f) {   // phrase peak — the level the next tier latch will read
			float y = gy + gh - gh * pk;
			nvgBeginPath(vg); nvgMoveTo(vg, pbx, y); nvgLineTo(vg, pbx + pbw, y);
			nvgStrokeColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.8f)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		}
		if (flash > 0.f) {
			nvgBeginPath(vg); nvgRoundedRect(vg, 2.f * U, 34.f * U, w - 4.f * U, h - 36.f * U, 2.f * U);
			nvgStrokeColor(vg, nvgRGBAf(0.92f, 0.4f, 0.18f, flash)); nvgStrokeWidth(vg, 1.5f); nvgStroke(vg);
		}
	}

	// 2-column grid of set names; pending selection highlighted, playing set outlined.
	// Browser layout (Note units): banks in the left third, the selected bank's sets in
	// the right two-thirds, each set row showing name · meter · typical BPM.
	static constexpr float BROW_H = 18.f, BROW_PITCH = 20.f;
	float bankColW() { return (box.size.x / u() - 18.f) / 3.f; }
	float setColX()  { return 7.f + bankColW() + 4.f; }

	int bankScroll = 0, setScroll = 0, queueScroll = 0;   // browser view state (per widget)
	// Row buttons sit at unhighlighted-text weight until the mouse is on them,
	// and flash white for a moment when pressed so the action is confirmed even
	// when its effect is off-screen (a move at the end of a long queue).
	Vec  hoverU = Vec(-1000.f, -1000.f);
	float pressFlash = 0.f;
	int   flashRow = -1, flashCol = -1;

	int   dragIdx = -1;        // queue entry under a reorder drag
	float dragAccum = 0.f;     // travel since the last swap, in Note units

	void onDragMove(const event::DragMove& e) override {
		if (module && dragIdx >= 0 && !module->queue.empty()) {
			auto& q = module->queue;
			dragAccum += e.mouseDelta.y / getAbsoluteZoom() / u();
			// Swap one place at a time, moving the play head with the entry it is
			// pointing at so the queue does not appear to jump under the drag.
			auto move = [&](int to) {
				std::swap(q[dragIdx], q[to]);
				if (module->queuePos == dragIdx) module->queuePos = to;
				else if (module->queuePos == to) module->queuePos = dragIdx;
				dragIdx = to;
			};
			// Strict on both sides: the two thresholds meet at exactly +-half a
			// pitch, and a swap that lands the accumulator on the boundary would
			// be undone by the opposite loop in the same call.
			while (dragAccum > BROW_PITCH * 0.5f && dragIdx < (int)q.size() - 1) {
				move(dragIdx + 1); dragAccum -= BROW_PITCH;
			}
			while (dragAccum < -BROW_PITCH * 0.5f && dragIdx > 0) {
				move(dragIdx - 1); dragAccum += BROW_PITCH;
			}
			e.consume(this);
			return;
		}
		Widget::onDragMove(e);
	}
	void onDragEnd(const event::DragEnd& e) override {
		dragIdx = -1;
		Widget::onDragEnd(e);
	}

	void onHover(const event::Hover& e) override {
		float U = u();
		hoverU = Vec(e.pos.x / U, e.pos.y / U);
		Widget::onHover(e);
	}
	void onLeave(const event::Leave& e) override {
		hoverU = Vec(-1000.f, -1000.f);
		Widget::onLeave(e);
	}
	int lastSelBank = -1, lastSelSet = -1, lastAxis = -1;
	int visRows() { return std::max(1, (int)((box.size.y / u() - 35.f - 4.f) / BROW_PITCH)); }

	void drawScrollbar(NVGcontext* vg, float xu, int n, int scroll, int vis) {
		if (n <= vis) return;
		const float U = u();
		float trackH = vis * BROW_PITCH - 2.f;
		float thH = std::max(6.f, trackH * vis / n);
		float thY = 35.f + (trackH - thH) * scroll / std::max(1, n - vis);
		nvgBeginPath(vg); nvgRect(vg, xu * U, thY * U, 1.5f * U, thH * U);
		nvgFillColor(vg, FBLUE_LINE); nvgFill(vg);
	}

	void drawBrowserTab(NVGcontext* vg, int axis) {
		const float U = u(), w = box.size.x;
		const float W = w / U;
		if (!module) return;
		Library& lib = module->lib;
		if (axis != lastAxis) { bankScroll = setScroll = 0; lastSelBank = lastSelSet = -1; lastAxis = axis; }
		int nb = (int)lib.families[axis].size();
		if (nb == 0) {                            // empty axis → hint text
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, FTEXT_DIM);
				const char* l1 = (axis == AX_USER) ? "No user pattern banks yet"
				               : (axis == AX_FAV)  ? "No favorites yet" : "No pattern libraries found";
				const char* l2 = (axis == AX_USER) ? "Module menu > Open user patterns folder"
				               : (axis == AX_FAV)  ? "Right-click any set to favorite it" : "";
				nvgText(vg, w / 2, 62.f * U, l1, NULL);
				if (l2[0]) nvgText(vg, w / 2, 76.f * U, l2, NULL);
			}
			return;
		}
		int bank = clamp(module->browseBank[axis], 0, nb - 1);
		int selSet = clamp((int)std::round(module->params[Fill::SET_PARAM].getValue()), 0, (int)lib.sets.size() - 1);
		float leftW = bankColW(), rightX = setColX(), rightW = W - 7.f - rightX, rad = 2.f * U;
		int vis = visRows();
		const std::vector<int>& ss = lib.famSets[axis][bank];
		int ns = (int)ss.size();

		// snap scroll to keep a CHANGED selection visible; manual scrolling is preserved
		if (bank != lastSelBank) {
			if (bank < bankScroll || bank >= bankScroll + vis) bankScroll = std::max(0, bank - vis / 2);
			setScroll = 0; lastSelBank = bank;
		}
		if (selSet != lastSelSet) {
			int k = -1; for (int i = 0; i < ns; i++) if (ss[i] == selSet) { k = i; break; }
			if (k >= 0 && (k < setScroll || k >= setScroll + vis)) setScroll = std::max(0, k - vis / 2);
			lastSelSet = selSet;
		}
		bankScroll = clamp(bankScroll, 0, std::max(0, nb - vis));
		setScroll = clamp(setScroll, 0, std::max(0, ns - vis));

		for (int r = 0; r < vis && bankScroll + r < nb; r++) {       // banks (left)
			int b = bankScroll + r;
			float y = (35.f + r * BROW_PITCH) * U;
			bool isSel = (b == bank);
			nvgBeginPath(vg); nvgRoundedRect(vg, 7.f * U, y, (leftW - 3.f) * U, BROW_H * U, rad);
			nvgFillColor(vg, isSel ? FBLUE_DARK : FPURPLE); nvgFill(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, isSel ? FTEXT : FTEXT_DIM);
				std::string nm = famDisplay(lib.families[axis][b]);
				nvgText(vg, 12.f * U, y + BROW_H * U / 2, nm.c_str(), NULL);
			}
		}
		drawScrollbar(vg, 7.f + leftW - 2.f, nb, bankScroll, vis);

		for (int r = 0; r < vis && setScroll + r < ns; r++) {        // sets of the bank (right)
			int i = ss[setScroll + r];
			LibSet& st = lib.sets[i];
			float y = (35.f + r * BROW_PITCH) * U;
			bool isSel = (i == selSet), isPlay = (i == module->curSet);
			nvgBeginPath(vg); nvgRoundedRect(vg, rightX * U, y, rightW * U, BROW_H * U, rad);
			nvgFillColor(vg, isSel ? FBLUE_DARK : FPURPLE); nvgFill(vg);
			if (isPlay && !isSel) { nvgStrokeColor(vg, FORANGE); nvgStrokeWidth(vg, 1.f); nvgStroke(vg); }
			if (module->favIds.count(st.id)) {    // favorite marker: orange corner tick
				nvgBeginPath(vg);
				nvgMoveTo(vg, (rightX + rightW - 7.f) * U, y + 1.f * U);
				nvgLineTo(vg, (rightX + rightW - 1.f) * U, y + 1.f * U);
				nvgLineTo(vg, (rightX + rightW - 1.f) * U, y + 7.f * U);
				nvgClosePath(vg); nvgFillColor(vg, FORANGE); nvgFill(vg);
			}
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);
				nvgFillColor(vg, isSel ? FTEXT : FTEXT_DIM);
				std::string nm = st.name; if (nm.size() > 26) nm = nm.substr(0, 25) + "…";
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				nvgText(vg, (rightX + 5.f) * U, y + BROW_H * U / 2, nm.c_str(), NULL);
				const char* meter = (st.main >= 0 && st.main < (int)lib.pats.size()) ? lib.pats[st.main].meter.c_str() : "4/4";
				std::string info = string::f("%s · %d", meter, (int)std::round(st.bpm));
				nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
				nvgText(vg, (rightX + rightW - QADD_W - 5.f) * U, y + BROW_H * U / 2, info.c_str(), NULL);
			}
			{
				float px = rightX + rightW - QADD_W / 2.f;
				bool hot = hoverU.x >= px - 8.f && hoverU.x <= px + 8.f
				        && hoverU.y >= 35.f + r * BROW_PITCH
				        && hoverU.y <= 35.f + r * BROW_PITCH + BROW_H;
				float fl = (flashRow == -2 && flashCol == r) ? pressFlash : 0.f;
				drawGlyph(vg, px * U, y + BROW_H * U / 2, U, '+', false, hot, fl);
			}
		}
		drawScrollbar(vg, W - 7.f + 0.5f, ns, setScroll, vis);
		if (pressFlash > 0.f) pressFlash -= 0.10f;
	}

	// Small vector glyphs for the row buttons — one place, so they line up.
	static constexpr float QADD_W = 14.f;      // width of the + hit zone on a set row
	void drawGlyph(NVGcontext* vg, float cx, float cy, float U, char g,
	               bool disabled = false, bool hot = false, float flash = 0.f) {
		float a = 3.2f * U;
		NVGcolor col = disabled ? nvgRGBA(FTEXT_DIM.r * 255, FTEXT_DIM.g * 255,
		                                  FTEXT_DIM.b * 255, 90)
		             : hot ? FTEXT : FTEXT_DIM;
		if (flash > 0.f) col = nvgLerpRGBA(col, nvgRGB(0xFF, 0xFF, 0xFF), clamp(flash, 0.f, 1.f));
		nvgStrokeColor(vg, col);
		nvgStrokeWidth(vg, 0.9f);
		nvgBeginPath(vg);
		switch (g) {
			case '+': nvgMoveTo(vg, cx - a, cy); nvgLineTo(vg, cx + a, cy);
			          nvgMoveTo(vg, cx, cy - a); nvgLineTo(vg, cx, cy + a); break;
			case '-': nvgMoveTo(vg, cx - a, cy); nvgLineTo(vg, cx + a, cy); break;
			case 'x': nvgMoveTo(vg, cx - a, cy - a); nvgLineTo(vg, cx + a, cy + a);
			          nvgMoveTo(vg, cx + a, cy - a); nvgLineTo(vg, cx - a, cy + a); break;
			case 'u': nvgMoveTo(vg, cx - a, cy + a * 0.6f); nvgLineTo(vg, cx, cy - a * 0.6f);
			          nvgLineTo(vg, cx + a, cy + a * 0.6f); break;
			case 'd': nvgMoveTo(vg, cx - a, cy - a * 0.6f); nvgLineTo(vg, cx, cy + a * 0.6f);
			          nvgLineTo(vg, cx + a, cy - a * 0.6f); break;
			case 'p': nvgMoveTo(vg, cx - a * 0.7f, cy - a);      // play
			          nvgLineTo(vg, cx + a * 0.9f, cy);
			          nvgLineTo(vg, cx - a * 0.7f, cy + a); nvgClosePath(vg); break;
			case 'c': nvgRect(vg, cx - a, cy - a * 0.7f, a * 1.4f, a * 1.4f);   // duplicate
			          nvgRect(vg, cx - a * 0.35f, cy - a * 1.3f, a * 1.4f, a * 1.4f); break;
		}
		nvgStroke(vg);
	}

	// Queue row geometry, measured in from the right edge so it survives resizing.
	struct QCols { float play, minus, count, plus, up, down, dup, del, nameEnd; };
	QCols qcols(float W) {
		QCols c;
		c.play = 15.f;
		c.del = W - 16.f; c.dup = W - 34.f; c.down = W - 52.f; c.up = W - 70.f;
		c.plus = W - 90.f; c.count = W - 106.f; c.minus = W - 122.f;
		c.nameEnd = c.minus - 8.f;
		return c;
	}

	// col 0..6 = minus count plus up down dup del; 1 is the read-out, not a
	// button. -1 with a valid row means the row body. Shared by draw and click
	// so the hit zones and the highlight can never disagree.
	int queueHit(float xu, float yu, int& row) {
		QCols c = qcols(box.size.x / u());
		row = (int)((yu - 35.f) / BROW_PITCH);
		float inRow = yu - 35.f - row * BROW_PITCH;
		if (row < 0 || row >= visRows() || inRow > BROW_H) { row = -1; return -1; }
		const float cx[8] = {c.minus, c.count, c.plus, c.up, c.down, c.dup, c.del, c.play};
		for (int i = 0; i < 8; i++)
			if (i != 1 && xu >= cx[i] - 8.f && xu <= cx[i] + 8.f) return i;
		return -1;
	}

	void drawQueueTab(NVGcontext* vg) {
		if (!module) return;
		const float U = u(), W = box.size.x / U;
		Library& lib = module->lib;
		if (module->queue.empty()) {
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, FTEXT_DIM);
				nvgText(vg, box.size.x / 2, 62.f * U, "Queue is empty", NULL);
				nvgText(vg, box.size.x / 2, 76.f * U, "Press + on any set in the browser tabs", NULL);
			}
			return;
		}
		QCols c = qcols(W);
		int vis = visRows(), nq = (int)module->queue.size();
		queueScroll = clamp(queueScroll, 0, std::max(0, nq - vis));
		for (int r = 0; r < vis && queueScroll + r < nq; r++) {
			int i = queueScroll + r;
			auto& it = module->queue[i];
			float y = (35.f + r * BROW_PITCH) * U, mid = y + BROW_H * U / 2;
			bool isPlay = (i == module->queuePos);
			nvgBeginPath(vg); nvgRoundedRect(vg, 7.f * U, y, (W - 14.f) * U, BROW_H * U, 2.f * U);
			nvgFillColor(vg, isPlay ? FBLUE_DARK : FPURPLE); nvgFill(vg);
			if (isPlay) { nvgStrokeColor(vg, FORANGE); nvgStrokeWidth(vg, 1.f); nvgStroke(vg); }
			if (i == dragIdx) { nvgStrokeColor(vg, FTEXT); nvgStrokeWidth(vg, 1.f); nvgStroke(vg); }
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f * U);
				nvgFillColor(vg, isPlay ? FTEXT : FTEXT_DIM);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				std::string nm = (it.set >= 0 && it.set < (int)lib.sets.size()) ? lib.sets[it.set].name : "?";
				size_t lim = (size_t)std::max(6.f, (c.nameEnd - 28.f) / 5.2f);
				if (nm.size() > lim) nm = nm.substr(0, lim - 1) + "…";
				nvgText(vg, 26.f * U, mid, nm.c_str(), NULL);
				// repeats, with the progress through them on the playing entry
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, FTEXT);
				std::string cnt = isPlay && module->advanceMode == 0
				                ? string::f("%d/%d", module->queueDone + 1, it.reps)
				                : string::f("x%d", it.reps);
				nvgText(vg, c.count * U, mid, cnt.c_str(), NULL);
			}
			int hoverRow = -1, hoverCol = queueHit(hoverU.x, hoverU.y, hoverRow);
			auto G = [&](int col, float cx, char g, bool dis) {
				bool hot = (hoverRow == r && hoverCol == col);
				float fl = (flashRow == i && flashCol == col) ? pressFlash : 0.f;
				drawGlyph(vg, cx * U, mid, U, g, dis, hot, fl);
			};
			// play stays lit while its jump is pending, so the queued switch is visible
			{
				bool hot = (hoverRow == r && hoverCol == 7) || module->queueJumpTo == i;
				float fl = (flashRow == i && flashCol == 7) ? pressFlash : 0.f;
				drawGlyph(vg, c.play * U, mid, U, 'p', false, hot, fl);
			}
			G(0, c.minus, '-', it.reps <= 1);
			G(2, c.plus,  '+', false);
			G(3, c.up,    'u', i == 0);
			G(4, c.down,  'd', i == nq - 1);
			G(5, c.dup,   'c', false);
			G(6, c.del,   'x', false);
		}
		drawScrollbar(vg, W - 7.f + 0.5f, nq, queueScroll, vis);
		if (pressFlash > 0.f) pressFlash -= 0.10f;   // ~6 frames
	}

	void onHoverScroll(const event::HoverScroll& e) override {
		// Only swallow the wheel when there is genuinely something to scroll.
		// Consuming it unconditionally means two-finger scrolling the Rack canvas
		// dies wherever the pointer happens to cross this module, which is a much
		// bigger cost than the convenience of scrolling a list that already fits.
		if (module) {
			const float U = u();
			float xu = e.pos.x / U, yu = e.pos.y / U;
			int vis = visRows();
			int d = (e.scrollDelta.y > 0.f) ? -1 : 1;
			if (yu >= 35.f) {                                   // never over the tab row
				if (module->screenTab == FILL_NAXIS + 1) {
					if ((int)module->queue.size() > vis) {
						queueScroll += d; e.consume(this); return;
					}
				} else if (module->screenTab >= 1 && module->screenTab <= FILL_NAXIS
				           && !module->lib.sets.empty()) {
					Library& lib = module->lib;
					int axis = module->screenTab - 1;
					if (!lib.families[axis].empty()) {
						int nb = (int)lib.families[axis].size();
						int bank = clamp(module->browseBank[axis], 0, nb - 1);
						int ns = (int)lib.famSets[axis][bank].size();
						bool leftCol = xu < setColX();
						if ((leftCol ? nb : ns) > vis) {
							if (leftCol) bankScroll += d; else setScroll += d;
							e.consume(this); return;
						}
					}
				}
			}
		}
		Widget::onHoverScroll(e);
	}

	void draw(const DrawArgs& args) override {
		if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		int tab = module ? clamp(module->screenTab, 0, FILL_NAXIS + 1) : 0;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
		nvgFillColor(vg, FBG); nvgFill(vg);
		drawTabs(vg, tab);
		if (tab == 5) drawQueueTab(vg); else if (tab >= 1) drawBrowserTab(vg, tab - 1); else drawPatternTab(vg);
	}

	void onButton(const event::Button& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			const float U = u();
			float xu = e.pos.x / U, yu = e.pos.y / U;                 // click position in Note units
			if (yu < 32.f) {                                          // tab row
				for (int i = 0; i < 6; i++)
					if (xu >= 7.f + i * 52.f && xu <= 57.f + i * 52.f) { module->screenTab = i; e.consume(this); return; }
				return;
			}
			if (module->screenTab == FILL_NAXIS + 1) {                    // queue tab
				int row = -1, col = queueHit(xu, yu, row);
				int nq = (int)module->queue.size();
				int i = queueScroll + row;
				if (row >= 0 && i < nq) {
					auto& q = module->queue;
					switch (col) {
						case 6: q.erase(q.begin() + i);
						        if (module->queuePos >= (int)q.size()) module->queuePos = 0; break;
						case 5: q.insert(q.begin() + i + 1, q[i]); break;
						case 3: if (i > 0)      std::swap(q[i], q[i - 1]); break;
						case 4: if (i < nq - 1) std::swap(q[i], q[i + 1]); break;
						case 2: q[i].reps = std::min(64, q[i].reps + 1); break;
						case 0: q[i].reps = std::max(1, q[i].reps - 1); break;
						case 7: module->queueJumpTo = i; break;    // play: next cycle
						default: dragIdx = i; dragAccum = 0.f; break;   // body: reorder
					}
					if (col >= 0) { flashRow = i; flashCol = col; pressFlash = 1.f; }
					e.consume(this);
				}
				return;                       // a click on empty space is not ours
			}
			if (module->screenTab >= 1 && module->screenTab <= FILL_NAXIS && !module->lib.sets.empty()) {
				Library& lib = module->lib;
				int axis = module->screenTab - 1;
				if (lib.families[axis].empty()) { Widget::onButton(e); return; }
				float leftW = bankColW(), rightX = setColX();
				int vis = visRows();
				int row = (int)((yu - 35.f) / BROW_PITCH);
				float inRow = yu - 35.f - row * BROW_PITCH;
				if (row >= 0 && row < vis && inRow <= BROW_H) {
					if (xu >= 7.f && xu <= 7.f + leftW) {                       // bank column
						int b = bankScroll + row;
						if (b < (int)lib.families[axis].size()) { module->browseBank[axis] = b; setScroll = 0; e.consume(this); return; }
					} else if (xu >= rightX) {                                  // set column
						int bank = clamp(module->browseBank[axis], 0, (int)lib.families[axis].size() - 1);
						const std::vector<int>& ss = lib.famSets[axis][bank];
						int k = setScroll + row;
						if (k < (int)ss.size()) {
							float W = box.size.x / U;
							if (xu >= W - 7.f - QADD_W) {                          // + : queue it
								module->queueAdd(ss[k]);
								flashRow = -2; flashCol = row; pressFlash = 1.f;
							}
							else module->params[Fill::SET_PARAM].setValue((float)ss[k]);
							e.consume(this); return;
						}
					}
				}
			}
		}
		// right-click a set row (any browser tab) → toggle favorite
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT
		    && module->screenTab >= 1 && module->screenTab <= FILL_NAXIS && !module->lib.sets.empty()) {
			Library& lib = module->lib;
			int axis = module->screenTab - 1;
			if (!lib.families[axis].empty()) {
				const float U = u();
				float xu = e.pos.x / U, yu = e.pos.y / U;
				int vis = visRows();
				int row = (int)((yu - 35.f) / BROW_PITCH);
				float inRow = yu - 35.f - row * BROW_PITCH;
				if (row >= 0 && row < vis && inRow <= BROW_H && xu >= setColX()) {
					int bank = clamp(module->browseBank[axis], 0, (int)lib.families[axis].size() - 1);
					const std::vector<int>& ss = lib.famSets[axis][bank];
					int k = setScroll + row;
					if (k < (int)ss.size()) { module->toggleFav(lib.sets[ss[k]].id); e.consume(this); return; }
				}
			}
		}
		Widget::onButton(e);
	}
};

struct FillWidget : ModuleWidget {
	FillWidget(Fill* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/fill.svg")));

		// Panel is 32HP (162.56mm): free margin on the left (art / future controls),
		// display right of it, drum outputs in three columns at the right edge,
		// controls spread across the wider lower area.
		FillDisplay* disp = new FillDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(4.f, 11.f));
		disp->box.size = mm2px(Vec(104.f, 52.f));
		addChild(disp);

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.f, 74.f)), module, Fill::ACCUM_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(29.6f, 74.f)), module, Fill::DISCHARGE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(47.2f, 74.f)), module, Fill::TIER_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(64.8f, 74.f)), module, Fill::PHRASE_PARAM));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(mm2px(Vec(82.4f, 74.f)), module, Fill::SYNC_PARAM, Fill::SYNC_LIGHT));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(100.f, 74.f)), module, Fill::SET_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.f, 90.f)), module, Fill::ACCUM_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.6f, 90.f)), module, Fill::DISCHARGE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.2f, 90.f)), module, Fill::TIER_CV_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(64.8f, 90.f)), module, Fill::EXTRAS_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(82.4f, 90.f)), module, Fill::EXTRAS_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(100.f, 90.f)), module, Fill::SET_CV_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.f, 108.f)), module, Fill::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(28.f, 108.f)), module, Fill::BAR_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.f, 108.f)), module, Fill::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(60.f, 108.f)), module, Fill::RESEED_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.f, 108.f)), module, Fill::NEXT_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(44.f, 120.f)), module, Fill::RESET_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(86.f, 102.f)), module, Fill::FILL_LIGHT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(86.f, 108.f)), module, Fill::FILL_OUTPUT));

		const float gateX = 136.2f, velX = 147.2f, accX = 158.2f;
		for (int c = 0; c < FILL_NCH; c++) {
			float y = 14.f + c * 13.f;
			// per-channel swing trimpot in the left margin, row-aligned with the outputs
			addParam(createParamCentered<Trimpot>(mm2px(Vec(123.f, y)), module, Fill::SWING_PARAM + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(gateX, y)), module, Fill::GATE_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(velX, y)),  module, Fill::VEL_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(accX, y)),  module, Fill::ACC_OUTPUT + c));
		}
		// bottom-right row: time-signature NUM / DEN (→ Meter) + pattern-set CV
		const float yb = 14.f + FILL_NCH * 13.f;
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(gateX, yb)), module, Fill::NUM_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(velX, yb)),  module, Fill::DEN_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(accX, yb)),  module, Fill::SET_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(123.f, yb)), module, Fill::BPM_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Fill* module = dynamic_cast<Fill*>(this->module);
		if (!module) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Clock"));
		{
			static const int RES[] = {0, 1, 2, 3, 4, 6, 8, 12, 16, 24};
			std::vector<std::string> lbl = {"Measure from BAR"};
			for (int i = 1; i < 10; i++) lbl.push_back(string::f("%d clocks per beat", RES[i]));
			int sel = 0;
			for (int i = 0; i < 10; i++) if (RES[i] == module->clockRes) sel = i;
			menu->addChild(createIndexSubmenuItem("Clock resolution", lbl,
				[=]() { return sel; },
				[module](int i) { module->clockRes = RES[i]; }));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Play fills", "", &module->playFills));
		menu->addChild(createIndexSubmenuItem("Queue advance",
			{"By repeat count", "By NEXT input only"},
			[=]() { return module->advanceMode; },
			[module](int i) { module->advanceMode = clamp(i, 0, 1); }));
		menu->addChild(createMenuItem("Clear queue", "", [module]() {
			module->queue.clear(); module->queuePos = 0; module->queueDone = 0; }));
		menu->addChild(createMenuLabel("Intensity"));
		menu->addChild(createIndexPtrSubmenuItem("Tier follows",
			{"Rise through the phrase", "One tier per phrase"}, &module->tierMode));
		menu->addChild(createIndexPtrSubmenuItem("Tier balance",
			{"Even thirds", "Main-heavy (stays basic longer)", "Slow build"}, &module->tierBalance));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Outputs"));
		sfs::addPulseWidthMenu(menu, &module->pulseWidthIdx);
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Open user patterns folder", "", []() {
			std::string dir = asset::user("SignalFunctionSet/patterns");
			system::createDirectories(dir);
			system::openDirectory(dir);
		}));
		menu->addChild(createMenuItem("Import pattern from URL (clipboard)", "", [module]() {
			const char* clip = glfwGetClipboardString(APP->window->win);
			std::string url = clip ? clip : "";
			while (!url.empty() && std::isspace((unsigned char)url.back())) url.pop_back();
			while (!url.empty() && std::isspace((unsigned char)url.front())) url.erase(0, 1);
			if (url.compare(0, 4, "http") != 0) {
				osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Copy a drum-patterns.com pattern URL to the clipboard first, then use this menu item.");
				return;
			}
			std::string slug = url;
			while (!slug.empty() && slug.back() == '/') slug.pop_back();
			size_t sl = slug.rfind('/');
			if (sl != std::string::npos) slug = slug.substr(sl + 1);
			size_t q = slug.find('?');
			if (q != std::string::npos) slug = slug.substr(0, q);
			slug = dpSlug(slug);
			std::string tmp = asset::user("SignalFunctionSet/dp-import.tmp");
			system::createDirectories(asset::user("SignalFunctionSet"));
			if (!network::requestDownload(url, tmp, nullptr)) {
				osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Download failed — check the URL and your connection.");
				return;
			}
			std::string data;
			if (FILE* f = std::fopen(tmp.c_str(), "rb")) {
				char buf[4096]; size_t r;
				while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, r);
				std::fclose(f);
			}
			system::remove(tmp);
			std::string txt = (data.find("<code") != std::string::npos || data.find("<html") != std::string::npos)
			                ? dpHtmlToTxt(data, slug) : data;   // pages get scraped; a raw export .txt passes through
			if (txt.empty() || txt.find('\n') == std::string::npos) {
				osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "No drum pattern grid found at that URL.");
				return;
			}
			std::string dir = asset::user("SignalFunctionSet/patterns");
			system::createDirectories(dir);
			std::string dst = system::join(dir, slug + ".txt");
			if (FILE* f = std::fopen(dst.c_str(), "wb")) { std::fwrite(txt.data(), 1, txt.size(), f); std::fclose(f); }
			module->rescanLibrary();
			module->screenTab = 1 + AX_USER;   // show the result
		}));
	}

	// Drag-and-drop import: .txt (drum-patterns.com export) or .json (SFS library)
	// files dropped on the panel are copied into the user patterns folder and the
	// library is reloaded live.
	void onPathDrop(const event::PathDrop& e) override {
		Fill* module = dynamic_cast<Fill*>(this->module);
		if (module) {
			bool got = false;
			std::string dir = asset::user("SignalFunctionSet/patterns");
			for (const std::string& p : e.paths) {
				std::string ext = string::lowercase(system::getExtension(p));
				if (ext != ".txt" && ext != ".json") continue;
				system::createDirectories(dir);
				system::copy(p, system::join(dir, system::getFilename(p)));
				got = true;
			}
			if (got) {
				module->rescanLibrary();
				module->screenTab = 1 + AX_USER;
				e.consume(this);
				return;
			}
		}
		ModuleWidget::onPathDrop(e);
	}
};

Model* modelFill = createModel<Fill, FillWidget>("Fill");
