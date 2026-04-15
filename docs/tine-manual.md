# Tine Manual

## Overview

Tine is a tunable 3rd-order pingable resonator based on the Gamelan Resonator circuit from Paul DeMarinis' *Pygmy Gamelan* (1973). The circuit was analyzed by Werner & Teboul (AES Convention Paper 10542, 2021) and found to be a completely unique active filter topology — distinct from the Bridged-T and Twin-T designs common in analog drum machines.

When pinged, Tine produces metallic, bell-like ringing tones. The damping control varies the ring time from short percussive thumps to long sustain approaching self-oscillation.

Tine is 8HP.

## Concepts

### The Gamelan Resonator

The original circuit uses three resistors (two at value R, one at the peculiar ratio R/12), three capacitors, and a Norton op-amp (LM3900). This arrangement produces a 3rd-order resonant lowpass filter with one real pole and two complex conjugate poles. The complex poles produce the ringing behavior; their position relative to the stability boundary determines the ring time.

### Why It Sounds Different

Most pingable filters in analog drum machines are 2nd-order (Bridged-T, Twin-T). Tine's 3rd-order topology produces:
- Steeper spectral rolloff above the resonance (18 dB/oct vs 12 dB/oct)
- A different phase response that shapes the attack character
- Two zeros that create a specific spectral notch pattern

The result is a ring that sounds more "metallic" than a typical 2-pole resonator — closer to struck metal than a filtered impulse.

### Damping and Amplifier Gain

In the original circuit, the ring time is determined by the LM3900 Norton amplifier's finite voltage gain (approximately 3500x). The circuit is only stable because the amplifier is imperfect — with infinite gain, it would ring forever.

Tine exposes this amplifier gain as the Damping parameter, allowing you to sweep from heavily damped (low gain, short thump) through the natural LM3900 behavior (moderate ring) to near-marginal stability (very long sustain).

### Digital Implementation

The analog transfer function is discretized using the bilinear transform with frequency pre-warping for accurate V/Oct tracking. Double-precision arithmetic is used for the filter state variables to maintain numerical stability at high damping values.

## Controls

| Control | Type | Range | Default | Function |
|---------|------|-------|---------|----------|
| **Freq** | RoundHugeBlackKnob | -4 to +4 octaves | 0 (C4) | Resonant frequency, log2 scaled |
| **Damp** | Trimpot | 0-1 | 0.5 | Ring time. Low = short thump, high = long sustain |
| **Ping** | VCVButton | Momentary | — | Manual trigger |

## Inputs

| Input | Range | Function |
|-------|-------|----------|
| **TRIG** | Gate/Trigger | Rising edge fires a ping |
| **V/Oct** | 1V/octave | Pitch CV |
| **Damp CV** | ±5V | Damping modulation |

## Outputs

| Output | Function |
|--------|----------|
| **Out** | Monophonic audio output |

## VCA Mode (Anti-Click)

Enabled by default (toggle in right-click context menu). When a new ping arrives while the previous ring is still decaying, VCA mode crossfades between them:

1. Output envelope fades down over 2.5ms
2. Filter state is cleared
3. New pulse fires
4. Output envelope fades back up over 2.5ms

This eliminates the zero-crossing click that occurs when a new impulse interrupts a decaying oscillation. With VCA mode off, pings fire immediately — raw and clicky, but maximally responsive.

## Excitation Pulse

Each ping fires a 16-sample sine-arch shaped pulse into the filter. This smooth excitation (rather than a single-sample impulse) reduces the initial transient click while still exciting the full frequency range of the resonator.

## Patch Ideas

**Metallic Ping Voice**: Patch a clock to TRIG, a sequencer to V/Oct. Each clock pulse produces a pitched metallic tone. Adjust Damp for short percussion or sustained bell tones.

**Gamelan Bell**: Set Freq low (-2 to -3 octaves), Damp high (0.7-0.9). Trigger with an irregular clock or random trigger source. The long, low-pitched ring evokes gamelan percussion.

**Damped Drum**: Damp very low (0.1-0.2), Freq in the bass range. Short, punchy, percussive. Modulate Damp CV with an envelope for dynamic variation.

**Resonant Drone**: Damp at maximum (1.0). The filter approaches self-oscillation — even small trigger inputs or noise produce sustained tones. Modulate Freq slowly for evolving pitched drones.

**Polyrhythmic Bells**: Use multiple Tine instances at different frequencies. Clock each from different divisions of a master clock. Pan them across the stereo field for a gamelan-like ensemble.

## Technical Notes

- The filter is a 3rd-order IIR (Direct Form I) with 4 feedforward and 3 feedback taps
- Coefficients are recalculated every 32 samples using smoothed parameter values
- Coefficient interpolation (1ms time constant) prevents artifacts during parameter changes
- Anti-denormal offset (1e-20) prevents CPU spikes during silent decay
- Output soft-clamped at ±10V in the feedback path to mimic real op-amp output limiting

## Historical Context

The Gamelan Resonator was designed by Paul DeMarinis in June-July 1973 for his *Pygmy Gamelan* installation, part of David Tudor's *Composers Inside Electronics* group. Six copies were built, each tuned to a different five-tone scale using whatever resistors and capacitors were available. The circuits played autonomously in gallery settings, pinged by shift registers receiving stray electromagnetic signals from crude antennae.

The circuit's topology has no documented precedent. A survey by Werner & Teboul of dozens of analog drum circuits, active filter textbooks, and op-amp datasheets found nothing with a similar arrangement.
