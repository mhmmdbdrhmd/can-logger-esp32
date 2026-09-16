/* ============================================================================
 *  test_bundle.cpp - does the firmware actually unpack a setup bundle?
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  bundle.cpp was shipped on the strength of one line on a serial console:
 *
 *      I setup bundle: unpacked 3 file(s), prepared for name_max 32
 *
 *  which proves it counted three headers. It says nothing about whether the
 *  bytes written were the bytes packed - and a frame map that is short by a
 *  few bytes, or off by one at a boundary, parses to a map with fewer signals
 *  and no error at all. That is precisely the failure the count cannot see.
 *
 *  These drive the real bundleUnpack() against an in-memory card and compare
 *  contents, not counts.
 * ==========================================================================*/
#include <cstdio>
#include <cstring>
#include <string>
#include "Arduino.h"
#include "SD.h"
#include "logger.h"      /* LogLevel, for the logPost stub below */
#include "bundle.h"

FakeSD     SD;
uint32_t   g_fakeMs = 0;
FakeSerial Serial;

/* bundle.cpp logs through logPost(). Stubbed rather than linking logger.cpp,
 * which drags in the log task and the queue - none of which this is testing,
 * and all of which would have to be started for it not to crash. */
void logPost(LogLevel, bool, const char *, ...) {}

static int fails = 0;

static void ck(const char *what, bool ok) {
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) fails++;
}

static void ckeq(const char *what, const std::string &got,
                 const std::string &want) {
  const bool ok = (got == want);
  printf("  %-4s %s", ok ? "ok" : "FAIL", what);
  if (!ok) printf("   (got %zu bytes, wanted %zu)", got.size(), want.size());
  printf("\n");
  if (!ok) fails++;
}

static std::string pack(const std::string &hdr,
                        const char *n1, const std::string &f1,
                        const char *n2, const std::string &f2) {
  char buf[128];
  std::string out = hdr;
  snprintf(buf, sizeof(buf), "#FILE %s %zu\n", n1, f1.size());
  out += buf; out += f1; out += "\n";
  if (n2) {
    snprintf(buf, sizeof(buf), "#FILE %s %zu\n", n2, f2.size());
    out += buf; out += f2; out += "\n";
  }
  out += "#END\n";
  return out;
}

int main() {
  printf("\n== no bundle on the card ==\n");
  SDFiles::clear();
  {
    const BundleInfo bi = bundleUnpack();
    ck("nothing found, nothing done", !bi.found && !bi.ok && bi.files == 0);
  }

  printf("\n== a normal bundle ==\n");
  SDFiles::clear();
  {
    /* A payload that ENDS IN A NEWLINE and one that does NOT, because the
     * separator is what the format got wrong first: it used to be written only
     * when the payload lacked one, so a reader could not know whether to skip
     * a byte and a round-trip silently lost the second file. */
    const std::string dbc  = "BO_ 100 Msg: 8 X\n SG_ A : 0|8@1+ (1,0) [0|0] \"\" Y\n";
    const std::string cfg  = "cells=1\nMsg.A";       /* no trailing newline */
    SDFiles::put("/logger.bundle",
                 pack("#DCLB1 name_max=32\n", "frames.dbc", dbc,
                      "dash.cfg", cfg));

    const BundleInfo bi = bundleUnpack();
    ck("found and unpacked", bi.found && bi.ok);
    ck("two files written", bi.files == 2);
    ck("name_max read from the header", bi.nameMax == 32);
    ck("bundleNameMax() agrees", bundleNameMax() == 32);
    ckeq("frames.dbc is byte-for-byte what was packed",
         SDFiles::get("/frames.dbc"), dbc);
    ckeq("dash.cfg is byte-for-byte what was packed",
         SDFiles::get("/dash.cfg"), cfg);
    ck("no .part left behind", !SDFiles::has("/frames.dbc.part") &&
                               !SDFiles::has("/dash.cfg.part"));
    ck("the bundle is set aside, not left to unpack again",
       !SDFiles::has("/logger.bundle") && SDFiles::has("/logger.applied"));

    /* The next boot. A layout edited in the browser in between must survive,
     * and the name length must still be known - it sizes the tables. */
    SDFiles::put("/dash.cfg", "EDITED IN THE BROWSER");
    const BundleInfo again = bundleUnpack();
    ck("next boot: nothing new to unpack", !again.found && again.files == 0);
    ck("but the applied name_max is still in force",
       again.applied && again.nameMax == 32 && bundleNameMax() == 32);
    ckeq("and the edited layout was left alone",
         SDFiles::get("/dash.cfg"), std::string("EDITED IN THE BROWSER"));
  }

  printf("\n== a new bundle replaces the applied one ==\n");
  {
    SDFiles::put("/logger.bundle",
                 pack("#DCLB1 name_max=16\n", "frames.dbc", "NEW MAP\n",
                      nullptr, ""));
    const BundleInfo bi = bundleUnpack();
    ck("unpacked", bi.found && bi.ok && bi.files == 1);
    ckeq("the new map is in place", SDFiles::get("/frames.dbc"),
         std::string("NEW MAP\n"));
    ck("its name_max is the one in force now",
       bi.nameMax == 16 && bundleNameMax() == 16);
    const BundleInfo later = bundleUnpack();
    ck("and it is what the next boot reads", later.applied && later.nameMax == 16);
  }

  printf("\n== no bundle ever ==\n");
  SDFiles::clear();
  {
    const BundleInfo bi = bundleUnpack();
    ck("no name_max to apply", !bi.applied && bi.nameMax == 0);
  }

  printf("\n== a file that is not a bundle ==\n");
  SDFiles::clear();
  {
    /* Refusing matters more than it looks: the three files this would write
     * over are a working setup, and half-unpacking junk across them turns a
     * recoverable mistake into a rebuild. */
    SDFiles::put("/frames.dbc", "ORIGINAL");
    SDFiles::put("/logger.bundle", "just some text\nnot a bundle at all\n");
    const BundleInfo bi = bundleUnpack();
    ck("found but refused", bi.found && !bi.ok);
    ck("and left in place to be seen again", SDFiles::has("/logger.bundle") &&
                                             !SDFiles::has("/logger.applied"));
    ck("says why", bi.err[0] != '\0');
    ckeq("the existing frame map is untouched",
         SDFiles::get("/frames.dbc"), std::string("ORIGINAL"));
  }

  printf("\n== a bundle cut short in transit ==\n");
  SDFiles::clear();
  {
    const std::string good = "GOOD MAP\n";
    std::string b = "#DCLB1 name_max=16\n";
    char buf[64];
    snprintf(buf, sizeof(buf), "#FILE frames.dbc %zu\n", good.size());
    b += buf; b += good; b += "\n";
    /* declares 900 bytes, supplies 4, then stops */
    b += "#FILE frames2.dbc 900\nabcd";
    SDFiles::put("/frames2.dbc", "PREVIOUS");
    SDFiles::put("/logger.bundle", b);

    const BundleInfo bi = bundleUnpack();
    ck("the complete file before the cut was written", bi.files == 1);
    ckeq("and is correct", SDFiles::get("/frames.dbc"), good);
    ck("the truncated one is reported, not silently dropped",
       !bi.ok && bi.err[0] != '\0');
    ckeq("the previous frames2.dbc is NOT replaced by a half file",
         SDFiles::get("/frames2.dbc"), std::string("PREVIOUS"));
    ck("no .part left behind", !SDFiles::has("/frames2.dbc.part"));
  }

  printf("\n== a payload containing lines that look like headers ==\n");
  SDFiles::clear();
  {
    /* The reason the format counts bytes instead of scanning for delimiters. A
     * .dbc may contain any line a delimiter could be - including these. */
    const std::string sneaky = "#FILE evil.dbc 999\n#END\nBO_ 1 A: 1 X\n";
    SDFiles::put("/logger.bundle",
                 pack("#DCLB1 name_max=64\n", "frames.dbc", sneaky, nullptr, ""));
    const BundleInfo bi = bundleUnpack();
    ck("one file, not three", bi.files == 1 && bi.ok);
    ckeq("the payload survived verbatim, headers and all",
         SDFiles::get("/frames.dbc"), sneaky);
    ck("nothing called evil.dbc was created", !SDFiles::has("/evil.dbc"));
  }

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
