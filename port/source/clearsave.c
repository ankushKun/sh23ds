#include "clearsave.h"
#include "font.h"
#include "sfx.h"
#include <stdio.h>
#include <string.h>

enum { CS_OFF, CS_ASK, CS_SLOTS, CS_OVERWRITE, CS_DONE };

static ClearSaveHost s_host;
static int s_state = CS_OFF;
static int s_sel;                  // CS_SLOTS: 0 the slot, 1 do not save; CS_OVERWRITE: 0 yes, 1 no
static char s_slot[48];
static bool s_slot_used, s_slot_clear, s_saved;
static bool s_dirty;

void clearsave_begin(const ClearSaveHost* host)
{
	s_host = *host;
	s_state = CS_ASK;
	s_sel = 0;
	s_dirty = true;
}

static void refresh_slot(void)
{
	s_slot_clear = false;
	s_slot_used = s_host.slot_info && s_host.slot_info(s_slot, sizeof s_slot, &s_slot_clear);
	if (!s_slot_used)
		snprintf(s_slot, sizeof s_slot, "No data");
}

static void bottom(void)
{
	if (!s_dirty)
		return;
	s_dirty = false;
	printf("\x1b[2J");
	const char* ask = s_host.message ? s_host.message(26) : NULL;
	printf("\x1b[3;3H%s", ask ? ask : "Save clear data.");
	if (s_state == CS_ASK) {
		printf("\x1b[29;24HA: continue");
	} else if (s_state == CS_SLOTS) {
		printf("\x1b[7;3H%c Slot 1  %-28.28s", s_sel == 0 ? '>' : ' ', s_slot);
		printf("\x1b[9;3H%c Do not save", s_sel == 1 ? '>' : ' ');
		printf("\x1b[29;12HA: choose   B: do not save");
	} else if (s_state == CS_OVERWRITE) {
		printf("\x1b[7;3HSlot 1 holds %s.", s_slot_clear ? "clear data" : "a saved game");
		printf("\x1b[8;3HWrite the clear data over it?");
		printf("\x1b[10;5H%c Yes    %c No", s_sel == 0 ? '>' : ' ', s_sel == 1 ? '>' : ' ');
		printf("\x1b[29;14HD-pad: choose   A: OK");
	} else if (s_state == CS_DONE) {
		printf("\x1b[7;3H%s", s_saved ? "The clear data was saved." : "Could not write the save file.");
		printf("\x1b[29;27HA: title");
	}
}

static void write_now(void)
{
	s_saved = s_host.write && s_host.write();
	sfx_play(s_saved ? SFX_MENU_OK : SFX_MENU_CANCEL, 1.0f, 0.0f);
	s_state = CS_DONE;
}

bool clearsave_update(u32 down)
{
	if (s_state == CS_OFF)
		return false;
	switch (s_state) {
	case CS_ASK:                     // 0x447650 case 4: any button opens the save screen
		if (down & (KEY_A | KEY_B | KEY_START)) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			refresh_slot();
			s_state = CS_SLOTS, s_sel = 0;
			s_dirty = true;
		}
		break;
	case CS_SLOTS:
		if (down & (KEY_DUP | KEY_DDOWN)) {
			s_sel ^= 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
			s_dirty = true;
		}
		if ((down & KEY_B) || ((down & KEY_A) && s_sel == 1)) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			s_state = CS_OFF;
			return false;
		}
		if (down & KEY_A) {
			if (s_slot_used) {
				sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
				s_state = CS_OVERWRITE, s_sel = 1;
			} else {
				write_now();
			}
			s_dirty = true;
		}
		break;
	case CS_OVERWRITE:
		if (down & (KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN)) {
			s_sel ^= 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
			s_dirty = true;
		}
		if ((down & KEY_B) || ((down & KEY_A) && s_sel == 1)) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			s_state = CS_SLOTS, s_sel = 0;
			s_dirty = true;
		} else if (down & KEY_A) {
			write_now();
			s_dirty = true;
		}
		break;
	case CS_DONE:
		if (down & (KEY_A | KEY_B | KEY_START)) {
			s_state = CS_OFF;
			return false;
		}
		break;
	}
	bottom();
	return true;
}

void clearsave_draw_top(void)
{
	if (s_state == CS_OFF || !font_ready())
		return;
	u32 white, yellow, dim, dummy;
	font_pc_color(0, &white, &dummy);
	font_pc_color(4, &yellow, &dummy);
	dim = 0x808080ff;
	const char* ask = s_host.message ? s_host.message(26) : NULL;
	ask = ask ? ask : "Save clear data.";
	if (s_state == CS_ASK) {
		// message 0x1a in colour 4, centred at (256, 240)
		font_text(FONT_BIG, 256, 240, ask, FONT_CENTER, yellow, yellow);
		return;
	}
	// The PC's save screen has its own pictures (data/menu/mc/savebg*.tbn2); this is the port's.
	font_text(FONT_BIG, 256, 96, ask, FONT_CENTER, yellow, yellow);
	if (s_state == CS_SLOTS) {
		char line[64];
		snprintf(line, sizeof line, "Slot 1   %s", s_slot);
		font_text(FONT_BIG, 96, 200, line, FONT_LEFT, s_sel == 0 ? yellow : white, s_sel == 0 ? yellow : white);
		font_text(FONT_BIG, 96, 248, "Do not save", FONT_LEFT, s_sel == 1 ? yellow : white, s_sel == 1 ? yellow : white);
	} else if (s_state == CS_OVERWRITE) {
		font_text(FONT_BIG, 256, 200, s_slot_clear ? "Overwrite the clear data in slot 1?" :
		          "Overwrite the game saved in slot 1?", FONT_CENTER, white, white);
		font_text(FONT_BIG, 200, 260, "Yes", FONT_CENTER, s_sel == 0 ? yellow : dim, s_sel == 0 ? yellow : dim);
		font_text(FONT_BIG, 312, 260, "No", FONT_CENTER, s_sel == 1 ? yellow : dim, s_sel == 1 ? yellow : dim);
	} else if (s_state == CS_DONE) {
		font_text(FONT_BIG, 256, 220, s_saved ? "The clear data was saved." : "Could not write the save file.",
		          FONT_CENTER, white, white);
	}
}
