#pragma once
#include "Arduino.h"
#define WIFI_AP 2
#define WIFI_STA 1
#define WL_CONNECTED 3
struct IPAddress { String toString() const { return String("192.168.4.1"); } };
struct FakeWiFi {
  void persistent(bool) {} void mode(int) {}
  void softAPsetHostname(const char*) {}
  bool softAP(const char*, const char* = nullptr) { return true; }
  IPAddress softAPIP() { return IPAddress(); }
  int softAPgetStationNum() { return 1; }
  void setHostname(const char*) {} void setSleep(bool) {}
  void begin(const char*, const char*) {}
  int status() { return WL_CONNECTED; }
  IPAddress localIP() { return IPAddress(); }
  int RSSI() { return -55; }
  void reconnect() {}
};
extern FakeWiFi WiFi;
