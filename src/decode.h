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
  uint8_t  used;
  uint32_t untracked;                 /* frames whose id did not fit the table */
  uint32_t undecoded;                 /* frames stored as raw bytes            */
  uint64_t total;
};

extern BusStats g_bus;

void busReset(BusStats &b);
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
  char     text[DBC_MAX_SIGNALS][LIVE_TEXT_MAX];
  uint32_t lastMs[DBC_MAX_SIGNALS];
  uint8_t  seen[DBC_MAX_SIGNALS];
  uint16_t seenCount;
};

extern LiveSignals g_live;

void liveReset(LiveSignals &l);

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
size_t csvHeaderBlock(char *buf, size_t cap, const char *filename,
                      const DbcDb &db);
