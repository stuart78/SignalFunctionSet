# Drift Manual

## Overview

Drift is a 4-channel phase-shifted LFO with chaos capabilities. Four outputs (A, B, C, D) generate waveforms that can be spread across phase, morphed between shapes, and destabilized using a Lorenz attractor-based chaos system. It produces everything from clean synchronized LFOs to slowly drifting organic modulation.

Drift is 10HP.

## Concepts

### Phase Spreading

The four outputs are phase-shifted relative to each other. At maximum spread, A is at 0 degrees, B at 90, C at 180, and D at 270 — a quadrature LFO. At minimum spread, all four outputs are in phase. The Phase knob controls this spread continuously.

### Waveform Morphing

The Shape control smoothly transitions between waveform types: sine, triangle, sawtooth, square, and chaos. Intermediate positions blend between adjacent waveforms, allowing continuous timbral modulation of the LFO shape.

### Chaos and Stability

The Stability control modulates a Lorenz attractor system that adds organic unpredictability to each output independently. At high stability, outputs are clean periodic waveforms. As stability decreases, the outputs begin to drift and mutate — each channel develops its own subtle variations because the chaos calculation is independent per output.

## Controls

| Control | Range | Default | Function |
|---------|-------|---------|----------|
| **Shape** | 0-1 | 0.5 | Waveform morphing: sine → triangle → sawtooth → square → chaos |
| **Stability** | 0-1 | — | Chaos amount. Higher = more stable/predictable |
| **Center** | -5V to +5V | 0V | DC offset applied to all outputs |
| **Spread** | 0-5V | — | Amplitude of all outputs |
| **Frequency** | — | — | LFO rate in Hz |
| **Phase** | 0-1 | — | Phase spread between the four outputs |

## Inputs

All controls have corresponding CV inputs (±5V):
- **Shape CV** — waveform morphing
- **Stability CV** — chaos amount
- **Clock** — external clock sync (overrides Frequency knob)
- **Phase CV** — phase spread
- **Center CV** — DC offset
- **Spread CV** — amplitude

## Outputs

| Output | Function |
|--------|----------|
| **A** | LFO output, reference phase |
| **B** | LFO output, phase-shifted from A |
| **C** | LFO output, phase-shifted from A |
| **D** | LFO output, phase-shifted from A |
| **Min** | Minimum value across all four outputs at each sample |
| **Max** | Maximum value across all four outputs at each sample |

The Min and Max outputs are useful for creating complex envelopes and modulation shapes derived from the four LFO channels.

## Patch Ideas

**Quadrature Modulation**: Set Phase to maximum for 90-degree separation. Patch A and C to a stereo panner's X and Y inputs for circular panning.

**Organic Drift**: Lower Stability to introduce chaos. Each output will drift independently, creating four related but non-identical modulation sources.

**Complex Envelope**: Use Min and Max outputs with different Shape settings to create asymmetric modulation shapes that no single LFO could produce.

**Synchronized Wobble**: Patch a clock to the Clock input for tempo-synced modulation. Adjust Shape for different rhythmic characters.
