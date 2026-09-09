#include "recorder.h"
#include "config.h"
#include "logger.h"
#include "decode.h"
#include "dbc.h"
#include "netcfg.h"
#include "dash.h"
#include "dashstore.h"
#include "cantx.h"

#include <SPI.h>
#include <SD.h>

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

/* ------------------------------------------------------------------------ */
bool recorderBeginSD() {
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

  bool mounted = false;
  for (uint8_t i = 0; i < sizeof(SPEEDS) / sizeof(SPEEDS[0]); i++) {
    if (SD.begin(PIN_SD_CS, s_sdSpi, SPEEDS[i])) {
      if (i) {
        LOG_LIVE(LVL_WARN, "SD card needed a slower clock: %lu kHz instead of "
                           "%lu kHz - check the wiring if writes cannot keep up",
                 (unsigned long)(SPEEDS[i] / 1000UL),
                 (unsigned long)(SD_SPI_HZ  / 1000UL));
      }
      mounted = true;
      break;
    }
    SD.end();
    delay(50);          /* let the card settle before re-clocking it */
  }

  if (!mounted) {
    LOG_LIVE(LVL_ERROR, "NO SD CARD at any clock down to %lu kHz - check that "
                        "the module is powered (many need 5V/VIN, not 3V3), "
                        "that the card is FAT32, and CS=D%d",
             (unsigned long)(SPEEDS[sizeof(SPEEDS) / sizeof(SPEEDS[0]) - 1] / 1000UL),
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
static bool readLine(File &f, char *buf, size_t cap) {
  size_t n = 0;
  bool   any = false;
  while (f.available()) {
    const int c = f.read();
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

  static char line[DBC_LINE_MAX];

  /* FIRST PASS: count. The tables are then sized to this file rather than to a
   * number picked at compile time, which is what stops a 707-signal bus being
   * decoded 256 signals deep and logged raw for the rest. Reading the file
   * twice costs a fraction of a second off an SD card and happens once at
   * boot. */
  DbcCounts want = {0, 0, 0};
  uint32_t   lines = 0;
  while (readLine(f, line, sizeof(line))) {
    dbcCountLine(line, want);
    if (++lines > 20000) break;            /* a runaway file is not a DBC */
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
    const size_t perMsg = sizeof(DbcMessage);
    const size_t perSig = sizeof(DbcSignal) + LIVE_TEXT_MAX
                        + sizeof(uint32_t) + 1;      /* the live slots too */
    const size_t perVal = sizeof(DbcValDesc);

    const size_t need = (size_t)want.messages * perMsg
                      + (size_t)want.signals  * perSig
                      + (size_t)want.values   * perVal;
    const size_t heap = (size_t)ESP.getFreeHeap();
    const size_t room = (heap > DBC_HEAP_RESERVE) ? heap - DBC_HEAP_RESERVE : 0;

    if (need > room) {
      LOG_LIVE(LVL_WARN, "frame map wants %lu KB, %lu KB free - keeping %lu KB "
                         "and leaving %lu KB for Wi-Fi. If the logger runs with "
                         "plenty spare, lower DBC_HEAP_RESERVE in config.h.",
               (unsigned long)((need + 1023) / 1024),
               (unsigned long)(heap / 1024),
               (unsigned long)((room + 1023) / 1024),
               (unsigned long)(DBC_HEAP_RESERVE / 1024));
      const double k = need ? (double)room / (double)need : 0.0;
      want.messages = (uint16_t)((double)want.messages * k);
      want.signals  = (uint16_t)((double)want.signals  * k);
      want.values   = (uint16_t)((double)want.values   * k);
      db.overflow = 1;
    }
  }

  const bool sized = dbcAllocate(db, want);
  liveAllocate(g_live[bus], db.sigCap);

  /* SECOND PASS: parse. */
  f.seek(0);
  lines = 0;
  while (readLine(f, line, sizeof(line))) {
    dbcParseLine(db, line);
    if (++lines > 20000) break;
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

  LOG_LIVE(LVL_INFO, "CAN%u frame map: %u messages, %u signals from %s (%lu KB, "
                     "%lu KB free)",
           busNo, (unsigned)db.msgCount, (unsigned)db.sigCount, path,
           (unsigned long)((dbcBytes(db) + 1023) / 1024),
           (unsigned long)(ESP.getFreeHeap() / 1024));
  LOG_FILE(LVL_INFO, "CAN%u dbc: version='%s' values=%u lineErrors=%u inexact=%u "
                     "caps=%u/%u/%u bytes=%lu",
           busNo, db.version, (unsigned)db.valCount,
           (unsigned)db.lineErrors, (unsigned)db.inexact,
           (unsigned)db.msgCap, (unsigned)db.sigCap,
           (unsigned)db.valCap, (unsigned long)dbcBytes(db));

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
             (unsigned long)(ESP.getFreeHeap() / 1024));
  }
  if (db.lineErrors) {
    LOG_LIVE(LVL_WARN, "%u line(s) of %s could not be parsed - see the .log",
             (unsigned)db.lineErrors, path);
  }
  if (db.nameClipped) {
    /* Said out loud, because the cost is invisible until somebody matches CSV
     * rows against the source DBC by name and quietly gets none. */
    LOG_LIVE(LVL_WARN, "%u name(s) are longer than %u characters and are cut "
                       "short in the CSV - raise DBC_NAME_MAX in dbc.h",
             (unsigned)db.nameClipped, (unsigned)(DBC_NAME_MAX - 1));
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

/* Lowest free index: 1.csv, 2.csv, ... A slot counts as taken if either the
 * .csv or the .log exists, so the pair always shares a number. */
static uint16_t nextFileIndex() {
  char a[20], b[20], c[20];
  for (uint16_t i = 1; i < 10000; i++) {
    snprintf(a, sizeof(a), "/%u.csv",  i);
    snprintf(b, sizeof(b), "/%u.log",  i);
    snprintf(c, sizeof(c), "/%u.meta", i);
    if (!SD.exists(a) && !SD.exists(b) && !SD.exists(c)) return i;
  }
  return 0;
}

/* ------------------------------------------------------------------------ */
static void flushBuffer(bool force) {
  if (!s_used) return;
  if (!force && s_used < SD_BLOCK_BYTES) return;
  if (!s_csv) { s_used = 0; return; }

  const uint32_t t0 = micros();
  const size_t   n  = s_csv.write((const uint8_t *)s_buf, s_used);
  const uint32_t dt = micros() - t0;

  if (n != s_used) {
    g_rec.sdError = true;
    LOG_LIVE(LVL_ERROR, "SD write failed: %u of %u bytes - card full or removed?",
             (unsigned)n, (unsigned)s_used);
  }
  g_rec.bytes += n;
  g_rec.writeCount++;
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

  s_csv = SD.open(g_rec.csvName, FILE_WRITE);
  if (!s_csv) { LOG_LIVE(LVL_ERROR, "cannot create %s", g_rec.csvName); return; }

  s_log = SD.open(g_rec.logName, FILE_WRITE);
  if (!s_log) {
    LOG_LIVE(LVL_WARN, "cannot create %s - continuing without the detailed log",
             g_rec.logName);
  } else {
    logAttachFile(&s_log);
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

  /* Health counters describe THIS recording. Without this, a single frame lost
   * during boot - before any file existed - would mark every later recording
   * as lossy forever, which trains you to ignore the one number that matters.
   * Lifetime totals are preserved separately for the detailed log. */
  g_rec.lifeDropped += g_rec.queueDropped;
  g_rec.queueDropped = 0;
  g_rec.queuePeak    = 0;
  g_rec.drainMaxUs   = 0;
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
                     "maxWrite=%lu us drainMax=%lu us",
           (unsigned long)g_rec.queueDropped, (unsigned long)g_rec.queuePeak,
           (unsigned long)g_rec.writeCount, (unsigned long)g_rec.writeMaxUs,
           (unsigned long)g_rec.drainMaxUs);
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

  for (uint8_t b = 0; b < CAN_BUSES; b++) busStatusTick(b, now, dt);

  g_rec.wakeRate = (uint32_t)(((uint64_t)(g_rec.wakeCount - s_wakeAtLastStatus)
                               * 1000ULL) / (dt ? dt : 1));
  s_wakeAtLastStatus = g_rec.wakeCount;

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

  if (anyOk) {
    LOG_LIVE(anyStuck ? LVL_WARN : LVL_INFO,
      "%s | %lu rows %lu KB | %s | %s | q=%lu/%u peak=%lu drain=%lu us | lost %lu",
      state,
      (unsigned long)g_rec.rows, (unsigned long)(g_rec.bytes / 1024ULL),
      perBus[0], perBus[1],
      (unsigned long)qNow, (unsigned)FRAME_QUEUE_LEN,
      (unsigned long)g_rec.queuePeak, (unsigned long)g_rec.drainMaxUs,
      (unsigned long)lost);
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
                         "shared SPI wiring (SCK/MISO/MOSI) and 3V3", state);
    }
  }

  /* ---- the detail that only the .log file gets -------------------------- */
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
   * anything approaching 200 means the margin is gone. */
  LOG_FILE(LVL_DEBUG,
    "health: queue=%lu peak=%lu drop=%lu drain=%lu us wake=%lu/s "
    "writes=%lu maxWr=%lu us "
    "syncs=%lu maxSync=%lu us atRisk<=%lu ms logDrop=%lu heap=%lu minHeap=%lu",
    (unsigned long)qNow, (unsigned long)g_rec.queuePeak,
    (unsigned long)g_rec.queueDropped, (unsigned long)g_rec.drainMaxUs,
    (unsigned long)g_rec.wakeRate,
    (unsigned long)g_rec.writeCount, (unsigned long)g_rec.writeMaxUs,
    (unsigned long)g_rec.syncCount, (unsigned long)g_rec.syncMaxUs,
    (unsigned long)(millis() - s_lastSyncMs),
    (unsigned long)logDroppedCount(),
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
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
  SD.remove(DASH_TMP_PATH);
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
    SD.remove(DASH_TMP_PATH);
    LOG_FILE(LVL_WARN, "short write to %s - the card may be full", DASH_TMP_PATH);
    return;
  }

  SD.remove(DASH_PATH);
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
    if (xQueueReceive(g_frameQueue, &f, pdMS_TO_TICKS(20)) == pdTRUE) {
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
  }
}
