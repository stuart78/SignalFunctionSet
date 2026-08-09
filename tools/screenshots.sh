#!/usr/bin/env bash
# Refresh screenshots/ — one PNG per VISIBLE module, exactly as the README uses.
#
# Rack takes these itself: `Rack -t <zoom>` renders every installed module and
# writes <userDir>/screenshots/<pluginSlug>/<moduleSlug>.png. That is why the
# committed files are 60*HP wide and 1520 tall (Rack's own grid at 4x) and are
# named by module slug rather than by module name -- `gsx.png`, not `GSX.png`.
#
# The catch is that Rack screenshots EVERY installed plugin, and this machine
# has ~170 of them. So we point -u at a throwaway user directory holding only
# this plugin: the real Rack install is never touched and nothing else renders.
#
# The modules are drawn with module == NULL, which is the same path the VCV
# Library uses for browser thumbnails -- so a display widget without a
# drawPreview() shows up here as an empty dark slab. That makes this script a
# check on the browser-preview contract as well as a way to make the README
# images. See docs/conventions/browser-preview-pattern.md.
set -euo pipefail

RACK="${RACK_APP:-/Applications/VCV Rack 2 Pro.app/Contents/MacOS/Rack}"
ZOOM="${ZOOM:-4}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="$(ls -t "$ROOT"/dist/SignalFunctionSet-*.vcvplugin 2>/dev/null | head -1)"

[ -x "$RACK" ] || { echo "Rack not found at: $RACK  (set RACK_APP)"; exit 1; }
[ -n "$PKG" ]  || { echo "No dist package. Run ./build.sh prod first."; exit 1; }
echo "Rack:    $RACK"
echo "Package: $(basename "$PKG")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/plugins-mac-arm64"
tar --use-compress-program=unzstd -xf "$PKG" -C "$TMP/plugins-mac-arm64/"

# Rack opens a window even in screenshot mode; it exits on its own when done.
"$RACK" -t "$ZOOM" -u "$TMP" >/dev/null 2>&1

SRC="$TMP/screenshots/SignalFunctionSet"
[ -d "$SRC" ] || { echo "Rack wrote no screenshots -- did the plugin load?"; exit 1; }

# Hidden modules are captured too; only the visible ones belong in the README.
python3 - "$ROOT" "$SRC" <<'PY'
import json, os, shutil, sys
root, src = sys.argv[1], sys.argv[2]
dst = os.path.join(root, "screenshots")
vis = [m["slug"] for m in json.load(open(os.path.join(root, "plugin.json")))["modules"]
       if not m.get("hidden")]
for slug in vis:
    p = os.path.join(src, slug + ".png")
    if not os.path.exists(p):
        print("  MISSING capture:", slug); continue
    shutil.copy2(p, os.path.join(dst, slug + ".png"))
stale = {f[:-4] for f in os.listdir(dst) if f.endswith(".png")} - set(vis)
print("%d screenshots refreshed" % len(vis))
if stale:
    print("  now-hidden modules still in screenshots/:", " ".join(sorted(stale)))
PY
