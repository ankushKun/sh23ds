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
#
# Silent Hill 2: Enhanced Edition: when the game folder has the EE's sh2e folder (its Essential Files,
# Image, FMV and Audio Enhancement Packs), every converter reads the files there in place of data/'s,
# file by file, as the EE does in the PC game (tools/sh2conv/ee.py, EE_PACKS.md). SH2EE=0 ignores the
# folder, SH2EE_SKIP=images,fmv,audio,essentials leaves out a part. The files used are listed in
# ee_used.txt in the output folder. Build into an empty folder after adding or removing packs.
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
$PY "$ROOT/tools/sh2conv/ee.py" report "$DATA"
export SH2EE_LOG="$OUT/ee_used.txt"
# Each step leaves a stamp in $OUT/.steps when it finishes, so running the same command again after a
# failure picks up at the step that stopped. FRESH=1 starts over. The stamps are removed at the end.
STEPS="$OUT/.steps"
[ "${FRESH:-0}" = 1 ] && rm -rf "$STEPS"
mkdir -p "$STEPS"
step() {   # step <id> <title>: false (skip) when <id> finished in an earlier run
	if [ -f "$STEPS/$1" ]; then echo "== $2 (already done, skipped)"; return 1; fi
	echo "== $2"
}
if step rooms "rooms (geometry, textures)"; then
	$PY "$ROOT/tools/sh2conv/convert.py" "$DATA" "$OUT"
	$PY "$ROOT/tools/sh2conv/convert.py" "$DATA" "$OUT" --bg bg2
	touch "$STEPS/rooms"
fi
if step collision "collision"; then
	$PY "$ROOT/tools/sh2conv/collision.py" "$DATA" "$OUT"
	$PY "$ROOT/tools/sh2conv/collision.py" "$DATA" "$OUT" --bg bg2
	touch "$STEPS/collision"
fi
if step james "James"; then
	$PY "$ROOT/tools/sh2conv/chr.py" "$DATA" "$OUT" jms hll_jms.mdl jms_basic1.anm
	# animation clips inside jms_basic1 (frame ranges, loop flag)
	printf 'idle 0 50 1\nwalk 51 91 1\nrun 92 170 1\n' > "$OUT/chr/jms/clips.txt"
	touch "$STEPS/james"
fi
if step characters "enemies and weapons (Lying Figure, Mannequin, wooden plank, James' plank moves)"; then
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
	touch "$STEPS/characters"
fi
if step events "stage events and messages"; then
	$PY "$ROOT/tools/sh2conv/events.py" --exe "$GAME/sh2pc.exe" --data "$DATA" --out "$OUT/events"
	$PY "$ROOT/tools/sh2conv/events_pack.py" "$DATA" "$OUT"
	$PY "$ROOT/tools/sh2conv/enemies_pack.py" "$OUT"
	touch "$STEPS/events"
fi
if step lighting "room lighting and fog (DrawEnvData records in the exe)"; then
	$PY "$ROOT/tools/sh2conv/lighting.py" --exe "$GAME/sh2pc.exe" --pack "$OUT"
	touch "$STEPS/lighting"
fi
if step fog "fog particle layer (.fcl areas, fog texture)"; then
	$PY "$ROOT/tools/sh2conv/fog.py" "$DATA" "$OUT" "$GAME/sh2pc.exe"
	touch "$STEPS/fog"
fi
if step ui "title art"; then
	$PY "$ROOT/tools/sh2conv/ui.py" --exe "$GAME/sh2pc.exe" "$DATA" "$OUT"
	touch "$STEPS/ui"
fi
if step pics "stills for the stage programs (data/pic)"; then
	$PY "$ROOT/tools/sh2conv/pics.py" "$DATA" "$OUT" --exe "$GAME/sh2pc.exe"
	touch "$STEPS/pics"
fi
if step maps "maps and memos"; then
	$PY "$ROOT/tools/sh2conv/maps.py" "$DATA" "$OUT" --exe "$GAME/sh2pc.exe"
	touch "$STEPS/maps"
fi
if step music "music"; then
	# the BGM (the Audio Enhancement Pack's when there); voice/ is left out: the cutscene voices are
	# taken from voice.afs by the demos step and the effects in it by the sound step
	$PY "$ROOT/tools/sh2conv/ee.py" copytree "$DATA" sound/adx "$OUT/sound/adx" voice
	touch "$STEPS/music"
fi
if step movies "movies (MPEG-1 + MP2 for movie.c; needs ffmpeg)"; then
	$PY "$ROOT/tools/sh2conv/movies.py" --data "$DATA" --out "$OUT"
	touch "$STEPS/movies"
fi
if step sound "sound effects, footstep materials, per-stage BGM tables"; then
	$PY "$ROOT/tools/sh2conv/sound.py" --data "$DATA" --exe "$GAME/sh2pc.exe" --out "$OUT/sound"
	touch "$STEPS/sound"
fi
if step demos "cutscenes (scripts, motions, the characters they need, voices from voice.afs)"; then
	$PY "$ROOT/tools/sh2conv/demos.py" --data "$DATA" --exe "$GAME/sh2pc.exe" --out "$OUT"
	touch "$STEPS/demos"
fi
if step stagemodels "stage-placed models (cars, furniture, props, puzzle objects)"; then
	$PY "$ROOT/tools/sh2conv/stagemodels.py" --data "$DATA" --exe "$GAME/sh2pc.exe" --out "$OUT" --events "$OUT/events/events.json"
	touch "$STEPS/stagemodels"
fi
if step shadows "character shadow models (.kg1 next to the S3C files) and per-area room indexes"; then
	$PY "$ROOT/tools/sh2conv/shadows.py" "$DATA" "$OUT"
	$PY "$ROOT/tools/sh2conv/areaindex.py" "$OUT"
	touch "$STEPS/shadows"
fi
rm -rf "$STEPS"
if [ -s "$SH2EE_LOG" ]; then
	sort -u "$SH2EE_LOG" -o "$SH2EE_LOG"
	echo "Enhanced Edition files used: $(wc -l < "$SH2EE_LOG" | tr -d ' ') (see $SH2EE_LOG)"
fi
echo "done: $OUT"
