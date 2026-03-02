# Signal Function Set

A plugin for [VCV Rack](https://vcvrack.com/) by Stuart Frederich-Smith.

## Modules

### Drift

A 4-channel LFO with chaos capabilities and advanced phase and scaling control.

**Features:**
- **4 Phase-Shifted Outputs** (A, B, C, D) - Each output can be independently phase-shifted using the Phase control
- **Morphing Waveforms** - Shape control smoothly transitions between sine, triangle, sawtooth, square, and chaos
- **Clock Sync** - Can sync to external clock input or run freely at set frequency
- **Flexible Scaling** - Center and Y Spread controls for precise voltage range adjustment
- **Stability Control** - Modulates chaos behavior and waveform characteristics

**Controls:**
- **Shape**: Morphs between waveform types (sine/triangle/sawtooth/square/chaos)
- **Stability**: Adds uncertainty to the waveforms. Higher values are more stable. Stability is calculated independently per output for subtle variation.
- **Center** (bipolar): DC offset for all outputs.
- **Spread**: Controls the amplitude of the outputs. Bipolar, so 5V means ±5V.
- **Frequency**: Sets LFO frequency in Hz. Slow to the left, fast to the right.
- **Phase**: Sets the phase offset relative to A. At 1, A is 0°, B is 90°, C is 180° and D is 270°.

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
- **Morphing Waveforms** - Continuous waveform morphing from sine to triangle to sawtooth to square
- **Dual Synthesis Modes** - Quasi-synchronous mode (pitched/tonal) and asynchronous mode (stochastic clouds)
- **Real-Time Control** - All parameters respond to CV for dynamic sound sculpting
- **Stereo Output** - Spatial distribution with dramatic stereo spread control
- **VCA Control** - External amplitude modulation for expressive dynamics

**Controls:**
- **Frequency** (50-2000 Hz, default C3): Center frequency of generated grains. CV input tracks 1V/octave.
- **Streams** (1-20, default 10): Number of simultaneous grain generators. More streams = denser texture.
- **Shape** (0-1, default 0): Grain waveform morphing: 0=sine, 0.33=triangle, 0.66=sawtooth, 1.0=square.
- **Range** (0-500 Hz, default 100 Hz): Frequency deviation around center frequency.
- **Duration** (1-100 ms, default 20 ms): Length of individual grains.
- **Delay** (0.1-200 ms, default 0.1 ms): Manual override for time between grains.
- **Density** (1-1000 grains/sec, default 100): Rate of grain generation per stream.
- **Variation** (0-100%, default 50%): 0% = quasi-synchronous (regular/pitched), 100% = fully asynchronous (cloud textures).
- **Spread** (0-100%, default 50%): Stereo width. 0% = mono center, 100% = dramatic hard left/right panning.

**Inputs:**
- 9 CV inputs (one per parameter, ±5V bipolar) plus VCA input (0-5V unipolar)

**Outputs:**
- **Left / Right**: Stereo output pair

### Fugue

An 8-step harmonic deviation sequencer with three independent CV/gate voices (A, B, C). Each voice reads the same pitch sequence but wanders harmonically according to its own controls — producing shifting counterpoint from a shared origin.

**Features:**
- **Three Independent Voices** - Each with its own clock, gate pattern, and wander control
- **Harmonic Deviation** - Wander control selects notes from musically-informed tiers: chord tones, extensions, chromatic neighbors
- **Harmonic Lock** - Voices bias toward consonance with each other, creating soft harmonic gravity (enabled by default)
- **Clock Normalling** - Clock B normalled to A, Clock C normalled to B. Patch separate clocks for polyrhythmic effects.
- **12 Scales** - Major, Natural Minor, Harmonic Minor, Melodic Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Pentatonic Major, Pentatonic Minor, Chromatic
- **Adaptive Slew** - Portamento that always resolves before the next note arrives
- **Per-Voice Gate Patterns** - 24 toggle buttons (8 steps x 3 voices) for independent rhythmic patterns

**Controls:**
- **Root** (C-B): Root note for scale quantization. CV: 1V = 1 semitone.
- **Scale** (12 modes): Scale selection. CV: 1V = 1 scale index.
- **Steps** (1-8): Active sequence length.
- **Slew** (0-100%): Adaptive portamento.
- **Pitch Faders** (8): Base pitch per step, quantized to the selected scale.
- **Gate Toggles** (3 rows x 8): Per-voice gate on/off per step.
- **Wander A/B/C** (0-100%): Horizontal sliders controlling harmonic deviation per voice.
- **Reset**: Jack + momentary button, returns all voices to step 1.

**Inputs:**
- **Clock A, B, C** - Trigger inputs (B normalled to A, C normalled to B)
- **Reset** - Trigger input
- **Root CV, Scale CV, Steps CV, Slew CV** - Parameter modulation
- **Wander A/B/C CV** - Per-voice wander modulation (±5V)

**Outputs:**
- **CV A, B, C** - 1V/octave pitch
- **Gate A, B, C** - +10V gate

**Context Menu:**
- Fader Range: 1V (1 octave), 2V (2 octaves), or 5V (5 octaves)
- Harmonic Lock: Toggle consonance-biased deviation (default: on)
- Randomize Sequence: Set all faders to random positions

## Building

```bash
# Build with Rack SDK
RACK_DIR=../Rack-SDK make

# Clean build artifacts
RACK_DIR=../Rack-SDK make clean

# Create distribution package
RACK_DIR=../Rack-SDK make dist
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) for details.
