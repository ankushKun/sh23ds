#pragma once
// The map screen and the memo list (PC Event/map.c 0x49a7e0..0x49e310, Event/memo.c
// 0x4989c0..0x49a700), with their tables from pack/map/map.dat (tools/sh2conv/maps.py).
#include <3ds.h>
#include <citro3d.h>

// 2D drawing service from main.c: a triangle list in top-screen pixels (0..400 x 0..240, y down),
// textured when tex is not NULL (u, v 0..1 from the image's top left), tinted by rgba.
typedef struct { float x, y, u, v; } MapVert;
typedef void (*MapDrawFn)(C3D_Tex* tex, const MapVert* v, int n, u32 rgba);

bool map_init(MapDrawFn draw);

// Where the player is (James, or Maria in Born From a Wish).
typedef struct {
	float x, y, z, yaw;
	int glb_crd;             // Stage_Data.glb_crd of the current stage
	int chapter;             // gChapterID: 1 in Born From a Wish
} MapPlayer;

// Opening (the PC's map key, 0x48ef33): MAP_DARK when the room is dark and no light is on
// ("It's too dark to read the map.", common message 2; the caller checks that), MAP_NONE when
// there is no map of this area (common message 3); with the town map the screen then shows it.
enum { MAP_OPEN, MAP_NONE, MAP_NONE_TOWN };
int map_open(const MapPlayer* p);
bool map_is_open(void);
// The marking animation a program starts (0x49c0f0 mode, then 0x49dbd0): modes 0..4. The screen
// runs by itself (map_update returns false when it has closed); map_marking is true meanwhile.
bool map_mark_open(int mode, const MapPlayer* p);
bool map_marking(void);
// One frame while open; false once closed (B / START). down / held are this frame's keys.
bool map_update(u32 down, u32 held, const circlePosition* cp, float dt);
void map_draw(void);                 // top screen, inside C3D_FrameBegin / End
void map_close(void);
const char* map_legend(void);        // the shown map's name and the keys, for the bottom screen
const char* map_name(void);          // the shown map's name
bool map_zoomed(void);               // the screen is zoomed in (the D-pad scrolls)
// The touch screen's small map (touchui.c): the map James is on, centred on him, zoom times the
// screen's zoomed-out size, with the red markings and his arrow, through fn into (x0, y0)-(x1, y1)
// in the caller's pixels. False when he has no map of where he is (or the screen is open).
bool map_mini(const MapPlayer* p, MapDrawFn fn, float x0, float y0, float x1, float y1, float zoom);
// Changes whenever what map_mini draws would (another map, James moved a pixel, a new mark).
u32 map_mini_key(const MapPlayer* p, float zoom);

// Memo list: the memos picked up so far (each memo has a game flag).
int memo_count(void);
int memo_list(int* out, int max);    // memo indices in the PC's order
const char* memo_title(int memo);
const char* memo_text(int memo);     // with the safe / box codes put in, see prog_memo_text
const char* memo_picture(int memo, int which);   // still name for host pictures, NULL if none
int memo_kind(int memo);             // 0 picture, 1 text on the memo paper, 2 picture then text
