#include "savescreen.h"
#include "events.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <citro3d.h>
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

static void touch(void)
{
	static const TouiButton save[2] = { { "Save", KEY_A }, { "Back", KEY_B } };
	static const TouiButton load[2] = { { "Load", KEY_A }, { "Back", KEY_B } };
	static const TouiButton yesno[2] = { { "Yes", KEY_A }, { "No", KEY_B } };
	static const TouiButton ok[1] = { { "OK", KEY_A } };
	if (s_state == ST_LIST) {
		static char text[SAVE_SLOTS][48];
		const char* rows[SAVE_SLOTS];
		for (int i = 0; i < SAVE_SLOTS; i++) {
			char t[16];
			play_time(&s_info[i], t, sizeof t);
			if (s_info[i].used)
				snprintf(text[i], sizeof text[i], "%d %.24s   %s", i + 1, place(&s_info[i]), s_info[i].clear ? "" : t);
			else
				snprintf(text[i], sizeof text[i], "%d %s", i + 1, s_mode == SS_LOAD ? card(33, "No file data.") :
				         card(36, "Save to new file."));
			rows[i] = text[i];
		}
		touchui_box(NULL, NULL, 0);
		touchui_menu(TOUI_SAVE, card(154, "SILENT HILL 2 Save Files"), rows, SAVE_SLOTS, &s_sel,
		             s_mode == SS_LOAD ? load : save, 2);
	} else {
		touchui_menu_close(TOUI_SAVE);
		if (s_state == ST_OVERWRITE)
			touchui_box(card(46, "Do you wish to overwrite?"), yesno, 2);
		else if (s_state == ST_DONE)
			touchui_box(s_ok ? card(40, "Save completed.") : card(41, "Save error occurred."), ok, 1);
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
			} else {
				s_ok = s_host.write && s_host.write(s_sel, s_mode);
				sfx_play(s_ok ? SFX_MENU_OK : SFX_MENU_CANCEL, 1.0f, 0.0f);
				refresh();
				s_state = ST_DONE;
			}
		}
	} else if (s_state == ST_OVERWRITE) {
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
	touch();
	return true;
}

void savescreen_draw_top(void)
{
	if (s_state == ST_OFF || !font_ready())
		return;
	u32 white, yellow, grey_t, grey_b, dummy;
	font_pc_color(0, &white, &dummy);
	font_pc_color(4, &yellow, &dummy);
	font_pc_color(10, &grey_t, &grey_b);
	if (s_bg_ok) {
		// the picture is 4:3, so the sides of the wide screen go black first (the same texture
		// tinted black over the whole width, x -64..576 in the PC's space)
		font_image(&s_bg, -64.0f, 0.0f, 576.0f, 512.0f, 0x000000ff);
		font_image(&s_bg, 0.0f, 0.0f, 512.0f, 512.0f, 0xffffffff);
	}
	font_text(FONT_BIG, 36, 110, card(154, "SILENT HILL 2 Save Files"), FONT_LEFT, white, white);
	int first = s_sel / PER_PAGE * PER_PAGE;
	for (int i = first; i < first + PER_PAGE && i < SAVE_SLOTS; i++) {
		const SaveSlotInfo* in = &s_info[i];
		float y = 151 + 40 * (i - first);
		bool on = i == s_sel;
		u32 t = on ? yellow : white;
		char line[64];
		snprintf(line, sizeof line, "File %d", i + 1);
		font_text(FONT_SMALL, 40, y + 4, line, FONT_LEFT, on ? yellow : grey_t, on ? yellow : grey_b);
		if (!in->used) {
			font_text(FONT_BIG, 120, y, s_mode == SS_LOAD ? card(33, "No file data.") : card(36, "Save to new file."),
			          FONT_LEFT, t, t);
			continue;
		}
		// a long place name goes small, so the time and the saves keep their column
		const char* pl = place(in);
		int f = font_width(FONT_BIG, pl) > 230.0f ? FONT_SMALL : FONT_BIG;
		font_text(f, 120, f == FONT_BIG ? y : y + 4, pl, FONT_LEFT, t, t);
		if (in->clear) {
			snprintf(line, sizeof line, "%d", in->clears);
		} else {
			char tm[16];
			play_time(in, tm, sizeof tm);
			snprintf(line, sizeof line, "%s   Saves %d", tm, in->saves);
		}
		font_text(FONT_SMALL, 480, y + 4, line, FONT_RIGHT, on ? yellow : grey_t, on ? yellow : grey_b);
	}
	char page[16];
	snprintf(page, sizeof page, "%d / %d", first / PER_PAGE + 1, (SAVE_SLOTS + PER_PAGE - 1) / PER_PAGE);
	font_text(FONT_SMALL, 480, 360, page, FONT_RIGHT, grey_t, grey_b);
	if (s_state == ST_OVERWRITE)
		font_text(FONT_BIG, 256, 400, card(46, "Do you wish to overwrite?"), FONT_CENTER, yellow, yellow);
	else if (s_state == ST_DONE)
		font_text(FONT_BIG, 256, 400, s_ok ? card(40, "Save completed.") : card(41, "Save error occurred."),
		          FONT_CENTER, yellow, yellow);
}
