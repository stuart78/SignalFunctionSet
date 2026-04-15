# Intone Manual

## Overview

Intone is a formant synthesis voice based on the IRCAM CHANT project (Rodet, Potard, Barriere, 1984). It generates vocal-character sound using 5 parallel formant cells, each producing overlapping FOF (Formant Wave Function) grains — damped sinusoids at formant frequencies. A vowel morph slider smoothly interpolates through /a/, /e/, /i/, /o/, /u/ presets.

Intone is 16HP.

## Concepts

### Formant Synthesis

The human voice's distinctive character comes from resonances (formants) in the vocal tract. Different vowel sounds correspond to different formant frequency patterns. Intone models this by summing 5 parallel resonant cells, each generating sound at a formant frequency.

### FOF (Formant Wave Functions)

Each formant cell generates FOF grains — short bursts of a damped sinusoid at the formant's center frequency, with a cosine-windowed attack and exponential decay. Multiple FOFs overlap as they're re-triggered at the fundamental frequency rate, creating a continuous tone with formant resonance character.

### Vowel Morphing

The vowel morph slider sweeps through 5 vowel positions (/a/, /e/, /i/, /o/, /u/), linearly interpolating the first 3 formant frequencies between presets. The per-formant frequency knobs act as additive offsets on top of the vowel preset — so you can dial in "mostly /a/ but with F2 pushed higher."

### Three Excitation Modes

Intone's behavior changes based on what's patched to the EXC input:

**Default (nothing patched)**: Internal F0 oscillator drives FOF generation. V/Oct controls pitch. Works as a vocal-character VCO.

**Audio mode (audio patched, switch up)**: Input audio passes through a parallel resonant bandpass filter bank at the 5 formant frequencies. The audio is formant-shaped — drums gain vocal coloring, oscillators acquire vowel character. V/Oct transposes the formant pattern.

**Trigger mode (clock patched, switch down)**: External rising edges fire fresh FOF grains. Each trigger produces a vowel-character burst. V/Oct transposes the formant frequencies.

## Controls

### Per-Formant (x5)

| Control | Range | Default | Function |
|---------|-------|---------|----------|
| **Freq offset** | ±1 octave | 0 | Shifts formant frequency relative to vowel preset |
| **Bandwidth** | 30-500 Hz | 80 Hz | Width of the formant resonance |
| **Amplitude** | 0-1 | varies per formant | Level of this formant |

### Global

| Control | Range | Default | Function |
|---------|-------|---------|----------|
| **Vowel Morph** (slider) | 0-1 | 0 (/a/) | Sweeps through /a/ → /e/ → /i/ → /o/ → /u/ |
| **Skirt** | 0-1 | 0.5 | FOF attack rate / spectral slope |
| **Mode Switch** | Audio/Trigger | Audio | Selects excitation mode when EXC is patched |

## Inputs

| Input | Range | Function |
|-------|-------|----------|
| **V/Oct** | 1V/octave | Pitch (default) or formant transposition (audio/trigger) |
| **EXC** | ±5V | Excitation source |
| **F1-F5 Freq CV** | ±5V | Per-formant frequency modulation |
| **F1-F5 BW CV** | ±5V | Per-formant bandwidth modulation |
| **Vowel CV** | 0-10V | Vowel morph position |
| **Skirt CV** | ±5V | Skirt width modulation |

## Outputs

| Output | Function |
|--------|----------|
| **Out** | Monophonic audio output |

## Vowel Preset Frequencies (Hz)

| Vowel | F1 | F2 | F3 | F4 | F5 |
|-------|-----|------|------|------|------|
| /a/ | 730 | 1090 | 2440 | 3400 | 4400 |
| /e/ | 530 | 1840 | 2480 | 3400 | 4400 |
| /i/ | 270 | 2290 | 3010 | 3400 | 4400 |
| /o/ | 570 | 840 | 2410 | 3400 | 4400 |
| /u/ | 300 | 870 | 2240 | 3400 | 4400 |

## Spectrum Display

Shows 5 formant peaks as colored bell curves on a logarithmic frequency axis (50 Hz - 5 kHz), with a bright composite envelope overlay. Updates in real time as parameters change.

## Patch Ideas

**Vowel Pad**: Patch a slow LFO to Vowel CV. The oscillator morphs between vowel sounds continuously.

**Vocal Filter**: Patch audio to EXC (Audio mode). Drum loops, oscillators, or samples get formant-shaped through the 5 vowel resonances.

**Triggered Vowels**: Patch a clock to EXC (Trigger mode). Each pulse produces a short vocal burst. Modulate Vowel CV for changing articulation.

**Formant Sweep**: Patch an LFO to one of the F1-F5 Freq CV inputs. The formant peak sweeps independently of the vowel preset.

## Historical Context

Based on the FOF technique from the IRCAM CHANT project (1979-1984), originally developed for singing voice synthesis. The same technique was used by composers including Kaija Saariaho, Gerard Grisey, and Marco Stroppa for spectral music composition.
