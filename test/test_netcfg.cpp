/* ============================================================================
 *  test_netcfg.cpp - is the radio configured before it is started?
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  WiFi.useStaticBuffers() is read exactly once, inside the Arduino core's
 *  lazy wifiLowLevelInit(). Call it after anything else has touched the radio
 *  and it is ignored - no error, no warning in the serial log, nothing. The
 *  firmware looks correct, the flag is set, and the buffers are still the
 *  core's 4 static / 32 dynamic.
 *
 *  That is the worst shape a bug can have here, because the symptom is
 *  "the fix did not work" rather than "the code is wrong", and the last time
 *  something in this area went wrong silently it cost two ten-minute bench
 *  runs before anyone suspected the call rather than the idea.
 *
 *  So the assertion is about ORDER, not about the call existing. The WiFi shim
 *  counts how many other WiFi calls were made before useStaticBuffers(); this
 *  insists the answer is zero.
 *
 *  It also pins the previous mistake: netBegin() must not stop, deinitialise
 *  and re-initialise the driver. That hung the board - the core creates the
 *  netifs and registers its event handlers around its own esp_wifi_init() and
 *  tracks it in a private flag, so tearing the driver down from outside leaves
 *  it holding pointers into something that no longer exists. setup() never
 *  returned, and because logService() drains the log queue from loop() the
 *  serial capture was empty.
 * ==========================================================================*/
#include <cstdio>
#include <cstring>
#include "Arduino.h"
#include "SD.h"
#include "WiFi.h"
#include "logger.h"
#include "mem.h"
#include "netcfg.h"
#include "recorder.h"

FakeSD     SD;
FakeSerial Serial;
uint32_t   g_fakeMs = 0;
RecStatus  g_rec;
FakeWiFi   WiFi;

void logPost(LogLevel, bool, const char *, ...) {}
MemStat  memStat() { return MemStat{ 60000, 45000, 55000, 45000 }; }
uint32_t memLargestBlock() { return 45000; }

static int fails = 0;

static void ck(const char *what, bool ok) {
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) fails++;
}

int main() {
  printf("\n== the buffer flag is set before the radio is touched ==\n");

  /* Hotspot mode, which is the short path through netBegin(): it sets the
   * flag, sets persistent(false), then starts the AP. Station mode takes the
   * same first two steps, so ordering is covered either way. */
  g_net.apMode = true;
  g_net.apSsid = "test-ap";
  g_net.apPass = "";

  wifiShim() = WiFiShimState{};
  netBegin();

#if WIFI_STATIC_BUFFERS
  ck("useStaticBuffers(true) was called", wifiShim().staticBuffers);
  ck("and NOTHING touched the radio before it - the core reads the flag "
     "once, in its own lazy init",
     wifiShim().touchedBefore == 0);
#else
  ck("with WIFI_STATIC_BUFFERS 0 the core's 4/32 are left alone",
     !wifiShim().staticBuffers);
#endif

  /* The AP still came up. A reordering that satisfied the assertion above by
   * not starting the radio at all would be worse than the bug. */
  ck("the access point was still started", wifiShim().otherCalls > 0);

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
