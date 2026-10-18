# Controls

James is controlled the PC way. The Options' Control Type is the PC's: Rotational Style (the "3D"
type: the circle pad turns him and walks him forward or back) or Directional Style (the "2D" type,
see below). The buttons are actions, and the actions are bound per layout. The New 3DS layout uses ZL, ZR and the C-Stick on top
of the buttons every 3DS has; the Old 3DS layout gets by with L, R, A, B, X, Y, the D-Pad, the
Circle Pad, START, SELECT and the touch screen. Which layout is used follows the console unless the
player picks one (`profile.c`, `controls_layout()`). Every action can be rebound on the Buttons
page of the Options' Control Options (`controls.c`, drawn in the Options screen's menu slot through
`controls_menu_slot(TOUI_OPTIONS)`), separately for each layout; the bindings are kept in
`controls.txt` next to `options.txt`.

## Default layouts

| Action | New 3DS | Old 3DS |
| --- | --- | --- |
| Walk, turn | Circle Pad | Circle Pad |
| Action (check, open, pick up, attack while aiming) | A | A |
| Run (hold; "toggle" in the Control Options) | B or ZR | B |
| Aim (hold) | R | R |
| Look (hold: the PC's search view) | ZL | L |
| Next weapon | L | D-Pad up |
| Flashlight | X | X |
| Items | Y | Y |
| Quick turn (180 degrees) | C-Stick down | D-Pad down |
| Strafe left / right | C-Stick or D-Pad left / right | D-Pad left / right |
| Map | C-Stick up, or tap the bottom screen | tap the bottom screen, or the pause menu |
| Brightness | D-Pad up / down | the Options (Advanced Options) |
| Pause | START | START |
| Debug page | SELECT twice | SELECT twice |

Pressing both strafe buttons together also turns James round, as on the PC (0x54b290). Menus keep
their own keys whatever the bindings: the D-Pad moves, A takes, B goes back, START closes.

## The Buttons page

Pause, Options, Control Options, then "Buttons". Up and down choose an action, A waits for the new button (START keeps the
old one), Y puts that action back to its default and "Defaults" resets the whole layout. A button
does one thing: binding it to an action takes it off the action that had it. The page edits the
layout in use, so an Old 3DS player never sees ZL, ZR or the C-Stick offered.

## Directional Style

Directional Style is the PC's 2D type (0x547bf0, 0x546760, 0x548bf0): the direction is taken from the
camera James saw when the stick was pushed and held across camera cuts until the stick turns more
than 30 degrees or is let go, he turns towards it at 20 x 0.3235 of the gap a second, and he walks
or runs along the way he faces as fast as the stick is tilted. The camera behaves as the PC's does
for that type. (The port's earlier camera-relative mode is gone: it turned at a fixed 7 rad/s to the
current camera's direction and spun James round at camera cuts, which Directional Style is built
to avoid, so it added nothing.)

## James' head (main.c head_update, combat_look_target)

As on the PC (0x535720), James' head turns to the locked target, or else to the nearest enemy,
person (Maria) or item lying about that is in his sight (the look and feel areas below) and not
hidden by the room. With the look key held it follows the circle pad instead. The angles ease
towards their aim at 20 x 0.3235 of the gap a second (0x5343e0) and stop at 0.8 radians to either
side, 0.5 up and 0.4 down; the neck takes a fifth and the head the rest. It rests while he relaxes
or looks round, and while he is kicking, hurt, held or dying.

## Aiming (combat.c)

Holding Aim locks on to a target as James readies (0x533ca0): the nearest enemy in his sight with
nothing of the room in between, one on its feet before one on the floor (0x542560). His sight is a
"look" sphere ahead of him (its centre as far ahead as its radius) and a "feel" sphere round him,
which shrink with the light and the action level (0x52f2c0, in metres x 500):

| Light | Look (beginner, easy, normal, hard) | Feel radius | Feel centre ahead |
| --- | --- | --- | --- |
| Lit room | 7, 7, 6, 4.5 | 6.5, 6.5, 5, 3 | 0.5 |
| Dark, flashlight on | 5, 5, 3.8, 3 | 4.5, 4.5, 3.8, 3.3 | 0.3 |
| Dark, flashlight off | 3, 3, 1.8, 1.5 | 3, 3, 2.3, 2 | 0.1 |
| Maria in the dark | 6, 6, 5, 4 | 5, 5, 4, 3 | 0.5 |

The lock holds while the target lives and stays in sight (0x533c10). While aiming, the strafe
buttons move it to the nearest other enemy on that side (0x542740; the PC has two keys of its own
for that, which the 3DS has no room for).
The attack windows were checked against the exe's sh2_attack_list (0x6ec568): the plank's swings 12
to 15, 8 to 11 and 8 to 10, the kicks 7 to 10 and the stomps 3 to 9 for every weapon the port has
(0x53b190 picks kick type 0, 1 or 2 for bare hands and the handgun, the long guns and the plank, all
with that window).

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
