# Figma panel template

Panels are still code — the geometry comes out of the widget constructor and the
labels are drawn at runtime. What Figma is for is the step *before* that:
deciding where things go, on a grid fine enough to place them and coarse enough
to keep the layout honest.

```bash
python3 tools/figma_panel_template.py            # design/figma/*.svg
```

Drag the files onto a Figma page. Each becomes a frame at the exact panel size.

## Why the files are at 4×

Rack's `mm2px()` is **75dpi**, and 1HP is 5.08mm — so in Rack's own space 1HP is
exactly 15 units and a quarter is exactly **3.75**. Illustrator takes that
happily. Figma will not: its layout-grid and nudge fields are integer-only and
round 3.75 to 4, which is a 6.7% error compounding across the panel.

So the files are emitted at **4×**, where the quarter is 15 and a whole HP is 60.
Nothing downstream cares what the unit is. Rack scales a panel by its
`width`/`height` **in mm**, and `panel_reticules.py` reads each file's own
`viewBox ÷ width_mm` — the same mechanism that already lets 72dpi Illustrator
artwork and 75dpi generated artwork coexist.

| | mm | 1× (Rack) | **4× (Figma)** |
|---|---|---|---|
| ¼HP — the design grid | 1.27 | 3.75 | **15** |
| ½HP | 2.54 | 7.5 | 30 |
| **1HP — the layout grid** | 5.08 | 15 | **60** |
| panel height (3U) | 128.5 | 379.43 | 1517.72 |
| 25 usable rows | 127.0 | 375 | 1500 |
| the foot remainder | 1.5 | 4.43 | 17.72 |

Panel width is `HP × 60`: 4HP → 240, 8 → 480, 12 → 720, 16 → 960, 20 → 1200,
26 → 1560, 32 → 1920. Reading a position back is `x ÷ 60` = the `hp(n)` the
widget states — an easier division than `÷ 15` with quarters in it.

`--scale 1` emits Rack's native space instead, for Illustrator. `--scale 8` puts
**eighths** on 15 (1HP = 120) if a panel ever needs them. The scale is recorded
nowhere: `--fixup` recovers it from the panel height, which is 128.5mm on every
3U panel ever made.

**The quarter is for drawing, not for placing.** Controls still land on 1HP
intersections, because that is what the widget code says — `hp(6)`, not
`hp(6.25)`. The quarter grid exists so plate edges, hairlines, screen bounds and
optical nudges have somewhere to sit between the cells.

## Components are sized in whole cells

A real component is not a grid multiple — a trimpot is 71.42 units at 4×, a jack
94.8 — so a reticule at its true size lands its edges nowhere in particular. In
the design file every component is therefore **rounded to a whole ¼HP cell**:
trimpot 75 (5 cells), jack 90 (6), knob 120 (8), huge knob 210 (14). A control
centred on the 1HP grid then has its edges on the ⅛HP grid, which the guides
layer draws as a third, fainter tier. Nothing else is that tier's business —
controls never sit on it.

Rounding is always **outward**. Rounding to the nearest cell fits tighter — six
of the sixteen dimensions land within 5.5% — but it puts those six *under* the
real component, and a reticule smaller than its component vanishes underneath it
in the rack, taking any art drawn flush with it. Outward costs a jack 6 → 7 cells
(+10.8%) and a `RoundSmallBlackKnob` 6 → 7 (+15.8%); two jacks on the standard
2HP pitch still keep a whole cell between their guides. `--round near` takes the
tighter fit instead.

`--snap 2` sizes in whole *half*-HP, putting every edge on the quarter grid
itself. It is the stricter fit and the more expensive one — a trimpot goes to 60
and a `VCVButton` to 60, both *inward*. Use it only if edges-on-the-placement-grid
is worth more than faithful footprints.

## Label gaps come from the component

`panel-style.hpp` states three fixed centre-to-centre gaps — knob 5.6mm, jack
5.4, trimpot 4.4 — one per *helper*, not per component. Since `knob()` labels
every size of knob, the clearance those produce is not consistent at all.
Measured from the real edge to the bottom of the caps (Figtree's middle sits
0.35em above the baseline, so an all-caps label extends 1.155mm below its point
at `TYPE_LABEL`):

| | code gap | clearance |
|---|---|---|
| trimpot | 4.40 | +0.22mm |
| jack | 5.40 | +0.23mm |
| RoundSmallBlackKnob | 5.60 | +0.61mm |
| RoundBlackKnob | 5.60 | **−0.36mm** |
| RoundLargeBlackKnob | 5.60 | **−1.65mm** |
| RoundHugeBlackKnob | 5.60 | **−4.68mm** |

Anything above a small knob has its label *inside* the knob. Nothing has hit it
yet only because no shipped widget calls `knob()`.

The design file instead derives the gap from the component: **half its snapped
size, plus one cell.** One rule, every control, and because the size is a whole
number of cells the label centre lands on the ⅛HP grid along with the edges.
Clearance becomes +0.24 to +0.72mm everywhere — never negative, which is exactly
what outward rounding buys: the gap is measured from the snapped edge, so it is
only safe while that edge is outside the real one.

| | derived gap | clearance |
|---|---|---|
| trimpot (5 cells) | 4.445 | +0.27mm |
| jack (7) | 5.715 | +0.55mm |
| RoundBlackKnob (8) | 6.350 | +0.39mm |
| RoundHugeBlackKnob (15) | 10.795 | +0.52mm |

`--gaps code` mocks the constants as they are in the source instead. Labels
placed by hand — `add()` with a computed `y`, `note()`, `title()` — are left
where the widget puts them either way.

**This applies to the design file only.** The shipped `res/*.svg` keeps 99% of
the real component: a reticule rounded outward there would show as a grey ring
around every knob in the rack.

Screens and plates are stated by hand — in the widget's `box.size` and in
`PLATES` — so `--from` reports the ones that are not whole cells rather than
moving them, and prints the `hp()` values that would be:

```
off-grid  plate 1 x/y/w/h hp(5.2) hp(11.7) hp(18.4) hp(6.6)  ->  hp(5.25) hp(11.75) hp(18.5) hp(6.5)
```

## Figma setup

Three settings, all integers:

1. **Frame size** — `HP × 60` wide by `1517.72` high. The template files already are.
2. **Layout grids** — two of them, stacked on the frame:
   - Grid, size `15`, ~4% black — the quarter
   - Grid, size `60`, ~10% black — the HP cells controls sit on
3. **Nudge amount** (Preferences → Nudge amount) — small `15`, big `60`. Arrow
   keys then move by a quarter and Shift+arrow by a whole cell.

Snap to pixel grid can be left on: at 4× a whole unit is 1/60 HP, a subdivision
of the grid rather than a fight with it. Turn it off only to keep a component
stamp's own size exact — those are inherently fractional (a trimpot is 71.42
wide at 4×) and nothing places by them.

## What is in the template

`panel-NNhp.svg` — the frame, in four layers:

| Layer | |
|---|---|
| `Background` | the `#f0f0f0` faceplate |
| `Reticules` | left empty — `tools/panel_reticules.py` owns it |
| `UI` | your artwork: plates, screens, marks |
| `GUIDES` | the grid, HP numbering down two edges, centre axis, foot band. **Hide it before exporting** |

The shaded band at the foot is the 1.5mm the 25 rows do not reach. It is left
there, never distributed.

`parts.svg` — every component at its real size, each with a centre crosshair and
its size in both units and mm; the three label gaps drawn as they are drawn in
nanovg; a `pair()`, a plate, a screen; the palette; and the type at all three
sizes.

To place a stamp at `hp(x), hp(y)`, centre it on that intersection — or set
`X = 15x − w/2`, `Y = 15y − h/2`.

## A module that already has a widget

```bash
python3 tools/figma_panel_template.py --from chime      # design/figma/chime-panel.svg
```

The blank frame is for a panel that does not exist yet. Once the widget code
does, the layout is already decided — so this reads it rather than redrawing it,
sharing `panel_reticules.py`'s extractor so the two cannot disagree. You get
`Background`, `Plates`, `Reticules`, `Labels (mock)` and `GUIDES`, with every
`PanelLabels` call resolved to the point nanovg will actually draw it: the gaps
applied, `pair()` and `link()` hairlines included, plate ink where it is on a
plate. Draw the faceplate around it, then throw the mock away — the labels
remain runtime-drawn.

Because the text is at the real size and spacing, a label that runs into its
neighbour or off the edge of its plate is visible in the file. That is worth
looking for first: it is the failure the panel cannot show you any other way,
since a white `ON_PLATE` label that overshoots its plate simply disappears
against the faceplate.

Any key in `MODULES` works. A module that predates `PanelLabels` reports zero
labels — Chance and Fill both do.

## Type

Labels are **not** exported. Rack ignores `<text>` in a panel and draws them at
runtime from `src/panel-style.hpp`, so text in Figma is a mock for judging the
layout — keep it in its own layer and hide it before export.

Figtree is on Google Fonts, so Figma has it. To mock what Rack will draw:

| | size | letter spacing | weight |
|---|---|---|---|
| `TYPE_TITLE` | 16.54 (5.6mm) | 0.30 | SemiBold |
| `TYPE_LABEL` | 9.74 (3.3mm) | 0.30 | Regular |
| `TYPE_NOTE` | 7.38 (2.5mm) | 0.12 | Regular |

Set the text box to **vertical align centre** and put its centre on the label
point — nanovg uses `NVG_ALIGN_MIDDLE`, so the gap is measured to the middle of
the text, not to its baseline. Gaps above the control centre: knob **16.53**
(5.6mm), jack **15.94** (5.4mm), trimpot **12.99** (4.4mm).

## Getting it back out

Export the frame as **SVG**, with *Include "id" attribute* on so the layer names
survive, and *Outline text* off. Then:

```bash
python3 tools/figma_panel_template.py --fixup res/newmodule.svg
```

That rewrites Figma's px header into the `width="40.64mm"` + 75dpi `viewBox`
form Rack and `panel_reticules.py` both read, drops any `GUIDES` layer that
slipped through, guarantees a `Reticules` layer, and prints the panel's width in
HP so a non-integer is caught immediately.

From there the pipeline is unchanged:

```bash
python3 tools/panel_reticules.py newmodule
```

which reads the control positions out of the widget constructor and rewrites
`Reticules`. The Figma file is upstream of the code, not the other way round —
once a control moves in `.cpp`, the generator is what puts it right on the panel.

## Two that bite

**Empty layers vanish.** Figma drops a group with nothing in it, so `Reticules`
will not survive the round trip. `--fixup` puts it back; nothing to do.

**Do not draw the components.** Rack paints each one over its own footprint, so
a knob drawn in Figma is invisible in the rack. The stamps are for placing
things, not for keeping. See `panel-design.md`.
