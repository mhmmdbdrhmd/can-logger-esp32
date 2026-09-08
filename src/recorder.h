/* ============================================================================
 *  recorder.h - SD card, file numbering, and the single task that owns them
 *
 *  Exactly one task ever touches the SD card. Producers (the CAN reader task,
 *  the web task, anything that logs) only ever push into lock-free FreeRTOS
 *  queues, so a slow card can never stall the receive path - it can only make
 *  the queues deeper, which is visible as `queuePeak` in the status line.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "config.h"
#include "mcp2515.h"

/* ---------------------------------------------------------------------------
 *  Everything that is true of ONE bus.
 *
 *  Split out from RecStatus rather than suffixed onto it, because with two
 *  controllers the important distinction is which numbers are per bus and
 *  which are shared - and a reader should not have to know the field names to
 *  tell them apart. Frames, interrupts, overflows and load belong to a bus.
 *  The queue, the card and the writer are shared by both, and summing a
 *  per-bus figure into a global one would hide exactly the fault you are
 *  looking for: one bus healthy, the other deaf.
 * -------------------------------------------------------------------------*/
struct BusHealth {
  /* Two different kinds of "not there", kept apart on purpose: `enabled` is a
   * build-time decision, `present` is what the hardware answered. Collapsing
   * them would make a missing module look like a config choice, which is the
   * one message that stops somebody checking their wiring. */
  bool     enabled     = false; /* compiled in at all (see CAN2_ENABLED)    */
  bool     present     = false; /* the controller answered at boot          */
  uint16_t bitrateKbps = 0;     /* what this bus ended up running at        */
  uint8_t  crystalMHz  = 0;     /* and the crystal that rate was figured on */
  bool     listenOnly  = false;

  /* Where those two came from. Reported separately because "500 kbit/s" the
   * machine told us and "500 kbit/s" somebody typed into config.h are worth
   * different amounts of trust, and because a detection that FAILED and fell
   * back is the case where the number on screen may be wrong - which is
   * exactly when a reader needs to be told. */
  bool     autoDetect  = false; /* CANn_AUTODETECT asked for a search       */
  bool     autoFound   = false; /* and a pair decoded real frames           */

  /* ---- frame map: one per bus, because an identifier means different
   * things on different buses ---- */
  bool     dbcLoaded   = false;
  uint16_t dbcMessages = 0;
  uint16_t dbcSignals  = 0;

  /* ---- traffic ---- */
  uint32_t framesRx    = 0;     /* frames pulled out of this controller     */
  uint32_t frameRate   = 0;     /* frames/s over the last second            */
  uint32_t lastFrameMs = 0;
  bool     canOk       = false;

  /* Two different things, kept apart because conflating them made the loss
   * figure wrong by about forty percent in ten hours of field recordings.
   *
   * canOvfEvents counts SERVICE PASSES that found this controller's overflow
   * flags set. In the wedged state that is one per 20 ms poll, so it converges
   * on a flat ~51/s - a poll rate wearing a loss figure's clothes.
   *
   * canOvfFramesMin is a LOWER BOUND on frames actually lost. EFLG has one
   * sticky bit per receive buffer, so a pass that finds both set means at
   * least two frames went missing; how many more is not knowable from the
   * controller, which only remembers that it happened. Measured against the
   * rolling counters carried by the bus itself, the true figure was roughly
   * 1.7x this - so treat it as the floor it is, never as the total. */
  uint32_t canOvfEvents    = 0;
  uint32_t canOvfFramesMin = 0;

  /* ---- receive-path diagnostics ----
   * Without these, a wedged interrupt is invisible: the logger keeps writing
   * rows, just ninety percent fewer of them. Per bus because one INT line can
   * die while the other is fine, and a shared counter would hide it. */
  uint32_t irqCount      = 0;   /* times THIS INT line actually fired        */
  uint32_t irqRate       = 0;   /* per second                                */
  uint8_t  intLevel      = 1;   /* current level of this MCP2515's INT pin   */
  bool     intStuck      = false;/* frames arriving but the line never fires */
  uint32_t canIntfSticky = 0;   /* ERRIF/MERRF events cleared                */

  /* ---- bus load ---- */
  uint64_t rxBits        = 0;   /* bits seen, incl. stuffing and IFS estimate */
  uint32_t busLoadPct    = 0;   /* percent of this bus's bit rate in use      */

  uint32_t lifeOverflow  = 0;   /* lifetime, survives across recordings       */
};

struct RecStatus {
  /* ---- storage ---- */
  bool     sdOk        = false;
  bool     sdError     = false;   /* a write failed after mounting          */
  uint64_t sdSizeMB    = 0;
  const char *sdType   = "-";

  /* ---- per bus ----
   * Indexed 0 = CAN1, 1 = CAN2, matching CanFrame::bus. */
  BusHealth bus[CAN_BUSES];

  /* ---- current recording ---- */
  bool     recording   = false;
  uint16_t fileIndex   = 0;
  char     csvName[20] = "";
  char     logName[20] = "";
  char     metaName[20] = "";
  uint32_t startMs     = 0;
  uint64_t rows        = 0;
  uint64_t bytes       = 0;

  /* ---- health, shared by both buses ----
   * One queue, one card, one writer task, so these are global by construction.
   * Anything that belongs to a single controller lives in bus[] above. */
  uint32_t queueDropped  = 0;   /* frame queue was full - data WAS lost     */
  uint32_t queuePeak     = 0;   /* deepest the frame queue has ever been    */
  uint32_t writeCount    = 0;
  uint32_t writeMaxUs    = 0;

  uint32_t wakeCount     = 0;   /* reader wake-ups, interrupt or timeout      */
  uint32_t wakeRate      = 0;

  /* Worst time one service pass took to drain BOTH controllers, in
   * microseconds. This is the number the whole dual-bus design turns on.
   *
   * A controller holds two frames. At 500 kbit/s a third arrives roughly
   * 200 us after the first, so a pass that takes longer than that can lose a
   * frame the counters above would only report as a floor. Measuring it
   * directly turns "we believe it keeps up" into something a recording can
   * show. Expect ~115 us with both buses busy; anything approaching 200 means
   * the margin is gone. */
  uint32_t drainMaxUs    = 0;

  /* Lifetime totals. The counters above are zeroed when a recording starts so
   * that "lost" describes THAT recording and not something that happened at
   * boot - otherwise one frame lost before any file existed marks every later
   * recording as lossy forever. */
  uint32_t lifeDropped   = 0;
  bool     powerFail     = false; /* a recording was closed by a supply loss */
  uint32_t syncCount     = 0;
  uint32_t syncMaxUs     = 0;     /* worst metadata sync - the exposure window*/
};

extern RecStatus g_rec;

/* Queue of raw frames, filled by the CAN reader task. */
extern QueueHandle_t g_frameQueue;

/* Mounts the card and reports what it found. Safe to call again to retry. */
bool recorderBeginSD();

/* Reads DBC_PATH and DBC2_PATH off the card into the two frame maps. Call
 * after the card is mounted and before the first recording starts. Absence of
 * either file is not an error: that bus then records raw payload bytes. */
void recorderLoadDbc();

/* Reconciles the dashboard layout on the card with the one in flash. Call
 * after dashStoreBegin() and after recorderLoadDbc(); see dashstore.h for the
 * rule that decides which copy wins. Absence of the file is not an error. */
void recorderLoadDash();

/* Asks the writer task to mirror the current layout back to DASH_PATH, so the
 * card keeps agreeing with flash after an edit in the browser. Asynchronous,
 * like the start/stop requests, because the web handler must never wait on the
 * SD card - a slow card would stall the HTTP loop, not just this write. */
void recorderRequestSaveDash();

/* Asynchronous requests - honoured by the writer task on its next pass, so
 * they are safe to call from the web handler or from setup(). */
void recorderRequestStart();
void recorderRequestStop();
bool recorderStartPending();

/* The writer task body: decode -> CSV buffer -> SD, plus log servicing and the
 * once-per-second status lines. */
void recorderTask(void *arg);

/* Called from the power-fail interrupt. Only sets a flag - the actual flush and
 * close happen in the writer task, because closing a file is not something that
 * can be done from an ISR. Safe to call at any time. */
void recorderSignalPowerFail();

/* Blocking stop, for use before an over-the-air update. An OTA rewrites the
 * flash, and neither a half-written file on the SD card nor a CAN interrupt
 * firing mid-erase may survive into that. Returns true if the recording was
 * closed within the timeout. */
bool recorderStopAndWait(uint32_t timeoutMs);

/* Elapsed recording time, milliseconds. */
uint32_t recorderElapsedMs();
