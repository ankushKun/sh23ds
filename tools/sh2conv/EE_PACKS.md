# Silent Hill 2: Enhanced Edition packs and the packer

The Enhanced Edition (EE) is a set of packages for the PC game. Most of them replace the game's own
files with fixed or better ones. This note says how the PC game finds those files, what each package
replaces and in which format, and what the packer does with each. The code is `ee.py` (one lookup
used by every converter) plus small changes in the converters that read the affected files.

Sources: the EE source in `~/Developer/refsrc/sh2ee` (commit 9cf4621, October 2026), its package list
`https://files.townofsilenthill.com/SH2EE/_sh2ee.csv` (the URL its auto-updater reads from
`Resources/webcsv.url`), and the central directory of each package's zip, read with HTTP range
requests together with the first bytes of a few entries to learn their formats. No package content
was downloaded or kept beyond those headers.

## How the PC game finds the files

`Common/FileSystemHooks.cpp` hooks the game's file opens (`fopen`, `CreateFileA/W`, `BinkOpen`,
`FindFirstFileA/FindNextFileA`). For every path starting with `data\` it tries, in order:

1. `lang\<rest>`, only when `EnableLangPath` is set in `d3d8.ini` (no package installs a `lang`
   folder; it is for translations),
2. `sh2e\<rest>`, or the folder named by `CustomModFolder`,
3. the original `data\<rest>`.

The first file that exists wins, one file at a time; there is no manifest. Windows matches names
without regard to case. A few special cases:

- `movie\end.bik` and `movie\ending.bik` are interchangeable: the mod folder is tried for `end.bik`
  and then `ending.bik`, then `data` the same way.
- `UsePS2LowResTextures` ("Disable sh2e\pic path" in the launcher) sends `pic\`, `menu\mc\` and
  `etc\effect\` to `sh2e\ps2\` instead (the PS2's low resolution pictures, from Essential Files).
- `LowHealthIndicatorStyle = 2` swaps `pic\etc\RedCrossIcon.png` for `LowHealthFade.png`, and a
  custom `start01*.tex` is used only when the matching `start00.tex` is in the same folder.

Larger files need the EE's address patches, which is why the packs only work with the EE module:

- `EnableTexAddrHack` (`Patches/TexPatch.cpp`) grows the image buffer to the largest file in
  `data\pic\*` and `data\menu\mc` (as redirected), and `Patches/FullscreenImages.cpp` scales the
  quads by the replacement's size over the original's (the `DefaultTextureList` table holds the
  original sizes). So a replacement image shows the same picture over the same area, just with more
  texels; maps and the save screen are drawn at their own aspect.
- `EnableSFXAddrHack` (`Patches/SfxPatch.cpp`) reads `sddata.bin` (as redirected), finds the 417
  `RIFF` headers in order and rewrites the exe's table of byte offsets with the new ones.
- The BGM sizes the game reads with `FindFirstFile` are replaced by those of the mod folder's files
  (`VISIT_BGM_FILES` in `FileSystemHooks.h` lists all 48 BGM files and `voice.afs`).

## The packages

From `_sh2ee.csv` (the list the SH2EE Setup Tool installs from), as of October 2026. Sizes are the
unpacked sizes.

| id | Package | Version | Installs |
| --- | --- | --- | --- |
| `sh2emodule` | SH2 Enhancements Module | 3.0.2218.0 | `d3d8.dll`, `d3d8.ini`, `SH2EEconfig.exe`, `sh2e\etc\resource\r_menu_*.res` (code and menu resources) |
| `ee_exe` | Enhanced Executable | 1.0.4GB | `sh2pc.exe`: a patched North American 1.0 exe, large address aware |
| `ee_essentials` | Enhanced Edition Essential Files | 1.3.1 | 720 entries in `sh2e\` (fixes; see below) |
| `img_pack` | Image Enhancement Pack | 1.2.0 | 211 files, 983 MB, `sh2e\pic`, `sh2e\menu\mc` |
| `fmv_pack` | FMV Enhancement Pack | 1.6.0 | 20 movies, `sh2e\movie` |
| `credits` | Credits | 1.6.1 | `sh2e\movie\credits.bik` |
| `audio_pack` | Audio Enhancement Pack | 2.1.0 | 52 files, 2.5 GB, `sh2e\sound` |
| `xidi` | Xidi | 1.0 | controller DLLs |
| `sh2emodule\wine_stub` | Wine stub | 1.0.1 | for Linux and macOS players |

### Essential Files

Fixes to the game's own data, all in the original formats:

- `sh2e\bg\<area>\*.map` for most rooms of 15 areas (`ap bw ca cb cc cd er hp ob ps qp qt rr ru th`)
  and `sh2e\bg2\ma\*.map`, with some `.cam`, `.cld` and `.kg2` files and `bg\ob\obGB.fcl` (the
  observation deck's fog widened for 16:9). These fix texture, geometry and collision errors.
- `sh2e\chr\item\*.mdl` (21 items and doors), `chr\mar\lll_mar.mdl`, `chr\scu\scu.mdl`,
  `chr2\mar\lxx_mar.mdl`.
- `sh2e\demo\{anahori,edi_korosu,sankaku1}\*.dds` (cutscene scripts) and `demo\saisho\hhh_jms.cls`.
- `sh2e\etc\message\*.mes` in every language (text fixes; `option_msg_*` is changed for the EE's own
  options menu).
- `sh2e\font\font000.png` (a high resolution font for the EE's font code), `sh2e\ps2\...` (see
  `UsePS2LowResTextures`).

### Image Enhancement Pack

The same `.tex` / `.tbn2` container as the originals (magic `0x19990901`, one sprite, its header at
0x30 with the width and height at 0x38, the format at 0x3c and the length at 0x40, pixels at 0x50,
then 16 bytes of padding), four times the size:

| Files | Original | Pack |
| --- | --- | --- |
| stills, `pic\{add,apt,dls,etc,hsp,htl,out,ufo}\*.tex` | 512x512 DXT | 2048x2048 DXT3 |
| sprite sheets on the stills (`clock_hari`, `p_desk_coin_coin`, `egg`, ...) | 64..512 | 4x, DXT3 |
| examined items, `pic\item\x_*.tex` | 256x256 | 1024x1024 |
| maps, `pic\map\*.tex` | 1024x1024 DXT | 5464x4096 DXT1 (4:3 at square pixels); `pic\add\mapboldwin.tex` twice that |
| map marker sheets, `*mark*.tex`, `hospitalmapmk.tex` | 256x512, 512x512 | 4x, DXT3 |
| `pic\etc\start00.tex`, `menu\mc\savebg.tbn2`, `savebg2.tbn2` | 512x512 | 2732x2048 DXT3 (4:3) |
| `pic\etc\start01*.tex` (title menu text, per language) | 512x512 | 2048x2048, 32-bit BGRA (format 0x20) |
| `pic\etc\itemmenu2.tex` | 1024x1024 | 4096x4096 |

The pack also replaces the originals that are in other formats (the 8-bit paletted `htl\p_hanging*`
and the PS2-format statue box, keys and snake coin) with ordinary DXT files, and adds PNGs the EE
draws itself (`MousePointer.png`, `RedCrossIcon.png`, `LowHealthFade.png`,
`controller_buttons_icons.png`). It ships `sh2e\effect\footmark.tbn2`, a path the game never opens
(the original is `data\etc\effect\footmark.tbn2`), so that file has no effect in either game.

### FMV Enhancement Pack and Credits

Every movie except the credits (which are their own package), the three trailers included, as Bink 1
(`BIKi`, which the game's own `binkw32.dll` plays and ffmpeg decodes):

| Movie | Original | Pack |
| --- | --- | --- |
| `open`, `end`, `credits` | 640x384 or 640x480 | 1920x1080 |
| `deai`, `toilet`, `water` and others | 640x384 or 640x480 | 1920x1230 |
| frame rate | 29.97 | 29.97, some 59.94 (`open`, `deai`, `toilet`) |

The audio tracks are laid out as in the originals (`open` 2, `end` 4, the others 1). The credits are
a new edit, 145 s long where the original is 93 s.

### Audio Enhancement Pack

| File | Original | Pack |
| --- | --- | --- |
| `sound\sddata.bin` | 417 mono PCM16 WAVs, 8..22.05 kHz, 17 MB | the same 417 in the same order, mono PCM16 at 48 kHz, 89 MB |
| `sound\adx\<area>\bgm_*.adx` / `.aix` (48 files) | 22.05 kHz ADX / AIX | 48 kHz ADX / AIX, loop points in the ADX headers |
| `sound\adx\voice\voice.afs` | 159 ADX files, 416 MB | 159 files, 1.5 GB: PCM16 WAV (44.1 kHz stereo for the lines), only the looping ones still ADX |
| `sound\extra\*.wav` | | sounds of EE-only features (flashlight click, save and load) |

`sddata.bin` keeps the original's layout: each WAV is followed by two bytes, the second 1 for a
looping sound, then a `LIST` chunk (`AudioScripts/SFX/mergebin.cpp`). Files 13..44 of `voice.afs`
are sound effects, not lines; the pack has longer versions of several of them.

## What the packer does

`ee.py` does what the EE's hook does: `ee.file(path)` takes a path inside `<game>/data` and returns
`<game>/sh2e/<same path>` when that exists (matched without regard to case, `end.bik` also as
`ending.bik`), else the path. The format readers (`sh2map.load`, `sh2cld.load`, `sh2mdl.load_mdl`
and `load_anm`, `sh2mes.load`, `ui.load_tex`) call it, so every converter that reads maps,
collision, models, motions, messages or pictures gets the replacements with no change of its own.
A replacement that does not parse is reported and the original is read instead. Converters that
copy or read files directly call it too: room camera files, fog, shadows, cutscene scripts and
motions, sounds and movies. `tools/make_pack.sh` prints what it found (`ee.py report`) and writes
every replacement it used to `ee_used.txt` in the output folder.

Only files the original game has are looked up; the EE-only files (PNGs, `font000.png`,
`sound\extra`, `sh2e\ps2`, the menu resources) are not used. `lang\` is not searched: the port reads
the English messages.

| Category | Converter | With the pack |
| --- | --- | --- |
| Essential Files: maps, `.cam`, `.cld` | `convert.py`, `collision.py`, `events.py`, `sound.py` (footstep surfaces) | read in place of the originals; textures capped at 256 like the originals' |
| Essential Files: models, motions, cutscene scripts | `chr.py`, `demos.py` | read in place of the originals (Maria's Born From a Wish model through make_pack's folder of links too) |
| Essential Files: messages | `events_pack.py`, `maps.py`, `ui.py` | read in place of the originals |
| Essential Files: `obGB.fcl` | `fog.py` | copied in place of the original |
| stills | `pics.py` | decoded from 2048x2048 and squeezed to 512x256 RGB565 as now |
| puzzle sprite sheets | `pics.py` | brought back to the original's size (the overlay tables address them in the original's texels) |
| maps | `maps.py` | decoded from 5464x4096 and squeezed to 1024x512 RGB565 as now |
| marker sheets | `maps.py` | brought back to the original's size (map.dat's rectangles are in its texels) |
| save screen | `ui.py` | decoded from 2732x2048 and squeezed to 512x256 as now |
| movies | `movies.py` | re-encoded from the 1080p source to the same 400x240 (or 320x240, 400x224, 368x240 for other aspects) MPEG-1 at 29.97 fps; `movies.txt` records the source, so changing it re-encodes |
| BGM | make_pack's music step (`ee.py copytree`) | the 48 kHz ADX / AIX files copied under the original names |
| sound effects | `sound.py` | the 48 kHz WAVs, resampled to 32 kHz (the DSP mixes at 32728 Hz) |
| voice.afs effects (13..44) | `sound.py` | decoded from WAV, mixed to mono, 32 kHz at most |
| cutscene voices | `demos.py` | the WAVs encoded to ADX with ffmpeg (the port streams ADX); the looping ADX ones copied |

`SH2EE=0` builds from `data` only. `SH2EE_SKIP=images,fmv,audio,essentials` (any of them) leaves
those parts out. `SH2EE_DIR` names another mod folder, as `CustomModFolder` does.

### Notes and limits

- **The exe check is unchanged.** The packer still needs the Director's Cut `sh2pc.exe` (MD5
  `f3f5360298b6d0f6f2e09cf24a0925d1`). The Enhanced Executable package replaces `sh2pc.exe` with a
  patched North American 1.0 exe (MD5 `f9ccf14b5e5a31d9fe50e5b5efe50ed7`), which `gameversion.py`
  recognises and names. Third-party install scripts keep the old exe as `sh2pc.bak`, and the packer
  reads a Director's Cut exe it finds there by itself; otherwise the player puts it back (the Setup
  Tool's own behaviour was not checked).
- **Build into an empty folder** after adding or removing packs. Converters skip textures and models
  they already wrote (for resuming), so an old output folder keeps old textures. Movies are the
  exception: their source is tracked.
- **Voices** gain less than the other audio: the port plays ADX, so the pack's lossless lines go
  through a 4-bit ADX encode again (ffmpeg's encoder, 37 dB SNR on speech). The content improvements
  (fixed lines, better masters) carry over; the codec ones do not. Playing PCM voices would need
  `audio.c` to stream WAV, at 3.5 times the space on the card.
- **ffmpeg's ADX and the port's decoder** differ in two conventions. CRI's files (and `audio.c`)
  use a frame's stored scale plus one: the original `voice.afs` has about 150,000 frames whose
  stored scale is 0 but whose nibbles are not, which only makes sense that way. ffmpeg's encoder and
  decoder use the stored scale as is, and its encoder predicts with one rounding (ADX version 4)
  where version 3 rounds each term. `sound.wav_to_adx` stores each scale one lower and marks the
  file version 4, which then decodes in `audio.c` as ffmpeg meant (36 to 43 dB SNR against the WAV
  on two cutscene lines); without that it was 15 to 23 dB. The pack's own ADX files were made with
  CRI's encoder (`AudioScripts`) and need nothing.
- **BGM at 48 kHz** costs the music thread about 2.2 times the decoding per second of music. The AIX
  tracks were measured at 25..30 ms per 4096-sample buffer (`port/PERFORMANCE.md`), which at 48 kHz
  is about 35% of the New 3DS's spare core where the thread runs. The pack's files cannot be
  resampled here (ffmpeg reads AIX but cannot write it), so `SH2EE_SKIP=audio` is the way back if
  that is too much on a console; it has not been measured on hardware.
- **Sound effects** grow from about 17 MB to about 60 MB in `se.bnk`; the port's sample cache stays
  at 4 MB, so effects are read from the card more often.
- **Movies** at 1920x1230 (a 1.56 aspect, taller than the 5:3 screen) come out as 368x240 between
  narrow bars with the default `--fit pillarbox`; the originals of those were 640x384 (5:3, full
  width). The 59.94 fps sources are reduced to 29.97.
- **Credits:** the Credits package's edit is longer than the original. The port plays the file to
  its end, so nothing depends on its length.

## Texture sizes and the memory budget

The port caps room and character textures at 256x256 (`convert.py`, `chr.py`, `demos.py`; ETC1 or
ETC1A4 with mipmaps) and keeps up to 12 MB of earlier rooms' textures (`TEXCACHE_BUDGET`,
`port/source/room.c`). Everything lives in linear memory, of which about 83..88 MB is free after an
area change in the 124 MB mode (`port/PERFORMANCE.md`). None of the EE packages has larger room or
character textures: Essential Files' maps and models carry textures at the original sizes, so the
256 cap does not lose anything from the packs. Raising it would be a separate change (four times
the memory and card reads per texture).

The classes the Image Enhancement Pack does improve:

- **Stills** (`data/pic`) are drawn as 320x240 in the middle of the top screen from a 512x256
  texture. That already has more texels than screen pixels, so a larger texture would not show;
  the gain is the cleaner downscale from the 2048x2048 source. Not worth making larger.
- **Save screen** (`ui/savebg*`): the same, 512x256 for a 320x240 picture.
- **Maps** are 1024x512 RGB565 (1 MB) because the PC stretches a 1024x1024 map over its 4:3 screen;
  1024 is the GPU's largest texture side. With the pack's 4096-line maps a 1024x1024 texture (2 MB,
  one map resident at a time) would keep twice the vertical detail when zoomed in. Memory allows
  it easily; it would need `port/source/map.c` checked for the texture height, so it is left as is.
- **Sprite and marker sheets** must keep the original's size, since the tables address them in
  texels.

## Verified and not verified

Verified with a synthetic install made from the player's own files (`.assetstest/game`: `data` and
`sh2pc.exe` linked to the real ones, and a `sh2e` folder written in the packs' layout and formats:
4x stills, a sprite sheet, a marker sheet and the save screens as DXT `.tex` / `.tbn2` with an
"EE 4x" stamp, three maps at 5464x4096 DXT1, a 32-bit `start01.tex`, a 48 kHz `sddata.bin`, a
`voice.afs` with files 0..44 as WAV, a 48 kHz BGM, `flash.bik` swapped for another movie, the
observation deck's maps with tinted textures, copies of Maria's model and a message file, and
EE-only files that must be ignored):

- Without a `sh2e` folder the changed converters write the same bytes as before (ui, maps, pics,
  fog, moths, convert and collision for `ob`, chr, movies, sound, demos for two scenes, events_pack);
  the only difference is the new source column in `movies.txt`.
- With it, the output differs only in files made from replacements: the `ob` room textures and
  `ob01.s3m`, the three maps and the marker sheet, the replaced stills, the clock sheet (back at
  256x256), the save screens, `flash.mpg`, `se.bnk` (32 kHz effects) and the two cutscene voices.
  Sheets, markers, stills and maps come out at the same sizes as before.
- `make_pack.sh` prints the report, takes Maria's model through its folder of links, copies the
  48 kHz BGM under the original name without the voice folder, and lists 9 replacements in
  `ee_used.txt` for the steps run.
- In Azahar: the memo screen shows the stamped still, the whole-map view the stamped map with
  James' marker in the same place as with the original, the observation deck the tinted
  textures, `movie flash` the swapped movie (400x240, 29 s), and the "exit" cutscene's voice,
  re-encoded from WAV, has the same loudness envelope as the original (correlation 0.994).
- The lookup matches names without regard to case (`SH2E/Movie/Ending.BIK` for `end.bik`),
  follows make_pack's links, and honours `SH2EE=0`, `SH2EE_SKIP` and `SH2EE_DIR`.

Not verified, as the real packs are not on this machine: the real files' contents beyond the header
bytes (that the pack's maps frame the map the same way as the originals, so the markers land right;
that Essential Files' `.map`, `.mdl` and `.dds` files parse, though a file that does not falls back
to the original; the real 1080p and 59.94 fps Bink files through ffmpeg), a 48 kHz AIX (no synthetic
one could be written), and console performance with the 48 kHz BGM.
