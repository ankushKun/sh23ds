#pragma once
#include <3ds.h>

// The PC's pause menu (game event 0x10 in 0x401fc0, drawn by 0x407310 / 0x407620): PAUSED at
// (256, 164) and Resume, Save Game, Load Game, Options, Quit from y 230 in steps of 30, the chosen
// one again at full alpha 2 up and 2 left; Save Game greyed where the game cannot be saved
// (0x44c7d0); Quit asks "Exit to Main Menu?" with Yes and No (No first). The port splits it
// (port/UI.md): the touch screen lists the rows, with its own Map and Memos after them, and the
// answers to the question; the top screen keeps PAUSED over the dimmed room, where James is and the
// play time, or the question.

enum { PM_NONE, PM_RESUME, PM_SAVE, PM_LOAD, PM_OPTIONS, PM_QUIT, PM_MAP, PM_MEMOS };

void pausemenu_open(bool can_save);
void pausemenu_open_at(bool can_save, int row);  // back from the Map or Memos row: that row chosen
void pausemenu_where(const char* place, float play_time);   // for the top screen, before opening
void pausemenu_close(void);
int pausemenu_update(u32 down);          // a PM_* once one is chosen
void pausemenu_draw_top(void);           // inside a C3D frame (font.c's text, flushed by the caller)
