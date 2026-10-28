# The browser packer

`site/packer.html` builds the SD card data in the player's browser from their own game folder, the way
the Docker packer does, with nothing uploaded. It runs the same converters: `tools/sh2conv/pack.py` is
the step list for both, and the converters reach outside Python only through `tools/sh2conv/host.py`.

## How it fits together

| Piece | Where | What it does |
| --- | --- | --- |
| Page | `site/packer.html`, `site/packer/page.js`, `packer.css` | Picks the game folder (the File System Access picker in Chromium, a folder input elsewhere), the options, progress, the download |
| Browser check | `site/packer/check.js`, `probe.js` | Runs when the page opens, before anything is picked: what the packer needs, with a message per browser when something is missing (below) |
| Coordinator | `site/packer/page.js` | Starts the writer and one to four packing workers, hands out `pack.py`'s tasks, copies files between workers when a task needs them, adds up the progress |
| Packing workers | `site/packer/worker.js` | Module workers, each with its own Pyodide. Mount the picked files read-only with WORKERFS (each file is read when a task opens it), load ffmpeg.wasm only for a task that needs it, send their output to the writer over a MessageChannel |
| Engine | `site/packer/core.js` | Runs `pack.py`'s tasks in Pyodide, provides `sh2host` for `host.py`, hands each output file to the sink between tasks |
| Writer | `site/packer/writer.js`, `sinks.js` | The one place the output is written: a stored zip (zip64 past 4 GB; a path written twice keeps the last copy), straight into a file the player picked where the browser has `showSaveFilePicker` (Chromium), else in the browser's private storage (OPFS) for the page to save; or a folder the player picked, each file read back for its size |
| Textures | `tools/texenc/` → `site/packer/texenc.mjs` | tex3ds's pixel encoders and rg_etc1, built to WebAssembly; `tools/sh2conv/t3x.py` does the rest of what tex3ds does |
| Converters | `tools/packer-web/bundle.py` → `site/packer/sh2conv.zip` | The Python files and two data tables, built by the Pages workflow from the same commit |

Libraries come from jsDelivr at fixed versions (`site/packer/cdn.js`): Pyodide 314.0.7 (Python 3.14, numpy,
Pillow) and `@ffmpeg/core` 0.12.10 (single-threaded, so the site needs no cross-origin isolation headers).
The page and its files are static, so GitHub Pages serves it as is.

`@ffmpeg/core` is built with WebAssembly SIMD from 0.12.1 on, and browsers compile SIMD only on a processor
with SSE4.1: on an older one (a Core 2 before Penryn, a Phenom II) the first task that loads ffmpeg failed
with "Compiling function #81 failed: Wasm SIMD unsupported". Each worker compiles and calls a two-instruction
SIMD function first and takes 0.12.0, the last build without SIMD, when that fails (`worker.js`;
`packer.html?simd=0` forces it). 0.12.0 has no `ffprobe`, so `core.js` answers the one probe `movies.py`
makes from `ffmpeg -i`'s log. Pyodide, numpy, Pillow and `texenc.mjs` have no SIMD.

### The browser check

`check.js` runs when the page opens and fills the "This browser" panel in step 1. A finding is a stop
(the buttons stay off), a warning or a note, worded for the browser it runs in (Brave, Edge, Opera,
Vivaldi, Firefox, Safari, Chrome). On the page: a phone or tablet, a page not served over https, no way to
pick a folder, the storage quota for the zip, the screen wake lock, and Brave's switched-off File System
Access. Inside a module worker like the packing workers (`probe.js`): WebAssembly, and the legacy
exception handling Pyodide is built with (a 35-byte module with one try/catch, which validates exactly
where `pyodide.asm.wasm` does: V8 rejects both with legacy EH turned off), `FileReaderSync`, a file
written through a sync access handle in the browser's storage as `writer.js` does, and jsDelivr: the
Pyodide loader imported, the two big wasm files reached. When the zip can't be stored but the browser
can write into a folder, Save as offers Folder only.

Checked on 2026-10-21 in headless Chrome, normal and incognito (everything passes); with jsDelivr
mapped to a dead address, `--js-flags=--jitless` (WebAssembly gone, as Lockdown Mode and Edge's
enhanced security do) and `--js-flags=--no-experimental-wasm-legacy-eh` (each the right stop); as an
iPhone (the computer stop); and in Playwright's WebKit with an ephemeral context, where only the storage
fails, as in a Safari private window. A pack from the page still runs (`e2e.mjs --only=collision,james`).

### host.py's three seams

- `host.run(argv)`: ffmpeg and ffprobe. In the browser `core.js` runs them synchronously in
  ffmpeg.wasm inside the same worker: game files are mounted into ffmpeg's file system as they are,
  other inputs are copied in, `pipe:` becomes a file, the output is copied back into Python's file system.
- `host.copy(src, dst)`: the music (740 MB) goes from the picked file straight into the zip, never
  through Python's memory.
- `convert.encode_textures` calls `t3x.encode_jobs` instead of tex3ds. Level 0 matches tex3ds byte for
  byte at rg_etc1's high quality ("Same as Docker" on the page); the default is medium, about 20 times
  faster at 0.2 dB lower PSNR. Mipmaps come from Pillow's Lanczos instead of ImageMagick's: the typical
  difference is under 1 of 255, and the browser's mipmaps lack two flaws of tex3ds's. ImageMagick resizes
  in floating point and its Lanczos overshoot past white wraps round to black when it becomes 8 bits,
  which leaves black outlines along bright edges, and it rings at the alpha edges of atlases, where
  Pillow resamples with premultiplied alpha.

### Memory

Python's file system lives in the worker's memory. After each step the engine sends every new file to
the sink and frees the kinds that no later step reads (`FREE_AFTER_SINK` in `core.js`, taken from a
`SH2PACK_TRACE` run of the native packer: `areaindex.py` reads the rooms' `.s3m` headers and `.cam`
files, `shadows.py` lists the `.s3c` files, and those stay). Freed files are cut to 0 bytes rather than
deleted, because converters skip a texture whose `.t3x` exists. PNG intermediates go as soon as their
step ends.

### Several workers at once

`pack.py tasks()` describes the steps as tasks for the browser. The steps that read each other's files
(James, the characters, events, the small steps, the cutscenes, the stage models, the shadows) are
'main' tasks and run in order in the first worker. The rooms, one task per area (the geometry, textures
and collision of that area), and the movies are 'any' tasks: the first free worker takes the longest one
waiting. The first worker joins in when its own list is done. Lighting needs every area's `rooms.txt`, so
it waits for the rooms and gets those files copied over from the other workers first, and each worker that
made rooms writes the area indexes for its own areas at the end. The Enhanced Edition logs (`ee_used.txt`)
of all workers are merged, and the writer adds `files.txt`, the pack's file list, last (as `pack.py`
does after a native run).

Each worker holds its own Python, a few hundred MB, so the page picks one to three by the computer's
cores and memory, and the Workers option overrides it. The per-area tasks write exactly what the rooms
step writes (`compare.py` checks the result against the native packer). The coordination happens between
tasks, never inside one, because a synchronous Pyodide call can't wait for another worker.

## Checks

```sh
npm install --prefix tools/packer-web          # Pyodide, ffmpeg.wasm and puppeteer-core, pinned
python3 tools/packer-web/bundle.py              # site/packer/sh2conv.zip
tools/texenc/build.sh native                    # build/libtexenc.so, for the texture checks

# textures: t3x.py + texenc against tex3ds, on a pack make_pack.sh built (its png folders kept)
.venv/bin/python tools/texenc/check.py pack

# the engine in Node on the real game folder, then against make_pack.sh's pack
node tools/packer-web/run.mjs "Silent Hill 2" /tmp/web-pack [--best] [--half]
.venv/bin/python tools/packer-web/compare.py /tmp/web-pack pack

# the whole page in headless Chrome: folder input, Start, the zip download
node tools/packer-web/e2e.mjs "Silent Hill 2" /tmp/web-zip [--only=collision,james] [--nosimd] [--savefile]
# the same against the live site (its analytics host is blocked)
node tools/packer-web/e2e.mjs "Silent Hill 2" /tmp/web-zip --url=https://ankush.one/sh23ds/packer.html
```

`compare.py` fails when a file is missing or a file differs in a way it doesn't expect. CI checks that
`tools/texenc/build.sh` rebuilds the committed `texenc.mjs` byte for byte.

### Changed exes, SIMD and the zip file (2026-10-27, Chrome for Testing on an 11-core Mac)

- The whole page on the Director's Cut (zip in the browser's storage, ffmpeg 0.12.10): 8 min 26 s, 5,757
  files. Again on a copy of the game whose `sh2pc.exe` has a byte of code and 64 KB of the SecuROM section
  changed, with `--savefile --nosimd` (the zip written into a picked file, ffmpeg 0.12.0): 8 min 06 s, the
  same 5,752 entries with the same CRCs except `movie/movies.txt`, where one duration reads 36.04 instead
  of 36.03 (`ffmpeg -i` prints hundredths). `?simd=0` loads `@ffmpeg/core@0.12.0` and nothing of 0.12.10's
  but the browser check's HEAD request.
- In x86 Node under Docker with `--no-enable-sse4-1`: 0.12.10's wasm fails with exactly the players'
  "Compiling function #81 failed: Wasm SIMD unsupported @+70121"; the worker's SIMD test fails the same
  way, and 0.12.0's wasm compiles.
- Folder output into a folder in the site's storage: every file read back at its size.

### Last results (2026-10-19, Chrome 155 on an 11-core Mac, with other jobs running)

- `make_pack.sh` through `pack.py` against the previous `make_pack.sh`: the whole pack, extras included,
  byte for byte the same.
- `check.py` at high quality: header and level 0 identical to tex3ds for all 3,101 textures that have a
  PNG; mipmaps a median 0.61 of 255 apart, plus tex3ds's wrapped pixels.
- The page in headless Chrome, whole game: 1,281 s with one worker, 407 s with three; the two zips hold
  the same 5,750 files with the same CRCs. Against the native pack (`compare.py`, OK): every file the 3DS
  reads is there; textures differ only by the fast encoding (level 0 a median 0.24 of 255 apart);
  movies 48 to 50 dB PSNR from another ffmpeg build; 238 models and animations differ only by float
  rounding (at most 1.5e-11) from numpy's linear algebra; `se_index.txt` in its notes and `ui/title.png`
  in its PNG compression only.
- The game in Azahar on that zip: all 12 smoke scenes pass and render as with the native pack.
- The final code, three workers, whole game: 410 s, `compare.py` OK again.
- WebKit (Playwright's build, Safari 26.4's engine), a persistent profile: James, events, maps, stills
  and the movies give the same 434 files, CRC for CRC, as Chrome. A private window has no storage for
  the zip; the page says so. Firefox is not tested yet: Playwright's Firefox would not start inside this
  machine's command sandbox.
- `SH2PACK_TRACE`: no step reads a `.t3x` or `.mpg` after the step that wrote it, so the engine may free
  them; the only reads across steps are the ones `tasks()` provides for.
