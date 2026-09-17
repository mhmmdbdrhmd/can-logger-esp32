/* ============================================================================
 *  test_cantx.cpp - a Send that cannot get on the bus
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  TX_ATTEMPTS is three attempts back to back, inside one pass of the CAN
 *  task. Sensor traffic arrives in bursts, so all three land inside the same
 *  burst: on the bench, 602 of 1265 sends failed that way in five minutes,
 *  and none failed once the request was given back to the queue for a later
 *  pass. These pin that behaviour - that a lost arbitration is retried across
 *  passes, that it is reported after the last one, and that a real failure
 *  (nothing acknowledges the frame) is never retried, because retrying it
 *  would just block the receive path again.
 * ==========================================================================*/
#include <cstdio>
#include <cstring>
#include <string>
#include "Arduino.h"
#include "SPI.h"
#include "config.h"
#include "mcp2515.h"
#include "cantx.h"
#include "dash.h"
#include "dbc.h"
#include "decode.h"
#include "recorder.h"
#include "logger.h"

uint32_t     g_fakeMs = 0;
FakeSerial   Serial;
FakeEsp      ESP;
BusStats     g_bus[CAN_BUSES];
RecStatus    g_rec;
QueueHandle_t g_frameQueue = nullptr;

void logPost(LogLevel, bool, const char *, ...) {}
bool busLastPayload(const BusStats &, uint32_t, bool, uint8_t *, uint8_t *) { return false; }

static int fails = 0;

static std::string slurp(const char *path) {
  std::string t;
  FILE *f = fopen(path, "rb");
  if (!f) return t;
  int c;
  while ((c = fgetc(f)) != EOF) t += (char)c;
  fclose(f);
  return t;
}

static void ck(const char *what, bool ok, const std::string &detail = "") {
  printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what,
         detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) fails++;
}

/* A controller that answers every send the same way, and keeps what it was
   handed - the payload is the point of the group test below. */
struct FakeCan {
  MCP2515::TxResult answer = MCP2515::TX_ARB_LOST;
  int sends = 0;
  MCP2515::TxResult thenAnswer = MCP2515::TX_ARB_LOST;
  int switchAfter = -1;             /* answer thenAnswer from this send on */
  CanFrame last[8];
};
static FakeCan s_fake;

/* MCP2515 is a real class here; only the two calls cantx makes are needed, so
 * they are defined for this binary instead of linking the driver. */
MCP2515::TxResult MCP2515::sendFrame(const CanFrame &f, uint8_t, int16_t *tec) {
  if (s_fake.sends < 8) s_fake.last[s_fake.sends] = f;
  s_fake.sends++;
  if (tec) *tec = 0;
  if (s_fake.switchAfter >= 0 && s_fake.sends > s_fake.switchAfter)
    return s_fake.thenAnswer;
  return s_fake.answer;
}
bool MCP2515::canTransmit() { return true; }
MCP2515::MCP2515(SPIClass &spi, int8_t csPin, uint32_t spiHz)
  : _spi(spi), _cs(csPin), _cfg(spiHz, MSBFIRST, SPI_MODE0) {}
void MCP2515::serviceBusy() {}
void MCP2515::setBusyHook(BusyHook, void *) {}

int main() {
  SPIClass spi;
  MCP2515 can(spi, 5, 10000000);

  txBegin();
  dashReset(g_dash);
  txArm(true, "test");

  printf("\n== a send that keeps losing arbitration is retried on later passes ==\n");
  {
    /* The shipped default. A build may set it to 0 deliberately, but the suite
     * runs on the default, and 0 there would make everything below vacuous. */
    ck("the default gives a send at least one later pass", TX_RETRY_PASSES >= 1,
       "TX_RETRY_PASSES=" + std::to_string(TX_RETRY_PASSES));
    const uint8_t data[1] = { 0x00 };
    s_fake.answer = MCP2515::TX_ARB_LOST;
    s_fake.sends  = 0;
    g_tx.sent = g_tx.failed = 0;

    txSendRaw(0, 0x7F0, false, data, 1);

    /* One pass sends once: the attempts inside sendFrame() are its own. */
    for (int pass = 0; pass <= TX_RETRY_PASSES; pass++) {
      const int before = s_fake.sends;
      txService(can, 0);
      ck("tried once more on this pass", s_fake.sends == before + 1,
         "pass " + std::to_string(pass) + ", sends " + std::to_string(s_fake.sends));
      const bool last = (pass == TX_RETRY_PASSES);
      ck(last ? "and reported as failed after the last pass"
              : "and not reported failed yet",
         (g_tx.failed == 1) == last,
         "failed=" + std::to_string(g_tx.failed));
    }

    const int after = s_fake.sends;
    txService(can, 0);
    ck("then it is gone, not retried for ever", s_fake.sends == after);
    ck("and it was never counted as sent", g_tx.sent == 0);
  }

  printf("\n== a bus nobody answers is NOT retried ==\n");
  {
    const uint8_t data[1] = { 0x00 };
    s_fake.answer = MCP2515::TX_NO_ACK;
    s_fake.sends  = 0;
    g_tx.sent = g_tx.failed = 0;

    txSendRaw(0, 0x7F0, false, data, 1);
    txService(can, 0);
    ck("reported the first time", g_tx.failed == 1 && s_fake.sends == 1);
    txService(can, 0);
    ck("and not tried again", s_fake.sends == 1,
       "sends=" + std::to_string(s_fake.sends));
  }

  printf("\n== a send that gets on the bus is not retried either ==\n");
  {
    const uint8_t data[1] = { 0x00 };
    s_fake.answer = MCP2515::TX_OK;
    s_fake.sends  = 0;
    g_tx.sent = g_tx.failed = 0;

    txSendRaw(0, 0x7F0, false, data, 1);
    txService(can, 0);
    txService(can, 0);
    ck("sent once, counted once", s_fake.sends == 1 && g_tx.sent == 1,
       "sends=" + std::to_string(s_fake.sends));
  }

  printf("\n== a retry sends the frame that was built, not a rebuilt one ==\n");
  {
    /* The case that makes this matter: two values that share one frame. The
       first is HELD, the second completes the frame and sends it. A retry that
       rebuilds finds the held value gone - the frame goes out with the second
       value and a zeroed first one, which on a multiplexed command is a valid
       frame carrying wrong numbers. */
    DbcDb &db = g_dbc[0];
    std::string dbc = slurp("examples/machine.dbc");
    std::string cfg = slurp("examples/dash.cfg");
    dbcLoadText(db, dbc.c_str(), dbc.size());
    dashReset(g_dash);
    dashParse(g_dash, cfg.c_str(), cfg.size());
    DbcDb maps[CAN_BUSES];
    for (int i = 0; i < CAN_BUSES; i++) maps[i] = db;
    dashResolve(g_dash, maps);
    txArm(true, "test");

    /* Send 0 is TyreSize, send 1 is SpeedLimit - both in MachineConfig. */
    ck("the example layout has the two values this needs",
       txCommandUsed(g_dash.tx[0]) && txCommandUsed(g_dash.tx[1]));

    s_fake.answer      = MCP2515::TX_ARB_LOST;   /* lose the first pass  */
    s_fake.thenAnswer  = MCP2515::TX_OK;         /* win the second       */
    s_fake.switchAfter = 1;
    s_fake.sends       = 0;
    g_tx.sent = g_tx.failed = 0;
    const uint8_t ring0 = g_tx.ringCount;

    txSendPart(0, 690.0f, true);      /* held: writes into the frame  */
    txSendPart(1, 8.0f,  false);      /* completes it and sends       */
    txService(can, 0);                /* pass 1: lost arbitration     */
    txService(can, 0);                /* pass 2: goes out             */

    ck("it was sent on the second pass", s_fake.sends == 2 && g_tx.sent == 1,
       "sends=" + std::to_string(s_fake.sends));
    bool same = true;
    for (int i = 0; i < 8; i++) if (s_fake.last[0].data[i] != s_fake.last[1].data[i]) same = false;
    char a[32], b[32];
    snprintf(a, sizeof(a), "%02X%02X%02X%02X%02X%02X%02X%02X",
             s_fake.last[0].data[0], s_fake.last[0].data[1], s_fake.last[0].data[2],
             s_fake.last[0].data[3], s_fake.last[0].data[4], s_fake.last[0].data[5],
             s_fake.last[0].data[6], s_fake.last[0].data[7]);
    snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X%02X%02X",
             s_fake.last[1].data[0], s_fake.last[1].data[1], s_fake.last[1].data[2],
             s_fake.last[1].data[3], s_fake.last[1].data[4], s_fake.last[1].data[5],
             s_fake.last[1].data[6], s_fake.last[1].data[7]);
    ck("and the retry carried the SAME payload, held value and all", same,
       std::string("first ") + a + ", retry " + b);
    ck("the held value is actually in there (not a pair of zeroes)",
       s_fake.last[1].data[0] != 0 || s_fake.last[1].data[1] != 0,
       std::string("payload ") + b);

    /* One outcome for one ticket. The dashboard keeps the first it sees, so a
       recorded loss on the retried pass reports a Send that went out as
       failed. */
    int forTicket = 0, failedRecords = 0;
    for (int i = 0; i < TX_RESULT_RING; i++) {
      const TxOutcome &o = g_tx.ring[i];
      if (o.ticket == 0 || o.status == TXS_PENDING) continue;
      if (o.cmd == 1) {
        forTicket++;
        if (o.status != TXS_OK) failedRecords++;
      }
    }
    ck("exactly one outcome was recorded for it, and it says OK",
       forTicket == 1 && failedRecords == 0,
       "records=" + std::to_string(forTicket) +
       " failures=" + std::to_string(failedRecords));
    (void)ring0;
  }

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
