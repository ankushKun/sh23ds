#!/bin/sh
# Builds the sh23ds SD card pack from your own Silent Hill 2 PC Director's Cut install.
#
#   tools/make_pack.sh "<Silent Hill 2 folder>" [output dir, default ./pack]
#
# Needs Python 3 with numpy, Pillow and pefile (in .venv, or set PY), ffmpeg (it decodes the Bink movies)
# and tex3ds (from devkitPro if installed, else run through Docker with devkitpro/devkitarm; with Colima the
# output folder must then be under your home folder). The packer image (tools/packer) bundles all of it.
# Copy the result to the SD card as sdmc:/3ds/sh23ds/ (the png/ folders and *.json files are not
# needed on the card). Real consoles also need sdmc:/3ds/dspfirm.cdc (dump it with DSP1) for sound.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME=${1:?usage: make_pack.sh <Silent Hill 2 folder> [out dir]}
OUT=${2:-$ROOT/pack}
# PY: the Python to run the converters with (the packer image sets it); else .venv, else python3
if [ -z "$PY" ]; then
	if [ -x "$ROOT/.venv/bin/python" ]; then PY="$ROOT/.venv/bin/python -I"; else PY="python3 -I"; fi
fi
DATA="$GAME/data"

# Only the Director's Cut exe is supported (its addresses are baked into the event decoder).
SUM=$(md5 -q "$GAME/sh2pc.exe" 2>/dev/null || md5sum "$GAME/sh2pc.exe" | cut -d' ' -f1)
if [ "$SUM" != "f3f5360298b6d0f6f2e09cf24a0925d1" ]; then
	echo "sh2pc.exe is not the Director's Cut build this port expects (md5 $SUM)" >&2
	exit 1
fi

mkdir -p "$OUT"
echo "== rooms (geometry, textures)"
$PY "$ROOT/tools/sh2conv/convert.py" "$DATA" "$OUT"
$PY "$ROOT/tools/sh2conv/convert.py" "$DATA" "$OUT" --bg bg2
echo "== collision"
$PY "$ROOT/tools/sh2conv/collision.py" "$DATA" "$OUT"
$PY "$ROOT/tools/sh2conv/collision.py" "$DATA" "$OUT" --bg bg2
echo "== James"
$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" jms hll_jms.mdl jms_basic1.anm
# animation clips inside jms_basic1 (frame ranges, loop flag)
printf 'idle 0 50 1\nwalk 51 91 1\nrun 92 170 1\n' > "$OUT/chr/jms/clips.txt"
echo "== enemies and weapons (Lying Figure, Mannequin, wooden plank, James' plank moves)"
$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" jms hll_jms.mdl jms_basic1.anm jms_wpkaku.anm jms_wpnone.anm \
	jms_wphand.anm jms_wpshot.anm jms_wprifl.anm
$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" scu scu.mdl scu.anm
$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" mkn mkn.mdl mkn.anm
$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" wp wp_kakuzai.mdl
for c in "nse nse.mdl nse.anm" "xoo xoo.mdl xoo.anm" "tyu tyu.mdl tyu.anm" "red red.mdl red.anm" \
         "mar lll_mar.mdl lll_mar.anm" "pap pap.mdl pap.anm" "oni oni.mdl oni.anm" "bos bos.mdl bos.anm" \
         "arm arm.mdl arm.anm" "ike ike.mdl ike.anm" "edi lll_edi.mdl lll_edi.anm" \
         "wp wp_handgun.mdl" "wp wp_shotgun.mdl" "wp wp_riflgun.mdl"; do
	$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" $c
done
# Mary's moths (camera-facing quads with the PC's wing-beat texture) and the creeper nests
$PY "$ROOT/tools/sh2conv/moths.py" "$DATA" "$OUT"
$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" item ty2.mdl
$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" item ty3.mdl
# Maria as the Born From a Wish player: her models live in data/chr2, which chr.py reads through a
# folder of links laid out as chr/xmar
X=$(mktemp -d); mkdir -p "$X/chr/xmar"
for f in lxx_mar.mdl xmar_wpnone.anm xmar_wpcolt.anm xmar_wpknif.anm; do ln -s "$DATA/chr2/mar/$f" "$X/chr/xmar/"; done
for f in wp_colt.mdl wp_chinanife.mdl; do ln -s "$DATA/chr2/wp/$f" "$X/chr/xmar/"; done
$PY "$ROOT/tools/sh2conv/chr.py" "$X" "$OUT" xmar lxx_mar.mdl xmar_wpnone.anm xmar_wpcolt.anm xmar_wpknif.anm
$PY "$ROOT/tools/sh2conv/chr.py" "$X" "$OUT" xmar wp_colt.mdl
$PY "$ROOT/tools/sh2conv/chr.py" "$X" "$OUT" xmar wp_chinanife.mdl
rm -rf "$X"
echo "== stage events and messages"
$PY "$ROOT/tools/sh2conv/events.py" --exe "$GAME/sh2pc.exe" --data "$DATA" --out "$OUT/events"
$PY "$ROOT/tools/sh2conv/events_pack.py" "$DATA" "$OUT"
$PY "$ROOT/tools/sh2conv/enemies_pack.py" "$OUT"
echo "== room lighting and fog (DrawEnvData records in the exe)"
$PY "$ROOT/tools/sh2conv/lighting.py" --exe "$GAME/sh2pc.exe" --pack "$OUT"
echo "== fog particle layer (.fcl areas, fog texture)"
$PY "$ROOT/tools/sh2conv/fog.py" "$DATA" "$OUT"
echo "== title art"
$PY "$ROOT/tools/sh2conv/ui.py" --exe "$GAME/sh2pc.exe" "$DATA" "$OUT"
echo "== stills for the stage programs (data/pic)"
$PY "$ROOT/tools/sh2conv/pics.py" "$DATA" "$OUT"
echo "== maps and memos"
$PY "$ROOT/tools/sh2conv/maps.py" "$DATA" "$OUT"
echo "== music"
mkdir -p "$OUT/sound"
cp -R "$DATA/sound/adx" "$OUT/sound/"
echo "== movies (MPEG-1 + MP2 for movie.c; needs ffmpeg)"
$PY "$ROOT/tools/sh2conv/movies.py" --data "$DATA" --out "$OUT"
echo "== sound effects, footstep materials, per-stage BGM tables"
$PY "$ROOT/tools/sh2conv/sound.py" --data "$DATA" --exe "$GAME/sh2pc.exe" --out "$OUT/sound"
echo "== cutscenes (scripts, motions, the characters they need, voices from voice.afs)"
$PY "$ROOT/tools/sh2conv/demos.py" --data "$DATA" --exe "$GAME/sh2pc.exe" --out "$OUT"
echo "== character shadow models (.kg1 next to the S3C files) and per-area room indexes"
$PY "$ROOT/tools/sh2conv/shadows.py" "$DATA" "$OUT"
$PY "$ROOT/tools/sh2conv/areaindex.py" "$OUT"
echo "done: $OUT"
