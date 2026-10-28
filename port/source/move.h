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
	float x, y;               // circle pad, -1..1 (y forward); move.c reads it as the PC's stick bytes
	                          // (0x52e110's dead zones), main.c only takes the 3DS pad's noise off
	bool run;                 // run held (or latched by the toggle option)
	bool strafe_l, strafe_r;  // held
	bool strafe_l_down, strafe_r_down;   // pressed this frame (the PC turns round on both at once)
	bool turn180;             // the quick turn button went down
	int control;              // MOVE_TANK or MOVE_2D (the Options' Control Type)
	float camera_yaw;         // for the 2D types: the camera's facing
	bool wall_ahead;          // a wall within 250 straight ahead (0x536a20): walk, do not run
	float hp_frac;            // health / full health: how long James can run flat out
	bool maria;               // Born From a Wish: Maria plays, at 0.8 of James' speed
	bool drag;                // the great knife in hand: dragged, no running (0x54b3b0, 0x52e110)
	bool enemy_near;          // an enemy is about (0x5422b0): standing still he looks round, not relaxes
	int place;                // 0x1fba8d8 (0x556490): 0 out of doors, 1 indoors, 2 a room whose byte
	                          // in 0x8bcce0 is 0; run3 only out of doors, the walk turns 1.3 x in 2
	// The weapon is up (the PC's upper status 0x1a, or 0x1c while attacking; combat_legs): the legs
	// keep the lower states of 0x6cf878's row 26 (stand still in the hold stance, walk, back off,
	// strafe, run1, strafe at a run), as far as the weapon's row of 0x6cf7cc lets them.
	bool aim;
	bool locked;              // 0x1fbad07: a target is locked, combat turns James; the stick does not
	bool attack_seq;          // 0x1fbad16 != 0xff: he has attacked since raising the weapon
	bool aim_walk, aim_run;   // the weapon's flags allow walking / running under it now
	bool cam_live;            // the port's behind camera turns smoothly: the 2D direction follows it
	bool stick_strafe;        // the behind camera's aim at a lock: the circle pad's x strafes
	bool keep_gait;           // an attack is under way (upper status 0x1c): 0x54e0f0 / 0x547bf0 keep the
	                          // lower state while the weapon's flags let it go on (combat_legs)
	bool digital;             // the D-Pad walks him (0x52e110: 0x1fbac6a 0, a direction key held): the
	                          // movers' key ramps instead of the stick's tilt
	// A stage program walks him to a point (0x53b010): auto_gait 0 stops, 1 walks, 2 runs, turning
	// him to auto_yaw. The stick and the keys are not read.
	bool autowalk;
	int auto_gait;
	float auto_yaw;
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
	bool scrape;              // the dragged great knife scrapes the floor this frame
	bool backward;            // the clip plays from its end (none does now)
} MoveOut;

enum {
	MV_STAND, MV_RELAX, MV_TIRED, MV_LROUND, MV_RROUND, MV_BACK, MV_WALK, MV_LSWALK, MV_RSWALK,
	MV_RUN1, MV_RUN2, MV_RUN3, MV_LSRUN, MV_RSRUN, MV_LTURN, MV_RTURN, MV_ALERT, MV_HOLD, MV_COUNT
};
// MV_HOLD is the PC's lower status 0x1a: standing with the weapon up, turned by the stick at
// 2.4 rad/s (3.6 with run held) while nothing is locked (0x54dcc0).

// The Options' Control Type (gControlType): 0 Rotational Style (3D, tank), 1 Directional Style (2D).
enum { MOVE_TANK, MOVE_2D };

void move_reset(void);
void move_update(const MoveInput* in, float* yaw, float dt, MoveOut* out);
// The last update's clip and frame, for whoever draws James while he is free.
const MoveOut* move_last(void);
// Something else (combat, a scene) animated James: the next free clip blends in from there.
void move_interrupt(void);
void move_set_infinite_stamina(bool on);   // Cheats: running never tires (no third pace limit, no panting)
const char* move_status(void);
// The weapon can come up now: the legs are in one of the states whose upper row of 0x6cf7f8 lets the
// upper body take status 0x1a (standing, idling, turning on the spot, already holding). Walking,
// running, backing off and the quick turn have to end first (0x557100).
bool move_can_aim(void);
// The PC's lower status number of the state (the camera's anim status, 0x1fba8e8).
int move_pc_status(int state);
