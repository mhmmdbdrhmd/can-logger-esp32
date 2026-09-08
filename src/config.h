/* ============================================================================
 *  config.h - compile-time configuration
 *
 *  Everything a user normally changes lives here or in two files on the SD
 *  card:
 *
 *      /frames.dbc   the frame map for CAN1 (optional - without it the logger
 *                    records raw payload bytes and decodes nothing)
 *      /frames2.dbc  the frame map for CAN2, kept separate because the same
 *                    identifier routinely means different things on two buses
 *      /config.txt   Wi-Fi credentials and hostname
 *
 *  Nothing below is specific to any particular bus. The defaults describe two
 *  plain 250 kbit/s classical CAN networks and a stock ESP32 DevKit v1.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* ---------------------------------------------------------------------------
 *  1. PIN MAP  (ESP32 DevKit v1)
 *
 *   MCP2515 #1 - CAN1           ESP32            SD card module      ESP32
 *   -----------------           -----            --------------      -----
 *     VCC ....................  3V3                VCC ............. 5V (VIN)
 *     GND ....................  GND                GND ............. GND
 *     CS  ....................  D22                CS  ............. D4
 *     INT ....................  D21                SCK ............. D14
 *     SCK ....................  D18  (VSPI)        MISO ............ D27
 *     MISO ...................  D19                MOSI ............ D13
 *     MOSI ...................  D23                                  (HSPI)
 *
 *   MCP2515 #2 - CAN2           ESP32
 *   -----------------           -----
 *     VCC ....................  3V3
 *     GND ....................  GND
 *     CS  ....................  D5
 *     INT ....................  D17   <- silkscreened TX2 on a DevKit v1
 *     SCK ....................  D18  (VSPI, shared)
 *     MISO ...................  D19  (shared)
 *     MOSI ...................  D23  (shared)
 *
 *  BOTH CONTROLLERS SHARE ONE SPI BUS, and that is fine: MISO tri-states while
 *  CS is high, so only CS and INT have to be unique. They are also driven from
 *  ONE task, which is what keeps it fine - two tasks would contend for the
 *  bus mutex on the one path that has a hard deadline. See app.cpp.
 *
 *  The SD card is on a SECOND, INDEPENDENT SPI BUS on purpose: it is written
 *  from a lower-priority task, and sharing one bus would serialise them - a
 *  32 KB SD write would stall CAN reads for milliseconds, which at thousands
 *  of frames a second is measured in lost frames.
 *
 *  D22 and D21 are the ESP32's default I2C pins, unused in this project.
 *  Neither is a strapping pin, and unlike GPIO16/17 neither is taken by PSRAM
 *  on a WROVER module - which is why CAN1 lives there rather than on D16.
 * -------------------------------------------------------------------------*/
#define PIN_CAN1_CS     22
#define PIN_CAN1_INT    21

#define PIN_CAN2_CS     5
#define PIN_CAN2_INT    17

/* Shared by both controllers. */
#define PIN_CAN_SCK     18
#define PIN_CAN_MISO    19
#define PIN_CAN_MOSI    23

#define PIN_SD_CS       4
#define PIN_SD_SCK      14
#define PIN_SD_MISO     27
#define PIN_SD_MOSI     13

/* Optional LED mirroring the recording state (blink = recording, fast blink =
 * fault). Set to -1 to disable. GPIO2 is the on-board LED of most DevKit v1s.
 *
 * CAUTION - GPIO2 is an ESP32 STRAPPING PIN. It must be LOW or floating at
 * reset for the chip to enter download mode. The on-board LED wiring
 * (GPIO2 -> resistor -> LED -> GND) is fine and even helps. An EXTERNAL LED
 * wired active-low (3V3 -> resistor -> LED -> GPIO2) holds GPIO2 HIGH and makes
 * the board impossible to flash - esptool reports "Wrong boot mode detected".
 * Rewire it active-high, or set this to -1; nothing depends on it.
 *
 * The only other strapping pin in this map is GPIO5 (PIN_CAN2_CS), which must be
 * HIGH at reset - an MCP2515 chip-select idles high, so that matches.
 *
 * One more board quirk, because it costs people an hour: many DevKit v1s have
 * NO PIN MARKED D17. That board labels UART2 by function - the pin silkscreened
 * TX2 is GPIO17 and RX2 is GPIO16. CAN2's INT goes to TX2. USB upload and the
 * serial monitor run on UART0 and are unaffected. */
#define PIN_STATUS_LED  2

/* ---------------------------------------------------------------------------
 *  2. CAN BUSES
 *
 *  Every setting here is PER BUS. That is not symmetry for its own sake: two
 *  buses on one machine routinely run at different bit rates, and two MCP2515
 *  modules out of the same order routinely carry different crystals.
 * -------------------------------------------------------------------------*/
#define CAN_BUSES           2       /* fixed by the pin map above            */

#define CAN1_BITRATE_KBPS   250     /* 100, 125, 250, 500 or 1000            */
#define CAN2_BITRATE_KBPS   250

/* Crystal soldered on EACH MCP2515 board - check them separately. The common
 * blue "MCP2515 + TJA1050" modules ship with 8 MHz; some clones use 16 MHz.
 * If a bus reports "no CAN traffic" but is alive, this is the first thing to
 * change, and it is the first thing to change for THAT bus only. */
#define CAN1_CRYSTAL_MHZ    8       /* 8 or 16 */
#define CAN2_CRYSTAL_MHZ    8

/* Find this bus's bit rate and crystal at boot instead of trusting the two
 * settings above.
 *
 *  1 = listen at each (bit rate, crystal) pair the driver has timings for and
 *      keep the first one that decodes real frames. The pair configured above
 *      is tried FIRST, so a correct config.h costs one window and usually far
 *      less - a busy bus answers in a few milliseconds.
 *  0 = use the settings above, unconditionally.
 *
 * Detection is done in LISTEN-ONLY mode whatever CANn_LISTEN_ONLY says: a
 * wrong bit rate on a live bus would otherwise make this logger flood it with
 * error frames while it guesses, which is exactly the damage a diagnostic tool
 * must not do. It also needs TRAFFIC - a quiet bus is indistinguishable from a
 * wrong bit rate, and on a quiet bus detection fails and the settings above
 * are used. That is the whole failure mode: it never leaves the bus
 * unconfigured.
 *
 * WHAT IT CAN GET WRONG. A crystal and a bit rate multiply: the registers for
 * 250 kbit/s on an 8 MHz module produce 500 kbit/s on a 16 MHz one. Both
 * decode the same wire perfectly, so detection cannot tell them apart, and it
 * would report the pair it happened to try first. It therefore sweeps the
 * crystal set HERE first and only then the other one - which means a correct
 * CANn_CRYSTAL_MHZ above gives a correct bit rate in the log, and a wrong one
 * still gets you a working bus with a warning that the reported rate is scaled
 * by the same factor. If the reported rate is not the rate you expect, the
 * crystal setting is what to fix.
 *
 * The cost is boot time: worst case (nothing on the bus) is every pair tried
 * for CAN_AUTODETECT_MS, about 3 s per bus. Nothing is recorded during that
 * time - detection runs before the reader task exists and the frames it hears
 * are thrown away. */
#define CAN1_AUTODETECT     0
#define CAN2_AUTODETECT     0

/* How long to listen at each candidate pair, and how many frames have to
 * decode before it counts as the answer.
 *
 * Two frames, not one: at a wrong bit rate the CRC check makes a whole valid
 * frame essentially impossible, but "essentially" is not "never", and a single
 * fluke would otherwise pick the wrong rate for the entire recording.
 *
 * 300 ms therefore needs a bus carrying roughly 7 frames/s or better to be
 * detectable, which covers any machine with a running controller on it. Raise
 * it for a bus that only speaks when spoken to; that multiplies the worst-case
 * boot delay above by the same factor. */
#define CAN_AUTODETECT_MS   300
#define CAN_AUTODETECT_FRAMES 2

/* 0 = normal mode: the logger acknowledges frames, which is what you want when
 *     it is the only other node on the bus (otherwise the talker goes
 *     error-passive and eventually bus-off).
 * 1 = listen-only: fully passive, never drives the bus. Correct when another
 *     node is already acknowledging, and the safe choice on a live machine.
 * Independent per bus, because the two buses are often not in the same state:
 * a diagnostic bus you may drive, a live powertrain bus you may not. */
#define CAN1_LISTEN_ONLY    0
#define CAN2_LISTEN_ONLY    0

/* Set to 0 to run this firmware on single-bus hardware - the second controller
 * is never probed, no interrupt is attached to PIN_CAN2_INT, and the CSV still
 * carries its bus column (always 1). Useful for a board that has not been
 * rewired yet, and the control case when measuring what the second bus costs. */
#define CAN2_ENABLED        1

/* SPI clock for both MCP2515s. The datasheet allows 10 MHz; at 10 MHz a full
 * frame read is ~13 us of clock, and the driver issues it as one block
 * transfer so the per-byte call overhead does not multiply that. See the
 * timing note at the top of mcp2515.cpp - with two controllers on one bus
 * this is the difference between meeting the deadline and missing it. */
#define CAN_SPI_HZ          10000000UL

/* ---------------------------------------------------------------------------
 *  3. FRAME MAP  (the DBC file)
 *
 *  If this file exists on the SD card it is parsed at boot and every frame it
 *  describes is decoded into named signals in physical units. If it is absent
 *  the logger still records everything, as raw payload bytes - no frame is ever
 *  dropped for being unrecognised.
 *
 *  See examples/example.dbc and the README for the supported subset.
 * -------------------------------------------------------------------------*/
/* One map per bus. They are separate files rather than one shared map because
 * the same identifier routinely means two different things on two buses - a
 * shared map would decode bus 2 with bus 1's meanings and be confidently,
 * silently wrong, which is the worst failure a logger has.
 *
 * Either file may be absent. That bus then records raw payload bytes, exactly
 * as a logger with no map at all does. */
#define DBC_PATH            "/frames.dbc"
#define DBC2_PATH           "/frames2.dbc"

/* A frame map uploaded from the web app lands here first and is renamed over
 * its DBC path only once the whole file has arrived. A dropped Wi-Fi connection
 * halfway through an upload then costs you the upload, not the map you were
 * already using. */
#define DBC_TMP_PATH        "/frames.tmp"
#define DBC2_TMP_PATH       "/frames2.tmp"

/* CEILINGS on the frame map, not its size.
 *
 * The tables are counted from the file and allocated to fit it (see dbc.h), so
 * a twenty-signal bus costs two kilobytes and only a genuinely large file gets
 * anywhere near these. They exist so a corrupt or hostile file cannot ask for
 * a gigabyte, and so the arithmetic stays in uint16_t.
 *
 * They were 64 / 256 / 128 and FIXED, which cost real data: a field recording
 * of a 104-message, 707-signal steering bus decoded the first 256 signals and
 * wrote the other 451 as raw payload, with one line in the CSV header to say
 * so. Nothing about the numbers was wrong for the bus they were chosen for -
 * being fixed at all was the problem. */
#define DBC_MAX_MESSAGES    256
#define DBC_MAX_SIGNALS     1024
#define DBC_MAX_VALDESC     2048

/* Heap the frame map will NOT take, whatever the file asks for.
 *
 * The map is loaded before the radio starts, so an unbounded map could leave
 * the Wi-Fi stack with nothing and take the web app down to decode a few more
 * signals - the wrong trade in both directions. When the file needs more than
 * the budget allows, the request is scaled down proportionally and the load
 * reports what it kept, which is the same "map did not fit" path that has
 * always existed. Wi-Fi AP plus the web server wants roughly 50 KB; the rest
 * is margin for the writer's buffers and the task stacks. */
#define DBC_HEAP_RESERVE    90000UL
/* Nodes named in BU_. Kept because a DBC says who TRANSMITS each message, and
 * that is the only thing in the file that separates "a reading to watch" from
 * "a command to send". Names are stored once and referenced by index, so this
 * is a fixed table and the only one left in static memory - 32 names is a
 * kilobyte. Eight was not enough: a real J1939 steering bus named 24. */
#define DBC_MAX_NODES       32

/* ---------------------------------------------------------------------------
 *  4. CANopen
 *
 *  An optional framing layer for buses that speak CANopen. It does NOT need a
 *  node map: the COB-ID itself carries the function code and the node ID, so
 *  0x18C is decoded as "TPDO1 from node 12" on any network.
 *
 *  It only ever fills in what the DBC did not: a frame described by the DBC is
 *  decoded from the DBC, always. Turn it off for a plain CAN bus so that
 *  unknown identifiers stay plain hex instead of being labelled with CANopen
 *  names that mean nothing there.
 * -------------------------------------------------------------------------*/
#define CANOPEN_DECODE      0       /* 1 = label unknown ids CANopen-style   */

/* ---------------------------------------------------------------------------
 *  5. CSV OUTPUT
 * -------------------------------------------------------------------------*/
/* Repeat the raw payload bytes on the first row of every decoded frame. Costs
 * ~17 bytes per frame and makes the CSV independently re-decodable later, which
 * is worth it when a factor or a start bit in the DBC is still in doubt. Rows
 * for frames that could not be decoded always carry the raw bytes regardless. */
#define CSV_INCLUDE_RAW     0

/* ---------------------------------------------------------------------------
 *  6. SD CARD / STORAGE
 * -------------------------------------------------------------------------*/
/* Starting point only. Most micro-SD breakout boards carry a 3V3 regulator and
 * level shifters and want 5V on VCC - the ESP32's VIN pin - even though the SPI
 * lines themselves are 3V3. Wired to 3V3 they often brown out under a write and
 * look exactly like a missing card.
 *
 * recorderBeginSD() falls back to 10, 4 and 1 MHz if the card will not mount at
 * this speed, because breadboard jumpers and cheap adapters frequently will not
 * carry 20 MHz and the failure is indistinguishable from "no card". */
#define SD_SPI_HZ           20000000UL

/* CSV rows accumulate in RAM and are handed to the card one full block at a
 * time. 32 KB / ~45 bytes per row = ~700 rows of data per write.
 *
 * Raised from 8 KB for the second bus. SD cards are far more efficient with
 * large writes - the erase-block and wear-levelling work is per write, not per
 * byte - and two busy buses can ask for several hundred kilobytes a second,
 * which is the regime where that stops being a nicety. */
#define SD_BLOCK_BYTES      32768

/* ---- surviving a sudden power cut --------------------------------------
 *
 *  Two different things have to reach the card, and only the second one makes
 *  a recording readable again:
 *
 *    1. the rows still sitting in the RAM block buffer  -> flushBuffer()
 *    2. the FAT and the directory entry, which is what records the file's new
 *       LENGTH. Bytes handed to write() may already be on the card, but until
 *       the metadata is synced the file still reports its old size and a
 *       remount truncates everything written since -> File::flush()
 *
 *  So this interval, not SD_BLOCK_BYTES, is what actually bounds the damage:
 *  an unannounced power loss costs up to this many ms of data.
 *
 *  A sync rewrites 2-3 sectors. At 1 Hz that is well under 1 % duty and the
 *  frame queue absorbs it completely, so 1000 ms is a much better default than
 *  the 5000 ms it is tempting to pick. Below ~250 ms it starts to cost real
 *  bandwidth and card wear for very little extra safety - use the power-fail
 *  input below instead if you need a hard guarantee. */
#define SD_SYNC_INTERVAL_MS 1000

/* Optional POWER-FAIL INPUT - the only way to lose nothing at all.
 *
 *  Feed the logger from a bulk capacitor behind a diode, and bring a divided
 *  copy of the *upstream* supply into this pin. When the machine's supply
 *  collapses the pin changes state while the capacitor still holds the ESP32
 *  up, and the logger immediately flushes and closes the files.
 *
 *  Sizing: the emergency close needs ~50 ms to be comfortable. Holding 150 mA
 *  for 50 ms across a 5 V -> 3.6 V droop needs C = I*t/dV = 0.15*0.05/1.4
 *  ~= 5400 uF, so a 6800 uF / 10 V electrolytic is a sensible choice.
 *
 *  Set to -1 to disable (default): with no such circuit the pin would float and
 *  fire spuriously. */
#define PIN_POWER_FAIL          -1
#define POWER_FAIL_ACTIVE_LOW    1      /* 1 = supply is good while pin HIGH */

/* ---------------------------------------------------------------------------
 *  7. QUEUES / TASKS
 * -------------------------------------------------------------------------*/
/* Raw frames buffered between the CAN reader task and the decode/write task.
 *
 * 2048 * 24 B = 48 KB. Sized against the SD card, not against the bus: a single
 * write on a healthy card is under 5 ms, but the outliers are ~320 ms, and the
 * queue exists entirely to absorb those. Two buses at 500 kbit/s and 80 % load
 * deliver ~6250 frames/s, so 2048 entries is ~330 ms - just past the worst
 * stall. At 1024 it would be 164 ms, which is not.
 *
 * This is the number to raise first if `drop` is ever non-zero while
 * `maxWr` shows a long write. Each entry costs 24 bytes. */
#define FRAME_QUEUE_LEN     2048

/* Log lines buffered between any task and the single SD-owning writer task. */
#define LOG_QUEUE_LEN       48
#define LOG_LINE_CHARS      160

/* Lines of "live" log kept in RAM for the web terminal. */
#define WEB_LOG_LINES       80

#define TASK_PRIO_CAN       20      /* CAN reader   - must never be starved  */
#define TASK_PRIO_WRITER    10      /* decode + SD                            */
#define TASK_CORE_CAN       1       /* app core; core 0 is left to Wi-Fi      */
#define TASK_CORE_WRITER    1

#define TASK_STACK_CAN      4096
#define TASK_STACK_WRITER   8192

/* ---------------------------------------------------------------------------
 *  8. LOGGING CADENCE
 * -------------------------------------------------------------------------*/
#define STATUS_PERIOD_MS    1000    /* one compact status line per second     */

/* 115200 rather than something faster: the serial log is one line per second,
 * so there is nothing to gain, and 115200 is the speed every USB-serial chip,
 * driver and terminal handles without argument. */
#define SERIAL_BAUD         115200

/* ---------------------------------------------------------------------------
 *  9. NETWORK DEFAULTS  (overridden by /config.txt on the SD card)
 * -------------------------------------------------------------------------*/
#define DEF_WIFI_MODE       "ap"        /* "ap" = hotspot, "sta" = join Wi-Fi */
#define DEF_STA_SSID        "MyNetwork"
#define DEF_STA_PASS        "MyPassword"
/* Deliberately the same hotspot name and hostname the single-bus logger uses.
 * This firmware is meant to be flashed onto that logger's hardware and judged
 * against it, and a bookmark or a saved Wi-Fi network that stops working on
 * upgrade is friction for no gain. Both are overridable in /config.txt. */
#define DEF_AP_SSID         "CAN-Logger"
#define DEF_AP_PASS         "canlogger"     /* >= 8 chars, or "" for open AP  */
#define DEF_HOSTNAME        "can-logger"
#define DEF_HTTP_PORT       80

/* If mode=sta and the network cannot be joined within this many ms, fall back
 * to hotspot mode so there is always a way in. 0 disables the fallback. */
#define DEF_STA_TIMEOUT_MS  15000

/* ---------------------------------------------------------------------------
 *  10. BEHAVIOUR
 * -------------------------------------------------------------------------*/
/* Begin recording automatically as soon as the SD card and CAN controller are
 * up, so nothing is missed on power-up. The web UI can stop/start afterwards. */
#define AUTO_START_RECORDING 1

/* The bus counts as alive if a frame arrived within this window. Drives the
 * green/red indicator on the web page. */
#define CAN_ALIVE_TIMEOUT_MS 500

/* How many distinct identifiers the live bus-activity table tracks. Frames with
 * ids beyond this are still recorded in full; they just do not get their own
 * row on the dashboard. */
#define BUS_TRACK_IDS       24

/* How many decoded signals the live page shows at once. The rest are still
 * decoded and written to the CSV - this only bounds the size of the status
 * document the browser polls twice a second. */
#define WEB_MAX_SIGNALS     48

/* ---------------------------------------------------------------------------
 *  11. OVER-THE-AIR UPDATES
 *
 *  Flash the board over Wi-Fi instead of USB. This exists because a DevKit's
 *  auto-reset circuit can fail (a dead transistor on the GPIO0 side leaves the
 *  board flashable only by holding BOOT by hand), and because pressing a button
 *  on a machine in the field is not a workflow.
 *
 *  After ONE successful USB flash of a build with this enabled, every
 *  subsequent upload goes over the network.
 *
 *  REQUIRES AN OTA-CAPABLE PARTITION TABLE - two app slots:
 *      PlatformIO   board_build.partitions = min_spiffs.csv   (already set)
 *      Arduino IDE  Tools > Partition Scheme >
 *                   "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"
 *  The "Huge APP" scheme has a single app slot and CANNOT do OTA.
 *
 *  Recording is stopped and the files closed before an update starts - an OTA
 *  rewrites the flash, and a half-written CSV must not survive into that.
 * -------------------------------------------------------------------------*/
#define ENABLE_OTA          1

/* Leave empty for no password. Anyone on the same network (or joined to the
 * hotspot) can otherwise reflash the logger. */
#define OTA_PASSWORD        "canlogger"

/* ---------------------------------------------------------------------------
 *  12. THE OPERATOR'S DASHBOARD
 *
 *  A grid of cells, each drawing one signal from the frame map the way the
 *  user asked for it. Nothing here is bus-specific either: the layout is a
 *  file, the same way the frame map is.
 *
 *  The layout lives in the ESP32's own NVS flash so it survives a card swap,
 *  and is mirrored to DASH_PATH on the card so it can be edited in a text
 *  editor and copied between loggers. See dash.h for which one wins.
 * -------------------------------------------------------------------------*/
#define DASH_PATH           "/dash.cfg"

/* Written first, then renamed over DASH_PATH. A power cut partway through a
 * direct write would leave a half-parsed layout on the card, which the boot
 * rule would then read as somebody's edit and import over the good copy. */
#define DASH_TMP_PATH       "/dash.tmp"

/* Grid limits. Six across is about where a cell stops being readable on a
 * phone; eight down is more than fits on any screen without scrolling, which
 * is the point at which a dashboard has stopped being a dashboard.
 *
 * DASH_MAX_CELLS covers the whole 6x8 grid. It used to be 36 - less than
 * cols x rows - because the configuration sat in static RAM and static RAM is
 * the binding constraint on this chip: the first version of the dashboard
 * overflowed `dram0_0_seg` outright. It now lives on the heap (see dash.h), so
 * the grid no longer has to be clipped to fit the segment.
 *
 * Raising these is bounded by the heap rather than by the link step, so the
 * honest test is the free-memory figure on the Dashboard tab after a boot with
 * a full configuration loaded, not a build that succeeds. */
#define DASH_MAX_COLS       6
#define DASH_MAX_ROWS       8
#define DASH_MAX_CELLS      48

/* How often the browser asks for the cell values, and the floor it is clamped
 * to. 200 ms is five updates a second: a needle with a CSS transition across
 * that interval looks continuous, and the request itself only carries the
 * cells actually on screen, so it is far cheaper than the status document.
 * Below ~100 ms the ESP32 pays real time for smoothness nobody can see. */
#define DASH_POLL_MS        200
#define DASH_STALE_MS       3000    /* a cell fades if its message stops     */
#define DASH_POLL_MIN_MS    100

/* Longest BU_ node name the role can hold. A role longer than this simply
 * cannot be selected, which is why it is generous rather than tight. */
#define DASH_ROLE_MAX       32

/* Serialised size of the whole configuration - layout and setpoints. Also the
 * size of the buffer it is built in, so it is charged to the stack of whoever
 * saves, not held permanently. */
#define DASH_CFG_MAX        6144

/* ---------------------------------------------------------------------------
 *  13. SENDING VALUES BACK TO THE BUS
 *
 *  Everything above is passive. This is not: it lets the dashboard write a
 *  value into an ECU - a tyre size, a limit, a calibration - while a recording
 *  is running, so the change and its effect land in the same file.
 *
 *  READ THIS BEFORE ENABLING IT ON A MACHINE. A CAN frame sent to a live
 *  controller can move hydraulics, release a brake or enable a drive. The
 *  logger cannot know which; it sends what it is told. The protections it does
 *  offer are:
 *
 *    - CAN_LISTEN_ONLY above overrides everything here. A logger in listen-only
 *      mode physically cannot drive the bus, and Send says so rather than
 *      failing quietly.
 *    - the dashboard must be ARMED before any Send button works, and disarms
 *      itself again after TX_ARM_TIMEOUT_MS without a send,
 *    - every arm, disarm and send is written to the recording's .log,
 *    - one-shot mode in the controller means an unacknowledged frame is
 *      reported once instead of being retried into bus-off (see mcp2515.h).
 * -------------------------------------------------------------------------*/
/* Saved values the Send tab offers.
 *
 * 32, and 32 is a hard ceiling rather than a budget: whether a value is
 * repeating is carried as a bit in a 32-bit mask, in the firmware and in the
 * browser alike, and JavaScript's bitwise operators are 32-bit whatever you do
 * to them. Going further means a different representation, not a bigger number
 * here.
 *
 * It used to be ten, because each one costs about 200 bytes and they sat in
 * static RAM. They are on the heap now (see dash.h), which is what paid for
 * the other twenty-two. */
#define TX_MAX_COMMANDS     32

/* The option list behind a "pick one of these" input, as text. Enough for six
 * or so labelled choices - more than anyone should be asked to scan while
 * standing next to a running machine. */
#define TX_CHOICES_MAX      80

/* Requests queued from the web handler to the CAN task, which is the only task
 * allowed to touch the controller. Small on purpose: this is an operator
 * pressing a button, not a data path. */
#define TX_QUEUE_LEN        8

/* Results kept for the browser to collect. The dashboard polls several times a
 * second and reads four at a time, so a burst cannot outrun this. */
#define TX_RESULT_RING      4

/* How long the dashboard stays armed with nothing being sent. Long enough to
 * work through a set of values, short enough that a forgotten browser tab on a
 * phone in someone's pocket is not still armed an hour later. */
#define TX_ARM_TIMEOUT_MS   300000UL

/* Fastest a cyclic setpoint may repeat. The CAN task wakes every 20 ms, so
 * anything below that would simply be rounded up to it. */
#define TX_CYCLIC_MIN_MS    50

/* Attempts before a send is reported as having lost arbitration. Losing it once
 * on a busy bus is normal; losing it three times running means the identifier
 * is too low a priority to get on the wire. */
#define TX_ATTEMPTS         3

#define FIRMWARE_NAME    "Dual CAN Logger ESP32"
#define FIRMWARE_VERSION "2.0.0"
