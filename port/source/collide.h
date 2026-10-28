#pragma once
#include <3ds.h>
#include <stdio.h>

// Room collision (S3L, see tools/sh2conv/collision.py). World Y points down.

typedef struct { float v[9]; } FloorTri;
// A wall stops bodies only from its front, as the PC's do: clCheckColumn2WallHit (0x522b50) pushes a
// column only while n . (c - p2) >= 0, c its centre at the place tested and n = (p2 - p1) x (p1 - p0)
// the polygon's normal (0x529b10 with the cross product 0x4d0cc0), and the line tests (0x528840
// triangles, 0x528350 quads) hit a wall only going from its front to its back. A one-sided wall's
// front is where col_wall_side is >= 0: the converters order the ends so. Walls from a pack made
// before (S3L and stagewalls.bin version 1) have no facing and stop from both sides.
typedef struct { float x0, z0, x1, z1, y_top, y_bottom; u32 group, material; bool one_sided; } WallSeg;

static inline float col_wall_side(const WallSeg* w, float x, float z)
{
	return (x - w->x0) * (w->z1 - w->z0) - (z - w->z0) * (w->x1 - w->x0);
}
// Whether a body centred at (x, z) meets the wall: in front of it, or either side of a two-sided one.
static inline bool col_wall_faces(const WallSeg* w, float x, float z)
{
	return !w->one_sided || col_wall_side(w, x, z) >= 0.0f;
}
// A column of the .cld (_CL_HITPOLY_COLUMN): the poles and tree trunks of the outdoor rooms
typedef struct { float x, z, y_bottom, y_top, radius; } ColumnSeg;

typedef struct {
	u32 n_floors, n_walls, n_cols;
	FloorTri* floors;
	WallSeg* walls;
	ColumnSeg* cols;
} Collision;

#define S3L_VERSION 2     // 1: two-sided walls; 2: the walls' ends in the order that gives their front
bool col_load(Collision* c, const char* path);
// The same from an .s3l already in memory (copied; the caller keeps data).
bool col_load_mem(Collision* c, const u8* data, long size);
void col_free(Collision* c);
// n walls as an S3L of the given version holds them (32 bytes each), read from f into w.
bool col_read_walls(FILE* f, WallSeg* w, u32 n, u32 version);

// Replaces out's contents with the floors and walls of all parts (neighbouring rooms resident at
// once). out keeps its address, so code holding a pointer to it sees the new set.
bool col_merge(Collision* out, const Collision* const* parts, int n);

// Floor height under (x, z) closest to y_ref within max_step; false if there is no floor.
bool col_floor(const Collision* c, float x, float z, float y_ref, float max_step, float* y_out);
// The highest floor under (x, z) from up above y_ref to down below it (Y down): James' steps.
bool col_floor_climb(const Collision* c, float x, float z, float y_ref, float up, float down, float* y_out);

// Walls the stage code adds for the current frame (the PC's per-frame collision lists 0x1f98680 and
// 0x1f92880, filled by 0x5228e0 / 0x522910): every collision set pushes against them as well.
void col_extra_clear(void);
// one_sided: its front is where col_wall_side is >= 0 (the PC's are all one-sided).
void col_extra_wall(float x0, float z0, float x1, float z1, float y_top, float y_bottom, bool one_sided);

// Every wall col_push tests, the room's and the extra ones (for path planning in playtest).
u32 col_wall_count(const Collision* c);
const WallSeg* col_wall(const Collision* c, u32 i);

// Pushes a vertical body (feet at y, height h) of the given radius out of the walls it is in front of
// and the columns.
void col_push(const Collision* c, float* x, float* z, float y, float h, float radius);

// Whether the path from (x0, z0) to (x1, z1) crosses a wall col_push would push the same body out of,
// going from its front to its back (from behind, a one-sided wall lets a body through).
bool col_crosses(const Collision* c, float x0, float z0, float x1, float z1, float y, float h);
// How far a body of the given radius at (x, z) stands from the nearest wall or column col_push would
// push it from (radius when none is that close; less, down to 0 and below, inside one's reach).
float col_clearance(const Collision* c, float x, float z, float y, float h, float radius);

// Clamps a camera on the segment from (tx, tz) to (*cx, *cz) at height y to stop short of the walls
// it meets from their front.
void col_clamp_ray(const Collision* c, float tx, float tz, float* cx, float* cz, float y);

// A walkable spawn point: centroid of the floor triangle nearest to (x, z).
bool col_spawn(const Collision* c, float x, float z, float* sx, float* sy, float* sz);
