/* Host-side exercise of the transmit-side encoder.
 *
 * The one property that matters here is that encoding is the exact inverse of
 * decoding. The logger reads a bus for a living; the moment it also writes to
 * one, a signal that encodes half a bit low stops being a wrong number on a
 * screen and starts being a machine set to the wrong value. So the central
 * test is not a handful of examples: it is a sweep over every signal in a
 * deliberately awkward frame map, every raw value a signal can hold (or a
 * sample of them for the wide ones), asserting decode(encode(x)) == x.
 *
 * It also checks the things that are only visible from the writing side:
 * neighbouring bits must survive untouched, out-of-range values must clamp
 * rather than wrap, and a signal that does not fit the payload must write
 * nothing at all. */
#include "dbc.h"
#include <math.h>
#include <string>
#include <vector>

uint32_t   g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;

static int failures = 0;

static void ck(const char *what, bool ok, const std::string &detail = "") {
  if (ok) printf("  ok   %-40s %s\n", what, detail.c_str());
  else  { printf("  FAIL %-40s %s\n", what, detail.c_str()); failures++; }
}

/* Loads the map the way the firmware does: count, size the tables to the file,
 * then parse. Feeding dbcParseLine() with no tables allocated parses nothing. */
static void feed(DbcDb &db, const char *const *lines) {
  std::string text;
  for (const char *const *l = lines; *l; l++) { text += *l; text += '\n'; }
  dbcLoadText(db, text.c_str(), text.size());
}

/* Intel and Motorola, signed and unsigned, awkward widths that straddle byte
 * boundaries, a negative factor, a large offset, a value table and a range. */
static const char *const MAP[] = {
  "VERSION \"encoder unit-test map\"",
  "BO_ 396 WheelInfo: 8 Vehicle",
  " SG_ TireSize : 0|16@1+ (1,0) [400|1200] \"mm\" ECU",
  " SG_ Pressure : 16|10@1+ (0.1,0) [0|102.3] \"bar\" ECU",
  " SG_ Temp : 26|9@1- (0.5,-40) [-60|100] \"degC\" ECU",
  " SG_ Flags : 35|5@1+ (1,0) [0|31] \"\" ECU",
  "BO_ 512 Steering: 8 Vehicle",
  " SG_ Angle : 7|16@0- (0.01,0) [-45|45] \"deg\" ECU",
  " SG_ Rate : 23|12@0- (0.25,0) [-500|500] \"deg/s\" ECU",
  " SG_ Mode : 27|3@0+ (1,0) [0|7] \"\" ECU",
  "BO_ 700 Drive: 8 Vehicle",
  " SG_ Speed : 0|12@1+ (0.0625,0) [0|255] \"km/h\" ECU",
  " SG_ Reverse : 12|1@1+ (1,0) [0|1] \"\" ECU",
  " SG_ Torque : 16|16@1- (0.5,-16384) [-16384|16383] \"Nm\" ECU",
  " SG_ Countdown : 32|8@1+ (-1,255) [0|255] \"s\" ECU",
  "VAL_ 700 Reverse 0 \"forward\" 1 \"reverse\" ;",
  nullptr
};

static DbcSignal *sig(DbcDb &db, const char *ref) {
  const int16_t i = dbcFindSignalRef(db, ref, nullptr);
  return i < 0 ? nullptr : &db.sig[i];
}

/* The physical value a raw value decodes to, as a double, however it scaled. */
static double physOf(DbcDb &db, DbcSignal &s, const uint8_t *d, uint8_t len) {
  DbcValue v = dbcDecodeSignal(db, s, d, len);
  if (!v.ok) return NAN;
  return v.exact ? (double)v.scaled / pow(10.0, (double)v.dec) : v.fval;
}

int main() {
  DbcDb db;
  dbcReset(db);
  feed(db, MAP);

  printf("== the map itself ==\n");
  ck("map parsed", db.loaded && db.msgCount == 3 && db.sigCount == 11,
     std::to_string(db.msgCount) + " messages, " +
     std::to_string(db.sigCount) + " signals");
  ck("no line errors", db.lineErrors == 0,
     "got " + std::to_string(db.lineErrors));

  printf("\n== [min|max] is read off the file ==\n");
  {
    DbcSignal *t = sig(db, "WheelInfo.TireSize");
    ck("TireSize range", t && t->hasRange &&
       fabs(t->phyMin - 400.0) < 1e-6 && fabs(t->phyMax - 1200.0) < 1e-6,
       t ? std::to_string(t->phyMin) + " .. " + std::to_string(t->phyMax) : "missing");

    DbcSignal *a = sig(db, "Steering.Angle");
    ck("Angle range is signed", a && a->hasRange && a->phyMin < 0,
       a ? std::to_string(a->phyMin) + " .. " + std::to_string(a->phyMax) : "missing");

    DbcSignal *f = sig(db, "WheelInfo.Flags");
    ck("an empty unit is not an error", f && f->unit[0] == '\0');
  }

  printf("\n== what the bits can actually hold ==\n");
  {
    double lo = 0, hi = 0;
    dbcSignalLimits(*sig(db, "WheelInfo.TireSize"), &lo, &hi);
    ck("16-bit unsigned, factor 1", lo == 0.0 && hi == 65535.0,
       std::to_string(lo) + " .. " + std::to_string(hi));

    dbcSignalLimits(*sig(db, "WheelInfo.Temp"), &lo, &hi);
    ck("9-bit signed, factor .5 offset -40",
       fabs(lo - (-168.0)) < 1e-9 && fabs(hi - 87.5) < 1e-9,
       std::to_string(lo) + " .. " + std::to_string(hi));

    /* A negative factor makes the raw minimum the physical maximum. The
     * limits have to come back the right way round or a gauge draws inside
     * out and a clamp rejects every legal value. */
    dbcSignalLimits(*sig(db, "Drive.Countdown"), &lo, &hi);
    ck("a negative factor does not invert the range", lo < hi,
       std::to_string(lo) + " .. " + std::to_string(hi));
    ck("negative factor limits are right",
       fabs(lo - 0.0) < 1e-9 && fabs(hi - 255.0) < 1e-9,
       std::to_string(lo) + " .. " + std::to_string(hi));
  }

  printf("\n== decode(encode(x)) == x, over every raw value ==\n");
  {
    /* The sweep. For each signal, walk the raw values it can hold - all of
     * them when there are few, a spread including both ends when there are
     * many - encode the physical value they decode to, and require the raw
     * that comes back to be identical. */
    int swept = 0, bad = 0;
    std::string firstBad;

    for (uint16_t mi = 0; mi < db.msgCount; mi++) {
      const DbcMessage &m = db.msg[mi];
      for (uint16_t k = 0; k < m.signalCount; k++) {
        DbcSignal &s = db.sig[m.firstSignal + k];

        int64_t rlo, rhi;
        if (s.isSigned) {
          rhi =  (int64_t)((1ULL << (s.bits - 1)) - 1ULL);
          rlo = -(int64_t)(1ULL << (s.bits - 1));
        } else {
          rlo = 0; rhi = (int64_t)((1ULL << s.bits) - 1ULL);
        }

        const int64_t span = rhi - rlo;
        const int64_t step = span > 512 ? span / 512 : 1;

        for (int64_t raw = rlo; raw <= rhi; raw += step) {
          /* Put the raw value on the wire, decode it, re-encode what came
           * out, and compare against the payload we started from. */
          uint8_t a[8] = {0}, b[8] = {0};
          if (!dbcInsertBits(a, 8, s.startBit, s.bits, s.intel != 0,
                             (uint64_t)raw)) { bad++; continue; }

          const double phys = physOf(db, s, a, 8);
          DbcEncoded e = dbcEncodeSignal(s, phys, b, 8);
          swept++;

          if (!e.ok || e.clamped || memcmp(a, b, 8) != 0) {
            bad++;
            if (firstBad.empty()) {
              firstBad = std::string(m.name) + "." + s.name +
                         " raw " + std::to_string(raw) +
                         " phys " + std::to_string(phys) +
                         " -> raw " + std::to_string(e.raw);
            }
          }
        }
      }
    }
    ck("every raw value survives the round trip", bad == 0,
       std::to_string(swept) + " values swept" +
       (firstBad.empty() ? "" : ", first failure: " + firstBad));
  }

  printf("\n== a signal only touches its own bits ==\n");
  {
    DbcSignal *pressure = sig(db, "WheelInfo.Pressure");
    uint8_t d[8];
    memset(d, 0xA5, sizeof(d));
    uint8_t before[8];
    memcpy(before, d, sizeof(d));

    DbcEncoded e = dbcEncodeSignal(*pressure, 3.4, d, 8);
    ck("encode reports success", e.ok && !e.clamped,
       "raw " + std::to_string(e.raw) + ", applied " + std::to_string(e.applied));
    ck("it reads back as what went in",
       fabs(physOf(db, *pressure, d, 8) - 3.4) < 1e-9,
       std::to_string(physOf(db, *pressure, d, 8)));

    /* Pressure is bits 16..25. Bytes 0, 1, 4, 5, 6, 7 must be untouched, and
     * the bits of byte 3 above the signal must be too. */
    ck("bytes below the signal untouched",
       d[0] == before[0] && d[1] == before[1]);
    ck("bytes above the signal untouched",
       d[4] == before[4] && d[5] == before[5] &&
       d[6] == before[6] && d[7] == before[7]);
    ck("neighbouring bits in a shared byte untouched",
       (d[3] & 0xFC) == (before[3] & 0xFC));
  }

  printf("\n== Motorola signals encode where they decode ==\n");
  {
    DbcSignal *angle = sig(db, "Steering.Angle");
    for (double want : { -45.0, -0.01, 0.0, 0.01, 12.34, 45.0 }) {
      uint8_t d[8] = {0};
      DbcEncoded e = dbcEncodeSignal(*angle, want, d, 8);
      const double got = physOf(db, *angle, d, 8);
      ck("Motorola round trip", e.ok && !e.clamped && fabs(got - want) < 1e-9,
         std::to_string(want) + " -> " + std::to_string(got));
    }
  }

  printf("\n== out of range clamps, and says so ==\n");
  {
    DbcSignal *t = sig(db, "WheelInfo.TireSize");
    uint8_t d[8] = {0};

    /* 70000 mm does not fit 16 bits. Wrapping it would put 4464 on the wire
     * and nobody would ever know; clamping and reporting is the only honest
     * answer available. */
    DbcEncoded e = dbcEncodeSignal(*t, 70000.0, d, 8);
    ck("over range is clamped", e.ok && e.clamped && e.raw == 65535,
       "raw " + std::to_string(e.raw));
    ck("it did not wrap", physOf(db, *t, d, 8) == 65535.0,
       std::to_string(physOf(db, *t, d, 8)));

    DbcEncoded u = dbcEncodeSignal(*t, -5.0, d, 8);
    ck("under range is clamped to zero", u.ok && u.clamped && u.raw == 0,
       "raw " + std::to_string(u.raw));

    DbcEncoded ok = dbcEncodeSignal(*t, 690.0, d, 8);
    ck("an in-range value is not flagged", ok.ok && !ok.clamped && ok.raw == 690,
       "raw " + std::to_string(ok.raw));
  }

  printf("\n== rounding goes to nearest, not towards zero ==\n");
  {
    /* Pressure has factor 0.1. Truncation would put 3.4 bar on the wire as 33
     * rather than 34 - a whole step low, on every single value that is not an
     * exact multiple of the factor. */
    DbcSignal *p = sig(db, "WheelInfo.Pressure");
    uint8_t d[8] = {0};
    ck("0.1 factor, positive", dbcEncodeSignal(*p, 3.4, d, 8).raw == 34);
    ck("0.1 factor, halfway rounds up", dbcEncodeSignal(*p, 0.05, d, 8).raw == 1);

    /* Temp has factor 0.5 and offset -40, so negative physical values map to
     * small positive raws; the rounding must not flip sign on the way. */
    DbcSignal *t = sig(db, "WheelInfo.Temp");
    ck("offset signal, negative physical",
       dbcEncodeSignal(*t, -37.5, d, 8).raw == 5,
       "raw " + std::to_string(dbcEncodeSignal(*t, -37.5, d, 8).raw));

    /* Torque is signed with a large negative offset: raw 0 is -16384 Nm. */
    DbcSignal *q = sig(db, "Drive.Torque");
    ck("signed with a large offset",
       dbcEncodeSignal(*q, -16384.0, d, 8).raw == 0,
       "raw " + std::to_string(dbcEncodeSignal(*q, -16384.0, d, 8).raw));
    /* -16383.1 is 1.8 steps above the bottom of the range. Truncating towards
     * zero would put 1 on the wire; only rounding to nearest gives 2. */
    ck("negative rounding does not truncate towards zero",
       dbcEncodeSignal(*q, -16383.1, d, 8).raw == 2,
       "raw " + std::to_string(dbcEncodeSignal(*q, -16383.1, d, 8).raw));

    /* Same again where the quotient itself is negative: Rate is signed with a
     * 0.25 factor, so -0.4 is -1.6 steps. Truncation gives -1, nearest -2. */
    DbcSignal *r = sig(db, "Steering.Rate");
    ck("a negative quotient rounds to nearest too",
       dbcEncodeSignal(*r, -0.4, d, 8).raw == -2,
       "raw " + std::to_string(dbcEncodeSignal(*r, -0.4, d, 8).raw));
  }

  printf("\n== a signal that does not fit writes nothing ==\n");
  {
    DbcSignal *q = sig(db, "Drive.Torque");     /* bits 16..31 */
    uint8_t d[2] = {0x11, 0x22};
    DbcEncoded e = dbcEncodeSignal(*q, 100.0, d, 2);
    ck("encode refuses", !e.ok);
    ck("payload is untouched", d[0] == 0x11 && d[1] == 0x22);

    ck("dbcInsertBits refuses too",
       !dbcInsertBits(d, 2, q->startBit, q->bits, q->intel != 0, 0xFFFF));
    ck("still untouched", d[0] == 0x11 && d[1] == 0x22);
  }

  printf("\n== Message.Signal references ==\n");
  {
    int16_t mi = -1;
    const int16_t si = dbcFindSignalRef(db, "Steering.Angle", &mi);
    ck("resolves a reference", si >= 0 && mi >= 0 &&
       strcmp(db.sig[si].name, "Angle") == 0 &&
       strcmp(db.msg[mi].name, "Steering") == 0);

    char back[64];
    const bool wrote = dbcSignalRef(db, (uint16_t)si, back, sizeof(back));
    ck("and writes it back out",
       wrote && strcmp(back, "Steering.Angle") == 0, back);

    ck("an unknown message is not found",
       dbcFindSignalRef(db, "Nope.Angle", nullptr) < 0);
    ck("an unknown signal is not found",
       dbcFindSignalRef(db, "Steering.Nope", nullptr) < 0);
    ck("a reference with no dot is not found",
       dbcFindSignalRef(db, "Steering", nullptr) < 0);
    ck("an empty reference is not found",
       dbcFindSignalRef(db, "", nullptr) < 0);

    /* A short buffer must fail rather than write a truncated reference that
     * would then resolve to the wrong signal. */
    char tiny[6];
    ck("a short buffer is refused",
       !dbcSignalRef(db, (uint16_t)si, tiny, sizeof(tiny)) && tiny[0] == '\0');
  }

  /* ----------------------------------------------------------------------
   *  A multiplexed command frame, which is the case a per-signal transmit
   *  gets silently wrong. In examples/example.dbc, HostCommand carries
   *  Command as the selector, Setpoint under code 16 and WheelDia_mm under
   *  code 32. Writing WheelDia_mm alone arrives as Command 0 and is thrown
   *  away by the ECU, so cantx.cpp writes the selector from the signal's own
   *  mux code. This asserts the mechanism that does it.
   * -------------------------------------------------------------------- */
  printf("\n== a multiplexed command frame ==\n");
  {
    DbcDb mx = {};
    FILE *fp = fopen("examples/example.dbc", "r");
    ck("the example frame map opens", fp != nullptr);
    if (fp) {
      std::string text;
      char line[DBC_LINE_MAX];
      while (fgets(line, sizeof(line), fp)) text += line;
      fclose(fp);
      dbcLoadText(mx, text.c_str(), text.size());
    }

    int16_t hm = -1;
    const int16_t wheel = dbcFindSignalRef(mx, "HostCommand.WheelDia_mm", &hm);
    ck("the multiplexed payload resolves", wheel >= 0 && hm >= 0);

    if (wheel >= 0 && hm >= 0) {
      const DbcMessage &m  = mx.msg[hm];
      const DbcSignal  &sg = mx.sig[wheel];

      ck("the message knows its selector", m.muxSignal >= 0);
      ck("the payload knows its code", sg.muxValue == 32);

      uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};

      /* Exactly what buildSignalFrame() does: the selector as raw bits, then
       * the payload through its scaling. */
      const DbcSignal &sel = mx.sig[m.muxSignal];
      ck("the selector is written",
         dbcInsertBits(data, 8, sel.startBit, sel.bits, sel.intel != 0,
                       (uint64_t)sg.muxValue));

      DbcEncoded e = dbcEncodeSignal(sg, 1380.0, data, 8);
      ck("the payload is written", e.ok && !e.clamped);

      /* Now read the frame back the way the ECU would. */
      ck("the selector reads back as 32",
         dbcExtractBits(data, 8, sel.startBit, sel.bits, sel.intel != 0, nullptr) == 32);

      const uint64_t raw = dbcExtractBits(data, 8, sg.startBit, sg.bits,
                                          sg.intel != 0, nullptr);
      ck("the diameter reads back as 1380", raw == 1380,
         std::to_string((unsigned long long)raw));

      /* And the other page's payload must not be sitting in the frame. */
      DbcEncoded e2 = dbcEncodeSignal(sg, 300.0, data, 8);
      ck("the low end fits", e2.ok && !e2.clamped);
      ck("the selector survives the second write",
         dbcExtractBits(data, 8, sel.startBit, sel.bits, sel.intel != 0, nullptr) == 32);

      /* Beyond what the bits can hold it clamps and says so, rather than
       * wrapping round to a small number that looks plausible. The [300|3000]
       * annotation is enforced a layer up, in txSendCommand(), which refuses
       * rather than clamps - by the time a value reaches here the only limit
       * left is physical. */
      double blo = 0, bhi = 0;
      dbcSignalLimits(sg, &blo, &bhi);
      DbcEncoded e3 = dbcEncodeSignal(sg, bhi + 1000.0, data, 8);
      ck("past the bit limit clamps and is flagged",
         e3.ok && e3.clamped && e3.applied <= bhi + 1e-9,
         std::to_string(e3.applied));
      ck("and does not wrap round",
         dbcExtractBits(data, 8, sg.startBit, sg.bits, sg.intel != 0, nullptr)
           == (uint64_t)((1u << sg.bits) - 1));
    }
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
