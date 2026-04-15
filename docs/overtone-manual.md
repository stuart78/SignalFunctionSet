# Overtone Manual

## Overview

Overtone is an additive synthesis VCO that builds waveforms from the harmonic series. The fundamental frequency is always present. Eight toggle switches enable individual overtones (harmonics 2 through 9), each at natural 1/n amplitude falloff. All overtones on approximates a sawtooth wave; all off produces a pure sine.

Overtone is 10HP.

## Concepts

### Additive Synthesis

Any periodic waveform can be decomposed into a sum of sine waves at integer multiples of a fundamental frequency. A sawtooth at 100 Hz is sines at 100, 200, 300, 400 Hz... each at amplitude 1/n. A square wave uses only odd harmonics. Overtone lets you choose which harmonics to include, building the timbre directly.

### Harmonic Amplitude Falloff

Each harmonic's amplitude follows the natural 1/n series:

| Harmonic | Frequency | Amplitude |
|----------|-----------|-----------|
| 1 (fundamental) | f | 1.000 (always on) |
| 2 | 2f | 0.500 |
| 3 | 3f | 0.333 |
| 4 | 4f | 0.250 |
| 5 | 5f | 0.200 |
| 6 | 6f | 0.167 |
| 7 | 7f | 0.143 |
| 8 | 8f | 0.125 |
| 9 | 9f | 0.111 |

### Zero-Crossing Gating

When a harmonic is toggled, the transition waits until that harmonic's sine wave crosses zero before switching on or off. This eliminates clicks without using a fade envelope. Higher harmonics cross zero more frequently, so they respond almost instantly.

### Even/Odd Filter

The 3-position filter restricts which harmonics can sound:
- **All**: All enabled harmonics play
- **Odd**: Only odd-numbered harmonics (1, 3, 5, 7, 9) — square wave character
- **Even**: Only even-numbered harmonics (1, 2, 4, 6, 8) plus fundamental

### Dynamic Normalization

The output is normalized by the sum of active 1/n amplitudes. Toggling harmonics changes timbre but not overall volume.

## Controls

| Control | Range | Default | Function |
|---------|-------|---------|----------|
| **Harmonic 2-9 Toggles** | On/Off | All on | Enable/disable individual overtones |
| **Even/Odd/All Switch** | 3-position | All | Harmonic filter |
| **Freq** | -4 to +4 octaves | 0 (C4) | Coarse pitch, log2 scaled |

## Inputs

| Input | Range | Function |
|-------|-------|----------|
| **V/Oct** | 1V/octave | Pitch CV |
| **Mask** | 0-10V | Harmonic selection CV (binary or sweep mode, context menu) |
| **Filter** | 0-5V | Filter CV (0-1.67V=All, 1.67-3.33V=Odd, 3.33-5V=Even) |

## Outputs

| Output | Function |
|--------|----------|
| **Out** | Monophonic audio, ±5V normalized |

## LED Indicators

Red LEDs above each toggle show the actual active state of each harmonic — reflecting the combined effect of toggles, mask CV, and filter. When the mask CV overrides the toggles, the LEDs track the CV-driven state.

## Mask CV Modes

Select via right-click context menu:

### Binary Mode (default)
0-10V maps to an 8-bit integer (0-255). Each bit controls one harmonic: bit 0 = harmonic 2, bit 7 = harmonic 9. Each step is ~0.039V. Sweeping produces rapid, complex timbral changes.

### Sweep Mode
0-10V maps to 0-8 harmonics enabled from the bottom up. 0V = fundamental only, 10V = all harmonics. Produces a smooth brightness sweep.

## Waveform Display

Shows the composite waveform (bright blue), individual harmonic traces (faint colors), and the fundamental (faint white). Updates when harmonics change.

## Patch Ideas

**Sine-to-Saw Envelope**: Patch an envelope to Mask CV in sweep mode. Notes start as pure sine and brighten as harmonics are added.

**Stepped Timbre Sequence**: Patch a sequencer to Mask CV in binary mode. Each step has a unique harmonic combination.

**Filter Swap**: Patch a stepped CV to Filter input. Timbre flips between All/Odd/Even on each clock pulse.
