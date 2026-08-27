#!/usr/bin/env python3
"""
Put the firmware on a board. macOS, Linux and Windows, one command.

    python3 tools/flash.py                     build, find the board, flash
    python3 tools/flash.py --port COM5         when auto-detect picks wrong
    python3 tools/flash.py --image merged.bin  flash a release image, no build
    python3 tools/flash.py --erase             wipe the flash first
    python3 tools/flash.py --merge out.bin     just build the single-file image

Needs esptool, which is pip-installable everywhere:  pip install esptool
Building from source additionally needs PlatformIO; flashing a release image
does not.

WHY A SINGLE IMAGE
------------------
An ESP32 build is four pieces at four offsets, and getting one offset wrong
produces a board that boots into nothing with no clue why. `--merge` folds them
into one file written at 0x0, which is what the release ships, so there is one
number to get right instead of four and it is zero.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "platformio", ".pio", "build", "esp32dev")

# offset, filename - the layout every ESP32 Arduino build uses
PARTS = [
    ("0x1000",  "bootloader.bin"),
    ("0x8000",  "partitions.bin"),
    ("0xe000",  "boot_app0.bin"),
    ("0x10000", "firmware.bin"),
]
CHIP = "esp32"
FLASH = ["--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"]


def esptool_cmd():
    """esptool as a module if it is importable, else the PlatformIO copy."""
    try:
        import esptool                                   # noqa: F401
        return [sys.executable, "-m", "esptool"]
    except ImportError:
        pass
    p = os.path.expanduser(
        "~/.platformio/packages/tool-esptoolpy/esptool.py")
    if os.path.isfile(p):
        return [sys.executable, p]
    exe = shutil.which("esptool.py") or shutil.which("esptool")
    if exe:
        return [exe]
    sys.exit("esptool not found.  pip install esptool")


def boot_app0():
    """Shipped with the Arduino core rather than built, so it has to be found."""
    for pat in (
        os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32"
                           "/tools/partitions/boot_app0.bin"),
        os.path.join(BUILD, "boot_app0.bin"),
    ):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    return None


def build():
    if not shutil.which("pio") and not shutil.which("platformio"):
        sys.exit("PlatformIO is not installed - flash a release image with "
                 "--image instead, or see README section 4")
    pio = shutil.which("pio") or shutil.which("platformio")
    print("building...")
    r = subprocess.run([pio, "run", "-d", os.path.join(ROOT, "platformio"),
                        "-e", "esp32dev"])
    if r.returncode:
        sys.exit("the build failed - nothing was flashed")


def gather():
    """(offset, path) for each piece, or exit saying which one is missing."""
    out = []
    for off, name in PARTS:
        path = os.path.join(BUILD, name)
        if name == "boot_app0.bin" and not os.path.isfile(path):
            found = boot_app0()
            if not found:
                sys.exit("cannot find boot_app0.bin - build once with "
                         "PlatformIO so the Arduino core is installed")
            path = found
        if not os.path.isfile(path):
            sys.exit("missing %s - build first" % path)
        out.append((off, path))
    return out


def merge(dest):
    parts = gather()
    cmd = esptool_cmd() + ["--chip", CHIP, "merge_bin", "-o", dest] + FLASH
    for off, path in parts:
        cmd += [off, path]
    subprocess.run(cmd, check=True)
    print("wrote %s  (%.2f MB) - flash it at 0x0"
          % (dest, os.path.getsize(dest) / 1e6))


def ports():
    """Serial ports that look like a dev board, best guess first."""
    try:
        from serial.tools import list_ports          # pyserial, ships with pio
    except ImportError:
        return []
    good, other = [], []
    for p in list_ports.comports():
        blurb = "%s %s" % (p.description or "", p.manufacturer or "")
        if any(k in blurb.lower() for k in
               ("cp210", "ch340", "ch910", "silicon labs", "wch", "ftdi",
                "usb serial", "uart")):
            good.append(p.device)
        else:
            other.append(p.device)
    return good + other


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (COM5, /dev/ttyUSB0, "
                                   "/dev/cu.usbserial-...)")
    ap.add_argument("--baud", default="921600")
    ap.add_argument("--image", help="flash this single merged image at 0x0")
    ap.add_argument("--merge", metavar="OUT",
                    help="build the single-file image and stop")
    ap.add_argument("--erase", action="store_true",
                    help="erase the whole flash first - also clears the saved "
                         "dashboard in NVS")
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()

    if a.merge:
        if not a.no_build:
            build()
        merge(a.merge)
        return 0

    if not a.image and not a.no_build:
        build()

    port = a.port
    if not port:
        found = ports()
        if not found:
            sys.exit("no serial port found. Plug the board in, and on Windows "
                     "check the USB-to-serial driver - see WINDOWS.md")
        port = found[0]
        if len(found) > 1:
            print("ports: %s   using %s (override with --port)"
                  % (", ".join(found), port))

    base = esptool_cmd() + ["--chip", CHIP, "--port", port, "--baud", a.baud]

    if a.erase:
        print("erasing...")
        subprocess.run(base + ["erase_flash"], check=True)

    cmd = base + ["write_flash"] + FLASH
    if a.image:
        cmd += ["0x0", a.image]
    else:
        for off, path in gather():
            cmd += [off, path]

    print("flashing %s ..." % port)
    r = subprocess.run(cmd)
    if r.returncode:
        print("\nIf it never entered download mode: hold BOOT, tap EN, release "
              "BOOT, and try again.\nSee 'Getting the chip into download mode' "
              "in WINDOWS.md.", file=sys.stderr)
        return r.returncode

    print("\ndone. Open the serial monitor at 115200 to see the boot banner,\n"
          "then connect to the Wi-Fi network 'CAN-Logger' and open "
          "http://192.168.4.1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
