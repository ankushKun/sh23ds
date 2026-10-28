#include "collide.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WALL_FILE_SIZE 32     // f32 x0, z0, x1, z1, y_top, y_bottom, u32 group, material

static void wall_from_file(WallSeg* w, const u8* rec, u32 version)
{
	memmove(w, rec, WALL_FILE_SIZE);
	w->one_sided = version >= 2;
}

bool col_read_walls(FILE* f, WallSeg* w, u32 n, u32 version)
{
	// read packed at the start of w, then spread out from the last (each lands at or past its source)
	if (fread(w, WALL_FILE_SIZE, n, f) != n)
		return false;
	for (u32 i = n; i-- > 0;)
		wall_from_file(&w[i], (const u8*)w + i * WALL_FILE_SIZE, version);
	return true;
}

static bool version_ok(const u32* head) { return !memcmp(head, "S3L1", 4) && head[1] >= 1 && head[1] <= S3L_VERSION; }

bool col_load(Collision* c, const char* path)
{
	memset(c, 0, sizeof *c);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	u32 head[4];
	if (fread(head, 4, 4, f) != 4 || !version_ok(head)) {
		fclose(f);
		return false;
	}
	c->n_floors = head[2];
	c->n_walls = head[3];
	c->floors = malloc((c->n_floors ? c->n_floors : 1) * sizeof(FloorTri));
	c->walls = malloc((c->n_walls ? c->n_walls : 1) * sizeof(WallSeg));
	bool ok = c->floors && c->walls &&
	          fread(c->floors, sizeof(FloorTri), c->n_floors, f) == c->n_floors &&
	          col_read_walls(f, c->walls, c->n_walls, head[1]);
	u32 nc;
	if (ok && fread(&nc, 4, 1, f) == 1 && nc && nc < 4096) {   // the columns, when the file has them
		c->cols = malloc(nc * sizeof(ColumnSeg));
		if (c->cols && fread(c->cols, sizeof(ColumnSeg), nc, f) == nc)
			c->n_cols = nc;
	}
	fclose(f);
	if (!ok)
		col_free(c);
	return ok;
}

bool col_load_mem(Collision* c, const u8* data, long size)
{
	memset(c, 0, sizeof *c);
	u32 head[4];
	if (!data || size < 16)
		return false;
	memcpy(head, data, sizeof head);
	if (!version_ok(head))
		return false;
	c->n_floors = head[2];
	c->n_walls = head[3];
	size_t nf = c->n_floors * sizeof(FloorTri), nw = c->n_walls * WALL_FILE_SIZE;
	if (16 + nf + nw > (size_t)size)
		return false;
	c->floors = malloc(nf ? nf : 1);
	c->walls = malloc(c->n_walls ? c->n_walls * sizeof(WallSeg) : 1);
	if (!c->floors || !c->walls) {
		col_free(c);
		return false;
	}
	memcpy(c->floors, data + 16, nf);
	for (u32 i = 0; i < c->n_walls; i++)
		wall_from_file(&c->walls[i], data + 16 + nf + i * WALL_FILE_SIZE, head[1]);
	size_t o = 16 + nf + nw;
	u32 nc;
	if (o + 4 <= (size_t)size) {                 // the columns, when the file has them
		memcpy(&nc, data + o, 4);
		if (nc && nc < 4096 && o + 4 + nc * sizeof(ColumnSeg) <= (size_t)size && (c->cols = malloc(nc * sizeof(ColumnSeg)))) {
			memcpy(c->cols, data + o + 4, nc * sizeof(ColumnSeg));
			c->n_cols = nc;
		}
	}
	return true;
}

void col_free(Collision* c)
{
	free(c->floors);
	free(c->walls);
	free(c->cols);
	memset(c, 0, sizeof *c);
}

// Barycentric test in XZ; returns the interpolated Y.
static bool tri_height(const FloorTri* t, float x, float z, float* y)
{
	float ax = t->v[0], az = t->v[2], bx = t->v[3], bz = t->v[5], cx = t->v[6], cz = t->v[8];
	float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
	if (fabsf(d) < 1e-6f)
		return false;
	float w0 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
	float w1 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
	float w2 = 1.0f - w0 - w1;
	const float e = -1e-4f;
	if (w0 < e || w1 < e || w2 < e)
		return false;
	*y = w0 * t->v[1] + w1 * t->v[4] + w2 * t->v[7];
	return true;
}

bool col_floor(const Collision* c, float x, float z, float y_ref, float max_step, float* y_out)
{
	bool found = false;
	float best = 0;
	for (u32 i = 0; i < c->n_floors; i++) {
		float y;
		if (!tri_height(&c->floors[i], x, z, &y))
			continue;
		float d = fabsf(y - y_ref);
		if (d > max_step)
			continue;
		if (!found || d < fabsf(best - y_ref)) {
			best = y;
			found = true;
		}
	}
	if (found)
		*y_out = best;
	return found;
}

// James' step (the PS2 beta's clCollectCharaHeightNormal, cl_main.c: a ray down from above his feet,
// the first floor it meets): the highest floor from up above y_ref to down below it. Nearest would
// keep him on the ground under a stairwell's first flight, whose foot starts 100 units up (ap22).
bool col_floor_climb(const Collision* c, float x, float z, float y_ref, float up, float down, float* y_out)
{
	bool found = false;
	float best = 0;
	for (u32 i = 0; i < c->n_floors; i++) {
		float y;
		if (!tri_height(&c->floors[i], x, z, &y) || y < y_ref - up || y > y_ref + down)
			continue;
		if (!found || y < best) {     // Y down: the smallest is the highest
			best = y;
			found = true;
		}
	}
	if (found)
		*y_out = best;
	return found;
}

static bool wall_overlaps(const WallSeg* w, float y, float h)
{
	// body spans y - h (head) .. y (feet); Y down
	return w->y_top < y - 10.0f && w->y_bottom > y - h;
}

#define EXTRA_MAX 64     // stage 36 room 0x8b adds up to 40 (stagewalls.c)
static WallSeg s_extra[EXTRA_MAX];
static u32 s_nextra;

void col_extra_clear(void) { s_nextra = 0; }

void col_extra_wall(float x0, float z0, float x1, float z1, float y_top, float y_bottom, bool one_sided)
{
	if (s_nextra < EXTRA_MAX)
		s_extra[s_nextra++] = (WallSeg){ x0, z0, x1, z1, y_top, y_bottom, 1, 0, one_sided };
}

static const WallSeg* wall_at(const Collision* c, u32 i) { return i < c->n_walls ? &c->walls[i] : &s_extra[i - c->n_walls]; }
u32 col_wall_count(const Collision* c) { return c->n_walls + s_nextra; }
const WallSeg* col_wall(const Collision* c, u32 i) { return wall_at(c, i); }

// The characters against the room (the PS2 beta's clCheckBg2Chara, cl_main.c, 0x5258e0 on the PC,
// tests a column against the wall list and the side wall list, its wl and sw, the .cld's lists at
// header 0x164 and 0x16c, through clCheckHitWallCollision 0x524f70, and the stage code's walls
// through clCheckHitDynamicWallCollision 0x525030): the ceiling list's near-vertical polygons
// (group 2, hp105's slabs hanging 636 units over the floor) do not push. Each wall goes through
// clCheckColumn2WallHit (0x522b50), which leaves a wall alone while the centre is behind it.
void col_push(const Collision* c, float* x, float* z, float y, float h, float radius)
{
	for (int iter = 0; iter < 3; iter++) {
		bool moved = false;
		for (u32 i = 0; i < c->n_walls + s_nextra; i++) {
			const WallSeg* w = wall_at(c, i);
			if (w->group == 2 || !wall_overlaps(w, y, h) || !col_wall_faces(w, *x, *z))
				continue;
			float dx = w->x1 - w->x0, dz = w->z1 - w->z0;
			float len2 = dx * dx + dz * dz;
			if (len2 < 1e-3f)
				continue;
			float t = ((*x - w->x0) * dx + (*z - w->z0) * dz) / len2;
			if (t < 0) t = 0;
			if (t > 1) t = 1;
			float px = w->x0 + dx * t, pz = w->z0 + dz * t;
			float ox = *x - px, oz = *z - pz;
			float d2 = ox * ox + oz * oz;
			if (d2 >= radius * radius)
				continue;
			float d = sqrtf(d2);
			if (d < 1e-3f) {
				// exactly on the segment: push along its normal (out of the front of a one-sided wall)
				float l = sqrtf(len2), s = w->one_sided ? -1.0f : 1.0f;
				ox = -dz / l * s;
				oz = dx / l * s;
				d = 0;
			} else {
				ox /= d;
				oz /= d;
			}
			*x += ox * (radius - d);
			*z += oz * (radius - d);
			moved = true;
		}
		// 0x522de0 (clCheckColumn2ColumnHit) with 0x5298a0 (clCheckSubColumnToColumn): a column
		// that overlaps the body in height pushes it out along the line between the centres, to
		// one unit past the two radii
		for (u32 i = 0; i < c->n_cols; i++) {
			const ColumnSeg* k = &c->cols[i];
			if (y < k->y_top || k->y_bottom < y - h)
				continue;
			float dx = *x - k->x, dz = *z - k->z, r = radius + k->radius;
			if (fabsf(dx) > r || fabsf(dz) > r)
				continue;
			float d = sqrtf(dx * dx + dz * dz);
			if (d > r || d < 1e-3f)
				continue;
			float s = (r - d + 1.0f) / d;
			*x += dx * s;
			*z += dz * s;
			moved = true;
		}
		if (!moved)
			break;
	}
}

// Segment intersection in XZ: returns t along p->q, or -1.
static float seg_hit(float px, float pz, float qx, float qz, const WallSeg* w)
{
	float rx = qx - px, rz = qz - pz, sx = w->x1 - w->x0, sz = w->z1 - w->z0;
	float den = rx * sz - rz * sx;
	if (fabsf(den) < 1e-6f)
		return -1;
	float t = ((w->x0 - px) * sz - (w->z0 - pz) * sx) / den;
	float u = ((w->x0 - px) * rz - (w->z0 - pz) * rx) / den;
	return (t >= 0 && t <= 1 && u >= 0 && u <= 1) ? t : -1;
}

// From the front to the back only: col_push never stops a body coming from behind a one-sided wall,
// and once past it, it pushes the body on out of its front.
static bool goes_in(const WallSeg* w, float x0, float z0, float x1, float z1)
{
	return !w->one_sided || (col_wall_side(w, x0, z0) >= 0.0f && col_wall_side(w, x1, z1) < 0.0f);
}

bool col_crosses(const Collision* c, float x0, float z0, float x1, float z1, float y, float h)
{
	for (u32 i = 0; i < c->n_walls + s_nextra; i++) {
		const WallSeg* w = wall_at(c, i);
		if (w->group != 2 && wall_overlaps(w, y, h) && goes_in(w, x0, z0, x1, z1) && seg_hit(x0, z0, x1, z1, w) >= 0.0f)
			return true;
	}
	return false;
}

float col_clearance(const Collision* c, float x, float z, float y, float h, float radius)
{
	float best = radius;
	for (u32 i = 0; i < c->n_walls + s_nextra; i++) {
		const WallSeg* w = wall_at(c, i);
		if (w->group == 2 || !wall_overlaps(w, y, h) || !col_wall_faces(w, x, z))
			continue;
		float dx = w->x1 - w->x0, dz = w->z1 - w->z0, len2 = dx * dx + dz * dz;
		if (len2 < 1e-3f)
			continue;
		float t = ((x - w->x0) * dx + (z - w->z0) * dz) / len2;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		float d = hypotf(x - w->x0 - dx * t, z - w->z0 - dz * t);
		if (d < best)
			best = d;
	}
	for (u32 i = 0; i < c->n_cols; i++) {
		const ColumnSeg* k = &c->cols[i];
		if (y < k->y_top || k->y_bottom < y - h)
			continue;
		float d = hypotf(x - k->x, z - k->z) - k->radius;
		if (d < best)
			best = d;
	}
	return best;
}

// The PC's line tests (0x528840, 0x528350, used by the eye, weapon and camera checks) meet a wall
// only from its front.
void col_clamp_ray(const Collision* c, float tx, float tz, float* cx, float* cz, float y)
{
	float best = 1.0f;
	for (u32 i = 0; i < c->n_walls + s_nextra; i++) {
		const WallSeg* w = wall_at(c, i);
		if (!(w->y_top < y && w->y_bottom > y) || !goes_in(w, tx, tz, *cx, *cz))
			continue;
		float t = seg_hit(tx, tz, *cx, *cz, w);
		if (t >= 0 && t < best)
			best = t;
	}
	if (best < 1.0f) {
		best = best > 0.08f ? best - 0.08f : 0.0f;   // stop a little before the wall
		*cx = tx + (*cx - tx) * best;
		*cz = tz + (*cz - tz) * best;
	}
}

bool col_spawn(const Collision* c, float x, float z, float* sx, float* sy, float* sz)
{
	float best = 1e30f;
	bool found = false;
	for (u32 i = 0; i < c->n_floors; i++) {
		const float* v = c->floors[i].v;
		float mx = (v[0] + v[3] + v[6]) / 3, my = (v[1] + v[4] + v[7]) / 3, mz = (v[2] + v[5] + v[8]) / 3;
		float d = (mx - x) * (mx - x) + (mz - z) * (mz - z);
		if (d < best) {
			best = d;
			*sx = mx;
			*sy = my;
			*sz = mz;
			found = true;
		}
	}
	return found;
}

bool col_merge(Collision* out, const Collision* const* parts, int n)
{
	u32 nf = 0, nw = 0, nc = 0;
	for (int i = 0; i < n; i++)
		nf += parts[i]->n_floors, nw += parts[i]->n_walls, nc += parts[i]->n_cols;
	FloorTri* floors = malloc((nf ? nf : 1) * sizeof(FloorTri));
	WallSeg* walls = malloc((nw ? nw : 1) * sizeof(WallSeg));
	ColumnSeg* cols = nc ? malloc(nc * sizeof(ColumnSeg)) : NULL;
	if (!floors || !walls || (nc && !cols)) {
		free(floors);
		free(walls);
		free(cols);
		return false;
	}
	nf = nw = nc = 0;
	for (int i = 0; i < n; i++) {
		memcpy(floors + nf, parts[i]->floors, parts[i]->n_floors * sizeof(FloorTri));
		memcpy(walls + nw, parts[i]->walls, parts[i]->n_walls * sizeof(WallSeg));
		if (parts[i]->n_cols)
			memcpy(cols + nc, parts[i]->cols, parts[i]->n_cols * sizeof(ColumnSeg));
		nf += parts[i]->n_floors, nw += parts[i]->n_walls, nc += parts[i]->n_cols;
	}
	free(out->floors);
	free(out->walls);
	free(out->cols);
	out->floors = floors, out->walls = walls, out->cols = cols;
	out->n_floors = nf, out->n_walls = nw, out->n_cols = nc;
	return true;
}
