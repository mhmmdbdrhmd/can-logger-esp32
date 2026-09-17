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

static void ck(const char *what, bool ok, const std::string &detail = "") {
  printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what,
         detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) fails++;
}

/* A controller that answers every send the same way. */
struct FakeCan {
  MCP2515::TxResult answer = MCP2515::TX_ARB_LOST;
  int sends = 0;
};
static FakeCan s_fake;

/* MCP2515 is a real class here; only the two calls cantx makes are needed, so
 * they are defined for this binary instead of linking the driver. */
MCP2515::TxResult MCP2515::sendFrame(const CanFrame &, uint8_t, int16_t *tec) {
  s_fake.sends++;
  if (tec) *tec = 0;
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

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
