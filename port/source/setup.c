#include "setup.h"
#include "controls.h"
#include "events.h"
#include "figures.h"
#include "font.h"
#include "sfx.h"
#include "touchui.h"
#include <string.h>

enum { STEP_CAMERA, STEP_MOVEMENT, STEP_N };

static Options* s_live;
static void (*s_apply)(const Options*);
static void (*s_save)(const Options*);
static bool s_on;
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
		lines[1] = new_layout ? "Follows James. The C-Stick turns it." : "Follows James. The D-Pad turns it.";
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
	s_sel = s_shown = *field(step) ? 1 : 0;
	s_step_ms = osGetTime();
	touch();
}

void setup_open(Options* live, void (*apply)(const Options*), void (*save)(const Options*))
{
	s_live = live, s_apply = apply, s_save = save;
	s_on = true;
	s_open_ms = osGetTime();
	go(STEP_CAMERA);
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
	if (s_sel != s_shown)                // a tap chose the other one
		choose(s_sel);
	if (down & (KEY_DLEFT | KEY_DRIGHT))
		choose(down & KEY_DLEFT ? 0 : 1);
	else if (down & (KEY_DUP | KEY_DDOWN))
		choose(s_sel ^ 1);
	if (down & KEY_A) {
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		bool last = s_step + 1 >= STEP_N || (s_step == STEP_CAMERA && s_live->camera == 1);
		if (!last) {
			go(s_step + 1);
			return;
		}
		s_live->setup = 1;
		if (s_apply)
			s_apply(s_live);
		if (s_save)
			s_save(s_live);
		touchui_choices_close();
		s_on = false;
		return;
	}
	if ((down & KEY_B) && s_step > 0) {
		sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		go(s_step - 1);
	}
}

void setup_draw_top(void)
{
	if (!s_on || !font_ready())
		return;
	u64 now = osGetTime();
	float in = (now - s_open_ms) / 600.0f, t = (now - s_step_ms) / 1000.0f;
	fig_begin(in);
	fig_title(s_step == STEP_CAMERA ? "Camera" : "Movement", 14);
	int fig = s_step == STEP_CAMERA ? FIG_CAMERA : FIG_MOVEMENT;
	for (int i = 0; i < 2; i++)
		fig_draw(fig, i, i ? 208.0f : 16.0f, 82.0f, t, s_sel == i, NULL);
	fig_end();
}

int setup_textcheck(FILE* out)
{
	int cuts0 = fig_cuts();
	for (int step = 0; step < STEP_N; step++)
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
