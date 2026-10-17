# Silent Hill 2 PC event tables (doors, triggers, room transitions)

This note documents what `events.py` decodes and where each piece of knowledge comes from. All of
it is static analysis of `sh2pc.exe` (Director's Cut, image base 0x400000), cross-checked against
the PS2 beta debug info (struct and function names) and against the game's own collision files.

```
.venv/bin/python -I tools/sh2conv/events.py            # writes pack/events/events.json + stage_NN.json
.venv/bin/python -I tools/sh2conv/events.py --no-rooms # skip the .cld based room tagging
```

`events.json` holds every stage plus a flat `door_links` list (one entry per door or warp event,
with the trigger shape, the requirements and the destination). Each `stage_NN.json` holds one
stage: its events, items, models, enemies and its own `door_links`. `sh2rooms.py` is a helper with
the game's position to room-id table.

## Data layout

`Stage_Data` (68 bytes, PS2 DWARF, layout confirmed on PC) starts with `ev_list`, `ev_pos`,
`ev_prog`, `gi_list`, `mdl_list`, `en_list`; `glb_crd` sits at +36. The current stage pointer is
`DAT_01f7d550` (PS2 `stage`), refreshed from the current stage id in byte 0 of `DAT_01dbeb80` by
0x4955e0 -> 0x495370 (`StgOverlayGetStageData`).

### Event_List (16 bytes: flag, cond, rslt0, rslt1)

All four words are bitfields. The PC build keeps the compiler-generated accessors at
0x48b781..0x48b847, which give the field boundaries directly; the readers in EventCheck and the
exec functions confirm the meaning.

| word | bits | meaning |
|---|---|---|
| flag | 31 / 15 | polarity of the high / low flag test |
| flag | 30 / 14 | "update" bit for the high / low flag |
| flag | 29..16 / 13..0 | high / low game flag number (0 = unused) |
| cond | 31..28 | trigger kind (table below) |
| cond | 27..16 | byte offset of the trigger record in `ev_pos` |
| cond | 15..12 | trigger record type (shape, see ev_pos) |
| cond | 11..4 | item number for the item kinds 5..10 (index into the possession bits at 0x1f7d3e0) |
| cond | 3 | consume the item when the event fires (calls 0x49e4e0, which clears the item) |
| cond | 2 | needs light: skipped when the room is dark and the flashlight is not usable |
| cond | 1..0 | unused (always 0 in the PC data) |
| rslt0 | 31..28 | result type (table below) |
| rslt0 | 27..16 | byte offset of the destination record in `ev_pos` (doors) |
| rslt0 | 15..12 | destination record type (doors) |
| rslt0 | 11..5 | destination stage id, 0 = stay in this stage |
| rslt0 | 4 | camera flag, copied to `DAT_00934d94` bit 2 and passed to the camera reset after the warp (0x518ca0) |
| rslt0 | 3..0 | unused |
| rslt1 | 27..22 | door sound set, index into `door_se[22]` at 0x88cb70 ({open, close, unlock, jam, lock}) |
| rslt1 | 21..14 | message number (8 bits) or program number (7 bits, 20..14) or save point id |
| rslt1 | 13..0 | game flag set when the event runs (0 = none); for result types 1/2 the flag to set or clear |

The list ends at the first entry with `cond == 0 && rslt0 == 0`. EventCheck scans it in order and
the first entry whose conditions all pass wins, which is how one door can have a "locked" entry
after its "open" entry: the open entry requires the unlock flag, so the locked one only runs
while that flag is clear.

Flag test (EventCheck 0x48e520): for each half with a nonzero flag number the event is skipped
unless `game_flag[n] == polarity`. Game flags are the bit array at 0x1f7d5e0 (`gGameFlag`),
flag n is word n>>5, bit n&31. When the event runs, 0x48bee0 (`EventExecSubFlagSet`) writes
`game_flag[n] = !polarity` for each half whose update bit is set, so update bits make an event
one-shot. Quirk: 0x48bee0 returns early when the high half's flag number is 0, so a low-half
update only happens when the high half is used too (the decoder models this).

### Trigger kinds (cond >> 28)

| kind | name | condition |
|---|---|---|
| 0, 1 | auto | no position test, fires as soon as the flags allow (used for scripted warps) |
| 2 | action | action button, plus a look test (types 0..10) or an inside test (types 11..13) |
| 3 | look | look test, no button |
| 4 | enter | inside test, no button |
| 5 | use_item_look | the item in bits 11..4 is used from the inventory, look test |
| 6 | use_item_enter | item used, inside test |
| 7 | action_have_item | action button while holding the item, look or inside |
| 8 | have_item_look | holding the item, look test, no button |
| 9 | have_item_enter | holding the item, inside test, no button |
| 10 | action_or_use_item | action while holding the item, or using it; look or inside |

Kinds 3/5/8 assert that the record is a look shape and 4/6/9 that it is an area; no event in
the PC data violates this. Counts across all stages: auto 92, action 1665, look 8, enter 219,
use_item_look 66, use_item_enter 5, action_or_use_item 23.

### ev_pos records

`ev_pos` is a byte array of packed, unaligned records. Every record starts with
`f32 x` (+0), `u16 y` (+4, 16-bit float), `f32 z` (+6); extra values are 16-bit floats from +10.
The 16-bit float is decoded by 0x48b8b0 (`CharToFloat2`): IEEE half layout, but exponent 0 and 31
are treated as normal exponents (no zero, denormal or infinity cases), so 0x0000 is 2^-15.

| type | size | shape | extras |
|---|---|---|---|
| 0 | 10 | point | none |
| 1 | 12 | line (x,z) to (x+w, z) | w |
| 2 | 12 | line (x,z) to (x-w, z) | w |
| 3 | 12 | line (x,z) to (x, z-w) | w |
| 4 | 12 | line (x,z) to (x, z+w) | w |
| 5..10 | 10+4(t-4) | polyline with t-3 points | (dx,dz) pairs relative to (x,z) |
| 11 | 14 | rectangle x..x+w, z..z+d | w, d |
| 12 | 18 | triangle | 2 (dx,dz) pairs relative to (x,z) |
| 13 | 22 | quad | 3 (dx,dz) pairs relative to (x,z) |

Height test (both look and inside): the record y must satisfy `py - 500 <= y <= py + 100`
(py is the player's feet, Y points down), unless `y >= 45000`, which means any height. The JSON
gives this as `player_y_range`.

Look tests (0x48d980, PS2 `EventCheckLook`):

* point (0x48b490): `|dx|, |dz| <= 400` and distance <= 400, and the direction from a point 50
  units in front of the player to the record is within 30 degrees of the player's facing.
* line and polyline segments (0x48b570, `EventCheckLookLine`): the 400-unit ray along the
  player's facing must cross the segment, and the player must be on the segment's front side,
  `cross(B-A, P-A) >= 0` with `cross(u,v) = u.x*v.z - u.z*v.x`. The front normal is
  `(-dz, dx)` of the segment direction; type 1 is triggered from +z, type 2 from -z, type 3
  from +x, type 4 from -x. The JSON gives `front_normal`.

Inside tests (0x48df40, PS2 `EventCheckIn`): type 11 is `x <= px <= x+w, z <= pz <= z+d`;
types 12/13 require `(px-ax)*(bz-az) - (bx-ax)*(pz-az) >= 0` for every edge A->B, closing edge
included.

### Result types (rslt0 >> 28) and the PC exec functions

| type | name | PC function | effect |
|---|---|---|---|
| 1 | flag_set | 0x48bfc0 | set game flag rslt1&0x3fff |
| 2 | flag_clear | 0x48bfc0 | clear it |
| 3 | program | 0x48c110 | call `ev_prog[(rslt1>>14)&0x7f]`, set the rslt1 flag when it returns nonzero |
| 4 | door | 0x48e220 | set the rslt1 flag, compute the destination (0x48bc30), fade, open/close sounds |
| 5 | door_program | 0x48e220 then 0x48c110 | warp first, then run the program; the flag is set after the program |
| 6 | message | 0x48c040 | stage message (rslt1>>14)&0xff, set the rslt1 flag |
| 7 | door_unlock | 0x48c1c0 | message (0xff means system message 5), door_se.unlock, set the rslt1 flag |
| 8 | door_locked | 0x48c1c0 | system message 4, door_se.lock |
| 9 | door_jammed | 0x48c1c0 | message, 0xff = system message 6, 0xfe = system message 7, door_se.jam |
| 10 | save_point | 0x48c680 | save menu (0x4551d0) with id (rslt1>>14)&0xff, first use shows system message 8 |

Counts across all stages: flag_set 158, flag_clear 2, program 426, door 557, door_program 13,
message 380, door_unlock 9, door_locked 51, door_jammed 455, save_point 27.

### Door destination (0x48bc30, PS2 `EventResultMovePosition`)

The destination is a record in the *current* stage's `ev_pos` (offset rslt0 27..16, type
rslt0 15..12), in the coordinates of the target stage. With record (x, y, z) and extra w:

| type | position | facing |
|---|---|---|
| 1 | (x + w/2, z + 250) | 0 (+z) |
| 2 | (x - w/2, z - 250) | pi (-z) |
| 3 | (x + 250, z - w/2) | pi/2 (+x) |
| 4 | (x - 250, z + w/2) | -pi/2 (-x) |
| 5 | midpoint of (x,z)..(x+dx, z+dz) plus 250 along (sin a, cos a) | a = atan2(dx, dz) - pi/2 |
| other (0, 6..) | (x, z) exactly | unchanged |

y is the record's 16-bit y. Facing follows the game's convention, forward = (sin r, cos r), and
is wrapped into (-pi, pi]. In other words the destination record is the door line on the far side
and the player appears 250 units (about 50 cm) in front of its midpoint, facing away from it. For
same-stage doors the destination record is usually the trigger record of the reverse door
(498 of the 552 line destinations coincide with a door trigger line in the target stage).

The result goes to `DAT_01dbc210..21c` (PS2 `connect_pos`: x, y, z, rot). When the stage bits
are nonzero, byte 0 of `DAT_01dbeb80` gets the new stage id, `DAT_00934d94` bit 3 is set when the
new stage is in a different building group (0x48d880), and the connect code at 0x401480 later
copies `connect_pos` into the player. The PC merges PS2 `EventExecDoor` and `EventExecMove` into
one function; a door with stage 0 is the old in-stage door, a door with a stage id is the old
move.

### Items, models, enemies

`Item_List` (16 bytes): `f32 x, f32 z, u16 y (half), u16 rot_y (half), u32 st`. `st >> 29` is the
pickup kind (0 ends the list), indexing the table at 0x88cca0 {item given, system message,
pickup model}; bits 28..26 are a difficulty class and 24..20 a condition type, both passed to
0x48cb20 (`EventItemConditionCheck`); bits 13..0 are a flag n. The pickup is offered while flags
n and n+1 are both clear and the pickup sets n+1. It is tested by 0x48b980 (`ItemCheckLookPoint`):
same 400 radius and 30 degree cone as a look point, height window `py - 750 <= y <= py + 100`.
Items live in rooms but often sit on furniture, so their y is commonly a few hundred units above
the floor.

`Model_List` (40 bytes) and `Enemy_List` (20 bytes) are decoded as in the PS2 headers and end at
`kind == 0` (the loops in the character loader 0x493430 stop there). Enemy positions are ints;
the unit of `rot_y` was not checked, so it is exported raw.

## Rooms

`sh2rooms.room_id(glb_crd, x, z)` transcribes 0x494db0, the game's position to room-id lookup.
The world is split into 20000-unit cells (`cell = int(x/20000+16)*32 + int(z/20000+16)`) and each
`glb_crd` maps cells (mostly 2x2 blocks) to a room id. Room ids are unique across the game
(apartments 0x0f..0x28, hospital 0x29..0x5b and so on). `GLB_AREAS` gives the bg directory for
each `glb_crd`; it was found by checking that every .cld of an area lands on a different room id
(ap 26/26, hp 51/52, ps 53/53, rr 24/24, ru 18/18, data/bg2/ma 21/21; bw, th and er match the
3 or 4 rooms of bowling, heaven's night and the opening toilet). The town, forest and lake share
one room id each and are matched against the ca/cb/cc and cd tiles geometrically.

`events.py` tags each trigger (at the point where the player stands: 150 units in front of a door
line, or the centre of an area) and each destination with `room_id`, the bg `area`, the `map`
file name (`ap06` means data/bg/ap/ap06.map and .cld), the floor height under the point and
`on_floor` (a floor triangle of that room under the point within 300 units of its y). Door
triggers also get `wall_dist`, the distance from the door line's midpoint to the nearest wall of
the room.

## Validation

* No event in the PC data has a set bit outside the fields above, and no trigger kind is paired
  with a shape type the game would assert on.
* 570 door events (type 4 or 5) decode in total. 566 destinations land on a floor of the room the
  game's own room table gives for that position. The other four are a scripted hospital warp
  (stage 22 event 4), the lake boat warps (stage 38 event 0, stage 51 event 8) and the hotel to
  dog-ending warp (stage 46 event 7, which has no collision file).
* All 525 door triggers that have a shape stand on a floor of a room; the other 45 are auto
  (flag driven) warps.
* Apartments (stages 6..12 and 58): 74 doors, all destinations on walkable floor. Counting
  apart_out (stage 13) as well, the door line midpoints sit a median 65 units from a wall of the
  room (77 measured, 3 over 100: two are apart_out doors in the open town tiles, one is a
  stairwell door in ap26).

Doors per stage group (type 4/5, cross-stage in brackets): town and outdoor 48 (33), apartments
74 (26), bowling and heaven's night 13 (3), hospital incl. the TGS trial stage 141 (28), society,
prison and labyrinth 128 (17), hotel 102 (29), endings 5 (3), DC-only stages 53..57 59 (12).

## PC function map

| PC address | PS2 name | confidence | notes |
|---|---|---|---|
| 0x48aed0 | FlagInit | high | clears game flags, randomises puzzle answers by riddle level |
| 0x48b440 | EventProgInit | medium | resets the event state machine |
| 0x48b490 | (EventCheckLookPoint core) | high | point look test |
| 0x48b570 | EventCheckLookLine | high | ray vs segment, front side only |
| 0x48b781..0x48b847 | Event_List bitfield accessors | high | |
| 0x48b8b0 | CharToFloat2 | high | 16-bit float |
| 0x48b980 | ItemCheckLookPoint | high | |
| 0x48bac0 | EventPositionSet | high | centre of a record, used for 3D sound positions |
| 0x48bc30 | EventResultMovePosition | high | door destination |
| 0x48bed0 | EventCancel | medium | sets the cancel flag |
| 0x48bee0 | EventExecSubFlagSet | high | |
| 0x48bfc0 | EventExecFlag | high | |
| 0x48c040 | EventExecMessage | high | |
| 0x48c110 | EventExecProgram | high | |
| 0x48c1c0 | (door message part of EventExecDoor) | high | locked / jammed / unlock |
| 0x48c3a0 | EventExecItem | high | item pickup |
| 0x48c680 | EventExecSave | high | |
| 0x48c840 / 0x48c870 | LightSpotOnOffCheck / LightSpotOnOffSet | medium | |
| 0x48c8d0 / 0x48c950 | (map / system message events) | low | possibly EventExecChizuFail |
| 0x48c9d0 | probably EventProgressCheck | medium | labelled RadioNoise in the Ghidra project; returns a progress code from stage and flags |
| 0x48cb20 | EventItemConditionCheck | medium | difficulty filter for pickups |
| 0x48cbc0 | RadioNoise | high | radio static from nearby enemies |
| 0x48d880 | (stage group compare) | medium | same building check for stage moves |
| 0x48d980 | EventCheckLook | high | |
| 0x48df40 | EventCheckIn | high | |
| 0x48e220 | EventExecDoor + EventExecMove | high | merged on PC |
| 0x48e520 | EventCheck | high | labelled EventCheckLookLine in the Ghidra project, which is wrong |
| 0x48eac0 | EventMainStandard | high | labelled EventCheckLook in the Ghidra project, which is wrong; dispatches on rslt0>>28 |
| 0x48ecb0 | EventMain | medium | |
| 0x494db0 | (room id lookup) | high | transcribed in sh2rooms.py |
| 0x401480 | (connect / room init) | high | consumes connect_pos |
| 0x49e4e0 | (remove item) | high | used by the consume bit |

Globals: `DAT_01f7d550` stage, `gEnableInput` (misnamed in Ghidra) is the active event index
(>= 0x400 means an item pickup, index - 0x400), `DAT_01f7d558` the event step (ev_e_step),
`DAT_01f7da80` its sub-step, 0x1f7d5e0 game flags, 0x1f7d3e0 item possession bits,
`DAT_01dbc210` connect_pos, `DAT_01dbeb80` byte 0 the stage id.

## Open questions

* The exact effect of rslt0 bit 4 (camera flag) was not traced past the call into 0x51d030.
* Message numbers are exported raw; mapping them to text needs the message files.
* System messages 6 and 7 for jammed doors are probably different wordings ("jammed" vs
  "the lock is broken"); not checked against the message data.
* `ev_prog` programs (result types 3 and 5) are native code per stage. Many doors in the data
  are gated by flags those programs set, so a port has to reimplement the relevant programs or
  pre-set flags.
* `rot_y` units in Enemy_List and the item difficulty class / condition type semantics are not
  fully pinned down (all PC items have class 0).
* glb_crd 14 (end_dog) and 15 have no matching collision area; destinations into stage 52 are
  untagged.
* The look-at polyline test uses each segment's own front side; the JSON only gives the first
  segment's normal.
