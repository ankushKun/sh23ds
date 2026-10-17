#!/bin/sh
# Packages sh23ds.cia from sh23ds.elf (run ./build.sh first) and collects the release files in dist/
# with their SHA-256 sums. bannertool and makerom run in a small container built from
# cia-tools.Dockerfile, whose downloads are pinned by checksum.
set -e
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
VERSION=$(tr -d '[:space:]' < "$ROOT/VERSION")
MAJOR=$(echo "$VERSION" | cut -d. -f1); MINOR=$(echo "$VERSION" | cut -d. -f2); MICRO=$(echo "$VERSION" | cut -d. -f3)
IMAGE=${SH23DS_CIA_IMAGE:-sh23ds-cia-tools:1.0.0}
[ -s sh23ds.elf ] || { echo "sh23ds.elf is missing: run ./build.sh first" >&2; exit 1; }

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	docker build --platform linux/amd64 -f cia-tools.Dockerfile -t "$IMAGE" .
fi
tool() { docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -v "$PWD":/w -w /w "$IMAGE" "$@"; }

mkdir -p build
tool bannertool makebanner -i meta/banner.png -a meta/banner.wav -o build/banner.bnr
tool bannertool makesmdh -s "Silent Hill 2 (sh23ds)" -l "Native New 3DS port, uses your own game data" \
	-p "ankushKun" -i meta/icon.png -f visible,nosavebackups -o build/icon.smdh
tool makerom -f cia -o sh23ds.cia -elf sh23ds.elf -rsf meta/template.rsf \
	-icon build/icon.smdh -banner build/banner.bnr -exefslogo -target t \
	-major "$MAJOR" -minor "$MINOR" -micro "$MICRO" \
	-DAPP_TITLE=sh23ds -DAPP_PRODUCT_CODE=CTR-H-SH23 -DAPP_UNIQUE_ID=0xF5D23 \
	-DAPP_ROMFS=meta/romfs -DAPP_CATEGORY=Application -DAPP_USE_ON_SD=true -DAPP_ENCRYPTED=false \
	-DAPP_MEMORY_TYPE=Application -DAPP_SYSTEM_MODE=64MB -DAPP_SYSTEM_MODE_EXT=124MB \
	-DAPP_CPU_SPEED=804MHz -DAPP_ENABLE_L2_CACHE=true -DAPP_VERSION_MAJOR="$MAJOR"

mkdir -p "$ROOT/dist"
cp sh23ds.3dsx sh23ds.cia "$ROOT/dist/"
cd "$ROOT/dist"
if command -v sha256sum >/dev/null 2>&1; then sha256sum sh23ds.3dsx sh23ds.cia > sh23ds.sha256
else shasum -a 256 sh23ds.3dsx sh23ds.cia > sh23ds.sha256; fi
echo "sh23ds $VERSION:"; ls -l sh23ds.3dsx sh23ds.cia; cat sh23ds.sha256
