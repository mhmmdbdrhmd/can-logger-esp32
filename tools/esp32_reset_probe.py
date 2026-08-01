#!/usr/bin/env python3
"""
Find a DTR/RTS sequence that puts THIS board, on THIS PC, into download mode -
without touching the BOOT button.

Background
----------
esptool's auto-reset drives two modem-control lines through a pair of
transistors on the DevKit:

    RTS -> EN     (reset the chip)
    DTR -> GPIO0  (choose download mode while it comes out of reset)

"Wrong boot mode detected (0x13)" means EN worked but GPIO0 was still high when
the chip came out of reset. On Windows each line change is a separate USB
control transfer, and some CH340 driver builds reorder or coalesce them, so
GPIO0 never settles low in time.

esptool only ever tries one ordering. This script tries several, and after each
one reads the ESP32's own ROM boot log to see what actually happened. Whatever
prints "DOWNLOAD MODE" works on this machine.

Usage
-----
    python esp32_reset_probe.py COM3                 # what works
    python esp32_reset_probe.py COM3 --force         # leave it ready to flash
    python esp32_reset_probe.py COM3 --diagnose      # when nothing works

Run it with PlatformIO's Python, which already has pyserial:

    C:\\Users\\<you>\\.platformio\\penv\\Scripts\\python.exe esp32_reset_probe.py COM3

Nothing here writes to the chip; it only toggles the two control lines.
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit(
        "pyserial is missing. Run this with PlatformIO's Python, e.g.\n"
        r"  C:\Users\<you>\.platformio\penv\Scripts\python.exe " + sys.argv[0] + " COM3"
    )

BAUD = 115200


# ---------------------------------------------------------------------------
#  THE CIRCUIT - read this before adding a strategy.
#
#  The DevKit's auto-reset is a CROSS-COUPLED transistor pair, not two
#  independent switches:
#
#      DTR  RTS |  EN    GPIO0
#      ---------+----------------
#       F    F  |  high  high      idle
#       F    T  |  LOW   high      chip held in reset
#       T    F  |  high  LOW       running, boot pin pulled low
#       T    T  |  high  high      BOTH RELEASED   <-- the trap
#
#  The last row is deliberate: it stops a terminal program that asserts both
#  lines from resetting the board. But it also means there is NO safe
#  intermediate state between "in reset" (F,T) and "running with GPIO0 low"
#  (T,F). That move is diagonal, and every path through it releases EN while
#  GPIO0 is still high. On a working board the only thing that saves you is the
#  capacitor on EN, which slows its rise just enough for the second line change
#  to land first.
#
#  Consequence: inserting a DELAY between the two line changes makes things
#  strictly worse, not better. The `gap` family below does exactly that and is
#  kept only as a diagnostic - if it fails identically to `classic`, the fault
#  is not a timing race.
#
#  What actually helps is changing both lines in ONE driver call, which is what
#  windows_tight does.
#
#  pyserial semantics: `port.dtr = True` asserts DTR, which the transistor
#  network turns into GPIO0 being pulled low. `port.rts = True` pulls EN low.
# ---------------------------------------------------------------------------


def _win_set_lines(p, dtr, rts):
    """
    Set DTR and RTS in a SINGLE driver call, on Windows.

    pyserial's `p.dtr = x` / `p.rts = y` each issue their own
    EscapeCommFunction, so the two lines change in two separate USB control
    transfers with an unpredictable gap between them. That gap is the whole
    problem - see the circuit note above.

    SetCommState writes a whole DCB at once, and fDtrControl / fRtsControl live
    in it, so one call carries both. This is the Windows counterpart of
    esptool's UnixTightReset, which esptool only implements for Unix.
    """
    import ctypes
    import serial.win32 as win32

    handle = p._port_handle
    dcb = win32.DCB()
    if not win32.GetCommState(handle, ctypes.byref(dcb)):
        raise OSError("GetCommState failed")

    dcb.fDtrControl = win32.DTR_CONTROL_ENABLE if dtr else win32.DTR_CONTROL_DISABLE
    dcb.fRtsControl = win32.RTS_CONTROL_ENABLE if rts else win32.RTS_CONTROL_DISABLE

    if not win32.SetCommState(handle, ctypes.byref(dcb)):
        raise OSError("SetCommState failed")

    # Keep pyserial's shadow state honest, or a later p.dtr = ... fights us.
    p._dtr_state = bool(dtr)
    p._rts_state = bool(rts)


def windows_tight(p, hold=0.1, delay=0.05):
    """THE ONE THAT SHOULD WORK on Windows. Both lines move together, so the
    board never passes through the (T,T) state where EN is released while GPIO0
    is still high."""
    if sys.platform != "win32":
        raise OSError("windows_tight only works on Windows")
    _win_set_lines(p, dtr=False, rts=True)    # EN low, GPIO0 high
    time.sleep(hold)
    _win_set_lines(p, dtr=True, rts=False)    # simultaneous: EN high + GPIO0 low
    time.sleep(delay)
    _win_set_lines(p, dtr=False, rts=False)   # release


def windows_tight_slow(p):
    windows_tight(p, hold=0.3, delay=0.3)


def classic(p, hold=0.1, delay=0.05):
    """Exactly what esptool does. The baseline that is failing."""
    p.dtr = False           # GPIO0 high
    p.rts = True            # EN low  -> chip held in reset
    time.sleep(hold)
    p.dtr = True            # GPIO0 low
    p.rts = False           # EN high -> chip boots, should sample GPIO0 low
    time.sleep(delay)
    p.dtr = False           # release GPIO0


def classic_slow(p):
    classic(p, hold=0.5, delay=0.5)


def gap(p, pre=0.2, gap_s=0.1, post=0.5):
    """Never change DTR and RTS in the same breath - kept as a diagnostic."""
    p.dtr = False
    p.rts = True            # EN low
    time.sleep(pre)
    p.dtr = True            # GPIO0 low ...
    time.sleep(gap_s)       # ... and let it settle before anything else moves
    p.rts = False           # EN high, GPIO0 already low
    time.sleep(post)
    p.dtr = False


def gap_very_slow(p):
    gap(p, pre=1.0, gap_s=1.0, post=1.5)


def hold_gpio0(p):
    """Assert GPIO0 first and hold it across the whole reset - the software
    equivalent of holding the BOOT button down the entire time."""
    p.dtr = True            # GPIO0 low, and it stays low
    time.sleep(0.2)
    p.rts = True            # EN low
    time.sleep(0.3)
    p.rts = False           # EN high, GPIO0 still low
    time.sleep(0.8)
    p.dtr = False


def inverted(p):
    """In case DTR and RTS are swapped by this adapter or driver."""
    p.rts = False
    p.dtr = True
    time.sleep(0.2)
    p.rts = True
    time.sleep(0.1)
    p.dtr = False
    time.sleep(0.5)
    p.rts = False


STRATEGIES = {
    "windows-tight": windows_tight,
    "windows-tight-slow": windows_tight_slow,
    "hold-gpio0": hold_gpio0,
    "classic": classic,
    "classic-slow": classic_slow,
    "gap": gap,
    "gap-very-slow": gap_very_slow,
    "inverted": inverted,
}

# Simultaneous line changes first - on the cross-coupled circuit they are the
# only ones that can work. The rest follow as diagnostics.
ORDER = ["windows-tight", "windows-tight-slow", "hold-gpio0", "classic",
         "classic-slow", "gap", "gap-very-slow", "inverted"]


# ---------------------------------------------------------------------------

def read_boot_log(p, seconds=0.6):
    deadline = time.time() + seconds
    buf = b""
    while time.time() < deadline:
        n = p.in_waiting
        if n:
            buf += p.read(n)
        else:
            time.sleep(0.02)
    return buf.decode("utf-8", "replace")


def classify(log):
    """Read the ESP32 ROM's own boot banner to see which mode it entered."""
    if "waiting for download" in log or "DOWNLOAD_BOOT" in log:
        return True, "DOWNLOAD MODE"
    m = re.search(r"boot:(0x[0-9a-fA-F]+)", log)
    if m:
        return False, "normal boot (boot:%s) - GPIO0 was high" % m.group(1)
    if log.strip():
        return False, "output, but no boot banner"
    return False, "silent - chip did not reset at all"


def attempt(port_name, name):
    """Returns (ok, reason, raw_log). Never raises for a line-control failure -
    a driver that refuses to drive DTR/RTS at all is itself the answer we are
    looking for, so it has to be reported rather than crash the probe."""
    fn = STRATEGIES[name]
    with serial.Serial(port_name, BAUD, timeout=0.1) as p:
        try:
            # Opening the port on Windows asserts DTR; settle both lines first.
            p.dtr = False
            p.rts = False
            time.sleep(0.2)
            p.reset_input_buffer()
            fn(p)
        except (OSError, serial.SerialException) as e:
            if "only works on Windows" in str(e):
                return (False, "skipped - Windows only", "")
            return (False, "driver REFUSED to drive DTR/RTS (%s: %s)"
                    % (type(e).__name__, e), "")
        return classify(read_boot_log(p)) + (read_boot_log(p, 0.05),)


def diagnose(port_name):
    """
    Decide whether DTR reaches the board at all - without touching the board.

    On the cross-coupled circuit, EN depends on BOTH lines:

        EN is pulled low   iff  RTS asserted AND DTR deasserted

    So from the "held in reset" state (dtr=F, rts=T), asserting DTR ALONE must
    release EN and let the chip boot - purely as a side effect, regardless of
    what happens on the GPIO0 side. That gives a clean split:

      chip boots  -> DTR physically reached the circuit. The driver is fine,
                     and the fault is the GPIO0 transistor. Hardware.
      stays quiet -> DTR never reached the circuit. The driver is not really
                     asserting it. Replaceable in software.
    """
    print("Isolating the DTR line (no BOOT button needed).\n")

    with serial.Serial(port_name, BAUD, timeout=0.1) as p:
        try:
            p.dtr = False
            p.rts = False
        except (OSError, serial.SerialException) as e:
            print("  the driver refuses line control outright: %s" % e)
            return 1
        time.sleep(0.4)
        p.reset_input_buffer()

        print("  step 1: RTS asserted     -> EN low, chip should be SILENT")
        p.rts = True
        time.sleep(0.6)
        p.reset_input_buffer()
        time.sleep(0.4)
        quiet = read_boot_log(p, 0.5)
        if quiet.strip():
            print("          ...but it is talking: %r" % quiet[:70])
            print("          RTS/EN is not holding the chip in reset either.")
        else:
            print("          silent, as expected - EN is being held low")

        print("  step 2: DTR asserted too -> should release EN if DTR is real")
        p.dtr = True
        log = read_boot_log(p, 1.2)

        p.dtr = False
        p.rts = False

    print()
    if ("boot:" in log) or ("rst:" in log):
        print("  RESULT: the chip BOOTED when DTR was asserted.")
        print("          -> DTR does reach the reset circuit; the driver works.")
        print("          -> GPIO0 still never goes low, so the transistor or")
        print("             resistor on the GPIO0 side of the circuit is dead.")
        print()
        print("          That is a HARDWARE fault on the board. No driver")
        print("          version and no script can work around it - use the")
        print("          manual BOOT+EN sequence, or flash over Wi-Fi (OTA).")
        return 1

    print("  RESULT: asserting DTR changed nothing - the chip stayed in reset.")
    print("          -> the DTR signal never reaches the circuit at all.")
    print("          -> the USB-serial driver is not really asserting DTR,")
    print("             even though the API calls succeed.")
    print()
    print("          This IS fixable in software: install the vendor driver")
    print("          (or an older CH340 build) and run this again.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial port, e.g. COM3 or /dev/ttyUSB0")
    ap.add_argument("--force", action="store_true",
                    help="stop at the first sequence that works and LEAVE the "
                         "chip in download mode, ready to flash")
    ap.add_argument("--strategy", choices=sorted(STRATEGIES), help="use only this one")
    ap.add_argument("--verbose", action="store_true", help="show the raw boot log")
    ap.add_argument("--diagnose", action="store_true",
                    help="when NO strategy works: decide whether the fault is "
                         "the driver or the board's reset transistor")
    args = ap.parse_args()

    if args.diagnose:
        try:
            return diagnose(args.port)
        except serial.SerialException as e:
            sys.exit("could not open %s: %s" % (args.port, e))

    names = [args.strategy] if args.strategy else ORDER

    print("port %s @ %d baud\n" % (args.port, BAUD))
    winners = []

    for name in names:
        try:
            ok, why, log = attempt(args.port, name)
        except serial.SerialException as e:
            sys.exit("could not open %s: %s\n"
                     "Close the serial monitor and any other program using the "
                     "port, then try again." % (args.port, e))

        print("  %-20s %s %s" % (name, "OK  " if ok else "fail", why))

        # "output, but no boot banner" is ambiguous - it can be a garbled banner
        # rather than a genuine failure, so always show the bytes.
        if log.strip() and (args.verbose or "no boot banner" in why):
            for line in log.strip().splitlines()[:6]:
                print("        | " + repr(line))

        if ok:
            winners.append(name)
            if args.force:
                print("\nChip is now in DOWNLOAD MODE and will stay there.")
                print("Flash it now WITHOUT letting the tool reset the board again:")
                print("  esptool --before no_reset --after hard_reset ...")
                print("or, in platformio.ini:")
                print("  board_upload.before_reset = no_reset")
                return 0
        time.sleep(0.3)

    print()
    if winners:
        print("Working sequences: %s" % ", ".join(winners))
        print("Use the first one:")
        print("  python %s %s --force --strategy %s"
              % (sys.argv[0], args.port, winners[0]))
        return 0

    print("No sequence worked. What the column above means:")
    print()
    print("  'REFUSED to drive DTR/RTS'  the driver does not implement line")
    print("                              control at all - conclusive, replace")
    print("                              the driver.")
    print("  'silent'                    the chip never reset; RTS/EN is dead")
    print("                              too. Suspect the cable or the port.")
    print("  'normal boot (boot:0x..)'   EN works, GPIO0 is never pulled low.")
    print()
    print("Run with --diagnose to separate a driver fault from a dead reset")
    print("transistor. Either way the manual BOOT+EN sequence still works, and")
    print("once one build is on the board you can flash over Wi-Fi instead.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
