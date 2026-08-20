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
};
