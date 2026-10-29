#!/bin/sh
# Smoke test for port/sh23ds.3dsx in Azahar (headless): title screen, new game (prologue at the mirror),
# stepping out of the restroom door to the observation deck, an apartment door warp, the jammed door message, a save point. Writes PNGs and
# a contact sheet to .lr3ds/smoke/ and fails if a run does not reach its last frame, or if Azahar logged
# something it lets pass but a console stops on (a read of unmapped memory, svcBreak).
#
#   tools/lr3ds/smoke.sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SD="${LR3DS_SAVE:-$ROOT/.lr3ds/save}/Azahar/sdmc/3ds/sh23ds"   # LR3DS_SAVE: another virtual SD
OUT="${SMOKE_OUT:-$ROOT/.lr3ds/smoke}"
RUN="$ROOT/tools/lr3ds/run.sh"
mkdir -p "$OUT"
[ -f "$SD/debug.txt" ] && cp "$SD/debug.txt" "$OUT/debug.txt.bak"
fail=0

run() {   # name, debug.txt contents, frames, lr3ds options...
	name=$1; dbg=$2; frames=$3; shift 3
	printf "logos 0\n$dbg" > "$SD/debug.txt"   # logos 0: no start-up screens, so the frame counts hold
	log=$("$RUN" 1200 --frames "$frames" --shot "$((frames - 5)):$OUT/$name.bmp" "$@" 2>&1)
	fatal=$(echo "$log" | grep -m1 -o "unmapped [A-Za-z0-9]* @ 0x[0-9A-Fa-f]* at PC 0x[0-9A-Fa-f]*\|broke execution.*")
	if [ -n "$fatal" ]; then
		echo "FAIL  $name (a console stops here: $fatal)"
		fail=1
	elif echo "$log" | grep -q "^\[done\] $frames frames"; then
		sips -s format png "$OUT/$name.bmp" --out "$OUT/$name.png" >/dev/null && rm -f "$OUT/$name.bmp"
		echo "ok    $name"
	else
		echo "FAIL  $name"
		fail=1
	fi
}

run title     "" 300
run newgame   "debughud 1\n" 760 --press 300-303:DOWN --press 330-333:A --press 360-363:A --press 390-393:A --press 420-423:A --press 590-593:A     # menu on CONTINUE: down to NEW GAME, MAIN SCENARIO, action and riddle level, then A skips toilet.bik
run deck      "debughud 1\nflag 32 1\nspawn er01 -19250 0 21250 90 2\n" 480 --analog 250-340:0,-1
run door      "debughud 1\nspawn ap06 -49648 0 -66025 90 2\n" 560 --press 420-424:A
run jammed    "debughud 1\nspawn ap38 -19625 0 99081 180 2\n" 480 --press 420-424:A
run savepoint "debughud 1\nflag 32 1\nspawn er05 9850 0 10281 0 2\n" 660 --press 420-423:A --press 460-463:A --press 490-493:A --press 520-523:A --press 560-563:A --press 600-603:A
run continue  "debughud 1\n" 560 --press 300-303:A     # CONTINUE (the default with saves) loads the save with the most play time
run heal      "debughud 1\nflag 32 1\nitem 1 2\nspawn er05 9850 0 10281 180 2\n" 520 --press 420-423:Y --press 450-453:A
run wish      "debughud 1\n" 840 --press 300-303:DOWN --press 330-333:A --press 360-363:DOWN --press 390-393:A --press 420-423:A     # NEW GAME, SUB SCENARIO, action level
run cutscene  "debughud 1\nflag 32 1\nspawn er01 -19520 0 20430 -90 2\ndemo first_toilet 300\n" 520
run angela    "debughud 1\nflag 32 1\nspawn ca11 3475 2318 -1034 0 2\ndemo haka_agl 300\n" 700     # an outdoor scene: the forest tiles, Angela and her voice
run pause     "flag 32 1\nspawn er05 9850 0 10281 180 2\n" 500 --press 420-423:START --press 450-453:DOWN

if [ -f "$OUT/debug.txt.bak" ]; then mv "$OUT/debug.txt.bak" "$SD/debug.txt"; else rm -f "$SD/debug.txt"; fi
"$ROOT/.venv/bin/python" -I - "$OUT" <<'EOF'
import sys, os
from PIL import Image
d = sys.argv[1]
names = ['title', 'newgame', 'deck', 'door', 'jammed', 'savepoint', 'continue', 'heal', 'wish', 'cutscene', 'angela', 'pause']
ims = [Image.open(os.path.join(d, n + '.png')) for n in names if os.path.exists(os.path.join(d, n + '.png'))]
if ims:
    sheet = Image.new('RGB', (400 * 3, 480 * ((len(ims) + 2) // 3)))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % 3) * 400, (i // 3) * 480))
    sheet.save(os.path.join(d, 'sheet.png'))
EOF
exit $fail
