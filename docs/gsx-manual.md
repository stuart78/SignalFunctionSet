# GSX Manual

## Overview

GSX is a real-time granular synthesis module inspired by Barry Truax's groundbreaking GSX system (1985-86) — the first implementation of real-time granular synthesis. It generates dense textures from hundreds of short sound events called "grains," operating in the microsound domain (1-50ms) where changes in the time domain produce changes in the frequency/spectral domain.

GSX is 12HP. Stereo output.

## Concepts

### Granular Synthesis

Granular synthesis builds sound from many tiny sonic events (grains), each typically 1-50ms long. At this timescale, individual grains are perceived not as separate notes but as texture. The aggregate of hundreds of overlapping grains creates continuous sound whose character is controlled by the statistical properties of the grain stream — density, frequency spread, duration, timing regularity.

### Streams

GSX runs up to 20 independent grain generators (streams) simultaneously. Each stream independently schedules and plays its own grains. More streams produce denser, smoother textures; fewer streams let individual grains become audible.

### Quasi-Synchronous vs. Asynchronous

At low Variation settings, grains fire at regular intervals and at consistent frequencies — this is quasi-synchronous mode. The regularity creates pitch through amplitude modulation: if grains arrive every 20ms, you hear a 50Hz modulation tone. This is how the original GSX produced tonal effects from granular techniques.

At high Variation settings, grain timing, frequency, duration, and panning all become stochastic. Grains scatter randomly, producing cloud-like textures with no clear pitch — asynchronous mode.

The Variation knob blends continuously between these extremes.

## Controls

| Control | Range | Default | Function |
|---------|-------|---------|----------|
| **Frequency** | 50-2000 Hz | 130.81 Hz (C3) | Center frequency of grains. CV tracks 1V/octave. |
| **Streams** | 1-20 | 10 | Number of simultaneous grain generators |
| **Shape** | 0-1 | 0 (sine) | Grain waveform: 0=sine, 0.33=triangle, 0.66=sawtooth, 1.0=square |
| **Range** | 0-500 Hz | 100 Hz | Frequency deviation around center frequency |
| **Duration** | 1-100 ms | 20 ms | Length of individual grains |
| **Delay** | 0.1-200 ms | 0.1 ms | Manual override for time between grains |
| **Density** | 1-1000 grains/sec | 100 | Primary control for grain generation rate per stream |
| **Variation** | 0-100% | 50% | Stochastic variation: 0%=quasi-synchronous, 100%=fully asynchronous |
| **Spread** | 0-100% | 50% | Stereo width: 0%=mono center, 100%=wide stereo |

## Inputs

Each of the 9 parameters has a dedicated CV input (±5V bipolar).

| Input | Function |
|-------|----------|
| **VCA** | Amplitude control (0-5V unipolar). 0V = silence, 5V = full volume. |

## Outputs

| Output | Function |
|--------|----------|
| **Left** | Stereo left channel |
| **Right** | Stereo right channel |

## Technical Details

- Each stream independently generates up to 20 overlapping grains
- Hann window envelope on each grain prevents clicks
- Per-grain random panning with equal-power panning law
- Intelligent gain scaling: `gain = 1/sqrt(activeGrainCount * 0.5)` prevents clipping with many grains
- Variation uses exponential scaling below 30% for tighter control in quasi-synchronous mode

## Patch Ideas

**Dense Cloud Texture**: Streams=15-20, Duration=10-30ms, Density=200-500, Variation=60-100%, Spread=80-100%.

**Quasi-Synchronous Pitched**: Streams=8-12, Duration=20ms, Delay=20ms, Variation=5-15%, Spread=30-50%. Creates tonal effects from granular synthesis.

**Sparse Granular**: Streams=3-8, Duration=5-15ms, Delay=50-200ms, Variation=40-80%, Spread=60-100%. Individual grains become audible.

**Rhythmic Pulse**: Streams=10-15, Duration=10-20ms, Delay=100-150ms, Variation=0-10%, Spread=20-40%. Regular rhythmic granular pattern.

## Historical Context

Based on Barry Truax's GSX system developed in 1985-86 for the DMX-1000 digital signal processor. First real-time granular synthesis implementation, used to create the seminal work "Riverrun" (1986).
