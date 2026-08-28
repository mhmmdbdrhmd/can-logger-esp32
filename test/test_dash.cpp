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
      "send 4 label=\"Wheel dia\" sig=Cmd.WheelDia lo=300 hi=3000 mux=1\n");

    ck("the node this logger is survives a round trip",
       strcmp(a.role, "Tester") == 0, a.role);

    /* Whether a value is one payload of a multiplexed message is carried BY the
       value, not looked up in the frame map, because the customizer's Remove
       button has to group the set even when no map is loaded - the desk tool
       before a .dbc is chosen, or a logger whose card has none. Looked up, the
       answer becomes "not multiplexed" the moment the map is away, the set
       stops being a set, and the payloads can be deleted one at a time. So it
       has to survive the round trip through the file. */
    ck("a multiplexed payload is remembered as one", a.tx[4].mux == 1,
       std::to_string(a.tx[4].mux));
    ck("and an ordinary value is not", a.tx[0].mux == 0,
       std::to_string(a.tx[0].mux));

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
         strcmp(legacy.role, "Tester") == 0, legacy.role);
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

    const uint16_t missing = dashResolve(c, db);
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
      snprintf(d.role, sizeof(d.role), "%s", "Vehicle");
      const uint16_t gone = dashDropUnresolved(d, db);
      ck("loading a new map drops what it cannot account for", gone == 1,
         std::to_string(gone) + " dropped");
      ck("a role the new map still names is left alone",
         strcmp(d.role, "Vehicle") == 0, d.role);
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
      snprintf(e.role, sizeof(e.role), "%s", "Vehicle");
      const uint16_t all = dashDropUnresolved(e, other);
      ck("an unrelated frame map clears the setup", all == 6,
         std::to_string(all) + " dropped");
      ck("and leaves nothing behind",
         !dashCellUsed(e.cell[0]) && !txCommandUsed(e.tx[0]), "empty");
      /* The role went with it. Nothing in the new file transmits under that
         name, so keeping it would leave the header asserting a role while both
         Fill buttons quietly stopped separating anything by it. */
      ck("including a role no node of the new map answers to", e.role[0] == 0,
         e.role);

      DashConfig f = c;
      snprintf(f.role, sizeof(f.role), "%s", "Tester");
      dashDropUnresolved(f, other);
      ck("a role the new map DOES name survives it",
         strcmp(f.role, "Tester") == 0, f.role);
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
    dashResolve(c, db);

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

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
