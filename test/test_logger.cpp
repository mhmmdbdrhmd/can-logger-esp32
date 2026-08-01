/* Exercises the two-tier logger: sink routing, ring wrap, incremental JSON. */
#include "logger.h"
#include <string>

uint32_t   g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;

static int failures = 0;
static void ck(const char *what, bool ok, const std::string &detail = "") {
  if (ok) printf("  ok   %s %s\n", what, detail.c_str());
  else  { printf("  FAIL %s %s\n", what, detail.c_str()); failures++; }
}
static size_t countJson(const String &s) {   /* number of "..." elements */
  size_t n = 0; bool in = false;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '"' && (i == 0 || s[i-1] != '\\')) { if (!in) n++; in = !in; }
  }
  return n;
}

int main() {
  logInit();

  /* ---- 1. routing: live goes everywhere, file-only skips serial ------ */
  printf("\n== sink routing ==\n");
  std::string logfile;
  File f; f.sink = &logfile;
  logAttachFile(&f);

  for (int i = 0; i < 5; i++) LOG_LIVE(LVL_INFO, "live %d", i);
  for (int i = 0; i < 3; i++) LOG_FILE(LVL_DEBUG, "detail %d", i);
  logService();

  ck("5 live lines on serial", Serial.lines.size() == 5,
     "got " + std::to_string(Serial.lines.size()));
  size_t nl = 0; for (char c : logfile) if (c == '\n') nl++;
  ck("all 8 lines in .log", nl == 8, "got " + std::to_string(nl));
  ck("log is a superset", logfile.find("live 0") != std::string::npos &&
                          logfile.find("detail 0") != std::string::npos);
  ck("detail not on serial", logfile.find("detail 2") != std::string::npos &&
     Serial.lines[4].find("detail") == std::string::npos);
  printf("       serial sample: %s\n", Serial.lines[0].c_str());

  /* ---- 2. incremental delivery to the browser ------------------------ */
  printf("\n== incremental web log ==\n");
  String j1; uint32_t s1 = webLogToJson(0, j1);
  ck("first poll returns 5", countJson(j1) == 5 && s1 == 5,
     "n=" + std::to_string(countJson(j1)) + " seq=" + std::to_string(s1));

  String j2; uint32_t s2 = webLogToJson(s1, j2);
  ck("nothing new -> empty", countJson(j2) == 0 && s2 == 5);

  LOG_LIVE(LVL_WARN, "one more"); logService();
  String j3; uint32_t s3 = webLogToJson(s2, j3);
  ck("only the new line", countJson(j3) == 1 && s3 == 6, j3.c_str());

  /* ---- 3. ring wrap: a client that fell behind gets what is left ----- */
  printf("\n== ring wrap (%d lines deep) ==\n", WEB_LOG_LINES);
  for (int i = 0; i < 300; i++) { LOG_LIVE(LVL_INFO, "flood %d", i); logService(); }
  String j4; uint32_t s4 = webLogToJson(0, j4);
  ck("clamped to ring depth", countJson(j4) == WEB_LOG_LINES,
     "got " + std::to_string(countJson(j4)));
  ck("holds the NEWEST lines", j4.find("flood 299") != std::string::npos &&
                               j4.find("flood 100") == std::string::npos);
  ck("seq keeps counting", s4 == 306, "seq=" + std::to_string(s4));

  /* a client reporting a sequence from before a reboot must not hang */
  String j5; uint32_t s5 = webLogToJson(999999, j5);
  ck("future seq is safe", countJson(j5) == 0 && s5 == s4);

  /* ---- 4. a full queue is counted, never blocks ---------------------- */
  printf("\n== queue overflow is counted, not hidden ==\n");
  const uint32_t before = logDroppedCount();
  for (int i = 0; i < LOG_QUEUE_LEN + 12; i++) LOG_LIVE(LVL_INFO, "burst %d", i);
  const uint32_t dropped = logDroppedCount() - before;
  ck("12 dropped, rest kept", dropped == 12, "dropped=" + std::to_string(dropped));
  logService();

  /* ---- 5. JSON escaping --------------------------------------------- */
  printf("\n== JSON escaping ==\n");
  LOG_LIVE(LVL_ERROR, "quote \" and backslash \\ here"); logService();
  String j6; webLogToJson(s4 + LOG_QUEUE_LEN, j6);
  ck("quotes escaped", j6.find("\\\"") != std::string::npos);
  ck("backslash escaped", j6.find("\\\\") != std::string::npos);

  /* ---- 6. detaching keeps serial alive ------------------------------- */
  printf("\n== detached (not recording) ==\n");
  logAttachFile(nullptr);
  ck("file detached", !logFileAttached());
  const size_t sizeBefore = logfile.size();
  LOG_LIVE(LVL_INFO, "still on serial"); logService();
  ck("file untouched", logfile.size() == sizeBefore);
  ck("serial still fed", Serial.lines.back().find("still on serial") != std::string::npos);

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
