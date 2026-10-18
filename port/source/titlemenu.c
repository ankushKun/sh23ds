#include "titlemenu.h"
#include "events.h"
#include "font.h"
#include "movie.h"
#include "sfx.h"
#include "touchui.h"
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>

enum { SC_MENU, SC_SCENARIO, SC_LEVELS, SC_MOVIES };

// the menu's items by their place (0x496280 draws each at its own y, whether shown or not)
enum { IT_LOAD, IT_CONTINUE, IT_NEW_GAME, IT_OPTIONS, IT_MOVIES, IT_EXIT, IT_VIEWER, IT_N };

static C3D_Tex s_bg;
static bool s_bg_ok, s_saves;
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

bool titlemenu_init(const char* pack_dir)
{
	char path[128];
	snprintf(path, sizeof path, "%s/ui/start00.t3x", pack_dir);
	FILE* f = fopen(path, "rb");
	if (f) {
		Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_bg, NULL, false);
		fclose(f);
		if (t) {
			Tex3DS_TextureFree(t);
			C3D_TexSetFilter(&s_bg, GPU_LINEAR, GPU_LINEAR);
			s_bg_ok = true;
		}
	}
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

void titlemenu_reset(bool saves)
{
	s_saves = saves;
	s_screen = SC_MENU;
	s_sel = saves ? IT_CONTINUE : IT_NEW_GAME;
	s_idle = 0.0f;
	// the touch list's row for it (the first update reads the row back)
	s_touch_sel = 0;
	for (int it = 0; it < s_sel; it++)
		s_touch_sel += shown(it) || it == IT_VIEWER;
}

// ---------------------------------------------------------------- the touch screen

static void touch(void)
{
	static const TouiButton back[1] = { { "Back", KEY_B } };
	static char text[24][48];
	const char* rows[24];
	int n = 0;
	switch (s_screen) {
	case SC_MENU: {
		static int map[IT_N];
		for (int it = 0; it < IT_N; it++)
			if (shown(it) || it == IT_VIEWER) {
				snprintf(text[n], sizeof text[n], "%s", it == IT_VIEWER ? "Room viewer (debug)" : msg(238 + it, "?"));
				map[n] = it;
				if (it == s_sel)
					s_touch_sel = n;
				rows[n] = text[n], n++;
			}
		touchui_menu_close(TOUI_LEVELS);
		touchui_menu(TOUI_TITLE, "SILENT HILL 2", rows, n, &s_touch_sel, NULL, 0);
		(void)map;
		return;
	}
	case SC_SCENARIO:
		rows[0] = msg(252, "Main Scenario"), rows[1] = msg(253, "Sub Scenario");
		n = 2;
		s_touch_sel = s_sel;
		touchui_menu(TOUI_LEVELS, msg(240, "NEW GAME"), rows, n, &s_touch_sel, back, 1);
		return;
	case SC_LEVELS: {
		int k = s_step == 0 ? 4 : 3;
		for (int i = 0; i < k; i++)
			rows[i] = msg(248 + i, "?");
		s_touch_sel = (s_step == 0 ? s_action_sel : s_riddle_sel) - 1;
		touchui_menu(TOUI_LEVELS, msg(s_step == 0 ? 246 : 247, "LEVEL"), rows, k, &s_touch_sel, back, 1);
		return;
	}
	case SC_MOVIES:
		for (int i = 0; i < s_movie_n; i++)
			rows[i] = msg(214 + s_movie_msg[i], MOVIE_FILE[s_movie_msg[i]]);
		s_touch_sel = s_sel;
		touchui_menu(TOUI_LEVELS, msg(210, "Movie Menu"), rows, s_movie_n, &s_touch_sel, back, 1);
		return;
	}
}

// the menu row the touch list's row stands for
static int menu_item(int row)
{
	int n = 0;
	for (int it = 0; it < IT_N; it++)
		if (shown(it) || it == IT_VIEWER) {
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
	int result = TM_NONE;
	bool up = down & KEY_DUP, dn = down & KEY_DDOWN;
	switch (s_screen) {
	case SC_MENU:
		if (up || dn) {
			int it = s_sel;
			do
				it = (it + (dn ? 1 : IT_N - 1)) % IT_N;
			while (!shown(it) && it != IT_VIEWER);
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
	if (s_bg_ok && draw) {
		// 0x496dd0: start00 over the whole 4:3 picture
		u32 b = (u32)(bright * 255.0f) & 0xff;
		MapVert v[6] = { { 40, 0, 0, 0 }, { 360, 0, 1, 0 }, { 360, 240, 1, 1 },
		                 { 40, 0, 0, 0 }, { 360, 240, 1, 1 }, { 40, 240, 0, 1 } };
		draw(&s_bg, v, 6, b << 24 | b << 16 | b << 8 | 0xff);
	}
	if (!font_ready())
		return s_bg_ok;
	switch (s_screen) {
	case SC_MENU:
		for (int it = 0; it < IT_VIEWER; it++)
			if (shown(it))
				item(256, 284 + 22 * it, msg(238 + it, "?"), it == s_sel, bright);
		break;
	case SC_SCENARIO:
		item(234, 324, msg(252, "MAIN SCENARIO"), s_sel == 0, bright);
		item(234, 350, msg(253, "SUB SCENARIO"), s_sel == 1, bright);
		break;
	case SC_LEVELS:
		if (s_wish) {
			// 0x496490: the action level alone, in the middle
			item(256, 290, msg(246, "ACTION LEVEL"), true, bright);
			for (int i = 0; i < 4; i++)
				item(256, 322 + 22 * i, msg(248 + i, "?"), s_action_sel == i + 1, bright);
		} else {
			item(128, 290, msg(246, "ACTION LEVEL"), s_step == 0, bright);
			item(384, 290, msg(247, "RIDDLE LEVEL"), s_step == 1, bright);
			for (int i = 0; i < 4; i++)
				item(128, 322 + 22 * i, msg(248 + i, "?"), s_action_sel == i + 1, bright);
			for (int i = 0; i < 3; i++)
				item(384, 322 + 22 * i, msg(248 + i, "?"), s_step == 1 && s_riddle_sel == i + 1, bright);
		}
		break;
	case SC_MOVIES:
		item(256, 270, msg(210, "Movie Menu"), false, bright);
		for (int i = s_movie_first; i < s_movie_n && i < s_movie_first + 8; i++)
			item(256, 296 + 22 * (i - s_movie_first), msg(214 + s_movie_msg[i], MOVIE_FILE[s_movie_msg[i]]),
			     i == s_sel, bright);
		break;
	}
	return s_bg_ok;
}
