// The behind camera (tpcam.h). A 3DS-only system: none of it comes from sh2pc.exe, whose camera is
// game/view/sh2cam.c. Distances are world units (500 to the metre), angles radians, Y points down.
#include "tpcam.h"
#include "camray.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f
#define PIVOT_H 800.0f        // the point the camera turns round: James' shoulders above his feet
#define DIST 1500.0f          // behind him while he explores
#define AIM_DIST 1150.0f      // over the shoulder with the weapon up
#define SHOULDER 220.0f       // how far right of him the aiming camera sits
#define PITCH 0.30f           // looking down on him a little
#define PITCH_MIN -0.35f
#define PITCH_MAX 1.0f
#define ORBIT_YAW 2.6f        // rad/s at full C-Stick
#define ORBIT_PITCH 1.6f
#define DRAG_RAD 0.012f       // rad per pixel of touch drag
#define FOLLOW_WAIT 1.2f      // seconds after an orbit before it follows his walk again
#define WALL_GAP 150.0f       // kept from a wall: the near plane (the rooms' clip near, 100) stays clear of it
#define CAM_RADIUS 90.0f      // the rays either side of the centre one, so walls beside the camera count too
#define NEAR_DIST 450.0f      // never closer than this: inside his head nothing reads
#define LEAD 700.0f           // pulled in all the way, it looks this far past him (into the room)
#define LIFT_FROM 700.0f      // closer than this the camera rises
#define LIFT 0.45f            // by this share of what it is short of LIFT_FROM
#define CEIL_GAP 160.0f       // kept under the ceiling
#define SQUEEZED 900.0f       // free room behind under this: look for a more open side
#define SWING 1.4f            // rad/s it turns towards one
#define FOVY 0.9273f          // 53.13 degrees: the PC camera's default (scr_z 448 of 448 lines)

static struct {
	int debug;                // debug.txt "tpcam": 1 counts frames with the eye behind the room, 2 also without camray
	u32 behind, frames;
	float yaw, pitch, dist;   // the orbit
	float eye[3], watch[3];
	float idle;               // seconds since the last orbit input
	bool recentring;
	bool started;
	float room, ceil_gap, eye_d, eye_up;   // the last frame's room behind, ceiling, eye (status)
	char status[128];
} T;

float tpcam_yaw(void) { return T.yaw; }
void tpcam_debug(int mode) { T.debug = mode; }

bool tpcam_behind_count(u32* behind, u32* frames)
{
	if (!T.debug || !T.frames)
		return false;
	*behind = T.behind, *frames = T.frames;
	T.behind = T.frames = 0;
	return true;
}
const float* tpcam_eye(void) { return T.eye; }
const float* tpcam_watch(void) { return T.watch; }
const char* tpcam_status(void)
{
	snprintf(T.status, sizeof T.status, "tp yaw %.2f pitch %.2f dist %.0f room %.0f eye %.0f from him, %.0f up, ceiling %.0f",
	         T.yaw, T.pitch, T.dist, T.room, T.eye_d, T.eye_up, T.ceil_gap);
	return T.status;
}

static float wrap(float a) { return remainderf(a, 2.0f * PI); }

// How far the camera can go from the pivot towards yaw / pitch before a wall, the centre ray and one
// CAM_RADIUS either side of it (a wall at the edge of the near plane counts as much as one ahead).
// ey: the height it will be at (under the ceiling): a low wall the camera would pass over at its
// orbit's height still stops it there.
static float free_dist(const Collision* col, const float pivot[3], float yaw, float pitch, float want, float ey)
{
	float c = cosf(pitch), bx = -sinf(yaw) * c, bz = -cosf(yaw) * c, rx = cosf(yaw), rz = -sinf(yaw);
	float best = want;
	for (int k = -1; k <= 1; k++) {
		// from James to the camera's place and to either side of it: a ray that started beside him
		// would start behind a wall he stands against, and every way round would look blocked
		float ex = pivot[0] + bx * want + rx * CAM_RADIUS * k, ez = pivot[2] + bz * want + rz * CAM_RADIUS * k;
		float full = hypotf(ex - pivot[0], ez - pivot[2]);
		col_clamp_ray(col, pivot[0], pivot[2], &ex, &ez, ey);
		float got = hypotf(ex - pivot[0], ez - pivot[2]);
		float d = full > 1.0f ? got / full * want : want;
		best = d < best ? d : best;
	}
	return best;
}

// The ceiling over James, from the walls round him: indoors the walls go up to it, so the highest
// top of the walls that stand on his floor within reach (Y down: the smallest y_top). None: no cap.
static bool ceiling(const Collision* col, const float feet[3], float* y)
{
	bool any = false;
	float top = 0.0f;
	for (u32 i = 0, n = col_wall_count(col); i < n; i++) {
		const WallSeg* w = col_wall(col, i);
		if (w->y_bottom < feet[1] - 200.0f || w->y_top > feet[1] - 900.0f)
			continue;                       // not down to his floor, or too low to be a room's wall
		float dx = w->x1 - w->x0, dz = w->z1 - w->z0, l2 = dx * dx + dz * dz;
		float t = l2 > 0.0f ? ((feet[0] - w->x0) * dx + (feet[2] - w->z0) * dz) / l2 : 0.0f;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		float px = w->x0 + dx * t - feet[0], pz = w->z0 + dz * t - feet[2];
		if (px * px + pz * pz > 1800.0f * 1800.0f)
			continue;
		if (!any || w->y_top < top)
			top = w->y_top, any = true;
	}
	*y = top;
	return any && feet[1] - top < 4000.0f;   // a few metres: a room's, not a building's outside
}

// The share of the way from the pivot to eye that nothing the room draws is in (camray.h): the centre
// ray and one CAM_RADIUS to each side and above and below, the near plane's size. The side pair and
// the up and down pair take turns a frame, each keeping its last share for the other's frames.
static float mesh_share(const float pivot[3], const float eye[3], float yaw)
{
	static float pair[2] = { 1.0f, 1.0f };
	static int turn;
	turn ^= 1;
	float rx = cosf(yaw) * CAM_RADIUS, rz = -sinf(yaw) * CAM_RADIUS;
	const float off[2][2][3] = { { { rx, 0, rz }, { -rx, 0, -rz } }, { { 0, -CAM_RADIUS, 0 }, { 0, CAM_RADIUS, 0 } } };
	pair[turn] = 1.0f;
	for (int k = 0; k < 2; k++) {
		float e[3] = { eye[0] + off[turn][k][0], eye[1] + off[turn][k][1], eye[2] + off[turn][k][2] };
		pair[turn] = fminf(pair[turn], camray_cast(pivot, e));
	}
	return fminf(camray_cast(pivot, eye), fminf(pair[0], pair[1]));
}

// eye brought in front of the first of the room's triangles between it and the pivot, by the wall
// gap (no closer to him than 120 units, or than the triangle)
static void pull_in(const float pivot[3], float eye[3], float yaw, bool wide)
{
	if (T.debug == 2)
		return;                          // the camera as before camray, for the count
	float t = wide ? mesh_share(pivot, eye, yaw) : camray_cast(pivot, eye);
	if (t >= 1.0f)
		return;
	float d[3] = { eye[0] - pivot[0], eye[1] - pivot[1], eye[2] - pivot[2] };
	float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len < 1.0f)
		return;
	float got = t * len, k = fmaxf(got - WALL_GAP, fminf(120.0f, got)) / len;
	for (int i = 0; i < 3; i++)
		eye[i] = pivot[i] + d[i] * k;
}

// d towards 0 by the share a time constant of 1/rate takes off in dt
static float ease(float rate, float dt) { return 1.0f - expf(-rate * dt); }

static void place(const float pivot[3], float yaw, float pitch, float dist, float side, float out[3])
{
	float c = cosf(pitch);
	// behind = -forward; right of the facing is (cos yaw, -sin yaw)
	out[0] = pivot[0] - sinf(yaw) * c * dist + cosf(yaw) * side;
	out[1] = pivot[1] - sinf(pitch) * dist;
	out[2] = pivot[2] - cosf(yaw) * c * dist - sinf(yaw) * side;
}

void tpcam_reset(const float pos[3], float yaw)
{
	T.yaw = yaw;
	T.pitch = PITCH;
	T.dist = DIST;
	T.idle = FOLLOW_WAIT;
	T.recentring = false;
	float pivot[3] = { pos[0], pos[1] - PIVOT_H, pos[2] };
	place(pivot, T.yaw, T.pitch, T.dist, 0.0f, T.eye);
	memcpy(T.watch, pivot, sizeof T.watch);
	T.started = true;
}

void tpcam_update(const TpCamInput* in, float dt, C3D_Mtx* view, float* fovy)
{
	if (!T.started)
		tpcam_reset(in->pos, in->yaw);
	float pivot[3] = { in->pos[0], in->pos[1] - PIVOT_H, in->pos[2] };
	// the ceiling over him (Y down: the camera stays below cap)
	float cy, cap = -1e9f;
	T.ceil_gap = in->col && ceiling(in->col, in->pos, &cy) ? in->pos[1] - cy : 0.0f;
	if (T.ceil_gap > 0.0f)
		cap = cy + CEIL_GAP;
	// and from the room's own triangles straight over him (camray.h): the walls' tops miss a
	// stairwell's landing or a lowered ceiling, which the camera then rose through
	{
		float up[3] = { pivot[0], pivot[1] - 3000.0f, pivot[2] };
		float t = T.debug == 2 ? 1.0f : camray_cast(pivot, up);
		if (t < 1.0f) {
			float y = pivot[1] - 3000.0f * t;
			cap = fmaxf(cap, fminf(y + CEIL_GAP, pivot[1]));
			T.ceil_gap = in->pos[1] - y;
		}
	}

	// the orbit: the C-Stick as a rate, a touch drag as an angle
	float ox = in->orbit_x + in->drag_x * DRAG_RAD / (dt > 0.0f ? dt * ORBIT_YAW : 1.0f);
	float oy = in->orbit_y - in->drag_y * DRAG_RAD / (dt > 0.0f ? dt * ORBIT_PITCH : 1.0f);
	bool orbiting = fabsf(in->orbit_x) > 0.15f || fabsf(in->orbit_y) > 0.15f || in->drag_x || in->drag_y;
	if (in->recentre)
		T.recentring = true;
	if (in->aim) {
		// the weapon up: over his shoulder, turned with him (the stick or the lock turns James)
		T.recentring = false;
		T.yaw = wrap(T.yaw + wrap(in->yaw - T.yaw) * ease(12.0f, dt));
		T.pitch += (0.12f - T.pitch) * ease(8.0f, dt);
		T.idle = 0.0f;
	} else if (orbiting) {
		T.recentring = false;
		T.yaw = wrap(T.yaw + ox * ORBIT_YAW * dt);
		T.pitch -= oy * ORBIT_PITCH * dt;
		T.idle = 0.0f;
	} else if (T.recentring) {
		float d = wrap(in->yaw - T.yaw);
		T.yaw = wrap(T.yaw + d * ease(10.0f, dt));
		T.pitch += (PITCH - T.pitch) * ease(10.0f, dt);
		if (fabsf(d) < 0.01f)
			T.recentring = false;
	} else {
		// lazily after his walk: only once the player has left the camera alone a moment, and only
		// when he goes away from it (pushing towards the camera or sideways leaves it be, so a
		// camera-relative stick does not chase its own tail)
		T.idle += dt;
		if (T.idle >= FOLLOW_WAIT && in->stick_on && fabsf(in->stick_dir) < 1.2f && in->speed > 300.0f) {
			float k = 1.5f * fminf(in->speed / 1500.0f, 1.0f);
			T.yaw = wrap(T.yaw + wrap(in->yaw - T.yaw) * ease(k, dt));
		}
		// squeezed against a wall (a corridor, a small room): round to the most open side near
		// behind him, a little at a time, so it sees the room instead of the back of his head
		float now = in->col ? free_dist(in->col, pivot, T.yaw, T.pitch, DIST, fmaxf(pivot[1] - sinf(T.pitch) * DIST, cap)) : DIST;
		if (in->col && (T.idle >= FOLLOW_WAIT || now < NEAR_DIST + WALL_GAP)) {
			if (now < SQUEEZED) {
				// near behind him first; in a room too small for that (a wall at his back), the
				// most open way round him, as the PC's fixed shots stand in a room's far corner
				bool tiny = now < NEAR_DIST + WALL_GAP;
				float best = now, to = T.yaw;
				for (int k = 1; k <= (tiny ? 9 : 6); k++)
					for (int s = -1; s <= 1; s += 2) {
						float y = wrap(T.yaw + s * k * 0.35f);
						float d = free_dist(in->col, pivot, y, T.pitch, DIST, fmaxf(pivot[1] - sinf(T.pitch) * DIST, cap));
						if (d > best * 1.25f + 100.0f)
							best = d, to = y;
					}
				float dy = wrap(to - T.yaw), step = SWING * (tiny ? 2.0f : 1.0f) * dt;
				T.yaw = wrap(T.yaw + (dy > step ? step : dy < -step ? -step : dy));
			}
		}
	}
	T.pitch = T.pitch < PITCH_MIN ? PITCH_MIN : T.pitch > PITCH_MAX ? PITCH_MAX : T.pitch;

	// where it wants to be, then stopped short of the walls between James and there
	float want_dist = in->aim ? AIM_DIST : DIST, side = in->aim ? SHOULDER : 0.0f;
	float look[3];
	memcpy(look, pivot, sizeof look);
	if (in->aim && in->target_on) {
		// James and the target together: the camera looks between them and backs off with the gap
		for (int i = 0; i < 3; i++)
			look[i] = pivot[i] + (in->target[i] - pivot[i]) * 0.4f;
		float gap = hypotf(in->target[0] - pivot[0], in->target[2] - pivot[2]);
		want_dist = AIM_DIST + fminf(gap * 0.25f, 900.0f);
	}
	float eye[3];
	place(pivot, T.yaw, T.pitch, want_dist, side, eye);
	float d = want_dist, room = want_dist;
	if (in->col) {
		room = free_dist(in->col, pivot, T.yaw, T.pitch, want_dist, fmaxf(pivot[1] - sinf(T.pitch) * want_dist, cap)) - WALL_GAP;
		T.room = room;
		if (room < want_dist - 1.0f)
			d = fmaxf(room, NEAR_DIST);
		d = fminf(d, fmaxf(room, 150.0f));   // a wall nearer than NEAR_DIST: no further than it
	}
	// in at once (no wall shows through), back out smoothly
	T.dist = d < T.dist ? d : T.dist + (d - T.dist) * ease(4.0f, dt);
	if (!in->aim) {
		// pulled in close, it looks past him the way it faces, so the room fills the picture rather
		// than the back of his head
		float k = fminf(fmaxf((DIST - T.dist) / (DIST - NEAR_DIST), 0.0f), 1.0f) * LEAD;
		k += fmaxf(NEAR_DIST - T.dist, 0.0f) * 0.8f;   // over his head: further on still
		look[0] += sinf(T.yaw) * k, look[2] += cosf(T.yaw) * k;
		look[1] += k * 0.25f;            // and a little lower: what is ahead of him, not over it
	}
	place(pivot, T.yaw, T.pitch, T.dist, side * (T.dist / want_dist), eye);
	// pulled in close by a wall: up over his head rather than into the back of it, the more the
	// closer (up to the ceiling, below)
	eye[1] -= fmaxf(0.0f, LIFT_FROM - T.dist) * (T.dist < NEAR_DIST ? 1.2f : LIFT);
	if (in->col) {
		// not below the floor under the camera, nor up through the ceiling of the room
		float fy;
		if (col_floor(in->col, eye[0], eye[2], eye[1], 2000.0f, &fy) && eye[1] > fy - 100.0f)
			eye[1] = fy - 100.0f;
		if (eye[1] < cap)
			eye[1] = cap;
	}
	pull_in(pivot, eye, T.yaw, true);    // nor behind anything the room draws
	float ke = ease(14.0f, dt), kw = ease(16.0f, dt);
	for (int i = 0; i < 3; i++) {
		// the orbit's own angles are already smooth; the easing takes the jolts of his steps
		T.eye[i] += (eye[i] - T.eye[i]) * ke;
		T.watch[i] += (look[i] - T.watch[i]) * kw;
	}
	// a wall must not come between: the eased eye stays on the clamped side
	if (in->col) {
		float ex = T.eye[0], ez = T.eye[2];
		col_clamp_ray(in->col, pivot[0], pivot[2], &ex, &ez, T.eye[1]);
		float full = hypotf(T.eye[0] - pivot[0], T.eye[2] - pivot[2]), got = hypotf(ex - pivot[0], ez - pivot[2]);
		if (got < full - 1.0f && got > 1.0f) {
			// stopped by a wall: back off it by the gap, so the near plane does not cut into it
			float k = fmaxf(got - WALL_GAP, fminf(120.0f, got)) / got;
			ex = pivot[0] + (ex - pivot[0]) * k, ez = pivot[2] + (ez - pivot[2]) * k;
		}
		T.eye[0] = ex, T.eye[2] = ez;
	}
	pull_in(pivot, T.eye, T.yaw, false); // the eased eye too: easing must not carry it through
	if (T.eye[1] < cap)
		T.eye[1] = cap;
	if (T.debug) {
		T.frames++;
		T.behind += camray_cast(pivot, T.eye) < 1.0f;
	}
	T.eye_d = hypotf(T.eye[0] - in->pos[0], T.eye[2] - in->pos[2]), T.eye_up = in->pos[1] - T.eye[1];
	C3D_FVec e = FVec3_New(T.eye[0], T.eye[1], T.eye[2]);
	C3D_FVec w = FVec3_New(T.watch[0], T.watch[1], T.watch[2]);
	Mtx_LookAt(view, e, w, FVec3_New(0.0f, -1.0f, 0.0f), false);
	*fovy = FOVY;
}
