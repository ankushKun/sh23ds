#!/bin/sh
# Runs port/sh23ds.3dsx headlessly in Azahar's libretro core with the strict settings used for testing.
# Usage: tools/lr3ds/run.sh <seconds timeout> [lr3ds options...]
# AZAHAR_CORE points at azahar_libretro.dylib (Azahar's libretro build for macOS).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORE=${AZAHAR_CORE:-$HOME/Developer/tools/azahar/azahar-libretro-macos-arm64-2126.2/azahar_libretro.dylib}
[ -f "$CORE" ] || { echo "Azahar libretro core not found at $CORE (set AZAHAR_CORE)" >&2; exit 2; }
[ -x "$ROOT/tools/lr3ds/lr3ds" ] || { echo "Build lr3ds first: clang -O2 -Wall -o tools/lr3ds/lr3ds tools/lr3ds/lr3ds.c -framework OpenGL" >&2; exit 2; }
T=$1; shift
exec perl -e 'alarm shift; exec @ARGV' "$T" "$ROOT/tools/lr3ds/lr3ds" "$CORE" "$ROOT/port/sh23ds.3dsx" \
  --save "${LR3DS_SAVE:-$ROOT/.lr3ds/save}" --sys "$ROOT/.lr3ds/sys" \
  --opt citra_graphics_api=Software --opt 'citra_is_new_3ds=New 3DS' "$@"
