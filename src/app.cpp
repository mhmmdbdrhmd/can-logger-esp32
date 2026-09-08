/* ============================================================================
 *  Dual CAN Logger ESP32 - two MCP2515 bus recorders writing one decoded CSV
 *
 *  ---------------------------------------------------------------------------
 *  HOW THE RECEIVE PATH AVOIDS BOTH LOSS AND LATENCY, TWICE OVER
 *
 *  A busy 500 kbit/s bus delivers thousands of frames a second and an MCP2515
 *  has room for exactly two. At 500 kbit/s a third frame arrives about 200 us
 *  after the first, against SD block writes that can stall for 320 ms on a bad
 *  card. Polling cannot bridge that, so the path is staged, and the first stage
 *  is deliberately tiny:
 *
 *   1. INT falls  ->  ISR (a few microseconds, IRAM-resident), one per bus
 *        Takes the arrival timestamp with esp_timer_get_time() - this is the
 *        number that ends up in the CSV, so it is captured before any queuing
 *        or scheduling delay can smear it - pushes it into that bus's ring and
 *        unblocks the reader task. NO SPI IN THE ISR: an SPI transaction can
 *        block, and blocking in an interrupt handler is how frames are lost.
 *        The work done here is bounded and constant, whatever the bus is doing.
 *
 *        Both buses share one esp_timer, so a frame on CAN1 and a frame on CAN2
 *        are directly comparable to the microsecond. That is the entire reason
 *        to log two buses on one device instead of two devices.
 *
 *   2. CAN reader task, priority 20, application core - ONE task, BOTH buses
 *        Drains every receive buffer on controller 1, then controller 2, and
 *        keeps draining each until it reports empty. This is what makes the
 *        edge-triggered interrupt safe: if a second frame arrives while INT is
 *        still low there is no new edge, but the drain loop picks it up anyway.
 *        A 20 ms timeout on the wait re-runs the drain unconditionally, so even
 *        a completely missed interrupt costs latency, never data.
 *
 *        ONE task rather than one per bus, and that is a deliberate choice.
 *        Two tasks would contend for the SPI bus mutex on the one path with a
 *        hard deadline, adding a priority-inversion surface for nothing: the
 *        work is identical either way, and serialising it here makes the worst
 *        case something you can compute. Drain time is measured directly into
 *        g_rec.drainMaxUs, and the design is only honest while that stays well
 *        under 200 us.
 *
 *   3. Writer task, priority 10, application core
 *        Decodes against that bus's frame map, formats CSV text, fills a 32 KB
 *        block, hands it to the SD card. While it is blocked in that write the
 *        reader task simply preempts it.
 *
 *   4. Wi-Fi and HTTP live on core 0 and in loop() at the lowest priority,
 *        where they cannot interfere with any of the above.
 *
 *  Every place a frame could still be lost is counted and reported, per bus for
 *  the controller's own overflow flags (stage 1->2) and once for the shared
 *  queue-full counter (stage 2->3). A recording that ends with `lost 0` is
 *  provably complete - on both buses.
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
#include "dash.h"
#include "dashstore.h"
#include "cantx.h"

#if ENABLE_OTA
#include <ArduinoOTA.h>
#endif

/* One SPI bus, two controllers. Only CS and INT are unique per controller -
 * MISO tri-states while CS is high - so the wiring cost of the second bus is
 * two pins. Both objects share s_canSpi, and because only the CAN task ever
 * touches either of them, the bus mutex inside SPIClass is never contended. */
static SPIClass s_canSpi(VSPI);
static MCP2515  s_can1(s_canSpi, PIN_CAN1_CS, CAN_SPI_HZ);
static MCP2515  s_can2(s_canSpi, PIN_CAN2_CS, CAN_SPI_HZ);

/* Indexed by bus, matching CanFrame::bus and g_rec.bus[]: 0 = CAN1, 1 = CAN2.
 * Pointers rather than an array of objects because MCP2515 holds a reference,
 * and this keeps the initialisation obvious on every C++ dialect the Arduino
 * cores have shipped. */
static MCP2515 *const s_can[CAN_BUSES]      = { &s_can1, &s_can2 };
static const uint8_t  s_intPin[CAN_BUSES]   = { PIN_CAN1_INT, PIN_CAN2_INT };
/* Not const: with CANn_AUTODETECT these hold what the search found, and every
 * later user of them - the log line, the health card, the "no traffic" hint -
 * has to see the rate the bus is actually running at rather than the guess it
 * started from. */
static uint16_t s_bitrate[CAN_BUSES]        = { CAN1_BITRATE_KBPS, CAN2_BITRATE_KBPS };
static uint8_t  s_crystal[CAN_BUSES]        = { CAN1_CRYSTAL_MHZ, CAN2_CRYSTAL_MHZ };
static const bool     s_listen[CAN_BUSES]   = { CAN1_LISTEN_ONLY, CAN2_LISTEN_ONLY };
static const bool     s_enabled[CAN_BUSES]  = { true, CAN2_ENABLED ? true : false };
static const bool     s_autoDet[CAN_BUSES]  = { CAN1_AUTODETECT ? true : false,
                                                CAN2_AUTODETECT ? true : false };

static TaskHandle_t s_canTask = nullptr;

/* ---- ISR -> task timestamp hand-off ------------------------------------ */
/* Power of two so the wrap is a mask. Sized well above the two frames a
 * controller can hold, to absorb a burst of interrupts during an SD stall.
 * One ring per bus: a timestamp that cannot be attributed to a controller is
 * worthless once there are two of them. */
#define TS_RING 32
static volatile uint64_t s_ts[CAN_BUSES][TS_RING];
static volatile uint8_t  s_tsHead[CAN_BUSES] = { 0, 0 };
static volatile uint8_t  s_tsTail[CAN_BUSES] = { 0, 0 };

/* Kept in IRAM: the flash cache can be disabled during an SPI flash write, and
 * an ISR that lives in flash would fault if it ran at that moment. Both
 * handlers share this body, and it is IRAM-resident for the same reason they
 * are - a call out to flash would defeat the point of putting them there. */
static void IRAM_ATTR canIsrBody(uint8_t b) {
  const uint64_t now = (uint64_t)esp_timer_get_time();

  const uint8_t head = s_tsHead[b];
  const uint8_t next = (uint8_t)((head + 1) & (TS_RING - 1));
  if (next != s_tsTail[b]) {       /* drop the timestamp, never the frame */
    s_ts[b][head] = now;
    s_tsHead[b]   = next;
  }

  g_rec.bus[b].irqCount++;        /* proves THIS INT line is actually firing */

  /* Both buses wake the same task. It drains both controllers on every pass,
   * so a notification from either is enough - and the count of pending
   * notifications is irrelevant, which is why pdTRUE clears it below. */
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_canTask, &woken);
  if (woken) portYIELD_FROM_ISR();
}

static void IRAM_ATTR canIsr1() { canIsrBody(0); }
static void IRAM_ATTR canIsr2() { canIsrBody(1); }

static void (*const s_isr[CAN_BUSES])() = { canIsr1, canIsr2 };

/* Arrival timestamp for the frame we are about to read off bus `b`. Falls back
 * to "now" if that ring ran dry, which can only happen after an interrupt
 * storm. */
static inline uint64_t popTimestamp(uint8_t b) {
  if (s_tsTail[b] != s_tsHead[b]) {
    const uint64_t t = s_ts[b][s_tsTail[b]];
    s_tsTail[b] = (uint8_t)((s_tsTail[b] + 1) & (TS_RING - 1));
    return t;
  }
  return (uint64_t)esp_timer_get_time();
}

/* ---- finding a bus's bit rate ------------------------------------------- */
/* Listen at one (bit rate, crystal) pair and report how many whole frames
 * decoded. A frame only reaches a receive buffer after its CRC has passed, so
 * this is not "did the line wiggle" - at the wrong bit rate the count stays at
 * zero however busy the bus is, which is what makes the search possible at
 * all.
 *
 * LISTEN-ONLY, always, whatever this bus is configured for: at the wrong bit
 * rate a normal-mode node reads valid traffic as malformed and answers with
 * error frames, and a diagnostic logger that corrupts the bus while it works
 * out how to read it would be worse than no logger. Listen-only never drives
 * the wire, so every wrong guess here is silent.
 *
 * Runs during setup, before the reader task and the interrupts exist, so
 * draining the controller by hand here cannot race anything - and the frames
 * it hears are discarded rather than recorded, which is honest: nobody asked
 * to log a bus at a bit rate that had not been established yet. */
static uint16_t autoListen(MCP2515 &can, uint16_t kbps, uint8_t crystalMHz) {
  /* begin() reprograms the timing and leaves the chip silent in configuration
   * mode; it fails only for a pair the driver has no timings for, which the
   * caller's tables already exclude. */
  if (!can.begin(kbps, crystalMHz))  return 0;
  if (!can.startReceiving(true))     return 0;

  uint16_t   frames = 0;
  CanFrame   f;
  const uint32_t deadline = millis() + CAN_AUTODETECT_MS;

  while ((int32_t)(millis() - deadline) < 0) {
    while (can.readFrame(f)) {
      if (++frames >= CAN_AUTODETECT_FRAMES) return frames;
    }
    /* At a wrong bit rate this is where the evidence piles up - overflow and
     * message-error flags, sticky and interrupt-latching. Cleared so the next
     * candidate starts from a clean chip rather than inheriting the last one's
     * complaints. */
    can.clearErrorInterrupts();
    can.takeRxOverflow();
    delay(1);
  }
  return frames;
}

/* Walk the pairs and keep the first that decodes. Returns true and writes back
 * through `kbps`/`crystalMHz`; leaves both alone when nothing decoded, so the
 * caller's config.h values survive a failed search untouched.
 *
 * Order matters and is not arbitrary. The configured pair goes first, so a
 * correct config.h is confirmed in one window instead of paying for the whole
 * sweep. Then the rest of the CONFIGURED crystal's rates, and only then the
 * other crystal - because a crystal and a bit rate multiply, and 250 kbit/s
 * timings on an 8 MHz part decode a 500 kbit/s bus on a 16 MHz one perfectly.
 * Nothing visible over SPI separates those two cases, so the search cannot
 * discover the crystal; what it can do is trust the one in config.h first, and
 * say so when it had to fall back to the other. */
static bool autoDetect(MCP2515 &can, uint8_t b,
                       uint16_t *kbps, uint8_t *crystalMHz) {
  const uint16_t wantKbps = *kbps;
  const uint8_t  wantXtal = *crystalMHz;

  /* Both loops run one extra step, with index 0 meaning "what config.h says"
   * and the rest walking the driver's tables while skipping that same value.
   * Written the same way twice so the two orderings read as one rule. */
  for (uint8_t ci = 0; ci <= MCP_CRYSTAL_COUNT; ci++) {
    const uint8_t x = (ci == 0) ? wantXtal : MCP_CRYSTALS[ci - 1];
    if (ci && x == wantXtal) continue;           /* already tried, first */

    for (uint8_t ri = 0; ri <= MCP_RATE_COUNT; ri++) {
      const uint16_t r = (ri == 0) ? wantKbps : MCP_RATES[ri - 1];
      if (ri && r == wantKbps) continue;         /* already tried, first */

      const uint16_t got = autoListen(can, r, x);
      LOG_FILE(LVL_DEBUG, "CAN%u autodetect: %u kbit/s @ %u MHz -> %u frame(s)",
               (unsigned)(b + 1), (unsigned)r, (unsigned)x, (unsigned)got);

      if (got >= CAN_AUTODETECT_FRAMES) {
        *kbps = r;
        *crystalMHz = x;
        return true;
      }
    }
  }
  return false;
}

/* ---- CAN reader task ---------------------------------------------------- */
static void canTaskFn(void *arg) {
  (void)arg;
  CanFrame f;

  for (;;) {
    /* Woken by either ISR, or every 20 ms as a safety net so a lost edge can
     * never wedge the receiver. */
    /* That safety net is a last resort, not a mode of operation: 50 wake-ups/s
     * x 2 receive buffers caps throughput at ~100 frames/s per bus. If the
     * status line ever shows a healthy rx with irq=0/s on a bus, that bus's
     * interrupt is dead and this poll is all that is left. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    g_rec.wakeCount++;

    /* Measured across BOTH controllers, because that is what the deadline is
     * about: a frame arriving on CAN2 does not care that the task was busy
     * with CAN1. Started here and stopped before txService below - a transmit
     * blocks for milliseconds by design and would swamp a figure that is only
     * meaningful in microseconds. */
    const uint32_t drainStart = micros();

    for (uint8_t b = 0; b < CAN_BUSES; b++) {
      /* `present` and not just `enabled`: with no module fitted, that chip
       * select selects nothing and MISO is left floating. A float that happens
       * to read back as 0xFF looks like READ STATUS reporting a full receive
       * buffer, and readFrame() would then never return false - this loop
       * would spin until the watchdog fired. Skipping a controller that did
       * not answer at boot is what makes "fit one module, leave the other
       * socket empty" a supported configuration rather than a hang. */
      if (!s_enabled[b] || !g_rec.bus[b].present) continue;

      BusHealth &h = g_rec.bus[b];
      MCP2515   &c = *s_can[b];

      /* Drain until this controller is empty. Both of its receive buffers are
       * checked on every pass, which is what covers the missed-edge case. */
      while (c.readFrame(f)) {
        f.esp_us = popTimestamp(b);
        f.bus    = b;

        h.framesRx++;
        h.lastFrameMs = millis();

        /* Bits this frame occupied on the wire, for the bus-load figure. A
         * standard data frame is 44 fixed bits + 8 per data byte, plus 3 bits
         * of inter-frame space, plus stuffing - which applies to the 34 + 8*len
         * bits from SOF to CRC and adds at most one bit per five. Extended
         * frames carry 20 more bits of identifier. */
        h.rxBits += (f.ext ? 67u : 47u) + 8u * f.len
                  + ((f.ext ? 54u : 34u) + 8u * f.len) / 5u;

        if (xQueueSend(g_frameQueue, &f, 0) != pdTRUE) {
          /* The writer could not keep up. Counted once, not per bus: there is
           * one queue, and a frame that did not fit is lost whichever
           * controller it came from. A recording is only trustworthy if this
           * stays at zero. */
          g_rec.queueDropped++;
        }
      }

      /* This controller itself overflowed: a frame was lost before we saw it. */
      const uint8_t ovf = c.takeRxOverflow();
      if (ovf) {
        h.canOvfEvents++;

        /* One sticky bit per receive buffer, so both set means at least two
         * frames went missing. How many MORE is not knowable here - the
         * controller only remembers THAT it happened, not how often - so this
         * is a floor and is named like one. Against the rolling counters this
         * bus carries, the true figure was about 1.7x it.
         *
         * Counted by set bits rather than by naming the two constants, because
         * takeRxOverflow() is documented to return those bits and nothing else,
         * and a popcount stays right if that ever widens. */
        uint8_t buffers = 0;
        for (uint8_t bit = ovf; bit; bit &= (uint8_t)(bit - 1)) buffers++;
        h.canOvfFramesMin += buffers ? buffers : 1;

        LOG_FILE(LVL_WARN, "CAN%u receive overflow (EFLG=0x%02X) - at least %u "
                           "frame(s) lost", (unsigned)(b + 1), ovf,
                 (unsigned)buffers);
      }

      /* MUST happen every pass, for every controller. ERRIF and MERRF are
       * sticky and the INT pin is level active-low, so one latched flag kills
       * every future edge and drops that bus into the 20 ms poll above -
       * permanently, and only for that bus, which is exactly the kind of
       * half-failure the per-bus counters exist to make visible. */
      const uint8_t sticky = c.clearErrorInterrupts();
      if (sticky) {
        h.canIntfSticky++;
        LOG_FILE(LVL_DEBUG, "CAN%u: cleared sticky CANINTF=0x%02X (would have "
                            "wedged INT)", (unsigned)(b + 1), sticky);
      }

      h.intLevel = (uint8_t)digitalRead(s_intPin[b]);
    }

    const uint32_t drainUs = micros() - drainStart;
    if (drainUs > g_rec.drainMaxUs) g_rec.drainMaxUs = drainUs;

    /* Last, and in this task rather than in the web handler: the receive path
     * has already been drained, so a transmit cannot delay a frame that was
     * waiting, and nothing else ever holds either chip select.
     *
     * A send does block this task for as long as the controller takes to
     * finish with the frame - milliseconds in the worst case, which is far
     * longer than the receive deadline. That is a real cost and it is paid on
     * both buses, but only while somebody is actually pressing Send, and if it
     * ever costs a frame the overflow counters above will say so rather than
     * letting it pass silently. One-shot mode keeps the worst case bounded. */
    for (uint8_t b = 0; b < CAN_BUSES; b++) {
      if (s_enabled[b] && g_rec.bus[b].present) txService(*s_can[b], b);
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
     *     is disabled while flash is being written, and although both handlers
     *     are in IRAM, the Arduino core's shared GPIO dispatcher they are
     *     reached through may not be. Detaching removes the question. */
    for (uint8_t b = 0; b < CAN_BUSES; b++) {
      if (s_enabled[b] && g_rec.bus[b].present) {
        detachInterrupt(digitalPinToInterrupt(s_intPin[b]));
      }
    }

    LOG_LIVE(LVL_WARN, "OTA UPDATE STARTING - closing files, pausing recording");
    if (!recorderStopAndWait(4000)) {
      LOG_LIVE(LVL_ERROR, "recording did not close in time - continuing anyway");
    }
    dashStoreService();    /* safe now, and the flash is about to be rewritten */
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
     * rather than sitting there deaf until someone power-cycles it. Both
     * buses: half a logger is harder to diagnose than none. */
    for (uint8_t b = 0; b < CAN_BUSES; b++) {
      if (s_enabled[b] && g_rec.bus[b].present) {
        attachInterrupt(digitalPinToInterrupt(s_intPin[b]), s_isr[b], FALLING);
      }
    }
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
  LOG_FILE(LVL_INFO, "pins  VSPI: sck=%d miso=%d mosi=%d @ %lu Hz (shared)",
           PIN_CAN_SCK, PIN_CAN_MISO, PIN_CAN_MOSI, (unsigned long)CAN_SPI_HZ);
  LOG_FILE(LVL_INFO, "pins  CAN1: cs=%d int=%d | CAN2: cs=%d int=%d%s",
           PIN_CAN1_CS, PIN_CAN1_INT, PIN_CAN2_CS, PIN_CAN2_INT,
           CAN2_ENABLED ? "" : " (DISABLED)");
  LOG_FILE(LVL_INFO, "pins  SD : cs=%d sck=%d miso=%d mosi=%d @ %lu Hz",
           PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, (unsigned long)SD_SPI_HZ);

  g_frameQueue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(CanFrame));
  if (!g_frameQueue) {
    LOG_LIVE(LVL_ERROR, "out of memory allocating the frame queue - halted");
    for (;;) { logService(); delay(1000); }
  }
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    busReset(g_bus[b]);
    liveReset(g_live[b]);
  }
  txBegin();

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

  /* The dashboard layout: flash first, then the card reconciles against it.
   * After the frame map, because the layout's signal references are resolved
   * against it. */
  dashStoreBegin();
  recorderLoadDash();

  netLoadConfig();

  /* ---- CAN controllers ----
   * One SPI bus for both. begin() is given CAN1's chip select only because
   * SPIClass wants one to drive; every transaction sets its own CS explicitly,
   * and CAN2's pin is configured below. */
  s_canSpi.begin(PIN_CAN_SCK, PIN_CAN_MISO, PIN_CAN_MOSI, PIN_CAN1_CS);
  pinMode(PIN_CAN2_CS, OUTPUT);
  digitalWrite(PIN_CAN2_CS, HIGH);   /* idle high before anything talks */

  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    BusHealth &h = g_rec.bus[b];
    h.enabled     = s_enabled[b];
    h.bitrateKbps = s_bitrate[b];
    h.crystalMHz  = s_crystal[b];
    h.listenOnly  = s_listen[b];
    h.autoDetect  = s_enabled[b] && s_autoDet[b];

    if (!s_enabled[b]) {
      LOG_LIVE(LVL_INFO, "CAN%u disabled in config.h - running single-bus",
               (unsigned)(b + 1));
      continue;
    }

    pinMode(s_intPin[b], INPUT_PULLUP);

    /* Configured but still silent - it does not open the bus until
     * startReceiving() below, once the reader task and the ISRs exist. */
    if (s_can[b]->begin(s_bitrate[b], s_crystal[b])) {
      h.present = true;

      /* Only now, with the controller proven to answer, is it worth listening
       * for the bus's own bit rate - a search against a chip that is not there
       * would just be ten silent windows and a misleading warning. */
      if (h.autoDetect) {
        uint16_t kbps = s_bitrate[b];
        uint8_t  xtal = s_crystal[b];
        const uint8_t cfgXtal = s_crystal[b];   /* before the search moves it */

        LOG_LIVE(LVL_INFO, "CAN%u listening for its bit rate (up to %u ms per "
                           "candidate, listen-only)...",
                 (unsigned)(b + 1), (unsigned)CAN_AUTODETECT_MS);

        h.autoFound = autoDetect(*s_can[b], b, &kbps, &xtal);

        if (h.autoFound) {
          s_bitrate[b] = kbps;
          s_crystal[b] = xtal;
          LOG_LIVE(LVL_INFO, "CAN%u detected: %u kbit/s (%u MHz crystal)",
                   (unsigned)(b + 1), (unsigned)kbps, (unsigned)xtal);
          if (xtal != cfgXtal) {
            /* The one case where the number above is a working setting rather
             * than a measurement: a crystal and a bit rate multiply, so if
             * this module's crystal is really the configured one, the true
             * rate is this rate scaled by the ratio between them. Said out
             * loud, because a CSV labelled with the wrong bit rate is the kind
             * of quiet error that survives into a report. */
            LOG_LIVE(LVL_WARN, "CAN%u decoded only with a %u MHz crystal, not "
                               "the %u MHz in config.h. The bus is readable "
                               "either way, but if this module really carries "
                               "%u MHz then the real rate is not %u kbit/s - "
                               "fix CAN%u_CRYSTAL_MHZ and the rate will be "
                               "right too.",
                     (unsigned)(b + 1), (unsigned)xtal,
                     (unsigned)cfgXtal, (unsigned)cfgXtal,
                     (unsigned)kbps, (unsigned)(b + 1));
          }
        } else {
          LOG_LIVE(LVL_WARN, "CAN%u bit rate not detected - nothing decoded at "
                             "any rate this driver knows. Using config.h: %u "
                             "kbit/s, %u MHz crystal. A bus with no traffic on "
                             "it looks exactly like this, so check there is a "
                             "node talking before doubting the rate.",
                   (unsigned)(b + 1), (unsigned)s_bitrate[b],
                   (unsigned)s_crystal[b]);
        }

        /* Back to configuration mode, programmed with the pair that will
         * actually be used. The search left the chip listening, and the boot
         * sequence's whole point is that no bus is open until the reader task
         * and the ISRs exist. */
        s_can[b]->begin(s_bitrate[b], s_crystal[b]);

        h.bitrateKbps = s_bitrate[b];
        h.crystalMHz  = s_crystal[b];
      }

      LOG_LIVE(LVL_INFO, "CAN%u controller OK: %u kbit/s, %s mode "
                         "(not listening yet)",
               (unsigned)(b + 1), (unsigned)s_bitrate[b],
               s_listen[b] ? "listen-only" : "normal");
      LOG_FILE(LVL_INFO, "CAN%u MCP2515: %u MHz crystal, mode=%u, filters "
                         "disabled, RXB0 rollover enabled",
               (unsigned)(b + 1), (unsigned)s_crystal[b],
               (unsigned)s_can[b]->mode());
    } else if (b > 0) {
      /* The second controller is optional hardware. Say so, at a level that
       * does not read like a fault, and carry on as a single-bus logger -
       * somebody running this firmware on one module must not be told their
       * logger is broken. */
      LOG_LIVE(LVL_WARN, "CAN%u did not answer - continuing on CAN1 alone. If "
                         "a second MCP2515 is fitted, check CS=D%d, INT=D%d, "
                         "3V3 and that module's crystal setting; if not, this "
                         "is expected and can be silenced with CAN2_ENABLED 0.",
               (unsigned)(b + 1), (int)PIN_CAN2_CS, (int)s_intPin[b]);
    } else {
      LOG_LIVE(LVL_ERROR, "CAN%u CONTROLLER NOT RESPONDING - check the MCP2515 "
                          "wiring (CS=D%d, INT=D%d, 3V3) and the crystal "
                          "setting for THAT module",
               (unsigned)(b + 1), (int)PIN_CAN1_CS, (int)s_intPin[b]);
    }
  }

  /* ---- tasks ---- */
  xTaskCreatePinnedToCore(canTaskFn, "can", TASK_STACK_CAN, nullptr,
                          TASK_PRIO_CAN, &s_canTask, TASK_CORE_CAN);
  xTaskCreatePinnedToCore(recorderTask, "writer", TASK_STACK_WRITER, nullptr,
                          TASK_PRIO_WRITER, nullptr, TASK_CORE_WRITER);

  /* Attach the interrupts only once the task exists - an early edge would
   * otherwise notify a null handle. FALLING is correct for the MCP2515's
   * active-low INT; the drain loop covers the level-triggered corner case. */
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    /* Only for a controller that actually answered. An unconnected INT pin is
     * held high by its pull-up and would never fire anyway, but arming it
     * would leave the ISR free to count edges picked up by a floating wire and
     * report an interrupt rate for a bus that does not exist. */
    if (s_enabled[b] && g_rec.bus[b].present) {
      attachInterrupt(digitalPinToInterrupt(s_intPin[b]), s_isr[b], FALLING);
    }
  }

  /* NOW open the buses. Everything that has to watch them already exists, so a
   * controller's two receive buffers cannot overflow in a gap - which used to
   * cost frames on every single boot. */
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    if (!s_enabled[b] || !g_rec.bus[b].present) continue;

    if (s_can[b]->startReceiving(s_listen[b])) {
      LOG_LIVE(LVL_INFO, "CAN%u bus open - listening%s", (unsigned)(b + 1),
               s_listen[b] ? " (listen-only: cannot Send on this bus)" : "");
    } else {
      LOG_LIVE(LVL_ERROR, "CAN%u controller would not leave configuration mode",
               (unsigned)(b + 1));
    }
  }

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

  /* Any dashboard change waiting to reach flash goes out here, once nothing is
   * being recorded - writing NVS stops the flash cache, and the CAN interrupt
   * is reached through a dispatcher that may not be resident in IRAM. */
  dashStoreService();

  if (webRebootRequested()) {
    /* Close the CSV before restarting, or the rows written since the last sync
     * are lost - the same discipline as the power-fail path. */
    LOG_LIVE(LVL_WARN, "restarting - closing files first");
    if (!recorderStopAndWait(4000)) {
      LOG_LIVE(LVL_ERROR, "recording did not close in time - restarting anyway");
    }
    /* The recording is closed now, so this is the moment the deferred flash
     * write is both safe and last useful. */
    dashStoreService();
    logService();
    delay(250);          /* let the HTTP reply and the log actually go out */
    ESP.restart();
  }
  serviceLed();
  delay(2);          /* yields to the idle task; the real work is in tasks */
}
