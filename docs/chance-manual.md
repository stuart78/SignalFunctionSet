# Chance Manual

## Overview

Chance is a generative melodic sequencer built on a simple idea: a melody is a **walk**. You don't program notes. You set a key, a window, and a few probabilities, and Chance walks a line through the scale for you. Every decision it makes is seeded, so a pattern is a *fixed, repeatable melody* rather than a stream of noise: it plays the same way every time it comes around, and every knob you turn changes it audibly.

The name is the concept. Each cycle the walk reaches a node and takes a branch (a garden of forking paths), but the paths are laid out ahead of time, not rolled fresh each bar.

Chance is 26HP and needs an external clock.

## The core idea: core + branch

Chance keeps two lines:

- **The core**: a deterministic skeleton melody built from the pattern's seed and shaped by **GRAV** and **DRIFT**. It is recomputed the same way every cycle, so it never wanders off.
- **The stray**: a deviation from the core. At each step, **BRANCH** decides whether to take the core note or stray to a neighbouring one.

Strays are **non-cascading**: taking one doesn't shift the notes after it. That's what lets the core show through no matter how high you push BRANCH: you always hear the same tune, wearing a different coat.

- **BRANCH = 0** → pure repeating core.
- **BRANCH up** → more wandering, but still recognisably the same melody.

## Note choice is musical, not random

The walk isn't a coin flip. Chance uses hand-authored **first-order Markov tables**, so a note's next note is weighted by what actually sounds like music in that scale:

- **7-note scales** share a diatonic table: chord-tone gravity (1/3/5), the leading tone pulls to the tonic, the subdominant leans to the 3rd/5th, the dominant resolves home.
- **Pentatonic** and **blues** have their own tables.
- Everything else (whole-tone, chromatic, harmonic series) falls back to a stepwise-plus-tonic formula.

Strays voice-lead from the *previously played* note, so even a deviation moves sensibly.

## Controls

### The walk

| Control | What it does |
|---|---|
| **GRAV** | Direction bias. Centre = even up/down; up/down weights the walk that way. |
| **DRIFT** | Move size. 0 = stepwise (2nds only); up = bigger leaps, out to an octave. |
| **BRANCH** | Chance of straying from the core at each step. 0 = the core, unchanged. |

A move always *moves*: there's no "stay" hiding in DRIFT. A note only repeats via the RATCHET control or a tie.

### Shaping the line

| Control | What it does |
|---|---|
| **REST** | Chance a step is silent (the walk continues underneath). |
| **HOLD** | Chance a step spans 2–4 clocks instead of one. |
| **LEAP** | Chance of a ±octave jump. |
| **RATCHET** | Chance a step retriggers as **2 or 3 short bursts inside the step**. The note keeps its own pitch, it just fires a rapid burst. It never extends the pattern. |
| **GATE LEN** | Gate length as a fraction of the step. |
| **GLIDE** | Portamento between notes. |

Every one of these has a CV input directly beneath it.

### Key and window

| Control | What it does |
|---|---|
| **KEY** | Scale, from the shared 19-scale list (see `docs/conventions/scales.md`). |
| **ROOT** | Root note. |
| **START / END** | The active step window. Set START **after** END to play the window in reverse. |
| **HARMONY** | Second voice: Off / 3rd / 5th / Octave (up or down), or **Varied**. |

KEY and ROOT have CV inputs (1V/scale and 1V/oct, semitone-quantized) that are interchangeable with Note, Fugue, Muse, and Arrange. Patch one source to all of them and they stay in agreement.

### The second voice

**HARMONY** adds a counter-line. The fixed modes track the main voice at a diatonic interval. **Varied** picks a seeded consonant interval per step from a set of 3rds, 5ths, 6ths and octaves, some in contrary motion, giving a weaving counter-melody that's stable per pattern rather than a rigid parallel. It has its own **H V/OCT** and **H GATE** outputs.

## The screen

Everything you edit lives on the screen.

**Key readout**: root + scale, top-left.

**Pattern bank (P1–P8)**: eight slots, each showing its melody as a micro-waveform.

- **Click** a pattern to focus it. While stopped, the focused pattern loads into the big walk below, so you can see what you're editing.
- **Double-click** to toggle it active. Inactive patterns are skipped in the rotation.
- **Repeat boxes** under each slot: click box *N* to play that pattern *N* times before advancing.
- **Recycle icon**, the slot's mode:
  - *Outline* = **normal**: a fixed seeded melody, identical every visit.
  - *Orange* = **reseed**: a fresh variation each time the rotation lands on it (its repeats still replay identically within the visit).
- **Right-click** a slot for Enable/Disable, Copy, Paste.

**Step gates (S1–S8)**, the focused pattern's per-step gate:

- **Click** = enable / disable. A disabled step is a **forced rest** regardless of the REST knob. This is the usual reason for an unexpected gap.
- **Shift-click** = **tie**: hold the previous note through this step, no retrigger (legato). Shown as a connector bar to the previous step.
- Gate edits take effect **immediately**, mid-cycle, not at the next loop.

**The walk**: the melody itself, with C-octave axis labels. The dark line is the core skeleton, the bright line is what's actually played, and the teal line is the harmony voice (when enabled). The orange dot is the playhead; a hollow circle is a rest.

## Patterns and rotation

Chance rotates through the **active** patterns at the *cycle* level, not the step level: a pattern plays its repeat count, then hands off to the next active slot. **RND** reseeds the playing slot (a new core); **RST** returns to the first active pattern.

This is the macro structure: eight melodies, each with its own gates, repeat count, and mode, cycling as a song section.

## Inputs and outputs

**Inputs:** CLK (Chance advances only on an external clock), RND gate, RST gate, ROOT cv, SCALE cv, and a CV jack under each of GRAV / DRIFT / BRANCH / REST / HOLD / LEAP / RATCHET.

**Outputs:** V/OCT, GATE, and the second voice's H V/OCT and H GATE.

## Context menu

- **Start on root** (default on): ground step 1 of each cycle on the tonic.
- **Walk range**: ±1 or ±2 octaves.
- **New core for current** / **New cores for all**: reseed.

## Patch ideas

**A melody that evolves but stays itself**: BRANCH ~0.3, one pattern, repeats 4. The tune returns each cycle with different ornaments.

**A song section**: activate P1–P4 with repeats 2/2/4/8, set P4 to reseed. Three fixed phrases and a variable tail, rotating forever.

**Two-part invention**: HARMONY = Varied, patch H V/OCT + H GATE to a second voice. The counter-line weaves instead of tracking in parallel.

**Drum-like ratcheting**: RATCHET up, GATE LEN short, into a percussive voice.

**Keyed to the song**: feed Arrange's ROOT and SCALE outs into Chance's ROOT cv / SCALE cv. The melody follows the arrangement's key changes.
