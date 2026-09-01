# Wheel: design

**Status: v0 built, hidden.**

A drone instrument after the hurdy-gurdy (vielle à roue). Named for the
mechanism, because the mechanism is the whole idea: a single rosined wheel bows
every string at once, so nothing in the instrument is independent of anything
else. Five drone voices and a trompette, one wheel, one crank.

Working width 30HP.

## What actually carries the identity

Four things, in the order they matter. Everything in this design is downstream
of them.

**The wheel is one bow.** Five oscillators with five LFOs sound like an organ.
Five oscillators sharing *one* slightly irregular modulator sound like one
physical object being played by one person. The wheel is not round: it is
slightly eccentric, the rosin sits on it unevenly, and there is a seam where it
was joined. Every revolution therefore imposes the *same* amplitude-and-
brightness ripple on every string at once, at 1–4 Hz. Common-mode modulation is
the tell.

**The dog.** The trompette string crosses a bridge with one loose foot. When the
player accelerates the crank the foot lifts and rattles against the soundboard —
a percussive buzz at the trompette's pitch. This is the rhythm section, and it is
the whole difference between a hurdy-gurdy and a bagpipe.

**Nothing articulates independently.** No note-offs, no per-string attack. A
string is on the wheel or it is lifted off it. All articulation comes from the
wheel and from the dog.

**Beatless tuning.** Drones are tuned by ear to octaves and fifths, which means
just intonation, not twelve-tone equal temperament. This matters more than it
sounds — see *Phase needs just intonation* below.

Two cheap extras that are mostly invisible until they are missing: continuous
slip-stick rosin noise scaling with wheel speed, and a strong wooden body
resonance.

## Coups par tour

The one fact that reorganised this design. Hurdy-gurdy rhythm is not free — it is
counted in **strokes per turn of the wheel**. Technique is named that way:
*quatre coups par tour*, four wrist accents per revolution. The dog's pattern is
a subdivision of the wheel's rotation.

So CRANK is the tempo, `COUPS` (1–8) is the subdivision, and which of those slots
actually gets a stroke is a ring of marks around the rim of the wheel display,
clicked to toggle. The display is the sequencer. Clock CRANK from Meter and the
whole instrument is locked to the patch; leave it free and it plays itself.

## The wheel, concretely

One `wheelPhase` per module, advancing at crank speed. The ripple is a **fixed
periodic profile**, not noise — that is the point of it:

- eccentricity → first harmonic
- the seam → a narrow bump at one angle
- uneven rosin → a couple of low harmonics

The same shape every revolution, with a very slow random wander on top (rosin
wearing, humidity) so it is not a perfect loop. It drives **amplitude, lowpass
cutoff and noise level together** — one object, three consequences. Crank speed
independently raises brightness and the noise floor: crank harder, play louder
and brighter, as on the instrument.

`SPREAD` gives each voice a small phase offset around the wheel, ±20% of a
revolution. This is a liberty and should be recorded as one: on a real gurdy the
strings all contact near the top of the wheel and are close to in phase. At
SPREAD 0 the instrument pumps; a little makes it breathe. Default ≈15%.

## The dog, concretely

**It fires on the strength of the stroke, and the design note that said
"acceleration" was right about the mechanism and wrong about the number.**

The first implementation did exactly what this document originally specified:
watch `d(speed)/dt` and fire when it crosses a threshold set by DOG. Measured
with `tools/wheel-dog-harness.py`, which extracts the real block from wheel.cpp
rather than a retyped copy of it, that was unusable in three separate ways:

- **60% of the DOG knob was dead.** Above about 0.4 nothing fired at any tempo.
- **The weak strokes of the turn never fired at all**, at any setting — so the
  knob was a mute switch rather than a threshold, with two states and nothing
  between them.
- **Cranking FASTER silenced the dog.** Backwards, and the exact opposite of the
  technique. Closely-spaced strokes land on a speed envelope that has not yet
  come back down, so they accelerate it less; the knob was measuring the
  envelope's shape and the spacing of the strokes, not the strokes.

Acceleration is what a stroke *does*. The stroke's own strength is what the foot
actually meets, and it is the same physical quantity read where it is still
clean, before an envelope has smeared it. So the threshold is on strength.
Crank speed keeps its part — a faster wheel makes the foot readier and the buzz
louder — as a term in the strength rather than as something that can take the
threshold away with it.

Measured after the change: every position of DOG changes the pattern, at every
crank speed from 0.5 to 6 rev/s, and cranking faster brings more strokes in.

**The accent ladder has to be SMOOTH, and the first one was not.** It was written
as hand rules — downbeat, half-turn, quarter-turns when n≥8, then odd/even — and
at eight coups that produced `1.00 · 0.52 · 0.70 · 0.52 · 0.82 · 0.52 · 0.70 ·
0.52`: four of the eight strokes identical, with a 0.18 gap above them. DOG fell
from "all eight" to "four" almost immediately and then sat on four for two thirds
of its travel, so eight coups sounded like one. The rules were also blind to odd
meters: at n = 6 nothing matched, and every stroke but the downbeat came out the
same.

`gcd(k, n)` says how coarse a grid a stroke lands on, which is the metric
hierarchy and is what a wrist is actually doing — and it is general, so n = 6
correctly gets stroke 3 as the half and strokes 2 and 4 as the thirds. The ladder
is `0.62 + 0.38·log2(gcd)/log2(n)`, evenly spaced, and DOG now sweeps
continuously from every stroke to the downbeat alone across its whole range.
Default DOG is 0.10, which fires everything at a normal crank: eight armed slots
should buzz eight times before you start taking them away.

**The retrigger guard has to be shorter than the gap between strokes.** A fixed
20 ms swallowed every other coup at eight per turn on a fast crank, where they
arrive 21 ms apart. It scales with `coups × crank` now.

**Per-stroke accents are load-bearing, not decoration.** They were listed below
as an open question — "accents will be wanted eventually". They turned out to be
the thing that gives DOG anything to discriminate between: with every stroke
identical the threshold can only be all-or-nothing. The downbeat is 1.00, the
half-turn 0.82, the quarter-turns 0.70 (only when there are eight or more), and
the rest 0.52 or 0.62 by parity. The ±9% jitter on top humanises, and also turns
each threshold crossing into a band where a stroke fires *sometimes*, so DOG
sweeps the density of the buzz continuously instead of stepping between flat
régimes.

The buzz itself is the trompette's own tone under a fast rattle envelope: sharp
attack, 40–150 ms decay (`DECAY`), rattle rate ~30–80 Hz scaling with intensity,
plus a short high-passed click where the foot strikes the soundboard. The string
is *chopped* at the rattle rate rather than having a buzz added to it, which is
what the loose foot physically does.

**A coup swells everything.** The stroke lifts crank speed briefly, so all five
drones get louder and brighter with it. That common-mode swell is half of why
the accent lands; a buzz on its own reads as a separate percussion module
sitting next to a drone.

## Six voices, one anchor

Voice 1 is locked to the root, and the panel shows it — no degree slider, fewer
trims than the others. The hierarchy is visible rather than documented.

| | top slider (snapped) | trims | LEVEL | ON |
|---|---|---|---|---|
| **V1** | OCTAVE −3…+2 | WAVE · PRESS | ✓ | ✓ |
| **V2–V5** | DEGREE | OCT · WAVE · PHASE · PRESS | ✓ | ✓ |
| **Trompette** | DEGREE (default root) | OCT · WAVE · DECAY | ✓ | ✓ |

DEGREE is a degree **within one period**, with the octave on its own trim. Five
sliders reading 0–6 side by side draw the voicing as a shape you can grab; one
long ±2-octave slider with 29 positions draws noise.

### PRESS is not the fader, and neither is ON

Three ways to quieten a voice, doing three different jobs:

- **LEVEL** is the mix. Post-everything, the modular convenience.
- **PRESS** is how hard that string sits on the wheel — the cotton wrapping. It
  changes *timbre* before it changes level: a string barely touching the wheel
  is duller, slips, and whistles rather than speaking. It reaches zero, but it
  is a physical continuum that happens to reach zero.
- **ON** is lifting the string off the wheel. Players shim strings with folded
  paper between phrases: binary, and a performance act. Its own latch and LED,
  and the lifted string's contact point goes dark on the rim.

Folding ON into the bottom of PRESS or of LEVEL would be the Slice `Square`
mistake again — a hidden mode wearing a continuous control's clothes.

This is also what settles the per-voice input: it drives **PRESS**, not the VCA.
Pressing a string onto the wheel gives attack timbre for free, which a VCA
cannot. Menu escape to plain VCA for anyone who wants the clean thing.

### The gate is a hand, not an exponential

The per-voice input is a **GATE**, because on the instrument there IS no
envelope: the string is against the wheel or it is not, and the shape of the
onset belongs to the wheel rather than to the player. `SWELL` is that shape —
8 ms to 1.2 s, how long the wheel takes to grip — and the number is literal, the
contour completing in exactly that time. (A one-pole's "time" was a 63% constant,
where 90% took 2.3× as long, so the readout meant nothing you could count on.)

**A one-pole slew starts at its maximum velocity, which is the one thing a hand
cannot do: it has mass.** The contour is a minimum-jerk profile instead (Flash &
Hogan, 1985), `s(t) = 10t³ − 15t⁴ + 6t⁵` — zero velocity *and* zero acceleration
at both ends. Measured opening velocity is exactly 0.0000 per sample against the
one-pole's 0.0026.

Two further things a finger does:

- **It overshoots its resting depth and settles back, and only when it moves
  fast.** Measured +17.6% at an 8 ms press, +16.3% at 45 ms, +4.9% at 200 ms:
  overshoot is mass rather than intent, and a slow deliberate press has almost
  none. On a bowed string that momentary extra pressure is the bite at the start
  of the note. It is scaled to the *distance* moved, as Slide's bar travel is, so
  pressing harder on an already-sounding string does not jump.
- **It is never twice the same.** ±12% on the timing, ±30% on the overshoot.

Releasing is 1.25× slower than pressing: lifting off a turning wheel is a
release, not a stop. `tools/wheel-dog-harness.py 4` prints all of it.

**Two contours, because they are two different acts.** The gate is a hand
pressing a string onto the wheel: SWELL long, and it overshoots. The latch is
shimming a string off the wheel between phrases: discrete, always 25 ms, and no
overshoot, because nothing is being pressed. Folded into one, setting a slow gate
made lifting a string take a second as well.

**The LED follows the latch, not the gate.** It says whether the string is on the
wheel, which is exactly what a gate does not change. A gated-off string keeps its
panel LED lit and goes dark on the *screen*, where the rim dot follows what is
actually speaking.

## Phase needs just intonation

A fifth in 12-TET is 2^(7/12) = 1.4983, not 1.5. The third harmonic of the root
and the second of the fifth therefore beat at about 0.9 Hz at C4: the relative
phase rotates through a full cycle roughly once a second, and a *static* phase
offset is inaudible — it only picks where in that rotation you happen to start.

`TEMPER` at 1 snaps every voice to the nearest just ratio against the root; at 0
it plays the scale as written.

**It looks broken on a traditional voicing, and it is not.** Measured with
`tools/wheel-dog-harness.py 6`: equal temperament is already within 2 cents of
just at the fifth and exact at the octave and the unison, so on a drone setup of
roots, octaves and fifths — which is what a hurdy-gurdy *is* — TEMPER has almost
nothing to move. On the shipped default voicing only one voice of six shifts at
all, the third, by 13.7 cents. It bites on thirds and sixths (±14–16 cents) and
sevenths (±12), and nowhere else.

So the readout says what TEMPER is DOING rather than what it is set to: `JUST
14c` when there is something for it to move, `JUST 0c` when there is not,
`EQUAL` when it is off, `SCALE` when the key is not 12-TET and it is inert. A
control whose effect depends on a setting somewhere else needs to say so, or it
reads as a control that does not work. At 0 the drones are beatless, which is how the
instrument is tuned by ear, and PHASE becomes a control that holds still and
reshapes the composite waveform instead of a control that does nothing slowly.
`DETUNE` puts the beating back deliberately, per voice, for the chorus real
unisons have.

## Oscillators, not waveguides

Loom already owns bowed-string physical modelling in this plugin, and its bow
model carries a lot of hard-won machinery (envelope-normalised friction curve,
pressure regulator, load-bearing hair noise) that a drone box should not
re-litigate. The character here comes from the wheel, not from the string.

Waveforms are sawtooth-anchored — Helmholtz motion *is* a sawtooth, and morphing
toward square reads as more bow pressure. Then a shared body resonator,
slip-stick noise, and the wheel ripple over everything. Cheap enough to run six
voices without thinking about it, and every control stays legible.

## Reading the key

ROOT is 1V/oct semitone-quantised and SCALE is 1V/scale in the plugin
convention, and SCALE reads **Key's polyphonic extension** (channel 0 = index,
channel 1 = period in volts, channels 2+ = degrees as 1V/oct offsets), so a
Scala or Pelog key reaches Wheel intact. Three consequences:

- **DEGREE wraps rather than clamps.** Scales run 5–14 degrees. Configure the
  slider 0–13 and wrap: degree 8 in a 7-note scale is degree 1 up a period. No
  dead travel when a small scale is selected.
- **OCT adds the period, not 12 semitones.** Bohlen-Pierce's period is 19.02.
- **TEMPER goes inert on a non-12-TET scale,** the same way Key's free
  sub-scale option does. Snapping Pelog toward just ratios makes it not Pelog:
  its intervals *are* the tuning, and there is no intended ratio to snap toward.
  The readout says so rather than the control silently doing nothing.

## Two amplitude faults, both measured

**RIPPLE was scaled to almost nothing.** The gain factor was `1 + depth·r·0.55`
over a ripple running −0.80..+0.54, so the *default* setting was a 1.5 dB wobble
and the maximum was 3.5 dB. On a sustained drone that is close to inaudible: the
control had a full range of travel and almost no consequence over it. It also
barely touched brightness (a 0.35 factor), and brightness is the more audible
half of what wheel grip actually varies — a bow pressing harder gets brighter
before it gets louder. Now 1.15 on level and 0.9 on brightness, and the rosin
follows the ripple too. `tools/wheel-dog-harness.py 3` prints the resulting dB
swing per setting: 0 at the bottom, −4.6..+2.1 dB at the default, −21..+4 dB at
the top, asymmetric because the seam is a loss of grip rather than a gain of it.

**The dog fired correctly and could not be heard.** The buzz chopped the
trompette's amplitude at the rattle rate — right, and enough on its own — but
left the voice at its drone level, one of six at 0.55. A chien is the accent of
the whole instrument, not a texture on one string. It now gets a +10 dB surge
while it buzzes, a `tanh` drive to make it broadband, a louder soundboard click,
and a lower rattle rate (21–60 Hz rather than 34–80) so it reads as a brrap
rather than a hiss. Every other voice swells with it as well.

Both were scaling errors invisible by inspection, and both were found by
measuring rather than by listening harder.

## Panel

42HP. One side of the panel is the instrument as a whole and the other is its
six strings — mixing them put a global control at the head of a voice column,
where it read as belonging to that voice.

```
 ┌──────────────┐ │  ROOT    2     3     4     5    TRP
 │   ⟳ wheel    │ │   ○      ○     ○     ○     ○     ○     ← ON latch + LED
 │    + crank   │ │   ║      ║     ║     ║     ║     ║     ← octave / degree
 └──────────────┘ │  OCTAVE DEGREE ...                       = the voicing
  ROOT SCALE       │  ·▪     ▪▪    ▪▪    ▪▪    ▪▪    ▪▪    ← OCT   WAVE
  CRANK COUPS      │  ·▪     ▪▪    ▪▪    ▪▪    ▪▪    ▪▪    ← PHASE PRESS
  PRESS RIPPLE     │   ║      ║     ║     ║     ║     ║     ← LEVEL = the mix
  DOG   TEMPER     │  ○○     ○○    ○○    ○○    ○○    ○○    ← GATE  WAVE
  COUP in CLOCK in │  ·○     ○○    ○○    ○○    ○○    ○○    ← PHASE PRESS
  L R POLY WHL CP  │
```

Two rows of sliders draw the instrument: the voicing as one shape, the balance
as another. They are the SDK's `VCVSlider`, the same fader Sigma uses.

### The panel is the designer's file now

`res/wheel.svg` is a Figma export and carries every label as an outlined path,
so the widget places components and nothing else — no `sfs::PanelLabels`. Rack
ignores `<text>` but renders outlines, so runtime labels on top of it print every
one twice.

**Its guides are colour-coded by what they take**, which is worth recording
because it is how the layout gets read:

| guide | drawn | component |
|---|---|---|
| pink `#FFDFDF` | 8.89 mm circle | `PJ301MPort` (8.03) |
| blue `#C3E8FE` | 6.35 mm circle | `Trimpot` (6.05) |
| green `#C3FED5` | 6.35 mm circle | `VCVLightLatch` (6.10) |
| grey stroke | 7.62 × 26.67 mm | `VCVSlider` (6.72 × 25.92) |

The colour is scaffolding and **must be stripped before the panel ships** — the
guides are drawn slightly larger than the components that cover them, so what
survives otherwise is a coloured ring round every control that reads as a
deliberate touch. `figma_panel.py strip` does it, and `grid` reporting `0 guide
circles` is how you know.

Two things this export taught that the skill's own list does not:

**Normalize is not once, it is every time.** The header reverted to raw pixels
the moment the designer re-saved after correcting a label, and Rack then read the
module as 853 mm wide and screenshotted a blank strip. Re-run `normalize` after
every re-export, before anything else.

**The labels sit 7.7 mm ABOVE the control they name, and geometry alone will not
tell you that.** Each label row is 6.3 mm below the previous control and 7.7 mm
above the next; both readings are plausible. What settles it is counting — there
are exactly five label rows for five control rows, so reading them as "below"
leaves the first row labelling nothing and the last control unlabelled. That
mattered: read the other way, four of the ten per-voice positions come out wrong.

### Each voice is a two-segment column

Everything a voice owns lives in one block, **three** sub-columns wide at 1HP
spacing, with the latch on the centre one and everything else paired on the
outer two:

| row | left | right |
|---|---|---|
| latch | | (centre column) |
| faders | DEG *(OCT on the root)* | LEVEL |
| trim A | PHASE *(DECAY on the trompette)* | PRESS |
| trim B | OCTAVE | WAVE |
| jack A | DEG / OCT | LVL |
| jack B | PHASE / DECAY | PRESS |
| jack C | GATE | WAVE |

Voice 1 is locked to the root, so it has no PHASE (it is the reference every
other phase is measured from) and no OCTAVE trim (its fader IS the octave).

**Degree CV is 1V per DEGREE, not 1V/oct.** This is what came back in place of
the per-voice pitch CV that was dropped, and it is the better control for the
same reason the original was wrong: a degree offset stays in the key whatever the
key is. Moving a drone by a semitone is not something a hurdy-gurdy can do;
moving it to the next degree of the scale is. On voice 1, which is locked to the
root, the same jack steps octaves instead, matching what its own fader does.
ROOT and SCALE still move all six together, as a key change does.

ROOT and SCALE read out as `C` and `Harmonic minor`, not as `0.000` and `12.000`
— an index shown as a number tells the reader nothing.

### No screen

The wheel is drawn straight onto the faceplate — no dark slab behind it, and no
tinted panel behind the voice columns either. That inverts every colour decision
in the display, because the plugin's screen palette is built to glow out of a
`#1a1a32` ground and simply disappears on `#f0f0f0`: the disc is line art rather
than a fill (a filled disc on a light ground is the slab back again in a rounder
shape), and the drawing moved from `drawLayer(1)` to `draw()`, since the light
layer is the emissive pass and ink on a faceplate should not glow when the room
brightness is turned down.

One trap this created, and it fired immediately: `res/wheel.svg` is BOTH the
designer's guide and the panel Rack renders, and `panel_reticules.py` fills every
display it detects with the screen blue — so the first regeneration after the
background was removed silently put the slab back. The tool now takes a
`NO_SCREEN_SLAB` set for modules that draw onto the faceplate.

### The wheel's plane is perpendicular to the strings

Got wrong first, badly enough to be worth writing down, because the wrong version
was drawn twice and looked plausible both times.

**Bowing needs the contact surface to move ACROSS the string.** The rim's
velocity lies in the wheel's plane. So if that plane contained the string
direction, the rim would be rubbing *along* the string's length — not bowing, and
it would not speak at all. The wheel's plane has to be perpendicular to the
strings, and its axle therefore **parallel** to them: the axle IS the crank
shaft, running up the instrument's long axis to the handle at the tail. The first
render had the wheel turning in the strings' own plane. A hamster wheel.

    X = along the instrument = the strings = the AXLE
        (+X is toward the tail, nearest the camera, on the LEFT of the picture)
    Y = up, with the soundboard at YSB
    Z = across the instrument = how the strings are spread

### Only the top of the wheel is drawn, because only the top is visible

The wheel stands in a slot in the soundboard and emerges through it; the rest is
inside the body. Drawing the whole disc spent most of the picture's height on a
part nobody can see and squeezed the strings into what was left. Showing the
visible 117° arc instead is both what the instrument looks like and what buys the
room — the wheel is drawn 26 × 17mm now, against 13 × 22mm for the whole circle,
and the strings gained the difference.

It is a **solid body**, not a cage: the tread filled as a band between the two
rims, the near face filled over it, and the near lip of the slot stroked back on
top so the wheel is coming out of the soundboard rather than sitting on it.
Rotation shows as radial marks on the face and the seam across the tread, each
clipped where it meets the soundboard, so they rise out of the slot and sink back
into it. A wireframe was the wrong instinct twice over — it read as a cage, and
with the far half faded it read as a spiky mass.

### The strings

**Parallel, and not evenly spaced.** No fan is needed: the perspective already
spreads the near end and converges the far one, which is what the eye expects
looking up the instrument from the tail. Measured on the shipped camera, the left
(tail) end is **1.83× more separated** than the right (pegbox) end.

And the spacing across them is not an even comb — the outer strings sit further
apart than the inner ones, as they do on the instrument, where the melody strings
run close together down the middle and the drones sit out at the edges. The outer
gap measures **3.26× the inner** one. An even comb is the single thing that
immediately looks synthetic.

Each rests ON the tread, which is curved, so the outer strings sit a shade lower.

**On is dark, off is light grey — and the string does not move.** Lifting it was
the obvious idea and the wrong one twice over. First a lifted string faded to
nothing (0.42 alpha times the depth and edge fades reached about 0.15 in places,
which says "gone" when it means "off the wheel"). Then, with that fixed, the lift
itself turned out to be unreadable: in a perspective view a vertical offset is the
one displacement the projection is worst at showing, because it competes with the
depth the whole picture is built on, so a raised string just looked like a string
somewhere else. Weight is one channel, unambiguous, and it survives any camera
angle.

**On is dark**, and the contact point warms toward orange with that string's
press and its own phase of the ripple — the one place the wheel is driving it.

**A string runs off the picture rather than stopping**, fading at both ends: it
carries on to the tailpiece one way and the pegbox the other. Both ends have to
LEAVE the frame or the fade never finishes, and the fade is by SCREEN position: a
world-space fade is symmetric in world space and is not on screen, because the
perspective throws one end off the edge at full strength.

**Only the trompette is named.** Six labels will not fit at this spacing, and one
anchor is enough to read the order off. It also flashes with the buzz.

### The stroke strip is the coup mechanism, drawn

The coups genuinely are positions on the wheel, and on a turning 3D wheel that is
where they belong — but marks that turn with the wheel are moving click targets.
One turn, unwrapped into a strip along the foot with a playhead running across
it, says the same thing and holds still.

What it used to be was a row of identical ticks, and that is why the mechanism
read as arbitrary: **two separate things take strokes away, and the strip showed
neither of them.** So the strip is now the comparison itself.

- A turn is divided into COUPS slots, one bar each, left to right.
- **The bar's HEIGHT is that stroke's strength** — its position in the bar. The
  downbeat is hardest, the half-turn next, the quarters next, the rest weakest
  (`gcd(k, n)`, the metric hierarchy). This was always in the DSP and was
  invisible, which made DOG look like it was choosing at random.
- **The dashed line across is DOG.** A stroke buzzes if its bar clears the line.
  Raising DOG walks the line up through the bars and takes the weak strokes out
  one tier at a time.
- **Cranking faster grows every bar**, which is the technique made visible: a
  hard crank makes the dog speak more readily, and you can watch bars rise
  through the line as you crank.
- **The whisker on each bar is the stroke-to-stroke jitter.** Where a whisker
  straddles the line, that stroke fires some turns and not others — which is what
  you hear, and it now has somewhere to be seen.
- **Clicking a bar mutes that slot.** Muted draws as a stub at the baseline: the
  slot is still there, it just takes no stroke.

The two layers are finally distinguishable: the strip is the pattern, DOG thins
whatever pattern is left. At COUPS=4 on a normal crank the bars stand at
4.3 / 2.7 / 3.5 / 2.7mm and the DOG line sweeps 2.0mm to 5.0mm, walking it from
under all four to over all four.

### Reading the viewpoint off a drawing

Two separate things had to be right before the picture stopped looking like it
was seen from underneath, and neither was the transform — that was verified
correct throughout.

**A circle tells you the angle you are looking at it from ONLY by how much it is
squashed.** At 48° yaw the wheel came out a 0.85-aspect ellipse, so nearly
circular that it carried no viewpoint information at all and read as seen from
below as readily as from above.

**And depth has to actually be drawn.** The rim was stroked as one flat-coloured
path, and the depth ramp was clamped at full ink across the whole wheel, so
nothing said which side was nearer. When that was fixed the first attempt varied
the stroke WIDTH per segment as well as the alpha, which broke the circle into a
row of disconnected lumps and made the wheel a spiky mass. Shading is allowed to
say which side is nearer and nothing else.

Both are moot now that the wheel is a filled solid seen as an arc in a slot,
which carries its own form — but they are why two versions read wrongly.

### Dark grey and orange

The palette is the panel's own greys with orange for the buzz — no blue. On a
faceplate the blue was doing the job it does on a screen, where it is the
"lit" colour against a dark ground; on light grey it just read as another
neutral. One accent, used for one thing (a stroke that fired), is easier to read
across a six-voice instrument than a second hue competing with it.

### The ring is static; one hand turns

Two faults in the first display, and they compounded: nothing on screen visibly
owned the rotation (the crank knob turned inside the disc, small and easy to
miss), and the coup ring highlighted whichever slot the wheel was *passing*. So
the marks looked as though they were sliding round the circle, and the ring was
also claiming a buzz on slots that never fired — an armed stroke under the DOG
threshold lit up exactly like one that lifted the foot.

Now the hand reaches the rim, so one object owns the motion and visibly arrives
at each slot as it comes round; and a slot lights only when it actually **buzzed**,
fading over 140 ms. An armed slot the foot did not lift for stays blue as the
hand passes it, which is the honest picture of what DOG just did.

**The buzz is not subject to the ripple.** The coup slots and the seam are both
fixed to the wheel, so a given stroke always lands at the same point of the
ripple: measured at RIPPLE=1, stroke 3 of four sits at −13 dB *every turn*. That
is not a musical variation, it is one stroke of the pattern permanently missing —
and it is not physical either, since the rattle is the bridge foot against the
soundboard, driven by the crank stroke rather than by how well the wheel happens
to be gripping at that instant. The trompette's ripple factor lifts toward unity
with `buzzEnv`. `tools/wheel-dog-harness.py 1 <coups>` prints the per-slot ripple
depth alongside the accent ladder.

**The click was a whole rattle period late.** `clickEnv` was only set where the
rattle phase wrapped, so the foot's strike on the soundboard arrived up to 29 ms
after the stroke landed and every coup began with a soft edge instead of a
strike. That is most of what made the timing feel unreliable rather than merely
varied. It fires with the stroke now.

**And DOG defaults to 0, meaning "play the ring exactly".** The threshold floor
used to sit just above the weakest stroke the accent ladder can produce, so at
the default the weak strokes dropped out at random under the ±9% jitter: the
pattern on the ring was not the pattern you heard. The floor is now below the
worst case (0.62 × 0.87 × 0.91 = 0.49 against a floor of 0.44), so every armed
slot fires every turn at every crank speed — measured — and DOG thins by accent
from there. The ring is the pattern; DOG is what takes strokes away from it.

**And the coup slots are radial ticks, not dots.** As dots they put two rings of
near-identical blue circles round the same wheel — the six strings just outside,
the coups just inside — with nothing to say which ring was which. A tick reads as
a mark ON the wheel; a circle reads as an object sitting beside it, which is what
a string is.

The display is a real spinning disc — rotation, the seam mark passing, six
string contact points around the rim brightening with their own ripple phase and
going dark when lifted, the coup slots as marks on the rim (click to toggle),
and a flash across the whole wheel on a stroke. It is the one display in this
plugin where the animation *is* the state, so `drawPreview()` should show the
wheel stopped with a plausible voicing lit rather than an empty disc.

`WHEEL` outputs the ripple as CV and `COUP` outputs a trigger per stroke, so the
rest of the patch can be bowed by the same wheel.

## The seam sits at 0.38, and that is measured too

`WH_SEAM` is a named constant because the DSP and the display both draw it and a
number written twice is a number that drifts. Its VALUE was measured: at 0.72 it
landed squarely in the eccentricity wave's own trough and only made it deeper,
so a revolution had one big dropout in it and nothing else. Moved onto the
rising side it is a distinct notch, and the turn has two features. The ripple is
also DC-trimmed by +0.035, because the seam is a one-sided event and without the
trim raising RIPPLE quietly turned the whole instrument down — a level change
wearing a modulation's clothes. `tools/wheel-dog-harness.py 2` prints the mean
for exactly that reason.

## Open

- The body resonator is one fixed voice. The instrument has a real range of
  shapes (lute-back vs flat-back) and a switchable second body may be worth it.
- Strings are panned to fixed positions with a global width. Per-voice pan would
  need a fifth trim and there is no room for one.
- The panel is placed from code with runtime `PanelLabels`; it has not been
  through the `figma-panel` route yet.
