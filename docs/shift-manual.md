# Shift Manual

## Overview

Shift is a four-lane CV shift register. One control voltage goes in, and four
come out, each one a delayed, re-clocked or recirculated version of what came
before it. It is for the moment when you have a good CV and want a small family
of related ones rather than four unrelated ones.

The four lanes are A, B, C and D, and each has its own step count, clock
division and mode. There is also a fifth output, **Jumble**, which picks at
random from whatever the four are currently holding.

Shift is 16HP.

## How a lane works

Each lane is a buffer of N values. What it does with that buffer depends on its
mode switch.

**Parallel.** An N-step delay line on the input. The output is the input as it
was N lane-clocks ago. Every lane-clock, one new value goes in the front and one
old value comes out the back. Set A to 1, B to 2, C to 3 and D to 4 and you get
the same CV arriving four times, staggered.

**Cascade.** A tape loop of length N, fed by *the previous lane* rather than by
the input. On each parent tick one value drips in from the lane to its left;
meanwhile the lane cycles through its whole buffer at clock rate. So the content
changes slowly and repeats quickly, which is a very different thing from a delay
line: the lane becomes a short loop that is gradually overwritten.

Cascade on lane A has no previous lane to feed from, so it falls back to
parallel.

## Controls

**Per lane (×4)**

| Control | Range | What it does |
|---|---|---|
| **N** | 1–16 | Buffer length. In parallel mode, how many clocks of delay. In cascade mode, how long the loop is. |
| **Mode** | cascade / parallel | See above. |
| **DIV** | ÷1, ÷2, ÷3, ÷4, ÷5, ÷8 | Slows that lane's reads *and* writes. Combines multiplicatively with N, so a lane at N=4, ÷3 comes out twelve input clocks behind. |

**Global**

| Control | What it does |
|---|---|
| **Reset** (button + input) | Clears every buffer, every held value, the Jumble sample and all the read/write and divider counters. Everything starts again from silence. |

## Inputs

| Input | What it does |
|---|---|
| **CV** | The data. Sampled on each tap fire. |
| **CLOCK** | Advances the lanes. |
| **N CV** | ±5V → ±15, summed into *every* lane's N pot. One cable stretches or compresses the whole family at once. |
| **Step CV** (×4) | Per lane, ±5V → ±N, on top of that lane's own pot. |
| **RESET** | As the button. |

## Outputs

| Output | What it does |
|---|---|
| **CV A / B / C / D** | The four lanes. |
| **Gate A / B / C / D** | A gate on each lane's tap fire, following the shape of the input clock. |
| **Jumble CV** | A random pick from among the four held values, re-rolled on every input clock. |
| **Jumble CLK** | Mirrors the input clock shape, so the pick has a gate to go with it. |

Each lane also has an LED, as does Jumble.

## Disconnect playback

Unpatch the CV input and Shift does not go quiet. Each lane's reads cycle
through a 16-slot full-depth history ring at clock rate, so the accumulated
content keeps playing, even a cascade lane at N=1, which would otherwise have
nothing to say.

This is the feature that turns Shift from a utility into an instrument. Feed it
a melody for a while, pull the cable, and it carries on making something out of
what it heard.

## Context menu

| Item | What it does |
|---|---|
| **Clear all** | Wipes all buffer contents, held values, the Jumble sample and every read/write and divider index. The same as a Reset trigger. |

## Patch ideas

**Canon.** One sequencer into CV, all four lanes parallel, N = 1 / 2 / 3 / 4.
Four oscillators, four staggered entries of the same line.

**A chord that fills in.** Parallel lanes with N = 0 / 3 / 6 / 9 into four
oscillators, and a slow melody in. Each note is held by successive lanes as the
melody moves, so a single line accumulates into changing harmony.

**Generative loop.** Set B, C and D to cascade with different N values, feed a
sequence in for a few bars, then unpatch the CV. The lanes keep cycling and
slowly drifting, and the Jumble output picks between them.

**Rhythmic decorrelation.** Same N on every lane but different DIV values. The
lanes carry the same material at four different rates, and the gates give you
four different rhythms to fire envelopes from.

**Sample and hold, four ways.** Noise into CV, a slow clock, and take Jumble CV:
a random voltage that is nonetheless drawn from a small pool of recent values,
so it wanders rather than jumping anywhere at all.
