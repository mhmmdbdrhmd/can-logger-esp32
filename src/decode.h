/* ============================================================================
 *  decode.h - raw CAN frame -> normalised CSV rows
 *
 *  THE SCHEMA
 *
 *      t_us;id;name;signal;value;unit;raw
 *
 *  Seven fields, ';' separated, always in that order, one row per decoded
 *  SIGNAL. A frame carrying four signals produces four rows that share a
 *  timestamp and an identifier; a frame nothing could decode produces exactly
 *  one row carrying its payload bytes. Nothing else ever appears, so a parser
 *  is a split on ';' and a group-by on (t_us, id) - no DBC, no bit twiddling
 *  and no per-project special cases downstream.
 *
 *      t_us    recorder clock, microseconds since the start of THIS file.
 *              Captured in the CAN interrupt, so it is the arrival time on the
 *              wire, not the time the row happened to be formatted.
 *      id      identifier, "0x18C" style. 29-bit ids print all eight digits.
 *      name    message name from the DBC, or the CANopen function when that
 *              layer is on. Empty for an identifier nothing described.
 *      signal  signal name from the DBC. Empty on a raw row.
 *      value   the physical value, or the symbolic text when the DBC gives the
 *              raw value a name (VAL_). Empty on a raw row.
 *      unit    the DBC unit string. Often empty; that is not an error.
 *      raw     the payload as hex. Always present when the row carries no
 *              decoded signal, so an unmapped bus is still captured in full.
 *              Present on the first row of a decoded frame too when
 *              CSV_INCLUDE_RAW is set, and always on CANopen rows - that layer
 *              only decodes part of a frame, so the bytes stay with it.
 *
 *  Values are exact wherever the DBC factor is a decimal literal - they are
 *  computed and printed as scaled integers, never through a float. See fmt.h.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "mcp2515.h"
#include "dbc.h"

/* Longest a single row can get, and the most rows one frame can produce. A
 * message with more signals than this keeps the first DECODE_MAX_ROWS and gets
 * its raw bytes as well, so nothing is silently lost. */
#define DECODE_ROW_MAX    160
#define DECODE_MAX_ROWS   32
#define DECODE_FRAME_MAX  (DECODE_ROW_MAX * DECODE_MAX_ROWS)

/* ---------------------------------------------------------------------------
 *  Live bus activity - what the dashboard shows. Generic by construction: a
 *  small table of the identifiers actually seen, with counts and rates. There
 *  is no place here for any particular signal, because there is no particular
 *  bus.
 *
 *  Written only by the decode task and read without a lock by the web task.
 *  Each member is a naturally aligned 32-bit word, so a reader sees either the
 *  old or the new value, never a torn one.
 * -------------------------------------------------------------------------*/
#define BUS_PAYLOAD_CHARS 17        /* 8 bytes as hex, plus the terminator */

struct BusStats {
  uint32_t id[BUS_TRACK_IDS];
  uint8_t  ext[BUS_TRACK_IDS];
  uint32_t count[BUS_TRACK_IDS];
  uint32_t prev[BUS_TRACK_IDS];
  uint32_t rate[BUS_TRACK_IDS];
  uint32_t lastMs[BUS_TRACK_IDS];
  uint8_t  known[BUS_TRACK_IDS];      /* 1 = the DBC describes this id       */
  char     last[BUS_TRACK_IDS][BUS_PAYLOAD_CHARS];   /* most recent payload  */

  /* The same payload as bytes. Kept because writing one signal into a message
   * means leaving the message's OTHER signals alone, and the only honest value
   * for the bits this logger is not setting is the last thing the bus said
   * they were. Zeroing them instead would command every other signal in the
   * message to zero as a side effect of setting one. */
  uint8_t  lastData[BUS_TRACK_IDS][8];
  uint8_t  lastLen[BUS_TRACK_IDS];
  uint8_t  used;
  uint32_t untracked;                 /* frames whose id did not fit the table */
  uint32_t undecoded;                 /* frames stored as raw bytes            */
  uint64_t total;
};

extern BusStats g_bus;

void busReset(BusStats &b);

/* The most recent payload seen for an identifier. False when it has not been
 * seen since the table was reset. */
/* Formats the per-identifier counters into one log line's worth of text.
 *
 * A busy bus carries a hundred identifiers and a log line holds about nine, so
 * this writes a WINDOW: it starts at *cursor, fits whole entries only, and
 * leaves *cursor pointing at the next one so the following call continues from
 * there and wraps round. Over a minute of once-a-second lines every identifier
 * has been reported, without turning a 25 MB log into a 300 MB one.
 *
 * Always nul-terminates, never writes a partial entry, and returns how many
 * identifiers it fitted. Lives here rather than in the recorder because this
 * is where the counters live, and because it is the part worth testing.
 */
uint8_t busFormatIds(const BusStats &b, uint8_t *cursor, char *out, size_t cap);

bool busLastPayload(const BusStats &b, uint32_t id, bool ext,
                    uint8_t *out, uint8_t *lenOut);
void busNote(BusStats &b, uint32_t id, bool ext, bool known, uint32_t nowMs);
void busTick(BusStats &b, uint32_t elapsedMs);   /* recomputes the rates */

/* Records one frame in the activity table, including its payload so the live
 * view can show raw traffic on a bus with no frame map. Called for every frame
 * whether or not a recording is running, so the page shows the bus while idle. */
void busObserve(BusStats &b, const CanFrame &f, const DbcDb *db);

/* ---------------------------------------------------------------------------
 *  Latest decoded value of every signal, for the live web view.
 *
 *  One slot per signal in the frame map, holding the value already rendered as
 *  text - the same text the CSV carries. Rendering here rather than in the HTTP
 *  handler keeps the web path free of any decoding, and means the page displays
 *  whatever the DBC describes with nothing named in the firmware.
 *
 *  Written only by the decode task and read without a lock by the web task. A
 *  slot can in principle be read mid-update; the cost is one cosmetically mixed
 *  reading on a dashboard, which is not worth a lock on the hot path.
 * -------------------------------------------------------------------------*/
#define LIVE_TEXT_MAX 16

struct LiveSignals {
  /* One slot per signal the frame map actually holds, allocated alongside it.
   * Fixed at DBC_MAX_SIGNALS it was five kilobytes of static memory whether
   * the bus had twenty signals or none. */
  char     (*text)[LIVE_TEXT_MAX] = nullptr;
  uint32_t  *lastMs   = nullptr;
  uint8_t   *seen     = nullptr;
  uint16_t   cap       = 0;
  uint16_t   seenCount = 0;
};

extern LiveSignals g_live;

/* Clears the values without releasing the slots. */
void liveReset(LiveSignals &l);

/* Sizes it for `signals` slots, releasing whatever was there. False when the
 * heap could not provide them, in which case cap is 0 and the live view is
 * empty - the recording is unaffected either way. */
bool liveAllocate(LiveSignals &l, uint16_t signals);
void liveFree(LiveSignals &l);

/* ---------------------------------------------------------------------------
 *  The decoder itself. One instance per recording; timestamps are rebased so
 *  every file starts at 0.
 * -------------------------------------------------------------------------*/
class Decoder {
public:
  /* Forgets the timestamp origin and every wrap counter in the frame map. */
  void reset(DbcDb *db);

  /* Formats one frame as one or more complete CSV rows (newlines included)
   * into `buf`. `cap` must be at least DECODE_FRAME_MAX. Returns the number of
   * bytes written, and the number of rows through `rowsOut` when given. */
  size_t rows(const CanFrame &f, char *buf, size_t cap, uint8_t *rowsOut = nullptr);

  bool     started() const { return _have; }
  uint64_t epoch() const   { return _epoch; }

private:
  DbcDb   *_db    = nullptr;
  bool     _have  = false;
  uint64_t _epoch = 0;
};

/* Writes the explanatory '#' header block plus the column header line. Returns
 * the number of bytes written, or 0 if it did not fit. */
/* The CSV's one and only header line: just the column names.
 *
 * The legend that used to sit above this as a block of '#' comments now lives
 * in a separate <n>.meta file. Tools that cannot be told to skip comments
 * choked on it, and stripping it by hand before every analysis is not a
 * workflow. The CSV is now pure data. */
size_t csvColumnHeader(char *buf, size_t cap);

/* The companion <n>.meta: everything needed to interpret the CSV, including
 * the frame map that was active when the recording was made, as JSON so a tool
 * can read it directly instead of parsing prose. Needs ~4 KB plus roughly
 * 120 bytes per mapped signal. */
size_t metaJson(char *buf, size_t cap, const char *csvName, const char *logName,
                const DbcDb &db);

size_t csvHeaderBlock(char *buf, size_t cap, const char *filename,
                      const DbcDb &db);
