#!/bin/sh
# Screenshots of every interface state (port/UI.md) in Azahar, both screens, one run at a time (the
# runs share the virtual SD's debug.txt). Writes PNGs and a contact sheet to .lr3ds/ui/.
#
#   tests/ui/capture.sh              every state
#   tests/ui/capture.sh pause items  only these
#
# Touch coordinates follow touchui.c's layout: list rows from y 32, 24 high when they fit; the play
# page's quick slots at x 220..318, y 146 + 18 per slot.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SD="${LR3DS_SAVE:-$ROOT/.lr3ds/save}/Azahar/sdmc/3ds/sh23ds"
OUT="${UI_OUT:-$ROOT/.lr3ds/ui}"
RUN="$ROOT/tools/lr3ds/run.sh"
mkdir -p "$OUT"
[ -f "$SD/debug.txt" ] && cp "$SD/debug.txt" "$OUT/debug.txt.bak"
fail=0
ROOM="flag 32 1\nitem 1 2\nitem 2 1\nitem 18 1\nspawn er05 9850 0 10281 180 2\n"
DECK="flag 32 1\nflag 24 1\nitem 18 1\nspawn er01 -19250 0 21250 90 2\n"

want() { [ -z "$ONLY" ] || echo " $ONLY " | grep -q " $1 "; }

shot() {   # name, debug.txt contents, frames, lr3ds options...
	name=$1; dbg=$2; frames=$3; shift 3
	want "$name" || return 0
	printf "logos 0\n$dbg" > "$SD/debug.txt"
	if "$RUN" 600 --frames "$frames" --shot "$((frames - 5)):$OUT/$name.bmp" "$@" 2>&1 | grep -q "^\[done\] $frames frames"; then
		sips -s format png "$OUT/$name.bmp" --out "$OUT/$name.png" >/dev/null && rm -f "$OUT/$name.bmp"
		echo "ok    $name"
	else
		echo "FAIL  $name"
		fail=1
	fi
}

ONLY="$*"
shot title     "setup 0\n" 300
shot setup     "setup 1\n" 300
shot setup2    "setup 1\n" 380 --press 300-303:A
shot levels    "setup 0\n" 420 --press 300-303:DOWN --press 330-333:A --press 360-363:A
shot play      "$ROOM" 450
shot playmap   "$DECK" 450
shot heal      "$ROOM" 480 --touch 440-443:260,155
shot pause     "$ROOM" 480 --press 420-423:START
shot quit      "$ROOM" 500 --press 420-423:START --touch 450-453:100,140
shot options   "$ROOM" 540 --press 420-423:START --press 450-453:DOWN --press 470-473:DOWN --press 490-493:DOWN --press 510-513:A
shot optmove   "$ROOM" 580 --press 420-423:START --press 450-453:DOWN --press 470-473:DOWN --press 490-493:DOWN --press 510-513:A --press 540-543:DOWN
shot optbehind "$ROOM" 620 --press 420-423:START --press 450-453:DOWN --press 470-473:DOWN --press 490-493:DOWN --press 510-513:A --press 540-543:A --press 570-573:DOWN
shot optgame   "$ROOM" 630 --press 420-423:START --press 450-453:DOWN --press 470-473:DOWN --press 490-493:DOWN --press 510-513:A --press 540-543:R --press 570-573:DOWN --press 590-593:DOWN
shot optdisp   "$ROOM" 640 --press 420-423:START --press 450-453:DOWN --press 470-473:DOWN --press 490-493:DOWN --press 510-513:A --press 540-543:R --press 560-563:R --press 590-593:DOWN --press 610-613:DOWN
shot optsound  "$ROOM" 660 --press 420-423:START --press 450-453:DOWN --press 470-473:DOWN --press 490-493:DOWN --press 510-513:A --press 540-543:L --press 560-563:L --press 590-593:DOWN --press 610-613:DOWN
shot optsystem "$ROOM" 620 --press 420-423:START --press 450-453:DOWN --press 470-473:DOWN --press 490-493:DOWN --press 510-513:A --press 540-543:L --press 570-573:DOWN
shot items     "$ROOM" 480 --press 420-423:Y
shot examine   "$ROOM" 520 --press 420-423:Y --press 450-453:X
shot memos     "$ROOM" 500 --press 420-423:START --touch 450-453:100,188
shot map       "$DECK" 500 --press 420-423:START --touch 450-453:100,164
shot message   "spawn ap38 -19625 0 99081 180 2\n" 480 --press 420-424:A
shot savepoint "flag 32 1\nspawn er05 9850 0 10281 0 2\n" 520 --press 420-423:A --press 460-463:A
shot savefiles "flag 32 1\nspawn er05 9850 0 10281 0 2\n" 600 --press 420-423:A --press 460-463:A --press 500-503:A --press 540-543:A
shot cutscene  "flag 32 1\nspawn er01 -19520 0 20430 -90 2\ndemo first_toilet 300\n" 520

# every explainer and first-run choice laid out in each language and button layout (UI.md, "Text that
# always fits"): any line cut fails the capture
if want textcheck; then
	rm -f "$SD/textcheck.txt"
	printf "logos 0\nsetup 0\ntextcheck 1\n" > "$SD/debug.txt"
	"$RUN" 600 --frames 500 >/dev/null 2>&1
	if [ -f "$SD/textcheck.txt" ] && tail -1 "$SD/textcheck.txt" | grep -q "^0 cut"; then
		echo "ok    textcheck"
	else
		echo "FAIL  textcheck"; grep "^cut" "$SD/textcheck.txt" 2>/dev/null | head -20
		fail=1
	fi
	cp "$SD/textcheck.txt" "$OUT/textcheck.txt" 2>/dev/null
fi

if [ -f "$OUT/debug.txt.bak" ]; then mv "$OUT/debug.txt.bak" "$SD/debug.txt"; else rm -f "$SD/debug.txt"; fi
"$ROOT/.venv/bin/python" -I - "$OUT" <<'EOF'
import sys, os
from PIL import Image
d = sys.argv[1]
names = ['title', 'setup', 'setup2', 'levels', 'play', 'playmap', 'heal', 'pause', 'quit', 'options', 'optmove',
         'optbehind', 'optgame', 'optdisp', 'optsound', 'optsystem', 'items', 'examine', 'memos', 'map', 'message',
         'savepoint', 'savefiles', 'cutscene']
ims = [(n, Image.open(os.path.join(d, n + '.png'))) for n in names if os.path.exists(os.path.join(d, n + '.png'))]
cols = 4
if ims:
    sheet = Image.new('RGB', (400 * cols, 480 * ((len(ims) + cols - 1) // cols)))
    for i, (n, im) in enumerate(ims):
        sheet.paste(im, ((i % cols) * 400, (i // cols) * 480))
    sheet.save(os.path.join(d, 'sheet.png'))
EOF
exit $fail
