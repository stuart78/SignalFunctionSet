# Fill Manual

## Overview

Fill is an eight-channel drum sequencer that plays itself.

Where [Beat](beat-manual.md) is one voice you program step by step, Fill is a
whole kit that plays from a library of 343 pattern sets and decides on its own
when to break. You choose a set and how restless it should be; it handles the
bars.

Fill is 32HP and needs an external clock.

## Pressure

The idea the module is built around, and worth understanding before anything
else.

One internal value, **pressure**, accumulates a little every bar and vents as
a fill. That single number drives two things at once:

- **The intensity tier**: sparse, main, or lift. Latched per phrase, so it does
  not flicker mid-idea.
- **A seeded variation layer** on top of whatever pattern is playing.

**ACCUM** is how much pressure builds per bar. **DISCHARGE** is how much of it
each fill spends. Turn ACCUM up and the kit gets restless; turn DISCHARGE up and
each fill costs more, so they come further apart but hit harder.

This is why Fill does not sound like a randomiser. Pressure is a single
accumulating quantity, so the fills arrive in a rhythm of their own rather than
independently at random.

## Playback is clock-driven

Steps fire on **CLOCK** edges, and `clocksPerBar` is measured between **BAR**
pulses. Fill has no internal timebase at all.

That means it freezes the moment the clock stops, rather than free-running away
from the rest of the patch. It also means the clock's swing, if it has any, is
Fill's swing too. Patch Meter's sixteenth into CLOCK and its BAR into BAR.

## Controls

| Control | Range | What it does |
|---|---|---|
| **ACCUM** | 0–100% | Pressure built per bar. |
| **DISCHARGE** | 0–100% | Pressure vented per fill. |
| **TIER** | −2 to +2 | Offset on the intensity tier. 0 follows pressure; turn it up to force the kit louder than the pressure says. |
| **PHRASE** | 4 / 8 / 16 bars | The phrase length fills organise themselves around. |
| **EXTRAS** | 0–8 | **A hard cap** on engine-added notes per bar. |
| **SET** | the library | Which pattern set. |
| **SWING** (×8) | 0–50% | Per channel, off-beats delayed. |
| **RESET / NEXT / RESEED** | buttons | Back to the start; advance the queue; roll a new variation. |

### EXTRAS is a ceiling, not an amount

It is the maximum number of notes the variation layer may add in a bar, and each
set carries its own `vary` value limiting how far its identity may bend. The
generator can decorate a pattern; it cannot turn it into a different one. That
is deliberate: a disco set should still sound like disco at full restlessness.

## Channels

**Kick, Snare, Closed hat, Open hat, Low perc, High perc, Clap/rim, Bell.**

Each has a gate, a velocity and an accent output, and its own swing control.

## The library

**Three shipped banks**, loaded in order so the canonical one's first set is the
default:

- a canonical set
- a General MIDI set
- a **disco** bank built from Rothman's method rather than his exercises

Plus your own, from `<Rack user dir>/SignalFunctionSet/patterns/`. The context
menu has **Open user patterns folder** to find it.

**drum-patterns.com `.txt` exports import natively**. Drop one in the folder
and it appears.

### The browser

Five tabs: **PATTERN · GENRE · REGION · USER · FAVS**.

Favourites persist plugin-wide in `fill-favorites.json`, not per patch, so a set
you starred in one project is starred in the next.

### The queue

Line sets up by hand and Fill plays through them. Each row has a play button,
and rows can be dragged to reorder. **Queue advance** in the context menu chooses
between advancing *by repeat count* and *by NEXT input only*.

## Inputs

**CLOCK** (required), **BAR** (the downbeat), RESET, NEXT (advance the queue at
the next cycle), RESEED, PHRASE CV (1V per phrase length), ACCUM CV, DISCHARGE
CV, TIER CV (±5V), SET CV, EXTRAS CV (0–10V → 0–8).

## Outputs

Per channel: **GATE**, **VELOCITY**, **ACCENT**: 24 jacks.

Plus:

| Output | What it does |
|---|---|
| **FILL** | High during a fill. Use it to duck a pad or open a reverb send exactly when the kit breaks. |
| **SET** | 1V per set. |
| **NUM** | Time-signature numerator, 0.5V per count. |
| **DEN** | Denominator, 1V per index (1, 2, 4, 8, 16, 32). |
| **BPM** | The set's tempo, 0.01V/BPM. |

### NUM, DEN and BPM go back to Meter

This is the loop that makes Fill more than a drum machine. Patch NUM and DEN
into Meter's **Time signature CV absolute** inputs and BPM into its **BPM CV
absolute**, and choosing a set in 5/4 at 104 BPM retimes the entire patch.

The pattern library is then not just drums. It is a source of time signatures
and tempos, and Fill becomes the thing that decides what the song is.

## Context menu

| Item | Options |
|---|---|
| **Clock resolution** | How finely the incoming clock is interpreted. |
| **Fills** | Synced to phrase, or free. |
| **Play fills** | Whether fills play at all. |
| **Queue advance** | By repeat count / by NEXT input only. Plus **Clear queue**. |
| **Tier follows** | Rise through the phrase / one tier per phrase. |
| **Tier balance** | Even thirds / slow build. How the three intensity tiers divide up the pressure range. |
| **Open user patterns folder** | Where to put your own banks. |

## Patch ideas

**Let it play.** CLOCK and BAR from Meter, the eight gate outputs to eight drum
voices, ACCUM around 20% and DISCHARGE at 50%. Then leave it. That is the
intended use.

**The kit decides the tempo.** NUM, DEN and BPM back into Meter as above. Change
set and the song changes metre.

**Duck on the fill.** FILL output into a VCA on a pad bus, inverted. The pad
drops out exactly when the drums break and comes back on the downbeat.

**Hand-built arrangement.** Queue four sets in order, **Queue advance → By NEXT
input only**, and drive NEXT from Arrange's phrase gate. Each section of the
song gets its own kit.

**Velocity matters.** The velocity outputs are not decoration. Patch them to
your drum voices' level or decay inputs. Most of the difference between Fill
sounding like a machine and sounding like a player is in those eight cables.

**Freeze the variation.** EXTRAS to 0 and the engine stops adding anything: you
get the pattern as written, with only the tier changing. A good way to hear what
a set actually is before letting the engine at it.
