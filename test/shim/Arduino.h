/* Host shim: enough of Arduino + FreeRTOS to compile and RUN the logger's
 * portable logic (the DBC parser, decoding, formatting, the logger ring)
 * natively, with no ESP32 and no CAN hardware. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string>
#include <deque>
#include <vector>

#define PROGMEM
#define IRAM_ATTR
#define LOW 0
#define HIGH 1
#define INPUT 0
#define INPUT_PULLUP 2
#define OUTPUT 1
#define CHANGE 1
#define FALLING 2
#define RISING 3

/* ---------------------------------------------------------------------------
 *  MACRO LANDMINES - deliberately reproduced, do not "clean these up".
 *
 *  The real Arduino core defines these as object-like macros. Any identifier in
 *  our code that happens to share one of these names gets textually replaced,
 *  producing errors that look nothing like the cause - `static const char
 *  HEX[]` becomes `static const char 16[]` and reports "expected unqualified-id
 *  before numeric constant".
 *
 *  A shim that omits them happily compiles code the real toolchain rejects.
 *  Keeping them here means the host build fails the same way the ESP32 build
 *  would, in one second instead of twenty minutes.
 * -------------------------------------------------------------------------*/
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2
#define LSBFIRST 0
#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352
#define bit(b) (1UL << (b))
#define sq(x) ((x) * (x))
#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
#define bitRead(v, b) (((v) >> (b)) & 0x01)
#define bitSet(v, b) ((v) |= (1UL << (b)))
#define bitClear(v, b) ((v) &= ~(1UL << (b)))
#define interrupts() do {} while (0)
#define noInterrupts() do {} while (0)

extern uint32_t g_fakeMs;
static inline uint32_t millis() { return g_fakeMs; }
static inline uint32_t micros() { return g_fakeMs * 1000u; }
static inline void digitalWrite(int, int) {}
static inline void pinMode(int, int) {}
static inline void delay(uint32_t) {}
static inline void delayMicroseconds(uint32_t) {}
static inline int64_t esp_timer_get_time() { return (int64_t)g_fakeMs * 1000; }

/* ---- String -------------------------------------------------------- */
class String : public std::string {
public:
  String() {}
  String(const char *s) : std::string(s ? s : "") {}
  String(const std::string &s) : std::string(s) {}
  String &operator+=(const char *s)   { append(s); return *this; }
  String &operator+=(char c)          { push_back(c); return *this; }
  String &operator+=(int v)           { append(std::to_string(v)); return *this; }
  String &operator+=(unsigned v)      { append(std::to_string(v)); return *this; }
  String &operator+=(long v)          { append(std::to_string(v)); return *this; }
  String &operator+=(unsigned long v) { append(std::to_string(v)); return *this; }
  String &operator+=(const String &s) { append(s); return *this; }
  void reserve(size_t n)              { std::string::reserve(n); }
  void trim() { size_t a=find_first_not_of(" \t\r\n"); size_t b=find_last_not_of(" \t\r\n");
                if(a==npos){clear();} else {*this=String(substr(a,b-a+1));} }
  int indexOf(char c) const { size_t i=find(c); return i==npos?-1:(int)i; }
  String substring(int a,int b) const { return String(substr(a,b-a)); }
  String substring(int a) const { return String(substr(a)); }
  bool equalsIgnoreCase(const char* o) const {
    std::string t(o); if(t.size()!=size()) return false;
    for(size_t i=0;i<size();i++) if(tolower((*this)[i])!=tolower(t[i])) return false;
    return true; }
  long toInt() const { return atol(c_str()); }
  bool operator==(const char* o) const { return std::string(*this)==std::string(o); }
  size_t length() const               { return std::string::size(); }
  const char *c_str() const           { return std::string::c_str(); }
};

/* ---- Serial -------------------------------------------------------- */
struct FakeSerial {
  std::vector<std::string> lines;
  void begin(unsigned long) {}
  void println(const char *s) { lines.push_back(s); }
  void print(const char *) {}
  void print(char) {}
};
extern FakeSerial Serial;

/* ---- ESP ----------------------------------------------------------- */
struct FakeEsp {
  uint32_t getFreeHeap()     { return 200000; }
  uint32_t getMinFreeHeap()  { return 180000; }
  uint32_t getFlashChipSize(){ return 4194304; }
  const char *getChipModel() { return "ESP32"; }
  int getChipRevision()      { return 3; }
  int getCpuFreqMHz()        { return 240; }
};
extern FakeEsp ESP;

/* ---- FreeRTOS ------------------------------------------------------ */
typedef int BaseType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)

struct QueueDef { std::deque<std::vector<uint8_t>> q; size_t item; size_t cap; };
typedef QueueDef *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(size_t len, size_t item) {
  QueueDef *d = new QueueDef(); d->item = item; d->cap = len; return d;
}
static inline BaseType_t xQueueSend(QueueHandle_t h, const void *src, int) {
  if (!h || h->q.size() >= h->cap) return pdFALSE;
  const uint8_t *b = (const uint8_t *)src;
  h->q.push_back(std::vector<uint8_t>(b, b + h->item));
  return pdTRUE;
}
static inline BaseType_t xQueueReceive(QueueHandle_t h, void *dst, int) {
  if (!h || h->q.empty()) return pdFALSE;
  memcpy(dst, h->q.front().data(), h->item);
  h->q.pop_front();
  return pdTRUE;
}
static inline size_t uxQueueMessagesWaiting(QueueHandle_t h) { return h ? h->q.size() : 0; }

typedef void *TaskHandle_t;
static inline void vTaskDelay(int) {}

typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void *)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, int) { return pdTRUE; }
static inline void xSemaphoreGive(SemaphoreHandle_t) {}
