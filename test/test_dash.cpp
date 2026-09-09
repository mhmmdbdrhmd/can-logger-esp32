/* Host-side exercise of the saved dashboard configuration.
 *
 * The property that matters is that a configuration survives being written and
 * read back unchanged, byte for byte. It has to, because the rule that decides
 * whether /dash.cfg on the card or the copy in NVS wins at boot compares a hash
 * of the serialised text: if serialising the same configuration twice produced
 * two different strings, the logger would decide the card had been edited every
 * single boot and quietly throw away whatever the browser had saved.
 *
 * The rest is tolerance. A config file is something people edit by hand, and
 * one written by a later firmware has to degrade rather than fail. */
#include "dash.h"
#include <string>
#include <vector>

uint32_t   g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;

static int failures = 0;

static void ck(const char *what, bool ok, const std::string &detail = "") {
  if (ok) printf("  ok   %-44s %s\n", what, detail.c_str());
  else  { printf("  FAIL %-44s %s\n", what, detail.c_str()); failures++; }
}

static void feed(DashConfig &c, const char *text) {
  dashParse(c, text, strlen(text));
}

static std::string dump(const DashConfig &c) {
  std::vector<char> buf(DASH_CFG_MAX);
  const size_t n = dashSerialize(c, buf.data(), buf.size());
  return std::string(buf.data(), n);
}

static const char *const MAP[] = {
  "BO_ 396 WheelInfo: 8 Vehicle",
  " SG_ TireSize : 0|16@1+ (1,0) [400|1200] \"mm\" ECU",
  " SG_ Pressure : 16|10@1+ (0.1,0) [0|102.3] \"bar\" ECU",
  "BO_ 512 Steering: 8 Vehicle",
  " SG_ Angle : 7|16@0- (0.01,0) [-45|45] \"deg\" ECU",
  "BO_ 700 Drive: 8 Vehicle",
  " SG_ Speed : 0|12@1+ (0.0625,0) [0|50] \"km/h\" ECU",
  " SG_ Gear : 12|4@1+ (1,0) \"\" ECU",
  nullptr
};

static void loadMap(DbcDb &db) {
  std::string text;
  for (const char *const *l = MAP; *l; l++) { text += *l; text += '\n'; }
  dbcLoadText(db, text.c_str(), text.size());
}

/* True when any non-comment, non-blank line of a serialised layout contains
 * `needle`. The file opens with a '#' legend that names every key it supports,
 * so a plain substring search over the whole text would find the documentation
 * rather than the data. */
static bool dataLinesMention(const char *text, size_t len, const char *needle) {
  const std::string all(text, len);
  size_t start = 0;
  while (start < all.size()) {
    size_t nl = all.find('\n', start);
    if (nl == std::string::npos) nl = all.size();
    const std::string line = all.substr(start, nl - start);
    start = nl + 1;
    if (line.empty() || line[0] == '#') continue;
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

/* dashResolve() and dashDropUnresolved() take one map PER BUS. These tests are
 * about resolution rather than about routing, so unless a case says otherwise
 * both buses get the same map - which also proves a single-bus layout still
 * resolves exactly as it did. */
static const DbcDb *both(const DbcDb &a) {
  static DbcDb pair[CAN_BUSES];
  pair[0] = a;
  pair[1] = a;
  return pair;
}

int main() {
  printf("== an empty configuration ==\n");
  {
    DashConfig c;
    dashReset(c);
    ck("starts with a usable grid", c.cols >= 1 && c.rows >= 1,
       std::to_string(c.cols) + "x" + std::to_string(c.rows));
    ck("no cells are in use", !dashCellUsed(c.cell[0]));
    ck("cell count follows the grid",
       dashCellCount(c) == (uint8_t)(c.cols * c.rows));
    ck("it serialises to something that parses back",
       dump(c).find("grid ") != std::string::npos);
  }

  printf("\n== a hand-written file ==\n");
  {
    DashConfig c;
    dashReset(c);
    feed(c,
      "# a comment, and a blank line follow\n"
      "\n"
      "version 1\n"
      "grid 3 2\n"
      "poll 250\n"
      "cell 0 widget=gauge sig=Drive.Speed lo=0 hi=50 dec=1 label=\"Ground speed\" unit=km/h warn=35 crit=45\n"
      "cell 1 widget=angle sig=Steering.Angle lo=-45 hi=45\n"
      "cell 4 widget=state sig=Drive.Gear label=Gear\n"
      "send 0 label=\"Tyre size\" sig=WheelInfo.TireSize lo=400 hi=1200 step=5 preset=690\n"
      "send 1 label=\"Reset trip\" id=0x600 data=2F10200001000000\n");

    ck("grid read", c.cols == 3 && c.rows == 2,
       std::to_string(c.cols) + "x" + std::to_string(c.rows));
    ck("poll read", c.pollMs == 250, std::to_string(c.pollMs));

    ck("cell 0 widget", c.cell[0].widget == DW_GAUGE);
    ck("cell 0 signal", strcmp(c.cell[0].ref, "Drive.Speed") == 0, c.cell[0].ref);
    ck("a quoted label keeps its space",
       strcmp(c.cell[0].label, "Ground speed") == 0, c.cell[0].label);
    ck("cell 0 range", c.cell[0].lo == 0.0f && c.cell[0].hi == 50.0f);
    ck("warn and crit are flagged as present",
       (c.cell[0].flags & DF_HAS_WARN) && (c.cell[0].flags & DF_HAS_CRIT));
    ck("a cell without them is not",
       (c.cell[1].flags & (DF_HAS_WARN | DF_HAS_CRIT)) == 0);
    ck("a negative range survives",
       c.cell[1].lo == -45.0f && c.cell[1].hi == 45.0f);
    ck("a sparse slot is honoured", dashCellUsed(c.cell[4]) &&
       !dashCellUsed(c.cell[2]) && !dashCellUsed(c.cell[3]));

    ck("a signal setpoint", c.tx[0].kind == TXK_SIGNAL &&
       strcmp(c.tx[0].label, "Tyre size") == 0 && c.tx[0].preset == 690.0f);
    ck("a raw setpoint", c.tx[1].kind == TXK_RAW && c.tx[1].id == 0x600 &&
       c.tx[1].len == 8 && c.tx[1].data[0] == 0x2F && c.tx[1].data[2] == 0x20,
       "len " + std::to_string(c.tx[1].len));
  }

  printf("\n== write, read, write again ==\n");
  {
    /* The round trip the boot rule depends on. */
    DashConfig a;
    dashReset(a);
    feed(a,
      "grid 4 3\n"
      "poll 200\n"
      "role Tester\n"
      "cell 0 widget=gauge sig=Drive.Speed lo=0 hi=50 dec=1 label=\"Ground speed\" unit=km/h warn=35 crit=45\n"
      "cell 3 widget=level sig=WheelInfo.Pressure lo=0 hi=10 lowbad=1\n"
      "cell 7 widget=spark sig=Steering.Angle lo=-45 hi=45 dec=2\n"
      "send 0 label=\"Tyre size\" sig=WheelInfo.TireSize lo=400 hi=1200 step=5 preset=690 unit=mm\n"
      "send 1 label=\"Axle mode\" sig=WheelInfo.Mode lo=0 hi=3 step=1 preset=0 group=2\n"
      "send 2 label=\"Axle load\" sig=WheelInfo.Load lo=0 hi=9000 step=10 preset=0 group=2\n"
      "send 3 label=\"Wake node\" id=0x700 data=00 cyclic=500\n"
      "send 4 label=\"Wheel dia\" sig=Cmd.WheelDia lo=300 hi=3000 mux=1\n"
      "send 5 label=\"Opcode arg\" sig=Cmd.Arg lo=0 hi=255 msel=Cmd_Op mxc=16\n");

    ck("the node this logger is survives a round trip",
       strcmp(a.role[0], "Tester") == 0, a.role[0]);
    /* An untagged role line is CAN1's, and says nothing about CAN2. A shared
       answer is what the two-button header exists to stop: the logger is
       routinely a node on one bus and a listener on the other. */
    ck("and it is CAN1's alone, not both buses'", a.role[1][0] == 0, a.role[1]);

    /* The manual override is carried BY the value, not looked up in the frame
       map, because the page needs it with no map loaded at all - the desk tool
       before a .dbc is chosen, or a logger whose card has none. Looked up, the
       answer becomes "not multiplexed" the moment the map is away, the frame
       stops being a frame, and its payloads become deletable one at a time. So
       both halves have to survive the round trip through the file. */
    ck("the selector named by hand survives a round trip",
       strcmp(a.tx[5].muxSel, "Cmd_Op") == 0, a.tx[5].muxSel);
    ck("and so does the code it travels under", a.tx[5].muxCode == 16,
       std::to_string(a.tx[5].muxCode));
    ck("a value with no override has neither",
       a.tx[0].muxSel[0] == 0 && a.tx[0].muxCode == -1,
       std::to_string(a.tx[0].muxCode));
    /* An older file's mux=1 said "this is one payload of a multiplexed
       message". The message name and the frame map answer that now, so the key
       is read without complaint and never written back. */
    ck("an old file's mux=1 is ignored, not echoed back",
       a.tx[4].muxSel[0] == 0, a.tx[4].muxSel);

    /* This setting was called `node` in an earlier version, and was briefly
       removed altogether. A setup file written by any of those has to keep its
       answer rather than silently coming back as "no role", which reads as a
       working import that then fills the wrong half of the map into the wrong
       screen. Both spellings in, one spelling out. */
    {
      DashConfig legacy;
      dashReset(legacy);
      const char *old = "grid 4 2\nnode Tester\n";
      ck("a setup file written when it was called `node` still sets the role",
         dashParse(legacy, old, strlen(old)) == 0 &&
         strcmp(legacy.role[0], "Tester") == 0, legacy.role[0]);
      /* Anchored to a line start: the header comment this file writes explains
         what the role is and contains the word "node" in that sentence, so an
         unanchored search finds the documentation and calls it a config line. */
      const std::string out = dump(legacy);
      ck("and it is written back out under the current name",
         out.find("\nrole Tester") != std::string::npos &&
         out.find("\nnode ") == std::string::npos, "role, not node");
    }
    /* `group=` is gone. Values of one message are one frame because that is
       what a frame is, so the message name groups them and nothing else has
       to. An old setup file still carries the key; it has to be read without
       complaint and written back out without it, rather than rejected. */
    {
      const std::string out = dump(a);
      ck("an old file's group= is ignored, not echoed back",
         out.find(" group=") == std::string::npos &&
         out.find("WheelInfo.Mode") != std::string::npos &&
         out.find("WheelInfo.Load") != std::string::npos, "dropped, values kept");
    }

    const std::string first = dump(a);

    DashConfig b;
    dashReset(b);
    feed(b, first.c_str());
    const std::string second = dump(b);

    ck("the text is identical the second time round", first == second,
       first == second ? std::to_string(first.size()) + " bytes"
                       : "<-- the boot rule would re-import every boot");
    ck("and so is the hash",
       dashHash(first.data(), first.size()) == dashHash(second.data(), second.size()));

    /* And a third pass, because a format that is stable once but not twice is
     * a format that is not stable. */
    DashConfig d;
    dashReset(d);
    feed(d, second.c_str());
    ck("stable on a third pass", dump(d) == second);

    ck("cells survived", strcmp(b.cell[0].ref, "Drive.Speed") == 0 &&
       strcmp(b.cell[3].ref, "WheelInfo.Pressure") == 0 &&
       strcmp(b.cell[7].ref, "Steering.Angle") == 0);
    ck("lowbad survived", (b.cell[3].flags & DF_LOW_BAD) != 0);
    ck("decimals survived", b.cell[0].dec == 1 && b.cell[7].dec == 2);
    ck("warn and crit survived",
       b.cell[0].warn == 35.0f && b.cell[0].crit == 45.0f);
    ck("the raw setpoint survived", b.tx[3].kind == TXK_RAW &&
       b.tx[3].id == 0x700 && b.tx[3].len == 1 && b.tx[3].cyclicMs == 500);
    ck("the signal setpoint survived", b.tx[0].kind == TXK_SIGNAL &&
       strcmp(b.tx[0].ref, "WheelInfo.TireSize") == 0 &&
       b.tx[0].step == 5.0f && b.tx[0].preset == 690.0f);
  }

  printf("\n== a config written by a later version ==\n");
  {
    DashConfig c;
    dashReset(c);
    feed(c,
      "grid 2 2\n"
      "hologram 3\n"                                   /* unknown keyword */
      "cell 0 widget=gauge sig=Drive.Speed lo=0 hi=50 sparkle=7 glow=blue\n"
      "cell 1 widget=teleporter sig=Drive.Gear\n");    /* unknown widget  */

    ck("an unknown keyword is skipped", c.cols == 2 && c.rows == 2);
    ck("unknown cell keys are skipped",
       dashCellUsed(c.cell[0]) && c.cell[0].hi == 50.0f);
    ck("an unknown widget falls back to a readable one",
       dashCellUsed(c.cell[1]) && c.cell[1].widget == DW_NUMBER);
  }

  printf("\n== nothing overruns ==\n");
  {
    DashConfig c;
    dashReset(c);
    feed(c,
      "grid 99 99\n"                     /* beyond the limits */
      "poll 1\n"                         /* below the floor   */
      "cell 999 widget=gauge sig=A.B\n"  /* beyond the array  */
      "send 999 label=x sig=A.B\n");

    ck("an impossible grid is rejected, not applied",
       c.cols <= DASH_MAX_COLS && c.rows <= DASH_MAX_ROWS,
       std::to_string(c.cols) + "x" + std::to_string(c.rows));
    ck("the poll rate is clamped to the floor", c.pollMs >= DASH_POLL_MIN_MS,
       std::to_string(c.pollMs));

    /* An over-long label and reference must be cut, not written past. */
    DashConfig d;
    dashReset(d);
    /* One over-long field per line: a line longer than DASH_LINE_MAX is cut
     * before the parser sees it, which would make this pass without ever
     * exercising the field truncation it claims to test. */
    std::string refLine = "cell 0 widget=gauge sig=" + std::string(150, 'S') + "\n";
    std::string labLine = "cell 1 widget=gauge sig=A.B label=\"" +
                          std::string(150, 'L') + "\"\n";
    feed(d, refLine.c_str());
    feed(d, labLine.c_str());
    ck("an over-long reference is truncated",
       strlen(d.cell[0].ref) == DASH_REF_MAX - 1,
       std::to_string(strlen(d.cell[0].ref)) + " chars");
    ck("an over-long label is truncated",
       strlen(d.cell[1].label) == DASH_LABEL_MAX - 1,
       std::to_string(strlen(d.cell[1].label)) + " chars");
    ck("and the truncated label is still nul terminated",
       d.cell[1].label[DASH_LABEL_MAX - 1] == '\0');

    /* A buffer too small to hold the output must stop, not run off the end. */
    DashConfig e;
    dashReset(e);
    feed(e, "grid 4 4\ncell 0 widget=gauge sig=Drive.Speed lo=0 hi=50\n");
    char tiny[40];
    memset(tiny, 0x7E, sizeof(tiny));
    const size_t n = dashSerialize(e, tiny, sizeof(tiny) - 1);
    ck("serialising into a short buffer stops short", n < sizeof(tiny) - 1,
       std::to_string(n) + " bytes");
    ck("and does not touch the guard byte", (uint8_t)tiny[sizeof(tiny) - 1] == 0x7E);
  }

  printf("\n== binding to the frame map ==\n");
  {
    DbcDb db;
    loadMap(db);

    DashConfig c;
    dashReset(c);
    feed(c,
      "grid 3 1\n"
      "cell 0 widget=gauge sig=Drive.Speed\n"          /* no range, no unit */
      "cell 1 widget=angle sig=Steering.Angle\n"
      "cell 2 widget=number sig=Ghost.Signal\n"        /* not in this DBC   */
      "send 0 label=Tyre sig=WheelInfo.TireSize\n"
      "send 1 label=Wide sig=WheelInfo.TireSize lo=-9999 hi=99999\n");

    const uint16_t missing = dashResolve(c, both(db));
    ck("one reference could not be resolved", missing == 1,
       std::to_string(missing) + " missing");
    ck("a resolved cell has an index", c.cell[0].sig >= 0);
    ck("an unresolved cell is marked, not dropped",
       c.cell[2].sig < 0 && dashCellUsed(c.cell[2]));

    /* ...but only at boot. Loading a DIFFERENT frame map is a statement that
       this logger is now looking at another bus, and the cells from the old
       one are not temporarily unresolvable, they are wrong. Left in place they
       are a screen of unknowns - and worse, values from a multiplexed message
       stop being recognised as a set the moment their message leaves the map,
       so they become deletable one at a time, which is exactly the
       half-described command the grouping exists to prevent. */
    {
      DashConfig d = c;
      snprintf(d.role[0], sizeof(d.role[0]), "%s", "Vehicle");
      const uint16_t gone = dashDropUnresolved(d, both(db));
      ck("loading a new map drops what it cannot account for", gone == 1,
         std::to_string(gone) + " dropped");
      ck("a role the new map still names is left alone",
         strcmp(d.role[0], "Vehicle") == 0, d.role[0]);
      ck("and closes the gap rather than leaving a hole",
         dashCellUsed(d.cell[0]) && dashCellUsed(d.cell[1]) &&
         !dashCellUsed(d.cell[2]), "two cells, contiguous");
      ck("the survivors are the ones the new map describes",
         strcmp(d.cell[0].ref, "Drive.Speed") == 0 &&
         strcmp(d.cell[1].ref, "Steering.Angle") == 0, d.cell[1].ref);
      ck("setpoints the map still describes are kept",
         txCommandUsed(d.tx[0]) && txCommandUsed(d.tx[1]), "both");

      /* Nothing at all in common: the honest result is an empty setup, not a
         grid of cells that will never resolve. */
      DbcDb other = {};
      const char *unrelated =
        "BO_ 800 ABS_Cmd: 8 Tester\n"
        " SG_ Cmd_Op : 0|8@1+ (1,0) [0|255] \"\" ABS_ECU\n";
      dbcLoadText(other, unrelated, strlen(unrelated));
      DashConfig e = c;
      snprintf(e.role[0], sizeof(e.role[0]), "%s", "Vehicle");
      const uint16_t all = dashDropUnresolved(e, both(other));
      ck("an unrelated frame map clears the setup", all == 6,
         std::to_string(all) + " dropped");
      ck("and leaves nothing behind",
         !dashCellUsed(e.cell[0]) && !txCommandUsed(e.tx[0]), "empty");
      /* The role went with it. Nothing in the new file transmits under that
         name, so keeping it would leave the header asserting a role while both
         Fill buttons quietly stopped separating anything by it. */
      ck("including a role no node of the new map answers to",
         e.role[0][0] == 0, e.role[0]);

      DashConfig f = c;
      snprintf(f.role[0], sizeof(f.role[0]), "%s", "Tester");
      dashDropUnresolved(f, both(other));
      ck("a role the new map DOES name survives it",
         strcmp(f.role[0], "Tester") == 0, f.role[0]);

      /* Each role held to ITS OWN bus's map, which is the whole point of one
         per bus - and this is the case that separates that from the union rule
         it replaced. Both roles below ARE named, but each by the OTHER bus's
         map: "Vehicle" transmits in `db` and is set on the bus carrying
         `other`, and vice versa. Per-bus resolution clears both. A union rule
         keeps both, and leaves each bus's Fill split on a node that does not
         exist there - which is invisible until somebody presses Fill. */
      {
        DashConfig g = c;
        snprintf(g.role[0], sizeof(g.role[0]), "%s", "Vehicle");
        snprintf(g.role[1], sizeof(g.role[1]), "%s", "Tester");
        DbcDb crossed[CAN_BUSES];
        crossed[0] = other;   /* names Tester,  not Vehicle */
        crossed[1] = db;      /* names Vehicle, not Tester  */
        dashDropUnresolved(g, crossed);
        ck("a role named only by the OTHER bus's map is cleared",
           g.role[0][0] == 0 && g.role[1][0] == 0,
           std::string(g.role[0]) + "/" + g.role[1]);

        /* And the same two names, each on the bus that does name them, are
           both left alone - so the assertion above is about the wrong bus and
           not about roles being dropped indiscriminately. */
        DashConfig h = c;
        snprintf(h.role[0], sizeof(h.role[0]), "%s", "Tester");
        snprintf(h.role[1], sizeof(h.role[1]), "%s", "Vehicle");
        dashDropUnresolved(h, crossed);
        ck("and each survives on the bus whose map does name it",
           strcmp(h.role[0], "Tester") == 0 &&
           strcmp(h.role[1], "Vehicle") == 0,
           std::string(h.role[0]) + "/" + h.role[1]);
      }
      dbcFree(other);
    }

    ck("the unit came from the DBC", strcmp(c.cell[0].unit, "km/h") == 0,
       c.cell[0].unit);
    ck("the range came from the DBC's [min|max]",
       c.cell[0].lo == 0.0f && c.cell[0].hi == 50.0f,
       std::to_string(c.cell[0].lo) + ".." + std::to_string(c.cell[0].hi));
    ck("a centre-zero range came through intact",
       c.cell[1].lo == -45.0f && c.cell[1].hi == 45.0f);

    ck("a setpoint picks up its range too",
       c.tx[0].lo == 400.0f && c.tx[0].hi == 1200.0f,
       std::to_string(c.tx[0].lo) + ".." + std::to_string(c.tx[0].hi));

    /* TireSize is 16 bits at factor 1, so it cannot carry 99999 whatever the
     * config says. A slider that goes further than the wire does would aim the
     * operator at a value that is silently clamped on the way out. */
    ck("a setpoint range is cut to what the bits can hold",
       c.tx[1].lo >= 0.0f && c.tx[1].hi <= 65535.0f,
       std::to_string(c.tx[1].lo) + ".." + std::to_string(c.tx[1].hi));
  }

  printf("\n== a signal with no range annotation ==\n");
  {
    DbcDb db;
    loadMap(db);
    DashConfig c;
    dashReset(c);
    feed(c, "grid 1 1\ncell 0 widget=bar sig=Drive.Gear\n");
    dashResolve(c, both(db));

    /* Gear has no [min|max] in the map. Falling back to what four bits can
     * hold gives a bar that is at least drawable, rather than one with a
     * zero-width range that divides by zero in the browser. */
    ck("falls back to what the bits can hold",
       c.cell[0].hi > c.cell[0].lo,
       std::to_string(c.cell[0].lo) + ".." + std::to_string(c.cell[0].hi));
    ck("and that is 0..15 for a 4-bit signal",
       c.cell[0].lo == 0.0f && c.cell[0].hi == 15.0f);
  }

  printf("\n== deleting things ==\n");
  {
    DashConfig c;
    dashReset(c);
    feed(c, "grid 2 1\ncell 0 widget=gauge sig=Drive.Speed lo=0 hi=50\n"
            "send 0 label=Tyre sig=WheelInfo.TireSize\n");
    ck("both are there", dashCellUsed(c.cell[0]) && txCommandUsed(c.tx[0]));

    /* Re-declaring a slot with no signal is how the browser clears one. */
    feed(c, "cell 0 widget=gauge\nsend 0 sig=WheelInfo.TireSize\n");
    ck("a cell with no signal is empty", !dashCellUsed(c.cell[0]));
    ck("a setpoint with no label is empty", !txCommandUsed(c.tx[0]));
    ck("and it leaves nothing behind",
       c.tx[0].sig == -1 && c.tx[0].ref[0] == '\0');

    const std::string out = dump(c);
    ck("neither is written back out",
       out.find("cell 0") == std::string::npos &&
       out.find("send 0") == std::string::npos);
  }

  /* ------------------------------------------------------------------------
   *  The manual multiplex override
   *
   *  For a frame map that multiplexes in fact and does not say so: payloads on
   *  the same bits, an opcode in front of them, and no M or m<code> anywhere.
   *  The override says which signal selects and which code each value travels
   *  under, and BOTH are needed - a selector alone does not say that Cmd_Amp
   *  means opcode 16.
   * ---------------------------------------------------------------------- */
  printf("\n== a message multiplexed by hand ==\n");
  {
    static const char kMap[] =
      "BU_: ABS_ECU Tester\n"
      "BO_ 800 ABS_Cmd: 8 Tester\n"
      " SG_ Cmd_Op : 0|8@1+ (1,0) [0|255] \"\" ABS_ECU\n"
      " SG_ Cmd_Val : 8|8@1+ (1,0) [0|255] \"\" ABS_ECU\n"
      " SG_ Cmd_Amp : 8|32@1- (1e-06,0) [-2147|2147] \"\" ABS_ECU\n"
      "BO_ 288 Declared: 8 Tester\n"
      " SG_ Sel M : 0|8@1+ (1,0) [0|255] \"\" ABS_ECU\n"
      " SG_ Pay m16 : 8|8@1+ (1,0) [0|255] \"\" ABS_ECU\n";

    DbcDb db = {};
    dbcLoadText(db, kMap, sizeof(kMap) - 1);
    ck("the hand-written map parses", db.msgCount == 2, std::to_string(db.msgCount));

    DashConfig c;
    dashReset(c);
    feed(c,
      "send 0 label=Val sig=ABS_Cmd.Cmd_Val lo=0 hi=255 msel=Cmd_Op mxc=1\n"
      "send 1 label=Amp sig=ABS_Cmd.Cmd_Amp lo=-2000 hi=2000 msel=Cmd_Op mxc=16\n"
      "send 2 label=Pay sig=Declared.Pay lo=0 hi=255 msel=Sel mxc=3\n"
      "send 3 label=Gone sig=ABS_Cmd.Cmd_Val lo=0 hi=255 msel=NoSuchSignal mxc=1\n");
    dashResolve(c, both(db));

    ck("the selector named by hand resolves to a signal of its own message",
       txOverrideSelector(c.tx[0], db) >= 0 &&
       strcmp(db.sig[txOverrideSelector(c.tx[0], db)].name, "Cmd_Op") == 0);
    ck("two payloads under different codes stay under different codes",
       c.tx[0].muxCode == 1 && c.tx[1].muxCode == 16);

    /* A file that declares its own M wins: it is the better place to say it,
       and two answers would be one too many. */
    ck("an override on a message the file already multiplexes is dropped",
       c.tx[2].muxSel[0] == '\0' && c.tx[2].muxCode == -1, c.tx[2].muxSel);
    /* And an override written against some other frame map stops being obeyed
       rather than putting a code into bits that are not the selector's. */
    ck("an override naming a signal this map does not have is dropped",
       c.tx[3].muxSel[0] == '\0' && c.tx[3].muxCode == -1, c.tx[3].muxSel);

    /* The mechanism buildSignalFrame() uses, end to end: the selector as raw
       bits, then the payload through its own scaling, then read both back the
       way the ECU would. */
    const int16_t selIdx = txOverrideSelector(c.tx[1], db);
    if (selIdx >= 0 && c.tx[1].sig >= 0) {
      const DbcSignal &sel = db.sig[selIdx];
      const DbcSignal &pay = db.sig[c.tx[1].sig];
      uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      ck("the selector is written from the override's code",
         dbcInsertBits(data, 8, sel.startBit, sel.bits, sel.intel != 0,
                       (uint64_t)c.tx[1].muxCode));
      DbcEncoded e = dbcEncodeSignal(pay, 1.5, data, 8);
      ck("the payload is written", e.ok);
      ck("the ECU would read opcode 16",
         dbcExtractBits(data, 8, sel.startBit, sel.bits, sel.intel != 0,
                        nullptr) == 16,
         std::to_string((unsigned long long)dbcExtractBits(
           data, 8, sel.startBit, sel.bits, sel.intel != 0, nullptr)));
    }

    /* A selector with no code cannot build a frame, so it is not an override. */
    DashConfig h;
    dashReset(h);
    feed(h, "send 0 label=Val sig=ABS_Cmd.Cmd_Val lo=0 hi=255 msel=Cmd_Op\n");
    dashResolve(h, both(db));
    ck("a selector with no code is not an override",
       txOverrideSelector(h.tx[0], db) < 0);
  }

  printf("\n== which bus a cell and a setpoint belong to ==\n");
  {
    DbcDb db;
    loadMap(db);

    /* A layout written by the SINGLE-BUS logger has no bus= anywhere. It must
     * load, mean bus 1, and serialise back to a file with no bus= in it - or
     * every existing dash.cfg quietly changes the first time it is saved. */
    DashConfig old;
    dashReset(old);
    feed(old,
      "grid 2 2\n"
      "cell 0 widget=gauge sig=Drive.Speed lo=0 hi=50\n"
      "send 0 label=Set sig=ABS_Cmd.Cmd_Val lo=0 hi=255\n");
    ck("a layout with no bus= means bus 1",
       old.cell[0].bus == 0 && old.tx[0].bus == 0);

    char buf[DASH_CFG_MAX];
    const size_t n0 = dashSerialize(old, buf, sizeof(buf));
    /* Only the data lines. The '#' legend at the top of the file documents the
     * bus= key and is expected to mention it. */
    ck("and writes no bus= back", !dataLinesMention(buf, n0, " bus="));

    /* An explicit bus survives the round trip, which is what makes the layout
     * a durable record rather than something the logger reinterprets. */
    DashConfig two;
    dashReset(two);
    feed(two,
      "grid 2 2\n"
      "cell 0 widget=gauge sig=Drive.Speed bus=2 lo=0 hi=50\n"
      "cell 1 widget=bar   sig=Drive.Speed bus=1 lo=0 hi=50\n"
      "send 0 label=Set sig=ABS_Cmd.Cmd_Val bus=2 lo=0 hi=255\n");
    ck("bus= is parsed, one-based in the file",
       two.cell[0].bus == 1 && two.cell[1].bus == 0 && two.tx[0].bus == 1);

    const size_t n1 = dashSerialize(two, buf, sizeof(buf));
    DashConfig back;
    dashReset(back);
    dashParse(back, buf, n1);
    ck("and survives a round trip",
       back.cell[0].bus == 1 && back.cell[1].bus == 0 && back.tx[0].bus == 1);

    /* An out-of-range bus is clamped rather than accepted: a hand-edited file
     * must not be able to point a cell at a controller that does not exist. */
    DashConfig bad;
    dashReset(bad);
    feed(bad, "cell 0 widget=gauge sig=Drive.Speed bus=9 lo=0 hi=50\n");
    ck("an impossible bus falls back to bus 1", bad.cell[0].bus == 0);

    /* The saved text is what dashstore hashes, so two layouts differing only
     * by bus must not collide - otherwise flash would keep the wrong one. */
    DashConfig a1, a2;
    dashReset(a1); dashReset(a2);
    feed(a1, "cell 0 widget=gauge sig=Drive.Speed bus=1 lo=0 hi=50\n");
    feed(a2, "cell 0 widget=gauge sig=Drive.Speed bus=2 lo=0 hi=50\n");
    char b1[DASH_CFG_MAX], b2[DASH_CFG_MAX];
    const size_t k1 = dashSerialize(a1, b1, sizeof(b1));
    const size_t k2 = dashSerialize(a2, b2, sizeof(b2));
    ck("the bus changes the hash", dashHash(b1, k1) != dashHash(b2, k2));

    /* Resolution follows the cell's own bus. With the map on bus 1 only, a
     * cell pointed at bus 2 must NOT resolve - decoding it against the other
     * bus's map is exactly the silent error the split exists to prevent. */
    DbcDb empty; dbcReset(empty);
    static DbcDb split[CAN_BUSES];
    split[0] = db;
    split[1] = empty;

    DashConfig r;
    dashReset(r);
    feed(r,
      "cell 0 widget=gauge sig=Drive.Speed bus=1 lo=0 hi=50\n"
      "cell 1 widget=gauge sig=Drive.Speed bus=2 lo=0 hi=50\n");
    const uint16_t missing = dashResolve(r, split);
    ck("the bus-1 cell resolves", r.cell[0].sig >= 0);
    ck("the bus-2 cell does not, against an empty map", r.cell[1].sig < 0);
    ck("and it is counted as missing", missing == 1,
       "got " + std::to_string(missing));
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
