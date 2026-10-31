#include "touchui.h"
#include "audio.h"
#include "bgm.h"
#include "combat.h"
#include "controls.h"
#include "events.h"
#include "font.h"
#include "itemview.h"
#include "map.h"
#include "prog.h"
#include "prog_int.h"
#include "profile.h"
#include "touchui_shbin.h"
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- state

#define MAX_ROWS 64
#define MAX_BUTTONS 6
#define MAX_VERTS 8192
#define SOLID_END 3584                // s_verts: solids, then pictures, then glyphs
#define IMAGE_END 5120
#define MAX_IMAGES 40
#define MAX_HITS 96

typedef struct {
	bool on;
	char title[48];
	char rows[MAX_ROWS][72];     // the other languages' rows run longer than the English ones
	int n, shown_sel;
	int* sel;
	char blabel[MAX_BUTTONS][16];
	u32 bkey[MAX_BUTTONS];
	int nb;
	u64 dim;
	int flags;
	char tabs[TOUI_MAX_TABS][16];    // the Options' tabs (none: the title)
	int ntabs, shown_tab;
	int* tab;
	TouiRow kinds[MAX_ROWS];
} Menu;

enum { HIT_KEY, HIT_ROW, HIT_PASS, HIT_ZOOM, HIT_QUICK, HIT_DRAG, HIT_TAB, HIT_CHOICE };
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
static int s_heal[3];                // the healing items' counts (the quick slots)
static char s_sound[64];             // a sound problem the player should know of (audio_problem)
static bool s_have_light;
static bool s_act_attack;          // Action attacks now (touchui_action_attacks)
static int s_quick, s_quick_item;    // a quick slot tapped, for touchui_quick
typedef struct {                     // the first run's two choices (touchui_choices)
	bool on;
	char name[2][40], line[2][160];
	int* sel;
	int shown;
	char blabel[MAX_BUTTONS][16];
	u32 bkey[MAX_BUTTONS];
	int nb;
	u32 dim;
} Choices;
static Choices s_ch;

static Hit s_hits[MAX_HITS];
static int s_nhits;
static u64 s_select_tick;
static float s_press[4];             // the prompt last tapped lights for a moment
static u64 s_press_tick;
static bool s_drag;                  // a finger moving the zoomed map, and where it was
static int s_drag_x, s_drag_y;
static bool s_free;                  // a finger that came down on nothing: it turns the behind camera
static int s_free_x, s_free_y, s_free_dx, s_free_dy;

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
static C3D_Tex s_grain[4];       // film grain, four noise frames swapped in place
static bool s_grain_ok;
static int s_grain_frame;
#define MINI_NEAR 1.8f               // the small map: the screen's zoomed-out size times this
#define MINI_CLOSE 3.4f              // and with the zoom ring on
static float s_mini_zoom = MINI_NEAR;
static u32 s_mini_key;
static bool s_map_on, s_map_zoomed;
static bool s_map_readable = true;
static char s_map_name[48];
static PrintConsole s_console;
static u16* s_offscreen;         // where the console writes while it is not shown

// The PC's colours (font.c) and the screen's own (port/UI.md, "The look"), 0xRRGGBBAA.
static u32 s_white, s_grey_top, s_grey_bottom, s_yellow;
#define BG 0x0b0a0aff
#define PANEL 0x000000ff
#define RULE 0x4a4440ff
#define LABEL 0x8a847cff
#define DIM 0x55524eff
#define RING 0xb4aea6ff
#define RED 0x741010ff
#define PAD 0x161313ff
#define FOOT_Y 206.0f                 // the footer's rule; its prompts below it
#define CLEAR(c) ((c) & 0xffffff00u)

// ---------------------------------------------------------------- setup

static void grain_init(void)
{
	u32 seed = 0x2c9277b5u;
	s_grain_ok = true;
	for (int f = 0; f < 4; f++) {
		if (!C3D_TexInit(&s_grain[f], 64, 64, GPU_RGBA8)) {
			s_grain_ok = false;
			return;
		}
		// noise needs no swizzling: any order of random texels is the same noise
		u32* px = (u32*)s_grain[f].data;
		for (int i = 0; i < 64 * 64; i++) {
			seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
			u32 g = seed >> 24;
			px[i] = g << 24 | g << 16 | g << 8 | 0x18;
		}
		C3D_TexFlush(&s_grain[f]);
		C3D_TexSetFilter(&s_grain[f], GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&s_grain[f], GPU_REPEAT, GPU_REPEAT);
	}
}

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
	grain_init();
	u32 dummy;
	font_pc_color(0, &s_white, &dummy);
	font_pc_color(10, &s_grey_top, &s_grey_bottom);
	font_pc_color(4, &s_yellow, &dummy);
	s_dirty = true;
}

// ---------------------------------------------------------------- what is shown

static void copy_buttons(char label[][16], u32* key, int* nb, const TouiButton* b, int n)
{
	*nb = n > MAX_BUTTONS ? MAX_BUTTONS : n < -1 ? -1 : n;
	for (int i = 0; i < *nb; i++) {
		snprintf(label[i], 16, "%s", b[i].label);
		key[i] = b[i].key;
	}
}

void touchui_menu_ex(int id, const char* title, const char* const* rows, int n, int* sel, const TouiButton* buttons,
                     int nbuttons, u64 dim, int flags)
{
	touchui_menu_full(id, title, rows, n, sel, buttons, nbuttons, dim, flags, NULL);
}

void touchui_menu_full(int id, const char* title, const char* const* rows, int n, int* sel,
                       const TouiButton* buttons, int nbuttons, u64 dim, int flags, const TouiMenuExtra* extra)
{
	if (id < 0 || id >= TOUI_MENUS)
		return;
	static Menu m;                   // big: off the stack
	memset(&m, 0, sizeof m);
	m.on = true;
	snprintf(m.title, sizeof m.title, "%s", title ? title : "");
	m.n = n > MAX_ROWS ? MAX_ROWS : n;
	for (int i = 0; i < m.n; i++)
		snprintf(m.rows[i], sizeof m.rows[i], "%s", rows[i] ? rows[i] : "");
	m.sel = sel;
	m.shown_sel = sel ? *sel : -1;
	copy_buttons(m.blabel, m.bkey, &m.nb, buttons, nbuttons);
	m.dim = dim;
	m.flags = flags;
	if (extra) {
		m.ntabs = extra->ntabs > TOUI_MAX_TABS ? TOUI_MAX_TABS : extra->ntabs;
		for (int i = 0; i < m.ntabs; i++)
			snprintf(m.tabs[i], sizeof m.tabs[i], "%s", extra->tabs[i]);
		m.tab = extra->tab;
		m.shown_tab = extra->tab ? *extra->tab : 0;
		for (int i = 0; extra->rows && i < m.n; i++)
			m.kinds[i] = extra->rows[i];
	}
	if (memcmp(&m, &s_menus[id], sizeof m)) {
		s_menus[id] = m;
		s_dirty = true;
	}
}

void touchui_menu(int id, const char* title, const char* const* rows, int n, int* sel, const TouiButton* buttons,
                  int nbuttons)
{
	touchui_menu_ex(id, title, rows, n, sel, buttons, nbuttons, 0, 0);
}

void touchui_menu_close(int id)
{
	if (id >= 0 && id < TOUI_MENUS && s_menus[id].on) {
		s_menus[id].on = false;
		s_dirty = true;
		if (id == TOUI_ITEMS)
			itemview_free();             // the model's memory goes back with the item screen
	}
}

void touchui_choices(const char* const* names, const char* const* lines, int* sel, const TouiButton* buttons,
                     int nbuttons, u32 dim)
{
	Choices c;
	memset(&c, 0, sizeof c);
	c.on = true;
	for (int i = 0; i < 2; i++) {
		snprintf(c.name[i], sizeof c.name[i], "%s", names[i] ? names[i] : "");
		snprintf(c.line[i], sizeof c.line[i], "%s", lines[i] ? lines[i] : "");
	}
	c.sel = sel;
	c.shown = sel ? *sel : 0;
	copy_buttons(c.blabel, c.bkey, &c.nb, buttons, nbuttons);
	c.dim = dim;
	if (memcmp(&c, &s_ch, sizeof c)) {
		s_ch = c;
		s_dirty = true;
	}
}

void touchui_choices_close(void)
{
	if (s_ch.on) {
		s_ch.on = false;
		s_dirty = true;
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

int touchui_quick(int* item)
{
	int q = s_quick;
	*item = s_quick_item;
	s_quick = TQ_NONE;
	return q;
}

// ---------------------------------------------------------------- drawing: shapes

static void hit(float x0, float y0, float x1, float y1, int kind, int menu, int row, u32 key)
{
	if (s_nhits < MAX_HITS)
		s_hits[s_nhits++] = (Hit){ x0, y0, x1, y1, (u8)kind, (u8)menu, (s16)row, key };
}

static void put(Vert* v, float x, float y, u32 c)
{
	*v = (Vert){ x, y, 0.0f, 0.0f, { c >> 24, c >> 16 & 0xff, c >> 8 & 0xff, c & 0xff } };
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c)
{
	if (s_nsolid + 3 > SOLID_END)
		return;
	Vert* v = &s_verts[s_nsolid];
	put(&v[0], x0, y0, c), put(&v[1], x1, y1, c), put(&v[2], x2, y2, c);
	s_nsolid += 3;
}

// A quad with a colour at each corner (top left, top right, bottom left, bottom right).
static void quad4(float x0, float y0, float x1, float y1, u32 tl, u32 tr, u32 bl, u32 br)
{
	if (s_nsolid + 6 > SOLID_END)
		return;
	Vert* v = &s_verts[s_nsolid];
	put(&v[0], x0, y0, tl), put(&v[1], x0, y1, bl), put(&v[2], x1, y1, br);
	put(&v[3], x0, y0, tl), put(&v[4], x1, y1, br), put(&v[5], x1, y0, tr);
	s_nsolid += 6;
}

static void rect(float x0, float y0, float x1, float y1, u32 top, u32 bottom)
{
	quad4(x0, y0, x1, y1, top, top, bottom, bottom);
}

static void outline(float x0, float y0, float x1, float y1, u32 c)
{
	rect(x0, y0, x1, y0 + 1, c, c);
	rect(x0, y1 - 1, x1, y1, c, c);
	rect(x0, y0 + 1, x0 + 1, y1 - 1, c, c);
	rect(x1 - 1, y0 + 1, x1, y1 - 1, c, c);
}

// A one-pixel rule that fades out at both ends.
static void rule(float x0, float x1, float y, u32 c)
{
	float m = (x0 + x1) * 0.5f;
	quad4(x0, y, m, y + 1, CLEAR(c), c, CLEAR(c), c);
	quad4(m, y, x1, y + 1, c, CLEAR(c), c, CLEAR(c));
}

// The chosen row's band: dark red, fading out at both ends.
static void band(float x0, float y0, float x1, float y1)
{
	float f = (x1 - x0) * 0.18f;
	quad4(x0, y0, x0 + f, y1, CLEAR(RED), RED, CLEAR(RED), RED);
	rect(x0 + f, y0, x1 - f, y1, RED, RED);
	quad4(x1 - f, y0, x1, y1, RED, CLEAR(RED), RED, CLEAR(RED));
}

static void circle(float cx, float cy, float r, u32 c)
{
	const int n = 20;
	for (int i = 0; i < n; i++) {
		float a0 = i * 6.2831853f / n, a1 = (i + 1) * 6.2831853f / n;
		tri(cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, c);
	}
}

// A line of thickness w between two points (the heart trace).
static void line(float x0, float y0, float x1, float y1, float w, u32 c)
{
	float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
	if (l < 0.01f)
		return;
	float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
	tri(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny, c);
	tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, c);
}

// A small arrow head pointing left, right, up or down (dx, dy), centred on (x, y).
static void arrow(float x, float y, int dx, int dy, float s, u32 c)
{
	if (dx)
		tri(x + dx * s, y, x - dx * s * 0.6f, y - s, x - dx * s * 0.6f, y + s, c);
	else
		tri(x, y + dy * s, x - s, y - dy * s * 0.6f, x + s, y - dy * s * 0.6f, c);
}

// Pictures (the small map, its marks, the grain; tex NULL for flat shapes such as the arrow), drawn
// after the solids and before the text. Fits map.c's MapDrawFn.
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

// Flat shapes in the pictures' layer (over the small map): a disc and a bar.
static void disc(float cx, float cy, float r, u32 rgba)
{
	MapVert v[20 * 3];
	for (int i = 0; i < 20; i++) {
		float a0 = i * 6.2831853f / 20, a1 = (i + 1) * 6.2831853f / 20;
		v[i * 3] = (MapVert){ cx, cy, 0, 0 };
		v[i * 3 + 1] = (MapVert){ cx + cosf(a0) * r, cy + sinf(a0) * r, 0, 0 };
		v[i * 3 + 2] = (MapVert){ cx + cosf(a1) * r, cy + sinf(a1) * r, 0, 0 };
	}
	image(NULL, v, 60, rgba);
}

static void bar_img(float x0, float y0, float x1, float y1, u32 rgba)
{
	MapVert v[6] = { { x0, y0, 0, 0 }, { x1, y0, 0, 0 }, { x1, y1, 0, 0 },
	                 { x0, y0, 0, 0 }, { x1, y1, 0, 0 }, { x0, y1, 0, 0 } };
	image(NULL, v, 6, rgba);
}

// ---------------------------------------------------------------- drawing: text

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

// White text a pixel off a dark shadow, as the chosen rows and titles are drawn.
static void text_lit(int font, float x, float y, const char* s, int align, u32 c)
{
	text(font, x + 1, y + 1, s, align, 0x000000c0, 0x000000c0);
	text(font, x, y, s, align, c, c);
}

// One character of s into one (a full-width code is FONT_FULL_WIDTH and four hex digits), the
// next one returned.
static const char* one_char(const char* s, char one[6])
{
	int n = *s == FONT_FULL_WIDTH ? 5 : 1;
	int k = 0;
	for (; k < n && s[k]; k++)
		one[k] = s[k];
	one[k] = 0;
	return s + k;
}

// A title, spread a little and centred on x.
static float spaced_width(int font, const char* s, float gap)
{
	float w = 0.0f;
	char one[6];
	for (const char* c = s; *c;) {
		c = one_char(c, one);
		w += font_width_px(font, one) + (*c ? gap : 0.0f);
	}
	return w;
}

static void text_spaced(int font, float cx, float y, const char* s, float gap, u32 c)
{
	float x = cx - spaced_width(font, s, gap) * 0.5f;
	char one[6];
	for (const char* p = s; *p;) {
		p = one_char(p, one);
		text_lit(font, x, y, one, FONT_LEFT, c);
		x += font_width_px(font, one) + gap;
	}
}

// A small grey capitals label ("CONDITION").
static void label(float x, float y, const char* s, int align)
{
	char up[32];
	int i = 0;
	for (; s[i] && i < 31; i++)
		up[i] = s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i];
	up[i] = 0;
	text(FONT_SMALL, x, y, up, align, LABEL, LABEL);
}

// ---------------------------------------------------------------- drawing: the kit

// The button's name on its glyph: NULL for the D-Pad and the C-Stick's directions (an arrow).
static const char* key_name(u32 key)
{
	static const struct { u32 key; const char* name; } names[] = {
		{ KEY_A, "A" }, { KEY_B, "B" }, { KEY_X, "X" }, { KEY_Y, "Y" }, { KEY_L, "L" }, { KEY_R, "R" },
		{ KEY_ZL, "ZL" }, { KEY_ZR, "ZR" }, { KEY_START, "START" }, { KEY_SELECT, "SELECT" },
		{ KEY_CSTICK_UP, "C" }, { KEY_CSTICK_DOWN, "C" }, { KEY_CSTICK_LEFT, "C" }, { KEY_CSTICK_RIGHT, "C" },
	};
	for (u32 i = 0; i < sizeof names / sizeof *names; i++)
		if (key & names[i].key)
			return names[i].name;
	return NULL;
}

static bool key_dpad(u32 key) { return key & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT); }

static float glyph_width(u32 key)
{
	if (!key)
		return 0.0f;
	const char* n = key_name(key);
	if (!n)
		return 12.0f;
	return n[1] ? font_width_px(FONT_SMALL, n) + 11.0f : 15.0f;
}

// A letter (or a short name) placed on whole pixels with its ink centred as near (cx, cy) as the
// pixel grid allows; out: the ink's real centre, which the ring or tag around it is drawn on.
static void ink_text(int font, float cx, float cy, const char* s, u32 c, float* ox, float* oy)
{
	float x0, y0, x1, y1;
	*ox = cx, *oy = cy;
	if (!font_ink_px(font, s, &x0, &y0, &x1, &y1))
		return;
	float x = floorf(cx - (x0 + x1) * 0.5f + 0.5f), y = floorf(cy - (y0 + y1) * 0.5f + 0.5f);
	text(font, x, y, s, FONT_LEFT, c, c);
	*ox = x + (x0 + x1) * 0.5f, *oy = y + (y0 + y1) * 0.5f;
}

// The button's glyph from x, centred on cy: a ring with its letter, a rounded tag for the longer
// names, an arrow for the D-Pad. The ring goes round the letter's ink, not its cell.
static void glyph(float x, float cy, u32 key, u32 c)
{
	const char* n = key_name(key);
	if (!n) {
		if (key_dpad(key)) {
			int dx = key & KEY_DLEFT ? -1 : key & KEY_DRIGHT ? 1 : 0, dy = key & KEY_DUP ? -1 : key & KEY_DDOWN ? 1 : 0;
			arrow(x + 6, cy, dx, dy, 4.5f, c);
		}
		return;
	}
	float w = glyph_width(key), lx, ly;
	ink_text(FONT_SMALL, x + w * 0.5f, cy, n, c, &lx, &ly);
	if (!n[1]) {
		circle(lx, ly, 7.0f, c);
		circle(lx, ly, 5.8f, BG);
		return;
	}
	float l = lx - w * 0.5f, r = lx + w * 0.5f;
	circle(l + 7, ly, 7.0f, c), circle(r - 7, ly, 7.0f, c);
	rect(l + 7, ly - 7, r - 7, ly + 7, c, c);
	circle(l + 7, ly, 5.8f, BG), circle(r - 7, ly, 5.8f, BG);
	rect(l + 7, ly - 5.8f, r - 7, ly + 5.8f, BG, BG);
}

// The prompts under the footer's rule, each in an equal share of the width; a dim prompt can't be
// taken (no hit).
static void footer(char label_[][16], const u32* key, int n, u32 dim)
{
	if (!n)
		return;
	rule(12, 308, FOOT_Y, RULE);
	float slot = 304.0f / n, cy = 223.0f;
	for (int i = 0; i < n; i++) {
		bool off = dim >> i & 1;
		u32 c = off ? DIM : RING;
		float gw = glyph_width(key[i]), lw = font_width_px(FONT_SMALL, label_[i]);
		float w = gw + (gw > 0 && lw > 0 ? 5.0f : 0.0f) + lw;
		float x = 8 + slot * i + (slot - w) * 0.5f;
		glyph(x, cy, key[i], c);
		text(FONT_SMALL, x + w - lw, cy - font_line_px(FONT_SMALL) * 0.5f, label_[i], FONT_LEFT, off ? DIM : s_grey_top,
		     off ? DIM : s_grey_bottom);
		if (!off)
			hit(8 + slot * i, FOOT_Y + 1, 8 + slot * (i + 1), 240, HIT_KEY, 0, 0, key[i]);
	}
}

// The background: black, a dark edge all round.
static void backdrop(void)
{
	rect(0, 0, 320, 240, BG, BG);
	const u32 edge = 0x000000a0;
	rect(0, 0, 320, 26, edge, CLEAR(edge));
	rect(0, 214, 320, 240, CLEAR(edge), edge);
	quad4(0, 0, 30, 240, edge, CLEAR(edge), edge, CLEAR(edge));
	quad4(290, 0, 320, 240, CLEAR(edge), edge, CLEAR(edge), edge);
}

// A centred title between two rules that fade out towards the edges.
static void header(const char* title)
{
	if (!title || !*title)
		return;
	float w = spaced_width(FONT_BIG, title, 1.0f), y = 7.0f, mid = y + font_line_px(FONT_BIG) * 0.5f;
	text_spaced(FONT_BIG, 160, y, title, 1.0f, s_white);
	float l = 160 - w * 0.5f - 10, r = 160 + w * 0.5f + 10;
	if (l > 30) {
		quad4(14, mid, l, mid + 1, CLEAR(RULE), RULE, CLEAR(RULE), RULE);
		quad4(r, mid, 306, mid + 1, RULE, CLEAR(RULE), RULE, CLEAR(RULE));
	}
}

// A touch pad (the map's and the panels' arrows): a dark cell with a thin edge.
static void pad(float x0, float y0, float x1, float y1, const char* label_, u32 key, int dx, int dy)
{
	rect(x0, y0, x1, y1, PAD, PAD);
	outline(x0, y0, x1, y1, RULE);
	float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
	if (label_ && *label_) {
		if (dx || dy)
			arrow(cx, y0 + 9, dx, dy, 4.0f, RING), cy += 4;
		text(FONT_SMALL, cx, cy - font_line_px(FONT_SMALL) * 0.5f, label_, FONT_CENTER, s_grey_top, s_grey_bottom);
	} else {
		arrow(cx, cy, dx, dy, 6.0f, RING);
	}
	hit(x0, y0, x1, y1, HIT_KEY, 0, 0, key);
}

// ---------------------------------------------------------------- the pages

// The Options' tabs in place of the title: L and R either side, the tabs between in small capitals
// spread evenly, the chosen one white over a red rule.
#define TAB_RED 0xa3141cff
static void draw_tabs(const Menu* m, int id)
{
	glyph(4, 13, KEY_L, RING);
	hit(0, 0, 24, 28, HIT_KEY, 0, 0, KEY_L);
	glyph(301, 13, KEY_R, RING);
	hit(296, 0, 320, 28, HIT_KEY, 0, 0, KEY_R);
	char up[TOUI_MAX_TABS][16];
	float w[TOUI_MAX_TABS], total = 0.0f;
	for (int i = 0; i < m->ntabs; i++) {
		int k = 0;
		for (; m->tabs[i][k] && k < 15; k++)
			up[i][k] = m->tabs[i][k] >= 'a' && m->tabs[i][k] <= 'z' ? m->tabs[i][k] - 32 : m->tabs[i][k];
		up[i][k] = 0;
		w[i] = spaced_width(FONT_SMALL, up[i], 1.0f);
		total += w[i];
	}
	float gap = (268.0f - total) / (m->ntabs + 1), x = 26.0f + (gap > 4.0f ? gap : 4.0f);
	gap = gap > 4.0f ? gap : 4.0f;
	int cur = m->tab ? *m->tab : 0;
	float ty = floorf(13.0f - font_line_px(FONT_SMALL) * 0.5f);
	for (int i = 0; i < m->ntabs; i++) {
		text_spaced(FONT_SMALL, x + w[i] * 0.5f, ty, up[i], 1.0f, i == cur ? s_white : LABEL);
		if (i == cur)
			rect(x, 23, x + w[i], 25, TAB_RED, TAB_RED);
		hit(x - gap * 0.5f, 0, x + w[i] + gap * 0.5f, 28, HIT_TAB, id, i, 0);
		x += w[i] + gap;
	}
	rule(12, 308, 28, RULE);
}

// The PC's volume blocks on a row (0x45bf10, fifteen two-tone blocks), right-aligned at x1.
static void blocks(float x1, float cy, int level, int max, bool on)
{
	max = max < 1 ? 15 : max > 15 ? 15 : max;
	for (int i = 0; i < max; i++) {
		float x = x1 - (max - i) * 6.0f;
		bool lit = i < level;
		u32 t = lit ? (on ? 0xe2ded6ff : 0xa8a39bff) : 0x3a3533ff, b = lit ? (on ? 0x8c8882ff : 0x5c5852ff) : 0x2a2624ff;
		rect(x, cy - 5, x + 4.5f, cy, t, t);
		rect(x, cy, x + 4.5f, cy + 5, b, b);
	}
}

static void draw_menu(int id)
{
	Menu* m = &s_menus[id];
	if (m->ntabs)
		draw_tabs(m, id);
	else
		header(m->title);
	int sel = m->sel ? *m->sel : -1;
	float right = 312;
	float top = 32.0f, bottom = m->nb ? FOOT_Y - 4 : 234.0f;
	float rh = m->n ? (bottom - top) / m->n : 20.0f;
	rh = rh > 24.0f ? 24.0f : rh < 19.0f ? 19.0f : rh;
	int fit = (int)((bottom - top) / rh);
	int first = sel - fit / 2;
	first = first > m->n - fit ? m->n - fit : first;
	first = first < 0 ? 0 : first;
	float lh = font_line_px(FONT_BIG);
	for (int i = first; i < m->n && i < first + fit; i++) {
		float y = top + (i - first) * rh, ty = y + (rh - lh) * 0.5f;
		bool on = i == sel, off = m->dim >> i & 1;
		if (on)
			band(8, y, right, y + rh - 1);
		u32 ct = off ? DIM : s_grey_top, cb = off ? DIM : s_grey_bottom;
		// "Label    value" rows: the value goes to the right edge, a "< value >" one with the
		// PC's arrows either side while it is chosen
		const char* gap = strstr(m->rows[i], "  ");
		char lab[72];
		snprintf(lab, sizeof lab, "%.*s", gap ? (int)(gap - m->rows[i]) : 71, m->rows[i]);
		// a label too long for the row (some languages' scenario names) in the small font
		const TouiRow* kind = &m->kinds[i];
		float room = (gap ? right - 18 - font_width_px(FONT_BIG, gap) : right - 4) - 22;
		if (kind->kind == TOUI_ROW_BAR)
			room = right - 26 - (kind->max ? kind->max : 15) * 6.0f - 22;
		int lf = font_width_px(FONT_BIG, lab) > room ? FONT_SMALL : FONT_BIG;
		float ly = lf == FONT_BIG ? ty : y + (rh - font_line_px(FONT_SMALL)) * 0.5f;
		if (on && !off)
			text_lit(lf, 22, ly, lab, FONT_LEFT, s_white);
		else
			text(lf, 22, ly, lab, FONT_LEFT, ct, cb);
		bool lr = false;
		if (gap) {
			while (*gap == ' ')
				gap++;
			char val[72];
			snprintf(val, sizeof val, "%s", gap);
			size_t k = strlen(val);
			if (val[0] == '<' && k > 2 && val[k - 1] == '>') {
				lr = true;
				val[k - 1] = 0;
				char* v = val + 1;
				while (*v == ' ')
					v++;
				for (char* e = v + strlen(v); e > v && e[-1] == ' '; )
					*--e = 0;
				memmove(val, v, strlen(v) + 1);
			}
			float vx = right - (lr ? 18 : 10);
			if (on && !off)
				text_lit(FONT_BIG, vx, ty, val, FONT_RIGHT, s_white);
			else
				text(FONT_BIG, vx, ty, val, FONT_RIGHT, ct, cb);
			if (lr && on) {
				float w = font_width_px(FONT_BIG, val), cy = y + rh * 0.5f;
				arrow(vx - w - 8, cy, -1, 0, 3.5f, RING);
				arrow(right - 9, cy, 1, 0, 3.5f, RING);
			}
		} else if (kind->kind == TOUI_ROW_BAR) {
			float cy = y + rh * 0.5f, bx = right - 18;
			blocks(bx, cy, kind->level, kind->max, on && !off);
			lr = true;
			if (on) {
				arrow(bx - (kind->max ? kind->max : 15) * 6.0f - 6, cy, -1, 0, 3.5f, RING);
				arrow(right - 9, cy, 1, 0, 3.5f, RING);
			}
		} else if (kind->kind == TOUI_ROW_PAGE) {
			arrow(right - 9, y + rh * 0.5f, 1, 0, 3.5f, off ? DIM : RING);
		}
		hit(8, y, right, y + rh, HIT_ROW, id, i, lr ? 1 : 0);
	}
	if (first > 0)
		arrow(right - 4, top - 3, 0, -1, 3.0f, RING);
	if (first + fit < m->n)
		arrow(right - 4, bottom + 1, 0, 1, 3.0f, RING);
	footer(m->blabel, m->bkey, m->nb, 0);
}

// The first run's two choices, left and right as the drawings above them: the name, the chosen one
// white on the band, and its lines under it.
static void draw_choices(void)
{
	int sel = s_ch.sel ? *s_ch.sel : 0;
	float ny = 64.0f, ly = 92.0f, lh = font_line_px(FONT_SMALL);
	for (int i = 0; i < 2; i++) {
		float x0 = i ? 164.0f : 8.0f, x1 = i ? 312.0f : 156.0f, cx = (x0 + x1) * 0.5f;
		bool on = i == sel;
		int f = font_width_px(FONT_BIG, s_ch.name[i]) > x1 - x0 - 12 ? FONT_SMALL : FONT_BIG;
		if (on) {
			band(x0, ny - 5, x1, ny + font_line_px(FONT_BIG) + 4);
			text_lit(f, cx, ny, s_ch.name[i], FONT_CENTER, s_white);
		} else {
			text(f, cx, ny, s_ch.name[i], FONT_CENTER, s_grey_top, s_grey_bottom);
		}
		FontFit ff;
		font_fit_px(s_ch.line[i], x1 - x0 - 16, 3, false, &ff);
		for (int k = 0; k < ff.n; k++)
			text(ff.font, cx, ly + k * lh, ff.line[k], FONT_CENTER, on ? RING : LABEL, on ? RING : LABEL);
		hit(x0, 8, x1, FOOT_Y - 2, HIT_CHOICE, 0, i, 0);
	}
	rect(160, 44, 161, 104, CLEAR(RULE), RULE);
	rect(160, 104, 161, 164, RULE, CLEAR(RULE));
	footer(s_ch.blabel, s_ch.bkey, s_ch.nb, s_ch.dim);
}

static void draw_box(void)
{
	int lines = 1;
	for (const char* c = s_box; *c; c++)
		lines += *c == '\n';
	int f = lines > 9 ? FONT_SMALL : FONT_BIG;
	float y = lines > 9 ? 10 : 18;
	char ln[96];
	for (const char* s = s_box; *s; ) {
		const char* e = strchr(s, '\n');
		int n = e ? (int)(e - s) : (int)strlen(s);
		snprintf(ln, sizeof ln, "%.*s", n < 95 ? n : 95, s);
		text_lit(f, 20, y, ln, FONT_LEFT, s_white);
		y += font_line_px(f);
		s = e ? e + 1 : s + n;
	}
	if (s_box_nb < 0)
		return;                          // a subtitle over the game: no prompt, the taps aren't ours
	hit(0, 0, 320, FOOT_Y, HIT_KEY, 0, 0, s_box_nb ? 0 : KEY_A);
	if (s_box_nb) {
		footer(s_box_label, s_box_key, s_box_nb, 0);
	} else {
		static char next[1][16] = { "Next" };
		static const u32 a[1] = { KEY_A };
		footer(next, a, 1, 0);
	}
}

// A stage program's panel (puzzles, the hospital lift, keypads): text written for the old console,
// so its last line holds the keys ("D-pad: floor  A: press  B: leave"). That line becomes the
// footer (A with its verb, B as Back) and a touch D-pad under its own caption; a line with the
// console's ">" cursor gets the red band.
static void draw_panel(void)
{
	char body[512], help[160] = "";
	snprintf(body, sizeof body, "%s", s_panel);
	for (char* ln = body; ln && *ln; ) {
		// a line of keys: one with "A: ", "D-pad: ", "Left/Right: " or "Up/Down: " in it
		char* e = strchr(ln, '\n');
		int n = e ? (int)(e - ln) : (int)strlen(ln);
		char one[96];
		snprintf(one, sizeof one, "%.*s", n < 95 ? n : 95, ln);
		if (strstr(one, "A: ") || strstr(one, "D-pad: ") || strstr(one, "Left/Right: ") || strstr(one, "Up/Down: ")) {
			size_t k = strlen(help);
			snprintf(help + k, sizeof help - k, "%s%s", k ? "  " : "", one);
			const char* rest = e ? e + 1 : ln + n;
			memmove(ln, rest, strlen(rest) + 1);
			continue;
		}
		ln = e ? e + 1 : NULL;
	}
	for (char* e = body + strlen(body); e > body && (e[-1] == '\n' || e[-1] == ' '); )
		*--e = 0;                        // the blank lines the console kept above its keys
	// the keys: "<keys>: <verb>" pairs two spaces apart
	char a_verb[16] = "OK", pad_verb[32] = "";
	for (char* t = help; *t; ) {
		while (*t == ' ')
			t++;
		char* colon = strstr(t, ": ");
		if (!colon)
			break;
		char* end = strstr(colon + 2, "  ");
		int vn = end ? (int)(end - colon - 2) : (int)strlen(colon + 2);
		if (!strncmp(t, "A:", 2)) {
			snprintf(a_verb, sizeof a_verb, "%.*s", vn, colon + 2);
		} else if (strncmp(t, "B:", 2)) {           // D-pad, Left/Right, Up/Down
			size_t k = strlen(pad_verb);
			snprintf(pad_verb + k, sizeof pad_verb - k, "%s%.*s", k ? " / " : "", vn, colon + 2);
		}
		t = end ? end : colon + 2 + vn;
	}
	if (a_verb[0] >= 'a' && a_verb[0] <= 'z')
		a_verb[0] -= 32;
	// the text, the cursor's line on the band
	int lines = 0;
	for (const char* c = body; *c; c++)
		lines += *c == '\n';
	int f = lines > 11 ? FONT_SMALL : FONT_BIG;
	float y = 10, lh = font_line_px(f) + (f == FONT_BIG ? 1.0f : 0.0f);
	char ln[96];
	for (const char* p = body; *p; ) {
		const char* e = strchr(p, '\n');
		int n = e ? (int)(e - p) : (int)strlen(p);
		snprintf(ln, sizeof ln, "%.*s", n < 95 ? n : 95, p);
		char* lead = ln;
		while (*lead == ' ')
			lead++;
		if (lead[0] == '>' && lead[1] == ' ') {
			*lead = ' ';
			band(8, y - 2, help[0] ? 224 : 312, y + lh - 1);
			text_lit(f, 16, y, ln, FONT_LEFT, s_white);
		} else {
			text(f, 16, y, ln, FONT_LEFT, s_grey_top, s_grey_bottom);
		}
		y += lh;
		p = e ? e + 1 : p + n;
	}
	// the D-pad, a cross of pads in the bottom right
	if (help[0]) {
		float cx = 272, cy = 150, r = 15;
		if (pad_verb[0])
			label(cx, cy - 3 * r - 6, pad_verb, FONT_CENTER);
		pad(cx - r, cy - 3 * r, cx + r, cy - r, "", KEY_DUP, 0, -1);
		pad(cx - r, cy + r, cx + r, cy + 3 * r, "", KEY_DDOWN, 0, 1);
		pad(cx - 3 * r, cy - r, cx - r, cy + r, "", KEY_DLEFT, -1, 0);
		pad(cx + r, cy - r, cx + 3 * r, cy + r, "", KEY_DRIGHT, 1, 0);
	}
	char labels[2][16];
	snprintf(labels[0], 16, "%s", a_verb);
	snprintf(labels[1], 16, "Back");
	static const u32 keys[2] = { KEY_A, KEY_B };
	footer(labels, keys, 2, 0);
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

// The lowest button bound to an action, which a prompt shows and a tap presses.
static u32 bound(ControlsAction act)
{
	u32 k = controls_keys(act);
	return k & -k;
}

// In play (port/UI.md, "The play page"): the map around James (a tap opens the map screen, the
// ring in its corner zooms), his condition the way the PC's item screen shows it (a heartbeat
// line in the condition's colour: Fine green, Caution yellow, Danger red, with its word), the
// weapon and its rounds (a tap: the next weapon), the healing items (a tap uses one) and the
// prompts. waiting: a message is up on the top screen, and the footer is its "Next".
static void draw_play(bool waiting)
{
	rect(6, 6, 214, 200, PANEL, PANEL);
	outline(6, 6, 214, 200, RULE);
	MapPlayer pl = player();
	float sl = font_line_px(FONT_SMALL);
	if (!s_map_readable)
		text(FONT_SMALL, 110, 103 - sl * 0.5f, "Too dark to read", FONT_CENTER, DIM, DIM);
	else if (!map_mini(&pl, image, 7, 7, 213, 199, s_mini_zoom))
		text(FONT_SMALL, 110, 103 - sl * 0.5f, "No map", FONT_CENTER, DIM, DIM);
	hit(6, 6, 214, 200, HIT_PASS, 0, 0, 0);
	if (s_map_readable) {
		// the zoom ring in the map's corner, drawn with the pictures so the map never covers it
		// the ring round (198, 184), its bars one whole pixel wide on either side of that
		disc(198, 184, 9.0f, 0x000000c0);
		disc(198, 184, 8.0f, RING);
		disc(198, 184, 6.8f, PAD);
		bar_img(195, 184, 202, 185, RING);
		if (s_mini_zoom < MINI_CLOSE - 0.1f)
			bar_img(198, 181, 199, 188, RING);
		hit(182, 168, 214, 200, HIT_ZOOM, 0, 0, 0);
	}
	if (s_sound[0]) {
		char line[64];
		snprintf(line, sizeof line, "%s", s_sound);
		char* colon = strchr(line, ':');
		if (colon && font_width_px(FONT_SMALL, line) > 204.0f)
			*colon = '\0';              // the file's name only where it fits
		bar_img(7, 7, 213, 10 + sl, 0x000000d0);   // over the map, under the text
		text(FONT_SMALL, 110, 8, line, FONT_CENTER, 0xe03020ff, 0xe03020ff);
	}

	// condition
	static const char* const word[3] = { "Fine", "Caution", "Danger" };
	static const u32 col[3] = { 0x40d040ff, 0xe0c030ff, 0xe03020ff };
	int c = s_cond < 0 ? 0 : s_cond;
	label(224, 6, "Condition", FONT_LEFT);
	rect(222, 19, 314, 55, PANEL, PANEL);
	outline(222, 19, 314, 55, RULE);
	static const float beat[] = { 0, 0, 0.1f, -0.15f, 0.1f, 0, 0, 0.05f, -0.9f, 0.7f, -0.1f, 0, 0.12f, 0.18f, 0.08f, 0, 0 };
	const int nb = (int)(sizeof beat / sizeof *beat);
	float amp = c == 0 ? 15.0f : c == 1 ? 11.0f : 7.0f, step = 86.0f / (nb - 1);
	for (int i = 0; i + 1 < nb; i++)
		line(225 + i * step, 37 + beat[i] * amp, 225 + (i + 1) * step, 37 + beat[i + 1] * amp, 1.4f, col[c]);
	text(FONT_BIG, 268, 58, word[c], FONT_CENTER, col[c], col[c]);

	// the weapon: "name  loaded / spare", the name, then the rounds below it
	char name[40], rounds[24] = "";
	snprintf(name, sizeof name, "%s", s_weapon);
	char* gap = strstr(name, "  ");
	if (gap) {
		*gap = 0;
		for (gap += 2; *gap == ' '; gap++)
			;
		snprintf(rounds, sizeof rounds, "%s", gap);
	}
	label(224, 80, "Weapon", FONT_LEFT);
	if (name[0]) {
		int f = font_width_px(FONT_BIG, name) > 80.0f ? FONT_SMALL : FONT_BIG;
		text_lit(f, 224, 94 + (f == FONT_SMALL ? 2 : 0), name, FONT_LEFT, s_white);
		if (rounds[0])
			text(FONT_SMALL, 224, 110, rounds, FONT_LEFT, s_grey_top, s_grey_bottom);
		arrow(309, 101, 1, 0, 3.5f, RING);
		u32 k = bound(ACT_WEAPON_NEXT);
		if (k)
			hit(220, 76, 318, 124, HIT_KEY, 0, 0, k);
		else
			hit(220, 76, 318, 124, HIT_QUICK, 0, 0, 0);    // unbound: the tap is the next weapon itself
	} else {
		text(FONT_SMALL, 224, 96, "None", FONT_LEFT, DIM, DIM);
	}

	// the quick slots: the three healing items
	static const char* const heal[3] = { "Drink", "First Aid", "Ampoule" };
	label(224, 132, "Healing", FONT_LEFT);
	for (int i = 0; i < 3; i++) {
		float y = 146 + 18 * i;
		bool have = s_heal[i] > 0;
		u32 t = have ? s_grey_top : DIM, b = have ? s_grey_bottom : DIM;
		text(FONT_SMALL, 224, y + 3, heal[i], FONT_LEFT, t, b);
		char n[8];
		snprintf(n, sizeof n, "x%d", s_heal[i]);
		text(FONT_SMALL, 312, y + 3, n, FONT_RIGHT, t, b);
		if (have && !waiting)
			hit(220, y, 318, y + 18, HIT_QUICK, 0, 0, (u32)(i + 1));
	}

	if (waiting) {
		// a message on the top screen: the whole page is its A
		s_nhits = 0;
		hit(0, 0, 320, 240, HIT_KEY, 0, 0, KEY_A);
		static char next[1][16] = { "Next" };
		static const u32 a[1] = { KEY_A };
		footer(next, a, 1, 0);
		return;
	}
	// the map is the map itself: a tap on it opens it, so it has no prompt. Action says what a
	// press does now: attack (the weapon up) or check
	char labels[4][16] = { "Check", "Items", "Light", "Pause" };
	if (s_act_attack)
		snprintf(labels[0], sizeof labels[0], "Attack");
	u32 keys[4] = { bound(ACT_ACTION), bound(ACT_ITEMS), bound(ACT_FLASHLIGHT), KEY_START };
	footer(labels, keys, 4, s_have_light ? 0 : 4);
}

// The map screen (on the top screen): its name, the other floors and maps, zoom and close; while
// zoomed in, the arrows scroll for as long as they are held.
static void draw_map(void)
{
	header(s_map_name);
	if (s_map_zoomed) {
		// zoomed in: the page is a pad that moves the map under the finger
		float lh = font_line_px(FONT_SMALL);
		rect(16, 34, 304, 198, PAD, PAD);
		outline(16, 34, 304, 198, RULE);
		arrow(160, 60, 0, -1, 5.0f, DIM), arrow(160, 172, 0, 1, 5.0f, DIM);
		arrow(60, 116, -1, 0, 5.0f, DIM), arrow(260, 116, 1, 0, 5.0f, DIM);
		text(FONT_SMALL, 160, 116 - lh * 0.5f, "Drag to move the map", FONT_CENTER, s_grey_top, s_grey_bottom);
		hit(16, 34, 304, 198, HIT_DRAG, 0, 0, 0);
	} else {
		// behind the pads, a drag moves the full-width sheet up and down (map.c)
		hit(16, 34, 304, 198, HIT_DRAG, 0, 0, 0);
		pad(110, 40, 210, 80, "Floor up", KEY_DUP, 0, -1);
		pad(110, 140, 210, 180, "Floor down", KEY_DDOWN, 0, 1);
		pad(20, 90, 104, 130, "Other map", KEY_DLEFT, -1, 0);
		pad(216, 90, 300, 130, "Other map", KEY_DRIGHT, 1, 0);
	}
	static char la[2][16];
	snprintf(la[0], 16, "%s", s_map_zoomed ? "Zoom out" : "Zoom in");
	snprintf(la[1], 16, "Back");
	static const u32 keys[2] = { KEY_A, KEY_B };
	footer(la, keys, 2, 0);
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
	static char skip[1][16] = { "Skip" };
	static const u32 keys[1] = { KEY_START };
	footer(skip, keys, 1, 0);
}

static void status_check(void)
{
	float hp = combat_hp(), max = combat_hp_max();
	int cond = hp > 0.7f * max ? 0 : hp > 0.35f * max ? 1 : 2;
	// "name  loaded / spare": the item's own name (combat's is a short one), then the rounds
	int item = combat_weapon_item(), in_gun, spare;
	const char* name = item ? ev_item_name(item) : NULL;
	char weapon[40];
	int k = snprintf(weapon, sizeof weapon, "%.*s", name ? (int)strcspn(name, "\n") : 0, name ? name : "");
	if (combat_weapon_rounds(&in_gun, &spare) && k < (int)sizeof weapon)
		snprintf(weapon + k, sizeof weapon - k, "  %d / %d", in_gun, spare);
	MapPlayer pl = player();
	u32 key = s_map_readable ? map_mini_key(&pl, s_mini_zoom) : 1;
	if (key != s_mini_key) {
		s_mini_key = key;
		s_dirty = true;
	}
	int heal[3] = { ev_item_count(1), ev_item_count(2), ev_item_count(3) };
	bool light = ev_have_item(18);
	// music that should play and can't: said on the play page, not only on the debug page
	const char* sound = audio_problem();
	if (!sound && !bgm_ready())
		sound = "Music missing: sound/bgm.bin";
	if (strcmp(sound ? sound : "", s_sound)) {
		snprintf(s_sound, sizeof s_sound, "%s", sound ? sound : "");
		s_dirty = true;
	}
	if (cond != s_cond || strcmp(weapon, s_weapon) || memcmp(heal, s_heal, sizeof heal) || light != s_have_light) {
		s_cond = cond;
		memcpy(s_weapon, weapon, sizeof s_weapon);
		memcpy(s_heal, heal, sizeof heal);
		s_have_light = light;
		s_dirty = true;
	}
}

void touchui_action_attacks(bool on)
{
	if (on != s_act_attack) {
		s_act_attack = on;
		s_dirty = true;
	}
}

// What is up, most important first.
static int top_menu(void)
{
	static const int order[] = { TOUI_OPTIONS, TOUI_SAVE, TOUI_CHOICE, TOUI_PAUSE, TOUI_ITEMS, TOUI_MEMOS, TOUI_LEVELS, TOUI_TITLE };
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
	if (top_menu() < 0 && !s_panel[0] && !s_movie && !s_map_on)
		status_check();
	u64 now = svcGetSystemTick();
	if (s_press_tick && now - s_press_tick > SYSCLOCK_ARM11 / 7) {
		s_press_tick = 0;                // the tapped prompt goes out again
		s_dirty = true;
	}
	if (s_grain_ok && profile_new()) {
		int g = (int)(now / (SYSCLOCK_ARM11 / 12) & 3);   // the grain flickers about 12 times a second
		if (g != s_grain_frame) {
			s_grain_frame = g;
			s_dirty = true;
		}
	}
	if (!s_dirty || s_debug)
		return;
	s_dirty = false;
	s_nsolid = s_ntext = s_nhits = s_nimage = s_nimages = 0;
	backdrop();
	int m = top_menu();
	if (s_movie)
		draw_movie();
	else if (s_ch.on)
		draw_choices();
	else if (m >= 0)
		draw_menu(m);
	else if (s_box_on && (s_box[0] || s_box_nb))
		draw_box();
	else if (s_map_on)
		draw_map();
	else if (s_panel[0])
		draw_panel();
	else
		draw_play(s_box_on);
	if (s_press_tick)
		band(s_press[0], s_press[1], s_press[2], s_press[3]);
	if (s_grain_ok) {
		static const MapVert gv[6] = { { 0, 0, 0, 0 }, { 320, 0, 5, 0 }, { 320, 240, 5, 3.75f },
		                               { 0, 0, 0, 0 }, { 320, 240, 5, 3.75f }, { 0, 240, 0, 3.75f } };
		image(&s_grain[s_grain_frame], gv, 6, 0xffffffff);
	}

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

static void pressed(const Hit* h)
{
	s_press[0] = h->x0, s_press[1] = h->y0, s_press[2] = h->x1, s_press[3] = h->y1;
	s_press_tick = svcGetSystemTick();
	s_dirty = true;
}

void touchui_input(u32* down, u32* held)
{
	if (s_debug)
		return;
	touchPosition t;
	if (!(*held & KEY_TOUCH))
		s_drag = s_free = false;
	if (!(*down & KEY_TOUCH) && s_free && (*held & KEY_TOUCH)) {
		hidTouchRead(&t);
		s_free_dx += t.px - s_free_x, s_free_dy += t.py - s_free_y;
		s_free_x = t.px, s_free_y = t.py;
		return;
	}
	if (!(*down & KEY_TOUCH)) {
		if (s_drag && s_map_on && (*held & KEY_TOUCH)) {
			hidTouchRead(&t);
			map_drag((float)(t.px - s_drag_x), (float)(t.py - s_drag_y));
			s_drag_x = t.px, s_drag_y = t.py;
			*held &= ~KEY_TOUCH;
			return;
		}
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
			s_mini_zoom = s_mini_zoom > MINI_NEAR + 0.1f ? MINI_NEAR : MINI_CLOSE;
			s_dirty = true;
		} else if (h->kind == HIT_PASS) {
			*down |= KEY_TOUCH;
		} else if (h->kind == HIT_DRAG) {
			s_drag = true;
			s_drag_x = t.px, s_drag_y = t.py;
		} else if (h->kind == HIT_QUICK) {
			s_quick = h->key ? TQ_HEAL : TQ_WEAPON, s_quick_item = (int)h->key;
			pressed(h);
		} else if (h->kind == HIT_KEY) {
			*down |= h->key;
			if (h->y0 >= FOOT_Y || h->x1 - h->x0 < 320)
				pressed(h);
		} else if (h->kind == HIT_TAB) {
			Menu* m = &s_menus[h->menu];
			if (m->tab)
				*m->tab = h->row;
			s_dirty = true;
		} else if (h->kind == HIT_CHOICE) {
			if (s_ch.sel && *s_ch.sel == h->row)
				*down |= KEY_A;          // a second tap on the chosen one takes it
			else if (s_ch.sel)
				*s_ch.sel = h->row;
			s_dirty = true;
		} else {
			Menu* m = &s_menus[h->menu];
			if (!m->sel)
				return;
			float w = h->x1 - h->x0;
			if (h->key && t.px < h->x0 + w * 0.3f && *m->sel == h->row)
				*down |= KEY_DLEFT;
			else if (h->key && t.px > h->x1 - w * 0.3f && *m->sel == h->row)
				*down |= KEY_DRIGHT;
			else if (*m->sel == h->row || (m->flags & TOUI_ONE_TAP))
				*down |= KEY_A;          // a second tap on the chosen row takes it (an action list's first)
			*m->sel = h->row;
			s_dirty = true;
		}
		return;
	}
	s_free = true;
	s_free_x = t.px, s_free_y = t.py;
}

bool touchui_free_drag(int* dx, int* dy)
{
	*dx = s_free_dx, *dy = s_free_dy;
	s_free_dx = s_free_dy = 0;
	return s_free;
}
