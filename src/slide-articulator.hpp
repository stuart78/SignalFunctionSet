// =============================================================================
//  SlideArticulator.hpp
//  A context-conditioned Markov model of slide-guitar articulation.
//
//  Signal Function Set — drop-in core. Header-only, C++17, no dependencies
//  (no VCV, no STL containers in the hot path, no allocation after construction).
//
//  WHAT THIS DOES
//  --------------
//  Given a stream of target pitches with rhythmic context, it decides for each
//  note whether the player SLIDES into it (no new attack, pitch glides) or
//  PLUCKS it (fresh attack at pitch) — plus four other articulations that real
//  slide players actually use. It then renders the pitch trajectory.
//
//  THE MODEL, IN ONE PARAGRAPH
//  ---------------------------
//  A first-order Markov chain over articulation states supplies *phrasing
//  inertia* (legato runs beget legato runs). That prior is combined in the LOG
//  domain with a set of context features — string change, interval size and
//  direction, beat strength, phrase position, decay pressure, scale degree,
//  and whether the slide physically fits in the available time. The result is
//  a conditional Markov model (MEMM), sampled with a temperature control.
//
//  Three things fall out of this rather than being hardcoded:
//    * Slide rate drops as tempo rises (the slide stops fitting in the IOI).
//    * Fast/percussive genres emerge from a *tuning geometry* preference, not
//      from a "be plucky" knob.
//    * Notes get re-attacked when the previous one has decayed past usefulness.
//
//  STYLE MORPHING
//  --------------
//  Two style presets are blended GEOMETRICALLY (log-linear), not linearly.
//  Linear interpolation of two stochastic matrices provably *raises* entropy
//  (Shannon entropy is concave) — at morph=0.5 you get a player less committed
//  than either parent, and you manufacture transitions present in neither.
//  Geometric interpolation intersects supports and lowers entropy. It is also
//  what IDyOM uses to combine models. Because everything lives in log space,
//  morph and temperature share a single code path.
//
//  OUTPUT CONTRACT (Eurorack / VCV)
//  --------------------------------
//    V/OCT      — pitch with the trajectory + vibrato baked in
//    TRIG       — fires ONLY on a fresh attack (drive your exciter/strike here)
//    GATE       — sustains across slides, retriggers on plucks
//    SLIDE CV   — 0..1 bump while a glide is in flight (patch to friction noise,
//                 filter, or a VCA on a noise source — the "smear")
//    PEDAL V/OCT— a second pitch that only moves on PEDAL events, so a second
//                 voice can be held in true oblique motion (the pedal-steel trick)
//    VEL        — 0..1, accent/dynamics
//
//  Author's notes on provenance are in RESEARCH.md. Numbers marked [est] in the
//  style tables are calibrated estimates, not measurements — tune to taste.
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace sfs {
namespace slide {

// -----------------------------------------------------------------------------
// Articulation alphabet
// -----------------------------------------------------------------------------
// The six things a slide player can do to get from one note to the next.
enum Artic : int {
    ART_PLUCK  = 0,  // fresh attack at the target pitch. No glide. Marks the grid.
    ART_SLIDE  = 1,  // legato slide: no new attack, pitch glides. (Hal Leonard
                     // "legato slide": second note is NOT struck.)
    ART_SHIFT  = 2,  // slide AND re-attack on arrival ("shift slide"). The fix for
                     // a note that has decayed too far to survive the glide —
                     // idiomatically common on DESCENDING moves.
    ART_HAMMER = 3,  // bar hammer-on / pull-off: no attack, near-instant move.
                     // On a bar instrument this only works against an open string.
    ART_PEDAL  = 4,  // pitch change with NO attack and NO bar move (pedal/knee
                     // lever). ±1 or ±2 semitones. This is the structural fact
                     // that makes pedal steel simultaneously fast and legato.
    ART_REST   = 5,  // silence.
    ART_COUNT  = 6
};

// Typical values of the continuous context features across ordinary playing.
// Used to centre those features so that a style's affinity numbers mean what
// they look like they mean. Tune these only if you change the feature defs.
static constexpr float kBeatMean  = 0.45f;
static constexpr float kDecayMean = 0.40f;
static constexpr float kTightMean = 0.35f;
static constexpr float kWideMean  = 0.35f;
static constexpr float kBlueMean  = 0.35f;

inline const char* articName(int a) {
    static const char* n[] = {"PLUCK","SLIDE","SHIFT","HAMMER","PEDAL","REST"};
    return (a >= 0 && a < ART_COUNT) ? n[a] : "?";
}

// True if this articulation produces a fresh right-hand attack.
inline bool articAttacks(int a) { return a == ART_PLUCK || a == ART_SHIFT; }
// True if this articulation moves pitch continuously over time.
inline bool articGlides(int a)  { return a == ART_SLIDE || a == ART_SHIFT; }

// -----------------------------------------------------------------------------
// Tuning geometry
// -----------------------------------------------------------------------------
// The single hardest constraint in slide playing: a slide only connects two
// notes on the SAME STRING. Crossing strings forces a pluck. Which melodic
// intervals live on one string is decided entirely by the tuning — so the
// tuning is not a cosmetic choice, it is a specification of which notes
// require a slide.
struct Tuning {
    const char* name;
    int   numStrings;
    int   offset[12];   // semitones above the tuning's root, low string first
    int   maxFret;
    // Mean of the inter-string intervals; low values (dobro, C6, E9) mean more
    // melody is reachable by crossing strings, hence a pluckier idiom.
    float meanStringGap;
};

// Root-relative offsets. Index 0 = lowest string.
// Open G   D G D G B D  ->  7 0 7 0 4 7   (str gaps 5,7,5,4,3)
// Open D   D A D F# A D ->  0 7 0 4 7 0   (str gaps 7,5,4,3,5)
// Dobro G  G B D G B D  ->  0 4 7 0 4 7   (str gaps 4,3,5,4,3)  <- close-voiced
// C6       C E G A C E  ->  0 4 7 9 0 4   (str gaps 4,3,2,3,4)  <- has a WHOLE
//                                            STEP between strings: stepwise
//                                            melody without moving the bar.
// E9 (10)  B D E F# G# B E G# D# F#
//                        ->  7 10 0 2 4 7 0 4 11 2
static const Tuning TUNING_OPEN_G  = {"Open G (DGDGBD)",   6, {7,0,7,0,4,7},            22, 4.8f};
static const Tuning TUNING_OPEN_D  = {"Open D/E (DADF#AD)",6, {0,7,0,4,7,0},            22, 4.8f};
static const Tuning TUNING_DOBRO_G = {"Dobro G (GBDGBD)",  6, {0,4,7,0,4,7},            22, 3.8f};
static const Tuning TUNING_C6      = {"C6 lap (CEGACE)",   6, {0,4,7,9,0,4},            22, 3.2f};
static const Tuning TUNING_E9      = {"E9 pedal steel",   10, {7,10,0,2,4,7,0,4,11,2},  24, 4.0f};

// -----------------------------------------------------------------------------
// Style parameters
// -----------------------------------------------------------------------------
// Everything the model needs to sound like one tradition. All probabilistic
// quantities are stored as LOGITS so that morph and temperature are the same
// operation (add in log space, divide by T, softmax).
struct Style {
    const char* name;
    const Tuning* tuning;

    // --- Markov prior: logit of P(next artic | previous artic). -------------
    // Supplies phrasing inertia. Rows do not need normalising; softmax handles it.
    float trans[ART_COUNT][ART_COUNT];

    // --- Context feature weights, in logits, per articulation. --------------
    // Each is multiplied by a feature value in roughly [0,1] (or [-1,1] where
    // noted) and added to that articulation's logit.
    float wBeatStrong[ART_COUNT];  // x beatStrength (1 = downbeat)
    float wPhraseStart[ART_COUNT]; // x 1 if first note of a phrase
    float wPhraseEnd[ART_COUNT];   // x 1 if last note of a phrase
    float wDecay[ART_COUNT];       // x decayPressure (1 = note has died)
    float wBlue[ART_COUNT];        // x 1 if target is b3/b5/b7
    float wAscend[ART_COUNT];      // x +1 ascending, -1 descending
    float wWideInt[ART_COUNT];     // x wideness (0 at 1 semitone, 1 past ~7)
    float wTight[ART_COUNT];       // x tightness (1 when the slide barely fits)

    // --- Fretboard / hand behaviour -----------------------------------------
    float sameStringBias;   // cost of leaving the current string. HIGH = single-
                            // string, vocal development (Trucks, Hindustani).
                            // LOW = cross-string rolls (dobro, Elmore).
    float barMoveCost;      // cost per fret of bar travel
    float positionCentre;   // preferred fret, as a fraction of the neck (0..1)
    float positionPull;     // how strongly the hand returns to that position
    bool  pedalCapable;     // does this instrument have pedals/levers?
    bool  slantCapable;     // can it produce non-fixed inter-string intervals?

    // --- Time ----------------------------------------------------------------
    float graceSlideSec;    // base duration of a short approach slide
    float longSlideSec;     // base duration of an expressive slide
    float longSlideThresh;  // semitones above which a slide counts as "long"
    float intervalStretch;  // 0 = constant time (TB-303), 1 = constant rate
                            // (tracker 3xx). Literature does not settle this;
                            // ~0.35 sounds right and matches Yang et al.'s
                            // finding that duration is performer-, not
                            // interval-determined.
    float slideSteepness;   // logistic k. 4 = lazy S, 8 = classic, 14 = snappy.
    float pedalSec;         // pedal/lever travel time
    float pedalLateSec;     // pedal moves are placed AFTER the attack — this is
                            // literally what the "crying" sound is.
    float hammerSec;        // bar hammer/pull travel time
    float sustainBudgetSec; // how long a note stays useful before it must be
                            // re-attacked. Resonator ~1.2s; electric+gain ~3s;
                            // volume-pedal ambient ~8s.

    // --- Vibrato -------------------------------------------------------------
    // Maher (JAES 2008): vibrato does not stop during portamento. So depth
    // ramps with time-since-attack rather than gating on slide arrival.
    float vibHz;
    float vibCents;         // peak deviation, one-sided
    float vibRampSec;       // time constant for depth ramp-in
    float vibCentred;       // 1 = oscillate around the target (blues/electric)
                            // 0 = oscillate below it (violin/Hawaiian convention)

    // --- Feel ----------------------------------------------------------------
    float arrivalLagSec;    // + = arrive behind the beat (blues, sacred steel)
                            // - = arrive ahead (Byrd's "P'tah")
    float restProb;         // baseline logit bump toward REST
    float accentDepth;      // velocity spread
};

// -----------------------------------------------------------------------------
// Per-note input
// -----------------------------------------------------------------------------
struct NoteRequest {
    float pitchVolts   = 0.f; // 1V/oct, target pitch
    float rootVolts    = 0.f; // tonic of the current key, for scale-degree tests
    float ioiSec       = 0.25f;// time until the NEXT note (the budget for a slide)
    float beatStrength = 0.5f; // 1 = downbeat, 0 = weakest subdivision
    bool  phraseStart  = false;
    bool  phraseEnd    = false;
    float accent       = 0.5f; // 0..1

    // --- set by the host, not by the style ----------------------------------
    // ACCENT. On a slide instrument you cannot accent a note without picking it:
    // a note arrived at by sliding carries only whatever energy the last pick
    // left in the string. So an accent is not a weight on the decision, it is a
    // constraint on it -- the silent articulations are struck out and the model
    // is left to choose between PLUCK and SHIFT, which is the real question (an
    // accent reached by a wide descending leap is a shift slide, not a pluck).
    //
    // Routing an accent through beatStrength instead was measured and does NOT
    // work: wBeatStrong[PLUCK] scales by the style's gridness, so the full swing
    // of beatStrength moves SAF by ~15 points in Elmore and under 1.5 points in
    // Trucks, Meend and Ambient -- inert in exactly the styles where a fresh
    // attack is most wanted. See tools/slide-articulation-harness.cpp, sec. 7.
    bool  forceAttack  = false;

    // Arrive within this many seconds, or 0 for the style's own slide duration.
    // A player departs BEFORE the beat in order to land on it; a purely reactive
    // model starts its glide when the note changes and so always arrives a
    // glide-time late. Given a clock the host knows where the next grid line is,
    // so the glide can be held to it. This only ever SHORTENS a glide -- it is a
    // deadline, not a duration.
    float arriveBySec  = 0.f;
};

// -----------------------------------------------------------------------------
// Per-note decision (control rate)
// -----------------------------------------------------------------------------
struct Decision {
    int   artic       = ART_PLUCK;
    int   string      = 0;      // chosen string index
    int   fret        = 0;      // chosen fret
    int   prevString  = 0;
    int   prevFret    = 0;
    float intervalSt  = 0.f;    // semitones from the previous sounding pitch
    float glideSec    = 0.f;    // trajectory duration (0 if none)
    bool  slidePossible = false;// was a slide even legal here?
    float probs[ART_COUNT] = {0}; // the sampled distribution, for UI/debug
};

// -----------------------------------------------------------------------------
// Deterministic RNG (xorshift32) — seedable so patches recall identically.
// -----------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    void reseed(uint32_t seed) { s = seed ? seed : 1u; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
private:
    uint32_t s;
};

// -----------------------------------------------------------------------------
// The articulator
// -----------------------------------------------------------------------------
class Articulator {
public:
    Articulator() { reset(); }

    // ---- configuration -----------------------------------------------------
    void setStyles(const Style* a, const Style* b) { styleA = a; styleB = b; }
    void setMorph(float m)       { morph = clampf(m, 0.f, 1.f); }
    // Temperature. 0.05 -> near-deterministic (always the majority choice);
    // 1 -> style statistics as authored; 3 -> nearly uniform.
    void setTemperature(float t) { temp = clampf(t, 0.05f, 4.f); }
    // Global thumb on the scale: + pushes toward legato, - toward attacks.
    // This is the user's "how much slide" knob, applied as a logit offset so it
    // can never override the hard physical constraints.
    void setSlideBias(float b)   { slideBias = clampf(b, -4.f, 4.f); }
    void setSeed(uint32_t s)     { rng.reseed(s); }

    void reset() {
        prevArtic = ART_PLUCK;
        curString = 0; curFret = 0; barFret = 0;
        haveSounding = false;
        sinceAttack = 0.f;
        srcVolts = tgtVolts = curVolts = 0.f;
        glideT = glideDur = 0.f;
        gateOpen = false; trigTimer = 0.f;
        pedalVolts = 0.f; pedalT = pedalDur = 0.f; pedalSrc = pedalTgt = 0.f;
        vibPhase = 0.f; velocity = 0.5f;
    }

    // ---- style access (interpolated) ---------------------------------------
    // Scalars blend linearly; anything probabilistic blends in log space.
    float sp(float Style::*f) const {
        return lerp(styleA->*f, styleB->*f, morph);
    }
    const Tuning& tuning() const { return morph < 0.5f ? *styleA->tuning : *styleB->tuning; }
    bool pedalCapable() const { return (morph < 0.5f ? styleA : styleB)->pedalCapable; }

    // =========================================================================
    //  decide() — call once per note, at control rate.
    // =========================================================================
    Decision decide(const NoteRequest& req) {
        Decision d;
        const float targetSt = req.pitchVolts * 12.f;
        const float prevSt   = srcTargetSt;

        // ---------------------------------------------------------------------
        // 1. FRETBOARD: choose (string, fret) for the target pitch.
        //    This is where the guitar-ness lives. The choice determines whether
        //    a slide is physically possible at all.
        // ---------------------------------------------------------------------
        const Tuning& tun = tuning();
        const float rootSt = req.rootVolts * 12.f;
        int bestS = curString, bestF = curFret;
        float bestCost = 1e30f;
        const int wantSemi = (int)std::lround(targetSt - rootSt);

        for (int s = 0; s < tun.numStrings; ++s) {
            int f = wantSemi - tun.offset[s];
            // wrap into range by octaves so the melody can roam the neck
            while (f < 0)          f += 12;
            while (f > tun.maxFret) f -= 12;
            if (f < 0 || f > tun.maxFret) continue;

            float cost = 0.f;
            if (haveSounding && s != curString) cost += sp(&Style::sameStringBias);
            cost += sp(&Style::barMoveCost) * std::fabs((float)(f - barFret));
            const float centre = sp(&Style::positionCentre) * (float)tun.maxFret;
            cost += sp(&Style::positionPull) * std::fabs((float)f - centre) / 12.f;
            if (cost < bestCost) { bestCost = cost; bestS = s; bestF = f; }
        }
        d.string = bestS; d.fret = bestF;
        d.prevString = curString; d.prevFret = curFret;

        const float interval = haveSounding ? (targetSt - prevSt) : 0.f;
        d.intervalSt = interval;
        const float absInt = std::fabs(interval);

        // ---------------------------------------------------------------------
        // 2. HARD CONSTRAINTS.
        //    These are physics, not taste. No knob may override them.
        // ---------------------------------------------------------------------
        const bool sameString = haveSounding && (bestS == curString);
        const bool moved      = (bestF != curFret) || (bestS != curString);
        // A slide needs: same string, an actual fret change, and a pitch change.
        const bool slideLegal = sameString && bestF != curFret && absInt > 0.01f;
        // A bar hammer/pull only works against an open string on a bar instrument.
        const bool hammerLegal = sameString && (bestF == 0 || curFret == 0) && absInt > 0.01f;
        // Pedals move ±1 or ±2 semitones with the bar stationary.
        // A pedal or knee lever moves a string 1-3 semitones with the bar still:
        // A pedal +2, B pedal +1, C pedal +2, levers +/-1, and combinations reach 3.
        // Anything wider needs the bar, so it is not a pedal move.
        const bool pedalLegal = pedalCapable() && absInt > 0.5f && absInt < 3.5f
                                && std::fabs(interval - std::round(interval)) < 0.02f;
        d.slidePossible = slideLegal;

        // ---------------------------------------------------------------------
        // 3. CONTEXT FEATURES.
        // ---------------------------------------------------------------------
        // Wideness: 0 at a semitone, 1 past ~7 semitones. The perceptual break
        // (Schubert & Wolfe) is around 6 semitones — past that the transit
        // segregates into its own audible event rather than fusing to the target.
        const float wideness = clampf((absInt - 1.f) / 6.f, 0.f, 1.f);

        // Tightness: does the slide actually FIT in the time available? This one
        // feature is what produces the tempo/legato correlation the literature
        // reports (r ~= -0.75) without any per-genre tempo tuning.
        const float needed = slideDuration(absInt);
        const float tight  = clampf(needed / std::max(req.ioiSec, 1e-4f), 0.f, 1.5f);

        // Decay pressure: how dead is the note we would be sliding FROM.
        const float decayP = clampf(sinceAttack / std::max(sp(&Style::sustainBudgetSec), 1e-3f), 0.f, 1.5f);

        // Blue note: is the target a b3, b5 or b7? These are the degrees players
        // slide INTO, because equal temperament is "wrong" there anyway and the
        // approach from below is the point.
        int deg = ((int)std::lround(targetSt - rootSt)) % 12; if (deg < 0) deg += 12;
        const float blue = (deg == 3 || deg == 6 || deg == 10) ? 1.f : 0.f;

        const float ascend = (interval > 0.f) ? 1.f : (interval < 0.f ? -1.f : 0.f);

        // Centre the continuous features on their typical values. Without this,
        // every context term pushes the same way (all of them argue for an
        // attack on average), the sum of their means swamps the style
        // affinities, and every style collapses to "pluck everything". Centred,
        // the affinity in the style table IS the base rate, and context
        // modulates around it — which is both tunable by ear and honest about
        // what the model is claiming.
        const float fBeat  = req.beatStrength - kBeatMean;
        const float fDecay = decayP           - kDecayMean;
        const float fTight = tight            - kTightMean;
        const float fWide  = wideness         - kWideMean;
        const float fBlue  = blue             - kBlueMean;

        // ---------------------------------------------------------------------
        // 4. SCORE every articulation in the log domain.
        // ---------------------------------------------------------------------
        float logit[ART_COUNT];
        for (int a = 0; a < ART_COUNT; ++a) {
            // Markov prior — geometric blend of the two styles' transition rows.
            float base = lerp(styleA->trans[prevArtic][a], styleB->trans[prevArtic][a], morph);

            base += lerp(styleA->wBeatStrong[a],  styleB->wBeatStrong[a],  morph) * fBeat;
            base += lerp(styleA->wPhraseStart[a], styleB->wPhraseStart[a], morph) * (req.phraseStart ? 1.f : 0.f);
            base += lerp(styleA->wPhraseEnd[a],   styleB->wPhraseEnd[a],   morph) * (req.phraseEnd ? 1.f : 0.f);
            base += lerp(styleA->wDecay[a],       styleB->wDecay[a],       morph) * fDecay;
            base += lerp(styleA->wBlue[a],        styleB->wBlue[a],        morph) * fBlue;
            base += lerp(styleA->wAscend[a],      styleB->wAscend[a],      morph) * ascend;
            base += lerp(styleA->wWideInt[a],     styleB->wWideInt[a],     morph) * fWide;
            base += lerp(styleA->wTight[a],       styleB->wTight[a],       morph) * fTight;

            logit[a] = base;
        }

        // User bias: pushes the legato family up and the attack family down.
        logit[ART_SLIDE]  += slideBias;
        logit[ART_HAMMER] += slideBias * 0.5f;
        logit[ART_PEDAL]  += slideBias * 0.5f;
        logit[ART_PLUCK]  -= slideBias * 0.5f;

        // Apply the hard constraints as -inf, AFTER all soft scoring.
        const float NEG = -1e30f;
        if (!slideLegal)  { logit[ART_SLIDE] = NEG; logit[ART_SHIFT] = NEG; }
        if (!hammerLegal)   logit[ART_HAMMER] = NEG;
        if (!pedalLegal)    logit[ART_PEDAL]  = NEG;
        if (!haveSounding || !moved) {
            // Nothing sounding, or a repeated pitch: only a fresh attack (or a
            // rest) makes sense.
            logit[ART_SLIDE] = logit[ART_SHIFT] = logit[ART_HAMMER] = logit[ART_PEDAL] = NEG;
        }
        // A slide that cannot physically complete in the time available is not
        // a slide, it is a smear. Past 1.15x the budget, forbid it outright.
        if (tight > 1.15f) { logit[ART_SLIDE] = NEG; }
        logit[ART_REST] += sp(&Style::restProb);
        if (req.phraseStart) logit[ART_REST] = NEG;
        // An accent is a right-hand event, so it has to produce one. Applied
        // with the other hard constraints rather than as a logit, for the reason
        // given on NoteRequest::forceAttack.
        if (req.forceAttack) {
            logit[ART_SLIDE] = logit[ART_HAMMER] = NEG;
            logit[ART_PEDAL] = logit[ART_REST]   = NEG;
        }

        // ---------------------------------------------------------------------
        // 5. SAMPLE with temperature.
        // ---------------------------------------------------------------------
        int chosen = sampleSoftmax(logit, d.probs);
        d.artic = chosen;

        // ---------------------------------------------------------------------
        // 6. COMMIT — set up the trajectory.
        // ---------------------------------------------------------------------
        velocity = clampf(req.accent * (0.6f + 0.4f * req.beatStrength)
                          + sp(&Style::accentDepth) * (rng.uniform() - 0.5f), 0.f, 1.f);

        srcVolts = curVolts;
        tgtVolts = req.pitchVolts;

        switch (chosen) {
        case ART_SLIDE:
        case ART_SHIFT:
            glideDur = needed;
            break;
        case ART_HAMMER:
            glideDur = sp(&Style::hammerSec);
            break;
        case ART_PEDAL:
            // The bar does not move. The pedal does, and it moves LATE — the
            // attack lands on the beat, the harmony changes after it. That
            // temporal split is exactly the "crying" sound.
            glideDur = sp(&Style::pedalSec);
            pedalSrc = pedalVolts; pedalTgt = req.pitchVolts;
            pedalDur = glideDur; pedalT = -sp(&Style::pedalLateSec);
            break;
        default:
            glideDur = 0.f;
            break;
        }
        // Feel. Positive lag = arrive behind the beat (blues drag, sacred steel's
        // preacher-scoop). Negative lag = arrive ahead of it, which is what Jerry
        // Byrd's "P'tah" does by plucking the new string early so the glide lands
        // on time — implemented here as a shortened glide rather than a negative
        // delay, since we cannot start before the note exists.
        const float lag = sp(&Style::arrivalLagSec);
        if (lag >= 0.f) { glideT = -lag; }
        else            { glideT = 0.f; glideDur = std::max(glideDur * 0.35f, glideDur + lag); }
        // The host's deadline, if it gave one. After the lag maths, so a style
        // that arrives late still cannot overrun the beat it was aiming at.
        if (req.arriveBySec > 0.f) glideDur = std::min(glideDur, req.arriveBySec);
        d.glideSec = glideDur;

        // SHIFT re-attacks on ARRIVAL; PLUCK attacks as soon as the lag expires.
        pendingAttack = articAttacks(chosen);
        if (chosen == ART_REST) { gateOpen = false; haveSounding = false; }
        else                    { gateOpen = true;  haveSounding = true; }

        if (chosen != ART_REST) {
            curString = bestS; curFret = bestF; barFret = bestF;
            srcTargetSt = targetSt;
        }
        prevArtic = chosen;
        return d;
    }

    // =========================================================================
    //  process() — call every audio sample (or every control block).
    // =========================================================================
    struct Out {
        float pitchVolts = 0.f;
        float pedalVolts = 0.f;
        bool  gate       = false;
        bool  trig       = false;
        float slideCv    = 0.f;  // 0..1 while a glide is in flight
        float velocity   = 0.f;
    };

    Out process(float dt) {
        Out o;
        sinceAttack += dt;

        // --- pitch trajectory: 4-parameter logistic ---------------------------
        // Yang, Chew & Rajab (MCM 2015) compared logistic, polynomial, Gaussian
        // and Fourier fits to real performed portamenti at equal parameter
        // count; the logistic won on RMSE and adjusted R^2, significantly.
        float base;
        if (glideT < 0.f) {
            // arrival lag: still sitting on the previous note
            glideT += dt;
            base = srcVolts;
        } else if (glideDur > 1e-6f && glideT < glideDur) {
            glideT += dt;
            const float u = clampf(glideT / glideDur, 0.f, 1.f);
            const float k = sp(&Style::slideSteepness);
            base = srcVolts + (tgtVolts - srcVolts) * logisticNorm(u, k);
            o.slideCv = std::sin(3.14159265f * u); // bump: peaks mid-glide
        } else {
            base = tgtVolts;
            if (pendingAttack) { fireTrig(); sinceAttack = 0.f; pendingAttack = false; }
        }
        curVolts = base;

        // --- pedal voice (oblique motion) -------------------------------------
        if (pedalDur > 1e-6f) {
            pedalT += dt;
            if (pedalT <= 0.f) {
                pedalVolts = pedalSrc;
            } else if (pedalT < pedalDur) {
                const float u = clampf(pedalT / pedalDur, 0.f, 1.f);
                pedalVolts = pedalSrc + (pedalTgt - pedalSrc) * logisticNorm(u, 6.f);
            } else {
                pedalVolts = pedalTgt; pedalDur = 0.f;
            }
        }

        // --- vibrato ----------------------------------------------------------
        // Continuous, with depth ramping in from the last attack rather than
        // hard-gating on slide arrival (Maher, JAES 2008).
        const float hz = sp(&Style::vibHz);
        vibPhase += dt * hz;
        if (vibPhase > 1.f) vibPhase -= 1.f;
        const float ramp = 1.f - std::exp(-sinceAttack / std::max(sp(&Style::vibRampSec), 1e-3f));
        const float cents = sp(&Style::vibCents) * ramp;
        const float centred = sp(&Style::vibCentred);
        float s = std::sin(6.28318531f * vibPhase);
        // centred=1 -> oscillate about the pitch; centred=0 -> only below it.
        float dev = centred * s + (1.f - centred) * (0.5f * (s - 1.f));
        o.pitchVolts = curVolts + dev * cents / 1200.f;

        o.pedalVolts = pedalVolts;
        o.velocity = velocity;
        if (trigTimer > 0.f) { trigTimer -= dt; o.trig = true; }
        // GATE dips for the length of the trig pulse so a downstream envelope
        // retriggers on an attack, and stays high across a slide so it does not.
        // That distinction is the whole point of the module: TRIG drives the
        // exciter, GATE drives the envelope, and a legato slide touches neither.
        o.gate = gateOpen && !o.trig;
        return o;
    }

    // introspection
    int   currentString() const { return curString; }
    int   currentFret()   const { return curFret; }
    float sinceAttackSec()const { return sinceAttack; }

private:
    // --- helpers -------------------------------------------------------------
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    // Normalised logistic: L(0)=0, L(1)=1, inflection at u=0.5.
    static float logisticNorm(float u, float k) {
        auto sig = [](float x) { return 1.f / (1.f + std::exp(-x)); };
        const float lo = sig(-0.5f * k), hi = sig(0.5f * k);
        return (sig(k * (u - 0.5f)) - lo) / (hi - lo);
    }

    // Duration of a slide covering |interval| semitones.
    // intervalStretch = 0 -> constant time (the TB-303's fixed RC lag)
    // intervalStretch = 1 -> constant rate (a tracker's 3xx command)
    // The literature does not adjudicate; ~0.35 is a good musical compromise and
    // is consistent with Yang et al.'s finding that slide *duration* is a
    // performer trait rather than a function of the interval.
    float slideDuration(float absInt) const {
        const float g = sp(&Style::graceSlideSec);
        const float L = sp(&Style::longSlideSec);
        const float thr = sp(&Style::longSlideThresh);
        const float mix = clampf((absInt - 1.f) / std::max(thr - 1.f, 0.5f), 0.f, 1.f);
        const float basedur = lerp(g, L, mix);
        const float stretch = 1.f + sp(&Style::intervalStretch) * (absInt / 3.f - 1.f);
        return std::max(basedur * std::max(stretch, 0.25f), 0.008f);
    }

    int sampleSoftmax(const float* logit, float* outProbs) {
        float mx = -1e30f;
        for (int a = 0; a < ART_COUNT; ++a) mx = std::max(mx, logit[a]);
        if (mx < -1e29f) { for (int a = 0; a < ART_COUNT; ++a) outProbs[a] = 0.f; outProbs[ART_PLUCK] = 1.f; return ART_PLUCK; }
        float sum = 0.f, p[ART_COUNT];
        for (int a = 0; a < ART_COUNT; ++a) {
            p[a] = (logit[a] < -1e29f) ? 0.f : std::exp((logit[a] - mx) / temp);
            sum += p[a];
        }
        if (sum <= 0.f) { for (int a = 0; a < ART_COUNT; ++a) outProbs[a] = 0.f; outProbs[ART_PLUCK] = 1.f; return ART_PLUCK; }
        float r = rng.uniform() * sum, acc = 0.f; int chosen = ART_PLUCK;
        for (int a = 0; a < ART_COUNT; ++a) {
            outProbs[a] = p[a] / sum;
            acc += p[a];
            if (r <= acc) { chosen = a; r = 1e30f; }
        }
        return chosen;
    }

    void fireTrig() { trigTimer = 0.001f; }

    // --- state ---------------------------------------------------------------
    const Style *styleA = nullptr, *styleB = nullptr;
    float morph = 0.f, temp = 1.f, slideBias = 0.f;
    Rng   rng;

    int   prevArtic = ART_PLUCK;
    int   curString = 0, curFret = 0, barFret = 0;
    bool  haveSounding = false;
    float srcTargetSt = 0.f;
    float sinceAttack = 0.f;

    float srcVolts = 0.f, tgtVolts = 0.f, curVolts = 0.f;
    float glideT = 0.f, glideDur = 0.f;
    bool  pendingAttack = false;

    float pedalVolts = 0.f, pedalSrc = 0.f, pedalTgt = 0.f, pedalT = 0.f, pedalDur = 0.f;

    bool  gateOpen = false;
    float trigTimer = 0.f;
    float vibPhase = 0.f;
    float velocity = 0.5f;
};

} // namespace slide
} // namespace sfs
