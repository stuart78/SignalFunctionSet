# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a VCV Rack plugin called "Signal Function Set" that provides modular synthesizer modules. The plugin is currently in development and contains one module: Drift - a phase-shifted LFO with offset, attenuation, and stability controls featuring Lorenz attractor-based chaos functionality.

## Build Commands

- `make` - Build the plugin (creates plugin.dylib)
- `make clean` - Clean build artifacts 
- `make dist` - Create distribution package
- `RACK_DIR=.. make` - Build with explicit Rack SDK path (default is two directories up)
- `RACK_DIR=.. make clean` - Clean with explicit Rack SDK path
- `RACK_DIR=.. make dist` - Create distribution with explicit Rack SDK path

The build system uses the VCV Rack plugin framework via `$(RACK_DIR)/plugin.mk`.

## Code Architecture

### Plugin Structure
- `src/plugin.hpp` - Main plugin header with model declarations
- `src/plugin.cpp` - Plugin initialization and model registration
- `src/quadlfo.cpp` - Drift module implementation

### Module Development Pattern
Each module follows the VCV Rack module pattern:
- Inherits from `Module` class
- Defines `ParamId`, `InputId`, `OutputId`, and `LightId` enums
- Implements `process()` method for audio processing
- Has corresponding widget class for UI

### Drift Module Architecture
The Drift module implements:
- 4 phase-shifted LFO outputs (A, B, C, D)
- Lorenz attractor chaos system for each output
- Parameters: shape, stability, frequency, X spread, center, Y spread
- Inputs: CV control for all parameters
- Outputs: min/max values plus 4 LFO outputs

### Resources
- `res/` directory contains SVG panel designs
- `plugin.json` defines plugin metadata and module listings
- Panel SVGs are referenced by module widgets for UI rendering

## Development Notes

- The plugin uses VCV Rack SDK framework
- Module widgets are defined in the same file as the module implementation
- SVG panels in `res/` directory define the visual appearance
- Build artifacts go to `build/` and distribution files to `dist/`

## GSX - Granular Synthesis Module

### Overview
GSX is a granular synthesis module that replicates the capabilities of Barry Truax's pioneering GSX system (1985-86). It generates dense textures from hundreds of short sound events called "grains," with real-time control over temporal, spectral, and spatial parameters.

### Technical Background
Granular synthesis operates in the "microsound" domain (typically 1-50ms grain durations) where changes in the time domain produce changes in the frequency/spectral domain. The module supports both quasi-synchronous granulation (which can produce pitch-like effects through amplitude modulation) and asynchronous/stochastic granulation (which creates evolving textures).

### Parameters

Each parameter has:
- Dedicated control knob
- CV input with attenuator
- Range optimized for musical granular synthesis

#### 1. Frequency
- **Range**: 50 Hz to 2000+ Hz
- **Function**: Average frequency of generated grains
- **CV Control**: 1V/octave tracking
- **Notes**: Center frequency around which grains are generated; lower frequencies produce bass textures, higher frequencies create bright, piercing sounds

#### 2. Streams
- **Range**: 1 to 20 streams
- **Function**: Number of simultaneous grain generators
- **CV Control**: Stepped values (integer stream counts)
- **Notes**: More streams = denser texture; fewer streams = individual grains more audible; affects CPU load

#### 3. Shape
- **Range**: Continuous morphing or stepped selection
- **Function**: Grain waveform (Sine → Triangle → Sawtooth → Square)
- **CV Control**: Waveform selection or morphing
- **Notes**: Sine = mellow textures; Square/Saw = rich harmonic content; can mix multiple waveforms for complex timbres

#### 4. Range
- **Range**: 0 Hz to 500+ Hz
- **Function**: Frequency deviation/bandwidth around the center frequency
- **CV Control**: Bipolar or unipolar range control
- **Notes**: 0 Hz = all grains at same frequency; larger values = wider spectral spread and richer textures

#### 5. Duration
- **Range**: 1 ms to 50 ms (microsound domain), extendable to 100ms
- **Function**: Length of individual grains
- **CV Control**: Millisecond range
- **Notes**: Shorter durations = percussive/granular quality; longer durations = smoother textures; affects perceived timbre due to time-domain/frequency-domain relationship

#### 6. Delay
- **Range**: 0 ms to 200+ ms
- **Function**: Time between successive grains
- **CV Control**: Millisecond range
- **Notes**: 0ms = dense/continuous texture (quasi-synchronous); larger values = rhythmic/detached grains; interacts with Duration to create amplitude modulation effects

#### 7. Density
- **Range**: 1 to 1000+ grains per second (per stream)
- **Function**: Rate of grain generation (alternative control to Delay)
- **CV Control**: Grains per second
- **Notes**: Complements or replaces Delay parameter; higher density = more continuous sound; lower density = sparse, scattered grains

#### 8. Variation
- **Range**: 0% to 100%
- **Function**: Amount of random/stochastic variation applied to grain parameters
- **CV Control**: Percentage of randomization
- **Notes**: 0% = quasi-synchronous (regular/pitched); 100% = fully asynchronous (stochastic texture); adds randomness to frequency, duration, timing, and spatial position

#### 9. Spread
- **Range**: 0% to 100% (or -100% to +100% for positioning)
- **Function**: Stereo width and spatial distribution of grain streams
- **CV Control**: Stereo field control
- **Notes**: 0% = mono (center); 100% = wide stereo; controls random panning of individual grains or grain streams across the stereo field

### Outputs

#### Left Out
- Stereo left channel output
- Eurorack level: ±10V (or ±5V for VCV Rack)
- Contains grain streams panned to left and center

#### Right Out
- Stereo right channel output  
- Eurorack level: ±10V (or ±5V for VCV Rack)
- Contains grain streams panned to right and center

### Synthesis Modes

#### Quasi-Synchronous Mode
- Achieved with low Variation and regular Delay values
- Creates pitch through amplitude modulation
- Grain duration determines modulation frequency (e.g., 20ms = 50Hz modulator)
- Can produce tonal effects despite being grain-based synthesis

#### Asynchronous Mode
- Achieved with high Variation values
- Creates stochastic, cloud-like textures
- No clear pitch relationship
- More typical "granular synthesis" sound

### Panel Layout Considerations
- 16-18 HP two-column design
- Each parameter: knob + CV input + attenuator
- Consider mixing knobs (performance parameters: Frequency, Duration, Delay) with sliders (set-and-forget: Streams, Variation, Spread)
- Stereo outputs at bottom of panel

### Implementation Notes

#### DSP Requirements
- Multiple grain generators running simultaneously (up to 20 streams)
- Each grain requires: waveform generation, envelope, frequency/duration/timing control
- Stereo panning and spatial distribution
- Real-time parameter modulation via CV inputs
- Expected sample rate: 48kHz (VCV Rack standard) or 44.1kHz

#### Grain Envelope
- Should use smooth envelope (Gaussian, Hann, or Tukey window) to avoid clicks
- Envelope applied to each grain based on Duration parameter
- Envelope shape affects timbre and smoothness of texture

#### Temporal Accuracy
- Grain timing must be sample-accurate for quasi-synchronous mode to work
- Delay/Density parameters control grain trigger timing
- Variation adds random offset to timing when >0%

#### Memory Considerations
- Multiple active grains per stream require efficient buffer management
- Consider grain pool or voice allocation strategy
- Each stream maintains independent state (phase, timing, spatial position)

### Parameter Interaction Examples

**Dense Texture (Cloud):**
- Streams: 15-20
- Duration: 10-30ms
- Delay: 0-5ms (or Density: 200-500 grains/sec)
- Variation: 60-100%
- Spread: 80-100%

**Quasi-Synchronous (Pitched):**
- Streams: 8-12
- Duration: 20ms (for 50Hz modulation)
- Delay: 20ms (matches duration)
- Variation: 5-15%
- Spread: 30-50%

**Sparse Granular (Detached grains):**
- Streams: 3-8
- Duration: 5-15ms
- Delay: 50-200ms
- Variation: 40-80%
- Spread: 60-100%

**Rhythmic Pulse:**
- Streams: 10-15
- Duration: 10-20ms
- Delay: 100-150ms (regular values)
- Variation: 0-10%
- Spread: 20-40%

### Historical Context
Based on Barry Truax's GSX system developed in 1985-86 for the DMX-1000 digital signal processor. First real-time granular synthesis implementation, used to create the seminal work "Riverrun" (1986). This module preserves the core algorithmic approach while adapting it to the Eurorack/VCV Rack paradigm with voltage control.

### Future Enhancements
- FM synthesis grain mode (add Modulation Index and C:M Ratio parameters)
- Multiple waveform layers (mix multiple shapes simultaneously)
- Sample-based grains (load audio files for grain source)
- Extended envelope shapes
- Additional spatial modes (circular, linear, random walk)