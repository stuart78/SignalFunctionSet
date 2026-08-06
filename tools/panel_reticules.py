#!/usr/bin/env python3
"""Generate a panel reticule SVG from a module's widget constructor.

The widget code is the single source of truth for where controls sit, so this
reads the positions straight out of it rather than asking anyone to keep a
second copy in sync. Run it again after moving anything:

    python3 tools/panel_reticules.py crystal chime fill chance

Conventions it enforces (see docs/conventions/panel-reticules.md):
  * one shape per element, so a reticule can be dragged as a unit
  * controls drawn at 99% of the real component, centred on the real position
  * screens at 100%, filled with the display blue
"""
import glob
import os
import re
import sys

MM = 2.952756                      # Rack SVG px per mm (75 dpi)
HPMM = 5.08                        # 1HP — the layout grid, in both axes
def hp(n): return n * HPMM
SCALE = 0.99                       # controls sit at 99%
DISPLAY_BLUE = "#1a1a32"           # the same deep blue the displays clear to
PANEL = "#f0f0f0"
RETICULE = "#b2b2b2"               # matches the existing hand-drawn panels
PLATE = "#1a1a1a"                  # dark inset grouping a section
# Held in mm, not panel units, because a panel may be authored at a larger scale
# so its quarter-HP grid comes out integral (tools/figma_panel_template.py). A
# file's own units-per-mm then scales these, and a 75dpi file is unchanged.
PLATE_R_MM = 6.0 / MM              # corner radius
SCREEN_R_MM = 3.0 / MM
STROKE_MM = 0.5 / MM               # reticule hairline

# Real component sizes, read from Rack's own ComponentLibrary (px).
SIZES = {
    "RoundHugeBlackKnob": (53.859, 53.859), "RoundLargeBlackKnob": (36.0, 36.0),
    "RoundBlackKnob": (28.348, 28.348), "RoundSmallBlackKnob": (22.676, 22.676),
    "Trimpot": (17.856, 17.859), "PJ301MPort": (23.7, 23.7),
    "CKSS": (14.0, 20.641), "CKSSThree": (13.457, 28.348),
    "VCVButton": (18.0, 18.0), "VCVLightBezel": (21.26, 21.26),
    "VCVLightLatch": (18.0, 18.0), "VCVSlider": (19.843, 76.535),
    "ChimeExciteSlider": (30.0 * MM, 7.0 * MM),      # custom horizontal slider
    "TinyLight": (1.0 * MM, 1.0 * MM), "SmallLight": (2.0 * MM, 2.0 * MM),
    "MediumLight": (3.0 * MM, 3.0 * MM), "LargeLight": (5.0 * MM, 5.0 * MM),
}
ROUND = ("Knob", "Trimpot", "PJ301MPort", "VCVButton", "VCVLightBezel",
         "VCVLightLatch", "Light")


def component(call):
    """Component class from create*Centered<X<Y<Z>>>(...) — outermost wins."""
    m = re.search(r"create\w*?Centered<\s*([A-Za-z0-9_]+)", call)
    return m.group(1) if m else None


def widget_source(src, module):
    """The body of struct <Module>Widget's constructor."""
    m = re.search(r"struct %sWidget\s*:\s*ModuleWidget\s*\{(.*?)\n\};" % module,
                  src, re.S)
    return m.group(1) if m else ""


def evaluate(expr, env):
    """Positions may be written on the 1HP grid as hp(n), so the grid helper has
    to exist in the evaluation namespace too."""
    # strip C++ float suffixes: 76.31f, 4.f and .5f all appear in these widgets
    e = re.sub(r"(?<=[\d.])f\b", "", expr)
    e = e.replace("M_PI", "3.141592653589793")
    e = e.replace("sfs::", "")          # panel-style constants are namespaced
    ns = dict(env)
    ns.setdefault("hp", hp)
    return float(eval(e, {"__builtins__": {}}, ns))


def constants(seed, *sources):
    """Constants the position expressions refer to, from widget body and file
    scope alike. Handles `const float a = 1.f, b = 2.f;` — declaring several per
    line is normal in these widgets and missing them silently drops controls."""
    env = dict(seed)
    for text in sources:
        for m in re.finditer(r"(?:static\s+)?const\s+float\s+(\w+)\s*\[\s*\w*\s*\]\s*=\s*\{([^}]*)\}", text):
            try:
                env[m.group(1)] = [float(v.strip().rstrip("f"))
                                   for v in m.group(2).split(",") if v.strip()]
            except Exception:
                pass
        for m in re.finditer(r"(?:static\s+)?const\s+float\s+([^;\[]+);", text):
            for decl in m.group(1).split(","):
                if "=" not in decl:
                    continue
                name, _, val = decl.partition("=")
                try:
                    env[name.strip()] = evaluate(val, env)
                except Exception:
                    pass
    return env


def vec_args(text, start=0):
    """The two expressions inside the first mm2px(Vec( ... )) at or after `start`.
    Split on the top-level comma with balanced parens -- a naive [^)]+ truncates
    hp(2) to "hp(2" the moment positions are written on the grid."""
    m = re.search(r"mm2px\(\s*Vec\(", text[start:])
    if not m:
        return None
    i = start + m.end()
    depth, j, comma = 1, i, -1
    while j < len(text):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                break
        elif text[j] == "," and depth == 1:
            comma = j
        j += 1
    if comma < 0 or depth != 0:
        return None
    return text[i:comma], text[comma + 1:j], j


def loop_ranges(body, env, extra_defs):
    """Every `for (int v = 0; v < N; v++)` as (var, count, start, end), where the
    range is found by matching braces — testing whether the loop variable appears
    in the position text instead would fire on the `c` in `Vec`."""
    out = []
    for m in re.finditer(r"for\s*\(\s*int\s+(\w+)\s*=\s*0;\s*\1\s*<\s*([A-Za-z0-9_]+)\s*;", body):
        n = m.group(2)
        n = int(n) if n.isdigit() else int(env.get(n, extra_defs.get(n, 0)) or 0)
        if not n:
            continue
        i = body.find("{", m.end())
        nl = body.find("\n", m.end())
        if i < 0 or (nl >= 0 and i > nl):        # braceless single-statement body
            end = body.find(";", m.end()) + 1
            out.append((m.group(1), n, m.end(), end))
            continue
        depth, j = 0, i
        while j < len(body):
            if body[j] == "{":
                depth += 1
            elif body[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((m.group(1), n, i, j))
    return out


def collect(body, env, extra_defs):
    """Every placed element as (kind, cx_px, cy_px, w_px, h_px)."""
    out = []
    env = dict(env)
    env.update(extra_defs)

    # displays / custom children: box.pos + box.size in mm
    for m in re.finditer(r"(\w+)->box\.pos\s*=\s*(mm2px.*?);\s*\1->box\.size\s*=\s*(mm2px.*?);",
                         body, re.S):
        try:
            pv, sv = vec_args(m.group(2)), vec_args(m.group(3))
            if not pv or not sv:
                continue
            x, y = evaluate(pv[0], env), evaluate(pv[1], env)
            w, h = evaluate(sv[0], env), evaluate(sv[1], env)
            if w >= 20.0 and h >= 12.0:      # a thin text readout is not a screen
                out.append(("screen", (x + w / 2) * MM, (y + h / 2) * MM, w * MM, h * MM))
        except Exception:
            pass

    loops = loop_ranges(body, env, extra_defs)

    for call in re.finditer(r"(add(?:Param|Input|Output|Child))\((.*?)\);", body, re.S):
        text = call.group(2)
        pos = vec_args(text)
        comp = component(text)
        if not pos or not comp:
            continue
        size = None
        for key, val in SIZES.items():
            if key in comp:
                size = val
                break
        if size is None:
            size = SIZES["PJ301MPort"] if "Port" in comp else SIZES["Trimpot"]

        # innermost enclosing loop, plus any `float v = ...;` it declares
        var, count, locals_src = None, 1, ""
        for lv, lc, ls, le in loops:
            if ls <= call.start() <= le:
                var, count = lv, lc
                locals_src = body[ls:le]
        decls = re.findall(r"float\s+(\w+)\s*=\s*([^;]+);", locals_src) if var else []

        for i in range(count):
            e = dict(env)
            if var:
                e[var] = i
                for name, expr in decls:
                    try:
                        e[name] = evaluate(expr, e)
                    except Exception:
                        pass
            try:
                x, y = evaluate(pos[0], e), evaluate(pos[1], e)
            except Exception:
                break
            kind = "round" if any(r in comp for r in ROUND) else "flat"
            out.append((kind, x * MM, y * MM, size[0], size[1]))
    return out


def group_range(svg, gid):
    """Balanced <g id="gid"> ... </g> span, tolerating nested groups."""
    m = re.search(r'<g id="%s"[^>]*>' % re.escape(gid), svg)
    if not m:
        return None
    i, depth = m.end(), 1
    for tag in re.finditer(r"<g\b[^>]*>|</g>", svg[m.end():]):
        depth += 1 if tag.group(0).startswith("<g") else -1
        if depth == 0:
            return (m.start(), m.end(), m.end() + tag.start(), m.end() + tag.end())
    return None


def art_shapes(elems, indent="    ", plates=(), upmm=MM):
    """What the player SEES: the dark plates and the screens."""
    out = []
    pr, sr = PLATE_R_MM * upmm, SCREEN_R_MM * upmm
    for (x, y, w, h) in plates:
        out.append(f'{indent}<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" '
                   f'height="{h:.2f}" rx="{pr:.2f}" fill="{PLATE}"/>')
    for k, cx, cy, w, h in elems:
        if k == "screen":
            out.append(f'{indent}<rect x="{cx-w/2:.2f}" y="{cy-h/2:.2f}" width="{w:.2f}" '
                       f'height="{h:.2f}" rx="{sr:.2f}" fill="{DISPLAY_BLUE}"/>')
    return out


def guide_shapes(elems, indent="    ", upmm=MM):
    """The placement guides. These are for DRAWING against and are hidden in the
    shipped panel — Rack covers most of them with the components, but not all,
    and a stray outline on a finished faceplate reads as a mistake."""
    out = []
    sw = STROKE_MM * upmm
    for k, cx, cy, w, h in elems:
        if k == "round":
            out.append(f'{indent}<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{w/2*SCALE:.2f}" '
                       f'fill="none" stroke="{RETICULE}" stroke-width="{sw:.2f}"/>')
        elif k == "flat":
            out.append(f'{indent}<rect x="{cx-w*SCALE/2:.2f}" y="{cy-h*SCALE/2:.2f}" '
                       f'width="{w*SCALE:.2f}" height="{h*SCALE:.2f}" rx="{w*0.12:.2f}" '
                       f'fill="none" stroke="{RETICULE}" stroke-width="{sw:.2f}"/>')
    return out


def splice(path, old, elems, plates=(), upmm=MM):
    """Rewrite the two layers this tool owns -- PanelArt and Reticules -- and
    leave every other layer of the designer's file alone."""
    art = art_shapes(elems, "    ", plates, upmm)
    guides = guide_shapes(elems, "    ", upmm)
    out = old
    for gid in ("Screens", "PanelArt", "Reticules"):   # drop whatever is there now
        r = group_range(out, gid)
        if r:
            out = out[:r[0]] + out[r[3]:]
    block = ('  <g id="PanelArt">\n' + "\n".join(art) + "\n  </g>\n"
             '  <g id="Reticules" style="display:none">\n'
             + "\n".join(guides) + "\n  </g>\n")
    out = out.replace("</svg>", block + "</svg>")
    out = re.sub(r"\n\s*\n", "\n", out)
    open(path, "w").write(out)
    return sum(1 for e in elems if e[0] != "screen"), sum(1 for e in elems if e[0] == "screen")



# ── design export ───────────────────────────────────────────────────────────
# The shipped panel hides its guides; a designer needs the opposite. This writes
# a separate file with everything visible AND the things that exist only in
# code — the 1HP grid the layout is stated on, and the runtime labels, which are
# drawn in Figtree at load time and appear nowhere in the SVG. Without them a
# designer has no way to know where the text lands or how much room it needs.
LABEL_GAP = {"knob": 5.6, "jack": 5.4, "jackOnPlate": 5.4, "trim": 4.4,
             "pair": 4.4, "add": 0.0, "note": 0.0, "title": 0.0}
# The same numbers, for the labels whose y is written as `row - LABEL_GAP_JACK`
# rather than by calling jack(). Without these those labels evaluate to nothing
# and vanish from the guide without a word.
STYLE_CONSTS = {"LABEL_GAP_KNOB": 5.6, "LABEL_GAP_JACK": 5.4, "LABEL_GAP_TRIM": 4.4,
                "PLATE_PAD": 2.6, "TYPE_LABEL": 3.3, "TYPE_NOTE": 2.5, "TYPE_TITLE": 5.6}
TYPE_MM = {"title": 5.6, "note": 2.5}          # everything else is TYPE_LABEL
TYPE_LABEL_MM = 3.3


def labels(body, env, extra_defs):
    """Every runtime label as (x_mm, y_mm, text, size_mm, anchor)."""
    out = []
    env = dict(env); env.update(extra_defs); env.update(STYLE_CONSTS)
    loops = loop_ranges(body, env, extra_defs)
    pat = re.compile(r"lbl->(\w+)\(([^;]*?)\);", re.S)
    for m in pat.finditer(body):
        meth, args = m.group(1), m.group(2)
        if meth == "link":
            continue
        sm = re.search(r'"((?:[^"\\]|\\.)*)"', args)
        if not sm:
            continue
        text = sm.group(1).encode().decode("unicode_escape")
        nums = args[:sm.start()].rstrip().rstrip(",")
        # split the two leading coordinates on the top-level comma
        depth, cut = 0, -1
        for i, ch in enumerate(nums):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == "," and depth == 0:
                cut = i
        if cut < 0:
            continue
        xs, ys = nums[:cut], nums[cut + 1:]
        var, count, locals_src = None, 1, ""
        for lv, lc, ls, le in loops:
            if ls <= m.start() <= le:
                var, count, locals_src = lv, lc, body[ls:le]
        decls = re.findall(r"float\s+(\w+)\s*=\s*([^;]+);", locals_src) if var else []
        for i in range(count):
            e = dict(env)
            if var:
                e[var] = i
                for name, expr in decls:
                    try:
                        e[name] = evaluate(expr, e)
                    except Exception:
                        pass
            try:
                x, y = evaluate(xs, e), evaluate(ys, e)
            except Exception:
                break
            size = TYPE_MM.get(meth, TYPE_LABEL_MM)
            # Labels on a dark plate are drawn in plate ink at runtime, so the
            # guide has to show them light too or they read as invisible.
            on_plate = (meth == "jackOnPlate") or ("ON_PLATE" in args)
            anchor = "start" if meth == "title" else "middle"
            if "NVG_ALIGN_RIGHT" in args:
                anchor = "end"
            elif "NVG_ALIGN_LEFT" in args:
                anchor = "start"
            out.append((x, y - LABEL_GAP.get(meth, 0.0), text, size, anchor, on_plate))
    return out


def design_svg(path, wmm, hmm, elems, plates, lbls, upmm):
    """A guide file to design against: grid, plates, screens, guides, labels."""
    W, H = wmm * upmm, hmm * upmm
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{wmm:.4f}mm" '
         f'height="{hmm:.1f}mm" viewBox="0 0 {W:.4f} {H:.4f}">',
         f'  <g id="Background"><rect width="{W:.4f}" height="{H:.4f}" fill="{PANEL}"/></g>',
         '  <g id="Grid">']
    q = HPMM / 4.0
    n = 0
    while n * q <= wmm + 1e-6:
        x = n * q * upmm
        major = abs((n * q) % HPMM) < 1e-6
        o.append(f'    <line x1="{x:.2f}" y1="0" x2="{x:.2f}" y2="{H:.2f}" '
                 f'stroke="#D8D8E0" stroke-width="{(0.6 if major else 0.3):.2f}" '
                 f'stroke-opacity="{(0.9 if major else 0.5):.2f}"/>')
        n += 1
    n = 0
    while n * q <= hmm + 1e-6:
        y = n * q * upmm
        major = abs((n * q) % HPMM) < 1e-6
        o.append(f'    <line x1="0" y1="{y:.2f}" x2="{W:.2f}" y2="{y:.2f}" '
                 f'stroke="#D8D8E0" stroke-width="{(0.6 if major else 0.3):.2f}" '
                 f'stroke-opacity="{(0.9 if major else 0.5):.2f}"/>')
        n += 1
    o.append("  </g>")
    o.append('  <g id="PanelArt">')
    o += art_shapes(elems, "    ", plates, upmm)
    o.append("  </g>")
    o.append('  <g id="Reticules">')
    o += guide_shapes(elems, "    ", upmm)
    o.append("  </g>")
    o.append('  <g id="Labels">')
    for (x, y, t, sz, anchor, on_plate) in lbls:
        esc = t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        ink = "#E8E8F0" if on_plate else "#231F20"
        o.append(f'    <text x="{x*upmm:.2f}" y="{y*upmm:.2f}" font-family="Figtree" '
                 f'font-size="{sz*upmm:.2f}" text-anchor="{anchor}" '
                 f'dominant-baseline="middle" fill="{ink}">{esc}</text>')
    o.append("  </g>")
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")


# Dark plates: the inset slabs that group a section. Rack draws components on
# top of their own footprint, so a plate is one of the few things in the SVG the
# player actually sees. Stated in mm as (x, y, w, h) — design intent, so it lives
# here rather than being inferred from control positions.
PLATES = {
    # A plate bleeds about a cell past its outermost control -- enough to read as
    # a region rather than a box, without running the width of the panel.
    "chime": [(hp(5.2), hp(11.7), hp(18.4), hp(6.6)),    # the eight note columns
              (hp(16.8), hp(19.7), hp(9.4), hp(2.7))],   # the output cluster
    "crystal": [],
    "fill": [(117.0, 8.0, 43.0, 116.0)],           # swing + the three output columns
    "chance": [],
    # the four channels read as one block, so they sit on one plate
    "key": [(hp(0.8), hp(16.6), hp(12.6), hp(9.0))],
    "slide": [(hp(18.2), hp(22.6), hp(7.0), hp(2.9))],
    # the eight string columns, and the mix pair at the foot
    "loom": [(hp(6.2), hp(15.8), hp(19.0), hp(4.7)),
             (hp(23.5), hp(20.9), hp(4.2), hp(2.9))],
}

MODULES = {
    "crystal": ("Crystal", "src/crystal.cpp", "res/crystal.svg", {"CR_NL": 4}),
    "chime":   ("Chime",   "src/chime.cpp",   "res/chime.svg",   {"CHIME_NCH": 8}),
    "fill":    ("Fill",    "src/fill.cpp",    "res/fill.svg",    {"FILL_NCH": 8}),
    "chance":  ("Chance",  "src/chance.cpp",  "res/chance.svg",  {"NUM_NODES": 8}),
    "key":     ("Key",     "src/key.cpp",     "res/key.svg",     {"KEY_NCH": 4}),
    "slide":   ("Slide",   "src/slide.cpp",   "res/slide.svg",   {"SLIDE_NCH": 8}),
    "loom":    ("Loom",    "src/loom.cpp",    "res/loom.svg",    {"LOOM_N": 8}),
}

# Panels whose artwork is hand-made and is now the SOURCE of the layout rather
# than a target for it. Splicing generated reticules into these would draw
# circles straight over the design. Named explicitly on the command line they
# still run, so the guard is against the bare sweep, not against intent.
FINISHED = {"crystal"}

if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    args = [a for a in sys.argv[1:] if a != "--design"]
    # --design writes a separate guide file instead of touching the shipped panel:
    # everything visible, plus the grid and the code-drawn labels.
    DESIGN = "--design" in sys.argv[1:]
    explicit = bool(args)
    for key in (args or MODULES.keys()):
        if key in FINISHED and not explicit and not DESIGN:
            print(f"{key}: skipped — finished artwork, layout comes FROM the panel")
            continue
        mod, cpp, svg, defs = MODULES[key]
        src = open(os.path.join(root, cpp)).read()
        body = widget_source(src, mod)
        if not body:
            print(f"{key}: could not find {mod}Widget"); continue
        # Panel size in mm comes from the existing file (so HP never changes by
        # accident), but the viewBox is rebuilt at Rack's own 75dpi. That makes a
        # coordinate in the SVG the same number as in mm2px() — Illustrator's
        # default 72dpi export silently breaks that correspondence.
        p = os.path.join(root, svg)
        old = open(p).read()
        wmm = float(re.search(r'width="([0-9.]+)mm"', old).group(1))
        vb = re.search(r'viewBox="0 0 ([0-9.]+) ([0-9.]+)"', old)
        # Work in the panel's own coordinate space. Illustrator exports at 72dpi
        # while Rack's mm2px() is 75dpi, so this factor is what keeps generated
        # shapes aligned with hand-drawn artwork.
        upmm = float(vb.group(1)) / wmm
        env = constants(defs, body, src)
        elems = [(k, x / MM * upmm, y / MM * upmm, w / MM * upmm, h / MM * upmm)
                 for (k, x, y, w, h) in collect(body, env, defs)]
        pl = [(x * upmm, y * upmm, w * upmm, h * upmm) for (x, y, w, h) in PLATES.get(key, [])]

        if DESIGN:
            hmm = float(vb.group(2)) / upmm
            lbls = labels(body, env, defs)
            os.makedirs(os.path.join(root, "design", "guides"), exist_ok=True)
            gp = os.path.join(root, "design", "guides", f"{key}-guide.svg")
            design_svg(gp, wmm, hmm, elems, pl, lbls, upmm)
            print(f"{key:8} {len(elems):3} controls, {len(lbls):3} labels "
                  f"-> design/guides/{key}-guide.svg")
            continue

        nc, ns = splice(p, old, elems, pl, upmm)
        print(f"{key:8} {nc:3} controls, {ns} screen(s) -> {svg}  ({upmm:.4f} units/mm)")
