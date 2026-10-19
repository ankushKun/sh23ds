#!/usr/bin/env bash
# Draws every picture of the HOME Menu art, so the icon, the banner's textures and the flat
# banner all come from code in this folder:
#   textures.py          -> rust, fog and port/meta/icon.png (48 x 48, the HOME Menu icon)
#   logo.html            -> the SILENT HILL 2 wordmark
#                           atlas.png (256 x 256: wordmark on top, rust and fog below)
#   model.py             -> banner.glb
#   render.html?flat     -> port/meta/banner.png (the flat banner, used with SH23DS_FLAT_BANNER=1)
#   render.html?frames   -> six frames of the loop, to look at (path printed at the end)
#
# Needs Google Chrome (headless), network access for three.js, and the repo's .venv (numpy,
# Pillow). Run make.sh afterwards to rebuild the jingle and the CGFX.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
META="${ROOT}/port/meta"
PY="${ROOT}/.venv/bin/python"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
OUT="${BANNER_OUT:-$(mktemp -d)}"
mkdir -p "${OUT}"

shoot() {
    "${CHROME}" --headless=new --use-angle=swiftshader --enable-unsafe-swiftshader \
        --hide-scrollbars --allow-file-access-from-files --virtual-time-budget=20000 "$@" 2>/dev/null
}

printf '==> Textures and icon\n'
"${PY}" -I "${HERE}/textures.py" "${OUT}"
cp "${OUT}/icon.png" "${META}/icon.png"

printf '==> Wordmark and atlas\n'
shoot --default-background-color=00000000 --window-size=1024,512 --screenshot="${OUT}/logo.png" "file://${HERE}/logo.html"
"${PY}" -I - "${OUT}" "${HERE}/atlas.png" <<'PYEOF'
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
atlas = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
logo = Image.open(src + "/logo.png").convert("RGBA").resize((256, 128), Image.LANCZOS)
a = logo.getchannel("A").point(lambda v: 255 if v >= 128 else 0)   # the cut-out the 3DS will make
logo.putalpha(a)
atlas.paste(logo, (0, 0))
atlas.paste(Image.open(src + "/rust.png").convert("RGBA"), (0, 128))
atlas.paste(Image.open(src + "/fog.png").convert("RGBA"), (128, 128))
atlas.save(dst)
PYEOF

printf '==> Model\n'
"${PY}" -I "${HERE}/model.py" "${HERE}/atlas.png" "${HERE}/banner.glb"

printf '==> Flat banner and preview frames\n'
shoot --window-size=512,256 --screenshot="${OUT}/flat.png" "file://${HERE}/render.html?mode=flat"
sips -z 128 256 "${OUT}/flat.png" --out "${META}/banner.png" >/dev/null
shoot --window-size=1232,504 --screenshot="${OUT}/frames.png" "file://${HERE}/render.html?mode=frames"

printf '\nDone. Preview: %s/frames.png. Now run make.sh to rebuild the jingle and the CGFX.\n' "${OUT}"
