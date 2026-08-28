#!/usr/bin/env python3
"""
Set the logger's dashboard and sendable values up at a desk, before going out.

    python3 customize.py                  open the page with no frame map, and
                                          load one from it with Frame map
    python3 customize.py path/to/mine.dbc start with this frame map
    python3 customize.py --browse         pick one in your file browser first
    python3 customize.py mine.dbc --role Tester
                                          say which node the logger IS, so Fill
                                          can tell a command from a reading.
                                          Leave it out and nothing is split -
                                          change it any time from the page.
    python customize.py                   (Windows)

Or just double-click this file. On Windows you can also drag a .dbc onto it.

It never asks a question in the terminal. With no argument it opens the page
empty and the Frame map button loads a .dbc from wherever you keep it, which
is the same button the logger itself has.

It opens the logger's real web page in your browser, fed with simulated data,
and writes everything you build into a .cfg file next to your .dbc. Copy the
two files onto the SD card as /frames.dbc and /dash.cfg and the logger starts
up with your dashboard already on it.

No hardware, no wiring, no CAN traffic, and nothing to install - just Python,
which is why this is a .py and not a shell script: it runs the same way on
Windows, macOS and Linux.
"""

import os
import socket
import sys
import threading
import time
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))


def free_port(preferred=8080):
    """The usual port if it is free, otherwise whatever the OS hands out."""
    for port in (preferred, 0):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", port))
            got = s.getsockname()[1]
            s.close()
            return got
        except OSError:
            s.close()
    return preferred



def browse():
    """The operating system's own file picker.

    tkinter ships with Python on Windows and macOS and is one package away on
    Linux, so this is a "usually" rather than an "always". When it is not
    there the page simply opens with no frame map and its own Frame map button
    loads one - which is a better dead end than a prompt in a terminal.
    """
    try:
        import tkinter
        from tkinter import filedialog
    except ImportError:
        print("\n(no file dialog available - tkinter is not installed;")
        print(" on Debian or Ubuntu: sudo apt install python3-tk)")
        return None

    try:
        root = tkinter.Tk()
        root.withdraw()
        root.update()
        picked = filedialog.askopenfilename(
            title="Choose the .dbc for this bus",
            filetypes=[("CAN frame maps", "*.dbc"), ("All files", "*.*")],
            initialdir=str(Path.cwd()))
        root.destroy()
    except Exception as exc:                      # no display, no X server, ...
        print("\n(could not open a file dialog: %s)" % exc)
        return None

    return Path(picked) if picked else None


def main():
    print(__doc__.strip().splitlines()[0])
    print()

    # Which node of the frame map the logger IS. Given here it applies from the
    # first press of Fill; left out, nothing is separated and both Fill buttons
    # offer every message - which is what you want when the logger is only
    # listening. Either way the page's Role button shows the answer and changes
    # it, so nothing is decided permanently on the command line.
    role = ""
    argv = list(sys.argv[1:])
    if "--role" in argv:
        i = argv.index("--role")
        if i + 1 >= len(argv):
            print("--role needs the name of a node from the .dbc")
            return 2
        role = argv[i + 1].strip('"')
        del argv[i:i + 2]

    args = [a for a in argv if a not in ("--browse", "-b")]
    # No question, ever. A frame map given here is used; otherwise the page
    # opens empty and its own Frame map button loads one - which is the same
    # button the logger has, so there is one way to do it rather than two.
    dbc = None
    if args and Path(args[0].strip('"')).suffix.lower() == ".dbc":
        dbc = Path(args[0].strip('"'))            # or dragged onto the icon
    elif argv and not args:
        dbc = browse()                            # --browse, straight to it

    if dbc is not None and not dbc.is_file():
        print("\nNo such file: %s" % dbc)
        return 2

    if dbc is not None:
        # Check it against the logger's limits first. A frame map the ESP32
        # cannot hold is better found now than after a drive to the machine.
        from check_dbc import check
        print()
        rc = check(str(dbc))
        if rc != 0:
            print("\nCarrying on anyway - you can still lay out whatever it did "
                  "read.\n")

    # The setup lands next to the frame map, named after it, so the pair stays
    # together and it is obvious which .cfg belongs to which .dbc. With no
    # frame map yet there is nothing to name it after, so it goes beside this
    # script until one is loaded.
    cfg = dbc.with_suffix(".cfg") if dbc is not None else ROOT / "desk-setup.cfg"
    port = free_port()
    url = "http://127.0.0.1:%d/" % port

    print("\n" + "=" * 68)
    print("  frame map   %s"
          % (dbc if dbc is not None
             else "none yet - load one with Frame map, top right"))
    print("  your setup  %s   (written as you go)" % cfg)
    print("  open        %s" % url)
    print("  role        %s"
          % (role if role else "not set - both Fill buttons offer everything "
                               "(change it in the page, top right)"))
    print("=" * 68)

    steps = [] if dbc is not None else ["Frame map (top right) -> load your .dbc"]
    steps += ["Role (top right)     -> which of these is this logger, or skip",
              "Dashboard -> Customize dashboard -> Fill from frame map",
              "Send -> Set up sendable values -> Fill from the frame map"]
    print()
    for n, line in enumerate(steps, 1):
        print("  %d. %s" % (n, line))
    print("""
  Then copy onto the SD card:
      your .dbc   ->  /frames.dbc
      %s   ->  /dash.cfg

  Close this window (or press Ctrl-C) when you are done.
""" % cfg.name)

    threading.Thread(target=lambda: (time.sleep(1.2), webbrowser.open(url)),
                     daemon=True).start()

    import preview_dashboard
    sys.argv = ["preview_dashboard.py", "--cfg", str(cfg), "--port", str(port)]
    sys.argv += ["--dbc", str(dbc)] if dbc is not None else ["--no-dbc"]
    if role:
        sys.argv += ["--role", role]
    try:
        return preview_dashboard.main()
    except KeyboardInterrupt:
        print("\n\nStopped. Your setup is in %s" % cfg)
        return 0


if __name__ == "__main__":
    code = main()
    # Double-clicked on Windows the console vanishes the instant this returns,
    # taking the instructions with it.
    if os.name == "nt" and sys.stdin and sys.stdin.isatty():
        input("\npress Enter to close ")
    sys.exit(code)
