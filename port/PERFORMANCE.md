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
| `perflog 1` | appends a line per second to `perf.txt`, plus a line for every frame over 25 ms (with the time of each named part of that frame) and one per streamed tile |
| `memlog 1` | appends the free linear, VRAM and heap memory to `mem.txt` at boot and after every area change |
| `memtest N` | warps through ten rooms of different areas N times, one every 150 frames, and logs memory after each (implies `memlog`) |
| `streamsync 1` | reads streamed tiles, a door's destination and its tiles on the main loop as before, for comparison |
| `sfxsync 1` | reads a sound effect's sample on the main loop at its first play, as before |
| `shadowdebug N` | character shadows: 1 shows the volumes in red, 2 darkens every changed stencil count, 6 turns them off |

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
  (frames keep coming, music keeps playing, at most 3 seconds) before `load_current` takes them with
  `stream_take_named` and does only the GPU side.
- **Tiles after the warp.** In the town the touching tiles were loaded inside the warp frame (1.1 s
  for cb03). After a door they now come in through the reader thread while the fade stays black
  until they are resident (`s_arrival`, at most 3 seconds).

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
| music refill (`audio.c`) | 2, else 0 | main + 1 | ADX/AIX decoding into the ndsp wave buffers of the music and voice channels, and starting and stopping their streams |
| sound effect reader (`sfx.c`) | 0 | main + 1 | reads a sound effect's sample from `se.bnk` into the buffer the main thread allocated for it |
| movie decoder and audio (`movie.c`) | 2, else 0 | main + 2 / + 1 | only while a movie plays |

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

## Memory

The CIA is built for the 124 MB mode (`package.sh`: `APP_SYSTEM_MODE_EXT=124MB`). libctru's default
split of that gives the linear heap 32 MB and malloc about 91 MB, the wrong way round for this port:
every texture, vertex and index buffer and sound buffer is in linear memory, and malloc held at most
3.7 MB in any measurement. `perf.c` now sets `__ctru_heap_size` to 32 MB and libctru gives the
linear heap the rest, about 90.9 MB in 124 MB mode. The 32 MB leave room for the tile reader's
buffers (an `.s3m` of up to 1.6 MB plus textures per queued tile). A 3dsx started from a smaller
memory mode simply gets a smaller linear heap.

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

## Not done

- Nothing here has been run on a console yet. Worth checking there: that the audio line says
  core 2, the perf.txt numbers for the town walk, and how long the tile reads take on a real SD
  card (the reader thread has 7000 units of walking, a few seconds, to finish one).
- Debug spawns still load everything in one frame, and enemy models that come in with a tile after
  the warp (the town) are still read on the main loop.
