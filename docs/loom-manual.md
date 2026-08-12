# Loom Manual

## Overview

Loom is eight strings sharing a bridge.

Each string is a digital waveguide: a delay loop that sets the pitch, a lowpass
inside the loop that makes the high partials die first, a four-section allpass
chain that gives the string stiffness, and a comb filter on the output tap for
where along the string you pick it. That is a real physical model, not a
sampled one, which is why it responds to how you play it rather than replaying
what it was given.

The display *is* the instrument. Strum it with the mouse, or drive the strings
from gates.

Loom is 28HP.

## EXCITE is an axis, not a menu

Four ways of starting a string vibrating (**HAMMER · PLUCK · BOW · WIND**) laid
out as points along one knob. You can sit anywhere between them.

At a pure node the code path is identical to a single-exciter implementation,
down to how many random numbers it draws, so hammer and pluck are bit-exact with
where they started. In between you get genuine blends: a bowed attack that
decays like a pluck, a hammer with wind noise behind it.

### Hammer and pluck

The two impulsive ones. Hammer is a broader, softer strike with more low
content; pluck is narrower and brighter. PICK sets hardness for both.

### Bow

A Smith friction model, and three things make it work at every pitch and
setting rather than only where it was tuned:

1. **The friction curve's input is normalised by the string's own envelope**, so
   its operating point does not drift as pitch, decay or damping change.
2. **A pressure regulator holds the string at a target amplitude.** An
   unregulated friction model settles anywhere between silent and pinned to the
   clamp, and can take six seconds to get there, and every measurement taken before
   about five seconds is measuring the build-up, not the tone.
3. **The regulator drives force, not bow speed.** The friction curve is
   non-monotonic in speed, so a controller pushing on speed sails straight past
   the peak.

Bow speed therefore sets **loudness**, not whether the string speaks. And the
hair noise is load-bearing: without it the string locks onto its sub-octave.

### Wind

An aeolian harp: the instrument you hang in a window and the wind plays.

Vortex shedding drives a *narrow* band, and the band **moves with the gust**
(the Strouhal relationship between wind speed and shedding frequency), so the
harp climbs and falls between partials instead of sitting on one. The
fundamental is about 26 dB down and the dominant partial roams from the third to
the eighth. A fixed band just sounds like a filtered bow; the movement is the
whole effect.

## Controls

| Control | What it does |
|---|---|
| **BODY** | Body resonance. |
| **COUPLE** | How much the strings hear each other through the bridge. See below. |
| **DECAY** | 0.15–20 s ring time. |
| **DAMP** | Treble loss in the loop. |
| **PICK** | Pick hardness. |
| **SPREAD** | Strum spread: negative strums up, positive strums down, and the magnitude is how long the strum takes. |
| **ROOT** | ±12 semitones. |
| **OCT** | −4 to +3 octaves. |
| **SCALE** | The canonical plugin scale list. |
| **PATTERN** | The auto-player's figure. See below. |
| **DENSITY** | How many of the auto-pattern's notes actually play. |
| **AUTO** | Runs the auto-player. |
| **RESET** | Back to the start of the pattern. |

### COUPLE is a real bridge

String *motion* feeds a shared bus, and each string takes back a band-limited
fraction of it: `x += k·(bus − ownLowpassed)`. Only the transmitted band is
damped, and the in-phase mode has gain exactly 1, so it is unconditionally
stable. It cannot run away however far you turn it.

It is deliberately capped at a low ceiling. Turning it up further cost 89% of
the sustain and bought *less* audible ring, because energy shared between eight
strings is energy leaving each of them.

### DAMP is relative, not absolute

DAMP is floored at ten times each string's own pitch. As an absolute cutoff it
filtered a high string below its second partial, and with so few partials left
the pick-position comb's null removed most of what was left, and a dark, high,
bowed string made almost no sound. Tying the floor to the string's own
fundamental fixes that.

## The display

Seven tabs.

**PLAY.** Strum the strings with the mouse. Y position is *where along the
string* you strike, so the top of the display is near the bridge (thin and
nasal) and the bottom is over the neck (round and hollow).

**TUNE · DECAY · STIFF · POS · EXCITE · LEVEL.** One attribute per string,
edited by dragging the height of each string's bar. This is where the instrument
gets set up: a bass string with a long decay and a soft pick, top strings short
and bright.

## Auto-player

Eighteen patterns. Twelve are shapes: up, down, up-down, down-up, converge,
diverge, thumb, pairs, skip 3, random, walk, strum. The other six are built from
a **wrapping 4-bit adder**: two chained operations, such as +3 then ×5, walk the
eight strings around a sixteen-step ring, producing figures that are neither
scalar nor random. Those six are shared with [Slide](slide-manual.md).

CLOCK advances the pattern; with nothing patched, the context menu's **Auto
rate** sets a free-running speed.

## Inputs

V/Oct (transposes the whole loom), GATE (strums every enabled string), VEL
(0–10V, polyphonic, channel N sets string N's velocity), CLOCK, RESET, **eight
per-string gates**, and CV for Body, Couple, Decay, Damp, Pick, Spread, Root,
Octave, Scale and Density.

## Outputs

MIX L, MIX R, and **eight per-string audio outputs**.

The mix is **soft-clipped**, linear to ±6 V and asymptotic to ±10. Eight bowed
strings have a crest factor no pluck comes near, and without the clip the mix
would spike well past what anything downstream expects.

## Context menu

| Item | What it does |
|---|---|
| **Tuning** | Preset tunings, with a quantize option. |
| **Mouse strum** | Hover over the strings, or click and drag only. |
| **Stereo width** | How far the eight strings spread across the image. |
| **Auto rate** | The free-running speed when no clock is patched. |
| **Hover plays when Rack is in the background** | Off by default. Hover events keep arriving when another application is in front, so an instrument that plays on hover would make a noise while you are reading a manual. |

## Patch ideas

**A wind harp you leave running.** EXCITE fully to WIND, DECAY long, COUPLE up,
and nothing patched at all. Modulate EXCITE slowly with an LFO and the wind
gusts.

**Bowed drone with a struck accent.** EXCITE around 60% toward BOW, then a
trigger into GATE occasionally: the strike rides on top of the bowed tone
because both exciters feed the same string.

**Eight-voice polyphony from one gate.** Poly VEL in, GATE in, and let the
velocity distribution across the sixteen channels decide the balance of the
strum.

**Per-string processing.** The eight individual outputs into eight filters or
delays, with COUPLE up so they still ring into each other before you split them.

**Prepared strings.** In the TUNE tab, tune four strings to a chord and the
other four to microtonal neighbours of the same notes. COUPLE turns the beating
between them into part of the instrument.
