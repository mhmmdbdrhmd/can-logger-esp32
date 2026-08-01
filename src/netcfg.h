/* ============================================================================
 *  netcfg.h - /config.txt on the SD card, and bringing Wi-Fi up from it
 *
 *  Credentials live on the card, not in the firmware, so the logger can be
 *  moved between sites by editing a text file - no toolchain, no reflash. If
 *  the file is missing, a fully commented default is written out on first boot,
 *  which doubles as the documentation.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

struct NetCfg {
  bool     apMode      = true;      /* true = hotspot, false = join a network */
  String   staSsid;
  String   staPass;
  String   apSsid;
  String   apPass;
  String   hostname;
  uint16_t httpPort    = 80;
  uint32_t staTimeout  = 15000;     /* 0 = never fall back to hotspot         */
  bool     fromFile    = false;     /* false = compiled-in defaults are in use*/
};

extern NetCfg g_net;

/* Reads /config.txt (call after the SD card is mounted). Creates it with the
 * compiled-in defaults when absent. Safe to call with no card - the defaults
 * from config.h are then used. */
void netLoadConfig();

/* Starts station or access-point mode according to the config. Station mode
 * falls back to the hotspot if the network cannot be joined in time, so there
 * is always a way to reach the dashboard. */
void netBegin();

/* Re-checks the link; call occasionally from loop(). */
void netService();

bool        netIsAp();
String      netIp();
const char *netStatusLine();       /* one-line summary for the detailed log  */
