# Chime Manual

## Overview

Chime is an eight-note resonating drone machine, built after a xylophone whose
resonator tubes rotate beneath the bars.

Each note has its own semi-free bidirectional LFO, its "tube rotation", and
the note blooms as its tube swings through centre. Nothing steps. The eight
rotations drift against each other and the pattern that comes out is whatever
their periods happen to make, which is why it never quite repeats and never
sounds random either.

Chime is 28HP.

## How it works

Picture eight tuned bars, each with a tube under it, and each tube slowly
turning. When a tube points straight up at its bar, that bar is coupled to its
resonator and rings; as the tube turns away, the coupling falls off.

Two things follow from that mechanism, and both are audible:

- **Every rotation crosses centre twice.** Once going one way, once coming back.
  So a struck note fires twice per rotation, not once.
- **The bloom is the crossing.** Notes do not have an envelope you set; they
  swell and fade because the tube is moving.

RATE sets how fast the tubes turn, SPREAD sets how different the eight rates
are, and DRIFT lets them wander rather than holding a fixed period.

## Controls

**The rotation**

| Control | Range | What it does |
|---|---|---|
| **RATE** | 0.02–2 Hz | How fast the tubes turn. At the bottom a note blooms once every 25 seconds. |
| **SPREAD** | 0–100% | How far apart the eight rates are. At 0 they move together and the whole rack breathes as one; turned up they scatter. In Ripple mode this becomes the coupling amount instead. |
| **DRIFT** | 0–100% | How freely each rate wanders from its nominal value. |
| **RELATE** | 4 modes | How the eight rates *relate*. See below. |
| **SHAPE** | −1 to +1 | The rotation curve: exponential, linear, or logarithmic. How long a tube lingers at the extremes versus rushing through centre. |

### RELATE

- **Ramp.** The rates increase smoothly from note 1 to note 8.
- **Stepped.** They take discrete related values.
- **Random.** Scattered, reseeded by the RESEED button.
- **Ripple.** SPREAD becomes coupling: a crossing excites its *neighbours*, so
  blooms travel down the row like a wave rather than each note minding its own
  business.

Ripple is the one that turns eight independent drones into a single instrument.

**The sound**

| Control | Range | What it does |
|---|---|---|
| **EXCITE** | 0–100% | A continuous blend from **bow** to **strike**. Bowed notes are driven continuously; struck notes fire at each centre crossing. |
| **DECAY** | 0.3–8 s | Ring and bloom time. |
| **OCT** | ±3 | Octave. |
| **ROOT** | 12 semitones | The key. |
| **SCALE** | canonical list | See [scales.md](conventions/scales.md). |
| **RESEED** | button | New random rates. |

**Per note (×8)**

| Control | What it does |
|---|---|
| **DEGREE** | Which scale degree this note plays. |
| **WEIGHT** | Strike likelihood, and bow level. A note at 0 is silent; the rest of the row still rings around it. |
| **ARC** | Arc width, 10–100%. A *narrower* arc means the tube crosses centre more often, so the note strikes more often. It trades width for rate. |

## Pitch latches at note boundaries

Retuning while a note is sounding does not bend it. A note takes its pitch when
it begins and holds it until it ends, so you can move ROOT, SCALE or a DEGREE
knob during a drone and hear the change arrive on the *next* bloom rather than
as a glide. That is what makes live retuning usable.

## Inputs

RATE, SPREAD, DRIFT, ROOT (1V/oct, semitone-quantized), SCALE (1V per scale),
RESEED, **CLOCK** (syncs the rotations), EXCITE (0–10V, bow → strike), OCT (1V
per octave), RELATE (0–10V across the four modes), SHAPE, DECAY (1 s/V).

## Outputs

| Output | What it does |
|---|---|
| **8 × tube LFO** | ±5V, one per note. The rotation itself, as a control voltage: the same signal that is blooming the note. |
| **8 × audio** | Each note on its own jack. |
| **MIX L / MIX R** | The stereo mix. |
| **V/OCT** | Polyphonic, 8 channels: what each note is currently tuned to. |
| **GATE** | Polyphonic, 8 channels: high while that note blooms. |

The tube LFO outputs are the most useful thing here after the audio. They are
slow, non-repeating and musically related to what you are hearing, which makes
them far better modulation sources than an unrelated LFO bank. Patch note 3's
LFO to a filter and the filter moves *with* note 3.

The V/OCT and GATE pair let Chime drive another voice entirely: the mechanism
becomes a sequencer and something else makes the sound.

## Patch ideas

**A drone you leave running.** RATE near the bottom, SPREAD around 35%, EXCITE
fully to bow, DECAY long. Nothing patched. It will not repeat.

**Ripple.** RELATE to Ripple, SPREAD up, EXCITE toward strike. Blooms travel
along the row and the eight notes become one instrument.

**Chime as a sequencer.** V/OCT and GATE into another polyphonic voice. You get
an eight-voice part with organic timing that no step sequencer would produce.

**Modulation from the mechanism.** The eight tube LFOs to eight destinations
elsewhere in the patch. Everything in the rack breathes on the same slow
mechanism the chime does.

**Clocked, but not gridded.** CLOCK in from Meter's bar output: the rotations
sync loosely to the song without becoming a step sequence.

**One note at a time.** WEIGHT to 0 on seven notes, and modulate the eighth's
DEGREE. A single bar in a big empty room.
