# Operator Manual

## Overview

Operator is a 6-operator FM synth voice in the Yamaha DX7 lineage. It loads DX7 `.syx` cartridges (or your own banks exported from [Dexed](https://asb2m10.github.io/dexed/)), lets you pick a voice, and plays it polyphonically. The sound engine is Google's [music-synthesizer-for-android](https://github.com/google/music-synthesizer-for-android) (msfa) DX7 core, so patches sound faithful to the original hardware: all six operators, every algorithm, feedback, key scaling, and the DX7 envelopes.

Out of the box it ships with four classic Brian Eno DX7 patches (transcribed from his *Keyboard* magazine charts, Feb 1987), so it makes sound the moment you patch a gate in.

Operator is 10HP and polyphonic (up to 16 voices).

## Quick start

1. Patch a polyphonic **V/OCT** and **GATE** from a sequencer or MIDI-CV module.
2. Turn the **VOICE** knob (or use the on-screen ◀ ▶ arrows) to audition patches.
3. Take **AUDIO** out, which is the voice with its own DX7 envelope, ready to play.

To load your own sounds, right-click → **Load bank (.syx)…** and choose any DX7 32-voice cartridge.

## Controls

| Control | Function |
|---------|----------|
| **VOICE** | Selects the patch within the current bank (0–31; the default Eno bank exposes 4). |
| **BANK** | Selects between loaded banks. |
| **◀ / ▶** (on screen) | Previous / next voice. |
| **TUNE** | Coarse tune, ±12 semitones. |
| **BRIGHTNESS** | Tilts the whole timbre darker/brighter (−100%…+100%) by scaling modulator output, the DX7-style "one-knob" timbre control. |
| **FEEDBACK** | Offsets the patch's feedback amount (−7…+7). Adds or removes the buzz/noise the feedback operator contributes. |

## Inputs

| Input | Function |
|-------|----------|
| **V/OCT** | Pitch, 1V/oct, polyphonic. The channel count here sets the module's polyphony. |
| **GATE** | Note on/off, polyphonic. Rising edge triggers the envelope; works with short triggers as well as sustained gates. |
| **VEL** | Velocity, polyphonic (0–10V). Sampled at note-on and applied via the patch's velocity sensitivity. |
| **VOICE CV** | Voice select, 0–10V across the 32 voice slots. |
| **BANK CV** | Bank select, 1V per bank. |
| **TUNE CV** | 1V/oct (adds to TUNE / V/OCT). |
| **BRIGHTNESS CV** | ±5V over the full brightness range. |
| **FEEDBACK CV** | ±5V over the full feedback-offset range. |

## Outputs

| Output | Function |
|--------|----------|
| **AUDIO** | The voice **with its internal DX7 envelope** applied, a complete VCA-shaped note. Use this most of the time. Polyphonic. |
| **VCO** | The **raw tone with no internal envelope**, always sounding while a voice is held, so you can shape it with an external envelope/VCA (e.g. [Vac](vac-manual.md), [Swell](swell-manual.md), or [OP ENV](op-env-manual.md)). Polyphonic. |
| **ENV** | An **envelope follower** of the audio amplitude, 0–10V, polyphonic, handy for ducking, side-chaining, or driving other modulation from the note's loudness. |

## The display

The screen is a two-tab interface (tap the tabs at the top, Beat/Note style):

- **OPERATORS**: shows the six operators laid out for the patch's algorithm. Carriers (the operators you actually hear) are highlighted; modulators feed into them. **Click an operator to mute/un-mute it**, a quick way to hear what each one contributes or to thin a patch out. The algorithm number is shown too.
- **ENVELOPE**: draws the selected voice's carrier EG shape, with a live amplitude trace overlaid so you can see the envelope as it plays.

## Banks & voices

- **Default bank**: four Brian Eno DX7 patches. Because there are only four, the VOICE control is capped to those slots for this bank.
- **Load bank (.syx)…**: load any standard DX7 32-voice cartridge, or a bank exported from Dexed. Multiple banks can be loaded; switch with BANK / BANK CV.
- **Remove current bank**: drops the selected bank (available once more than one is loaded).
- Loaded banks and the current selection are saved with the patch.

## Notes on the engine

- **Faithful DX7 voicing**: packed VMEM is read exactly as the hardware stores it (6 ops × 17 bytes + global + name), so cartridges play as intended, including algorithm, feedback, oscillator sync, key level/rate scaling, and the per-operator 4-stage envelopes.
- **Sample-rate-corrected envelopes**: the DX7 EG rates are scaled to the host sample rate, so attack/decay times are correct at any sample rate.
- **Polyphony**: driven by the V/OCT channel count, up to 16 voices. Released notes free their voice once their envelope decays below threshold.

## Patch Ideas

**Classic FM keys:** sequence V/OCT + GATE polyphonically, take AUDIO, and audition voices with the VOICE knob. Add a touch of BRIGHTNESS movement with an LFO for an evolving timbre.

**External envelope shaping:** take **VCO** instead of AUDIO and run it through [Vac](vac-manual.md) or [Swell](swell-manual.md) for a release/attack curve the original patch never had. Turn a percussive DX7 patch into a pad, or vice versa.

**FM as a sound source for OP ENV:** pair Operator with [OP ENV](op-env-manual.md): play the same DX7 voice on both, use Operator's VCO for the tone and OP ENV for an authentic DX7 envelope you can offset and modulate independently.

**Spectral dissection:** on the OPERATORS tab, mute operators one at a time to hear how a patch is built, which is great for learning FM, and for stripping a busy patch down to a cleaner core.

**Velocity dynamics:** patch a velocity source to VEL; patches with velocity sensitivity will open up their brightness and level as you play harder, just like the hardware.
