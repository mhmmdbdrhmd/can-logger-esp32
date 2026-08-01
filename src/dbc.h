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
 *    BO_ <id> <Name>: <dlc> <Node>       message; id | 0x80000000 = 29-bit
 *    SG_ <Name> [mux] : <start>|<len>@<order><sign> (<fac>,<off>)
 *                       [<min>|<max>] "<unit>" <receivers>
 *    VAL_ <id> <Signal> <n> "label" ... ;   enumerated values
 *    SIG_VALTYPE_ <id> <Signal> : 1;        1 = IEEE float, 2 = double
 *    BA_ "Unwrap" SG_ <id> <Signal> 1;      see below
 *    CM_ / BU_ / NS_ / BS_ / BA_DEF_ ...    parsed past, ignored
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

#define DBC_NAME_MAX    24
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
};

struct DbcValDesc {
  uint16_t sig;
  int32_t  value;
  char     label[DBC_LABEL_MAX];
};

struct DbcDb {
  DbcMessage msg[DBC_MAX_MESSAGES];
  DbcSignal  sig[DBC_MAX_SIGNALS];
  DbcValDesc val[DBC_MAX_VALDESC];

  uint16_t   msgCount;
  uint16_t   sigCount;
  uint16_t   valCount;

  char       version[DBC_VERSION_MAX];

  uint16_t   lineErrors;   /* lines that looked like ours but did not parse  */
  uint8_t    overflow;     /* a table filled up - the map is INCOMPLETE      */
  uint8_t    loaded;       /* at least one message was parsed                */
  uint8_t    inexact;      /* at least one signal needs floating point       */
};

extern DbcDb g_dbc;

/* Empties the database. Always call before feeding a file. */
void dbcReset(DbcDb &db);

/* Feeds one line. The buffer is written to (it is tokenised in place), so pass
 * a mutable copy. Returns false only when the line was recognised as a DBC
 * construct and then failed to parse; unknown constructs are silently skipped,
 * which is what lets a real-world DBC full of CM_/BA_DEF_/NS_ noise through. */
bool dbcParseLine(DbcDb &db, char *line);

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
