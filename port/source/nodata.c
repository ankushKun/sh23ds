#include "nodata.h"
#include "nodata_font.h"
#include "nodata_qr.h"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Files every finished pack has, one or more from each of the packer's main steps
// (tools/sh2conv/pack.py). The first is the one the folders below are searched for.
static const char* const s_need[] = {
	"ui/font.bin",            // ui (ui.py), read before anything is drawn
	"ui/font.t3x",
	"ui/title.t3x",
	"bg/ap/rooms.txt",        // rooms
	"chr/jms/hll_jms.s3c",    // james
	"sound/se/se.bnk",        // sound
};
#define NEED (int)(sizeof s_need / sizeof s_need[0])

// Where the folder ends up when it is copied one level off: the sh23ds folder alone at the root, the
// whole 3ds folder inside 3ds, the zip (writer.js: sh23ds-sd.zip) unzipped into a folder of its own,
// Docker's sd folder copied whole, or a make_pack.sh pack copied as it is.
static const char* const s_elsewhere[] = {
	"sdmc:/sh23ds",
	"sdmc:/3ds/3ds/sh23ds",
	"sdmc:/3ds/sh23ds/3ds/sh23ds",
	"sdmc:/3ds/sh23ds/sh23ds",
	"sdmc:/sh23ds-sd/3ds/sh23ds",
	"sdmc:/3ds/sh23ds-sd/3ds/sh23ds",
	"sdmc:/sd/3ds/sh23ds",
	"sdmc:/3ds/sd/3ds/sh23ds",
	"sdmc:/pack",
	"sdmc:/3ds/pack",
	"sdmc:/3ds/sh23ds/pack",
};
#define ELSEWHERE (int)(sizeof s_elsewhere / sizeof s_elsewhere[0])

#define GUIDE_URL "ankush.one/sh23ds/install.html"
#define PACKER_URL "ankush.one/sh23ds/packer.html"

// The website's colours (site/site.css), 0xRRGGBB
#define C_TEXT 0xdadada
#define C_TEXT2 0xc4c4c4
#define C_HI 0xf2f2f2
#define C_DIM 0x9a9a9a
#define C_LABEL 0x8a8d90
#define C_RED 0xb3121b
#define C_RED_HI 0xe2363c
#define C_PAPER 0xd9d3c5
#define C_INK 0x1b1b1b
#define C_INK2 0x4a463f
#define C_RULE 0x5a5550
#define C_RING 0xb4aea6

// In text, a change of style: back to the paragraph's own, or the addresses' (VT323, bright)
#define BASE "\x01"
#define MONO "\x02"

#define FOG_W 512                // the fog field, tiled: a column per x, FOG_H values down it
#define FOG_H 256
#define FADE_IN 60               // frames from black
#define FADE_OUT 18

// One screen: a background drawn under a still overlay each frame, in the framebuffer's order (a
// column per screen x, its rows bottom up, B G R)
typedef struct {
	int w;                       // 400 or 320; 240 high
	u8* ov;                      // while drawing: the overlay, rows top down, premultiplied R G B A
	u8* base;                    // B G R: black, the red glow, the vignette
	u8* fogm;                    // how much of the fog shows, NULL: none on this screen
	u8* ovr;                     // the overlay in framebuffer order, B G R A
} Screen;

typedef struct {
	int style;
	u32 rgb;
} Ink;

// What a framebuffer holds (grain frame, the two fog offsets, fade), so a still frame isn't drawn again:
// the grain changes every fifth frame and the fog moves a pixel every ninth, which spares the Old 3DS
typedef struct {
	u8* fb;
	int state[4];
} Shown;

static PrintConsole s_top;
static u8* s_fog;

static bool has_file(const char* dir, const char* file)
{
	char path[192];
	snprintf(path, sizeof path, "%s/%s", dir, file);
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static float smooth(float a, float b, float x)
{
	float t = (x - a) / (b - a);
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

static u32 hash(u32 x, u32 y, u32 seed)
{
	u32 h = x * 374761393u + y * 668265263u + seed * 2246822519u;
	h = (h ^ h >> 13) * 1274126177u;
	return h ^ h >> 16;
}

// ---------------------------------------------------------------- drawing into the overlay

static void blend(Screen* s, int x, int y, u32 rgb, int a)
{
	if (a <= 0 || x < 0 || y < 0 || x >= s->w || y >= 240)
		return;
	if (a > 255)
		a = 255;
	u8* p = s->ov + (y * s->w + x) * 4;
	int k = 255 - a;
	p[0] = (u8)(((rgb >> 16 & 255) * a + p[0] * k + 127) / 255);
	p[1] = (u8)(((rgb >> 8 & 255) * a + p[1] * k + 127) / 255);
	p[2] = (u8)(((rgb & 255) * a + p[2] * k + 127) / 255);
	p[3] = (u8)(a + (p[3] * k + 127) / 255);
}

static void rect(Screen* s, int x, int y, int w, int h, u32 rgb, int a)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			blend(s, i, j, rgb, a);
}

// A thin rule that fades out at both ends, as the PC's menus and the website draw them
static void rule(Screen* s, int x0, int x1, int y, u32 rgb, int a)
{
	float fade = (x1 - x0) * 0.35f;
	for (int x = x0; x < x1; x++) {
		float e = (x - x0 < x1 - 1 - x ? x - x0 : x1 - 1 - x) / fade;
		blend(s, x, y, rgb, (int)(a * smooth(0.0f, 1.0f, e)));
	}
}

// The website's mark: a small grid of red squares
static void mark(Screen* s, int x, int y)
{
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			rect(s, x + i * 3, y + j * 3, 2, 2, C_RED_HI, (i + j) & 1 ? 150 : 255);
}

static const NdGlyph* glyph(int style, int c)
{
	if (c < ND_FIRST || c > ND_LAST)
		c = '?';
	return &nd_font[style].glyph[c - ND_FIRST];
}

static int kern(int style, int a, int b)
{
	const NdFont* f = &nd_font[style];
	for (int i = 0; i < f->nkern; i++)
		if (f->kern[i].a == a && f->kern[i].b == b)
			return f->kern[i].k;
	return 0;
}

// Sets t[0..n) from x8 (1/ND_SUB pixel) on baseline y, following the style changes in it (ink
// carries the style from run to run), track 1/ND_SUB pixels apart. Draws only when s is set; the
// advance in 1/ND_SUB pixel either way.
static int run(Screen* s, int x8, int y, const char* t, int n, Ink* ink, Ink base, int track)
{
	int pen = 0, prev = 0;
	for (int i = 0; i < n; i++) {
		int c = (u8)t[i];
		if (c == 1 || c == 2) {
			*ink = c == 1 ? base : (Ink){ND_MONO, C_HI};
			prev = 0;
			continue;
		}
		if (prev)
			pen += kern(ink->style, prev, c);
		const NdGlyph* g = glyph(ink->style, c);
		if (s) {
			int gx = (x8 + pen + ND_SUB / 2) / ND_SUB + g->x, gy = y + g->y;
			for (int k = 0; k < g->w * g->h; k++) {
				int v = nd_bits[g->off + k / 2] >> (k & 1) * 4 & 15;
				if (v)
					blend(s, gx + k % g->w, gy + k / g->w, ink->rgb, v * 17);
			}
		}
		pen += g->adv + track;
		prev = c;
	}
	return pen;
}

static int text_w(int style, const char* t, int track)
{
	Ink ink = {style, 0};
	return (run(NULL, 0, 0, t, (int)strlen(t), &ink, ink, track) - track + ND_SUB / 2) / ND_SUB;
}

static void text(Screen* s, int style, int x, int y, u32 rgb, const char* t, int track)
{
	Ink ink = {style, rgb};
	run(s, x * ND_SUB, y, t, (int)strlen(t), &ink, ink, track);
}

// A paragraph wrapped at maxw, its baselines lh apart from y; the baseline after its last line
static int para(Screen* s, int x, int y, int maxw, int lh, int style, u32 rgb, const char* t)
{
	Ink base = {style, rgb}, ink = base;
	int pen = 0;
	while (*t) {
		const char* w = t;
		while (*t && *t != ' ' && *t != '\n')
			t++;
		Ink probe = ink;
		int ww = run(NULL, 0, 0, w, (int)(t - w), &probe, base, 0);
		int space = pen ? glyph(ink.style, ' ')->adv + (ink.style == ND_MONO ? 0 : ND_SUB) : 0;   // airier
		if (pen && pen + space + ww > maxw * ND_SUB)
			y += lh, pen = space = 0;
		pen += space;
		pen += run(s, x * ND_SUB + pen, y, w, (int)(t - w), &ink, base, 0);
		if (*t == '\n')
			y += lh, pen = 0;
		if (*t)
			t++;
	}
	return y + lh;
}

// "SILENT HILL 2 . <what>" in spaced capitals, after the mark
static void label(Screen* s, int x, int y, const char* what)
{
	mark(s, x, y - 9);
	x += 18;
	text(s, ND_LABEL, x, y, C_LABEL, "SILENT HILL 2", 4 * ND_SUB);
	x += text_w(ND_LABEL, "SILENT HILL 2", 4 * ND_SUB) + 9;
	rect(s, x, y - 5, 2, 2, C_LABEL, 255);
	text(s, ND_LABEL, x + 11, y, C_LABEL, what, 4 * ND_SUB);
}

// ---------------------------------------------------------------- the background

static float lattice(int x, int y, int seed)
{
	return hash((u32)x, (u32)y, (u32)seed) / 4294967296.0f;
}

// Value noise that tiles over the fog field, cells cw x ch
static float vnoise(int x, int y, int cw, int ch, int seed)
{
	int nx = FOG_W / cw, ny = FOG_H / ch, cx = x / cw, cy = y / ch;
	float fx = smooth(0.0f, 1.0f, (float)(x % cw) / cw), fy = smooth(0.0f, 1.0f, (float)(y % ch) / ch);
	float a = lattice(cx % nx, cy % ny, seed), b = lattice((cx + 1) % nx, cy % ny, seed);
	float c = lattice(cx % nx, (cy + 1) % ny, seed), d = lattice((cx + 1) % nx, (cy + 1) % ny, seed);
	float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
	return top + (bottom - top) * fy;
}

// Wisps of fog, wider than high: three octaves, the thin parts cut away
static void fog_init(void)
{
	for (int x = 0; x < FOG_W; x++)
		for (int y = 0; y < FOG_H; y++) {
			float n = 0.5f * vnoise(x, y, 128, 64, 1) + 0.3f * vnoise(x, y, 64, 32, 2) + 0.2f * vnoise(x, y, 32, 16, 3);
			float f = smooth(0.38f, 0.85f, n);
			s_fog[x * FOG_H + y] = (u8)(f * 255.0f);
		}
}

// Black with a dull red glow around (gx, gy), the website's vignette, and on the top screen the fog
// thicker towards the ground
static void background(Screen* s, float gx, float gy, float gr, float glow)
{
	for (int y = 0; y < 240; y++)
		for (int x = 0; x < s->w; x++) {
			float dx = (x - s->w * 0.5f) / (s->w * 0.5f), dy = (y - 120.0f) / 120.0f;
			float v = 1.0f - 0.85f * smooth(0.5f, 1.5f, sqrtf(dx * dx + dy * dy));
			float ex = (x - gx) / gr, ey = (y - gy) / gr;
			float g = glow * expf(-(ex * ex + ey * ey));
			int i = (x * 240 + 239 - y) * 3;
			s->base[i + 0] = (u8)((5.0f + 27.0f * g) * v);
			s->base[i + 1] = (u8)((5.0f + 18.0f * g) * v);
			s->base[i + 2] = (u8)((5.0f + 179.0f * g) * v);
			if (s->fogm)
				s->fogm[i / 3] = (u8)(255.0f * v * (0.25f + 0.75f * smooth(30.0f, 240.0f, (float)y)));
		}
}

// ---------------------------------------------------------------- the two screens

static void top_screen(Screen* s, const char* pack_dir, const char* found, const char* const* missing, int nmissing)
{
	const int x = 26, w = 400 - 2 * x;
	char line[160];
	label(s, x, 30, "GAME DATA");
	text(s, ND_TITLE, x - 1, 70, C_TEXT, found ? "WRONG FOLDER" : nmissing ? "INCOMPLETE" : "NOT FOUND", ND_SUB);
	if (found) {
		para(s, x, 96, w, 19, ND_ITALIC, C_DIM,
		     "The game data is on the card, one folder off from where sh23ds looks for it.");
		text(s, ND_LABEL, x, 148, C_LABEL, "FOUND", 2 * ND_SUB);
		snprintf(line, sizeof line, "%s/", found);
		text(s, ND_MONO, x + 72, 148, C_HI, line, 0);
		text(s, ND_LABEL, x, 172, C_LABEL, "MOVE TO", 2 * ND_SUB);
		snprintf(line, sizeof line, "%s/", pack_dir);
		text(s, ND_MONO, x + 72, 172, C_HI, line, 0);
		para(s, x, 204, w, 18, ND_BODY, C_TEXT2,
		     "Move it on a computer or with GodMode9, and keep any save*.dat files already there.");
	} else if (nmissing) {
		snprintf(line, sizeof line, "Some of it is missing from " MONO "%s/" BASE ". The copy or the packer "
		         "stopped before the end.", pack_dir);
		int y = para(s, x, 96, w, 19, ND_ITALIC, C_DIM, line) + 12;
		text(s, ND_LABEL, x, y, C_LABEL, "MISSING", 2 * ND_SUB);
		int shown = nmissing > 3 ? 2 : nmissing;
		for (int i = 0; i < shown; i++, y += 18)
			text(s, ND_MONO, x + 72, y, C_HI, missing[i], 0);
		if (shown < nmissing) {
			snprintf(line, sizeof line, "and %d more", nmissing - shown);
			text(s, ND_MONO, x + 72, y, C_DIM, line, 0);
			y += 18;
		}
		para(s, x, y + 10, w, 18, ND_BODY, C_TEXT2,
		     "Copy the 3ds folder from the packer to the card again. Back up your save*.dat files first.");
	} else {
		para(s, x, 96, w, 19, ND_ITALIC, C_DIM,
		     "The engine is here. The game comes from your own PC copy, converted once on a computer.");
		static const char* const steps[] = {
			"Open the packer on a computer:\n" MONO PACKER_URL,
			"Copy the 3ds folder it makes to the SD card",
			"Put the card back and start sh23ds again",
		};
		int y = 146;
		for (int i = 0; i < 3; i++) {
			char n[2] = {(char)('1' + i), 0};
			text(s, ND_MONO, x, y, C_RED_HI, n, 0);
			y = para(s, x + 20, y, w - 20, 20, ND_BODY, C_TEXT2, steps[i]) + 3;
		}
	}
}

static void paper(Screen* s, int x0, int y0, int w, int h)
{
	// a soft shadow, dropped a little
	for (int y = y0 - 8; y < y0 + h + 12; y++)
		for (int x = x0 - 10; x < x0 + w + 12; x++) {
			float dx = x < x0 + 2 ? x0 + 2 - x : x > x0 + w + 1 ? x - (x0 + w + 1) : 0;
			float dy = y < y0 + 4 ? y0 + 4 - y : y > y0 + h + 3 ? y - (y0 + h + 3) : 0;
			float d = sqrtf(dx * dx + dy * dy) / 10.0f;
			if (d < 1.0f)
				blend(s, x, y, 0, (int)(170.0f * (1.0f - d) * (1.0f - d)));
		}
	// the memo: burnt towards the edges, mottled, a few specks, the edge a little ragged
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++) {
			int ex = x - x0 < x0 + w - 1 - x ? x - x0 : x0 + w - 1 - x;
			int ey = y - y0 < y0 + h - 1 - y ? y - y0 : y0 + h - 1 - y;
			int e = ex < ey ? ex : ey;
			u32 r = hash((u32)x, (u32)y, 7);
			float stain = s_fog[((x * 3) & (FOG_W - 1)) * FOG_H + ((y * 3) & (FOG_H - 1))] / 255.0f;
			float k = 1.0f - (0.18f + 0.16f * stain) * (1.0f - smooth(0.0f, 14.0f + 8.0f * stain, (float)e));
			k *= 0.92f + 0.08f * (1.0f - stain);
			if ((r >> 8 & 255) < 2)
				k *= 0.85f;
			k *= 0.97f + 0.03f * (r >> 16 & 255) / 255.0f;
			u32 c = (u32)((C_PAPER >> 16 & 255) * k) << 16 | (u32)((C_PAPER >> 8 & 255) * k) << 8 |
			        (u32)((C_PAPER & 255) * k);
			blend(s, x, y, c, 255);
		}
}

static void bottom_screen(Screen* s, int* touch)
{
	const int pw = 214, ph = 192, px = (320 - pw) / 2, py = 6;
	paper(s, px, py, pw, ph);
	const char* head = "the install guide";
	text(s, ND_ITALIC, (320 - text_w(ND_ITALIC, head, 0)) / 2, py + 23, C_INK2, head, 0);
	int q = 4, side = NODATA_QR_SIZE * q, qx = (320 - side) / 2, qy = py + 33;
	for (int y = 0; y < NODATA_QR_SIZE; y++)
		for (int x = 0; x < NODATA_QR_SIZE; x++)
			if (nodata_qr[y] >> x & 1)
				rect(s, qx + x * q, qy + y * q, q, q, C_INK, 255);
	text(s, ND_LABEL, (320 - text_w(ND_LABEL, GUIDE_URL, 0)) / 2, py + ph - 15, C_INK, GUIDE_URL, 0);

	// the footer, as touchui.c draws it: a rule, then the button and what it does
	rule(s, 20, 300, 206, C_RULE, 255);
	const char* what = "Close";
	int tw = text_w(ND_LABEL, "START", ND_SUB), bw = tw + 12, ww = text_w(ND_BODY, what, 0);
	int x = (320 - (bw + 8 + ww)) / 2, y = 214, h = 17;
	for (int i = 1; i < bw - 1; i++) {
		blend(s, x + i, y, C_RING, 255);
		blend(s, x + i, y + h - 1, C_RING, 255);
	}
	for (int j = 1; j < h - 1; j++) {
		blend(s, x, y + j, C_RING, 255);
		blend(s, x + bw - 1, y + j, C_RING, 255);
	}
	text(s, ND_LABEL, x + 6, y + 13, C_RING, "START", ND_SUB);
	text(s, ND_BODY, x + bw + 8, y + 13, C_TEXT2, what, 0);
	touch[0] = x - 8, touch[1] = y - 6, touch[2] = x + bw + 8 + ww + 8, touch[3] = y + h + 6;
}

// ---------------------------------------------------------------- each frame

static bool screen_init(Screen* s, int w, bool fog)
{
	s->w = w;
	s->ov = calloc((size_t)w * 240, 4);
	s->base = malloc((size_t)w * 240 * 3);
	s->ovr = malloc((size_t)w * 240 * 4);
	s->fogm = fog ? malloc((size_t)w * 240) : NULL;
	return s->ov && s->base && s->ovr && (s->fogm || !fog);
}

static void screen_free(Screen* s)
{
	free(s->ov), free(s->base), free(s->ovr), free(s->fogm);
}

// The overlay into the framebuffer's order
static void screen_finish(Screen* s)
{
	for (int y = 0; y < 240; y++)
		for (int x = 0; x < s->w; x++) {
			const u8* p = s->ov + (y * s->w + x) * 4;
			u8* q = s->ovr + (x * 240 + 239 - y) * 4;
			q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = p[3];
		}
	free(s->ov);
	s->ov = NULL;
}

// Background, fog drifting two ways, film grain (four frames, each held for five: the port's own
// grain, port/UI.md), the overlay, then the fade; st as in Shown
static void screen_frame(const Screen* s, u8* fb, const int* st)
{
	static const u32 seeds[4] = {0x2c9277b5u, 0x9e3779b9u, 0x7f4a7c15u, 0x51ed270bu};
	u32 seed = seeds[st[0]];
	u32 o1 = (u32)st[1], o2 = 4096u + 300u - (u32)st[2];
	int fade = st[3];
	for (int x = 0; x < s->w; x++) {
		const u8* f1 = s_fog + ((x + o1) & (FOG_W - 1)) * FOG_H;
		const u8* f2 = s_fog + ((x + o2) & (FOG_W - 1)) * FOG_H;
		for (int j = 0; j < 240; j++) {
			int i = x * 240 + j;
			const u8* b = s->base + i * 3;
			const u8* o = s->ovr + i * 4;
			seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
			int add = ((int)((seed >> 24 & 15) + (seed >> 16 & 15)) - 15) / 2;
			if (s->fogm) {
				int y = 239 - j;
				add += (f1[y] + f2[(y + 101) & (FOG_H - 1)]) * s->fogm[i] >> 10;
			}
			int k = 256 - o[3];
			for (int c = 0; c < 3; c++) {
				int v = ((b[c] + add) * k >> 8) + o[c];
				v = v < 0 ? 0 : v > 255 ? 255 : v;
				fb[i * 3 + c] = (u8)(v * fade >> 8);
			}
		}
	}
}

static void show(const Screen* s, Shown* held, int screen, u32 t, int fade)
{
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	int st[4] = {(int)(t / 5 & 3), s->fogm ? (int)(t / 9) : 0, s->fogm ? (int)(t / 15) : 0, fade};
	Shown* h = held[0].fb == fb || !held[0].fb ? &held[0] : &held[1];
	if (h->fb == fb && !memcmp(h->state, st, sizeof st))
		return;
	h->fb = fb;
	memcpy(h->state, st, sizeof st);
	screen_frame(s, fb, st);
}

// Plain text, if the screens' memory can't be had
static void fallback(void)
{
	consoleInit(GFX_TOP, &s_top);
	printf("\n  sh23ds: the game data was not found in\n  sdmc:/3ds/sh23ds/\n\n"
	       "  Install guide:\n  " GUIDE_URL "\n\n  Press START to return to the HOME Menu.");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
}

bool nodata_check(const char* pack_dir)
{
	const char* missing[NEED];
	int nmissing = 0;
	for (int i = 0; i < NEED; i++)
		if (!has_file(pack_dir, s_need[i]))
			missing[nmissing++] = s_need[i];
	if (!nmissing)
		return true;
	const char* found = NULL;
	if (nmissing == NEED)
		for (int i = 0; i < ELSEWHERE && !found; i++)
			if (has_file(s_elsewhere[i], s_need[0]))
				found = s_elsewhere[i];

	Screen top = {0}, bottom = {0};
	s_fog = malloc(FOG_W * FOG_H);
	if (!s_fog || !screen_init(&top, 400, true) || !screen_init(&bottom, 320, false)) {
		screen_free(&top), screen_free(&bottom), free(s_fog);
		fallback();
		return false;
	}
	int touch[4];
	fog_init();
	background(&top, 110.0f, 120.0f, 150.0f, 0.2f);
	background(&bottom, 160.0f, 110.0f, 170.0f, 0.1f);
	top_screen(&top, pack_dir, found, missing, nmissing == NEED ? 0 : nmissing);
	bottom_screen(&bottom, touch);
	screen_finish(&top);
	screen_finish(&bottom);

	u32 t = 0, closing = 0;
	Shown held_top[2] = {0}, held_bottom[2] = {0};
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			if (tp.px >= touch[0] && tp.px < touch[2] && tp.py >= touch[1] && tp.py < touch[3])
				down |= KEY_START;
		}
		if ((down & KEY_START) && !closing)
			closing = 1;
		int fade = t < FADE_IN ? (int)(t * 256 / FADE_IN) : 256;
		if (closing) {
			if (closing > FADE_OUT)
				break;
			int out = (int)((FADE_OUT - closing) * 256 / FADE_OUT);
			fade = fade < out ? fade : out;
			closing++;
		}
		show(&top, held_top, GFX_TOP, t, fade);
		show(&bottom, held_bottom, GFX_BOTTOM, t, fade);
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
		t++;
	}
	screen_free(&top), screen_free(&bottom), free(s_fog);
	return false;
}
