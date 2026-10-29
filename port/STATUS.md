# sh23ds port status

Goal: Silent Hill 2 Director's Cut (PC) running natively on the New 3DS and, with lighter settings, the
original 3DS (real hardware and Azahar), as an installable `.cia` / `.3dsx` that uses the player's own
game data, with the Enhanced Edition's fixes folded in where they apply.

Last updated 2026-10-29: textures at the PC's sizes and the supersampling (`VISUALS.md`), after
0.2.2's fixes for the players' reports (sources and triage kept locally in `.triage/`).

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

Pack layout: `bg/<area>/<room>.s3m` (geometry), `.s3l` (collision), `tex/<id>.t3x` (ETC1/ETC1A4, the
PC's 512 px, mipmaps), `area.idx`; `chr/<folder>/<model>.s3c` + `.s3a` + `tex/`; `demo/`, `movie/`, `sound/`,
`events/`, `fog/`, `pic/`, `map/`, `effect/`.

## Working (tested in Azahar)

### World and rendering

- All 17 areas (16 in `data/bg` plus the DC-only `ma`): geometry, ETC1 textures at the PC's 512 px
  sampled down to 256 and no further (the PC samples one level, 0x459320; the whole mip chain of the
  atlases lined the ground; `VISUALS.md`), decals (drawn after
  the characters, as the PC's ordering table does, so a chain-link fence in front of them covers
  them), the transparent pass, map part switches with their flags (traffic lights, the clock room, stairwell lights;
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
- The PC's five European languages (English, French, German, Italian, Spanish) with its Language
  row in Options; the console's language is the default, as the PC takes the system's (0x4f6cc0).
- Back faces culled as on the PC (D3DCULL_CW, 0x406ef0), for the map and for model parts with the cull
  flag, so cameras that stand outside a room see through its outer walls.
- Open areas stream, stand on and warp to only the blocks the PC's grid tables name (0x494ef0, tables
  0x88e568 to 0x88e7f0; `main.c` `k_grid`). The floating trunks by the cemetery lake (GitHub #4) were
  `ca02`, a grove no cell names, which the port had streamed as a neighbour of ca13.
- Map columns (poles, tree trunks: the `.cld` columns) push James, Maria, enemies and the boat as
  0x522de0 does.
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
- The PC's pad reading (0x52e110) and Analog run; run3 only out of doors and the 1.3x walk turn in the
  0x8bcce0 rooms (0x556490); the weapon comes up only from standing (0x557100).
- Lock-on as on the PC: the candidate list, the lock kept while any candidate is in clear sight, Target
  left and right (0x54f100), PlayerCheckAimingToEnemy turning (0x5568c0); walking, backing off,
  strafing and running under the weapon by grip flag (0x6cf7cc) with the upper body on its own clip;
  the PC camera gets James' candidates and lower status.
- A port-only behind camera (`tpcam.c`, Options > Controls > Camera: Original or Behind, also asked on the first run): orbit with the
  C-Stick (the D-Pad on an Old 3DS, a drag on the touch screen on both), the Look key recentres,
  over the shoulder when aiming, camera-relative movement; scenes and scripted shots keep the PC camera.
- Collision from the `.cld` with the PC's floor and wall rules (James climbs stairs to the highest floor
  within a step; the ceiling list does not push). A frame's move goes in steps of at most 60 units and
  never crosses a wall (`walk_move`), so a run on a slow frame no longer takes James through a thin wall;
  if he still ends up somewhere he can't walk out of, a second of holding the stick takes him back to
  the last open place he stood (`unstick_update`).

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

- Characters' transparent parts (hair, fringes, eyelashes: the `.mdl`'s transparent primitives) drawn
  blended after the opaque ones (0x504a50).
- James' weapons: handgun, shotgun and rifle (aim, lock-on and switching as on the PC, ammo from items),
  plank, steel pipe, great knife, chainsaw, Hyper Spray (`combat.c`); the stomp, holds with the struggle
  meter, the PC's damage values and health per difficulty.
- Every enemy and boss (`enemy.c`, `ENEMIES.md`): Lying Figures, Mannequins, nurses, creepers on floors,
  walls and ceilings (round corners), Pyramid Head's scripted encounters and fights, the Abstract Daddy,
  Eddie, the Flesh Lips, Mary, the Mandarin; regeneration, hard-mode ambushes, the PC's path search.
  Their sight and pace take the PC's light row (lit place, dark with or without the light), and every
  change of anime crossfades over 8 frames as 0x538920 mode 4 does.
- Maria as a companion (`maria.c`) and as the Born From a Wish player with her revolver and knife.
- Item pickups as on the PC: their models lie in the rooms (0x490080), `EventItemConditionCheck`
  (0x48cb20) decides which exist (35 town pickups only at night, flag 251), and in a dark room James
  needs the flashlight to take one (0x48e520). Packs from before the condition was packed use a table
  of those pickups' flags.
- The parts models swap (0x50dfc0): one right and one left hand of James' eighteen by weapon and state,
  the flashlight on his chest once he has it (0x52f6a0, in scenes 0x540d80), Maria's in Born From a Wish
  (0x549050). Packs from before the converter kept the pose index still get James' gameplay model right.

### Cutscenes, movies and sound

- All 128 in-engine cutscenes (`demo.c`, `CUTSCENES.md`): scene cameras, motion data, face morphs, scene
  lights, voices clock-synced, subtitles, chained and branching scenes, START to skip.
- Every FMV (`movie.c`, MPEG-1 through pl_mpeg and Y2R), including the endings, credits, boss movies and
  the title's attract movie; half-size movies for the Old 3DS if packed.
- Music: ADX and AIX with every layer at its gain, per-stage pages and area fades as on the PC, the
  stages' `bgm_control` (all 25, the scene and story pages and the fixed mixes at 0x8a9eb8), the
  programs' page sets and the duck under scenes; a music file that can't be read is tried again and
  named on the touch screen, as is a missing `dspfirm.cdc`; sound
  effects with the PC's tables, distance curve, doors and footsteps per floor (`SOUND.md`); BGM and SE
  volume options. Music, voices and movies are brought to one loudness per kind from the pack's
  measurements, through a peak limiter, effects up to 6 dB louder (3DS-only, `SOUND.md` "Levels").

### Screens and interface

- The PC's start-up logos, title with its logo pulse, scenario and level screens, Movie Menu, attract
  mode, game over, every setting of the PC's options screens (Bullet Adjust after a clear), regrouped
  into the port's five tabs.
- The two screens split as `UI.md` sets out: every list on the touch screen, the thing itself on top
  (the item's model and text, the chosen save file, the memo, the map), nothing drawn twice. The
  chosen setting is explained there: what it does, what each value changes, with animated drawings
  for the ones with two values (`figures.c`) and a note when another setting overrides it.
- A first run (`setup.c`, 3DS-only): after the logos, once, Camera and then Control Type side by side
  with their drawings; Control Type is skipped after Behind. The touch screen in the PC front end's look (`touchui.c`): grain, the red selection
  band, button prompts that show the bound button. The play page: the mini map with zoom, condition,
  the weapon (tap: next weapon), the healing items as quick slots. Messages and subtitles on top
  by default. The debug page sits behind SELECT twice.

### Platform and performance

- Textures at the PC's sizes on the New 3DS and the earlier caps on the Old 3DS (`t3xcap.c`), and the
  top screen supersampled at twice its height while the GPU has the time (`supersample.c`); see
  `VISUALS.md`.
- `.cia` and `.3dsx`; New 3DS at 804 MHz with L2 and the 124 MB mode, the Old 3DS in its 64 MB mode with
  a Low quality profile and a 30 fps cap (`profile.c`, `OLD3DS.md`).
- Music decoded on its own thread (core 2 when allowed), tiles, models and sound read on worker threads,
  room changes in about 250 ms in Azahar, audio read ahead in 64 KB pieces (`PERFORMANCE.md`).
- Every Enhanced Edition fix that applies, always on (`EE_FIXES.md`); the packer reads installed EE packs
  (`tools/sh2conv/EE_PACKS.md`).
- The packer runs in Docker, natively or in the player's browser (`site/packer.html`): the same
  converters, with nothing uploaded; checked file by file against the native packer.
- Stereoscopic 3D on the top screen (`stereo.c`): while the slider is up the world is drawn once per
  eye, with the screen plane a third of the way to what the camera looks at and 11.5 px between the eyes
  for far points at the slider's top. Text, menus, fades and the grain sit at the screen plane. The
  Display tab's row 3D (`stereo3d`) is on by default; Auto picks 60 or 30 fps by the GPU's time as
  in 2D. The map is culled per eye in runs of 32 triangles, which leaves the picture as it was (2D
  frames byte-identical) and sends 3 to 6 times fewer map vertices in the town and the forest.

## Testing

Details and commands are in TESTING.md. In short, as of 2026-10-27 (a fresh native pack and the
tree after the players' reports):

- Smoke run 12 of 12, teleport chain 17 of 17 (the critical path, all six endings, Born From a Wish),
  and the walk on foot w01 to w11 with no failure, all in Azahar; the walk and the endings also pass
  on beginner, hard and extra riddle levels (`run_levels.sh`, 17 of 17 each). Nothing of these fixes has run on a
  console yet.

- 2026-10-29, textures at the PC's sizes and the supersampling (`VISUALS.md`): smoke run 12 of 12 on both models on
  a pack converted again at the new sizes, doors within 0.1 s of the old pack's black screen on a
  5 MB/s card, the Old 3DS model drawing the new pack as it drew the old. Not run on a console.

Earlier results, as of 2026-10-18:

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
- 3D: GPU time with both eyes in the fogged town and forest after the map culling and the fog's area
  budget (aim: under 24 ms, from 30 to 34 before), whether the indoor rooms hold 60, the fog's look
  in 3D against 2D, depth comfort and ghosting in the dark scenes, the barrier switching with the
  slider, and the lake light's glow (PERFORMANCE.md "3D" has the test plan).

### 2. Playthrough

- The easy action level and the other riddle and action combinations have never been run.
- Puzzles are solved with preset answers; the Maria escort and damage counters are set by the ending
  scripts rather than built up.

### 3. PC logic not ported or approximated

- Controls (`move.c`, `combat.c`, `main.c`): the arms' aim pitch and the shotgun's three aim heights
  (0x535a90); the swing lunges (0x558830 per-frame speeds) and the combo window (0x544ac0);
  0x1fba908/0x1fba904 bit 0x40 freeing the gait mid-attack (the port keeps it for the whole attack);
  the lock-run animation speed (0x538e00 0x200); the 2D type's turn toward the last stick direction
  after release (0x546760 runs every frame); the auto-ready distance uses the feet (the PC uses James'
  0xa0); the back-off turn reversal option (0x1dbec01). The camera's slots hold enemies only (no Maria or items) and drop dying ones at once (dead_timer 1.5 s);
  every live enemy counts as active (status bit 0x400); the look line (0x527850) is `col_clamp_ray`
  at chest height. The behind camera's rates and distances are the port's own.
- A wall's facing is tested in the ground plane against its footprint; the PC uses the full plane at
  the column's height (feet minus 50 for James), which differs only for the 17 of 11,324 pushing .cld
  walls that lean more than a degree. The playtest planner (`playnav.c`) treats walls as two-sided.
- Map transparent parts are drawn in file order; the PC sorts each pass by a depth key (0x5b4ac0, qsort
  with 0x5b4ab0) whose key function (0x56e3b0) is encrypted. Both write depth with the alpha test
  (0x5b4f10), so it only matters where two transparent parts overlap. Map mode set 2 (0x5b2f20,
  alpha test off) is not modelled.
- The stages' per-frame walls (`stagewalls.c`, `clAddDynamicWall` 0x5228e0: the town's barricades and
  street ends, the forest, the lake edge and story-flag walls in fifteen stages, read from
  `events/stagewalls.bin`) and the rooms' walls stop James only from their front, as the PC's do
  (0x522b50; packs made before S3L and stagewalls.bin version 2 keep two-sided walls); the PC tests
  the previous frame's list where the port tests the current one. `clAddDynamicFloor` (0x522910) is not
  ported (stage 7 room 0x18, stage 34), nor four near-flat planes in stage 21's lists.
- Getting James out of a place he can't leave (`main.c` `unstick_update`) is the port's own. The
  autotest's stuck scan still finds 4 at the worst frame time (ma41, and three in ca12 and ps149
  that start inside solid blocks).
- In 3D the fog's sprites are held to 3 screens of area per eye (`fog.c` `stereo_thin`, the
  `fog_fill_3d` knob): the far ones are all drawn, the large near ones each with a chance that keeps
  the total, at their alpha over that chance up to twice. On the fogged roads that is about a third
  of the area 2D draws, so the near fog is patchier and slightly thinner than in 2D. The PC draws one
  view.
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
  (0x4ab780). Holds assume the PC ran them at 30 fps. The anime cuts other kinds set with bit 0x8000
  (death entries of 0x4b5190, 0x4b9460, 0x4c00a0) blend instead.
- The Lying Figure's acid is bile green (`mist.c`); the PC's is the poison texture's tan through grey
  0x80 (0x4a3d00), a yellowish haze.
- Stomping a body James killed, again and again until he walks 1000 units away (`enemy.c`
  `enemy_corpse_stompable`): the port's choice; the PC never offers a dead body (0x542a10).
- Maria (`maria.c`): looking about and pointing at enemies, the one-step shuffle, friendly fire
  (0x52d280), lying down in the resting rooms (type 1 stands), appear points in streamed town tiles,
  being drawn behind the pause menu. In Born From a Wish, her hurt voice by damage state and her ground
  speeds from the PC's table.
- Music (`bgm.c`): situation pages 2 and 3 (an enemy within 6000 units, James under 0.2 health) are
  not ported, so a stage's `bgm_control` page below 4 always stands. No 0.5 duck in the menus that stop
  the game (gEventIndex 5..9); scenes duck by DramaDemoNumber. Stage 15 still waits for flag 159 instead
  of `DAT_01F837D0`. EvProgMariaMeetingStandby's held page 0xd is asked for when EvProgMariaMeeting
  starts. Stage 10's `bgm_control` reads its room test live, where the PC sets it at room init
  (0x598700). The ending programs' page 0x2b and the page-0 sets in their results are not ported. The
  0.7 scene duck is from SOUND.md's formula (the function that applies it, 0x56e3b0, is encrypted).
- Movies (`movie.c`): A or START always skip, where the PC has its own skip rules (0x43ded0) and treats
  `gero` apart; no subtitles over movies.
- The hospital elevator's radio quiz plays as a voice track over the game view; the PC also turns the
  0x4a43 elevator hum down to 0.7 meanwhile (0x588e60 case 10).
- Cutscenes (`demo.c`): scene light gains are a guess (colour x 0.5 x brightness) and scene lights don't
  light the map; transparent cluster maps are not morphed; agl_himei is a best fit. The Born From a
  Wish voice tracks `voice_571860`, `voice_571880` and `voice_5718a0` (and `voice_571a30`,
  `voice_577fa0`) are packed but nothing plays them; `prog_wish.c` calls 0x49fe30 a BGM change, but
  `prog_stagefn.c` uses that function as the voice call, so those are probably the missing voices.
- Sound: per-room footstep overrides (`sfx.c`, rooms 0x78, 0x2a, 0x46, 0x3e, 0xb8, 0x92..0xa3); a few
  unconfirmed sound labels (`ENEMIES.md`); the entry sounds of Born From a Wish are not positional.
- EvProgUseDryCell (0x586f40) spawns the creeper room's rows after the item message, where the PC
  creates them before it, and the PC's 0x148 bit 0x400 on them is not ported.
- Stage 7 room 208 (0x59bca0): the per-frame volume of sound 16000 is left to the distance curve.
  Model 0x557's bit-1 switch (0x50b340) is not ported.
- Maria's wall column ignores the 150 height the PC uses when 0x1fb3b4c is set.
- Characters (`chr.c`): transparent parts (hair, fringes) are drawn after the opaque ones in file
  order, where the PC sorts their triangles far to near across the model (0x503e10). Opaque parts are
  not alpha-tested, where the PC tests above 0 (0x4ffa30), because ETC1A4 keeps only 4 bits of alpha.
- Fog sprites: the fade from centre to edge comes from the procedural texture unit instead of the
  vertex colour (the PC's 0x488a80 fan); the same image, chosen to avoid the flat white blocks consoles
  showed with Complex fog. Console confirmation pending.
- Untextured map materials (texture id 0) draw black, as the PC's empty stage-0 texture does; how the
  PC's loader resolves id 0 is assumed, not traced.
- Column collision (`.cld` columns: poles, trunks) pushes characters as 0x522de0 does; the camera does
  not test columns (clCheckHitEyeVectorBGColumn).
- The endings' stairwell collapse (0x440470) and walls (0x440ad0) are ported but untested while
  climbing; the effect task 0x4ee470 that 0x4ed6f0 frees is not ported.
- Languages: Japanese is not packed. The PC's options never offer it (0x4630c0 cycles 1..5) and its
  locale default never picks it (0x4f6cc0); its text needs about 1,800 full-width glyphs per font. The
  port's own texts (touch prompts, the tab names, the explainer's lines and the port's rows, the first
  run, the save screen's lines, the map legend) stay English; the PC's setting and value names come
  translated from the game. Not ported per language: the save screen's date order (0x44f4d0), the
  Italian and Spanish offset of its level text (0x452890), the options value column (0x463b33) and
  the UFO ending's captions (0x5785d0).
- Room effects (`roomfx.c`): only rooms 0xaa and 0x0e of the AutoPost table (0x8942c8) are ported.
  The lake's light is drawn 1000 units from the eye instead of in screen space; the sign of its spin
  and its order against the fog layer are assumed.
- Water (`water.c`): the fade to grey 70 is the GPU's fog with a table of the PC's line, where the PC
  fades per vertex (0x4d45e0, 0x4d2fe0); the Old 3DS rebuilds the water's vertices 15 times a second;
  the light override 0x945938 (0x47af80) is not read. Classes 22 and 23 (the boat mask, 0x4e0640 /
  0x4e02b0) and the emitters 54 and 55 are not ported.
- Shadows only from the flashlight and only for characters: none from event lights or in cutscenes.
  Characters are all the PC casts too: its caster list (0x512d60) holds only the eight character
  slots of the loader 0x513210 (0xac9a38). 0x5aad70's two room lights are ported (`shadow_frame`):
  room 312 (rr91) casts from far above (light kind 4, as a parallel light), ap42 with the flashlight
  off from its lamp. The kind 4 volumes are the port's own (the PC's are 0x444a70's cases 3 and 4),
  0x5a5860 (apartments) is not ported, and neither light has been seen casting in Azahar: James'
  own .kg1 is empty, so only other characters cast, and none were in shot.
- Pickups (`events.c`): in a dark room the PC also lets James take an item that a light near it shows
  (0x507e00 keeps a list of lit objects); the port waits for the flashlight. A stack of an item holds
  255 in the port (a byte per item in the save), where ItemGet caps it at 999, which only Bullet Adjust
  x3 reaches.
- James' death (`combat.c` `die_range`): the PC's choice of damage motion for the killing blow is in
  the encrypted 0x56e456 (after 0x534700); the port plays the knockdown (anime 112, which ends
  standing again) from the set's fall to its last frame on the floor and holds it there, as the PC
  holds the damage motion once 0x1fbad17 is set (0x559100). Blows that hurt always play anime 121,
  where the PC also has 123 (lower state 0x15).
- Model parts (`combat_set_parts`, `demo.c`): James' left hand reads the port's states for the PC's
  upper-body ones, so walking into a wall counts as walking, being held as DAMAGE, a reload as ATTACK,
  every scene as EVENT with a non-standing motion, and the plank's two-handed frames 8..21 count from the
  start of the port's heavy swing clip; his status bit 0x8000, which also hides the flashlight, isn't
  kept. Pyramid Head's one part (pose 3) always shows, where 0x53d3d0 hides it in his setup state 0xc
  and in some events. Packs made before the converter kept the pose index get the parts of every
  swapping model (`lll`, `hll`, `hhl`, `hhh` James and `lxx_mar`) from a layout table.
- Options (`optmenu.c`): the PC's pages and their layout (0x461600, 0x464eb0, the panel 0x45e750)
  give way to the port's five tabs and its top-screen explainer (`UI.md` "Options"); the settings,
  their names, values and defaults are the PC's. The explainer's text, the first run (`setup.c`) and
  three rows that each stand for two settings (Walk/Run Control with the Enhanced Edition's toggle,
  Noise Effect with its PS2 style, Heavy Attack for hold to stomp and the light and heavy swings) are
  the port's.

### 4. Renderer features the port does not have

`VISUALS.md` orders these by what they show against their GPU cost. Water, specular (set to 0 in
`chr.c`), blur, bloom and motion blur, rain and smoke particles, 3D trees,
event lights such as the swinging hotel lamp outside cutscenes. EE_FIXES.md lists the EE options waiting
on each. The lake (Effect2 class 42, 0x4d5210) and the cemetery lake (class 43, 0x4d3c20)
surfaces are drawn (`water.c`); the indoor water (classes 27 to 41: the hotel, labyrinth, apartments,
Strange Area and the final boss room, textured via 0x4d0d70 / 0x4d1a40, with waves from James' steps)
is not. The burning stairs' flames and smoke (Effect2 classes 46 to 49 and 52) and the
lake's guiding light (class 56) are ported in `roomfx.c`; the lake's water classes 22, 23 and 42
around the boat are not.

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
