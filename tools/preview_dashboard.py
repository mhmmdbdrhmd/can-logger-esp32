#!/usr/bin/env python3
"""
Run the dashboard in a normal browser, with no ESP32 and no CAN bus.

The page is extracted straight out of src/webpage.cpp, so it can never drift
from what the firmware actually serves, and every endpoint it talks to is
simulated here: live signal values, the saved layout, the frame map the editor
picks signals from, and the transmit path including its failure modes.

    python3 tools/preview_dashboard.py --dbc examples/machine.dbc
    python3 tools/preview_dashboard.py --dbc examples/machine.dbc --cfg examples/dash.cfg
    python3 tools/preview_dashboard.py                    # no frame map at all

Then open http://127.0.0.1:8080 and use it exactly as you would use the real
thing: customize the dashboard, drag cells around, arm the Send tab.

THE DATA IS INVENTED. The point of this tool is the interface and the
interaction; the numbers are made up here in Python and prove nothing about the
firmware. Layout changes are kept in memory, and written back to --cfg if one
was given, so a layout worked out here can be copied to the SD card.

Only the standard library is used.
"""
import argparse
import http.server
import json
import math
import random
import re
import shutil
from decimal import Decimal, InvalidOperation
import socketserver
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "webpage.cpp"


def load_page():
    """Every R"HTML(...)HTML" chunk in webpage.cpp, in order - which is exactly
    what handleRoot() sends."""
    parts = re.findall(r'R"HTML\((.*?)\)HTML"', SRC.read_text(), re.S)
    if not parts:
        sys.exit(f"could not find the page literals in {SRC}")
    return "".join(parts).encode()


# ---------------------------------------------------------------------------
#  A DBC reader, deliberately shallow
#
#  It reads enough to drive the picker and to invent plausible values: name,
#  unit, the [min|max] annotation, the range the bits can hold, and the value
#  labels. The real parser is src/dbc.cpp, and it is the one that matters -
#  this exists so the preview can show what the editor will show.
# ---------------------------------------------------------------------------
SG_RE = re.compile(
    r'^\s*SG_\s+(\w+)\s*((?:[mM]\d*\s*)*):\s*'
    r'(\d+)\|(\d+)@([01])([+-])\s*'
    r'\(([^,]+),([^)]+)\)\s*'
    r'\[([^|\]]*)\|([^\]]*)\]\s*'
    r'"([^"]*)"'
)


def firmware_version():
    """Read from config.h, so the preview cannot claim a version the firmware
    does not have - it did, for a while, and nothing caught it."""
    try:
        m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"',
                      (ROOT / "src" / "config.h").read_text())
        return m.group(1) if m else "?"
    except OSError:
        return "?"


def factor_decimals(fac_text, off_text):
    """Decimal places the scaling needs - the same rule as src/dbc.cpp.

    The firmware works the physical value out in integers: physical * 10^dec
    equals raw * num + off. dec is however many places the factor AND the
    offset between them require, after trailing zeros are dropped. Getting this
    wrong here would show a different number of digits at a desk than in the
    field, which is exactly the drift test/run_tests.sh asserts against.
    """
    def places(text):
        try:
            d = Decimal(str(text).strip()).normalize()
        except (InvalidOperation, ValueError):
            return 3
        return max(0, -d.as_tuple().exponent)
    return max(places(fac_text), places(off_text))


def _multipart_file(raw, ctype):
    """The one file out of a multipart/form-data body, as (name, bytes).

    The firmware streams these to the SD card with the ESP32 WebServer's own
    parser; here there is exactly one known sender - our own page - so finding
    the boundary and taking what is between the headers and the closing marker
    is enough, and pulling in `email` or `cgi` for it is not.

    The name matters here in a way it does not on the logger, which has exactly
    one card and calls it /frames.dbc. At a desk you load one map after
    another, and the setup you build has to land beside the map it was built
    for rather than in a single file that outlives all of them.
    """
    m = re.search(r'boundary=(?:"([^"]+)"|([^;]+))', ctype or "")
    if not m:
        return "", b""
    sep = b"--" + (m.group(1) or m.group(2)).strip().encode()
    for part in raw.split(sep):
        head, _, data = part.partition(b"\r\n\r\n")
        if b"filename=" not in head or not data:
            continue
        fn = re.search(rb'filename="([^"]*)"', head)
        name = fn.group(1).decode("utf-8", "replace") if fn else ""
        # Exactly the one CRLF that separates the data from the next boundary -
        # rstrip would eat a final newline the file genuinely has.
        return name, (data[:-2] if data.endswith(b"\r\n") else data)
    return "", b""


def _sig_of(line):
    """The Message.Signal a cell or sendable line names, or None."""
    m = re.search(r'sig=("([^"]*)"|(\S+))', line)
    return (m.group(2) or m.group(3)) if m else None


def prune_cfg(text, by_ref, nodes):
    """Everything in a setup that this frame map cannot account for, removed.

    The same rule as dashDropUnresolved() in src/dash.cpp, and it has to STAY
    the same rule: a setup built here is copied onto the card, so a preview
    that keeps what the firmware would drop teaches a layout the logger will
    not have. Slots are renumbered from zero for the same reason the firmware
    compacts its arrays - a hole where the old map's signals used to be reads
    as a layout that is still half there.
    """
    out, dropped = [], 0
    cells = sends = 0
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("cell "):
            ref = _sig_of(s)
            if not ref or ref not in by_ref:
                dropped += 1
                continue
            out.append(re.sub(r"^(\s*cell)\s+\d+", r"\g<1> %d" % cells, line))
            cells += 1
        elif s.startswith("send "):
            ref = _sig_of(s)
            # A one-off frame names an identifier, not a signal, so no frame
            # map can invalidate it.
            if ref and ref not in by_ref:
                dropped += 1
                continue
            out.append(re.sub(r"^(\s*send)\s+\d+", r"\g<1> %d" % sends, line))
            sends += 1
        elif s.startswith(("role ", "node ")):
            m = re.match(r'(?:role|node)\s+("([^"]*)"|(\S+))', s)
            name = (m.group(2) or m.group(3)) if m else ""
            # No BU_ node of this name means the answer is not stale but
            # unanswerable: nothing in the new file transmits under it.
            if name and name in nodes:
                out.append(line)
            else:
                dropped += 1
        else:
            out.append(line)
    return "\n".join(out) + "\n", dropped


def load_dbc(path):
    if not path:
        return {"loaded": 0, "m": []}

    msgs, cur = [], None
    vals = {}
    nodes = []
    for line in Path(path).read_text().splitlines():
        s = line.strip()
        if s.startswith("BU_"):
            # "BU_: A B" and "BU_ A B" are both in the wild.
            for n in s[3:].lstrip(":").split():
                if n not in nodes:
                    nodes.append(n)
        elif s.startswith("BO_ "):
            p = s.split()
            # p[3] is the dlc, p[4] the TRANSMITTER - the only direction a DBC
            # states. Kept so the customizer can tell commands from readings.
            tx = p[4] if len(p) > 4 else ""
            if tx == "Vector__XXX":
                tx = ""
            if tx and tx not in nodes:
                nodes.append(tx)
            cur = {"n": p[2].rstrip(":"), "id": "0x%03X" % (int(p[1]) & 0x1FFFFFFF),
                   "tx": tx, "mux": 0, "s": []}
            msgs.append(cur)
        elif s.startswith("SG_ ") and cur is not None:
            m = SG_RE.match(line)
            if not m:
                continue
            name, mux, start, bits, order, sign, fac, off, lo, hi, unit = m.groups()
            dec = factor_decimals(fac, off)     # from the TEXT, before float()
            bits, fac, off = int(bits), float(fac), float(off)
            signed = sign == "-"
            rlo = -(2 ** (bits - 1)) if signed else 0
            rhi = (2 ** (bits - 1)) - 1 if signed else (2 ** bits) - 1
            blo, bhi = sorted((rlo * fac + off, rhi * fac + off))
            try:
                alo, ahi = float(lo), float(hi)
            except ValueError:
                alo = ahi = 0.0
            # -1 plain, -2 the multiplexor, >= 0 the code that selects it.
            mux = (mux or "").strip()
            if mux == "M":
                mx = -2
                cur["mux"] = 1
            elif mux.startswith("m") and mux[1:].isdigit():
                mx = int(mux[1:])
            else:
                mx = -1
            cur["s"].append({
                "i": len(cur["s"]), "n": name, "u": unit, "b": bits,
                "r": 1 if ahi > alo else 0, "lo": alo, "hi": ahi,
                "blo": round(blo, 6), "bhi": round(bhi, 6), "d": dec,
                "mx": mx, "v": [], "_msg": cur["n"],
            })
        elif s.startswith("VAL_ "):
            p = s.split(None, 3)
            if len(p) >= 4:
                labels = re.findall(r'"([^"]*)"', p[3])
                vals.setdefault(p[2], []).extend(labels)

    for msg in msgs:
        for sg in msg["s"]:
            sg["v"] = vals.get(sg["n"], [])

    return {"loaded": 1 if msgs else 0, "nodes": nodes, "m": msgs}


# ---------------------------------------------------------------------------
#  Inventing values
#
#  Named signals get a shape that looks like a machine doing something, so the
#  widgets can be judged the way they will be seen. Everything else gets a sine
#  across whatever range the file annotates.
# ---------------------------------------------------------------------------
def duty(t, period, lo, hi):
    """A smooth ramp up, hold, ramp down - a machine working, not a sine."""
    x = (t % period) / period
    if x < 0.25:
        f = x / 0.25
    elif x < 0.55:
        f = 1.0
    elif x < 0.8:
        f = 1 - (x - 0.55) / 0.25
    else:
        f = 0.0
    return lo + (hi - lo) * f


def simulate(sig, t, over, shown=None):
    """A plausible value for this signal.

    `shown` is the range the dashboard cell was given, if any. When it differs
    from the range the frame map declares, the value is remapped into it, so a
    gauge narrowed to 0..10 sweeps 0..10 instead of sitting pegged at the top -
    the layout being built is what this tool exists to show.
    """
    name = sig["n"]
    if name in over:
        return over[name]

    lo = sig["lo"] if sig["r"] else sig["blo"]
    hi = sig["hi"] if sig["r"] else sig["bhi"]
    if not (hi > lo) or hi - lo > 1e7:
        lo, hi = 0.0, 100.0

    v = _natural(sig, t, name, lo, hi)
    if shown is None:
        return v

    slo, shi = shown
    if not (shi > slo) or (abs(slo - lo) < 1e-9 and abs(shi - hi) < 1e-9):
        return v                       # the cell kept the file's own range

    span = hi - lo
    frac = 0.5 if span <= 0 else (v - lo) / span
    frac = 0.0 if frac < 0 else (1.0 if frac > 1 else frac)
    return slo + frac * (shi - slo)


def _natural(sig, t, name, lo, hi):

    n = name.lower()
    if sig["v"]:
        return float(int(t / 6) % len(sig["v"]))
    if "groundspeed" in n:
        return duty(t, 34, 0, 17.5) + random.uniform(-.12, .12)
    if "enginespeed" in n:
        return 850 + duty(t, 34, 0, 1250) + random.uniform(-14, 14)
    if "throttle" in n:
        return duty(t, 34, 4, 82)
    if "steeringangle" in n:
        return 34 * math.sin(t / 5.5)
    if "articulation" in n:
        return 28 * math.sin(t / 5.5 - 0.4)
    if "steeringrate" in n:
        return 60 * math.cos(t / 5.5)
    if "heading" in n:
        return (t * 7.5) % 360
    if "roll" in n:
        return 6 * math.sin(t / 3.1)
    if "pitch" in n:
        return 4 * math.sin(t / 4.7 + 1)
    if "systempressure" in n:
        return 120 + duty(t, 34, 0, 170) + random.uniform(-4, 4)
    if "pilotpressure" in n:
        return 28 + 9 * math.sin(t / 2.2)
    if "oiltemperature" in n:
        return min(84, 22 + t * 0.5) + 1.4 * math.sin(t / 9)
    if "coolanttemp" in n:
        return min(89, 25 + t * 0.62) + 1.1 * math.sin(t / 7)
    if "oillevel" in n:
        return 72 - 0.4 * math.sin(t / 11)
    if "fuellevel" in n:
        return max(6, 64 - t * 0.045)
    if "tyrepress" in n:
        base = {"fl": 4.4, "fr": 4.35, "rl": 3.1, "rr": 2.05}.get(n[-2:], 4.0)
        return base + 0.03 * math.sin(t / 6 + len(n))
    if "axleload" in n:
        return 5200 + duty(t, 34, 0, 3400) + random.uniform(-60, 60)
    if "batteryvoltage" in n:
        return 27.9 + 0.35 * math.sin(t / 4) + random.uniform(-.04, .04)
    if "alternatorcurrent" in n:
        return 42 + 16 * math.sin(t / 6)

    span = hi - lo
    return lo + span * (0.5 + 0.44 * math.sin(t / 4 + len(name)))


def render(sig, value):
    if sig["v"]:
        i = int(round(value))
        if 0 <= i < len(sig["v"]):
            return sig["v"][i]
    return f"{value:.{sig['d']}f}"


# ---------------------------------------------------------------------------
#  The layout, held in memory and mirrored to --cfg
# ---------------------------------------------------------------------------
DEFAULT_CFG = "version 1\ngrid 4 2\npoll 200\n"

TX_STATUS = {
    0: "sent and acknowledged",
    1: "nothing on the bus acknowledged it",
    2: "the bus was too busy to get on",
    8: "the dashboard is not armed",
    9: "this frame map has no such signal",
    10: "the value does not fit the signal",
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dbc", default=str(ROOT / "examples" / "machine.dbc"),
                    help="frame map to show in the signal picker "
                         "(default: examples/machine.dbc)")
    ap.add_argument("--no-dbc", action="store_true",
                    help="show the page as it looks with no frame map at all")
    ap.add_argument("--cfg",
                    help="setup file to start from, and write back to. Default: a "
                         "COPY of examples/dash.cfg in the temp directory, so the "
                         "preview always opens with something to look at and never "
                         "edits the file in the repo")
    ap.add_argument("--cfg-dir",
                    help="where a setup goes when the frame map is loaded from "
                         "the PAGE rather than named here: <that map>.cfg in "
                         "this directory. One setup per frame map, so loading a "
                         "second map can never open on the first one's layout "
                         "or write over its file")
    ap.add_argument("--empty", action="store_true",
                    help="start with no setup at all - the page as it looks on a "
                         "logger with nothing on its card")
    ap.add_argument("--role", default="",
                    help="which BU_ node of the frame map this logger IS, so "
                         "Fill can tell a reading from a command. Leave it out "
                         "and nothing is separated - both Fill buttons offer "
                         "every message, which is the right answer when you are "
                         "only listening. Changeable in the page afterwards, "
                         "from the Role button in the header.")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--run", default="",
                    help="JavaScript to run once the page has loaded. Only for "
                         "capturing screenshots of things that need a click - "
                         "it is appended by THIS tool, never by the firmware.")
    args = ap.parse_args()

    if args.no_dbc:
        args.dbc = None

    # Opening on an empty setup is what made an earlier review conclude the
    # example sendable values had been deleted - they were in examples/dash.cfg
    # the whole time, and the preview simply was not started with it.
    #
    # The example layout is seeded ONLY when the example frame map is also in
    # use. Against somebody else's .dbc every cell in it would name a signal
    # that does not exist, which is a worse start than an empty grid - and with
    # NO frame map at all it is worse still: eight cells that all read
    # "unknown", which is precisely the stale-looking page this tool must never
    # open on.
    example_dbc = ROOT / "examples" / "machine.dbc"
    own_dbc = bool(args.dbc) and Path(args.dbc).resolve() != example_dbc.resolve()
    if not args.cfg and not args.empty and args.dbc and not own_dbc:
        seed = ROOT / "examples" / "dash.cfg"
        if seed.exists():
            args.cfg = str(Path(tempfile.gettempdir()) / "preview-dash.cfg")
            shutil.copyfile(seed, args.cfg)

    dbc = load_dbc(args.dbc)
    flat = [s for m in dbc["m"] for s in m["s"]]
    by_ref = {s["_msg"] + "." + s["n"]: s for s in flat}

    start_cfg = (Path(args.cfg).read_text() if args.cfg and Path(args.cfg).exists()
                 else DEFAULT_CFG)
    if args.role:
        known = [n for n in dbc.get("nodes", [])]
        if known and args.role not in known:
            sys.exit("--role %s is not a node in %s.  It names: %s"
                     % (args.role, args.dbc, ", ".join(known) or "(none)"))
        # Replace rather than append: an existing file may already carry one.
        lines = [ln for ln in start_cfg.splitlines()
                 if not ln.strip().startswith(("role ", "node "))]
        lines.insert(1 if lines else 0, 'role "%s"' % args.role)
        start_cfg = "\n".join(lines) + "\n"

    state = {
        "cfg": start_cfg,
        "gen": 1,
        "armed": False,
        "arm_until": 0.0,
        "cyc": 0,
        "ticket": 0,
        "results": [],
        "sent": 0,
        "failed": 0,
        "over": {},          # signals the Send tab has written
    }
    t0 = time.time()

    def parse_cells(text):
        """Slot -> Message.Signal, the only part of the layout this tool needs."""
        cols = rows = None
        poll = 200
        cells = {}
        ranges = {}
        for line in text.splitlines():
            line = line.strip()
            if line.startswith("grid "):
                p = line.split()
                cols, rows = int(p[1]), int(p[2])
            elif line.startswith("poll "):
                poll = int(line.split()[1])
            elif line.startswith("cell "):
                p = line.split(None, 2)
                rest = p[2] if len(p) > 2 else ""
                m = re.search(r'sig=("([^"]*)"|(\S+))', rest)
                if m:
                    ref = m.group(2) or m.group(3)
                    cells[int(p[1])] = ref
                    # The range the operator chose for this cell, which is what
                    # the simulated value has to move within. A gauge narrowed
                    # to 0..10 that still gets a 0..50 sweep just sits pegged,
                    # which tells you nothing about the layout you are building.
                    lo = re.search(r'\blo=(-?[\d.eE+-]+)', rest)
                    hi = re.search(r'\bhi=(-?[\d.eE+-]+)', rest)
                    if lo and hi:
                        try:
                            ranges[ref] = (float(lo.group(1)), float(hi.group(1)))
                        except ValueError:
                            pass
        return cols or 4, rows or 2, poll, cells, ranges

    def armed_now():
        return state["armed"] and time.time() < state["arm_until"]

    def add_result(status, ident, cmd=255, clamped=0, tec=0):
        state["ticket"] += 1
        state["results"].append({
            "t": state["ticket"], "s": status, "cmd": cmd, "id": ident,
            "c": clamped, "tec": tec, "m": TX_STATUS.get(status, "unknown"),
        })
        state["results"][:] = state["results"][-8:]
        if status == 0:
            state["sent"] += 1
        else:
            state["failed"] += 1
        return state["ticket"]

    def make_dash():
        t = time.time() - t0
        cols, rows, poll, cells, ranges = parse_cells(state["cfg"])
        n = min(cols * rows, 48)  # noqa: F841 - rows is used for the count
        v, f = [], []
        for i in range(n):
            ref = cells.get(i)
            if ref is None:
                v.append(None)
                f.append(0)
            elif ref not in by_ref:
                v.append(False)          # configured, but not in this frame map
                f.append(0)
            else:
                sg = by_ref[ref]
                v.append(render(sg, simulate(sg, t, state["over"],
                                             ranges.get(ref))))
                f.append(1)
        d = {
            "gen": state["gen"], "poll": poll,
            "v": v, "f": f,
            "arm": 1 if armed_now() else 0,
            "armLeft": max(0, int(state["arm_until"] - time.time())),
            "txOk": state["sent"], "txBad": state["failed"], "cyc": state["cyc"],
            "canTx": 1,
            "n": state["ticket"], "tx": state["results"][-4:],
        }
        # The firmware sends the health counters here as well, so the dashboard
        # is one request rather than two. Mirror that, minus the big tables.
        st = make_status()
        for k in ("rec", "can", "sd", "sdErr", "sdType", "sdMB", "lost", "fps",
                  "irq", "intStuck", "intLevel", "load", "risk", "file",
                  "elapsed", "rows", "kb", "pf", "dbc", "up", "heap", "ap",
                  "ip", "fw"):
            d[k] = st[k]
        return d

    def make_status():
        t = time.time() - t0
        _, _, _, _, ranges = parse_cells(state["cfg"])
        sig = []
        for s in flat[:48]:
            ref = s["_msg"] + "." + s["n"]
            sig.append({"m": s["_msg"], "s": s["n"], "u": s["u"],
                        "v": render(s, simulate(s, t, state["over"],
                                                ranges.get(ref)))})
        ids = [{"id": m["id"], "d": "%016X" % (random.getrandbits(64)),
                "n": int(t * 50), "r": 50, "k": True} for m in dbc["m"]]
        ids.append({"id": "0x3FF", "d": "11223344", "n": int(t * 8), "r": 8,
                    "k": False})
        return {
            "sd": 1, "sdErr": 0, "sdType": "SDHC", "sdMB": 15193,
            "rec": 1, "file": "1.csv", "elapsed": int(t),
            "rows": int(t * 220), "kb": int(t * 15), "pf": 0, "risk": 1000,
            "can": 1, "fps": 50 * max(1, len(dbc["m"])) + 8, "lost": 0,
            "irq": 50 * max(1, len(dbc["m"])) + 8, "intStuck": 0, "intLevel": 1,
            "load": 14,
            "dbc": dbc["loaded"], "dbcMsg": len(dbc["m"]), "dbcSig": len(flat),
            "ids": ids, "idMore": 0, "sig": sig, "sigMore": 0,
            "ap": 1, "ip": "192.168.4.1",
            "up": int(t * 1000), "heap": 198744,
            "fw": "CAN Logger ESP32 v%s  (PREVIEW - every number here is "
                  "invented)" % firmware_version(),
        }

    base_log = [
        "[     0.412] I ==== CAN Logger ESP32 v%s ====" % firmware_version(),
        "[     0.690] I SD card OK: SDHC, 15193 MB",
        (f"[     0.735] I frame map: {len(dbc['m'])} messages, {len(flat)} "
         f"signals from /frames.dbc") if flat else
        "[     0.735] I no /frames.dbc on the card - recording raw payload bytes.",
        "[     0.780] I dashboard layout read from flash",
        "[     0.802] I CAN controller OK: 250 kbit/s, normal mode",
        "[     1.140] I HOTSPOT 'CAN-Logger' is up - open http://192.168.4.1",
        "[     1.201] I RECORDING STARTED -> /1.csv (+ /1.log)",
    ]

    class H(http.server.BaseHTTPRequestHandler):
        def _send(self, body, ctype="application/json", code=200):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            # The firmware sets this on every response; without it here the
            # browser caches the page and a change to webpage.cpp appears not
            # to have happened, which is a very convincing way to review work
            # that is not there.
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj):
            self._send(json.dumps(obj))

        def do_GET(self):
            p = urllib.parse.urlparse(self.path).path
            if p == "/api/status":
                self._json(make_status())
            elif p == "/api/dash":
                self._json(make_dash())
            elif p == "/api/dash/cfg":
                self._send(state["cfg"], "text/plain")
            elif p == "/api/signals":
                self._json(dbc)
            elif p == "/api/log":
                m = re.search(r"since=(\d+)", self.path)
                since = int(m.group(1)) if m else 0
                tick = int(time.time() - t0)
                lines = base_log + [
                    f"[{i:10.3f}] I REC 1.csv 00:00:{i:02d} | {i*220} rows "
                    f"{i*15} KB | 220 f/s | {len(dbc['m'])+1} ids | lost 0"
                    for i in range(2, tick + 2)
                ]
                self._json({"seq": len(lines), "lines": lines[since:]})
            else:
                # Re-read on every request rather than caching at start-up.
                # Editing webpage.cpp and pressing refresh is the whole point of
                # this tool, and a cached page silently shows the old one - which
                # is a very convincing way to review a change that is not there.
                body = load_page()
                if args.run:
                    # Appended here, in the preview, so the firmware's page has
                    # no hook in it that exists only to take screenshots.
                    body += ("<script>setTimeout(function(){%s},900);</script>"
                             % args.run).encode()
                self._send(body, "text/html")

        def do_POST(self):
            p = urllib.parse.urlparse(self.path).path
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n) if n else b""
            body = raw.decode("utf-8", "replace")
            form = dict(urllib.parse.parse_qsl(body))

            if p == "/api/dbc":
                nonlocal dbc, flat, by_ref
                name, data = _multipart_file(raw,
                                             self.headers.get("Content-Type", ""))
                if not data:
                    self._json({"ok": 0, "err": "the upload did not finish"})
                    return
                tmp = Path(tempfile.gettempdir()) / "preview-frames.dbc"
                tmp.write_bytes(data)
                try:
                    new = load_dbc(str(tmp))
                except Exception as exc:              # noqa: BLE001
                    self._json({"ok": 0, "err": "could not read it: %s" % exc})
                    return
                if not new["m"]:
                    self._json({"ok": 0, "err": "no BO_ messages in that file"})
                    return
                dbc = new
                flat = [sg for m in dbc["m"] for sg in m["s"]]
                by_ref = {sg["_msg"] + "." + sg["n"]: sg for sg in flat}
                args.dbc = str(tmp)
                print("frame map replaced from the web app: "
                      "%d messages, %d signals" % (len(dbc["m"]), len(flat)),
                      flush=True)

                # The setup follows the map. The logger has one card and one
                # /dash.cfg; a desk has a drawer of .dbc files and one window
                # open all afternoon, so the setup for the map being loaded is
                # picked up here and the one for the map being left behind is
                # left alone rather than written over.
                if args.cfg_dir and name:
                    args.cfg = str(Path(args.cfg_dir)
                                   / (Path(name).stem + ".cfg"))
                    state["cfg"] = (Path(args.cfg).read_text()
                                    if Path(args.cfg).exists() else DEFAULT_CFG)

                # And whatever is now in hand is held to this map, by the same
                # rule the firmware applies in dashDropUnresolved().
                state["cfg"], gone = prune_cfg(state["cfg"], by_ref,
                                               dbc.get("nodes", []))
                state["gen"] += 1
                state["over"] = {}       # overrides named the old map's signals
                if args.cfg:
                    Path(args.cfg).write_text(state["cfg"])
                    print("setup for this map: %s%s"
                          % (args.cfg,
                             " (%d item(s) the map does not describe removed)"
                             % gone if gone else ""), flush=True)

                self._json({"ok": 1, "bytes": len(data),
                            "messages": len(dbc["m"]), "signals": len(flat),
                            "nodes": len(dbc.get("nodes", [])),
                            "errors": 0, "inexact": 0, "missing": 0,
                            "dropped": gone, "clipped": 0})
                return

            if p == "/api/dash/cfg":
                state["cfg"] = body
                state["gen"] += 1
                if args.cfg:
                    Path(args.cfg).write_text(body)
                self._json({"ok": 1, "errors": 0, "missing": 0, "gen": state["gen"]})

            elif p == "/api/tx/arm":
                state["armed"] = form.get("on") == "1"
                state["arm_until"] = time.time() + 300 if state["armed"] else 0
                self._json({"arm": 1 if armed_now() else 0,
                            "armLeft": max(0, int(state["arm_until"] - time.time()))})

            elif p == "/api/tx/send":
                if not armed_now():
                    self._json({"ticket": add_result(8, "-"), "n": state["ticket"]})
                    return
                if "id" in form:
                    # A one-off frame to an identifier nothing answers, which is
                    # what a real bus does when the ECU is not there.
                    self._json({"ticket": add_result(1, form["id"], tec=8),
                                "n": state["ticket"]})
                    return
                if "cmds" in form:
                    # A group: several values that only mean anything in the
                    # same frame. The real logger queues them holding and sends
                    # one frame; here they simply all take effect at once.
                    ids = [int(x) for x in form["cmds"].split(",") if x != ""]
                    vals = [float(x or 0) for x in form.get("values", "").split(",")]
                    for cmd, val in zip(ids, vals):
                        ref = self._ref_for(cmd)
                        if ref and ref in by_ref:
                            state["over"][by_ref[ref]["n"]] = val
                    self._json({"ticket": add_result(0, "0x110",
                                                     cmd=ids[0] if ids else 0),
                                "n": state["ticket"]})
                    return
                cmd = int(form.get("cmd", 0))
                val = float(form.get("value", 0) or 0)
                ref = self._ref_for(cmd)
                if ref and ref in by_ref:
                    # Show the effect: the simulated bus now reports what was set.
                    state["over"][by_ref[ref]["n"]] = val
                self._json({"ticket": add_result(0, "0x110", cmd=cmd),
                            "n": state["ticket"]})

            elif p == "/api/tx/cyclic":
                cmd = int(form.get("cmd", 0))
                if form.get("on") == "1" and armed_now():
                    state["cyc"] |= 1 << cmd
                else:
                    state["cyc"] &= ~(1 << cmd)
                self._json({"cyc": state["cyc"], "n": state["ticket"]})

            else:
                self._json({"ok": 1})

        def _ref_for(self, cmd):
            for line in state["cfg"].splitlines():
                if line.startswith(f"send {cmd} "):
                    m = re.search(r'sig=("([^"]*)"|(\S+))', line)
                    if m:
                        return m.group(2) or m.group(3)
            return None

        def log_message(self, *a):
            pass

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", args.port), H) as srv:
        mode = f"frame map: {args.dbc}" if flat else "no frame map"
        print(f"dashboard preview ({mode}) -> http://127.0.0.1:{args.port}", flush=True)
        cells = sum(1 for l in state["cfg"].splitlines() if l.startswith("cell "))
        sends = sum(1 for l in state["cfg"].splitlines() if l.startswith("send "))
        print(f"setup: {cells} dashboard cells, {sends} sendable values", flush=True)
        if flat:
            msgs = len(dbc["m"])
            print(f"frame map: {msgs} messages, {len(flat)} signals")
        if args.cfg:
            print(f"changes are written back to {args.cfg}")
        if own_dbc and not cells:
            print("nothing set up yet - press 'Customize dashboard', then "
                  "'Fill from frame map'")
        print("the data is simulated - Ctrl-C to stop", flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
