#include "webui.h"
#include "config.h"
#include "recorder.h"
#include "decode.h"
#include "logger.h"
#include "netcfg.h"
#include "dbc.h"

#include <WebServer.h>
#include <ESPmDNS.h>

static WebServer *s_srv = nullptr;

/* ==========================================================================
 *  The page. One file, no external assets - it must load with no internet.
 * ======================================================================== */
static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>CAN Logger</title>
<style>
:root{
  --bg:#0e1116; --panel:#171c24; --line:#252c38; --txt:#e8edf5; --dim:#8b97a8;
  --ok:#22c55e; --warn:#f59e0b; --bad:#ef4444; --acc:#3b82f6;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);
  font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  padding:14px;padding-bottom:32px;-webkit-text-size-adjust:100%}
h1{font-size:19px;margin:0;letter-spacing:.2px}
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:14px}
#conn{margin-left:auto;font-size:13px;color:var(--dim)}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}
/* Five status tiles across on a desktop, wrapping to one on a phone. */
.grid5{grid-template-columns:repeat(auto-fit,minmax(215px,1fr))}
.ctl{margin-top:12px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
.meter{height:6px;border-radius:3px;background:#0d1219;margin-top:12px;overflow:hidden}
.meter span{display:block;height:100%;width:0;background:var(--ok);
  transition:width .4s ease,background .4s ease}
button.reboot{background:transparent;border:1px solid var(--line);color:var(--dim);
  font-weight:500}
button.reboot:hover{border-color:var(--bad);color:var(--bad)}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px}
.card h2{font-size:12px;letter-spacing:.14em;text-transform:uppercase;
  color:var(--dim);margin:0 0 10px;font-weight:600}
.state{display:flex;align-items:center;gap:12px}
.dot{width:18px;height:18px;border-radius:50%;flex:none;background:var(--dim);
  box-shadow:0 0 0 4px rgba(255,255,255,.05)}
.dot.ok{background:var(--ok);box-shadow:0 0 0 4px rgba(34,197,94,.18)}
.dot.warn{background:var(--warn);box-shadow:0 0 0 4px rgba(245,158,11,.18)}
.dot.bad{background:var(--bad);box-shadow:0 0 0 4px rgba(239,68,68,.18)}
.dot.rec{animation:pulse 1.2s ease-in-out infinite}
@keyframes pulse{50%{opacity:.35}}
.big{font-size:23px;font-weight:650;line-height:1.15}
.sub{font-size:13px;color:var(--dim);margin-top:5px}
button{font:inherit;font-weight:650;border:0;border-radius:11px;padding:15px 22px;
  color:#fff;cursor:pointer;width:100%;margin-top:12px;letter-spacing:.02em}
.start{background:var(--ok)} .stop{background:var(--bad)}
button:active{transform:translateY(1px)}
.scroll{overflow-x:auto}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
/* Headers and cells must share an alignment or the columns visibly disagree,
   which is what happened when th was right-aligned and td was not. Alignment
   is set per column instead, on both. */
th{font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--dim);
  text-align:left;font-weight:600;padding:6px 8px;border-bottom:1px solid var(--line)}
th.num,td.num{text-align:right;font-variant-numeric:tabular-nums}
/* Fixed layout so the columns do not jump about as values change width. */
table.sig{table-layout:fixed}
table.sig th.c1,table.sig td.c1{width:30%}
table.sig th.c2,table.sig td.c2{width:30%}
table.sig th.c3,table.sig td.c3{width:22%}
table.sig th.c4,table.sig td.c4{width:18%;padding-left:14px}
td.ell{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
td{padding:6px 8px;border-bottom:1px solid #1b212b;font-size:14px}
td.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
tr.un td{color:var(--dim)}
#term{background:#080b0f;border:1px solid var(--line);border-radius:12px;
  margin-top:12px;height:290px;overflow:auto;padding:11px 13px;
  font:12.5px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  color:#a8b6c8;white-space:pre-wrap;word-break:break-word}
#term div.W{color:var(--warn)} #term div.E{color:var(--bad)}
#term div.I{color:#cfe0f5}
.foot{color:var(--dim);font-size:12px;margin-top:12px;text-align:center}
</style></head><body>

<header>
  <h1>CAN Logger</h1>
  <span id="conn">connecting...</span>
</header>

<div class="grid grid5">
  <div class="card">
    <h2>SD Card</h2>
    <div class="state"><span class="dot" id="d_sd"></span>
      <div><div class="big" id="t_sd">--</div><div class="sub" id="s_sd"></div></div></div>
  </div>

  <div class="card">
    <h2>Bus</h2>
    <div class="state"><span class="dot" id="d_can"></span>
      <div><div class="big" id="t_can">--</div><div class="sub" id="s_can"></div></div></div>
  </div>

  <div class="card">
    <h2>Interrupt Path</h2>
    <div class="state"><span class="dot" id="d_irq"></span>
      <div><div class="big" id="t_irq">--</div><div class="sub" id="s_irq"></div></div></div>
  </div>

  <div class="card">
    <h2>Data Integrity</h2>
    <div class="state"><span class="dot" id="d_lost"></span>
      <div><div class="big" id="t_lost">--</div><div class="sub" id="s_lost"></div></div></div>
  </div>

  <div class="card">
    <h2>CAN Bus Load</h2>
    <div class="state"><span class="dot" id="d_load"></span>
      <div><div class="big" id="t_load">--</div><div class="sub" id="s_load"></div></div></div>
    <div class="meter"><span id="loadbar"></span></div>
  </div>
</div>

<div class="card" style="margin-top:12px">
  <h2>Live Signals</h2>
  <div class="scroll">
    <table class="sig"><thead><tr>
      <th class="c1">Message</th><th class="c2">Signal</th>
      <th class="c3 num">Value</th><th class="c4">Unit</th>
    </tr></thead><tbody id="sigs"></tbody></table>
  </div>
  <div class="sub" id="s_sigs">&nbsp;</div>
</div>

<div class="card" style="margin-top:12px">
  <h2>Identifiers on the wire</h2>
  <div class="scroll">
    <table><thead><tr>
      <th>ID</th><th>Last payload</th><th>Frames</th><th>Rate</th><th>Mapped</th>
    </tr></thead><tbody id="ids"></tbody></table>
  </div>
  <div class="sub" id="s_ids">&nbsp;</div>
</div>

<div class="card" style="margin-top:12px">
  <h2>Live Log</h2>
  <div id="term"></div>
</div>

<div class="grid ctl">
  <div class="card">
    <h2>Recording</h2>
    <div class="state"><span class="dot" id="d_rec"></span>
      <div><div class="big" id="t_rec">--</div><div class="sub" id="s_rec"></div></div></div>
    <button id="btn" class="start" onclick="toggle()">START</button>
  </div>

  <div class="card">
    <h2>Logger</h2>
    <div class="state"><span class="dot ok"></span>
      <div><div class="big">RUNNING</div><div class="sub" id="s_up">&nbsp;</div></div></div>
    <button class="reboot" onclick="reboot()">RESTART</button>
  </div>
</div>

<div class="foot" id="foot">&nbsp;</div>

<script>
var seq = 0, rec = false, fails = 0;

function q(id){return document.getElementById(id)}
function setDot(id,cls){q(id).className = 'dot ' + cls}
function hms(s){
  var h=Math.floor(s/3600), m=Math.floor(s/60)%60, x=s%60;
  return (h<10?'0':'')+h+':'+(m<10?'0':'')+m+':'+(x<10?'0':'')+x;
}

function toggle(){
  fetch(rec ? '/api/stop' : '/api/start', {method:'POST'});
  q('btn').textContent = '...';
}

function reboot(){
  if(!confirm('Restart the logger?\n\nAny running recording is closed and '+
              'saved first. The next recording goes to a new file.')) return;
  fetch('/api/reboot', {method:'POST'});
  q('conn').textContent = 'restarting...';
  setTimeout(function(){ location.reload(); }, 8000);
}

/* Everything below renders whatever the payload contains. No signal, message
   or identifier is named anywhere in this page - the frame map on the card
   decides what appears, and an empty map simply yields raw frames. */
function esc(s){
  return String(s).replace(/[&<>]/g, function(c){
    return c === '&' ? '&amp;' : (c === '<' ? '&lt;' : '&gt;');
  });
}

function paintIds(list){
  var body = q('ids'), html = '', i;
  for(i=0;i<list.length;i++){
    var e = list[i];
    html += '<tr class="'+(e.k?'':'un')+'"><td class="mono">'+esc(e.id)+
            '</td><td class="mono">'+esc(e.d)+'</td><td>'+
            e.n.toLocaleString()+'</td><td>'+e.r+'/s</td><td>'+
            (e.k?'yes':'raw')+'</td></tr>';
  }
  body.innerHTML = html || '<tr><td colspan="5">nothing received yet</td></tr>';
}

function paintSigs(list, mapped){
  var body = q('sigs'), html = '', i;
  for(i=0;i<list.length;i++){
    var e = list[i];
    html += '<tr><td class="c1 ell" title="'+esc(e.m)+'">'+esc(e.m)+
            '</td><td class="c2 ell" title="'+esc(e.s)+'">'+esc(e.s)+
            '</td><td class="c3 num mono">'+esc(e.v)+
            '</td><td class="c4">'+esc(e.u)+'</td></tr>';
  }
  if(!html){
    html = '<tr><td colspan="4">' + (mapped
      ? 'no mapped frame has arrived yet'
      : 'no frame map on the card - see the raw frames below') + '</td></tr>';
  }
  body.innerHTML = html;
}

function paint(d){
  q('conn').textContent = (d.ap ? 'Hotspot ' : 'Wi-Fi ') + d.ip;

  /* --- SD --- */
  if(!d.sd){ setDot('d_sd','bad'); q('t_sd').textContent='NOT FOUND';
             q('s_sd').textContent='Insert a FAT32 card and restart'; }
  else if(d.sdErr){ setDot('d_sd','bad'); q('t_sd').textContent='WRITE ERROR';
             q('s_sd').textContent='Card may be full or was removed'; }
  else { setDot('d_sd','ok'); q('t_sd').textContent='READY';
             q('s_sd').textContent=d.sdType+', '+(d.sdMB/1024).toFixed(1)+' GB'; }

  /* --- interrupt path ---
     The receive path only keeps up if the controller's INT line actually
     fires. When it does not, the 20 ms fallback poll caps throughput at about
     100 frames/s no matter what the bus is doing, so it gets its own tile
     rather than being buried in the log. */
  if(!d.can){
    setDot('d_irq','warn'); q('t_irq').textContent='IDLE';
    q('s_irq').textContent='No traffic, nothing to interrupt on';
  } else if(d.intStuck){
    setDot('d_irq','bad'); q('t_irq').textContent='NOT FIRING';
    q('s_irq').textContent='Running on the fallback poll - check the INT wire';
  } else {
    setDot('d_irq','ok'); q('t_irq').textContent=d.irq.toLocaleString()+' /s';
    q('s_irq').textContent='ISR healthy - INT line '+(d.intLevel?'idle high':'asserted');
  }

  /* --- CAN bus load --- */
  var L = d.load;
  q('t_load').textContent = L+'%';
  q('s_load').textContent = d.fps.toLocaleString()+' frames/s';
  var bar = q('loadbar');
  bar.style.width = Math.min(L,100)+'%';
  if(L < 60){ setDot('d_load','ok');   bar.style.background='var(--ok)'; }
  else if(L < 80){ setDot('d_load','warn'); bar.style.background='var(--warn)'; }
  else { setDot('d_load','bad'); bar.style.background='var(--bad)'; }

  /* --- recording --- */
  rec = !!d.rec;
  if(rec){
    setDot('d_rec','ok rec');
    q('t_rec').textContent = 'REC  ' + hms(d.elapsed);
    q('s_rec').textContent = d.file+'  -  '+d.rows.toLocaleString()+' rows, '+
                             (d.kb/1024).toFixed(1)+' MB';
    q('btn').textContent='STOP'; q('btn').className='stop';
  } else if(d.pf){
    setDot('d_rec','bad');
    q('t_rec').textContent='POWER LOSS';
    q('s_rec').textContent=d.file+' was closed safely - press START for a new file';
    q('btn').textContent='START'; q('btn').className='start';
  } else {
    setDot('d_rec','warn');
    q('t_rec').textContent='STOPPED';
    q('s_rec').textContent='Nothing is being saved';
    q('btn').textContent='START'; q('btn').className='start';
  }

  /* --- bus --- */
  if(d.can){ setDot('d_can','ok'); q('t_can').textContent='RECEIVING';
             q('s_can').textContent=d.fps+' frames/s'; }
  else { setDot('d_can','bad'); q('t_can').textContent='NO DATA';
             q('s_can').textContent='Check the wiring, bit rate and crystal'; }

  /* --- integrity --- */
  if(!d.lost){ setDot('d_lost','ok'); q('t_lost').textContent='ALL GOOD';
               q('s_lost').textContent='No frames lost - up to '+d.risk+
                 ' ms at risk if power is cut'; }
  else { setDot('d_lost','bad'); q('t_lost').textContent=d.lost+' LOST';
               q('s_lost').textContent='Some frames could not be saved'; }

  /* --- signals and identifiers --- */
  paintSigs(d.sig, d.dbc);
  q('s_sigs').textContent = d.dbc
      ? ('frame map loaded: '+d.dbcMsg+' messages, '+d.dbcSig+' signals'+
         (d.sigMore ? '  -  showing the first '+d.sig.length : ''))
      : 'add a DBC file to the card to decode signals in real time';

  paintIds(d.ids);
  q('s_ids').textContent = d.idMore
      ? ('more identifiers are on the bus than the table tracks - all of them '+
         'are still recorded')
      : 'every identifier seen since the recording started';

  q('s_up').textContent = 'up '+hms(Math.floor(d.up/1000))+
                          ', '+Math.round(d.heap/1024)+' KB free';
  /* Uptime and free memory live in the Logger card now, so the footer is just
     the firmware string - no leading separator. */
  q('foot').textContent = d.fw;
}

function pollStatus(){
  fetch('/api/status').then(function(r){return r.json()})
    .then(function(d){ fails=0; paint(d); })
    .catch(function(){ if(++fails>2) q('conn').textContent='connection lost'; });
}

function pollLog(){
  fetch('/api/log?since='+seq).then(function(r){return r.json()})
    .then(function(d){
      seq = d.seq;
      if(!d.lines.length) return;
      var t = q('term');
      var atBottom = t.scrollHeight - t.scrollTop - t.clientHeight < 40;
      for(var i=0;i<d.lines.length;i++){
        var s = d.lines[i], div = document.createElement('div');
        var m = s.match(/^\[[^\]]*\]\s(\w)\s/);
        div.className = m ? m[1] : '';
        div.textContent = s;
        t.appendChild(div);
      }
      while(t.childNodes.length > 300) t.removeChild(t.firstChild);
      if(atBottom) t.scrollTop = t.scrollHeight;
    }).catch(function(){});
}

pollStatus(); pollLog();
setInterval(pollStatus, 500);
setInterval(pollLog, 700);
</script>
</body></html>)HTML";

/* ==========================================================================
 *  Handlers
 * ======================================================================== */
static void handleRoot() {
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send_P(200, "text/html", PAGE_HTML);
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
  const uint32_t lost = g_rec.queueDropped + g_rec.canOverflow;

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
      if (si >= DBC_MAX_SIGNALS || !g_live.seen[si]) continue;

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
