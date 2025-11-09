# SignalFunctionSet

This is a in-development plugin for VCV Rack.

## Modules

### Drift

A  4-channel LFO with chaos capabilities and advanced phase and scaling control.

**Features:**
- **4 Phase-Shifted Outputs** (A, B, C, D) - Each output can be independently phase-shifted using the Phase control
- **Morphing Waveforms** - Shape control smoothly transitions between sine → triangle → sawtooth → square → chaos
- **Clock Sync** - Can sync to external clock input or run freely at set frequency
- **Flexible Scaling** - Center and Y Spread controls for precise voltage range adjustment
- **Stability Control** - Modulates chaos behavior and waveform characteristics


**Controls:**
- **Shape**: Morphs between waveform types (sine/triangle/sawtooth/square/chaos)
- **Stability**: Adds uncertaintly to the wave forms. Higer values are more stable. Stability is calcularted independently, so each output will be slightly different. This is useful for sublte 
- **Center** (bipolar): DC offset for all outputs. 
- **Spread**: Controls the amplitude of the oututs. This output is bipoloar, so 5v is +/- 5v.
- **Frequency**: Sets LFO frequency in Hz. Slow to the left, fast to the right.
- **Phase**: Sets the phase offset relative to A. At 1, A is 0°, B is 90°, C is 180° and D is 270°

**Inputs:**
- **Shape**: CV control for waveform morphing
- **Stability**: CV control for stability parameter
- **Clock**: External clock input for sync (overrides frequency control)
- **Phase**: CV control for phase spreading
- **Center**: CV control for DC offset
- **Spread**: CV control for amplitude

**Outputs:**
- **A, B, C, D**: Four phase-shifted LFO outputs
- **Min**: Minimum value across all 4 channels
- **Max**: Maximum value across all 4 channels

### GSX

A real-time granular synthesis module inspired by Barry Truax's groundbreaking GSX system (1985-86), the first implementation of real-time granular synthesis. GSX generates dense textures from hundreds of short sound events called "grains," operating in the microsound domain (1-50ms) where changes in the time domain produce changes in the frequency/spectral domain.

**Features:**
- **Multi-Stream Architecture** - Up to 20 independent grain streams for dense, evolving textures
- **Morphing Waveforms** - Continuous waveform morphing from sine → triangle → sawtooth → square
- **Dual Synthesis Modes** - Quasi-synchronous mode (pitched/tonal) and asynchronous mode (stochastic clouds)
- **Real-Time Control** - All parameters respond to CV for dynamic sound sculpting
- **Stereo Output** - Spatial distribution with dramatic stereo spread control
- **VCA Control** - External amplitude modulation for expressive dynamics

**Controls:**
- **Frequency** (50-2000 Hz, default 130.81 Hz / C3): Center frequency of generated grains. CV input tracks 1V/octave.
- **Streams** (1-20, default 10): Number of simultaneous grain generators. More streams = denser texture.
- **Shape** (0-1, default 0): Grain waveform morphing: 0=sine, 0.33=triangle, 0.66=sawtooth, 1.0=square.
- **Range** (0-500 Hz, default 100 Hz): Frequency deviation/bandwidth around center frequency. 0 Hz = all grains at same pitch, higher values = wider spectral spread.
- **Duration** (1-100 ms, default 20 ms): Length of individual grains. Shorter = percussive/granular, longer = smoother textures.
- **Delay** (0.1-200 ms, default 0.1 ms): Manual override for time between grains (alternative to Density).
- **Density** (1-1000 grains/sec, default 100): Rate of grain generation per stream. Higher = continuous sound, lower = sparse grains.
- **Variation** (0-100%, default 50%): Amount of stochastic variation. 0% = quasi-synchronous (regular/pitched), 100% = fully asynchronous (cloud textures).
- **Spread** (0-100%, default 50%): Stereo width with bias toward extremes. 0% = mono center, 100% = dramatic hard left/right panning.

**Inputs:**
- **Frequency CV**: 1V/octave pitch tracking (±5V)
- **Streams CV**: Modulate number of active streams (±5V, ±10 streams)
- **Shape CV**: Morph between waveforms (±5V maps to full 0-1 range)
- **Range CV**: Modulate frequency bandwidth (±5V = ±500 Hz)
- **Duration CV**: Modulate grain length (±5V = ±100 ms)
- **Delay CV**: Modulate grain spacing (±5V = ±200 ms)
- **Density CV**: Modulate grain rate (±5V = ±1000 grains/sec)
- **Variation CV**: Modulate randomness amount (±5V maps to full 0-1 range)
- **Spread CV**: Modulate stereo width (±5V maps to full 0-1 range)
- **VCA CV**: External amplitude control (0-5V unipolar, linear)

**Outputs:**
- **Left**: Stereo left channel output
- **Right**: Stereo right channel output

**Historical Context:**

Barry Truax developed the original GSX (Granular Synthesis eXperiment) system in 1985-86 for the DMX-1000 digital signal processor at Simon Fraser University. It was the first system capable of real-time granular synthesis, allowing composers to manipulate hundreds of grains per second with immediate auditory feedback. Truax used GSX to create the seminal electroacoustic work "Riverrun" (1986), demonstrating the musical potential of granular synthesis for creating evolving, organic textures that blur the boundaries between tone and noise, rhythm and texture.

This module preserves the core algorithmic approach of Truax's GSX while adapting it to the modular synthesis paradigm with extensive voltage control, making these pioneering techniques accessible for real-time performance and patching.
