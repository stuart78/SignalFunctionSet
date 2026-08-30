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
import keyword
import math
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
    "Rogan1PBlue": (31.398, 31.398), "Rogan1PSBlue": (39.684, 39.684),
    "Rogan2PBlue": (34.293, 34.293), "Rogan3PBlue": (41.762, 41.762),
    "Rogan3PSBlue": (51.844, 51.844), "Rogan5PSGray": (59.527, 59.527),
    "CKSS": (14.0, 20.641), "CKSSThree": (13.457, 28.348),
    "VCVButton": (18.0, 18.0), "VCVLightBezel": (21.26, 21.26),
    "VCVLightLatch": (18.0, 18.0), "VCVSlider": (19.843, 76.535),
    "ChimeExciteSlider": (30.48 * MM, 5.08 * MM),    # custom horizontal slider
    "TinyLight": (1.0 * MM, 1.0 * MM), "SmallLight": (2.0 * MM, 2.0 * MM),
    "MediumLight": (3.0 * MM, 3.0 * MM), "LargeLight": (5.0 * MM, 5.0 * MM),
}
ROUND = ("Knob", "Rogan", "Trimpot", "PJ301MPort", "VCVButton", "VCVLightBezel",
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
    e = re.sub(r"\(\s*(?:float|int|double)\s*\)", "", e)
    e = e.replace("std::", "").replace("::", "_")
    ns = dict(env)
    # A C++ local may be named `as`, `is`, `in`, `not`, `lambda`... all legal
    # there and all reserved here. Gravity names its sector angle `as`, and the
    # SyntaxError that caused took twelve controls off the panel in silence.
    for name in [n for n in ns if keyword.iskeyword(n)]:
        ns[name + "__kw"] = ns.pop(name)
        e = re.sub(r"\b%s\b" % re.escape(name), name + "__kw", e)
    ns.setdefault("hp", hp)
    for fn in ("sin", "cos", "tan", "sqrt", "floor", "ceil", "fabs", "pow", "atan2"):
        ns.setdefault(fn, getattr(math, fn))
    ns.setdefault("M_PI", math.pi)
    return float(eval(e, {"__builtins__": {}}, ns))


def constants(seed, *sources):
    """Constants the position expressions refer to, from widget body and file
    scope alike. Handles `const float a = 1.f, b = 2.f;` — declaring several per
    line is normal in these widgets and missing them silently drops controls."""
    env = dict(seed)
    for text in sources:
        for m in re.finditer(r"(?:static\s+)?const\s+float\s+(\w+)\s*\[\s*\w*\s*\]\s*=\s*\{([^}]*)\}", text):
            # Each element is EVALUATED, not just parsed as a literal: a column
            # table written on the grid is `{hp(3), hp(8), ...}`, and reading
            # only bare numbers silently dropped every control that used one.
            try:
                env[m.group(1)] = [evaluate(v, env)
                                   for v in m.group(2).split(",") if v.strip()]
            except Exception:
                pass
        for m in re.finditer(r"(?:static\s+)?const\s+int\s+(\w+)\s*=\s*(-?\d+)\s*;", text):
            env.setdefault(m.group(1), float(m.group(2)))
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


def struct_arrays(*sources):
    """`const S arr[N] = {{...}, ...}` as {(arr, field): [v0, v1, ...]}.

    A widget that states a column as a table of structs is not being clever, it
    is being readable -- but `lc[i].py` is opaque to an expression evaluator, so
    without this every control in such a loop is dropped without a word."""
    out = {}
    for text in sources:
        for sm in re.finditer(r"struct\s+(\w+)\s*\{([^}]*)\}\s*;", text):
            sname, fields = sm.group(1), re.findall(r"\b(?:int|float|bool)\s+([\w,\s]+);", sm.group(2))
            names = [f.strip() for grp in fields for f in grp.split(",") if f.strip()]
            if not names:
                continue
            for am in re.finditer(r"(?:static\s+)?const\s+%s\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;"
                                  % re.escape(sname), text, re.S):
                arr, body = am.group(1), am.group(2)
                rows = re.findall(r"\{([^{}]*)\}", body)
                for fi, fname in enumerate(names):
                    vals = []
                    for row in rows:
                        cells = [c.strip() for c in row.split(",")]
                        if fi >= len(cells):
                            vals.append(None); continue
                        cell = cells[fi].strip()
                        if cell in ("true", "false"):
                            vals.append(1.0 if cell == "true" else 0.0)
                            continue
                        try:
                            vals.append(float(cell.rstrip("f")))
                        except ValueError:
                            vals.append(None)      # an enum name, not a coordinate
                    out[(arr, fname)] = vals
    return out


def struct_strings(*sources):
    """`const S arr[N] = {{..., "NAME"}, ...}` as {(arr, field): ["NAME", ...]}.

    struct_arrays deliberately keeps only int/float fields, because it exists to
    resolve coordinates. Label text lives in the same tables as `const char*`,
    and without this a panel that states its jack row as a table -- which is the
    readable way to write it -- exports with every one of those labels missing.
    """
    out = {}
    for text in sources:
        for sm in re.finditer(r"struct\s+(\w+)\s*\{([^}]*)\}\s*;", text):
            sname = sm.group(1)
            # field order must match the initialiser, so read ALL fields, then
            # record which of them are strings.
            decls = re.findall(r"\b(?:const\s+char\s*\*|int|float|bool)\s+([\w,\s]+);",
                               sm.group(2))
            kinds = re.findall(r"\b(const\s+char\s*\*|int|float|bool)\s+[\w,\s]+;",
                               sm.group(2))
            names, isstr = [], []
            for kind, grp in zip(kinds, decls):
                for f in grp.split(","):
                    if f.strip():
                        names.append(f.strip())
                        isstr.append("char" in kind)
            if not any(isstr):
                continue
            for am in re.finditer(r"(?:static\s+)?const\s+%s\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;"
                                  % re.escape(sname), text, re.S):
                arr, abody = am.group(1), am.group(2)
                rows = re.findall(r"\{([^{}]*)\}", abody)
                for fi, fname in enumerate(names):
                    if not isstr[fi]:
                        continue
                    vals = []
                    for row in rows:
                        cells = split_top(row)
                        cell = cells[fi].strip() if fi < len(cells) else ""
                        m = re.match(r'^"(.*)"$', cell)
                        vals.append(m.group(1) if m else None)
                    out[(arr, fname)] = vals
    return out


def split_top(row):
    """Split a struct initialiser on commas that are not inside quotes."""
    out, cur, q = [], "", False
    for ch in row:
        if ch == '"':
            q = not q
        if ch == "," and not q:
            out.append(cur); cur = ""; continue
        cur += ch
    out.append(cur)
    return out


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


def vec_var(name, scope):
    """`Vec gp(x, y);` or `Vec gp = Vec(x, y);` -> the two expressions.

    Gravity builds its sector ring in polar coordinates and passes the Vec, so
    mm2px(gp) carries no coordinates at the call site at all. Twelve jacks and
    twelve lights were missing from its export because of it."""
    m = re.search(r"Vec\s+%s\s*(?:=\s*Vec\s*)?\(" % re.escape(name), scope)
    if not m:
        return None
    i, depth, j, comma = m.end(), 1, m.end(), -1
    while j < len(scope):
        if scope[j] == "(": depth += 1
        elif scope[j] == ")":
            depth -= 1
            if depth == 0: break
        elif scope[j] == "," and depth == 1: comma = j
        j += 1
    if comma < 0 or depth != 0:
        return None
    return scope[i:comma], scope[comma + 1:j]


def loop_ranges(body, env, extra_defs):
    """Every `for (int v = 0; v < N; v++)` as (var, count, start, end), where the
    range is found by matching braces — testing whether the loop variable appears
    in the position text instead would fire on the `c` in `Vec`."""
    out = []
    # The bound may be qualified -- `i < SlideXMessage::NCH` is how an expander
    # states its own width. Without the `::` this matched nothing and every row
    # in the loop was dropped in silence, which is the one thing this tool must
    # not do; SLIDE X exported with zero reticules that way.
    for m in re.finditer(r"for\s*\(\s*int\s+(\w+)\s*=\s*0;\s*\1\s*<\s*([A-Za-z0-9_:]+)\s*;", body):
        n = m.group(2)
        if not n.isdigit():
            bare = n.split("::")[-1]
            n = env.get(n, extra_defs.get(n, env.get(bare, extra_defs.get(bare, 0))))
        n = int(n or 0)
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
    tables = struct_arrays(body)

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
        if not comp:
            continue
        # the position may have been built into a Vec first
        indirect = None
        if not pos:
            vm = re.search(r"mm2px\s*\(\s*(\w+)\s*\)", text)
            indirect = vm.group(1) if vm else None
            if not indirect:
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
        if indirect:
            pos = vec_var(indirect, locals_src) or vec_var(indirect, body)
            if not pos:
                continue

        for i in range(count):
            e = dict(env)
            px, py = pos[0], pos[1]
            if var:
                e[var] = i
                # `lc[i].py` -> the value in row i of the table
                for (arr, field), vals in tables.items():
                    if i < len(vals) and vals[i] is not None:
                        pat = r"\b%s\s*\[\s*%s\s*\]\s*\.\s*%s\b" % (
                            re.escape(arr), re.escape(var), re.escape(field))
                        px = re.sub(pat, repr(vals[i]), px)
                        py = re.sub(pat, repr(vals[i]), py)
                for name, expr in decls:
                    try:
                        e[name] = evaluate(expr, e)
                    except Exception:
                        pass
            try:
                x, y = evaluate(px, e), evaluate(py, e)
            except Exception as err:
                print("    DROPPED %s at %s: cannot evaluate (%s, %s) -- %s"
                      % (comp, call.group(1), px.strip(), py.strip(), err))
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


# Dark plates: the inset slabs that group a section. Rack draws components on
# top of their own footprint, so a plate is one of the few things in the SVG the
# player actually sees. Stated in mm as (x, y, w, h) — design intent, so it lives
# here rather than being inferred from control positions.
PLATES = {
    # every jack is at the foot, so the only thing separating the four outputs
    # from the three CV inputs beside them is this plate
    "slice": [(60.96, 109.98, 47.50, 17.10)],   # mm, read from the design file
    # the output matrix, one row per lane, plus LOOP under it. Tall enough to
    # carry the column labels, which sit above the first row of jacks.
    "trace": [(146.00, 7.50, 25.20, 63.10)],
    # A plate bleeds about a cell past its outermost control -- enough to read as
    # a region rather than a box, without running the width of the panel.
    "chime": [(hp(7.25), hp(13.5), hp(20.0), hp(6.0)),   # the eight note out rows
              (hp(22.25), hp(21.5), hp(5.0), hp(3.25))], # the stereo mix pair
    "crystal": [],
    "fill": [(117.0, 8.0, 43.0, 116.0)],           # swing + the three output columns
    "chance": [],
    # the four channels read as one block, so they sit on one plate
    "key": [(hp(0.8), hp(16.6), hp(12.6), hp(9.0))],
    "slide": [(hp(18.2), hp(22.6), hp(7.0), hp(2.9))],
    # the eight string columns, and the mix pair at the foot
    "loom": [(hp(6.2), hp(15.8), hp(19.0), hp(4.7)),
             (hp(23.5), hp(20.9), hp(4.2), hp(2.9))],
    "slidex": [], "opmorph": [], "gravity": [],
}

MODULES = {
    "crystal": ("Crystal", "src/crystal.cpp", "res/crystal.svg", {"CR_NL": 4}),
    "chime":   ("Chime",   "src/chime.cpp",   "res/chime.svg",   {"CHIME_NCH": 8}),
    "fill":    ("Fill",    "src/fill.cpp",    "res/fill.svg",    {"FILL_NCH": 8}),
    "chance":  ("Chance",  "src/chance.cpp",  "res/chance.svg",  {"NUM_NODES": 8}),
    "key":     ("Key",     "src/key.cpp",     "res/key.svg",     {"KEY_NCH": 4}),
    "slide":   ("Slide",   "src/slide.cpp",   "res/slide.svg",   {"SLIDE_NCH": 8}),
    "loom":    ("Loom",    "src/loom.cpp",    "res/loom.svg",    {"LOOM_N": 8}),
    "gravity": ("Gravity", "src/gravity.cpp", "res/gravity.svg", {}),
    "touch":   ("Touch",   "src/touch.cpp",   "res/touch.svg",   {"ART_COUNT": 6}),
    "slidex":  ("SlideX",  "src/slidex.cpp",  "res/slidex.svg",  {"NCH": 8}),
    "slice": ("Slice", "src/slice.cpp", "res/slice.svg",
               {"SLICE_NXF": 7}),
    "opmorph": ("OpMorph", "src/opmorph.cpp", "res/opmorph.svg", {"OPM_COLS": 4, "OPM_ROWS": 4, "OPM_SLOTS": 16}),
    "kit":     ("Kit",     "src/kit.cpp",     "res/kit.svg",     {}),
    "trace":   ("Trace",   "src/trace.cpp",   "res/trace.svg",   {"TR_LANES": 4}),
    "sigma":   ("Sigma",   "src/sigma.cpp",   "res/sigma.svg",   {"SG_PARTIALS": 16}),
}

# Panels whose artwork is hand-made and is now the SOURCE of the layout rather
# than a target for it. Splicing generated reticules into these would draw
# circles straight over the design. Named explicitly on the command line they
# still run, so the guard is against the bare sweep, not against intent.
FINISHED = {"crystal", "chime", "loom", "slide", "slidex", "fill", "gravity", "key",
            "slice"}

if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    explicit = bool(sys.argv[1:])
    for key in (sys.argv[1:] or MODULES.keys()):
        if key in FINISHED and not explicit:
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
        nc, ns = splice(p, old, elems, pl, upmm)
        print(f"{key:8} {nc:3} controls, {ns} screen(s) -> {svg}  ({upmm:.4f} units/mm)")
