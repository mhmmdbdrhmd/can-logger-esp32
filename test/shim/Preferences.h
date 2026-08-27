#pragma once
#include "Arduino.h"
#include <map>
#include <vector>
/* In-memory NVS, so dashstore.cpp type-checks and the store/load round trip
 * can be exercised on the host. Real NVS is key/value in flash; this is the
 * same shape with none of the wear. */
class Preferences {
public:
  bool begin(const char *ns, bool = false) { _ns = ns; return true; }
  void end() {}
  bool clear() { _blob.clear(); _u32.clear(); return true; }

  size_t getBytesLength(const char *k) {
    auto it = _blob.find(k);
    return it == _blob.end() ? 0 : it->second.size();
  }
  size_t getBytes(const char *k, void *out, size_t cap) {
    auto it = _blob.find(k);
    if (it == _blob.end()) return 0;
    const size_t n = it->second.size() < cap ? it->second.size() : cap;
    memcpy(out, it->second.data(), n);
    return n;
  }
  size_t putBytes(const char *k, const void *v, size_t n) {
    const uint8_t *p = (const uint8_t *)v;
    _blob[k] = std::vector<uint8_t>(p, p + n);
    return n;
  }
  uint32_t getUInt(const char *k, uint32_t dflt = 0) {
    auto it = _u32.find(k);
    return it == _u32.end() ? dflt : it->second;
  }
  size_t putUInt(const char *k, uint32_t v) { _u32[k] = v; return 4; }
  bool remove(const char *k) { _blob.erase(k); _u32.erase(k); return true; }

private:
  std::string _ns;
  std::map<std::string, std::vector<uint8_t>> _blob;
  std::map<std::string, uint32_t> _u32;
};
