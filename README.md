<p align="center">
  <img src="site/assets/banner.jpg" width="720" alt="sh23ds: Silent Hill 2 on the New Nintendo 3DS">
</p>

<p align="center">
  <b>Silent Hill 2 for the New Nintendo 3DS.</b><br>
  A native port of the 2001 PC Director's Cut that runs on your own copy of the game.
</p>

<p align="center">
  <img alt="Version 0.1.1" src="https://img.shields.io/badge/version-0.1.1-8fa3b3?style=for-the-badge">
  <img alt="New Nintendo 3DS and Nintendo 3DS" src="https://img.shields.io/badge/platform-New%203DS%20%7C%203DS-c4453a?style=for-the-badge">
  <img alt="GPLv3 license" src="https://img.shields.io/badge/license-GPLv3-5b7083?style=for-the-badge">
  <img alt="In development" src="https://img.shields.io/badge/status-in%20development-d9b26a?style=for-the-badge">
</p>

<p align="center">
  <a href="https://ankush.one/sh23ds/"><b>Website</b></a> &nbsp;•&nbsp;
  <a href="#-install">Install</a> &nbsp;•&nbsp;
  <a href="#-build-the-game-data">Game data</a> &nbsp;•&nbsp;
  <a href="#%EF%B8%8F-build-from-source">Build</a> &nbsp;•&nbsp;
  <a href="#-debugging">Debug</a> &nbsp;•&nbsp;
  <a href="#-developing">Develop</a> &nbsp;•&nbsp;
  <a href="#%EF%B8%8F-status">Status</a> &nbsp;•&nbsp;
  <a href="#-credits">Credits</a>
</p>

<br>

sh23ds is a new engine for Silent Hill 2, written in C for the New 3DS's ARM11 cores and its PICA200 GPU. The rooms, characters, camera, lighting, events, puzzles, enemies, cutscenes and endings are rebuilt from a study of the PC executable, then fed the PC version's own data, which a converter turns into 3DS formats on your computer. The fixes of the [Silent Hill 2: Enhanced Edition](https://enhanced.townofsilenthill.com/SH2/) project are built in wherever they apply.

The repository and the releases contain **no game files**. You need the PC release of *Silent Hill 2: Director's Cut*, and the packer in this repository turns it into the data the 3DS reads from its SD card.

> [!IMPORTANT]
> Version 0.1.1 is a development build. Every part of the game is in, from the prologue to each ending and Born From a Wish, and each part has been tested in the Azahar emulator, with an automated test that loads every door and warp in the game. It hasn't been checked on a console yet, so expect rough edges on real hardware. What's missing is listed under [Status](#%EF%B8%8F-status).

## ✨ What's in it

<table>
  <tr>
    <td width="50%" valign="top">
      <h4>🌫️ The fog, the dark, the flashlight</h4>
      Each room's lighting record from the PC exe, three directional lights per vertex, the PC's fog curve and the drifting fog sprite layer, and James' flashlight as a per-pixel spotlight. Dark rooms are as dark as they were on the PC.
    </td>
    <td width="50%" valign="top">
      <h4>🎥 The PC camera</h4>
      The camera module, ported function by function, follows the same camera roads through every room, with chase, fixed and scripted cameras and the PC's field of view changes.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h4>🗝️ The whole game</h4>
      Every stage program a player can reach: the puzzles, keys, elevators, questions, item uses, the endings and their score, the results screen, New Game+, and Born From a Wish with Maria as the player.
    </td>
    <td valign="top">
      <h4>🔦 Enemies and bosses</h4>
      Lying Figures, Mannequins, nurses, creepers on the floor, walls and ceiling, Pyramid Head's encounters, the Abstract Daddy, Eddie, the Flesh Lips and Mary, with melee, firearms, holds and the PC's damage values.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h4>🎞️ Cutscenes and movies</h4>
      All 128 in-engine scenes play with the scene cameras, motion data, face morphs, lights, voices and subtitles. The FMVs are re-encoded to MPEG-1 and played on the console.
    </td>
    <td valign="top">
      <h4>🎧 The sound</h4>
      CRI ADX and AIX music with every layer mixed as on the PC, per-area music changes, positional sound effects, footsteps per floor material, and voices from the game's own archive.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h4>🩹 Enhanced Edition fixes</h4>
      The PS2 flashlight, the room lighting fix, the fog and lighting record fixes, draw distance, the script fixes, hold-to-stomp, the low-health cross and more. Every EE option and what the port does with it is in <a href="port/EE_FIXES.md">EE_FIXES.md</a>.
    </td>
    <td valign="top">
      <h4>⚡ Made for the New 3DS, plays on the original</h4>
      804 MHz with the L2 cache and the 124 MB memory mode, music decoded on the second core, open areas streamed tile by tile on a worker thread, and textures in ETC1 with mipmaps. An original 3DS gets its own profile with lighter settings that targets 30 fps (tested in Azahar's Old 3DS model, not yet on hardware).
    </td>
  </tr>
</table>

## 🎮 Install

You need:

- A **New Nintendo 3DS, New 3DS XL or New 2DS XL**, or an **original 3DS, 3DS XL or 2DS**, running Luma3DS custom firmware, with FBI or the Homebrew Launcher. Not set up yet? Follow [3ds.hacks.guide](https://3ds.hacks.guide/) first. The New models give the best results. On an original 3DS the game picks its Old 3DS profile by itself: fewer fog sprites and streamed town tiles, no character shadows, simpler character lighting and 30 fps (see [port/OLD3DS.md](port/OLD3DS.md)).
- Your own copy of **Silent Hill 2 PC, Director's Cut**. The packer checks that `sh2pc.exe` has the MD5 `f3f5360298b6d0f6f2e09cf24a0925d1`, the unmodified retail Director's Cut build.
- About **1.3 GB free** on the SD card, and a computer with [Docker](https://www.docker.com/products/docker-desktop/) to build the game data.

Then:

1. **Build the game data** from your copy of the game, as described in the [next section](#-build-the-game-data). The result is a folder called `3ds`.
2. **Copy that `3ds` folder to the root of the SD card.** It merges with the `3ds` folder that is already there and adds `sdmc:/3ds/sh23ds/`.
3. **Dump the DSP firmware** if you haven't before: run [DSP1](https://github.com/zoogie/DSP1/releases) once, which writes `sdmc:/3ds/dspfirm.cdc`. Without it the game runs in silence.
4. **Install the app.** Either open FBI, choose *Remote Install*, then *Scan QR Code*, and scan the code on the [website](https://ankush.one/sh23ds/#install), or download `sh23ds.cia` from the [latest release](https://github.com/ankushKun/sh23ds/releases/latest) and install it from the SD card. The game then has its own icon on the HOME Menu. For the Homebrew Launcher, put `sh23ds.3dsx` in `sdmc:/3ds/` instead.

The SD card ends up like this:

```text
sdmc:/
└── 3ds/
    ├── dspfirm.cdc              DSP firmware, from DSP1
    ├── sh23ds.3dsx              only for the Homebrew Launcher
    └── sh23ds/                  the game data from the packer
        ├── bg/  chr/  demo/  events/  map/  movie/  pic/  sound/  ui/
        ├── lighting.txt  chrlight.txt
        ├── save.dat             written by the game
        └── options.txt          written by the game
```

> [!NOTE]
> The CIA asks a New 3DS for the 124 MB memory mode and the 804 MHz clock with the L2 cache, and runs in the normal 64 MB mode on an original 3DS. The `.3dsx` gets whatever the Homebrew Launcher was started with, so the CIA is the better choice on a New 3DS.

### Controls

| Button | In game |
| --- | --- |
| Circle Pad | walk and turn (3D type), or walk where you point (2D type, set on the title screen) |
| B (hold) | run, or toggle it with the *Run* option |
| A | check, open doors, pick up, next message, skip a movie |
| R (hold) + A | ready the weapon and attack, stomp a downed enemy |
| L | next weapon |
| X | flashlight on and off |
| Y | items (A uses, X examines, Y or B close) |
| D-pad | brightness 1 to 7 |
| Touch screen | the map |
| START | pause: map, memos, brightness, controls, run, flashlight and message options, quit to title |
| SELECT | debug information on the bottom screen |

*Advanced Options* sets the hardware profile (Auto, Old 3DS, New 3DS), the quality level and the frame rate, and *Control Options* the button layout. Every action above works on an original 3DS, which has no ZL, ZR or C-Stick; [port/OLD3DS.md](port/OLD3DS.md) lists the Old and New 3DS layouts.

## 📦 Build the game data

The packer reads your `Silent Hill 2` folder (the one with `sh2pc.exe` and `data/`), converts every room, texture, character, animation, event, message, picture, map, sound and movie into the formats the port reads, and writes the `3ds` folder for the SD card. It only reads the game folder and never changes it.

It runs in a container with everything it needs, so the only thing to install is Docker. The image holds the converters from this repository and nothing from the game.

**macOS and Linux**

```sh
mkdir sd
docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/sd:/out" ghcr.io/ankushkun/sh23ds-packer
```

**Windows (PowerShell)**

```powershell
mkdir sd
docker run --rm -v "C:\Games\Silent Hill 2:/game:ro" -v "${PWD}\sd:/out" ghcr.io/ankushkun/sh23ds-packer
```

When it finishes, `sd/3ds/sh23ds/` holds about 1.3 GB. It takes a while, most of it on textures: plan on an hour or two, depending on how many CPU cores Docker gets.

<details>
<summary><b>Notes and other ways to run it</b></summary>

<br>

- **Linux:** add `--user "$(id -u):$(id -g)"` after `--rm` so the files belong to you.
- **macOS with Colima:** only your home folder is shared with Docker, so keep both the game folder and the output folder inside it.
- **If it stops partway**, run the same command again. Steps that finished are skipped. Add `-e FRESH=1` to start over.
- **Inspection files:** add `-e KEEP_EXTRAS=1` to keep the PNG previews of every texture and the decoded events as JSON, about 0.6 GB more. The game doesn't need them.
- **Build the image yourself** from a clone instead of pulling it:

  ```sh
  docker build -f tools/packer/Dockerfile -t sh23ds-packer .
  docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/sd:/out" sh23ds-packer
  ```

- **Without Docker:** install Python 3.11 or newer with `numpy`, `Pillow` and `pefile`, ffmpeg (with the Bink decoder, which Homebrew's and most Linux builds have) and devkitPro's `tex3ds`, then run the script directly. It writes the data to `./pack`, which you copy to `sdmc:/3ds/sh23ds/`.

  ```sh
  python3 -m venv .venv && .venv/bin/pip install numpy pillow pefile
  tools/make_pack.sh "/path/to/Silent Hill 2"
  ```

- **Enhanced Edition packs:** if your game folder has the Enhanced Edition's `sh2e` folder (its Essential Files and the Image, FMV and Audio Enhancement Packs), the packer uses those files in place of the originals and lists them in `ee_used.txt`. The Enhanced Executable replaces `sh2pc.exe`, and the packer still needs the Director's Cut one, so put that back first. Add `-e SH2EE=0` to build from the original files only. Details are in [EE_PACKS.md](tools/sh2conv/EE_PACKS.md).

</details>

## 🛠️ Build from source

Everything builds in containers with pinned images, so the only requirement is Docker (Docker Desktop, or the CLI with [Colima](https://github.com/abiosoft/colima) on macOS).

```sh
git clone https://github.com/ankushKun/sh23ds.git
cd sh23ds
./port/build.sh       # devkitARM: port/sh23ds.3dsx and sh23ds.elf
./port/package.sh     # bannertool and makerom: port/sh23ds.cia, then dist/ with SHA-256 sums
```

`build.sh` passes its arguments to `make`, so `./port/build.sh clean` works, and `SH23DS_BUILD_JOBS` sets the job count. The first `package.sh` builds the small `sh23ds-cia-tools` image from `port/cia-tools.Dockerfile`, which downloads makerom and bannertool and checks them against pinned checksums.

<details>
<summary><b>Releases</b></summary>

<br>

GitHub Actions byte-compiles the converters and builds the 3DSX and CIA on every push and pull request, and keeps `dist/` as a build artifact. To release, bump `VERSION`, write what changed in `.github/releases/v<version>.md`, commit, and push a matching tag such as `v0.1.0`. The CI workflow checks that the tag matches `VERSION`, verifies the checksums and publishes the release with those notes, the install steps from `.github/release-notes.md` and the checksums. The packer workflow publishes `ghcr.io/ankushkun/sh23ds-packer` with the same version and `latest`. The website's QR code points at `releases/latest/download/sh23ds.cia`, so it stays valid for every release.

</details>

## 🐞 Debugging

### The debug file

A text file at `sdmc:/3ds/sh23ds/debug.txt` changes how the game starts, one setting per line. Delete it to play normally.

| Line | What it does |
| --- | --- |
| `spawn <map> <x> <y> <z> <yaw> <mode>` | start in a room, e.g. `spawn er01 -19250 0 21250 90 2` (mode 2 plays as James, other modes open the room viewer) |
| `stage <n>` | the stage to start the spawn in (otherwise taken from the room) |
| `flag <n> <0\|1>` | set a game flag before the spawn |
| `item <id> <count>` | give an item before the spawn |
| `hp <n>` | start with this much health |
| `action <0..3>` / `riddle <0..3>` | action and riddle difficulty |
| `enemy <kind> <x> <z> <yaw> [status] [height]` | place an extra enemy, kind in hex (`enemy 202 ...` is a creeper) |
| `wish` | Born From a Wish flags (with `stage 53` for its spawn) |
| `demo <name> <seconds>` | play a cutscene from this point |
| `movie <name>` | play a movie |
| `newgame 1` | run New Game without pressing anything |
| `results 1` / `itemget <id>` | open the results screen, after counting these item pickups |
| `debughud 1` | the debug readout on the bottom screen (SELECT toggles it in game) |
| `dumpframe <n>` | save frame *n* as `frame_<n>.bmp` and its stats as text |
| `perflog 1` / `memlog 1` / `memtest <n>` | frame time breakdown, memory use, a memory stress test |
| `streamsync 1` | load streamed tiles on the main thread, to compare with the worker |
| `autotest warps <first> <count>` | load every door and warp destination in turn and log the result |
| `pt <command>` | a playtest script line: `ev <n>`, `use <item>`, `combine <a> <b>`, `press <buttons>`, `answer <k>`, `kill`, `attack`, `expect flag <n> <0\|1>`, `save` / `load <name>`, `fast 1`; on foot with `walkmode 1` (then `ev <n> [@stage]` walks there through the doors), `walkto <x y z>`, `blocked <x y z>`, `collect`, `fight [s] [radius] [flag n]` with James' own weapons, `cube <outer> <inner>` and more, logged to `ptlog.txt` (see [`playtest.c`](port/source/playtest.c)) |
| `rendermode <n>` / `batch <n>` / `freeze 1` / `ambient <percent>` | renderer experiments |

### In an emulator

[Azahar](https://azahar-emu.org/) runs the port. Set the system to New 3DS (or Old 3DS to try the Old 3DS profile), put the game data in its virtual SD card under `3ds/sh23ds/` (the `sdmc` folder in Azahar's user directory), and add any file named `3ds/dspfirm.cdc`, since Azahar's audio doesn't need the real firmware. Then open `port/sh23ds.3dsx` or install the CIA.

If the top screen stays black in the desktop app, try switching the graphics API between Vulkan and OpenGL, or clearing Azahar's shader cache for title `0000000000000000`. A `dumpframe` line in `debug.txt` tells you whether the game drew anything.

### Headless test runs (macOS)

`tools/lr3ds` is a small libretro frontend that runs a build in Azahar's libretro core with no window: software renderer, New 3DS model, scripted buttons and screenshots at chosen frames.

```sh
clang -O2 -Wall -o tools/lr3ds/lr3ds tools/lr3ds/lr3ds.c -framework OpenGL
export AZAHAR_CORE=/path/to/azahar_libretro.dylib
tools/lr3ds/run.sh 600 --frames 600 --shot 590:/tmp/title.bmp
tools/lr3ds/run.sh 600 --frames 480 --press 300-303:A --analog 320-400:0,1 --shot 470:/tmp/walk.bmp
tools/lr3ds/smoke.sh  # 11 scenarios from the title to a save point, with a contact sheet
```

`run.sh` uses `.lr3ds/save/Azahar/sdmc/` as the SD card, so the game data goes in `.lr3ds/save/Azahar/sdmc/3ds/sh23ds/`. Every option is listed at the top of [`lr3ds.c`](tools/lr3ds/lr3ds.c).

## 🧭 Developing

<details open>
<summary><b>Project layout</b></summary>

<br>

```text
port/source/          the 3DS engine (C, libctru and citro3d)
  main.c                main loop, title, saves, menus, room streaming, rendering
  room.c collide.c      room geometry and textures, collision
  game/view/sh2cam.c    the PC camera module
  envlight.c chrlight.c fog.c     lighting, character lights, the fog layer
  events.c prog*.c      stage events and the stage programs, one file per part of the town
  enemy.c combat.c maria.c        enemies, bosses, weapons, Maria
  demo.c movie.c        in-engine cutscenes and FMV
  audio.c bgm.c sfx.c   music, per-area music, sound effects
  stream.c perf.c       worker thread streaming, memory split, frame timing
  *.v.pica              vertex shaders
port/meta/            CIA banner, icon and RSF
tools/sh2conv/        the converters from the PC formats to the pack
tools/make_pack.sh    runs every converter in order
tools/packer/         the packer image
tools/lr3ds/          headless test runner
sh2-3ds-port-kit/     reverse-engineering tools: labels, Ghidra scripts, version detection
docs/RESEARCH.md      how the exe was mapped, key addresses, the work log
site/                 the website, published by GitHub Pages
```

</details>

### How a feature gets ported

Most work follows the same loop. Find the PC function in the Director's Cut exe (the Ghidra project gets its names from the PS2 beta's debug information and the Enhanced Edition source, see [RESEARCH.md](docs/RESEARCH.md)), write the C in `port/source` with the PC address in a comment, add or extend a converter in `tools/sh2conv` if it needs data, rebuild the pack for that part only, and check it in Azahar with a `spawn` line and a screenshot. Then run `tools/lr3ds/smoke.sh` before you commit.

The notes for each part live next to the code:

| File | Covers |
| --- | --- |
| [port/STATUS.md](port/STATUS.md) | what works, what doesn't, how it was tested |
| [port/PROGRAMS.md](port/PROGRAMS.md) | stage programs, puzzles, endings, results, debug keys for them |
| [port/ENEMIES.md](port/ENEMIES.md) | enemies, bosses, combat |
| [port/CUTSCENES.md](port/CUTSCENES.md), [port/MOVIES.md](port/MOVIES.md) | in-engine scenes and FMV |
| [port/SOUND.md](port/SOUND.md) | music, sound effects, voices |
| [port/EE_FIXES.md](port/EE_FIXES.md) | every Enhanced Edition option and its state in the port |
| [port/PERFORMANCE.md](port/PERFORMANCE.md) | frame budget, streaming, memory |
| [tools/sh2conv/EVENTS.md](tools/sh2conv/EVENTS.md), [LIGHTING.md](tools/sh2conv/LIGHTING.md) | event and lighting data formats |

> [!CAUTION]
> Never commit anything that comes from the game: no exe bytes, no extracted assets, no converted pack, no Ghidra databases or decompiler output. `.gitignore` covers the usual places, `tools/install_hooks.sh` adds a pre-commit hook that refuses them, and CI runs the same check (`tools/check_repo.sh`).

## 🗺️ Status

Working in Azahar: the whole game from the prologue to every ending, Born From a Wish, saves and New Game+, all 17 areas, every door and warp, the PC camera, lighting and fog, all enemies and bosses, cutscenes, movies, music and sound effects, the maps and memos, and the Enhanced Edition fixes listed in [EE_FIXES.md](port/EE_FIXES.md).

Still to do:

- [ ] A test pass on a real New 3DS: frame times, SD card read times, audio on the second core
- [ ] A test pass on a real original 3DS: frame times at 268 MHz, music and movie decoding, the Circle Pad Pro
- [ ] Drop shadows and shadows from lights other than the flashlight
- [ ] Climbing creepers that follow the room round corners, as on the PC
- [ ] The music box's three figure boxes
- [ ] The fog layer's wall test and blow points

The full list, with how each part was checked, is in [port/STATUS.md](port/STATUS.md). Bug reports are welcome in [Issues](https://github.com/ankushKun/sh23ds/issues). Please include your console model, your Luma3DS version and what you were doing.

## ❓ FAQ

<details>
<summary><b>Where do I get the game files?</b></summary>
<br>
From your own copy of the PC game. This project doesn't host, link to or share game files, and issues asking for them will be closed.
</details>

<details>
<summary><b>Does it run on an original 3DS or 2DS?</b></summary>
<br>
Yes, with lighter settings. The game detects the console and switches to its Old 3DS profile: two streamed tiles instead of four in the open areas, half the fog sprites, no character shadows, fewer character lights and a 30 fps cap for even frame pacing. It fits in the original 3DS's normal 64 MB application memory, so the console doesn't restart into an extended memory mode. *Advanced Options* switches between the Old 3DS and New 3DS profiles, the quality level and the frame rate, and *Control Options* has the button layout. This has been tested in Azahar's Old 3DS model only, not on a real console yet; [port/OLD3DS.md](port/OLD3DS.md) has the details.
</details>

<details>
<summary><b>Can I use the v1.0 PC release, or a copy with the Enhanced Edition installed?</b></summary>
<br>
The v1.0 release, no: the converters read tables at fixed addresses in the Director's Cut exe, so the packer only accepts that build. The Enhanced Edition's packs, yes: the packer uses the files in your `sh2e` folder. Its Enhanced Executable replaces `sh2pc.exe`, so put the Director's Cut exe back before running the packer.
</details>

<details>
<summary><b>Is this an emulator?</b></summary>
<br>
No. It's a new engine that runs the game's logic as native ARM code and draws with the 3DS's own GPU. The PC game's data is converted once, on your computer.
</details>

## 💙 Credits

- [**Silent Hill 2: Enhanced Edition**](https://github.com/elishacloud/Silent-Hill-2-Enhancements) by Elisha Riedlinger and contributors (zlib), whose fixes and research into the PC exe this port builds on
- The **Silent Hill Database** for its file format notes, and the debug information in the PS2 beta build, which names much of the PC code
- [**devkitPro**](https://devkitpro.org/), **libctru** and **citro3d**, which make 3DS homebrew possible at all
- [**pl_mpeg**](https://github.com/phoboslab/pl_mpeg) by Dominic Szablewski (MIT), the MPEG-1 decoder for the movies
- [**Azahar**](https://azahar-emu.org/), the emulator every change was tested in
- [**Ghidra**](https://ghidra-sre.org/), for the reverse engineering

## ⚖️ License and legal

sh23ds is released under the [GNU General Public License v3.0](LICENSE). Bundled third-party code keeps its own license.

This is a fan project, not affiliated with or endorsed by Konami or Nintendo. *Silent Hill* is a trademark of Konami Digital Entertainment. Nintendo 3DS is a trademark of Nintendo. The repository, its releases and the packer image contain no game code or assets, and playing requires a legally obtained copy of the PC game.

<br>

<p align="center">
  Made by <a href="https://ankush.one">ankushKun</a> &nbsp;•&nbsp; <a href="https://github.com/ankushKun">GitHub</a> &nbsp;•&nbsp; <a href="https://x.com/ankushKun_">X</a>
</p>
