# Performance on the New 3DS

This page records where the main loop spends its time, what was moved off it, and how the memory is
split. The numbers come from Azahar (software renderer, New 3DS mode, headless through `lr3ds`),
so treat them as estimates until someone repeats them on a console with the same debug switches.
Azahar counts emulated CPU time and also charges time for SD card reads, but it does not model the
PICA's drawing time, so the "gpu" column below is close to zero everywhere and says nothing.

## Measuring

`source/perf.c` times every frame with `svcGetSystemTick` and asks citro3d for its own numbers:

- **upd**: from the top of the main loop to just before `C3D_FrameBegin` (input, audio, game
  logic, cutscenes, tile streaming).
- **render**: from there to the end of `C3D_FrameEnd`. Most of it is waiting for the vertical
  blank, so a frame that made 60 fps shows about 15 ms here whatever it drew.
- **c3d cpu**: `C3D_GetProcessingTime()`, the CPU time citro3d spent building the GPU commands.
- **frame max**: the slowest single frame (upd + render) of the second. 16.7 ms means every frame
  made the vertical blank; 33 or 50 means one or two were missed.

Switches in `debug.txt` on the SD card (`sdmc:/3ds/sh23ds/debug.txt`):

| Line | Effect |
| --- | --- |
| `debughud 1` | bottom screen debug page, with the last second of timings and the free memory |
| `perflog 1` | appends a line per 60 frames to `perf.txt` (with the free linear memory and the music decoding's share of a core), plus a line for every frame half a refresh over its time (25 ms at 60 fps, 41.7 ms with the 30 fps cap), with the time of each named part of that frame, one per streamed tile and stage model, and after each per-60-frames line a `parts` line with every named part's average and worst over those frames |
| `memlog 1` | appends the free linear, VRAM and heap memory to `mem.txt` at boot and after every area change, with the lowest linear free and the highest malloc use since the line before |
| `memtest N` | warps through ten rooms of different areas N times, one every 150 frames, and logs memory after each (implies `memlog`) |
| `streamsync 1` | reads streamed tiles, a door's destination and its tiles on the main loop as before, for comparison |
| `sfxsync 1` | reads a sound effect's sample on the main loop at its first play, as before |
| `shadowdebug N` | character shadows: 1 shows the volumes in red, 2 darkens every changed stencil count, 6 turns them off |
| `autotest warps F N perf` | the warp autotest over destinations F to F+N-1, measuring each room once: 90 frames from the 10th after the load, logged to `autotest.txt` as a `perf` line (frames, slowest frame, frames over 17 and 33 ms, average update and citro3d time, most work in one frame). Rooms listed in `autoperf_seen.txt` on the SD card are skipped, and each measured one is added, so a long sweep can run in short pieces |
| `fogsprites N`, `shadows 0/1`, `framecap 0/30/60`, `tiledist N`, `fogfill N` | override one `quality.h` knob (see Frame rate) for a test; `fogfill` in tenths of a screen, 0 for no limit |
| `gpucost 1` | the GPU cost model (`source/gpucost.c`): a `gpucost` line every 60 frames with each pass's pixels and vertices per eye (see 3D) |
| `mapcull 0` | sends every map triangle, as before the map was culled per eye (see 3D), for comparing GPU times |

The log lines are kept in memory and written once a second, after the frame's timing has been
taken, because an SD card write costs tens of milliseconds and would otherwise show up as the very
hitch being measured. Leave `perflog` off for normal play all the same.

## Results

Before means this batch's code with the instrumentation but without the fixes below (music decoded
on the main loop, tiles read on the main loop); after is the merged state. Times are in ms; "upd"
is the average with the worst frame in brackets.

| Place | Before: upd | Before: frame max | After: upd | After: frame max | c3d cpu |
| --- | --- | --- | --- | --- | --- |
| Town east, running west from cb03 through cb11 to cb19 (fog, 3 or 4 neighbouring tiles) | 4.3 (33.4), tile loads 385..490 | 50 every second, 418..501 when a tile came in, 128..178 on a tile change | 1.1 (8.5) | 17.2 (19.4 once) | 2.9..3.8 |
| Hospital 1F hp01 (layered AIX music) | 5.2 (33.3) | 50 | 0.86 (0.93) | 16.8 (17.8 once) | 1.4 |
| Boss fight ru25 (hotel, two enemies, stage 44) | 1.6 (13.3) | 16.8 | 0.49 (11.6 once) | 16.9 | 2.1 |
| Cutscene `first_toilet` in er01 | 1.1 (5.9) | 16.8 | 0.10 (0.14) | 16.8 | 1.5 |

With the fixes James also gets about 800 units further along the street in the same 1500 frames,
since the clamped frame time no longer loses distance on long frames.

The first frame after a room load is always slow because the load runs there; it sits behind the
fade and is not counted above. What it costs and what was taken out of it is under "Room loads".

Three things caused the hitches, and the per-part times on the slow frame lines pointed at each:

1. **Music decoding.** `audio_update` refilled a 4096-sample ndsp buffer whenever one finished,
   and decoding a buffer of layered AIX music (five layers in the hospital and the town) took
   25..32 ms. That happened every 6 or 7 frames and dropped a frame each time, which is the
   "50 every second" above. `audio.c` now refills on its own thread (see below).
2. **Tile reads.** `stream_update` read a neighbouring tile's `.s3m`, `.s3l` and missing textures
   with `fopen`/`fread` on the main loop: 240..440 ms per town tile in Azahar.
3. **Tile changes.** Walking onto a neighbour re-read `lighting.txt` (200 KB), `chrlight.txt`
   (77 KB) and the footstep table from the SD card, 128..178 ms in all, and dropping a far tile
   waited for the GPU with `C3D_FrameSync` (13 ms, one missed frame).

What is left on a tile change is the main thread's share of a new tile (4..7 ms: copying the
vertices to linear memory and creating its textures) and the collision merge (under 0.1 ms).

### Second pass

| Place | Upd average (worst after the first frame) | Frame max | c3d cpu | Notes |
| --- | --- | --- | --- | --- |
| Town walk as above | 1.1 (1.2) | 16.9 | 2.9..3.8 | no slow frame at all after the first |
| Hospital hp01 | 0.87 (0.91) | 16.8 | 1.4 | |
| Boss ru25 | 0.51 (0.56) | 22.0 once | 5.6 | James dies after 10 s, the run ends at the title |
| Cutscene `first_toilet` | 0.10 (0.15) | 16.8 | 1.5 | the first frame was 34 ms of starting the music, now under 1 ms |
| ap06 corridor, a Lying Figure in the flashlight | 0.80 (0.87) | 16.9 | 4.6 with shadows, 2.8 without | 2 casters, about 2200 shadow triangles, 1.5 ms of CPU to build them |

The c3d cpu column includes everything the main thread does between `C3D_FrameBegin` and
`C3D_FrameEnd`, so the shadow volumes show up there: 1.5 ms of building for two casters plus the
two stencil passes and the darkening quad.

The drop shadows of the lit rooms cost less: about 230 triangles and 0.4 ms of CPU for a Lying
Figure, one translucent draw. The fog's wall test runs over the walls within 5000 of the fog
centre (3 to 6 in the town and the prison), and the Eddie rooms add up to 48 blow puffs to the
fog sprites.

### Sound effects

A sound effect's first play read its sample from `se.bnk` on the main loop: with `sfxsync 1` the ap06
fight above has five frames of 10..24 ms in the game logic, one per new sound. `sfx.c` now asks a
reader thread for the sample (its own handle on `se.bnk`, the linear buffer allocated on the main
thread first) and starts the voice in `sfx_update` once it is in, a frame or two later; the same
fight has no slow frame. The resident set (footsteps, menus, pickup, flashlight) is still read at
start, and a door's close sound while the door fades out. The bottom screen's sfx line counts the
reads done each way.

### Starting music

Starting a track (`audio_play_music` / `audio_play_layers`, from `bgm.c` when the area or the page
changes) opened the file, read the first AIX chunk and decoded four buffers on the main loop, and
could wait for the refill thread's lock on top: 20..130 ms. Start and stop are now requests the
refill thread carries out (`audio.c`), so the call returns at once and the track starts a frame
later. A missing file then just stays silent, and `audio_voice_playing` covers the frame in between
for the cutscene voices.

## Room loads

With `perflog 1`, a room load writes the parts of its frame (`ld_stage` the stage's events,
`ld_list` the area's room list and bounds, `ld_col` collision and footstep table, `ld_room` the
map and its textures, `ld_tiles` the touching tiles, `ld_enemy` enemy models, `ld_cam` the camera
roads, `ld_env` lighting). Measured with `memtest 1` over the same ten rooms, the update time of the load frame in ms (Azahar):

| Room | Before | After the index files | Through a door | With stage and enemy prefetch |
| --- | --- | --- | --- | --- |
| ap06 | 3156 | 1428 | 593 | 91 |
| cb03 | 5617 | 753 | 223 | 50 |
| er05 | 874 | 623 | 222 | 88 |
| bw05 | 1098 | 1020 | 208 | 74 |
| ps189 | 4787 | 1128 | 208 | 91 |
| rr91 | 2853 | 1154 | 207 | 73 |
| ap38 | 2938 | 1144 | 208 | 74 |
| hp230 | 4497 | 1004 | 258 | 91 |
| ca14 | - | 810 | 201 | 58 |
| rr49 | - | 719 | 207 | 73 |

What changed:

- **Area index.** Entering an area opened every room's `.s3m` for its bounds and every room's
  `.cam` for its camera roads, two files per room: up to 1.9 s each in the town, the hospital and
  the prison. `tools/sh2conv/areaindex.py` writes `bg/<area>/area.idx` with both for the whole area,
  and `areaidx.c` reads that one file (falling back to the single files for rooms it does not list).
- **Door prefetch.** When a door's fade starts, the destination room's `.s3m`, `.s3l`, textures and
  footstep table go to the tile reader thread (`warp_prefetch`), and the black screen waits for them
  (frames keep coming, music keeps playing, at most 6 seconds) before `load_current` takes them with
  `stream_take_named` and does only the GPU side.
- **Tiles after the warp.** In the town the touching tiles were loaded inside the warp frame (1.1 s
  for cb03). After a door they now come in through the reader thread while the fade stays black
  until they are resident (`s_arrival`, at most 6 seconds).

- **Stage files, enemy models and the area's index.** A door into another stage or area now also
  hands the reader thread the stage's event files (`s<NN>.s3e`, `.msg`), the `.s3c`, `.s3a` and `.kg1`
  of every enemy model the stage's list and scripts can need and that is not loaded yet
  (`enemy_prefetch_stage`; an `.s3c` brings its textures along), and the new area's `rooms.txt`
  and `area.idx`. `events.c`, `chr.c`, `areaidx.c` and `list_rooms` take the files from the reader
  (`stream_take_file`) and only fall back to the SD card for what it does not have; what nobody took
  is dropped after the warp.
- **Continue.** The title waits one second for its start sound before it loads the saved room, so
  that second now reads the room ahead the same way (`title_start`): the start frame went from 747 to
  243 ms in Azahar.

Since textures are packed at the PC's sizes (`VISUALS.md`), a door reads the room's map textures from
their 256 level and the models' from their 512 level, and the whole files follow once the room shows
(`stream_set_draft`, `room_tex_refine`, `chr_refine`): the same `memtest` on a 5 MB/s card keeps every
black screen within 0.1 s of the old pack's, and the rooms are whole 0.7 to 2.4 s later.

The last two columns have `memtest` going through the door path (prefetch and fade) like a real
door. A real door within one area (ap06 to ap38) went from an 825 ms frame to 28 ms, with the screen
black for half a second longer. What is left in an area change's frame, 50 to 90 ms in Azahar, is
work on the main thread: converting the camera roads (10..30 ms), the GPU side of the room (5..15
ms), the enemy models' GPU side the first time (10 ms for ap06) and the rest of the stage setup.
Debug spawns (`spawn` in debug.txt) still load everything in their first frame.

## Threads

| Thread | Core | Priority | Work |
| --- | --- | --- | --- |
| main | 0 | libctru default | game, drawing, GPU uploads |
| tile reader (`stream.c`) | 0 | main + 1 | reads `.s3m`, `.s3l`, uncached `.t3x` and the footstep `.sfm` of a requested tile into malloc'ed buffers |
| music refill (`audio.c`) | 2 (New 3DS), else 0 | main + 1 | ADX/AIX decoding into the ndsp wave buffers of the music and voice channels, and starting and stopping their streams |
| sound effect reader (`sfx.c`) | 0 | main + 1 | reads a sound effect's sample from `se.bnk` into the buffer the main thread allocated for it |
| movie decoder and audio (`movie.c`) | 2, else 0 (Old 3DS: picture on 0, sound on 1) | main + 2 / + 1 | only while a movie plays |

The tile reader spends almost all its time blocked in the file system, so core 0 is fine for it.
The music thread asks for core 2, the New 3DS's spare core, like the movie threads do; if the kernel
refuses, it runs on core 0 below the main thread and decodes while the main thread waits for the
vertical blank, which leaves it plenty of time. The bottom screen's audio line says which
(`audio ready, core 2`). Its lock is only taken where the main thread starts or stops a stream;
layer volumes and the voice position are plain words both sides read and write.

### Streamed tiles

`main.c` `stream_update_ex` decides every 8 frames which touching tiles should be resident (the
nearest 4 within 7000 units). For a missing one it calls `stream_request`, and the reader thread
fills a `StreamJob` with the file contents (`stream.c`, up to 6 jobs; a door's prefetch uses the
same jobs with the key `STREAM_PREFETCH`). The jobs are collected on a
later pass with `stream_take`, and `neighbour_add_job` does the GPU side on the main thread:
`room_load_mem` (linear memory, `C3D_TexInit` through `Tex3DS_TextureImport` from the blob) and
`col_load_mem`. The texture ids the cache already holds are passed to the job so they are not read
twice. A tile that left the wanted set while it was being read is thrown away. On an area change
`stream_flush` drops the queued jobs and results of other areas.

Dropped tiles go to a short list and are freed right after the next `C3D_FrameBegin`, which has
waited for the GPU to finish the frame that may still have drawn them; the texture cache trim waits
with them for the same reason.

Two rooms of the town's list are not blocks of its grid: cb90 (a quad 500 units across) and cb99
(a dark sphere 5000 across), both centred on the world origin. Their boxes touch the four blocks
round the origin, so they used to stream in as neighbours: at cb03 cb99 drew a black wedge over the
street, and the two took places from real blocks (cb12 was left out there). `room_is_block` now
takes only the blocks the PC's grid tables name (0x494ef0), which leaves out cb90, cb99 and the
forest's ca02 (GitHub #4). The neighbour culling (`room_visible`) also no longer tests a box with
corners behind the eye against the side planes, where their x and y flip sign.

## Memory

The CIA is built for the 124 MB mode on the New 3DS (`package.sh`: `APP_SYSTEM_MODE_EXT=124MB`) and the
normal 64 MB mode on the Old 3DS. libctru's default split of the 124 MB gives the linear heap 32 MB and
malloc about 91 MB, the wrong way round for this port: every texture, vertex and index buffer and sound
buffer is in linear memory, and malloc held at most 3.7 MB in any measurement. `profile.c` replaces
libctru's heap set-up and sizes malloc from the memory the process has: 32 MB in the 124 MB mode, which
leaves the linear heap about 90.6 MB, and 15 MB in the Old 3DS's 64 MB mode (47.4 MB linear). The 32
MB leave room for the tile reader's buffers (an `.s3m` of up to 1.6 MB plus textures per queued tile).
The Old 3DS numbers are in `OLD3DS.md`.

Free memory as `memtest 2` logged it (KB, right after each area change; the run ended during the
second round, after seventeen doors):

| Where | Linear free (of 93048) | Heap free (of 32768) | malloc in use |
| --- | --- | --- | --- |
| boot | 88281 | 30556 | 2211 KB |
| ap/ap06 | 86857, then 86798 (-59) | 30070, then 30061 (-9) | 2697, then 2706 KB |
| cb/cb03 | 83637, then 83627 (-10) | 30028, then 30018 (-10) | 2739, then 2749 KB |
| er/er05 | 87501, then 87490 (-11) | 30080, then 30077 (-3) | 2687, then 2690 KB |
| bw/bw05 | 86851, then 86841 (-10) | 30090, then 30087 (-3) | 2677, then 2680 KB |
| ps/ps189 | 86742, then 86732 (-10) | 30062, then 30059 (-3) | 2705, then 2708 KB |
| rr/rr91 | 86456, then 86445 (-11) | 30072, then 30069 (-3) | 2695, then 2698 KB |
| ap/ap38 | 86520, then 86510 (-10) | 30065, then 30063 (-2) | 2702, then 2704 KB |
| hp/hp230 | 86850 | 30067 | 2700 KB |
| ca/ca14 | 86671 | 30041 | 2726 KB |
| rr/rr49 | 87080 | 30051 | 2716 KB |

No leak shows up across room changes. A leak would make the second round fall further behind the
first with every door; instead every room of the second round has about 10 KB less linear memory
than in the first, the same 10 KB from the first room to the last, so something was allocated once
during the first round and kept. That fits a cache filling up (the sound effect samples are cached
in linear memory) rather than anything tied to rooms. The extra 59 KB in ap06 is the same effect in
the first room visited. VRAM stays at 5394 KB free throughout; malloc stays between 2.7 and 2.8 MB
in use, with a peak arena of 3.7 MB.

Textures are kept at the PC's sizes on the New 3DS (`VISUALS.md`): a room's map textures take at
most 3 MB (a median of 1 MB) where they took a quarter of that, and the texture cache of earlier rooms
keeps 24 MB. The supersampling's target takes 1.5 MB of VRAM the first time it turns on.

3D (`stereo.c`) takes nothing until the slider first goes up. Then the right eye's render target takes
750 KB of VRAM, and its fog and 2D vertex buffers 0.7 to 0.9 MB of linear memory.

## Frame rate

The goal here was a steady frame rate everywhere: an even 30 fps at least, 60 where the frame fits,
and no frame after a room load longer than 33 ms in the warp autotest. In Azahar both models now get
there. What Azahar cannot say is how long the PICA takes to draw a frame, so the fill-rate costs
(fog sprites, shadow volumes, translucent layers) still need a console; the knobs for them are in
`quality.h` below.

### Every room

`autotest warps 0 570 perf` (see Measuring) warps through all 570 door destinations and measures
each room once, 90 frames from the 10th after the load. Rooms that a second door leads to are
skipped, which leaves 229 rooms on the New 3DS model. The New 3DS sweep ran in pieces of 3,000
frames over a day in which `main` kept moving, so 134 rooms were measured on the batch 5 tree with
the first of these changes and the other 95 on the tree just before the save files; the rooms that
were slow then were measured again on the final build.

| Model, profile | Rooms | Slowest frame | Rooms with a frame over 17 ms | Over 33 ms | Update, median (worst) | citro3d CPU, median (worst) | Most work in a frame |
| --- | --- | --- | --- | --- | --- | --- | --- |
| New 3DS, high quality, 60 fps | 229 | 17.2 ms (cc53, cb03, cb47), 18.9 in cb68 on the final build | 1 | 0 | 0.82 (1.46, cb67) ms | 1.48 (4.86, hp198) ms | 12.7 ms (cc53, a town tile joining) |
| Old 3DS, low quality, 30 fps | 228 | 35.7 ms (cc53), 34.9 (cb68, ap104), every other room 33.8 or less | (all, at 30 fps) | 3 rooms, one frame each, none near a miss (50 ms) | 0.86 (1.44, cb67) ms | 1.23 (2.94, cb67) ms | 9.2 ms (ps67) |

On the Old 3DS profile every frame lasts two vertical blanks, 33.3 ms, so the measure there is a
missed blank, which would show as 50 ms. None did; the three frames of 34.9 to 35.7 ms are a frame
that ended a little after its blank and the next that ended correspondingly early, with 5 to 8 ms of
work in them. The Old 3DS sweep ran on the final build, in three pieces side by side; 221 of its
rooms are the same as the New 3DS sweep's and 7 are rooms the playthrough batches' door changes now
lead to.

"Work" is the update plus citro3d's CPU time, the part of a frame that does not wait for the vertical
blank: it has to stay under 16.7 ms for 60 fps. Half the rooms need under 2.5 ms of the 16.7, the
hospital's busiest (hp198, hp202) about 5, and the town peaks at 12.7 ms in the frame where a tile's
geometry joins. In Azahar's count that is about the Old 3DS CPU (268 MHz at roughly one tick per
instruction); a New 3DS at 804 MHz with its L2 cache has about three times that, so the New 3DS has
a wide margin on the CPU side and the open question is the GPU.

The first New 3DS sweep found three rooms over 33 ms, and each was a load in a frame of play:

- **cb68, 445 ms**: an enemy kind that came in with a streamed town tile after the warp read its
  model, animation and textures on the main loop. Enemy models now go through the reader thread
  (below); the room measures 18.9 ms at worst.
- **ps67, 33.7 ms**: the warp puts James in ps66's record but on ps67's floor, so the primary tile
  swapped in the first frames, and `sfx_floor_load` probed the SD card for a floor table ps67 does
  not have (23 ms in Azahar). A tile whose reader found no `.sfm` is now remembered as having none;
  the room measures 16.9 ms.
- **cb02, 34.4 ms**: the same enemy load as cb68. The door that led there now leads to cc11 (the
  playthrough batches changed it); cc11 measures 16.9 ms. No door in the current tree leads to cb02 itself.

### Walking the town

The town walk from Measuring (cb03 west through cb11 to cb19, 1,500 frames of running, perflog on),
with the same `main` with and without these changes. Old 3DS is the Old 3DS profile (low quality,
30 fps), so its frames are 33.3 ms when they make their two blanks.

| Model | Build | Update | citro3d CPU, median second | Typical second's slowest frame | Slow frames |
| --- | --- | --- | --- | --- | --- |
| New 3DS | before | 1.15 ms | 3.84 ms | 17.1 ms | 10: 83 to 211 ms in the first five frames, then 106, 123, 136 and 141 ms (stage models) and 29 ms (a tile joining) |
| New 3DS | after | 1.15 ms | 2.98 ms | 16.9 ms | 1: 46 ms in the first frame, behind the fade |
| Old 3DS | before | 1.21 ms | 3.11 ms | 34.2 ms | 9: 87 to 217 ms in the first five frames, then 140 to 157 ms (stage models) |
| Old 3DS | after | 1.22 ms | 2.43 ms | 33.7 ms | 1: 58 ms in the first frame, behind the fade |

The same walk on the final build (rebased on batch 17 and the map part switches) gave the same
picture: citro3d 2.99 ms on the New 3DS model and 2.41 on the Old, and one slow frame each, the
first (37 and 53 ms).

That first frame was the fog reading its area's `.fcl` (22 ms of `fog_fcl` in the perflog parts,
24 to 33 ms in all). The ten `.fcl` files are 50 KB together, so `fog_init` now reads them all at
start-up and a mode change only copies one; the walk from cb03 then has no slow frame at all.

### What changed

1. **Stage models off the main loop.** `stagemdl_draw` loaded a placed model (the town's items, the
   hotel's music box and the like) the first time one came within 14000 units, one per frame, inside
   the render section: 58 to 217 ms each in Azahar, mostly SD reads. Now a door's read ahead (`warp_prefetch`)
   asks the reader thread for the models placed within range of where James arrives (`.s3c`, its
   textures and its `.kg1`), and the room load sets them up behind the fade from what it read
   (`stagemdl_preload`). A model that comes into range later is asked for from 17000 units on
   and set up once its files are in, one a frame: 0.1 to 1.7 ms. `stagemdl.txt` is read once
   instead of at every stage change. A load no longer drops a model that is in range but later in the
   list, which made the town reload the same few models every second or so.
2. **Enemy models off the main loop.** A kind that arrives with a streamed tile (the town) used to
   read its model on the main loop when it spawned. `enemy.c` now asks the reader thread for the
   files and spawns it a few frames later, once they are in; at a room entry, behind the fade, the
   model still loads at once from the door's read ahead. `chr.c` no longer looks on the SD card for a
   file the reader was asked for and did not find (a model without a `.kg1`).
3. **Tile joins spread over frames.** The reader thread now also rebases a tile's indices
   (`room_prepare`), and the main thread puts the tile's textures into the cache about 2 ms a frame
   (`room_tex_warm`) before its geometry joins in a frame of its own. The geometry frame is 1 to
   4.8 ms in the town (it was 4 to 7.2 ms with the textures).
4. **Fewer state changes.** Room batches keep the texenv, material, texture and alpha test of the
   batch before when they match, and character draws skip a palette upload that matches the one just
   sent and only rebind the texture, texenv and alpha test when they change. citro3d's CPU time in
   the town fell from 3.8 to 3.0 ms on the New 3DS model and from 3.1 to 2.4 on the Old.
5. **Frame pacing.** `perf_pace` runs after every frame. With a fixed cap (30 or 60) it sets
   `C3D_FrameRate` once; with `frame_cap` 0 it runs at 60 while the frames' work fits and drops to
   an even 30 when frames keep missing (four in a row with more than 15.5 ms of work, or more of them
   spread out),
   going back to 60 after two seconds in which every frame would fit with room to spare (11 ms).
   That avoids the judder of frames alternating between one and two vertical blanks. The New 3DS
   profile's Auto frame rate uses it; the Old 3DS profile's 30 fps cap and an explicit 60 are fixed.
6. **The touch screen's map off the main loop.** The first frame after loading a save took 2.2 to 3.1
   s on a console (and 1.3 s on entering ob01), all of it in `r_touch`: the small map read its
   map and marks textures (about 1.3 MB) through Tex3DS's stdio reader while the tiles were being
   read. `map.c` now asks the reader thread for them (`textures_ready`) and draws the map once they
   are in; the map screen itself reads a map whole with `sd_fread` and waits for one already asked
   for.

### 3D

Drawing both eyes, in Azahar with `perflog 1`, slider at 0 then 100 (ms; update and citro3d CPU are
medians, the parts averages). Azahar does not time the GPU, so these are CPU figures only.

| Scene | Model | Update | citro3d CPU | r_map / r_chr / r_fog | Slowest frame |
| --- | --- | --- | --- | --- | --- |
| Town cb03 | New | 1.38 → 1.55 | 6.79 → 8.19 | 0.86 / 0.71 / 5.02 → 1.70 / 1.16 / 5.02 | 17.3 → 34.1 |
| hp198 | New | 0.93 → 1.11 | 3.21 → 4.89 | r_chr 2.68 → 3.89 | 17.2 → 34.1 |
| hp202 | New | 0.98 → 1.15 | 4.12 → 6.12 | r_chr 3.52 → 4.98 | 17.8 → 34.1 |
| Cutscene first_toilet | New | 0.14 → 0.26 | 1.32 → 2.40 | | 16.9 → 33.7 |
| Forest cutscene haka_agl | New | 0.15 → 0.27 | 5.01 → 7.67 | | 16.9 → 33.7 |
| Town cb03 | Old | 1.31 → 1.47 | 4.11 → 7.35 | r_fog 2.63 → 4.72 | 33.5 → 33.7 |
| Forest cutscene haka_agl | Old | 0.12 → 0.28 | 3.84 → 6.63 | | 33.5 → 33.5 |

The CPU work peaks near 10 ms of the 33 ms a 30 fps frame has. The GPU is what a console showed
to be short: with the slider up, every second in the fogged forest (ca13 to ca27) and the town ran
at 30 fps with the GPU at a median of 30 to 31 ms a frame and peaks of 34 to 38, so any busier
moment fell to 20 fps; at the lake shore (ca13) nine frames in a row took over 50 ms of GPU.

#### The GPU cost model

Azahar does not time the PICA, so `gpucost 1` in debug.txt (`source/gpucost.c`) counts what each pass
hands it: the screen area of every triangle, clipped to the screen and to the far plane, after back
face culling where the pass culls, and the vertices (the map is counted on one frame in eight, as
it has tens of thousands of triangles). Every 60 frames perf.txt gets a line like

    ca12  frame 120: gpucost eyes 2.0 per eye: screens (k verts)  map 3.07 (35.3) shown 22% ...  fog 2.89 (0.4) n43  fogscr 1.00 (0.0)  total 7.41

with each pass's area in screens (400 x 240 pixels) and thousands of vertices, per eye. In 3D, before
and after the changes below (per eye; the 2D fog for comparison):

| Pass | Forest ca12 | Lake shore ca13 | Town cb03 | Town cb41 (no fog) | What the GPU does |
| --- | --- | --- | --- | --- | --- |
| Map vertices (k) | 120..151 → 35..40 | 90..205 → 28..38 | 79..170 → 23..36 | 96..207 → 27..31 | a 55-instruction vertex shader each |
| Map triangles reaching the screen | 6..7 % → 22 % | 5..8 % → 23..27 % | 3..8 % → 17..23 % | 2..3 % → 12 % | |
| Map area (screens) | 3.1..3.5 | 2.9 | 1.2..1.4 | 2.1..2.4 | textured, flashlight lit per pixel, opaque |
| Fog sprites (screens) | 4.5..7.0 → 2.9..3.2 (2D 9.6..11.7) | 4.1 → 2.4..2.7 (2D 9.7) | 4.2..4.4 → 2.8..3.0 | | blended, textured, faded by the procedural texture |
| Fog screen layer (screens) | 1 | 1 | 1 | | blended, textured |
| Characters (k vertices) | 21 | 21 | 23 | 21 | an 88-instruction skinning shader |
| Water | | 0.23 screens, 10k vertices | | | untextured |

Ranked by cost per eye: the fog sprites (blended over the screen 4 to 7 times), the map's vertices
(80,000 to 200,000, of which 2 to 8 % reach the screen: the resident tiles surround James), the map's
area, the fog's screen layer, the characters, the water. These scenes have no flashlight shadows;
indoors the stencil volumes add their area and one blended full-screen quad per eye. The console's
numbers fit about 1 ms per eye for each screen of fog sprites (one eye took 15 to 27 ms on the roads
with 10 to 12 screens of them, both eyes 30 ms with half that each), but the split between the
passes is an estimate until the console's A/B runs below.

What changed, 3D only unless said:

- **The map is culled per eye** (`main.c` `box_test`, `room.h` `ROOM_CHUNK_TRIS`): each batch is cut
  into chunks of 32 triangles in its own order, the reader thread boxes them (`room_prepare`), and a
  run of chunks wholly outside one side of the eye's view, or beyond its far plane, is not sent. A
  run of visible chunks stays one draw, over gaps of one hidden chunk. Nothing the GPU would have
  drawn is left out, so this applies in 2D too: 2D frames stay byte-identical to the build before
  (er01, ca12, ca13, cb03 and cb41 compared with `dumpframe`, and `mapcull 0` against `mapcull 1` with
  the behind camera), and 2D gets the same saving.
- **The fog's sprites are held to 3 screens per eye** (`fog.c` `stereo_thin`, `quality.h`
  `fog_fill_3d`, `fogfill N` in tenths to try others): the far sprites are all drawn and the large near
  ones each with a chance that keeps the total, at their alpha over that chance (at most twice: at
  three times the rarely drawn near ones showed as dark blots at the lake shore). This replaces
  drawing every other particle at twice the alpha.
- **Auto picks 60 or 30 in 3D as in 2D** (`perf_pace` by the GPU's own time), instead of holding 30:
  the lit rooms take about 6.5 ms of GPU for one eye, so both eyes fit in a 60 fps frame.

#### CPU with both eyes

In Azahar with `perflog 1`, slider at 100, before and after (ms, medians of the per-second lines and
the parts' averages):

| Scene | citro3d CPU | r_map | r_fog | Slowest frame |
| --- | --- | --- | --- | --- |
| Forest ca12 | 6.0 → 5.7 | 0.50 → 2.18 | 3.96 → 2.49 | 34.0 → 17.3 |
| Town cb03 | 8.6 → 9.7 | 1.79 → 1.80 | 5.26 → 6.38 | 34.1 → 18.6 |
| er01 | 1.9 → 2.6 | 0.51 → 1.28 | | 34.0 → 17.2 |

Azahar charges the GPU nothing, so after the change Auto runs all three at 60 there; a console's GPU
time puts the fogged roads back at 30. The culling costs CPU time per eye (a box test per group of 8
chunks, and per chunk where a group crosses the view's edge), and in the town the fog sizes and
sorts all its sprites where it used to drop half before drawing. The work stays under 12 ms in
Azahar, which counts about an Old 3DS's speed; a New 3DS does it in about a third of that.

#### Console test plan

Only a console times the GPU. With `perflog 1`, the slider up, the same walk as before (the forest
from ca13 to ca27, the town from cb64 to cb13, the lake shore at ca13):

1. The per-second lines' `gpu` (and `max`): it was 30 to 31 ms with peaks of 34 in the forest and the
   town and over 50 at the lake shore. The aim is under 24 ms with peaks under 30, so the forest and
   the town hold 30 fps with no slow frame lines.
2. Where it falls short, the same place with `mapcull 0`, then with `fogfill 0` (all sprites, as in
   2D) and `fogfill 20`, one at a time: the difference in `gpu` is what each change is worth, and
   which knob has room.
3. Indoor rooms (er01, the apartments, the hospital) in 3D: `render` near 16 ms and `gpu` under about
   15 means they run at 60; the slow frame lines show where it falls back to 30.
4. The slider down: `gpu` should be lower than before in the town and the forest (the culling) and the
   picture the same.
5. By eye, toggling the slider on the fogged roads: the fog's density in 3D against 2D, near sprites
   popping in or out, or a near sprite showing as a blot.

### Quality knobs

`quality.h` holds what trades looks for frame time. `profile.c` sets it from the hardware profile
and the quality level (`profile_apply_quality`, at start and on every change in the System page),
debug.txt can override one knob for a test, and the subsystems read only `quality()`:

| Knob | High (New 3DS) | Low (Old 3DS) | Read by |
| --- | --- | --- | --- |
| `fog_sprites` | 144 | 72 | `fog.c` `set_partnum` |
| `stencil_shadows` | on | off | `shadow.c` `shadow_begin` |
| `drop_shadows` | on | off | `main.c` `shadow_frame` |
| `tile_distance` | 7000 | 5000 | `main.c` `stream_update_ex` |
| `frame_cap` | 0 (60, an even 30 when it does not fit) | 30 | `perf_pace` |
| `fog_fill_3d` | 3 screens per eye | 3 | `fog.c` `stereo_thin` |
| `supersample` | 1 (the top screen at twice its height while the GPU has the time) | 0 | `supersample.c` |

The supersampling (`VISUALS.md`) is the one knob that adds GPU work, and it watches the GPU's time
itself: it turns on only after 2 seconds of frames under 7 ms and off after one doubled frame over
14 ms, so it gives way before `perf_pace` would drop to 30.

The fog sprites and the stencil shadows are the two likeliest to matter on a console's GPU: 144
alpha-blended sprites can cover most of the top screen several times over, and each shadow volume
is drawn twice into the stencil buffer. They are the first things to turn down if a console shows
frames over budget that Azahar does not.

### Per-part timing

With `perflog 1` every 60-frame line is followed by a `parts` line, for example in the town:

    cb11       parts  game 0.16/0.21  stream 0.01/0.22  cam 0.88/0.91  r_wait 12.24/12.54  r_map 0.57/0.61  r_chr 0.95/1.07  r_shadow 0.02/0.06  r_fog 1.26/1.40  r_end 0.38/0.42

Each is the average and the worst of those frames in ms: the update's parts (`game`, `stream`,
`join`, `cam`, ...), then the render's: `r_wait` (`C3D_FrameBegin` waiting for the GPU and the
vertical blank, which is the slack), `r_map` (the room), `r_chr` (James, enemies, stage models),
`r_shadow` (shadows and blood), `r_fog`, `r_ui`, `r_touch` and `r_end` (`C3D_FrameEnd`). In the
town the camera (0.9 ms) and the fog (1.3 ms) are the biggest parts after citro3d's own command
building.

## Slow SD cards

Azahar reads the host's disk, far faster than a console reads its SD card (roughly 5 to 10 MB/s,
less with many small files). `sdthrottle KB/s [open ms]` in `debug.txt` (`source/sdsim.c`) makes the
readers sleep for the time a console would take on top of what Azahar charges: the tile and file
reader (`stream.c`), the sound effect reader, the music thread, and the main loop's own fallback
reads (`chr.c`, `room.c`). The open time stands for the directory lookup of each file and defaults
to 8 ms. Every warp now logs how long the black screen waited for the door's read ahead and whether
it all came in (`warp` lines), and when the arrival's tiles were in (`arrival` lines); a tile or a
stage model logs how far from James it was when it was set up.

At 5000 KB/s and 8 ms a file:

- **Town walk** (cb03 to cb28, New 3DS model): no slow frame. A tile's read on the reader thread
  went from 400 to 700 ms, which it has time for: tiles still joined 4400 to 4700 units away, in the
  fog, and the stage models were set up 14000 to 16800 away, just as they come into range.
- **Door warps** (`memtest 1` from er05 through ten rooms of different areas): every door's read
  ahead was in before the fade ended, after 0.9 to 2.2 s of black (the cap was 3 s then), and the town's
  neighbouring tiles were in 0.5 s after that. The models placed near the arrival loaded with the
  room. The load frames themselves (behind the black) are 50 to 125 ms.

At 2000 KB/s and 15 ms a file, a slow card's worst case, the black screen's caps (3 s for the read
ahead, 3 s more for the tiles) were too short: ps189's read ahead was not all in after 3 s, so its
load read the rest itself behind the black, and at cb03 the fade began 3 s after the room loaded
with the last tile still on its way; it joined half a second later, 2100 units from James, while
the screen faded in. Both caps are now 6 s (`READ_AHEAD_MAX`, `ARRIVAL_MAX`). With them every door
of the same sample was in before the fade ended: the read ahead after 0.9 to 3.3 s, the cb03 tiles
3.3 s after the load. The load frames behind the black stay at 55 to 140 ms. `memtest` also waits
now for each door to finish before the next, so a slow card does not stack its warps.

Nothing in the town walk or the doors showed pop-in at either speed once the caps were raised, so
nothing had to be read ahead earlier or loaded smaller. A real card's speed with small files is the
number to check on hardware, since it sets the open time.

## Not done

- Nothing here has been run on a console yet. Worth checking there: that the audio line says
  core 2, the perf.txt numbers for the town walk, and how long the tile reads take on a real SD
  card (the reader thread has 7000 units of walking, a few seconds, to finish one).
- The warp autotest calls `apply_warp` without a door's read ahead, so its load frames read
  everything from the SD card (its measuring window starts after them).
- Azahar does not time the GPU. The town at 60 fps and the fog-heavy rooms are where a console
  would show it first; `quality.h` has the knobs to turn down.
