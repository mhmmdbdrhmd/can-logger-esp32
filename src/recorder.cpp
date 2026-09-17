#include "recorder.h"
#include "config.h"
#include "logger.h"
#include "decode.h"
#include "dbc.h"
#include "netcfg.h"
#include "dash.h"
#include "dashstore.h"
#include "cantx.h"
#include "psram.h"
#include "mem.h"

#include <SPI.h>
#include <SD.h>
#include "sdutil.h"

RecStatus     g_rec;
QueueHandle_t g_frameQueue = nullptr;

static SPIClass s_sdSpi(HSPI);

static File     s_csv;
static File     s_log;
static Decoder  s_dec;

/* One CSV staging buffer. A second one would only help if the SD write were
 * asynchronous, which it is not; the real decoupling from the CAN path is the
 * one-second-deep frame queue in front of this task. The tail beyond
 * SD_BLOCK_BYTES is headroom so a single frame's worth of rows always fits
 * without a mid-frame flush. */
static char     s_buf[SD_BLOCK_BYTES + DECODE_FRAME_MAX];
static size_t   s_used = 0;

static volatile bool s_wantStart = false;
static volatile bool s_wantDashSave = false;
static volatile bool s_wantStop  = false;
static volatile bool s_powerFail = false;

static uint32_t s_lastSyncMs   = 0;

/* Where the rolling id list in the per-second debug line starts. A hundred ids
 * do not fit on one line and never will; showing a different nine each second
 * beats showing the same nine for ever. */
static uint8_t  s_idCursor[CAN_BUSES] = { 0, 0 };
static uint32_t s_lastStatusMs = 0;
static uint32_t s_framesAtLastStatus[CAN_BUSES] = { 0, 0 };
static uint32_t s_irqAtLastStatus[CAN_BUSES]    = { 0, 0 };
static uint32_t s_loopAtLastStatus = 0;

static uint32_t s_lostSeen  = 0;   /* frames lost that have been reported     */
static uint8_t  s_lostSaid  = 0;
static uint32_t s_wakeAtLastStatus   = 0;
static bool     s_warnedIntStuck[CAN_BUSES]     = { false, false };
static uint64_t s_bitsAtLastStatus[CAN_BUSES]   = { 0, 0 };

void recorderRequestStart() { s_wantStart = true; s_wantStop = false; }
void recorderRequestSaveDash() { s_wantDashSave = true; }
void recorderRequestStop()  { s_wantStop  = true; s_wantStart = false; }
bool recorderStartPending() { return s_wantStart; }
void recorderSignalPowerFail() { s_powerFail = true; }

bool recorderStopAndWait(uint32_t timeoutMs) {
  if (!g_rec.recording) return true;
  recorderRequestStop();

  /* The writer task runs at a higher priority than any caller of this, so it
   * preempts us and does the work; we only have to wait for it to finish. */
  const uint32_t t0 = millis();
  while (g_rec.recording && (millis() - t0) < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return !g_rec.recording;
}

uint32_t recorderElapsedMs() {
  return g_rec.recording ? (millis() - g_rec.startMs) : 0;
}

/* ------------------------------------------------------------------------ *
 *  Unstick a card that was cut off mid-write
 *
 *  A serial upload is not a graceful shutdown. esptool pulls EN low through
 *  the auto-reset transistors at whatever instant it likes, and with
 *  AUTO_START_RECORDING this firmware is almost always in the middle of a
 *  32 KB block write when that happens. The ESP32 restarts; the SD card does
 *  not, because its 3V3 rail never dropped. It is still sitting in the data
 *  phase of a multi-block write, holding DO low to say "busy", and it will
 *  ignore CMD0 forever. f_mount then fails with FR_DISK_ERR (1) - "a hard
 *  error occurred in the low level disk I/O layer" - which reads like a dead
 *  card but is really an unfinished sentence.
 *
 *  The cure is the one the SD spec gives for exactly this: clock the card
 *  until it lets go. Phase 1 holds CS low and shifts 0xFF until DO releases,
 *  finishing the transaction the reset interrupted. Phase 2 raises CS and
 *  sends the >=74 idle clocks the card needs before it will accept CMD0
 *  again. Bit-banged on purpose - this runs before the SPI peripheral is
 *  attached to the pins, and it must work even when the driver's own state
 *  machine is the thing that is confused.
 *
 *  Costs about 8 ms in the worst case and nothing in the common one. An OTA
 *  update never needs it: onStart() closes the file first, which is why
 *  flashing over the air has always mounted cleanly and flashing over USB
 *  did not.                                                                */
static void sdBusRecover() {
  pinMode(PIN_SD_CS,   OUTPUT);
  pinMode(PIN_SD_SCK,  OUTPUT);
  pinMode(PIN_SD_MOSI, OUTPUT);
  pinMode(PIN_SD_MISO, INPUT_PULLUP);

  digitalWrite(PIN_SD_MOSI, HIGH);        /* 0xFF on the line throughout */
  digitalWrite(PIN_SD_SCK,  LOW);

  /* Phase 1: card selected, clock until DO goes high (not busy) and stays
   * there for a full byte. ~200 kHz, capped so a genuinely absent or dead
   * card cannot stall the boot. */
  digitalWrite(PIN_SD_CS, LOW);
  uint16_t highRun = 0;
  uint16_t clocks  = 0;
  for (; clocks < 8192 && highRun < 8; clocks++) {
    digitalWrite(PIN_SD_SCK, HIGH); delayMicroseconds(2);
    highRun = digitalRead(PIN_SD_MISO) ? (uint16_t)(highRun + 1) : 0;
    digitalWrite(PIN_SD_SCK, LOW);  delayMicroseconds(2);
  }

  /* Phase 2: deselect, then the mandatory idle clocks. */
  digitalWrite(PIN_SD_CS, HIGH);
  for (uint16_t i = 0; i < 160; i++) {
    digitalWrite(PIN_SD_SCK, HIGH); delayMicroseconds(2);
    digitalWrite(PIN_SD_SCK, LOW);  delayMicroseconds(2);
  }

  /* Only worth a line when it actually did something. A card that was idle
   * releases within the first byte or two. */
  if (clocks > 64) {
    LOG_LIVE(LVL_WARN, "SD card was still busy from an interrupted write - "
                       "took %u clocks to release it. Normal after flashing "
                       "over USB while recording.", (unsigned)clocks);
  }
}

/* ------------------------------------------------------------------------ */
bool recorderBeginSD() {
  sdBusRecover();
  s_sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  /* Try progressively slower clocks before giving up.
   *
   * SD.begin() reports the same failure for "there is no card" and "this
   * wiring will not carry 20 MHz", which sends you hunting for a dead card or
   * a wrong CS pin when the real fault is 10 cm of breadboard jumper, a cheap
   * adapter board with no level shifter, or a long ribbon to a panel-mounted
   * slot. All of those work perfectly at 4 MHz. Losing a little write
   * throughput beats not mounting at all - and the log says which one it took,
   * so a card that needed 1 MHz is not a silent mystery later. */
  static const uint32_t SPEEDS[] = { SD_SPI_HZ, 10000000UL, 4000000UL, 1000000UL };

  /* Several ROUNDS of the speed ladder, each after a longer settle.
   *
   * One round was not enough. After a USB flash the card regularly comes up
   * in a state where every speed fails:
   *
   *     [E][sd_diskio.cpp:199] sdCommand(): Card Failed! cmd: 0x00
   *     [E][sd_diskio.cpp:806] f_mount failed: (3) The physical drive cannot work
   *     E NO SD CARD at any clock down to 1000 kHz
   *
   * and the consequence is out of all proportion to the cause: with no card
   * there is no /config.txt, so the firmware falls back to compiled-in
   * defaults, those say hotspot, and the board sits on its own AP recording
   * nothing. An entire experiment run produces no data and looks like a
   * Wi-Fi fault. It has cost two runs that way.
   *
   * A plain reset clears it every time - measured three for three on this
   * board. The only thing a reset gives the card that the old single round
   * did not is TIME: four attempts 50 ms apart is about 1.7 seconds, while a
   * reset is several hundred milliseconds of quiet with the lines idle
   * followed by a clean initialisation sequence.
   *
   * So: the same ladder, up to three times, settling 50 ms, then 300, then
   * 800 before each round. Worst case adds about a second to a boot that was
   * going to fail anyway, and turns the common case from "no card" into "card
   * mounted, second round". */
  static const uint16_t SETTLE_MS[] = { 50, 300, 800 };
  const uint8_t ROUNDS = sizeof(SETTLE_MS) / sizeof(SETTLE_MS[0]);
  const uint8_t NSPEED = sizeof(SPEEDS)    / sizeof(SPEEDS[0]);

  bool mounted = false;
  for (uint8_t r = 0; r < ROUNDS && !mounted; r++) {
    for (uint8_t i = 0; i < NSPEED; i++) {
      if (SD.begin(PIN_SD_CS, s_sdSpi, SPEEDS[i])) {
        if (i || r) {
          LOG_LIVE(LVL_WARN, "SD card mounted at %lu kHz on round %u of %u "
                             "(wanted %lu kHz on the first try) - check the "
                             "wiring if writes cannot keep up",
                   (unsigned long)(SPEEDS[i] / 1000UL),
                   (unsigned)(r + 1), (unsigned)ROUNDS,
                   (unsigned long)(SD_SPI_HZ / 1000UL));
        }
        mounted = true;
        break;
      }
      SD.end();
      s_sdSpi.end();
      delay(SETTLE_MS[r]);  /* let the card settle before re-clocking it */
      sdBusRecover();       /* and clear anything the failed attempt left */
      s_sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    }
    if (!mounted && r + 1 < ROUNDS) {
      LOG_LIVE(LVL_WARN, "no SD card after round %u - waiting %u ms and trying "
                         "the whole ladder again. This is usually the first "
                         "boot after a USB flash, not a missing card.",
               (unsigned)(r + 1), (unsigned)SETTLE_MS[r + 1]);
      delay(SETTLE_MS[r + 1]);
      sdBusRecover();
    }
  }

  if (!mounted) {
    LOG_LIVE(LVL_ERROR, "NO SD CARD after %u rounds down to %lu kHz - check "
                        "that the module is powered (many need 5V/VIN, not "
                        "3V3), that the card is FAT32, and CS=D%d. NOTHING "
                        "WILL BE SAVED, and with no /config.txt the network "
                        "settings fall back to the compiled-in defaults.",
             (unsigned)ROUNDS,
             (unsigned long)(SPEEDS[NSPEED - 1] / 1000UL),
             PIN_SD_CS);
    g_rec.sdOk = false;
    return false;
  }

  switch (SD.cardType()) {
    case CARD_MMC:  g_rec.sdType = "MMC";   break;
    case CARD_SD:   g_rec.sdType = "SDSC";  break;
    case CARD_SDHC: g_rec.sdType = "SDHC";  break;
    default:        g_rec.sdType = "NONE";  break;
  }
  /* Mounted, but the card answers as no card - release the bus rather than
   * leaving a half-initialised SD driver holding it. */
  if (SD.cardType() == CARD_NONE) { SD.end(); g_rec.sdOk = false; return false; }

  g_rec.sdSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
  g_rec.sdOk     = true;
  g_rec.sdError  = false;
  return true;
}

/* ------------------------------------------------------------------------ *
 *  The frame map
 * ------------------------------------------------------------------------ */
/* A frame map is read twice - once to count, once to parse - and the obvious
 * way to do that is File::read() one byte at a time. On the ESP32 that is one
 * VFS syscall per byte through FatFs, roughly 25 us each. It is invisible on
 * the 6 KB map this project ships and ruinous on a real vendor DBC: a
 * megabyte-class file costs about two million round-trips per pass, which
 * measured at 53 SECONDS per pass on hardware - 107 s of boot before the radio
 * started or a single frame was recorded.
 *
 * Reading in 512-byte blocks turns those two million syscalls into two
 * thousand. The buffer has to be part of the reader rather than hidden inside
 * readLine(), because the loader seeks back to 0 between the passes and a
 * stale buffer would silently re-serve bytes from the first one. */
struct LineReader {
  File   *f;
  size_t  pos;
  size_t  len;
  uint8_t buf[512];
};

static void lrBegin(LineReader &r, File &f) { r.f = &f; r.pos = 0; r.len = 0; }

static void lrRewind(LineReader &r) {
  r.f->seek(0);
  r.pos = 0;
  r.len = 0;
}

static int lrGet(LineReader &r) {
  if (r.pos >= r.len) {
    const size_t n = r.f->read(r.buf, sizeof(r.buf));
    if (n == 0) return -1;
    r.len = n;
    r.pos = 0;
  }
  return r.buf[r.pos++];
}

static bool readLine(LineReader &r, char *buf, size_t cap) {
  size_t n = 0;
  bool   any = false;
  for (;;) {
    const int c = lrGet(r);
    if (c < 0) break;
    any = true;
    if (c == '\n') break;
    if (c != '\r' && n + 1 < cap) buf[n++] = (char)c;
  }
  buf[n] = '\0';
  return any;
}

/* Loads ONE bus's frame map.
 *
 * Called once per bus, in order, and the heap arithmetic below reads the live
 * free heap - so the second map is fitted to what the first one left rather
 * than to what the board had at boot. That is the correct joint budget and it
 * needs no separate bookkeeping, but it does mean a very large map on CAN1 is
 * the one that shrinks CAN2's. Said out loud in the log when it happens. */
static void loadOneDbc(uint8_t bus, const char *path) {
  DbcDb     &db = g_dbc[bus];
  BusHealth &bh = g_rec.bus[bus];
  const unsigned busNo = (unsigned)(bus + 1);

  dbcReset(db);
  bh.dbcLoaded   = false;
  bh.dbcMessages = 0;
  bh.dbcSignals  = 0;

  if (!g_rec.sdOk) return;

  if (!SD.exists(path)) {
    LOG_LIVE(LVL_INFO, "CAN%u: no %s on the card - recording raw payload bytes. "
                       "Add a DBC to decode this bus in real time.",
             busNo, path);
    return;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    LOG_LIVE(LVL_WARN, "CAN%u: could not open %s - recording raw payload bytes",
             busNo, path);
    return;
  }

  const size_t fileBytes = f.size();

  static char       line[DBC_LINE_MAX];
  static LineReader lr;
  lrBegin(lr, f);

  /* FIRST PASS: count. The tables are then sized to this file rather than to a
   * number picked at compile time, which is what stops a 707-signal bus being
   * decoded 256 signals deep and logged raw for the rest. Reading the file
   * twice costs a fraction of a second off an SD card and happens once at
   * boot - in 512-byte blocks, see LineReader above, because doing it a
   * byte at a time makes a large map take minutes rather than seconds. */
  DbcCounts want = {0, 0, 0};
  uint32_t   lines = 0;
  while (readLine(lr, line, sizeof(line))) {
    dbcCountLine(line, want);
    if (++lines > DBC_MAX_LINES) break;    /* a runaway file is not a DBC */
  }

  /* A little slack, so a file that gains a signal after being counted - it
   * cannot here, but the arithmetic should not be load-bearing - still fits. */
  if (want.messages < 0xFF00) want.messages = (uint16_t)(want.messages + 4);
  if (want.signals  < 0xFF00) want.signals  = (uint16_t)(want.signals  + 8);
  if (want.values   < 0xFF00) want.values   = (uint16_t)(want.values   + 8);

  /* Fit the request to the heap BEFORE asking for it. This runs before the
   * radio starts, so a map that takes everything would leave the Wi-Fi stack
   * with nothing - a logger that decodes every signal and cannot be reached is
   * not the better half of that trade. Scale down proportionally rather than
   * dropping one table, so a large map degrades evenly. */
  {
    /* PSRAM changes the question entirely. The tables go there when it exists
     * (see psram.h), so the budget is the free PSRAM and the internal-heap
     * reserve does not apply - there is nothing on that side to protect.
     *
     * The arithmetic itself lives in dbcFitToHeap() so it can be tested
     * against this board's real heap figures without an SD card or a board.
     * It used to be written out here, and that is how a DBC_HEAP_RESERVE
     * larger than the free heap came to drop every message on both buses
     * with nothing louder than a fit-down warning. */
    const bool   ps   = psramSize() != 0;
    const size_t heap = ps ? psramFree() : (size_t)memStat().freeNow;

    const DbcCounts asked = want;
    const DbcFit    fit   = dbcFitToHeap(want, heap, DBC_HEAP_RESERVE, ps);

    if (fit == DBC_FIT_NONE) {
      /* NOT a fit-down, and it must not be reported as one. The budget is
       * zero, so EVERY message is dropped and the dashboard comes up with its
       * panels and no values in any of them - which reads as a decode fault
       * rather than the configuration fault it is. */
      LOG_LIVE(LVL_ERROR, "NO frame map will be loaded for this bus: "
                          "DBC_HEAP_RESERVE is %lu bytes but only %lu bytes of "
                          "%s are free, so the budget is zero and all %u "
                          "message(s) are dropped. This is a CONFIGURATION "
                          "fault, not a file fault - lower DBC_HEAP_RESERVE in "
                          "config.h. Recording continues as raw payload bytes.",
               (unsigned long)DBC_HEAP_RESERVE, (unsigned long)heap,
               ps ? "PSRAM" : "internal heap", (unsigned)asked.messages);
      db.overflow = 1;
    } else if (fit == DBC_FIT_PART) {
      const size_t need = (size_t)asked.messages * dbcBytesPerMessage()
                        + (size_t)asked.signals  * dbcBytesPerSignal()
                        + (size_t)asked.values   * dbcBytesPerValue();
      const size_t room = ps ? heap : heap - DBC_HEAP_RESERVE;
      LOG_LIVE(LVL_WARN, "frame map wants %lu KB but only %lu KB of %s is free"
                         "%s. Keeping what fits - the rest of the bus is still "
                         "RECORDED, just as raw payload, and decodes offline "
                         "against the same DBC.",
               (unsigned long)((need + 1023) / 1024),
               (unsigned long)((room + 1023) / 1024),
               ps ? "PSRAM" : "internal heap",
               ps ? "" : " - a map this size needs a module with PSRAM, no "
                         "ESP32 has that much internal RAM");
      db.overflow = 1;
    }
  }

  const bool sized = dbcAllocate(db, want);
  liveAllocate(g_live[bus], db.sigCap);

  /* SECOND PASS: parse. */
  lrRewind(lr);
  lines = 0;
  while (readLine(lr, line, sizeof(line))) {
    dbcParseLine(db, line);
    if (++lines > DBC_MAX_LINES) break;
  }
  f.close();

  (void)sized;
  bh.dbcLoaded   = db.loaded != 0;
  bh.dbcMessages = db.msgCount;
  bh.dbcSignals  = db.sigCount;

  if (!db.loaded) {
    LOG_LIVE(LVL_WARN, "CAN%u: %s has no BO_ messages - recording raw payload "
                       "bytes", busNo, path);
    return;
  }

  /* What the map COST, not what the file weighed. Those are different numbers
   * and only the first one competes with the radio: dbcBytes() is the three
   * tables, and the live-value slots are allocated alongside them at
   * LIVE_TEXT_MAX + 5 bytes a signal, so leaving them out of the figure
   * understates a 47-signal map by a kilobyte and an 8000-signal one by
   * 168 KB. Largest block is here for the same reason it is everywhere else:
   * it, and not the total, is what the next allocation has to fit into. */
  {
    const size_t liveBytes = (size_t)db.sigCap
                           * (LIVE_TEXT_MAX + sizeof(uint32_t) + 1);
    const MemStat mm = memStat();
    LOG_LIVE(LVL_INFO, "CAN%u frame map: %u messages, %u signals from %s "
                       "(%lu bytes on the card) "
                       "(%lu KB tables + %lu KB live = %lu KB; heap %lu KB "
                       "free, largest block %lu KB)",
             busNo, (unsigned)db.msgCount, (unsigned)db.sigCount, path,
             (unsigned long)fileBytes,
             (unsigned long)((dbcBytes(db) + 1023) / 1024),
             (unsigned long)((liveBytes + 1023) / 1024),
             (unsigned long)((dbcBytes(db) + liveBytes + 1023) / 1024),
             (unsigned long)(mm.freeNow / 1024),
             (unsigned long)(mm.largest / 1024));
  }
  /* The per-entry costs too, in bytes: they are what a desk tool needs to
   * predict this board's heap from a .dbc, and they change with name_max. */
  LOG_FILE(LVL_INFO, "CAN%u dbc: version='%s' values=%u lineErrors=%u inexact=%u "
                     "caps=%u/%u/%u bytes=%lu name_max=%u entry=%u/%u/%u",
           busNo, db.version, (unsigned)db.valCount,
           (unsigned)db.lineErrors, (unsigned)db.inexact,
           (unsigned)db.msgCap, (unsigned)db.sigCap,
           (unsigned)db.valCap, (unsigned long)dbcBytes(db),
           (unsigned)db.nameMax, (unsigned)dbcBytesPerMessage(),
           (unsigned)dbcBytesPerSignal(), (unsigned)dbcBytesPerValue());

  if (db.overflow) {
    /* Now genuinely rare: it means the file exceeded the DBC_MAX_* ceilings in
     * config.h, or the heap could not hold what the file asked for. Either way
     * say what was kept, because "did not fit" without a number is not
     * something anybody can act on. */
    LOG_LIVE(LVL_WARN, "the frame map did NOT fit: kept %u of the messages and "
                       "%u signals it asked for (ceilings %u/%u in config.h, "
                       "%lu KB heap free). Frames beyond it are still recorded, "
                       "as raw bytes.",
             (unsigned)db.msgCap, (unsigned)db.sigCap,
             (unsigned)DBC_MAX_MESSAGES, (unsigned)DBC_MAX_SIGNALS,
             (unsigned long)(memStat().freeNow / 1024));
  }
  if (db.skipped) {
    LOG_LIVE(LVL_WARN, "%u definition(s) in %s were readable but did not fit "
                       "and were skipped - the map is incomplete, not corrupt",
             (unsigned)db.skipped, path);
  }
  if (db.lineErrors) {
    LOG_LIVE(LVL_WARN, "%u line(s) of %s could not be parsed - see the .log",
             (unsigned)db.lineErrors, path);
  }
  if (db.nameClipped) {
    /* Said out loud, because the cost is invisible until somebody matches CSV
     * rows against the source DBC by name and quietly gets none. */
    LOG_LIVE(LVL_WARN, "%u name(s) are longer than %u characters and are cut "
                       "short in the CSV - export the setup bundle with a "
                       "larger name_max, which abbreviates them instead",
             (unsigned)db.nameClipped, (unsigned)(db.nameMax - 1));
  }
}

void recorderLoadDbc() {
  static const char *const kPath[CAN_BUSES] = { DBC_PATH, DBC2_PATH };

  if (!g_rec.sdOk) {
    for (uint8_t b = 0; b < CAN_BUSES; b++) {
      dbcReset(g_dbc[b]);
      g_rec.bus[b].dbcLoaded = false;
    }
    LOG_LIVE(LVL_WARN, "no SD card - recording raw payload bytes, nothing decoded");
    return;
  }

  /* CAN1 first, deliberately: the maps are fitted to the heap in the order
   * they load, so the bus named first is the one that gets the room. */
  for (uint8_t b = 0; b < CAN_BUSES; b++) loadOneDbc(b, kPath[b]);

  bool any = false;
  for (uint8_t b = 0; b < CAN_BUSES; b++) any = any || g_rec.bus[b].dbcLoaded;
  if (!any) {
    LOG_LIVE(LVL_INFO, "no frame map for either bus - every frame is recorded "
                       "as raw payload bytes, which decodes offline just as "
                       "well. Add %s or %s to decode live.",
             DBC_PATH, DBC2_PATH);
  }
}

/* Empties the frame queue without recording anything, still counting each
 * frame for the dashboard. Called between the card operations that start a
 * recording: the writer is the queue's only reader, and those operations are
 * slow enough to fill it. On a card holding a few dozen files the start took
 * 1.5 s - 512 frames at 350/s - and what arrived after that was dropped. A
 * frame taken here belongs to no recording, so nothing is lost: a recording
 * begins when its file is ready, not when it was asked for. */
static void drainUnrecorded() {
  if (!g_frameQueue) return;
  CanFrame f;
  while (xQueueReceive(g_frameQueue, &f, 0) == pdTRUE) {
    const uint8_t fb = (f.bus < CAN_BUSES) ? f.bus : 0;
    if (!f.tx) busObserve(g_bus[fb], f, &g_dbc[fb]);
  }
}

/* Lowest free index: 1.csv, 2.csv, ... A slot counts as taken if either the
 * .csv or the .log exists, so the pair always shares a number. Every lookup is
 * a directory search on the card, so the queue is drained between them. */
static uint16_t nextFileIndex() {
  char a[20], b[20], c[20];
  for (uint16_t i = 1; i < 10000; i++) {
    drainUnrecorded();
    snprintf(a, sizeof(a), "/%u.csv",  i);
    snprintf(b, sizeof(b), "/%u.log",  i);
    snprintf(c, sizeof(c), "/%u.meta", i);
    if (!SD.exists(a) && !SD.exists(b) && !SD.exists(c)) return i;
  }
  return 0;
}

/* ------------------------------------------------------------------------ */
#if SD_FAULT_TEST_AT_KB
static bool s_faultDone = false;
#endif

/* Close the CSV and open it again for appending.
 *
 * FatFS remembers a failed write on the open file and refuses every later
 * write to it, for good - while the .log beside it, a different open file,
 * carries on normally. On the bench that turned one card hiccup at 4 min 30 s
 * into a recording that silently stopped growing for the remaining five
 * minutes, with `lost 0` on the status line. A fresh open clears it. */
static bool reopenCsv() {
  s_csv.close();
  s_csv = SD.open(g_rec.csvName, FILE_APPEND);
  return (bool)s_csv;
}

static void flushBuffer(bool force) {
  if (!s_used) return;
  if (!force && s_used < SD_BLOCK_BYTES) return;
  if (!s_csv) { s_used = 0; return; }

#if SD_FAULT_TEST_AT_KB
  if (!s_faultDone && g_rec.bytes >= (uint64_t)SD_FAULT_TEST_AT_KB * 1024ULL) {
    s_faultDone = true;
    s_csv.close();
    s_csv = SD.open(g_rec.csvName, FILE_READ);   /* writes now return 0 */
    LOG_LIVE(LVL_WARN, "TEST: the CSV now refuses writes, as after a card error");
  }
#endif

  const uint32_t t0 = micros();
  size_t n = s_csv.write((const uint8_t *)s_buf, s_used);

  if (n != s_used) {
    g_rec.sdWriteFails++;
    const bool again = reopenCsv();
    if (again) n += s_csv.write((const uint8_t *)s_buf + n, s_used - n);
    if (n != s_used) {
      g_rec.sdError = true;
      g_rec.sdBytesLost += (uint32_t)(s_used - n);
    }
    LOG_LIVE(n == s_used ? LVL_WARN : LVL_ERROR,
             "SD write failed at %lu KB of %s; reopened it %s - %s%lu KB not "
             "recorded so far",
             (unsigned long)(g_rec.bytes / 1024ULL), g_rec.csvName,
             again ? "and wrote the block" : "- COULD NOT reopen it",
             n == s_used ? "recovered, " : "",
             (unsigned long)(g_rec.sdBytesLost / 1024UL));
    if (n != s_used && !again) {
      LOG_LIVE(LVL_ERROR, "card full or removed?");
    }
  }
  const uint32_t dt = micros() - t0;
  g_rec.bytes += n;
  g_rec.writeCount++;
  g_rec.sdBusyUs += dt;       /* a SUBSET of writerBusyUs - see recorder.h */
  if (dt > g_rec.writeMaxUs) g_rec.writeMaxUs = dt;
  if (dt > 100000UL) {
    LOG_FILE(LVL_WARN, "slow SD write: %u bytes took %lu us",
             (unsigned)s_used, (unsigned long)dt);
  }
  s_used = 0;
}

/* Pushes the RAM buffer out AND commits the metadata, which is the part that
 * actually makes the bytes readable after an unclean shutdown. Everything
 * written before this returns survives a power cut; everything after it is at
 * risk until the next call. */
static void syncToCard() {
  flushBuffer(true);
  const uint32_t t0 = micros();
  if (s_csv) s_csv.flush();
  if (s_log) s_log.flush();
  const uint32_t dt = micros() - t0;

  g_rec.syncCount++;
  g_rec.sdBusyUs += dt;       /* the commit is card time too, not decode time */
  if (dt > g_rec.syncMaxUs) g_rec.syncMaxUs = dt;
  s_lastSyncMs = millis();
}

static void startRecording() {
  if (g_rec.recording) return;
  if (!g_rec.sdOk && !recorderBeginSD()) {
    LOG_LIVE(LVL_ERROR, "cannot start: no SD card");
    return;
  }

  const uint16_t idx = nextFileIndex();
  if (!idx) { LOG_LIVE(LVL_ERROR, "cannot start: no free file index"); return; }

  snprintf(g_rec.csvName,  sizeof(g_rec.csvName),  "/%u.csv",  idx);
  snprintf(g_rec.logName,  sizeof(g_rec.logName),  "/%u.log",  idx);
  snprintf(g_rec.metaName, sizeof(g_rec.metaName), "/%u.meta", idx);

  /* What a recording itself costs the heap, step by step: open files carry
   * buffers, and the web server competes for the same memory. */
  memLog(LVL_INFO, true, "recording, before opening files");
  s_csv = SD.open(g_rec.csvName, FILE_WRITE);
  if (!s_csv) { LOG_LIVE(LVL_ERROR, "cannot create %s", g_rec.csvName); return; }
  drainUnrecorded();
  memLog(LVL_INFO, true, "recording, the .csv open");

  s_log = SD.open(g_rec.logName, FILE_WRITE);
  drainUnrecorded();
  memLog(LVL_INFO, true, "recording, the .log open");
  if (!s_log) {
    LOG_LIVE(LVL_WARN, "cannot create %s - continuing without the detailed log",
             g_rec.logName);
  } else {
    logAttachFile(&s_log);
    /* The boot heap ladder happened before this file existed. Put it in now,
     * so a recording is self-contained and the one figure that predicts the
     * web UI does not depend on someone having captured serial. */
    memReplayBoot();
  }

  /* Self-describing preamble, written before a single sample. */
  /* The legend goes to its own file, written and closed immediately so it is
   * safe on the card before a single sample is taken. The CSV itself gets
   * nothing but its column names - see the note in decode.cpp. */
  {
    File meta = SD.open(g_rec.metaName, FILE_WRITE);
    if (!meta) {
      LOG_LIVE(LVL_WARN, "cannot create %s - the CSV will have no legend",
               g_rec.metaName);
    } else {
      /* What each controller ENDED UP doing, not what config.h asked for -
       * with CANn_AUTODETECT they differ, and the sidecar is what a reader
       * trusts long after nobody remembers the build settings. */
      MetaBus mb[CAN_BUSES];
      for (uint8_t b = 0; b < CAN_BUSES; b++) {
        const BusHealth &h = g_rec.bus[b];
        mb[b].bitrateKbps = h.bitrateKbps;
        mb[b].crystalMHz  = h.crystalMHz;
        mb[b].listenOnly  = h.listenOnly;
        mb[b].present     = h.present;
        /* Assumed unless a search actually confirmed it. A detection that ran
         * and FAILED counts as assumed, which is the whole point of the
         * distinction. */
        mb[b].fromConfig  = !(h.autoDetect && h.autoFound);
      }
      const size_t mn = metaJson(s_buf, sizeof(s_buf),
                                 g_rec.csvName + 1, g_rec.logName + 1, g_dbc,
                                 mb);
      if (mn) meta.write((const uint8_t *)s_buf, mn);
      else    LOG_LIVE(LVL_WARN, "meta did not fit the buffer - frame map too "
                                 "large for %u bytes", (unsigned)sizeof(s_buf));
      meta.flush();
      meta.close();
    }
  }
  drainUnrecorded();

  const size_t hdr = csvColumnHeader(s_buf, sizeof(s_buf));
  if (hdr) {
    s_used = hdr;
    flushBuffer(true);
  } else {
    /* Only reachable with a very large frame map. The recording is still valid
     * - it just has to be read against the DBC rather than being self-
     * describing - so say so rather than failing the start. */
    LOG_LIVE(LVL_WARN, "the CSV column header did not fit: %s starts straight "
                       "into rows - read it against %s", g_rec.csvName,
             g_rec.metaName);
  }
  drainUnrecorded();
  memLog(LVL_INFO, true, "recording, header written");

  s_dec.reset(g_dbc);
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    busReset(g_bus[b]);
    liveReset(g_live[b]);
  }

  g_rec.fileIndex  = idx;
  g_rec.startMs    = millis();
  g_rec.rows       = 0;
  g_rec.bytes      = hdr;
  g_rec.writeCount = 0;
  g_rec.writeMaxUs = 0;
  g_rec.syncCount  = 0;
  g_rec.syncMaxUs  = 0;
  g_rec.powerFail  = false;
  g_rec.sdWriteFails = 0;
  g_rec.sdBytesLost  = 0;
  g_rec.sdError      = false;
#if SD_FAULT_TEST_AT_KB
  s_faultDone = false;
#endif

  /* Health counters describe THIS recording. Without this, a single frame lost
   * during boot - before any file existed - would mark every later recording
   * as lossy forever, which trains you to ignore the one number that matters.
   * Lifetime totals are preserved separately for the detailed log. */
  g_rec.lifeDropped += g_rec.queueDropped;
  g_rec.queueDropped = 0;
  g_rec.queuePeak    = 0;
  g_rec.drainMaxUs   = 0;
  s_lostSeen = 0;
  s_lostSaid = 0;
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    BusHealth &h = g_rec.bus[b];
    h.lifeOverflow   += h.canOvfFramesMin;
    h.canOvfEvents    = 0;
    h.canOvfFramesMin = 0;
    h.canIntfSticky   = 0;
  }
  g_rec.recording  = true;

  uint8_t mapped = 0;
  for (uint8_t b = 0; b < CAN_BUSES; b++) if (g_rec.bus[b].dbcLoaded) mapped++;
  LOG_LIVE(LVL_INFO, "RECORDING STARTED -> %s (+ %s), %u of %u buses decoding "
                     "via a frame map",
           g_rec.csvName, g_rec.logName, (unsigned)mapped, (unsigned)CAN_BUSES);
  LOG_FILE(LVL_INFO, "recorder: block=%u B, sync every %u ms, frame queue depth %u, "
                     "power-fail pin %d",
           (unsigned)SD_BLOCK_BYTES, (unsigned)SD_SYNC_INTERVAL_MS,
           (unsigned)FRAME_QUEUE_LEN, (int)PIN_POWER_FAIL);

  /* Commit the header immediately, so even a recording that is cut short a
   * moment from now leaves a valid, self-describing file behind. */
  syncToCard();
}

static void stopRecording() {
  if (!g_rec.recording) return;

  flushBuffer(true);

  const uint32_t secs = recorderElapsedMs() / 1000UL;
  LOG_LIVE(LVL_INFO, "RECORDING STOPPED: %s, %lu rows, %lu KB, %lu s",
           g_rec.csvName, (unsigned long)g_rec.rows,
           (unsigned long)(g_rec.bytes / 1024ULL), (unsigned long)secs);
  LOG_FILE(LVL_INFO, "summary: dropped=%lu queuePeak=%lu writes=%lu "
                     "maxWrite=%lu us drainMax=%lu us sdWriteFails=%lu "
                     "sdBytesLost=%lu",
           (unsigned long)g_rec.queueDropped, (unsigned long)g_rec.queuePeak,
           (unsigned long)g_rec.writeCount, (unsigned long)g_rec.writeMaxUs,
           (unsigned long)g_rec.drainMaxUs, (unsigned long)g_rec.sdWriteFails,
           (unsigned long)g_rec.sdBytesLost);
  if (g_rec.sdBytesLost) {
    LOG_LIVE(LVL_ERROR, "%s is INCOMPLETE: %lu KB of rows could not be written "
                        "to the card", g_rec.csvName,
             (unsigned long)((g_rec.sdBytesLost + 1023UL) / 1024UL));
  }
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    const BusHealth &h = g_rec.bus[b];
    LOG_FILE(LVL_INFO, "summary CAN%u: frames=%lu ovfEvents=%lu ovfFrames>=%lu "
                       "undecoded=%lu sticky=%lu",
             (unsigned)(b + 1), (unsigned long)h.framesRx,
             (unsigned long)h.canOvfEvents, (unsigned long)h.canOvfFramesMin,
             (unsigned long)g_bus[b].undecoded, (unsigned long)h.canIntfSticky);
  }

  logService();                 /* make sure the closing lines reach the file */
  logAttachFile(nullptr);

  if (s_csv) { s_csv.flush(); s_csv.close(); }
  if (s_log) { s_log.flush(); s_log.close(); }

  g_rec.recording = false;
}

/* The supply is collapsing and we are running on the hold-up capacitor. Do the
 * minimum that makes the file readable, in the order that matters: the RAM
 * buffer first, then the metadata, then close. Nothing here may block on
 * anything other than the card. */
static void emergencyStop() {
  s_powerFail = false;
  if (!g_rec.recording) return;

  LOG_LIVE(LVL_ERROR, "POWER FAIL - closing %s (%lu rows)",
           g_rec.csvName, (unsigned long)g_rec.rows);
  logService();               /* get that line into the .log while it is open */

  flushBuffer(true);
  if (s_csv) { s_csv.flush(); s_csv.close(); }
  if (s_log) { s_log.flush(); s_log.close(); }
  logAttachFile(nullptr);

  g_rec.recording = false;
  g_rec.powerFail = true;

  /* Deliberately no restart: if the supply recovers, the operator decides
   * whether to record again. Silently reopening would hide the event and
   * produce a second file whose first rows are missing. */
  LOG_LIVE(LVL_WARN, "recording closed safely - press START to record again");
}

/* ------------------------------------------------------------------------ */
/* Everything about ONE bus that has to be recomputed once a second, plus the
 * one-shot warnings that belong to it. Split out because the interesting
 * failure is asymmetric - one bus deaf while the other is fine - and a routine
 * that averaged the two would be incapable of saying so. */
static void busStatusTick(uint8_t b, uint32_t now, uint32_t dt) {
  BusHealth &h = g_rec.bus[b];
  static const uint8_t  kIntPin[CAN_BUSES] = { PIN_CAN1_INT, PIN_CAN2_INT };
  const unsigned busNo = (unsigned)(b + 1);

  h.frameRate = (uint32_t)(((uint64_t)(h.framesRx - s_framesAtLastStatus[b])
                            * 1000ULL) / (dt ? dt : 1));
  s_framesAtLastStatus[b] = h.framesRx;
  h.canOk = (now - h.lastFrameMs) < CAN_ALIVE_TIMEOUT_MS;

  busTick(g_bus[b], dt);

  /* Interrupt-path health, measured rather than assumed. */
  h.irqRate = (uint32_t)(((uint64_t)(h.irqCount - s_irqAtLastStatus[b])
                          * 1000ULL) / (dt ? dt : 1));
  s_irqAtLastStatus[b] = h.irqCount;

  /* Frames arriving but this line never firing means this bus is running
   * purely on the 20 ms fallback poll, which caps at ~100 frames/s. */
  h.intStuck = (h.frameRate > 0) && (h.irqRate == 0);

  /* Bus load: bits/s seen, as a percentage of THIS bus's configured bit rate.
   * Per bus because the two can be configured differently, and dividing both
   * by one number would misreport whichever one is not it. */
  const uint64_t bits = h.rxBits - s_bitsAtLastStatus[b];
  s_bitsAtLastStatus[b] = h.rxBits;
  const uint64_t rate  = h.bitrateKbps ? h.bitrateKbps : 1;
  h.busLoadPct = (uint32_t)((bits * 1000ULL * 100ULL) /
                            ((uint64_t)(dt ? dt : 1) * rate * 1000ULL));

  if (h.intStuck && !s_warnedIntStuck[b]) {
    s_warnedIntStuck[b] = true;
    LOG_LIVE(LVL_ERROR,
      "CAN%u INTERRUPT NOT FIRING - that bus is running on the 20 ms fallback "
      "poll, which caps at ~100 frames/s. Check its INT wire (MCP2515 INT -> "
      "D%d).", busNo, (int)kIntPin[b]);
  } else if (!h.intStuck && s_warnedIntStuck[b]) {
    s_warnedIntStuck[b] = false;
    LOG_LIVE(LVL_INFO, "CAN%u interrupt recovered - %lu irq/s", busNo,
             (unsigned long)h.irqRate);
  }
}

static void statusTick() {
  const uint32_t now = millis();
  const uint32_t dt  = now - s_lastStatusMs;
  if (dt < STATUS_PERIOD_MS) return;
  s_lastStatusMs = now;

  /* Once per recording, when it has settled: the heap the web server is left
   * with while the recording runs, which is lower than at "ready". */
  static uint16_t s_settledFor = 0;
  if (g_rec.recording && s_settledFor != g_rec.fileIndex &&
      now - g_rec.startMs >= 5000) {
    s_settledFor = g_rec.fileIndex;
    memLog(LVL_INFO, true, "recording, 5 s in");
  }

  for (uint8_t b = 0; b < CAN_BUSES; b++) busStatusTick(b, now, dt);

  g_rec.wakeRate = (uint32_t)(((uint64_t)(g_rec.wakeCount - s_wakeAtLastStatus)
                               * 1000ULL) / (dt ? dt : 1));
  s_wakeAtLastStatus = g_rec.wakeCount;

  g_rec.loopRate = (uint32_t)(((uint64_t)(g_rec.loopCount - s_loopAtLastStatus)
                               * 1000ULL) / (dt ? dt : 1));
  s_loopAtLastStatus = g_rec.loopCount;

  /* ---- where core 1 went, this second ----------------------------------
   *
   * busyUs / dt(ms) IS permille, exactly - no scaling constant, no rounding
   * step. 1000 is one whole core. The three tasks share core 1, so they can
   * sum to at most ~1000 between them; whatever is missing went to the idle
   * task, and that margin is the answer to "can this board carry a bigger
   * frame map". */
  {
    const uint32_t d = dt ? dt : 1;
    g_rec.canPermille    = g_rec.canBusyUs    / d;
    g_rec.writerPermille = g_rec.writerBusyUs / d;
    g_rec.loopPermille   = g_rec.loopBusyUs   / d;
    g_rec.sdPermille     = g_rec.sdBusyUs     / d;

    const uint64_t rowsNow  = g_rec.rows;
    const uint32_t rowsDelta = (uint32_t)(rowsNow - g_rec.rowsAtLastStatus);
    g_rec.rowsAtLastStatus  = rowsNow;
    g_rec.rowRate = (uint32_t)(((uint64_t)rowsDelta * 1000ULL) / d);

    g_rec.frameRateAll = 0;
    for (uint8_t b = 0; b < CAN_BUSES; b++) g_rec.frameRateAll += g_rec.bus[b].frameRate;

    /* The frame map's multiplier, in tenths: how many CSV rows one frame off
     * the wire turns into. This is the number the whole "how big a map can
     * this board carry" question actually turns on, and until now it was only
     * ever inferred from the file. */
    g_rec.rowsPerFrame10 = g_rec.frameRateAll
        ? (uint32_t)(((uint64_t)g_rec.rowRate * 10ULL) / g_rec.frameRateAll) : 0;

    /* Decode+format time is the writer's time MINUS the time it spent inside
     * the card. Guarded because the two are sampled independently and a pass
     * can straddle the boundary. */
    const uint32_t decodeUs = (g_rec.writerBusyUs > g_rec.sdBusyUs)
                            ? (g_rec.writerBusyUs - g_rec.sdBusyUs) : 0;
    g_rec.usPerRow10 = rowsDelta ? (uint32_t)(((uint64_t)decodeUs * 10ULL) / rowsDelta) : 0;

    const uint32_t flushDelta = g_rec.writeCount - g_rec.flushAtLastStatus;
    g_rec.flushAtLastStatus = g_rec.writeCount;
    g_rec.flushRate  = (uint32_t)(((uint64_t)flushDelta * 1000ULL) / d);
    g_rec.usPerFlush = flushDelta ? (g_rec.sdBusyUs / flushDelta) : 0;

    g_rec.canBusyUs = g_rec.writerBusyUs = g_rec.loopBusyUs = g_rec.sdBusyUs = 0;
  }

  const uint32_t qNow = g_frameQueue ? uxQueueMessagesWaiting(g_frameQueue) : 0;
  if (qNow > g_rec.queuePeak) g_rec.queuePeak = qNow;

  /* ---- the one line per second that goes to serial and the web terminal -- */
  char state[40];
  if (g_rec.recording) {
    const uint32_t s = recorderElapsedMs() / 1000UL;
    snprintf(state, sizeof(state), "REC %s %02lu:%02lu:%02lu",
             g_rec.csvName + 1, (unsigned long)(s / 3600UL),
             (unsigned long)((s / 60UL) % 60UL), (unsigned long)(s % 60UL));
  } else {
    snprintf(state, sizeof(state), "IDLE");
  }

  /* Both buses on one line, each with its own rx/irq, because the question
   * this line answers is "is the logger working" and with two buses that has
   * two answers. Kept to one line so a serial console stays readable. */
  char perBus[CAN_BUSES][48];
  bool anyOk   = false;
  bool anyStuck = false;
  uint32_t lost = g_rec.queueDropped;

  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    const BusHealth &h = g_rec.bus[b];
    anyOk    = anyOk || h.canOk;
    anyStuck = anyStuck || h.intStuck;
    lost    += h.canOvfFramesMin;

    if (!h.present) {
      snprintf(perBus[b], sizeof(perBus[b]), "CAN%u=-", (unsigned)(b + 1));
    } else {
      snprintf(perBus[b], sizeof(perBus[b]), "CAN%u rx=%lu/s irq=%lu/s %s%lu%%",
               (unsigned)(b + 1), (unsigned long)h.frameRate,
               (unsigned long)h.irqRate,
               h.intStuck ? (h.intLevel ? "DEAD " : "STUCK ")
                          : (h.canOk ? "" : "QUIET "),
               (unsigned long)h.busLoadPct);
    }
  }

  /* Heap on the same line as everything else, because a leak is only visible
   * as a trend and a single boot-time reading cannot show one. Free first,
   * then the largest block, then the worst that block has ever been - three
   * numbers because the total on its own has now twice failed to predict a
   * failure that the block size predicted exactly. */
  /* What is NOT being used. The three tasks share core 1, so the remainder is
   * the headroom a bigger frame map would have to fit into - and it is the
   * only figure here that answers "how much more can this carry". */
  const uint32_t busyAll = g_rec.canPermille + g_rec.writerPermille
                         + g_rec.loopPermille;
  const uint32_t idlePermille = (busyAll < 1000) ? (1000 - busyAll) : 0;

  char heap[72];
#if MEM_IN_STATUS_LINE
  {
    const MemStat mm = memStat();
    /* NOT the place for allocFail, though it was tried. LOG_LINE_CHARS is 160
     * and this line already runs to exactly that: measured on the wire it ends
     * mid-word at "blk 15", so `low` and `web` have been silently truncated
     * away for some time. Anything appended here is thrown away before it is
     * printed. allocFail gets its own line below; the truncation is recorded
     * separately, because widening the buffer moves the heap and the heap is
     * what these runs are measuring. */
    snprintf(heap, sizeof(heap), " | heap %luK blk %luK low %luK | web %lu/s",
             (unsigned long)(mm.freeNow  / 1024),
             (unsigned long)(mm.largest  / 1024),
             (unsigned long)(mm.lowBlock / 1024),
             (unsigned long)g_rec.loopRate);
  }
#else
  snprintf(heap, sizeof(heap), " | web %lu/s", (unsigned long)g_rec.loopRate);
#endif

  /* ---- frames lost, said out loud the first time it happens --------------
   *
   * `lost` is on the status line, but a number on a line nobody is reading is
   * not a report. This fires once when loss first appears, and
   * names the half it came from - because the two halves have opposite causes
   * and opposite fixes. It fires again if the OTHER half starts too. */
  if (lost > s_lostSeen) {
    const uint32_t byQueue = g_rec.queueDropped;
    uint32_t byCtrl = 0;
    for (uint8_t b = 0; b < CAN_BUSES; b++) byCtrl += g_rec.bus[b].canOvfFramesMin;

    const uint8_t which = (uint8_t)((byQueue ? 1 : 0) | (byCtrl ? 2 : 0));
    if (which & ~s_lostSaid) {
      s_lostSaid |= which;
      LOG_LIVE(LVL_ERROR,
        "FRAMES ARE BEING LOST: %lu so far (%lu dropped at the queue, %lu at "
        "least overrun in a controller). Queue drops mean the WRITER could not "
        "keep up - it is decoding too many rows a frame, or the card is "
        "blocking it, or something is preempting it. Controller overruns mean "
        "the CAN TASK did not get there in time - drain is %lu us worst. "
        "This configuration is not viable whatever the dashboard is doing.",
        (unsigned long)lost, (unsigned long)byQueue, (unsigned long)byCtrl,
        (unsigned long)g_rec.drainMaxUs);
    }
    s_lostSeen = lost;
  }

  /* Right after the file name: this line is cut at LOG_LINE_CHARS, and rows
   * that never reached the card must not be the part that is cut. */
  char sdLost[40] = "";
  if (g_rec.sdBytesLost) {
    snprintf(sdLost, sizeof(sdLost), " | SD LOST %lu KB",
             (unsigned long)((g_rec.sdBytesLost + 1023UL) / 1024UL));
  }

  if (anyOk) {
    LOG_LIVE(g_rec.sdBytesLost ? LVL_ERROR : (anyStuck ? LVL_WARN : LVL_INFO),
      "%s%s | %lu rows %lu KB | %s | %s | q=%lu/%u peak=%lu drain=%lu us "
      "write=%lu us | lost %lu%s",
      state, sdLost,
      (unsigned long)g_rec.rows, (unsigned long)(g_rec.bytes / 1024ULL),
      perBus[0], perBus[1],
      (unsigned long)qNow, (unsigned)FRAME_QUEUE_LEN,
      (unsigned long)g_rec.queuePeak, (unsigned long)g_rec.drainMaxUs,
      (unsigned long)g_rec.writeMaxUs,
      (unsigned long)lost, heap);
  } else {
    /* Counted rather than assumed: with one module fitted, "on either bus" is
     * wrong and sends somebody looking at hardware that is not there. */
    uint8_t live = 0;
    for (uint8_t b = 0; b < CAN_BUSES; b++) if (g_rec.bus[b].present) live++;

    if (live > 1) {
      /* The rates each bus is RUNNING at, not the ones in config.h: with
       * CANn_AUTODETECT those differ, and a warning that names a rate the
       * controller is not using sends somebody to change a setting that was
       * never in play. */
      LOG_LIVE(LVL_WARN, "%s | NO CAN TRAFFIC ON EITHER BUS - check the "
                         "wiring, the bit rates (%u / %u kbit/s) and the "
                         "crystal setting of each module", state,
               (unsigned)g_rec.bus[0].bitrateKbps,
               (unsigned)g_rec.bus[1].bitrateKbps);
    } else if (live == 1) {
      const uint8_t only = g_rec.bus[0].present ? 0 : 1;
      LOG_LIVE(LVL_WARN, "%s | NO CAN TRAFFIC on CAN%u (the only controller "
                         "that answered) - check its wiring, its bit rate "
                         "(%u kbit/s) and its crystal setting", state,
               (unsigned)(only + 1),
               (unsigned)g_rec.bus[only].bitrateKbps);
    } else {
      LOG_LIVE(LVL_WARN, "%s | NO CAN CONTROLLER FOUND AT ALL - check the "
                         "shared SPI wiring (SCK/MISO/MOSI) and 3V3%s",
               state, heap);
    }
  }

  /* ---- the profile: one line, and the model is closed -------------------
   *
   * Everything needed to compute the ceiling instead of bracketing it:
   *
   *   frames/s x rows/frame        = rows/s
   *   rows/s   x us/row            = decode CPU, permille
   *   flushes/s x us/flush         = card CPU, permille
   *   1000 - (can + writer + loop) = the margin that is left
   *
   * So the largest frame map this board can carry is the one whose rows/frame
   * keeps that margin positive - a number, from this bus, not a bracket. */
#if PROF_LIVE_LINE
  LOG_LIVE(LVL_INFO,
#else
  LOG_FILE(LVL_INFO,
#endif
    "prof: core1 can=%lu.%lu%% writer=%lu.%lu%% (sd %lu.%lu%%) loop=%lu.%lu%% "
    "idle=%lu.%lu%% | %lu frames/s x %lu.%lu rows = %lu rows/s @ %lu.%lu us/row "
    "| %lu flush/s @ %lu us | web %lu/s%s",
    (unsigned long)(g_rec.canPermille    / 10), (unsigned long)(g_rec.canPermille    % 10),
    (unsigned long)(g_rec.writerPermille / 10), (unsigned long)(g_rec.writerPermille % 10),
    (unsigned long)(g_rec.sdPermille     / 10), (unsigned long)(g_rec.sdPermille     % 10),
    (unsigned long)(g_rec.loopPermille   / 10), (unsigned long)(g_rec.loopPermille   % 10),
    (unsigned long)(idlePermille / 10),         (unsigned long)(idlePermille % 10),
    (unsigned long)g_rec.frameRateAll,
    (unsigned long)(g_rec.rowsPerFrame10 / 10), (unsigned long)(g_rec.rowsPerFrame10 % 10),
    (unsigned long)g_rec.rowRate,
    (unsigned long)(g_rec.usPerRow10 / 10), (unsigned long)(g_rec.usPerRow10 % 10),
    (unsigned long)g_rec.flushRate, (unsigned long)g_rec.usPerFlush,
    (unsigned long)g_rec.loopRate, "");

  /* ---- the detail that only the .log file gets -------------------------- */
  g_rec.stackFreeWriter = uxTaskGetStackHighWaterMark(nullptr);
  LOG_FILE(LVL_DEBUG, "stack free: can=%lu of %u writer=%lu of %u loop=%lu B",
           (unsigned long)g_rec.stackFreeCan, (unsigned)TASK_STACK_CAN,
           (unsigned long)g_rec.stackFreeWriter, (unsigned)TASK_STACK_WRITER,
           (unsigned long)g_rec.stackFreeLoop);
  /* Sized to the line the logger actually writes. The old 240-byte buffer only
   * ever bought truncation somewhere less visible: LOG_LINE_CHARS is the real
   * limit and everything past it is dropped by vsnprintf in logger.cpp. */
  char per[LOG_LINE_CHARS];
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    if (!g_rec.bus[b].present) continue;
    const uint8_t shown = busFormatIds(g_bus[b], &s_idCursor[b], per, sizeof(per));

    /* Totals FIRST. Appended after the list they were the first thing the
     * 160-character line dropped, and they were dropped on every bus with more
     * than about nine ids - which is every real one. */
    LOG_FILE(LVL_DEBUG, "ids CAN%u: %u of %u | untracked=%lu undecoded=%lu | %s",
             (unsigned)(b + 1), (unsigned)shown, (unsigned)g_bus[b].used,
             (unsigned long)g_bus[b].untracked,
             (unsigned long)g_bus[b].undecoded, per);
  }

  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    const BusHealth &h = g_rec.bus[b];
    if (!h.present) continue;
    LOG_FILE(LVL_DEBUG,
      "health CAN%u: frames=%lu rx=%lu/s ovfEvents=%lu ovfFrames>=%lu "
      "sticky=%lu INT=%d load=%lu%%",
      (unsigned)(b + 1), (unsigned long)h.framesRx, (unsigned long)h.frameRate,
      (unsigned long)h.canOvfEvents, (unsigned long)h.canOvfFramesMin,
      (unsigned long)h.canIntfSticky, (int)h.intLevel,
      (unsigned long)h.busLoadPct);
  }

  /* drain is the dual-bus number: worst microseconds spent emptying BOTH
   * controllers in one pass. A controller holds two frames, so at 500 kbit/s
   * anything approaching 200 means the margin is gone.
   *
   * block and lowBlock are here because heap and minHeap on their own were
   * actively misleading. The largest free BLOCK is what decides whether an
   * allocation succeeds - that is the whole argument of mem.h - and yet it was
   * only ever written to the log when it crossed a warning threshold. So every
   * recording carried a 1 Hz series of the number that does not matter and two
   * scattered samples of the number that does, which made the web UI's decay
   * impossible to characterise after the fact. Two more heap_caps calls a
   * second closes that. */
  LOG_FILE(LVL_DEBUG,
    "health: queue=%lu peak=%lu drop=%lu drain=%lu us wake=%lu/s "
    "writes=%lu maxWr=%lu us "
    "syncs=%lu maxSync=%lu us atRisk<=%lu ms logDrop=%lu heap=%lu minHeap=%lu "
    "block=%lu lowBlock=%lu allocFail=%lu lastFail=%lu maxFail=%lu",
    (unsigned long)qNow, (unsigned long)g_rec.queuePeak,
    (unsigned long)g_rec.queueDropped, (unsigned long)g_rec.drainMaxUs,
    (unsigned long)g_rec.wakeRate,
    (unsigned long)g_rec.writeCount, (unsigned long)g_rec.writeMaxUs,
    (unsigned long)g_rec.syncCount, (unsigned long)g_rec.syncMaxUs,
    (unsigned long)(millis() - s_lastSyncMs),
    (unsigned long)logDroppedCount(),
    (unsigned long)memStat().freeNow, (unsigned long)memStat().minFree,
    (unsigned long)memStat().largest, (unsigned long)memStat().lowBlock,
    (unsigned long)memAllocFailures(), (unsigned long)memAllocFailLast(),
    (unsigned long)memAllocFailMax());
  /* Its own line, short enough to survive LOG_LINE_CHARS, and LIVE so it
   * reaches the serial capture - the experiments that need it run from a card
   * whose .log cannot be read without reflashing the board to get at it.
   *
   * Silent until the first failure, then once a second. A counter that prints
   * "0" every second for forty minutes trains everyone to stop reading it. */
  {
    const uint32_t nf = memAllocFailures();
    if (nf) {
      LOG_LIVE(LVL_WARN, "allocFail %lu, last %lu B caps=0x%lx from %s - an "
                         "allocation was REFUSED; the SYN for an inbound "
                         "connection is dropped and the port looks dead",
               (unsigned long)nf, (unsigned long)memAllocFailLast(),
               (unsigned long)memAllocFailCaps(), memAllocFailFn());
    }
  }
  LOG_FILE(LVL_DEBUG, "net: %s", netStatusLine());
}

/* ------------------------------------------------------------------------ */
/* ==========================================================================
 *  The dashboard layout on the card
 *
 *  Read at boot, written back whenever the browser saves. See dashstore.h for
 *  which copy wins and why.
 * ======================================================================== */

/* Serialises g_dash and writes it to DASH_PATH. Runs in the writer task, the
 * only task that touches the card. */
static void writeDashFile() {
  if (!g_rec.sdOk) return;

  char *buf = (char *)malloc(DASH_CFG_MAX);
  if (!buf) {
    LOG_FILE(LVL_WARN, "not enough memory to write %s", DASH_PATH);
    return;
  }
  const size_t n = dashSerialize(g_dash, buf, DASH_CFG_MAX);
  if (n == 0 || n >= DASH_CFG_MAX) {
    free(buf);
    LOG_FILE(LVL_WARN, "the dashboard layout did not fit %s", DASH_PATH);
    return;
  }

  /* Write to a temporary name and rename over the top. A power cut halfway
   * through a direct write leaves a half-parsed layout on the card that would
   * then be treated as an edit and imported over the good copy in flash. */
  sdRemoveIfThere(DASH_TMP_PATH);
  File f = SD.open(DASH_TMP_PATH, FILE_WRITE);
  if (!f) {
    free(buf);
    LOG_FILE(LVL_WARN, "could not open %s for writing", DASH_TMP_PATH);
    return;
  }
  const size_t wrote = f.write((const uint8_t *)buf, n);
  f.flush();
  f.close();

  if (wrote != n) {
    free(buf);
    sdRemoveIfThere(DASH_TMP_PATH);
    LOG_FILE(LVL_WARN, "short write to %s - the card may be full", DASH_TMP_PATH);
    return;
  }

  sdRemoveIfThere(DASH_PATH);
  if (!SD.rename(DASH_TMP_PATH, DASH_PATH)) {
    free(buf);
    LOG_FILE(LVL_WARN, "could not put %s in place", DASH_PATH);
    return;
  }

  /* Agree with what was just written, so the next boot does not read it back
   * as somebody else's edit. */
  dashStoreNoteCardHash(dashHash(buf, n));
  free(buf);

  LOG_FILE(LVL_INFO, "dashboard layout mirrored to %s (%u bytes)",
           DASH_PATH, (unsigned)n);
}

void recorderLoadDash() {
  /* No card: flash is all there is, which is a perfectly good configuration. */
  if (!g_rec.sdOk) {
    dashResolve(g_dash, g_dbc);
    return;
  }

  if (!SD.exists(DASH_PATH)) {
    /* A card with no layout on it. If flash has one, put it there - that is
     * how the file comes into existence, and how a layout gets copied from one
     * logger to another. */
    if (dashStoreHadConfig()) {
      LOG_FILE(LVL_INFO, "no %s on the card - writing the stored layout to it",
               DASH_PATH);
      writeDashFile();
    } else {
      LOG_FILE(LVL_INFO, "no %s and no stored layout - the dashboard starts "
                         "empty; build one in the browser", DASH_PATH);
    }
    dashResolve(g_dash, g_dbc);
    return;
  }

  File f = SD.open(DASH_PATH, FILE_READ);
  if (!f) {
    LOG_LIVE(LVL_WARN, "could not open %s - using the layout stored in flash",
             DASH_PATH);
    dashResolve(g_dash, g_dbc);
    return;
  }

  char  *buf = (char *)malloc(DASH_CFG_MAX);
  size_t n   = 0;
  if (!buf) {
    f.close();
    LOG_LIVE(LVL_WARN, "not enough memory to read %s", DASH_PATH);
    dashResolve(g_dash, g_dbc);
    return;
  }
  while (f.available() && n < DASH_CFG_MAX - 1) {
    const int c = f.read();
    if (c < 0) break;
    buf[n++] = (char)c;
  }
  const bool truncated = f.available();
  f.close();
  buf[n] = '\0';

  if (truncated) {
    LOG_LIVE(LVL_WARN, "%s is larger than %u bytes - only the first part was "
                       "read", DASH_PATH, (unsigned)DASH_CFG_MAX);
  }

  const uint32_t cardHash = dashHash(buf, n);
  if (dashStoreHadConfig() && cardHash == dashStoreCardHash()) {
    /* The file is exactly what this logger last agreed with, so nothing has
     * been edited on the card and flash is the newer copy. This is the branch
     * that stops a boot from throwing away everything saved in the browser. */
    free(buf);
    LOG_FILE(LVL_INFO, "%s is unchanged - keeping the layout stored in flash",
             DASH_PATH);
    dashResolve(g_dash, g_dbc);
    return;
  }

  /* Either the file was edited, or this logger has never seen one. Either way
   * the card is the newer copy. */
  dashReset(g_dash);
  const uint16_t errors = dashParse(g_dash, buf, n);
  free(buf);

  dashStoreNoteCardHash(cardHash);
  dashStoreSave();

  LOG_LIVE(LVL_INFO, "dashboard layout loaded from %s: %ux%u grid", DASH_PATH,
           (unsigned)g_dash.cols, (unsigned)g_dash.rows);
  if (errors) {
    LOG_LIVE(LVL_WARN, "%u line(s) of %s could not be parsed - see the .log",
             (unsigned)errors, DASH_PATH);
  }

  const uint16_t missing = dashResolve(g_dash, g_dbc);
  if (missing) {
    LOG_LIVE(LVL_WARN, "%u dashboard cell(s) name a signal this frame map does "
                       "not have - they show as unknown until the DBC or the "
                       "layout is corrected", (unsigned)missing);
  }
}

void recorderTask(void *arg) {
  (void)arg;
  CanFrame f;

  s_lastStatusMs = millis();

  for (;;) {
    /* Checked first and on every pass: on a supply loss the only thing that
     * matters is getting the file closed before the capacitor runs out. */
    if (s_powerFail) emergencyStop();

    if (s_wantStart) { s_wantStart = false; startRecording(); }
    if (s_wantStop)  { s_wantStop  = false; stopRecording();  }

    /* Mirroring the layout to the card is the lowest-priority thing this task
     * does. It happens here, in the only task that owns the card, rather than
     * in the HTTP handler that asked for it. */
    if (s_wantDashSave) { s_wantDashSave = false; writeDashFile(); }

    /* Block until work arrives, then take everything that is already queued in
     * one go - one wake-up per burst instead of one per frame. */
    /* The clock starts AFTER the blocking receive returns, so waiting for work
     * is not counted as doing it. A pass that times out with nothing queued
     * contributes almost nothing, which is what makes writerPermille mean
     * "share of core 1 this task consumed". */
    const bool got = (xQueueReceive(g_frameQueue, &f, pdMS_TO_TICKS(20)) == pdTRUE);
    const uint32_t busyStart = micros();

    if (got) {
      do {
        /* Always counted, so the dashboard shows the bus even while idle -
         * except for frames this logger sent, which were never on the wire as
         * far as this controller is concerned. Counting them would inflate the
         * frame rate and the bus load with our own traffic, and would let a
         * cyclic setpoint make an idle bus look alive. */
        const uint8_t fb = (f.bus < CAN_BUSES) ? f.bus : 0;
        if (!f.tx) busObserve(g_bus[fb], f, &g_dbc[fb]);
        if (!g_rec.recording) continue;

        /* Decode straight into the staging buffer. The buffer is oversized by
         * one frame's worth of rows, so this never has to flush mid-frame. */
        if (sizeof(s_buf) - s_used < DECODE_FRAME_MAX) flushBuffer(true);

        uint8_t nRows = 0;
        s_used += s_dec.rows(f, s_buf + s_used, sizeof(s_buf) - s_used, &nRows);
        g_rec.rows += nRows;
        flushBuffer(false);            /* writes only once the block is full */
      } while (xQueueReceive(g_frameQueue, &f, 0) == pdTRUE);
    }

    /* Bound the data at risk from a power cut, and make sure a low-rate bus
     * still gets its rows onto the card. */
    if (g_rec.recording && (millis() - s_lastSyncMs) >= SD_SYNC_INTERVAL_MS) {
      syncToCard();
      LOG_FILE(LVL_DEBUG, "sync: %lu rows, %lu KB committed, took %lu us",
               (unsigned long)g_rec.rows, (unsigned long)(g_rec.bytes / 1024ULL),
               (unsigned long)g_rec.syncMaxUs);
    }

    statusTick();
    logService();

    g_rec.writerBusyUs += micros() - busyStart;
  }
}
