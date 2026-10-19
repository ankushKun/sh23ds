#!/bin/sh
# Puts a build on a 3DS over Wi-Fi and gets its logs back (port/TESTING.md, "On a console").
#
#   tools/console/deploy.sh link             send port/sh23ds.3dsx to the Homebrew Launcher's
#                                            netloader (press Y in the launcher first) and run it
#   tools/console/deploy.sh put <file> [dir] upload a file over FTP (default dir /3ds/sh23ds)
#   tools/console/deploy.sh pull [dir]       download perf.txt, ptlog.txt, options.txt and any
#                                            frame_*.bmp from the pack folder into dir
#                                            (default .lr3ds/console/<date-time>)
#   tools/console/deploy.sh ls               list the pack folder
#
# N3DS_IP is the console's address (the launcher's netloader and ftpd both show it); FTP uses
# N3DS_FTP_PORT (5000, ftpd's default). 3dslink runs in the devkitARM image, as the build does.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IP=${N3DS_IP:?set N3DS_IP to the console address}
PORT=${N3DS_FTP_PORT:-5000}
IMAGE=${SH23DS_BUILD_IMAGE:-devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5}
PACK=/3ds/sh23ds

case "$1" in
link)
	docker run --rm -v "$ROOT/port":/port "$IMAGE" /opt/devkitpro/tools/bin/3dslink -a "$IP" /port/sh23ds.3dsx
	;;
put)
	[ -f "$2" ] || { echo "no such file: $2" >&2; exit 2; }
	curl -sS --ftp-create-dirs -T "$2" "ftp://$IP:$PORT${3:-$PACK}/$(basename "$2")"
	echo "sent $(basename "$2") to ${3:-$PACK}"
	;;
pull)
	OUT=${2:-$ROOT/.lr3ds/console/$(date +%Y%m%d-%H%M%S)}
	mkdir -p "$OUT"
	LIST=$(curl -sS --list-only "ftp://$IP:$PORT$PACK/")
	for f in $LIST; do
		case "$f" in
		perf.txt|ptlog.txt|options.txt|controls.txt|frame_*.bmp|frame_*.txt)
			curl -sS "ftp://$IP:$PORT$PACK/$f" -o "$OUT/$f" && echo "got $f" ;;
		esac
	done
	echo "in $OUT"
	;;
ls)
	curl -sS --list-only "ftp://$IP:$PORT$PACK/"
	;;
*)
	sed -n '2,13p' "$0"
	exit 2
	;;
esac
