# Sound in sh23ds

How the PC Director's Cut plays sound effects and picks its music, what the converter extracts, and
how the 3DS side (`sfx.c`, `bgm.c`, the layered `audio.c`) reproduces it. Everything comes from static
analysis of `sh2pc.exe` (MD5 f3f5360298b6d0f6f2e09cf24a0925d1), the PS2 beta debug info
(`src/sound/sh_sound.c`, `sh_sd_call.c`, `snd_select.c`) and the Enhanced Edition source, which
patches several of these functions and confirms the addresses.

Confidence: **high** means the decompiled code was read and the result was checked against the
data or in Azahar, **medium** means read in the decompiler but not exercised, **low** means inferred.

## Files on the SD card

`tools/sh2conv/sound.py` writes everything below into `pack/sound/` (copy to `sdmc:/3ds/sh23ds/sound/`).
It takes about 3 seconds and needs only `sh2pc.exe` and `data/`.

| File | What |
| --- | --- |
| `se/se.bnk` | every effect as mono PCM16 at its own rate (449 samples, 26 MB), the game's SE id table and the 22 door sound sets |
| `se_index.txt` | one line per game SE id: sample, rate, length, loop, use (ports' own labels plus the EE's SFX map) |
| `floor/<map>.sfm` | every floor polygon of the room's `.cld` with its collision material (footstep surface) |
| `steps.txt` | heel-strike frames of James' walk and run clips, found from the animation |
| `bgm.bin` | the BGM file list, the 48 BGM pages, the per-stage sound areas and the room-id tables |

Formats are documented at the top of `sound.py`. In short: `se.bnk` is a 32-byte header, a sample
table `{offset, frames, rate, flags}`, a sorted `{game id, sample}` table, the door table (6 x u16 per
set) and the PCM, each sample 32-byte aligned.

## Where the PC keeps its effects (high)

- `data/sound/sddata.bin` is 417 RIFF WAVE files back to back, all mono PCM16 at 8000 to 22050 Hz
  (most 13 to 16 kHz), each followed by 2 bytes. The byte at `riff + 0x2D + data_size` is the loop
  flag (FUN_00517470 reads it); it is set on WAVs 350 to 366 (engines, generators, radio static).
- The exe never scans the file. `0x8A45A8 + 4 * id` holds the byte offset of internal id 5000 to 5698
  (the EE's `sfxBlock` / `DefaultSFXAddrList`, at 0x8A93C4 for this build). Every one of the 699
  offsets lands on a RIFF start.
- `0x8A4AC0` is 666 pairs `{game SE id, internal id}`. Game ids are 10000 to 19998 (2D calls) and
  40000 to 40281 (3D variants, see `change_list` below).
- `0x8A4A40` is 32 pairs `{u16 game id, u16 60000 + n}`: these effects are ADX files streamed from
  `voice.afs` (AFS partition 0, opened in `Snd_Init_DSound_ADX` 0x514B60), files 13 to 44. They are
  stereo 22 kHz; the converter decodes them (checked against ffmpeg, correlation 0.99992) and mixes
  them to mono. 10051 (the save point sound) and 18820 (door set 20 close) are among them.
- `dsstdfx.bin` (24 KB, half zeros) and the 256-byte `apart.dat`, `hospital.dat` and friends are
  entries of the game's file table (pointers at 0x7BB598 onward, next to `sddata.bin`), but no code
  in the PC sound module reads them that I could find, and the effects play without them. They look
  like leftovers of the PS2 driver, which loaded per-building banks (`sdb_list[53]`,
  `SeSoundEffectLoad`) and an IOP effect setup. **low**

## The PC sound driver

PC addresses with the PS2 beta names where the code matches.

| PC | PS2 name | What | Confidence |
| --- | --- | --- | --- |
| 0x5151D0 | `shSdCall` | entry for 2D calls: id, volume, pan; returns 0x10 on success | high |
| 0x514D60 | (driver dispatch) | command switch: 1000 init, 0x3F3 stop AIX, 0x401/0x402 option volumes, 5000..5083 load a BGM file, 10000..19998 play SE, 50000..59998 start BGM, 60000.. ADX SE | high |
| 0x514500 | | 2D SE: game id -> internal id (0x8A4AC0) -> 0x518270; ADX SEs via 0x8A4A40 | high |
| 0x518270 / 0x5181C0 / 0x518000 | | stop the same internal id if it plays, then create a DirectSound 3D buffer from the WAV at `offset + base` | high |
| 0x517960 | | (angle, distance) -> 3D buffer position: x = -sin(a) d/25.5, z = cos(a) d/25.5; d > 250 multiplies by 100 | high |
| 0x516AE0 | `SeCallPosDistanceF` | positional call used by doors, footsteps and most of the game | high |
| 0x515230 | (`SeCallPosDistance`) | distance level: `255 - 255 * vol * fs * fc`, fs and fc fade linearly from 1 at 1500 units to 0 at 11500 for source-to-James and camera-to-James | high |
| 0x515AB0 | (`SeCallPosDirection`) | pan angle from the view matrix: sign(x) * 80 * (1 - z) degrees | high |
| 0x515B60 | `SeStop` | | high |
| 0x5167E0 / 0x516A10 | `Se2dManager` / `Se3dManager` | per-frame volume and position of managed (looping) sounds | medium |
| 0x515800 / 0x515850, 0x8A7778 | `SeChange2Dto3D` / `SeChange3Dto2D`, `change_list` | 2D id <-> 3D id (40000+) | medium |
| 0x515C20 | `SeBgmChange` | picks a BGM page from stage, flags, room | high |
| 0x516080 | `SeBgmManager` | page switch, AIX track fades | high |
| 0x5187E0 | `sndGetSoundAryByCharaPos` | which tracks the sound areas around James switch on | high |
| 0x515690 | (`BgmPageSet`) | situation page: 0 calm, 2 low health (< 0.2), 3 enemy within 6000 units, or the stage's `bgm_control` | medium |
| 0x54A9A0 | (James footsteps) | footstep triggers and floor material -> SE | high |
| 0x549150 | (Maria footsteps) | same for Maria | medium |

DirectSound does the attenuation: every effect is a 3D buffer (min distance 1, max 1000, rolloff
left at 1), so the volume a caller passes ends up as `gain = min(1, 25.5 / (255 * (1 - v)))`.
That makes 1.0 full, 0.8 about -6 dB and 0.5 about -14 dB. `sfx.c` reproduces this curve
(`level_gain`), and the Azahar captures match it: walking steps (0.5) come out at 0.20, running steps
(0.8) at 0.50.

Repeating an id that is still playing stops the old instance first (0x518270), so footsteps cut
each other's tails. `sfx.c` does the same.

## Doors (high)

`door_se` at 0x88CB70 is 22 sets of 6 shorts `{open, close, unlock, jam, lock, pad}`, picked by
event `rslt1` bits 27..22.

- Door warp (0x48E220): `open` with `SeCallPosDistanceF(id, 1.0, door position - 500 Y)` when the
  fade starts; after the warp, at fade-in, `SeStop(open)` and `close` at the destination.
- 0x48C1C0: type 7 (unlock) plays slot 2, type 9 (jammed) slot 3, type 8 (locked) slot 4, all at 1.0.
- The commonest set is 1 (19000 open, 19001 close, 19003 unlock, 19002 jam, 19004 lock).

`sfx_door_close` waits until the open sound has played 0.6 s (or all of it when shorter) before it
cuts it, so a fast room load does not chop the door. In Azahar the ap06 to ap38 load takes about
1.2 s, so the open sound plays out in full anyway.

## Other effects the port uses

| Id | Use | Source |
| --- | --- | --- |
| 11041 | item pickup | `shSdCall(0x2B21, 1.0)` in 0x48C3A0 (high) |
| 10051 | save menu opened at a save point | `shSdCall(0x2743, 1.0)` in 0x48C680 (high) |
| 10000 / 10002 / 10003 | menu move / confirm / cancel | EE MenuSounds.cpp and the callers (high) |
| 15002 | game start: New Game, Born From a Wish, Continue (played while the title fades out) | `shSdCall(0x3a9a, 1.0)` at 0x498201 and 0x4978c4 (high) |
| 18001 / 18000 | flashlight on / off | the PC has **no** flashlight sound; the EE adds its own WAVs. These are short button clicks from the bank standing in (port choice) |

## Footsteps

0x54A9A0 runs every frame for James. For the lower-body state it lists up to four triggers
`{frame of the current motion, foot, volume}`: walking states fire at frames 5 and 17 (3 and 14 with
a two-handed grip) at 0.5, running states at 0.8; turning in place (states 6, 7) at frames 6 and 13.
A trigger fires once when the motion frame passes it. The SE comes from the collision material of
the floor polygon under each foot (`DAT_01FBAA74 + 8`):

| Material | SE | | Material | SE |
| --- | --- | --- | --- | --- |
| 0, 12 | 11001 | | 6 | 11003 |
| 1, 4 | 11004 | | 7 | 11005 |
| 2 | 11002 (grass) | | 8 | 11009 |
| 3 | 11007 | | 9 | 11006 |
| 5, 11 | 11000 (metal) | | 10 | 11008 (glass) |

Room overrides that need PC room ids are not ported yet: room 0x78 uses 18803 for material 0, rooms
0x2A and 0x46 use 19908 for 5, room 0x3E uses 11054 for 8, room 0xB8 and boxes in rooms 0x92, 0x99,
0xA0, 0xA3 switch 6 and 8 to 18807 / 19907.

The port plays `jms_basic1` ranges that hold several gait cycles, so the PC frame numbers do not
apply. `sound.py` finds the heel strikes in the animation instead (an ankle bone dropping into the
lowest 12 units it reaches in the clip; the clips do not loop seamlessly, so of two strikes closer
than 8 frames the later one stays). That gives walk 57, 69, 82 and run 94, 110, 122, 135, 148, 156.
`sfx_footsteps` plays a step for every strike the animation passed this frame, in either direction,
on the material of the floor under James (`floor/<map>.sfm`).
A step that comes less than 6 animation frames after the previous one is dropped: switching from
walk to run restarts the run clip 2 frames before its first strike, which played two steps at once
(the Enhanced Edition's DoubleFootstepFix for the PC's same bug at 0x54af80).

## Background music

### Files and pages (high)

`0x8A70B0` lists the 47 BGM files `{load id 5030..5083, type (1 ADX, 0 AIX), path}`. `0x8A75D0` is
48 pages `{load id, start id 50000 + n}`; page 0 is silence. `SeBgmChange` (0x515C20) turns the
current stage (`DAT_01DBEB80`, the same ids as the event stages) into a page, with a few game flag
and room tests. `bgm.c` has it as `pick_page`, flags converted from the PC word/bit tests
(`gGameFlag` at 0x1F7D5E0, flag n = word n >> 5, bit n & 31). Examples: stage 2 (toilet) plays
`forest/bgm_114_a.adx`, stage 6 (apartments 1F) `apart/bgm_003.adx` until flag 91, stage 7
`apart/bgm_118.aix`.

A new page first fades every sounding track out over 3 s (0x5155E0), then the new file starts with
all tracks silent and the areas fade them in. `SeBgmManager` also sets a per-file master volume
(0x8A73B4, 0.25 to 0.9 by start id, 0.4 by default); `bgm.c` has the table.

### AIX layers from sound areas (high)

Each stage has an array of `_SOUND_DATA` (pointer table at 0x8BC8C0, 64-byte records):
an area box (x0..x2, z2..z0 tested with a 50-unit margin around James), a Y window, `flags` and
`chanstat[7]`. Record 0 is the default when no area matches; the list ends at negative flags.
For each of the 7 tracks (AIX layers) a record says on/off, a fade-in type and a fade-out type. When
`flags` has the bit of the current situation page, the track uses that page's own 5 bits
(`chanstat >> (page * 5)`), otherwise bits 0..4. Fade types: 1 = 3 s, 2 = 1.5 s, 3 = 6 s,
4 = instant; when several areas overlap the type with the highest priority (0x8A7750: 4, 2, 1, 3)
wins. The layer volume is `option volume * master * duck * fade` on a linear amplitude scale (the
0.1 dB table at 0x8A73C0 is `200 * log10(i / 127)`). Duck is 0.5 during events and 0.7 in some
menus; the port leaves it at 1.

`audio.c` now demuxes every AIX layer (the files have 2 to 7 stereo layers) and mixes them with
per-layer gains, ramped across each 4096-sample buffer. Plain `audio_play_music` keeps the old
behaviour (layer 0 only), `audio_play_layers` starts everything silent for `bgm.c`.

### Not ported

- Situation pages 2 and 3 (low health, enemy near) and the stage `bgm_control` programs that return
  pages 6 and up (fixed mixes at 0x8A9EB8); `bgm_set_situation` is there for when enemies exist.
- Stage 15 waits for its stage program to start the music (`DAT_01F837D0`); the port keeps it silent.
- Ducking during events and menus, and the BGM / SE option volumes (0x401 / 0x402).
- The `.dat` files per building (see above).

## Runtime on the 3DS

- ndsp channel 0 is music (`audio.c`), channels 8 to 23 are 16 effect voices (`sfx.c`). Stealing takes
  the oldest one-shot; loops are never stolen.
- Effects load on first use into linear memory with a 4 MB LRU budget. Footsteps, menu sounds, the
  pickup and the flashlight clicks (about 200 KB) are loaded at boot and pinned. A door's close
  sound is read while the screen fades out. Nothing is allocated per frame; `sfx_floor_load` and
  the AIX chunk buffers allocate on room or stream changes only.
- Decoding all 7 layers of a 24 kHz stereo AIX costs well under 1% of the CPU; the debug HUD showed no
  change in Azahar.

## Verified in Azahar

Captures with `lr3ds --wav` (32728 Hz stereo), each effect cross-correlated with its source PCM from
the bank (`.sfxtest/scripts/xcorr.py`):

| Test | Result |
| --- | --- |
| ap06 door (stage 6 event 5) | open 19000 at 0.995, close 19001 at 0.9996, both gain 1.00; 0.993 / 0.979 with music playing |
| ap38 jammed door (event 9) | 19002 at 0.997 with the "lock is broken" message |
| footsteps ap06 (material 8) | 17 steps of 11009 at 1.000 (0.3 s window), gain 0.20 walking, 0.50 running |
| footsteps ap38 (material 0) | 10 steps of 11001 at 1.000, gain 0.20 walking, 0.47 running |
| flashlight off, inventory open / close | 18000 at 0.989 (gain 0.5), 10002 at 1.000, 10003 at 0.9998 |
| BGM stage 6 | page 4 `bgm_003.adx` against ffmpeg: 0.9993, gain 0.396 (master 0.4), 6 s fade-in |
| BGM stage 7 (ap34) | page 5 `bgm_118.aix`, layers 2 + 4 from area 20: 0.9926 against a Python mix of those layers, each at 0.35 to 0.40 |
