#include "cantx.h"
#include "dbc.h"
#include "decode.h"
#include "logger.h"
#include "recorder.h"

#include <string.h>

TxState g_tx;

/* One queued request. Small and flat so it can go through a FreeRTOS queue by
 * value - nothing here points at anything that could be freed underneath it. */
struct TxRequest {
  uint32_t ticket;
  uint8_t  cmd;          /* index into g_dash.tx, or 0xFF for a raw frame */
  uint8_t  hold;         /* 1 = write into the frame but do not send it yet */
  float    value;
  uint32_t id;
  uint8_t  ext;
  uint8_t  len;
  uint8_t  data[8];
};

/* One frame under construction.
 *
 * Some messages only mean anything whole. On a multiplexed command frame the
 * selector and the payload are two signals that MUST leave together - writing
 * a wheel diameter without the opcode that says "this is a wheel diameter"
 * arrives as opcode zero and is discarded. The same is true, less obviously,
 * of any frame whose signals are read as a set.
 *
 * So a group of values is queued as several requests, each writing into this
 * buffer, and only the last one transmits. The queue is FIFO and drained by
 * one task, so the group cannot be split by anything else in between. */
struct TxPending {
  int16_t  msg;          /* index into g_dbc.msg, -1 = nothing in progress */
  uint8_t  len;
  uint8_t  data[8];
};
static TxPending s_pending = { -1, 0, {0, 0, 0, 0, 0, 0, 0, 0} };

static QueueHandle_t s_queue  = nullptr;
static uint32_t      s_ticket = 0;

#define TX_RAW_CMD 0xFF

const char *txStatusText(uint8_t status) {
  switch (status) {
    case TXS_OK:              return "sent and acknowledged";
    case TXS_NO_ACK:          return "nothing on the bus acknowledged it";
    case TXS_ARB_LOST:        return "the bus was too busy to get on";
    case TXS_ABORTED:         return "the controller gave up on it";
    case TXS_TIMEOUT:         return "the controller did not finish it";
    case TXS_BUSY:            return "a previous frame is still going out";
    case TXS_NOT_LISTENING:   return "listen-only mode: the logger cannot drive the bus";
    case TXS_BAD_FRAME:       return "the identifier or length is out of range";
    case TXS_NOT_ARMED:       return "the dashboard is not armed";
    case TXS_NO_SIGNAL:       return "this frame map has no such signal";
    case TXS_OUT_OF_RANGE:    return "the value does not fit the signal";
    case TXS_QUEUE_FULL:      return "too many sends at once";
    case TXS_UNKNOWN_COMMAND: return "no such setpoint";
    case TXS_PENDING:         return "queued";
  }
  return "unknown";
}

/* ==========================================================================
 *  Results
 * ======================================================================== */
static void record(const TxOutcome &o) {
  const uint8_t slot = (uint8_t)(g_tx.ringCount % TX_RESULT_RING);
  g_tx.ring[slot] = o;
  g_tx.ringCount++;                     /* published last: see the header */
}

/* A refusal that never reached the controller still has to be reported, or the
 * browser sees a Send that did nothing and no reason why. */
static uint32_t refuse(uint8_t cmd, float value, uint8_t status) {
  TxOutcome o;
  memset(&o, 0, sizeof(o));
  o.ticket    = ++s_ticket;
  o.ms        = millis();
  o.status    = status;
  o.cmd       = cmd;
  o.requested = value;
  record(o);

  LOG_FILE(LVL_WARN, "TX refused: %s", txStatusText(status));
  return 0;
}

/* ==========================================================================
 *  The arm gate
 * ======================================================================== */
bool txArmed() {
  if (!g_tx.armed) return false;
  /* Signed comparison so the millis() wrap at 49 days does not read as an
   * expiry that already happened. */
  return (int32_t)(g_tx.armUntilMs - millis()) > 0;
}

uint32_t txArmRemainingMs() {
  if (!g_tx.armed) return 0;
  const int32_t left = (int32_t)(g_tx.armUntilMs - millis());
  return left > 0 ? (uint32_t)left : 0;
}

void txArm(bool on, const char *who) {
  if (on) {
    const bool wasArmed = txArmed();
    g_tx.armed      = 1;
    g_tx.armUntilMs = millis() + TX_ARM_TIMEOUT_MS;
    if (!wasArmed) {
      /* LIVE, not FILE: this belongs on the operator's screen as well as in
       * the recording. Somebody standing next to the machine should be able to
       * see that the logger has been given permission to drive the bus. */
      LOG_LIVE(LVL_WARN, "TRANSMIT ARMED by %s - the dashboard can now write "
                         "to the bus for %lu s",
               who ? who : "the dashboard",
               (unsigned long)(TX_ARM_TIMEOUT_MS / 1000UL));
    }
  } else {
    if (g_tx.armed) {
      LOG_LIVE(LVL_INFO, "transmit disarmed by %s", who ? who : "the dashboard");
    }
    g_tx.armed    = 0;
    g_tx.cyclicOn = 0;          /* repeats never outlive the gate */
  }
}

/* ==========================================================================
 *  Requests
 * ======================================================================== */
static uint32_t enqueue(const TxRequest &r) {
  if (!s_queue) return refuse(r.cmd, r.value, TXS_QUEUE_FULL);
  if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
    return refuse(r.cmd, r.value, TXS_QUEUE_FULL);
  }
  return r.ticket;
}

uint32_t txSendCommand(uint8_t cmdIndex, float value) {
  return txSendPart(cmdIndex, value, false);
}

uint32_t txSendPart(uint8_t cmdIndex, float value, bool hold) {
  if (cmdIndex >= TX_MAX_COMMANDS || !txCommandUsed(g_dash.tx[cmdIndex])) {
    return refuse(cmdIndex, value, TXS_UNKNOWN_COMMAND);
  }
  if (!txArmed()) return refuse(cmdIndex, value, TXS_NOT_ARMED);

  const TxCommand &c = g_dash.tx[cmdIndex];
  if (c.kind == TXK_SIGNAL && c.sig < 0) {
    return refuse(cmdIndex, value, TXS_NO_SIGNAL);
  }

  /* Refuse rather than clamp. The slider already stops at the limits, so a
   * value outside them arrived by some other route - a stale bookmark, a
   * hand-written request - and quietly moving it somewhere else is how an
   * operator ends up believing they set something they did not. */
  if (c.kind == TXK_SIGNAL && c.hi > c.lo &&
      (value < c.lo - 1e-6f || value > c.hi + 1e-6f)) {
    return refuse(cmdIndex, value, TXS_OUT_OF_RANGE);
  }

  TxRequest r;
  memset(&r, 0, sizeof(r));
  r.ticket = ++s_ticket;
  r.cmd    = cmdIndex;
  r.value  = value;
  r.hold   = hold ? 1 : 0;
  return enqueue(r);
}

uint32_t txSendRaw(uint32_t id, bool ext, const uint8_t *data, uint8_t len) {
  if (!txArmed()) return refuse(TX_RAW_CMD, 0.0f, TXS_NOT_ARMED);
  if (len > 8)    return refuse(TX_RAW_CMD, 0.0f, TXS_BAD_FRAME);

  TxRequest r;
  memset(&r, 0, sizeof(r));
  r.ticket = ++s_ticket;
  r.cmd    = TX_RAW_CMD;
  r.id     = id;
  r.ext    = ext ? 1 : 0;
  r.len    = len;
  if (data && len) memcpy(r.data, data, len);
  return enqueue(r);
}

void txSetCyclic(uint8_t cmdIndex, bool on, float value) {
  if (cmdIndex >= TX_MAX_COMMANDS) return;
  const uint32_t bit = 1UL << cmdIndex;

  if (!on) {
    g_tx.cyclicOn &= ~bit;
    LOG_FILE(LVL_INFO, "cyclic send stopped: %s", g_dash.tx[cmdIndex].label);
    return;
  }
  if (!txCommandUsed(g_dash.tx[cmdIndex]) || !g_dash.tx[cmdIndex].cyclicMs) return;
  if (!txArmed()) { (void)refuse(cmdIndex, value, TXS_NOT_ARMED); return; }

  g_tx.cyclicValue[cmdIndex]  = value;
  g_tx.cyclicNextMs[cmdIndex] = millis();       /* first one goes out now */
  g_tx.cyclicOn |= bit;
  LOG_LIVE(LVL_WARN, "cyclic send started: %s every %u ms",
           g_dash.tx[cmdIndex].label, (unsigned)g_dash.tx[cmdIndex].cyclicMs);
}

bool txCyclicOn(uint8_t cmdIndex) {
  return cmdIndex < TX_MAX_COMMANDS && (g_tx.cyclicOn & (1UL << cmdIndex));
}

/* ==========================================================================
 *  Doing it - all of this runs in the CAN task
 * ======================================================================== */

/* Builds the payload for a setpoint.
 *
 * The message's OTHER signals are the interesting part. A command that sets
 * one signal must not command every other signal in the same message to zero
 * as a side effect, so the payload starts as the last thing the bus said that
 * message contained, and only the target signal's bits are changed. With
 * nothing ever seen for that identifier - a message this logger originates -
 * zeros are the only available starting point, and are correct. */
static bool buildSignalFrame(const TxCommand &c, float value, CanFrame &f,
                             uint8_t &status, float &applied, bool &clamped) {
  if (c.sig < 0 || c.msg < 0 || c.msg >= (int16_t)g_dbc.msgCount) {
    status = TXS_NO_SIGNAL;
    return false;
  }
  const DbcMessage &m  = g_dbc.msg[c.msg];
  const DbcSignal  &sg = g_dbc.sig[c.sig];

  memset(&f, 0, sizeof(f));
  f.id  = m.id;
  f.ext = m.ext;
  f.tx  = 1;
  f.len = m.dlc ? m.dlc : 8;

  if (s_pending.msg == c.msg) {
    /* Continuing a group: keep what the earlier members already wrote. */
    memcpy(f.data, s_pending.data, 8);
    if (s_pending.len > f.len) f.len = s_pending.len;
  } else if (m.muxSignal < 0) {
    /* Start from what the bus last said, so the other signals in the message
     * keep the values they already had rather than being zeroed. A MULTIPLEXED
     * message is the exception: its bytes mean different things on different
     * pages, so carrying a previous page's bytes forward would send garbage
     * under a new opcode. */
    uint8_t seenLen = 0;
    if (busLastPayload(g_bus, m.id, m.ext != 0, f.data, &seenLen)) {
      if (seenLen > f.len) f.len = seenLen;
    }
  }

  /* The selector, written from the signal's own mux code. This is not a
   * setting anyone should have to remember: the frame map already says that
   * WheelDia_mm is the payload of opcode 32, so opcode 32 is what goes out
   * with it. The code is a raw bit pattern by definition, so it is inserted
   * directly rather than pushed through the selector's scaling. */
  if (sg.muxValue >= 0 && m.muxSignal >= 0 &&
      (uint16_t)m.muxSignal < g_dbc.sigCount) {
    const DbcSignal &ms = g_dbc.sig[m.muxSignal];
    if (!dbcInsertBits(f.data, f.len, ms.startBit, ms.bits, ms.intel != 0,
                       (uint64_t)sg.muxValue)) {
      status = TXS_BAD_FRAME;
      return false;
    }
  }

  DbcEncoded e = dbcEncodeSignal(sg, (double)value, f.data, f.len);
  if (!e.ok) { status = TXS_BAD_FRAME; return false; }

  applied = (float)e.applied;
  clamped = e.clamped;
  return true;
}

static void perform(MCP2515 &can, const TxRequest &r) {
  TxOutcome o;
  memset(&o, 0, sizeof(o));
  o.ticket    = r.ticket;
  o.cmd       = r.cmd;
  o.requested = r.value;

  /* Checked here as well as at the door. A request can sit in the queue while
   * the gate expires, and a cyclic repeat comes through this path with no door
   * to be checked at. */
  if (!txArmed()) {
    o.status = TXS_NOT_ARMED;
    o.ms     = millis();
    s_pending.msg = -1;      /* an abandoned group leaves nothing behind */
    record(o);
    return;
  }

  CanFrame f;
  uint8_t  status  = TXS_OK;
  float    applied = r.value;
  bool     clamped = false;

  if (r.cmd == TX_RAW_CMD) {
    memset(&f, 0, sizeof(f));
    f.id  = r.id;
    f.ext = r.ext;
    f.len = r.len;
    f.tx  = 1;
    memcpy(f.data, r.data, 8);
  } else {
    const TxCommand &c = g_dash.tx[r.cmd];
    if (c.kind == TXK_RAW) {
      memset(&f, 0, sizeof(f));
      f.id  = c.id;
      f.ext = c.ext;
      f.len = c.len;
      f.tx  = 1;
      memcpy(f.data, c.data, 8);
    } else if (!buildSignalFrame(c, r.value, f, status, applied, clamped)) {
      o.status = status;
      o.ms     = millis();
      s_pending.msg = -1;
      record(o);
      g_tx.failed++;
      return;
    }

    /* Hold the frame for the next member of the group. Nothing goes on the
     * wire yet - a half-written command frame is worse than none. */
    if (r.hold && c.kind == TXK_SIGNAL) {
      s_pending.msg = c.msg;
      s_pending.len = f.len;
      memcpy(s_pending.data, f.data, 8);
      o.status  = TXS_PENDING;
      o.ms      = millis();
      o.id      = f.id;
      o.ext     = f.ext;
      o.len     = f.len;
      o.applied = applied;
      o.clamped = clamped ? 1 : 0;
      memcpy(o.data, f.data, 8);
      record(o);
      return;
    }
  }

  s_pending.msg = -1;          /* this frame is complete either way */

  int16_t tec = 0;
  const MCP2515::TxResult res = can.sendFrame(f, TX_ATTEMPTS, &tec);

  o.ms       = millis();
  o.status   = (uint8_t)res;            /* the first eight codes line up */
  o.id       = f.id;
  o.ext      = f.ext;
  o.len      = f.len;
  o.applied  = applied;
  o.clamped  = clamped ? 1 : 0;
  o.tecDelta = tec;
  memcpy(o.data, f.data, 8);
  record(o);

  if (res != MCP2515::TX_OK) {
    g_tx.failed++;
    LOG_LIVE(LVL_ERROR, "TX 0x%lX failed: %s",
             (unsigned long)f.id, txStatusText(o.status));
    return;
  }

  g_tx.sent++;

  /* Into the recording. The controller does not hear its own transmissions, so
   * without this the command would be missing from the very file it was sent
   * during. Timestamped now, which is when it went on the wire. */
  f.esp_us = (uint64_t)esp_timer_get_time();
  if (g_frameQueue) {
    if (xQueueSend(g_frameQueue, &f, 0) != pdTRUE) {
      LOG_FILE(LVL_WARN, "TX 0x%lX went out but did not fit in the recording",
               (unsigned long)f.id);
    }
  }

  if (r.cmd != TX_RAW_CMD && g_dash.tx[r.cmd].kind == TXK_SIGNAL) {
    LOG_FILE(LVL_INFO, "TX %s = %s%.4g %s (0x%lX)", g_dash.tx[r.cmd].label,
             clamped ? "clamped to " : "", (double)applied,
             g_dash.tx[r.cmd].unit, (unsigned long)f.id);
  } else {
    LOG_FILE(LVL_INFO, "TX raw 0x%lX, %u bytes",
             (unsigned long)f.id, (unsigned)f.len);
  }
}

void txBegin() {
  memset(&g_tx, 0, sizeof(g_tx));
  s_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(TxRequest));
  if (!s_queue) {
    LOG_LIVE(LVL_WARN, "no memory for the transmit queue - Send is unavailable");
  }
}

void txService(MCP2515 &can) {
  /* The gate expiring is not a quiet event: a cyclic setpoint stops when it
   * happens, and the operator needs to know why. */
  if (g_tx.armed && !txArmed()) {
    LOG_LIVE(LVL_INFO, "transmit disarmed itself after %lu s idle",
             (unsigned long)(TX_ARM_TIMEOUT_MS / 1000UL));
    g_tx.armed    = 0;
    g_tx.cyclicOn = 0;
  }

  if (s_queue) {
    TxRequest r;
    while (xQueueReceive(s_queue, &r, 0) == pdTRUE) perform(can, r);
  }

  if (!g_tx.cyclicOn) return;

  const uint32_t now = millis();
  for (uint8_t i = 0; i < TX_MAX_COMMANDS; i++) {
    if (!(g_tx.cyclicOn & (1UL << i))) continue;

    const TxCommand &c = g_dash.tx[i];
    if (!txCommandUsed(c) || !c.cyclicMs) { g_tx.cyclicOn &= ~(1UL << i); continue; }
    if ((int32_t)(now - g_tx.cyclicNextMs[i]) < 0) continue;

    /* Step the schedule from the deadline, not from now, so a period does not
     * drift outwards by however long this task took to get here. */
    g_tx.cyclicNextMs[i] += c.cyclicMs;
    if ((int32_t)(now - g_tx.cyclicNextMs[i]) > (int32_t)c.cyclicMs) {
      g_tx.cyclicNextMs[i] = now + c.cyclicMs;   /* we fell far behind; resync */
    }

    TxRequest r;
    memset(&r, 0, sizeof(r));
    r.ticket = ++s_ticket;
    r.cmd    = i;
    r.value  = g_tx.cyclicValue[i];
    perform(can, r);
  }
}
