#pragma once
#include "Arduino.h"
#include "FS.h"
#include "SPI.h"
#include <map>
#include <string>
#define FILE_WRITE "w"
#define FILE_READ  "r"
enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC };

/* A CARD THAT ACTUALLY HOLDS FILES.
 *
 * This was a stub: exists() answered false, open() handed back an empty File,
 * and nothing could be read back. That is fine for code which only writes, and
 * useless for code which reads - so src/bundle.cpp, whose whole job is to read
 * one file off the card and write three, could not be tested at all. It was
 * shipped on the strength of "unpacked 3 file(s)" appearing on a serial
 * console, which proves it counted three headers and nothing about whether the
 * bytes were right.
 *
 * The map is empty to begin with, so exists() still answers false until
 * something is put there and every test that relied on the stub behaves as it
 * did. SDFiles::put() seeds a card; SDFiles::get() reads one back.
 */
struct SDFiles {
  static std::map<std::string, std::string> &all() {
    static std::map<std::string, std::string> m;
    return m;
  }
  static void clear() { all().clear(); }
  static void put(const char *p, const std::string &s) { all()[p] = s; }
  static bool has(const char *p) { return all().count(p) != 0; }
  static std::string get(const char *p) {
    auto it = all().find(p);
    return it == all().end() ? std::string() : it->second;
  }
};

struct FakeSD {
  bool begin(uint8_t, SPIClass &, uint32_t) { return true; }
  void end() {}
  int cardType() { return CARD_SDHC; }
  uint64_t cardSize() { return 16ULL*1024*1024*1024; }

  bool exists(const char *p) { return SDFiles::has(p); }
  bool remove(const char *p) { return SDFiles::all().erase(p) > 0; }

  bool rename(const char *from, const char *to) {
    auto it = SDFiles::all().find(from);
    if (it == SDFiles::all().end()) return false;
    SDFiles::all()[to] = it->second;
    SDFiles::all().erase(it);
    return true;
  }

  File open(const char *p, const char *mode) {
    File f;
    if (mode && mode[0] == 'w') {
      /* Writing: hand back a File whose sink is the stored string, so what the
       * firmware writes is what a later open() reads. */
      SDFiles::all()[p] = std::string();
      f.sink = &SDFiles::all()[p];
      return f;
    }
    if (!SDFiles::has(p)) return File();      /* missing: a false-y File */
    f.src = SDFiles::get(p);
    return f;
  }
};
extern FakeSD SD;
