# Record Manual

## Overview

Record is an **auto-sampler**. Point it at any voice in your rack (a patch you've built, a granular texture, an FM bell), press RECORD, and it plays that voice across a range of notes and velocities, captures each one, and writes a complete multisampled instrument to disk: a folder of WAVs plus an `.sfz` file.

The result loads straight into [Play](play-manual.md), or into any sampler that reads SFZ.

Record is 16HP. It pairs with Play, but they don't talk over an expander: the handoff is files on disk, so your instruments are yours.

## How it works

Record drives the voice and listens to it come back:

1. **Outputs** V/OCT and GATE (plus VELOCITY) to your patch.
2. **Inputs** the voice's stereo return on L / R.
3. For each note × velocity layer, it holds the gate for **SUSTAIN**, records the **TAIL** after release, waits for silence, and moves on.
4. When the sweep finishes, it trims, normalises, and writes the files.

## Setting up a capture

Patch **V/OCT** and **GATE** into your voice, and the voice's output back into **L / R**. Then set the sweep:

| Control | What it does |
|---|---|
| **START** | Lowest note to sample. |
| **SPACING** | Semitones between samples. 1 = every note (big, accurate); 3–4 is a good balance; 12 = one per octave. |
| **OCTAVES** | How many octaves the sweep covers. |
| **VEL LAYERS** | Velocity layers per note (1 = no velocity switching). |
| **SUSTAIN** | How long the gate is held for each capture. |
| **TAIL** | How long to keep recording after the gate releases. Set this long enough for the release/reverb to decay. |

**AUDITION** runs the same sweep through the voice **without capturing or writing**. Always audition first: it's how you hear whether SUSTAIN and TAIL are right and whether the voice is in tune, before committing to a long capture.

**RECORD** runs the real thing. You'll be asked for a destination folder up front. The status readout shows progress, then `done ✓ N files`.

## The display

**Live scope**: a min/max waveform of the incoming audio, so you can see the voice arriving and confirm your levels.

**Keyboard / grid**, two tabbed views:

- **GRID** (the default): a 12×8 Push-style isomorphic pad grid.
- **PIANO**: the full 88 keys (A0–C8) with octave labels.

The sampled range shows as a translucent orange band with orange root keys; the note currently being captured is cyan.

**The pads are playable.** Tap a pad or key and Record emits V/OCT + GATE to your voice, so you can audition the patch you're about to sample without leaving the module. (Only while idle, so it won't interfere with a capture in progress.)

The grid has three layouts in the context menu (**Grid view → Layout**):

- **Chromatic 4ths**: right = +1 semitone, up = +5 (a bass-guitar-like isomorphic layout).
- **In-Key**: only notes in the chosen Root + Scale.
- **Chromatic grid**: 12 pitch classes per row from C0, up = octave, with accidental columns shaded darker for a piano-roll look and the C column ringed and labelled.

## What gets written

Into your destination folder:

- `NNN_vVVV.wav`: one file per note × velocity (`NNN` = MIDI note, `VVV` = velocity), or `NNN_vVVV_rK.wav` with round-robins.
- `<Instrument>.sfz`: the instrument definition: key ranges, `pitch_keycenter`, velocity zones, tuning, loop points, and round-robin `seq_length` / `seq_position`.

Files are written on the GUI thread when the sweep completes, so recording never glitches the audio.

## Context menu options

**Destination folder** / **Instrument name**: where it goes and what it's called.

**Normalize**: scale each capture to full level.

**Auto-trim**: remove the silence before the note starts.

**Wait for silence between notes** (default on): after the tail, monitor the input and only advance once it's been quiet (~-50dB) for 30ms, with a 3s timeout. This stops a long release bleeding into the next capture. The gate stays low during the wait and it isn't recorded.

**Gate / Trigger**: hold a gate for SUSTAIN, or emit a 1ms pulse per note (for drums and other trigger-driven voices).

**Round-robins (1–4)**: capture each note/velocity several times back to back and write them as a round-robin group. Only worth it if the voice actually varies between takes: noise, randomness, or chaos in the patch. A perfectly deterministic voice gives you four identical files.

**Loop this instrument**: enable loop-point detection (see below).

**Latency calibration**: fires a C4 note, measures how long the sound takes to come back, and stores the delay. Captures are then trimmed to the measured onset rather than by threshold, which is more accurate for quiet or slow-attack voices. There's a "Clear latency" item to reset it.

**File options**: channels (1 = mono downmix, 2 = stereo) and bit depth (16 / 24 / 32-bit float). Default 24.

## Loops

With **Loop this instrument** on, Record finds a loop point in each capture: a start around 40% in and an end between 68–93%, both on positive zero-crossings, choosing the end that minimises the seam error over a 256-sample window. Those points are written as `loop_start` / `loop_end` in the SFZ, and Play will loop the sample for as long as the note is held.

This works well on steady, sustained material: pads, strings, drones. It's less reliable on anything that evolves through the note, where there may be no honest loop point to find. For one-shots (drums, plucks, bells), leave it off.

## Patch ideas

**Freeze a patch you can't keep**: sample a CPU-heavy granular or FM voice into an instrument you can play polyphonically from Play at a fraction of the cost.

**Sample your own hardware**: run an external synth through an audio interface into L/R, with V/OCT + GATE driving a MIDI-CV interface.

**Capture the randomness**: a chaotic voice with round-robins set to 4 gives you an instrument that never repeats itself exactly.

**Drum kit**: Trigger mode, SPACING 1, one octave, several velocity layers, loops off.
