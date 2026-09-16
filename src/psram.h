/* ============================================================================
 *  psram.h - where the big read-mostly tables live
 *
 *  THE ARITHMETIC THAT MAKES THIS NECESSARY
 *  ----------------------------------------
 *  A real vendor DBC is not a small file. Measured on this project's own
 *  hardware, a 1090-message / 8380-signal map asks for:
 *
 *      8380 signals  x 133 B (DbcSignal + the live slot)  = 1088 KB
 *      1090 messages x  48 B                              =   51 KB
 *                                                   total ~ 1154 KB
 *
 *  An ESP32 has 320 KB of internal DRAM in TOTAL, of which roughly 200 KB is
 *  ever available as heap and the Wi-Fi stack wants 50 KB of that in large
 *  contiguous blocks. So a map like that does not miss by a few kilobytes that
 *  a smaller queue or a tighter struct could recover - it misses by a factor
 *  of six. There is no arrangement of internal memory in which it fits, and
 *  shrinking DbcSignal to a third of its size would not change the answer.
 *
 *  On a module with PSRAM (WROVER and friends: 2, 4 or 8 MB of SPI RAM) it
 *  fits with room to spare, and it is the right place for it. The tables are
 *  written once at boot and then only read - a few thousand lookups a second,
 *  which the cache absorbs - while internal DRAM is the scarce thing that the
 *  radio, the task stacks and the SD buffer all compete for. Moving the map
 *  out of DRAM does not just make the map fit; it hands the whole DRAM budget
 *  back to everything else.
 *
 *  WITHOUT PSRAM
 *  -------------
 *  psCalloc() falls back to the internal heap and behaves exactly as calloc()
 *  always did, so a plain WROOM build is unchanged. It will still fit the map
 *  to the heap and report what it kept - which for a file this size means most
 *  of the bus is recorded as raw payload rather than decoded live. Nothing is
 *  lost from the recording either way; the frames are written whole and decode
 *  offline against the same DBC.
 *
 *  A build with no PSRAM compiled in resolves all of this at compile time -
 *  psramSize() is a constant 0 and the branch disappears.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include <stdlib.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

/* Bytes of PSRAM fitted, 0 when there is none or the core was built without
 * support for it. */
static inline size_t psramSize() {
#if defined(ARDUINO_ARCH_ESP32)
  return (size_t)ESP.getPsramSize();
#else
  return 0;
#endif
}

static inline size_t psramFree() {
#if defined(ARDUINO_ARCH_ESP32)
  return (size_t)ESP.getFreePsram();
#else
  return 0;
#endif
}

/* Zeroed block, from PSRAM when there is any and from the internal heap when
 * there is not. Release with psFree() - which is plain free(), because
 * ESP-IDF's allocator tracks the region a pointer came from itself. Falls back
 * to internal memory rather than failing if PSRAM is present but exhausted. */
static inline void *psCalloc(size_t n, size_t sz) {
#if defined(ARDUINO_ARCH_ESP32)
  if (psramSize()) {
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);
    if (p) return p;
  }
#endif
  return calloc(n, sz);
}

static inline void psFree(void *p) { free(p); }
