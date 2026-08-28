#!/usr/bin/env bash
# ============================================================================
#  Host-side tests for the portable logic.
#
#  These compile the REAL sources from src/ against the small shims in shim/,
#  so they exercise the same code the firmware runs - no ESP32, no CAN hardware
#  and no SD card. They cover the parts where a silent bug would corrupt a
#  recording without anyone noticing: the DBC parser, bit extraction, scaling,
#  CSV row construction, the CANopen framing layer and the logger.
#
#      ./test/run_tests.sh
#
#  Also type-checks every module under -Wall -Wextra. Note the shims are not the
#  real ESP-IDF/Arduino APIs, so this cannot catch an SDK signature mismatch -
#  only a real `pio run` does that.
# ============================================================================
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../src"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

CXX="${CXX:-g++}"
FLAGS=(-std=c++17 -Wall -Wextra -Wno-unused-parameter -I "$here/shim" -I "$src")

fail=0

echo "=== type-checking every module ==="
for f in dbc canopen decode logger mcp2515 recorder netcfg dash dashstore cantx webui webpage; do
    if "$CXX" "${FLAGS[@]}" -c "$src/$f.cpp" -o "$out/$f.o" 2>"$out/$f.err"; then
        echo "  ok   $f.cpp"
    else
        echo "  FAIL $f.cpp"; sed 's/^/       /' "$out/$f.err" | head -20; fail=1
    fi
done

run_test() {                      # name, source, extra sources...
    local name="$1"; shift
    local main="$1"; shift
    echo
    echo "=== $name ==="
    if "$CXX" "${FLAGS[@]}" "$main" "$@" -o "$out/t"; then
        "$out/t" || fail=1
    else
        echo "  FAIL could not build $name"; fail=1
    fi
}

run_test "DBC parser and signal decoding" "$here/test_dbc.cpp"     "$src/dbc.cpp"
run_test "CANopen framing"                "$here/test_canopen.cpp" "$src/canopen.cpp"
run_test "CSV schema"                     "$here/test_decode.cpp"  "$src/decode.cpp" "$src/dbc.cpp"
run_test "logger"                         "$here/test_logger.cpp"  "$src/logger.cpp"
run_test "MCP2515 driver and transmit"    "$here/test_mcp2515.cpp" "$src/mcp2515.cpp"
run_test "signal encoding"                "$here/test_encode.cpp"  "$src/dbc.cpp"
run_test "dashboard configuration"        "$here/test_dash.cpp"    "$src/dash.cpp" "$src/dbc.cpp"

echo
echo "=== the example frame maps parse ==="
"$CXX" "${FLAGS[@]}" -x c++ - "$src/dbc.cpp" -o "$out/t_example" <<'CPP' && \
    for f in "$here"/../examples/*.dbc; do "$out/t_example" "$f" || fail=1; done
#include "dbc.h"
#include <stdio.h>
uint32_t g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;
int main(int argc, char **argv) {
  if (argc < 2) return 1;
  FILE *fp = fopen(argv[1], "r");
  if (!fp) { printf("  FAIL cannot open %s\n", argv[1]); return 1; }
  DbcDb db = {};
  std::string text;
  char line[DBC_LINE_MAX];
  while (fgets(line, sizeof(line), fp)) text += line;
  fclose(fp);
  dbcLoadText(db, text.c_str(), text.size());
  const bool ok = db.loaded && db.msgCount > 0 && db.sigCount > 0 &&
                  db.lineErrors == 0 && !db.overflow;
  printf("  %s %u messages, %u signals, %u value labels, %u line errors\n",
         ok ? "ok  " : "FAIL", db.msgCount, db.sigCount, db.valCount, db.lineErrors);
  return ok ? 0 : 1;
}
CPP

echo
echo "=== the page (DOM ids, JSON fields, JS syntax) ==="
python3 - "$src/webpage.cpp" "$src/webui.cpp" "$src/config.h" <<'PAGECHECK' || fail=1
import re, subprocess, sys, shutil, tempfile, os
page = open(sys.argv[1]).read()
api  = open(sys.argv[2]).read()
conf = open(sys.argv[3]).read()

# The page is several PROGMEM chunks that the root handler concatenates. The
# browser only ever sees the whole thing, so that is what gets checked.
parts = re.findall(r'R"HTML\((.*?)\)HTML"', page, re.S)
html  = "".join(parts)
bad = 0
print(f"  ok   {len(parts)} page chunks, {len(html)} bytes")

# Every id the script reaches for must exist in the markup. This is the check
# that catches a rename made in one half and not the other.
ids = set(re.findall(r"q\('([^']+)'\)", html))
missing = sorted(i for i in ids if 'id="%s"' % i not in html)
print("  %s %d DOM ids referenced%s" % ("ok  " if not missing else "FAIL",
      len(ids), "" if not missing else " - missing %s" % missing))
bad |= bool(missing)

# Controls that exist in more than one place are hooked by class instead of by
# id. Same failure mode as above - a rename in one half only - so same check.
cls  = set(re.findall(r"querySelectorAll\('\.([A-Za-z0-9_-]+)'\)", html))
def has_class(c):
    return any(c in a.split() for a in re.findall(r'class="([^"]*)"', html))
gone = sorted(c for c in cls if not has_class(c))
print("  %s %d class hooks resolve%s" % ("ok  " if not gone else "FAIL",
      len(cls), "" if not gone else " - missing %s" % gone))
bad |= bool(gone)

# The page carries its own copy of the two capacity limits, because it has to
# draw something before the first fetch answers. A copy that disagrees with the
# firmware would let somebody build a layout the logger then silently truncates.
def cdefine(name):
    m = re.search(r'#define\s+%s\s+(\d+)' % name, conf)
    return int(m.group(1)) if m else None
def jsvar(name):
    m = re.search(r'\b%s\s*=\s*(\d+)' % name, page)
    return int(m.group(1)) if m else None
lim = [("MAXCELLS", "DASH_MAX_CELLS"), ("MAXSEND", "TX_MAX_COMMANDS")]
off = [(j, jsvar(j), c, cdefine(c)) for j, c in lim if jsvar(j) != cdefine(c)]
print("  %s the page's capacity limits match config.h%s" %
      ("ok  " if not off else "FAIL",
       "" if not off else " - " + ", ".join("%s=%s but %s=%s" % o for o in off)))
bad |= bool(off)

# Every JSON field the page reads must be one a handler actually produces.
made = set(re.findall(r'\\"([a-zA-Z]+)\\":', api))
used = set(re.findall(r'\bd\.([a-zA-Z_]+)', html)) - {"lines", "seq"}
gap  = sorted(f for f in used if f not in made)
print("  %s %d JSON fields all produced by a handler%s" %
      ("ok  " if not gap else "FAIL", len(used),
       "" if not gap else " - missing %s" % gap))
bad |= bool(gap)

# A field emitted twice in one document is not an error any JSON parser
# reports: the second one silently wins. That cost the dashboard its grid size
# once already, when "rows" meant both the grid's rows and the recording's.
for fn in ("handleDash", "handleStatus"):
    start = api.find("static void " + fn + "(")
    if start < 0:
        print("  FAIL %s not found" % fn); bad = 1; continue
    end  = api.find("\nstatic ", start + 10)
    body = api[start:end if end > 0 else len(api)]
    keys = re.findall(r'\\"([a-zA-Z]+)\\":', body)
    dupes = sorted({k for k in keys if keys.count(k) > 1})
    print("  %s %s emits no field twice%s" % ("ok  " if not dupes else "FAIL",
          fn, "" if not dupes else " - %s" % dupes))
    bad |= bool(dupes)

# The page must not name any bus-specific signal: everything it shows comes out
# of the frame map at runtime.
if "g_dbc" not in api or "g_live" not in api:
    print("  FAIL the status handler does not read the frame map"); bad = 1
else:
    print("  ok   the live view is driven by the frame map, not by the firmware")

scripts = re.findall(r"<script>(.*?)</script>", html, re.S)
if shutil.which("node"):
    # Each block, then all of them together, because they share globals.
    cases = [("block %d" % (i + 1), s) for i, s in enumerate(scripts)]
    cases.append(("all blocks together", "\n".join(scripts)))
    for name, js in cases:
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
            f.write(js); pth = f.name
        r = subprocess.run(["node", "--check", pth], capture_output=True, text=True)
        os.unlink(pth)
        print("  %s JS syntax, %s%s" % ("ok  " if r.returncode == 0 else "FAIL",
              name, "" if r.returncode == 0 else "\n" + r.stderr))
        bad |= (r.returncode != 0)
else:
    print("  skip JS syntax (node not installed)")

# A name declared with `var` twice inside one function is legal JavaScript and
# silently wrong: `var` is function-scoped, so the second declaration reaches
# back and overwrites the first for every closure already holding it. That cost
# a real bug - two `var mates`, one meaning "the values that LEAVE together"
# (excluding this card) and one meaning "the values that are REMOVED together"
# (including it). The Remove button captured the first and was handed the
# second, so it deleted every value of a message except the one whose button was
# pressed, while its label, computed before the overwrite, still read correctly.
# Nothing about that is a syntax error, so only a check like this notices it.
def _close(text, open_brace):
    """Index of the } matching the { at open_brace."""
    d = 0
    for k in range(open_brace, len(text)):
        if text[k] == "{":
            d += 1
        elif text[k] == "}":
            d -= 1
            if d == 0:
                return k
    return len(text)


def scopes(text, label="(top level)"):
    """Every function body in `text`, each paired with a name, innermost too.

    EVERY function, not only the declared ones: the bug this exists for lived in
    an anonymous forEach callback, which a check that only walks `function foo()`
    would have blanked out as somebody else's scope and never looked at.
    """
    out = []
    for m in re.finditer(r"\bfunction\b\s*(\w*)\s*\([^)]*\)\s*\{", text):
        b = m.end() - 1
        e = _close(text, b)
        nm = m.group(1) or label + " callback"
        body = text[b + 1:e]
        out.append((nm, body))
        out.extend(scopes(body, nm))
    return out


dupes = []
for block in scripts:
    for name, body in scopes(block):
        # Blank every nested function body: a `var` inside one belongs to that
        # function, not this one, and it is visited as its own scope anyway.
        flat, j = [], 0
        while True:
            f = body.find("function", j)
            if f < 0:
                flat.append(body[j:]); break
            b = body.find("{", f)
            if b < 0:
                flat.append(body[j:]); break
            e = _close(body, b)
            flat.append(body[j:b])
            flat.append(" " * (e - b))           # keep offsets, drop content
            j = e
        flat, seen = "".join(flat), set()

        for v in re.finditer(r"\bvar\s+(\w+)\b", flat):
            if v.group(1) in seen:
                dupes.append("%s: var %s declared twice" % (name, v.group(1)))
            seen.add(v.group(1))
if dupes:
    print("  FAIL a var is declared twice in one function scope:")
    for d in sorted(set(dupes)):
        print("       " + d)
    bad = 1
else:
    print("  ok   no var is declared twice in one function scope")

sys.exit(1 if bad else 0)
PAGECHECK

echo
echo "=== the example layout loads against the example frame map ==="
"$CXX" "${FLAGS[@]}" -x c++ - "$src/dash.cpp" "$src/dbc.cpp" -o "$out/t_cfg" <<'CFGCHECK' && \
    "$out/t_cfg" "$here/../examples/dash.cfg" "$here/../examples/machine.dbc" || fail=1
#include "dash.h"
#include <stdio.h>
#include <vector>
uint32_t g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;
int main(int argc, char **argv) {
  if (argc < 3) return 1;
  FILE *f = fopen(argv[1], "rb");
  if (!f) { printf("  FAIL cannot open %s\n", argv[1]); return 1; }
  std::vector<char> txt;
  int c;
  while ((c = fgetc(f)) != EOF) txt.push_back((char)c);
  fclose(f);

  DashConfig cfg;
  dashReset(cfg);
  const uint16_t errors = dashParse(cfg, txt.data(), txt.size());

  DbcDb db = {};
  FILE *d = fopen(argv[2], "r");
  if (!d) { printf("  FAIL cannot open %s\n", argv[2]); return 1; }
  std::string dtxt;
  char line[DBC_LINE_MAX];
  while (fgets(line, sizeof(line), d)) dtxt += line;
  fclose(d);
  dbcLoadText(db, dtxt.c_str(), dtxt.size());

  const uint16_t missing = dashResolve(cfg, db);

  /* Serialising twice must produce the same bytes, or the boot rule would
   * decide the card had been edited on every single start. */
  std::vector<char> a(DASH_CFG_MAX), b(DASH_CFG_MAX);
  const size_t na = dashSerialize(cfg, a.data(), a.size());
  DashConfig again;
  dashReset(again);
  dashParse(again, a.data(), na);
  const size_t nb = dashSerialize(again, b.data(), b.size());
  const bool stable = (na == nb) && memcmp(a.data(), b.data(), na) == 0;

  int cells = 0, sends = 0;
  for (int i = 0; i < DASH_MAX_CELLS; i++) if (dashCellUsed(cfg.cell[i])) cells++;
  for (int i = 0; i < TX_MAX_COMMANDS; i++) if (txCommandUsed(cfg.tx[i])) sends++;

  const bool ok = !errors && !missing && cells && sends && stable &&
                  na < DASH_CFG_MAX;
  printf("  %s %d cells, %d sendable values, %u parse errors, %u unresolved, "
         "%zu bytes, round trip %s\n",
         ok ? "ok  " : "FAIL", cells, sends, errors, missing, na,
         stable ? "stable" : "NOT STABLE");
  return ok ? 0 : 1;
}
CFGCHECK

echo
echo "=== the desk tools read a frame map the same way the firmware does ==="
# tools/check_dbc.py and tools/preview_dashboard.py share one DBC reader written
# in Python, so they run on any machine with no compiler. That is a SECOND
# implementation of src/dbc.cpp, which is normally a reason to distrust it - so
# this asserts the two agree, message for message and signal for signal, on
# every frame map in examples/. Drift becomes a test failure rather than a
# dashboard that works at a desk and not in a field.
"$CXX" "${FLAGS[@]}" -x c++ - "$src/dbc.cpp" -o "$out/t_dump" <<'CPP' || fail=1
#include "dbc.h"
#include <stdio.h>
uint32_t g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;
int main(int argc, char **argv) {
  if (argc < 2) return 1;
  FILE *fp = fopen(argv[1], "r");
  if (!fp) return 1;
  DbcDb db = {};
  std::string text;
  char line[DBC_LINE_MAX];
  while (fgets(line, sizeof(line), fp)) text += line;
  fclose(fp);
  dbcLoadText(db, text.c_str(), text.size());
  for (uint16_t mi = 0; mi < db.msgCount; mi++) {
    const DbcMessage &m = db.msg[mi];
    printf("M\t%s\t%lu\t%s\t%d\n", m.name, (unsigned long)m.id,
           dbcTxNode(db, m), m.muxSignal >= 0 ? 1 : 0);
    for (uint16_t k = 0; k < m.signalCount; k++) {
      const uint16_t si = (uint16_t)(m.firstSignal + k);
      if (si >= db.sigCount) break;
      const DbcSignal &s = db.sig[si];
      printf("S\t%s\t%s\t%u\t%d\t%u\t%s\n", m.name, s.name, s.bits,
             s.muxValue, s.exact ? s.dec : 3, s.unit);
    }
  }
  return 0;
}
CPP

for f in "$here"/../examples/*.dbc; do
    "$out/t_dump" "$f" > "$out/c.txt" 2>/dev/null
    python3 - "$f" "$out/c.txt" <<'AGREE' || fail=1
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), ""))
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__ if '__file__' in dir() else sys.argv[0])))
sys.path.insert(0, os.path.join(os.getcwd(), "tools"))
from preview_dashboard import load_dbc

path, cdump = sys.argv[1], sys.argv[2]
db = load_dbc(path)
mine = []
for m in db["m"]:
    mid = int(m["id"], 16)
    mine.append("M\t%s\t%d\t%s\t%d" % (m["n"], mid, m["tx"], m["mux"]))
    for s in m["s"]:
        mine.append("S\t%s\t%s\t%d\t%d\t%d\t%s"
                    % (m["n"], s["n"], s["b"], s["mx"], s["d"], s["u"]))
theirs = [l for l in open(cdump).read().splitlines() if l]

name = os.path.basename(path)
if mine == theirs:
    print("  ok   %-16s %d messages, %d signals - both parsers agree"
          % (name, len(db["m"]), sum(len(m["s"]) for m in db["m"])))
    sys.exit(0)
print("  FAIL %s - the Python reader and src/dbc.cpp disagree" % name)
for a, b in zip(mine + [""] * len(theirs), theirs + [""] * len(mine)):
    if a != b:
        print("       python: %s" % a)
        print("       c     : %s" % b)
        break
sys.exit(1)
AGREE
done


if python3 "$here/../tools/check_dbc.py" "$here/../examples/example.dbc" > "$out/chk" 2>&1; then
    echo "  ok   check_dbc.py reports a clean file as clean"
else
    echo "  FAIL check_dbc.py"; sed 's/^/       /' "$out/chk" | head -20; fail=1
fi
printf 'BU_: X\nBO_ 100 T: 8 X\n SG_ Broken : not a signal\n' > "$out/bad.dbc"
if python3 "$here/../tools/check_dbc.py" "$out/bad.dbc" > "$out/chk2" 2>&1; then
    echo "  FAIL check_dbc.py called a broken file clean"; fail=1
else
    echo "  ok   check_dbc.py reports an unreadable line and exits nonzero"
fi

echo
echo "=== a setup and a frame map that do not belong together ==="
# The firmware drops what a newly loaded map cannot account for
# (dashDropUnresolved) and so does the desk tool (prune_cfg in
# tools/preview_dashboard.py). Two implementations of one rule is how a page
# comes to write a layout back over a card that had just been cleaned, so this
# asserts they agree: same survivors, same order, same answer about the role.
"$CXX" "${FLAGS[@]}" -x c++ - "$src/dash.cpp" "$src/dbc.cpp" -o "$out/t_prune" <<'PRUNE' || fail=1
#include "dash.h"
#include <stdio.h>
#include <vector>
uint32_t g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;
static std::string slurp(const char *path) {
  std::string t;
  FILE *f = fopen(path, "rb");
  if (!f) return t;
  int c;
  while ((c = fgetc(f)) != EOF) t += (char)c;
  fclose(f);
  return t;
}
int main(int argc, char **argv) {
  if (argc < 3) return 1;
  const std::string ctext = slurp(argv[1]), dtext = slurp(argv[2]);

  DashConfig cfg;
  dashReset(cfg);
  dashParse(cfg, ctext.data(), ctext.size());
  DbcDb db = {};
  dbcLoadText(db, dtext.c_str(), dtext.size());

  printf("dropped\t%u\n", (unsigned)dashDropUnresolved(cfg, db));
  printf("role\t%s\n", cfg.role);
  for (int i = 0; i < DASH_MAX_CELLS; i++)
    if (dashCellUsed(cfg.cell[i])) printf("cell\t%s\n", cfg.cell[i].ref);
  for (int i = 0; i < TX_MAX_COMMANDS; i++)
    if (txCommandUsed(cfg.tx[i]))
      printf("send\t%s\n", cfg.tx[i].kind == TXK_SIGNAL ? cfg.tx[i].ref : "-raw-");
  return 0;
}
PRUNE

for f in "$here"/../examples/*.dbc; do
    "$out/t_prune" "$here/../examples/dash.cfg" "$f" > "$out/prune.txt" 2>/dev/null
    python3 - "$here/../examples/dash.cfg" "$f" "$out/prune.txt" <<'SAME' || fail=1
import os, sys
sys.path.insert(0, os.path.join(os.getcwd(), "tools"))
from preview_dashboard import load_dbc, prune_cfg, _sig_of

cfg_path, dbc_path, cdump = sys.argv[1], sys.argv[2], sys.argv[3]
db = load_dbc(dbc_path)
by_ref = {m["n"] + "." + s["n"]: s for m in db["m"] for s in m["s"]}
text, gone = prune_cfg(open(cfg_path).read(), by_ref, db.get("nodes", []))

mine = ["dropped\t%d" % gone]
role = [l.split(None, 1)[1].strip('"') for l in text.splitlines()
        if l.startswith("role ")]
mine.append("role\t%s" % (role[0] if role else ""))
for line in text.splitlines():
    if line.startswith("cell "):
        mine.append("cell\t%s" % _sig_of(line))
    elif line.startswith("send "):
        mine.append("send\t%s" % (_sig_of(line) or "-raw-"))
theirs = [l for l in open(cdump).read().splitlines() if l]

name = os.path.basename(dbc_path)
if mine == theirs:
    print("  ok   dash.cfg against %-16s %s dropped, both agree"
          % (name, gone))
    sys.exit(0)
print("  FAIL dash.cfg against %s - prune_cfg and dashDropUnresolved disagree"
      % name)
for a, b in zip(mine + [""] * len(theirs), theirs + [""] * len(mine)):
    if a != b:
        print("       python: %s" % a)
        print("       c     : %s" % b)
        break
sys.exit(1)
SAME
done
echo
echo "=== the Arduino sketch folder regenerates from src/ ==="
if "$here/../arduino/sync.sh" --check; then
    echo "  ok   sketch folder regenerates byte-for-byte from src/"
else
    echo "  FAIL run arduino/sync.sh"; fail=1
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit "$fail"
