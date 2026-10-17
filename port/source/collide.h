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

// Pushes a vertical body (feet at y, height h) of the given radius out of the walls.
void col_push(const Collision* c, float* x, float* z, float y, float h, float radius);

// Clamps a camera on the segment from (tx, tz) to (*cx, *cz) at height y to stop short of walls.
void col_clamp_ray(const Collision* c, float tx, float tz, float* cx, float* cz, float y);

// A walkable spawn point: centroid of the floor triangle nearest to (x, z).
bool col_spawn(const Collision* c, float x, float z, float* sx, float* sy, float* sz);
