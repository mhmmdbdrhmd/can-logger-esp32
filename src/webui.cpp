#include "webui.h"
#include "config.h"
#include "recorder.h"
#include "decode.h"
#include "logger.h"
#include "netcfg.h"
#include "dbc.h"
#include "dash.h"
#include "dashstore.h"
#include "cantx.h"
#include "webpage.h"
#include "webpage_gz.h"
#include "mem.h"

#include <SD.h>
#include <WebServer.h>
#include <lwip/sockets.h>
#include <sys/time.h>       /* struct timeval for SO_SNDTIMEO; SO_LINGER is in lwip */
#include <ESPmDNS.h>

static WebServer *s_srv = nullptr;

/* ==========================================================================
 *  Handlers
 * ======================================================================== */
/* ------------------------------------------------------------------------ *
 *  One way out for every JSON response.
 *
 *  Handing a whole response to send() is a single WiFiClient::write(). lwIP's
 *  send buffer is a few kilobytes, so anything larger asks it to accept more
 *  than it can hold: the write returns EAGAIN, the body is cut short, and the
 *  Content-Length header still promises the rest. A browser told to expect
 *  15 KB and given 4 KB does not error - it WAITS. The fetch never settles,
 *  the page shows neither data nor "connection lost", and nothing is logged on
 *  either side. That silence is the reason this took so long to find.
 *
 *  Sliced, with a yield between slices so the TCP task can drain, the promise
 *  in the header is one the writes can keep.                                */
static void sendJson(const String &j) {
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->setContentLength(j.length());
  s_srv->send(200, "application/json", "");

  const char  *body = j.c_str();
  const size_t len  = j.length();
  /* By value: WebServer::client() returns a copy, and the copy shares the
   * underlying socket through a shared_ptr - so connected() reports on the
   * real connection and stop() really closes it. */
  WiFiClient   cl   = s_srv->client();
  const uint32_t started = millis();
  for (size_t off = 0; off < len; off += 2048) {
    /* Same reason as handleRoot: a write to a client that has gone, or has
     * merely stopped reading, costs up to ten seconds per slice inside
     * WiFiClient::write() - on the loop task, which is also the only thing
     * that accepts new connections. Both cheap to check, so check both. */
    /* Returning is enough: handleClient() drops the client as soon as this
     * request finishes. cl.stop() would only clear this COPY's flag - the
     * server holds its own reference, so the socket would stay open. */
    if (!cl.connected()) return;
    if (millis() - started > WEB_SEND_MAX_MS) return;
    const size_t n = (len - off < 2048) ? (len - off) : 2048;
    s_srv->sendContent(body + off, n);
    delay(0);
  }
}

static void handleRoot() {
  s_srv->sendHeader("Cache-Control", "no-store");

  /* ---- THE PAGE, SENT SO THAT A SLOW CLIENT CANNOT WEDGE THE SERVER -----
   *
   * The page is stored pre-compressed (src/webpage_gz.h, about 52 KB against
   * 170 KB of source) and goes out in WEB_PAGE_SLICE pieces with a real
   * Content-Length. Each of those choices fixes a failure that was measured in
   * station mode, where the browser is a router hop away rather than on the
   * board's own hotspot:
   *
   *  - ONE WRITE PER PART was up to 73 KB handed to WiFiClient::write(). lwIP's
   *    send buffer is a few kilobytes; once the window filled the write came
   *    back EAGAIN - `write(): fail on fd 50, errno: 11` - and the page died
   *    half-sent. On the hotspot the client drains fast enough to hide it.
   *
   *  - A CHUNKED response has to be terminated after the handler returns: three
   *    more writes to a socket this function may have just given up on. A
   *    Content-Length has no terminator and no framing to desynchronise.
   *
   *  - NOTHING STOPPED THE LOOP. sendContent_P() ignores short writes, and
   *    WiFiClient::write() retries a full buffer ten times with a one-second
   *    wait each, so a client that stopped reading held this handler - and with
   *    it every other request, since the server takes one client at a time -
   *    for up to ten seconds a slice. So: SO_SNDTIMEO makes a stalled write
   *    return, and between slices the send gives up if the socket has closed or
   *    WEB_SEND_MAX_MS has passed. connected() alone is not enough: a client
   *    that timed out without closing still reads as connected.
   *
   *  - AN ABANDONED SEND held its queued bytes. A polite close keeps them and
   *    retransmits for minutes, from the same heap an inbound connection needs
   *    its ~2.3 KB receive buffer from, so the board stopped accepting anything.
   *    SO_LINGER with a zero timeout turns that close into a reset and the
   *    memory comes straight back.
   *
   * What none of this can fix: if the heap has no room for a receive buffer
   * the connection is refused before any handler runs. That is a budgeting
   * question - see MEM_WEB_SERVES in config.h - not a sending one. */
  WiFiClient cl = s_srv->client();   /* a copy, sharing the same socket */

  memSample();

#if WEB_SEND_SLICE_TIMEOUT_MS
  if (cl.fd() >= 0) {
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = WEB_SEND_SLICE_TIMEOUT_MS * 1000;
    cl.setSocketOption(SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
#endif

  const size_t SLICE = WEB_PAGE_SLICE;
  bool   gone = false, slow = false;
  size_t sent = 0;
  const uint32_t started = millis();

#if WEB_PAGE_GZIP
  s_srv->sendHeader("Content-Encoding", "gzip");
  s_srv->setContentLength(PAGE_GZ_LEN);
  s_srv->send(200, "text/html", "");
  for (size_t off = 0; off < PAGE_GZ_LEN; off += SLICE) {
    if (!cl.connected())                        { gone = true; break; }
    if (millis() - started > WEB_SEND_MAX_MS)   { slow = true; break; }
    const size_t n = (PAGE_GZ_LEN - off < SLICE) ? (PAGE_GZ_LEN - off) : SLICE;
    s_srv->sendContent_P((PGM_P)(PAGE_GZ + off), n);
    sent += n;
    delay(0);            /* let the TCP task drain what was just queued */
  }
#else
  static size_t pageBytes = 0;
  if (!pageBytes) {
    for (uint8_t i = 0; i < PAGE_PART_COUNT; i++) pageBytes += strlen(PAGE_PARTS[i]);
  }
  s_srv->setContentLength(pageBytes);
  s_srv->send(200, "text/html", "");
  for (uint8_t i = 0; i < PAGE_PART_COUNT && !gone && !slow; i++) {
    const char  *part = PAGE_PARTS[i];
    const size_t len  = strlen(part);
    for (size_t off = 0; off < len; off += SLICE) {
      if (!cl.connected())                      { gone = true; break; }
      if (millis() - started > WEB_SEND_MAX_MS) { slow = true; break; }
      const size_t n = (len - off < SLICE) ? (len - off) : SLICE;
      s_srv->sendContent_P(part + off, n);
      sent += n;
      delay(0);
    }
  }
#endif

  if (gone || slow) {
    LOG_LIVE(LVL_WARN, "%s %u KB into the page (%lu ms) - stopped sending, so "
                       "other requests are not held up behind it",
             gone ? "the browser dropped the connection"
                  : "the client stopped reading",
             (unsigned)(sent / 1024), (unsigned long)(millis() - started));
    /* No cl.stop(): it would only release this copy. handleClient() closes the
     * real socket when this returns, and the linger setting makes that close a
     * reset rather than minutes of retransmitting to nobody. */
    if (cl.fd() >= 0) {
      struct linger sl;
      sl.l_onoff  = 1;
      sl.l_linger = 0;
      cl.setSocketOption(SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    }
  }
  memSample();
}

/* Appends a JSON string body (no surrounding quotes). Names and units come out
 * of a file the user wrote, so they cannot be trusted to be JSON-safe. */
static void jsonStr(String &out, const char *s) {
  for (const char *p = s; *p; ++p) {
    if      (*p == '"')  out += "\\\"";
    else if (*p == '\\') out += "\\\\";
    else if ((uint8_t)*p >= 0x20 && (uint8_t)*p < 0x7F) out += *p;
  }
}

/* One bus's worth of status: its health, the identifiers it has seen and the
 * signals its own frame map decoded. Emitted as an element of the "bus" array
 * rather than as two sets of suffixed top-level keys, so the page renders it
 * with one loop and adding a third controller would be a config change. */
static void statusBus(String &j, uint8_t b) {
  const BusHealth  &h = g_rec.bus[b];
  const BusStats   &bs = g_bus[b];
  const DbcDb      &db = g_dbc[b];
  const LiveSignals &lv = g_live[b];

  j += "{\"b\":";        j += (uint32_t)(b + 1);
  j += ",\"on\":";       j += h.present ? 1 : 0;
  /* Separate from "on": a bus can be off because it was compiled out or
   * because nothing answered, and only the second one is worth a wiring
   * check. */
  j += ",\"en\":";       j += h.enabled ? 1 : 0;
  j += ",\"kbps\":";     j += (uint32_t)h.bitrateKbps;
  j += ",\"xtal\":";     j += (uint32_t)h.crystalMHz;
  /* Where the rate above came from. A detected rate is what the machine said;
   * a fallen-back one is what somebody typed and the bus never confirmed, and
   * the page has to be able to tell a reader which they are looking at. */
  j += ",\"auto\":";     j += h.autoDetect ? 1 : 0;
  j += ",\"autoOk\":";   j += h.autoFound ? 1 : 0;
  j += ",\"send\":";     j += h.listenOnly ? 0 : 1;
  j += ",\"alive\":";    j += h.canOk ? 1 : 0;
  j += ",\"fps\":";      j += h.frameRate;
  j += ",\"irq\":";      j += h.irqRate;
  j += ",\"intStuck\":"; j += h.intStuck ? 1 : 0;
  j += ",\"intLevel\":"; j += (uint32_t)h.intLevel;
  j += ",\"load\":";     j += h.busLoadPct;

  /* The controller's overflow floor, on its own. Deliberately NOT added to the
   * queue-drop count here: one is a floor and the other is exact, and the page
   * has to be able to say which is which. */
  j += ",\"ovf\":";      j += h.canOvfFramesMin;
  j += ",\"ovfEv\":";    j += h.canOvfEvents;
  j += ",\"sticky\":";   j += h.canIntfSticky;
  j += ",\"dbc\":";      j += h.dbcLoaded ? 1 : 0;
  j += ",\"dbcMsg\":";   j += (uint32_t)h.dbcMessages;
  j += ",\"dbcSig\":";   j += (uint32_t)h.dbcSignals;

  /* ---- identifiers actually seen on this bus, with their latest payload -- */
  j += ",\"idMore\":"; j += bs.untracked ? 1 : 0;
  j += ",\"ids\":[";
  char id[16];
  for (uint8_t i = 0; i < bs.used; i++) {
    if (i) j += ',';
    snprintf(id, sizeof(id), bs.ext[i] ? "0x%08lX" : "0x%03lX",
             (unsigned long)bs.id[i]);
    j += "{\"id\":\""; j += id; j += '"';
    j += ",\"d\":\"";  j += bs.last[i]; j += '"';
    j += ",\"n\":";    j += (uint32_t)bs.count[i];
    j += ",\"r\":";    j += (uint32_t)bs.rate[i];
    j += ",\"k\":";    j += bs.known[i] ? 1 : 0;
    j += '}';
  }
  j += ']';

  /* ---- live decoded signals, straight out of THIS bus's frame map -------
   * Nothing here knows what any of these are. The names, units and order all
   * come from the DBC on the card, so the page shows a drive, a weather
   * station or a test rig without a line of firmware changing. */
  uint16_t shown = 0;
  j += ",\"sig\":[";
  for (uint16_t mi = 0; mi < db.msgCount && shown < WEB_MAX_SIGNALS; mi++) {
    const DbcMessage &m = db.msg[mi];
    for (uint16_t k = 0; k < m.signalCount && shown < WEB_MAX_SIGNALS; k++) {
      const uint16_t si = m.firstSignal + k;
      if (si >= lv.cap || !lv.seen[si]) continue;

      if (shown) j += ',';
      j += "{\"m\":\""; jsonStr(j, m.name);
      j += "\",\"s\":\""; jsonStr(j, db.sig[si].name);
      j += "\",\"v\":\""; jsonStr(j, lv.text[si]);
      j += "\",\"u\":\""; jsonStr(j, db.sig[si].unit);
      j += "\"}";
      shown++;
    }
  }
  j += ']';
  j += ",\"sigMore\":"; j += (lv.seenCount > shown) ? 1 : 0;
  j += '}';
}

static void handleStatus() {
  /* A FLOOR, not a total: the controllers' overflow flags are sticky and say
   * only that it happened, never how often. The queue-drop half IS exact.
   * Summed here for the headline figure, and available separately per bus so
   * the page can show where it came from. */
  uint32_t lost = g_rec.queueDropped;
  for (uint8_t b = 0; b < CAN_BUSES; b++) lost += g_rec.bus[b].canOvfFramesMin;

  /* STATIC, and reserved at what the reply MEASURES rather than at its
   * theoretical ceiling.
   *
   * Two mistakes, one after the other, and both are worth keeping written
   * down because they pull in opposite directions.
   *
   * It began as a local String with reserve(4096). Arduino's String does not
   * grow geometrically - concat() calls reserve(needed) and reserve() reallocs
   * to exactly that - so every append past the reservation moved the whole
   * buffer again, a thousand times a reply, five times a second. Static fixed
   * that, and static is still right.
   *
   * The reservation was then raised to the worst case the ceilings allow:
   *
   *     1024 + CAN_BUSES * (BUS_TRACK_IDS * 80 + WEB_MAX_SIGNALS * 120)
   *   = 1024 + 2 * (24 * 80 + 48 * 120) = 16384 bytes
   *
   * against a reply that measures 2111-2126 bytes in practice. Eight times
   * over-asked, as one unbroken run of heap. Once the frame maps had
   * fragmented DRAM that allocation stopped succeeding and /api/status
   * answered 503 for the rest of the run - in 15 of 20 test runs, on
   * every map from v1 (16 KB of tables) upward and never on v0 or with no map.
   *
   * 4096 is double the measured reply. If a bus ever tracks enough signals to
   * pass it the String grows once more and stays grown, which is the same
   * property the worst-case reservation was after and does not need 16 KB of
   * contiguous heap to get.
   *
   * Sent with a real Content-Length through sendJson(), NOT chunked. Chunked
   * framing has to be terminated, and terminating it means another write to a
   * socket that may already be dead - see handleRoot for what that costs. */
  const uint32_t blockNow = memSample();

  static String j;
  if (!j.reserve(4096)) {
    LOG_LIVE(LVL_ERROR, "/api/status: could not reserve 4096 bytes for the "
                        "reply - largest free block is %lu, total free %lu. "
                        "The dashboard will show this as a failed poll.",
             (unsigned long)blockNow, (unsigned long)memStat().freeNow);
    s_srv->send(503, "text/plain", "out of memory");
    return;
  }

  j  = "{\"sd\":";      j += g_rec.sdOk ? 1 : 0;
  j += ",\"sdErr\":";   j += g_rec.sdError ? 1 : 0;
  j += ",\"sdType\":\"";j += g_rec.sdType; j += '"';
  j += ",\"sdMB\":";    j += (uint32_t)g_rec.sdSizeMB;
  j += ",\"rec\":";     j += g_rec.recording ? 1 : 0;
  j += ",\"file\":\"";  j += (const char *)(g_rec.csvName[0] ? g_rec.csvName + 1 : "-");
  j += '"';
  j += ",\"elapsed\":"; j += recorderElapsedMs() / 1000UL;
  j += ",\"rows\":";    j += (uint32_t)g_rec.rows;
  j += ",\"kb\":";      j += (uint32_t)(g_rec.bytes / 1024ULL);
  j += ",\"pf\":";      j += g_rec.powerFail ? 1 : 0;
  j += ",\"risk\":";    j += (uint32_t)SD_SYNC_INTERVAL_MS;

  /* ---- shared between the buses: one queue, one card, one reader task ---- */
  j += ",\"lost\":";    j += lost;
  j += ",\"qDrop\":";   j += g_rec.queueDropped;
  j += ",\"qPeak\":";   j += g_rec.queuePeak;
  j += ",\"qLen\":";    j += (uint32_t)FRAME_QUEUE_LEN;

  /* The number the dual-bus design turns on: worst microseconds spent draining
   * BOTH controllers in one pass, against a ~200 us deadline at 500 kbit/s. */
  j += ",\"drain\":";   j += g_rec.drainMaxUs;
  j += ",\"wrMax\":";   j += g_rec.writeMaxUs;

  /* Named "can" rather than "bus" because "bus" reads as a single thing and
   * this is the list of controllers. The one remaining top-level "bus" in this
   * API is a transmit outcome saying which one a frame went out on. */
  j += ",\"can\":[";
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    if (b) j += ',';
    statusBus(j, b);
  }
  j += ']';

  j += ",\"ap\":";      j += netIsAp() ? 1 : 0;
  j += ",\"ip\":\"";    j += netIp(); j += '"';
  j += ",\"up\":";      j += millis();
  /* Three numbers, not one. The total is the reassuring one and the
   * useless one; `heapMax` is the largest block that can still be
   * handed out, which is what decides whether the next allocation
   * succeeds, and `heapLow` is the worst that block has ever been -
   * so a squeeze that has since recovered is still visible. */
  {
    const MemStat mm = memStat();
    j += ",\"heap\":";    j += mm.freeNow;
    j += ",\"heapMax\":"; j += mm.largest;
    j += ",\"heapLow\":"; j += mm.lowBlock;
    j += ",\"heapMin\":"; j += mm.minFree;
  }
  /* Passes of appLoop() a second. Under a few tens the web server is not being
   * scheduled - which is what a browser sees as "the logger stopped answering"
   * even though the recording is fine. */
  j += ",\"web\":";     j += (uint32_t)g_rec.loopRate;
  j += ",\"fw\":\"";    j += FIRMWARE_NAME " v" FIRMWARE_VERSION; j += "\"}";

  sendJson(j);
}

static void handleLog() {
  const uint32_t since = s_srv->hasArg("since")
                       ? (uint32_t)strtoul(s_srv->arg("since").c_str(), nullptr, 10) : 0;
  String lines;
  lines.reserve(2048);
  const uint32_t seq = webLogToJson(since, lines);

  String j;
  j.reserve(lines.length() + 40);
  j  = "{\"seq\":"; j += seq;
  j += ",\"lines\":["; j += lines; j += "]}";

  s_srv->sendHeader("Cache-Control", "no-store");
  sendJson(j);
}

static void handleStart() {
  recorderRequestStart();
  LOG_LIVE(LVL_INFO, "start requested from the web dashboard");
  s_srv->send(200, "application/json", "{\"ok\":1}");
}

static volatile bool s_wantReboot = false;

bool webRebootRequested() { return s_wantReboot; }

static void handleReboot() {
  /* Answer first, restart later. Rebooting inside the handler would drop the
   * connection before the browser sees a reply, and would abandon an open CSV
   * mid-write. appLoop() picks this up and shuts down properly. */
  s_srv->send(200, "application/json", "{\"ok\":1}");
  LOG_LIVE(LVL_WARN, "REBOOT requested from the web dashboard");
  s_wantReboot = true;
}

static void handleStop() {
  recorderRequestStop();
  LOG_LIVE(LVL_INFO, "stop requested from the web dashboard");
  s_srv->send(200, "application/json", "{\"ok\":1}");
}


/* ==========================================================================
 *  The dashboard
 *
 *  Three endpoints, deliberately shaped around what costs the ESP32 time:
 *
 *    /api/dash        polled several times a second. Carries ONLY the cells
 *                     that are actually configured, as the text the decode
 *                     task already rendered - so the fast path copies strings
 *                     and does no decoding, no formatting and no float work.
 *    /api/dash/cfg    the layout, as the same text that lives on the card.
 *                     One format, one parser, and Export/Import are then just
 *                     this endpoint's body.
 *    /api/signals     the picker list. Fetched once, when the editor opens.
 *                     Streamed rather than assembled, because a full frame map
 *                     is bigger than anything else this firmware puts in RAM.
 * ======================================================================== */

/* Bumped whenever the layout is saved, so a second browser notices that the
 * first one changed it instead of quietly showing a stale grid. */
static uint32_t s_dashGen = 1;

static void handleDash() {
  /* THE endpoint the Dashboard tab polls - the busiest response this firmware
   * produces, and it was reserved at 1 KB. Three arrays of DASH_MAX_CELLS plus
   * a copy of the health block is several times that, and Arduino's String
   * reallocs to the exact size on every append past the reservation. Sized
   * from the grid it actually describes, and static so the buffer is not
   * taken and returned on every poll. */
  static const size_t DASH_RESERVE = 1024                    /* health block */
                                   + (size_t)DASH_MAX_CELLS * 48;
  const uint32_t dashBlockNow = memSample();
  static String j;
  if (!j.reserve(DASH_RESERVE)) {
    LOG_LIVE(LVL_ERROR, "/api/dash: could not reserve %lu bytes for the reply - "
                        "largest free block is %lu, total free %lu",
             (unsigned long)DASH_RESERVE, (unsigned long)dashBlockNow,
             (unsigned long)memStat().freeNow);
    s_srv->send(503, "text/plain", "out of memory");
    return;
  }

  const uint8_t cells = dashCellCount(g_dash);
  const uint32_t now  = millis();

  /* Note what is NOT here: the grid's own cols and rows. They were, and they
   * collided with the recording's row count under the same name - the document
   * carried "rows" twice and the second one won. The browser gets the layout
   * from /api/dash/cfg anyway; this endpoint only has to say WHEN it changed,
   * which is what gen is for. */
  j  = "{\"gen\":";   j += s_dashGen;
  j += ",\"poll\":";  j += g_dash.pollMs;
  /* The cap comes from the firmware, so the browser cannot offer a slot the
   * logger has no room to store. */
  j += ",\"max\":";   j += (uint32_t)DASH_MAX_CELLS;

  /* One entry per cell slot. An empty slot, an unresolved signal and a signal
   * that has simply not arrived yet are three different things and the page
   * draws them differently, so they are three different values here:
   *   null   nothing configured in this slot
   *   ""     configured, but the frame map has no such signal
   *   "..."  the value, with `f` saying whether it is still fresh */
  j += ",\"v\":[";
  for (uint8_t i = 0; i < cells; i++) {
    if (i) j += ',';
    const DashCell &c = g_dash.cell[i];
    if (!dashCellUsed(c)) { j += "null";  continue; }   /* empty slot        */
    if (c.sig < 0)        { j += "false"; continue; }   /* no such signal     */
    const LiveSignals &lv = g_live[c.bus < CAN_BUSES ? c.bus : 0];
    if (c.sig >= (int16_t)lv.cap || !lv.seen[c.sig]) {
      j += "\"\"";  continue;                              /* not arrived yet */
    }
    j += '"';
    jsonStr(j, lv.text[c.sig]);
    j += '"';
  }
  j += ']';

  /* Freshness, so a cell whose message stopped arriving fades instead of
   * showing a value that is minutes old as though it were current. */
  j += ",\"f\":[";
  for (uint8_t i = 0; i < cells; i++) {
    if (i) j += ',';
    const DashCell &c = g_dash.cell[i];
    const LiveSignals &lv = g_live[c.bus < CAN_BUSES ? c.bus : 0];
    const bool fresh = dashCellUsed(c) && c.sig >= 0 &&
                       c.sig < (int16_t)lv.cap && lv.seen[c.sig] &&
                       (now - lv.lastMs[c.sig]) < DASH_STALE_MS;
    j += fresh ? '1' : '0';
  }
  j += ']';

  /* Which bus each cell reads from, so the page can badge it without having to
   * fetch the whole layout again on every poll. */
  j += ",\"cb\":[";
  for (uint8_t i = 0; i < cells; i++) {
    if (i) j += ',';
    const DashCell &c = g_dash.cell[i];
    j += (uint32_t)((dashCellUsed(c) ? c.bus : 0) + 1);
  }
  j += ']';

  /* The whole of the logger's state EXCEPT the two big arrays.
   *
   * This is what lets the dashboard be one request. The expensive parts of
   * /api/status are the per-signal and per-identifier tables - they are loops
   * over the frame map that build kilobytes of JSON - and the dashboard needs
   * neither: it has its own values above. Everything else is a handful of
   * counters, so carrying them here costs almost nothing and saves a second
   * poll running alongside the first. */
  j += ",\"rec\":";   j += g_rec.recording ? 1 : 0;
  j += ",\"sd\":";    j += g_rec.sdOk ? 1 : 0;
  j += ",\"sdErr\":"; j += g_rec.sdError ? 1 : 0;
  j += ",\"sdType\":\""; j += g_rec.sdType; j += '"';
  j += ",\"sdMB\":";  j += (uint32_t)g_rec.sdSizeMB;

  uint32_t dashLost = g_rec.queueDropped;
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    dashLost += g_rec.bus[b].canOvfFramesMin;
  }
  j += ",\"lost\":";  j += dashLost;
  j += ",\"qDrop\":"; j += g_rec.queueDropped;
  j += ",\"drain\":"; j += g_rec.drainMaxUs;

  /* The per-bus half, small enough to carry on the dashboard poll: the four
   * numbers a header strip shows for each controller, and nothing else. The
   * full tables stay on /api/status. */
  j += ",\"can\":[";
  for (uint8_t b = 0; b < CAN_BUSES; b++) {
    const BusHealth &h = g_rec.bus[b];
    if (b) j += ',';
    j += "{\"b\":";         j += (uint32_t)(b + 1);
    j += ",\"on\":";        j += h.present ? 1 : 0;
    j += ",\"en\":";        j += h.enabled ? 1 : 0;
    j += ",\"alive\":";     j += h.canOk ? 1 : 0;
    j += ",\"fps\":";       j += h.frameRate;
    j += ",\"irq\":";       j += h.irqRate;
    j += ",\"load\":";      j += h.busLoadPct;
    j += ",\"ovf\":";       j += h.canOvfFramesMin;
    j += ",\"ovfEv\":";     j += h.canOvfEvents;
    j += ",\"intStuck\":";  j += h.intStuck ? 1 : 0;
    j += ",\"intLevel\":";  j += (uint32_t)h.intLevel;
    j += ",\"dbc\":";       j += h.dbcLoaded ? 1 : 0;
    j += ",\"send\":";      j += h.listenOnly ? 0 : 1;
    j += '}';
  }
  j += ']';
  j += ",\"risk\":";  j += (uint32_t)SD_SYNC_INTERVAL_MS;
  j += ",\"file\":\""; j += (const char *)(g_rec.csvName[0] ? g_rec.csvName + 1 : "-");
  j += '"';
  j += ",\"elapsed\":"; j += recorderElapsedMs() / 1000UL;
  j += ",\"rows\":";  j += (uint32_t)g_rec.rows;
  j += ",\"kb\":";    j += (uint32_t)(g_rec.bytes / 1024ULL);
  j += ",\"pf\":";    j += g_rec.powerFail ? 1 : 0;
  j += ",\"up\":";    j += millis();
  {
    const MemStat mm = memStat();
    j += ",\"heap\":";    j += mm.freeNow;
    j += ",\"heapMax\":"; j += mm.largest;
    j += ",\"heapLow\":"; j += mm.lowBlock;
  }
  j += ",\"web\":";     j += (uint32_t)g_rec.loopRate;
  j += ",\"ap\":";    j += netIsAp() ? 1 : 0;
  j += ",\"ip\":\"";  j += netIp(); j += '"';
  j += ",\"fw\":\"";  j += FIRMWARE_NAME " v" FIRMWARE_VERSION; j += '"';

  /* ---- transmit ---- */
  j += ",\"arm\":";     j += txArmed() ? 1 : 0;
  j += ",\"armLeft\":"; j += (uint32_t)(txArmRemainingMs() / 1000UL);
  j += ",\"txOk\":";    j += g_tx.sent;
  j += ",\"txBad\":";   j += g_tx.failed;
  j += ",\"cyc\":";     j += g_tx.cyclicOn;
  /* Whether Send is possible AT ALL. Which bus a given setpoint goes out on,
   * and whether that one is listen-only, is carried per bus above. */
  j += ",\"canTx\":";   j += (CAN1_LISTEN_ONLY && CAN2_LISTEN_ONLY) ? 0 : 1;

  /* The last few outcomes, newest last. The browser matches them by ticket;
   * sending several means a burst of sends is not lost between two polls. */
  j += ",\"n\":"; j += (uint32_t)g_tx.ringCount;
  j += ",\"tx\":[";
  const uint8_t have = (g_tx.ringCount < TX_RESULT_RING)
                     ? g_tx.ringCount : (uint8_t)TX_RESULT_RING;
  const uint8_t show = have < 4 ? have : 4;
  for (uint8_t k = 0; k < show; k++) {
    const uint8_t slot =
        (uint8_t)((g_tx.ringCount - show + k) % TX_RESULT_RING);
    const TxOutcome &o = g_tx.ring[slot];
    if (k) j += ',';
    char id[16];
    snprintf(id, sizeof(id), o.ext ? "0x%08lX" : "0x%03lX", (unsigned long)o.id);
    j += "{\"t\":";    j += o.ticket;
    j += ",\"s\":";    j += o.status;
    j += ",\"cmd\":";  j += o.cmd;
    j += ",\"bus\":";  j += (uint32_t)(o.bus + 1);
    j += ",\"id\":\""; j += id; j += '"';
    j += ",\"c\":";    j += o.clamped ? 1 : 0;
    j += ",\"tec\":";  j += o.tecDelta;
    j += ",\"m\":\"";  jsonStr(j, txStatusText(o.status)); j += '"';
    j += '}';
  }
  j += "]}";

  s_srv->sendHeader("Cache-Control", "no-store");
  sendJson(j);
}

/* The layout as text - byte for byte what is on the card. Doubles as Export. */
static void handleDashCfgGet() {
  char *buf = (char *)malloc(DASH_CFG_MAX);
  if (!buf) { s_srv->send(500, "text/plain", "out of memory"); return; }

  const size_t n = dashSerialize(g_dash, buf, DASH_CFG_MAX);
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->setContentLength(n);
  s_srv->send(200, "text/plain", "");
  s_srv->sendContent(buf);
  free(buf);
}

/* And the same text back the other way, which is both Save and Import. Going
 * through the identical parser the card uses means the browser cannot produce
 * a layout the file format cannot express. */
static void handleDashCfgPost() {
  const String body = s_srv->arg("plain");
  if (body.length() == 0) {
    s_srv->send(400, "application/json", "{\"ok\":0,\"err\":\"empty\"}");
    return;
  }
  if (body.length() >= DASH_CFG_MAX) {
    s_srv->send(413, "application/json",
                "{\"ok\":0,\"err\":\"the layout is too large\"}");
    return;
  }

  dashReset(g_dash);
  const uint16_t errors  = dashParse(g_dash, body.c_str(), body.length());
  const uint16_t missing = dashResolve(g_dash, g_dbc);

  /* The card first, through the task that owns it - that copy is the one the
   * next boot reads and the one a power cut has to survive. Flash catches up
   * when no recording is running; see dashstore.h. */
  recorderRequestSaveDash();
  const bool saved = dashStoreSave();
  s_dashGen++;

  LOG_LIVE(LVL_INFO, "dashboard layout saved: %ux%u grid, %u bytes",
           (unsigned)g_dash.cols, (unsigned)g_dash.rows,
           (unsigned)body.length());

  String j;
  j.reserve(160);
  j  = "{\"ok\":";      j += saved ? 1 : 0;
  j += ",\"pending\":"; j += dashStorePending() ? 1 : 0;
  j += ",\"errors\":";  j += errors;
  j += ",\"missing\":"; j += missing;
  j += ",\"gen\":";     j += s_dashGen;
  j += '}';
  sendJson(j);
}

/* Uploading a frame map from the browser.
 *
 * Streamed to the card a chunk at a time rather than buffered: a real machine's
 * .dbc runs to ninety kilobytes, which is a third of the free heap on this chip
 * and more than the frame map built from it. The upload lands on a temporary
 * name and is renamed over DBC_PATH only when the whole file has arrived, so a
 * Wi-Fi dropout costs the upload rather than the map already in use.
 *
 * Refused outright while a recording is running. Every CSV opens with a header
 * describing the exact map its rows were decoded through; swapping the map
 * underneath a file in progress would make that header a lie for every row
 * after the swap, and the decoder is being read by the writer task at the
 * time. Stop, load, start. */
/* Which bus a request is about. Absent means CAN1, so every URL the single-bus
 * page used still resolves to the bus it used to mean. Out-of-range is clamped
 * rather than refused: the alternative is a dialog that fails with no signals
 * and no explanation. */
static uint8_t argBus() {
  if (!s_srv->hasArg("bus")) return 0;
  const long v = strtol(s_srv->arg("bus").c_str(), nullptr, 10);
  if (v < 1 || v > (long)CAN_BUSES) return 0;
  return (uint8_t)(v - 1);
}

static File s_dbcUp;
static bool s_dbcUpOk   = false;
static uint32_t s_dbcUpBytes = 0;
static uint8_t  s_dbcUpBus   = 0;    /* captured at UPLOAD_FILE_START */

static const char *const kDbcPath[CAN_BUSES]    = { DBC_PATH,     DBC2_PATH };
static const char *const kDbcTmpPath[CAN_BUSES] = { DBC_TMP_PATH, DBC2_TMP_PATH };

/* GET /api/bundle - the whole setup as one file.
 *
 * Export used to hand back /dash.cfg alone, which is half a setup: the layout
 * names signals as "Message.Signal", so a layout without the maps it was built
 * against is a page full of "unknown". This streams the maps and the layout
 * together in the format tools/make_bundle.py writes and bundle.cpp unpacks,
 * so what comes out of a logger can be copied straight onto another one.
 *
 * Streamed from the card in the same slices the dashboard page uses, and for
 * the same reason: the maps can be 60 KB and this board does not have 60 KB to
 * assemble them in. The declared name_max is this firmware's own - these files
 * are what it is actually running. */
static void handleBundle() {
  static const char *const kNames[] = { "frames.dbc", "frames2.dbc",
                                        "dash.cfg" };
  const char *paths[3] = { DBC_PATH, DBC2_PATH, DASH_PATH };

  char head[64];
  snprintf(head, sizeof(head), "#DCLB1 name_max=%u\n", (unsigned)DBC_NAME_MAX);

  /* Content-Length is worked out first so this is a plain response rather than
   * a chunked one - see the note on handleRoot for what chunked encoding cost
   * here. */
  size_t total = strlen(head) + 5;          /* header + "#END\n" */
  size_t sizes[3] = { 0, 0, 0 };
  bool   have[3]  = { false, false, false };
  for (uint8_t i = 0; i < 3; i++) {
    File f = SD.open(paths[i], FILE_READ);
    if (!f) continue;
    sizes[i] = (size_t)f.size();
    f.close();
    if (!sizes[i]) continue;
    have[i] = true;
    char hdr[80];
    total += snprintf(hdr, sizeof(hdr), "#FILE %s %u\n",
                      kNames[i], (unsigned)sizes[i]);
    total += sizes[i] + 1;                  /* payload + its newline */
  }

  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->sendHeader("Content-Disposition",
                    "attachment; filename=\"logger.bundle\"");
  s_srv->setContentLength(total);
  s_srv->send(200, "application/octet-stream", "");
  s_srv->sendContent(head);

  WiFiClient cl = s_srv->client();
  uint8_t buf[512];
  for (uint8_t i = 0; i < 3; i++) {
    if (!have[i]) continue;
    char hdr[80];
    snprintf(hdr, sizeof(hdr), "#FILE %s %u\n", kNames[i],
             (unsigned)sizes[i]);
    s_srv->sendContent(hdr);
    File f = SD.open(paths[i], FILE_READ);
    if (!f) continue;
    size_t left = sizes[i];
    while (left && cl.connected()) {
      const size_t want = (left < sizeof(buf)) ? left : sizeof(buf);
      const int got = f.read(buf, want);
      if (got <= 0) break;
      s_srv->sendContent((const char *)buf, (size_t)got);
      left -= (size_t)got;
      delay(0);
    }
    f.close();
    s_srv->sendContent("\n");
  }
  s_srv->sendContent("#END\n");
  LOG_FILE(LVL_INFO, "served the setup bundle, %u bytes", (unsigned)total);
}

static void handleDbcUpload() {
  HTTPUpload &up = s_srv->upload();

  if (up.status == UPLOAD_FILE_START) {
    s_dbcUpOk    = false;
    s_dbcUpBytes = 0;
    /* Read once, here, and remembered for the rest of the upload: the argument
     * belongs to the request, and the WRITE and END callbacks must not depend
     * on it still being parseable. */
    s_dbcUpBus   = argBus();
    if (g_rec.recording || !g_rec.sdOk) return;
    SD.remove(kDbcTmpPath[s_dbcUpBus]);
    s_dbcUp = SD.open(kDbcTmpPath[s_dbcUpBus], FILE_WRITE);
    s_dbcUpOk = (bool)s_dbcUp;
    return;
  }

  if (up.status == UPLOAD_FILE_WRITE) {
    if (!s_dbcUpOk) return;
    if (s_dbcUp.write(up.buf, up.currentSize) != up.currentSize) {
      s_dbcUpOk = false;                 /* card full, or gone */
      s_dbcUp.close();
      return;
    }
    s_dbcUpBytes += up.currentSize;
    return;
  }

  if (up.status == UPLOAD_FILE_END) {
    if (s_dbcUpOk) s_dbcUp.close();
    return;
  }

  /* ABORTED */
  if (s_dbcUp) s_dbcUp.close();
  s_dbcUpOk = false;
  SD.remove(kDbcTmpPath[s_dbcUpBus]);
}

static void handleDbcDone() {
  String j;
  j.reserve(200);

  const char *err = nullptr;
  if (g_rec.recording)      err = "stop the recording first - the CSV header "
                                  "describes the map its rows were decoded through";
  else if (!g_rec.sdOk)     err = "no SD card";
  else if (!s_dbcUpOk)      err = "the upload did not finish";
  else if (!s_dbcUpBytes)   err = "the file was empty";

  const uint8_t   bus  = s_dbcUpBus;
  const char *const dst = kDbcPath[bus];
  const char *const tmp = kDbcTmpPath[bus];

  if (err) {
    SD.remove(tmp);
    j  = "{\"ok\":0,\"err\":\""; jsonStr(j, err); j += "\"}";
    s_srv->send(409, "application/json", j);
    return;
  }

  SD.remove(dst);
  if (!SD.rename(tmp, dst)) {
    SD.remove(tmp);
    s_srv->send(500, "application/json",
                "{\"ok\":0,\"err\":\"could not put the file in place\"}");
    return;
  }

  LOG_LIVE(LVL_INFO, "CAN%u frame map uploaded: %lu bytes to %s",
           (unsigned)(bus + 1), (unsigned long)s_dbcUpBytes, dst);

  recorderLoadDbc();

  /* A different frame map means the old layout is about a different bus. What
   * cannot be found in the new one goes, rather than lingering as a screen of
   * unknowns - and the result is saved, so the card agrees with the page. */
  const uint16_t dropped = dashDropUnresolved(g_dash, g_dbc);
  const uint16_t missing = dashResolve(g_dash, g_dbc);
  if (dropped) {
    recorderRequestSaveDash();
    dashStoreSave();
    /* Any OTHER browser still holding the old layout re-reads on a new gen.
     * Without this it would carry on showing cells this map cannot decode,
     * and would write them straight back the next time it saved. */
    s_dashGen++;
    LOG_LIVE(LVL_INFO, "%u item(s) dropped - cells, setpoints and the logger's "
                       "role that the new frame map does not describe",
             (unsigned)dropped);
  }

  const DbcDb &db = g_dbc[bus];
  j  = "{\"ok\":";        j += db.loaded ? 1 : 0;
  j += ",\"bus\":";       j += (uint32_t)(bus + 1);
  j += ",\"bytes\":";     j += (uint32_t)s_dbcUpBytes;
  j += ",\"messages\":";  j += (uint32_t)db.msgCount;
  j += ",\"signals\":";   j += (uint32_t)db.sigCount;
  j += ",\"nodes\":";     j += (uint32_t)db.nodeCount;
  j += ",\"errors\":";    j += (uint32_t)db.lineErrors;
  j += ",\"inexact\":";   j += db.inexact ? 1 : 0;
  j += ",\"missing\":";   j += (uint32_t)missing;
  j += ",\"dropped\":";   j += (uint32_t)dropped;
  j += ",\"clipped\":";   j += (uint32_t)db.nameClipped;
  j += '}';
  sendJson(j);
}

/* Everything the editor needs to offer a signal: its name, unit, the range the
 * DBC annotates it with, what its bits can actually hold, and any value labels.
 * Streamed a message at a time - a full frame map is several times larger than
 * anything else this firmware builds in RAM, and building it as one String
 * would be the largest allocation in the program for the sake of a list that
 * is fetched when somebody opens a dialog. */
/* ---------------------------------------------------------------------------
 *  /api/websurvival - will this page keep answering while the logger records?
 *
 *  The design tools serve this too, and NOT with the same thing. They predict,
 *  from the .dbc files, for all three DBC_NAME_MAX settings, and can therefore
 *  offer to change one. This board can only report what it actually came up
 *  with: the map is loaded, the tables are allocated, and it cannot rebuild
 *  itself to try a different name length.
 *
 *  So can_choose and can_trim are 0 here, and the page renders a verdict with
 *  no controls under it. That asymmetry is the point rather than a limitation:
 *  the choice belongs at design time, and a logger that offered it would be
 *  offering something it cannot do.
 *
 *  Small, fixed-size and built on the stack. It is one short object and must
 *  not become another reason to reserve a kilobyte during a recording - see
 *  the note on handleStatus for what that cost the last time.
 * -------------------------------------------------------------------------*/
static void handleWebSurvival() {
  const uint32_t block = memReadyBlock();
  const bool ok = block >= MEM_WEB_SERVES;

  /* Before setup finished there is no answer yet, and saying "not guaranteed"
   * would be a claim rather than an absence. */
  if (!block) {
    s_srv->sendHeader("Cache-Control", "no-store");
    s_srv->send(200, "application/json",
                "{\"available\":0,\"why\":\"still starting up\"}");
    return;
  }

  const char *why =
      ok ? "the largest free block at start-up cleared the mark every "
           "measured run served every request above."
         : (block > MEM_WEB_DEAD
            ? "this is in the band where the same map served anywhere from "
              "3% to 100% of its requests on repeated runs. The recording is "
              "unaffected and will not lose a frame."
            : "this is at or below the mark where no measured run served even "
              "10% of its requests. Expect this page to stop answering within "
              "the first minute. The recording is unaffected and will not "
              "lose a frame.");

  char j[512];
  snprintf(j, sizeof(j),
           "{\"available\":1,\"guaranteed\":%d,\"block\":%lu,"
           "\"name_max\":%u,\"serves_at\":%lu,\"dead_at\":%lu,"
           "\"can_choose\":0,\"can_trim\":0,\"remedies\":[],"
           "\"options\":[{\"name_max\":%u,\"block\":%lu,"
           "\"guaranteed\":%d,\"chosen\":1}],\"why\":\"%s\"}",
           ok ? 1 : 0, (unsigned long)block, (unsigned)DBC_NAME_MAX,
           (unsigned long)MEM_WEB_SERVES, (unsigned long)MEM_WEB_DEAD,
           (unsigned)DBC_NAME_MAX, (unsigned long)block, ok ? 1 : 0, why);

  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send(200, "application/json", j);
}

static void handleSignals() {
  const uint8_t bus = argBus();
  const DbcDb  &db  = g_dbc[bus];

  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_srv->send(200, "application/json", "");

  /* The BUFFER here is already right and must stay as it is: the response is
   * chunked and flushed every couple of kilobytes (see the sendContent below),
   * so `j` is a working buffer, not the whole payload. Do not "fix" it the way
   * handleDash and handleStatus were fixed - and do not add a 503 here,
   * because the 200 header has already gone out.
   *
   * What it was missing was a YIELD. Each sendContent() on a chunked response
   * is three socket writes (size, body, CRLF), and this loop fired them back
   * to back with nothing in between. On a 33-signal map that is 4 flushes and
   * survives; on a 98-signal map it is 13, the TCP window fills part way
   * through, and the write comes back EAGAIN - so the response is truncated,
   * r.json() throws in the browser, and the page's init chain never reaches
   * startPolls(). The dashboard then sits on "connecting..." forever with
   * every cell showing "--", which looks like the poll failing when in fact
   * the poll never started.
   *
   * Deleting /frames2.dbc from the card "fixed" it for exactly this reason:
   * it took the bus-2 response from 14363 bytes back down to 38. */
  String j;
  j.reserve(1400);
  j = "{\"bus\":";
  j += (uint32_t)(bus + 1);
  j += ",\"loaded\":";
  j += db.loaded ? 1 : 0;

  /* The BU_ node list, and each message's transmitter further down. A .dbc
   * states who sends what but never which of those nodes is this logger, so
   * the page offers the list and the answer is stored as the role. */
  j += ",\"nodes\":[";
  for (uint8_t i = 0; i < db.nodeCount; i++) {
    if (i) j += ',';
    j += '"'; jsonStr(j, db.node[i]); j += '"';
  }
  j += "],\"m\":[";

  char num[40];
  for (uint16_t mi = 0; mi < db.msgCount; mi++) {
    const DbcMessage &m = db.msg[mi];
    if (mi) j += ',';

    snprintf(num, sizeof(num), m.ext ? "0x%08lX" : "0x%03lX", (unsigned long)m.id);
    j += "{\"n\":\""; jsonStr(j, m.name);
    j += "\",\"id\":\""; j += num;
    j += "\",\"tx\":\""; jsonStr(j, dbcTxNode(db, m));
    j += "\",\"mux\":";
    j += (m.muxSignal >= 0) ? 1 : 0;
    j += ",\"s\":[";

    for (uint16_t k = 0; k < m.signalCount; k++) {
      const uint16_t si = (uint16_t)(m.firstSignal + k);
      if (si >= db.sigCount) break;
      const DbcSignal &sg = db.sig[si];
      if (k) j += ',';

      j += "{\"i\":";   j += si;
      j += ",\"n\":\""; jsonStr(j, sg.name);
      j += "\",\"u\":\""; jsonStr(j, sg.unit);
      j += "\",\"b\":";  j += sg.bits;

      /* Both ranges. The annotation is what the bus designer meant and makes
       * the better default; the bit limits are what is actually possible and
       * are what a setpoint has to be clamped to. */
      j += ",\"r\":";   j += sg.hasRange ? 1 : 0;
      snprintf(num, sizeof(num), ",\"lo\":%.6g,\"hi\":%.6g",
               (double)sg.phyMin, (double)sg.phyMax);
      j += num;

      double blo = 0, bhi = 0;
      dbcSignalLimits(sg, &blo, &bhi);
      snprintf(num, sizeof(num), ",\"blo\":%.6g,\"bhi\":%.6g", blo, bhi);
      j += num;

      /* Decimal places the factor actually justifies: showing 12.4000 km/h
       * from a factor of 0.1 is three digits of invention. */
      j += ",\"d\":"; j += sg.exact ? sg.dec : 3;

      /* -1 plain, -2 the multiplexor, >= 0 the mux code that selects it. A
       * signal only reachable under one mux code cannot be written without
       * writing that code as well, which is what makes this worth sending. */
      j += ",\"mx\":"; j += sg.muxValue;

      /* Value labels, which are what makes a signal worth drawing as a state
       * rather than as a number. */
      j += ",\"v\":[";
      for (uint8_t vi = 0; vi < sg.valCount; vi++) {
        const uint16_t vk = (uint16_t)(sg.valFirst + vi);
        if (sg.valFirst < 0 || vk >= db.valCount) break;
        if (vi) j += ',';
        j += '"';
        jsonStr(j, db.val[vk].label);
        j += '"';
      }
      j += "]}";
    }
    j += "]}";

    /* Flush at message boundaries so the buffer never grows with the map, and
     * yield so the TCP task can drain what was just queued. 2 KB rather than
     * 1 KB halves the chunk count for the same buffer ceiling. */
    if (j.length() > 2048) { s_srv->sendContent(j); j = ""; delay(0); }
  }

  j += "]}";
  s_srv->sendContent(j);
  delay(0);
  s_srv->sendContent("");
}

/* ==========================================================================
 *  Transmit
 * ======================================================================== */
static void txReply(uint32_t ticket) {
  String j;
  j.reserve(64);
  j  = "{\"ticket\":"; j += ticket;
  j += ",\"n\":";      j += (uint32_t)g_tx.ringCount;
  j += '}';
  sendJson(j);
}

static void handleTxArm() {
  const bool on = s_srv->hasArg("on") && s_srv->arg("on") == "1";
  txArm(on, "the web dashboard");

  String j;
  j.reserve(64);
  j  = "{\"arm\":";     j += txArmed() ? 1 : 0;
  j += ",\"armLeft\":"; j += (uint32_t)(txArmRemainingMs() / 1000UL);
  j += '}';
  sendJson(j);
}

static uint8_t hexPair(const char *p) {
  uint8_t v = 0;
  for (uint8_t i = 0; i < 2; i++) {
    const char c = p[i];
    v = (uint8_t)(v << 4);
    if      (c >= '0' && c <= '9') v |= (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint8_t)(c - 'A' + 10);
  }
  return v;
}

static void handleTxSend() {
  uint32_t ticket = 0;

  if (s_srv->hasArg("id")) {
    /* A one-off frame, typed in rather than saved. */
    const uint32_t id  = (uint32_t)strtoul(s_srv->arg("id").c_str(), nullptr, 0);
    const bool     ext = s_srv->hasArg("ext") ? (s_srv->arg("ext") == "1")
                                              : (id > 0x7FF);
    const String   hex = s_srv->arg("data");
    uint8_t data[8] = {0};
    uint8_t len = 0;
    for (size_t i = 0; i + 1 < (size_t)hex.length() && len < 8; i += 2) {
      data[len++] = hexPair(hex.c_str() + i);
    }
    /* A typed-in frame names its own bus - there is no setpoint to take it
     * from - and defaults to CAN1 when the page does not say. */
    ticket = txSendRaw(argBus(), id, ext, data, len);
  } else if (s_srv->hasArg("cmds")) {
    /* A group: several values that only mean anything in the same frame, sent
     * as "cmds=0,2,3&values=32,1,1380". Every member but the last is queued
     * holding, so one frame leaves with all of them in it. Parsed in place -
     * a handful of comma-separated numbers does not justify a tokeniser. */
    const String  ids = s_srv->arg("cmds");
    const String  vs  = s_srv->arg("values");
    const char   *ip  = ids.c_str();
    const char   *vp  = vs.c_str();

    uint8_t idx[TX_MAX_COMMANDS];
    float   val[TX_MAX_COMMANDS];
    uint8_t n = 0;
    while (*ip && n < TX_MAX_COMMANDS) {
      idx[n] = (uint8_t)strtoul(ip, nullptr, 10);
      val[n] = (float)atof(vp);
      n++;
      const char *ic = strchr(ip, ',');
      const char *vc = strchr(vp, ',');
      if (!ic || !vc) break;
      ip = ic + 1;
      vp = vc + 1;
    }
    for (uint8_t i = 0; i < n; i++) {
      ticket = txSendPart(idx[i], val[i], (uint8_t)(i + 1) < n);
    }
  } else {
    const uint8_t cmd = (uint8_t)strtoul(s_srv->arg("cmd").c_str(), nullptr, 10);
    const float   val = (float)atof(s_srv->arg("value").c_str());
    ticket = txSendCommand(cmd, val);
  }
  txReply(ticket);
}

static void handleTxCyclic() {
  const uint8_t cmd = (uint8_t)strtoul(s_srv->arg("cmd").c_str(), nullptr, 10);
  const bool    on  = s_srv->hasArg("on") && s_srv->arg("on") == "1";
  const float   val = (float)atof(s_srv->arg("value").c_str());
  txSetCyclic(cmd, on, val);

  String j;
  j.reserve(48);
  j  = "{\"cyc\":"; j += g_tx.cyclicOn;
  j += ",\"n\":";   j += (uint32_t)g_tx.ringCount;
  j += '}';
  sendJson(j);
}

void webBegin() {
  s_srv = new WebServer(g_net.httpPort);

  s_srv->on("/",            HTTP_GET,  handleRoot);
  s_srv->on("/api/status",  HTTP_GET,  handleStatus);
  s_srv->on("/api/log",     HTTP_GET,  handleLog);
  s_srv->on("/api/start",   HTTP_POST, handleStart);
  s_srv->on("/api/start",   HTTP_GET,  handleStart);   /* convenience */
  s_srv->on("/api/stop",    HTTP_POST, handleStop);
  s_srv->on("/api/stop",    HTTP_GET,  handleStop);
  s_srv->on("/api/reboot",  HTTP_POST, handleReboot);

  s_srv->on("/api/dash",      HTTP_GET,  handleDash);
  s_srv->on("/api/dash/cfg",  HTTP_GET,  handleDashCfgGet);
  s_srv->on("/api/dash/cfg",  HTTP_POST, handleDashCfgPost);
  s_srv->on("/api/signals",   HTTP_GET,  handleSignals);
  s_srv->on("/api/websurvival", HTTP_GET, handleWebSurvival);
  s_srv->on("/api/dbc",       HTTP_POST, handleDbcDone, handleDbcUpload);
  s_srv->on("/api/bundle",    HTTP_GET,  handleBundle);

  s_srv->on("/api/tx/arm",    HTTP_POST, handleTxArm);
  s_srv->on("/api/tx/send",   HTTP_POST, handleTxSend);
  s_srv->on("/api/tx/cyclic", HTTP_POST, handleTxCyclic);

  /* Every browser asks for this on every page load, and some ask again while
   * the tab is open. Without a handler it fell through to onNotFound, which
   * serves the WHOLE 168 KB dashboard - so the cost of an icon nobody wants was
   * a second full page render, streamed down the same single-client socket the
   * status poll is queued on. 204 ends it in one packet. */
  s_srv->on("/favicon.ico", HTTP_GET, []() { s_srv->send(204); });

  /* Anything else goes to the dashboard, including the captive-portal probes
   * phones fire when they join the hotspot - EXCEPT an unknown /api/ path,
   * which gets a small 404. Serving 168 KB of HTML in answer to a mistyped or
   * newer API call wedges the server for no possible benefit: nothing that
   * calls /api/ can use a page, and the caller is a script that will retry. */
  s_srv->onNotFound([]() {
    if (s_srv->uri().startsWith("/api/")) {
      s_srv->send(404, "application/json", "{\"err\":\"no such endpoint\"}");
      return;
    }
    handleRoot();
  });

  s_srv->begin();

  if (MDNS.begin(g_net.hostname.c_str())) {
    MDNS.addService("http", "tcp", g_net.httpPort);
    LOG_LIVE(LVL_INFO, "dashboard on http://%s  (or http://%s.local)",
             netIp().c_str(), g_net.hostname.c_str());
  } else {
    LOG_LIVE(LVL_INFO, "dashboard on http://%s", netIp().c_str());
  }
}

void webService() {
  if (!s_srv) return;
  s_srv->handleClient();
  /* Sampled HERE and nowhere cheaper: serving is what spends the heap,
   * so this is the only place that sees the trough. A boot-time reading
   * cannot - by the time anything is wrong the page has been served a
   * hundred times. */
  memSample();
}
