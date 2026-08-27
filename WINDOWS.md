# Building and flashing on Windows

Two ways to do this. **Neither needs bash or a command line.**

| | PlatformIO in VS Code | Arduino IDE |
|---|---|---|
| Install | one extension, brings its own toolchain | board package via a URL you paste in |
| Board settings | **already in `platformio.ini`** | 6 settings by hand, 2 of them non-default |
| COM port | found automatically | picked by hand |
| Serial monitor | built in, right baud rate | built in, set the baud rate yourself |
| Sketch folder | not needed | must be generated first (`arduino\sync.bat`) |
| Most common failure | none of note | wrong partition scheme → "Sketch too big" |

**PlatformIO is the easier path.** The settings that most often go wrong in the
Arduino IDE are committed to `platformio.ini`, so they cannot be got wrong. It
runs on Windows perfectly well — it is a VS Code extension with buttons, not a
command-line tool.

Use the Arduino IDE instead if you already have it set up, or want the smaller
install.

1. **[Step 0 — USB driver](#step-0--usb-driver)** ← do this first either way
2. Then **[Path A — PlatformIO](#path-a--platformio-in-vs-code)**
   *or* **[Path B — Arduino IDE](#path-b--arduino-ide)**
3. **[Prepare the SD card](#prepare-the-sd-card)**
4. **[Laying out the dashboard](#laying-out-the-dashboard-before-you-go-out)**
   ← needs no board at all
5. **[After it boots](#after-it-boots)**
6. **[Common errors](#common-errors)**

---

# Step 0 — USB driver

**Needed for both paths.** This is the single most common reason the board never
shows up as a COM port. The ESP32 DevKit talks to the PC through a USB-to-serial
chip, and Windows usually has no driver for it.

Look at the small chip next to the USB socket on the board:

| Chip marking | Driver to install |
|---|---|
| **CP2102** / CP2104 | Silicon Labs *CP210x USB to UART Bridge VCP Drivers* |
| **CH340** / CH9102 | WCH *CH341SER* |

Install it, then **unplug and replug** the board.

**Check it worked:** Device Manager → **Ports (COM & LPT)** should list something
like `Silicon Labs CP210x USB to UART Bridge (COM5)`. Note the COM number.

If nothing appears at all, try a different USB cable — a lot of cheap cables are
charge-only and have no data lines.

---

# Path A — PlatformIO in VS Code

## A1. Install VS Code and the extension

1. Install **Visual Studio Code** from <https://code.visualstudio.com>.
2. Open it and click the **Extensions** icon in the left bar (four squares).
3. Search **PlatformIO IDE** and click **Install**.
4. Wait. On first install it downloads its own Python and the ESP32 toolchain —
   several hundred MB. A "PlatformIO: Installing…" notification appears in the
   corner; let it finish and reload the window when asked.

You do **not** need to install Python, git, or anything else yourself.

## A2. Open the project

**File → Open Folder…** and select exactly this folder:

```
can-logger-esp32\platformio
```

> Open the `platformio` folder **itself** — not its parent, not `src`.
> PlatformIO looks for `platformio.ini` in whatever folder you open. The sources
> live in `..\src`, and `platformio.ini` already points at them.

First open takes a minute: it downloads the ESP32 platform and shows
"Configuring project" in the status bar.

## A3. Build

A blue status bar runs along the bottom of the window:

| Button | Does |
|---|---|
| **✓** checkmark | **Build** |
| **→** arrow | **Upload** (builds, then flashes) |
| **🔌** plug | **Serial Monitor**, already at the right baud rate |
| **🗑** bin | Clean, if a build ever goes strange |

Press **✓** first, with nothing plugged in — that separates build problems from
upload problems. It ends with something like:

```
RAM:   [====      ]  35.1% (used 114896 bytes from 327680 bytes)
Flash: [=====     ]  47.1% (used 925849 bytes from 1966080 bytes)
======================== [SUCCESS] Took 34.21 seconds ========================
```

## A4. Upload

Plug the board in and press **→**.

PlatformIO finds the COM port itself. If several serial devices are attached and
it picks the wrong one, add this to `platformio.ini` (use the COM number from
Step 0):

```ini
upload_port  = COM5
monitor_port = COM5
```

If it stalls on `Connecting........_____`, or fails with **"Wrong boot mode
detected"**, see [Getting the chip into download
mode](#getting-the-chip-into-download-mode) below.

Then press **🔌** and jump to [Prepare the SD card](#prepare-the-sd-card).

---

## Getting the chip into download mode

This is the most common hardware-side failure, and it looks like:

```
A fatal error occurred: Failed to connect to ESP32: Wrong boot mode detected (0x13)!
The chip needs to be in download mode.
```

### Read the message carefully — it narrows the fault to one wire

`boot:0x13` is printed by the **ESP32's own ROM**, not by esptool. esptool resets
the board, reads the boot log off the serial line, and matches `boot:(0x..)`. For
that message to exist at all, three things must already work:

| | Verdict |
|---|---|
| **RTS → EN** (reset) | ✅ works — the chip rebooted, which is why there is a fresh boot log |
| **RX data path** | ✅ works — esptool received and parsed that log |
| **DTR → GPIO0** (boot select) | ❌ **failed** — GPIO0 was high, so it booted the app |

So the COM port, the cable and the driver's *data* path are all fine. The fault
is specifically the **DTR** line, which is what tells the chip to enter download
mode.

### Fix 1 — the manual BOOT sequence (try this first)

Most DevKit boards do this automatically over DTR/RTS. When that circuit is
missing, slow, or the USB cable is marginal, you do it by hand:

1. **Press and hold `BOOT`** (sometimes labelled `IO0` or `FLASH`).
2. While still holding it, **press and release `EN`** (sometimes `RST`).
3. **Keep holding `BOOT`** and press Upload (**→**).
4. Release `BOOT` once `Writing at 0x00001000...` appears.

Holding `BOOT` from *before* you press Upload is the reliable order — the window
esptool gives you is short.

The manual sequence is also the **decisive diagnostic**, because the button
grounds GPIO0 directly and bypasses DTR entirely:

* **flashes with `BOOT`, never without** → the DTR auto-reset path is the fault.
  Go to Fix 2. Meanwhile the manual sequence is a perfectly usable workflow —
  nothing is wrong with the board or the firmware.
* **fails even with `BOOT`** → not the driver. Look at power, the cable, or the
  `BOOT` button's contact.

### Fix 2 — the USB driver (prime suspect if `BOOT` works)

A driver that asserts RTS but ignores or mistimes DTR produces exactly the
signature above: the chip resets, but never enters download mode. Worth checking
first if auto-reset used to work and then stopped, or if the board has moved to a
different PC.

**Device Manager → Ports (COM & LPT) → right-click the port → Properties →
Driver tab → *Driver Provider*:**

| Provider | Meaning |
|---|---|
| `Silicon Laboratories` | ✅ correct CP210x VCP driver |
| `wch.cn` | ✅ correct CH340/CH9102 driver |
| **`Microsoft`** | ⚠ Windows fell back to the generic `usbser.sys` |

A device that appears as **"USB Serial Device (COMx)"** with a Microsoft provider
is the generic CDC driver. **CH9102 chips very often enumerate this way**, and
the generic driver is a known cause of DTR/RTS misbehaviour. Install the vendor
driver over it (Step 0), then unplug and replug.

To find out which of the two it is without guessing, run the probe with
PlatformIO's own Python (it already has pyserial):

```
C:\Users\<you>\.platformio\penv\Scripts\python.exe tools\esp32_reset_probe.py COM5
C:\Users\<you>\.platformio\penv\Scripts\python.exe tools\esp32_reset_probe.py COM5 --diagnose
```

It tries several DTR/RTS orderings and reads the chip's own boot banner after
each, and `--diagnose` separates "the driver never asserts DTR" (fixable in
software) from "the GPIO0 transistor on this board is dead" (not).

### Fix 3 — slow the upload down

In `platformio.ini`:

```ini
upload_speed = 115200
```

Some USB-serial chips and long or thin cables cannot hold 921600 through the
reset handshake.

### Fix 4 — unplug the MCP2515 and the SD card, then retry

A pure diagnostic, and worth doing before you chase anything else. If it flashes
with the peripherals disconnected and fails with them attached, the wiring is
loading a strapping pin — see below.

### Fix 5 — strapping-pin conflicts

The ESP32 samples five pins at reset to decide how to boot. **Two of them are in
this project's pin map:**

| Pin | Used here for | Required at reset | Risk |
|---|---|---|---|
| **GPIO2** | `PIN_STATUS_LED` | **LOW or floating** to allow download mode | ⚠ real |
| **GPIO5** | `PIN_CAN_CS` | HIGH (its default) | low — an MCP2515's CS idles high, which matches |
| GPIO0 | *unused* | LOW to enter download mode | — |
| GPIO12 | *unused* | LOW, else the chip expects 1.8 V flash and will not boot | — |
| GPIO15 | *unused* | HIGH for normal boot | — |

**The one to check is GPIO2.** The on-board LED of a DevKit v1 drives GPIO2
through a resistor to ground, which is harmless and even helpful. But if an
**external LED has been wired active-low** — 3V3 → resistor → LED → GPIO2 — then
GPIO2 is held HIGH and **download mode is blocked permanently**; no amount of
BOOT-button pressing will help.

If that is how the LED is wired, either rewire it active-high (GPIO2 → resistor →
LED → GND) or simply disable it: set

```c
#define PIN_STATUS_LED  -1
```

in `src/config.h`. The LED is a convenience, nothing depends on it.

### Fix 6 — power

Do not power the board from USB and an external supply at the same time while
flashing. Disconnect the external supply, flash over USB only, then reconnect.

### Fix 7 — the auto-reset circuit itself

If the manual sequence works every time but automatic flashing never does, the
board's auto-reset circuit is the problem. A 1 µF capacitor between `EN` and
`GND` fixes most such boards.

Until then the manual sequence is a perfectly good workflow — and once **one**
build is on the board, you never need the cable again: `ENABLE_OTA` is on by
default, so subsequent uploads go over Wi-Fi.

---

# Path B — Arduino IDE

## B1. Generate the sketch folder

The firmware lives once, in `src\`. The Arduino IDE needs its own copy next to a
`.ino` named after the folder, so **double-click `arduino\sync.bat`** first. It
creates `arduino\CanLogger\` with every source file in it.

Re-run it whenever you edit anything in `src\`.

## B2. Install the ESP32 board support

The Arduino IDE cannot build for the ESP32 until you add Espressif's board
package. This is the step people usually miss.

1. **File → Preferences**
2. In **Additional boards manager URLs**, paste:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
   (If there is already a URL there, separate them with a comma.)
3. **OK**
4. **Tools → Board → Boards Manager…**, search **esp32**, install
   **"esp32" by Espressif Systems**.

Several hundred MB. Let it finish completely before continuing.

## B3. Open the sketch

**File → Open…** and select:

```
can-logger-esp32\arduino\CanLogger\CanLogger.ino
```

The IDE opens a window with many tabs (`app.cpp`, `config.h`, `dbc.cpp`, …). That
is correct — they are all part of the sketch and all get compiled.

> Do not move or rename files. The Arduino IDE requires the sketch folder and the
> `.ino` inside it to share a name (`CanLogger\CanLogger.ino`).

## B4. Board settings

Under **Tools**. The two marked ⚠ are **not** the defaults, and the build or the
upload fails without them.

| Setting | Value |
|---|---|
| Board | **ESP32 Dev Module** (under "ESP32 Arduino") |
| ⚠ Partition Scheme | **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** |
| ⚠ Port | the COM port from Step 0 |
| Upload Speed | **115200** to begin with |
| Flash Frequency | 80 MHz |
| Flash Size | 4MB (32Mb) |
| CPU Frequency | 240 MHz |
| Core Debug Level | None |

**Why that partition scheme:** the default leaves ~1.2 MB for the program, and
this firmware — Wi-Fi, web server, SD driver, DBC parser, dashboard — lands close
to that. If you see *"Sketch too big"* or *"text section exceeds available space
in board"*, this setting is the fix. The "Minimal SPIFFS" scheme also has two app
slots, which is what makes over-the-air updates possible; "Huge APP" has one and
cannot do OTA. (PlatformIO sets this automatically, which is the main reason
Path A goes wrong less often.)

**Why 115200 upload:** faster speeds fail on some USB-serial chips and cables.
Raise it to 921600 later, once it is working.

## B5. Compile

Press **✓ (Verify)** first, with nothing plugged in. A good build ends with:

```
Sketch uses 925849 bytes (47%) of program storage space.
Global variables use 114896 bytes (35%) of dynamic memory.
```

**No libraries need installing.** `SPI`, `SD`, `WiFi`, `WebServer`, `ESPmDNS` and
`ArduinoOTA` all ship with the ESP32 board package, and the MCP2515 driver is
part of this project. If the IDE reports a missing library, Step B2 did not
finish.

## B6. Upload

Press **→ (Upload)**. If it stalls on:

```
Connecting........_____.....
```

**hold the `BOOT` button** while that line prints, releasing once
`Writing at 0x...` appears. See
[download mode](#getting-the-chip-into-download-mode) if that does not help.

Then **Tools → Serial Monitor**, and set the baud rate at the bottom right to
**115200**.

---

# Prepare the SD card

Format it **FAT32** (cards over 32 GB usually ship as exFAT and must be
reformatted), then optionally put two files in the root:

| File | What for |
|---|---|
| `frames.dbc` | Your frame map. Without it the logger records raw frames. Start from `examples\example.dbc`. |
| `dash.cfg` | Your dashboard and your sendable values. Without it the web app is health cards and controls only. Made by `customize.bat` — see below. |
| `config.txt` | Wi-Fi settings. If absent, the logger writes a commented default on first boot — easiest to let it do that and then edit it. |

All three are plain text; Notepad is fine. Nothing needs rebuilding when you
change any of them — edit, put the card back, power cycle.

---

# Laying out the dashboard, before you go out

**This needs no board, no wiring and no CAN traffic — only your `.dbc`.**

You need Python: install it from <https://www.python.org/downloads/> and tick
**Add python.exe to PATH** during setup. Nothing else, no packages.

Then either:

- **drag your `.dbc` file onto `customize.bat`**, or
- **double-click `customize.bat`** and either pick your file from the list it
  shows, or press **b** to open a normal Windows file browser

It checks the file against the limits the firmware was built with, opens the
logger's real web page in your browser fed with simulated data, and writes
everything you build into a `.cfg` next to your `.dbc`.

In the page:

1. **Role (top right).** A DBC says who *sends* each message but not which of
   those nodes is your logger. Say which one it is and Fill puts what it sends
   on the Send tab and everything else on the dashboard. **Skip it** if you are
   only listening to a machine that already works — then both offer everything.
2. **Dashboard → Customize dashboard → Fill from frame map.** Every signal
   becomes a gauge, a bar, a thermometer or a state pill, picked from its unit
   and name. Drag them around, tap one to change it, delete what you do not want.
3. **Send → Set up sendable values → Fill from the frame map.** Pick the message
   your controller takes its settings from.

Close the window when you are done, then copy **both** files to the card:

```
mine.dbc   ->  frames.dbc
mine.cfg   ->  dash.cfg
```

If `customize.bat` closes instantly, Python is not on PATH — reinstall it with
the **Add python.exe to PATH** box ticked.

---

# After it boots

Press the board's `EN`/`RST` button — the banner only prints at boot. You should
see:

```
[     0.412] I ==== CAN Logger ESP32 v1.0.0 ====
[     0.690] I SD card OK: SDHC, 15193 MB
[     0.741] I frame map: 6 messages, 19 signals from /frames.dbc
[     0.802] I CAN controller OK: 250 kbit/s, normal mode
[     1.140] I HOTSPOT 'CAN-Logger' is up - open http://192.168.4.1 in a browser
[     1.201] I RECORDING STARTED -> /1.csv (+ /1.log), decoding via the frame map
```

Then connect a phone or laptop to the Wi-Fi network **`CAN-Logger`** (password
`canlogger`) and open **http://192.168.4.1**.

From then on it is one status line per second:

```
[   142.003] I REC 1.csv 00:02:21 | 141000 rows 3672 KB | 220 f/s | 7 ids | lost 0
```

---

# Common errors

| Message / symptom | Cause and fix |
|---|---|
| No COM port anywhere | USB driver not installed — **Step 0**. Or a charge-only USB cable. |
| `Failed to connect to ESP32: No serial data received` | Hold `BOOT` during upload — see [download mode](#getting-the-chip-into-download-mode). |
| `Wrong boot mode detected (0x13)` | The serial link is fine; GPIO0 was HIGH at reset. Start with the manual `BOOT`+`EN` sequence, and check GPIO2 is not held high by an external LED. |
| `esptool.write_flash` fails partway | Lower the upload speed to 115200. |
| `Sketch too big` / `text section exceeds available space` | Arduino IDE only: partition scheme is wrong — **B4**. |
| `fatal error: WiFi.h: No such file or directory` | Board is not set to an ESP32 board, or B2 did not finish. |
| PlatformIO: `Error: Unknown environment names` | You opened the wrong folder. Open `platformio\`, the one containing `platformio.ini` — **A2**. |
| Arduino IDE: only `CanLogger.ino` in the window | You did not run `arduino\sync.bat` — **B1**. |
| Serial Monitor shows garbage | Baud rate is not 115200. |
| Serial Monitor is empty | Press `EN`/`RST`; the banner only prints at boot. |
| `SD CARD NOT FOUND` | Card must be **FAT32**. Re-check wiring: CS=D4, SCK=D14, MISO=D27, MOSI=D13. |
| `no /frames.dbc on the card` | Expected if you did not put one there — everything is recorded as raw bytes. |
| `CAN CONTROLLER NOT RESPONDING` | MCP2515 wiring or power. The driver tests the SPI link both ways at boot, so this means CS=D5, MISO/MOSI or 3V3 is wrong. |
| Boots fine but `NO CAN TRAFFIC` | Check `CAN_CRYSTAL_MHZ` in `config.h` — 8 vs 16 MHz. See the main README. |

---

# Changing settings later

Do **not** rebuild the firmware to change the Wi-Fi or the frame map. Put the SD
card in the laptop and edit `config.txt` or `frames.dbc` in Notepad, save, put
the card back, power cycle.

Everything else worth changing is in **`src\config.h`**: pin map, bit rate,
crystal, listen-only, SD sync interval, power-fail pin, CANopen, table sizes.

**With PlatformIO** there is nothing else to think about: it compiles `src\`
directly, so just edit and press **✓**.

**With the Arduino IDE**, the tabs you see are generated copies. Edit `src\` and
double-click **`arduino\sync.bat`** to refresh them. Close the sketch in the IDE
first, then reopen it.

> One rule if you edit code: **do not move functions into the `.ino`.** The
> Arduino IDE auto-generates prototypes for anything it finds there, which strips
> `IRAM_ATTR` off the CAN interrupt handler and silently relocates it into flash.
> The `.ino` is a deliberate four-line wrapper around `app.cpp`.
