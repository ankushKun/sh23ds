#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "chr.h"

// Character and map shadows as the PC draws them: stencil shadow volumes built on the CPU from each
// character's .kg1 and each map's .kg2 shadow model and James' flashlight, then one full-screen pass
// that darkens every pixel inside a volume. PC: FUN_00445540 (states, two stencil passes and the
// darkening quad), FUN_00444b20 / FUN_004449f0 (the spot light volumes from the strips, planes and
// billboards), 0x5aad70 (the light, kind 0 = the flashlight; up to four maps), PS2 beta sh2shd/*.c
// for the names.
// Characters themselves receive no shadow, as on the PC. See LIGHTING.md section 11.

bool shadow_init(void);
void shadow_exit(void);

// Before the characters are drawn: the light and how dark a shadowed pixel gets (0 = no shadows,
// 1 = black). Off when strength <= 0.
void shadow_begin(const float pos[3], const float dir[3], float cone, float strength);
// Instead of shadow_begin, for a light from far away along dir (light kind 4, which 0x5aad70 uses in
// hotel room 312): the volumes are cast along dir from every character, with no range or cone.
void shadow_begin_parallel(const float dir[3], float strength);
// Instead of shadow_begin, in the lit rooms (and dark ones whose record has no ShadowDarkNess):
// the PC's drop shadows, every character's .kg1 flattened onto the floor under it (0x5a7c90).
void shadow_begin_drop(int map_id, const float cam[3]);
// After chr_pose: the character's shadow volume (or drop shadow), if it has a .kg1.
void shadow_cast(const Character* c, const float pos[3], float yaw);
// James (and his weapon), which the PC never casts from his own flashlight's spot: his drop shadow
// in the drop mode, and with light non-NULL a point light's volume (kind 5 of sh2shd_get_jms_shadow_env
// 0x5acaf0: room 205's spot), every part, with no range or cone (block 999), each vertex pulled
// 6000 from the light (0x5a90e0's 5000 x 1.2). He does not count among the four casters.
void shadow_cast_james(const Character* c, const float pos[3], float yaw, const float* light);
// The maps' shadow models (.kg2 next to each room, tools/sh2conv/shadows.py): the bars, railings,
// furniture and the like the PC casts under the flashlight with the characters (0x5aad70 ->
// 0x5a4ed0, 0x444b20, 0x4449f0; LIGHTING.md section 11). shadow_maps_set names the rooms resident
// this frame, the primary first (read through stream.c); the first four that have one cast.
void shadow_maps_set(const char* area, const char* const* rooms, int n);
typedef struct {
	int map_id;                  // the record id of the map James is in (0x5abf70: glb << 16 | map)
	int darkness;                // its ShadowDarkNess (0x47bb00)
	bool flashlight;             // James' flashlight on (0x50a190, 0x48c840)
	float james[3], yaw;         // his place and turn (SubCharacter pos, rot.y)
	float cam[3];                // the eye (0x51f950)
} ShadowMapCtx;
// After shadow_begin: the map volumes under its light.
void shadow_cast_maps(const ShadowMapCtx* c);
// sh2shd_off_obj (0x5a5560 / 0x5a55e0): map's object obj casts nothing. The stage code lists them
// each frame (stageparts.c), after shadow_maps_off_begin.
void shadow_maps_off_begin(void);
void shadow_map_off(int map, int obj);
// A stage-placed model (stagemdl.c) after its pose: the PC adds kinds 0x400..0x5ff with a .kg1 to
// its shadow characters like the others (0x50b5d0 -> 0x50b210 -> 0x5a5c70), save a few by the
// room (0x5abcd0). The pickups (0x7xx) cast none.
void shadow_cast_placed(const Character* c, const C3D_Mtx* model, int kind);
// Around character draws: their pixels are marked so the darkening pass leaves them alone.
void shadow_mark_begin(void);
void shadow_mark_end(void);
// After the characters: counts the volumes in the stencil buffer and darkens the shadowed pixels.
void shadow_draw(const C3D_Mtx* proj, const C3D_Mtx* view, const C3D_Mtx* ortho);

// The stencil value the frame must be cleared to (C3D_RenderTargetClear's depth word, top byte).
#define SHADOW_STENCIL_CLEAR 0x80000000u

// Enhanced Edition EnableSoftShadows opacity (of 255) for a room (bgm_room_id) and chapter.
int shadow_ee_opacity(int room_id, bool born_from_a_wish);
const char* shadow_status(void);
// Each frame before the casts: the camera and its far plane, which the volumes' far caps stay inside.
void shadow_view(const float cam[3], float far);
// debug.txt "shadowdebug N": 1 draws the volumes in red, 2 darkens every pixel whose count changed,
// 3 darkens the whole screen, 4 the pixels with an unchanged count, 5 counts on depth pass, 6 off,
// 7 drop shadows in red, 8 no map volumes.
void shadow_debug(int mode);
