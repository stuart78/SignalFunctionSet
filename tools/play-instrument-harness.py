#!/usr/bin/env python3
"""Play — offline instrument harness.

Extracts Play's REAL parsing, sample loading and voice rendering out of
src/play.cpp (verbatim, by marker) into a standalone C++ program, then loads an
.sfz / .dspreset / .dsbundle and renders notes through it. It answers the one
question the module's display cannot: does a note-on on this instrument produce
audible samples, and if not, which stage dropped it.

Extracting rather than reimplementing is the point — a harness that models the
engine tells you about the model. If a marker below stops matching, the script
fails loudly rather than testing a stale copy.

    python3 tools/play-instrument-harness.py <file.sfz|.dspreset|.dsbundle> [...]
"""
import os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAY = os.path.join(ROOT, "src", "play.cpp")


def slice_between(src, start, end, what):
    i = src.find(start)
    if i < 0:
        sys.exit("harness: could not find %s (start marker %r) in play.cpp" % (what, start))
    j = src.find(end, i + len(start))
    if j < 0:
        sys.exit("harness: could not find end of %s (marker %r) in play.cpp" % (what, end))
    return src[i:j]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = open(PLAY).read()

    region_struct = slice_between(src, "struct SfzRegion {", "struct Instrument {", "SfzRegion")
    vel_gain      = slice_between(src, "static inline float playVelGain", "struct Instrument {", "playVelGain")
    instrument    = slice_between(src, "struct Instrument {", "// ─── SFZ parsing", "Instrument")
    parsers       = slice_between(src, "static void sfzStripComments", "// A .dsbundle is a folder holding", "parsers")
    decode        = slice_between(src, "static int16_t* playDecodeFile", "// ─── Module ───", "decoder + loader")
    voice_struct  = slice_between(src, "\tstruct Voice {", "\tVoice voices[PLAY_MAX_VOICES];", "Voice")
    note_on       = slice_between(src, "\tvoid noteOn(int chan, int note, int vel) {",
                                  "\tvoid process(const ProcessArgs& args)", "noteOn + resolveEnv")
    render        = slice_between(src, "\t\tfloat outL = 0.f, outR = 0.f;",
                                  "\t\tfloat lvl = params[LEVEL_PARAM].getValue();", "voice render")

    # SfzRegion carries only the struct; playVelGain sits between it and Instrument.
    region_struct = region_struct[:region_struct.find("// Velocity → gain")]

    shim = r"""
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

namespace rack {

template<typename T> static T clamp(T x, T a, T b) { return x < a ? a : (x > b ? b : x); }

namespace string {
static std::string lowercase(const std::string& s) {
    std::string r = s; for (char& c : r) c = tolower((unsigned char)c); return r;
}
}

namespace system {
static std::string getFilename(const std::string& p) { size_t i = p.rfind('/'); return i == std::string::npos ? p : p.substr(i + 1); }
static std::string getDirectory(const std::string& p) { size_t i = p.rfind('/'); return i == std::string::npos ? "" : p.substr(0, i); }
static std::string getStem(const std::string& p) { std::string f = getFilename(p); size_t i = f.rfind('.'); return i == std::string::npos ? f : f.substr(0, i); }
static std::string getExtension(const std::string& p) { std::string f = getFilename(p); size_t i = f.rfind('.'); return i == std::string::npos ? "" : f.substr(i); }
static std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }
static bool isDirectory(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static std::vector<std::string> getEntries(const std::string& d, int depth = 0) {
    (void)depth; std::vector<std::string> out; DIR* dp = ::opendir(d.c_str()); if (!dp) return out;
    while (struct dirent* e = ::readdir(dp)) { std::string n = e->d_name; if (n != "." && n != "..") out.push_back(join(d, n)); }
    ::closedir(dp); std::sort(out.begin(), out.end()); return out;
}
}

static const int PLAY_MAX_VOICES = 16;
static float HARNESS_SR = 48000.f;
struct EngineShim { float getSampleRate() { return HARNESS_SR; } };
struct AppShim { EngineShim e; EngineShim* engine = &e; };
static AppShim appShim;
#define APP (&appShim)
"""

    body = r"""
// ─── extracted verbatim from src/play.cpp ────────────────────────────────────
%s
%s
%s
%s
%s

struct PlayEngine {
%s
    Voice voices[PLAY_MAX_VOICES];
    std::vector<Instrument> instruments;
    int curInstrument = 0;
    int rrCounter = 0;
    bool oneShot = false;
    enum EnvMode { ENV_OFF, ENV_SFZ, ENV_DEFAULT };
    int envMode = ENV_OFF;

%s
    // one sample of the voice mixer, lifted out of process()
    void renderOne(float& L, float& R) {
%s
        L = outL; R = outR;
    }
};
} // namespace rack
using namespace rack;
""" % (region_struct, vel_gain, instrument, parsers, decode, voice_struct, note_on, render)

    main_cpp = r"""
static std::string findDspresetIn(const std::string& dir) {
    for (const std::string& e : system::getEntries(dir))
        if (string::lowercase(system::getExtension(e)) == ".dspreset") return e;
    for (const std::string& e : system::getEntries(dir))
        if (system::isDirectory(e)) { std::string p = findDspresetIn(e); if (!p.empty()) return p; }
    return "";
}

int main(int argc, char** argv) {
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        std::string path = argv[a], preset = path;
        if (system::isDirectory(path)) preset = findDspresetIn(path);
        std::string ext = string::lowercase(system::getExtension(preset));
        PlayEngine eng;
        Instrument in;
        bool ok = !preset.empty() && ((ext == ".dspreset") ? parseDecentSampler(preset, in) : parseSfz(preset, in));
        printf("\n=== %s\n", path.c_str());
        printf("    preset  : %s\n", preset.empty() ? "(none found)" : system::getFilename(preset).c_str());
        if (!ok) { printf("    PARSE FAILED\n"); bad++; continue; }
        printf("    regions : %d\n", (int)in.regions.size());

        int failed = 0;
        for (auto& r : in.regions) { loadRegionAudio(r, in.dir); if (!r.loaded) failed++; }
        printf("    loaded  : %d / %d%s\n", (int)in.regions.size() - failed, (int)in.regions.size(),
               failed ? "   <<< SOME SAMPLES DID NOT LOAD" : "");
        if (failed) {
            int shown = 0;
            for (auto& r : in.regions) if (!r.loaded && shown++ < 4)
                printf("              missing: %s\n", system::join(in.dir, r.sample).c_str());
        }

        // key coverage
        bool mapped[128] = {false};
        for (auto& r : in.regions) if (r.loaded)
            for (int k = std::max(0, r.lokey); k <= std::min(127, r.hikey); k++) mapped[k] = true;
        int lo = -1, hi = -1, n = 0;
        for (int k = 0; k < 128; k++) if (mapped[k]) { if (lo < 0) lo = k; hi = k; n++; }
        printf("    keys    : %d mapped (%d..%d)\n", n, lo, hi);

        eng.instruments.push_back(std::move(in));
        eng.curInstrument = 0;

        // Play a note in the middle of the mapped range and measure the output.
        int note = (lo >= 0) ? (lo + hi) / 2 : 60;
        int silent = 0, played = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (auto& v : eng.voices) v.active = false;
            eng.envMode = pass;                       // Off, then SFZ envelopes
            eng.noteOn(0, note, 100);
            bool any = false; for (auto& v : eng.voices) if (v.active) any = true;
            float peak = 0.f;
            for (int i = 0; i < (int)(HARNESS_SR * 0.5f); i++) {
                float L = 0, R = 0; eng.renderOne(L, R);
                peak = std::max(peak, std::max(std::fabs(L), std::fabs(R)));
            }
            printf("    note %3d: env=%-7s voice=%s peak=%.4f%s\n", note,
                   pass == 0 ? "Off" : "SFZ", any ? "yes" : "NO ", peak,
                   (!any || peak < 1e-4f) ? "   <<< SILENT" : "");
            if (!any || peak < 1e-4f) silent++; else played++;
        }
        if (!played) bad++;
        (void)silent;
    }
    printf("\n%s\n", bad ? "FAIL — one or more instruments produced no audio" : "OK — every instrument sounded");
    return bad ? 1 : 0;
}
"""

    tmp = tempfile.mkdtemp(prefix="play-harness-")
    cpp = os.path.join(tmp, "harness.cpp")
    open(cpp, "w").write(shim + body + main_cpp)
    exe = os.path.join(tmp, "harness")
    cmd = ["c++", "-std=c++11", "-O2", "-I", os.path.join(ROOT, "src"), "-o", exe, cpp]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-6000:])
        sys.exit("harness: build failed (see above) — source: %s" % cpp)
    sys.exit(subprocess.run([exe] + sys.argv[1:]).returncode)


if __name__ == "__main__":
    main()
