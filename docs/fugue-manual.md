# Fugue — 8-Step Harmonic Deviation Sequencer

Fugue is a sequencer that generates evolving chord voicings from a single melodic sequence. Three independent voices (A, B, C) interpret the same 8-step pitch sequence, each wandering harmonically according to its own controls. The result is shifting counterpoint — three melodies that share a common origin but diverge according to musical rules.

## Concepts

### The Sequence
Eight pitch faders define a base melody. All three voices read from this same sequence, quantized to the selected root note and scale. Each voice has its own clock input and gate pattern, so they can traverse the sequence at different rates and with different rhythms.

### Wander
The central concept in Fugue. Each voice has a horizontal Wander slider that determines how far it strays from the base sequence. At 0%, the voice plays the sequence faithfully. As Wander increases, the voice begins choosing harmonically related notes instead of the written pitch — first chord tones, then extensions, then increasingly colorful substitutions.

Wander is not random pitch chaos. It uses a tiered system that respects music theory: notes closer to the harmonic center of the scale are preferred at low settings, and more distant intervals only appear as Wander increases.

### Harmonic Lock
When enabled (the default), each voice considers what the other two are currently playing before choosing its deviation. It generates three candidate notes and picks the one most consonant with the other voices. This creates a soft harmonic gravity — the voices negotiate with each other without being rigidly locked to the same chord.

When disabled, each voice deviates independently. This produces more dissonance and unpredictability, which can be useful for atonal or experimental textures.

## Panel Layout

Fugue is a 20HP module with a light panel. The layout is organized into three main sections:

**Top-left: Global Controls** — Root, Scale, Steps, Slew knobs with CV inputs, plus a Reset jack and button.

**Top-right: Sequencer** — Three rows of step indicator LEDs, eight vertical pitch faders, and three rows of gate toggle buttons.

**Bottom: Voice Rows** — Three identical rows (A, B, C), each containing Clock input, Wander CV input, horizontal Wander slider, Gate output, and CV output.

## Controls

### Global Controls (top-left)

| Control | Range | Function |
|---------|-------|----------|
| **Root** | C through B | Root note for scale quantization. CV: 1V = 1 semitone, wraps around. |
| **Scale** | 12 scales | Major, Natural Minor, Harmonic Minor, Melodic Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Pentatonic Major, Pentatonic Minor, Chromatic. CV: 1V = 1 scale index. |
| **Steps** | 1–8 | Active sequence length. Steps beyond this count are skipped. CV: 1V = 1 step. |
| **Slew** | 0–100% | Portamento between notes. Uses adaptive timing — the slew duration is proportional to the time until the next active gate, so it always resolves before the next note. CV: ±5V = ±100%. |
| **Reset** | Jack + button | Returns all voices to step 1. Trigger input or momentary push button. |

### Sequencer Area (top-right)

**Step Indicator LEDs** — Three rows of red LEDs above the faders (one row per voice: A, B, C). Each LED illuminates when its voice is on that step and the gate is active.

**Pitch Faders** — Eight vertical faders set the base pitch for each step. Hover text shows the quantized note name. The fader range (1V, 2V, or 5V) is set in the context menu.

**Gate Toggles** — Three rows of red toggle buttons below the faders (one row per voice: A, B, C). When a gate is off, that voice is silent on that step but continues advancing through the sequence. By default, Voice A has all gates on, while Voices B and C have all gates off.

### Voice Rows (bottom)

Each voice (A, B, C) has an identical row, arranged left to right:

| Control | Function |
|---------|----------|
| **Clock** | Trigger input. Clock B is normalled to A, Clock C is normalled to B. Use separate clocks for polyrhythmic effects. |
| **Wander CV** | ±5V bipolar. Adds to the Wander slider position. |
| **Wander** | Horizontal slider, 0–100%. Controls harmonic deviation for this voice. See detailed breakdown below. |
| **Gate Out** | +10V gate, high when the clock is high and the step's gate toggle is on. |
| **CV Out** | 1V/octave pitch output, quantized to the selected scale. |

## Wander in Detail

The Wander control governs a probability system that selects notes from increasingly distant harmonic tiers. The slider position (0–100%) maps directly to how much probability mass flows away from unison into higher tiers.

### Diatonic Scales (Major, Minor, Dorian, etc.)

Five tiers of harmonic distance:

| Tier | Notes | Character |
|------|-------|-----------|
| 0 | Unison | Faithful to the written sequence |
| 1 | 3rd, 5th | Chord tones — consonant, supportive |
| 2 | 7th, 9th, 11th | Extensions — jazzy, colorful |
| 3 | 6th | Remaining diatonic tone |
| 4 | Chromatic neighbor | Half-step outside the scale — tension |

Probability distribution at each Wander position:

| Wander | Unison | 3rd/5th | 7th/9th/11th | 6th | Chromatic |
|--------|--------|---------|--------------|-----|-----------|
| 0% | 100% | — | — | — | — |
| 10% | 90.5% | 3% | 3% | 2% | 1.5% |
| 25% | 76.25% | 7.5% | 7.5% | 5% | 3.75% |
| 50% | 52.5% | 15% | 15% | 10% | 7.5% |
| 75% | 28.75% | 22.5% | 22.5% | 15% | 11.25% |
| 100% | 5% | 30% | 30% | 20% | 15% |

### Pentatonic Scales

Three tiers (fewer notes means fewer tiers):

| Tier | Notes | Character |
|------|-------|-----------|
| 0 | Unison | Faithful |
| 1 | 2nd, 3rd degree | Neighboring pentatonic tones |
| 2 | 4th, 5th degree | Distant pentatonic tones |

### Chromatic Scale

Five tiers based on interval consonance rather than scale degrees:

| Tier | Intervals | Character |
|------|-----------|-----------|
| 0 | Unison | Identical |
| 1 | Perfect 5th, Perfect 4th | Open, stable |
| 2 | Major 3rd, minor 3rd, Major 6th, minor 6th | Warm, tonal |
| 3 | Major 2nd, minor 7th | Mild tension |
| 4 | minor 2nd, Major 7th, tritone | Sharp dissonance |

### Wander Ranges

- **0%** — Faithful (unison only)
- **~25%** — Chord tones begin appearing; still mostly faithful
- **~50%** — The tipping point; even odds of staying or moving
- **~75%** — Extensions and color dominate; rich harmonic movement
- **100%** — Full wander; only 5% unison remains

## Context Menu Options

| Option | Description |
|--------|-------------|
| **Fader Range** | 1V (1 octave), 2V (2 octaves), or 5V (5 octaves). Controls the pitch range of the step faders. |
| **Harmonic Lock** | When checked, voices bias toward consonance with each other. Default: on. |
| **Randomize Sequence** | Sets all 8 faders to random positions. |

## Inputs and Outputs Summary

### Inputs (11 total)
- **Clock A, B, C** — trigger inputs (B normalled to A, C normalled to B)
- **Reset** — trigger input
- **Root CV, Scale CV, Steps CV, Slew CV** — parameter modulation
- **Wander A CV, Wander B CV, Wander C CV** — per-voice wander modulation

### Outputs (6 total)
- **CV A, B, C** — 1V/octave pitch
- **Gate A, B, C** — +10V gate

## Patch Ideas

**Slowly Diverging Canon** — Send the same clock to all three voices. Set Wander A to 0%, Wander B to 25%, Wander C to 50%. Voice A plays the sequence faithfully while B and C wander further, creating a canon that gradually breaks apart.

**Polyrhythmic Chords** — Send different clock divisions to each voice (e.g., 1/4, 1/6, 1/8). With low Wander and Harmonic Lock on, the voices form chords that shift at different rates.

**Generative Texture** — Set all three Wander controls to 75-100% with Harmonic Lock off. Feed slow random CV into the Wander inputs. The result is an evolving atonal texture where moments of consonance emerge and dissolve.

**Call and Response** — Use gate toggles to alternate which voices are active on each step. Voice A plays steps 1-4, Voice B plays steps 5-8. With moderate Wander, each voice develops its own variation on its half of the sequence.

**Modulated Harmony** — Patch an LFO into Wander A CV. As the LFO sweeps, Voice A alternates between faithful and deviated, creating a breathing harmonic texture against the steady B and C voices.
