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

#include <WebServer.h>
#include <ESPmDNS.h>

static WebServer *s_srv = nullptr;

/* ==========================================================================
 *  Handlers
 * ======================================================================== */
static void handleRoot() {
  s_srv->sendHeader("Cache-Control", "no-store");

  /* Chunked, so the page streams from flash to the socket without ever being
   * assembled in RAM. The parts are only split for readability - the browser
   * sees one document. */
  s_srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_srv->send(200, "text/html", "");
  for (uint8_t i = 0; i < PAGE_PART_COUNT; i++) s_srv->sendContent_P(PAGE_PARTS[i]);
  s_srv->sendContent("");
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

static void handleStatus() {
  /* A FLOOR, not a total: the controller's overflow flags are sticky and say
   * only that it happened, never how often. Reported as such by the page. */
  const uint32_t lost = g_rec.queueDropped + g_rec.canOvfFramesMin;

  String j;
  j.reserve(2048);
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
  j += ",\"can\":";     j += g_rec.canOk ? 1 : 0;
  j += ",\"fps\":";     j += g_rec.frameRate;
  j += ",\"irq\":";     j += g_rec.irqRate;
  j += ",\"intStuck\":"; j += g_rec.intStuck ? 1 : 0;
  j += ",\"intLevel\":"; j += (uint32_t)g_rec.intLevel;
  j += ",\"load\":";    j += g_rec.busLoadPct;
  j += ",\"lost\":";    j += lost;
  j += ",\"ovfEv\":";   j += g_rec.canOvfEvents;
  j += ",\"qDrop\":";   j += g_rec.queueDropped;
  j += ",\"dbc\":";     j += g_rec.dbcLoaded ? 1 : 0;
  j += ",\"dbcMsg\":";  j += (uint32_t)g_rec.dbcMessages;
  j += ",\"dbcSig\":";  j += (uint32_t)g_rec.dbcSignals;

  /* ---- identifiers actually seen, with their most recent payload ---- */
  j += ",\"idMore\":"; j += g_bus.untracked ? 1 : 0;
  j += ",\"ids\":[";
  char id[16];
  for (uint8_t i = 0; i < g_bus.used; i++) {
    if (i) j += ',';
    snprintf(id, sizeof(id), g_bus.ext[i] ? "0x%08lX" : "0x%03lX",
             (unsigned long)g_bus.id[i]);
    j += "{\"id\":\""; j += id; j += '"';
    j += ",\"d\":\"";  j += g_bus.last[i]; j += '"';
    j += ",\"n\":";    j += (uint32_t)g_bus.count[i];
    j += ",\"r\":";    j += (uint32_t)g_bus.rate[i];
    j += ",\"k\":";    j += g_bus.known[i] ? 1 : 0;
    j += '}';
  }
  j += ']';

  /* ---- live decoded signals, straight out of the frame map ----------
   * Nothing here knows what any of these are. The names, units and order all
   * come from the DBC on the card, so the page shows a drive, a weather
   * station or a test rig without a line of firmware changing. */
  uint16_t shown = 0;
  j += ",\"sig\":[";
  for (uint16_t mi = 0; mi < g_dbc.msgCount && shown < WEB_MAX_SIGNALS; mi++) {
    const DbcMessage &m = g_dbc.msg[mi];
    for (uint16_t k = 0; k < m.signalCount && shown < WEB_MAX_SIGNALS; k++) {
      const uint16_t si = m.firstSignal + k;
      if (si >= g_live.cap || !g_live.seen[si]) continue;

      if (shown) j += ',';
      j += "{\"m\":\""; jsonStr(j, m.name);
      j += "\",\"s\":\""; jsonStr(j, g_dbc.sig[si].name);
      j += "\",\"v\":\""; jsonStr(j, g_live.text[si]);
      j += "\",\"u\":\""; jsonStr(j, g_dbc.sig[si].unit);
      j += "\"}";
      shown++;
    }
  }
  j += ']';
  j += ",\"sigMore\":"; j += (g_live.seenCount > shown) ? 1 : 0;

  j += ",\"ap\":";      j += netIsAp() ? 1 : 0;
  j += ",\"ip\":\"";    j += netIp(); j += '"';
  j += ",\"up\":";      j += millis();
  j += ",\"heap\":";    j += (uint32_t)ESP.getFreeHeap();
  j += ",\"fw\":\"";    j += FIRMWARE_NAME " v" FIRMWARE_VERSION; j += "\"}";

  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send(200, "application/json", j);
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
  s_srv->send(200, "application/json", j);
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
  String j;
  j.reserve(1024);

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
    if (c.sig >= (int16_t)g_live.cap || !g_live.seen[c.sig]) {
      j += "\"\"";  continue;                              /* not arrived yet */
    }
    j += '"';
    jsonStr(j, g_live.text[c.sig]);
    j += '"';
  }
  j += ']';

  /* Freshness, so a cell whose message stopped arriving fades instead of
   * showing a value that is minutes old as though it were current. */
  j += ",\"f\":[";
  for (uint8_t i = 0; i < cells; i++) {
    if (i) j += ',';
    const DashCell &c = g_dash.cell[i];
    const bool fresh = dashCellUsed(c) && c.sig >= 0 &&
                       c.sig < (int16_t)g_live.cap && g_live.seen[c.sig] &&
                       (now - g_live.lastMs[c.sig]) < DASH_STALE_MS;
    j += fresh ? '1' : '0';
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
  j += ",\"can\":";   j += g_rec.canOk ? 1 : 0;
  j += ",\"sd\":";    j += g_rec.sdOk ? 1 : 0;
  j += ",\"sdErr\":"; j += g_rec.sdError ? 1 : 0;
  j += ",\"sdType\":\""; j += g_rec.sdType; j += '"';
  j += ",\"sdMB\":";  j += (uint32_t)g_rec.sdSizeMB;
  j += ",\"lost\":";  j += (uint32_t)(g_rec.queueDropped + g_rec.canOvfFramesMin);
  j += ",\"ovfEv\":"; j += g_rec.canOvfEvents;
  j += ",\"qDrop\":"; j += g_rec.queueDropped;
  j += ",\"fps\":";   j += g_rec.frameRate;
  j += ",\"irq\":";   j += g_rec.irqRate;
  j += ",\"intStuck\":"; j += g_rec.intStuck ? 1 : 0;
  j += ",\"intLevel\":"; j += (uint32_t)g_rec.intLevel;
  j += ",\"load\":";  j += g_rec.busLoadPct;
  j += ",\"risk\":";  j += (uint32_t)SD_SYNC_INTERVAL_MS;
  j += ",\"file\":\""; j += (const char *)(g_rec.csvName[0] ? g_rec.csvName + 1 : "-");
  j += '"';
  j += ",\"elapsed\":"; j += recorderElapsedMs() / 1000UL;
  j += ",\"rows\":";  j += (uint32_t)g_rec.rows;
  j += ",\"kb\":";    j += (uint32_t)(g_rec.bytes / 1024ULL);
  j += ",\"pf\":";    j += g_rec.powerFail ? 1 : 0;
  j += ",\"dbc\":";   j += g_rec.dbcLoaded ? 1 : 0;
  j += ",\"up\":";    j += millis();
  j += ",\"heap\":";  j += (uint32_t)ESP.getFreeHeap();
  j += ",\"ap\":";    j += netIsAp() ? 1 : 0;
  j += ",\"ip\":\"";  j += netIp(); j += '"';
  j += ",\"fw\":\"";  j += FIRMWARE_NAME " v" FIRMWARE_VERSION; j += '"';

  /* ---- transmit ---- */
  j += ",\"arm\":";     j += txArmed() ? 1 : 0;
  j += ",\"armLeft\":"; j += (uint32_t)(txArmRemainingMs() / 1000UL);
  j += ",\"txOk\":";    j += g_tx.sent;
  j += ",\"txBad\":";   j += g_tx.failed;
  j += ",\"cyc\":";     j += g_tx.cyclicOn;
  j += ",\"canTx\":";   j += CAN_LISTEN_ONLY ? 0 : 1;

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
    j += ",\"id\":\""; j += id; j += '"';
    j += ",\"c\":";    j += o.clamped ? 1 : 0;
    j += ",\"tec\":";  j += o.tecDelta;
    j += ",\"m\":\"";  jsonStr(j, txStatusText(o.status)); j += '"';
    j += '}';
  }
  j += "]}";

  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send(200, "application/json", j);
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
  s_srv->send(200, "application/json", j);
}

/* Everything the editor needs to offer a signal: its name, unit, the range the
 * DBC annotates it with, what its bits can actually hold, and any value labels.
 * Streamed a message at a time - a full frame map is several times larger than
 * anything else this firmware builds in RAM, and building it as one String
 * would be the largest allocation in the program for the sake of a list that
 * is fetched when somebody opens a dialog. */
static void handleSignals() {
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_srv->send(200, "application/json", "");

  String j;
  j.reserve(1400);
  j = "{\"loaded\":";
  j += g_dbc.loaded ? 1 : 0;

  /* The BU_ node list, and further down each message's transmitter. This is
   * the only thing a DBC says about direction, and it is what lets the
   * customiser stop offering command frames as gauges. */
  j += ",\"nodes\":[";
  for (uint8_t i = 0; i < g_dbc.nodeCount; i++) {
    if (i) j += ',';
    j += '"'; jsonStr(j, g_dbc.node[i]); j += '"';
  }
  j += "],\"me\":\"";
  jsonStr(j, g_dash.node);
  j += "\",\"m\":[";

  char num[40];
  for (uint16_t mi = 0; mi < g_dbc.msgCount; mi++) {
    const DbcMessage &m = g_dbc.msg[mi];
    if (mi) j += ',';

    snprintf(num, sizeof(num), m.ext ? "0x%08lX" : "0x%03lX", (unsigned long)m.id);
    j += "{\"n\":\""; jsonStr(j, m.name);
    j += "\",\"id\":\""; j += num;
    j += "\",\"tx\":\""; jsonStr(j, dbcTxNode(g_dbc, m));
    j += "\",\"mux\":";
    j += (m.muxSignal >= 0) ? 1 : 0;
    j += ",\"s\":[";

    for (uint16_t k = 0; k < m.signalCount; k++) {
      const uint16_t si = (uint16_t)(m.firstSignal + k);
      if (si >= g_dbc.sigCount) break;
      const DbcSignal &sg = g_dbc.sig[si];
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
        if (sg.valFirst < 0 || vk >= g_dbc.valCount) break;
        if (vi) j += ',';
        j += '"';
        jsonStr(j, g_dbc.val[vk].label);
        j += '"';
      }
      j += "]}";
    }
    j += "]}";

    /* Flush at message boundaries so the buffer never grows with the map. */
    if (j.length() > 1024) { s_srv->sendContent(j); j = ""; }
  }

  j += "]}";
  s_srv->sendContent(j);
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
  s_srv->send(200, "application/json", j);
}

static void handleTxArm() {
  const bool on = s_srv->hasArg("on") && s_srv->arg("on") == "1";
  txArm(on, "the web dashboard");

  String j;
  j.reserve(64);
  j  = "{\"arm\":";     j += txArmed() ? 1 : 0;
  j += ",\"armLeft\":"; j += (uint32_t)(txArmRemainingMs() / 1000UL);
  j += '}';
  s_srv->send(200, "application/json", j);
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
    ticket = txSendRaw(id, ext, data, len);
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
  s_srv->send(200, "application/json", j);
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

  s_srv->on("/api/tx/arm",    HTTP_POST, handleTxArm);
  s_srv->on("/api/tx/send",   HTTP_POST, handleTxSend);
  s_srv->on("/api/tx/cyclic", HTTP_POST, handleTxCyclic);

  /* Anything else goes to the dashboard, including the captive-portal probes
   * phones fire when they join the hotspot. */
  s_srv->onNotFound(handleRoot);

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
  if (s_srv) s_srv->handleClient();
}
