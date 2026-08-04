# Roadmap

Modules and work that are planned but not started. Shipped work lives in
`CLAUDE.md`; this file is only what is still ahead.

## Planned modules

### Comb filter
A comb as an instrument rather than a utility — the delay-line half of what
Loom's strings already do, exposed on its own so it can colour any source.
Worth reusing: Loom's fractional delay reads, its in-loop damping, and the
lesson that the filter's phase delay has to be evaluated at the tuned
frequency rather than at DC or the pitch runs sharp.

### Phaser
Cascaded allpass notches swept by an LFO. Loom already carries a 4-section
allpass chain for string stiffness (`loomAllpassDelay`), so the section maths
and its exact phase delay are in hand.

### Flanger
Short modulated delay with feedback. Shares the delay-line and interpolation
work with the comb; the distinguishing parts are through-zero behaviour and
the feedback path's DC handling — note that a DC blocker belongs *inside* a
feedback loop, not on its input (learned the hard way in Loom and Crystal).

All three are effects rather than voices, which is a gap in the set: the plugin
is currently almost all sources and sequencers. They should share one delay-line
core rather than each growing their own.

## Known gaps in shipped work

- **Slide** — built 2026-08-03 and not yet played in anger. The bar, glide law,
  scrape, slant and pickup comb are verified numerically but the *sound* has had
  no listening pass; expect the scrape level, drive amount and pickup default to
  need tuning by ear.

- **Loom** — strings tuned near the very top of the range (+36 semitones,
  ~523 Hz) bow much quieter than the rest; the pluck-position comb's null lands
  on what little the loop leaves them. Separately, below about 24 Hz every
  exciter breaks down, which is the subsonic end of the tuning range rather than
  an exciter fault.
- **Nightly workflow** — `.github/workflows/nightly.yml` release notes name a
  stale WIP list ("Wave, Ratio"); the build un-hides everything correctly, only
  the description is out of date. The nightly version string is also pinned to
  `plugin.json`'s version, so consecutive nightlies are not distinguishable by
  filename. Its actions also emit Node 20 deprecation warnings.
- **Crystal** — retired enum entries still appear in the parameter list for
  MIDI-map and automation; `surfaceDist()` is now unused.
- **Fill** — dragging a queue row past the visible rows does not auto-scroll,
  and the `×N` repeat read-out is not click-draggable.
