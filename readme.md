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
