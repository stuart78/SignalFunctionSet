# Gravity Manual

## Overview

Gravity is a multi-mode chaos and motion engine. A single moving point, driven by one of six very different physical or generative systems, is read out as a rich set of control voltages: bipolar X/Y position, radius, angle, six "sector" distance CVs, and six boundary-ray gates. One small set of controls (Speed, Chaos, and a mode-dependent Gravity) reshapes whatever engine is running, and the large circular display shows exactly what the voltages are doing.

Gravity is 30HP.

## The Six Modes

Set the mode with the **MODE** knob (or its CV input, where 0–10V spans all six modes evenly).

1. **Pendulum**: a double pendulum integrated with RK4. The lower bob is the tracked point. Chaos sets the kinetic energy (gentle swing → full chaotic tumble). Drag either joint on the display to relaunch the motion (ragdoll).
2. **Gravity Well**: a spring-bound "rocket" orbits a central sun while being perturbed by several planets (inner ones orbit faster, outer ones are heavier and yank harder on each flyby). Chaos sets the planet count and launch energy. The orbit is always bounded: it can't escape or get captured.
3. **Billiards**: elastic balls bounce inside the circle. The cue ball (ball 0) is the tracked point; any ball crossing a ray fires that gate. Chaos sets the number of extra balls. Drag the cue to aim a slingshot launch (pull back, release).
4. **Hungry Man**: a Pac-Man-style maze. A random single-width polar maze is carved each level; Hungry Man chases the nearest big dot, eating small dots along the way. Chaos sets the number of big dots; Gravity biases new dots toward the center or the edge. Eat everything to advance a level (with a "LEVEL N" flash and a fresh maze). Score and level show in the center hub.
5. **Turtle**: a LOGO-style turtle executes random drawing commands, building generative artwork. Speed is the pen speed, Chaos is how often it picks a new command, and Gravity biases toward common moves (forward, gentle turns, arcs) vs. esoteric ones (spirals, set-heading jumps, pen-up hops, home). The current instruction list is shown top-left, and the drawing persists for a long time.
6. **Pattern**: a turtle that traces spirograph / Maurer-rose figures. Every figure is a rose `r = sin((p/q)·θ)` walked at integer-degree steps, so it always closes into a symmetric picture. Gravity changes the *form* (low = simple few-petal roses, mid = woven multi-loop rose-stars, high = dense webs); Chaos changes the *intricacy* (smooth curve → star polygon → Maurer web, plus lobed/fractal petals at the top). When a figure completes it instantly clears and draws the next.

## Controls

| Control | Range | Function |
|---------|-------|----------|
| **MODE** | 0–5 (snap) | Selects the active engine (see above) |
| **SPEED** | n/a | Time scale / draw speed of the active engine |
| **CHAOS** | 0–1 | Complexity: energy (Pendulum/GW), ball count (Billiards), big-dot count (Hungry Man), command rate (Turtle), figure intricacy (Pattern) |
| **GRAVITY** | -1 to +1 | Mode-dependent. Pendulum/Billiards: direction of the pull. Gravity Well: strength of the central sun. Hungry Man: center/edge dot bias. Turtle: common-vs-esoteric command bias. Pattern: the figure's form/symmetry |

Each of the four knobs has a matching CV input in the left-hand column.

## Inputs

| Input | Function |
|-------|----------|
| **SPEED CV** | Modulates speed (≈1 oct / 10V exponential) |
| **CHAOS CV** | Adds to the chaos amount (±10V → ±1) |
| **GRAVITY CV** | Adds to gravity (direction / magnitude depending on mode) |
| **MODE CV** | Selects mode; 0–10V spans all six |

## Outputs

The mandala of jacks around the display carries the spatial readout of the tracked point. The "0°" ray points straight down; rays and wedges are numbered clockwise.

| Output | Range | Function |
|--------|-------|----------|
| **X** | ±5V | Horizontal position of the tracked point |
| **Y** | ±5V | Vertical position of the tracked point |
| **RADIUS** | ±5V | Distance from center (−5 = center, +5 = rim) |
| **ANGLE** | ±5V | Angle of the point relative to the gravity direction (±5V = ±180°) |
| **SECTOR 1–6** | 0–10V | Six distance CVs, one per 60° wedge. Each is the point's distance from center, morph-crossfaded by how close the point's angle is to that wedge's center. Only the wedge(s) the point is near are active |
| **GATE 1–6** | 0/10V | Retriggerable gates that fire as a tracked/gate point sweeps across each of the six boundary rays. A red LED beside each jack shows activity (sector LEDs scale with signal strength; gate LEDs go full-on while high) |

## Display Interactions

- **Pendulum**: drag either joint (elbow or tip) to throw the pendulum.
- **Billiards**: drag the cue ball to aim and release a slingshot shot.
- The display always reflects the live voltages: the sector wedges brighten with their CV level and the boundary rays flash orange as gates fire.

## Context Menu

- **Relaunch (kick)**: re-energize the current engine (in Pendulum, throws it back into chaos).
- **Clear drawing**: (Turtle / Pattern) wipe the canvas and start fresh.
- **Gate hold**: Tight (30ms) / Medium (60ms) / Gluey (120ms): how long ray gates stay high.
- **Trail length**: Off / Short / Medium / Long: the fading motion trail in Pendulum / Gravity Well / Billiards.

## Patch Ideas

**Chaotic stereo modulation**: Pendulum mode, patch X and Y to a stereo VCA or panner. Raise Chaos for wilder motion; lower Speed for slow evolving drift.

**Generative melody + rhythm**: Pattern or Turtle mode. Send RADIUS or ANGLE through a quantizer for pitch, and use one of the GATE outputs as the note trigger, and the drawing literally plays itself.

**Polyrhythmic gates**: Any mode, take several GATE outputs at once. As the point loops around the field it crosses the six rays at irregular intervals, producing evolving polyrhythms. Use Gate hold = Gluey for overlapping gates.

**Orbital LFO**: Gravity Well mode with low Speed and low Chaos gives a smooth, slightly-perturbed elliptical X/Y: an organic two-axis LFO that never quite repeats.

**Game as a sequencer**: Hungry Man mode. The maze-walker's RADIUS and sector CVs jump around as he navigates; clock a sample-and-hold from a GATE output to grab stepped values.

**Living wallpaper**: Pattern mode on its own: set Gravity to the middle for woven rose-stars and let it cycle through figures. It's a visual centerpiece that also outputs six sector CVs you can tap for slow modulation.
