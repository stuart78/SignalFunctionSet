#pragma once
// =============================================================================
// Drum taste — how a style ornaments a groove.
//
// One table describing, per genre and sub-genre, what a player or producer
// actually does to a pattern: how freely they ratchet and ON WHICH DRUM, how
// many ghost notes they leave, which lanes a variation layer may add to, how
// far the groove may bend before it stops being the style, and which fills are
// idiomatic versus plain wrong.
//
// This is not only a fill setting. The same numbers drive added notes, ghosts
// and per-set identity strength, so one lookup governs every way the engine
// touches a groove.
//
// Three findings from the research that the code had wrong:
//
//   * The ratchet lane is NOT always the hat. Trap and drill ratchet the closed
//     hat, crunk ratchets the SNARE in triplets, and footwork ratchets the
//     KICK. "Ratchet the hats" gets three whole styles wrong.
//   * `added` must be lane-scoped. Tech house spends its budget on percussion,
//     G-funk on auxiliary percussion, electro on the kick — and house must
//     never have a note added to its kick at all.
//   * Low-vary styles need the engine to SUBTRACT. Dub techno, techno, minimal,
//     crunk and lo-fi are defined by restraint; adding notes destroys them.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <string>

namespace sfs {

// lane indices, matching the pattern format's canonical order
enum { L_KICK = 0, L_SNARE, L_CHH, L_OHH, L_LO, L_HI, L_CP, L_BELL };
static inline uint8_t laneBit(int l) { return (uint8_t)(1u << l); }

// fill shapes the engine can build
enum FillShape { F_NONE = 0, F_ROLL, F_STUTTER, F_DROP, F_CASCADE, F_BUILD, F_LIFT, F_ACCENT, F_COUNT };

struct Taste {
	const char* key;        // "family" or "family.subgenre"
	float ratchet;          // 0-1 propensity for fast retriggers
	uint8_t ratchetLane;    // which drum they belong on
	float ghost;            // 0-1 ghost-note density
	uint8_t ghostLane;
	float added;            // 0-1 licence for the variation layer to add notes
	uint8_t addedLanes;     // bitmask of lanes it may touch
	float vary;             // 0-1 identity strength (how far the groove may bend)
	float swing;            // 0-1 typical feel
	uint16_t fills;         // bitmask of idiomatic FillShape values
	uint8_t fillBeats;      // typical fill length
	uint8_t frozenLanes;    // lanes the engine must not touch AT ALL
};

// Frozen lanes are the strongest finding in the traditional research. Eight of
// ten Latin styles and most West African ones have a timeline lane -- güira,
// triangle, cuá, chico, gankogui, the bembé bell -- that never varies for the
// whole piece: "the fundamental pattern remains unaltered throughout the entire
// form". Dembow has no timeline but freezes its kick/snare tresillo instead,
// and the MENA styles freeze their dum positions (perturb saidi's two centre
// dums and it silently becomes maqsum). These are not low probabilities; they
// are exclusions.

#define FB(x) (uint16_t)(1u << (x))

// Ordered most-specific first; lookup falls back from family.subgenre → family
// → a neutral default.
static const Taste DRUM_TASTE[] = {
	{"hiphop.boombap", 0.10f, L_SNARE, 0.85f, L_KICK, 0.35f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_CHH)), 0.40f, 0.40f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 2, 0},
	{"hiphop.trap", 0.95f, L_CHH, 0.30f, L_CHH, 0.50f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI)), 0.48f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_LIFT)), 2, (uint8_t)(laneBit(L_KICK))},
	{"hiphop.lofi", 0.05f, L_CHH, 0.70f, L_SNARE, 0.30f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI)), 0.40f, 0.50f,
	 (uint16_t)(FB(F_NONE) | FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 2, 0},
	{"hiphop.ukdrill", 0.70f, L_CHH, 0.35f, L_SNARE, 0.45f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI)), 0.40f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 2, 0},
	{"hiphop.chidrill", 0.55f, L_CHH, 0.20f, L_SNARE, 0.35f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_BELL)), 0.35f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 2, 0},
	{"hiphop.gfunk", 0.10f, L_CHH, 0.60f, L_CHH, 0.50f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_LO) | laneBit(L_HI)), 0.45f, 0.45f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 2, 0},
	{"hiphop.crunk", 0.70f, L_SNARE, 0.15f, L_SNARE, 0.30f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CP)), 0.30f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 4, 0},
	{"hiphop.cloud", 0.20f, L_CHH, 0.40f, L_SNARE, 0.55f,
	 (uint8_t)(laneBit(L_HI) | laneBit(L_BELL)), 0.75f, 0.25f,
	 (uint16_t)(FB(F_STUTTER) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 4, 0},
	{"hiphop", 0.35f, L_CHH, 0.55f, L_SNARE, 0.35f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI)), 0.45f, 0.30f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 2, 0},
	{"electronic.house", 0.05f, L_CHH, 0.30f, L_CP, 0.40f,
	 (uint8_t)(laneBit(L_OHH) | laneBit(L_HI) | laneBit(L_CP)), 0.20f, 0.15f,
	 (uint16_t)(FB(F_DROP) | FB(F_BUILD) | FB(F_LIFT) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"electronic.deephouse", 0.02f, L_CHH, 0.60f, L_CHH, 0.50f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.35f, 0.45f,
	 (uint16_t)(FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"electronic.techhouse", 0.15f, L_HI, 0.50f, L_CHH, 0.55f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.40f, 0.30f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"electronic.techno", 0.12f, L_CHH, 0.20f, L_CHH, 0.30f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI) | laneBit(L_BELL)), 0.18f, 0.05f,
	 (uint16_t)(FB(F_DROP) | FB(F_BUILD) | FB(F_LIFT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"electronic.dubtechno", 0.02f, L_CHH, 0.35f, L_LO, 0.20f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_LO)), 0.20f, 0.10f,
	 (uint16_t)(FB(F_NONE) | FB(F_DROP) | FB(F_ACCENT)), 8, (uint8_t)(laneBit(L_KICK))},
	{"electronic.minimal", 0.15f, L_HI, 0.70f, L_HI, 0.50f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.35f, 0.25f,
	 (uint16_t)(FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 8, (uint8_t)(laneBit(L_KICK))},
	{"electronic.ambient", 0.05f, L_CHH, 0.50f, L_HI, 0.25f,
	 (uint8_t)(laneBit(L_HI)), 0.50f, 0.20f,
	 (uint16_t)(FB(F_NONE) | FB(F_DROP) | FB(F_LIFT)), 8, 0},
	{"electronic.electro", 0.35f, L_CHH, 0.30f, L_CHH, 0.50f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_CHH) | laneBit(L_LO)), 0.40f, 0.05f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_ACCENT)), 4, 0},
	{"electronic.nudisco", 0.05f, L_CHH, 0.50f, L_CHH, 0.60f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI) | laneBit(L_BELL)), 0.45f, 0.35f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_LIFT) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"electronic.breakbeat", 0.30f, L_SNARE, 0.80f, L_SNARE, 0.50f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_HI) | laneBit(L_BELL)), 0.55f, 0.30f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_CASCADE) | FB(F_LIFT) | FB(F_ACCENT)), 4, 0},
	{"electronic.jungle", 0.50f, L_SNARE, 0.90f, L_SNARE, 0.65f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH) | laneBit(L_BELL)), 0.70f, 0.35f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 4, 0},
	{"electronic.dnb", 0.35f, L_SNARE, 0.75f, L_SNARE, 0.60f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH) | laneBit(L_HI)), 0.70f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_LIFT)), 2, 0},
	{"electronic.garage", 0.25f, L_CHH, 0.60f, L_CP, 0.60f,
	 (uint8_t)(laneBit(L_HI) | laneBit(L_CP)), 0.50f, 0.65f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 2, 0},
	{"electronic.footwork", 0.85f, L_KICK, 0.30f, L_CP, 0.60f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_LO) | laneBit(L_CP)), 0.60f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 4, 0},
	{"electronic.dubstep", 0.30f, L_CHH, 0.50f, L_CHH, 0.45f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI)), 0.40f, 0.30f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 4, 0},
	{"electronic.triphop", 0.10f, L_SNARE, 0.80f, L_SNARE, 0.35f,
	 (uint8_t)(laneBit(L_HI) | laneBit(L_BELL)), 0.50f, 0.35f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 4, 0},
	{"electronic.idm", 0.90f, L_CHH, 0.60f, L_SNARE, 0.85f,
	 0xFF, 0.95f, 0.30f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_CASCADE) | FB(F_LIFT) | FB(F_ACCENT)), 2, 0},
	{"electronic", 0.10f, L_CHH, 0.25f, L_CHH, 0.35f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI) | laneBit(L_CP)), 0.25f, 0.15f,
	 (uint16_t)(FB(F_DROP) | FB(F_BUILD) | FB(F_LIFT) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"rock.motorik", 0.00f, L_SNARE, 0.05f, L_SNARE, 0.05f,
	 (uint8_t)(laneBit(L_CHH)), 0.10f, 0.00f,
	 (uint16_t)(FB(F_NONE) | FB(F_ACCENT)), 1, 0},
	{"rock", 0.15f, L_SNARE, 0.25f, L_SNARE, 0.30f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_OHH) | laneBit(L_BELL)), 0.35f, 0.00f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"funk", 0.35f, L_CHH, 0.85f, L_SNARE, 0.50f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH) | laneBit(L_HI)), 0.70f, 0.20f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_BUILD) | FB(F_ACCENT)), 2, (uint8_t)(laneBit(L_KICK))},
	{"pop.motown", 0.05f, L_SNARE, 0.20f, L_SNARE, 0.35f,
	 (uint8_t)(laneBit(L_HI) | laneBit(L_CP)), 0.25f, 0.10f,
	 (uint16_t)(FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"pop", 0.10f, L_SNARE, 0.15f, L_SNARE, 0.35f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI) | laneBit(L_CP)), 0.35f, 0.05f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"disco", 0.05f, L_CHH, 0.20f, L_SNARE, 0.40f,
	 (uint8_t)(laneBit(L_OHH) | laneBit(L_HI) | laneBit(L_CP)), 0.20f, 0.05f,
	 (uint16_t)(FB(F_ROLL) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"rnb", 0.30f, L_CHH, 0.60f, L_SNARE, 0.45f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI) | laneBit(L_CP)), 0.55f, 0.28f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"jazz", 0.50f, L_SNARE, 0.70f, L_SNARE, 0.85f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_SNARE)), 0.85f, 0.62f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_BUILD) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"swing", 0.35f, L_SNARE, 0.40f, L_SNARE, 0.50f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_SNARE)), 0.50f, 0.60f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"blues", 0.20f, L_SNARE, 0.45f, L_SNARE, 0.30f,
	 (uint8_t)(laneBit(L_CHH)), 0.40f, 0.62f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"shuffle", 0.30f, L_SNARE, 0.80f, L_SNARE, 0.35f,
	 (uint8_t)(laneBit(L_CHH)), 0.40f, 0.65f,
	 (uint16_t)(FB(F_ROLL) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"boogie", 0.20f, L_SNARE, 0.40f, L_SNARE, 0.25f,
	 (uint8_t)(laneBit(L_CHH)), 0.25f, 0.65f,
	 (uint16_t)(FB(F_ROLL) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"ballad", 0.15f, L_SNARE, 0.30f, L_SNARE, 0.25f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_LO)), 0.35f, 0.20f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_LIFT) | FB(F_ACCENT)), 4, 0},
	{"slow", 0.15f, L_SNARE, 0.35f, L_SNARE, 0.25f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_LO)), 0.30f, 0.55f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_ACCENT)), 4, 0},
	{"twist", 0.05f, L_SNARE, 0.15f, L_SNARE, 0.20f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI)), 0.15f, 0.10f,
	 (uint16_t)(FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"waltz", 0.10f, L_SNARE, 0.20f, L_SNARE, 0.20f,
	 (uint8_t)(laneBit(L_CHH)), 0.20f, 0.00f,
	 (uint16_t)(FB(F_ROLL) | FB(F_BUILD) | FB(F_ACCENT)), 3, 0},
	{"march", 0.70f, L_SNARE, 0.30f, L_SNARE, 0.30f,
	 (uint8_t)(laneBit(L_SNARE)), 0.20f, 0.00f,
	 (uint16_t)(FB(F_ROLL) | FB(F_BUILD) | FB(F_ACCENT)), 4, 0},
	{"charleston", 0.60f, L_SNARE, 0.30f, L_SNARE, 0.40f,
	 (uint8_t)(laneBit(L_OHH) | laneBit(L_HI)), 0.45f, 0.58f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 2, 0},
	{"endings", 0.20f, L_SNARE, 0.30f, L_SNARE, 0.15f,
	 (uint8_t)(laneBit(L_CHH)), 0.15f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_ACCENT)), 2, 0},
	{"latin.bossa", 0.05f, L_HI, 0.40f, L_KICK, 0.15f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_CHH)), 0.25f, 0.15f,
	 (uint16_t)(FB(F_NONE) | FB(F_ACCENT)), 2, (uint8_t)(laneBit(L_CP) | laneBit(L_BELL))},
	{"latin.samba", 0.25f, L_HI, 0.65f, L_SNARE, 0.45f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_HI)), 0.45f, 0.25f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_CP) | laneBit(L_BELL))},
	{"latin.dembow", 0.35f, L_CHH, 0.20f, L_CHH, 0.35f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI) | laneBit(L_CP)), 0.15f, 0.05f,
	 (uint16_t)(FB(F_STUTTER) | FB(F_DROP) | FB(F_BUILD) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK) | laneBit(L_SNARE))},
	{"latin.songo", 0.30f, L_HI, 0.60f, L_SNARE, 0.55f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_SNARE) | laneBit(L_HI)), 0.65f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_BUILD) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_CP) | laneBit(L_BELL))},
	{"latin", 0.15f, L_HI, 0.45f, L_LO, 0.30f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.32f, 0.12f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 2, (uint8_t)(laneBit(L_CP) | laneBit(L_BELL))},
	{"caribbean.reggae", 0.05f, L_CHH, 0.45f, L_SNARE, 0.10f,
	 (uint8_t)(laneBit(L_CHH)), 0.20f, 0.20f,
	 (uint16_t)(FB(F_NONE) | FB(F_DROP) | FB(F_ACCENT)), 2, (uint8_t)(laneBit(L_CP))},
	{"caribbean.dancehall", 0.55f, L_CHH, 0.25f, L_CHH, 0.45f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI) | laneBit(L_CP)), 0.30f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 2, 0},
	{"caribbean.soca", 0.40f, L_SNARE, 0.35f, L_HI, 0.50f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH) | laneBit(L_HI)), 0.40f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_BUILD) | FB(F_LIFT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"caribbean.ska", 0.20f, L_SNARE, 0.35f, L_SNARE, 0.30f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH)), 0.35f, 0.35f,
	 (uint16_t)(FB(F_ROLL) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"caribbean", 0.12f, L_CHH, 0.42f, L_SNARE, 0.22f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH)), 0.26f, 0.18f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 2, (uint8_t)(laneBit(L_CP) | laneBit(L_BELL))},
	{"africa.afrobeat", 0.25f, L_CHH, 0.75f, L_SNARE, 0.60f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_SNARE) | laneBit(L_CHH)), 0.70f, 0.20f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_CP) | laneBit(L_BELL))},
	{"africa.amapiano", 0.45f, L_HI, 0.30f, L_CHH, 0.45f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_HI) | laneBit(L_CP)), 0.35f, 0.25f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_LIFT) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"africa.gqom", 0.50f, L_LO, 0.20f, L_CHH, 0.45f,
	 (uint8_t)(laneBit(L_CHH) | laneBit(L_LO) | laneBit(L_CP)), 0.40f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_CASCADE)), 4, (uint8_t)(laneBit(L_KICK))},
	{"africa.highlife", 0.15f, L_HI, 0.45f, L_SNARE, 0.40f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH) | laneBit(L_LO)), 0.50f, 0.20f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"africa.kuku", 0.35f, L_LO, 0.50f, L_LO, 0.45f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.60f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_LO) | laneBit(L_BELL))},
	{"africa.soukous", 0.30f, L_SNARE, 0.50f, L_SNARE, 0.45f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CHH) | laneBit(L_HI)), 0.50f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"africa", 0.22f, L_LO, 0.52f, L_LO, 0.38f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.48f, 0.15f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"mena.saidi", 0.50f, L_HI, 0.50f, L_HI, 0.45f,
	 (uint8_t)(laneBit(L_HI)), 0.25f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"mena", 0.55f, L_HI, 0.55f, L_HI, 0.50f,
	 (uint8_t)(laneBit(L_HI)), 0.30f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_KICK))},
	{"europe.bulerias", 0.80f, L_CP, 0.70f, L_CP, 0.70f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CP)), 0.60f, 0.22f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_ACCENT)), 6, (uint8_t)(laneBit(L_BELL))},
	{"europe.jig", 0.80f, L_LO, 0.55f, L_LO, 0.50f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.40f, 0.00f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_ACCENT)), 2, 0},
	{"europe.pasodoble", 0.55f, L_SNARE, 0.25f, L_SNARE, 0.30f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_CP)), 0.30f, 0.00f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_BUILD) | FB(F_ACCENT)), 4, 0},
	{"europe.racenitsa", 0.50f, L_HI, 0.40f, L_LO, 0.35f,
	 (uint8_t)(laneBit(L_HI)), 0.30f, 0.00f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_ACCENT)), 3, 0},
	{"europe", 0.45f, L_HI, 0.42f, L_LO, 0.35f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.35f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_ACCENT)), 2, (uint8_t)(laneBit(L_BELL))},
	{"asia.kotekan", 0.05f, L_LO, 0.05f, L_LO, 0.03f,
	 0, 0.10f, 0.00f,
	 (uint16_t)(FB(F_ACCENT)), 1, (uint8_t)(laneBit(L_HI) | laneBit(L_CP) | laneBit(L_BELL))},
	{"asia.lancaran", 0.05f, L_LO, 0.05f, L_LO, 0.05f,
	 (uint8_t)(laneBit(L_LO)), 0.12f, 0.00f,
	 (uint16_t)(FB(F_NONE) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_HI) | laneBit(L_CP) | laneBit(L_BELL))},
	{"asia.taiko", 0.70f, L_LO, 0.20f, L_HI, 0.20f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.25f, 0.05f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_ACCENT)), 4, 0},
	{"asia.bhangra", 0.75f, L_HI, 0.35f, L_HI, 0.55f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.60f, 0.10f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_BUILD) | FB(F_ACCENT)), 4, 0},
	{"asia", 0.55f, L_LO, 0.45f, L_LO, 0.35f,
	 (uint8_t)(laneBit(L_LO) | laneBit(L_HI)), 0.42f, 0.20f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"indian", 0.85f, L_SNARE, 0.70f, L_LO, 0.50f,
	 (uint8_t)(laneBit(L_SNARE) | laneBit(L_HI)), 0.42f, 0.08f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_BUILD) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_BELL))},
	{"oceania", 0.85f, L_HI, 0.20f, L_HI, 0.25f,
	 (uint8_t)(laneBit(L_HI)), 0.30f, 0.00f,
	 (uint16_t)(FB(F_ROLL) | FB(F_DROP) | FB(F_CASCADE) | FB(F_ACCENT)), 4, 0},
	{"usa", 0.70f, L_SNARE, 0.95f, L_SNARE, 0.85f,
	 (uint8_t)(laneBit(L_KICK) | laneBit(L_SNARE) | laneBit(L_CHH)), 0.90f, 0.45f,
	 (uint16_t)(FB(F_ROLL) | FB(F_STUTTER) | FB(F_DROP) | FB(F_CASCADE) | FB(F_BUILD) | FB(F_ACCENT)), 2, 0},
	{"gamelan", 0.05f, L_LO, 0.05f, L_LO, 0.05f,
	 (uint8_t)(laneBit(L_LO)), 0.12f, 0.00f,
	 (uint16_t)(FB(F_NONE) | FB(F_ACCENT)), 4, (uint8_t)(laneBit(L_HI) | laneBit(L_CP) | laneBit(L_BELL))},
	{"euclidean", 0.00f, L_CHH, 0.00f, L_SNARE, 0.05f,
	 0, 0.05f, 0.00f,
	 (uint16_t)(FB(F_NONE)), 2, 0},
	{"timeline", 0.00f, L_KICK, 0.00f, L_KICK, 0.00f,
	 0, 0.00f, 0.00f,
	 (uint16_t)(FB(F_NONE)), 1, 0xFF},
};
#undef FB

static const int NUM_DRUM_TASTE = (int)(sizeof(DRUM_TASTE) / sizeof(DRUM_TASTE[0]));

// Regional traditions carry their own figures and should not be ratcheted or
// padded; they get a deliberately quiet default rather than a table row each.
static inline bool traditionalFamily(const std::string& f) {
	static const char* T[] = {"latin", "caribbean", "africa", "mena", "europe", "asia",
	                          "indian", "oceania", "usa", "gamelan", "euclidean"};
	for (const char* t : T) if (f == t) return true;
	return false;
}

static inline Taste tasteFor(const std::string& family, const std::string& sub) {
	std::string full = sub.empty() ? family : family + "." + sub;
	for (int i = 0; i < NUM_DRUM_TASTE; i++) if (full == DRUM_TASTE[i].key) return DRUM_TASTE[i];
	for (int i = 0; i < NUM_DRUM_TASTE; i++) if (family == DRUM_TASTE[i].key) return DRUM_TASTE[i];
	if (traditionalFamily(family))
		return Taste{"traditional", 0.03f, L_HI, 0.35f, L_SNARE, 0.15f,
		             (uint8_t)(laneBit(L_HI) | laneBit(L_LO)), 0.20f, 0.f,
		             (uint16_t)((1u << F_NONE) | (1u << F_ACCENT)), 2,
	             (uint8_t)(laneBit(L_BELL) | laneBit(L_CP))};
	return Taste{"default", 0.12f, L_CHH, 0.45f, L_SNARE, 0.35f,
	             (uint8_t)(laneBit(L_CHH) | laneBit(L_HI)), 0.40f, 0.2f,
	             (uint16_t)((1u << F_ROLL) | (1u << F_ACCENT) | (1u << F_DROP)), 2, 0};
}

static inline bool allows(const Taste& t, FillShape s) { return (t.fills & (1u << s)) != 0; }
static inline bool frozen(const Taste& t, int lane) { return (t.frozenLanes & laneBit(lane)) != 0; }

// Ratchet density and structural freedom are ORTHOGONAL, and the engine must
// not derive one from the other. Taiko, Tahitian 'ote'a and teental all pair
// near-maximal roll density with near-minimal freedom to restructure; New
// Orleans second line has both; motorik and gamelan have neither. Deriving
// ornament density from `vary` gets every one of those wrong.

}  // namespace sfs
