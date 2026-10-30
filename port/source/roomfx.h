#pragma once
#include <3ds.h>
#include <citro3d.h>

// The effects a room posts when James enters it (the PC's Effect2 "impact queue" classes; the
// room loader 0x401480 calls 0x4c71b0, which clears every instance and posts the classes the
// table at 0x8942c8 gives the room id, 0x4ca6d0 HH_Effect_Object_AutoPost). Two rooms are ported:
//   0xaa (ru21, the burning stairs, stage 47)  class 46 (0x4e1340): while fire_agl plays it posts
//                flames (classes 47..49, data/pic/effect/fire01.tbn2) and their smoke (class 52,
//                playable0.tbn2) from a timed table, and when the scene is over a fixed set
//   0x0e (the lake, cd01..cd05)               class 56 (0x4dbcd0): the light over the far shore
//                that the boat steers by (flear.tbn2), brighter and larger as the camera nears it
// The lake's water surface (class 42) and the cemetery's (class 43) are water.c's; the lake's
// classes 22 and 23 (the boat mask) are not ported.
// Textures: pack/effect/fire.t3x, smoke.t3x and flare.t3x (tools/sh2conv/ui.py write_room_fx).

// Sets the program for world-space triangles with the vertex colour passed through (main.c).
typedef void (*RoomFxBindFn)(const C3D_Mtx* proj, const C3D_Mtx* view);

bool roomfx_init(const char* pack_dir, RoomFxBindFn bind);   // false: a texture is missing
void roomfx_exit(void);
// A room was loaded (the PC's 0x401480): the next update clears everything and posts the room's
// classes again.
void roomfx_room_loaded(void);
// One game frame (dt 0 while paused): the classes run in steps of 1/30 s as on the PC. room is the
// PC room id (bgm_room_id); a change of it counts as entering a room too.
void roomfx_update(float dt, int room);
// After the opaque scene, with the light environment unbound: the flames and the smoke, then the
// glow posted for the frame.
void roomfx_draw(const C3D_Mtx* proj, const C3D_Mtx* view);
// Port only (no PC counterpart): a soft additive glow at a world place for the next roomfx_draw,
// a camera-facing quad of these half sizes; rgba 0..255, alpha the strength. Post it every frame.
void roomfx_glow(const float at[3], float half_w, float half_h, const float rgba[4]);
// The lake's light, drawn in front of everything (the PC puts it at screen depth 0.01).
void roomfx_draw_flare(const C3D_Mtx* proj, const C3D_Mtx* view);
const char* roomfx_status(void);         // debug HUD line
