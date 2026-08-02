# can-logger-esp32

A CAN bus logger built from an ESP32 and an MCP2515: it captures **every frame
on the wire**, decodes it against a **DBC file you put on the SD card**, and
writes normalised CSV — with a live web dashboard that shows whatever that DBC
describes.

No frame identifier, signal name, scaling factor or unit is compiled into the
firmware. Give it a DBC and it logs named signals in engineering units. Give it
nothing and it logs raw frames. The same binary does both.

```
        any CAN bus                    ESP32 DevKit                phone / laptop
   ┌────────────────────┐         ┌────────────────────┐        ┌──────────────┐
   │  frames @ any rate │  CAN_H  │ MCP2515 ── ISR     │  WiFi  │  dashboard   │
   │                    │────────>│   queue ── decode  │<──────>│  live signals│
   └────────────────────┘  CAN_L  │   SD ── N.csv/N.log│   AP   │  live log    │
                                  └────────────────────┘  or STA└──────────────┘
                                        ▲
                                  /frames.dbc  ← your frame map
```

**Contents** · [Hardware](#1-hardware) · [Wiring](#2-wiring) ·
[Frame map](#3-the-frame-map-framesdbc) · [Build](#4-building-and-flashing) ·
[Output](#5-what-gets-recorded) · [Web app](#6-the-live-web-app) ·
[Design](#7-why-it-does-not-lose-frames) · [Power cuts](#8-surviving-a-power-cut) ·
[Tools](#9-host-side-tools) · [Tuning](#10-tuning) ·
[Troubleshooting](#11-troubleshooting) ·
[Verification](#12-verification-status)

---

![Wiring diagram](docs/wiring.svg)


## 1. Hardware

| Part | Notes |
|---|---|
| **ESP32 DevKit v1** (30-pin) | Any ESP32 dev board works; the pin map below is for the 30-pin v1. |
| **MCP2515 + TJA1050 CAN module** | The common blue module. **Check the crystal** — 8 MHz or 16 MHz, see below. |
| **Micro-SD card module** (SPI) | 3V3 logic. Modules with a 5 V regulator and level shifters also work off 5 V. |
| **Micro-SD card**, FAT32 | Class 10 or better. Cards over 32 GB usually ship as exFAT and must be reformatted. |
| 120 Ω resistor | Only if the logger sits at the physical end of the bus. |
| 6800 µF / 10 V electrolytic + a diode | Optional, for the power-fail input — see §8. |

Total, at the time of writing, well under the price of a commercial bus logger's
licence dongle.

> **Check the crystal on your MCP2515 board.** The common blue modules use
> 8 MHz; some clones use 16 MHz. Set `CAN_CRYSTAL_MHZ` in `src/config.h`.
> The wrong value gives "NO CAN TRAFFIC" on a perfectly healthy bus, and it is
> the single most common reason a first attempt sees nothing.

---

## 2. Wiring

Two separate SPI buses **on purpose**: an 8 KB SD write takes milliseconds, and
sharing one bus would stall CAN reads for exactly that long.

| MCP2515 | ESP32 | | SD card | ESP32 |
|---|---|---|---|---|
| VCC | 3V3 | | VCC | 3V3 |
| GND | GND | | GND | GND |
| CS  | **D5**  | | CS   | **D4**  |
| INT | **D17** | | SCK  | **D14** |
| SCK | D18 (VSPI) | | MISO | **D27** |
| MISO| D19 | | MOSI | **D13** |
| MOSI| D23 | | | (HSPI) |

Bus side: `CAN_H` and `CAN_L` to the bus, `GND` to the bus ground. Default bit
rate is **250 kbit/s** (`CAN_BITRATE_KBPS`); 100, 125, 250, 500 and 1000 are
supported at either crystal frequency.

The on-board LED (D2) is steady off when idle, blinks slowly while recording and
blinks fast on an SD fault. Set `PIN_STATUS_LED` to `-1` to disable.

### Normal mode or listen-only

`CAN_LISTEN_ONLY` defaults to **0** (normal mode), so the logger acknowledges
frames. That is correct when it is the only other node on the bus — with nobody
to ACK, the talking node goes error-passive and eventually bus-off.

Set it to **1** when you are tapping a bus that already has two or more live
nodes. Listen-only never drives the bus at all, which is the safe choice on a
machine that is actually running.

---

## 3. The frame map (`/frames.dbc`)

This is the part that keeps the firmware generic. Everything the logger knows
about a bus comes from a **DBC file in the root of the SD card**, read once at
boot. Nothing is compiled in.

- **DBC present** → frames it describes are decoded into named signals in
  physical units, and both the CSV and the web page show those names.
- **DBC absent** → every frame is recorded as raw payload bytes. This is a
  supported mode, not a failure. You can decode the CSV offline later.
- **Both at once** → identifiers the DBC does not describe still get recorded,
  as raw bytes, alongside the decoded ones. A logger that silently drops the
  frames it does not recognise is a logger that lies about what was on the bus.

Copy [`examples/example.dbc`](examples/example.dbc) to the card as
`/frames.dbc` and edit it, or export one from your usual CAN tool. The path is
`DBC_PATH` in `src/config.h`.

### What the parser supports

| Construct | Support |
|---|---|
| `VERSION "…"` | Kept, and written into every CSV header |
| `BO_ <id> <Name>: <dlc> <Node>` | Yes. 29-bit ids carry bit 31 set, as usual |
| `SG_ … : <start>\|<len>@<order><sign> (<fac>,<off>) [min\|max] "<unit>"` | Yes |
| Byte order `@1` (Intel) and `@0` (Motorola) | Both |
| Signed and unsigned, 1–64 bits | Yes |
| Factor and offset | Yes — **exactly**, see below |
| Units | Yes, carried into the CSV and the dashboard |
| `VAL_` value tables | Yes — the CSV prints `running`, not `2` |
| Multiplexing (`M`, `m0`, `m1`, …) | Yes — a multiplexed signal is emitted only when the multiplexor selects it |
| `SIG_VALTYPE_ … : 1;` / `: 2;` | Yes — IEEE float32 and float64 payloads |
| `BA_ "Unwrap" SG_ <id> <Sig> 1;` | Yes — see [free-running counters](#free-running-counters) |
| `CM_`, `BU_`, `NS_`, `BS_`, `BA_DEF_` | Parsed past and ignored |

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
(`N line(s) of /frames.dbc could not be parsed`), never skipped in silence. If
the map is larger than `DBC_MAX_MESSAGES` / `DBC_MAX_SIGNALS`, the logger says
so and the frames beyond it are recorded as raw bytes rather than lost.

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

### PlatformIO (recommended)

```bash
cd platformio
pio run -t upload -t monitor
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
/1.csv   /1.log
/2.csv   /2.log
```

A slot counts as taken if *either* file exists, so the pair always shares a
number.

### `N.csv` — the normalised schema

Seven fields, `;` separated, always in this order, **one row per decoded
signal**:

```
t_us;id;name;signal;value;unit;raw
0;0x100;NodeStatus;Uptime;42;s;
0;0x100;NodeStatus;State;running;;
1000;0x101;MotorFeedback;Speed;-250.00;rpm;
2000;0x200;;;;;1122
```

| Column | Meaning |
|---|---|
| `t_us` | Recorder clock, µs, **starting at 0 for every file**. Captured inside the CAN interrupt, so it is the arrival time on the wire — not the time the row happened to be formatted. |
| `id` | Identifier. An 11-bit id prints short (`0x100`); a 29-bit id always prints eight digits (`0x00000100`). That is not cosmetic — the two are different frames, and the width is what tells them apart. |
| `name` | Message name from the DBC, or the CANopen function. Empty for an identifier nothing described. |
| `signal` | Signal name from the DBC. Empty on a raw row. |
| `value` | The physical value, or the symbolic text when a `VAL_` table names it. Empty on a raw row. |
| `unit` | Unit string from the DBC. Often empty; that is not an error. |
| `raw` | Payload as hex. **Always** present when the row carries no decoded signal. Also on the first row of each decoded frame if you set `CSV_INCLUDE_RAW`. |

**All rows of one frame share `t_us` and `id`** — group on that pair. A frame
with four signals is four rows; a frame nothing could decode is exactly one row
carrying its bytes. That is the whole contract, and it does not change with the
DBC: downstream tooling parses deterministically without re-implementing any CAN
decoding.

Every file opens with a `#` header block listing the columns, the bus settings,
and the entire frame map that was active — message by message, signal by signal,
with units. A CSV found on a card a year from now is interpretable on its own,
even if the DBC has since been edited.

Cost: roughly 30–40 bytes per row.

### `N.log` — the detailed log

Same base name. Everything the serial port shows **plus** per-identifier counters
and rates, queue depth and high-water mark, SD write and sync timings, MCP2515
error registers, heap and network state — once per second.

The serial stream is deliberately one line per second so it stays informative
without becoming an I/O cost of its own:

```
[   142.003] I REC 1.csv 00:02:21 | 141000 rows 3672 KB | 220 f/s | 7 ids | lost 0
```

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

The page shows:

- **four status cards** — SD Card, Recording (with START/STOP), Bus, and Data
  Integrity;
- **Live Signals** — every signal the frame map describes, with its current
  value and unit, updated twice a second;
- **Identifiers on the wire** — every id seen, its most recent payload, its frame
  count and rate, and whether the map describes it;
- **Live Log** — the serial log, mirrored.

Like the firmware, the page names nothing. It renders whatever the status
document contains, and that document is built from the DBC and the traffic. With
a frame map you get named signals in engineering units; without one the signal
table says so and the identifier table shows raw payloads live. Nothing about it
changes when you swap buses — only the file on the card does.

Preview it with no hardware at all:

```bash
python3 tools/preview_dashboard.py                          # raw-frame view
python3 tools/preview_dashboard.py --dbc examples/example.dbc
```

That serves the real page, extracted from `src/webui.cpp`, against simulated
data — so the preview can never drift from what the firmware ships.

It is a plain `WebServer` with 2 Hz polling rather than an async server with
websockets, on purpose: no third-party libraries, and the HTTP handler runs at
the lowest priority and never touches the SD card or the CAN controller, so a
browser hitting refresh cannot perturb a recording.

---

## 7. Why it does not lose frames

This is the part worth copying if you build something else.

A busy 250 kbit/s bus delivers on the order of 1000 frames/s and **the MCP2515
holds exactly two**. That is roughly 2 ms of slack, against SD block writes that
can stall for 100 ms on a bad card. Polling cannot bridge that gap. Neither can
"do the work in the interrupt" — an SPI transaction can block, and blocking in an
interrupt handler is how frames get lost.

So the interrupt does the **minimum bounded work** and nothing else:

| Stage | Who | What, and why |
|---|---|---|
| INT falls | `canIsr`, in IRAM | Takes the arrival timestamp with `esp_timer_get_time()` and pushes it into a small ring, then unblocks the reader task. **No SPI, no allocation, no file I/O, no logging.** The work is constant regardless of what the bus is doing. |
| drain | CAN task, prio 20 | Empties *both* receive buffers over SPI and keeps going until the controller reports empty — that is what makes the edge-triggered INT safe when a second frame arrives while INT is still low. A 20 ms timeout re-drains unconditionally, so even a completely missed edge costs latency, never data. |
| buffer | 1024-frame queue | 24 KB = one full second of slack in front of the SD card. |
| decode + write | writer task, prio 10 | Looks the id up in the frame map, decodes each signal, formats CSV, fills an 8 KB block, writes it. While blocked in that write the reader simply preempts it. |
| Wi-Fi / HTTP | core 0 and `loop()` | Lowest priority, on the other core. Cannot interfere with either. |

The timestamp is taken **in the ISR**, before any queuing or scheduling delay can
smear it — which is why `t_us` is trustworthy even when the writer is 200 ms
behind. Everything expensive (DBC lookup, bit extraction, scaling, decimal
formatting, SD I/O) happens on the far side of the queue, where being slow costs
queue depth instead of frames.

Both remaining loss points are **counted and reported**: the controller's own
overflow flags (stage 1→2) and the queue-full counter (stage 2→3). A recording
that ends with `lost 0` is provably complete — that is what the *Data Integrity*
card shows.

---

## 8. Surviving a power cut

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

## 9. Host-side tools

Standard library only, except the plotter.

```bash
# summary, per-id rates, and an integrity check
python3 tools/parse_log.py 1.csv

# which signals are in this recording
python3 tools/parse_log.py 1.csv --list

# pivot the long form to one column per signal, for pandas/Excel
python3 tools/parse_log.py 1.csv --wide wide.csv

# plot (needs matplotlib); signals sharing a unit share an axis
python3 tools/plot_log.py 1.csv 'MotorFeedback.*' -o out.png

# the dashboard, with no hardware
python3 tools/preview_dashboard.py --dbc examples/example.dbc

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

## 10. Tuning

All in `src/config.h`:

| Setting | Default | |
|---|---|---|
| `CAN_BITRATE_KBPS` | 250 | 100 / 125 / 250 / 500 / 1000 |
| `CAN_CRYSTAL_MHZ` | 8 | Must match your MCP2515 board |
| `CAN_LISTEN_ONLY` | 0 | 1 = never drive the bus |
| `DBC_PATH` | `/frames.dbc` | Where the frame map lives |
| `DBC_MAX_MESSAGES` / `DBC_MAX_SIGNALS` | 64 / 256 | ~25 KB of static tables |
| `CANOPEN_DECODE` | 0 | Label unmapped ids CANopen-style |
| `CSV_INCLUDE_RAW` | 0 | Keep the payload on decoded frames too |
| `SD_BLOCK_BYTES` | 8192 | Bytes per SD write |
| `SD_SYNC_INTERVAL_MS` | 1000 | The power-cut exposure window |
| `PIN_POWER_FAIL` | -1 | See §8 |
| `FRAME_QUEUE_LEN` | 1024 | One second of slack |
| `BUS_TRACK_IDS` / `WEB_MAX_SIGNALS` | 24 / 48 | Dashboard table sizes |
| `AUTO_START_RECORDING` | 1 | Record from power-on |
| `ENABLE_OTA` | 1 | Wi-Fi flashing |

Plus the pin map, task priorities and cores, and the Wi-Fi fallbacks.

---

## 11. Troubleshooting

| Symptom | Cause |
|---|---|
| `CAN CONTROLLER NOT RESPONDING` | MCP2515 wiring or power. The driver verifies the SPI link both ways at boot, so this means CS, MISO/MOSI or 3V3 is wrong. |
| `NO CAN TRAFFIC`, bus is fine | Wrong `CAN_CRYSTAL_MHZ` (8 vs 16), wrong bit rate, or CAN_H/CAN_L swapped. |
| Everything logs as raw bytes | No `/frames.dbc` on the card, or it has no `BO_` lines. The boot log says which. |
| One message logs as raw, the rest decode | Its identifier is not in the DBC — or it is, but with the 29-bit flag set/unset differently. |
| `N line(s) of /frames.dbc could not be parsed` | Check the `.log` file. Interleaved `SG_` blocks and unsupported constructs are the usual causes. |
| Values look scaled wrong | Factor, offset or start bit in the DBC. Set `CSV_INCLUDE_RAW = 1` and check the payload against the decoded value. |
| `the frame map did not fit` | Raise `DBC_MAX_MESSAGES` / `DBC_MAX_SIGNALS`. Frames beyond it are still recorded, as raw bytes. |
| `lost` climbing | Slow SD card. Use a decent class-10, raise `SD_BLOCK_BYTES`, or check `maxWr` in `N.log`. |
| Last seconds missing after a power cut | Expected without the power-fail input — see §8. If `maxSync` is large the card is slow at committing metadata. |
| File exists but has only the header | Power was cut in the first second. The header is committed at start, so this is the floor, not corruption. |
| The other node goes error-passive | `CAN_LISTEN_ONLY = 1` with nothing else on the bus to ACK. |
| `SD CARD NOT FOUND` | Card must be FAT32. Cards over 32 GB often ship as exFAT. Re-check CS=D4, SCK=D14, MISO=D27, MOSI=D13. |
| Dashboard unreachable | Check `mode` in `/config.txt`; on `sta` failure it falls back to the `CAN-Logger` hotspot. |
| `Wrong boot mode detected (0x13)` | Flashing, not running — see [WINDOWS.md](WINDOWS.md). |

---

---

## 12. Verification status

Being straight about what has and has not been proven.

**Verified — the firmware compiles for real ESP32 hardware.**
`pio run -e esp32dev` against espressif32 7.0.1 / arduino-esp32 3.20017 /
xtensa-gcc 8.4.0 links a complete image, with **no warnings** from any of the ten
translation units under `-Wall -Wextra`:

```
RAM:   [====      ]  35.1% (used 114896 bytes from 327680 bytes)
Flash: [=====     ]  47.1% (used 925849 bytes from 1966080 bytes)
```

Flash sits at 47 % of one 1.9 MB app slot, so the OTA partition scheme has ample
room. RAM includes the ~25 KB of frame-map tables, the 24 KB frame queue and the
13 KB CSV staging buffer.

**Verified — the portable logic, natively.** `./test/run_tests.sh` runs 128
assertions across the DBC parser, the CSV schema, the CANopen layer and the
logger, and all pass. That covers Intel and Motorola bit extraction, signed
values, exact decimal scaling, value tables, multiplexing, IEEE floats, counter
unwrapping, malformed DBC input, table overflow, the seven-field row invariant,
the header block and the dashboard's DOM/status contract.

**NOT verified — anything requiring hardware.** Nothing here has been run against
an actual ESP32, an MCP2515, an SD card or a live CAN bus. The wiring, bit
timing, throughput figures and SD behaviour are reasoned from the datasheets and
the code, not measured.

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

