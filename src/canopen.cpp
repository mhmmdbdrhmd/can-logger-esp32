#include "canopen.h"
#include <string.h>
#include <stdio.h>

/* CiA 301 function codes, bits 7..10 of the COB-ID. */
#define FC_NMT      0x0
#define FC_SYNC     0x1     /* node 0 = SYNC, otherwise EMCY from that node   */
#define FC_TIME     0x2
#define FC_TPDO1    0x3
#define FC_RPDO1    0x4
#define FC_TPDO2    0x5
#define FC_RPDO2    0x6
#define FC_TPDO3    0x7
#define FC_RPDO3    0x8
#define FC_TPDO4    0x9
#define FC_RPDO4    0xA
#define FC_SDO_TX   0xB     /* server -> client                               */
#define FC_SDO_RX   0xC     /* client -> server                               */
#define FC_HEARTBEAT 0xE    /* NMT error control                              */

static const char *functionName(uint8_t fc) {
  switch (fc) {
    case FC_TPDO1: return "TPDO1";
    case FC_RPDO1: return "RPDO1";
    case FC_TPDO2: return "TPDO2";
    case FC_RPDO2: return "RPDO2";
    case FC_TPDO3: return "TPDO3";
    case FC_RPDO3: return "RPDO3";
    case FC_TPDO4: return "TPDO4";
    case FC_RPDO4: return "RPDO4";
    case FC_SDO_TX: return "SDO_TX";
    case FC_SDO_RX: return "SDO_RX";
    case FC_HEARTBEAT: return "HEARTBEAT";
    default: return nullptr;
  }
}

bool canopenName(uint32_t id, bool ext, char *out, size_t cap) {
  if (!cap) return false;
  out[0] = '\0';
  if (ext || id > 0x7FFu) return false;     /* CANopen COB-IDs are 11-bit    */

  const uint8_t fc   = (uint8_t)((id >> 7) & 0xFu);
  const uint8_t node = (uint8_t)(id & 0x7Fu);

  if (fc == FC_NMT) {
    if (node != 0) return false;            /* 0x001..0x07F is reserved      */
    snprintf(out, cap, "NMT");
    return true;
  }
  if (fc == FC_SYNC) {
    if (node == 0) snprintf(out, cap, "SYNC");
    else           snprintf(out, cap, "EMCY.%u", (unsigned)node);
    return true;
  }
  if (fc == FC_TIME) {
    if (node != 0) return false;
    snprintf(out, cap, "TIME");
    return true;
  }
  if (id == 0x7E4u || id == 0x7E5u) {
    snprintf(out, cap, "LSS");
    return true;
  }

  const char *fn = functionName(fc);
  if (!fn || node == 0) return false;       /* node 0 is not a valid address */

  snprintf(out, cap, "%s.%u", fn, (unsigned)node);
  return true;
}

/* ------------------------------------------------------------------------ */
static const char *nmtCommand(uint8_t cs) {
  switch (cs) {
    case 0x01: return "START";
    case 0x02: return "STOP";
    case 0x80: return "PRE-OPERATIONAL";
    case 0x81: return "RESET-NODE";
    case 0x82: return "RESET-COMMUNICATION";
    default:   return nullptr;
  }
}

static const char *nmtState(uint8_t st) {
  switch (st & 0x7Fu) {
    case 0x00: return "BOOT-UP";
    case 0x04: return "STOPPED";
    case 0x05: return "OPERATIONAL";
    case 0x7F: return "PRE-OPERATIONAL";
    default:   return nullptr;
  }
}

/* CiA 301 SDO command specifiers. The top three bits mean different things
 * depending on which way the frame is going, which is why the direction (taken
 * from the COB-ID, not from the payload) has to be passed in. */
static const char *sdoCommand(uint8_t b0, bool fromServer) {
  switch (b0 >> 5) {
    case 0: return fromServer ? "UPLOAD-SEGMENT-RESP"   : "DOWNLOAD-SEGMENT";
    case 1: return fromServer ? "DOWNLOAD-SEGMENT-RESP" : "DOWNLOAD-INIT";
    case 2: return fromServer ? "UPLOAD-INIT-RESP"      : "UPLOAD-INIT";
    case 3: return fromServer ? "DOWNLOAD-INIT-RESP"    : "UPLOAD-SEGMENT";
    case 4: return "ABORT";
    case 5: return fromServer ? "BLOCK-DOWNLOAD-RESP"   : "BLOCK-UPLOAD";
    case 6: return fromServer ? "BLOCK-UPLOAD-RESP"     : "BLOCK-DOWNLOAD";
    default: return nullptr;
  }
}

static inline uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t canopenFields(uint32_t id, const uint8_t *data, uint8_t len,
                      CanopenField *out, uint8_t maxFields) {
  if (!out || !maxFields || id > 0x7FFu) return 0;

  const uint8_t fc   = (uint8_t)((id >> 7) & 0xFu);
  const uint8_t node = (uint8_t)(id & 0x7Fu);
  uint8_t n = 0;

  #define PUSH(nm, lbl, val, ishex, un)                          \
    do {                                                         \
      if (n < maxFields) {                                       \
        out[n].name = (nm); out[n].label = (lbl);                \
        out[n].value = (val); out[n].hex = (ishex);              \
        out[n].unit = (un); n++;                                 \
      }                                                          \
    } while (0)

  if (fc == FC_NMT && node == 0 && len >= 2) {
    PUSH("command", nmtCommand(data[0]), data[0], 1, "");
    PUSH("target",  nullptr,             data[1], 0, "node");
    return n;
  }

  if (fc == FC_SYNC && node == 0) {
    if (len >= 1) PUSH("counter", nullptr, data[0], 0, "");
    return n;
  }

  if (fc == FC_SYNC && node != 0 && len >= 3) {         /* EMCY */
    PUSH("error_code",     nullptr, (uint32_t)data[0] | ((uint32_t)data[1] << 8), 1, "");
    PUSH("error_register", nullptr, data[2], 1, "");
    return n;
  }

  if (fc == FC_HEARTBEAT && node != 0 && len >= 1) {
    PUSH("state", nmtState(data[0]), data[0], 1, "");
    return n;
  }

  if ((fc == FC_SDO_TX || fc == FC_SDO_RX) && node != 0 && len >= 4) {
    const bool fromServer = (fc == FC_SDO_TX);
    PUSH("cs",    sdoCommand(data[0], fromServer), data[0], 1, "");
    PUSH("index", nullptr, (uint32_t)data[1] | ((uint32_t)data[2] << 8), 1, "");
    PUSH("sub",   nullptr, data[3], 0, "");
    if (data[0] == 0x80 && len >= 8) {
      PUSH("abort_code", nullptr, le32(&data[4]), 1, "");
    }
    return n;
  }

  #undef PUSH
  return 0;         /* PDOs: map them in the DBC, the payload is theirs */
}
