# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a VCV Rack plugin called "Signal Function Set" that provides modular synthesizer modules. The plugin contains two modules:

1. **Drift** - A phase-shifted LFO with offset, attenuation, and stability controls featuring Lorenz attractor-based chaos functionality
2. **GSX** - A granular synthesis module based on Barry Truax's pioneering GSX system (1985-86), fully implemented and operational

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
- `src/gsx.cpp` - GSX granular synthesis module implementation

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
GSX is a fully implemented granular synthesis module that replicates the capabilities of Barry Truax's pioneering GSX system (1985-86). It generates dense textures from hundreds of short sound events called "grains," with real-time control over temporal, spectral, and spatial parameters.

### Implementation Status
**COMPLETE AND OPERATIONAL** - The module is fully functional and sounds very close to Barry Truax's original GSX system. All 9 parameters, 10 CV inputs, VCA control, and stereo outputs are working correctly.

### Technical Background
Granular synthesis operates in the "microsound" domain (typically 1-50ms grain durations) where changes in the time domain produce changes in the frequency/spectral domain. The module supports both quasi-synchronous granulation (which can produce pitch-like effects through amplitude modulation) and asynchronous/stochastic granulation (which creates evolving textures).

### Parameters

Each parameter has:
- Dedicated control knob
- CV input (±5V bipolar for all parameters, 0-5V unipolar for VCA)
- Range optimized for musical granular synthesis

#### 1. Frequency
- **Range**: 50 Hz to 2000 Hz
- **Default**: 130.81 Hz (C3)
- **Function**: Center frequency of generated grains
- **CV Input**: 1V/octave standard (±∞V, bipolar)
- **Notes**: Center frequency around which grains are generated; lower frequencies produce bass textures, higher frequencies create bright, piercing sounds. CV input multiplies frequency exponentially for musical pitch tracking.

#### 2. Streams
- **Range**: 1 to 20 streams
- **Default**: 10 streams
- **Function**: Number of simultaneous grain generators
- **CV Input**: ±5V = ±10 streams (bipolar)
- **Notes**: More streams = denser texture and smoother sound; fewer streams = individual grains more audible. Each stream independently generates up to 20 overlapping grains. Affects CPU load.

#### 3. Shape
- **Range**: 0.0 to 1.0 (Sine → Triangle → Sawtooth → Square)
- **Default**: 0.0 (Sine wave)
- **Function**: Grain waveform with continuous morphing
- **CV Input**: ±5V = full range (bipolar)
- **Notes**: 0.0=sine (mellow), 0.33=triangle, 0.66=sawtooth, 1.0=square (bright/rich harmonics). Smooth morphing between waveforms for complex timbres.

#### 4. Range
- **Range**: 0 Hz to 500 Hz
- **Default**: 100 Hz
- **Function**: Frequency deviation/bandwidth around center frequency
- **CV Input**: ±5V = ±500 Hz (bipolar)
- **Notes**: 0 Hz = all grains at same frequency; larger values = wider spectral spread and richer textures. Combined with Variation to control frequency randomness.

#### 5. Duration
- **Range**: 1 ms to 100 ms
- **Default**: 20 ms
- **Function**: Length of individual grains
- **CV Input**: ±5V = ±100 ms (bipolar)
- **Notes**: Shorter durations = percussive/granular quality; longer durations = smoother textures. Affects perceived timbre due to time-domain/frequency-domain relationship. Typical microsound domain is 1-50ms.

#### 6. Delay
- **Range**: 0.1 ms to 200 ms
- **Default**: 0.1 ms (minimum, Density is primary control)
- **Function**: Manual override for time between grains
- **CV Input**: ±5V = ±200 ms (bipolar)
- **Notes**: Overrides Density when >0.2ms. Lower values = dense/continuous texture; larger values = rhythmic/detached grains. Interacts with Duration to create amplitude modulation effects in quasi-synchronous mode.

#### 7. Density
- **Range**: 1 to 1000 grains per second (per stream)
- **Default**: 100 grains/sec
- **Function**: Primary control for grain generation rate
- **CV Input**: ±5V = ±1000 grains/sec (bipolar)
- **Notes**: Primary timing control (converted to delay internally: delay = 1/density). Higher density = more continuous sound; lower density = sparse, scattered grains. Each stream generates grains at this rate.

#### 8. Variation
- **Range**: 0% to 100%
- **Default**: 50%
- **Function**: Amount of stochastic variation applied to grain parameters
- **CV Input**: ±5V = ±100% (bipolar)
- **Notes**: 0% = quasi-synchronous (regular/pitched); 100% = fully asynchronous (stochastic cloud texture). Adds randomness to frequency (with Range), duration, timing, and spatial position. Uses exponential scaling below 30% for tighter control.

#### 9. Spread
- **Range**: 0% to 100%
- **Default**: 50%
- **Function**: Stereo width and spatial distribution
- **CV Input**: ±5V = ±100% (bipolar)
- **Notes**: 0% = mono (center); 100% = wide stereo. Controls random panning of individual grains across the stereo field using equal-power panning.

### Inputs/Outputs

#### VCA Input
- **Range**: 0-5V (unipolar)
- **Function**: Linear VCA control over final output
- **Notes**: 0V = silence, 5V = full volume. Applied after intelligent gain scaling. Use for envelope control, ducking, or external amplitude modulation.

#### Left Output
- Stereo left channel output
- VCV Rack level: ±10V
- Contains grain streams with equal-power panning

#### Right Output
- Stereo right channel output
- VCV Rack level: ±10V
- Contains grain streams with equal-power panning

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

### Panel Layout (Implemented)
- **Width**: 3-column design at X positions: 10.16mm, 30.48mm, 50.8mm
- **Knobs (3 rows)**:
  - Row 1 (Y=28.69mm): Frequency, Streams, Shape
  - Row 2 (Y=59.17mm): Range, Duration, Delay
  - Row 3 (Y=89.65mm): Density, Variation, Spread
- **CV Inputs (positioned directly below knobs)**:
  - Row 1 (Y=41.39mm): Frequency CV, Streams CV, Shape CV
  - Row 2 (Y=71.87mm): Range CV, Duration CV, Delay CV
  - Row 3 (Y=102.35mm): Density CV, Variation CV, Spread CV
- **Bottom Row (Y=120.13mm)**: VCA input (10.16mm), Left output (40.64mm), Right output (50.8mm)
- **Panel**: res/gsx.svg

### Implementation Details

#### Core DSP Architecture (src/gsx.cpp)
- **20 streams** maximum (MAX_STREAMS = 20)
- **20 grains per stream** (GRAINS_PER_STREAM = 20) for dense textures
- Each grain has **separate envelope and waveform phases** (critical for correct sound):
  - `envelopePhase`: 0-1 over grain lifetime (controls Hann window)
  - `wavePhase`: 0-1, wraps continuously (oscillates at grain frequency)
- Sample rate: 48kHz (VCV Rack standard)

#### Grain Structure
```cpp
struct Grain {
    bool active;
    float envelopePhase;  // 0-1 over grain duration
    float wavePhase;      // 0-1, wraps for oscillation
    float frequency;      // Hz
    float duration;       // seconds
    float pan;           // 0=left, 1=right
};
```

#### Grain Envelope
- **Hann window** used for smooth grain envelope: `0.5 * (1 - cos(2π * phase))`
- Prevents clicks and artifacts
- Applied to grain amplitude based on `envelopePhase`

#### Waveform Generation
- Continuous morphing between 4 waveforms: Sine → Triangle → Sawtooth → Square
- Shape parameter (0-1) controls blend
- Each grain oscillates at its assigned frequency throughout its duration
- Waveform phase advances independently from envelope phase

#### Intelligent Gain Scaling
- Automatic gain compensation based on active grain count
- Formula: `gain = clamp(1.0 / sqrt(activeGrainCount * 0.5), 0.15, 1.0)`
- Prevents clipping with many grains while maintaining presence with few grains
- Scales from 1.0 (1 grain) to 0.15 (100+ grains)
- VCA gain applied after this scaling

#### Temporal Accuracy
- Sample-accurate grain timing for quasi-synchronous mode
- Density primary control: `delay = 1/density`
- Delay parameter overrides when >0.2ms
- Variation adds stochastic offset to timing (exponential scaling below 30%)

#### Spatial Distribution
- Per-grain random panning (not per-stream)
- Equal-power panning law: `leftGain = sqrt(1-pan)`, `rightGain = sqrt(pan)`
- Spread parameter controls width of random distribution

#### Stream Management
- Each stream independently schedules grains
- Round-robin grain allocation within each stream (up to 20 concurrent grains)
- No grain stealing - grains must finish naturally

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
Based on Barry Truax's GSX system developed in 1985-86 for the DMX-1000 digital signal processor. First real-time granular synthesis implementation, used to create the seminal work "Riverrun" (1986). This VCV Rack implementation preserves the core algorithmic approach while adapting it to the modular synthesis paradigm with full voltage control.

### Sound Quality
The implemented module sounds **very close** to Barry Truax's original GSX system. The combination of proper envelope/waveform phase separation, Hann window envelopes, intelligent gain scaling, and per-grain random panning creates authentic granular textures in both quasi-synchronous and asynchronous modes.

### Development History
- Initial implementation with all 9 parameters and CV inputs
- Critical bug fix: Separated envelope phase from waveform phase (grains now oscillate at their frequency throughout their duration, not just one cycle)
- Added VCA input for external amplitude control
- Tuned variation scaling (exponential below 30% for quasi-synchronous mode)
- Optimized CV input ranges to ±5V standard (0-5V for VCA)
- Changed default frequency from A4 (440 Hz) to C3 (130.81 Hz)
- Implemented per-grain random panning for authentic stereo spread

### Possible Future Enhancements
- FM synthesis grain mode (add Modulation Index and C:M Ratio parameters)
- Sample-based grains (load audio files for grain source)
- Alternative envelope shapes (Gaussian, Tukey, custom)
- Grain reverse playback option
- Additional spatial modes (circular panning, motion paths)