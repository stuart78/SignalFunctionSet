# Fugue Manual

## Overview

Fugue is an 8-step harmonic deviation sequencer for VCV Rack. It generates evolving counterpoint from a single melodic sequence. Eight pitch faders define a melody. Three independent voices (A, B, and C) each read that same sequence, but each can wander harmonically according to its own controls. The result is three intertwined melodies that share a common origin but gradually diverge.

Fugue is 20HP.

## Concepts

### The Sequence

Eight vertical pitch faders define a base melody. All three voices read from this same sequence, quantized to the selected root note and scale. Each voice has its own clock input and gate pattern, so they can traverse the sequence at different rates and with different rhythms.

### Wander

Each voice has a horizontal Wander slider that determines how far it strays from the base sequence. At 0%, the voice plays the sequence faithfully. As Wander increases, the voice begins choosing harmonically related notes instead of the written pitch: first chord tones, then extensions, then increasingly colorful substitutions.

Wander is not random pitch chaos. It uses a tiered system that respects music theory. Notes closer to the harmonic center of the scale are preferred at low settings, and more distant intervals only appear as Wander increases.

### Harmonic Lock

When enabled (the default), each voice considers what the other two are currently playing before choosing its deviation. It generates three candidate notes and picks the one most consonant with the other voices. This creates a soft harmonic gravity where the voices negotiate with each other without being rigidly locked to the same chord.

When disabled, each voice deviates independently. This produces more dissonance and unpredictability, which can be useful for atonal or experimental textures.

Harmonic Lock can be toggled in the right-click context menu.

## Controls

### Global Controls

These are located in the top-left section of the panel.

| Control | Range | Default | Description |
|---------|-------|---------|-------------|
| Root | C through B | C | Root note for scale quantization. CV: 1V per semitone, wraps around. |
| Scale | 12 scales | Major | Scale selection (see Scale List below). CV: 1V per scale index. |
| Steps | 1 to 8 | 8 | Number of active steps in the sequence. Steps beyond this count are skipped. CV: 1V per step. |
| Slew | 0 to 100% | 0% | Portamento between notes. Uses adaptive timing: the slew duration is proportional to the time until the next active gate, so it always resolves before the next note arrives. CV: ±5V maps to ±100%. |
| Reset | Jack + Button | | Returns all three voices to step 1. Accepts a trigger input or a momentary push of the panel button. |

#### Scale List

Major, Natural Minor, Harmonic Minor, Melodic Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Pentatonic Major, Pentatonic Minor, Chromatic.

### Sequencer

The sequencer area occupies the top-right section of the panel.

**Step Indicator LEDs**: Three rows of red LEDs above the faders, one row per voice (A, B, C). Each LED illuminates when its voice is on that step and the step's gate toggle is active.

**Pitch Faders**: Eight vertical faders set the base pitch for each step. Hover over a fader to see the quantized note name. The fader range (1V, 2V, or 5V) is set in the right-click context menu.

**Gate Toggles**: Three rows of red toggle buttons below the faders, one row per voice (A, B, C). When a gate is off, that voice is silent on that step but continues advancing through the sequence. By default, Voice A has all gates on, Voices B and C have all gates off.

### Voice Rows

Three identical rows at the bottom of the panel, one per voice (A, B, C). Each row contains, from left to right:

| Control | Description |
|---------|-------------|
| Clock | Trigger input. Clock B is normalled to Clock A. Clock C is normalled to Clock B. Patch separate clocks for polyrhythmic effects. |
| Wander CV | ±5V bipolar input. Adds to the Wander slider position. |
| Wander | Horizontal slider, 0 to 100%. Controls harmonic deviation for this voice. |
| Gate Out | +10V gate output, high when the clock is high and the step's gate toggle is on. |
| CV Out | 1V/octave pitch output, quantized to the selected scale. |

## Wander Reference

The Wander control governs a probability system that selects notes from increasingly distant harmonic tiers. The slider position (0 to 100%) determines how much probability flows away from unison into higher tiers.

### Diatonic Scales (Major, Minor, Dorian, etc.)

| Tier | Notes | Character |
|------|-------|-----------|
| 0 | Unison | Faithful to the written sequence |
| 1 | 3rd, 5th | Chord tones, consonant and supportive |
| 2 | 7th, 9th, 11th | Extensions, jazzy and colorful |
| 3 | 6th | Remaining diatonic tone |
| 4 | Chromatic neighbor | Half-step outside the scale, tension |

#### Probability Table (Diatonic)

| Wander | Unison | 3rd/5th | 7th/9th/11th | 6th | Chromatic |
|--------|--------|---------|--------------|-----|-----------|
| 0% | 100% | 0% | 0% | 0% | 0% |
| 10% | 90.5% | 3% | 3% | 2% | 1.5% |
| 25% | 76.25% | 7.5% | 7.5% | 5% | 3.75% |
| 50% | 52.5% | 15% | 15% | 10% | 7.5% |
| 75% | 28.75% | 22.5% | 22.5% | 15% | 11.25% |
| 100% | 5% | 30% | 30% | 20% | 15% |

### Pentatonic Scales

| Tier | Notes | Character |
|------|-------|-----------|
| 0 | Unison | Faithful |
| 1 | 2nd, 3rd degree | Neighboring pentatonic tones |
| 2 | 4th, 5th degree | Distant pentatonic tones |

### Chromatic Scale

Uses interval consonance rather than scale degrees:

| Tier | Intervals | Character |
|------|-----------|-----------|
| 0 | Unison | Identical |
| 1 | Perfect 5th, Perfect 4th | Open, stable |
| 2 | Major 3rd, minor 3rd, Major 6th, minor 6th | Warm, tonal |
| 3 | Major 2nd, minor 7th | Mild tension |
| 4 | minor 2nd, Major 7th, tritone | Sharp dissonance |

### Quick Reference

- **0%**: Faithful (unison only)
- **~25%**: Chord tones begin appearing, still mostly faithful
- **~50%**: Even odds of staying on the written note or moving
- **~75%**: Extensions and color tones dominate
- **100%**: Full wander, only 5% chance of unison

## Context Menu

Right-click the module to access these options:

| Option | Description |
|--------|-------------|
| Fader Range | Choose 1V (1 octave), 2V (2 octaves), or 5V (5 octaves). Controls the pitch range of the step faders. |
| Harmonic Lock | When checked, voices bias toward consonance with each other. Default: on. |
| Randomize Sequence | Sets all 8 faders to random positions. |

## Inputs and Outputs

### Inputs (11 total)

- Clock A, Clock B, Clock C: trigger inputs (B normalled to A, C normalled to B)
- Reset: trigger input
- Root CV, Scale CV, Steps CV, Slew CV: parameter modulation
- Wander A CV, Wander B CV, Wander C CV: per-voice wander modulation (±5V)

### Outputs (6 total)

- CV A, CV B, CV C: 1V/octave pitch
- Gate A, Gate B, Gate C: +10V gate

## Patch Ideas

**Slowly Diverging Canon**: Send the same clock to all three voices. Set Wander A to 0%, Wander B to 25%, Wander C to 50%. Voice A plays the sequence faithfully while B and C wander further, creating a canon that gradually breaks apart.

**Polyrhythmic Chords**: Send different clock divisions to each voice (for example, 1/4, 1/6, 1/8). With low Wander and Harmonic Lock on, the voices form chords that shift at different rates.

**Generative Texture**: Set all three Wander controls to 75-100% with Harmonic Lock off. Feed slow random CV into the Wander inputs. The result is an evolving atonal texture where moments of consonance emerge and dissolve.

**Call and Response**: Use gate toggles to alternate which voices are active on each step. Voice A plays steps 1-4, Voice B plays steps 5-8. With moderate Wander, each voice develops its own variation on its half of the sequence.

**Modulated Harmony**: Patch an LFO into Wander A CV. As the LFO sweeps, Voice A alternates between faithful and deviated, creating a breathing harmonic texture against the steady B and C voices.

---

# Fugue X (Expander) Manual

## Overview

Fugue X is an expander module for Fugue that adds per-voice controls for steps, range, sleep, and probability, plus sorted CV outputs and per-step trigger outputs. Place it to the right of Fugue to connect automatically via the expander bus.

Fugue X is 20HP.

## Connection

Fugue X must be placed immediately to the right of a Fugue module. It communicates via VCV Rack's expander system — no cables needed between them. If Fugue X is not adjacent to a Fugue, its controls have no effect.

## Per-Voice Controls

Each of these parameters is available for all three voices (A, B, C) with a dedicated CV input.

### Steps (per voice)

| Range | Default | Function |
|-------|---------|----------|
| 1-8 | 8 | Independent step count per voice |

Overrides Fugue's global Steps parameter for this voice. Voice A can play 3 steps while Voice B plays 7, creating polymetric patterns from the same sequence.

### Range (per voice)

| Options | Default | Function |
|---------|---------|----------|
| 1V / 2V / 5V | 1V | Fader range per voice |

Overrides Fugue's global fader range. Each voice can interpret the same fader positions across a different pitch range.

### Sleep (per voice)

| Values | Default | Function |
|--------|---------|----------|
| 0, 1, 2, 4, 5, 8, 16, 32, 48, 64 | 0 | Clock ticks to skip between active steps |

Introduces rests into the voice's playback. A sleep of 4 means the voice advances one step, then waits 4 clock ticks before advancing again. Creates rhythmic variation and longer phrase lengths from short sequences.

An amber LED indicates when each voice is sleeping.

### Probability (per voice)

| Range | Default | Function |
|-------|---------|----------|
| 0-100% | 100% | Chance each step fires its gate |

At 100%, every step plays. At 50%, roughly half the steps produce gates (randomly determined each step). At 0%, no gates fire. Introduces controlled randomness to the rhythmic pattern.

## Sample & Hold Mode

Toggle switch (with LED indicator). When enabled, held notes sustain through rests instead of returning to silence. If a step is skipped by Sleep or Probability, the previous pitch continues instead of going silent. Creates legato phrases with gaps.

## Randomize Sequence

Button + trigger input. Randomizes the pitch faders on the parent Fugue module. Useful for generative patches — patch a slow clock divider to the trigger input for periodic sequence randomization.

## LED Matrix

An 8-column x 3-row grid of LEDs shows the current step position for each voice. Provides visual feedback of where each voice is in the sequence, including sleep states.

## Sorted CV Outputs

Three outputs that present the three voice CV values sorted by pitch:

| Output | Function |
|--------|----------|
| **Max** | Highest of the three voice CV values |
| **Mid** | Middle value |
| **Min** | Lowest of the three voice CV values |

These outputs always track the current chord in sorted order regardless of which voice is playing which note. Useful for consistent bass/melody separation or for processing the harmonic spread.

## Per-Step Trigger Outputs

24 individual trigger outputs (8 steps x 3 voices). Each output fires a short trigger pulse when its specific voice reaches its specific step. Useful for driving external percussion, envelopes, or effects from specific sequence positions.

## Inputs

All per-voice parameters have CV inputs (±5V):
- **Steps A/B/C CV** — step count modulation
- **Range A/B/C CV** — range selection
- **Sleep A/B/C CV** — sleep amount
- **Prob A/B/C CV** — probability modulation
- **Randomize Trigger** — trigger input for sequence randomization

## Patch Ideas

**Polymetric Canon**: Set Steps A=8, Steps B=5, Steps C=3. Same clock to all. Each voice cycles through a different portion of the sequence at a different rate.

**Rhythmic Thinning**: Set Probability A=100%, B=70%, C=40%. Voice A plays every note, B drops some, C plays sparsely. Combined with Harmonic Lock on Fugue, this creates rhythmically differentiated but harmonically coherent counterpoint.

**Ratcheted Sequence**: Use Sleep to create irregular rhythmic patterns. Sleep A=0, Sleep B=1, Sleep C=3. Voice A plays every clock, B alternates play/rest, C plays one note then rests for three clocks.

**Generative Evolution**: Patch a slow random source to Randomize Trigger. The sequence periodically reshuffles while the voices continue their independent rhythmic and harmonic wandering.
