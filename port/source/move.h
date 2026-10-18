#pragma once
#include <3ds.h>
#include <stdbool.h>

// James' (and Maria's) legs: the PC's lower-body states for walking about (0x54e0f0 picks the state,
// the table at 0x8bf6d8 runs it, 0x531a00 picks the animation) with their speeds, turn rates and
// clips: standing, turning on the spot, walking, backing off, strafing, the three running speeds,
// strafing at a run and the quick 180 degree turn. Combat (readied weapon, attacks, hurt) still
// lives in combat.c; this is what James does when he is free.
//
// Angles follow the port: yaw grows when James turns left. Speeds are units per second.

typedef struct {
	float x, y;               // circle pad, -1..1 (y forward), dead zone already taken off
	bool run;                 // run held (or latched by the toggle option)
	bool strafe_l, strafe_r;  // held
	bool strafe_l_down, strafe_r_down;   // pressed this frame (the PC turns round on both at once)
	bool turn180;             // the quick turn button went down
	bool camera_relative;     // the 2D control type: the stick points where to go on screen
	float camera_yaw;         // for camera_relative
	bool wall_ahead;          // a wall within 250 straight ahead (0x536a20): walk, do not run
	float hp_frac;            // health / full health: how long James can run flat out
	bool maria;               // Born From a Wish: her clips are only stand, walk, back and run
} MoveInput;

typedef struct {
	u16 start, end;           // frames in the character's anm
	bool loop;
	float rate;               // frames per second
} MoveClip;

typedef struct {
	float dx, dz;             // where the legs take James this frame (before collision)
	float turn_speed;         // yaw rate, rad/s (for the camera)
	int state;                // MV_*
	MoveClip clip;
	float frame;              // current frame of clip
	float blend;              // seconds to crossfade into clip when it has just changed, else 0
	bool clip_changed;
	bool sync;                // the crossfade keeps the gait's phase: the old clip (from_start, same
	u16 from_start;           // length) plays on at the same step while it fades out
	bool step;                // a foot came down this frame (0x54a9a0's frames of the clip)
	float step_volume;        // 0.5 walking, 0.8 running
	bool backward;            // the clip plays from its end (backing off with Maria's walk)
} MoveOut;

enum {
	MV_STAND, MV_RELAX, MV_TIRED, MV_LROUND, MV_RROUND, MV_BACK, MV_WALK, MV_LSWALK, MV_RSWALK,
	MV_RUN1, MV_RUN2, MV_RUN3, MV_LSRUN, MV_RSRUN, MV_LTURN, MV_RTURN, MV_COUNT
};

void move_reset(void);
void move_update(const MoveInput* in, float* yaw, float dt, MoveOut* out);
// The last update's clip and frame, for whoever draws James while he is free.
const MoveOut* move_last(void);
// Something else (combat, a scene) animated James: the next free clip blends in from there.
void move_interrupt(void);
const char* move_status(void);
