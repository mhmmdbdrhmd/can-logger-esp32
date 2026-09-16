#include "mem.h"
#include "logger.h"
#include "dbc.h"      /* name_max - named in the survival line below */
#include <esp_heap_caps.h>

/* WHY NOT ESP.getFreeHeap() / ESP.getMaxAllocHeap().
 *
 * Both ask for MALLOC_CAP_INTERNAL, which means "internal RAM" and nothing
 * about how that RAM may be addressed. On the ESP32 the IRAM the application
 * did not use is handed to the heap as 32-BIT-ACCESS-ONLY memory: readable and
 * writable a word at a time, never a byte at a time. malloc(), realloc() and
 * every String, std::vector and lwIP pbuf go through MALLOC_CAP_8BIT and can
 * NEVER be served out of it.
 *
 * So getMaxAllocHeap() happily reports a block that no allocation this
 * firmware makes can use. It is not a small discrepancy. Measured on this
 * board across the whole experiment matrix:
 *
 *     map        total free   getMaxAllocHeap()
 *     none            95100               47092
 *     v0 (8 KB)       85460               47092
 *     v1 (16 KB)      77184               47092
 *     v2 (48 KB)      71852               47092
 *     v5 (1.2 MB)     71228               47092
 *
 * The total moves by 24 KB as the frame maps grow. The "largest block" does
 * not move at all, because it is a fixed lump of leftover IRAM and the maps
 * come out of DRAM. That constant is what the last two sessions read as
 * "smallest block ever 45 KB, so memory is not the problem" - while
 * /api/status was failing to reserve 16384 bytes at that very moment.
 *
 * MALLOC_CAP_8BIT is the pool allocations actually come from, so it is the
 * only one worth printing. */

/* Lowest `largest` seen since boot. Starts at "not yet sampled" so the first
 * sample sets it rather than being compared against a guess. */
static uint32_t s_lowBlock = 0xFFFFFFFFUL;

/* Which thresholds have already been reported. One bit each, so each is said
 * once and the log does not fill with the same sentence. */
static uint8_t  s_said     = 0;
#define SAID_WARN 0x01
#define SAID_CRIT 0x02

/* The largest block malloc() could actually return, right now. */
uint32_t memLargestBlock() {
  return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

MemStat memStat() {
  MemStat m;
  m.freeNow  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
  m.largest  = memLargestBlock();
  m.minFree  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  m.lowBlock = (s_lowBlock == 0xFFFFFFFFUL) ? m.largest : s_lowBlock;
  return m;
}

uint32_t memSample() {
  const uint32_t block = memLargestBlock();
  if (block < s_lowBlock) s_lowBlock = block;

  /* Said when the block size first drops past each mark, not while it stays
   * there.
   *
   * The marks themselves are derived in config.h, and the derivation there
   * replaced an earlier one that claimed "the radio would not start below
   * ~47 KB". That was never a measurement: 47092 was the fixed lump of
   * 32-bit-only IRAM the old gauge reported, unchanged whatever the firmware
   * did, and a warning set just above it fired on every healthy boot. */
  if (block < MEM_CRIT_BLOCK && !(s_said & SAID_CRIT)) {
    s_said |= SAID_CRIT | SAID_WARN;
    LOG_LIVE(LVL_ERROR,
             "heap is FRAGMENTED: largest block down to %lu bytes (%lu total "
             "free). Allocations this size fail one at a time - the page stops "
             "rendering and the JSON endpoints start answering 503 while the "
             "total still looks healthy. Shrink the frame maps; see "
             "DBC_HEAP_RESERVE in config.h.",
             (unsigned long)block,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  } else if (block < MEM_WARN_BLOCK && !(s_said & SAID_WARN)) {
    s_said |= SAID_WARN;
    LOG_LIVE(LVL_WARN,
             "largest free block is down to %lu bytes (%lu total free) - the "
             "block size is what decides an allocation, not the total",
             (unsigned long)block,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  }
  return block;
}

/* The boot checkpoints are taken before the recording .log exists - the file
 * is not opened until the card is up and a recording starts, some three
 * seconds in. Every "heap at ..." line therefore went to the serial console
 * and nowhere else, and on an unattended run with nothing capturing serial it
 * was simply lost. Four runs' worth of the one measurement that predicts the
 * web UI went missing exactly that way.
 *
 * So each checkpoint is latched here as it is taken, and replayed into the log
 * once there is a log to replay it into. Four lines of a fixed size; no
 * allocation, because this module exists to watch allocation. */
#define MEM_BOOT_MARKS 6
static char     s_mark[MEM_BOOT_MARKS][28];
static MemStat  s_markStat[MEM_BOOT_MARKS];
static uint8_t  s_markN = 0;

void memLog(uint8_t level, bool live, const char *where) {
  memSample();
  const MemStat m = memStat();
  if (s_markN < MEM_BOOT_MARKS) {
    snprintf(s_mark[s_markN], sizeof(s_mark[0]), "%s", where ? where : "?");
    s_markStat[s_markN] = m;
    s_markN++;
  }
  logPost((LogLevel)level, live,
          "heap at %s: free=%lu largest=%lu lowestBlock=%lu minFree=%lu",
          where ? where : "?",
          (unsigned long)m.freeNow, (unsigned long)m.largest,
          (unsigned long)m.lowBlock, (unsigned long)m.minFree);
}

/* ---- the failed-allocation trap ---------------------------------------- *
 *
 * The hook can run in ISR context and must not allocate, block or log. So it
 * does the only safe thing: bumps counters. Everything that reads them runs in
 * the 1 Hz health line, where logging is already safe.
 *
 * `volatile` rather than an atomic: these are diagnostic counters, a lost
 * increment across a core boundary costs nothing, and the hook has to stay as
 * close to free as possible because it fires exactly when the system is
 * already in trouble. */
static volatile uint32_t s_allocFails   = 0;
static volatile uint32_t s_allocFailLast = 0;
static volatile uint32_t s_allocFailMax  = 0;
static volatile uint32_t s_allocFailCaps = 0;

/* The name of the allocator that asked, kept as the POINTER the hook was
 * handed rather than a copy. It is a string literal in flash owned by the
 * caller, so it outlives us and costs four bytes; copying it here would mean
 * a strncpy inside a hook that can run in an ISR. Read it only from task
 * context, where a torn update does not matter because the next failure will
 * print the same name anyway. */
static const char *volatile s_allocFailFn = nullptr;

static void IRAM_ATTR memAllocFailHook(size_t size, uint32_t caps,
                                       const char *fn) {
  s_allocFails++;
  s_allocFailLast = (uint32_t)size;
  s_allocFailCaps = caps;
  s_allocFailFn   = fn;
  if ((uint32_t)size > s_allocFailMax) s_allocFailMax = (uint32_t)size;
}

void memTrapAllocFailures() {
  const esp_err_t e = heap_caps_register_failed_alloc_callback(memAllocFailHook);
  if (e != ESP_OK) {
    LOG_FILE(LVL_WARN, "could not install the failed-allocation hook (%d) - "
                       "allocFail counters will stay at zero", (int)e);
  }
}

uint32_t memAllocFailures()  { return s_allocFails; }
uint32_t memAllocFailLast()  { return s_allocFailLast; }
uint32_t memAllocFailMax()   { return s_allocFailMax; }
uint32_t memAllocFailCaps()  { return s_allocFailCaps; }
const char *memAllocFailFn() {
  const char *f = s_allocFailFn;
  return f ? f : "?";
}

void memReplayBoot() {
  if (!s_markN) return;
  LOG_FILE(LVL_INFO, "heap ladder at boot (taken before this file existed):");
  for (uint8_t i = 0; i < s_markN; i++) {
    LOG_FILE(LVL_INFO, "  %-34s free=%lu largest=%lu",
             s_mark[i],
             (unsigned long)s_markStat[i].freeNow,
             (unsigned long)s_markStat[i].largest);
  }
}

/* The block the board actually came up with, latched at the end of setup.
 *
 * Kept rather than re-sampled, because the number the verdict is ABOUT is the
 * one at ready: later in the run the block moves with traffic, and answering
 * /api/websurvival with a mid-recording sample would have the dashboard
 * contradict the line in the boot log for the same board and the same map. */
static uint32_t s_readyBlock = 0;

uint32_t memReadyBlock() { return s_readyBlock; }

void memWebVerdict() {
  const uint32_t block = memSample();
  s_readyBlock = block;

  /* Three answers, because fifteen measured runs give three. A two-way verdict
   * was tried twice and was wrong both times - see MEM_WEB_SERVES in config.h.
   *
   * None of this touches the recording. Every run on record captured every
   * frame, including the ones where the dashboard never answered a request, so
   * the middle and lower cases are warnings about what you can WATCH and must
   * not read as data loss. */
  if (block >= MEM_WEB_SERVES) {
    LOG_LIVE(LVL_INFO,
             "web UI check: largest block %lu bytes - above %lu, where every "
             "measured run served every request. The dashboard will work.",
             (unsigned long)block, (unsigned long)MEM_WEB_SERVES);
    return;
  }

  if (block > MEM_WEB_DEAD) {
    LOG_LIVE(LVL_WARN,
             "web UI check: largest block %lu bytes, between %lu and %lu. Runs "
             "measured in this band served anywhere from 58%% to 99%% of their "
             "requests, and which one you get is not predictable from the map. "
             "Expect a dashboard that mostly works and intermittently stalls. "
             "The recording is unaffected and will not lose a frame.",
             (unsigned long)block, (unsigned long)MEM_WEB_DEAD,
             (unsigned long)MEM_WEB_SERVES);
    return;
  }

  LOG_LIVE(LVL_WARN,
           "web UI check: largest block %lu bytes, at or below %lu, where every "
           "measured run served under 10%% of its requests. The page and the "
           "JSON endpoints will stop answering within the first minute. The "
           "recording is unaffected and will not lose a frame - shrink the "
           "frame maps, or export the setup with name_max 32, if you want the dashboard "
           "during this run.",
           (unsigned long)block, (unsigned long)MEM_WEB_DEAD);
}
