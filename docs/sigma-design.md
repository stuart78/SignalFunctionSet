# Sigma: design

**Status: v1 built, hidden.**

Named `Sigma`, for the summation sign — the whole operation is a sum of sines,
with no waveform generated and nothing filtered away. It was called `Prism`
until 2026-08, which was the wrong metaphor twice over: a prism *splits* a beam
into a spectrum, where this *assembles* one, and there is already a plugin brand
called Prism in the VCV Library (Rainbow, Droplet), so the browser would have
shown a Signal Function Set module sitting beside a vendor of the same name.

`Alles` was the sentimental alternative, after Hal Alles, whose Bell Labs
machine this descends from; it is also German for "everything", which is what
additive synthesis is.

Working width 30HP. Sixteen partials, polyphonic.

## What the GDS actually was, and what is worth taking

The Crumar GDS (1980, $30,000, roughly three sold — Wendy Carlos and Klaus
Schulze had two of them) commercialised Hal Alles' Bell Labs digital
synthesiser. The Synergy (1982) was the same engine with the computer removed.
Three things about it are worth having, and one thing everybody remembers about
it is not what made it good.

**32 oscillators in a free pool.** Not 32 per voice: "voices which use more
oscillators per note have less polyphony". Timbral depth and polyphony came out
of one budget, which is a mechanic nothing modern does.

**A 16-stage amplitude envelope AND a 16-stage frequency envelope per
oscillator.** The frequency envelope is the real differentiator — partials could
start inharmonic and settle harmonic, or drift apart across the note. Competing
additive machines locked partials to fixed ratios.

**Two complete envelope sets per oscillator, a maximum and a minimum,
interpolated by velocity.** Velocity did not scale the sound, it *morphed* it.
This is the least-copied idea in the machine and the cheapest to steal.

What is NOT worth taking literally is the sixteen-stage-envelope-per-partial
part. That is 512 breakpoints for sixteen partials, it is why the GDS needed a
$30,000 computer attached to it to be programmable at all, and a Eurorack panel
has no answer to it. The interesting behaviour it bought can be had for two
numbers per partial — see below.

## Two numbers per partial, not an envelope

One shared envelope, with **depth** and **rate scale** per partial.

Depth alone is not enough, and this is the thing to get right. A shared envelope
scaled only in amplitude gives a spectral fade, bright to dark, but every partial
still ends at the same instant. The strongest perceptual cue in any struck or
plucked tone is that **high partials die faster than low ones** — it is exactly
what Kit's modal damping does and what Loom's in-loop lowpass does. That needs
the envelope to run at a different *rate* per partial, not at a different level.

So partial 7 can run three times faster than partial 1 through the same
envelope. Two knobs' worth of data per partial buys the whole family of
struck, plucked and blown behaviour, and the GDS's own frequency envelopes are
approximated by the STRETCH macro below rather than drawn by hand.

## Per-partial state

Sixteen partials, each holding:

| Field | Notes |
|---|---|
| level | 0..1, the loud spectrum |
| level (soft) | 0..1, the quiet spectrum — velocity morphs between the two |
| pitch offset | cents, a fine adjustment on top of STRETCH |
| pan | -1..1 |
| env depth | how much of the shared envelope this partial gets |
| env rate | 0.25x .. 4x the shared envelope's rate |

ENV SPREAD deliberately has **no** per-partial column. Adding one would make a
seventh screen tab, and six is already at the edge of what Loom's tabbed layout
carries comfortably. If the macro alone turns out not to be enough, that is the
tab to add and the place to spend the seventh slot.

Six values × 16 partials = 96, which is more than a panel can hold and is why
deep editing is on screen. It is the same shape of problem Loom has with eight
strings × six attributes, and the same answer applies: a tabbed display, one
attribute per screen, height as value.

### Velocity morphs the spectrum, it does not scale it

Two complete level sets, soft and loud, crossfaded by velocity. A quiet note is
a genuinely different timbre rather than a quieter one, which is the single most
GDS-like thing available and costs one extra tab and a crossfade.

v1 morphs **levels only**. Morphing pan and pitch as well is a later question,
not a hard one, but levels carry almost all of the effect.

## The panel is spreads; the screen is the exceptions

This is the organising idea, and it arrived by noticing that every macro worth
having turned out to be the same shape of thing: **a curve applied across the
partial index.** TILT is a spread of level. STRETCH is a spread of pitch. WIDTH
is a spread of pan. ENV RATE is a spread of envelope rate, ENV SPREAD is a
spread of envelope time, and each LFO's own spread is the same idea in phase.

So the panel is not a pile of unrelated controls, it is one gesture — *how does
this attribute vary from the fundamental to the sixteenth partial* — repeated
for each attribute. The screen then holds the per-partial **deviations** from
those curves: the exceptions, the one partial you want louder than the tilt
says.

That makes the module teachable in a sentence, and it means you can get a long
way without opening the screen at all.

## Macros, on the panel and under CV

| Macro | What it does |
|---|---|
| **TILT** | Spectral slope. Scales level by partial index — the brightness control, and the one additive macro nobody can do without. |
| **ODD/EVEN** | Balance between odd and even partials. Square-ish to clarinet-ish at one end, full at the other. |
| **STRETCH** | Inharmonicity, by the real law rather than a spread: `f_n = n·f0·√(1 + B·n²)`. That is piano string stiffness, `B` around 1e-4 for a piano and much larger for a bell, so one knob runs from pure harmonic through piano to gong. Per-partial pitch offsets are a fine adjustment on top of it. |
| **WIDTH** | Scales all the pan values together. |
| **MORPH** | Manual position between the soft and loud spectra, summed with velocity. |
| **ENV RATE** | Spread of envelope *rate* across the index: at zero every partial shares one envelope, turned up the highs die first. |
| **ENV SPREAD** | Spread of envelope *time* — each partial's envelope starts a little later than the one below it. Positive delays the HIGH partials, so the tone blooms upward the way a struck bell develops. Negative delays the LOW ones, so the highs speak immediately and the fundamental builds underneath, which is what a bowed or blown onset actually does. Nought to about half a second across the whole spectrum. |

Plus V/OCT, GATE, VEL, and an internal envelope driving a built-in VCA with its
own CV input.

## The LFOs

Three, and **they do not have per-partial assignments** — that would be 48 more
values and the panel could never reach them. Each LFO instead has:

`rate` · `depth` · `target` (level / pitch / pan) · `spread`

**SPREAD is what makes this work.** It sets how the LFO's phase varies across
the partial index: at zero all sixteen partials move together, turned up each
partial lags the one below it and the modulation travels up the spectrum. Four
knobs per LFO instead of forty-eight values, and it is the idiom the plugin
already uses — Cycle's phase spread and Chime's RELATE are the same idea.

An LFO on **pan** with spread is the one to reach for first: partials rotating
past each other at slightly different phases is most of what makes a sound read
as a body rather than as a line. It is close to what Chime does with its tubes.

## Things that will bite

**Partials above Nyquist must fade, not cut.** At sixteen partials a note at
1 kHz already puts partial 16 at 16 kHz, and a pitch sweep will walk partials
through the ceiling. A hard mute clicks; fade each partial out over its last
octave below Nyquist. This is the classic additive gotcha and it is cheap to get
right at the start and miserable to retrofit.

**CPU.** Sixteen partials × sixteen voices is 256 oscillators, each wanting a
sine and two multiplies for the pan. That is fine with a table or a polynomial,
and not fine with `std::sin`. Measure before choosing the voice count.

**STRETCH changes every partial's frequency at once**, so it has to be smoothed
or it zippers. Same for TILT on levels.

## Voice count: sixteen, and it is not close

Measured rather than guessed. A table sine with linear interpolation plus
per-partial panning, ten seconds of audio at 48k, one core:

| | oscillators | cost |
|---|---|---|
| 4 voices x 16 partials | 64 | 0.33% |
| 8 voices x 16 partials | 128 | 0.65% |
| **16 voices x 16 partials** | **256** | **1.59%** |
| 16 voices x 32 partials | 512 | 2.65% |

That benchmark is a floor, not a ceiling: it has no per-partial envelope state,
no Nyquist fade and no LFO modulation, and its inner loop is far more
vectorisable than the real one will be. Call it three to six per cent at full
polyphony and it is still not a consideration.

So the GDS's oscillator *budget* — spend 32 on polyphony or on partials — is a
lovely mechanic that exists only because the hardware had 32 oscillators and no
more. In software it would be an artificial scarcity, and dressing up a
limitation nobody has as a feature is the wrong kind of homage. Sixteen voices,
sixteen partials, always.

## Open questions

1. **Does the shared envelope need more than ADSR?** The GDS had sixteen stages.
   Per-partial rate scaling may make a plain ADSR sound far more alive than it
   has any right to, in which case sixteen stages is complexity for its own
   sake. Build ADSR, listen, then decide.
3. **Should the soft/loud morph cover pan and pitch too**, or only levels.
4. **Where the internal envelope ends and Swell/Vac begin.** The plugin already
   has two envelope modules; this one needs an internal envelope to be a voice
   at all, but it should not grow into a third.

## Prior art in this repo worth reusing

- **Kit**, for the fact that high partials must die faster, and for the 1/omega
  amplitude law taken from eigendrum — a force impulse gives a mode initial
  velocity, so amplitude goes as 1/omega. Sigma's default TILT should probably
  start there rather than flat.
- **Loom**, for the tabbed display over eight strings × six attributes. This is
  sixteen × six and the pattern scales.
- **Chime** and **Cycle**, for spread as a way to reach many voices with one
  control.
- **Overtone stays as it is** — eight togglable harmonics with an even/odd
  filter and a binary-mask CV. It is the quick additive VCO you patch without
  thinking. Sigma is the one you sit down with. If Sigma ever grows a "simple"
  mode it has taken a wrong turn.
