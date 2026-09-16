#pragma once
#include "Arduino.h"
#define WIFI_AP 2
#define WIFI_STA 1
/* wl_status_t, with the values the real header gives them. begin() returns
 * one of these and NOT a bool - WL_CONNECT_FAILED is 4, so testing it with
 * ! silently never fires. That bug was written and this shim caught it. */
#define WL_IDLE_STATUS   0
#define WL_CONNECTED     3
#define WL_CONNECT_FAILED 4
#define WL_DISCONNECTED  6
struct IPAddress { String toString() const { return String("192.168.4.1"); } };

/* One live connection. The real class hands out a shared socket, so a copy
 * taken by a handler refers to the SAME connection - which is what lets a
 * streaming handler call stop() on its copy and really close the socket.
 * Only the two calls the firmware makes are modelled. */
struct WiFiClient {
  bool connected() const { return true; }
  void stop() {}

  /* Enough of the real API for the abandoned-page abort path to compile and
   * be type-checked. The firmware sets SO_LINGER to zero on a page the client
   * has given up on, so the close that follows is a RST and lwIP hands the
   * queued buffers straight back instead of retransmitting them for minutes -
   * see WEB_ABORT_ON_ABANDON in config.h.
   *
   * No counters: nothing here can observe a socket option, so a test asserting
   * on one would be asserting about this mock rather than about the firmware.
   * The value of the shim is that the call compiles against the same signature
   * the core declares. */
  int fd() const { return 3; }
  int setSocketOption(int level, int option, const void *value, size_t len) {
    (void)level; (void)option; (void)value; (void)len;
    return 0;
  }
};
/* What the last useStaticBuffers() call asked for, and whether the radio had
 * already been touched when it did.
 *
 * The flag is only read inside the core's lazy wifiLowLevelInit(), so setting
 * it after any other WiFi call silently does nothing - and "silently does
 * nothing" is exactly the failure that would look like the fix not working.
 * The counter lets a host test assert the ORDER, not just the call. */
struct WiFiShimState {
  bool staticBuffers   = false;   /* what was asked for            */
  int  touchedBefore   = 0;       /* other WiFi calls made first   */
  int  otherCalls      = 0;
};
inline WiFiShimState &wifiShim() { static WiFiShimState s; return s; }

struct FakeWiFi {
  void useStaticBuffers(bool on) {
    wifiShim().staticBuffers = on;
    wifiShim().touchedBefore = wifiShim().otherCalls;
  }
  void persistent(bool) { wifiShim().otherCalls++; }
  void mode(int)        { wifiShim().otherCalls++; }
  void softAPsetHostname(const char*) {}
  bool softAP(const char*, const char* = nullptr) { return true; }
  IPAddress softAPIP() { return IPAddress(); }
  int softAPgetStationNum() { return 1; }
  void setHostname(const char*) {} void setSleep(bool) {}
  /* The real one hands back a status, not nothing: WL_CONNECT_FAILED when
   * the driver could not be brought up, otherwise a not-yet-connected
   * code. Returning WL_DISCONNECTED here is the success-shaped answer. */
  int begin(const char*, const char*) { return WL_DISCONNECTED; }
  int status() { return WL_CONNECTED; }
  IPAddress localIP() { return IPAddress(); }
  int RSSI() { return -55; }
  void reconnect() {}
};
extern FakeWiFi WiFi;
