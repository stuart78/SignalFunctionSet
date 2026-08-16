# Slide Manual

## Overview

Slide is an electric lap steel.

Eight waveguide strings, stopped by a **steel bar** instead of frets. The bar is
one rigid object lying across every string, so it moves them all by the same
ratio and the tuning's intervals survive wherever you put it. That is why lap
steel lives in 6th and 7th tunings (C6, E7, E13, A6): a straight bar is already
a chord, and moving it gives you the same chord somewhere else.

It shares its waveguide code with [Loom](loom-manual.md), and everything else
about it is different, because a bar is not a fret.

Slide is 28HP.

## What makes it a slide and not a pitch bend

Five things, roughly in order of how much they matter.

**1. Glide is rate-based, not time-based.** A hand crosses the neck at roughly
constant speed, so a twelfth takes about 2.4 times as long as a fifth. Nearly
every synth portamento is constant-*time*-per-interval, every move taking the
same duration regardless of distance, and that is most of the reason synth
glides do not sound like slides. Bar travel is also an **S-curve scaled to the
distance**, so a short move is not all ease and a long one is not a hard ramp
with a nub on the end.

**2. SLANT.** Angling the bar across the strings is the technique that gets
major, minor and dominant voicings out of one tuning without retuning. It is the
reason Slide is its own module rather than a glide mode on Loom.

**3. The pickup is fixed in space** while the speaking length changes. Its
position as a fraction of the string runs from about 14% to 48% as you climb the
neck, and the comb null walks from the 7th harmonic down to the 2nd, so the
tone hollows out as you go up. Loom's comb is a fixed fraction of the string,
which is right for a fretted instrument and wrong for this one.

**4. The bar is a lossy, mass-loaded termination.** It returns less treble than
a fret clamping a string against wood.

**5. Vibrato is a rocking motion**, wide and centred on the pitch, rather than a
bend up to it.

## The volume pedal

SWELL per note, plus a VOL input, and it swells in **past the pick attack**.

The missing transient is what makes a steel cry, and it is the one thing a slide
model can do that a portamento cannot fake. If you only reach for one control
after the bar, reach for this.

## Controls

**The bar**

| Control | What it does |
|---|---|
| **POS** | Bar position. With nothing patched to BAR it is the position itself; with BAR patched it becomes an in-key transpose on top of it. |
| **SLANT** | ±6 semitones of angle across the eight strings. |
| **GLIDE** | Bar travel *rate*. |
| **VIBRATO** | Depth of the rocking motion. |
| **SPEED** | Vibrato speed, 2–9 Hz. |
| **BLOCK** | Hand damping on the strings you are not playing. |

**The strings**

| Control | What it does |
|---|---|
| **DECAY** | 0.2–20 s. |
| **DAMP** | Treble loss. |
| **PICK** | Pick hardness. |
| **COUPLE** | Sympathetic ring through the bridge. Loom's bridge bus, ported over. Slide had no halo at all before it. |
| **SWELL** | The volume pedal, per note. |

**The amp**

| Control | What it does |
|---|---|
| **PICKUP** | Pickup position, 4%–34% from the bridge. |
| **TONE** | Coil resonance, moving the peak 1.6–6.2 kHz. |
| **DRIVE** | Amp drive. |

The body here is a **magnetic pickup**, not a soundboard: a resonant lowpass
standing in for coil inductance against cable capacitance, then drive.

**The key**

ROOT (±12 semitones), SCALE (the canonical plugin list), OCT (−4 to +2).

**The roll**

| Control | What it does |
|---|---|
| **ROLL** | Which of sixteen fingerpicking patterns. |
| **DENSITY** | How many of its notes play. |
| **DYN** | Dynamics: pick accent first, note-to-note jitter second. |
| **AUTO** | Runs the roll. |
| **RESET** | Back to the start. |

## Tunings

Ten, from the context menu: **C6, C6 add 9, E7, E13, A6, Open major, Open minor,
Dobro G, Fourths, Unison.**

The 6th and 7th tunings are the point. In C6 the open strings are already a
chord, so any bar position is that chord transposed, and SLANT tilts it into a
different one.

## Rolls

Sixteen fingerpicking patterns: forward, backward, alternating, thumb & index,
inside out, climb, fall, pinch, random, strum, then six built from a wrapping
4-bit adder, shared with Loom.

Each carries **per-step accents**, because the thumb is a heavier finger than
the index: bass strokes land harder, and the stroke that starts a roll hardest
of all. DYN brings that shape in first and note-to-note jitter second, because jitter
alone humanises *when* the accents fall but not *what shape they make*, which is
why the order matters.

## The display

A **fretboard lying flat**: strings horizontal, logarithmic fret spacing, and
the bar drawn as a line whose *angle* is the slant. The segment behind the bar
is drawn dead, because it is.

- **Drag the bar** with the mouse.
- **Hover across the strings** to pick them.

## Inputs

**BAR** (1V/oct, and the POS trimpot then transposes it in key), SLANT, V/OCT
(transposes the whole instrument), GATE (polyphonic: channel N picks a note, and
the bar solver decides which string), VEL (polyphonic), CLOCK, RESET, **VOL**
(0–10V volume pedal), and CV for Decay, Damp, Pick, Tone, Roll, Density, Root
and Scale.

For **per-string** gates and outputs, add [SLIDE XP](slidex-manual.md).

## Outputs

MIX L, MIX R, **EVEN** (strings 2, 4, 6, 8 summed) and **ODD** (1, 3, 5, 7).

The even/odd pair is a cheap way to get two related but distinct signals out
without an expander. Send them to different amps and the instrument widens.

## What a gate does

One stroke, never a strum.

- **V/oct set to "places the bar and picks the string"**, with V/OCT patched:
  the gate picks the string the solver chose for that note.
- **V/oct set to "transposes"**, or melody mode with nothing in V/OCT: the gate
  plays one step of the selected roll pattern, exactly as a CLOCK tick would.
  VEL sets the stroke's strength.

If you want a gate to strum all eight strings, select the **Strum** roll
pattern. That is a thing to ask for rather than a thing that should happen to
you, which is why it lives in the pattern selector.

## Legato reach

In the context menu, measured in frets.

When a note arrives while another is **still held**, a steel player does not
lift the bar and put it down somewhere else. They move it, and that movement is
the sound of the instrument. Legato reach is how far the bar will travel to keep
that held note on the string it is already sounding on, before it gives up and
crosses to a different string instead.

At **0** Slide behaves as it always did: it takes whichever string needs the
least bar movement, so a melodic line comes out as a series of clean attacks on
different strings. Turn it up and the line stays on one string and slides.

It is a distance, not a strength, and that matters. A stepwise line moves one or
two frets per note, so anything above about 1 keeps the whole line on one
string. The control earns its range on **leaps**: over a line of fifths, sevenths
and octaves it takes the part from seven string crossings down to none, and bar
travel from five frets to twenty-one, changing at every step in between.

Two things worth knowing:

- **It only applies to legato.** After silence, a fresh note is free to take the
  nearest string, because that is what a player does when they lift.
- **It is not an absolute guarantee.** A note past the end of the current
  string cannot be played on it, so Slide crosses anyway and re-picks the new
  string so you hear the right pitch.

## Context menu

| Item | What it does |
|---|---|
| **Tuning** | The ten above. |
| **V/oct** | Whether V/OCT transposes the whole instrument, or places the bar and picks the string. |
| **Pickup** | Modern single coil, or **Horseshoe**, which adds a broad midrange band under the coil resonance. Bark and honk is a *wide* lift, and no single resonant lowpass makes one wherever you put its corner. |
| **Mouse** | Hover strums the strings, or click and drag only. |
| **Auto roll moves the bar** | Whether the auto-player also walks the bar around. |
| **Hover plays when Rack is in the background** | Off by default. |

## Getting the crying Hawaiian sound

The question this module gets asked most. In order:

1. **SWELL up**, so the volume pedal hides the pick attack. This is most of it.
2. **GLIDE around 0.5–0.7**, so moves take real time.
3. **VIBRATO shallow and slow**, around 5 Hz.
4. **DAMP fairly high**, since a steel is a dark instrument.
5. **C6 or E7 tuning.**

Then move the **bar** between two chord positions rather than picking new notes.
The sound is in the movement between them, not in the notes at either end.

## Patch ideas

**Pedal-steel chord change.** Park the bar, set SLANT to 0, and automate SLANT
alone with a slow envelope. The chord changes quality without the pitch centre
moving. That is the pedal-steel move.

**In-key bar sequencing.** BAR from Note or Chance at 1V/oct, POS as an offset,
and SCALE from Key. The bar lands on scale positions and the whole thing stays
in the song's key.

**Two amps.** EVEN and ODD to separate channels with different reverbs.

**Slow-motion.** GLIDE near maximum and a big interval jump: the bar takes
seconds to arrive and you hear every string passing through every intervening
chord on the way.
