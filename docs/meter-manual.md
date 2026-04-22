# Meter Manual

## Overview

Meter is a time-signature-aware musical clock module. Most VCV clocks are subdivision dividers — they output evenly-spaced pulses at fixed ratios. Meter understands the *musical* structure of those pulses: bars, beats, swing, and time signatures with CV control.

It is designed to be the master clock for a Beat-driven rhythm rig. One Meter feeds many downstream modules (Beats, sequencers, drums), each potentially clocked from a different swung or grid subdivision.

Meter is 18HP.

## Concepts

### Subdivisions

Meter outputs six standard musical subdivisions, each at the rate implied by the current time signature:

- **BAR** — one pulse at the start of every bar (the downbeat)
- **QUARTER** — quarter notes
- **EIGHTH** — eighth notes
- **SIXTEENTH** — sixteenth notes
- **QUARTER TRIPLET** — three pulses per quarter note
- **EIGHTH TRIPLET** — three pulses per eighth note

In 4/4 time the bar contains 4 quarters / 8 eighths / 16 sixteenths / 12 quarter-triplets / 24 eighth-triplets.

### Time Signature

Meter exposes both the **numerator** (1–16, the top of the time signature) and **denominator** (1, 2, 4, 8, 16, 32 — the bottom) as snapped knobs with CV inputs. Time signature CV lets you sequence meter changes from another module.

By default, time signature changes are *queued for the next bar boundary* so the current bar plays to completion. A context-menu option lets you apply changes immediately if you want unmusical glitching.

### Swing (per output)

Each subdivision (except BAR) has its own **swing trimpot** (-50% to +50%) and matching CV input.

- **Positive swing**: off-beats are delayed, producing the standard shuffle / swing feel
- **Zero**: pulses on the grid
- **Negative swing**: off-beats fire early, producing a rushed / anticipatory feel

Swing changes are **latched per bar** — the value at the end of each bar is committed for the next bar. This prevents mid-period glitches (e.g. the period-being-raced-toward changing while a pulse is in flight, which would either fire it early or swallow it).

### Swung vs Grid Outputs

For each swingable subdivision, Meter provides **two output jacks**:

- **SWG (swung)** — pulses with the swing offset applied
- **GRD (grid)** — pulses at exact base-period intervals, unaffected by swing

You can patch the swung output of one subdivision into one drum and the grid output of another into a different drum, all from the same Meter — letting you mix straight and grooved feels in the same patch.

BAR has no swing, so its single output is always on the grid.

### External Clock Sync

Patch an external clock pulse train into the **EXT CLOCK** input and Meter measures the inter-pulse interval to derive BPM. The PPQN (pulses per quarter note) is selectable in the context menu (1, 2, 4, 8, 12, 16, or 24). One-pole smoothing reduces jitter so the measured BPM stays stable.

When EXT CLOCK is patched and a measurement is available, the BPM knob is overridden and the on-screen BPM readout shows the measured value with a flashing sync indicator next to it.

## Display

Top status line:
- **Left**: current BPM (numeric)
- **Sync indicator**: small dot to the right of BPM when EXT CLOCK is patched — flashes orange on each external pulse, dim when idle
- **Center**: time signature `4/4`, with optional pending change `→ 7/8` shown to the right when a change is queued
- **Right**: `BAR N` counter — increments each bar, resets to 1 on Reset

Six per-output **hit indicator rows** below the status line. Each row visualises that output's pulse positions across the bar:
- Faint baseline through each row
- Bright tick marks at each fire position
- Dim "ghost" ticks at the un-swung positions plus a connector line to the actual swung position
- The tick that just fired flashes from blue → orange and decays over ~100ms

**Position tracker** at the bottom: one cell per sixteenth-note in the bar, with the current sixteenth highlighted in orange, beat boundaries in mid-purple, others in dim purple.

## Controls

| Control | Type | Range | Default | Function |
|---------|------|-------|---------|----------|
| **BPM** | RoundBlackKnob | 30–300 | 120 | Quarter notes per minute (DAW convention) |
| **NUM** | Knob (snap) | 1–16 | 4 | Time signature numerator |
| **DEN** | configSwitch | 1, 2, 4, 8, 16, 32 | 4 | Time signature denominator |
| **RUN** | Light latch (green) | — | On | Play/stop toggle |
| **RST** | Momentary button | — | — | Reset to bar 1, position 0; fires Reset OUT |
| **Swing** (×5) | Trimpot | -0.5 to +0.5 | 0 | Per-subdivision swing amount |

## Inputs

| Input | Type | Function |
|-------|------|----------|
| **BPM CV** | ±5V (~27 BPM/V) | BPM modulation, sums with knob |
| **NUM CV** | CV | Numerator modulation |
| **DEN CV** | CV | Denominator stepping |
| **RUN gate** | Gate | Overrides Run button when patched |
| **EXT CLOCK** | Trigger | External clock; overrides internal BPM when patched |
| **Swing CV** (×5) | ±5V → ±50% | Per-subdivision swing modulation, sums with trimpot |

## Outputs

All gate outputs are 1ms 10V pulses via `dsp::PulseGenerator`.

| Output | Function |
|--------|----------|
| **BAR** | Downbeat per bar |
| **Q SWG / Q GRD** | Quarter notes (swung / on-grid) |
| **8 SWG / 8 GRD** | Eighth notes |
| **16 SWG / 16 GRD** | Sixteenth notes |
| **QT SWG / QT GRD** | Quarter triplets |
| **8T SWG / 8T GRD** | Eighth triplets |
| **RESET** | 1ms trigger that fires when the Reset button is pressed — Meter is the master, downstream modules receive reset via this jack |

## Context Menu

- **External Clock PPQN** selector (1, 2, 4, 8, 12, 16, 24)
- **Apply time signature changes immediately** (default off — changes queue for next bar)
- **Reset on play** (default off — Run after Stop resumes from current position)
- **Detected: NN.N BPM** label when EXT clock is connected and measuring

## Patch Ideas

**Master clock + 3-piece kit**: Meter QUARTER (grid) → kick Beat clock; Meter EIGHTH (swung) → hi-hat Beat clock; Meter SIXTEENTH (swung) → snare Beat clock. Meter BAR → all three Beat BAR inputs. Meter RESET → all RESET inputs. Now you can dial swing per drum from the master.

**Cross-rhythm playground**: Patch Meter QUARTER TRIPLET into one Beat and Meter EIGHTH into another. The triplet fires 3-against-2 against the eighth, producing classic polyrhythms.

**Sequenced time signatures**: Patch a sequencer or LFO into NUM CV and DEN CV to morph the time signature mid-piece. Pending changes apply at the next bar boundary by default, so the music stays musical.

**External tempo follow**: Patch a hardware MIDI clock or a Clk module into EXT CLOCK at 24 PPQN. Meter follows the external tempo while still emitting its own subdivisions, swing, and bar boundaries.
