#include "nodata.h"
#include "nodata_font.h"
#include "nodata_qr.h"
#include <3ds.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

#define LIST_NAME "files.txt"       // every file of the pack, one per line, written last by the packer
#define LIST_TOP 36              // the missing files' list on the touch screen, rows ROW high
#define LIST_BOTTOM 200
#define ROW 18

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
	int clip[4];                 // where drawing reaches: x0, y0, x1, y1
} Screen;

typedef struct {
	int style;
	u32 rgb;
} Ink;

// What a framebuffer holds (grain frame, the two fog offsets, fade, overlay serial), so a still frame
// isn't drawn again: the grain changes every fifth frame and the fog moves a pixel every ninth, which
// spares the Old 3DS
typedef struct {
	u8* fb;
	int state[5];
} Shown;

// A footer prompt, as touchui.c draws them: the button (a ring with its letter, or a tag), then what
// it does; tapping it is the same as the button
typedef struct {
	const char* key;
	const char* what;
	u32 button;
	int box[4];
} Prompt;

enum { KIND_NONE, KIND_ELSEWHERE, KIND_INCOMPLETE };

static PrintConsole s_top;
static u8* s_fog;
static char* s_list;             // LIST_NAME, read whole; the paths below point into it
static const char** s_missing;   // what the card lacks, folder by folder
static int s_nmissing;
static int s_listed;             // files in LIST_NAME, 0 without one (a pack from an older packer)

// The list's rows: each folder's heading (with how many of its files are missing), then its files
typedef struct {
	const char* path;
	int dl;                      // the folder part's length
	int count;                   // a heading's files, -1 on a file's row
} Row;
static Row* s_rows;
static int s_nrows;

static bool has_file(const char* dir, const char* file)
{
	char path[192];
	snprintf(path, sizeof path, "%s/%s", dir, file);
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// The length of a path's folder part, 0 at the pack's root
static int dirlen(const char* p)
{
	const char* e = strrchr(p, '/');
	return e ? (int)(e - p) : 0;
}

// Folder by folder, then by name, so each folder is read once
static int cmp_path(const void* a, const void* b)
{
	const char *p = *(const char* const*)a, *q = *(const char* const*)b;
	int lp = dirlen(p), lq = dirlen(q);
	int c = strncmp(p, q, (size_t)(lp < lq ? lp : lq));
	if (!c)
		c = lp - lq;
	return c ? c : strcmp(p + lp, q + lq);
}

static int cmp_name(const void* a, const void* b)
{
	return strcasecmp(*(const char* const*)a, *(const char* const*)b);   // FAT ignores case
}

// LIST_NAME against the card: each folder's names read once with readdir, which is far quicker on
// the SD card than a stat per file. False without the list.
static bool scan(const char* pack_dir)
{
	char path[256];
	snprintf(path, sizeof path, "%s/" LIST_NAME, pack_dir);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	s_list = n > 0 ? malloc((size_t)n + 1) : NULL;
	if (!s_list || fread(s_list, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(s_list);
		s_list = NULL;
		return false;
	}
	fclose(f);
	s_list[n] = 0;
	int cap = 1;
	for (long i = 0; i < n; i++)
		cap += s_list[i] == '\n';
	const char** all = malloc(cap * sizeof *all);
	s_missing = malloc(cap * sizeof *s_missing);
	if (!all || !s_missing) {
		free(all);
		return false;
	}
	int nall = 0;
	for (char* p = s_list; *p;) {
		char* e = strchr(p, '\n');
		if (e)
			*e = 0;
		size_t len = strlen(p);
		if (len && p[len - 1] == '\r')
			p[len - 1] = 0;
		if (*p && *p != '#')
			all[nall++] = p;
		if (!e)
			break;
		p = e + 1;
	}
	qsort(all, nall, sizeof *all, cmp_path);

	char** names = NULL;
	int nnames = 0, capnames = 0;
	const char* dir = NULL;
	for (int i = 0; i < nall; i++) {
		int dl = dirlen(all[i]);
		if (!dir || dirlen(dir) != dl || strncmp(dir, all[i], (size_t)dl)) {
			for (int k = 0; k < nnames; k++)
				free(names[k]);
			nnames = 0;
			dir = all[i];
			snprintf(path, sizeof path, dl ? "%s/%.*s" : "%s", pack_dir, dl, all[i]);
			DIR* d = opendir(path);
			struct dirent* e;
			while (d && (e = readdir(d))) {
				if (nnames == capnames) {
					char** more = realloc(names, (capnames = capnames * 2 + 64) * sizeof *names);
					if (!more)
						break;
					names = more;
				}
				if ((names[nnames] = strdup(e->d_name)))
					nnames++;
			}
			if (d)
				closedir(d);
			qsort(names, nnames, sizeof *names, cmp_name);
		}
		const char* base = all[i] + dl + (dl ? 1 : 0);
		if (!nnames || !bsearch(&base, names, nnames, sizeof *names, cmp_name))
			s_missing[s_nmissing++] = all[i];
	}
	for (int k = 0; k < nnames; k++)
		free(names[k]);
	free(names);
	free(all);
	s_listed = nall;
	return true;
}

static void list_rows(void)
{
	s_rows = malloc((2 * (size_t)s_nmissing + 1) * sizeof *s_rows);
	s_nrows = 0;
	Row* head = NULL;
	for (int i = 0; s_rows && i < s_nmissing; i++) {
		const char* p = s_missing[i];
		int dl = dirlen(p);
		if (!head || head->dl != dl || strncmp(head->path, p, (size_t)dl)) {
			head = &s_rows[s_nrows++];
			*head = (Row){p, dl, 0};
		}
		head->count++;
		s_rows[s_nrows++] = (Row){p, dl, -1};
	}
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
	if (a <= 0 || x < s->clip[0] || y < s->clip[1] || x >= s->clip[2] || y >= s->clip[3])
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

static void top_screen(Screen* s, const char* pack_dir, int kind, const char* found)
{
	const int x = 26, w = 400 - 2 * x;
	char line[160];
	label(s, x, 30, "GAME DATA");
	text(s, ND_TITLE, x - 1, 70, C_TEXT,
	     kind == KIND_ELSEWHERE ? "WRONG FOLDER" : kind == KIND_INCOMPLETE ? "INCOMPLETE" : "NOT FOUND", ND_SUB);
	if (kind == KIND_ELSEWHERE) {
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
	} else if (kind == KIND_INCOMPLETE) {
		snprintf(line, sizeof line, "Some of it is missing from " MONO "%s/" BASE ". The copy or the packer "
		         "stopped before the end.", pack_dir);
		int y = para(s, x, 96, w, 19, ND_ITALIC, C_DIM, line) + 12;
		text(s, ND_LABEL, x, y, C_LABEL, "MISSING", 2 * ND_SUB);
		if (s_listed)
			snprintf(line, sizeof line, "%d of %d files", s_nmissing, s_listed);
		else
			snprintf(line, sizeof line, "%d key file%s", s_nmissing, s_nmissing == 1 ? "" : "s");
		text(s, ND_MONO, x + 72, y, C_HI, line, 0);
		for (int i = 0; i < s_nmissing && i < 2 && y + 18 + 46 <= 240; i++) {   // the note's two lines below
			y += 18;
			snprintf(line, sizeof line, "%.40s", s_missing[i]);
			text(s, ND_MONO, x + 72, y, C_DIM, line, 0);
		}
		para(s, x, y + 28, w, 18, ND_BODY, C_TEXT2,
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

static void guide_view(Screen* s)
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
}

// The list's height in pixels, the note about older packs included
static int list_height(void)
{
	return s_nrows * ROW + (s_listed ? 0 : 44);
}

static int list_max_scroll(void)
{
	int h = list_height() - (LIST_BOTTOM - LIST_TOP);
	return h > 0 ? h : 0;
}

// t into line, cut in the middle to fit chars characters
static void fit(char* line, size_t size, const char* t, int n, int chars)
{
	if (n <= chars)
		snprintf(line, size, "%.*s", n, t);
	else
		snprintf(line, size, "%.*s...%.*s", chars / 2 - 2, t, chars - chars / 2 - 1, t + n - (chars - chars / 2 - 1));
}

// Every missing file under its folder's heading, scrolled by scroll pixels
static void list_view(Screen* s, int scroll)
{
	char line[64];
	text(s, ND_LABEL, 16, 23, C_LABEL, "MISSING FILES", 3 * ND_SUB);
	if (s_listed)
		snprintf(line, sizeof line, "%d OF %d", s_nmissing, s_listed);
	else
		snprintf(line, sizeof line, "%d", s_nmissing);
	text(s, ND_LABEL, 304 - text_w(ND_LABEL, line, ND_SUB), 23, C_TEXT2, line, ND_SUB);
	rule(s, 10, 310, 30, C_RULE, 255);

	s->clip[1] = LIST_TOP, s->clip[3] = LIST_BOTTOM;
	for (int r = scroll / ROW; r < s_nrows && r * ROW - scroll < LIST_BOTTOM - LIST_TOP; r++) {
		const Row* row = &s_rows[r];
		int y = LIST_TOP + r * ROW - scroll;
		if (row->count >= 0) {           // a folder: the website's square, its path, how many
			rect(s, 13, y + 7, 3, 3, C_RED_HI, 255);
			char n[16];
			snprintf(n, sizeof n, "%d", row->count);
			int nw = text_w(ND_LABEL, n, 0);
			if (row->dl)
				fit(line, sizeof line, row->path, row->dl, (296 - nw - 22 - 10) / 6 - 1);
			else
				line[0] = 0;
			strcat(line, "/");
			text(s, ND_LABEL, 22, y + 13, C_LABEL, line, 0);
			text(s, ND_LABEL, 300 - nw, y + 13, C_DIM, n, 0);
		} else {                         // a file in it
			if (r & 1)
				rect(s, 10, y, 292, ROW, 0xffffff, 7);
			const char* name = row->path + row->dl + (row->dl ? 1 : 0);
			fit(line, sizeof line, name, (int)strlen(name), (300 - 32) / 6);
			text(s, ND_LABEL, 32, y + 13, C_HI, line, 0);
		}
	}
	if (!s_listed)
		para(s, 16, LIST_TOP + s_nrows * ROW - scroll + 22, 280, 18, ND_ITALIC, C_DIM,
		     "An older packer made this pack, so only its key files could be checked.");
	s->clip[1] = 0, s->clip[3] = 240;

	// the rows fade out under the edges, so the list reads as one that goes on
	for (int k = 0; k < 10; k++) {
		int a = 256 * k / 10;
		for (int x = 0; x < 320; x++) {
			u8* top = s->ov + ((LIST_TOP + k) * 320 + x) * 4;
			u8* bottom = s->ov + ((LIST_BOTTOM - 1 - k) * 320 + x) * 4;
			for (int c = 0; c < 4; c++) {
				if (scroll > 0)
					top[c] = (u8)(top[c] * a >> 8);
				if (scroll < list_max_scroll())
					bottom[c] = (u8)(bottom[c] * a >> 8);
			}
		}
	}
	int max = list_max_scroll();
	if (max) {
		int track = LIST_BOTTOM - LIST_TOP, thumb = track * track / list_height();
		thumb = thumb < 14 ? 14 : thumb;
		int y = LIST_TOP + (track - thumb) * scroll / max;
		rect(s, 307, LIST_TOP, 1, track, C_RULE, 160);
		rect(s, 306, y, 3, thumb, C_RING, 255);
	}
}

static void ring(Screen* s, float cx, float cy, float r, u32 rgb)
{
	for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); y++)
		for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); x++) {
			float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
			float a = 1.0f - fabsf(d - r);
			if (a > 0.0f)
				blend(s, x, y, rgb, (int)(255.0f * a));
		}
}

// The footer, as touchui.c draws it: a rule, then the prompts side by side, centred
static void prompts(Screen* s, Prompt* p, int n)
{
	rule(s, 20, 300, 206, C_RULE, 255);
	int w[4], total = 0;
	for (int i = 0; i < n; i++) {
		int kw = p[i].key[1] ? text_w(ND_LABEL, p[i].key, ND_SUB) + 12 : 15;
		w[i] = kw + 7 + text_w(ND_BODY, p[i].what, 0);
		total += w[i] + (i ? 20 : 0);
	}
	int x = (320 - total) / 2, y = 214, h = 17;
	for (int i = 0; i < n; i++) {
		if (p[i].key[1]) {               // a tag, for START and the shoulder buttons
			int bw = text_w(ND_LABEL, p[i].key, ND_SUB) + 12;
			for (int k = 1; k < bw - 1; k++)
				blend(s, x + k, y, C_RING, 255), blend(s, x + k, y + h - 1, C_RING, 255);
			for (int k = 1; k < h - 1; k++)
				blend(s, x, y + k, C_RING, 255), blend(s, x + bw - 1, y + k, C_RING, 255);
			text(s, ND_LABEL, x + 6, y + 13, C_RING, p[i].key, ND_SUB);
			text(s, ND_BODY, x + bw + 7, y + 13, C_TEXT2, p[i].what, 0);
		} else {                         // a ring with the letter
			ring(s, x + 7.5f, y + 8.5f, 7.0f, C_RING);
			text(s, ND_LABEL, x + 8 - text_w(ND_LABEL, p[i].key, 0) / 2, y + 13, C_RING, p[i].key, 0);
			text(s, ND_BODY, x + 22, y + 13, C_TEXT2, p[i].what, 0);
		}
		p[i].box[0] = x - 8, p[i].box[1] = y - 6, p[i].box[2] = x + w[i] + 8, p[i].box[3] = y + h + 6;
		x += w[i] + 20;
	}
}

// ---------------------------------------------------------------- each frame

static bool screen_init(Screen* s, int w, bool fog)
{
	s->w = w;
	s->ov = calloc((size_t)w * 240, 4);
	s->base = malloc((size_t)w * 240 * 3);
	s->ovr = malloc((size_t)w * 240 * 4);
	s->fogm = fog ? malloc((size_t)w * 240) : NULL;
	s->clip[0] = s->clip[1] = 0, s->clip[2] = w, s->clip[3] = 240;
	return s->ov && s->base && s->ovr && (s->fogm || !fog);
}

static void screen_free(Screen* s)
{
	free(s->ov), free(s->base), free(s->ovr), free(s->fogm);
}

// The overlay into the framebuffer's order; keep: the overlay stays for drawing again
static void screen_finish(Screen* s, bool keep)
{
	for (int y = 0; y < 240; y++)
		for (int x = 0; x < s->w; x++) {
			const u8* p = s->ov + (y * s->w + x) * 4;
			u8* q = s->ovr + (x * 240 + 239 - y) * 4;
			q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = p[3];
		}
	if (!keep) {
		free(s->ov);
		s->ov = NULL;
	}
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

static void show(const Screen* s, Shown* held, int screen, u32 t, int fade, int serial)
{
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	int st[5] = {(int)(t / 5 & 3), s->fogm ? (int)(t / 9) : 0, s->fogm ? (int)(t / 15) : 0, fade, serial};
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

// The touch screen's overlay for the view: the guide or the list, then the prompts for it
static void bottom_draw(Screen* s, bool list, int scroll, Prompt* p, int* np)
{
	memset(s->ov, 0, (size_t)s->w * 240 * 4);
	int n = 0;
	if (list) {
		list_view(s, scroll);
		p[n++] = (Prompt){"B", "Back", KEY_B, {0}};
	} else {
		guide_view(s);
		if (s_nmissing)
			p[n++] = (Prompt){"Y", "Missing files", KEY_Y, {0}};
	}
	p[n++] = (Prompt){"START", "Close", KEY_START, {0}};
	prompts(s, p, n);
	*np = n;
	screen_finish(s, true);
}

bool nodata_check(const char* pack_dir)
{
	const char* key[NEED];
	int nkey = 0;
	for (int i = 0; i < NEED; i++)
		if (!has_file(pack_dir, s_need[i]))
			key[nkey++] = s_need[i];
	if (!nkey)
		return true;
	// something is missing: now the whole list, if the pack has one
	bool listed = scan(pack_dir);
	if (!listed || !s_nmissing) {
		free(s_missing);
		s_missing = malloc(sizeof key);
		s_nmissing = 0, s_listed = 0;
		for (int i = 0; s_missing && i < nkey; i++)
			s_missing[s_nmissing++] = key[i];
	}
	const char* found = NULL;
	if (nkey == NEED && !listed)
		for (int i = 0; i < ELSEWHERE && !found; i++)
			if (has_file(s_elsewhere[i], s_need[0]))
				found = s_elsewhere[i];
	int kind = found ? KIND_ELSEWHERE : nkey == NEED && !listed ? KIND_NONE : KIND_INCOMPLETE;
	if (kind != KIND_INCOMPLETE)
		s_nmissing = 0;              // nothing to list: the whole pack is missing
	list_rows();

	Screen top = {0}, bottom = {0};
	s_fog = malloc(FOG_W * FOG_H);
	if (!s_fog || !screen_init(&top, 400, true) || !screen_init(&bottom, 320, false)) {
		screen_free(&top), screen_free(&bottom), free(s_fog), free(s_missing), free(s_list), free(s_rows);
		fallback();
		return false;
	}
	fog_init();
	background(&top, 110.0f, 120.0f, 150.0f, 0.2f);
	background(&bottom, 160.0f, 110.0f, 170.0f, 0.1f);
	top_screen(&top, pack_dir, kind, found);
	screen_finish(&top, false);
	Prompt prompt[3];
	int nprompt, serial = 0, scroll = 0, drag = -1;
	bool list = false;
	bottom_draw(&bottom, list, scroll, prompt, &nprompt);

	u32 t = 0, closing = 0;
	Shown held_top[2] = {0}, held_bottom[2] = {0};
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		touchPosition tp = {0};
		if (held & KEY_TOUCH)
			hidTouchRead(&tp);
		if (down & KEY_TOUCH)
			for (int i = 0; i < nprompt; i++)
				if (tp.px >= prompt[i].box[0] && tp.px < prompt[i].box[2] && tp.py >= prompt[i].box[1] &&
				    tp.py < prompt[i].box[3])
					down |= prompt[i].button;
		if ((down & KEY_START) && !closing)
			closing = 1;
		bool redraw = false;
		if (!closing && ((!list && (down & KEY_Y) && s_nmissing) || (list && (down & KEY_B)))) {
			list = !list;
			scroll = 0, drag = -1;
			redraw = true;
		}
		if (list && !closing) {
			// the D-Pad and Circle Pad scroll (the Circle Pad by how far it is pushed), L and R a
			// page, and the list follows a finger dragged on it
			int dy = 0, page = LIST_BOTTOM - LIST_TOP - ROW;
			circlePosition cp;
			hidCircleRead(&cp);
			if (held & KEY_DDOWN)
				dy += 4;
			if (held & KEY_DUP)
				dy -= 4;
			if (cp.dy < -20 || cp.dy > 20)
				dy -= cp.dy / 12;
			if (down & KEY_R)
				dy += page;
			if (down & KEY_L)
				dy -= page;
			if (!(held & KEY_TOUCH))
				drag = -1;
			else if ((down & KEY_TOUCH) && tp.py >= LIST_TOP && tp.py < LIST_BOTTOM)
				drag = tp.py;
			else if (drag >= 0) {
				dy += drag - tp.py;
				drag = tp.py;
			}
			int to = scroll + dy, max = list_max_scroll();
			to = to < 0 ? 0 : to > max ? max : to;
			if (to != scroll) {
				scroll = to;
				redraw = true;
			}
		}
		if (redraw) {
			bottom_draw(&bottom, list, scroll, prompt, &nprompt);
			serial++;
		}
		int fade = t < FADE_IN ? (int)(t * 256 / FADE_IN) : 256;
		if (closing) {
			if (closing > FADE_OUT)
				break;
			int out = (int)((FADE_OUT - closing) * 256 / FADE_OUT);
			fade = fade < out ? fade : out;
			closing++;
		}
		show(&top, held_top, GFX_TOP, t, fade, 0);
		show(&bottom, held_bottom, GFX_BOTTOM, t, fade, serial);
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
		t++;
	}
	screen_free(&top), screen_free(&bottom), free(s_fog), free(s_missing), free(s_list), free(s_rows);
	return false;
}
