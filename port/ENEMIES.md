# Enemies, weapons and Maria

This covers the Lying Figure, the Mannequin, the Bubble Head Nurse (and its town variant), the creepers
and Pyramid Head, James' plank, guns and stomp, and Maria as a companion. The code lives in
`port/source/enemy.c/h`, `port/source/combat.c/h` and `port/source/maria.c/h`; `main.c` only calls into
the first two (see the hook list at the end), and `enemy.c` drives Maria. Everything here was found by static analysis of `sh2pc.exe` (Director's Cut,
image base 0x400000) with the PS2 beta debug info for names. Addresses are PC virtual addresses.

## Which model is which enemy

The Enemy_List `kind` is mapped to model files by the table at **0x88d408** (16-byte rows: kind,
then pointers to the mdl, anm and kg1 file entries; the loader 0x493430 walks it):

| kind | folder | enemy |
| --- | --- | --- |
| 0x200 | `chr/scu` | Lying Figure |
| 0x201 | `chr/mkn` | Mannequin |
| 0x202 | `chr/tyu` | creepers (the cockroaches) |
| 0x203 | `chr/ike` | not identified yet |
| 0x204 | `chr/pap` | not identified yet (probably Abstract Daddy, "papa") |
| 0x205 | `chr/edi` | Eddie |
| 0x206 | `chr/bos` | not identified yet (a boss) |
| 0x207 | `chr/nse` | Bubble Head Nurse |
| 0x208 | `chr/red` | Pyramid Head |
| 0x209 | `chr/oni` | Pyramid Head of the hospital chase (EvProgChaseInHospital 0x587db0) |
| 0x20a | `chr/arm` | Mandarin (the arms under the floor grates) |
| 0x20b | `chr/xoo` | a tougher nurse (same skeleton and 658-frame anm as `nse`, the nurse code with health x1.2): the town after the hospital and the Otherworld hospital |
| 0x20c, 0x20d | `chr/item/ty2.mdl`, `ty3.mdl` | props, no animation |

So the Lying Figure is **`chr/scu`**, not `chr/arm`: `arm` is the Mandarin (its PS2 code, `en_arm.c`,
has `enARMCtrlHand`, `enARMCheckFloor` and a hand position). The apartment stages 6..12 and 58 use
kinds 0x200 (Lying Figure), 0x201 (Mannequin), 0x202 (creepers) and one 0x208 (Pyramid Head).

The enemy control code is called through two local tables of function pointers indexed by the
EnLOCAL kind byte (1 = Lying Figure): `enExecTask` 0x4a88e0 (control) and `enInitData` 0x4aa5e0 (init;
Ghidra mislabels it `enTransID`). Index 1 is 0x4c7070 / 0x4c46c0, which is where the en_scu code is.

## Converting the assets

All with the existing `tools/sh2conv/chr.py` (it handles a rigid two-bone weapon model as is):

```
PY=".venv/bin/python -I"; D="Silent Hill 2/data"
$PY tools/sh2conv/chr.py "$D" pack scu scu.mdl scu.anm              # Lying Figure, 30 bones, 779 frames
$PY tools/sh2conv/chr.py "$D" pack mkn mkn.mdl mkn.anm              # Mannequin, 17 bones, 720 frames
$PY tools/sh2conv/chr.py "$D" pack wp wp_kakuzai.mdl                # wooden plank, 2 bones
$PY tools/sh2conv/chr.py "$D" pack jms hll_jms.mdl jms_wpkaku.anm   # James with the plank, 1024 frames
$PY tools/sh2conv/enemies_pack.py pack                              # events/enemies.txt (needs stage_NN.json)
```

Batch 2 adds:

```
$PY tools/sh2conv/chr.py "$D" pack nse nse.mdl nse.anm              # Bubble Head Nurse, 30 bones, 658 frames
$PY tools/sh2conv/chr.py "$D" pack xoo xoo.mdl xoo.anm              # its town variant, same skeleton
$PY tools/sh2conv/chr.py "$D" pack tyu tyu.mdl tyu.anm              # creeper, 22 bones, 8 frames
$PY tools/sh2conv/chr.py "$D" pack red red.mdl red.anm              # Pyramid Head, 45 bones, 373 frames
$PY tools/sh2conv/chr.py "$D" pack mar lll_mar.mdl lll_mar.anm      # Maria (gameplay model), 36 bones, 851 frames
$PY tools/sh2conv/chr.py "$D" pack wp wp_handgun.mdl                # and wp_shotgun.mdl, wp_riflgun.mdl
$PY tools/sh2conv/chr.py "$D" pack jms hll_jms.mdl jms_wpnone.anm   # and jms_wphand, jms_wpshot, jms_wprifl
```

Batch 3 adds:

```
$PY tools/sh2conv/chr.py "$D" pack pap pap.mdl pap.anm              # Abstract Daddy, 29 bones, 686 frames
$PY tools/sh2conv/chr.py "$D" pack oni oni.mdl oni.anm              # the spear Pyramid Head, 41 bones, 360 frames
$PY tools/sh2conv/chr.py "$D" pack bos bos.mdl bos.anm              # Mary, 45 bones, 228 frames
$PY tools/sh2conv/chr.py "$D" pack arm arm.mdl arm.anm              # Mandarin, 15 bones, 405 frames
$PY tools/sh2conv/chr.py "$D" pack ike ike.mdl ike.anm              # Flesh Lip, 16 bones, 541 frames
$PY tools/sh2conv/chr.py "$D" pack edi lll_edi.mdl lll_edi.anm      # Eddie (gameplay model), 35 bones, 437 frames
```

Copy `pack/chr/scu`, `mkn`, `nse`, `xoo`, `tyu`, `red`, `mar`, `pap`, `oni`, `bos`, `arm`, `ike`,
`edi`, `pack/chr/wp`, the five `pack/chr/jms/jms_wp*.s3a` and `pack/events/enemies.txt` to the SD
pack. Memory once loaded (each kind loads on first use and stays):
scu.s3a 467 KB and mkn.s3a 245 KB, jms_wpkaku.s3a 840 KB (heap), scu.s3c 86 KB, mkn.s3c 75 KB, plank
10 KB (vertices in linear memory).

`events/enemies.txt` has one line per Enemy_List row: `stage kind id x y z rot_y status condition`.

## Enemy_List fields (20 bytes)

* `id` is a bit number in the killed-enemy bitfield at 0x1f7d9e0: 0x48fc30 skips the row when it is set.
* `rot_y` is a 4.12 fixed-point angle in radians (12867 = pi, 6433 = pi/2), same facing convention as
  the port (forward = sin, cos).
* `status` goes to the SubCharacter (+0xec) and picks the start state in the init (see below).
* `condition` (0x48f5b0): bit 0 = not on easy/beginner, bit 1 = not on normal, bit 2 = not on hard.
  Among the codes in bits 6..15, 0x340 means "not while the dry cell is held" (item 55): the creeper
  room in the labyrinth (stage 30) gets its creepers and nests from EvProgUseDryCell (0x586f40).
* A blow that lands while James' damage motion still plays takes its health and nothing more
  (0x5350d0); the motion is not started again, so a room of biting creepers can't hold him still.
  Bits 6..15 select a story test. The ones used in the apartments are ported (flag n means game flag n,
  bit n&31 of the word at 0x1f7d5e0 + 4*(n>>5)):

  | code | spawns when |
  | --- | --- |
  | 0x080 | flag 67 |
  | 0x100 | flag 91 |
  | 0x140 | flag 146 |
  | 0x180 | flag 68 and not flag 70 |
  | 0x1c0 | not flag 68, or flag 70 |
  | 0x200 | not (flag 68 and not flag 70), and the flashlight has been found (assumed true) |
  | 0x380 | flag 251 and not flag 619 (the town after the hospital: the 0x20b nurses) |
  | 0x3c0 | not flag 482 |

  The other codes (0x40, 0xc0, 0x240..0x600) test item bits and other state and are treated as true.
* A row spawns in the loaded room when the room's collision has a floor under (x, z) (the PC compares
  room ids from 0x494db0, which gives the same answer for the apartments).

## Lying Figure animations

`EnSCUAnime` (0x893130, 33 x {u16 anime id, u8 loop}) gives the AI's anime numbers; the ids 5001.. index
the `_AnimeInfo` table `scu_anim` (12 bytes: name, frame count, speed, start, end, loop) whose live copy
is at 0x6e1554 (`shCharacterEnemySCUAnimeSetP` 0x53de20 reads `0x6e1548 + (id - 5000) * 12`). Speed 2048 is
the normal rate (30 frames per second, as for James). Frames are in `scu.anm`; the meaning comes from the
AI code that plays them and was checked with `chrpreview.py`:

| # | id | frames | loop | use |
| --- | --- | --- | --- | --- |
| 0, 1 | 5002 | 15-45 | 1 | walk (root motion from the planted foot, 13.4 units per frame) |
| 2 | 5001 | 0-14 | yes | stand |
| 3, 4 | 5005/6 | 77-100 | | attack: bends over and sprays acid from the torso |
| 5, 13 | 5007 | 101-120 | | flinch |
| 6, 7 | 5008/9 | 121-183 | | flinch front / back |
| 8, 9 | 5010/11 | 184-224 | | flinch, blow from the left / right (plank swings) |
| 10 | 5012 | 225-243 | | flinch, blow from above |
| 11, 12 | 5013/14 | 244-314 | | staggers |
| 14..21 | 5015..5022 | 315-500 | | knocked down (15 ends face up; 17/18 plank, 19 from above) |
| 22 | 5024 | 537-583 | | gets up from face down |
| 23 | 5023 | 501-536 | | gets up from face up |
| 24, 25 | 5028/5027 | 675-689 / 660-674 | yes | writhes on the floor (face down / up) |
| 26, 27 | 5033/5032 | 367 / 351 | | lies still (face down / up) |
| 28 | 5003 | 46-62 | yes | crawls along the floor |
| 29 | 5004 | 63-76 | | drops from standing to crawling |
| 30..32 | 5031/5035/5034 | 714-778 | | event poses (status 13..15) |

Every change of anime crossfades from the pose shown over 8 frames, as 0x4a9920 sets it through the
kind's setter (0x892740, the Lying Figure's 0x53de20) into 0x538920 mode 4 (`anim_set`, `enemy_draw`,
chr.c's `chr_pose_blend`). It cuts in (mode 2) in an enemy's first 10 anime steps and for anime 0x1c,
which the Lying Figure's crawl and the mannequin's drop set with the +0x10 bit 0x8000. The other kinds'
0x8000 cases (the death entries of 0x4b5190, 0x4b9460, 0x4c00a0) are not ported and blend. Until
2026-10-28 every change was a cut, so each flinch, fall and attack snapped into place.

## Lying Figure AI

Control entry 0x4c7070 (`enSCUCtrlMain`) dispatches on the control mode (1 automatic, 2 sleep 0x4c3a00,
3 go-playable 0x4c4a50, 4 event 0x4c4500, 6 wait to regenerate 0x4aa760). Automatic (0x4c6f50) runs one
of twelve states, in the order of the PC table:

| state | PC | port | what it does |
| --- | --- | --- | --- |
| 0 Wander | 0x4c4b30 | yes | walks to random points 1000 ahead (+-45 degrees); stays within 3500 of its start (type bit 1); stops to stand now and then |
| 1 Precaution | 0x4c4f50 | yes | stands (random 2..4 loops), turns to James |
| 2 Chase | 0x4c5070 | yes | walks at James, slower while turning (0x4a66c0); calls the others within 4000 (communication queue) |
| 3 Backward | 0x4c5230 | yes | walks backwards when James is within 750 (type bit 2, not on beginner) |
| 4 Ambush | 0x4c5370 | no | needs the `ambush_apart` rectangles (hard and up only); falls back to Wander |
| 5 WaitCar | 0x4c5740 | simplified | lies in wait (status 8) until James is within 2000, then crawls |
| 6 Crawl | 0x4c5800 | yes | dashes along the floor at 1500 x crawl rate units/s, grabbing at James (attack 43), rests, writhes, or stands up |
| 7 Attack | 0x4c63f0 | yes | sprays acid (attack 42), turning slowly toward James; may repeat |
| 8 Damage | 0x4c65d0 | yes | applies the hit: flinch, or a knock-down when endurance runs out |
| 9 Confuse | 0x4c6840 | no | only for special hits; goes to Precaution |
| 10 Down | 0x4c6900 | yes | lies, writhes, regains health, then crawls (80% when face down) or gets up; at 0 health with James locked on it and no kick or stomp, the throes (sub-state 4): it lies as it fell for 200 ticks, each further blow knocks it back less ((max - n) / (max + 1)) and waits again, up to 4 + rand(difficulty + 2) blows; otherwise it dies at once |
| 11 Dying | 0x4a96c0 | yes | body stays; the Enemy_List id is marked killed |

Senses (0x4a9aa0 with the EnSCU table at 0x8931b8): sight distance 5000 in lit places, 4000 in the dark
with the flashlight on, 750 with it off. The row is DAT_01f7fa24, which 0x4a88e0 sets every frame: 0
where the place is lit (0x47b1f0, gFlashlightAvailable 1), 3 in the dark with the flashlight off, and
with it on 1 in stages 1..4 and 17 (0x494d80), else 2; 0x4a5a60 makes it 2 in stages 13 and 16 and in
rooms 0x89, 0x8f, 0x90 and 0xa2 elsewhere (`light_index`, with `enemy_set_dark` from main.c). Until
2026-10-28 the port never chose row 0, so in every lit place an enemy saw James only within 750, as
if it were dark with the light off, and walked about blind in daylight; the paces, reaches and turn
rates that read the row were off the same way; view half angle 70 degrees; feels James within
{0, 750, 1000, 1500, 1500} by difficulty; spits when James is within the acid reach and 5 degrees of
straight ahead. Being lit by the flashlight raises an alert value (0..3 s) that keeps it aware.

Per difficulty (gActionDifficulty 0..3, 4 unused; the port plays Normal = 2 unless `debug.txt` has
`action <0..3>`):

| value | table | beginner | easy | normal | hard |
| --- | --- | --- | --- | --- | --- |
| health | 0x4c46c0 | 400 | 400 | 700 | 1500 |
| endurance | 0x4c46c0 | 10 | 190 | 285 | 380 |
| walk anime rate | 0x4c3ea0 | 0.7 | 0.8 | 1.0 | 1.2 (x hp% * 0.004 + 0.6, x1.2 dark + light on, x0.8 dark + light off) |
| crawl rate | 0x4c3f30 | 1.5 | 1.7 | 2.0 | 2.0 (x hp% * 0.0025 + 0.75) |
| acid reach | 0x4c3fb0 | 750 | 900 | 1200 | 1500 (x1.5 lit / light on, x0.8 dark without light) |
| spit chance | 0x4c4010 | 0.5 | 0.5 | 0.7 | 0.8 |
| repeat spit | 0x4c40a0 | 0 | 0 | 0.2 | 0.5 |
| turn speed | 0x4c4140 | 1.5 | 2.1 | 3.0 | 3.6 degrees per 1/60 s |
| turn while spitting | 0x4c4190 | 0 | 0 | 0.3 | 1.5 degrees per 1/60 s |
| endurance regain | 0x4c3e30 | 0 | 10 | 30 | 60 per second |
| time down | 0x4c4220 | 900 | 400 | 300 | 240 ticks (+0..600 when spawned lying) |

Health and endurance get a random spread (0x4a5b50, 0x448910): x(1 + r) with |r| <= 0.5 for health
and 0.3 for endurance, doubled upward half the time above easy. Spawn status scales them (1: x1.2,
2: x0.5, 3/4 lying: x0.3, default x1.0). Endurance is the stagger budget: every hit takes the damage
from both; a hit that leaves endurance at 0, or fails the roll `rand <= endurance * 0.002 + 0.8`, knocks
it down (0x4c65d0). A knocked-down figure takes downward swings and stomps; at 0 health it twitches a
few times and dies.

Attacks (sh2_attack_list; the copy the battle code reads is at 0x6ec568, one of many identical copies,
the first at 0x629000; 36-byte shAttackInfo: id, kind, ap, sp, min/max range, hit check,
first/last frame, sound, effect):

| id | kind | ap | sp | range | frames | user |
| --- | --- | --- | --- | --- | --- | --- |
| 42 | 4 spray | 7 | 5 | 0..250 | 9-22 | Lying Figure acid (anime 3) |
| 43 | 3 strike | 5 | 5 | 0..250 | 0-16 | Lying Figure grab while crawling |
| 12 | 2 melee | 100 | 80 | -150..350 | 12-15 | plank swing |
| 13 | 2 melee | 80 | 80 | -150..350 | 8-11 | plank combo swing |
| 14 | 2 melee | 140 | 80 | -150..350 | 8-10 | plank downward swing |
| 15..17 | 2 melee | 120 / 96 / 160 | 100 | -150..540 | | steel pipe |

How the acid is placed is simplified: James is hit once per spit when he is in front within the acid
reach + 150 and 380 to the side, with a clear line through the room walls.

The acid itself (0x4c3c20, anime 3 past its frame 10 and short of 20): a puff each tick through
0x4a4580 (effect 0x1e, the PS2 beta's enEfctSetPoisonFog), from 700 up the body along its facing,
2500 long in stages 1..4 and 17 and 1250 elsewhere, tipped by half of James' height above it. Each
puff flies, slows, sinks and fades over 22 ticks (0x4a3a30) as a soft disc with the poison texture
(0x4a3d00), drawn by `mist.c` with the hyper spray's puffs (`mist_poison`). The PC's grey (0x80, 0x18
in the dark with the light off) modulates the texture's own tan, a yellowish haze; the pack keeps
only that texture's alpha, and the port tints the acid a bile green by the same grey (its choice). Before
2026-10-29 the port drew none, so a spit hurt James with nothing between them; and a second spit
in a row (the repeat chance) played no anime at all, since the attack anime had already played out
and `anim_set` kept it: a played-out anime now starts again, as 0x4a9920 always does.

## Mannequin

Kind 0x201, `chr/mkn` (17 bones, `mkn.anm` 720 frames; `mkn_311.anm` is a 489-frame event set that is
not used yet). Convert with `chr.py "$D" pack mkn mkn.mdl mkn.anm`. The PC control code is the en_mkn
port at 0x4c0d70..0x4c38dc (main 0x4c3890, automatic 0x4c3790, init 0x4c15c0). EnMKNAnime is at
0x893070 and `mkn_anim` (ids 5101..5134) at 0x6b1f9c:

| # | frames | use |
| --- | --- | --- |
| 0, 2 | 0-23 | stand |
| 1 | 24-54 loop | walk (19.2 units per frame of root motion, skeleton pair 15/16) |
| 3 / 4 | 55-89 / 90-124 | attack: arm swing (attack 44) / kick (attack 45) |
| 5..13 | 125-323 | flinches (8/9 from the left / right, 10 from above) |
| 14..21 | 324-470 | knocked down |
| 22 / 23 | 515-564 / 471-514 | gets up (face down / up) |
| 24 / 25 | 662-677 / 646-661 loop | writhes |
| 26 / 27 | 371 / 355 | lies still |
| 28 / 29 | 694-708 / 709-715 | drops from above / lands |

States (0x4c3790 order) and what the port does:

| state | PC | port |
| --- | --- | --- |
| 0 Stay | 0x4c18f0 | yes: frozen in an attack pose (anime 3 or 4, frame 0..17), wakes when the flashlight is on it within 2250 and 40 degrees or James is within 900 (0x4c0eb0); within 1000 it strikes at once; wakes others within 1000 (communication kind 2) |
| 1 Event, 2/3 Drop | 0x4c1bb0, 0x4c1cd0, 0x4c20f0 | no (story events, falling from the ceiling with gravity 9800 units/s^2); they stand still instead |
| 4 Wander | 0x4c2420 | yes: targets 1500 away, stays within 5000 of its start |
| 5 Precaution, 6 Chase | 0x4c2860, 0x4c2950 | yes |
| 7 Circle | 0x4c2a90 | yes: when it loses sight of James it walks to a point 1000 from him, 120 degrees off his facing |
| 8 Attack | 0x4c2c70 | yes: alternates arm swing and kick; chance {0.4, 0.5, 0.8, 0.9} (0x4c1260), repeat {0, 0, 0.2, 0.6} (0x4c12b0) |
| 9 Damage, 11 Down, 12 Dying | 0x4c2e20, 0x4c3250, 0x4a96c0 | shared with the Lying Figure code (no crawling; down time {900, 500, 400, 300} ticks, 0x4c13a0) |
| 10 Confuse | 0x4c3030 | no (it turns on another enemy lying close by) |

Root motion (0x4c1020 applies the foot-based 0x4a7a20 to anime 1, 3, 4, 11 and 12): the walk moves
19.2 units per frame, the two attacks lunge about 500 units, the staggers move back or forward about
400; the Lying Figure's staggers (11, 12) move too. The port uses the average per frame.

Sight table 0x8930f0: sight 5000 / 3000 / 3000 / 900 by light, half angle 60 degrees, the same hearing
ranges, attack range 1100 within 15 degrees. Attacks 44 and 45 (kind 2, ap 18, sp 10, reach 400, frames
19..22). The reach is counted from the striking limb, whose position function was not ported; the port
adds 300 for the limb and James' radius of 130. Health and endurance use the same tables as the Lying Figure times 0.7 by default.

## James and the plank

The weapon file table at 0x88d2f0 (20 bytes: grip flags, anm, wp mdl, rwp mdl, kg1) gives the grip
types: 1 handgun, 2 shotgun, 3 rifle, 4 hyper spray, **5 wooden plank** (`jms_wpkaku.anm`,
`wp_kakuzai.mdl`), 6 steel pipe, 7 chainsaw, 8 great knife, 9/10 Maria's weapons (DC). The attack code
0x52fae0 / 0x531a00 selects animation entries per grip; for grip 5 it uses the `_AnimeInfo` table at
0x6ceeb0 (names 351..382) and for every grip the base table at 0x6ceac4 (names 101..133). James is
animated entirely from `jms_wpkaku.anm` while he carries the plank (frames 0..721 are the base moves
with the plank in hand, 722.. the weapon moves).

| name | frames | speed | use |
| --- | --- | --- | --- |
| 101 | 1-10 | 256, loop | stand |
| 104 / 108 | 51-74 / 147-170 | 1408 / 2048, loop | walk / run |
| 121 | 453-470 | 1024 | hurt |
| 111 / 112 | 221-245 / 256-280 | 1024 | knockdown and getting up (lower state 0x18, 0x52eb40 picks one); the port's death plays the set's fall to its last frame on the floor (`combat.c` `die_range`) |
| 351 / 353 | 722-733 / 734-745 | 2048 | ready, plank over the right / left shoulder (352/354 reversed: put away) |
| 359 / 360 | 746-755 / 756-765 | 320, loop | hold ready, right / left |
| 361 / 362 | 766-775 | +-1536 | change shoulder |
| 363 / 369 | 776-797 / 810-831 | 1536 | swing from the right / left (hit frames 12-15, attack 12) |
| 365 / 371 | 780-797 / 814-831 | 1536 | combo swing (attack 13, hit frames 8-11) |
| 364 / 370 | same | -1024 | rebound: played back from the current frame when a swing connects |
| 367 / 373 | 800-809 / 834-843 | 1024 / 1536 | follow-through back to the other shoulder |
| 375 / 379 | 846-873 / 876-903 | 1024 | downward swing at a figure on the floor (attack 14, frames 8-10) |

Which shoulder each pose is was checked by comparing bone positions (end of 351 = 359, end of 367 = 360,
end of 373 = 359). The attack ids for the plank are inferred, not read from code: the hit windows of ids
12/13 fall on the same absolute frames for 363/365 (788) and 369/371 (822), and the pipe's 15/16 line up
the same way with its own table (401..432, 782).

The plank model's root bind matrix equals James' bone 35 (right hand), and its second bone's bind pose
equals frame 4 of `wp/jms_weapon.anm`, so the plank is drawn rigidly with bone 35's skinning matrix.
Hits are tested with the plank's handle-to-tip segment (from the posed hand bone) against each enemy's
hit column (radius size x 0.8 from 0x892800; lying bodies use a wider column over the torso) inside the
attack's frame window.

Controls: hold **R** to ready (James turns to the nearest enemy within 4000 and 100 degrees, or with
the circle pad, and can step forward or back at 500 / 350 units/s), **A** swings, A again during the follow-through chains the next swing, releasing R puts
the plank away. A target on the floor gets the downward swing. James has 100 health; a hit makes him
flinch (anime 121) and gives one second of immunity; at 0 he falls and stays down (the PC holds the
damage motion's end, 0x559100 setting 0x1fbad17 to 2), and the game over follows.
The acid spray (attack 42, kind 4) takes health without making James drop his guard while the plank is
up; the PC handles fog attacks in their own routine (`shBattleAttackByEnemyFog`), which was not
decompiled, so this is a gameplay choice: with a flinch on every spray the fight could not be won.

Sounds (sfx.h, ids from `pack/sound/se_index.txt`): spray 12000, Lying Figure noises 12001..12008
every 5 s at the earliest (0x4c4350), crawling 12009..12014 every 20 ticks (0x4c43e0), footsteps 12019
(0x2ef3) on walk frames 2 and 18, pain 12017/12018; Mannequin footsteps 12500, noises 12505..12508,
pain 12503/12504; plank swing 11027 and hit 11032; James hurt 11020..11023 and dying 11024.

## Guns

`PlayerRequestAttack` (0x5569b0) turns the grip and the button into an attack id: handgun 1 / 2 and 3
for the reload, shotgun 4 / 5, rifle 6 / 7, plank 12..14, steel pipe 15..18, great knife 19..22,
chainsaw 23 / 24, Maria's knife in Born from a Wish 28..30, and 31.. for the kicks and stomps (36.. with
the chainsaw grip). The grip to item table at 0x534240 is {-1, 4, 6, 8, 12, 13, 14, 16, 15, 10, 17}, so
the PC item numbers are 4 handgun, 5 handgun bullets, 6 shotgun, 7 shells, 8 rifle, 9 rifle shells,
10 / 11 the revolver and its bullets, 12 hyper spray, 13 plank, 14 pipe, 15 great knife, 16 chainsaw.

* The rounds in the gun are the gun item's own count (0x49e620 reads `DAT_01f7d3ec + item * 2`); the
  bullets item is the spare ammunition. A reload (0x558bd0) moves `min(clip - loaded, spare)` at frame
  21 of the handgun's 209 / 210, 12 of the shotgun's 260 and 17 of the rifle's 304. Clips are 10, 6 and
  4 (the amounts `p_item_get` gives with a new gun).
* The handgun and rifle fire one ray (0x543600), the shotgun ten (0x543810) in random directions
  within 30 degrees of the aim. The port aims at the body centre of the locked target (or level ahead),
  stops the ray at the room walls (`col_clamp_ray`) and tests it against the enemies' hit columns.
* Attack entries used: 2 (handgun, ap 120, sp 60, reach 7500), 4 (shotgun, ap 100 for each pellet that
  lands, sp 120, reach 5000), 6 (rifle, ap 500, sp 150, reach 9000).
* Sounds: shot 11048 (0x2b28, handgun and rifle) and 11051 (shotgun); empty click 11030 / 11052 /
  11049 (0x533ff0); reload 11029 / 11053 / 11050.
* The gun models' root bind equals James' bone 35 like the plank's; their bone 1 is the muzzle, 125 /
  294 / 483 units out. The flash is two crossed additive quads there for 0.06 s.

Animation sets: `jms_wphand.anm` (handgun: raise 744-756, aim 771, shot 772-786, reload 792-831),
`jms_wpshot.anm` (raise 722-736, shot 736-786, reload 917-956), `jms_wprifl.anm` (raise 722-741, shot
741-771, reload 772-811) and `jms_wpnone.anm` for bare hands; each set also has the base moves at
frames 0..721, so James' stand, walk, run and hurt keep working while it is loaded. Switching weapons
frees the old set and loads the new one (840 KB each), so only one is in memory.

Controls: **R** raises the gun and locks on to the nearest enemy in reach (or turns with the circle
pad), **A** fires and reloads by itself when the gun is empty and spare rounds remain, **L** cycles to
the next weapon James owns, and "use" on a weapon in the item list equips it (again: bare hands).
A gun stays owned with an empty clip through a hidden marker item (250 / 251 / 252); the item list
skips items 250 and up.

## Stomp

At a downed enemy the PC requests a kick or a stomp (0x534210: request 6 + (pressure > 0xc0)), and
lower-body state 0x1d (0x531a00) plays anim 132 for the kick (682-701, speed 768: the right foot swings
up and forward at knee height) or 133 for the stomp (702-719, speed 512: the foot comes down on the
floor in front). Both start and end on the standing pose. The attacks are 31..35 (kick, frames 7..10)
and 36..40 (stomp, frames 3..9), all kind 1, ap 9999.9, sp 0.1, reach 0..350; the check in 0x4a8d80
only lets them count on an enemy that is down, so they finish a body on the floor. A digital button
always reads full pressure, so the PC as shipped only ever stomps. The port follows the Enhanced
Edition's "EnableHoldToStomp" (on by default, `holdstomp` in options.txt): with bare hands or a gun,
**R + tap A** kicks and **R + hold A** for 0.2 s stomps. The sound is James' effort 11021 at 0.9 on
frame 4 of the stomp or 8 of the kick, whatever the foot meets (0x549e20; Maria 17032, 0x549500);
there is no thud (the port played the steel pipe's 11040 on a hit until 2026-10-31).

**Keeping apart.** Every frame after the characters move, clCollectCharaALL (0x525ed0) puts each
column back where it stood, then in registration order replays its move and pushes it out of the
columns it overlaps (clCheckSubColumnToColumn 0x5298a0, the overlap plus 1, at most its radius); the
other gets back a share by weight (0x522940), up to three passes. James' column is 175 wide 25 ahead,
wider with a weapon held or swung (0x52ed60, eased 5 a tick by 0x534d70); the Lying Figure,
Mannequin and nurse weigh 2, the creepers 1 (`enemy_push_apart`, `combat_column`). A blow runs from
the weapon's bone 0 towards bone 1 from the attack's min to max range (the plank and pipe from -150)
and sweeps five steps back to last frame's end (0x543b10, 0x527b60), so an enemy pressed against
James is still hit.

How often: as often as there is a body to stomp. 0x542a10 offers the kick or stomp at the first
camera slot whose enemy is down (+0x148 bit 4) and not dead (bit 2 clear) and at James' feet
(0x542060). Each stomp is a whole attack, and James can stomp again as soon as it ends while the body
lives (a figure lying with health left, or in its throes, see the Lying Figure's Down state). The
killing stomp sends it to Dying, and enDyingExec 0x4a96c0 then sets bit 2 and clears bit 4, so on the
PC a dead body is never offered again.

The port goes further, by choice (3DS-only): a Lying Figure, mannequin or nurse James killed stays
open to stomps for as long as he stays by it (`enemy_corpse_stompable`, `combat_scan` falls back to it
when no live one is down). Each stomp on it thuds and bleeds and changes nothing else. Once James is
more than 1000 units from the body (`CORPSE_LEAVE`, the stomp is offered within 600) it closes for
good. Bodies placed dead in a stage never open.

The plank keeps its own downward swing (attack 14, anims 375 / 379). That is its heavy attack: the PC
asks for it on every first press (pressure again), and the port follows the EE's "SwapLightHeavyAttack"
(`swapattack`): a tap is the light swing (12), holding A for 0.2 s the heavy downswing. A downed target
within 1400 units always gets the downswing (port choice: the level swing passes over a body on the
floor). Pressing A again during a plank swing chains the short swing from the other shoulder at once
(the PC sets `DAT_01fbad19` and the next request becomes attack 13 without the recovery), which is the
quicker rhythm the Normal fights needed.

## Bubble Head Nurse

Kind 0x207 (`chr/nse`) and 0x20b (`chr/xoo`, health and endurance x1.2) share the PC code at
0x4b9be0..0x4bb4c0 (EnLOCAL kind 6: control 0x4bb470, automatic 0x4bb390, init 0x4ba1e0). Anime ids
start at 5501 (`nse_anim` 0x6ea240); the port's table:

| # | frames | use |
| --- | --- | --- |
| 0 / 2 | 0-16 | stand (2 loops) |
| 1 | 17-50 loop | walk, 10.8 units per frame (feet 12 / 13, 0x8927a0) |
| 3 | 51-82 | overhead swing with the pipe (attack 46: kind 2, ap 20, frames 15..18) |
| 4 | 83-116 | short jab (attack 47: kind 3, ap 15, frames 13..16) |
| 5..13 | 117-346 | flinches (6 / 7 for shots from the front / back) |
| 14..21 | 347-557 | knocked down (15 / 16 for shots) |
| 22 / 23 | 593-631 / 558-592 | gets up |
| 24 / 25 | 643-657 / 632-642 loop | writhes |
| 26 / 27 | 418 / 395 | lies still |

| state | PC | what it does |
| --- | --- | --- |
| 0 Wander | 0x4ba3f0 | walks ahead at a speed factor 1.0..1.5 that drifts by 0.01 a frame; now and then stops to twitch (SE 12109/12110) |
| 1 Precaution | 0x4ba6d0 | stands and turns to James; back to wandering beyond 1500 |
| 2 Chase | 0x4ba7e0 | walks at James, slower while turning |
| 3 Attack | 0x4ba970 | swings (chance {0.1, 0.5, 0.7, 0.9}) or hesitates; the jab when James is close and to the side; turns while swinging on hard or with the overhead swing |
| 4 Damage, 6 Down, 7 Dying | 0x4bac00, 0x4bae80, 0x4a96c0 | the shared en_common code (down time {600, 480, 360, 300} ticks, 0x4b9fa0) |
| 5 Confuse | 0x4badc0 | only after the hyper spray (attack 8 / 9), which the port does not have yet |

Senses (EnNSE table 0x892e18): sight 4000 / 4000 / 3000 / 1000 by light, 80 degrees to either side,
hearing {500, 750, 1200, 1500}, result 3 when James is within 1500 and 5 degrees, 4 within 900 and 30
degrees. Walk rate {0.7, 0.8, 1.0, 1.2} x the speed factor, turn {0.5, 0.8, 1.0, 1.2} x 3 degrees a
frame (half in the dark without the light). Health {400, 500, 800, 1500}, endurance {10, 190, 285, 380}
(x1.2 / 0.5 / 0.3 by status as for the others). Sounds: footsteps 12100 on walk frames 1 and 18, the
radio-like breathing 12103..12106 at 0.6 every second, the attack cry 12107/12108, pain 12113..12116.

## Creepers

Kind 0x202 (`chr/tyu`, 22 bones; a cockroach about 140 units long) runs the code at 0x4ad1e0..0x4b0470
(EnLOCAL kind 3: control 0x4b0420, automatic 0x4b0370, init 0x4ae1f0). Two anime: 5201 still and 5202
the scuttle loop (frames 0-7). Creepers move by velocity, not root motion: {0.8, 0.9, 1.0, 1.1} x 1250
units/s, turning 5 degrees a frame.

| state | PC | port |
| --- | --- | --- |
| 0 Wander | 0x4af6f0 | yes: runs ahead, veering up to 60 degrees now and then (0.003 a frame) |
| 1 Chase | 0x4afa00 | yes: within {0, 500, 600, 750} of James (never on beginner or in the dark without the light) |
| 2 Target, 4 Wall, 5 Ceiling | 0x4afb90, 0x4aff00, 0x4b00f0 | no: the climbing (wall probes 0x4ae7d0 / 0x4aed10) is left out; they wander on the floor |
| 3 Run off | 0x4afd00 | yes: after a bite it runs round James at right angles until it is out of chase range |
| 6 Bite | 0x4ae360 | yes: attack 57 (kind 3, ap 5, reach 300) for {60, 40, 30, 20} ticks |
| 7 Damage | 0x4ae3e0 | yes: stunned {60, 40, 30, 20} ticks, or {600, 500, 400, 300} on its back once endurance is gone |
| 8 Dying | 0x4ae730 | yes |
| 9 Event | 0x4af450 | no (wanders) |

A creeper counts as down for the stomp at all times, so R + A on one at James' feet kills it. Health
{1, 250, 400, 600}, endurance {1, 100, 150, 240}. The PC also adds creepers when one is hit (0x4add40,
up to 12 in a room); the port does not. Sounds: the skitter 12200..12207 at 0.1 every third of a
second, pain 12208..12213 (labelled "??" in the index, next to the cockroach set).

## Pyramid Head

Kind 0x208 (`chr/red`, 45 bones) runs at 0x4be930..0x4c0cb0 (EnLOCAL kind 4: control 0x4c0c60,
automatic 0x4c0b00, event control 0x4bf4f0, init 0x4bf2d0). EnREDAnime (0x892fa0) gives 0 stand
(5301), 1 walk (5302: frames 0-29 at speed 1280, 14.3 units per frame, feet 12 / 13 at 0x892790), 2 an
idle loop (5310), 3 a swing (5303), 4 the long great knife swing (5304, frames 97-216), 5 a short swing
(5305), 6 the shove (5306), 7..9 lift, hold and let go (5307..5309), 10 the idle behind the bars (5311).
He cannot be killed. The port puts him where the stage programs need him and nowhere else:

* **Behind the bars** (stage 7, Enemy_List status 12, condition 0x180: flag 68 and not 70): event
  control, idle loop, can not be hit. He leaves when James first enters room 208 after the cry: stage
  7's `sound_call_after_load` (0x59bc50) plays sound 16000 there and sets flag 70. He stands at
  (-99400, 0, 31650) in ap50, 2600 behind the bars (the wall at z 29067, material 11), so James never
  gets closer than about 2600. His red comes from nothing of his own: ap50's DrawEnvData record
  (0x7fa5e8) has a fifth point light (0x7fa758) of colour (5, 0.9, 0.9), falloff 500..900, at
  (-99400, -1000, 31275), 375 in front of him at the height of his helmet. Like every record light it
  lights characters only, never the map, and nothing turns it on or off. The PC's character shader
  adds it unclamped (5 x 0.7 = 3.5, so the red channel saturates wherever falloff x N.L passes 0.29);
  `chrlight.c` does the same through `chr_light_range`. The port also adds a red rim light and a red
  haze behind him (`red_bars_glow`, its own, STATUS section 3).
* **The apartment stairwell** (stage 12). EvProgApartBoss (0x596220, program 5) plays the demo and then
  creates him with status 6 at (99846, -2203, -100532), facing -1.374 (0x8e1fe0). The port spawns him
  in `enemy_update` once flag 143 is set and flag 146 is not, so a save in that room brings him back.
  He walks at James (turn {0.5, 0.7, 1.0, 1.5} x 3 degrees a frame, walk rate {0.5, 0.6, 0.8, 1.0} x
  (endurance ratio x 0.75 + 0.25)) and swings: anime 3 within 900 and 10 degrees (attack 50: kind 1, ap
  40, frames 21..27, reach 600), anime 4 within 1500 and 7 degrees after three swings (attack 51, ap
  1000, frames 67..71: one of these kills James on any difficulty), anime 5 to the side within 500
  (attack 52, ap 25), and the shove within 500 and 30 degrees (attack 53, ap 10). A shove that lands
  lifts James and holds him up (attack 54, 0.2 every frame) until he struggles free (0x4c00a0). On the
  stairs he follows the PC's stair paths (state 3, 0x4bfb80): up by (98850, -99400) and (99200, -99000)
  when James is on the upper flight, down by (98850, -99400) and (100000, -101000). The fight lasts {20, 90, 180, 300} seconds (+0xac) or until his
  health {600, 2400, 4000, 5000} is gone; then he leaves (0x4c05e0): flag 144 sounds the siren (event 8,
  program 6), he walks down through the points at 0x893000 and flag 146 at the last one ends it (event
  9, program 7) and unlocks the door (event 0). The port removes him there itself instead of in program
  7, and gives up on a point it cannot reach after 8 seconds.
* **The hospital roof** (stage 21). EvProgTriheadEntry (0x58cea0, program 4, flag 201) creates him with
  status 15 at (18800, 0, -23600). He walks to (17800, 0, -23200) and raises flag 202 there, which runs
  the demo where he pushes James through the fence (program 3, flag 203). The port spawns him while 201
  is set and 203 is not.

Sounds: footsteps 16113 and the knife dragging 16100..16105 on walk frames 2 and 17, a groan
16118/16119 now and then (0x4bed70), 16107 when the blade meets something, 16110 when a shot hits him.

## Maria

`maria.c` follows the PC's follower code (0x529480..0x52df40; the PS2 beta calls the file
`m3_maria_sub.c` and the work `shMariaWork sh2mar`). She is with James while game flag 15 is set
(gGameFlag 0x8000; EvProgMariaMeeting and the bowling and hospital programs raise and clear it).

* **Placement** (MariaInitOnConnect 0x52d550): when a room loads, the appear point list at 0x6cd6d0
  (91 rows: previous room, new room, type, position, facing) says where she is. Type 3 follows James,
  0 waits at that spot, 1 rests (the hospital bed; the port leaves her out of sight), 2 waits until
  James walks on (room 8). Rooms not listed put her 500 units off James' right shoulder. Room ids are
  the PC's (0x494db0, `bgm_room_id`); a loaded save counts as coming from room -1.
* **Following** (MariaSetStraightRoot 0x52adc0): a trail of James' last 12 positions, one every half
  second he moves or at once when he turns a corner; she heads for James when nothing stands between
  them, else for the newest trail point she can see. She runs when 2000 away (speed up to 3.5 at 3 a
  second, 5.0 once she is 6000 behind), walks when 1000 away or out of sight (up to 1.4 at 2 a second),
  and stands otherwise. Speeds are x500 units/s; walking and running last at least 1.06 / 0.91 s.
  She turns 15 x 0.323 of the angle left each second (0x529d60).
* **Health** (0x53a170): 160 on beginner and easy, 80 on normal and hard. Enemy blows that catch her
  (the same reach and frames as on James) take their ap and play a hit motion by side (ids 20..23);
  after one she has two seconds of immunity. When hurt below half she catches her breath (tired, anime
  id 14) and regains half a point a second. At 0 she falls (id 24) and the game is over
  (`combat_dead()` reports it).

Motions used from `lll_mar.anm` (ids from 11): 11 stand (0-9), 12 walk (10-33), 13 run (34-57), 14 tired
(58-77), 20..23 hit (204-263), 24 falls (264-293). Footsteps 19031 (0x4a57), pain 19040, death 19041.
The enemies still only target James; in the PC they can pick Maria (0x542ad0).

## Results in the emulator (Azahar, New 3DS, `.enemytest/shots`)

* Guns: `h*.png` (handgun: lock-on, shot with flash, reload, Lying Figure health 976 to 707);
  `s*.png` (shotgun fired in ap06, a Lying Figure knocked down by it).
* Nurse: `s*.png` (ap06): she walks up, swings and hurts James (HP 93 to 46 over several blows).
* Creepers: `c*.png` (ap06): one bit James (HP 100 to 95), ran off, and died to a stomp (state 8).
  The bugs are very small on the top screen.
* Pyramid Head: `p*.png` (ap88, the stairwell, Normal): he walks in from the landing, swings from 750
  away and takes James from 100 to 60 to 20; handgun hits take his health from 4000 to 3640.
* Pyramid Head leaving: `q*.png`, `r*.png` (beginner): five handgun shots take his 600 health, the
  siren event (8) runs and he walks the points down the stairs; by frame 2850 he was at the last but
  one, so the end flag itself was not seen in that run.
* Maria: `m4_*.png` (cc07, the park, flag 15 set by debug.txt): she appears beside James, walks after
  him and keeps up when he runs off (status `M8.2/80`: room 8, walking, 80 health).

## Batch 3: the bosses and the rest of the cast

### Anime tables, a correction

Every kind plays its animes through a table of {anime id, loop} entries (`EnSCUAnime`, `EnREDAnime`
and so on); `FUN_004a9920(e, index, table[index].id)` starts one, and the number pushed to 0x4a77a0
is only the kind's "no anime" id, not a base. For the Lying Figure, Mannequin, nurse and creeper the
tables happen to run in id order, so nothing changed there. Pyramid Head's does not (index 2 is 5310,
3 is 5303, 4 is 5304, ...): batch 2 had his animes shifted by one from index 2 on, which is why
attack 51 looked unreachable. With the right table his long swing (anime 4, 5304, 120 frames) hits
in frames 67..71 like the attack list says. The window test (0x545680 into 0x544170) compares the
frame within the playing anime with atk_start / atk_end.

### Holds

Several attacks take hold of James (0x533d00 for 0x35, 0x3c, 0x3f, 0x41, 0x44): he is held while a
meter runs down, and every new button press or flick of the circle pad takes one frame time off it
(0x5565c0; on beginner it also drains by itself). The hold attack of the enemy (kind 6, ap 0.2 every
PC frame for most) drains him meanwhile. The port takes the PC frame as 1/30 s for both, so a meter of
1.0 asks for 30 presses and the hold costs 6 health a second. `combat_grab_james(meter)`, `combat_held()` and
`combat_hold_damage(ap, dt)` in combat.c do this. Meters: Pyramid Head's lift {0.35, 0.35, 0.7, 1.4},
the Abstract Daddy {0.5, 0.5, 1.0, 2.0}; the others reuse the latter (their column of the 5 x 5
table was not read).

### Scripted spawns

`SCRIPTS` in enemy.c lists the enemies the stage programs create after their demos, with the flag
that brings them and the flag that takes them away; `script_spawn()` runs every frame from
`enemy_update`, so a save made in the room brings them back:

| stage | flags | kind | from |
| --- | --- | --- | --- |
| 12 | 143 .. 146 | Pyramid Head, the stairwell (status 6) | EvProgApartBoss 0x596220 |
| 21 | 201 .. 203 | Pyramid Head on the roof (15) | EvProgTriheadEntry 0x58cea0 |
| 22 | 521 .. 210 | three Flesh Lips | EvProgLauraDeceive3rd 0x58b750 |
| 27 | 243 .. 248 | the chasing Pyramid Head (0x209, 15) | EvProgChaseInHospital1st 0x587db0 |
| 35 | 333 .. 334 | the Abstract Daddy (6) | EvProgAngelaAttackPapa 0x580e30 |
| 37 | 375 .. 373 / 377 .. 378 | Eddie, rounds one (0) and two (6) | EvProgEddieMurderer1st / 3rd |
| 44 | 2241 .. 483 | the two Pyramid Heads (0x209, 6) | EvProgTriheadBattle 0x576050 |
| 48..51 | 505 .. 506 / 507 .. 508 | Mary (0), then lying (13) | EvProgMaryBeforeBattle / MaryFall |

The bodies afterwards are Enemy_List rows whose conditions are now ported: 0x0c0 (flag 483, the
hotel Pyramid Heads) and 0x240 (flag 335, the Abstract Daddy), along with every other condition of
0x48f5b0 that reads game flags (0x040, 0x280, 0x2c0, 0x300, 0x400..0x600; two of them also compare
James' x with a constant, which is left out).

### The Abstract Daddy (0x204, chr/pap)

en_pap (EnLOCAL 8): control 0x4b7e90, automatic 0x4b7df0, init 0x4b6390. EnPAPAnime 0x892c58.

* The fight (status 6, type 1): health {900, 2500, 5000, 7000}, endurance {300, 400, 600, 800}, no
  spread; it starts in Chase and never gives up. Normal spawns (the labyrinth rows, condition 0x4c0
  = flag 395) have {500, 750, 1000, 2000} / {150, 285, 380, 500} and start walking straight.
* It sees only 10 degrees either side and 2500 away (0x4b56d0). Walk rate {0.7, 0.8, 1.0, 1.2} x
  (0.5 + 0.005 x health %), halved in the fight, x the facing factor; the walk moves 20.8 units a
  frame (feet 20 / 24). Turns {0.5, 0.8, 1.0, 2.0} x 1.5 degrees a frame, half in the dark.
* Lunge (attack 63: kind 2, ap 10, reach 400, frames 0..19) within 535 and 10 degrees, from behind
  when James has his back to it (anime 31) or else from the front (28). A lunge that lands holds
  James (attack 64, 0.2 a tick) until he struggles free, then it backs off at 500 a second.
* No flinch while its endurance is full and it is not attacking; otherwise a flinch, or a knock-down
  when the endurance is gone (or by a roll under 100). The fight's Daddy gets up at once (+100
  health). A handgun hit starts a five second rush at 1.25 x that shrugs off up to eight bullets at
  0.4 x (0x4b6f00).
* Health gone in the fight: the last move (falls at 1/8 speed), then flag 334 runs the after-fight
  program (event 4). Sounds: footsteps 18300, groans 18305..18312 at 0.4 every two seconds.

### The other Pyramid Head (0x209, chr/oni)

en_oni (EnLOCAL 5): control 0x4be7f0 (1 automatic 0x4be550, 4 event 0x4be740), init 0x4bbe50.
EnONIAnime 0x892e58. Attacks 55 (kind 3, ap 30, reach 700, frames 17..23 of the thrust) and 56
(kind 2, ap 50, frames 0..28 of each loop of the swings); the oni never hurt each other.

* The hospital chase (stage 27, status 15, event control): health 1e6, he only stalls when hit
  (180..600 ticks). He follows the 14-point route at 0x892e98 at 1.5 x walking pace and thrusts at
  whichever of James and Maria is nearer once one is within 750 (cool-down {1800, 960, 540, 120}
  ticks after a blow that lands). Behind James' back, or after a stage program called him on
  (FUN_004a8750 (4, n) from programs 2..5; the port reads their flags 244..247), he catches up at
  1500 a second. Program 6 (flag 248, EvProgMariaKilled) ends the chase with its demo.
* The hotel pair (stage 44, status 6, type 2): health {1200, 5000, 9000, 20000}, endurance {300, 500,
  1300, 1500}. Damage x1.5 while they attack and for the rifle, x5 for the great knife's heavy swings;
  what goes past 0 is taken from the other at x0.75. A spent endurance makes them flinch except to the
  handgun and the plank. A clock of {60, 180, 300} s (none on hard) ends the fight on its own. When both
  are done they walk to the spots at 0x892f7c / 0x892f8c, turn, and flag 483 runs EvProgTriheadDead.
  The port keeps the thrust and the run of swings but not the flanking and corner guarding (states
  2..4, 0x4bd230..0x4bdb50).
* The labyrinth ones (stages 34 / 35, status 0) wander, cannot be hurt and thrust when James is close.
  The status 13 rows are the hotel bodies.

### Eddie (0x205, chr/edi lll_edi)

en_edi (EnLOCAL 9): control 0x4b41b0, automatic 0x4b4100, init 0x4b0ed0. EnEDIAnime 0x892bb8.
Round one (status 0) has {100, 1800, 2500, 4000} health and ends when it is gone: he runs for the
door at (100000, -99100) and flag 373 takes him away (program 2). Round two (status 6) has {1000, 4000,
5000, 10000}; his death (anime 9) raises flag 374 after 90 ticks (program 4). Endurance {200, 300,
350, 500}. He walks (1000) and runs (1400) by velocity, shoots (attack 58: kind 3, ap 24, a ray 5000
long, six rounds and then a rest) when James is within 4000 and 30 degrees with a clear line, and
punches (attack 59: ap 15, frames 9..11) within 550. In round two he backs off when James comes within
1000. The cover and pillar runs of the PC (states 2 and 3, the points at 0x89294c and 0x8928c0) are
left out.

### Flesh Lips (0x203, chr/ike)

en_ike (EnLOCAL 7): control 0x4b9b70, automatic 0x4b9ae0, init 0x4b8700. EnIKEAnime 0x892ce0.
EvProgLauraDeceive3rd hangs three in room S3: two drop to y -1640 at once, the third waits at -3200
until the others are dead (or three minutes) and comes down at the drop point (0x892d38) farthest
from James. They glide on the ceiling rails at {0.5, 0.8, 1.0, 1.2} x 500 x (0.8 + 0.002 x health %);
touching James they grab him (attack 60: ap 7, frames 28..31) and hold him (61); within 1000 and 10
degrees on normal and up they kick (62: ap 17, frames 11..14). Health {500, 2000, 3000, 5000}. When
the last one has collapsed flag 210 runs EvProgHospitalBack1st. Their lift (state 8, which can end
James' life when he is carried up past y -4000) is folded into the hold.

### Mary (0x206, chr/bos)

en_bos (EnLOCAL 11): control 0x4b5540, automatic 0x4b5460, init 0x4b46f0. EnBOSAnime 0x892c10.
`mxx` (kind 0x10a) and `mry` (0x109) are demo models without AI. Health {1200, 4000, 8000, 20000}, no
spread. She hovers about 1500 above the floor (y -17500), springs up and down, dips when James swings
at her within 750 (0x4b44b0), turns to him at a degree a tick and flies at {300, 400, 500, 1500} units a
second toward a point 550 in front of him. Attacks: a swipe (0x43, ap 20, frames 7..9) within 600, a
lunging grab (0x41, ap 10, frames 11..14) from 400..700 when James has turned half away, then she
chokes him (0x42, 0.2 a tick) until he struggles free; and the moths, which the port keeps as an
invisible swarm point that settles on James (0x44) and holds him (0x45, 0.05 a tick). The handgun
never makes her flinch; the plank's and pipe's third hit counts x5. With no ammunition left the fight
ends on its own after 30 s (300 on hard, 0x4b42b0). At 0 health flag 506 runs EvProgMaryFall; the
lying Mary (status 13) needs one more hit, which heals James and raises flag 508 two seconds later.

### The Mandarin (0x20a, chr/arm)

en_arm (EnLOCAL 10): control 0x4ad0e0, automatic 0x4acff0, init 0x4ac770. EnARMAnime 0x892870.
It hangs under the floor and crawls on its arms (about 590 units a step), strikes within 1000
(attacks 0x46 / 0x47: kind 3, ap 10, reach 450 from the hand) and rests three seconds after a run of
strikes. Health {300, 800, 1200, 2500} with the usual spread. The PC lets it strike only when both
hands and the point ahead are over a grate (floor material 9 or 0xb, 0x4a72c0); the port's collision
files carry no floor materials yet, so it keeps within 2500 of its spawn point instead.

### Smaller things

* Regeneration (enDyingExec 0x4a96c0): Lying Figures, Mannequins, creepers, nurses and Mandarins
  spawned alive come back with chance {0.1, 0.1, 0.2, 0.4} the next time their room loads (half the
  Lying Figures come back crawling); the PC revives them once James has been 30000 away.
* Ambush (0x4c3a70 / 0x4c5370, hard only): Lying Figures in the buildings (glb_crd 9..13, type 6)
  wait beside doorways from the ambush_apart lists (0x8935f4) when James stands in one of the
  rectangles; they back into the spot when that is shorter.
* Path search (enSetPath 0x4aa960): a blocked way tries headings 15..75 degrees to either side and
  keeps the clear one for a moment, or turns about.
* Enemies go for whichever of James and Maria is closer (0x542ad0 with 0, called by 0x4a5b20).
* Maria: relaxes after 6 s of standing about (ids 16 / 18, and 17 to get going again), cowers (id 15)
  when an enemy is within 3000, and stands where the appear list's type 1 rows put her (the resting
  rooms; lll_mar.anm has no lying pose for the bed).

### Batch 3 in the emulator (`.enemytest/shots`)

* Abstract Daddy (`d*`, `d2*`, `d3*`, ps97, flags 332 / 333): it charged after the handgun hits
  (Rush), caught James with the lunge and held him (state 4.2, James "held") while its health went
  5000 to 4664. The test stands still and presses A about eight times a second, so James struggles
  free and is caught again by the next lunge until the holds wear him down.
* Mary (`y*`, `y2*`, ru73, flag 505): she hovers above the floor, lunges, catches and chokes James
  (the same pattern as above in a test that never walks away).
* The hotel pair (`h3*`, ru25, flag 2241): both spawn and thrust from 1250 away (the reach was then
  too short; it is now 1150 from the body).
* Eddie round one (`e*`, ps189, flag 375): he walks up and punched James to death with the plank in
  his hand; the punches now come in twos with a side-step away.
* The hospital chase (`o*`, hp230, flags 243 and 15): the oni walks the route and spears both James
  (100 to 40) and Maria (80 to 21), who cowers.
* Flesh Lips (`f*`, hp37, flag 521): two hang at working height over the room and kicked James to
  death; the kicks now have a breather.
* Mandarins (`g*`, ru37): two crawl under the grate corridor and struck James (90 to 80).
* Maria targeted (`t_*`, ap06 with flag 15): the nurse and the Lying Figure go for her when she is
  closer; she cowers (state 3) and her health falls 80, 73, 66, 59, 52.

## Batch 4: Maria as the player in Born From a Wish

The PC runs Born From a Wish (gChapterID 1, stages 53..58) on the same player code as James: the
base motion table (0x6ceab8, entries 101..133) is shared, so stand, walk, run, hurt, fall, kick and
stomp are the same frame ranges in `xmar_*.anm` as in `jms_*.anm`. Only the model, the weapon tables,
the health, the speeds and the sounds differ, so combat.c swaps them rather than adding a second
player:

* **Model.** `chr2/mar/lxx_mar.mdl` (36 bones). `combat_update` compares `prog_chapter()` with the
  model it has; on a change it frees main's `s_james` Character and loads `chr/xmar/lxx_mar` into the
  same struct (after `C3D_FrameSync`), so `draw_james` and everything else in main.c follow without a
  hook. Outside the chapter it loads `hll_jms` back. If Maria's files are missing it stays James.
* **Weapons.** Rows 10..12 of the weapon file table (0x88d2f0): `xmar_wpnone`, `xmar_wpcolt` with
  `wp_colt` (grip 9, item 10, ammo item 11) and `xmar_wpknif` with `wp_chinanife` (grip 10, item 17,
  the "cleaver" `s53_get_cleaver` hands out). Both weapon roots sit on Maria's bone 32 (James' hand is
  35). L and the item menu only offer the current character's weapons.
* **Revolver.** Table 0x6cf3cc: raise 744..756 (21103), aim end 771 (21105), shot 771..786 (21107),
  reload 792..831 (21109). Attack 26: ap 150, range 7500, no push (the 21108 shot from 757 uses
  attack 25, frames 14..25; not used). Shot SE 0x2b14 (0x543778, attacks 0x19 / 0x1a), click 0x2b16,
  reload 0x2b15 on frame 21 of 21109 (0x549500). ItemEventCheck (0x49e6a0) reloads it to 10 like the
  handgun. ItemInit (0x49e980) for chapter 1 gives the revolver with one round in it and makes it the
  drawn weapon (DAT_01f7d409 = 10); the port does this on the first update after `combat_reset`, and a
  continue (`combat_set_hp`) skips it. Marker item 253 keeps the empty revolver listed.
* **Knife.** Table 0x6cf454 (21201..21232) has the plank's layout; the follow-through (798 / 832) and
  the downswing (844 / 874) start two frames earlier. Attacks 28, 29, 30 (swing, short swing,
  downswing): ap 60, 35, 60, reach 250, no push, the plank's frame windows. Swing SE 0x428a with an
  effort 0x4286 / 0x4287 (0x549500).
* **Health.** PlayerInit (0x533ad0) sets hp and hp_max from gActionDifficulty and the chapter: James
  200 on beginner and easy, 100 on normal and hard; Maria 160 and 80 (the same values maria.c uses for
  the companion). `JAMES_HP_MAX` is now `combat_hp_max()`, so main.c's healing, overheal score, damage
  service and low-health cross follow it. This also gives James his 200 on the two easy levels, which
  the port had at 100.
* **Speeds.** Measured like James' from the planted foot in the same entries: Maria's strides are
  about two thirds of his walking and 0.6 running, so 650 and 930 units a second (James 975 and 1550)
  and 300 backwards (450). main.c reads them through `combat_ground_speed()`.
* **Sounds.** 0x549500 is Maria's counterpart of James' 0x549e20: dying 0x4a61 (19041), blows from
  0x4285..0x4289 (17029..17033).

Converting (`chr.py` reads `data/chr/<folder>`, so the chr2 files go through a folder of links):

```
S=$(mktemp -d); mkdir -p $S/chr/xmar
for f in lxx_mar.mdl xmar_wpnone.anm xmar_wpcolt.anm xmar_wpknif.anm; do
  ln -s "$PWD/Silent Hill 2/data/chr2/mar/$f" $S/chr/xmar/; done
for f in wp_colt.mdl wp_chinanife.mdl; do ln -s "$PWD/Silent Hill 2/data/chr2/wp/$f" $S/chr/xmar/; done
$PY tools/sh2conv/chr.py $S pack xmar lxx_mar.mdl xmar_wpnone.anm xmar_wpcolt.anm xmar_wpknif.anm
$PY tools/sh2conv/chr.py $S pack xmar wp_colt.mdl
$PY tools/sh2conv/chr.py $S pack xmar wp_chinanife.mdl
```

and copy `pack/chr/xmar` (192 KB model, three 899 KB animation sets, 21 KB and 12 KB weapons) to the
SD pack.

### Batch 4 in the emulator (`.enemytest/shots`, `m4*`)

Started with debug.txt `spawn th09 59622.3 0 20308.75 -90 2`, `stage 53`, `wish` and `flag 668 1`
(the opening program has run), on normal:

* `m4a`, `m4b`: with `wish` alone the "additional scenario" notice and the opening scene play with
  Maria drawn from `lxx_mar` in th09; the debug line shows HP 80.
* `m4e`: the revolver is drawn at the start with its one round; R raises it in both hands, A fires
  (mode `shot`). The walk this run asked for did not happen: it held the D-pad, which does not move
  the player (batch 6 walks her with the circle pad).
* `m4f`: with `item 17 1`, L switches to the knife; R holds it over her shoulder and A swings it
  across (the EE tap / hold timing as for the plank), then the follow-through.
* `m4i`: a Lying Figure (`enemy 200 60200 20308 -90`) walks up; the first shot (with a muzzle flash)
  takes 150 off its 681, A on the empty revolver reloads from `item 11 20`, two more shots put it on
  the floor (hp 0) and the next A stomps it. Its acid took Maria from 80 to 73.
* `m4h`: the same figure further back, behind the door frame: the shots stop at the wall.
* `m4j`: James in the labyrinth with the Abstract Daddy (chapter 0) is unchanged: his model, the
  plank and full health 100 on normal.

## Batch 5: ammo, the boss details and Maria's steps

### Ammo pickups

ItemGet (0x49e310) adds one box per pickup: a health item and the revolver 1, the handgun and its
bullets 10, the shotgun and its shells 6, the rifle and its shells 4, the hyper spray 8. Only the four
ammo boxes (items 5, 7, 9, 11) are multiplied, by 0x1dbec05, which is not the difficulty: it is the
Extra Options "Bullet Adjust" (the options page 0x4630c0 cycles it 1..3, the default comes from the
saved options). The difficulty only decides which pickups exist (EventItemConditionCheck 0x48cb20
leaves out the ones marked 2, 3 and 6 on easy, 1, 3 and 5 on normal and 1, 2 and 4 on hard). `events.c` now has `ev_item_get_amount()` (in place of
`pickup_amount`, which gave the revolver box 6) and `ev_set_bullet_adjust()`; main.c keeps the
option in options.txt as `bulletadjust`. prog.c's `p_item_get_now` has its own copy of the table
without the multiplier and could call `ev_item_get_amount` instead.

### Mary's moths

They are drawn now. Mary releases 33 (0x4b4ec0 case 0), each a camera-facing quad 100 units across
with the effect texture 0x12 (0x4a5270), which is file id 0x52f3, `data/pic/effect/fly1.tbn2`: a 64 x 64 DXT1 sheet of four
wing beats, picked by the moth's frame byte (u = (frame & 1) / 2, v = (frame / 2) / 2, 0x4a46f0). The
frame runs at 0x400 a tick over a 0x2580 cycle (0x4ac070). `tools/sh2conv/moths.py` builds `pack/chr/moth/moth.s3c`: four unit quads on four bones, one per frame, so
enemy.c draws a moth by giving its frame's bone the billboard matrix and collapsing the other three.
The swarm keeps the PC's states: chest height (y -16800) while it hunts, on James while it holds him,
and up and away (2.5 s) once it lets go or Mary is hit out of the state.

### The Flesh Lips' lift

When a Flesh Lip's hold kills James (DAT_01fbad17 3) it goes to state 8 (0x4b7ff0) instead of letting go:
it turns square to the room (yaw 0 or pi, 0.5 degrees a tick), slides along its rail to z -21450 at 500
a second and rises at 500 until it is above y -4000, and only then is the game over (0x4a63b0 sets 2).
James' body hangs from it: `combat_carry_james()` holds him in the hurt pose at its position and
`combat_dead()` waits for the lift.

### The hotel pair's teamwork

Once a second (0x4bcff0) the one farther from James, while the other chases or attacks, either flanks or,
with James in a corner, guards it (the corner test 0x4bba10: outside x -21800..-18200 and outside
z 19800..24200).

* Flank (state 2, 0x4bd230): the point on James' far side from the partner, 40 degrees off the line,
  as far from him as it is (at most 2500), chosen again every second; more than 100 degrees round from
  the partner it closes in to 500 behind him and waits facing him. It thrusts, or goes into the swings
  when the partner is over 750 away, and drops back to the chase beyond 4000 or once it is 150 nearer
  than the partner.
* Corner (state 3, 0x4bd780): the corner James is in (x -17200 / -22800, z 25200 / 18800), and of its two
  walls the one away from the partner; it waits 1000 out along it and thrusts within 1250.

### Eddie's cover

Round two now takes cover (state 2, 0x4b2270, chooser 0x4b1640): eleven crates and carcasses in the
freezer (0x89294c). The spot is 500 behind one as seen from James, at least 2000 from him, out of his
sight and not among the last three; he runs there, waits up to 600 ticks facing James, shoots when he
has him in view and moves on when James comes within 1500, or closes in to punch within 1000.

### Creeper nests

Kinds 0x20c and 0x20d (`chr/item/ty2.mdl`, `ty3.mdl`; EnLOCAL 13 and 14, controls 0x4b0250 / 0x4b02e0)
are masses of creepers, three and six of them in stage 30 (ps39), where five creepers wait as well;
`ENEMY_MAX` goes from 8 to 24 for that room (the PC has 32 slots). They cannot be killed: each blow that
lands releases one creeper at the spot, stunned at first (state 7), while there are fewer than twelve
(0x4add40 / 0x4ae020). Convert with
`$PY tools/sh2conv/chr.py "$D" pack item ty2.mdl` and `... item ty3.mdl` (no animation; enemy.c draws them in
their bind pose).

### The Mandarin's grates

The floor materials were there all along in the footstep floors (`sound/floor/<map>.sfm`, written by
`tools/sh2conv/sound.py` from the .cld files), so collision.py and S3L stay as they are. The Mandarin
strikes only when both hands (550 out) and the point 500 ahead are over material 9 or 0xb (0x4a72c0,
0x4ac9d0), and its hand comes down with 12250 on a grate and 12251 elsewhere (0x4acab0). In ru37 the
grate corridor is material 9 and the rest 0.

### Maria's footsteps and the knife

Her steps come from her own function (0x549150): both feet on frames 5 and 17 of her walk and run, at
0.5 and 0.8, with her own sound per floor material (19031 default, 19032 grass, 19033..19038 by
material). combat.c plays them from her animation and main.c leaves out James' clip-timed ones while she
is the player. The knife's hit sound is 17035 at 0.8 (0x5350d0, which plays the plank's 11032 the same
way).

### Batch 5 in the emulator (`.enemytest/shots`)

* Mary's moths (`y4b*`, ru73 with flag 505, James 3900 from her): at 400 the swarm leaves her, at 500 the
  moths fly at James in their wing beats, from 600 they cover his head and hold him (state 2.2, his
  health 96, 94, ... 77 as the test never struggles).
* The Flesh Lips' lift (`f5b*`, hp37 with flag 521, James standing still): the hold takes him to 0, the
  lifter (state 8) carries him through the wall to z -21522 and up (his y 0, -403, -1161, -2171, -2357),
  and only then is he dead (1890) and the game over.
* The hotel pair (`h5a*`, ru25 with flag 2241): one chases while the other flanks (state 7.2, closing in
  from behind); together they killed James by frame 700.
* Creeper nests (`n5g*`, ps39 on beginner, the handgun from 400 away): each shot that lands on a nest
  releases a creeper, the room's count going 14, 15, 16, 17.
* Not seen in the emulator: Eddie's cover (round two needs the freezer's program), the Mandarin's grate
  test and Maria's footsteps (sounds); they build and run through the same paths as the rest.

## Batch 6: checks in the emulator, the last simplifications, a balance check

### Seen working now

* Eddie, round two (`e6b*`, ps193 on beginner with flag 377, stage 37): James' handgun takes him
  from 1000 to 280 (880, 640, 400, 280). On normal (`e6a*`, health 5000) he only walks up and shoots
  (states 0 and 1): the PC takes cover only after a shot once his health is under 30 % (0x4b3010
  case 3: `+0x144 < 30` goes to state 2, otherwise state 4), and the port now does the same instead
  of hiding whenever he is not shooting. Under 30 % he looks for cover, finds none the port counts
  as hidden (the meat in the freezer is not in the collision walls), and runs the pillars instead
  (state 7.1 here, from 1200 on).
* The Mandarin's grates (ru37): with James on the grate (x -23700, material 9) and the Mandarin 800
  away (`g6a*`) it strikes over and over (states 0.3, anime 4 / 5) and takes James from 80 to 10;
  with James off the grate (x -25300, material 0, `g6b*`) it crawls up to 370 from him and never
  strikes (state 0.2, James at 100 the whole time).
* Maria's footsteps (`w6c.wav`, th09, the circle pad held up for 250 frames, material 0):
  `tools/stepmatch.py` (a high-passed normalised cross-correlation against every
  sample in se.bnk) finds 19031, her default step, seven times 0.57 s apart (7.70 .. 11.16 s, two
  steps per 1.16 s walk cycle), and none of 19032..19038 or James' 11000..11005. An earlier capture
  held the D-pad, which does not move the player; batch 4's "Maria walks" result had the same flaw.

### The last simplifications

* The hotel pair's stand-off (state 4, 0x4bdb50): after a thrust that leaves James in reach (0x4bdea0)
  the oni stays put: it attacks again while he stays in reach, holds its ground facing him while it
  is more than 100 degrees round from its partner, and otherwise flanks (or guards the corner).
  Beyond 2500 it gives chase. (`h6a*` shows the flank, the thrust and the swings; James died before
  the HUD's nearest oni showed state 9.)
* Eddie's pillar runs (state 3, 0x4b2700): with no cover he runs to the nearest of the six pillars at
  0x8928c0 he has not used lately, waits {3, 4, 5, 6} s facing James, shoots when he has him in view
  and leans out sideways (anime 4 / 5, past 50 degrees) up to three times when he does not; on
  beginner and easy he looks for cover again at once. James within 1000 makes him close in.
* Wall and ceiling nests (`n6a*`): a status 1 / 3 nest is lined up with the nearest wall segment at
  its height (0x4af260 -> 0x4ae7d0) and drawn standing on it, facing into the room; a status 4 nest is
  turned over at its own height (0x4af2c0). In ps39 the six ty3 masses now cover the walls.

### Balance on normal (the PC's tables, as the port uses them)

| fight | the PC's numbers | in the emulator (`bp*`, `bh*`, `bn*`, `bph*`, ru25 / ap88) |
| --- | --- | --- |
| Lying Figure, plank | health 700 (x 0.5 .. 2), endurance 285 recovering 30 a second; swing 100, short swing 80, downswing 140; a stomp 9999.9 | health 681; each short swing (A pressed again during a swing) took 80 (681, 601, 521, 441, 361, 281, 201 in 15 s). At one hit about every 1.2 s the 30 a second recovery kept it on its feet; James lost 21 to three acid sprays (ap 7). |
| Lying Figure, handgun | the shot 120 (attack 2) | 681, 561, 441, 321, 201, 81: down after the fifth shot, dead at the sixth; James lost 7 |
| Nurse, handgun | health 800, endurance 285; her blows 20 / 15 (attacks 46 / 47) | health 778; seven shots of 120 (778, 658, .. 58, 0), she never reached James |
| Pyramid Head, stairwell (stage 12, flag 143) | cannot be killed; 40 (attack 50), 1000 (51, the long swing), 25 (52), 10 (53) | James 100 to 60 (attack 50) within 8 s, then to 0 with the long swing about three seconds later |

The PC applies an attack's power to James' health as it is (the damage step, like Maria's at
0x52d280, only scales friendly fire by 0.1); difficulty changes the health on both sides (James 200 /
100, the enemy tables by level), which is what the port does.

## Batch 7: creepers on the walls and ceiling, the hotel pair taking turns

### Creeper climbing (states 4 / 5, 0x4aff00 / 0x4b00f0)

* A creeper listed on a wall (status 1 or 3) holds on to the nearest wall segment at its listed
  height and runs about on it (state 4) with its belly to the wall; it heads straight down when it is
  more than 2000 above James, and a status 3 one steps off onto the floor once it is within 100 of it
  (100 out from the wall, facing away) and goes on as a floor creeper. A status 4 one runs about upside
  down on the ceiling at its height (state 5). Stomps do not reach them up there.
* A blow knocks a climbing creeper off (state 7 sub 0xff / 0xfe): it falls at 1500 a second, lands on
  its back and is stunned as after any blow, a floor creeper from then on. Creepers released by a wall
  or ceiling nest start that way too (the PC gives the new creeper the nest's status and state 7 sub
  0xff), so they now fall from the nest instead of appearing on the floor.
* The debug.txt `enemy` line takes an optional height after the status (`enemy 202 x z deg 1 -1200`),
  so a wall or ceiling creeper can be placed for a test.

### A shot bug found on the way

`fire()` in combat.c stopped every shot where `col_clamp_ray` stopped, and that camera helper ends
8 % of the segment before the wall, which for a 7500-unit shot is 600 units. Anything within about
600 of the wall behind it could not be shot: wall creepers, and also enemies standing against a wall.
The shot now adds that 8 % back (plus the 40 it had).

### The hotel pair take turns (0x4bc6b0)

The PC's attack decision lets an oni start an attack only while its partner is not in its thrust
(state 6 up to sub 1), its run of swings (7) or the thrust from a corner (3 sub 4). The port had no
such rule, so the stand-off (state 4) lasted one frame before the next thrust; now the one waiting
holds its ground facing James while the other attacks. The debug line shows both of the pair's
states (`s<near>|s<far>` and their distances) when the nearest enemy is one of them.

### In the emulator (`.enemytest/shots`)

* `c7c*` (ma77, stage 57, beginner, two debug creepers at -1200 and -2600 plus the list's two at -600
  and -800): the nearest is in state 4 throughout with its distance changing as it runs; the
  creepers are seen at different heights on both corridor walls (frames 400..500). A temporary log
  (not in the hand-back) showed all four placed on the walls at their heights and the status 3 one
  stepping off at (58850, 0, -97187).
* `c7i*` (same room, shooting): frames 556..586 show the hit creeper falling (state 7.10); the log
  had three creepers knocked off, falling about 250 units every 8 frames and landing on the floor.
* `h7a*` (ru25, flag 2241): frames 460..520 show `s3.1|s9.1`: the near oni swings (state 3) while the
  far one stands off (state 9) at 1247, then attacks in its turn.
* `c7d*`: a debug ceiling creeper (status 4 at -1400) shows as state 5 on the debug line while it
  runs off along the ceiling.
* Not seen: a ceiling creeper falling (the one placed ran out of the shot's reach), and a released
  creeper's fall from a wall nest (in ps39 a floor nest stays nearest on the debug line).

## Batch 8: creepers round the corners, with the PC's probe

* On a wall a creeper now walks the room's wall segments as one surface: past a segment's end it goes
  on along a segment that meets it there and spans its height. At an inside corner (the next wall
  turns towards the creeper's side) it faces back the way it came; at an outside corner (a pillar)
  it wraps round onto the other face. The heading keeps going away from the corner.
* Its heading follows 0x4ad3a0: a probe walks 500 ahead along the surface in 50-unit steps (the top
  of the wall, the floor and an end with no wall beyond are obstacles). While clear it keeps its
  heading; blocked at d it tries 15 degrees to either side (probing d + 50), widening by 15 up to 75,
  and turns right round when none is clear. It turns 5 degrees a tick towards the new heading while
  the obstacle is 250 or more away, at once when nearer, and within 100 of it heads straight down
  (up when the obstacle is the floor). More than 2000 above James, or on its way down (status 3),
  it heads down.
* On the ceiling (0x4b00f0) it turns by up to 60 degrees at 0.01 a tick, and when the point 500 ahead
  has no floor under it or a wall on the way it turns 15 degrees at a time until it has.

In the emulator (a temporary log, removed again, and `.enemytest/shots`):

* `m8a*` (ma77): the log had creepers crossing from segment 19 to 18, 17 and 12, and from 21 to 22,
  back to 21 and on to 24, the inward bearing changing with each face. The corridor is too dark for
  the screenshots to show the creepers well.
* `m8b444` .. `m8b512` (ru25): a ceiling creeper (state 5) shot by the handgun falls (state 7.10)
  from y -1300 to the floor.
* `m8f612` .. `m8f640` (ru25, a debug wall nest above the door): the shot nest releases a creeper
  that falls (state 7.10) from y -900 to the floor (y -17).

## Batch C3: the steel pipe, the great knife, the chainsaw and the hyper spray

James now has every weapon of the PC (combat.c). The models and James' moves for them come from
`wp_pipe` / `wp_nata` / `wp_csaw` / `wp_sp` and `jms_wppipe` / `jms_wpnata` / `jms_wpcsaw` /
`jms_wpsp.anm` (added to make_pack.sh's characters step; the `rwp_` variants are not used, as for the
other weapons). The stage programs already hand them out (prog_town.c, prog_prison.c): the pipe
from the car in stage 14, the great knife in stage 34, the chainsaw at the grave and the spray in
stage 2 on later playthroughs. They are items 14, 15, 16 and 12 (0x534240), so they show in the
item list and on the touch screen with their own names and models, and "use" equips them.

Each melee weapon's clips are its row of the PC tables (0x52fae0 / 0x531a00 with the tables at
0x6cf04c, 0x6cf1dc, 0x6cf2ac), its attacks the PC's (sh2_attack_list 0x6ec568):

| Weapon (grip) | Light press | Hard press | Notes |
| --- | --- | --- | --- |
| Steel pipe (6) | lunge 429 / 431, attack 18 (130, reach 540, frames 8..10) | overhead 421 / 425, attack 17 (160, frames 15..17) | on hard with run held the swing 409 / 415 (15, 120, frames 10..12) and its follow-up 411 / 417 (16, 96, frames 4..6) instead of the lunge (0x533ff0); a swing that connects plays back; hit 11040 |
| Great knife (8) | swing 461 / 459, attacks 19 / 20 by shoulder (600, reach 725) | heavy 465 / 463, attacks 21 / 22 (1000) | one swing from each shoulder in turn, no follow-up; swing sound 11039 on frames 8 / 11 and 17 / 18; dragged at the speeds of 0x54b3b0, no running, slower turns; hit 11038 |
| Chainsaw (7) | saw 567 / 565, attack 23 (250, reach 525, frames 12..27) | thrust 571 / 569, attack 24 (300, frames 6..13) | pressed again while sawing: the thrust; the engine revs (11044) on raising and runs (11045) in each cut, stops when lowered; no fuel; cut 11046 |
| Hyper spray (4) | spray 508, attack 8 (frames 8..25), 509 (attack 9) while the button stays down | | 20 seconds a can, used up while it sprays; costs James a point of health a second on normal and hard; empty, the can is gone (0x545260); hiss 11047 |

The swing sound and James' effort (11018 / 11019) play on the clip's frame from 0x549e20. A body
on the floor gets a kick or stomp (with each weapon's kick type, 0x53b190) instead of a swing, as on
the PC; the plank keeps its downswing there. The spray (attack kind 7) hurts nobody: it puts Lying
Figures, Mannequins and nurses in their "confuse" state, where the nurse stands dazed (0x4badc0)
and the others back off. Its mist (0x4a4c00) is drawn since batch C4.

In the emulator (`.enemytest/shots/ctl`, debug.txt `weapon <item>` equips, `god 1` keeps the
enemies' blows off James):

* `w7_680` .. `w7_722`: the pipe's overhead takes a Lying Figure from 681 to 521 (160) and knocks
  it down; the lunge before it fell short.
* `w11_598` .. `w11_662`: the great knife's light swing takes a nurse from 778 to 178 (600) and
  knocks her down; held A over her body then stomps her (0).
* `w12_566` .. `w12_846`: the chainsaw saws a nurse from 778 to 528 (250), the thrust takes her to
  228 (300).
* `w13`: the spray puts a nurse in state 5 (dazed) with no damage; James goes from 100 to 98 while
  spraying (normal).
* `dr1`: with the great knife in hand and B held James still walks, at the drag's changing pace
  (1.2, 0.6, 0.25 m/s on the debug page).
* Not seen: the pickups themselves. The pipe's car event (stage 14, cc41 event 12, a look at its two
  segments) did not fire from a debug spawn at the car in four tries, so it may need state that a
  spawn does not set; the others were not tried.

## Batch C4: the pickups in normal play, and the spray's mist

All four weapon pickups fire from a normal approach. Nothing in the game state was missing for
the pipe: the look test (events.c `look_segment`) only passes on the side a segment faces, with
cross(B - A, P - A) >= 0, and every C3 debug spawn stood on the far side of the car. Event 12's
segments, (-70704, 62170) -> (-70141, 61844) -> (-70383, 61424), face +x and +z, toward the pump,
so James has to walk up to the car from that side and look at it. Under each pickup:

| Weapon | Event | Gate | Where James stands | Seen |
| --- | --- | --- | --- | --- |
| Steel pipe (14) | stage 14 event 12, s14 #20 | flag 168 clear (set by taking it); needs light | on the car's +x / +z side, facing it, e.g. (-70250, 62250) facing 180 | a playtest walk from `c04.sav` gets "I got a steel pipe." |
| Great knife (15) | stage 34 event 4, s34 #2 | flag 316 clear | in front of (21369, -100063) -> (20905, -100244) -> (20615, -99490), e.g. (21228, -100386) facing -21 | the playthrough's own c08 run, and a spawn |
| Chainsaw (16) | stage 4 event 20, s04 #5 | flag 12 set, flag 38 clear | on the -z side of the grave's segment (-61100, -850) -> (-61450, -850), e.g. (-61275, -1100) facing 0 | a spawn with `flag 12 1` |
| Hyper spray (12) | stage 2 event 8, s02 #2 | flag 631 set, flag 632 clear | on the -x side of (10155, 9850) -> (10155, 10149) in the trailer (er05), e.g. (9905, 10000) facing 90 | a spawn with `flag 631 1` (and `flag 32 1`, or the prologue plays first) |

The chainsaw and the spray only need their unlock flags, which FlagInit (prog.c `prog_new_game`)
sets from the clear data on a New Game after a clear:

* flag 12 (chainsaw): any of the Leave, In Water or Maria endings seen before.
* flag 631 (hyper spray): a game cleared on hard action level (clear bit 0x80), or on normal (0x40)
  with two clears or more, or three clears of any kind.

So a clear save whose clear data has those bits, loaded from the title, lines both up.

### The spray's mist (mist.c)

The PC's puff (effect 0x1f): 0x4a4c00 spawns it, 0x4a3f60 moves it, 0x4a4170 draws it. The spray
(0x545260) posts one each 60 Hz tick from the can toward the end of its reach plus 150, with the
life argument 12:

* The vector gets up to 50 of jitter on each axis (0x4489a0, small values far likelier); its length
  is the puff's speed, and it is then normalised.
* Phase 0, ticks 0..3 (ftol(0.3 * 12)): 0.3 of the speed of travel; alpha 0 -> 32, size 0 -> 0.2.
* Phase 1, ticks 3..7 (ftol(0.6 * 12)): another 0.3 of the speed every 3 ticks, sinking 100 a second;
  alpha 32, size 0.2 * (1 + f).
* Phase 2, ticks 7..14: 0.5 of the speed over 7 ticks, still sinking; alpha 32 -> 0, size
  0.4 -> 0.8. Then it is freed, so about 14 puffs are out while James sprays.

Each puff is a camera-facing disc of radius speed * size * 0.25: a 16-point fan whose centre
carries the alpha (vertex alpha * a / 64) and whose rim has none. Its texture is effect texture 2
(0x4c9b20(2), file id 0x2c1a, data/pic/effect/poison.tbn2, a cloud in the alpha), sampled around a
random point with radius 0.125. The colour is 0xffc0c0c0 with its bytes scaled by 1, 144/128 and
160/128, which gives (192, 216, 240): a pale blue-white. ui.py's `write_mist` converts the texture's
alpha to pack/effect/mist.t3x (A8, 128 x 128); mist.c draws it with ordinary alpha blending and keeps
only the texture's alpha, the PC's tint standing in for its brown RGB.

The PC changes the spray's colour by the rank of the last clear (0x1dbeb89, set by 0x539dd0 from
the 0..100 score of 0x539bf0): 20 or less 0xff400040, under 80 the grey above, under 100 0xff40c0c0,
100 0xff40ff40. The port has no ranking yet, so it stays grey; `mist_set_color` takes the tier once
it does.

In the emulator:

* `ptr` (`.enemytest/shots/ctl/ptr_ptlog.txt`): `pt walkmode 1`, `pt load c04`, `pt ev 12` walks
  James from cc11 to the car and the pipe is his at 255.7 s ("I got a steel pipe.", flag 168,
  item 14 0 -> 1, the expect passes).
* `pk9b_560` .. `pk9b_790`: the spray's program runs in the trailer and the can is gone from the
  table.
* `mi1_600` .. `mi1_700`: spraying a nurse at cb47, the cloud leaves the can and spreads past her;
  the debug page counts 14 puffs.
* `mi3_660` .. `mi3_780`: the same on the tree with ws5's frame-rate work. The press has to come
  after the can is up: in `mi2` A went down while James was still raising it, so he only aimed.

## Testing keys (debug.txt, read by enemy_init)

* `action <0..3>`: action level (main.c reads the same key for new games; 0 beginner: one hit knocks a
  figure down and it never gets up).
* `weapon <item>` (main.c): equips that weapon item on the first free frame; `god 1`: enemy blows
  and grabs do not reach James.
* `enemy <kind hex> <x> <z> <yaw degrees> [status]`: an extra enemy, spawned in whichever room has a
  floor under (x, z), e.g. `enemy 200 -50000 -63000 0` puts a Lying Figure in the ap06 corridor.
* `enemylog 1`: `enemylog.txt` on the SD card, a line per enemy per frame (kind, state.sub-state,
  anime and frame, health, endurance, pending damage, distance, position) and one for James (his
  combat status and the keys held, with those pressed this frame shifted up 16 bits).

## main.c hooks

Batch 1 (merged): includes, `enemy_room_enter` in `load_current`, `combat_reset` in `new_game` /
`continue_game`, `combat_james_anim` / `combat_draw_weapon` / `enemy_draw` in `draw_james`, init and exit,
`combat_update` before `player_update` / `events_update`, `enemy_update` unless a program holds James,
`combat_dead()` back to the title.

Batch 2: `.enemytest/main_hooks.diff`, made by `.enemytest/apply_hooks.py main.c.orig out.c` (text
anchors, against the main.c of 2026-10-17 08:26 with the pause menu, Born from a Wish and the
toggle-run option):

* the controls comment and two help lines (`R + A, L`, and the weapon with its rounds)
* the item list shows owned guns with an empty clip (`combat_owns_item`) and skips the marker items
  250 and up; "use" on a weapon equips it (`combat_equip_item`)
* `#include "maria.h"` and `prog_score_frame(..., maria_distance())` for the ending score

Batch 3: `.enemytest/main_hooks.diff` from `.enemytest/apply_hooks.py` (batch 2's script is kept as
`apply_hooks_b2.py`): in the streamed open areas, `enemy_stream_update(ev_stage(), &s_col)` when
`stream_update()` reports that the touching tiles changed. It drops the enemies whose floor left the
merged collision (they come back from the list when their tile returns, unless they were killed) and
spawns the rows whose tile came in; the ones that stay keep going. Maria follows James across tiles
and needs nothing.

Batch 4: `.enemytest/main_hooks.diff` from `.enemytest/apply_hooks.py` (batch 3's script is kept as
`apply_hooks_b3.py`): `player_update` takes its ground speeds from `combat_ground_speed()` instead of
the constants 975 / 1550 / 450. The player model swap needs no hook (see batch 4 above).

Batch 5: `.enemytest/main_hooks.diff` (paths `port/source/main.c`) from `.enemytest/apply_hooks.py`
(batch 4's script is kept as `apply_hooks_b4.py`): `player_update` skips `sfx_footsteps` while
`combat_own_footsteps()` (Maria's steps come from combat.c), and `options_load` / `options_save` keep
`bulletadjust` (`ev_set_bullet_adjust` / `ev_bullet_adjust`).

Batch C4 (`.enemytest/c4/apply_main.py main.c`): `mist_init` after `blood_init`, `mist_clear` after
`blood_clear` on a room load, `mist_update` after `blood_update`, `mist_draw` after `blood_draw`, and the
debug page's blood line gets the puff count.

Everything else goes through the existing calls: Maria is updated and drawn from `enemy_update` /
`enemy_draw`, placed from `enemy_room_enter`, reset in `combat_reset`, and her death shows up in
`combat_dead()`. Kills call `prog_stat_kill(attack id)` where the killed bit is set (the results
screen counts shot and beaten kills apart, and the ending score counts both).

## Not done yet

* The PC frame rate is assumed to be 30 for the holds (the drain per check and the struggle meter in
  frame times); the escape takes about as many presses as the PC asks for at 30 frames a second.
* Eddie's choice of the deepest cover (0x4aa800; the port takes the nearest spot out of James' sight,
  and in the freezer the carcasses are not in the collision, so he mostly runs the pillars instead);
  the moths fly on fixed orbits round the swarm point instead of steering one by one (0x4ab780) and do
  not leave by the room's three exits.
* Creeper climbing works on the collision's vertical wall segments, so a creeper crosses inside and
  outside corners but cannot climb from a wall onto the ceiling or onto sloped faces (the S3L has no
  ceilings); the ceiling creeper keeps its height. The oni's hyper spray reaction.
* Maria: alert / discover (she looks at enemies and points), the one-step shuffle, James' friendly
  fire (0.1 x in 0x52d280); type 1 (the resting rooms) shows her standing, since lll_mar.anm has no
  lying pose; the appear point list only fits rooms the port loads whole (a spot in another piece of
  the town falls back to James' shoulder).
* Maria is not drawn while the world is stopped (a program holding James, the pause menu), so the
  demos do not show her twice; this also hides her behind the pause menu.
* Born from a Wish: Maria's hurt voice is not chosen by damage state, and her ground speeds are
  measured, not read from a PC table. (Bullet Adjust has its row on the Options' Game tab after a clear, as the PC's Extra Options.)
* Balance: the stomp's ap 9999.9 finishes any body on the floor, and Pyramid Head's long swing (ap 1000)
  kills James outright, as in the PC tables.
* Sounds without a confirmed label: the creeper's pain (12208..12213), Pyramid Head's footsteps
  (16113; the PC plays 0x4978 or 0x3e94, which are not in the sound index), the boss hit sounds
  (18301, 18863, 12255) and the Flesh Lips' creaks and gasps (18512.., 18504..).
