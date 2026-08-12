# Crystal Manual

## Overview

Crystal is a 3D echo chamber shaped like an actual crystal.

Stereo in, **quad out**, and the four outputs are four listeners standing
inside the thing, hearing the same events from four different places. It is a
reverb in the sense that a cathedral is a reverb: the shape is doing the work,
and the shape is real.

Crystal is 28HP.

## The crystals are real crystals

Sixteen habits, built as intersections of half-spaces from symmetry-expanded
Miller-index normals, which is the way a mineralogist would describe them and
also the way you actually construct one:

- cube **{100}**
- octahedron **{111}**
- dodecahedron **{110}**
- the sphalerite tetrahedron
- pyrite's **{210}** pyritohedron
- quartz
- the low-symmetry systems

They are ordered simple to complex, so turning MATERIAL up walks you from a
six-faced box to something with dozens of faces at awkward angles. The last four
are **clusters**: several whole crystals intergrown, so a ray can be trapped
rattling in one point or wander between them.

## How the sound is made

**By letting rays loose.** Seven rays × 26 bounces per emitter, in a
deterministic fan. Every time a ray strikes a wall it sheds an echo toward each
of the four interior listeners, and that set of arrival times and levels **is**
the quad output.

Tracing runs on a worker thread and takes around 50 ms; the audio thread adopts
the new tap set with a crossfade, so changing the shape does not click.

Per sample, the audio path is a multitap delay reading those taps, six
recirculating "pockets" per emitter, and an FDN tail whose length is sized by
**Sabine's equation** on the crystal's own volume and surface area, so a bigger
habit genuinely does ring longer, for the same reason a bigger room does.

## CHAMBER and DELAY

The MODE switch, and it changes what kind of module this is.

**CHAMBER** keeps real acoustic timing. Arrivals land where the geometry says
they land, in milliseconds. This is the reverb.

**DELAY** keeps the geometry's arrival *ratios* but stretches them out to
musical times, with SIZE becoming the delay time. Same shape, same relationships
between the echoes, but now they are hundreds of milliseconds apart instead of
tens. This is a multitap delay whose tap pattern is a crystal.

## Controls

**The space**

| Control | Range | What it does |
|---|---|---|
| **SIZE** | 0.06–24 m | How big the crystal is. In DELAY mode, the delay time. |
| **DAMP** | 1–60% | Absorbed per bounce. |
| **MATERIAL** | 16 habits | The shape. Simple to complex, ending in the clusters. |
| **TAIL** | 0–100% | The FDN tail behind the discrete echoes. |
| **ECHOES** | 2–N | How many reflections are kept. Few is discrete and rhythmic; many is dense. |
| **MIX** | 0–100% | Dry/wet. |

**The emitters**

Two of them, so a stereo input enters at two different points in the crystal.

| Control | What it does |
|---|---|
| **Emitter A / B azimuth, elevation** | Where each one sits. |
| **Emitter axis X / Y / Z** | The axis the pair is placed along. They are two ends of one axis rather than two independent points, which keeps the stereo image coherent. |
| **HEAD A / B** | Each emitter's heading. |
| **NAVSPEED** | How fast the X/Y velocity CVs move them. |
| **FEEDBACK** | 0–92%. How much of what comes back re-enters the emitter. |

**The ping**

| Control | What it does |
|---|---|
| **PING** (button + input) | Excites the crystal with an internal striker. |
| **DECAY** | 0.05–4 s ring on the striker. |

The internal exciter is Chime's struck bar. With nothing patched and MIX fully
wet, Crystal is a bare resonator you can play by pinging.

**The view**

ROTY / ROTX (drag the display), SPIN X / Y / Z. **Rotation is camera-only.**
turning the display does not move the acoustics. The shape you hear is the shape
that is there.

## Inputs

AUDIO (enters at emitter A), **AUDIO B** (emitter B, patch stereo here), PING,
V/OCT (ping pitch), SIZE, DAMP, MATERIAL (1V per habit, simple → complex), TAIL,
MIX, ECHOES, FEEDBACK, and **X/Y velocity** for each emitter.

The emitter CVs are **velocity**, not position: they steer rather than jump. A
slow LFO on A's X moves the emitter smoothly through the crystal and the whole
reflection pattern changes with it, which is a kind of modulation nothing else
in the plugin does.

## Outputs

**QUAD A / B / C / D.** Four listeners inside the crystal.

For stereo, take A and B. For quad, take all four. They are genuinely different
signals with different arrival times and levels, not a stereo pair
duplicated, so a four-speaker setup gets real spatial information.

## Context menu

| Item | What it does |
|---|---|
| **Solid faces (occlusion)** | Draw the crystal solid so near faces hide far ones. Off draws it as a wireframe. |
| **Draw on the panel (no screen)** | Let the crystal render across the panel rather than inside a screen rectangle. |
| **Ping alternates A / B** | Successive pings enter at alternating emitters. |
| **Repeat pitch** | A real pitch shifter in the recirculating pocket loop, so repeats rise or fall. |

## Patch ideas

**A reverb with a shape.** Audio in, MIX around 40%, CHAMBER mode, and walk
MATERIAL up from cube to cluster while listening. The tail changes character
completely and none of it sounds like a plate.

**Rhythmic multitap.** DELAY mode, ECHOES low so the taps stay discrete, SIZE
set against your tempo. The echo pattern is the crystal's geometry, which is
irregular in a way no delay-time knob would give you.

**Play it as an instrument.** Nothing patched, MIX fully wet, PING from a
sequencer with V/OCT. You are striking the crystal itself.

**Moving source.** Slow LFOs into emitter A's X and Y velocity. The sound
wanders around inside the space and the four outputs pass it between them.

**Quad.** All four outputs to four speakers, TAIL up, one emitter in a cluster
habit. The clusters are where the four listeners hear genuinely different
things, because a ray trapped in one point never reaches the others.

**Feedback at the edge.** FEEDBACK around 80% with a short SIZE: the crystal
starts to sing at its own resonances rather than reverberating.
