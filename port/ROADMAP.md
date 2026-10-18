# sh23ds roadmap, phase 2

Phase 1 got every part of the game into the port and tested in Azahar (see `STATUS.md`). Phase 2 makes it
play like a finished 3DS game on both the New 3DS and the original 3DS: the whole game playable from start to every ending with near 1:1 gameplay,
a real touch-screen interface, better controls and animation, the Enhanced Edition's assets where a player
has them, and a steadier frame rate.

Each workstream has one owner (a subagent working in its own test folder) and a check that says when it
is done. The main session merges every batch, builds, runs `tools/lr3ds/smoke.sh` and repackages the CIA.

## Where phase 2 stands (2026-10-18, version 0.1.2)

No workstream is closed by its own check yet, mostly because each check needs a real console or a run
nobody has made. In Azahar:

| Workstream | State | What is left |
| --- | --- | --- |
| 1. Playthrough | The critical path passes on foot on normal levels, with every ending and Born From a Wish; beginner, hard and extra riddle pass but for one walk step | The stage 43 step on non-normal levels; the easy action level; puzzles from real input |
| 2. Touch screen | GPU-drawn play page, items, memos, map, pause menu and options; debug page behind SELECT twice | A picture for items with no model; the console flashes at boot |
| 3. Controls and feel | Both layouts, remapping, the PC's locomotion, crossfades, quick turn, strafing, target lock | Stop animations; a check of the feel on a console |
| 4. EE assets | The packer reads every pack that applies | A run with the real packs |
| 5. Frame rate | No frame over 33 ms after a load in any room on either model | GPU time, which only a console shows |
| 6. Old 3DS | Profile, memory, Low quality, controls; smoke and warp autotest pass on Azahar's Old 3DS model | Everything on a real Old 3DS |

Next, in order: a console pass on a New 3DS and an original 3DS (TESTING.md, "On a console"), the
playthrough gaps, then the PC logic in STATUS.md's "Gaps" section 3, starting with the enemy spawn
conditions.

## 1. Full playthrough, 1:1 (owner: playthrough agent)

The game must be completable without debug lines, in order, on every difficulty.

- A scripted critical-path test: from New Game, every story beat in order (the stage programs' required
  flags, items, puzzles, bosses, scene and area changes) driven with `debug.txt` setups where walking
  would take too long, checking the flag and stage state after each beat.
- Every ending and Born From a Wish reached from a save made just before its decision point.
- Anything that blocks progress, soft-locks or differs from the PC behaviour gets fixed at the source, with
  the PC function's address in the commit.
- Done when the critical path passes end to end for James (all endings) and Maria, and the list of known
  differences from the PC is written down in `STATUS.md`.

## 2. The touch screen as a game screen (owner: programs agent)

The bottom screen is still a text console. It becomes a GPU-drawn interface in the game's look (its font,
its pictures, its colours):

- **In play:** the current map with James' marker and the red markings, live, with a tap to zoom or open
  the full map; health shown the way the PC's status screen does; the equipped weapon and its ammo.
- **Items:** the inventory as the PC's item screen (model or picture of each item, its name and text), with
  touch and button use, examine and combine.
- **Memos, map and options:** the memo list and reader, the map screen with floor switching, and the
  options (brightness, controls, run, flashlight, messages, sound volumes) as touch menus.
- **Messages and subtitles** where the player chose (top or bottom), drawn with the game's font.
- The debug readout moves to a hidden page (SELECT twice), off by default.
- Done when no text-console output is visible in normal play and each screen works with touch and buttons.

## 3. Controls and feel (owner: enemies agent)

- New 3DS inputs: C-Stick, ZL and ZR. Sensible defaults (R aims, ZR runs, ZL looks ahead or targets), the
  PC's quick 180° turn, strafing, and a remap screen in the options.
- The PC's aiming and target selection, melee timing and stomp windows checked against the exe.
- Animation: blending between clips (no snapping between idle, walk, run, aim and hits), the PC's
  turn-in-place and stop animations, and smooth motion when the frame rate isn't a multiple of 30.
- Done when a player can fight and explore without the controls fighting back, checked on recorded runs.

## 4. Enhanced Edition assets (owner: assets agent)

The Enhanced Edition's optional packs (image enhancement, FMV enhancement, audio enhancement and the
others it ships) improve the PC game's own files. When a player has them installed, the packer should use
them:

- Find out from the EE source (`~/Developer/refsrc/sh2ee`) and the packs' layout which files each pack
  replaces and how the game finds them.
- The packer reads the replacements when present and falls back to the originals, with the exe check
  unchanged. Higher-resolution textures are resized to what the 3DS can hold, where the memory budget
  allows larger textures than now.
- Done when a pack built from a copy with the packs differs only where the packs apply, and runs.

## 5. Frame rate (owner: lighting and performance agent)

- Measure every area with `perflog` (CPU and GPU time) and find where frames go over budget.
- The target is a steady 30 fps everywhere with even frame pacing, and 60 fps where the frame fits.
- Likely work: draw-call batching and fewer state changes, texture and vertex buffer reuse, culling, the
  fog layer's cost, skinning cost, and moving work off the main core.
- Done when no area in the warp autotest has a frame over 33 ms after loading in Azahar, with the
  numbers in `PERFORMANCE.md`.

## 6. The Old 3DS (owner: Old 3DS agent)

The game must also be playable on the original 3DS, 3DS XL and 2DS: 268 MHz instead of 804, no L2 cache,
64 MB of application memory by default (a larger SystemMode is possible in the CIA), and no C-Stick, ZL or
ZR.

- A hardware profile picked at startup (Auto / Old 3DS / New 3DS, overridable in the options) that sets the
  memory split, a quality level and the control layout.
- Memory that adapts to what the system gives the app; on the Old 3DS, textures loaded from their second
  mip level, fewer resident tiles and smaller caches.
- A quality setting (Low on the Old 3DS) that trims draw distance, fog sprites, shadows and lighting detail;
  movies the Old 3DS can decode in time.
- Two complete control layouts (Old 3DS: L, R, A, B, X, Y, D-Pad, Circle Pad, START, SELECT and touch; New
  3DS: adds the C-Stick, ZL and ZR) with an option to switch, shared by the controls and touch UI work.
- One CIA for both models.
- Done when the smoke scenes and the warp autotest pass on Azahar's Old 3DS model with a steady 20 to 30 fps
  in the heavy scenes, and the New 3DS profile is no slower than before.

## Rules for every workstream

- Port from the PC exe first; an improvement over the PC must be optional or clearly better (EE's
  fixes are the model).
- Every batch comes back as a diff against the current tree plus any data steps, with screenshots.
- Never commit game data. Quit every emulator when its work is done. Never touch `site/`.
