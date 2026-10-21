# sh23ds port status

Goal: Silent Hill 2 Director's Cut (PC) running natively on the New 3DS and, with lighter settings, the
original 3DS (real hardware and Azahar), as an installable `.cia` / `.3dsx` that uses the player's own
game data, with the Enhanced Edition's fixes folded in where they apply.

Last updated 2026-10-18, at version 0.1.2 (commit 5050c00 and the clean-up after it).

## Where it stands

Every part of the game is in, and in Azahar the whole game plays from New Game to each of the six endings
and through Born From a Wish, on foot, without debug help. What is left falls in three groups, in this
order of weight: testing on real consoles (almost none so far), a tail of PC behaviours that are not
ported or only approximated (listed under "Gaps" with the code that holds each one), and features of the
PC renderer the port never had (water, specular, blur). The subsystem notes (PROGRAMS, ENEMIES,
CUTSCENES, MOVIES, SOUND, EE_FIXES, PERFORMANCE, OLD3DS, CONTROLS) hold the detail; this page is the
summary and the list of what to do next. How to run the tests is in TESTING.md.

## How it is built

| Piece | Where | Notes |
| --- | --- | --- |
| 3DS app | `port/` | C, libctru + citro3d. `./build.sh` (devkitARM in Docker), `./package.sh` makes `sh23ds.cia` and `dist/` |
| Asset converter | `tools/sh2conv/` (`pack.py` is the step list) | Reads the user's `Silent Hill 2` folder (`gameversion.py` checks the DC exe's MD5, names any other release and compares the data folder's file sizes), writes the SD pack; `tools/make_pack.sh` runs it natively, `tools/packer/` in a Docker image (ghcr.io/ankushkun/sh23ds-packer) and `site/packer.html` in the browser (Pyodide, ffmpeg.wasm and `tools/texenc`, see `tools/packer-web/README.md`) |
| Headless tests | `tools/lr3ds/`, `tests/playtest/` | Azahar's libretro core with the software renderer at 100 % clock, scripted input and screenshots; the smoke run and the playthrough suite (TESTING.md) |
| Decomp tooling | `tools/*.py`, `sh2-3ds-port-kit/` | PS2 beta DWARF names and types for the Ghidra project, PC to PS2 name mapping (`docs/RESEARCH.md`) |
| Website | `site/` | GitHub Pages, deployed on push |

Pack layout: `bg/<area>/<room>.s3m` (geometry), `.s3l` (collision), `tex/<id>.t3x` (ETC1/ETC1A4, 256 px
max, mipmaps), `area.idx`; `chr/<folder>/<model>.s3c` + `.s3a` + `tex/`; `demo/`, `movie/`, `sound/`,
`events/`, `fog/`, `pic/`, `map/`, `effect/`.

## Working (tested in Azahar)

### World and rendering

- All 17 areas (16 in `data/bg` plus the DC-only `ma`): geometry, ETC1 textures, decals, the transparent
  pass, map part switches with their flags (traffic lights, the clock room, stairwell lights;
  `stageparts.c`), stage-placed models such as the lobby music box and its figure boxes (`stagemdl.c`).
- The town, forest and lake stream their tiles: touching tiles within 7000 units stay resident, the
  primary tile follows James, and the area-wide `<area>GB.cam` roads load first, as on the PC.
- Lighting as on the PC (`envlight.c`, `tools/sh2conv/LIGHTING.md`): the per-room DrawEnvData record,
  three directional lights per vertex, the material modes, the fog curve, clip planes, clear colour,
  James' flashlight as a fragment spotlight, brightness 1..7.
- Character lighting (`chrlight.c`): the room's parallel lights plus each character's strongest point and
  spot lights by the PC's influence formula, kept inside the GPU's 8-bit light range.
- The fog particle layer (`fog.c`, from the PS2 Fog module in the DC exe): drifting sprites, the screen
  layer, the `.fcl` areas, wind, the density grid, James pushing the fog, the wall test and blow points.
- Stencil shadow volumes from the `.kg1` models in James' flashlight, and drop shadows (`shadow.c`).
- Blood (`blood.c`): hit sprays, drops, floor stains, pools and bloody footprints; the noise effect
  (`noise.c`); the PC's font (`font.c`).

### Camera, controls and movement

- The PC camera module in C (`game/view/sh2cam.c`, 86 of 100 functions, see its `PORTING_NOTES.md`),
  driven by the rooms' `.cam` roads: chase, settle, fixed and locus cameras, the road-driven field of
  view, View Control (search view held or toggled).
- James' locomotion from the PC (`move.c`): turning on the spot, walk, back, strafe, three run speeds with
  stamina, the quick turn, idles, with the PC's speeds and transitions; clip crossfades in the PC's three
  modes; head turning toward targets; Rotational and Directional control styles.
- Two complete button layouts (New 3DS with the C-Stick, ZL and ZR; Old 3DS) and a Buttons page that
  remaps them (`controls.c`, `CONTROLS.md`).
- Collision from the `.cld` with the PC's floor and wall rules (James climbs stairs to the highest floor
  within a step; the ceiling list does not push).

### Story, events and programs

- Stage events (`events.c`): the PC's EventCheck order, triggers, flags, door warps, locked / jammed /
  unlock results, item pickups, save points, the needs-light bit.
- Stage programs (`prog*.c`, `PROGRAMS.md`): every `ev_prog` entry a player can reach in stages 2..58,
  all puzzles (with their overlay sprites), questions, item use and combining, elevators, the endings'
  score and decision, all six endings, the results screen and rank, clear data, the eight secret items,
  New Game+ with the extra riddle level, and Born From a Wish (four of its programs reconstructed, since
  the PC keeps stages 57 and 58 encrypted).
- Stage init, room and per-frame code where the story needs it (`prog_stagefn.c`): the well wall, the
  labyrinth lift and cube room, the hotel's last corridor, Maria in the day room bed, the prison cell bars.
- The lake boat, rowed with the PC's controls (`boat.c`).
- Items with their names and texts, the map (`map.c`) with owned maps, markers and floors, the memo list
  with stills and overlays, healing as on the PC.
- Ten save files on the PC's save and load screens (save v6; older saves load), Continue picks the one
  with the most play time.

### Combat and characters

- James' weapons: handgun, shotgun and rifle (aim, lock-on and switching as on the PC, ammo from items),
  plank, steel pipe, great knife, chainsaw, Hyper Spray (`combat.c`); the stomp, holds with the struggle
  meter, the PC's damage values and health per difficulty.
- Every enemy and boss (`enemy.c`, `ENEMIES.md`): Lying Figures, Mannequins, nurses, creepers on floors,
  walls and ceilings (round corners), Pyramid Head's scripted encounters and fights, the Abstract Daddy,
  Eddie, the Flesh Lips, Mary, the Mandarin; regeneration, hard-mode ambushes, the PC's path search.
- Maria as a companion (`maria.c`) and as the Born From a Wish player with her revolver and knife.

### Cutscenes, movies and sound

- All 128 in-engine cutscenes (`demo.c`, `CUTSCENES.md`): scene cameras, motion data, face morphs, scene
  lights, voices clock-synced, subtitles, chained and branching scenes, START to skip.
- Every FMV (`movie.c`, MPEG-1 through pl_mpeg and Y2R), including the endings, credits, boss movies and
  the title's attract movie; half-size movies for the Old 3DS if packed.
- Music: ADX and AIX with every layer at its gain, per-stage pages and area fades as on the PC; sound
  effects with the PC's tables, distance curve, doors and footsteps per floor (`SOUND.md`); BGM and SE
  volume options.

### Screens and interface

- The PC's start-up logos, title with its logo pulse, scenario and level screens, Movie Menu, attract
  mode, game over, Options, Game, Advanced and Control Options, Extra Options after a clear.
- The two screens split as `UI.md` sets out: every list on the touch screen, the thing itself on top
  (the item's model and text, the chosen save file, the chosen setting, the memo, the map), nothing
  drawn twice. The touch screen in the PC front end's look (`touchui.c`): grain, the red selection
  band, button prompts that show the bound button. The play page: the mini map with zoom, condition,
  the weapon (tap: next weapon), the healing items as quick slots. Messages and subtitles on top
  by default. The debug page sits behind SELECT twice.

### Platform and performance

- `.cia` and `.3dsx`; New 3DS at 804 MHz with L2 and the 124 MB mode, the Old 3DS in its 64 MB mode with
  a Low quality profile and a 30 fps cap (`profile.c`, `OLD3DS.md`).
- Music decoded on its own thread (core 2 when allowed), tiles, models and sound read on worker threads,
  room changes in about 250 ms in Azahar, audio read ahead in 64 KB pieces (`PERFORMANCE.md`).
- Every Enhanced Edition fix that applies, always on (`EE_FIXES.md`); the packer reads installed EE packs
  (`tools/sh2conv/EE_PACKS.md`).
- The packer runs in Docker, natively or in the player's browser (`site/packer.html`): the same
  converters, with nothing uploaded; checked file by file against the native packer.

## Testing

Details and commands are in TESTING.md. In short, as of 2026-10-18:

- Smoke run: 12 of 12 scenarios on both console models (0.1.2, rerun during the clean-up), and on a
  pack the browser packer made.
- Browser packer: the whole game in about 7 minutes with three workers in Chrome; the result matches
  the native packer's file by file within the expected encoder differences (tools/packer-web/README.md).
- Warp autotest: all 570 door and warp destinations load on both models, with textures, camera roads
  and, except the lake (which has no collision on the PC either), a floor.
- Playthrough suite (`tests/playtest/`): on 0.1.2 the teleport chain, all six endings and Born From
  a Wish pass. The walk on foot passed in full on the build before 0.1.2's console fixes (and on
  beginner, hard and extra riddle all but one step of `w10`), but on 0.1.2 it breaks in the forest in
  `w02`; see Gaps.
- Frame rate in Azahar: no frame over 33 ms after a room load in any of 229 rooms on the New 3DS model
  (60 fps target, slowest 18.9 ms) or 228 on the Old 3DS model (30 fps target).
- Real consoles: 0.1.1 ran once on a console; 0.1.2 fixes what it showed (characters too dark, cutscene
  audio stutter, left and right swapped, the flashlight button before the flashlight). Those fixes are
  unconfirmed, nothing has been measured on a console, and nothing has run on an original 3DS.

## Gaps

Ordered by weight. Each line names where the work starts.

### 1. Real hardware

- A New 3DS pass with `perflog`: frame and GPU time in the town, fog rooms, shadow rooms and the busiest
  hospital rooms; core 2 for audio; audio gaps; SD read and tile load times; movie decoding and the Y2R
  line nudge (TESTING.md, "On a console").
- The same on an original 3DS at 268 MHz, music and movies on core 0, and the Circle Pad Pro.
- Enemy models and stage events still load during the black frame of a room change.

### 2. Playthrough

- The walk on foot breaks on 0.1.2 in `w02` (stage 4, "no way on foot from ca12 to ca11"). The
  console fixes flipped the walker's turning in `playtest.c` `walk_update`; start there, then rerun
  `tests/playtest/run_walk.sh` (it stops at the first failed chapter).
- The `w10` walk step in stage 43 fails on non-normal levels (`.lr3ds/playtest/logs/L_*_w10.log` once
  run): the nav plan finds no way inside ru13.
- The easy action level and the other riddle and action combinations have never been run.
- Puzzles are solved with preset answers; the Maria escort and damage counters are set by the ending
  scripts rather than built up.

### 3. PC logic not ported or approximated

- Enemy spawn conditions: `condition_ok` (`enemy.c`, the Enemy_List row test 0x48f5b0) handles about 20
  of the codes in bits 6..15 and treats the rest as "spawn". Most likely place for an enemy the PC would
  not put there.
- Stage 17 has no program table (`prog_hospital.c`, `{ 1, NULL }`); stage 4's game mode switch
  (0x59fcd0) is not ported (`prog_town.c`). Two other NULL entries are an empty function and code no
  event runs.
- Stage room code still missing (`prog_stagefn.c`): stage 12's fade (526), stage 22's ambient sounds
  (216), stage 28 room 0x5d's ambience, stage 36's room code (365, model 0x41c), Laura's model in
  stage 14 (0x104, motion 0xb55, and 0x408), stage 32 room 0x72's timer and room 0x68's sound, the
  labyrinth lift's sounds (0x4655 / 0x4656) and its gate shake. None gates the critical path.
- Enemies (`enemy.c`): Mannequin event drops and ceiling drops (status 10 / 11 stand instead), confused
  enemies fighting each other, the Abstract Daddy's confuse state, the oni's Hyper Spray reaction,
  creepers climbing from a wall onto the ceiling or slopes, Eddie's deepest cover (0x4aa800; the freezer's
  carcasses are not in the collision), Mary's moths steering one by one and leaving by the exits
  (0x4ab780). Holds assume the PC ran them at 30 fps.
- Maria (`maria.c`): looking about and pointing at enemies, the one-step shuffle, friendly fire
  (0x52d280), lying down in the resting rooms (type 1 stands), appear points in streamed town tiles,
  being drawn behind the pause menu. In Born From a Wish, her hurt voice by damage state and her ground
  speeds from the PC's table.
- Music (`bgm.c`): the situation pages (low health, enemy near) never change, since nothing calls
  `bgm_set_situation`; the stage `bgm_control` mixes (pages 6 and up, 0x8A9EB8); stage 15 waits on flag
  159, which nothing sets, so it stays silent; no ducking in events and menus.
- Movies (`movie.c`): A or START always skip, where the PC has its own skip rules (0x43ded0) and treats
  `gero` apart; no subtitles over movies.
- Cutscenes (`demo.c`): scene light gains are a guess (colour x 0.5 x brightness) and scene lights don't
  light the map; transparent cluster maps are not morphed; agl_himei is a best fit. The Born From a
  Wish voice tracks `voice_571860`, `voice_571880` and `voice_5718a0` (and `voice_571a30`,
  `voice_577fa0`) are packed but nothing plays them; `prog_wish.c` calls 0x49fe30 a BGM change, but
  `prog_stagefn.c` uses that function as the voice call, so those are probably the missing voices.
- Sound: per-room footstep overrides (`sfx.c`, rooms 0x78, 0x2a, 0x46, 0x3e, 0xb8, 0x92..0xa3); a few
  unconfirmed sound labels (`ENEMIES.md`); the entry sounds of Born From a Wish are not positional.
- Shadows only from the flashlight and only for characters: none from event lights, map objects or in
  cutscenes.

### 4. Renderer features the port does not have

Water, specular (set to 0 in `chr.c`), blur, bloom and motion blur, rain and smoke particles, 3D trees,
event lights such as the swinging hotel lamp outside cutscenes. EE_FIXES.md lists the EE options waiting
on each.

### 5. Smaller things

- An item with no 3D model shows nothing on the item screen (no picture fallback).
- No PC stop animations; stopping is a speed ramp-down.
- The text console shows for a moment at boot before the touch screen starts.
- The EE packs are tested only on a synthetic copy built in the packs' layout, never on the real packs.
- The cube room's arms sit 75 units lower than the PC's added floors.
- "There's no use for just this." for items that do not combine is a guess at the PC's message.

## Decomp status

- Ghidra project `~/ghproj/sh2dc`: imports named behind SecuROM, 6,620 functions, module tags, 28,104 PS2
  types (one category per source file, layouts verified: `_VC_ROAD_DATA` 128, `_CL_CLDHEADER` 372,
  `SubCharacter.pos` at +0x1C).
- Names: 30 from assert line numbers (`data/dc/ps2_assert_labels.json`), plus order alignment within files
  (`tools/align_names.py`). Alignment is only trusted between consecutive anchors: PC `vc_main.c` is
  partly reordered compared to the PS2 beta (`vcExecCamera` moved to the end).
- The work goes subsystem by subsystem, with the PC address of every ported function in a comment.
