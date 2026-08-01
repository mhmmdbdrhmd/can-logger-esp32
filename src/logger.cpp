#include "logger.h"
#include <stdarg.h>

struct LogLine {
  uint32_t ms;
  uint8_t  lvl;
  bool     live;
  char     text[LOG_LINE_CHARS];
};

static QueueHandle_t     s_queue    = nullptr;
static File             *s_file     = nullptr;
static volatile uint32_t s_dropped  = 0;

/* Ring of the most recent LIVE lines, for the web terminal. */
static char              s_ring[WEB_LOG_LINES][LOG_LINE_CHARS + 16];
static volatile uint32_t s_ringSeq  = 0;      /* total lines ever pushed     */
static SemaphoreHandle_t s_ringLock = nullptr;

static const char LVL_CHAR[4] = { 'D', 'I', 'W', 'E' };

void logInit() {
  s_queue    = xQueueCreate(LOG_QUEUE_LEN, sizeof(LogLine));
  s_ringLock = xSemaphoreCreateMutex();
}

void logAttachFile(File *f) { s_file = f; }
bool logFileAttached()      { return s_file != nullptr; }
uint32_t logDroppedCount()  { return s_dropped; }

void logPost(LogLevel lvl, bool live, const char *fmt, ...) {
  if (!s_queue) return;

  LogLine ln;
  ln.ms   = millis();
  ln.lvl  = (uint8_t)lvl;
  ln.live = live;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ln.text, sizeof(ln.text), fmt, ap);
  va_end(ap);

  /* Never block a producer - a full queue means the SD card is stalling, and
   * the last thing we want is for logging to back-pressure the CAN path. */
  if (xQueueSend(s_queue, &ln, 0) != pdTRUE) s_dropped++;
}

static void ringPush(const char *rendered) {
  if (!s_ringLock) return;
  if (xSemaphoreTake(s_ringLock, pdMS_TO_TICKS(20)) != pdTRUE) return;
  strncpy(s_ring[s_ringSeq % WEB_LOG_LINES], rendered, sizeof(s_ring[0]) - 1);
  s_ring[s_ringSeq % WEB_LOG_LINES][sizeof(s_ring[0]) - 1] = '\0';
  s_ringSeq++;
  xSemaphoreGive(s_ringLock);
}

void logService() {
  if (!s_queue) return;

  LogLine ln;
  char    out[LOG_LINE_CHARS + 24];

  while (xQueueReceive(s_queue, &ln, 0) == pdTRUE) {
    /* [   12.345] I  message  - seconds since boot, ms resolution */
    snprintf(out, sizeof(out), "[%6lu.%03lu] %c %s",
             (unsigned long)(ln.ms / 1000UL),
             (unsigned long)(ln.ms % 1000UL),
             LVL_CHAR[ln.lvl & 3], ln.text);

    if (ln.live) {
      Serial.println(out);
      ringPush(out);
    }
    if (s_file) {
      s_file->print(out);
      s_file->print('\n');
    }
  }
}

uint32_t webLogSeq() { return s_ringSeq; }

/* Minimal JSON string escaping - log lines are ASCII and control-character
 * free by construction, so only the two structural characters can appear. */
static void appendEscaped(String &out, const char *s) {
  out += '"';
  for (const char *p = s; *p; ++p) {
    if      (*p == '"')  out += "\\\"";
    else if (*p == '\\') out += "\\\\";
    else if ((uint8_t)*p >= 0x20) out += *p;
  }
  out += '"';
}

uint32_t webLogToJson(uint32_t since, String &out) {
  if (!s_ringLock) return 0;
  if (xSemaphoreTake(s_ringLock, pdMS_TO_TICKS(50)) != pdTRUE) return since;

  const uint32_t seq = s_ringSeq;

  /* A client that has been away longer than the ring is deep gets whatever is
   * still held, rather than nothing. */
  uint32_t from = since;
  if (seq > WEB_LOG_LINES && from < seq - WEB_LOG_LINES) from = seq - WEB_LOG_LINES;
  if (from > seq) from = seq;                 /* client saw a reboot */

  bool first = true;
  for (uint32_t i = from; i < seq; i++) {
    if (!first) out += ',';
    appendEscaped(out, s_ring[i % WEB_LOG_LINES]);
    first = false;
  }

  xSemaphoreGive(s_ringLock);
  return seq;
}
