#include "dbc.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Tables are allocated at load time, so the object itself is just pointers
 * and counters - a few dozen bytes rather than thirty-two kilobytes. */
DbcDb g_dbc = {};

/* ==========================================================================
 *  Small text helpers. Everything here works on a mutable line buffer and
 *  allocates nothing - the parser runs on the ESP32 with a 256-byte line.
 * ======================================================================== */
static inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *skipSpace(char *p) {
  while (*p && isSpace(*p)) p++;
  return p;
}

/* Copies the next whitespace-delimited token, advancing *pp past it. */
static bool nextToken(char **pp, char *out, size_t cap) {
  char *p = skipSpace(*pp);
  if (!*p) { *pp = p; if (cap) out[0] = '\0'; return false; }

  size_t n = 0;
  while (*p && !isSpace(*p)) {
    if (n + 1 < cap) out[n] = *p;
    n++;
    p++;
  }
  out[(n + 1 < cap) ? n : (cap ? cap - 1 : 0)] = '\0';
  *pp = p;
  return true;
}

static void copyBounded(char *dst, size_t cap, const char *src, size_t n) {
  if (!cap) return;
  if (n > cap - 1) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

/* Reads a "quoted string" starting at the next '"'. Returns the character
 * after the closing quote, or nullptr. */
static char *readQuoted(char *p, char *out, size_t cap) {
  while (*p && *p != '"') p++;
  if (!*p) return nullptr;
  p++;
  const char *start = p;
  while (*p && *p != '"') p++;
  if (!*p) return nullptr;
  copyBounded(out, cap, start, (size_t)(p - start));
  return p + 1;
}

/* ==========================================================================
 *  Decimal numbers without floating point.
 *
 *  A DBC factor is almost always a decimal literal - 1, 0.001, 1e-06, 2.5.
 *  Expressed as an integer numerator over a power of ten, the whole scaling
 *  chain stays in integer arithmetic and the printed value is exact. Anything
 *  that does not fit that shape (1/3, absurd exponents) is flagged and handled
 *  in double instead.
 * ======================================================================== */
struct DecNum {
  int64_t num;
  uint8_t dec;
  bool    ok;
  double  dv;
};

static const int64_t POW10_64[19] = {
  1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL,
  100000000LL, 1000000000LL, 10000000000LL, 100000000000LL,
  1000000000000LL, 10000000000000LL, 100000000000000LL,
  1000000000000000LL, 10000000000000000LL, 100000000000000000LL,
  1000000000000000000LL
};

static DecNum parseDecimal(const char *s, size_t len) {
  DecNum r; r.num = 0; r.dec = 0; r.ok = false; r.dv = 0.0;

  char buf[40];
  copyBounded(buf, sizeof(buf), s, len);
  r.dv = strtod(buf, nullptr);

  const char *p = buf;
  while (*p && isSpace(*p)) p++;

  bool neg = false;
  if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

  int64_t mant  = 0;
  int     digits = 0;
  int     frac   = 0;
  bool    seen   = false;

  while (*p >= '0' && *p <= '9') {
    if (digits < 18) { mant = mant * 10 + (*p - '0'); digits++; }
    else return r;                       /* far more precision than we need */
    seen = true;
    p++;
  }
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') {
      if (digits < 18) { mant = mant * 10 + (*p - '0'); digits++; frac++; }
      else return r;
      seen = true;
      p++;
    }
  }
  if (!seen) return r;

  int exp = 0;
  if (*p == 'e' || *p == 'E') {
    p++;
    bool eneg = false;
    if (*p == '+' || *p == '-') { eneg = (*p == '-'); p++; }
    if (!(*p >= '0' && *p <= '9')) return r;
    while (*p >= '0' && *p <= '9') {
      exp = exp * 10 + (*p - '0');
      if (exp > 30) return r;
      p++;
    }
    if (eneg) exp = -exp;
  }
  while (*p && isSpace(*p)) p++;
  if (*p) return r;                      /* trailing junk - not a number    */

  /* Fold the exponent into the numerator / decimal count. */
  int dec = frac;
  while (exp > 0 && dec > 0) { dec--; exp--; }
  while (exp > 0) {
    if (mant > 100000000000000000LL) return r;
    mant *= 10; exp--;
  }
  if (exp < 0) dec += -exp;

  while (dec > 0 && mant != 0 && (mant % 10) == 0) { mant /= 10; dec--; }
  if (mant == 0) dec = 0;

  if (dec > 9) return r;                 /* keeps raw*num inside int64      */
  if (mant > 1000000000LL) return r;

  r.num = neg ? -mant : mant;
  r.dec = (uint8_t)dec;
  r.ok  = true;
  return r;
}

/* ==========================================================================
 *  Bit extraction
 * ======================================================================== */
uint64_t dbcExtractBits(const uint8_t *data, uint8_t len,
                        uint8_t startBit, uint8_t bits, bool intel, bool *fits) {
  if (fits) *fits = false;
  if (bits == 0 || bits > 64) return 0;

  const uint16_t nbits = (uint16_t)len * 8u;
  uint64_t raw = 0;

  if (intel) {
    /* Intel: startBit is the least significant bit, numbering runs upward. */
    if ((uint16_t)startBit + bits > nbits) return 0;
    for (uint8_t i = 0; i < bits; i++) {
      const uint16_t b = (uint16_t)startBit + i;
      raw |= (uint64_t)((data[b >> 3] >> (b & 7)) & 1u) << i;
    }
  } else {
    /* Motorola: startBit is the most significant bit; the walk steps down
     * inside a byte and jumps to bit 7 of the next byte at each boundary. */
    int b = startBit;
    for (uint8_t i = 0; i < bits; i++) {
      if (b < 0 || (uint16_t)b >= nbits) return 0;
      raw = (raw << 1) | (uint64_t)((data[b >> 3] >> (b & 7)) & 1u);
      if ((b & 7) == 0) b += 15; else b -= 1;
    }
  }

  if (fits) *fits = true;
  return raw;
}

/* ==========================================================================
 *  Database management
 * ======================================================================== */
void dbcFree(DbcDb &db) {
  free(db.msg); free(db.sig); free(db.val);
  db.msg = nullptr; db.sig = nullptr; db.val = nullptr;
  db.msgCap = db.sigCap = db.valCap = 0;
  db.msgCount = db.sigCount = db.valCount = 0;
}

size_t dbcBytes(const DbcDb &db) {
  return (size_t)db.msgCap * sizeof(DbcMessage)
       + (size_t)db.sigCap * sizeof(DbcSignal)
       + (size_t)db.valCap * sizeof(DbcValDesc);
}

bool dbcAllocate(DbcDb &db, const DbcCounts &want) {
  dbcFree(db);

  uint16_t m = want.messages, g = want.signals, v = want.values;
  if (m > DBC_MAX_MESSAGES) { m = DBC_MAX_MESSAGES; db.overflow = 1; }
  if (g > DBC_MAX_SIGNALS)  { g = DBC_MAX_SIGNALS;  db.overflow = 1; }
  if (v > DBC_MAX_VALDESC)  { v = DBC_MAX_VALDESC;  db.overflow = 1; }

  /* Try the whole thing, then progressively less. A map half its intended size
   * still decodes half the bus, which is worth more than refusing outright -
   * and is what this firmware did unconditionally until now. */
  for (int attempt = 0; attempt < 5; attempt++) {
    if (m == 0 && g == 0 && v == 0) break;

    DbcMessage *pm = m ? (DbcMessage *)calloc(m, sizeof(DbcMessage)) : nullptr;
    DbcSignal  *pg = g ? (DbcSignal  *)calloc(g, sizeof(DbcSignal))  : nullptr;
    DbcValDesc *pv = v ? (DbcValDesc *)calloc(v, sizeof(DbcValDesc)) : nullptr;

    if ((!m || pm) && (!g || pg) && (!v || pv)) {
      db.msg = pm; db.sig = pg; db.val = pv;
      db.msgCap = m; db.sigCap = g; db.valCap = v;
      return !db.overflow;
    }

    free(pm); free(pg); free(pv);
    db.overflow = 1;
    m = (uint16_t)(m / 2); g = (uint16_t)(g / 2); v = (uint16_t)(v / 2);
  }

  db.overflow = 1;
  return false;
}

/* Counts generously: a line that looks like a definition is one, and every
 * quoted string on a VAL_ line is a label. Over-counting costs a few bytes,
 * under-counting silently drops the tail of the map. */
void dbcCountLine(const char *line, DbcCounts &c) {
  if (!line) return;
  const char *p = line;
  while (*p == ' ' || *p == '\t') p++;

  if (!strncmp(p, "BO_ ", 4))  { if (c.messages < 0xFFFF) c.messages++; return; }
  if (!strncmp(p, "SG_ ", 4))  { if (c.signals  < 0xFFFF) c.signals++;  return; }
  if (!strncmp(p, "VAL_ ", 5)) {
    for (const char *q = p; *q; q++) {
      if (*q == '"') {
        if (c.values < 0xFFFF) c.values++;
        q++;
        while (*q && *q != '"') q++;
        if (!*q) break;
      }
    }
  }
}

uint16_t dbcLoadText(DbcDb &db, const char *text, size_t len) {
  if (!text) return 0;

  char line[DBC_LINE_MAX];
  DbcCounts want = {0, 0, 0};

  /* Pass one: how big does this file need the tables to be? */
  for (size_t i = 0; i < len; ) {
    size_t n = 0;
    while (i < len && text[i] != '\n') {
      if (n + 1 < sizeof(line)) line[n++] = text[i];
      i++;
    }
    if (i < len) i++;
    line[n] = '\0';
    dbcCountLine(line, want);
  }

  if (want.messages < 0xFFF0) want.messages = (uint16_t)(want.messages + 4);
  if (want.signals  < 0xFFF0) want.signals  = (uint16_t)(want.signals  + 8);
  if (want.values   < 0xFFF0) want.values   = (uint16_t)(want.values   + 8);

  /* Reset BEFORE allocating: dbcReset() clears `overflow`, and allocating is
     one of the two things that can set it. */
  dbcReset(db);
  dbcAllocate(db, want);

  /* Pass two: parse. */
  for (size_t i = 0; i < len; ) {
    size_t n = 0;
    while (i < len && text[i] != '\n') {
      if (n + 1 < sizeof(line)) line[n++] = text[i];
      i++;
    }
    if (i < len) i++;
    line[n] = '\0';
    dbcParseLine(db, line);
  }
  return db.lineErrors;
}

void dbcReset(DbcDb &db) {
  db.msgCount   = 0;
  db.sigCount   = 0;
  db.valCount   = 0;
  db.version[0] = '\0';
  db.lineErrors = 0;
  db.nameClipped = 0;
  db.overflow   = 0;
  db.loaded     = 0;
  db.inexact    = 0;
  db.nodeCount  = 0;
}

/* Nodes are interned: the table holds each name once and messages point at it
 * by index, which costs one byte per message instead of a name per message. */
static int8_t internNode(DbcDb &db, const char *name) {
  if (!name || !*name) return -1;
  /* Vector__XXX is the exporter's way of writing "nobody in particular". */
  if (strcmp(name, "Vector__XXX") == 0) return -1;

  for (uint8_t i = 0; i < db.nodeCount; i++) {
    if (strcmp(db.node[i], name) == 0) return (int8_t)i;
  }
  if (db.nodeCount >= DBC_MAX_NODES) return -1;
  copyBounded(db.node[db.nodeCount], DBC_NAME_MAX, name, strlen(name));
  return (int8_t)db.nodeCount++;
}

const char *dbcTxNode(const DbcDb &db, const DbcMessage &m) {
  if (m.txNode < 0 || (uint8_t)m.txNode >= db.nodeCount) return "";
  return db.node[m.txNode];
}

/* BU_: NodeA NodeB      (some exporters omit the colon) */
static bool parseBu(DbcDb &db, char *p) {
  char tok[DBC_NAME_MAX + 8];
  p = skipSpace(p);
  if (*p == ':') p++;
  while (nextToken(&p, tok, sizeof(tok))) {
    size_t n = strlen(tok);
    if (n && tok[n - 1] == ':') tok[--n] = '\0';
    if (n) internNode(db, tok);
  }
  return true;
}

void dbcResetRuntime(DbcDb &db) {
  for (uint16_t i = 0; i < db.sigCount; i++) {
    db.sig[i].unwrapLast  = 0;
    db.sig[i].unwrapCount = 0;
  }
}

const DbcMessage *dbcFind(const DbcDb &db, uint32_t id, bool ext) {
  for (uint16_t i = 0; i < db.msgCount; i++) {
    if (db.msg[i].id == id && (bool)db.msg[i].ext == ext) return &db.msg[i];
  }
  return nullptr;
}

/* Index of a message by its numeric id, for VAL_ / BA_ / SIG_VALTYPE_ lines
 * that refer back to it. */
static int16_t findMsgByRawId(const DbcDb &db, uint32_t rawId) {
  const uint32_t id  = rawId & 0x1FFFFFFFu;
  const uint8_t  ext = (rawId & 0x80000000u) ? 1 : 0;
  for (uint16_t i = 0; i < db.msgCount; i++) {
    if (db.msg[i].id == id && db.msg[i].ext == ext) return (int16_t)i;
  }
  /* Some tools omit the extended flag on VAL_ lines; fall back to id only. */
  for (uint16_t i = 0; i < db.msgCount; i++) {
    if (db.msg[i].id == id) return (int16_t)i;
  }
  return -1;
}

static int16_t findSignal(const DbcDb &db, int16_t msgIdx, const char *name) {
  if (msgIdx < 0) return -1;
  const DbcMessage &m = db.msg[msgIdx];
  for (uint16_t i = 0; i < m.signalCount; i++) {
    const uint16_t s = m.firstSignal + i;
    if (strcmp(db.sig[s].name, name) == 0) return (int16_t)s;
  }
  return -1;
}

/* ==========================================================================
 *  Line parsers
 * ======================================================================== */
static bool parseBo(DbcDb &db, char *p) {
  char tok[40];

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  char *endp = nullptr;
  const uint32_t rawId = (uint32_t)strtoul(tok, &endp, 10);
  if (endp == tok) return false;

  char name[DBC_NAME_MAX + 8];
  if (!nextToken(&p, name, sizeof(name))) return false;
  size_t nl = strlen(name);
  if (nl && name[nl - 1] == ':') name[--nl] = '\0';   /* "Name:" */

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  const uint8_t dlc = (uint8_t)strtoul(tok, nullptr, 10);

  if (db.msgCount >= db.msgCap) { db.overflow = 1; return false; }

  DbcMessage &m = db.msg[db.msgCount];
  m.id          = rawId & 0x1FFFFFFFu;
  m.ext         = (rawId & 0x80000000u) ? 1 : 0;
  m.dlc         = (dlc > 8) ? 8 : dlc;
  if (strlen(name) > sizeof(m.name) - 1) db.nameClipped++;
  copyBounded(m.name, sizeof(m.name), name, strlen(name));
  m.firstSignal = db.sigCount;
  m.signalCount = 0;
  m.muxSignal   = -1;

  /* The transmitter, which is the only RX/TX information a DBC carries. A file
   * that omits it simply leaves txNode at -1 and nothing downstream filters. */
  m.txNode = nextToken(&p, tok, sizeof(tok)) ? internNode(db, tok) : -1;

  db.msgCount++;
  db.loaded = 1;
  return true;
}

static bool parseSg(DbcDb &db, char *p) {
  if (db.msgCount == 0) return false;                 /* SG_ before any BO_ */
  DbcMessage &m = db.msg[db.msgCount - 1];

  /* Signals of one message must stay contiguous, which they are as long as the
   * file is not interleaved. If it is, we cannot represent it. */
  if (m.firstSignal + m.signalCount != db.sigCount) return false;

  char name[DBC_NAME_MAX + 8];
  if (!nextToken(&p, name, sizeof(name))) return false;
  size_t nl = strlen(name);
  if (nl && name[nl - 1] == ':') name[--nl] = '\0';

  int16_t muxValue = -1;

  /* Optional multiplexing indicator, then the ':' separator. */
  for (int guard = 0; guard < 3; guard++) {
    char *save = p;
    char tok[16];
    if (!nextToken(&p, tok, sizeof(tok))) return false;
    if (tok[0] == ':' && tok[1] == '\0') break;
    if (tok[0] == 'M' && tok[1] == '\0') { muxValue = -2; continue; }
    if (tok[0] == 'm' && tok[1] >= '0' && tok[1] <= '9') {
      muxValue = (int16_t)strtoul(tok + 1, nullptr, 10);
      continue;
    }
    /* Not a separator and not a mux token: the ':' was glued to the name. */
    p = save;
    break;
  }

  /* start|len@order sign */
  p = skipSpace(p);
  char *endp = nullptr;
  const unsigned long start = strtoul(p, &endp, 10);
  if (endp == p || *endp != '|') return false;
  p = endp + 1;
  const unsigned long bits = strtoul(p, &endp, 10);
  if (endp == p || *endp != '@') return false;
  p = endp + 1;
  if (*p != '0' && *p != '1') return false;
  const bool intel = (*p == '1');
  p++;
  if (*p != '+' && *p != '-') return false;
  const bool isSigned = (*p == '-');
  p++;

  if (bits == 0 || bits > 64 || start > 63) return false;

  /* (factor,offset) */
  while (*p && *p != '(') p++;
  if (!*p) return false;
  p++;
  const char *fs = p;
  while (*p && *p != ',') p++;
  if (!*p) return false;
  const DecNum fac = parseDecimal(fs, (size_t)(p - fs));
  p++;
  const char *os = p;
  while (*p && *p != ')') p++;
  if (!*p) return false;
  const DecNum offs = parseDecimal(os, (size_t)(p - os));
  p++;

  /* [min|max]. The decoder still does not use it - a reading outside the range
   * is recorded exactly as it arrived, because the bus is the authority and
   * the file is only an annotation - but the dashboard scales a gauge from it,
   * which is the difference between picking a signal and being asked for two
   * numbers the file already knew. [0|0] means "unspecified" to every exporter
   * that writes it, so it is not treated as a range. */
  double phyMin = 0.0, phyMax = 0.0;
  bool   hasRange = false;
  while (*p && *p != '[' && *p != '"') p++;
  if (*p == '[') {
    p++;
    char *e = nullptr;
    const double lo = strtod(p, &e);
    if (e != p && *e == '|') {
      p = e + 1;
      const double hi = strtod(p, &e);
      if (e != p) { phyMin = lo; phyMax = hi; hasRange = (hi > lo); }
    }
    while (*p && *p != ']') p++;
    if (*p == ']') p++;
  }

  char unit[DBC_UNIT_MAX];
  unit[0] = '\0';
  char *after = readQuoted(p, unit, sizeof(unit));
  (void)after;

  if (db.sigCount >= db.sigCap) { db.overflow = 1; return false; }

  DbcSignal &s = db.sig[db.sigCount];
  memset(&s, 0, sizeof(s));
  if (strlen(name) > sizeof(s.name) - 1) db.nameClipped++;
  copyBounded(s.name, sizeof(s.name), name, strlen(name));
  copyBounded(s.unit, sizeof(s.unit), unit, strlen(unit));
  s.startBit = (uint8_t)start;
  s.bits     = (uint8_t)bits;
  s.intel    = intel ? 1 : 0;
  s.isSigned = isSigned ? 1 : 0;
  s.fltType  = 0;
  s.muxValue = muxValue;
  s.valFirst = -1;
  s.valCount = 0;
  s.fFactor  = fac.dv;
  s.fOffset  = offs.dv;
  s.phyMin   = (float)phyMin;
  s.phyMax   = (float)phyMax;
  s.hasRange = hasRange ? 1 : 0;

  /* Bring factor and offset onto one common power of ten. */
  s.exact = 0;
  if (fac.ok && offs.ok) {
    const uint8_t dec = (fac.dec > offs.dec) ? fac.dec : offs.dec;
    const int64_t num = fac.num  * POW10_64[dec - fac.dec];
    const int64_t off = offs.num * POW10_64[dec - offs.dec];
    if (dec <= 9 && num <= 2000000000LL && num >= -2000000000LL &&
        off <= 2000000000LL && off >= -2000000000LL) {
      s.num   = (int32_t)num;
      s.off   = (int32_t)off;
      s.dec   = dec;
      s.exact = 1;
    }
  }
  if (!s.exact) db.inexact = 1;

  if (muxValue == -2) m.muxSignal = (int16_t)db.sigCount;

  db.sigCount++;
  m.signalCount++;
  return true;
}

static bool parseVal(DbcDb &db, char *p) {
  char tok[DBC_NAME_MAX + 8];

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  char *endp = nullptr;
  const uint32_t rawId = (uint32_t)strtoul(tok, &endp, 10);
  if (endp == tok) return false;

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  const int16_t sigIdx = findSignal(db, findMsgByRawId(db, rawId), tok);
  if (sigIdx < 0) return false;

  DbcSignal &s = db.sig[sigIdx];

  for (;;) {
    p = skipSpace(p);
    if (!*p || *p == ';') break;

    endp = nullptr;
    const long v = strtol(p, &endp, 10);
    if (endp == p) break;
    p = endp;

    char label[DBC_LABEL_MAX];
    char *after = readQuoted(p, label, sizeof(label));
    if (!after) break;
    p = after;

    if (db.valCount >= db.valCap) { db.overflow = 1; break; }
    DbcValDesc &d = db.val[db.valCount];
    d.sig   = (uint16_t)sigIdx;
    d.value = (int32_t)v;
    copyBounded(d.label, sizeof(d.label), label, strlen(label));

    if (s.valFirst < 0) s.valFirst = (int16_t)db.valCount;
    if (s.valCount < 255) s.valCount++;
    db.valCount++;
  }
  return true;
}

static bool parseSigValtype(DbcDb &db, char *p) {
  char tok[DBC_NAME_MAX + 8];

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  const uint32_t rawId = (uint32_t)strtoul(tok, nullptr, 10);
  if (!nextToken(&p, tok, sizeof(tok))) return false;

  const int16_t sigIdx = findSignal(db, findMsgByRawId(db, rawId), tok);
  if (sigIdx < 0) return false;

  while (*p && *p != ':') p++;
  if (!*p) return false;
  p++;
  const unsigned long t = strtoul(p, nullptr, 10);
  if (t != 1 && t != 2) return false;

  DbcSignal &s = db.sig[sigIdx];
  if ((t == 1 && s.bits != 32) || (t == 2 && s.bits != 64)) return false;
  s.fltType = (uint8_t)t;
  s.exact   = 0;
  db.inexact = 1;
  return true;
}

/* BA_ "Unwrap" SG_ <id> <Signal> 1;  - the one attribute the logger acts on. */
static bool parseBa(DbcDb &db, char *p) {
  char attr[24];
  char *after = readQuoted(p, attr, sizeof(attr));
  if (!after) return false;
  if (strcmp(attr, "Unwrap") != 0) return true;      /* not ours, not an error */
  p = after;

  char tok[DBC_NAME_MAX + 8];
  if (!nextToken(&p, tok, sizeof(tok))) return false;
  if (strcmp(tok, "SG_") != 0) return true;

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  const uint32_t rawId = (uint32_t)strtoul(tok, nullptr, 10);

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  const int16_t sigIdx = findSignal(db, findMsgByRawId(db, rawId), tok);
  if (sigIdx < 0) return false;

  if (!nextToken(&p, tok, sizeof(tok))) return false;
  const long on = strtol(tok, nullptr, 10);

  DbcSignal &s = db.sig[sigIdx];
  if (on && s.bits <= 32 && !s.isSigned && s.fltType == 0) s.unwrap = 1;
  return true;
}

bool dbcParseLine(DbcDb &db, char *line) {
  char *p = skipSpace(line);
  if (!*p || *p == '/') return true;                  /* blank or comment    */

  char kw[24];
  char *save = p;
  if (!nextToken(&p, kw, sizeof(kw))) return true;

  bool ok = true;

  if      (strcmp(kw, "BO_")  == 0)          ok = parseBo(db, p);
  else if (strcmp(kw, "BU_")  == 0)          ok = parseBu(db, p);
  else if (strcmp(kw, "BU_:") == 0)          ok = parseBu(db, p);
  else if (strcmp(kw, "SG_")  == 0)          ok = parseSg(db, p);
  else if (strcmp(kw, "VAL_") == 0)          ok = parseVal(db, p);
  else if (strcmp(kw, "SIG_VALTYPE_") == 0)  ok = parseSigValtype(db, p);
  else if (strcmp(kw, "BA_")  == 0)          ok = parseBa(db, p);
  else if (strcmp(kw, "VERSION") == 0) {
    char v[DBC_VERSION_MAX];
    if (readQuoted(p, v, sizeof(v))) copyBounded(db.version, sizeof(db.version),
                                                 v, strlen(v));
  } else {
    (void)save;                                       /* everything else is
                                                       * deliberately ignored */
  }

  if (!ok) db.lineErrors++;
  return ok;
}

/* ==========================================================================
 *  Decoding
 * ======================================================================== */
static const char *lookupLabel(const DbcDb &db, const DbcSignal &s, int64_t v) {
  if (s.valFirst < 0) return nullptr;
  for (uint8_t i = 0; i < s.valCount; i++) {
    const uint16_t k = (uint16_t)s.valFirst + i;
    if (k >= db.valCount) break;
    if ((int64_t)db.val[k].value == v) return db.val[k].label;
  }
  return nullptr;
}

DbcValue dbcDecodeSignal(const DbcDb &db, DbcSignal &s,
                         const uint8_t *data, uint8_t len) {
  DbcValue r;
  r.ok = false; r.exact = false; r.scaled = 0; r.dec = 0;
  r.fval = 0.0; r.label = nullptr;

  bool fits = false;
  const uint64_t bits = dbcExtractBits(data, len, s.startBit, s.bits,
                                       s.intel != 0, &fits);
  if (!fits) return r;
  r.ok = true;

  /* IEEE payloads carry their own representation; nothing to scale. */
  if (s.fltType == 1) {
    float f;
    const uint32_t u = (uint32_t)bits;
    memcpy(&f, &u, sizeof(f));
    r.fval = (double)f * s.fFactor + s.fOffset;
    return r;
  }
  if (s.fltType == 2) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    r.fval = d * s.fFactor + s.fOffset;
    return r;
  }

  int64_t value;
  if (s.unwrap) {
    /* A free-running counter. A backwards step of more than half the range can
     * only be a wrap; anything smaller is ordinary jitter between samples. */
    const uint32_t cur  = (uint32_t)bits;
    const uint64_t span = (s.bits >= 32) ? 0x100000000ULL : (1ULL << s.bits);
    if ((uint64_t)cur < (uint64_t)s.unwrapLast &&
        ((uint64_t)s.unwrapLast - (uint64_t)cur) > (span >> 1)) {
      s.unwrapCount++;
    }
    s.unwrapLast = cur;
    value = (int64_t)cur + (int64_t)((uint64_t)s.unwrapCount * span);
  } else if (s.isSigned && s.bits < 64) {
    const uint64_t sign = 1ULL << (s.bits - 1);
    value = (bits & sign) ? (int64_t)(bits | ~((1ULL << s.bits) - 1ULL))
                          : (int64_t)bits;
  } else {
    value = (int64_t)bits;
  }

  /* Value tables are defined against the raw value, before scaling. */
  r.label = lookupLabel(db, s, value);

  if (s.exact) {
    r.exact  = true;
    r.scaled = value * (int64_t)s.num + (int64_t)s.off;
    r.dec    = s.dec;
  } else {
    r.fval = (double)value * s.fFactor + s.fOffset;
  }
  return r;
}

/* ==========================================================================
 *  Encoding
 *
 *  Written as the mirror image of the decode path above, deliberately reusing
 *  its bit walk rather than writing a second one: dbcInsertBits validates by
 *  asking dbcExtractBits whether the signal fits, so "can be read" and "can be
 *  written" can never drift apart. A transmit path that is subtly not the
 *  inverse of the receive path is the kind of bug that shows up as a machine
 *  doing the wrong thing, not as a wrong number on a screen.
 * ======================================================================== */
bool dbcInsertBits(uint8_t *data, uint8_t len,
                   uint8_t startBit, uint8_t bits, bool intel, uint64_t raw) {
  if (bits == 0 || bits > 64) return false;

  /* Same walk, same bounds. Asking the reader keeps the two in step, and it
   * also means nothing is half-written when a signal does not fit: the check
   * completes before the first bit is touched. */
  bool fits = false;
  (void)dbcExtractBits(data, len, startBit, bits, intel, &fits);
  if (!fits) return false;

  if (bits < 64) raw &= (1ULL << bits) - 1ULL;

  if (intel) {
    for (uint8_t i = 0; i < bits; i++) {
      const uint16_t b = (uint16_t)startBit + i;
      const uint8_t  m = (uint8_t)(1u << (b & 7));
      if ((raw >> i) & 1ULL) data[b >> 3] |= m;
      else                   data[b >> 3] = (uint8_t)(data[b >> 3] & ~m);
    }
  } else {
    int b = startBit;
    for (uint8_t i = 0; i < bits; i++) {
      const uint8_t m = (uint8_t)(1u << (b & 7));
      if ((raw >> (bits - 1 - i)) & 1ULL) data[b >> 3] |= m;
      else                                data[b >> 3] = (uint8_t)(data[b >> 3] & ~m);
      if ((b & 7) == 0) b += 15; else b -= 1;
    }
  }
  return true;
}

/* The raw range the bit width can hold. Kept separate because both the clamp
 * and the physical limits need it. */
static void rawLimits(const DbcSignal &s, int64_t *lo, int64_t *hi) {
  if (s.bits >= 64) {
    *lo = INT64_MIN; *hi = INT64_MAX;
  } else if (s.isSigned) {
    *hi =  (int64_t)((1ULL << (s.bits - 1)) - 1ULL);
    *lo = -(int64_t)(1ULL << (s.bits - 1));
  } else {
    *lo = 0; *hi = (int64_t)((1ULL << s.bits) - 1ULL);
  }
}

void dbcSignalLimits(const DbcSignal &s, double *lo, double *hi) {
  double a, b;

  if (s.fltType) {
    /* An IEEE payload carries the number itself; the bit width bounds nothing
     * a dashboard would want to draw. */
    a = -3.4e38; b = 3.4e38;
  } else {
    int64_t rlo, rhi;
    rawLimits(s, &rlo, &rhi);
    const double f = s.exact ? ((double)s.num / (double)POW10_64[s.dec]) : s.fFactor;
    const double o = s.exact ? ((double)s.off / (double)POW10_64[s.dec]) : s.fOffset;
    a = (double)rlo * f + o;
    b = (double)rhi * f + o;
  }
  if (a > b) { const double t = a; a = b; b = t; }   /* a negative factor flips it */
  if (lo) *lo = a;
  if (hi) *hi = b;
}

DbcEncoded dbcEncodeSignal(const DbcSignal &s, double phys,
                           uint8_t *data, uint8_t len) {
  DbcEncoded r;
  r.ok = false; r.clamped = false; r.raw = 0; r.applied = 0.0;

  if (s.fltType == 1) {
    const double d = s.fFactor != 0.0 ? s.fFactor : 1.0;
    const float  f = (float)((phys - s.fOffset) / d);
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    if (!dbcInsertBits(data, len, s.startBit, s.bits, s.intel != 0, u)) return r;
    r.ok = true; r.raw = (int64_t)u;
    r.applied = (double)f * s.fFactor + s.fOffset;
    return r;
  }
  if (s.fltType == 2) {
    const double d  = s.fFactor != 0.0 ? s.fFactor : 1.0;
    const double v  = (phys - s.fOffset) / d;
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    if (!dbcInsertBits(data, len, s.startBit, s.bits, s.intel != 0, u)) return r;
    r.ok = true; r.raw = (int64_t)u;
    r.applied = v * s.fFactor + s.fOffset;
    return r;
  }

  /* The integer path, which is what nearly every signal takes. Scaling is done
   * the same way round as the decoder does it, so a value shown as 690 encodes
   * to the raw that decodes back to exactly 690 rather than to 689.9999. */
  int64_t raw;
  if (s.exact && s.num != 0) {
    const int64_t scaled = (int64_t)llround(phys * (double)POW10_64[s.dec]);
    const int64_t numr   = scaled - (int64_t)s.off;
    const int64_t den    = (int64_t)s.num;

    /* Round to nearest rather than letting C truncate towards zero: with a
     * factor of 0.1, a value typed as 0.7 would otherwise land one step low. */
    const bool    neg = (numr < 0) != (den < 0);
    const int64_t an  = numr < 0 ? -numr : numr;
    const int64_t ad  = den  < 0 ? -den  : den;
    const int64_t q   = (an + ad / 2) / ad;
    raw = neg ? -q : q;
  } else {
    const double f = (s.fFactor != 0.0) ? s.fFactor : 1.0;
    raw = (int64_t)llround((phys - s.fOffset) / f);
  }

  /* Clamp to what the bits can hold, and say so. Silently wrapping a value
   * that does not fit is how a tyre size of 6900 mm becomes 388 mm on the
   * wire, so the caller is told and the dashboard shows it. */
  if (s.bits < 64) {
    int64_t lo, hi;
    rawLimits(s, &lo, &hi);
    if (raw < lo) { raw = lo; r.clamped = true; }
    if (raw > hi) { raw = hi; r.clamped = true; }
  }

  if (!dbcInsertBits(data, len, s.startBit, s.bits, s.intel != 0, (uint64_t)raw))
    return r;

  r.ok  = true;
  r.raw = raw;
  r.applied = s.exact
      ? ((double)(raw * (int64_t)s.num + (int64_t)s.off) / (double)POW10_64[s.dec])
      : ((double)raw * s.fFactor + s.fOffset);
  return r;
}

/* ==========================================================================
 *  "Message.Signal" references
 *
 *  The dashboard and the transmit list store signals by name, not by index.
 *  An index would be smaller and faster, and it would also silently point at a
 *  different signal the first time someone adds a message to the DBC - so the
 *  name is the identity, and the index is resolved once at load time.
 * ======================================================================== */
int16_t dbcFindSignalRef(const DbcDb &db, const char *ref, int16_t *msgOut) {
  if (msgOut) *msgOut = -1;
  if (!ref || !*ref) return -1;

  const char *dot = strrchr(ref, '.');
  if (!dot || dot == ref || !dot[1]) return -1;
  const size_t mlen = (size_t)(dot - ref);

  for (uint16_t mi = 0; mi < db.msgCount; mi++) {
    const DbcMessage &m = db.msg[mi];
    if (strlen(m.name) != mlen || strncmp(m.name, ref, mlen) != 0) continue;
    for (uint16_t k = 0; k < m.signalCount; k++) {
      const uint16_t si = (uint16_t)(m.firstSignal + k);
      if (si >= db.sigCount) break;
      if (strcmp(db.sig[si].name, dot + 1) == 0) {
        if (msgOut) *msgOut = (int16_t)mi;
        return (int16_t)si;
      }
    }
  }
  return -1;
}

bool dbcSignalRef(const DbcDb &db, uint16_t si, char *out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (si >= db.sigCount) return false;

  for (uint16_t mi = 0; mi < db.msgCount; mi++) {
    const DbcMessage &m = db.msg[mi];
    if (si < m.firstSignal || si >= m.firstSignal + m.signalCount) continue;
    const size_t need = strlen(m.name) + 1 + strlen(db.sig[si].name) + 1;
    if (need > cap) return false;
    strcpy(out, m.name);
    strcat(out, ".");
    strcat(out, db.sig[si].name);
    return true;
  }
  return false;
}
