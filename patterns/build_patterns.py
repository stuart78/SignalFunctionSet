#!/usr/bin/env python3
"""Build drum-patterns-v1.json — SFS master drum pattern library.

Authoring convention matches fill.cpp's fillRow(): velocity strings with
'1'-'9' (= n/9) and '.', accent strings with 'A', plus optional probability
('1'-'9' = n/9 chance) and ratchet ('2'-'8' = retrigger count) rows.

Ruler for 16-step 4/4 grids:   1e&a2e&a3e&a4e&a
Ruler for 12-step 12/8 grids:  1..2..3..4..  (3 steps per pulse)
"""
import json
import os, sys

LANES = ["kick", "snare", "chh", "ohh", "lo", "hi", "cp", "bell"]

patterns = []
sets = []


def P(pid, name, family, tags, grid, beats=4, spb=4, bars=1, meter="4/4",
      swing=0.0, bpm=(100, 120), notes=None, kind="groove", source=None):
    steps = beats * spb * bars
    g = {}
    for lane, rows in grid.items():
        assert lane in LANES, f"{pid}: unknown lane {lane}"
        entry = {}
        for key, s in rows.items():
            assert key in ("v", "a", "p", "r"), f"{pid}.{lane}: bad row {key}"
            assert len(s) == steps, \
                f"{pid}.{lane}.{key}: len {len(s)} != steps {steps}: {s!r}"
            ok = {"v": ".123456789", "a": ".A", "p": ".123456789",
                  "r": ".2345678"}[key]
            assert all(c in ok for c in s), f"{pid}.{lane}.{key}: bad chars {s!r}"
            entry[key] = s
        assert "v" in entry, f"{pid}.{lane}: missing v row"
        g[lane] = entry
    p = {"id": pid, "name": name, "family": family, "tags": tags,
         "meter": meter, "beats": beats, "stepsPerBeat": spb, "bars": bars,
         "swing": swing, "bpm": list(bpm), "kind": kind, "grid": g}
    if notes:
        p["notes"] = notes
    if source:
        p["source"] = source
    patterns.append(p)


def T(pid, name, tags, grid, beats=4, spb=4, meter="4/4", bpm=(90, 130),
      notes=None, source=None):
    """A cyclic timeline / key pattern: standalone, no set."""
    P(pid, name, "timeline", tags, grid, beats=beats, spb=spb, meter=meter,
      bpm=bpm, notes=notes, kind="timeline", source=source)


# Browser axis per family: "genre" tab vs "region" tab.
FAMILY_AXIS = {
    "rock": "genre", "funk": "genre", "pop": "genre", "electronic": "genre",
    "hiphop": "genre", "timeline": "genre", "euclidean": "genre",
    "latin": "region", "caribbean": "region", "africa": "region", "mena": "region",
    "europe": "region", "asia": "region", "oceania": "region", "usa": "region",
    "indian": "region",
}

# Default identity strength per family (see format doc `vary`); timelines NEVER vary.
FAMILY_VARY = {
    "timeline": 0.0, "gamelan": 0.15, "indian": 0.2, "mena": 0.25, "asia": 0.25,
    "latin": 0.3, "africa": 0.3, "europe": 0.3, "oceania": 0.3, "euclidean": 0.3,
    "caribbean": 0.35, "rock": 0.4, "pop": 0.4, "usa": 0.4, "electronic": 0.45,
    "hiphop": 0.5, "funk": 0.6,
}


def S(sid, name, family, bpm, roles, arrangement, notes=None, vary=None, axis=None):
    s = {"id": sid, "name": name, "family": family, "bpm": bpm,
         "vary": FAMILY_VARY.get(family, 0.4) if vary is None else vary,
         "axis": axis or FAMILY_AXIS.get(family, "genre"),
         "roles": roles, "arrangement": arrangement}
    if notes:
        s["notes"] = notes
    sets.append(s)


def std_roles(prefix):
    return {"sparse": f"{prefix}.sparse", "main": f"{prefix}.main",
            "lift": f"{prefix}.lift", "fill": [f"{prefix}.fill"]}


MAIN_ARR = [{"role": "main", "bars": 8, "fillEvery": 4}]
MAIN_LIFT_ARR = [{"role": "main", "bars": 8, "fillEvery": 4},
                 {"role": "lift", "bars": 8, "fillEvery": 4},
                 {"role": "main", "bars": 8}]
STD_ARR = [
    {"role": "sparse", "bars": 4},
    {"role": "main", "bars": 8, "fillEvery": 4},
    {"role": "lift", "bars": 8, "fillEvery": 4},
    {"role": "main", "bars": 8},
    {"role": "sparse", "bars": 4},
]

# ═══════════════════════════ ROCK / FUNK / POP ═══════════════════════════
#                                  1e&a2e&a3e&a4e&a
P("rock.motorik.sparse", "Motorik — sparse", "rock", ["krautrock", "motorik"], {
    "kick":  {"v": "9.......9.......", "a": "A..............."},
    "snare": {"v": "....8.......8...", "a": "....A.......A..."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(110, 160))
P("rock.motorik.main", "Motorik — main", "rock", ["krautrock", "motorik"], {
    "kick":  {"v": "9.....7.9.......", "a": "A..............."},
    "snare": {"v": "....8.......8...", "a": "....A.......A..."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
    "ohh":   {"v": "..............6."},
}, bpm=(110, 160), notes="Dinger-beat stylization: forward kick lean, relentless 8th hats.")
P("rock.motorik.lift", "Motorik — lift", "rock", ["krautrock", "motorik"], {
    "kick":  {"v": "9.....7.9.....7.", "a": "A..............."},
    "snare": {"v": "....8.....3.8...", "a": "....A.......A..."},
    "chh":   {"v": "6565656565656565"},
    "ohh":   {"v": "..............7."},
    "hi":    {"v": "..5.......5....."},
}, bpm=(110, 160))
P("rock.motorik.fill", "Motorik — fill", "rock", ["krautrock", "fill"], {
    "kick":  {"v": "9.......9......."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
    "snare": {"v": "........55667789", "a": "...............A"},
}, bpm=(110, 160), notes="The pulse never stops; snare builds under the hats into the downbeat.")
S("rock.motorik", "Motorik", "rock", 132, std_roles("rock.motorik"), STD_ARR,
  notes="Default set. Krautrock pulse — the hats are the engine.", vary=0.1)

P("rock.basic.sparse", "Basic rock — sparse", "rock", ["rock", "pop"], {
    "kick":  {"v": "9.......7......."},
    "snare": {"v": "....8.......8..."},
    "chh":   {"v": "7...7...7...7..."},
}, bpm=(96, 132))
P("rock.basic.main", "Basic rock — main", "rock", ["rock", "pop", "backbeat"], {
    "kick":  {"v": "9.......9.9....."},
    "snare": {"v": "....9.......9...", "a": "....A.......A..."},
    "chh":   {"v": "8.6.8.6.8.6.8.6."},
}, bpm=(96, 132))
P("rock.basic.lift", "Basic rock — lift", "rock", ["rock", "pop"], {
    "kick":  {"v": "9.....7.9.9....."},
    "snare": {"v": "....9.......9..3", "a": "....A.......A..."},
    "ohh":   {"v": "8.7.8.7.8.7.8.7."},
    "bell":  {"v": "9..............."},
}, bpm=(96, 132), notes="Open hats replace closed; bell step 1 as crash proxy.")
P("rock.basic.fill", "Basic rock — fill", "rock", ["rock", "fill"], {
    "kick":  {"v": "9..............."},
    "snare": {"v": "....9...7788....", "a": "....A..........."},
    "chh":   {"v": "8.6.8.6........."},
    "hi":    {"v": "............88.."},
    "lo":    {"v": "..............99", "a": "..............AA"},
}, bpm=(96, 132), notes="Half-bar groove, then snare-tom run down to the floor.")
S("rock.basic", "Basic rock", "rock", 118, std_roles("rock.basic"), STD_ARR)

P("funk.sixteen.sparse", "Funk 16ths — sparse", "funk", ["funk"], {
    "kick":  {"v": "9..............."},
    "cp":    {"v": "....7.......7..."},
    "chh":   {"v": "7...7...7...7..."},
}, bpm=(92, 112), notes="Cross-stick on cp.")
P("funk.sixteen.main", "Funk 16ths — main", "funk", ["funk", "ghost-notes"], {
    "kick":  {"v": "9.6....7..8....."},
    "snare": {"v": "....9..3.3..9..3", "a": "....A.......A..."},
    "chh":   {"v": "7464746474647464"},
    "ohh":   {"v": "..............7."},
}, bpm=(92, 112))
P("funk.sixteen.lift", "Funk 16ths — lift", "funk", ["funk"], {
    "kick":  {"v": "9.6....7..8..6.."},
    "snare": {"v": "....9..3.3..9..3", "a": "....A.......A..."},
    "cp":    {"v": "....9.......9..."},
    "chh":   {"v": "7464746474647464"},
    "ohh":   {"v": "......7.......7."},
    "hi":    {"v": "6363636363636363"},
}, bpm=(92, 112), notes="Clap doubles the backbeat, shaker 16ths on hi.")
P("funk.sixteen.fill", "Funk 16ths — fill", "funk", ["funk", "fill"], {
    "kick":  {"v": "9.6....7........"},
    "snare": {"v": "....9...35357899", "a": "....A..........A"},
    "chh":   {"v": "74647464........"},
}, bpm=(92, 112))
S("funk.sixteen", "Funk sixteenths", "funk", 102, std_roles("funk.sixteen"), STD_ARR, vary=0.7)

P("pop.motown.sparse", "Motown four — sparse", "pop", ["motown", "soul"], {
    "kick":  {"v": "9.......9......."},
    "cp":    {"v": "....8.......8..."},
    "hi":    {"v": "7...7...7...7..."},
}, bpm=(112, 126), notes="Tambourine on hi.")
P("pop.motown.main", "Motown four — main", "pop", ["motown", "soul"], {
    "kick":  {"v": "9...8...9...8..."},
    "snare": {"v": "8...9...8...9...", "a": "....A.......A..."},
    "chh":   {"v": "7.5.7.5.7.5.7.5."},
    "hi":    {"v": "7.5.7.5.7.5.7.57"},
}, bpm=(112, 126), notes="Snare on all four, heavier on 2 and 4.")
P("pop.motown.lift", "Motown four — lift", "pop", ["motown", "soul"], {
    "kick":  {"v": "9...8...9...8..."},
    "snare": {"v": "8...9...8...9...", "a": "....A.......A..."},
    "cp":    {"v": "....9.......9..."},
    "chh":   {"v": "7.5.7.5.7.5.7.5."},
    "ohh":   {"v": "..............7."},
    "hi":    {"v": "7575757575757575"},
}, bpm=(112, 126), notes="Tambourine moves to 16ths, claps join the backbeat.")
P("pop.motown.fill", "Motown four — fill", "pop", ["motown", "fill"], {
    "kick":  {"v": "9...8...9......."},
    "snare": {"v": "5.5.6.6.7.7.8899", "a": "...............A"},
    "hi":    {"v": "7.5.7.5.7.5.7.5."},
}, bpm=(112, 126))
S("pop.motown", "Motown four-on-the-snare", "pop", 118, std_roles("pop.motown"), STD_ARR)

# 12/8 shuffle — 12 steps, 3 per pulse.  Ruler: 1..2..3..4..
P("rock.shuffle.sparse", "Blues shuffle — sparse", "rock", ["shuffle", "blues", "triplet"], {
    "kick":  {"v": "9..........."},
    "cp":    {"v": "...8.....8.."},
    "chh":   {"v": "7..7..7..7.."},
}, beats=4, spb=3, meter="12/8", bpm=(66, 92))
P("rock.shuffle.main", "Blues shuffle — main", "rock", ["shuffle", "blues", "triplet"], {
    "kick":  {"v": "9.....9....."},
    "snare": {"v": "...9.....9..", "a": "...A.....A.."},
    "chh":   {"v": "7.57.57.57.5"},
}, beats=4, spb=3, meter="12/8", bpm=(66, 92),
  notes="Hat plays 1-and-a shuffle (skips the middle triplet).")
P("rock.shuffle.lift", "Blues shuffle — lift", "rock", ["shuffle", "blues", "triplet"], {
    "kick":  {"v": "9.....9....5"},
    "snare": {"v": "...9.....9..", "a": "...A.....A.."},
    "bell":  {"v": "9.59.59.59.5"},
    "chh":   {"v": "5..5..5..5.."},
}, beats=4, spb=3, meter="12/8", bpm=(66, 92), notes="Shuffle moves to the ride; hat foot on quarters.")
P("rock.shuffle.fill", "Blues shuffle — fill", "rock", ["shuffle", "fill", "triplet"], {
    "kick":  {"v": "9..........."},
    "snare": {"v": "445566778899", "a": "...........A"},
}, beats=4, spb=3, meter="12/8", bpm=(66, 92), notes="Full-bar triplet roll, crescendo into the downbeat.")
S("rock.shuffle", "Blues shuffle (12/8)", "rock", 76, std_roles("rock.shuffle"), STD_ARR)

# ═══════════════════════════════ ELECTRONIC ═══════════════════════════════
P("edm.house.sparse", "Classic house — sparse", "electronic", ["house"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(120, 128))
P("edm.house.main", "Classic house — main", "electronic", ["house"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "cp":    {"v": "....9.......9..."},
    "ohh":   {"v": "..7...7...7...7."},
    "chh":   {"v": "5.4.5.4.5.4.5.4."},
    "hi":    {"v": "3434343434343434"},
}, bpm=(120, 128), notes="Offbeat opens, claps on 2/4, shaker 16ths underneath.")
P("edm.house.lift", "Classic house — lift", "electronic", ["house"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "....8.......8..."},
    "cp":    {"v": "....9.......9..."},
    "ohh":   {"v": "..7...7...7...7."},
    "chh":   {"v": "5454545454545454"},
    "hi":    {"v": "4343434343434343"},
    "lo":    {"v": "...7......7....."},
}, bpm=(120, 128))
P("edm.house.fill", "Classic house — fill", "electronic", ["house", "fill"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "5555666677778888"},
    "cp":    {"v": "....9.......9..."},
}, bpm=(120, 128), notes="Snare build over an unbroken four-on-the-floor.")
S("edm.house", "Classic house", "electronic", 124, std_roles("edm.house"),
  [{"role": "sparse", "bars": 8}, {"role": "main", "bars": 16, "fillEvery": 8},
   {"role": "lift", "bars": 16, "fillEvery": 8}, {"role": "main", "bars": 8},
   {"role": "sparse", "bars": 8}])

P("edm.techno.sparse", "Driving techno — sparse", "electronic", ["techno"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "chh":   {"v": "4.4.4.4.4.4.4.4."},
    "lo":    {"v": "..3.....3.....3."},
}, bpm=(128, 140))
P("edm.techno.main", "Driving techno — main", "electronic", ["techno"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "ohh":   {"v": "..7...7...7...7."},
    "chh":   {"v": "5353535353535353"},
    "lo":    {"v": "..5.....5....5.."},
    "cp":    {"v": ".........6......"},
}, bpm=(128, 140))
P("edm.techno.lift", "Driving techno — lift", "electronic", ["techno"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "....7.......7..."},
    "ohh":   {"v": "..7...7...7...7."},
    "chh":   {"v": "5454545454545454"},
    "bell":  {"v": "6.6.6.6.6.6.6.6."},
    "lo":    {"v": "..5.....5....5.."},
}, bpm=(128, 140), notes="Ride 8ths on bell for the peak section.")
P("edm.techno.fill", "Driving techno — fill", "electronic", ["techno", "fill"], {
    "kick":  {"v": "9...9...9......."},
    "snare": {"v": "........55667788", "a": "...............A"},
    "ohh":   {"v": "..7...7...7...7."},
    "chh":   {"v": "53535353........"},
}, bpm=(128, 140), notes="Kick drops out under the snare rush — tension, not addition.")
S("edm.techno", "Driving techno", "electronic", 134, std_roles("edm.techno"),
  [{"role": "sparse", "bars": 8}, {"role": "main", "bars": 16, "fillEvery": 8},
   {"role": "lift", "bars": 16, "fillEvery": 8}, {"role": "main", "bars": 8},
   {"role": "sparse", "bars": 8}])

P("edm.dnb.sparse", "DnB two-step — sparse", "electronic", ["dnb", "halftime"], {
    "kick":  {"v": "9..............."},
    "snare": {"v": "........8......."},
    "chh":   {"v": "6.4.6.4.6.4.6.4."},
}, bpm=(168, 178), notes="Half-time intro feel.")
P("edm.dnb.main", "DnB two-step — main", "electronic", ["dnb", "jungle"], {
    "kick":  {"v": "9.........9....."},
    "snare": {"v": "....9.3....39...", "a": "....A.......A..."},
    "chh":   {"v": "6353635363536353"},
    "ohh":   {"v": "..............5."},
}, bpm=(168, 178), notes="Two-step: kick 1 and the 'and' of 3; ghosts around the backbeat.")
P("edm.dnb.lift", "DnB two-step — lift", "electronic", ["dnb", "jungle"], {
    "kick":  {"v": "9.........9..7.."},
    "snare": {"v": "....9.3..3.39..3", "a": "....A.......A..."},
    "chh":   {"v": "6454645464546454"},
    "bell":  {"v": "5.5.5.5.5.5.5.5."},
    "ohh":   {"v": "..............6."},
}, bpm=(168, 178))
P("edm.dnb.fill", "DnB two-step — fill", "electronic", ["dnb", "fill"], {
    "kick":  {"v": "9.........9....."},
    "snare": {"v": "....9...56678899", "a": "....A..........A"},
    "chh":   {"v": "63536353........"},
}, bpm=(168, 178))
S("edm.dnb", "Drum & bass two-step", "electronic", 174, std_roles("edm.dnb"), STD_ARR, vary=0.6)

P("hiphop.boombap.sparse", "Boom bap — sparse", "hiphop", ["boombap"], {
    "kick":  {"v": "9..............."},
    "cp":    {"v": "............8..."},
    "chh":   {"v": "6...6...6...6..."},
}, swing=0.35, bpm=(84, 96))
P("hiphop.boombap.main", "Boom bap — main", "hiphop", ["boombap"], {
    "kick":  {"v": "9..3..6...9..3..", "a": "A.....A........."},
    "snare": {"v": "....9...3...9..3", "a": "....A.......A..."},
    "chh":   {"v": "7.5.7.4.7.5.7.4."},
    "ohh":   {"v": "..............6."},
}, swing=0.40, bpm=(84, 96),
   notes="Ghost kick on the 16th before a main hit gives the 'ba-boom'; "
         "ghost snares at 3/9 velocity between the backbeats.")
P("hiphop.boombap.lift", "Boom bap — lift", "hiphop", ["boombap"], {
    "kick":  {"v": "9......6..9..6.."},
    "snare": {"v": "....9.......9..3", "a": "....A.......A..."},
    "chh":   {"v": "7.5.7.5.7.5.7.5."},
    "ohh":   {"v": "......6.......6."},
    "hi":    {"v": "..3...3...3...3."},
}, swing=0.35, bpm=(84, 96))
P("hiphop.boombap.fill", "Boom bap — fill", "hiphop", ["boombap", "fill"], {
    "kick":  {"v": "9......6........"},
    "snare": {"v": "....9.......9899", "a": "....A.......A..."},
    "chh":   {"v": "7.5.7.5.7.5....."},
}, swing=0.35, bpm=(84, 96), notes="Snare stutter at the bar's tail rather than a run.")
S("hiphop.boombap", "Boom bap", "hiphop", 90, std_roles("hiphop.boombap"), STD_ARR, vary=0.6)

P("hiphop.trap.sparse", "Trap halftime — sparse", "hiphop", ["trap", "halftime"], {
    "kick":  {"v": "9..............."},
    "snare": {"v": "........9.......", "a": "........A......."},
    "chh":   {"v": "6...6...6...6..."},
}, bpm=(130, 150))
P("hiphop.trap.main", "Trap halftime — main", "hiphop", ["trap", "halftime"], {
    "kick":  {"v": "9.....7....9...."},
    "snare": {"v": "........9.......", "a": "........A......."},
    "chh":   {"v": "6.6.6.6.6.6.6.6.", "r": "......2.......2."},
}, bpm=(130, 150), notes="Snare only on beat 3 (halftime); ratchets are hat rolls.")
P("hiphop.trap.lift", "Trap halftime — lift", "hiphop", ["trap", "halftime"], {
    "kick":  {"v": "9.....7....9...."},
    "snare": {"v": "........9.......", "a": "........A......."},
    "cp":    {"v": "........9......."},
    "chh":   {"v": "6666666666666666", "r": "....2.......3..."},
    "ohh":   {"v": "..............6."},
}, bpm=(130, 150))
P("hiphop.trap.fill", "Trap halftime — fill", "hiphop", ["trap", "fill"], {
    "kick":  {"v": "9..............."},
    "snare": {"v": "........9...5678", "a": "........A......."},
    "chh":   {"v": "6.6.6.6.66667777", "r": "............2222"},
}, bpm=(130, 150))
S("hiphop.trap", "Trap halftime", "hiphop", 140, std_roles("hiphop.trap"), STD_ARR)

# ═══════════════════ LATIN / AFRO-CUBAN / BRAZILIAN ═══════════════════
# One 16-step bar carries the full 2-bar clave cycle in 8th-note pairs —
# the standard step-sequencer convention (son 3-2: steps 1,4,7,11,13).
P("latin.son.sparse", "Son montuno — sparse", "latin", ["salsa", "son", "clave-3-2"], {
    "cp":    {"v": "9..9..9...9.9...", "a": "A..A..A........."},
    "chh":   {"v": "5.5.5.5.5.5.5.5."},
    "bell":  {"v": "7.......7......."},
}, bpm=(88, 108), notes="cp = claves (3-2 son); chh = maracas.")
P("latin.son.main", "Son montuno — main", "latin", ["salsa", "son", "tumbao"], {
    "kick":  {"v": "......7.....9..."},
    "cp":    {"v": "9..9..9...9.9...", "a": "A..A..A........."},
    "hi":    {"v": "..3.9..3..3.8.8.", "a": "....A..........."},
    "chh":   {"v": "5.5.5.5.5.5.5.5."},
    "bell":  {"v": "9...7...9...7..."},
}, bpm=(88, 108),
  notes="Conga tumbao on hi: slap on 2, open tones on beat 4 (steps 12,14) per pedagogy sources; kick = bombo (and-of-2, 3-side) / ponche (beat 4).")
P("latin.son.lift", "Son montuno — lift", "latin", ["salsa", "mambo"], {
    "kick":  {"v": "......7.....9..."},
    "cp":    {"v": "9..9..9...9.9...", "a": "A..A..A........."},
    "hi":    {"v": "..3.9..3..3.8.88", "a": "....A..........."},
    "chh":   {"v": "5454545454545454"},
    "bell":  {"v": "9..7.7.99..7.7.9"},
}, bpm=(88, 108), notes="Stylized mambo-section bell; maracas to 16ths.")
P("latin.son.fill", "Son montuno — fill", "latin", ["salsa", "fill"], {
    "kick":  {"v": "......7.....9..."},
    "cp":    {"v": "9..9..9...9.9...", "a": "A..A..A........."},
    "snare": {"v": "........55667789", "a": "...............A"},
    "lo":    {"v": "............7.9."},
}, bpm=(88, 108), notes="Timbale-style roll into the downbeat. Clave never stops.")
S("latin.son", "Son montuno (3-2)", "latin", 96, std_roles("latin.son"), STD_ARR,
  notes="Join sections only on clave-cycle boundaries.", vary=0.2)

P("latin.samba.sparse", "Samba — sparse", "latin", ["samba", "brazil"], {
    "kick":  {"v": "....9.......9..8", "a": "....A.......A..."},
    "hi":    {"v": "6467646764676467"},
}, swing=0.2, bpm=(96, 116), notes="kick = surdo answer on 2 and 4 with pickup; hi = ganzá. Swing approximates the samba 16th feel.")
P("latin.samba.main", "Samba — main", "latin", ["samba", "brazil"], {
    "kick":  {"v": "....9.......9..8", "a": "....A.......A..."},
    "snare": {"v": "4449444944494449"},
    "cp":    {"v": "9..9..9.9..9..9."},
    "hi":    {"v": "6467646764676467"},
}, swing=0.2, bpm=(96, 116), notes="snare = caixa; cp = stylized tamborim telecoteco.")
P("latin.samba.lift", "Samba — lift", "latin", ["samba", "batucada"], {
    "kick":  {"v": "....9.......9..8", "a": "....A.......A..."},
    "snare": {"v": "4449444944494449"},
    "cp":    {"v": "9..9..9.9..9..9."},
    "lo":    {"v": "..6.....6.....6."},
    "hi":    {"v": "6467646764676467"},
    "bell":  {"v": "9..7..9..7..9..7"},
}, swing=0.2, bpm=(96, 116), notes="lo = third-surdo cuts; bell = agogô cross-rhythm.")
P("latin.samba.fill", "Samba — fill", "latin", ["samba", "fill"], {
    "kick":  {"v": "....9.......99..", "a": "....A.......AA.."},
    "snare": {"v": "5555666677778888", "a": "...............A"},
    "hi":    {"v": "6467646764676467"},
}, swing=0.2, bpm=(96, 116), notes="Caixa crescendo; ganzá never stops.")
S("latin.samba", "Samba", "latin", 104, std_roles("latin.samba"), STD_ARR)

P("latin.bossa.sparse", "Bossa nova — sparse", "latin", ["bossa", "brazil"], {
    "kick":  {"v": "9.....9.9.....9."},
    "cp":    {"v": "9..9..9...9..9.."},
}, bpm=(58, 84), notes="cp = rim, playing the bossa clave.")
P("latin.bossa.main", "Bossa nova — main", "latin", ["bossa", "brazil"], {
    "kick":  {"v": "9.....9.9.....9."},
    "cp":    {"v": "9..9..9...9..9.."},
    "chh":   {"v": "6.4.6.4.6.4.6.4."},
}, bpm=(58, 84))
P("latin.bossa.lift", "Bossa nova — lift", "latin", ["bossa", "brazil"], {
    "kick":  {"v": "9.....9.9.....9."},
    "cp":    {"v": "9..9..9...9..9.."},
    "bell":  {"v": "6.6.6.6.6.6.6.6."},
    "hi":    {"v": "3232323232323232"},
}, bpm=(58, 84), notes="Ride 8ths and soft shaker for the lift — bossa peaks are gentle.")
P("latin.bossa.fill", "Bossa nova — fill", "latin", ["bossa", "fill"], {
    "kick":  {"v": "9.....9.9......."},
    "snare": {"v": "..........335678"},
    "cp":    {"v": "9..9..9........."},
    "chh":   {"v": "6.4.6.4.6.4....."},
}, bpm=(58, 84), notes="A soft brushed drag, not a rock run.")
S("latin.bossa", "Bossa nova", "latin", 72, std_roles("latin.bossa"), STD_ARR, vary=0.2)

P("latin.dembow.sparse", "Dembow — sparse", "latin", ["reggaeton", "dembow"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "hi":    {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(88, 102))
P("latin.dembow.main", "Dembow — main", "latin", ["reggaeton", "dembow"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "...9..9....9..9."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
    "hi":    {"v": "3434343434343434"},
}, bpm=(88, 102), notes="The dembow snare: 3-3-2 across each half-bar.")
P("latin.dembow.lift", "Dembow — lift", "latin", ["reggaeton", "dembow"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "...9..9....9..9."},
    "cp":    {"v": "....9.......9..."},
    "chh":   {"v": "6464646464646464"},
    "hi":    {"v": "3434343434343434"},
    "ohh":   {"v": "..............7."},
}, bpm=(88, 102))
P("latin.dembow.fill", "Dembow — fill", "latin", ["reggaeton", "fill"], {
    "kick":  {"v": "9...9...9......."},
    "snare": {"v": "...9..9.55667889"},
    "chh":   {"v": "6.6.6.6........."},
}, bpm=(88, 102))
S("latin.dembow", "Dembow / reggaetón", "latin", 94, std_roles("latin.dembow"), STD_ARR, vary=0.3)

# ═══════════════ AFRICAN / CARIBBEAN / GLOBAL ═══════════════
# Bembé 12/8 — the seven-stroke standard bell over 12 pulses.
P("africa.bembe.sparse", "Bembé 12/8 — sparse", "africa", ["bembe", "west-african", "12-8"], {
    "bell":  {"v": "9.7.77.7.7.7"},
    "hi":    {"v": "6..6..6..6.."},
}, beats=4, spb=3, meter="12/8", bpm=(100, 130),
  notes="Standard bell (X.X.XX.X.X.X); hi = shekere on the dotted-quarter pulse.")
P("africa.bembe.main", "Bembé 12/8 — main", "africa", ["bembe", "west-african", "12-8"], {
    "bell":  {"v": "9.7.77.7.7.7"},
    "kick":  {"v": "9..5..8..5.."},
    "lo":    {"v": "..5.7...5.7."},
    "hi":    {"v": "6..6..6..6.."},
}, beats=4, spb=3, meter="12/8", bpm=(100, 130),
  notes="Low drum on the pulse, conga tones answering off it.")
P("africa.bembe.lift", "Bembé 12/8 — lift", "africa", ["bembe", "west-african", "12-8"], {
    "bell":  {"v": "9.7.77.7.7.7"},
    "kick":  {"v": "9..5..8..5.5"},
    "lo":    {"v": "..5.7...5.7."},
    "hi":    {"v": "6.36.36.36.3"},
    "cp":    {"v": "...9.....9.."},
}, beats=4, spb=3, meter="12/8", bpm=(100, 130))
P("africa.bembe.fill", "Bembé 12/8 — fill", "africa", ["bembe", "fill", "12-8"], {
    "bell":  {"v": "9.7.77.7.7.7"},
    "kick":  {"v": "9..........."},
    "lo":    {"v": "...45677899.", "a": "..........A."},
}, beats=4, spb=3, meter="12/8", bpm=(100, 130), notes="Drum flourish under an unbroken bell.")
S("africa.bembe", "Bembé (12/8)", "africa", 112, std_roles("africa.bembe"), STD_ARR,
  notes="The bell never stops across any section change.", vary=0.2)

P("africa.afrobeat.sparse", "Afrobeat — sparse", "africa", ["afrobeat"], {
    "kick":  {"v": "9..............."},
    "cp":    {"v": "....7.....7....."},
    "chh":   {"v": "5454545454545454"},
}, bpm=(100, 126))
P("africa.afrobeat.main", "Afrobeat — main", "africa", ["afrobeat"], {
    "kick":  {"v": "9......7..7....."},
    "snare": {"v": "..3....3.3....3."},
    "cp":    {"v": "....7.....7....."},
    "chh":   {"v": "6454645464546454"},
    "ohh":   {"v": "......6.......6."},
    "bell":  {"v": "9..9..9.9..9...."},
}, bpm=(100, 126), notes="Broken kick, ghosted snare, busy hats — stylized Afrobeat kit part.")
P("africa.afrobeat.lift", "Afrobeat — lift", "africa", ["afrobeat"], {
    "kick":  {"v": "9..4...7..7...4."},
    "snare": {"v": "..3....3.3....3."},
    "cp":    {"v": "....7.....7....."},
    "chh":   {"v": "6454645464546454"},
    "ohh":   {"v": "..6...6...6...6."},
    "bell":  {"v": "9..9..9.9..9..9."},
    "hi":    {"v": "3.3.3.3.3.3.3.3."},
}, bpm=(100, 126))
P("africa.afrobeat.fill", "Afrobeat — fill", "africa", ["afrobeat", "fill"], {
    "kick":  {"v": "9......7........"},
    "snare": {"v": "..3....3........"},
    "lo":    {"v": "........66.7.99."},
    "bell":  {"v": "9..9..9.9..9...."},
    "chh":   {"v": "64546454........"},
}, bpm=(100, 126), notes="Tom conversation; bell keeps the topline.")
S("africa.afrobeat", "Afrobeat", "africa", 112, std_roles("africa.afrobeat"), STD_ARR)

P("carib.onedrop.sparse", "Reggae one drop — sparse", "caribbean", ["reggae"], {
    "chh":   {"v": "7.4.7.4.7.4.7.4."},
    "cp":    {"v": "........8......."},
}, swing=0.15, bpm=(58, 80))
P("carib.onedrop.main", "Reggae one drop — main", "caribbean", ["reggae", "one-drop"], {
    "kick":  {"v": "........9.......", "a": "........A......."},
    "cp":    {"v": "........8......."},
    "chh":   {"v": "7.4.7.4.7.4.7.4."},
}, swing=0.15, bpm=(58, 80),
  notes="Everything lands on beat 3; beat 1 stays empty — that IS the one drop.")
P("carib.onedrop.lift", "Reggae steppers — lift", "caribbean", ["reggae", "steppers"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "cp":    {"v": "........8......."},
    "chh":   {"v": "7.4.7.4.7.4.7.4."},
    "ohh":   {"v": "..............7."},
}, swing=0.15, bpm=(58, 80), notes="The lift is the steppers variant: four-on-the-floor kick.")
P("carib.onedrop.fill", "Reggae one drop — fill", "caribbean", ["reggae", "fill"], {
    "kick":  {"v": "........9......."},
    "snare": {"v": "........5566...."},
    "lo":    {"v": "............7789"},
    "chh":   {"v": "7.4.7.4........."},
}, swing=0.15, bpm=(58, 80))
S("carib.onedrop", "Reggae one drop / steppers", "caribbean", 70, std_roles("carib.onedrop"), STD_ARR, vary=0.3)

P("mena.maqsum.sparse", "Maqsum — sparse", "mena", ["maqsum", "middle-eastern"], {
    "kick":  {"v": "9.......7......."},
    "cp":    {"v": "..8...8.....8..."},
}, bpm=(92, 112), notes="kick = dum, cp = tek. The maqsum skeleton: D T . T D . T .")
P("mena.maqsum.main", "Maqsum — main", "mena", ["maqsum", "middle-eastern"], {
    "kick":  {"v": "9.......7......."},
    "cp":    {"v": "..8...8.....8..."},
    "hi":    {"v": ".....3.3..3...3."},
}, bpm=(92, 112), notes="hi = ka finger ornaments between the skeleton strokes.")
P("mena.maqsum.lift", "Baladi — lift", "mena", ["baladi", "middle-eastern"], {
    "kick":  {"v": "9.7.....7......."},
    "cp":    {"v": "..8...8.....8.8."},
    "hi":    {"v": "2323232323232323"},
    "bell":  {"v": "6.......6......."},
}, bpm=(92, 112), notes="Lift is the heavier baladi cousin (doubled dum); hi = riqq; bell = finger cymbals.")
P("mena.maqsum.fill", "Maqsum — fill", "mena", ["maqsum", "fill"], {
    "kick":  {"v": "9..............."},
    "cp":    {"v": "..8...8........."},
    "hi":    {"v": "..3344556677889.", "a": "..............A."},
}, bpm=(92, 112), notes="Darbuka roll crescendo.")
S("mena.maqsum", "Maqsum / baladi", "mena", 100, std_roles("mena.maqsum"), STD_ARR, vary=0.3)

# ═════════════════════ V2: GLOBAL TIMELINES ═════════════════════
# Cyclic key patterns (kind="timeline"). Placements verified against
# Toussaint (BRIDGES 2003 / Geometry of Musical Rhythm) and the sources in
# global-drum-patterns-report.md unless noted stylized.
TOUS = "Toussaint, African Ternary Rhythm Timelines / Geometry of Musical Rhythm"

# 16-pulse binary family                 1e&a2e&a3e&a4e&a
T("tl.clave.son",   "Son clave (3-2)",   ["cuba", "clave"],
  {"cp": {"v": "9..9..9...9.9..."}}, source=TOUS)
T("tl.clave.rumba", "Rumba clave (3-2)", ["cuba", "clave"],
  {"cp": {"v": "9..9...9..9.9..."}}, source=TOUS)
T("tl.clave.bossa", "Bossa clave",       ["brazil", "clave"],
  {"cp": {"v": "9..9..9...9..9.."}}, source=TOUS)
T("tl.bell.shiko",  "Shiko bell",        ["nigeria", "bell"],
  {"bell": {"v": "9...9.9...9.9..."}}, source=TOUS)
T("tl.bell.soukous", "Soukous bell",     ["congo", "bell"],
  {"bell": {"v": "9..9..9..99....."}}, source=TOUS,
  notes="Clustered strokes 9,10 give the characteristic limp.")
T("tl.bell.gahu",   "Gahu bell",         ["ghana", "ewe", "bell"],
  {"bell": {"v": "9..9..9...9...9."}}, source=TOUS)

# 8-pulse cells (2/4 bar; double for a 16 grid)     1e&a2e&a
T("tl.cell.tresillo",  "Tresillo",  ["cell", "3+3+2"],
  {"cp": {"v": "9..9..9."}}, beats=2, meter="2/4", source=TOUS,
  notes="Seed of dembow, baiao, malfuf, habanera.")
T("tl.cell.cinquillo", "Cinquillo", ["cell", "cuba", "haiti"],
  {"cp": {"v": "9.99.99."}}, beats=2, meter="2/4", source=TOUS,
  notes="Danzon baqueteo cell; Haitian ti-bwa; klezmer bulgar.")
T("tl.cell.habanera",  "Habanera",  ["cell", "cuba", "argentina"],
  {"cp": {"v": "9..99.9."}}, beats=2, meter="2/4", source=TOUS,
  notes="Tango/milonga cell; early jazz 'Spanish tinge'.")

# 12-pulse ternary bell family (beats=4, spb=3)   1.a2.a3.a4.a
TERN = [
    ("standard", "Standard pattern (Bembe bell)", "9.7.77.7.7.7",
     "Ewe/Yoruba; Cuban bembe. Rotation start disagreement Anlo-Ewe vs Yoruba (Agawu) - flag."),
    ("soli",     "Soli bell (Guinea)",            "9.7.7.7.77.7", None),
    ("tambu",    "Tambu bell",                    "9.7.7.77.7.7", None),
    ("bembe2",   "Bembe-2 bell",                  "99.7.77.7.7.", None),
    ("yoruba",   "Yoruba bell (konkolo)",         "9.7.77.7.77.", "Used in Cuban columbia (Toussaint)."),
    ("tonada",   "Tonada bell (Ashanti adowa)",   "9.77.77.7.7.", "Also Guinea Mandiani (Toussaint)."),
    ("asaadua",  "Asaadua bell (Akan)",           "9.7.7.77.77.", None),
    ("sorsonet", "Sorsonet bell (Guinea)",        "997.7.7.7.7.", None),
    ("bemba",    "Bemba clap (N. Zimbabwe)",      "9.77.7.7.77.", "Hand-clap/axe-blade timeline."),
    ("ashanti",  "Ashanti bell",                  "9.77.7.77.7.", None),
]
for key, nm, v, nt in TERN:
    T(f"tl.bell.{key}", nm, ["ternary", "12-8", "bell"],
      {"bell": {"v": v}}, beats=4, spb=3, meter="12/8", source=TOUS, notes=nt)

# Aksak / odd meters (spb=1, one char per pulse; accents mark group starts)
AKSAK = [
    ("pajdushko",  "Pajdushko (Bulgaria)",  5,  "2+3",         "9.8..",           "A.A..", (180, 260)),
    ("racenitsa",  "Racenitsa (Bulgaria)",  7,  "2+2+3",       "8.8.9..",         "A.A.A..", (160, 240)),
    ("dajchovo",   "Dajchovo (Bulgaria)",   9,  "2+2+2+3",     "9.8.8.8..",       "A.A.A.A..", (160, 240)),
    ("kopanica",   "Kopanica (Bulgaria)",   11, "2+2+3+2+2",   "9.8.9..8.8.",     "A.A.A..A.A.", (160, 260)),
    ("krivo",      "Krivo Sadovsko",        13, "2+2+2+3+2+2", "9.8.8.9..8.8.",   "A.A.A.A..A.A.", (160, 260)),
    ("bucimis",    "Bucimis (Bulgaria)",    15, "2+2+2+2+3+2+2", "9.8.8.8.9..8.8.", "A.A.A.A.A..A.A.", (160, 280)),
    ("kalamatianos", "Kalamatianos / Makedonsko", 7, "3+2+2",   "9..8.8.",         "A..A.A.", (100, 200)),
    ("curcuna",    "Curcuna (Turkey)",      10, "3+2+2+3",     "9..8.8.9..",      "A..A.A.A..", (100, 150)),
]
for key, nm, n, grp, v, a, bpm in AKSAK:
    T(f"tl.aksak.{key}", nm, ["aksak", "balkan", grp],
      {"kick": {"v": v, "a": a}}, beats=n, spb=1, meter=f"{n}/16", bpm=bpm,
      source="Folkdance Footnotes / Chromatone Balkan systems",
      notes=f"Grouping {grp}; strokes on quick/slow group starts (tapan dum). Pulse up to ~520/min in fast dances.")

# Hindustani tala thekas (kick = baya/bass presence, hi = treble bols;
# khali vibhag drops the bass). spb=1, one matra per step.
T("tl.tala.tintal", "Tintal theka (16)", ["india", "tala"],
  {"kick": {"v": "77777777....7777", "a": "A...A.......A..."},
   "hi":   {"v": "9777777777777777"}},
  beats=16, spb=1, meter="16 matras",
  bpm=(60, 200), source="ragajunglism.org tala index / RagaNet",
  notes="Vibhags 4+4+4+4; tali at 0,4,12, khali at 8 (bass drops steps 8-11). Sam = step 0.")
T("tl.tala.keherwa", "Keherwa theka (8)", ["india", "tala"],
  {"kick": {"v": "97....8.", "a": "A......."},
   "hi":   {"v": "97777677"}},
  beats=8, spb=1, meter="8 matras", bpm=(80, 160),
  source="ragajunglism.org tala index",
  notes="Dha Ge Na Ti | Na Ka Dhi Na - the film/folk workhorse.")
T("tl.tala.dadra", "Dadra theka (6)", ["india", "tala"],
  {"kick": {"v": "98.7..", "a": "A....."},
   "hi":   {"v": "977767"}},
  beats=6, spb=1, meter="6 matras", bpm=(80, 160),
  source="ragajunglism.org tala index", notes="Dha Dhin Na | Dha Tun Na (3+3).")
T("tl.tala.rupak", "Rupak theka (7)", ["india", "tala"],
  {"kick": {"v": "...7.7.", "a": "...A.A."},
   "hi":   {"v": "9777777"}},
  beats=7, spb=1, meter="7 matras", bpm=(60, 140),
  source="ragajunglism.org tala index",
  notes="3+2+2; unique: cycle BEGINS with khali (no bass on beat 1).")
T("tl.tala.jhaptal", "Jhaptal theka (10)", ["india", "tala"],
  {"kick": {"v": "9.77...77.", "a": "A.A....A.."},
   "hi":   {"v": "9777777777"}},
  beats=10, spb=1, meter="10 matras", bpm=(60, 140),
  source="ragajunglism.org tala index", notes="2+3+2+3; khali at step 5.")

# Gamelan colotomy (one stroke per beat; END-weighted - gong on last step)
T("tl.gamelan.lancaran", "Lancaran colotomy (Java)", ["indonesia", "gamelan"],
  {"hi":   {"v": "7.7.7.7.7.7.7.7."},
   "bell": {"v": "...8...8...8...8"},
   "lo":   {"v": ".....7...7...7.."},
   "kick": {"v": "...............9", "a": "...............A"}},
  beats=16, spb=1, meter="lancaran", bpm=(60, 140),
  source="Colotomy (Wikipedia et al.)",
  notes="hi=ketuk, bell=kenong, lo=kempul (wela/rest at step 1), kick=gong ageng WITH kenong on final step. End-weighted: strong beat is the LAST step, not step 0.")
T("tl.gamelan.ketawang", "Ketawang colotomy (Java)", ["indonesia", "gamelan"],
  {"hi":   {"v": "7.7.7.7.7.7.7.7."},
   "cp":   {"v": ".6...6...6...6.."},
   "bell": {"v": ".......8.......8"},
   "lo":   {"v": "...........7...."},
   "kick": {"v": "...............9", "a": "...............A"}},
  beats=16, spb=1, meter="ketawang", bpm=(40, 100),
  source="Colotomy (Wikipedia et al.)",
  notes="hi=kempyang, cp=ketuk, bell=kenong (7,15), lo=kempul (11; wela at 3), kick=gong (15).")

# Other timelines
T("tl.usa.bodiddley", "Bo Diddley beat", ["usa", "clave"],
  {"lo": {"v": "9..7..7...7.7...", "a": "A..........."+"A..."}},
  bpm=(90, 115), source="Drum Magazine '49 Beats'",
  notes="3-2 son clave orchestrated on floor toms.")
T("tl.flamenco.compas12", "Flamenco 12-count compas", ["spain", "flamenco"],
  {"cp":   {"v": "977977979797", "a": "A..A..A.A.A."},
   "kick": {"v": "8..8..8.8.8."}},
  beats=12, spb=1, meter="12-count", bpm=(120, 240),
  notes="Accents 12,3,6,8,10 encoded from count 12 as step 0 (bulerias convention). Count-start varies by school - variants-disagree.")
T("tl.mena.malfuf", "Malfuf (Egypt)", ["egypt", "malfuf"],
  {"kick": {"v": "9.......", "a": "A......."},
   "hi":   {"v": "...7..7."}},
  beats=2, meter="2/4", bpm=(110, 140),
  source="khafif.com Middle Eastern Rhythms FAQ", notes="D..T..T. - tresillo-shaped.")
T("tl.mena.ayyub", "Ayyub / Zar (Egypt)", ["egypt", "ayyub"],
  {"kick": {"v": "9..7..7.", "a": "A......."},
   "hi":   {"v": "....7..."}},
  beats=2, meter="2/4", bpm=(100, 150),
  source="khafif.com Middle Eastern Rhythms FAQ",
  notes="D..D T.D. rendering; sources disagree on tek placement - variants-disagree.")
T("tl.mena.masmoudi", "Masmoudi kabir (Egypt)", ["egypt", "masmoudi"],
  {"kick": {"v": "99......7.......", "a": "AA.............."},
   "hi":   {"v": "....7......77.7."}},
  beats=8, spb=2, meter="8/4", bpm=(60, 90),
  source="khafif.com Middle Eastern Rhythms FAQ",
  notes="Slow 8/4: D D..T...D..T T.T.; heavily ornamented in practice.")
T("tl.cuba.cascara", "Cascara (3-2)", ["cuba", "timbale"],
  {"bell": {"v": "9.77.77.7.77.7.7"}},
  bpm=(85, 110), notes="Timbale-shell pattern against 3-2 clave; stylized - schools vary on two strokes.")
T("tl.cuba.baqueteo", "Danzon baqueteo", ["cuba", "danzon"],
  {"cp": {"v": "9.77.77.9.77.77."}},
  bpm=(80, 105), source="Fiol, Hidden Rhythm (cinquillo derivation)",
  notes="Cinquillo doubled across the cycle; also the Haitian kompa ti-bwa line.")
T("tl.eire.slipjig", "Slip-jig timeline (Ireland)", ["ireland", "9-8"],
  {"cp": {"v": "9.87.87..", "a": "A..A..A.."}},
  beats=9, spb=1, meter="9/8", bpm=(100, 140),
  source="Gotham (MTO 19.2) jig timeline [2-1-2-1-3]",
  notes="9-pulse jig-family cell, IOI 2-1-2-1-3.")

# ═════════════════════ V2: GLOBAL GROOVE SETS ═════════════════════
def arr2():
    return [{"role": "main", "bars": 8, "fillEvery": 4},
            {"role": "main", "bars": 8}]

def arr3(prefix):
    return [{"role": "main", "bars": 8, "fillEvery": 4},
            {"role": "lift", "bars": 8, "fillEvery": 4},
            {"role": "main", "bars": 4}]

# ---- Latin America ----
P("latin.cumbia.main", "Cumbia - main", "latin", ["colombia", "cumbia"], {
    "kick": {"v": "9.......8......."},
    "cp":   {"v": "....8.......8..."},
    "lo":   {"v": "......7.......7."},
    "hi":   {"v": "7.377.377.377.37"},
}, swing=0.25, bpm=(85, 105),
  notes="cp = llamador backbeat; lo = tambora; hi = guacharaca gallop (x.xx per beat), swung.")
P("latin.cumbia.lift", "Cumbia - lift", "latin", ["colombia", "cumbia"], {
    "kick": {"v": "9.......8......."},
    "snare": {"v": "....7.......7..."},
    "cp":   {"v": "....8.......8..."},
    "lo":   {"v": "..5...7...5...7."},
    "hi":   {"v": "7.377.377.377.37"},
    "bell": {"v": "9.......8......."},
}, swing=0.25, bpm=(85, 105))
P("latin.cumbia.fill", "Cumbia - fill", "latin", ["colombia", "fill"], {
    "kick": {"v": "9..............."},
    "lo":   {"v": "........55667789"},
    "hi":   {"v": "7.377.377.377.37"},
}, swing=0.25, bpm=(85, 105))
S("latin.cumbia", "Cumbia", "latin", 95,
  {"main": "latin.cumbia.main", "lift": "latin.cumbia.lift", "fill": ["latin.cumbia.fill"]},
  arr3("latin.cumbia"))

P("latin.merengue.main", "Merengue - main", "latin", ["dominican", "merengue"], {
    "kick": {"v": "9.......9......."},
    "lo":   {"v": "9...6.6.9...6.6."},
    "hi":   {"v": "6464646464646464"},
    "snare": {"v": "..............67"},
}, bpm=(120, 160),
  notes="lo = tambora (low + rim taps), snare = roll pickup into 1, hi = guira 16ths. Tempo verified 120-160.")
P("latin.merengue.fill", "Merengue - fill (jaleo)", "latin", ["dominican", "fill"], {
    "kick": {"v": "9...9..99......."},
    "snare": {"v": "........55667889"},
    "hi":   {"v": "6464646464646464"},
}, bpm=(120, 160))
S("latin.merengue", "Merengue", "latin", 140,
  {"main": "latin.merengue.main", "fill": ["latin.merengue.fill"]}, arr2())

P("latin.bachata.main", "Bachata - main", "latin", ["dominican", "bachata"], {
    "kick": {"v": "8.......8......."},
    "hi":   {"v": "7.5.7.5.7.5.9.5.", "a": "............A..."},
    "chh":  {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(108, 135),
  notes="hi = bongo martillo 8ths with the verified beat-4 accent (step 12); chh = guira.")
P("latin.bachata.fill", "Bachata - fill", "latin", ["dominican", "fill"], {
    "kick": {"v": "8..............."},
    "hi":   {"v": "7.5.7.5.66778899", "a": "...............A"},
    "chh":  {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(108, 135))
S("latin.bachata", "Bachata", "latin", 120,
  {"main": "latin.bachata.main", "fill": ["latin.bachata.fill"]}, arr2())

P("latin.candombe.main", "Candombe - main", "latin", ["uruguay", "candombe"], {
    "cp":   {"v": "9..7..7...7.7...", "a": "A..............."},
    "hi":   {"v": ".655.655.655.655"},
    "kick": {"v": "9.....7.8..7...."},
    "lo":   {"v": "...7.......7..7."},
}, bpm=(105, 140),
  notes="cp = madera (clave on drum shell), hi = chico cell (rest ON the beat, strokes on 2nd-4th 16ths - verified), kick = piano (stylized), lo = repique figures. Microtiming: pushed/contracted, near-ternary lean (Jure & Rocamora).")
P("latin.candombe.lift", "Candombe - lift", "latin", ["uruguay", "candombe"], {
    "cp":   {"v": "9..7..7...7.7...", "a": "A..............."},
    "hi":   {"v": ".655.655.655.655"},
    "kick": {"v": "9.....7.8..7..7."},
    "lo":   {"v": "...7..7....7.77."},
}, bpm=(105, 140))
P("latin.candombe.fill", "Candombe - fill (repique call)", "latin", ["uruguay", "fill"], {
    "cp":   {"v": "9..7..7...7.7...", "a": "A..............."},
    "kick": {"v": "9..............."},
    "lo":   {"v": "7..77..77..7889."},
}, bpm=(105, 140), notes="The madera never stops.")
S("latin.candombe", "Candombe", "latin", 130,
  {"main": "latin.candombe.main", "lift": "latin.candombe.lift", "fill": ["latin.candombe.fill"]},
  arr3("latin.candombe"))

P("latin.festejo.main", "Festejo - main", "latin", ["peru", "festejo", "12-8"], {
    "kick": {"v": "9..5..7....."},
    "hi":   {"v": "..7.7...7.7."},
    "bell": {"v": "7..7..7..7.."},
    "cp":   {"v": "........7..."},
}, beats=4, spb=3, meter="12/8", bpm=(100, 125),
  notes="Cajon-led Afro-Peruvian 12/8 (verified meter); placements stylized. cp = quijada rattle.")
P("latin.festejo.fill", "Festejo - fill", "latin", ["peru", "fill", "12-8"], {
    "kick": {"v": "9..........."},
    "lo":   {"v": "556677889999"},
}, beats=4, spb=3, meter="12/8", bpm=(100, 125))
S("latin.festejo", "Festejo", "latin", 112,
  {"main": "latin.festejo.main", "fill": ["latin.festejo.fill"]}, arr2())

P("latin.baiao.main", "Baiao - main", "latin", ["brazil", "baiao"], {
    "kick": {"v": "9..7....9..7...."},
    "chh":  {"v": "5656565656565656"},
    "cp":   {"v": "....7.......7..."},
}, bpm=(90, 120),
  notes="kick = zabumba on the doubled tresillo (0,3,8,11); chh = triangle open/closed 16ths.")
P("latin.baiao.lift", "Baiao - lift", "latin", ["brazil", "baiao"], {
    "kick": {"v": "9..7....9..7...."},
    "snare": {"v": "....7..3.3..7..3"},
    "chh":  {"v": "6767676767676767"},
    "cp":   {"v": "....7.......7..."},
}, bpm=(90, 120))
P("latin.baiao.fill", "Baiao - fill", "latin", ["brazil", "fill"], {
    "kick": {"v": "9..7....9......."},
    "snare": {"v": "........55667789"},
    "chh":  {"v": "56565656........"},
}, bpm=(90, 120))
S("latin.baiao", "Baiao", "latin", 104,
  {"main": "latin.baiao.main", "lift": "latin.baiao.lift", "fill": ["latin.baiao.fill"]},
  arr3("latin.baiao"))

P("latin.chacarera.main", "Chacarera - main", "latin", ["argentina", "6-8", "hemiola"], {
    "kick": {"v": "9..7..8.7.7.", "a": "A.....A....."},
    "cp":   {"v": "666666666666", "a": "A..A..A.A.A."},
}, beats=4, spb=3, meter="6/8+3/4", bpm=(100, 125),
  notes="Bombo leguero sesquialtera: bar 1 in 6/8 (0,3), bar 2 in 3/4 (6,8,10). cp = rim clicks.")
P("latin.chacarera.fill", "Chacarera - fill", "latin", ["argentina", "fill"], {
    "kick": {"v": "9..7..9..7.."},
    "lo":   {"v": "......556789"},
}, beats=4, spb=3, meter="6/8+3/4", bpm=(100, 125))
S("latin.chacarera", "Chacarera", "latin", 112,
  {"main": "latin.chacarera.main", "fill": ["latin.chacarera.fill"]}, arr2())

P("latin.bomba.main", "Bomba sica - main", "latin", ["puertorico", "bomba"], {
    "cp":   {"v": "9.7.7..79.7.7..7"},
    "kick": {"v": "9.....7.9.....7."},
    "hi":   {"v": "..5...5....5..5."},
    "chh":  {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(90, 110),
  notes="cp = cua sticks timeline, kick = buleador, hi = primo answers (stylized), chh = maraca.")
P("latin.bomba.fill", "Bomba sica - fill", "latin", ["puertorico", "fill"], {
    "cp":   {"v": "9.7.7..79.7.7..7"},
    "kick": {"v": "9.....7.9......."},
    "lo":   {"v": "....77.77..7889."},
}, bpm=(90, 110))
S("latin.bomba", "Bomba sica", "latin", 100,
  {"main": "latin.bomba.main", "fill": ["latin.bomba.fill"]}, arr2())

# ---- Caribbean ----
P("carib.soca.main", "Soca - main", "caribbean", ["trinidad", "soca"], {
    "kick": {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "....8.......8.8."},
    "hi":   {"v": "8686868686868686"},
    "bell": {"v": "..7...7...7...7."},
}, bpm=(150, 165),
  notes="Power soca: four-floor kick, engine-room iron 16ths on hi, cowbell offbeats.")
P("carib.soca.lift", "Soca - lift", "caribbean", ["trinidad", "soca"], {
    "kick": {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "....8......58.8."},
    "hi":   {"v": "8686868686868686"},
    "ohh":  {"v": "..7...7...7...7."},
    "bell": {"v": "..7...7...7...7."},
}, bpm=(150, 165))
P("carib.soca.fill", "Soca - fill", "caribbean", ["trinidad", "fill"], {
    "kick": {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "snare": {"v": "55667788999.9.9."},
    "hi":   {"v": "8686868686868686"},
}, bpm=(150, 165))
S("carib.soca", "Soca", "caribbean", 158,
  {"main": "carib.soca.main", "lift": "carib.soca.lift", "fill": ["carib.soca.fill"]},
  arr3("carib.soca"))

P("carib.ska.main", "Ska - main", "caribbean", ["jamaica", "ska"], {
    "kick": {"v": "9.......9......."},
    "snare": {"v": "....9.......9...", "a": "....A.......A..."},
    "chh":  {"v": "..8...8...8...8."},
}, bpm=(120, 140), notes="Everything leans on the offbeat 8ths (2,6,10,14).")
P("carib.ska.fill", "Ska - fill", "caribbean", ["jamaica", "fill"], {
    "kick": {"v": "9..............."},
    "snare": {"v": "....9...66778899", "a": "....A..........A"},
    "chh":  {"v": "..8...8........."},
}, bpm=(120, 140))
S("carib.ska", "Ska", "caribbean", 128,
  {"main": "carib.ska.main", "fill": ["carib.ska.fill"]}, arr2())

P("carib.dancehall.main", "Dancehall - main", "caribbean", ["jamaica", "dancehall"], {
    "kick": {"v": "9..7..7.8..7..7."},
    "cp":   {"v": "....8.......8..."},
    "chh":  {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(95, 105),
  notes="Double-tresillo kick (Bam Bam / dembow skeleton - verified lineage); cp = rim.")
P("carib.dancehall.lift", "Dancehall - lift", "caribbean", ["jamaica", "dancehall"], {
    "kick": {"v": "9..7..7.8..7..7."},
    "cp":   {"v": "....8.......8..."},
    "chh":  {"v": "6464646464646464"},
    "bell": {"v": "9.......8......."},
    "ohh":  {"v": "..............6."},
}, bpm=(95, 105))
P("carib.dancehall.fill", "Dancehall - fill", "caribbean", ["jamaica", "fill"], {
    "kick": {"v": "9..7..7........."},
    "snare": {"v": "......9....98.99"},
    "chh":  {"v": "6.6.6.6........."},
}, bpm=(95, 105))
S("carib.dancehall", "Dancehall", "caribbean", 100,
  {"main": "carib.dancehall.main", "lift": "carib.dancehall.lift", "fill": ["carib.dancehall.fill"]},
  arr3("carib.dancehall"))

P("carib.kompa.main", "Kompa - main", "caribbean", ["haiti", "kompa"], {
    "cp":   {"v": "9.77.77.9.77.77."},
    "bell": {"v": "6.6.6.6.6.6.6.6."},
    "kick": {"v": "8...8...8...8..."},
    "lo":   {"v": "..5..7....5..7.."},
}, bpm=(95, 115),
  notes="cp = ti-bwa playing the cinquillo timeline (verified cell); lo = tanbou (stylized).")
P("carib.kompa.fill", "Kompa - fill", "caribbean", ["haiti", "fill"], {
    "cp":   {"v": "9.77.77.9.77.77."},
    "kick": {"v": "8...8...8...8..."},
    "lo":   {"v": "....55667788999."},
}, bpm=(95, 115))
S("carib.kompa", "Kompa", "caribbean", 105,
  {"main": "carib.kompa.main", "fill": ["carib.kompa.fill"]}, arr2())

# ---- Africa ----
P("africa.gahu.main", "Gahu - main", "africa", ["ghana", "ewe", "gahu"], {
    "bell": {"v": "9..7..7...7...7."},
    "kick": {"v": "9...7...9...7.7."},
    "cp":   {"v": "..77..77..77..77"},
    "hi":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(110, 130),
  notes="Bell verified (Toussaint 0,3,6,10,14); kick/sticks from Pocket Operations kit reduction.")
P("africa.gahu.fill", "Gahu - fill", "africa", ["ghana", "fill"], {
    "bell": {"v": "9..7..7...7...7."},
    "kick": {"v": "9...7..........."},
    "lo":   {"v": "....77..77..889."},
}, bpm=(110, 130), notes="The bell never stops.")
S("africa.gahu", "Gahu", "africa", 120,
  {"main": "africa.gahu.main", "fill": ["africa.gahu.fill"]}, arr2())

P("africa.highlife.main", "Highlife - main", "africa", ["ghana", "highlife"], {
    "bell": {"v": "9..7..7...7.7..."},
    "kick": {"v": "9.......8......."},
    "cp":   {"v": "....7.......7..."},
    "hi":   {"v": "6565656565656565"},
}, bpm=(100, 125), notes="Son-shaped bell; guitar carries most syncopation.")
P("africa.highlife.lift", "Highlife - lift", "africa", ["ghana", "highlife"], {
    "bell": {"v": "9..7..7...7.7..."},
    "kick": {"v": "9.......8.7....."},
    "cp":   {"v": "....7.......7..."},
    "hi":   {"v": "6565656565656565"},
    "ohh":  {"v": "..6...6...6...6."},
}, bpm=(100, 125))
P("africa.highlife.fill", "Highlife - fill", "africa", ["ghana", "fill"], {
    "bell": {"v": "9..7..7...7.7..."},
    "kick": {"v": "9..............."},
    "snare": {"v": "........66778899"},
}, bpm=(100, 125))
S("africa.highlife", "Highlife", "africa", 112,
  {"main": "africa.highlife.main", "lift": "africa.highlife.lift", "fill": ["africa.highlife.fill"]},
  arr3("africa.highlife"))

P("africa.kuku.main", "Kuku - main", "africa", ["guinea", "djembe", "kuku"], {
    "kick": {"v": "9.........7....."},
    "lo":   {"v": "....7.......7..."},
    "bell": {"v": "8.8.8.8.8.8.8.8."},
    "hi":   {"v": "..7...7...7...7."},
    "cp":   {"v": "..77..7...77..7."},
}, bpm=(120, 140),
  notes="Stylized ensemble reduction (kick=dununba, lo=sangban, hi=kenkeni, cp=djembe accompaniment). Full parts: WAP Pages box-notation book.")
P("africa.kuku.fill", "Kuku - fill (echauffement)", "africa", ["guinea", "fill"], {
    "kick": {"v": "9..............."},
    "bell": {"v": "8.8.8.8.8.8.8.8."},
    "lo":   {"v": "5566778899999999"},
}, bpm=(120, 140))
S("africa.kuku", "Kuku", "africa", 130,
  {"main": "africa.kuku.main", "fill": ["africa.kuku.fill"]}, arr2())

P("africa.soukous.main", "Soukous - main", "africa", ["congo", "soukous"], {
    "bell": {"v": "9..7..7..77....."},
    "kick": {"v": "9...9...9...9..."},
    "snare": {"v": "....7.......7..7"},
    "hi":   {"v": "5654565456545654"},
}, bpm=(130, 160), notes="Bell verified (Toussaint 0,3,6,9,10).")
P("africa.soukous.lift", "Soukous - lift (seben)", "africa", ["congo", "soukous"], {
    "bell": {"v": "9..7..7..77....."},
    "kick": {"v": "9...9...9...9..."},
    "snare": {"v": "..7.7..77.7.7..7"},
    "hi":   {"v": "5654565456545654"},
    "ohh":  {"v": "..6...6...6...6."},
}, bpm=(130, 160))
P("africa.soukous.fill", "Soukous - fill", "africa", ["congo", "fill"], {
    "bell": {"v": "9..7..7..77....."},
    "kick": {"v": "9...9...9......."},
    "snare": {"v": "....7...66778899"},
}, bpm=(130, 160))
S("africa.soukous", "Soukous", "africa", 145,
  {"main": "africa.soukous.main", "lift": "africa.soukous.lift", "fill": ["africa.soukous.fill"]},
  arr3("africa.soukous"))

P("africa.gnawa.main", "Gnawa - main", "africa", ["morocco", "gnawa", "12-8"], {
    "bell": {"v": "9.77.77.77.7"},
    "kick": {"v": "9.....7....."},
    "lo":   {"v": "...5....5..."},
}, beats=4, spb=3, meter="12/8", bpm=(90, 140),
  notes="bell = qraqeb gallop; ceremonies accelerate continuously - map tempo CV.")
P("africa.gnawa.fill", "Gnawa - fill", "africa", ["morocco", "fill", "12-8"], {
    "bell": {"v": "9.77.77.77.7"},
    "kick": {"v": "9..7..7..7.."},
}, beats=4, spb=3, meter="12/8", bpm=(90, 140))
S("africa.gnawa", "Gnawa", "africa", 110,
  {"main": "africa.gnawa.main", "fill": ["africa.gnawa.fill"]}, arr2())

P("africa.amapiano.main", "Amapiano - main", "africa", ["southafrica", "amapiano"], {
    "kick": {"v": "9.......8......."},
    "lo":   {"v": "...8..8...8..8.."},
    "hi":   {"v": "5555555555555555"},
    "cp":   {"v": "....7.....7....."},
    "chh":  {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(110, 115),
  notes="lo = log drum on tresillo-leaning hits (3,6,10,13); half-time feel over house tempo. Stylized.")
P("africa.amapiano.lift", "Amapiano - lift", "africa", ["southafrica", "amapiano"], {
    "kick": {"v": "9.......8......."},
    "lo":   {"v": "...8..8.8.8..8.8"},
    "hi":   {"v": "5656565656565656"},
    "cp":   {"v": "....7.....7....."},
    "chh":  {"v": "6.6.6.6.6.6.6.6."},
    "ohh":  {"v": "..............6."},
}, bpm=(110, 115))
P("africa.amapiano.fill", "Amapiano - fill", "africa", ["southafrica", "fill"], {
    "kick": {"v": "9..............."},
    "lo":   {"v": "...8..8.55667788"},
    "hi":   {"v": "5555555555555555"},
}, bpm=(110, 115))
S("africa.amapiano", "Amapiano", "africa", 112,
  {"main": "africa.amapiano.main", "lift": "africa.amapiano.lift", "fill": ["africa.amapiano.fill"]},
  arr3("africa.amapiano"))

P("africa.gqom.main", "Gqom - main", "africa", ["southafrica", "gqom"], {
    "kick": {"v": "9..8..8....8...."},
    "lo":   {"v": "........7....7.."},
    "cp":   {"v": "....8........8.."},
    "chh":  {"v": "..6...6...6...6."},
}, bpm=(120, 128),
  notes="Broken 3+3+2 kick, no four-floor; dark tom stabs. Stylized.")
P("africa.gqom.fill", "Gqom - fill", "africa", ["southafrica", "fill"], {
    "kick": {"v": "9..8..8........."},
    "lo":   {"v": "....55667788999."},
}, bpm=(120, 128))
S("africa.gqom", "Gqom", "africa", 124,
  {"main": "africa.gqom.main", "fill": ["africa.gqom.fill"]}, arr2())

# ---- Middle East / North Africa ----
P("mena.saidi.main", "Saidi - main", "mena", ["egypt", "saidi"], {
    "kick": {"v": "9.....7.8.......", "a": "A..............."},
    "cp":   {"v": "..8.........8..."},
    "hi":   {"v": ".....3.3..3...3."},
}, bpm=(90, 115),
  notes="D T . D D . T . (verified skeleton, khafif.com): dums 0,6,8; teks 2,12; hi = ka ornaments.")
P("mena.saidi.lift", "Saidi - lift", "mena", ["egypt", "saidi"], {
    "kick": {"v": "9.....7.8.......", "a": "A..............."},
    "cp":   {"v": "..8.........8..."},
    "chh":  {"v": "2323232323232323"},
    "bell": {"v": "6.......6......."},
    "hi":   {"v": ".....3.3..3...3."},
}, bpm=(90, 115), notes="chh = riqq 16ths, bell = finger cymbals.")
P("mena.saidi.fill", "Saidi - fill", "mena", ["egypt", "fill"], {
    "kick": {"v": "9..............."},
    "hi":   {"v": "..3344556677889.", "a": "..............A."},
    "cp":   {"v": "..8............."},
}, bpm=(90, 115))
S("mena.saidi", "Saidi", "mena", 100,
  {"main": "mena.saidi.main", "lift": "mena.saidi.lift", "fill": ["mena.saidi.fill"]},
  arr3("mena.saidi"))

P("mena.karsilama.main", "Karsilama - main", "mena", ["turkey", "9-8", "aksak"], {
    "kick": {"v": "9...8....", "a": "A...A...."},
    "cp":   {"v": "..7...7.7"},
}, beats=9, spb=1, meter="9/8", bpm=(100, 130),
  notes="2+2+2+3: dums 0,4; teks 2,6,8. Roman havasi adds offbeat teks - variants-disagree.")
P("mena.karsilama.fill", "Karsilama - fill", "mena", ["turkey", "fill"], {
    "kick": {"v": "9...8...."},
    "cp":   {"v": "..7.7.777"},
}, beats=9, spb=1, meter="9/8", bpm=(100, 130))
S("mena.karsilama", "Karsilama", "mena", 115,
  {"main": "mena.karsilama.main", "fill": ["mena.karsilama.fill"]}, arr2())

P("persia.shesh.main", "Shesh-o-hasht - main", "mena", ["iran", "6-8"], {
    "kick": {"v": "9..7..9..7.."},
    "hi":   {"v": "..6.66..6.66"},
}, beats=4, spb=3, meter="6/8", bpm=(110, 130),
  notes="Persian 6/8 pop/classical groove; kick = tombak bass strokes, hi = tek chatter. Stylized.")
P("persia.shesh.fill", "Shesh-o-hasht - fill", "mena", ["iran", "fill"], {
    "kick": {"v": "9..7........"},
    "hi":   {"v": "..6.66667788"},
}, beats=4, spb=3, meter="6/8", bpm=(110, 130))
S("persia.shesh", "Shesh-o-hasht (6/8)", "mena", 120,
  {"main": "persia.shesh.main", "fill": ["persia.shesh.fill"]}, arr2())

# ---- Europe ----
P("europe.racenitsa.main", "Racenitsa - main", "europe", ["bulgaria", "7-16", "aksak"], {
    "kick": {"v": "7.7.9..", "a": "A.A.A.."},
    "hi":   {"v": ".4.4.44"},
}, beats=7, spb=1, meter="7/16", bpm=(160, 240),
  notes="2+2+3 quick-quick-slow (verified grouping); tapan dums on group starts, accent on the slow.")
P("europe.racenitsa.fill", "Racenitsa - fill", "europe", ["bulgaria", "fill"], {
    "kick": {"v": "9......"},
    "hi":   {"v": "4455667"},
}, beats=7, spb=1, meter="7/16", bpm=(160, 240))
S("europe.racenitsa", "Racenitsa", "europe", 200,
  {"main": "europe.racenitsa.main", "fill": ["europe.racenitsa.fill"]}, arr2())

P("eire.jig.main", "Jig - main", "europe", ["ireland", "jig", "6-8"], {
    "kick": {"v": "9..7..9..7..", "a": "A.....A....."},
    "hi":   {"v": "755755755755"},
}, beats=4, spb=3, meter="6/8", bpm=(110, 130),
  notes="Bodhran: low on dotted beats, tip 8ths in DUD lilt.")
P("eire.jig.lift", "Jig - lift", "europe", ["ireland", "jig", "6-8"], {
    "kick": {"v": "9..7..9.7.7.", "a": "A.....A....."},
    "hi":   {"v": "756756756756"},
    "cp":   {"v": "9.....7....."},
}, beats=4, spb=3, meter="6/8", bpm=(110, 130),
  notes="Hemiola tease in the second half.")
P("eire.jig.fill", "Jig - fill", "europe", ["ireland", "fill"], {
    "kick": {"v": "9..........."},
    "hi":   {"v": "455667788999"},
}, beats=4, spb=3, meter="6/8", bpm=(110, 130))
S("eire.jig", "Irish jig", "europe", 120,
  {"main": "eire.jig.main", "lift": "eire.jig.lift", "fill": ["eire.jig.fill"]},
  arr3("eire.jig"))

P("eire.reel.main", "Reel - main", "europe", ["ireland", "reel"], {
    "lo":   {"v": "9.6.7.6.8.6.7.6."},
}, swing=0.2, bpm=(100, 120),
  notes="Bodhran continuous 8ths, accents on 0 and 8; light lilt.")
P("eire.reel.fill", "Reel - fill", "europe", ["ireland", "fill"], {
    "lo":   {"v": "9.6.7.6.55667889"},
}, swing=0.2, bpm=(100, 120))
S("eire.reel", "Irish reel", "europe", 110,
  {"main": "eire.reel.main", "fill": ["eire.reel.fill"]}, arr2())

P("flamenco.bulerias.main", "Bulerias - main", "europe", ["spain", "flamenco", "12-count"], {
    "cp":   {"v": "977977979797", "a": "A..A..A.A.A."},
    "kick": {"v": "8..8..8.8.8."},
}, beats=12, spb=1, meter="12-count", bpm=(180, 240),
  notes="Palmas + cajon on the 12-count compas (accents 12,3,6,8,10 from count 12). Count-start varies by school.")
S("flamenco.bulerias", "Bulerias", "europe", 210,
  {"main": "flamenco.bulerias.main"},
  [{"role": "main", "bars": 16}])

# ---- Asia ----
P("asia.keherwa.main", "Keherwa groove - main", "asia", ["india", "keherwa"], {
    "kick": {"v": "97....8.", "a": "A......."},
    "hi":   {"v": "76767676"},
}, beats=4, spb=2, meter="8 matras", bpm=(95, 115),
  notes="Dha Ge Na Ti Na Ka Dhi Na as a groove (verified theka); kick = baya/dhol bass.")
P("asia.keherwa.lift", "Keherwa groove - lift", "asia", ["india", "keherwa"], {
    "kick": {"v": "97....8.", "a": "A......."},
    "hi":   {"v": "76767676"},
    "chh":  {"v": "56565656"},
}, beats=4, spb=2, meter="8 matras", bpm=(95, 115))
P("asia.keherwa.fill", "Keherwa groove - fill", "asia", ["india", "fill"], {
    "kick": {"v": "9......."},
    "hi":   {"v": "66778899"},
}, beats=4, spb=2, meter="8 matras", bpm=(95, 115))
S("asia.keherwa", "Keherwa", "asia", 105,
  {"main": "asia.keherwa.main", "lift": "asia.keherwa.lift", "fill": ["asia.keherwa.fill"]},
  arr3("asia.keherwa"))

P("asia.bhangra.main", "Bhangra chaal - main", "asia", ["punjab", "bhangra"], {
    "kick": {"v": "9.....7.8.....7."},
    "hi":   {"v": "....8.......8..."},
    "chh":  {"v": "6.6.6.6.6.6.6.6."},
}, swing=0.3, bpm=(95, 110),
  notes="Dhol chaal: bass on 0,6,8,14 (bossa-kick shape), dagga tak on 2 and 4. Stylized.")
P("asia.bhangra.fill", "Bhangra chaal - fill", "asia", ["punjab", "fill"], {
    "kick": {"v": "9.....7........."},
    "hi":   {"v": "....8...55667889"},
    "chh":  {"v": "6.6.6.6........."},
}, swing=0.3, bpm=(95, 110))
S("asia.bhangra", "Bhangra chaal", "asia", 102,
  {"main": "asia.bhangra.main", "fill": ["asia.bhangra.fill"]}, arr2())

P("asia.taiko.main", "Taiko matsuri - main", "asia", ["japan", "taiko"], {
    "kick": {"v": "9.......8.......", "a": "A..............."},
    "lo":   {"v": "......55......55"},
    "cp":   {"v": "..7.......7....."},
    "hi":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(90, 130),
  notes="'Don doko' cells: kick = odaiko don, lo = doko doubles, cp = ka rim, hi = shime ji. Stylized.")
P("asia.taiko.lift", "Taiko matsuri - lift", "asia", ["japan", "taiko"], {
    "kick": {"v": "9...8...8...8...", "a": "A..............."},
    "lo":   {"v": "..55..55..55..55"},
    "cp":   {"v": "..7.......7....."},
    "hi":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(90, 130))
P("asia.taiko.fill", "Taiko matsuri - fill", "asia", ["japan", "fill"], {
    "kick": {"v": "9..............."},
    "lo":   {"v": "5566778899999.9.", "a": "..............A."},
}, bpm=(90, 130))
S("asia.taiko", "Taiko matsuri", "asia", 110,
  {"main": "asia.taiko.main", "lift": "asia.taiko.lift", "fill": ["asia.taiko.fill"]},
  arr3("asia.taiko"))

P("asia.gutgeori.main", "Gutgeori jangdan - main", "asia", ["korea", "12-8"], {
    "kick": {"v": "9..7.....7..", "a": "A..........."},
    "cp":   {"v": "....7..7..7."},
}, beats=4, spb=3, meter="12/8", bpm=(60, 90),
  notes="Janggu cycle (verified 12/8 compound meter; stroke placements stylized): kick = kung/deong low, cp = deok rim.")
P("asia.gutgeori.fill", "Gutgeori jangdan - fill", "asia", ["korea", "fill"], {
    "kick": {"v": "9..........."},
    "cp":   {"v": "....77778899"},
}, beats=4, spb=3, meter="12/8", bpm=(60, 90))
S("asia.gutgeori", "Gutgeori jangdan", "asia", 72,
  {"main": "asia.gutgeori.main", "fill": ["asia.gutgeori.fill"]}, arr2())

# ---- Oceania ----
P("oceania.tahiti.main", "'Ote'a - main", "oceania", ["tahiti", "toere"], {
    "hi":   {"v": "9669669696969696", "a": "A..A..A.A......."},
    "kick": {"v": "9.......8......."},
    "lo":   {"v": "..7...7...7...7."},
}, bpm=(120, 160),
  notes="To'ere log drums at 16th density with 3+3+2 accents over pahu bass. Stylized.")
P("oceania.tahiti.lift", "'Ote'a - lift", "oceania", ["tahiti", "toere"], {
    "hi":   {"v": "9669669696969696", "a": "A..A..A.A......."},
    "kick": {"v": "9.......8...8..."},
    "lo":   {"v": "..7.7.7...7.7.7."},
}, bpm=(120, 160))
P("oceania.tahiti.fill", "'Ote'a - fill", "oceania", ["tahiti", "fill"], {
    "hi":   {"v": "999999999999999.", "a": "A..............."},
    "kick": {"v": "9..............."},
}, bpm=(120, 160))
S("oceania.tahiti", "'Ote'a (Tahiti)", "oceania", 140,
  {"main": "oceania.tahiti.main", "lift": "oceania.tahiti.lift", "fill": ["oceania.tahiti.fill"]},
  arr3("oceania.tahiti"))

# ---- USA (beyond v1) ----
P("usa.secondline.main", "Second line - main", "usa", ["neworleans", "secondline"], {
    "kick": {"v": "9..7..7...7.7..."},
    "snare": {"v": "..3.8..3..3.8..3", "a": "....A.......A..."},
    "chh":  {"v": "7.7.7.7.7.7.7.7."},
}, swing=0.35, bpm=(90, 110),
  notes="Kick walks the 3-2 son clave (verified basis); swung, laid-back. Snare = press-roll figures.")
P("usa.secondline.lift", "Second line - lift", "usa", ["neworleans", "secondline"], {
    "kick": {"v": "9..7..7...7.7..."},
    "snare": {"v": "3.3.8..33.3.8..3", "a": "....A.......A..."},
    "chh":  {"v": "7.7.7.7.7.7.7.7."},
    "bell": {"v": "9..7..7...7.7..."},
}, swing=0.35, bpm=(90, 110))
P("usa.secondline.fill", "Second line - fill", "usa", ["neworleans", "fill"], {
    "kick": {"v": "9..7............"},
    "snare": {"v": "....8...55667889", "a": "....A..........A"},
}, swing=0.35, bpm=(90, 110))
S("usa.secondline", "New Orleans second line", "usa", 100,
  {"main": "usa.secondline.main", "lift": "usa.secondline.lift", "fill": ["usa.secondline.fill"]},
  arr3("usa.secondline"))

# ═══════════════════════════ EUCLIDEAN ═══════════════════════════
# Canonical Euclidean necklaces placed on the 16-step grid.
#                                  1e&a2e&a3e&a4e&a
P("eucl.tresillo.sparse", "Tresillo E(3,8) — sparse", "euclidean", ["euclidean", "tresillo"], {
    "kick": {"v": "9.....8.....8..."},
    "chh":  {"v": "6...6...6...6..."},
}, bpm=(90, 130))
P("eucl.tresillo.main", "Tresillo E(3,8) — main", "euclidean", ["euclidean", "tresillo"], {
    "kick":  {"v": "9.....8.....8..."},
    "snare": {"v": "....8.......8...", "a": "....A.......A..."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(90, 130), notes="E(3,8) kick under a straight backbeat.")
P("eucl.tresillo.lift", "Tresillo E(3,8) — lift", "euclidean", ["euclidean", "tresillo"], {
    "kick":  {"v": "9.....8.....8..."},
    "snare": {"v": "....8.......8...", "a": "....A.......A..."},
    "chh":   {"v": "6464646464646464"},
    "bell":  {"v": "7..7.7.7..7.7.7."},
    "ohh":   {"v": "..............6."},
}, bpm=(90, 130), notes="E(7,16) bell against the E(3,8) kick.")
P("eucl.tresillo.fill", "Tresillo E(3,8) — fill", "euclidean", ["euclidean", "fill"], {
    "kick":  {"v": "9.....8.....8..."},
    "snare": {"v": "..........668899", "a": "...............A"},
    "chh":   {"v": "6.6.6.6.6.6....."},
    "ohh":   {"v": "............7..."},
}, bpm=(90, 130))
S("eucl.tresillo", "Tresillo E(3,8)", "euclidean", 110, std_roles("eucl.tresillo"), STD_ARR)

P("eucl.cinquillo.sparse", "Cinquillo E(5,8) — sparse", "euclidean", ["euclidean", "cinquillo"], {
    "kick": {"v": "9.......8......."},
    "cp":   {"v": "5...5.5...5.5..."},
}, bpm=(85, 120))
P("eucl.cinquillo.main", "Cinquillo E(5,8) — main", "euclidean", ["euclidean", "cinquillo"], {
    "kick":  {"v": "9...7.7...7.7..."},
    "snare": {"v": "....8.......8...", "a": "....A.......A..."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(85, 120), notes="E(5,8) kick — the cinquillo cell.")
P("eucl.cinquillo.lift", "Cinquillo E(5,8) — lift", "euclidean", ["euclidean", "cinquillo"], {
    "kick":  {"v": "9...7.7...7.7..."},
    "snare": {"v": "....8.......8...", "a": "....A.......A..."},
    "chh":   {"v": "6565656565656565"},
    "hi":    {"v": "..5..5..5..5..5."},
    "ohh":   {"v": "..............6."},
}, bpm=(85, 120))
P("eucl.cinquillo.fill", "Cinquillo E(5,8) — fill", "euclidean", ["euclidean", "fill"], {
    "kick":  {"v": "9...7.7...7.7..."},
    "snare": {"v": "........77889999", "a": "...............A"},
    "lo":    {"v": "............88.."},
}, bpm=(85, 120))
S("eucl.cinquillo", "Cinquillo E(5,8)", "euclidean", 96, std_roles("eucl.cinquillo"), STD_ARR)

P("eucl.five16.sparse", "Euclid E(5,16) — sparse", "euclidean", ["euclidean"], {
    "kick": {"v": "9..7..7..7..7..."},
    "chh":  {"v": "6...6...6...6..."},
}, bpm=(95, 125))
P("eucl.five16.main", "Euclid E(5,16) — main", "euclidean", ["euclidean"], {
    "kick":  {"v": "9..7..7..7..7..."},
    "snare": {"v": "....8.......8...", "a": "....A.......A..."},
    "chh":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(95, 125), notes="E(5,16) kick — quasi-bossa spacing.")
P("eucl.five16.lift", "Euclid E(5,16) — lift", "euclidean", ["euclidean"], {
    "kick":  {"v": "9..7..7..7..7..."},
    "snare": {"v": "....8..3....8..3", "a": "....A.......A..."},
    "chh":   {"v": "6464646464646464"},
    "bell":  {"v": "7..7.5.7..7.5.7."},
}, bpm=(95, 125))
P("eucl.five16.fill", "Euclid E(5,16) — fill", "euclidean", ["euclidean", "fill"], {
    "kick":  {"v": "9..7..7..7..7..."},
    "snare": {"v": ".........7788999", "a": "...............A"},
    "ohh":   {"v": "..............6."},
}, bpm=(95, 125))
S("eucl.five16", "Euclid E(5,16)", "euclidean", 105, std_roles("eucl.five16"), STD_ARR)

# ═══════════════════════════ INDIAN ═══════════════════════════
# Stylized tabla thekas: kick = bass strokes (dha/ge/dhin), hi = treble (na/tin/ta),
# cp = ka. Genre-convention stylizations on the 16-grid, not transcriptions.
#                                  1e&a2e&a3e&a4e&a
P("indian.keherwa.sparse", "Keherwa — sparse", "indian", ["keherwa", "tabla"], {
    "kick": {"v": "9...........7..."},
    "hi":   {"v": "....5...5......."},
}, bpm=(85, 115))
P("indian.keherwa.main", "Keherwa — main", "indian", ["keherwa", "tabla"], {
    "kick": {"v": "9.7.........7...", "a": "A..............."},
    "hi":   {"v": "....7.6.7.....6."},
    "cp":   {"v": "..........5....."},
    "chh":  {"v": "5.5.5.5.5.5.5.5."},
}, bpm=(85, 115), notes="Dha ge na ti · na ka dhi na, one stroke per 8th.")
P("indian.keherwa.lift", "Keherwa — lift", "indian", ["keherwa", "tabla"], {
    "kick": {"v": "9.7.........7...", "a": "A..............."},
    "hi":   {"v": "....7.6.7.3...6."},
    "cp":   {"v": "..........5....."},
    "chh":  {"v": "4444444444444444"},
    "lo":   {"v": "............7..."},
}, bpm=(85, 115))
P("indian.keherwa.fill", "Keherwa — fill", "indian", ["keherwa", "fill"], {
    "kick": {"v": "9...........9..."},
    "hi":   {"v": "..55..66..778899", "a": "...............A"},
}, bpm=(85, 115), notes="Tirakita-style run into sam.")
S("indian.keherwa", "Keherwa (8)", "indian", 100, std_roles("indian.keherwa"), STD_ARR)

P("indian.teental.sparse", "Teental — sparse", "indian", ["teental", "tabla"], {
    "kick": {"v": "9.......7......."},
    "hi":   {"v": "7...5...5...5..."},
}, bpm=(80, 110))
P("indian.teental.main", "Teental — main", "indian", ["teental", "tabla"], {
    "kick": {"v": "955675567....567", "a": "A..............."},
    "hi":   {"v": "7555755575557555", "a": "....A.......A..."},
}, bpm=(80, 110), notes="16 matras, one per 16th; khali (matras 10-12) drops the bass.")
P("indian.teental.lift", "Teental — lift", "indian", ["teental", "tabla"], {
    "kick": {"v": "955675567....567", "a": "A..............."},
    "hi":   {"v": "7565756575657565", "a": "....A.......A..."},
    "chh":  {"v": "4444444444444444"},
    "lo":   {"v": "9...........7..."},
}, bpm=(80, 110))
P("indian.teental.fill", "Teental — fill", "indian", ["teental", "fill"], {
    "kick": {"v": "9..............."},
    "hi":   {"v": "77.5.77.5.77.5.9", "a": "...............A"},
}, bpm=(80, 110), notes="Tihai-flavoured: the figure states three times into sam.")
S("indian.teental", "Teental (16)", "indian", 92, std_roles("indian.teental"), STD_ARR)

# ═══════════════════════════ GAMELAN ═══════════════════════════
# Colotomic stylizations: kick = gong, bell = kenong, lo = kempul/jegogan,
# hi = balungan/polos, cp = sangsih/peking.
#                                  1e&a2e&a3e&a4e&a
P("gamelan.lancaran.sparse", "Lancaran — sparse", "gamelan", ["gamelan", "lancaran"], {
    "kick": {"v": "9...............", "a": "A..............."},
    "bell": {"v": "....6...6...6..."},
    "hi":   {"v": "5...5...5...5..."},
}, bpm=(100, 140))
P("gamelan.lancaran.main", "Lancaran — main", "gamelan", ["gamelan", "lancaran"], {
    "kick": {"v": "9...............", "a": "A..............."},
    "bell": {"v": "....7...7...7..."},
    "lo":   {"v": "......6...6...6."},
    "hi":   {"v": "6.6.6.6.6.6.6.6."},
}, bpm=(100, 140), notes="Gong on 1, kenong on the beats, kempul between, balungan 8ths.")
P("gamelan.lancaran.lift", "Lancaran — lift", "gamelan", ["gamelan", "lancaran"], {
    "kick": {"v": "9...............", "a": "A..............."},
    "bell": {"v": "....7...7...7..."},
    "lo":   {"v": "......6...6...6."},
    "hi":   {"v": "6.6.6.6.6.6.6.6."},
    "cp":   {"v": "4545454545454545"},
}, bpm=(100, 140), notes="Peking-style 16th doubling on cp.")
P("gamelan.lancaran.fill", "Lancaran — fill", "gamelan", ["gamelan", "fill"], {
    "kick": {"v": "9..............."},
    "bell": {"v": "....7...7...7..."},
    "hi":   {"v": ".......566778899", "a": "...............A"},
}, bpm=(100, 140))
S("gamelan.lancaran", "Lancaran (gamelan)", "asia", 120, std_roles("gamelan.lancaran"), STD_ARR,
  vary=0.15)

P("gamelan.kotekan.sparse", "Kotekan — sparse", "gamelan", ["gamelan", "kotekan"], {
    "kick": {"v": "9...............", "a": "A..............."},
    "hi":   {"v": "7.7.7..7.7.7..7."},
}, bpm=(110, 150))
P("gamelan.kotekan.main", "Kotekan — main", "gamelan", ["gamelan", "kotekan"], {
    "kick": {"v": "9...............", "a": "A..............."},
    "hi":   {"v": "7.7.7..7.7.7..7."},
    "cp":   {"v": ".6.6..66.6.6..66"},
    "bell": {"v": "....6.......6..."},
}, bpm=(110, 150), notes="Interlocking polos/sangsih; together they fill the 16ths.")
P("gamelan.kotekan.lift", "Kotekan — lift", "gamelan", ["gamelan", "kotekan"], {
    "kick": {"v": "9...............", "a": "A..............."},
    "hi":   {"v": "7.7.7..7.7.7..7.", "a": "A..............."},
    "cp":   {"v": ".6.6..66.6.6..66"},
    "bell": {"v": "....7...7...7..."},
    "lo":   {"v": "9.......9......."},
}, bpm=(110, 150))
P("gamelan.kotekan.fill", "Kotekan — fill", "gamelan", ["gamelan", "fill"], {
    "kick": {"v": "9..............."},
    "hi":   {"v": ".......577889999", "a": "...............A"},
    "cp":   {"v": ".......577889999"},
}, bpm=(110, 150), notes="Unison run — the interlock collapses into one line into the gong.")
S("gamelan.kotekan", "Kotekan (gamelan)", "asia", 132, std_roles("gamelan.kotekan"), STD_ARR,
  vary=0.15)


# Expose every timeline as a playable one-lane set (Timelines bank, genre axis,
# vary=0 — key patterns regulate the ensemble and never vary).
def _gridkey(p):
    return (p["beats"], p["stepsPerBeat"], p["bars"],
            tuple(sorted((l, e["v"], e.get("a", "")) for l, e in p["grid"].items())))

_pat_by_id = {p["id"]: p for p in patterns}
_set_mains = {_gridkey(_pat_by_id[s["roles"]["main"]]) for s in sets
              if isinstance(s["roles"].get("main"), str) and s["roles"]["main"] in _pat_by_id}
# ═══════════════════════════ HIP-HOP ═══════════════════════════
# Each sub-genre gets the grid it actually needs: boom bap and lo-fi on swung
# 16ths, drill on 16ths with `r` rolls, crunk on 24ths because its rolls are
# triplet-subdivided (12ths and 24ths, not 32nds).

P("hiphop.lofi.main", "Lo-fi \u2014 main", "hiphop", ["lofi", "chillhop"], {
    "kick":  {"v": "7......6..6....."},
    "snare": {"v": "....7...2...7..2"},
    "chh":   {"v": "5.4.5.3.5.4.5.3."},
}, swing=0.50, bpm=(70, 90), notes="Restraint is the style \u2014 often no fill for eight bars.")
P("hiphop.lofi.fill", "Lo-fi \u2014 fill", "hiphop", ["lofi", "fill"], {
    "kick":  {"v": "7......6........"},
    "snare": {"v": "....7...2.3.7.32"},
    "chh":   {"v": "5.4.5.3.5.4....."},
}, swing=0.50, bpm=(70, 90))
S("hiphop.lofi", "Lo-fi / chillhop", "hiphop", 80,
  {"main": "hiphop.lofi.main", "fill": ["hiphop.lofi.fill"]},
  [{"role": "main", "bars": 16, "fillEvery": 8}], vary=0.40)

P("hiphop.ukdrill.main", "UK drill \u2014 main", "hiphop", ["drill", "uk"], {
    "kick":  {"v": "9....8..7...8...", "a": "A..............."},
    "snare": {"v": "........9.......", "a": "........A......."},
    "chh":   {"v": "6..5..5.6..5..5.", "r": "..............3."},
}, swing=0.10, bpm=(138, 145), notes="Hats on the 3+3+2 tresillo inherited from grime.")
P("hiphop.ukdrill.lift", "UK drill \u2014 snare on 4", "hiphop", ["drill", "uk"], {
    "kick":  {"v": "9....8..7...8...", "a": "A..............."},
    "snare": {"v": "............9...", "a": "............A..."},
    "chh":   {"v": "6..5..5.6..5..5.", "r": "......4.......4."},
}, swing=0.10, bpm=(138, 145),
   notes="The snare slides from beat 3 to beat 4 on alternate bars \u2014 a UK marker.")
P("hiphop.ukdrill.fill", "UK drill \u2014 fill", "hiphop", ["drill", "fill"], {
    "kick":  {"v": "9...............", "a": "A..............."},
    "snare": {"v": "........9......8", "a": "........A......."},
    "chh":   {"v": "6..5..5.6.5.5.65", "r": "..........4.4.56"},
}, swing=0.10, bpm=(138, 145))
S("hiphop.ukdrill", "UK drill", "hiphop", 142,
  {"main": "hiphop.ukdrill.main", "lift": "hiphop.ukdrill.lift",
   "fill": ["hiphop.ukdrill.fill"]}, MAIN_LIFT_ARR, vary=0.40)

P("hiphop.chidrill.main", "Chicago drill \u2014 main", "hiphop", ["drill", "chicago"], {
    "kick":  {"v": "9.......8...6...", "a": "A..............."},
    "snare": {"v": "........9.......", "a": "........A......."},
    "chh":   {"v": "6.5.6.5.6.5.6.5.", "r": "..............3."},
    "bell":  {"v": "9..............."},
}, swing=0.05, bpm=(130, 145), notes="Crash is structural punctuation, not decoration.")
P("hiphop.chidrill.fill", "Chicago drill \u2014 fill", "hiphop", ["drill", "fill"], {
    "kick":  {"v": "9...............", "a": "A..............."},
    "snare": {"v": "........9...9.99", "a": "........A......."},
    "chh":   {"v": "6.5.6.5.6......."},
    "bell":  {"v": "9..............."},
}, swing=0.05, bpm=(130, 145))
S("hiphop.chidrill", "Chicago drill", "hiphop", 138,
  {"main": "hiphop.chidrill.main", "fill": ["hiphop.chidrill.fill"]}, MAIN_ARR, vary=0.35)

P("hiphop.gfunk.main", "G-funk \u2014 main", "hiphop", ["westcoast", "gfunk"], {
    "kick":  {"v": "8....3..7..3...."},
    "cp":    {"v": "....9.......9..."},
    "chh":   {"v": "5.4.5.4.5.4.5.4."},
    "ohh":   {"v": "....6.......6..."},
    "hi":    {"v": "..4...4...4...4."},
}, swing=0.45, bpm=(90, 105), notes="Tambourine rushes; open hats double the clap.")
P("hiphop.gfunk.fill", "G-funk \u2014 fill", "hiphop", ["gfunk", "fill"], {
    "kick":  {"v": "8....3.........."},
    "cp":    {"v": "....9.......9.99"},
    "chh":   {"v": "5.4.5.4.5......."},
    "hi":    {"v": "..4...4...4.4.44"},
}, swing=0.45, bpm=(90, 105))
S("hiphop.gfunk", "West Coast / G-funk", "hiphop", 96,
  {"main": "hiphop.gfunk.main", "fill": ["hiphop.gfunk.fill"]}, MAIN_ARR, vary=0.45)

# crunk: 24 steps per bar, because its rolls are 12ths and 24ths
P("hiphop.crunk.main", "Crunk \u2014 main", "hiphop", ["crunk", "southern"], {
    "kick":  {"v": "9...........8.....9.....", "a": "A......................."},
    "cp":    {"v": "......9...........9.....", "a": "......A...........A....."},
    "chh":   {"v": "5.....5.....5.....5....."},
}, beats=4, spb=6, bpm=(135, 160), notes="Bare on purpose, so the triplet rolls detonate.")
P("hiphop.crunk.fill", "Crunk \u2014 triplet roll", "hiphop", ["crunk", "fill"], {
    "kick":  {"v": "9......................."},
    "snare": {"v": "......7.7.7.8.8.8.9.9.99", "a": "....................A..A"},
    "chh":   {"v": "5.....5................."},
}, beats=4, spb=6, bpm=(135, 160))
S("hiphop.crunk", "Southern / crunk", "hiphop", 145,
  {"main": "hiphop.crunk.main", "fill": ["hiphop.crunk.fill"]}, MAIN_ARR, vary=0.30)

# ═══════════════════════════ ELECTRONIC ═══════════════════════════
# House/techno keep the kick on every beat and put the identity in the offbeat
# open hat. Electro and 2-step deliberately break that pulse. Footwork gets 32nds
# because its kick is a rhythmic voice, not a pulse.

P("electronic.house.main", "House \u2014 main", "electronic", ["house", "4x4"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "cp":    {"v": "....9.......9..."},
    "chh":   {"v": "5.4.5.4.5.4.5.4."},
    "ohh":   {"v": "..7...7...7...7."},
}, swing=0.15, bpm=(118, 128),
   notes="Open hat on every 8th offbeat is the single most identifying element.")
P("electronic.house.lift", "House \u2014 lift", "electronic", ["house"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "cp":    {"v": "....9.......9..."},
    "chh":   {"v": "5454545454545454"},
    "ohh":   {"v": "..7...7...7...7."},
    "hi":    {"v": "..3...3...3...3."},
}, swing=0.15, bpm=(118, 128))
P("electronic.house.fill", "House \u2014 clap build", "electronic", ["house", "fill"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "cp":    {"v": "....9.......5679", "a": "...............A"},
    "chh":   {"v": "5.4.5.4.5......."},
}, swing=0.15, bpm=(118, 128),
   notes="The kick survives the fill \u2014 muting it reads as a mistake.")
S("electronic.house", "House", "electronic", 124,
  {"main": "electronic.house.main", "lift": "electronic.house.lift",
   "fill": ["electronic.house.fill"]}, MAIN_LIFT_ARR, vary=0.20)

P("electronic.deephouse.main", "Deep house \u2014 main", "electronic", ["deephouse"], {
    "kick":  {"v": "8...8...8...8...", "a": "A...A...A...A..."},
    "cp":    {"v": "....7.......7..."},
    "chh":   {"v": "4...4...4...4..."},
    "ohh":   {"v": "..6...6...6...6."},
    "lo":    {"v": "...3....3...3..3"},
}, swing=0.45, bpm=(118, 125), notes="Shuffled and warm; the perc layer is organic.")
S("electronic.deephouse", "Deep house", "electronic", 122,
  {"main": "electronic.deephouse.main"}, MAIN_ARR, vary=0.35)

P("electronic.techhouse.main", "Tech house \u2014 main", "electronic", ["techhouse"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "cp":    {"v": "....9.......9..."},
    "chh":   {"v": "5454545454545454"},
    "ohh":   {"v": "..7...7...7...7."},
    "hi":    {"v": "..4.3...4.3.4..3"},
}, swing=0.30, bpm=(124, 130), notes="Mechanical, with a busy percussion top-line.")
S("electronic.techhouse", "Tech house", "electronic", 127,
  {"main": "electronic.techhouse.main"}, MAIN_ARR, vary=0.40)

P("electronic.techno.main", "Techno \u2014 main", "electronic", ["techno"], {
    "kick":  {"v": "9...9...9...9...", "a": "A...A...A...A..."},
    "chh":   {"v": "5.5.5.5.5.5.5.5."},
    "ohh":   {"v": "..6...6...6...6."},
    "hi":    {"v": "......4.......4."},
}, swing=0.05, bpm=(130, 145),
   notes="Tension comes from timbre and silence, not fills \u2014 the 'fill' is a kick drop.")
P("electronic.techno.fill", "Techno \u2014 kick drop", "electronic", ["techno", "fill"], {
    "chh":   {"v": "5.5.5.5.5.5.5.5."},
    "ohh":   {"v": "..6...6...6...6."},
}, swing=0.05, bpm=(130, 145))
S("electronic.techno", "Techno", "electronic", 138,
  {"main": "electronic.techno.main", "fill": ["electronic.techno.fill"]},
  MAIN_ARR, vary=0.18)

P("electronic.dubtechno.main", "Dub techno \u2014 main", "electronic", ["dubtechno"], {
    "kick":  {"v": "8...8...8...8..."},
    "chh":   {"v": "..4...4...4...4."},
    "lo":    {"v": "......5........."},
}, swing=0.05, bpm=(118, 128),
   notes="The fewest notes of any 4/4 style; movement comes from delay, not groove.")
S("electronic.dubtechno", "Dub techno", "electronic", 122,
  {"main": "electronic.dubtechno.main"},
  [{"role": "main", "bars": 16}], vary=0.20)

P("electronic.minimal.main", "Minimal \u2014 main", "electronic", ["minimal"], {
    "kick":  {"v": "9...9...9...9..."},
    "cp":    {"v": "............7..."},
    "hi":    {"v": ".2.2.3.2.2.3.2.2"},
    "lo":    {"v": "..2....2..2....2"},
}, swing=0.25, bpm=(125, 132),
   notes="Almost everything audible is a ghost note, 20dB under the kick.")
S("electronic.minimal", "Minimal techno", "electronic", 128,
  {"main": "electronic.minimal.main"}, MAIN_ARR, vary=0.35)

P("electronic.electro.main", "Electro \u2014 main", "electronic", ["electro", "808"], {
    "kick":  {"v": "9......8..9.....", "a": "A..............."},
    "snare": {"v": "....9.......9...", "a": "....A.......A..."},
    "cp":    {"v": "....7.......7..."},
    "chh":   {"v": "5.5.5.5.5.5.5.55"},
}, swing=0.0, bpm=(118, 135),
   notes="Syncopated kick against a rigid backbeat \u2014 not four-on-the-floor.")
P("electronic.electro.fill", "Electro \u2014 808 toms", "electronic", ["electro", "fill"], {
    "kick":  {"v": "9......8........"},
    "snare": {"v": "....9..........."},
    "lo":    {"v": "........7.6.5.4."},
    "hi":    {"v": ".........7.6.5.4"[:16]},
}, swing=0.0, bpm=(118, 135), notes="Tom cascades are idiomatic here, unlike most electronic styles.")
S("electronic.electro", "Electro", "electronic", 128,
  {"main": "electronic.electro.main", "fill": ["electronic.electro.fill"]},
  MAIN_ARR, vary=0.40)

P("electronic.nudisco.main", "Nu-disco \u2014 main", "electronic", ["nudisco"], {
    "kick":  {"v": "9...8...9...8...", "a": "A.......A......."},
    "snare": {"v": "....9.......9...", "a": "....A.......A..."},
    "chh":   {"v": "5454545454545454"},
    "ohh":   {"v": "..7...7...7...7."},
    "hi":    {"v": "4...4...4...4..."},
}, swing=0.35, bpm=(105, 125), notes="House with a live drummer's vocabulary bolted on.")
P("electronic.nudisco.fill", "Nu-disco \u2014 tom cascade", "electronic", ["nudisco", "fill"], {
    "kick":  {"v": "9...8...9......."},
    "snare": {"v": "....9..........."},
    "hi":    {"v": "........8.7.6..."},
    "lo":    {"v": "...........7.6.5"},
}, swing=0.35, bpm=(105, 125),
   notes="The descending disco tom fill \u2014 correct here, wrong almost everywhere else.")
S("electronic.nudisco", "Nu-disco", "electronic", 115,
  {"main": "electronic.nudisco.main", "fill": ["electronic.nudisco.fill"]},
  MAIN_ARR, vary=0.45)

P("electronic.breakbeat.main", "Breakbeat \u2014 main", "electronic", ["breakbeat", "bigbeat"], {
    "kick":  {"v": "9..8..7...9....."},
    "snare": {"v": "...39..29..39..3", "a": "....A.......A..."},
    "chh":   {"v": "5.4.5.4.5.4.5.4."},
    "ohh":   {"v": "......6.......6."},
}, swing=0.30, bpm=(125, 140), notes="Ghost snares inherited from the sampled funk break.")
S("electronic.breakbeat", "Breakbeat / big beat", "electronic", 132,
  {"main": "electronic.breakbeat.main"}, MAIN_ARR, vary=0.55)

P("electronic.jungle.main", "Jungle \u2014 main", "electronic", ["jungle", "amen"], {
    "kick":  {"v": "9.....7...8....."},
    "snare": {"v": "...39..3.2.39..2", "a": "....A.......A..."},
    "chh":   {"v": "5.4.5.4.5.4.5.4."},
    "ohh":   {"v": ".....6.........."},
}, swing=0.35, bpm=(155, 175),
   notes="Ghost snares outnumber accented ones; no two bars identical.")
P("electronic.jungle.fill", "Jungle \u2014 snare rush", "electronic", ["jungle", "fill"], {
    "kick":  {"v": "9..............."},
    "snare": {"v": "...39..3.2.37777", "a": "...............A", "r": "..............34"},
    "chh":   {"v": "5.4.5.4.5......."},
}, swing=0.35, bpm=(155, 175))
S("electronic.jungle", "Jungle", "electronic", 168,
  {"main": "electronic.jungle.main", "fill": ["electronic.jungle.fill"]},
  MAIN_ARR, vary=0.70)

P("electronic.dnb.main", "Drum & bass \u2014 two-step", "electronic", ["dnb", "twostep"], {
    "kick":  {"v": "9.........8....."},
    "snare": {"v": "...39......39...", "a": "....A.......A..."},
    "chh":   {"v": "5.4.5.4.5.4.5.4."},
}, swing=0.15, bpm=(170, 176),
   notes="Ghost snare on the 16th before each backbeat is what makes it roll.")
P("electronic.dnb.fill", "Drum & bass \u2014 roll", "electronic", ["dnb", "fill"], {
    "kick":  {"v": "9..............."},
    "snare": {"v": "...39......39.77", "a": "....A.......A..."},
    "chh":   {"v": "5.4.5.4.5.4.5.55", "r": "..............34"},
}, swing=0.15, bpm=(170, 176))
S("electronic.dnb", "Drum & bass", "electronic", 174,
  {"main": "electronic.dnb.main", "fill": ["electronic.dnb.fill"]}, MAIN_ARR, vary=0.70)

P("electronic.garage.main", "UK garage \u2014 2-step", "electronic", ["garage", "2step"], {
    "kick":  {"v": "9.........8....."},
    "snare": {"v": "....9.......9..."},
    "chh":   {"v": "5.45.4.55.4.5.4."},
    "cp":    {"v": "......3....3...3"},
}, swing=0.65, bpm=(130, 138),
   notes="The missing kicks on beats 2 and 3 are the identity; heavy shuffle.")
S("electronic.garage", "UK garage / 2-step", "electronic", 134,
  {"main": "electronic.garage.main"}, MAIN_ARR, vary=0.50)

# footwork: 32 steps per bar \u2014 the kick is a voice, not a pulse
P("electronic.footwork.main", "Footwork \u2014 main", "electronic", ["footwork", "juke"], {
    "kick":  {"v": "9.......8.......9.....8.......8.",
              "a": "A..............................."},
    "cp":    {"v": "......7.....7.....7.....7.....7."},
    "chh":   {"v": "4...4...4...4...4...4...4...4..."},
}, beats=4, spb=8, bpm=(155, 165),
   notes="Claps grouped in threes against 4/4; kick bursts ratchet, not the hat.")
P("electronic.footwork.fill", "Footwork \u2014 kick burst", "electronic", ["footwork", "fill"], {
    "kick":  {"v": "9.......8.......999999999999999.",
              "r": "..............................3."},
    "cp":    {"v": "......7.....7..................."},
}, beats=4, spb=8, bpm=(155, 165))
S("electronic.footwork", "Footwork / juke", "electronic", 160,
  {"main": "electronic.footwork.main", "fill": ["electronic.footwork.fill"]},
  MAIN_ARR, vary=0.60)

P("electronic.dubstep.main", "Dubstep \u2014 halftime", "electronic", ["dubstep", "halftime"], {
    "kick":  {"v": "9......7........", "a": "A..............."},
    "snare": {"v": "........9.......", "a": "........A......."},
    "chh":   {"v": "..5..5....5..5.5"},
}, swing=0.30, bpm=(138, 142),
   notes="Enormous space; the hats pause on beat 3 to clear room for the snare.")
S("electronic.dubstep", "Dubstep / halftime", "electronic", 140,
  {"main": "electronic.dubstep.main"}, MAIN_ARR, vary=0.40)

P("electronic.triphop.main", "Trip-hop \u2014 main", "electronic", ["triphop", "downtempo"], {
    "kick":  {"v": "8......6..7....."},
    "snare": {"v": "....8...2...8..2"},
    "chh":   {"v": "4.3.4.3.4.3.4.3."},
}, swing=0.35, bpm=(70, 100), notes="A slowed, degraded funk break \u2014 the rhythm is found, not programmed.")
S("electronic.triphop", "Trip-hop", "electronic", 88,
  {"main": "electronic.triphop.main"}, MAIN_ARR, vary=0.50)

for _p in [p for p in patterns if p.get("kind") == "timeline"]:
    if _gridkey(_p) in _set_mains:
        continue                     # a groove set already plays this exact grid
    S(_p["id"] + ".set", _p["name"], "timeline", 110,
      {"main": _p["id"]}, [{"role": "main", "bars": 8}])

# ═════════════════════════════ EMIT ═════════════════════════════
lib = {
    "formatVersion": 1,
    "meta": {
        "title": "SFS drum pattern library v2 (global expansion)",
        "license": "CC0-1.0",
        "provenance": "Original stylizations of traditional and genre-conventional "
                      "rhythms; not transcriptions of specific recordings. "
                      "Key-pattern placements cross-checked against the sources in "
                      "global-drum-patterns-report.md; kind='timeline' marks cyclic "
                      "key patterns, 'source' cites the verifying reference.",
    },
    "lanes": LANES,
    "patterns": patterns,
    "sets": sets,
}

ids = [p["id"] for p in patterns]
assert len(ids) == len(set(ids)), "duplicate pattern ids"
for s in sets:
    refs = []
    for r, v in s["roles"].items():
        refs += v if isinstance(v, list) else [v]
    for ref in refs:
        assert ref in ids, f"set {s['id']}: unresolved ref {ref}"

# Resolve relative to this file, not the shell's cwd: running it from the repo
# root silently wrote the library to the top level and left patterns/ stale.
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "drum-patterns-v2.json")
with open(out, "w") as f:
    json.dump(lib, f, indent=1, ensure_ascii=False)
print(f"{len(patterns)} patterns, {len(sets)} sets -> {out}")
