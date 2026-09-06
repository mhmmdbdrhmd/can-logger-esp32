#pragma once
#include "Arduino.h"
#define MSBFIRST 1
#define SPI_MODE0 0
#define VSPI 3
#define HSPI 2
struct SPISettings { SPISettings(uint32_t=0,uint8_t=0,uint8_t=0){} };
/* virtual so a test can subclass this and model a real device on the bus.
 * The driver brackets every operation in beginTransaction/endTransaction, so
 * those two are enough to frame a transaction without modelling the CS pin. */
class SPIClass {
public:
  SPIClass(int = 0) {}
  virtual ~SPIClass() {}
  void begin(int8_t=-1,int8_t=-1,int8_t=-1,int8_t=-1) {}
  virtual void beginTransaction(SPISettings) {}
  virtual void endTransaction() {}
  virtual uint8_t transfer(uint8_t) { return 0; }

  /* The driver clocks whole transactions as one block. Modelling that here as
   * a loop over the virtual transfer() means a test device only has to
   * implement the byte-level protocol it already implements - the block
   * transfer is a transport detail, not a behaviour change. */
  virtual void transferBytes(const uint8_t *tx, uint8_t *rx, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t v = transfer(tx ? tx[i] : 0x00);
      if (rx) rx[i] = v;
    }
  }
};
