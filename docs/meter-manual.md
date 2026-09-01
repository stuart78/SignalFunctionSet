# Meter Manual

## Overview

Meter is a time-signature-aware musical clock module. Most VCV clocks are subdivision dividers: they output evenly-spaced pulses at fixed ratios. Meter understands the *musical* structure of those pulses: bars, beats, swing, and time signatures with CV control.

It is designed to be the master clock for a Beat-driven rhythm rig. One Meter feeds many downstream modules (Beats, sequencers, drums), each potentially clocked from a different swung or grid subdivision.

Meter is 18HP.

## Concepts

### Subdivisions

Meter outputs six standard musical subdivisions, each at the rate implied by the current time signature:

- **BAR**: one pulse at the start of every bar (the downbeat)
- **QUARTER**: quarter notes
- **EIGHTH**: eighth notes
- **SIXTEENTH**: sixteenth notes
- **QUARTER TRIPLET**: three pulses per quarter note
- **EIGHTH TRIPLET**: three pulses per eighth note

In 4/4 time the bar contains 4 quarters / 8 eighths / 16 sixteenths / 12 quarter-triplets / 24 eighth-triplets.

### Time Signature

Meter exposes both the **numerator** (1–16, the top of the time signature) and **denominator** (1, 2, 4, 8, 16, 32; the bottom) as snapped knobs with CV inputs. Time signature CV lets you sequence meter changes from another module.

By default, time signature changes are *queued for the next bar boundary* so the current bar plays to completion. A context-menu option lets you apply changes immediately if you want unmusical glitching.

### Swing (per output)

Each subdivision (except BAR) has its own **swing trimpot** (-50% to +50%) and matching CV input.

- **Positive swing**: off-beats are delayed, producing the standard shuffle / swing feel
- **Zero**: pulses on the grid
- **Negative swing**: off-beats fire early, producing a rushed / anticipatory feel

Swing changes are **latched per bar**: the value at the end of each bar is committed for the next bar. This prevents mid-period glitches (e.g. the period-being-raced-toward changing while a pulse is in flight, which would either fire it early or swallow it).

### Swung vs Grid Outputs

For each swingable subdivision, Meter provides **two output jacks**:

- **SWG (swung)**: pulses with the swing offset applied
- **GRD (grid)**: pulses at exact base-period intervals, unaffected by swing

You can patch the swung output of one subdivision into one drum and the grid output of another into a different drum, all from the same Meter, letting you mix straight and grooved feels in the same patch.

BAR has no swing, so its single output is always on the grid.

### External Clock Sync

Patch an external clock pulse train into the **EXT CLOCK** input and Meter measures the inter-pulse interval to derive BPM. The PPQN (pulses per quarter note) is selectable in the context menu (1, 2, 4, 8, 12, 16, or 24). **MIDI clock is always 24**, which is the default.

**If the PPQN does not match what the source is sending, Meter cannot know the tempo, and it says so.** A steady clock whose implied tempo is impossible (24 PPQN read as 4 implies 720 BPM at a host tempo of 120) is not a tempo Meter can guess its way out of: several PPQN settings give a plausible answer for any given tick interval, so there is nothing to auto-detect. After four such ticks the BPM readout turns orange and reads `PPQN?`, and the context menu says what is wrong. Until it matches, Meter runs on its **BPM knob** while the sync light keeps flashing. That combination, a flashing sync light and a tempo that is not the host's, is what a PPQN mismatch looks like, and it is why the readout now names it.

When EXT CLOCK is patched and a measurement is available, the BPM knob is overridden and the on-screen BPM readout shows the measured value with a flashing sync indicator next to it.

Meter does not merely follow the external *tempo*; it **phase-locks** to it. The tempo estimate averages the tick period (never the tempo — averaging BPM biases the result high when the ticks are uneven), and on every tick Meter nudges its whole grid toward where the tick count says it should be. The correction is a share of a filtered offset rather than of a single tick reading, because a host's MIDI clock usually arrives quantized to the audio block and chasing each tick would hand that jitter to the outputs.

The lock is re-anchored at every downbeat, because MIDI clock on its own never says which tick is a beat and at 24 PPQN the ticks are only 20 ms apart. If a tick has just gone by — under a DAW it usually lands a sample or two before the Run gate rises — **that** tick is taken as the beat and the whole grid is anchored to it, accumulators included. Anchoring only the lock's reference and leaving the accumulators at the downbeat is not enough: the two then disagree, the lock sees no error to correct, and the grid runs permanently behind the host. Waiting for the *next* tick instead is worse still — it declares the wrong tick the beat and drags everything after beat 1 a full tick (20.8 ms) off the host, leaving beat 1 alone in the right place.

### Running under a DAW

With Rack as a plugin, the usual patch is: host **Start** → a flipflop → Meter's **RUN**, host **Stop** → Meter's **RESET**, host **clock** → **EXT CLOCK** with PPQN 24.

Reset arms the downbeat rather than firing it — under a DAW the Stop that resets Meter arrives seconds before the Start that runs it, and beat 1 belongs where playback actually begins. Every output, swung and grid alike, fires together on that sample.

One MIDI Stop usually drives *both* the Reset cable and whatever holds the RUN gate, so Meter sees the reset a sample or two before the gate drops — still running, and about to spend beat 1 on nobody. If the clock stops before even a sixteenth of that beat has played, nothing was played and the downbeat is owed again, so it lands on the next Start instead.

Meter does not rely on the RUN gate to know this. **A master clock that has gone quiet is a stop**, whatever the gate says — which matters because the gate is often held by a flipflop cleared by Continue rather than by Stop, in which case it never drops at all. When the ticks dry up, an unplayed downbeat is re-armed the same way, and an owed downbeat waits for the master to tick again rather than firing into silence.

An owed downbeat fires **on a tick, or within one tick of the clock coming back** — never merely because the clock still looks alive. A host stops emitting clock slightly *before* it sends Stop (measured at about 18 ms in Live), so at the moment its Reset arrives the clock does still look alive, and firing there put a stray one-sample trigger on every output: a drum hit every time you pressed stop. A Stop has neither a tick nor a recent resumption; a Start has both. The wait costs at most one tick and lands the downbeat exactly on the master's grid.

Resuming with RUN *without* a Reset deliberately does not fire a downbeat: it picks up where it left off. If you want every Start to begin at the top regardless of how Stop is wired, turn on **Reset on play** — it now applies to the RUN gate as well as the RUN button, and forwards a pulse from RESET OUT so downstream sequencers restart with Meter.

Residual timing error against the host is dominated by how precisely the host delivers its MIDI clock. With 128-sample blocks Meter holds to about 1.3 ms of the host's grid; a very large block (512 samples, so a 24-PPQN tick is only about two blocks long) leaves a few milliseconds more, and a low PPQN setting makes that worse rather than better.

## Display

Top status line:
- **Left**: current BPM (numeric)
- **Sync indicator**: small dot to the right of BPM when EXT CLOCK is patched, flashing orange on each external pulse, dim when idle
- **Center**: time signature `4/4`, with optional pending change `→ 7/8` shown to the right when a change is queued
- **Right**: `BAR N` counter: increments each bar, resets to 1 on Reset

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
| **RUN** | Light latch (green) | n/a | On | Play/stop toggle |
| **RST** | Momentary button | n/a | n/a | Reset to bar 1, position 0; fires Reset OUT |
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
| **RESET** | 1ms trigger that fires when the Reset button is pressed. Meter is the master, so downstream modules receive reset via this jack |

## Context Menu

- **External Clock PPQN** selector (1, 2, 4, 8, 12, 16, 24; default 24, the MIDI standard)
- **Apply time signature changes immediately** (default off: changes queue for next bar)
- **Reset on play** (default off: Run after Stop resumes from current position)
- **Detected: NN.N BPM** label when EXT clock is connected and measuring

## Patch Ideas

**Master clock + 3-piece kit**: Meter QUARTER (grid) → kick Beat clock; Meter EIGHTH (swung) → hi-hat Beat clock; Meter SIXTEENTH (swung) → snare Beat clock. Meter BAR → all three Beat BAR inputs. Meter RESET → all RESET inputs. Now you can dial swing per drum from the master.

**Cross-rhythm playground**: Patch Meter QUARTER TRIPLET into one Beat and Meter EIGHTH into another. The triplet fires 3-against-2 against the eighth, producing classic polyrhythms.

**Sequenced time signatures**: Patch a sequencer or LFO into NUM CV and DEN CV to morph the time signature mid-piece. Pending changes apply at the next bar boundary by default, so the music stays musical.

**External tempo follow**: Patch a hardware MIDI clock or a Clk module into EXT CLOCK at 24 PPQN. Meter follows the external tempo while still emitting its own subdivisions, swing, and bar boundaries.
