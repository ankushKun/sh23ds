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
#include "playnav.h"
#include "stagewalls.h"
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
static int s_story;                      // the same without room changes (walking over tiles)
static int s_auto_f;                     // frames since the last automatic button
static bool s_warped;                    // "ev" already warped to the trigger's room
static float s_aim[2];                   // the point a look trigger is aimed at
static bool s_aim_on;
static int s_ev_stage;                   // the stage an "ev" started in
static int s_target_stage = -1;          // the stage whose event an "ev" is about (walking may leave it)

// ---------------------------------------------------------------- walking ("walk 1")
// With walk mode on, "ev" walks James to the trigger with the Circle Pad (playnav.c picks the way
// over the loaded collision) and takes the doors on the way (a breadth-first search over the
// stage's door events that pass their flags now) instead of warping and placing him.
static bool s_walkmode;
static bool s_walkdebug;               // log every plan's points and James' track
static float s_dt = 1.0f / 30.0f;      // this frame's game time
typedef struct {
	float goal[3], face, wp[128], since, best;
	int nwp, cur, replans, turn_t;
	bool partial, noway;
	float near;                       // near enough to the spot (a door's look test reaches 400)
	float wiggle, stuck_x, stuck_z;   // stuck at one spot again and again: a step back first
	int stuck_n;
	float route[3 * 256], lg[3];      // the area route (open areas) and the leg's goal
	int nroute, ri;
} Walk;
static Walk W;
static float s_wpad[2];
static bool s_wpad_on;
static u32 s_wheld;
static int s_route_door, s_doors_taken;
static bool s_walked;                    // the ev's James got there on foot (no placing)
static bool s_on_way;                    // another trigger stopped the walk short of the ev's own
static int s_rewalks, s_retries;
static int s_story_at;                   // s_story when the walk began

static void node_of(const char* map, char out[8]);
static bool open_area(const char* map);
static char s_walk_goal_map[8];          // the map the walk's goal is in ("" not known)

// An open-area route over the tiles of James' own folder, else of the goal's when that is another
// open area's (the courtyard of Wood Side is the town's, the gate tile ca23 the forest's), else of
// both ("ca,cb"; together their overlapping tiles can wall each other in, so last). used: the
// folders of the route found, or James' own when there is none.
static int area_route_any(const char* map, const char* goal_map, const float s[3], const float g[3], float* out, int max,
                          char used[8])
{
	snprintf(used, 8, "%.2s", map);
	int n = nav_area_route(used, s, g, out, max);
	if (n > 0 || !goal_map || !goal_map[0] || !open_area(goal_map) || !strncmp(map, goal_map, 2))
		return n;
	char f[8];
	snprintf(f, sizeof f, "%.2s", goal_map);
	if ((n = nav_area_route(f, s, g, out, max)) > 0) {
		memcpy(used, f, sizeof f);
		return n;
	}
	bool first = strncmp(map, goal_map, 2) < 0;
	snprintf(f, sizeof f, "%.2s,%.2s", first ? map : goal_map, first ? goal_map : map);
	if ((n = nav_area_route(f, s, g, out, max)) > 0)
		memcpy(used, f, sizeof f);
	return n;
}

static bool walk_plan(const PtState* st)
{
	const Collision* c = H->collision ? H->collision() : NULL;
	W.nwp = c ? nav_plan(c, st->pos, W.lg, W.wp, 64, &W.partial) : 0;
	W.cur = 0;
	W.since = 0.0f;
	W.best = 1e9f;
	if (s_walkdebug) {
		char b[600];
		int k = 0;
		const float* ys = nav_last_heights();
		for (int i = 0; i < W.nwp && k < 560; i++)
			k += snprintf(b + k, sizeof b - k, " (%.0f %.0f %.0f)", W.wp[2 * i], ys[i], W.wp[2 * i + 1]);
		playtest_log("  plan from (%.0f %.0f %.0f):%s%s", st->pos[0], st->pos[1], st->pos[2], b, W.partial ? " partial" : "");
	}
	return W.nwp > 0;
}

static bool walk_begin(const PtState* st, const float goal[3], float face)
{
	memcpy(W.goal, goal, sizeof W.goal);
	memcpy(W.lg, goal, sizeof W.lg);
	W.face = face;
	W.replans = 0;
	W.turn_t = 0;
	W.nroute = W.ri = 0;
	W.noway = false;
	W.near = 300.0f;
	W.wiggle = 0.0f, W.stuck_n = 0;
	char node[8];
	if (open_area(st->map) && hypotf(goal[0] - st->pos[0], goal[2] - st->pos[2]) > 4000.0f) {
		W.nroute = area_route_any(st->map, s_walk_goal_map, st->pos, goal, W.route, 256, node);
		if (W.nroute > 1)
			memcpy(W.lg, W.route, sizeof W.lg);
		W.noway = W.nroute <= 0;     // no way over the area's tiles: a door, not a long wander
	}
	bool ok = walk_plan(st);
	playtest_log("walk to (%.0f %.0f %.0f): %d legs, %d points%s", goal[0], goal[1], goal[2], W.nroute, W.nwp,
	             W.partial ? ", partial" : "");
	return ok;
}

// 1 there (and turned to face), -1 no way, 0 still walking
static int walk_update(const PtState* st, float dt)
{
	s_wpad_on = true;
	s_wpad[0] = s_wpad[1] = 0.0f;
	s_wheld = 0;
	if (W.noway) {
		playtest_log("walk: no way over the area to (%.0f %.0f %.0f)", W.goal[0], W.goal[1], W.goal[2]);
		return -1;
	}
	if (W.wiggle > 0.0f) {
		W.wiggle -= dt;                  // back off at an angle (a monster or a corner in the way)
		s_wpad[0] = (W.stuck_n & 1) ? 0.6f : -0.6f;
		s_wpad[1] = -1.0f;
		return 0;
	}
	float gx = W.lg[0] - st->pos[0], gz = W.lg[2] - st->pos[2], dg = sqrtf(gx * gx + gz * gz);
	if (fabsf(W.lg[1] - st->pos[1]) > 400.0f)
		dg += fabsf(W.lg[1] - st->pos[1]);   // another floor of the room: not there yet
	if (W.ri + 1 < W.nroute && dg < 400.0f) {
		// close to a leg's point (it may sit just behind a wall the way curves round): the next one
		W.ri++;
		memcpy(W.lg, W.route + 3 * W.ri, sizeof W.lg);
		W.replans = 0;
		walk_plan(st);
		return 0;
	}
	if (W.cur >= W.nwp && W.ri + 1 < W.nroute && (dg < 600.0f || !W.partial)) {
		W.ri++;                          // the next leg of the area route
		memcpy(W.lg, W.route + 3 * W.ri, sizeof W.lg);
		W.replans = 0;
		if (!walk_plan(st))
			return -1;
		return 0;
	}
	if (W.cur >= W.nwp) {
		if (W.ri + 1 >= W.nroute && dg < W.near && W.partial)
			W.partial = false;           // the walls keep him a little short of the spot: near enough
		if ((W.partial || dg > 120.0f) && !(W.ri + 1 >= W.nroute && dg < W.near)) {
			if (++W.replans > 40) {
				playtest_log("walk: no nearer to (%.0f %.0f %.0f) from (%.0f %.0f %.0f)", W.lg[0], W.lg[1], W.lg[2], st->pos[0],
				             st->pos[1], st->pos[2]);
				return -1;
			}
			bool ok = walk_plan(st);
			if (!ok || (W.partial && hypotf(W.wp[2 * W.nwp - 2] - st->pos[0], W.wp[2 * W.nwp - 1] - st->pos[2]) < 150.0f)) {
				if (W.ri + 1 < W.nroute) {
					W.ri++;                  // a leg's point in a gravestone or the like: the next one
					memcpy(W.lg, W.route + 3 * W.ri, sizeof W.lg);
					W.replans = 0;
					walk_plan(st);
					return 0;
				}
				playtest_log("walk: no way to leg %d/%d (%.0f %.0f %.0f) from (%.0f %.0f %.0f)", W.ri, W.nroute, W.lg[0],
				             W.lg[1], W.lg[2], st->pos[0], st->pos[1], st->pos[2]);
				return -1;
			}
			return 0;
		}
		if (W.face > 1e8f)               // no facing wanted
			return 1;
		if (s_aim_on)                    // short of the spot: face the look point from here
			W.face = atan2f(s_aim[0] - st->pos[0], s_aim[1] - st->pos[2]);
		float diff = remainderf(W.face - st->yaw, 2.0f * (float)M_PI);
		if (fabsf(diff) < 0.03f || ++W.turn_t > 150)
			return 1;                    // (a look test's 400-unit ray misses a short line at 0.1)
		float turn = fabsf(diff) > 0.4f ? 1.0f : fabsf(diff) > 0.1f ? 0.5f : 0.15f;
		s_wpad[0] = diff > 0 ? turn : -turn;     // positive x turns right, raising the yaw
		return 0;
	}
	if (s_walkdebug && (int)(s_t * 2.0f) != (int)((s_t - dt) * 2.0f))
		playtest_log("  at (%.0f %.0f %.0f) yaw %.0f, point %d", st->pos[0], st->pos[1], st->pos[2], st->yaw * 57.2958f, W.cur);
	float tx = W.wp[W.cur * 2] - st->pos[0], tz = W.wp[W.cur * 2 + 1] - st->pos[2], d = sqrtf(tx * tx + tz * tz);
	bool last = W.cur == W.nwp - 1;
	if (d < (last ? 60.0f : 130.0f)) {
		W.cur++;
		W.since = 0.0f;
		W.best = 1e9f;
		return 0;
	}
	if (d < W.best - 25.0f) {
		W.best = d;
		W.since = 0.0f;
	} else if ((W.since += dt) > 2.5f) {
		if (last && d < 250.0f && W.ri + 1 >= W.nroute) {
			W.cur = W.nwp;               // held off by a wall right at the spot: near enough
			return 0;
		}
		if (hypotf(st->pos[0] - W.stuck_x, st->pos[2] - W.stuck_z) < 60.0f) {
			if (++W.stuck_n % 3 == 0) {
				W.wiggle = 0.6f;
				W.since = 0.0f;
				return 0;
			}
		} else {
			W.stuck_x = st->pos[0], W.stuck_z = st->pos[2], W.stuck_n = 0;
		}
		if (++W.replans > 40 || !walk_plan(st)) {
			playtest_log("walk: stuck at (%.0f %.0f %.0f), no way on from there", st->pos[0], st->pos[1], st->pos[2]);
			return -1;
		}
		playtest_log("walk: stuck at (%.0f %.0f), new way of %d points", st->pos[0], st->pos[2], W.nwp);
		return 0;
	}
	float diff = remainderf(atan2f(tx, tz) - st->yaw, 2.0f * (float)M_PI);
	if (fabsf(diff) > 0.6f) {
		s_wpad[0] = diff > 0 ? 1.0f : -1.0f;      // turn on the spot (positive x turns right)
	} else {
		s_wpad[0] = fmaxf(-1.0f, fminf(1.0f, diff * 2.5f));
		s_wpad[1] = d < 250.0f ? 0.6f : 1.0f;
		if (d > 1500.0f && fabsf(diff) < 0.25f)
			s_wheld = KEY_B;                        // run on the long stretches
	}
	return 0;
}

// The open areas are one place for the routing (their tiles join without doors).
// (the areas' folders overlap: the town east tiles cover the Wood Side gate, whose own tile ca23
// is in the forest's folder, so all open areas are one place here; the walk plans on the tiles of
// James' own folder)
static void node_of(const char* map, char out[8])
{
	if (!strncmp(map, "ca", 2) || !strncmp(map, "cb", 2) || !strncmp(map, "cc", 2) || !strncmp(map, "cd", 2))
		snprintf(out, 8, "open");
	else
		snprintf(out, 8, "%s", map);
}

static bool open_area(const char* map) { return !strncmp(map, "ca", 2) || !strncmp(map, "cb", 2) || !strncmp(map, "cc", 2) || !strncmp(map, "cd", 2); }

// Open areas whose parts are joined by gates (the forest's graveyard): which doors to take, found
// by trying the area route from James to the goal, and from where each door leads to the goal.
static void door_spot(const EvInfo* d, const char* area, float out[3])
{
	float cx = 0, cz = 0;
	if (d->shape == 1 || d->shape == 2) {
		// in front of a line's middle (the side its look test wants)
		const float* a = d->pts[0];
		const float* b = d->pts[d->shape == 1 ? 0 : 1];
		cx = (a[0] + b[0]) * 0.5f, cz = (a[1] + b[1]) * 0.5f;
		float bx = b[0] - a[0], bz = b[1] - a[1], l = sqrtf(bx * bx + bz * bz);
		if (l > 1.0f)
			cx += -bz / l * 200.0f, cz += bx / l * 200.0f;
	} else {
		int n = d->shape == 3 ? 2 : d->npts;
		for (int i = 0; i < n; i++)
			cx += d->pts[i][0] / n, cz += d->pts[i][1] / n;
	}
	out[0] = cx, out[2] = cz, out[1] = d->y;
	float y;
	if (nav_area_floor(area, cx, cz, d->y, &y))
		out[1] = y;
}

static bool bad_door(int stage, int ev);
static int area_first_door(const PtState* st, const float goal[3], const char* goal_map)
{
	char area[8], fused[8];
	snprintf(area, sizeof area, "%.2s", st->map);     // the doors: on James' own tiles
	float route[3 * 8];
	if (area_route_any(st->map, goal_map, st->pos, goal, route, 8, fused) > 0)
		return -2;
	enum { NQ = 24 };
	float qpos[NQ][3];
	int qfirst[NQ], n = 0, head = 0;
	bool used[512] = { false };
	memcpy(qpos[0], st->pos, sizeof qpos[0]);
	qfirst[0] = -1, n = 1;
	int nev = ev_count() < 512 ? ev_count() : 512;
	while (head < n) {
		int cur = head++;
		if (cur > 0) {
			int nr = area_route_any(st->map, goal_map, qpos[cur], goal, route, 8, fused);
			playtest_log("  from (%.0f %.0f %.0f) to the goal: %s", qpos[cur][0], qpos[cur][1], qpos[cur][2], nr > 0 ? "yes" : "no");
			if (nr > 0)
				return qfirst[cur];
		}
		for (int i = 0; i < nev && n < NQ; i++) {
			EvInfo d;
			if (used[i] || !ev_info(i, &d) || !d.enabled || (d.result != 4 && d.result != 5) || d.dest_stage != st->stage ||
			    bad_door(st->stage, i))
				continue;
			if (!open_area(d.map) || (d.kind != 2 && d.kind != 3 && d.kind != 4))
				continue;
			float spot[3];
			door_spot(&d, area, spot);
			int nr = nav_area_route(area, qpos[cur], spot, route, 8);
			playtest_log("  door %d (%s -> %s) at (%.0f %.0f %.0f) from (%.0f %.0f): %s", i, d.map, d.dest_map, spot[0],
			             spot[1], spot[2], qpos[cur][0], qpos[cur][2], nr > 0 ? "reachable" : "no");
			if (nr <= 0)
				continue;
			used[i] = true;
			int first = qfirst[cur] < 0 ? i : qfirst[cur];
			if (!strcmp(d.dest_map, goal_map))
				return first;
			if (!open_area(d.dest_map))
				continue;            // leaves the area
			memcpy(qpos[n], d.dest_pos, sizeof qpos[n]);
			qfirst[n] = first;
			n++;
		}
	}
	return -1;
}

// The first door to take from James' room towards the map: its event index, -2 when he is there
// already, -1 when no door that passes its flags now leads there.
static bool s_door_last;                 // the walk across the area failed: reach the map by a door

// Doors James could not walk to in this command (a wall across the corridor between: ap50's north
// end): the routing leaves them out.
enum { NBAD = 16 };
static int s_bad_stage[NBAD], s_bad_ev[NBAD], s_bad_side[NBAD], s_nbad;   // side: led to the wrong side (kept)
static int s_in_stage, s_in_ev = -1;      // the door James came in by (a dead end when the walk then fails)
static bool bad_door(int stage, int ev)
{
	for (int i = 0; i < s_nbad; i++)
		if (s_bad_stage[i] == stage && s_bad_ev[i] == ev)
			return true;
	return false;
}

// goal: where in the map James is going (or NULL); after a failed walk across an open area, a door
// into the area counts as there when the area route from where it leads reaches the goal (the
// courtyard of Wood Side is walled in: cb68 by the door from the stairwell, not from the street)
static int route_first_door(const PtState* st, int stage_to, const char* map, const float* goal)
{
	char from[8], to[8];
	node_of(st->map, from);
	node_of(map, to);
	// (a walk inside the map that failed: the map is in parts, ap50's hallway with its barricade, so
	// out by a door and back in by another)
	bool again = s_door_last && stage_to == st->stage && !strcmp(st->map, map);
	if (stage_to == st->stage && !again && ((!strcmp(from, to) && !s_door_last) || !strcmp(st->map, map)))
		return -2;
	if (s_door_last)
		snprintf(to, sizeof to, "%s", map);   // the map itself, behind a door of its open area
	// breadth first over (stage, place), through the doors of this stage and of the stages they
	// lead to (the apartments' stairwell joins its floors through other stages' corridors)
	enum { NN = 160 };
	char node[NN][8];
	int nst[NN], via[NN], prev[NN], n = 0, head = 0;
	snprintf(node[n], 8, "%s", again ? "-" : from);   // "-": James' part of the map, not the goal
	nst[n] = st->stage, via[n] = -1, prev[n] = -1, n++;
	while (head < n) {
		int cur = head++;
		if (nst[cur] == stage_to && !strcmp(node[cur], to)) {
			while (prev[cur] > 0)
				cur = prev[cur];
			return via[cur];
		}
		int stage = nst[cur], nev = ev_count_of(stage);
		for (int i = 0; i < nev && n < NN; i++) {
			EvInfo e;
			if (!ev_info_of(stage, i, &e) || (e.result != 4 && e.result != 5))
				continue;
			if (s_walkdebug && cur == 0)
				playtest_log("  door %d of %d: %s -> %d:%s kind %d %s", i, stage, e.map, e.dest_stage, e.dest_map, e.kind,
				             e.enabled ? "open" : "flags do not pass");
			if (!e.enabled)
				continue;
			if (e.kind != 2 && e.kind != 3 && e.kind != 4 && e.kind != 7 && e.kind != 10)
				continue;
			if ((e.kind == 7 || e.kind == 10) && !ev_have_item(e.item))
				continue;
			if (bad_door(stage, i))
				continue;
			if (e.dest_stage >= 48 && e.dest_stage <= 52)
				continue;            // never through an ending
			char a[8], b[8];
			node_of(e.map, a);
			if (s_door_last && !strcmp(e.dest_map, to))
				snprintf(b, sizeof b, "%s", to);
			else
				node_of(e.dest_map, b);
			if (strcmp(a, cur == 0 && again ? from : node[cur]) || !b[0])
				continue;
			if (s_door_last && goal && e.dest_stage == stage_to && open_area(e.dest_map) && open_area(map)) {
				char fold[8];
				float r[3 * 8];
				bool hit = area_route_any(e.dest_map, map, e.dest_pos, goal, r, 8, fold) > 0;
				playtest_log("  door %d (%d:%s -> %s): %s", i, stage, e.map, e.dest_map, hit ? "leads there" : "not there");
				if (hit) {
					if (cur == 0)
						return i;
					int c = cur;
					while (prev[c] > 0)
						c = prev[c];
					return via[c];
				}
			}
			int k = 0;
			while (k < n && (strcmp(node[k], b) || nst[k] != e.dest_stage))
				k++;
			if (k < n)
				continue;
			snprintf(node[n], 8, "%s", b);
			nst[n] = e.dest_stage, via[n] = i, prev[n] = cur, n++;
		}
	}
	// none, in his own stage: rooms that join without a door (the labyrinth's ps66 / ps67): walk
	if (stage_to == st->stage && !s_door_last) {
		playtest_log("route to %d:%s: no door between, walking", stage_to, to);
		return -2;
	}
	// none: James may be shut in a part of his map (a cell of the prison's ps25): out through one of
	// its doors into another part of the same map, if there is one he has not tried
	for (int i = 0, nev = ev_count_of(st->stage); i < nev; i++) {
		EvInfo e;
		if (ev_info_of(st->stage, i, &e) && e.enabled && (e.result == 4 || e.result == 5) && !strcmp(e.map, st->map) &&
		    !strcmp(e.dest_map, st->map) && e.dest_stage == st->stage && (e.kind == 2 || e.kind == 3 || e.kind == 4) &&
		    !bad_door(st->stage, i)) {
			playtest_log("route to %d:%s: through the room's own door %d first", stage_to, to, i);
			return i;
		}
	}
	char list[400];
	int k = 0;
	for (int q = 0; q < n && k < 380; q++)
		k += snprintf(list + k, sizeof list - k, " %d:%s", nst[q], node[q]);
	playtest_log("route to %d:%s: none among%s", stage_to, to, list);
	return -1;
}

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
	s_story++;
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
				s_story++;
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
		s_story++;
	}
}

// Walls at the height of James' body (feet y, 900 tall) near a spot / across a line (walk mode picks
// the side of a look trigger he can stand at: ap64's sink is looked at over a low wall from the west).
static bool body_wall(const WallSeg* w, float y) { return w->group != 2 && w->y_top < y - 10.0f && w->y_bottom > y - 900.0f; }

static float wall_clearance(const Collision* c, float px, float pz, float y)
{
	float best = 1e9f;
	for (u32 i = 0, n = col_wall_count(c); i < n; i++) {
		const WallSeg* w = col_wall(c, i);
		if (!body_wall(w, y))
			continue;
		float dx = w->x1 - w->x0, dz = w->z1 - w->z0, l2 = dx * dx + dz * dz;
		float t = l2 > 1e-6f ? ((px - w->x0) * dx + (pz - w->z0) * dz) / l2 : 0.0f;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		float d = hypotf(px - w->x0 - dx * t, pz - w->z0 - dz * t);
		if (d < best)
			best = d;
	}
	return best;
}

static bool wall_across(const Collision* c, float ax, float az, float bx, float bz, float y)
{
	for (u32 i = 0, n = col_wall_count(c); i < n; i++) {
		const WallSeg* w = col_wall(c, i);
		if (!body_wall(w, y))
			continue;
		float rx = bx - ax, rz = bz - az, sx = w->x1 - w->x0, sz = w->z1 - w->z0, den = rx * sz - rz * sx;
		if (fabsf(den) < 1e-6f)
			continue;
		float t = ((w->x0 - ax) * sz - (w->z0 - az) * sx) / den, u = ((w->x0 - ax) * rz - (w->z0 - az) * rx) / den;
		if (t > 0.02f && t < 0.98f && u >= 0 && u <= 1)
			return true;
	}
	return false;
}

// A look trigger James can see from where he stands (the PC's look tests, 0x48b490 / 0x48b570, do
// not ask about walls: ap64's sink is looked at over its counter): the point to face, or false.
static bool look_from_here(const EvInfo* e, const PtState* st, float aim[2])
{
	if (e->shape == 1) {
		aim[0] = e->pts[0][0], aim[1] = e->pts[0][1];
		return hypotf(aim[0] - st->pos[0], aim[1] - st->pos[2]) < 360.0f;
	}
	if (e->shape != 2)
		return false;
	float best = 1e9f;
	for (int i = 0; i + 1 < e->npts; i++) {
		const float* a = e->pts[i];
		const float* b = e->pts[i + 1];
		float bx = b[0] - a[0], bz = b[1] - a[1], l2 = bx * bx + bz * bz;
		if (l2 < 1.0f || bx * (st->pos[2] - a[1]) - bz * (st->pos[0] - a[0]) < 0.0f)
			continue;             // behind it
		float t = ((st->pos[0] - a[0]) * bx + (st->pos[2] - a[1]) * bz) / l2;
		t = t < 0.1f ? 0.1f : t > 0.9f ? 0.9f : t;
		float px = a[0] + bx * t, pz = a[1] + bz * t, d = hypotf(px - st->pos[0], pz - st->pos[2]);
		if (d < best && d < 360.0f) {
			best = d;
			aim[0] = px, aim[1] = pz;
		}
	}
	return best < 1e8f;
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
	const Collision* col = s_walkmode && H->collision ? H->collision() : NULL;
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
			if (col && fl) {
				if (wall_across(col, px, pz, tx, tz, y))
					score += 300.0f;      // the thing is behind a wall from there
				if (wall_clearance(col, px, pz, y) < 120.0f)
					score += 100.0f;      // James cannot stand there
			}
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
	s_doors_taken = 0;
	s_on_way = false;
	s_rewalks = 0;
	s_retries = 0;
	s_nbad = 0;
	s_in_ev = -1;
	s_door_last = false;
	s_target_stage = -1;
	s_wpad_on = false;
	s_wheld = 0;
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
	// "riddle <levels> <command>": the command only on those riddle levels (0..3, comma separated),
	// for the puzzles whose answer the level decides (the keypad pin, the coins, the music box)
	if (!strcmp(op, "riddle")) {
		char levels[16] = "";
		int n = 0;
		if (sscanf(arg, "%15s %n", levels, &n) != 1 || !strchr(levels, '0' + prog_riddle_level())) {
			next();
			return;
		}
		c = arg + n;
		op[0] = '\0';
		sscanf(c, "%23s", op);
		arg = c + strlen(op);
		while (*arg == ' ')
			arg++;
	}
	int a = 0, b = 0;
	float f[5] = { 0 };
	char word[32] = "";
	if (s_phase == 0 && s_frames == 0)
		playtest_log("> %s", c);
	s_frames++;

	// commands that do not wait for the game
	if (!strcmp(op, "log")) { next(); return; }
	if (!strcmp(op, "where")) {   // logs where James stands (scans that walk him and look where he got)
		playtest_log("  at %.0f %.0f %.0f in %s", st->pos[0], st->pos[1], st->pos[2], st->map ? st->map : "-");
		next();
		return;
	}
	if (!strcmp(op, "flag") && sscanf(arg, "%d %d", &a, &b) == 2) { ev_flag_set(a, b); next(); return; }
	if (!strcmp(op, "item") && sscanf(arg, "%d %d", &a, &b) == 2) { ev_give_item(a, b - ev_item_count(a)); next(); return; }
	if (!strcmp(op, "hp") && sscanf(arg, "%d", &a) == 1) { combat_set_hp((float)a); next(); return; }
	if (!strcmp(op, "walkmode")) {
		s_walkmode = atoi(arg) != 0;
		stagewalls_all_tiles(s_walkmode);   // the planner sees the town's walls before James is near them
		next();
		return;
	}
	if (!strcmp(op, "walkdebug")) { s_walkdebug = atoi(arg) != 0; next(); return; }
	if (!strcmp(op, "fast")) { s_fast = atoi(arg) != 0; next(); return; }
	if (!strcmp(op, "god")) { s_god = atoi(arg) != 0; combat_test_god(s_god); next(); return; }
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
	if (!strcmp(op, "collect")) {
		// "collect [radius]": on foot to each item still lying within radius on James' floor (ammo,
		// health drinks and the rest a player picks up on the way), facing it and pressing A
		static bool tried[256];
		static int cur, before;
		float radius = 6000.0f;
		sscanf(arg, "%f", &radius);
		if (s_phase == 0 || s_phase == 1) {
			if (s_phase == 0)
				memset(tried, 0, sizeof tried);
			int best = -1;
			float bd = radius;
			float bp[3];
			for (int i = 0; i < ev_pickup_count() && i < 256; i++) {
				int it;
				float p[3];
				bool av;
				if (tried[i] || !ev_pickup_info(i, &it, p, &av) || !av || fabsf(p[1] - st->pos[1]) > 800.0f)
					continue;
				float d = hypotf(p[0] - st->pos[0], p[2] - st->pos[2]);
				if (d < bd)
					bd = d, best = i, memcpy(bp, p, sizeof bp);
			}
			if (best < 0) {
				next();
				return;
			}
			tried[best] = true;
			cur = best;
			int it;
			bool av;
			ev_pickup_info(best, &it, bp, &av);
			before = ev_item_count(it);
			s_aim[0] = bp[0], s_aim[1] = bp[2];
			s_aim_on = true;
			float goal[3] = { bp[0], st->pos[1], bp[2] };
			s_walk_goal_map[0] = 0;
			walk_begin(st, goal, 0.0f);
			W.face = atan2f(bp[0] - st->pos[0], bp[2] - st->pos[2]);
			playtest_log("collect: item %d (%s) at (%.0f %.0f %.0f)", it, ev_item_name(it) ? ev_item_name(it) : "?", bp[0], bp[1], bp[2]);
			s_phase = 2;
			s_frames = 0;
			return;
		}
		if (s_phase == 2) {
			int r = walk_update(st, s_dt);
			if (r == 0 && s_frames < 30 * 40)
				return;
			s_wpad_on = false;
			if (H->place) {      // face it from where he stands (a turn on the spot)
				float p2[3] = { st->pos[0], st->pos[1], st->pos[2] };
				H->place(p2, atan2f(s_aim[0] - p2[0], s_aim[1] - p2[2]));
			}
			press(KEY_A, 2, 4);
			s_phase = 3;
			s_frames = 0;
			return;
		}
		if (s_phase == 3) {
			auto_advance(st);
			if (s_frames < 45 || !idle(st))
				return;
			int it;
			float p[3];
			bool av;
			ev_pickup_info(cur, &it, p, &av);
			playtest_log("collect: item %d %s", it, ev_item_count(it) > before ? "picked up" : "not reached");
			s_phase = 1;
			return;
		}
		return;
	}
	if (!strcmp(op, "cube") && sscanf(arg, "%d %d", &a, &b) == 2) {
		// "cube A B": in the cube room's control (EvProgControlCube, its panel up), Up turns the outer
		// ring and Right the inner one a quarter until they read A and B (flags 317..320), then B
		if (s_npress)
			return;
		int ra = ev_flag(317) | ev_flag(318) << 1, rb = ev_flag(319) | ev_flag(320) << 1;
		if (ra != (a & 3))
			press(KEY_DUP, 2, 6);
		else if (rb != (b & 3))
			press(KEY_DRIGHT, 2, 6);
		else {
			playtest_log("cube: rings %d %d, ways %d %d", ra, rb, ev_flag(325), ev_flag(326));
			press(KEY_B, 2, 10);
			next();
		}
		return;
	}
	if (!strcmp(op, "enemies")) {
		// "enemies": the enemies in the log (kind, status, health, distance), nothing else
		for (int i = 0; i < enemy_count(); i++) {
			Enemy* e = enemy_get(i);
			if (e && e->used)
				playtest_log("enemy kind %d status %d hp %.0f at %.0f%s (%s)", e->kind, e->status, e->hp,
				             hypotf(e->x - st->pos[0], e->z - st->pos[2]), enemy_alive(e) ? "" : " dead", combat_status());
		}
		next();
		return;
	}
	if (!strcmp(op, "fight")) {
		// "fight [limit] [radius] [flag F]": James fights what is within radius with what he owns, as a
		// player does: the best gun with rounds (shotgun, rifle, handgun) or the plank, R held so the
		// game turns him to the nearest enemy, A to fire or swing, a step back when one is too close
		// for a gun, a health item below 40 % (the smallest that does), struggling out of holds. It
		// ends when nobody within radius is left (or flag F is set: the fights that end on a timer
		// or a scene) and fails when James dies or the time runs out. No god mode, no extra damage.
		float limit = 300.0f, radius = 6000.0f;
		int flag = -1;
		sscanf(arg, "%f %f", &limit, &radius);
		const char* fl = strstr(arg, "flag");
		if (fl)
			sscanf(fl + 4, "%d", &flag);
		static float back_t, heal_t, log_t, gun_t, equip_t;
		static int dead_frames, a_gap, cur_pick;
		if (s_frames == 1)
			back_t = heal_t = log_t = gun_t = equip_t = 0.0f, dead_frames = a_gap = cur_pick = 0;
		s_wpad_on = true;
		s_wpad[0] = s_wpad[1] = 0.0f;
		s_wheld = 0;
		bool free_now = strstr(combat_status(), "free") != NULL;
		if ((gun_t -= s_dt) <= 0.0f && free_now) {   // the weapon: re-chosen every few seconds (a gun
			gun_t = 3.0f;                            // runs dry), only with nothing up (0x49ea00)
			static const int guns[3][2] = { { 6, 7 }, { 8, 9 }, { 4, 5 } };
			int pick = 13;
			for (int g = 0; g < 3; g++)
				if (combat_owns_item(guns[g][0]) && (ev_item_count(guns[g][0]) > 0 || ev_item_count(guns[g][1]) > 0)) {
					pick = guns[g][0];
					break;
				}
			// (using the weapon in hand puts it away, as on the PC: only a different one)
			const char* wl = combat_weapon_line();
			int held_item = !strncmp(wl, "plank", 5) ? 13 : !strncmp(wl, "handgun", 7) ? 4 : !strncmp(wl, "shotgun", 7) ? 6
			              : !strncmp(wl, "rifle", 5) ? 8 : 0;
			cur_pick = held_item;
			if (pick != cur_pick && H->use_item) {
				playtest_log("fight: weapon item %d", pick);
				H->use_item(pick);
				cur_pick = pick;
				equip_t = 0.5f;              // the use is taken while James is free (events_update)
			}
		}
		Enemy* near = NULL;
		float nd = 1e9f;
		int alive = 0;
		for (int i = 0; i < enemy_count(); i++) {
			Enemy* e = enemy_get(i);
			if (!e || !e->used)
				continue;
			float d = hypotf(e->x - st->pos[0], e->z - st->pos[2]);
			if (d > radius || !(enemy_alive(e) || (e->hp > 0.0f && !e->dead_flag && e->kind != EN_KIND_RED)))
				continue;
			alive++;
			if (enemy_alive(e) && d < nd)
				nd = d, near = e;
		}
		bool gun = ev_item_count(4) + ev_item_count(6) + ev_item_count(8) > 0 &&
		           (combat_owns_item(4) || combat_owns_item(6) || combat_owns_item(8));
		if ((log_t -= s_dt) <= 0.0f) {
			log_t = s_walkdebug ? 0.25f : 2.0f;
			playtest_log("fight: %s [%s] items 4:%d 5:%d, %d left%s", combat_status(), combat_weapon_line(), ev_item_count(4),
			             ev_item_count(5), alive,
			             near ? "" : " (none in reach)");
			if (near)
				playtest_log("  nearest kind %d hp %.0f at %.0f", near->kind, near->hp, nd);
		}
		auto_advance(st);
		if (combat_dead() || combat_dying()) {
			result(false, "fight: James died");
			next();
			return;
		}
		if (combat_held()) {
			if (!s_npress) {
				press(KEY_A, 1, 1);
				press(KEY_B, 1, 1);
			}
		} else if (combat_hp() < 0.4f * JAMES_HP_MAX && (heal_t -= s_dt) <= 0.0f) {
			heal_t = 2.0f;                  // the smallest item that does
			float lost = JAMES_HP_MAX - combat_hp();
			int it = lost <= 0.25f * JAMES_HP_MAX && ev_item_count(1) ? 1 : lost <= 0.5f * JAMES_HP_MAX && ev_item_count(2) ? 2
			       : ev_item_count(1) ? 1 : ev_item_count(2) ? 2 : ev_item_count(3) ? 3 : 0;
			if (it && H->use_item) {
				playtest_log("fight: healing with item %d at HP %.0f", it, combat_hp());
				H->use_item(it);
			}
		} else if (near) {
			float dx = near->x - st->pos[0], dz = near->z - st->pos[2];
			float diff = remainderf(atan2f(dx, dz) - st->yaw, 2.0f * (float)M_PI);
			// with a gun James keeps his distance as a player does, running off into the open (the
			// stairwell Pyramid Head's long swing reaches 1500 and kills, the others' blows less far)
			float keep = near->kind == EN_KIND_RED ? 1600.0f : near->kind == EN_KIND_ONI ? 1100.0f
			           : near->kind == EN_KIND_BOS ? 700.0f : near->kind == EN_KIND_PAP ? 800.0f
			           : near->kind == EN_KIND_IKE ? 450.0f : 500.0f;
			static float run_dir;
			// rounds in the gun in hand (an empty one reloads on A: a long time standing still)
			const char* wl2 = combat_weapon_line();
			int in_gun = !strncmp(wl2, "handgun", 7) ? ev_item_count(4) : !strncmp(wl2, "shotgun", 7) ? ev_item_count(6)
			           : !strncmp(wl2, "rifle", 5) ? ev_item_count(8) : 1;
			if (gun && in_gun == 0 && near->kind == EN_KIND_RED && nd < keep + 700.0f && !strstr(combat_status(), "reload") &&
			    back_t <= 0.0f)
				nd = keep - 1.0f;               // no reloading with it this close: run first
			if (gun && nd < keep && back_t <= 0.0f) {
				// the open way that leaves the enemy furthest behind: 16 headings, floor and no wall
				// out to 900 at the body's height
				const Collision* col = H->collision ? H->collision() : NULL;
				float best = -1e9f;
				for (int k = 0; k < 16; k++) {
					float a2 = k * (float)M_PI / 8.0f, ok = 1.0f;
					for (float r = 300.0f; r <= 900.0f && ok > 0.0f; r += 300.0f) {
						float px = st->pos[0] + sinf(a2) * r, pz = st->pos[2] + cosf(a2) * r, y;
						if (!H->floor || !H->floor(px, pz, st->pos[1], 300.0f, &y) ||
						    (col && wall_across(col, st->pos[0], st->pos[2], px, pz, st->pos[1])))
							ok = r <= 300.0f ? -1.0f : 0.5f;
					}
					if (ok < 0.0f)
						continue;
					float ex = st->pos[0] + sinf(a2) * 900.0f - near->x, ez = st->pos[2] + cosf(a2) * 900.0f - near->z;
					float sc = hypotf(ex, ez) * ok - fabsf(remainderf(a2 - st->yaw, 2.0f * (float)M_PI)) * 80.0f;
					if (sc > best)
						best = sc, run_dir = a2;
				}
				back_t = best > -1e8f ? 1.2f : 0.0f;
			}
			if (back_t > 0.0f) {
				back_t -= s_dt;
				float rd = remainderf(run_dir - st->yaw, 2.0f * (float)M_PI);
				s_wpad[0] = fabsf(rd) > 0.3f ? (rd > 0 ? -1.0f : 1.0f) : -rd * 2.0f;
				s_wpad[1] = fabsf(rd) < 0.9f ? 1.0f : 0.0f;
				s_wheld = KEY_B;               // run
				if (nd > keep + 400.0f)
					back_t = 0.0f;             // far enough: turn and shoot
			} else if (!gun && nd > 600.0f) {
				// the plank: walk up to it
				s_wpad[0] = fabsf(diff) > 0.3f ? (diff > 0 ? -1.0f : 1.0f) : -diff * 2.0f;
				s_wpad[1] = fabsf(diff) < 0.6f ? 1.0f : 0.0f;
			} else if ((equip_t -= s_dt) > 0.0f) {
				// the weapon change first
			} else if (fabsf(diff) > 1.2f && !strstr(combat_status(), "ready")) {
				// facing away (after a run): turn first, the ready turn only reaches 100 degrees
				s_wpad[0] = diff > 0 ? -1.0f : 1.0f;
			} else {
				s_wheld = KEY_R;               // ready: the game turns James to the nearest enemy
				if (gun && nd < keep * 0.8f)
					back_t = 0.0f;              // (the next frame runs again)
				// the plank up: step in (it reaches about 200); the weapon comes up only once the
				// legs have stopped (0x557100), so the stick waits for the ready stance
				if (!gun && nd > 180.0f && strstr(combat_status(), "ready"))
					s_wpad[1] = 0.7f;
				if (++a_gap >= (gun ? 10 : 24) && !s_npress) {
					a_gap = 0;
					press(KEY_R | KEY_A, 2, 0);
				}
			}
		}
		dead_frames = alive || s_frames == 1 ? 0 : dead_frames + 1;
		if ((flag >= 0 && ev_flag(flag)) || dead_frames > 90) {
			char what[96];
			snprintf(what, sizeof what, "fight: won, HP %.0f", combat_hp());
			result(true, what);
			next();
			return;
		}
		if (s_cmd_t > limit) {
			char what[96];
			snprintf(what, sizeof what, "fight: %d left after %.0f s (HP %.0f)", alive, limit, combat_hp());
			result(false, what);
			next();
		}
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
	float limit = s_walkmode && (!strcmp(op, "ev") || !strcmp(op, "evgo") || !strcmp(op, "walkto") || !strcmp(op, "blocked")) ? 900.0f : 90.0f;
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
			int at_stage;
			const char* at = strchr(arg, '@');
			if (s_target_stage < 0)
				s_target_stage = at && sscanf(at + 1, "%d", &at_stage) == 1 ? at_stage : st->stage;   // "ev N @S"
			if (!ev_info_of(s_walkmode ? s_target_stage : st->stage, a, &e)) {
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
			if (!e.enabled && s_walkmode && e.kind == 4) {   // (the expect lines after it check its flags)
				playtest_log("ev %d: went off on the way already", a);   // an enter trigger James walked through
				next();
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
			s_walked = false;
			if (s_walkmode && e.kind >= 2) {
				char na[8], nb[8];
				node_of(st->map, na);
				node_of(e.map[0] ? e.map : st->map, nb);
				int door;
				if (s_door_last && open_area(st->map) && !strcmp(na, nb)) {
					float gp[3], gy;
					char fold[8];
					snprintf(fold, sizeof fold, "%.2s", st->map);
					event_pose(&e, st, gp, &gy);
					if (e.any_height && !nav_area_floor(fold, gp[0], gp[2], st->pos[1], &gp[1])) {
						snprintf(fold, sizeof fold, "%.2s", e.map[0] ? e.map : st->map);
						if (!nav_area_floor(fold, gp[0], gp[2], st->pos[1], &gp[1]))
							gp[1] = st->pos[1];
					}
					door = area_first_door(st, gp, e.map);
					if (door == -2)
						door = -1;       // the area route says yes but the walk failed: no other way
					if (door == -1)      // walled in (Wood Side's courtyard): out through the buildings
						door = route_first_door(st, s_target_stage, e.map, gp);
				} else {
					float gp[3], gy;
					event_pose(&e, st, gp, &gy);
					door = e.map[0] || st->stage != s_target_stage ? route_first_door(st, s_target_stage, e.map, gp) : -2;
				}
				if (door == -1 || s_doors_taken > 16) {
					char what[192];
					snprintf(what, sizeof what, "ev %d (stage %d): no way on foot from %s to %s", a, st->stage, st->map, e.map);
					result(false, what);
					next();
					return;
				}
				if (door >= 0) {
					EvInfo d;
					ev_info(door, &d);            // the first door is always in James' stage
					event_pose(&d, st, pos, &yaw);
					playtest_log("ev %d: through door %d (%s -> %s)", a, door, d.map, d.dest_map);
					s_route_door = door;
					snprintf(s_walk_goal_map, sizeof s_walk_goal_map, "%s", d.map);
					walk_begin(st, pos, d.kind == 4 ? 1e9f : yaw);
					W.near = 380.0f;
					float ex = W.nwp ? W.wp[2 * W.nwp - 2] : st->pos[0], ez = W.nwp ? W.wp[2 * W.nwp - 1] : st->pos[2];
					if (W.partial && !open_area(st->map) && hypotf(ex - pos[0], ez - pos[2]) > 600.0f && s_nbad < NBAD) {
						// the plan over the room cannot reach it: leave it out without walking there
						playtest_log("ev %d: door %d is out of reach, routing round it", a, door);
						s_bad_stage[s_nbad] = st->stage, s_bad_ev[s_nbad] = door, s_bad_side[s_nbad] = 0, s_nbad++;
						return;
					}
					s_phase = 20;
					s_frames = 0;
					return;
				}
				event_pose(&e, st, pos, &yaw);
				snprintf(s_walk_goal_map, sizeof s_walk_goal_map, "%s", e.map);
				walk_begin(st, pos, e.kind == 3 || e.kind == 4 || e.kind == 6 || e.kind == 8 || e.kind == 9 ? 1e9f : yaw);
				s_changes = 0;
				s_story_at = s_story;
				s_phase = 21;
				s_frames = 0;
				return;
			}
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
		if (s_phase == 20 || s_phase == 21) {
			// on foot: to the door (20) or to the event (21)
			if (s_phase == 21 && (s_story != s_story_at || !idle(st))) {
				s_on_way = hypotf(W.goal[0] - st->pos[0], W.goal[2] - st->pos[2]) > 600.0f || fabsf(W.goal[1] - st->pos[1]) > 400.0f;
				s_phase = 3;             // an enter trigger fired on the way in (or the event itself)
				s_frames = 0;
				return;
			}
			if (s_phase == 20 && (!idle(st) || st->stage != s_ev_stage)) {
				s_phase = 22;            // an enter door took him (or something else happened)
				s_frames = 0;
				return;
			}
			int r = walk_update(st, s_dt);
			EvInfo le;
			if (r < 0 && s_phase == 21 && ev_info(a, &le) && le.enabled && look_from_here(&le, st, s_aim)) {
				// short of the spot but in sight of the trigger: face it from here
				playtest_log("ev %d: in sight from (%.0f %.0f), facing it", a, st->pos[0], st->pos[2]);
				s_aim_on = true;
				s_walked = false;
				s_phase = 2;
				s_frames = 0;
				return;
			}
			if (r < 0 && s_phase == 21 && s_in_ev >= 0 && !bad_door(s_in_stage, s_in_ev) && s_nbad < NBAD) {
				// in by a door to the wrong part of the map (the north end of ap50's hallway): another way
				playtest_log("ev %d: door %d led to the wrong side, routing round it", a, s_in_ev);
				s_bad_stage[s_nbad] = s_in_stage, s_bad_ev[s_nbad] = s_in_ev, s_bad_side[s_nbad] = 1, s_nbad++;
				s_in_ev = -1;
				s_door_last = true;
				s_phase = 1;
				return;
			}
			if (r < 0 && !s_door_last && s_phase == 21) {
				s_door_last = true;      // the gate of the graveyard and the like: look for a door
				playtest_log("ev %d: no way on foot, looking for a door", a);
				s_phase = 1;
				return;
			}
			if (r < 0 && s_phase == 20 && s_nbad < NBAD) {
				playtest_log("ev %d: door %d is out of reach, routing round it", a, s_route_door);
				s_bad_stage[s_nbad] = st->stage, s_bad_ev[s_nbad] = s_route_door, s_bad_side[s_nbad] = 0, s_nbad++;
				s_phase = 1;
				return;
			}
			if (r < 0) {
				char what[192];
				snprintf(what, sizeof what, "ev %d (stage %d): could not walk to (%.0f %.0f) from (%.0f %.0f)", a, st->stage,
				         W.goal[0], W.goal[2], st->pos[0], st->pos[2]);
				result(false, what);
				next();
				return;
			}
			if (r == 0)
				return;
			if (s_phase == 21) {
				s_walked = true;
				s_phase = 2;
				s_frames = 2;            // already standing: face it again (frame 3), then the button
				return;
			}
			EvInfo d;
			ev_info(s_route_door, &d);
			if (d.kind == 2 || d.kind == 3 || d.kind == 7 || d.kind == 10)
				press(KEY_A, 2, 2);
			s_in_stage = st->stage, s_in_ev = s_route_door;
			s_doors_taken++;
			s_phase = 22;
			s_frames = 0;
			return;
		}
		if (s_phase == 22) {
			// through a door: when the game settles, route again from the new room
			auto_advance(st);
			if (s_frames > 40 && idle(st)) {
				s_ev_stage = st->stage;
				s_door_last = false;     // through: walk again from here
				int k = 0;               // what was out of reach may not be from here
				for (int i = 0; i < s_nbad; i++)
					if (s_bad_side[i])
						s_bad_stage[k] = s_bad_stage[i], s_bad_ev[k] = s_bad_ev[i], s_bad_side[k] = 1, k++;
				s_nbad = k;
				s_phase = 1;
				s_frames = 0;
			} else if (s_cmd_t > limit) {
				result(false, "ev: a door on the way did not let James through");
				next();
			}
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
				// the walls may have pushed him (or the walk stopped him short): face the trigger again
				// from where he stands, turning on the spot
				EvInfo le;
				if (s_walked && ev_info(a, &le) && le.enabled)
					look_from_here(&le, st, s_aim);
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
			if (s_frames >= 30 && idle(st) && !s_changes && s_walkmode && s_retries < 3) {
				// a monster's blow or a turn short of the line: walk up and press again
				playtest_log("ev %d: nothing happened, again (%s)", a, combat_status());
				s_retries++;
				s_phase = 1;
				s_frames = 0;
				return;
			}
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
			EvInfo e2;
			if (idle(st) && s_frames > 4 && s_on_way && s_rewalks < 6 && ev_info_of(s_target_stage, a, &e2) && e2.enabled) {
				// the PC's triggers go off in the order James passes them (town west's cc04 before
				// cc03): on to this one when the other is done
				playtest_log("ev %d: another trigger on the way, walking on", a);
				s_on_way = false;
				s_rewalks++;
				s_phase = 1;
				s_frames = 0;
				return;
			}
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
	bool blocked_cmd = !strcmp(op, "blocked");
	if ((!strcmp(op, "walkto") || blocked_cmd) && sscanf(arg, "%f %f %f", &f[0], &f[1], &f[2]) == 3) {
		// "walkto X Y Z": on foot to a point of the loaded collision; "blocked X Y Z": James must not
		// get there (within 600) on foot (the cube room with its rings set wrong)
		if (s_phase == 1) {
			s_walk_goal_map[0] = 0;
			if (!walk_begin(st, f, 1e9f)) {
				result(blocked_cmd, c);
				next();
				return;
			}
			s_phase = 2;
			return;
		}
		int r = walk_update(st, s_dt);
		if (r != 0) {
			char what[160];
			bool there = r > 0 && hypotf(f[0] - st->pos[0], f[2] - st->pos[2]) < 600.0f;
			snprintf(what, sizeof what, "%s: %s at (%.0f %.0f)", c, there ? "there" : "no way", st->pos[0], st->pos[2]);
			result(blocked_cmd ? !there : there, what);
			next();
		}
		return;
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
	s_dt = dt;
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
	s_wpad_on = false;
	s_wheld = 0;
	if (!s_quit)
		step(st);
	// the buttons: the script's presses replace the player's
	*down = 0;
	*held = s_wheld;
	if (s_npress) {
		Press* p = &s_press[0];
		if (s_press_f < p->hold) {
			*held |= p->keys;
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

bool playtest_steering(void) { return s_active && s_wpad_on; }

void playtest_pad(circlePosition* cp)
{
	if (!s_active)
		return;
	if (s_wpad_on) {
		cp->dx = (s16)(s_wpad[0] * 156.0f);
		cp->dy = (s16)(s_wpad[1] * 156.0f);
	} else if (s_pad_t > 0.0f) {
		cp->dx = (s16)(s_pad[0] * 156.0f);
		cp->dy = (s16)(s_pad[1] * 156.0f);
	} else {
		cp->dx = cp->dy = 0;
	}
}
