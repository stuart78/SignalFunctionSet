# SLIDE XP Manual

## Overview

SLIDE XP is an expander for [Slide](slide-manual.md). It gives each of Slide's
eight strings its own jacks: an audio output, a gate input, a velocity input,
and a level trimpot.

Slide used to carry the eight strings on a polyphonic output. A poly cable is
the right answer when the channels are one voice being played polyphonically. It
is the wrong one here, because these are eight strings of a single instrument
and the reason you want them apart is to send them to eight *different* places:
separate amps, a filter on one, a mixer with its own pan per string. Splitting a
poly cable back out costs a module and a row of cables anyway, so the jacks
belong on the instrument.

SLIDE XP is 12HP.

## Connecting it

**Place SLIDE XP immediately to the right of Slide.** No cables. Everything
crosses the expander bus one sample late, which is inaudible and is the price of
not making Slide 8HP wider.

With no Slide to its left the outputs go silent rather than holding whatever was
last in the buffer.

## The gates address strings, not notes

This is the important difference from Slide's own GATE input, and it is worth
being clear about.

On Slide, a polyphonic GATE is a stream of *notes*. Channel N is the Nth note,
and which string it lands on is the bar solver's business, because on a real
lap steel you do not choose: the bar is across all the strings and the note you
get is the one that string can make at that bar position.

On SLIDE XP, the jack labelled **3** plays **string 3**. That is the thing a
patch cannot otherwise ask for, and it is what makes the expander worth having
beyond the extra outputs: you can play the instrument string by string, with
eight separate triggers, rather than handing it notes and letting it decide.

## Per string

| Jack / control | What it does |
|---|---|
| **GATE** | Picks that string. A rising edge is a pluck. |
| **VEL** | Velocity for that string, 0–10V. |
| **level trimpot** | Scales the velocity. |
| **OUT** | That string's audio, with an activity LED beside it. |

### The trimpot is more than an attenuator

With nothing patched to a string's VEL jack, **the trimpot is that string's
velocity**. So the eight of them are a picking-balance control in their own
right: pull the bass strings down, lean on an inner voice, and the instrument
plays with a hand rather than flat. Patch a VEL cable and the same trimpot
scales it.

They default to full, so an unpatched SLIDE XP behaves exactly as Slide did
before it existed.

## Outputs

Eight string outputs, each with an activity LED. They are the individual strings
before Slide's stereo mix: post pickup, post amp drive, pre pan.

Slide keeps its own MIX L / MIX R and its EVEN / ODD sums; those are unaffected
by the expander and can be used at the same time.

## Patch ideas

**Eight amps.** The obvious one, and the reason it exists. Each string to its
own reverb send, filter or channel strip. A lap steel where the low strings are
dry and the top three are drowning.

**Playing it string by string.** Eight gates from a drum sequencer, one per
string, with the bar parked somewhere. You are no longer playing notes, you are
playing an eight-string zither, and SLANT still tilts the whole thing into a
different chord.

**Picking balance without automation.** Set the eight trimpots by ear with
nothing patched to VEL. It is the fastest way to make an auto-roll stop sounding
mechanical, because the difference between a thumb and an index finger is mostly
level.

**Per-string processing in a stereo field.** Strings 1–4 to one bus and 5–8 to
another, each with its own delay time. The instrument spreads without any
panning at all.
