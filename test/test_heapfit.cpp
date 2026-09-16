/* ============================================================================
 *  The frame-map heap budget, against the figures a WROOM board really reports.
 *
 *  WHY THIS FILE EXISTS.
 *
 *  DBC_HEAP_RESERVE was tuned against ESP.getFreeHeap(), which counts ~41 KB
 *  of 32-bit-only IRAM that no map table can ever be allocated from. When the
 *  gauge was corrected to MALLOC_CAP_8BIT the real free heap turned out to be
 *  76696 bytes, not 124508 - and the reserve, left at 90000, made the budget
 *  NEGATIVE. The proportional fit-down then scaled by zero and every message
 *  on both buses disappeared:
 *
 *      W frame map wants 18 KB but only 0 KB of internal heap is free
 *      W CAN2: /frames2.dbc has no BO_ messages
 *
 *  The dashboard came up with the right panels and no values in any of them.
 *
 *  Nothing caught it, because the shim's getFreeHeap() answered 200000 and
 *  `pio run` only proves it compiles. So this drives the real arithmetic with
 *  the real counted files at the real heap figures, and the numbers below are
 *  measurements off the board, not guesses:
 *
 *      free after Wi-Fi, before the maps    76696   <- the budget is taken here
 *      free once the server is up           54108   <- what is left at ready
 *
 *  Both MALLOC_CAP_8BIT, both from a boot log with no map loaded.
 * ==========================================================================*/
#include "dbc.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void ok(bool cond, const char *what, const char *detail = "") {
  printf("  %-4s %-52s %s\n", cond ? "ok" : "FAIL", what, detail);
  if (!cond) failures++;
}

/* The heap this board really has when loadOneDbc() takes its budget. */
static const size_t HEAP_AT_MAP_LOAD = 76696;

/* Count a real file the way the loader's first pass does. */
static bool countFile(const char *path, DbcCounts &c) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  c = DbcCounts{0, 0, 0};
  char line[DBC_LINE_MAX];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    dbcCountLine(line, c);
  }
  fclose(f);
  /* The same slack the loader adds before fitting. */
  if (c.messages < 0xFF00) c.messages = (uint16_t)(c.messages + 4);
  if (c.signals  < 0xFF00) c.signals  = (uint16_t)(c.signals  + 8);
  if (c.values   < 0xFF00) c.values   = (uint16_t)(c.values   + 8);
  return true;
}

static size_t bytesFor(const DbcCounts &c) {
  return (size_t)c.messages * dbcBytesPerMessage()
       + (size_t)c.signals  * dbcBytesPerSignal()
       + (size_t)c.values   * dbcBytesPerValue();
}

int main(int argc, char **argv) {
  /* Anchored to this source file, not to the working directory. run_tests.sh
   * can be invoked from anywhere, and a test that silently reads no files and
   * passes is worse than no test - this one would have reported "could not
   * read the map files" instead, but only if it got the path right. */
  static char dirbuf[1024];
  if (argc > 1) {
    snprintf(dirbuf, sizeof(dirbuf), "%s", argv[1]);
  } else {
    snprintf(dirbuf, sizeof(dirbuf), "%s", __FILE__);
    char *slash = strrchr(dirbuf, '/');
    if (slash) *slash = 0; else snprintf(dirbuf, sizeof(dirbuf), ".");
    strncat(dirbuf, "/../examples", sizeof(dirbuf) - strlen(dirbuf) - 1);
  }
  const char *dir = dirbuf;
  char can1[512], can2[512];
  snprintf(can1, sizeof(can1), "%s/machine.dbc", dir);
  snprintf(can2, sizeof(can2), "%s/example.dbc", dir);

  printf("== the budget must not be zero at this board's real heap ==\n");
  {
    DbcCounts c{100, 500, 200};
    const DbcFit fit = dbcFitToHeap(c, HEAP_AT_MAP_LOAD, DBC_HEAP_RESERVE, false);
    char d[128];
    snprintf(d, sizeof(d), "heap %zu - reserve %lu",
             HEAP_AT_MAP_LOAD, (unsigned long)DBC_HEAP_RESERVE);
    ok(fit != DBC_FIT_NONE, "DBC_HEAP_RESERVE leaves room to load something", d);
    ok(DBC_HEAP_RESERVE < HEAP_AT_MAP_LOAD,
       "DBC_HEAP_RESERVE is smaller than the free heap");
  }

  printf("\n== the two example maps both fit whole, one after the other ==\n");
  {
    DbcCounts m1, m2;
    if (!countFile(can1, m1) || !countFile(can2, m2)) {
      ok(false, "could not read the map files", dir);
      printf("\n%s\n", failures ? "TESTS FAILED" : "ALL PASSED");
      return failures ? 1 : 0;
    }

    /* CAN1 is loaded first and takes its bytes off the heap CAN2 then sees.
     * Testing them independently would miss exactly that interaction. */
    DbcCounts a = m1;
    const DbcFit f1 = dbcFitToHeap(a, HEAP_AT_MAP_LOAD, DBC_HEAP_RESERVE, false);
    char d1[160];
    snprintf(d1, sizeof(d1), "%u msg %u sig, %zu B",
             m1.messages, m1.signals, bytesFor(m1));
    ok(f1 == DBC_FIT_ALL && a.messages == m1.messages,
       "machine.dbc (CAN1) keeps every message", d1);

    const size_t afterCan1 = HEAP_AT_MAP_LOAD - bytesFor(m1);
    DbcCounts b = m2;
    const DbcFit f2 = dbcFitToHeap(b, afterCan1, DBC_HEAP_RESERVE, false);
    char d2[160];
    snprintf(d2, sizeof(d2), "%u msg %u sig, %zu B, heap now %zu",
             m2.messages, m2.signals, bytesFor(m2), afterCan1);
    ok(f2 == DBC_FIT_ALL && b.messages == m2.messages,
       "example.dbc (CAN2) keeps every message after CAN1", d2);
  }

  printf("\n== a reserve larger than the heap is NONE, never a fit-down ==\n");
  {
    DbcCounts c{100, 500, 200};
    const DbcFit fit = dbcFitToHeap(c, 76696, 90000, false);
    ok(fit == DBC_FIT_NONE, "reserve 90000 vs heap 76696 reports DBC_FIT_NONE");
    ok(c.messages == 0 && c.signals == 0 && c.values == 0,
       "and zeroes the request rather than leaving a stale count");
  }

  printf("\n== a genuinely oversized file still fits down, not out ==\n");
  {
    DbcCounts c{1104, 8472, 2199};           /* a real vendor-size map */
    const DbcCounts asked = c;
    const DbcFit fit = dbcFitToHeap(c, HEAP_AT_MAP_LOAD, DBC_HEAP_RESERVE, false);
    ok(fit == DBC_FIT_PART, "a vendor-size map is scaled down");
    ok(c.messages > 0 && c.signals > 0,       "and keeps a usable part of it");
    ok(c.messages < asked.messages,           "which is smaller than it asked for");
    ok(bytesFor(c) <= HEAP_AT_MAP_LOAD - DBC_HEAP_RESERVE,
       "and what it kept fits the budget");
  }

  printf("\n== PSRAM ignores the reserve, because nothing shares that pool ==\n");
  {
    DbcCounts c{1104, 8472, 2199};
    const DbcFit fit = dbcFitToHeap(c, 2 * 1024 * 1024, DBC_HEAP_RESERVE, true);
    ok(fit == DBC_FIT_ALL, "the whole vendor map fits in 2 MB of PSRAM");
  }

  printf("\n%s\n", failures ? "TESTS FAILED" : "ALL PASSED (0 failures)");
  return failures ? 1 : 0;
}
