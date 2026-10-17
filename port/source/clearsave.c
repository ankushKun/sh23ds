#include "clearsave.h"
#include "events.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"

enum { CS_OFF, CS_ASK, CS_SLOTS, CS_OVERWRITE, CS_DONE };

static ClearSaveHost s_host;
static int s_state = CS_OFF;
static int s_sel;                  // CS_SLOTS: 0 the slot, 1 do not save; CS_OVERWRITE: 0 yes, 1 no
static char s_slot[48];
static bool s_slot_used, s_slot_clear, s_saved;
static bool s_dirty;
static C3D_Tex s_bg;               // the save screen's picture (pack/ui/savebg.t3x)
static bool s_bg_ok;

static void bg_load(bool on)
{
	if (s_bg_ok) {
		C3D_TexDelete(&s_bg);
		s_bg_ok = false;
	}
	FILE* f = on ? fopen(PACK_DIR "/ui/savebg.t3x", "rb") : NULL;
	if (!f)
		return;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_bg, NULL, false);
	fclose(f);
	if (t) {
		Tex3DS_TextureFree(t);
		C3D_TexSetFilter(&s_bg, GPU_LINEAR, GPU_LINEAR);
		s_bg_ok = true;
	}
}

// m_card_x_msg_e.mes with the port's words when the pack has no mcard.msg
static const char* card(int n, const char* fallback)
{
	const char* m = ev_mcard_message(n);
	return m && *m ? m : fallback;
}

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
	{
		// the touch screen: the same question and buttons (touchui.c)
		static char t[160];
		const char* ask = s_host.message ? s_host.message(26) : NULL;
		static const TouiButton go[1] = { { "Continue", KEY_A } };
		static const TouiButton slots[2] = { { "Save", KEY_A }, { "Do not save", KEY_B } };
		static const TouiButton yesno[2] = { { "Yes", KEY_A }, { "No", KEY_B } };
		if (s_state == CS_ASK) {
			touchui_box(ask ? ask : "Save clear data.", go, 1);
		} else if (s_state == CS_SLOTS) {
			snprintf(t, sizeof t, "%s\n\nSlot 1   %s", ask ? ask : "Save clear data.", s_slot);
			touchui_box(t, slots, 2);
		} else if (s_state == CS_OVERWRITE) {
			snprintf(t, sizeof t, "Slot 1 holds %s.\nWrite the clear data over it?", s_slot_clear ? "clear data" : "a saved game");
			touchui_box(t, yesno, 2);
		} else if (s_state == CS_DONE) {
			touchui_box(s_saved ? "The clear data was saved." : "Could not write the save file.", go, 1);
		}
	}
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
			bg_load(true);
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
			bg_load(false);
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
			bg_load(false);
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
	// The PC's save screen (0x453fc0, drawn by 0x4537b0): its picture over the whole screen, the
	// title at (36, 110), a file a row from y 150 in steps of 40 and the questions in yellow. The PC
	// lists every file of its save folder; the port has the one slot.
	if (s_bg_ok)
		font_image(&s_bg, 0.0f, 0.0f, 512.0f, 512.0f, 0xffffffff);
	font_text(FONT_BIG, 36, 110, card(154, "SILENT HILL 2 Save Files"), FONT_LEFT, white, white);
	if (s_state == CS_SLOTS || s_state == CS_OVERWRITE) {
		char line[80];
		if (!s_slot_used)
			snprintf(line, sizeof line, "%s", card(36, "Save to new file."));
		else if (s_slot_clear)
			snprintf(line, sizeof line, "%s   %s", card(80, "Game Result"), s_slot);
		else
			snprintf(line, sizeof line, "%s", s_slot);
		bool on = s_state == CS_SLOTS && s_sel == 0;
		font_text(FONT_BIG, 56, 150, line, FONT_LEFT, on ? yellow : white, on ? yellow : white);
		if (s_state == CS_SLOTS) {
			font_text(FONT_BIG, 56, 190, "Do not save", FONT_LEFT, s_sel == 1 ? yellow : white, s_sel == 1 ? yellow : white);
		} else {
			font_text(FONT_BIG, 256, 300, card(46, "Do you wish to overwrite?"), FONT_CENTER, yellow, yellow);
			font_text(FONT_BIG, 200, 350, "Yes", FONT_CENTER, s_sel == 0 ? yellow : dim, s_sel == 0 ? yellow : dim);
			font_text(FONT_BIG, 312, 350, "No", FONT_CENTER, s_sel == 1 ? yellow : dim, s_sel == 1 ? yellow : dim);
		}
	} else if (s_state == CS_DONE) {
		font_text(FONT_BIG, 256, 300, s_saved ? card(40, "Save completed.") : card(41, "Save error occurred."),
		          FONT_CENTER, yellow, yellow);
	}
}
