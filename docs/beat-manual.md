# Beat Manual

## Overview

Beat is a single-voice pattern sequencer designed to be paired with Meter (or any source of clock + bar pulses). One Beat instance = one drum / voice. Eight patterns × sixteen steps each, with per-step velocity, accent, and probability.

Most editing happens on the screen — the panel is a narrow 10HP with just the display and six jacks.

Beat is 10HP.

## Concepts

### One Voice Per Module

Beat is intentionally **monophonic per instance** — each module drives exactly one drum or voice via Gate, Velocity, and Accent outputs. To build a kit, instantiate multiple Beats and clock them from a shared Meter. Each instance has its own pattern bank, length, swing-source choice, etc., so each drum can have its own feel and composition independently.

### Patterns and Steps

Each Beat instance holds **8 patterns × 16 steps**. Each step has four properties:

- **On/off** (whether the gate fires)
- **Velocity** (0..1, drives 0..10V VEL output)
- **Accent** (boolean, drives a 1ms pulse on the ACC output)
- **Probability** (0..1, chance the step actually fires when reached)

### Pattern Length

Each pattern has its own **length** (1–16 steps). When the playhead reaches the last step, on the next CLOCK pulse it wraps back to step 0 — even mid-bar. This lets you build cross-rhythm patterns shorter than the bar: a 5-step pattern clocked at 16th notes will keep cycling through the bar producing polyrhythm against the underlying meter.

### Pattern Repeats

Each pattern has its own **repeat count** (1–8 bars). When BAR fires, Beat increments its bar counter. Once the count exceeds the pattern's repeats, Beat advances to the next active pattern.

### Active Patterns

Each pattern has an **active** flag. Inactive patterns are skipped in the rotation — useful for chaining a subset of patterns without manually editing each one. By default all 8 patterns are active.

Toggle active by **double-clicking** a pattern cell.

### Bar / Clock Coincidence Handling

Meter fires BAR and downbeat-EIGHTH/QUARTER/SIXTEENTH on the same sample (or within 1ms of each other). Beat collapses these into a single event: if BAR fired this sample OR if BAR voltage is currently high (still within its 1ms pulse window), CLOCK is suppressed. This handles either-direction sub-sample drift between Meter outputs and prevents the bar boundary from firing two steps back-to-back.

### "Advance only on bar trigger"

Default ON (context menu toggle). When ON, pattern advance happens **only** on a real BAR pulse. With BAR not patched, the pattern just loops the same one indefinitely — you patch BAR when you want pattern progression. When OFF, a legacy fallback advances on pattern wrap when BAR isn't patched.

## Display

The display is the primary editing surface. Top to bottom:

1. **Mode tabs**: `STEPS · VEL · ACC · PROB` — click to switch edit modes
2. **Top connector rail** — visual link from active mode tab down to the step grid
3. **Step grid**: 2 rows × 8 cols (16 steps). Cell colors:
   - Out of length: very dim
   - Active step: blue
   - Active step + currently playing: orange
   - Inactive step at beat boundary (every 4 steps): mid purple
   - Inactive elsewhere: dim purple
4. **Length dots**: 16 small bars below the grid showing pattern length
5. **PATTERN label** (left)
6. **Pattern selector**: 8 cells (1–8). Each shows the pattern number + a row of small dots indicating that pattern's repeat count. The dot at `currentBar - 1` lights up brightly on the playing pattern
7. **Bottom connector rail** — visual link from edit-pattern cell to the repeats bar
8. **Repeats bar**: 8 cells. Orange = current bar of the loop, blue = in-range, dim = out-of-range

## Controls (display interactions)

### Mode Tab

Click any of the four mode tabs to switch edit mode. The selected tab highlights blue.

### Step Grid

**STEPS mode**:
- Click a cell to toggle on/off
- Drag across cells to paint the same new state to subsequent cells
- Click beyond current pattern length auto-extends the length to include that step

**VEL mode**:
- Click a cell to set velocity by Y position within the cell (top = 1.0, bottom = 0.0)
- Vertical drag adjusts further
- Auto-enables the step
- Cell renders a bottom-up white overlay sized to velocity (60% white in VEL mode, 10% white as a hint in STEPS / ACC modes)

**ACC mode**:
- Click toggles the accent flag (auto-enables the step)
- Drag paints
- Accent shows as an unfilled white circle at cell center — full opacity in ACC mode, 10% opacity hint elsewhere

**PROB mode**:
- Same vertical-drag behavior as VEL but writes to step probabilities
- 60% white bottom-up overlay shown only in PROB mode (no faint hints elsewhere)
- DSP fires the step probabilistically: `random::uniform() >= probability` short-circuits the gate

### Length Dots

- **Click** any length dot → set length to that index + 1
- **Drag** across length dots → scrub length 1..16

### Pattern Selector

- **Click** → select for editing (and drag across to scrub the edit pattern)
- **Double-click** → toggle pattern active/inactive
- **Scroll wheel** → adjust that pattern's repeat count

### Repeats Bar

- **Click** any cell → set the edit pattern's repeats to that index + 1
- **Drag** across cells → scrub repeats 1..8

## Inputs

| Input | Function |
|-------|----------|
| **CLOCK** | Advances the step counter |
| **BAR** | Advances to the next active pattern (with `repeats` honored) |
| **RESET** | Returns to first active pattern, step 0 |

(MUTE input was removed in 2.8.0 — silence a Beat by switching off all its steps or muting downstream.)

## Outputs

| Output | Function |
|--------|----------|
| **GATE** | 1ms 10V pulse on each fired step |
| **VEL** | Sample-and-hold CV 0..10V — the previous step's velocity stays held until the next fire |
| **ACC** | 1ms 10V pulse on accented hits |

## Context Menu

- **Advance only on bar trigger** (default ON) — see *Concepts* above
- **Patterns**:
  - **Randomize current pattern steps** — fires each of 16 step on/off slots at 50% density (doesn't touch velocity/accent/probability)
  - **Clear current pattern**
  - **Clear all patterns**

## Persistence

JSON saves: editPattern, editMode, playPattern, playStep, currentBar, advanceOnBarOnly, and per-pattern: active, length, repeats, steps[16], velocities[16], accents[16], probabilities[16].

## Default State

On Initialize: all 8 patterns active (so a fresh Beat will visibly cycle through patterns even if most are empty), `advanceOnBarOnly = true`, `editMode = STEPS`, `editPattern = 0`.

## Patch Ideas

**3-piece drum kit driven by Meter**: 3 × Beat (kick, snare, hat). Meter QUARTER → kick CLOCK, Meter EIGHTH → snare CLOCK, Meter SIXTEENTH (swung) → hat CLOCK. All three BAR inputs fed from Meter BAR. All three RESET inputs from Meter RESET. Each Beat holds its own pattern bank for its drum.

**Polyrhythm**: Set kick pattern length = 4 with CLOCK at 16ths → 4-step pattern fires every quarter. Set hat pattern length = 3 → 3-against-4 polyrhythm.

**Pattern variations across bars**: Set pattern 1 reps = 3, pattern 2 reps = 1. Beat plays pattern 1 three times then pattern 2 once, then loops. Build multi-bar phrases with simple repeat counts.

**Probability-based variation**: In PROB mode, pull the probability of "ghost note" steps down to ~0.3. They fire occasionally for human-feeling variation. Combine with VEL to make ghost notes quieter.
