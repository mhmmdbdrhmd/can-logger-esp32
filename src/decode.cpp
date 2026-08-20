#include "decode.h"
#include <stdarg.h>
#include "fmt.h"
#include "config.h"

#if CANOPEN_DECODE
#include "canopen.h"
#endif

BusStats    g_bus;
LiveSignals g_live;

/* ==========================================================================
 *  Live bus activity
 * ======================================================================== */
void busReset(BusStats &b) {
  memset(&b, 0, sizeof(b));
}

void liveReset(LiveSignals &l) {
  memset(&l, 0, sizeof(l));
}

/* Returns the table slot for this identifier, or -1 when the table is full. */
static int16_t busSlot(BusStats &b, uint32_t id, bool ext, bool known,
                       uint32_t nowMs) {
  for (uint8_t i = 0; i < b.used; i++) {
    if (b.id[i] == id && (bool)b.ext[i] == ext) {
      b.count[i]++;
      b.lastMs[i] = nowMs;
      b.known[i]  = known ? 1 : 0;
      b.total++;
      return (int16_t)i;
    }
  }
  if (b.used < BUS_TRACK_IDS) {
    const uint8_t i = b.used;
    b.id[i]      = id;
    b.ext[i]     = ext ? 1 : 0;
    b.count[i]   = 1;
    b.prev[i]    = 0;
    b.rate[i]    = 0;
    b.lastMs[i]  = nowMs;
    b.known[i]   = known ? 1 : 0;
    b.last[i][0] = '\0';
    b.used       = (uint8_t)(i + 1);
    b.total++;
    return (int16_t)i;
  }
  b.untracked++;
  b.total++;
  return -1;
}

void busNote(BusStats &b, uint32_t id, bool ext, bool known, uint32_t nowMs) {
  (void)busSlot(b, id, ext, known, nowMs);
}

void busObserve(BusStats &b, const CanFrame &f, const DbcDb *db) {
  const bool    known = db && dbcFind(*db, f.id, f.ext != 0) != nullptr;
  const int16_t slot  = busSlot(b, f.id, f.ext != 0, known, millis());
  if (slot < 0) return;

  /* Keep the payload so a bus with no frame map is still watchable live. */
  static const char kDigits[] = "0123456789ABCDEF";
  char *p = b.last[slot];
  const uint8_t n = (f.len > 8) ? 8 : f.len;
  for (uint8_t i = 0; i < n; i++) {
    *p++ = kDigits[f.data[i] >> 4];
    *p++ = kDigits[f.data[i] & 0x0Fu];
  }
  *p = '\0';
}

void busTick(BusStats &b, uint32_t elapsedMs) {
  if (!elapsedMs) elapsedMs = 1;
  for (uint8_t i = 0; i < b.used; i++) {
    const uint32_t d = b.count[i] - b.prev[i];
    b.rate[i] = (uint32_t)(((uint64_t)d * 1000ULL) / elapsedMs);
    b.prev[i] = b.count[i];
  }
}

/* ==========================================================================
 *  Row construction
 * ======================================================================== */
static char *putStr(char *p, const char *s) {
  if (s) while (*s) *p++ = *s++;
  return p;
}

static char *putPayload(char *p, const CanFrame &f) {
  for (uint8_t i = 0; i < f.len; i++) p = fmtHex8(p, f.data[i]);
  return p;
}

/* Copies the text just written into the CSV into the live view's slot for that
 * signal. The web handler then only has to concatenate strings - it never
 * decodes anything, and it never learns a signal's name from the firmware. */
static void publishLive(const DbcDb *db, uint16_t sigIndex,
                        const char *text, size_t len) {
  if (!db || sigIndex >= DBC_MAX_SIGNALS) return;
  if (len > LIVE_TEXT_MAX - 1) len = LIVE_TEXT_MAX - 1;

  memcpy(g_live.text[sigIndex], text, len);
  g_live.text[sigIndex][len] = '\0';
  g_live.lastMs[sigIndex]    = millis();
  if (!g_live.seen[sigIndex]) {
    g_live.seen[sigIndex] = 1;
    g_live.seenCount++;
  }
}

/* t_us;id;  - the two columns every row starts with. */
static char *putPrefix(char *p, int64_t t, uint32_t id, bool ext) {
  p = fmtI64(p, t);
  *p++ = ';';
  p = fmtCanId(p, id, ext);
  *p++ = ';';
  return p;
}

void Decoder::reset(DbcDb *db) {
  _db    = db;
  _have  = false;
  _epoch = 0;
  if (db) dbcResetRuntime(*db);
}

size_t Decoder::rows(const CanFrame &f, char *buf, size_t cap, uint8_t *rowsOut) {
  if (rowsOut) *rowsOut = 0;
  if (cap < DECODE_FRAME_MAX) return 0;

  if (!_have) { _epoch = f.esp_us; _have = true; }
  const int64_t t = (int64_t)(f.esp_us - _epoch);
  const bool    ext = (f.ext != 0);

  char *p = buf;
  uint8_t emitted = 0;
  bool    rawDone = false;

  const DbcMessage *m = _db ? dbcFind(*_db, f.id, ext) : nullptr;
  const char *msgName = m ? m->name : nullptr;

#if CANOPEN_DECODE
  char coName[CANOPEN_NAME_MAX];
  coName[0] = '\0';
  if (!m && canopenName(f.id, ext, coName, sizeof(coName))) msgName = coName;
#endif

  /* ---- signals from the frame map ------------------------------------- */
  if (m && m->signalCount) {
    /* A multiplexed message only carries the signals its multiplexor selects,
     * so that value has to be read before anything else can be trusted. */
    int32_t muxNow = -1;
    if (m->muxSignal >= 0) {
      DbcValue mv = dbcDecodeSignal(*_db, _db->sig[m->muxSignal], f.data, f.len);
      if (mv.ok) muxNow = (int32_t)(mv.exact ? mv.scaled : (int64_t)mv.fval);
    }

    for (uint16_t i = 0; i < m->signalCount && emitted < DECODE_MAX_ROWS; i++) {
      DbcSignal &s = _db->sig[m->firstSignal + i];

      if (s.muxValue >= 0 && s.muxValue != muxNow) continue;

      DbcValue v = dbcDecodeSignal(*_db, s, f.data, f.len);
      if (!v.ok) continue;          /* declared past the end of this payload */

      p = putPrefix(p, t, f.id, ext);
      p = putStr(p, msgName);
      *p++ = ';';
      p = putStr(p, s.name);
      *p++ = ';';

      char *const valStart = p;
      if (v.label)      p = putStr(p, v.label);
      else if (v.exact) p = fmtFixed(p, v.scaled, v.dec);
      else              p = fmtDouble(p, v.fval);

      publishLive(_db, (uint16_t)(m->firstSignal + i), valStart,
                  (size_t)(p - valStart));

      *p++ = ';';
      p = putStr(p, s.unit);
      *p++ = ';';

#if CSV_INCLUDE_RAW
      if (!rawDone) { p = putPayload(p, f); rawDone = true; }
#endif
      *p++ = '\n';
      emitted++;
    }
  }

  /* ---- fields the CANopen standard itself defines ---------------------- */
#if CANOPEN_DECODE
  if (!emitted && !m) {
    CanopenField fld[CANOPEN_MAX_FIELDS];
    const uint8_t nf = canopenFields(f.id, f.data, f.len, fld, CANOPEN_MAX_FIELDS);
    for (uint8_t i = 0; i < nf && emitted < DECODE_MAX_ROWS; i++) {
      p = putPrefix(p, t, f.id, ext);
      p = putStr(p, msgName);
      *p++ = ';';
      p = putStr(p, fld[i].name);
      *p++ = ';';
      if (fld[i].label)   p = putStr(p, fld[i].label);
      else if (fld[i].hex) p = fmtHexValue(p, fld[i].value, 2);
      else                p = fmtU32(p, fld[i].value);
      *p++ = ';';
      p = putStr(p, fld[i].unit);
      *p++ = ';';
      if (!rawDone) { p = putPayload(p, f); rawDone = true; }
      *p++ = '\n';
      emitted++;
    }
  }
#endif

  /* ---- nothing decoded: keep the bytes --------------------------------- *
   * This is the normal path with no DBC on the card, and the safety net for
   * an identifier the map does not describe. Either way the frame is on the
   * card in full and can be decoded offline later.                         */
  if (!emitted) {
    p = putPrefix(p, t, f.id, ext);
    p = putStr(p, msgName);
    *p++ = ';';                    /* end of name   */
    *p++ = ';';                    /* signal, empty */
    *p++ = ';';                    /* value,  empty */
    *p++ = ';';                    /* unit,   empty */
    p = putPayload(p, f);
    *p++ = '\n';
    emitted = 1;
    g_bus.undecoded++;
  }

  (void)rawDone;
  if (rowsOut) *rowsOut = emitted;
  return (size_t)(p - buf);
}


/* ==========================================================================
 *  The CSV header, and the sidecar that explains it
 *
 *  These used to be one thing: a block of '#' comments above the column names.
 *  That made every recording self-describing, but it also made the file awkward
 *  to open - plenty of tools have no way to skip a comment block.
 *
 *  So the CSV now carries only its column names, and the legend moves to
 *  <n>.meta beside <n>.csv and <n>.log. JSON, so a tool reads it directly.
 *  The frame map is included, because the DBC that produced a recording may
 *  well have been edited by the time anyone reads it back.
 * ======================================================================== */
size_t csvColumnHeader(char *buf, size_t cap) {
  const int n = snprintf(buf, cap, "t_us;id;name;signal;value;unit;raw\n");
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

/* Appends at *n, keeping the overflow check in one place. */
static bool mAppend(char *buf, size_t cap, int *n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int k = vsnprintf(buf + *n, cap - (size_t)*n, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)(*n + k) >= cap) return false;
  *n += k;
  return true;
}

/* JSON string escaping. Signal names and units come from a user-supplied DBC,
 * so they are not trusted to be free of quotes or backslashes. */
static bool mStr(char *buf, size_t cap, int *n, const char *s) {
  if (!mAppend(buf, cap, n, "\"")) return false;
  for (const char *p = s; p && *p; ++p) {
    const char c = *p;
    bool ok;
    if      (c == '"')  ok = mAppend(buf, cap, n, "\\\"");
    else if (c == '\\') ok = mAppend(buf, cap, n, "\\\\");
    else if ((uint8_t)c < 0x20) continue;
    else                ok = mAppend(buf, cap, n, "%c", c);
    if (!ok) return false;
  }
  return mAppend(buf, cap, n, "\"");
}

size_t metaJson(char *buf, size_t cap, const char *csvName, const char *logName,
                const DbcDb &db) {
  int n = 0;

  if (!mAppend(buf, cap, &n,
      "{\n"
      "  \"firmware\": \"%s\",\n"
      "  \"version\": \"%s\",\n"
      "  \"csv\": \"%s\",\n"
      "  \"log\": \"%s\",\n"
      "  \"bus\": { \"bitrate_kbps\": %d, \"listen_only\": %d },\n",
      FIRMWARE_NAME, FIRMWARE_VERSION, csvName, logName,
      CAN_BITRATE_KBPS, CAN_LISTEN_ONLY ? 1 : 0)) return 0;

  if (!mAppend(buf, cap, &n,
      "  \"columns\": [\n"
      "    { \"name\": \"t_us\",   \"unit\": \"us\", \"zero\": \"start of this file\",\n"
      "      \"desc\": \"arrival time, captured in the CAN interrupt\" },\n"
      "    { \"name\": \"id\",     \"desc\": \"CAN identifier, hex\" },\n"
      "    { \"name\": \"name\",   \"desc\": \"message name from the frame map, "
      "empty if unmapped\" },\n"
      "    { \"name\": \"signal\", \"desc\": \"signal name, empty for a raw row\" },\n"
      "    { \"name\": \"value\",  \"desc\": \"scaled value, or the value-table "
      "label when one applies\" },\n"
      "    { \"name\": \"unit\",   \"desc\": \"unit from the frame map\" },\n"
      "    { \"name\": \"raw\",    \"desc\": \"payload bytes, hex; present once "
      "per frame\" }\n"
      "  ],\n"
      "  \"layout\": \"one row per SIGNAL; a frame with several signals "
      "produces several rows sharing t_us and id\",\n")) return 0;

  if (!mAppend(buf, cap, &n,
      "  \"dbc\": { \"loaded\": %d, \"path\": \"%s\", \"messages\": %u, "
      "\"signals\": %u, \"line_errors\": %u, \"overflow\": %d, \"inexact\": %d,\n"
      "    \"version\": ", db.loaded ? 1 : 0, DBC_PATH,
      (unsigned)db.msgCount, (unsigned)db.sigCount,
      (unsigned)db.lineErrors, db.overflow ? 1 : 0, db.inexact ? 1 : 0)) return 0;
  if (!mStr(buf, cap, &n, db.version)) return 0;
  if (!mAppend(buf, cap, &n, " },\n")) return 0;

  /* The frame map as it was when this recording was made. */
  if (!mAppend(buf, cap, &n, "  \"messages\": [\n")) return 0;
  for (uint16_t i = 0; i < db.msgCount; i++) {
    const DbcMessage &m = db.msg[i];
    if (!mAppend(buf, cap, &n, "    { \"id\": \"0x%lX\", \"ext\": %d, \"dlc\": %u, "
                 "\"name\": ", (unsigned long)m.id, m.ext ? 1 : 0,
                 (unsigned)m.dlc)) return 0;
    if (!mStr(buf, cap, &n, m.name)) return 0;
    if (!mAppend(buf, cap, &n, ", \"signals\": [")) return 0;
    for (uint16_t k = 0; k < m.signalCount; k++) {
      const DbcSignal &sg = db.sig[m.firstSignal + k];
      if (!mAppend(buf, cap, &n, "%s{ \"name\": ", k ? ", " : "")) return 0;
      if (!mStr(buf, cap, &n, sg.name)) return 0;
      if (!mAppend(buf, cap, &n, ", \"unit\": ")) return 0;
      if (!mStr(buf, cap, &n, sg.unit)) return 0;
      if (!mAppend(buf, cap, &n,
                   ", \"bits\": %u, \"signed\": %d, \"exact\": %d }",
                   (unsigned)sg.bits, sg.isSigned ? 1 : 0,
                   sg.exact ? 1 : 0)) return 0;
    }
    if (!mAppend(buf, cap, &n, "] }%s\n",
                 (i + 1 < db.msgCount) ? "," : "")) return 0;
  }
  if (!mAppend(buf, cap, &n, "  ],\n")) return 0;

  if (!mAppend(buf, cap, &n,
      "  \"notes\": [\n"
      "    \"An unmapped identifier is still logged: the row carries the raw "
      "payload and no signal name.\",\n"
      "    \"Values marked inexact in the dbc block were scaled in floating "
      "point; everything else is exact integer arithmetic.\",\n"
      "    \"t_us is the recorder's clock. If a node stamps its own time into "
      "the payload, prefer that for signal timing.\"\n"
      "  ]\n"
      "}\n")) return 0;

  return (size_t)n;
}

/* ==========================================================================
 *  The self-describing header
 *
 *  Every recording carries its own legend, including the frame map that was
 *  active when it was made. A CSV found on a card a year later is interpretable
 *  on its own, even if the DBC that produced it has since been edited.
 * ======================================================================== */
size_t csvHeaderBlock(char *buf, size_t cap, const char *filename,
                      const DbcDb &db) {
  int n = snprintf(buf, cap,
    "# =====================================================================\n"
    "# %s v%s  -  file: %s\n"
    "# Bus    : classical CAN, %d kbit/s, %s mode\n"
    "# Layout : one row per decoded signal, event based, ';' separated\n"
    "#\n"
    "# COLUMNS\n"
    "#   t_us    Recorder clock, microseconds since the START of THIS file.\n"
    "#           Monotonic, captured in the CAN interrupt, so it reflects the\n"
    "#           moment the frame arrived on the wire.\n"
    "#   id      CAN identifier, 0x notation. 29-bit ids print 8 digits.\n"
    "#   name    Message name from the frame map. Empty if unmapped.\n"
    "#   signal  Signal name from the frame map. Empty on a raw row.\n"
    "#   value   Physical value, or the symbolic name of that raw value.\n"
    "#   unit    Unit from the frame map. Often empty.\n"
    "#   raw     Payload bytes, hex. Always present when no signal could be\n"
    "#           decoded, so nothing on the bus is ever discarded.\n"
    "#\n"
    "# All rows of one frame share t_us and id: group on that pair.\n"
    "#\n",
    FIRMWARE_NAME, FIRMWARE_VERSION, filename, CAN_BITRATE_KBPS,
    CAN_LISTEN_ONLY ? "listen-only" : "normal");

  if (n < 0 || (size_t)n >= cap) return 0;

  if (!db.loaded) {
    const int k = snprintf(buf + n, cap - (size_t)n,
      "# FRAME MAP\n"
      "#   none - no %s was found on the card, so every frame was stored as\n"
      "#   raw payload bytes. Decode them offline against a DBC, or put one on\n"
      "#   the card and record again.\n"
      "# =====================================================================\n"
      "t_us;id;name;signal;value;unit;raw\n", DBC_PATH);
    if (k < 0 || (size_t)(n + k) >= cap) return 0;
    return (size_t)(n + k);
  }

  int k = snprintf(buf + n, cap - (size_t)n,
    "# FRAME MAP: %s\n"
    "#   %s\n"
    "#   %u messages, %u signals%s%s\n"
    "#\n",
    DBC_PATH, db.version[0] ? db.version : "(no VERSION string)",
    (unsigned)db.msgCount, (unsigned)db.sigCount,
    db.inexact  ? ", some values via floating point" : ", all values exact",
    db.overflow ? ", TRUNCATED - the map did not fit" : "");
  if (k < 0 || (size_t)(n + k) >= cap) return 0;
  n += k;

  for (uint16_t i = 0; i < db.msgCount; i++) {
    const DbcMessage &m = db.msg[i];
    k = snprintf(buf + n, cap - (size_t)n, "#   0x%0*lX %-24s",
                 m.ext ? 8 : 3, (unsigned long)m.id, m.name);
    if (k < 0 || (size_t)(n + k) >= cap) return 0;
    n += k;

    for (uint16_t j = 0; j < m.signalCount; j++) {
      const DbcSignal &s = db.sig[m.firstSignal + j];
      k = snprintf(buf + n, cap - (size_t)n, "%s%s%s%s",
                   j ? ", " : " ", s.name,
                   s.unit[0] ? " " : "", s.unit);
      if (k < 0 || (size_t)(n + k) >= cap) return 0;
      n += k;
    }
    k = snprintf(buf + n, cap - (size_t)n, "\n");
    if (k < 0 || (size_t)(n + k) >= cap) return 0;
    n += k;
  }

  k = snprintf(buf + n, cap - (size_t)n,
    "# =====================================================================\n"
    "t_us;id;name;signal;value;unit;raw\n");
  if (k < 0 || (size_t)(n + k) >= cap) return 0;
  return (size_t)(n + k);
}
