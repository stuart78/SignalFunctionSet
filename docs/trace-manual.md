# Trace Manual

## Overview

Trace is a paper loop and four brushes. You draw four CVs onto moving paper, or
let the patch draw them for you, and four read heads play them back.

It is part Oramics, which read drawn shapes off moving film, and part chart
recorder, whose pens sit at different points along the drum. That second half is
why the read offsets here are per lane rather than global.

Trace is 34HP.

## Two things carry it

Neither of them is "a curve editor".

### The transport is uncoupled from the brush

Every draw-a-shape module is a static editor: you see the whole loop, you edit
it as an object, and playback is a cursor sweeping over your artwork. Here the
paper moves whether you are drawing or not, which is what makes it an
instrument rather than an editor.

It also gives you three behaviours for free, decided by nothing except where
the mouse is:

- **Left of NOW**, you are editing what just played.
- **Right of NOW**, you are composing the future, with lead time.
- **On NOW**, you are performing.

### Ink is a second dimension

The brush lays down weight as well as position, so every lane carries two
signals. Ink accumulates over repeated passes and pools where the line sits
still. The thickness track is therefore "how settled is this line": sustains
heavy, transitions light, without anyone having drawn it.

## Controls

| Control | Range | Notes |
|---|---|---|
| SPEED | -4× to +4× | Negative runs the paper backwards. |
| LENGTH | | How much paper goes round. A length, not a duration: see below. |
| SLEW | | Smooths the read, not the drawing. |
| INK | 0–100% | How much ink a pass lays down. |
| LEAK | | How much a pass lifts. Balance against INK to decide whether repeated passes build up or wash out. |
| SPREAD | ±100% | Fans the four read heads apart. |
| RUN / REV / RESET | | Transport. |
| Brush 1-4, ERASE | | Which lane the mouse writes to, or erase. |

**The paper is a length, not a duration.** LENGTH sets how many cells go round
and SPEED sets how fast they pass the head, so speeding up shortens the loop
rather than compressing what is drawn on it. That is how a tape loop behaves.

## Inputs

Per lane: **DRAW** (draw this lane from CV instead of the mouse), **THICK**
(thickness, or velocity), and **OFFSET** (that lane's read head position).

Global: CLOCK, BAR, RESET, RUN, REV, SPEED, SLEW, INK, LEAK, SPREAD.

## Outputs

Per lane: **VALUE** (the line under the head), **OFS** (the line under that
lane's offset head), and **INK** (the weight there).

Global: **LOOP**, a trigger each time the paper comes round.

Events come from the **shape**, not the value. Each lane classifies its slope as
rising, falling or flat and fires on the transitions, so a scribble makes notes
where it turns around, with no quantization involved.

## The brush cannot be down while the paper runs backwards

This is stated as a rule about state rather than as "a direction change lifts
the brush", and the difference matters. The state version already answers what
happens if you press the mouse while reversed, which is that nothing is
written. An edge rule has to answer that separately, and gets it wrong.

A brush writing onto paper moving the other way retraces over what it has just
laid down, so the stroke eats itself. Solving that means deciding whether
reverse writing blends with what is underneath or erases it, and there is no
reason to decide that before the module exists.

## Context menu

Per lane: **Range** (bipolar ±5V or unipolar 0–10V), **Read** (smooth or
stepped, for sharp edges), **Quantize**, **Clear lane**, and **Copy to lane N**.

Copying a shape across lanes and then offsetting their heads is how you get
phase-shifted reads of one gesture, so those copy items are load-bearing rather
than a convenience.

Global: **Stroke weight** (Hairline, Fine, Normal, Bold, Heavy), **Clocks per
bar**, **Clear all paper**.

Stroke weight is how far ink is allowed to swing the width. The default used to
be the only setting and it was timid: a heavy line barely read as heavier than
a light one. At the top of the range the width varies a lot along a stroke,
which is the point. A brush that never changes width is a pen.

## Patch ideas

**Four takes of one gesture.** Draw one lane, copy it to the other three from
the menu, then wind SPREAD up. The same shape reads at four points along the
paper, which is the chart-recorder arrangement and gives you a canon from one
drawing.

**Let the patch draw.** Patch an envelope into lane 1's DRAW and a slow random
into its THICK. The paper records what the patch did, and you can then play it
back at a different speed, backwards, or with the heads spread.

**Ink as a second envelope.** Draw the same passage several times with INK low
and LEAK lower. The line stays where it is but its ink builds, so the INK output
climbs over repeats while VALUE does not.

**Turnaround sequencing.** Scribble a loose shape and take the lane's events
rather than its value. Notes land where the line changes direction, so the
rhythm comes from the drawing's shape and not from a grid.

## Technical notes

The read is a proper resampling of the paper, not nearest-neighbour. A scrolling
display and a min/max reduction are the two standard ways to make a drawn line
look like it is moving when it is not, and both were avoided here: see
`docs/trace-design.md`.

WRITE was a jack and then a parameter and is now neither. Both entries are
retired in place rather than deleted, because params and inputs serialise by
index and removing one would repatch every saved patch.
