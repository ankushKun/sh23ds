# Website

The static site published at https://ankush.one/sh23ds/ by `.github/workflows/pages.yml` whenever
`site/` changes on `main`. There is no build step. To preview it, run `python3 -m http.server -d site`
and open http://localhost:8000.

| File | What it is |
| --- | --- |
| `index.html` | Home: the console with live screens, the feature viewer, the install card with the FBI code, status, questions |
| `install.html` | The install guide: five steps with saved progress and a command builder for the packer |
| `status.html` | What works, part by part, and how to report a console run |
| `docs.html` | Building, testing, debugging, the converters and the porting workflow, with a filter (press `/`) |
| `site.css`, `site.js` | One stylesheet and one script for every page. Each page works without the script |
| `fonts/` | M PLUS Rounded 1c (text and headings), DotGothic16 (labels and status), M PLUS 1 Code (code), Reenie Beanie (the handwritten notes). Latin subsets, SIL Open Font License (`OFL-*.txt`) |
| `qr/` | FBI install codes pointing at `releases/latest/download/...`; `tools/make_site_qr.py` regenerates them |
| `assets/shots/` | Screenshots of the port in Azahar, built by `tools/site/make_shots.py` from lr3ds captures and the mapping in `tools/site/shots.txt` |
| `assets/banner.jpg` | The social image and README banner, rendered from `tools/site/banner.html` by `tools/site/render_banner.sh` |

## How it is put together

The site borrows the 3DS HOME Menu: a status bar with the clock and battery, rounded cards, app tiles in
a dock that switch the feature viewer, a red pill for the current page. The Silent Hill 2 part is the map:
the wallpaper is grid paper, the notes beside the console are in James's red pen, and the tips in the
install guide are memo paper.

Every colour comes from the tokens at the top of `site.css`. There is one accent, the red of the
save-point square.

## Console photo

| File | Source | Author | Licence |
| --- | --- | --- | --- |
| `assets/new-3ds-angled.png` | [File:New Nintendo 3DS (weiß) 20250129 HOF2650 RAW-Export.png](https://commons.wikimedia.org/wiki/File:New_Nintendo_3DS_(wei%C3%9F)_20250129_HOF2650_RAW-Export.png), resized and cropped to the console (1356 × 1177) | PantheraLeo1359531 | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |

The footer carries the credit the licence asks for. The page projects the screenshots onto the photo's
screens with the `matrix3d()` values on `.device-stage` in `site.css`, which come from 3dsFM's
`scripts/screen-map`. If the photo is replaced, the matrices have to be recomputed.

When the version, the controls or the install steps change, update `install.html` and the README together.
The release version and file sizes on the pages come from the GitHub API at load time.
