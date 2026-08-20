/* ============================================================================
 *  test_mcp2515.cpp - drive the real driver against a simulated controller
 *
 *  The MCP2515's INT pin is LEVEL active-low: it stays asserted until every
 *  flag enabled in CANINTE has been cleared in CANINTF. Two of those flags -
 *  ERRIF and MERRF - are sticky and are NOT cleared by reading a receive
 *  buffer. If nothing ever clears them, INT never rises again, a FALLING-edge
 *  interrupt never fires again, and reception silently collapses onto whatever
 *  the fallback poll manages.
 *
 *  That is not a hypothetical: on the ABS recorder this firmware grew out of,
 *  the identical omission capped a 1000 frame/s bus at 100 frames/s - 50 poll
 *  wake-ups per second times the two receive buffers - and it presented as
 *  "the CAN interrupt is not firing" with no other symptom.
 *
 *  So the register file below models the flag behaviour, and the test asserts
 *  the thing that actually matters: after an error flag latches, can the driver
 *  get INT back?
 * ==========================================================================*/

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "Arduino.h"
#include "SPI.h"
#include "mcp2515.h"

static int failures = 0;
static void ck(const char *what, bool ok, const char *note = "") {
  printf(ok ? "  ok   %s %s\n" : "  FAIL %s %s\n", what, note);
  if (!ok) failures++;
}

/* ---- registers ---------------------------------------------------------- */
#define R_CANSTAT  0x0E
#define R_CANCTRL  0x0F
#define R_CNF1     0x2A
#define R_CANINTE  0x2B
#define R_CANINTF  0x2C
#define R_EFLG     0x2D

#define F_RX0      0x01
#define F_RX1      0x02
#define F_ERR      0x20
#define F_MERR     0x80

/* ---- a fake MCP2515 on the SPI bus -------------------------------------- */
class FakeMcp : public SPIClass {
public:
  uint8_t reg[128];
  std::deque<std::vector<uint8_t>> rx0;   /* queued frames for RXB0 */

  FakeMcp() { reset(); }

  void reset() {
    memset(reg, 0, sizeof(reg));
    reg[R_CANCTRL] = 0x87;                /* config mode after reset */
    reg[R_CANSTAT] = 0x80;
  }

  /* The physical INT pin: low (asserted) while any ENABLED flag is set. */
  bool intAsserted() const { return (reg[R_CANINTF] & reg[R_CANINTE]) != 0; }

  void queueFrame(uint32_t id, const uint8_t *d, uint8_t len) {
    std::vector<uint8_t> b(13, 0);
    b[0] = (uint8_t)(id >> 3);
    b[1] = (uint8_t)((id & 7) << 5);
    b[4] = len;
    for (uint8_t i = 0; i < len && i < 8; i++) b[5 + i] = d[i];
    rx0.push_back(b);
    reg[R_CANINTF] |= F_RX0;
  }

  /* ---- SPI protocol ---- */
  void beginTransaction(SPISettings) override { n = 0; cmd = 0; addr = 0; }
  void endTransaction() override {}

  uint8_t transfer(uint8_t b) override {
    if (n++ == 0) {                       /* first byte is the command */
      cmd = b;
      if (cmd == 0xC0) { reset(); return 0; }              /* RESET */
      /* SPI is full duplex: the answer to a command comes on the NEXT
       * transfer, while the command byte itself is still being clocked in.
       * Returning it here would be a simulator that no real chip matches. */
      if (cmd == 0x90 || cmd == 0x94) {                     /* READ RX BUFFER */
        out.clear();
        if (cmd == 0x90 && !rx0.empty()) {
          out.assign(rx0.front().begin(), rx0.front().end());
          rx0.pop_front();
          /* Hardware clears the flag as a side effect of this read. */
          if (rx0.empty()) reg[R_CANINTF] &= (uint8_t)~F_RX0;
        } else {
          out.assign(13, 0);
        }
        oi = 0;
        return 0;
      }
      return 0;
    }

    switch (cmd) {
      case 0x03:                                            /* READ */
        if (n == 2) { addr = b; return 0; }
        return reg[addr++ & 0x7F];

      case 0x02:                                            /* WRITE */
        if (n == 2) { addr = b; return 0; }
        applyWrite(addr++, b);
        return 0;

      case 0x05:                                            /* BIT MODIFY */
        if (n == 2) { addr = b; return 0; }
        if (n == 3) { mask = b; return 0; }
        applyWrite(addr, (uint8_t)((reg[addr] & ~mask) | (b & mask)));
        return 0;

      case 0xA0:                                            /* READ STATUS */
        return (uint8_t)(reg[R_CANINTF] & (F_RX0 | F_RX1));

      case 0x90: case 0x94:
        return (oi < out.size()) ? out[oi++] : 0;
    }
    return 0;
  }

private:
  void applyWrite(uint8_t a, uint8_t v) {
    a &= 0x7F;
    reg[a] = v;
    /* CANCTRL's top three bits select the mode; CANSTAT echoes them back. */
    if (a == R_CANCTRL) reg[R_CANSTAT] = (uint8_t)(v & 0xE0);
  }

  size_t  n = 0, oi = 0;
  uint8_t cmd = 0, addr = 0, mask = 0;
  std::vector<uint8_t> out;
};

int main() {
  FakeMcp fake;
  MCP2515 can(fake, 5, 10000000UL);

  printf("\n== bring-up ==\n");
  const bool up = can.begin(250, 8);
  ck("begin() succeeds against a responding chip", up);
  ck("ERRIF and MERRF are enabled in CANINTE",
     (fake.reg[R_CANINTE] & (F_ERR | F_MERR)) != 0);

  /* The bus must still be shut: begin() may not receive anything before the
   * reader task and the ISR exist, or every boot loses the frames that arrive
   * in the gap. */
  ck("begin() leaves the controller in CONFIG mode (bus not yet open)",
     (fake.reg[R_CANSTAT] & 0xE0) == 0x80);
  ck("startReceiving() opens the bus", can.startReceiving(false) &&
     (fake.reg[R_CANSTAT] & 0xE0) == 0x00);

  printf("\n== a received frame releases INT ==\n");
  const uint8_t payload[8] = {1,2,3,4,5,6,7,8};
  fake.queueFrame(0x123, payload, 8);
  ck("INT asserted once a frame is waiting", fake.intAsserted());

  CanFrame f;
  const bool got = can.readFrame(f);
  ck("readFrame() returns the frame", got && f.id == 0x123 && f.len == 8);
  ck("INT released after the buffer is read", !fake.intAsserted());

  printf("\n== the sticky error flag ==\n");
  /* A bus error, a message error or a receive overflow all latch here. One is
   * enough, and on a busy bus it is a matter of seconds. */
  fake.reg[R_CANINTF] |= F_ERR;
  fake.reg[R_EFLG]    |= 0x40;            /* RX0OVR, as an overflow would */
  ck("INT asserted by the error flag", fake.intAsserted());

  /* Exactly what the CAN task does on every pass: drain, take overflow, then
   * clear the sticky flags. */
  while (can.readFrame(f)) { }
  const uint8_t ovf = can.takeRxOverflow();
  const uint8_t sticky = can.clearErrorInterrupts();
  ck("clearErrorInterrupts() reports what it cleared", (sticky & F_ERR) != 0);
  ck("takeRxOverflow() reports and clears EFLG", ovf != 0 &&
     (fake.reg[R_EFLG] & 0x40) == 0);

  const bool stillLow = fake.intAsserted();
  ck("INT is released after a full service pass", !stillLow,
     stillLow ? "<-- ERRIF still set: INT can never fall again, so the "
                "edge-triggered ISR is dead from here on"
              : "");

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
