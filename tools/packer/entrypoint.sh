#!/bin/sh
# Entry point of the packer image. Mount your game folder at /game (read-only is fine) and an empty
# folder at /out. The result is /out/3ds/sh23ds/, ready to copy to the root of the SD card.
# Set KEEP_EXTRAS=1 to keep the inspection files (bg/*/png folders and events/*.json, about 0.6 GB).
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
	find "$OUT/bg" -mindepth 2 -maxdepth 2 -type d -name png -exec rm -rf {} +
	find "$OUT/events" -name '*.json' -delete
fi
echo
echo "Done. Copy the 3ds folder inside your output folder to the root of the SD card."
du -sh "$OUT" 2>/dev/null || true
