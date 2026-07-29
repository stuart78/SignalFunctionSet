#!/usr/bin/env python3
"""Build the disco bank for FILL.

Principles distilled from Joel Rothman, *Disco Drumming* (1977). The book's
architecture — not its exercises — is what is encoded here:

  1. The kick is four-on-the-floor and the snare is a backbeat. Both are close
     to invariant; the book prints them once as an instruction and then stops
     notating them.
  2. The style's vocabulary lives in WHERE THE HI-HAT OPENS. Four of its five
     parts are organised by hat resolution and opening position, not by tempo
     or sub-style.
  3. The upbeat is the canonical opening; openings on downbeats exist but are
     the uncommon case.
  4. Hat resolution sets the tempo: eighths run fast, one-handed sixteenths
     must run slower, and there are triplet and 6/8 feels besides.
  5. Against a FIXED hat, the two remaining axes are sixteenth-note bass-drum
     pickups and extra snare placements around the backbeat.
  6. Two-beat cells are the unit of construction; repeating one fills a 4/4
     bar, and pairing two different ones makes a two-bar phrase.

All grids below are written fresh from those principles. No exercise from the
book is transcribed.
"""
import json

STEP16, STEP12, STEP6 = 16, 12, 6

def row(n, **kw):
    """row(16, v={0:'9',4:'9'}) -> '9...9...........'"""
    out = {}
    for key, spec in kw.items():
        s = ['.'] * n
        for i, c in spec.items():
            s[i] = c
        out[key] = ''.join(s)
    return out

def every(n, start, stride, ch):
    return {i: ch for i in range(start, n, stride)}

# ── shared skeletons ────────────────────────────────────────────────────────
FOUR    = every(16, 0, 4, '9')                       # kick on every quarter
BACK    = {4: '9', 12: '9'}                          # snare on 2 and 4
EIGHTHS = every(16, 0, 2, '7')                       # closed hat, eighth notes

def hat(open_at, res=8):
    """Closed hat at `res` resolution with the open-hat steps taken out of it.
    A hat cannot be closed and open on the same step — that is the whole point
    of the notation the book uses."""
    stride = {8: 2, 16: 1}[res]
    closed = {i: ('7' if i % 4 == 0 else '6') for i in range(0, 16, stride)}
    for i in open_at:
        closed.pop(i, None)
    return closed, {i: '8' for i in open_at}

patterns, sets = [], []

def add(pid, name, grid, beats=4, spb=4, bars=1, meter="4/4", bpm=(112, 126), notes=""):
    patterns.append({
        "id": pid, "name": name, "family": "disco", "tags": ["disco", "fourfloor"],
        "meter": meter, "beats": beats, "stepsPerBeat": spb, "bars": bars,
        "swing": 0.0, "bpm": list(bpm), "grid": grid, "notes": notes,
    })

def addset(sid, name, bpm, notes):
    sets.append({
        "id": sid, "name": name, "family": "disco", "bpm": bpm, "vary": 0.2,
        "axis": "genre",
        "roles": {"sparse": sid + ".sparse", "main": sid + ".main",
                  "lift": sid + ".lift", "fill": [sid + ".fill"]},
        "arrangement": [
            {"role": "sparse", "bars": 4},
            {"role": "main", "bars": 8, "fillEvery": 4},
            {"role": "lift", "bars": 8, "fillEvery": 4},
            {"role": "main", "bars": 8},
            {"role": "sparse", "bars": 4},
        ],
        "notes": notes,
    })

def std_fill(pid, name, keep_floor=True):
    """Disco fills keep the floor: the four-on-the-floor is the identity, and
    dropping it is a different genre's gesture."""
    snare = {4: '9', 6: '5', 8: '7', 10: '6', 11: '5', 12: '9', 13: '7', 14: '8', 15: '9'}
    grid = {
        "kick": row(16, v=FOUR)["v"] if keep_floor else None,
        "snare": row(16, v=snare, a={4: 'A', 12: 'A', 15: 'A'}),
        "chh": row(16, v=every(16, 0, 2, '5')),
        "ohh": row(16, v={15: '9'}),
    }
    g = {"snare": grid["snare"], "chh": grid["chh"], "ohh": grid["ohh"]}
    if keep_floor:
        g["kick"] = {"v": grid["kick"]}
    add(pid, name, g, notes="roll into the downbeat; floor held")

# ── 1. the foundation: eighths, open on the & of 4 ──────────────────────────
c, o = hat([14])
add("disco.fourfloor.sparse", "Four on the floor — sparse",
    {"kick": row(16, v=FOUR), "chh": row(16, v=every(16, 0, 4, '6'))},
    notes="floor and a quarter-note hat; no backbeat yet")
add("disco.fourfloor.main", "Four on the floor — main",
    {"kick": row(16, v=FOUR), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c), "ohh": row(16, v=o)},
    notes="the canonical single opening: the & of 4, spilling into bar 1")
c2, o2 = hat([6, 14])
add("disco.fourfloor.lift", "Four on the floor — lift",
    {"kick": row(16, v=FOUR), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c2), "ohh": row(16, v=o2),
     "cp": row(16, v={4: '7', 12: '7'})},
    notes="second opening on the & of 2; clap doubles the backbeat")
std_fill("disco.fourfloor.fill", "Four on the floor — fill")
addset("disco.fourfloor", "Four on the floor", 118,
       "The foundation: kick every quarter, snare on 2 and 4, eighth hats "
       "opening on the & of 4.")

# ── 2. every upbeat open — the sizzle ───────────────────────────────────────
c, o = hat([2, 6, 10, 14])
add("disco.upbeats.sparse", "Open upbeats — sparse",
    {"kick": row(16, v=FOUR), "chh": row(16, v={0: '7', 8: '7'}),
     "ohh": row(16, v={6: '7', 14: '7'})},
    notes="half the openings, no backbeat")
add("disco.upbeats.main", "Open upbeats — main",
    {"kick": row(16, v=FOUR), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c), "ohh": row(16, v=o)},
    notes="hat opens on all four upbeats — the continuous disco sizzle")
add("disco.upbeats.lift", "Open upbeats — lift",
    {"kick": row(16, v=FOUR), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c), "ohh": row(16, v={i: '9' for i in (2, 6, 10, 14)}),
     "cp": row(16, v={4: '8', 12: '8'}),
     "hi": row(16, v=every(16, 0, 2, '4'))},
    notes="openings pushed to full; shaker fills the eighths")
std_fill("disco.upbeats.fill", "Open upbeats — fill")
addset("disco.upbeats", "Open upbeats", 122,
       "Hat opens on every upbeat. The book's most common placement, applied "
       "to all four beats at once.")

# ── 3. one-handed sixteenths — slower, per the book's own caveat ────────────
c16 = {i: ('8' if i % 4 == 0 else ('6' if i % 2 == 0 else '4')) for i in range(16)}
for i in (15,):
    c16.pop(i)
add("disco.sixteenth.sparse", "Sixteenth hat — sparse",
    {"kick": row(16, v=FOUR),
     "chh": row(16, v={i: ('7' if i % 4 == 0 else '4') for i in range(16)})},
    bpm=(98, 112), notes="hat alone carries the subdivision")
add("disco.sixteenth.main", "Sixteenth hat — main",
    {"kick": row(16, v=FOUR), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c16), "ohh": row(16, v={15: '8'})},
    bpm=(98, 112),
    notes="one hand on sixteenths, so slower; opens on the last sixteenth")
c16b = dict(c16); c16b.pop(7, None)
add("disco.sixteenth.lift", "Sixteenth hat — lift",
    {"kick": row(16, v=FOUR), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c16b), "ohh": row(16, v={7: '7', 15: '9'}),
     "cp": row(16, v={4: '7', 12: '7'})},
    bpm=(98, 112), notes="a second opening at the end of beat 2")
std_fill("disco.sixteenth.fill", "Sixteenth hat — fill")
addset("disco.sixteenth", "Sixteenth hat", 104,
       "Sixteenth-note hat played with one hand, which is why the tempo drops. "
       "Openings land on sixteenth positions rather than eighths.")

# ── 4. triplet hat over a straight floor ────────────────────────────────────
F12 = every(12, 0, 3, '9')
B12 = {3: '9', 9: '9'}
add("disco.shuffle.sparse", "Triplet hat — sparse",
    {"kick": row(12, v=F12), "chh": row(12, v=every(12, 0, 3, '6'))},
    spb=3, meter="4/4", bpm=(104, 118), notes="floor with the pulse only")
c12 = {i: ('7' if i % 3 == 0 else '5') for i in range(12)}; c12.pop(11)
add("disco.shuffle.main", "Triplet hat — main",
    {"kick": row(12, v=F12), "snare": row(12, v=B12, a={3: 'A', 9: 'A'}),
     "chh": row(12, v=c12), "ohh": row(12, v={11: '8'})},
    spb=3, meter="4/4", bpm=(104, 118),
    notes="hat in triplets against a straight four-on-the-floor")
c12b = dict(c12); c12b.pop(5, None)
add("disco.shuffle.lift", "Triplet hat — lift",
    {"kick": row(12, v=F12), "snare": row(12, v=B12, a={3: 'A', 9: 'A'}),
     "chh": row(12, v=c12b), "ohh": row(12, v={5: '7', 11: '9'}),
     "cp": row(12, v={3: '7', 9: '7'})},
    spb=3, meter="4/4", bpm=(104, 118), notes="opens twice per bar")
add("disco.shuffle.fill", "Triplet hat — fill",
    {"kick": row(12, v=F12),
     "snare": row(12, v={3: '9', 5: '5', 6: '7', 8: '6', 9: '9', 10: '7', 11: '9'},
                  a={3: 'A', 9: 'A', 11: 'A'}),
     "chh": row(12, v=every(12, 0, 3, '5')), "ohh": row(12, v={11: '9'})},
    spb=3, meter="4/4", bpm=(104, 118), notes="triplet roll, floor held")
addset("disco.shuffle", "Triplet hat", 110,
       "Triplet-feel hat over an unswung four-on-the-floor — the swung "
       "hand against the straight foot.")

# ── 5. the 6/8 feel: kick on 1 and 4, snare only on 4 ───────────────────────
add("disco.sixeight.sparse", "Six-eight feel — sparse",
    {"kick": row(12, v={0: '9', 3: '9', 6: '9', 9: '9'}),
     "chh": row(12, v=every(12, 0, 3, '6'))},
    beats=2, spb=3, bars=2, meter="6/8", bpm=(96, 112),
    notes="the two pulses, no backbeat")
c68 = {i: ('7' if i % 3 == 0 else '5') for i in range(12)}; c68.pop(11)
add("disco.sixeight.main", "Six-eight feel — main",
    {"kick": row(12, v={0: '9', 3: '9', 6: '9', 9: '9'}),
     "snare": row(12, v={3: '9', 9: '9'}, a={3: 'A', 9: 'A'}),
     "chh": row(12, v=c68), "ohh": row(12, v={11: '8'})},
    beats=2, spb=3, bars=2, meter="6/8", bpm=(96, 112),
    notes="kick on 1 and 4 of the six, snare only on 4 — the book's own "
          "instruction for this feel")
c68b = dict(c68); c68b.pop(5, None)
add("disco.sixeight.lift", "Six-eight feel — lift",
    {"kick": row(12, v={0: '9', 3: '9', 6: '9', 9: '9'}),
     "snare": row(12, v={3: '9', 9: '9'}, a={3: 'A', 9: 'A'}),
     "chh": row(12, v=c68b), "ohh": row(12, v={5: '7', 11: '9'}),
     "hi": row(12, v={i: '4' for i in range(12) if i % 3 != 0})},
    beats=2, spb=3, bars=2, meter="6/8", bpm=(96, 112),
    notes="second opening, shaker on the inner eighths")
add("disco.sixeight.fill", "Six-eight feel — fill",
    {"kick": row(12, v={0: '9', 3: '9', 6: '9'}),
     "snare": row(12, v={6: '7', 7: '5', 8: '6', 9: '9', 10: '7', 11: '9'},
                  a={9: 'A', 11: 'A'}),
     "chh": row(12, v=every(12, 0, 3, '5')), "ohh": row(12, v={11: '9'})},
    beats=2, spb=3, bars=2, meter="6/8", bpm=(96, 112),
    notes="compound-time turnaround")
addset("disco.sixeight", "Six-eight feel", 104,
       "Disco in compound time: kick on 1 and 4, snare only on 4.")

# ── 6. bass-drum pickups against a fixed hat ────────────────────────────────
c, o = hat([14])
PICK = dict(FOUR); PICK.update({7: '6', 15: '6'})
SPARSE_PICK = dict(FOUR); SPARSE_PICK[15] = '6'
add("disco.pickup.sparse", "Kick pickups — sparse",
    {"kick": row(16, v=SPARSE_PICK),
     "chh": row(16, v=every(16, 0, 2, '5'))},
    notes="one pickup, into the downbeat")
add("disco.pickup.main", "Kick pickups — main",
    {"kick": row(16, v=PICK), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c), "ohh": row(16, v=o)},
    notes="sixteenth pickups before beats 3 and 1 — the second axis, worked "
          "against an unchanging hat")
PICK2 = dict(PICK); PICK2.update({3: '5', 11: '5'})
add("disco.pickup.lift", "Kick pickups — lift",
    {"kick": row(16, v=PICK2), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=c2), "ohh": row(16, v=o2),
     "cp": row(16, v={4: '7', 12: '7'})},
    notes="a pickup before every backbeat as well")
std_fill("disco.pickup.fill", "Kick pickups — fill")
addset("disco.pickup", "Kick pickups", 116,
       "Four-on-the-floor with sixteenth-note bass-drum pickups, the hat held "
       "fixed so the foot is what varies.")

# ── 7. snare placements around the backbeat ─────────────────────────────────
add("disco.ghost.sparse", "Snare ghosts — sparse",
    {"kick": row(16, v=FOUR), "snare": row(16, v=BACK, a={4: 'A', 12: 'A'}),
     "chh": row(16, v=every(16, 0, 2, '5'))},
    notes="the backbeat clean")
GH = dict(BACK); GH.update({7: '3', 11: '3', 14: '4'})
add("disco.ghost.main", "Snare ghosts — main",
    {"kick": row(16, v=FOUR),
     "snare": row(16, v=GH, a={4: 'A', 12: 'A'}, p={7: '6', 11: '6', 14: '7'}),
     "chh": row(16, v=c), "ohh": row(16, v=o)},
    notes="ghosts around the backbeat, probabilistic so they breathe")
GH2 = dict(GH); GH2.update({2: '3', 6: '4', 10: '3', 15: '4'})
add("disco.ghost.lift", "Snare ghosts — lift",
    {"kick": row(16, v=FOUR),
     "snare": row(16, v=GH2, a={4: 'A', 12: 'A'},
                  p={2: '5', 6: '6', 7: '6', 10: '5', 11: '6', 14: '7', 15: '6'}),
     "chh": row(16, v=c2), "ohh": row(16, v=o2),
     "cp": row(16, v={4: '8', 12: '8'})},
    notes="ghost density up; clap reinforces the backbeat")
std_fill("disco.ghost.fill", "Snare ghosts — fill")
addset("disco.ghost", "Snare ghosts", 114,
       "The third axis: extra snare around the backbeat while kick and hat "
       "hold still.")

lib = {"formatVersion": 1,
       "lanes": ["kick", "snare", "chh", "ohh", "lo", "hi", "cp", "bell"],
       "patterns": patterns, "sets": sets}

import os
out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "res", "patterns", "drum-patterns-disco-v1.json")
with open(out, "w") as f:
    json.dump(lib, f, indent=1)
    f.write("\n")
print("wrote %s: %d patterns, %d sets" % (os.path.normpath(out), len(patterns), len(sets)))
