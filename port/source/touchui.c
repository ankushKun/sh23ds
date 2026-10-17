#include "touchui.h"
#include "combat.h"
#include "events.h"
#include "font.h"
#include "itemview.h"
#include "map.h"
#include "prog.h"
#include "prog_int.h"
#include "touchui_shbin.h"
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- state

#define MAX_ROWS 24
#define MAX_BUTTONS 5
#define MAX_VERTS 4096
#define SOLID_END 1536                // s_verts: solids, then pictures, then glyphs
#define IMAGE_END 2048
#define MAX_IMAGES 24
#define MAX_HITS 48

typedef struct {
	bool on;
	char title[48];
	char rows[MAX_ROWS][48];
	int n, shown_sel;
	int* sel;
	char blabel[MAX_BUTTONS][16];
	u32 bkey[MAX_BUTTONS];
	int nb;
} Menu;

enum { HIT_KEY, HIT_ROW, HIT_PASS, HIT_ZOOM };
typedef struct { float x0, y0, x1, y1; u8 kind, menu; s16 row; u32 key; } Hit;
typedef struct { float x, y, u, v; u8 c[4]; } Vert;

static Menu s_menus[TOUI_MENUS];
static char s_box[700];
static char s_box_label[MAX_BUTTONS][16];
static u32 s_box_key[MAX_BUTTONS];
static int s_box_nb;
static bool s_box_on;
static char s_panel[512];
static bool s_debug, s_movie, s_dirty = true;
static int s_cond = -1;              // 0 Fine, 1 Caution, 2 Danger (the help screen's thresholds)
static char s_weapon[40];

static Hit s_hits[MAX_HITS];
static int s_nhits;
static u64 s_select_tick;

static C3D_RenderTarget* s_target;
static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uProj;
static C3D_Mtx s_proj;
static Vert* s_verts;            // linear memory, reused for every redraw
static int s_nsolid, s_ntext;    // solids from the front of s_verts, glyphs from IMAGE_END
static int s_nimage;             // picture vertices from SOLID_END
static struct { C3D_Tex* tex; int first, count; } s_images[MAX_IMAGES];
static int s_nimages;
static float s_mini_zoom = 1.0f; // the small map: the screen's zoomed-out size times this
static u32 s_mini_key;
static bool s_map_on, s_map_zoomed;
static bool s_map_readable = true;
static char s_map_name[48];
static int s_item_ids[MAX_ROWS];  // the item menu's items (touchui_menu_items)
static int s_model_item;          // this redraw's item model and where (itemview.c)
static float s_model_rect[4], s_model_angle;
static u64 s_model_tick;
static PrintConsole s_console;
static u16* s_offscreen;         // where the console writes while it is not shown

// The PC's colours (font.c) and the screen's own darks.
static u32 s_white, s_grey_top, s_grey_bottom, s_yellow;
#define BG 0x0c0c0cff
#define FRAME 0x3a3a3aff
#define BAR 0x3c1010ff
#define BUTTON 0x1c1c1cff
#define BUTTON_EDGE 0x5a5a5aff

// ---------------------------------------------------------------- setup

void touchui_init(void)
{
	// The console keeps working for the debug page, into a buffer of its own (the bottom screen's
	// framebuffer layout, 240 x 320 RGB565).
	consoleInit(GFX_BOTTOM, &s_console);
	s_offscreen = calloc(240 * 320, 2);
	if (s_offscreen)
		s_console.frameBuffer = s_offscreen;
	s_target = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);   // depth for the item model
	C3D_RenderTargetSetOutput(s_target, GFX_BOTTOM, GFX_LEFT,
	                          GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	                          GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                          GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
	s_dvlb = DVLB_ParseFile((u32*)touchui_shbin, touchui_shbin_size);
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uProj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
	Mtx_OrthoTilt(&s_proj, 0.0f, 320.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
	s_verts = linearAlloc(MAX_VERTS * sizeof(Vert));
	u32 dummy;
	font_pc_color(0, &s_white, &dummy);
	font_pc_color(10, &s_grey_top, &s_grey_bottom);
	font_pc_color(4, &s_yellow, &dummy);
	s_dirty = true;
}

// ---------------------------------------------------------------- what is shown

static void copy_buttons(char label[][16], u32* key, int* nb, const TouiButton* b, int n)
{
	*nb = n > MAX_BUTTONS ? MAX_BUTTONS : n;
	for (int i = 0; i < *nb; i++) {
		snprintf(label[i], 16, "%s", b[i].label);
		key[i] = b[i].key;
	}
}

void touchui_menu(int id, const char* title, const char* const* rows, int n, int* sel, const TouiButton* buttons,
                  int nbuttons)
{
	if (id < 0 || id >= TOUI_MENUS)
		return;
	Menu m;
	memset(&m, 0, sizeof m);
	m.on = true;
	snprintf(m.title, sizeof m.title, "%s", title ? title : "");
	m.n = n > MAX_ROWS ? MAX_ROWS : n;
	for (int i = 0; i < m.n; i++)
		snprintf(m.rows[i], sizeof m.rows[i], "%s", rows[i] ? rows[i] : "");
	m.sel = sel;
	m.shown_sel = sel ? *sel : -1;
	copy_buttons(m.blabel, m.bkey, &m.nb, buttons, nbuttons);
	if (memcmp(&m, &s_menus[id], sizeof m)) {
		s_menus[id] = m;
		s_dirty = true;
	}
}

void touchui_menu_items(const int* items, int n)
{
	for (int i = 0; i < MAX_ROWS; i++)
		s_item_ids[i] = i < n ? items[i] : 0;
}

void touchui_menu_close(int id)
{
	if (id >= 0 && id < TOUI_MENUS && s_menus[id].on) {
		s_menus[id].on = false;
		s_dirty = true;
		if (id == TOUI_ITEMS) {
			itemview_free();             // the model's memory goes back with the item screen
			s_model_tick = 0;
		}
	}
}

void touchui_box(const char* text, const TouiButton* buttons, int nbuttons)
{
	bool on = text != NULL;
	char box[sizeof s_box];
	int n = 0;
	for (; text && text[n] && text[n] != '\f' && n < (int)sizeof box - 1; n++)
		box[n] = text[n];
	box[n] = 0;
	char label[MAX_BUTTONS][16];
	u32 key[MAX_BUTTONS];
	int nb;
	memset(label, 0, sizeof label);
	memset(key, 0, sizeof key);
	copy_buttons(label, key, &nb, buttons, nbuttons);
	if (on != s_box_on || strcmp(box, s_box) || nb != s_box_nb || memcmp(label, s_box_label, sizeof label) ||
	    memcmp(key, s_box_key, sizeof key)) {
		s_box_on = on;
		memcpy(s_box, box, sizeof box);
		memcpy(s_box_label, label, sizeof label);
		memcpy(s_box_key, key, sizeof key);
		s_box_nb = nb;
		s_dirty = true;
	}
}

void touchui_panel(const char* text)
{
	if (!text)
		text = "";
	if (strcmp(text, s_panel)) {
		snprintf(s_panel, sizeof s_panel, "%s", text);
		s_dirty = true;
	}
}

bool touchui_select(void)
{
	u64 now = svcGetSystemTick();
	bool twice = s_select_tick && now - s_select_tick < SYSCLOCK_ARM11 / 2;
	s_select_tick = twice ? 0 : now;
	return twice;
}

void touchui_debug(bool on)
{
	if (on == s_debug || !s_offscreen)
		return;
	s_debug = on;
	u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	if (on) {
		memcpy(fb, s_offscreen, 240 * 320 * 2);
		s_console.frameBuffer = fb;
	} else {
		memcpy(s_offscreen, fb, 240 * 320 * 2);
		s_console.frameBuffer = s_offscreen;
		s_dirty = true;
	}
}

void touchui_movie(bool on)
{
	if (on != s_movie) {
		s_movie = on;
		s_dirty = true;
	}
}

// ---------------------------------------------------------------- drawing

static void hit(float x0, float y0, float x1, float y1, int kind, int menu, int row, u32 key)
{
	if (s_nhits < MAX_HITS)
		s_hits[s_nhits++] = (Hit){ x0, y0, x1, y1, (u8)kind, (u8)menu, (s16)row, key };
}

static void put(Vert* v, float x, float y, u32 c)
{
	*v = (Vert){ x, y, 0.0f, 0.0f, { c >> 24, c >> 16 & 0xff, c >> 8 & 0xff, c & 0xff } };
}

static void rect(float x0, float y0, float x1, float y1, u32 top, u32 bottom)
{
	if (s_nsolid + 6 > SOLID_END)
		return;
	Vert* v = &s_verts[s_nsolid];
	put(&v[0], x0, y0, top), put(&v[1], x0, y1, bottom), put(&v[2], x1, y1, bottom);
	put(&v[3], x0, y0, top), put(&v[4], x1, y1, bottom), put(&v[5], x1, y0, top);
	s_nsolid += 6;
}

static void frame_rect(float x0, float y0, float x1, float y1, u32 fill, u32 edge)
{
	rect(x0, y0, x1, y1, edge, edge);
	rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, fill, fill);
}

// A line of thickness w between two points (the heart trace).
static void line(float x0, float y0, float x1, float y1, float w, u32 c)
{
	if (s_nsolid + 6 > SOLID_END)
		return;
	float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
	if (l < 0.01f)
		return;
	float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
	Vert* v = &s_verts[s_nsolid];
	put(&v[0], x0 + nx, y0 + ny, c), put(&v[1], x0 - nx, y0 - ny, c), put(&v[2], x1 - nx, y1 - ny, c);
	put(&v[3], x0 + nx, y0 + ny, c), put(&v[4], x1 - nx, y1 - ny, c), put(&v[5], x1 + nx, y1 + ny, c);
	s_nsolid += 6;
}

// Pictures (the small map, its marks; tex NULL for flat shapes such as the arrow), drawn after the
// solids and before the text. Fits map.c's MapDrawFn.
static void image(C3D_Tex* tex, const MapVert* mv, int n, u32 rgba)
{
	if (SOLID_END + s_nimage + n > IMAGE_END || s_nimages >= MAX_IMAGES)
		return;
	if (!s_nimages || s_images[s_nimages - 1].tex != tex || !tex) {
		s_images[s_nimages].tex = tex;
		s_images[s_nimages].first = SOLID_END + s_nimage;
		s_images[s_nimages].count = 0;
		s_nimages++;
	}
	for (int i = 0; i < n; i++) {
		Vert* v = &s_verts[SOLID_END + s_nimage++];
		*v = (Vert){ mv[i].x, mv[i].y, mv[i].u, mv[i].v, { rgba >> 24, rgba >> 16 & 0xff, rgba >> 8 & 0xff, rgba & 0xff } };
	}
	s_images[s_nimages - 1].count += n;
}

static void text(int font, float x, float y, const char* s, int align, u32 top, u32 bottom)
{
	FontVert fv[6 * 80];
	int n = font_layout_px(font, x, y, s, align, top, bottom, fv, (int)(sizeof fv / sizeof *fv));
	for (int i = 0; i < n && IMAGE_END + s_ntext < MAX_VERTS; i++) {
		Vert* v = &s_verts[IMAGE_END + s_ntext++];
		u32 c = fv[i].rgba;
		*v = (Vert){ fv[i].x, fv[i].y, fv[i].u, fv[i].v, { c >> 24, c >> 16 & 0xff, c >> 8 & 0xff, c & 0xff } };
	}
}

// Text with line breaks, one call per line (font_layout_px takes 80 glyphs at a time). A line
// with three spaces in it is a "label   value" pair: the value goes to the right edge.
static float text_block(int font, float x, float y, const char* s, int align, u32 top, u32 bottom)
{
	char line[96];
	float lh = font_line_px(font);
	while (s && *s) {
		const char* e = strchr(s, '\n');
		int n = e ? (int)(e - s) : (int)strlen(s);
		snprintf(line, sizeof line, "%.*s", n < 95 ? n : 95, s);
		char* lead = line;
		while (*lead == ' ')
			lead++;
		char* gap = align == FONT_LEFT ? strstr(lead, "   ") : NULL;
		if (gap) {
			*gap = 0;
			for (gap += 3; *gap == ' '; gap++)
				;
			text(font, 320.0f - x, y, gap, FONT_RIGHT, top, bottom);
		}
		text(font, x, y, line, align, top, bottom);
		y += lh;
		s = e ? e + 1 : s + n;
	}
	return y;
}

static void button(float x0, float y0, float x1, float y1, const char* label, u32 key)
{
	frame_rect(x0, y0, x1, y1, BUTTON, BUTTON_EDGE);
	text(FONT_SMALL, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f - font_line_px(FONT_SMALL) * 0.5f, label, FONT_CENTER, s_white,
	     s_white);
	hit(x0, y0, x1, y1, HIT_KEY, 0, 0, key);
}

static void button_row(char label[][16], const u32* key, int nb, float y)
{
	if (!nb)
		return;
	float w = (304.0f - 6.0f * (nb - 1)) / nb;
	for (int i = 0; i < nb; i++)
		button(8 + i * (w + 6), y, 8 + i * (w + 6) + w, y + 30, label[i], key[i]);
}

static void screen_frame(const char* title)
{
	rect(0, 0, 320, 240, BG, BG);
	frame_rect(3, 3, 317, 237, BG, FRAME);
	if (title && *title) {
		text(FONT_BIG, 160, 8, title, FONT_CENTER, s_white, s_white);
		rect(16, 26, 304, 27, FRAME, FRAME);
	}
}

static void draw_menu(int id)
{
	Menu* m = &s_menus[id];
	screen_frame(m->title);
	int sel = m->sel ? *m->sel : -1;
	float right = 310;
	if (id == TOUI_ITEMS) {
		int item = sel >= 0 && sel < m->n ? s_item_ids[sel] : 0;
		if (item > 0 && itemview_has(item)) {
			right = 176;
			frame_rect(182, 32, 312, 170, 0x000000ff, FRAME);
			if (!s_model_tick)
				s_model_tick = svcGetSystemTick();
			s_model_item = item;
			s_model_rect[0] = 183, s_model_rect[1] = 33, s_model_rect[2] = 311, s_model_rect[3] = 169;
			s_model_angle = (float)(svcGetSystemTick() - s_model_tick) / (float)SYSCLOCK_ARM11 * 0.9f;
		} else {
			s_model_tick = 0;
		}
	}
	float lh = font_line_px(FONT_BIG) + 2.0f;
	int fit = (int)((m->nb ? 160.0f : 196.0f) / lh);
	int first = sel - fit / 2;
	first = first > m->n - fit ? m->n - fit : first;
	first = first < 0 ? 0 : first;
	for (int i = first; i < m->n && i < first + fit; i++) {
		float y = 34 + (i - first) * lh;
		bool on = i == sel;
		if (on)
			rect(10, y - 1, right, y + lh - 2, BAR, BAR);
		u32 ct = on ? s_yellow : s_grey_top, cb = on ? s_yellow : s_grey_bottom;
		// "Label    value" rows (the console's columns): the value goes to the right edge
		const char* gap = strstr(m->rows[i], "  ");
		if (gap) {
			char label[48];
			snprintf(label, sizeof label, "%.*s", (int)(gap - m->rows[i]), m->rows[i]);
			while (*gap == ' ')
				gap++;
			text(FONT_BIG, 18, y, label, FONT_LEFT, ct, cb);
			text(FONT_BIG, right - 8, y, gap, FONT_RIGHT, ct, cb);
		} else {
			text(FONT_BIG, 18, y, m->rows[i], FONT_LEFT, ct, cb);
		}
		hit(10, y - 1, right, y + lh - 2, HIT_ROW, id, i, strchr(m->rows[i], '<') ? 1 : 0);
	}
	button_row(m->blabel, m->bkey, m->nb, 202);
}

static void draw_box(void)
{
	screen_frame(NULL);
	int lines = 1;
	for (const char* c = s_box; *c; c++)
		lines += *c == '\n';
	text_block(lines > 9 ? FONT_SMALL : FONT_BIG, 18, lines > 9 ? 10 : 24, s_box, FONT_LEFT, s_white, s_white);
	if (s_box_nb)
		button_row(s_box_label, s_box_key, s_box_nb, 202);
	else
		hit(0, 0, 320, 240, HIT_KEY, 0, 0, KEY_A);
}

static void draw_panel(void)
{
	screen_frame(NULL);
	text_block(FONT_SMALL, 14, 10, s_panel, FONT_LEFT, s_white, s_white);
	static char labels[6][16] = { "<", "^", "v", ">", "A", "B" };
	static const u32 keys[6] = { KEY_DLEFT, KEY_DUP, KEY_DDOWN, KEY_DRIGHT, KEY_A, KEY_B };
	button_row(labels, keys, 6, 202);
}

static MapPlayer player(void)
{
	MapPlayer p;
	float pos[3], yaw;
	p_player(pos, &yaw);
	p.x = pos[0], p.y = pos[1], p.z = pos[2], p.yaw = yaw;
	p.glb_crd = ev_glb_crd();
	p.chapter = prog_chapter();
	return p;
}

// In play: the map around James (a tap opens the map screen, the corner button zooms), his
// condition the way the PC's item screen shows it (a heartbeat line in the condition's colour:
// Fine green, Caution yellow, Danger red, with its word), the weapon and its rounds.
static void draw_play(void)
{
	screen_frame(NULL);
	frame_rect(8, 8, 190, 172, 0x000000ff, FRAME);
	MapPlayer pl = player();
	if (!s_map_readable)
		text(FONT_SMALL, 99, 84, "Too dark to read", FONT_CENTER, s_grey_top, s_grey_bottom);
	else if (!map_mini(&pl, image, 9, 9, 189, 171, s_mini_zoom))
		text(FONT_SMALL, 99, 84, "No map", FONT_CENTER, s_grey_top, s_grey_bottom);
	hit(8, 8, 190, 172, HIT_PASS, 0, 0, 0);
	frame_rect(8, 176, 190, 196, BUTTON, BUTTON_EDGE);
	text(FONT_SMALL, 99, 186 - font_line_px(FONT_SMALL) * 0.5f, s_mini_zoom > 1.5f ? "Zoom out" : "Zoom in", FONT_CENTER,
	     s_white, s_white);
	hit(8, 176, 190, 196, HIT_ZOOM, 0, 0, 0);

	static const char* const word[3] = { "Fine", "Caution", "Danger" };
	static const u32 col[3] = { 0x40d040ff, 0xe0c030ff, 0xe03020ff };
	int c = s_cond < 0 ? 0 : s_cond;
	text(FONT_SMALL, 198, 10, "Condition", FONT_LEFT, s_grey_top, s_grey_bottom);
	frame_rect(196, 26, 312, 82, 0x000000ff, FRAME);
	static const float beat[] = { 0, 0, 0.1f, -0.15f, 0.1f, 0, 0, 0.05f, -0.9f, 0.7f, -0.1f, 0, 0.12f, 0.18f, 0.08f, 0, 0 };
	const int nb = (int)(sizeof beat / sizeof *beat);
	float amp = c == 0 ? 20.0f : c == 1 ? 14.0f : 9.0f;
	for (int i = 0; i + 1 < nb; i++) {
		float x0 = 200 + i * 6.7f, x1 = x0 + 6.7f;
		line(x0, 52 + beat[i] * amp, x1, 52 + beat[i + 1] * amp, 1.6f, col[c]);
	}
	text(FONT_BIG, 254, 86, word[c], FONT_CENTER, col[c], col[c]);
	text(FONT_SMALL, 198, 116, "Weapon", FONT_LEFT, s_grey_top, s_grey_bottom);
	// "name  loaded / spare": the name, then the rounds below it
	char name[40], rounds[24] = "";
	snprintf(name, sizeof name, "%s", s_weapon);
	char* gap = strstr(name, "  ");
	if (gap) {
		*gap = 0;
		for (gap += 2; *gap == ' '; gap++)
			;
		snprintf(rounds, sizeof rounds, "%s", gap);
	}
	text(FONT_BIG, 254, 132, name, FONT_CENTER, s_white, s_white);
	if (rounds[0])
		text(FONT_BIG, 254, 152, rounds, FONT_CENTER, s_white, s_white);
	button(8, 202, 104, 232, "Items", KEY_Y);
	button(112, 202, 208, 232, "Light", KEY_X);
	button(216, 202, 312, 232, "Pause", KEY_START);
}

// The map screen (on the top screen): its name, the other floors and maps, zoom and close; while
// zoomed in, the arrows scroll for as long as they are held.
static void draw_map(void)
{
	screen_frame(s_map_name);
	static char la[6][16] = { "Up", "Down", "<", ">", "Zoom", "Close" };
	if (s_map_zoomed) {
		snprintf(la[0], 16, "^"), snprintf(la[1], 16, "v"), snprintf(la[4], 16, "Out");
	} else {
		snprintf(la[0], 16, "Floor up"), snprintf(la[1], 16, "Floor down"), snprintf(la[4], 16, "Zoom");
	}
	button(110, 40, 210, 80, la[0], KEY_DUP);
	button(110, 120, 210, 160, la[1], KEY_DDOWN);
	button(20, 80, 100, 120, la[2], KEY_DLEFT);
	button(220, 80, 300, 120, la[3], KEY_DRIGHT);
	button(8, 202, 156, 232, la[4], KEY_A);
	button(164, 202, 312, 232, la[5], KEY_B);
	if (!s_map_zoomed)
		text(FONT_SMALL, 160, 172, "< > other maps", FONT_CENTER, s_grey_top, s_grey_bottom);
}

void touchui_map_readable(bool yes)
{
	if (yes != s_map_readable) {
		s_map_readable = yes;
		s_dirty = true;
	}
}

void touchui_map(bool open)
{
	const char* name = open ? map_name() : "";
	bool z = open && map_zoomed();
	if (open != s_map_on || z != s_map_zoomed || strcmp(name, s_map_name)) {
		s_map_on = open, s_map_zoomed = z;
		snprintf(s_map_name, sizeof s_map_name, "%s", name);
		s_dirty = true;
	}
}

static void draw_movie(void)
{
	screen_frame(NULL);
	button(100, 104, 220, 136, "Skip", KEY_START);
}


static void status_check(void)
{
	float hp = combat_hp(), max = combat_hp_max();
	int cond = hp > 0.7f * max ? 0 : hp > 0.35f * max ? 1 : 2;
	const char* w = combat_weapon_line();
	char weapon[40];
	snprintf(weapon, sizeof weapon, "%s", w ? w : "");
	for (char* e = weapon + strlen(weapon); e > weapon && e[-1] == ' '; )
		*--e = 0;
	MapPlayer pl = player();
	u32 key = s_map_readable ? map_mini_key(&pl, s_mini_zoom) : 1;
	if (key != s_mini_key) {
		s_mini_key = key;
		s_dirty = true;
	}
	if (cond != s_cond || strcmp(weapon, s_weapon)) {
		s_cond = cond;
		memcpy(s_weapon, weapon, sizeof s_weapon);
		s_dirty = true;
	}
}

// What is up, most important first.
static int top_menu(void)
{
	static const int order[] = { TOUI_PAUSE, TOUI_ITEMS, TOUI_MEMOS, TOUI_LEVELS, TOUI_TITLE };
	for (u32 i = 0; i < sizeof order / sizeof *order; i++)
		if (s_menus[order[i]].on)
			return order[i];
	return -1;
}

static void bind_ui(void)
{
	C3D_BindProgram(&s_prog);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProj, &s_proj);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
	               GPU_ONE_MINUS_SRC_ALPHA);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_verts, sizeof(Vert), 3, 0x210);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
}

void touchui_frame(void)
{
	if (!s_target || !s_verts || !font_ready())
		return;
	if (top_menu() < 0 && !s_box_on && !s_panel[0] && !s_movie && !s_map_on)
		status_check();
	static u32 frame;
	if (s_model_tick && top_menu() == TOUI_ITEMS && (++frame & 1))
		s_dirty = true;                  // the item model turns: about 30 redraws a second
	if (!s_dirty || s_debug)
		return;
	s_dirty = false;
	s_nsolid = s_ntext = s_nhits = s_nimage = s_nimages = 0;
	s_model_item = 0;
	int m = top_menu();
	if (s_movie)
		draw_movie();
	else if (m >= 0)
		draw_menu(m);
	else if (s_box_on)
		draw_box();
	else if (s_map_on)
		draw_map();
	else if (s_panel[0])
		draw_panel();
	else
		draw_play();

	// the GPU reads the vertices from memory: write them out of the CPU's data cache first
	GSPGPU_FlushDataCache(s_verts, MAX_VERTS * sizeof(Vert));
	C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, BG, 0);
	C3D_FrameDrawOn(s_target);
	bind_ui();
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	if (s_nsolid) {
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		C3D_DrawArrays(GPU_TRIANGLES, 0, s_nsolid);
	}
	if (s_model_item > 0) {
		itemview_draw(s_model_item, s_model_rect[0], s_model_rect[1], s_model_rect[2], s_model_rect[3], s_model_angle);
		bind_ui();
	}
	for (int i = 0; i < s_nimages; i++) {
		env = C3D_GetTexEnv(0);          // getting it again marks it changed after the last draw
		C3D_TexEnvInit(env);
		if (s_images[i].tex) {
			C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
			C3D_TexBind(0, s_images[i].tex);
		} else {
			C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		}
		C3D_DrawArrays(GPU_TRIANGLES, s_images[i].first, s_images[i].count);
	}
	if (s_ntext) {
		env = C3D_GetTexEnv(0);          // getting it again marks it changed after the first draw
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		C3D_TexBind(0, font_texture());
		C3D_DrawArrays(GPU_TRIANGLES, IMAGE_END, s_ntext);
	}
}

// ---------------------------------------------------------------- input

void touchui_input(u32* down, u32* held)
{
	if (s_debug)
		return;
	touchPosition t;
	if (!(*down & KEY_TOUCH)) {
		// a held arrow keeps scrolling the zoomed map (the screen reads held keys)
		if (s_map_on && (*held & KEY_TOUCH)) {
			hidTouchRead(&t);
			for (int i = 0; i < s_nhits; i++)
				if (s_hits[i].kind == HIT_KEY && t.px >= s_hits[i].x0 && t.px < s_hits[i].x1 && t.py >= s_hits[i].y0 &&
				    t.py < s_hits[i].y1 && (s_hits[i].key & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)))
					*held |= s_hits[i].key;
			*held &= ~KEY_TOUCH;
		}
		return;
	}
	hidTouchRead(&t);
	*down &= ~KEY_TOUCH;
	for (int i = s_nhits - 1; i >= 0; i--) {
		const Hit* h = &s_hits[i];
		if (t.px < h->x0 || t.px >= h->x1 || t.py < h->y0 || t.py >= h->y1)
			continue;
		if (h->kind == HIT_ZOOM) {
			s_mini_zoom = s_mini_zoom > 1.5f ? 1.0f : 2.0f;
			s_dirty = true;
		} else if (h->kind == HIT_PASS) {
			*down |= KEY_TOUCH;
		} else if (h->kind == HIT_KEY) {
			*down |= h->key;
		} else {
			Menu* m = &s_menus[h->menu];
			if (!m->sel)
				return;
			float w = h->x1 - h->x0;
			if (h->key && t.px < h->x0 + w * 0.3f && *m->sel == h->row)
				*down |= KEY_DLEFT;
			else if (h->key && t.px > h->x1 - w * 0.3f && *m->sel == h->row)
				*down |= KEY_DRIGHT;
			else if (*m->sel == h->row)
				*down |= KEY_A;          // a second tap on the chosen row takes it
			*m->sel = h->row;
			s_dirty = true;
		}
		return;
	}
}
