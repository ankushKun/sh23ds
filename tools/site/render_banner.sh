#!/bin/sh
# Renders tools/site/banner.html to site/assets/banner.jpg (1200 x 630) with headless Chrome.
#   tools/site/render_banner.sh [path to Chrome]
set -e
here="$(cd "$(dirname "$0")" && pwd)"
chrome="${1:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
tmp="$(mktemp -d)"
"$chrome" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
  --window-size=1200,630 --virtual-time-budget=4000 --screenshot="$tmp/banner.png" \
  "file://$here/banner.html" 2>/dev/null
if command -v sips >/dev/null; then
  sips -s format jpeg -s formatOptions 88 "$tmp/banner.png" --out "$here/../../site/assets/banner.jpg" >/dev/null
else
  python3 -c "from PIL import Image; Image.open('$tmp/banner.png').convert('RGB').save('$here/../../site/assets/banner.jpg', quality=88)"
fi
rm -rf "$tmp"
echo "wrote site/assets/banner.jpg"
