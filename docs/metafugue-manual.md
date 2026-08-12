# MetaFugue Manual

## Overview

MetaFugue is [Fugue](fugue-manual.md) and [Fugue X](fuguex-manual.md) merged
into one panel. Every control from both, no expander bus, nothing to place next
to anything.

Use it when:

- **your host does not support expanders**, MetaModule for one;
- **you want the whole instrument in one place**, without a rule about which
  module sits to the right of which;
- **you are moving patches between hosts** and would rather not depend on
  adjacency surviving the trip.

Everything below is a summary. For what the controls actually *do*, and why,
read the two manuals above: the behaviour is identical.

## What it is

An 8-step harmonic deviation sequencer with three CV/gate voices. All three read
the same pitch sequence, and each wanders away from it under its own controls,
so you get counterpoint out of a single line rather than out of three sequences
you had to write.

## Controls

**The sequence, shared by all three voices**

| Control | What it does |
|---|---|
| **Root** | 12 semitones. The key. |
| **Scale** | The canonical plugin scale list. See [scales.md](conventions/scales.md). A SCALE CV here is interchangeable with Note, Chance, Muse, Key and Arrange. |
| **Steps** | 1–8, the master sequence length. |
| **Slew** | Portamento on all three CV outputs. |
| **Gate toggles** | Per step, per voice: whether that voice has a gate on that step. |
| **Reset** (button + input) | Back to step 1. |

**Per voice (A, B, C)**

| Control | Range | What it does |
|---|---|---|
| **Wander** | 0–100% | How far this voice strays from the shared sequence. |
| **Steps** | 1–8 | This voice's own length. Different lengths phase against each other. |
| **Range** | 1V / 2V / 5V | The ceiling on how far Wander may reach. |
| **Sleep** | 0, 1, 2, 4, 5, 8, 16, 32, 48, 64 | Clock ticks skipped between active steps. |
| **Prob** | 0–100% | Chance a step sounds when reached. |

**Global**

| Control | What it does |
|---|---|
| **Sample & Hold** | Hold the last note through rests, rather than returning to silence. |
| **Randomize Sequence** (button + input) | Roll a new pitch sequence. |

## Inputs

Clock A, **Clock B (normalled to A)**, **Clock C (normalled to B)**, Reset, Root
CV, Scale CV, Steps CV, Slew CV, Wander A/B/C CV, and per voice Steps / Range /
Sleep / Prob CV.

The normalling is worth knowing: patch one clock into A and all three voices run
from it. Patch a second into C and you get A+B on one clock and C on another,
without having to mult anything.

## Outputs

| Output | What it does |
|---|---|
| **CV A / B / C** | The three voices, 1V/oct. |
| **Gate A / B / C** | Their gates. |
| **MIN / MID / MAX** | The three voices sorted by pitch rather than by name: a bass line, an inner line and a top line. See the Fugue X manual for why this is the useful way to drive three oscillators. |
| **Per-step triggers** | One per voice per step, 24 in all. |

## Context menu

| Item | What it does |
|---|---|
| **Harmonic Lock** | Constrain the wander to harmonically related intervals rather than any scale degree. |
| **Fader Range** | 1V (1 octave) / 2V / 5V. The range the on-panel faders cover. |
| **Gate length** | How long the gate outputs stay high. |
| **Randomize Sequence** | The same action as the panel button. |

## Which one should I use?

If your host supports expanders and you like a narrower panel, use Fugue plus
Fugue X. If you would rather not think about placement, use MetaFugue. There is
no difference in sound, and patches are not interchangeable between the two
(they are separate modules with separate parameter lists), so pick one per patch
rather than switching mid-project.
