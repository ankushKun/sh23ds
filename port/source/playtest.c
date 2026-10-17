// Playtest driver (see playtest.h). The script is the "pt" lines of debug.txt, run in order; each
// command waits for the game to be idle first where that matters (no warp, fade, message, program,
// scene, still or menu), and while it waits it closes messages with A, skips scenes with START and
// answers choices from the "answer" queue (first answer otherwise). Every flag, item, stage and room
// change, message and program start is logged with the game time, so a failing beat shows what the
// game did instead.
#include "playtest.h"
#include "enemy.h"
#include "combat.h"
#include "events.h"
#include "prog.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define LOG_PATH PACK_DIR "/ptlog.txt"
#define MAX_CMDS 512

typedef struct {
	char text[96];
} Cmd;

static const PtHost* H;
static Cmd s_cmd[MAX_CMDS];
static int s_ncmd, s_pc, s_phase, s_frames;
static float s_t, s_cmd_t;               // game time, time in the current command
static FILE* s_log;
static bool s_active, s_skip = true, s_god, s_noenemies, s_stop_on_fail, s_quit;
static int s_answers[16], s_nanswers;
static int s_pass, s_fail;
// buttons the driver presses: a queue of (keys, frames held, frames released after)
typedef struct { u32 keys; int hold, gap; } Press;
static Press s_press[32];
static int s_npress, s_press_f;
static float s_pad[2], s_pad_t;
static u8 s_flags_prev[EV_MAX_FLAGS / 8], s_items_prev[256];
static bool s_have_prev;
static char s_prev_map[8], s_prev_prog[40];
static int s_prev_stage = -1, s_prev_state = -1;
static int s_changes;                    // anything the game did (flag, item, room, message, program)
static int s_auto_f;                     // frames since the last automatic button
static bool s_warped;                    // "ev" already warped to the trigger's room
static float s_aim[2];                   // the point a look trigger is aimed at
static bool s_aim_on;
static int s_ev_stage;                   // the stage an "ev" started in

void playtest_init(const PtHost* host) { H = host; }
bool playtest_active(void) { return s_active; }
static bool s_fast;
bool playtest_fast(void) { return s_fast; }

void playtest_log(const char* fmt, ...)
{
	if (!s_log) {
		s_log = fopen(LOG_PATH, "w");
		if (!s_log)
			return;
	}
	fprintf(s_log, "%7.1f ", s_t);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(s_log, fmt, ap);
	va_end(ap);
	fputc('\n', s_log);
	fflush(s_log);
}

bool playtest_line(const char* line)
{
	if (strncmp(line, "pt ", 3))
		return false;
	if (s_ncmd < MAX_CMDS) {
		snprintf(s_cmd[s_ncmd].text, sizeof s_cmd[s_ncmd].text, "%s", line + 3);
		char* nl = strpbrk(s_cmd[s_ncmd].text, "\r\n");
		if (nl)
			*nl = 0;
		s_ncmd++;
		s_active = true;
	}
	return true;
}

void playtest_message(const char* text)
{
	if (!s_active || !text)
		return;
	char line[100];
	int n = 0;
	for (const char* c = text; *c && n < 90; c++)
		line[n++] = (*c == '\n' || *c == '\f') ? '/' : (u8)*c < 32 ? '?' : *c;
	line[n] = 0;
	playtest_log("msg  \"%s\"", line);
	s_changes++;
}

static void result(bool ok, const char* what)
{
	playtest_log("%s %s", ok ? "PASS" : "FAIL", what);
	if (ok)
		s_pass++;
	else
		s_fail++;
	if (!ok && s_stop_on_fail)
		s_pc = s_ncmd;
}

static void press(u32 keys, int hold, int gap)
{
	if (s_npress < 32)
		s_press[s_npress++] = (Press){ keys, hold, gap };
}

static u32 parse_keys(const char* s)
{
	static const struct { const char* n; u32 k; } K[] = {
		{ "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
		{ "ZL", KEY_ZL }, { "ZR", KEY_ZR }, { "START", KEY_START }, { "SELECT", KEY_SELECT },
		{ "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
	};
	u32 k = 0;
	char buf[64];
	snprintf(buf, sizeof buf, "%s", s);
	for (char* t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
		for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++)
			if (!strcmp(t, K[i].n))
				k |= K[i].k;
	return k;
}

static bool idle(const PtState* st)
{
	return st->state == PT_GAME && !st->loading && !st->message && !st->save_prompt && !st->prog &&
	       !st->demo && !st->picture && !st->inv && !st->paused && !st->screens && s_npress == 0;
}

// While waiting: close messages, skip scenes, answer choices, cancel save prompts, leave the
// results screen. Returns true when it pressed something.
static void auto_advance(const PtState* st)
{
	if (s_npress || ++s_auto_f < 10)
		return;
	if (st->state == PT_RESULTS) {
		// the results go on with START; the clear data question and slot screen are left with B
		// (scripts that want the clear data saved use "press")
		static int alt;
		press(alt++ & 1 ? KEY_B : KEY_START, 2, 8);
	} else if (st->state != PT_GAME) {
		return;
	} else if (st->demo) {
		if (s_skip)
			press(KEY_START, 2, 20);
	} else if (st->message) {
		press(KEY_A, 2, 6);
	} else if (st->save_prompt) {
		press(KEY_B, 2, 6);
	} else if (st->paused) {
		press(KEY_START, 2, 6);
	} else if (st->prog && prog_choice_waiting()) {
		int k = 0;
		if (s_nanswers) {
			k = s_answers[0];
			memmove(s_answers, s_answers + 1, --s_nanswers * sizeof s_answers[0]);
		}
		playtest_log("answer %d of %d", k, prog_choice_waiting());
		for (int i = 0; i < k; i++)
			press(KEY_DDOWN, 2, 4);
		press(KEY_A, 2, 6);
	} else if (st->picture && st->prog && !st->panel) {
		press(KEY_A, 2, 6);          // a still waiting for a button
	} else {
		return;
	}
	s_auto_f = 0;
}

// What changed since the last frame, into the log.
static void watch(const PtState* st)
{
	u8 flags[EV_MAX_FLAGS / 8];
	memcpy(flags, ev_flag_block(), sizeof flags);
	u8 items[256];
	for (int i = 0; i < 256; i++)
		items[i] = (u8)ev_item_count(i);
	if (s_have_prev) {
		int n = 0;
		for (int i = 0; i < 1000 && n < 24; i++) {     // game flags; the counters above 8000 are not logged
			if (flags[i] == s_flags_prev[i])
				continue;
			for (int b = 0; b < 8; b++)
				if ((flags[i] ^ s_flags_prev[i]) >> b & 1) {
					playtest_log("flag %c%d", flags[i] >> b & 1 ? '+' : '-', i * 8 + b);
					n++;
					s_changes++;
				}
		}
		for (int i = 1; i < 256; i++)
			if (items[i] != s_items_prev[i]) {
				const char* name = ev_item_name(i);
				char nm[32];
				snprintf(nm, sizeof nm, "%s", name ? name : "?");
				char* nl = strchr(nm, '\n');
				if (nl)
					*nl = 0;
				playtest_log("item %d (%s) %d -> %d", i, nm, s_items_prev[i], items[i]);
				s_changes++;
			}
	}
	memcpy(s_flags_prev, flags, sizeof flags);
	memcpy(s_items_prev, items, sizeof items);
	s_have_prev = true;
	if (st->state != s_prev_state) {
		static const char* const names[] = { "title", "game", "other", "results" };
		playtest_log("state %s", names[st->state & 3]);
		s_prev_state = st->state;
		s_changes++;
	}
	if (st->state == PT_GAME && (st->stage != s_prev_stage || strcmp(st->map, s_prev_map))) {
		playtest_log("room stage %d %s (%.0f %.0f %.0f)", st->stage, st->map, st->pos[0], st->pos[1], st->pos[2]);
		s_prev_stage = st->stage;
		snprintf(s_prev_map, sizeof s_prev_map, "%s", st->map);
		s_changes++;
	}
	const char* pr = prog_debug_status();
	char key[40];
	snprintf(key, sizeof key, "%s", pr);
	char* step = strstr(key, " step");     // log a program when it starts, not every step
	if (step)
		*step = 0;
	if (strcmp(key, s_prev_prog)) {
		if (strcmp(key, "-"))
			playtest_log("prog %s", key);
		snprintf(s_prev_prog, sizeof s_prev_prog, "%s", key);
		s_changes++;
	}
}

// Where to stand for event i: inside its area, or in front of the point or line it looks at.
static bool event_pose(const EvInfo* e, const PtState* st, float pos[3], float* yaw)
{
	*yaw = st->yaw;
	float y_ref = e->any_height ? st->pos[1] : e->y;
	pos[1] = y_ref;
	if (e->shape == 3 || e->shape == 4) {          // rect (min, max) or convex polygon
		float cx = 0, cz = 0;
		int n = e->shape == 3 ? 2 : e->npts;
		for (int i = 0; i < n; i++)
			cx += e->pts[i][0], cz += e->pts[i][1];
		cx /= n, cz /= n;
		pos[0] = cx, pos[2] = cz;
		float y;
		if (H->floor && H->floor(pos[0], pos[2], y_ref, e->any_height ? 100000.0f : 600.0f, &y))
			pos[1] = y;
		if (e->any_height || (pos[1] >= e->y - 100.0f && pos[1] <= e->y + 500.0f))
			return true;
		// on stairs the middle may be out of the height window: points between the middle and
		// the corners (inside a rect and a convex polygon alike)
		int nc = e->shape == 3 ? 4 : e->npts;
		for (int k = 0; k < nc; k++)
			for (int t = 1; t <= 4; t++) {
				float vx = e->shape == 3 ? e->pts[k & 1][0] : e->pts[k][0];
				float vz = e->shape == 3 ? e->pts[k >> 1][1] : e->pts[k][1];
				float px = cx + (vx - cx) * t * 0.2f, pz = cz + (vz - cz) * t * 0.2f;
				if (H->floor && H->floor(px, pz, e->y + 200.0f, 400.0f, &y) && y >= e->y - 100.0f &&
				    y <= e->y + 500.0f) {
					pos[0] = px, pos[1] = y, pos[2] = pz;
					return true;
				}
			}
		return true;
	}
	// look tests: candidates around the point, or in front of each segment's middle
	float best = 1e9f;
	bool found = false;
	int nseg = e->shape == 1 ? 1 : e->npts - 1;
	for (int s = 0; s < nseg; s++) {
		for (int k = 0; k < 8; k++) {
			float d = k < 4 ? 160.0f : 280.0f, tx, tz, px, pz;
			if (e->shape == 1) {
				float a = (float)(k & 3) * (float)M_PI / 2.0f + (k >= 4 ? (float)M_PI / 4.0f : 0.0f);
				tx = e->pts[0][0], tz = e->pts[0][1];
				px = tx + sinf(a) * d, pz = tz + cosf(a) * d;
			} else {
				const float* a = e->pts[s];
				const float* b = e->pts[s + 1];
				float bx = b[0] - a[0], bz = b[1] - a[1], len = sqrtf(bx * bx + bz * bz);
				if (len < 1.0f)
					continue;
				float t = (k & 3) == 0 ? 0.5f : (k & 3) == 1 ? 0.3f : (k & 3) == 2 ? 0.7f : 0.5f;
				tx = a[0] + bx * t, tz = a[1] + bz * t;
				float nx = -bz / len, nz = bx / len;   // the front side: cross(B - A, P - A) >= 0
				float dd = (k & 3) == 3 ? 100.0f : d;
				px = tx + nx * dd, pz = tz + nz * dd;
			}
			float y = y_ref;
			bool fl = H->floor && H->floor(px, pz, y_ref, e->any_height ? 100000.0f : 600.0f, &y);
			bool hok = e->any_height || (e->y >= y - 500.0f && e->y <= y + 100.0f);
			float score = (fl ? 0.0f : 1000.0f) + (hok ? 0.0f : 500.0f) + (k >= 4 ? 1.0f : 0.0f) + s * 0.01f;
			if (score < best) {
				best = score;
				found = true;
				pos[0] = px, pos[1] = y, pos[2] = pz;
				*yaw = atan2f(tx - px, tz - pz);
				s_aim[0] = tx, s_aim[1] = tz;
				s_aim_on = true;
			}
		}
	}
	return found;
}

static bool copy_file(const char* from, const char* to)
{
	FILE* a = fopen(from, "rb");
	if (!a)
		return false;
	FILE* b = fopen(to, "wb");
	if (!b) {
		fclose(a);
		return false;
	}
	static u8 buf[8192];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof buf, a)) > 0)
		ok = fwrite(buf, 1, n, b) == n && ok;
	fclose(a);
	ok = fclose(b) == 0 && ok;
	return ok;
}

static void next(void)
{
	s_pc++;
	s_phase = 0;
	s_frames = 0;
	s_cmd_t = 0.0f;
	s_warped = false;
}

// One command; returns when it needs more frames.
static void step(const PtState* st)
{
	if (s_pc >= s_ncmd) {
		if (!s_quit) {
			s_quit = true;
			playtest_log("# %d passed, %d failed", s_pass, s_fail);
			playtest_log("# end of script");
			if (H->quit)
				H->quit();
		}
		return;
	}
	char op[24] = "";
	const char* c = s_cmd[s_pc].text;
	sscanf(c, "%23s", op);
	const char* arg = c + strlen(op);
	while (*arg == ' ')
		arg++;
	int a = 0, b = 0;
	float f[5] = { 0 };
	char word[32] = "";
	if (s_phase == 0 && s_frames == 0)
		playtest_log("> %s", c);
	s_frames++;

	// commands that do not wait for the game
	if (!strcmp(op, "log")) { next(); return; }
	if (!strcmp(op, "flag") && sscanf(arg, "%d %d", &a, &b) == 2) { ev_flag_set(a, b); next(); return; }
	if (!strcmp(op, "item") && sscanf(arg, "%d %d", &a, &b) == 2) { ev_give_item(a, b - ev_item_count(a)); next(); return; }
	if (!strcmp(op, "hp") && sscanf(arg, "%d", &a) == 1) { combat_set_hp((float)a); next(); return; }
	if (!strcmp(op, "fast")) { s_fast = atoi(arg) != 0; next(); return; }
	if (!strcmp(op, "god")) { s_god = atoi(arg) != 0; next(); return; }
	if (!strcmp(op, "noenemies")) { s_noenemies = atoi(arg) != 0; next(); return; }
	if (!strcmp(op, "autoskip")) { s_skip = atoi(arg) != 0; next(); return; }
	if (!strcmp(op, "stoponfail")) { s_stop_on_fail = atoi(arg) != 0; next(); return; }
	if (!strcmp(op, "answer")) {
		s_nanswers = 0;
		for (const char* p = arg; *p && s_nanswers < 16;) {
			s_answers[s_nanswers++] = atoi(p);
			while (*p && *p != ' ') p++;
			while (*p == ' ') p++;
		}
		next();
		return;
	}
	if (!strcmp(op, "press")) {
		char keys[64] = "";
		int hold = 2;
		sscanf(arg, "%63s %d", keys, &hold);
		press(parse_keys(keys), hold, 6);
		next();
		return;
	}
	if (!strcmp(op, "attack")) {
		// "attack N": N blows with the weapon in hand (R held to ready it, A to swing or fire)
		int n = 1;
		sscanf(arg, "%d", &n);
		for (int k = 0; k < n && k < 8; k++) {
			press(KEY_R, 20, 0);
			press(KEY_R | KEY_A, 2, 0);
			press(KEY_R, 40, 0);
		}
		press(0, 1, 10);
		next();
		return;
	}
	if (!strcmp(op, "walk") && sscanf(arg, "%f %f %f", &f[0], &f[1], &f[2]) == 3) {
		if (s_phase == 0) {
			s_pad[0] = f[0], s_pad[1] = f[1], s_pad_t = f[2];
			s_phase = 1;
		}
		if (s_pad_t <= 0.0f)
			next();
		return;
	}
	if (!strcmp(op, "wait") && sscanf(arg, "%f", &f[0]) == 1) {
		if (s_cmd_t >= f[0])
			next();
		return;
	}
	if (!strcmp(op, "prog")) { prog_debug_line(arg); next(); return; }   // e.g. "pt prog clock 545"
	if (!strcmp(op, "shot")) { if (H->shot) H->shot(); next(); return; }
	if (!strcmp(op, "newgame") || !strcmp(op, "wish")) {
		if (H->new_game) H->new_game(!strcmp(op, "wish"));
		next();
		return;
	}
	if (!strcmp(op, "load")) {
		char from[96];
		snprintf(from, sizeof from, PACK_DIR "/pt/%.40s.sav", arg);
		bool ok = copy_file(from, PACK_DIR "/save.dat") && H->load && H->load();
		char what[192];
		snprintf(what, sizeof what, "load %s", arg);
		result(ok, what);
		next();
		return;
	}
	if (!strcmp(op, "kill")) {
		// every living enemy takes a large hit each frame until none is left (bosses go through
		// their own death code)
		float limit = 20.0f, radius = 4000.0f;
		sscanf(arg, "%f %f", &limit, &radius);
		int alive = 0;
		if (s_frames == 1)
			for (int i = 0; i < enemy_count(); i++) {
				Enemy* e = enemy_get(i);
				if (e)
					playtest_log("enemy kind %d id %d status %d mlv %d hp %.0f at %.0f away%s", e->kind, e->list_id,
					             e->status, e->mlv, e->hp, hypotf(e->x - st->pos[0], e->z - st->pos[2]),
					             enemy_alive(e) ? "" : " (dead)");
			}
		for (int i = 0; i < enemy_count(); i++) {
			Enemy* e = enemy_get(i);
			if (e && e->used && hypotf(e->x - st->pos[0], e->z - st->pos[2]) < radius &&
			    (enemy_alive(e) || (e->hp > 0.0f && !e->dead_flag && e->kind != EN_KIND_RED))) {
				alive++;             // (waiting ones, like a Flesh Lip on the ceiling, count until they come)
				if (!enemy_alive(e))
					continue;
				if (s_frames % 10 == 1)
					enemy_damage(e, 1, 200.0f, 200.0f, 0.0f, 1.0f);
			}
		}
		auto_advance(st);
		if (combat_held() && !s_npress) {
			press(KEY_A, 1, 1);          // held by an enemy: struggle free as a player does
			press(KEY_B, 1, 1);
		}
		static int dead_frames;
		dead_frames = alive || s_frames == 1 ? 0 : dead_frames + 1;
		if (dead_frames > 90 || s_cmd_t > limit) {          // a few seconds for the deaths to play out
			char what[64];
			snprintf(what, sizeof what, "kill: %d left", alive);
			result(alive == 0, what);
			next();
		}
		return;
	}
	if (!strcmp(op, "expect")) {
		char what[160];
		bool ok = false;
		sscanf(arg, "%31s", word);
		const char* rest = arg + strlen(word);
		if (!strcmp(word, "stage") && sscanf(rest, "%d", &a) == 1)
			ok = st->stage == a && st->state == PT_GAME;
		else if (!strcmp(word, "map") && sscanf(rest, " %31s", word) == 1)
			ok = !strcmp(st->map, word) && st->state == PT_GAME;
		else if (!strcmp(word, "flag") && sscanf(rest, "%d %d", &a, &b) == 2)
			ok = ev_flag(a) == (b != 0);
		else if (!strcmp(word, "item") && sscanf(rest, "%d %d", &a, &b) >= 1)
			ok = b ? ev_item_count(a) >= b : ev_item_count(a) == 0;
		else if (!strcmp(word, "evon") && sscanf(rest, "%d %d", &a, &b) == 2) {
			EvInfo e;
			ok = ev_info(a, &e) && e.enabled == (b != 0);
		} else if (!strcmp(word, "title"))
			ok = st->state == PT_TITLE;
		else if (!strcmp(word, "results"))
			ok = st->state == PT_RESULTS;
		else if (!strcmp(word, "ending") && sscanf(rest, "%i", &a) == 1) {
			u8 cl[3];
			prog_clear_get(cl);
			ok = (cl[1] & a) == a;
		} else if (!strcmp(word, "alive"))
			ok = !st->dead && st->state == PT_GAME;
		snprintf(what, sizeof what, "expect %s   [stage %d %s, prog %s]", arg, st->stage, st->map,
		         prog_debug_status());
		result(ok, what);
		next();
		return;
	}

	// the rest waits for the game to settle first
	float limit = 90.0f;
	if (!strcmp(op, "idle"))
		sscanf(arg, "%f", &limit);
	else if (!strcmp(op, "until"))
		sscanf(arg, !strncmp(arg, "flag", 4) ? "%*s %*d %*d %f" : !strncmp(arg, "panel", 5) ? "%*s %f" : "%*s %*d %f", &limit);
	if (s_phase == 0) {
		auto_advance(st);
		if (combat_held() && !s_npress && st->state == PT_GAME) {
			press(KEY_A, 1, 1);
			press(KEY_B, 1, 1);
		}
		bool title_ok = !strcmp(op, "idle") && strstr(arg, "title") && st->state == PT_TITLE;
		if (!strcmp(op, "until")) {
			if (sscanf(arg, "%31s %d %d", word, &a, &b) == 3 && !strcmp(word, "flag") && ev_flag(a) == (b != 0)) {
				result(true, c);
				next();
			} else if (sscanf(arg, "%31s %d", word, &a) == 2 && !strcmp(word, "stage") && st->stage == a &&
			           idle(st)) {
				result(true, c);
				next();
			} else if (!strcmp(word, "panel") && st->panel && !st->message && !s_npress) {
				next();                // a puzzle or choice panel is up: the script plays it
			} else if (s_cmd_t > limit) {
				result(false, c);
				next();
			}
			return;
		}
		if (!idle(st) && !title_ok) {
			if (s_cmd_t > limit) {
				char what[128];
				snprintf(what, sizeof what, "%s: not idle after %.0f s (prog %s, msg %d demo %d load %d pic %d panel %d)", c,
				         limit, prog_debug_status(), st->message, st->demo, st->loading, st->picture, st->panel);
				result(false, what);
				if (st->prog) {
					prog_abort();       // a stuck program: end it so the rest of the script can run
					playtest_log("program aborted");
				}
				next();
			}
			return;
		}
		if (!strcmp(op, "idle")) {
			next();
			return;
		}
		s_phase = 1;
		s_frames = 0;
	}

	// "ev N": stand at event N's trigger, press A / use its item, wait until the game is idle again;
	// "evgo N": the same but returns as soon as something happened (a puzzle the script then plays)
	bool go_only = !strcmp(op, "evgo");
	if ((!strcmp(op, "ev") || go_only) && sscanf(arg, "%d", &a) == 1) {
		EvInfo e;
		if (s_phase == 1) {
			if (s_warped && st->stage != s_ev_stage) {
				playtest_log("ev %d ran on arrival", a);   // a door where the warp put James
				s_phase = 3;
				s_frames = 0;
				return;
			}
			if (!s_warped)
				s_ev_stage = st->stage;
			if (!ev_info(a, &e)) {
				result(false, "ev: no such event");
				next();
				return;
			}
			if (!e.enabled && s_warped && s_changes) {
				playtest_log("ev %d ran on arrival", a);   // an enter trigger where the warp put James
				s_phase = 3;
				s_frames = 0;
				return;
			}
			if (!e.enabled) {
				char what[192];
				snprintf(what, sizeof what, "ev %d (stage %d): its flags do not pass", a, st->stage);
				result(false, what);
				next();
				return;
			}
			float pos[3], yaw;
			s_aim_on = false;
			event_pose(&e, st, pos, &yaw);
			if (e.kind >= 2 && e.map[0] && strcmp(e.map, st->map) && H->warp && !s_warped) {
				s_warped = true;
				s_changes = 0;
				playtest_log("ev %d is in %s: warping there", a, e.map);
				H->warp(st->stage, e.map, pos, yaw);
				s_phase = 0;          // wait until idle, then place again
				s_frames = 1;
				return;
			}
			if (e.kind >= 2 && H->place)
				H->place(pos, yaw);
			playtest_log("ev %d kind %d result %d prog %d at (%.0f %.0f %.0f) yaw %.0f", a, e.kind, e.result, e.prog,
			             pos[0], pos[1], pos[2], yaw * 57.2958f);
			s_changes = 0;
			s_phase = 2;
			s_frames = 0;
			return;
		}
		if (s_phase == 2) {
			// a frame for the room and the edge test, then the button or the item
			if (s_frames < 3)
				return;
			if (st->stage != s_ev_stage) {          // an enter trigger already took James elsewhere
				s_phase = 3;
				s_frames = 0;
				return;
			}
			if (s_frames == 3 && s_aim_on && H->place) {
				// the walls may have pushed him: face the trigger again from where he stands
				float p2[3] = { st->pos[0], st->pos[1], st->pos[2] };
				H->place(p2, atan2f(s_aim[0] - p2[0], s_aim[1] - p2[2]));
				return;
			}
			ev_info(a, &e);
			if ((e.kind == 5 || e.kind == 6) && !ev_have_item(e.item) && !ev_combo_owned(e.item)) {
				char what[192];
				snprintf(what, sizeof what, "ev %d (stage %d): James does not have item %d", a, st->stage, e.item);
				result(false, what);
				next();
				return;
			}
			if (e.kind == 5 || e.kind == 6) {
				if (H->use_item)
					H->use_item(e.item);
				s_changes = 0;
			} else if (e.kind == 2 || e.kind == 7 || e.kind == 10) {
				press(KEY_A, 2, 2);
				s_changes = 0;
			}
			s_phase = 3;
			s_frames = 0;
			return;
		}
		if (s_phase == 3) {
			if (st->state == PT_TITLE && s_changes) {
				next();                 // an ending: back on the title
				return;
			}
			if (s_frames < 30 && idle(st) && !s_changes)
				return;
			if (s_frames >= 30 && idle(st) && !s_changes) {
				char what[192];
				snprintf(what, sizeof what, "ev %d (stage %d): nothing happened (James %.0f %.0f %.0f yaw %.0f, %s)", a,
				         st->stage, st->pos[0], st->pos[1], st->pos[2], st->yaw * 57.2958f, combat_status());
				result(false, what);
				next();
				return;
			}
			if (go_only && (s_changes || !idle(st))) {
				next();
				return;
			}
			auto_advance(st);
			if (idle(st) && s_frames > 4) {
				next();
				return;
			}
			if (s_cmd_t > limit) {
				char what[128];
				snprintf(what, sizeof what, "ev %d: not idle after %.0f s (prog %s)", a, limit, prog_debug_status());
				result(false, what);
				next();
			}
			return;
		}
	}
	if (!strcmp(op, "at") && sscanf(arg, "%f %f %f %f", &f[0], &f[1], &f[2], &f[3]) == 4) {
		float pos[3] = { f[0], f[1], f[2] };
		if (H->place)
			H->place(pos, f[3] * (float)M_PI / 180.0f);
		next();
		return;
	}
	if (!strcmp(op, "goto")) {
		char map[8] = "";
		a = st->stage;
		if (s_phase == 1) {
			if (sscanf(arg, "%7s %f %f %f %f %d", map, &f[0], &f[1], &f[2], &f[3], &a) >= 5 && H->warp) {
				float pos[3] = { f[0], f[1], f[2] };
				H->warp(a, map, pos, f[3] * (float)M_PI / 180.0f);
			}
			s_phase = 2;
			s_frames = 0;
			return;
		}
		auto_advance(st);
		if (s_frames > 4 && idle(st))
			next();
		else if (s_cmd_t > limit) {
			result(false, c);
			next();
		}
		return;
	}
	// "combine A B [C]": through the item list as a player does it: Y opens it, the D-pad goes to
	// each item, R marks the first ones, A uses the last
	if (!strcmp(op, "combine")) {
		int it[3] = { 0, 0, 0 }, n = sscanf(arg, "%d %d %d", &it[0], &it[1], &it[2]);
		if (s_phase == 1) {
			int cur = 0;
			press(KEY_Y, 2, 6);
			for (int k = 0; k < n; k++) {
				int row = H->inv_index ? H->inv_index(it[k]) : -1;
				if (row < 0) {
					char what[64];
					snprintf(what, sizeof what, "combine: James does not have item %d", it[k]);
					result(false, what);
					s_npress = 0;
					next();
					return;
				}
				for (; cur < row; cur++)
					press(KEY_DDOWN, 2, 4);
				for (; cur > row; cur--)
					press(KEY_DUP, 2, 4);
				press(k + 1 < n ? KEY_R : KEY_A, 2, 6);
			}
			s_phase = 2;
			s_frames = 0;
			s_changes = 0;
			return;
		}
		auto_advance(st);
		if (s_frames > 10 && idle(st))
			next();
		return;
	}
	if (!strcmp(op, "use") && sscanf(arg, "%d", &a) == 1) {
		if (s_phase == 1) {
			if (H->use_item)
				H->use_item(a);
			s_phase = 2;
			s_frames = 0;
			return;
		}
		auto_advance(st);
		if (s_frames > 4 && idle(st))
			next();
		return;
	}
	if (!strcmp(op, "save")) {
		mkdir(PACK_DIR "/pt", 0777);
		char to[96], what[96];
		snprintf(to, sizeof to, PACK_DIR "/pt/%.40s.sav", arg);
		bool ok = H->save && H->save() && copy_file(PACK_DIR "/save.dat", to);
		snprintf(what, sizeof what, "save %s", arg);
		result(ok, what);
		next();
		return;
	}
	playtest_log("?? unknown command: %s", c);
	next();
}

void playtest_frame(const PtState* st, u32* down, u32* held, float dt)
{
	if (!s_active)
		return;
	s_t += dt;
	s_cmd_t += dt;
	watch(st);
	if (s_god && st->state == PT_GAME)
		combat_set_hp(combat_hp_max());
	if (s_noenemies && st->state == PT_GAME)
		enemy_clear();
	if (st->dead && st->state == PT_GAME) {
		static bool said;
		if (!said)
			playtest_log("James died");
		said = true;
	}
	if (!s_quit)
		step(st);
	// the buttons: the script's presses replace the player's
	*down = *held = 0;
	if (s_npress) {
		Press* p = &s_press[0];
		if (s_press_f < p->hold) {
			*held = p->keys;
			if (s_press_f == 0)
				*down = p->keys;
		}
		if (++s_press_f >= p->hold + p->gap) {
			memmove(s_press, s_press + 1, --s_npress * sizeof s_press[0]);
			s_press_f = 0;
		}
	}
	if (s_pad_t > 0.0f)
		s_pad_t -= dt;
}

void playtest_pad(circlePosition* cp)
{
	if (!s_active)
		return;
	if (s_pad_t > 0.0f) {
		cp->dx = (s16)(s_pad[0] * 156.0f);
		cp->dy = (s16)(s_pad[1] * 156.0f);
	} else {
		cp->dx = cp->dy = 0;
	}
}
