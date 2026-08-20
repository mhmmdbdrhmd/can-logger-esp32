#include "recorder.h"
#include "config.h"
#include "logger.h"
#include "decode.h"
#include "dbc.h"
#include "netcfg.h"

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
static volatile bool s_wantStop  = false;
static volatile bool s_powerFail = false;

static uint32_t s_lastSyncMs   = 0;
static uint32_t s_lastStatusMs = 0;
static uint32_t s_framesAtLastStatus = 0;
static uint32_t s_irqAtLastStatus    = 0;
static uint32_t s_wakeAtLastStatus   = 0;
static bool     s_warnedIntStuck     = false;
static uint64_t s_bitsAtLastStatus   = 0;

void recorderRequestStart() { s_wantStart = true; s_wantStop = false; }
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

void recorderLoadDbc() {
  dbcReset(g_dbc);
  g_rec.dbcLoaded   = false;
  g_rec.dbcMessages = 0;
  g_rec.dbcSignals  = 0;

  if (!g_rec.sdOk) {
    LOG_LIVE(LVL_WARN, "no SD card - recording raw payload bytes, nothing decoded");
    return;
  }
  if (!SD.exists(DBC_PATH)) {
    LOG_LIVE(LVL_INFO, "no %s on the card - recording raw payload bytes. "
                       "Add a DBC to decode signals in real time.", DBC_PATH);
    return;
  }

  File f = SD.open(DBC_PATH, FILE_READ);
  if (!f) {
    LOG_LIVE(LVL_WARN, "could not open %s - recording raw payload bytes", DBC_PATH);
    return;
  }

  static char line[DBC_LINE_MAX];
  uint32_t lines = 0;
  while (readLine(f, line, sizeof(line))) {
    dbcParseLine(g_dbc, line);
    lines++;
    if (lines > 20000) break;              /* a runaway file is not a DBC */
  }
  f.close();

  g_rec.dbcLoaded   = g_dbc.loaded != 0;
  g_rec.dbcMessages = g_dbc.msgCount;
  g_rec.dbcSignals  = g_dbc.sigCount;

  if (!g_dbc.loaded) {
    LOG_LIVE(LVL_WARN, "%s has no BO_ messages - recording raw payload bytes",
             DBC_PATH);
    return;
  }

  LOG_LIVE(LVL_INFO, "frame map: %u messages, %u signals from %s",
           (unsigned)g_dbc.msgCount, (unsigned)g_dbc.sigCount, DBC_PATH);
  LOG_FILE(LVL_INFO, "dbc: version='%s' values=%u lineErrors=%u inexact=%u",
           g_dbc.version, (unsigned)g_dbc.valCount,
           (unsigned)g_dbc.lineErrors, (unsigned)g_dbc.inexact);

  if (g_dbc.overflow) {
    LOG_LIVE(LVL_WARN, "the frame map did not fit - raise DBC_MAX_MESSAGES / "
                       "DBC_MAX_SIGNALS in config.h. Frames beyond it are still "
                       "recorded, as raw bytes.");
  }
  if (g_dbc.lineErrors) {
    LOG_LIVE(LVL_WARN, "%u line(s) of %s could not be parsed - see the .log",
             (unsigned)g_dbc.lineErrors, DBC_PATH);
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
      const size_t mn = metaJson(s_buf, sizeof(s_buf),
                                 g_rec.csvName + 1, g_rec.logName + 1, g_dbc);
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
    LOG_LIVE(LVL_WARN, "the CSV header did not fit: %s has no legend block, "
                       "keep %s alongside it", g_rec.csvName, DBC_PATH);
  }

  s_dec.reset(&g_dbc);
  busReset(g_bus);
  liveReset(g_live);

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
  g_rec.lifeDropped  += g_rec.queueDropped;
  g_rec.lifeOverflow += g_rec.canOverflow;
  g_rec.queueDropped  = 0;
  g_rec.canOverflow   = 0;
  g_rec.queuePeak     = 0;
  g_rec.canIntfSticky = 0;
  g_rec.recording  = true;

  LOG_LIVE(LVL_INFO, "RECORDING STARTED -> %s (+ %s), %s",
           g_rec.csvName, g_rec.logName,
           g_rec.dbcLoaded ? "decoding via the frame map" : "raw bytes only");
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
  LOG_FILE(LVL_INFO, "summary: dropped=%lu canOverflow=%lu queuePeak=%lu "
                     "writes=%lu maxWrite=%lu us undecoded=%lu",
           (unsigned long)g_rec.queueDropped, (unsigned long)g_rec.canOverflow,
           (unsigned long)g_rec.queuePeak, (unsigned long)g_rec.writeCount,
           (unsigned long)g_rec.writeMaxUs, (unsigned long)g_bus.undecoded);

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
static void statusTick() {
  const uint32_t now = millis();
  const uint32_t dt  = now - s_lastStatusMs;
  if (dt < STATUS_PERIOD_MS) return;
  s_lastStatusMs = now;

  g_rec.frameRate = (uint32_t)(((uint64_t)(g_rec.framesRx - s_framesAtLastStatus)
                                * 1000ULL) / (dt ? dt : 1));
  s_framesAtLastStatus = g_rec.framesRx;
  g_rec.canOk = (now - g_rec.lastFrameMs) < CAN_ALIVE_TIMEOUT_MS;

  busTick(g_bus, dt);

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

  /* Interrupt-path health, measured rather than assumed. */
  g_rec.irqRate  = (uint32_t)(((uint64_t)(g_rec.irqCount  - s_irqAtLastStatus)
                               * 1000ULL) / (dt ? dt : 1));
  g_rec.wakeRate = (uint32_t)(((uint64_t)(g_rec.wakeCount - s_wakeAtLastStatus)
                               * 1000ULL) / (dt ? dt : 1));
  s_irqAtLastStatus  = g_rec.irqCount;
  s_wakeAtLastStatus = g_rec.wakeCount;

  /* Frames arriving but the line never firing means we are running purely on
   * the 20 ms fallback poll, which caps at ~100 frames/s. */
  g_rec.intStuck = (g_rec.frameRate > 0) && (g_rec.irqRate == 0);

  /* Bus load: bits/s seen, as a percentage of the configured bit rate. */
  const uint64_t bits = g_rec.rxBits - s_bitsAtLastStatus;
  s_bitsAtLastStatus  = g_rec.rxBits;
  g_rec.busLoadPct = (uint32_t)((bits * 1000ULL * 100ULL) /
                                ((uint64_t)(dt ? dt : 1) *
                                 (uint64_t)CAN_BITRATE_KBPS * 1000ULL));

  if (g_rec.intStuck && !s_warnedIntStuck) {
    s_warnedIntStuck = true;
    LOG_LIVE(LVL_ERROR,
      "CAN INTERRUPT NOT FIRING - running on the 20 ms fallback poll, which "
      "caps at ~100 frames/s. Check the INT wire (MCP2515 INT -> D%d).",
      PIN_CAN_INT);
  } else if (!g_rec.intStuck && s_warnedIntStuck) {
    s_warnedIntStuck = false;
    LOG_LIVE(LVL_INFO, "CAN interrupt recovered - %lu irq/s",
             (unsigned long)g_rec.irqRate);
  }

  if (g_rec.canOk) {
    LOG_LIVE(g_rec.intStuck ? LVL_WARN : LVL_INFO,
      "%s | %lu rows %lu KB | rx=%lu/s irq=%lu/s INT=%s(%d) | %u ids%s | "
      "q=%lu/%u peak=%lu sticky=%lu | lost %lu",
      state,
      (unsigned long)g_rec.rows, (unsigned long)(g_rec.bytes / 1024ULL),
      (unsigned long)g_rec.frameRate, (unsigned long)g_rec.irqRate,
      g_rec.intStuck ? (g_rec.intLevel ? "DEAD" : "STUCK-LOW")
                     : (g_rec.irqRate ? "ok" : "idle"),
      (int)g_rec.intLevel,
      (unsigned)g_bus.used, g_rec.dbcLoaded ? "" : " (raw)",
      (unsigned long)qNow, (unsigned)FRAME_QUEUE_LEN,
      (unsigned long)g_rec.queuePeak, (unsigned long)g_rec.canIntfSticky,
      (unsigned long)(g_rec.queueDropped + g_rec.canOverflow));
  } else {
    LOG_LIVE(LVL_WARN, "%s | NO CAN TRAFFIC - check the wiring, the bit rate "
                       "(%d kbit/s) and CAN_CRYSTAL_MHZ", state, CAN_BITRATE_KBPS);
  }

  /* ---- the detail that only the .log file gets -------------------------- */
  char per[240];
  int  k = 0;
  for (uint8_t i = 0; i < g_bus.used && k < (int)sizeof(per) - 24; i++) {
    k += snprintf(per + k, sizeof(per) - (size_t)k, "0x%lX=%lu(%lu/s)%s ",
                  (unsigned long)g_bus.id[i], (unsigned long)g_bus.count[i],
                  (unsigned long)g_bus.rate[i], g_bus.known[i] ? "" : "?");
  }
  LOG_FILE(LVL_DEBUG, "ids: %s untracked=%lu undecoded=%lu", per,
           (unsigned long)g_bus.untracked, (unsigned long)g_bus.undecoded);
  LOG_FILE(LVL_DEBUG,
    "health: queue=%lu peak=%lu drop=%lu canOvf=%lu writes=%lu maxWr=%lu us "
    "syncs=%lu maxSync=%lu us atRisk<=%lu ms logDrop=%lu heap=%lu minHeap=%lu",
    (unsigned long)qNow, (unsigned long)g_rec.queuePeak,
    (unsigned long)g_rec.queueDropped, (unsigned long)g_rec.canOverflow,
    (unsigned long)g_rec.writeCount, (unsigned long)g_rec.writeMaxUs,
    (unsigned long)g_rec.syncCount, (unsigned long)g_rec.syncMaxUs,
    (unsigned long)(millis() - s_lastSyncMs),
    (unsigned long)logDroppedCount(),
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  LOG_FILE(LVL_DEBUG, "net: %s", netStatusLine());
}

/* ------------------------------------------------------------------------ */
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

    /* Block until work arrives, then take everything that is already queued in
     * one go - one wake-up per burst instead of one per frame. */
    if (xQueueReceive(g_frameQueue, &f, pdMS_TO_TICKS(20)) == pdTRUE) {
      do {
        /* Always counted, so the dashboard shows the bus even while idle. */
        busObserve(g_bus, f, &g_dbc);
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
