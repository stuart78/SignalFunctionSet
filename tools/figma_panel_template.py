#!/usr/bin/env python3
"""Figma panel templates for Signal Function Set.

Emits SVGs that Figma imports as frames: one per HP width, plus a parts bin of
real-size component stamps, label gaps, palette and type specimens.

Rack's own space is 75dpi, where 1HP is exactly 15 units -- so a quarter is
3.75, and Figma's layout grid is integer-only and rounds it to 4. Everything is
therefore emitted at **4x** by default, where a quarter is 15 and a whole HP is
60. Nothing downstream cares: Rack scales a panel by its width/height in mm, and
panel_reticules.py reads each file's own viewBox/width_mm, so the unit is free.

    python3 tools/figma_panel_template.py               # default widths, 4x
    python3 tools/figma_panel_template.py 12 26 34      # just these
    python3 tools/figma_panel_template.py --scale 1     # Rack units, for Illustrator
    python3 tools/figma_panel_template.py --from chime  # a module that already exists
    python3 tools/figma_panel_template.py --fixup res/new.svg

--fixup rewrites a Figma export's px header into the mm+viewBox form Rack and
tools/panel_reticules.py both expect, and adds the empty Reticules layer. It
recovers the scale from the panel height, so it does not need to be told.
"""

import math
import os
import re
import sys

MM = 2.952756                  # Rack px per mm (75dpi) -- mm2px()
HP = 15.0                      # 1HP = 5.08mm, exactly 15 units at 75dpi
Q = HP / 4                     # the design grid, 3.75 in Rack units
PANEL_MM = 128.5               # 3U
PANEL_H = PANEL_MM * MM        # 379.43
ROWS = 25                      # usable 1HP rows; the 1.5mm remainder is the foot

# The design scale. 4 makes the quarter grid 15 and a whole HP 60, both of which
# Figma's integer-only grid and nudge fields will accept. 8 additionally puts
# eighths on 15, so component edges are integral too. 1 is Rack's own space, for
# Illustrator.
S = 4.0

# Components are drawn a whole number of 1/SNAP HP cells across, so a control
# centred on the grid has its edges on it as well. 4 (quarter cells) is the
# closest fit that still only ever rounds outward -- a reticule smaller than its
# component disappears underneath it, and takes any art drawn flush with it.
# 2 puts every edge on the quarter grid itself, at up to 17% distortion.
SNAP = 4

PANEL = "#f0f0f0"
INK = "#231f20"
INK_SOFT = "#6a6a78"
PLATE = "#1a1a1a"
PLATE_INK = "#e8e8f0"
HAIRLINE = "#b2b2b2"
SCREEN_BG = "#1a1a32"
LINK = "#9a9aa6"               # the pot-to-jack connector, as drawn in nanovg

G_EIGHTH = "#eeeef2"
G_QUARTER = "#e4e4e9"
G_HP = "#d0d0d8"
G_MAJOR = "#b6b6c4"
G_TEXT = "#a8a8b6"
G_AXIS = "#d8b0a8"

FONT = "Figtree"
T_LABEL = 3.3 * MM             # 9.74 -- one label size, per panel-style.hpp
T_NOTE = 2.5 * MM              # 7.38
T_TITLE = 5.6 * MM             # 16.54
LS_LABEL = 0.10 * MM           # letter spacing, 0.30
LS_NOTE = 0.04 * MM

GAP_KNOB = 5.6 * MM            # control centre -> label centre (NVG_ALIGN_MIDDLE)
GAP_JACK = 5.4 * MM
GAP_TRIM = 4.4 * MM
PAIR_SPAN = 2 * HP             # cells from a pot to its jack
SZ_TRIM, SZ_JACK, SZ_KNOB = 17.856, 23.700, 28.348   # what each helper labels

RETICULE_W = 0.5               # stroke widths, in Rack units
LINK_W = 0.35 * MM

# Real component sizes in px, the same table panel_reticules.py works from.
PARTS = [
    ("RoundHugeBlackKnob",  "round", 53.859, 53.859),
    ("RoundLargeBlackKnob", "round", 36.000, 36.000),
    ("RoundBlackKnob",      "round", 28.348, 28.348),
    ("RoundSmallBlackKnob", "round", 22.676, 22.676),
    ("Trimpot",             "round", 17.856, 17.859),
    ("PJ301MPort",          "round", 23.700, 23.700),
    ("VCVButton",           "round", 18.000, 18.000),
    ("VCVLightBezel",       "round", 21.260, 21.260),
    ("VCVLightLatch",       "round", 18.000, 18.000),
    ("CKSS",                "flat",  14.000, 20.641),
    ("CKSSThree",           "flat",  13.457, 28.348),
    ("VCVSlider",           "flat",  19.843, 76.535),
    ("LargeLight",          "round",  5.000 * MM, 5.000 * MM),
    ("MediumLight",         "round",  3.000 * MM, 3.000 * MM),
    ("SmallLight",          "round",  2.000 * MM, 2.000 * MM),
    ("TinyLight",           "round",  1.000 * MM, 1.000 * MM),
]

SWATCHES = [
    ("PANEL", PANEL), ("INK", INK), ("INK_SOFT", INK_SOFT), ("PLATE", PLATE),
    ("PLATE_INK", PLATE_INK), ("HAIRLINE", HAIRLINE), ("SCREEN_BG", SCREEN_BG),
    ("SCREEN_BLUE", "#0097de"), ("SCREEN_DEEP", "#0d5986"), ("SCREEN_LINE", "#0d5988"),
    ("SCREEN_PURP", "#35354d"), ("SCREEN_PMID", "#4a4a66"), ("SCREEN_HOT", "#ec652e"),
    ("SCREEN_TEXT", "#e8e8f0"), ("SCREEN_DIM", "#8a8aa5"),
]


def raw(v):
    """A plain number -- a size quoted in a caption, not a coordinate."""
    return f"{v:.4f}".rstrip("0").rstrip(".")


def n(v):
    """A length. Geometry is reasoned about in Rack units and emitted at S, so
    only this function ever needs to know what the design scale is."""
    return raw(v * S)


ROUND = "ceil"                 # "ceil" or "near"; see snap()


def snap(v):
    """A component size, rounded to a whole 1/SNAP HP. Never to zero: the 1mm
    TinyLight is a third of a cell and still has to be findable.

    Rounds OUTWARD by default. A reticule under its component's true size is
    invisible in the rack and takes any art drawn flush with it, and it also
    breaks the derived label gap below, which measures from the snapped edge and
    is only safe when that edge is outside the real one. The price is a 7-cell
    jack: adjacent jacks on the standard 2HP pitch keep one whole cell between
    their guides instead of the real parts\' 2.13mm. "near" is the tighter fit
    at the cost of six dimensions that end up under size."""
    cell = HP / SNAP
    k = math.ceil(v / cell - 1e-9) if ROUND == "ceil" else int(v / cell + 0.5)
    return max(1, k) * cell


# The label sits this many cells clear of the component's snapped EDGE. Stating
# it from the edge rather than the centre is what makes the relationship the
# same for a trimpot and a huge knob; panel-style.hpp states three fixed centre
# gaps instead, which do not hold a constant clearance across the three.
GAP_CELLS = 1.0
GAPS = "cells"                 # "cells" = derived from the size, "code" = panel-style.hpp


def gap_for(size_px, code_gap):
    """Control centre -> label centre, in mm."""
    if GAPS != "cells":
        return code_gap / MM
    return (snap(size_px) / 2 + GAP_CELLS * HP / SNAP) / MM


def g(name, body, extra=""):
    # Figma names a layer from data-name if present, else id.
    return (f'  <g id="{name}" data-name="{name}"{extra}>\n'
            + "\n".join("    " + b for b in body) + f"\n  </g>")


def line(x1, y1, x2, y2, stroke, w=0.5, dash=None, op=None):
    d = f' stroke-dasharray="{n(dash[0])} {n(dash[1])}"' if dash else ""
    d += f' stroke-opacity="{op}"' if op is not None else ""
    return (f'<line x1="{n(x1)}" y1="{n(y1)}" x2="{n(x2)}" y2="{n(y2)}" '
            f'stroke="{stroke}" stroke-width="{n(w)}"{d}/>')


def rect(x, y, w, h, fill=None, stroke=None, sw=0.5, rx=None, opacity=None):
    a = f'<rect x="{n(x)}" y="{n(y)}" width="{n(w)}" height="{n(h)}"'
    if rx is not None:
        a += f' rx="{n(rx)}"'
    a += f' fill="{fill}"' if fill else ' fill="none"'
    if stroke:
        a += f' stroke="{stroke}" stroke-width="{n(sw)}"'
    if opacity is not None:
        a += f' opacity="{opacity}"'
    return a + "/>"


def circle(cx, cy, r, fill=None, stroke=None, sw=0.5, fill_opacity=None):
    a = f'<circle cx="{n(cx)}" cy="{n(cy)}" r="{n(r)}"'
    a += f' fill="{fill}"' if fill else ' fill="none"'
    if fill_opacity is not None:
        a += f' fill-opacity="{fill_opacity}"'
    if stroke:
        a += f' stroke="{stroke}" stroke-width="{n(sw)}"'
    return a + "/>"


def text(x, y, s, size=T_LABEL, fill=INK, anchor="middle", ls=None, weight=400):
    ls = LS_LABEL if ls is None else ls
    return (f'<text x="{n(x)}" y="{n(y)}" font-family="{FONT}" font-size="{n(size)}" '
            f'font-weight="{weight}" letter-spacing="{n(ls)}" fill="{fill}" '
            f'text-anchor="{anchor}" dominant-baseline="middle">{s}</text>')


def svg_open(w, h, note):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{n(w)}" height="{n(h)}" '
            f'viewBox="0 0 {n(w)} {n(h)}">\n  <!-- {note} -->\n')


# ── the grid ────────────────────────────────────────────────────────────────
def grid_layer(w, numbered=True):
    grid, nums = [], []
    # Three tiers. The eighth is not a placement grid -- controls never sit on
    # it -- but a control sized to a whole quarter has its EDGES there, so it is
    # what makes the fit visible.
    E = HP / (2 * SNAP)
    i = 1
    while i * E < w:
        if i % 2:
            grid.append(line(i * E, 0, i * E, PANEL_H, G_EIGHTH, 0.2, op=0.5))
        i += 1
    i = 1
    while i * E < PANEL_H:
        if i % 2:
            grid.append(line(0, i * E, w, i * E, G_EIGHTH, 0.2, op=0.5))
        i += 1
    i = 1
    while i * Q < w:                       # quarters, skipping the HP lines
        if i % 4:
            grid.append(line(i * Q, 0, i * Q, PANEL_H, G_QUARTER, 0.25))
        i += 1
    i = 1
    while i * Q < PANEL_H:
        if i % 4:
            grid.append(line(0, i * Q, w, i * Q, G_QUARTER, 0.25))
        i += 1

    hp_units = int(round(w / HP))
    for i in range(1, hp_units):           # 1HP, every 5th emphasised
        grid.append(line(i * HP, 0, i * HP, PANEL_H, G_MAJOR if i % 5 == 0 else G_HP,
                         0.5 if i % 5 == 0 else 0.35))
    for j in range(1, ROWS + 1):
        grid.append(line(0, j * HP, w, j * HP, G_MAJOR if j % 5 == 0 else G_HP,
                         0.5 if j % 5 == 0 else 0.35))

    grid.append(line(w / 2, 0, w / 2, PANEL_H, G_AXIS, 0.5, (3, 3)))
    # The 1.5mm the 25 rows do not reach. It is left at the foot, never spread.
    grid.append(rect(0, ROWS * HP, w, PANEL_H - ROWS * HP, "#000000", opacity="0.06"))

    if numbered:
        # Counted in HP from the top-left corner. The last of each runs inward,
        # so it is not clipped by the panel edge.
        for i in range(0, hp_units + 1, 1 if hp_units <= 14 else 2):
            last = i == hp_units
            nums.append(text(i * HP + (-2 if last else 2), 5, str(i), 4.2, G_TEXT,
                             "end" if last else "start", 0))
        for j in range(0, ROWS + 1):
            nums.append(text(2, min(j * HP + 5, PANEL_H - 3), str(j), 4.2, G_TEXT,
                             "start", 0))
    return g("GUIDES", grid + nums)


# ── the blank panel frame ───────────────────────────────────────────────────
def panel_svg(hp_units):
    w = hp_units * HP
    layers = [
        g("Background", [rect(0, 0, w, PANEL_H, PANEL)]),
        g("Reticules", ['<!-- owned by tools/panel_reticules.py, never hand-edited -->']),
        g("UI", ['<!-- artwork: plates, screens, marks -->']),
        grid_layer(w),
    ]
    return (svg_open(w, PANEL_H,
                     f"{hp_units}HP = {raw(hp_units * 5.08)}mm at {raw(S)}x: "
                     f"1HP = {raw(HP * S)}u, 1/4HP = {raw(Q * S)}u")
            + "\n".join(layers) + "\n</svg>\n")


# ── the parts bin ───────────────────────────────────────────────────────────
def stamp(name, kind, w, h, cx, cy):
    """Snapped to the grid, plus a crosshair -- placing by centre is the whole
    job, and a size in whole cells is what puts the edges on the grid too."""
    w, h = snap(w), snap(h)
    if kind == "round":
        shape = circle(cx, cy, w / 2, "#ffffff", HAIRLINE, RETICULE_W, 0.55)
    else:
        shape = rect(cx - w / 2, cy - h / 2, w, h, "#ffffff", HAIRLINE, RETICULE_W,
                     rx=HP / SNAP / 2)
        shape = shape.replace('fill="#ffffff"', 'fill="#ffffff" fill-opacity="0.55"')
    cross = [line(cx - 4, cy, cx + 4, cy, "#ec652e", 0.5),
             line(cx, cy - 4, cx, cy + 4, "#ec652e", 0.5)]
    return g(name, [shape] + cross)


def parts_svg():
    W, H = 960, 700
    out = [rect(0, 0, W, H, PANEL)]

    for i in range(1, int(W / Q)):
        out.append(line(i * Q, 0, i * Q, H, G_QUARTER if i % 4 else G_HP,
                        0.25 if i % 4 else 0.35))
    for j in range(1, int(H / Q)):
        out.append(line(0, j * Q, W, j * Q, G_QUARTER if j % 4 else G_HP,
                        0.25 if j % 4 else 0.35))

    out.append(text(15, 30, "SIGNAL FUNCTION SET — PANEL PARTS", T_TITLE, INK,
                    "start", LS_LABEL, 600))
    out.append(text(15, 48, f"{raw(S)}x · 1HP = {raw(HP * S)}u · cell = {raw(HP / SNAP * S)}u "
                            f"· sizes are whole cells, so edges land on "
                            f"{raw(HP / (2 * SNAP) * S)}u",
                    T_NOTE, INK_SOFT, "start", LS_NOTE))

    # components, on a 120u pitch so the widest (the slider) still clears
    parts = []
    x0, y0, pitch, rowh = 75, 130, 120, 165
    for i, (name, kind, w, h) in enumerate(PARTS):
        cx = x0 + (i % 8) * pitch
        cy = y0 + (i // 8) * rowh
        parts.append(stamp(name, kind, w, h, cx, cy))
        sw, sh, cell = snap(w), snap(h), HP / SNAP
        parts.append(text(cx, cy + 52, name, T_NOTE, INK, "middle", LS_NOTE))
        parts.append(text(cx, cy + 62, f"{raw(sw * S)} × {raw(sh * S)}u = "
                                       f"{round(sw / cell)} × {round(sh / cell)} cells",
                          T_NOTE, INK_SOFT, "middle", LS_NOTE))
        parts.append(text(cx, cy + 72, f"real {raw(round(w / MM, 2))} × "
                                       f"{raw(round(h / MM, 2))}mm  {(sw - w) / w * 100:+.1f}%",
                          T_NOTE, INK_SOFT, "middle", LS_NOTE))
    out.append(g("COMPONENTS", parts))

    # label gaps: the constants a caller never states, applied here as drawn
    gy = 470
    demo = [text(15, gy - 40, "LABEL GAPS — label centre above control centre",
                 T_NOTE, INK_SOFT, "start", LS_NOTE)]
    for i, (name, kind, w, h, gap, cap) in enumerate([
            ("knob", "round", 28.348, 28.348, GAP_KNOB, "RATE"),
            ("trim", "round", 17.856, 17.859, GAP_TRIM, "CURVE"),
            ("jack", "round", 23.700, 23.700, GAP_JACK, "CLOCK")]):
        cx = 75 + i * 150
        gap = gap_for(w, gap) * MM
        demo.append(stamp(name, kind, w, h, cx, gy))
        demo.append(text(cx, gy - gap, cap, T_LABEL, INK))
        demo.append(line(cx + 40, gy, cx + 40, gy - gap, "#ec652e", 0.4))
        demo.append(text(cx + 44, gy - gap / 2, f"{raw(round(gap / MM, 2))}mm",
                         T_NOTE, INK_SOFT, "start", LS_NOTE))

    # a pot and the jack it modulates: the line says what the jack is, so no label
    px, py = 560, gy
    demo.append(line(px, py, px + PAIR_SPAN, py, LINK, LINK_W))
    demo.append(stamp("pair-trim", "round", 17.856, 17.859, px, py))
    demo.append(stamp("pair-jack", "round", 23.700, 23.700, px + PAIR_SPAN, py))
    demo.append(text(px, py - GAP_TRIM, "SPREAD", T_LABEL, INK))
    demo.append(text(px + PAIR_SPAN / 2, py + 34, "pair() — 2 cells, no jack label",
                     T_NOTE, INK_SOFT, "middle", LS_NOTE))
    out.append(g("LABEL GAPS", demo))

    # a plate and a screen, the two things in the SVG the player actually sees
    slab = [text(15, 545, f"PLATE (r = {raw(6 * S)}u) · SCREEN — both are stated by hand "
                          f"in the widget, so size them in whole cells",
                 T_NOTE, INK_SOFT, "start", LS_NOTE),
            rect(15, 560, 6 * HP, 3 * HP, PLATE, rx=6),
            text(15 + 3 * HP, 560 + 1.5 * HP, "OUTPUTS", T_LABEL, PLATE_INK),
            rect(15 + 7 * HP, 560, 8 * HP, 3 * HP, SCREEN_BG, rx=3),
            text(15 + 11 * HP, 560 + 1.5 * HP, "SCREEN", T_LABEL, "#8a8aa5")]
    out.append(g("PLATE + SCREEN", slab))

    ty = [text(400, 560, "CHIME", T_TITLE, INK, "start", LS_LABEL, 600),
          text(400, 580, f"TYPE_TITLE 5.6mm / {raw(round(T_TITLE * S, 2))}u — Figtree SemiBold",
               T_NOTE, INK_SOFT, "start", LS_NOTE),
          text(400, 600, "STABILITY", T_LABEL, INK, "start"),
          text(400, 616, f"TYPE_LABEL 3.3mm / {raw(round(T_LABEL * S, 2))}u · "
                         f"letter-spacing {raw(round(LS_LABEL * S, 2))}u — Figtree Regular",
               T_NOTE, INK_SOFT, "start", LS_NOTE),
          text(400, 636, "BOW ←→ STRIKE", T_NOTE, INK_SOFT, "start", LS_NOTE),
          text(400, 650, f"TYPE_NOTE 2.5mm / {raw(round(T_NOTE * S, 2))}u · "
                         f"letter-spacing {raw(round(LS_NOTE * S, 2))}u",
               T_NOTE, INK_SOFT, "start", LS_NOTE)]
    out.append(g("TYPE", ty))

    sw = []
    for i, (name, hexv) in enumerate(SWATCHES):
        sx = 15 + (i % 15) * 62
        sw.append(rect(sx, 665, 46, 20, hexv, HAIRLINE, 0.4, rx=2))
        sw.append(text(sx, 692, name, 5.2, INK_SOFT, "start", 0))
    out.append(g("PALETTE", sw))

    return (svg_open(W, H, f"panel parts at {raw(S)}x")
            + "\n".join(l if l.startswith("  <g") else "  " + l for l in out)
            + "\n</svg>\n")


# ── an existing module, as a Figma working file ─────────────────────────────
# The blank frame is for a panel that does not exist yet. Once the widget code
# does, the layout is already decided -- so pull it out rather than redraw it,
# and design around the real thing.

def split_args(s):
    """Top-level commas only: hp(2), Vec(a, b) and "a, b" all appear as args."""
    out, depth, cur, q = [], 0, "", False
    for ch in s:
        if ch == '"':
            q = not q
        if not q:
            if ch in "([":
                depth += 1
            elif ch in ")]":
                depth -= 1
            elif ch == "," and depth == 0:
                out.append(cur.strip()); cur = ""; continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def cstring(arg):
    if not (arg.startswith('"') and arg.endswith('"')):
        return None
    s = arg[1:-1]
    s = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), s)
    s = s.replace('\\"', '"').replace("\\\\", "\\")
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


ALIGN = {"NVG_ALIGN_LEFT": "start", "NVG_ALIGN_RIGHT": "end", "NVG_ALIGN_CENTER": "middle"}
KIND_FILL = {"LABEL": INK, "ON_PLATE": PLATE_INK, "NOTE": INK_SOFT, "TITLE": INK}
KIND_SIZE = {"LABEL": T_LABEL, "ON_PLATE": T_LABEL, "NOTE": T_NOTE, "TITLE": T_TITLE}


def labels(body, ev):
    """Every PanelLabels call, resolved to where nanovg will actually draw it.

    Also returns the ones it could NOT resolve. A call whose coordinates come
    from a lambda parameter or a loop variable is unreachable to a regex, and a
    label that quietly fails to appear reads as a label that is not there --
    which is the one thing this file must never say."""
    texts, links, skipped = [], [], []
    for m in re.finditer(r"lbl->(\w+)\(", body):
        depth, j = 1, m.end()
        while j < len(body) and depth:
            depth += (body[j] == "(") - (body[j] == ")")
            j += 1
        a = split_args(body[m.end():j - 1])
        fn = m.group(1)
        try:
            if fn == "link":
                onplate = len(a) > 4 and "true" in a[4]
                links.append((ev(a[0]), ev(a[1]), ev(a[2]), ev(a[3]), onplate))
                continue
            # pairDown puts the jack's y where the others put the text, so it has
            # to be unpacked before the common (x, y, text) read below.
            if fn == "pairDown":
                x, y, y2 = ev(a[0]), ev(a[1]), ev(a[2])
                onplate = len(a) > 4 and "true" in a[4]
                links.append((x, y, x, y2, onplate))
                texts.append((x, y - gap_for(SZ_TRIM, GAP_TRIM), cstring(a[3]),
                              "ON_PLATE" if onplate else "LABEL", "middle"))
                continue
            x, y, t = ev(a[0]), ev(a[1]), cstring(a[2]) if len(a) > 2 else None
            if t is None:
                skipped.append(fn)
                continue
            kind, align = "LABEL", "middle"
            if fn == "knob":
                y -= gap_for(SZ_KNOB, GAP_KNOB)
            elif fn == "trim":
                y -= gap_for(SZ_TRIM, GAP_TRIM)
            elif fn == "jack":
                y -= gap_for(SZ_JACK, GAP_JACK)
            elif fn == "jackOnPlate":
                y -= gap_for(SZ_JACK, GAP_JACK); kind = "ON_PLATE"
            elif fn == "note":
                kind = "NOTE"
            elif fn == "title":
                kind, align = "TITLE", "start"
            elif fn == "pair":
                onplate = len(a) > 3 and "true" in a[3]
                kind = "ON_PLATE" if onplate else "LABEL"
                links.append((x, y, x + PAIR_SPAN / MM, y, onplate))
                y -= gap_for(SZ_TRIM, GAP_TRIM)
            elif fn == "add":
                if len(a) > 3 and "ON_PLATE" in a[3]:
                    kind = "ON_PLATE"
                elif len(a) > 3 and "NOTE" in a[3]:
                    kind = "NOTE"
                if len(a) > 4:
                    align = ALIGN.get(a[4].strip(), "middle")
            else:
                continue
            texts.append((x, y, t, kind, align))
        except Exception:
            skipped.append(fn)
            continue
    return texts, links, skipped


def module_svg(key):
    """A working file for a module that already has a widget: the real controls,
    screens and plates, with the labels mocked where nanovg draws them."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import panel_reticules as pr

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    name, cpp, svg, defs = pr.MODULES[key]
    src = open(os.path.join(root, cpp)).read()
    body = pr.widget_source(src, name)
    if not body:
        raise SystemExit(f"{key}: no {name}Widget")

    env = pr.constants(dict(defs), body, src)
    env.update(defs)
    env.update({"LABEL_GAP_KNOB": GAP_KNOB / MM, "LABEL_GAP_JACK": GAP_JACK / MM,
                "LABEL_GAP_TRIM": GAP_TRIM / MM})

    def ev(expr):
        return pr.evaluate(expr.replace("sfs::", ""), env)

    elems = pr.collect(body, env, defs)
    # Only the shared label widget can be mocked faithfully. Crystal has its own
    # CrystalLabels -- Share Tech Mono, size in px, no gaps -- so drawing its
    # calls in Figtree at TYPE_LABEL would misrepresent the panel.
    shared = "sfs::PanelLabels" in src
    texts, links, skipped = labels(body, ev) if shared else ([], [], [])
    plates = [(x * MM, y * MM, w * MM, h * MM) for (x, y, w, h) in pr.PLATES.get(key, [])]

    head = open(os.path.join(root, svg)).read()
    m = re.search(r'width="([0-9.]+)mm"', head)
    if not m:
        # A px header (Figma's default) rather than the mm one Rack and these
        # tools expect. Raised rather than guessed, and named rather than
        # crashing the whole batch, because it stops at the FIRST such module
        # and every later one silently never runs -- which is how this went
        # unnoticed for crystal.
        raise ValueError("%s has no width in mm -- run --fixup on it first" % svg)
    wmm = float(m.group(1))
    # From HP rather than mm x MM: 28HP is exactly 1680u at 4x, and rounding the
    # truncated mm-per-unit constant leaves 1680.0001 in the header instead.
    hp_units = int(round(wmm / 5.08))
    w = hp_units * HP

    art = [rect(x, y, pw, ph, PLATE, rx=6) for (x, y, pw, ph) in plates]
    ret = [rect(cx - ew / 2, cy - eh / 2, ew, eh, SCREEN_BG, rx=3)
           for k, cx, cy, ew, eh in elems if k == "screen"]
    # Snapped, not 99%: in the design file the point of a reticule is that its
    # edges are on the grid. The shipped res/*.svg keeps 99% of the real part.
    for k, cx, cy, ew, eh in elems:
        if k == "round":
            ret.append(circle(cx, cy, snap(ew) / 2, None, HAIRLINE, RETICULE_W))
        elif k == "flat":
            sw, sh = snap(ew), snap(eh)
            ret.append(rect(cx - sw / 2, cy - sh / 2, sw, sh, None, HAIRLINE,
                            RETICULE_W, rx=HP / SNAP / 2))

    lab = [line(x1 * MM, y1 * MM, x2 * MM, y2 * MM, "#4a4a52" if op else LINK, LINK_W)
           for (x1, y1, x2, y2, op) in links]
    for (x, y, t, kind, align) in texts:
        lab.append(text(x * MM, y * MM, t, KIND_SIZE[kind], KIND_FILL[kind], align,
                        LS_NOTE if kind == "NOTE" else LS_LABEL,
                        600 if kind == "TITLE" else 400))

    # Controls are snapped for you. Screens and plates are stated by hand in the
    # widget and in PLATES, so they are reported rather than moved -- the numbers
    # to change are hp() values in the source.
    cell = HP / SNAP
    def off(vals):
        # Loose, because these have been through mm -> units and back on a
        # truncated units-per-mm; 1e-4 of a cell is 5e-4 mm.
        return any(abs(v / cell - round(v / cell)) > 1e-4 for v in vals)
    def as_hp(vals):
        return " ".join(f"hp({v / HP:g})" for v in vals)
    def to_hp(vals):
        return " ".join(f"hp({round(v / cell) * cell / HP:g})" for v in vals)
    audit = []
    for k, cx, cy, ew, eh in elems:
        if k == "screen":
            box = (cx - ew / 2, cy - eh / 2, ew, eh)
            if off(box):
                audit.append(f"off-grid  screen  x/y/w/h {as_hp(box)}  ->  {to_hp(box)}")
    for i, box in enumerate(plates):
        if off(box):
            audit.append(f"off-grid  plate {i + 1} x/y/w/h {as_hp(box)}  ->  {to_hp(box)}")

    layers = [
        g("Background", [rect(0, 0, w, PANEL_H, PANEL)]),
        g("Plates", art or ['<!-- none: see PLATES in tools/panel_reticules.py -->']),
        g("Reticules", ret),
        g("Labels (mock)", lab),
        grid_layer(w),
    ]
    if not shared:
        audit.insert(0, "labels not mocked: this widget does not use sfs::PanelLabels")
    elif skipped:
        c = ", ".join(f"{k}x{skipped.count(k)}" if skipped.count(k) > 1 else k
                      for k in sorted(set(skipped)))
        audit.insert(0, f"{len(skipped)} label call(s) NOT mocked -- coordinates come "
                        f"from a loop or lambda this cannot evaluate: {c}")
    return (svg_open(w, PANEL_H, f"{name}: {hp_units}HP at {raw(S)}x, geometry read "
                                 f"from {cpp}. Design layer only.")
            + "\n".join(layers) + "\n</svg>\n"), hp_units, len(ret), len(texts), audit


# ── Figma export -> Rack panel ──────────────────────────────────────────────
def fixup(path):
    """Figma exports px; Rack and panel_reticules.py want mm + a viewBox. The
    design scale is recovered from the height, which is 128.5mm on every panel
    ever made -- so a 4x file needs no flag and cannot be mistaken for a 1x one
    that happens to be four times as wide."""
    svg = open(path).read()
    vb = re.search(r'viewBox="0 0 ([0-9.]+) ([0-9.]+)"', svg)
    if not vb:
        print(f"{path}: no viewBox"); return
    w, h = float(vb.group(1)), float(vb.group(2))
    # Snap the scale, exactly as --publish does. A designer's file often carries
    # a rounded viewBox height (1518 where 4x is 1517.7166), and carrying that
    # 0.02% through makes a 25HP panel come out 24.995HP -- off the grid by a
    # hair, which is invisible in the file and visible in the rack.
    scale = h / PANEL_H
    if abs(scale - round(scale)) < 0.01 and round(scale) > 0:
        scale = float(round(scale))
    elif abs(scale - round(scale, 3)) > 1e-6 or scale <= 0:
        print(f"{path}: height {h:.2f} is not a 3U panel at any clean scale")
    wmm = w / (MM * scale)
    svg = re.sub(r'width="[^"]+"', f'width="{wmm:.4f}mm"', svg, count=1)
    svg = re.sub(r'height="[^"]+"', f'height="{PANEL_MM:.4f}mm"', svg, count=1)
    if 'id="Reticules"' not in svg:
        svg = svg.replace("</svg>", '  <g id="Reticules"></g>\n</svg>')
    for name in ("GUIDES", "Guides"):
        # Balanced, because a designer may well have grouped things inside it.
        m = re.search(r'<g id="%s"[^>]*>' % name, svg)
        if not m:
            continue
        depth, end = 1, None
        for tag in re.finditer(r"<g\b[^>]*>|</g>", svg[m.end():]):
            depth += 1 if tag.group(0).startswith("<g") else -1
            if depth == 0:
                end = m.end() + tag.end()
                break
        if end:
            svg = svg[:m.start()] + svg[end:].lstrip("\n")
            print(f"{path}: dropped the {name} layer")
    open(path, "w").write(svg)
    hp_units = wmm / 5.08
    warn = "" if abs(hp_units - round(hp_units)) < 1e-4 else "  ** not a whole HP **"
    print(f"{path}: {wmm:.2f} × {PANEL_MM}mm = {hp_units:.3f}HP at {scale:.4g}x{warn}")


def publish(key):
    """design/<key>.svg IS the panel. Copy it to res/, with the header rewritten
    for Rack and the placement guides hidden.

    This is the direction the tools did not have. panel_reticules.py GENERATES a
    panel from the widget source, which means it emits the plates and the screen
    and nothing else -- no logo, no artwork -- and leaves the labels and the
    connector lines to be redrawn at runtime in Figtree at panel-style.hpp's own
    size and weight. That is right while a layout is being worked out in code.
    It is wrong the moment a designer hands over a finished face, because every
    line they set is then replaced by one this repo chose. Publishing keeps the
    file exactly as drawn: their type, their weights, their logo.

    Two things are removed. The reticule circles, which are guides to draw
    against and not part of the face. A flat export has no layer to hide, so
    each is identified by sitting on a control the widget actually declares --
    position and size both, so nothing else at that spot is caught.

    And the MOVING part of any control the designer drew in full: a slider's
    track is artwork and stays, but the pointer drawn on it is at one value,
    and Rack has to draw that live. Anything sitting wholly inside a flat
    control and under 60% of its width is taken to be that part."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import panel_reticules as pr

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    name, cpp, out, defs = pr.MODULES[key]
    src_svg = os.path.join(root, "design", f"{key}.svg")
    if not os.path.exists(src_svg):
        print(f"{key}: no design/{key}.svg"); return
    svg = open(src_svg).read()

    vb = re.search(r'viewBox="0 0 ([0-9.]+) ([0-9.]+)"', svg)
    if not vb:
        print(f"{key}: design file has no viewBox"); return
    w, h = float(vb.group(1)), float(vb.group(2))
    # The scale comes from the 3U height, then snaps: a designer's file may carry
    # a rounded height (1518 where 4x is 1517.7166), and carrying that 0.02%
    # through is the difference between 28.000HP and 27.995HP.
    scale = h / PANEL_H
    if abs(scale - round(scale)) < 0.01:
        scale = float(round(scale))
    upmm = MM * scale                         # file units per mm

    body = pr.widget_source(open(os.path.join(root, cpp)).read(), name)
    env = pr.constants(defs, body, open(os.path.join(root, cpp)).read())
    spots = [(k, cx / MM * upmm, cy / MM * upmm, ew / MM * upmm, eh / MM * upmm)
             for (k, cx, cy, ew, eh) in pr.collect(body, env, defs) if k != "screen"]

    def bbox(el, d):
        """A shape's real extent. Figma writes a rotated rect as an unrotated one
        plus rotate(a cx cy), so the raw x/y are not where it ends up -- the
        slider's own pointer lands a whole control away from its attributes."""
        if d is not None:
            nums = [float(v) for v in re.findall(r"-?\d+\.?\d*(?:e-?\d+)?", d)]
            pts = list(zip(nums[0::2], nums[1::2]))
        else:
            def f(a, dv=0.0):
                m = re.search(r'%s="(-?[\d.]+)"' % a, el)
                return float(m.group(1)) if m else dv
            x, y, ew, eh = f("x"), f("y"), f("width"), f("height")
            pts = [(x, y), (x + ew, y), (x + ew, y + eh), (x, y + eh)]
        if not pts:
            return None
        t = re.search(r'transform="rotate\(\s*(-?[\d.]+)[\s,]+(-?[\d.]+)[\s,]+(-?[\d.]+)\s*\)"', el)
        if t:
            a = math.radians(float(t.group(1)))
            px, py = float(t.group(2)), float(t.group(3))
            ca, sa = math.cos(a), math.sin(a)
            pts = [(px + (x - px) * ca - (y - py) * sa,
                    py + (x - px) * sa + (y - py) * ca) for (x, y) in pts]
        xs = [q[0] for q in pts]; ys = [q[1] for q in pts]
        return (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, \
               max(xs) - min(xs), max(ys) - min(ys)

    def is_guide(el, d):
        b = bbox(el, d)
        if not b:
            return False
        cx, cy, bw, bh = b
        for i, (kind, sx, sy, sw, sh) in enumerate(spots):
            near = abs(cx - sx) < sw * 0.5 and abs(cy - sy) < sh * 0.5
            if not near:
                continue
            if pr.RETICULE in el.lower() \
               and abs(bw - sw) < sw * 0.35 and abs(bh - sh) < sh * 0.35:
                hit[i] = True
                return True
            if kind == "flat" and bw < sw * 0.6:      # the pointer, not the track
                movers.append(1)
                return True
        return False

    guides, kept, n, hit, movers = [], [], 0, [False] * len(spots), []
    pos = 0
    for m in re.finditer(r'<(path|circle|rect)\b[^>]*?/>', svg):
        el = m.group(0)
        dm = re.search(r'd="([^"]*)"', el)
        if is_guide(el, dm.group(1) if dm else None):
            guides.append(el)
            kept.append(svg[pos:m.start()])
            pos = m.end()
            n += 1
    kept.append(svg[pos:])
    svg = "".join(kept)

    svg = re.sub(r'width="[^"]+"', f'width="{w / upmm:.4f}mm"', svg, count=1)
    svg = re.sub(r'height="[^"]+"', f'height="{PANEL_MM:.4f}mm"', svg, count=1)
    svg = svg.replace("</svg>", '<g id="Reticules" style="display:none">\n'
                      + "\n".join("  " + gd for gd in guides) + "\n</g>\n</svg>")

    open(os.path.join(root, out), "w").write(svg)
    hp_units = (w / upmm) / 5.08
    warn = "" if abs(hp_units - round(hp_units)) < 1e-4 else "  ** not a whole HP **"
    print(f"{key}: design/{key}.svg -> {out}   {w / upmm:.2f}mm = {hp_units:.3f}HP "
          f"at {upmm / MM:.4g}x, {n - len(movers)}/{len(spots)} guides + "
          f"{len(movers)} live part(s) hidden{warn}")
    # A control with no guide is usually fine -- a slider's real artwork is in
    # the file, so there is nothing to hide -- but a knob or jack missing one
    # means the code and the design have drifted apart, so name the positions.
    missing = [f"({sx / upmm:.2f}, {sy / upmm:.2f})mm"
               for i, (kind, sx, sy, sw, sh) in enumerate(spots)
               if not hit[i] and kind != "flat"]
    if missing:
        print(f"    no guide in the design at {', '.join(missing)} -- expected for a "
              f"control drawn in full (the slider), otherwise the two have drifted")


DEFAULT_WIDTHS = [4, 6, 8, 10, 12, 16, 20, 26, 32]

if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    args = sys.argv[1:]
    if "--scale" in args:
        i = args.index("--scale")
        S = float(args[i + 1])
        del args[i:i + 2]
    if "--snap" in args:
        i = args.index("--snap")
        SNAP = int(args[i + 1])
        del args[i:i + 2]
    for flag, name in (("--round", "ROUND"), ("--gaps", "GAPS")):
        if flag in args:
            i = args.index(flag)
            globals()[name] = args[i + 1]
            del args[i:i + 2]
    if args and args[0] == "--publish":
        for k in args[1:]:
            publish(k)
        sys.exit(0)
    if args and args[0] == "--fixup":
        for p in args[1:]:
            fixup(p)
        sys.exit(0)
    out = os.path.join(root, "design", "figma")
    os.makedirs(out, exist_ok=True)
    print(f"{raw(S)}x — 1HP = {raw(HP * S)}u, 1/4HP = {raw(Q * S)}u, "
          f"panel height {raw(PANEL_H * S)}u; sizes snapped to 1/{SNAP} HP "
          f"({raw(HP / SNAP * S)}u), rounding {ROUND}, gaps from {GAPS}")
    if args and args[0] == "--from":
        for key in args[1:]:
            try:
                svg, hpn, nret, nlab, audit = module_svg(key)
            except ValueError as e:
                print(f"{key}: SKIPPED -- {e}")
                continue
            open(os.path.join(out, f"{key}-panel.svg"), "w").write(svg)
            print(f"{key}-panel.svg    {hpn}HP   {nret} reticules   {nlab} labels")
            for a in audit:
                print(f"    {a}")
        sys.exit(0)
    for hpn in ([int(a) for a in args] or DEFAULT_WIDTHS):
        open(os.path.join(out, f"panel-{hpn:02d}hp.svg"), "w").write(panel_svg(hpn))
        print(f"panel-{hpn:02d}hp.svg   {hpn * 5.08:.2f}mm   {raw(hpn * HP * S)}u wide")
    open(os.path.join(out, "parts.svg"), "w").write(parts_svg())
    print("parts.svg          component stamps, label gaps, palette, type")
