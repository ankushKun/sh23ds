#include "collide.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool col_load(Collision* c, const char* path)
{
	memset(c, 0, sizeof *c);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	u32 head[4];
	if (fread(head, 4, 4, f) != 4 || memcmp(head, "S3L1", 4) || head[1] != 1) {
		fclose(f);
		return false;
	}
	c->n_floors = head[2];
	c->n_walls = head[3];
	c->floors = malloc((c->n_floors ? c->n_floors : 1) * sizeof(FloorTri));
	c->walls = malloc((c->n_walls ? c->n_walls : 1) * sizeof(WallSeg));
	bool ok = c->floors && c->walls &&
	          fread(c->floors, sizeof(FloorTri), c->n_floors, f) == c->n_floors &&
	          fread(c->walls, sizeof(WallSeg), c->n_walls, f) == c->n_walls;
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
	if (memcmp(head, "S3L1", 4) || head[1] != 1)
		return false;
	c->n_floors = head[2];
	c->n_walls = head[3];
	size_t nf = c->n_floors * sizeof(FloorTri), nw = c->n_walls * sizeof(WallSeg);
	if (16 + nf + nw > (size_t)size)
		return false;
	c->floors = malloc(nf ? nf : 1);
	c->walls = malloc(nw ? nw : 1);
	if (!c->floors || !c->walls) {
		col_free(c);
		return false;
	}
	memcpy(c->floors, data + 16, nf);
	memcpy(c->walls, data + 16 + nf, nw);
	return true;
}

void col_free(Collision* c)
{
	free(c->floors);
	free(c->walls);
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

static bool wall_overlaps(const WallSeg* w, float y, float h)
{
	// body spans y - h (head) .. y (feet); Y down
	return w->y_top < y - 10.0f && w->y_bottom > y - h;
}

void col_push(const Collision* c, float* x, float* z, float y, float h, float radius)
{
	for (int iter = 0; iter < 3; iter++) {
		bool moved = false;
		for (u32 i = 0; i < c->n_walls; i++) {
			const WallSeg* w = &c->walls[i];
			if (!wall_overlaps(w, y, h))
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
				// exactly on the segment: push along its normal
				float l = sqrtf(len2);
				ox = -dz / l;
				oz = dx / l;
				d = 0;
			} else {
				ox /= d;
				oz /= d;
			}
			*x += ox * (radius - d);
			*z += oz * (radius - d);
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

void col_clamp_ray(const Collision* c, float tx, float tz, float* cx, float* cz, float y)
{
	float best = 1.0f;
	for (u32 i = 0; i < c->n_walls; i++) {
		const WallSeg* w = &c->walls[i];
		if (!(w->y_top < y && w->y_bottom > y))
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
	u32 nf = 0, nw = 0;
	for (int i = 0; i < n; i++)
		nf += parts[i]->n_floors, nw += parts[i]->n_walls;
	FloorTri* floors = malloc((nf ? nf : 1) * sizeof(FloorTri));
	WallSeg* walls = malloc((nw ? nw : 1) * sizeof(WallSeg));
	if (!floors || !walls) {
		free(floors);
		free(walls);
		return false;
	}
	nf = nw = 0;
	for (int i = 0; i < n; i++) {
		memcpy(floors + nf, parts[i]->floors, parts[i]->n_floors * sizeof(FloorTri));
		memcpy(walls + nw, parts[i]->walls, parts[i]->n_walls * sizeof(WallSeg));
		nf += parts[i]->n_floors, nw += parts[i]->n_walls;
	}
	free(out->floors);
	free(out->walls);
	out->floors = floors, out->walls = walls;
	out->n_floors = nf, out->n_walls = nw;
	return true;
}
