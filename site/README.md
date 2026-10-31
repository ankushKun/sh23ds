# Website

The static site published at https://ankush.one/sh23ds/ by `.github/workflows/pages.yml` whenever
`site/` changes on `main`. There is no build step. To preview it, run `python3 -m http.server -d site`
and open http://localhost:8000. (`404.html` sets `<base href="/sh23ds/">` for GitHub Pages, so it only
looks right when the site is served under `/sh23ds/`.)

| File | What it is |
| --- | --- |
| `index.html` | The site: the title screen, the video, the install in three steps with the browser packer built into step 1 (`#pack`; Options, Docker and Help folded under it), and questions |
| `install.html`, `packer.html`, `roadmap.html`, `status.html`, `docs.html` | Redirects for old links. The in-game setup screen's QR code points at `install.html`, so it stays. The install's old `#step-N` anchors land on the matching step, `packer.html` keeps its query (`?simd=0`), the roadmap goes to `port/STATUS.md` and the docs to the README on GitHub |
| `404.html` | "There was a hole here." GitHub Pages serves it for any missing page |
| `site.css`, `site.js` | The stylesheet and script. The page works without the script |
| `packer/` | The browser packer: `page.js` and `check.js` run in the page, the rest in workers. `tools/packer-web/README.md` has the details |
| `fonts/` | Libre Caslon Display (the big titles in capitals and the step numerals), Inter (everything you read, in sentence case), IBM Plex Mono (commands and values), VT323 (only the video's VCR display and the wordmark). Latin subsets, SIL Open Font License (`OFL-*.txt`) |
| `qr/` | FBI install codes pointing at `releases/latest/download/...`; `tools/make_site_qr.py` regenerates them |
| `assets/shots/` | Screenshots of the port, built by `tools/site/make_shots.py` from captures and the mapping in `tools/site/shots.txt` |
| `assets/og/` | Social cards (1200 x 630) rendered by `tools/site/render_banner.sh` from `tools/site/banner.html`; the page uses `home.jpg` |
| `assets/banner.jpg` | The README banner, a copy of `assets/og/home.jpg` |
| `sitemap.xml` | The page with its images. `https://ankush.one/robots.txt` (the root site) points at it |
| `llms.txt` | A plain summary and link list for AI assistants and crawlers ([llmstxt.org](https://llmstxt.org)) |
| `site.webmanifest`, `assets/icon-*.png` | The manifest and the nine squares at 192 and 512 px |

## How it is put together

One page, read top to bottom. The title screen is the 2001 game's own, over a loop cut from the PC game's
opening movies, with the GitHub mark in the corner and no menu. The video follows, then the install: three
steps, each a big numeral beside its title, one line and what to do. Step 1 is the packer itself; the
browser check (`check.js`) runs only once the visitor scrolls near it, since it starts a worker and
touches the CDN, and it shows nothing unless something is wrong. Film grain and scanlines sit over
everything.

Support (`#funds`) is pinned to the left edge below the brand: Buy Me a Coffee, GitHub Sponsors and
Ko-fi, the accounts in `.github/FUNDING.yml`, as the game's menu, with the red bar on the item under the
cursor and on the first until another is chosen. From 1660 px the margin beside the page fits it, so it
stands there open; narrower, it waits behind a Support tab that hover or a tap slides out. The marks are
Simple Icons (CC0).

Keep it short. Every line on the page should be something a player needs to get the game running;
details belong in the folds under step 1 or in the repository. The mark is the nine red squares from
the last wall in the game. Every colour comes from the tokens at the top of `site.css`; the one accent
is the red of the save point. The grain is drawn by the browser from an SVG filter in those tokens, so
there are no texture images. Motion stops under `prefers-reduced-motion`.

When the install steps change, update `index.html` and the README together. The page's `<head>` carries
its title, description, social card and JSON-LD (the game, its source code, the install as a HowTo, the
packer and the questions). On a release, bump `softwareVersion` and `dateModified` in `index.html`, the
`data-version` tags (under the title and in the footer), the facts in `llms.txt` and the `lastmod` date
in `sitemap.xml`. Questions added to the page go in its FAQPage JSON-LD too. The release version and
file sizes come from the GitHub API at load time; the tags in the page are what shows until it answers.

## Analytics

`site.js` starts with the same PostHog setup as the rest of ankush.one: the same project, sent through
the first-party proxy at `p1.ankush.one`, with autocapture, heatmaps, Web Vitals, error capture and
session replay (console and network calls included). Every event carries `site: "sh23ds"`, and since the
pages share the ankush.one origin, a visitor keeps one id and one replay session across this site, the
portfolio and the other project pages. Nothing loads on localhost, so previews and the packer's e2e test
send nothing, or when the browser sends Global Privacy Control.

Custom events go through `window.track(name, props)`: `install_os_changed`, `command_copied` and
`video_played` from `site.js`, and `packer_browser` (the
browser check: the browser's name and the ids of what failed), `packer_folder`, `packer_started`,
`packer_finished`, `packer_failed` (also sent to error tracking) and `packer_zip_saved` from
`packer/page.js`. The packer's events carry counts, sizes and the chosen options, never file names or
contents. What the site sends is listed on https://ankush.one/privacy/, so update that page when adding
an event.
