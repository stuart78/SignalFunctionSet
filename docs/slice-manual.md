# Slice Manual

## Overview

Slice cuts a stereo stream onto a grid and reworks each piece.

Audio runs into a circular buffer, a grid of slices runs over it, and every so
often one slice is replaced by an altered version of itself: cut to silence,
swapped across the stereo field, pulled from a moment ago, reversed, stuttered,
pitched down. The rest pass through untouched.

Slice is 22HP. It works on anything, but it is aimed at rhythmic material, where
the grid has something to line up with.

## Two knobs, two questions

The heart of the module is that **what happens** and **how often** are separate
controls.

**EFFECT** picks one of seven transforms, or MIXED to rotate through them.
**EVERY** says how many slices pass between firings. Set EFFECT to REVERSE and
EVERY to 4 and every fourth slice plays backwards. Nothing else changes.

This matters because the obvious designs are worse. Seven per-slice probability
weights give the same undifferentiated scatter at every setting. Replacing them
with named patterns welds the two questions together, so most effects end up
stuck at one rate. Two knobs, two questions.

**The last slice of each group is the one that fires**, so the effect lands on
the approach to the downbeat rather than on it.

## The seven transforms

| Effect | What it does |
|---|---|
| **CUT** | Silence. The slice does not play. |
| **SWAP** | Left and right exchange for the duration of the slice. |
| **DELAY** | Plays a slice from further back instead of this one. |
| **SHUFFLE** | Plays a slice from a random earlier position. |
| **REVERSE** | Plays the previous slice backwards. |
| **REPEAT** | Stutters: takes the first 1/N of the slice and loops it. |
| **PITCH** | Plays the previous slice slowed down, so it drops in pitch. |
| **MIXED** | Walks the seven in order. Still a figure, just a longer one. |

**RESEED** rotates where MIXED starts, so you can shift the sequence without
changing anything else. It has a button and a trigger input.

### Why some transforms reach back a slice

Slice is **zero-latency when it passes through**: an untouched slice is read
from the write head sample for sample, so the module adds no delay at all.

But you cannot reverse a slice you are still recording. REVERSE and PITCH
therefore work on the *previous* slice, which is finished and sitting in the
buffer. REVERSE plays that one backwards, PITCH plays it slowed.

**REPEAT stays live.** It plays the first 1/N of the slice as it arrives, then
loops what it just captured. That keeps a stutter feeling immediate.

## WINDOW and SHAPE

These two are worth understanding together, because WINDOW decides whether
SHAPE matters at all.

**WINDOW is how much of the slice is window**, expressed as a fade time. It runs
from 5 ms at the bottom to **half the slice** at the top, and the tooltip reads
the actual milliseconds.

Those two ends are genuinely different jobs:

- **Near the bottom** the fade is an *edge taper*. Its only work is hiding the
  discontinuity where one slice meets the next. The ear hears how long a taper
  is, not how it curves, so at this end SHAPE is close to irrelevant. That is
  correct rather than a shortcoming.
- **At the top** the two fades meet in the middle and the slice becomes a
  **window** in the proper sense: nought up to full and back, no plateau. Now
  SHAPE governs the whole envelope and picks the character completely.

So WINDOW sweeps from *declick* to *grain envelope*, and Slice turns into a
gater with a shape control at the far end.

An edge with nothing to hide gets no fade at all. A run of untransformed slices
is already continuous, and fading it would only punch a hole in it.

### SHAPE

Three windows, ordered from most to least concentrated:

| Shape | As a full-slice window |
|---|---|
| **Gaussian** | 47% of the slice loud. A blip in the middle of its slot. |
| **Hann** | 80%. The classic arc. Default. |
| **Log** | 97%. Nearly the whole slice, with the corners taken off. |

There are three because there are only three. Window functions are all designed
to be smooth, so a longer list of famous names (Blackman, Sinc, smoothstep and
the rest) collapses into one wide arc that no listener can separate. What
actually distinguishes a window is its **width**, and these are three widths.

## DEPTH and the rest

| Control | Range | What it does |
|---|---|---|
| **EFFECT** | 8 positions | The seven transforms, plus MIXED. |
| **EVERY** | 1 / 2 / 3 / 4 / 6 / 8 / 12 / 16 | Slices between firings. Default 4. |
| **LENGTH** | 10 ms to 1 s | Slice length, when free-running. |
| **CLOCK RATE** | /8 /4 /2 **x1** x2 x4 x8 | Slice length as a multiple of the clock interval. x1 is dead centre, division to the left and multiplication to the right. |
| **DEPTH** | 0 to 100% | Crossfades the altered slice against the straight one. |
| **REACH** | 1 to 32 | How far back DELAY and SHUFFLE may pull from, counted in **steps**, or in **bars** once BAR is patched. |
| **WINDOW** | 5 ms to half the slice | See above. |
| **SHAPE** | 3 positions | See above. |
| **FREEZE** | latch | Stops the write head and loops what is in the buffer. |
| **RESEED** | button | Rotates where MIXED starts. |

**REACH is a count, and the parameter itself is one.** It is 1 to 32, snapped,
and only the unit changes when BAR is patched, so the tooltip reads "8 steps" or
"2 bars" rather than a percentage. Expressed as a percentage of the buffer, the
knob's whole lower half rounded to the same slice and no position on it named
anything.

Thirty-two bars at a slow tempo can be longer than the buffer. When that
happens the screen shows what you are actually getting, for example `7/32 bars`,
rather than printing a reach that is not happening.

## CLOCK, and which control is in charge

With nothing in **CLOCK**, slice length comes from **LENGTH**. Patch a clock and
it takes over, with CLOCK RATE setting the multiple. The screen tags the readout
**CLK** or **LEN** so you can always see which one is in charge.

**BAR** does one job: it changes REACH from counting steps to counting bars, so
DELAY and SHUFFLE pull from musically meaningful distances.

## The screen

**The main pane is anchored to NOW.** The write head is the right-hand edge and
the window is exactly REACH wide, so what you see is the span the module can
actually reach into. The slice grid is ticked along the foot, and a line runs
back to wherever the current slice was fetched from.

Under it, **one pane per channel, five seconds each, with now at the right**.
Both signals for that channel are drawn, but deliberately drawn differently:

- **Blue, filled: what leaves the jack.** The real output, after DEPTH.
- **Orange, outline: what came in.**

Only the audible one is solid. When both were filled, CUT at full depth drew a
fat orange waveform against complete silence, and a display that draws a signal
you cannot hear as though you can is worse than no display.

Everything is drawn as a filled envelope, the way Phase draws a sample: peak
along the top, back along the bottom, closed and filled. A column of separate
bars reads as a bar chart rather than as a waveform.

The header states the two fade lengths currently in force, which is the fastest
way to see what WINDOW is really doing.

## Inputs

**L** and **R** audio, with R normalled from L.

| Input | What it does |
|---|---|
| **CLOCK** | Sets the slice length and takes over from LENGTH. |
| **BAR** | Switches REACH to counting bars. |
| **RESET** | Resets the grid and the pattern. |
| **FREEZE** | Gate. Freezes the buffer. |
| **RESEED** | Trigger. Rotates where MIXED starts. |
| **DEPTH** | ±5V. |
| **LENGTH** | ±5V. |
| **EFFECT** | 1V per effect. |
| **EVERY** | 1V per step. |
| **REACH** | 0.1V per step or bar. |

## Outputs

| Output | What it does |
|---|---|
| **L / R** | The stereo output. |
| **SLICE** | Trigger on every slice boundary. Use it to clock something else off Slice's own grid. |
| **XF** | Gate, high while a slice is being altered. Duck a reverb, open a filter, or light something up exactly when the effect fires. |

Slice is **bypassable**: L and R pass straight through when the module is
bypassed.

## Context menu

| Item | Options |
|---|---|
| **Buffer length** | 30 / **60** / 120 seconds, with the memory cost shown for each. |
| **Repeat splice** | **Clean** or **Dirty**. |
| **Channels** | **Paired** (default) or Independent. |

**Buffer length** matters because REACH counts up to 32 bars, and at 120 BPM
that is 64 seconds. With a 30-second buffer, two thirds of that knob's travel
was unreachable. The cost is real, which is why it is a choice: 120 seconds is
44 MB at 48k and 176 MB at 192k, per instance. Resizing is safe to do while
audio is running.

**Repeat splice** exists because REPEAT is the only transform that splices
*inside* a slice, so it is the only place a click can come from that the edge
fades do not cover. **Clean** puts a 1.5 ms raised cosine either side of the
wrap, which cuts the discontinuity from about 5.9V to 2 mV. **Dirty** leaves it,
because the click is most of what makes a stutter sound like a stutter rather
than a loop.

That splice fade is deliberately not the SHAPE curve. Hiding a discontinuity
inside a slice is a different job from shaping the slice's edges, and it should
not change character when the edge shape does.

**Channels** paired means both sides slice identically, which keeps the stereo
image intact. Independent gives the right channel its own source for SHUFFLE and
DELAY, so the two sides pull from different places.

## The panel

Slice departs from the plugin's usual pot-over-jack pairs: **screen on top,
controls in the middle, every jack in two rows of eight at the foot**, with the
outputs on their own plate.

Slice has twelve inputs and only five of them modulate a knob, so pairing would
have scattered the audio and clock jacks in among the controls.

## Patch ideas

**Start here.** Drums into L, a sixteenth clock into CLOCK, EFFECT to REVERSE,
EVERY to 4, DEPTH full. One beat in four flips. Then walk EFFECT through the
seven and leave everything else alone.

**A grid that is not the drummer's.** Leave CLOCK unpatched and set LENGTH by
hand, slightly off the tempo. The slice grid drifts against the music and the
effects land somewhere new each bar.

**Long-memory stutter.** BAR patched, REACH to 4 bars, EFFECT to SHUFFLE, EVERY
to 8. Fragments from a few bars ago drop into the present, still on the grid.

**Grain envelope.** WINDOW to maximum, SHAPE to Gaussian, EVERY to 1. Every
slice becomes a short bloom in the middle of its slot, and Slice stops being an
effect and becomes a texture.

**Freeze and play.** Let a phrase run in, hit FREEZE, then work EFFECT, REACH
and LENGTH with the buffer held. Nothing new arrives, so you are performing on a
fixed piece of audio.

**Duck on the effect.** XF into a VCA on a reverb send. The tail opens only on
the slices that were altered.

**Slice as the clock.** SLICE out into a sequencer or an envelope. The rest of
the patch inherits Slice's grid, including the parts of it you set by hand.

**Stutter that means it.** EFFECT to REPEAT, Repeat splice set to Dirty, WINDOW
low. The clicks are the sound.
