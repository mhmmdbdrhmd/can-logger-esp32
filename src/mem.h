/* ============================================================================
 *  mem.h - the free-memory watch
 *
 *  WHY THIS EXISTS, AND WHY "FREE HEAP" ON ITS OWN IS A MISLEADING NUMBER
 *  ---------------------------------------------------------------------
 *  This firmware has already been taken down twice by memory, and neither time
 *  did the total free heap predict it:
 *
 *    - esp_wifi_init() returned ESP_ERR_NO_MEM (257) with 80136 bytes free.
 *      It wants a few LARGE CONTIGUOUS blocks and could not get one. Total
 *      free was not short; the heap was in pieces.
 *
 *      The "largest block was 47092" recorded against this at the time is NOT
 *      a usable figure and no threshold should be derived from it: it came
 *      from ESP.getMaxAllocHeap(), which answers for MALLOC_CAP_INTERNAL and
 *      so counts 32-bit-only IRAM that malloc() can never return. It read
 *      47092 whatever the firmware did. The failure was real; the number
 *      attached to it was the gauge, not the heap.
 *
 *    - handleStatus() built a 15 KB reply into a String reserved at 4 KB.
 *      Arduino's String reallocs to the exact size on every append past the
 *      reservation, so that is on the order of a thousand copies five times a
 *      second. Total free barely moved - the heap simply stopped having a
 *      block big enough to serve the 168 KB page.
 *
 *  So three numbers are tracked, not one:
 *
 *      free      total bytes available, summed over every free block
 *      largest   the biggest SINGLE block that can still be handed out. This
 *                is what actually decides whether an allocation succeeds, and
 *                it is the number that was missing both times.
 *      lowest    the smallest `largest` seen since boot - a high-water mark
 *                for pressure, so a transient squeeze that has since recovered
 *                is still visible after the fact.
 *
 *  memSample() is cheap (two heap_caps calls) and is meant to be called from
 *  the web loop, which is where the pressure comes from. It logs once when the
 *  block size first crosses each threshold downward, and then stays quiet -
 *  a warning that repeats every 20 ms is not a warning.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "config.h"

struct MemStat {
  uint32_t freeNow;   /* total free internal heap now                      */
  uint32_t largest;   /* largest single block obtainable now               */
  uint32_t minFree;   /* lowest total free since boot (the SDK's own mark) */
  uint32_t lowBlock;  /* lowest `largest` this module has observed         */
};

MemStat memStat();

/* Samples and updates the low-water mark. Call it wherever memory is spent -
 * the web loop, the status tick. Returns the block size just observed. */
/* The largest block malloc() could actually return (MALLOC_CAP_8BIT).
 * NOT ESP.getMaxAllocHeap(), which counts 32-bit-only IRAM no byte
 * allocation can ever use - see the note at the top of mem.cpp. */
uint32_t memLargestBlock();

uint32_t memSample();

/* One line: "heap at <where>: free=... largest=... lowest=... minFree=...".
 * `live` sends it to the serial console and the web terminal as well as the
 * file, which is what you want at a boot stage and not what you want in a
 * loop. */
void memLog(uint8_t level, bool live, const char *where);

/* The pre-flight check.
 *
 * Call once at the end of setup, when the map is loaded and the server is up.
 * Compares the largest free block against MEM_WEB_SERVES and says plainly
 * whether the dashboard will survive this map - while there is still time to
 * put a smaller one on the card, rather than after a forty-minute recording
 * whose web column is empty.
 *
 * Also latches the four boot checkpoints, which are taken before the .log file
 * exists and were otherwise lost to the serial console. memReplayBoot() writes
 * them into the recording log once it opens. */
void memWebVerdict();

/* The largest free block at the moment memWebVerdict() ran - the number the
 * verdict is about. 0 before setup finishes. Served to the dashboard so the
 * page's own badge and the boot log cannot disagree. */
uint32_t memReadyBlock();

/* Replays the boot checkpoints into the file log. Call just after the .log is
 * opened - see the note in memWebVerdict(). */
void memReplayBoot();

/* ---- the failed-allocation trap ---------------------------------------- *
 *
 * Every web failure on record carried the same note from the host probe:
 *
 *     timed out after 8011 ms [board REFUSED tcp/80]
 *
 * The board was not answering slowly, it was not accepting the connection at
 * all - while an already-running request finished in 30 ms. And it was not
 * starved: core 1 sat at 38-44% idle and appLoop() ran at 241-271/s in the
 * runs that served 5% of their requests, exactly as in the ones that served
 * 100%. So the accept path is failing while the application is healthy, which
 * leaves one candidate: lwIP cannot allocate the PCB or pbuf for an inbound
 * SYN, drops the packet, and the client's retransmission ladder (1+2+4 s) runs
 * past the 8 s timeout.
 *
 * That is a guess until it is measured. esp_heap_caps.h can call us on EVERY
 * failed allocation with the size and the caps requested, which turns it into
 * a fact: if the drops are allocation failures, the counter moves in the same
 * second the host records a refusal.
 *
 * memAllocFailures() returns how many have happened; memAllocFailLast()
 * returns the size of the most recent one. Both are cheap reads of a counter
 * the hook maintains. The hook itself does nothing but increment - it can be
 * called from an ISR, and it must not allocate or block. */
void     memTrapAllocFailures();
uint32_t memAllocFailures();
uint32_t memAllocFailLast();
uint32_t memAllocFailMax();
uint32_t memAllocFailCaps();
/* Which allocator was refused. A string literal owned by the caller - never
 * freed, safe to hold as a pointer, and the one field that says whether the
 * 2308-byte refusals come from the Wi-Fi driver, from lwIP, or from us. */
const char *memAllocFailFn();
