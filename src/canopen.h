/* ============================================================================
 *  canopen.h - optional CANopen framing, with no node map
 *
 *  On a CANopen network the identifier is not opaque: CiA 301 splits the 11-bit
 *  COB-ID into a 4-bit function code and a 7-bit node ID, so 0x18C is "the
 *  first transmit PDO of node 12" on every conforming network in existence.
 *  That much can be decoded without knowing anything about the devices, which
 *  is why it belongs in the firmware while the signal layouts do not.
 *
 *  What this layer does NOT do is guess at PDO contents. The bytes inside a PDO
 *  are whatever the node's mapping objects say they are, and inventing signal
 *  names for them would be worse than useless. PDOs get a name and their raw
 *  payload; map them in the DBC if you want them decoded.
 *
 *  The DBC always wins: this is only consulted for identifiers the frame map
 *  does not describe. Disable it with CANOPEN_DECODE 0 on a plain CAN bus,
 *  where CANopen names would be actively misleading.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

#define CANOPEN_NAME_MAX 24
#define CANOPEN_MAX_FIELDS 4

struct CanopenField {
  const char *name;    /* signal name for the CSV                            */
  const char *label;   /* symbolic value; when set, `value` is not printed    */
  uint32_t    value;
  uint8_t     hex;     /* 1 = print `value` as 0x...                          */
  const char *unit;
};

/* Writes a name such as "TPDO1.12", "HEARTBEAT.3", "SDO_RX.5" or "SYNC" into
 * `out`. Returns false for identifiers that are not CANopen-shaped (29-bit
 * identifiers, and 11-bit ones in the reserved ranges), leaving `out` empty. */
bool canopenName(uint32_t id, bool ext, char *out, size_t cap);

/* Decodes the frames whose contents are defined by the standard itself:
 * NMT commands, heartbeats, emergencies and SDO transfers. Returns the number
 * of fields written, which is 0 for PDOs and anything unrecognised. */
uint8_t canopenFields(uint32_t id, const uint8_t *data, uint8_t len,
                      CanopenField *out, uint8_t maxFields);
