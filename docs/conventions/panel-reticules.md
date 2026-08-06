# Panel reticules

A *reticule* is the placement guide on a panel SVG that marks where a control
actually sits. It is what the panel art is drawn around, so if it drifts from
the widget code the art is wrong everywhere.

This had been done differently on every panel — different sizes, different
shapes, some controls drawn with three or four nested paths, some panels at
72dpi and some at 75. The rules below are the settlement, and
`tools/panel_reticules.py` applies them mechanically so they cannot drift again.

## The rules

**1. Controls are drawn at 99% of the real component.** Not a nominal size, not
a round number — 99% of what Rack actually renders, so the art can sit flush
against the edge of a knob without ever being overlapped by it. The 1%
under-size is what keeps the reticule visible when the component is on top.

**2. One shape per element.** A control is a single `<circle>` or `<rect>`,
never a group of concentric rings or tick marks. It has to be selectable and
draggable as one object; anything more makes repositioning a control a
multi-step edit and is how panels get subtly misaligned.

**3. Screens are full size and filled with the display blue** (`#1a1a32`) — the
same colour the display widgets clear to. A screen is not a guide to draw
around, it *is* the finished element, so it should look on the panel exactly as
it looks in Rack. Full size, not 99%: nothing overlaps it.

## Layers

**One layer.** Everything the generator produces lives in `Reticules` — screens
and controls together — so the whole guide can be shown or hidden with a single
toggle. Screens are emitted first so a control is never buried under one.

| Layer | Owner | Contents |
|---|---|---|
| `Background` | designer | the panel fill, base artwork |
| `Reticules` | **generator** | every screen and control guide |
| `UI`, `Layer_*`, … | designer | labels, decoration, VCV's signal icons |

Reticules deliberately do *not* live in `UI`: the VCV Illustrator template
already puts its signal-type icons there, and an earlier version of this tool
overwrote them.

## Coordinate space

Rack's `mm2px()` uses **75 dpi** (2.952756 units/mm). Illustrator exports at
**72 dpi** (2.834646). Both render identically because Rack scales the SVG by
its `width`/`height` in mm — but only at 75dpi does a number in the SVG equal
the same number in the widget code, which is worth having.

- **New panels**: author at 75dpi, so `viewBox = mm × 2.952756`.
- **Existing artwork**: leave it alone. The generator reads each file's own
  `viewBox ÷ width_mm` and emits in that space, so it aligns either way.

## Component sizes

Taken from Rack's own `res/ComponentLibrary`, in px (÷2.952756 for mm):

| Component | px | Component | px |
|---|---|---|---|
| `RoundHugeBlackKnob` | 53.86 | `PJ301MPort` | 23.70 |
| `RoundLargeBlackKnob` | 36.00 | `CKSS` | 14.00 × 20.64 |
| `RoundBlackKnob` | 28.35 | `CKSSThree` | 13.46 × 28.35 |
| `RoundSmallBlackKnob` | 22.68 | `VCVButton` | 18.00 |
| `Trimpot` | 17.86 | `VCVLightBezel` | 21.26 |
| `MediumLight` | 3 mm | `VCVSlider` | 19.84 × 76.54 |

Round components get a `<circle>`; switches and sliders get a `<rect>` with a
small corner radius.

## Regenerating

```
python3 tools/panel_reticules.py                 # all registered panels
python3 tools/panel_reticules.py crystal chime   # just these
```

It reads positions **out of the widget constructor** — `mm2px(Vec(...))` calls,
including those inside `for` loops and those using local constants — so the
widget code stays the single source of truth. Run it after moving any control.

Adding a module means one line in `MODULES` at the foot of the script, plus any
integer constants its loops use (`{"CR_NL": 4}`).

## Other panel rules

- **Never add screws.** No `ScrewSilver`/`ScrewBlack` anywhere in this plugin.
- **Labels are drawn in nanovg, not SVG.** Rack ignores `<text>` in panels; see
  the `*Labels : Widget` pattern in any module.
- Panel fill is `#f0f0f0`; reticule stroke is `#b2b2b2` at 0.5 width.

## Two layers, and only one of them ships visible

The tool owns two groups and rewrites both on every run:

* **`PanelArt`** — the dark plates and the screen rects. These are DESIGN: Rack
  draws components over their own footprints, so the plates, the screens and the
  runtime labels are most of what a player actually sees.
* **`Reticules`** — the placement guides, written with `style="display:none"`.
  They are for drawing against, not for shipping. Rack covers most of them with
  the components, but not all, and a stray outline on a finished faceplate reads
  as a mistake.

They used to be one group, which meant the guides could not be hidden without
losing the plates and screens with them. To work on a panel's artwork, delete
the `style` attribute (or toggle the layer in the editor), draw, and let the next
tool run put it back.
