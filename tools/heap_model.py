#!/usr/bin/env python3
"""
Will the dashboard stay reachable with these frame maps? Answered at a desk.

    python3 tools/heap_model.py can1.dbc [can2.dbc] [--name-max 32]

The logger's web server, its Wi-Fi driver and its frame maps share one heap.
Every incoming connection needs a 2.3 KB receive buffer from it; when no block
that large is left, the connection is dropped before any handler runs, and the
page stops loading. What decides it is the LARGEST FREE BLOCK once boot has
finished - the `ready` line in the boot log - and that is a function of the
build and of the maps, both of which are known before anything is flashed.

WHERE THE NUMBERS COME FROM
---------------------------
What a map costs is exact: the firmware allocates it from the counts in the
file (src/dbc.cpp, dbcBytesPerMessage/Signal/Value), and the counts are read
here the same way (dbcCountLine). The rest - what the board has left before
any map, and what a second map costs on top of its entries - was measured on
the bench and is kept in BENCH below with the runs it came from. The allocator
hands out whole kilobytes less a 12-byte header, so every block is
k * 1024 + 1012 and so is every prediction.

THE LINE
--------
At or above SAFE_BLOCK every measured run served every request. Below it the
same map served anything from 3 % to 100 % on repeated runs, so there is no
"probably" to offer: a setup is guaranteed or it is not.
"""

import argparse
import re
import sys

# --- what the firmware allocates, per entry (src/dbc.h, 32-bit ESP32) -------
MSG_FIXED = 20          # sizeof(DbcMessage) without its name
SIG_FIXED = 80          # sizeof(DbcSignal) without its name
LIVE_PER_SIGNAL = 17    # LIVE_TEXT_MAX + a timestamp + a flag
VAL_BYTES = 24          # sizeof(DbcValDesc)
SLACK = (4, 8, 8)       # added to every count before allocating
ALLOCS_PER_MAP = 7      # four tables and three live arrays
ALLOC_HEADER = 20       # what each allocation costs beyond its bytes

# --- measured on the bench (see BENCH) ---------------------------------------
BASE_BLOCK = 56940      # largest block at ready with no frame map at all
SECOND_MAP = 0          # what loading a second map costs beyond its entries
FREE_BEFORE_MAPS = 80092  # total free heap once Wi-Fi is up
DBC_HEAP_RESERVE = 40000  # src/config.h - what a map may never take
SAFE_BLOCK = 40000      # the target: every run at or above served everything
SERVES_AT = 36852       # the lowest block where every measured run served all
DEAD_AT = 28660         # at or below, no measured run served even 10 %

BENCH = """
    build                          maps (msgs/sigs/vals)  name_max  ready block
    FRAME_QUEUE_LEN 512            13/40/15                     64        34804
    FRAME_QUEUE_LEN 256            13/40/15                     64        40948
    256 + LOG_QUEUE 16 + stack 6144 13/40/15                     64        47092  (the default)
"""

NAME_MAX_CHOICES = (64, 32, 16)


def counts(text):
    """(messages, signals, value labels) the way dbcCountLine() counts them:
    every BO_ line, every SG_ line, every quoted string on a VAL_ line."""
    m = s = v = 0
    for line in text.splitlines():
        t = line.lstrip(" \t")
        if t.startswith("BO_ "):
            m += 1
        elif t.startswith("SG_ "):
            s += 1
        elif t.startswith("VAL_ "):
            v += len(re.findall(r'"[^"]*"', t))
    return m, s, v


def entry_bytes(name_max):
    """(message, signal, value label) - what one of each costs."""
    return (MSG_FIXED + name_max,
            SIG_FIXED + name_max + LIVE_PER_SIGNAL,
            VAL_BYTES)


def caps_of(c):
    """The table sizes the firmware asks for, for a file with counts `c`."""
    return tuple(n + k for n, k in zip(c, SLACK)) if c and c[0] else None


def caps_cost(caps, name_max):
    """Bytes tables of these sizes take from the heap."""
    if not caps or not caps[0]:
        return 0
    return sum(n * b for n, b in zip(caps, entry_bytes(name_max))) \
        + ALLOCS_PER_MAP * ALLOC_HEADER


def map_cost(c, name_max):
    """Bytes one map with counts `c` takes from the heap."""
    return caps_cost(caps_of(c), name_max)


def fit(maps, name_max):
    """The table sizes the firmware ends up with, map by map, after fitting
    each to the heap the way recorderLoadDbc() does: what is free before it,
    less DBC_HEAP_RESERVE, scaled down proportionally when it does not fit.
    -> [(caps or None, cut_down)]"""
    free = FREE_BEFORE_MAPS
    out = []
    for c in maps:
        want = caps_of(c)
        if not want:
            out.append((None, False))
            continue
        need = caps_cost(want, name_max)
        room = max(0, free - DBC_HEAP_RESERVE)
        if need <= room:
            caps, cut = want, False
        else:
            k = room / need
            caps, cut = tuple(int(n * k) for n in want), True
        free -= caps_cost(caps, name_max)
        out.append((caps if caps[0] else None, cut))
    return out


def predict_caps(caps_list, name_max):
    """Largest free block at ready for tables of these sizes."""
    loaded = [c for c in caps_list if c and c[0]]
    raw = BASE_BLOCK - sum(caps_cost(c, name_max) for c in loaded)
    if len(loaded) > 1:
        raw -= SECOND_MAP
    return quantise(raw)


def quantise(x):
    """The allocator's staircase: k * 1024 + 1012, rounded down."""
    if x < 1012:
        return 0
    return ((x - 1012) // 1024) * 1024 + 1012


def predict(maps, name_max):
    """Largest free block at ready for these maps. maps: list of count tuples,
    CAN1's first; an empty or None entry is a bus with no map. A map too big
    to load whole is cut down first, exactly as the firmware would."""
    return predict_caps([c for c, _ in fit(maps, name_max)], name_max)


def verdict(block):
    """(guaranteed, one sentence)."""
    if block >= SAFE_BLOCK:
        return True, ("%d bytes at start-up: every measured run with this much "
                      "served every request." % block)
    if block > DEAD_AT:
        return False, ("%d bytes at start-up, under the %d the tool aims for. "
                       "Runs in this range served anywhere from 3%% to 100%% of "
                       "their requests, and which one you get is not "
                       "predictable. Recording is not affected." % (block, SAFE_BLOCK))
    return False, ("%d bytes at start-up: no measured run this low served even "
                   "10%% of its requests. Expect the page to stop answering in "
                   "the first minute. Recording is not affected." % block)


def signals_that_fit(maps, name_max, bus):
    """How many signals the map on `bus` (0 or 1) may keep for the prediction
    to reach SAFE_BLOCK, the other map unchanged. Messages are assumed in
    proportion. None if no amount of pruning reaches it."""
    c = maps[bus]
    if not c or not c[1]:
        return None
    lo, hi = 0, c[1]
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        k = mid / c[1]
        trial = list(maps)
        trial[bus] = (max(1, round(c[0] * k)), mid, round(c[2] * k)) if mid else None
        if predict(trial, name_max) >= SAFE_BLOCK:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("dbc", nargs="?")
    ap.add_argument("dbc2", nargs="?")
    ap.add_argument("--name-max", type=int, choices=NAME_MAX_CHOICES)
    a = ap.parse_args()

    maps = []
    for p in (a.dbc, a.dbc2):
        maps.append(counts(open(p, encoding="utf-8", errors="replace").read())
                    if p else None)
    for i, c in enumerate(maps):
        if c:
            print("CAN%d  %d messages, %d signals, %d value labels"
                  % (i + 1, c[0], c[1], c[2]))
    print("largest free block at ready, by name_max (aiming for %d):" % SAFE_BLOCK)
    for nm in ((a.name_max,) if a.name_max else NAME_MAX_CHOICES):
        b = predict(maps, nm)
        ok, _ = verdict(b)
        print("  %2d  %6d  %s" % (nm, b, "GUARANTEED" if ok else "not guaranteed"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
