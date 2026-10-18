# sh23ds port status

Goal: Silent Hill 2 Director's Cut (PC) running natively on New 3DS and, with lighter settings, the original
3DS (real hardware and Azahar), as an
installable `.cia` / `.3dsx` that uses the player's own game data. The Enhanced Edition fixes are to be
folded in where they apply.

Last updated 2026-10-17 (PC camera, PC lighting, title, save/load, inventory).

## How it is built

| Piece | Where | Notes |
| --- | --- | --- |
| 3DS app | `port/` | C, libctru + citro3d. `./build.sh` (devkitARM in Docker), `./package.sh` makes `sh23ds.cia` |
| Asset converter | `tools/sh2conv/` | Reads the user's `Silent Hill 2/data`, writes the SD pack (`pack/`, copy to `sdmc:/3ds/sh23ds/`) |
| Headless tests | `tools/lr3ds/` | Runs the app in Azahar's libretro core (software renderer, New 3DS, 100% clock), scripted input, screenshots |
| Decomp tooling | `tools/*.py`, `sh2-3ds-port-kit/ghidra_scripts/` | PS2 beta DWARF names/types in the Ghidra project, PC to PS2 name mapping |

Pack layout: `bg/<area>/<room>.s3m` (geometry), `.s3l` (collision), `tex/<id>.t3x` (ETC1/ETC1A4, 256 px max,
mipmaps); `chr/<folder>/<model>.s3c` + `.s3a` + `tex/`; `debug.txt` optional (rendermode, batch, freeze).

## Working now (tested in Azahar, strict settings)

- Title screen with the user's own Director's Cut logo (`pic/etc/sh2dc.tex`), New Game (starts in the
  observation deck restroom, stage 2 / er01, where the PC version opens), Continue, Room viewer (debug)
- Room rendering for all 17 areas (16 in `data/bg` plus the DC-only `ma` from `data/bg2`): geometry, ETC1
  textures, decals, transparent pass, S3M v2 with vertex normals
- Lighting as on the PC (`envlight.c`, `world.v.pica`, data from `tools/sh2conv/lighting.py`, see
  `LIGHTING.md`): the per-room DrawEnvData record of the exe, three directional lights per vertex, the c19..c22
  constants, material modes (mode 4 adds the baked vertex colour, modes 1/2/6 scale by the material colour,
  mode 0 full bright), the record's fog curve (A + B/w), clip planes and clear colour. Dark rooms get James'
  flashlight as a PICA fragment spotlight on the specular-0 path (no N.L, like the PC's projected cookie).
  Brightness option 1..7 on the D-pad (dark rooms are as dark as on the PC)
- Camera: the PC camera module ported to C (`game/view/sh2cam.c`, 86 of 100 functions, see its
  `PORTING_NOTES.md`), driven by the rooms' `.cam` roads (all rooms of the area are resident, as the PC keeps
  neighbouring blocks); chase / settle / fixed / locus cameras and the road-driven field of view
- James: GPU-skinned, lit, animated (idle / walk / run), tank controls, collision from `.cld`
- Stage events (`events.c`): the PC EventCheck order and bitfields, look/inside triggers, game flags with
  one-shot bits, door warps across rooms and stages with a fade, locked / jammed / unlock / message results
  with the game's English text, item pickups (counts, pickup text), save points
- Save / load (`sdmc:/3ds/sh23ds/save.dat`: stage, room, position, flags, items)
- Inventory on Y (item names and descriptions from `item_msg_e.mes`)
- Music: CRI ADX and AIX with every layer mixed at its own gain, matches an ffmpeg decode (correlation
  0.999). Per-stage BGM as on the PC (`bgm.c`: SeBgmChange 0x515C20 pages, SeBgmManager area boxes with layer
  fades). Needs `sdmc:/3ds/dspfirm.cdc`
- Sound effects (`sfx.c`, data from `tools/sh2conv/sound.py`, see `port/SOUND.md`): the PC's SE id tables
  into sddata.bin and voice.afs, DirectSound-style volume and distance curve, door open/close/unlock/jam/lock
  sets, footsteps per floor material from the .cld, pickup, save, menu sounds. Verified by WAV capture
  (cross-correlation 0.99+ against the source PCM)
- `.cia` / `.3dsx` packaging (New 3DS: 124 MB ext memory, 804 MHz, L2 cache; Old 3DS: the normal 64 MB mode);
  `tools/make_pack.sh` builds the SD pack
  from the user's own install (checks the DC exe MD5); verified 2026-10-17 by a from-scratch build that reproduces the
  working pack file for file
- Debug: `debug.txt` keys `rendermode`, `batch`, `freeze`, `debughud`, `ambient`, `brightness`,
  `spawn <map> <x> <y> <z> <yaw deg> <camera mode>`; `tools/lr3ds/run.sh <timeout> [options]`

- Stage programs (`prog.c`, `prog_town.c`, `prog_apart.c`, `prog_hospital.c`, `prog_prison.c`, `prog_hotel.c`,
  `prog_wish.c`, see `port/PROGRAMS.md`): every `ev_prog` entry of stages 2..58 that a player can reach,
  ported from the PC (EventExecProgram 0x48c110 runner, event_sub.c helpers, FlagInit with the PC random
  number generator): the prologue, keys, the clock / coin / safe / box / keypad / hangman / music box /
  trunk / tombstone puzzles, yes/no questions, item use, elevators, scripted door unlocks, the ending score
  and decision (0x576660) and all endings, and Born From a Wish (second title entry; James stands in for
  Maria until her model and moves are converted). Four Born From a Wish programs are reconstructed (the PC
  keeps stages 57/58 encrypted). Cutscenes go through `prog_demo_request`; their FMVs play
- Picture viewer for examine stills (`tools/sh2conv/pics.py`, 238 stills from `data/pic`; puzzle overlay
  sprites are not drawn, the bottom screen prints the puzzle state instead)
- Firearms and more enemies (combat batch 2, `port/ENEMIES.md`): handgun, shotgun and rifle (R aims and locks on,
  A fires, automatic reload, L cycles weapons, ammo from the item counts, PC damage values, shotgun pellets),
  the stomp (R+A at a downed enemy), the plank combo; the Bubble Head Nurse (0x207) and its town variant
  (0x20b), creepers (0x202), Pyramid Head as the PC's scripted pursuer (stage 7 bars, the stage 12
  stairwell fight with its siren and exit path, the hospital roof); Maria as a companion (`maria.c`: appears
  with flag 15 at the PC appear points, follows James' trail, can be hurt, her death is a game over)
- Bosses and the rest of the enemies (combat batch 3, `port/ENEMIES.md`): the Abstract Daddy, the spear
  Pyramid Heads (the hospital chase, the hotel pair, the labyrinth wanderers), Eddie's two rounds, the Flesh
  Lips, Mary's final form, the Mandarin; holds with the struggle meter, enemy regeneration, Lying Figure
  ambushes on hard, the PC's probe path search, enemies targeting Maria, Maria's relax/cower states, and
  `enemy_stream_update` so enemies spawn and despawn with streamed tiles
- Climbing creepers and the hotel pair's turns (combat batch 7, `port/ENEMIES.md`): wall and ceiling creepers
  (PC states 4 and 5) run along their wall or the ceiling, are knocked off by hits and fall, nest releases
  start in that fall; the oni pair take turns so one stands off while the other attacks (0x4bc6b0); shots no
  longer stop 8 % short of the wall behind the target
- Maria as the Born From a Wish player (`combat.c` player switch): her model and the PC's motion tables,
  the revolver (one round to start, reloads from the ammo item) and the knife, her health (PlayerInit
  0x533ad0: 160 / 80; James 200 / 100 on beginner-easy / normal-hard), her speeds and voice
- In-engine cutscenes (`demo.c`, `tools/sh2conv/demos.py`, see `port/CUTSCENES.md`): all 128 PC scene records
  (including the seven Born From a Wish scenes from `data/chr2`), the PC player and its frame decoder: the
  scene camera, characters with the PC motion data and face morphs (.cls clusters), the scene's own lights
  as fragment lights, subtitles, voices clock-synced from voice.afs, the Angela scream and the other
  voice-and-subtitle steps, branching and chained scenes chosen as the PC programs choose them, START to skip
- Map screen (`map.c`, `tools/sh2conv/maps.py`, from the PC's Event/map.c 0x49a7e0..0x49e310): the map for
  the current room and floor, owned-map flags, the red markers set by events and programs, James' arrow
  from the per-room transform, zoom, floor switching, the dark/no-map messages. Tap the touch screen or
  use "Map" in the pause menu
- Memo list ("Memos" in the pause menu): the 57 memos of the PC table with their stills and texts
- Ending clear data (`clear.dat`) and New Game+ (the FlagInit branches for cleared games, the extra riddle
  level, endings seen on the title)
- Open areas stream their tiles (`main.c` neighbour section): the town, the forest and the lake are
  continuous tile grids, so the touching tiles within 7000 units of James stay resident (drawn, one merged
  collision set), the primary tile switches as he walks, and the area-wide `<area>GB.cam` street roads load
  first as on the PC (block table at 0x8885a0, 0x476310). Verified running from cb47 into cb44
- Enhanced Edition fixes (see `port/EE_FIXES.md`, every option with its status): PS2 flashlight brightness
  and reach, RoomLightingFix (the PC's flashlight-off level function plus EE's fixed levels), fog and
  lighting record fixes (Blue Creek, room 312, the Maria ending letter, the catacombs), IncreaseDrawDistance,
  HalogenLightFix, the gate / chainsaw / closet / fire-escape script fixes, double footsteps, menu sounds,
  hold-to-stomp, swapped light/heavy attack, PS2 camera speed, the low-health red cross, ReduceCutsceneFOV.
  The EE fog particle layer (FogFix) is not ported: the port has no particle fog yet
- The fog particle layer (`fog.c`, `fog.v.pica`, `tools/sh2conv/fog.py`; the PS2 Fog/fog.c module from the DC
  exe, 0x483820..0x48a0d0): drifting camera-facing fog sprites and the scrolling screen layer, outdoors and in
  the PC's fog rooms, with the game's .fcl areas and records, wind, the density grid and James pushing the
  fog; EE's FogFix, FogSpeedFix and ObservationDeckFogFix on top. 144 sprites at most (the PC allows 500;
  their alpha is redistributed). The PC's grey fill light with the flashlight off; a pause-menu
  "Flashlight PS2 / PC" option
- Message insertion codes (ff e0..e3 values, ff f6 questions) as on the PC; puzzle overlay sprites over the
  stills (clock, safe, coin desk, hospital box and wall symbols, quiz, trunk, egg hollow, tombstone;
  `prog_overlay.c`, sprite sheets and tables from the exe); the PC's healing (0x49e7e0: drink 25%, first-aid
  kit 50%, ampoule 100%); the map marking animation; the results screen after an ending with the PC's
  counters and rank formula
- Character lighting as on the PC (`chrlight.c`): the room's parallel lights plus each character's strongest
  point/spot lights by the PC's influence formula, the PC character shader's mix, for James, enemies, Maria
  and cutscene characters (`pack/chrlight.txt` from `lighting.py`)
- Performance (see `port/PERFORMANCE.md`): music decoded on its own thread (core 2 when allowed), streamed
  tiles read on a worker thread with only the GPU setup on the main loop, lighting tables kept in memory, an
  explicit 32 MB malloc / ~91 MB linear split in the 124 MB mode. In Azahar the worst frame in the town walk
  went from 418..501 ms (tile loads) to 17..19 ms; `perflog`, `memlog`, `memtest`, `streamsync` debug keys
- Warp autotest (`autotest warps` in debug.txt, see PROGRAMS.md): all 570 door and warp destinations load
  in Azahar with no crash, no missing textures and camera roads everywhere; the lake return after the
  Rebirth ending now lands in its room
- The PC's font (`font.c`, decoded from the exe by `ui.py`) and the results screen in the PC's layout on the
  top screen; a "Messages: top / bottom" option shows messages and subtitles on the top screen like the PC;
  the hospital elevator and manacle overlays; the music box's org_a / org_b scenes
- Save files v4 (puzzle answers, difficulty, killed enemies, health); v2 and v3 saves still load
- Character shadows, background sound loads and fast room loads (EE batch 4, `port/PERFORMANCE.md`,
  `tools/sh2conv/LIGHTING.md`): the PC's stencil shadow volumes from the `.kg1` shadow models, cast by
  characters in James' flashlight when the room's record allows it (enemies only, as on the PC, whose
  James and Maria shadow models are empty), with EE's soft-shadow opacity; sound effects read on a reader
  thread and music starts and stops handled by the music thread; `bg/<area>/area.idx` and door prefetch
  cut area changes from 3 to 5 s to about 250 ms in Azahar
- Clear data, secret items, memo overlays and the music box (programs batch 8, `port/PROGRAMS.md`): the
  "Save clear data." question after James' endings and a clear-data save (save v5, v2..v4 still load) that
  starts a New Game+ with its clear data; the PC's eight secret items (ItemGet 0x49e310) counted as "n(+k)"
  on the results screen with their rank points; the seven memo overlays (0x49a210); stage-placed models
  (`stagemdl.c`) with the lobby music box and its part animation during the tune (0x57b5c0)


## Playthrough test (critical path)

`port/source/playtest.c` plays the game from scripts of `pt` lines in `debug.txt` (see the README's
debug table): it stands James at each event's trigger and presses A or uses the event's item, closes
messages, answers questions, skips scenes, plays puzzles with scripted buttons, fights with `kill`,
`attack` and button mashing in holds, saves and loads, and checks flags, items, stages and the
endings seen, logging every flag, item, room, message and program change to `ptlog.txt`. With
`pt fast 1` the game draws nothing, which makes a chapter take seconds instead of minutes in Azahar.
The scripts and the runner live in the playtest copy (`.playtest/scripts`, `.playtest/run_all.sh`):
ten chapters from New Game to the hotel's last door (each loads the save the previous one made),
the six endings of James' game from that door or earlier saves, and Born From a Wish from the title.

Checked in Azahar on 2026-10-18 (normal levels): every chapter, Leave, Maria, In Water, Rebirth, Dog,
UFO (with its hospital and lake steps) and Born From a Wish pass. What the test found and fixed:

* The first Lying Figure (stage 5): after EvProgFirstMonsterStart the PC makes the tunnel's figure
  again with status 16 (0x48fd40), which dies the first time it is knocked down and sets flag 41
  (0x4c6614); without it the corpse scene, flag 43 and the tunnel's doors never came. The list row
  (status 15) stands still under event control until then (0x4c48f4).
* The safe in Blue Creek turned the wrong way: right turns the dial down, as the wallet's ">>" says
  (0x5971a0).
* Combining items was missing (hair and needle for the elevator key in the hospital, the rings, the
  tablets, the horseshoe with the lighter and the wax doll): R in the item list marks items, A uses
  them together; the PC's table (0x891858) gives the result id, which is used like an item
  (0x49ea00), and ItemLose of a result takes its parts (0x49e4e0).
* The third Flesh Lip of the hospital fight hangs 3200 above the floor and was never spawned (the
  floor search reached 3000), so the fight ended with two.
* The stages' room and per-frame code (Stage_Data +0x1c / +0x20) set flags the story needs, and only
  the hotel 2F weight check was ported. `prog_stagefn.c` adds the ones that matter: the well wall at
  the bottom of the hole (stage 30: James' blows that strike the room take 250 points off it, flag
  267; without it the wall could not be broken), the lift in the labyrinth's first room (stage 34:
  flags 313 and 314 after the ride; without them the room could not be left), the corridor before
  the hotel's last door (stage 44: flags 623 and 624 after 3.5 and 80 more seconds there, which
  count for the endings), Maria lying down in the day room S3 (stage 20: she stops following, flags
  188 and 629), and the small ones in stages 16, 35 and 56.
* The ending decision read the endings seen as 0; it uses the clear data now, as the PC does.

On foot (batch 2, `pt walkmode 1`, scripts `w01`..`w11` and the runner `.playtest/run_walk.sh`): James
walks to every trigger with the Circle Pad and B as a player does, on a plan over the loaded collision
(`playnav.c`: a grid with his body's clearance and the floors stacked up to six deep for stairwells)
and, in the town and forest, over all of an area's tiles first; between rooms the driver picks doors
from the event data (also through other stages, round doors it cannot reach, and back out of a room
part it got shut in). Checked in Azahar on 2026-10-19: chapters 1 to 8 pass on foot, from the
apartment in the prologue to the end of the prison and the labyrinth (Eddie's second round); the
hotel (w09, w10) and Born From a Wish (w11) are not walked yet. What walking found and fixed:

* Stairs: James kept to the floor nearest his feet, so he walked under a stairwell's first flight
  instead of up it (ap22's foot starts 100 units up). He now takes the highest floor within his step
  (the PS2 beta's clCollectCharaHeightNormal casts down from above his feet, cl_main.c).
* The ceiling list's near-vertical polygons pushed characters (hp105's slabs 636 units over the floor
  shut James into a corner). The PC's character test uses the wall and side wall lists only (the PS2
  beta's clCheckBg2Chara has wl, sw and the columns); `col_push` leaves the ceiling list out.
* Doors into the town and forest named tiles of another area (Blue Creek's way out into town west
  said cb02, town east's flat copy of the square, walled in at x 20000). The PC loads the destination
  stage's own blocks (table 0x8885a0 by glb_crd); `apply_warp` now takes that area's tile under the
  arrival point (cc11 there, cb46 for Wood Side's gate in stage 13).
* Locked and jammed doors did not set their flags (the map's door marks, and the prison's bars).
* The prison's Oppressor cell (stage 32, 0x5847f0): the tablet shuts the bars (288); 20 s after
  James first tries them (289) they open again, 10 s sooner for each later try (290). Without it a
  player was shut in.
* The labyrinth lift (stage 34, 0x582f41): 2 s after the ride the gate goes up and sets 315; the
  gate's walls kept James in the lift.
* The cube room (stage 34, 0x583960): only the two door walls follow 325 / 326; the four arms are
  floors the PC adds by ring at y 0 over the room's own at 75 (the batch 1 port wrongly walled them).
  Walked with the rings set wrong (the north way stays shut) and right (the way through, flag 327).

Fights with James' own weapons (`pt fight`, `.playtest/run_boss.sh`, Normal action level, the ammunition
and health items a player could have collected by then, well short of what the stages hold): with
`pt god 1` (blows land but take no health, to check how each fight ends) the first Lying Figure,
the stairwell Pyramid Head (health 4000 down to 40, then he leaves), the three Flesh Lips (498 s), the
Abstract Daddy, Eddie's two rounds, the hotel pair and Mary (both phases, then the Maria ending) all
end the PC's way. Without god mode the driver's play beat the first Lying Figure (HP 86 left), the
Abstract Daddy (59), Eddie (46, then 2) and Mary (98), and lost to the stairwell Pyramid Head (one
long swing does 1000), the Flesh Lips and the hotel pair; those are fights a player wins by running,
which the driver does crudely, so they say little about the port.

Known differences from the PC that are left:

* The hotel (w09, w10) and Born From a Wish (w11) are not walked yet: the chapter scripts follow the
  event numbers, not the hotel's real order (its west wing opens from 2F through a program's warp,
  stage 41 event 13), and need reordering as was done for the apartments (w03).
* Some walks take a scripted way point where the area plan's grid is too coarse for a narrow way (the
  sloping street west of Rosewater Park's stair blocks, w05 and w08).
* Puzzle answers are set with debug keys before the test plays them (the inputs are the real ones).
* The other stage room and per-frame functions are not ported. Their flag changes, none of which an
  event or program of the critical path waits for: stage 5's init (map marks 1496, 1497, 1526), 9
  (552), 11 (547), 12 (149, 526), 14's init (flag 15 and marks 1533, 1534) and 15's init (clears 15),
  15 (530, 161), 22 (216), 28 (551), 31 (648, 649), 34's room code (328, 653), 36 (365), 39 (549,
  2070), 41's room code (541), 49's init (45, 168) and 55 (729). Their models, lights and sounds are
  not ported either (stage 32's room 0x72 timer and 0x68 sound, the lift's sounds 0x4655 / 0x4656).
* The corridor's voice (60120, now in the pack as voice_5769b0) is not played: the port's voice
  steps stop the game, and the PC plays this one while James walks; Maria is not drawn lying in S3
  (the PC's character 0x106); the labyrinth lift's and the cube room's models do not move, and the
  cube's arms stay at the room's own floor height (75 lower than the PC's added floors).
* The port has the plank as James' only melee weapon (no steel pipe, great knife or chainsaw yet).
* "There's no use for just this." (item message 17) for items that do not combine is a guess at the
  PC's message (0x9445f4 = 3).
* The scripted Maria escort, the "near / far from Maria" counters and the damage counters are not
  representative in the teleport chapters, so the ending tests set those counters.
- Old 3DS support (`profile.c`, see `port/OLD3DS.md`): the console is detected at start-up and picks the
  Old 3DS or New 3DS profile (Auto / Old / New in `options.txt` and the options screens), with a
  quality level (low: two streamed tiles, half the fog sprites, no character shadows, fewer character
  lights, the half-size movie set if packed, a 30 fps cap), a controls layout (`controls_layout()`, which
  controls.c reads) and a frame rate option. The malloc / linear split
  follows the memory the process has, and room textures load from their second mip level when the
  linear heap is under 32 MB. In Azahar's Old 3DS model the 64 MB CIA
  passes the 11 smoke scenarios and all 570 warp autotest destinations, peaking at 15.5 MB of linear
  memory.

## Not done yet

1. The music box's three figure boxes on the seats (kinds 0x718..0x71a) do not turn; its part motions are
   from the decompile and not compared with a PC capture; the clear-data save uses the port's own slot
   screen (one slot) instead of the PC's save screen pictures
2. Climbing creepers stay on one straight wall segment (the PC follows the room mesh round corners and onto
   other faces)
3. Drop shadows and shadows from lights other than the flashlight; the fog layer's wall test and blow points
4. Testing on a real New 3DS (core 2 audio thread, SD read times, stencil shadows, perflog numbers) and on a
   real Old 3DS (frame times at 268 MHz, music on core 0, movie decoding, the Circle Pad Pro); enemy
   models and stage events still load during the black frame of a room change

## Decomp status

- Ghidra project `~/ghproj/sh2dc`: imports named behind SecuROM, 6,620 functions, module tags, 28,104 PS2 types
  (one category per source file, layouts verified: `_VC_ROAD_DATA` 128, `_CL_CLDHEADER` 372, `SubCharacter.pos` at +0x1C)
- Names: 30 from assert line numbers (`data/dc/ps2_assert_labels.json`), plus order alignment within files
  (`tools/align_names.py`). Alignment is only trusted between consecutive anchors: PC `vc_main.c` is partly
  reordered compared to the PS2 beta (e.g. `vcExecCamera` moved to the end)
- Budget note: a full decompile of ~5,070 game functions costs far more than one session; work goes subsystem by
  subsystem, driven by the vertical slice
