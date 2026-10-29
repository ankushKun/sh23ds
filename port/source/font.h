#pragma once
#include <3ds.h>
#include <citro3d.h>

// The PC's text fonts (pack/ui/font.t3x and font.bin, written by tools/sh2conv/ui.py from the exe)
// drawn on the top screen. Positions are in the PC's text space (0x47f400): x 0..512 left to right
// and y 16..496 top to bottom, the middle of the screen at (256, 256); the 4:3 picture is drawn as
// 320x240 in the middle of the top screen, like the stills. The atlas holds the glyphs already
// shrunk to that size, so they go on whole pixels one texel per pixel.
//
// Text is the pack's message text (a byte c is the PC code c - 0x20, the Windows-1252 character c,
// '\n' starts a line, FONT_FULL_WIDTH and four hex digits are a full-width code (0xe0 and up) or the
// titles' kerning, tools/sh2conv/sh2mes.py); font_codes takes the codes themselves. Glyphs are
// collected during the frame and drawn with one call to the draw function at font_flush.

typedef struct { float x, y, u, v; u32 rgba; } FontVert;    // top-screen pixels (y down), 0xRRGGBBAA
typedef void (*FontDrawFn)(C3D_Tex* tex, const FontVert* v, int n);

#define FONT_FULL_WIDTH '\x06'
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
// A picture in the same space (x0, y0)-(x1, y1), drawn at once (before the frame's text).
void font_image(C3D_Tex* tex, float x0, float y0, float x1, float y1, u32 rgba);
// A quad in top-screen pixels with texture coordinates (u0, v0)-(u1, v1) and a colour per corner
// (top left, top right, bottom left, bottom right), drawn at once; tex NULL: flat, for shading.
void font_quad_px(C3D_Tex* tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  const u32 rgba[4]);
// The same with a texture coordinate per corner (uv: top left, top right, bottom left, bottom right
// as u, v pairs), for a picture that is turned in its texture (gameover.c's still of the screen).
void font_quad_uv_px(C3D_Tex* tex, float x0, float y0, float x1, float y1, const float uv[8], const u32 rgba[4]);
// The same glyphs laid out in plain screen pixels for a screen of the caller's own (touchui.c):
// (x, y) is the top left of the first line, glyphs at their atlas size, the PC's advances times
// 0.625 and the lines 0.46875 of the cell height apart. Writes up to max vertices (6 a glyph) and
// returns how many; font_texture is the atlas they sample.
int font_layout_px(int font, float x, float y, const char* s, int align, u32 top, u32 bottom, FontVert* out, int max);
float font_width_px(int font, const char* s);
// The ink of a line laid out by font_layout_px from (0, 0), left-aligned: the box its visible
// pixels cover (for centring a letter in a ring). False when there is no ink.
bool font_ink_px(int font, const char* s, float* x0, float* y0, float* x1, float* y1);
float font_line_px(int font);
C3D_Tex* font_texture(void);
// The PC's text colours (0x47eca0, index 0..16) as top and bottom RGBA.
void font_pc_color(int index, u32* top, u32* bottom);
