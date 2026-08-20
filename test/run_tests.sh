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
for f in dbc canopen decode logger mcp2515 recorder netcfg webui; do
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
run_test "MCP2515 driver"                 "$here/test_mcp2515.cpp" "$src/mcp2515.cpp"

echo
echo "=== example DBC parses ==="
"$CXX" "${FLAGS[@]}" -x c++ - "$src/dbc.cpp" -o "$out/t_example" <<'CPP' && \
    "$out/t_example" "$here/../examples/example.dbc" || fail=1
#include "dbc.h"
#include <stdio.h>
uint32_t g_fakeMs = 0;
FakeSerial Serial;
FakeEsp    ESP;
int main(int argc, char **argv) {
  if (argc < 2) return 1;
  FILE *fp = fopen(argv[1], "r");
  if (!fp) { printf("  FAIL cannot open %s\n", argv[1]); return 1; }
  DbcDb db; dbcReset(db);
  char line[DBC_LINE_MAX];
  while (fgets(line, sizeof(line), fp)) dbcParseLine(db, line);
  fclose(fp);
  const bool ok = db.loaded && db.msgCount > 0 && db.sigCount > 0 &&
                  db.lineErrors == 0 && !db.overflow;
  printf("  %s %u messages, %u signals, %u value labels, %u line errors\n",
         ok ? "ok  " : "FAIL", db.msgCount, db.sigCount, db.valCount, db.lineErrors);
  return ok ? 0 : 1;
}
CPP

echo
echo "=== dashboard (ids, status fields, JS syntax) ==="
python3 - "$src/webui.cpp" <<'PY' || fail=1
import re, subprocess, sys, shutil, tempfile, os
src = open(sys.argv[1]).read()
html = re.search(r'R"HTML\((.*?)\)HTML"', src, re.S).group(1)
js   = re.search(r'<script>(.*?)</script>', html, re.S).group(1)
bad  = 0

ids = set(re.findall(r"q\('([^']+)'\)", js))
missing = [i for i in ids if f'id="{i}"' not in html]
print(f"  {'ok  ' if not missing else 'FAIL'} {len(ids)} DOM ids referenced"
      + (f" - missing {missing}" if missing else ""))
bad |= bool(missing)

used = set(re.findall(r'\bd\.([a-zA-Z_]+)', js)) - {'lines', 'seq'}
made = set(re.findall(r'\\"([a-zA-Z]+)\\":', src))
gap  = sorted(f for f in used if f not in made)
print(f"  {'ok  ' if not gap else 'FAIL'} {len(used)} status fields all produced by handleStatus"
      + (f" - missing {gap}" if gap else ""))
bad |= bool(gap)

# The page must not name any bus-specific signal: everything it displays has to
# come from the frame map at runtime.
if 'g_dbc' not in src or 'g_live' not in src:
    print("  FAIL the status handler does not read the frame map")
    bad = 1
else:
    print("  ok   the live view is driven by the frame map, not by the firmware")

if shutil.which('node'):
    with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as f:
        f.write(js); p = f.name
    r = subprocess.run(['node', '--check', p], capture_output=True, text=True)
    os.unlink(p)
    print(f"  {'ok  ' if r.returncode == 0 else 'FAIL'} JS syntax"
          + ("" if r.returncode == 0 else "\n" + r.stderr))
    bad |= (r.returncode != 0)
else:
    print("  skip JS syntax (node not installed)")
sys.exit(1 if bad else 0)
PY

echo
echo "=== the Arduino sketch folder is in sync with src/ ==="
if "$here/../arduino/sync.sh" --check; then
    echo "  ok   sketch folder matches src/"
else
    echo "  FAIL run arduino/sync.sh"; fail=1
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit "$fail"
