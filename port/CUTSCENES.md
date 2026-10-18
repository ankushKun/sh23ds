# In-engine cutscenes (demos)

The story scenes that are not Bink movies are played by the engine itself on the PC: a script moves
the camera and places the characters, every character plays motions from `.anm` files, a voice
stream from `voice.afs` runs alongside, and the stage's message file supplies the subtitles. The port
does the same with `source/demo.c`, from data written by `tools/sh2conv/demos.py`. Everything here
comes from static analysis of the Director's Cut `sh2pc.exe` (MD5 f3f5360298b6d0f6f2e09cf24a0925d1)
with the names of the PS2 beta debug info (`Event/demoview.c`, `Chacter/anime.c`).

## How a scene is asked for

A stage program calls `DramaDemoMain` (0x4a1c80) every frame with a `DramaDemo_PlayInfo` record
from the stage's data until it returns 1. The port's programs call `prog_demo_request(id)` with a
`ProgDemo` id instead (see `PROGRAMS.md`); `main.c`'s `host_demo` plays the movie part first when
the demo has one, then starts the in-engine part with `demo_start()` and keeps answering "not yet"
until the scene is over. The program then goes on exactly as before: it sets its flags and puts
James where the PC's `CharaSetPos` puts him. A `ProgDemo` the pack has no scene for still counts
as watched at once.

A request can be more than one scene. The PC programs chain scenes, movies and voice-only
sequences and pick between scenes by game flags or by stage; `demo.c` keeps that order in a table
of steps per `ProgDemo` (`s_seq`, see "Sequences" below), so the stage programs still ask for one id.

While a scene runs, `main.c` takes the camera from `demo_camera()`, draws the scene's characters
with `demo_draw()` (and hides James when the scene has its own), prints the subtitle on the bottom
screen and lets START skip the scene instead of opening the pause menu. When the scene ends the
picture is black and the game fades back in.

`DramaDemo_PlayInfo` (40 bytes, layout from the PS2 type, checked against every record found):

| offset | field | meaning |
|---|---|---|
| 0x00 | demo_no | scene number; demos 7, 0x5a, 0x5d and 100 scale the field of view |
| 0x04 | adr_dds_top | the script, normally the buffer at 0x1dbec40 the program loaded it into |
| 0x08 | adr_anim | s16 motion ids: one per character for every "start motions" key |
| 0x0c | adr_msg_time | {u16 start, u16 end} frames per subtitle (no count; see below) |
| 0x10 | msg_start | stage message number of the first subtitle |
| 0x12 | voice_sd_no, 0x14 adr_voice | unused on the PC |
| 0x18 | stream_no | SE id of the voice: 60000 + the `voice.afs` file |
| 0x1c | stream_start | scene frame the voice starts at |
| 0x20 | add_pos_x, add_pos_z | offset added to the camera and every character |

## The script (.dds)

`data/demo/<folder>/<name>.dds`, little-endian:

```
0x00 "dds\0", two floats, u32 0
0x10 s16 frame count, s16
0x14 u8 directional lights, u8 spot lights, u8 point lights, u8 -, u8 characters
0x19 16-byte character names ("hhh_jms_pos", "agl_pos", ...)
then records until the frame value 0xffff:
     u16 frame, then sections "u8 object, codes..." until object 0xff
```

Objects: 0 keys, 1 camera, 2.. the lights, then the characters. Every section reads codes until a
code it does not know (by convention 0x0b), which is consumed:

| section | PC | codes |
|---|---|---|
| keys | 0x49fab0 DdsPlayKey | 0x10 cut: the values of this record are raw floats; 0x11 start the next motions; 0x14 unknown (a flag nothing in the player reads) |
| camera | 0x4a0000 DdsPlayCamera | 3 position, 4 interest, 5 angles (RotationToInterest 0x49fd30, unused by every script), 6 roll (half float), 7 plane (raw float) |
| light | 0x4a0270 DdsPlayLight | 1 on, 2 off, 3 position, 4 interest, 5 angles, 8 colour, 9 two halves, 10 two halves |
| character | 0x49fb20 DdsPlayCharacter | 1 show, 2 hide, 3 position |

Positions are three raw floats after a cut key (they also become the new base) and otherwise three
half floats added to the base (DdsReadFloat2 0x49f750: a half float with the exponent rebiased and no
zero case). The player keeps two states, "last" and "next", reads every record up to the current
frame + 1 into "next" and blends the two by the fraction of the frame, at 30 frames a second.
The port does the same (`dds_advance`), except that it does not blend into a cut (the PC smears the
frame before a cut).

The camera is `vcSetEventCamParamRefView(position, interest, roll)` (0x51ced0) with the projection
distance `gCameraFOV = plane * 1.14702` (times 1.1 for demo 7, 0.91 for 0x5a and 0x5d, 1.5 for 100),
which the port turns into a vertical field of view like `gamecam.c` does (448-line screen).
Characters are placed at their track position plus add_pos with the rotation (0, pi, 0) every frame;
their motions turn them.

The parser in `demos.py` reads all 123 scripts of `data/demo` and `data/demo2` to their end marker.

## Motions

Key 0x11 calls DramaDemoAnimationStart (0x49f7e0): every character takes the next id of adr_anim;
the id's range in the `DramaDemo_AnimInfo` table (0x891908, 79 entries: name, kind, id, first and
last motion id) names the character, and the setter for its kind looks the motion up:

| kind | who | first id | record table | .anm offset table | setter |
|---|---|---|---|---|---|
| 0x100, 0x101 | lll / hll James | 950 | 0x6d8398 | 0x8bd158 | 0x53b120 |
| 0x102, 0x103 | hhl / hhh James | 1000 | 0x6e8288 | 0x8be8d8 (.cls 0x8bebc8) | 0x540f60 |
| 0x104 | Laura | 2500 | 0x6e6780 | 0x8be5d8 | 0x540a20 |
| 0x105 | lll Maria | 40 | 0x6d1ca0 | filled at run time (0x1d82188) | 0x53a090 |
| 0x106 | hhh Maria | 3000 | 0x6e74a8 | 0x8be728 | 0x540c60 |
| 0x107 | Angela | 3500 | 0x6e5aa0 | 0x8be4d8 | 0x540730 |
| 0x108 | hhh Eddie | 4500 | 0x6e4e08 | 0x8be3f8 | 0x5405c0 |
| 0x109 / 0x10a | Mary | 4000 / 4100 | 0x6d4fd0 / 0x6e4268 | 0x8bd098 / 0x8be39c | 0x53a920 / 0x5404e0 |
| 0x10b / 0x10d | boat / dog | 55 / 4900 | 0x6e22c8 / 0x6e3728 | 0x8be380 / 0x8be390 | 0x5400d0 / 0x5402a0 |
| 0x200..0x209 | Lying Figure, Mannequin, Pyramid Heads, Abstract Daddy, lll Eddie, Mary boss | 6000.. | see `demos.py` | | 0x53ddc0 .. 0x53c580 |
| 0x4xx | props, doors, items | 10000 | 0x6da4f8 | 0x8bd740 | 0x53b820 |

A record is `{s16 id, s16 stored frames, s16 speed (0x800 = 1), s16 first, s16 last, s16 loop}`;
the offset is a byte offset into the character's `.anm`. The motion starts when the key is read
and runs on the character's own clock (FUN_00538920 sets it up). The scripts' frame counts between
motion keys match the motions' lengths, which is also how the converter tells which script belongs
to which PlayInfo.

### The .anm frame layout (FUN_0054fa10, PS2 `shExec`)

A frame is, for every bone in order, a 4-bit code from a u32 header that covers 8 bones. Bit 3 of
the code is a flag stored in the bone (0x0da); bits 0..2 give the layout. Rotations are three s16
Euler angles in 4.12 radians; root bones store translations as raw floats, the other bones as half
floats (GetSF 0x54f850):

| code | bytes | meaning |
|---|---|---|
| 0, 7 | 0 | no data: the bone keeps its values |
| 1 | 6 | rotation |
| 2 | root 12 / 6, then 6 | translation, rotation |
| 3 | 16 | translation = second slot; rotation, 3 s16 (x 1/32768), 1 s16 (x 1/4096), 2 pad |
| 4 | root 12 / 6, then 16 | translation = second slot, new second slot (root: halves 4 bytes apart) |
| 5 | root 12 / 6, then 16 | translation into both slots |
| 6 | root 12 / 6, root 12 / 6, then 16 | translation and second slot |

`tools/sh2conv/sh2mdl.py` (load_anm) reads code 6 of non-root bones as 18 + 16 bytes instead of
12 + 16 and treats a missing bone as the bind pose; the demo player follows the PC (James' basic
animations have neither case, so `chr.py` is not affected).

### Faces: .cls cluster tracks and the models' clusters

Each motion of the hhh James, hhh Maria, Angela, Laura, hhh Eddie and Mary kinds has a cluster
track beside it in the `.cls` of the same folder: the setter passes `cls base + table[id]`
(0x8bebc8, 0x8be800, 0x8be558, 0x8be680, 0x8be468, 0x8bd0f0, 0x8be3c8) to FUN_00537780, which is
ClusterAnimeSet (0x506cd0). A track block (`clani.c` Header) is

```
u32 id 0x29843918 or 0x29853918, u16 revision, u16 flag, u16 n_clusters, u16 -, u16 n_frames, u16 -,
u32 offsets[n_clusters] (from the block start), each cluster: u16 n_keys, {u16 frame, s16 weight 4.12}
```

ClusterAnimeExec (0x506d40) evaluates every cluster at the character's motion frame with the
revision's function, Calc1 (0x506af0, not defined in Ghidra, read from the disassembly): binary
search for the keys around the frame, linear blend with the 12-bit frame fraction, and on the last
frame a blend from the last key into the first. The weights move the model's cluster nodes
(`model3_cluster_n.c` ApplyCluster):

| .mdl header (+0x40) | meaning |
|---|---|
| 0x44 / 0x48 | cluster node count / offset: s16 x, y, z at 1/16 scale in the bone space of the vertex |
| 0x4c / 0x50 | cluster count / offset: {u32 n, u32 offset} each, elements {s16 dx, dy, dz, s16 node} at 1/16 |
| 0x98 / 0x9c / 0xa0 | opaque / transparent cluster map counts, map offset: {u16 node start, u16 vertex start, u16 count} |

A mapped vertex sits at node / 16 (checked: node (76, -77, 971) is vertex (4.75, -4.8125, 60.6875)).
`demos.py` writes `pack/chr/<folder>/<model>.s3k` with every offset turned into model space for the
S3C vertices that come from mapped `.mdl` vertices (`chr.py`'s `convert_model` reports that order),
and the motion's track block into the scene file. `demo.c` evaluates the weights each frame, adds
the offsets to the bind positions of those vertices (CPU, about 850 vertices and 3,000 offsets a
model), flushes them, and skinning does the rest. The transparent maps (eyelashes on some models)
are left out because the S3C has only the opaque vertices.

## Scene lights

DdsPlay hands the script's lights to the light module every frame: the first group (the PS2 names
call it infinite) to the character point lights (FUN_0047ae60: position, colour, decay start and
end), the second to the one spot (FUN_0047b040: direction from position to interest, decay, cone
half angle as cosine) and the third to parallel lights 1 and 2 (FUN_0047b6c0, direction = -position).
The character light list (0x508a90) puts the point lights in with a linear ramp, full up to the
decay start and nothing from the end (0x50cff0). `demo.c` gives each lit scene its own PICA light
environment: James' room ambient as before (with the Enhanced Edition reveal factor), plus up to six
of the scene's lights as fragment lights with N.L, a distance LUT made from the same ramp and a soft
spot LUT. The colours are taken at half (the scripts go up to 2.0). The PC's exact character shader
gains were not traced, so this is a close look, not a match; the scene lights do not reach the map
either (the PC also lets the parallel overrides and the spot change the map's lighting).

## Voice and subtitles

DramaDemoMain starts the voice stream (`FUN_00514d60(stream_no)`) once `gCutsceneTimer`
(scene frame - 1) passes stream_start and holds the scene clock until the stream really plays.
DdsPlay shows subtitle `msg_start + i` from the current stage's message file while a second clock
(0x9509e8, frames since the start, not clamped at the script's end) is between start and end of
entry i. Some programs (EvProgLetterFromMary) keep calling DramaDemoMain after the script has ended
until the stream is finished, so the narration carries on over the last picture.

The port streams `pack/sound/voice/<n>.adx` (the `voice.afs` file copied as is, stereo ADX at
44.1 kHz) on a second ndsp channel (`audio_play_voice`, channel 1; music stays on channel 0), shows
the subtitles on the bottom screen, and ends a scene when the script and the voice are both done.
Like DramaDemoMain, the scene clock waits while the stream is starting (`audio_voice_position()`
is below zero until the DSP has begun on it), and after that it follows the stream: when they drift
more than two frames apart, the clock is set from the stream's position.

SubtitlesExec (0x49fe30) and SubtitlesManager (0x49fe70) play a voice with subtitles and no script
(the Angela scream before `agl_himei`, Laura at the piano, the radio in room 312 and six more).
The converter finds the nine calls (times, first message, stream) and writes `pack/demo/voice_<program>.voc`
(`"VOC1"`, s32 voice, s32 first message, u32 n, {u16 start, u16 end} x n). A "!" step in a sequence
plays one under whatever follows: EvProgPapaAttackAngela goes on to its scene after 105 frames
(0x71d598) and the piano scene after 212 (0x715f10), as the PC programs do.
The subtitle list has no count on the PC; the converter stops at 0xffff or where the times stop
rising or go past the end of the voice.

## Sequences

`s_seq` in `demo.c`, one string per `ProgDemo`, from the PC programs' state machines:

| step | meaning | PC |
|---|---|---|
| `key` | a scene | DramaDemoMain(PlayInfo) |
| `@name` | a movie, between the scenes | FUN_004a2b30 / 0x4a2bb0 |
| `!key:frames` | a voice-and-subtitles track; the next step waits that many frames or for its end | 0x49fe30 |
| `f87:a\|b` | `a` when game flag 87 is set, else `b` | the programs' flag tests |
| `s48:a\|...` | by the current stage | the four ending stages' copies |

Choices the PC programs make, now made the same way: the teddy bear with Maria along (gGameFlag
0x8000, flag 15: teddy_b, else teddy_a, 0x590090); Maria in the day room (flag 187: hagureru_c,
else hagureru_d, 0x58d990; the program toggles the flag afterwards); Eddie's second talk (flag 87:
gero_edi_h, else gero_edi_i, 0x59cdd0); the endings' last corridor and Mary's fall by stage (48
Leave, 49 Maria, 50 In Water, 51 Rebirth). Chains: the fence (hei_lau_a, hei.sfd, hei_lau_b), Maria
behind the bars (ikiteruka_1, rouya.sfd, ikiteruka_2), Eddie and the corpse (edi_korosu_a,
korosu_a.sfd, edi_korosu_c, korosu_b.sfd, edi_korosu_e), Maria's death (mar_dead_a, mar_dead_b), the
Maria meeting (deai.sfd, mar_deai_c, flash.sfd, mar_deai_e), the videotape (vtr, murder.sfd, tomatta),
the endings (the scene first, then their movies and the credits). START ends the whole request,
as the programs do (a skip in the first scene goes straight to their end state). `main.c` plays
the movies of a demo's name itself only when `demo.c` has no sequence with movies for it.

## Pack format (DEM1)

See the top of `tools/sh2conv/demos.py`. One `pack/demo/<key>.dem` per PlayInfo (version 2: motion
records are 16 bytes with the offset of their `.cls` track block): the original script, the motion id
list, the subtitle times, and per character its model (in `pack/chr/`, shared with the rest of the
pack, `.s3k` beside it for the face clusters) and its motions as the original `.anm` bytes with a
frame offset table. Version 1 files are refused; rebuild the pack.
`pack/demo/demos.txt` lists every key with its PlayInfo, program, stage, script, voice and size, and
the voice tracks. All 128 PlayInfos in the exe are converted (69 MB of scenes, 20 MB of models, the
voices), the Born From a Wish ones included: the DC extends DramaDemo_AnimInfo to 84 entries
(b_d1x, b_d8x, bdcard, i_oil and i_revo after the PS2's 79), and those scenes take the DC models
of `data/chr2` (pack folders `item2`, `mar2`).

How the converter finds things:

- PlayInfos: every `push imm32` / `mov eax, imm32` just before a call to 0x4a1c80, checked for a
  plausible record.
- Script: most programs load it into 0x1dbec40 through a file list in their own function. Others
  copy a stage buffer into the PlayInfo (`mov reg, [buffer]` ... `mov [PlayInfo + 4], reg`), and the
  stage fills that buffer with FUN_00492a50(file list) (`mov [buffer], eax` after the call); the
  converter follows both, so EvProgHospitalBack1st plays go_to_ura_a (0x1d8645c) and
  EvProgMariaNotHotel mar_deai_00 (0x1d865a8). Within what is allowed, every script is scored against
  the PlayInfo's motion list (the id ranges must name the script's characters, the list must have
  exactly as many valid ids as the script uses, and one-shot motions should last until the next
  key); ties go to the file list order of the program, and a script that fits one PlayInfo exactly
  costs others extra. A PlayInfo without a motion list is allowed (DramaDemoAnimationStart then does
  nothing). Motion records are indexed like the PC does, without trusting their id field (lll
  Maria's motion 46 says 45).
- Motions: the .anm in the script's folder first, then the same name in other demo folders
  (boat2 plays from boat1's file) and `chr/jms/jms_*.anm` for the in-game James kinds; a candidate
  must parse to the end of the file or to another motion's start. The in-game James kinds (lll, hll)
  take their motions from the stage's own `chr/jms/jms_stg_<stage>.anm` first (the stage of each
  program comes from the ev_prog tables, Stage_Data + 8), which is where EvProgPapaAttackAngela's
  motion 969 is. About twenty motions end a little before their file does (kanaami, papa_agl,
  reizouko, mar_dead, hagureru); the converter says so and keeps the script folder's file.

## PC functions

| PC | PS2 name | what | confidence |
|---|---|---|---|
| 0x4a1c80 | DramaDemoMain | per frame: init once, voice stream, DdsPlay, skip button | high |
| 0x4a0b30 | DramaDemoInit | header, characters (find or create), first record; asserts at demoview.c 364, 387, 400 | high |
| 0x4a0f30 | DdsPlay | records up to frame + 1, blend, camera, lights, characters, subtitles, clocks | high |
| 0x49fab0 | DdsPlayKey | key codes | high |
| 0x4a0000 | DdsPlayCamera | camera codes | high |
| 0x4a0270 | DdsPlayLight | light codes | high |
| 0x49fb20 | DdsPlayCharacter | character codes | high |
| 0x49f7e0 | DramaDemoAnimationStart | motion ids to characters | high |
| 0x49f750 | DdsReadFloat2 | half float | high |
| 0x49fd30 | RotationToInterest | angles to an interest point | high |
| 0x4a0560 | DramaDemoSkipLast | reads every record to the end and applies the last state | medium |
| 0x49fe30 / 0x49fe70 | SubtitlesExec / SubtitlesManager | voice + subtitles without a script (EvProgPapaAttackAngela's scream) | high |
| 0x506af0 | Calc1 | cluster key blend (revision 1, table 0x8a2ae8) | high |
| 0x47ae60 / 0x47b040 / 0x47b6c0 | | demo point lights / spot / parallel overrides | high |
| 0x508a90 | | character light list (reads the demo point lights through 0x47af00) | medium |
| 0x492a50 | | loads one file list entry into a new buffer (stage scene buffers) | medium |
| 0x49fe20 | DramaDemoNumber | returns the demo number | high |
| 0x54fa10 | shExec | one .anm frame into the skeleton | high |
| 0x54f850 | GetSF | half float with zero | high |
| 0x538920 | (m3_sc.c) | starts a motion on a character | medium |
| 0x506cd0 / 0x506d40 | ClusterAnimeSet / exec | .cls cluster weights | high / medium |
| 0x537950 / 0x48fcc0 | | find a character by kind and id / create one | medium |
| 0x51ced0 | vcSetEventCamParamRefView | event camera (ported in `sh2cam.c`) | high |

## Ported

- Script player (keys, camera with roll and plane, character tracks and visibility, cuts, lights),
  the motion id dispatch and the .anm frame decoder, 30 frames a second on the scene clock, held for
  the voice stream and kept to it.
- Faces: the cluster tracks and the models' clusters, morphed on the CPU each frame.
- Scene lights as PICA fragment lights on the characters, over James' room ambient.
- Sequences: chained scenes and movies, flag and stage choices, voice-and-subtitle tracks.
- Born From a Wish's seven scenes with the DC models.
- Characters drawn with the existing skinning path; models shared when two characters use the same
  one; everything is loaded at the start of a scene (models, motions, the script: up to 3 MB per scene
  plus the models) and freed when it ends; no allocation per frame.
- Voice on its own ndsp channel, subtitles, START to skip, fades in and out, the program's flags and
  James' position afterwards (the programs already did those).
- Enhanced Edition: ReduceCutsceneFOV, EnemyRevealLighting (also with scene lights), CutsceneUnequip.
- debug.txt: `demo <key> [frame]` plays a scene after `spawn` (in player mode), optionally from a
  frame (the voice is left out then); `main.c`'s `stage <n>` picks the stage when maps are shared
  (subtitles come from that stage's messages); the debug HUD shows the scene frame.

## Open issues

- Scene light gains are a guess (colour x 0.5 x brightness, diffuse N.L). The PC's character shader
  is traced for play (`chrlight.c`), but scenes still use their own lights in `demo.c`, and the scene
  lights do not change the map.
- A model shared by two characters of one scene would show the second one's face on both (the
  vertex buffer is morphed in place); no scene shares a model that has clusters.
- The transparent cluster maps (a few vertices on some models) are not morphed.
- voice_5769b0 and voice_591f20 play from the stage code (`prog_stagefn.c`). The Born From a Wish
  tracks voice_571860, voice_571880 and voice_5718a0 (and voice_571a30, voice_577fa0) are converted
  but nothing plays them; `prog_wish.c` treats 0x49fe30 as a BGM change, while `prog_stagefn.c` uses
  the same function as the voice call, so those programs are the place to look.
- The piano scene is split as on the PC: piano_lau_b, then Laura's letter from the stage program
  (step 0x18 of 0x57beb0), then piano_lau_e as its own request (`DEMO_LAURA_PIANO_END`).
- The music box plays org_b when the puzzle is solved and org_a when not, as the PC program does
  (0x57c470, `prog_hotel.c`).
- Maria as the player model in Born From a Wish is done in `combat.c` (see ENEMIES.md, batch 4).
- Motion ids whose first frame is not 0 would need the PC's stride arithmetic (FUN_00538920); no
  demo motion has one.
- agl_himei for the Angela scream is still a best fit: its script comes from a stage buffer
  (0x1d861d4) whose loader was not found; the motion it plays (969, 135 frames) is in the stage's James
  file, while agl_himei's own 67-frame James file matches its script.
- Keys with an address suffix (`mry_drop_7b3eb8`) depend on the converter's matching order; if the
  matching changes, check `s_seq` in `demo.c` against `demos.txt`.
