# Phase Manual

## Overview

Phase is a dual sample looper inspired by Steve Reich's phase compositions (*It's Gonna Rain*, *Piano Phase*). Two loops play the same or different audio samples with independent drift controls that create gradual phase relationships. Each loop has a mode switch choosing between Sleep (silence gap after each cycle) and Rotate (continuous tape-style content drift).

Phase is 18HP. Stereo output.

## Concepts

### Phase Drift

Each loop has a bipolar Drift parameter (-500ms to +500ms) and a mode switch:

**Sleep Mode (SLP)**: After a loop completes one cycle, it waits the drift duration before restarting. Positive values add silence; negative values cut the loop short. If Loop A has 10ms drift and Loop B has 0ms, Loop A's effective period is `sample_length + 10ms` while Loop B's is `sample_length`. They gradually drift apart.

**Rotate Mode (ROT)**: The loop plays continuously with no gaps. The drift parameter controls a tiny speed offset that causes the content to gradually rotate within the loop, like two tape machines running at slightly different speeds. This creates a subtle pitch shift proportional to the drift rate (e.g., 10ms over 5s = 0.2% = ~3.5 cents, inaudible).

### Sample Cascade

If you load only Sample A, it automatically cascades to Sample B. This is the classic Reich workflow: same sample in both loops. Loading Sample B explicitly breaks the cascade.

### Transient Detection

Phase performs energy-based onset detection on each sample when loaded. Detected transients become jump targets for the CLK inputs, so each clock pulse advances the playhead to the next transient within the loop region.

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

- **Play**: green LED latch button, toggles play/stop
- **Sync**: momentary button, resets both loops to start
- **Rec A / Rec B**: red LED latch buttons. Click to arm; click again to stop. The LED is dim while armed but waiting for input, bright while actually recording.

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
| **REC A IN** | Audio input for live recording into Sample A (±5V) |
| **REC A GATE** | Gate input. While high, Sample A is recording. Overrides the panel button when patched. |
| **REC B IN** | Audio input for Sample B. Normalled from REC A IN, so patching into A alone feeds both buffers. |
| **REC B GATE** | Gate input for Sample B. Normalled from REC A GATE (unless REC B's panel button is independently armed). |

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
- **Load Sample A / B**: WAV files, mono or stereo (mixed to mono on load), resampled to 48kHz
- **Clear Sample A / B**: Remove loaded sample
- Maximum sample length: 10 minutes

## Live Recording

Each buffer can be recorded into directly from a patched audio source. Recording is **straight-in (replace)**: it overwrites the buffer rather than overdubbing.

### How it works

1. Patch an audio source into **REC A IN** (and optionally **REC B IN** for an independent source).
2. Start recording one of two ways:
   - **Click the REC button** on the panel. Click again to stop.
   - **Send a gate to REC GATE.** Recording happens while the gate is high. The gate overrides the button when patched.
3. While recording, the loop's playback is suppressed and you hear a **dry monitor passthrough** of the incoming audio at the buffer's current Pan position. No loop content plays through, so there's no risk of feedback.
4. On stop, the loop region is reset to `0..length`, the new waveform is rendered, transient detection runs on the captured audio, and playback resumes from sample 0.

### Recording length

Maximum recording length is **60 seconds**. Recording auto-stops if the buffer fills.

### Cascading

REC B IN is normalled from REC A IN, and REC B GATE is normalled from REC A GATE. Patch a single audio source and gate into the A jacks and both buffers will record the same input simultaneously, the equivalent of the file-load cascade ("loading Sample A also loads Sample B"). Patch into B explicitly to record different sources.

### Anti-click

A 1ms fade-in/fade-out envelope is applied to the captured audio at the start and end of each recording, eliminating click artifacts at the boundaries.

### Persistence

Recorded samples are **not** saved with the patch (they can be megabytes of audio data). If you want to keep a recording, capture it to disk in your DAW or by recording the Phase output.

## Transient Detection

Right-click menu controls:
- **Re-detect Transients**: Re-run with current settings (overrides WAV cue points and any manual edits)
- **Sensitivity**: High / Medium (default) / Low
- **Min Transient Gap**: 10ms / 50ms / 100ms (default)

WAV cue points are read from the file (their sample-frame positions, resampled to 48kHz) and used as the transient markers when present, in place of auto-detection.

## Cue Points (manual editing)

The transient markers double as cue points you can edit directly on the waveform (top half = Sample A, bottom half = Sample B):

- **Double-click an empty spot**: add a cue there.
- **Double-click an existing cue**: remove it.
- **Click and drag a cue**: move it (loop-region handles take priority where they overlap).
- Right-click menu: **Clear cue points A / B**.

Manual edits are saved with the patch and restored on load, overriding the file-derived cues. (Re-detect Transients or loading a new file replaces them.)

## VCA Mode (Anti-Click)

Default: on. Toggle in right-click menu. Applies a 1ms fade envelope around all discontinuities: loop restarts, transient jumps, sync resets, sleep wake-ups.

## Patch Ideas

**Steve Reich Phase Drift**: Load same sample in both loops. Drift A: 5-10ms, Drift B: 0ms. Pan A left, B right.

**Tape Machine Drift**: Same setup, both in Rotate mode. Continuous seamless drift, subtle pitch shift adds tape-like character.

**Transient Slicer**: Load rhythmic material. High sensitivity detection. Clock both loops from different clock dividers.

**Granular Scanning**: LFO on START CV to sweep through the sample. Short LEN for small loop windows. Different LFO rates on A and B.

**Live Recorded Phase**: Patch a synth voice into REC A IN. Hit Rec A, play a short phrase, click Rec A again to stop. Both buffers now hold the phrase (cascade). Set Drift A to 5-10ms, Drift B to 0ms, for instant Steve Reich phasing on a live take.

**Gated Loop Capture**: Patch a clock or trigger sequence into REC A GATE so each gate cycle captures a fresh loop into the buffer. With Pan opposed and small Drift, every captured loop becomes its own short phasing study.

**Two-Source Tape Phase**: Patch one source into REC A IN and a different source into REC B IN. Send a shared gate to REC A GATE (B picks it up via normalling). Both buffers capture in lockstep but hold completely different material, so phase drift now creates evolving counterpoint between two distinct sources.
