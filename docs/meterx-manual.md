# Meter X Manual

## Overview

Meter X is an expander for [Meter](meter-manual.md). Meter's own panel covers the musical subdivisions: bar, quarter, eighth, sixteenth, triplets, each with swing. Meter X covers the other half: the **long game**. It gives you a high-resolution 24 PPQN clock, a run gate, and bar-multiple triggers all the way out to 128 bars, so you can build structure that unfolds over minutes rather than beats.

Meter X is 8HP.

## Connecting it

**Place Meter X immediately to the right of Meter.** That's the whole setup: no cables. It reads Meter's clock over the expander bus. If it isn't directly to Meter's right, its outputs stay silent.

## Outputs

Ten rows, top to bottom:

| Output | What it does |
|---|---|
| **24 PPQN** | 24 pulses per quarter note, the MIDI-standard clock resolution. Straight (un-swung), so it stays a true timing reference regardless of Meter's swing settings. |
| **RUN** | 10V while Meter is running, 0V when stopped. |
| **BAR** | One trigger per bar, on the downbeat. |
| **2 / 4 / 8 / 16 / 32 / 64 / 128** | A trigger every N bars, on the downbeat of that bar. |

All bar triggers are aligned to reset: they fire on bars 1, 1+N, 1+2N, and so on, so at reset every row fires together, and the whole hierarchy stays locked to the song from that point.

Each row has an **activity LED** (a short flash on each pulse; RUN is steady).

## The pies

Each bar row has a small **pie chart** that fills clockwise as you advance through that output's cycle, then resets when it fires. The 4-bar pie fills over four bars; the 128-bar pie creeps around over 128.

This is what makes long-form structure legible. Instead of counting bars in your head, you can see at a glance that you're three-quarters of the way through a 32-bar section. The pies freeze while Meter is stopped.

The 24 PPQN and RUN rows have no pie, because they have no cycle to show.

## Why 24 PPQN

24 pulses per quarter is the MIDI clock standard. It's the rate to use when you're driving something that expects a real clock resolution rather than a musical subdivision:

- Modules that derive their own subdivisions internally
- Anything doing swing or shuffle of its own (Meter X's 24 PPQN is deliberately un-swung, so downstream swing isn't applied twice)
- MIDI interfaces and clock converters

Meter's own PPQN context-menu setting is about the clock it *receives*; Meter X's 24 PPQN is a clock it *sends*.

## Patch ideas

**Long-form structure**: 32-bar out into a sequencer's reset, 8-bar out into a pattern advance. The piece reorganises itself on a schedule you can see coming.

**Section-aware modulation**: a 16-bar trigger into a sample & hold fed by noise: a new random voltage every 16 bars, for a filter cutoff or a chord inversion that changes once per section.

**Run-gated drone**: RUN into a VCA or envelope gate so a pad sounds only while the clock runs.

**Slow evolution**: 64- or 128-bar triggers into an envelope with a long rise, for changes that take minutes to arrive.

**Driving Arrange**: Meter's BAR out drives Arrange; use Meter X's longer multiples to trigger events *within* a section that Arrange doesn't know about.
