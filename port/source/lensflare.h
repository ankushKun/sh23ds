#pragma once
#include <3ds.h>
#include <citro3d.h>

// The PC's lens flare (Advanced Options "Lens Flare", 0xa36000, on by default as 0x4f6ae0 sets it):
// a glow, a ring, streaks and ghosts round a lamp that faces the camera in a dark room. The lamp is
// James' flashlight, or the room record's own spot where one is on (room 205 until the flashlight
// lying on the mannequin is taken). lensflare.c has the PC's code; the sprites are
// pack/effect/lensflare.t3x (tools/sh2conv/ui.py write_lens_flare).

// Sets the program for world-space triangles with the vertex colour passed through (main.c).
typedef void (*LensFlareBindFn)(const C3D_Mtx* proj, const C3D_Mtx* view);
// Nothing drawn lies between a and b, the PC's depth-buffer test (0x511170). james: James counts
// too (a lamp that is not his own).
typedef bool (*LensFlareClearFn)(const float a[3], const float b[3], bool james);

enum { LF_NONE, LF_ROOM, LF_JAMES };

bool lensflare_init(const char* pack_dir, LensFlareBindFn bind, LensFlareClearFn clear);   // false: no texture
void lensflare_exit(void);
// This frame's lamp, before lensflare_draw: LF_ROOM the record's spot (place +0xd0, aim +0xe0),
// LF_JAMES his flashlight's lamp (0x533960), LF_NONE none (the option off, a lit room, a scene, the
// flashlight off: 0x511b80 clears the flare). bright: the flashlight colour (gFlashlightBrightness,
// 0x9456b0).
void lensflare_light(int kind, const float pos[3], const float dir[3], float bright);
// Last of the scene, in each eye (the first eye steps it by dt, 0 while paused). view: the camera's;
// scr_z: its projection distance in 448-line units (gCameraFOV).
void lensflare_draw(const C3D_Mtx* proj, const C3D_Mtx* view, float scr_z, float dt);
const char* lensflare_status(void);      // a line for the frame dumps
