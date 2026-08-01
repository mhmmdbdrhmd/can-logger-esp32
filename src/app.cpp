/* ============================================================================
 *  CAN Logger ESP32 - an MCP2515 bus recorder that writes decoded CSV to SD
 *
 *  ---------------------------------------------------------------------------
 *  HOW THE RECEIVE PATH AVOIDS BOTH LOSS AND LATENCY
 *
 *  A busy 250 kbit/s bus delivers on the order of 1000 frames/s and the MCP2515
 *  has room for exactly two. That is roughly 2 ms of slack, against SD block
 *  writes that can stall for 100 ms on a bad card. Polling cannot bridge that,
 *  so the path is staged, and the first stage is deliberately tiny:
 *
 *   1. INT falls  ->  ISR (a few microseconds, IRAM-resident)
 *        Takes the arrival timestamp with esp_timer_get_time() - this is the
 *        number that ends up in the CSV, so it is captured before any queuing
 *        or scheduling delay can smear it - pushes it into a small ring and
 *        unblocks the reader task. NO SPI IN THE ISR: an SPI transaction can
 *        block, and blocking in an interrupt handler is how frames are lost.
 *        The work done here is bounded and constant, whatever the bus is doing.
 *
 *   2. CAN reader task, priority 20, application core
 *        Drains BOTH receive buffers over SPI and keeps draining until the
 *        controller reports empty. This is what makes the edge-triggered
 *        interrupt safe: if a second frame arrives while INT is still low there
 *        is no new edge, but the drain loop picks it up anyway. A 20 ms timeout
 *        on the wait re-runs the drain unconditionally, so even a completely
 *        missed interrupt costs latency, never data. Frames go into a
 *        1024-deep queue = one full second of buffering.
 *
 *   3. Writer task, priority 10, application core
 *        Decodes against the frame map, formats CSV text, fills an 8 KB block,
 *        hands it to the SD card. While it is blocked in that write the reader
 *        task simply preempts it.
 *
 *   4. Wi-Fi and HTTP live on core 0 and in loop() at the lowest priority,
 *        where they cannot interfere with either of the above.
 *
 *  Every place a frame could still be lost is counted and reported: the
 *  controller's own overflow flags (stage 1->2) and the queue-full counter
 *  (stage 2->3). A recording that ends with `lost 0` is provably complete.
 * ==========================================================================*/

#include "app.h"
#include <Arduino.h>
#include <SPI.h>

#include "config.h"
#include "mcp2515.h"
#include "dbc.h"
#include "decode.h"
#include "logger.h"
#include "recorder.h"
#include "netcfg.h"
#include "webui.h"

#if ENABLE_OTA
#include <ArduinoOTA.h>
#endif

static SPIClass s_canSpi(VSPI);
static MCP2515  s_can(s_canSpi, PIN_CAN_CS, CAN_SPI_HZ);

static TaskHandle_t s_canTask = nullptr;

/* ---- ISR -> task timestamp hand-off ------------------------------------ */
/* Power of two so the wrap is a mask. Sized well above the two frames the
 * controller can hold, to absorb a burst of interrupts during an SD stall. */
#define TS_RING 32
static volatile uint64_t s_ts[TS_RING];
static volatile uint8_t  s_tsHead = 0;
static volatile uint8_t  s_tsTail = 0;

/* Kept in IRAM: the flash cache can be disabled during an SPI flash write, and
 * an ISR that lives in flash would fault if it ran at that moment. */
static void IRAM_ATTR canIsr() {
  const uint64_t now = (uint64_t)esp_timer_get_time();

  const uint8_t head = s_tsHead;
  const uint8_t next = (uint8_t)((head + 1) & (TS_RING - 1));
  if (next != s_tsTail) {          /* drop the timestamp, never the frame */
    s_ts[head] = now;
    s_tsHead   = next;
  }

  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_canTask, &woken);
  if (woken) portYIELD_FROM_ISR();
}

/* Arrival timestamp for the frame we are about to read. Falls back to "now" if
 * the ring ran dry, which can only happen after an interrupt storm. */
static inline uint64_t popTimestamp() {
  if (s_tsTail != s_tsHead) {
    const uint64_t t = s_ts[s_tsTail];
    s_tsTail = (uint8_t)((s_tsTail + 1) & (TS_RING - 1));
    return t;
  }
  return (uint64_t)esp_timer_get_time();
}

/* ---- CAN reader task ---------------------------------------------------- */
static void canTaskFn(void *arg) {
  (void)arg;
  CanFrame f;

  for (;;) {
    /* Woken by the ISR, or every 20 ms as a safety net so a lost edge can
     * never wedge the receiver. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));

    /* Drain until the controller is empty. Both receive buffers are checked on
     * every pass, which is what covers the missed-edge case. */
    while (s_can.readFrame(f)) {
      f.esp_us = popTimestamp();

      g_rec.framesRx++;
      g_rec.lastFrameMs = millis();

      if (xQueueSend(g_frameQueue, &f, 0) != pdTRUE) {
        /* The writer could not keep up for a full second. Count it - a
         * recording is only trustworthy if this stays at zero. */
        g_rec.queueDropped++;
      }
    }

    /* The controller itself overflowed: a frame was lost before we saw it. */
    const uint8_t ovf = s_can.takeRxOverflow();
    if (ovf) {
      g_rec.canOverflow++;
      LOG_FILE(LVL_WARN, "MCP2515 receive overflow (EFLG=0x%02X) - a frame was lost", ovf);
    }
  }
}

/* ---- over-the-air updates ----------------------------------------------- */
#if ENABLE_OTA
static void setupOta() {
  ArduinoOTA.setHostname(g_net.hostname.c_str());
  if (OTA_PASSWORD[0] != '\0') ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    /* An OTA erases and rewrites the flash. Two things must be true first:
     *
     *  1. no file may be open on the SD card - the update reboots the board the
     *     moment it finishes, and a CSV whose length was never committed would
     *     lose everything since the last sync,
     *  2. the CAN interrupt must not fire during the update - the flash cache
     *     is disabled while flash is being written, and although canIsr is in
     *     IRAM, the Arduino core's shared GPIO dispatcher it is reached through
     *     may not be. Detaching removes the question entirely. */
    detachInterrupt(digitalPinToInterrupt(PIN_CAN_INT));

    LOG_LIVE(LVL_WARN, "OTA UPDATE STARTING - closing files, pausing recording");
    if (!recorderStopAndWait(4000)) {
      LOG_LIVE(LVL_ERROR, "recording did not close in time - continuing anyway");
    }
    logService();          /* flush the log queue while the card is still ours */
  });

  ArduinoOTA.onEnd([]() {
    LOG_LIVE(LVL_INFO, "OTA complete - rebooting into the new firmware");
    logService();
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static uint8_t lastPct = 255;
    const uint8_t pct = total ? (uint8_t)((done * 100UL) / total) : 0;
    if (pct != lastPct && (pct % 10) == 0) {   /* one line per 10%, not per packet */
      lastPct = pct;
      LOG_LIVE(LVL_INFO, "OTA %u%%", (unsigned)pct);
      logService();
    }
  });

  ArduinoOTA.onError([](ota_error_t err) {
    const char *what = "unknown";
    switch (err) {
      case OTA_AUTH_ERROR:    what = "authentication failed - wrong OTA password"; break;
      case OTA_BEGIN_ERROR:   what = "begin failed - partition table has no OTA slot?"; break;
      case OTA_CONNECT_ERROR: what = "connection lost"; break;
      case OTA_RECEIVE_ERROR: what = "receive failed"; break;
      case OTA_END_ERROR:     what = "end failed"; break;
    }
    LOG_LIVE(LVL_ERROR, "OTA FAILED: %s", what);
    logService();
    /* Put the receive path back so the logger keeps working on the old firmware
     * rather than sitting there deaf until someone power-cycles it. */
    attachInterrupt(digitalPinToInterrupt(PIN_CAN_INT), canIsr, FALLING);
  });

  ArduinoOTA.begin();

  LOG_LIVE(LVL_INFO, "OTA ready on %s - upload over Wi-Fi, no cable needed",
           netIp().c_str());
  LOG_FILE(LVL_INFO, "OTA: hostname=%s password=%s",
           g_net.hostname.c_str(), OTA_PASSWORD[0] ? "set" : "NONE");
}
#endif

/* ---- power-fail input --------------------------------------------------- */
#if PIN_POWER_FAIL >= 0
/* Runs while the supply is already collapsing and the ESP32 is living off its
 * hold-up capacitor. Does nothing but raise a flag - opening, flushing and
 * closing files are all forbidden from an ISR. The writer task picks it up on
 * its next pass, at most 20 ms later. */
static void IRAM_ATTR powerFailIsr() {
  recorderSignalPowerFail();
}
#endif

/* ---- status LED --------------------------------------------------------- */
static void serviceLed() {
#if PIN_STATUS_LED >= 0
  static uint32_t last = 0;
  static bool     on   = false;
  const uint32_t  now  = millis();

  if (!g_rec.sdOk || g_rec.sdError) {           /* fast blink = fault       */
    if (now - last >= 120) { last = now; on = !on; digitalWrite(PIN_STATUS_LED, on); }
  } else if (g_rec.recording) {                 /* slow blink = recording   */
    if (now - last >= 500) { last = now; on = !on; digitalWrite(PIN_STATUS_LED, on); }
  } else {                                      /* steady off = idle        */
    if (on) { on = false; digitalWrite(PIN_STATUS_LED, LOW); }
  }
#endif
}

/* ======================================================================== */
void appSetup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  logInit();

#if PIN_STATUS_LED >= 0
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
#endif

  LOG_LIVE(LVL_INFO, "==== %s v%s ====", FIRMWARE_NAME, FIRMWARE_VERSION);
  LOG_FILE(LVL_INFO, "build %s %s, chip %s rev %d, %d MHz, flash %lu KB",
           __DATE__, __TIME__, ESP.getChipModel(), ESP.getChipRevision(),
           (int)ESP.getCpuFreqMHz(), (unsigned long)(ESP.getFlashChipSize() / 1024));
  LOG_FILE(LVL_INFO, "pins  CAN: cs=%d int=%d sck=%d miso=%d mosi=%d @ %lu Hz",
           PIN_CAN_CS, PIN_CAN_INT, PIN_CAN_SCK, PIN_CAN_MISO, PIN_CAN_MOSI,
           (unsigned long)CAN_SPI_HZ);
  LOG_FILE(LVL_INFO, "pins  SD : cs=%d sck=%d miso=%d mosi=%d @ %lu Hz",
           PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, (unsigned long)SD_SPI_HZ);

  g_frameQueue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(CanFrame));
  if (!g_frameQueue) {
    LOG_LIVE(LVL_ERROR, "out of memory allocating the frame queue - halted");
    for (;;) { logService(); delay(1000); }
  }
  busReset(g_bus);
  liveReset(g_live);

  /* ---- SD card ---- */
  if (recorderBeginSD()) {
    LOG_LIVE(LVL_INFO, "SD card OK: %s, %lu MB", g_rec.sdType,
             (unsigned long)g_rec.sdSizeMB);
  } else {
    LOG_LIVE(LVL_ERROR, "SD CARD NOT FOUND - nothing will be saved. "
                        "Insert a FAT32 card and restart.");
  }

  /* ---- the frame map, then the network: both live on that card ---- */
  recorderLoadDbc();
  netLoadConfig();

  /* ---- CAN controller ---- */
  s_canSpi.begin(PIN_CAN_SCK, PIN_CAN_MISO, PIN_CAN_MOSI, PIN_CAN_CS);
  pinMode(PIN_CAN_INT, INPUT_PULLUP);

  if (s_can.begin(CAN_BITRATE_KBPS, CAN_CRYSTAL_MHZ, CAN_LISTEN_ONLY)) {
    LOG_LIVE(LVL_INFO, "CAN controller OK: %d kbit/s, %s mode",
             CAN_BITRATE_KBPS, CAN_LISTEN_ONLY ? "listen-only" : "normal");
    LOG_FILE(LVL_INFO, "MCP2515: %u MHz crystal, mode=%u, filters disabled, "
                       "RXB0 rollover enabled",
             (unsigned)CAN_CRYSTAL_MHZ, (unsigned)s_can.mode());
  } else {
    LOG_LIVE(LVL_ERROR, "CAN CONTROLLER NOT RESPONDING - check the MCP2515 "
                        "wiring (CS=D%d, 3V3) and the crystal setting",
             PIN_CAN_CS);
  }

  /* ---- tasks ---- */
  xTaskCreatePinnedToCore(canTaskFn, "can", TASK_STACK_CAN, nullptr,
                          TASK_PRIO_CAN, &s_canTask, TASK_CORE_CAN);
  xTaskCreatePinnedToCore(recorderTask, "writer", TASK_STACK_WRITER, nullptr,
                          TASK_PRIO_WRITER, nullptr, TASK_CORE_WRITER);

  /* Attach the interrupt only once the task exists - an early edge would
   * otherwise notify a null handle. FALLING is correct for the MCP2515's
   * active-low INT; the drain loop covers the level-triggered corner case. */
  attachInterrupt(digitalPinToInterrupt(PIN_CAN_INT), canIsr, FALLING);

#if PIN_POWER_FAIL >= 0
  pinMode(PIN_POWER_FAIL, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_POWER_FAIL), powerFailIsr,
                  POWER_FAIL_ACTIVE_LOW ? FALLING : RISING);
  LOG_LIVE(LVL_INFO, "power-fail watchdog armed on D%d - recordings will be "
                     "closed cleanly on supply loss", PIN_POWER_FAIL);
#else
  LOG_FILE(LVL_INFO, "no power-fail input configured: an unannounced power cut "
                     "costs up to %u ms of data (SD_SYNC_INTERVAL_MS)",
           (unsigned)SD_SYNC_INTERVAL_MS);
#endif

  /* ---- Wi-Fi and dashboard, after the recording path is already live ---- */
  netBegin();
  webBegin();

#if ENABLE_OTA
  setupOta();
#endif

#if AUTO_START_RECORDING
  if (g_rec.sdOk) {
    recorderRequestStart();
  } else {
    LOG_LIVE(LVL_WARN, "auto-start skipped: no SD card");
  }
#else
  LOG_LIVE(LVL_INFO, "idle - press START on the dashboard to record");
#endif
}

void appLoop() {
#if ENABLE_OTA
  ArduinoOTA.handle();
#endif
  webService();
  netService();
  serviceLed();
  delay(2);          /* yields to the idle task; the real work is in tasks */
}
