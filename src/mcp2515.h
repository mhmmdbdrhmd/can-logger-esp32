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
  uint8_t  ext;       /* 1 = 29-bit identifier                               */
  uint8_t  rtr;
  uint8_t  data[8];
};

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
   * controller itself dropped a frame because we were too slow. */
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

private:
  SPIClass  &_spi;
  int8_t     _cs;
  SPISettings _cfg;

  inline void select()   { digitalWrite(_cs, LOW);  }
  inline void deselect() { digitalWrite(_cs, HIGH); }

  void    reset();
  uint8_t readReg(uint8_t addr);
  void    readRegs(uint8_t addr, uint8_t *buf, uint8_t n);
  void    writeReg(uint8_t addr, uint8_t val);
  void    modifyReg(uint8_t addr, uint8_t mask, uint8_t val);
  uint8_t readStatus();
  bool    setMode(uint8_t mode);
  bool    setBitrate(uint16_t kbps, uint8_t crystalMHz);
};
