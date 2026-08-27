/* ============================================================================
 *  cantx.h - writing values back to the bus
 *
 *  Everything else in this firmware listens. This is the one part that talks,
 *  and it is built to be the boring, obvious, auditable version of talking.
 *
 *  WHY IT IS NOT SIMPLY "CALL sendFrame() FROM THE WEB HANDLER"
 *  -----------------------------------------------------------
 *  Three reasons, all of them structural:
 *
 *   1. ONE TASK OWNS THE CONTROLLER. The CAN reader task is the only thing
 *      that touches the MCP2515's SPI bus. A web handler reaching in from the
 *      HTTP loop would interleave a transmit transaction with a receive burst
 *      on the same chip select, which is exactly the sort of fault that shows
 *      up once a week and is never reproducible. So a Send becomes a request
 *      in a queue, and the CAN task performs it between drains.
 *
 *   2. THE ANSWER TAKES LONGER THAN AN HTTP HANDLER SHOULD. Waiting for the
 *      controller to finish with a frame is milliseconds, and the browser
 *      wants to know whether it was acknowledged. So a Send returns a ticket
 *      immediately and the result arrives in the next dashboard poll, which
 *      the page is making anyway.
 *
 *   3. THE ARM GATE HAS TO BE ENFORCED WHERE THE SENDING HAPPENS. A gate
 *      checked only in the browser is a decoration; a gate checked only in the
 *      HTTP handler still lets a cyclic repeat outlive it. It is checked here,
 *      on every frame, including repeats.
 *
 *  WHAT ARMING IS FOR
 *  ------------------
 *  A CAN frame sent to a live controller can move hydraulics, release a brake
 *  or enable a drive. The logger cannot know which. Arming is not security -
 *  anyone on the hotspot can arm it - it is the thing that stops a stray tap
 *  on a phone in a pocket, a bookmarked URL, or a browser restoring its tabs
 *  from sending a command nobody meant to send. It expires by itself, because
 *  a gate that stays open is not a gate.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "config.h"
#include "mcp2515.h"
#include "dash.h"

/* How a send finished. The first values line up with MCP2515::TxResult; the
 * rest are refusals that never reached the controller. */
enum TxStatus : uint8_t {
  TXS_OK = 0,
  TXS_NO_ACK,
  TXS_ARB_LOST,
  TXS_ABORTED,
  TXS_TIMEOUT,
  TXS_BUSY,
  TXS_NOT_LISTENING,
  TXS_BAD_FRAME,

  TXS_NOT_ARMED,        /* the dashboard is not armed                       */
  TXS_NO_SIGNAL,        /* the setpoint names a signal this DBC lacks       */
  TXS_OUT_OF_RANGE,     /* the value does not fit the signal                */
  TXS_QUEUE_FULL,       /* more requests in flight than the queue holds     */
  TXS_UNKNOWN_COMMAND,
  TXS_PENDING           /* queued, not yet attempted                        */
};

/* One finished (or refused) send, kept for the browser to collect. */
struct TxOutcome {
  uint32_t ticket;
  uint32_t ms;          /* millis() when it finished                        */
  uint8_t  status;
  uint8_t  cmd;         /* index in g_dash.tx, or 0xFF for a one-off frame  */
  uint32_t id;
  uint8_t  ext;
  uint8_t  len;
  uint8_t  data[8];
  float    requested;
  float    applied;     /* what the bits could actually represent           */
  uint8_t  clamped;
  int16_t  tecDelta;    /* the evidence behind a TXS_NO_ACK                 */
};

struct TxState {
  /* Written by the CAN task and by the web handler; read by both. Each is a
   * single aligned word, so a reader sees the old value or the new one. */
  volatile uint8_t  armed;
  volatile uint32_t armUntilMs;

  uint32_t sent;
  uint32_t failed;

  /* Which cyclic setpoints are currently repeating. Runtime state, not part
   * of the saved config: a logger that came back from a power cut still
   * repeating a command nobody re-enabled would be a nasty surprise. */
  uint32_t cyclicOn;                    /* bitmask over g_dash.tx           */
  uint32_t cyclicNextMs[TX_MAX_COMMANDS];
  float    cyclicValue[TX_MAX_COMMANDS];

  TxOutcome        ring[TX_RESULT_RING];
  volatile uint8_t ringCount;           /* total ever written, wraps freely */
};

extern TxState g_tx;

/* Creates the request queue. Call from setup(), before the CAN task starts. */
void txBegin();

/* ---- the arm gate ------------------------------------------------------- */
void     txArm(bool on, const char *who);
bool     txArmed();
uint32_t txArmRemainingMs();

/* ---- asking for a send -------------------------------------------------- *
 * Both return a ticket the browser can match against an outcome, or 0 when the
 * request was refused outright - in which case an outcome carrying the reason
 * has already been recorded, so the page still gets told why. */
uint32_t txSendCommand(uint8_t cmdIndex, float value);

/* One member of a group that has to leave in a single frame. With `hold` set
 * the value is written into the frame under construction and nothing is
 * transmitted; the last member of the group is sent with hold false, and that
 * is the call that puts the finished frame on the wire. Members are drained in
 * order by one task, so nothing can be interleaved between them. */
uint32_t txSendPart(uint8_t cmdIndex, float value, bool hold);
uint32_t txSendRaw(uint32_t id, bool ext, const uint8_t *data, uint8_t len);

/* Starts or stops a cyclic setpoint. Repeats stop on their own when the
 * dashboard disarms. */
void txSetCyclic(uint8_t cmdIndex, bool on, float value);
bool txCyclicOn(uint8_t cmdIndex);

/* ---- the CAN task's half ------------------------------------------------ *
 * Drains the request queue, performs the sends, expires the arm gate and
 * services cyclic repeats. Called from the CAN reader task on every pass, and
 * from nowhere else. */
void txService(MCP2515 &can);

const char *txStatusText(uint8_t status);
