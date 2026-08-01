#!/usr/bin/env bash
# ============================================================================
#  Mirrors src/ into the Arduino IDE sketch folder.
#
#  There is exactly one copy of the firmware, in src/. PlatformIO compiles it
#  directly. The Arduino IDE cannot: it insists every source file sits next to
#  a .ino named after its directory, so the sketch folder has to be a copy.
#
#  Run this once after cloning, and again after editing anything in src/:
#
#      ./arduino/sync.sh            # populate arduino/CanLogger/
#      ./arduino/sync.sh --check    # verify the script still works (used by CI)
#
#  The generated copies are git-ignored on purpose - a second checked-in copy
#  of every file is a second thing to keep correct.
# ============================================================================
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../src"
sketch="CanLogger"

check=0
[ "${1:-}" = "--check" ] && check=1

if [ "$check" -eq 1 ]; then
    dst="$(mktemp -d)/$sketch"
else
    dst="$here/$sketch"
fi
mkdir -p "$dst"

# Drop previously synced sources so a file deleted in src/ also disappears
# here, instead of lingering and being compiled into the sketch.
rm -f "$dst"/*.h "$dst"/*.cpp

copied=0
for f in "$src"/*.h "$src"/*.cpp; do
    base="$(basename "$f")"
    # main.cpp becomes the .ino; everything else keeps its name. The IDE
    # compiles .cpp files in the sketch folder as-is, so nothing else changes.
    if [ "$base" = "main.cpp" ]; then
        cp "$f" "$dst/$sketch.ino"
    else
        cp "$f" "$dst/$base"
    fi
    copied=$((copied + 1))
done

if [ "$check" -eq 1 ]; then
    rc=0
    [ -f "$dst/$sketch.ino" ] || { echo "  missing $sketch.ino"; rc=1; }
    for f in "$src"/*.h "$src"/*.cpp; do
        base="$(basename "$f")"
        [ "$base" = "main.cpp" ] && continue
        cmp -s "$f" "$dst/$base" || { echo "  $base did not copy"; rc=1; }
    done
    rm -rf "$(dirname "$dst")"
    exit "$rc"
fi

echo "synced $copied files -> $dst"
echo "open $dst/$sketch.ino in the Arduino IDE"
