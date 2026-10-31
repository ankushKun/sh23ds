#include "setup.h"
#include "controls.h"
#include "events.h"
#include "figures.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <string.h>

enum { STEP_CAMERA, STEP_MOVEMENT, STEP_CONTROLS, STEP_N };

static Options* s_live;
static void (*s_apply)(const Options*);
static void (*s_save)(const Options*);
static bool s_on, s_only_controls;
static int s_step, s_sel, s_shown;
static u64 s_open_ms, s_step_ms;

static const char* msg(int n, const char* fallback)
{
	const char* m = ev_option_message(n);
	return m && *m ? m : fallback;
}

static u8* field(int step) { return step == STEP_CAMERA ? &s_live->camera : &s_live->control_type; }

// The names and lines of a step's two choices (the Options' explainer says more, optmenu.c).
static void texts(int step, bool new_layout, const char* names[2], const char* lines[2])
{
	if (step == STEP_CAMERA) {
		names[0] = "Original", names[1] = "Behind";
		lines[0] = "The PC's own shots, framed for each room.";
		lines[1] = new_layout ? "Follows James. The C-Stick turns it." : "Follows James. Drag the touch screen to turn it.";
	} else {
		names[0] = msg(85, "Rotational Style"), names[1] = msg(86, "Directional Style");
		lines[0] = "Left and right turn James. Up walks, down backs away.";
		lines[1] = "Push the way you want James to go.";
	}
}

static void touch(void)
{
	// B on the left and A on the right, where they sit on the console; Back keeps its place, dim, on
	// the first step
	static const TouiButton prompts[2] = { { "Back", KEY_B }, { "Choose", KEY_A } };
	const char *names[2], *lines[2];
	texts(s_step, controls_layout() == CONTROLS_LAYOUT_NEW, names, lines);
	touchui_choices(names, lines, &s_sel, prompts, 2, s_step == 0 ? 1u : 0u);
}

static void go(int step)
{
	s_step = step;
	s_step_ms = osGetTime();
	if (step == STEP_CONTROLS) {
		static const TouiButton prompts[2] = { { "Back", KEY_B }, { "Start", KEY_A } };
		touchui_choices_close();
		// shown alone (the buttons moved under a player who had seen the first run) it has no Back
		touchui_box("Every button can be changed\nin Options, Controls.", s_only_controls ? prompts + 1 : prompts,
		            s_only_controls ? 1 : 2);
		return;
	}
	touchui_box(NULL, NULL, 0);
	s_sel = s_shown = *field(step) ? 1 : 0;
	touch();
}

void setup_open(Options* live, void (*apply)(const Options*), void (*save)(const Options*), bool only_controls)
{
	s_live = live, s_apply = apply, s_save = save;
	s_on = true;
	s_only_controls = only_controls;
	s_open_ms = osGetTime();
	go(only_controls ? STEP_CONTROLS : STEP_CAMERA);
}

bool setup_active(void) { return s_on; }

static void choose(int v)
{
	if (v == s_shown)
		return;
	s_sel = s_shown = v;
	*field(s_step) = (u8)v;
	if (s_apply)
		s_apply(s_live);
	sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
	touch();
}

void setup_update(u32 down)
{
	if (!s_on)
		return;
	if (down & KEY_START)
		down |= KEY_A;                   // START chooses, as on the title's menus
	bool controls = s_step == STEP_CONTROLS;
	if (!controls) {
		if (s_sel != s_shown)            // a tap chose the other one
			choose(s_sel);
		if (down & (KEY_DLEFT | KEY_DRIGHT))
			choose(down & KEY_DLEFT ? 0 : 1);
		else if (down & (KEY_DUP | KEY_DDOWN))
			choose(s_sel ^ 1);
	}
	// the Control Type is only the PC camera's, so Behind goes from the camera to the buttons
	bool behind = s_live->camera == 1;
	if (down & KEY_A) {
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		if (!controls) {
			go(s_step == STEP_CAMERA && behind ? STEP_CONTROLS : s_step + 1);
			return;
		}
		s_live->setup = s_live->ctl_seen = 1;
		if (s_apply)
			s_apply(s_live);
		if (s_save)
			s_save(s_live);
		touchui_choices_close();
		touchui_box(NULL, NULL, 0);
		s_on = false;
		return;
	}
	if ((down & KEY_B) && s_step > 0 && !s_only_controls) {
		sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		go(controls && behind ? STEP_CAMERA : s_step - 1);
	}
}

// The buttons as they are bound now (controls.c), in two columns: the Circle Pad, the face buttons,
// the shoulders and START, then the D-Pad and the New 3DS's own. An action with no button is left
// out. The Action row says what One Button makes of it (optmenu.h "attack").
typedef struct { int act; const char* name; } Row;
static const Row ROWS[2][8] = {
	{ { -1, "Move" }, { ACT_ACTION, NULL }, { ACT_RUN, "Run" }, { ACT_AIM, "Aim" }, { ACT_TURN180, "Quick turn" },
	  { ACT_FLASHLIGHT, "Flashlight" }, { ACT_ITEMS, "Items" }, { ACT_PAUSE, "Pause" } },
	{ { ACT_LOOK, "Look" }, { ACT_WEAPON_NEXT, "Next weapon" }, { ACT_HEAL, "Quick heal" }, { ACT_MAP, "Map" },
	  { ACT_STRAFE_LEFT, "Strafe left" }, { ACT_STRAFE_RIGHT, "Strafe right" }, { ACT_TARGET_LEFT, "Target left" },
	  { ACT_TARGET_RIGHT, "Target right" } },
};

static const char* row_name(const Row* r)
{
	return r->name ? r->name : s_live && s_live->attack == 0 ? "Check, attack" : "Check, attack when aiming";
}

// A D-Pad direction: the cross, the arm pressed lit
static void dpad_glyph(float cx, float cy, u32 key, u32 dim, u32 lit)
{
	fig_rect(cx - 2.5f, cy - 8.0f, cx + 2.5f, cy + 8.0f, dim);
	fig_rect(cx - 8.0f, cy - 2.5f, cx + 8.0f, cy + 2.5f, dim);
	if (key == KEY_DUP) fig_rect(cx - 2.5f, cy - 8.0f, cx + 2.5f, cy - 2.5f, lit);
	if (key == KEY_DDOWN) fig_rect(cx - 2.5f, cy + 2.5f, cx + 2.5f, cy + 8.0f, lit);
	if (key == KEY_DLEFT) fig_rect(cx - 8.0f, cy - 2.5f, cx - 2.5f, cy + 2.5f, lit);
	if (key == KEY_DRIGHT) fig_rect(cx + 2.5f, cy - 2.5f, cx + 8.0f, cy + 2.5f, lit);
}

static void draw_controls(void)
{
	fig_title("Controls", 14);
	for (int col = 0; col < 2; col++) {
		float x = col ? 212.0f : 20.0f, y = 58.0f;
		for (int i = 0; i < 8; i++) {
			const Row* r = &ROWS[col][i];
			u32 k = r->act < 0 ? 1 : controls_keys((ControlsAction)r->act);
			if (!k)
				continue;
			k &= -k;                         // the first of its buttons
			if (r->act < 0) {
				fig_ring(x + 34.0f, y, "Circle Pad", FIGC_GREY);
			} else if (k & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) {
				dpad_glyph(x + 34.0f, y, k, FIGC_DIM, FIGC_WHITE);
			} else {
				char name[24];
				fig_ring(x + 34.0f, y, controls_key_text(k, name, sizeof name), FIGC_GREY);
			}
			fig_text(row_name(r), x + 80.0f, y - 6.0f, 104.0f, 1, false, 0, FIGC_WHITE);
			y += 22.0f;
		}
	}
}

void setup_draw_top(void)
{
	if (!s_on || !font_ready())
		return;
	u64 now = osGetTime();
	float in = (now - s_open_ms) / 600.0f, t = (now - s_step_ms) / 1000.0f;
	fig_begin(in);
	if (s_step == STEP_CONTROLS) {
		draw_controls();
		fig_end();
		return;
	}
	fig_title(s_step == STEP_CAMERA ? "Camera" : "Movement", 14);
	int fig = s_step == STEP_CAMERA ? FIG_CAMERA : FIG_MOVEMENT;
	for (int i = 0; i < 2; i++)
		fig_draw(fig, i, i ? 208.0f : 16.0f, 82.0f, t, s_sel == i, NULL);
	fig_end();
}

int setup_textcheck(FILE* out)
{
	int cuts0 = fig_cuts();
	for (int col = 0; col < 2; col++)
		for (int i = 0; i < 8; i++) {
			fig_measure(out, "first run controls");
			fig_text(row_name(&ROWS[col][i]), 0, 0, 104, 1, false, 0, 0xffffffff);
			fig_measure_end();
		}
	for (int step = 0; step < STEP_CONTROLS; step++)
		for (int lay = 0; lay < 2; lay++) {
			const char *names[2], *lines[2];
			texts(step, lay == 1, names, lines);
			for (int i = 0; i < 2; i++) {
				char ctx[64];
				snprintf(ctx, sizeof ctx, "first run %s, %s layout", names[i], lay ? "New" : "Old");
				fig_measure(out, ctx);
				fig_text(names[i], 0, 0, 136, 1, true, 0, 0xffffffff);     // touchui.c draw_choices
				fig_text(lines[i], 0, 0, 132, 3, false, 0, 0xffffffff);
				fig_measure_end();
			}
		}
	return fig_cuts() - cuts0;
}
