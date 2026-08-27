#!/usr/bin/env python3
"""
Record the animated walkthroughs in the README.

    python3 tools/capture_gifs.py            # both
    python3 tools/capture_gifs.py customise  # just one

Drives the real page in headless Chrome over the DevTools protocol, with real
mouse events - the drag in the dashboard GIF is an actual pointer drag through
the same handlers a finger goes through, not a scripted state change. Frames go
to ffmpeg, which builds a palette from the whole clip so the dark theme does not
band.

Needs Chrome or Chromium, ffmpeg, and the `websocket-client` package
(`pip install websocket-client`). The rest of tools/ is standard library only;
these two recordings are the exception, along with the plotter.
"""

import base64
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

import websocket                                    # pip install websocket-client

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "img")
PORT = 8097
CDP = 9234
FPS = 12


def find_chrome():
    for c in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        p = shutil.which(c)
        if p:
            return p
    sys.exit("no Chrome/Chromium on PATH")


def wait_up(url, timeout=25):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return urllib.request.urlopen(url, timeout=1).read()
        except Exception:
            time.sleep(0.25)
    return None


class Page:
    """The few DevTools calls this needs, and nothing else."""

    def __init__(self, ws_url):
        self.ws = websocket.create_connection(ws_url, timeout=20)
        self.n = 0

    def call(self, method, **params):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": method,
                                 "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == self.n:
                if "error" in msg:
                    raise RuntimeError("%s: %s" % (method, msg["error"]))
                return msg.get("result", {})

    def js(self, src):
        r = self.call("Runtime.evaluate", expression=src, awaitPromise=True,
                      returnByValue=True)
        return r.get("result", {}).get("value")

    def shot(self, path):
        r = self.call("Page.captureScreenshot", format="png")
        with open(path, "wb") as f:
            f.write(base64.b64decode(r["data"]))

    def move(self, x, y):
        self.call("Input.dispatchMouseEvent", type="mouseMoved", x=x, y=y,
                  button="left", buttons=1)

    def down(self, x, y):
        self.call("Input.dispatchMouseEvent", type="mousePressed", x=x, y=y,
                  button="left", buttons=1, clickCount=1)

    def up(self, x, y):
        self.call("Input.dispatchMouseEvent", type="mouseReleased", x=x, y=y,
                  button="left", buttons=0, clickCount=1)

    def click(self, sel):
        """Clicks the centre of an element, with real events."""
        box = self.js("(function(){var e=document.querySelector(%s);"
                      "if(!e)return null;var r=e.getBoundingClientRect();"
                      "return [r.left+r.width/2, r.top+r.height/2];})()"
                      % json.dumps(sel))
        if not box:
            raise RuntimeError("no element for %s" % sel)
        x, y = box
        self.down(x, y)
        self.up(x, y)


class Recorder:
    """Captures at a steady rate into a temp directory."""

    def __init__(self, page, folder):
        self.page = page
        self.folder = folder
        self.i = 0

    def hold(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            t0 = time.time()
            self.page.shot(os.path.join(self.folder, "f%05d.png" % self.i))
            self.i += 1
            rest = (1.0 / FPS) - (time.time() - t0)
            if rest > 0:
                time.sleep(rest)

    def caption(self, text, seconds=1.5):
        """A line of text over the page, so a silent loop still explains itself."""
        self.page.js("""
          (function(t){
            var b = document.getElementById('__cap');
            if(!b){
              b = document.createElement('div');
              b.id = '__cap';
              b.style.cssText = 'position:fixed;left:50%%;top:14px;'
                + 'transform:translateX(-50%%);z-index:9999;padding:11px 20px;'
                + 'border-radius:999px;background:rgba(10,14,20,.93);'
                + 'border:1px solid #2a3444;color:#e7edf5;font:600 15px '
                + 'system-ui,sans-serif;letter-spacing:.01em;'
                + 'box-shadow:0 8px 30px rgba(0,0,0,.55);white-space:nowrap';
              document.body.appendChild(b);
            }
            b.textContent = t;
            b.style.opacity = t ? '1' : '0';
            /* Toasts and captions both live at the top; the caption is the
               narration, so the toasts get out of its way. */
            var ts = document.getElementById('toasts');
            if(ts) ts.innerHTML = '';
          })(%s);""" % json.dumps(text))
        if seconds:
            self.hold(seconds)


def drag(page, rec, from_sel, to_sel, steps=18):
    """A real pointer drag, so the GIF shows the handler working."""
    box = page.js("""(function(){
        var a=document.querySelector(%s), b=document.querySelector(%s);
        if(!a||!b) return null;
        var ra=a.getBoundingClientRect(), rb=b.getBoundingClientRect();
        return [ra.left+ra.width/2, ra.top+ra.height/2,
                rb.left+rb.width/2, rb.top+rb.height/2];})()"""
                 % (json.dumps(from_sel), json.dumps(to_sel)))
    if not box:
        raise RuntimeError("cannot drag %s -> %s" % (from_sel, to_sel))
    x0, y0, x1, y1 = box
    page.down(x0, y0)
    for i in range(1, steps + 1):
        k = i / float(steps)
        # ease-in-out, so it looks like a hand and not a teleport
        k = k * k * (3 - 2 * k)
        page.move(x0 + (x1 - x0) * k, y0 + (y1 - y0) * k)
        rec.hold(1.0 / FPS)
    page.up(x1, y1)
    rec.hold(0.5)


# ---------------------------------------------------------------------------
#  The two walkthroughs
# ---------------------------------------------------------------------------
def scene_customise(page, rec):
    rec.caption("A logger with nothing set up yet", 1.6)
    page.js("showTab('dash')")
    rec.hold(0.6)

    rec.caption("Customise dashboard", 1.0)
    page.click("#customise")
    rec.hold(0.8)

    rec.caption("Fill from frame map - every signal the .dbc describes", 1.4)
    page.click("#fillmap")
    rec.hold(2.4)                      # let the values arrive before moving on

    rec.caption("Drag a cell to move it", 1.0)
    drag(page, rec, '.cell.slot[data-slot="4"]', '.cell.slot[data-slot="1"]')

    rec.caption("Tap a cell to change how it is drawn", 1.2)
    page.click('.cell.slot[data-slot="1"] [data-act=edit]')
    rec.hold(1.6)

    # Bring the style picker into view, so changing it is visible rather than
    # something that happens off the bottom of the frame.
    page.js("q('sheet').querySelector('.sheetbox').scrollTop=330")
    rec.caption("Ten ways to draw a value", 1.4)
    for w in ("gauge", "arc", "bar", "level", "angle"):
        page.js("edCfg.w=%s;edPicked=true;renderWidgetPicker();"
                "applyFieldVisibility();refreshPreview();" % json.dumps(w))
        rec.hold(0.9)

    rec.caption("The preview is the real widget, fed the real value", 2.0)
    page.click("#e_ok")
    rec.hold(2.0)

    rec.caption("Done - it is on the SD card as /dash.cfg", 1.4)
    page.click("#donedit")
    rec.hold(2.6)
    rec.caption("", 0)
    rec.hold(1.2)


def scene_send(page, rec):
    page.js("showTab('send')")
    rec.hold(0.8)
    rec.caption("Values you can write back to the bus", 1.6)

    rec.caption("Set up sendable values", 1.2)
    page.click("#editsend")
    rec.hold(1.4)

    rec.caption("Fill from the frame map", 2.0)
    page.js("q('txfillmsg').value='all';")
    rec.hold(0.8)
    page.click("#txfill")
    rec.hold(2.2)

    rec.caption("Its selector is not on the list - the logger writes it", 2.4)
    page.js("q('txsheet').querySelector('.sheetbox').scrollTop=420")
    rec.hold(1.8)

    page.js("var b=q('txsheet').querySelector('.sheetbox');"
            "b.scrollTop=b.scrollHeight;")
    rec.hold(0.8)
    page.click("#tx_ok")
    rec.hold(1.4)

    rec.caption("Every Send button is dead until you arm", 1.6)
    page.click("#armbtn")
    rec.hold(1.8)

    rec.caption("Sent with Command = 32, because the frame map says so", 2.6)
    page.js("var r=q('sendlist').querySelectorAll('.sendrow');"
            "if(r.length>1){var b=r[1].querySelector('button[data-role=send]');"
            "if(b) b.click();}")
    rec.hold(2.4)
    rec.caption("", 0)
    rec.hold(1.0)


SCENES = {
    # name, tab, frame map, layout, window, scene
    "customise": ("dash", "machine", None,      (1180, 880), scene_customise),
    "sending":   ("send", "example", None,      (1180, 800), scene_send),
}


def record(name, chrome):
    tab, dbc, layout, (w, h), scene = SCENES[name]

    cmd = [sys.executable, os.path.join(ROOT, "tools", "preview_dashboard.py"),
           "--port", str(PORT), "--dbc",
           os.path.join(ROOT, "examples", dbc + ".dbc")]
    if layout:
        tmp = os.path.join(tempfile.gettempdir(), "gif-%s.cfg" % name)
        shutil.copyfile(os.path.join(ROOT, "examples", layout), tmp)
        cmd += ["--cfg", tmp]
    else:
        cmd += ["--empty"]

    srv = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    profile = tempfile.mkdtemp(prefix="gifprof-")
    frames = tempfile.mkdtemp(prefix="gifframes-")
    br = None
    try:
        if not wait_up("http://127.0.0.1:%d/api/status" % PORT):
            sys.exit("preview did not start")

        br = subprocess.Popen([
            chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
            "--force-device-scale-factor=1", "--no-first-run",
            "--user-data-dir=" + profile,
            "--remote-debugging-port=%d" % CDP,
            "--remote-allow-origins=*",
            "--window-size=%d,%d" % (w, h),
            "http://127.0.0.1:%d/#%s" % (PORT, tab),
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        info = wait_up("http://127.0.0.1:%d/json" % CDP)
        if not info:
            sys.exit("Chrome did not open a debugging port")
        targets = [t for t in json.loads(info) if t.get("type") == "page"]
        if not targets:
            sys.exit("Chrome has no page target")

        page = Page(targets[0]["webSocketDebuggerUrl"])
        page.call("Page.enable")
        page.call("Runtime.enable")
        time.sleep(2.5)                      # let it poll once and settle

        rec = Recorder(page, frames)
        scene(page, rec)

        out = os.path.join(OUT, name + ".gif")
        pal = os.path.join(frames, "pal.png")
        common = ["-y", "-loglevel", "error", "-framerate", str(FPS),
                  "-i", os.path.join(frames, "f%05d.png")]
        # One palette for the whole clip: a per-frame palette makes the dark
        # background crawl, which is exactly what people mean by "cheap gif".
        subprocess.run(["ffmpeg"] + common +
                       ["-vf", "scale=880:-1:flags=lanczos,palettegen="
                               "stats_mode=diff:max_colors=200", pal],
                       check=True)
        subprocess.run(["ffmpeg"] + common + ["-i", pal, "-lavfi",
                        "scale=880:-1:flags=lanczos[x];[x][1:v]paletteuse="
                        "dither=bayer:bayer_scale=4:diff_mode=rectangle",
                        out], check=True)
        print("  wrote %s  (%d frames, %.1f MB)"
              % (os.path.relpath(out, ROOT), rec.i,
                 os.path.getsize(out) / 1e6))
    finally:
        for p in (br, srv):
            if p:
                p.send_signal(signal.SIGINT)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
        shutil.rmtree(profile, ignore_errors=True)
        shutil.rmtree(frames, ignore_errors=True)
        time.sleep(0.5)


def main():
    chrome = find_chrome()
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is not on PATH")
    os.makedirs(OUT, exist_ok=True)

    names = sys.argv[1:] or list(SCENES)
    for n in names:
        if n not in SCENES:
            sys.exit("no such recording: %s (have %s)"
                     % (n, ", ".join(SCENES)))
        record(n, chrome)
    return 0


if __name__ == "__main__":
    sys.exit(main())
