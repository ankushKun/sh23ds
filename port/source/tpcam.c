// The behind camera (tpcam.h). A 3DS-only system: none of it comes from sh2pc.exe, whose camera is
// game/view/sh2cam.c. Distances are world units (500 to the metre), angles radians, Y points down.
#include "tpcam.h"
#include <math.h>
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
#define WALL_GAP 80.0f        // kept from a wall
#define NEAR_DIST 200.0f
#define LIFT_FROM 700.0f      // closer than this the camera rises
#define FOVY 0.9273f          // 53.13 degrees: the PC camera's default (scr_z 448 of 448 lines)

static struct {
	float yaw, pitch, dist;   // the orbit
	float eye[3], watch[3];
	float idle;               // seconds since the last orbit input
	bool recentring;
	bool started;
} T;

float tpcam_yaw(void) { return T.yaw; }
const float* tpcam_eye(void) { return T.eye; }
const float* tpcam_watch(void) { return T.watch; }

static float wrap(float a) { return remainderf(a, 2.0f * PI); }

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

	// the orbit: the C-Stick (or D-Pad) as a rate, a touch drag as an angle
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
	float d = want_dist;
	if (in->col) {
		float ex = eye[0], ez = eye[2];
		col_clamp_ray(in->col, pivot[0], pivot[2], &ex, &ez, eye[1]);
		float full = hypotf(eye[0] - pivot[0], eye[2] - pivot[2]), got = hypotf(ex - pivot[0], ez - pivot[2]);
		if (got < full - 1.0f && full > 1.0f)
			d = fmaxf((got - WALL_GAP) / full * want_dist, NEAR_DIST);
	}
	// in at once (no wall shows through), back out smoothly
	T.dist = d < T.dist ? d : T.dist + (d - T.dist) * ease(4.0f, dt);
	place(pivot, T.yaw, T.pitch, T.dist, side * (T.dist / want_dist), eye);
	// pulled in close by a wall: up over his head rather than into the back of it
	eye[1] -= fmaxf(0.0f, LIFT_FROM - T.dist) * 0.9f;
	if (in->col) {
		// not below the floor under the camera
		float fy;
		if (col_floor(in->col, eye[0], eye[2], eye[1], 2000.0f, &fy) && eye[1] > fy - 100.0f)
			eye[1] = fy - 100.0f;
	}
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
		T.eye[0] = ex, T.eye[2] = ez;
	}
	C3D_FVec e = FVec3_New(T.eye[0], T.eye[1], T.eye[2]);
	C3D_FVec w = FVec3_New(T.watch[0], T.watch[1], T.watch[2]);
	Mtx_LookAt(view, e, w, FVec3_New(0.0f, -1.0f, 0.0f), false);
	*fovy = FOVY;
}
