#!/bin/sh
# Runs one playtest script in Azahar (headless) and keeps its log.
#   tests/playtest/pt.sh <script name, without .pt> [max frames]
# The script is written to the virtual SD as debug.txt (its "pt" lines drive port/source/playtest.c); the
# run stops when the driver writes "# end of script" to ptlog.txt. Scripts are looked up in
# tests/playtest/scripts, then in .lr3ds/playtest/scripts (the level copies run_levels.sh writes).
#
#   SLOT=2 / SLOT=3   use the virtual SD .lr3ds/save2 or save3, for runs side by side (each needs the pack
#                     and the saves the script loads; see port/TESTING.md)
#   APP=<path>        another build than port/sh23ds.3dsx
#   LR3DS_MODEL="Old 3DS"   the original 3DS model
# Log: .lr3ds/playtest/logs/<name>.log (ptlog.txt) and <name>.lr3ds (the emulator's output).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME=$1; FRAMES=${2:-60000}
SLOT=${SLOT:-}
SAVE="$ROOT/.lr3ds/save$SLOT"
SD="$SAVE/Azahar/sdmc/3ds/sh23ds"
OUT="$ROOT/.lr3ds/playtest"
CORE=${AZAHAR_CORE:-$HOME/Developer/tools/azahar/azahar-libretro-macos-arm64-2126.2/azahar_libretro.dylib}
APP=${APP:-$ROOT/port/sh23ds.3dsx}
[ -n "$NAME" ] || { echo "usage: $0 <script> [max frames]" >&2; exit 2; }
SCRIPT="$ROOT/tests/playtest/scripts/$NAME.pt"
[ -f "$SCRIPT" ] || SCRIPT="$OUT/scripts/$NAME.pt"
[ -f "$SCRIPT" ] || { echo "no script $NAME" >&2; exit 2; }
[ -d "$SD/bg" ] || { echo "no pack on the virtual SD $SD" >&2; exit 2; }
[ -f "$CORE" ] || { echo "Azahar libretro core not found at $CORE (set AZAHAR_CORE)" >&2; exit 2; }
mkdir -p "$OUT/logs"
[ -f "$SD/debug.txt" ] && mv "$SD/debug.txt" "$SD/debug.txt.pt-bak"   # put back after the run
grep -v '^#' "$SCRIPT" > "$SD/debug.txt"
rm -f "$SD/ptlog.txt"
start=$(date +%s)
perl -e 'alarm shift; exec @ARGV' 3000 "$ROOT/tools/lr3ds/lr3ds" "$CORE" "$APP" \
  --save "$SAVE" --sys "$ROOT/.lr3ds/sys$SLOT" --opt citra_graphics_api=Software \
  --opt "citra_is_new_3ds=${LR3DS_MODEL:-New 3DS}" \
  --frames "$FRAMES" --stop-on "$SD/ptlog.txt:# end of script" > "$OUT/logs/$NAME.lr3ds" 2>&1
cp "$SD/ptlog.txt" "$OUT/logs/$NAME.log" 2>/dev/null
rm -f "$SD/debug.txt"
[ -f "$SD/debug.txt.pt-bak" ] && mv "$SD/debug.txt.pt-bak" "$SD/debug.txt"
end=$(date +%s)
p=$(grep -c " PASS " "$OUT/logs/$NAME.log" 2>/dev/null); f=$(grep -c " FAIL " "$OUT/logs/$NAME.log" 2>/dev/null)
done=$(grep -c "# end of script" "$OUT/logs/$NAME.log" 2>/dev/null)
# what Azahar lets pass but a console stops on: a read of unmapped memory, svcBreak (citro3d's checks)
fatal=$(grep -m1 -o "unmapped [A-Za-z0-9]* @ 0x[0-9A-Fa-f]* at PC 0x[0-9A-Fa-f]*\|broke execution.*" "$OUT/logs/$NAME.lr3ds")
echo "$NAME: pass ${p:-0} fail ${f:-0} finished ${done:-0} ($((end - start)) s, $(tail -1 "$OUT/logs/$NAME.lr3ds"))${fatal:+, a console stops here: $fatal}"
[ "${f:-0}" = 0 ] && [ "${done:-0}" != 0 ] && [ -z "$fatal" ]
