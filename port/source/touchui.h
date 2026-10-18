#pragma once
#include <3ds.h>

// The touch screen as a game screen (port/ROADMAP.md workstream 2): drawn with citro3d in the PC
// font (font.c) instead of the libctru text console. The console still exists, writing into a
// buffer of its own; the hidden debug page (SELECT twice) shows it.
//
// main.c tells touchui what is up (menus, message boxes, panels) where it used to print them; the
// screen is redrawn only when something it shows changed, and taps turn into the buttons the
// screens already take (touchui_input), so the game logic stays where it is.

typedef struct { const char* label; u32 key; } TouiButton;

enum { TOUI_TITLE, TOUI_LEVELS, TOUI_PAUSE, TOUI_ITEMS, TOUI_MEMOS, TOUI_SAVE, TOUI_MENUS };

void touchui_init(void);                     // after C3D_Init and consoleInit
void touchui_input(u32* down, u32* held);    // after the keys are read, before the game uses them
void touchui_frame(void);                    // inside a C3D frame, before C3D_FrameEnd

// A menu (rows the D-pad walks, *sel the row): a tap on a row selects it, a second tap presses A,
// and on the chosen row's left or right end of a "< value >" row presses left or right. buttons
// are extra touch buttons.
void touchui_menu(int id, const char* title, const char* const* rows, int n, int* sel,
                  const TouiButton* buttons, int nbuttons);
void touchui_menu_close(int id);
void touchui_menu_items(const int* items, int n);   // the item menu's item ids (their models)
// A message, subtitle or question (text NULL: none); a tap presses A unless buttons are given.
void touchui_box(const char* text, const TouiButton* buttons, int nbuttons);
// The panel the stage programs and the map and memo screens print (empty: none), with the D-pad
// and A / B as touch buttons.
void touchui_panel(const char* text);
// SELECT: true on the second press within half a second (main.c then toggles the debug page).
bool touchui_select(void);
void touchui_debug(bool on);                 // the debug page: the console on the screen
void touchui_movie(bool on);                 // a movie plays: "skip" only
void touchui_map(bool open);                 // the map screen is up (map.c): its buttons
void touchui_map_readable(bool yes);         // the small map shows (false: too dark, as the map key)
