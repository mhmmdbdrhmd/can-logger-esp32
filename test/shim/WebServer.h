#pragma once
#include "Arduino.h"
#define HTTP_GET 1
#define HTTP_POST 2
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)
class WebServer {
public:
  WebServer(uint16_t){}
  void on(const char*, int, void(*)()) {}
  void onNotFound(void(*)()) {}
  void begin() {}
  void handleClient() {}
  void send(int, const char*, const String&) {}
  void send(int, const char*, const char*) {}
  void send_P(int, const char*, const char*) {}
  void setContentLength(size_t) {}
  void sendContent(const String&) {}
  void sendContent(const char*) {}
  void sendContent_P(const char*) {}
  void sendContent_P(const char*, size_t) {}
  void sendHeader(const char*, const char*) {}
  bool hasArg(const char*) { return false; }
  String arg(const char*) { return String("0"); }
  int args() { return 0; }
};
