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

#### Fugue X (Expander)

An expander module for Fugue that adds per-voice controls for steps, range, sleep, and probability, plus sorted CV outputs and per-step trigger outputs. Place to the right of Fugue to connect automatically.

**Per-Voice Controls (A, B, C):**
- **Steps** (1-8): Independent step count per voice (overrides Fugue's global steps)
- **Range** (1V/2V/5V): Independent fader range per voice (overrides Fugue's global range)
- **Sleep** (0-64 steps): Number of clock ticks to skip between active steps — creates rests and rhythmic variation
- **Probability** (0-100%): Chance that each step actually fires its gate. At 100% every step plays; lower values introduce random silences.

**Additional Features:**
- **Sample & Hold Mode** - Toggle: when enabled, held notes sustain through rests instead of returning to silence
- **Randomize Sequence** - Button + trigger input to randomize the parent Fugue's pitch faders
- **LED Matrix** - 8×3 grid showing the current step position for each voice, plus sleep indicator LEDs
- **Per-Step Trigger Outputs** - 24 individual trigger outputs (8 steps × 3 voices) for driving external modules from specific sequence positions

**Sorted CV Outputs:**
- **Max** - Highest of the three voice CV values
- **Mid** - Middle value
- **Min** - Lowest value

**All per-voice parameters have CV inputs** (±5V).

### Phase

A dual sample looper inspired by Steve Reich's phase compositions. Two loops play the same or different audio samples with independent drift controls that create gradual phase relationships. Each loop has a mode switch choosing between Sleep (silence gap after each cycle) and Rotate (continuous tape-style content drift).

**Features:**
- **Dual Loop Playback** - Load one or two WAV files; loading only one cascades to both loops
- **Sleep Mode** - Adds silence after each loop cycle, creating phase drift through timing gaps
- **Rotate Mode** - Continuous content drift like tape machines at slightly different speeds
- **Bipolar Drift** (-500ms to +500ms) - Positive and negative drift in both modes
- **Reverse Playback** - Speed range of -4x to +4x with center at 0 (stopped)
- **Transient Detection** - Energy-based onset detection with adjustable sensitivity and minimum gap
- **WAV Cue Point Support** - Embedded cue markers used as transient positions when present
- **Clock-Triggered Jumps** - Per-loop clock input jumps playhead to next detected transient
- **Loop Region Controls** - Draggable bracket handles on waveform display, plus Start/Length CV inputs
- **Waveform Display** - Dual waveform view with playhead, transient markers, loop handles, and rotation origin line
- **VCA Anti-Click** - 1ms envelope on all discontinuities (enabled by default)

**Controls:**
- **Drift** (-500 to +500 ms): Phase drift amount per loop cycle. In Sleep mode: positive = silence gap, negative = early restart. In Rotate mode: continuous speed offset.
- **Speed** (-4x to +4x, default 1x): Playback speed and direction. Center = stopped.
- **Pan** (-1 to +1): Per-loop stereo position with equal-power panning.
- **Mode Switch** (SLP/ROT): Sleep or Rotate mode per loop.

**Inputs:**
- **CLK A/B** - Trigger: jump to next transient
- **START A/B** (0-10V) - Loop start position (0-100% of sample)
- **LEN A/B** (0-10V) - Loop length (0-100% of remaining sample)
- **Drift CV, Speed CV, Pan CV** (±5V) - Per-loop parameter modulation
- **PLAY GATE** - High (>=1V) = play, overrides button
- **SYNC** - Trigger: reset both loops to start

**Outputs:**
- **Left / Right** - Stereo output pair

**Context Menu:**
- Load/Clear Sample A and B
- Transient sensitivity (High/Medium/Low) and minimum gap (10/50/100ms)
- Re-detect Transients (overrides WAV cue points)
- VCA Mode toggle (anti-click envelope)

### Overtone

An additive synthesis VCO that builds waveforms from the harmonic series. The fundamental is always present; 8 toggle switches enable/disable overtones (harmonics 2-9) with natural 1/n amplitude falloff. All overtones on produces a saw-like wave; all off gives a pure sine.

**Features:**
- **8 Harmonic Toggles** - Individual on/off for harmonics 2 through 9
- **Natural Amplitude Falloff** - Each harmonic scaled by 1/n (H2=0.5, H3=0.33, etc.)
- **Even/Odd Filter** - 3-position switch: All, Odd only (square-like), Even only
- **Binary Mask CV** - 0-10V mapped to 8-bit pattern for voltage-controlled timbre
- **Sweep Mask Mode** - Alternative CV mode: 0-10V enables 0-8 harmonics from bottom up
- **Zero-Crossing Gating** - Click-free transitions when toggling harmonics
- **Waveform Display** - Shows composite wave with faint individual harmonic traces and fundamental
- **LED Indicators** - Show actual active state after filter/CV processing

**Controls:**
- **Harmonic 2-9 Toggles**: Enable/disable individual overtones (default: all on)
- **Even/Odd/All Switch**: 3-position filter for harmonic selection
- **Freq** (-4 to +4 octaves, default C4): Coarse frequency control, log2 scaled

**Inputs:**
- **V/Oct** - 1V/octave pitch tracking
- **Mask** (0-10V) - Binary (8-bit) or sweep (0-8 harmonics) mode, selected via context menu
- **Filter** (0-5V) - Even/odd filter CV (0-1.67V=All, 1.67-3.33V=Odd, 3.33-5V=Even)

**Outputs:**
- **Out** - Monophonic audio output (±5V normalized)

**Context Menu:**
- Mask CV Mode: Binary (8-bit pattern) or Sweep (bottom-up harmonics)

### Intone

A CHANT/FOF formant synthesis voice inspired by the IRCAM CHANT project (Rodet, Potard, Barriere, 1984). Generates vocal-character sound using 5 parallel formant cells, each producing overlapping FOF (Formant Wave Function) grains — damped sinusoids at formant frequencies.

**Features:**
- **5 Parallel Formant Cells** - Each generating overlapping FOF grains at independently controllable center frequencies
- **Vowel Morph Slider** - Smoothly interpolates through /a/, /e/, /i/, /o/, /u/ formant presets
- **Per-Formant Controls** - Frequency offset, bandwidth, and amplitude knobs with CV inputs
- **Skirt Width** - Controls the spectral slope of each formant peak (FOF attack rate)
- **Spectrum Display** - 5 formant bell curves + composite envelope on a logarithmic frequency axis
- **Three Excitation Modes:**
  - **Default** (nothing patched): FOF vocal VCO, V/Oct controls pitch
  - **Audio mode** (audio + switch up): Parallel resonant bandpass filter bank — input audio is formant-filtered through the 5 vowel resonances, V/Oct transposes the formant pattern
  - **Trigger mode** (clock + switch down): External trigger fires FOF grains for rhythmic vowel hits, V/Oct transposes formants

**Controls:**
- **Formant 1-5 Frequency** (±1 octave offset): Adjusts each formant relative to the vowel morph preset
- **Formant 1-5 Bandwidth** (30-500 Hz): Width of each formant resonance
- **Formant 1-5 Amplitude** (0-1): Level of each formant
- **Vowel Morph** (slider): Sweeps through /a/ → /e/ → /i/ → /o/ → /u/
- **Skirt**: Spectral slope control (soft to hard formant edges)
- **Mode Switch** (Audio/Trigger): Selects excitation mode when EXC is patched

**Inputs:**
- **V/Oct** - Pitch (default mode) or formant transposition (audio/trigger modes)
- **EXC** - Excitation source (audio or trigger, behavior depends on mode switch)
- **Formant 1-5 Freq CV, Formant 1-5 BW CV** (±5V) - Per-formant modulation
- **Vowel CV** (0-10V) - Vowel morph position
- **Skirt CV** (±5V) - Skirt width modulation

**Outputs:**
- **Out** - Monophonic audio output

### Tine

A tunable 3rd-order pingable resonator based on the Gamelan Resonator circuit from Paul DeMarinis' *Pygmy Gamelan* (1973), analyzed by Werner & Teboul (AES Convention Paper 10542, 2021). The unique 3rd-order active filter topology — distinct from classic Bridged-T and Twin-T designs — produces metallic, bell-like ringing tones when pinged.

**Features:**
- **3rd-Order IIR Filter** - Bilinear transform of the analog Gamelan Resonator transfer function with frequency pre-warping for accurate V/Oct tracking
- **Variable Damping** - From short percussive thumps to long metallic rings approaching self-oscillation
- **Trigger Input** - Accepts gates and triggers via rising edge detection
- **Manual Ping Button** - Immediate excitation without patching
- **Damping CV** - Expressive ring time modulation
- **VCA Anti-Click Mode** - Crossfade envelope on retrigger eliminates zero-crossing clicks (default: on)
- **Double-Precision Filter** - Numerical stability at high damping values

**Controls:**
- **Freq** (-4 to +4 octaves, default C4): Pitch, log2 scaled with V/Oct tracking
- **Damp** (0-1, default 0.5): Ring time from short thump to long sustain

**Inputs:**
- **TRIG** - Trigger/gate input (rising edge fires ping)
- **V/Oct** - 1V/octave pitch CV
- **Damp CV** (±5V) - Damping modulation

**Outputs:**
- **Out** - Monophonic audio output

**Context Menu:**
- VCA Mode (anti-click): Toggle crossfade envelope on retrigger (default: on)

## Building

See [build-doc.md](build-doc.md) for detailed build instructions.

```bash
# Quick start
export PATH="/opt/homebrew/bin:$PATH"
./build.sh dev   # Development build + auto-install
./build.sh prod  # Production build
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) for details.
