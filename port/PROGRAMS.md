# Stage programs (ev_prog)

Result types 3 (program) and 5 (door, then program) of the stage events call native code in the
stage overlay: `ev_prog[(rslt1 >> 14) & 0x7f]` of the current `Stage_Data`. These programs give the
key items, run the puzzles, set the flags that unlock doors and start the cutscenes. The port
reimplements them by hand in `source/prog*.c`, from the PC Director's Cut exe (static analysis
only; the PC stage code at 0x590000..0x5a2000 has no symbols) with the names of the PS2 beta debug
info (`Event/stage/stg_*.c`, which lists the programs in the same order as the PC tables in most
stages). Flag, item, message and sound numbers are the PC's.

Stages 2..58 are ported: the opening restroom and observation deck, the forest path, town east,
all apartment stages, town west, Pete's Bowl-o-Rama and Heaven's Night, Brookhaven Hospital on both
sides, the Historical Society, the prison and the Labyrinth up to the Eddie fight (230 of the 234
table entries of stages 2..37; the four left out are never run by an event or switch the game
mode, listed below), then Toluca Lake, the Lakeview Hotel in both worlds, the six ending stages and
the Director's Cut scenario Born From a Wish (all 97 entries of stages 38..58, four of them
reconstructed because the PC keeps their code encrypted). Stage 17 (Heaven's Night) has no table of its own, and the TGS trial
stage 1 has its own copies of the hospital programs (0x5a21f0..0x5a4900), which are not ported.

## How a program runs

`events.c` handles result types 3 and 5 like the PC's `EventExecProgram` (0x48c110):

1. When the event fires, its flag update bits are applied (`EventExecSubFlagSet`) and
   `prog_start(stage, prog_no, flag)` gets the event's rslt1 flag (bits 13..0).
2. `main.c` calls `prog_update(down, held, dt)` every frame instead of the event check while the
   program runs. A program is a state machine over `ev_p_step` / `ev_s_step` like on the PC, so it
   can wait for a message to close, an answer, a puzzle or a cutscene.
3. When the program returns nonzero the runner sets the rslt1 flag, unless the program cleared
   `flag_set` (the PC's `ev_prog_flag_set`, 0x1f7d554), and the event check resumes.

Type 5 warps first and starts the program once the warp is done; the door's flag is then set when
the program finishes, as on the PC. Program events of stages without a port stay skipped as
before, and type 5 events of those stages still warp and set their flag.

While a program has James in event mode (`JmsEventMode(jms, 1)` on the PC) he stands still; a few
programs leave him free to move.

The helpers in `prog.c` are the PC's `event_sub.c` functions:

| PC | port | what it does |
|---|---|---|
| 0x4a1e40 EvSubMessage | `p_message` | stage message, waits until it is closed |
| 0x4a1ea0 + MesState | `p_question` | message with a yes / no answer (the PC marks it with code ff f6) |
| 0x4a1fb0 EvSubItemGet | `p_item_get` | "I got ..." with SE 11041, the item is added when the message closes |
| 0x4a1f10 | `p_item_get_quiet` | the same without James' pick-up motion |
| 0x4a2f20 EvSubItemUse0 | `p_item_use` | removes the item, "I used ...", door sound |
| 0x4a21e0 EvSubPictureDisplay | `p_picture` | full-screen still, waits for a button |
| (same, under a puzzle) | `p_picture_show` | the still stays up while the puzzle panel runs |
| 0x4a3170 | `p_picture_msg` | still, button, message |
| 0x4a3370 EvSubMapGet | `p_map_get` | map still and message |
| 0x53b010 / 0x53b320 | `p_walk` | James walks to a point (the "I can't go back" programs) |
| 0x49e310 ItemGet / 0x49e4e0 ItemLose | `p_item_get_now` / `p_item_lose` | counts as on the PC (ammo boxes, health items) |

## Engine services

`prog.h` declares `ProgHost`, filled in by `main.c` (`prog_init`). Every entry may be NULL.

| service | main.c today |
|---|---|
| `message(text)` / `message_open()` | the bottom-screen message box ('\f' pages, A closes) |
| `panel(text)` | persistent text under the HUD for yes / no choices and the puzzles (clock, coins, safe, elevator, box, keypad, quiz, panel lock, cube) |
| `picture(name)` | shows `pack/pic/<dir>/<name>.t3x` 320x240 over black on the top screen, NULL hides it; false if the file is missing, and programs then go on without it |
| `fade(type)` / `fade_done()` | PC FadeStart types: 1, 2, 6, 7, 11 fade out; 3, 8 black at once; 4, 9, 12 fade in; 5, 10 clear |
| `se(id)` | `sfx_play(id, 1, 0)` with the PC SE id (positional SEs play at full volume for now) |
| `player_get` / `player_set` | James' position; `player_set` is `CharaSetPos(jms)` with the floor snapped |
| `walk_to(x, z)` | straight walk at walking speed, gives up after four seconds |
| `demo(id)` | cutscene hook, see below |
| `save_menu()` | the save prompt (the first save point in the forest well) |
| `flashlight(on)` | the flashlight comes on when James picks it up |
| `game_end(ending)` | an ending's last program ran (PC 0x48d7e0 with the ending's bit: 1 Leave, 2 Maria, 4 In Water, 8 Rebirth, 0x10 Dog, 0x20 UFO, 0x40 Born From a Wish, 0 = backed out of Born From a Wish); main.c goes back to the title once the program has returned |
| `damage(fraction)` | hurts James by a part of his maximum health (`combat_damage_james`, the hotel fire) |

`prog_stage_tick(stage)` is called every game frame for the stages' own per-frame code that the
programs depend on; today that is the hotel 2F (stage 41): the weight check of the employee
elevator (0x57a4a0, flags 400 and 401) and the trunk's drums (EvStageInit 0x57a340).

## Pictures

`tools/sh2conv/pics.py` converts every single-sprite still in `data/pic/<dir>/*.tex` (226 of them,
57 MB) to `pack/pic/<dir>/<name>.t3x`: decoded with `ui.py`'s `load_tex`, squeezed to 512x256
(the PC stretches its 512x512 stills to 4:3, so nothing is lost on a 240-line screen), RGB565
without mipmaps. Programs name a still like the PC's file id without `data/pic` and `.tex`, with
the first underscore as the folder: `hsp_p_box` is `pic/hsp/p_box`. `main.c` loads one at a time
into linear memory (256 KB) and frees it when the program hides it or ends.

### Overlay sprites

Many puzzle stills come with a second file, a sprite sheet (the third argument of
EvSubPictureDisplay), and the PC draws sprites from it over the still every frame. `prog_overlay.c`
does the same for fifteen stills, transcribed from their draw functions; it picks the overlay by the
still's name whenever a program shows one, so the programs did not change:

| still | sheet | PC | what is drawn |
|---|---|---|---|
| `apt_clock_close` / `apt_clock_open` | `apt/clock_hari` | 0x59a390 | the hour and minute hands from the clock's time, the second hand at 15 seconds, the cap |
| `apt_p_safe_close` | `apt/p_safe_close2` | 0x596f20 | the dial, turned to its position (flags 103..107) |
| `apt_p_desk_coin` | `apt/p_desk_coin_coin` | 0x597ce0 | the coins in the five holes (flags 126..140) |
| `hsp_p_box` | `hsp/pboxkey01` | 0x58e1f0 | the key locks (until 195), the wheel panel and its four symbols (until 197), the button panel (until 198) |
| `hsp_p_boxnumber` | `hsp/p_boxnumber_2` | 0x58db10 | the four symbols on the wall the wheels must match |
| `hsp_p_quiz` | `hsp/p_quiz_btn` | 0x589820 | the pressed button of each row (flags 234..239) |
| `htl_p_trank` | `htl/dram` | 0x579af0 | the trunk's four drums, the letter in the window and the ones above and below |
| `htl_p_egghollow` | `htl/egg` | 0x576510 | the rust-colored or scarlet egg in the door |
| `add_p_teien` | `add/teien2` | 0x571f50 | the boards in the tombstone, turned as they went in, and the Acacia key until it is taken |
| `htl_p_video` | `htl/p_video02` | 0x57bce0 | the videotape and the can opener in the office safe until they are taken |
| `htl_p_marker` | `htl/maker` | 0x579f60 | the trunk's word on the photo, the photo's frame and the marker over the word until the thinner takes it off |
| `hsp_p_h_elevator` / `_2` | `hsp/p_h_elevator_botan` | 0x58cad0 / 0x5888a0 | the button that was pressed, fading in over a second: 3F, 2F, 1F from the top (flags 21..23), the fourth when the car is already on that floor (17..19) |
| `dls_p_manacles_on` | `dls/p_manacles_off` | 0x57fd50 | the open manacles fading in over the closed ones in two seconds once the key turns, then held until a button (flag 367) |

Sprite positions are the PC's sprite units: x and y -256..256 across the still (the PC keeps them
in sixteenths), mapped onto the port's 320x240 still; turned sprites keep the PC's 0.8 squeeze of
x. The PC fades some parts in and out (the box's locks over a second once they open, the board
being put into the tombstone); the port shows them while they apply. `tools/sh2conv/pics.py` now
also writes the sheets at their own size with alpha (`pack/sheet/<dir>/<name>.t3x`, RGBA4) and the
tables these overlays read from the exe (`pack/pic/overlay.dat`: the clock hands' sprites, the
desk coins' places, the trunk's letters and places, the tombstone, the box and the wall). The panels
on the bottom screen still print each puzzle's state. The music box and the hanging corpses are
3D models on the PC, not overlays. The PC moves a cursor over the hospital elevator's five buttons
(0x58c820, centres at 0x8da814); the port keeps its floor list on the bottom screen and lights the
PC's button once a floor is chosen.

The seven stills that used to be skipped are converted now: `htl/p_hanging` and `p_hanging2` are
PC files with 8-bit indices and an RGBA palette (format 8), and `hsp/p_box_key`,
`item/x_coin_snake` and `out/p_statue_box`, `p_statue_key`, `p_statue_key2` are PS2-format files
(32-bit pixels in the PS2's 0..0x80 range, or GS-swizzled 8-bit indices with a CSM1 palette).
`hsp/p_box_key` is not a still but the sprite sheet of the hospital box (wheels, buttons, chains),
the first piece an overlay for that box would need. Only `effect/poison.tex` is still skipped.

## Cutscenes

Programs that start a demo (`.dds` + `.anm`) or a movie (`.sfd`, Bink on PC) call
`prog_demo_request(id)` every frame until it returns true; the ids are `ProgDemo` in `prog.h` and
`prog_demo_name(id)` gives the PC file names. `main.c`'s hook returns true at once, so a cutscene
counts as watched and only its side effects happen: the flags it sets, the items it gives (the
plank and the radio in town east, Angela's knife), and where James stands afterwards (the PC's
`CharaSetPos(jms, ...)` after the demo). A cutscene player can take over by keeping the hook false
until the demo ends.

Every movie in a demo name plays in turn (`movie_play`), so the endings run their movies and the
credits: "water.sfd + end_scde.sfd + credits.sfd" for In Water, "end_dog.sfd + credits.sfd" for the
dog. The ending programs then call `game_end`.

## Stage models

The stages place models of their own in the rooms: each stage's Model_List (40 bytes: a kind, an
id, flag_off, flag_on, a place and a turn) lists cars, trailers and street furniture in the town,
furniture and props in the buildings (the piano, the beds, the tombstones), the puzzle objects
and keys that lie around (the 0x7xx kinds, such as the clock key, the hotel keys, the coins, the
figure boxes) and the Born From a Wish props. The character loader (0x493430,
Event/chara_data_load.c) finds a kind's files in the table at 0x88d4f8 (s32 kind, then file
records for the .mdl, an .anm and a .kg1) and loads the models placed in James' room whose
flag_off is clear; the 0x3xx kinds (the town's cars) go to 0x53b510 instead in James' game,
which places them the same way. All 425 places use 184 models, every one of which has a file.

`tools/sh2conv/stagemodels.py` converts all 184 (`pack/chr/item`, `item2` for data/chr2, about
27 MB with their textures at most 256 texels) and writes `pack/events/stagemdl.txt` with one line
per place. `stagemdl.c` reads the current stage's places and draws, with the characters' lights,
those within 14000 units of James whose flag_off is clear and whose flag_on is set (or 0); a
model is loaded the first time one of its places comes that close (one a frame, at most 20 kept,
the one unused longest dropped). The turn is applied as z * y * x. A place whose kind a running
cutscene has among its characters is left to the cutscene (`demo_has_kind`), so a door or a box
the scene moves is not drawn twice. Models with an .anm of their own (the fridge, the meat hooks)
are drawn in their bind pose, and the .kg1 files are not used.

The music box (stage 40, kind 0x524) turns its parts while the right order plays (0x57b5c0 called
with 15 seconds each frame of org_b), as a function of the time t:

* bones 0, 6 and 7 by -pi/2 about y over the first tenth and back over the last (bone 8 the other
  way), bone 2 at 11.78 / 15 rad/s in between, ending at pi;
* the figure box on each seat (kinds 0x718..0x71a): from 1.5 to 13.5 s bone 0 turns about y by
  theta = (t - 1.5) * 11.78 / 15 and goes round a circle of 135, x = 135 sin(theta), z =
  135 (cos(theta) - 1), stopping at theta = 3 pi;
* the stairway key in the box (0x747): bone 0 turns about z by theta and goes round x = -135
  sin(theta - pi/4) - 103, y = 135 cos(theta - pi/4) - 103, stopping at 11 pi / 4.

The PC reads a bone's rotation, changes a component and writes it back (0x538b80 / 0x538c30); the
port adds to the components of the bind rotation (taken apart in chr.c's z * y * x order). The
figure boxes carry flag_off 472, which the right order sets; the PC tests flag_off only when it
loads a room's models, so the boxes stay for the tune, and the port keeps them while it plays.
`debug.txt` `musicbox <seconds>` (with a spawn in stage 40) poses everything as at that time.

## New game: FlagInit (0x48aed0)

`prog_new_game(riddle, action)` runs after `ev_reset()`. It sets the difficulty flags (12 for
beginner and easy action, 13 or 617 for the riddle level) and the random ones (543, 547, 551, the
hospital and hotel puzzle flags 317..324 and 406..410), and it randomises the puzzle answers the
PC keeps at 0x1f7da60..0x1f7da7f: the safe code in w2f, the clock's start time in room 208 (never
within 8:40..9:40) and the values the hospital and hotel puzzles will need. The PC's random
generator (0x448810, a 31-bit LCG) is used. Its New Game+ branches read the clear data (below).
The puzzle state, both levels and the chapter are saved with the game (`save.dat` version 3;
version 2 files still load).

New Game and Born From a Wish first ask for the action level (beginner, easy, normal, hard) and
the riddle level (easy, normal, hard), as the PC's difficulty screens do; hard shows as "Extra" once
FlagInit would play the extra riddle level (`prog_extra_riddle`). The defaults are riddle level 1
(normal) and action level 2 (normal); `debug.txt` takes `riddle <0..3>` and `action <0..3>`.

Born From a Wish (gChapterID 1) is the second title menu entry. `prog_new_game_wish(riddle,
action)` does what FlagInit does for that chapter: only flags 5 and 24, no puzzle answers. The game
starts in stage 53 in th09, the back room of Heaven's Night, where the opening program
(0x5740c0) shows the "additional scenario" notice with Yes / No and places the player after the
opening scene. The door records carry the stage number, so the scenario stays in stages 53..58.
The PC switches the player model to Maria there; the port has no Maria model yet, so James stands
in for her. That switch, Maria's own motion set and her weapon handling are the engine support
the scenario still needs beyond `ProgHost`.

## The ending score

The ending decision at the hotel's last door (stage 44 program 6, 0x576660) weighs flags and
counters the PC keeps in its flag block, so the port keeps them in the same place
(`p_flag_bytes`, saved with the flags) and main.c reports what happens:

| counter (PC) | port | counts for |
|---|---|---|
| 0x1f7d9d8: time at low health, weighted by 1 - sin(pi * hp) | `prog_score_frame` | In Water (> 60 and > 240 seconds) |
| 0x1f7d9d4: health items used over the maximum, in maximums | `prog_score_overheal` (the item use in main.c) | Leave (> 2) |
| 0x1f7d9cc / 0x1f7d9d0: time near Maria (< 1000) / far from her (> 5000) | `prog_score_frame` (no Maria yet: -1) | Maria (> 600 / > 300 seconds) |
| 0x1f7da7a: looking at Mary's photo or letter in the item list, up to 2 | `prog_score_examine` | Maria (one less above 1) |
| 0x1f7da7b: enemies killed, up to 100 | `prog_score_kill` (needs a call where enemy.c kills) | Maria (above 30, under 3) |
| 0x1dbebb0, 0x1dbebac, 0x1dbeba8: damage to Maria, the other damage class, damage James took | `p_damage_stats`, James' part from his health in `prog_score_frame` | Maria |

Flags weigh in as on the PC (655, 656, 204, 371, 500, 615, 629, 630, 624), and having the four
Rebirth items (74..77) sets 621 before anything else. The result is flag 618 (Leave), 619 (Maria)
or 620 (In Water), which the stage's doors turn into stages 48, 49 and 50; 621 leads to 51
(Rebirth). The endings seen before (0x1dbeb84) come from the clear data.

## Clear data and New Game+

The PC keeps three bytes of clear data in its system data, outside the saves: 0x1dbeb82, the
riddle (bits 0..3) and action (bits 4..7) levels a game was finished on; 0x1dbeb84, the endings
seen (1 Leave, 2 Maria, 4 In Water, 8 Rebirth, 0x10 Dog, 0x20 UFO, 0x40 Born From a Wish); and
0x1dbeb85, the number of clears (up to 99). `p_game_end` records an ending into them like
0x48d7e0 does (Born From a Wish only adds its bit), and main.c keeps them in `clear.dat` next to
the options (`SHCL` and the three bytes), written when an ending runs and read at start-up. The
title lists the endings seen.

Nothing is carried over in the items; as on the PC, a new game reads the clear data in FlagInit
(`prog_new_game`): with one or two endings seen it sets 7, 8 or 9 (which of the extra items and
endings come up), with any it sets 10 and 12 (the New Game+ items such as the chainsaw), all three
of Leave, Maria and In Water or Rebirth seen sets 11 (the dog key), Born From a Wish cleared sets
723, and three clears (or the right levels) set 631. Hard riddles become the extra level once Leave,
Maria and In Water have been seen.

## Map screen

`map.c` is the PC's Event/map.c (0x49a7e0..0x49e310) with its tables out of the exe
(`tools/sh2conv/maps.py`, which writes `pack/map/`):

* The maps, indices 1..36 as on the PC: the town (1..3), the apartments (4..11), the hospital
  (12..15), the prison and the Labyrinth (16..19), the hotel floors BF to 3F (20..27 the guest map,
  28..35 with the employee map's rooms) and the Baldwin mansion (36). Their textures and marker
  sheets come from 0x891670 and 0x891708; the 1024x1024 maps are squeezed to 1024x512 RGB565 (the
  PC stretches them over its 4:3 screen), the marker sheets are RGBA4. One map and one sheet are in
  memory while the screen is open (about 1.5 MB), none otherwise.
* Which map the player is on: the room id (FUN_00494db0, `bgm_room_id`) picks a map from 0x88f700,
  and 0x49a7e0's special cases pick floors from the height (the apartment and hospital stairs, the
  hotel's two-storey rooms) and switch to the combined apartment maps and the employee hotel maps.
* Which maps the player owns (0x49adb0): flags 24 (town), 25 / 26 (apartments), 27 and 215
  (hospital), 28 / 29 (prison), 30 / 31 (hotel, 476 for the later floors) and 670 (Baldwin).
* The markers (0x49afa0): each map has one or two tables of flags (1485..2451) with a sprite and a
  position each, the red marks the events and programs set (locked doors, items, the music box);
  any set flag draws its sprite.
* The marking animation (0x49c0f0 / 0x49dbd0 / 0x49c180): four programs mark James' map, the park
  board (mode 3), Neely's memo (1), the restroom wall map (2 or 4) and the Director's map in the
  hospital (0, which goes on with mode 5 on the west map). The map screen opens by itself, centres
  on each new mark, zooms in, fades it in over 16 frames, waits 24 frames and scrolls to the next
  (the lists are in `map.dat` version 2), then closes and the program goes on (`p_map_mark`).
* The player's arrow (0x49b510): position = world x and z times a per-room scale plus the room's
  offset (some prison rooms turned a quarter, the hospital's stacked otherworld floors by height),
  the direction from his yaw. The PC draws a sprite from a menu texture; the port draws a red
  arrow of the same size.
* Opening (0x48ef03): a tap on the touch screen (the PC's map key) or "Map" in the pause menu. In a
  dark room without the flashlight on it says common message 2 ("It's too dark to read the map.");
  without a map of the area common message 3, and then the town map if the player has it, as on
  the PC. The screen opens zoomed in on the player except on the town map.
* Keys as on the PC: zoomed out the D-pad goes to the floor above or below and to the neighbouring
  maps (0x890108), A zooms in; zoomed in the Circle Pad and the D-pad scroll (307.2 and 384 units a
  second) and A zooms out; B or START closes. The bottom screen names the map.

## Memo list

The memo list (Event/memo.c, 0x4989c0..0x49a2c0) opens from the pause menu. Its 57 memos
(0x88ef60) each have a game flag (set when the memo is read in the world), a title and a text in
`memo_x_msg_e.mes` (converted to `pack/events/memo.msg`), and up to two stills. The list shows the
read memos in the PC's order on the bottom screen over the memo paper (`etc/p_memo`); A opens one:
its still first and, for the memos with text, the text below. The wallet memo gets the safe code
and the carbon paper and the "i know it" notes the box code, as on the PC (`prog_memo_text`). The
PC draws overlays over seven memo stills (0x49a210, by memo number), and `prog_overlay.c` does too
(`prog_overlay_memo`, called each frame while a memo's still is up, with the memo's second
picture as the sprite sheet): the safe's dial in the wallet memo's safe (0x12, once flag 607 is
set, the dial at flags 103..107), the four wall symbols (0x20), the hair in the drain picture (0x21,
flag 171), the woman's two rings (0x23, flags 227 and 228), the three tablets (0x29, flags 301..303),
the trunk's word on the photo (0x35) and the tombstone's boards (0x36). Four of them are the stills
of puzzles and draw what those overlays draw; the PC keeps its own copies of their tables (0x88f2f0..)
with the same values as far as checked on screen.

## Inserted values in messages

The PC puts values into messages at codes ff e0..ff e3 (MesSetInsert 0x47fd70) and marks yes / no
questions with ff f6. `tools/sh2conv/sh2mes.py` now keeps them as bytes 1..4 and 5, and
`ev_msg_expand` (called by main.c's message box) puts in the values set last with
`ev_msg_set_insert`, as on the PC; byte 5 is dropped (the choice panel asks the question). The
clock time (0x59c34c: the hour, the minutes with two digits in English), the safe code in the
wallet (0x596dd0 / 0x499520) and the box code on the carbon paper and in the "i know it" notes
(0x58fe64) now go through the inserted values instead of the old text searches. The message files
need converting again (`events_pack.py`, and `maps.py` for the memos); old files show the messages
without the values.

## Healing items

The item use code (0x49e7e0): the health drink adds a quarter of James' maximum health, the
first-aid kit half and the ampoule all of it, at once; whatever goes over the maximum counts for the
ending (0x1f7d9d4). The ampoule also starts a 600 second timer (0x1f7d40c, full for the first 300
seconds, then fading, read by 0x49e8b0) during which the PC does not count up jms + 0x474, the
counter that picks James' tired, hurt motions; the port has no such motions yet, so the timer is
not kept. main.c's provisional recovery pool is not used for the items any more.

## Results screen

After an ending's movies the PC shows the results (0x447650, `result_msg`) and then returns to the
title; the port does the same (any ending but backing out of Born From a Wish). It lists the
levels, the ending, the endings seen, the saves, the play time, walking and running distance, the
items picked up, enemies shot and beaten, the boat stage (James' game only), the damage taken and
the rank. The counters are kept as on the PC (0x1dbeb8a..): time each game frame (0x5396c0),
distance each frame under 200 units, split by the running stride (0x5396e0), a save each time the
game saves, an item each ItemGet and pickup (0x5397f0), kills by attack id (0x539760, ids 1, 2, 4,
6, 0x19 and 0x1a are the guns), and James' damage. They are saved with the program state (bytes
48..63). The rank (0x539bf0) adds 4 per ending seen (at most 20), the levels (action 0, 1, 3, 5;
riddle 1, 3, 5, 5), kills / 5 of each kind (at most 15 each), damage (5 under 500 down to 0 over
2500), boat time (5 under 80 seconds down to 0 over 960), play time (10 up to 3 hours down to 0
over 16 hours), items / 20 (at most 15) and 5.

The top screen shows the results in the PC's layout (0x447650 with the English positions; the PC
clears the screen to black and has no frame or background picture, only `pic/etc/botan.tex` for
its button prompt): "GAME RESULT" in the big font's full-width letters (codes 0x17c..) centred at
the top, the fifteen labels of the table at 0x7b566c right-aligned at x 240 in the small font with
the white to grey fade of colour 10, and the values from x 256 in the PC's formats ("%dh %02dm
%02ds", "%.2fkm", "%d/4(+%d)", "%d pt%s", ...). A value is yellow (colour 4) when its category
earns its most points (action and riddle 5, endings 20, saves 5 for at most two (0x539850, which
only colours the line), time 10, items, shot and beaten 15, boat and damage 5; Dog and Rebirth for
the ending), except in Born From a Wish, where the riddle, ending, endings, boat and rank labels
are drawn in a colour past the end of the PC's table (shown dim here) and their values are left
out. The rank is a row of big stars (glyph 0x138), one per ten points, and a row of small ones for
the rest, yellow from 100. The footer is message 0x1c at (35, 460) with "Return" changed to "A".
The items line counts the secret items as the PC does: ItemGet (0x49e310) sets a bit of
gSecretItemsCollected (0x1dbeb88) for the chainsaw (16), hyper spray (12), dog key (25), the two
books (74, 75), white chrism (76), obsidian goblet (77) and blue gem (78) instead of counting them,
and counts nothing for Mary's photo and letter (20, 21). The line reads "n(+k)" with k in yellow once
all eight are found (the format's \4 escape), and each secret item is worth a rank point on top of
items / 20 (0x539970, at most 15). The bits are saved with the program state (byte 35).

After the results of one of James' endings the PC says "Save clear data." (message 0x1a in yellow,
centred) and a button opens its save screen (cases 3..5); Born From a Wish goes straight back to the
title. `clearsave.c` does the same with the port's one save slot, in the PC's save screen look
(0x453fc0 / 0x4537b0): James' picture (`data/menu/mc/savebg.tbn2`, converted by `ui.py` to
`pack/ui/savebg.t3x`) over the whole screen, "SILENT HILL 2 Save Files" at (36, 110), the file row
from y 150 and the questions in yellow, with the PC's own texts (`m_card_x_msg_e.mes` as
`pack/events/mcard.msg`: "Save to new file.", "Game Result", "Do you wish to overwrite?", "Save
completed."). It lists what the slot holds,
asks before writing over a saved game or other clear data, and writes a clear data save
(`save.dat` version 5: the program state with the results counters, the clear data's three bytes
and a clear mark, no place to start from). Continue on such a save does what the PC's load does
(0x402d00 case 10): the save's clear data joins the system's (the levels and endings added, the
higher clear count, written to `clear.dat`) and the New Game level screens open, so FlagInit gives
the New Game+ items and endings from it. Version 2 to 4 saves still load.

The bottom screen keeps its text version. `debug.txt` `results 1` (or `results 2` for Born From a
Wish) opens the screen right after start, and `itemget <id>` (up to eight) runs ItemGet before it.

## Text on the top screen

`font.c` draws the PC's own text font. The exe carries two fonts (0x806480: font 0 with 20x30
cells, font 1 with 16x24) as 3-bit run-length bitstreams per character, which the PC decodes into a
glyph cache texture on demand (0x480ea0, 0x480e00); `tools/sh2conv/ui.py` decodes the text
characters (codes 0..0xdf, Windows-1252 minus 0x20), the star and the full-width Latin letters of
both fonts, shrinks them to the 3DS size of the PC's 512x448 text space (320x210 pixels) and packs
them into `pack/ui/font.t3x` (LA4, 512x128) with their rectangles and the PC's advances in
`pack/ui/font.bin`. Text is laid out as 0x47f400 does it (advance = width + 2, the space 7, the
full-width codes the cell width + 2; centred and right-aligned lines measure without the last 2) and
coloured with the PC's colour table (0x47eca0), top and bottom colour per glyph. All glyphs of a
frame go out in one draw call.

The pause menu's "Messages" option (`options.txt` `messagestop`, bottom screen by default) puts
messages and cutscene subtitles on the top screen the way the PC's message window places them when
the message gives no position (0x47ff50): the big font, the lines left-aligned in a block centred
on the widest one, the first line at 476 - (lines + 4) * 15, half a line higher for each line
past three. The pack's message text no longer carries the PC's position words (0xfa00 / 0xf800
codes), so every message uses that place. A one-pixel dark shadow, which the PC does not draw,
keeps the text readable over bright rooms. The bottom screen then shows only the "A: next" line,
and choices, the save question and the puzzle panels stay on the bottom screen.

## Touch screen

The bottom screen is drawn with citro3d by `touchui.c` (port/ROADMAP.md workstream 2) in the
game's font (font.c lays the glyphs out in screen pixels, `font_layout_px`) and colours: the
labels in the PC's white to grey fade, the chosen row in yellow on a dark red bar. The libctru
console still exists but writes into a buffer of its own; SELECT twice shows it as the debug page
(and the room viewer always does), so nothing of the console is seen in play.

main.c tells touchui what is up where it used to print it, and touchui turns taps into the buttons
those screens already take, so the game logic did not move:

* in play: James' condition as a heartbeat line in its colour (Fine green, Caution yellow, Danger
  red, the help screen's thresholds of 70 and 35 percent), the equipped weapon and its rounds, and
  Items (Y), Map (the tap goes through as before), Light (X) and Pause (START);
* menus (title, New Game levels, pause, items): a tap chooses a row, a second tap takes it, and on
  a "< value >" row the ends step the value; the item list has Use (A), Examine (X) and Close (B);
* message boxes, subtitles (with Skip), the save question (Save / Cancel), the results' lines with
  Continue and the clear data save's questions;
* the stage programs' panels and the map and memo screens' text with the D-pad and A / B as
  buttons; a movie shows a Skip button.

In play the screen also shows the map James is on (`map_mini` in map.c): centred on him with his
arrow turned as the map screen turns it and the red markings whose flags are set, at the map
screen's zoomed-out size or twice that (the Zoom button), sharing the map screen's textures. A tap
on it opens the map screen as the map key does, and it says "Too dark to read" where the map key
would refuse (a dark room without the flashlight). While the map screen is up the touch screen has
its buttons: the floors above and below, the other maps, zoom and close; zoomed in, the arrows
scroll while held. The memo list is a touch menu (Read, Close), the memo's still on the top
screen and its text below. The item screen turns the selected item's model the way the PC's item
menu does (`itemview.c`, the skinning shader under its own light): the PC gives item i the model
of kind 0x892640[i] from the model table at 0x88d4f8 or, for the drinks, kits and rounds, the
table at 0x88d2b8; `tools/sh2conv/stagemodels.py` converts them (70 items have one) and writes
`pack/events/itemmdl.txt`. Mark (R) is the PC's way of choosing items to use together, whose
combining the playthrough batch ports in main.c. The pause menu has Music and Sound volumes
(0..10, `options.txt` `musicvol`, `soundvol`): music on channel 0, the cutscene voices and the
sound effects (`audio_set_gains`, `sfx_set_gain`).

The screen is redrawn only when something it shows changed (the condition, the weapon line, a
menu row, a message, the small map's picture: another map, James a pixel further, a new mark),
from one vertex buffer allocated once, so a frame without a change costs a few comparisons, which
matters on the Old 3DS; only the turning item model redraws continuously (every other frame) while
the item screen is up. The hardware rows of the Old 3DS profile (hardware, control layout,
quality) go into the pause menu's rows in `pause_draw` once `profile.h` is in the tree.

## Item use and item names
## Item use and item names

Trigger kinds 5 and 6 (use an item from the inventory) and the "use" half of kind 10 now work: A
in the item list uses the item (X examines). If no event takes it, James says item message 16, "It
looks like I can't use that here.", like the PC. This is how the clock key, the canned juice and
the three coins are used.

Item names: the converter wrote `pack/events/item.msg` from `item_msg_e.mes`, whose order drifts
from the game's item ids at id 10 (no revolver, no Chinese cleaver), so keys showed the wrong
names. The Director's Cut table is `item_x_msg_e.mes`, where item n is message 18 + 2n for every
id, so the converter should read that one (one line in `tools/sh2conv/events_pack.py`, in the
hooks diff).

## Program table

Ported means the flags, items, messages, choices and puzzles are in; cutscenes go to the demo
hook and stills show when the pack has them. Events are the stage's event indices.

### Stage 2 Stg_toilet (ev_prog 0x8eaaa8)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5a1670 | EvProgPrologueInToilet | 0 auto | movie toilet.sfd + demo, James at the sink, flag 32 | ported |
| 2 | 0x5a18d0 | (DC) | 8 | hyper spray (item 12), New Game+ flag 631 | ported |
| 3 | 0x5a18e0 | (DC) | 3 | memo on the sofa, "BAR Neely's", flag 1499 | ported, no map marker |
| 4 | 0x5a1a00 | (DC) | 5 | town map on the wall, flags 1489, 1492, 1493, 1498, 1510, 1512 | ported, no map marker |
| 5 | 0x5a1ce0 | (DC) | 2 | blue gem (item 78), New Game+ flag 723 | ported |

### Stage 3 Stg_observation (0x8ea6e8)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5a0a30 | EvProgLetterFromMary | 0 auto | demo tenbou, flags 517, 1553, 1504 | ported |
| 2 | 0x5a0b80 | EvProgItemInCar | 1 | the town map in the car (flag 24 by the event) | ported |
| 3 | 0x5a0e70 | EvProgNothingTakeInCar | 2 | message 3 | ported |
| 4 | 0x5a0e80 | EvProgHadBetterGetMap | 6 enter | message 2, James walks back 500 | ported |
| 5 | 0x5a0fb0 | EvProgCantGoBack | 7 enter | message 0, James walks back 500 | ported |
| 6 | 0x5a10e0 | EvProgLastScene | 11 auto | Maria ending: mar_isho, end_mria.sfd, credits, ending bit 2 | ported |

### Stage 4 Stg_forest (0x8ea108)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x59f480 | EvProgFirstSaveWell | 11 | red square in the well, flag 5, save menu | ported |
| 2 | 0x5a0740 | EvProgAngelaInGrave | 12..17 enter | movie hakaba.sfd, flag 36 (event), random flag 37 | ported |
| 3 | 0x59f670 | EvProgGraveSureQuiet | 18 | Angela again, flag 37 random | ported |
| 4 | 0x59f790 | EvProgGraveLookingFor | 19 | Angela again, flag 37 random | ported |
| 5 | 0x59fc90 | EvProgGetChainsaw | 20 | chainsaw (item 16), needs flag 12 | ported |
| 6 | 0x59f8b0 | EvProgLastScene | 21 auto | Leave ending: mry_yarinaoshi, ending.sfd, credits, ending bit 1 | ported |
| 7 | 0x59fcd0 | (DC) | 5 | fades out and sets gEventIndex 0xc (mode switch) | not ported |

### Stage 5 Stg_town_east (0x8e8358)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x59d2a0 | EvProgGetWrench | 0 | wrench (item 58), after flag 251 | ported |
| 2 | 0x59d2b0 | (DC) | 1 | wrench note, message 32 | ported |
| 3 | 0x59d4f0 | EvProgShadowOfSomenthing | 2 enter | walk, blood marks, shadow, messages 28 and 29 | ported |
| 4 | 0x59d8f0 | EvProgSandersBack | 3 enter | message 30, James walks back | ported |
| 5 | 0x59da20 | EvProgFirstMonsterStart | 21 | first Lying Figure: plank (13) and radio (19), flag 518 | ported |
| 6 | 0x59dbd0 | EvProgFirstMonsterCorpse | 22 auto | flags 41 and 518 | ported |
| 7 | 0x59dd40 | EvProgFirstMonsterEnd | 18 | demo, James placed | ported |
| 8 | 0x59de50 | EvProgBloodSwamp | 17 | swamp monument, message 4 or 5, flag 628 | ported |
| 9 | 0x59dea0 | EvProgGetApartGateKey | 12 | apartment gate key (26), flag 1511 | ported, tested |
| 10 | 0x59dec0 | EvProgUseApartGateKey | 13 | uses the gate key, flag 48 opens the gate | ported, tested |
| 11 | 0x59dee0 | EvProgLookMemoFalled | 28..33 | the scattered memo pages (flags 50..59, 610, 611) | ported |

### Stage 6 Stg_apart_e1f (0x8e64e0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x59cbf0 | EvProgVomitEddie | 0 | movie gero.sfd + demo, flag 87 | ported |
| 2 | 0x59cdd0 | EvProgVomitEddieAfter | 4 | Eddie again (not a function in the Ghidra project) | ported |
| 3 | 0x59cf00 | EvProgTouristGuideRead | 10 | brochure, messages 1 and 0 | ported |

### Stage 7 Stg_apart_e2f (0x8e5768)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x599f60 | EvProgThreeNameOnWall | 15 | names on the wall of 208 | ported |
| 2 | 0x59a160 | EvProgHintOfClockSet | 24 | clock riddle note, flag 75 or 78 | ported |
| 3 | 0x59c1c0 | EvProgClockTime | 18, 20 | the clock face with its time, flag 77 | ported |
| 4 | 0x59a6a0 | EvProgClockNeedleMove | none | returns 0 on the PC | not ported (unused) |
| 5 | 0x59c4b0 | EvProgUseClockKey | 16, 17 | clock key opens the face (flag 81 then 80), hands puzzle | ported, tested |
| 6 | 0x59a6b0 | EvProgTryMoveClock | 22 | "Will you push the clock?", moves at 9:07..9:13, flag 82 | ported, tested |
| 7 | 0x59aa10 | EvProgUseEmergencyKey | 31 | fire escape key (30), flag 93 | ported |
| 8 | 0x59aa30 | EvProgAnyoneInHole | 13 | the hole in 202, clock key (28), flag 79 | ported, tested |
| 9 | 0x59ad80 | EvProgNooneInHole | 14 | message 3 | ported |
| 10 | 0x59afb0 | EvProgLookDustChute | 10 | trash chute, messages 15 (and 16 on easy) | ported |
| 11 | 0x59b1e0 | EvProgUseCannedJuice | 11 | juice down the chute (the event consumes it), flag 95 | ported |
| 12 | 0x59b300 | EvProgNoFaceCorpse | 7 | demo, flag 71 | ported |
| 13 | 0x59b3c0 | EvProgAnyoneCry | 6 enter | noise, "What was that!?", flag 69 | ported |
| 14 | 0x59b4c0 | EvProgGetLight | 1 | flashlight (18), turns it on, flags 68 and 2457 | ported |
| 15 | 0x59b520 | EvProgGetApart202Key | 9 | key to room 202 (27), flag 72 | ported, tested |
| 16..18 | 0x59b530, 0x59b730, 0x59b930 | EvProgEndHint*Read | 3, 4, 5 | the three ending articles | ported |
| 19 | 0x59bb30 | EvProgUseApart202Key | 34 | uses the 202 key, flag 73 opens 202 | ported, tested |
| 20 | 0x59bb50 | EvProgMonkeyKick206 | 75 | thump from 206 (SE) | ported |

### Stage 8 Stg_apart_e3fw (0x8e4080)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x599ce0 | EvProgLauraKickKey | 0 | question, Laura kicks the key, flag 67 (68 with the handgun) | ported |
| 2 | 0x599f30 | EvProgGetHandgun | 3 | handgun (4), flag 66 (68 after the kick) | ported |

### Stage 9 Stg_apart_e3fe (0x8e3948)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5991f0 | EvProgFirstMeetTrihead | 0 | Pyramid Head behind the bars, flag 91 | ported |
| 2 | 0x599490 | EvProgGetYardKey | 5 | courtyard key (29) | ported |
| 3 | 0x5994a0 | EvProgGetEmergencyKey | 6 | fire escape key (30) | ported |

### Stage 10 Stg_apart_w1f (0x8e2d38)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x598870 | EvProgLookThreeCoin | 7 | desk writing, riddle per level (flag 609, 123, 124 or 125) | ported |
| 2 | 0x598c30 | EvProgSetThreeCoin | 1..3, 6 | coin puzzle: flags 126 + 5 coin + hole, 141, 142 | ported, tested |
| 3 | 0x598180 | EvProgAngelaWithKnife | 10 | movie knife.sfd, Angela's knife (24) | ported |
| 4 | 0x5984a0 | EvProgGetCoinOfPrisoner | 14 | coin [Prisoner] (52) | ported |
| 5 | 0x5984b0 | EvProgGetWhiteChrism | 9 | white chrism (76), New Game+ flag 10 | ported |
| 6 | 0x5984c0 | EvProgGetLyneKey | 4 | Lyne house key (31) once flag 142 is set | ported, tested |
| 7 | 0x5984d0 | EvProgLookFamilyPicture | 16 | the photo | ported |

### Stage 11 Stg_apart_w2f (0x8e2620)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x596c10 | EvProgReadDearTim | 8 | "It's locked.", the note to Tim, flag 1698 | ported |
| 2 | 0x596d80 | EvProgUseLyneKey | 5 | uses the Lyne key, flag 113 | ported |
| 3 | 0x596da0 | EvProgGetApartStairKey | 10 | apartment stairway key (32) | ported |
| 4 | 0x596db0 | EvProgUseApartStairKey | 18 | uses the stairway key, flag 115 | ported |
| 5 | 0x597710 | EvProgWalletInToilet | 3 | question, wallet with the safe code, flag 102 | ported |
| 6 | 0x5979a0 | EvProgWalletOutToilet | 1 | the code again | ported |
| 7 | 0x597b00 | EvProgOpenSafe | 4 | safe dial (flags 103..107), opens with flag 108 | ported, tested |

### Stage 12 Stg_apart_stair (0x8e1e58)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5961d0 | EvProgUseYardKey | 18 | uses the courtyard key, flag 90 | ported |
| 2 | 0x5961f0 | EvProgGetApartMap | 11 | apartment map, flag 25 | ported |
| 3 | 0x596200 | EvProgGetApartWestMap | 12, 13 | west building map, flag 26 | ported |
| 4 | 0x596210 | EvProgGetCannedJuice | 3..6 | canned juice (49) | ported |
| 5 | 0x596220 | EvProgApartBoss | 7 auto | Pyramid Head in the stairwell, flag 529 | ported |
| 6 | 0x5964c0 | EvProgApartBossSiren | 8 auto | siren SE | ported |
| 7 | 0x5964e0 | EvProgApartBossEnd | 9 auto | flags 147 and 148 after four seconds | ported |

### Stage 13 Stg_apart_out (0x8e1590)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5959d0 | EvProgGetCoinOfSnake | 7 | coin [Snake] (50) | ported |
| 2 | 0x5959e0 | EvProgGetCoinOfOldman | 5 | coin [Old Man] (51), magazine, flag 96 | ported |
| 3 | 0x595d80 | EvProgMurderNewsRead | 4 | message 1 | ported |
| 4 | 0x595d90 | EvProgApartEastToWest | 0 auto | fire escape demos and message 4 | ported |
| 5 | 0x595f50 | EvProgApartWestToEast | 2 auto | demo | ported |

### Stage 14 Stg_town_west (0x8df5b8)

The PC table is reordered and extended against the PS2 beta here; names marked "probably" are
matched by behaviour.

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x591fb0 | (none) | 0 enter | Rosewater Park, timed subtitles 111, 112 | ported (subtitles in the panel) |
| 2 | 0x592160 | (none) | 1 | park map board, flags 1530, 1536 | ported, no map marker |
| 3 | 0x592430 | (none) | 2 auto | after Maria's death, subtitles 97..103 | ported |
| 4 | 0x592720 | (none) | 3 enter | "I have to go after Laura", walks back | ported |
| 5 | 0x594850 | probably EvProgLauraOnWall | 28 enter | movie hei.sfd, flag 659 | ported |
| 6 | 0x592830 | EvProgMariaMeetingStandby | 16 enter | flag 519 | ported |
| 7 | 0x5928e0 | EvProgMariaMeeting | 17 enter | movie deai.sfd, Maria joins (flag 15), flags 1505, 1554, 1555 | ported |
| 8 | 0x592c90 | EvProgMariaNotHotel | 18 enter | flag 655 | ported |
| 9 | 0x592e00 | EvProgBowlDemoDataLoad | 20 enter | preload only | ported |
| 10 | 0x592e40 | EvProgMariaFrontBowling1st | 29 | Maria stays (flag 15 cleared) | ported |
| 11 | 0x592fd0 | EvProgMariaFrontBowling2nd | 31..33 enter | flag 658, flag 15 | ported |
| 12 | 0x593100 | probably EvProgLauraCantGo | 35 enter | flag 656, James moved by the demo offset | ported |
| 13 | 0x593300 | EvProgEnemyLoadWestTownStart | 19 enter | flag 153 | ported |
| 14 | 0x593330 | EvProgEnemyLoadWestTownInit | none | returns 1 | ported |
| 15 | 0x593340 | EvProgLadyHoleMake | 21 | "Will you dig?", flag 252 | ported |
| 16 | 0x593570 | EvProgUseOldBronzeKey | 40 | uses the old bronze key (40) | ported |
| 17 | 0x594a90 | EvProgUseWrench | 22 | wrench on the box, old bronze key, flag 258 | ported |
| 18 | 0x5937c0 | EvProgLookMetalBox | 23, 24 | messages 2, 3 or 94 | ported |
| 19 | 0x593800 | EvProgLauraGoHospital | 42 enter | flags 660, 1537 | ported |
| 20 | 0x593940 | EvProgGetSteelPipe | 12 | steel pipe (14) | ported |
| 21 | 0x593980 | EvProgGetLostMemory | 13 | book "Lost Memories" (74), message 11 | ported |
| 22 | 0x593ba0 | EvProgGetDogKey | 9 | dog key (25) | ported |
| 23 | 0x593bb0 | EvProgEndMaria | 46 auto | ending demo | demo hook only |
| 24 | 0x5947e0 | | none | empty function | not ported (unused) |
| 25 | 0x5954b0 | | none | per-frame lights and radio of the stage | not ported (unused by events) |

### Stage 15 Stg_bowling (0x8dd7b0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x591860 | EvProgEddieAndLaura | 0 | demo | ported |
| 2 | 0x591a40 | EvProgEddieEatPizza | 2 enter | flag 548, demo, James placed | ported |
| 3 | 0x591c10 | EvProgEddieEatPizzaAfter | 3..5 | demo, James placed | ported |

### Stage 16 Stg_to_heaven (0x8dd358)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x590f10 | EvProgLauraWentSideRoad | 0 enter | flag 657, demo | ported |
| 2 | 0x591070 | EvProgMariaUnlockHeaven | 3 | demo, flag 166 by the event | ported |
| 3 | 0x5911c0 | EvProgJamesCantGo | 1 | demo | ported |

Stage 17 Stg_heaven_night has no program table (its `ev_prog` points into bss).

### Stage 18 Stg_hospital_1f_f (0x8dcc30)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x590420 | EvProgGetHospitalMap | 4 | hospital map (flag 27) | ported |
| 2 | 0x590430 | EvProgDoctorMemo1st | 7 | the doctor's memo, "Purple Bull" key (35) | ported, tested |
| 3 | 0x590730 | EvProgDoctorMemo2nd | 8 | the memo again | ported |
| 4 | 0x5908c0 | EvProgUseExamKey | 12 | examination room key (33), flag 177 | ported |
| 5 | 0x5908e0 | EvProgReadWhiteBoard | 11 | the pin memo ("T", "X", "Z") | ported |
| 6 | 0x5908f0 | EvProgCardsOfPatient | 6 | patient cards | ported |
| 7 | 0x590ab0 | EvProgHospitalExit | 2 | Maria at the exit (flag 15) | ported |
| 8 | 0x590c20 | EvProgLauraDeceive1st | 0 enter | demo, flag 207 | ported |

### Stage 19 Stg_hospital_2f_f (0x8dbff8)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x58fcb0 | EvProgGetLapisEyeKey | 7 | "Lapis Eye" key (36) | ported |
| 2 | 0x58fcc0 | EvProgReadCarbonNumber | 6 | the typewriter's carbon paper with the box code, flag 613 or 183 | ported |
| 3 | 0x58ff50 | EvProgGetExamKey | 4 | examination room key (33) from the lab jacket | ported |
| 4 | 0x590090 | EvProgNeedleInAnimal | 2 | bent needle (54) | ported |
| 5 | 0x5902a0 | EvProgUseElevatorKey | 37 | elevator key (37), flag 173 | ported |
| 6 | 0x5902c0 | EvProgGetShotgun | 0 | shotgun (6) | ported |

### Stage 20 Stg_hospital_3f_f (0x8db0e0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x58d7e0 | EvProgLostMaria | 1 | Maria rests in the day room, flag 186, random 187 | ported |
| 2 | 0x58d990 | EvProgLostMariaAfter | 2 | talks alternate (187) | ported |
| 3 | 0x58db00 | EvProgGetRoofKey | 15 | roof key (34) | ported |
| 4 | 0x58f090 | EvProgGuruguruNumber | 3 | the wall with the box's four symbols (listed in the panel), flag 192 | ported |
| 5 | 0x58dd70 | EvProgOnlyNeedle | 12 | message 11 | ported |
| 6 | 0x58dc60 | EvProgOnlyHair | 11 | message 12 | ported |
| 7 | 0x58de80 | EvProgInShowerDrain | 13 | the drain, message 8 or 9 | ported |
| 8 | 0x58e0b0 | EvProgFishKey | 10 | hair and needle (87) fish out the elevator key (37), flag 172 | ported |
| 9 | 0x58f2d0 | EvProgBoxWithKey | 6..9 | the box with four locks (see below), the hair (53), flag 171 | ported, tested |
| 10 | 0x58ebd0 | EvProgEmptyBox | 5 | message 6 | ported |
| 11 | 0x58fae0 | EvProgSetPassNumberT | 33 | keypad "T": 7 3 3 5 (easy, normal) or 1 3 2 8 (hard, extra), flag 181 opens the door | ported, tested |
| 12 | 0x58ed40 | EvProgLouiseTakecare | 4 | "Louise, I'll take care of you four ever", flag 191 | ported |
| 13 | 0x58ee40 | EvProgUseElevatorKey | 62 | elevator key (37), flag 173 | ported |
| 14 | 0x58ee60 | EvProgFromRoof | 16 auto | James falls from the roof | ported |

The box (0x58f2d0, cursor code 0x58e600): flags 193 and 195 are the "Purple Bull" and "Lapis Eye"
locks (the use events set 194 / 196 for the key being used, and combined item 86 sets both); 197
is the wheel lock, which opens when the four wheels (0x1f7da6c) match the wall's symbols
(0x1f7da68) and flag 203 is set; 198 is the button lock, which opens when the last four of nine
buttons pressed match the carbon paper's number (0x1f7da72). Leaving with the wheels right but 203
clear turns one wheel away again, as on the PC.

### Stage 21 Stg_hospital_rf_f (0x8da750)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x58d480 | EvProgElevatorButton | 18 | the front elevator (see below) | ported |
| 2 | 0x58cc80 | EvProgUseRoofKey | 4 | roof key (34), flag 199 | ported |
| 3 | 0x58cca0 | EvProgTriheadOnRoof | 2 auto | Pyramid Head on the roof, flag 200 cleared | ported, no damage |
| 4 | 0x58cea0 | EvProgTriheadEntry | 0 enter | puts Pyramid Head on the roof | flags only |
| 5 | 0x58ced0 | EvProgReadDiary | 11, 12 | the diary, flag 204 | ported |
| 6 | 0x58d1c0 | EvProgMetalNetRusty | 10 | message 4 | ported |

Elevators (0x58d480 here, 0x588e60 in stage 26): flags 17, 18, 19 say which floor the car is on,
the buttons set 21, 22, 23 for 1F, 2F, 3F and the stage's auto door events do the ride. The PC
maps its five buttons per floor (0x58c820, 0x5885f0); the panel lists the floors instead. The
front elevator sets flag 176 when it goes to 1F; the otherworld one plays the ride with Maria
once (flag 232, which the quiz needs).

### Stage 22 Stg_hospital_1fw_b (0x8d9cd8)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x58b670 | EvProgLauraDeceive2nd | 1 auto | demo | ported |
| 2 | 0x58b750 | EvProgLauraDeceive3rd | 2 auto | demo, flag 521 | ported |
| 3 | 0x58b910 | EvProgHospitalBack1st | 3 auto | the hospital turns: flags 213, 215, 521 cleared | ported |
| 4 | 0x58ba80 | EvProgHospitalBack2nd | 4 auto | flag 214, James placed | ported |
| 5 | 0x58c4b0 | (DC) | 24 | blue gem in the courtyard, the UFO scene | demo hook only |

### Stage 23 Stg_hospital_1fe_b (0x8d9730)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x58af50 | EvProgMariaKilledAfter | 3 auto | the elevator down, James placed | ported |
| 2 | 0x58b040 | EvProgUseHospitalEnterKey | 5 | hospital lobby key (39), flag 217 | ported |
| 3 | 0x58b060 | EvProgGetHospitalEnterKey | 1 | the Director's map, lobby key (39), flags 251, 1514, 1516, 1524, 1525 | ported, no map marker |
| 4 | 0x58b660 | EvProgLookDirectorMap | 2 | the map again | ported |

### Stage 24 Stg_hospital_2f_b (0x8d90e8)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x58a8a0 | EvProgRefrigeratorOpen | 1 | with Maria: question, lead ring (57), flag 219 | ported |
| 2 | 0x58ac20 | EvProgRefrigeratorNotOpen | 0 | alone: question, "too heavy", flag 1956 | ported |
| 3 | 0x58aed0 | EvProgGetDryCell | 3 | dry cell battery (55) | ported |
| 4 | 0x58aee0 | EvProgGetStoreroomKey | 4 | basement storeroom key (38) | ported |

### Stage 25 Stg_hospital_3f_b (0x8d8b38)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x58a260 | EvProgPictureOfAngel | 1..4 | the lady on the door: copper (56) and lead (57) rings, flags 227, 228 | ported |
| 2 | 0x589790 | EvProgAngelNoOneKnows | 6 | the scrap of paper, flag 231 | ported |
| 3 | 0x58a6e0 | EvProgTryQuizAnswer | 8 | quiz: three rows of three buttons, third, first, third is right (240), else 241; 233 ends it | ported |
| 4 | 0x589f70 | EvProgIncorrectQuizAnswer | 9 | message 6 | ported |

### Stage 26 Stg_hospital_bf_b (0x8d8070)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5881a0 | EvProgMariaMeetAgain | 5 | movie saikai.sfd, flag 15 | ported |
| 2 | 0x5883f0 | EvProgMoveEmptyShelf | 7 | question, the shelf moves, flag 223 | ported |
| 3 | 0x5885c0 | EvProgGetCopperRing | 10 | copper ring (56) | ported |
| 4 | 0x5885d0 | EvProgUseStoreroomKey | 12 | storeroom key (38), flag 222 | ported |
| 5 | 0x588e60 | EvProgElevatorButton | 25 | the otherworld elevator | ported |
| 6 | 0x588a50 | EvProgDirectorBronzeKey | 1..4 | the note about the bronze key, flag 242 | ported |

### Stage 27 Stg_hospital_pass (0x8d77f0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1..5 | 0x587db0, 0x587de0, 0x587e20, 0x587e70, 0x587ed0 | EvProgChaseInHospital1st..5th | 2..6 enter | steer the chaser | flags only |
| 6 | 0x587f10 | EvProgMariaKilled | 7 enter | Maria is killed: flag 522, 15 cleared | ported |

### Stage 28 Stg_society (0x8d75c0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x587c00 | EvProgTriheadPicture | 2 | the painting, flag 262 (not a function in the Ghidra project) | ported |
| 2 | 0x587c10 | EvProgGetGoblet | 5 | obsidian goblet (77), needs flag 10 | ported |

### Stage 29 Stg_delusion_2 (0x8d72a8)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x587920 | EvProgHoleTo3rdLayer | 1 | "Will you jump down?", flag 265 (read from the disassembly) | ported |

### Stage 30 Stg_delusion_3 (0x8d6ce8)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x586040 | EvProgStartBottomOfWell | 0 auto | demo, James placed | ported |
| 2 | 0x586160 | EvProgBreakWellWall | 6 auto | flag 268 | ported |
| 3 | 0x586210 | EvProgGetSpiralKey | 16 | spiral-writing key (41), flags 269, 272 | ported |
| 4 | 0x587470 | EvProgCocRoomLock | 18 | nine panels, three light up; pressing them in order clears 269 | ported |
| 5 | 0x586bb0 | EvProgUseSpiralKey | 21..23 | spiral key, flag 271 | ported |
| 6 | 0x586d40 | EvProgHoleTo4thLayer | 20 | question, flag 274 | ported |
| 7 | 0x586f40 | EvProgUseDryCell | 17 | dry cell for the flashlight, flag 272 cleared | ported |

### Stage 31 Stg_prison_n (0x8d6028)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x584fc0 | EvProgEddieKillHuman | 0 auto | movies korosu_a / korosu_b | ported |
| 2 | 0x585350 | EvProgGetPrisonMap | 2 | prison map (flag 28) | ported |
| 3 | 0x585360 | EvProgGetTabletPig | 3 | tablet of "Gluttonous Pig" (60) | ported |
| 4 | 0x585370 | EvProgGetTabletSeduct | 4 | tablet of "The Seductress" (61) | ported |
| 5 | 0x585380 | EvProgGetRifle | 6 | hunting rifle (8) | ported |
| 6 | 0x585390 | EvProgGetLighter | 7 | lighter (63) | ported |
| 7 | 0x5853a0 | EvProgReadLakeMagazine | 5 | the magazine, flag 279 | ported |
| 8 | 0x5855c0 | EvProgTryOpenFloor | 10..21, 24 | the floor cover: which message depends on 280..282 | ported |
| 9 | 0x585d00 | EvProgSuccessOpenFloor | 22, 23 | horseshoe with wax (96) lifts it, flag 283 | ported |
| 10 | 0x585950 | EvProgHoleTo5thLayer | 9 | question, flag 284 | ported |

### Stage 32 Stg_prison_s (0x8d5218)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5844f0 | EvProgGetTabletOppressor | 0 | tablet of "The Oppressor" (59), flag 288 | ported |
| 2 | 0x584520 | EvProgGetWaxDoll | 1 | wax doll (64) | ported |
| 3 | 0x584530 | EvProgGetHorseshoe | 10 | horseshoe (62), flag 2037 | ported |
| 4 | 0x584980 | EvProgTabletSetScaffold | 2..9 | tablets into the gallows: 301..303, all three set 304 | ported |
| 5..7 | 0x584790, 0x5847a0, 0x5847b0 | (paintings) | 53..55 | "Woman in Flight", "436 People", "Burning Man" | ported |

### Stage 33 Stg_prison_bf (0x8d4898)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x583c60 | EvProg5thLayerStart | 0 auto | demo, James placed | ported |
| 2 | 0x583db0 | EvProgHoleTo6thLayer | 3 | question, flag 306 | ported |
| 3 | 0x583fb0 | EvProg6thLayerStart | 1 auto | demo, James placed | ported |
| 4 | 0x584100 | EvProgDoorOpen6Layer | 13 | the door bursts open, flag 309 | ported |
| 5 | 0x5841b0 | EvProgHoleTo7thLayer | 15 | question, flag 310 | ported |

### Stage 34 Stg_labyrinth_w (0x8d3efc)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x581cc0 | EvProg7thLayerStart | 1 auto | demo, James placed | ported |
| 2 | 0x581e10 | EvProgGetGreatKnife | 4 | great knife (15) | ported |
| 3 | 0x581e20 | EvProgControlCube | 5 | the cube room: two rings in flags 317..320, the table at 0x8d41a4 opens 325 / 326 | ported |
| 4 | 0x582750 | EvProgMariaIsAlive | 6 enter | movie rouya.sfd | ported |
| 5 | 0x5829c0 | EvProgAfterMariaIsAlive | 7 | returns 1 | ported |
| 6 | 0x5829d0 | EvProgGetWireCutter | 8 | wire cutter (65) | ported |
| 7 | 0x5829e0 | EvProgUseWireCutter | 9 | demo, flag 330 by the event | ported |

### Stage 35 Stg_labyrinth_e (0x8d2cb0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x580bf0 | EvProgScreamOfAngela | 1 enter | returns 1 | ported |
| 2 | 0x580c00 | EvProgPapaAttackAngela | 2 enter | flag 606, demo | ported |
| 3 | 0x580e30 | EvProgAngelaAttackPapa | 3 auto | Abstract Daddy, James placed | ported |
| 4 | 0x581090 | (none) | 4 auto | after the fight | ported |
| 5 | 0x581260 | EvProgNewsOfPapa | 5, 6 | the newspaper | ported |

### Stage 36 Stg_labyrinth_n (0x8d1b90)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x57f730 | EvProgHangingCorpse | 0, 3, 6, 9, 12, 15 | a corpse's crime, wording per riddle level | ported |
| 2 | 0x57f950 | EvProgHangRope | 24..29 | pull a rope; the innocent man's rope sets 343 + j | ported |
| 3 | 0x57fc80 | EvProgReadHangHint | 30 | the hint for the riddle level (361..363, 612) | ported |
| 4 | 0x57fd20 | EvProgReadPullOnce | 31 | message 14, flag 364 | ported |
| 5 | 0x57fd30 | EvProgGetFalseChargeKey | 1, 4, .., 16 | key of the Persecuted (42), flag 366 | ported |
| 6 | 0x57fd40 | EvProgCanNotRotate | 33 | message 17 | ported |
| 7 | 0x57fd50 | EvProgUseFalseChargeKey | 32 | the manacles, flag 367 | ported |
| 8 | 0x5801c0 | EvProgMariaRedeath | 34 | flag 524, demo | ported |
| 9 | 0x580330 | EvProgHoleTo9thLayer | 52 | question, flag 372 | ported |

The hangman (0x57f730, 0x57f950): corpse k's crime is (0x1f7da78 / 6^k) % 6, a permutation FlagInit
makes; crime 5 is the innocent man, and each riddle level words the six crimes differently (the
table is in `prog_prison.c`).

### Stage 37 Stg_eddie_boss (0x8cfa5c)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1..4 | 0x57ef10, 0x57f060, 0x57f150, 0x57f2c0 | EvProgEddieMurderer1st..4th | 0..3 | Eddie's scenes, James placed, flag 626 at the end | ported |

### Stage 38 Stg_lake (0x8cf598)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x57dd20 | EvProgEmbarkBoat | 2 | the boat: flag 654, demo, then straight to the disembark scene (the port has no rowing) and flag 381 | ported |
| 2 | 0x57de40 | EvProgDisembarkBoat | 3 enter | demo, James on the hotel pier | ported |
| 3 | 0x57df80 | EvProgGetLittleMermaid | 4 | "Little Mermaid" music box (item 67) | ported |
| 4 | 0x57df90 | EvProgEndRebirth | 7 auto | Rebirth ending: end_rbrt.sfd, credits, ending bit 8 | ported |
| 5 | 0x57e9e0 | (DC) | 11 (use item 78) | the blue gem at the lake, UFO scene | ported (demo hook) |

### Stage 39 Stg_hotel_bf_f (0x8cf198)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x57d530 | EvProgGetThinner | 0 | thinner (item 66) | ported |
| 2 | 0x57d540 | EvProgGetBarKey | 1 | bar key (item 46) | ported |
| 3 | 0x57d550 | EvProgNoLabelCan | 3 | the can with no label: flag 2085, still, message 2 | ported |
| 4 | 0x57d570 | EvProgGetLightbulb | 4 (use item 70) | can opener on the can: demo, still, the light bulb (item 71) | ported |
| 5 | 0x57da50 | EvProgUseLightbulb | 9 (use item 71) | flag 386, bulb used, SE 0x4dc1 | ported |
| 6 | 0x57da80 | EvProgUseBarKey | 6 | bar key used | ported |

### Stage 40 Stg_hotel_1f_f (0x8ce608)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x57b580 | EvProgGetHotelGuestMap | 5 | hotel map 3F (flag 30) | ported |
| 2 | 0x57b590 | EvProgGetHotelEmployeeMap | 6 | employee map 1F (flag 31) | ported |
| 3 | 0x57b5a0 | EvProgGetHotel312Key | 7 | room 312 key (item 43) | ported |
| 4 | 0x57b5b0 | EvProgReadReceptionMemo | 8 | the receipt still, message 2 | ported |
| 5 | 0x57c470 | EvProgOrgel | 11..26 | the music box: put boxes down, take them back, read the plates, play it; flag 472 when the order is right | ported |
| 6 | 0x57bcc0 | EvProgGetHotelStairKey | 9 | stairway key (item 48) out of the music box, flag 2113 | ported |
| 7 | 0x57d190 | EvProgGetVideoTape | 29 | the videotape (item 23) and the can opener (item 70) | ported |
| 8 | 0x57be80 | EvProgNothingInSafe | 30 | still, message 0xf | ported |
| 9 | 0x57be90 | EvProgGetSnowWhite | 31 | "Snow White" music box (item 69) | ported |
| 10 | 0x57bea0 | EvProgGetFishKey | 4 | "Fish" key (item 47) | ported |
| 11 | 0x57beb0 | EvProgLauraPlayPiano | 2, 3 enter | Laura: the voice and piano_lau_b, her letter on screen (message 0x2e), piano_lau_e, then the letter (item 22); flags 636, 528, 383 | ported |
| 12 | 0x57c280 | (none) | 1 enter | returns 1 (flag 634) | ported |

The music box's seats: James looks at seat s (flags 448..450, the third seat when neither of the
first two is set); box b (Snow White, Little Mermaid, Cinderella) is put on seat s with 451 + 3s +
b from the use event; 460 + 4s says a box is there and 461 + 4s + b which one. Easy riddles want
any box on every seat, normal wants Little Mermaid, Snow White, Cinderella, and hard and extra want
Snow White, Little Mermaid, Cinderella.

### Stage 41 Stg_hotel_2f_f (0x8ccf38)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x579220 | EvProgItemPutForShelf | (item menu) | puts the chosen item on the shelf | done by program 6's panel |
| 2 | 0x579420 | EvProgUseHotel204Key | 27 | key to 204 used | ported |
| 3 | 0x579440 | EvProgUseHotelElevatorKey | 53 | employee elevator key used | ported |
| 4 | 0x57a790 | EvProgElvatorButton | 12 | the employee elevator's six buttons; the third goes down (flag 21) unless the buzzer (401) is on | ported |
| 5 | 0x579500 | EvProgGetBaggageInShelf | 16 | takes everything back off the shelf | ported |
| 6 | 0x5796e0 | EvProgPutBaggageForShelf | 17 | "Put an item on the shelf?", then a panel of the items that weigh (flag 416 while open) | ported |
| 7 | 0x57ac90 | EvProgUseFishKey | 7 (use item 47) | the bag opens, key to 204 (item 44) | ported |
| 8 | 0x579ab0 | EvProgFishCaseClose | 8 | bag still, message 2, flag 2189 | ported |
| 9 | 0x579ae0 | EvProgFishCaseOpen | 9 | open bag still, message 3 | ported |
| 10 | 0x57afc0 | EvProgTrunkUnlockChallenge | 5 | the trunk's four letter drums (0x1f7da7c..f); flag 413 opens it | ported |
| 11 | 0x57b1f0 | EvProgPictureWithMarker | 3 (use item 66), 4 | the photo; the thinner takes the marker off and shows the trunk's word | ported |
| 12 | 0x57a310 | EvProgGetElvHallKey | 1 | employee elevator key (item 45) | ported |
| 13 | 0x57a320 | EvProgGetCinderella | 6 | "Cinderella" music box (item 68) from the trunk | ported |

The shelf (table 0x8cd158) keeps a flag per item (417..445, 730) and the counts of the ammo and
health items in 0x1f7d9c0..0x1f7d9ca, which the port keeps in the flag block as the PC does. The
weight check sets 400 when James carries none of the heavy items (weapons, ammo, health items,
three of the Rebirth items and the blue gem). The trunk's word is one of 19 ("open", "damn", "hell", ...),
picked by FlagInit's flags 406..410; EvStageInit puts each drum one to seven letters off.

### Stage 42 Stg_hotel_3f_f (0x8cbdd0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x577e80 | EvProgUseHotelStairKey | 6 | flag 550, stairway key used, SE 0x4980 | ported |
| 2 | 0x577f80 | EvProgUseHotel312Key | 10 | room 312 key used | ported |
| 3 | 0x577fa0 | EvProgVideoReplay | 0 (use item 23) | the videotape: murder.sfd, flags 475, 625, 527, 627 | ported |
| 4 | 0x578440 | (none) | none | returns 1 | ported |
| 5 | 0x578e00 | (DC) | 21 (use item 78) | UFO ending in 312: flag 727, ending bit 0x20 | ported |

### Stage 43 Stg_hotel_bf_b (0x8cb840)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5777f0 | EvProgOpenElevatorWater | 0 | the flooded elevator: the fourth button drains the water (478, 501, 502, 2219, 479), the fifth sets 22 and the door event takes James up | ported |
| 2 | 0x5775f0 | (none) | 2 enter | returns 1 (flag 480) | ported |

### Stage 44 Stg_hotel_1f_b (0x8cad68)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x576050 | EvProgTriheadBattle | 13 | the two Pyramid Heads, Mary's letter is gone | ported (no fight) |
| 2 | 0x576200 | EvProgTriheadDead | 8 auto | their end | ported |
| 3 | 0x576370 | EvProgGetEggRustColored | 9 | rust-colored egg (item 72) | ported |
| 4 | 0x576440 | EvProgGetEggScarlet | 11 | scarlet egg (item 73) | ported |
| 5 | 0x576a90 | EvProgEggHoleOnDoor | 20..28 | the two doors' egg hollows | ported |
| 6 | 0x576660 | (ending decision) | 4 | sets 618, 619, 620 or 621 (see "The ending score") | ported |

### Stage 45 Stg_hotel_2f_b (0x8ca5d0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x575b80 | EvProgNotifyDeath | 0 | the doctor's tape | ported (demo hook) |
| 2 | 0x575c70 | EvProgGetCrimsonCeremony | 2 | "Crimson Ceremony" (item 75, New Game+), its still | ported |

### Stage 46 Stg_hotel_3f_b (0x8c9e88)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x575ae0 | EvProgUseDogKey | 8 | the dog key (item 25); flag 516 sends James to the dog ending | ported |

### Stage 47 Stg_hotel_fire (0x8c99e0)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x575630 | EvProgAngelaInFire | 0 auto | Angela in the fire | ported (demo hook) |
| 2 | 0x575750 | EvProgFireDamage | 1 enter | 5% of James' maximum health | ported |

### Stages 48..51: the endings (code in the main exe)

| stage | # | PC | what it does | status |
|---|---|---|---|---|
| 48 Stg_end_recovery (Leave) | 1..4 | 0x43fe30, 0x4400c0, 0x440210, 0x440230 | the last corridor (flag 531), Mary's fall, the room change, the scene after the fight; the ending goes on in the forest (stage 4 program 6) | ported |
| 49 Stg_end_maria | 1, 2 | 0x440c80, 0x440f40 | the same corridor and fall, flag 33 after the first; the ending goes on at the observation deck (stage 3 program 6) | ported |
| 50 Stg_end_suicide (In Water) | 1..4 | 0x43eac0, 0x43ed60, 0x43eeb0, 0x43eee0 | as 48, then water.sfd, end_scde.sfd, credits, ending bit 4 | ported |
| 51 Stg_end_rebirth | 1, 2 | 0x574860, 0x574b00 | as 49; the ending goes on at the lake (stage 38 program 4) | ported |

Stage 3 program 6 (end_mria.sfd, ending bit 2) and stage 4 program 6 (ending.sfd, ending bit 1)
now call `game_end` too.

### Stage 52 Stg_end_dog (0x8c8c0c)

| # | PC | PS2 name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x574600 | EvProgEndingDog | 0 auto | end_dog.sfd, credits, flag 622, ending bit 0x10 | ported |

### Stage 53, Born From a Wish: Heaven's Night (0x8c89d8)

| # | PC | name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x5740c0 | (opening) | 0 auto | the notice (message 16) with Yes / No, No goes back to the title; the opening scene, the player placed in th09 | ported |
| 2 | 0x574590 | (Chinese cleaver) | 1 | Chinese cleaver (item 17) | ported |

### Stage 54 (0x8c7c60)

| # | PC | name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x573a60 | (ending) | 2 auto (flag 721) | end_wish.sfd, credits, flag 728, ending bit 0x40 | ported |

### Stage 55, the Baldwin mansion (0x8c6d10)

| # | PC | name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x571cb0 | (map) | 7 | Baldwin house map, message 0x48 | ported |
| 2 | 0x571cc0 | (chimney) | 10 | the fireplace still, "Climb the ladder?" (flag 674) | ported |
| 3 | 0x571ea0 | (chimney, down) | 70 | "Go down the ladder?" (flag 675) | ported |
| 4 | 0x571f30 | (black board) | 64 | black board (item 80) | ported |
| 5 | 0x571f40 | (red board) | 65 | red board (item 81) | ported |
| 6 | 0x572f10 | (tombstone) | 66..69 | the three boards with holes; when they line up the Acacia key (item 82), flag 704 | ported |
| 7 | 0x5725f0 | (Lost Memories) | 57 | a sound, the book (message 0x50) | ported |
| 8 | 0x572800 | (Ernest, the card) | 53 | Maria gives Ernest the birthday card (item 84 lost) | ported (demo hook) |
| 9 | 0x572920 | (Ernest) | 54 | Ernest | ported (demo hook) |
| 10 | 0x572a00 | (Ernest, the liquid) | 55 | the white liquid scene, flag 722 | ported (demo hook) |

The tombstone: per board b (white, black, red) 678 + 9b means it is being put in, 679 + 9b + s that
it sits in slot s and 682 + 9b + r that it was turned r (as it is, right, 180 degrees, left). The
first board goes in as it is; the others ask which way (message 0x5b, four answers). The holes line
up when the white and black boards are turned the same way and the red one a quarter turn back.

### Stage 56, the Baldwin mansion upstairs (0x8c5f60)

| # | PC | name | events | what it does | status |
|---|---|---|---|---|---|
| 1 | 0x571120 | (Ernest 1) | 10 | Ernest behind the door, flag 2436 | ported (demo hook) |
| 2 | 0x571220 | (Ernest 2) | 11 | Ernest again | ported (demo hook) |
| 3 | 0x5712f0 | (white board) | 32 | white board (item 79) | ported |
| 4 | 0x571300 | (Acacia key) | 33 | Acacia key used | ported |
| 5 | 0x571320 | (matches) | 45 | matches (item 83) | ported |
| 6 | 0x571330 | (candle) | 53 (use item 83) | SE 0x4293, flag 710, the player by the candle | ported |
| 7 | 0x571370 | (birthday card) | 54 | still, message 0x24, birthday card (item 84) | ported |
| 8 | 0x5715e0 | (calendar) | 18 | a sound, message 0x26 | ported |
| 9, 10 | 0x5717f0, 0x571810 | (sounds) | 14, 15 enter | SE 0x428d, 0x428e | ported |
| 11, 12 | 0x571860, 0x571880 | (BGM) | 55, 56 enter | BGM changes, left to bgm.c | ported (no-op) |
| 13 | 0x5718a0 | (music) | 39 | BGM and a fixed camera for three seconds | ported (wait only) |

### Stages 57 and 58 (encrypted on the PC)

The code of these two tables (0x570e40..0x571000) is encrypted in the exe (SecuROM), so it cannot
be read statically. The programs are reconstructed from their events and the stages' messages:

| stage | # | PC | events | reconstruction | status |
|---|---|---|---|---|---|
| 57 | 1 | 0x570f70 | 7, 8 (two look events a few units apart, ma77) | message 0 (Amy's grave plate) or 1 (the "Keep out" sign), whichever event point is nearer | reconstructed |
| 58 | 1 | 0x570e40 | 34, 35 (ap108, flag 26) | west building map, message 4 | reconstructed |
| 58 | 2 | 0x570e50 | 17 (ap96) | the locked door with Tim's note, message 3 | reconstructed |
| 58 | 3 | 0x570ef0 | 6 (ap76, flag 720) | the white liquid (item 85), message 5 | reconstructed |

## Testing

`debug.txt` on the SD takes, besides `spawn`: `flag <n> <0|1>`, `item <id> <count>`,
`riddle <0..3>`, `action <0..3>`, `clock <minutes>` (time on the 208 clock),
`safe <a> <b> <c> <d>` (the safe code as the wallet shows it) and `box <code> <symbols>` (the
hospital box's button code and wall symbols, four digits 1..9 each; the wheels are set to match).
They apply after `spawn`. `stage <n>` gives the spawn its stage when the map belongs to several
(th01 is stage 17 for James and 53 in Born From a Wish), `wish` sets up the Born From a Wish flags
and `trunk <0..18>` picks the hotel trunk's word; `clear <levels> <endings> <clears>` sets the
clear data. The debug HUD has a `prog` line with the running
program, its step and whether James is locked.

Checked in Azahar (strict settings) with the test copy in `.progtest/`: the opening restroom
program on New Game, the 202 key in 208 with the item in the inventory, the 202 door going from
"It's locked." to "I used the key to room 202." and opening, the gate key pickup and the gate
opening into the apartments, the clock key and the hands (9:05 to 9:10), the push question, the
hole question and the clock key, the coin puzzle solved for the normal riddle and the Lyne key,
the safe dial opening with a set code, and the car park walk-back on the observation deck.

Second batch: the "Purple Bull" key from the doctor's memo (the still, five pages of text, the
item in the inventory), the 3F keypad "T" with its still, flag 181 and the door behind it opening
into the next room, and the box in the day room opening with both keys, the wheels and the buttons
and giving the hair.

Third batch: the key to room 312 in the hotel office (message and the key's description), the
music box solved for the normal riddle (Snow White put on the second seat, "Play the music box?",
then the stairway key from the next look at the box), the dog key in 3F of the other hotel taking
James to stage 52 where end_dog.sfd and the credits play and the game returns to the title, and
Born From a Wish from the title (stage 53 in th09, the notice and the Yes / No).

Fourth batch: the map in the hotel 1F, zoomed in on James in the lobby with the music box mark,
zoomed out, and the floor above; the Wood Side 1F map with two marks; "It's too dark to read the
map." with the flashlight off and "I don't have a map of this area." without the map flag; the
memo list with the wallet memo (the safe code put in) and the carbon paper (the box code put in);
the dog ending writing `clear.dat` (normal levels, the dog bit, one clear); and the title with the
endings seen and the levels screen offering the extra riddle level.

Fifth batch: the clock in 208 at 9:05 with its hands drawn and "The clock is stopped at 9:05 and 15
seconds." from the inserted values; the safe dial, the desk coins in three holes, the hospital box
with its key locks, wheel panel and button panel, the wall symbols 3579, the trunk's drums and the
tombstone's three boards (one turned right, one left) and key; the hospital Director's map marking
the east map ("letter wrench" fading in) and then the west map (Rosewater Park, the Historical
Society); and the results screen after the dog ending.

Sixth batch: the videotape and the can opener drawn in the office safe, the photo with the trunk's
word, frame and marker, Laura's letter after the piano scene and before her second scene, and the
autotest below over every door.

Seventh batch: the results screen in the PC's layout with the PC's font (James' game and Born From
a Wish), the hospital elevator's button lighting up for 1F with the car on 2F, the manacles fading
open after the key of the Persecuted and the kaiten scene after it, the music box with a box in the
wrong seat playing org_a, and a message and the piano_lau_e subtitles on the top screen with the
"Messages" option on.

Eighth batch: the secret items on the results screen ("1(+3)" after the chainsaw, the hyper spray,
the dog key, Mary's photo and one more item), the clear data question and slot screen writing an
empty slot and, after asking, over clear data, Continue on a clear data save opening the New Game
levels with the endings it carried listed on the title, the memo overlays of the tablets, the
rings, the hair, the safe's dial, the wall symbols, the trunk's word and the tombstone's board,
and the music box drawn in the hotel lobby.

Ninth batch: the figure boxes on the music box's seats moving with the tune (posed at 0, 5 and 14
seconds), the town's vans (one placed with turns of pi about x and z, one without, both upright)
and street furniture, the hotel lobby with its stage models, and the save screen with the PC's
picture and texts (the file row, the overwrite question, "Save completed.").

## Autotest

`autotest warps [first] [count]` in `debug.txt` walks through every door and warp destination of
the event data (the EV_WARP results of the door records of stages 1..63, 570 of them), starting at
index `first`. For each one it loads the stage, applies the warp the way a door does
(`apply_warp`, `goto_map`, the room load), runs 8 frames with no input and events paused, and writes
a line to `sdmc:/3ds/sh23ds/autotest.txt`, flushed after each half, so the last line shows where a
crash or hang happened:

    220 st22 ev4 -> st22 hp170 (-60250 -0 -300000) | room hp170 loaded 1 col 1 floor 1 moved 7052
        roads 159 type 0 miss 0 linear 87768K heap 2171K load 4233ms

That is the index, the source stage and event, the destination stage, map and position, then the
room James ended up in (`WRONG` if it is not the record's map), whether the room and its collision
loaded, whether there is a floor under James, how far the spawn moved him, the camera road count
and type, missing textures, free linear memory, heap in use and the load time. The run ends with
`# done, next N`. With `first` greater than 0 the file is appended to, so a long run can be split
into chunks.

Run over all 570 destinations in Azahar (summary in `.progtest/autotest_summary.txt` of the test
copy): no crash or hang, every room loaded with its textures, a camera road set and, except for one
case, collision and a floor under James. What stood out:

* A warp with no map (st51 ev8, the Rebirth ending going back to the lake, stage 38) left James in
  the previous room. `apply_warp` now takes the first map of the stage from `rooms.txt`
  (`ev_map_of_stage`) and the room whose bounds hold the position, which is cd11 on the lake. The
  lake has no collision on the PC either (no cd10 / cd11 `.cld`; the boat moves freely), so that
  is the one destination without a floor.
* st46 ev7 goes to stage 52, the dog ending, which has no rooms; its program plays the movie and
  returns to the title, which the autotest does not run.
* st33 ev14 and st34 ev21 put James on the edge of the ps66 tile, so streaming makes ps67 the
  primary room a few frames later, as intended.
* st22 ev4 points into hp170, the hospital elevator car, at a spot 7 m east of the car's floor
  (outside every floor; the room's mesh reaches that far). The spawn puts James on the car's floor,
  where st22 ev20 lands as well. This looks like a leftover in the PC's data.
* 27 other doors push James 30..172 units off the record's point, which is the wall push-out next
  to the door.
* Loads take 1.4 s on average in the emulator, up to 7.5 s in the town (cc) and 4..5 s in the
  hospital, with 1.9..2.9 MB of heap in use.

## Open questions

* Overlay sprites are drawn for fifteen stills and seven memos (see Pictures and Memo list), and
  most of the PC's fades of the overlay parts are shown as on / off (the elevator button
  and the manacles fade). The quiz buttons' sprites were not compared with the PC.
* Born From a Wish runs with James' model and moves; Maria's model, motions and weapons are not
  converted. Her escort in James' game is missing as well, so the "near / far from Maria" ending
  counters and the damage she takes stay 0, which tilts the ending decision away from Maria.
* enemy.c calls `prog_score_kill`, which does not know the weapon, so every kill counts as beaten
  on the results screen; calling `prog_stat_kill(attack id)` there instead (it also counts for the
  ending) splits shot and beaten kills as the PC does. The boat stage is not tracked (0, as on the
  PC without a boat ride).
* The lake has no boat: the embark program goes straight to the disembark scene on the hotel pier.
* The hotel elevators, the trunk and the tombstone take the D-pad for the PC's cursor; their
  layouts (which button is which on the still) follow the PC's button tables but were not checked
  against the pictures.
* Stages 57 and 58 are reconstructed (see above); if the decrypted code is ever available, check
  which message the two look events in ma77 should give.
* The port has one save slot, so the clear data save goes to `save.dat` (after asking when a game
  is saved there). The save screen has the PC's picture, title and texts but not its file list
  details (the save icons `si_*.xbx`, place names, play time per file) nor the memory unit pages.
  The results' fade in and out is not done. Messages on the top screen
  all use the default place since the pack's message text has lost the PC's position words, and
  the full-width and accented characters of other languages are not in the pack's text either.
* The quiz in the otherworld 3F (0x589820) reads its questions from the still; the buttons are
  mapped as on the PC (rows top to bottom, buttons left to right), which was not checked against
  the picture's layout. The same goes for the cube room's two rings and the panel lock's grid.
* The hospital chase (stage 27) and Pyramid Head on the roof only keep their flags; the enemy code
  needs them later (0x4a8750 steers the chaser, 0x58cca0 also hurts James).
* "There are a total of <e0> locks." (stage 20) and "I put <e0> in the shelf." (stage 41) have no
  MesSetInsert caller in the PC and no event shows them; they are left unused there too.
* The music box plays org_b for the right order and org_a for a wrong one, as the PC loads them into
  the same PlayInfo (0x8cea6c, `EXTRA_SCRIPTS` in `tools/sh2conv/demos.py`). The turns of the box's
  parts, the figure boxes and the key (0x57b5c0, see Stage models) are taken from the decompile
  without a PC capture to compare with.
* Stage models load from the SD card the first time they come near (one a frame); on a console
  that can show as a short hitch when several new ones come into range at once, in the town most.
* The PC's option to open the map zoomed out is not ported. The arrow is drawn, not the PC's
  sprite. The room table depends on `bgm.bin` (`bgm_room_id`), so without it there is no map.
* PC 0x1dbec05 multiplies ammo boxes in ItemGet (the Extra Options "Bullet Adjust"). Pickups and
  `p_item_get_now` both go through `ev_item_get_amount()`, so programs that hand out ammo get the
  multiplier as well.
* The scripted walk has no walking animation and goes straight, without the PC's path code.
* `EvSubItemGet` also removes the pickup model and plays James' bending motion; the port has no
  item models in rooms yet.
* Programs that only move characters, cameras or lights (Maria following, enemy removal in
  0x599c30, the stage's per-frame function) keep only their flags; Maria and the enemies are not in
  the port yet.
* Where the PC lets the player skip a demo with the skip button (`ev_skip`), programs sometimes
  clear `ev_prog_flag_set`; the demo hook has no skip yet, so those branches are not taken.
* The letters (`letter_msg`, the item "Mary's letter") still read as their item description; the
  letter files are the subtitles of the opening and ending voice-overs.
