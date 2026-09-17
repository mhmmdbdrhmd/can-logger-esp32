/* ============================================================================
 *  test_sdutil.cpp - after a failed write, where does the CSV continue?
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  A write to the card failed on the bench after its first sector had already
 *  landed. The recovery trusted write()'s count, wrote the whole block again,
 *  and 29 bytes appeared in the CSV twice, breaking a row. The recovery now
 *  measures the file instead; these pin that arithmetic.
 * ==========================================================================*/
#include <cstdio>
#include "sdutil.h"

FakeSD SD;

static int fails = 0;

static void ck(const char *what, bool ok) {
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) fails++;
}

int main() {
  printf("\n== how much of the block is already on the card ==\n");
  {
    SdResume r = sdResume(1000, 1000, 500);
    ck("nothing landed: write the whole block", r.done == 0 && r.lostBefore == 0);

    r = sdResume(1000, 1029, 500);
    ck("29 bytes landed: skip exactly those", r.done == 29 && r.lostBefore == 0);

    r = sdResume(1000, 1500, 500);
    ck("all of it landed: write nothing more", r.done == 500 && r.lostBefore == 0);

    r = sdResume(1000, 1700, 500);
    ck("a length past the block is capped at the block", r.done == 500);

    r = sdResume(1000, 600, 500);
    ck("the card kept less than was written before: counted as lost",
       r.done == 0 && r.lostBefore == 400);

    r = sdResume(5000000000ULL, 5000000512ULL, 32768);
    ck("past 4 GB of bookkeeping the sums still hold", r.done == 512);
  }

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
