/* The CANopen framing layer: COB-ID -> function and node, plus the frames the
 * standard itself defines. No node map is involved anywhere. */
#include "canopen.h"
#include <string>

uint32_t   g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;

static int failures = 0;

static void eq(const char *what, const std::string &got, const std::string &want) {
  if (got == want) printf("  ok   %-32s %s\n", what, got.c_str());
  else { printf("  FAIL %-32s got '%s'  want '%s'\n", what, got.c_str(), want.c_str());
         failures++; }
}
static void ck(const char *what, bool ok, const std::string &d = "") {
  if (ok) printf("  ok   %-32s %s\n", what, d.c_str());
  else  { printf("  FAIL %-32s %s\n", what, d.c_str()); failures++; }
}

static std::string name(uint32_t id, bool ext = false) {
  char b[CANOPEN_NAME_MAX];
  return canopenName(id, ext, b, sizeof(b)) ? std::string(b) : std::string("<none>");
}

/* Renders the decoded fields the way the CSV writer does. */
static std::string fields(uint32_t id, std::initializer_list<uint8_t> bytes) {
  uint8_t d[8] = {0};
  uint8_t n = 0;
  for (uint8_t v : bytes) d[n++] = v;

  CanopenField f[CANOPEN_MAX_FIELDS];
  const uint8_t k = canopenFields(id, d, n, f, CANOPEN_MAX_FIELDS);

  std::string out;
  for (uint8_t i = 0; i < k; i++) {
    char buf[64];
    if (f[i].label)   snprintf(buf, sizeof(buf), "%s=%s", f[i].name, f[i].label);
    else if (f[i].hex) snprintf(buf, sizeof(buf), "%s=0x%X", f[i].name, f[i].value);
    else               snprintf(buf, sizeof(buf), "%s=%u", f[i].name, f[i].value);
    if (i) out += ' ';
    out += buf;
  }
  return out;
}

int main() {
  printf("\n== the COB-ID carries the function and the node ==\n");
  eq("NMT",              name(0x000), "NMT");
  eq("SYNC",             name(0x080), "SYNC");
  eq("TIME",             name(0x100), "TIME");
  eq("emergency, node 5",name(0x085), "EMCY.5");
  eq("TPDO1, node 12",   name(0x18C), "TPDO1.12");
  eq("RPDO1, node 12",   name(0x20C), "RPDO1.12");
  eq("TPDO4, node 127",  name(0x4FF), "TPDO4.127");
  eq("SDO server->client",name(0x585), "SDO_TX.5");
  eq("SDO client->server",name(0x605), "SDO_RX.5");
  eq("heartbeat, node 3",name(0x703), "HEARTBEAT.3");
  eq("LSS",              name(0x7E5), "LSS");

  printf("\n== identifiers that are not CANopen ==\n");
  eq("reserved 0x001",   name(0x001), "<none>");
  eq("node 0 PDO",       name(0x180), "<none>");
  eq("29-bit id",        name(0x18FF5000, true), "<none>");
  eq("out of range",     name(0x800), "<none>");

  printf("\n== frames the standard defines ==\n");
  eq("heartbeat state",  fields(0x703, {0x05}), "state=OPERATIONAL");
  eq("heartbeat boot",   fields(0x703, {0x00}), "state=BOOT-UP");
  eq("heartbeat pre-op", fields(0x703, {0x7F}), "state=PRE-OPERATIONAL");
  eq("heartbeat unknown",fields(0x703, {0x33}), "state=0x33");

  eq("NMT start all",    fields(0x000, {0x01, 0x00}), "command=START target=0");
  eq("NMT reset node 7", fields(0x000, {0x81, 0x07}), "command=RESET-NODE target=7");

  eq("emergency",        fields(0x085, {0x30, 0x81, 0x21}),
     "error_code=0x8130 error_register=0x21");

  eq("SDO read request", fields(0x605, {0x40, 0x18, 0x10, 0x01}),
     "cs=UPLOAD-INIT index=0x1018 sub=1");
  eq("SDO read response",fields(0x585, {0x43, 0x18, 0x10, 0x01, 1, 2, 3, 4}),
     "cs=UPLOAD-INIT-RESP index=0x1018 sub=1");
  eq("SDO abort",        fields(0x605, {0x80, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x06}),
     "cs=ABORT index=0x1000 sub=0 abort_code=0x6020000");

  printf("\n== a PDO payload is never guessed at ==\n");
  {
    uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CanopenField f[CANOPEN_MAX_FIELDS];
    ck("TPDO1 yields no fields",
       canopenFields(0x18C, d, 8, f, CANOPEN_MAX_FIELDS) == 0);
    ck("but it still has a name", name(0x18C) == "TPDO1.12");
  }

  printf("\n== truncated frames do not produce nonsense ==\n");
  {
    uint8_t d[8] = {0};
    CanopenField f[CANOPEN_MAX_FIELDS];
    ck("zero-length heartbeat", canopenFields(0x703, d, 0, f, CANOPEN_MAX_FIELDS) == 0);
    ck("1-byte NMT",            canopenFields(0x000, d, 1, f, CANOPEN_MAX_FIELDS) == 0);
    ck("3-byte SDO",            canopenFields(0x605, d, 3, f, CANOPEN_MAX_FIELDS) == 0);
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
