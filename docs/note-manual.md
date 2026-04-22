# Note Manual

## Overview

Note is the pitched (CV/gate) cousin of Beat. It's a monophonic pattern sequencer with on-screen pitch matrix editing — eight patterns × eight steps each — quantized to a selectable scale and root. The architecture mirrors Beat for clock/bar/repeat behavior, so a single Meter can drive a whole rig of Beats and Notes in lockstep.

Note includes a unique just-intonation **Harmonic series** scale and two **Gamelan** scales (Slendro, Pelog) that produce non-12-TET pitches you can't get from a chromatic quantizer.

Note is 10HP.

## Concepts

### Patterns and Steps

Each Note instance holds **8 patterns × 8 steps**. Each step has five properties:

- **Pitch** (-1 = rest, otherwise an index into the current scale's note list)
- **Velocity** (0..1)
- **Accent** (boolean)
- **Probability** (0..1, chance the step actually fires when reached)
- **Active** (per-pattern flag for inclusion in the rotation)

### Pattern Length and Repeats

Same as Beat: each pattern has its own length (1–8) and repeat count (1–8 bars). On BAR pulse, Beat increments its bar counter; once it exceeds the current pattern's repeats, advances to the next active pattern.

### Root, Scale, Octave

- **Root**: 0–11 semitones (C through B), set with a trimpot + summed CV input. ROOT CV is 1V/oct, semitone-quantized.
- **Scale**: one of 17 scales (see list below), set with a trimpot + summed CV input. SCALE CV is 1V per scale.
- **Octave**: -4 to +4 octave shift, set with a trimpot + summed CV input. OCT CV is 1V/oct.

ROOT, SCALE, OCT all have **OUT jacks** that relay the current values, so multiple Notes can share root/scale by chaining one's OUT into the next one's CV in.

### Pitch Matrix

The display matrix is 8 columns (steps) × 13 rows (pitches). Each scale's notes are shown bottom-up:
- **Bottom row** = the scale's root pitch (degree 0)
- **Above** = each successive scale degree
- **Top row** for each scale = root + 1 octave (always shown so scale octave is visible regardless of scale size)
- **Rows above the scale** = out-of-scale, dim, not clickable

For a 7-note scale (Major, Minor, Dorian, etc.) you'll see 7 + 1 = 8 rows lit in the matrix. Pentatonics show 5 + 1 = 6 rows. Chromatic and Harmonic series show all 12 + 1 = 13 rows.

### Available Scales

| # | Display | Description |
|---|---|---|
| 0 | Chromatic | All 12 semitones |
| 1 | Major | 0,2,4,5,7,9,11 |
| 2 | Minor | Natural minor, 0,2,3,5,7,8,10 |
| 3 | Penta+ | Major pentatonic, 0,2,4,7,9 |
| 4 | Penta- | Minor pentatonic, 0,3,5,7,10 |
| 5 | Blues | 0,3,5,6,7,10 |
| 6 | Whole | Whole-tone, 0,2,4,6,8,10 |
| 7 | Harmonic | Just-intonation harmonics 1..12 — non-12-TET |
| 8 | Dorian | 0,2,3,5,7,9,10 — minor with raised 6th |
| 9 | Phrygian | 0,1,3,5,7,8,10 — minor with b2 |
| 10 | Lydian | 0,2,4,6,7,9,11 — major with #4 |
| 11 | Mixolyd | 0,2,4,5,7,9,10 — major with b7 |
| 12 | HarmMin | 0,2,3,5,7,8,11 — harmonic minor |
| 13 | Hijaz | 0,1,4,5,7,8,10 — Arabic / Klezmer phrygian dominant |
| 14 | Hirajoshi | 0,2,3,7,8 — Japanese pentatonic |
| 15 | Pelog | 7-tone Indonesian Gamelan, Surakarta-style: 0, 1.2, 2.7, 5.4, 7.0, 8.0, 10.4 — non-12-TET |
| 16 | Slendro | 5-equal Indonesian Gamelan: 0, 2.4, 4.8, 7.2, 9.6 — non-12-TET |

The Harmonic, Pelog, and Slendro scales use non-integer semitones, producing pitches that don't fall on the standard 12-TET grid. They'll sound noticeably "off-grid" against equal-tempered instruments — by design.

### Bar / Clock Coincidence Handling

Same as Beat: collapses BAR + downbeat-CLOCK into a single event so the bar boundary fires step 0 once, not twice.

### "Advance only on bar trigger"

Default ON. Pattern advance only happens on BAR pulse. With BAR not patched, the pattern just loops the same one indefinitely.

## Display

Top to bottom:

1. **Mode tabs**: `STEPS · VEL · ACC · PROB`
2. **Top connector rail** + stem from active mode tab
3. **Pitch matrix** (13 rows × 8 cols) — see *Pitch Matrix* above
4. **Length dots** (8 wide bars, one per step, lit if `i < length`)
5. **PATTERN label** (left) and **Root / Scale / Octave status** (right) — three small dark cells on the same baseline showing the current ROOT (e.g. `C`), SCALE (e.g. `Harmonic`), and OCT (e.g. `+1`)
6. **Pattern selector**: 8 cells with pattern numbers + repeat-count dot row (filled per repeat, bright dot indicates current bar in the loop on the playing pattern)
7. **Bottom connector rail** + stem from edit pattern
8. **Repeats bar**: 8 cells

## Edit Mode Behaviors

**STEPS mode**: Click a cell in the matrix to set that step's pitch to the clicked row. Click the same cell again to clear (rest). Drag-paint paints the same row across columns.

**VEL mode**: The matrix transforms into 8 vertical column faders. Each column shows:
- Background: dim purple (full column height)
- Bar: blue from bottom up to velocity × column height (orange on the currently-playing column)
- Pitch indicator: thin white horizontal line at the lit pitch row (so you still see what note the step plays)
- Click anywhere in a column sets velocity by Y position; vertical drag continues to adjust
- Pitch is **not auto-set** in VEL — switch to STEPS to add notes

**ACC mode**: Click toggles the accent flag on the lit pitch (auto-sets the pitch to the clicked row if the step was a rest). Drag paints. Accent shows as a white circle around the lit cell.

**PROB mode**: Same column-fader UI as VEL, but writes to the step's probability.

## Length Dots / Pattern Selector / Repeats Bar

All same as Beat:
- Click length dot N → set length to N+1; drag to scrub
- Click pattern cell → select for editing; drag to scrub; double-click to toggle active; scroll wheel adjusts that pattern's repeats
- Click repeats cell N → set repeats to N+1; drag to scrub

## Controls

| Control | Type | Range | Default | Function |
|---------|------|-------|---------|----------|
| **ROOT** | Trimpot (snap) | 0–11 | 0 (C) | Root note in semitones |
| **SCL** | Trimpot (snap) | 0–16 | 0 (Chromatic) | Scale select |
| **OCT** | Trimpot (snap) | -4 to +4 | 0 | Octave shift |

All three are summed with their CV inputs (knob + CV, clamped to range).

## Inputs

| Input | Function |
|-------|----------|
| **CLOCK** | Advances the step counter |
| **BAR** | Advances to the next active pattern |
| **RESET** | Returns to first active pattern, step 0 |
| **ROOT CV** | 1V/oct semitone offset, sums with ROOT trimpot |
| **SCALE CV** | 1V per scale index, sums with SCL trimpot |
| **OCT CV** | 1V/oct shift, sums with OCT trimpot |

## Outputs

| Output | Function |
|--------|----------|
| **GATE** | 1ms 10V pulse on each fired step |
| **V/OCT** | 1V/oct pitch CV (sample-and-hold; previous note's pitch stays held until next fire) |
| **VEL** | 0..10V velocity CV (S&H) |
| **ACC** | 1ms 10V pulse on accented hits |
| **ROOT** | Relays current root as `rootNote / 12` V (1V/oct compatible) |
| **SCALE** | Relays current scale as `scaleIndex` V (1V per scale) |

## Context Menu

- **Advance only on bar trigger** (default ON)
- **Patterns**:
  - **Randomize current pattern (notes only)** — randomizes pitches at ~60% density, leaves velocity/accent/probability alone
  - **Clear current pattern**
  - **Clear all patterns**

## Patch Ideas

**Bass + arp from one Meter**: One Meter clocks two Notes. Note 1 (bass) on a 5-note pentatonic, low octave, 4-step pattern; Note 2 (arp) on the same scale, +1 octave, 8-step pattern with all-on steps. Patch Note 1's ROOT and SCALE OUT into Note 2's ROOT and SCALE CV — now changing the bass key transposes the arp automatically.

**Gamelan ensemble**: 4 × Note instances on Slendro scale, all sharing root via SCALE OUT chaining. Each at a different octave (-1, 0, +1, +2). Different pattern lengths (5, 7, 8, 11) for shifting polyrhythmic feel. Patch each into a Tine for metallic resonator timbre — instant inharmonic gamelan.

**Just-intonation drone**: Set scale = Harmonic, pattern = single sustained note on row 0 (root), velocity = max. The V/OCT output gives precise just-intonation harmonics 1..12. Step through them one at a time as a sweep through the harmonic series.

**Live retune**: Patch an LFO into SCALE CV scaled to step through scale indices 0–6 every few bars. Chord shifts happen on bar boundaries since the V/OCT only updates on step fire.
