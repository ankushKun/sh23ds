#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>

// The top screen's drawing kit for the Options explainer (optmenu.c) and the first run (setup.c),
// 3DS-only (port/UI.md, "The explainer"): titles between fading rules, text in boxes that always fit
// (font_fit_px), the red band, button rings, and the animated drawings that show what a setting's
// values do. Everything is in top-screen pixels (400 x 240, y down) and goes through font.c: the
// shapes at fig_end, the text at the frame's font_flush. The colours are the touch screen's.

enum {
	FIG_NONE, FIG_CAMERA, FIG_MOVEMENT, FIG_AIM, FIG_LOOK, FIG_SCREENS, FIG_BEAM, FIG_FRAMING, FIG_MAP,
	FIG_STAMINA,
};
#define FIG_W 176.0f                 // a drawing's box
#define FIG_H 104.0f

#define FIGC_WHITE 0xf0ece6ffu
#define FIGC_GREY  0xb4aea6ffu
#define FIGC_LABEL 0x8a847cffu
#define FIGC_DIM   0x5e5954ffu
#define FIGC_RULE  0x4a4440ffu
#define FIGC_FAINT 0x2b2726ffu
#define FIGC_RED   0xc4161fffu
#define FIGC_BAND  0x741010ffu

// A frame's drawing: alpha fades all of it (the first run fades in); fig_end draws the shapes.
void fig_begin(float alpha);
void fig_end(void);
// Measuring instead of drawing (debug.txt "textcheck"): nothing is drawn, and every text that had
// to be cut is written to log as "cut <context>: <text>".
void fig_measure(FILE* log, const char* context);
void fig_measure_end(void);
int fig_cuts(void);                  // texts cut while measuring

void fig_title(const char* s, float y);   // centred, spaced, between two rules that fade out
// s in a box at (x, y) w wide, at most max lines, big font when it fits (align FONT_LEFT or
// FONT_CENTER, x then the middle); the height it took.
float fig_text(const char* s, float x, float y, float w, int max, bool big, int align, u32 rgba);
void fig_band(float x0, float y0, float x1, float y1);
void fig_rect(float x0, float y0, float x1, float y1, u32 rgba);
void fig_ring(float cx, float cy, const char* label, u32 rgba);   // a button: ring, or a tag for "ZL"
void fig_drop(float cx, float cy, float s, u32 fill, bool lit);   // a drop of blood (Blood Color)
// A drawing in its FIG_W x FIG_H box at (x, y): value is the setting's value it shows, t the time
// in seconds since it came up, lit false for the value not chosen (drawn faint), key the button
// the hold drawings show.
void fig_draw(int fig, int value, float x, float y, float t, bool lit, const char* key);
