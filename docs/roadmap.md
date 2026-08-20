# Roadmap

Modules and work that are planned but not started. Shipped work lives in
`CLAUDE.md`; this file is only what is still ahead.

## Planned modules

### Comb filter
A comb as an instrument rather than a utility: the delay-line half of what
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
the feedback path's DC handling. Note that a DC blocker belongs *inside* a
feedback loop, not on its input (learned the hard way in Loom and Crystal).

All three are effects rather than voices, which is a gap in the set: the plugin
is currently almost all sources and sequencers. They should share one delay-line
core rather than each growing their own.

## Known gaps in shipped work

- **Slide**: SCRAPE was tried twice and dropped: bandpassed noise sounded like
  hiss, an impulse train at the winding-crossing rate sounded like a record
  scratch. The physics was right both times (a wound string is a grating, and
  the crossing rate really does track bar speed), which is the interesting part,
  a correct model of the mechanism is not the same as a convincing sound, and
  what a bar on a string sounds like in a recording is mostly the room and the
  body, neither of which this has. Worth another attempt only with a different
  idea, not a third tuning of the same one. Still unproven by ear: whether the
  hand tremor is the right size, and whether the poly bar-solver picks positions
  a player would. The
  bar's approach curve is an S-curve but does not overshoot-and-settle the way a
  player arriving at a note does. The behind-the-bar string segment is still not
  modelled, deliberately, since players damp it, but it is the Dobro ring if
  ever wanted. **Slide is not decided, it is emergent**: the bar always glides to
  wherever the solver puts it, so a note landing on a string at the current
  position does not slide and one needing a new position does. There is no
  legato/staccato distinction, which is the obvious next control.

- **Loom**: strings tuned near the very top of the range (+36 semitones,
  ~523 Hz) bow much quieter than the rest; the pluck-position comb's null lands
  on what little the loop leaves them. Separately, below about 24 Hz every
  exciter breaks down, which is the subsonic end of the tuning range rather than
  an exciter fault.
- **Nightly workflow**: its actions still emit Node 20 deprecation warnings.
- **Crystal**: retired enum entries still appear in the parameter list for
  MIDI-map and automation; `surfaceDist()` is now unused.
- **Fill**: dragging a queue row past the visible rows does not auto-scroll,
  and the `×N` repeat read-out is not click-draggable.

## Arbitrary drum shapes (Kit)

Kit uses the closed-form Bessel modes of a circle, which is why strike position
is a table lookup and the whole thing is cheap. A non-circular head has no closed
form: you have to mesh the region and solve the Laplacian eigenvalue problem
numerically, then resynthesise whenever the shape changes.

[eigendrum](https://github.com/BaselAshraf81/eigendrum) does exactly that in the
browser, with P1 finite elements, the lowest 16 eigenpairs by block inverse
iteration with Rayleigh-Ritz, and the solver on a worker. It is worth reading
before anyone attempts this here. It also ships the Kac isospectral pair, two
different shapes with the same spectrum, which is a good reminder that shape and
sound are not in one-to-one correspondence.

For Kit this is a different architecture rather than a feature: a mesh, an
eigensolver, a worker thread and a shape editor. Probably its own module if it
happens at all.

Two smaller things from the same source, one taken and one not:

* **Taken.** A force impulse gives a mode initial velocity, so amplitude goes as
  1/omega. Kit was injecting force straight into displacement and every high
  mode was that much too loud.
* **Not yet.** Rayleigh damping, `C = alpha*M + beta*K`. The beta term damps high
  modes as omega squared, where Kit's law is a single power near 1/omega at its
  default; and the alpha term damps LOW modes, which Kit has no equivalent of
  at all. That is what stops a real drum's fundamental ringing forever.
