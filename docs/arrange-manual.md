# Arrange Manual

## Overview

Arrange is the song-form brain that sits above the rest of the sequencing rig. Where Beat, Note, and Chance each play a part, Arrange decides **what section you're in**: how many bars it lasts, what key and tempo it's in, and which instruments are playing during it.

It's a single horizontal chain of **8 phrases**. A phrase is a bar-length section: intro, verse, chorus, break. The arrangement walks through the active phrases in order and wraps.

Arrange is 34HP. It doesn't generate a clock, because **Meter is the master**. Arrange counts Meter's bars and hands out gated, divided clocks to your instruments.

## Wiring it up

Arrange is driven by two inputs from Meter:

- **BAR in** ← Meter's BAR out. One pulse per bar; this is what advances the arrangement.
- **CLOCK in** ← whichever Meter subdivision you want as the base rate. This is divided per channel.
- **RESET in** / **RST** button: back to the first active phrase.

Then feed the tempo back the other way: **BPM out** → Meter's **BPM CV**, and turn on Meter's **"BPM CV absolute"** context-menu option. That switches Meter's BPM CV from an offset to an absolute 0.01V/BPM reading, so each phrase's BPM trimpot sets the actual tempo.

## Per phrase

Each of the 8 phrase columns carries:

| Thing | Where | What it does |
|---|---|---|
| **Bar length** | 4×4 grid on screen | 1–16 bars. Click a cell to set the length. |
| **Enable** | phrase cell | Double-click to toggle. Inactive phrases are skipped. |
| **Scale / Root / BPM** | three trimpot rows | The phrase's key and tempo. |
| **Channel enables** | 4 coloured bars | Which instruments play during this phrase. |
| **GATE out** | jack under the column | High for the whole time this phrase is playing. |

The screen shows the chain left to right with an arrow between phrases, the playing phrase outlined in orange, the focused phrase in blue, and the current bar lit in the grid.

## Linking (BPM, root, scale)

Most songs don't change key every eight bars. The small **LED dots between the trimpot columns** cascade values down the chain, one dot per row, so **scale, root, and BPM link independently**. They're **on by default**.

- **Linked**: the phrase inherits its group's leftmost phrase's value *exactly*. Its own trimpot is ignored, and the value renders dim on screen so followers are obvious at a glance.
- **Break a link**: that phrase becomes independent and its own trimpot applies again, starting a new group that the phrases after it inherit from.

So the common case, one key and tempo for the whole song, is the default, and a key change is one dot away.

> Note: this is *exact inheritance*, deliberately unlike Cycle's link buttons (where a follower's pot becomes an offset). For an arrangement, "linked" means "same key," so a follower's stale trimpot position can never transpose it.

## The four channels

The right-hand inset is the heart of the module. The 4 channels are **per-instrument clock buses**: each one feeds one instrument (a Beat, a Note, a Chance), and each phrase decides which of them are playing.

Per channel:

| Output | What it does |
|---|---|
| **DIV** (trimpot) | Clock division of the master CLOCK in: ÷1, 2, 3, 4, 6, 8, 12, 16. Re-aligns to the bar. |
| **CLOCK** | The divided clock. |
| **BAR** | One pulse per bar. |
| **RESET** | Fires on a master reset **and** whenever the channel re-enables after being off. |
| **EOC** | Fires when the arrangement wraps. |

**CLOCK and BAR are muted on any phrase where that channel is disabled.** That's the whole point: toggle a channel's coloured bar off for the verse and that instrument simply stops, with no muting downstream, no silence tricks. Toggle it back on for the chorus and its **RESET** fires so the instrument re-enters in sync rather than mid-pattern.

Division counters keep running while a channel is off, so a returning instrument stays phase-locked to the master rather than restarting on a random subdivision.

## Master outputs

At the bottom of the inset:

| Output | Convention |
|---|---|
| **BPM** | 0.01V per BPM → Meter's BPM CV (with "BPM CV absolute" on). |
| **ROOT** | 1V/oct, semitone-quantized, into Note's or Chance's root CV. |
| **SCALE** | 1V per scale, into Note's or Chance's scale CV. |
| **PHRASE** | 1V per phrase (phrase 1 = 0V). |

ROOT and SCALE use the shared scale list, so they're interchangeable across Note, Fugue, Muse, Chance, and MetaFugue. One arrangement drives the key of the whole patch.

There is deliberately **no master Bar or Clock output**, because Meter is already the source of both, and instruments should take their channel's gated, divided versions so they drop in and out with the arrangement.

## Screen interactions

| Action | Result |
|---|---|
| Click a grid cell | Set that phrase's bar length |
| Click a coloured bar | Toggle that channel for that phrase |
| Click a phrase cell | Focus it |
| Double-click a phrase cell | Enable / disable the phrase |
| Right-click a phrase cell | Root and Scale menus |

## Patch ideas

**A song**: Meter BAR → Arrange BAR in, Meter's sixteenth → CLOCK in, Arrange BPM → Meter BPM CV (absolute on). Channel 1 → a Beat (drums), channel 2 → a Note (bass, ÷2), channel 3 → a Chance (lead). Set P1 = 4 bars intro with only channel 1 on; P2–P3 = 8-bar verse with 1+2; P4 = 16-bar chorus with all three. The band arrives and leaves on its own.

**Key change at the bridge**: break the root link on P5 and set its ROOT trimpot up a fourth. P5 onward inherits the new key until the next break.

**Half-time break**: leave the channels on but set channel 1's DIV to ÷2 for a section.

**Per-section modulation**: take a phrase's GATE out into an envelope or VCA to fade something in for exactly that section.
