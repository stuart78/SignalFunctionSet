# Phase Manual

## Overview

Phase is a dual sample looper inspired by Steve Reich's phase compositions (*It's Gonna Rain*, *Piano Phase*). Two loops play the same or different audio samples with independent drift controls that create gradual phase relationships. Each loop has a mode switch choosing between Sleep (silence gap after each cycle) and Rotate (continuous tape-style content drift).

Phase is 18HP. Stereo output.

## Concepts

### Phase Drift

Each loop has a bipolar Drift parameter (-500ms to +500ms) and a mode switch:

**Sleep Mode (SLP)**: After a loop completes one cycle, it waits the drift duration before restarting. Positive values add silence; negative values cut the loop short. If Loop A has 10ms drift and Loop B has 0ms, Loop A's effective period is `sample_length + 10ms` while Loop B's is `sample_length`. They gradually drift apart.

**Rotate Mode (ROT)**: The loop plays continuously with no gaps. The drift parameter controls a tiny speed offset that causes the content to gradually rotate within the loop — like two tape machines running at slightly different speeds. This creates a subtle pitch shift proportional to the drift rate (e.g., 10ms over 5s = 0.2% = ~3.5 cents — inaudible).

### Sample Cascade

If you load only Sample A, it automatically cascades to Sample B. This is the classic Reich workflow — same sample in both loops. Loading Sample B explicitly breaks the cascade.

### Transient Detection

Phase performs energy-based onset detection on each sample when loaded. Detected transients become jump targets for the CLK inputs — each clock pulse advances the playhead to the next transient within the loop region.

If a WAV file contains embedded cue points, those are used as transient markers instead of auto-detection.

## Controls

### Per Loop (A and B)

| Control | Range | Default | Function |
|---------|-------|---------|----------|
| **Drift** | -500 to +500 ms | 0 ms | Phase drift amount and direction |
| **Speed** | -4x to +4x | 1x | Playback speed and direction. Center = stopped. |
| **Pan** | -1 to +1 | 0 (center) | Stereo position with equal-power panning |
| **Mode Switch** | SLP/ROT | SLP (Sleep) | Selects drift mode |

### Global

- **Play** — green LED latch button, toggles play/stop
- **Sync** — momentary button, resets both loops to start

## Inputs

### Per Loop (A and B)

| Input | Range | Function |
|-------|-------|----------|
| **CLK** | Trigger | Jump to next transient |
| **START** | 0-10V | Loop start position (0-100% of sample) |
| **LEN** | 0-10V | Loop length (0-100% of remaining sample after start) |
| **Drift CV** | ±5V | 50ms/V |
| **Speed CV** | ±5V | 0.8x/V |
| **Pan CV** | ±5V | 0.2/V |

### Global

| Input | Function |
|-------|----------|
| **PLAY GATE** | High (>=1V) = play, overrides button |
| **SYNC** | Trigger: reset both loops |

## Outputs

| Output | Function |
|--------|----------|
| **Left** | Stereo left mix |
| **Right** | Stereo right mix |

## Waveform Display

- **Top half**: Sample A waveform (blue)
- **Bottom half**: Sample B waveform (orange)
- **White vertical line**: Playhead position
- **Subtle vertical lines**: Transient markers
- **Bracket handles**: Draggable loop start/end (zoom-aware sensitivity)
- **Dim overlay**: Regions outside the active loop
- **Origin line**: Shows where the original start has drifted to in Rotate mode
- **Rotated waveform**: In Rotate mode, the waveform image rotates to match audio content

## Sample Loading

Right-click menu:
- **Load Sample A / B** — WAV files, mono or stereo (mixed to mono on load), resampled to 48kHz
- **Clear Sample A / B** — Remove loaded sample
- Maximum sample length: 10 minutes

## Transient Detection

Right-click menu controls:
- **Re-detect Transients** — Re-run with current settings (overrides WAV cue points)
- **Sensitivity**: High / Medium (default) / Low
- **Min Transient Gap**: 10ms / 50ms / 100ms (default)

## VCA Mode (Anti-Click)

Default: on. Toggle in right-click menu. Applies a 1ms fade envelope around all discontinuities — loop restarts, transient jumps, sync resets, sleep wake-ups.

## Patch Ideas

**Steve Reich Phase Drift**: Load same sample in both loops. Drift A: 5-10ms, Drift B: 0ms. Pan A left, B right.

**Tape Machine Drift**: Same setup, both in Rotate mode. Continuous seamless drift, subtle pitch shift adds tape-like character.

**Transient Slicer**: Load rhythmic material. High sensitivity detection. Clock both loops from different clock dividers.

**Granular Scanning**: LFO on START CV to sweep through the sample. Short LEN for small loop windows. Different LFO rates on A and B.
