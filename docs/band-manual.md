# Band Manual

## Overview

Band is a harmonic bandpass bank built for one specific, hard-to-do job: isolating the individual harmonics of a sound. Inspired by a technique Suzanne Ciani has described, starting with a low, harmonically-rich wave and using a very narrow bandpass to pull out one harmonic at a time. Band makes that effortless.

The trick is that **harmonics are linearly spaced** (f0, 2·f0, 3·f0 …), not exponential, so a normal 1V/oct filter makes them fiddly to land on. Band instead locks each of its four bands to an **integer harmonic** of a shared fundamental, so every band sits dead-on a partial, every time. It can find that fundamental automatically from the incoming audio, or track it via 1V/oct.

Band is 24HP.

## How It Works

- A shared **fundamental** `f0` is established (auto-detected from the audio, or set by TUNE + V/OCT).
- Each of the four bands (**A, B, C, D**) selects an integer **harmonic** *N*; its bandpass center is computed as `N × f0`, so it lands exactly on that partial.
- **WIDTH** sets bandwidth as a *fraction of f0*, giving constant absolute bandwidth: every harmonic isolates equally cleanly (the internal Q auto-scales with N).
- Each band has its own colour (A blue, B green, C orange, D purple) carried into the on-screen spectrum so you always know which control owns which harmonic.

## Controls

### Per band (×4: A, B, C, D)
| Control | Function |
|---------|----------|
| **Level** (top knob) | Output level of that band |
| **Harmonic** (middle knob) | Which integer harmonic this band isolates (snaps 1–32; 1 = the fundamental) |
| **Enable** (button + LED) | Switch the band in/out (fast anti-click fade) |

### Global (bottom row)
| Control | Function |
|---------|----------|
| **TUNE** | Manual fundamental, standard 1V/oct convention (0V = C4) |
| **WIDTH** | Bandpass bandwidth as a fraction of f0. Narrow = razor-thin single harmonic; wider = neighbours bleed in (controlled "impurity"); >1 = clusters |

## Inputs

| Input | Function |
|-------|----------|
| **IN** | Audio in: the rich source you're isolating harmonics from |
| **V/OCT** | Fundamental pitch, 1V/oct (matches a standard oscillator; used in manual mode) |
| **SHIFT** | Global continuous harmonic shift: slides *all* bands between harmonics at once (1V = 1 harmonic) for scanning/impurity |
| **W-CV** | Width modulation |
| **Per band: Level CV** | Modulates that band's level (10V → +full) |
| **Per band: Harmonic CV** | Continuously shifts that band's harmonic (1V = 1 harmonic) |
| **Per band: Enable gate** | Gate ≥1V forces the band on (overrides its button when patched) |

## Outputs

| Output | Function |
|--------|----------|
| **MIX** | Sum of all enabled bands (each × its level) |
| **POLY** | Each band on its own polyphonic channel (A=1, B=2, C=3, D=4), to spatialize or process harmonics separately |

## The Display

A spectrum analyzer of the incoming audio (FFT) drawn on a **harmonic axis** (frequency ÷ f0), so the spectral peaks line up with the harmonic gridlines. Each band is drawn as a coloured "bell" sitting on its harmonic, height = level, labelled with its letter and harmonic number (e.g. `A5` = band A on harmonic 5). The readout at the bottom-left shows the current `f0` in Hz, with **AUTO** shown when the pitch is being auto-detected.

## Auto-Follow vs. Manual

- **Auto-follow (default, context menu):** Band detects the source's fundamental directly from the audio (FFT autocorrelation) and locks the harmonic grid to it. Feed any rich wave and the bands snap onto its real harmonics with no tuning. As you play a melody into IN, the whole bank tracks. (V/OCT and TUNE are ignored while auto-follow is locked.)
- **Manual:** turn auto-follow off and set the fundamental with TUNE + V/OCT. Because Band uses the standard 1V/oct reference, patching the same pitch CV into both your oscillator and Band's V/OCT keeps the harmonics aligned exactly at every note, the most precise option, especially for high harmonics.

## Context Menu

- **Follow input pitch (auto-lock harmonics)**: toggle auto-detection (default on).
- **Filter slope**: 4-pole (steep, default) or 2-pole (gentler).

## Patch Ideas

**Ciani-style harmonic isolation:** feed a low saw or pulse (~30–110 Hz) into IN. Leave auto-follow on. Turn WIDTH right down, then set A/B/C/D to different harmonics, and each rings out a single partial. Bring WIDTH up slightly for a breathier, less pure tone.

**Additive re-voicing:** isolate harmonics 1, 2, 3, 5 across the four bands and ride their Levels (or modulate with LFOs/envelopes) to re-balance the timbre of any source in real time.

**Spatial harmonics:** take the POLY output into four VCAs/panners (or a poly VCA) and place each harmonic in its own position in the stereo field.

**Harmonic sequencing:** sequence the per-band Harmonic CVs (1V/harmonic) or the global SHIFT to step the bands up and down the harmonic series in time with your patch.

**Vocoder-ish movement:** modulate WIDTH and SHIFT slowly with LFOs while the source plays. The bank rakes across the partials for evolving, formant-like motion.
