// Button layouts and remapping (see controls.h and port/CONTROLS.md).
#include "controls.h"
#include <string.h>
#include "sfx.h"
#include "touchui.h"

#define BINDABLE (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR | KEY_SELECT | KEY_DUP | \
                  KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_CSTICK_UP | KEY_CSTICK_DOWN | KEY_CSTICK_LEFT | \
                  KEY_CSTICK_RIGHT)
#define OLD_KEYS (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_SELECT | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)

// The actions the menu lists, in its order (pause and cancel stay on START and B)
static const ControlsAction MENU_ACTS[] = {
	ACT_ACTION, ACT_RUN, ACT_AIM, ACT_WEAPON_NEXT, ACT_FLASHLIGHT, ACT_ITEMS, ACT_LOOK, ACT_TURN180,
	ACT_STRAFE_LEFT, ACT_STRAFE_RIGHT, ACT_MAP,
};
#define N_MENU_ACTS ((int)(sizeof MENU_ACTS / sizeof MENU_ACTS[0]))

static struct {
	char path[96];
	u32 user[2][ACT_COUNT];     // per layout; 0 = the default
	bool set[2][ACT_COUNT];
	int sel;
	bool capture;               // waiting for the button to bind
	bool dirty;
	char label[40];
	char rows[N_MENU_ACTS + 2][40];
} C;

#if !__has_include("profile.h")
ControlsLayout controls_layout(void)
{
	bool n = false;
	APT_CheckNew3DS(&n);
	return n ? CONTROLS_LAYOUT_NEW : CONTROLS_LAYOUT_OLD;
}
#endif

u32 controls_default(ControlsLayout layout, ControlsAction act)
{
	bool n = layout == CONTROLS_LAYOUT_NEW;
	switch (act) {
	case ACT_RUN: return n ? KEY_B | KEY_ZR : KEY_B;
	case ACT_ACTION: return KEY_A;
	case ACT_AIM: return KEY_R;
	case ACT_WEAPON_NEXT: return n ? KEY_L : KEY_DUP;
	case ACT_FLASHLIGHT: return KEY_X;
	case ACT_ITEMS: return KEY_Y;
	case ACT_LOOK: return n ? KEY_ZL : KEY_L;
	case ACT_TURN180: return n ? KEY_CSTICK_DOWN : KEY_DDOWN;
	case ACT_STRAFE_LEFT: return n ? KEY_CSTICK_LEFT | KEY_DLEFT : KEY_DLEFT;
	case ACT_STRAFE_RIGHT: return n ? KEY_CSTICK_RIGHT | KEY_DRIGHT : KEY_DRIGHT;
	case ACT_MAP: return n ? KEY_CSTICK_UP : 0;      // and a tap on the bottom screen, both layouts
	case ACT_PAUSE: return KEY_START;
	case ACT_CANCEL: return KEY_B;
	default: return 0;
	}
}

const char* controls_name(ControlsAction act)
{
	static const char* const names[ACT_COUNT] = { "Run", "Action", "Aim", "Next weapon", "Flashlight", "Items",
	                                               "Look", "Quick turn", "Strafe left", "Strafe right", "Map",
	                                               "Pause", "Cancel" };
	return act < ACT_COUNT ? names[act] : "";
}

static int L(void) { return controls_layout() == CONTROLS_LAYOUT_NEW ? 1 : 0; }

u32 controls_keys(ControlsAction act)
{
	int l = L();
	u32 k = C.set[l][act] ? C.user[l][act] : controls_default((ControlsLayout)l, act);
	return l ? k : k & ~(KEY_ZL | KEY_ZR | KEY_CSTICK_UP | KEY_CSTICK_DOWN | KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT);
}

static const char* const LAYOUT_KEY[2] = { "old", "new" };

void controls_init(const char* dir)
{
	snprintf(C.path, sizeof C.path, "%s/controls.txt", dir);
	FILE* f = fopen(C.path, "r");
	if (!f)
		return;
	char lay[8], act[24];
	unsigned keys;
	while (fscanf(f, "%7s %23s %x", lay, act, &keys) == 3) {
		int l = !strcmp(lay, "new") ? 1 : !strcmp(lay, "old") ? 0 : -1;
		for (int a = 0; l >= 0 && a < ACT_COUNT; a++) {
			char name[24];
			snprintf(name, sizeof name, "%s", controls_name((ControlsAction)a));
			for (char* p = name; *p; p++)
				if (*p == ' ') *p = '_';
			if (!strcmp(name, act)) {
				C.user[l][a] = keys & BINDABLE & (l ? BINDABLE : OLD_KEYS);
				C.set[l][a] = true;
			}
		}
	}
	fclose(f);
}

static void save(void)
{
	FILE* f = fopen(C.path, "w");
	if (!f)
		return;
	fprintf(f, "# layout action keys (libctru KEY_* bits); written by the Buttons page\n");
	for (int l = 0; l < 2; l++)
		for (int a = 0; a < ACT_COUNT; a++) {
			if (!C.set[l][a])
				continue;
			char name[24];
			snprintf(name, sizeof name, "%s", controls_name((ControlsAction)a));
			for (char* p = name; *p; p++)
				if (*p == ' ') *p = '_';
			fprintf(f, "%s %s %lx\n", LAYOUT_KEY[l], name, (unsigned long)C.user[l][a]);
		}
	fclose(f);
}

void controls_game(u32 down, u32 held, u32* gdown, u32* gheld)
{
	static const struct { ControlsAction act; u32 bit; } MAP[] = {
		{ ACT_ACTION, KEY_A }, { ACT_RUN, KEY_B }, { ACT_AIM, KEY_R }, { ACT_WEAPON_NEXT, KEY_L },
		{ ACT_FLASHLIGHT, KEY_X }, { ACT_ITEMS, KEY_Y }, { ACT_LOOK, GK_LOOK }, { ACT_TURN180, GK_TURN180 },
		{ ACT_STRAFE_LEFT, GK_STRAFE_L }, { ACT_STRAFE_RIGHT, GK_STRAFE_R }, { ACT_MAP, GK_MAP },
	};
	u32 d = 0, h = 0;
	for (unsigned i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
		u32 k = controls_keys(MAP[i].act);
		if (down & k) d |= MAP[i].bit;
		if (held & k) h |= MAP[i].bit;
	}
	if (controls_dpad_brightness()) {
		d |= down & (KEY_DUP | KEY_DDOWN);
		h |= held & (KEY_DUP | KEY_DDOWN);
	}
	d |= down & (KEY_START | KEY_SELECT | KEY_TOUCH);
	h |= held & (KEY_START | KEY_SELECT | KEY_TOUCH);
	*gdown = d;
	*gheld = h;
}

bool controls_dpad_brightness(void)
{
	u32 used = 0;
	for (int a = 0; a < ACT_COUNT; a++)
		used |= controls_keys((ControlsAction)a);
	return !(used & (KEY_DUP | KEY_DDOWN));
}

static const char* key_names(u32 k, char* out, size_t n)
{
	static const struct { u32 bit; const char* name; } K[] = {
		{ KEY_A, "A" }, { KEY_B, "B" }, { KEY_X, "X" }, { KEY_Y, "Y" }, { KEY_L, "L" }, { KEY_R, "R" },
		{ KEY_ZL, "ZL" }, { KEY_ZR, "ZR" }, { KEY_START, "START" }, { KEY_SELECT, "SELECT" },
		{ KEY_DUP, "Up" }, { KEY_DDOWN, "Down" }, { KEY_DLEFT, "Left" }, { KEY_DRIGHT, "Right" },
		{ KEY_CSTICK_UP, "C-Up" }, { KEY_CSTICK_DOWN, "C-Down" }, { KEY_CSTICK_LEFT, "C-Left" },
		{ KEY_CSTICK_RIGHT, "C-Right" },
	};
	size_t len = 0;
	out[0] = 0;
	for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++)
		if (k & K[i].bit)
			len += snprintf(out + len, len < n ? n - len : 0, "%s%s", len ? " / " : "", K[i].name);
	if (!len)
		snprintf(out, n, "-");
	return out;
}

int controls_help(char lines[][40], int max)
{
	int n = 0;
	for (int i = 0; i < N_MENU_ACTS && n < max; i++) {
		char k[32];
		snprintf(lines[n++], 40, "%-12s%s", controls_name(MENU_ACTS[i]), key_names(controls_keys(MENU_ACTS[i]), k, sizeof k));
	}
	return n;
}

// ---------------------------------------------------------------- the Buttons page

const char* controls_menu_label(void)
{
	snprintf(C.label, sizeof C.label, "Buttons     %s 3DS layout", L() ? "New" : "Old");
	return C.label;
}

void controls_menu_open(void)
{
	C.sel = 0;
	C.capture = false;
	C.dirty = true;
}

static void bind_key(ControlsAction act, u32 key)
{
	int l = L();
	// a button does one thing: take it off the action that had it
	for (int a = 0; a < ACT_COUNT; a++) {
		if (a == (int)act || !(controls_keys((ControlsAction)a) & key))
			continue;
		C.user[l][a] = controls_keys((ControlsAction)a) & ~key;
		C.set[l][a] = true;
	}
	C.user[l][act] = key;
	C.set[l][act] = true;
	save();
}

bool controls_menu_update(u32 down)
{
	int n = N_MENU_ACTS + 2;          // the actions, "Defaults", "Back"
	if (C.capture) {
		u32 allowed = L() ? BINDABLE : OLD_KEYS;
		if (down & KEY_START) {
			C.capture = false;
			C.dirty = true;
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		} else if (down & allowed) {
			u32 k = down & allowed;
			k &= -k;                    // the lowest bit if several went down at once
			bind_key(MENU_ACTS[C.sel], k);
			C.capture = false;
			C.dirty = true;
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		}
		return true;
	}
	if ((down & (KEY_B | KEY_START)) || ((down & KEY_A) && C.sel == n - 1)) {
		sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		for (int r = 18; r < 30; r++)
			printf("\x1b[%d;1H%40s", r, "");
		return false;
	}
	if (down & (KEY_DUP | KEY_DDOWN)) {
		C.sel = (C.sel + (down & KEY_DDOWN ? 1 : n - 1)) % n;
		C.dirty = true;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
	}
	if (down & KEY_A) {
		if (C.sel < N_MENU_ACTS) {
			C.capture = true;
		} else {
			int l = L();                // Defaults: this layout's own
			memset(C.set[l], 0, sizeof C.set[l]);
			save();
		}
		C.dirty = true;
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
	}
	if ((down & KEY_Y) && C.sel < N_MENU_ACTS) {
		C.set[L()][MENU_ACTS[C.sel]] = false;      // this action back to its default
		save();
		C.dirty = true;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
	}
	return true;
}

void controls_menu_draw(void)
{
	const char* rows[N_MENU_ACTS + 2];
	for (int i = 0; i < N_MENU_ACTS; i++) {
		char k[32];
		snprintf(C.rows[i], sizeof C.rows[i], "%-12s %s", controls_name(MENU_ACTS[i]),
		         C.capture && i == C.sel ? "press a button" : key_names(controls_keys(MENU_ACTS[i]), k, sizeof k));
		rows[i] = C.rows[i];
	}
	rows[N_MENU_ACTS] = "Defaults";
	rows[N_MENU_ACTS + 1] = "Back";
	static const TouiButton back[1] = { { "Back", KEY_B } };
	touchui_menu(TOUI_PAUSE, L() ? "Buttons (New 3DS)" : "Buttons (Old 3DS)", rows, N_MENU_ACTS + 2, &C.sel, back, 1);
	if (!C.dirty)
		return;
	C.dirty = false;
	for (int r = 18; r < 30; r++)
		printf("\x1b[%d;1H%40s", r, "");
	printf("\x1b[17;1H------- Buttons (%s 3DS layout) -------", L() ? "New" : "Old");
	// 13 rows for 12 lines: scroll so the cursor stays on screen
	int first = C.sel > 9 ? C.sel - 9 : 0;
	for (int i = first; i < N_MENU_ACTS + 2 && i < first + 10; i++)
		printf("\x1b[%d;3H%c %-34.34s", 18 + i - first, i == C.sel ? '>' : ' ', rows[i]);
	printf("\x1b[29;1H%-40s", C.capture ? " Press the button (START: keep)" : " A: change  Y: default  B: back");
}
