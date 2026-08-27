#pragma once
#include "Arduino.h"
class __FlashStringHelper;
/* In-memory File, so the log and CSV paths can actually be exercised and a DBC
 * can be fed through the same readLine() the firmware uses. */
class File {
public:
  std::string *sink = nullptr;   /* write target */
  std::string  src;              /* read source  */
  size_t       pos = 0;

  explicit operator bool() const { return sink != nullptr || !src.empty(); }
  size_t write(const uint8_t *b, size_t n) { if (sink) sink->append((const char *)b, n); return n; }
  void print(const char *s) { if (sink) sink->append(s); }
  void print(char c)        { if (sink) sink->push_back(c); }
  void print(const class __FlashStringHelper *s) { if (sink) sink->append((const char*)s); }
  void flush() {}
  bool available() { return pos < src.size(); }
  int  read() { return (pos < src.size()) ? (unsigned char)src[pos++] : -1; }
  String readStringUntil(char t) {
    std::string out;
    while (pos < src.size() && src[pos] != t) out.push_back(src[pos++]);
    if (pos < src.size()) pos++;
    return String(out);
  }
  /* The real Arduino File has this; the DBC loader reads the file twice - once
   * to count, once to parse - so the shim needs it too. */
  bool   seek(size_t p) { if (p > src.size()) return false; pos = p; return true; }
  size_t position() const { return pos; }
  size_t size() const { return src.size(); }
  void close() { sink = nullptr; pos = src.size(); }
};
