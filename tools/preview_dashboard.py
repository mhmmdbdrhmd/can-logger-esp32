#!/usr/bin/env python3
"""
Preview the live dashboard in a normal browser, with no ESP32 and no CAN bus.

Extracts the page straight out of src/webui.cpp (so it can never drift from what
the firmware actually serves) and serves it alongside a fake /api/status and
/api/log that replay a plausible recording.

    python3 tools/preview_dashboard.py                    # raw-frame mode
    python3 tools/preview_dashboard.py --dbc examples/example.dbc

The DBC is only used to invent plausible signal names for the preview - the
firmware parses it on the device. Passing one shows what the page looks like
with a frame map loaded; leaving it out shows the raw-frame view.

Then open http://127.0.0.1:8080.
"""
import argparse
import http.server
import json
import math
import re
import socketserver
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "webui.cpp"


def load_page():
    m = re.search(r'R"HTML\((.*?)\)HTML"', SRC.read_text(), re.S)
    if not m:
        sys.exit(f"could not find the PAGE_HTML literal in {SRC}")
    return m.group(1).encode()


def load_dbc(path):
    """A deliberately shallow read: message and signal names and units only.
    The device-side parser in src/dbc.cpp is the real one."""
    signals, msg = [], None
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line.startswith("BO_ "):
            parts = line.split()
            msg = parts[2].rstrip(":") if len(parts) > 2 else None
        elif line.startswith("SG_ ") and msg:
            name = line.split()[1]
            unit = re.search(r'"([^"]*)"\s*\S*\s*$', line)
            signals.append((msg, name, unit.group(1) if unit else ""))
    return signals


def make_status(signals, t):
    ids = [
        {"id": "0x100", "d": "2A00000002640000", "n": int(t * 100), "r": 100, "k": bool(signals)},
        {"id": "0x101", "d": "18FC00000000E803", "n": int(t * 100), "r": 100, "k": bool(signals)},
        {"id": "0x200", "d": "11223344",         "n": int(t * 20),  "r": 20,  "k": False},
    ]
    sig = []
    for i, (msg, name, unit) in enumerate(signals[:48]):
        wave = 50 + 45 * math.sin(t / 3 + i)
        sig.append({"m": msg, "s": name, "v": f"{wave:.3f}", "u": unit})

    return {
        "sd": 1, "sdErr": 0, "sdType": "SDHC", "sdMB": 15193,
        "rec": 1, "file": "1.csv", "elapsed": int(t),
        "rows": int(t * 220), "kb": int(t * 15),
        "pf": 0, "risk": 1000,
        "can": 1, "fps": 220, "lost": 0,
        # Receive-path health and bus load, as the firmware reports them.
        "irq": 220, "intStuck": 0, "intLevel": 1, "load": 11,
        "dbc": 1 if signals else 0,
        "dbcMsg": len({m for m, _, _ in signals}),
        "dbcSig": len(signals),
        "ids": ids, "idMore": 0,
        "sig": sig, "sigMore": 0,
        "ap": 1, "ip": "192.168.4.1",
        "up": int(t * 1000), "heap": 198744,
        "fw": "CAN Logger ESP32 v1.0.0 (PREVIEW - simulated data)",
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dbc", help="show the page as it looks with this frame map")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    page = load_page()
    signals = load_dbc(args.dbc) if args.dbc else []
    t0 = time.time()

    base_log = [
        "[     0.412] I ==== CAN Logger ESP32 v1.0.0 ====",
        "[     0.690] I SD card OK: SDHC, 15193 MB",
        ("[     0.735] I frame map: %d messages, %d signals from /frames.dbc"
         % (len({m for m, _, _ in signals}), len(signals))) if signals else
        "[     0.735] I no /frames.dbc on the card - recording raw payload bytes.",
        "[     0.802] I CAN controller OK: 250 kbit/s, normal mode",
        "[     1.140] I HOTSPOT 'CAN-Logger' is up - open http://192.168.4.1 in a browser",
        "[     1.201] I RECORDING STARTED -> /1.csv (+ /1.log)",
    ]

    class H(http.server.BaseHTTPRequestHandler):
        def _send(self, body, ctype):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            t = time.time() - t0
            if self.path.startswith("/api/status"):
                self._send(json.dumps(make_status(signals, t)).encode(),
                           "application/json")
            elif self.path.startswith("/api/log"):
                m = re.search(r"since=(\d+)", self.path)
                since = int(m.group(1)) if m else 0
                tick = int(t)
                lines = base_log + [
                    f"[{i:10.3f}] I REC 1.csv 00:00:{i:02d} | {i*220} rows "
                    f"{i*15} KB | 220 f/s | 3 ids | lost 0"
                    for i in range(2, tick + 2)
                ]
                self._send(json.dumps({"seq": len(lines),
                                       "lines": lines[since:]}).encode(),
                           "application/json")
            else:
                self._send(page, "text/html")

        def do_POST(self):
            self._send(b'{"ok":1}', "application/json")

        def log_message(self, *a):
            pass

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", args.port), H) as srv:
        mode = f"frame map: {args.dbc}" if signals else "raw frames (no DBC)"
        print(f"dashboard preview ({mode}) -> http://127.0.0.1:{args.port}"
              f"   Ctrl-C to stop")
        srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
