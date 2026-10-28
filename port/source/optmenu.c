#include "optmenu.h"
#include "profile.h"
#include "controls.h"
#include "events.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PG_MAIN, PG_GAME, PG_ADV, PG_CONTROL, PG_BRIGHT, PG_BUTTONS, PG_CHEATS };
enum { ST_OFF, ST_MENU, ST_ASK };
enum { K_PAGE, K_LIST, K_BAR, K_TIMES };

typedef struct {
	int kind;
	int label;                   // option.msg number, -1: name
	const char* name;
	int help;                    // option.msg number of the line in the box below, -1: text
	const char* text;
	int page;                    // K_PAGE: the page it opens
	u8* v;                       // the setting
	int lo, hi;
	bool wrap;
	const short* names;          // option.msg numbers of the values lo..hi, NULL: texts
	const char* const* texts;
} Row;

#define MAX_ROWS 13
#define VISIBLE 10               // the PC's panel holds ten rows (y 100 to 343)

static Options* s_live;
static Options s_old;            // as they were when the screen opened
static OptHost s_host;
static int s_state = ST_OFF, s_page, s_sel, s_top, s_bright_sel, s_yes = 1;
static Row s_rows[MAX_ROWS];
static int s_n;

static const short OFF_ON[2] = { 176, 177 };
static const short CONTROL_TYPE[2] = { 85, 86 };      // Rotational Style, Directional Style
static const short SPEAKER[3] = { 194, 195, 197 };    // Mono, Stereo, Surround
static const short FOG[2] = { 179, 180 };             // Simple, Complex
static const short NORMAL_SWITCH[2] = { 70, 47 };
static const short ZOOM[2] = { 95, 96 };
static const short WALK_RUN[3] = { 102, 104, 103 };   // Walk, Analog, Run
static const short BLOOD[4] = { 70, 50, 51, 52 };     // Normal, Green, Violet, Black
static const short LANGUAGE[5] = { 79, 81, 80, 82, 83 }; // English, French, German, Italian, Spanish (0x461600)
static const char* const SCREENS[2] = { "Bottom Screen", "Top Screen" };
static const char* const PC_PS2[2] = { "PC", "PS2" };
static const char* const FRAMING[2] = { "Full Width", "4:3" };
static const char* const RUN_BUTTON[2] = { "Hold", "Toggle" };
static const char* const CAMERA[2] = { "Original", "Behind" };
static const char* const TIMING[2] = { "PC", "Hold A" };
static const char* const SWAP[2] = { "Normal", "Swapped" };
static const char* const OFF_ON_TEXT[2] = { "Off", "On" };
// the hardware profile's rows (profile.c): three values each, Auto first
static const char* const HARDWARE[3] = { "Auto", "Old 3DS", "New 3DS" };
static const char* const QUALITY[3] = { "Auto", "Low", "High" };
static const char* const FRAMES[3] = { "Auto", "30 fps", "60 fps" };
#define OWN3(name, text, field, texts) add(K_LIST, -1, name, -1, text, &s_live->field, 0, 2, NULL, texts, 0)

static const char* msg(int n, const char* fallback)
{
	const char* m = ev_option_message(n);
	return m && *m ? m : fallback;
}

// ---------------------------------------------------------------- settings file

void opt_defaults(Options* o)
{
	memset(o, 0, sizeof *o);
	o->subtitles = 1;
	o->speaker = 1;
	o->bgm = o->se = 12;
	o->bright = 3;
	o->shadows = o->fog = 1;
	o->bullet = 1;
	o->ps2_flash = o->noise_ps2 = o->ps2_camera = o->cutscene_fov = 1;
	o->hold_stomp = o->swap_attack = 1;
	o->msg_top = 1;              // messages are the top screen's (port/UI.md)
	o->stereo = 1;               // 3D follows the slider
	o->control_type = 1;         // Directional Style: the stick points where James goes
	o->inf_stamina = 1;
	o->lang = (u8)ev_system_language();   // 0x4f6cc0 when there is no language.ini
}

typedef struct { const char* key; size_t off; u8 lo, hi; } Key;
#define K(name, field, lo, hi) { name, offsetof(Options, field), lo, hi }
static const Key s_keys[] = {
	K("controls", control_type, 0, 1), K("language", lang, 1, 5), K("subtitles", subtitles, 0, 1),
	K("speaker", speaker, 0, 2),
	K("bgmvol", bgm, 0, 15), K("sevol", se, 0, 15), K("brightness", bright, 0, 7), K("noise", noise, 0, 1),
	K("shadows", shadows, 0, 1), K("fog", fog, 0, 1), K("weaponcontrol", weapon, 0, 1),
	K("mapzoom", map_zoom, 0, 1), K("walkrun", walk_run, 0, 2), K("viewcontrol", view, 0, 1),
	K("bloodcolor", blood, 0, 3), K("bulletadjust", bullet, 1, 3), K("messagestop", msg_top, 0, 1),
	K("stereo3d", stereo, 0, 1),
	K("ps2flashlight", ps2_flash, 0, 1), K("ps2noise", noise_ps2, 0, 1), K("ps2camera", ps2_camera, 0, 1),
	K("cutscenefov", cutscene_fov, 0, 1), K("togglerun", toggle_run, 0, 1), K("holdstomp", hold_stomp, 0, 1),
	K("swapattack", swap_attack, 0, 1), K("hwprofile", hwprofile, 0, 2), K("quality", quality, 0, 2),
	K("framerate", framerate, 0, 2), K("ctrllayout", ctrllayout, 0, 2), K("infstamina", inf_stamina, 0, 1),
	K("infammo", inf_ammo, 0, 1), K("camera", camera, 0, 1),
};

// options.txt files written before these defaults (no "optver" line) take the new defaults of the
// settings that changed: Directional Style and messages on the top screen.
#define OPT_VERSION 2

void opt_load(Options* o, const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f)
		return;
	char key[32];
	int val, music = -1, sound = -1, version = 1;
	bool bgm = false, se = false;
	while (fscanf(f, "%31s %d", key, &val) == 2) {
		if (!strcmp(key, "optver"))
			version = val;
		for (size_t i = 0; i < sizeof s_keys / sizeof *s_keys; i++)
			if (!strcmp(key, s_keys[i].key))
				((u8*)o)[s_keys[i].off] = val < s_keys[i].lo ? s_keys[i].lo : val > s_keys[i].hi ? s_keys[i].hi : val;
		bgm |= !strcmp(key, "bgmvol"), se |= !strcmp(key, "sevol");
		if (!strcmp(key, "musicvol")) music = val;          // the earlier 0..10 volumes
		if (!strcmp(key, "soundvol")) sound = val;
	}
	fclose(f);
	if (version < OPT_VERSION) {
		Options d;
		opt_defaults(&d);
		o->control_type = d.control_type;
		o->msg_top = d.msg_top;
	}
	if (!bgm && music >= 0)
		o->bgm = music > 10 ? 15 : (music * 15 + 5) / 10;
	if (!se && sound >= 0)
		o->se = sound > 10 ? 15 : (sound * 15 + 5) / 10;
}

void opt_save(const Options* o, const char* path)
{
	FILE* f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "optver %d\n", OPT_VERSION);
	for (size_t i = 0; i < sizeof s_keys / sizeof *s_keys; i++)
		fprintf(f, "%s %d\n", s_keys[i].key, ((const u8*)o)[s_keys[i].off]);
	fclose(f);
}

// ---------------------------------------------------------------- pages

static void add(int kind, int label, const char* name, int help, const char* text, u8* v, int lo, int hi,
                const short* names, const char* const* texts, int page)
{
	if (s_n >= MAX_ROWS)
		return;
	s_rows[s_n++] = (Row){ kind, label, name, help, text, page, v, lo, hi, kind != K_BAR && v != &s_live->bright,
	                       names, texts };
}

#define LIST(label, help, field, n, names) add(K_LIST, label, NULL, help, NULL, &s_live->field, 0, (n) - 1, names, NULL, 0)
#define OWN(name, text, field, texts) add(K_LIST, -1, name, -1, text, &s_live->field, 0, 1, NULL, texts, 0)

static void build(void)
{
	s_n = 0;
	switch (s_page) {
	case PG_MAIN:
		// 0x461600 page 2: three pages, then the settings (Vibration left out)
		add(K_PAGE, 189, NULL, 190, NULL, NULL, 0, 0, NULL, NULL, PG_GAME);
		add(K_PAGE, 166, NULL, 191, NULL, NULL, 0, 0, NULL, NULL, PG_ADV);
		add(K_PAGE, 6, NULL, 7, NULL, NULL, 0, 0, NULL, NULL, PG_CONTROL);
		add(K_PAGE, -1, "Cheats", -1, "Infinite stamina, infinite ammo", NULL, 0, 0, NULL, NULL, PG_CHEATS);
		LIST(84, 87, control_type, 2, CONTROL_TYPE);
		// Language (76, help 77): English to Spanish and round again either way (0x4630c0 row 5)
		add(K_LIST, 76, NULL, 77, NULL, &s_live->lang, 1, 5, LANGUAGE, NULL, 0);
		LIST(74, 75, subtitles, 2, OFF_ON);
		LIST(34, 35, speaker, 3, SPEAKER);
		add(K_BAR, 40, NULL, 41, NULL, &s_live->bgm, 0, 15, NULL, NULL, 0);
		add(K_BAR, 42, NULL, 43, NULL, &s_live->se, 0, 15, NULL, NULL, 0);
		break;
	case PG_GAME:
		// 0x461600 with 0x944201 set: the extra gameplay options; Bullet Adjust once cleared
		LIST(44, 45, weapon, 2, NORMAL_SWITCH);
		LIST(94, 99, map_zoom, 2, ZOOM);
		LIST(57, 58, walk_run, 3, WALK_RUN);
		LIST(53, 54, view, 2, NORMAL_SWITCH);
		LIST(48, 49, blood, 4, BLOOD);
		if (s_host.cleared)
			add(K_TIMES, 65, NULL, 66, NULL, &s_live->bullet, 1, 3, NULL, NULL, 0);
		break;
	case PG_ADV:
		// 0x464eb0: Brightness Level, Noise Effect, Shadows and Fog apply here (Screen Position,
		// the textures, resolution, filters, lens flare and 3D sound are the PC's own)
		add(K_PAGE, 2, NULL, 3, NULL, NULL, 0, 0, NULL, NULL, PG_BRIGHT);
		LIST(97, 98, noise, 2, OFF_ON);
		LIST(175, 203, shadows, 2, OFF_ON);
		LIST(178, 204, fog, 2, FOG);
		OWN("Messages", "Show messages and subtitles on the top screen", msg_top, SCREENS);
		OWN("3D", "Depth on the top screen with the 3D slider", stereo, OFF_ON_TEXT);
		OWN("Flashlight", "Enhanced Edition: the PS2's softer flashlight", ps2_flash, PC_PS2);
		OWN("Noise Style", "Enhanced Edition: the PS2's darker grain", noise_ps2, PC_PS2);
		OWN("Camera Speed", "Enhanced Edition: the PS2's camera speed", ps2_camera, PC_PS2);
		OWN("Cutscene View", "Enhanced Edition: keep the 4:3 framing in scenes", cutscene_fov, FRAMING);
		// the hardware profile (profile.c): what Auto means on this console is in the help line
		{
			static char hw[80], q[80], fr[80];
			bool n = profile_console_new();
			snprintf(hw, sizeof hw, "Auto: this console (%s 3DS)", n ? "New" : "Old");
			snprintf(q, sizeof q, "Low: lighter scenes, no shadows (Auto: %s)",
			         profile_new() ? "High" : "Low");
			snprintf(fr, sizeof fr, "30 fps keeps frames even (Auto: %d fps)", profile_frame_rate());
			OWN3("Hardware", hw, hwprofile, HARDWARE);
			OWN3("Quality", q, quality, QUALITY);
			OWN3("Frame Rate", fr, framerate, FRAMES);
		}
		break;
	case PG_CONTROL:
		// the PC's key setup becomes the port's: the buttons of each action (controls.c), then the
		// control settings
		add(K_PAGE, -1, "Buttons", -1, "Choose the button for each action", NULL, 0, 0, NULL, NULL, PG_BUTTONS);
		OWN("Camera", "Behind: a camera that follows James", camera, CAMERA);
		OWN("Run Button", "Enhanced Edition: B starts and stops running", toggle_run, RUN_BUTTON);
		OWN("Heavy Attack", "Enhanced Edition: hold A to stomp or swing heavy", hold_stomp, TIMING);
		OWN("Light / Heavy", "Enhanced Edition: swap the light and heavy swings", swap_attack, SWAP);
		// which buttons the bindings may use: the Old 3DS layout has no ZL, ZR or C-Stick
		{
			static char lay[80];
			snprintf(lay, sizeof lay, "Old 3DS: no ZL, ZR or C-Stick (Auto: %s 3DS)", profile_console_new() ? "New" : "Old");
			OWN3("Layout", lay, ctrllayout, HARDWARE);
		}
		break;
	case PG_CHEATS:
		// the port's own: not the PC's (its cheats are the clear rewards), off but for stamina
		OWN("Infinite Stamina", "James can run for as long as B is held", inf_stamina, OFF_ON_TEXT);
		OWN("Infinite Ammo", "Guns never run out of rounds", inf_ammo, OFF_ON_TEXT);
		break;
	}
}

static const char* label(const Row* r) { return r->label >= 0 ? msg(r->label, "") : r->name; }
static const char* help(const Row* r) { return r->help >= 0 ? msg(r->help, "") : r->text; }

static const char* value(const Row* r, char* buf, int size)
{
	int v = *r->v;
	if (r->kind == K_TIMES) {
		// "x \1" (67) with the number inserted
		const char* m = msg(67, "x \x01");
		int k = 0;
		for (; *m && k < size - 4; m++)
			k += *m == 1 ? snprintf(buf + k, size - k, "%d", v) : (buf[k] = *m, 1);
		buf[k] = 0;
		return buf;
	}
	if (r->kind == K_BAR) {
		snprintf(buf, size, "%d", v);
		return buf;
	}
	return r->names ? msg(r->names[v - r->lo], "") : r->texts[v - r->lo];
}

static const char* page_title(int page)
{
	switch (page) {
	case PG_GAME: return msg(189, "Game Options");
	case PG_ADV: return msg(166, "Advanced Options");
	case PG_CONTROL: return msg(6, "Control Options");
	case PG_BRIGHT: return msg(2, "Brightness Level");
	case PG_BUTTONS: return "Buttons";
	case PG_CHEATS: return "Cheats";
	}
	return ev_language() == 1 ? "Options" : msg(23, "Options");   // "Option" in English
}

static int s_touch_answer;          // the question's row on the touch screen: 0 Yes, 1 No

static void touch(void)
{
	static const TouiButton back[1] = { { "Back", KEY_B } };
	touchui_box(NULL, NULL, 0);
	if (s_state == ST_ASK) {
		// the answers act on the first tap (port/UI.md)
		static const TouiButton prompts[2] = { { "Select", KEY_A }, { "Back", KEY_B } };
		const char* rows[2] = { msg(90, "Yes."), msg(91, "No.") };
		s_touch_answer = s_yes ? 0 : 1;
		touchui_menu_ex(TOUI_OPTIONS, msg(89, "Change the settings?"), rows, 2, &s_touch_answer, prompts, 2, 0,
		                TOUI_ONE_TAP);
		return;
	}
	static char text[MAX_ROWS][64];
	const char* rows[MAX_ROWS];
	if (s_page == PG_BRIGHT) {
		snprintf(text[0], sizeof text[0], "Level   < %d >", s_live->bright);
		rows[0] = text[0];
		s_bright_sel = 0;
		touchui_menu(TOUI_OPTIONS, page_title(PG_BRIGHT), rows, 1, &s_bright_sel, back, 1);
		return;
	}
	for (int i = 0; i < s_n; i++) {
		char v[24];
		if (s_rows[i].kind == K_PAGE)
			snprintf(text[i], sizeof text[i], "%s", label(&s_rows[i]));
		else
			snprintf(text[i], sizeof text[i], "%s   < %s >", label(&s_rows[i]), value(&s_rows[i], v, sizeof v));
		rows[i] = text[i];
	}
	touchui_menu(TOUI_OPTIONS, page_title(s_page), rows, s_n, &s_sel, back, 1);
}

static void go(int page, int sel)
{
	s_page = page;
	if (page == PG_BUTTONS) {
		// controls.c's Buttons page draws the bottom screen itself, in this screen's menu slot
		touchui_box(NULL, NULL, 0);
		controls_menu_slot(TOUI_OPTIONS);
		controls_menu_open();
		controls_menu_draw();
		return;
	}
	build();
	s_sel = sel < s_n ? sel : 0;
	s_top = 0;
	touch();
}

void optmenu_open(Options* live, const OptHost* host)
{
	s_live = live;
	s_old = *live;
	s_host = *host;
	s_state = ST_MENU;
	go(PG_MAIN, 0);
}

bool optmenu_active(void) { return s_state != ST_OFF; }
bool optmenu_brightness(void) { return s_state == ST_MENU && s_page == PG_BRIGHT; }

static void close_screen(void)
{
	s_state = ST_OFF;
	touchui_menu_close(TOUI_OPTIONS);
	touchui_box(NULL, NULL, 0);
}

static void changed(void)
{
	if (s_host.apply)
		s_host.apply(s_live);
	sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);       // 10000, as the PC on every change
}

static void step(Row* r, int dir)
{
	int v = *r->v;
	for (int k = 0; k <= r->hi - r->lo; k++) {
		v += dir;
		if (v > r->hi)
			v = r->wrap ? r->lo : r->hi;
		if (v < r->lo)
			v = r->wrap ? r->hi : r->lo;
		// the Language row passes over the languages the pack lacks (the PC has them all)
		if (r->v != &s_live->lang || ev_language_available(v))
			break;
	}
	if (v != *r->v) {
		*r->v = (u8)v;
		changed();
	}
}

bool optmenu_update(u32 down)
{
	if (s_state == ST_OFF)
		return false;
	if (s_state == ST_ASK) {
		// "Change the settings?": Yes keeps them, No puts the old ones back (0x4630c0); B goes back
		// to the menu
		if ((s_touch_answer == 0) != (s_yes == 1) && (s_touch_answer == 0 || s_touch_answer == 1))
			s_yes = s_touch_answer == 0;
		if (down & (KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN)) {
			s_yes ^= 1;
			sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
			touch();
		}
		int pick = down & KEY_A ? s_yes : -1;
		if (pick == 1) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			if (s_host.save)
				s_host.save(s_live);
			close_screen();
			return false;
		}
		if (pick == 0) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			*s_live = s_old;
			if (s_host.apply)
				s_host.apply(s_live);
			close_screen();
			return false;
		}
		if (down & KEY_B) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			s_state = ST_MENU;
			touch();
		}
		return true;
	}
	if (s_page == PG_BUTTONS) {
		if (controls_menu_update(down))
			controls_menu_draw();
		else
			go(PG_CONTROL, 0);
		return true;
	}
	if (s_page == PG_BRIGHT) {
		// 0x466c30: left and right from 0 to 7, back to the Advanced Options with either button
		int v = s_live->bright + (down & KEY_DRIGHT ? 1 : 0) - (down & KEY_DLEFT ? 1 : 0);
		if (v >= 0 && v <= 7 && v != s_live->bright) {
			s_live->bright = (u8)v;
			changed();
			touch();
		}
		if (down & (KEY_A | KEY_B)) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			go(PG_ADV, 0);
		}
		return true;
	}
	bool redraw = false;
	if (down & (KEY_DUP | KEY_DDOWN)) {
		s_sel = (s_sel + (down & KEY_DDOWN ? 1 : s_n - 1)) % s_n;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		redraw = true;
	}
	Row* r = &s_rows[s_sel];
	if (r->kind == K_PAGE) {
		if (down & KEY_A) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			go(r->page, 0);
			return true;
		}
	} else if (down & (KEY_DLEFT | KEY_DRIGHT | KEY_A)) {
		step(r, down & KEY_DLEFT ? -1 : 1);
		redraw = true;
	}
	if (down & KEY_B) {
		if (s_page != PG_MAIN) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			go(PG_MAIN, s_page == PG_GAME ? 0 : s_page == PG_ADV ? 1 : s_page == PG_CONTROL ? 2 : 3);
			return true;
		}
		if (memcmp(s_live, &s_old, sizeof s_old)) {
			s_state = ST_ASK;
			s_yes = 1;
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			touch();
			return true;
		}
		sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		close_screen();
		return false;
	}
	if (s_sel < s_top)
		s_top = s_sel;
	if (s_sel >= s_top + VISIBLE)
		s_top = s_sel - VISIBLE + 1;
	if (redraw)
		touch();
	return true;
}

// ---------------------------------------------------------------- the top screen

static MapDrawFn s_draw;

// The PC's text space (x 0..512, y 16..496, the 4:3 picture at 40..360 on the top screen).
static float sx(float x) { return 200.0f + (x - 256.0f) * 0.625f; }
static float sy(float y) { return 120.0f + (y - 256.0f) * 0.46875f; }

static void quad(float x0, float y0, float x1, float y1, u32 rgba)
{
	MapVert a = { sx(x0), sy(y0), 0, 0 }, b = { sx(x1), sy(y0), 0, 0 };
	MapVert c = { sx(x1), sy(y1), 0, 0 }, d = { sx(x0), sy(y1), 0, 0 };
	MapVert v[6] = { a, b, c, a, c, d };
	s_draw(NULL, v, 6, rgba);
}

// a convex outline given as fractions of the screen (0x45e750's vertices: W * (0.5 + x))
static void poly(const float (*p)[2], int n, u32 fill, u32 line)
{
	MapVert v[3 * 8];
	int k = 0;
	for (int i = 1; i + 1 < n && k + 3 <= 24; i++) {
		const float* q[3] = { p[0], p[i], p[i + 1] };
		for (int j = 0; j < 3; j++)
			v[k++] = (MapVert){ sx(256.0f + 512.0f * q[j][0]), sy(256.0f + 480.0f * q[j][1]), 0, 0 };
	}
	if (fill)
		s_draw(NULL, v, k, fill);
	if (!line)
		return;
	for (int i = 0; i < n; i++) {
		// one-pixel edges as thin quads
		float x0 = sx(256.0f + 512.0f * p[i][0]), y0 = sy(256.0f + 480.0f * p[i][1]);
		float x1 = sx(256.0f + 512.0f * p[(i + 1) % n][0]), y1 = sy(256.0f + 480.0f * p[(i + 1) % n][1]);
		float dx = x1 - x0, dy = y1 - y0, len = dx * dx + dy * dy;
		if (len <= 0.0f)
			continue;
		float inv = 0.5f / __builtin_sqrtf(len), nx = -dy * inv, ny = dx * inv;
		MapVert a = { x0 + nx, y0 + ny, 0, 0 }, b = { x1 + nx, y1 + ny, 0, 0 };
		MapVert c = { x1 - nx, y1 - ny, 0, 0 }, d = { x0 - nx, y0 - ny, 0, 0 };
		MapVert e[6] = { a, b, c, a, c, d };
		s_draw(NULL, e, 6, line);
	}
}

// 0x45e750: the panel, the help box below it and the outline around both
static void panels(void)
{
	static const float panel[6][2] = {
		{ -0.41015625f, -0.34375f }, { 0.41015625f, -0.34375f }, { 0.443359375f, -0.3020833f },
		{ 0.443359375f, 0.2666667f }, { -0.443359375f, 0.2666667f }, { -0.443359375f, -0.3020833f },
	};
	static const float box[6][2] = {
		{ -0.443359375f, 0.2791667f }, { 0.443359375f, 0.2791667f }, { 0.443359375f, 0.3291667f },
		{ 0.41015625f, 0.3708333f }, { -0.41015625f, 0.3708333f }, { -0.443359375f, 0.3291667f },
	};
	static const float outline[8][2] = {
		{ -0.4140625f, -0.3541667f }, { 0.4140625f, -0.3541667f }, { 0.453125f, -0.30625f },
		{ 0.453125f, 0.3333333f }, { 0.4140625f, 0.38125f }, { -0.4140625f, 0.38125f },
		{ -0.453125f, 0.3333333f }, { -0.453125f, -0.30625f },
	};
	// D3D colours 0x40808080 (fill) and 0x60252550 (lines) as RGBA
	poly(panel, 6, 0x80808040, 0x25255060);
	poly(box, 6, 0x80808040, 0x25255060);
	poly(outline, 8, 0, 0x25255060);
}

// Text colours: 0x47edb0 takes PS2-style values (0x40 full), here doubled; alpha 0x60 for the
// rows, full for the chosen one, which is drawn again 2 up and 2 left (0x461600).
#define GREY 0x7e7e7e00u
#define YELLOW 0x7e7e3e00u
static void text(float x, float y, const char* s, int align, u32 rgb, bool chosen)
{
	font_text(FONT_BIG, x, y, s, align, rgb | 0xc0, rgb | 0xc0);
	if (chosen)
		font_text(FONT_BIG, x - 2, y - 2, s, align, rgb | 0xff, rgb | 0xff);
}

static void arrow(float x, float y, int dir, u32 rgba)
{
	// the value's arrows (0x460c60 / 0x4610e0) as small triangles, tip outwards
	float t = x + 8.0f * dir;
	MapVert v[3] = { { sx(x), sy(y - 7), 0, 0 }, { sx(t), sy(y), 0, 0 }, { sx(x), sy(y + 7), 0, 0 } };
	s_draw(NULL, v, 3, rgba);
}

// A title from option.msg when the pack carries its full-width codes (packs from before the other
// languages have '?' there), NULL otherwise.
static const char* full_width_msg(int n)
{
	const char* m = ev_option_message(n);
	return m && strchr(m, FONT_FULL_WIDTH) ? m : NULL;
}

static void title(void)
{
	// the full-width titles: option_msg 72 OPTIONS, 73 GAME OPTIONS, 167 ADVANCED OPTIONS
	// (0x461600 / 0x464eb0 at 0x100, 0x34)
	bool control = s_page == PG_CONTROL || s_page == PG_BUTTONS;
	const char* t = full_width_msg(s_page == PG_GAME ? 73 : s_page == PG_ADV ? 167 : 72);
	if (control && ev_language() != 1) {
		// the PC's Control Options screen (0x467440) is its own; the port's page is titled with
		// the row's name in the other languages (a stand-in)
		font_text(FONT_BIG, 256, 52, msg(6, "Control Options"), FONT_CENTER, 0x808080ff, 0x808080ff);
		return;
	}
	if (t && !control) {
		font_text(FONT_BIG, 256, 52, t, FONT_CENTER, 0x808080ff, 0x808080ff);
		return;
	}
	// the English codes (option_msg_e.mes 72, 73, 167; 0xfc03 moves back 3, 0xfffe a space), with
	// CONTROL in the same letters for the port's Control Options page
	static const u16 opt[] = { 0x18a, 0x18b, 0x18f, 0xfc03, 0x184, 0xfc03, 0x18a, 0x189, 0x18e };
	static const u16 game[] = { 0x182, 0x17c, 0x188, 0x180, 0xfffe };
	static const u16 adv[] = { 0x17c, 0x17f, 0x191, 0x17c, 0x189, 0x17e, 0x180, 0x17f, 0xfffe };
	static const u16 ctl[] = { 0x17e, 0x18a, 0x189, 0x18f, 0x18d, 0x18a, 0x187, 0xfffe };
	u16 line[24];
	int n = 0;
	const u16* head = s_page == PG_GAME ? game : s_page == PG_ADV ? adv : control ? ctl : NULL;
	int hn = s_page == PG_GAME ? 5 : s_page == PG_ADV ? 9 : control ? 8 : 0;
	for (int i = 0; i < hn; i++)
		line[n++] = head[i];
	for (int i = 0; i < 9; i++)
		line[n++] = opt[i];
	font_codes(FONT_BIG, 256, 52, line, n, FONT_CENTER, 0x808080ff, 0x808080ff);
}

static void bar(float x0, float y, int v, bool changed)
{
	// 0x45bf10: fifteen blocks from x 273 (x0 256), 11 apart, lit up to the volume, two-tone
	for (int i = 0; i < 15; i++) {
		float x = x0 + 17.0f + 11.0f * i;
		bool lit = i < v;
		u32 top = lit ? (changed ? 0xb0b05880 : 0xb0b0b080) : (changed ? 0x50502580 : 0x50505080);
		u32 bottom = lit ? (changed ? 0x40402080 : 0x40404080) : (changed ? 0x50502580 : 0x50505080);
		quad(x - 2, y + 5, x + 7, y + 16, top);
		quad(x - 2, y + 16, x + 7, y + 28, bottom);
	}
}

static void brightness_screen(void)
{
	// 0x466c30: twenty vertical lines whose alpha grows with the level, "Adjust until vertical
	// lines appear faint." and "Level n" in full-width letters
	for (int k = -190; k < 0xd2; k += 20) {
		MapVert a = { sx(256.0f + k), sy(176), 0, 0 }, b = { sx(256.0f + k) + 1.0f, sy(176), 0, 0 };
		MapVert c = { sx(256.0f + k) + 1.0f, sy(406), 0, 0 }, d = { sx(256.0f + k), sy(406), 0, 0 };
		MapVert v[6] = { a, b, c, a, c, d };
		s_draw(NULL, v, 6, 0x80808000u | (u32)(s_live->bright * 12));
	}
	bool ch = s_live->bright != s_old.bright;
	font_text(FONT_BIG, 25, 100, msg(4, "Adjust until vertical lines appear faint."), FONT_LEFT, 0x7e7e7eff, 0x7e7e7eff);
	u32 c = ch ? 0x7e7e3eff : 0x7e7e7eff;
	const char* m = full_width_msg(5);   // "Level \1" in full-width letters, the level 0x172 + n
	if (m) {
		char line[160];
		int k = 0;
		for (; *m && k < (int)sizeof line - 6; m++)
			k += *m == 1 ? snprintf(line + k, sizeof line - k, "%c%04x", FONT_FULL_WIDTH, 0x172 + s_live->bright)
			             : (line[k] = *m, 1);
		line[k] = 0;
		font_text(FONT_BIG, 316, 420, line, FONT_RIGHT, c, c);
	} else {
		u16 level[] = { 0x187, 0xfc02, 0x19a, 0xfc04, 0x1ab, 0xfc04, 0x19a, 0xfc06, 0x1a1, 0xfc04, 0xfffe,
		                (u16)(0x172 + s_live->bright) };
		font_codes(FONT_BIG, 316, 420, level, sizeof level / sizeof *level, FONT_RIGHT, c, c);
	}
	arrow(290, 436, -1, 0x7e7e7eff);         // either side of the number (0x460c60 / 0x4610e0)
	arrow(322, 436, 1, 0x7e7e7eff);
}

// The PC's panel and title stay; the rows are on the touch screen (port/UI.md), so the panel shows
// the chosen one large: its name, its value between the PC's arrows (or the volume blocks), and
// the PC's help line in the box below; the question in the middle while it is asked.
void optmenu_draw_top(MapDrawFn draw)
{
	if (s_state == ST_OFF || !font_ready() || !draw)
		return;
	s_draw = draw;
	if (optmenu_brightness()) {
		brightness_screen();
		return;
	}
	panels();
	title();
	if (s_page == PG_BUTTONS) {
		const char* name;
		char keys[48];
		bool wait = controls_menu_focus(&name, keys, sizeof keys);
		text(256, 190, name, FONT_CENTER, GREY, true);
		text(256, 240, wait ? "Press a button" : keys, FONT_CENTER, wait ? YELLOW : GREY, false);
		font_text(FONT_BIG, 70, 395, wait ? "START keeps the button it had" : "Choose the button for each action",
		          FONT_LEFT, 0x7e7e7eff, 0x7e7e7eff);
		return;
	}
	if (s_state == ST_ASK) {
		// 0x461600 with 0x94421c set: the question; Yes. and No. are on the touch screen
		text(256, 215, msg(89, "Change the settings?"), FONT_CENTER, GREY, true);
		return;
	}
	if (s_sel >= s_n)
		return;
	const Row* r = &s_rows[s_sel];
	if (r->kind == K_PAGE) {
		text(256, 215, label(r), FONT_CENTER, GREY, true);
	} else {
		text(256, 190, label(r), FONT_CENTER, GREY, true);
		size_t off = r->v - (u8*)s_live;
		bool ch = ((u8*)&s_old)[off] != *r->v;
		if (r->kind == K_BAR) {
			bar(174.0f - 273.0f + 256.0f, 236, *r->v, ch);
		} else {
			char buf[24];
			const char* v = value(r, buf, sizeof buf);
			float w = font_width(FONT_BIG, v);
			text(256, 236, v, FONT_CENTER, ch ? YELLOW : GREY, false);
			arrow(256 - w * 0.5f - 14, 250, -1, 0x7e7e7eff);
			arrow(256 + w * 0.5f + 12, 250, 1, 0x7e7e7eff);
		}
	}
	font_text(FONT_BIG, 70, 395, help(r), FONT_LEFT, 0x7e7e7eff, 0x7e7e7eff);
}
