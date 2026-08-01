/* Host-side exercise of the DBC parser and the signal decoder: feeds a small
 * frame map through dbcParseLine(), then checks that real payloads come back
 * out as the right numbers, labels and units. */
#include "dbc.h"
#include "fmt.h"
#include <string>
#include <vector>

uint32_t   g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;

static int failures = 0;

static void ck(const char *what, bool ok, const std::string &detail = "") {
  if (ok) printf("  ok   %-34s %s\n", what, detail.c_str());
  else  { printf("  FAIL %-34s %s\n", what, detail.c_str()); failures++; }
}

static void eq(const char *what, const std::string &got, const std::string &want) {
  if (got == want) printf("  ok   %-34s %s\n", what, got.c_str());
  else { printf("  FAIL %-34s got '%s'  want '%s'\n", what, got.c_str(), want.c_str());
         failures++; }
}

/* Renders a decoded signal exactly the way the CSV writer does. */
static std::string render(DbcDb &db, DbcSignal &s, const uint8_t *d, uint8_t len) {
  DbcValue v = dbcDecodeSignal(db, s, d, len);
  if (!v.ok) return "<nofit>";
  char buf[64];
  char *p = buf;
  if (v.label)       { return std::string(v.label); }
  else if (v.exact)  { p = fmtFixed(p, v.scaled, v.dec); }
  else               { p = fmtDouble(p, v.fval); }
  return std::string(buf, (size_t)(p - buf));
}

static DbcSignal *sig(DbcDb &db, const char *msg, const char *name) {
  for (uint16_t i = 0; i < db.msgCount; i++) {
    if (strcmp(db.msg[i].name, msg)) continue;
    for (uint16_t j = 0; j < db.msg[i].signalCount; j++) {
      DbcSignal &s = db.sig[db.msg[i].firstSignal + j];
      if (strcmp(s.name, name) == 0) return &s;
    }
  }
  return nullptr;
}

static void feed(DbcDb &db, const char *const *lines) {
  char buf[DBC_LINE_MAX];
  for (const char *const *l = lines; *l; l++) {
    snprintf(buf, sizeof(buf), "%s", *l);
    dbcParseLine(db, buf);
  }
}

/* A deliberately awkward little map: Intel and Motorola, signed and unsigned,
 * an offset, a value table, a multiplexor, an IEEE float and a wrapping clock. */
static const char *const MAP[] = {
  "VERSION \"unit-test map\"",
  "",
  "NS_ :",
  "    BA_DEF_",
  "BS_:",
  "BU_ NodeA Host",
  "",
  "BO_ 256 NodeStatus: 8 NodeA",
  " SG_ Uptime : 0|32@1+ (1,0) [0|4294967295] \"s\" Vector__XXX",
  " SG_ State : 32|4@1+ (1,0) [0|15] \"\" Vector__XXX",
  " SG_ Load : 40|8@1+ (0.5,0) [0|127.5] \"%\" Vector__XXX",
  "",
  "BO_ 257 MotorFeedback: 8 NodeA",
  " SG_ Speed : 0|16@1- (0.25,0) [-8192|8191.75] \"rpm\" Vector__XXX",
  " SG_ Current : 16|16@1+ (0.001,0) [0|65.535] \"A\" Vector__XXX",
  " SG_ Setpoint : 32|32@1- (1e-06,0) [0|0] \"\" Vector__XXX",
  "",
  "BO_ 258 SampleClock: 8 NodeA",
  " SG_ SampleTime : 0|32@1+ (1,0) [0|4294967295] \"us\" Vector__XXX",
  "",
  "BO_ 259 Analog: 8 NodeA",
  " SG_ Pressure : 7|16@0+ (0.5,-100) [-100|32667.5] \"kPa\" Vector__XXX",
  " SG_ Flow : 32|32@1+ (1,0) [0|0] \"l/min\" Vector__XXX",
  "",
  "BO_ 512 Diagnostics: 8 NodeA",
  " SG_ Page M : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX",
  " SG_ Temperature m0 : 8|16@1- (0.1,0) [-3276.8|3276.7] \"degC\" Vector__XXX",
  " SG_ RunHours m1 : 8|32@1+ (1,0) [0|4294967295] \"h\" Vector__XXX",
  "",
  "CM_ SG_ 256 Uptime \"seconds since the node last booted\";",
  "BA_DEF_ SG_ \"Unwrap\" INT 0 1;",
  "BA_DEF_DEF_ \"Unwrap\" 0;",
  "BA_ \"Unwrap\" SG_ 258 SampleTime 1;",
  "SIG_VALTYPE_ 259 Flow : 1;",
  "VAL_ 256 State 0 \"boot\" 1 \"ready\" 2 \"running\" 5 \"fault\" ;",
  nullptr
};

int main() {
  DbcDb db;
  dbcReset(db);
  feed(db, MAP);

  printf("\n== parsing ==\n");
  ck("5 messages", db.msgCount == 5, "got " + std::to_string(db.msgCount));
  ck("12 signals", db.sigCount == 12, "got " + std::to_string(db.sigCount));
  ck("no line errors", db.lineErrors == 0, "got " + std::to_string(db.lineErrors));
  ck("no overflow", db.overflow == 0);
  eq("VERSION kept", db.version, "unit-test map");
  ck("4 value labels", db.valCount == 4, "got " + std::to_string(db.valCount));
  ck("float signal flagged inexact", db.inexact == 1);

  const DbcMessage *m = dbcFind(db, 0x100, false);
  ck("lookup by id", m && strcmp(m->name, "NodeStatus") == 0);
  ck("unknown id -> nullptr", dbcFind(db, 0x123, false) == nullptr);
  ck("11-bit and 29-bit are distinct", dbcFind(db, 0x100, true) == nullptr);

  /* ---------------------------------------------------------------- */
  printf("\n== Intel layout, factors and offsets ==\n");
  {
    uint8_t d[8] = {0x2A, 0x00, 0x00, 0x00, 0x02, 0x64, 0x00, 0x00};
    eq("uptime, factor 1",      render(db, *sig(db,"NodeStatus","Uptime"), d, 8), "42");
    eq("nibble field + VAL_",   render(db, *sig(db,"NodeStatus","State"),  d, 8), "running");
    eq("factor 0.5 stays exact",render(db, *sig(db,"NodeStatus","Load"),   d, 8), "50.0");
  }
  {
    /* an unlabelled raw value must print as a number, not blank */
    uint8_t d[8] = {0, 0, 0, 0, 0x07, 0, 0, 0};
    eq("value with no label",   render(db, *sig(db,"NodeStatus","State"),  d, 8), "7");
  }

  printf("\n== signed values keep their sign ==\n");
  {
    uint8_t d[8] = {0x18, 0xFC, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x00};
    /* 0xFC18 as int16 = -1000, x 0.25 = -250 */
    eq("negative, factor 0.25", render(db, *sig(db,"MotorFeedback","Speed"), d, 8), "-250.00");
    eq("milli-unit unsigned",   render(db, *sig(db,"MotorFeedback","Current"), d, 8), "1.000");
  }
  {
    uint8_t d[8] = {0, 0, 0, 0, 0x2E, 0xFB, 0xFF, 0xFF};   /* -1234 */
    eq("1e-06 keeps its sign",  render(db, *sig(db,"MotorFeedback","Setpoint"), d, 8),
       "-0.001234");
  }
  {
    uint8_t d[8] = {0, 0, 0, 0, 0xA0, 0x25, 0x26, 0x00};   /* 2500000 */
    eq("1e-06 positive",        render(db, *sig(db,"MotorFeedback","Setpoint"), d, 8),
       "2.500000");
  }

  printf("\n== Motorola (big-endian) layout ==\n");
  {
    /* start bit 7, 16 bits big-endian => bytes 0..1 MSB first. 0x0100 = 256,
     * x 0.5 = 128, minus the offset of 100 => 28 */
    uint8_t d[8] = {0x01, 0x00, 0, 0, 0, 0, 0, 0};
    eq("factor and offset together", render(db, *sig(db,"Analog","Pressure"), d, 8), "28.0");
  }
  {
    uint8_t d[8] = {0x00, 0x00, 0, 0, 0, 0, 0, 0};
    eq("offset applies at zero", render(db, *sig(db,"Analog","Pressure"), d, 8), "-100.0");
  }
  {
    bool fits = false;
    const uint64_t v = dbcExtractBits((const uint8_t *)"\x12\x34", 2, 7, 16, false, &fits);
    ck("extractBits big-endian", fits && v == 0x1234,
       "got " + std::to_string(v) + " want 4660");
    const uint64_t w = dbcExtractBits((const uint8_t *)"\x12\x34", 2, 0, 16, true, &fits);
    ck("extractBits little-endian", fits && w == 0x3412,
       "got " + std::to_string(w) + " want 13330");
  }

  printf("\n== a signal that does not fit the payload is skipped ==\n");
  {
    uint8_t d[4] = {1, 2, 3, 4};
    eq("32-bit signal, 4-byte frame", render(db, *sig(db,"SampleClock","SampleTime"), d, 4),
       "67305985");
    eq("64-bit-wide read, 2-byte frame",
       render(db, *sig(db,"MotorFeedback","Setpoint"), d, 2), "<nofit>");
  }

  printf("\n== IEEE float payloads (SIG_VALTYPE_) ==\n");
  {
    uint8_t d[8] = {0, 0, 0, 0, 0x00, 0x00, 0x48, 0x42};   /* 50.0f */
    eq("float32 decoded", render(db, *sig(db,"Analog","Flow"), d, 8), "50");
  }

  printf("\n== multiplexing ==\n");
  {
    DbcMessage *dm = nullptr;
    for (uint16_t i = 0; i < db.msgCount; i++)
      if (strcmp(db.msg[i].name, "Diagnostics") == 0) dm = &db.msg[i];
    ck("multiplexor recorded", dm && dm->muxSignal >= 0);
    ck("m0 signal tagged",  sig(db,"Diagnostics","Temperature")->muxValue == 0);
    ck("m1 signal tagged",  sig(db,"Diagnostics","RunHours")->muxValue == 1);
    ck("multiplexor tagged", sig(db,"Diagnostics","Page")->muxValue == -2);

    uint8_t d[8] = {0x00, 0x2C, 0x01, 0, 0, 0, 0, 0};      /* page 0, 30.0 degC */
    eq("page 0 temperature", render(db, *sig(db,"Diagnostics","Temperature"), d, 8), "30.0");
  }

  printf("\n== a free-running counter is unwrapped (BA_ \"Unwrap\") ==\n");
  {
    DbcSignal &s = *sig(db, "SampleClock", "SampleTime");
    ck("attribute applied", s.unwrap == 1);

    uint8_t a[8] = {0x00, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};   /* 0xFFFFFF00 */
    uint8_t b[8] = {0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0};   /* wrapped to 0    */
    uint8_t c[8] = {0xE8, 0x03, 0x00, 0x00, 0, 0, 0, 0};   /* 1000 after that */
    eq("before the wrap", render(db, s, a, 8), "4294967040");
    eq("across the wrap", render(db, s, b, 8), "4294967296");
    eq("after the wrap",  render(db, s, c, 8), "4294968296");

    dbcResetRuntime(db);
    eq("runtime reset rebases", render(db, s, c, 8), "1000");
  }

  printf("\n== junk is survivable ==\n");
  {
    DbcDb j;
    dbcReset(j);
    char buf[DBC_LINE_MAX];
    const char *bad[] = {
      "SG_ Orphan : 0|8@1+ (1,0) [0|0] \"\" X",     /* SG_ with no BO_ */
      "BO_ notanumber Broken: 8 X",
      "VAL_ 999 Nope 0 \"x\" ;",
      "BA_ \"Unwrap\" SG_ 999 Nope 1;",
      "random text that is not DBC at all",
      nullptr
    };
    for (const char **l = bad; *l; l++) {
      snprintf(buf, sizeof(buf), "%s", *l);
      dbcParseLine(j, buf);
    }
    ck("nothing was loaded", j.loaded == 0 && j.msgCount == 0);
    ck("bad lines are counted", j.lineErrors == 4,
       "got " + std::to_string(j.lineErrors));
  }

  printf("\n== the table cannot be overrun ==\n");
  {
    DbcDb o;
    dbcReset(o);
    char buf[DBC_LINE_MAX];
    for (int i = 0; i < DBC_MAX_MESSAGES + 20; i++) {
      snprintf(buf, sizeof(buf), "BO_ %d Msg%d: 8 N", 1000 + i, i);
      dbcParseLine(o, buf);
    }
    ck("message table clamps", o.msgCount == DBC_MAX_MESSAGES,
       "got " + std::to_string(o.msgCount));
    ck("overflow is reported", o.overflow == 1);
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
