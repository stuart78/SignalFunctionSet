#!/usr/bin/env python3
"""Import GM drum-pattern MIDIs into an SFS drum-pattern library JSON.

Source collection: https://github.com/jmantra/LogicalArdour (MIT license)
  samples/Drum loops, chords, and chord progressions/drumpatterns/*.mid
466 one-bar grooves named by genre. Two conventions map onto SFS set roles:
  Genre_N_Measure_A / Genre_N_Measure_B / Genre_N_Break  -> main / lift / fill
  GenreN.mid + GenreBreakN.mid                           -> main + genre break pool

Usage: python3 import_midi.py <midi-dir> [out.json]

Grid: per-file best fit among 16ths (spb 4), 8th-triplets (spb 3) and
16th-triplets (spb 6); 3/4 honoured via the MIDI time-signature meta or a
"Waltz" name. GM note -> lane: 36 kick · 38 snare · 42 chh · 46 ohh ·
43/47 lo · 50/54 hi · 37/39 cp · 49/56 bell.
"""
import json, math, os, re, struct, sys
from collections import defaultdict

LANES = ["kick", "snare", "chh", "ohh", "lo", "hi", "cp", "bell"]
NOTE_LANE = {36: "kick", 35: "kick", 38: "snare", 40: "snare", 42: "chh", 44: "chh",
             46: "ohh", 41: "lo", 43: "lo", 45: "lo", 47: "lo", 48: "hi", 50: "hi",
             54: "hi", 69: "hi", 70: "hi", 37: "cp", 39: "cp", 33: "cp",
             49: "bell", 51: "bell", 52: "bell", 55: "bell", 56: "bell", 53: "bell"}

GENRE_BPM = {"afrocuban": 105, "ballad": 70, "blues": 84, "boogie": 132, "bossa": 128,
             "chacha": 118, "charleston": 200, "disco": 118, "endings": 120, "funk": 102,
             "jazz": 132, "march": 112, "pasodoble": 122, "pop": 116, "reggae": 76,
             "rnb": 88, "rock": 120, "samba": 102, "shuffle": 112, "ska": 132,
             "slow": 64, "swing": 140, "tango": 120, "twist": 152, "waltz": 92}

# Identity strength per genre (set "vary" 0..1): how far FILL's variation layer may
# bend these grooves. Strict dance forms stay low; funk/jazz breathe more.
GENRE_VARY = {"afrocuban": 0.3, "ballad": 0.3, "blues": 0.4, "boogie": 0.4, "bossa": 0.2,
              "chacha": 0.25, "charleston": 0.3, "disco": 0.45, "endings": 0.2, "funk": 0.6,
              "jazz": 0.5, "march": 0.2, "pasodoble": 0.25, "pop": 0.4, "reggae": 0.3,
              "rnb": 0.5, "rock": 0.4, "samba": 0.4, "shuffle": 0.4, "ska": 0.35,
              "slow": 0.3, "swing": 0.5, "tango": 0.2, "twist": 0.35, "waltz": 0.25}

# Region-rooted GM genres fold into the canonical v2 regional families, so each
# style lives in exactly one browser bank (set names keep the genre identity).
FAMILY_FOLD = {"afrocuban": "latin", "bossa": "latin", "samba": "latin",
               "chacha": "latin", "tango": "latin", "pasodoble": "europe",
               "reggae": "caribbean", "ska": "caribbean",
               # "Slow" is a tempo, not a genre: these are 12/8 slow-rock and
               # doo-wop ballads, so they belong with the other ballads.
               "slow": "ballad",
               # "Endings" is a song position -- one-shot cadences, not loops.
               "endings": "cadence"}
# Families shown on the Region tab; everything else is a Genre bank.
REGION_FAMILIES = {"latin", "caribbean", "africa", "mena", "europe", "asia",
                   "oceania", "usa", "indian"}


def set_family_axis(g):
    fam = FAMILY_FOLD.get(g, g)
    return fam, ("region" if fam in REGION_FAMILIES else "genre")


def read_varlen(d, i):
    v = 0
    while True:
        b = d[i]; i += 1; v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, i


def parse_midi(path):
    d = open(path, "rb").read()
    if d[:4] != b"MThd":
        raise ValueError("not SMF")
    _fmt, ntrk, ppq = struct.unpack(">HHH", d[8:14])
    i = 14
    notes, maxtick, tempo, tsig = [], 0, None, None
    for _ in range(ntrk):
        if d[i:i+4] != b"MTrk":
            raise ValueError("bad track")
        ln = struct.unpack(">I", d[i+4:i+8])[0]
        j, end, t, run = i + 8, i + 8 + ln, 0, 0
        while j < end:
            dt, j = read_varlen(d, j); t += dt
            st = d[j]
            if st & 0x80:
                run = st; j += 1
            else:
                st = run
            typ = st & 0xF0
            if st == 0xFF:
                mt = d[j]; j += 1; l, j = read_varlen(d, j)
                if mt == 0x51 and l == 3:
                    tempo = 60000000 / struct.unpack(">I", b"\0" + d[j:j+3])[0]
                elif mt == 0x58 and l >= 2:
                    tsig = (d[j], 1 << d[j+1])
                j += l
            elif st in (0xF0, 0xF7):
                l, j = read_varlen(d, j); j += l
            elif typ in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                a, b = d[j], d[j+1]; j += 2
                if typ == 0x90 and b > 0:
                    notes.append((t, a, b))
                    maxtick = max(maxtick, t)
            elif typ in (0xC0, 0xD0):
                j += 1
        i = end
    return ppq, notes, maxtick, tempo, tsig


def convert(path, name_hint):
    ppq, notes, maxtick, tempo, tsig = parse_midi(path)
    if not notes:
        return None
    beats = 3 if (tsig and tsig[0] == 3) or "waltz" in name_hint.lower() else 4
    bar_ticks = ppq * beats
    bars = max(1, math.ceil((maxtick + 1) / bar_ticks))
    if bars > 2:
        bars = 2                       # clamp outliers; content beyond 2 bars wraps off
    best = None
    for spb in (4, 3, 6):
        steps = beats * spb * bars
        tps = bar_ticks * bars / steps
        err = sum(min(t % tps, tps - t % tps) for t, _, _ in notes) / len(notes)
        if best is None or err < best[0] - 1e-6:
            best = (err, spb, steps, tps)
    _, spb, steps, tps = best
    vel = defaultdict(lambda: [0.0] * steps)
    acc = defaultdict(lambda: [False] * steps)
    skipped = 0

    def shape(lane, s, v):
        """Role-aware dynamics. The collection is authored almost entirely at
        velocity 64 (flat), so copying velocities yields lifeless grooves —
        instead shape by musical function. Real accents (v>=120) pass through."""
        if v >= 120:
            return 9, True
        pos = s % spb
        if lane == "kick":
            return (8 if s == 0 else 7 if pos == 0 else 6), False
        if lane == "snare":                # on-beat crack + accent; off-grid hits are ghosts
            return (8, True) if pos == 0 else (3, False)
        if lane == "chh":                  # beats > 8th-offbeats > 16ths
            return (6 if pos == 0 else 5 if pos * 2 == spb else 4), False
        if lane == "bell":
            return (6 if pos == 0 else 5), False
        return 6, False                    # ohh / toms / colour

    for t, n, v in notes:
        lane = NOTE_LANE.get(n)
        if not lane:
            skipped += 1
            continue
        s = int(round(t / tps)) % steps
        vv, ac = shape(lane, s, v)
        if vv / 9 > vel[lane][s]:
            vel[lane][s] = vv / 9
            acc[lane][s] = ac
    grid = {}
    for lane in LANES:
        if lane not in vel:
            continue
        vs = "".join("." if x == 0 else str(int(round(x * 9))) for x in vel[lane])
        entry = {"v": vs}
        if any(acc[lane]):
            entry["a"] = "".join("A" if a else "." for a in acc[lane])
        grid[lane] = entry
    if not grid:
        return None
    meter = "3/4" if beats == 3 else ("12/8" if spb == 3 else "4/4")
    return {"beats": beats, "spb": spb, "bars": bars, "meter": meter,
            "grid": grid, "tempo": tempo, "skipped": skipped}


def norm_genre(stem):
    g = re.sub(r"[_ ]?\d.*$", "", stem).replace("_", "").lower()
    g = g.replace("break", "")
    aliases = {"afrocub": "afrocuban", "bossanova": "bossa", "chacha": "chacha",
               "cha": "chacha", "rhythmblues": "rnb", "paso": "pasodoble",
               "ending": "endings", "endingsmeasurea": "endings", "endingsmeasureb": "endings",
               "skameasurea": "ska", "skameasureb": "ska", "chachameasurea": "chacha",
               "chachameasureb": "chacha"}
    return aliases.get(g, g)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "midi-gm"
    out = sys.argv[2] if len(sys.argv) > 2 else "drum-patterns-gm-v1.json"
    files = sorted(f for f in os.listdir(src) if f.lower().endswith(".mid"))
    patterns, sets = [], []
    pat_ids = set()
    # organise files
    triples = defaultdict(dict)          # (genre, n) -> {A, B, Break}
    singles = defaultdict(dict)          # genre -> {n: file}
    breakpool = defaultdict(list)        # genre -> [file]
    for f in files:
        stem = f[:-4]
        m = re.match(r"(.+?)_(\d+)_Measure_([AB])$", stem)
        if m:
            triples[(norm_genre(m.group(1)), int(m.group(2)))][m.group(3)] = f
            continue
        m = re.match(r"(.+?)_(\d+)_Break(?:-\d+)?$", stem)
        if m:
            triples[(norm_genre(m.group(1)), int(m.group(2)))].setdefault("F", f)
            continue
        m = re.match(r"(.+?)Break[_ ]?(\d+)?$", stem) or re.match(r"(.+?)_Break$", stem)
        if m and "break" in stem.lower():
            breakpool[norm_genre(m.group(1))].append(f)
            continue
        m = re.match(r"(.+?)[_ ]?(\d+)$", stem)
        if m:
            singles[norm_genre(m.group(1))][int(m.group(2))] = f
        else:
            singles[norm_genre(stem)][1] = f

    seen = {}                            # grid content -> first pattern id

    def add_pattern(pid, name, family, f, tags):
        c = convert(os.path.join(src, f), f)
        if c is None:
            return None
        key = (c["beats"], c["spb"], c["bars"],
               tuple(sorted((l, e["v"], e.get("a", "")) for l, e in c["grid"].items())))
        if key in seen:
            return seen[key]             # exact duplicate: reuse the first id, never store twice
        seen[key] = pid
        p = {"id": pid, "name": name, "family": family, "tags": tags,
             "meter": c["meter"], "beats": c["beats"], "stepsPerBeat": c["spb"],
             "bars": c["bars"], "swing": 0.0,
             "bpm": [GENRE_BPM.get(family, 110) - 10, GENRE_BPM.get(family, 110) + 15],
             "grid": c["grid"]}
        patterns.append(p); pat_ids.add(pid)
        return pid

    def title(g):
        names = {"afrocuban": "Afro-Cuban", "rnb": "R&B", "chacha": "Cha-cha",
                 "pasodoble": "Paso doble", "slow": "Slow rock (12/8)",
                 "endings": "Ending", "twist": "Twist (early-60s)",
                 "charleston": "Charleston (20s)", "boogie": "Boogie-woogie"}
        return names.get(g, g.capitalize())

    # triples -> full sets
    topn = defaultdict(int)              # highest set number used per genre
    for (g, n), parts in sorted(triples.items()):
        if "A" not in parts:
            continue
        base = f"gm.{g}.{n}"
        main = add_pattern(base + ".main", f"{title(g)} {n} — main", g, parts["A"], [g, "gm"])
        if main != base + ".main":
            continue                     # unreadable, or a duplicate of an earlier groove
        roles = {"main": main}
        if "B" in parts:
            lift = add_pattern(base + ".lift", f"{title(g)} {n} — lift", g, parts["B"], [g, "gm"])
            if lift and lift != main: roles["lift"] = lift
        if "F" in parts:
            fill = add_pattern(base + ".fill", f"{title(g)} {n} — fill", g, parts["F"], [g, "gm", "fill"])
            if fill and fill != main: roles["fill"] = [fill]
        topn[g] = max(topn[g], n)
        fam, axis = set_family_axis(g)
        sets.append({"id": base, "name": f"{title(g)} {n}", "family": fam,
                     "bpm": GENRE_BPM.get(g, 110), "vary": GENRE_VARY.get(g, 0.4),
                     "axis": axis, "roles": roles,
                     "arrangement": [{"role": "main", "bars": 8, "fillEvery": 4}]})

    # break pools (shared per genre)
    poolids = defaultdict(list)
    for g, fl in sorted(breakpool.items()):
        for k, f in enumerate(sorted(fl), 1):
            pid = add_pattern(f"gm.{g}.break{k}", f"{title(g)} break {k}", g, f, [g, "gm", "fill"])
            if pid and pid not in poolids[g]: poolids[g].append(pid)

    # singles -> sets with the genre break pool as fills; numbering continues
    # after the genre's triples so every "<Genre> N" label is unique
    for g, d in sorted(singles.items()):
        num = topn[g]
        for n, f in sorted(d.items()):
            base = f"gm.{g}.s{n}"
            main = add_pattern(base + ".main", f"{title(g)} {num + 1}", g, f, [g, "gm"])
            if main != base + ".main":
                continue                 # unreadable, or a duplicate of an earlier groove
            num += 1
            roles = {"main": main}
            if poolids[g]:
                roles["fill"] = poolids[g]
            fam, axis = set_family_axis(g)
            sets.append({"id": base, "name": f"{title(g)} {num}", "family": fam,
                         "bpm": GENRE_BPM.get(g, 110), "vary": GENRE_VARY.get(g, 0.4),
                         "axis": axis, "roles": roles,
                         "arrangement": [{"role": "main", "bars": 8, "fillEvery": 4}]})

    lib = {"formatVersion": 1,
           "meta": {"source": "jmantra/LogicalArdour drumpatterns (MIT). "
                              "Converted by import_midi.py; genre-named GM grooves."},
           "lanes": LANES, "patterns": patterns, "sets": sets}
    with open(out, "w") as f:
        json.dump(lib, f, indent=1, ensure_ascii=False)
    fams = sorted(set(s["family"] for s in sets))
    print(f"{len(patterns)} patterns, {len(sets)} sets, {len(fams)} families -> {out}")
    print("families:", ", ".join(fams))


if __name__ == "__main__":
    main()
