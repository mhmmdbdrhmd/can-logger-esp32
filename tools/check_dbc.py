#!/usr/bin/env python3
"""
Does this .dbc load on the logger?

    python3 tools/check_dbc.py path/to/mine.dbc
    python3 tools/check_dbc.py path/to/mine.dbc --list

Run this before copying a frame map onto the SD card. It reports what the
logger will hold, every line it could not read, and whether the file fits the
tables the firmware was built with - a map that is fine on a PC does not
automatically fit the ceilings in config.h, and a map big enough to starve the
Wi-Fi stack is worse than one that decodes a few signals fewer.

Pure Python, so it runs on Windows, macOS and Linux with nothing installed.

WHY THIS IS TRUSTWORTHY
-----------------------
It reads the file with the SAME reader the preview tool uses, and applies the
limits read out of src/config.h. That is a second implementation of the
firmware's parser, which would normally be a reason to distrust it - so
test/run_tests.sh compiles src/dbc.cpp and asserts the two agree, message for
message and signal for signal, on every frame map in examples/. If they ever
drift, that test fails.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from preview_dashboard import load_dbc          # noqa: E402  the shared reader


def limits():
    """The table sizes this firmware was built with."""
    text = (ROOT / "src" / "config.h").read_text()
    out = {}
    for name in ("DBC_MAX_MESSAGES", "DBC_MAX_SIGNALS", "DBC_MAX_VALDESC",
                 "DBC_MAX_NODES", "DBC_LINE_MAX"):
        m = re.search(r"#define\s+%s\s+(\d+)" % name, text)
        out[name] = int(m.group(1)) if m else None
    m = re.search(r"#define\s+DBC_LINE_MAX\s+(\d+)", (ROOT / "src" / "dbc.h").read_text())
    if m:
        out["DBC_LINE_MAX"] = int(m.group(1))
    return out


def check(path, show_list=False, quiet=False):
    raw = Path(path).read_text(errors="replace").splitlines()
    lim = limits()
    db = load_dbc(path)

    # Lines the firmware would recognise as a definition but could not read.
    bad = []
    cur_ok = False
    for n, line in enumerate(raw, 1):
        s = line.strip()
        if s.startswith("BO_ "):
            cur_ok = len(s.split()) >= 4
            if not cur_ok:
                bad.append((n, s))
        elif s.startswith("SG_ "):
            from preview_dashboard import SG_RE
            if not SG_RE.match(line):
                bad.append((n, s))

    msgs = len(db["m"])
    sigs = sum(len(m["s"]) for m in db["m"])
    vals = sum(len(s["v"]) for m in db["m"] for s in m["s"])
    nodes = db.get("nodes", [])
    longest = max((len(l) for l in raw), default=0)

    # An over-long line is read in pieces, and the pieces after the first are
    # not lines the parser recognises - so they are dropped. On a CM_ comment
    # that costs nothing; on a definition it silently loses the definition.
    READ = ("VERSION", "BO_", "SG_", "VAL_", "BA_", "SIG_VALTYPE_", "BU_")
    longs, harmless = [], 0
    for n, l in enumerate(raw, 1):
        if len(l) < lim["DBC_LINE_MAX"]:
            continue
        kw = l.strip().split(" ", 1)[0]
        if kw in READ:
            longs.append((n, kw, len(l)))
        else:
            harmless += 1

    def line(label, got, cap):
        room = "" if cap is None else "  of %d" % cap
        over = cap is not None and got > cap
        return "  %-22s %5d%s%s" % (label, got, room, "   TOO MANY" if over else "")

    # What the tables will cost. They are sized to the file and allocated on
    # the heap, so this is the number that decides whether it fits - the counts
    # above are only ceilings that stop a corrupt file asking for a gigabyte.

    print(path)
    print("  %-22s %5d   longest %d bytes of %s"
          % ("lines read", len(raw), longest, lim["DBC_LINE_MAX"]))
    print(line("messages", msgs, lim["DBC_MAX_MESSAGES"]))
    print(line("signals", sigs, lim["DBC_MAX_SIGNALS"]))
    print(line("value labels", vals, lim["DBC_MAX_VALDESC"]))
    print(line("nodes", len(nodes), lim["DBC_MAX_NODES"]))
    print("  %-22s %5d" % ("unreadable lines", len(bad)))

    # Struct sizes come from the header rather than being guessed: they change
    # whenever DBC_NAME_MAX does.
    name_max = 32
    m = re.search(r"#define\s+DBC_NAME_MAX\s+(\d+)", (ROOT / "src" / "dbc.h").read_text())
    if m:
        name_max = int(m.group(1))
    per_msg = 16 + name_max          # DbcMessage, rounded to its members
    per_sig = 72 + name_max + 21     # DbcSignal + the live-value slot
    per_val = 8 + 16                 # DbcValDesc
    kb = (msgs * per_msg + sigs * per_sig + vals * per_val + 1023) // 1024
    reserve = 88
    m = re.search(r"#define\s+DBC_HEAP_RESERVE\s+(\d+)",
                  (ROOT / "src" / "config.h").read_text())
    if m:
        reserve = int(m.group(1)) // 1024
    print("  %-22s ~%4d KB of heap  (the logger keeps %d KB back for Wi-Fi)"
          % ("this map costs", kb, reserve))
    for n, s in bad[:8]:
        print("    line %d: %s" % (n, s[:100]))
    if len(bad) > 8:
        print("    ... and %d more" % (len(bad) - 8))

    # Direction, which is the part people do not expect a DBC to carry.
    sends = {}
    for m in db["m"]:
        sends.setdefault(m["tx"] or "(unnamed)", []).append(m["n"])
    if nodes:
        print("\n  who sends what")
        for who, what in sends.items():
            print("    %-16s %s" % (who, ", ".join(what)))
        print("    Nothing acts on this - a frame is recorded whoever the file")
        print("    says sends it - but it tells you which of these you would be.")

    muxed = [m for m in db["m"] if m["mux"]]
    if muxed:
        print("\n  multiplexed messages (all of one must be sent together)")
        for m in muxed:
            sel = [s["n"] for s in m["s"] if s["mx"] == -2]
            print("    %s  selector %s" % (m["n"], sel[0] if sel else "?"))

    if show_list:
        print()
        for m in db["m"]:
            print("  %s  %s  %s" % (m["n"], m["id"],
                                    ("sent by " + m["tx"]) if m["tx"] else ""))
            for s in m["s"]:
                rng = ("%g..%g" % (s["lo"], s["hi"])) if s["r"] else \
                      ("%g..%g (bits)" % (s["blo"], s["bhi"]))
                tag = ""
                if s["mx"] == -2:
                    tag = "  [selector]"
                elif s["mx"] >= 0:
                    tag = "  [only when selector = %d]" % s["mx"]
                if s["v"]:
                    tag += "  [named states]"
                print("    %-28s %2d bit  %-22s %s%s"
                      % (s["n"], s["b"], rng, s["u"], tag))

    over = (msgs > (lim["DBC_MAX_MESSAGES"] or msgs)
            or sigs > (lim["DBC_MAX_SIGNALS"] or sigs)
            or vals > (lim["DBC_MAX_VALDESC"] or vals)
            or len(nodes) > (lim["DBC_MAX_NODES"] or len(nodes)))
    ok = bool(msgs) and not bad and not over and not longs

    if longs:
        print("\n  Lines longer than the %d-byte limit that the logger READS -"
              " these lose\n  everything past the cut:" % lim["DBC_LINE_MAX"])
        for n, kw, ln in longs[:8]:
            print("    line %d: %s, %d bytes" % (n, kw, ln))
    if harmless:
        print("\n  %d comment line(s) are longer than %d bytes. The logger "
              "skips comments,\n  so this costs nothing."
              % (harmless, lim["DBC_LINE_MAX"]))
    print("\n" + ("OK - this file loads cleanly. Copy it to the card as "
                  "/frames.dbc."
                  if ok else
                  "PROBLEMS above. The logger will still run, but only with "
                  "what it managed to read."))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("dbc", help="the .dbc file to check")
    ap.add_argument("--list", action="store_true",
                    help="print every message and signal it found")
    a = ap.parse_args()
    if not Path(a.dbc).is_file():
        sys.exit("no such file: %s" % a.dbc)
    return check(a.dbc, a.list)


if __name__ == "__main__":
    sys.exit(main())
