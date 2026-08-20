#!/usr/bin/env bash
# Static analysis, the way the VCV Library runs it.
#
# THE LIBRARY RUNS TWO CHECKERS, cppcheck AND clang-tidy. This script ran only
# cppcheck, which is how 2.18.0 shipped with fourteen clang-tidy findings in it
# (issue #12) after we had declared static analysis clean. cppcheck agreed with
# us exactly -- the gap was never a disagreement, it was a checker we simply
# never ran. If you add a checker to the pipeline, add it here first.
#
# Running these before submitting means finding our own bugs instead of being
# told about them, and it has earned that twice: cppcheck caught Fill importing
# a bank with an uninitialised taste struct and a read one past the end of the
# Hann table (issue #11), and clang-tidy caught a file-read loop that treated a
# disk error as end-of-file and imported the truncated result (issue #12).
#
# Vendored third-party sources are filtered out. dr_wav.h and dr_flac.h alone
# produce well over a hundred style notes that are not ours to fix, and the
# signal disappears underneath them. Everything under src/msfa is upstream
# Google code and is reported but flagged, since we do sometimes patch it.
#
# Exit status is the number of findings in our own code, so this can gate a
# release: `./tools/static-check.sh && ./build.sh prod`.
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
# ── clang-tidy ──────────────────────────────────────────────────────────────
# The Library runs the clang-analyzer-* checks. Everything else clang-tidy can
# do is off: the readability and modernize packs produce thousands of notes it
# does not report, and a checker that cries wolf is how a real finding gets
# missed.
TIDY="$(command -v clang-tidy || true)"
[ -n "$TIDY" ] || TIDY="$(ls /opt/homebrew/opt/llvm/bin/clang-tidy 2>/dev/null || true)"

tn=0
if [ -z "$TIDY" ]; then
    echo
    echo "clang-tidy NOT INSTALLED -- the Library runs it and this check is being"
    echo "skipped.  brew install llvm"
    echo "Findings it reports will arrive as a GitHub issue instead."
    tn=0
else
    traw=""
    for f in "$ROOT"/src/*.cpp; do
        case "$(basename "$f")" in dr_flac.cpp) continue ;; esac
        traw+="$("$TIDY" --quiet --checks='-*,clang-analyzer-*' "$f" -- \
                 -std=c++11 -I "$ROOT/src" -I "$RACK_DIR/include" \
                 -I "$RACK_DIR/dep/include" 2>/dev/null \
                 | grep -E "^$ROOT/src/[a-z_-]+\\.(cpp|hpp):" \
                 | grep -v "/msfa/" | sed "s|^$ROOT/||" || true)"
    done
    traw="$(printf '%s\n' "$traw" | grep . || true)"
    if [ -n "$traw" ]; then
        echo
        echo "── clang-tidy (ours) ──"
        printf '%s\n' "$traw"
        tn=$(printf '%s\n' "$traw" | wc -l | tr -d ' ')
    fi
fi

total=$((n + tn))
echo
echo "$n cppcheck + $tn clang-tidy = $total finding(s) in our own sources."
exit "$total"
