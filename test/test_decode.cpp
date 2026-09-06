/* Host-side exercise of the CSV writer: real payloads in, exact rows out, in
 * both modes - with a frame map and without one. */
#include "decode.h"
#include "fmt.h"
#include <string>
#include <sstream>
#include <set>
#include <string.h>

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
                   bool ext = false, uint8_t bus = 0) {
  CanFrame f{};
  f.id = id; f.esp_us = us; f.ext = ext ? 1 : 0; f.bus = bus & 1;
  f.len = (uint8_t)b.size();
  uint8_t i = 0;
  for (uint8_t v : b) f.data[i++] = v;
  return f;
}

/* The decoder takes an ARRAY of maps, one per bus. Most tests only care about
 * one, so this builds the pair with the same map on both unless told otherwise. */
static DbcDb *pair(DbcDb *a, DbcDb *b) {
  static DbcDb both[CAN_BUSES];
  both[0] = *a;
  both[1] = *(b ? b : a);
  return both;
}

/* Loads the map the way the firmware does: count, size the tables to the file,
 * then parse. Feeding dbcParseLine() with no tables allocated parses nothing. */
static void feed(DbcDb &db, const char *const *lines) {
  std::string text;
  for (const char *const *l = lines; *l; l++) { text += *l; text += '\n'; }
  dbcLoadText(db, text.c_str(), text.size());
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
    busReset(g_bus[0]);

    eq("first frame is t=0", emit(d, mk(0x123, 1000000, {0xDE, 0xAD, 0xBE})),
       "0;1;0x123;;;;;DEADBE\n");
    eq("timestamps are rebased", emit(d, mk(0x7FF, 1000250, {0x01})),
       "250;1;0x7FF;;;;;01\n");
    eq("zero-length frame", emit(d, mk(0x001, 1000500, {})),
       "500;1;0x1;;;;;\n");
    eq("29-bit id prints wide",
       emit(d, mk(0x18FF5000, 1000750, {0xAA}, true)),
       "750;1;0x18FF5000;;;;;AA\n");

    ck("undecoded frames counted", g_bus[0].undecoded == 4,
       "got " + std::to_string(g_bus[0].undecoded));
    /* Row formatting and activity accounting are separate on purpose - the
     * table is fed by busObserve() whether or not a recording is running. */
    ck("rows() leaves the id table alone", g_bus[0].used == 0);
  }

  /* ================================================================== */
  printf("\n== with a frame map: one row per signal ==\n");
  DbcDb db;
  dbcReset(db);
  feed(db, MAP);

  {
    Decoder d;
    d.reset(pair(&db, nullptr));
    busReset(g_bus[0]);

    uint8_t n = 0;
    const std::string two = emit(d, mk(0x100, 0, {42, 0, 0, 0, 2, 0, 0, 0}), &n);
    eq("two signals, two rows", two,
       "0;1;0x100;NodeStatus;Uptime;42;s;\n"
       "0;1;0x100;NodeStatus;State;running;;\n");
    ck("row count reported", n == 2, "got " + std::to_string(n));

    eq("signed, scaled, exact",
       emit(d, mk(0x101, 1500, {0x18, 0xFC, 0, 0, 0, 0, 0, 0})),
       "1500;1;0x101;MotorFeedback;Speed;-250.00;rpm;\n");

    eq("unmapped id still recorded",
       emit(d, mk(0x200, 2000, {0x11, 0x22})),
       "2000;1;0x200;;;;;1122\n");

    /* A 29-bit identifier with the same numeric value as an 11-bit entry must
     * not match it - they are different frames on the wire. */
    eq("29-bit id is not the 11-bit one",
       emit(d, mk(0x100, 2500, {1, 2}, true)),
       "2500;1;0x00000100;;;;;0102\n");

    /* Signals declared past the end of a short payload are skipped, and the
     * frame falls back to raw rather than producing a half-decoded row. */
    eq("short payload falls back to raw",
       emit(d, mk(0x100, 3000, {1, 2})),
       "3000;1;0x100;NodeStatus;;;;0102\n");
  }

  /* ================================================================== */
  printf("\n== the schema never varies ==\n");
  {
    Decoder d;
    d.reset(pair(&db, nullptr));
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
      if (seps != 7) bad++;
    }
    ck("every row has 8 fields", bad == 0,
       std::to_string(rows) + " rows, " + std::to_string(bad) + " malformed");
  }

  /* ================================================================== */
  printf("\n== bus activity table ==\n");
  {
    busReset(g_bus[0]);
    g_fakeMs = 1000;
    for (int i = 0; i < 10; i++) busObserve(g_bus[0], mk(0x100, 0, {0}), &db);
    for (int i = 0; i < 3;  i++) busObserve(g_bus[0], mk(0x555, 0, {0}), &db);
    busTick(g_bus[0], 1000);

    ck("two ids tracked", g_bus[0].used == 2, "got " + std::to_string(g_bus[0].used));
    ck("counts and rates", g_bus[0].count[0] == 10 && g_bus[0].rate[0] == 10 &&
                           g_bus[0].count[1] == 3  && g_bus[0].rate[1] == 3);
    ck("mapped ids flagged", g_bus[0].known[0] == 1 && g_bus[0].known[1] == 0);
    ck("total counted", g_bus[0].total == 13, "got " + std::to_string((unsigned long)g_bus[0].total));

    for (int i = 0; i < BUS_TRACK_IDS + 8; i++)
      busObserve(g_bus[0], mk(0x600 + (uint32_t)i, 0, {0}), &db);
    ck("table clamps", g_bus[0].used == BUS_TRACK_IDS,
       "got " + std::to_string(g_bus[0].used));
    ck("overflow counted, frames not lost", g_bus[0].untracked > 0);
  }

  /* ================================================================== */
  printf("\n== the bus column, and the map that goes with it ==\n");
  {
    /* Two DIFFERENT maps that both describe identifier 0x100. This is the case
     * the whole per-bus design exists for: the same number on two wires means
     * two different things, and a logger that decodes both against one map is
     * confidently wrong rather than merely unhelpful. */
    static const char *const MAP2[] = {
      "VERSION \"the other bus\"",
      "BO_ 256 PumpState: 8 NodeC",
      " SG_ Pressure : 0|16@1+ (0.5,0) [0|32767] \"bar\" Vector__XXX",
      nullptr
    };
    DbcDb db2; dbcReset(db2); feed(db2, MAP2);

    static DbcDb both[CAN_BUSES];
    both[0] = db;
    both[1] = db2;

    Decoder d;
    d.reset(both);

    eq("bus 1 decodes with map 1",
       emit(d, mk(0x100, 0, {42, 0, 0, 0, 2, 0, 0, 0}, false, 0)),
       "0;1;0x100;NodeStatus;Uptime;42;s;\n"
       "0;1;0x100;NodeStatus;State;running;;\n");

    eq("the SAME id on bus 2 decodes with map 2",
       emit(d, mk(0x100, 100, {20, 0, 0, 0, 0, 0, 0, 0}, false, 1)),
       "100;2;0x100;PumpState;Pressure;10.0;bar;\n");

    /* One clock for both buses: the epoch is taken from the first frame seen,
     * whichever bus it came from, so the two are directly comparable. */
    eq("one timestamp origin across both buses",
       emit(d, mk(0x777, 250, {0xAB}, false, 1)),
       "250;2;0x777;;;;;AB\n");

    eq("an id only bus 1 knows is raw on bus 2",
       emit(d, mk(0x101, 300, {0x18, 0xFC, 0, 0, 0, 0, 0, 0}, false, 1)),
       "300;2;0x101;;;;;18FC000000000000\n");

    /* Undecoded frames are counted against the bus they arrived on. A merged
     * counter would report a fully mapped bus as partly undecoded whenever the
     * other one was not mapped at all. */
    busReset(g_bus[0]);
    busReset(g_bus[1]);
    Decoder d2;
    d2.reset(both);
    (void)emit(d2, mk(0x999, 0, {1}, false, 0));
    (void)emit(d2, mk(0x999, 10, {1}, false, 1));
    (void)emit(d2, mk(0x998, 20, {1}, false, 1));
    ck("undecoded counted per bus",
       g_bus[0].undecoded == 1 && g_bus[1].undecoded == 2,
       "bus1=" + std::to_string(g_bus[0].undecoded) +
       " bus2=" + std::to_string(g_bus[1].undecoded));
  }

  /* ================================================================== */
  printf("\n== the column header is the schema ==\n");
  {
    char hdr[128];
    const size_t n = csvColumnHeader(hdr, sizeof(hdr));
    eq("eight columns, bus second", std::string(hdr, n),
       "t_us;bus;id;name;signal;value;unit;raw\n");
    ck("refuses to truncate", csvColumnHeader(hdr, 10) == 0);
  }

  /* ================================================================== */
  printf("\n== the sidecar says which schema it is ==\n");
  {
    static char meta[16384];
    static DbcDb both[CAN_BUSES];
    both[0] = db;
    dbcReset(both[1]);

    const size_t n = metaJson(meta, sizeof(meta), "1.csv", "1.log", both);
    const std::string m(meta, n);
    ck("meta fits", n > 0, std::to_string(n) + " bytes");
    ck("declares schema 2", m.find("\"schema\": 2") != std::string::npos);
    ck("names the bus column", m.find("\"bus\"") != std::string::npos);
    ck("one entry per bus", m.find("\"bus\": 1") != std::string::npos &&
                            m.find("\"bus\": 2") != std::string::npos);
    ck("names both map paths", m.find("/frames.dbc") != std::string::npos &&
                               m.find("/frames2.dbc") != std::string::npos);
    ck("carries the map that was loaded",
       m.find("NodeStatus") != std::string::npos &&
       m.find("decode test map") != std::string::npos);
    ck("tells a reader to group on the bus too",
       m.find("t_us, bus and id") != std::string::npos);

    /* A buffer that is too small must report failure, not truncate silently. */
    ck("refuses to truncate", metaJson(meta, 200, "3.csv", "3.log", both) == 0);
  }

  /* ================================================================== */
  printf("\n== sizing ==\n");
  {
    Decoder d;
    d.reset(pair(&db, nullptr));
    size_t total = 0;
    for (int i = 0; i < 100; i++) {
      total += emit(d, mk(0x100, (uint64_t)i * 10000,
                          {(uint8_t)i, 0, 0, 0, 1, 0, 0, 0})).size();
    }
    printf("  mean frame %.1f bytes -> %.1f KB/s at 1000 frames/s\n",
           total / 100.0, total / 100.0 * 1000 / 1024.0);
  }

  /* ==================================================================
   *  The per-identifier log line.
   *
   *  Every one of these asserts a bug found in ten hours of real recordings:
   *  an uninitialised buffer printed as %s, a list that ran past the line
   *  limit and pushed the totals off the end, and snprintf's return value
   *  added blind so the last entry was cut mid-number.
   * ================================================================== */
  printf("\n== the per-identifier log line ==\n");
  {
    BusStats b;
    busReset(b);
    char    line[LOG_LINE_CHARS];
    uint8_t cur = 0;

    /* (a) No traffic at all. The old code left `per` uninitialised and the
     *     log printed whatever was on the stack. */
    memset(line, 0x5A, sizeof(line));
    ck("an empty table writes an empty string",
       busFormatIds(b, &cur, line, sizeof(line)) == 0 && line[0] == '\0');

    /* A busy bus: far more identifiers than a line can hold. */
    const uint32_t N = BUS_TRACK_IDS;
    for (uint32_t i = 0; i < N; i++) {
      for (int r = 0; r < 700 + (int)i; r++) busNote(b, 0x18FEF100u + i, true,
                                                     (i % 2) == 0, 1000);
    }
    busTick(b, 1000);
    ck("the table filled", b.used == N, std::to_string(b.used));

    /* (b) and (c): whatever it writes must fit, must be nul-terminated, and
     *     must never end in a half-written entry. */
    cur = 0;
    std::set<std::string> seen;
    uint8_t  totalShown = 0;
    bool     partial = false, overran = false;

    for (int pass = 0; pass < 12; pass++) {
      memset(line, 0x5A, sizeof(line));
      const uint8_t n = busFormatIds(b, &cur, line, sizeof(line));
      totalShown = (uint8_t)(totalShown + n);

      const char *nul = (const char *)memchr(line, '\0', sizeof(line));
      if (!nul) { overran = true; break; }

      /* Split on spaces. Each entry must be complete: 0x<id>=<n>(<r>/s) */
      std::string  text(line);
      std::istringstream is(text);
      std::string  tok;
      int          entries = 0;
      while (is >> tok) {
        entries++;
        if (tok.rfind("0x", 0) != 0 || tok.find('=') == std::string::npos ||
            tok.find("/s)") == std::string::npos) {
          partial = true;
        }
        seen.insert(tok.substr(0, tok.find('=')));
      }
      if (entries != n) partial = true;
    }

    ck("never writes past the buffer", !overran);
    ck("never leaves a half-written entry", !partial);
    ck("more than one identifier fits a line", totalShown >= 12,
       std::to_string(totalShown));

    /* The window rotates, so a hundred-identifier bus is fully reported over
     * a minute of once-a-second lines instead of showing the same nine for
     * ever. Twelve passes is more than enough to cover forty. */
    ck("the window covers every identifier", seen.size() == N,
       std::to_string(seen.size()) + " of " + std::to_string(N));

    /* A buffer too small for even one entry must still terminate. */
    char tiny[8];
    memset(tiny, 0x5A, sizeof(tiny));
    cur = 0;
    ck("a buffer too small fits nothing and still terminates",
       busFormatIds(b, &cur, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0');
    ck("and the cursor still moves, so it cannot pin itself", cur != 0,
       std::to_string(cur));
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
