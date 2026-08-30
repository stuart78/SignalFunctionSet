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
| 0 | the scale index, 1V per scale — exactly as before. Its **fractional part** carries how many degrees did not fit (see *What was cut*) |
| 1 | the period, in volts (12 semitones = 1V) |
| 2 … n+1 | the n degrees, as 1V/oct offsets from the root; channel 2 is always 0 |

`Port::getVoltage()` returns channel 0 whatever the channel count, so **the
extra channels are invisible to a module that only wants an index, and change
nothing for it.** A module that wants the real scale reads the further channels
instead.

### Use `scale-bus.hpp` — do not re-implement it

The format, one encoder and one **validated** decoder live in
`src/scale-bus.hpp`. It began inside `key.cpp`, which was fine while Key was the
only module that spoke it; the moment Note needed it too, a second copy of a
wire format would have been a bug waiting for someone to fix only one of them.

A consumer needs three lines:

```cpp
#include "scale-bus.hpp"
sfs::BusScale scale;                                    // a member
scale = sfs::busResolve(inputs[SCALE_INPUT], knobIndex); // once per block
```

`BusScale` names its fields `intervals[]` and `size` **deliberately, to match
`sfs::Scale`**, so existing code that reads `sc.intervals[k]` and `sc.size` keeps
working and only the line the scale *comes from* has to change.

Then two rules:

* **Wrap by `scale.period`, never by 12.** `sc.intervals[d] + 12.f * oct` is the
  single most common way a module silently forces a scale back onto the octave.
  Bohlen-Pierce repeats at 19.02 semitones.
* **Do not round a degree to a whole semitone.** Pelog, Slendro and the harmonic
  series carry fractional intervals before Scala is even involved; Chance was
  quietly flattening all three onto 12-TET with an `lround` long before any of
  this existed.

A module that also has a SCALE **output** should relay the whole bus with
`sfs::busScaleToOutput`, not just the index. Relaying only the index makes that
module the point where every scale an index cannot name dies.

Where the key cannot be named by an index — a custom mask, or Scala — channel 0
carries the canonical scale sharing the most pitch classes with it, so an
index-only consumer lands on a near neighbour rather than something unrelated.
(Pelog, for instance, resolves to Phrygian, which is exactly its rounded
pitch-class set.)

Limits: 14 degrees, since 16 channels less the index and the period. That covers
every canonical scale and most Scala files; a 31-note Scala scale is truncated.

### What was cut

A consumer that is handed fourteen degrees cannot tell a scale that genuinely
has fourteen from a 31-note scale cut down to fourteen — and that is exactly
what its reader needs to know. So `BusScale::dropped` carries the count, and
the producer sets it (Key: `parent.n - b.size`).

There is no channel left to put it in: a fourteen-degree scale already fills all
sixteen. It rides in the **fractional part of channel 0** instead. Channel 0 is
defined as "1V per scale", so every index-only consumer rounds it, and the
fraction is spare by the definition of the format — invisible to them, free to
us. `busScaleToOutput` writes `index + dropped/128`; `busScaleFromInput` takes
the integer as the index and the remainder as the count. A count rather than a
flag, because "14 of 31" tells the reader something that "14, and some were
lost" does not.

### Naming a scale on screen

`BusScale::canonical()` asks whether the index actually **names** this scale, by
comparing the degrees with `SCALES[index]`. Do not use `extended` for this: it
says the scale arrived over the wire, and a plain Major relayed by Key arrives
over the wire too.

Once the key comes off the bus, channel 0 is only ever a nearest neighbour, so
a display that reads `SCALES[index].longName` announces **"Chromatic" over a
Bohlen-Pierce scale** — confidently, in the one place a player looks to find out
what key they are in. Note's status cell and Chance's key readout both did.

Use the shared formatters so every module says the same thing:

| | canonical | microtonal / custom | cut |
|---|---|---|---|
| `label()` (a status cell) | `Major` | `13 deg` | `14/31` |
| `describe()` (a readout or menu) | `Major` | `13 degrees, period 19.02 st` | `14 of 31 degrees (cut to the 14-degree bus limit)` |

Flag a cut scale in the display's warning colour: the degrees shown are a
prefix of the real scale, not the whole of it. Key says the same thing at the
source, in the Scala section of its menu, since Key is where the cut happens.

**Receiving:** validate before trusting. A 16-channel pitch cable patched into a
SCALE input would otherwise be read as a scale. Require an ascending degree list
starting at 0, all degrees inside the period, and a period between 1 and 48
semitones; anything else falls back to plain index behaviour.

## Audit: who speaks the protocol (2026-08)

| module | canonical index | reads the bus | SCALE out | relays the bus |
|---|---|---|---|---|
| Note | yes | yes | yes | yes |
| Key | yes | yes (`busScaleFromInput`) | yes | yes |
| Chance | yes | yes | — | — |
| Fugue | yes | yes | — | — |
| MetaFugue | yes | yes | — | — |
| Muse | yes | yes | — | — |
| Loom | yes | yes | — | — |
| Slide | yes | yes | — | — |
| Chime | yes | yes | — | — |
| Arrange | yes | no input | yes | index only |
| Play, Record | yes | **no SCALE input** | — | — |
| Cycle | n/a | n/a | — | — |

Notes on the three that are not a plain "yes":

* **Arrange** emits a bare index because a bare index is all it has: its scale
  comes from a trimpot per phrase. A consumer reading it with `busResolve` falls
  back to index behaviour correctly, so this works — it simply cannot carry a
  scale that an index cannot name. If Arrange ever gains a scale input, it
  should relay with `busScaleToOutput`.
* **Play** and **Record** use a scale for one thing only: the **pad grid**
  (`pushgrid.hpp`) — which notes the In-Key layout puts on the pads, and which
  pads get the in-scale highlight in the chromatic layout. It never quantises
  anything, never touches incoming V/OCT, and never reaches audio.

  `gridNoteAt()` returns an **int MIDI note**, so the grid is 12-TET by
  construction rather than by oversight: a pad has to send a note, and a
  multisample library is indexed by note. Its `lround` on a degree and its
  `* 12` per octave are therefore correct here, and are the one place those two
  patterns are not the bug they are everywhere else.

  A SCALE input would let the pads follow the patch key, which is worth
  something — but it could only ever read channel 0, so it would carry the KEY
  and not the TUNING. Deliberately not added: the value did not justify the
  jack.
* **Cycle**'s `SCALE` is an LFO depth, not a musical scale. It shows up in any
  grep for `SCALE_INPUT` and is not part of this convention.

Two bug classes were found by this audit and fixed, both of which predate the
bus and were wrong for shipped canonical scales:

* **`intervals[d] + 12 * oct`** in Loom, Slide, Chime, Fugue and MetaFugue —
  wrapping at the octave rather than the scale's own period.
* **Rounding a degree to a whole semitone**, in Chance's pitch path and in
  Chime's and Chance's note read-outs. That flattened Pelog, Slendro and the
  harmonic series onto 12-TET with no Scala involved at all.

A display that reads the canonical table while the pitch comes off the bus will
silently disagree with itself. Chime's note label and Chance's octave axis both
did; both now take the module's resolved scale.
