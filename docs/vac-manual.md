# Vac Manual

## Overview

Vac is a semi-stable attack/release envelope generator. It has the classic A/R shape, but with a per-stage **STAB** (stability) control that introduces *controlled, musical* cycle-to-cycle variation in timing, the way a real vactrol (LED-driven photoresistor) drifts. At STAB=0 it's a perfectly ordinary, repeatable envelope; turn STAB up or down and each trigger's rise and/or fall stretches or shortens by a fresh random amount, so a repeated pattern breathes instead of marching.

Vac is 6HP.

## How the drift works

Each rising-edge trigger samples a fresh random factor for the rise and fall stages, scaled exponentially by their STAB knobs:

```
factor = exp(STAB · r · ln 2.5)     where r = random 0..1
```

- **STAB = 0** → factor = 1 (perfectly stable, like any normal envelope).
- **STAB = +1** → factor in 1×–2.5× (stages run *longer*, by a random amount each cycle).
- **STAB = −1** → factor in 0.4×–1× (stages run *shorter*).

The exponential form is **log-symmetric around 1**, so STAB never collapses a stage to zero, and it matches how vactrols actually drift (their response is multiplicative, not additive). Rise and fall have independent STAB controls, so you can keep attacks tight while letting releases wander, or vice versa.

## Controls

| Control | Function |
|---------|----------|
| **RISE** | Attack time |
| **RISE STAB** | Bipolar stability of the rise stage (0 = stable; ± = random longer/shorter per trigger) |
| **FALL** | Release time |
| **FALL STAB** | Bipolar stability of the fall stage |
| **CURVE** | Blends linear-rate ↔ exponential-rate stage shaping (Swell-style), applied to both stages |
| **LOOP** (button + LED) | Latches auto-retriggering: when the fall completes, a new cycle starts. Toggling LOOP on from idle also kicks off a cycle (no external trigger needed) |

## Inputs

| Input | Function |
|-------|----------|
| **TRIG** | Rising edge starts one A/R cycle |
| **RISE CV** | Rise time modulation (±5V → ±50%) |
| **RISE STAB CV** | Rise stability modulation |
| **FALL CV** | Fall time modulation |
| **FALL STAB CV** | Fall stability modulation |
| **CURVE CV** | Curve modulation (±5V → ±50%) |

## Outputs

| Output | Function |
|--------|----------|
| **ENV** | The envelope, 0–10V |
| **END** | 1ms trigger fired at the end of each fall stage (chain it, or use it to clock other events at the envelope's natural, drifting rate) |

## Context Menu

- **Continuous drift (rate wobbles during stage)**: instead of one random factor sampled per stage, the rate wobbles smoothly *throughout* each stage, for an even more "thermal," organic feel.

## Patch Ideas

**Humanized plucks:** trigger Vac from a sequencer's gate, set a short RISE and medium FALL, and dial RISE STAB / FALL STAB to ~±0.2. Each note's envelope is subtly different, so the rigid grid loosens up without losing the groove.

**Self-running drifting LFO:** turn on LOOP with no trigger patched. Vac free-runs; raise the STAB controls and it becomes a slow, wandering LFO whose period never quite repeats. Take END to clock other modules at that organic rate.

**Vactrol-style filter pings:** send ENV to a filter's cutoff or VCA, trigger rhythmically, and use FALL STAB to make each decay tail breathe like a real vactrol-based gate.

**Generative timing:** chain END → TRIG of another Vac (or back into a sequencer's clock) so the drifting release time becomes the timing source for the next event. Small STAB values create gently elastic rhythms.

**Tame it:** set both STAB knobs to 0 and Vac is a clean, repeatable A/R envelope with a nice linear↔exponential CURVE, useful even when you don't want the drift.
