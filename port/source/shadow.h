#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "chr.h"

// Character shadows as the PC draws them: stencil shadow volumes built on the CPU from each
// character's .kg1 shadow model and James' flashlight, then one full-screen pass that darkens every
// pixel inside a volume. PC: FUN_00445540 (states, two stencil passes and the darkening quad),
// FUN_00444b20 / FUN_004449f0 (the spot light volumes from the .kg1 triangle strips), 0x5aad70 (up
// to four casters, light kind 0 = the flashlight), PS2 beta sh2shd/*.c for the names.
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
// debug.txt "shadowdebug N": 1 draws the volumes in red, 2 darkens every pixel whose count changed,
// 3 darkens the whole screen, 4 the pixels with an unchanged count, 5 counts on depth pass, 6 off,
// 7 drop shadows in red.
void shadow_debug(int mode);
