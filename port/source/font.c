#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <tex3ds.h>

#define MAX_CODE 0x200
#define MAX_GLYPHS 1024      // per frame: a full results screen is about 450

typedef struct {
	u16 x, y; u8 w, h, adv;      // atlas rectangle (3DS pixels), advance (PC units)
	u8 ix0, iy0, ix1, iy1;       // the ink inside the rectangle (font_ink_px), ix1 = 0: none
} Glyph;

static void ink_boxes(void);

static struct {
	u16 cell_w, cell_h;
	s16 index[MAX_CODE];      // code -> glyph, -1 when the font has none
	Glyph* glyphs;
} s_font[2];
static C3D_Tex s_tex;
static bool s_ready;
static FontDrawFn s_draw;
static FontVert s_verts[MAX_GLYPHS * 6];
static int s_nverts;

bool font_ready(void) { return s_ready; }
int font_line_height(int font) { return s_font[font & 1].cell_h; }

bool font_init(const char* pack_dir, FontDrawFn draw)
{
	s_draw = draw;
	if (s_ready)
		return true;
	char path[160];
	snprintf(path, sizeof path, "%s/ui/font.bin", pack_dir);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	char magic[4];
	u16 n = 0;
	bool ok = fread(magic, 4, 1, f) == 1 && !memcmp(magic, "FNT1", 4) && fread(&n, 2, 1, f) == 1 && n == 2;
	for (int k = 0; ok && k < 2; k++) {
		u16 head[3];
		ok = fread(head, 2, 3, f) == 3;
		if (!ok)
			break;
		s_font[k].cell_w = head[0], s_font[k].cell_h = head[1];
		memset(s_font[k].index, 0xff, sizeof s_font[k].index);
		free(s_font[k].glyphs);
		s_font[k].glyphs = calloc(head[2] ? head[2] : 1, sizeof(Glyph));
		ok = s_font[k].glyphs != NULL;
		for (int i = 0; ok && i < head[2]; i++) {
			u8 rec[10];
			ok = fread(rec, 10, 1, f) == 1;
			u16 code = rec[0] | rec[1] << 8;
			Glyph* g = &s_font[k].glyphs[i];
			g->x = rec[2] | rec[3] << 8, g->y = rec[4] | rec[5] << 8;
			g->w = rec[6], g->h = rec[7], g->adv = rec[8];
			if (ok && code < MAX_CODE)
				s_font[k].index[code] = (s16)i;
		}
	}
	fclose(f);
	if (!ok)
		return false;
	snprintf(path, sizeof path, "%s/ui/font.t3x", pack_dir);
	f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(&s_tex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	ink_boxes();
	s_ready = true;
	return true;
}

// Where each glyph's ink is inside its rectangle, read once from the atlas (LA4: alpha in the low
// nibble, 8x8 tiles in Morton order, rows in the order font.c's v coordinates count them, which
// measuring the rings on screen settled). The rectangles are
// whole cells, so centring on them leaves a letter off centre inside a button's ring.
static void ink_boxes(void)
{
	const u8* px = (const u8*)s_tex.data;
	int W = s_tex.width, H = s_tex.height;
	if (!px || s_tex.fmt != GPU_LA4)
		return;
	for (int k = 0; k < 2; k++)
		for (int i = 0; s_font[k].glyphs && i < MAX_CODE; i++) {
			if (s_font[k].index[i] < 0)
				continue;
			Glyph* g = &s_font[k].glyphs[s_font[k].index[i]];
			int x0 = 255, y0 = 255, x1 = -1, y1 = -1;
			for (int y = 0; y < g->h; y++)
				for (int x = 0; x < g->w; x++) {
					int u = g->x + x, r = g->y + y;
					if (u >= W || r >= H)
						continue;
					u32 m = (u & 1) | (r & 1) << 1 | (u & 2) << 1 | (r & 2) << 2 | (u & 4) << 2 | (r & 4) << 3;
					u8 a = px[((r >> 3) * (W >> 3) + (u >> 3)) * 64 + m] & 15;
					if (a >= 4) {
						x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
						y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
					}
				}
			if (x1 >= 0)
				g->ix0 = (u8)x0, g->iy0 = (u8)y0, g->ix1 = (u8)(x1 + 1), g->iy1 = (u8)(y1 + 1);
		}
}

static const Glyph* glyph(int font, int code)
{
	if (code < 0 || code >= MAX_CODE || s_font[font].index[code] < 0)
		return NULL;
	return &s_font[font].glyphs[s_font[font].index[code]];
}

// 0x47f400: codes below 0xe0 advance by their width plus 2 (the space by 7), the full-width ones
// by the cell width plus 2; a line's width leaves out the last 2.
static int advance(int font, int code)
{
	// 0xfcnn / 0xfdnn move the pen back / on by nn (the titles' kerning); 0xfffe, a full-width
	// space, falls through to the full-width advance
	if ((code & 0xff00) == 0xfc00)
		return -(code & 0xff);
	if ((code & 0xff00) == 0xfd00)
		return code & 0xff;
	const Glyph* g = glyph(font, code);
	return g ? g->adv : code < 0xe0 ? 7 : s_font[font].cell_w + 2;
}

// The code at *s in the pack's text, moving *s past it: a byte c >= 0x20 is the code c - 0x20,
// FONT_FULL_WIDTH and four hex digits a full-width code or a kerning step; -1 for the other bytes
// below 0x20 (inserts and page marks are not drawn). Stops at the end of the string or a line.
static int next_code(const char** s)
{
	const char* p = *s;
	u8 c = (u8)*p++;
	int code = c >= 0x20 ? c - 0x20 : -1;
	if (c == FONT_FULL_WIDTH) {
		code = 0;
		for (int k = 0; k < 4; k++, p++) {
			char h = (char)(*p | 0x20);      // either case
			int d = *p >= '0' && *p <= '9' ? *p - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
			if (d < 0) {
				code = -1;
				break;
			}
			code = code << 4 | d;
		}
	}
	*s = p;
	return code;
}

// One line of the pack's text as codes (up to max), *s left at its '\n' or the end.
static int line_codes(const char** s, u16* out, int max)
{
	int n = 0;
	while (**s && **s != '\n') {
		int code = next_code(s);
		if (code >= 0 && n < max)
			out[n++] = (u16)code;
	}
	return n;
}

static float line_width(int font, const u16* codes, int n)
{
	int w = 0;
	for (int i = 0; i < n; i++)
		w += advance(font, codes[i]);
	return w > 2 ? (float)(w - 2) : 0.0f;
}

static void put_glyph(const Glyph* g, float x, float y, u32 top, u32 bottom)
{
	if (!g->w || s_nverts + 6 > MAX_GLYPHS * 6)
		return;
	// The PC draws the cell at (x, y) with the glyph one pixel in (0x480ea0). The atlas holds the
	// glyph at its 3DS size, so it goes on whole pixels one texel per pixel.
	float x0 = floorf(200.0f + (x + 1.0f - 256.0f) * 0.625f + 0.5f);
	float y0 = floorf(120.0f + (y + 1.0f - 256.0f) * 0.46875f + 0.5f);
	float x1 = x0 + g->w, y1 = y0 + g->h;
	float u0 = g->x / (float)s_tex.width, v0 = g->y / (float)s_tex.height;
	float u1 = (g->x + g->w) / (float)s_tex.width, v1 = (g->y + g->h) / (float)s_tex.height;
	FontVert a = { x0, y0, u0, v0, top }, b = { x1, y0, u1, v0, top };
	FontVert c = { x0, y1, u0, v1, bottom }, d = { x1, y1, u1, v1, bottom };
	FontVert* v = &s_verts[s_nverts];
	v[0] = a, v[1] = c, v[2] = d, v[3] = a, v[4] = d, v[5] = b;
	s_nverts += 6;
}

void font_codes(int font, float x, float y, const u16* codes, int n, int align, u32 top, u32 bottom)
{
	if (!s_ready)
		return;
	font &= 1;
	float pen = x;
	if (align == FONT_CENTER)
		pen -= line_width(font, codes, n) * 0.5f;
	else if (align == FONT_RIGHT)
		pen -= line_width(font, codes, n);
	for (int i = 0; i < n; i++) {
		const Glyph* g = glyph(font, codes[i]);
		if (g)
			put_glyph(g, pen, y, top, bottom);
		pen += advance(font, codes[i]);
	}
}

void font_text(int font, float x, float y, const char* s, int align, u32 top, u32 bottom)
{
	if (!s_ready || !s)
		return;
	font &= 1;
	u16 line[128];
	while (*s) {
		int n = line_codes(&s, line, sizeof line / sizeof *line);
		font_codes(font, x, y, line, n, align, top, bottom);
		if (*s == '\n')
			s++;
		y += s_font[font].cell_h;
	}
}

float font_width(int font, const char* s)
{
	if (!s_ready || !s)
		return 0.0f;
	font &= 1;
	float best = 0.0f;
	u16 line[128];
	while (*s) {
		float lw = line_width(font, line, line_codes(&s, line, sizeof line / sizeof *line));
		best = lw > best ? lw : best;
		if (*s == '\n')
			s++;
	}
	return best;
}

C3D_Tex* font_texture(void) { return s_ready ? &s_tex : NULL; }
float font_line_px(int font) { return s_font[font & 1].cell_h * 0.46875f; }
float font_width_px(int font, const char* s) { return font_width(font, s) * 0.625f; }

bool font_ink_px(int font, const char* s, float* x0, float* y0, float* x1, float* y1)
{
	if (!s_ready || !s)
		return false;
	font &= 1;
	float pen = 0.0f, bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
	u16 line[128];
	int n = line_codes(&s, line, sizeof line / sizeof *line);
	for (int i = 0; i < n; i++) {
		const Glyph* g = glyph(font, line[i]);
		if (g && g->ix1) {
			float gx = floorf(pen + 0.625f + 0.5f);     // as font_layout_px places it
			bx0 = fminf(bx0, gx + g->ix0), bx1 = fmaxf(bx1, gx + g->ix1);
			by0 = fminf(by0, (float)g->iy0), by1 = fmaxf(by1, (float)g->iy1);
		}
		pen += advance(font, line[i]) * 0.625f;
	}
	if (bx1 < bx0)
		return false;
	*x0 = bx0, *y0 = by0, *x1 = bx1, *y1 = by1;
	return true;
}

int font_layout_px(int font, float x, float y, const char* s, int align, u32 top, u32 bottom, FontVert* out, int max)
{
	if (!s_ready || !s)
		return 0;
	font &= 1;
	int n = 0;
	u16 line[128];
	while (*s) {
		int k = line_codes(&s, line, sizeof line / sizeof *line);
		float pen = x;
		if (align == FONT_CENTER)
			pen -= line_width(font, line, k) * 0.625f * 0.5f;
		else if (align == FONT_RIGHT)
			pen -= line_width(font, line, k) * 0.625f;
		for (int i = 0; i < k; i++) {
			const Glyph* g = glyph(font, line[i]);
			if (g && g->w && n + 6 <= max) {
				float x0 = floorf(pen + 0.625f + 0.5f), y0 = floorf(y + 0.5f);
				float x1 = x0 + g->w, y1 = y0 + g->h;
				float u0 = g->x / (float)s_tex.width, v0 = g->y / (float)s_tex.height;
				float u1 = (g->x + g->w) / (float)s_tex.width, v1 = (g->y + g->h) / (float)s_tex.height;
				FontVert a = { x0, y0, u0, v0, top }, b = { x1, y0, u1, v0, top };
				FontVert c = { x0, y1, u0, v1, bottom }, d = { x1, y1, u1, v1, bottom };
				FontVert* v = &out[n];
				v[0] = a, v[1] = c, v[2] = d, v[3] = a, v[4] = d, v[5] = b;
				n += 6;
			}
			pen += advance(font, line[i]) * 0.625f;
		}
		if (*s == '\n')
			s++;
		y += s_font[font].cell_h * 0.46875f;
	}
	return n;
}

void font_image(C3D_Tex* tex, float x0, float y0, float x1, float y1, u32 rgba)
{
	if (!s_draw || !tex)
		return;
	float a0 = 200.0f + (x0 - 256.0f) * 0.625f, b0 = 120.0f + (y0 - 256.0f) * 0.46875f;
	float a1 = 200.0f + (x1 - 256.0f) * 0.625f, b1 = 120.0f + (y1 - 256.0f) * 0.46875f;
	FontVert tl = { a0, b0, 0, 0, rgba }, tr = { a1, b0, 1, 0, rgba }, bl = { a0, b1, 0, 1, rgba }, br = { a1, b1, 1, 1, rgba };
	const FontVert v[6] = { tl, bl, br, tl, br, tr };
	s_draw(tex, v, 6);
}

void font_flush(void)
{
	if (s_nverts && s_draw)
		s_draw(&s_tex, s_verts, s_nverts);
	s_nverts = 0;
}

void font_pc_color(int index, u32* top, u32* bottom)
{
	// 0x47eca0's table: PS2-style 0xBBGGRR pairs where 0x80 is full brightness.
	static const u32 table[17][2] = {
		{ 0x808080, 0x808080 }, { 0x806060, 0x806060 }, { 0x303080, 0x303080 }, { 0x608060, 0x608060 },
		{ 0x308080, 0x308080 }, { 0x808030, 0x808030 }, { 0x803080, 0x803080 }, { 0xa05050, 0x50a050 },
		{ 0x000030, 0x202050 }, { 0x803080, 0x803080 }, { 0x808080, 0x202020 }, { 0x2810e0, 0x2810e0 },
		{ 0xa05870, 0xa05870 }, { 0x38d088, 0x38d088 }, { 0x10f8f8, 0x10f8f8 }, { 0xa8b820, 0xa8b820 },
		{ 0x202020, 0x202020 },
	};
	const u32* c = table[index >= 0 && index < 17 ? index : 0];
	for (int k = 0; k < 2; k++) {
		u32 out = 0xff;
		for (int ch = 0; ch < 3; ch++) {
			u32 v = (c[k] >> (ch * 8) & 0xff) * 2;
			out |= (v > 255 ? 255 : v) << (24 - ch * 8);
		}
		*(k ? bottom : top) = out;
	}
}
