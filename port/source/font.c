#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <tex3ds.h>

#define MAX_CODE 0x200
#define MAX_GLYPHS 1024      // per frame: a full results screen is about 450

typedef struct { u16 x, y; u8 w, h, adv; } Glyph;    // atlas rectangle (3DS pixels), advance (PC units)

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
	s_ready = true;
	return true;
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
	const Glyph* g = glyph(font, code);
	return g ? g->adv : code < 0xe0 ? 7 : s_font[font].cell_w + 2;
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
		int n = 0;
		for (; *s && *s != '\n'; s++) {
			u8 c = (u8)*s;
			if (c >= 0x20 && n < (int)(sizeof line / sizeof *line))
				line[n++] = (u16)(c - 0x20);
		}
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
	int w = 0;
	for (;; s++) {
		if (!*s || *s == '\n') {
			float lw = w > 2 ? (float)(w - 2) : 0.0f;
			best = lw > best ? lw : best;
			w = 0;
			if (!*s)
				break;
		} else if ((u8)*s >= 0x20) {
			w += advance(font, (u8)*s - 0x20);
		}
	}
	return best;
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
