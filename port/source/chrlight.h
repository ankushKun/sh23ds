#pragma once
#include <3ds.h>
#include <citro3d.h>

// Character lighting as on the PC: the light list FUN_00508a90 builds from the room's DrawEnvData
// record (PS2 Chacter_Draw/light_n.c: parallels, spots, points, fake points and fake spots), the
// per-character choice of the strongest point and spot lights (0x50ce10, 0x5091c0) and the
// character vertex shader's formula (0x8a2ed8): lights x material diffuse (0.7) + ambient
// (2.1 x Ambient + 0.3 x flashlight level) x material ambient (0.3), at most 1. The PICA does it
// with fragment lights 1..7 of the game's light environment (light 0 stays the map's flashlight).
// Data: pack/chrlight.txt from tools/sh2conv/lighting.py. See LIGHTING.md section 10.

typedef struct {
	bool on;                     // dark room with the flashlight switched on
	float pos[3], dir[3];        // world space (Y down)
	float start, end, cone;      // the record's flashlight decay and half angle
} ChrFlash;

// Hooks the extra lights into the game's light environment (call once after it is set up).
void chrlight_init(C3D_LightEnv* env);
// The record lights of a room ("ap38", variant as for lighting.txt).
void chrlight_load(const char* pack_dir, const char* map, const char* variant);
// Before drawing characters: view matrix, the record Ambient (rgb), the flashlight level
// (0x8a2ba8), the brightness factor and the flashlight.
void chrlight_begin(const C3D_Mtx* view, const float ambient[3], float level, float bright, const ChrFlash* fl);
// Before each character's chr_draw: its root position. James does not light himself with his own
// flashlight.
void chrlight_at(const float pos[3], bool james);
// After the characters: lights 1..7 off again for the map.
void chrlight_end(void);
// Scales the ambient of the next characters (demo.c's EnemyRevealLighting factor), 1 = normal.
void chrlight_ambient_scale(float k);
const char* chrlight_status(void);
