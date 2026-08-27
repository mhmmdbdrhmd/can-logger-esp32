#!/usr/bin/env python3
"""
Set the logger's dashboard and sendable values up at a desk, before going out.

    python3 customise.py                  pick a .dbc it finds, ask, or browse
    python3 customise.py path/to/mine.dbc use this frame map
    python3 customise.py --browse         go straight to the file dialog
    python customise.py                   (Windows)

Or just double-click this file. On Windows you can also drag a .dbc onto it.

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


def find_dbcs():
    """Frame maps worth offering, nearest first, without duplicates."""
    seen, out = set(), []
    for folder in (Path.cwd(), ROOT, ROOT / "examples"):
        try:
            for f in sorted(folder.glob("*.dbc")):
                r = f.resolve()
                if r not in seen:
                    seen.add(r)
                    out.append(f)
        except OSError:
            pass
    return out


def browse():
    """The operating system's own file picker.

    tkinter ships with Python on Windows and macOS and is one package away on
    Linux, so this is a "usually" rather than an "always" - hence the caller
    always keeps a way through that is only typing.
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


def choose_dbc():
    found = find_dbcs()

    if not found:
        print("No .dbc file next to this script or in the current folder.\n")
        print("  b. browse for it")
        print("  or type the full path")
        typed = input("\nchoice [b]: ").strip().strip('"')
        if typed in ("", "b", "B"):
            return browse()
        return Path(typed)

    print("Frame maps I can see:\n")
    for i, f in enumerate(found, 1):
        where = "" if f.parent == Path.cwd() else "   (%s)" % f.parent.name
        print("  %d. %s%s" % (i, f.name, where))
    print("\n  b. browse for another one...")
    print()
    pick = input("which one? [1] ").strip()

    if pick in ("b", "B"):
        picked = browse()
        if picked:
            return picked
        # No dialog on this machine. Do not dead-end - ask for the path.
        typed = input("\npath to your .dbc (blank for %s): "
                      % found[0].name).strip().strip('"')
        return Path(typed) if typed else found[0]
    if not pick:
        return found[0]
    if pick.isdigit() and 1 <= int(pick) <= len(found):
        return found[int(pick) - 1]
    return Path(pick.strip('"'))


def main():
    print(__doc__.strip().splitlines()[0])
    print()

    args = [a for a in sys.argv[1:] if a not in ("--browse", "-b")]
    if args and Path(args[0].strip('"')).suffix.lower() == ".dbc":
        dbc = Path(args[0].strip('"'))            # or dragged onto the icon
    elif len(sys.argv) > 1 and not args:
        dbc = browse()                            # --browse, straight to it
    else:
        dbc = choose_dbc()

    if dbc is None:
        print("\nNothing chosen.")
        return 0
    if not dbc.is_file():
        print("\nNo such file: %s" % dbc)
        input("\npress Enter to close ")
        return 2

    # Check it against the logger's limits first. A frame map the ESP32 cannot
    # hold is better found now than after a drive to the machine.
    from check_dbc import check
    print()
    rc = check(str(dbc))
    if rc != 0:
        print("\nCarrying on anyway - you can still lay out whatever it did "
              "read.\n")

    # The setup lands next to the frame map, named after it, so the pair stays
    # together and it is obvious which .cfg belongs to which .dbc.
    cfg = dbc.with_suffix(".cfg")
    port = free_port()
    url = "http://127.0.0.1:%d/" % port

    print("\n" + "=" * 68)
    print("  frame map   %s" % dbc)
    print("  your setup  %s   (written as you go)" % cfg)
    print("  open        %s" % url)
    print("=" * 68)
    print("""
  1. Dashboard -> Customise dashboard -> Fill from frame map
  2. Send -> Set up sendable values -> Fill from the frame map
  3. Setup file -> say which node this logger stands in for

  Then copy onto the SD card:
      %s   ->  /frames.dbc
      %s   ->  /dash.cfg

  Close this window (or press Ctrl-C) when you are done.
""" % (dbc.name, cfg.name))

    threading.Thread(target=lambda: (time.sleep(1.2), webbrowser.open(url)),
                     daemon=True).start()

    import preview_dashboard
    sys.argv = ["preview_dashboard.py", "--dbc", str(dbc),
                "--cfg", str(cfg), "--port", str(port)]
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
