# Roadmap

Modules and work that are planned but not started. Shipped work lives in
`CLAUDE.md`; this file is only what is still ahead.

## Planned modules

### Trace — v1 built, hidden
A paper loop running behind the panel, four brushes drawing lines on it, one
read head per lane. Part Oramics, part chart recorder. The two ideas that carry
it are that the transport is uncoupled from the brush (so where your mouse sits
relative to the head decides whether you are editing the past, composing the
future, or performing), and that ink thickness is a second output per lane:
write heads lay it down, read heads lift it off, and it pools where the lines
cross. Events come from the line's *shape* rather than its value -- each lane
classifies its slope as up, down or flat and fires on the transitions, so a
scribble makes notes where it turns around with no quantization involved. Patch
a CV into a lane and it becomes a CV tape recorder, with a thickness input
alongside for the velocity contour. Specified in full in `docs/trace-design.md`,
which also records where v1 departs from the spec and, at length, every way the
display managed to invent movement that was not in the paper.

Unproven by ear: the flat-threshold default, whether crossings are the right
source for ink, and whether drawing onto moving paper feels direct or fights
you. Known gap: the browser preview and the live display now draw the ribbon the
same way, but the preview's content is still a synthetic sine rather than
anything the module would produce.

### Prism — v1 built, hidden
A deep additive voice, sixteen partials, after the Crumar GDS. The GDS gave each
oscillator a sixteen-stage amplitude AND frequency envelope, plus two complete
envelope sets interpolated by velocity; that needed a $30,000 computer bolted to
it to be programmable. The bet here is that the interesting behaviour survives
two numbers per partial — envelope **depth** and envelope **rate** — because
what makes a struck tone sound struck is that high partials die *sooner*, not
just quieter, and depth alone cannot express that. Velocity morphs between two
complete spectra rather than scaling one. Macros (TILT, ODD/EVEN, STRETCH,
WIDTH, MORPH, ENV RATE, ENV SPREAD) reach all sixteen at once and take CV; deep
editing is on screen, tabbed the way Loom is.

The organising idea is that **every macro is a curve across the partial index**
— tilt is a spread of level, stretch a spread of pitch, width a spread of pan,
ENV SPREAD a spread of envelope start time so the tone blooms upward or the
highs speak first and the fundamental builds underneath. The panel is that one
gesture repeated per attribute; the screen holds the per-partial exceptions to
it. The three LFOs use the same idea in phase rather than per-partial routing.

Sixteen voices by sixteen partials, settled by measurement: 256 oscillators is
1.6% of one core, so the GDS's spend-your-oscillators budget would be an
artificial scarcity here rather than a homage. Specified in
`docs/prism-design.md`. Distinct from Overtone, which stays the quick
eight-harmonic additive VCO.

Built, with the partial count now selectable at 16/32/64. What the v1 taught:
the per-partial arrays are only worth having if the macros can be *escaped*, so
the screen is five tabs over one shared spectrum rather than five graphs.
Unproven by ear: whether ENV SPREAD's negative half (highs first, fundamental
building underneath) is musical or merely a delay, and whether the SOFT/LEVEL
velocity morph earns a second full spectrum's worth of editing.

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
