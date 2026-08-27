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
#include <string>
#include <vector>

#include "Arduino.h"
#include "SPI.h"
#include "mcp2515.h"

static int failures = 0;

/* Register values read better as hex when an assertion prints one back. */
static const char *hex(uint8_t v) {
  static char buf[8];
  snprintf(buf, sizeof(buf), "0x%02X", v);
  return buf;
}
static void ck(const char *what, bool ok, const char *note = "") {
  printf(ok ? "  ok   %s %s\n" : "  FAIL %s %s\n", what, note);
  if (!ok) failures++;
}

/* ---- registers ---------------------------------------------------------- */
#define R_TEC      0x1C
#define R_TXB0CTRL 0x30
#define R_TXB0SIDH 0x31
#define R_CANSTAT  0x0E
#define R_CANCTRL  0x0F
#define R_CNF1     0x2A
#define R_CANINTE  0x2B
#define R_RXB0CTRL 0x60
#define R_RXB1CTRL 0x70
#define R_CANINTF  0x2C
#define R_EFLG     0x2D

#define F_RX0      0x01
#define F_RX1      0x02
#define F_ERR      0x20
#define F_MERR     0x80

#define T_ABTF     0x40
#define T_MLOA     0x20
#define T_TXERR    0x10
#define T_TXREQ    0x08

/* ---- a fake MCP2515 on the SPI bus -------------------------------------- */
class FakeMcp : public SPIClass {
public:
  uint8_t reg[128];
  std::deque<std::vector<uint8_t>> rx0;   /* queued frames for RXB0 */

  /* ---- what the simulated bus does with a transmission ---- */
  enum TxSim {
    SIM_ACCEPT,        /* another node acknowledges it                       */
    SIM_NOACK,         /* nobody is out there - the ACK slot stays empty     */
    SIM_LOSE_ARB_ONCE, /* a higher priority frame wins the first attempt     */
    SIM_LOSE_ARB_ALWAYS,
    SIM_WEDGE          /* the controller never clears TXREQ                  */
  };
  TxSim   txSim      = SIM_ACCEPT;
  int     txAttempts = 0;
  std::vector<uint8_t> lastTx;            /* the 13 bytes handed to TXB0 */

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
      if (cmd == 0x81) { requestToSend(); return 0; }        /* RTS TXB0 */
      if (cmd == 0x40) { addr = R_TXB0SIDH; return 0; }      /* LOAD TX BUFFER */
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

      case 0x40:                                            /* LOAD TX BUFFER */
        applyWrite(addr++, b);
        return 0;

      case 0xA0:                                            /* READ STATUS */
        return (uint8_t)(reg[R_CANINTF] & (F_RX0 | F_RX1));

      case 0x90: case 0x94:
        return (oi < out.size()) ? out[oi++] : 0;
    }
    return 0;
  }

private:
  /* A request-to-send sets TXREQ in hardware and then the bus decides. The
   * driver polls TXB0CTRL afterwards, so resolving it here immediately is
   * indistinguishable from a real controller that finished quickly. */
  void requestToSend() {
    txAttempts++;
    lastTx.assign(&reg[R_TXB0SIDH], &reg[R_TXB0SIDH] + 13);
    reg[R_TXB0CTRL] |= T_TXREQ;

    switch (txSim) {
      case SIM_ACCEPT:
        reg[R_TXB0CTRL] &= (uint8_t)~T_TXREQ;
        if (reg[R_TEC]) reg[R_TEC]--;         /* a good frame lowers TEC */
        break;
      case SIM_NOACK:
        reg[R_TXB0CTRL] &= (uint8_t)~T_TXREQ;
        reg[R_TXB0CTRL] |= T_TXERR;
        reg[R_TEC] = (uint8_t)(reg[R_TEC] + 8 > 255 ? 255 : reg[R_TEC] + 8);
        break;
      case SIM_LOSE_ARB_ONCE:
        reg[R_TXB0CTRL] &= (uint8_t)~T_TXREQ;
        if (txAttempts == 1) reg[R_TXB0CTRL] |= T_MLOA;
        else if (reg[R_TEC]) reg[R_TEC]--;
        break;
      case SIM_LOSE_ARB_ALWAYS:
        reg[R_TXB0CTRL] &= (uint8_t)~T_TXREQ;
        reg[R_TXB0CTRL] |= T_MLOA;
        break;
      case SIM_WEDGE:
        break;                                 /* TXREQ stays set */
    }
  }

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

  /* The receive configuration, pinned.
   *
   * This exact setup - filters off on both buffers, rollover on - is the one
   * that recorded hours of a 540 frame/s bus without losing a frame. Two bits
   * carry that:
   *
   *   RXM = 11  accept everything. A logger that filters is a logger that
   *             lies about what was on the bus.
   *   BUKT      let a full RXB0 roll into RXB1. Without it, every frame
   *             targets RXB0 (because RXM = 11 means every frame passes its
   *             filter), RXB1 is never used at all, and the controller holds
   *             ONE frame instead of two - halving the time available to
   *             service an interrupt before something is dropped.
   *
   * Neither bit changes anything visible until the bus gets busy, which is why
   * they are asserted here rather than left to be noticed in the field. */
  ck("RXB0: filters off and rollover into RXB1 enabled",
     (fake.reg[R_RXB0CTRL] & 0x64) == 0x64,
     hex(fake.reg[R_RXB0CTRL]));
  ck("RXB1: filters off", (fake.reg[R_RXB1CTRL] & 0x60) == 0x60,
     hex(fake.reg[R_RXB1CTRL]));

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

  printf("\n== one-shot mode is armed before the bus opens ==\n");
  /* Without this the controller retries an unacknowledged frame forever, and
   * eight TEC per attempt reaches bus-off in about 30 ms - which would take
   * the RECEIVE path down as collateral damage from a Send button. */
  ck("CANCTRL has one-shot set", (fake.reg[R_CANCTRL] & 0x08) != 0);

  printf("\n== a frame that is acknowledged ==\n");
  {
    fake.txSim = FakeMcp::SIM_ACCEPT;
    fake.txAttempts = 0;
    fake.reg[R_TEC] = 10;

    CanFrame t;
    memset(&t, 0, sizeof(t));
    t.id = 0x18C; t.len = 3;
    t.data[0] = 0xDE; t.data[1] = 0xAD; t.data[2] = 0xBE;

    int16_t dtec = 0;
    const MCP2515::TxResult r = can.sendFrame(t, 3, &dtec);
    ck("sendFrame reports success", r == MCP2515::TX_OK);
    ck("it took one attempt", fake.txAttempts == 1);
    ck("TEC fell, which is the proof it was acknowledged", dtec == -1);

    /* The bytes on the wire have to be the frame that was asked for, laid out
     * the way readFrame() parses one - the two are inverses. */
    ck("identifier is in SIDH/SIDL",
       fake.lastTx[0] == 0x31 && (fake.lastTx[1] & 0xE0) == 0x80);
    ck("standard frames do not set the extended bit",
       (fake.lastTx[1] & 0x08) == 0);
    ck("length and payload are loaded",
       fake.lastTx[4] == 3 && fake.lastTx[5] == 0xDE &&
       fake.lastTx[6] == 0xAD && fake.lastTx[7] == 0xBE);
    ck("bytes past the length are zeroed, not left over",
       fake.lastTx[8] == 0 && fake.lastTx[12] == 0);
  }

  printf("\n== an extended identifier ==\n");
  {
    fake.txSim = FakeMcp::SIM_ACCEPT;
    CanFrame t;
    memset(&t, 0, sizeof(t));
    t.id = 0x18DAF110; t.ext = 1; t.len = 1; t.data[0] = 0x42;
    ck("sendFrame accepts it", can.sendFrame(t) == MCP2515::TX_OK);
    ck("the extended bit is set in SIDL", (fake.lastTx[1] & 0x08) != 0);

    /* Reassemble the identifier the way readFrame() does and require it back. */
    const uint32_t back = ((uint32_t)fake.lastTx[0] << 21) |
                          ((uint32_t)(fake.lastTx[1] & 0xE0) << 13) |
                          ((uint32_t)(fake.lastTx[1] & 0x03) << 16) |
                          ((uint32_t)fake.lastTx[2] << 8) | fake.lastTx[3];
    ck("the identifier survives the round trip", back == 0x18DAF110,
       back == 0x18DAF110 ? "" : "<-- SIDH/SIDL/EID packing is wrong");
  }

  printf("\n== nothing on the bus acknowledges ==\n");
  {
    fake.txSim = FakeMcp::SIM_NOACK;
    fake.txAttempts = 0;
    fake.reg[R_TEC] = 0;

    CanFrame t;
    memset(&t, 0, sizeof(t));
    t.id = 0x100; t.len = 1;

    int16_t dtec = 0;
    const MCP2515::TxResult r = can.sendFrame(t, 3, &dtec);
    ck("reported as unacknowledged, not as success", r == MCP2515::TX_NO_ACK);
    ck("it was not retried into bus-off", fake.txAttempts == 1,
       ("attempts=" + std::to_string(fake.txAttempts)).c_str());
    ck("TEC rose by exactly one failed attempt", dtec == 8,
       ("dtec=" + std::to_string(dtec)).c_str());
    ck("the controller is still far from bus-off", fake.reg[R_TEC] < 128,
       ("TEC=" + std::to_string(fake.reg[R_TEC])).c_str());
  }

  printf("\n== losing arbitration is retried, not reported ==\n");
  {
    fake.txSim = FakeMcp::SIM_LOSE_ARB_ONCE;
    fake.txAttempts = 0;
    fake.reg[R_TEC] = 5;

    CanFrame t;
    memset(&t, 0, sizeof(t));
    t.id = 0x7FF; t.len = 0;
    ck("a busy bus does not fail the send", can.sendFrame(t, 3) == MCP2515::TX_OK);
    ck("it took a second attempt", fake.txAttempts == 2,
       ("attempts=" + std::to_string(fake.txAttempts)).c_str());

    fake.txSim = FakeMcp::SIM_LOSE_ARB_ALWAYS;
    fake.txAttempts = 0;
    ck("but it gives up eventually", can.sendFrame(t, 3) == MCP2515::TX_ARB_LOST);
    ck("after exactly the attempts it was given", fake.txAttempts == 3,
       ("attempts=" + std::to_string(fake.txAttempts)).c_str());
  }

  printf("\n== a wedged controller does not block every later send ==\n");
  {
    fake.txSim = FakeMcp::SIM_WEDGE;
    fake.reg[R_TXB0CTRL] = 0;

    CanFrame t;
    memset(&t, 0, sizeof(t));
    t.id = 0x200; t.len = 1;
    ck("the send times out", can.sendFrame(t, 1) == MCP2515::TX_TIMEOUT);

    /* If TXREQ were left set, every future send would come back TX_BUSY and
     * the Send button would be dead until the next power cycle. */
    ck("TXREQ was cleared on the way out",
       (fake.reg[R_TXB0CTRL] & T_TXREQ) == 0);

    fake.txSim = FakeMcp::SIM_ACCEPT;
    ck("the next send works", can.sendFrame(t, 1) == MCP2515::TX_OK);
  }

  printf("\n== a frame the driver will not send ==\n");
  {
    CanFrame t;
    memset(&t, 0, sizeof(t));

    t.id = 0x800; t.len = 1;             /* 0x800 needs 12 bits */
    ck("an 11-bit identifier that does not fit is refused",
       can.sendFrame(t) == MCP2515::TX_BAD_FRAME);

    t.id = 0x100; t.len = 9;
    ck("a length above 8 is refused", can.sendFrame(t) == MCP2515::TX_BAD_FRAME);

    t.id = 0x20000000; t.ext = 1; t.len = 0;
    ck("a 29-bit identifier that does not fit is refused",
       can.sendFrame(t) == MCP2515::TX_BAD_FRAME);
  }

  printf("\n== listen-only refuses to drive the bus ==\n");
  {
    /* The safe configuration on a live machine, and the one where a Send that
     * silently did nothing would be worst: the operator would conclude the
     * ECU ignored the command. */
    ck("the bus can be reopened listen-only", can.startReceiving(true));
    ck("one-shot survives the mode change", (fake.reg[R_CANCTRL] & 0x08) != 0);

    fake.txSim = FakeMcp::SIM_ACCEPT;
    fake.txAttempts = 0;

    CanFrame t;
    memset(&t, 0, sizeof(t));
    t.id = 0x100; t.len = 1;
    ck("sendFrame refuses", can.sendFrame(t) == MCP2515::TX_NOT_LISTENING);
    ck("and nothing reached the controller", fake.txAttempts == 0);

    ck("back to normal mode", can.startReceiving(false));
    ck("and sending works again", can.sendFrame(t) == MCP2515::TX_OK);
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
