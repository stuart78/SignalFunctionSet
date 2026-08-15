# Play Manual

## Overview

Play is a **polyphonic multisample player**. It loads an `.sfz` or a DecentSampler `.dspreset` instrument and plays it back with up to 16 voices, with velocity layers, round-robins, loops, and per-note tuning.

It's the other half of [Record](record-manual.md): load what Record captured and play it. But it isn't limited to that. It reads a practical subset of SFZ, so simple third-party SFZ libraries and DecentSampler presets work too.

Play is 16HP.

## Loading an instrument

**Right-click → Load instrument** and pick a `.sfz` or `.dspreset` file. Play holds a list of instruments; the **INSTR** knob (and its CV input) selects between them, so you can keep a drum kit, a bass, and a pad loaded at once and switch with a voltage.

The context menu lists what's loaded and lets you remove entries. Instrument paths are saved with the patch and reload automatically.

## Playing it

| Input | What it does |
|---|---|
| **V/OCT** | Pitch. Polyphonic, one voice per channel. |
| **GATE** | Note on/off. |
| **VEL** | Velocity, 0–10V → 1–127. Sets the voice's level and picks the velocity layer. |
| **INSTR CV** | Select instrument. |
| **LEVEL CV** | Output VCA. |

**Outputs:** L / R.

Polyphony comes from the V/OCT and GATE cable channels. Patch a polyphonic sequencer and you get one voice per channel, up to 16. A **mono** cable broadcasts to all voices, so a single velocity cable applies to the whole chord rather than leaving voices 2+ silent.

Voices are pitched by resampling: the ratio is derived from the note's distance from the region's `pitch_keycenter`, plus any `tune`, plus the sample rate difference. There's a 2ms attack and 30ms release envelope to keep note edges clean.

## Note-off behaviour

By default **note-off releases the voice**: the gate controls the note's duration, like any normal instrument. This matters more than it sounds: if one-shots ignored note-off, a melodic sequence would pile up voices until it hit the 16-voice cap and started stealing.

For drums and other play-through material, the context menu has **One-shot (play through)**, which lets samples run to their natural end regardless of gate length.

Looped regions loop for as long as the note is held.

## The display

**Instrument name**, region count, and active voice count.

**Keyboard / grid**, the same two tabbed views as Record:

- **GRID** (default): a 12×8 Push-style isomorphic pad grid.
- **PIANO**: the full 88 keys.

Mapped notes show blue; playing notes show cyan, so you can see the instrument's real range at a glance, and spot gaps in a library that doesn't cover the whole keyboard.

**The pads are playable**: tap one and Play auditions its own voice on a reserved internal channel, without needing anything patched. It's the fastest way to check a freshly loaded instrument.

Grid layouts (context menu **Grid view → Layout**): Chromatic 4ths, In-Key (with Root + Scale menus), and Chromatic grid with shaded accidental columns.

## Supported formats

### SFZ

A practical subset, enough for Record's output and for simple libraries:

`sample`, `lokey` / `hikey` / `key`, `pitch_keycenter`, `lovel` / `hivel`, `tune`, `volume`, `loop_mode` / `loop_start` / `loop_end`, `default_path`, `seq_length` / `seq_position`.

Keys can be note names or numbers. Opcodes cascade global → group → region as they should.

### DecentSampler

`.dspreset` files are parsed too (`groups` → `group` → `sample`, cascading). Following the DecentSampler convention, **tuning and volume accumulate** across levels while everything else overrides. Loop settings and round-robins map onto the same engine.

Not yet honored: sample start/end trim, pan, and loop crossfade.

## Round-robins

If an instrument defines `seq_length` / `seq_position`, Play rotates through the matching samples on successive notes. Record writes these when you capture with round-robins enabled.

## Patch ideas

**Play back what you sampled**: Record a granular patch to disk, load the `.sfz` here, and play it polyphonically from Note or a keyboard for a fraction of the CPU.

**Switchable kit**: load several instruments and sequence the INSTR CV to change instrument per section (Arrange's PHRASE out works well here).

**Velocity-layered drums**: a kit captured with several velocity layers, in One-shot mode, driven from Beat's velocity output.

**Sampled pad with real loops**: capture a slow evolving voice with Record's loop detection on; Play holds the loop for as long as the note sustains.
