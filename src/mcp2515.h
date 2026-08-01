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

  /* Hard-resets the controller, programs the bit timing and opens the bus.
   * Returns false if the chip does not answer (wiring / power problem). */
  bool begin(uint16_t bitrateKbps, uint8_t crystalMHz, bool listenOnly);

  /* True while at least one of the two receive buffers holds a frame. */
  bool framePending();

  /* Pops one frame. Returns false when both buffers are empty. */
  bool readFrame(CanFrame &out);

  /* EFLG receive-overflow flags; reading clears them. Non-zero means the
   * controller itself dropped a frame because we were too slow. */
  uint8_t takeRxOverflow();

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
