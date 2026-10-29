#include "savescreen.h"
#include "events.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define PER_PAGE 5               // 0x4537b0 draws rows from y 151 in steps of 40 up to 350

enum { ST_OFF, ST_LIST, ST_OVERWRITE, ST_DONE };

static SaveScreenHost s_host;
static int s_mode, s_state = ST_OFF, s_sel, s_chapter;
static SaveSlotInfo s_info[SAVE_SLOTS];
static bool s_ok;                // the last write worked
static C3D_Tex s_bg;
static bool s_bg_ok;
static float s_clock;            // seconds since the screen opened (the top screen's motion)
static float s_sel_t, s_state_t; // when the file and the question last changed
static u64 s_clock_ms;
static int s_shown_sel, s_shown_state;

static const char* card(int n, const char* fallback)
{
	const char* m = ev_mcard_message(n);
	return m && *m ? m : fallback;
}

static void bg_load(bool on)
{
	if (s_bg_ok) {
		C3D_TexDelete(&s_bg);
		s_bg_ok = false;
	}
	FILE* f = on ? fopen(s_chapter == 1 ? PACK_DIR "/ui/savebg2.t3x" : PACK_DIR "/ui/savebg.t3x", "rb") : NULL;
	if (!f)
		return;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_bg, NULL, false);
	fclose(f);
	if (t) {
		Tex3DS_TextureFree(t);
		C3D_TexSetFilter(&s_bg, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&s_bg, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		s_bg_ok = true;
	}
}

static void refresh(void)
{
	for (int i = 0; i < SAVE_SLOTS; i++) {
		memset(&s_info[i], 0, sizeof s_info[i]);
		s_info[i].place = -1;
		s_info[i].used = s_host.info && s_host.info(i, &s_info[i]);
	}
}

// The file's place: the save point's name (m_card_x_msg_e.mes 0..), "Game Result" for clear data.
static const char* place(const SaveSlotInfo* in)
{
	static char buf[48];
	if (in->clear)
		return card(80, "Game Result");
	const char* m = in->place >= 0 ? ev_mcard_message(in->place) : NULL;
	if (m && *m)
		return m;
	snprintf(buf, sizeof buf, "%.7s", in->map);
	return buf;
}

static void play_time(const SaveSlotInfo* in, char* out, int size)
{
	int t = (int)in->time;
	snprintf(out, size, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
}

// The touch screen (port/UI.md): the ten files to pick from, the overwrite question's answers
// (acting on the first tap), "OK" under "Save completed."; the top screen shows the chosen file.
static int s_touch_answer;       // 0 Yes, 1 No

static void touch(void)
{
	static const TouiButton save[2] = { { "Save", KEY_A }, { "Back", KEY_B } };
	static const TouiButton load[2] = { { "Load", KEY_A }, { "Back", KEY_B } };
	static const TouiButton answer[2] = { { "Select", KEY_A }, { "Back", KEY_B } };
	static const TouiButton ok[1] = { { "OK", KEY_A } };
	if (s_state == ST_LIST) {
		static char text[SAVE_SLOTS][48];
		const char* rows[SAVE_SLOTS];
		u64 dim = 0;
		for (int i = 0; i < SAVE_SLOTS; i++) {
			char t[16];
			play_time(&s_info[i], t, sizeof t);
			if (s_info[i].used) {
				snprintf(text[i], sizeof text[i], "%d. %.22s   %s", i + 1, place(&s_info[i]), s_info[i].clear ? "" : t);
			} else {
				snprintf(text[i], sizeof text[i], "%d. %s", i + 1, s_mode == SS_LOAD ? "-" : card(36, "Save to new file."));
				if (s_mode == SS_LOAD)
					dim |= 1ull << i;
			}
			rows[i] = text[i];
		}
		touchui_box(NULL, NULL, 0);
		touchui_menu_ex(TOUI_SAVE, s_mode == SS_LOAD ? "Load" : "Save", rows, SAVE_SLOTS, &s_sel,
		                s_mode == SS_LOAD ? load : save, 2, dim, 0);
	} else if (s_state == ST_OVERWRITE) {
		const char* rows[2] = { "Yes", "No" };
		touchui_box(NULL, NULL, 0);
		touchui_menu_ex(TOUI_SAVE, card(46, "Do you wish to overwrite?"), rows, 2, &s_touch_answer, answer, 2, 0,
		                TOUI_ONE_TAP);
	} else if (s_state == ST_DONE) {
		touchui_menu_close(TOUI_SAVE);
		touchui_box("", ok, 1);
	}
}

void savescreen_open(int mode, const SaveScreenHost* host, int chapter)
{
	s_host = *host;
	s_mode = mode;
	s_chapter = chapter;
	s_state = ST_LIST;
	refresh();
	// the newest file first, as the PC's list opens on the last one used
	s_sel = 0;
	float best = -1.0f;
	for (int i = 0; i < SAVE_SLOTS; i++)
		if (s_info[i].used && mode == SS_LOAD && s_info[i].time > best)
			best = s_info[i].time, s_sel = i;
	bg_load(true);
	s_clock = s_sel_t = s_state_t = 0.0f;
	s_clock_ms = osGetTime();
	s_shown_sel = s_sel;
	s_shown_state = s_state;
	touch();
}

bool savescreen_active(void) { return s_state != ST_OFF; }

static void close_screen(void)
{
	s_state = ST_OFF;
	touchui_menu_close(TOUI_SAVE);
	touchui_box(NULL, NULL, 0);
	bg_load(false);
}

bool savescreen_update(u32 down)
{
	if (s_state == ST_OFF)
		return false;
	u64 now = osGetTime();
	s_clock += (float)(now - s_clock_ms) / 1000.0f;
	s_clock_ms = now;
	if (s_state == ST_LIST) {
		if (down & (KEY_DUP | KEY_DDOWN)) {
			s_sel = (s_sel + (down & KEY_DDOWN ? 1 : SAVE_SLOTS - 1)) % SAVE_SLOTS;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		if (down & (KEY_L | KEY_R | KEY_DLEFT | KEY_DRIGHT)) {       // the other page
			s_sel = (s_sel + PER_PAGE) % SAVE_SLOTS;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		if (down & KEY_B) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			close_screen();
			return false;
		}
		if (down & KEY_A) {
			const SaveSlotInfo* in = &s_info[s_sel];
			if (s_mode == SS_LOAD) {
				if (!in->used) {
					sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
				} else {
					sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
					int slot = s_sel;
					close_screen();
					if (s_host.load)
						s_host.load(slot);
					return false;
				}
			} else if (in->used) {
				sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
				s_state = ST_OVERWRITE;
				s_touch_answer = 1;      // No first
			} else {
				s_ok = s_host.write && s_host.write(s_sel, s_mode);
				sfx_play(s_ok ? SFX_MENU_OK : SFX_MENU_CANCEL, 1.0f, 0.0f);
				refresh();
				s_state = ST_DONE;
			}
		}
	} else if (s_state == ST_OVERWRITE) {
		if (down & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) {
			s_touch_answer ^= 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		if ((down & KEY_A) && s_touch_answer == 1)
			down = KEY_B;            // No
		if (down & KEY_A) {
			s_ok = s_host.write && s_host.write(s_sel, s_mode);
			sfx_play(s_ok ? SFX_MENU_OK : SFX_MENU_CANCEL, 1.0f, 0.0f);
			refresh();
			s_state = ST_DONE;
		} else if (down & KEY_B) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			s_state = ST_LIST;
		}
	} else if (s_state == ST_DONE) {
		if (down & (KEY_A | KEY_B)) {
			close_screen();
			return false;
		}
	}
	if (s_sel != s_shown_sel)
		s_shown_sel = s_sel, s_sel_t = s_clock;
	if (s_state != s_shown_state)
		s_shown_state = s_state, s_state_t = s_clock;
	touch();
	return true;
}

// The level a file was played on, in the title screen's words (option_msg_e.mes 248..: HARD,
// NORMAL, EASY, BEGINNER; action 0 beginner .. 3 hard, riddle 0 easy .. 2 hard).
static const char* level(int n)
{
	const char* m = ev_option_message(248 + n);
	return m && *m ? m : "?";
}

// The top screen (port/UI.md), the port's own layout of the PC's parts: James' picture (Maria's in
// Born From a Wish) over the whole width, the title, and the file chosen on the touch screen in full
// at the lower left, where a shade keeps it readable. The picture drifts and breathes slowly (the
// title logo's pulse is the model), and the file's lines slide in when another file is chosen, its
// name on the touch screen's red band.

// top-screen pixels to the PC's text space (font.h)
static float pcx(float px) { return 256.0f + (px - 200.0f) / 0.625f; }
static float pcy(float py) { return 256.0f + (py - 120.0f) / 0.46875f; }

static u32 faded(u32 rgba, float a)
{
	a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
	return (rgba & 0xffffff00u) | (u32)((rgba & 0xff) * a);
}

static float ease(float t, float dur)        // 0..1, eased out
{
	float x = t / dur;
	x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
	return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
}

#define LEFT 22.0f                   // the text's left edge, in pixels
#define BAND_RED 0x741010ffu         // touchui.c RED

void savescreen_draw_top(void)
{
	if (s_state == ST_OFF || !font_ready())
		return;
	float t = s_clock;
	float in = ease(t, 0.35f);                       // the screen fading in over the scene
	u32 white, yellow, grey_t, grey_b, dummy;
	font_pc_color(0, &white, &dummy);
	font_pc_color(4, &yellow, &dummy);
	font_pc_color(10, &grey_t, &grey_b);
	const float tau = 6.2831853f;
	if (s_bg_ok) {
		// the 4:3 picture over the 5:3 screen: 1.25 times its size fills the width, a slow zoom and
		// drift on top, the crop kept inside the picture
		float zoom = 1.27f + 0.05f * (0.5f - 0.5f * cosf(tau * t / 26.0f));
		float w = 320.0f * zoom, h = 240.0f * zoom;
		float cx = 200.0f + 0.4f * (w - 400.0f) * sinf(tau * t / 37.0f);
		float cy = 120.0f + 0.25f * (h - 240.0f) + 0.2f * (h - 240.0f) * sinf(tau * t / 29.0f);   // the face sits high
		float u0 = 0.5f - cx / w, v0 = 0.5f - cy / h;  // the picture under the screen's corners
		float u1 = u0 + 400.0f / w, v1 = v0 + 240.0f / h;
		float b = 0.86f + 0.14f * (0.5f + 0.5f * sinf(tau * t / 7.0f));
		u32 c = 0xff;
		c |= (u32)(255.0f * b) << 24 | (u32)(255.0f * b) << 16 | (u32)(255.0f * b) << 8;
		c = faded(c, in);
		const u32 pic[4] = { c, c, c, c };
		font_quad_px(&s_bg, 0.0f, 0.0f, 400.0f, 240.0f, u0, v0, u1, v1, pic);
	} else {
		const u32 black[4] = { faded(0x000000ff, in), faded(0x000000ff, in), faded(0x000000ff, in), faded(0x000000ff, in) };
		font_quad_px(NULL, 0.0f, 0.0f, 400.0f, 240.0f, 0, 0, 0, 0, black);
	}
	// the shade under the text: from the left edge across, and from the middle down
	const u32 left[4] = { faded(0x000000ff, 0.6f * in), 0x00000000, faded(0x000000ff, 0.6f * in), 0x00000000 };
	font_quad_px(NULL, 0.0f, 0.0f, 260.0f, 240.0f, 0, 0, 0, 0, left);
	const u32 low[4] = { 0x00000000, 0x00000000, faded(0x000000ff, 0.85f * in), faded(0x000000ff, 0.85f * in) };
	font_quad_px(NULL, 0.0f, 110.0f, 400.0f, 240.0f, 0, 0, 0, 0, low);

	font_text(FONT_SMALL, pcx(LEFT), pcy(14.0f), card(154, "SILENT HILL 2 Save Files"), FONT_LEFT,
	          faded(white, in), faded(white, in));

	// the chosen file, sliding in from the left when it changes
	float k = ease(t - s_sel_t, 0.28f) * in;
	float x = pcx(LEFT - 12.0f * (1.0f - k));
	const SaveSlotInfo* f = &s_info[s_sel];
	const char* title = !f->used ? (s_mode == SS_LOAD ? card(33, "No file data.") : card(36, "Save to new file.")) :
	                    place(f);
	// the touch screen's red band (touchui.c band) behind the file's name, sweeping in with it
	float bx = (LEFT + font_width(FONT_BIG, title) * 0.625f + 40.0f) * k, fade = bx * 0.3f;
	u32 red = faded(BAND_RED, 0.9f * k), clear = faded(BAND_RED, 0.0f);
	const u32 solid[4] = { red, red, red, red }, edge[4] = { red, clear, red, clear };
	font_quad_px(NULL, 0.0f, 151.0f, bx - fade, 169.0f, 0, 0, 0, 0, solid);
	font_quad_px(NULL, bx - fade, 151.0f, bx, 169.0f, 0, 0, 0, 0, edge);
	char line[64];
	snprintf(line, sizeof line, "File %d", s_sel + 1);
	font_text(FONT_SMALL, x, pcy(140.0f), line, FONT_LEFT, faded(grey_t, k), faded(grey_b, k));
	if (!f->used) {
		font_text(FONT_BIG, x, pcy(154.0f), title, FONT_LEFT, faded(white, k), faded(white, k));
	} else {
		font_text(FONT_BIG, x, pcy(154.0f), title, FONT_LEFT, faded(yellow, k), faded(yellow, k));
		if (f->clear) {
			snprintf(line, sizeof line, "Clears %d", f->clears);
			font_text(FONT_SMALL, x, pcy(176.0f), line, FONT_LEFT, faded(white, k), faded(white, k));
		} else {
			char tm[16];
			play_time(f, tm, sizeof tm);
			snprintf(line, sizeof line, "Play time %s     Saves %d", tm, f->saves);
			font_text(FONT_SMALL, x, pcy(176.0f), line, FONT_LEFT, faded(white, k), faded(white, k));
			if (f->chapter == 1)
				snprintf(line, sizeof line, "Born From a Wish     Action %s", level(3 - f->action));
			else
				snprintf(line, sizeof line, "Action %s     Riddle %s", level(3 - f->action), level(2 - f->riddle));
			font_text(FONT_SMALL, x, pcy(189.0f), line, FONT_LEFT, faded(grey_t, k), faded(grey_b, k));
		}
	}
	// the question and the result where the PC puts them, below the file, fading in
	const char* ask = s_state == ST_OVERWRITE ? card(46, "Do you wish to overwrite?") :
	                  s_state == ST_DONE ? (s_ok ? card(40, "Save completed.") : card(41, "Save error occurred.")) : NULL;
	if (ask) {
		float q = ease(t - s_state_t, 0.25f) * in;
		font_text(FONT_BIG, pcx(LEFT), pcy(212.0f), ask, FONT_LEFT, faded(yellow, q), faded(yellow, q));
	}
}
