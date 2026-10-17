# Movies (FMV)

The PC game plays 21 Bink movies from `data/movie/*.bik`. The 3DS has no Bink decoder, so
`tools/sh2conv/movies.py` re-encodes the player's own copies on the computer and `source/movie.c`
plays them back with pl_mpeg, the Y2R colour converter and ndsp.

## Files

| Where | What |
| --- | --- |
| `tools/sh2conv/movies.py` | ffmpeg wrapper: Bink to MPEG-1 program stream (`<pack>/movie/<name>.mpg`) plus `movies.txt` |
| `source/movie.c`, `movie.h` | player: decoder threads, Y2R, ndsp, frame pacing, the blocking `movie_play()` |
| `source/movie.v.pica` | the quad shader (position and texcoord only) |
| `source/third_party/pl_mpeg.h` | PL_MPEG by Dominic Szablewski (MIT, `pl_mpeg.LICENSE`), with three marked speed patches |

## Format

`movies.py` writes ordinary MPEG-1 program streams (`ffplay movie/toilet.mpg` plays them), which keeps
the 3DS side down to one small decoder for both video and audio.

- Video: mpeg1video at 29.97 fps (MPEG-1 only allows standard rates; Bink's 29.94..29.97 map onto it
  and ffmpeg keeps the timing), constant quantiser 3 with RD macroblock decisions and trellis, GOP 60,
  two B-frames. 11 of the 18 story movies are 640x384, which is 5:3 like the top screen, so they
  become exactly 400x240. The 640x480 ones (credits, endings, murder, water, flash, trailers) are full 4:3 frames with
  credit text right up to the edges, so they are encoded at 320x240 and shown between black bars.
  `--fit crop` instead scales them to 400x300 and keeps the middle 240 lines, which loses the top and
  bottom credit lines (compare `.fmvtest/evidence/pillarbox_vs_crop.png`).
- Audio: MP2 at 32 kHz stereo, 128 kbit/s. The DSP mixes at 32728 Hz, so a higher rate would only
  cost space. MP2 is half the size of IMA-ADPCM at that rate and pl_mpeg already decodes it.
- Tracks: `Movie_BinkPlay` (0x43d770) selects Bink sound tracks for two files, so those keep every
  track as its own MP2 stream in the same order. `end_dog.bik` also has two tracks, but they are
  identical, so only the first is kept.

Sizes with the defaults: the 18 story movies (26.5 minutes) take 155.5 MB, and with the three trailers
(`--trailers`) the folder is 238.6 MB. The story movies converted in 23 seconds here (parallel
ffmpeg jobs on an M-series Mac). Quantiser 2 (`--q 2`) would land near 200 MB for the story movies but costs decode time (below).

### Names

The game asks for PS2 names (`d:\data\movie\toilet.sfd`) and the PC turns them into `.bik`. `movie.c`
accepts either form and maps the special ones the way `Movie_BinkPlay` does:

| Name the game asks for | File | Audio stream |
| --- | --- | --- |
| `open_bgm` | `open.mpg` | 0 |
| `open_voc` | `open.mpg` | 1 (the narrated opening; 0 is the same mix without the voice) |
| `end_mria` / `end_rbrt` / `end_scde` / `ending` | `end.mpg` | 0 / 1 / 2 / 3 (staff roll, one song per ending) |
| anything else, e.g. `toilet` | `<name>.mpg` | 0 |

Stage programs request movies through the demo table in `prog.c` (`"toilet.sfd + first_toilet"`,
`hakaba`, `gero`, `knife`, `hei`, `deai`); `host_demo()` in `main.c` plays the `.sfd` part.

## Playback

`movie_play(name)` is blocking: it runs its own frame loop until the movie ends or A / START is pressed,
pauses every other ndsp channel meanwhile (music and effects carry on afterwards) and leaves a black
top screen behind. `movie_start` / `movie_update` / `movie_draw` / `movie_stop` are there for a
non-blocking caller. Call `movie_update` outside `C3D_FrameBegin`/`C3D_FrameEnd`, because stopping
waits for the GPU.

### Threads

Two pl_mpeg instances read the same file. The video one (audio disabled) decodes on a thread that keeps
up to eight frames ahead in linear memory, each frame copied out of pl_mpeg and cleaned from the data
cache on that core so Y2R's DMA sees it. The audio one (video disabled) decodes MP2 on a second thread
straight into six ndsp wave buffers of 2304 samples (72 ms each). Both threads are created on core 2
with priorities below the main thread (audio above video). If the kernel refuses core 2 they fall back
to core 0, where the lower priority keeps the display loop on time. The main thread only converts and
draws, so an SD read can never make it miss a vblank. That was a real problem while the audio
instance still ran on the main thread: one 128 KB read took two vblanks and frames were dropped.
pl_mpeg now reads in 16 KB chunks for the same reason.

### Y2R

Each frame is converted by the Y2R service (`INPUT_YUV420_INDIV_8`, `OUTPUT_RGB_16_565`,
`BLOCK_8_BY_8`, `COEFFICIENT_ITU_R_BT_601_SCALING` because MPEG-1 luma is 16..235) straight into a
512x256 RGB565 texture in the PICA's tiled layout. The receive transfer unit is one row of 8x8 tiles
(`width * 8 * 2` bytes) and the transfer gap skips the texture columns right of the picture. Two
textures alternate so Y2R never writes the one the GPU may still be drawing. One full-screen quad
draws the texture 1:1 with nearest filtering; outside the picture it samples cleared texels or the
black border colour, which gives the pillarbox bars for free.

A test pattern in Azahar showed the plain texcoord mapping put the picture one line low, so the
quad's t coordinates are nudged by one texel. Lines 1..239 now match ffmpeg's decode exactly (best
alignment 0,0 in both directions) and line 0 is black.

`debug.txt` `movielog 2` converts on the CPU instead (same tiled layout, same BT.601 maths). It
exists for emulators without Y2R; Azahar emulates Y2R correctly, and the CPU path costs about 20 ms
per frame on the main thread, so real hardware should keep Y2R.

### Sync

The audio is the clock. Every frame the main thread asks ndsp which wave buffer is playing
(`ndspChnGetWaveBufSeq`) and the sample position inside it, adds the stream position where that buffer
started and shows the decoded frame nearest to that time one vblank ahead (a frame drawn now is
visible from the next vblank). If two frames are due at once the older one is dropped. Without audio
(no `dspfirm.cdc`) or after the audio ends first, a tick clock carries on from the last audio time.
Playback starts once two frames and two audio buffers are ready, so the start is not ragged.

One pl_mpeg quirk had to be handled: the audio decoder reads its first frames from stream 0 when it is
created, so selecting stream 1 afterwards played 108 ms of `open_bgm` before `open_voc`. `movie.c`
rewinds the audio instance after picking the stream.

## pl_mpeg changes

Three patches, each marked `sh23ds:` in the header, all checked bit-exact against upstream pl_mpeg by
decoding toilet, open, murder, end_wish and credits natively and comparing every plane (identical MD5):

1. `plm_buffer_read_vlc` reads the next bit inline instead of calling `plm_buffer_read(self, 1)`.
2. DCT coefficients go through an 8-bit lookup table (`plm_buffer_read_dct_coeff`), with the tree walk
   only for codes longer than 8 bits.
3. Motion compensation works on four pixels per 32-bit word with SWAR averages that round exactly
   like the per-pixel cases (the ARM11 has no NEON).

The movie TU is compiled at `-O3`. Together these took open_voc from 12.5 to 9.3 ms per frame in
Azahar (the final 10.1 below also counts time the audio thread takes from the same core).

## Measured performance

Azahar 2126.2 libretro core (software renderer, New 3DS model, 100% clock), headless through lr3ds.
Times are `svcGetSystemTick` deltas per decoded frame (268 MHz ticks, printed on the bottom screen and
logged with `movielog 1` to `sdmc:/3ds/sh23ds/movie_log.txt`):

| Movie | Size | Frames | Mean ms | Median | p90 | p99 | Max | Over 33.4 ms | Dropped |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| toilet | 400x240 | 602 | 7.26 | 6.46 | 10.78 | 17.77 | 23.62 | 0 | 0 |
| open_voc | 400x240 | 6983 | 10.06 | 9.12 | 16.47 | 24.84 | 58.53 (first frame) | 1 | 0 |
| ending | 320x240 | 6899 | 7.16 | 6.51 | 11.37 | 16.08 | 22.47 | 0 | 0 |
| murder | 320x240 | 2399 | 16.32 | 14.41 | 30.50 | 38.76 | 49.81 | 141 | 0 |

Y2R conversion takes 0.52 ms per frame on the main thread, the plane copy 0.5 ms on the decoder
thread, and MP2 decoding 5 to 8 ms per 72 ms buffer on the audio thread (the audio instance also has to
read past all video packets). murder is the worst case: film grain at 1.7 Mbit/s. Its slow stretches
are absorbed by the eight-frame queue, which never ran empty in any run.

How this maps to hardware: Azahar counts roughly one tick per guest instruction at 268 MHz and does not
model the New 3DS 804 MHz clock (`ConfigureNew3DSCPU` is unimplemented), so 10 ms here means about 2.7
million instructions. At 804 MHz that is 3.3 ms at one instruction per cycle; the ARM11 manages less
than that on decoder code (cache misses on 144 KB frames, branchy VLC), so expect somewhere between a
third of these numbers and about the same. Either way core 2 has several times the 33 ms a frame needs
for every movie except possibly murder's grainiest seconds, which the queue covers. This still needs
confirming on a console: the bottom screen shows the averages, the per-frame maximum and the queue
depth while a movie plays.

Memory: two 256 KB textures, up to eight decoded frames (144 KB each at 400x240, fewer if linear memory
is short, at least three), 55 KB of PCM, all in linear memory and freed after the movie. pl_mpeg's
buffers and reference frames take about 1 MB of heap for the two instances. 26 MB of linear memory
was still free while toilet played after New Game.

## Verification

`.fmvtest/` (not in the repo) holds the test setup: `run.sh` and `run2.sh` run the copies in Azahar,
`lr3ds_av` is lr3ds that also prints the audio sample count at each screenshot, and `avsync.py`
compares a run with ffmpeg's decode of the same `.mpg`:

- Audio: the core's WAV output cross-correlated with ffmpeg's MP2 decode (stretched to the DSP's
  32728.498 Hz): 0.9989 over the whole 233 s of open_voc, 0.9991 over the 230 s of ending, 0.9985 for
  toilet, with 2 s pieces all at lag 0 (no gaps, no drift). The ending check against stream 0 instead
  of 3 gives 0.014, so the track selection is right too.
- Picture: each screenshot is matched against every decoded frame and compared with the movie time
  the audio was at when the screenshot was taken. Clear matches: toilet (New Game) -9 ms mean
  (-31..+26), open_voc -16 ms (-32..-2), so within one movie frame (33.4 ms). The ending credits scroll
  so slowly that neighbouring frames differ less than the colour conversion error, and the image match
  there drifts by three frames. In a repeat run the frame number and clock the player prints on the
  bottom screen every frame agreed with the audio (frame 565 shown while the audio was at 18.87 s,
  where 565 / 29.97 = 18.85 s), so that is a limit of the comparison, not of playback.
- `evidence/` has contact sheets of toilet and open_voc, New Game on the current port (title, toilet
  with live timings, then James at the mirror), a pillarboxed credits frame and the WAV captures.

## Debug

`debug.txt` in the pack folder: `movie <name>` plays a movie before the title screen,
`movielog 1` logs per-frame times, `movielog 2` uses the CPU converter, `movielog 3` both.

## Open issues

- Not run on a console yet. Things to watch there: that the CIA really gets core 2 (the bottom screen
  shows `core 2` or `core -2` for the fallback), decode times against the estimate above, and Y2R
  timing (Azahar finishes conversions instantly).
- Azahar does not implement `svcGetProcessorID`, so the `id` shown next to the core is meaningless there.
- Line 0 of the picture is not shown (see Y2R). Hardware may sample the texture edge differently; if
  the picture sits one line high there, drop the nudge in `movie_draw`.
- Only the stage program demos are hooked (toilet, hakaba, gero, knife, hei, deai). The endings
  (`end_*`, `ending`, `water`, `end_wish`, `end_dog`, `credits`), `open_bgm` on the title, `open_voc`
  and the boss movies (`flash`, `murder`, `rouya`, `saikai`, `korosu_*`) still need their callers.
- The PC's skip rules are not ported. It skips on its own button tests (0x43ded0) and treats `gero`
  differently; here A or START always skip.
- The audio instance keeps a video decoder with three unused reference frames (about 430 KB of heap),
  because pl_mpeg creates decoders for every stream in the file.
- Grainy movies (murder, end_wish, water) cost the most to decode. If a console struggles, re-encode
  only those with `--q 4` or a light denoise before giving up on quality elsewhere.
