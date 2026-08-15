# Cycle Manual

## Overview

Cycle is a four-channel LFO that thinks in bars, not Hertz. All four outputs (**A, B, C, D**) run the same underlying cycle, but each can be spread, scaled, and shaped independently, and the whole bank locks to a musical bar via clock and bar inputs. Unpatched, it free-runs in Hz like an ordinary LFO; patched into Meter (or any clock + bar source), it phase-locks to the song so every modulation lands on the grid. It is the tempo-synced companion to Drift: the same four-voice, phase-fanned idea, anchored to the bar instead of running free.

Cycle is 26HP.

## How timing works

Cycle has two modes that switch automatically:

- **Free (BAR unpatched).** An ordinary LFO. **FREQUENCY** sets the rate continuously from 0.02 Hz to 20 Hz.
- **Locked (BAR patched).** **BAR** is the sole timing authority: Cycle measures the bar length and hard-aligns its cycle to every downbeat, so it can never drift out of phase no matter how long it runs (there is no clock-pulse counting to lose). **FREQUENCY** becomes a musical divider/multiplier.

A single master phase drives all four channels. **PHASE** fans the four outputs across that cycle; each channel then applies its own **SHAPE** and **SCALE**.

## The shape ring

**SHAPE** sweeps a *closed loop* of waveforms:

> sine → triangle → saw → square → staircase → stepped-random → *(back to sine)*

Because the loop is circular, sweeping past stepped-random morphs smoothly back into sine: there are no end-stops. This is what makes shape **CV** and the link offsets wrap continuously around the timbre loop instead of clamping at an edge.

Two shapes are step-based and follow the clock grid:

- **Staircase**: an ascending stair of equal steps (each 1/n tall). Step count = the bar division (a 4-bar cycle → 4 steps), or, if **CLOCK** is patched, one step per clock pulse so the stair climbs in real beats. A negative SCALE inverts it (descending).
- **Stepped-random**: a fresh set of random step voltages generated each cycle, **shared by all four channels** so they jump together. Step count follows the same grid as the staircase.

## Controls

### Per channel (×4: A, B, C, D)

| Control | Function |
|---------|----------|
| **SHAPE** | Position around the shape ring. Wraps (sine at both ends). |
| **SCALE** | Bipolar output depth: full right = full level, centre = silent, left of centre **inverts** the wave. |

### Global

| Control | Function |
|---------|----------|
| **FREQUENCY** | Locked: bars-per-cycle / cycles-per-bar, one of 64, 32, 16, 8, 4, 2, 1, ½, ¼, ⅛ (default 1 bar). Free: 0.02–20 Hz. Can be changed live. |
| **PHASE** | Phase spread. Fans channels A→D across the cycle for staggered/chasing motion; at zero all four ride together. |
| **STABILITY** | 100% = rock-steady. Lower it to introduce Drift-style cycle-to-cycle amplitude wander, so each channel breathes. |
| **RESET** (button) | Restart the cycle from the top of the next downbeat. |

### Linking and offsets

Toggle buttons between adjacent rows link channels: a **shape** link on the SHAPE column and a **scale** link on the SCALE column (links A–B, B–C, C–D for each). **All six links default to on**, so a fresh Cycle starts with A–D fully ganged: turn channel A and the rest follow.

When a channel is linked, its own knob is not dead. It becomes an **offset** from the group leader, measured from the knob's default position:

- **SHAPE** offsets *wrap around the ring*. At the default knob position the follower matches the leader exactly; turn it and the follower rotates around the timbre loop relative to the leader.
- **SCALE** offsets are bounded (it is amplitude, not a loop): the follower tracks the leader with a fixed amount more or less depth.

So you can lock all four channels to one master shape, then nudge each for richness, or unlink for four fully independent LFOs.

## Inputs

| Input | Function |
|-------|----------|
| **BAR** | Downbeat sync, the timing authority. Patch this to lock Cycle to a bar grid. |
| **CLOCK** | Optional. Sets the staircase / stepped-random step grid: one step per pulse (e.g. feed Meter's SIXTEENTH). |
| **RESET** | Restart the cycle (trigger). |
| **FREQUENCY CV** | Modulates the rate / division. |
| **PHASE CV** | Modulates the phase spread. |
| **STABILITY CV** | Modulates the amount of amplitude wander. |
| **Per channel: SHAPE CV** | Rotates that channel around the shape ring (wraps). |
| **Per channel: SCALE CV** | Modulates that channel's depth. |

## Outputs

| Output | Function |
|--------|----------|
| **Per channel: UNI** | Unipolar, 0–5V: for levels, cutoff, anything wanting positive-only CV. |
| **Per channel: BI** | Bipolar, ±5V: the same wave centred on zero, for pitch, pan, FM. |
| **EOC** | End-of-cycle trigger, firing each time the cycle completes. Chain cycles, advance a sequencer, or re-trigger envelopes once per loop. |

## The display

The screen shows all four channel waveforms over one cycle, each in its colour (A blue, B green, C orange, D purple), with a playhead sweeping the current position. When locked, bar gridlines divide the cycle and a readout tracks your place in it (`BAR 2/4`, bars remaining, or cycles-per-bar when running fast); unpatched it shows the free-running frequency. Voltage scales label the edges, bipolar (±5V) on the left and unipolar (0–5V) on the right, so each output's range is readable at a glance.

## Patch Ideas

**Whole-patch tempo breathing.** Clock Cycle from Meter (BAR + a subdivision into CLOCK). Send the four channels to filter cutoffs, levels, and effect sends across the patch. Everything modulates in lockstep with the bar.

**Staggered chase.** Link all four shapes, turn PHASE up, and the channels fan into a chasing wave, great for sequential VCAs, panning, or sweeps.

**Tempo-locked random.** Set stepped-random with a 1-bar cycle and sixteenths into CLOCK. All four channels jump to a new shared random value every sixteenth, perfectly on grid: a sample-and-hold that never loses time.

**Inverted pairs.** Link A–B scale, then pull B's SCALE below centre to invert it: two channels mirror each other for see-saw modulation (one filter opens as the other closes).

**Generative drift with a leash.** Pull STABILITY down for living, wandering amplitudes, but keep BAR patched so the timing stays locked: organic movement, rigid tempo.

**Slow evolving timbres.** Set a long division (8–64 bars), feed BI into pitch/FM and UNI into a wavefolder or Overtone's harmonics. The patch transforms over many bars and resets cleanly at the loop.
