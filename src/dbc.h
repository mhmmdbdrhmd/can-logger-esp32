/* ============================================================================
 *  dbc.h - the frame map, read from a DBC file at boot
 *
 *  This is what keeps the firmware generic. Nothing about any particular bus is
 *  compiled in: identifiers, signal layouts, scaling factors, units and enum
 *  labels all come from a text file on the SD card, so the same binary logs a
 *  vehicle bus, a test rig or a CANopen drive without being rebuilt.
 *
 *  If the file is absent the logger records raw payload bytes instead. That is
 *  a supported mode, not a failure: an unmapped bus is still fully captured,
 *  and the CSV can be decoded offline once a DBC exists.
 *
 *  SUPPORTED SUBSET
 *  ----------------
 *    VERSION "..."                       kept and written into the CSV header
 *    BU_: <Node> <Node> ...              the nodes on this bus
 *    BO_ <id> <Name>: <dlc> <Node>       message; <Node> is its TRANSMITTER
 *    SG_ <Name> [mux] : <start>|<len>@<order><sign> (<fac>,<off>)
 *                       [<min>|<max>] "<unit>" <receivers>
 *    VAL_ <id> <Signal> <n> "label" ... ;   enumerated values
 *    SIG_VALTYPE_ <id> <Signal> : 1;        1 = IEEE float, 2 = double
 *    BA_ "Unwrap" SG_ <id> <Signal> 1;      see below
 *    CM_ / NS_ / BS_ / BA_DEF_ ...          parsed past, ignored
 *
 *  THE TRANSMITTER, AND WHY IT IS KEPT
 *  -----------------------------------
 *  Every `BO_` names the node that sends the message. That single token is the
 *  only thing in a DBC that distinguishes a reading from a command: on
 *  `BO_ 288 HostCommand: 8 Host` the sender is the tool, so it is something
 *  this logger WRITES; on `BO_ 256 NodeStatus: 8 NodeA` the sender is the
 *  ECU, so it is something this logger READS. Nothing here acts
 *  on that by itself - the decoder records whatever arrives - but the dashboard
 *  uses it to stop offering command frames as gauges and readings as setpoints.
 *
 *  Multiplexed signals (`m0`, `M`) are honoured: a multiplexed signal is only
 *  emitted when the message's multiplexor currently selects it.
 *
 *  THE "Unwrap" ATTRIBUTE
 *  ----------------------
 *  Free-running counters are common on machine buses - a node stamps its own
 *  microsecond clock into the payload, and that clock wraps. Declaring
 *
 *      BA_DEF_ SG_ "Unwrap" INT 0 1;
 *      BA_ "Unwrap" SG_ 512 SourceTime 1;
 *
 *  makes the logger track the wraps and emit a value that keeps counting up
 *  across them, so the column stays monotonic over a long session and can be
 *  differentiated without a special case downstream.
 *
 *  EXACTNESS
 *  ---------
 *  Where the factor and offset are decimal (which is nearly always - 1, 0.001,
 *  1e-06, 2.5 ...) the physical value is computed in integer arithmetic and the
 *  decimal point is simply placed. No float is involved anywhere in the hot
 *  path, so the CSV is bit-exact with respect to the bus and formatting stays
 *  fast enough for four-figure frame rates. Factors that cannot be expressed
 *  that way fall back to double, which is flagged in the CSV header.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "config.h"

/* Message and signal names, including the nul.
 *
 * 24 was too short for real files: `GuidanceCurvatureCommand` is 24 characters
 * and was written into the CSV as `GuidanceCurvatureComman`, so matching rows
 * against the source DBC by exact name silently missed them. The tables are
 * heap-allocated and sized to the file now, so eight more bytes per signal
 * costs about six kilobytes on a seven-hundred-signal bus and nothing at all
 * on a small one. Anything still too long is counted and reported, never
 * clipped in silence. */
#define DBC_NAME_MAX    32
#define DBC_UNIT_MAX    10
#define DBC_LABEL_MAX   16
#define DBC_VERSION_MAX 96
#define DBC_LINE_MAX    256

/* Value scaling that cannot be done in integers is rare; when it happens the
 * signal carries a double factor instead and `exact` is 0. */
struct DbcSignal {
  char     name[DBC_NAME_MAX];
  char     unit[DBC_UNIT_MAX];

  uint8_t  startBit;
  uint8_t  bits;
  uint8_t  intel;        /* 1 = @1 Intel/little-endian, 0 = @0 Motorola     */
  uint8_t  isSigned;
  uint8_t  fltType;      /* 0 = integer, 1 = IEEE float32, 2 = float64      */

  /* physical * 10^dec == raw * num + off   (only when `exact`)             */
  int32_t  num;
  int32_t  off;
  uint8_t  dec;
  uint8_t  exact;
  double   fFactor;
  double   fOffset;

  /* The [min|max] range as written in the DBC. The decoder ignores it - it
   * records what is on the wire, in range or not - but it is what lets the
   * dashboard scale a gauge correctly the moment a signal is picked, instead
   * of asking the user for numbers the file already knows. hasRange is 0 when
   * the file wrote [0|0], which every exporter emits for "unspecified". */
  float    phyMin;
  float    phyMax;
  uint8_t  hasRange;

  uint8_t  unwrap;       /* free-running counter: keep counting across wraps */
  uint32_t unwrapLast;
  uint32_t unwrapCount;

  int16_t  muxValue;     /* -1 = plain, -2 = the multiplexor, >=0 = selected */

  int16_t  valFirst;     /* first entry in the value table, -1 = none        */
  uint8_t  valCount;
};

struct DbcMessage {
  uint32_t id;
  uint8_t  ext;          /* 1 = 29-bit identifier                            */
  uint8_t  dlc;
  char     name[DBC_NAME_MAX];
  uint16_t firstSignal;
  uint16_t signalCount;
  int16_t  muxSignal;    /* index of the multiplexor signal, -1 = none       */
  int8_t   txNode;       /* index into DbcDb::node, -1 = unnamed / Vector    */
};

struct DbcValDesc {
  uint16_t sig;
  int32_t  value;
  char     label[DBC_LABEL_MAX];
};

/* How many of each a file contains. Counted in a first pass so the tables can
 * be sized to the file instead of to a guess. */
struct DbcCounts {
  uint16_t messages;
  uint16_t signals;
  uint16_t values;
};

/* The frame map.
 *
 * The three tables are HEAP BLOCKS sized to the file, not fixed arrays. They
 * used to be fixed, at 64 messages and 256 signals, and that was a real cost:
 * a field recording of a 104-message, 707-signal steering bus decoded 256 of
 * those signals and logged the other 451 as raw bytes, with nothing but a line
 * in the CSV header to say so.
 *
 * Sizing to the file also makes the common case cheaper - a twenty-signal bus
 * now costs two kilobytes instead of thirty-two - and moves the whole thing
 * off the static segment, which is the tightest part of this build.
 *
 * `msgCap`/`sigCap`/`valCap` are what was actually allocated. When the heap
 * cannot provide what the file needs, the load falls back to a smaller table
 * and sets `overflow`, which is exactly the old behaviour - so the failure
 * mode is no worse than before, it is just far rarer. */
struct DbcDb {
  /* Every member is initialised here, not left to the caller. The tables are
   * now pointers, and dbcAllocate() releases whatever was there first - so a
   * plain `DbcDb db;` with indeterminate pointers would free three garbage
   * addresses on its first use. Defaulting them makes that impossible rather
   * than merely documented. */
  char       node[DBC_MAX_NODES][DBC_NAME_MAX] = {};
  uint8_t    nodeCount   = 0;

  DbcMessage *msg = nullptr;
  DbcSignal  *sig = nullptr;
  DbcValDesc *val = nullptr;

  uint16_t   msgCap      = 0;
  uint16_t   sigCap      = 0;
  uint16_t   valCap      = 0;

  uint16_t   msgCount    = 0;
  uint16_t   sigCount    = 0;
  uint16_t   valCount    = 0;

  char       version[DBC_VERSION_MAX] = {};

  uint16_t   lineErrors  = 0; /* lines that looked like ours but did not parse */
  uint16_t   nameClipped = 0; /* names too long for DBC_NAME_MAX - see above   */
  uint8_t    overflow    = 0; /* a table filled up - the map is INCOMPLETE     */
  uint8_t    loaded      = 0; /* at least one message was parsed               */
  uint8_t    inexact     = 0; /* at least one signal needs floating point      */
};

extern DbcDb g_dbc;

/* Empties the counters. Keeps whatever tables are allocated, so the usual
 * sequence is dbcAllocate() once and dbcReset() before each re-read. */
void dbcReset(DbcDb &db);

/* Sizes the tables for a file with this many of each, releasing whatever was
 * there before. Sizes are clamped to the DBC_MAX_* ceilings in config.h, which
 * exist to stop a corrupt file asking for a gigabyte.
 *
 * Returns false when the heap could not provide the full request; in that case
 * it retries progressively smaller and leaves the caps at whatever it managed,
 * so the map is truncated rather than absent. Always leaves the tables in a
 * usable state, possibly with every cap at zero. */
bool dbcAllocate(DbcDb &db, const DbcCounts &want);
void dbcFree(DbcDb &db);

/* Bytes the tables currently occupy. Reported at boot, because "the map did
 * not fit" is only actionable next to what it cost. */
size_t dbcBytes(const DbcDb &db);

/* Tallies one line without storing anything, for the counting pass. Counts
 * generously - over-counting wastes a few bytes, under-counting truncates. */
void dbcCountLine(const char *line, DbcCounts &c);

/* Counts, sizes and parses a whole frame map already in memory: the same
 * count-then-allocate-then-parse sequence recorderLoadDbc() does over a file,
 * for callers that hold the text. Returns the number of lines that failed.
 *
 * `text` is not modified - each line is copied into a scratch buffer first,
 * because dbcParseLine() tokenises in place. */
uint16_t dbcLoadText(DbcDb &db, const char *text, size_t len);

/* Feeds one line. The buffer is written to (it is tokenised in place), so pass
 * a mutable copy. Returns false only when the line was recognised as a DBC
 * construct and then failed to parse; unknown constructs are silently skipped,
 * which is what lets a real-world DBC full of CM_/BA_DEF_/NS_ noise through. */
bool dbcParseLine(DbcDb &db, char *line);

/* Name of the node that transmits this message, or "" when the file does not
 * say. Never returns nullptr, so callers can print it unconditionally. */
const char *dbcTxNode(const DbcDb &db, const DbcMessage &m);

/* Clears per-recording decode state (the wrap counters). The map itself is
 * untouched. */
void dbcResetRuntime(DbcDb &db);

/* Message lookup by identifier. Returns nullptr when the id is not mapped. */
const DbcMessage *dbcFind(const DbcDb &db, uint32_t id, bool ext);

struct DbcValue {
  bool        ok;        /* false = the signal does not fit this payload     */
  bool        exact;     /* true  = `scaled`/`dec` is the exact value        */
  int64_t     scaled;    /* physical * 10^dec                                */
  uint8_t     dec;
  double      fval;      /* only meaningful when !exact                      */
  const char *label;     /* value-table text, or nullptr                     */
};

/* Decodes one signal out of a payload. `s` is non-const because a signal
 * marked `Unwrap` carries its own wrap counter. */
DbcValue dbcDecodeSignal(const DbcDb &db, DbcSignal &s,
                         const uint8_t *data, uint8_t len);

/* Raw bit extraction, exposed for tests. */
uint64_t dbcExtractBits(const uint8_t *data, uint8_t len,
                        uint8_t startBit, uint8_t bits, bool intel, bool *fits);

/* ---------------------------------------------------------------------------
 *  ENCODING - the exact inverse of the four functions above
 *
 *  Only needed to transmit. It is written as the mirror image of the decode
 *  path on purpose: the same bit walk, the same integer scaling, so that
 *  encode(decode(x)) == x for every signal the logger can read. test_encode.cpp
 *  asserts exactly that over the whole example DBC, because a transmit path
 *  that is subtly not the inverse of the receive path is a bug that only shows
 *  up as a machine doing the wrong thing.
 * -------------------------------------------------------------------------*/

/* Writes `raw` into the payload at the signal's position, leaving every other
 * bit alone. False when the signal does not fit `len` bytes. */
bool dbcInsertBits(uint8_t *data, uint8_t len,
                   uint8_t startBit, uint8_t bits, bool intel, uint64_t raw);

/* The physical range the signal's bit width can actually represent, which is
 * what a value must be clamped to regardless of what the DBC's [min|max] says.
 * A 12-bit unsigned signal at factor 0.5 cannot carry 4000 however the file is
 * annotated. */
void dbcSignalLimits(const DbcSignal &s, double *lo, double *hi);

struct DbcEncoded {
  bool    ok;          /* false = the signal does not fit the payload        */
  bool    clamped;     /* the value was outside what the bits can hold       */
  int64_t raw;         /* what actually went on the wire                     */
  double  applied;     /* the physical value that raw represents             */
};

/* Places one physical value into a payload. `data` must already hold whatever
 * the rest of the message should carry - this only touches the signal's bits. */
DbcEncoded dbcEncodeSignal(const DbcSignal &s, double phys,
                           uint8_t *data, uint8_t len);

/* Resolves "MessageName.SignalName" to a signal index, and to its message
 * through `msgOut` when given. Returns -1 when either name is unknown, which
 * is the normal state of affairs after the DBC on the card has been changed
 * and a saved dashboard still refers to the old one. */
int16_t dbcFindSignalRef(const DbcDb &db, const char *ref, int16_t *msgOut);

/* Writes "MessageName.SignalName" for signal index `si`. Returns false when
 * the index is not part of any message. */
bool dbcSignalRef(const DbcDb &db, uint16_t si, char *out, size_t cap);
