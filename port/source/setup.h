#pragma once
#include <3ds.h>
#include <stdio.h>
#include "optmenu.h"

// The first run (3DS-only, port/UI.md "First run"): once, after the start-up logos and before the
// title menu, the two choices that change most how the game plays, each pair side by side: the
// drawings on the top screen (figures.c), the names with a line each on the touch screen. Camera
// (the PC's or the port's behind camera, tpcam.c), then the Control Type, which only the PC's
// camera uses outside scripted shots, so it is skipped after Behind. Then the buttons as they are
// bound (two columns on the top screen). The current values start chosen; every change applies at
// once, and the last step saves the options with "setup 1" and "ctlseen 1". only_controls: the
// Controls page alone, for a player who made the choices before it was there.
void setup_open(Options* live, void (*apply)(const Options*), void (*save)(const Options*), bool only_controls);
bool setup_active(void);
void setup_update(u32 down);
// Inside a C3D frame on the top screen, before font_flush.
void setup_draw_top(void);
// debug.txt "textcheck": the choices' texts laid out in both button layouts; the number cut.
int setup_textcheck(FILE* out);
