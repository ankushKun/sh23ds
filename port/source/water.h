#pragma once
#include <3ds.h>
#include <citro3d.h>

// The outdoor water surfaces: the PC's Effect2 water classes that HH_Effect_Object_AutoPost
// (0x4ca6d0, table 0x8942c8) posts for the room the stage connects into (0x401480 -> 0x4c71b0):
//   class 42 (main 0x4d5210, draw 0x4d45e0)  room 0xe, the lake (glb 4, cd): a 35 x 35 grid of
//              400-unit cells kept round James, opaque
//   class 43 (main 0x4d3c20, draw 0x4d2fe0)  room 3, the forest (glb 1, ca): a fixed 52 x 34 grid
//              of 300-unit cells over the cemetery lake (ca13), blended
// Each frame (30 a second) the class adds up its waves (the Effect2 water common code, 0x4d1120)
// into the grid's heights, shades every vertex from the facet's normal and a fixed light direction,
// fades it to grey 70 between the fog's start and end itself (D3D fog off) and draws one triangle
// strip per row with no texture. Here the vertices are built on those updates and the fade is the
// GPU's fog with a table of the PC's line. See PROGRAMS.md "Water".

// Sets the program for world-space triangles with the vertex colour passed through (main.c).
typedef void (*WaterBindFn)(const C3D_Mtx* proj, const C3D_Mtx* view);

bool water_init(WaterBindFn bind);
void water_exit(void);

// What the classes read each frame: the PC room id (bgm_room_id), James' place (0x4ca0f0 reads
// the player's x y z), whether a scripted scene runs (0x934d94 & 0x40) and the game flags they test.
typedef struct {
	int room;
	float player[3];
	bool scene;
	bool flag379, flag618, flag621;
	float light[3];              // the flashlight colour (0x9456b0, the record's SpotCol0)
	bool low;                    // the Old 3DS quality: the vertices half as often
} WaterInput;

// A change of room drops the instance and posts the new room's class, as the connect does.
void water_update(const WaterInput* in, float dt);
void water_reset(void);              // a new game, a load: nothing carries over

// After the opaque scene and the characters. fog_start / fog_end are the room's D3D fog distances
// (0x1f61a70 / 0x1f61a74), near / far the projection's. The PC draws the water with D3D fog off and
// fades it itself; this binds its own fog colour and table, so the caller binds the room's again.
void water_draw(const C3D_Mtx* proj, const C3D_Mtx* view, float fog_start, float fog_end, float near, float far);
bool water_active(void);
const char* water_status(void);      // debug HUD line
