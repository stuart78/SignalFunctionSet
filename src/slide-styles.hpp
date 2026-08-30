// =============================================================================
//  SlideStyles.hpp
//  Ten hand-authored style presets for SlideArticulator.
//
//  Each preset is built from a small set of interpretable seeds rather than a
//  raw 6x6 matrix, so it stays tunable by ear. The 6x6 transition matrix is
//  generated from three numbers (affinity, inertia, breath) — see makeTrans().
//
//  The target SAF (slide-attack fraction) in each comment is the figure the
//  research survey estimates for that tradition: of all melodic note events,
//  the fraction whose pitch is ARRIVED AT by bar/slide motion rather than by a
//  fresh pluck already at pitch. Run test_slide to check the model reproduces it.
//
//  Note the headline finding these numbers encode: the two most famous "slide
//  guitar" sounds in popular music — Delta bottleneck and the Dust My Broom
//  figure — are near the BOTTOM of the legato ranking. Slide guitar's reputation
//  for legato is earned by the steel family, not the bottleneck family.
// =============================================================================

#pragma once
#include "slide-articulator.hpp"

namespace sfs {
namespace slide {

// -----------------------------------------------------------------------------
// Seeds — the tunable surface. Six numbers shape the whole Markov prior.
// -----------------------------------------------------------------------------
struct StyleSeed {
    const char*   name;
    const Tuning* tuning;

    float aff[ART_COUNT];   // base log-affinity for each articulation
    float inertia;          // bonus for repeating the previous articulation
                            // (this is what makes legato runs run)
    float breath;           // bonus toward PLUCK after a legato move — the
                            // player re-articulating to take a breath

    // context sensitivities, 0..2 -ish
    float gridness;         // how hard the beat and phrase structure force attacks
    float decaySens;        // how urgently a dying note demands a re-attack
    float blueAttract;      // pull of b3/b5/b7 as slide targets
    float descShift;        // preference for re-attacking on DESCENDING slides
                            // (descending loses energy fastest — the classic
                            // reason a shift slide exists)
    float widePenalty;      // resistance to slides wider than ~6 semitones

    // hand
    float sameStringBias, barMoveCost, positionCentre, positionPull;
    bool  pedalCapable, slantCapable;

    // time
    float graceSec, longSec, longThresh, stretch, steep;
    float pedalSec, pedalLate, hammerSec, sustainSec;

    // vibrato
    float vibHz, vibCents, vibRamp, vibCentred;

    // feel
    float arrivalLag, restProb, accentDepth;
};

// -----------------------------------------------------------------------------
// Build a 6x6 transition matrix from three seeds.
// trans[prev][next] = affinity(next) + inertia*[prev==next] + breath*[legato->pluck]
// -----------------------------------------------------------------------------
inline void makeTrans(const StyleSeed& s, float t[ART_COUNT][ART_COUNT]) {
    for (int p = 0; p < ART_COUNT; ++p) {
        const bool prevLegato = (p == ART_SLIDE || p == ART_HAMMER || p == ART_PEDAL);
        for (int n = 0; n < ART_COUNT; ++n) {
            float v = s.aff[n];
            if (p == n)                        v += s.inertia;
            if (prevLegato && n == ART_PLUCK)  v += s.breath;
            // Leaving a rest, you must attack.
            if (p == ART_REST && (n == ART_SLIDE || n == ART_SHIFT ||
                                  n == ART_HAMMER || n == ART_PEDAL)) v -= 6.f;
            t[p][n] = v;
        }
    }
}

// -----------------------------------------------------------------------------
// Expand a seed into a full Style.
// The context-weight table is shared across styles and scaled by the seed's
// sensitivities — so the *shape* of musical reasoning is universal, and what
// differs between traditions is how strongly each consideration is felt.
// -----------------------------------------------------------------------------
inline Style expand(const StyleSeed& s) {
    Style st{};
    st.name = s.name;
    st.tuning = s.tuning;
    makeTrans(s, st.trans);

    for (int a = 0; a < ART_COUNT; ++a) {
        st.wBeatStrong[a] = st.wPhraseStart[a] = st.wPhraseEnd[a] = 0.f;
        st.wDecay[a] = st.wBlue[a] = st.wAscend[a] = 0.f;
        st.wWideInt[a] = st.wTight[a] = 0.f;
    }

    // Beat strength: the pluck is the only unambiguous rhythmic marker an
    // instrument with a gradual-onset gesture has. Downbeats want attacks.
    st.wBeatStrong[ART_PLUCK]  =  1.10f * s.gridness;
    st.wBeatStrong[ART_SHIFT]  =  0.45f * s.gridness;
    st.wBeatStrong[ART_SLIDE]  = -0.85f * s.gridness;
    st.wBeatStrong[ART_PEDAL]  = -0.30f * s.gridness;
    st.wBeatStrong[ART_REST]   = -0.60f;

    // "Pick only the first note of a group, then slide into the remaining notes
    // without additional picking." Phrase-initial notes want a fresh attack —
    // but scaled by gridness, because a tradition that barely acknowledges a
    // metrical grid (ambient steel, alap) barely acknowledges a phrase boundary
    // as a reason to re-articulate either. A floor remains: something has to
    // start the sound.
    st.wPhraseStart[ART_PLUCK] =  1.20f + 1.90f * s.gridness;
    st.wPhraseStart[ART_SLIDE] = -0.80f - 1.55f * s.gridness;
    st.wPhraseStart[ART_SHIFT] = -0.40f - 0.70f * s.gridness;
    st.wPhraseStart[ART_PEDAL] = -0.60f - 1.10f * s.gridness;

    // Phrase exits are where the fall-off / throwaway lives — cheap, no attack.
    st.wPhraseEnd[ART_SLIDE]   =  1.00f;
    st.wPhraseEnd[ART_PLUCK]   = -0.40f;

    // Decay pressure: a note that has gone dull cannot carry a slide.
    st.wDecay[ART_PLUCK] =  1.90f * s.decaySens;
    st.wDecay[ART_SHIFT] =  1.60f * s.decaySens;
    st.wDecay[ART_SLIDE] = -2.40f * s.decaySens;
    st.wDecay[ART_PEDAL] = -1.20f * s.decaySens;

    // Blue notes are the degrees where equal temperament is "wrong" anyway —
    // players slide into them and shade the intonation deliberately.
    st.wBlue[ART_SLIDE] =  1.10f * s.blueAttract;
    st.wBlue[ART_SHIFT] =  0.35f * s.blueAttract;
    st.wBlue[ART_PLUCK] = -0.45f * s.blueAttract;

    // Ascending slides arrive with energy; descending ones bleed it, which is
    // why the shift slide (re-struck on arrival) is a descending idiom.
    st.wAscend[ART_SLIDE] =  0.45f;
    st.wAscend[ART_SHIFT] = -0.70f * s.descShift;
    st.wAscend[ART_PLUCK] = -0.20f;

    // Past ~6 semitones the transit segregates from the target and reads as its
    // own event rather than an ornament (Schubert & Wolfe's threshold).
    st.wWideInt[ART_SLIDE] = -1.90f * s.widePenalty;
    st.wWideInt[ART_SHIFT] =  0.35f;
    st.wWideInt[ART_PLUCK] =  1.00f * s.widePenalty;

    // Tightness: if the slide does not fit in the time available, it isn't one.
    // This single term produces the tempo/legato correlation.
    st.wTight[ART_SLIDE] = -2.60f;
    st.wTight[ART_SHIFT] = -1.40f;
    st.wTight[ART_PLUCK] =  0.90f;

    st.sameStringBias = s.sameStringBias;
    st.barMoveCost    = s.barMoveCost;
    st.positionCentre = s.positionCentre;
    st.positionPull   = s.positionPull;
    st.pedalCapable   = s.pedalCapable;
    st.slantCapable   = s.slantCapable;

    st.graceSlideSec   = s.graceSec;
    st.longSlideSec    = s.longSec;
    st.longSlideThresh = s.longThresh;
    st.intervalStretch = s.stretch;
    st.slideSteepness  = s.steep;
    st.pedalSec        = s.pedalSec;
    st.pedalLateSec    = s.pedalLate;
    st.hammerSec       = s.hammerSec;
    st.sustainBudgetSec= s.sustainSec;

    st.vibHz     = s.vibHz;
    st.vibCents  = s.vibCents;
    st.vibRampSec= s.vibRamp;
    st.vibCentred= s.vibCentred;

    st.arrivalLagSec = s.arrivalLag;
    st.restProb      = s.restProb;
    st.accentDepth   = s.accentDepth;
    return st;
}

// -----------------------------------------------------------------------------
// The ten presets, ordered by legato content — which is a musically meaningful
// ordering for a morph knob: turn it clockwise and the player gets smoother.
// -----------------------------------------------------------------------------
// Ordered by MEASURED legato content, so that turning a MORPH knob clockwise
// monotonically gets smoother. That ordering is the one property of this table
// that survives a change of test melody -- Spearman +0.93 to +0.99 across
// pentatonic, diatonic and chromatic sources, while the absolute SAF moves by up
// to 30 points. The absolute figures below are the CALIBRATION TARGET, not a
// prediction, and should not be quoted as a result.
//
// Pedal Steel used to sit between Hawaiian and Sacred, on its SAF. That was
// wrong and audibly so: SAF does not count ART_PEDAL, which carries 43-47% of
// its notes, and by legato it outranks the three styles that followed it -- so
// the knob went up, down, then up again through that region. Moved on measured
// legato (90.6% diatonic, above Meend's 90.2%), which is what the ordering
// claims to be. Safe to do now ONLY because no style index has been serialised
// into a patch yet; once one has, this order is frozen like every other enum in
// the plugin and an inversion has to be lived with instead.
enum StyleId {
    STYLE_ELMORE = 0,   // measured legato ~30%
    STYLE_DOBRO,        //                 ~35%
    STYLE_DELTA,        //                 ~47%
    STYLE_CHICAGO,      //                 ~65%
    STYLE_HAWAIIAN,     //                 ~75%
    STYLE_SACRED,       //                 ~84%
    STYLE_TRUCKS,       //                 ~88%
    STYLE_MEEND,        //                 ~90%
    STYLE_PEDAL,        //                 ~91%   (slide + pedal)
    STYLE_AMBIENT,      //                 ~96%
    STYLE_COUNT
};

inline const Style* styleTable() {
    static Style tbl[STYLE_COUNT];
    static bool built = false;
    if (built) return tbl;

    // --- 0. ELMORE JAMES / HOUND DOG TAYLOR --------------------------------
    // The great paradox: the most famous slide riff in blues is one of the least
    // legato things there is. The bar is parked at the 12th fret and the right
    // hand rakes triplets across three strings. Its identity is the ATTACK.
    // Hence: near-zero sameStringBias (cross-string raking) and a HIGH bar move
    // cost (the bar does not want to travel).
    { StyleSeed s{};
      s.name="Elmore Boogie"; s.tuning=&TUNING_OPEN_D;
      s.aff[ART_PLUCK]=2.000f; s.aff[ART_SLIDE]=1.435f; s.aff[ART_SHIFT]=-0.307f;
      s.aff[ART_HAMMER]=-1.389f; s.aff[ART_PEDAL]=-20.000f; s.aff[ART_REST]=-1.500f;
      // calibrated legato trim +2.185 applied to the legato affinities above
      s.inertia=0.45f; s.breath=0.70f;
      s.gridness=1.5f; s.decaySens=1.0f; s.blueAttract=0.7f; s.descShift=0.8f; s.widePenalty=1.3f;
      s.sameStringBias=0.20f; s.barMoveCost=0.34f; s.positionCentre=0.55f; s.positionPull=0.55f;
      s.graceSec=0.045f; s.longSec=0.20f; s.longThresh=4.f; s.stretch=0.30f; s.steep=12.f;
      s.pedalSec=0.10f; s.pedalLate=0.12f; s.hammerSec=0.020f; s.sustainSec=2.0f;
      s.vibHz=7.0f; s.vibCents=90.f; s.vibRamp=0.06f; s.vibCentred=0.9f;
      s.arrivalLag=0.02f; s.restProb=0.0f; s.accentDepth=0.30f;
      tbl[STYLE_ELMORE]=expand(s); }

    // --- 1. DOBRO / BLUEGRASS ----------------------------------------------
    // Josh Graves imported Earl Scruggs' three-finger banjo rolls wholesale. The
    // GBDGBD tuning is a close-voiced triad chosen to fit what the banjo does —
    // it is optimised for PLUCKING, not sliding. Slides survive as sub-50ms
    // grace notes. Bar hammer-ons and pull-offs are idiomatic, so ART_HAMMER
    // gets real weight here.
    { StyleSeed s{};
      s.name="Dobro Bluegrass"; s.tuning=&TUNING_DOBRO_G;
      s.aff[ART_PLUCK]=1.524f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-0.113f;
      s.aff[ART_HAMMER]=0.660f; s.aff[ART_PEDAL]=-20.000f; s.aff[ART_REST]=-2.076f;
      // calibrated legato trim +2.726 applied to the legato affinities above
      s.inertia=0.40f; s.breath=0.60f;
      s.gridness=1.4f; s.decaySens=1.1f; s.blueAttract=0.8f; s.descShift=0.7f; s.widePenalty=1.4f;
      s.sameStringBias=0.15f; s.barMoveCost=0.30f; s.slantCapable=true; s.positionCentre=0.40f; s.positionPull=0.40f;
      s.graceSec=0.040f; s.longSec=0.18f; s.longThresh=4.f; s.stretch=0.30f; s.steep=12.f;
      s.pedalSec=0.10f; s.pedalLate=0.12f; s.hammerSec=0.018f; s.sustainSec=1.6f;
      s.vibHz=7.0f; s.vibCents=32.f; s.vibRamp=0.06f; s.vibCentred=0.6f;
      s.arrivalLag=0.0f; s.restProb=-0.2f; s.accentDepth=0.25f;
      tbl[STYLE_DOBRO]=expand(s); }

    // --- 2. DELTA BOTTLENECK ------------------------------------------------
    // Lower than its reputation, and for a structural reason: the solo
    // bottleneck player IS the rhythm section. Thumb bass and chordal strums are
    // all fresh attacks; the slide's territory is a narrow melodic band on the
    // top strings. Short sustain budget — a resonator note goes dull fast.
    { StyleSeed s{};
      s.name="Delta Bottleneck"; s.tuning=&TUNING_OPEN_G;
      s.aff[ART_PLUCK]=0.638f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-0.081f;
      s.aff[ART_HAMMER]=-1.095f; s.aff[ART_PEDAL]=-20.000f; s.aff[ART_REST]=-1.612f;
      // calibrated legato trim +2.362 applied to the legato affinities above
      s.inertia=0.50f; s.breath=0.50f;
      s.gridness=1.2f; s.decaySens=1.3f; s.blueAttract=1.2f; s.descShift=1.0f; s.widePenalty=1.1f;
      s.sameStringBias=1.20f; s.barMoveCost=0.11f; s.positionCentre=0.50f; s.positionPull=0.45f;
      s.graceSec=0.075f; s.longSec=0.30f; s.longThresh=5.f; s.stretch=0.35f; s.steep=9.f;
      s.pedalSec=0.10f; s.pedalLate=0.12f; s.hammerSec=0.025f; s.sustainSec=1.2f;
      s.vibHz=6.5f; s.vibCents=85.f; s.vibRamp=0.10f; s.vibCentred=0.8f;
      s.arrivalLag=0.03f; s.restProb=0.0f; s.accentDepth=0.30f;
      tbl[STYLE_DELTA]=expand(s); }

    // --- 3. CHICAGO SLOW BLUES ---------------------------------------------
    // Muddy, Robert Nighthawk, Earl Hooker. Amplification changes the physics:
    // gain and a light touch extend the sustain budget, so the re-attack clock
    // runs slower and single-note vocal lines become possible. Hooker added a
    // wah "to add a vocal-like quality" — the goal is stated outright.
    { StyleSeed s{};
      s.name="Chicago Slow"; s.tuning=&TUNING_OPEN_G;
      s.aff[ART_PLUCK]=-0.521f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-0.361f;
      s.aff[ART_HAMMER]=-1.398f; s.aff[ART_PEDAL]=-20.000f; s.aff[ART_REST]=-1.871f;
      // calibrated legato trim +2.121 applied to the legato affinities above
      s.inertia=0.55f; s.breath=0.45f;
      s.gridness=0.9f; s.decaySens=1.0f; s.blueAttract=1.4f; s.descShift=0.9f; s.widePenalty=1.0f;
      s.sameStringBias=2.00f; s.barMoveCost=0.09f; s.positionCentre=0.55f; s.positionPull=0.40f;
      s.graceSec=0.090f; s.longSec=0.36f; s.longThresh=5.f; s.stretch=0.35f; s.steep=8.f;
      s.pedalSec=0.10f; s.pedalLate=0.12f; s.hammerSec=0.025f; s.sustainSec=2.8f;
      s.vibHz=7.0f; s.vibCents=105.f; s.vibRamp=0.09f; s.vibCentred=0.9f;
      s.arrivalLag=0.05f; s.restProb=0.0f; s.accentDepth=0.28f;
      tbl[STYLE_CHICAGO]=expand(s); }

    // --- 4. HAWAIIAN KIKA KILA ---------------------------------------------
    // The steel player has no rhythm duty — an ukulele or rhythm guitar holds the
    // groove — and the tempos are slow, so a 5-12 semitone glissando fits inside
    // a note value. Jerry Byrd's discipline: shallow vibrato, held frequency,
    // and arriving slightly EARLY (his "P'tah" plucks the new string early so
    // the glide lands on time), hence the negative arrival lag.
    { StyleSeed s{};
      s.name="Hawaiian Kika Kila"; s.tuning=&TUNING_C6;
      s.aff[ART_PLUCK]=-2.829f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-1.040f;
      s.aff[ART_HAMMER]=-1.732f; s.aff[ART_PEDAL]=-20.000f; s.aff[ART_REST]=-3.279f;
      // calibrated legato trim +3.079 applied to the legato affinities above
      s.inertia=0.60f; s.breath=0.35f;
      s.gridness=0.6f; s.decaySens=0.8f; s.blueAttract=0.3f; s.descShift=0.4f; s.widePenalty=0.45f;
      s.sameStringBias=1.60f; s.barMoveCost=0.05f; s.slantCapable=true; s.positionCentre=0.45f; s.positionPull=0.30f;
      s.graceSec=0.140f; s.longSec=0.65f; s.longThresh=7.f; s.stretch=0.50f; s.steep=6.f;
      s.pedalSec=0.10f; s.pedalLate=0.12f; s.hammerSec=0.030f; s.sustainSec=3.0f;
      s.vibHz=4.8f; s.vibCents=32.f; s.vibRamp=0.25f; s.vibCentred=0.15f;
      s.arrivalLag=-0.02f; s.restProb=0.0f; s.accentDepth=0.18f;
      tbl[STYLE_HAWAIIAN]=expand(s); }

    // --- 5. PEDAL STEEL BALLAD (E9) ----------------------------------------
    // The structural outlier, and the reason ART_PEDAL exists. Bud Isaacs' 1953
    // baling-wire hinge let a player change pitch while a chord SUSTAINS. The E9
    // copedent then hard-wires country's two commonest melodic motions (5->6 and
    // 3->4) to the feet. So the bar barely travels — note the very high
    // barMoveCost — and yet the result is among the most legato playing there is.
    // The pedal move is placed AFTER the attack: that lag is the crying sound.
    { StyleSeed s{};
      s.name="Pedal Steel Ballad"; s.tuning=&TUNING_E9;
      s.aff[ART_PLUCK]=-4.200f; s.aff[ART_SLIDE]=1.600f; s.aff[ART_SHIFT]=-2.500f;
      s.aff[ART_HAMMER]=-2.650f; s.aff[ART_PEDAL]=2.000f; s.aff[ART_REST]=-5.150f;
      // calibrated legato trim +6.000 applied to the legato affinities above
      s.inertia=0.40f; s.breath=0.30f;
      s.gridness=0.7f; s.decaySens=0.5f; s.blueAttract=0.4f; s.descShift=0.5f; s.widePenalty=0.9f;
      s.sameStringBias=1.60f; s.barMoveCost=0.40f; s.positionCentre=0.35f; s.positionPull=0.60f;
      s.pedalCapable=true; s.slantCapable=true;
      s.graceSec=0.100f; s.longSec=0.42f; s.longThresh=6.f; s.stretch=0.40f; s.steep=7.f;
      s.pedalSec=0.11f; s.pedalLate=0.14f; s.hammerSec=0.028f; s.sustainSec=8.0f;
      s.vibHz=5.8f; s.vibCents=24.f; s.vibRamp=0.30f; s.vibCentred=0.5f;
      s.arrivalLag=0.01f; s.restProb=0.0f; s.accentDepth=0.15f;
      tbl[STYLE_PEDAL]=expand(s); }

    // --- 6. SACRED STEEL ----------------------------------------------------
    // E7th rather than E9 — the b7 sits inside the open tuning, so the blue
    // seventh is available under a straight bar with no pedal or slant. The
    // aesthetic target is stated explicitly by the tradition: Henry Nelson "was
    // the first guy who ever moaned on a guitar". Emphatically late arrivals,
    // the widest and least regular vibrato in the survey.
    { StyleSeed s{};
      s.name="Sacred Steel"; s.tuning=&TUNING_E9;
      s.aff[ART_PLUCK]=-3.485f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-1.567f;
      s.aff[ART_HAMMER]=-2.254f; s.aff[ART_PEDAL]=0.673f; s.aff[ART_REST]=-4.285f;
      // calibrated legato trim +4.135 applied to the legato affinities above
      s.inertia=0.55f; s.breath=0.35f;
      s.gridness=0.7f; s.decaySens=0.7f; s.blueAttract=1.5f; s.descShift=0.6f; s.widePenalty=0.55f;
      s.sameStringBias=2.20f; s.barMoveCost=0.10f; s.positionCentre=0.45f; s.positionPull=0.35f;
      s.pedalCapable=true; s.slantCapable=true;
      s.graceSec=0.160f; s.longSec=0.55f; s.longThresh=7.f; s.stretch=0.45f; s.steep=5.f;
      s.pedalSec=0.12f; s.pedalLate=0.16f; s.hammerSec=0.030f; s.sustainSec=5.0f;
      s.vibHz=5.5f; s.vibCents=150.f; s.vibRamp=0.15f; s.vibCentred=0.9f;
      s.arrivalLag=0.12f; s.restProb=0.0f; s.accentDepth=0.35f;
      tbl[STYLE_SACRED]=expand(s); }

    // --- 7. DEREK TRUCKS / VOCAL ROCK --------------------------------------
    // "Playing on one string can almost emulate the human voice." That sentence
    // is a parameter: sameStringBias is the single biggest lever on legato
    // content in the whole model, and here it is very high. Bare fingers (no
    // pick) supply the damping control that makes long legato lines clean.
    { StyleSeed s{};
      s.name="Trucks Vocal"; s.tuning=&TUNING_OPEN_D;
      s.aff[ART_PLUCK]=-6.800f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-3.550f;
      s.aff[ART_HAMMER]=-3.800f; s.aff[ART_PEDAL]=-20.000f; s.aff[ART_REST]=-7.500f;
      // calibrated legato trim +7.000 applied to the legato affinities above
      s.inertia=0.65f; s.breath=0.35f;
      s.gridness=0.55f; s.decaySens=0.8f; s.blueAttract=1.2f; s.descShift=0.7f; s.widePenalty=0.6f;
      s.sameStringBias=3.00f; s.barMoveCost=0.06f; s.positionCentre=0.55f; s.positionPull=0.30f;
      s.graceSec=0.110f; s.longSec=0.45f; s.longThresh=6.f; s.stretch=0.40f; s.steep=6.f;
      s.pedalSec=0.10f; s.pedalLate=0.12f; s.hammerSec=0.028f; s.sustainSec=3.5f;
      s.vibHz=5.6f; s.vibCents=58.f; s.vibRamp=0.30f; s.vibCentred=0.7f;
      s.arrivalLag=0.06f; s.restProb=0.0f; s.accentDepth=0.22f;
      tbl[STYLE_TRUCKS]=expand(s); }

    // --- 8. HINDUSTANI MEEND (alap) ----------------------------------------
    // The most legato playing in the survey. One pluck can carry a meend of
    // 5-14 semitones through several scale degrees, because chikari drone strokes
    // supply the articulation the melody doesn't have to. Note widePenalty is
    // very low: a long slide is NORMAL here, not a special gesture. Gamak is
    // slow and wide, not a fast shimmer.
    { StyleSeed s{};
      s.name="Hindustani Meend"; s.tuning=&TUNING_OPEN_D;
      s.aff[ART_PLUCK]=-6.300f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-3.450f;
      s.aff[ART_HAMMER]=-3.100f; s.aff[ART_PEDAL]=-20.000f; s.aff[ART_REST]=-5.800f;
      // calibrated legato trim +5.000 applied to the legato affinities above
      s.inertia=0.70f; s.breath=0.25f;
      s.gridness=0.35f; s.decaySens=0.6f; s.blueAttract=0.5f; s.descShift=0.3f; s.widePenalty=0.15f;
      s.sameStringBias=4.00f; s.barMoveCost=0.04f; s.positionCentre=0.50f; s.positionPull=0.20f;
      s.graceSec=0.220f; s.longSec=1.10f; s.longThresh=9.f; s.stretch=0.60f; s.steep=4.5f;
      s.pedalSec=0.10f; s.pedalLate=0.12f; s.hammerSec=0.035f; s.sustainSec=6.0f;
      s.vibHz=3.0f; s.vibCents=120.f; s.vibRamp=0.40f; s.vibCentred=0.6f;
      s.arrivalLag=0.0f; s.restProb=0.0f; s.accentDepth=0.20f;
      tbl[STYLE_MEEND]=expand(s); }

    // --- 9. AMBIENT STEEL ---------------------------------------------------
    // The volume pedal deletes the attack transient entirely, so the slide/pluck
    // distinction dissolves — there are no perceptible attacks left to count.
    // Enormous sustain budget, very lazy logistic (steep=3.5 gives a long slow S),
    // barely any grid pressure.
    { StyleSeed s{};
      s.name="Ambient Steel"; s.tuning=&TUNING_E9;
      s.aff[ART_PLUCK]=-10.200f; s.aff[ART_SLIDE]=2.000f; s.aff[ART_SHIFT]=-6.100f;
      s.aff[ART_HAMMER]=-5.800f; s.aff[ART_PEDAL]=-0.800f; s.aff[ART_REST]=-10.000f;
      // calibrated legato trim +8.000 applied to the legato affinities above
      s.inertia=0.70f; s.breath=0.20f;
      s.gridness=0.25f; s.decaySens=0.4f; s.blueAttract=0.3f; s.descShift=0.3f; s.widePenalty=0.25f;
      s.sameStringBias=5.00f; s.barMoveCost=0.05f; s.positionCentre=0.45f; s.positionPull=0.25f;
      s.pedalCapable=true; s.slantCapable=true;
      s.graceSec=0.400f; s.longSec=1.50f; s.longThresh=10.f; s.stretch=0.25f; s.steep=3.5f;
      s.pedalSec=0.30f; s.pedalLate=0.25f; s.hammerSec=0.060f; s.sustainSec=12.0f;
      s.vibHz=4.0f; s.vibCents=18.f; s.vibRamp=1.20f; s.vibCentred=0.4f;
      s.arrivalLag=0.0f; s.restProb=-0.3f; s.accentDepth=0.10f;
      tbl[STYLE_AMBIENT]=expand(s); }

    built = true;
    return tbl;
}

inline const Style& getStyle(int id) {
    return styleTable()[id < 0 ? 0 : (id >= STYLE_COUNT ? STYLE_COUNT - 1 : id)];
}

} // namespace slide
} // namespace sfs
