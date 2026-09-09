<h1 align="center">dual-can-logger-esp32</h1>
<p align="center"><i>ESP32 logger for two CAN buses at once, with optional DBC decoding</i></p>
<p align="center"><img alt="platform" src="https://img.shields.io/badge/platform-ESP32-E7352C?style=flat-square"> <img alt="framework" src="https://img.shields.io/badge/framework-Arduino%20%7C%20PlatformIO-00979D?style=flat-square&logo=arduino&logoColor=white"> <img alt="license" src="https://img.shields.io/badge/license-MIT-3FB950?style=flat-square"> <img alt="build" src="https://img.shields.io/badge/build-esp32dev%20compiles-58A6FF?style=flat-square"> <img alt="release" src="https://img.shields.io/badge/release-v2.0.0-8957E5?style=flat-square"></p>

> Log **two CAN buses at once** to one SD card, on one clock, with **nothing
> bus-specific compiled in** — identifiers, scaling and units all come from DBC
> files on the card, one per bus.

> [!IMPORTANT]
> **The single-bus receive path is field-proven; the second bus is not.** The
> reader task, the driver and the controller configuration grew out of code that
> recorded hours of a 540 frame/s bus without losing a frame, on real hardware.
> What has *not* been on a live wire is the second controller, and everything
> the dual-bus work touched: the shared SPI bus, the eight-column CSV, and the
> per-bus dashboard. The timing budget is computed and instrumented rather than
> measured in the field — the logger reports its own worst-case drain time so
> you can check it on your bench.
> → [Verification status](#13-verification-status)

<details open>
<summary><b>Contents</b></summary>
<br>

- [1. Hardware](#1-hardware)
- [2. Wiring](#2-wiring)
- [3. The frame maps (`/frames.dbc`, `/frames2.dbc`)](#3-the-frame-maps-framesdbc-frames2dbc)
- [4. Building and flashing](#4-building-and-flashing)
- [5. What gets recorded](#5-what-gets-recorded)
- [6. The live web app](#6-the-live-web-app)
- [7. Sending values back to the bus](#7-sending-values-back-to-the-bus)
- [8. Why it does not lose frames](#8-why-it-does-not-lose-frames)
- [9. Surviving a power cut](#9-surviving-a-power-cut)
- [10. Host-side tools](#10-host-side-tools)
- [11. Tuning](#11-tuning)
- [12. Troubleshooting](#12-troubleshooting)
- [13. Verification status](#13-verification-status)
- [14. Known issues](#14-known-issues)
- [15. Future development](#15-future-development)
- [License](#license)
- [Author](#author)

</details>

---

A logger for **two CAN buses at the same time**, built from an ESP32 and two
MCP2515s. It captures every frame on both wires, decodes each against **its own
DBC file** on the SD card, and writes one normalised CSV — with a live web
dashboard that shows whatever those DBCs describe.

Both buses are timestamped from **one clock, in the interrupt**, so a frame on
CAN1 and a frame on CAN2 are directly comparable to the microsecond. That is the
whole reason to log two buses on one device instead of two devices: a gateway
you are trying to characterise, a diagnostic bus next to the powertrain bus it
is answering for, a retrofit that has to agree with what it replaced.

No frame identifier, signal name, scaling factor or unit is compiled into the
firmware. Give it a DBC and it logs named signals in engineering units. Give it
nothing and it logs raw frames. The same binary does both, per bus.

```
     CAN bus 1                        ESP32 DevKit                phone / laptop
 ┌────────────────┐             ┌────────────────────────┐      ┌──────────────┐
 │ frames @ rate 1│───CAN_H/L──>│ MCP2515 #1 ─┐          │ WiFi │  dashboard   │
 └────────────────┘             │             ├─ ISR ─┐  │<────>│  both buses  │
     CAN bus 2                  │ MCP2515 #2 ─┘       │  │  AP  │  live log    │
 ┌────────────────┐             │   (shared VSPI)     ▼  │ or   └──────────────┘
 │ frames @ rate 2│───CAN_H/L──>│      queue ── decode   │ STA
 └────────────────┘             │      SD ── N.csv/N.log │
                                └────────────────────────┘
                                     ▲            ▲
                              /frames.dbc   /frames2.dbc
                                 (CAN1)        (CAN2)
```

**Contents** · [Hardware](#1-hardware) · [Wiring](#2-wiring) ·
[Frame maps](#3-the-frame-maps-framesdbc-frames2dbc) · [Build](#4-building-and-flashing) ·
[Output](#5-what-gets-recorded) · [Web app](#6-the-live-web-app) · [Sending](#7-sending-values-back-to-the-bus) ·
[Design](#8-why-it-does-not-lose-frames) · [Power cuts](#9-surviving-a-power-cut) ·
[Tools](#10-host-side-tools) · [Tuning](#11-tuning) ·
[Troubleshooting](#12-troubleshooting) ·
[Verification](#13-verification-status) · [Known issues](#14-known-issues) ·
[Future](#15-future-development) · [Changelog](CHANGELOG.md)

---

![Wiring diagram](docs/wiring.svg)


## 1. Hardware

| Part | Notes |
|---|---|
| **ESP32 DevKit v1** (30-pin) | Any ESP32 dev board works; the pin map below is for the 30-pin v1. |
| **2 × MCP2515 + TJA1050 CAN module** | The common blue module. **Check each crystal separately** — 8 MHz or 16 MHz, and two boards from one order can differ. |
| **Micro-SD card module** (SPI) | 3V3 signalling, but **power it from 5V (VIN)** — see §2. |
| **Micro-SD card**, FAT32 | Class 10 or better. Cards over 32 GB usually ship as exFAT and must be reformatted. |
| 120 Ω resistor, ×0–2 | One per bus, and only where the logger sits at that bus's physical end. The two decisions are independent. |
| 6800 µF / 10 V electrolytic + a diode | Optional, for the power-fail input — see §9. |

Total, at the time of writing, well under the price of a commercial two-channel
bus logger's licence dongle.

> **Check the crystal on each MCP2515 board.** The common blue modules use
> 8 MHz; some clones use 16 MHz — and the two modules in your drawer are not
> necessarily the same. Set `CAN1_CRYSTAL_MHZ` and `CAN2_CRYSTAL_MHZ` in
> `src/config.h` independently. The wrong value gives "NO CAN TRAFFIC" on a
> perfectly healthy bus, and it is the single most common reason a first attempt
> sees nothing on one bus while the other works.

### Running it with one bus

**Fit one MCP2515 and it works.** Nothing has to be changed to try it: at boot
the firmware probes both chip selects, and a controller that does not answer is
reported once and then skipped — no interrupt is attached to its INT pin, it is
never read, and the dashboard card for it says `NOT DETECTED` rather than
implying the logger is broken. Everything else behaves as the single-bus
logger does.

That matters more than it sounds. With no module fitted, that chip select
selects nothing and MISO is left floating; a float that happens to read back as
`0xFF` looks exactly like a controller reporting a full receive buffer. Reading
a controller that is not there is what turns "I only wired one" into a hang, so
the reader task skips anything that did not answer at boot.

Setting **`CAN2_ENABLED` to `0`** goes one step further: the second controller
is not probed at all, and the boot log says single-bus instead of warning about
a module you never installed. Use it when the second bus is a permanent
non-feature; leave it at `1` if you might plug one in later.

| | `CAN2_ENABLED 1`, module fitted | `CAN2_ENABLED 1`, nothing fitted | `CAN2_ENABLED 0` |
|---|---|---|---|
| Boot log | `CAN2 controller OK` | `CAN2 did not answer - continuing on CAN1 alone` | `CAN2 disabled in config.h` |
| Dashboard card | live figures | `NOT DETECTED` | `OFF` |
| CAN2 read each pass | yes | **no** | no |
| INT pin | armed | not armed | not armed |
| CSV `bus` column | `1` and `2` | always `1` | always `1` |

**What it is not:** byte-identical to a `can-logger-esp32` recording. The CSV
keeps its eight columns and the `bus` column simply always says `1`. A column
that appeared and disappeared depending on how the hardware happened to be
populated would be far worse than one that is sometimes constant — every tool
reading these files would need to handle both. `tools/parse_log.py` reads the
old seven-column files too, so recordings from either logger open in the same
tools.

It is also the control case when you want to measure what the second bus
actually costs.

---

## 2. Wiring

**Both CAN controllers share one SPI bus. The SD card gets its own.** That split
is the load-bearing part of the design: an SD write takes milliseconds and
stalls for hundreds of them on a bad card, and sharing a bus with the
controllers would stall CAN reads for exactly that long. Two MCP2515s sharing
VSPI costs nothing, because MISO tri-states while CS is high — only CS and INT
have to be unique.

| MCP2515 #1 → CAN1 | ESP32 | | MCP2515 #2 → CAN2 | ESP32 | | SD card | ESP32 |
|---|---|---|---|---|---|---|---|
| VCC | 3V3 | | VCC | 3V3 | | VCC | **5V (VIN)** |
| GND | GND | | GND | GND | | GND | GND |
| CS  | **D22** | | CS  | **D5**  | | CS   | **D4**  |
| INT | **D21** | | INT | **D17** | | SCK  | **D14** |
| SCK | D18 (VSPI) | | SCK | D18 (shared) | | MISO | **D27** |
| MISO| D19 | | MISO| D19 (shared) | | MOSI | **D13** |
| MOSI| D23 | | MOSI| D23 (shared) | | | (HSPI) |

Keep the shared SCK/MISO/MOSI stubs short — a breadboard star with two long legs
is the one place this topology gets fussy.

> **There is no pin marked D17 on most DevKit v1 boards.** That board labels
> UART2 by function: the pin silkscreened **TX2** is GPIO17 and **RX2** is
> GPIO16. CAN2's INT goes to TX2. USB upload and the serial monitor run on
> UART0 and are unaffected by this.

> **Why D22/D21 for CAN1.** Neither is a strapping pin, and — unlike GPIO16/17 —
> neither is taken by PSRAM on a WROVER module. They are the ESP32's default I2C
> pins, unused here. If you are on a WROVER, note that CAN2's INT on GPIO17 is
> still a conflict; move it to GPIO25 or GPIO26 and update `PIN_CAN2_INT`.

> **Power the SD module from 5V (VIN), not 3V3.** Almost all micro-SD breakout
> boards carry their own 3V3 regulator and level shifters, and expect 5 V on
> VCC. On 3V3 the regulator has no headroom, the card browns out the moment it
> draws write current, and `SD.begin()` fails in a way that is indistinguishable
> from an empty slot. The SPI lines stay 3V3 either way. If your module is one
> of the bare ones with no regulator, 3V3 is correct for it — check the board.
>
> If the card still will not mount, it is usually the clock rather than the
> card: the firmware retries at 10, 4 and 1 MHz and logs which speed it took,
> because breadboard jumpers and cheap adapters often will not carry 20 MHz.

Bus side: each module's `CAN_H` and `CAN_L` go to **its own** bus, with `GND` to
that bus's ground. Bit rates are set per bus (`CAN1_BITRATE_KBPS`,
`CAN2_BITRATE_KBPS`); 100, 125, 250, 500 and 1000 are supported at either
crystal frequency, and the two buses do not have to match. If you do not know a
bus's rate, the logger can [work it out by listening](#finding-the-bit-rate-itself).

**Terminate each bus on its own merits.** 120 Ω belongs across `CAN_H`/`CAN_L`
only where the logger is at that bus's physical end. Being the end node on one
bus says nothing about the other.

The on-board LED (D2) is steady off when idle, blinks slowly while recording and
blinks fast on an SD fault. Set `PIN_STATUS_LED` to `-1` to disable.

### Normal mode or listen-only, per bus

`CAN1_LISTEN_ONLY` and `CAN2_LISTEN_ONLY` default to **0** (normal mode), so the
logger acknowledges frames. That is correct when it is the only other node on
that bus — with nobody to ACK, the talking node goes error-passive and
eventually bus-off.

Set one to **1** when you are tapping a bus that already has two or more live
nodes. Listen-only never drives that bus at all, which is the safe choice on a
machine that is actually running — and being independent per bus is the point:
a diagnostic bus you may drive, sitting next to a powertrain bus you may not.

### Finding the bit rate itself

Set **`CAN1_AUTODETECT`** or **`CAN2_AUTODETECT`** to `1` and that bus works out
its own bit rate at boot: it listens at each `(bit rate, crystal)` pair the
driver has timings for and keeps the first one that decodes real frames. A frame
only reaches a receive buffer after its CRC has passed, so this is not "did the
line wiggle" — at the wrong bit rate the count stays at zero however busy the
bus is, and that is what makes it work at all.

The pair already in `config.h` is tried **first**, so a correct configuration
costs one window and usually far less. Detection is per bus: one bus can be
detected while the other is pinned.

**It listens in listen-only mode whatever `CAN<n>_LISTEN_ONLY` says.** At the
wrong bit rate a normal-mode node reads valid traffic as malformed and answers
with error frames — a diagnostic tool that corrupts the bus while working out
how to read it would be worse than no tool. Every wrong guess here is silent.

Three things to know before you turn it on:

| | |
|---|---|
| **It needs traffic** | A quiet bus is indistinguishable from a wrong bit rate. With nothing talking, detection fails and the `config.h` values are used — it never leaves a bus unconfigured. `CAN_AUTODETECT_MS` is 300 ms per candidate, so a bus carrying roughly 7 frames/s or better is detectable; raise it for a bus that only speaks when spoken to |
| **A crystal and a bit rate multiply** | The registers for 250 kbit/s on an 8 MHz module produce 500 kbit/s on a 16 MHz one, and both decode the same wire perfectly. Nothing visible over SPI separates them, so detection sweeps the crystal **you configured** first and only then the other. A correct `CAN<n>_CRYSTAL_MHZ` therefore gives a correct bit rate; a wrong one still gets you a working bus, and a warning in the log that the reported rate is scaled by the same factor |
| **It costs boot time** | Worst case — nothing on the bus — is every pair tried for `CAN_AUTODETECT_MS`, about 3 s per bus. Nothing is recorded during it: detection runs before the reader task exists and the frames it hears are thrown away |

The log says which it was, and so does the Bus tab: `250 kbit/s detected` is a
rate the bus confirmed, `250 kbit/s assumed` is a search that decoded nothing
and fell back to `config.h`. Only the second is a reason to go and look at
`CAN<n>_BITRATE_KBPS`.

---

## 3. The frame maps (`/frames.dbc`, `/frames2.dbc`)

This is the part that keeps the firmware generic. Everything the logger knows
about a bus comes from a **DBC file in the root of the SD card**, read once at
boot. Nothing is compiled in.

**One map per bus, and they are separate files:**

| File | Bus |
|---|---|
| `/frames.dbc` | CAN1 |
| `/frames2.dbc` | CAN2 |

They are separate rather than merged because **the same identifier routinely
means different things on two buses**. A shared map would decode CAN2 with
CAN1's meanings and be confidently, silently wrong — which is the one failure a
logger must not have. Two buses that genuinely share an ID space can simply be
given identical files.

- **Map present** → frames it describes are decoded into named signals in
  physical units, and both the CSV and the web page show those names.
- **Map absent** → every frame on **that bus** is recorded as raw payload bytes.
  This is a supported mode, not a failure, and it is per bus: mapping CAN1 while
  CAN2 records raw is an ordinary way to run.
- **Both at once** → identifiers the map does not describe still get recorded,
  as raw bytes, alongside the decoded ones. A logger that silently drops the
  frames it does not recognise is a logger that lies about what was on the bus.

Copy [`examples/example.dbc`](examples/example.dbc) to the card as
`/frames.dbc` and edit it, or export one from your usual CAN tool. The paths are
`DBC_PATH` and `DBC2_PATH` in `src/config.h`. Either can be uploaded from the
web app — there is a **Frame map: CAN 1** and a **Frame map: CAN 2** button in
the header, so the one you press names the bus it will replace.

> **The two maps share one heap budget.** They are sized to the free heap in
> order, CAN1 first, so a very large map on CAN1 is the one that shrinks CAN2's.
> `DBC_HEAP_RESERVE` in `src/config.h` sets what is held back for Wi-Fi. The log
> says what was kept if either map did not fit.

### What the parser supports

| Construct | Support |
|---|---|
| `VERSION "…"` | Kept, and written into every CSV header |
| `BO_ <id> <Name>: <dlc> <Node>` | Yes. 29-bit ids carry bit 31 set, as usual. `<Node>` is the **transmitter** and is kept — see below |
| `BU_: <Node> …` | Yes — the node list, up to `DBC_MAX_NODES` |
| `SG_ … : <start>\|<len>@<order><sign> (<fac>,<off>) [min\|max] "<unit>"` | Yes |
| Byte order `@1` (Intel) and `@0` (Motorola) | Both |
| Signed and unsigned, 1–64 bits | Yes |
| Factor and offset | Yes — **exactly**, see below |
| Units | Yes, carried into the CSV and the dashboard |
| `VAL_` value tables | Yes — the CSV prints `running`, not `2` |
| Multiplexing (`M`, `m0`, `m1`, …) | Yes — a multiplexed signal is emitted only when the multiplexor selects it |
| `SIG_VALTYPE_ … : 1;` / `: 2;` | Yes — IEEE float32 and float64 payloads |
| `BA_ "Unwrap" SG_ <id> <Sig> 1;` | Yes — see [free-running counters](#free-running-counters) |
| `CM_`, `NS_`, `BS_`, `BA_DEF_` | Parsed past and ignored |

**The transmitter is the only direction a DBC states**, and it is half of what
you need. The file says `ABS_Cmd` is sent by `Tester`; it does not say whether
*you* are the tester. Answer that once — [which node this logger
is](#which-node-this-logger-is) — and the two Fill buttons can tell a command
from a reading. Nothing in the recording path acts on it either way: a frame
that arrives is recorded whoever the file says sends it.

**Deliberately not supported**, so you are not surprised later:

- **`[min|max]` is parsed but not enforced.** A logger records what is on the
  wire. Clamping a signal to its declared range would hide exactly the fault you
  bought the logger to find.
- **Signal groups, `SG_MUL_VAL_` (extended multiplexing), environment variables
  and attribute definitions other than `Unwrap`.** Extended multiplexing in
  particular is silently ignored: such a signal is treated as ordinary, which
  may decode nonsense on the pages where it is not present.
- **CAN FD** — the MCP2515 is a classical CAN controller. Payloads over 8 bytes
  do not exist here.
- **Receive filters.** The controller's acceptance filters are switched off on
  purpose. Everything on the bus is captured.

Anything the parser cannot make sense of is counted and reported at boot
(`N line(s) of /frames.dbc could not be parsed`), never skipped in silence.

### How big a frame map can be

**The tables are sized to your file, not to a number chosen at compile time.**
The file is read twice at boot — once to count `BO_`, `SG_` and `VAL_`, once to
parse — and the tables are then allocated to fit it. A twenty-signal bus costs
about two kilobytes; a 104-message, 707-signal J1939 steering bus costs about
84 KB and loads whole.

That is a fix, not a feature. The tables used to be fixed at 64 messages and 256
signals, and a real ten-hour field recording on that steering bus decoded the
first 256 signals and wrote the other **451 as raw payload bytes**, with one
line in the CSV header to say so.

Three things still bound it, and all three are reported at boot:

| Bound | Default | What happens |
|---|---|---|
| `DBC_MAX_MESSAGES` / `DBC_MAX_SIGNALS` / `DBC_MAX_VALDESC` | 256 / 1024 / 2048 | Ceilings, so a corrupt file cannot ask for a gigabyte. Exceeding one truncates the map and says so |
| `DBC_HEAP_RESERVE` | 90 KB | Memory the map will **not** take. It loads before the radio starts, so an unbounded map could leave Wi-Fi with nothing. Over budget, the request is scaled down proportionally |
| `DBC_MAX_NODES` | 32 | Names in `BU_`. The only fixed table left, at a kilobyte |

The boot line reports what it cost and what is left:

```
I frame map: 104 messages, 707 signals from /frames.dbc (84 KB, 118 KB free)
```

If it had to cut the map back, it names the knob:

```
W frame map wants 138 KB, 205 KB free - keeping 115 KB and leaving 90 KB for
  Wi-Fi. If the logger runs with plenty spare, lower DBC_HEAP_RESERVE in config.h.
```

`python3 tools/check_dbc.py yours.dbc` prints the same estimate before you go
anywhere near the machine.

### How long a name can be

`DBC_NAME_MAX` is 32, so **31 characters** of message and signal name reach the
CSV. It was 24, which wrote `GuidanceCurvatureCommand` as
`GuidanceCurvatureComman` — long enough to look right and short enough that
matching rows back to the source DBC by exact name silently returned nothing.

Anything still too long is counted and reported (`N name(s) are longer than 31
characters and are cut short in the CSV`), and every CSV header states the cap
whether or not it bit:

```
#   names are cut to 31 characters
```

### Values are exact

Where the factor and offset are decimal literals — `1`, `0.001`, `1e-06`, `0.5`,
`2.5` — the physical value is computed in **integer arithmetic** and the decimal
point is simply placed. No `float` is involved anywhere in the hot path.

That matters twice over: a value that left a node as the exact integer `12345`
cannot come back as `12.344`, and formatting stays fast enough for four-figure
frame rates. A factor that genuinely cannot be expressed that way falls back to
`double`, and the CSV header says so (`some values via floating point`).

### Free-running counters

Machine buses often carry a node's own microsecond clock in the payload, and
that clock wraps — a 32-bit µs counter wraps every ~71.6 minutes. Declaring the
signal as unwrappable:

```
BA_DEF_ SG_ "Unwrap" INT 0 1;
BA_ "Unwrap" SG_ 258 SampleTime 1;
```

makes the logger track the wraps and emit a value that keeps counting up across
them. The column stays monotonic over a long session and can be differentiated
without a special case downstream. It is ordinary DBC syntax, so your other
tools ignore it harmlessly.

### CANopen

Set `CANOPEN_DECODE` to `1` in `src/config.h` and identifiers the DBC does not
describe are labelled the way CiA 301 defines them — the COB-ID carries a 4-bit
function code and a 7-bit node ID, so `0x18C` is *the first transmit PDO of node
12* on any conforming network, with no node map needed.

Heartbeats, NMT commands, emergencies and SDO transfers are decoded properly
(`state=OPERATIONAL`, `cs=UPLOAD-INIT index=0x1018 sub=1`). PDOs get a name and
their raw payload and nothing more — what is inside a PDO is whatever that
node's mapping objects say, and inventing signal names for it would be worse
than useless. Map your PDOs in the DBC.

Leave it at `0` on a plain CAN bus, where CANopen names would be actively
misleading.

---

## 4. Building and flashing

The sources live once, in **`src/`**. Both front ends compile the same files.

### From a release, without building anything

Every release ships **one file**, flashed at offset `0x0`. An ESP32 build is
normally four pieces at four offsets, and getting one of them wrong gives a
board that boots into nothing and says nothing about why; the merged image
leaves one number to get right, and it is zero.

**There is one image, not one per operating system.** It is ESP32 machine code
and it runs on the board; your computer only copies it over USB. macOS, Linux
and Windows all send the same bytes, so the same two commands do it everywhere:

```bash
pip install esptool
python3 tools/flash.py --image dual-can-logger-esp32-v2.0.0-4mb-merged.bin
```

It finds the board itself, and says what to try if the chip never enters
download mode. `--port COM5` (or `/dev/ttyUSB0`, or `/dev/cu.usbserial-0001`)
overrides the guess; `--erase` wipes the flash first, which also clears the
dashboard saved in NVS.

On Windows, if the board never appears as a COM port at all, that is the
USB-to-serial driver — [WINDOWS.md](WINDOWS.md#step-0--usb-driver) has the two
chips and their drivers.

### PlatformIO (recommended)

```bash
cd platformio
pio run -t upload -t monitor
```

or, from anywhere and on any of the three platforms:

```bash
python3 tools/flash.py                # build, find the board, flash
python3 tools/flash.py --merge out.bin   # build the single-file image
```

`platformio.ini` points `src_dir` at `../src`, so there is nothing to copy, and
the partition scheme is already correct.

**No shell needed:** open the **`platformio/`** folder in VS Code with the
PlatformIO extension and use the ✓ (build) / → (upload) / 🔌 (monitor) buttons in
the bottom status bar. Same on Windows.

### Arduino IDE

The Arduino IDE requires every source file to sit next to a `.ino` named after
its folder, so the sketch folder is **generated** from `src/`:

```bash
./arduino/sync.sh          # or double-click arduino\sync.bat on Windows
```

Then open `arduino/CanLogger/CanLogger.ino`, select **ESP32 Dev Module**, set
**Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA)**, and upload. Re-run
`sync.sh` after editing anything in `src/`.

📄 **On Windows, follow [WINDOWS.md](WINDOWS.md)** — both front ends from
scratch with no command line: the USB driver, the two non-default Arduino IDE
settings that otherwise break the build, and what to do about
*"Wrong boot mode detected"*.

**No libraries to install.** `SPI`, `SD`, `WiFi`, `WebServer`, `ESPmDNS` and
`ArduinoOTA` all ship with the Arduino-ESP32 core, and the MCP2515 driver is
part of this project (`src/mcp2515.cpp`) so the interrupt path is fully under
our control.

> The `.ino` is a deliberate four-line wrapper around `app.cpp`. The Arduino IDE
> auto-generates prototypes for functions it finds in a `.ino`, which strips
> `IRAM_ATTR` off the CAN interrupt handler and silently moves it into flash —
> where it crashes as soon as the flash cache is disabled. Keeping every real
> function in a `.cpp` avoids that preprocessor entirely. Don't move code into
> the `.ino`.

### Over the air

`ENABLE_OTA` is on by default. After **one** USB flash, every later upload can go
over Wi-Fi:

```bash
pio run -e ota -t upload
```

Recording is stopped and the files are closed before the update starts, and the
CAN interrupt is detached for the duration — an OTA rewrites flash while the
firmware is live, and neither a half-written CSV nor an interrupt firing
mid-erase may survive into that.

---

## 5. What gets recorded

Recording starts automatically as soon as the SD card and CAN controller come up,
so nothing is missed at power-on. Files are numbered from 1:

```
/1.csv   /1.log   /1.meta
/2.csv   /2.log   /2.meta
```

A slot counts as taken if *either* file exists, so the set always shares a
number. **Both buses go into the same files** — one CSV, one clock, with a bus
column — because the reason to record two buses on one device is to be able to
line them up afterwards, and two files would make that a join.

### `N.csv` — the normalised schema

**Eight** fields, `;` separated, always in this order, **one row per decoded
signal**:

```
t_us;bus;id;name;signal;value;unit;raw
0;1;0x100;NodeStatus;Uptime;42;s;
0;1;0x100;NodeStatus;State;running;;
1000;1;0x101;MotorFeedback;Speed;-250.00;rpm;
1500;2;0x100;PumpState;Pressure;3.5;bar;
2000;2;0x200;;;;;1122
```

Look at rows 3 and 4: **the same identifier, `0x100`, on both buses, decoded
into two different messages.** That is the normal case on a real machine, and it
is why the bus column is where it is.

A frame the logger **sent** rather than received carries a `TX:` prefix on the
message name, and nothing else changes — see
[section 7](#7-sending-values-back-to-the-bus).

| Column | Meaning |
|---|---|
| `t_us` | Recorder clock, µs, **starting at 0 for every file**. Captured inside the CAN interrupt, so it is the arrival time on the wire — not the time the row happened to be formatted. **One clock for both buses**, so CAN1 and CAN2 rows are directly comparable. |
| `bus` | Which CAN bus the frame arrived on: `1` or `2`. Counts from one, matching the wiring diagram and the labels on the case. |
| `id` | Identifier. An 11-bit id prints short (`0x100`); a 29-bit id always prints eight digits (`0x00000100`). That is not cosmetic — the two are different frames, and the width is what tells them apart. |
| `name` | Message name from **that bus's** DBC, or the CANopen function. Empty for an identifier nothing described. |
| `signal` | Signal name from the DBC. Empty on a raw row. |
| `value` | The physical value, or the symbolic text when a `VAL_` table names it. Empty on a raw row. |
| `unit` | Unit string from the DBC. Often empty; that is not an error. |
| `raw` | Payload as hex. **Always** present when the row carries no decoded signal. Also on the first row of each decoded frame if you set `CSV_INCLUDE_RAW`. |

**All rows of one frame share `t_us`, `bus` and `id`** — group on that triple. A
frame with four signals is four rows; a frame nothing could decode is exactly one
row carrying its bytes. That is the whole contract, and it does not change with
the DBCs: downstream tooling parses deterministically without re-implementing any
CAN decoding.

> **Group on the bus too.** With two buses an identifier alone no longer names
> anything. A tool that groups on `(t_us, id)` will silently merge two different
> messages the first time both buses carry `0x100` — which they usually do.
> `tools/parse_log.py` names every signal `CAN<n>.Message.Signal` for exactly
> this reason.

#### Reading recordings from the single-bus logger

The single-bus `can-logger-esp32` wrote **seven** columns and no `bus`. Both
schemas are readable:

- `tools/parse_log.py` and `tools/plot_log.py` detect the header and treat a
  seven-column file as one bus's worth of rows, reported as `CAN1`.
- The companion `N.meta` carries `"schema": 2`, so a tool of your own can branch
  on a number rather than counting separators.

This logger only ever *writes* schema 2, including when `CAN2_ENABLED` is `0` —
the column is there and always says `1`. A column that appears and disappears
depending on a build flag would be worse than one that is sometimes constant.

### `N.meta` — the sidecar that explains the CSV

JSON, written and closed before the first data row. It carries the schema
number, the column descriptions, and **both** frame maps exactly as they were
when the recording was made — message by message, signal by signal, with units
and exactness flags.

A CSV found on a card a year from now is interpretable on its own, even if both
DBCs have since been edited.

Cost: roughly 30–45 bytes per CSV row.

### `N.log` — the detailed log

Same base name. Everything the serial port shows **plus** per-identifier counters
and rates, queue depth and high-water mark, SD write and sync timings, sticky
interrupt flags, heap and network state — once per second.

The serial stream is deliberately one line per second so it stays informative
without becoming an I/O cost of its own:

```
[   142.003] I REC 1.csv 00:02:21 | 141000 rows 3672 KB | CAN1 rx=358/s irq=358/s 14% | CAN2 rx=143/s irq=143/s 42% | q=61/2048 peak=124 drain=112 us | lost 0
```

Both buses on one line, each with its own frame rate, interrupt rate and load,
because "is the logger working" has two answers now. `drain` is the worst
microseconds one pass spent emptying both controllers — see
[section 8](#8-why-it-does-not-lose-frames).

Serial is **115200 baud** — one line per second has nothing to gain from a faster
port, and 115200 is the speed every USB-serial chip and driver handles without
argument.

---

## 6. The live web app

Open `http://<ip>` (or `http://can-logger.local`). Credentials live in
`/config.txt` on the SD card — see [`examples/config.txt`](examples/config.txt) —
so the logger can be moved between sites by editing a text file, no reflash. In
`ap` mode it makes its own hotspot; in `sta` mode it joins your network and falls
back to the hotspot if it cannot.

Four tabs, arranged around *when* each one is needed:

| Tab | For | Polls |
|---|---|---|
| **Dashboard** | standing at the machine: health, your own gauges, start/stop | one small request, 5 Hz |
| **Bus** | working out a frame map, or something is wrong: every identifier and every decoded signal as text | the full status document, 1.7 Hz |
| **Send** | writing a value back — see [section 7](#7-sending-values-back-to-the-bus) | 1.4 Hz |
| **Log** | what the logger has been saying | the log tail |

**Only the visible tab is polled, and a backgrounded browser tab polls nothing
at all.** That is not a detail: it is why a dashboard updating five times a
second costs the logger *less* than the old single view did at two.

**Nothing is rendered on the ESP32.** Every dial, needle, compass, thermometer
and pill is SVG built by JavaScript in the browser — hand-drawn rather than a
library, because the page has to load from the logger's own hotspot with no
route to the internet. What the logger sends is one small JSON document of
pre-formatted values and health counters; it never touches a pixel. Customizing
costs it even less: laying out a dashboard is browser work on a copy of the
config, and the logger sees a request only when the frame map is first read and
about a second after each edit stops.

![The dashboard](docs/img/dashboard-dbc.png)

### The Dashboard

**Five health cards across the top**, all driven by measurements rather than
assumptions:

| Card | Shows | Turns red when |
|---|---|---|
| **SD Card** | type and capacity | no card, or a write failed |
| **Bus** | frames/s arriving | nothing has arrived for 500 ms |
| **Interrupt Path** | interrupts/s and the INT pin level | the ISR stops firing — see below |
| **Data Integrity** | frames lost this recording, and how much is at risk from a power cut | anything was lost, or nothing is arriving to check |
| **CAN Bus Load** | percent of the configured bit rate, with a meter | above 80 % (amber from 60 %) |

**Interrupt Path deserves its own card.** The MCP2515 holds two frames, and the
reader has a 20 ms fallback poll behind the interrupt. If the INT line ever
stops firing, the poll silently caps throughput at **~100 frames/s** — 50
wake-ups a second times two buffers — and *nothing else looks wrong*. The logger
keeps writing rows, just ninety percent fewer of them. This card is what makes
that visible instead of invisible.

**In the middle, whatever you decided matters** — a grid of gauges you lay out
yourself. See [Customizing it](#customizing-the-dashboard) below.

**Two control cards at the bottom** — Recording, with START/STOP and the current
file, rows and size; and Logger, with uptime, free memory and RESTART. They sit
below the data on purpose: the things you read constantly belong at the top, the
things you press occasionally at the bottom.

On a fresh logger the middle is empty and the tab is just health and controls,
which is a complete and useful dashboard on its own.

### Customizing the dashboard

![Building a dashboard](docs/img/customize.gif)

*A logger with nothing on its card, to a laid-out dashboard. Recorded from the
real page — the drag is a real pointer drag through the same handlers a finger
goes through.*

Press **Customize dashboard**. The grid becomes editable and a toolbar appears.

![Customizing the dashboard](docs/img/customizing.png)

While the grid is in this state the fast poll drops to a two-second heartbeat —
the cells are placeholders being dragged around, so there is nothing live to
update and no reason to ask the logger five times a second.

Tap any cell to open the editor.

<img src="docs/img/editor.png" alt="The cell editor" width="720">

Choosing a signal fills in everything the frame map already knows — its unit,
its `[min|max]` range, and how many decimals its scaling factor actually
justifies — and suggests a shape from the unit and the name: `km/h` and `rpm`
get a dial, `deg` with a negative minimum gets a centre-zero indicator, `bar` a
half gauge, `degC` a thermometer, a signal with `VAL_` labels a state pill. All
of it is a suggestion you can override; none of it is a question you have to
answer.

The preview at the top is the **real widget fed the real live value**, so the
needle sits where it is going to sit.

| | |
|---|---|
| **Ten ways to draw a value** | dial (speed, rpm, flow) · half dial (pressure, load) · centre-zero angle (steering, tilt, articulation) · compass (heading, yaw) · bar (percentages) · tank (fuel, oil level) · thermometer · plain number · trend (a rolling trace) · state (a coloured pill showing a `VAL_` name) |
| **Arranging** | drag cells to swap them — pointer events, so it works with a finger as well as a mouse. Columns 1–6, rows 1–8, up to `DASH_MAX_CELLS` cells |
| **Colour** | optional amber and red thresholds, and a *low values are the bad ones* switch for a level or a pressure that must not drop |
| **Fill from frame map** | lays out everything the loaded `.dbc` describes, in file order, as far as the grid goes. Nothing has to be connected — this is the one to press at a desk, before going out |
| **Fill from bus** | the same list, but the signals *actually arriving* are placed first and the rest are capped. This is the one to press standing at the machine, because a grid full of cells that will never update is worse than a small one where everything moves |
| **Setup file** | export and import — see [The setup file](#the-setup-file) |

### Loading a frame map

**Frame map: CAN 1** and **Frame map: CAN 2**, in the header, each take a
`.dbc` off the phone or laptop you are holding and put it on the card as
`/frames.dbc` or `/frames2.dbc`. The map is rebuilt on the spot
— no reboot, no card reader, no laptop cable — and the dashboard re-binds to it
immediately, saying how many saved cells no longer match if any do not.

**Loading a map clears what the new one cannot account for.** A different frame
map means a different bus, so every cell and every sendable value naming a
signal the new file does not have is removed and the gaps closed. That bus's
role goes the same way if the new file has no `BU_` node of that name — it is not
merely stale then, it is unanswerable, and the header would go on claiming a role
while that bus's Fill buttons quietly stopped separating anything by it, so the
question is asked again. The other bus's role is untouched; it was never about
this file. Load an unrelated `.dbc` and you get an empty setup, which is the
honest result; reload a corrected version of the same one and your layout
survives, because its signals are still there. One-off frames named by
identifier are never touched: they name no signal, so no frame map can
invalidate them.

The logger does the clearing and the page **re-reads** the result rather than
repeating it on its own copy. That is deliberate: two copies pruning themselves
independently is exactly how a browser holding the old layout gets to write it
back over a card that had just been cleaned.

This is not what happens at boot. There, a cell whose signal is missing is kept
and drawn as unresolvable — the usual reason is a card with no DBC on it, and
destroying a layout built at a desk because the card was in the other pocket
would be unforgivable.

It is streamed to the card a chunk at a time rather than held in memory: a real
machine's `.dbc` runs to ninety kilobytes, which is more than the frame map
built from it and a third of this chip's free heap. It lands on a temporary name
and is renamed into place only once the whole file has arrived, so a Wi-Fi
dropout mid-upload costs you the upload and not the map you were already using.

**Refused while a recording is running**, deliberately. Every CSV opens with a
header naming the exact map its rows were decoded through; swapping the map
underneath a file in progress would make that header a lie for every row after
the swap. Stop, load, start.

### Which node this logger is

<img src="docs/img/role.png" alt="Choosing which node the logger is" width="720">

A `.dbc` says who **sends** each message. It does not say which of those nodes
is the box in your hand — and that one missing fact is the whole difference
between a reading and a command:

```
BO_ 768 ABS_LeftEncoder: 8 ABS_ECU     the ECU sends it   -> a reading
BO_ 800 ABS_Cmd:         8 Tester      the tester sends it -> a command
```

Answer it and **Fill from frame map** puts what your node sends on the **Send**
tab and everything else on the dashboard. The answer lives on a **Role** button
in the header — visible from every tab, because a wrong answer does not announce
itself, it just fills the wrong half of the map into the wrong screen. It opens
by itself the first time after you load a frame map, which is the moment it is
worth answering and the only moment it costs nothing.

**The answer is per bus, and there is a button for each.** The header carries a
column per controller — its frame map above, its role below — because both are
facts about one bus and nothing else:

|  | CAN 1 | CAN 2 |
|---|---|---|
| | `Frame map` | `Frame map` |
| | `Role: Host` | `Role: none` |

The two buses have separate frame maps and therefore separate `BU_` node lists,
so a name that identifies this logger on one of them usually does not appear in
the other's file at all. More to the point, the logger is routinely **a node on
one bus and a pure listener on the other** — a tester driving a diagnostic bus
while only watching the powertrain. One shared answer forced those two
situations to give the same reply, and could not be read either: the button said
`Role: Tester` without saying which bus that was about.

In `dash.cfg` each is its own line, tagged the way cells and setpoints are, with
bus 1 left implicit — so a file written before this still means what it meant:

```
role "Tester"
role "Logger" bus=2
```

**Skip is a real answer, and usually the right one.** On a machine that already
works, none of the nodes in the file is you — you are a bystander with a clip
lead. Skipping means both Fill buttons offer everything and you sort out which
is which, exactly as they behave with no frame map at all.

Nothing about recording changes either way: every frame that arrives is logged
whoever the file says sends it, and a frame you send is built from the frame map
alone. The role is an authoring aid, and the firmware never filters on it.

### The setup file

Everything customized on a logger — the dashboard cells **and** the values that
can be sent — is one file. So there is **one** Export and **one** Import, in the
header, on every tab, behind **Setup file**.

<img src="docs/img/setup-file.png" alt="The setup file" width="720">

**Export downloads to the device in your hand**, into its downloads folder.
Nothing on the logger changes and no card is written, so it is safe to press
mid-recording. **Import replaces both halves at once**, on the card and in the
logger's own memory.

That is the whole sheet: what is on the logger now, Export, Import.

### Saving happens by itself, and has to

An edit reaches the logger about a second after you stop making it. There is no
Save button, and there was one for exactly one release — it was a mistake worth
recording, because it broke the dashboard in a way that is easy to miss.

**The dashboard's values come from the logger, not from the browser.**
`handleDash()` walks `g_dash` — the layout the *logger* holds — and renders the
numbers from it. A cell the logger has not been told about is a cell it sends no
value for, so it sits blank. With a Save button, an editing session that had not
reached the logger yet was one where half the screen showed nothing, and the
dashboard only came alive once you found the button.

The delay is what keeps it cheap: dragging a cell across the grid is one write
when you let go, not one per frame. Leaving *Customize* writes immediately.

The desk tools do the same thing for the same reason — the page tells the server
after every edit, or the preview would show blank cells — but **they write no
files at all.** `--cfg` is read once at the start and never written back, no
`.cfg` appears next to a `.dbc` you loaded, and **Export** in the page is the
one way a setup comes out of them. A logger is where a setup lives; a desk is
not, and a tool that wrote a file every time somebody dragged a gauge left them
in whatever directory it had been pointed at.

Two copies, one rule — because this is set up **at a desk, before you go out**,
and has to be there when you arrive.

```
/dash.cfg on the SD card        the copy you can edit in any text editor,
                                keep in version control, and copy between
                                loggers. See examples/dash.cfg.

NVS in the ESP32's own flash    survives a card swap, a reformat, and
                                running with no card at all.
```

At boot the logger compares `/dash.cfg` against a hash of the text it last
agreed with:

- **hashes match** → nobody touched the card; the flash copy wins, which is
  what preserves anything saved from the browser since the last boot
- **hashes differ** → somebody edited the file; the card wins
- **no file on the card** → the flash copy is written out to it, which is how
  the file comes into existence
- **nothing anywhere** → an empty grid, and the page says so

The effect is the one people expect: *whichever you edited last is the one you
get.* Saving from the browser writes the card immediately, through the task that
owns it; the flash copy is written when no recording is running, because writing
NVS stops the flash cache and the CAN interrupt is reached through a dispatcher
that may not be resident in IRAM. Losing power in between costs nothing — the
card is correct and the boot rule imports it.

### The Bus tab

![The Bus tab](docs/img/bus-view.png)

**Live Signals** — every signal the frame map describes, with its current value
and unit. **Identifiers on the wire** — every id seen since the recording
started, its most recent payload, frame count, rate, and whether the frame map
describes it. This is the tab for building a frame map, and the one to look at
when a dashboard cell says a signal is missing.

### With no frame map

Without a DBC the page does not pretend: there are no named signals to put on a
dashboard, the Bus tab becomes the live view showing raw payloads as they
change, and the Dashboard is health and controls.

![The dashboard with no frame map](docs/img/dashboard-raw.png)

Like the firmware, the page names nothing of its own. Every widget, every unit
and every range comes from the file on the card. Nothing about the page changes
when you swap buses — only that file does.

### Preview it with no hardware

```bash
# the real page, against simulated data - opens on http://127.0.0.1:8080.
# Starts from examples/machine.dbc and the text of examples/dash.cfg, so there
# is a dashboard and a set of sendable values to look at from the first second.
# It writes no file at all.
python3 tools/preview_dashboard.py

# start from a setup of your own. It is READ, never written - use Export in
# the page to get your changes back out.
python3 tools/preview_dashboard.py --cfg examples/dash.cfg

# a logger with nothing on its card, which is a different page
python3 tools/preview_dashboard.py --empty --no-dbc

# one frame map per bus, as on the logger. Either can also be uploaded from
# the page, from its own Frame map button.
python3 tools/preview_dashboard.py --dbc examples/machine.dbc \
                                   --dbc2 examples/example.dbc

# what CAN<n>_AUTODETECT looks like on screen, without a bus to detect
python3 tools/preview_dashboard.py --autodetect found     # or: fallback
```

It prints what it loaded — `setup: 12 dashboard cells, 4 sendable values`, then
a line per bus — so an empty page is never a mystery.

### Setting it up for your own bus, at a desk

No logger, no wiring, no traffic. All you need is your `.dbc` and Python.

```bash
python3 customize.py                                   # load the .dbc in the page
python3 customize.py path/to/mine.dbc                  # or start with it
python3 customize.py path/to/mine.dbc --role Tester    # if one of them is you
                                                      # (CAN 1's; CAN 2's is a
                                                      #  button in the page)
```

**It never asks a question in the terminal.** With no argument the page opens
empty and its own **Frame map: CAN 1** button loads a `.dbc` from wherever you
keep it — the same button the logger itself has, so there is one way to do this
rather than two. It holds one map per bus, as the logger does, so **Frame map:
CAN 2** loads CAN 2's and neither disturbs the other; `--dbc2` names it on the
command line.

or **double-click `customize.py`** and pick your file — from the list it finds,
or press **b** to open your computer's own file browser. On Windows,
`customize.bat` does the same and you can drag a `.dbc` straight onto it.
`--browse` goes straight to the file dialog.

That one command:

- checks the file against the limits this firmware was built with, and says what
  it could not read
- picks a free port, starts the page and opens your browser at it
- reads `mine.cfg` if one is already sitting beside your `.dbc` — and never
  writes it back

**It writes nothing you did not ask for.** No file appears next to the `.dbc`
you opened, and loading a second frame map in the same session leaves the first
one's work alone rather than pairing a new file with it. When the setup is worth
keeping, press **Export** in the page and put the downloaded file where you want
it. This tool used to pair a `.cfg` with every map it was shown, which left files
in the directories it was pointed at and opened the page on setups nobody had
asked for.

Then, in the page:

**1. Load your frame map** if you did not name one on the command line —
*Frame map: CAN 1*, in the header.

**2. Say which node you are** — or skip. *Role: CAN 1* / *Role: CAN 2*, in the
header, one per bus. See [which node this logger is](#which-node-this-logger-is);
`--role` above answers CAN 1's before the page opens, and the buttons change
either afterwards. Skip if you are only
listening, and both Fill buttons will offer everything.

**3. Build the dashboard.** *Customize dashboard* → **Fill from frame map**. Every
signal becomes a cell, drawn as its unit and name suggest. Delete what you do not
want, drag the rest into order, tap any cell to change the shape, the range or the
thresholds. Removing one closes the gap. The values moving on them are invented —
the point is the layout.

**4. Build the sendable values.** Send tab → *Set up sendable values* → **Fill from
the frame map**, and pick the message your controller takes its settings from.
Then fix the inputs: the one that should be a list of four tyre sizes becomes
*Pick from a list I write*.

**5. Take it with you.** Press **Export** — in the header, behind *Setup file* —
and copy two files to the root of the SD card:

```
mine.dbc        ->  /frames.dbc
the export      ->  /dash.cfg
```

Power the logger up and it opens on your dashboard, with your sendable values,
having never been connected to a bus during any of it.

#### Checking a frame map on its own

```bash
python3 tools/check_dbc.py path/to/mine.dbc          # counts, limits, direction
python3 tools/check_dbc.py path/to/mine.dbc --list   # every message and signal
```

It names the lines it could not read, warns if the file overruns
`DBC_MAX_MESSAGES` / `DBC_MAX_SIGNALS` / `DBC_MAX_VALDESC`, flags definition
lines longer than `DBC_LINE_MAX` (which lose everything past the cut), and prints
**who sends what** and which messages are multiplexed, payload by payload.

Two things it warns about are worth acting on before you go out. Signals of a
*plain* message that share bits — almost always a frame that multiplexes in fact
and does not say so, which the logger would otherwise send as one frame with the
payloads written over each other. And a signal of a *multiplexed* message with
no code on it, which this build cannot send at all — see [known
issues](#14-known-issues).

It is pure Python — no compiler, no shell — which means it is a *second*
implementation of `src/dbc.cpp` and would normally be a reason to distrust it.
So `test/run_tests.sh` compiles the real parser and asserts the two agree,
message for message and signal for signal, on every frame map in `examples/`.
That test has already earned its place: it caught the Python reader reading a
factor of `1e-06` as zero decimal places, which would have shown different
digits at a desk than in the field.

The page is extracted straight out of `src/webpage.cpp` on every request, so the
preview can never drift from what the firmware serves, and editing the page and
pressing refresh just works. **The data is invented** — the point of the tool is
the interface, not the numbers. Layout changes are kept in memory and written
back to `--cfg` if one is given, so a dashboard worked out here can be copied
straight to the SD card.

**The invented values follow the range you give a cell.** Narrow a gauge from
`[0|4294967295]` down to 88–92 — the useful band around a steering-angle filter
where 90 is straight — and the needle sweeps 88 to 92, instead of sitting pegged
at the top of a scale nothing will ever reach. The value is remapped from the
range the frame map declares into the range you chose, so it is the same motion
expressed in your scale, and the Bus tab agrees with the dashboard about where
the signal is running.

The screenshots above are generated from it:

```bash
python3 tools/capture_screenshots.py     # regenerates docs/img/*.png
python3 tools/capture_gifs.py            # regenerates the two walkthroughs
```

Re-run that after changing the page, or this section will document a dashboard
that no longer exists.

### Why plain polling

It is a plain `WebServer` with polling rather than an async server with
websockets, on purpose: no third-party libraries, so the PlatformIO and Arduino
IDE builds are the same code with no install steps. The HTTP handler runs at the
lowest priority and never touches the SD card or the CAN controller, so a
browser hitting refresh cannot perturb a recording.

The dashboard's request carries only the cells that exist, as text the decode
task had already rendered — so the fast path copies strings and does no
decoding, no formatting and no floating point. The two expensive tables live on
the Bus tab, and are built only when that tab is open.

The page itself is ~122 KB of HTML, CSS and hand-drawn SVG in one document with
no external assets, streamed from flash to the socket a chunk at a time so it is
never assembled in RAM. It has to load from a hotspot with no route anywhere,
which rules out every CDN and therefore every gauge library there is.

---

## 7. Sending values back to the bus

> **Read this before enabling it on a machine.** A CAN frame sent to a live
> controller can move hydraulics, release a brake or enable a drive. The logger
> sends what it is told and cannot know which. Everything below is about making
> that deliberate rather than easy.

Everything else in this firmware listens. This is the one part that talks: it
writes a value into an ECU — a tyre size, a limit, a calibration offset — **while
a recording is running**, so the change and its effect land in the same file.

![Setting up what can be sent](docs/img/sending.gif)

*Saying which node the logger is, filling the sendable values from the frame map
— only what that node sends — arming, and sending. The frame it writes carries
`Command = 32` because the `.dbc` says that is the opcode `WheelDia_mm` belongs
to.*

![The Send tab](docs/img/send-values.png)

### Set the values up first, use them in the field

The whole point is that nobody types a number next to a running machine. At the
desk, press **Set up sendable values**.

<img src="docs/img/send-setup.png" alt="Setting up the values that can be sent" width="620">

Start with **Fill from the frame map**. Choose one message — or
**every message this logger sends**, which is usually what you want, since a bus
has one or two command frames and no reason to add them one at a time. Every
signal in them becomes a value you can send, with the input guessed from the file — a signal with `VAL_` names
becomes a list, a one-bit signal becomes two states, a signal with a declared
range becomes a slider, anything else a number box. On
[`examples/machine.dbc`](examples/machine.dbc) that turns `MachineConfig` into
its four signals in one press.

Then adjust each one: which signal it writes, and *how the value is picked*.

| Input | For |
|---|---|
| **Pick from a list I write** | a tyre size that is one of the four your fleet uses; a gear ratio; anything with a small set of right answers |
| **Pick from the frame map's own names** | a signal with `VAL_` labels — the list comes from the DBC, so it cannot disagree with it |
| **Two states** | an enable, a mode flag, anything boolean, with your own labels rather than 0 and 1 |
| **Slider** | a continuous limit, stopping exactly where the signal does |
| **Type a number** | the fallback, not the default |

Ranges come from the frame map, and are then **cut to what the bits can actually
hold**: a 16-bit signal at factor 1 cannot carry 99999 however the file is
annotated, and a slider that goes further than the wire does would aim the
operator at a value that is silently clamped on the way out.

These live in the same `/dash.cfg` as the dashboard, so they travel with it.
[`examples/dash.cfg`](examples/dash.cfg) sets up four against
[`examples/machine.dbc`](examples/machine.dbc), including the tyre size:

```
send 0 label="Tyre size" sig=MachineConfig.TyreSize unit=mm lo=400 hi=1400 \
       preset=690 style=choice choices="620:620 mm|650:650 mm|690:690 mm|710:710 mm"
```

On the machine, the Send tab is then just: arm, pick, press.

### The unit is the frame

> A frame is one message, and — when the message is multiplexed — one selector
> code. The values of a frame are **added together, removed together and sent
> together, under one button.**

That is the whole rule. It is not a policy; it is what a CAN frame is. Eight
bytes leave the logger whether or not somebody typed all of them, so the page
refuses to pretend one signal of a frame can be sent by itself.

A message is **multiplexed** when the `.dbc` marks it with an `M` selector and
`m<code>` payloads, or when you say so by hand (below). Nothing is inferred from
the bit layout — overlapping signals are a hint a person can read, not a
declaration, and a guess here writes a real command to a real ECU.

So one message can hold several frames. Four shapes, and the boxes follow:

| | frames | what you get |
|---|---|---|
| plain, one signal set up | 1 | one row, one **Send** |
| plain, several signals set up | 1 | one box, **Send all *N*** and **Remove all *N*** |
| multiplexed, one signal per code | one per code | an outer box for the message, one sub-box per code, each with its own Send and Remove |
| multiplexed, several signals under a code | one per code | the same, and the sub-box holding two signals sends and removes both |

On [`examples/example.dbc`](examples/example.dbc) as `NodeA`: `NodeStatus` is
one box with **Send all 4**; `Diagnostics` is an outer box holding **Page = 0**
(`SupplyVoltage` + `BoardTemp`) and **Page = 1** (`RunHours` + `ErrorCount`),
each with its own button. You cannot have `SupplyVoltage` without `BoardTemp`,
because one frame under `Page 0` carries both fields and sending one alone would
assert a value for the other that nobody chose.

The setup sheet and the Send tab draw their boxes from the same function, so a
box in one and a button in the other cannot mean different things.

![Values that leave together](docs/img/send-groups.png)

**Whole frames only.** Half a frame set up is a frame that still goes out, with
the signals nobody configured as zeros — a command that looks complete and is
not. So choosing one signal by hand brings its whole frame with it; a frame that
will not fit in the remaining room is not added at all rather than added in
part; and a setup file written before this rule, or carried across to a frame
map where the message has gained a signal, is completed on load and told to you.

**The selector is written for you.** A command frame usually carries an opcode
and a payload whose meaning depends on it:

```
BO_ 288 HostCommand: 8 Host
 SG_ Command M         : 0|8@1+  ...     the selector
 SG_ WheelDia_mm m32   : 8|16@1+ ...     only means "wheel diameter" under op 32
```

Writing `WheelDia_mm` on its own would arrive as opcode 0 and be thrown away, so
the logger writes the selector itself, with the code the frame map says belongs
to the signal being sent — inserted as raw bits, because a mux code is a bit
pattern by definition and has no scaling of its own. It is never offered as a
value to set up: it is not a decision anybody should have to get right twice.
The row that needs it says *sent with Command = 32*.

That example is in [`examples/example.dbc`](examples/example.dbc), and
`test_encode.cpp` builds the frame from it and reads it back.

A multiplexed frame is also the one case where the payload is **not** seeded
from what the bus last said: the bytes mean different things under different
codes, so carrying a previous code's bytes forward would send garbage.

### Saying a message is multiplexed when the file will not

Some `.dbc` files multiplex in fact and do not admit it: payload signals sitting
on the same bits, an opcode byte in front of them, and no marker anywhere. Left
alone, the logger sends all of them in one frame, writing them over each other.

In the setup sheet the message header offers a picker: **which signal selects**.
Choose it and the selector drops out of the value list — it is written for you
from then on — and each remaining value gains a **selector code** field. Both
halves are needed and neither can be skipped: knowing that `Cmd_Op` selects does
not say that `Cmd_Amp` means opcode 16.

It is recorded on the `send` lines as `msel=` and `mxc=`:

```
send 0 label="Cmd val" sig=ABS_Cmd.Cmd_Val ... msel=Cmd_Op mxc=1
send 1 label="Cmd amp" sig=ABS_Cmd.Cmd_Amp ... msel=Cmd_Op mxc=16
```

An override is dropped rather than half-obeyed if a later frame map has no
signal of that name in the message, or declares its own `M` — the file wins,
because the file is the better place to say it. `tools/check_dbc.py` reports the
overlapping signals that suggest you need this, and **fixing the `.dbc` is still
the better answer**; this is for the file you have rather than the file you want.

**One press, one frame.** Everything in a box is one message under one selector
code, so it is one request and one frame. Within it the values are queued back
to back with every one but the last held: each writes into the frame under
construction and only the last transmits, and the queue is drained by a single
task, so nothing can slip in and split it. `ABS_Cmd` with six opcodes is six
boxes and six buttons, and each press puts one frame on the bus.

**What you type stays typed.** The Send tab redraws whenever anything changes —
arming, a poll, a frame arriving — and each box keeps the values in it across
those redraws and after sending, which for a multiplexed frame is the part worth
keeping: the whole frame is written every time, so the values you did not touch
are as much a part of the command as the one you did. **Reset** on each box puts
it back to the values it was set up with, and so does reloading the page.
Nothing about this is stored on the logger.

### Arming

Every Send button is dead until you press **ARM TRANSMIT**, and the logger
disarms itself again after `TX_ARM_TIMEOUT_MS` (five minutes) without a send.

<img src="docs/img/send-pinned.png" alt="The arm bar pinned to the top of a phone screen" width="300">

Once the permission card scrolls away, a compact copy pins itself to the top of
the screen, so **DISARM is always one press** however long the list of values is.
It is fixed to the viewport rather than sticky in the flow, so nothing moves
when it appears — which matters, because it appears while somebody is reaching
for a Send button.

This is not security — anyone on the hotspot can arm it. It is what stops a
stray tap, a bookmarked page, or a browser restoring its tabs from sending a
command nobody meant to send. It expires by itself because a gate that stays
open is not a gate. Arming, disarming and every frame sent are written to the
recording's `.log`.

The gate is enforced **where the sending happens**, not in the browser and not
in the HTTP handler: a repeating value stops the moment the gate closes.

### What ends up in the recording

An MCP2515 does not hear its own transmissions, so a frame the dashboard sent
would otherwise be missing from the very file it was sent during — and a
setpoint whose effect you can see but whose cause you cannot is worse than
useless. Sent frames are therefore fed back into the recording, decoded the same
way as everything else, with the message name prefixed:

```
t_us;bus;id;name;signal;value;unit;raw
1042318;2;0x110;TX:MachineConfig;TyreSize;690;mm;
1042994;1;0x100;Drive;GroundSpeed;12.4;km/h;
```

**The schema is unchanged by this feature** — a sent frame is an ordinary row
with a prefixed name, and it carries the bus it went out on like any other. A
reader that does not know about the prefix simply sees a message called
`TX:MachineConfig`. `tools/parse_log.py` does know about it — it reports sent
frames separately, marks them in `--list`, and groups them with the signal they
wrote in `--wide` rather than making a second, nearly-empty column.

**A setpoint remembers which bus it belongs to.** That is stored with the value,
not inferred from wherever the identifier happens to appear, because sending a
command down the wrong wire is not a cosmetic error. The Send tab shows the bus,
the setup sheet chooses it, and the one-off frame box has its own selector.

### What happens on the wire

`MCP2515::sendFrame()` puts the controller in **one-shot mode**, which is not
the obvious choice and is the important one.

Left to itself an MCP2515 retries an unacknowledged frame forever, and each
failed attempt adds 8 to the transmit error counter. At 250 kbit/s, a frame sent
to an ECU that is not there drives TEC from 0 to 255 in about **30 milliseconds**
and the controller goes **bus-off** — which stops it *receiving* too. A logger
that goes deaf because somebody pressed Send on a disconnected bus is a worse
logger than one that cannot send at all.

One-shot attempts the frame once, so TEC moves by 8 and the failure is reported
instead of escalating. Losing arbitration is retried in software up to
`TX_ATTEMPTS` times, because on a busy bus that is normal and is not a failure.
The answer comes back as one of:

| | |
|---|---|
| **sent and acknowledged** | at least one other node ACKed it, and TEC went *down* — which is the independent evidence, not just a status bit |
| **nothing on the bus acknowledged it** | the ACK slot stayed empty. The most common reason a Send does not work, so it gets its own answer rather than a generic bus error |
| **the bus was too busy to get on** | lost arbitration every attempt |
| **listen-only mode** | `CAN1_LISTEN_ONLY` / `CAN2_LISTEN_ONLY` is set for that bus, so the logger physically cannot drive it. Said out loud, rather than failing quietly |

### Encoding

A value is placed into its message by `dbcEncodeSignal()`, written as the mirror
image of the decoder and reusing its bit walk, so `decode(encode(x)) == x` for
every signal the logger can read. `test/test_encode.cpp` asserts exactly that by
sweeping **every raw value** each signal can hold — a few thousand per run — and
comparing the payload byte for byte. A transmit path that is subtly not the
inverse of the receive path is the kind of bug that shows up as a machine doing
the wrong thing, not as a wrong number on a screen.

Two details that matter:

- **The message's other signals are preserved.** The payload starts as the last
  thing the bus said that message contained, and only the target signal's bits
  are changed. Zeroing the rest would command every other signal in the message
  to zero as a side effect of setting one.
- **Out of range clamps, and says so.** Wrapping 70000 mm into 16 bits would put
  4464 mm on the wire and nobody would ever know.

### Turning it off entirely

Set `CAN1_LISTEN_ONLY` or `CAN2_LISTEN_ONLY` to `1` in `src/config.h`. That
controller then physically cannot drive its bus, and the Send tab says so
instead of appearing to work. Per bus, deliberately: a diagnostic bus you may
drive often sits next to a live bus you may not.

---

## 8. Why it does not lose frames

This is the part worth copying if you build something else, and the part the
second bus put under real pressure.

### The deadline

An MCP2515 holds **exactly two frames**. A third arriving before firmware has
read the first two is gone, and only a sticky bit in `EFLG` records that it
happened. That is the constraint the whole design answers to, and at 500 kbit/s
it is tight:

| | |
|---|---|
| Shortest frame (11-bit id, DLC 0) + 3-bit inter-frame space | ~50 bits → **100 µs** |
| Typical 8-byte frame, worst-case bit stuffing | ~128 bits → **256 µs** |
| **Time to fill both receive buffers on one controller** | **~200 µs** |
| Sustained rate, two buses at 500 kbit/s and 80 % load | **~6 250 frames/s** |

Against that, SD block writes stall for **~320 ms** on a bad card. Polling cannot
bridge a gap of six thousand to one. Neither can "do the work in the interrupt" —
an SPI transaction can block, and blocking in an interrupt handler is how frames
get lost.

So the interrupt does the **minimum bounded work** and nothing else:

| Stage | Who | What, and why |
|---|---|---|
| INT falls | `canIsr1` / `canIsr2`, in IRAM | Takes the arrival timestamp with `esp_timer_get_time()`, pushes it into **that bus's** ring, and unblocks the reader task. **No SPI, no allocation, no file I/O, no logging.** Constant work, whatever either bus is doing. |
| drain | **one** CAN task, prio 20 | Empties both receive buffers on controller 1, then controller 2, and keeps going until each reports empty — that is what makes the edge-triggered INT safe when a second frame arrives while INT is still low. A 20 ms timeout re-drains unconditionally, so even a completely missed edge costs latency, never data. |
| buffer | 2048-frame queue | 48 KB ≈ 330 ms of slack in front of the SD card, sized against the card's worst stall rather than against the bus. |
| decode + write | writer task, prio 10 | Looks the id up in **that bus's** frame map, decodes each signal, formats CSV, fills a 32 KB block, writes it. While blocked in that write the reader simply preempts it. |
| Wi-Fi / HTTP | core 0 and `loop()` | Lowest priority, on the other core. Cannot interfere with any of the above. |

The timestamp is taken **in the ISR**, before any queuing or scheduling delay can
smear it — which is why `t_us` is trustworthy even when the writer is 200 ms
behind. Both buses share that one clock, so rows from CAN1 and CAN2 are directly
comparable. Everything expensive (DBC lookup, bit extraction, scaling, decimal
formatting, SD I/O) happens on the far side of the queue, where being slow costs
queue depth instead of frames.

### One task drives both controllers, deliberately

Two controllers share VSPI. The obvious design — a task each — is the wrong one.
Espressif's own guidance is that the SPI driver is thread-safe across different
devices on a bus but that each bus is cleanest driven from a single task, and the
Arduino `SPIClass` guards it with a FreeRTOS mutex. Two reader tasks would add
mutex contention and a priority-inversion surface on the **one path with a hard
deadline**, in exchange for nothing: the work is identical either way.

One task, two devices, no contention — and a worst case you can compute.

### Reading a frame in one block, not fourteen

At one bus this was invisible slack. At two it is the budget.

`readFrame()` issues `READ STATUS` then the burst `READ RX BUFFER` command — the
right instructions, 16 bytes total, ~13 µs of clock at 10 MHz. But
arduino-esp32's `SPIClass::transfer(uint8_t)` is a **complete peripheral round
trip** per byte: load W0, set the length registers, raise USR, poll for done.
Roughly 2–3 µs of overhead against 0.8 µs of wire time.

| | Per frame | Worst case: drain 2 controllers × 2 frames | vs the 200 µs deadline |
|---|---|---|---|
| One `transfer()` per byte | ~45 µs | ~185 µs + ~60 µs pass overhead ≈ **245 µs** | **misses** |
| One `transferBytes()` per transaction | ~18 µs | ~75 µs + ~40 µs ≈ **115 µs** | **~40 % margin** |

So every register access and both frame paths go through a single block
transfer (`MCP2515::xfer`, `src/mcp2515.cpp`). Sustained cost is not the issue
either way — 6 250 frames/s × ~18 µs is about **11 %** of one core — but the
worst case is, and the per-byte version does not close it.

### The margin is measured, not asserted

Because that budget is the claim, the logger times itself. `drainMaxUs` is the
worst microseconds any single pass spent emptying **both** controllers, and it is
on the dashboard, in `/api/status` and in the `health:` line:

```
health: queue=61 peak=124 drop=0 drain=112 us wake=51/s writes=38 maxWr=41200 us ...
```

Under 120 µs is comfortable. Approaching 200 µs means the margin is gone — check
`CAN_SPI_HZ`, and check nothing has been added to the reader task. A figure you
can read beats a design note you have to trust.

### Where frames can still be lost, and how you know

Both remaining loss points are **counted and reported**: each controller's own
overflow flags (stage 1→2, per bus) and the shared queue-full counter (stage
2→3). A recording that ends with `lost 0` is provably complete on both buses —
that is what the *Data Integrity* card shows.

**A non-zero figure is a floor, and says so.** The queue-full counter is exact:
it counts frames. The controllers' overflow flags are not — `EFLG` carries one
sticky bit per receive buffer, so a service pass that finds them set knows *that*
frames were lost, never how many. So the page reads `≥ 412 LOST`, breaks it down
per bus, and the log separates `ovfEvents` from `ovfFrames>=`.

That distinction was worth drawing. The counter used to be incremented once per
service pass, which in a fault that pinned the receiver made `lost` converge on
a flat ~51/s — a poll rate wearing a loss figure's clothes. Checked against the
4-bit rolling counters the bus itself carries, the true loss in three affected
recordings was roughly **1.7× what was reported**.

*(Those figures come from field recordings on the single-bus logger this grew
out of, measured with tooling outside this repository. They are the reason for
the change, not something a clone can re-check — unlike every number under
[Verification status](#13-verification-status), which is.)*

| recording | reported | actual, from rolling counters |
|---|---|---|
| A | 265 983 | 456 814 |
| B | 271 305 | 478 986 |
| C | 28 038 | 48 845 |

The floor it now reports is still below the truth. It is at least honest about
which direction it is wrong in.

### The honest limit: the writer, not the capture path

The capture path closes with margin. **The write path can be exceeded**, and
this is the thing to know before you trust a recording.

The CSV is one row per decoded *signal*, not per frame. At ~6 250 frames/s:

| | Rows/s | at ~45 B/row | |
|---|---|---|---|
| No frame map (raw rows only) | 6 250 | **~280 kB/s** | fine on SPI SD |
| Both maps loaded, ~4 signals/frame | 25 000 | **~1.1 MB/s** | **beyond the card** |
| Both maps loaded, ~8 signals/frame | 50 000 | ~2.2 MB/s | far beyond it |

A well-behaved SPI SD card sustains roughly 0.5–1 MB/s with large block writes.
So two fully-mapped, genuinely busy 500 kbit/s buses can outrun the writer — and
when they do, it shows up as `qDrop`, which is **exact**. Nothing is hidden and
nothing is estimated: you get a count of frames lost and the `maxWr` figure that
explains why.

If you hit it, in order of effect: record raw and decode offline with
`tools/parse_log.py`; trim the frame maps to the messages you actually need;
raise `FRAME_QUEUE_LEN` if the stalls are bursty rather than sustained; use a
faster card. What you should *not* do is assume it cannot happen because the
capture path is sound — those are two different budgets.

---

## 9. Surviving a power cut

Two different things have to reach the card, and only the second one makes a
recording readable again:

1. the rows still in the **RAM block buffer** (8 KB);
2. the **FAT and directory entry**, which is what records the file's new
   *length*. Bytes handed to `write()` may already be physically on the card, but
   until the metadata is committed the file still reports its old size, and a
   remount truncates everything written since.

Point 2 is the one that catches people out: the exposure window is set by
`SD_SYNC_INTERVAL_MS`, **not** by the buffer size.

| | Worst-case loss |
|---|---|
| Default, no extra hardware | **≤ 1 s** (`SD_SYNC_INTERVAL_MS`) |
| With the power-fail input wired | **0** — files are flushed and closed before the capacitor runs out |

A sync rewrites 2–3 sectors; at 1 Hz that is well under 1 % duty and the frame
queue absorbs it completely. The CSV header is committed immediately at start, so
even a recording cut short a moment later leaves a valid, self-describing file.
Measured sync cost and the current exposure window are reported once per second
into `N.log`:

```
health: ... syncs=142 maxSync=8113 us atRisk<=340 ms ...
```

### Getting to zero

Set `PIN_POWER_FAIL` in `src/config.h` (default `-1`, disabled) and wire:

- the logger fed from a **bulk capacitor behind a diode**, so it keeps running
  briefly after the supply drops;
- a **divided copy of the upstream supply** into `PIN_POWER_FAIL`.

When the supply collapses the pin changes state while the capacitor still holds
the ESP32 up. The ISR only raises a flag — opening, flushing and closing files
are all forbidden from an ISR — and the writer task, which checks that flag
before anything else on every pass (≤ 20 ms), flushes the buffer, commits the
metadata and closes both files.

Sizing: the emergency close wants ~50 ms. Holding 150 mA for 50 ms across a
5 V → 3.6 V droop needs `C = I·t/ΔV = 0.15·0.05/1.4 ≈ 5400 µF`, so a
**6800 µF / 10 V** electrolytic is a sensible choice.

After a power-fail close the logger does **not** silently reopen — that would
hide the event and produce a second file with its first rows missing. The
dashboard shows **POWER LOSS** in red until someone presses START.

Leave `PIN_POWER_FAIL` at `-1` if you have no such circuit: an unconnected pin
would float and fire spuriously.

---

## 10. Host-side tools

Standard library only, except the plotter and the GIF recorder (`matplotlib`
and `websocket-client`). **All of them are Python**, so they
run the same on Windows, macOS and Linux with nothing installed — that is why
there are no shell scripts here. (`test/run_tests.sh` and `arduino/sync.sh` are
for contributors on Unix; the Arduino sketch folder has `arduino\sync.bat` for
Windows.)

```bash
# lay the dashboard and the sendable values out for your own bus, at a desk.
# Double-clickable; on Windows use customize.bat, or drag a .dbc onto it
python3 customize.py path/to/mine.dbc

# will this frame map load on the logger?
python3 tools/check_dbc.py path/to/mine.dbc --list
```

```bash
# summary, per-id rates, and an integrity check
python3 tools/parse_log.py 1.csv

# which signals are in this recording
python3 tools/parse_log.py 1.csv --list

# pivot the long form to one column per signal, for pandas/Excel
python3 tools/parse_log.py 1.csv --wide wide.csv

# plot (needs matplotlib); signals sharing a unit share an axis
python3 tools/plot_log.py 1.csv 'MotorFeedback.*' -o out.png

# the same page directly, when you want the arguments rather than the prompts
python3 tools/preview_dashboard.py --dbc examples/example.dbc --cfg mine.cfg

# when a board will not enter download mode - see WINDOWS.md
python tools/esp32_reset_probe.py COM3
```

### Tests

```bash
./test/run_tests.sh
```

Compiles the **real** sources from `src/` against small shims in `test/shim/`
and runs them natively — the DBC parser (Intel and Motorola layouts, signed
values, factors and offsets, value tables, multiplexing, IEEE floats, counter
unwrapping, malformed input, table overflow), the CSV schema in both modes, the
CANopen framing layer, and the logger. Also type-checks every module under
`-Wall -Wextra`, parses `examples/example.dbc`, checks that every DOM id and
status field the dashboard's JavaScript touches actually exists, and verifies
the Arduino sketch generator.

The shims are not the real Arduino/ESP-IDF APIs, so they cannot catch an SDK
signature mismatch — only a real `pio run` does that, which is why CI does both.

> `test/shim/Arduino.h` deliberately reproduces the Arduino core's **object-like
> macros** (`HEX`, `DEC`, `BIN`, `PI`, `bit()`, `sq()`, …). Do not remove them to
> tidy up. A shim that omits them compiles code the real toolchain rejects, with
> errors that point nowhere near the cause — a `static const char HEX[]` array
> becomes `static const char 16[]`.

---

## 11. Tuning

All in `src/config.h`:

**Per bus** — the two are independent, because two buses on one machine rarely
run the same way and two MCP2515 modules rarely carry the same crystal:

| Setting | Default | |
|---|---|---|
| `CAN1_BITRATE_KBPS` / `CAN2_BITRATE_KBPS` | 250 / 250 | 100 / 125 / 250 / 500 / 1000, per bus |
| `CAN1_CRYSTAL_MHZ` / `CAN2_CRYSTAL_MHZ` | 8 / 8 | Must match **that** MCP2515 board — check them separately |
| `CAN1_AUTODETECT` / `CAN2_AUTODETECT` | 0 / 0 | 1 = find that bus's bit rate at boot instead of trusting the two settings above. See [Finding the bit rate itself](#finding-the-bit-rate-itself) |
| `CAN_AUTODETECT_MS` / `CAN_AUTODETECT_FRAMES` | 300 / 2 | How long to listen at each candidate, and how many frames have to decode before it counts |
| `CAN1_LISTEN_ONLY` / `CAN2_LISTEN_ONLY` | 0 / 0 | 1 = never drive that bus |
| `CAN2_ENABLED` | 1 | 0 = run on single-bus hardware; the CSV keeps its bus column and always says `1` |
| `DBC_PATH` / `DBC2_PATH` | `/frames.dbc` / `/frames2.dbc` | Where each bus's frame map lives |

**Shared** — one queue, one card, one writer:

| Setting | Default | |
|---|---|---|
| `CAN_SPI_HZ` | 10 MHz | Both controllers. The datasheet maximum, and the drain budget assumes it |
| `DBC_MAX_MESSAGES` / `DBC_MAX_SIGNALS` | 256 / 1024 | Ceilings **per map**; the tables are sized to your file |
| `DBC_HEAP_RESERVE` | 90 KB | Heap **both** maps together will not take, so Wi-Fi still starts |
| `CANOPEN_DECODE` | 0 | Label unmapped ids CANopen-style |
| `CSV_INCLUDE_RAW` | 0 | Keep the payload on decoded frames too |
| `SD_BLOCK_BYTES` | 32768 | Bytes per SD write. Raised from 8 KB for the second bus — large writes are far more efficient on SD |
| `SD_SYNC_INTERVAL_MS` | 1000 | The power-cut exposure window |
| `PIN_POWER_FAIL` | -1 | See §9 |
| `FRAME_QUEUE_LEN` | 2048 | ≈330 ms of slack at 6 250 frames/s — sized against the SD card's worst stall, not the bus. **Raise this first** if `drop` is non-zero while `maxWr` shows a long write |
| `BUS_TRACK_IDS` / `WEB_MAX_SIGNALS` | 24 / 48 | Dashboard table sizes, **per bus** |
| `AUTO_START_RECORDING` | 1 | Record from power-on |
| `ENABLE_OTA` | 1 | Wi-Fi flashing |

Plus the pin map, task priorities and cores, and the Wi-Fi fallbacks.

> **`FRAME_QUEUE_LEN` costs 24 bytes an entry** and is static RAM, which is the
> binding constraint on this chip (see
> [§13](#13-verification-status)). 2048 entries is 48 KB of the 328 KB the chip
> has. Doubling it again is possible; tripling it is not.

---

## 12. Troubleshooting

<details>
<summary><i>expand</i></summary>
<br>


| Symptom | Cause |
|---|---|
| `CAN1 CONTROLLER NOT RESPONDING` (or CAN2) | That module's wiring or power. The driver verifies the SPI link both ways at boot, so this means **its** CS, the shared MISO/MOSI, or 3V3 is wrong. The message names the bus and its pins. |
| One controller answers, the other does not | If a second module **is** fitted: almost always the chip select — that is the only line that is not shared. Check the pin the message names, and that the two CS wires are not swapped. If one **is not** fitted, this is expected; see [§1](#running-it-with-one-bus). |
| `NO CAN TRAFFIC ON EITHER BUS` | Both wrong at once is usually the shared wiring: SCK/MISO/MOSI, or 3V3. |
| `NO CAN TRAFFIC` on one bus only | Wrong `CAN1_CRYSTAL_MHZ` / `CAN2_CRYSTAL_MHZ` for **that** module (8 vs 16), wrong bit rate for that bus, or that bus's CAN_H/CAN_L swapped. Two modules from one order can carry different crystals. Set `CAN<n>_AUTODETECT 1` and let it find the rate — see [Finding the bit rate itself](#finding-the-bit-rate-itself). |
| `CAN2 bit rate not detected` (or CAN1) | Detection heard nothing decodable at any rate. Usually there is simply no traffic — a quiet bus looks exactly like a wrong bit rate — so check a node is talking before doubting the rate. Wiring and the crystal are the other two. The `config.h` values are used meanwhile. |
| `decoded only with a 16 MHz crystal, not the 8 MHz in config.h` | The bus reads fine, but the reported rate is scaled: a crystal and a bit rate multiply. Fix `CAN<n>_CRYSTAL_MHZ` to the crystal actually on that module and the rate in the log becomes the real one. |
| `There is no pin marked D17` | Correct — that DevKit labels UART2 by function. CAN2's INT goes to the pin silkscreened **TX2**. |
| `CAN2 INTERRUPT NOT FIRING` (or CAN1) | That bus's INT wire. Frames still arrive on the 20 ms fallback poll, which caps at ~100 frames/s — so this looks like "it works but slowly", which is why it is called out by name. |
| `drain` approaching 200 µs in `N.log` | The margin in [§8](#8-why-it-does-not-lose-frames) is gone. Check `CAN_SPI_HZ` is 10 MHz and that nothing has been added to the reader task. |
| Everything logs as raw bytes on one bus | No frame map for that bus — `/frames.dbc` for CAN1, `/frames2.dbc` for CAN2 — or it has no `BO_` lines. The boot log says which, per bus. |
| The same id decodes differently per bus | Working as intended. Each bus is decoded against its own map; that is the whole reason they are separate files. |
| One message logs as raw, the rest decode | Its identifier is not in the DBC — or it is, but with the 29-bit flag set/unset differently. |
| `N line(s) of /frames.dbc could not be parsed` | Check the `.log` file. Interleaved `SG_` blocks and unsupported constructs are the usual causes. |
| Values look scaled wrong | Factor, offset or start bit in the DBC. Set `CSV_INCLUDE_RAW = 1` and check the payload against the decoded value. |
| `the frame map did NOT fit` | It says what it kept. Either the file passed a ceiling in `config.h`, or it wanted more heap than `DBC_HEAP_RESERVE` leaves. Frames beyond it are still recorded, as raw bytes. |
| `lost` climbing, `qDrop` non-zero | The writer cannot keep up. Slow SD card, or two fully-mapped busy buses — see the throughput ceiling in [§8](#8-why-it-does-not-lose-frames). Check `maxWr` in `N.log`, use a decent class-10, trim the frame maps, or record raw and decode offline. |
| `lost` climbing, `ovfFrames>=` non-zero on one bus | Frames lost **in that controller**, before the firmware saw them. Check `drain` and that bus's `irq` rate. |
| Last seconds missing after a power cut | Expected without the power-fail input — see §8. If `maxSync` is large the card is slow at committing metadata. |
| File exists but has only the header | Power was cut in the first second. The header is committed at start, so this is the floor, not corruption. |
| The other node goes error-passive | That bus's `CAN<n>_LISTEN_ONLY = 1` with nothing else on it to ACK. |
| `NO SD CARD at any clock ...` | First suspect **power**: most modules need 5V on VIN, not 3V3. Then: card must be FAT32 (cards over 32 GB often ship as exFAT), and CS=D4, SCK=D14, MISO=D27, MOSI=D13. |
| `SD card needed a slower clock` | Not an error — it mounted, just below `SD_SPI_HZ`. Long jumpers, a cheap adapter or a ribbon to a panel-mounted slot. Shorten the wiring if `lost` climbs; otherwise ignore it. |
| Dashboard unreachable | Check `mode` in `/config.txt`; on `sta` failure it falls back to the `CAN-Logger` hotspot. |
| `Wrong boot mode detected (0x13)` | Flashing, not running — see [WINDOWS.md](WINDOWS.md). |

---

---

</details>

## 13. Verification status

<details>
<summary><i>expand</i></summary>
<br>


Being straight about what has and has not been proven.

**Verified — the firmware compiles for real ESP32 hardware.**
`pio run -e esp32dev` against espressif32 7.0.1 / arduino-esp32 3.20017 /
xtensa-gcc 8.4.0 links a complete image, with **no warnings** from any of the ten
translation units under `-Wall -Wextra`:

```
RAM:   [===       ]  33.2% (used 108824 bytes from 327680 bytes)
Flash: [======    ]  57.1% (used 1123401 bytes from 1966080 bytes)
```

Flash sits at 57 % of one 1.9 MB app slot, so the OTA partition scheme still has
ample room; the web page accounts for about 125 KB of it. That RAM figure is
**static** memory, which is the one with a hard ceiling. It holds the 48 KB
frame queue and the 37 KB CSV staging buffer; both frame maps, the live values
and the saved dashboard are all on the heap and sized to what is actually
loaded.

The second bus cost **8.3 points of static RAM**, and nearly all of it is one
decision: `FRAME_QUEUE_LEN` going from 1024 to 2048 entries, which is 24 KB.
That is sized against the SD card's worst stall at the doubled frame rate, not
against the bus — see [section 8](#8-why-it-does-not-lose-frames). Adding the
bus itself was almost free, because `CanFrame` did not grow:

| | |
|---|---|
| `sizeof(CanFrame)` before | 24 bytes |
| `sizeof(CanFrame)` after, **with** a bus field | 24 bytes |

The struct had no padding left, so a plain `uint8_t bus` would have aligned it
up to 32 and cost a third of the queue. Packing the flags as bitfields
(`ext:1, rtr:1, tx:1, bus:1`) got it for nothing and changed no call site, since
those fields only ever held 0 or 1. A `static_assert` in `mcp2515.h` makes
adding a field a compile error rather than a silent 30 % increase.

**Static RAM is the binding constraint on this chip, not flash.** That is not a
guess: the first build of the dashboard overflowed `dram0_0_seg` by 2248 bytes,
and `DASH_MAX_CELLS` and `TX_MAX_COMMANDS` were cut to 36 and 10 to fit. Raising
them to 32 later failed the link again, by 3960 bytes.

Moving the big tables to the heap is what paid for the rest of this:

| | before | after |
|---|---|---|
| Static RAM | 37.8 % (123,872 B) | **24.9 %** (81,496 B) |
| Dashboard cells | 36 | 48 — the whole 6 × 8 grid |
| Sendable values | 10 | 32 |
| Frame map | fixed 64 msgs / 256 signals, 32 KB always | sized to the file, up to 1024 signals |
| Live values | fixed 5 KB always | sized to the map |

**32 sendable values is a hard ceiling, not a budget.** Whether a value is
repeating is carried as a bit in a 32-bit mask, in the firmware and in the
browser alike, and JavaScript's bitwise operators are 32-bit whatever you do to
them. Going past 32 needs a different representation, not a bigger number in
`config.h`.

**Verified — the portable logic, natively.** `./test/run_tests.sh` runs the DBC
parser, the signal encoder, the CSV schema, the CANopen layer, the MCP2515
driver, the saved dashboard and the logger, and all pass. That covers Intel and
Motorola bit extraction, signed values, exact decimal scaling, value tables,
multiplexing, IEEE floats, counter unwrapping, malformed DBC input, table
overflow, the **eight**-field row invariant, the config round trip, and the
page's DOM and JSON contract.

Four of those are new, and they are the ones that check the dual-bus claims
rather than restating them:

- **Two controllers really are two controllers.** `test_mcp2515.cpp` builds a
  simulated SPI bus with two register files and routes each transaction by
  **which chip select is low** — which is the only thing that distinguishes them
  in hardware. It asserts each gets its own bit timing, that filters-off and
  rollover-on are set on **both**, that a frame queued on one comes out of that
  one, and that a transmit on CAN1 never touches CAN2's transmit buffer. It also
  counts transactions that reached no chip or both; that count must stay zero.
- **The same identifier decodes differently per bus.** `test_decode.cpp` loads
  two different maps that both describe `0x100` and asserts the CSV rows differ
  accordingly — `NodeStatus.Uptime` on bus 1, `PumpState.Pressure` on bus 2 —
  and that an id only bus 1 knows falls back to raw on bus 2.
- **One clock across both buses.** The same test asserts the timestamp origin is
  taken from the first frame seen on **either** bus, so CAN1 and CAN2 rows share
  an epoch and are directly comparable.
- **A single-bus layout still means what it meant.** `test_dash.cpp` asserts a
  `dash.cfg` with no `bus=` anywhere loads as bus 1, serialises back with no
  `bus=` in its data lines, that an explicit bus survives a round trip, that an
  out-of-range bus is clamped, and that two layouts differing only by bus hash
  differently — otherwise flash would keep the wrong one.

Two of those are worth naming, because they check a property rather than a
handful of examples:

- **`decode(encode(x)) == x` for every raw value.** `test_encode.cpp` sweeps
  every value each signal in a deliberately awkward frame map can hold — a few
  thousand per run — and compares the payload byte for byte. A transmit path
  that is subtly not the inverse of the receive path is the kind of bug that
  shows up as a machine doing the wrong thing.
- **An unacknowledged frame does not reach bus-off.** `test_mcp2515.cpp` drives
  the real driver against a simulated controller that never ACKs, and asserts
  the frame is attempted **once** and reported, rather than retried into
  bus-off — which would take the receive path down with it.
- **The desk tools read a frame map exactly as the firmware does.**
  `tools/check_dbc.py` and the preview share one reader written in Python so
  they need no compiler; the test compiles `src/dbc.cpp` and asserts the two
  produce the same messages, signals, bit widths, decimal places, units,
  transmitters and multiplex codes for every file in `examples/`. It caught the
  Python reader treating a factor of `1e-06` as zero decimal places.
- **A real-sized frame map loads whole.** `test_dbc.cpp` builds a 104-message,
  707-signal map — the shape of the bus that exposed the old fixed tables — and
  asserts every message and every signal survives, that a small file gets a
  small table, and that a 27-character signal name is not clipped.
- **The per-identifier log line cannot be wrong.** `test_decode.cpp` asserts it
  writes an empty string when there is no traffic (it used to print
  uninitialised stack), never leaves a half-written entry (`snprintf`'s return
  value was being added blind), never runs past its buffer, and that its rolling
  window reports every identifier over successive lines rather than the same
  nine for ever.
- **A multiplexed command frame carries its own selector.** `test_encode.cpp`
  builds `HostCommand.WheelDia_mm` the way `cantx.cpp` does and reads the frame
  back: selector 32, payload 1380, selector still 32 after the payload is
  rewritten, and a clamp rather than a wrap past the bit limit.

**Verified — the web app, in a real browser.** The page is driven headlessly
against the simulator: customizing a dashboard, dragging cells, saving, reloading
from the stored file, arming, and sending. Every screenshot in this README is
generated by `tools/capture_screenshots.py` from that same page.

**Verified in the field — the single-bus receive path this grew out of.** Not by
this build: by its ancestor. The reader task's structure, the MCP2515 driver and
the controller configuration come from a firmware that recorded hours of a
250 kbit/s bus at ~540 frames/s with **zero frames lost**, across several
sessions on real hardware. Priorities, stack sizes, the SPI clocks, the sync
interval and the `RXB0CTRL`/`RXB1CTRL` bits are unchanged, and
`test_mcp2515.cpp` asserts the two that matter — filters off, rollover on — on
both controllers, so they cannot drift back without CI saying so.

Two things in that path **did** change for the second bus, and neither is field
proven: `FRAME_QUEUE_LEN` (1024 → 2048) and the SPI access pattern (one
`transfer()` per byte → one block transfer per transaction). Both are argued for
in section 8 and both are measurable on a bench.

That history is also what identified the one failure mode this repository has
seen. Ten recordings made here **before** the sticky-flag clearing landed
(`b2a9d18`) show four wedged interrupts and heavy loss; the older firmware
already had that clearing and never wedged. It is the same fix, in both, now.

**NOT verified — the second bus, on hardware.** No frame has been received from
two MCP2515s sharing one SPI bus on an actual board. Everything above is a host
test against a simulator, plus a build that links. Specifically unproven:

- **The 200 µs deadline, in practice.** The ~115 µs worst-case drain in
  [section 8](#8-why-it-does-not-lose-frames) is computed from the byte counts
  and arduino-esp32's per-transaction overhead. The firmware measures the real
  figure into `drainMaxUs` and puts it on the dashboard — so this is checkable
  in about ten minutes on your bench, and it is the first thing to check.
- **Two GPIO interrupts on one dispatcher.** Classic ESP32 handles this
  routinely; a known arduino-esp32 bug where only the last-registered GPIO ISR
  fires is **ESP32-C6-specific**. Still worth a smoke test: pull one INT line
  and confirm only that bus reports `intStuck`.
- **Sustained write throughput with both maps loaded.** The ceiling in section 8
  is arithmetic over row sizes and typical SD speeds, not a measurement.

**NOT verified — everything added since the single-bus field recordings.**
Transmitting, the dashboard, the heap-sized frame maps and the SD mount retry
have not been run against an actual ESP32, MCP2515, SD card or live bus. The
wiring, bit timing and throughput figures are reasoned from the datasheets and
the code, not measured by me.

**NOT verified — transmitting to a real ECU.** The encoder is proven against its
own decoder and the driver against a simulated controller, but no frame from this
firmware has been put on a real wire or acknowledged by a real node. The one-shot
and bus-off reasoning comes from the MCP2515 datasheet, not from a scope. Bench
it against a node you can afford to confuse before pointing it at a machine.

**NOT verified — the power-cut path specifically.** The sync interval, the
emergency-close sequence and the hold-up capacitor sizing are reasoned from how
FatFs commits metadata. Before trusting a session, test it: record for a minute,
pull the power, and check how many rows survived against `SD_SYNC_INTERVAL_MS`.

### A note on the shims

`test/shim/Arduino.h` deliberately reproduces the Arduino core's object-like
macros. Its remaining blind spot is API *signatures*: `SD`, `WiFi`, `WebServer`
and the FreeRTOS calls are stubs with plausible prototypes, not the real ones.
Only a real `pio run` covers those — which is why CI runs both, and why the
result above matters.

---

</details>

## 14. Known issues

<details>
<summary><i>expand</i></summary>
<br>


Six things are known to be wrong or unfinished. Everything here is either
visible in the source or came out of the field recordings; nothing is
speculative, and nothing known is being left out.

**Export can be up to 1.2 s out of date.** `exportSetup()` downloads what the
LOGGER holds — it fetches `/api/dash/cfg` rather than serialising the page —
which is right almost always, and is what makes an export exactly the file that
is on the card. But an edit only reaches the logger about 1.2 s after you stop
making it, so pressing Export inside that window downloads the *previous*
version, with no sign that it did. Wait a moment after your last change, or
press *Done* in Customize first, which writes immediately. The fix is for Export
to flush the pending save before it fetches.

**A plain signal in a multiplexed message cannot be sent.** A `.dbc` may give a
multiplexed message a signal with no `m<code>` on it — no marker at all — which
means it rides in *every* frame that message sends, whatever the selector says.
An alive-counter or a CRC normally sits there. This build has nowhere to put it:
the unit it writes is one message under one selector code, and a signal that
belongs to every code belongs to no one frame. So it is left out of the sendable
list and goes out as **zero** in every frame. Decoding and logging are
unaffected — `decode.cpp` shows it on every frame, correctly.

The consequence is bounded but sharp: if that signal is a counter or a checksum
the ECU validates, every command this logger sends is rejected, and the reason
is not visible from the Send tab. `tools/check_dbc.py` reports the shape by name
so you find out at the desk instead of on the bus, and the setup sheet leaves the
signal out of the picker rather than offering a value it cannot honour.

None of the 15 `.dbc` files this was developed against has one — 234 messages,
14 of them multiplexed, none with a plain signal — but the shape is normal in
OEM and AUTOSAR-derived files, where end-to-end protection puts a counter and a
CRC outside the multiplexed payload. Supporting it means letting one value
belong to every code of its message and writing it into each frame; the guard is
in place so that until then the failure is announced rather than silent.

**Extended multiplexing decodes silently wrong.** `SG_MUL_VAL_` is not parsed,
and a signal that depends on it is treated as an ordinary signal — so it is
decoded on *every* page of its frame instead of only the pages where it exists,
producing values that are plausible and wrong. This is the one parser gap that
does not announce itself: an unparsable line is counted and reported at boot, a
map that does not fit says so in the CSV header, but this one just quietly puts
numbers in the file. Until it is supported, delete those signals from the copy
of the DBC on the card — a missing signal is obvious, a wrong one is not.

**The controller's error counters are read by nothing.** `txErrorCount()`,
`rxErrorCount()` and `errorFlags()` exist in `mcp2515.cpp` and are called from
nowhere in `src/`. The consequence is specific: a board that boots clean, mounts
its card and then records **zero frames** cannot tell you why. A genuinely quiet
bus and a wrong `CAN<n>_BITRATE_KBPS` or `CAN<n>_CRYSTAL_MHZ` look identical from the
status line, and REC climbing is exactly what distinguishes them — the receiver
counting errors means frames are arriving and being mangled, not absent. One
field recording sat in that state for its whole length. The registers are there;
they are simply not surfaced yet.

**What stalls the receive task has never been identified.** In one recording the
path ran clean for 6187 s at 541 frames/s with zero loss, then took an overflow
and never fully recovered. Slow SD writes were ruled out — the nearest one is
36 s away, and the load *fell* fourfold after the fault — and so were acceptance
filters, since loss was uniform across identifiers. Something held the service
task past roughly 1.2 ms, which is two receive buffers at that frame spacing.
Clearing the sticky flags every pass makes that survivable instead of terminal,
which is why it is a known issue and not an open wound, but the trigger is still
unknown.

**An overflow is not recovered for free.** Between the overflow and the next
service pass, anything arriving past the two hardware buffers is gone: at
540 frames/s and a 20 ms worst case that is on the order of ten frames per
event. And the number reported is a **floor**, never a total — `EFLG`'s two
overflow bits are sticky and say *at least once since you last cleared me*, so
the page prints `≥`. Expect occasional non-zero `canOvf` on a fault rather than
a guarantee of zero, and read the figure as the least that was lost.

---

</details>

## 15. Future development

<details>
<summary><i>expand</i></summary>
<br>


None of this is committed to. It is written down because the design decisions
that make each one cheap or expensive are already made, and knowing which is
which is worth more than a wish list.

### Dual CAN — done, and what it cost

This section used to be the top of the wish list. It is now the repository, so
what follows is the account rather than the plan — the estimate is left visible
because two of its five lines were wrong in a way worth recording.

| | estimated | what actually happened |
|---|---|---|
| A second frame queue | +24 KB, or one shared queue with a bus field | One shared queue, and the bus field was **free**: `CanFrame` had no padding left, so packing the flags as bitfields kept it at 24 bytes. The +24 KB was spent instead on doubling the queue's *depth*, which is a different argument (the SD card's stall, not the second bus). |
| The CSV | "an eighth column appended, or an id prefix" | An eighth column, but **second**, not appended: `bus` is part of a row's identity, not its payload, and putting it next to `t_us` and `id` is what makes it hard to forget when grouping. |
| The frame map | two DBCs, one heap budget | As estimated. They load in order, CAN1 first, so a large map on CAN1 shrinks CAN2's — stated in the log when it happens. |
| SPI | "the case to measure, not assume" | The right instinct, and the estimate missed *why*. The problem was never bus bandwidth (~11 % at full load); it was that arduino-esp32 charges 2–3 µs of overhead **per byte**, which put the worst-case drain at ~245 µs against a 200 µs deadline. One block transfer per transaction fixed it. See [section 8](#8-why-it-does-not-lose-frames). |
| The dashboard | a cell must say which bus it came from | As estimated, and it reached further than expected: the frame maps, the live-value cache, the signal picker, the transmit frames and `dash.cfg` all needed the same field. |

The summary that held up: **the driver work was small and the schema work was
not.** The parts that took the longest were the ones where "which bus" had to
travel — through a queue, into a file format, out to a browser, and back.

The one thing the estimate did not anticipate at all is the honest limit in
section 8: doubling the frame rate moved the binding constraint off the capture
path and onto the *writer*. The capture path closes with margin; a pair of
fully-mapped busy buses can still outrun the SD card. That is instrumented and
documented rather than hidden, but it is the thing to know.

### The rest, roughly in order of value per unit of work

- **One desk tool, not two.** `customize.py` and `tools/preview_dashboard.py`
  started with different jobs — one set a logger up for a bus, the other showed
  the page moving with invented data — and every release since has made them
  more alike. They now share the frame-map reader, the setup file, the role, the
  `/api/dbc` upload and the pruning rule, and the pruning rule has already had
  to be fixed in both. Two programs that must agree about everything are one
  program with a flag: fold the preview into `customize.py` (or leave a thin
  `--preview` entry point) and the class of bug where the two drift apart stops
  existing. The test that asserts the Python and the C prune identically is
  there to catch that drift; not needing it at all is better.
- **Surface `TEC`/`REC`/`EFLG`** on the status line and in `N.log`. The
  accessors already exist and are called by nothing (see §14); this is an hour's
  work and it turns "no frames, no idea why" into a diagnosis.
- **`SG_MUL_VAL_`.** Closes the one gap in the parser that produces wrong
  numbers rather than an error.
- **A pre-trigger ring buffer.** Hold the last N seconds in RAM and commit to
  the card only when a condition fires — a signal crossing a threshold, a
  specific identifier appearing. The dashboard already carries per-cell
  thresholds, so the condition language mostly exists; what is missing is
  recording into a ring instead of straight through. This is how you catch an
  intermittent fault without a 1.5 GB file.
- **Wall-clock time.** `t_us` starts at zero every file, on purpose, because
  there is no RTC. An RTC module or a GPS PPS input would let the header carry a
  real start timestamp — which is what you need to line a recording up against
  anything else that was logging at the time.
- **Opt-in acceptance filters.** Filters are off deliberately, and that should
  stay the default. But on a bus where you know you want four identifiers out of
  a hundred, letting the controller drop the rest removes the SD card from the
  argument entirely. It must be loud about being on: a filtered recording that
  looks like a complete one is a trap.
- **CAN FD.** Not this chip — the MCP2515 is classical CAN and no amount of
  firmware changes that. The MCP2517FD/MCP2518FD are the same SPI pattern, so
  the driver is replaceable in isolation, but 64-byte payloads touch the frame
  struct, the queue sizing, the CSV `raw` column and the DBC parser's assumption
  that a message fits in eight bytes. A real port, not a swap.
- **A host-side viewer.** `tools/` reads a frame map and previews a dashboard;
  it does not plot a recording. Something that opens an `N.csv`, groups on
  `(t_us, id)` and draws signals against time would close the loop between
  recording and looking, without a spreadsheet that dies at a million rows.
- **File rotation by size.** One recording is one file however long it runs.
  A 1.5 GB `.csv` is awkward to move and awkward to open; rolling at a
  configurable size, with the header repeated, would not change the format.

---

</details>

## License

MIT — see [LICENSE](LICENSE).

## Author

**Mohammad Badri Ahmadi** — embedded systems & on-device AI

<br><br>

<div align="center"><p align="center">
    &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
    <a href="mailto:contact@biss.qzz.io" style="text-decoration: none;" alt="Email">
        <img src="https://github.com/mhmmdbdrhmd/Data/blob/main/Icons/ICON%20_Black%20-%20GMail.png" width="6%" />
    </a>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
    <a href="https://github.com/mhmmdbdrhmd" style="text-decoration: none;" alt="GitHub">
        <img src="https://github.com/mhmmdbdrhmd/Data/blob/main/Icons/ICON%20_Black-%20Github.png" width="6%" />
    </a>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
    <a href="https://www.linkedin.com/in/mohamad-badri-ahmadi-aa2a1a8a" style="text-decoration: none;" alt="LinkedIn">
        <img src="https://github.com/mhmmdbdrhmd/Data/blob/main/Icons/ICON%20_Black%20-%20Linkding.png" width="6%" />
    </a>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
  <a href="https://twitter.com/mhmmdbdrhmd" style="text-decoration: none;" alt="Twitter">
        <img src="https://github.com/mhmmdbdrhmd/Data/blob/main/Icons/ICON%20_Black%20-%20Twitter%20X.png" width="6%"/>
    </a>
    &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
  <a href="https://biss.qzz.io" style="text-decoration: none;" alt="Website">
        <img src="https://github.com/mhmmdbdrhmd/Data/blob/main/Icons/ICON%20_Black%20-%20Website.png" width="6%"/>
    </a>
    &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
</p></div>

