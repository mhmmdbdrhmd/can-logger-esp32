#pragma once
#include "Arduino.h"
#define MSBFIRST 1
#define SPI_MODE0 0
#define VSPI 3
#define HSPI 2
struct SPISettings { SPISettings(uint32_t=0,uint8_t=0,uint8_t=0){} };
class SPIClass {
public:
  SPIClass(int = 0) {}
  void begin(int8_t=-1,int8_t=-1,int8_t=-1,int8_t=-1) {}
  void beginTransaction(SPISettings) {}
  void endTransaction() {}
  uint8_t transfer(uint8_t) { return 0; }
};
