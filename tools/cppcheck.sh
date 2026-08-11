#!/usr/bin/env bash
# Static analysis, the way the VCV Library runs it.
#
# The Library's integration pipeline runs cppcheck on every submission and files
# a GitHub issue with whatever it finds (issue #11 was the first). Running it
# here first means finding our own bugs instead of being told about them, and it
# has already earned that: it caught Fill importing a bank with an uninitialised
# taste struct, and a read one past the end of the Hann table.
#
# Vendored third-party sources are filtered out. dr_wav.h and dr_flac.h alone
# produce well over a hundred style notes that are not ours to fix, and the
# signal disappears underneath them. Everything under src/msfa is upstream
# Google code and is reported but flagged, since we do sometimes patch it.
#
# Exit status is the number of findings in our own code, so this can gate a
# release: `./tools/cppcheck.sh && ./build.sh prod`.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RACK_DIR="${RACK_DIR:-$ROOT/../Rack-SDK}"

command -v cppcheck >/dev/null || {
    echo "cppcheck not found.  brew install cppcheck"; exit 127; }

# warning + performance + portability matches what the Library reports; error-
# level checks are always on. --enable=style adds several hundred "consider
# std::any_of" notes that it does not report, so it is left off here.
raw="$(cppcheck \
    --enable=warning,performance,portability \
    --inline-suppr --std=c++11 --quiet \
    --suppress=missingIncludeSystem --suppress=unusedFunction \
    -I "$ROOT/src" -I "$RACK_DIR/include" \
    "$ROOT/src" 2>&1 | grep "^src/\|^$ROOT/src/" | sed "s|^$ROOT/||")"

ours="$(   printf '%s\n' "$raw" | grep -v "^src/dr_wav.h\|^src/dr_flac.h\|^src/msfa/" | grep . || true)"
vendored="$(printf '%s\n' "$raw" | grep    "^src/msfa/" | grep . || true)"

if [ -n "$vendored" ]; then
    echo "── vendored (src/msfa, upstream Google DX7 code) ──"
    printf '%s\n' "$vendored"
    echo
fi

n=0
if [ -n "$ours" ]; then
    echo "── ours ──"
    printf '%s\n' "$ours"
    n=$(printf '%s\n' "$ours" | wc -l | tr -d ' ')
    echo
fi
echo "$n finding(s) in our own sources."
exit "$n"
