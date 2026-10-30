#pragma once
#include <3ds.h>
#include <citro3d.h>

// Character lighting as on the PC: the light list FUN_00508a90 builds from the room's DrawEnvData
// record (PS2 Chacter_Draw/light_n.c: parallels, spots, points, fake points and fake spots), the
// per-character choice of the strongest point and spot lights (0x50ce10, 0x5091c0) and the
// character vertex shader's formula (0x8a2ed8): lights x material diffuse (0.7) + ambient
// (2.1 x Ambient + 0.3 x flashlight level) x material ambient (0.3), at most 1. The PICA does it
// with the fragment lights of a light environment of the characters' own, packed from light 0
// (chrlight.c says why: sharing the game's, after its flashlight, turned them black on a console).
// Data: pack/chrlight.txt from tools/sh2conv/lighting.py. See LIGHTING.md section 10.

typedef struct {
	bool on;                     // dark room with the flashlight switched on, or a room's own spot
	bool room;                   // the spot is the room's (main.c room_spot): it lights James too
	float pos[3], dir[3];        // world space (Y down)
	float start, end, cone;      // the record's flashlight decay and half angle
} ChrFlash;

// Sets up the characters' light environment; env is the game's, bound again by chrlight_end.
void chrlight_init(C3D_LightEnv* env);
// The record lights of a room ("ap38", variant as for lighting.txt).
void chrlight_load(const char* pack_dir, const char* map, const char* variant);
// Before drawing characters: view matrix, the record Ambient (rgb), the flashlight level
// (0x8a2ba8), the brightness factor and the flashlight.
void chrlight_begin(const C3D_Mtx* view, const float ambient[3], float level, float bright, const ChrFlash* fl);
// Before each character's chr_draw: its root position. James does not light himself with his own
// flashlight.
void chrlight_at(const float pos[3], bool james);
// Port only (no PC counterpart): after chrlight_at, one more point light for that character, with
// the PC's falloff from start to end. Colours are in the range chrlight_at chose for it.
void chrlight_add_point(const float pos[3], const float col[3], float start, float end);
// After the characters: their lights off, the game's environment bound again.
void chrlight_end(void);
// Scales the ambient of the next characters (demo.c's EnemyRevealLighting factor), 1 = normal.
void chrlight_ambient_scale(float k);
const char* chrlight_status(void);
// 0x507e00 for a model at pos in the room loaded now: the strongest point or spot light it would be
// drawn with (0x50ccb0 picks by 0x50c3d0's influence at the radius 800 every model gets, 0x8a2ccc)
// is over 0.1 (0x8a2b60). EventCheckLookLine (0x48e520) asks it for items in a dark room while the
// flashlight is off.
bool chrlight_lit(const float pos[3]);
