#include "optmenu.h"
#include "profile.h"
#include "controls.h"
#include "events.h"
#include "figures.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAB_CONTROLS, TAB_GAME, TAB_DISPLAY, TAB_SOUND, TAB_SYSTEM, TAB_N };
enum { PG_TABS, PG_BRIGHT, PG_BUTTONS };   // the tabs, or the screen a row opens
enum { ST_OFF, ST_MENU, ST_ASK };
enum { K_LIST, K_BAR, K_PAGE, K_TIMES };

// A setting: where it is, its field and values, and what the top screen says about it. Text the
// port writes may name buttons and the console with {AIM}, {RUN}, {ACT}, {LOOK} (the bound
// button), {CAMTURN} (what turns the behind camera), {AUTOQ}, {AUTOFPS} and {CONSOLE} (expand).
typedef struct {
	int tab, kind;
	int label;                       // option.msg number of the name, -1: name
	const char* name;
	size_t off;                      // the field (rows with get and set stand for two)
	int lo, hi;
	const short* names;              // option.msg numbers of the values lo..hi, -1: texts[i]
	const char* const* texts;        // the port's names of the values
	int (*get)(const Options*);
	void (*set)(Options*, int);
	int page;                        // K_PAGE: PG_BRIGHT or PG_BUTTONS
	int fig;                         // figures.h, beside a setting of two values
	bool swatch;                     // Blood Color: its colours
	const char* what;                // what the setting does
	const char* const* lines;        // what each value does (lo..hi); NULL: the names alone
	bool (*dim)(const Options*);     // another setting overrides it now
	const char* (*note)(const Options*);
	bool (*shown)(void);
} Setting;

#define MAX_ROWS 10

static Options* s_live;
static Options s_old;            // as they were when the screen opened
static OptHost s_host;
static int s_state = ST_OFF, s_page, s_tab, s_sel, s_shown_tab, s_shown_sel, s_bright_sel, s_yes = 1;
static int s_tab_sel[TAB_N];     // the row last chosen on each tab (kept for the next opening)
static const Setting* s_rows[MAX_ROWS];
static int s_n;
static u64 s_focus_ms;           // when the chosen row came up: its drawings start from there
static int s_force_layout = -1;  // textcheck: the buttons of this layout in the texts

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
	o->lens_flare = 1;           // 0x4f6ae0
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
	K("shadows", shadows, 0, 1), K("fog", fog, 0, 1), K("lensflare", lens_flare, 0, 1),
	K("weaponcontrol", weapon, 0, 1),
	K("mapzoom", map_zoom, 0, 1), K("walkrun", walk_run, 0, 2), K("viewcontrol", view, 0, 1),
	K("bloodcolor", blood, 0, 3), K("bulletadjust", bullet, 1, 3), K("messagestop", msg_top, 0, 1),
	K("stereo3d", stereo, 0, 1), K("picture", picture, 0, 1),
	K("ps2flashlight", ps2_flash, 0, 1), K("ps2noise", noise_ps2, 0, 1), K("ps2camera", ps2_camera, 0, 1),
	K("cutscenefov", cutscene_fov, 0, 1), K("togglerun", toggle_run, 0, 1), K("holdstomp", hold_stomp, 0, 1),
	K("swapattack", swap_attack, 0, 1), K("hwprofile", hwprofile, 0, 2), K("quality", quality, 0, 2),
	K("framerate", framerate, 0, 2), K("ctrllayout", ctrllayout, 0, 2), K("infstamina", inf_stamina, 0, 1),
	K("infammo", inf_ammo, 0, 1), K("camera", camera, 0, 1), K("setup", setup, 0, 1),
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

// ---------------------------------------------------------------- the settings

static const short OFF_ON[2] = { 176, 177 };
static const short CONTROL_TYPE[2] = { 85, 86 };      // Rotational Style, Directional Style
static const short SPEAKER[3] = { 194, 195, 197 };    // Mono, Stereo, Surround
static const short FOG[2] = { 179, 180 };             // Simple, Complex
static const short NORMAL_SWITCH[2] = { 70, 47 };
static const short ZOOM[2] = { 95, 96 };
static const short WALK_RUN[4] = { 102, -1, 104, 103 };   // Walk, (Toggle), Analog, Run
static const short BLOOD[4] = { 70, 50, 51, 52 };     // Normal, Green, Violet, Black
static const short LANGUAGE[5] = { 79, 81, 80, 82, 83 }; // English, French, German, Italian, Spanish (0x461600)
static const short NOISE[3] = { 176, -1, -1 };        // Off, (PC, PS2)
static const char* const WALK_RUN_T[4] = { NULL, "Toggle", NULL, NULL };
static const char* const NOISE_T[3] = { NULL, "PC", "PS2" };
static const char* const SCREENS[2] = { "Bottom Screen", "Top Screen" };
static const char* const PC_PS2[2] = { "PC", "PS2" };
static const char* const PICTURE[2] = { "Soft", "Sharp" };
static const char* const FRAMING[2] = { "Full Width", "4:3" };
static const char* const CAMERA[2] = { "Original", "Behind" };
static const char* const HEAVY[2] = { "PC", "Hold {ACT}" };
static const char* const OFF_ON_TEXT[2] = { "Off", "On" };
// the hardware profile's rows (profile.c): three values each, Auto first
static const char* const HARDWARE[3] = { "Auto", "Old 3DS", "New 3DS" };
static const char* const QUALITY[3] = { "Auto", "Low", "High" };
static const char* const FRAMES[3] = { "Auto", "30 fps", "60 fps" };

static const char* const CAMERA_L[2] = { "The PC's own shots, framed for each room. They cut as James moves.",
                                         "Follows James. {CAMTURN}" };
static const char* const CONTROL_L[2] = { "Left and right turn James. Up walks ahead, down backs away.",
                                          "Push the way you want James to go. It holds through camera cuts." };
static const char* const CAMSPEED_L[2] = { "The PC's slower turns, which float and bounce.",
                                           "Quicker, steadier turns, as on the PS2." };
static const char* const WEAPON_L[2] = { "Hold {AIM} to keep the weapon up. Let go to lower it.",
                                         "Press {AIM} to raise the weapon, and again to lower it." };
static const char* const WALKRUN_L[4] = { "James walks. Hold {RUN} to run.", "Press {RUN} to run, until James stops.",
                                          "Push the Circle Pad all the way to run.", "James runs. Hold {RUN} to walk." };
static const char* const VIEW_L[2] = { "Hold {LOOK} to look ahead of James.",
                                       "Press {LOOK} to look ahead of James, and again to stop." };
static const char* const HEAVY_L[2] = { "Every press is heavy, as with a PC key: a downswing or a stomp.",
                                        "Tap {ACT} to swing light or kick. Hold it to swing heavy or stomp." };
static const char* const MAP_L[2] = { "Close up on where James is.", "The whole floor at once." };
static const char* const BULLET_L[3] = { "As on the PC.", "Twice the rounds.", "Three times the rounds." };
static const char* const STAMINA_L[2] = { "As on the PC: a long run tires James, sooner when he is hurt.",
                                          "James runs for as long as you like." };
static const char* const AMMO_L[2] = { "Every shot uses a round.", "Guns never run out." };
static const char* const STEREO_L[2] = { "The top screen stays flat whatever the slider says.",
                                         "The world gains depth. Text and menus stay on the screen." };
static const char* const PICTURE_L[2] = { "Edges and texture grain smoothed, as the PC looks at this size.",
                                          "Every pixel as the console draws it, crisper and grainier." };
static const char* const MESSAGES_L[2] = { "On the touch screen, which keeps the picture clear.",
                                           "In the PC's message window over the game." };
static const char* const NOISE_L[3] = { "A clean picture, as the PC starts.", "The PC's light grain.",
                                        "The PS2's darker grain, which the Enhanced Edition restores." };
static const char* const FLASH_L[2] = { "A strong, short beam that washes walls out.",
                                        "A softer beam that reaches further. Dark rooms stay dark." };
static const char* const CUTFOV_L[2] = { "Scenes widen to the screen and show more at the sides.",
                                         "Scenes keep the width they were framed with, a little closer." };
static const char* const SHADOWS_L[2] = { "No character shadows. Lighter for the console.",
                                          "Characters cast shadows, softened as in the Enhanced Edition." };
static const char* const FOG_L[2] = { "Fog without drifting particles. Lighter for the console.",
                                      "Drifting fog particles, as on the PC." };
static const char* const FLARE_L[2] = { "Lamps show no glow.",
                                        "A lamp facing the camera glows and throws streaks, as on the PC." };
static const char* const SUBS_L[2] = { "Voices only.", "Spoken lines are written out as they play." };
static const char* const SPEAKER_L[3] = { "One channel from both speakers.", "Left and right kept apart.",
                                          "The 3DS's virtual surround." };
static const char* const QUALITY_L[3] = { "This console picks {AUTOQ}.",
                                          "Fewer fog particles and lights, no shadows, no supersampling.",
                                          "Everything the port draws, supersampled while the frame rate holds." };
static const char* const FRAMES_L[3] = { "Follows Quality: {AUTOFPS} fps on this console.",
                                         "The steadiest motion, and the most room for supersampling.",
                                         "Smoother, when the console keeps up." };
static const char* const HARDWARE_L[3] = { "This console, a {CONSOLE} 3DS.",
                                           "Lighter defaults for quality, frame rate and textures.",
                                           "Full defaults for quality, frame rate and textures." };
static const char* const LAYOUT_L[3] = { "This console's buttons.", "No ZL, ZR or C-Stick.",
                                         "Adds ZL, ZR and the C-Stick, or a Circle Pad Pro." };

// Walk/Run Control and the Enhanced Edition's toggle in one row: Walk, Toggle, Analog, Run. The
// toggle wins over the other two in main.c, so they are never set together here.
static int walkrun_get(const Options* o) { return o->toggle_run ? 1 : o->walk_run == 0 ? 0 : o->walk_run == 1 ? 2 : 3; }
static void walkrun_set(Options* o, int v)
{
	o->toggle_run = v == 1;
	o->walk_run = v == 2 ? 1 : v == 3 ? 2 : 0;
}
// Noise Effect and the Enhanced Edition's PS2 style in one row: Off, PC, PS2 (Off keeps the style).
static int noise_get(const Options* o) { return !o->noise ? 0 : o->noise_ps2 ? 2 : 1; }
static void noise_set_v(Options* o, int v)
{
	o->noise = v != 0;
	if (v)
		o->noise_ps2 = v == 2;
}
// The Enhanced Edition's two button-pressure stand-ins (hold to stomp, light and heavy swings) in one
// row: PC or Hold A. A hand-edited options.txt with them apart shows the stomp's until changed.
static int heavy_get(const Options* o) { return o->hold_stomp; }
static void heavy_set(Options* o, int v) { o->hold_stomp = o->swap_attack = (u8)v; }

static bool behind(const Options* o) { return o->camera == 1; }
static bool low(const Options* o) { (void)o; return profile_quality() == QUALITY_LOW; }
static const char* camera_note(const Options* o) { (void)o; return "Scenes and scripted shots always use the PC's camera."; }
static const char* scripted_note(const Options* o)
{
	return behind(o) ? "With the Behind camera, this only applies in scripted shots." : NULL;
}
static const char* view_note(const Options* o)
{
	return behind(o) ? "With the Behind camera, {LOOK} swings the camera behind James." : NULL;
}
static const char* noise_note(const Options* o) { (void)o; return "Brightness Level changes how strong the grain is."; }
static const char* shadows_note(const Options* o) { return low(o) ? "Low quality turns shadows off." : NULL; }
static const char* fog_note(const Options* o) { return low(o) ? "Low quality draws half as many particles." : NULL; }
static bool cleared(void) { return s_host.cleared; }

#define AT(field) .off = offsetof(Options, field)
static const Setting SETTINGS[] = {
	// Controls: the camera and the Control Type first, as they change the most
	{ .tab = TAB_CONTROLS, .label = -1, .name = "Camera", AT(camera), .hi = 1, .texts = CAMERA, .fig = FIG_CAMERA,
	  .what = "How the game frames James while you play.", .lines = CAMERA_L, .note = camera_note },
	{ .tab = TAB_CONTROLS, .label = 84, .name = "Control Type", AT(control_type), .hi = 1, .names = CONTROL_TYPE,
	  .fig = FIG_MOVEMENT, .what = "How the Circle Pad steers James.", .lines = CONTROL_L, .dim = behind,
	  .note = scripted_note },
	{ .tab = TAB_CONTROLS, .label = -1, .name = "Camera Speed", AT(ps2_camera), .hi = 1, .texts = PC_PS2,
	  .what = "How quickly the PC camera turns and settles.", .lines = CAMSPEED_L, .dim = behind,
	  .note = scripted_note },
	{ .tab = TAB_CONTROLS, .label = 44, .name = "Weapon Control", AT(weapon), .hi = 1, .names = NORMAL_SWITCH,
	  .fig = FIG_AIM, .what = "How {AIM} raises the weapon.", .lines = WEAPON_L },
	{ .tab = TAB_CONTROLS, .label = 57, .name = "Walk/Run Control", .hi = 3, .names = WALK_RUN, .texts = WALK_RUN_T,
	  .get = walkrun_get, .set = walkrun_set, .what = "When James walks and when he runs.", .lines = WALKRUN_L },
	{ .tab = TAB_CONTROLS, .label = 53, .name = "View Control", AT(view), .hi = 1, .names = NORMAL_SWITCH,
	  .fig = FIG_LOOK, .what = "How {LOOK} turns the view to where James faces.", .lines = VIEW_L, .dim = behind,
	  .note = view_note },
	{ .tab = TAB_CONTROLS, .label = -1, .name = "Heavy Attack", .hi = 1, .texts = HEAVY, .get = heavy_get,
	  .set = heavy_set, .what = "How {ACT} picks a light or a heavy attack.", .lines = HEAVY_L },
	{ .tab = TAB_CONTROLS, .kind = K_PAGE, .label = -1, .name = "Buttons", .page = PG_BUTTONS,
	  .what = "Choose the button for each action." },
	// Game: the PC's Game Options and the port's cheats
	{ .tab = TAB_GAME, .label = 94, .name = "Map", AT(map_zoom), .hi = 1, .names = ZOOM, .fig = FIG_MAP,
	  .what = "How the map opens.", .lines = MAP_L },
	{ .tab = TAB_GAME, .label = 48, .name = "Blood Color", AT(blood), .hi = 3, .names = BLOOD, .swatch = true,
	  .what = "The color of blood." },
	{ .tab = TAB_GAME, .kind = K_TIMES, .label = 65, .name = "Bullet Adjust", AT(bullet), .lo = 1, .hi = 3,
	  .what = "How many rounds a box of ammo holds.", .lines = BULLET_L, .shown = cleared },
	{ .tab = TAB_GAME, .label = -1, .name = "Infinite Stamina", AT(inf_stamina), .hi = 1, .texts = OFF_ON_TEXT,
	  .fig = FIG_STAMINA, .what = "Whether running tires James.", .lines = STAMINA_L },
	{ .tab = TAB_GAME, .label = -1, .name = "Infinite Ammo", AT(inf_ammo), .hi = 1, .texts = OFF_ON_TEXT,
	  .what = "Whether shots use up rounds.", .lines = AMMO_L },
	// Display
	{ .tab = TAB_DISPLAY, .kind = K_PAGE, .label = 2, .name = "Brightness Level", .page = PG_BRIGHT,
	  .what = "How bright the game's darkness looks." },
	{ .tab = TAB_DISPLAY, .label = -1, .name = "Picture", AT(picture), .hi = 1, .texts = PICTURE, .fig = FIG_PICTURE,
	  .what = "How smooth the picture looks.", .lines = PICTURE_L },
	{ .tab = TAB_DISPLAY, .label = -1, .name = "3D", AT(stereo), .hi = 1, .texts = OFF_ON_TEXT,
	  .what = "Depth on the top screen with the 3D slider.", .lines = STEREO_L },
	{ .tab = TAB_DISPLAY, .label = -1, .name = "Messages", AT(msg_top), .hi = 1, .texts = SCREENS, .fig = FIG_SCREENS,
	  .what = "Where messages and subtitles appear.", .lines = MESSAGES_L },
	{ .tab = TAB_DISPLAY, .label = 97, .name = "Noise Effect", .hi = 2, .names = NOISE, .texts = NOISE_T,
	  .get = noise_get, .set = noise_set_v, .what = "Film grain over the picture.", .lines = NOISE_L,
	  .note = noise_note },
	{ .tab = TAB_DISPLAY, .label = -1, .name = "Flashlight", AT(ps2_flash), .hi = 1, .texts = PC_PS2, .fig = FIG_BEAM,
	  .what = "How James' flashlight lights a room.", .lines = FLASH_L },
	{ .tab = TAB_DISPLAY, .label = -1, .name = "Cutscene View", AT(cutscene_fov), .hi = 1, .texts = FRAMING,
	  .fig = FIG_FRAMING, .what = "How scenes fill the wide top screen.", .lines = CUTFOV_L },
	{ .tab = TAB_DISPLAY, .label = 175, .name = "Shadows", AT(shadows), .hi = 1, .names = OFF_ON,
	  .what = "Shadows that characters cast from the lights.", .lines = SHADOWS_L, .dim = low, .note = shadows_note },
	{ .tab = TAB_DISPLAY, .label = 178, .name = "Fog", AT(fog), .hi = 1, .names = FOG,
	  .what = "How the drifting fog is drawn.", .lines = FOG_L, .note = fog_note },
	{ .tab = TAB_DISPLAY, .label = 192, .name = "Lens Flare", AT(lens_flare), .hi = 1, .names = OFF_ON,
	  .what = "The glow round a lamp that faces the camera.", .lines = FLARE_L },
	// Sound
	{ .tab = TAB_SOUND, .label = 74, .name = "Subtitles", AT(subtitles), .hi = 1, .names = OFF_ON,
	  .what = "Spoken lines written out on screen.", .lines = SUBS_L },
	{ .tab = TAB_SOUND, .label = 34, .name = "Speaker Config.", AT(speaker), .hi = 2, .names = SPEAKER,
	  .what = "How sound is mixed for the speakers.", .lines = SPEAKER_L },
	{ .tab = TAB_SOUND, .kind = K_BAR, .label = 40, .name = "BGM Volume", AT(bgm), .hi = 15,
	  .what = "Music and ambient sound." },
	{ .tab = TAB_SOUND, .kind = K_BAR, .label = 42, .name = "SE Volume", AT(se), .hi = 15,
	  .what = "Sound effects and voices." },
	// System: the language and the hardware profile (profile.c)
	{ .tab = TAB_SYSTEM, .label = 76, .name = "Language", AT(lang), .lo = 1, .hi = 5, .names = LANGUAGE,
	  .what = "The language of the game's text." },
	{ .tab = TAB_SYSTEM, .label = -1, .name = "Quality", AT(quality), .hi = 2, .texts = QUALITY,
	  .what = "How much the console draws.", .lines = QUALITY_L },
	{ .tab = TAB_SYSTEM, .label = -1, .name = "Frame Rate", AT(framerate), .hi = 2, .texts = FRAMES,
	  .what = "How often the picture is drawn.", .lines = FRAMES_L },
	{ .tab = TAB_SYSTEM, .label = -1, .name = "Hardware", AT(hwprofile), .hi = 2, .texts = HARDWARE,
	  .what = "Which console the game tunes itself for.", .lines = HARDWARE_L },
	{ .tab = TAB_SYSTEM, .label = -1, .name = "Layout", AT(ctrllayout), .hi = 2, .texts = HARDWARE,
	  .what = "Which buttons the controls may use.", .lines = LAYOUT_L },
};
#define N_SETTINGS ((int)(sizeof SETTINGS / sizeof *SETTINGS))
static const char* const TABS[TAB_N] = { "Controls", "Game", "Display", "Sound", "System" };

// ---------------------------------------------------------------- values and text

static int get(const Setting* s) { return s->get ? s->get(s_live) : ((const u8*)s_live)[s->off]; }
static void set(const Setting* s, int v)
{
	if (s->set)
		s->set(s_live, v);
	else
		((u8*)s_live)[s->off] = (u8)v;
}
static bool is_lang(const Setting* s) { return !s->get && s->off == offsetof(Options, lang) && s->kind == K_LIST; }

static int layout(void) { return s_force_layout >= 0 ? s_force_layout : (int)controls_layout(); }

// the first button bound to an action, by name ("ZL"), or the action's own name when none is
static const char* key_of(ControlsAction act, char* out, size_t n)
{
	u32 k = s_force_layout >= 0 ? controls_default((ControlsLayout)s_force_layout, act) : controls_keys(act);
	if (!k) {
		snprintf(out, n, "%s", controls_name(act));
		return out;
	}
	return controls_key_text(k & -k, out, n);
}

// src with its {TOKENS} in place
static const char* expand(const char* src, char* out, size_t n)
{
	size_t k = 0;
	out[0] = 0;
	for (const char* p = src ? src : ""; *p && k + 1 < n;) {
		const char* e = *p == '{' ? strchr(p, '}') : NULL;
		if (!e) {
			out[k++] = *p++;
			continue;
		}
		char tok[16], val[96] = "", key[24];
		snprintf(tok, sizeof tok, "%.*s", (int)(e - p - 1), p + 1);
		if (!strcmp(tok, "AIM")) key_of(ACT_AIM, val, sizeof val);
		else if (!strcmp(tok, "RUN")) key_of(ACT_RUN, val, sizeof val);
		else if (!strcmp(tok, "ACT")) key_of(ACT_ACTION, val, sizeof val);
		else if (!strcmp(tok, "LOOK")) key_of(ACT_LOOK, val, sizeof val);
		else if (!strcmp(tok, "CAMTURN"))   // tpcam.c: the C-Stick on the New 3DS layout, the D-Pad's free directions on the Old
			snprintf(val, sizeof val, "%s, and %s puts it back.",
			         layout() == CONTROLS_LAYOUT_NEW ? "The C-Stick turns it" : "The D-Pad or a drag turns it",
			         key_of(ACT_LOOK, key, sizeof key));
		else if (!strcmp(tok, "AUTOQ")) snprintf(val, sizeof val, "%s", profile_new() ? "High" : "Low");
		else if (!strcmp(tok, "AUTOFPS")) snprintf(val, sizeof val, "%d", profile_quality() == QUALITY_LOW ? 30 : 60);
		else if (!strcmp(tok, "CONSOLE")) snprintf(val, sizeof val, "%s", profile_console_new() ? "New" : "Old");
		k += snprintf(out + k, n - k, "%s", val);
		k = k < n ? k : n - 1;
		p = e + 1;
	}
	out[k] = 0;
	return out;
}

static const char* label(const Setting* s) { return s->label >= 0 ? msg(s->label, s->name) : s->name; }

static const char* value_name(const Setting* s, int v, char* buf, int size)
{
	if (s->kind == K_TIMES) {
		// "x \1" (67) with the number inserted
		const char* m = msg(67, "x \x01");
		int k = 0;
		for (; *m && k < size - 4; m++)
			k += *m == 1 ? snprintf(buf + k, size - k, "%d", v) : (buf[k] = *m, 1);
		buf[k] = 0;
		return buf;
	}
	if (s->kind == K_BAR) {
		snprintf(buf, size, "%d", v);
		return buf;
	}
	int i = v - s->lo;
	if (i < 0 || i > s->hi - s->lo)
		i = 0;
	if (s->names && s->names[i] >= 0)
		return msg(s->names[i], "");
	return expand(s->texts ? s->texts[i] : "", buf, size);
}

static bool dimmed(const Setting* s) { return s->dim && s->dim(s_live); }

// ---------------------------------------------------------------- the touch screen

static void build(void)
{
	s_n = 0;
	for (int i = 0; i < N_SETTINGS && s_n < MAX_ROWS; i++)
		if (SETTINGS[i].tab == s_tab && (!SETTINGS[i].shown || SETTINGS[i].shown()))
			s_rows[s_n++] = &SETTINGS[i];
}

static const char* page_title(int page)
{
	if (page == PG_BRIGHT)
		return msg(2, "Brightness Level");
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
	static char text[MAX_ROWS][72];
	const char* rows[MAX_ROWS];
	if (s_page == PG_BRIGHT) {
		snprintf(text[0], sizeof text[0], "Level   < %d >", s_live->bright);
		rows[0] = text[0];
		s_bright_sel = 0;
		touchui_menu(TOUI_OPTIONS, page_title(PG_BRIGHT), rows, 1, &s_bright_sel, back, 1);
		return;
	}
	TouiRow kinds[MAX_ROWS];
	u64 dim = 0;
	memset(kinds, 0, sizeof kinds);
	for (int i = 0; i < s_n; i++) {
		const Setting* s = s_rows[i];
		char v[48];
		if (s->kind == K_PAGE || s->kind == K_BAR)
			snprintf(text[i], sizeof text[i], "%s", label(s));
		else
			snprintf(text[i], sizeof text[i], "%s   < %s >", label(s), value_name(s, get(s), v, sizeof v));
		kinds[i] = (TouiRow){ s->kind == K_PAGE ? TOUI_ROW_PAGE : s->kind == K_BAR ? TOUI_ROW_BAR : TOUI_ROW_VALUE,
		                      (u8)(s->kind == K_BAR ? get(s) : 0), (u8)(s->kind == K_BAR ? s->hi : 0) };
		if (dimmed(s))
			dim |= 1ull << i;
		rows[i] = text[i];
	}
	const TouiMenuExtra extra = { TABS, TAB_N, &s_tab, kinds };
	touchui_menu_full(TOUI_OPTIONS, NULL, rows, s_n, &s_sel, back, 1, dim, 0, &extra);
}

static void focus(void)
{
	s_shown_tab = s_tab;
	s_shown_sel = s_sel;
	s_tab_sel[s_tab] = s_sel;
	s_focus_ms = osGetTime();
}

static void go_tab(int tab)
{
	s_page = PG_TABS;
	s_tab = (tab % TAB_N + TAB_N) % TAB_N;
	build();
	s_sel = s_tab_sel[s_tab] < s_n ? s_tab_sel[s_tab] : 0;
	focus();
	touch();
}

static void go(int page)
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
	touch();
}

void optmenu_open(Options* live, const OptHost* host)
{
	s_live = live;
	s_old = *live;
	s_host = *host;
	s_state = ST_MENU;
	go_tab(s_tab);
}

bool optmenu_active(void) { return s_state != ST_OFF; }
bool optmenu_brightness(void) { return s_state == ST_MENU && s_page == PG_BRIGHT; }

bool optmenu_noise_focus(void)
{
	return s_state == ST_MENU && s_page == PG_TABS && s_sel < s_n && s_rows[s_sel]->get == noise_get;
}

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

static void step(const Setting* r, int dir)
{
	int v0 = get(r), v = v0;
	bool wrap = r->kind != K_BAR;
	for (int k = 0; k <= r->hi - r->lo; k++) {
		v += dir;
		if (v > r->hi)
			v = wrap ? r->lo : r->hi;
		if (v < r->lo)
			v = wrap ? r->hi : r->lo;
		// the Language row passes over the languages the pack lacks (the PC has them all)
		if (!is_lang(r) || ev_language_available(v))
			break;
	}
	if (v != v0) {
		set(r, v);
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
			go_tab(s_tab);
		return true;
	}
	if (s_page == PG_BRIGHT) {
		// 0x466c30: left and right from 0 to 7, back to the tab with either button
		int v = s_live->bright + (down & KEY_DRIGHT ? 1 : 0) - (down & KEY_DLEFT ? 1 : 0);
		if (v >= 0 && v <= 7 && v != s_live->bright) {
			s_live->bright = (u8)v;
			changed();
			touch();
		}
		if (down & (KEY_A | KEY_B)) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			go_tab(s_tab);
		}
		return true;
	}
	if (s_tab != s_shown_tab) {          // a tab was tapped
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		go_tab(s_tab);
		return true;
	}
	if (down & (KEY_L | KEY_R)) {
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		go_tab(s_tab + (down & KEY_R ? 1 : -1));
		return true;
	}
	bool redraw = false;
	if (s_n && (down & (KEY_DUP | KEY_DDOWN))) {
		s_sel = (s_sel + (down & KEY_DDOWN ? 1 : s_n - 1)) % s_n;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		redraw = true;
	}
	if (s_sel >= s_n)
		s_sel = 0;
	if (s_sel != s_shown_sel) {          // moved, or a row was tapped
		focus();
		redraw = true;
	}
	const Setting* r = s_n ? s_rows[s_sel] : NULL;
	if (r && r->kind == K_PAGE) {
		if (down & KEY_A) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			go(r->page);
			return true;
		}
	} else if (r && (down & (KEY_DLEFT | KEY_DRIGHT | KEY_A))) {
		step(r, down & KEY_DLEFT ? -1 : 1);
		redraw = true;
	}
	if (down & KEY_B) {
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
	if (redraw)
		touch();
	return true;
}

// ---------------------------------------------------------------- the top screen

static MapDrawFn s_draw;

// The PC's text space (x 0..512, y 16..496, the 4:3 picture at 40..360 on the top screen).
static float sx(float x) { return 200.0f + (x - 256.0f) * 0.625f; }
static float sy(float y) { return 120.0f + (y - 256.0f) * 0.46875f; }

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

// The explainer's layout, in top-screen pixels: the title at 8, what the setting does at 31, the
// values from 50, a note at 223. Two values with a drawing go side by side (the drawings 176 x 104
// at x 16 and 208, the names under them, their lines under the names); the rest are listed, name
// on the left and what it does beside it, the chosen one on the band.
#define EX_TOP 50.0f
#define EX_NOTE 223.0f

static void key_for(int fig, char* out, size_t n)
{
	ControlsAction act = fig == FIG_AIM ? ACT_AIM : fig == FIG_LOOK ? ACT_LOOK : ACT_RUN;
	key_of(act, out, n);
}

static void explain_buttons(void)
{
	// the actions of the Buttons page with their buttons, in two columns
	ControlsAction acts[16];
	int n = controls_actions(acts, 16), half = (n + 1) / 2;
	for (int i = 0; i < n; i++) {
		float x = i < half ? 26.0f : 214.0f, y = EX_TOP + 4 + (i % half) * 21.0f;
		char keys[48];
		u32 k = s_force_layout >= 0 ? controls_default((ControlsLayout)s_force_layout, acts[i]) : controls_keys(acts[i]);
		fig_text(controls_name(acts[i]), x, y + 2, 92, 1, false, FONT_LEFT, FIGC_LABEL);
		fig_text(controls_key_text(k, keys, sizeof keys), x + 96, y, 66, 1, true, FONT_LEFT, k ? FIGC_WHITE : FIGC_DIM);
	}
}

static void explain(const Setting* s, float t)
{
	char buf[200], name[64];
	fig_title(label(s), 8);
	fig_text(expand(s->what, buf, sizeof buf), 200, 31, 372, 1, false, FONT_CENTER, FIGC_GREY);
	const char* note = s->note ? s->note(s_live) : NULL;
	float bottom = note ? EX_NOTE - 5 : 234;
	if (s->kind == K_PAGE && s->page == PG_BUTTONS) {
		explain_buttons();
	} else if (s->kind == K_PAGE) {
		// the brightness screen's lines at the level now, and the PC's own line for it
		for (int k = -190; k < 0xd2; k += 20)
			fig_rect(200.0f + k * 0.625f, EX_TOP + 12, 201.0f + k * 0.625f, 170, 0x80808000u | (u32)(s_live->bright * 12));
		fig_text(msg(4, "Adjust until vertical lines appear faint."), 200, 186, 372, 1, true, FONT_CENTER, FIGC_GREY);
	} else if (s->kind == K_BAR) {
		// the PC's fifteen two-tone blocks (0x45bf10), large
		int v = get(s);
		float x0 = 200.0f - (15 * 18 - 4) * 0.5f;
		for (int i = 0; i < 15; i++) {
			bool lit = i < v;
			fig_rect(x0 + i * 18, 92, x0 + i * 18 + 14, 104, lit ? 0xb0b0b0ffu : 0x3a3a3affu);
			fig_rect(x0 + i * 18, 104, x0 + i * 18 + 14, 118, lit ? 0x5a5a5affu : 0x2e2e2effu);
		}
		snprintf(buf, sizeof buf, "%d", v);
		fig_text(buf, 200, 130, 60, 1, true, FONT_CENTER, FIGC_WHITE);
	} else if (s->swatch) {
		// the blood's colours (blood.c) as drops, the chosen one lit
		static const u32 col[4] = { 0x6e0a0aff, 0x2e5a1cff, 0x4c2161ff, 0x161313ff };
		for (int i = 0; i <= s->hi; i++) {
			float cx = 200.0f + (i - 1.5f) * 88.0f;
			bool on = get(s) == i;
			fig_drop(cx, 104, 17, col[i], on);
			fig_text(value_name(s, i, name, sizeof name), cx, 140, 84, 1, true, FONT_CENTER, on ? FIGC_WHITE : FIGC_LABEL);
			if (on)
				fig_rect(cx - 12, 156, cx + 12, 158, FIGC_RED);
		}
	} else if (s->fig && s->hi - s->lo == 1) {
		char key[24];
		key_for(s->fig, key, sizeof key);
		for (int i = 0; i < 2; i++) {
			float x = i ? 208.0f : 16.0f, cx = x + FIG_W * 0.5f;
			bool on = get(s) == s->lo + i;
			fig_draw(s->fig, i, x, EX_TOP, t, on, key);
			fig_text(value_name(s, s->lo + i, name, sizeof name), cx, 158, 172, 1, true, FONT_CENTER,
			         on ? FIGC_WHITE : FIGC_LABEL);
			if (on)
				fig_rect(cx - 12, 174, cx + 12, 176, FIGC_RED);
			int max = (int)((bottom - 179) / font_line_px(FONT_SMALL));
			fig_text(expand(s->lines[i], buf, sizeof buf), cx, 179, 172, max > 3 ? 3 : max, false, FONT_CENTER,
			         on ? FIGC_GREY : FIGC_DIM);
		}
	} else {
		int n = s->hi - s->lo + 1;
		float rh = s->lines ? (n > 3 ? 30.0f : 36.0f) : 18.0f, y = EX_TOP + (bottom - EX_TOP - rh * n) * 0.5f;
		y = y < EX_TOP ? EX_TOP : y;
		for (int i = 0; i < n; i++, y += rh) {
			int v = s->lo + i;
			bool on = get(s) == v;
			const char* nm = value_name(s, v, name, sizeof name);
			if (s->lines) {
				float h = font_line_px(FONT_BIG);
				char line[200];
				expand(s->lines[i], line, sizeof line);
				FontFit f;
				font_fit_px(line, 236, 2, false, &f);
				float lh = f.n * font_line_px(FONT_SMALL);
				h = lh > h ? lh : h;
				if (on)
					fig_band(14, y - 4, 386, y + h + 3);
				fig_text(nm, 30, y, 104, 1, true, FONT_LEFT, on ? FIGC_WHITE : FIGC_LABEL);
				fig_text(line, 142, y + 1, 236, 2, false, FONT_LEFT, on ? FIGC_WHITE : FIGC_GREY);
			} else {
				if (on)
					fig_band(110, y - 3, 290, y + font_line_px(FONT_BIG) + 2);
				fig_text(nm, 200, y, 170, 1, true, FONT_CENTER, on ? FIGC_WHITE : FIGC_LABEL);
			}
		}
	}
	if (note)
		fig_text(expand(note, buf, sizeof buf), 200, EX_NOTE, 372, 1, false, FONT_CENTER, FIGC_LABEL);
}

// Over play the explainer has a darker ground than the paused game's half dim (main.c), so its
// text reads over bright rooms.
static void scrim(void)
{
	MapVert v[6] = { { 0, 0, 0, 0 }, { 400, 0, 0, 0 }, { 400, 240, 0, 0 }, { 0, 0, 0, 0 }, { 400, 240, 0, 0 }, { 0, 240, 0, 0 } };
	s_draw(NULL, v, 6, 0x0b0a0aa0);
}

void optmenu_draw_top(MapDrawFn draw)
{
	if (s_state == ST_OFF || !font_ready() || !draw)
		return;
	s_draw = draw;
	if (optmenu_brightness()) {
		brightness_screen();
		return;
	}
	if (!optmenu_noise_focus())
		scrim();
	fig_begin(1.0f);
	if (s_page == PG_BUTTONS) {
		// the chosen action and its buttons, "Press a button" while the page waits for one
		const char* name;
		char keys[48];
		bool wait = controls_menu_focus(&name, keys, sizeof keys);
		fig_title("Buttons", 8);
		fig_text(name, 200, 96, 372, 1, true, FONT_CENTER, FIGC_WHITE);
		fig_text(wait ? "Press a button" : keys, 200, 122, 372, 1, true, FONT_CENTER, wait ? FIGC_RED : FIGC_GREY);
		if (wait)
			fig_text("START keeps the button it had.", 200, EX_NOTE, 372, 1, false, FONT_CENTER, FIGC_LABEL);
	} else if (s_state == ST_ASK) {
		fig_text(msg(89, "Change the settings?"), 200, 110, 372, 2, true, FONT_CENTER, FIGC_WHITE);
	} else if (s_sel < s_n) {
		explain(s_rows[s_sel], (osGetTime() - s_focus_ms) / 1000.0f);
	}
	fig_end();
}

// ---------------------------------------------------------------- debug.txt "textcheck"

int optmenu_textcheck(FILE* out, Options* live)
{
	static const char* const LANG[6] = { "", "English", "French", "German", "Italian", "Spanish" };
	Options* keep = s_live;
	int lang0 = ev_language(), cuts0 = fig_cuts();
	for (int lang = 1; lang <= 5; lang++) {
		if (!ev_language_available(lang))
			continue;
		ev_set_language(lang);
		for (int lay = 0; lay < 2; lay++) {
			s_force_layout = lay;
			for (int i = 0; i < N_SETTINGS; i++) {
				const Setting* s = &SETTINGS[i];
				int vals = s->kind == K_PAGE ? 1 : s->hi - s->lo + 1;
				for (int k = 0; k < vals; k++)
					for (int side = 0; side < 2; side++) {
						// side 1: the settings that bring the notes up (Behind, Low quality)
						Options o = *live;
						if (side)
							o.camera = 1;
						s_live = &o;
						if (s->kind != K_PAGE)
							set(s, s->lo + k);
						char ctx[96], v[48];
						snprintf(ctx, sizeof ctx, "%s %s layout, %s = %d", LANG[lang], lay ? "New" : "Old", s->name, s->lo + k);
						fig_measure(out, ctx);
						explain(s, 0.0f);
						// the touch screen's row: the label and its value side by side (touchui.c
						// draws a long label small, so this only notes the tight ones)
						if (s->kind == K_LIST || s->kind == K_TIMES) {
							float w = font_width_px(FONT_BIG, label(s)) + font_width_px(FONT_BIG, value_name(s, get(s), v, sizeof v));
							if (w > 304 - 22 - 18 - 30)
								fprintf(out, "tight %s: %s / %s\n", ctx, label(s), v);
						}
						fig_measure_end();
					}
			}
		}
	}
	s_force_layout = -1;
	ev_set_language(lang0);
	s_live = keep;
	return fig_cuts() - cuts0;
}
