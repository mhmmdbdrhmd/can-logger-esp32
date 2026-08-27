/* ============================================================================
 *  webpage.h - the dashboard page itself
 *
 *  Split out of webui.cpp when the page grew a layout editor: that file is
 *  about handlers and JSON, this one is about HTML, and mixing forty kilobytes
 *  of markup into the request handling made both harder to read.
 *
 *  The page is served as several PROGMEM chunks rather than one literal, for
 *  two reasons: a raw string that long is unreadable, and sending it in pieces
 *  means the whole thing is never copied into RAM - it goes from flash to the
 *  socket a chunk at a time.
 *
 *  Still one page with no external assets. It has to load with no internet,
 *  from a hotspot with no route anywhere, which rules out every CDN and so
 *  every charting and gauge library. The widgets are hand-drawn SVG for that
 *  reason and not out of principle.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

extern const char *const PAGE_PARTS[];
extern const uint8_t     PAGE_PART_COUNT;
