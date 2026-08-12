# Key Manual

## Overview

Most quantizers are islands. You set a scale on the front panel, and then you
keep every other module in the patch in step with it by hand. Change the key
and you go round the rack changing it everywhere.

Key reads **ROOT** and **SCALE** as control voltages, in the same convention
Arrange, Note, Chance, Muse, Fugue, Chime, Loom and Slide already use, and hands
the same key back out of its own ROOT and SCALE outputs. One key change moves
the whole patch.

Four channels, each polyphonic to 16 voices, all sharing that key. 14HP.

## The convention

- **ROOT** is 1V/oct, quantized to the nearest semitone. 0V is C.
- **SCALE** is 1V per scale, indexing the canonical list in
  [scales.md](conventions/scales.md).

Any module in the plugin with a ROOT or SCALE input speaks this, so Key's
outputs drive all of them, and any of their outputs drive Key.

## Channels

Four identical channels, side by side. Each has:

| Control / jack | What it does |
|---|---|
| **IN** | Pitch in, 1V/oct, polyphonic to 16 channels. |
| **SUB** | Which scale this channel quantizes to: the full scale (**M**), or sub-scale **1**, **2** or **3**. |
| **OFF** | An offset, applied after quantizing. |
| **OUT** | Quantized pitch out, same channel count as the input. |

### OFFSET moves in scale degrees

By default **+2 means two steps up the scale**, not two semitones. In C major,
+2 on a C gives you E. In C minor it gives you E♭. The note stays in key, which
is almost always what you wanted when you reached for an offset on a quantizer.

Semitones are available in the context menu if you need literal transposition.

## Sub-scales

Three of them, edited on screen, and they are the reason Key is more than a
quantizer with four channels.

A sub-scale is a **mask over the parent scale's degree indices**, never over
absolute pitches. Select degrees 1, 3 and 5 and you have a triad: C E G in major,
C D♯ G in minor, D♯ G A♯ in E♭. The selection survives a change of scale and a
change of root, because what you selected was *a role in the key*, not a set of
notes.

That is what makes them usable live. Point channel 1 at the full scale for a
melody, channel 2 at a triad for a pad and channel 3 at root-and-fifth for a
bass, then move ROOT and SCALE around: all three keep doing their job.

An empty sub-scale falls back to the parent rather than going silent.

### Letting a sub-scale leave the key

The context menu has **Sub-scales may leave the key**, off by default. Turned
on, the out-of-scale cells become clickable, so a sub-scale can carry an
accidental: a flat 5th under a walking line, a passing tone the rest of the
patch never sees.

Those picks cannot be degree indices, because a note chosen for being outside
the scale has no degree. They are stored as **semitones from the root** instead,
which means they transpose with ROOT and stay put when SCALE changes. A ring
around the cell marks one on screen, so an accidental reads as an accidental
rather than looking like a degree.

The option is inert on a scale that is not 12-tone, where there is no chromatic
to pick from.

## The screen has two states

Key shows one or the other, chosen by the scale itself.

**The keyboard**, when the period is 12 semitones and every degree lands within
a cent of one. Five circles above, seven below, on the piano's own layout, with
the gap where there is no black key between E and F. It is drawn **from C
whatever the root is**, because you read a keyboard by its shape; the root is
marked in place with an inset dot instead.

Under it, the three sub-scale rows show all twelve chromatic tones, each aligned
under its own note on the keyboard above.

**The region strip**, otherwise. Harmonic series, Pelog, Slendro and every Scala
file get this one. It is linear in pitch across one period, with a line at each
degree, so **the gaps between the lines are the snap regions**. You can see
that Pelog's second degree sits at 1.20 semitones, just above C♯, and how wide
the band of input pitches that lands on it actually is. A keyboard can only
round that; the strip shows it.

In this state the sub-scale rows show one cell per degree, since there is no
chromatic to align to.

Both states carry the same header (root and scale name) and the same footer:
four cells, one per channel, showing which scale it is using and what note it is
currently putting out.

### Editing

- **Click a key** to fork a custom scale of your own. The first edit copies the
  current scale and then you are editing your own mask.
- **Click a sub-scale cell** to add or remove that degree.

## Nothing here is a 12-bit pitch mask

Quantizers are usually built on twelve bits, one per semitone. Key is not, and
it matters.

Three of the canonical scales (**Harmonic series, Pelog and Slendro**) have
fractional semitone intervals. A twelve-bit mask would silently round them onto
the chromatic grid and destroy exactly the thing that makes them worth having.
Key works in *semitones within one period* against real floating-point
intervals, so Pelog stays 0 / 1.20 / 2.70 / 5.40 / 7.00 / 8.00 / 10.40.

## Scala files

Load a `.scl` file from the context menu. It occupies SCALE index 19, leaving
0–18 as exactly the canonical list, so cross-module SCALE CV is unaffected.

- Cents and ratios both parse.
- **The period need not be an octave.** Bohlen-Pierce arrives as 13 degrees
  repeating at 19.02 semitones, a 3/1, and Key quantizes to it correctly.
- The parsed scale is saved into the patch alongside the path, so it survives
  the file being moved or the patch being opened on another machine.

## The scale bus

**ROOT OUT** is 1V/oct. **SCALE OUT** is polyphonic, and this is how a
microtonal scale reaches another module at all.

| Channel | Carries |
|---|---|
| 0 | The plain 1V-per-scale index. |
| 1 | The period, in volts. |
| 2 and up | The degrees, as 1V/oct offsets from the root. |

Every existing consumer calls `getVoltage()`, which returns channel 0 whatever
the channel count, so the extension is invisible to them and costs them
nothing. Where an index cannot name the key (a custom mask, a Scala file),
channel 0 carries the **nearest canonical scale** by pitch-class content as a
lossy summary; Pelog resolves to Phrygian.

Patch SCALE OUT into another Key and the whole key crosses intact, non-octave
period and all. Fourteen degrees maximum, being sixteen channels less the index
and the period.

## Inputs and outputs

**Inputs:** ROOT, SCALE, TRIG, and per channel IN (polyphonic).
**Outputs:** per channel OUT (polyphonic), plus ROOT and SCALE.

**TRIG** turns Key into a sample-and-hold: patched, the outputs update only on a
trigger, so pitches change on your clock rather than the moment the input moves.

## Context menu

| Item | What it does |
|---|---|
| **Load Scala file (.scl)…** | See above. |
| **Offset in scale degrees** | On by default. Off makes OFFSET semitones. |
| **Rounding** | Nearest / Down / Up. Which way a pitch between two degrees goes. |
| **Sub-scales may leave the key** | See above. Off by default. |
| **Hysteresis** | 12 cents by default. How far a pitch has to move before the output will change. |

### Hysteresis

A pitch sitting exactly on the boundary between two degrees will flicker between
them on the smallest amount of noise. Hysteresis makes the output stick until
the input has moved decisively.

It is **abandoned** in three cases, because holding on would be wrong: when the
key changes, when the channel's sub-scale changes, and when a held note would
otherwise survive outside the scale it is now supposed to be in.

## Patch ideas

**One key change, whole patch.** Key's ROOT and SCALE outs into Note, Chance,
Loom and Slide. Then drive Key's own ROOT and SCALE from Arrange, and the
arrangement changes the key of everything in one move.

**Three roles from one melody.** One sequence into all four INs. Channel 1 on
the full scale, channel 2 on a triad, channel 3 on root-and-fifth, channel 4 on
the full scale with OFFSET +2. Melody, pad, bass and a harmony line, all from
one part and all in key.

**Microtonal without the arithmetic.** Load a Scala file, and everything
downstream that reads the scale bus follows. The region strip shows you what you
are actually getting rather than what a keyboard would pretend you were.

**Chromatic colour on demand.** Turn on *Sub-scales may leave the key*, add a
flat 5th to sub-scale 3, and put the bass on it. The rest of the patch stays
diatonic; the bass gets one note that does not belong.

**A quantizer that follows the song.** TRIG from Meter's quarter note: pitches
change on the beat, not when the CV happens to move.
