# Kit Manual

## Overview

Kit is a struck membrane, modelled mode by mode. A drum head is a 2D wave
equation on a disc, and its solutions are the Bessel modes. Rather than run a
mesh over the disc, Kit runs one resonator per mode, which is cheaper and puts
every parameter you would want to play in plain sight: strike position becomes a
per-mode gain, and muffling becomes the same calculation used subtractively.

One instrument covers tom, timpani, kick, snare, gong, steel pan, frame drum and
tabla, because those differ in shell depth, head stiffness and air cavity rather
than in kind.

Kit is 22HP.

## The one thing to know about the parameters

For an ideal membrane, `f = (c / 2πR) · j_mn` with `c = sqrt(T/σ)`. Radius and
tension scale every mode by the same factor and leave the *ratios* untouched.
Acoustically, SIZE and TENSION are therefore the same control, and shipping both
as separate knobs would be shipping a duplicate.

What breaks that scale invariance is bending stiffness, the air cavity, and
damping that depends on size. That is why MATERIAL and AIR exist, and it is why
SIZE is allowed to exist alongside TENSION: a small tight drum and a large slack
one at the same pitch are different instruments, and those three controls are
where the difference lives.

## Controls

The panel is eight columns by four rows. Every control is a trimpot, on one
pitch, so sixteen of them read as one instrument rather than as four ideas
competing for the same panel.

### Row 1, what the drum is

| Control | Range | Notes |
|---|---|---|
| SIZE | 6 to 22 inches | The tooltip reads inches and centimetres, not a percentage. |
| TENSION | ±8.4 semitones | Head tension, read as a pitch offset. |
| MATERIAL | drum head to gong | Bending stiffness. Pushes the modes from a membrane's inharmonic ratios toward a plate's. |
| AIR | open shell to sealed kettle | Closes the bottom. Wound up, it pulls the modes into a kettledrum's harmonic series. |
| DECAY | 80 ms to 7 s | Scaled by SIZE as well, and the tooltip says so. |
| TONE | | How much faster the high modes die than the low ones. |
| EXCITER | soft felt to hard stick | Reads as a beater name and a contact time in milliseconds. |
| MUFFLE | 0 to 100% | A hand on the head. Subtracts the modes that are live at that spot. |

### Row 2, how it is played and dressed

| Control | Notes |
|---|---|
| COUPLE | How much the batter head drives the resonant head. |
| RESO | Resonant head tuning, as a ratio to the batter head. |
| BEND | Pitch drop after the hit, from the head being driven hard. |
| WEIGHT | Beater mass, 10 to 70 g. |
| WIRES | Snare wire amount. |
| TIGHT | Wire tightness, loose buzz to tight snap. |
| LEVEL | Output trim. |
| HIT | A momentary strike. A button, not a value. |

### Row 3, CV

One jack per row-1 control, in the same order, directly underneath. A cable
hangs under the thing it modulates, so the pairing needs no label.

## Inputs

Row 4 is the transport row: performance data in and out, nothing else.

| Input | Notes |
|---|---|
| GATE | Strikes on a rising edge. |
| V/OCT | 1V/oct. |
| VEL | Velocity, 0–10V. |
| X, Y | Strike position, ±5V, summed with the STRIKE X/Y parameters. |

## Outputs

| Output | Notes |
|---|---|
| HEAD | The membrane alone. |
| WIRES | The snare wires alone. |
| MIX | Both, at LEVEL. |

Taking HEAD and WIRES separately is how you put the wires through their own
compressor or reverb, which is what a close-miked snare gets in practice.

## The display

The head is drawn as it moves. Rings and spokes trace the modal displacement,
excited areas warm toward orange, and the strike mark sits where X and Y put it.
Click or drag on the head to strike it, and the position you click becomes the
strike position.

Three details are load-bearing rather than decorative:

- **The grid redistributes with TENSION.** A taut head pulls its rings out
  toward the rim; a slack one lets them gather in the middle. That is what
  tension does to a real head's response.
- **AIR closes the bottom.** At zero it is an open shell with a resonant head
  across it, which is a tom or a kick. Wound up it bellies into a sealed bowl,
  which is a kettledrum. A kettledrum is what the harmonic series AIR
  imposes belongs to.
- **The snare wires appear one at a time** as WIRES comes up, and they buzz
  while the drum sounds. They are drawn as straight lines rather than as coils.
  A coil is the more accurate picture, and at this size it read as a braid
  wrapped round the shell, so the accurate drawing was the wrong one.

A small spectrum in the corner answers the one question the head cannot: which
modes are actually sounding.

## Context menu

- **Head view**: Flat or 3D.
- **Instruments**: Tom, Floor tom, Timpani, Kick, Snare, Brush snare, Gong,
  Steel pan, Frame drum, Tabla. These are the instruments the engine was
  measured against while it was being built, so they are also the shortest route
  to hearing whether something has broken.

## Patch ideas

**A kit from one module.** Take three instances, load Kick, Snare and Floor tom,
and clock them from Beat. The three share nothing, so they detune and decay
independently the way a real kit does.

**Timpani that follow a line.** Load Timpani, patch V/OCT from Note or Chance,
and put a slow LFO on TENSION CV. The pedal glide comes from tension, not from
pitch, so it bends the way the instrument does.

**Playing the head.** Patch an LFO into X and another, slower, into Y. The
strike position walks around the head and the timbre moves with it, because
position is a per-mode gain rather than a filter.

**Gongs.** Load Gong, wind MATERIAL up, DECAY long, and strike hard with
velocity. MATERIAL is bending stiffness, so a high setting is genuinely a plate
rather than a membrane with a longer tail.

## Technical notes

The mode table is `j_mn`, the zeros of the Bessel functions, sorted by
frequency and computed offline by bisection. It reproduces the textbook series
1, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501 to three decimals.

The first attempt at generating it scanned upward from zero and "found" roots at
x = 0.00002, because `J_m(0) = 0` for m ≥ 1 and rounding noise flips the sign
down there. `J_m` has no zeros below x = m.

The DSP lives in `src/membrane.hpp` so it can be measured without Rack in the
way. That is how the mallet's 38 ms contact times and its 296-bounce chatter
were found.
