#pragma once
#include "Arduino.h"
#include "FS.h"
#include "SPI.h"
#define FILE_WRITE "w"
#define FILE_READ  "r"
enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC };
struct FakeSD {
  bool begin(uint8_t, SPIClass &, uint32_t) { return true; }
  void end() {}
  int cardType() { return CARD_SDHC; }
  uint64_t cardSize() { return 16ULL*1024*1024*1024; }
  bool exists(const char *) { return false; }
  bool remove(const char *) { return true; }
  bool rename(const char *, const char *) { return true; }
  File open(const char *, const char *) { return File(); }
};
extern FakeSD SD;
