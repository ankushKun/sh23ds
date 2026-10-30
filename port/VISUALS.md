# Visuals

What the top screen shows next to the PC game, what was changed to bring it closer, and what comes
next, each with its cost on the GPU. The rule from `PERFORMANCE.md` holds here too: nothing may cost
frames. A change either costs nothing measurable or runs only while the frame has the time for it.

## Where it stood (review of 2026-10-29)

**Textures at a quarter of the PC's texels.** The packer capped room, cutscene model and prop textures
at 256 px and gameplay characters at 512, against the PC's own sizes:

| What | PC | Pack before | Pack now |
| --- | --- | --- | --- |
| Map atlases (about 2,600 in 16 areas) | 512 x 512, 161 at 1024 (most of them in the town) | 256 | 512 |
| Gameplay characters (James, Maria, enemies) | 1024 x 512 body atlas, 512 and 256 for the rest | 512 x 512 | as the PC |
| Cutscene characters (Laura, Mary, Angela, the scene Maria, Mary's double) | the same | 256 x 256 | as the PC |
| Items and stage props (cars, furniture, doors, puzzle objects) | 512 x 512, 256 x 512, a few 1024 x 512 | 256 | as the PC |

A room uses at most 18 map textures, so the PC's sizes cost at most 3 MB of a room's memory (a median
of 1 MB), against 86 MB of free linear memory on the New 3DS. A face in a cutscene close-up had a
quarter of the texels across it, the restroom wall's graffiti could not be read and the urinals and
tiles showed the 4 x 4 blocks of the compressed texture magnified.

**The map sampled at one level.** The PC makes its map textures with one level (0x459320), and the
pack's mipmaps of the atlases drew lines along the ground's quad edges on a console, so the port
sampled the first level only (`room.c`).

**No anti-aliasing.** 400 x 240 with every edge in steps, and the ground shimmering where its
texture is seen at a slant. The PC runs at the monitor's resolution; the EE adds MSAA, SMAA and
anisotropic filtering on top (`AntiAliasing`, `EnableSMAA`, `AnisotropicFiltering`).

**Renderer features the port does not have** (STATUS.md, "Renderer features"): specular, the indoor
water, blur and bloom, rain and smoke, event lights outside cutscenes, map-object and cutscene shadows.

**The GPU budget.** The one console run with 3D on put the fogged forest and town at 30 to 31 ms of
GPU for both eyes; lit rooms take about 6.5 ms per eye. So indoors there is room, and outdoors in the
fog there is none.

Things that were checked and are fine as they are: stills and the save screen are packed above the
screen's resolution (512 x 256 for a 320 x 240 picture), movies are MPEG-1 at quantiser 3 at the
screen's size, and character textures already had their full mip chain.

## What changed

### Textures at the PC's sizes

`convert.py` packs map textures at 512 (`--max-tex`), `chr.py`, `demos.py` and `stagemodels.py`
models at their own sizes up to 1024. Every file keeps its whole mip chain.

The console decides what it keeps (`profile.c` `profile_tex_max_map` / `_chr`, `t3xcap.c`): the New
3DS profile with 56 MB of linear heap or more keeps the PC's sizes, the Old 3DS profile (or a 3dsx
started with less memory) keeps 256 for the map and 512 for models, the sizes the pack had before.
`t3x_read_capped` skips the levels above the cap on the card, so the Old 3DS reads and keeps exactly
what it did. `texfull 1` or `2` in `options.txt` forces either.

The map is sampled down to the level whose larger side is 256 and no further (`room.c`
`texcache_get`): near walls and ground get the PC's texels, far ones the 256 level that was the only
one before, so nothing blends wider across an atlas cell than it did. The PC samples its 512 level
everywhere; the 256 floor is closer to the PC than before and keeps the far ground as it was checked
on a console. `mapmips 1` in `debug.txt` still samples the whole chain.

The texture cache for earlier rooms went from 12 to 24 MB on the New 3DS (`profile_texcache_budget`),
so it keeps about as many rooms as before.

**Doors stay as quick as they were.** Read whole, the larger files made the black screen after a door
0.5 to 1.3 s longer on a 5 MB/s card. So what a door's black screen waits for comes from the sizes
packs had before: while a door is under way (`stream_set_draft`), the room and the tiles round it read
their map textures from the 256 level and the models it brings (enemies, placed props) theirs from
the 512 level. Once the room shows, `room_tex_refine` and `chr_refine` ask the reader thread for the
whole files and swap each one in as it arrives, one a frame, at the same address the materials point
to; what was swapped out is freed after the next `C3D_FrameBegin` (`room_tex_reap`, `chr_reap`). Tiles
that stream in while James walks are read whole, out of sight, as before. perf.txt gets a `refine`
line with how long the room took to become whole, and the debug page counts the drafts left.

### The Picture

The video of the PC game is drawn at 3840 x 2160 and kept at 640 x 360, so scaled to the top screen
it shows what a perfectly smoothed 400 x 240 picture looks like. Measured as edge energy (the mean
step between neighbouring pixels) in four rooms, the PC at 400 x 240 is 2.5 to 5.5 and the port 6.8
to 16.2. Supersampling 2 x 2 brings the port to 5.9 to 11.7; most of what is left is texture grain,
not polygon edges, and a [1, 2, 1] filter each way after it brings it to 4.0 to 4.8.

`picture.c` (a 3DS-only system, `picture.h` explains it) is the Display tab's Picture: Soft (the
default) or Sharp. Soft draws the scene into a texture in VRAM, as the item screen draws its model,
and draws it into the screen through four bilinear taps half a pixel off diagonally, one quad each
averaged in turn, which is that filter exactly. The 2D (messages, the grain, menus) goes on top
afterwards, as the PC draws its text and noise after its own filters. It costs four full-screen quads
at the screen's size on both models, in 2D and per eye in 3D, and 1 MB of VRAM (3 MB with the 2x
scene or a 3D frame's right eye). The first version copied the scene out with the transfer engine
between two halves of the frame (`C3D_FrameSplit` and `GX_TextureCopy`); Azahar ran it, but a New 3DS
froze on the first frame of play, and so did the second, drawn from a texture in one command list.
Both drew the taps with a program of their own that took a single vertex attribute, which no other
draw in the port does, so the pass now draws with the movie's quad program and its two attributes
(`movie.c`), its taps' texture coordinates worked out on the CPU. `picture 3` in `debug.txt` tells the
two halves apart on a console if it freezes again.

### Supersampling

`supersample.c` (a 3DS-only system, `supersample.h` explains it) draws the scene larger while the GPU
has the time: 2x (twice the screen's height, 1.5 MB of VRAM) or 2x2 (twice both ways, 3 MB), one
target at a time. On Sharp the display transfer halves it into the screen; on Soft the soft pass
reads its 2x texture at the screen's pixels, where its taps average a 2 x 4 block, so the soft pass
costs the same at every level. Soft stops at 2x: a 2x2 texture with its depth (4 MB) would not fit
beside a 3D frame's right eye.

The level follows the GPU, read from `C3D_GetDrawingTime` as `perf_pace` reads it, in fractions of
the frame's budget: 16.7 ms at 60 fps, 33.3 ms when the Frame Rate is 30. After 2 seconds of frames
whose time doubled would fit 0.84 of the budget it steps up; a frame over 0.84 of it steps down at
once, before the pacing would give up a frame, then 5 seconds before it may step up again, doubling up
to a minute where it keeps stepping down. When the pacing has dropped an Auto 60 fps game to 30 it
goes back to the screen's size. 3D frames and movies are drawn at the screen's size, and Quality Low
(the Old 3DS profile's) has no supersampling. `supersample 0`, `1` (the default), `2` (always 2x) or
`3` (always 2x2) in `debug.txt` overrides it; the debug page and frame dumps' `picture` line show the
level and the Picture (`2x2 soft`).

## Checked in Azahar

On 2026-10-29, the pack's textures converted again at the new sizes (`convert.py` for every area, the
models' textures at their own sizes), against the build before this change on the same scenes:

- **Pictures.** The restroom wall's graffiti reads, the tiles, the urinals and the apartment walls
  lose their 4 x 4 blocks, Angela's sweater and the cemetery ground show their weave, the town's
  chain-link fence is finer and its outline smoother. With the old pack the new build draws the same
  picture apart from the supersampling (mean difference 0.6 to 3 of 255 outside cutscenes, whose
  timing differs).
- **Old 3DS model.** The new pack draws as the old one did (mean difference 0.3 of 255, the mip
  level's filter), so the trim reads the right levels.
- **Doors** (`memtest 1` with `sdthrottle 5000 8`, ten rooms in ten areas): the black screen after
  each door is within 0.1 s of the old pack's, 0.65 to 2.4 s, and the load frames behind it are as
  long as they were. The rooms become whole 0.7 to 2.4 s after they show. Read whole, without the
  drafts, the same doors took 0.5 to 1.3 s longer. Linear memory stays above 75 MB free and malloc
  under 9 MB of 32.
- **Supersampling.** On after the first 2 seconds of play in every scene (Azahar reports 0.25 ms of
  GPU); menus drawn on their own frames, movies and 3D stay at the screen's size. Frame dumps and
  screenshots show the halved image.
- **Smoke run** (`tools/lr3ds/smoke.sh` on the new pack): 12 of 12 on the New 3DS and Old 3DS models.

On 2026-10-30, the soft pass drawn from a texture target instead of a copy, with the console's own
saves and options (CONTINUE into the town), then again on the movie's program: `picture 1` against
`picture 3` (the same GPU work, the scene drawn as on Sharp) differs by 0.002 of 255 on the top
screen. On the texture target alone: `picture 1` against Sharp differs by 0.96 of 255 on
average with no shifted row or column (the extra GPU work moves the frames' timing; two Sharp runs
match exactly), 2x on Soft, 3D (`citra_render_3d`, `citra_factor_3d 100`) and the smoke run (12 of
12 on both models) log nothing a console stops on.

On 2026-10-30, the Picture and the supersampling's 2x2 step:

- **The copy.** `picture 1` in `debug.txt` draws the copy back without the filter; against Sharp it
  differs by 0.04 of 255 on average (James' idle motion, the frames' timing differs), so the copy, its
  orientation and its edges are right.
- **The filter.** Azahar's software renderer samples every texture without filtering, so its frames
  show the soft pass's taps as single texels, not the filter the console draws, and they show the
  game's magnified textures blockier than a console does. Only a console shows the result.
- **The ladder.** On Soft and on Sharp the level reaches 2x after 2 seconds and 2x2 after 4 (Azahar
  reports almost no GPU time). Frame dumps work at every level on Soft; on Sharp they still need
  `supersample 0`. In 3D both eyes get the soft pass at the screen's size. The Old 3DS model's Auto
  profile runs `1x soft`.
- **Text.** The explainer's lines fit in every language and button layout (`tests/ui/capture.sh
  textcheck`).
- **Smoke run:** 12 of 12 on both models.

The packing gets slower: the map's textures take about four times the encoding they did. The browser
packer encodes at rg_etc1's medium quality, about 20 times faster than the high quality tex3ds and
the native packer use, so the change is smaller there. The pack on the SD card grows from about 1.3
to about 1.8 GB (the map's textures from 143 to 559 MB).

## On a console

Azahar does not time the GPU and hides the ground's mip seams, so these need a New 3DS:

1. `perflog 1`, 2D, the debug page on: in er01, ap06, the hospital and the prison the line should
   reach `2x soft` or `2x2 soft` with the per-second `gpu` under 14 ms and no slow frame lines. In the
   fogged town and forest it should stay at `1x soft` and the frame rate be what it was. The same
   with Frame Rate on 30 fps: 2x2 in more places, `gpu` under 28 ms.
2. Soft against Sharp in the same rooms: the picture smoother without looking smeared, the text and
   menus as crisp, and `gpu` about 1 ms higher on Soft. On an Old 3DS, Soft at 30 fps with no new slow
   frames.
3. The ground and walls up close, against the previous build: no lines along the ground's quad edges
   (the first-level-only rule was made for those), no new shimmer on far floors.
4. `supersample 0`, `2` and `3` in the same rooms: `gpu` should rise by less than twice per step; if a
   room flips between levels, the hold-off should keep it at the lower one.
5. Room loads with the larger textures (`memlog 1`, `perflog 1`): the `warp` lines' black screen
   against the previous build, the `refine` lines (how long until the room is whole, and whether the
   sharpening shows as a pop), and that tiles still join out of sight in the town.

## The Enhanced Edition

How the EE improves the look, and what the port takes from it (the formats are in
`tools/sh2conv/EE_PACKS.md`):

- **Textures and models.** None of the EE's packages has larger world or character textures. Its
  Essential Files fix maps, models and cutscene scripts at the original sizes, and the packer reads
  them in place of the originals, so with this change they come in at the PC's sizes too.
- **Image Enhancement Pack** (stills, maps, examined items, the save screen at 4x). The packer reads
  them and squeezes them to the sizes the port draws, which are already at or above the screen's
  resolution. The maps are the one case with room to gain: 1024 x 1024 instead of 1024 x 512 would
  keep twice the vertical detail when zoomed in, for 1 MB more while the map is open (`map.c`).
- **FMV Enhancement Pack.** The packer re-encodes the 1080p movies to the screen's size, so the
  cleaner source shows; nothing more to take.
- **Renderer options.** `AntiAliasing`, `EnableSMAA` and `AnisotropicFiltering` are Direct3D
  settings; the supersampling above is the 3DS's counterpart. `EnableSoftShadows` is ported
  (EE_FIXES.md). `SpecularFix`, `RestoreSpecialFX` (blur, bloom), `WaterEnhancedRender`,
  `HotelWaterFix`, `RainParticles`, `FlashlightReflection` and `SmokeFogFix` each need the PC
  feature they fix, which the port does not have yet; they belong with that feature in the list
  below.
- **JamesTextureFix** swaps James' jacket texture (1240) for the EE's own (9980) in one cutscene to
  hide the shoulders clipping. It needs the EE's texture and is left out.

## Next, in order

By what a player sees against what it costs the GPU. Each starts from the PC function named.

| # | What | PC | On the 3DS | GPU cost |
| --- | --- | --- | --- | --- |
| 1 | Character specular: the gloss masks in the textures' alpha (the Lying Figure's sheath, skin, James' jacket) | the character pixel shader and its presets (EE's 0x4ff1c6, 0x502b96; alpha test 0x4ffa30), `SpecularFix` | the fragment lighting the characters already have, specular into the combiner times the texture's alpha (ETC1A4 keeps 4 bits, enough for a mask) | small: characters cover little of the screen |
| 2 | Indoor water (hotel, labyrinth, apartments, Strange Area, the last boss room) | Effect2 classes 27 to 41, 0x4d0d70 / 0x4d1a40 | as `water.c` draws the lakes | a blended surface, under a screen |
| 3 | Character transparent parts sorted far to near | 0x503e10 | sort the hair's triangles per frame on the CPU | none on the GPU |
| 4 | Event lights outside cutscenes (the hotel's swinging lamp) | 0x47b040, 0x576fa0 | a point light in `envlight.c` and `chrlight.c` | none |
| 5 | The EE's maps at 1024 x 1024 | | `maps.py`, `map.c` | none, 1 MB while the map is open |
| 6 | The PC's 2D filters: motion blur (boss deaths, scenes), glow (the radio tunnel, room 312), soft focus (the closet), depth of field (the opening toilets), with the Enhanced Edition's `RestoreSpecialFX` corrections | the dispatcher 0x479840 (motion blur 0x478de0, the blur 0x477a60), the setter 0x478740, `enSetBlur` 0x4a7d80 | the Picture's frame copy (`picture.c`) blended over or blurred, while a filter runs | one to five quads while it runs |
| 7 | Rain and smoke particles | Effect2 rain, smoke classes | the blood module's sprite path | blended sprites, budgeted like the fog |
| 8 | The 800-pixel mode on the New 3DS: the top screen's full horizontal resolution in 2D | | `gfxSetWide`, as an alternative to the supersampling where it fits | twice the pixels, and no 3D at the same time |
