#pragma once
#include <3ds.h>

// The touch screen as a game screen (port/UI.md): drawn with citro3d in the PC font (font.c) in
// the PC front end's look instead of the libctru text console. The console still exists, writing
// into a buffer of its own; the hidden debug page (SELECT twice) shows it.
//
// main.c tells touchui what is up (menus, message boxes, panels) where it used to print them; the
// screen is redrawn only when something it shows changed (and for the grain), and taps turn into
// the buttons the screens already take (touchui_input), so the game logic stays where it is.

// A footer prompt: the button's glyph, then the label. A tap on it presses key.
typedef struct { const char* label; u32 key; } TouiButton;

enum { TOUI_TITLE, TOUI_LEVELS, TOUI_PAUSE, TOUI_ITEMS, TOUI_MEMOS, TOUI_SAVE, TOUI_OPTIONS, TOUI_MENUS };

void touchui_init(void);                     // after C3D_Init and consoleInit
void touchui_input(u32* down, u32* held);    // after the keys are read, before the game uses them
void touchui_frame(void);                    // inside a C3D frame, before C3D_FrameEnd

// A menu (rows the D-pad walks, *sel the row): a tap on a row selects it, a second tap presses A,
// and on the chosen row's left or right end of a "< value >" row presses left or right. buttons
// are the footer's prompts.
void touchui_menu(int id, const char* title, const char* const* rows, int n, int* sel,
                  const TouiButton* buttons, int nbuttons);
// The same with dim rows (bit i: row i can't be taken now) and flags: TOUI_ONE_TAP for action
// lists, where the first tap on a row takes it (port/UI.md, "Buttons mean the same thing").
enum { TOUI_ONE_TAP = 1 };
void touchui_menu_ex(int id, const char* title, const char* const* rows, int n, int* sel,
                     const TouiButton* buttons, int nbuttons, u64 dim, int flags);
// The Options screen's extras: tabs in place of the title, with L and R either side (a tap presses
// KEY_L or KEY_R, a tap on a tab writes *tab), and what each row shows on its right: its
// "< value >", a small arrow for a row that opens a screen, or the PC's volume blocks.
enum { TOUI_ROW_VALUE, TOUI_ROW_PAGE, TOUI_ROW_BAR };
typedef struct { u8 kind, level, max; } TouiRow;
#define TOUI_MAX_TABS 6
typedef struct { const char* const* tabs; int ntabs; int* tab; const TouiRow* rows; } TouiMenuExtra;
void touchui_menu_full(int id, const char* title, const char* const* rows, int n, int* sel,
                       const TouiButton* buttons, int nbuttons, u64 dim, int flags, const TouiMenuExtra* extra);
void touchui_menu_close(int id);
// Two choices side by side (the first run, setup.c): each a name with up to three lines under it,
// the chosen one on the red band. A tap on a choice chooses it, a second tap presses A. dim: bit i,
// prompt i can't be taken (it keeps its place).
void touchui_choices(const char* const* names, const char* const* lines, int* sel, const TouiButton* buttons,
                     int nbuttons, u32 dim);
void touchui_choices_close(void);
// A message, subtitle or question (text NULL: none); a tap presses A unless buttons are given.
// Empty text with no buttons: a message is up on the top screen (the play page with "Next").
// nbuttons -1: text alone (a subtitle over play), no prompt, the taps left to the game.
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

// The play page's quick slots: TQ_HEAL with the healing item (1 drink, 2 first aid kit,
// 3 ampoule) that was tapped, once; main.c uses it as the item screen's Use would. TQ_WEAPON: the
// weapon was tapped while no button is bound to the next weapon.
enum { TQ_NONE, TQ_HEAL, TQ_WEAPON };
int touchui_quick(int* item);

// A finger that came down where the screen shows nothing to tap: how far it moved since the last
// call, in pixels (the behind camera turns with it). False when no such finger is down.
bool touchui_free_drag(int* dx, int* dy);
