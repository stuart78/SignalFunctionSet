# Wheel Manual

## Overview

Wheel is a drone instrument after the hurdy-gurdy.

One rosined wheel bows every string at once. That is the whole idea, and
everything in the module follows from it: the wheel is not perfectly round, its
rosin is uneven, and it has a seam where it was joined, so every revolution
imposes the same ripple of level and brightness on all six voices together. Six
oscillators with six LFOs sound like an organ. Six sharing one slightly
irregular modulator sound like one object being played by one person.

The other half of the instrument is the dog. The trompette string crosses a
bridge with one loose foot, and when the player accelerates the crank the foot
lifts and rattles. That buzz is the rhythm section, and it is the difference
between a hurdy-gurdy and a bagpipe.

Wheel is 42HP. Five drone voices, of which the first is locked to the root, plus
the trompette.

## The wheel

**CRANK** sets the speed in revolutions per second, and that is the tempo of
everything: the ripple happens once per turn, and the strokes are counted per
turn. Patch **CLOCK** and one pulse locks one revolution, so the whole instrument
runs in time with the patch.

**RIPPLE** sets how deeply the wheel's irregularity reaches the sound. It is a
fixed periodic profile rather than noise, which is the point of it: eccentricity
as the first harmonic, uneven rosin above that, and a narrow dip where the wheel
was joined, in the same place every revolution. It moves level, brightness and
rosin noise together, because one wheel is doing all three.

**PRESS** is how hard the wheel bears on the strings overall. It changes timbre
before it changes level, which is what bow pressure does.

## Coups par tour

Hurdy-gurdy rhythm is counted in strokes per turn of the wheel. Four coups par
tour is four wrist accents per revolution, and the pattern is a subdivision of
the rotation rather than a free rhythm.

**COUPS** sets how many slots the turn is divided into, from one to eight. The
strip along the foot of the display is that turn unwrapped, one bar per slot,
with a playhead running across it.

**The strip is the mechanism, drawn.** Two separate things take strokes away, and
both are visible:

- **The bar's height is that stroke's strength**, which comes from its position
  in the bar. The downbeat is hardest, the half turn next, the quarters next,
  the rest weakest.
- **The dashed line across is DOG.** A stroke buzzes if its bar clears the line.
  Raising DOG walks the line up through the bars and removes the weak strokes a
  tier at a time, so it sweeps from every stroke, to the accented ones, to the
  downbeat alone.
- **Cranking faster grows every bar.** A hard crank makes the dog speak more
  readily, which is the technique, and you can watch bars rise through the line
  as you crank.
- **The whisker on each bar is the stroke to stroke jitter.** Where a whisker
  straddles the line, that stroke fires some turns and not others.
- **Click a bar to mute that slot.** It draws as a stub: the slot is still
  there, it just takes no stroke.

So the strip is the pattern, and DOG thins whatever pattern is left. DOG
defaults to 0, meaning play the ring exactly.

**COUP** in fires a stroke directly, at the hardest strength available, so it
clears any setting of DOG. **COUP** out sends a trigger every time the dog
actually buzzes.

A coup does not only buzz. The stroke lifts the wheel's speed briefly, so all
five drones swell and brighten with it. That common mode swell is half of why
the accent lands.

## The six voices

Each voice is a block three sub-columns wide, with its button on the centre one
and everything else paired on the outer two.

| | |
|---|---|
| **DEG** (**OCT** on the root) | **LEVEL** |
| **PHASE** (**DECAY** on the trompette) | **PRESS** |
| **OCTAVE** | **WAVE** |

**DEG** is a scale degree, so the voicing stays in the key whatever the key is.
The root voice is locked to the root, so its fader is an octave instead and it
has no OCTAVE trim and no PHASE.

**WAVE** morphs sine, triangle, sawtooth, square. The sawtooth is the anchor:
Helmholtz motion on a bowed string is a sawtooth, and morphing toward square
reads as more bow pressure.

**PRESS** is how hard that one string sits on the wheel, which is the cotton
wrapping on a real instrument. It changes timbre before level: a string barely
touching the wheel is duller, slips, and gives more rosin than tone.

**The button puts the string on the wheel**, and it is the normalled value of
that voice's GATE input. Unpatched, the button decides and defaults to on.
Patched, the gate decides, high is on. Either way the string swells onto the
wheel rather than switching, because the wheel takes a moment to grip.

## Tuning

**ROOT** and **SCALE** move all six voices together, as a key change does. SCALE
reads the plugin's polyphonic scale bus, so a Scala file or a non octave scale
set on Key arrives intact.

**TEMPER** snaps every voice to the nearest just ratio against the root, which
is how a hurdy-gurdy is tuned by ear.

**It will look as though it does nothing on a traditional drone voicing, and
that is correct.** Equal temperament is exact at the octave and within 2 cents
at the fifth, so on a setup of roots, octaves and fifths there is almost nothing
for TEMPER to move. It bites on thirds and sixths, by 14 to 16 cents, and on
sevenths by 12. The readout says what TEMPER is doing rather than what it is set
to: `JUST 14c` when there is something to move, `JUST 0c` when there is not,
`EQUAL` when it is off, and `SCALE` when the key is not twelve tone equal
temperament and the control is inert.

Just intonation is also what makes **PHASE** work. A twelve tone equal fifth is
1.4983 rather than 1.5, so the relative phase of two voices rotates through a
full cycle about once a second and a static phase offset is inaudible. Snapped
to 3/2 it holds still and becomes a real timbral control.

## The display

The display is a perspective render of the instrument's own geometry, drawn on
the faceplate.

The wheel's plane is perpendicular to the strings and its axle is parallel to
them, which is the crank shaft running up the instrument's long axis. That is
the only orientation that makes a sound: bowing needs the contact surface to
move across the string, and the rim's velocity lies in the wheel's plane.

Only the top of the wheel is drawn, because only the top is visible. It stands
in a slot in the soundboard and emerges through it.

- **A dark string is on the wheel, a light grey one is off.** The string does not
  move; only its weight changes.
- **The dot where a string crosses the wheel is the contact point**, and it warms
  toward orange with that string's pressure and its own phase of the ripple. It
  is the one place the wheel is driving the string.
- **The strings run off the picture** rather than stopping, because they carry on
  to the tailpiece one way and the pegbox the other.
- Click a string to take it off the wheel or put it back.

The readout along the top gives the key, the crank speed in revolutions per
second, and what TEMPER is doing.

## Controls

**Global:** Root, Scale, Crank, Coups, Press, Ripple, Dog, Temper.
**Per voice:** Degree or Octave, Level, Phase or Decay, Press, Octave, Wave, On.

**Inputs:** Root, Scale, Crank, Coups, Press, Ripple, Dog, Temper, Clock, Coup,
and per voice Degree, Level, Phase or Decay, Press, Gate, Wave.

**Outputs:** L, R, Poly (six channels, six is the trompette), Wheel (the ripple
as CV), Coup (a trigger per buzz).

## Context menu

- **Gate presses the string.** On by default. Off makes the gate a plain VCA.
- **Gate press time**, 8 ms to 1.2 s. How long the wheel takes to grip.
- **Wheel:** spread, detune, rosin, body, stereo width.
- **All strings on the wheel**, and **every stroke of the turn**.

## Patch ideas

**A traditional drone.** Root, fifth, root, octave. Leave TEMPER up, RIPPLE
around 0.4, COUPS at 4, DOG at 0. Crank slowly and it breathes; crank harder and
the dog comes in on its own.

**Locked to the patch.** Meter's BAR out into CLOCK gives one revolution per bar,
so the coups become a bar subdivision and the ripple lands on the downbeat.

**Played from a sequencer.** Six gates into the six GATE inputs, with a slow
gate press time, gives strings that swell on and off the wheel rather than
switching.

**The dog alone.** Take the strings off the wheel except the trompette, patch a
rhythm into COUP, and the module is a buzzing bridge.
