#include "dash.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* See dash.h: on the heap so it does not sit in the static segment, which is
 * the part of this build with no room left. Nothing frees it - the logger holds
 * one configuration for its whole life. */
static DashConfig *dashAllocate() {
  void *p = calloc(1, sizeof(DashConfig));
  /* Six kilobytes, at start-up, before anything else has allocated. If this
   * fails the board is not going to run; failing here beats dereferencing a
   * null pointer somewhere less obvious an hour later. */
  if (!p) abort();
  return (DashConfig *)p;
}
DashConfig &g_dash = *dashAllocate();

/* ==========================================================================
 *  Text helpers. Same shape as the DBC parser's: a mutable line buffer,
 *  nothing allocated, nothing longer than DASH_LINE_MAX.
 * ======================================================================== */
static const char *const WIDGET_NAME[DW_COUNT] = {
  "gauge", "arc", "angle", "compass", "bar",
  "level", "thermo", "number", "spark", "state"
};

const char *dashWidgetName(uint8_t w) {
  return (w < DW_COUNT) ? WIDGET_NAME[w] : WIDGET_NAME[DW_NUMBER];
}

static const char *const STYLE_NAME[5] = {
  "number", "slider", "toggle", "enum", "choice"
};

const char *dashInputName(uint8_t st) {
  return (st < 5) ? STYLE_NAME[st] : STYLE_NAME[TXI_NUMBER];
}

int8_t dashInputId(const char *name) {
  if (!name) return -1;
  for (uint8_t i = 0; i < 5; i++) {
    if (strcmp(name, STYLE_NAME[i]) == 0) return (int8_t)i;
  }
  return -1;
}

int8_t dashWidgetId(const char *name) {
  if (!name) return -1;
  for (uint8_t i = 0; i < DW_COUNT; i++) {
    if (strcmp(name, WIDGET_NAME[i]) == 0) return (int8_t)i;
  }
  return -1;
}

static inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *skipSpace(char *p) {
  while (*p && isSpace(*p)) p++;
  return p;
}

static void copyBounded(char *dst, size_t cap, const char *src) {
  if (!cap) return;
  size_t i = 0;
  for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
  dst[i] = '\0';
}

/* Reads the next whitespace-delimited token, honouring double quotes so a
 * label can contain spaces. Advances *pp past it. */
static bool nextToken(char **pp, char *out, size_t cap) {
  char *p = skipSpace(*pp);
  if (!*p) { *pp = p; return false; }

  size_t n = 0;
  if (*p == '"') {
    p++;
    while (*p && *p != '"') {
      if (n + 1 < cap) out[n++] = *p;
      p++;
    }
    if (*p == '"') p++;
  } else {
    while (*p && !isSpace(*p)) {
      if (n + 1 < cap) out[n++] = *p;
      p++;
    }
  }
  out[n] = '\0';
  *pp = p;
  return true;
}

/* Reads one key=value pair, taking quoting into account. Unlike splitKv this
 * works on the raw line, because a quoted value can contain spaces and would
 * otherwise be cut in half by tokenising first. */
static bool nextPair(char **pp, char *key, size_t kcap, char *val, size_t vcap) {
  char *p = skipSpace(*pp);
  if (!*p) { *pp = p; return false; }

  size_t n = 0;
  while (*p && *p != '=' && !isSpace(*p)) {
    if (n + 1 < kcap) key[n++] = *p;
    p++;
  }
  key[n] = '\0';

  val[0] = '\0';
  if (*p == '=') {
    p++;
    n = 0;
    if (*p == '"') {
      p++;
      while (*p && *p != '"') {
        if (n + 1 < vcap) val[n++] = *p;
        p++;
      }
      if (*p == '"') p++;
    } else {
      while (*p && !isSpace(*p)) {
        if (n + 1 < vcap) val[n++] = *p;
        p++;
      }
    }
    val[n] = '\0';
  }

  *pp = p;
  return key[0] != '\0';
}

/* "%g" without dragging in the whole of printf's float machinery twice, and
 * without ever emitting an exponent or a NaN - both of which would come back
 * as something else when the file is read again. */
static size_t fmtFloat(char *out, size_t cap, float v) {
  if (!(v == v)) { copyBounded(out, cap, "0"); return strlen(out); }   /* NaN */
  if (v > 1e9f)  v =  1e9f;
  if (v < -1e9f) v = -1e9f;

  /* Three decimals is enough for every physical range a gauge is drawn over,
   * and trailing zeros are trimmed so a whole number stays a whole number. */
  int n = snprintf(out, cap, "%.3f", (double)v);
  if (n < 0 || (size_t)n >= cap) { copyBounded(out, cap, "0"); return strlen(out); }

  char *dot = strchr(out, '.');
  if (dot) {
    char *end = out + n;
    while (end > dot + 1 && end[-1] == '0') end--;
    if (end == dot + 1) end = dot;                 /* "5." -> "5" */
    *end = '\0';
    n = (int)(end - out);
  }
  return (size_t)n;
}

uint32_t dashHash(const char *text, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)text[i];
    h *= 16777619u;
  }
  return h;
}

/* ==========================================================================
 *  The configuration itself
 * ======================================================================== */
void dashReset(DashConfig &c) {
  memset(&c, 0, sizeof(c));
  c.cols   = 4;
  c.rows   = 2;
  c.pollMs = DASH_POLL_MS;
  for (uint8_t i = 0; i < DASH_MAX_CELLS; i++) {
    c.cell[i].sig = -1;
    c.cell[i].dec = 255;
  }
  for (uint8_t i = 0; i < TX_MAX_COMMANDS; i++) {
    c.tx[i].sig = -1;
    c.tx[i].msg = -1;
    c.tx[i].dec = 255;
  }
}

/* ---- cell <slot> key=value ... ---------------------------------------- */
static bool parseCell(DashConfig &c, char *p) {
  char tok[DASH_LINE_MAX];
  if (!nextToken(&p, tok, sizeof(tok))) return false;

  char *endp = nullptr;
  const unsigned long slot = strtoul(tok, &endp, 10);
  if (endp == tok || slot >= DASH_MAX_CELLS) return false;

  DashCell &d = c.cell[slot];
  memset(&d, 0, sizeof(d));
  d.sig = -1;
  d.dec = 255;

  char key[24], val[DASH_LINE_MAX];
  while (nextPair(&p, key, sizeof(key), val, sizeof(val))) {
    if      (!strcmp(key, "widget")) {
      const int8_t w = dashWidgetId(val);
      d.widget = (w < 0) ? (uint8_t)DW_NUMBER : (uint8_t)w;
    }
    else if (!strcmp(key, "sig"))   copyBounded(d.ref,   sizeof(d.ref),   val);
    else if (!strcmp(key, "label")) copyBounded(d.label, sizeof(d.label), val);
    else if (!strcmp(key, "unit"))  copyBounded(d.unit,  sizeof(d.unit),  val);
    else if (!strcmp(key, "lo"))    d.lo = (float)atof(val);
    else if (!strcmp(key, "hi"))    d.hi = (float)atof(val);
    else if (!strcmp(key, "dec"))   d.dec = (uint8_t)strtoul(val, nullptr, 10);
    else if (!strcmp(key, "warn"))  { d.warn = (float)atof(val); d.flags |= DF_HAS_WARN; }
    else if (!strcmp(key, "crit"))  { d.crit = (float)atof(val); d.flags |= DF_HAS_CRIT; }
    else if (!strcmp(key, "lowbad")) { if (atoi(val)) d.flags |= DF_LOW_BAD; }
    /* anything else is from a newer version: skip it, do not fail */
  }

  /* Deliberately no fallback range here. Leaving hi <= lo is what tells
   * dashResolve() the cell wants the DBC's answer - a cell that names only a
   * signal is a complete cell, and filling in a guess at parse time would
   * make it look like the user had already chosen one. */
  return true;
}

/* ---- send <index> key=value ... ---------------------------------------- */
static uint8_t hexNibble(char ch) {
  if (ch >= '0' && ch <= '9') return (uint8_t)(ch - '0');
  if (ch >= 'a' && ch <= 'f') return (uint8_t)(ch - 'a' + 10);
  if (ch >= 'A' && ch <= 'F') return (uint8_t)(ch - 'A' + 10);
  return 0xFF;
}

static bool parseSend(DashConfig &c, char *p) {
  char tok[DASH_LINE_MAX];
  if (!nextToken(&p, tok, sizeof(tok))) return false;

  char *endp = nullptr;
  const unsigned long idx = strtoul(tok, &endp, 10);
  if (endp == tok || idx >= TX_MAX_COMMANDS) return false;

  TxCommand &t = c.tx[idx];
  memset(&t, 0, sizeof(t));
  t.sig  = -1;
  t.msg  = -1;
  t.dec  = 255;
  t.step = 1.0f;
  t.kind = TXK_SIGNAL;
  t.muxCode = -1;               /* no override until msel names one */

  char key[24], val[DASH_LINE_MAX];
  while (nextPair(&p, key, sizeof(key), val, sizeof(val))) {
    if      (!strcmp(key, "label")) copyBounded(t.label, sizeof(t.label), val);
    else if (!strcmp(key, "sig"))   copyBounded(t.ref,   sizeof(t.ref),   val);
    else if (!strcmp(key, "unit"))  copyBounded(t.unit,  sizeof(t.unit),  val);
    else if (!strcmp(key, "lo"))    t.lo     = (float)atof(val);
    else if (!strcmp(key, "hi"))    t.hi     = (float)atof(val);
    else if (!strcmp(key, "step"))  t.step   = (float)atof(val);
    else if (!strcmp(key, "preset")) t.preset = (float)atof(val);
    else if (!strcmp(key, "dec"))   t.dec    = (uint8_t)strtoul(val, nullptr, 10);
    else if (!strcmp(key, "cyclic")) t.cyclicMs = (uint16_t)strtoul(val, nullptr, 10);
    else if (!strcmp(key, "msel")) copyBounded(t.muxSel, sizeof(t.muxSel), val);
    else if (!strcmp(key, "mxc"))  t.muxCode = (int16_t)strtol(val, nullptr, 10);
    /* "mux=1" is what an earlier version wrote to mark a value as one
     * payload of a multiplexed message. The message name and the frame map
     * carry that now, so it is accepted here and not echoed back. */
    else if (!strcmp(key, "style")) {
      const int8_t st = dashInputId(val);
      t.style = (st < 0) ? (uint8_t)TXI_NUMBER : (uint8_t)st;
    }
    else if (!strcmp(key, "choices")) copyBounded(t.choices, sizeof(t.choices), val);
    else if (!strcmp(key, "id")) {
      t.kind = TXK_RAW;
      t.id   = (uint32_t)strtoul(val, nullptr, 0);
      if (t.id > 0x7FF) t.ext = 1;
    }
    else if (!strcmp(key, "ext"))  t.ext = atoi(val) ? 1 : 0;
    else if (!strcmp(key, "data")) {
      t.kind = TXK_RAW;
      uint8_t n = 0;
      for (const char *q = val; q[0] && q[1] && n < 8; q += 2) {
        const uint8_t hi = hexNibble(q[0]), lo = hexNibble(q[1]);
        if (hi == 0xFF || lo == 0xFF) break;
        t.data[n++] = (uint8_t)((hi << 4) | lo);
      }
      t.len = n;
    }
  }

  if (t.step <= 0.0f) t.step = 1.0f;

  /* A cyclic period below the CAN task's own wake interval would just be
   * rounded up to it, so clamping here keeps what is saved honest about what
   * will happen. */
  if (t.cyclicMs && t.cyclicMs < TX_CYCLIC_MIN_MS) t.cyclicMs = TX_CYCLIC_MIN_MS;

  /* A command with no label is how a slot is deleted, and it must not keep
   * whatever else was on the line. */
  if (!t.label[0]) {
    memset(&t, 0, sizeof(t));
    t.sig = -1;
    t.msg = -1;
  }
  return true;
}

bool dashParseLine(DashConfig &c, char *line) {
  char *p = skipSpace(line);
  if (!*p || *p == '#') return true;

  char kw[16];
  if (!nextToken(&p, kw, sizeof(kw))) return true;

  if (!strcmp(kw, "grid")) {
    char a[16], b[16];
    if (!nextToken(&p, a, sizeof(a)) || !nextToken(&p, b, sizeof(b))) return false;
    const unsigned long cols = strtoul(a, nullptr, 10);
    const unsigned long rows = strtoul(b, nullptr, 10);
    if (cols < 1 || cols > DASH_MAX_COLS || rows < 1 || rows > DASH_MAX_ROWS)
      return false;
    c.cols = (uint8_t)cols;
    c.rows = (uint8_t)rows;
    return true;
  }
  if (!strcmp(kw, "poll")) {
    char a[16];
    if (!nextToken(&p, a, sizeof(a))) return false;
    unsigned long ms = strtoul(a, nullptr, 10);
    /* Bounded on both sides. Faster than DASH_POLL_MIN_MS costs the ESP32 real
     * time for a smoothness the eye cannot see; slower than a couple of
     * seconds stops being a live view. */
    if (ms < DASH_POLL_MIN_MS) ms = DASH_POLL_MIN_MS;
    if (ms > 5000)             ms = 5000;
    c.pollMs = (uint16_t)ms;
    return true;
  }
  /* "node" was this setting's name before it was briefly removed. Accepting it
     costs one comparison and means a setup file exported by any version still
     carries its role across. */
  if (!strcmp(kw, "role") || !strcmp(kw, "node")) {
    char v[DASH_ROLE_MAX + 8];
    if (!nextToken(&p, v, sizeof(v))) return false;
    copyBounded(c.role, sizeof(c.role), v);
    return true;
  }
  if (!strcmp(kw, "cell")) return parseCell(c, p);
  if (!strcmp(kw, "send")) return parseSend(c, p);
  if (!strcmp(kw, "version")) return true;      /* recorded, not acted on */

  return true;   /* an unknown keyword is a later version's, not an error */
}

uint16_t dashParse(DashConfig &c, const char *text, size_t len) {
  char     line[DASH_LINE_MAX];
  size_t   n = 0;
  uint16_t errors = 0;

  for (size_t i = 0; i <= len; i++) {
    const char ch = (i < len) ? text[i] : '\n';
    if (ch == '\n' || ch == '\r') {
      if (n) {
        line[n] = '\0';
        if (!dashParseLine(c, line)) errors++;
        n = 0;
      }
    } else if (n + 1 < sizeof(line)) {
      line[n++] = ch;
    }
  }
  return errors;
}

/* ==========================================================================
 *  Serialising
 *
 *  Byte-stable: the same configuration must always produce the same text, or
 *  the hash the boot rule compares against the card would change on its own
 *  and re-import a file nobody edited.
 * ======================================================================== */
static size_t appendStr(char *out, size_t cap, size_t n, const char *s) {
  while (*s && n + 1 < cap) out[n++] = *s++;
  if (n < cap) out[n] = '\0';
  return n;
}

/* Quotes a value only when it needs it, so the common case stays readable. */
static size_t appendValue(char *out, size_t cap, size_t n, const char *v) {
  const bool quote = (v[0] == '\0') || strchr(v, ' ') || strchr(v, '\t');
  if (quote) n = appendStr(out, cap, n, "\"");
  for (const char *p = v; *p && n + 1 < cap; p++) {
    if (*p == '"' || *p == '\n') continue;      /* cannot survive the format */
    out[n++] = *p;
  }
  if (n < cap) out[n] = '\0';
  if (quote) n = appendStr(out, cap, n, "\"");
  return n;
}

static size_t appendNum(char *out, size_t cap, size_t n, float v) {
  char buf[24];
  fmtFloat(buf, sizeof(buf), v);
  return appendStr(out, cap, n, buf);
}

static size_t appendInt(char *out, size_t cap, size_t n, long v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%ld", v);
  return appendStr(out, cap, n, buf);
}

size_t dashSerialize(const DashConfig &c, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  out[0] = '\0';

  n = appendStr(out, cap, n,
      "# CAN Logger dashboard\n"
      "#\n"
      "# Written by the logger, and safe to edit by hand: put this file on the\n"
      "# SD card as " DASH_PATH " and it is picked up at the next boot.\n"
      "#\n"
      "#   grid <cols> <rows>          the layout\n"
      "#   poll <ms>                   how often the browser asks for values\n"
      "#   role \"<Name>\"               which BU_ node this logger IS, if any\n"
      "#   cell <slot> widget=.. sig=Message.Signal lo=.. hi=..\n"
      "#   send <n> label=\"..\" sig=Message.Signal lo=.. hi=..\n"
      "#              mux=1            one payload of a multiplexed frame\n"
      "#\n"
      "# widget is one of: gauge arc angle compass bar level thermo number\n"
      "#                   spark state\n"
      "\n"
      "version 1\n"
      "grid ");
  n = appendInt(out, cap, n, c.cols);
  n = appendStr(out, cap, n, " ");
  n = appendInt(out, cap, n, c.rows);
  n = appendStr(out, cap, n, "\npoll ");
  n = appendInt(out, cap, n, c.pollMs);
  n = appendStr(out, cap, n, "\n");
  if (c.role[0]) {
    n = appendStr(out, cap, n, "role ");
    n = appendValue(out, cap, n, c.role);
    n = appendStr(out, cap, n, "\n");
  }
  const uint8_t cells = dashCellCount(c);
  bool any = false;
  for (uint8_t i = 0; i < cells; i++) {
    const DashCell &d = c.cell[i];
    if (!dashCellUsed(d)) continue;
    if (!any) { n = appendStr(out, cap, n, "\n"); any = true; }

    n = appendStr(out, cap, n, "cell ");
    n = appendInt(out, cap, n, i);
    n = appendStr(out, cap, n, " widget=");
    n = appendStr(out, cap, n, dashWidgetName(d.widget));
    n = appendStr(out, cap, n, " sig=");
    n = appendValue(out, cap, n, d.ref);

    if (d.label[0]) { n = appendStr(out, cap, n, " label=");
                      n = appendValue(out, cap, n, d.label); }
    if (d.unit[0])  { n = appendStr(out, cap, n, " unit=");
                      n = appendValue(out, cap, n, d.unit); }

    n = appendStr(out, cap, n, " lo=");  n = appendNum(out, cap, n, d.lo);
    n = appendStr(out, cap, n, " hi=");  n = appendNum(out, cap, n, d.hi);

    if (d.dec != 255) { n = appendStr(out, cap, n, " dec=");
                        n = appendInt(out, cap, n, d.dec); }
    if (d.flags & DF_HAS_WARN) { n = appendStr(out, cap, n, " warn=");
                                 n = appendNum(out, cap, n, d.warn); }
    if (d.flags & DF_HAS_CRIT) { n = appendStr(out, cap, n, " crit=");
                                 n = appendNum(out, cap, n, d.crit); }
    if (d.flags & DF_LOW_BAD)  { n = appendStr(out, cap, n, " lowbad=1"); }

    n = appendStr(out, cap, n, "\n");
  }

  any = false;
  for (uint8_t i = 0; i < TX_MAX_COMMANDS; i++) {
    const TxCommand &t = c.tx[i];
    if (!txCommandUsed(t)) continue;
    if (!any) { n = appendStr(out, cap, n, "\n"); any = true; }

    n = appendStr(out, cap, n, "send ");
    n = appendInt(out, cap, n, i);
    n = appendStr(out, cap, n, " label=");
    n = appendValue(out, cap, n, t.label);

    if (t.kind == TXK_RAW) {
      char buf[24];
      snprintf(buf, sizeof(buf), " id=0x%lX", (unsigned long)t.id);
      n = appendStr(out, cap, n, buf);
      if (t.ext) n = appendStr(out, cap, n, " ext=1");
      n = appendStr(out, cap, n, " data=");
      for (uint8_t k = 0; k < t.len; k++) {
        snprintf(buf, sizeof(buf), "%02X", t.data[k]);
        n = appendStr(out, cap, n, buf);
      }
      if (t.len == 0) n = appendStr(out, cap, n, "\"\"");
    } else {
      n = appendStr(out, cap, n, " sig=");
      n = appendValue(out, cap, n, t.ref);
      if (t.unit[0]) { n = appendStr(out, cap, n, " unit=");
                       n = appendValue(out, cap, n, t.unit); }
      n = appendStr(out, cap, n, " lo=");     n = appendNum(out, cap, n, t.lo);
      n = appendStr(out, cap, n, " hi=");     n = appendNum(out, cap, n, t.hi);
      n = appendStr(out, cap, n, " step=");   n = appendNum(out, cap, n, t.step);
      n = appendStr(out, cap, n, " preset="); n = appendNum(out, cap, n, t.preset);
      if (t.dec != 255) { n = appendStr(out, cap, n, " dec=");
                          n = appendInt(out, cap, n, t.dec); }
      n = appendStr(out, cap, n, " style=");
      n = appendStr(out, cap, n, dashInputName(t.style));
      if (t.choices[0]) { n = appendStr(out, cap, n, " choices=");
                          n = appendValue(out, cap, n, t.choices); }
    }
    if (t.muxSel[0]) { n = appendStr(out, cap, n, " msel=");
                       n = appendValue(out, cap, n, t.muxSel);
                       n = appendStr(out, cap, n, " mxc=");
                       n = appendInt(out, cap, n,
                                     t.muxCode < 0 ? 0 : t.muxCode); }
    if (t.cyclicMs) { n = appendStr(out, cap, n, " cyclic=");
                      n = appendInt(out, cap, n, t.cyclicMs); }

    n = appendStr(out, cap, n, "\n");
  }

  return n;
}

/* ==========================================================================
 *  Binding names to the frame map
 * ======================================================================== */
int16_t txOverrideSelector(const TxCommand &t, const DbcDb &db) {
  if (!t.muxSel[0] || t.muxCode < 0) return -1;
  if (t.msg < 0 || (uint16_t)t.msg >= db.msgCount) return -1;

  const DbcMessage &m = db.msg[t.msg];
  if (m.muxSignal >= 0) return -1;            /* the file already said so */

  for (uint16_t k = 0; k < m.signalCount; k++) {
    const uint16_t idx = (uint16_t)(m.firstSignal + k);
    if (idx >= db.sigCount) break;
    if (strcmp(db.sig[idx].name, t.muxSel) == 0) return (int16_t)idx;
  }
  return -1;
}

uint16_t dashResolve(DashConfig &c, const DbcDb &db) {
  uint16_t missing = 0;

  for (uint8_t i = 0; i < DASH_MAX_CELLS; i++) {
    DashCell &d = c.cell[i];
    if (!dashCellUsed(d)) { d.sig = -1; continue; }
    d.sig = dbcFindSignalRef(db, d.ref, nullptr);
    if (d.sig < 0) {
      if (i < dashCellCount(c)) missing++;
      /* Still give it a drawable range. The cell has to render in order to
       * show that it cannot find its signal; a zero-width range would divide
       * by zero in the browser and the cell would just vanish. */
      if (d.hi <= d.lo) { d.lo = 0.0f; d.hi = 100.0f; }
      continue;
    }

    /* Fill in whatever the cell did not say from what the DBC knows. A cell
     * that names only a signal is a complete cell. */
    const DbcSignal &s = db.sig[d.sig];
    if (!d.unit[0]) copyBounded(d.unit, sizeof(d.unit), s.unit);
    if (d.hi <= d.lo) {
      /* The file's own annotation first, because it is what the bus designer
       * meant; the bit width second, because it is at least true. */
      if (s.hasRange) { d.lo = s.phyMin; d.hi = s.phyMax; }
      else {
        double lo = 0, hi = 0;
        dbcSignalLimits(s, &lo, &hi);
        d.lo = (float)lo;
        d.hi = (float)hi;
      }
    }
  }

  for (uint8_t i = 0; i < TX_MAX_COMMANDS; i++) {
    TxCommand &t = c.tx[i];
    if (!txCommandUsed(t) || t.kind != TXK_SIGNAL) { continue; }
    t.sig = dbcFindSignalRef(db, t.ref, &t.msg);
    if (t.sig < 0) { missing++; continue; }

    const DbcSignal &s = db.sig[t.sig];
    if (!t.unit[0]) copyBounded(t.unit, sizeof(t.unit), s.unit);

    /* A setpoint's range must never exceed what the bits can carry, whatever
     * the config or the DBC annotation claims: the slider has to stop where
     * the wire does, or the operator aims at a value that will be clamped. */
    double lo = 0, hi = 0;
    dbcSignalLimits(s, &lo, &hi);
    if (t.hi <= t.lo) {
      if (s.hasRange) { t.lo = s.phyMin; t.hi = s.phyMax; }
      else            { t.lo = (float)lo; t.hi = (float)hi; }
    }
    if (t.lo < (float)lo) t.lo = (float)lo;
    if (t.hi > (float)hi) t.hi = (float)hi;

    /* An override this map cannot honour is forgotten rather than half-obeyed.
     * Left in place it would put a code into bits that are not the selector's,
     * or into a message the file already multiplexes its own way - either of
     * which is a frame that looks sent and is not the one asked for. */
    if (t.muxSel[0] && txOverrideSelector(t, db) < 0) {
      t.muxSel[0] = 0;
      t.muxCode   = -1;
    }
  }

  return missing;
}

uint16_t dashDropUnresolved(DashConfig &c, const DbcDb &db) {
  uint16_t dropped = 0;

  /* The role names a node of the OLD map. If the new file has no such node the
   * answer is not merely stale, it is unanswerable - and left alone the header
   * would go on asserting "Role: Tester" while both Fill buttons quietly
   * stopped separating anything, because nothing transmits under that name.
   * Cleared, so the page asks the question again against the new file. */
  if (c.role[0]) {
    bool known = false;
    for (uint8_t i = 0; i < db.nodeCount && !known; i++)
      known = strcmp(db.node[i], c.role) == 0;
    if (!known) { c.role[0] = 0; dropped++; }
  }

  for (uint8_t i = 0; i < DASH_MAX_CELLS; i++) {
    DashCell &d = c.cell[i];
    if (!dashCellUsed(d)) continue;
    if (dbcFindSignalRef(db, d.ref, nullptr) >= 0) continue;
    memset(&d, 0, sizeof(d));
    d.sig = -1;
    d.dec = 255;
    dropped++;
  }

  /* Compacted, because a hole in the middle of the grid is not what anyone
   * means by "the cells that survived". The browser fills from slot 0 up and
   * an operator reads the layout left to right; leaving gaps where the old
   * map's signals used to be would look like the layout is still half there. */
  uint8_t w = 0;
  for (uint8_t i = 0; i < DASH_MAX_CELLS; i++) {
    if (!dashCellUsed(c.cell[i])) continue;
    if (w != i) { c.cell[w] = c.cell[i]; memset(&c.cell[i], 0, sizeof(DashCell));
                  c.cell[i].sig = -1; c.cell[i].dec = 255; }
    w++;
  }

  for (uint8_t i = 0; i < TX_MAX_COMMANDS; i++) {
    TxCommand &t = c.tx[i];
    if (!txCommandUsed(t)) continue;
    /* A raw-identifier command names no signal, so no frame map can invalidate
     * it. Those are the one thing that survives a map it was not written for. */
    if (t.kind != TXK_SIGNAL) continue;
    int16_t msgIdx = -1;
    if (dbcFindSignalRef(db, t.ref, &msgIdx) >= 0) {
      /* The value survives, but an override written against the old map may
       * not: its selector has to be a signal of THIS message under THIS file.
       * Counted, so the page can say what stopped being true. */
      if (t.muxSel[0]) {
        t.msg = msgIdx;
        if (txOverrideSelector(t, db) < 0) {
          t.muxSel[0] = 0;
          t.muxCode   = -1;
          dropped++;
        }
      }
      continue;
    }
    memset(&t, 0, sizeof(t));
    t.sig = -1;
    t.msg = -1;
    t.dec = 255;
    dropped++;
  }

  uint8_t tw = 0;
  for (uint8_t i = 0; i < TX_MAX_COMMANDS; i++) {
    if (!txCommandUsed(c.tx[i])) continue;
    if (tw != i) { c.tx[tw] = c.tx[i]; memset(&c.tx[i], 0, sizeof(TxCommand));
                   c.tx[i].sig = -1; c.tx[i].msg = -1; c.tx[i].dec = 255; }
    tw++;
  }

  return dropped;
}
