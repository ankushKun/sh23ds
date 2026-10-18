# Testing sh23ds

Four kinds of test cover the port: the smoke run after every change, the warp autotest over every door,
the playthrough suite that plays the game from New Game to each ending, and passes on a real console.
The first three run headless in Azahar's libretro core on a Mac; the console pass is by hand.

Everything below assumes a build (`port/build.sh`), the lr3ds frontend (`clang -O2 -Wall -o
tools/lr3ds/lr3ds tools/lr3ds/lr3ds.c -framework OpenGL`), `AZAHAR_CORE` pointing at
`azahar_libretro.dylib` when it is not in the default place, and the game data on the virtual SD at
`.lr3ds/save/Azahar/sdmc/3ds/sh23ds/` with any file named `.lr3ds/save/Azahar/sdmc/3ds/dspfirm.cdc`.
Every run uses the software renderer and the emulated CPU at 100 %, the strictest settings Azahar has.
Quit every emulator when its run is done; the headless runs exit on their own.

## Smoke run (every change)

```sh
tools/lr3ds/smoke.sh                       # New 3DS model
LR3DS_MODEL="Old 3DS" tools/lr3ds/smoke.sh # the original 3DS model and the Old 3DS profile
```

Twelve scenarios: the title, New Game into the prologue, the observation deck, an apartment door, a
jammed door, a save point, Continue, healing, Born From a Wish, an indoor cutscene, Angela's scene in
the cemetery (outdoor tiles and a voice) and the pause menu. It fails when a run does not reach its last frame, and writes PNGs and a contact sheet
(`sheet.png`) to `.lr3ds/smoke/`. Look at the sheet: the test cannot tell a black screen from a picture.

## Warp autotest (area, door or streaming changes)

`autotest warps <first> <count>` in `debug.txt` loads door and warp destinations in turn and logs each
to `autotest.txt` on the SD card: missing textures, missing camera roads, load time, crashes. There are
570 destinations. Add `perf` to measure each room as well (see PERFORMANCE.md, "Every room"); a long
sweep can run in short pieces, since measured rooms are remembered in `autoperf_seen.txt`.

```sh
SD=.lr3ds/save/Azahar/sdmc/3ds/sh23ds
printf 'logos 0\nautotest warps 0 570\n' > $SD/debug.txt
tools/lr3ds/run.sh 3000 --frames 400000 --stop-on "$SD/autotest.txt:# done"
rm $SD/debug.txt
```

The log ends with `# done, next N`. With `first` above 0 the file is appended to, so a run can go in
pieces. PROGRAMS.md, "Autotest", explains each column.

Last full pass: 2026-10-18, all 570 on both models.

## Playthrough suite (story, program or combat changes)

`port/source/playtest.c` plays the game from scripts of `pt` lines (the README's debug table lists
them). The scripts and their runners are in `tests/playtest/`; logs and generated scripts go to
`.lr3ds/playtest/`.

| Runner | What it plays | Time in Azahar |
| --- | --- | --- |
| `pt.sh <name> [frames]` | one script | |
| `run_chain.sh` | the critical path by teleport (`c01`..`c10`, `pt fast 1` draws nothing), the six endings from `c10`'s save, then Born From a Wish (`c11`) | about 2 minutes |
| `run_walk.sh` | the critical path on foot (`w01`..`w10`, `pt walkmode 1`, James walks to every trigger over a nav grid), then Born From a Wish (`w11`). `FROM=w05` starts later | about 17 minutes (w08, the prison and labyrinth, takes 8) |
| `run_levels.sh` | the walk and the endings on other difficulty levels: `TAG=hard RIDDLE=2 ACTION=3` and so on (see the file) | about 17 minutes each |
| `run_boss.sh` | the boss fights with James' own weapons from the chain's saves: `b*g` in god mode (does each fight end the PC's way), `b*` without (balance) | about 35 minutes, most of it the fights without god mode |

Each chapter loads the save the previous one made (`pt save` / `pt load`, kept on the SD card under
`pt/`), so run a chain in order once before running single chapters. `SLOT=2` or `SLOT=3` uses
`.lr3ds/save2` or `save3` as the SD card, which lets two runs go side by side; give each copy the pack
(an APFS clone, `cp -c -R`, costs no space) and the saves its scripts load.

A script passes when it reaches `# end of script` with no `FAIL` line; `pt.sh` prints
`<name>: pass N fail N finished 1`.

### Last results (2026-10-18)

On 0.1.2 (rerun during the clean-up):

- The teleport chain, all six endings and Born From a Wish pass, after `c03` was reordered to take
  the flashlight first. 0.1.2 made needs-light events in dark rooms wait for the flashlight, as the PC
  does (0x48e560: `0x47b1f0`, the room record's dark flag, then `gFlashLightAcquired`), and the old
  script picked up the apartment map, the handgun and a 3F item before it.
- The walk is broken: `w02` fails in the forest (stage 4, "no way on foot from ca12 to ca11") and
  everything after it fails with it. `w03` has the same flashlight reorder as `c03` but has not run
  since. The last full walk pass was on the build before 0.1.2's console fixes, which also flipped
  the walker's turning (`playtest.c` `walk_update`), so look there first.
- The runners now stop at the first failed chapter, since every later chapter loads its save.

On the build before 0.1.2's console fixes:

- Normal levels: the teleport chain, all six endings, Born From a Wish and the whole walk pass.
- Beginner (riddle 0, action 0), hard (riddle 2, action 3) and the extra riddle level (riddle 2 with
  clear data, action 2): everything passes except one step of `w10` on all three, `FAIL ev 2 (stage
  43): no way on foot from ru13 to ru13`. Normal passes it. Not yet looked into.
- Not run: the easy action level, and riddle and action combinations other than these four.
- Bosses in god mode all end the PC's way. Without it the driver beat the first Lying Figure, the
  Abstract Daddy, Eddie's first round and Mary, and lost the rest; a player wins those by running or
  with more health items, which the driver does crudely.

Puzzle answers are set with debug keys before the scripts play them, and the ending scripts set the
Maria escort and damage counters, which the teleport chapters do not build up as a player would.

## Browser packer (converter or packer-page changes)

`site/packer.html` runs the converters in Pyodide (tools/packer-web/README.md). Three checks, all on the
real game folder:

- `tools/texenc/check.py <pack>`: every texture of a native pack (built with its png folders) re-encoded
  by `t3x.py` + texenc against tex3ds's file. Header and level 0 byte for byte at high quality,
  mipmaps by PSNR.
- `node tools/packer-web/run.mjs "Silent Hill 2" <out>` then `compare.py <out> <native pack>`: the
  whole engine in Node, compared file by file. Fails on a missing file or on any difference outside
  textures and movies.
- `node tools/packer-web/e2e.mjs "Silent Hill 2" <dir>`: the page in headless Chrome, from the folder
  input to the saved zip.

Last run 2026-10-19: all three pass, and the smoke run passes on the zip the page made. The numbers are
in tools/packer-web/README.md.

## On a console

Nothing has been measured on a console yet, and it is the biggest open question. Version 0.1.1 ran on
one console once; 0.1.2 fixes what that showed (characters too dark, cutscene audio stutter, left and
right swapped, the flashlight button before the flashlight), but those fixes need confirming.

Put `debughud 1` and `perflog 1` in `debug.txt` (SELECT twice shows or hides the debug page in play),
then send back `perf.txt` from `sdmc:/3ds/sh23ds/` and a photo of the debug page. Worth checking, in
order:

1. The 0.1.2 fixes: James and the enemies lit as in Azahar, no stutter in the first Angela scene,
   turning and positional sound the right way round.
2. The audio line on the debug page says `core 2` on a New 3DS (`core -2` means the fallback), and its
   `gaps` count stays at 0 in cutscenes and area changes.
3. Frame times (`frame max` in `perf.txt`) in the town walk (cb03 west to cb19), a fog room (the
   observation deck), a dark room with the flashlight and an enemy (shadow volumes), and the hospital's
   busiest rooms (hp198, hp202). Azahar does not time the GPU, so these are the first real numbers;
   `quality.h` has the knobs if a scene is too slow.
4. Area changes and streamed town tiles: how long the black frame lasts, and whether walking fast
   through the town ever shows a missing tile (the reader has about 7000 units of walking to load one).
5. Movies: the picture sits right (MOVIES.md, the Y2R line nudge), the sound keeps in sync.
6. On an original 3DS: the Old 3DS profile is picked, frame times hold 30 fps, music and movies keep
   up at 268 MHz, and the Circle Pad Pro works if there is one.

`tools/lr3ds` can't stand in for these: Azahar finishes Y2R conversions at once, does not count GPU
time, and serves SD reads faster than most cards (`sdthrottle <KB/s> [open ms]` in `debug.txt` simulates
a slow card, see PERFORMANCE.md, "Slow SD cards").
