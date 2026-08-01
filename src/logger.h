/* ============================================================================
 *  logger.h - three sinks, one call site
 *
 *  There are two verbosity tiers, and every message picks one:
 *
 *    LOG_LIVE(...)  what an operator needs to see: state changes, faults, and
 *                   one compact status line per second. Goes to the serial
 *                   port, to the web terminal, AND to the .log file.
 *    LOG_FILE(...)  the deep detail: per-id counters, SD write timings, queue
 *                   high-water marks, controller error registers, heap. Goes
 *                   only to the .log file on the SD card.
 *
 *  So <n>.log is a strict superset of the serial output, which is what keeps
 *  the serial stream informative without making it an I/O cost of its own.
 *
 *  Calls are non-blocking: the message is rendered into a fixed-size slot and
 *  posted to a queue. Exactly one task (the SD writer) ever touches the card or
 *  the serial port, so there is no interleaving and no lock is ever held across
 *  a multi-millisecond SD write.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include <FS.h>
#include "config.h"

enum LogLevel : uint8_t { LVL_DEBUG = 0, LVL_INFO = 1, LVL_WARN = 2, LVL_ERROR = 3 };

void logInit();

/* Renders and posts a line. Prefer the macros below. */
void logPost(LogLevel lvl, bool live, const char *fmt, ...)
     __attribute__((format(printf, 3, 4)));

#define LOG_LIVE(lvl, ...)  logPost(lvl, true,  __VA_ARGS__)
#define LOG_FILE(lvl, ...)  logPost(lvl, false, __VA_ARGS__)

/* Called only from the SD writer task: drains the queue into the serial port,
 * the web ring buffer and the currently attached .log file. */
void logService();

/* The recorder hands over the open .log file for the current recording. Pass
 * nullptr when the recording stops; lines posted while detached still reach the
 * serial port and the web terminal. */
void logAttachFile(File *f);
bool logFileAttached();

/* Number of lines dropped because the queue was full - a real bottleneck
 * indicator, reported once per second into the .log file. */
uint32_t logDroppedCount();

/* ---- web terminal ring -------------------------------------------------- */
/* Sequence number of the newest line ever produced. The browser polls with the
 * last sequence it has and receives only what is new. */
uint32_t webLogSeq();

/* Appends the lines newer than `since` to `out` as JSON string elements
 * (comma separated, no enclosing brackets). Returns the new sequence. */
uint32_t webLogToJson(uint32_t since, String &out);
