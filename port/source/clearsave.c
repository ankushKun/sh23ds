#include "clearsave.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <stdio.h>
#include <string.h>

enum { CS_OFF, CS_ASK, CS_FILES };

static ClearSaveHost s_host;
static int s_state = CS_OFF;

static const char* ask(void)
{
	const char* m = s_host.message ? s_host.message(26) : NULL;
	return m && *m ? m : "Save clear data.";
}

void clearsave_begin(const ClearSaveHost* host)
{
	s_host = *host;
	s_state = CS_ASK;
	static const TouiButton go[1] = { { "Continue", KEY_A } };
	touchui_box(ask(), go, 1);
	printf("\x1b[2J\x1b[3;3H%s\x1b[29;24HA: continue", ask());
}

bool clearsave_update(u32 down)
{
	if (s_state == CS_ASK) {             // 0x447650 case 4: any button opens the save screen
		if (down & (KEY_A | KEY_B | KEY_START)) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			touchui_box(NULL, NULL, 0);
			if (s_host.files)
				savescreen_open(SS_CLEAR, s_host.files, 0);
			s_state = CS_FILES;
		}
		return true;
	}
	if (s_state == CS_FILES) {
		if (savescreen_update(down))
			return true;
		s_state = CS_OFF;
	}
	return false;
}

void clearsave_draw_top(void)
{
	if (s_state == CS_OFF || !font_ready())
		return;
	if (s_state == CS_ASK) {
		// message 0x1a in colour 4, centred at (256, 240)
		u32 yellow, dummy;
		font_pc_color(4, &yellow, &dummy);
		font_text(FONT_BIG, 256, 240, ask(), FONT_CENTER, yellow, yellow);
		return;
	}
	savescreen_draw_top();
}
