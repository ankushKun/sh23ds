# Old 3DS support

sh23ds was built for the New 3DS (804 MHz, L2 cache, 124 MB). This page covers what makes it run on
an original 3DS, 3DS XL or 2DS as well: the hardware profile and its options, the memory mode and
split, what the low quality level cuts, the controls on a console without ZL, ZR and the C-Stick,
and what was measured in Azahar's Old 3DS model.

Nothing here has been run on a real Old 3DS. Azahar does not model the clock speed of either model
(it does not implement `ConfigureNew3DSCPU` and charges about one tick per instruction at 268 MHz
for both), it does not model the PICA's drawing time, and it does not limit the application's
share of core 1. Its Old 3DS model does model the memory modes, the missing core 2 and
`APT_CheckNew3DS`, which is what the tests below rely on.

## The profile

`profile.c` detects the console with `APT_CheckNew3DS` before anything else starts and picks the
Old 3DS or New 3DS profile. Four keys in `options.txt` override it and its defaults. The options
screens (`optmenu.c`) show them as Quality, Frame Rate, Hardware and Layout on the System tab, keep them with the other settings, and hand every change to
`profile_set_choices` through `options_apply`:

| options.txt | Values | Default (Auto) | What it sets |
| --- | --- | --- | --- |
| `hwprofile` | 0 Auto, 1 Old 3DS, 2 New 3DS | the console | the default quality level, frame rate and movie set |
| `quality` | 0 Auto, 1 Low, 2 High | Low on the Old 3DS profile, High on the New | streamed tiles, fog sprites, shadows, character lights, movie set |
| `ctrllayout` | 0 Auto, 1 Old 3DS, 2 New 3DS | the console | which buttons the controls code may use (see Controls) |
| `framerate` | 0 Auto, 1 30 fps, 2 60 fps | 30 on the low quality level, 60 on the high one | the frame rate cap |

Whatever the profile, a New 3DS always runs at 804 MHz with the L2 cache (the profile only picks
settings). Texture and tile changes apply from the next room load; the rest at once.

Threads: on a New 3DS the music and movie threads run on core 2 as before. On an Old 3DS
`profile_init` asks for 30 % of core 1 (`APT_SetAppCpuTimeLimit`; 3dbrew notes more does not help
on the Old 3DS), the movie's sound thread runs there, and the music thread and the movie's picture
decoder run on core 0 below the main thread, which at 30 fps spends most of every frame waiting for
the vertical blank. In Azahar the five-layer AIX music of the hospital took 13 % of core 0 there,
but 36 % of the time on core 1, where the 30 % share stretches it; on hardware slower than
Azahar's count that would outgrow core 1, so the music stays on core 0. `helpercore N` in
`options.txt` forces both onto core N for tests.

| Setting | High (New 3DS) | Low (Old 3DS) | Where |
| --- | --- | --- | --- |
| Touching tiles resident (town, forest, lake) | 4 within 7000 units | 2 within 5000 units | `main.c` `stream_update_ex` |
| Fog sprites | 144 | 72 (the alpha of the rest goes to the drawn ones, as before) | `fog.c` `set_partnum` |
| Character shadows (stencil volumes) | on | off | `shadow.c` `shadow_begin` |
| Point and spot lights per character | 4 (3 with the flashlight) | 2 (plus the flashlight) | `chrlight.c` |
| Movies | `movie/<name>.mpg` | `movie/half/<name>.mpg` when the pack has it, drawn at twice the size | `movie.c` |
| Frame rate cap | 60 | 30, so every frame lasts two vertical blanks (`framerate` overrides it) | `C3D_FrameRate` |
| Supersampling | 2x or 2x2 while the GPU has the time | none | `supersample.c` |

The Picture (Soft or Sharp, `picture.c`) is the same on both: its soft pass is one copy and two
full-screen quads, well inside a 30 fps frame. The PC's own Shadows (On / Off) and Fog (Complex / Simple) rows on the Display tab still work on
top (the explainer says so when the quality is Low): shadows are drawn when both allow them, and the fog sprites need Complex and are capped by the
quality level. The test keys `texhalf` and `helpercore` are read at start-up only; optmenu.c does
not keep them when it rewrites the file.

The pack keeps textures at the PC's sizes (512 for the map, up to 1024 x 512 for models,
`VISUALS.md`). The Old 3DS profile keeps them at the sizes packs had before, 256 for the map and 512
for models: `t3xcap.c` reads only the levels from that size down, so the card reads and the memory are
what they were (`profile_tex_max_map` / `_chr`, `texfull 1` or `2` in `options.txt` to force either).
Memory, not the profile, decides two more things. Room textures load from one level further down
(half the width and height again, a quarter of the memory, from the same `.t3x` files) only when the
linear heap is under 32 MB, as for a 3dsx started from a title with little memory; `texhalf 1` or `2`
in `options.txt` forces it on or off. The texture cache of earlier rooms keeps 24 MB with 80 MB of
linear heap or more and 6 MB below that. Halving was the Old 3DS profile's default at first, but the
measurements below showed room textures to be a small part of a room's memory: `memtest 1` over
ten rooms of different areas in the 64 MB mode had only 0.2 to 0.6 MB more linear memory free with
them halved, for a visibly blurrier picture.

Changes that help both profiles: AIX music layers that are faded out for a whole buffer are no
longer decoded, and the ADX inner loop reads two nibbles per byte with the version test hoisted
out (bit-exact with the old loop, checked on 20,000 random frames).

## Controls

The Old 3DS has no ZL, ZR or C-Stick. The bindings, their defaults per layout and the remap page are
`controls.c`'s (port/CONTROLS.md has the table); `profile.h` gives it and the touch screen code the
layout and the action list:

```c
ControlsLayout controls_layout(void);        // CONTROLS_LAYOUT_OLD or CONTROLS_LAYOUT_NEW, read every frame
bool controls_have_cstick(void);             // the New layout
typedef enum { ACT_RUN, ACT_ACTION, ACT_AIM, ACT_WEAPON_NEXT, ACT_FLASHLIGHT, ACT_ITEMS, ACT_LOOK,
               ACT_TURN180, ACT_STRAFE_LEFT, ACT_STRAFE_RIGHT, ACT_MAP, ACT_PAUSE, ACT_CANCEL,
               ACT_COUNT } ControlsAction;
```

`controls.h` includes `profile.h` when it is there and has the same declarations as a fallback,
so the two batches merge in either order. `ctrllayout` in `options.txt` (Auto follows the console,
then Old 3DS or New 3DS) picks the layout, shown as Layout on the Options' System tab. The New layout on an
Old 3DS is for players with the Circle Pad Pro: `profile.c` makes libctru open `ir:rst` on every
model (`hidShouldUseIrrst`), which is how the add-on's ZL, ZR and right stick reach `hidKeysHeld`
exactly like the New 3DS's own buttons. Without the add-on nothing changes.

`controls_default()` keeps every action reachable on the Old layout with L, R, A, B, X, Y, the D-Pad,
the Circle Pad, START, SELECT and the touch screen: run B, action A, aim R, look L, next weapon D-Pad
up, quick turn D-Pad down, strafe D-Pad left and right, flashlight X, items Y, map on the touch screen
or in the pause menu, brightness in the pause menu, pause START.

## Memory

`profile.c` replaces libctru's `__system_allocateHeaps` (the same steps as libctru 2.x) so the
malloc heap is sized from the memory the process can still commit instead of a fixed 32 MB, and
the linear heap (textures, vertex, index and sound buffers) gets the rest:

| Memory the process has | malloc | linear | Where |
| --- | --- | --- | --- |
| 124 MB mode (New 3DS CIA) | 32 MB | 90.6 MB | as before |
| 96 MB (a 3dsx in Azahar's Old 3DS model) | 20 MB | 74.6 MB | |
| 80 MB mode | 19 MB | 59.4 MB | `SH23DS_SYSTEM_MODE=80MB` |
| 64 MB mode (Old 3DS CIA, a 3dsx from the Homebrew Launcher) | 15 MB | 47.4 MB | |

malloc gets a quarter of what is left, between 12 and 20 MB, and 32 MB from 100 MB up.

### The memory mode

The CIA keeps `SystemMode` at the default 64 MB (`SystemModeExt` stays 124 MB for the New 3DS). The
measurements below show the Old 3DS profile using at most about 15.5 MB of linear memory and 5.3 MB
of malloc, so the 47 MB linear heap of the 64 MB mode has more than 30 MB to spare. An extended mode
(80 or 96 MB, `SH23DS_SYSTEM_MODE` in `package.sh`) would cost the player more than it gives:

- The console restarts into the other memory layout when the game starts and again when it closes
  (as with the Old 3DS games that use these modes, such as Pokemon Sun and Moon or Smash Bros.).
- The SYSTEM region shrinks from 44 MB to 28 MB (80 MB mode) or 12 MB (96 MB mode), so the internet
  browser and Miiverse cannot open during the game, the HOME Menu is slower to open, and Luma's
  Rosalina plugins and NTR lose their memory.

If a later batch needs more, 80 MB is the mode to try first: 96 MB leaves only 12 MB to the system.

### Measured (Azahar, Old 3DS model, 64 MB CIA, Old 3DS profile)

| Run | Linear free, lowest (of 47.4 MB) | malloc in use, highest |
| --- | --- | --- |
| Boot | 42.5 MB | 2.4 MB |
| Warp autotest, all 570 doors and warps (final build, full-size textures) | 31.9 MB | 5.3 MB |
| The same with room textures halved (the first default) | 35.6 MB | 5.5 MB |
| Town walk cb03 to cb19 (two streamed tiles) | 38.7 MB | 2.5 MB |
| Hospital hp01, boss ru25, cutscene first_toilet, ap06 with a Lying Figure | 41.1 .. 41.8 MB | 2.6 MB |
| Movie `hei` (8 decoded frames queued) | 53.1 MB free after it, of 59.4 (80 MB mode) | |

The other rows had room textures halved (the first default). With full-size textures the town
walk's lowest was 38.0 MB, and `memtest 1` 0.2 to 0.6 MB lower per room than with halved ones.

The warp autotest (`autotest warps` in two halves of 285 destinations) loaded every destination
with its room, collision, textures (no missing texture anywhere) and camera roads, with the same one
known exception as on the New 3DS (st51 ev8 to the lake, which has no collision on the PC either).
Room loads took 596 ms on average and 1.5 s at most in Azahar. The 11 smoke scenarios pass on the
same 64 MB image.

## CPU and frame times

Azahar counts about one 268 MHz tick per emulated instruction on both models, so its numbers are
neither the New 3DS (three times the clock, plus the L2 cache) nor the Old 3DS (the same clock, but
slower memory and more than one cycle for most instructions). To bracket a real Old 3DS the town
walk also ran with Azahar's CPU clock at 50 % and 25 %, which charges every instruction two and four
ticks. The GPU's drawing time is not modelled at all (the "gpu" column stays near zero), and the
PICA200 is the same chip on both models.

Times in ms per frame, from `perflog` (60-frame windows after the first two), Old 3DS model, Old 3DS
profile, 30 fps cap. "upd" is the game update, "c3d" the CPU time building the frame's GPU commands,
"window max" the slowest frame of a 60-frame window (33.3 means every frame made its two blanks).

| Scene | upd | c3d | Window max, typical | Music thread |
| --- | --- | --- | --- | --- |
| Town, running cb03 to cb19 (80 MB CCI) | 1.21 | 3.3 | 33.8 | 13 to 14 % of core 0 |
| same, CPU clock 50 % | 2.44 | 6.2 | 34.5 | 17 to 22 % |
| same, CPU clock 25 % | 5.14 | 12.1 | 37 to 68 (tile joins) | 33 to 41 % |
| Hospital hp01, five-layer AIX music | 0.88 | 1.5 | 33.5 | 13 % on core 0 (36 % of the time on core 1) |
| Boss ru25, two enemies | 0.49 | 2.2 | 33.6 | 13 % |
| Cutscene first_toilet | 0.10 | 1.5 | 33.5 | 9 % |
| ap06, a Lying Figure in the flashlight (shadows off) | 0.82 | 2.7 | 33.6 | 5 % |

So even with a CPU four times slower than Azahar's count the main thread needs about 17 ms of the
33 ms, and the frames that miss are the tile joins (18 to 31 ms of `room_load_mem` on the main loop
at 25 %) and the stage model loads below. With the cap the frames that fit come at an even 33.3 ms
instead of alternating between 16.7 and 33.3.

`stagemdl_draw` (stagemdl.c) used to load one stage-placed model per frame on the main thread,
inside the render section: 58 to 185 ms each in Azahar, five in the first five frames of the town
and one more whenever James walked into range of another. They now come through the reader thread
and load behind the fade or in about a millisecond (PERFORMANCE.md, Frame rate).

### New 3DS profile

The New 3DS profile keeps every setting it had (12 MB texture cache, four tiles within 7000
units, 144 fog sprites, shadows, four character lights, 60 fps), the same 32 MB / 90.6 MB memory
split, music on core 2, and gains the skipped silent AIX layers and the tighter ADX loop.
Measured against the main tree's build of the same day on Azahar's New 3DS model:

| Scene | This batch: upd / c3d / window max | Main tree: upd / c3d / window max |
| --- | --- | --- |
| Town walk cb03 to cb19 | 1.15 / 4.04 / 17.1 (worst 157, stage models) | 1.15 / 4.17 / 16.9 (worst 157, stage models) |
| Boss ru25 | 0.49 / 4.16 / 16.8 | 0.50 / 4.27 / 16.8 |

The smoke scenarios pass on both models (`LR3DS_MODEL="Old 3DS" tools/lr3ds/smoke.sh`; the memory
numbers were taken with a `.3ds` image, see below).

## Movies

`movielog 1` with `movie hei` (400x240, 29.97 fps), in Azahar:

| Model, profile | Movie | Picture decode per frame, average / max | Sound, per 72 ms buffer | Dropped |
| --- | --- | --- | --- | --- |
| New 3DS, core 2 | full size | 5.65 / 43.8 ms | 5.1 ms | 0 of 298 |
| Old 3DS, core 0, high quality | full size | 5.34 / 20.5 ms | 15.0 ms on core 1 | 0 of 295 |
| Old 3DS, core 0, low quality | half size (200x120) | 1.68 / 8.8 ms | 14.5 ms on core 1 | 0 of 296 |

In Azahar's count pl_mpeg decodes the full-size movies in a sixth of the frame time, and the eight
queued frames absorb the slow I-frames. A real Old 3DS is slower than that count by an unknown
factor, so the packer can also write a half-size set (`movies.py --half`, `HALF_MOVIES=1` for
`make_pack.sh` and the packer image, about 75 MB more on the SD card), which the low quality level
plays when it is there, drawn at twice the size with linear filtering. Without it the full-size
movies play on every profile. The half-size files were checked for `hei`, `toilet` and `open`.

## Testing it yourself

Old 3DS runs need Azahar's libretro core with `--opt 'citra_is_new_3ds=Old 3DS'`. A
`.3dsx` started that way gets 96 MB (Azahar's choice for a 3dsx on the Old 3DS), so the memory
numbers above come from `.3ds` images made with the CIA's settings (`makerom -f cci` with the same
RSF and `-DAPP_SYSTEM_MODE=64MB`), which Azahar loads with the exheader's memory mode. Azahar's
core does not implement `ConfigureNew3DSCPU`, so the 804 MHz switch only shows as a log line.

## Not verified

- Nothing ran on a real Old 3DS. The frame times above are Azahar's instruction count; the 50 % and
  25 % clock runs are a bracket, not a measurement. GPU time, SD card speed and the cost of the
  missing L2 cache are not modelled.
- `APT_SetAppCpuTimeLimit(30)` and threads on core 1 under the CIA's exheader (AffinityMask 1); Azahar
  accepts both. If core 1 is refused, the movie sound thread falls back to core 0.
- The Circle Pad Pro through `ir:rst` on an Old 3DS (`hidShouldUseIrrst`); Azahar's Old 3DS model
  has no add-on to test with.
- The restart and applet costs of the extended memory modes are from 3dbrew and reports about retail
  games, not tried with this CIA.
