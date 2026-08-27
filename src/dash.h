/* ============================================================================
 *  dash.h - the operator's own dashboard, and the values they can send back
 *
 *  Everything the logger displays already comes from the DBC on the card. This
 *  is the layer that lets someone say HOW they want it displayed: a grid of
 *  cells, each one naming a signal and a way of drawing it, plus a list of
 *  values they can write back to the bus.
 *
 *  WHY THIS IS DATA AND NOT CODE
 *  -----------------------------
 *  The same reason the frame map is. A speedometer for a 0-50 km/h machine, a
 *  centre-zero dial for a +/-45 degree steering angle and a tank gauge for
 *  hydraulic oil are three arrangements of the same firmware, not three builds
 *  of it. Nothing here names a signal, a unit or a range; the config does.
 *
 *  WHERE IT LIVES
 *  --------------
 *  Two places, with one rule. NVS - the ESP32's own flash - is the live copy,
 *  so a layout survives a card swap or a reformat. /dash.cfg on the card is the
 *  same thing as text, so it can be edited in any editor, copied between
 *  loggers, and kept in version control.
 *
 *  At boot the card wins IF the file changed: dashStore remembers a hash of the
 *  text it last agreed with, and re-imports only when what is on the card no
 *  longer matches it. Saving from the browser writes both. The effect is the
 *  one people expect - whichever you edited last is the one you get - without
 *  either copy silently overwriting the other on every boot.
 *
 *  THE FORMAT
 *  ----------
 *      grid <cols> <rows>
 *      poll <ms>
 *      cell <slot> widget=gauge sig=Drive.Speed lo=0 hi=50 label="Ground speed"
 *      send <index> label="Tyre size" sig=WheelInfo.TireSize lo=400 hi=1200
 *      send <index> label="Reset trip" id=0x600 data=2F10200001000000
 *
 *  Key=value, order-independent, values quoted when they contain spaces, and
 *  unknown keys are skipped rather than rejected - the same tolerance the DBC
 *  parser has, and for the same reason: a config written by a later version
 *  should degrade, not fail.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "config.h"
#include "dbc.h"

#define DASH_REF_MAX    (DBC_NAME_MAX * 2)   /* "Message.Signal" plus the nul */
#define DASH_LABEL_MAX  24
#define DASH_UNIT_MAX   DBC_UNIT_MAX
#define DASH_LINE_MAX   200

/* How a cell draws its signal.
 *
 * The set is chosen to cover the shapes a machine bus actually produces rather
 * than to be exhaustive: something that sweeps up from zero, something that
 * sweeps either side of zero, something that fills, something that is just a
 * number, and something that is a named state. Everything on a vehicle,
 * hydraulic rig or test bench is one of those five wearing different clothes. */
enum DashWidget : uint8_t {
  DW_GAUGE = 0,   /* 240-degree dial with a needle: speed, rpm, flow          */
  DW_ARC,         /* compact half dial: pressure, load, duty                  */
  DW_ANGLE,       /* centre-zero, sweeps both ways: steering, tilt, roll      */
  DW_COMPASS,     /* full circle: heading, yaw, crank position                */
  DW_BAR,         /* horizontal bar: percentages and generic ranges           */
  DW_LEVEL,       /* vertical tank: fuel, oil, hydraulic level                */
  DW_THERMO,      /* thermometer: coolant, oil and ambient temperature        */
  DW_NUMBER,      /* large numeric readout: counters, anything unbounded      */
  DW_SPARK,       /* number plus a rolling trace: trends and drift            */
  DW_STATE,       /* coloured pill showing a VAL_ label: gear, mode, faults   */
  DW_COUNT
};

/* Cell flags. warn/crit are optional, so their presence is a flag rather than
 * a sentinel value - a NaN that has to survive a text round trip is a bug
 * waiting to happen. */
#define DF_HAS_WARN  0x01
#define DF_HAS_CRIT  0x02
#define DF_LOW_BAD   0x04   /* the LOW end is the dangerous one (oil level)  */

struct DashCell {
  uint8_t widget;
  uint8_t flags;
  uint8_t dec;                     /* decimal places, 255 = follow the signal */

  /* The signal is stored by name, not by index. An index is smaller and would
   * quietly point at a different signal the first time a message is added to
   * the DBC; the name is the identity and the index is resolved at load. */
  char    ref[DASH_REF_MAX];
  int16_t sig;                     /* resolved index, -1 = not in this DBC   */

  char    label[DASH_LABEL_MAX];   /* empty = use the signal's own name      */
  char    unit[DASH_UNIT_MAX];     /* empty = use the DBC unit               */

  float   lo, hi;
  float   warn, crit;
};

static inline bool dashCellUsed(const DashCell &c) { return c.ref[0] != '\0'; }

/* ---------------------------------------------------------------------------
 *  Values that can be written back to the bus.
 * -------------------------------------------------------------------------*/
#define TXK_SIGNAL 0    /* encode one DBC signal into its message            */
#define TXK_RAW    1    /* a fixed frame, byte for byte                      */

/* How the value is asked for on the machine. This is the whole point of
 * setting these up beforehand: a tyre size is not a number somebody should be
 * typing while standing next to a running machine, it is one of four sizes the
 * fleet actually uses. Getting the input right is what turns a text box into a
 * decision that cannot be fat-fingered. */
#define TXI_NUMBER 0    /* type it. The fallback, not the default.           */
#define TXI_SLIDER 1    /* drag between the limits, with the number shown    */
#define TXI_TOGGLE 2    /* two states, each with its own label               */
#define TXI_ENUM   3    /* the DBC's own VAL_ labels, as a list              */
#define TXI_CHOICE 4    /* a list written by whoever set this up             */

struct TxCommand {
  uint8_t  kind;
  uint8_t  style;                 /* TXI_*                                  */
  char     label[DASH_LABEL_MAX];

  /* The list behind TXI_CHOICE and TXI_TOGGLE, as "value:label|value:label".
   * Kept as the text it was written in rather than parsed into a table: the
   * firmware never needs to understand it - it only has to store it, serve it
   * and write it back unchanged - and a list of four tyre sizes does not
   * justify a parser and an array on a microcontroller. */
  char     choices[TX_CHOICES_MAX];

  /* TXK_SIGNAL */
  char     ref[DASH_REF_MAX];
  int16_t  sig;
  int16_t  msg;
  char     unit[DASH_UNIT_MAX];
  float    lo, hi, step, preset;
  uint8_t  dec;

  /* Values sharing a non-zero group leave in ONE frame, with a single Send
   * button. That is what a message whose signals are only meaningful together
   * needs - and a multiplexed payload always needs it, though there the
   * selector is written automatically and does not appear here. All members of
   * a group must target the same message; the customiser only ever forms them
   * that way. */
  uint8_t  group;

  /* TXK_RAW */
  uint32_t id;
  uint8_t  ext;
  uint8_t  len;
  uint8_t  data[8];

  /* 0 = send once. Otherwise the frame is repeated at this period for as long
   * as it stays enabled, which is what parameter servers on a machine bus
   * generally want - a value written once and never refreshed is treated as
   * stale by plenty of ECUs. */
  uint16_t cyclicMs;
};

static inline bool txCommandUsed(const TxCommand &c) { return c.label[0] != '\0'; }

/* ---------------------------------------------------------------------------
 *  The whole saved configuration.
 * -------------------------------------------------------------------------*/
struct DashConfig {
  uint8_t   cols;
  uint8_t   rows;
  uint16_t  pollMs;      /* how often the browser asks for values            */

  /* Which node in the frame map this logger stands in for, or "" for none.
   * Purely an authoring aid: with it set, the customiser knows that messages
   * this node transmits are things to SEND and everything else is things to
   * WATCH, and stops offering each list to the wrong screen. The firmware
   * itself never filters on it - a frame that arrives is recorded whoever the
   * file says sends it. */
  char      node[DASH_NODE_MAX];

  DashCell  cell[DASH_MAX_CELLS];
  TxCommand tx[TX_MAX_COMMANDS];
};

/* The live configuration.
 *
 * A REFERENCE to a heap block, not a static object. It is nearly six kilobytes
 * and the ESP32's static data segment is the tightest thing in this build - the
 * first version of the dashboard overflowed `dram0_0_seg` outright. The heap is
 * the rest of the same DRAM and has room to spare, so putting it there is what
 * makes room for a useful number of cells and sendable values rather than a
 * token few. Allocated once at start-up and never freed. */
extern DashConfig &g_dash;

/* Empties the configuration to a sane, empty grid. Always call before feeding
 * a file. */
void dashReset(DashConfig &c);

/* Number of cells the current grid actually shows. */
static inline uint8_t dashCellCount(const DashConfig &c) {
  const uint16_t n = (uint16_t)c.cols * c.rows;
  return (uint8_t)(n > DASH_MAX_CELLS ? DASH_MAX_CELLS : n);
}

/* Feeds one line. The buffer is tokenised in place, so pass a mutable copy.
 * Returns false only when a line was recognised and then failed to parse. */
bool dashParseLine(DashConfig &c, char *line);

/* Parses a whole config. Convenience over dashParseLine for a buffer already
 * in RAM - the boot path streams the file line by line instead. Returns the
 * number of lines that failed. */
uint16_t dashParse(DashConfig &c, const char *text, size_t len);

/* Writes the configuration back out. The output is stable - same config, same
 * bytes - because the boot rule compares a hash of it against the card. */
size_t dashSerialize(const DashConfig &c, char *out, size_t cap);

/* Resolves every "Message.Signal" against the frame map, filling in the cached
 * indices. Call after the DBC is loaded and after any config change. Returns
 * how many references could not be found, which the dashboard shows rather
 * than hiding - a cell pointing at a signal this DBC does not have is the
 * normal result of swapping cards, and the user needs to see it. */
uint16_t dashResolve(DashConfig &c, const DbcDb &db);

/* FNV-1a over the serialised text. Used only to notice that /dash.cfg was
 * edited outside the logger; it is not a checksum against corruption. */
uint32_t dashHash(const char *text, size_t len);

const char *dashWidgetName(uint8_t w);
int8_t      dashWidgetId(const char *name);   /* -1 when unknown */

const char *dashInputName(uint8_t style);
int8_t      dashInputId(const char *name);    /* -1 when unknown */
