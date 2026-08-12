# Fugue X Manual

## Overview

Fugue X is an expander for [Fugue](fugue-manual.md). Fugue gives three voices a
shared 8-step pitch sequence and a WANDER control each; Fugue X gives each of
those voices its own **rhythm and reach**, so that A, B and C stop being three
readings of one sequence and become three independent parts that happen to agree
about the key.

Everything it adds is per voice: how many steps that voice plays, how far its
wander may take it, how often it rests, and how likely each step is to sound at
all. It also adds three **sorted outputs** and a **trigger per step**.

## Connecting it

**Place Fugue X immediately to the right of Fugue.** There are no cables to
patch. It reads and writes over the expander bus. Moved away from Fugue's
right edge, its controls stop having any effect.

If you would rather have all of this on one panel, or you are in a host that
does not support expanders, use [MetaFugue](metafugue-manual.md) instead: it is
Fugue and Fugue X merged into a single wider module.

## Per-voice controls

Four controls for each of A, B and C.

| Control | Range | What it does |
|---|---|---|
| **STEPS** | 1–8 | How many of the eight steps this voice plays before looping. Set A to 8, B to 5 and C to 3 and the three parts phase against each other, coming back into alignment every 120 steps. |
| **RANGE** | 1V / 2V / 5V | A ceiling on how far this voice's WANDER may move it: one octave, two, or five. Fugue's WANDER knob says *how much* the voice strays; RANGE says how far that straying is allowed to reach. |
| **SLEEP** | 0, 1, 2, 4, 5, 8, 16, 32, 48, 64 | Clock ticks skipped between active steps. At 0 the voice plays on every clock. At 4 it plays, waits four ticks, plays again: rests, without changing the sequence. |
| **PROB** | 0–100% | Chance that a step actually sounds when it is reached. At 100% every step fires; at 50% the part thins to roughly half its notes, differently each pass. |

Each has a **CV input** beside it.

## Global controls

| Control | What it does |
|---|---|
| **Sample & Hold** | Off: a voice's CV returns to silence during a rest or a skipped step. On: it holds the last note it played through the gap. Off gives you a part with holes in it; on gives you a part with long notes. |
| **Randomize Sequence** (button + trigger input) | Rolls a new pitch sequence for Fugue. The button and the input do the same thing, so the sequence can be re-rolled from a clock divider or a manual press. |

## Outputs

**Sorted CV: MIN, MID, MAX.** The three voices' current pitches, ordered low to
high rather than by name. Voice A might be the highest note this step and the
lowest the next; MAX always carries whichever is highest right now.

This matters when you are voicing chords. Patch A, B and C to three oscillators
and each oscillator's line jumps around as the voices cross. Patch MIN, MID and
MAX instead and you get a bass part, an inner part and a top line. The same
notes, distributed by register instead of by origin. Sorted outputs are how you
get voice leading out of three independent parts.

**Per-step triggers.** A trigger output for each voice at each of the eight
steps: 24 jacks in all. Step 3 of voice B fires a trigger every time voice B
reaches step 3. Use them to fire drums in time with a melody, to advance another
sequencer on a particular step, or to open an envelope only on the steps you
choose.

The gate/trigger width follows the **Gate/trigger width** setting in the context
menu, the same one Meter and the other sequencers carry. Widen it to 5 ms if
you are driving an Expert Sleepers Encoders output, where a 1 ms pulse can be
dropped.

## Patch ideas

**Three tempos from one clock.** STEPS 8 / 5 / 3, SLEEP 0 for all three. One
clock in, three parts that phase and realign. This is the thing Fugue X is for.

**A part that breathes.** PROB around 60% on voice C with Sample & Hold off:
the top line comes and goes, leaving the other two exposed, and never in the
same place twice.

**Long-form counterpoint.** SLEEP 16 on voice A with STEPS 8, against SLEEP 0 on
B and C. A becomes a slow bass that moves once every sixteen ticks while the
others run.

**Chord voicing.** MIN / MID / MAX to three oscillators, and RANGE set to 1V on
all three so the voices stay inside an octave of each other. You get closed
voicings that reharmonise rather than three lines crossing.

**Rhythm from the melody.** Take the step-3 and step-7 triggers of voice A into
a snare and a hat. The drums are locked to the tune by construction, not by
having programmed the same figure twice.
