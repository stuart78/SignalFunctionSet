# Trace: design

**Status: v1 built and hidden.** `src/trace.cpp`, 34HP, panel `res/trace.svg`.

Where the build departs from what is specified below:

- **Range, quantize and trigger condition are in the context menu**, per lane,
  not on screen. Only the head offset is draggable. The spec put them on screen
  and that is still where they belong; it was cut to get to something playable.
- **LEAK is a panel knob**, stated as a half-life in passes.
- **The screen window IS the loop**, with NOW centred and the paper scrolling
  under it, rather than a window narrower than the loop. So you always see
  everything, and the paper's seam travels across the screen the way a real
  loop's join does.
- **Phase lock counts bars** and places the paper at that bar's boundary. It
  began as a snap to the nearest boundary, which turned out to be the wrong
  shape of answer -- see Transport.
- Jacks landed at **22 in, 9 out**: a lane row carries its draw CV, thickness CV
  and head-offset CV on the left, and its value and ink on the right. The
  globals that modulate a knob (SPEED, SLEW, INK, SPREAD) sit under those knobs,
  which is the plugin's ordinary pot-over-jack pairing. The screen is 105mm.
- **Orientation is gone.** See below.
- **The brush buttons light in their lane's colour**, which is also what
  retired the numbers printed under them.

## The idea

A paper loop runs behind the panel. Four brushes draw lines on it, each line is
read by its own head, and each head puts out a voltage. You draw with the mouse,
or you patch a CV in and let the module draw, and the paper keeps moving either
way.

Two things carry the module, and neither is "a curve editor":

**The transport is uncoupled from the brush.** Every draw-a-shape module is a
static editor: you see the whole loop, you edit it as an object, playback is a
cursor sweeping your artwork. Here the paper moves whether you are drawing or
not, which makes it an instrument. It also gives you three behaviours for free,
depending only on where your mouse is. Draw to the left of the head and you are
editing what just played. Draw to the right and you are composing the future
with lead time. Draw on it and you are performing.

**Ink is a second dimension.** The brush lays down ink as well as position, so
every lane carries two signals: where the line is, and how heavy it is. Ink
accumulates where you draw repeatedly, and pools where the line sits still. That
gives a velocity or accent track that comes out of the same gesture as the
melody, and it gives the screen a signature: a line whose stroke width varies is
legible at a glance where a plain polyline is not.

The reference points are Daphne Oram's Oramics, which read drawn shapes off
moving film, and a chart recorder, whose pens sit at different points along the
drum. The second is why the read offsets are per lane.

## What was considered and rejected

**Global read heads.** The first shape had two or three heads, each reading all
four lanes, which is where the phase-shifted-copies idea came from. Without
polyphonic outputs it costs 24 jacks and it dies. Per-lane read offsets give the
same result for four trimpots' worth of panel, and are truer to the chart
recorder. It makes **copy lane to lane** load-bearing rather than a convenience,
since copying a shape across lanes and then offsetting their heads is how you
get the phase trick at all.

**One head offset instead of four.** The four per-lane offsets were briefly
replaced by a single global SPREAD, on the argument that for four lanes holding
four different drawings a static offset is exactly equivalent to having drawn
that lane earlier, so it mostly restates the drawing. That reasoning still
stands, and SPREAD stayed -- fanning all four heads from lane 1 is one gesture
where four drags were four. But it is not a substitute for reaching one head,
so the per-lane offsets came back and paid for themselves out of the trigger
outputs.

Worth keeping in mind either way: crossings are scanned in PAPER space, so
moving a head changes what plays together but not where ink pools.

**Polyphonic outputs.** Rejected: poly is confusing in Rack for a module whose
whole point is that you can see what each line is doing.

**Fading ink as an expressive control.** Ink that only accumulates saturates:
every cell reaches maximum and the thickness track goes flat. Leak is therefore
a **regulator**, not an expressive control, and it applies to ink only. The
value line never fades. It is the drawing, and it stays until it is overwritten.

**Vertically stacked lanes.** Four lanes stacked leaves each one too short to
draw into accurately. Overlaid in one tall strip they all get full resolution,
and the usual objection to overlaying (which line did I just grab?) does not
apply, because the brush selector has already declared which lane the mouse
owns. Overlaid is also the only arrangement where you can see the relationships
between the four, which is most of the reason to draw them together.

## The paper

A ring of fixed cell count per lane, holding a value and an ink amount per cell.

The paper is a physical length, not a duration: `LENGTH` sets how many cells go
round, and `SPEED` sets how fast they pass the head. Speeding up the transport
therefore plays your drawing faster without altering it, the way tape does.

Sizing: 1000 cells per second of paper at 1x, up to 60 seconds, so 60,000 cells
per lane maximum. As `float` value plus `float` ink that is 1.9 MB for four
lanes, which is nothing in RAM.

Resolution in time varies with speed, which is correct and also tape-like. At
one loop per bar at 120 BPM a full-length paper is running 60,000 cells through
2 seconds, far finer than any CV needs; at 60 seconds it is 1 kHz, still ample.
Reads interpolate linearly between cells.

### Persistence

The paper is the patch's content, so it has to be saved. Raw is too big: value
as `int16` scaled to +/-10V (0.3 mV resolution) and ink as `uint8` is 3 bytes
per cell per lane, 720 KB before base64.

Save at a quarter of the paper's resolution, 250 Hz, and resample on load. That
is 180 KB per instance, which is acceptable, and 250 Hz is well above anything a
hand-drawn CV line contains. A menu option to skip saving the paper is worth
having for patches that are only using CV recording.

## Lanes

Four, colour coded. Each lane holds:

| Field | Notes |
|---|---|
| value ring | volts |
| ink ring | 0 to 1 |
| read offset | 0 to 100% of the paper, dragged on screen. Fanned by the global SPREAD. It **wraps** rather than clamping, because the paper is a loop and a head swept past the end should come round the front. |
| range | unipolar 0 to 10V, or bipolar +/-5V |
| read | smooth (interpolate between cells) or stepped |
| quantize | off, N equal steps, or the patch key (see `docs/conventions/scales.md`) |
| ink source | passes, dwell, or both |

Quantize to the patch key ties Trace into the plugin's ROOT/SCALE convention, so
a drawn line lands in the same key as Note, Chance, Loom and Key.

## The brush

One brush per lane, with a target and a position. The target comes from the
mouse while you are dragging, or from that lane's CV input when it is patched
and WRITE is high. The position chases the target through the slew limit, and it
is the position that gets written to the paper. So the paper records the
brush's path, not your mouse's path, and at zero slew those are the same thing.

**The brush is only down while it is working.** It writes while you are
dragging, and it stays down after you release until it reaches its target, so
the stroke finishes. Otherwise the paper only plays back. Without this rule a
lane you touched once would keep painting a flat line over the whole loop and
you would lose the drawing.

Draw your cursor's target as a ghost alongside the brush's actual trace. At one
bar of slew the lag is enormous, and without the ghost showing what it is
chasing it reads as a bug.

### The brush position crosses threads as one value

The widget owns the mouse and the audio thread owns the paper, so the brush's
position, offset and pressed-state have to cross between them. Sending them as
separate relaxed atomics is a data race by construction: the reader can observe
any mixture of old and new. `down` arriving ahead of the coordinates activates
the brush at the *previous* stroke's position and writes a cell there, and a new
value with a stale offset writes the right voltage in the wrong place. Both are
single-cell spikes, both sporadic, and neither can be reproduced deliberately.

They cross as one value behind a seqlock, and the reader keeps the last
consistent pair rather than accepting a torn one.

Worth recording because of how it hid: the offline harness that found every
other defect here is single-threaded, so it reported this path clean however
hard it was driven. A green harness is evidence about the code it exercises and
about nothing else.

### Erase is a property of the stroke, not of the moment

Erase was applied only on the samples where the button was HELD, so the exit
taper -- which by definition runs after the release -- fell through to the
drawing path and painted the brush's own value at the end of every erase
stroke. Measured over a paper holding +3V and erased with the brush sitting at
+4V, the cell after the erased run jumped to **4.00V** and tapered down from
there: a hard step out of silence, which is exactly what a spike is.

Both ends now share **one weight**, zero where the stroke meets the paper and
one in its body, and erase forces the stroke's value to zero before that weight
is applied. The same run now reads `0.00` and ramps back into the un-erased
material.

The erase bezel is also **red** rather than a fifth white one, and the screen's
header turns orange, because it latches -- and a latched erase looks exactly
like drawing that has stopped working.

### Strokes taper at both ends

A brush that lands and lifts at full value leaves a **step** at each end, and a
click that never became a drag leaves a rectangular notch a few tens of
milliseconds wide. In the output that is a spike, and it was one: it is what
made Trace click on every stray press. So the stroke is joined to whatever it
lands on and blended back into whatever it lifts off, over 24 cells at each end.

Stated in **cells rather than milliseconds**, because the paper is the domain:
it is the physical width of the brush's entry, not a time.

This is deliberately **not** the SLEW curve, for the same reason Slice keeps its
repeat splice separate from SHAPE: hiding a discontinuity is a different job
from shaping a gesture, and it should not change character when the gesture
control moves.

The trap, which cost an afternoon and is worth writing down: the taper must
blend toward a value **captured once as the brush enters each cell**. Reading
the cell live reads back what this same stroke wrote a sample ago, since
forty-eight samples land on every cell at 1x, so the blend converges on the
brush's own value instead of on the paper. That silently collapsed a 24-cell
taper into three and left the step it existed to hide. Measured over four
gestures, the steepest edge anywhere on the paper went from 3.5 V per cell to
0.18 once the value was captured rather than re-read.

**Writing while the paper runs backwards is allowed.** It was forbidden at
first, on the theory that a brush working against the paper retraces over what
it has just laid down and the stroke eats itself. In practice the stroke stays
coherent -- it is simply drawn backwards -- and the shortest-way-round span
handling and the edge tapers cope with it, so the restriction was buying nothing
and costing a legitimate gesture. It also un-breaks ping-pong, which could
otherwise only record on half of its passes.


### Slew is rate limited, not time limited

"Draw a point and the line arrives by the end of the bar" is ambiguous: does a
1V move take as long as a 10V move, or a tenth as long? Constant time is the
common synth portamento and it is wrong here for the same reason it is wrong in
Slide: a hand crossing a distance moves at roughly constant speed, so a long
move takes longer.

So `SLEW` is stated as the time to cross the full range, and a partial move
takes proportionally less. A full-scale jump at SLEW = 1 bar arrives in a bar,
matching the original example, while a small correction is quick.

Units follow Slice's REACH: one range, and only the unit changes. Seconds when
free running, bars and beats once CLOCK is patched.

## Ink

**Write heads lay ink down, read heads lift it off, and it pools where the lines
cross.** Those three together are what make the thickness move rather than
settle: `INK` sets how much a pass of the brush adds, `LEAK` how much a pass of
a read head takes away, and the crossings supply the shape in between.

**Thickness can be written directly.** A per-lane `TH` input SETS the ink where
the INK knob accumulates it, because recording a velocity contour wants the
contour and not a running total of it. It respects the stroke taper like
everything else the brush writes.

**An empty lane is not a line.** Two undrawn lanes both sit at 0V, so every
lane that swung through zero counted as crossing both of them: ink pooled onto
lanes nobody had touched -- a row of blobs along the centre line -- and the lane
that actually moved took the deposit two or three times over. A lane joins the
crossing scan only once something has been written to it.

**The amounts have to leave room to move.** At the first cut a redrawn lane
reached 0.98 ink on its very first pass and stayed there, so the thickness had
no range to vary in and the whole feature read as dead. Rebalanced, the same
lane climbs 0.20, 0.41, 0.61, 0.82 over five passes and, left to play without
being redrawn, wears back to nothing over about twenty-five. A deposit that
saturates immediately is the same mistake as a control whose travel all lives in
its first eighth.

**The nib is small, because at a near-vertical run all of its width goes
sideways.** That is what a round tip dragged straight down actually does, but at
the original size it filled a slab rather than drawing a stroke -- and the ink
was maximal exactly there, since a fast transition through zero is where the
crossings were.

**Crossings.** A crossing belongs to the PAPER, not to any one head, so it is
scanned once per cell as the transport passes over it rather than per lane. Six
pairs at a thousand cells a second costs nothing, and it means ink gathers where
the lines meet and thins everywhere else. Two lanes that keep crossing in the
same place build a heavy mark there.

**Dwell was tried and dropped.** A brush dragged sideways lays down less per
unit area than one held steady, so deposit scaled as `1/(1 + k*|dValue/dCell|)`.
The physics is right and it read well on paper, but what it actually measured
was how steady your hand was, which is not a musical quantity. Crossings do the
shaping now and the brush lays a flat amount per cell.

**Leak is subtractive**, `ink -= wear` at the read head, and the tooltip states
it as the number of passes needed to clear a cell, which is the thing being
chosen. It was multiplicative -- a half-life -- which never reaches zero, so a
mark could fade forever without ever going.

An **erase** brush clears ink and returns the value to the lane's centre.

### Drawing ink

Three things had to be right before the stroke stopped flickering, and each was
a different mistake.

**Crossings deposit a pool, not a point.** All the ink in a single cell sits
inside one screen column at most paper lengths, so the stroke grew a
one-column barb and its width jumped as that cell scrolled across a column
boundary. Ink that pools should spread; that is what the word means.

**A column takes the MEAN of its cells, not the max.** A column spans a hundred
cells at long lengths, and the loudest one owning the whole column is the same
single-cell problem again, one layer up.

**Each column takes the MEAN of its samples, and reads the paper by
interpolation.** The centre used to be the midpoint of the column's min and max,
which hops the instant an extreme enters or leaves the window -- a fraction of a
cell of scroll is enough -- while the mean moves smoothly because every sample
only ever contributes its 1/N. And each sample read its nearest cell: sample
points slide continuously while the cell they land in changes in steps, and with
the spacing close to one cell the two beat against each other. Between them they
made a static line shimmer. Measured on a hand-drawn line held still, the
rendered centre moved **0.77 design units peak to peak and up to 0.30 in a single
frame; interpolating and averaging takes that to 0.14 and 0.025.**

**The sample count per column is fixed.** Walking `for (c = c0; c < c0 + per;
c += 1)` yields fourteen samples on one frame and fifteen on the next, purely
from where the column's sub-cell start falls, so the average moved while the
paper under it did not. A filter whose tap count depends on sub-pixel phase is
not a filter.

Modelled at 4 seconds on a 96mm screen, tracking one spot on the paper as it
scrolls, worst frame-to-frame change in the drawn width: **1.12 design units
before, 0.058 after the pool and the mean, 0.028 once the tap count stopped
drifting.** Width and path are then smoothed over a few columns and the tangent
taken over a wider span, because a brush is a round tip dragged along a line:
its width does not step between neighbouring columns and its direction does not
swing ninety degrees between them either.


The ribbon is a **polyline offset along its own normal**. Offsetting straight up
and down by the half-width makes a steep run look thin, because the ink is
spread along the slope instead of across it, while a flat run keeps its full
thickness -- so a drawn line pinched exactly where it moved fastest.

The obvious repair is to scale the vertical offset by `sqrt(1+m^2)`, which is
correct for a line and wrong for a **step**. Both ends of every stroke are
steps, the slope term saturates there, and it threw a tall spike above and below
each one: measured at a near-vertical edge it added eighteen design units of
vertical extent where it should have added none. A vertical line needs its width
added *horizontally*, which a pair of vertical offsets cannot do at all.

A real normal can. At a step the normal is horizontal, so the offset is too: the
same measurement gives 0.01 units vertically and the full half-width
horizontally, which is simply a thick vertical stroke. The browser preview
draws itself the same way, or the thumbnail advertises a stroke that pinches.

## Reading

Transport position advances at the speed. Lane `i` reads at
`(paperPos + offset[i]) mod cells`, and outputs the value on `L[i]` and the ink
on `T[i]`.

**Interpolate backwards**, from the cell the head has just left to the cell it
is on. Reading forward, from the current cell to the next, reads a cell the
brush has not reached yet. When the head and the brush sit on the same cell --
which is the default, since every offset starts at zero, and is unavoidable
while a CV is being recorded -- that next cell still holds the **previous
revolution**. The output then ramps from the value just written toward a stale
one and snaps back, once per cell: a 1 kHz sawtooth riding on the signal, which
on a scope is a thick fuzzy band rather than a line.

Measured on a 3 Hz triangle recorded through a lane, the worst sample-to-sample
step in the output was **3.92 V reading forward and 0.002 V reading backwards**,
and mean error against the input fell from 0.22 V to 0.0085 V.

The brush writes at the paper position under the mouse, which is
`paperPos + (mouseX - nowX) / pxPerCell`. Between frames the brush has to be
interpolated across every cell it traversed, not written to a single cell, or
fast transport speeds leave gaps in the stroke.

There is one **NOW** line on screen, the drawing reference. The four read
offsets are drawn as additional markers in each lane's colour, so you can see
the four read points without them competing to be the origin.

## Orientation: built, then removed

The spec's second headline idea was that events should come from the line's
SHAPE rather than its value -- each lane classifying its slope as up, down or
flat and firing on the transitions, so a scribble makes notes where it turns
around with no quantization involved. It was built and it worked: a centred
difference (the future is already drawn on the paper, so the lookahead that puts
the event on the turn rather than a window late is free), hysteresis on the flat
threshold (no hand-drawn line is ever exactly flat, and a bare threshold chatters
and emits hundreds of triggers a second on recorded CV), and one TRIG jack per
lane with the condition chosen per lane.

It came out to pay for the per-lane head offsets, and then the rest went with it
rather than being left as an orphan: the FLAT knob, the "Trigger on" menu, the
per-strip orientation glyph and activity dot. What survives is the retired
`FLAT_PARAM` and `TRIG_OUTPUT` enum slots, kept because params and outputs
serialise by index.

The honest reading is that **ink turned out to be the better second dimension**.
Both ideas answer "what else can a drawn line give you", ink answers it
continuously where orientation answers it in events, and a module carrying both
was carrying one too many. Worth remembering that this was decided by running out
of jacks rather than by listening, so if events ever want to come back, the
argument for them was never actually tested.

## Transport

**SPEED CV is 0.8/V**, so +/-5V covers the knob's whole +/-4 range. At 0.4/V it
took +/-10V to reach the ends, and with the knob at its default of 1 a CV of
about -2.5V landed exactly on zero: a stopped transport sitting in the middle of
the control's travel, which reads as the module freezing rather than reversing.

**The readout is signed.** It printed the absolute speed, so running backwards
looked identical to running forwards and a bipolar CV swinging through zero just
looked like the transport had stopped somewhere for no reason. It now carries a
direction marker.

**An interval needs two edges.** The counter that measures bar length runs from
the moment the module is created and is only reset by a BAR, so treating the
FIRST edge as a measurement measures how long the module sat there before anyone
patched it. Thirty seconds of patching became a thirty-second bar: the paper
crawled at a fifteenth of the right speed, and the phase lock then rounded it to
the nearest bar boundary, which was always zero. **That is what made BAR behave
like a reset**, and smoothing towards the true tempo took ten bars to climb out,
so it looked permanent rather than transient. A large change is now taken whole
rather than smoothed, since smoothing is right for jitter and wrong for a tempo
change.

**Phase lock counts bars rather than rounding to the nearest boundary.**
Rounding decides where the paper is from where it already got to, so a rate
estimate that is even slightly out drags it back to the same boundary every bar
instead of moving it on. Counting says which bar of the loop this is and puts
the paper there, which is correct whatever the estimate is doing.

Unclocked, `LENGTH` is the loop in seconds, 1 to 60. Clocked, it is the loop in
bars, 1 to 32 snapped, again the count-with-a-changing-unit pattern rather than
two ranges. The screen tags the readout to show which is in charge, as Slice
does.

`SPEED` multiplies, bipolar, 0 stopped at centre. Clocked, it snaps to musical
ratios so it stays in time. Keeping it bipolar keeps the tape sweep through zero,
which is worth having on its own.

Transport proper is three button-and-jack pairs in a strip under the screen,
rather than knobs:

| Control | Behaviour |
|---|---|
| **RUN** | Latch plus gate. Stopped, the paper holds and every head outputs its held value. |
| **DIR** | Latch plus gate. **Inverts** the current direction rather than setting an absolute one, so it composes with a negative SPEED instead of fighting it. Gate high means flipped. |
| **RESET** | Button plus trigger. Returns the transport to the paper origin. |

Direction as a flip is the important detail. An absolute forward/reverse control
plus a bipolar SPEED gives two ways to say the same thing and one of them has to
lose; a flip is unambiguous at every SPEED setting, and it makes reversal a
performable act rather than a knob sweep.

Reverse needs one special case and no others: **the brush cannot write while the
paper runs backwards**, for the reasons under "The brush" above. Everything else
falls out. Ink deposits the same, leak still fires once per revolution
regardless of direction (so a stopped paper simply never leaks, which answers
that question by construction), and on screen the paper scrolls the other way
while the NOW line stays put. The screen should show that writing is disabled
rather than letting a click do nothing silently.

**Ping-pong** as a menu option: reverse at each loop end instead of wrapping. It
is a few lines given DIR already exists, and it is the obvious thing to want
from a loop that has a direction.

## Panel

Screen in the centre, inputs down the left, outputs down the right, transport
and controls in a strip underneath.

**The outputs are a 4 by 3 matrix, one row per lane**: value, ink, trigger. That
is the layout the module actually has, so the panel should say so, and the four
rows carry the lane colours. LOOP sits on its own below.

The lane CV inputs align to the same four rows on the left, with the global
inputs grouped beside them: WRITE, SPEED, SLEW, CLOCK, BAR.

The transport strip under the screen holds RUN, DIR and RESET as button-and-jack
pairs, then the four knobs, SPEED, LENGTH, SLEW and BRUSH, then the brush
selector: four lanes and erase.

Read offsets, range, quantize and each lane's trigger condition live on screen
rather than on the panel, because the screen is already showing the thing they
refer to. That is what keeps the panel to four knobs.

Jack count is 25, thirteen out and twelve in, which needs three columns a side.
At 34HP, as Arrange already uses, that leaves roughly 100mm for the screen,
which a scrolling timeline needs. 28HP would squeeze the screen to about 74mm
and is probably too tight.

Note one tension to resolve in the artwork: the outputs are stacked one lane per
row, while the screen overlays all four lanes in one strip. The lane colours
have to carry that, since the rows cannot line up with anything.

## Open questions

1. **Default ink source.** Dwell alone is the most immediately expressive;
   passes alone is the more novel behaviour.
2. **Should the CV inputs overwrite or mix?** Overwrite is the tape model and is
   probably right, but mixing into what is there is a genuine second instrument.
3. **Default flat threshold.** It decides whether a drawn line is a handful of
   events or a swarm of them, so the default is most of the module's first
   impression.

Resolved: quantized lanes need no separate gate jack, since the per-lane TRIG
condition covers step changes alongside everything else. A stopped paper does
not leak, because leak fires per revolution. And the brush does not reverse,
which is deliberately the conservative choice rather than the finished answer,
so it is worth revisiting once the module is real.

## Prior art in this repo worth reusing

- **Slide**, for rate-based rather than time-based motion, and for the S-curve
  scaled to distance.
- **Slice**, for a count parameter whose unit swaps when a clock arrives, and
  for a screen anchored to NOW rather than sweeping a static buffer.
- **Phase**, for filled-envelope waveform drawing and for the double-precision
  playhead that keeps a loop from drifting over long runs.
- **Key**, for ROOT/SCALE if lanes quantize to the patch key.
