# The two screens

How the port splits the game's interface between the top screen and the touch screen, and how the
touch screen looks. Code: `touchui.c` (the bottom screen and its kit) and the `*_draw_top`
functions of each screen. Decided 2026-10-19.

## The rule: the list below, the subject above

Every list the player picks from lives only on the touch screen, where it can be tapped or walked
with the D-Pad. The top screen shows the thing itself: the world, the item, the memo, the map, the
save file, the setting being changed. Nothing is drawn on both screens.

| Screen | Top screen | Touch screen |
| --- | --- | --- |
| Play | the game | the map around James, condition, weapon, quick slots, prompts |
| Message, subtitle | the PC's message window (Messages: Top, the default) | the play page with an "A Next" prompt; a tap anywhere is A |
| Question ("Save the game?") | the question in the message window | the answers as rows that act on one tap |
| Cutscene, movie | the scene | black, START Skip |
| Pause | the dimmed world, PAUSED, where James is and the play time | Resume, Save Game, Load Game, Options, Quit, Map, Memos |
| Options | the page title, the chosen setting large with its value and the PC's help line; the brightness picture | the page's rows with their values |
| Buttons page | the chosen action and its buttons, "press a button" while waiting | the actions and their buttons |
| Save and load | James' picture, the chosen file in full (place, play time, saves), the questions | the ten files |
| Items | the dimmed world, the item's model turning, its name and count, its text after Examine | the items, marks for Combine |
| Memos | the memo paper with the chosen memo's title, then the memo itself | the memo list |
| Map | the map | the map's name, floors and other maps, zoom; dragging scrolls a zoomed map |
| Puzzles and stage panels | the puzzle's pictures, as the PC draws them | the panel text, the D-Pad and A / B as touch buttons |
| Title | start00 and the pulsing logo | LOAD, CONTINUE, NEW GAME, OPTIONS, MOVIES, EXIT |
| New Game levels | both levels as chosen so far, the current one lit | the current level's choices |
| Movie Menu | the chosen movie's name | the movies |
| Game over, results | as the PC draws them | black, the prompt to go on |

Messages and subtitles can still go on the touch screen (Options, Messages: Bottom), as the
Enhanced Edition's own option has it; they then replace the play page while they are up.

## Buttons mean the same thing everywhere

- A takes the chosen row, B goes back, the D-Pad moves. A question's B is No.
- Labels: "Back" for every way out (never Close or Cancel), "Yes" and "No" for questions.
- Every prompt shows the button it stands for. On the play page that is the button bound to the
  action in the active layout (`controls_keys`), so a remapped Items shows its own button.
- Action lists (title, pause, questions) act on the first tap. Browsing lists (items, memos, save
  files, options, movies) take the first tap as a choice, which the top screen then shows, and the
  second as A.

## The look

The PC's own front end, as on the website: no boxes.

- Black with film grain that flickers in place (four noise frames swapped about twelve times a
  second, held still on the Old 3DS profile) and a soft dark edge.
- The PC font. Titles in the big font, centred, between two thin rules that fade out at the ends.
  Labels in the small font, grey, in capitals.
- Rows in the PC's grey; the chosen row white over a dark red bar that fades out at both ends
  (the website's selection bar and the PC options' band). Rows that can't be used now are dim.
- Values on the right of their row, with the PC's small arrows either side when the row is chosen.
- A footer of prompts under a thin rule: a ring with the button's letter, then the action. The
  whole prompt is the touch target, at least 28 pixels high, and it lights red for a moment when
  tapped. Shoulder buttons and START are drawn as rounded tags, the D-Pad as a cross.
- Condition as the PC's item screen draws it: the heartbeat line in green, yellow or red.

## The play page

- The map around James fills the left of the screen (tap: the map screen; the ring in its corner
  zooms). Where James has no map of the place, a dim "No map" line, no box.
- Right column: Condition (the heartbeat and its word), the weapon in hand with its rounds (tap:
  next weapon), then the three healing items with their counts (tap: use one, as the item screen's
  Use does; dim when there are none).
- Footer: Items, Light (dim without the flashlight), Pause, each with its bound button. The map has
  no prompt: tapping the map is the way to it. The zoom ring is drawn with the map's pictures so the
  map never covers it.

## No game data

Before anything else is set up, `nodata.c` checks `sdmc:/3ds/sh23ds/` for a few files every finished
pack has (one or more from each of the packer's main steps). Without them the game has none of its
own art or type, since the PC font is in the pack too, so this screen carries its own and draws in
software into the framebuffers, ahead of citro3d. A 3DS-only screen with no PC counterpart. It tells
three cases apart:

- **Not found**: nothing of the pack there. What the packer is, the three install steps and the
  packer's address.
- **Wrong folder**: the pack found one level off (the `sh23ds` folder at the root, `3ds` inside
  `3ds`, the zip unzipped into a `sh23ds-sd` folder, Docker's `sd` folder copied whole, a
  `make_pack.sh` `pack` folder). Where it is, where it should be, and to keep any saves already in
  the right folder.
- **Incomplete**: some of the files there, others missing (a copy that stopped, a packer run that
  did not finish). The missing files and what to do.

The look is the website's (`site/site.css`, and its install card `site/assets/og/install.jpg`):

- Top: black with a dull red glow, fog drifting two ways and thicker towards the ground, the
  vignette, and the port's film grain (four frames, each held for five). Over it the red mark and
  the spaced VT323 label, the heading in Libre Caslon capitals, the lede in Crimson Pro italic, then
  the steps with red numbers and the addresses in VT323.
- Bottom: the install guide's QR code inked on a memo, the game's paper with its burnt edge, under
  the footer rule and a START prompt drawn like the kit's (tapping it does the same).
- Both screens fade in from black, and out when START closes the app. A screen is drawn again only
  when its grain, fog or fade has changed, which keeps the Old 3DS at its frame rate.

The type is the website's fonts (SIL Open Font License) rasterized at the screen's sizes into
`nodata_font.h` by `tools/make_nodata_font.py`; VT323 is only sharp at sizes that are a multiple
of 5 px. The QR code is `nodata_qr.h`, made by `tools/make_nodata_qr.py`.

## Checkpoints

Each one is built, checked in Azahar with the UI capture script (`tests/ui/capture.sh`, every
state above as a screenshot), then run on a New 3DS before the next starts.

1. The kit (background, grain, rules, rows, prompts, pressed state), every existing touch screen
   drawn through it, the play page, the button labels made consistent.
2. Pause, options, save files, title, levels, movies, messages and questions moved to the rule.
3. Items with the model on top, memos, the map page, puzzle panels, game over and results; the old
   text-console copies of these screens taken out of `main.c`.
