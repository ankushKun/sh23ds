#pragma once
#include <3ds.h>
#include <citro3d.h>

// The PC's text fonts (pack/ui/font.t3x and font.bin, written by tools/sh2conv/ui.py from the exe)
// drawn on the top screen. Positions are in the PC's text space (0x47f400): x 0..512 left to right
// and y 16..496 top to bottom, the middle of the screen at (256, 256); the 4:3 picture is drawn as
// 320x240 in the middle of the top screen, like the stills. The atlas holds the glyphs already
// shrunk to that size, so they go on whole pixels one texel per pixel.
//
// Text is the pack's message text (a byte c is the PC code c - 0x20, '\n' starts a line); the
// full-width codes (0xe0 and up) go through font_codes. Glyphs are collected during the frame and
// drawn with one call to the draw function at font_flush.

typedef struct { float x, y, u, v; u32 rgba; } FontVert;    // top-screen pixels (y down), 0xRRGGBBAA
typedef void (*FontDrawFn)(C3D_Tex* tex, const FontVert* v, int n);

enum { FONT_BIG = 0, FONT_SMALL = 1 };                     // 20x30 and 16x24 cells
enum { FONT_LEFT = 0, FONT_CENTER = 1, FONT_RIGHT = 3 };   // the PC's alignment modes

bool font_init(const char* pack_dir, FontDrawFn draw);
bool font_ready(void);
int font_line_height(int font);
float font_width(int font, const char* s);                 // the widest line, in PC units
// top and bottom are the colours of the glyphs' upper and lower edges (the PC fades some labels).
void font_text(int font, float x, float y, const char* s, int align, u32 top, u32 bottom);
void font_codes(int font, float x, float y, const u16* codes, int n, int align, u32 top, u32 bottom);
void font_flush(void);
// The PC's text colours (0x47eca0, index 0..16) as top and bottom RGBA.
void font_pc_color(int index, u32* top, u32* bottom);
