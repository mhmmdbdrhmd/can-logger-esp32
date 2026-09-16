#include "netcfg.h"
#include "config.h"
#include "logger.h"
#include "mem.h"
#include "recorder.h"

#include <WiFi.h>
#include <SD.h>

NetCfg g_net;

static const char *CONFIG_PATH = "/config.txt";

static const char DEFAULT_CONFIG[] PROGMEM =
  "# ============================================================\n"
  "#  CAN logger - network configuration\n"
  "#\n"
  "#  Edit this file on any computer, put the card back, power\n"
  "#  cycle the logger. Lines starting with # are ignored.\n"
  "# ============================================================\n"
  "\n"
  "# Which mode to use:\n"
  "#   ap  = the logger creates its own hotspot (no infrastructure\n"
  "#         needed - connect a phone or laptop directly to it)\n"
  "#   sta = the logger joins an existing Wi-Fi network\n"
  "mode = " DEF_WIFI_MODE "\n"
  "\n"
  "# --- hotspot mode (mode = ap) -------------------------------\n"
  "# ap_pass must be at least 8 characters, or empty for an open network.\n"
  "ap_ssid = " DEF_AP_SSID "\n"
  "ap_pass = " DEF_AP_PASS "\n"
  "\n"
  "# --- join an existing network (mode = sta) ------------------\n"
  "wifi_ssid = " DEF_STA_SSID "\n"
  "wifi_pass = " DEF_STA_PASS "\n"
  "\n"
  "# If the network cannot be joined within this many milliseconds the\n"
  "# logger falls back to hotspot mode, so the dashboard is always\n"
  "# reachable. Set to 0 to keep retrying instead.\n"
  "sta_timeout_ms = 15000\n"
  "\n"
  "# --- both modes ---------------------------------------------\n"
  "# Reachable as http://<hostname>.local on networks with mDNS.\n"
  "hostname = " DEF_HOSTNAME "\n"
  "http_port = 80\n";

static char s_status[128] = "not started";

static void applyDefaults() {
  g_net.apMode     = (strcmp(DEF_WIFI_MODE, "ap") == 0);
  g_net.staSsid    = DEF_STA_SSID;
  g_net.staPass    = DEF_STA_PASS;
  g_net.apSsid     = DEF_AP_SSID;
  g_net.apPass     = DEF_AP_PASS;
  g_net.hostname   = DEF_HOSTNAME;
  g_net.httpPort   = DEF_HTTP_PORT;
  g_net.staTimeout = DEF_STA_TIMEOUT_MS;
}

static String trim(const String &s) {
  String t = s;
  t.trim();
  return t;
}

void netLoadConfig() {
  applyDefaults();

  if (!g_rec.sdOk) {
    LOG_LIVE(LVL_WARN, "no SD card - using the network settings compiled into the firmware");
    return;
  }

  if (!SD.exists(CONFIG_PATH)) {
    File f = SD.open(CONFIG_PATH, FILE_WRITE);
    if (f) {
      f.print((const __FlashStringHelper *)DEFAULT_CONFIG);
      f.close();
      LOG_LIVE(LVL_INFO, "created %s on the SD card - edit it to set your Wi-Fi",
               CONFIG_PATH);
    } else {
      LOG_LIVE(LVL_WARN, "could not create %s", CONFIG_PATH);
    }
    return;
  }

  File f = SD.open(CONFIG_PATH, FILE_READ);
  if (!f) { LOG_LIVE(LVL_WARN, "could not open %s", CONFIG_PATH); return; }

  uint16_t applied = 0;
  while (f.available()) {
    String line = trim(f.readStringUntil('\n'));
    if (!line.length() || line[0] == '#' || line[0] == ';') continue;

    const int eq = line.indexOf('=');
    if (eq < 0) continue;

    const String key = trim(line.substring(0, eq));
    const String val = trim(line.substring(eq + 1));

    if      (key == "mode")           { g_net.apMode = !val.equalsIgnoreCase("sta"); }
    else if (key == "wifi_ssid")      { g_net.staSsid = val; }
    else if (key == "wifi_pass")      { g_net.staPass = val; }
    else if (key == "ap_ssid")        { g_net.apSsid = val; }
    else if (key == "ap_pass")        { g_net.apPass = val; }
    else if (key == "hostname")       { g_net.hostname = val; }
    else if (key == "http_port")      { g_net.httpPort = (uint16_t)val.toInt(); }
    else if (key == "sta_timeout_ms") { g_net.staTimeout = (uint32_t)val.toInt(); }
    else continue;
    applied++;
  }
  f.close();

  g_net.fromFile = true;
  LOG_LIVE(LVL_INFO, "loaded %s (%u settings)", CONFIG_PATH, applied);
  /* Passwords are deliberately not logged. */
  LOG_FILE(LVL_INFO, "config: mode=%s ap_ssid='%s' wifi_ssid='%s' host='%s' port=%u timeout=%lu",
           g_net.apMode ? "ap" : "sta", g_net.apSsid.c_str(), g_net.staSsid.c_str(),
           g_net.hostname.c_str(), g_net.httpPort, (unsigned long)g_net.staTimeout);
}

static void startAp(const char *why) {
  WiFi.mode(WIFI_AP);
  WiFi.softAPsetHostname(g_net.hostname.c_str());

  const bool open = g_net.apPass.length() < 8;
  if (open && g_net.apPass.length() > 0) {
    LOG_LIVE(LVL_WARN, "ap_pass is shorter than 8 characters - starting an OPEN hotspot");
  }
  WiFi.softAP(g_net.apSsid.c_str(), open ? nullptr : g_net.apPass.c_str());

  snprintf(s_status, sizeof(s_status), "AP '%s' %s ip=%s",
           g_net.apSsid.c_str(), open ? "(open)" : "(protected)",
           WiFi.softAPIP().toString().c_str());
  LOG_LIVE(LVL_INFO, "HOTSPOT '%s' is up%s - open http://%s in a browser",
           g_net.apSsid.c_str(), why, WiFi.softAPIP().toString().c_str());
}

/* Set when the driver itself could not be brought up. Retrying that is not a
 * recovery strategy - the memory it wanted is not going to appear while the
 * logger is recording - and each attempt reprints four lines of ESP-IDF
 * errors, which is how the one message that explains the fault scrolls off
 * the top of somebody's serial monitor. */
static bool s_wifiDead = false;

void netBegin() {
  WiFi.persistent(false);

  if (g_net.apMode) { startAp(""); return; }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(g_net.hostname.c_str());
  WiFi.setSleep(false);                 /* keeps the dashboard responsive */

  /* begin() returns a wl_status_t, NOT a bool - and WL_CONNECT_FAILED is 4, so
   * testing it with ! would never fire. It is the code the core returns when
   * wifiLowLevelInit() could not bring the driver up at all, which on this
   * firmware means one thing in practice: esp_wifi_init could not get its
   * memory, and the core printed "STA enable failed!" without ever mentioning
   * memory. Said plainly here, with the numbers that fix it, because the
   * alternative is reading ESP-IDF error codes off a serial log.
   *
   * This is NOT the same as failing to join: a wrong password or an absent
   * access point still starts the driver and shows up as a timeout below. */
  if (WiFi.begin(g_net.staSsid.c_str(), g_net.staPass.c_str())
        == WL_CONNECT_FAILED) {
    LOG_LIVE(LVL_ERROR, "the Wi-Fi driver would not start - almost always out "
                        "of memory (esp_wifi_init -> ESP_ERR_NO_MEM), not a "
                        "wrong SSID. %lu bytes free, largest block %lu. Lower "
                        "FRAME_QUEUE_LEN or SD_BLOCK_BYTES in config.h and "
                        "reflash; recording is unaffected and continues.",
             (unsigned long)memStat().freeNow,
             (unsigned long)memLargestBlock());
    snprintf(s_status, sizeof(s_status), "Wi-Fi driver failed to start");
    s_wifiDead = true;
    /* No AP fallback: it would need the same allocation and fail the same way,
     * and a second identical failure in the log helps nobody. */
    g_net.apMode = false;
    return;
  }

  LOG_LIVE(LVL_INFO, "joining Wi-Fi '%s' ...", g_net.staSsid.c_str());

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (g_net.staTimeout && (millis() - t0) > g_net.staTimeout) {
      LOG_LIVE(LVL_WARN, "could not join '%s' in %lu ms", g_net.staSsid.c_str(),
               (unsigned long)g_net.staTimeout);
      startAp(" as a fallback");
      g_net.apMode = true;
      return;
    }
    delay(200);
  }

  snprintf(s_status, sizeof(s_status), "STA '%s' ip=%s rssi=%d dBm",
           g_net.staSsid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  LOG_LIVE(LVL_INFO, "connected to '%s' - open http://%s in a browser",
           g_net.staSsid.c_str(), WiFi.localIP().toString().c_str());
}

void netService() {
  if (s_wifiDead) return;

  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();

  if (g_net.apMode) {
    snprintf(s_status, sizeof(s_status), "AP '%s' ip=%s clients=%d",
             g_net.apSsid.c_str(), WiFi.softAPIP().toString().c_str(),
             WiFi.softAPgetStationNum());
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(s_status, sizeof(s_status), "STA '%s' ip=%s rssi=%d dBm",
             g_net.staSsid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    snprintf(s_status, sizeof(s_status), "STA '%s' DISCONNECTED", g_net.staSsid.c_str());
    /* Reconnect quietly; recording is unaffected either way. */
    WiFi.reconnect();
  }
}

bool   netIsAp() { return g_net.apMode; }
String netIp()   { return g_net.apMode ? WiFi.softAPIP().toString()
                                       : WiFi.localIP().toString(); }
const char *netStatusLine() { return s_status; }
