#include "pausemenu.h"
#include "events.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <stdio.h>

// The PC's strings by language (0x407310 picks the table, English 0x7abd84): the title, the five
// rows, the quit question, Yes and No, from the pack's pause.msg; English when the pack has none.
static const char* const ENGLISH[9] = { "PAUSED", "Resume", "Save Game", "Load Game", "Options", "Quit",
	"Exit to Main Menu?", "Yes", "No" };
enum { S_TITLE, S_ROW, S_ASK = 6, S_YES, S_NO };

static const char* str(int n)
{
	const char* m = ev_pause_message(n);
	return m && *m ? m : ENGLISH[n];
}

static bool s_on, s_can_save, s_ask;
static int s_sel, s_yes;          // 0x9352b4; gPauseMenuQuitIndex (0 Yes, 1 No)
static int s_touch_sel, s_touch_yes;
static char s_place[48];
static float s_time;

void pausemenu_where(const char* place, float play_time)
{
	snprintf(s_place, sizeof s_place, "%s", place ? place : "");
	s_time = play_time;
}

// The touch screen's lists (port/UI.md): the menu, or the question's answers, each acting on the
// first tap; Save Game is dim where it can't be used.
static void touch(void)
{
	static const TouiButton prompts[2] = { { "Select", KEY_A }, { "Back", KEY_B } };
	touchui_box(NULL, NULL, 0);
	if (s_ask) {
		const char* yesno[2] = { str(S_YES), str(S_NO) };
		s_touch_yes = s_yes;
		touchui_menu_ex(TOUI_PAUSE, str(S_ASK), yesno, 2, &s_touch_yes, prompts, 2, 0, TOUI_ONE_TAP);
		return;
	}
	const char* rows[7] = { str(S_ROW), str(S_ROW + 1), str(S_ROW + 2), str(S_ROW + 3), str(S_ROW + 4), "Map", "Memos" };
	s_touch_sel = s_sel;
	touchui_menu_ex(TOUI_PAUSE, str(S_TITLE), rows, 7, &s_touch_sel, prompts, 2, s_can_save ? 0 : 2, TOUI_ONE_TAP);
}

void pausemenu_open_at(bool can_save, int row)
{
	s_on = true;
	s_can_save = can_save;
	s_ask = false;
	s_sel = row >= 0 && row < 7 ? row : 0;
	touch();
}

void pausemenu_open(bool can_save) { pausemenu_open_at(can_save, 0); }

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
		// "Exit to Main Menu?": the D-Pad moves (left and right as on the PC, up and down on the
		// touch screen's list), A takes it, B is No
		if (s_touch_yes != s_yes && (s_touch_yes == 0 || s_touch_yes == 1))
			s_yes = s_touch_yes;
		if (down & (KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN)) {
			s_yes ^= 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
			touch();
		}
		int pick = down & KEY_A ? s_yes : down & KEY_B ? 1 : -1;
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
		// 0x401fc0 event 0x10: the rows round, Save Game passed over where it cannot be used (the
		// PC's five, then the port's Map and Memos)
		int d = down & KEY_DDOWN ? 1 : -1, n = 7;
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
	const u32 grey = 0xfefefe00u;
	if (s_ask) {
		// 0x407620 state 10: the question at (256, 232); the answers are on the touch screen
		text(256, 232, str(S_ASK), FONT_CENTER, grey, 0xff);
		return;
	}
	text(256, 196, str(S_TITLE), FONT_CENTER, grey, 0xff);
	// where James is and the play time, in the small font (the port's)
	char t[24];
	int s = (int)s_time;
	snprintf(t, sizeof t, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	if (s_place[0])
		font_text(FONT_SMALL, 256, 244, s_place, FONT_CENTER, 0xc8c8c8c0, 0xc8c8c8c0);
	font_text(FONT_SMALL, 256, s_place[0] ? 268 : 244, t, FONT_CENTER, 0xa0a0a0c0, 0xa0a0a0c0);
}
