# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a VCV Rack plugin called "Signal Function Set" that provides modular synthesizer modules. Currently shipping:

1. **Drift** — Phase-shifted LFO with offset, attenuation, and Lorenz-attractor chaos
2. **GSX** — Granular synthesis (Barry Truax GSX system, 1985-86)
3. **Fugue** — 8-step harmonic deviation sequencer with three CV/gate voices
4. **Fugue X** — Expander for Fugue: per-voice steps/range/sleep/probability
5. **Phase** — Dual sample looper with sleep-based phase drift + live recording
6. **Overtone** — Additive VCO with 8 togglable harmonics, even/odd filter, binary-mask CV
7. **Intone** — CHANT/FOF formant synthesis voice with vowel morphing
8. **Tine** — Tunable 3rd-order pingable resonator (Gamelan Resonator circuit)
9. **Meter** — Time-signature-aware musical clock with subdivision outputs and per-output swing
10. **Meter X** (slug `MeterX`) — Expander for Meter: 24 PPQN clock, Run gate, and Bar / 2 / 4 / 8 / 16 / 32 / 64 / 128-bar trigger outputs, each with an activity LED. Reads Meter's clock over the expander bus (`src/meter-messages.hpp`)
11. **Beat** — Per-voice pattern sequencer (8×16) with on-screen step/velocity/accent/probability editing
12. **Note** — Monophonic CV/gate pattern sequencer with 12-note pitch matrix and scale/root selection
13. **Swell** — Ping-driven additive A/D envelope with stacked rises
14. **Shift** — 4-output CV shift register with per-lane delay/cascade modes
15. **Wave** — Polaroid wavetable voice *(WIP, hidden — not ready)*: live parametric shape + 8 FIFO snapshots + WANDER macro
16. **Vac** — Semi-stable A/R envelope with vactrol-like timing drift (log-symmetric STAB)
17. **Muse** — Faithful Triadex Muse recreation (Fredkin/Minsky 1972) — 4 theme + 4 interval sliders
18. **Gravity** — Multi-mode chaos engine (pendulum / gravity well / billiards / Hungry Man Pac-Man maze / LOGO Turtle / Pattern spirograph-rose generator) with X/Y, radius, angle, sector CVs and ray-crossing gates
19. **Band** — Harmonic bandpass bank: 4 bands each lock to an integer harmonic of a shared (auto-detected or 1V/oct) fundamental; per-band level/harmonic/enable + CV, spectrum display
20. **Cycle** (slug `Cycle`) — Bar-synced quad LFO: four channels (A-D) with morphable shape + depth, locked to a musical bar via clock + bar inputs (free-run Hz unpatched); global phase spread + stability; per-pair SHAPE/SCALE **link buttons** (a linked channel follows its group's leftmost leader, its own pot becomes an offset from the leader — circular values wrap, bounded ones clamp); per-channel uni/bipolar outs
21. **Operator** (slug `Bell`) — DX7-style 6-operator FM voice on the msfa engine; loads .syx cartridges, polyphonic, AUDIO/VCO/ENV-follower outs, tabbed operators/envelope display
22. **OP ENV** (slug `OpEnv`) — standalone DX7 operator envelope generator; loads a voice's carrier EG, offsets all 8 rate/level attributes via trimpot/CV, V/oct rate scaling, LFO tremolo, "Release to 0V" option
23. **Arrange** (slug `Arrange`, formerly Phrase) — song-form / arrangement sequencer: a single horizontal chain of 8 phrases (bar-length sections) advancing linearly (next active, wraps). Each phrase carries a bar length (4×4 grid, 1-16), enable, root + scale + BPM (trimpots; **LED link dots between columns cascade BPM/root/scale independently per row, default linked** — a linked phrase inherits its group's leftmost leader's value exactly; break the link for independence), a **GATE out that stays high while that phrase plays** (row under the trimpots), and **4 channel enables** (colored bars on screen: blue/green/orange-red/purple). The 4 channels are per-instrument clock buses in a dark inset — each column has a clock-division trimpot (÷1..÷16, bar-aligned) above its divided CLOCK out, then BAR, RESET (fires on master reset + channel re-enable), and EOC (arrangement wrap) outs, all muted on phrases where the channel is off (instruments drop in/out per phrase); the master outs (BPM 0.01V/BPM → Meter's "BPM CV absolute", Root + Scale CV in Note convention, Phrase index 1V/phrase) sit at the inset's bottom. BAR in (advances the arrangement — Meter's BAR out) + CLOCK in (divided per channel) + RESET in/btn bottom-left. No master Bar/Clock outs — Meter is the source; instruments take the channel outs. 34HP. The arrangement brain above Beat/Note/Chance.
24. **Chance** (slug `Chance`) — Generative melodic walk sequencer (26HP, external clock): a seeded deterministic core melody + a BRANCH probability of straying to a neighbour (non-cascading, so the core always shows through); Markov note choice (`src/chance-markov.hpp`); GRAV/DRIFT = direction + move size; Rest/Hold/Leap/Ratchet shaping each with CV; **START/END** set the play window — trimpot + CV jack at 1V/step, and setting END *before* START plays the window backwards; GATE LEN + GLIDE live in the context menu (menu sliders) rather than on the panel; 8 seeded patterns (micro-waveform, per-step 3-state gates off/gate/tie, repeat count, normal-vs-reseed mode) rotating at cycle level; Harmony 2nd voice (fixed interval or Varied); on-screen editing (click a pattern to load it into the walk; gate edits apply immediately mid-cycle)
25. **Record** (slug `Record`) — Auto-sampler (16HP): drives any voice with V/OCT+GATE+VEL, records the stereo return across a note × velocity sweep, and writes WAVs + an `.sfz`. Audition mode, round-robins, loop-point detection, latency calibration, wait-for-silence, mono/stereo + 16/24/32-bit. **WAVs are written on the GUI thread** (`RecordWidget::step()`), never from `process()`
26. **Play** (slug `Play`) — Polyphonic multisample player (16HP): loads `.sfz` or DecentSampler `.dspreset` (both parsed inline in play.cpp) into a shared 16-voice engine; velocity layers, round-robins, loops-while-held, per-note tune; note-off releases by default (menu "One-shot (play through)" for drums); multi-instrument via INSTR knob/CV; poly V/OCT+GATE read with `getPolyVoltage`. **WAV + FLAC** samples (dr_wav / dr_flac, implementation TU in `src/dr_flac.cpp`), decoded to **int16** and loaded on a **background thread** (`startLoader()`; regions are silent until their `loaded` flag flips, and every structural change to `instruments` calls `joinLoader()` first) — third-party piano libraries run to gigabytes. SFZ compatibility: `\` path separators are translated, `trigger != attack` and CC-gated regions are dropped (Play has no release-trigger or pedal state, and leaving them in makes the round-robin play resonances instead of notes), keyswitched libraries keep only the `sw_default` articulation, and `offset` / `amp_veltrack` / `global_volume` are honoured
27. **Fill** (slug `Fill`) — *(WIP, hidden)* Auto-playing 8-channel drum sequencer (32HP, external clock): loads every `res/patterns/*.json` bank (canonical file first, so its first set stays the default) plus user banks from `<Rack user dir>/SignalFunctionSet/patterns/`. One internal **pressure** value accumulates per bar and vents as a fill, driving both the intensity tier (sparse/main/lift, phrase-latched) and a seeded variation layer; the EXTRAS knob is a hard cap on engine-added notes and each set's `vary` limits how far its identity may bend. Playback is **clock-driven** (steps fire on CLOCK edges, `clocksPerBar` measured between BARs) so it freezes the moment the clock stops. Five-tab display (PATTERN · GENRE · REGION · USER · FAVS); favourites persist plugin-wide in `fill-favorites.json`; drum-patterns.com `.txt` exports import natively. Per-channel gate/velocity/accent + NUM/DEN outs feeding Meter's "Time signature CV absolute"
28. **Chime** (slug `Chime`) — *(WIP, hidden)* Eight-note resonating drone machine (28HP), after a xylophone whose resonator tubes rotate beneath the bars: each note has a semi-free bidirectional LFO ("tube rotation") and blooms as its tube swings through centre. Excitation is a continuous **bow ↔ strike** blend (struck at every centre crossing — two per rotation, as the mechanism gives); RELATE sets how the eight rates relate (ramp / stepped / random / **ripple**, where a crossing excites its neighbours and blooms travel down the row); per-note weight = strike probability, per-note swing attenuator trades arc width for strike rate. Pitch **latches at note boundaries**, so retuning never bends a sounding note. 8 tube-LFO CV outs, 8 audio outs, stereo mix, poly V/OCT + GATE
29. **Crystal** (slug `Crystal`) — *(WIP, hidden)* 3D echo chamber shaped like a real crystal (28HP). Sixteen habits built as intersections of half-spaces from symmetry-expanded Miller-index normals (cube {100}, octahedron {111}, dodecahedron {110}, sphalerite tetrahedron, pyrite {210} pyritohedron, quartz, the low-symmetry systems) ordered simple→complex, ending in four **clusters** — several whole crystals intergrown, so a ray can be trapped in one point or wander between them. Traced by **letting rays loose** (7 rays × 26 bounces per emitter, deterministic fan): every wall strike sheds an echo toward each of four interior listeners = the **quad outs**. Tracing runs on a worker (~50ms) and the audio thread adopts the tap set with a crossfade; per sample it reads a multitap delay, six recirculating "pockets" per emitter, and an FDN tail sized by Sabine on the crystal's own volume/area. CHAMBER keeps real acoustic timing, **DELAY** keeps the geometry's arrival ratios but stretches them to musical times (SIZE = delay time). Two emitters (stereo in → quad out) navigable by X/Y **velocity** CV; internal exciter is Chime's struck bar, so fully dry is a bare resonator. Rotation is camera-only

30. **Loom** (slug `Loom`) — *(WIP, hidden)* Eight-string resonator (28HP). Each string is a digital waveguide: delay loop for pitch, in-loop lowpass, a 4-section allpass chain for stiffness, and a comb on the **output tap** for pluck position. Filter phase delay is evaluated **exactly at the string's fundamental**, not at DC, or the strings run sharp. **EXCITE is a continuous axis** — HAMMER · PLUCK · BOW · WIND as nodes; at a pure node the code path (including how many random numbers it draws) is identical to the single-exciter version, so pluck and hammer stay **bit-exact**. **Bow** = Smith friction curve, and three things make it work everywhere rather than only where it was tuned: (1) the curve's input is **normalised by the string's own envelope**, so its operating point does not drift with pitch/decay/damping; (2) a **pressure regulator** holds the string at a target amplitude — an unregulated friction model settled anywhere between silent and pinned to the clamp, taking up to 6 s to get there, and *every measurement taken before ~5 s is measuring the build-up, not the tone*; (3) the regulator drives **force, not bow speed**, because the friction curve is non-monotonic in speed and a controller pushing on speed sails past the peak. Bow speed sets loudness, not whether it speaks; **hair noise** is load-bearing (without it the string locks to its sub-octave). **WIND** is an aeolian harp: vortex shedding drives a *narrow* band that **moves with the gust** (Strouhal), so the harp climbs and falls between partials — fundamental ~26 dB down, dominant partial roams 3→8. A fixed band just sounds like a filtered bow. **DAMP is floored at 10× each string's own pitch**: as an absolute cutoff it filtered a high string below its second partial, and with so few partials left the pluck-position comb's null (near the 4.5th) removed what remained — a dark high bowed string made almost no sound. **COUPLE**: string *motion* (not the combed pickup, which peaks at +6 dB and howls in a loop) feeds a bridge bus; each string trades a band-limited fraction, `x += k·(bus − ownLowpassed)`. Only the transmitted band is damped, the in-phase mode has gain 1 (unconditionally stable). Ceiling **0.06** — 0.35 cost 89% of the sustain and bought *less* ring. Output is **soft-clipped** (linear to ±6 V, asymptotic to ±10): eight bowed strings have a crest factor no pluck approaches. Display is the instrument: PLAY strums with the mouse (Y = where along the string), six tabs edit one attribute per string by height. 8 gate ins + poly VEL, 8 string outs + stereo mix, 12 arpeggio patterns. **Known gap:** strings tuned near the very top (+36 st ≈ 523 Hz) bow much quieter than the rest.
31. **Key** (slug `Key`) — *(WIP, hidden)* Quantizer that takes its key from the patch (14HP). Reads **ROOT** (1V/oct, semitone-quantized) and **SCALE** (1V per scale) in the plugin convention, so one key change travels to Arrange, Note, Chance, Muse, Fugue, Loom and this together. Four channels, each **polyphonic** (16ch), all sharing one key. Per channel: IN, **SUB** (full scale or one of three sub-scales), **OFFSET** (scale *degrees* by default, so +2 moves two steps up the scale and stays in key; semitones via the menu), OUT. (Per-channel **CHG** triggers were retired with the 2026-08 panel: the enum slots stay, because outputs serialise by index.) Global **TRIG** turns it into a sample-and-hold. **Three SUB-SCALES** filter the selection, stored as masks over the parent's **degree indices** rather than absolute pitches — so `{0,2,4}` is a triad in Major (C E G), still a triad in Minor (C D♯ G), and follows the root (D♯ G A♯ in E♭). A sub-scale is a *role within the key*, not a set of notes; an empty one falls back to the parent rather than muting. A menu option, **"Sub-scales may leave the key"** (off by default), makes the out-of-scale cells clickable so a sub-scale can carry an accidental. Those picks live in a **second store, `subChrom`, as semitone offsets from the root** rather than degree indices, because a note chosen for being outside the scale has no degree to be stored as: they transpose with ROOT and do *not* follow a SCALE change. A ring round the cell marks one on screen. The option is inert on a non-12-tone scale, where there is no chromatic to pick from. **Scala (.scl) files load** from the menu and occupy SCALE index `NUM_SCALES` (0..18 stay exactly the canonical list, so cross-module SCALE CV is unaffected); cents *and* ratios parse, and the **period need not be an octave** — Bohlen-Pierce comes in as 13 degrees repeating at 19.02 semitones (3/1). The parsed scale is saved into the patch alongside the path, so it survives the file moving. **Nothing here is a 12-bit pitch mask**: Harmonic series, Pelog and Slendro carry fractional intervals and Scala can carry anything, so the quantizer works in "semitones within one period" against real floats. The screen has **two states, and shows one or the other** (`Key::chromaticKey()`): the **keyboard** when the period is 12 semitones and every degree lands within a cent of a semitone, otherwise the **region strip**. Harmonic series, Pelog, Slendro and every Scala file therefore get the strip, which is linear in pitch across one period so the gaps between its degree lines *are* the snap regions — the keyboard could only round them. The keyboard is drawn from **C whatever the root is** (you read a keyboard by its shape), 5 upper and 7 lower circles on a 13-slot grid whose gap is the missing black key between E and F, with the root marked by an inset dot. The three sub-scale rows sit under it as **12 chromatic cells aligned to their own note** in the keyboard state, and as **one cell per degree** in the strip state. Clicking a key forks a **custom mask**; clicking a sub-scale cell toggles the degree at that pitch class, or the chromatic pick when free mode is on. Screen geometry is transcribed from `design/Key/*.svg` in the design's own 760x480 units and scaled once, because the panel's screen is the same ratio. Hysteresis (menu, default 12 cents) is abandoned whenever the key *or the channel's sub-scale* changes, or a held note would survive outside its new scale. **Key is the master key control**: **ROOT OUT** (1V/oct) and **SCALE OUT** drive every other module's ROOT/SCALE inputs. SCALE OUT is **polyphonic** — channel 0 is the plain 1V-per-scale index every existing module already reads (they all call `getVoltage()`, which returns channel 0 regardless of channel count, so the extension is invisible to them), channel 1 is the period in volts, and channels 2+ are the degrees as 1V/oct offsets from the root. That is how a microtonal or Scala scale reaches another module at all: the index rides on channel 0 as a lossy summary (the nearest canonical scale by pitch-class content — Pelog resolves to Phrygian), and the real scale rides behind it. Key's own SCALE **input** reads the extension when present, so Keys chain and the whole key crosses intact, non-octave period and all; a stray poly cable is validated and rejected rather than misread. 14 degrees max (16 channels less index and period). See `docs/conventions/scales.md`.

32. **Slide** (slug `Slide`) — *(WIP, hidden)* Electric lap steel (26HP). Eight waveguide strings stopped by a **steel bar** rather than frets. The bar is one rigid object lying across every string, so it moves them all by the same ratio and the tuning's intervals survive — which is why lap steel lives in 6th and 7th tunings (C6, E7, E13, A6): a straight bar is already a chord. **SLANT** angles the bar across the strings, the technique that gets major, minor and dominant voicings out of one tuning without retuning; it is the reason this is its own module rather than a glide mode on Loom. What makes it read as a slide rather than a pitch bend, in order: (1) **GLIDE is rate-based, not time-based** — a hand crosses the neck at roughly constant speed, so a twelfth takes 2.4× as long as a fifth; nearly every synth portamento is constant-time-per-interval and that is most of why synth glides don't sound like slides; (2) *(a SCRAPE model was tried twice — bandpassed noise, then an impulse train at the winding-crossing rate — and dropped: the physics is right and neither sounded like a bar on a string; the param is retired in place so indices do not move)*; (3) the **pickup is fixed in space** while the speaking length changes, so its position as a fraction of the string runs 14%→48% up the neck and the comb null walks from the 7th harmonic to the 2nd — the tone hollows as you climb (Loom's comb is a fixed fraction, right for a fretted instrument and wrong for this); (4) the bar is a **lossy, mass-loaded termination**, returning less treble than a fret clamping against wood; (5) vibrato is a **rocking** motion, wide and centred on the pitch rather than bending up to it. The "body" is a magnetic **pickup**, not a soundboard: a resonant lowpass (coil inductance against cable capacitance) with TONE moving the peak 1.6–6.2 kHz, then amp DRIVE. Display is a **fretboard lying flat** — strings horizontal, logarithmic fret spacing, the bar drawn as a line whose *angle* is the slant, and the segment behind the bar drawn dead. Drag the bar with the mouse; hover across the strings to pick them. Clocked fingerpicking **rolls** (forward, backward, alternating, pinch…), poly GATE/VEL, stereo mix + 8-channel poly out. Shares `src/waveguide.hpp` with Loom. Signal chain: a **volume pedal** (SWELL per note, plus a VOL input) that swells in *past* the pick attack — the missing transient is what makes a steel cry, and it is the one thing a slide model can do that a portamento cannot fake; **COUPLE**, Loom's bridge bus ported over, for the sympathetic halo Slide previously had none of; and a menu **pickup character** where Horseshoe adds a broad midrange band under the coil resonance, because bark and honk is a wide lift and no single resonant lowpass makes one wherever you put its corner. **LEGATO REACH** (menu) is how far the bar will travel to keep a held note on the string it is already sounding on, in frets, before it gives up and crosses. It exists because a steel player does not lift the bar for a note that arrives while the last one is still ringing, they move it, and that is where the cry comes from. It also fixes a real bug: **nothing in Slide picks on a note change** (every `pick()` comes from a gate edge, a roll, the mouse or Slide X), so when the solver moved a held voice to another string, that string had no energy in it and the string still ringing got dragged to whatever pitch the new bar position gave it. Measured on a held C major scale in C6, the last three notes came out a third and a fifth flat. A forced cross now re-picks. The control is a **reach in frets, not a weight**: the first attempt lerped a stickiness factor into the cost and saturated by 0.5, so half the knob did nothing — the same failure as Slice's SHAPE. As a budget it sweeps properly, taking a leaping line from 7 string crossings to 0 and bar travel from 5 frets to 21 across its range, while a stepwise line correctly stays on one string above about 1 fret. 0 is the pre-2026-08 behaviour. Bar travel is an **S-curve scaled to the distance** — a fixed deceleration window makes a short move all ease and a long one a hard ramp with a nub on the end. Auto-play rolls carry **per-step accents** — the thumb is a heavier finger than the index, so bass strokes land harder and the stroke that starts a roll hardest; DYN brings in that shape first and note-to-note jitter second, because jitter alone humanises when the accents fall but not what shape they make.

32. **Slice** (slug `Slice`) — *(WIP, hidden)* Cuts a stereo stream onto a grid and reworks each piece (22HP). Sixty seconds of audio go into a circular buffer (30 / 60 / 120s on the menu, which matters because REACH counts up to 32 bars and at 120 BPM that is 64 seconds — with the old 30s buffer two thirds of that knob's travel was unreachable; 120s costs 44MB at 48k and 176MB at 192k, per instance, which is why it is a choice). **Resizing hands the buffer over rather than reallocating in `process()`**: the UI thread builds the new vectors, the audio thread takes them with a `std::vector::swap` (a pointer exchange, no allocation), and the widget frees the old memory on the next frame; a grid of slices runs over it, 10ms to 1s each, free-running from LENGTH or measured from CLOCK (which then takes over, and the screen tags the readout CLK or LEN so you can see which is in charge). The clock rate runs /8 /4 /2 **x1** x2 x4 x8 with x1 dead centre. **WHAT happens and HOW OFTEN are two knobs, not one**: EFFECT picks one of seven (**cut · swap · delay · shuffle · reverse · repeat · pitch**) or MIXED to rotate through them, and EVERY says how many slices pass between firings (1/2/3/4/6/8/12/16). Two earlier designs were wrong and are worth remembering: seven per-slice probability weights gave the same undifferentiated scatter at every setting, and replacing them with twelve named patterns welded the two questions together so most effects were stuck at every-4. The last slice of each group fires, so the effect lands on the approach to the downbeat. **It is zero-latency when it passes through**: a straight slice is read from the write head sample for sample, but you cannot reverse a slice you are still recording, so the transforms that need a finished one reach back a slot and work on THAT (reverse plays the previous slice backwards, pitch plays it slowed). **REPEAT stays live** — it plays the first 1/N as it arrives, then loops what it captured. It is also the only transform that splices *inside* a slice, so it is the only place a click can come from that the edge fades do not cover: the menu's **Repeat splice** is **Clean** (a 1.5ms raised cosine either side of the wrap, measured to cut the discontinuity from 5.9V to 2mV) or **Dirty** (leave it, because the click is most of what makes a stutter sound like a stutter rather than a loop). The splice fade is deliberately NOT the SHAPE curve: hiding a discontinuity inside a slice is a different job from shaping its edges, and it should not change character when the edge shape does. **WINDOW is how much of the slice is window**, expressed as a fade time: 5ms at the bottom, **half the slice** at the top, with the tooltip reading milliseconds rather than a percentage. It ran to a fixed 200ms ceiling until that ceiling turned out to be holding two different ideas apart. A fade is an *edge taper*, and the curve of an edge taper is nearly inaudible — the ear hears how long it is, not what shape it is. A window function is the *whole arc*, nought to one and back, and choosing between Hann and Gaussian only describes something real when the window spans the thing it is windowing. With the ceiling in place a 940ms slice at WINDOW maximum still kept a 540ms plateau, so the two half-curves never met: every setting was a Tukey window with a short taper, SHAPE only ever got to bend the taper, and five window functions were never allowed to be windows. That is why they all sounded the same. Clamped instead to half the slice, the top of the knob is a true window and the shapes separate completely — measured over a 940ms slice they run 19/29/36/42/55% of it above -20dB and 5.2dB apart in energy, where at a 48ms fade the same five sit within 0.004 of each other. WINDOW therefore sweeps from *declick* (SHAPE correctly irrelevant) to *grain envelope* (SHAPE decisive). Past half the slice the two fades would overlap and it would never reach full level, which is a volume drop rather than a window. It used to run from 1ms, which was wrong twice: a 1ms ramp is a quarter cycle at 261 Hz, so no curve is distinguishable from a hard edge down there — no curve was distinguishable from a hard edge, so the shape control had nothing to say at the bottom of its range — and the bottom of the knob was an unusable setting that looked like a legitimate one. There was briefly a longer floor for edges that gate to silence and a shorter one for edges that splice between two pieces of audio; the distinction is real, but 5ms serves both, so one number does it and there is no conditional left to get wrong. An edge with nothing to hide still gets no fade at all. **SHAPE picks the window, and there are only three of them.** The control was wrong three times, each one level deeper. Square returned 1.0 unconditionally, so one position of a *shape* control silently switched the whole envelope off — a hidden mode, not a shape, and it cost an afternoon of chasing a click that was a setting. Its replacement was *inaudible*: Gaussian(σ=0.4), Hann and smoothstep sat within 0.01 of each other. Respacing them by gain ratio did not help either, because a 27dB ratio between two silences is still silence — the honest metric is where a curve crosses into audibility, not how it compares with its neighbour. The actual mistake was a category error, and the fix is the WINDOW change above: a fade is an *edge taper* and the ear hears its length rather than its curvature, where a window function is the *whole arc* and its family name only describes something audible when the window spans the slice. Once they are genuinely windows, what separates them is **width, not pedigree**: Blackman, Hann, Sinc and Log are all wide smooth arcs, keeping 70/80/91/97% of the slice above -20dB, which is four names for one window. Gaussian stands apart only because its σ is narrowed to 0.22. So the set is three real widths and nothing that merely sounds like a fourth — **Gaussian** (47% of the slice loud, a blip in the middle of its slot), **Hann** (80%, the classic arc), **Log** (97%, nearly the whole slice with the corners taken off), a weakest gap of 17 points against 5.8 for the pair that was cut. Gaussian is shifted and rescaled so it truly reaches zero, since a Gaussian never does and starting at 0.044 is a step of 0.044; Log is a cubic attack, nearly immediate as a taper and near-rectangular as a window, and is the honest version of what Square was reached for. The screen header states the two fade lengths actually in force. DEPTH crossfades the altered slice against the straight one. **REACH is a COUNT, and the parameter itself is one** — 1..32, snapped, of steps or of bars once BAR is patched, with a `SliceReachQuantity` swapping the unit so the tooltip reads "8 steps" or "2 bars" rather than a percentage. One range and only the unit changing, because two ranges would leave three quarters of the travel dead in bar mode. As a percentage of thirty seconds the knob's whole lower half rounded to the same slice and no position on it named anything. Thirty-two bars at a slow tempo is still longer than the buffer, so the screen shows `7/32 bars` when the request is being capped rather than printing a reach that is not happening. **The screen is anchored to NOW**: the write head is the right-hand edge and the window is exactly REACH wide, with peaks read out of the buffer at whatever resolution that needs, the slice grid ticked along the foot, and a line back to where the current slice was fetched from. Drawing the whole buffer with a sweeping cursor showed where in an array you happened to be, which is a fact about the implementation. Under it, **one pane per channel, five seconds each, now at the right**, carrying both signals for that channel but drawn differently on purpose: **blue filled is what leaves the jack**, orange outline is what came in. Both were filled at first, and CUT at full depth then showed a fat orange waveform against complete silence — a display that draws a signal you cannot hear as though you can is worse than no display, so only the audible one is solid and the source it departed from is a line. The trace is the real output, post-DEPTH, not the pre-mix wet signal. Everything is drawn as a **filled envelope, the way Phase draws a sample** (peak along the top, back along the bottom, closed and filled); a column of separate bars reads as a bar chart rather than as a waveform. FREEZE stops the write head and loops. LINK (menu; it came off the panel to make room for WINDOW) pairs the channels (default) or gives the right its own source. The panel departs from the plugin's pot-over-jack pairs: **screen on top, controls in the middle, every jack in two rows of eight at the foot** with the outputs on a plate — Slice has twelve inputs and only five modulate a knob, so pairing would scatter the audio and clock jacks among the controls. Verified against the real table: all eight effects fire at all eight rates, exactly 48/n times in 48 slices.

## Build Commands

- `make` - Build the plugin (creates plugin.dylib)
- `make clean` - Clean build artifacts 
- `make dist` - Create distribution package
- `RACK_DIR=.. make` - Build with explicit Rack SDK path (default is two directories up)
- `RACK_DIR=.. make clean` - Clean with explicit Rack SDK path
- `RACK_DIR=.. make dist` - Create distribution with explicit Rack SDK path

The build system uses the VCV Rack plugin framework via `$(RACK_DIR)/plugin.mk`.

## Code Architecture

### Plugin Structure
- `src/plugin.hpp` — Main plugin header with model declarations
- `src/plugin.cpp` — Plugin initialization and model registration
- `src/quadlfo.cpp` — Drift
- `src/gsx.cpp` — GSX
- `src/fugue.cpp` — Fugue
- `src/fugue-expander.cpp` — Fugue X
- `src/phase.cpp` — Phase
- `src/overtone.cpp` — Overtone
- `src/intone.cpp` — Intone
- `src/tine.cpp` — Tine
- `src/meter.cpp` — Meter
- `src/meterx.cpp` — Meter X (expander; `src/meter-messages.hpp` = Meter ↔ Meter X bus)
- `src/beat.cpp` — Beat
- `src/note.cpp` — Note
- `src/swell.cpp` — Swell
- `src/shift.cpp` — Shift
- `src/wave.cpp` — Wave
- `src/vac.cpp` — Vac
- `src/muse.cpp` — Muse
- `src/gravity.cpp` — Gravity
- `src/band.cpp` — Band
- `src/cycle.cpp` — Cycle
- `src/chance.cpp` — Chance (`src/chance-markov.hpp` = note-choice tables)
- `src/record.cpp` — Record
- `src/play.cpp` — Play (`src/pushgrid.hpp` = shared pad grid, also used by Record)
- `src/arrange.cpp` — Arrange
- `src/fill.cpp` — Fill (banks in `res/patterns/*.json`, format doc + generators in `patterns/`)
- `src/chime.cpp` — Chime
- `src/crystal.cpp` — Crystal
- `src/loom.cpp` — Loom
- `src/slice.cpp` — Slice
- `src/dr_wav.h` — Header-only WAV loader (Phase, Record, Play)
- `src/dr_flac.h` / `src/dr_flac.cpp` — Header-only FLAC loader + its implementation TU (Play)
- `src/fugue-messages.hpp` — Fugue ↔ Fugue X expander messages
- `docs/conventions/` — Cross-module conventions and required patterns

### Module Development Pattern
Each module follows the VCV Rack module pattern:
- Inherits from `Module` class
- Defines `ParamId`, `InputId`, `OutputId`, and `LightId` enums
- Implements `process()` method for audio processing
- Has corresponding widget class for UI

### Launch Checklist
**Before a module goes from hidden to shipping, and before any release is
tagged, work through `docs/conventions/launch-checklist.md`.** Every item on it
was learned by getting it wrong once. It covers the panel, the browser preview,
metadata (a tag the Library does not recognise is rejected *after* you tag the
release), documentation, and the release steps.

**Static analysis is part of it, at release time.** The VCV Library runs
cppcheck on every submission and opens an issue with what it finds, so run it
before they do — when preparing a release, **not** on every dev build:

```bash
./tools/cppcheck.sh      # exit status = findings in our own code
```

The first such report contained two real bugs that no amount of playing the
modules would have surfaced: Fill importing a bank with an uninitialised taste
struct, and a lookup table read one element past its end at a phase of exactly
1.0. Fix the *declaration* as well as the call site, and silence a false
positive rather than living with it — a checker that cries wolf on every
submission is how a real finding gets missed.

### Panel Conventions
**NEVER add virtual screws to a panel.** Signal Function Set faceplates omit the
`ScrewSilver`/`ScrewBlack` corner screws by design — do not call
`createWidget<ScrewSilver>(...)` (or any screw widget) in a `ModuleWidget`
constructor, for new modules or existing ones. This is intentional and applies
to every module in the plugin.

### Display Widget Conventions
**Any module with a custom NanoVG display widget MUST implement a `drawPreview()` method** for the browser screenshot (`ModuleWidget->module == NULL` during VCV Library thumbnail generation). Without it, the browser thumbnail shows an empty dark slab and tells nothing about what the module does.

See **`docs/conventions/browser-preview-pattern.md`** for the full pattern, examples from every shipped module, and testing instructions. Treat this as a required part of the display widget contract from day one of any new module.

### Panel Design Conventions
Panels are **code**, not Illustrator files. Rack ignores `<text>` in a panel SVG, so labels are drawn at runtime in **Figtree** (bundled in `res/fonts/`, SIL OFL) from `src/panel-style.hpp` — which also holds the palette, the single label size (4.4mm), and the label gaps. Callers state a **control's** position and the header applies the gap, so a moved control takes its label with it.

Remember that Rack draws each component over its own footprint: anything in the SVG the size of a knob or jack is invisible. The visible design is the faceplate, the dark **plates** grouping sections, the screens, and the labels.

See **`docs/conventions/panel-design.md`**.

### Panel Reticule Conventions
Panel SVGs carry **reticules** — the placement guides the art is drawn around. They are generated from the widget constructor by `tools/panel_reticules.py`, never hand-placed, so they cannot drift from the code:

```bash
python3 tools/panel_reticules.py            # every registered panel
```

Controls are drawn at **99%** of the real component as a **single shape**; screens are **full size** filled with the display blue (`#1a1a32`). The tool owns only the `Screens` and `Reticules` layers and never touches the designer's artwork.

See **`docs/conventions/panel-reticules.md`** for the rules, the component size table, and the 72dpi-vs-75dpi coordinate-space trap.

### Scale Conventions
**Any module exposing a SCALE control MUST use the shared canonical list in `src/scales.hpp`** (namespace `sfs`) — never define its own scale table. The list is append-only; reordering breaks cross-module SCALE CV compatibility and saved patches. Note, Fugue, Fugue's MetaFugue variant, Muse, Chance, and Arrange all alias `sfs::Scale` / `sfs::SCALES[]`, so a SCALE CV (1V/scale) and ROOT CV (1V/oct, semitone-quantized) are interchangeable across all of them. (Fugue was migrated onto the canonical list in 2026-07; a `scaleCanonical` JSON flag + `LEGACY_SCALE_REMAP` translate pre-migration patches.)

See **`docs/conventions/scales.md`** for the 19-scale list, struct fields (longName/shortName/museName, intervals/size, museSemis[8]), and how to add a scale.

### Drift Module Architecture
The Drift module implements:
- 4 phase-shifted LFO outputs (A, B, C, D)
- Lorenz attractor chaos system for each output
- Parameters: shape, stability, frequency, X spread, center, Y spread
- Inputs: CV control for all parameters
- Outputs: min/max values plus 4 LFO outputs

### Resources
- `res/` directory contains SVG panel designs
- `plugin.json` defines plugin metadata and module listings
- Panel SVGs are referenced by module widgets for UI rendering

## Development Notes

- The plugin uses VCV Rack SDK framework
- Module widgets are defined in the same file as the module implementation
- SVG panels in `res/` directory define the visual appearance
- Build artifacts go to `build/` and distribution files to `dist/`

## GSX - Granular Synthesis Module

### Overview
GSX is a fully implemented granular synthesis module that replicates the capabilities of Barry Truax's pioneering GSX system (1985-86). It generates dense textures from hundreds of short sound events called "grains," with real-time control over temporal, spectral, and spatial parameters.

### Implementation Status
**COMPLETE AND OPERATIONAL** - The module is fully functional and sounds very close to Barry Truax's original GSX system. All 9 parameters, 10 CV inputs, VCA control, and stereo outputs are working correctly.

### Technical Background
Granular synthesis operates in the "microsound" domain (typically 1-50ms grain durations) where changes in the time domain produce changes in the frequency/spectral domain. The module supports both quasi-synchronous granulation (which can produce pitch-like effects through amplitude modulation) and asynchronous/stochastic granulation (which creates evolving textures).

### Parameters

Each parameter has:
- Dedicated control knob
- CV input (±5V bipolar for all parameters, 0-5V unipolar for VCA)
- Range optimized for musical granular synthesis

#### 1. Frequency
- **Range**: 50 Hz to 2000 Hz
- **Default**: 130.81 Hz (C3)
- **Function**: Center frequency of generated grains
- **CV Input**: 1V/octave standard (±∞V, bipolar)
- **Notes**: Center frequency around which grains are generated; lower frequencies produce bass textures, higher frequencies create bright, piercing sounds. CV input multiplies frequency exponentially for musical pitch tracking.

#### 2. Streams
- **Range**: 1 to 20 streams
- **Default**: 10 streams
- **Function**: Number of simultaneous grain generators
- **CV Input**: ±5V = ±10 streams (bipolar)
- **Notes**: More streams = denser texture and smoother sound; fewer streams = individual grains more audible. Each stream independently generates up to 20 overlapping grains. Affects CPU load.

#### 3. Shape
- **Range**: 0.0 to 1.0 (Sine → Triangle → Sawtooth → Square)
- **Default**: 0.0 (Sine wave)
- **Function**: Grain waveform with continuous morphing
- **CV Input**: ±5V = full range (bipolar)
- **Notes**: 0.0=sine (mellow), 0.33=triangle, 0.66=sawtooth, 1.0=square (bright/rich harmonics). Smooth morphing between waveforms for complex timbres.

#### 4. Range
- **Range**: 0 Hz to 500 Hz
- **Default**: 100 Hz
- **Function**: Frequency deviation/bandwidth around center frequency
- **CV Input**: ±5V = ±500 Hz (bipolar)
- **Notes**: 0 Hz = all grains at same frequency; larger values = wider spectral spread and richer textures. Combined with Variation to control frequency randomness.

#### 5. Duration
- **Range**: 1 ms to 100 ms
- **Default**: 20 ms
- **Function**: Length of individual grains
- **CV Input**: ±5V = ±100 ms (bipolar)
- **Notes**: Shorter durations = percussive/granular quality; longer durations = smoother textures. Affects perceived timbre due to time-domain/frequency-domain relationship. Typical microsound domain is 1-50ms.

#### 6. Delay
- **Range**: 0.1 ms to 200 ms
- **Default**: 0.1 ms (minimum, Density is primary control)
- **Function**: Manual override for time between grains
- **CV Input**: ±5V = ±200 ms (bipolar)
- **Notes**: Overrides Density when >0.2ms. Lower values = dense/continuous texture; larger values = rhythmic/detached grains. Interacts with Duration to create amplitude modulation effects in quasi-synchronous mode.

#### 7. Density
- **Range**: 1 to 1000 grains per second (per stream)
- **Default**: 100 grains/sec
- **Function**: Primary control for grain generation rate
- **CV Input**: ±5V = ±1000 grains/sec (bipolar)
- **Notes**: Primary timing control (converted to delay internally: delay = 1/density). Higher density = more continuous sound; lower density = sparse, scattered grains. Each stream generates grains at this rate.

#### 8. Variation
- **Range**: 0% to 100%
- **Default**: 50%
- **Function**: Amount of stochastic variation applied to grain parameters
- **CV Input**: ±5V = ±100% (bipolar)
- **Notes**: 0% = quasi-synchronous (regular/pitched); 100% = fully asynchronous (stochastic cloud texture). Adds randomness to frequency (with Range), duration, timing, and spatial position. Uses exponential scaling below 30% for tighter control.

#### 9. Spread
- **Range**: 0% to 100%
- **Default**: 50%
- **Function**: Stereo width and spatial distribution
- **CV Input**: ±5V = ±100% (bipolar)
- **Notes**: 0% = mono (center); 100% = wide stereo. Controls random panning of individual grains across the stereo field using equal-power panning.

### Inputs/Outputs

#### VCA Input
- **Range**: 0-5V (unipolar)
- **Function**: Linear VCA control over final output
- **Notes**: 0V = silence, 5V = full volume. Applied after intelligent gain scaling. Use for envelope control, ducking, or external amplitude modulation.

#### Left Output
- Stereo left channel output
- VCV Rack level: ±10V
- Contains grain streams with equal-power panning

#### Right Output
- Stereo right channel output
- VCV Rack level: ±10V
- Contains grain streams with equal-power panning

### Synthesis Modes

#### Quasi-Synchronous Mode
- Achieved with low Variation and regular Delay values
- Creates pitch through amplitude modulation
- Grain duration determines modulation frequency (e.g., 20ms = 50Hz modulator)
- Can produce tonal effects despite being grain-based synthesis

#### Asynchronous Mode
- Achieved with high Variation values
- Creates stochastic, cloud-like textures
- No clear pitch relationship
- More typical "granular synthesis" sound

### Panel Layout (Implemented)
- **Width**: 3-column design at X positions: 10.16mm, 30.48mm, 50.8mm
- **Knobs (3 rows)**:
  - Row 1 (Y=28.69mm): Frequency, Streams, Shape
  - Row 2 (Y=59.17mm): Range, Duration, Delay
  - Row 3 (Y=89.65mm): Density, Variation, Spread
- **CV Inputs (positioned directly below knobs)**:
  - Row 1 (Y=41.39mm): Frequency CV, Streams CV, Shape CV
  - Row 2 (Y=71.87mm): Range CV, Duration CV, Delay CV
  - Row 3 (Y=102.35mm): Density CV, Variation CV, Spread CV
- **Bottom Row (Y=120.13mm)**: VCA input (10.16mm), Left output (40.64mm), Right output (50.8mm)
- **Panel**: res/gsx.svg

### Implementation Details

#### Core DSP Architecture (src/gsx.cpp)
- **20 streams** maximum (MAX_STREAMS = 20)
- **20 grains per stream** (GRAINS_PER_STREAM = 20) for dense textures
- Each grain has **separate envelope and waveform phases** (critical for correct sound):
  - `envelopePhase`: 0-1 over grain lifetime (controls Hann window)
  - `wavePhase`: 0-1, wraps continuously (oscillates at grain frequency)
- Sample rate: 48kHz (VCV Rack standard)

#### Grain Structure
```cpp
struct Grain {
    bool active;
    float envelopePhase;  // 0-1 over grain duration
    float wavePhase;      // 0-1, wraps for oscillation
    float frequency;      // Hz
    float duration;       // seconds
    float pan;           // 0=left, 1=right
};
```

#### Grain Envelope
- **Hann window** used for smooth grain envelope: `0.5 * (1 - cos(2π * phase))`
- Prevents clicks and artifacts
- Applied to grain amplitude based on `envelopePhase`

#### Waveform Generation
- Continuous morphing between 4 waveforms: Sine → Triangle → Sawtooth → Square
- Shape parameter (0-1) controls blend
- Each grain oscillates at its assigned frequency throughout its duration
- Waveform phase advances independently from envelope phase

#### Intelligent Gain Scaling
- Automatic gain compensation based on active grain count
- Formula: `gain = clamp(1.0 / sqrt(activeGrainCount * 0.5), 0.15, 1.0)`
- Prevents clipping with many grains while maintaining presence with few grains
- Scales from 1.0 (1 grain) to 0.15 (100+ grains)
- VCA gain applied after this scaling

#### Temporal Accuracy
- Sample-accurate grain timing for quasi-synchronous mode
- Density primary control: `delay = 1/density`
- Delay parameter overrides when >0.2ms
- Variation adds stochastic offset to timing (exponential scaling below 30%)

#### Spatial Distribution
- Per-grain random panning (not per-stream)
- Equal-power panning law: `leftGain = sqrt(1-pan)`, `rightGain = sqrt(pan)`
- Spread parameter controls width of random distribution

#### Stream Management
- Each stream independently schedules grains
- Round-robin grain allocation within each stream (up to 20 concurrent grains)
- No grain stealing - grains must finish naturally

### Parameter Interaction Examples

**Dense Texture (Cloud):**
- Streams: 15-20
- Duration: 10-30ms
- Delay: 0-5ms (or Density: 200-500 grains/sec)
- Variation: 60-100%
- Spread: 80-100%

**Quasi-Synchronous (Pitched):**
- Streams: 8-12
- Duration: 20ms (for 50Hz modulation)
- Delay: 20ms (matches duration)
- Variation: 5-15%
- Spread: 30-50%

**Sparse Granular (Detached grains):**
- Streams: 3-8
- Duration: 5-15ms
- Delay: 50-200ms
- Variation: 40-80%
- Spread: 60-100%

**Rhythmic Pulse:**
- Streams: 10-15
- Duration: 10-20ms
- Delay: 100-150ms (regular values)
- Variation: 0-10%
- Spread: 20-40%

### Historical Context
Based on Barry Truax's GSX system developed in 1985-86 for the DMX-1000 digital signal processor. First real-time granular synthesis implementation, used to create the seminal work "Riverrun" (1986). This VCV Rack implementation preserves the core algorithmic approach while adapting it to the modular synthesis paradigm with full voltage control.

### Sound Quality
The implemented module sounds **very close** to Barry Truax's original GSX system. The combination of proper envelope/waveform phase separation, Hann window envelopes, intelligent gain scaling, and per-grain random panning creates authentic granular textures in both quasi-synchronous and asynchronous modes.

### Development History
- Initial implementation with all 9 parameters and CV inputs
- Critical bug fix: Separated envelope phase from waveform phase (grains now oscillate at their frequency throughout their duration, not just one cycle)
- Added VCA input for external amplitude control
- Tuned variation scaling (exponential below 30% for quasi-synchronous mode)
- Optimized CV input ranges to ±5V standard (0-5V for VCA)
- Changed default frequency from A4 (440 Hz) to C3 (130.81 Hz)
- Implemented per-grain random panning for authentic stereo spread

### Possible Future Enhancements
- FM synthesis grain mode (add Modulation Index and C:M Ratio parameters)
- Sample-based grains (load audio files for grain source)
- Alternative envelope shapes (Gaussian, Tukey, custom)
- Grain reverse playback option
- Additional spatial modes (circular panning, motion paths)

## Phase - Dual Sample Looper

### Overview
Phase is a dual sample looper inspired by Steve Reich's phase compositions ("It's Gonna Rain," "Piano Phase"). Two loops play the same or different audio samples with independent "sleep" parameters that create gradual phase drift between the loops. Each loop has a mode switch choosing between **Sleep** (silence gap after each cycle) and **Rotate** (continuous content drift like tape machines at slightly different speeds). The module supports forward and reverse playback, per-loop panning, transient detection with clock-triggered jumps, WAV cue point support, adjustable loop regions, and a VCA anti-click mode.

### Implementation Status
**COMPLETE AND OPERATIONAL** - The module is fully functional with all parameters, CV inputs, waveform display, transient detection, and loop region controls working correctly.

### How Phase Drift Works
Each loop has a bipolar "sleep" parameter (-500ms to +500ms) and a mode switch:

**Sleep Mode (SLP):** After a loop completes one cycle, it waits the sleep duration before restarting. Positive values add silence; negative values cut the loop short. If Loop A has 10ms sleep and Loop B has 0ms, Loop A's effective period is `sample_length + 10ms` while Loop B's is just `sample_length`. They gradually drift apart.

**Rotate Mode (ROT):** The loop plays continuously with no gaps. The sleep parameter controls a tiny speed offset that causes the content to gradually rotate within the loop — like two tape machines running at slightly different speeds. The read position drifts by `sleepMs` worth of samples per loop cycle. This creates a subtle pitch shift proportional to the drift rate (e.g., 10ms over 5s = 0.2% = ~3.5 cents — inaudible). Negative sleep rotates in the opposite direction.

### Parameters

#### Per Loop (x2: Loop A and Loop B)

##### 1. Sleep
- **Range**: -500 to +500 ms
- **Default**: 0 ms
- **Function**: Controls phase drift amount and direction
- **CV Input**: ±5V, 50ms/V (bipolar)
- **Notes**: In Sleep mode: positive = silence gap, negative = loop ends early. In Rotate mode: positive = drift forward, negative = drift backward. This is the core phasing mechanism.

##### 2. Speed
- **Range**: -4x to +4x
- **Default**: 1x (center of knob is 0/stopped)
- **Function**: Playback speed and direction
- **CV Input**: ±5V, 0.8x/V (bipolar)
- **Notes**: Negative values play in reverse. 0 = stopped. Speed also affects the effective loop period, providing a second axis for phase drift. Center detent is at 0 (stopped), default is 1x.

##### 3. Pan
- **Range**: -1 (full left) to +1 (full right)
- **Default**: 0 (center)
- **Function**: Stereo panning position for this loop
- **CV Input**: ±5V, 0.2/V (bipolar)
- **Notes**: Uses equal-power panning law (cos/sin). Allows positioning each loop in the stereo field independently.

##### 4. Mode Switch (SLP/ROT)
- **Type**: CKSS toggle switch
- **Function**: Selects between Sleep mode (up) and Rotate mode (down) per loop

### Inputs

#### Per Loop (x2)
| Input | Range | Function |
|-------|-------|----------|
| Sleep CV | ±5V | Modulates sleep time (50ms/V) |
| Speed CV | ±5V | Modulates playback speed (0.8x/V) |
| Pan CV | ±5V | Modulates stereo pan (0.2/V) |
| CLK | Trigger | Jump playhead to next detected transient |
| START | 0-10V | Loop start position (0-100% of sample) |
| LEN | 0-10V | Loop length (0-100% of remaining sample after start) |

#### Global
| Input | Range | Function |
|-------|-------|----------|
| SYNC | Trigger | Reset both loops to their start positions |
| PLAY GATE | Gate | High (>=1V) = play, overrides button state |

### Outputs
| Output | Function |
|--------|----------|
| LEFT | Stereo left mix of both loops |
| RIGHT | Stereo right mix of both loops |

### Controls

#### Play Button
- Green LED latch button
- Toggles play/stop state on click
- When PLAY GATE CV is connected, gate overrides the button state
- Module outputs silence when stopped

#### Sync Button
- Momentary push button
- Resets both loops to their start positions
- Works alongside the SYNC CV input (either triggers a reset)

### Waveform Display

The display occupies the top portion of the module and shows:
- **Top half**: Sample A waveform (blue)
- **Bottom half**: Sample B waveform (orange)
- **Playhead**: White vertical line showing current position per loop
- **Transient markers**: Subtle vertical lines spanning waveform height
- **Loop handles**: Draggable bracket-style start/end handles (solid filled, no transparency stacking)
- **Dim overlay**: Regions outside the active loop are darkened
- **Origin line**: Semi-transparent white line showing where the original sample start has drifted to in rotate mode
- **Rotated waveform**: In rotate mode, the waveform image rotates to match the audio content

#### Loop Region Handles
- Bracket-style handles with 1px vertical bar and horizontal ticks at top/bottom
- Start handles bracket right, end handles bracket left
- Drag sensitivity accounts for zoom level via `getAbsoluteZoom()`
- When START/LEN CV inputs are connected, they override the handle positions
- Loop regions are persisted with patch save/load

### Sample Loading

- Right-click menu: "Load Sample A" / "Load Sample B"
- WAV files supported (mono or stereo, any sample rate — resampled to 48kHz on load)
- Stereo files are mixed down to mono; stereo placement comes from the Pan control
- If only Sample A is loaded, it cascades to Sample B automatically
- Loading Sample B explicitly breaks the cascade
- "Clear Sample A" / "Clear Sample B" removes loaded samples
- Maximum sample length: 10 minutes (28,800,000 samples at 48kHz)
- File paths persist with patch save/load via JSON
- **WAV cue point support**: If the WAV file contains embedded cue points, they are used as transient markers instead of auto-detection. Cue positions are resampled if the file isn't 48kHz. Re-detect Transients from the context menu overrides cues with auto-detection.

### Transient Detection

#### Algorithm
Energy-based onset detection computed once per sample load:
1. RMS energy computed in 1024-sample windows with 256-sample hop
2. High-frequency emphasis via sample differencing (catches transients in noise)
3. Half-wave rectified onset detection function
4. Adaptive threshold based on local mean energy over 30-frame context window
5. Local peak picking with ±2 frame neighborhood
6. Minimum gap enforcement between detected transients
7. Absolute energy floor (0.005) prevents false triggers on quiet/silent passages

#### Context Menu Controls
- **Re-detect Transients**: Re-run detection with current settings (overrides WAV cue points)
- **Sensitivity**: High (0.15), Medium/default (0.7), Low (0.95) — maps to threshold range 1.0-12.0
- **Min Transient Gap**: 10ms (fast), 50ms (medium), 100ms/default (slow)

#### Clock Input Behavior
On rising edge of CLK input, the playhead jumps to the next transient after the current position (within the active loop region). If at the end, wraps to the first transient in the region.

### VCA Mode (Anti-Click)

- **Default**: Enabled
- **Context menu**: "VCA Mode (anti-click)" toggle
- When enabled, applies a 1ms attack/release envelope around all discontinuities:
  - Loop restarts (forward and reverse wrap-around)
  - Transient jumps via clock trigger
  - Sync resets
  - Sleep wake-up transitions
- Eliminates clicks from playhead discontinuities
- Implementation: fade-out (1ms) → execute jump → fade-in (1ms)

### Panel Layout (20HP = 101.6mm wide)

All positions in mm, used with `mm2px()`:

```
WAVEFORM DISPLAY: position (5.8, 14), size 90mm x 24mm

LOOP A:
  Knobs  Y=50:    Sleep(15.24)   Speed(35.56)   Pan(55.88)
  CVs    Y=62:    SleepCV(15.24) SpeedCV(35.56) PanCV(55.88)
  Jacks  Y=50:    ClkA(76.2)     StartA(86.36)  LenA(96.52)
  Switch Y=62:    ModeA(86.36)

LOOP B:
  Knobs  Y=78:    Sleep(15.24)   Speed(35.56)   Pan(55.88)
  CVs    Y=90:    SleepCV(15.24) SpeedCV(35.56) PanCV(55.88)
  Jacks  Y=78:    ClkB(76.2)     StartB(86.36)  LenB(96.52)
  Switch Y=90:    ModeB(86.36)

BOTTOM ROW Y=110:
  Play(15.24)  PlayGate(25.4)  Sync(45.72)  SyncCV(56)  Left(76.2)  Right(91.44)
```

### Implementation Details

#### Core DSP (src/phase.cpp)
- **Double-precision playhead**: Prevents cumulative drift at fractional speeds over long playback
- **Linear interpolation**: Sub-sample accuracy for non-integer speed values
- **Mono mixdown on load**: Simplifies DSP; stereo placement via per-loop pan control
- **Precomputed waveform overview**: 512-point peak array per sample, no per-frame buffer scanning
- **Equal-power panning**: cos/sin law from bipolar [-1,+1] pan position
- **Continuous drift (rotate mode)**: Read position advances at `speed + (sleepSamples/regionLength)` per sample — no discrete jumps, no crossfade needed

#### Data Structures
```cpp
struct SampleData {
    vector<float> samples;        // Mono float samples at 48kHz
    size_t length;                // Number of samples
    string filePath, fileName;    // For persistence and display
    vector<size_t> transients;    // Precomputed transient positions
    vector<float> waveformMini;   // 512-point peak array for display
    bool loaded;
    bool hasCuePoints;            // true if transients from WAV cue chunk
    float loopStart, loopEnd;     // Normalized 0-1 loop region
};

struct LoopState {
    double playhead;              // Current position (double precision)
    bool sleeping;                // In post-loop silence
    float sleepRemaining;         // Sleep countdown in seconds
    SchmittTrigger clockTrigger;
    float envelope;               // VCA mode anti-click envelope (0-1)
    bool ramping;                 // In fade transition
    double jumpTarget;            // Deferred jump destination
    double rotationOffset;        // Accumulated drift in rotate mode
};
```

#### JSON Persistence
Saves and restores: file paths for both samples, explicit-B flag, play state, transient sensitivity, min gap, VCA mode, and loop start/end regions for both samples.

#### Dependencies
- **dr_wav.h**: Header-only WAV loader (included in src/, `#define DR_WAV_IMPLEMENTATION` in phase.cpp). Opened with `drwav_init_file_with_metadata()` to read cue points.
- **osdialog**: File open dialogs (provided by VCV Rack SDK)
- **NanoVG**: Waveform display drawing (provided by VCV Rack SDK)

### Patch Ideas

**Steve Reich Phase Drift:**
- Load same sample in both loops
- Sleep A: 5-10ms, Sleep B: 0ms
- Speed both at 1x
- Pan A left, Pan B right
- Listen as patterns gradually shift

**Tape Machine Drift (Rotate):**
- Load same sample, set both to Rotate mode
- Sleep A: 5ms, Sleep B: 0ms
- Continuous seamless drift with no gaps
- Content gradually rotates — subtle pitch shift adds to the effect

**Reverse Texture:**
- Speed A: 1x, Speed B: -0.5x
- Same sample, different loop regions
- High variation creates evolving textures

**Transient Slicer:**
- Load rhythmic material
- High sensitivity transient detection
- Clock both loops from an external sequencer at different rates
- Each clock pulse jumps to the next transient

**Granular-Style Scanning:**
- Use LFO on START CV to slowly scan through the sample
- Short LEN CV value for small loop windows
- Different LFO rates on A and B for complex interplay

**Bidirectional Drift:**
- Sleep A: +10ms, Sleep B: -10ms
- Loops drift in opposite directions simultaneously

## Meter - Musical Clock with Time Signature

### Overview
Meter is a musical clock module designed to be the master clock for a Beat-driven rhythm rig. Unlike most VCV clocks, it understands musical structure: time signature, bar boundaries, and beat subdivisions. Six gate outputs cover the common subdivisions (Bar, Quarter, Eighth, Sixteenth, Quarter Triplet, Eighth Triplet), each with independent enable and swing controls — so the user can shape the macro feel of an entire patch from one place. The intent is that one Meter drives many Beat instances, each per-instrument, each potentially using a different swung/unswung subdivision as its clock.

### Implementation Status
**COMPLETE AND OPERATIONAL** — all subdivision outputs, per-output enable + swing (with mockup-aligned blue/orange display), external clock sync, time signature CV, and reset out are working. Display shows BPM, time signature, bars-since-reset, sync indicator, plus per-output hit indicator rows with swing ghost markers and a position tracker.

### Parameters

| Param | Range | Default | Notes |
|---|---|---|---|
| BPM (huge knob) | 30–300 | 120 | Quarter notes per minute (DAW convention) |
| Numerator (snap knob) | 1–16 | 4 | Top of time signature |
| Denominator (config-switch) | indices 0–5 → 1, 2, 4, 8, 16, 32 | 2 (=4) | Bottom of time signature |
| Run (light latch) | momentary | — | Play/stop toggle |
| Reset (button) | momentary | — | Resets bar to 1, position to 0; fires Reset OUT |
| Per-output Enable (×6) | latch w/ green LED | on | Mute that subdivision output |
| Per-output Swing (×5, no swing on BAR) | -0.5 .. +0.5 | 0 | Standard convention: positive = off-beats delayed |

### Inputs
- **BPM CV** (~27 BPM/V)
- **Numerator CV / Denominator CV** (CV stepping)
- **Run gate** (overrides Run button when patched)
- **External clock** (overrides internal BPM; PPQN configurable via context menu: 1/2/4/8/12/16/24)
- Per-output **Enable CV** (×6, gate input that overrides the latch)
- Per-output **Swing CV** (×5, ±5V → ±50%)

### Outputs
All gate outputs are 1ms 10V pulses via `dsp::PulseGenerator`:
- **BAR** (downbeat per bar)
- **QUARTER** / **EIGHTH** / **SIXTEENTH**
- **QUARTER TRIPLET** / **EIGHTH TRIPLET**
- **RESET OUT** (1ms trigger; fires when the Reset button is pressed — Meter is the master, downstream modules receive reset via this jack)

### Display

Top status line:
- Left: current BPM (numeric)
- Sync indicator (right of BPM, only when EXT CLOCK is patched): small dot, dim orange at rest, flashes bright on each external clock pulse, decays over ~100ms
- Center (large): time signature `4/4`, with optional pending `→ 7/8` to the right when a change is queued
- Right: `BAR N` counter, increments on each bar wrap, resets to 1 on Reset

Six per-output hit indicator rows above the position tracker. Each row shows tick marks for that output's pulse positions across the bar, with:
- Swing ghosts (dim ticks at un-swung positions) plus connector lines from ghost → actual position
- A pulse flash that lerps blue → orange when the pulse fires (decays over ~100ms)
- Disabled rows render in dim purple

Position tracker at the bottom: one cell per sixteenth-note in the bar, with the current sixteenth highlighted orange, beat boundaries shown in mid-purple, and other cells dim purple.

### DSP Design

#### Per-subdivision phase accumulators
Each subdivision (Quarter, Eighth, Sixteenth, Quarter Triplet, Eighth Triplet) maintains its own `samplesSinceX` accumulator and `pulseCountX` counter. Per sample:
- `samplesPerQuarter = 60 * sampleRate / effectiveBpm`
- `eTarget = swingAdjustedPeriod(pulseCountX, basePeriod, activeSwing[X])`
- When `samplesSinceX >= eTarget`, fire the pulse, subtract eTarget, increment pulse count

#### Bar tracking
SIXTEENTH drives `sixteenthCount`. When it reaches `sixteenthsPerBar = numerator * 16 / denominator`, the bar wraps:
- `sixteenthCount = 0`
- `barsSinceReset++`
- BAR pulse fires
- Triplet phases reset to 0 (so triplets always realign with the downbeat)
- Pending swing values copied to active swing (per-bar latching — see "Swing latching" below)

#### Swing math
```cpp
// pulseCount = pulses already fired since reset.
// Next pulse to fire is pulse (pulseCount + 1).
// Off-beats are pulses 1, 3, 5... (odd index).
bool nextIsOffBeat = (pulseCount % 2) == 0;
return basePeriod * (nextIsOffBeat ? (1 + swing) : (1 - swing));
```
Each pair of (on-beat → off-beat → on-beat) periods sums to exactly 2*base, so on-beats always land on the grid regardless of swing amount. Swing range is ±0.5 (off-beat pulled all the way to the next on-beat at +0.5, all the way to the previous on-beat at -0.5).

#### Swing latching (CRITICAL)
Mid-period swing changes would corrupt the accumulator (the threshold being raced toward changes mid-race), causing notes to fire early or get swallowed. To prevent this:
- `pendingSwing[i]` is read from knob+CV every sample
- `activeSwing[i]` is what `swingAdjustedPeriod` actually uses
- On bar boundary (and on Reset), `activeSwing = pendingSwing`
- On the very first process call, also commit (so initial knob position takes effect immediately)
- Display reflects `pendingSwing` (so the visualization stays responsive while the audio waits for the bar boundary)

#### External clock sync
On each rising edge of EXT CLOCK:
- Measure samples since last pulse → `samplesPerQuarter = samples_between * ppqn`
- `measuredBpmRaw = clamp(60 * sampleRate / samplesPerQuarter, 30, 300)`
- One-pole LPF (coefficient 0.1) toward `measuredBpm`
- Sync indicator flash set to 1.0
- When EXT is connected and has a measurement, `effectiveBpm = measuredBpm`; otherwise `effectiveBpm = bpmKnob + bpmCV`

#### Pulse flash (display only)
Each output has a `pulseFlash[i]` brightness (0..1, decays over 100ms), `pulseFlashIdx[i]` (which tick within the bar most recently fired), and `pulseInBar[i]` (running counter, reset on bar wrap). When BAR fires on the same sample as another subdivision's downbeat, the flash indices for those subdivisions are post-corrected to point at tick 0 of the new bar (rather than the last tick of the old bar).

### Panel Layout (18HP = 91.44mm)

Two-column left side (x=8, x=22):
- BPM huge knob (centered between columns at x=15)
- Y=58: EXT clock | BPM CV
- Y=72: NUM knob | DEN knob
- Y=84: NUM CV | DEN CV
- Y=98: RUN latch | RST button
- Y=110: RUN gate (in) | RESET OUT (with dark plate behind)

Right side: 6 output rows at y=44, 57, 70, 83, 96, 109. Each row has:
- Enable button (latch+LED) at x=44
- Enable CV at x=53
- Swing trimpot at x=65 (omitted on BAR row)
- Swing CV at x=74 (omitted on BAR row)
- Output jack at x=86 (with dark plate behind for visual separation)

### Context Menu
- External Clock PPQN selector (1, 2, 4, 8, 12, 16, 24)
- "Apply time signature changes immediately" toggle (default off — changes queue for next bar)
- "Reset on play" toggle (default off — Run after Stop resumes from current position)
- "Detected: NN.N BPM" label when ext clock is connected and measuring

### Persistence
JSON saves: running state, ext clock PPQN index, applyTimeSigImmediately, resetOnPlay, outputEnabled[6], barsSinceReset.

## Beat - Per-Voice Pattern Sequencer

### Overview
Beat is a single-voice pattern sequencer designed to be paired with Meter (or any clock+bar source). One Beat instance = one drum/voice. Eight patterns × sixteen steps each, with per-step velocity, accent, and probability. Per-pattern length (1–16) and per-pattern repeat count (1–8 bars) define the macro structure. Most editing happens on the screen — the panel is a narrow 10HP with just the display + jacks.

### Implementation Status
**COMPLETE AND OPERATIONAL** — full edit modes (STEPS / VEL / ACC / PROB), drag-to-paint/scrub across all sequential elements, double-click to toggle pattern active state, on-screen length and repeat count controls, blue/orange Beat-design palette, connector rails between mode tabs/step-grid and pattern-selector/repeats-bar, and persistent state.

### Per-pattern Data (×8 patterns)
- `bool steps[16]` — gate on/off per step
- `float velocities[16]` — 0..1 per step (drives 0..10V VEL output)
- `bool accents[16]` — accent flag per step (drives 1ms ACC pulse)
- `float probabilities[16]` — 0..1 chance the step actually fires when reached
- `int length` — 1..16 (step count per loop)
- `int repeats` — 1..8 (number of bars to play this pattern before advancing)
- `bool active` — included in the pattern rotation

### Inputs
- **CLOCK** — advances the step counter
- **BAR** — advances to the next active pattern (with `repeats` honored)
- **RESET** — returns to first active pattern, step 0
- **MUTE** (gate ≥1V) — silences all three outputs

### Outputs
- **GATE** — 1ms 10V pulse on each fired step
- **VELOCITY** — sample-and-hold CV 0..10V (the previous step's velocity stays held until the next fire)
- **ACCENT** — 1ms 10V pulse on accented steps

### Display Layout (mockup-aligned)

Mockup uses a 174 × 155 unit display (= 46 × 41 mm). Internal coordinates use a `s = w / 174` scale factor for unit conversion. Display origin is at panel `(2.4, 12)` mm.

Top to bottom:
1. **Mode tabs row** (y=8, height 18): 4 cells of 38×18 at x = 7, 47, 87, 127 — `STEPS / VEL / ACC / PROB`. Selected tab = dark blue (`#0D5986`); inactive = dim purple.
2. **Top connector rail** at y=32 (between mode tabs and step grid) — horizontal `#0D5988` line spanning x=7..165, with a short vertical stem from the active mode tab's center.
3. **Step grid** (y=35..73): 2 rows × 8 cols of 18×18 cells. Cells colored:
   - Out of length → very dim (`#1A1A32`)
   - Active step → blue (`#0097DE`)
   - Active step + currently-playing → orange (`#EC652E`)
   - Inactive step at beat boundary (idx % 4 == 0) → mid purple (`#4A4A66`)
   - Inactive step elsewhere → dim purple (`#35354D`)
4. **Length dots** (y=75): 16 small 8×8 dots at x = 7+i*10. Lit (blue) for `i < length`, dim for the rest.
5. **PATTERN label** (y=103, baseline): white text matching mode tab font size, left-aligned.
6. **Pattern selector** (y=111..129): 8 cells of 18×18 with pattern numbers 1–8 slightly above center. Loop-count dots row at the bottom of each cell, centered horizontally — N dots where N = pattern's repeats. The dot at `currentBar - 1` lights bright on the playing pattern; others stay dim.
7. **Bottom connector rail** at y=134.5 (between pattern selector and repeats bar) — same style as the top rail, with a stem from the edit pattern's center.
8. **Repeats bar** (y=137..145): 8 cells of 18×8. Cell colors: orange for current playhead bar, blue for in-range cells (`i < reps`), dim purple for out-of-range.

### Edit Mode Behaviors

**STEPS mode**: Click toggles a step on/off. Drag paints subsequent cells with the same new state (only within current pattern length — drag won't extend length, but click can).

**VEL mode**: Click a cell to set velocity by Y position within the cell (top = 1.0, bottom = 0.0). Vertical drag adjusts further. Auto-enables the step. Cell renders a bottom-up white overlay sized to velocity (60% white in VEL mode, 10% white as a hint in STEPS/ACC modes — skipped entirely in PROB mode).

**ACC mode**: Click toggles the accent flag (auto-enables the step). Drag paints. Accent shows as an unfilled white circle at the cell center — full opacity in ACC mode, 10% opacity hint elsewhere.

**PROB mode**: Same vertical-drag behavior as VEL but writes to `probabilities[]`. Fired probabilistically in DSP via `random::uniform() >= probabilities[step]`. Renders a 60% white bottom-up overlay (only in PROB mode — no faint hints in other modes to avoid clutter).

### Pattern Selector Interactions
- **Left-click**: select for editing (also drag across cells to scrub edit pattern)
- **Double-click**: toggle pattern active/inactive (replaced the previous right-click for better discoverability)
- Inactive patterns are skipped in the rotation

### Length Dots / Repeats Bar Interactions
- **Click** any length dot → set length to that index + 1
- **Drag** across length dots → scrub length 1..16
- **Click** any repeats cell → set repeats to that index + 1
- **Drag** across repeats cells → scrub repeats 1..8
- Scroll-wheel over a pattern cell → adjust that pattern's repeats (alternative shortcut)

### DSP Logic

#### Bar / clock coincidence
Meter typically fires BAR and downbeat-EIGHTH/QUARTER/SIXTEENTH on the same sample. Beat collapses these into a single event: if BAR fired this sample OR if BAR voltage is currently high (still within its 1ms pulse window), CLOCK is suppressed. This handles either-direction sub-sample drift between Meter outputs.

#### Pattern advance
- BAR pulse: `currentBar++`. If `currentBar > repeats`, advance to `nextActivePattern(playPattern)` and `currentBar = 1`. Reset `playStep = 0`. Fire step.
- CLOCK pulse: `playStep = (playStep + 1) % length`. Fire step if active and probability check passes.

#### "Advance only on bar trigger" (default ON)
Context menu toggle. When ON (default), pattern advances only happen on BAR pulse — even if BAR isn't patched, the pattern just loops the same one indefinitely. When OFF (legacy fallback), pattern wrap also advances when BAR isn't connected.

#### Fire logic
```cpp
void fireStepIfActive() {
    if (!steps[playStep]) return;
    if (random::uniform() >= probabilities[playStep]) return;
    gatePulse.trigger(0.001f);
    currentVelocity = clamp(velocities[playStep], 0.f, 1.f);
    if (accents[playStep]) accentPulse.trigger(0.001f);
}
```

### Panel Layout (10HP)
- Display: x=2.4, y=12, 46mm × 41mm
- Inputs row at y=80: CLK (x=8), BAR (x=20), RST (x=32), MUTE (x=44)
- Outputs row at y=110 (with dark plates): GATE (x=10), VEL (x=25.4), ACC (x=40.8)

### Context Menu
- "Advance only on bar trigger" (default ON) — see DSP section above
- Patterns submenu:
  - Randomize current pattern steps (50% density, doesn't touch velocity/accent/probability)
  - Clear current pattern
  - Clear all patterns

### Persistence
JSON saves: editPattern, editMode, playPattern, playStep, currentBar, advanceOnBarOnly, and per-pattern: active, length, repeats, steps[16], velocities[16], accents[16], probabilities[16].

### Default State
On Initialize: only pattern 1 active (enable more via double-click or the per-cell right-click menu). `advanceOnBarOnly = true`. EditMode = STEPS, EditPattern = 0. Pattern selector right-click menu: Enable/Disable, Copy pattern, Paste pattern (clipboard shared across instances; paste preserves the target's active state). Same for Note.
- Creates expanding then contracting phase relationships