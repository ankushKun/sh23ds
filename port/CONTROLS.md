# Controls

James is controlled the PC way. The Options' Control Type is the PC's: Rotational Style (the "3D"
type: the circle pad turns him and walks him forward or back) or Directional Style (the "2D" type,
see below). The buttons are actions, and the actions are bound per layout. The New 3DS layout uses ZL, ZR and the C-Stick on top
of the buttons every 3DS has; the Old 3DS layout gets by with L, R, A, B, X, Y, the D-Pad, the
Circle Pad, START, SELECT and the touch screen. Which layout is used follows the console unless the
player picks one (`profile.c`, `controls_layout()`). Every action can be rebound on the Buttons
page of the Options' Controls tab (`controls.c`, drawn in the Options screen's menu slot through
`controls_menu_slot(TOUI_OPTIONS)`), separately for each layout; the bindings are kept in
`controls.txt` next to `options.txt`.

## Default layouts

| Action | New 3DS | Old 3DS |
| --- | --- | --- |
| Walk, turn | Circle Pad | Circle Pad |
| Action (check, open, pick up, attack while aiming) | A | A |
| Run (hold; Walk/Run Control has Toggle, Analog and Run too) | B | B |
| Aim (hold) | R | R |
| Look (hold: the PC's search view) | ZL | L |
| Next weapon | L | D-Pad up |
| Flashlight | X | X |
| Items | Y | Y |
| Quick turn (180 degrees) | ZR | D-Pad down |
| Strafe left / right | none (bindable) | none (bindable) |
| Target left / right (while aiming) | C-Stick left / right | none (bindable) |
| Map | tap the map on the touch screen, or the pause menu | the same |
| Brightness | the Options (Display tab) | the same |
| Pause | START | START |
| Debug page | SELECT twice | SELECT twice |

The circle pad walks and turns James. On the New 3DS the C-Stick moves the lock to the next enemy
left or right while James aims (the PC has two keys of its own for that, 0x54f100), and turns the
behind camera otherwise. The D-Pad walks him as the PC's direction keys do (0x52e110): full turns,
and the speed ramps up as it does for the keys (walk to 1.5 m/s at 2.5 m/s/s, back to 1.2 after a
stop), in each direction no action is bound to (`main.c` `dpad_walk`): all four on the New
3DS, left and right on the Old 3DS, whose up and down keep the next weapon and the quick turn. With
Rotational Style the Old 3DS defaults leave up and down free too, since the PC's 3D type walks James
with the direction keys (0x52e110): the next weapon is then a tap on the touch screen's weapon and the
quick turn both strafe buttons together (0x54b290), unless the player binds them. The
circle pad wins while it is pushed, and menus keep the D-Pad for moving through rows. Strafing and
the map can still be bound to the D-Pad or the C-Stick on the Buttons page. The Control Type defaults to Directional
Style; an `options.txt` written before that (no `optver` line) is moved to it once.

Console test combos (`main.c`): SELECT held with R steps the fog test (`chr.c`, port/TESTING.md),
and SELECT held with L the map test. (An earlier SELECT + L frame capture froze the console and was
taken out.)

Pressing both strafe buttons together also turns James round, as on the PC (0x54b290). Menus keep
their own keys whatever the bindings: the D-Pad or the Circle Pad moves, A takes, B goes back, START
closes. On the title's menus START chooses as A does, and on the start-up logos it skips them; EXIT
is the one way out of the game from the title. In menus (`main.c` `menu_keys`, on every frame where
James is not steered: the title, pause, items, map and memos, save files, options, questions, puzzles,
results) the Circle Pad counts by its stronger axis, past a dead zone, and a direction held on it or
the D-Pad repeats after 0.4 s, ten times a second. A direction already held when a menu opens waits
until it is let go, so pausing on the move does not move the menu; the Buttons page takes keys as
they are.

## The Buttons page

Pause, Options, the Controls tab, then "Buttons". Up and down choose an action, A waits for the new button (START keeps the
old one), Y puts that action back to its default and "Defaults" resets the whole layout. A button
does one thing: binding it to an action takes it off the action that had it. The page edits the
layout in use, so an Old 3DS player never sees ZL, ZR or the C-Stick offered.

## Toggle running

Walk/Run Control's Toggle value is the Enhanced Edition's toggle sprint (`main.c` `player_update`):
a press of the run button starts a run and the next ends it, and the run ends by itself when James
stops. With Rotational Style he stops when the stick no longer pushes him ahead (the stick's y at 0
or below); with Directional Style and the behind camera the stick's way is his way, so only when the
stick is let go. Until 2026-10-29 the port ended it on the stick's y in every style, which cut any
run sideways or towards the camera short.

## Reading the circle pad (main.c player_update, move.c)

The pad reads about 156 at its rim, taken as the PC stick's 127. Rest noise under 12 % of the rim
reads as centred (the PC's DirectInput dead zone is in the encrypted 0x56e3b0, so this one is the
port's). Then 0x52e110: with Rotational Style an axis tilted less than half the other (at most 64 of
127) reads 0, and once that half passes 31 the turn counts from there; the turn is full at 85 and
the forward tilt at 100. Directional Style skips the cross and takes the length of both (0x52dd20).
The Analog Walk/Run Control runs past 110 of 127 while the stick is in use, and the run key still
counts with the D-Pad (0x52e110).

## Directional Style

Directional Style is the PC's 2D type (0x547bf0, 0x546760, 0x548bf0): the direction is taken from the
camera James saw when the stick was pushed and held across camera cuts until the stick turns more
than 30 degrees or is let go, he turns towards it at 20 x 0.3235 of the gap a second, and he walks
or runs along the way he faces as fast as the stick is tilted. The camera behaves as the PC's does
for that type. (The port's earlier camera-relative mode is gone: it turned at a fixed 7 rad/s to the
current camera's direction and spun James round at camera cuts, which Directional Style is built
to avoid, so it added nothing.)

Aiming, he turns to the stick; locked on, the stick's direction against his facing walks, backs or
strafes him that way (0x547bf0, 0x548bf0). The behind camera keeps the Rotational reading while aiming.

## James' head (main.c head_update, combat_look_target)

As on the PC (0x535720), James' head turns to the locked target, or else to the nearest enemy,
person (Maria) or item lying about that is in his sight (the look and feel areas below) and not
hidden by the room. With the look key held it follows the circle pad instead. The angles ease
towards their aim at 20 x 0.3235 of the gap a second (0x5343e0) and stop at 0.8 radians to either
side, 0.5 up and 0.4 down; the neck takes a fifth and the head the rest. It rests while he relaxes
or looks round, and while he is kicking, hurt, held or dying.

## Aiming (combat.c)

The weapon comes up only while James stands, idles or turns on the spot: walking, running, backing
off and the quick turn go on until they end (the upper rows of 0x6cf7f8 that allow status 0x1a,
0x557100). Holding Aim locks on to a target as it comes up (0x533ca0): the nearest enemy in his sight
with nothing of the room in between, one on its feet before one on the floor (0x542560). With a lock
James turns to it by 0.3235 x 15 of the gap a second (0x5568c0) while he aims and attacks, and the
stick turns him not at all; without one the stick turns him in the stance at 2.4 rad/s, 3.6 with run
held (0x54dcc0). The handgun and Maria's revolver lock again with each shot if the lock was lost.

Under the weapon the legs keep moving as the PC's grip flags allow (0x6cf7cc, 0x54e0f0): the plank,
bare hands, the spray and Maria's knife walk, back off, strafe and run (no running indoors for the
spray, plank and knife, 0x52dac0); the handgun, the pipe and Maria's revolver walk; the shotgun and
the chainsaw walk while aiming but stop to fire; the rifle and the great knife stand. Before the
first attack since raising, any weapon may break into a run. Locked on, run held quickens the walk
1.5 x (at most 2.25 m/s, 1.8 backing off or strafing). The legs play their gait while the upper body
keeps the weapon's pose (the two body parts on their own anime, 0x539140). His sight is a
"look" sphere ahead of him (its centre as far ahead as its radius) and a "feel" sphere round him,
which shrink with the light and the action level (0x52f2c0, in metres x 500):

| Light | Look (beginner, easy, normal, hard) | Feel radius | Feel centre ahead |
| --- | --- | --- | --- |
| Lit room | 7, 7, 6, 4.5 | 6.5, 6.5, 5, 3 | 0.5 |
| Dark, flashlight on | 5, 5, 3.8, 3 | 4.5, 4.5, 3.8, 3.3 | 0.3 |
| Dark, flashlight off | 3, 3, 1.8, 1.5 | 3, 3, 2.3, 2 | 0.1 |
| Maria in the dark | 6, 6, 5, 4 | 5, 5, 4, 3 | 0.5 |

The lock holds while the target lives and any candidate stays in clear sight (0x533c10); lowering
the weapon or changing it drops it. While aiming, Target left / right move it to the nearest other
enemy on that side (0x542740). The strafe buttons strafe, as on the PC.
With Aim held, Attack with an enemy lying on the floor ahead of James within 600 units (0x542a10)
kicks or stomps it, whatever James holds (0x5569b0 picks that branch before it looks at the weapon),
and the kick turns him to it (0x558f40). With the Enhanced Edition's Hold to Stomp,
a tap kicks and holding Attack for 0.2 s stomps.
The attack windows were checked against the exe's sh2_attack_list (0x6ec568): the plank's swings 12
to 15, 8 to 11 and 8 to 10, the kicks 7 to 10 and the stomps 3 to 9 for every weapon the port has
(0x53b190 picks kick type 0, 1 or 2 for bare hands and the handgun, the long guns and the plank, all
with that window).

A light swing (a tap with Swap Light/Heavy on, a combo, or any swing while running) of the plank,
Maria's knife or the pipe lets the legs walk or run on through it (grip flags 0x10 / 0x20, 0x556840,
0x556880); heavy swings, the lunge, the chainsaw and the great knife stop him (0x5569b0). While any
attack plays the legs keep their gait (0x54e0f0). Holding Action attacks again, light, as each attack
ends (0x52e110, 0x533ff0). On beginner and easy, holding Action with an enemy close ahead raises the
weapon by itself (0x542620), unless the press opens an event.

## How James moves (move.c)

The PC's lower-body states of the 3D control type, from sh2pc.exe:

| State | Clip (base id) | Speed (x 500 units/s) | Turning (rad/s) |
| --- | --- | --- | --- |
| Stand | 101 (relax 115 to 117 after 10 s still, alert 118 with an enemy about; tired 113 / 114) | slows at 10 m/s/s (20 after a run) | |
| Turn on the spot | 103 left, 102 right | | 2.4 x tilt (x1.5 with run held) |
| Walk | 104, played slower with less tilt | 1.5 x tilt | 1.9 x tilt |
| Back | 105 | 1.2 x tilt | 1.3 x tilt |
| Strafe | 125 left, 124 right | up to 1.3 | 1.9 x tilt |
| Run 1, 2, 3 | 106, 107, 108 | up to 3.5, 4.0, 4.8 at 3 m/s/s | 1.7 x tilt |
| Strafe at a run | 127 left, 126 right | up to 3.8 | 1.7 x tilt |
| Quick turn | 110 left, 109 right | none | 5.5 (180 degrees in 0.57 s) |
| Alert (instead of relax when an enemy is about) | 118 | | |

Maria plays with the same table: her anm (xmar_wp*) holds the same moves at the same frames, so she
strafes, backs off, turns round and idles like James, at 0.8 of his speed (0x54ef10).

Running starts at run 1; reaching its top speed lets run 2 follow, and run 2's top speed lets run 3
follow while James has stamina (600 frames at full health, fewer when hurt; standing gives it back).
A wall within 250 units ahead stops a run from starting. From a run he cannot back off or turn
round; the quick turn runs to its end.

Clips change the way the PC's anime code does: walking, running and strafing clips crossfade into
each other over 32 frames keeping the step (mode 10), most others over 8 frames from the pose shown
(mode 4), and the quick turn, swings, shots, kicks, hits and falls cut in (mode 2). Footsteps land
on frames 5 and 17 of the walking clips (6 and 13 when turning on the spot), at 0.5 or 0.8.

All of it runs on real time, and the frame time is snapped to whole screen refreshes, so motion stays
even when the frame rate is not a multiple of 30.

## The behind camera (tpcam.c, port only)

Options, Controls tab, Camera (also the first run's first choice, `setup.c`): Original is the PC camera; Behind is the port's own third-person camera,
not in the PC game. It sits behind and above James and follows the way he walks once the player has
left it alone a moment (not when he walks towards it or sideways, so the stick never chases it). The
C-Stick turns it on the New 3DS layout; on the Old one the D-Pad's free directions do (they then do
not walk James), and a drag on an empty part of the touch screen does on both. The Look key swings it
back behind him. It keeps 150 from walls, testing rays fanned from James to either side of where it
will be, at the height it will be (under the ceiling), so a wall at the edge of the picture counts too
(the near plane, 100 deep, would otherwise cut into it) and a low partition it would sink to does as
well; it stays above
the floor and under the ceiling (the highest top of the walls that stand on James' floor near him,
which indoors reach the ceiling), and is never nearer than 450 unless a wall is. Squeezed against a
wall it turns, a little at a time, to the most open side near behind him, and in a room too small for
that (a wall at his back) to the most open way round him, as the PC's fixed shots stand in a room's far
corner; pulled in close it looks past him into the room rather than at his head. With the weapon
up it goes over his right shoulder; with a target locked it frames James and the target together, and
the circle pad's x strafes instead of turning. In this mode the stick moves James relative to the
camera (Directional Style's reading, taken every frame). Cutscenes, the event roads and program shots
of the .cam data, the boat and the viewer keep the PC camera.
