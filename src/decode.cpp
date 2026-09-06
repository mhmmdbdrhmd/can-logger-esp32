#include "decode.h"

#include <stdlib.h>
#include <stdarg.h>
#include "fmt.h"
#include "config.h"

#if CANOPEN_DECODE
#include "canopen.h"
#endif

BusStats    g_bus[CAN_BUSES];
LiveSignals g_live[CAN_BUSES];

/* ==========================================================================
 *  Live bus activity
 * ======================================================================== */
void busReset(BusStats &b) {
  memset(&b, 0, sizeof(b));
}

void liveReset(LiveSignals &l) {
  if (l.text)   memset(l.text,   0, (size_t)l.cap * LIVE_TEXT_MAX);
  if (l.lastMs) memset(l.lastMs, 0, (size_t)l.cap * sizeof(uint32_t));
  if (l.seen)   memset(l.seen,   0, (size_t)l.cap);
  l.seenCount = 0;
}

void liveFree(LiveSignals &l) {
  free(l.text); free(l.lastMs); free(l.seen);
  l.text = nullptr; l.lastMs = nullptr; l.seen = nullptr;
  l.cap = 0; l.seenCount = 0;
}

bool liveAllocate(LiveSignals &l, uint16_t signals) {
  liveFree(l);
  if (!signals) return true;

  l.text   = (char (*)[LIVE_TEXT_MAX])calloc(signals, LIVE_TEXT_MAX);
  l.lastMs = (uint32_t *)calloc(signals, sizeof(uint32_t));
  l.seen   = (uint8_t  *)calloc(signals, 1);

  if (!l.text || !l.lastMs || !l.seen) {
    /* The live view is a convenience; the recording is not. Give the memory
     * back and carry on with an empty one rather than failing the load. */
    liveFree(l);
    return false;
  }
  l.cap = signals;
  return true;
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

  /* And as bytes, for the transmit path - see the note in decode.h. */
  memcpy(b.lastData[slot], f.data, 8);
  b.lastLen[slot] = n;
}

uint8_t busFormatIds(const BusStats &b, uint8_t *cursor, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';                 /* an empty table must not leave stack junk */

  const uint8_t used = b.used;
  if (!used || !cursor) return 0;
  if (*cursor >= used) *cursor = 0;

  size_t  k     = 0;
  uint8_t shown = 0;

  for (uint8_t n = 0; n < used; n++) {
    const uint8_t i = (uint8_t)((*cursor + n) % used);

    /* snprintf returns what it WANTED to write, not what it wrote. Adding that
     * blind walks k past the end and leaves the last entry cut off mid-number,
     * which then reads as a plausible count and is not one. */
    const int w = snprintf(out + k, cap - k, "0x%lX=%lu(%lu/s)%s ",
                           (unsigned long)b.id[i],
                           (unsigned long)b.count[i],
                           (unsigned long)b.rate[i],
                           b.known[i] ? "" : "?");
    if (w < 0 || (size_t)w >= cap - k) { out[k] = '\0'; break; }
    k += (size_t)w;
    shown++;
  }

  /* Advance even when nothing fitted, so a single over-long entry cannot pin
   * the window on itself for ever. */
  *cursor = (uint8_t)((*cursor + (shown ? shown : 1)) % used);
  return shown;
}

bool busLastPayload(const BusStats &b, uint32_t id, bool ext,
                    uint8_t *out, uint8_t *lenOut) {
  for (uint8_t i = 0; i < b.used; i++) {
    if (b.id[i] != id || (bool)b.ext[i] != ext) continue;
    if (!b.lastLen[i]) return false;
    if (out)    memcpy(out, b.lastData[i], 8);
    if (lenOut) *lenOut = b.lastLen[i];
    return true;
  }
  return false;
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
static void publishLive(const DbcDb *db, uint8_t bus, uint16_t sigIndex,
                        const char *text, size_t len) {
  if (!db || bus >= CAN_BUSES) return;
  LiveSignals &l = g_live[bus];
  if (sigIndex >= l.cap) return;
  if (len > LIVE_TEXT_MAX - 1) len = LIVE_TEXT_MAX - 1;

  memcpy(l.text[sigIndex], text, len);
  l.text[sigIndex][len] = '\0';
  l.lastMs[sigIndex]    = millis();
  if (!l.seen[sigIndex]) {
    l.seen[sigIndex] = 1;
    l.seenCount++;
  }
}

/* t_us;bus;id;  - the three columns every row starts with.
 *
 * The bus is printed one-based. Internally it is an index; on the wiring
 * diagram, on the case and in every conversation about the machine it is
 * "CAN 1" and "CAN 2", and a log that disagrees with the label on the
 * connector is a log people misread. */
static char *putPrefix(char *p, int64_t t, uint8_t bus, uint32_t id, bool ext) {
  p = fmtI64(p, t);
  *p++ = ';';
  *p++ = (char)('1' + (bus < CAN_BUSES ? bus : 0));
  *p++ = ';';
  p = fmtCanId(p, id, ext);
  *p++ = ';';
  return p;
}

void Decoder::reset(DbcDb *db) {
  _db    = db;                    /* an array of CAN_BUSES maps */
  _have  = false;
  _epoch = 0;
  if (db) for (uint8_t b = 0; b < CAN_BUSES; b++) dbcResetRuntime(db[b]);
}

size_t Decoder::rows(const CanFrame &f, char *buf, size_t cap, uint8_t *rowsOut) {
  if (rowsOut) *rowsOut = 0;
  if (cap < DECODE_FRAME_MAX) return 0;

  if (!_have) { _epoch = f.esp_us; _have = true; }
  const int64_t t = (int64_t)(f.esp_us - _epoch);
  const bool    ext = (f.ext != 0);
  const uint8_t bus = (f.bus < CAN_BUSES) ? f.bus : 0;

  /* THE frame map for this frame: the one belonging to the bus it arrived on.
   * Decoding a CAN2 frame against CAN1's map would produce a row that looks
   * perfectly plausible and is wrong, which is the one failure mode a logger
   * must not have. */
  DbcDb *const db = _db ? &_db[bus] : nullptr;

  char *p = buf;
  uint8_t emitted = 0;
  bool    rawDone = false;

  const DbcMessage *m = db ? dbcFind(*db, f.id, ext) : nullptr;
  const char *msgName = m ? m->name : nullptr;

  /* A frame this logger sent is written into the same file, in the same
   * schema, with its message name prefixed. The columns are unchanged - a
   * reader can tell a command apart from the data around it by looking at the
   * name, which is where a reader would look anyway. */
  const bool isTx = (f.tx != 0);

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
      DbcValue mv = dbcDecodeSignal(*db, db->sig[m->muxSignal], f.data, f.len);
      if (mv.ok) muxNow = (int32_t)(mv.exact ? mv.scaled : (int64_t)mv.fval);
    }

    for (uint16_t i = 0; i < m->signalCount && emitted < DECODE_MAX_ROWS; i++) {
      DbcSignal &s = db->sig[m->firstSignal + i];

      if (s.muxValue >= 0 && s.muxValue != muxNow) continue;

      DbcValue v = dbcDecodeSignal(*db, s, f.data, f.len);
      if (!v.ok) continue;          /* declared past the end of this payload */

      p = putPrefix(p, t, bus, f.id, ext);
      if (isTx) p = putStr(p, "TX:");
      p = putStr(p, msgName);
      *p++ = ';';
      p = putStr(p, s.name);
      *p++ = ';';

      char *const valStart = p;
      if (v.label)      p = putStr(p, v.label);
      else if (v.exact) p = fmtFixed(p, v.scaled, v.dec);
      else              p = fmtDouble(p, v.fval);

      /* Deliberately not published to the live view. What the dashboard shows
       * is what the bus said, not what this logger asked for - a gauge that
       * jumps to the commanded value the moment Send is pressed would be
       * reporting the request as though it were the answer. */
      if (!isTx) {
        publishLive(db, bus, (uint16_t)(m->firstSignal + i), valStart,
                    (size_t)(p - valStart));
      }

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
      p = putPrefix(p, t, bus, f.id, ext);
      if (isTx) p = putStr(p, "TX:");
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
    p = putPrefix(p, t, bus, f.id, ext);
    if (isTx) p = putStr(p, "TX:");
    p = putStr(p, msgName);
    *p++ = ';';                    /* end of name   */
    *p++ = ';';                    /* signal, empty */
    *p++ = ';';                    /* value,  empty */
    *p++ = ';';                    /* unit,   empty */
    p = putPayload(p, f);
    *p++ = '\n';
    emitted = 1;
    if (!isTx) g_bus[bus].undecoded++;
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
  const int n = snprintf(buf, cap, "t_us;bus;id;name;signal;value;unit;raw\n");
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

/* The frame map of ONE bus, as it was when this recording was made. */
static bool metaBus(char *buf, size_t cap, int *n, uint8_t b, const DbcDb &db) {
  static const char *const kPath[CAN_BUSES] = { DBC_PATH, DBC2_PATH };
  static const uint16_t    kRate[CAN_BUSES] = { CAN1_BITRATE_KBPS, CAN2_BITRATE_KBPS };
  static const uint8_t     kListen[CAN_BUSES] = { CAN1_LISTEN_ONLY, CAN2_LISTEN_ONLY };

  if (!mAppend(buf, cap, n,
      "    { \"bus\": %u, \"bitrate_kbps\": %u, \"listen_only\": %d,\n"
      "      \"dbc\": { \"loaded\": %d, \"path\": \"%s\", \"messages\": %u, "
      "\"signals\": %u, \"line_errors\": %u, \"overflow\": %d, \"inexact\": %d,\n"
      "        \"version\": ",
      (unsigned)(b + 1), (unsigned)kRate[b], kListen[b] ? 1 : 0,
      db.loaded ? 1 : 0, kPath[b],
      (unsigned)db.msgCount, (unsigned)db.sigCount,
      (unsigned)db.lineErrors, db.overflow ? 1 : 0, db.inexact ? 1 : 0)) return false;
  if (!mStr(buf, cap, n, db.version)) return false;
  if (!mAppend(buf, cap, n, " },\n      \"messages\": [\n")) return false;

  for (uint16_t i = 0; i < db.msgCount; i++) {
    const DbcMessage &m = db.msg[i];
    if (!mAppend(buf, cap, n, "        { \"id\": \"0x%lX\", \"ext\": %d, "
                 "\"dlc\": %u, \"name\": ", (unsigned long)m.id, m.ext ? 1 : 0,
                 (unsigned)m.dlc)) return false;
    if (!mStr(buf, cap, n, m.name)) return false;
    if (!mAppend(buf, cap, n, ", \"signals\": [")) return false;
    for (uint16_t k = 0; k < m.signalCount; k++) {
      const DbcSignal &sg = db.sig[m.firstSignal + k];
      if (!mAppend(buf, cap, n, "%s{ \"name\": ", k ? ", " : "")) return false;
      if (!mStr(buf, cap, n, sg.name)) return false;
      if (!mAppend(buf, cap, n, ", \"unit\": ")) return false;
      if (!mStr(buf, cap, n, sg.unit)) return false;
      if (!mAppend(buf, cap, n,
                   ", \"bits\": %u, \"signed\": %d, \"exact\": %d }",
                   (unsigned)sg.bits, sg.isSigned ? 1 : 0,
                   sg.exact ? 1 : 0)) return false;
    }
    if (!mAppend(buf, cap, n, "] }%s\n",
                 (i + 1 < db.msgCount) ? "," : "")) return false;
  }
  return mAppend(buf, cap, n, "      ] }");
}

size_t metaJson(char *buf, size_t cap, const char *csvName, const char *logName,
                const DbcDb *db) {
  int n = 0;

  /* "schema" is the field a tool should branch on. Recordings from the
   * single-bus logger have no bus column and no such key; anything carrying
   * schema 2 has eight columns, the second of which is the bus. */
  if (!mAppend(buf, cap, &n,
      "{\n"
      "  \"firmware\": \"%s\",\n"
      "  \"version\": \"%s\",\n"
      "  \"schema\": 2,\n"
      "  \"csv\": \"%s\",\n"
      "  \"log\": \"%s\",\n"
      "  \"buses\": %u,\n",
      FIRMWARE_NAME, FIRMWARE_VERSION, csvName, logName,
      (unsigned)CAN_BUSES)) return 0;

  if (!mAppend(buf, cap, &n,
      "  \"columns\": [\n"
      "    { \"name\": \"t_us\",   \"unit\": \"us\", \"zero\": \"start of this file\",\n"
      "      \"desc\": \"arrival time, captured in the CAN interrupt; one clock "
      "for both buses\" },\n"
      "    { \"name\": \"bus\",    \"desc\": \"which CAN bus the frame arrived "
      "on, 1 or 2\" },\n"
      "    { \"name\": \"id\",     \"desc\": \"CAN identifier, hex\" },\n"
      "    { \"name\": \"name\",   \"desc\": \"message name from that bus's frame "
      "map, empty if unmapped\" },\n"
      "    { \"name\": \"signal\", \"desc\": \"signal name, empty for a raw row\" },\n"
      "    { \"name\": \"value\",  \"desc\": \"scaled value, or the value-table "
      "label when one applies\" },\n"
      "    { \"name\": \"unit\",   \"desc\": \"unit from the frame map\" },\n"
      "    { \"name\": \"raw\",    \"desc\": \"payload bytes, hex; present once "
      "per frame\" }\n"
      "  ],\n"
      "  \"layout\": \"one row per SIGNAL; a frame with several signals "
      "produces several rows sharing t_us, bus and id\",\n")) return 0;

  if (!mAppend(buf, cap, &n, "  \"can\": [\n")) return 0;
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    if (!metaBus(buf, cap, &n, b, db[b])) return 0;
    if (!mAppend(buf, cap, &n, "%s\n", (b + 1 < CAN_BUSES) ? "," : "")) return 0;
  }
  if (!mAppend(buf, cap, &n, "  ],\n")) return 0;

  if (!mAppend(buf, cap, &n,
      "  \"notes\": [\n"
      "    \"An unmapped identifier is still logged: the row carries the raw "
      "payload and no signal name.\",\n"
      "    \"Group on (t_us, bus, id). An identifier alone does not name a "
      "message when two buses are recorded.\",\n"
      "    \"Values marked inexact in the dbc block were scaled in floating "
      "point; everything else is exact integer arithmetic.\",\n"
      "    \"t_us is the recorder's clock, shared by both buses, so rows from "
      "CAN1 and CAN2 are directly comparable.\"\n"
      "  ]\n"
      "}\n")) return 0;

  return (size_t)n;
}
