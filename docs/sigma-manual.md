# Sigma Manual

## Overview

Sigma is an additive voice after the Crumar GDS. Nothing here generates a
waveform or filters anything away: the output is a sum of sines, and every
control is a statement about what enters that sum. It is named for the summation
sign.

16, 32 or 64 partials, sixteen voices.

Sigma is 34HP.

## The bet

The GDS gave each oscillator a 16-stage amplitude *and* frequency envelope, plus
two velocity-interpolated envelope sets. That is 512 breakpoints per voice,
which is why it needed a $30,000 computer to program it.

Sigma bets that the behaviour survives **two numbers per partial**, envelope
depth and envelope rate, because what makes a struck tone sound struck is that
its high partials die *sooner*, not merely quieter.

## Every macro is a curve across the partial index

That is the whole design. A macro does not set a value, it sets how a value
varies from the first partial to the last.

| Macro | What it spreads |
|---|---|
| TILT | Level. The spectral slope. |
| ODD/EVEN | Balance between odd and even partials. |
| STRETCH | Pitch, as `f_n = n·f₀·√(1 + B·n²)`, which is real string inharmonicity. |
| WIDTH | Pan. Positive fans the partials outward in order; negative scatters them. |
| ENV RATE | Envelope rate. Highs die sooner. |
| ENV SPREAD | Envelope start time, so the tone blooms upward, or the highs speak first and the fundamental builds underneath. |
| MORPH | Where between the two spectra velocity sits. |
| CUTOFF, RES | A spectral filter: it removes partials from the sum rather than filtering the output. |

## Velocity morphs the spectrum, it does not scale it

There are two complete level sets, **SOFT** and **LOUD**, and velocity crossfades
between them. A quiet note is a different timbre, not a quieter copy of a loud
one, which is what a real instrument does.

MORPH is the centre of that range and **MORPH SENS** (context menu) is how much
of it velocity commands. Past halfway the blend hardens into a switch, which is
what the Touch Switch preset uses to put two instruments under one keyboard.

## Controls

**Macros**: TILT, ODD/EVEN, WIDTH, STRETCH, MORPH, each a trimpot over its CV
jack. CUTOFF and RES sit together on the right.

**Envelope**: A, D, S, R. The tooltips read milliseconds, not percentages.

**Spread**: ENV RATE and ENV SPREAD, each a trimpot with its jack alongside.

**LFOs**: three, each with rate, depth, spread and a sync input.

## Inputs and outputs

The bottom row is the transport row: **GATE**, **V/OCT**, **VEL**, all
polyphonic, then **VCA**, then **L** and **R** out on a plate at the right.

## The screen

Five tabs over one shared spectrum, plus a mod matrix and a pan block.

| Tab | What it holds |
|---|---|
| LOUD | The spectrum at full velocity. |
| SOFT | The same partials at zero velocity. |
| PITCH | Per-partial detune. |
| DEPTH | Per-partial envelope depth. Below the zero line, the partial is inverted. |
| RATE | Per-partial envelope rate. |

LOUD and SOFT are one control in two halves: the two spectra velocity crosses
between. They were called LEVEL and SOFT, which made one look like "the
levels" and the other like a tone control, and hid the fact that they are a
pair. The MORPH bar along the foot runs between the same two words.

The live spectrum is drawn as a filled area, not as bars: sixty-four bars two
pixels wide stopped being a spectrum and became a comb, and bars imply the
partials are separate buckets when what they trace is one curve.

The **mod matrix** is 4 sources (LFO 1–3, ENV) by 6 destinations (level, pitch,
pan, tilt, stretch, cutoff), bipolar. Each cell is a dot resting on its zero
line rather than a bar growing from it: a bar has area, and area reads as
quantity even at 5%, so an untouched matrix looked busy.

Hover anything on the screen to read what it is and what it is set to.

## Context menu

- **Preset**: 21 of them, listed below.
- **Partials**: 16, 32 or 64.
- **Voice allocation**: First available, or Rolling.
- **PITCH tab snaps to**: Free, Cents, Semitones or Octaves. This quantizes what
  a drag produces; the stored value is cents either way.
- **Touch response**: velocity to level, where its range sits, and velocity to
  timbre.
- **LFO delay and random**: per LFO. RANDOM also frees each voice's phase, so
  periodic locks the voices together and aperiodic lets them drift apart.

## Presets

Grouped by the mechanism each one exercises rather than by timbre, because a
preset is the cheapest way to find out whether a control works.

Ramp, Square, Drawbar Organ, E-piano, Vibraphone, Marimba, Bell, Tubular Bells,
Singing Bowl, Gong, Psychedelic Gong, Bowed, Choir, Vowel, CS-80, Touch Switch,
Bloom, Cloud, Rotor, Acid, Ensemble.

A few worth knowing what they are for:

- **Marimba** is what exposed the PITCH tab spanning two cents instead of two
  octaves. A tuned bar is undercut so its first overtone is a fourth above,
  which needs real pitch offsets.
- **Bowed** is ENV SPREAD's negative half: the highs speak first and the
  fundamental builds underneath.
- **Bloom** is its positive half, plus negative DEPTH.
- **Cloud** is WIDTH fully negative, the scatter rather than the fan.
- **Acid** is the only preset where the spectral filter is the instrument.
- **Ensemble** uses PITCH as fine detune.
- **Rotor** is pan spread, with LFO 2 sweeping it.

## Technical notes

Two faults are worth recording, because both were audible and neither was where
it seemed to be.

**The clicking was the note-on, not the attack.** `g = V.mEnv` is the VCA, and a
note-on zeroed it outright, so retriggering a still-sounding voice threw the
output to silence in one sample, measured at 0.55 to 0.94 of full scale on
thirteen of fourteen presets. The envelope now re-attacks from wherever it
already is.

**ENV RATE was multiplying the attack as well as the decay**, though it means
"highs die sooner". A 64-partial Gong's top partial opened in 92 µs, far less
than one cycle of anything it was playing, and an envelope segment shorter than
a cycle is a step. The spread is now confined to decay and release.

Both are verified by `tools/sigma-envelope-harness.py`, which extracts
`advance()`, `loadPreset()` and the note-on block **verbatim** from `sigma.cpp`
and fails loudly rather than testing a stale copy.

The design document is `docs/sigma-design.md`.
