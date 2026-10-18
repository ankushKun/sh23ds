#!/bin/sh
# Entry point of the packer image. Mount your game folder at /game (read-only is fine) and an empty
# folder at /out. The result is /out/3ds/sh23ds/, ready to copy to the root of the SD card.
# Set KEEP_EXTRAS=1 to keep the inspection files (the png folders and the .json files, about 0.6 GB).
# If a run stops partway, the same command picks up where it stopped; set FRESH=1 to start over.
# Set HALF_MOVIES=1 to add the half-size movies the Old 3DS profile plays (about 75 MB more).
# A game folder with the Enhanced Edition's sh2e folder (its Image, FMV and Audio Enhancement Packs
# and Essential Files) gives the improved files (tools/sh2conv/EE_PACKS.md); SH2EE=0 ignores them.
set -e
if [ ! -f /game/sh2pc.exe ] || [ ! -d /game/data ]; then
	echo "Mount your Silent Hill 2 folder at /game: it must hold sh2pc.exe and the data folder." >&2
	echo '  docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/sd:/out" ghcr.io/ankushkun/sh23ds-packer' >&2
	exit 2
fi
OUT=/out/3ds/sh23ds
echo "sh23ds packer $(cat /opt/sh23ds/VERSION), writing $OUT"
/opt/sh23ds/tools/make_pack.sh /game "$OUT"
if [ "${KEEP_EXTRAS:-0}" != "1" ]; then
	find "$OUT" -type d -name png -prune -exec rm -rf {} +
	find "$OUT" -name '*.json' -delete
fi
echo
echo "Done. Copy the 3ds folder inside your output folder to the root of the SD card."
du -sh "$OUT" 2>/dev/null || true
