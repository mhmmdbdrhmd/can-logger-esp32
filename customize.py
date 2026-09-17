#!/usr/bin/env python3
"""
Set the logger up at a desk, before going out: dashboard, sendable values,
both frame maps and the name length - and see it the way the logger will.

    python3 customize.py                  open the page with no frame map, and
                                          load one from it with Frame map: CAN 1
    python3 customize.py path/to/mine.dbc start with this frame map
    python3 customize.py can1.dbc can2.dbc
                                          one frame map per bus
    python3 customize.py logger.bundle    carry on from an exported setup
    python3 customize.py --browse         pick one in your file browser first
    python3 customize.py mine.dbc --role Tester
                                          say which node the logger IS, so Fill
                                          can tell a command from a reading.
                                          Leave it out and nothing is split -
                                          change it any time from the page.
    python3 customize.py mine.dbc --name-max 32
                                          start with names kept to 31 characters
    python customize.py                   (Windows)

In the page:

  Web UI (header)  whether the logger will still serve this page while it
                   records, with these maps. If not, it offers the way out:
                   trim frames.dbc or frames2.dbc (messages the layout uses are
                   always kept), or a shorter name length (16, 32 or 64 -
                   longer names are abbreviated, not cut).
  Preview          the page as the logger will show it: the maps as the
                   exported bundle carries them, read at that name length,
                   cut down if the logger could not hold them whole. Press it
                   again to go back to designing.
  Export           ONE file, logger.bundle: both frame maps, the layout and
                   the name length. Put it on the SD card as /logger.bundle, or
                   Import it from the logger's own dashboard; the logger
                   unpacks it at its next start.

Or just double-click this file. On Windows you can also drag a .dbc onto it.

It never asks a question in the terminal. With no argument it opens the page
empty and the Frame map: CAN 1 button loads a .dbc from wherever you keep it,
which is the same button the logger itself has. One map per bus, as on the
logger: the CAN 2 button loads CAN 2's, and neither disturbs the other.

It opens the logger's real web page in your browser, fed with simulated data.
Press Export in the page to save the setup you have built as logger.bundle,
and put that one file on the SD card - the logger starts up with your frame
maps and your dashboard already on it.

IT WRITES NO FILES AT ALL. This tool used to pair a .cfg with every .dbc it was
shown, which left files in whatever directory you had pointed it at and opened
the page on a setup you had not asked for. Now a frame map you merely looked at
leaves nothing behind, and Export is the one way a setup comes out.

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

    name_max = None
    if "--name-max" in argv:
        i = argv.index("--name-max")
        try:
            name_max = int(argv[i + 1])
        except (IndexError, ValueError):
            name_max = 0
        if name_max not in (16, 32, 64):
            print("--name-max is 16, 32 or 64")
            return 2
        del argv[i:i + 2]

    args = [a for a in argv if a not in ("--browse", "-b")]
    # No question, ever. Files given here are used; otherwise the page opens
    # empty and its own Frame map button loads one - which is the same button
    # the logger has, so there is one way to do it rather than two.
    files = [Path(a.strip('"')) for a in args]
    dbc = dbc2 = bundle = None
    dbcs = [f for f in files if f.suffix.lower() == ".dbc"]
    bundles = [f for f in files if f.suffix.lower() == ".bundle"]
    if bundles:
        bundle = bundles[0]
    elif dbcs:
        dbc = dbcs[0]                             # or dragged onto the icon
        dbc2 = dbcs[1] if len(dbcs) > 1 else None
    elif argv and not args:
        dbc = browse()                            # --browse, straight to it

    for f in (dbc, dbc2, bundle):
        if f is not None and not f.is_file():
            print("\nNo such file: %s" % f)
            return 2

    for f in (dbc, dbc2):
        if f is None:
            continue
        # Check it against the logger's limits first. A frame map the ESP32
        # cannot hold is better found now than after a drive to the machine.
        from check_dbc import check
        print()
        rc = check(str(f))
        if rc != 0:
            print("\nCarrying on anyway - you can still lay out whatever it did "
                  "read.\n")

    # A setup file BESIDE the frame map is read if it is already there, and is
    # never created. Two bugs came out of pairing them automatically: a setup
    # built for one bus was read back the next day beside no frame map at all,
    # and the page opened on eighteen cells that all read "unknown"; and a .dbc
    # opened out of curiosity acquired a .cfg next to it that nobody wanted.
    # Reading an existing one is useful, writing one uninvited is not.
    cfg = dbc.with_suffix(".cfg") if dbc is not None else None
    have_cfg = cfg is not None and cfg.exists()
    port = free_port()
    url = "http://127.0.0.1:%d/" % port

    print("\n" + "=" * 68)
    if bundle is not None:
        print("  setup       %s" % bundle)
    else:
        print("  CAN1 map    %s"
              % (dbc if dbc is not None
                 else "none yet - load one with Frame map, top right"))
        print("  CAN2 map    %s" % (dbc2 if dbc2 is not None else "none"))
        print("  your setup  %s"
              % ("%s   (read from there; Export to save your changes)" % cfg
                 if have_cfg else "starts empty - Export in the page when you "
                                  "want to keep it"))
    print("  open        %s" % url)
    print("  role        %s"
          % (role if role else "not set - both Fill buttons offer everything "
                               "(change it in the page, top right)"))
    print("=" * 68)

    steps = [] if (dbc or bundle) else ["Frame map (top right) -> load your .dbc"]
    steps += ["Role (top right)     -> which of these is this logger, or skip",
              "Dashboard -> Customize dashboard -> Fill from frame map",
              "Send -> Set up sendable values -> Fill from the frame map",
              "Web UI (top right)   -> must say OK; if not, take a fix it offers",
              "Preview (top right)  -> check it the way the logger will show it"]
    print()
    for n, line in enumerate(steps, 1):
        print("  %d. %s" % (n, line))
    print("""
  Then press Export. It saves ONE file, logger.bundle - both frame maps, the
  layout and the name length. Copy it onto the SD card as /logger.bundle, or
  Import it from the logger's dashboard; the logger unpacks it when it starts.

  Two buses, two frame maps: the same identifier usually means different things
  on each, so they are separate files. A cell or a sendable value remembers
  which bus it belongs to, and only that bus's map is offered when you pick its
  signal. Either map may be left off - that bus is then recorded as raw bytes.

  Close this window (or press Ctrl-C) when you are done.
""")

    threading.Thread(target=lambda: (time.sleep(1.2), webbrowser.open(url)),
                     daemon=True).start()

    import preview_dashboard
    # --cfg only when there is already a file to read; it is never written back.
    # Export in the page is the one way a setup leaves this tool.
    sys.argv = ["preview_dashboard.py", "--port", str(port)]
    if bundle is not None:
        sys.argv += ["--bundle", str(bundle), "--empty"]
    else:
        sys.argv += ["--cfg", str(cfg)] if have_cfg else ["--empty"]
        sys.argv += ["--dbc", str(dbc)] if dbc is not None else ["--no-dbc"]
        if dbc2 is not None:
            sys.argv += ["--dbc2", str(dbc2)]
    if name_max:
        sys.argv += ["--name-max", str(name_max)]
    if role:
        sys.argv += ["--role", role]
    try:
        return preview_dashboard.main()
    except KeyboardInterrupt:
        print("\n\nStopped. Anything you did not Export is gone - "
              "nothing was written.")
        return 0


if __name__ == "__main__":
    code = main()
    # Double-clicked on Windows the console vanishes the instant this returns,
    # taking the instructions with it.
    if os.name == "nt" and sys.stdin and sys.stdin.isatty():
        input("\npress Enter to close ")
    sys.exit(code)
