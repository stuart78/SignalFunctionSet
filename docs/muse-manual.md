# Muse Manual

## Overview

Muse is a faithful recreation of the **Triadex Muse**, the legendary algorithmic sequencer designed by Edward Fredkin and Marvin Minsky in 1972 (US Patent 3,610,801). It is not a step sequencer: it has no notes to program. Instead, eight sliders tap into a network of binary counters and a 31-bit shift register, and the *interaction* of those digital signals generates long, surprisingly musical melodies that can repeat over hundreds of steps. Small slider changes produce dramatically different tunes, but the results are always structured and deterministic.

This version is clock-driven (no internal tempo), adds a scale selector and root control compatible with the rest of Signal Function Set, a randomize button, and can be chained: place a second Muse to the right and it will follow the first.

Muse is 16HP.

## How It Works

Every clock pulse advances the engine. There are **40 "taps"** each slider can point to:

| Tap | Meaning |
|-----|---------|
| **OFF** | constant 0 |
| **ON** | constant 1 |
| **C ½** | the raw clock (toggles every pulse) |
| **C1, C2, C4, C8** | binary counter bits with periods of 2, 4, 8, 16 clocks |
| **C3, C6** | mod-12 counter taps with periods of 6 and 12 clocks (triplet/ternary feel) |
| **B1 … B31** | the 31 bits of a feedback shift register (B1 = newest) |

There are two banks of four sliders:

- **THEME (4 sliders)**: the four selected tap bits are combined with **XNOR** to produce the next bit fed into the shift register (B1). This is the "DNA" of the pattern: it determines how the shift register evolves, and therefore the long-term shape and length of the melody. (XNOR, not XOR, so an all-OFF theme is a stable fixed point rather than silence.)
- **INTERVAL (4 sliders)**: the four selected tap bits form a 4-bit number each clock. The low three bits (A, B, C) index a note in the current scale; the top bit (D) adds an octave. That note, plus the Root, becomes the pitch output as 1V/oct.

Because the shift register and counters cycle at different rates, the combined pattern can be very long before it repeats, exactly what made the original Muse so compelling.

## Controls

| Control | Function |
|---------|----------|
| **THEME 1–4** (sliders) | Select the taps XNOR'd to drive the shift register (the pattern engine) |
| **INTERVAL 1–4** (sliders) | Select the taps that form the 4-bit pitch address each clock |
| **ROOT** | Transposes the output, ±24 semitones |
| **SCALE** | Selects the scale the interval bits read (shared canonical SFS scale list) |
| **RUN** | Start / stop the sequence (LED shows running state) |
| **RESET** | Clears the counters and shift register and restarts the pattern |
| **RANDOMIZE** | Re-rolls the slider positions (scope set in the context menu) |

The live state of each tap is shown as an LED column beside the sliders, with the original Triadex-style labels, so you can watch the counters and shift register evolve.

## Inputs

| Input | Function |
|-------|----------|
| **CLOCK** | Advances the sequence one step per rising edge (Muse has no internal clock) |
| **RESET** | Trigger to reset counters + shift register |
| **RUN** | Gate that overrides the Run button (high = run) |
| **RANDOMIZE** | Trigger to re-roll the sliders |
| **THEME 1–4 CV** | ±5V offsets the corresponding Theme slider's tap selection |
| **INTERVAL 1–4 CV** | ±5V offsets the corresponding Interval slider's tap selection |
| **ROOT CV** | 1V/oct added to the root |
| **SCALE CV** | Selects the scale (interchangeable with Note and Fugue) |

## Outputs

| Output | Function |
|--------|----------|
| **V/OCT** | The melody pitch, 1V/oct (with Scale + Root applied) |
| **GATE** | A trigger on each clocked step |

## Context Menu

- **Presets (from the Triadex manual)**: 17 classic slider snapshots straight from the original 1972 manual ("240-Note Pattern", "Birds", "Christmas Bells", "Marvin's Yodel", and more), a great starting point and a tour of what the machine can do.
- **Output range**: *V/oct* (default, standard 1V/oct with Root) or scale-quantized *1V / 2V / 5V* modes that rescale Muse's natural range to that voltage span and ignore Root (useful when using Muse as a modulation source rather than a melody).
- **Link → Allow expander linking**: when a second Muse sits directly to the right, it follows this one's state. Chain several for layered/harmonized voices off one engine.
- **Gate**: *Every clock pulse* (default) or *Only when pitch changes* (tie repeated notes together).
- **Randomize**: scope the Randomize button/CV to *All 8 sliders*, *Theme only*, or *Interval only*, plus a *Randomize now* action.

## Tips & Patch Ideas

**Start from a preset**: Load a preset from the context menu, then nudge a single Interval slider. You'll hear how one tap change re-voices the whole melody while keeping its rhythmic skeleton.

**Theme = structure, Interval = melody**: If you like a pattern's *rhythm and length* but not its *notes*, leave the Theme sliders alone and only move the Interval sliders (and vice-versa).

**Tame it with a scale**: The original always plays a fixed diatonic set. Pick a SCALE (e.g. minor pentatonic) and set ROOT to taste so anything the Muse produces stays in key. Sequence the SCALE CV from another module to modulate the tonality over time.

**Two-voice canon**: Place a second Muse to the right with linking enabled. Give it a different SCALE or ROOT, or use "Only when pitch changes" gating, for an instant harmonized counter-melody locked to the master.

**Muse as modulation**: Switch Output range to 1V/2V/5V and patch V/OCT into a filter cutoff or wavetable index for stepped, ever-evolving but structured modulation.

**Drum trigger source**: Clock Muse fast and take the GATE (set to "Only when pitch changes"). The irregular-but-repeating note changes make a characterful trigger pattern.
