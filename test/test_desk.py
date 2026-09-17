#!/usr/bin/env python3
"""
test_desk.py - does trimming keep what the dashboard and the Send tab use?

Drives customize.py's server (tools/preview_dashboard.py) over HTTP the way the
page does, with two generated frame maps too large for the logger to serve the
page with. The layout deliberately uses the LARGEST messages - exactly the ones
"largest first" trimming would drop if it forgot the layout - through dashboard
cells and sendable values on both buses, one written with a quoted reference
and one with a multiplexor selector.

Then it takes every remedy offered, and checks: the verdict turns guaranteed,
no cell or sendable value is lost, every message they name is still in the
exported bundle, and the exported layout resolves against the exported maps.

    python3 test/test_desk.py            (run_tests.sh runs it)
"""
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import make_bundle                                           # noqa: E402

fails = 0


def ck(what, ok, info=""):
    global fails
    print("  %-4s %s%s" % ("ok" if ok else "FAIL", what,
                           ("   " + info) if info else ""))
    fails += 0 if ok else 1


def gen_map(prefix, messages, big):
    """`messages` messages of 4 signals; the ones in `big` get 20 signals, so
    they are the first a size-ordered trim would take."""
    out = ["VERSION \"\"", "", "BU_: Logger Machine", ""]
    for i in range(messages):
        n = 20 if i in big else 4
        out.append("BO_ %d %sMessageNumber%d: 8 Machine" % (100 + i, prefix, i))
        if i == big[-1]:
            # multiplexed: a selector and two payloads
            out.append(' SG_ Selector M : 0|8@1+ (1,0) [0|255] "" Logger')
            out.append(' SG_ SetpointA m1 : 8|16@1+ (0.1,0) [0|6553] "bar" Logger')
            out.append(' SG_ SetpointB m2 : 8|16@1+ (1,0) [0|65535] "rpm" Logger')
            n -= 3
        for k in range(n):
            out.append(' SG_ %sSignalWithALongName%d : %d|1@1+ (1,0) [0|1] "" Logger'
                       % (prefix, k, k % 64))
        out.append("")
    for i in big:
        out.append('VAL_ %d %sSignalWithALongName0 0 "off" 1 "on" ;' % (100 + i, prefix))
    return "\n".join(out) + "\n"


BIG1, BIG2 = [3, 7], [10, 20, 30]
map1 = gen_map("Alpha", 60, BIG1)
map2 = gen_map("Beta", 700, BIG2)
layout = "\n".join([
    "version 1", "grid 4 3", "poll 200",
    "cell 0 widget=gauge sig=AlphaMessageNumber3.AlphaSignalWithALongName0 lo=0 hi=1",
    'cell 1 widget=number sig="AlphaMessageNumber7.AlphaSignalWithALongName5"',
    "cell 2 widget=bar sig=BetaMessageNumber10.BetaSignalWithALongName1 bus=2 lo=0 hi=1",
    "cell 3 widget=state sig=BetaMessageNumber20.BetaSignalWithALongName0 bus=2",
    'send 0 label="Pressure" sig=BetaMessageNumber30.SetpointA bus=2 lo=0 hi=100 step=1 preset=10',
    'send 1 label="Engine" sig=AlphaMessageNumber7.AlphaSignalWithALongName2 lo=0 hi=1 step=1 preset=0',
]) + "\n"

tmp = Path(tempfile.mkdtemp())
(tmp / "a.dbc").write_text(map1)
(tmp / "b.dbc").write_text(map2)
(tmp / "dash.cfg").write_text(layout)

s = socket.socket()
s.bind(("127.0.0.1", 0))
port = s.getsockname()[1]
s.close()
base = "http://127.0.0.1:%d" % port
proc = subprocess.Popen(
    [sys.executable, str(ROOT / "tools" / "preview_dashboard.py"),
     "--dbc", str(tmp / "a.dbc"), "--dbc2", str(tmp / "b.dbc"),
     "--cfg", str(tmp / "dash.cfg"), "--port", str(port)],
    stdout=open(tmp / "server.log", "w"), stderr=subprocess.STDOUT)


def get(path, method="GET", body=None):
    req = urllib.request.Request(base + path, data=body, method=method)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def js(path, method="GET", body=None):
    if method == "POST" and body is None:
        body = b""
    return json.loads(get(path, method, body))


def items(cfg):
    return sorted(l.split()[0] + " " + l.split()[1] for l in cfg.splitlines()
                  if l.startswith(("cell ", "send ")))


try:
    for _ in range(150):
        try:
            get("/api/desk")
            break
        except OSError:
            time.sleep(0.1)

    # The page posts the layout it holds before acting; so does this.
    js("/api/dash/cfg", "POST", layout.encode())
    before = items(get("/api/dash/cfg").decode())
    ck("the layout has 4 cells and 2 sendable values", len(before) == 6,
       " ".join(before))

    w = js("/api/websurvival")
    ck("the full maps are not guaranteed", w["guaranteed"] == 0,
       "block %d" % w["block"])
    trims = [r["action"] for r in w["remedies"] if "/trim" in r["action"]]
    ck("a trim is offered", bool(trims), " | ".join(r["text"] for r in w["remedies"]))

    for action in trims:
        w = js("/api/websurvival")
        if w["guaranteed"]:
            break
        r = js(action, "POST")
        ck("trim %s" % action, r.get("ok") == 1 and r.get("lost") == 0, json.dumps(r))
    w = js("/api/websurvival")
    ck("guaranteed after trimming", w["guaranteed"] == 1, "block %d" % w["block"])

    after = items(get("/api/dash/cfg").decode())
    ck("no cell or sendable value was lost", after == before,
       "%d -> %d" % (len(before), len(after)))

    for p in ("?preview=1", "?preview=0"):
        js("/api/desk" + p, "POST")
    ck("Preview on and off leaves the layout alone",
       items(get("/api/dash/cfg").decode()) == before)

    nm, files = make_bundle.unpack(get("/api/bundle"))
    cfg_out = files["dash.cfg"]
    ck("the exported layout still has all of them", items(cfg_out) == before)
    for bus, label in ((1, "frames.dbc"), (2, "frames2.dbc")):
        text = files.get(label, "")
        have = set(re.findall(r"^BO_ \d+ (\w+)\s*:", text, re.M))
        sigs = set(re.findall(r"^ SG_ (\w+)", text, re.M))
        for line in cfg_out.splitlines():
            if not line.startswith(("cell ", "send ")):
                continue
            if make_bundle.line_bus(line) != bus:
                continue
            m = re.search(r'sig="?(\w+)\.(\w+)', line)
            ck("%s resolves on CAN%d" % (line.split()[0] + " " + line.split()[1], bus),
               m and m.group(1) in have and m.group(2) in sigs,
               m.group(0) if m else line)
        orig = map1 if bus == 1 else map2
        n_before = len(re.findall(r"^BO_ ", orig, re.M))
        ck("CAN%d kept %d of %d messages" % (bus, len(have), n_before),
           0 < len(have) <= n_before)
    sel = re.search(r"^BO_ 130 \w+:.*?(?=^BO_|\Z)", files["frames2.dbc"], re.M | re.S)
    ck("the multiplexed setpoint's message kept its selector",
       bool(sel) and " M :" in sel.group(0))
finally:
    proc.terminate()
    proc.wait(timeout=10)
    if fails:
        print((tmp / "server.log").read_text()[-3000:])

print("\n%s (%d failure%s)" % ("FAILED" if fails else "ALL PASSED", fails,
                               "" if fails == 1 else "s"))
sys.exit(1 if fails else 0)
