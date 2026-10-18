# Website

The static site published at https://ankush.one/sh23ds/ by `.github/workflows/pages.yml` whenever
`site/` changes on `main`. There is no build step. To preview it, run `python3 -m http.server -d site`
and open http://localhost:8000. (`404.html` sets `<base href="/sh23ds/">` for GitHub Pages, so it only
looks right when the site is served under `/sh23ds/`.)

| File | What it is |
| --- | --- |
| `index.html` | Home: the title screen with its menu, the opening scene, the memo list of features, the save slots and FBI code, the results screen, questions |
| `install.html` | The install guide: five steps, with a command builder for the packer (system, game folder, copy buttons) |
| `roadmap.html` | Chapters done and ahead, every part's state on memo paper, and the release log |
| `docs.html` | How the port fits together, building, releases, the debug file, the converters and porting a feature |
| `404.html` | "There was a hole here." GitHub Pages serves it for any missing page |
| `status.html` | Sends old links on to `roadmap.html` |
| `site.css`, `site.js` | One stylesheet and one script for every page. Each page works without the script |
| `fonts/` | Libre Caslon Display (titles), Crimson Pro (text and menus), VT323 (system text and code). Latin subsets, SIL Open Font License (`OFL-*.txt`) |
| `qr/` | FBI install codes pointing at `releases/latest/download/...`; `tools/make_site_qr.py` regenerates them |
| `assets/shots/` | Screenshots of the port, built by `tools/site/make_shots.py` from captures and the mapping in `tools/site/shots.txt` |
| `assets/banner.jpg` | The social image and README banner, rendered from `tools/site/banner.html` by `tools/site/render_banner.sh` |

## How it is put together

The site is the 2001 game's own front end. Home opens on a title screen whose menu is the navigation, with
the red selection bar moving to whatever the pointer or keyboard is on, over a loop cut from the PC game's
opening movies. Features are memos on paper and what the port covers is the results screen. Film grain
and scanlines sit over every page.

On phones the install steps and the docs contents become a sticky strip under the page heading, and the
memo list becomes a row of tabs above the memo, so whatever you tap is on screen.

The mark is the nine red squares from the last wall in the game. Every colour comes from the tokens at
the top of `site.css`; the one accent is the red of the save point. The grain is drawn by the browser
from an SVG filter in those tokens, so there are no texture images. Motion stops under
`prefers-reduced-motion`.

When the version, the controls or the install steps change, update `install.html` and the README together.
The release version, file sizes and any release newer than the ones written into `roadmap.html` come
from the GitHub API at load time.
