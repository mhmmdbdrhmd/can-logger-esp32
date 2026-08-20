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
#include "mcp2515.h"

struct RecStatus {
  /* ---- storage ---- */
  bool     sdOk        = false;
  bool     sdError     = false;   /* a write failed after mounting          */
  uint64_t sdSizeMB    = 0;
  const char *sdType   = "-";

  /* ---- frame map ---- */
  bool     dbcLoaded   = false;
  uint16_t dbcMessages = 0;
  uint16_t dbcSignals  = 0;

  /* ---- current recording ---- */
  bool     recording   = false;
  uint16_t fileIndex   = 0;
  char     csvName[20] = "";
  char     logName[20] = "";
  char     metaName[20] = "";
  uint32_t startMs     = 0;
  uint64_t rows        = 0;
  uint64_t bytes       = 0;

  /* ---- health ---- */
  uint32_t framesRx      = 0;   /* frames pulled out of the controller      */
  uint32_t frameRate     = 0;   /* frames/s over the last second            */
  uint32_t queueDropped  = 0;   /* frame queue was full - data WAS lost     */
  uint32_t canOverflow   = 0;   /* MCP2515 dropped it before we read it     */
  uint32_t queuePeak     = 0;   /* deepest the frame queue has ever been    */
  uint32_t writeCount    = 0;
  uint32_t writeMaxUs    = 0;
  uint32_t lastFrameMs   = 0;
  bool     canOk         = false;

  /* ---- receive-path diagnostics ----
   * Without these, a wedged interrupt is invisible: the logger keeps writing
   * rows, just ninety percent fewer of them. */
  uint32_t irqCount      = 0;   /* times the INT line actually fired          */
  uint32_t irqRate       = 0;   /* per second                                 */
  uint32_t wakeCount     = 0;   /* reader wake-ups, interrupt or timeout      */
  uint32_t wakeRate      = 0;
  uint8_t  intLevel      = 1;   /* current level of the MCP2515 INT pin       */
  bool     intStuck      = false;/* frames arriving but the line never fires  */
  uint32_t canIntfSticky = 0;   /* ERRIF/MERRF events cleared                 */

  /* ---- bus load ---- */
  uint64_t rxBits        = 0;   /* bits seen, incl. stuffing and IFS estimate */
  uint32_t busLoadPct    = 0;   /* percent of CAN_BITRATE_KBPS in use         */

  /* Lifetime totals. The counters above are zeroed when a recording starts so
   * that "lost" describes THAT recording and not something that happened at
   * boot - otherwise one frame lost before any file existed marks every later
   * recording as lossy forever. */
  uint32_t lifeDropped   = 0;
  uint32_t lifeOverflow  = 0;
  bool     powerFail     = false; /* a recording was closed by a supply loss */
  uint32_t syncCount     = 0;
  uint32_t syncMaxUs     = 0;     /* worst metadata sync - the exposure window*/
};

extern RecStatus g_rec;

/* Queue of raw frames, filled by the CAN reader task. */
extern QueueHandle_t g_frameQueue;

/* Mounts the card and reports what it found. Safe to call again to retry. */
bool recorderBeginSD();

/* Reads DBC_PATH off the card into the global frame map. Call after the card
 * is mounted and before the first recording starts. Absence of the file is not
 * an error: the logger then records raw payload bytes. */
void recorderLoadDbc();

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
