# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a VCV Rack plugin called "Signal Function Set" that provides modular synthesizer modules. The plugin contains the following modules:

1. **Drift** - A phase-shifted LFO with offset, attenuation, and stability controls featuring Lorenz attractor-based chaos functionality
2. **GSX** - A granular synthesis module based on Barry Truax's pioneering GSX system (1985-86), fully implemented and operational
3. **Fugue** - An 8-step harmonic deviation sequencer with three independent CV/gate voices
4. **Phase** - A dual sample looper with sleep-based phase drift, inspired by Steve Reich's phase compositions

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
- `src/fugue.cpp` - Fugue module implementation
- `src/phase.cpp` - Phase module implementation

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

## Phase - Dual Sample Looper

### Overview
Phase is a dual sample looper inspired by Steve Reich's phase compositions ("It's Gonna Rain," "Piano Phase"). Two loops play the same or different audio samples with independent "sleep" parameters that create gradual phase drift between the loops. Each loop has a mode switch choosing between **Sleep** (silence gap after each cycle) and **Rotate** (continuous content drift like tape machines at slightly different speeds). The module supports forward and reverse playback, per-loop panning, transient detection with clock-triggered jumps, WAV cue point support, adjustable loop regions, and a VCA anti-click mode.

### Implementation Status
**COMPLETE AND OPERATIONAL** - The module is fully functional with all parameters, CV inputs, waveform display, transient detection, and loop region controls working correctly.

### How Phase Drift Works
Each loop has a bipolar "sleep" parameter (-500ms to +500ms) and a mode switch:

**Sleep Mode (SLP):** After a loop completes one cycle, it waits the sleep duration before restarting. Positive values add silence; negative values cut the loop short. If Loop A has 10ms sleep and Loop B has 0ms, Loop A's effective period is `sample_length + 10ms` while Loop B's is just `sample_length`. They gradually drift apart.

**Rotate Mode (ROT):** The loop plays continuously with no gaps. The sleep parameter controls a tiny speed offset that causes the content to gradually rotate within the loop — like two tape machines running at slightly different speeds. The read position drifts by `sleepMs` worth of samples per loop cycle. This creates a subtle pitch shift proportional to the drift rate (e.g., 10ms over 5s = 0.2% = ~3.5 cents — inaudible). Negative sleep rotates in the opposite direction.

### Parameters

#### Per Loop (x2: Loop A and Loop B)

##### 1. Sleep
- **Range**: -500 to +500 ms
- **Default**: 0 ms
- **Function**: Controls phase drift amount and direction
- **CV Input**: ±5V, 50ms/V (bipolar)
- **Notes**: In Sleep mode: positive = silence gap, negative = loop ends early. In Rotate mode: positive = drift forward, negative = drift backward. This is the core phasing mechanism.

##### 2. Speed
- **Range**: -4x to +4x
- **Default**: 1x (center of knob is 0/stopped)
- **Function**: Playback speed and direction
- **CV Input**: ±5V, 0.8x/V (bipolar)
- **Notes**: Negative values play in reverse. 0 = stopped. Speed also affects the effective loop period, providing a second axis for phase drift. Center detent is at 0 (stopped), default is 1x.

##### 3. Pan
- **Range**: -1 (full left) to +1 (full right)
- **Default**: 0 (center)
- **Function**: Stereo panning position for this loop
- **CV Input**: ±5V, 0.2/V (bipolar)
- **Notes**: Uses equal-power panning law (cos/sin). Allows positioning each loop in the stereo field independently.

##### 4. Mode Switch (SLP/ROT)
- **Type**: CKSS toggle switch
- **Function**: Selects between Sleep mode (up) and Rotate mode (down) per loop

### Inputs

#### Per Loop (x2)
| Input | Range | Function |
|-------|-------|----------|
| Sleep CV | ±5V | Modulates sleep time (50ms/V) |
| Speed CV | ±5V | Modulates playback speed (0.8x/V) |
| Pan CV | ±5V | Modulates stereo pan (0.2/V) |
| CLK | Trigger | Jump playhead to next detected transient |
| START | 0-10V | Loop start position (0-100% of sample) |
| LEN | 0-10V | Loop length (0-100% of remaining sample after start) |

#### Global
| Input | Range | Function |
|-------|-------|----------|
| SYNC | Trigger | Reset both loops to their start positions |
| PLAY GATE | Gate | High (>=1V) = play, overrides button state |

### Outputs
| Output | Function |
|--------|----------|
| LEFT | Stereo left mix of both loops |
| RIGHT | Stereo right mix of both loops |

### Controls

#### Play Button
- Green LED latch button
- Toggles play/stop state on click
- When PLAY GATE CV is connected, gate overrides the button state
- Module outputs silence when stopped

#### Sync Button
- Momentary push button
- Resets both loops to their start positions
- Works alongside the SYNC CV input (either triggers a reset)

### Waveform Display

The display occupies the top portion of the module and shows:
- **Top half**: Sample A waveform (blue)
- **Bottom half**: Sample B waveform (orange)
- **Playhead**: White vertical line showing current position per loop
- **Transient markers**: Subtle vertical lines spanning waveform height
- **Loop handles**: Draggable bracket-style start/end handles (solid filled, no transparency stacking)
- **Dim overlay**: Regions outside the active loop are darkened
- **Origin line**: Semi-transparent white line showing where the original sample start has drifted to in rotate mode
- **Rotated waveform**: In rotate mode, the waveform image rotates to match the audio content

#### Loop Region Handles
- Bracket-style handles with 1px vertical bar and horizontal ticks at top/bottom
- Start handles bracket right, end handles bracket left
- Drag sensitivity accounts for zoom level via `getAbsoluteZoom()`
- When START/LEN CV inputs are connected, they override the handle positions
- Loop regions are persisted with patch save/load

### Sample Loading

- Right-click menu: "Load Sample A" / "Load Sample B"
- WAV files supported (mono or stereo, any sample rate — resampled to 48kHz on load)
- Stereo files are mixed down to mono; stereo placement comes from the Pan control
- If only Sample A is loaded, it cascades to Sample B automatically
- Loading Sample B explicitly breaks the cascade
- "Clear Sample A" / "Clear Sample B" removes loaded samples
- Maximum sample length: 10 minutes (28,800,000 samples at 48kHz)
- File paths persist with patch save/load via JSON
- **WAV cue point support**: If the WAV file contains embedded cue points, they are used as transient markers instead of auto-detection. Cue positions are resampled if the file isn't 48kHz. Re-detect Transients from the context menu overrides cues with auto-detection.

### Transient Detection

#### Algorithm
Energy-based onset detection computed once per sample load:
1. RMS energy computed in 1024-sample windows with 256-sample hop
2. High-frequency emphasis via sample differencing (catches transients in noise)
3. Half-wave rectified onset detection function
4. Adaptive threshold based on local mean energy over 30-frame context window
5. Local peak picking with ±2 frame neighborhood
6. Minimum gap enforcement between detected transients
7. Absolute energy floor (0.005) prevents false triggers on quiet/silent passages

#### Context Menu Controls
- **Re-detect Transients**: Re-run detection with current settings (overrides WAV cue points)
- **Sensitivity**: High (0.15), Medium/default (0.7), Low (0.95) — maps to threshold range 1.0-12.0
- **Min Transient Gap**: 10ms (fast), 50ms (medium), 100ms/default (slow)

#### Clock Input Behavior
On rising edge of CLK input, the playhead jumps to the next transient after the current position (within the active loop region). If at the end, wraps to the first transient in the region.

### VCA Mode (Anti-Click)

- **Default**: Enabled
- **Context menu**: "VCA Mode (anti-click)" toggle
- When enabled, applies a 1ms attack/release envelope around all discontinuities:
  - Loop restarts (forward and reverse wrap-around)
  - Transient jumps via clock trigger
  - Sync resets
  - Sleep wake-up transitions
- Eliminates clicks from playhead discontinuities
- Implementation: fade-out (1ms) → execute jump → fade-in (1ms)

### Panel Layout (20HP = 101.6mm wide)

All positions in mm, used with `mm2px()`:

```
WAVEFORM DISPLAY: position (5.8, 14), size 90mm x 24mm

LOOP A:
  Knobs  Y=50:    Sleep(15.24)   Speed(35.56)   Pan(55.88)
  CVs    Y=62:    SleepCV(15.24) SpeedCV(35.56) PanCV(55.88)
  Jacks  Y=50:    ClkA(76.2)     StartA(86.36)  LenA(96.52)
  Switch Y=62:    ModeA(86.36)

LOOP B:
  Knobs  Y=78:    Sleep(15.24)   Speed(35.56)   Pan(55.88)
  CVs    Y=90:    SleepCV(15.24) SpeedCV(35.56) PanCV(55.88)
  Jacks  Y=78:    ClkB(76.2)     StartB(86.36)  LenB(96.52)
  Switch Y=90:    ModeB(86.36)

BOTTOM ROW Y=110:
  Play(15.24)  PlayGate(25.4)  Sync(45.72)  SyncCV(56)  Left(76.2)  Right(91.44)
```

### Implementation Details

#### Core DSP (src/phase.cpp)
- **Double-precision playhead**: Prevents cumulative drift at fractional speeds over long playback
- **Linear interpolation**: Sub-sample accuracy for non-integer speed values
- **Mono mixdown on load**: Simplifies DSP; stereo placement via per-loop pan control
- **Precomputed waveform overview**: 512-point peak array per sample, no per-frame buffer scanning
- **Equal-power panning**: cos/sin law from bipolar [-1,+1] pan position
- **Continuous drift (rotate mode)**: Read position advances at `speed + (sleepSamples/regionLength)` per sample — no discrete jumps, no crossfade needed

#### Data Structures
```cpp
struct SampleData {
    vector<float> samples;        // Mono float samples at 48kHz
    size_t length;                // Number of samples
    string filePath, fileName;    // For persistence and display
    vector<size_t> transients;    // Precomputed transient positions
    vector<float> waveformMini;   // 512-point peak array for display
    bool loaded;
    bool hasCuePoints;            // true if transients from WAV cue chunk
    float loopStart, loopEnd;     // Normalized 0-1 loop region
};

struct LoopState {
    double playhead;              // Current position (double precision)
    bool sleeping;                // In post-loop silence
    float sleepRemaining;         // Sleep countdown in seconds
    SchmittTrigger clockTrigger;
    float envelope;               // VCA mode anti-click envelope (0-1)
    bool ramping;                 // In fade transition
    double jumpTarget;            // Deferred jump destination
    double rotationOffset;        // Accumulated drift in rotate mode
};
```

#### JSON Persistence
Saves and restores: file paths for both samples, explicit-B flag, play state, transient sensitivity, min gap, VCA mode, and loop start/end regions for both samples.

#### Dependencies
- **dr_wav.h**: Header-only WAV loader (included in src/, `#define DR_WAV_IMPLEMENTATION` in phase.cpp). Opened with `drwav_init_file_with_metadata()` to read cue points.
- **osdialog**: File open dialogs (provided by VCV Rack SDK)
- **NanoVG**: Waveform display drawing (provided by VCV Rack SDK)

### Patch Ideas

**Steve Reich Phase Drift:**
- Load same sample in both loops
- Sleep A: 5-10ms, Sleep B: 0ms
- Speed both at 1x
- Pan A left, Pan B right
- Listen as patterns gradually shift

**Tape Machine Drift (Rotate):**
- Load same sample, set both to Rotate mode
- Sleep A: 5ms, Sleep B: 0ms
- Continuous seamless drift with no gaps
- Content gradually rotates — subtle pitch shift adds to the effect

**Reverse Texture:**
- Speed A: 1x, Speed B: -0.5x
- Same sample, different loop regions
- High variation creates evolving textures

**Transient Slicer:**
- Load rhythmic material
- High sensitivity transient detection
- Clock both loops from an external sequencer at different rates
- Each clock pulse jumps to the next transient

**Granular-Style Scanning:**
- Use LFO on START CV to slowly scan through the sample
- Short LEN CV value for small loop windows
- Different LFO rates on A and B for complex interplay

**Bidirectional Drift:**
- Sleep A: +10ms, Sleep B: -10ms
- Loops drift in opposite directions simultaneously
- Creates expanding then contracting phase relationships