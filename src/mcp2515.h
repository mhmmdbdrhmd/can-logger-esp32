/* ============================================================================
 *  mcp2515.h - minimal, interrupt-friendly MCP2515 driver
 *
 *  Deliberately hand-rolled instead of pulling in a library:
 *    - the receive path is a single 13-byte SPI burst (READ RX BUFFER), which
 *      auto-clears the interrupt flag; no read-modify-write round trips,
 *    - nothing allocates, nothing blocks, so it is safe to call from a
 *      high-priority FreeRTOS task woken directly by the INT pin,
 *    - the error counters and overflow flags are exposed, so the logger can
 *      prove it did not silently drop frames.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include <SPI.h>

struct CanFrame {
  uint64_t esp_us;    /* host timestamp captured in the INT service routine  */
  uint32_t id;
  uint8_t  len;

  /* The flags are bitfields rather than four bytes because the struct had no
   * padding left to spend. 8 + 4 + 4 + 8 was exactly 24 bytes, and a plain
   * `uint8_t bus` would have aligned it up to 32 - a third more RAM for a
   * queue that is thousands of entries deep. Packed, `bus` is free, and every
   * call site is unchanged because these only ever hold 0 or 1 anyway. */
  uint8_t  ext : 1;   /* 1 = 29-bit identifier                               */
  uint8_t  rtr : 1;

  /* 1 = this logger sent it, rather than receiving it. An MCP2515 does not
   * hear its own transmissions, so a frame the dashboard sent would otherwise
   * be missing from the recording it was sent during - and a setpoint whose
   * effect you can see but whose cause you cannot is worse than useless. */
  uint8_t  tx  : 1;

  /* Which controller it came from: 0 = CAN1, 1 = CAN2. Zero-based here and
   * one-based in the CSV, because the wire and the wiring diagram both count
   * from one and a log nobody can read against the diagram is no use. */
  uint8_t  bus : 1;

  uint8_t  data[8];
};

/* The queue holds thousands of these, so the size is load-bearing rather than
 * incidental. Asserted here so that adding a field is a compile error and not
 * a silent 30 % increase in the frame queue. */
static_assert(sizeof(CanFrame) == 24, "CanFrame must stay 24 bytes");

/* The bit rates and crystals this driver holds timings for.
 *
 * Exposed rather than kept private because a bit-rate search has to walk
 * exactly the pairs setBitrate() will accept: a candidate the driver rejects
 * is a candidate silently skipped, and a bus running at that rate would then
 * be reported as undetectable. Ordered most-likely-first, so a search spends
 * its first windows where the answer usually is. Kept beside the timing table
 * in mcp2515.cpp, and asserted against it in test/test_mcp2515.cpp. */
extern const uint16_t MCP_RATES[];
extern const uint8_t  MCP_RATE_COUNT;
extern const uint8_t  MCP_CRYSTALS[];
extern const uint8_t  MCP_CRYSTAL_COUNT;

class MCP2515 {
public:
  MCP2515(SPIClass &spi, int8_t csPin, uint32_t spiHz);

  /* Hard-resets the controller and programs the bit timing, but deliberately
   * LEAVES IT IN CONFIGURATION MODE - it does not touch the bus and receives
   * nothing yet. Returns false if the chip does not answer (wiring / power).
   *
   * Separated from startReceiving() because the controller holds only two
   * frames: on a busy bus it overflows a couple of milliseconds after it
   * starts listening. If reception began here, every boot would lose frames in
   * the gap before the reader task and the interrupt exist. */
  bool begin(uint16_t bitrateKbps, uint8_t crystalMHz);

  /* Opens the bus. Call this LAST, once the reader task is running and the
   * interrupt is attached, so the very first frame is already being watched
   * for. */
  bool startReceiving(bool listenOnly);

  /* True while at least one of the two receive buffers holds a frame. */
  bool framePending();

  /* Pops one frame. Returns false when both buffers are empty. */
  bool readFrame(CanFrame &out);

  /* EFLG receive-overflow flags; reading clears them. Non-zero means the
   * controller itself dropped a frame because we were too slow.
   *
   * The result contains ONLY the two receive-buffer overflow bits, one per
   * buffer, so the number of set bits is the number of buffers that
   * overflowed - and therefore a floor on how many frames were lost. It is a
   * floor and not a count: each bit is sticky and says "at least once since
   * you last cleared me", never how many times. */
  uint8_t takeRxOverflow();

  /* Raw CANINTF. Bit 0/1 are the receive buffers, bit 5 ERRIF, bit 7 MERRF. */
  uint8_t interruptFlags();

  /* Clears everything in CANINTF EXCEPT the two receive-buffer flags, and
   * returns what was cleared.
   *
   * This is not optional housekeeping. The INT pin is LEVEL active-low: it
   * stays asserted until every flag enabled in CANINTE is clear. Reading a
   * receive buffer auto-clears its own flag, but ERRIF and MERRF are sticky.
   * The moment one of them latches - a single receive overflow is enough - INT
   * never rises again, so a FALLING-edge interrupt never fires again either,
   * and reception silently degrades to whatever the fallback poll manages
   * (~100 frames/s). Call this on every service pass. */
  uint8_t clearErrorInterrupts();

  uint8_t txErrorCount();
  uint8_t rxErrorCount();
  uint8_t errorFlags();

  /* CANSTAT >> 5: 0=normal 1=sleep 2=loopback 3=listen-only 4=config */
  uint8_t mode();

  /* -------------------------------------------------------------------------
   *  TRANSMIT
   *
   *  The controller is put in ONE-SHOT mode, which is not the obvious choice
   *  and is the important one. Left to itself an MCP2515 retries a frame that
   *  nobody acknowledges forever, and each failed attempt adds 8 to the
   *  transmit error counter: at 250 kbit/s a frame sent to an ECU that is not
   *  there drives TEC from 0 to 255 in about 30 ms and the controller goes
   *  BUS-OFF - which stops it receiving too. A logger that goes deaf because
   *  someone pressed Send on a disconnected bus is a worse logger than one
   *  that cannot send at all.
   *
   *  One-shot attempts the frame exactly once, so TEC moves by 8 and the
   *  failure is reported instead of escalating. Arbitration lost is retried in
   *  software a bounded number of times, because on a busy bus losing
   *  arbitration is normal and is not a failure.
   * ---------------------------------------------------------------------- */
  enum TxResult : uint8_t {
    TX_OK = 0,        /* transmitted and acknowledged by at least one node   */
    TX_NO_ACK,        /* nothing on the bus acknowledged it                  */
    TX_ARB_LOST,      /* still losing arbitration after every attempt        */
    TX_ABORTED,       /* the controller gave up on it                        */
    TX_TIMEOUT,       /* TXREQ never cleared - the controller is wedged      */
    TX_BUSY,          /* the transmit buffer still holds an earlier frame    */
    TX_NOT_LISTENING, /* listen-only or configuration mode: cannot drive     */
    TX_BAD_FRAME      /* identifier or length out of range                   */
  };

  /* Sends one frame and waits for the controller to finish with it. Blocks for
   * at most a few milliseconds - it is called from the CAN task, which is the
   * only task allowed to touch this chip.
   *
   * `tecDelta`, when given, receives the change in the transmit error counter,
   * which is the evidence behind a TX_NO_ACK: an unacknowledged frame moves it
   * by 8, a successful one by -1. */
  TxResult sendFrame(const CanFrame &f, uint8_t attempts = 3,
                     int16_t *tecDelta = nullptr);

  /* True when the controller is in a mode that can drive the bus at all. */
  bool canTransmit();

private:
  SPIClass  &_spi;
  int8_t     _cs;
  SPISettings _cfg;

  inline void select()   { digitalWrite(_cs, LOW);  }
  inline void deselect() { digitalWrite(_cs, HIGH); }

  /* One SPI transaction, clocked as a single block. Every register access and
   * both frame paths go through this - see the comment in mcp2515.cpp for why
   * a byte-at-a-time version does not meet the deadline with two controllers
   * sharing the bus. `rx` must be as long as `tx`. */
  void    xfer(const uint8_t *tx, uint8_t *rx, size_t n);

  void    reset();
  void    abortTx();
  uint8_t readReg(uint8_t addr);
  void    writeReg(uint8_t addr, uint8_t val);
  void    modifyReg(uint8_t addr, uint8_t mask, uint8_t val);
  uint8_t readStatus();
  bool    setMode(uint8_t mode);
  bool    setBitrate(uint16_t kbps, uint8_t crystalMHz);
};
