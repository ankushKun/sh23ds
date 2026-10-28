#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "game/view/sh2cam.h"

// The game camera: the PC camera module (game/view/sh2cam.c) driven by each room's .cam roads.

typedef struct {
	float pos[3];        // feet position (Y down)
	float yaw;           // facing, forward = (sin yaw, cos yaw)
	float vel[2];        // world velocity x, z in units per second
	float turn_speed;    // yaw change in radians per second
	int stage, glb_crd;  // current stage id and its building group
	int control_type;    // gControlType: 0 = 3D (tank), 1 = 2D (camera relative)
	int anim_status;     // 0x1fba8e8: James' lower status (move_pc_status)
	// The character slots (0x1f991a0, the copy 0x542300 makes of James' candidates): enemies
	// nearest first, kind 0 past the last. NULL: none.
	SH2CamChara* chara;
} GameCamPlayer;

// Loads the .cam roads of every room in the area (<pack>/bg/<area>/<room>.cam); does nothing when
// the area is already loaded. No roads: the camera's built-in null road.
void gamecam_load_area(const char* pack_dir, const char* area, const char (*rooms)[16], int nrooms);

// Places the camera behind the player with no smoothing (new game, door warps).
void gamecam_reset(const GameCamPlayer* p);

// One frame of dt seconds (in->frame_ticks60 = dt * 60). Writes the view matrix and the vertical field of view in radians.
void gamecam_update(const GameCamPlayer* p, float dt, bool far_view, C3D_Mtx* view, float* fovy);

// Camera yaw (forward = sin, cos) of the last frame, for camera-relative controls.
float gamecam_yaw(void);

bool gamecam_has_roads(void);
int gamecam_road_count(void);
int gamecam_mv_type(void);
const float* gamecam_eye(void);
const float* gamecam_watch(void);  // the camera's look-at target (watch_tgt_pos)
float gamecam_scr_z(void);          // gCameraFOV in 448-line screen units
float gamecam_fov(void);       // radians
// The PC camera is on a shot the game sets itself: an event road of the .cam data or a camera a
// stage program placed (user camera or watch). The port's behind camera leaves those to it.
bool gamecam_scripted(void);

// James rows the boat (his status bit 0x20000): the camera takes the PC's boat parameters.
void gamecam_on_boat(bool on);
