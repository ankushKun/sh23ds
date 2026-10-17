#pragma once
#include <3ds.h>

// Per-room light and fog from the PC's DrawEnvData records (pack/lighting.txt, written by
// tools/sh2conv/lighting.py; see LIGHTING.md). The map is lit per vertex like the PC shaders:
// three directional lights plus the c19..c22 constants, with the material mode choosing whether the
// baked vertex colour is added (mode 4) or the material colour scales the light (modes 1, 2, 6).

typedef struct {
	int map_id;                  // glb << 16 | map number (the PC's current map id, 0x945ad0)
	bool dark;                   // flashlight room: light 0 is James' flashlight
	float amb[3], bamb[3], bcol[3], vamb[3];   // record Ambient, BaseAmbient, BaseColor, VertexAmbient
	float fog_rgb[3];            // 0..255
	float fog_start, fog_end, fog_near_f, fog_far_f;   // factors 0..255, 255 = no fog
	float clear_rgb[3];          // 0..255
	float clip_near, clip_far;
	float ldir[3][3];            // direction the light travels
	float lcol[3][3];            // shader colour (record colour x 0.25)
	float fl_col[3], fl_start, fl_end, fl_cone;
	int fogp[4];                 // fog particle colour RGBA (MoveFogCol), -1: the record keeps the last
	int shadow;                  // record ShadowDarkNess (0x47bb00): 0 = no character shadows, -1 unknown
} RoomEnv;

// Shader constants for the current flashlight level (0x8a2ba8: 0 with the light on, see
// env_flash_off_level for the light off).
typedef struct {
	float c19[3], c20[3], c21[3], c22[3];
} RoomEnvConst;

// Finds the line for a bg map ("ap38") and variant ("-" default, "night", "flag251", "bfaw").
// Falls back to the default variant, then to a lit default when the map is unknown.
bool env_load(const char* pack_dir, const char* map, const char* variant, RoomEnv* out);
void env_constants(const RoomEnv* e, float level, RoomEnvConst* out);
// Enhanced Edition PS2 flashlight brightness for the rooms loaded after this (on by default).
void env_set_ps2_flashlight(bool on);

// FUN_0050a010: the level a dark room uses while the flashlight is off. Some maps have their own,
// otherwise 1.0 before the flashlight is found and 0.3 after (also after the room 312 videotape,
// game flag 625).
float env_flash_off_level(const RoomEnv* e, bool have_flashlight, bool flag625);

// Enhanced Edition "RoomLightingFix": fixed levels for a few lit rooms where the PC keeps whatever
// the last dark room left. Returns false when the room keeps the level it has.
bool env_ee_room_level(int pc_room_id, bool born_from_a_wish, const RoomEnv* e, float* level);
