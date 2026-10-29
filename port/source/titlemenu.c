#include "titlemenu.h"
#include "events.h"
#include "font.h"
#include "movie.h"
#include "sfx.h"
#include "touchui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>

enum { SC_MENU, SC_SCENARIO, SC_LEVELS, SC_MOVIES, SC_BOOT };

// the menu's items by their place (0x496280 draws each at its own y, whether shown or not)
enum { IT_LOAD, IT_CONTINUE, IT_NEW_GAME, IT_OPTIONS, IT_MOVIES, IT_EXIT, IT_VIEWER, IT_N };

static C3D_Tex s_bg, s_glow, s_boot[2];
static bool s_bg_ok, s_glow_ok, s_boot_ok[2], s_saves;
static float s_glow_t;           // 0x88ead4: the logo's pulse, 15.5 s round
static int s_boot_pic;
static float s_boot_t, s_boot_b, s_boot_speed;   // time on the picture; its brightness; 0 fading in
static int s_screen, s_sel, s_step;      // s_step: levels 0 action, 1 riddle
static bool s_wish;
static int s_action_sel = 2, s_riddle_sel = 2;   // 1-based as the PC: action 1 hard .. 4 beginner
static float s_idle;
static int s_touch_sel;
static int s_movie_n, s_movie_first;
static int s_movie_msg[24];

// Movie Menu: option_msg_e.mes 214..237 and the PC's files (the list at "d:\data\movie\...")
static const char* const MOVIE_FILE[24] = {
	"open_bgm", "open_voc", "toilet", "hakaba", "gero", "knife", "hei", "deai", "flash", "saikai", "rouya",
	"korosu_a", "korosu_b", "murder", "end_mria", "end_rbrt", "end_scde", "ending", "end_wish", "end_dog",
	"sh2e3", "sh3e3", "credits", "sh3tgs",
};

static const char* msg(int n, const char* fallback)
{
	const char* m = ev_option_message(n);
	return m && *m ? m : fallback;
}

static bool load(const char* pack_dir, const char* name, C3D_Tex* tex)
{
	char path[128];
	snprintf(path, sizeof path, "%s/ui/%s.t3x", pack_dir, name);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	return true;
}

bool titlemenu_init(const char* pack_dir)
{
	s_bg_ok = load(pack_dir, "start00", &s_bg);
	s_glow_ok = load(pack_dir, "titleglow", &s_glow);
	s_boot_ok[0] = load(pack_dir, "boot0", &s_boot[0]);
	s_boot_ok[1] = load(pack_dir, "boot1", &s_boot[1]);
	s_movie_n = 0;
	for (int i = 0; i < 24; i++)
		if (movie_exists(MOVIE_FILE[i]))
			s_movie_msg[s_movie_n++] = i;
	return s_bg_ok;
}

static bool shown(int it)
{
	if (it == IT_LOAD || it == IT_CONTINUE)
		return s_saves;
	if (it == IT_MOVIES)
		return s_movie_n > 0;
	return it != IT_VIEWER;     // the room viewer is a touch-screen row only
}

// The room viewer's row on the touch list, only with "viewer 1" in debug.txt.
static bool s_viewer;
void titlemenu_viewer(bool on) { s_viewer = on; }

static bool listed(int it) { return shown(it) || (it == IT_VIEWER && s_viewer); }

void titlemenu_boot(void)
{
	s_screen = SC_BOOT;
	s_boot_pic = s_boot_ok[0] ? 0 : 1;
	s_boot_t = s_boot_b = s_boot_speed = 0.0f;
	if (!s_boot_ok[s_boot_pic])
		s_screen = SC_MENU;
}

void titlemenu_no_boot(void)
{
	if (s_screen == SC_BOOT) {
		touchui_movie(false);
		s_screen = SC_MENU;
	}
}

bool titlemenu_booting(void) { return s_screen == SC_BOOT; }

void titlemenu_reset(bool saves)
{
	s_saves = saves;
	if (s_screen != SC_BOOT)             // the start-up screens run on into the menu
		s_screen = SC_MENU;
	s_sel = saves ? IT_CONTINUE : IT_NEW_GAME;
	s_idle = 0.0f;
	// the touch list's row for it (the first update reads the row back)
	s_touch_sel = 0;
	for (int it = 0; it < s_sel; it++)
		s_touch_sel += listed(it);
}

// ---------------------------------------------------------------- the touch screen

static void touch(void)
{
	static const TouiButton prompts[2] = { { "Select", KEY_A }, { "Back", KEY_B } };
	static const TouiButton play[2] = { { "Play", KEY_A }, { "Back", KEY_B } };
	static char text[24][48];
	const char* rows[24];
	int n = 0;
	switch (s_screen) {
	case SC_MENU: {
		static int map[IT_N];
		for (int it = 0; it < IT_N; it++)
			if (listed(it)) {
				snprintf(text[n], sizeof text[n], "%s", it == IT_VIEWER ? "Room viewer (debug)" : msg(238 + it, "?"));
				map[n] = it;
				if (it == s_sel)
					s_touch_sel = n;
				rows[n] = text[n], n++;
			}
		touchui_menu_close(TOUI_LEVELS);
		static const TouiButton select[1] = { { "Select", KEY_A } };
		touchui_menu_ex(TOUI_TITLE, "SILENT HILL 2", rows, n, &s_touch_sel, select, 1, 0, TOUI_ONE_TAP);
		(void)map;
		return;
	}
	case SC_SCENARIO:
		rows[0] = msg(252, "Main Scenario"), rows[1] = msg(253, "Sub Scenario");
		n = 2;
		s_touch_sel = s_sel;
		touchui_menu_ex(TOUI_LEVELS, msg(240, "NEW GAME"), rows, n, &s_touch_sel, prompts, 2, 0, TOUI_ONE_TAP);
		return;
	case SC_LEVELS: {
		int k = s_step == 0 ? 4 : 3;
		for (int i = 0; i < k; i++)
			rows[i] = msg(248 + i, "?");
		s_touch_sel = (s_step == 0 ? s_action_sel : s_riddle_sel) - 1;
		touchui_menu_ex(TOUI_LEVELS, msg(s_step == 0 ? 246 : 247, "LEVEL"), rows, k, &s_touch_sel, prompts, 2, 0,
		                TOUI_ONE_TAP);
		return;
	}
	case SC_MOVIES:
		for (int i = 0; i < s_movie_n; i++)
			rows[i] = msg(214 + s_movie_msg[i], MOVIE_FILE[s_movie_msg[i]]);
		s_touch_sel = s_sel;
		touchui_menu(TOUI_LEVELS, msg(210, "Movie Menu"), rows, s_movie_n, &s_touch_sel, play, 2);
		return;
	}
}

// the menu row the touch list's row stands for
static int menu_item(int row)
{
	int n = 0;
	for (int it = 0; it < IT_N; it++)
		if (listed(it)) {
			if (n == row)
				return it;
			n++;
		}
	return IT_NEW_GAME;
}

// ---------------------------------------------------------------- input

static void go(int screen, int sel)
{
	s_screen = screen;
	s_sel = sel;
	s_idle = 0.0f;
}

int titlemenu_update(u32 down, float dt, int* action, int* riddle, const char** movie)
{
	// a tap on the touch list moves the choice first (touchui writes the row)
	if (s_screen == SC_MENU)
		s_sel = menu_item(s_touch_sel);
	else if (s_screen == SC_LEVELS)
		*(s_step == 0 ? &s_action_sel : &s_riddle_sel) = s_touch_sel + 1;
	else
		s_sel = s_touch_sel;
	s_idle = down ? 0.0f : s_idle + dt;
	s_glow_t += dt;
	if (s_glow_t >= 15.5f)
		s_glow_t -= 15.5f;
	if (s_screen == SC_BOOT) {
		// 0x405e90: in over a second, a second, out over a second; a key fades out in half a second
		// (the touch screen shows only Skip, which is START)
		touchui_menu_close(TOUI_TITLE);
		touchui_menu_close(TOUI_LEVELS);
		touchui_movie(true);
		s_boot_t += dt;
		if (s_boot_speed == 0.0f) {
			s_boot_b = s_boot_t < 1.0f ? s_boot_t : 1.0f;
			if (down & (KEY_A | KEY_B | KEY_START | KEY_TOUCH))
				s_boot_speed = 2.0f;
			else if (s_boot_t >= 2.0f)
				s_boot_speed = 1.0f;
		} else {
			s_boot_b -= dt * s_boot_speed;
		}
		if (s_boot_speed > 0.0f && s_boot_b <= 0.0f) {
			s_boot_t = s_boot_b = s_boot_speed = 0.0f;
			if (++s_boot_pic > 1 || !s_boot_ok[s_boot_pic]) {
				touchui_movie(false);
				s_screen = SC_MENU;
				s_idle = 0.0f;
				touch();
			}
		}
		return TM_NONE;
	}
	if (down & KEY_START)
		down |= KEY_A;               // START chooses, as A does (it skips the logos above)
	int result = TM_NONE;
	bool up = down & KEY_DUP, dn = down & KEY_DDOWN;
	switch (s_screen) {
	case SC_MENU:
		if (up || dn) {
			int it = s_sel;
			do
				it = (it + (dn ? 1 : IT_N - 1)) % IT_N;
			while (!listed(it));
			s_sel = it;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		if (down & KEY_A) {
			switch (s_sel) {
			case IT_LOAD: result = TM_LOAD; break;
			case IT_CONTINUE: result = TM_CONTINUE; break;
			case IT_NEW_GAME:
				// 0x4976b0: NEW GAME goes to the scenarios, the last one played chosen
				sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
				go(SC_SCENARIO, s_wish ? 1 : 0);
				break;
			case IT_OPTIONS: result = TM_OPTIONS; break;
			case IT_MOVIES:
				sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
				s_movie_first = 0;
				go(SC_MOVIES, 0);
				break;
			case IT_EXIT: result = TM_EXIT; break;
			case IT_VIEWER: result = TM_VIEWER; break;
			}
		}
		// 0x4976b0: a minute without a key, then the opening movie (title event 8, menu event 0xc)
		if (s_idle > 60.0f && result == TM_NONE) {
			s_idle = 0.0f;
			result = TM_ATTRACT;
		}
		break;
	case SC_SCENARIO:
		if (up || dn) {
			s_sel ^= 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		if (down & KEY_A) {
			s_wish = s_sel == 1;
			s_step = 0;
			s_action_sel = 2;            // 0x496070: NORMAL
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			go(SC_LEVELS, 0);
		} else if (down & KEY_B) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			go(SC_MENU, IT_NEW_GAME);
		}
		break;
	case SC_LEVELS: {
		int* sel = s_step == 0 ? &s_action_sel : &s_riddle_sel;
		int k = s_step == 0 ? 4 : 3;
		if (up || dn) {
			*sel = (*sel - 1 + (dn ? 1 : k - 1)) % k + 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		if (down & KEY_A) {
			if (s_step == 0 && !s_wish) {
				// 0x497bb0: on to the riddle level (Born From a Wish has none)
				s_step = 1;
				s_riddle_sel = 2;        // 0x495f60: NORMAL
				sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
				s_idle = 0.0f;
			} else {
				*action = 4 - s_action_sel;
				*riddle = 3 - s_riddle_sel;
				result = s_wish ? TM_WISH : TM_NEW_GAME;    // the caller plays the game start sound
			}
		} else if (down & KEY_B) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			if (s_step == 1)
				s_step = 0;
			else
				go(SC_SCENARIO, s_wish ? 1 : 0);
		}
		// 60 seconds without a key: back to the menu (0x495fc0)
		if (s_idle > 60.0f) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			go(SC_MENU, IT_NEW_GAME);
		}
		break;
	}
	case SC_MOVIES:
		if ((up || dn) && s_movie_n) {
			s_sel = (s_sel + (dn ? 1 : s_movie_n - 1)) % s_movie_n;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		if ((down & KEY_A) && s_movie_n) {
			*movie = MOVIE_FILE[s_movie_msg[s_sel]];
			result = TM_MOVIE;
		} else if (down & KEY_B) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			go(SC_MENU, IT_MOVIES);
		}
		break;
	}
	if (s_screen == SC_MOVIES) {
		if (s_sel < s_movie_first)
			s_movie_first = s_sel;
		if (s_sel >= s_movie_first + 8)
			s_movie_first = s_sel - 7;
	}
	if (result == TM_NONE)
		touch();
	else
		touchui_menu_close(TOUI_LEVELS);
	return result;
}

// ---------------------------------------------------------------- the top screen

// 0x496280 and the level screens: grey (0x7f, PS2 units, doubled) at alpha 0x70, the chosen one at
// full alpha a pixel up and left; the small font (0x47f350)
static void item(float x, float y, const char* s, bool chosen, float bright)
{
	u32 g = (u32)(254 * bright) & 0xff;
	u32 c = g << 24 | g << 16 | g << 8;
	font_text(FONT_SMALL, x, y, s, FONT_CENTER, c | 0xe0, c | 0xe0);
	if (chosen)
		font_text(FONT_SMALL, x - 1, y - 1, s, FONT_CENTER, c | 0xff, c | 0xff);
}

bool titlemenu_draw_top(MapDrawFn draw, float bright)
{
	if (s_screen == SC_BOOT) {
		if (!draw || !s_boot_ok[s_boot_pic])
			return true;
		float b = s_boot_b < 0.0f ? 0.0f : s_boot_b > 1.0f ? 1.0f : s_boot_b;
		u32 c = (u32)(b * 255.0f) & 0xff;
		MapVert v[6] = { { 40, 0, 0, 0 }, { 360, 0, 1, 0 }, { 360, 240, 1, 1 },
		                 { 40, 0, 0, 0 }, { 360, 240, 1, 1 }, { 40, 240, 0, 1 } };
		draw(&s_boot[s_boot_pic], v, 6, c << 24 | c << 16 | c << 8 | 0xff);
		return true;
	}
	if (s_bg_ok && draw) {
		// 0x496dd0: start00 over the whole 4:3 picture
		u32 b = (u32)(bright * 255.0f) & 0xff;
		MapVert v[6] = { { 40, 0, 0, 0 }, { 360, 0, 1, 0 }, { 360, 240, 1, 1 },
		                 { 40, 0, 0, 0 }, { 360, 240, 1, 1 }, { 40, 240, 0, 1 } };
		draw(&s_bg, v, 6, b << 24 | b << 16 | b << 8 | 0xff);
		if (s_glow_ok) {
			// 0x496dd0 / 0x496a10: start01's white logo at (-257, -103)-(255, -19) of the 512x448
			// screen, its colour from (255, 255, 255, 128) to (38, 38, 38, 8) and back over 15.5 s
			float f = sinf(s_glow_t / 15.5f * 3.14159265f);
			float c = 255.0f + (38.0f - 255.0f) * f, a = (128.0f + (8.0f - 128.0f) * f) * 2.0f;
			u32 cc = (u32)(c * bright) & 0xff, aa = (u32)(a > 255.0f ? 255.0f : a) & 0xff;
			float y0 = 120.0f - 103.0f * 240.0f / 448.0f, y1 = 120.0f - 19.0f * 240.0f / 448.0f;
			float x0 = 200.0f - 257.0f * 0.625f, x1 = 200.0f + 255.0f * 0.625f, t1 = 96.0f / 128.0f;
			MapVert g[6] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x1, y1, 1, t1 },
			                 { x0, y0, 0, 0 }, { x1, y1, 1, t1 }, { x0, y1, 0, t1 } };
			draw(&s_glow, g, 6, cc << 24 | cc << 16 | cc << 8 | aa);
		}
	}
	if (!font_ready())
		return s_bg_ok;
	// The rows are on the touch screen (port/UI.md); the top keeps the picture and shows what has
	// been chosen: the scenario, both levels (the one being chosen lit), the movie.
	switch (s_screen) {
	case SC_SCENARIO:
		item(256, 324, msg(240, "NEW GAME"), false, bright);
		item(256, 350, msg(252 + s_sel, s_sel ? "SUB SCENARIO" : "MAIN SCENARIO"), true, bright);
		break;
	case SC_LEVELS:
		if (s_wish) {
			// 0x496490: the action level alone, in the middle
			item(256, 324, msg(246, "ACTION LEVEL"), false, bright);
			item(256, 350, msg(248 + s_action_sel - 1, "?"), true, bright);
		} else {
			item(160, 324, msg(246, "ACTION LEVEL"), false, bright);
			item(352, 324, msg(247, "RIDDLE LEVEL"), false, bright);
			item(160, 350, msg(248 + s_action_sel - 1, "?"), s_step == 0, bright);
			if (s_step == 1)
				item(352, 350, msg(248 + s_riddle_sel - 1, "?"), true, bright);
		}
		break;
	case SC_MOVIES:
		item(256, 324, msg(210, "Movie Menu"), false, bright);
		if (s_movie_n)
			item(256, 350, msg(214 + s_movie_msg[s_sel], MOVIE_FILE[s_movie_msg[s_sel]]), true, bright);
		break;
	}
	return s_bg_ok;
}
