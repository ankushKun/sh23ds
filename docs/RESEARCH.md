# sh23ds research notes

The reverse-engineering side of the port: how the Director's Cut exe was mapped, the decomp tooling,
key addresses, the original plan and the work log. The player and developer guide is the
[README](../README.md).

Research workspace for a native New 3DS port of Silent Hill 2, built from a decompilation of the Windows Director's Cut (`sh2pc.exe`, 2002). The research phase is done: the reference exe is identified and verified, both PC builds have been through Ghidra and REA, the Enhanced Edition source has been mined for addresses, and the game's assets have been surveyed. The port in `port/source` is the hand-written C that came out of it, with the PC address of each ported function in its comments. [AGENTS.md](../AGENTS.md) is the index for working on it.

Phase 1 (reference setup) is done on macOS, and phase 2 (mapping the game) has started. The saved Ghidra project `~/ghproj/sh2dc` names the imports hidden behind SecuROM, has 6,620 functions in `.text` (up from 4,846) with only 1,060 orphan instructions left, and decompiles platform code with real API names. A first module map tags every function it can place: all statically linked libraries (D3DX8 with its bundled libjpeg, libpng and zlib, plus CRI) and 150 KB of game code tied to known source files. The rest of the game code is listed as gaps with hints. See [Module map](#module-map) and [Work log](#work-log) steps 24 to 40.

The shared doc "Silent Hill 2 PC to New 3DS: Port Plan" (claude.ai, ask Ankush for access) has the same material with diagrams, plus a checklist people can tick off. This README is the offline copy.

## Contents

1. [Folder layout](#folder-layout)
2. [Reference binary](#reference-binary)
3. [Quick start](#quick-start)
4. [What we know](#what-we-know)
5. [Fitting it on the New 3DS](#fitting-it-on-the-new-3ds)
6. [Target architecture](#target-architecture)
7. [Roadmap](#roadmap)
8. [Next steps](#next-steps)
9. [Key addresses (Director's Cut)](#key-addresses-directors-cut)
10. [Toolkit reference](#toolkit-reference)
11. [Setup](#setup)
12. [Work log](#work-log)
13. [Risks and open questions](#risks-and-open-questions)
14. [Ground rules](#ground-rules)
15. [Resources](#resources)

## Folder layout

| Path | What it is | In git? |
| --- | --- | --- |
| `Silent Hill 2/` | Retail Director's Cut install. **This is the reference.** | Never |
| `sh2-3ds-port-kit/` | Our toolkit: version detection, Enhanced Edition address resolver, label files, Ghidra scripts, texture census, v1.0-to-DC function map | Yes |
| `.claude/skills/reverse-engineer-anything/` | REA's skill for Claude Code, installed by `rea-agents setup` | Yes, or leave it to setup |
| `docs/RESEARCH.md` | This file | Yes |
| `sh2-3ds-port-kit/data/dc/project/` | The saved Ghidra project as text: functions, names, tags, labels, comments | Yes |
| `tools/ghidra/` | `rebuild.sh`, `export.sh`, `dec.sh`, `fetch_refs.sh`, `port_names.py` | Yes |
| `~/ghproj/sh2dc` | The saved Ghidra project itself (contains the program) | Never |

`.gitignore` keeps game files, exes and Ghidra projects out of the repo.

## Reference binary

The project targets the **Director's Cut** executable in `Silent Hill 2/`. It's a genuine, unmodified retail build.

| Field | Value |
| --- | --- |
| File | `Silent Hill 2/sh2pc.exe`, 6,180,307 bytes, dated 2002-11-20 |
| Version | Director's Cut (matches the Enhanced Edition's DC signature at 0x401005) |
| Compiled | 2002-11-05, MSVC 7.0 |
| MD5 | `f3f5360298b6d0f6f2e09cf24a0925d1` (on the Enhanced Edition's known-good list) |
| SHA-1 | `9cfb34b42da3a40975a1afa8df92c0335aeef43a` |
| SHA-256 | `f067357c3ac7815e900be6c0d3e633d7ae722bb11951a9d62a54368cc208349d` |
| Protection | SecuROM (`.cms_t`/`.cms_d` sections, entry point 0x24ceca7 inside the wrapper). The CD check at 0x4088d1 is intact |
| Game code | `.text` is not encrypted. 407 of 468 Enhanced Edition byte patterns match at their exact DC addresses |

Check your copy with:

```bash
python3 sh2-3ds-port-kit/tools/sh2version.py "Silent Hill 2/sh2pc.exe"
```

We also analyzed a **v1.0** exe (MD5 `37caa31bdb473d9019be2491bd8dc914`, SHA-1 `3201a5e1029f3ae3b77770255dcbe10360b3cd09`), which has no SecuROM. Both builds come from the same source. 3,535 functions (79%) match one-to-one between them, and every global we checked sits at a fixed offset (+0x2C00, or +0x2BE8 for some `.data` values in the 0x7F/0x88/0x89 range). Labels and notes made on either build can be carried over using `data/v10_to_dc_functions.tsv`.

## Quick start

```bash
# 1. Verify the reference exe
python3 sh2-3ds-port-kit/tools/sh2version.py "Silent Hill 2/sh2pc.exe"

# 2. Install Ghidra 12.1.4 + JDK 21 and register REA (see Setup)

# 3. The outside references (the PS2 beta's debug info gives the project its PS2 types)
tools/ghidra/fetch_refs.sh

# 4. Build the saved Ghidra project from the committed dump (~5 min): auto-analysis, the import names
#    SecuROM hides, PS2 types, then every function, name, tag, label and comment in data/dc/project
tools/ghidra/rebuild.sh

# 5. Decompile with our names, or open ~/ghproj/sh2dc in the Ghidra GUI
tools/ghidra/dec.sh 0x4a96c0 EventExecProgram
```

The dump in `data/dc/project` replaces the original build steps (labels, `FixOrphans.java` twice,
`ExportGraph.java`, `module_map.py`, `ApplyModuleMap.java`), which are kept in the kit and in the work
log below for how the data was first made. After changing the project, `tools/ghidra/export.sh` writes
the dump again for review and commit.

## What we know

### The Director's Cut exe

- **6,620 functions in `.text`** after fix-ups (auto-analysis found 4,599), about 1.5 MB of code ending near 0x5b6000. Ghidra also finds ~526 functions in the SecuROM sections, which aren't part of the game.
- **The Windows layer is small.** Window, DirectInput, Direct3D 8, DirectSound and CRI ADX, Bink and file I/O all start from about a dozen functions (see Key addresses).
- **The renderer is 194 functions** that use the D3D device (`g_pD3DDevice8` at 0xa35494), with no orphan sites left. Ghidra's auto-analysis found only 131 functions and left 225 sites orphaned. Most orphan code is reached only through vtables, callback tables or addresses stored by other code. `FixOrphans.java` turns that code into functions and cuts orphan instructions from 74,928 to 1,060. It drops candidates that turn out to be switch cases or pieces of a neighbour (210 after re-analysis) and gives the code back to the parent.
- **SecuROM redirects imports, and the names are now rebuilt.** SecuROM pointed the PE header at a new import table, but the game's original import descriptors are still intact in the first `.idata` (0x24a9000, 193 imports from 12 DLLs). Game code reaches imports four ways:
  - 730 `call [0x24dc3f0]`. That slot in `.cms_d` points at one SecuROM dispatcher that picks the API from the return address, so each call site has to be named separately. 605 are named (83%, 106 distinct APIs).
  - 628 `call`s into two encrypted stub tables (33 stubs) that SecuROM fills at run time: ~0x56e300 to 0x56ed06 (CRT, `Direct3DCreate8`, `DirectSoundCreate`) and 0x5707c0 to 0x5707fc (Bink).
  - 82 MSVC `jmp [__imp_X]` thunks that read the original IAT (e.g. 0x43d52a = `DirectInput8Create`).
  - `mov reg,[__imp_X]` + `call reg`, also through the original IAT.

  Names come from v1.0, where the same functions use plain imports. `tools/dc_imports.py` lines call sites up between the two builds, and `ApplyImportSites.java` applies the names in Ghidra. Of the 534 sites that more than one method could name, all agree.
- **Built-in libraries to replace, not decompile.** Statically linked: **D3DX8** (math, texture loading), **IJG libjpeg 6a** (1996), **libpng 1.0.5** (Oct 1999) and **zlib 1.1.3**, all in 0x408f12 to 0x43d274 (the image libraries are probably there for D3DX's texture loader; not confirmed), and **CRI middleware** (`CRI UT/PC Ver.1.24`, built 2002-09-19: ADX, ADXF, Sofdec `mf`/`mw`) in 0x55a010 to ~0x56eae0. About 181 KB of code in total. 16 CRI functions are named from their own error strings (`ADXF_Open`, `mfCiOpen`, ...).
- **The original source layout leaked.** Assert strings name 22 files under `\CVSWork\sh2pc\src\` (v1.0 says `\projects\sh2pc\src\`). Folders: `Chacter` (sic), `Collision`, `Effect`, `Enemy`, `Event`, `sound`, `view`. The full list is in `data/dc/srcfiles.txt`. Use these folder names in the decomp.
- **Floating point is mostly x87** (~57,000 x87 ops in v1.0).
- **The SSE/3DNow! code is D3DX8, not the game.** The ~95 SSE and 3DNow! routines (0x42dce0 to 0x43a640) are D3DX's processor-specific math. `D3DX_InitPSGP` (0x41df24) reads the `DisableD3DXPSGP` registry value, tests CPU features 7, 10 and 6 (3DNow!, SSE2, SSE) and fills a 57-entry function table at 0x7af8e0 from one of three setters, with x87 defaults at 0x7af9c8. The earlier note that this block was the game's own vertex transform was wrong; its first function (0x42d247) is libjpeg's `select_ncolors`. For the port, D3DX math becomes a small math library, and the game's vertex work happens in its own render code through D3D8.
- **SecuROM also encrypts some code in `.text`.** 56 functions decode to privileged or junk opcodes, almost all in 0x56c000 to 0x571000 (the statically linked MSVC runtime and the import stubs). They're decrypted at run time and can't be decompiled from this exe. They're runtime helpers the 3DS build replaces anyway; v1.0 has them in plain form. Tagged `securom-encrypted` in Ghidra.
- **Enhanced Edition labels.** Statically resolving the Enhanced Edition's `Get*Pointer()` helpers gives 71 named globals for each build (room/chapter progress, James' position, camera, flashlight, fog, timers, menus, memos, results stats). With 6 platform globals and 11 platform functions, that's 82 labels for the DC, and they import cleanly. `gChapterID` (0x1dbec0c in the DC) is read by 49 functions.
- **The duplicate labels are settled.** `gRunOption` was a resolver bug: the Enhanced Edition computes it as `ControlType - 0x06`, so it's 0x1dbec02 in the DC (0x1dbc002 in v1.0). The 6 functions that use it all use `gControlType` too, which fits an options struct. `gMemoInventory` and `gGameFlag` really are the same address (0x1f7d5e0). Neither getter adds an offset, and `GetGameFlagPointer` indexes it as a bit array (`flag >> 3`), so the memo inventory lives inside the game-flag bitfield.

### Module map

`tools/module_map.py` places functions in modules from anchors (source-path asserts, library strings, IJG's `ERREXIT` pattern, D3DX's CPU checks) and spreads them along address runs and call/pointer ownership, since MSVC links each `.obj` as one contiguous run. `ApplyModuleMap.java` turns the result into Ghidra function tags. 2,079 of 6,620 functions (22% of code bytes) are placed. That's deliberately conservative: game modules only grow through direct calls, within 16 KB, and never across another module.

| Range | What | Bytes |
| --- | --- | --- |
| 0x401000 to 0x408ba0 | Game startup, window, boot screens, CD check | ~18 KB |
| 0x408f12 to 0x43d274 | D3DX8 + libjpeg + libpng + zlib | ~130 KB |
| 0x43d740 to 0x559fff | Main game objects (Event, Enemy, Effect, sound, view, Collision, Chacter) | ~1.1 MB |
| 0x55a010 to ~0x56eae0 | CRI middleware | ~50 KB |
| 0x56c000 to 0x571000 | MSVC runtime + SecuROM stubs (partly encrypted) | ~20 KB |
| 0x570950 to 0x5b6000 | Per-area stage code up to ~0x5a4ed0 (`pic/htl`, `hsp`, `apt`, `dls`, `out`, `ufo` hints), then unplaced objects | ~240 KB |

Known source files, in link order (first/last function, after spreading):

| File | Range | Functions |
| --- | --- | --- |
| `Event/event.c` | 0x48aea0 to 0x48eac0 | 53 |
| `Event/chara_admin.c` | 0x48f4b0 to 0x48fed0 | 11 |
| `Event/chara_data_load.c` | 0x490e60 to 0x493430 | 27 |
| `Event/stg_overlay.c` | 0x495370 | 1 |
| `Event/title.c` | 0x4959d0 to 0x497fc0 | 24 |
| `Event/memo.c`, `item.c`, `event_sub.c` | 0x498b70, 0x49e6a0, 0x4a21e0 | 1 each |
| `Enemy/en_common.c` | 0x4aa5e0 | 1 |
| `Effect/ef_common.cpp` | 0x4ee930 to 0x4f3980 | 89 |
| `sound/sh_sound.c` | 0x514430 to 0x5184b0 | 81 |
| `sound/snd_select.c` | 0x5187e0 | 1 |
| `view/vc_main.c` | 0x519430 to 0x522410 | 100 |
| `Collision/cl_main.c` | 0x522760 to 0x529b10 | 64 |
| `Chacter/m3_sc.c` | 0x5369e0 to 0x53a050 | 99 |
| `Chacter/sh2_character_manage.c` | 0x53a270 | 1 |
| `Chacter/m3_play_common.c` | 0x5568c0 to 0x558fa0 | 27 |

The biggest unplaced runs, from `data/dc/module_gaps.tsv`, are the phase 2 work list:

| Range | Functions | Bytes | Next to / hints |
| --- | --- | --- | --- |
| 0x4aa760 to 0x4d0cc0 | 578 | 137 KB | after `en_common.c`: likely the individual enemy files |
| 0x57f730 to 0x5a4ed0 | 392 | 132 KB | stage code: `pic/hsp` (hospital), `apt`, `dls`, `out` |
| 0x4595c0 to 0x476380 | 149 | 113 KB | `pic/item`: items and menus |
| 0x4d0d70 to 0x4ee890 | 324 | 89 KB | before `ef_common.cpp`: likely other `Effect/` files |
| 0x478ac0 to 0x48ae60 | 278 | 65 KB | before `event.c`: `bg/ca..cd` |
| 0x4fc630 to 0x50ce00 | 193 | 57 KB | between Effect and sound |
| 0x449d50 to 0x459560 | 224 | 57 KB | `menu/mc` |
| 0x408f12 to 0x41706e | 203 | 55 KB | inside the library area: probably D3DX8 core, not yet anchored |
| 0x570950 to 0x57f480 | 162 | 52 KB | stage code: `pic/htl` (hotel), `ufo`, `chr/bos` |
| 0x529cc0 to 0x5369c0 | 126 | 47 KB | between `cl_main.c` and `m3_sc.c` |

### Game data (`Silent Hill 2/data`, 2.4 GB)

| Group | Size | Formats |
| --- | --- | --- |
| `sound` | 763 MB | CRI ADX/AIX streams, AFS archives (`voice.afs`) |
| `bg`, `bg2` (maps) | 741 MB | `.map` (geometry + textures), `.cld` collision, `.cam` cameras, `.kg1/.kg2` |
| `movie` | 599 MB | 21 Bink `.bik` files. About 216 MB of that is E3/TGS trailers (`sh2e3`, `sh3e3`, `sh3tgs`) a port can skip |
| `chr`, `chr2` (characters) | 121 MB | `.mdl` with embedded textures, `.anm` |
| `demo`, `demo2` (cutscenes) | 75 MB | `.cam`, `.anm`, `.dmm` |
| `pic`, `menu`, `etc` | ~100 MB | `.tex`, `.tbn2`, `.mes` text |

Texture census (identical for both builds): **3,581 textures, ~608 MB**.

| Format | Count | Size |
| --- | --- | --- |
| DXT1 | 2,237 | 299 MB |
| DXT3 | 791 | 220 MB |
| DXT5 | 519 | 83 MB |
| DXT4 | 20 | 1.4 MB |
| 8-bit paletted | 12 | 3.5 MB |
| RGBA8 | 2 | 1.1 MB |

Largest side: 3,070 textures are 512 px, 219 are 1024 px, 3 are 2048 px. The heaviest room is `bg/ob/ob01.map` at 3.1 MB of textures. The heaviest character models (Maria, Mary) carry ~1.1 MB each.

## Fitting it on the New 3DS

The assets fit after an offline conversion. DXT1 is 4 bits per pixel like ETC1, and DXT3/5 is 8 bpp like ETC1A4, so a straight transcode keeps the same size. citro3d keeps textures in linear FCRAM by default, so the 6 MB of VRAM isn't a hard cap.

| PC | New 3DS | Notes |
| --- | --- | --- |
| DXT1 / paletted / RGBX textures | ETC1 | Halving 512 to 256 px for the 400x240 screen cuts a heavy room from ~3.1 MB to ~0.8 MB |
| DXT3 / DXT5 / RGBA textures | ETC1A4 | Check alpha-tested fences and foliage with 4-bit alpha |
| 1024 / 2048 px textures | Downscale to 1024 max | citro3d rejects textures over 1024 |
| D3D8 fixed-function pipeline | citro3d + PICA200 vertex shaders | Texture stage states map onto the 6 TEV combiner stages. Transforms the game hands to D3D8 move into a vertex shader |
| D3DX8 math (x87/SSE/3DNow! variants) | Small math library on VFP | Replace, don't decompile. Only the D3DX calls the game actually makes need equivalents |
| Fog | PICA200 fog LUT | Built from the game's fog parameters (`PatchFogParameters`, `RunFogSpeed` in the Enhanced Edition point at them) |
| Flashlight and shadows | Fragment lighting (spotlight LUT) + shadow-map path | Highest visual risk. Prototype early |
| Film grain | Scrolling noise texture in a final pass | Cheap |
| Bink movies | H.264 at 400x240 on the New 3DS MVD hardware decoder | Re-encode during conversion |
| CRI ADX/AIX/AFS | Own decoder on a spare ARM11 core, then ndsp | Cheap |
| DirectInput | `hid` | Circle pad move, C-stick camera, bottom screen map/inventory |
| Win32 files/threads/timers | libctru | Small |
| x87 math | ARM11 VFP | Expect small physics/camera drift. Compare against PC |

Memory is comfortable: the PS2 ran the game in 32 MB, and New 3DS titles get roughly 124 MB, more in the extended memory mode. Target 30 fps at 400x240 with stereoscopic 3D off at first.

## Target architecture

```
                 Decompiled game code (C)
   Chacter, Enemy, Collision, Event, Effect, view, sound
                            |
             Platform interface (thin C API)
   render, input, audio, video, files, threads and timers
          /                 |                  \
 Windows hook harness   SDL2 desktop build    New 3DS
 original exe + DLL     OpenGL, SDL audio     citro3d + shaders, fog LUT
 A/B each function      runs without exe      hid, ndsp, MVD H.264, libctru
                                                   ^
                          user's own data/ --> asset converter
                                               (DXT to ETC1, Bink to H.264)
```

The Windows harness proves each decompiled function against the original game, the SDL2 build proves the code runs on its own, and the New 3DS backend is the last swap.

## Roadmap

1. **Reference setup (1 to 2 weeks).** Pin the DC exe by hash. Saved Ghidra project with `sh2_dc_labels.json`. MSVC 7.0 runtime signatures. Rebuild DC import names past SecuROM. REA in Claude Code. *Exit: every function in `.text` belongs to a known library, the CRI block, or a game module.*
2. **Map the game (3 to 6 weeks).** Fix function boundaries, including ~225 orphan D3D sites. Sort functions into the 22 source files. Name the player, camera, room and actor structs using the Enhanced Edition globals as anchors. *Exit: a module map that covers most of the code.*
3. **Hook harness (2 to 4 weeks).** A DLL that loads with the game and redirects any function to our C version, re3 style. The Enhanced Edition's zlib-licensed hooking code is a working reference. A per-function toggle to A/B in-game. Note: the DC exe's SecuROM may not start on modern Windows without the Enhanced Edition's fixes, so decide early whether the harness runs on the DC or the v1.0 build (the function map makes either workable). *Exit: one leaf function replaced and a room playable.*
4. **Decompile in layers (12 to 24 months part-time).** Math and utilities, then file/asset loading, then the render front end, then gameplay. Skip D3DX8, CRI, libpng, libjpeg and zlib and link open replacements (about 181 KB of the 1.5 MB of code). *Exit: the game runs with no original functions left.*
5. **Platform layer (in parallel with 4).** All D3D8/DInput/DSound/Bink/file/thread calls behind a thin interface, first implemented on SDL2 + OpenGL. *Exit: the decomp runs on desktop with identical behavior.*
6. **Asset converter (4 to 6 weeks, in parallel).** Reads the user's `data/`, writes a 3DS pack: DXT to ETC1/ETC1A4 with optional half-res, Bink to H.264, repacked maps. *Exit: every room and character converts.*
7. **3DS bring-up (3 to 6 months).** citro3d backend, vertex shaders, fog LUT. Milestones: title screen, first room, James walking, combat, a cutscene, an FMV, save/load. *Exit: the apartment area plays start to finish.*
8. **Performance and polish (ongoing).** Draw distance, culling, streaming, flashlight/shadow look, 30 fps lock. *Exit: full playthrough at a stable framerate on a New 3DS XL.*

## Next steps

- [x] Get a retail Director's Cut exe and confirm it (done: `Silent Hill 2/sh2pc.exe`)
- [x] Install Ghidra 12.1.4 + JDK 21 and build the macOS decompiler (done: Homebrew `openjdk@21`, decompiler in `build/os/mac_arm_64`)
- [x] Register REA with Claude Code: `GHIDRA_INSTALL_DIR` and `JAVA_HOME` added to this project's REA entry. Restart Claude Code, then confirm with REA's `binary_session` that the Ghidra provider shows `available`
- [x] Create the Ghidra project for the DC exe with all labels (done locally: `~/ghproj/sh2dc`, Quick start step 3)
- [x] Share the Ghidra project (done 2026-10-19: committed as a text dump in `data/dc/project`, rebuilt by `tools/ghidra/rebuild.sh`)
- [x] Rebuild DC import names behind SecuROM (done: 193 IAT slots, 33 stubs, 82 jmp thunks, 605 of 730 dispatcher sites)
- [ ] Name the last 125 dispatcher sites. Most sit in functions with no v1.0 match. Rerun `match_versions.py` against the fixed DC function list, or name them by hand from their arguments
- [x] Mark the library code (done: D3DX8, libjpeg, libpng, zlib and CRI tagged `lib/*` by the module map). Ghidra has no Function ID database for MSVC 7.0, and `vsOlder` gave a false MFC match (`RFX_Text_Bulk` at 0x40931f), so the statically linked runtime is identified by position and SecuROM encryption instead
- [x] Fix function boundaries for the orphan D3D call sites (done: 0 orphan D3D sites, 6,620 functions, 1,060 orphan instructions left)
- [x] First module map (done: `data/dc/module_map.tsv`, Ghidra tags; 22% of code bytes placed, the rest listed in `module_gaps.tsv`)
- [ ] Place the big gaps (see the table under Module map): enemy files after `en_common.c`, stage code by area, items/menus, effects. Name new modules after the leaked folders, e.g. `game/Enemy/en_<name>.c`, with a hint from the data files they load
- [ ] Name the 57 D3DX math table slots (decompile the x87 defaults at 0x7af9c8) so game calls through `g_D3DXPSGPTable` read as `D3DXMatrixMultiply` and so on
- [ ] Name the core structs (player struct around 0x1fb3c00 in the DC, camera, room, actors)
- [x] Verify the duplicate labels (done: `gRunOption` fixed to 0x1dbec02; `gMemoInventory`/`gGameFlag` is a real alias)
- [ ] Decide which build the Windows hook harness runs on (DC with Enhanced Edition fixes, or v1.0)
- [ ] Build the hook-harness DLL and replace one leaf function end to end
- [ ] Prototype fog + flashlight spotlight + shadows in citro3d on real New 3DS hardware
- [ ] Decide whether the SDL2 desktop build is an official milestone (recommended)
- [ ] Set up the repo: push `sh2-3ds-port-kit/`, `README.md`, `.gitignore` and nothing from the game folders

## Key addresses (Director's Cut)

All addresses are for the DC reference exe. v1.0 equivalents are in `data/v10/sh2_v10_labels.json`.

### Platform functions

| DC | v1.0 | Name | What it does |
| --- | --- | --- | --- |
| 0x4063c0 | 0x4063b0 | `Win_CreateWindowAndDInput` | `CreateWindowExA("SH2PC Class")`, then `DirectInput8Create` |
| 0x4f5e80 | 0x4f6310 | `Gfx_CreateD3D8` | `Direct3DCreate8(0xDC)` to `g_pD3D8` |
| 0x4f5ef0 | 0x4f6380 | `Gfx_CreateDevice` | `IDirect3D8::CreateDevice` (vtable +0x3C), tries vertex-processing modes 0x40, 0x80, 0x20 |
| 0x4f5920 | 0x4f5db0 | `Gfx_EnumAdapterModes` | Fills the resolution table |
| 0x514b60 | 0x514f10 | `Snd_Init_DSound_ADX` | DirectSound, CRI ADX on `data\sound\adx`, opens `voice.afs` |
| 0x5187b0 | 0x518b60 | `Snd_StartThread` | `_beginthread` for the sound thread |
| 0x55e050 | 0x55fd70 | `Snd_StartAudioTimer` | `timeSetEvent` audio timer |
| 0x44a510 | 0x44a370 | `File_StartLoaderThread` | Two mutexes + loader thread |
| 0x44a3a0 | 0x44a200 | `File_LoaderThreadProc` | Streaming loader |
| 0x44a250 | 0x44a0b0 | `File_GetSize` | `CreateFileA` + `GetFileSize` |
| 0x43d770 | 0x43d5b0 | `Movie_BinkPlay` | `BinkOpen` and the playback loop |

### Platform globals

| DC | v1.0 | Name | Notes |
| --- | --- | --- | --- |
| 0x934d6c | 0x93216c | `g_hWnd` | Main window |
| 0x934da8 | 0x9321a8 | `g_pDirectInput8` | 4 functions + 1 orphan site |
| 0xa35490 | 0xa32890 | `g_pD3D8` | `IDirect3D8*` |
| 0xa35494 | 0xa32894 | `g_pD3DDevice8` | The renderer: 131 functions + 225 orphan sites |
| 0xa35498 | 0xa32898 | `g_D3DPresentParams` | `D3DPRESENT_PARAMETERS` |
| 0x1d81420 | 0x1d7e820 | `g_pDirectSound` | `IDirectSound*` |

### Game globals (from the Enhanced Edition)

The full list of 71 is in `data/dc/sh2_dc_labels.json`. The ones you'll hit first:

| DC | Name | Group |
| --- | --- | --- |
| 0x1dbec0c | `gChapterID` | Progress (49 functions) |
| 0x934d78 | `gEventIndex` | Progress |
| 0x934d74 | `gMenuEvent` | Progress |
| 0x1f80090 | `gTransitionState` | Progress |
| 0x1fb3c1c | `gJamesPosX` | Player (struct around 0x1fb3c00, X at +0x1C) |
| 0x1f83bc0 | `gCameraFOV` | Camera |
| 0x1f83e40 | `gInGameCameraPosY` | Camera |
| 0x8a2b6a | `gFlashLightRender` | Flashlight |
| 0x1fba928 | `gFlashlightDir` | Flashlight |
| 0x935294 | `gLoadingScreen` | Progress |
| 0x935948 | `gFrametime` | Timing |
| 0x1dbeb94 | `gInGameTime` | Results stats |

### SecuROM import plumbing (DC)

| DC | What |
| --- | --- |
| 0x24a9000 | First `.idata`: the game's original import descriptors (193 names). IAT slots 0x24a9104 to 0x24a9c14 are labelled `__imp_*` |
| 0x2549000 | Second `.idata`: SecuROM's import table, which the PE header points at. Ghidra's externals come from here |
| 0x24dc3f0 | `.cms_d` slot pointing at the API dispatcher. 730 `call [0x24dc3f0]` sites, named per site |
| ~0x56e300 to 0x56ed06 | Encrypted stub table (CRT, `Direct3DCreate8` at 0x56ed00, `DirectSoundCreate` at 0x56ed06), labelled `stub_*`. The bytes stay encrypted until SecuROM runs, so they aren't functions; their 628 call sites get call overrides |
| 0x5707c0 to 0x5707fc | Encrypted stub table for Bink (`BinkOpen` at 0x5707c6, `BinkDoFrame` at 0x5707f0, ...), labelled `stub_*` |
| 0x43d52a etc. | MSVC `jmp [__imp_X]` thunks (82), labelled `Imp_*` |
| 0x2600000 | `sh23ds_imp`: a synthetic block in the Ghidra project only, one thunk per API, used as call-override targets. It's not part of the exe |

### Code regions (DC)

| Range | What |
| --- | --- |
| 0x401000 to 0x629000 | `.text` (2.2 MB; code ends near 0x5b6000) |
| 0x408f12 to 0x43d274 | D3DX8, libjpeg, libpng, zlib (see Module map) |
| 0x42dce0 to 0x43a640 | D3DX8 SSE / 3DNow! math variants, dispatched through `g_D3DXPSGPTable` (0x7af8e0) |
| 0x55a010 to ~0x56eae0 | CRI middleware (UT/PC 1.24) |
| 0x56c000 to 0x571000 | MSVC runtime and SecuROM stub tables, partly encrypted |
| 0x799000 to ~0x24a83e0 | `.data` (mostly zero-filled pools) |
| 0x24ac000 onward | SecuROM `.cms_t` / `.cms_d` |

## Toolkit reference

Everything is in `sh2-3ds-port-kit/`. Python tools need `pip install pefile capstone`.

| File | Use |
| --- | --- |
| `tools/sh2version.py` | Identify any `sh2pc.exe`: v1.0 / v1.1 / DC, hashes, Enhanced Edition known-good, SecuROM |
| `tools/ee_resolve.py` | Resolve every Enhanced Edition address lookup against an exe (version auto-detected). `ee_resolve.py <SH2EE checkout> <exe> <out.json> [v10\|v11\|dc]` |
| `tools/ee_labels.py` | Turn that into a label JSON with platform labels for v1.0 and DC. `ee_labels.py <SH2EE checkout> <exe> <ee_resolved.json> <labels.json>` |
| `tools/match_versions.py` | Map functions between two builds by masked-byte hashing. `match_versions.py a.exe a_functions.tsv b.exe b_functions.tsv out.tsv` |
| `tools/texture_census.py` | Formats and sizes of every texture in a `data/` folder |
| `tools/dc_imports.py` | Name the DC's SecuROM-redirected imports from v1.0 call sites. `dc_imports.py <dc exe> <kit data dir> <out dir>` writes `dc_import_sites.tsv`, `dc_import_thunks.tsv` and `dc_import_labels.json` |
| `ghidra_scripts/ApplyLabels.java` | Import a label JSON into Ghidra. Names like `Prefix_Name` become functions (disassembled first if needed), and a label that moved in the JSON is moved in Ghidra |
| `ghidra_scripts/ApplyImportSites.java` | Apply `dc_imports.py` output: jmp thunks become thunk functions of the external API, and dispatcher and stub call sites get a comment plus a call override to a synthetic thunk. Args: `<sites.tsv> <thunks.tsv>` |
| `ghidra_scripts/FixOrphans.java` | Create functions for orphan `.text` code (undisassembled call targets, code pointers in data, orphan runs), drop fragments, report D3D/dispatcher coverage. Args: `[report.txt] [watch addr ...]`. Run it twice |
| `tools/module_map.py` | First-pass module map from `ExportGraph.java` output: anchors, address-run and ownership spreading, gaps with data-folder hints, CRI names, SecuROM-encrypted flags. `module_map.py <graph dir> <exe> <out dir>` |
| `ghidra_scripts/ExportGraph.java` | Dump functions, referenced strings, calls, globals and taken code addresses for `module_map.py`. Args: `<outdir>` |
| `ghidra_scripts/ApplyModuleMap.java` | Tag functions with their module (`game/...`, `lib/...`, `import`) and `securom-encrypted`. Args: `<module_map.tsv>` |
| `ghidra_scripts/Export.java` | Dump `functions.tsv`, `import_xrefs.tsv`, optional `decomp.c`. Args: `<outdir> [addr ...]` |
| `ghidra_scripts/Refs.java` | Who references an address; `dec:<addr>` decompiles. Args: `<outfile> <addr\|dec:addr> ...` |
| `data/dc/` | DC: labels (82), resolved Enhanced Edition lookups, Ghidra function list from auto-analysis (5,125 incl. SecuROM) and after fix-ups (`ghidra_functions_fixed.tsv`: entry, name, size, body min/max, thunk), import xrefs (mostly SecuROM), import names (`dc_import_*`), module map (`module_map.tsv`, `module_ranges.tsv`, `module_gaps.tsv`), CRI names (`cri_labels.json`), hand-checked names with evidence (`manual_labels.json`), source paths |
| `data/v10/` | Same for v1.0 (82 labels, 4,838 functions, 1,840 import call sites) |
| `data/v10_to_dc_functions.tsv` | 3,535 one-to-one function matches, v1.0 entry to DC entry |

## Setup

### Ghidra + REA on macOS (Apple Silicon)

The official Ghidra zip only ships Linux and Windows decompilers, so step 3 builds the macOS one.

```bash
# 1. JDK 21 (native arm64) and Xcode command line tools
brew install --cask temurin@21
xcode-select --install 2>/dev/null

# 2. Ghidra 12.1.4 (the version REA is verified against)
mkdir -p ~/Developer/tools && cd ~/Developer/tools
curl -LO https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip
shasum -a 256 ghidra_12.1.4_PUBLIC_20260921.zip
# expect ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db
unzip -q ghidra_12.1.4_PUBLIC_20260921.zip
xattr -dr com.apple.quarantine ghidra_12.1.4_PUBLIC

# 3. Build the native decompiler
export JAVA_HOME=$(/usr/libexec/java_home -v 21)
cd ghidra_12.1.4_PUBLIC/support/gradle && ./gradlew buildNatives

# 4. Check REA can see Ghidra
export GHIDRA_INSTALL_DIR="$HOME/Developer/tools/ghidra_12.1.4_PUBLIC"
npx rea-agents doctor

# 5. Quit the Claude app fully (Cmd+Q), then register REA
npx rea-agents setup --client claude_desktop --client claude_code
```

Run step 5 in the same shell as step 4 so the Ghidra and JDK paths are saved into each agent's registration. This repo already has REA's Claude Code skill in `.claude/skills/`.

Notes from the macOS setup on 2026-10-16:

- `brew install openjdk@21` works without sudo, unlike the Temurin cask, which runs a pkg installer. Then use `JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home`.
- `buildNatives` takes ~40 s and writes the decompiler to `Ghidra/Features/Decompiler/build/os/mac_arm_64/`. Ghidra and REA both find it there.
- If REA was added at local scope for this folder (`claude mcp add` stores it under `projects` in `~/.claude.json`), that entry shadows the user-scope one `rea setup` writes. Put `GHIDRA_INSTALL_DIR` and `JAVA_HOME` in that entry's `env`, then restart Claude Code.
- Python tools: `python3 -m venv .venv && .venv/bin/pip install pefile capstone` (Homebrew Python refuses global pip installs).

### Using REA

- Use it through MCP. The CLI re-imports and re-analyzes the exe every call (about 2.5 to 3 minutes), while MCP keeps the Ghidra session open.
- Quick CLI checks: `rea analyze <exe> --provider ghidra`, `rea function <exe> <addr>`, `rea decompile <exe> <addr>`, `rea search <exe> <text>`.
- Loop per function: decompile through REA, draft clean C using the labels, test it in the hook harness against the original.
- REA's annotations last only for its session, and REA cannot open the saved project (it imports the exe into a temporary one). Keep permanent names in the saved Ghidra project and commit them with `tools/ghidra/export.sh`.

### Headless Ghidra

```bash
GH=$GHIDRA_INSTALL_DIR; K=sh2-3ds-port-kit
# Import + auto-analysis (~2.5 min) + labels
$GH/support/analyzeHeadless ~/ghproj sh2dc -import "Silent Hill 2/sh2pc.exe" -overwrite \
  -scriptPath $K/ghidra_scripts -postScript ApplyLabels.java $K/data/dc/sh2_dc_labels.json
# Dump functions and import call sites, optionally decompile a list
$GH/support/analyzeHeadless ~/ghproj sh2dc -process sh2pc.exe -noanalysis -readOnly \
  -scriptPath $K/ghidra_scripts -postScript Export.java out/ 0x4f5e80 0x514b60
# Who uses the D3D device, and decompile CreateDevice
$GH/support/analyzeHeadless ~/ghproj sh2dc -process sh2pc.exe -noanalysis -readOnly \
  -scriptPath $K/ghidra_scripts -postScript Refs.java refs.txt 0xa35494 dec:0x4f5ef0
```

### Regenerating labels

```bash
pip install pefile capstone
git clone --depth 1 https://github.com/elishacloud/Silent-Hill-2-Enhancements
python3 $K/tools/ee_resolve.py Silent-Hill-2-Enhancements "Silent Hill 2/sh2pc.exe" ee_resolved.json
python3 $K/tools/ee_labels.py  Silent-Hill-2-Enhancements "Silent Hill 2/sh2pc.exe" ee_resolved.json sh2_dc_labels.json
python3 $K/tools/texture_census.py "Silent Hill 2/data"
```

### 3DS toolchain (later phases)

devkitPro's `3ds-dev` group: devkitARM, libctru, citro3d, citro2d, `picasso` (shader assembler), `tex3ds` (texture converter). Test performance on real New 3DS hardware. Emulators like Azahar are fine for quick iteration but don't measure performance accurately.

## Work log

All on 2026-10-16. Steps 1 to 23 ran in a Linux x86-64 sandbox (Ubuntu 24.04, OpenJDK 21, Node 22, Python 3) against copies of the exes, and the censuses ran directly on the game folders. Steps 24 to 32 ran on macOS (Apple Silicon) against the DC exe in `Silent Hill 2/`.

| # | Step | How | Result |
| --- | --- | --- | --- |
| 1 | Inventory the v1.0 install | `ls`, `du`, `find` | `data/` 2.4 GB, Enhanced Edition files present |
| 2 | Hash and identify v1.0 | `shasum`, `md5`, version bytes at 0x401005 | v1.0, MD5 `37caa31b...`, on Enhanced Edition known-good list |
| 3 | Check v1.0 for patches | CD check bytes vs `NoCDPatch.cpp` | Unpatched |
| 4 | Clone Enhanced Edition source | elishacloud/Silent-Hill-2-Enhancements, commit 9cf4621 | ~470 address lookups |
| 5 | Resolve lookups on v1.0 | `ee_resolve.py` | 417 exact, 137 pointers resolved |
| 6 | Build v1.0 labels | `ee_labels.py` | 71 globals + platform labels |
| 7 | Install REA + Ghidra | `rea-agents` 6.5.0, Ghidra 12.1.4 (SHA-256 verified) | `rea doctor` healthy |
| 8 | REA overview of v1.0 | `rea analyze --provider ghidra` | 5,029 procedures, 12,497 strings, ~2.5 min |
| 9 | Ghidra export v1.0 | `analyzeHeadless` + `Export.java` | 4,620 non-thunk functions, 1,840 import call sites |
| 10 | Platform init (v1.0) | Decompiled init functions | D3D8, device, window, DInput, DSound + CRI, loader thread |
| 11 | Renderer users (v1.0) | `Refs.java` on the device global | ~130 functions + ~230 orphan sites |
| 12 | Strings + instruction mix | `strings`, `objdump` | 22 source paths, CRI versions, x87 heavy, SSE/3DNow! block |
| 13 | Texture census | `texture_census.py` | 3,581 textures, 608 MB |
| 14 | 3DS facts | citro3d + libctru source | Texture 8 to 1024 px, ETC1/ETC1A4, linear/VRAM, fog, MVD |
| 15 | Port plan + handoff doc | claude.ai doc | Roadmap, architecture, risks |
| 16 | Identify the DC exe | `sh2version.py`, PE sections, strings | Genuine DC, MD5 `f3f5360...` known-good, SecuROM intact, CD check intact |
| 17 | Resolve lookups on DC | `ee_resolve.py` (DC column) | 407 exact matches at DC addresses |
| 18 | Ghidra import of DC | `analyzeHeadless` + `Export.java` (2m42s) | 4,599 `.text` functions + 526 in SecuROM sections. Imports go through SecuROM stubs |
| 19 | Match v1.0 to DC | `match_versions.py` (capstone, masked addresses) | 3,535 unique function matches (79%) |
| 20 | Platform labels for DC | Matched functions + decompile + byte checks | All 6 globals and 11 functions located |
| 21 | DC labels | `ee_labels.py`, then `ApplyLabels.java` + `Refs.java` | 82 labels applied. Globals shift by +0x2C00 (4 by +0x2BE8). Device: 131 functions + 225 orphans |
| 22 | DC data census | `texture_census.py` on `Silent Hill 2/data` | Identical textures. 21 movies, 599 MB |
| 23 | Toolkit update | Version-aware tools, DC data, function map | `sh2-3ds-port-kit/` in this repo |
| 24 | macOS toolchain | Homebrew `openjdk@21` 21.0.11, Ghidra 12.1.4 (SHA-256 verified), `buildNatives` | Decompiler built for `mac_arm_64`. `rea doctor`: all 8 Ghidra checks pass. REA 6.5.0 is the latest on npm |
| 25 | REA registration | Local-scope entry in `~/.claude.json` lacked Ghidra/Java env | Env added. Takes effect after a Claude Code restart |
| 26 | SecuROM import layout | Section scan + disassembly of DC `.text` | Original descriptors intact in first `.idata`. 730 calls go through one dispatcher slot (0x24dc3f0), plus two encrypted stub tables and 82 `jmp` thunks |
| 27 | Name dispatcher sites | `dc_imports.py`: same offset in matched functions, order-paired unmatched functions, offset from nearest matched anchor | 605 of 730 named (518 + 18 + 69). The anchor pass agreed with all 534 sites already named |
| 28 | Name stubs and thunks | Same alignment for `call` targets near 0x56e300 and 0x5707c0; `jmp [IAT]` scan | 33 stubs (incl. 10 Bink), 82 jmp thunks, no conflicting votes |
| 29 | Duplicate labels | Enhanced Edition `Common.cpp` getters | `gRunOption` = `gControlType - 6` (fixed in both label files and `ee_labels.py`). `gMemoInventory`/`gGameFlag` is a real alias |
| 30 | Import names in Ghidra | `ApplyImportSites.java` | The decompiler ignores overrides that target externals, so 1,233 call sites (605 dispatcher, 628 stub) target 130 thunks in a synthetic block. `File_GetSize` decompiles to `CreateFileA`/`GetFileSize`/`CloseHandle`, and `Movie_BinkPlay` to `BinkOpen` and friends |
| 31 | Function boundaries | `FixOrphans.java`, run twice | `.text` functions 4,846 to 6,523. Orphan instructions 74,928 to 4,848. D3D device users 131 + 225 orphan sites to 191 + 49. 148 fragments dropped |
| 32 | Project build | Quick start step 3 from scratch | ~2 min on Apple Silicon, reproducible |
| 33 | REA with Ghidra | Restarted Claude Code, `binary_session` | Ghidra 12.1.4 provider `available`. REA sessions are ephemeral, so bulk passes stay in headless scripts and REA is used to check single functions |
| 34 | Library identification | `.rdata` byte search + REA `search_strings` | IJG libjpeg 6a, libpng 1.0.5, zlib 1.1.3, CRI UT/PC 1.24, D3DX8 (CPU detection strings) |
| 35 | The "SIMD block" | REA `analyze_function`, `xrefs`, `procedure_pseudo_code` on 0x42d247, 0x425b90, 0x41df24 | It's D3DX8's PSGP math: `D3DX_InitPSGP` picks SSE / SSE2 / 3DNow! setters for a 57-entry table. 0x42d247 is libjpeg `select_ncolors`. README claim corrected |
| 36 | Taken code addresses | `FixOrphans.java` pass 1 extended to `mov/push offset` targets, post-analysis fragment cleanup | `.text` functions 6,620, orphan instructions 1,060, D3D device 194 functions + 0 orphan sites |
| 37 | Function graph | `ExportGraph.java` | Functions, string refs, calls, globals, taken addresses. String address follows code address (Spearman 0.91), as expected from MSVC link order |
| 38 | Module map | `module_map.py`, three rule revisions | 2,079 functions placed (22% of bytes), libraries complete, 15 game files with ranges, 136 gaps with hints. Loose ownership rules over-reached (`en_common.c` to 247 KB), so game modules now need direct calls, 16 KB and no foreign module in between |
| 39 | SecuROM-encrypted code | Junk/privileged opcode scan + REA decompile of 0x56e456 | 56 functions, mostly the MSVC runtime area 0x56c000 to 0x571000 |
| 40 | Names and tags in Ghidra | `ApplyModuleMap.java`, `cri_labels.json`, `manual_labels.json` | 2,079 module tags, 56 encrypted tags, 16 CRI names, D3DX PSGP and libjpeg error-manager names (`jpeg_std_error` and its 5 methods, checked with REA) |
| 41 | 3DS port bring-up | `port/` (libctru + citro3d), `tools/sh2conv/`, `tools/lr3ds/` | Rooms, lit and fogged, with skinned James, collision and `.cia` packaging, all tested headlessly in Azahar. Details and the ordered plan live in `port/STATUS.md` |
| 42 | Music | `port/source/audio.c`, WAV capture from the libretro core | ADX and AIX play through ndsp. Silence at first came from a missing `dspfirm.cdc` (ndsp init fails without it, on hardware too). The decode matches ffmpeg at 0.999 correlation |
| 43 | Stage events | `tools/sh2conv/events.py` (layout in `EVENTS.md`), `events_pack.py`, `port/source/events.c` | 58 stages and 2,078 events decoded from the exe. Doors warp between rooms and stages, and locked/jammed messages use the game's own text from `.mes` files (`sh2mes.py`). DC stages 53..58 were matched to the `stage_x_*` message files |
| 44 | Lit characters | S3C v2 (normals), `skin.v.pica` | Normals are skinned with the same 4-bone blend, so James takes the room's flashlight and ambient light |
| 45 | Game camera | Subagent port of the PC `vc_main` module (100 functions, registers checked against objdump) | `.cam` roads drive the camera exactly as on the PC. Several decompile labels in the view module are shifted by one function; the port notes list each correction |
| 46 | Room lighting | Subagent: DrawEnvData record table at 0x8063c8, selection in 0x47c780, the game's own D3D8 shaders | Map vertex colours are added, never multiplied. Dark rooms are nearly black without the flashlight on the PC too. The 3DS reproduces the shader formulas in a vertex shader |
| 47 | Game flow | Title art from `sh2dc.tex`, save file, inventory | New Game starts in the restroom (stage 2), Continue restores flags and position, items carry the game's names and descriptions |
| 48 | Stage programs | Subagent ports of `ev_prog` for stages 2..58 (`port/PROGRAMS.md`) | Every reachable program ported with PC addresses, flags and message numbers; four Born From a Wish programs reconstructed because stages 57/58 are SecuROM-encrypted |
| 49 | Enemies and weapons | Subagent ports of en_scu, en_mkn, the nurse, creepers, Pyramid Head, Maria (`port/ENEMIES.md`) | The kind-to-model table at 0x88d408 corrected the brief: the Lying Figure is `chr/scu`, not `chr/arm` |
| 50 | Sound, music, movies, cutscenes | `sfx.c`, `bgm.c`, `movie.c` (pl_mpeg + Y2R), `demo.c` | WAV captures correlate 0.99+ with the source audio; MPEG-1 decode on core 2 keeps up in Azahar |
| 51 | Map, memos, clear data | `map.c` from Event/map.c 0x49a7e0.., `clear.dat` | Map choice, markers and the arrow follow the PC tables; New Game+ uses the ported FlagInit branches |
| 52 | Open areas | Block table at 0x8885a0, 0x476310 -> 0x512f10 | The town, forest and lake are tile grids with an area-wide `<area>GB.cam`; neighbour tiles now stream and the street camera works |
| 53 | Enhanced Edition fixes | Subagent pass over every EE option (`port/EE_FIXES.md`) | Lighting/fog record fixes, the PS2 flashlight, script fixes and control options ported; it also found a real converter bug (0x18-byte map vertices of material mode 3) |
| 54 | Desktop renderers | Portable desktop Azahar (user/ next to the app) and a `dumpframe` debug key | The game draws correctly under the desktop app's OpenGL and Vulkan renderers; a user-reported black screen is still open (stale homebrew shader cache suspected) |
| 55 | Fog layer, character light, performance | Subagent ports of Fog/fog.c (0x483820..), the PC character light lists, worker-thread streaming and audio | SH2's drifting fog outdoors; the worst town frame in Azahar went from ~500 ms to ~19 ms |
| 56 | Born From a Wish as Maria | Player switch in `combat.c`, her weapon tables (0x88d2f0 rows 10..12) | Maria plays her scenario with the revolver and knife and the PC's health values |
| 57 | Warp autotest | `autotest warps` over all 570 destinations | Every destination loads with camera roads and no missing textures; one real bug (the Rebirth lake return) fixed |

## Risks and open questions

- **Scale.** About 5,070 game functions (6,620 in `.text` minus ~1,160 library functions, thunks and runtime) is a lot for one or two people. re3 took a small team a couple of years for GTA III. Agents through REA speed up first drafts, but every function still needs testing in the harness.
- **The look.** Fog density, the flashlight cone and self-shadowing are what make it Silent Hill 2. The PC port already got them wrong before the Enhanced Edition fixed them. Prototype on real hardware first.
- **Vertex throughput.** Heavy rooms (hospital, Lakeview) may need simpler geometry or shorter draw distance.
- **SecuROM on the DC.** Static analysis is mostly unaffected. 56 runtime functions are encrypted in the DC (take them from v1.0 or the 3DS libc), and running the original DC exe for the hook harness may need the Enhanced Edition's fixes or a switch to v1.0. Decide before phase 3.
- **Undocumented formats.** `.map` geometry, `.kg1/.kg2`, `.cld`, `.anm`, `.dmm`. The decompiled loaders will document them.
- **Open question:** make the SDL2 desktop build an official milestone? It costs little and makes testing much easier.

## Ground rules

- The reference is the retail DC exe in `Silent Hill 2/`. Check hashes with `sh2version.py` before trusting any address.
- Never commit game files, exe bytes, extracted assets or Ghidra databases containing the program. `.gitignore` covers the obvious paths.
- The port ships only our code. Users point the asset converter at their own copy of the game.

## Resources

| Link | Why |
| --- | --- |
| [REA (morluto/rea)](https://github.com/morluto/rea) | MCP server for Ghidra/IDA/Hopper. npm `rea-agents`, tested at 6.5.0 |
| [REA installation guide](https://github.com/morluto/rea/blob/main/docs/installation.md) | Setup and Ghidra provider requirements |
| [REA client configuration](https://github.com/morluto/rea/blob/main/docs/client-configuration.md) | Where each agent stores the MCP entry |
| [Silent Hill 2 Enhancements](https://github.com/elishacloud/Silent-Hill-2-Enhancements) | Version detection, known-good hashes, v1.0/v1.1/DC addresses, zlib-licensed hooks |
| [Enhanced Edition releases](https://github.com/elishacloud/Silent-Hill-2-Enhancements/releases) | Installer and notes |
| [sh2tex (iOrange)](https://github.com/iOrange/sh2tex) | SH2 texture formats in `.tex`, `.tbn2`, `.mdl`, `.map` |
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) | Use 12.1.4 |
| [citro3d](https://github.com/devkitPro/citro3d) | 3DS GPU library |
| [libctru](https://github.com/devkitPro/libctru) | 3DS system library |
| [3dbrew](https://www.3dbrew.org/wiki/New_3DS) | 3DS hardware reference |
| [Silent Hill Memories: modding](https://www.silenthillmemories.net/sh2/modding_en.htm) | Older SH2 tools |
| [Silent Hill Memories: versions](https://silenthillmemories.net/sh2/versions_en.htm) | Every SH2 release |
| [The Cutting Room Floor: SH2](https://tcrf.net/Silent_Hill_2_(PlayStation_2)) | Debug features, unused content |
| [Silent Hill (PS1) decomp](https://github.com/Vatuu/silent-hill-decomp) | Same community, matching decomp workflow |
| [decomp.dev](https://decomp.dev) | Decomp progress tracking |
| re3 / reVC | The hook-DLL method this plan follows |
| [vgmstream](https://github.com/vgmstream/vgmstream) | Reference decoders for ADX/AIX/AFS |
