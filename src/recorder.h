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
