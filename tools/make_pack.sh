#!/bin/sh
# Builds the sh23ds SD card pack from your own Silent Hill 2 PC Director's Cut install.
#
#   tools/make_pack.sh "<Silent Hill 2 folder>" [output dir, default ./pack]
#
# Needs Python 3 with numpy and Pillow (in .venv, or set PY), ffmpeg (it decodes the Bink movies)
# and tex3ds (from devkitPro if installed, else run through Docker with devkitpro/devkitarm; with Colima the
# output folder must then be under your home folder). The packer image (tools/packer) bundles all of it.
# SH2_TEXENC=native encodes the textures with the browser packer's encoder built for this machine
# (tools/texenc/build.sh native), on every core, with no tex3ds or Docker.
# Copy the result to the SD card as sdmc:/3ds/sh23ds/ (the png/ folders and *.json files are not
# needed on the card). Real consoles also need sdmc:/3ds/dspfirm.cdc (dump it with DSP1) for sound.
#
# Silent Hill 2: Enhanced Edition: when the game folder has the EE's sh2e folder (its Essential Files,
# Image, FMV and Audio Enhancement Packs), every converter reads the files there in place of data/'s,
# file by file, as the EE does in the PC game (tools/sh2conv/ee.py, EE_PACKS.md). SH2EE=0 ignores the
# folder, SH2EE_SKIP=images,fmv,audio,essentials leaves out a part. The files used are listed in
# ee_used.txt in the output folder. Build into an empty folder after adding or removing packs.
# The step list itself is tools/sh2conv/pack.py, which the browser packer runs too.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME=${1:?usage: make_pack.sh <Silent Hill 2 folder> [out dir]}
OUT=${2:-$ROOT/pack}
# PY: the Python to run the converters with (the packer image sets it); else .venv, else python3
if [ -z "$PY" ]; then
	if [ -x "$ROOT/.venv/bin/python" ]; then PY="$ROOT/.venv/bin/python -I"; else PY="python3 -I"; fi
fi
shift; [ $# -gt 0 ] && shift
exec $PY "$ROOT/tools/sh2conv/pack.py" "$GAME" "$OUT" "$@"
