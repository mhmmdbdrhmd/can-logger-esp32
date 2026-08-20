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
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "img")
PORT = 8099

SHOTS = [
    # name              dbc?   window size      description
    ("dashboard-dbc",   True,  (1400, 1810)),
    ("dashboard-raw",   False, (1400, 1350)),
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

    for name, with_dbc, (w, h) in SHOTS:
        cmd = [sys.executable, os.path.join(ROOT, "tools", "preview_dashboard.py"),
               "--port", str(PORT)]
        if with_dbc:
            cmd += ["--dbc", os.path.join(ROOT, "examples", "example.dbc")]

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
                "--virtual-time-budget=4000",
                f"http://127.0.0.1:{PORT}/",
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
