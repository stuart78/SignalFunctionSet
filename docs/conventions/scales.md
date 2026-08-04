# Canonical scale list

All modules that expose a SCALE control share **one** scale list, defined in
`src/scales.hpp` (namespace `sfs`). This guarantees that a SCALE CV value
(1V per scale step) selects the *same* scale on every module — you can patch
one SCALE sequence to Note, Fugue, MetaFugue, Muse, Chance, and Arrange at once and
they stay in agreement.

## The rule

- **Never reorder or insert** scales. The list is **append-only**. Reordering
  breaks cross-module CV compatibility and every saved patch.
- New scales go at the end, taking the next index.
- All three consumers (`note.cpp`, `fugue.cpp`, `muse.cpp`) `#include "scales.hpp"`
  and alias the shared `sfs::Scale` struct + `sfs::SCALES[]` array. No module
  keeps its own table.

## The 19 scales (as of v2.11)

```
0  Chromatic           7  Harmonic series   14  Hirajoshi (Japanese)
1  Major               8  Dorian            15  Pelog (Gamelan, 7-tone)
2  Minor               9  Phrygian          16  Slendro (Gamelan, 5-equal)
3  Pentatonic Major   10  Lydian            17  Melodic Minor
4  Pentatonic Minor   11  Mixolydian        18  Locrian
5  Blues              12  Harmonic Minor
6  Whole tone         13  Hijaz (Arabic)
```

## Struct fields

```cpp
struct Scale {
    const char* longName;   // tooltips / context menus / configSwitch labels
    const char* shortName;  // compact display (Note's matrix status cell)
    const char* museName;   // Muse's label (== longName except index 0)
    const float* intervals; // variable-length semitone offsets (Note + Fugue)
    int size;               // number of intervals
    float museSemis[8];     // flattened 8-slot table (Muse only)
};
```

## Why three name fields

- **longName** is the canonical full name. Use it everywhere there's room.
- **shortName** exists only because Note's on-screen pitch matrix has a tiny
  status cell (e.g. `Penta+`, `HarmMin`). Display-only.
- **museName** is identical to longName except **index 0**, which reads
  `"Chromatic-ish"` on Muse. Muse's pitch engine is a 3-bit index into 8 slots,
  so it can only reach 8 of the 12 chromatic steps — the name flags that.

## Why Muse needs `museSemis[8]`

Note and Fugue use variable-length scales (5/6/7/12 notes) and handle octave
wrapping in DSP. Muse is different: it reads a fixed **8-slot** table directly
via a 3-bit pitch index, with a 4th bit (D) adding a +12 octave on top. So every
scale is pre-flattened to exactly 8 ascending degrees:

- **7-note scales** → degrees 1–7 then 13 (the 6th up an octave), so the 8th
  slot is distinct from "slot 0 + octave bit": `{d0..d6, intervals[5]+12}`. This
  gives bit D a clean octave to stack on.
- **other sizes** → first 8 consecutive degrees, wrapping +12 per octave:
  `museSemis[i] = intervals[i % size] + 12*(i / size)`.

This keeps the pre-existing pentatonic Muse tables unchanged. The 12-note scales
(Chromatic, Harmonic series) can only surface their first 8 degrees on Muse —
accepted limitation of a 3-bit index, hence "Chromatic-ish".

## Adding a scale

1. Append an interval array `SCL_NEWSCALE[]` in `scales.hpp`.
2. Append one `Scale{}` row at the **end** of `SCALES[]` with all fields,
   including a `museSemis[8]` computed by the rule above.
3. That's it — all three modules pick it up automatically (their configSwitch
   labels and CV ranges are generated from `sfs::NUM_SCALES`).

A standalone check of the `museSemis` derivation lives in the commit history
(compile `scales.hpp` against the rule and assert equality for all entries).

## The scale bus (SCALE CV, extended)

`SCALE` CV has always been **1V per scale** — an index into the canonical list.
That is enough for the 19 scales here and nothing else: it cannot express a
custom note mask, a loaded Scala file, or a scale whose period is not an octave.

**Key** extends it without breaking it. Its `SCALE` output is *polyphonic*:

| channel | carries |
|---|---|
| 0 | the scale index, 1V per scale — exactly as before |
| 1 | the period, in volts (12 semitones = 1V) |
| 2 … n+1 | the n degrees, as 1V/oct offsets from the root; channel 2 is always 0 |

`Port::getVoltage()` returns channel 0 whatever the channel count, and every
existing consumer (Note, Fugue, MetaFugue, Muse, Chime) calls exactly that — so
**the extra channels are invisible to them and change nothing.** A module that
wants the real scale reads the further channels instead.

Where the key cannot be named by an index — a custom mask, or Scala — channel 0
carries the canonical scale sharing the most pitch classes with it, so an
index-only consumer lands on a near neighbour rather than something unrelated.
(Pelog, for instance, resolves to Phrygian, which is exactly its rounded
pitch-class set.)

Limits: 14 degrees, since 16 channels less the index and the period. That covers
every canonical scale and most Scala files; a 31-note Scala scale is truncated.

**Receiving:** validate before trusting. A 16-channel pitch cable patched into a
SCALE input would otherwise be read as a scale. Require an ascending degree list
starting at 0, all degrees inside the period, and a period between 1 and 48
semitones; anything else falls back to plain index behaviour.
