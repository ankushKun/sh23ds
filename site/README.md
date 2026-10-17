# Website

The static site published at https://ankush.one/sh23ds/ by `.github/workflows/pages.yml` whenever
`site/` changes on `main`. To preview it, run `python3 -m http.server -d site` and open
http://localhost:8000.

- `index.html`, `styles.css`, `app.js`: one page, dark and quiet after the Enhanced Edition's site. It
  works without JavaScript. `app.js` maps the screenshots onto the console photo, switches the tabs,
  copies commands, opens a screenshot with both screens, fades sections in, and shows the latest
  release's version and file sizes from the GitHub API.
- `qr/`: the FBI install codes. They point at `releases/latest/download/...`, so they stay valid for
  every release. `tools/make_site_qr.py` regenerates them if the repository or asset names change.
- `fonts/`: Cormorant Garamond (headings), Barlow (text), Questrial (labels) and JetBrains Mono (code),
  Latin subsets, all under the SIL Open Font License (`OFL-*.txt`).
- `assets/shots/`: screenshots of the port running in Azahar at the console's native size, each as a
  top screen (400 × 240) and a touch screen (320 × 240).
- `assets/`: the icons (the red square), the social image `banner.jpg` (also the README banner), and
  the console photo below.

## Console photo

| File | Source | Author | Licence |
| --- | --- | --- | --- |
| `assets/new-3ds-angled.png` | [File:New Nintendo 3DS (weiß) 20250129 HOF2650 RAW-Export.png](https://commons.wikimedia.org/wiki/File:New_Nintendo_3DS_(wei%C3%9F)_20250129_HOF2650_RAW-Export.png), resized and cropped to the console (1356 × 1177) | PantheraLeo1359531 | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |

The footer carries the credit the licence asks for. The page darkens the photo with a CSS filter and
projects the screenshots onto its screens with the `matrix3d()` values on `.device-stage` in
`styles.css`, which come from 3dsFM's `scripts/screen-map`. If the photo is replaced, the matrices have
to be recomputed.
