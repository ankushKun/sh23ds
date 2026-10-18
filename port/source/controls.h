#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>

// Button layout and remapping (port/CONTROLS.md). The player's actions (run, aim, quick turn ...) are
// bound to buttons per layout: the New 3DS layout uses ZL, ZR and the C-Stick as well, the Old 3DS
// layout only L, R, A, B, X, Y, the D-Pad, the Circle Pad, START, SELECT and the touch screen. Which
// layout is active comes from profile.c's controls_layout() (Auto follows the console).
//
// Menus keep reading the buttons themselves (A confirms, B goes back, the D-Pad moves); only the
// game while James is free goes through here. controls_game() turns the buttons into "game keys",
// the KEY_* bits the game code already tested before there was remapping:
//   KEY_A action, KEY_B run, KEY_R aim, KEY_L next weapon, KEY_X flashlight, KEY_Y items,
//   GK_LOOK, GK_TURN180, GK_STRAFE_L, GK_STRAFE_R, GK_MAP, and KEY_DUP / KEY_DDOWN for the
//   brightness when the layout leaves the D-Pad's up and down free.

#if __has_include("profile.h")
#include "profile.h"
#else
// Before profile.c is in (the Old 3DS work): the same declarations, New layout on a New 3DS.
typedef enum { CONTROLS_LAYOUT_OLD = 0, CONTROLS_LAYOUT_NEW = 1 } ControlsLayout;
typedef enum {
	ACT_RUN, ACT_ACTION, ACT_AIM, ACT_WEAPON_NEXT, ACT_FLASHLIGHT, ACT_ITEMS, ACT_LOOK, ACT_TURN180,
	ACT_STRAFE_LEFT, ACT_STRAFE_RIGHT, ACT_MAP, ACT_PAUSE, ACT_CANCEL, ACT_COUNT
} ControlsAction;
ControlsLayout controls_layout(void);
#endif

#define GK_LOOK     KEY_ZL
#define GK_TURN180  KEY_ZR
#define GK_STRAFE_L KEY_CSTICK_LEFT
#define GK_STRAFE_R KEY_CSTICK_RIGHT
#define GK_MAP      KEY_CSTICK_UP

// Reads <dir>/controls.txt (the player's own bindings, if any).
void controls_init(const char* dir);
// The default buttons of an action in a layout (several bits: any of them works).
u32 controls_default(ControlsLayout layout, ControlsAction act);
// The buttons bound now (the player's or the default) in the active layout.
u32 controls_keys(ControlsAction act);
const char* controls_name(ControlsAction act);

// Once per frame with the raw buttons (hidKeysDown / hidKeysHeld after touchui_input): game keys.
void controls_game(u32 down, u32 held, u32* gdown, u32* gheld);
// D-Pad up / down change the brightness in play (the layout does not use them for anything else).
bool controls_dpad_brightness(void);

// The "Buttons" page of the pause menu, drawn like the other pages (bottom console rows 18..29 and
// touchui_menu). controls_menu_update returns false once it is closed.
void controls_menu_open(void);
bool controls_menu_update(u32 down);
void controls_menu_draw(void);
const char* controls_menu_label(void);     // the pause menu line
// One line per action of the active layout for the help screen ("Run  B / ZR").
int controls_help(char lines[][40], int max);
