#!/bin/sh
# Renders tools/site/banner.html to the social cards in site/assets/og/ (1200 x 630) with headless
# Chrome, copies the home card to site/assets/banner.jpg for the README, and renders GitHub's
# 1280 x 640 social preview to .github/social-preview.jpg (upload it in the repository's settings).
#   tools/site/render_banner.sh [path to Chrome]
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../../site/assets/og"
chrome="${1:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
tmp="$(mktemp -d)"
mkdir -p "$out"
for page in home install packer roadmap docs; do
  "$chrome" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
    --window-size=1200,630 --virtual-time-budget=4000 --screenshot="$tmp/$page.png" \
    "file://$here/banner.html#$page" 2>/dev/null
  if command -v sips >/dev/null; then
    sips -s format jpeg -s formatOptions 86 "$tmp/$page.png" --out "$out/$page.jpg" >/dev/null
  else
    python3 -c "from PIL import Image; Image.open('$tmp/$page.png').convert('RGB').save('$out/$page.jpg', quality=86)"
  fi
  echo "wrote site/assets/og/$page.jpg"
done
cp "$out/home.jpg" "$here/../../site/assets/banner.jpg"
echo "wrote site/assets/banner.jpg"
"$chrome" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
  --window-size=1280,640 --virtual-time-budget=4000 --screenshot="$tmp/github.png" \
  "file://$here/banner.html#github" 2>/dev/null
if command -v sips >/dev/null; then
  sips -s format jpeg -s formatOptions 86 "$tmp/github.png" --out "$here/../../.github/social-preview.jpg" >/dev/null
else
  python3 -c "from PIL import Image; Image.open('$tmp/github.png').convert('RGB').save('$here/../../.github/social-preview.jpg', quality=86)"
fi
echo "wrote .github/social-preview.jpg"
rm -rf "$tmp"
