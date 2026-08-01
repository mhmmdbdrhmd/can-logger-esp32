/* Host-side exercise of the CSV writer: real payloads in, exact rows out, in
 * both modes - with a frame map and without one. */
#include "decode.h"
#include "fmt.h"
#include <string>

uint32_t   g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;

static int failures = 0;

static void eq(const char *what, const std::string &got, const std::string &want) {
  if (got == want) printf("  ok   %-30s %s", what, got.c_str());
  else { printf("  FAIL %-30s got  %s       want %s\n", what, got.c_str(), want.c_str());
         failures++; }
}

static void ck(const char *what, bool ok, const std::string &detail = "") {
  if (ok) printf("  ok   %-30s %s\n", what, detail.c_str());
  else  { printf("  FAIL %-30s %s\n", what, detail.c_str()); failures++; }
}

static char g_rowbuf[DECODE_FRAME_MAX + 64];

static std::string emit(Decoder &d, const CanFrame &f, uint8_t *nOut = nullptr) {
  uint8_t n = 0;
  const size_t k = d.rows(f, g_rowbuf, sizeof(g_rowbuf), &n);
  if (nOut) *nOut = n;
  return std::string(g_rowbuf, k);
}

static CanFrame mk(uint32_t id, uint64_t us, std::initializer_list<uint8_t> b,
                   bool ext = false) {
  CanFrame f{};
  f.id = id; f.esp_us = us; f.ext = ext ? 1 : 0;
  f.len = (uint8_t)b.size();
  uint8_t i = 0;
  for (uint8_t v : b) f.data[i++] = v;
  return f;
}

static void feed(DbcDb &db, const char *const *lines) {
  char buf[DBC_LINE_MAX];
  for (const char *const *l = lines; *l; l++) {
    snprintf(buf, sizeof(buf), "%s", *l);
    dbcParseLine(db, buf);
  }
}

static const char *const MAP[] = {
  "VERSION \"decode test map\"",
  "BO_ 256 NodeStatus: 8 NodeA",
  " SG_ Uptime : 0|32@1+ (1,0) [0|4294967295] \"s\" Vector__XXX",
  " SG_ State : 32|4@1+ (1,0) [0|15] \"\" Vector__XXX",
  "BO_ 257 MotorFeedback: 8 NodeA",
  " SG_ Speed : 0|16@1- (0.25,0) [-8192|8191.75] \"rpm\" Vector__XXX",
  "BO_ 419364096 WideId: 2 NodeB",      /* 0x18FF5000 with the 29-bit flag off */
  " SG_ Value : 0|16@1+ (1,0) [0|65535] \"\" Vector__XXX",
  "VAL_ 256 State 0 \"boot\" 2 \"running\" ;",
  nullptr
};

int main() {
  /* ================================================================== */
  printf("\n== no frame map: every frame is kept as raw bytes ==\n");
  {
    Decoder d;
    d.reset(nullptr);
    busReset(g_bus);

    eq("first frame is t=0", emit(d, mk(0x123, 1000000, {0xDE, 0xAD, 0xBE})),
       "0;0x123;;;;;DEADBE\n");
    eq("timestamps are rebased", emit(d, mk(0x7FF, 1000250, {0x01})),
       "250;0x7FF;;;;;01\n");
    eq("zero-length frame", emit(d, mk(0x001, 1000500, {})),
       "500;0x1;;;;;\n");
    eq("29-bit id prints wide",
       emit(d, mk(0x18FF5000, 1000750, {0xAA}, true)),
       "750;0x18FF5000;;;;;AA\n");

    ck("undecoded frames counted", g_bus.undecoded == 4,
       "got " + std::to_string(g_bus.undecoded));
    /* Row formatting and activity accounting are separate on purpose - the
     * table is fed by busObserve() whether or not a recording is running. */
    ck("rows() leaves the id table alone", g_bus.used == 0);
  }

  /* ================================================================== */
  printf("\n== with a frame map: one row per signal ==\n");
  DbcDb db;
  dbcReset(db);
  feed(db, MAP);

  {
    Decoder d;
    d.reset(&db);
    busReset(g_bus);

    uint8_t n = 0;
    const std::string two = emit(d, mk(0x100, 0, {42, 0, 0, 0, 2, 0, 0, 0}), &n);
    eq("two signals, two rows", two,
       "0;0x100;NodeStatus;Uptime;42;s;\n"
       "0;0x100;NodeStatus;State;running;;\n");
    ck("row count reported", n == 2, "got " + std::to_string(n));

    eq("signed, scaled, exact",
       emit(d, mk(0x101, 1500, {0x18, 0xFC, 0, 0, 0, 0, 0, 0})),
       "1500;0x101;MotorFeedback;Speed;-250.00;rpm;\n");

    eq("unmapped id still recorded",
       emit(d, mk(0x200, 2000, {0x11, 0x22})),
       "2000;0x200;;;;;1122\n");

    /* A 29-bit identifier with the same numeric value as an 11-bit entry must
     * not match it - they are different frames on the wire. */
    eq("29-bit id is not the 11-bit one",
       emit(d, mk(0x100, 2500, {1, 2}, true)),
       "2500;0x00000100;;;;;0102\n");

    /* Signals declared past the end of a short payload are skipped, and the
     * frame falls back to raw rather than producing a half-decoded row. */
    eq("short payload falls back to raw",
       emit(d, mk(0x100, 3000, {1, 2})),
       "3000;0x100;NodeStatus;;;;0102\n");
  }

  /* ================================================================== */
  printf("\n== the schema never varies ==\n");
  {
    Decoder d;
    d.reset(&db);
    const std::string all =
      emit(d, mk(0x100, 0, {1, 0, 0, 0, 0, 0, 0, 0})) +
      emit(d, mk(0x999, 100, {0xFF})) +
      emit(d, mk(0x101, 200, {0, 0, 0, 0, 0, 0, 0, 0}));

    size_t rows = 0, bad = 0;
    size_t start = 0;
    while (start < all.size()) {
      const size_t nl = all.find('\n', start);
      const std::string line = all.substr(start, nl - start);
      start = nl + 1;
      rows++;
      size_t seps = 0;
      for (char c : line) if (c == ';') seps++;
      if (seps != 6) bad++;
    }
    ck("every row has 7 fields", bad == 0,
       std::to_string(rows) + " rows, " + std::to_string(bad) + " malformed");
  }

  /* ================================================================== */
  printf("\n== bus activity table ==\n");
  {
    busReset(g_bus);
    g_fakeMs = 1000;
    for (int i = 0; i < 10; i++) busObserve(g_bus, mk(0x100, 0, {0}), &db);
    for (int i = 0; i < 3;  i++) busObserve(g_bus, mk(0x555, 0, {0}), &db);
    busTick(g_bus, 1000);

    ck("two ids tracked", g_bus.used == 2, "got " + std::to_string(g_bus.used));
    ck("counts and rates", g_bus.count[0] == 10 && g_bus.rate[0] == 10 &&
                           g_bus.count[1] == 3  && g_bus.rate[1] == 3);
    ck("mapped ids flagged", g_bus.known[0] == 1 && g_bus.known[1] == 0);
    ck("total counted", g_bus.total == 13, "got " + std::to_string((unsigned long)g_bus.total));

    for (int i = 0; i < BUS_TRACK_IDS + 8; i++)
      busObserve(g_bus, mk(0x600 + (uint32_t)i, 0, {0}), &db);
    ck("table clamps", g_bus.used == BUS_TRACK_IDS,
       "got " + std::to_string(g_bus.used));
    ck("overflow counted, frames not lost", g_bus.untracked > 0);
  }

  /* ================================================================== */
  printf("\n== the header block describes the file it opens ==\n");
  {
    static char hdr[8192];

    DbcDb none; dbcReset(none);
    const size_t n0 = csvHeaderBlock(hdr, sizeof(hdr), "1.csv", none);
    const std::string h0(hdr, n0);
    ck("raw-mode header fits", n0 > 0, std::to_string(n0) + " bytes");
    ck("raw-mode header says so", h0.find("none - no /frames.dbc") != std::string::npos);
    const std::string cols = "t_us;id;name;signal;value;unit;raw\n";
    ck("column line last",
       h0.size() > cols.size() &&
       h0.compare(h0.size() - cols.size(), cols.size(), cols) == 0);

    const size_t n1 = csvHeaderBlock(hdr, sizeof(hdr), "2.csv", db);
    const std::string h1(hdr, n1);
    ck("mapped header fits", n1 > 0, std::to_string(n1) + " bytes");
    ck("lists the messages", h1.find("NodeStatus") != std::string::npos &&
                             h1.find("MotorFeedback") != std::string::npos);
    ck("lists the signals and units", h1.find("Uptime s") != std::string::npos);
    ck("records the DBC version", h1.find("decode test map") != std::string::npos);
    ck("declares exactness", h1.find("all values exact") != std::string::npos);

    /* A buffer that is too small must report failure, not truncate silently. */
    ck("refuses to truncate", csvHeaderBlock(hdr, 200, "3.csv", db) == 0);
  }

  /* ================================================================== */
  printf("\n== sizing ==\n");
  {
    Decoder d;
    d.reset(&db);
    size_t total = 0;
    for (int i = 0; i < 100; i++) {
      total += emit(d, mk(0x100, (uint64_t)i * 10000,
                          {(uint8_t)i, 0, 0, 0, 1, 0, 0, 0})).size();
    }
    printf("  mean frame %.1f bytes -> %.1f KB/s at 1000 frames/s\n",
           total / 100.0, total / 100.0 * 1000 / 1024.0);
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
