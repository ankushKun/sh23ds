# Debugging sh23ds

How to start the game in a particular state, run it in an emulator, and drive it headlessly. The test
suites built on these are in [TESTING.md](TESTING.md).

## The debug file

A text file at `sdmc:/3ds/sh23ds/debug.txt` changes how the game starts, one setting per line. Delete
it to play normally.

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
| `boat [x z yaw]` | start rowing on the lake (stage 38) at this place, e.g. `spawn cd01 -14970 3592 -98254 -90 2` with `boat -12338 -70000 0`; yaw in radians, default the PC's embark place (0x8cf698) |
| `demo <name> <seconds>` | play a cutscene from this point |
| `movie <name>` | play a movie |
| `newgame 1` | run New Game without pressing anything |
| `results 1` / `itemget <id>` | open the results screen, after counting these item pickups |
| `debughud 1` | start with the debug page on the bottom screen (SELECT twice shows or hides it in game) |
| `viewer 1` | add the room viewer to the title's touch list |
| `logos 0` | skip the start-up screens |
| `sdthrottle <KB/s> [open ms]` | simulate a console's slower SD card |
| `dumpframe <n>` | save frame *n* as `frame_<n>.bmp` and its stats as text; a 3D frame also as `frame_<n>_right.bmp` |
| `perflog 1` / `memlog 1` / `memtest <n>` | frame time breakdown, memory use, a memory stress test |
| `gpucost 1` / `mapcull 0` / `fogfill <tenths>` | the GPU cost model in perf.txt, the map sent whole, the fog's area per eye in 3D (PERFORMANCE.md, "3D") |
| `mapmips 1` | sample the map textures' mipmaps (off by default: the PC makes its map textures with one level, and the mipmaps of its atlases drew lines along the ground's quad edges) |
| `streamsync 1` | load streamed tiles on the main thread, to compare with the worker |
| `autotest warps <first> <count>` | load every door and warp destination in turn and log the result |
| `pt <command>` | a playtest script line, logged to `ptlog.txt`; the commands are listed in [`playtest.c`](source/playtest.c) |
| `rendermode <n>` / `batch <n>` / `freeze 1` / `ambient <percent>` | renderer experiments |

The playtest commands include `ev <n>`, `use <item>`, `combine <a> <b>`, `press <buttons>`,
`answer <k>`, `kill`, `attack`, `expect flag <n> <0|1>`, `save` / `load <name>` and `fast 1`;
`riddle <levels> <command>` runs the command only on those riddle levels (`riddle 2,3 press A`). With
`walkmode 1`, `ev <n> [@stage]` walks James there through the doors, and `walkto <x y z>`,
`blocked <x y z>`, `collect`, `fight [s] [radius] [flag n]` and `cube <outer> <inner>` work on foot.
`where` logs James' position.

## In an emulator

[Azahar](https://azahar-emu.org/) runs the port. Set the system to New 3DS (or Old 3DS for the Old 3DS
profile), put the game data in its virtual SD card under `3ds/sh23ds/` (the `sdmc` folder in Azahar's
user directory), and add any file named `3ds/dspfirm.cdc`, since Azahar's audio doesn't need the real
firmware. Then open `port/sh23ds.3dsx` or install the CIA.

If the top screen stays black in the desktop app, switch the graphics API between Vulkan and OpenGL,
or clear Azahar's shader cache for title `0000000000000000`. A `dumpframe` line in `debug.txt` tells you
whether the game drew anything.

For 3D, `--opt citra_factor_3d=100` on a headless run sets the slider the game reads, but Azahar's
software renderer shows only the left eye, so use `dumpframe` for the right one. With both eyes drawn
the game often runs at 30 fps, so a frame number can be reached about half as early in emulator frames.

## Headless runs (macOS)

`tools/lr3ds` is a small libretro frontend that runs a build in Azahar's libretro core with no window:
software renderer, New 3DS model, scripted buttons and screenshots at chosen frames.

```sh
clang -O2 -Wall -o tools/lr3ds/lr3ds tools/lr3ds/lr3ds.c -framework OpenGL
export AZAHAR_CORE=/path/to/azahar_libretro.dylib
tools/lr3ds/run.sh 600 --frames 600 --shot 590:/tmp/title.bmp
tools/lr3ds/run.sh 600 --frames 480 --press 300-303:A --analog 320-400:0,1 --shot 470:/tmp/walk.bmp
tools/lr3ds/smoke.sh          # 12 scenarios from the title to a save point, with a contact sheet
tests/playtest/run_chain.sh   # the whole story by teleport, every ending, Born From a Wish
```

`run.sh` uses `.lr3ds/save/Azahar/sdmc/` as the SD card, so the game data goes in
`.lr3ds/save/Azahar/sdmc/3ds/sh23ds/`. Every option is listed at the top of
[`lr3ds.c`](../tools/lr3ds/lr3ds.c).
