#pragma once
#include <3ds.h>

// Room collision (S3L v1, see tools/sh2conv/collision.py). World Y points down.

typedef struct { float v[9]; } FloorTri;
typedef struct { float x0, z0, x1, z1, y_top, y_bottom; u32 group, material; } WallSeg;

typedef struct {
	u32 n_floors, n_walls;
	FloorTri* floors;
	WallSeg* walls;
} Collision;

bool col_load(Collision* c, const char* path);
// The same from an .s3l already in memory (copied; the caller keeps data).
bool col_load_mem(Collision* c, const u8* data, long size);
void col_free(Collision* c);

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
void col_extra_wall(float x0, float z0, float x1, float z1, float y_top, float y_bottom);

// Every wall col_push tests, the room's and the extra ones (for path planning in playtest).
u32 col_wall_count(const Collision* c);
const WallSeg* col_wall(const Collision* c, u32 i);

// Pushes a vertical body (feet at y, height h) of the given radius out of the walls.
void col_push(const Collision* c, float* x, float* z, float y, float h, float radius);

// Clamps a camera on the segment from (tx, tz) to (*cx, *cz) at height y to stop short of walls.
void col_clamp_ray(const Collision* c, float tx, float tz, float* cx, float* cz, float y);

// A walkable spawn point: centroid of the floor triangle nearest to (x, z).
bool col_spawn(const Collision* c, float x, float z, float* sx, float* sy, float* sz);
