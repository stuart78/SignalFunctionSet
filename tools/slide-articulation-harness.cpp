// =============================================================================
//  slide-articulation-harness.cpp
//
//  Offline harness for the Slide articulation model — src/slide-articulator.hpp
//  and src/slide-styles.hpp. It INCLUDES those headers directly rather than
//  carrying a copy, so it can never pass against a stale duplicate. The headers
//  are free-standing C++17 with no VCV dependency, which is what makes this
//  possible; keep them that way.
//
//     clang++ -std=c++17 -O2 -o /tmp/slide-harness tools/slide-articulation-harness.cpp
//     /tmp/slide-harness
//
//  WHAT SAF MEANS HERE
//  -------------------
//  Reverse-derived from the original result set and checked against every row:
//      SAF    = (SLIDE + HAMMER) / non-rest notes
//      legato = (SLIDE + HAMMER + PEDAL) / non-rest notes
//  SHIFT is excluded from SAF because a shift slide re-attacks on arrival — the
//  note is not "arrived at without a fresh pluck", which is the definition.
//
//  WHAT THIS HARNESS IS FOR
//  ------------------------
//  The style table's legato affinities were CALIBRATED to hit a target SAF band
//  per tradition (each preset says so: "calibrated legato trim +N"). So a run
//  that reproduces those bands proves nothing — it is the fit target. The
//  quantities worth measuring are the ones that were never fitted:
//
//    * the tempo/legato correlation             (section 2)
//    * the slide interval distribution          (section 6)
//    * whether the style ORDERING is stable     (section 1)
//    * whether the morph knob is continuous     (section 3)
//
//  And section 1's absolute numbers depend heavily on the test melody, which is
//  why it reports three of them rather than one. The ordering is what a MORPH
//  knob actually needs; the absolute SAF is not reproducible without pinning the
//  melody, and pinning the melody is a choice about the test, not about the model.
// =============================================================================

#include "../src/slide-styles.hpp"

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>

using namespace sfs::slide;

// ------------------------------------------------------------------ melody ---
// A random walk over a scale. Mostly stepwise, occasionally a third, so the
// interval content is the scale's own — which section 6 shows is the single
// biggest lever on the slide-width distribution.
struct MelodySpec {
    const char*      name;
    std::vector<int> scale;    // semitones from the root, one period
    int              lo, hi;   // degree range of the walk
};

static const MelodySpec MEL_PENT     = {"minor pent + b5", {0,3,5,6,7,10},            -7, 14};
static const MelodySpec MEL_DIATONIC = {"diatonic",        {0,2,4,5,7,9,11},          -8, 16};
static const MelodySpec MEL_CHROM    = {"chromatic",       {0,1,2,3,4,5,6,7,8,9,10,11}, -12, 24};

struct Melody {
    const MelodySpec& sp;
    Rng rng;
    int deg = 0;
    Melody(const MelodySpec& s, uint32_t seed) : sp(s), rng(seed) {}
    float degToSemi(int d) const {
        int n = (int)sp.scale.size();
        int oct = (int)std::floor((double)d / n);
        return (float)(sp.scale[d - oct * n] + 12 * oct);
    }
    float next() {
        float r = rng.uniform();
        int step = (r < 0.40f) ? 1 : (r < 0.80f ? -1 : (r < 0.90f ? 2 : (r < 0.97f ? -2 : 3)));
        deg += step;
        if (deg < sp.lo) deg = sp.lo + 2;
        if (deg > sp.hi) deg = sp.hi - 2;
        return degToSemi(deg);
    }
};

// ------------------------------------------------------------------- stats ---
struct Stats {
    long n = 0, count[ART_COUNT] = {0};
    double sumAbsInt = 0; long nInt = 0, nAsc = 0;
    double sumGlide = 0;  long nGlide = 0;
    long slideInt[16] = {0}; long nSlideInt = 0;
    long slideLegal = 0, stringCross = 0;
    double sumFret = 0;
    bool  usedString[16] = {false};

    double pct(int a) const { return n ? 100.0 * count[a] / n : 0; }
    double nonRest() const  { return (double)(n - count[ART_REST]); }
    double saf() const {
        double d = nonRest();
        return d > 0 ? 100.0 * (count[ART_SLIDE] + count[ART_HAMMER]) / d : 0;
    }
    double legato() const {
        double d = nonRest();
        return d > 0 ? 100.0 * (count[ART_SLIDE] + count[ART_HAMMER] + count[ART_PEDAL]) / d : 0;
    }
    double meanInt()   const { return nInt   ? sumAbsInt / nInt : 0; }
    double ascPct()    const { return nInt   ? 100.0 * nAsc / nInt : 0; }
    double meanGlide() const { return nGlide ? 1000.0 * sumGlide / nGlide : 0; }
    double meanFret()  const { return n ? sumFret / n : 0; }
    int    nStrings()  const { int c = 0; for (int i = 0; i < 16; i++) if (usedString[i]) c++; return c; }
};

struct Cfg {
    const Style* a = nullptr;
    const Style* b = nullptr;
    float morph = 0.f, temp = 1.f, bias = 0.f, bpm = 100.f;
    long  notes = 4000;
    const MelodySpec* mel = &MEL_PENT;
    uint32_t seed = 12345u;
    float beatOverride = -1.f;   // >=0 forces beatStrength on every note
};

static Stats run(const Cfg& c) {
    Articulator art;
    art.setStyles(c.a, c.b ? c.b : c.a);
    art.setMorph(c.morph);
    art.setTemperature(c.temp);
    art.setSlideBias(c.bias);
    art.setSeed(c.seed);
    art.reset();

    Melody mel(*c.mel, c.seed ^ 0xA5A5A5A5u);
    Rng rr(c.seed ^ 0x5A5A5A5Au);

    const float secPer16 = 60.f / c.bpm / 4.f;
    Stats st;
    int gridPos = 0, notesInPhrase = 0;
    int phraseLen = 6 + (int)(rr.uniform() * 6);

    // Pitch and duration are drawn one note AHEAD, because NoteRequest::ioiSec is
    // documented as the time to the NEXT note. That is lookahead: a live patch
    // cannot know it, and `tight` — the feature that carries the whole
    // tempo/legato relationship — is computed from it. Noted here, not fixed.
    int   durNext   = 1 + (int)(rr.uniform() * 4);
    float pitchNext = mel.next();

    for (long i = 0; i < c.notes; ++i) {
        const float pitch = pitchNext;
        const int   dur   = durNext;
        durNext = 1 + (int)(rr.uniform() * 4);
        if (rr.uniform() < 0.10f) durNext = 8;         // an occasional long note
        pitchNext = mel.next();

        NoteRequest req;
        req.pitchVolts = pitch / 12.f;
        req.rootVolts  = 0.f;
        req.ioiSec     = dur * secPer16;
        const int m16  = gridPos % 16;
        const float bs = (m16 == 0) ? 1.00f
                       : (m16 % 4 == 0) ? 0.70f
                       : (m16 % 2 == 0) ? 0.45f : 0.20f;
        req.beatStrength = (c.beatOverride >= 0.f) ? c.beatOverride : bs;
        req.phraseStart  = (notesInPhrase == 0);
        req.phraseEnd    = (notesInPhrase == phraseLen - 1);
        req.accent       = 0.5f;

        Decision d = art.decide(req);

        // Let the articulator's own clock run, so sinceAttack — and therefore
        // decay pressure — is real rather than assumed.
        for (float t = 0.f; t < req.ioiSec; t += 0.001f) art.process(0.001f);

        st.n++;
        st.count[d.artic]++;
        st.sumFret += d.fret;
        if (d.string >= 0 && d.string < 16) st.usedString[d.string] = true;
        if (d.slidePossible) st.slideLegal++;
        if (d.string != d.prevString) st.stringCross++;
        if (std::fabs(d.intervalSt) > 0.01f) {
            st.nInt++; st.sumAbsInt += std::fabs(d.intervalSt);
            if (d.intervalSt > 0) st.nAsc++;
        }
        if (d.glideSec > 0.f && (d.artic == ART_SLIDE || d.artic == ART_SHIFT)) {
            st.sumGlide += d.glideSec; st.nGlide++;
        }
        if (d.artic == ART_SLIDE) {
            int k = (int)std::lround(std::fabs(d.intervalSt));
            if (k >= 1 && k < 16) { st.slideInt[k]++; st.nSlideInt++; }
        }

        gridPos += dur;
        if (++notesInPhrase >= phraseLen) {
            notesInPhrase = 0; phraseLen = 6 + (int)(rr.uniform() * 6);
        }
    }
    return st;
}

// Mean SAF over several seeds, so a step can be told from sampling noise.
static void safStats(const Cfg& base, const uint32_t* seeds, int ns, double& mu, double& sd) {
    std::vector<double> v(ns);
    mu = 0;
    for (int i = 0; i < ns; i++) { Cfg c = base; c.seed = seeds[i]; v[i] = run(c).saf(); mu += v[i] / ns; }
    sd = 0;
    for (int i = 0; i < ns; i++) sd += (v[i] - mu) * (v[i] - mu) / std::max(ns - 1, 1);
    sd = std::sqrt(sd);
}

static double pearson(const double* x, const double* y, int n) {
    double mx = 0, my = 0;
    for (int i = 0; i < n; i++) { mx += x[i] / n; my += y[i] / n; }
    double sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < n; i++) {
        sxy += (x[i]-mx)*(y[i]-my); sxx += (x[i]-mx)*(x[i]-mx); syy += (y[i]-my)*(y[i]-my);
    }
    return (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0;
}

static void ranks(const double* v, int n, double* out) {
    for (int i = 0; i < n; i++) {
        double r = 1;
        for (int j = 0; j < n; j++) if (v[j] < v[i]) r += 1;
        out[i] = r;
    }
}

// In StyleId order, so the printed table reads in the same sequence a MORPH
// knob traverses. The "band" is the calibration target, not a prediction.
struct Ref { int id; const char* name; float bpm; const char* band; };
static const Ref REFS[] = {
    {STYLE_ELMORE,   "Elmore Boogie",      150, "10-20"},
    {STYLE_DOBRO,    "Dobro Bluegrass",    140, "15-25"},
    {STYLE_DELTA,    "Delta Bottleneck",   105, "30-45"},
    {STYLE_CHICAGO,  "Chicago Slow",        72, "45-60"},
    {STYLE_HAWAIIAN, "Hawaiian Kika Kila",  75, "55-75"},
    {STYLE_SACRED,   "Sacred Steel",        68, "60-75"},
    {STYLE_TRUCKS,   "Trucks Vocal",        92, "65-80"},
    {STYLE_MEEND,    "Hindustani Meend",    45, "75-92"},
    {STYLE_PEDAL,    "Pedal Steel Ballad",  72, "60-75*"},
    {STYLE_AMBIENT,  "Ambient Steel",       40, "85-98"},
};
static const int NREF = (int)(sizeof(REFS) / sizeof(REFS[0]));

static void hdr(const char* s) {
    printf("\n================================================================================\n %s\n"
           "================================================================================\n", s);
}

int main() {
    // -------------------------------------------------------------- 1 --------
    hdr("1. STYLE PROFILES under THREE melodies, each style at its own tempo.\n"
        "    The bands are the fit target, not a prediction. What matters is\n"
        "    whether the ORDERING survives a change of source material, because\n"
        "    that is what a MORPH knob rides on.");
    {
        const MelodySpec* mels[3] = {&MEL_PENT, &MEL_DIATONIC, &MEL_CHROM};
        double saf[3][NREF];
        for (int m = 0; m < 3; m++) {
            printf("\n  melody: %s\n", mels[m]->name);
            printf("  %-20s %7s %7s   band     PLUCK SLIDE SHIFT  HAM  PED REST  |int| glide\n",
                   "style", "SAF", "legato");
            for (int i = 0; i < NREF; i++) {
                Cfg c; c.a = &getStyle(REFS[i].id); c.bpm = REFS[i].bpm; c.mel = mels[m];
                Stats s = run(c);
                saf[m][i] = s.saf();
                const bool in = (s.legato() >= atof(REFS[i].band) &&
                                 s.legato() <= atof(strchr(REFS[i].band, '-') + 1));
                printf("  %-20s %6.1f%% %6.1f%%  %-7s %s %5.1f %5.1f %5.1f %4.1f %4.1f %4.1f  %4.2f %4.0fms\n",
                       REFS[i].name, s.saf(), s.legato(), REFS[i].band, in ? "in " : "OUT",
                       s.pct(ART_PLUCK), s.pct(ART_SLIDE), s.pct(ART_SHIFT),
                       s.pct(ART_HAMMER), s.pct(ART_PEDAL), s.pct(ART_REST),
                       s.meanInt(), s.meanGlide());
            }
        }
        double r0[NREF], r1[NREF], r2[NREF];
        ranks(saf[0], NREF, r0); ranks(saf[1], NREF, r1); ranks(saf[2], NREF, r2);
        printf("\n  Spearman rank correlation of the style ordering between melodies:\n");
        printf("    pentatonic vs diatonic   %+.3f\n", pearson(r0, r1, NREF));
        printf("    pentatonic vs chromatic  %+.3f\n", pearson(r0, r2, NREF));
        printf("    diatonic   vs chromatic  %+.3f\n", pearson(r1, r2, NREF));
        printf("    (absolute SAF is melody-dependent; the question is whether the\n"
               "     ordering a MORPH knob traverses stays put. 1.000 = identical.)\n");
    }

    // -------------------------------------------------------------- 2 --------
    hdr("2. TEMPO -> LEGATO, measured twice.  Literature reports r ~= -0.75.\n"
        "    WIDE  = 40..220 BPM for every style, as the original harness ran it.\n"
        "    IN-DIST = 0.7x..1.4x that style's own reference tempo. A tradition\n"
        "    tested where it never occurs is out of distribution, and the decay\n"
        "    term correctly forbids slides there.");
    {
        const float wide[] = {40, 60, 80, 100, 130, 170, 220};
        printf("  %-20s %7s  %-38s %8s %7s\n",
               "style", "r WIDE", "in-distribution sweep", "r IN-DIST", "spread");
        for (int i = 0; i < NREF; i++) {
            Cfg c; c.a = &getStyle(REFS[i].id);
            double x[7], y[7];
            for (int b = 0; b < 7; b++) { c.bpm = wide[b]; x[b] = wide[b]; y[b] = run(c).saf(); }
            const double rw = pearson(x, y, 7);

            double xi[5], yi[5];
            char buf[256] = {0}; int off = 0;
            for (int b = 0; b < 5; b++) {
                float f = 0.7f + 0.175f * b;                 // 0.70 .. 1.40
                c.bpm = REFS[i].bpm * f;
                xi[b] = c.bpm; yi[b] = run(c).saf();
                off += snprintf(buf + off, sizeof(buf) - off, "%3.0f:%4.1f%% ", xi[b], yi[b]);
            }
            // r alone is misleading: a +0.83 over a 1.5-point spread is a
            // correlation fitted to sampling noise. Report the effect size
            // beside it, and read the two together or not at all.
            double ylo = yi[0], yhi = yi[0];
            for (int b = 1; b < 5; b++) { ylo = std::min(ylo, yi[b]); yhi = std::max(yhi, yi[b]); }
            printf("  %-20s %+6.2f  %-38s %+7.2f %6.1f%s\n",
                   REFS[i].name, rw, buf, pearson(xi, yi, 5), yhi - ylo,
                   (yhi - ylo) < 5.0 ? "  (flat)" : "");
        }
    }

    // -------------------------------------------------------------- 3 --------
    hdr("3. MORPH CONTINUITY.  tuning() and pedalCapable() hard-switch at 0.5,\n"
        "    while everything else blends. SAF alone cannot show this (the softmax\n"
        "    is steepest mid-morph anyway, so the control pair steps too) — so\n"
        "    measure the FRETBOARD, which depends on the tuning and nothing else.");
    {
        struct P { int a, b; const char* n; };
        const P ps[] = {
            {STYLE_DELTA,  STYLE_PEDAL,  "Delta -> Pedal Steel   Open G (6 str, 22 fr) -> E9 (10 str, 24 fr)"},
            {STYLE_ELMORE, STYLE_TRUCKS, "Elmore -> Trucks       Open D -> Open D            [CONTROL]"},
        };
        const uint32_t seeds[] = {11, 22, 33, 44};
        for (int p = 0; p < 2; p++) {
            printf("\n  %s\n    morph   meanFret  slideLegal  stringCross  #strings   SAF+-sd\n", ps[p].n);
            for (float m = 0.44f; m <= 0.5605f; m += 0.02f) {
                double f = 0, l = 0, x = 0, ns = 0;
                for (int s = 0; s < 4; s++) {
                    Cfg c; c.a = &getStyle(ps[p].a); c.b = &getStyle(ps[p].b);
                    c.morph = m; c.bpm = 90.f; c.seed = seeds[s];
                    Stats st = run(c);
                    f += st.meanFret() / 4; l += 100.0 * st.slideLegal / st.n / 4;
                    x += 100.0 * st.stringCross / st.n / 4; ns += st.nStrings() / 4.0;
                }
                Cfg base; base.a = &getStyle(ps[p].a); base.b = &getStyle(ps[p].b);
                base.morph = m; base.bpm = 90.f;
                double mu, sd; safStats(base, seeds, 4, mu, sd);
                printf("    %5.3f %s %8.2f %10.1f%% %11.1f%% %8.2f  %5.1f+-%.2f\n",
                       m, (std::fabs(m - 0.5f) < 0.005f ? "<<" : "  "), f, l, x, ns, mu, sd);
            }
        }
    }

    // -------------------------------------------------------------- 4 --------
    hdr("4. TEMPERATURE (Delta, 105 BPM). Does T act on commitment, or is it a\n"
        "    hedge-and-drop-out knob? Watch PLUCK against SHIFT and REST.");
    {
        const float Ts[] = {0.10f, 0.35f, 0.70f, 1.00f, 1.60f, 2.50f, 4.00f};
        printf("  %-6s %7s %7s %7s %7s %7s %7s | %6s\n",
               "T", "PLUCK", "SLIDE", "SHIFT", "HAM", "PED", "REST", "SAF");
        for (int i = 0; i < 7; i++) {
            Cfg c; c.a = &getStyle(STYLE_DELTA); c.bpm = 105.f; c.temp = Ts[i];
            Stats s = run(c);
            printf("  T=%-4.2f %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% | %5.1f%%\n",
                   Ts[i], s.pct(ART_PLUCK), s.pct(ART_SLIDE), s.pct(ART_SHIFT),
                   s.pct(ART_HAMMER), s.pct(ART_PEDAL), s.pct(ART_REST), s.saf());
        }
    }

    // -------------------------------------------------------------- 5 --------
    hdr("5. SLIDE BIAS (Delta, 105 BPM). The user's thumb. It must move the\n"
        "    CHOICE without moving what is physically LEGAL.");
    {
        for (float b = -3.f; b <= 3.01f; b += 1.f) {
            Cfg c; c.a = &getStyle(STYLE_DELTA); c.bpm = 105.f; c.bias = b;
            Stats s = run(c);
            printf("  bias %+4.1f   SAF %5.1f%%   (slide legal on %4.1f%% of notes)\n",
                   b, s.saf(), 100.0 * s.slideLegal / s.n);
        }
    }

    // -------------------------------------------------------------- 6 --------
    hdr("6. SLIDE INTERVAL DISTRIBUTION (Delta). Research: 1-2 st = 85-90%, n=21.\n"
        "    Never fitted, so this is real out-of-sample evidence.");
    {
        const MelodySpec* mels[3] = {&MEL_PENT, &MEL_DIATONIC, &MEL_CHROM};
        for (int q = 0; q < 3; q++) {
            Cfg c; c.a = &getStyle(STYLE_DELTA); c.bpm = 105.f; c.mel = mels[q];
            Stats s = run(c);
            double c12 = 0, c13 = 0;
            printf("\n  %-18s (mean |int| over all notes %.2f st)\n", mels[q]->name, s.meanInt());
            for (int k = 1; k <= 7; k++) {
                double pc = s.nSlideInt ? 100.0 * s.slideInt[k] / s.nSlideInt : 0;
                if (k <= 2) c12 += pc;
                if (k <= 3) c13 += pc;
                printf("   %2d st %5.1f%%  ", k, pc);
                for (int z = 0; z < (int)(pc / 2); z++) printf("#");
                printf("\n");
            }
            printf("   --> 1-2 st = %.1f%%   1-3 st = %.1f%%\n", c12, c13);
        }
    }

    // -------------------------------------------------------------- 7 --------
    hdr("7. ACCENT AS RE-PLUCK, routed through beatStrength.\n"
        "    wBeatStrong[PLUCK] = 1.10 * gridness, so an accent's authority is\n"
        "    scaled by how much the tradition cares about a grid. Check whether\n"
        "    it survives in the styles where you would most want it.");
    {
        printf("  %-20s %7s %7s %7s %7s %7s   authority\n",
               "style", "bs=0", "bs=.25", "bs=.5", "bs=.75", "bs=1");
        for (int i = 0; i < NREF; i++) {
            double v[5];
            const float bss[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
            printf("  %-20s", REFS[i].name);
            for (int b = 0; b < 5; b++) {
                Cfg c; c.a = &getStyle(REFS[i].id); c.bpm = REFS[i].bpm; c.beatOverride = bss[b];
                v[b] = run(c).saf();
                printf("%6.1f%%", v[b]);
            }
            printf("  %6.1f pts %s\n", v[0] - v[4], (v[0] - v[4]) < 2.0 ? " <-- inert" : "");
        }
    }
    printf("\n");
    return 0;
}
