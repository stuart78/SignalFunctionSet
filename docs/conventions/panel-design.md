# Panel design

Panels are code. Geometry is generated from the widget constructor; text is drawn
at runtime in Figtree from `src/panel-style.hpp`. Nothing is hand-placed, so
nothing can drift out of step with the module.

## Why not Illustrator

**Rack ignores `<text>` in a panel SVG.** Every label therefore had to be
outlined to paths — Chance carries 84 text paths, Band 106 — so changing one
word meant reopening Illustrator and re-outlining. Drawing labels at runtime
removes that entirely: text stays text, the font is one line, and a moved
control takes its label with it.

## What is actually visible

Rack draws each component on top of its own footprint, so **anything in the SVG
the size of a knob or a jack is hidden underneath it**. Effort spent detailing
those shapes is wasted. What the player sees is:

- the faceplate
- the **plates** — dark insets that group a section
- the **screens**
- the **labels**
- any mark that falls *outside* a component's footprint

Reticules are the exception that proves it: they are drawn at 99% precisely so a
sliver stays visible around the component.

## Palette

| Token | Value | Use |
|---|---|---|
| `PANEL` | `#f0f0f0` | faceplate |
| `INK` | `#231f20` | labels on the faceplate |
| `INK_SOFT` | `#6a6a78` | pot-position notes, ranges |
| `PLATE` | `#1a1a1a` | dark inset grouping a section |
| `PLATE_INK` | `#e8e8f0` | labels on a plate |
| `HAIRLINE` | `#b2b2b2` | reticules, connectors |
| `SCREEN_BG` | `#1a1a32` | what displays clear to |

The screen palette (`SCREEN_BLUE`, `SCREEN_HOT`, …) is shared by every display
widget so the modules read as one instrument.

## Type

**Figtree**, bundled in `res/fonts/` under the SIL Open Font License (licence
included). Loaded with `asset::plugin`, not `asset::system`.

**One label size.** `TYPE_LABEL` 4.4mm — smaller than the 6mm the old mono face
used, because Figtree's x-height is much larger and reads bigger at the same
nominal size. Only two other sizes exist: `TYPE_NOTE` 3.2mm for annotations that
mark what a pot position means, and `TYPE_TITLE` 7mm for the module name.

Labels are **above** their control, uppercase, centred, with a little letter
spacing. The gap is a constant per control type, so callers state the *control's*
position and never a label position:

```cpp
lbl->knob(x, y, "RATE");     // 6.6mm above the knob centre
lbl->trim(x, y, "CURVE");    // 5.2mm — trimpots are smaller
lbl->jack(x, y, "CLOCK");
lbl->add(x, y, "AUDIO", sfs::PanelLabels::ON_PLATE);   // light ink on a plate
lbl->note(x, y, "BOW ←→ STRIKE");
lbl->title(4.5f, 7.f, "CHIME");
```

Prefer **trimpots to large pots** — they leave room for the layout to breathe,
and the plugin's density suits them.

## Plates

Declared in `PLATES` in `tools/panel_reticules.py` as mm rects. Grouping is
design intent, so it is stated rather than inferred. Leave ~3mm between a plate
and a screen so the two read as separate objects, and remember that any label
inside a plate needs `ON_PLATE` ink or it will be dark-on-dark.

## Generating

```bash
python3 tools/panel_reticules.py            # every registered panel
```

See `panel-reticules.md` for the reticule rules (99%, one shape, single layer)
and the 72dpi-vs-75dpi trap.
