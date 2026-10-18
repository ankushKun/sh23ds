#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include "chr.h"
#include "collide.h"

// The rowing boat on Toluca Lake (stage 38): the PC's boat character 0x10b and James in boat mode
// (0x20000 in his status), 0x53fe40 and the functions it runs each frame. The embark program
// (0x57dd20) puts both on the water and the rowing starts; the disembark program (0x57de40) leaves
// the boat at the hotel pier. While James rows, his place is the boat's, the camera takes the PC's
// boat parameters (gamecam_on_boat) and the boat stage's time and top speed go to the results.

typedef struct {
	float x, y;          // circle pad, -1..1 (y forward), dead zone already taken off
	bool up, down, left, right;   // the D-pad (the PC's arrow keys)
	bool run;            // run held: full strokes, half without (0x1fbab0d)
} BoatInput;

// Once a frame from the game loop, rowing or not: loads the boat in stage 38 and frees it outside,
// lays it at the town pier until James takes it (room function 0x57e7a0) and plays its motions.
void boat_tick(int stage, float dt);
// James rows (the PC's status bit 0x20000).
bool boat_active(void);
// EvProgEmbarkBoat step 0xd: James and the boat on the water at 0x8cf688 / 0x8cf698, facing -pi/2.
void boat_embark(void);
void boat_embark_at(float x, float z, float yaw);    // debug: in the boat somewhere else
// EvProgDisembarkBoat step 0xd: James is off; the boat lies at the hotel pier (0x8cf6dc, facing
// pi, motion 54).
void boat_disembark(void);
// New game, a load or a warp out of the lake: no ride.
void boat_reset(void);

// One frame of rowing: the PC's 0x53e760 (keys), 0x53ee20 (speed and turn), 0x53f9b0 and the lake
// waves 0x559b40 (rocking), 0x53f3c0 (stroke speed) and 0x53f5b0 (which oars). Writes James' place
// (the boat's), his velocity in units per second and the turn rate for the camera. pos and yaw come
// in as where James is: if something else moved him, the boat follows.
void boat_update(const BoatInput* in, const Collision* col, float dt, float pos[3], float* yaw,
                 float vel[2], float* turn_speed);

// The boat where it is (skin program and lights bound, as stagemdl_draw).
void boat_draw(const C3D_Mtx* view, int uModelView, int uBones);
// While James rows: his model matrix (the boat's place, turn and roll) and his pose from
// jms_boat, in step with the oars. False when he does not row (the caller poses him itself).
bool boat_james_model(C3D_Mtx* model);
bool boat_james_pose(Character* james, float dt);

const char* boat_status(void);
