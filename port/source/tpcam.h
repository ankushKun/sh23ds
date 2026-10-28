#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include "collide.h"

// The behind camera: a 3DS-only alternative to the PC camera (Control Options, "Camera"), not in
// the PC game. It sits behind and above James, turns lazily after the way he walks, orbits with the
// C-Stick (on an Old 3DS with the D-Pad or a drag on the touch screen), swings round behind him with
// the look key and stops short of walls (col_clamp_ray). With the weapon up it goes over his right
// shoulder, and with a target locked it frames James and the target together. main.c shows the PC
// camera instead in cutscenes, on the event roads and user shots of the .cam data, on the boat and in
// the viewer; the stick then moves James relative to this camera (move.c's 2D reading).

typedef struct {
	float pos[3];          // James' feet (Y down)
	float yaw;             // his facing: forward = (sin yaw, cos yaw)
	float speed;           // his ground speed, units per second
	float orbit_x, orbit_y;   // -1..1, held: the C-Stick or the D-Pad (right / up positive)
	float drag_x, drag_y;  // a drag on the touch screen this frame, in pixels
	bool recentre;         // the look key went down: back round behind him
	bool aim;              // the weapon is up
	bool target_on;        // a target is locked: target holds its chest
	float target[3];
	bool stick_on;         // the circle pad is pushed; stick_dir is its angle from the camera's forward
	float stick_dir;
	const Collision* col;  // NULL: no walls
} TpCamInput;

// Behind James at once (a new room, after a scripted shot or a scene).
void tpcam_reset(const float pos[3], float yaw);
void tpcam_update(const TpCamInput* in, float dt, C3D_Mtx* view, float* fovy);
float tpcam_yaw(void);              // the camera's forward, for the stick (forward = sin, cos)
const float* tpcam_eye(void);
const float* tpcam_watch(void);
