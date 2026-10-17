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
| `streamsync 1` | reads streamed tiles on the main loop as before, for comparison |

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

The first frame after a room load is always slow (125..450 ms) because the load itself runs there;
that sits behind the fade and is not counted above.

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

## Threads

| Thread | Core | Priority | Work |
| --- | --- | --- | --- |
| main | 0 | libctru default | game, drawing, GPU uploads |
| tile reader (`stream.c`) | 0 | main + 1 | reads `.s3m`, `.s3l`, uncached `.t3x` and the footstep `.sfm` of a requested tile into malloc'ed buffers |
| music refill (`audio.c`) | 2, else 0 | main + 1 | ADX/AIX decoding into the ndsp wave buffers of the music and voice channels |
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
fills a `StreamJob` with the file contents (`stream.c`, up to 6 jobs). The jobs are collected on a
later pass with `stream_take`, and `neighbour_add_job` does the GPU side on the main thread:
`room_load_mem` (linear memory, `C3D_TexInit` through `Tex3DS_TextureImport` from the blob) and
`col_load_mem`. The texture ids the cache already holds are passed to the job so they are not read
twice. A tile that left the wanted set while it was being read is thrown away. An area change
(`load_current`) still loads synchronously behind the fade, and `stream_flush` drops queued jobs and
the results of other areas.

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
- The first frame of a room load (125..450 ms) is still one long frame; it is hidden by the fade.
- Sound effects (`sfx.c`) load their samples on the main loop when first played; they did not show
  up in these measurements, but a busy scene with many new sounds could.
