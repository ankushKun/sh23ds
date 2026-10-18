#include "pausemenu.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <stdio.h>

// the PC's English strings (0x7abd84, by language in 0x407310)
static const char* const ITEM[5] = { "Resume", "Save Game", "Load Game", "Options", "Quit" };

static bool s_on, s_can_save, s_ask;
static int s_sel, s_yes;          // 0x9352b4; gPauseMenuQuitIndex (0 Yes, 1 No)
static int s_touch_sel;

static void touch(void)
{
	static const TouiButton back[1] = { { "Back", KEY_B } };
	static const TouiButton yesno[2] = { { "Yes", KEY_Y }, { "No", KEY_X } };
	if (s_ask) {
		touchui_menu_close(TOUI_PAUSE);
		touchui_box("Exit to Main Menu?", yesno, 2);
		return;
	}
	touchui_box(NULL, NULL, 0);
	static char save[24];
	snprintf(save, sizeof save, "%s%s", ITEM[1], s_can_save ? "" : "  (not here)");
	const char* rows[7] = { ITEM[0], save, ITEM[2], ITEM[3], ITEM[4], "Map", "Memos" };
	s_touch_sel = s_sel;
	touchui_menu(TOUI_PAUSE, "PAUSED", rows, 7, &s_touch_sel, back, 1);
}

void pausemenu_open(bool can_save)
{
	s_on = true;
	s_can_save = can_save;
	s_ask = false;
	s_sel = 0;
	touch();
}

void pausemenu_close(void)
{
	s_on = false;
	s_ask = false;
	touchui_menu_close(TOUI_PAUSE);
	touchui_box(NULL, NULL, 0);
}

int pausemenu_update(u32 down)
{
	if (!s_on)
		return PM_NONE;
	if (s_ask) {
		// "Exit to Main Menu?": left and right move, A takes it, B is No
		if (down & (KEY_DLEFT | KEY_DRIGHT)) {
			s_yes ^= 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		}
		int pick = down & KEY_Y ? 0 : down & KEY_X ? 1 : down & KEY_A ? s_yes : down & KEY_B ? 1 : -1;
		if (pick == 0) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			return PM_QUIT;
		}
		if (pick == 1) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			s_ask = false;
			touch();
		}
		return PM_NONE;
	}
	if (s_touch_sel != s_sel && s_touch_sel >= 0 && s_touch_sel < 7)
		s_sel = s_touch_sel;
	if (down & (KEY_START | KEY_B)) {
		sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		return PM_RESUME;
	}
	if (down & (KEY_DUP | KEY_DDOWN)) {
		// 0x401fc0 event 0x10: five rows round, Save Game passed over where it cannot be used
		int d = down & KEY_DDOWN ? 1 : -1, n = s_sel >= 5 ? 7 : 5;
		s_sel = (s_sel + d + n) % n;
		if (s_sel == 1 && !s_can_save)
			s_sel = (s_sel + d + n) % n;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		touch();
	}
	if (down & KEY_A) {
		switch (s_sel) {
		case 0:
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			return PM_RESUME;
		case 1:
			if (!s_can_save) {
				sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
				return PM_NONE;
			}
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			return PM_SAVE;
		case 2:
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			return PM_LOAD;
		case 3:
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			return PM_OPTIONS;
		case 4:
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			s_ask = true;
			s_yes = 1;               // thunk 0x4072b0(1): No first
			touch();
			return PM_NONE;
		case 5:
			return PM_MAP;
		case 6:
			return PM_MEMOS;
		}
	}
	return PM_NONE;
}

// 0x47edb0's PS2 units doubled: 0x7f grey, alpha 0x60 -> 0xc0, greyed 0x55 -> 0xaa
static void text(float x, float y, const char* s, int align, u32 rgb, u32 a)
{
	font_text(FONT_BIG, x, y, s, align, rgb | a, rgb | a);
}

void pausemenu_draw_top(void)
{
	if (!s_on || !font_ready())
		return;
	const u32 grey = 0xfefefe00u, dim = 0xaaaaaa00u;
	if (s_ask) {
		// 0x407620 state 10: the question at (256, 232), Yes right-aligned at 226 and No at 286
		text(256, 232, "Exit to Main Menu?", FONT_CENTER, grey, 0xff);
		text(226, 266, "Yes", FONT_RIGHT, grey, 0xc0);
		text(286, 266, "No", FONT_LEFT, grey, 0xc0);
		if (s_yes == 0)
			text(224, 264, "Yes", FONT_RIGHT, grey, 0xff);
		else
			text(284, 264, "No", FONT_LEFT, grey, 0xff);
		return;
	}
	text(256, 164, "PAUSED", FONT_CENTER, grey, 0xff);
	for (int i = 0; i < 5; i++) {
		bool off = i == 1 && !s_can_save;
		text(256, 230 + 30 * i, ITEM[i], FONT_CENTER, off ? dim : grey, 0xc0);
		if (i == s_sel && !off)
			text(254, 228 + 30 * i, ITEM[i], FONT_CENTER, grey, 0xff);
	}
}
