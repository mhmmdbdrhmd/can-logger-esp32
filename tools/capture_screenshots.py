#!/usr/bin/env python3
"""
Regenerate the dashboard screenshots used by the README.

Starts the preview server (which extracts the page straight out of
src/webui.cpp, so it cannot drift from what the firmware serves) and drives a
headless Chrome over it.

    python3 tools/capture_screenshots.py

Writes into docs/img/. Re-run after changing the page, or the README will show
a dashboard that no longer exists.

Needs Chrome or Chromium on PATH. The preview's data is synthetic - the point
of these images is the layout, not the numbers.
"""

import os
import signal
import shutil
import subprocess
import tempfile
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "img")
PORT = 8099

# name, tab, layout, frame map, window size, and any JavaScript needed to reach
# the state being photographed. The frame maps and layouts are real files in
# examples/, so a screenshot cannot show a dashboard that could not be built.
SHOTS = [
    ("dashboard-dbc",  "dash", "machine", "machine",  (1400, 1035), ""),
    ("dashboard-raw",  "dash", None,      None, (1400,  660), ""),
    ("bus-view",       "bus",  "machine", "machine",  (1400,  900), ""),
    ("send-values",    "send", "machine", "machine",  (1400,  990),
     "postForm('/api/tx/arm',{on:1}).then(pollDash)"),
    ("editor",         "dash", "machine", "machine",  (1360, 1290),
     "setEdit(true);openEditor(4)"),
    ("customising",    "dash", "machine", "machine",  (1400,  760), "setEdit(true)"),
    ("send-setup",     "send", "machine", "machine",  (1160, 1180), "openTxEdit()"),
    ("setup-file",     "dash", "machine", "machine",  (1160,  860),
     "q('setupbtn').click()"),
    # A multiplexed command frame and a frame whose signals only mean anything
    # together - both against examples/example.dbc, which carries one of each.
    ("send-groups",    "send", None,      "example",  (1240,  980),
     "postForm('/api/tx/arm',{on:1}).then(function(){return openTxEdit();})"
     ".then(function(){TXED={};var h=-1,f=-1;"
     "DBC.m.forEach(function(m,i){if(m.n==='HostCommand')h=i;"
     "if(m.n==='MotorFeedback')f=i;});"
     "q('txfillmsg').value=h;txFillFromMap();"
     "q('txfillmsg').value=f;txFillFromMap();"
     "CFG.tx=TXED;q('txsheet').classList.remove('on');"
     "renderSend();q('toasts').innerHTML='';return pollDash();})"),
    # A phone, scrolled past the permission card - the case the pinned arm bar
    # exists for. Same setup as above, so it is the same page, smaller.
    ("send-pinned",    "send", None,      "example",  (390,   680),
     "postForm('/api/tx/arm',{on:1}).then(function(){return openTxEdit();})"
     ".then(function(){TXED={};var h=-1,f=-1;"
     "DBC.m.forEach(function(m,i){if(m.n==='HostCommand')h=i;"
     "if(m.n==='MotorFeedback')f=i;});"
     "q('txfillmsg').value=h;txFillFromMap();"
     "q('txfillmsg').value=f;txFillFromMap();"
     "CFG.tx=TXED;q('txsheet').classList.remove('on');"
     "renderSend();q('toasts').innerHTML='';return pollDash();})"
     ".then(function(){window.scrollTo(0,600);})"),
]


def find_chrome():
    for c in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        p = shutil.which(c)
        if p:
            return p
    sys.exit("no Chrome/Chromium on PATH - cannot capture screenshots")


def wait_up(url, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        try:
            urllib.request.urlopen(url, timeout=1)
            return True
        except Exception:
            time.sleep(0.2)
    return False


def main():
    chrome = find_chrome()
    os.makedirs(OUT, exist_ok=True)

    for name, tab, layout, with_dbc, (w, h), run in SHOTS:
        cmd = [sys.executable, os.path.join(ROOT, "tools", "preview_dashboard.py"),
               "--port", str(PORT)]
        if with_dbc:
            cmd += ["--dbc", os.path.join(ROOT, "examples", with_dbc + ".dbc")]
        else:
            cmd += ["--no-dbc"]
        if not layout:
            cmd += ["--empty"]
        if run:
            cmd += ["--run", run]
        if layout:
            # A copy, so capturing screenshots never edits what is in the repo.
            src = os.path.join(ROOT, "examples", "dash.cfg")
            tmp = os.path.join(tempfile.gettempdir(), f"shot-{layout}.cfg")
            shutil.copyfile(src, tmp)
            cmd += ["--cfg", tmp]

        srv = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        try:
            if not wait_up(f"http://127.0.0.1:{PORT}/api/status"):
                sys.exit("preview server did not start")

            # Let the page poll once so the cards hold real values rather than
            # the "--" they render with before the first fetch.
            time.sleep(2.5)

            path = os.path.join(OUT, name + ".png")
            subprocess.run([
                chrome, "--headless", "--disable-gpu", "--hide-scrollbars",
                "--force-device-scale-factor=1",
                f"--window-size={w},{h}",
                f"--screenshot={path}",
                "--virtual-time-budget=5000",
                f"http://127.0.0.1:{PORT}/#{tab}",
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f"  wrote {os.path.relpath(path, ROOT)}  ({w}x{h})")
        finally:
            srv.send_signal(signal.SIGINT)
            try:
                srv.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv.kill()
            time.sleep(0.4)


if __name__ == "__main__":
    main()
