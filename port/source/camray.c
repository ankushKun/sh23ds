#include "camray.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// camray.h says what this is. Each room's opaque triangles are binned once into a grid of cells on
// the ground plane (kept while the room stays loaded); a ray walks
// the cells under it nearest first and stops once a hit lies before the cell it is in. The rooms'
// own chunk boxes follow their drawing order and each can span much of a room: walking those
// tested about 2,600 triangles a ray.

#define MAX_ROOMS 16
#define CELL 300.0f                      // world units (500 to the metre)
#define MAX_CELLS 64                     // a side; larger areas get larger cells
#define STEP 1500                        // triangles a frame for a grid made during play (camray_set)
#define REF(b, t) ((u32)(b) << 18 | (u32)(t))

// A grid is made in three passes over the room's opaque triangles (their bounds, the refs per cell,
// the refs), all at once behind a door's black (camray_warm) or STEP triangles a frame for a room
// streamed in during play. Until it is ready that room's triangles are not tested.
enum { G_BOUNDS, G_COUNT, G_FILL, G_READY, G_FAILED };

typedef struct {
	const Room* room;                    // the room it is for, found by its vertices (NULL: free)
	const void* vertices;
	u32 n_vertices, n_indices;
	int state;
	u32 bi, ti;                          // where the pass goes on: batch, triangle
	float lo[2], hi[2];                  // the bounds on the ground plane
	float x0, z0, cell;
	int nx, nz;
	u32* start;                          // nx * nz + 1: cell c's refs are start[c] .. start[c + 1]
	u32* refs;                           // REF(batch, triangle)
	u32* cur;                            // the fill pass's next ref of each cell
} Grid;

static const Room* s_rooms[MAX_ROOMS];
static int s_n;
static Grid s_grid[MAX_ROOMS];
u32 camray_stat[4];                      // rays, cells walked, triangles tested, grids made (tpcam's count)

static bool opaque(const RoomBatch* b) { return b->group == GROUP_OPAQUE && b->indices; }

static const float* vtx(const Room* r, u16 i) { return (const float*)((const u8*)r->vertices + i * ROOM_VERTEX_SIZE); }

static void grid_free(Grid* g)
{
	free(g->start);
	free(g->refs);
	free(g->cur);
	memset(g, 0, sizeof *g);
}

static bool same(const Grid* g, const Room* r)
{
	return g->vertices == r->vertices && g->n_vertices == r->n_vertices && g->n_indices == r->n_indices;
}

static bool listed(const Grid* g)
{
	for (int k = 0; k < s_n; k++)
		if (s_rooms[k] && same(g, s_rooms[k]))
			return true;
	return false;
}

// The room's grid; make: start one when there is none (in a free place, or one whose room is gone)
static Grid* find(const Room* r, bool make)
{
	for (int k = 0; k < MAX_ROOMS; k++)
		if (s_grid[k].vertices && same(&s_grid[k], r)) {
			s_grid[k].room = r;          // the neighbours' structs move when one is dropped
			return &s_grid[k];
		}
	if (!make)
		return NULL;
	for (int k = 0; k < MAX_ROOMS; k++) {
		Grid* g = &s_grid[k];
		if (g->vertices && listed(g))
			continue;
		grid_free(g);
		g->room = r, g->vertices = r->vertices, g->n_vertices = r->n_vertices, g->n_indices = r->n_indices;
		g->state = G_BOUNDS;
		return g;
	}
	return NULL;
}

// The cells a triangle's ground-plane box covers
static void cells_of(const Grid* g, const Room* r, const u16* ix, int c[4])
{
	float lx = 1e30f, hx = -1e30f, lz = 1e30f, hz = -1e30f;
	for (int k = 0; k < 3; k++) {
		const float* p = vtx(r, ix[k]);
		lx = fminf(lx, p[0]), hx = fmaxf(hx, p[0]), lz = fminf(lz, p[2]), hz = fmaxf(hz, p[2]);
	}
	c[0] = (int)((lx - g->x0) / g->cell), c[1] = (int)((lz - g->z0) / g->cell);
	c[2] = (int)((hx - g->x0) / g->cell), c[3] = (int)((hz - g->z0) / g->cell);
	c[0] = c[0] < 0 ? 0 : c[0], c[1] = c[1] < 0 ? 0 : c[1];
	c[2] = c[2] >= g->nx ? g->nx - 1 : c[2], c[3] = c[3] >= g->nz ? g->nz - 1 : c[3];
}

// The end of a pass: the next one set up
static bool pass_end(Grid* g)
{
	g->bi = g->ti = 0;
	if (g->state == G_BOUNDS) {
		if (g->lo[0] > g->hi[0] || g->room->n_batches >= (1u << 14))
			return false;
		g->cell = fmaxf(CELL, fmaxf(g->hi[0] - g->lo[0], g->hi[1] - g->lo[1]) / MAX_CELLS + 1.0f);
		g->x0 = g->lo[0], g->z0 = g->lo[1];
		g->nx = (int)((g->hi[0] - g->lo[0]) / g->cell) + 1, g->nz = (int)((g->hi[1] - g->lo[1]) / g->cell) + 1;
		g->start = calloc(g->nx * g->nz + 1, sizeof(u32));
		g->state = G_COUNT;
		return g->start != NULL;
	}
	int ncell = g->nx * g->nz;
	if (g->state == G_COUNT) {
		for (int k = 0; k < ncell; k++)
			g->start[k + 1] += g->start[k];
		g->cur = malloc(sizeof(u32) * ncell);
		g->refs = malloc(sizeof(u32) * (g->start[ncell] ? g->start[ncell] : 1));
		if (!g->cur || !g->refs)
			return false;
		memcpy(g->cur, g->start, sizeof(u32) * ncell);
		g->state = G_FILL;
		return true;
	}
	free(g->cur);
	g->cur = NULL;
	g->state = G_READY;
	camray_stat[3]++;
	return true;
}

// Up to budget triangles more of g's passes (0: to the end); true when it is ready.
static bool grid_step(Grid* g, u32 budget)
{
	const Room* r = g->room;
	if (!g->bi && !g->ti && g->state == G_BOUNDS)
		g->lo[0] = g->lo[1] = 1e30f, g->hi[0] = g->hi[1] = -1e30f;
	for (u32 done = 0; g->state < G_READY;) {
		if (g->bi >= r->n_batches) {
			if (!pass_end(g)) {
				const Room* keep = g->room;
				grid_free(g);
				g->room = keep, g->vertices = keep->vertices, g->n_vertices = keep->n_vertices;
				g->n_indices = keep->n_indices, g->state = G_FAILED;   // out of memory: its rays go without
				return false;
			}
			continue;
		}
		const RoomBatch* b = &r->batches[g->bi];
		if (g->ti >= (opaque(b) ? b->index_count / 3 : 0)) {
			g->bi++, g->ti = 0;
			continue;
		}
		if (budget && done++ >= budget)
			return false;
		const u16* ix = b->indices + g->ti * 3;
		if (g->state == G_BOUNDS) {
			for (int k = 0; k < 3; k++) {
				const float* p = vtx(r, ix[k]);
				g->lo[0] = fminf(g->lo[0], p[0]), g->hi[0] = fmaxf(g->hi[0], p[0]);
				g->lo[1] = fminf(g->lo[1], p[2]), g->hi[1] = fmaxf(g->hi[1], p[2]);
			}
		} else {
			int c[4];
			cells_of(g, r, ix, c);
			for (int z = c[1]; z <= c[3]; z++)
				for (int x = c[0]; x <= c[2]; x++) {
					int k = z * g->nx + x;
					if (g->state == G_COUNT)
						g->start[k + 1]++;
					else
						g->refs[g->cur[k]++] = REF(g->bi, g->ti);
				}
		}
		g->ti++;
	}
	return g->state == G_READY;
}

void camray_warm(const Room* r)
{
	Grid* g = r && r->vertices ? find(r, true) : NULL;
	if (g)
		grid_step(g, 0);
}

void camray_set(const Room* const* rooms, int n)
{
	s_n = n < MAX_ROOMS ? n : MAX_ROOMS;
	for (int k = 0; k < MAX_ROOMS; k++)
		s_rooms[k] = k < s_n ? rooms[k] : NULL;
	// one room's grid a frame goes on by STEP triangles
	for (int k = 0; k < s_n; k++) {
		const Room* r = s_rooms[k];
		Grid* g = r && r->vertices && r->n_batches ? find(r, true) : NULL;
		if (g && g->state < G_READY) {
			grid_step(g, STEP);
			break;
		}
	}
}

// Both faces of the triangle: t along o + t d where it meets it, or a value over tmax.
static float tri_hit(const float* p0, const float* p1, const float* p2, const float o[3], const float d[3], float tmax)
{
	float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
	float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
	float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
	float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
	if (fabsf(det) < 1e-9f)
		return tmax + 1.0f;
	float inv = 1.0f / det;
	float s[3] = { o[0] - p0[0], o[1] - p0[1], o[2] - p0[2] };
	float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
	if (u < 0.0f || u > 1.0f)
		return tmax + 1.0f;
	float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
	float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
	if (v < 0.0f || u + v > 1.0f)
		return tmax + 1.0f;
	float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
	return t >= 0.0f ? t : tmax + 1.0f;
}

static float tri_ref(const Room* r, u32 ref, const float o[3], const float d[3], float best)
{
	const RoomBatch* b = &r->batches[ref >> 18];
	if (b->hidden)
		return best;                     // a part the stage code has switched off
	const u16* ix = b->indices + (ref & 0x3ffff) * 3;
	camray_stat[2]++;
	float h = tri_hit(vtx(r, ix[0]), vtx(r, ix[1]), vtx(r, ix[2]), o, d, best);
	return h < best ? h : best;
}

// The cells under the segment o + t d, t in [0, best], nearest first (a walk on the ground plane)
static float room_grid(const Room* r, const Grid* g, const float o[3], const float d[3], float best)
{
	float p[2] = { (o[0] - g->x0) / g->cell, (o[2] - g->z0) / g->cell };
	float v[2] = { d[0] / g->cell, d[2] / g->cell };
	const float hi[2] = { (float)g->nx, (float)g->nz };
	float t0 = 0.0f, t1 = best;
	for (int i = 0; i < 2; i++) {        // the part of the segment over the grid
		if (fabsf(v[i]) < 1e-9f) {
			if (p[i] < 0.0f || p[i] >= hi[i])
				return best;
			continue;
		}
		float a = -p[i] / v[i], b = (hi[i] - p[i]) / v[i];
		t0 = fmaxf(t0, fminf(a, b)), t1 = fminf(t1, fmaxf(a, b));
		if (t0 > t1)
			return best;
	}
	int c[2], step[2];
	float next[2], each[2];
	for (int i = 0; i < 2; i++) {
		float s = p[i] + v[i] * t0;
		c[i] = (int)s;
		c[i] = c[i] < 0 ? 0 : c[i] >= (int)hi[i] ? (int)hi[i] - 1 : c[i];
		step[i] = v[i] > 0.0f ? 1 : -1;
		each[i] = fabsf(v[i]) > 1e-9f ? fabsf(1.0f / v[i]) : 1e30f;
		next[i] = fabsf(v[i]) > 1e-9f ? t0 + (v[i] > 0.0f ? c[i] + 1 - s : s - c[i]) * each[i] : 1e30f;
	}
	for (;;) {
		camray_stat[1]++;
		int k = c[1] * g->nx + c[0];
		for (u32 j = g->start[k]; j < g->start[k + 1]; j++)
			best = tri_ref(r, g->refs[j], o, d, best);
		int i = next[0] < next[1] ? 0 : 1;
		if (best <= next[i] || next[i] >= t1)
			return best;                 // a hit before this cell ends, or the segment ends in it
		c[i] += step[i], next[i] += each[i];
		if (c[i] < 0 || c[i] >= (int)hi[i])
			return best;
	}
}

float camray_cast(const float a[3], const float b[3])
{
	float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float best = 1.0f;
	camray_stat[0]++;
	for (int k = 0; k < s_n; k++) {
		const Room* r = s_rooms[k];
		if (!r || !r->vertices || !r->n_batches)
			continue;
		const Grid* g = find(r, false);
		if (g && g->state == G_READY)
			best = room_grid(r, g, a, d, best);
	}
	return best;
}
