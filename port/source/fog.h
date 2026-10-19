#pragma once
#include <3ds.h>
#include <citro3d.h>

// The fog particle layer (Fog/fog.c on the PS2, 0x483820..0x48a0d0 in the PC Director's Cut): a few
// hundred soft sprites drifting with the wind in a box around the camera's target, pushed aside
// by James, plus one full-screen scrolling fog layer on top. Data: pack/fog/*.fcl (fog areas and
// their parameters) and pack/fog/fog.t3x, written by tools/sh2conv/fog.py. See LIGHTING.md.

typedef struct {
	int glb, room, stage;        // Stage_Data.glb_crd, the PC room id (FUN_00495220), stage id
	float james[3];              // feet position (Y down)
	float eye[3], watch[3];      // camera position and its look-at point
	float scr_z;                 // gCameraFOV: projection distance in 448-line screen units
	bool dark;                   // record dark flag (FUN_0047b1f0): fewer particles
	int demo_no;                 // running scene (demo.c demo_number), -1 none
	float demo_timer;            // gCutsceneTimer
	bool flag508;                // game flag 508 (0x1f7d61c bit 28): the ending scenes
	float dt;                    // seconds, 0 while paused
} FogFrame;

bool fog_init(const char* pack_dir);   // texture and shader; false: the layer stays off
void fog_exit(void);
void fog_reset(void);                  // 0x489af0, on every stage connect
// The record's fog particle colour (FUN_004879c0, MoveFogCol); r < 0 keeps the current one.
void fog_set_color(int r, int g, int b, int a);
void fog_set_clip(float near, float far);   // the projection's near and far planes, the sprites are cut to them
void fog_update(const FogFrame* f);    // 0x489b90 without the draw
// 0x488a80 + 0x4876c0: sorted sprites, then the screen layer. The caller has the scene drawn,
// the light environment unbound and the top screen target bound.
void fog_draw(const C3D_Mtx* proj, const C3D_Mtx* view, const C3D_Mtx* ortho);
const char* fog_status(void);          // debug HUD line
// debug.txt "fogdebug N": 1 draws only the blow puffs, 2 leaves the walls out, 3 draws only the
// puffs at eight times their alpha
void fog_debug(int mode);

// The console test (SELECT + R in play): 0 normal, then one part of the particle drawing taken away.
int fog_debug_mode(void);
const char* fog_debug_name(void);
int fog_debug_cycle(void);
