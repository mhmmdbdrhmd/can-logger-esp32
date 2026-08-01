/* ============================================================================
 *  config.h - compile-time configuration
 *
 *  Everything a user normally changes lives here or in two files on the SD
 *  card:
 *
 *      /frames.dbc   the frame map (optional - without it the logger records
 *                    raw payload bytes and decodes nothing)
 *      /config.txt   Wi-Fi credentials and hostname
 *
 *  Nothing below is specific to any particular bus. The defaults describe a
 *  plain 250 kbit/s classical CAN network and a stock ESP32 DevKit v1.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* ---------------------------------------------------------------------------
 *  1. PIN MAP  (ESP32 DevKit v1)
 *
 *   MCP2515 CAN module          ESP32            SD card module      ESP32
 *   ------------------          -----            --------------      -----
 *     VCC ....................  3V3                VCC ............. 3V3
 *     GND ....................  GND                GND ............. GND
 *     CS  ....................  D5                 CS  ............. D4
 *     INT ....................  D17                SCK ............. D14
 *     SCK ....................  D18  (VSPI)        MISO ............ D27
 *     MISO ...................  D19                MOSI ............ D13
 *     MOSI ...................  D23                                  (HSPI)
 *
 *  Two independent SPI buses on purpose: the CAN controller is read from a
 *  high-priority task while the SD card is written from a lower one, and
 *  sharing one bus would serialise them - an 8 KB SD write would stall CAN
 *  reads for milliseconds, which at 1000 frames/s is measured in lost frames.
 * -------------------------------------------------------------------------*/
#define PIN_CAN_CS      5
#define PIN_CAN_INT     17
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
 * The only other strapping pin in this map is GPIO5 (PIN_CAN_CS), which must be
 * HIGH at reset - an MCP2515 chip-select idles high, so that matches. */
#define PIN_STATUS_LED  2

/* ---------------------------------------------------------------------------
 *  2. CAN BUS
 * -------------------------------------------------------------------------*/
#define CAN_BITRATE_KBPS    250     /* 100, 125, 250, 500 or 1000            */

/* Crystal soldered on your MCP2515 board. The common blue "MCP2515 + TJA1050"
 * modules ship with 8 MHz; some clones use 16 MHz. If the logger reports "no
 * CAN traffic" but the bus is alive, this is the first thing to change. */
#define CAN_CRYSTAL_MHZ     8       /* 8 or 16 */

/* SPI clock for the MCP2515. The datasheet allows 10 MHz; at 10 MHz a full
 * frame read takes ~15 us, which comfortably keeps up with 1000 frames/s. */
#define CAN_SPI_HZ          10000000UL

/* 0 = normal mode: the logger acknowledges frames, which is what you want when
 *     it is the only other node on the bus (otherwise the talker goes
 *     error-passive and eventually bus-off).
 * 1 = listen-only: fully passive, never drives the bus. Correct when another
 *     node is already acknowledging, and the safe choice on a live machine. */
#define CAN_LISTEN_ONLY     0

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
#define DBC_PATH            "/frames.dbc"

/* Static capacity of the frame map. Sized for a comfortable machine bus; raise
 * if the parser reports "table full", but watch the heap - the tables are
 * roughly DBC_MAX_MESSAGES*48 + DBC_MAX_SIGNALS*72 + DBC_MAX_VALDESC*40 bytes,
 * about 25 KB at these defaults. */
#define DBC_MAX_MESSAGES    64
#define DBC_MAX_SIGNALS     256
#define DBC_MAX_VALDESC     128

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
#define SD_SPI_HZ           20000000UL

/* CSV rows accumulate in RAM and are handed to the card one full block at a
 * time. 8 KB / ~40 bytes per row = ~200 rows of data per write. */
#define SD_BLOCK_BYTES      8192

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
 * 1024 * 24 B = 24 KB = a full second of traffic at 1000 frames/s. If the SD
 * card stalls for a second (cheap cards do), nothing is lost. */
#define FRAME_QUEUE_LEN     1024

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

#define FIRMWARE_NAME    "CAN Logger ESP32"
#define FIRMWARE_VERSION "1.0.0"
