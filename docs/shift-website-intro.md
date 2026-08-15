# Introducing Shift

A 4-output CV shift register. One signal in, four related signals out, each with its own delay, divider, and behavior.

---

## What it does

You feed Shift a CV (anything from a slow LFO to a melody from a sequencer) and it produces four outputs that are versions of that signal, each routed through its own little delay/cascade pipeline. The four outputs can be set to follow the input directly with their own offsets (parallel mode), or to chain together so each lane reads from the previous one (cascade mode). With cascade chains, content trickles down the line, evolving as it goes.

Per lane you get:

- **N** (1–16): how many lane-clocks back to read from
- **DIV** (÷1, 2, 3, 4, 8): how many input clocks pass between lane ticks
- **Mode**: parallel (delay line on input) or cascade (FIFO from previous lane)
- **Step CV**: per-lane modulation of N
- **CV** + **Gate** outputs

Plus a **Jumble** pair that picks a random one of the four lane values on every input clock and emits it as both a CV and an accompanying clock, a stuttery, glitchy bonus output for free motion.

## How to think about it

There are three useful framings, depending on what you're patching:

**As a delay line.** Set every lane to parallel. With N=4, output A is the input from 4 clocks ago. Bump A's N to 1, B to 2, C to 4, D to 8, and you get four taps spread across the recent past. Feed them to four oscillators tuned a fifth apart and you've got a sliding-counterpoint pad from a single melody source.

**As a shift register.** Set lanes B/C/D to cascade and they form a chain: B reads from A, C from B, D from C. The CV in the buffer doesn't get re-sampled at each stage; it gets *passed*. Send a slow stream of values into the input and watch them propagate across the four outputs, each one a deeper memory of where the input was a moment ago.

**As a clock-driven generator.** Patch your input CV but don't bother feeding it anything interesting; *unplug it entirely*. With CV disconnected, each lane keeps cycling through its 16-slot history ring at clock rate, replaying the most recent material it captured before the cable came out. Plug it back in to refresh the buffers; pull it out again to lock the contents and just play. It's a tactile way to capture a moment and let it loop.

The clock divider is the lever that keeps these from feeling samey. Set lane A to ÷1 and lane B to ÷4 and they'll move at radically different rates from the same input clock, even if their N values match.

## A few patches

**Cascading harmonies.** Patch a slow CV (an LFO, a wandering V/Oct, a Drift channel) into CV. Send CLOCK to whatever rate you want the chain to advance at. Set all four lanes to cascade, all N=2. Patch each lane's CV out to its own quantizer + oscillator. As the input drifts, each oscillator's pitch trails behind, and the four together form a slowly-rotating harmonic cloud.

**Polyrhythmic gates.** Set divider to ÷1, ÷2, ÷3, ÷4 across the four lanes. Patch the four GATE outs to four drum modules. You've got an instant 1:2:3:4 polyrhythm whose contents respond to whatever's at CV in.

**Frozen tape loop.** Connect input CV (a melody, vocal sample, anything) and let Shift accumulate. Then disconnect the cable. The lanes keep cycling through their history rings forever. Pull on the divider knobs to slow the playback, change a lane to cascade to read from another lane's loop instead of the captured input. Patch back in to refresh, out to lock again. It's a very analog feel for what is effectively four simultaneous tape loops at different speeds.

**Glitch / fill.** Send the JUMBLE CV to a sample-playback module's start position, and JUMBLE CLK to its trigger. On every input clock, jumble re-rolls and triggers a new playback at one of the four lane values' positions: instant chopped-up texture from whatever's in the buffer.

**Slow generative bass.** Patch a single random voltage source (Drift, Marbles, anything) to CV. Lane A: parallel, N=1, DIV=÷4. Lane D: cascade, N=4, DIV=÷8. Lane A gives you a clean S&H every 4 clocks; D gives you a deeper, rarer evolution of that same stream. Quantize one for bass, the other for a slow countermelody, and walk away.

## At a glance

| | |
|---|---|
| HP | 16 |
| Inputs | CV, CLOCK, N CV, RESET, Step CV ×4 |
| Outputs | CV ×4, Gate ×4, Jumble CV + Clock |
| Mode | Parallel (delay line) / Cascade (FIFO) per lane |
| Divider | ÷1, ÷2, ÷3, ÷4, ÷8 per lane |
| N range | 1–16 per lane |
| History ring | 16 slots per lane (used for disconnect playback) |
| Right-click | Clear all |

Shift ships in v2.10 of the Signal Function Set plugin for VCV Rack.
