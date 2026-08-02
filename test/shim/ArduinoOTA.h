/* Host-side stub for ArduinoOTA.
 *
 * The type-check job compiles the Arduino sketch folder with a normal g++ so a
 * syntax or signature error is caught in CI rather than by whoever next opens
 * the IDE. That needs every header the sketch includes to exist on the host --
 * hence this stub. It implements the surface app.cpp actually uses and nothing
 * more; it is never compiled into firmware. */
#pragma once
#include <Arduino.h>
#include <functional>

typedef enum {
  OTA_AUTH_ERROR = 0,
  OTA_BEGIN_ERROR,
  OTA_CONNECT_ERROR,
  OTA_RECEIVE_ERROR,
  OTA_END_ERROR
} ota_error_t;

class ArduinoOTAClass {
 public:
  void setHostname(const char *) {}
  void setPassword(const char *) {}
  void setPort(uint16_t) {}
  void onStart(std::function<void()>) {}
  void onEnd(std::function<void()>) {}
  void onProgress(std::function<void(unsigned int, unsigned int)>) {}
  void onError(std::function<void(ota_error_t)>) {}
  void begin() {}
  void handle() {}
};

extern ArduinoOTAClass ArduinoOTA;
