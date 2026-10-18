#pragma once
#include <3ds.h>
#include "map.h"

// The PC's title screens (Event/title.c in the PC's own asserts; 0x4972a0 loads them):
//   menu        0x4976b0 / 0x496280: LOAD and CONTINUE when there are saves, NEW GAME, OPTIONS,
//               MOVIES, EXIT (option_msg_e.mes 238..243) in the small font, centred at x 256 from
//               y 284, 22 apart, over data/pic/etc/start00.tex
//   scenario    0x4979c0 / 0x4968c0: MAIN SCENARIO and SUB SCENARIO (Born From a Wish), 252 / 253
//   levels      0x497bb0 / 0x497fc0: ACTION LEVEL (HARD, NORMAL, EASY, BEGINNER) at x 128 and
//               RIDDLE LEVEL (HARD, NORMAL, EASY) at x 384, one after the other; Born From a Wish
//               has the action level alone, centred (0x496490)
//   movies      the Movie Menu: the PC's movies (214..237) that the pack has
// Drawn on the top screen, chosen on the touch screen or with the buttons.

enum {
	TM_NONE, TM_LOAD, TM_CONTINUE, TM_NEW_GAME, TM_WISH, TM_OPTIONS, TM_MOVIE, TM_EXIT, TM_VIEWER,
	TM_ATTRACT,          // a minute on the menu without a key: the opening movie (menu event 0xc)
};

// Back on the menu: saves 0 none, 1 some (LOAD and CONTINUE show); CONTINUE is chosen when there
// are saves (0x4959d0), NEW GAME otherwise.
void titlemenu_reset(bool saves);
// One frame of input; TM_NEW_GAME / TM_WISH come with the levels chosen (action 0 beginner .. 3
// hard, riddle 0 easy .. 2 hard), TM_MOVIE with the movie's file name.
int titlemenu_update(u32 down, float dt, int* action, int* riddle, const char** movie);
// The screens over the background (start00), brightness 0..1 for the fades; false: the pack has
// no start00 (the caller draws a background of its own).
bool titlemenu_draw_top(MapDrawFn draw, float bright);
bool titlemenu_init(const char* pack_dir);
// The start-up screens before the menu (menu events 4 and 5, 0x405e90): the Konami logo and the
// splash screen, each a second in, a second and a second out (a key cuts them short).
void titlemenu_boot(void);
void titlemenu_no_boot(void);            // debug.txt "logos 0"
void titlemenu_viewer(bool on);          // debug.txt "viewer 1": the room viewer's row on the touch list
