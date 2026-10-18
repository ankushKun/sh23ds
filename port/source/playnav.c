// Path planning for the playtest driver (playtest.c): a grid over the loaded collision (the merged
// set of the room and its streamed neighbours), with the floor heights rasterised from the floor
// triangles (two layers for rooms over rooms), cells near walls blocked as James' body (radius 130,
// 900 tall) would be pushed out, and an A* search with the step the player code allows. The walk
// itself goes through the game's own movement and collision; this only picks the way.
#include "playnav.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Circles the coarse grid leaves out (legs the fine check could not walk), for the next try.
static float s_blk[16][3];
static int s_nblk;
static bool s_use_blk;

static float CELL = 100.0f;
static int MAXN = 300;          // cells per side
static int LAYERS = 6;          // floors over floors (the stairwells stack three, hp162 five)
static float STEP = 140.0f;     // floor height change between neighbouring cells
static float BODY_R = 120.0f;
static float MARGIN = 2500.0f;
#define NOFLOOR 1e9f

typedef struct {
	int nx, nz;
	float x0, z0;
	float* h;                  // [LAYERS][nz][nx]
	u8* blocked;               // [LAYERS][nz][nx]: 1 the cell is too close to a wall, 2 / 4 a wall
	                           // crosses the way to the next cell east / south (the coarse grid)
} Grid;
static bool s_edges;           // block the ways between cells (coarse grid) rather than the cells

static int idx(const Grid* g, int l, int i, int j) { return (l * g->nz + j) * g->nx + i; }

static void put_height(Grid* g, int i, int j, float y)
{
	for (int l = 0; l < LAYERS; l++) {
		float* h = &g->h[idx(g, l, i, j)];
		if (*h == NOFLOOR) { *h = y; return; }
		if (fabsf(*h - y) < 100.0f) { if (y < *h) *h = y; return; }   // the same floor (keep the top; no more than a step apart, so merging makes no step: cc28's terraces)
	}
}

static bool inside_tri(const FloorTri* t, float x, float z, float* y)
{
	float ax = t->v[0], az = t->v[2], bx = t->v[3], bz = t->v[5], cx = t->v[6], cz = t->v[8];
	float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
	if (fabsf(d) < 1e-6f)
		return false;
	float w0 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
	float w1 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
	float w2 = 1.0f - w0 - w1;
	if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
		return false;
	*y = w0 * t->v[1] + w1 * t->v[4] + w2 * t->v[7];
	return true;
}

static float WAY_CLEAR = 110.0f;   // the coarse grid's ways keep this far from walls

static float pt_seg(float px, float pz, float ax, float az, float bx, float bz)
{
	float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
	float t = l2 > 1e-6f ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0f;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return hypotf(px - ax - dx * t, pz - az - dz * t);
}

// Distance between the segment p-q and the wall in the ground plane (0 when they cross).
static float seg_seg_dist(float px, float pz, float qx, float qz, const WallSeg* w)
{
	float rx = qx - px, rz = qz - pz, sx = w->x1 - w->x0, sz = w->z1 - w->z0;
	float den = rx * sz - rz * sx;
	if (fabsf(den) > 1e-6f) {
		float t = ((w->x0 - px) * sz - (w->z0 - pz) * sx) / den;
		float u = ((w->x0 - px) * rz - (w->z0 - pz) * rx) / den;
		if (t >= 0 && t <= 1 && u >= 0 && u <= 1)
			return 0.0f;
	}
	float d = pt_seg(px, pz, w->x0, w->z0, w->x1, w->z1);
	d = fminf(d, pt_seg(qx, qz, w->x0, w->z0, w->x1, w->z1));
	d = fminf(d, pt_seg(w->x0, w->z0, px, pz, qx, qz));
	return fminf(d, pt_seg(w->x1, w->z1, px, pz, qx, qz));
}

static void build(Grid* g, const Collision* c)
{
	int n = LAYERS * g->nx * g->nz;
	for (int k = 0; k < n; k++)
		g->h[k] = NOFLOOR;
	memset(g->blocked, 0, n);
	for (u32 k = 0; k < c->n_floors; k++) {
		const FloorTri* t = &c->floors[k];
		float mnx = fminf(t->v[0], fminf(t->v[3], t->v[6])), mxx = fmaxf(t->v[0], fmaxf(t->v[3], t->v[6]));
		float mnz = fminf(t->v[2], fminf(t->v[5], t->v[8])), mxz = fmaxf(t->v[2], fmaxf(t->v[5], t->v[8]));
		int i0 = (int)ceilf((mnx - g->x0) / CELL - 0.5f), i1 = (int)floorf((mxx - g->x0) / CELL - 0.5f);
		int j0 = (int)ceilf((mnz - g->z0) / CELL - 0.5f), j1 = (int)floorf((mxz - g->z0) / CELL - 0.5f);
		if (i0 < 0) i0 = 0;
		if (j0 < 0) j0 = 0;
		if (i1 >= g->nx) i1 = g->nx - 1;
		if (j1 >= g->nz) j1 = g->nz - 1;
		for (int j = j0; j <= j1; j++)
			for (int i = i0; i <= i1; i++) {
				float y;
				if (inside_tri(t, g->x0 + (i + 0.5f) * CELL, g->z0 + (j + 0.5f) * CELL, &y))
					put_height(g, i, j, y);
			}
	}
	// the circles the area route leaves out after a failed leg
	if (s_use_blk)
		for (int q = 0; q < s_nblk; q++) {
			int i0 = (int)((s_blk[q][0] - s_blk[q][2] - g->x0) / CELL), i1 = (int)((s_blk[q][0] + s_blk[q][2] - g->x0) / CELL);
			int j0 = (int)((s_blk[q][1] - s_blk[q][2] - g->z0) / CELL), j1 = (int)((s_blk[q][1] + s_blk[q][2] - g->z0) / CELL);
			for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < g->nz; j++)
				for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < g->nx; i++)
					if (hypotf(g->x0 + (i + 0.5f) * CELL - s_blk[q][0], g->z0 + (j + 0.5f) * CELL - s_blk[q][1]) < s_blk[q][2])
						for (int l = 0; l < LAYERS; l++)
							g->blocked[idx(g, l, i, j)] |= 1;
		}
	// walls: cells whose centre is within the body's radius of a wall at the cell's height
	u32 nw = col_wall_count(c);
	for (u32 k = 0; k < nw; k++) {
		const WallSeg* w = col_wall(c, k);
		float dx = w->x1 - w->x0, dz = w->z1 - w->z0, len2 = dx * dx + dz * dz;
		if (len2 < 1e-3f || w->group == 2)
			continue;                // the ceiling list does not stop James (col_push)
		float pad = s_edges ? CELL * 1.5f : BODY_R;
		float mnx = fminf(w->x0, w->x1) - pad, mxx = fmaxf(w->x0, w->x1) + pad;
		float mnz = fminf(w->z0, w->z1) - pad, mxz = fmaxf(w->z0, w->z1) + pad;
		int i0 = (int)floorf((mnx - g->x0) / CELL), i1 = (int)floorf((mxx - g->x0) / CELL);
		int j0 = (int)floorf((mnz - g->z0) / CELL), j1 = (int)floorf((mxz - g->z0) / CELL);
		if (i0 < 0) i0 = 0;
		if (j0 < 0) j0 = 0;
		if (i1 >= g->nx) i1 = g->nx - 1;
		if (j1 >= g->nz) j1 = g->nz - 1;
		for (int j = j0; j <= j1; j++)
			for (int i = i0; i <= i1; i++) {
				float px = g->x0 + (i + 0.5f) * CELL, pz = g->z0 + (j + 0.5f) * CELL;
				float t = ((px - w->x0) * dx + (pz - w->z0) * dz) / len2;
				t = t < 0 ? 0 : t > 1 ? 1 : t;
				float ox = px - (w->x0 + dx * t), oz = pz - (w->z0 + dz * t);
				if (ox * ox + oz * oz >= (s_edges ? 2.0f * CELL * CELL : BODY_R * BODY_R))
					continue;
				for (int l = 0; l < LAYERS; l++) {
					float y = g->h[idx(g, l, i, j)];
					// the player code's test: the wall spans part of the body (feet y, 900 tall)
					if (y == NOFLOOR || !(w->y_top < y - 10.0f && w->y_bottom > y - 900.0f))
						continue;
					if (!s_edges) {
						g->blocked[idx(g, l, i, j)] = 1;
						continue;
					}
					// the ways east and south from this cell's centre: does the wall cross them or come
					// closer than James' width allows?
					for (int dir = 0; dir < 2; dir++) {
						float qx = px + (dir == 0 ? CELL : 0.0f), qz = pz + (dir == 1 ? CELL : 0.0f);
						if (seg_seg_dist(px, pz, qx, qz, w) < WAY_CLEAR)
							g->blocked[idx(g, l, i, j)] |= (u8)(2 << dir);
					}
				}
			}
	}
}

// A wall at the body's height (feet y, 900 tall) across the segment a-b.
static bool wall_between(const Collision* c, float ax, float az, float bx, float bz, float y)
{
	for (u32 k = 0, n = col_wall_count(c); k < n; k++) {
		const WallSeg* w = col_wall(c, k);
		if (w->group == 2 || !(w->y_top < y - 10.0f && w->y_bottom > y - 900.0f))
			continue;
		float rx = bx - ax, rz = bz - az, sx = w->x1 - w->x0, sz = w->z1 - w->z0, den = rx * sz - rz * sx;
		if (fabsf(den) < 1e-6f)
			continue;
		float t = ((w->x0 - ax) * sz - (w->z0 - az) * sx) / den, u = ((w->x0 - ax) * rz - (w->z0 - az) * rx) / den;
		if (t >= 0 && t <= 1 && u >= 0 && u <= 1)
			return true;
	}
	return false;
}

static int layer_near(const Grid* g, int i, int j, float y)
{
	int best = -1;
	float bd = 400.0f;
	for (int l = 0; l < LAYERS; l++) {
		float h = g->h[idx(g, l, i, j)];
		if (h != NOFLOOR && fabsf(h - y) < bd) {
			bd = fabsf(h - y);
			best = l;
		}
	}
	return best;
}

// The floor James steps onto there (col_floor_climb): the highest within a step of y, else -1.
static int layer_climb(const Grid* g, int i, int j, float y)
{
	int best = -1;
	for (int l = 0; l < LAYERS; l++) {
		float h = g->h[idx(g, l, i, j)];
		if (h != NOFLOOR && fabsf(h - y) <= STEP && (best < 0 || h < g->h[idx(g, best, i, j)]))
			best = l;
	}
	return best;
}

static bool free_cell(const Grid* g, int k) { return g->h[k] != NOFLOOR && !(g->blocked[k] & 1); }

// A wall across the way between neighbouring cells (coarse grid): from (i, j, layer k) by (di, dj).
static bool way_walled(const Grid* g, int l, int i, int j, int di, int dj)
{
	if (!s_edges)
		return false;
	if (di == 1) return g->blocked[idx(g, l, i, j)] & 2;
	if (di == -1) return g->blocked[idx(g, l, i - 1, j)] & 2;
	if (dj == 1) return g->blocked[idx(g, l, i, j)] & 4;
	return g->blocked[idx(g, l, i, j - 1)] & 4;
}

// Straight walk from cell a to b on layer heights: every cell on the line is free, the steps small,
// and it ends on b's own floor (a straight line under a stairwell's landing stays on the ground).
static bool line_ok(const Grid* g, int ia, int ja, int la, int ib, int jb, int lb)
{
	int n = abs(ib - ia) > abs(jb - ja) ? abs(ib - ia) : abs(jb - ja);
	float y = g->h[idx(g, la, ia, ja)];
	int pi = ia, pj = ja;
	for (int s = 1; s <= n * 4; s++) {
		float f = (float)s / (n * 4);
		int i = (int)lroundf(ia + (ib - ia) * f), j = (int)lroundf(ja + (jb - ja) * f);
		int l = layer_climb(g, i, j, y);
		if (l < 0)
			return false;
		int k = idx(g, l, i, j);
		if (!free_cell(g, k) || fabsf(g->h[k] - y) > STEP)
			return false;
		if (s_edges && (i != pi || j != pj)) {
			int pl = layer_near(g, pi, pj, y);
			if (pl < 0 || (i != pi && way_walled(g, pl, pi, pj, i - pi, 0)) || (j != pj && way_walled(g, pl, i, pj, 0, j - pj)))
				return false;
		}
		pi = i, pj = j;
		y = g->h[k];
	}
	return layer_near(g, ib, jb, y) == lb;
}

static int plan(const Collision* c, const float start[3], const float goal[3], float* out, int max, bool* partial, float* ys)
{
	*partial = false;
	if (!c || !c->n_floors)
		return 0;
	float mx = fminf(start[0], goal[0]) - MARGIN, Mx = fmaxf(start[0], goal[0]) + MARGIN;
	float mz = fminf(start[2], goal[2]) - MARGIN, Mz = fmaxf(start[2], goal[2]) + MARGIN;
	Grid g;
	g.nx = (int)((Mx - mx) / CELL) + 1, g.nz = (int)((Mz - mz) / CELL) + 1;
	g.x0 = mx, g.z0 = mz;
	// too far for the grid: plan toward the goal across the part around James
	if (g.nx > MAXN) {
		float d = goal[0] > start[0] ? 1.0f : -1.0f;
		g.nx = MAXN;
		g.x0 = d > 0 ? start[0] - MARGIN : start[0] + MARGIN - MAXN * CELL;
	}
	if (g.nz > MAXN) {
		float d = goal[2] > start[2] ? 1.0f : -1.0f;
		g.nz = MAXN;
		g.z0 = d > 0 ? start[2] - MARGIN : start[2] + MARGIN - MAXN * CELL;
	}
	if (LAYERS > 2 && LAYERS * g.nx * g.nz > 400000)
		LAYERS = 400000 / (g.nx * g.nz) < 2 ? 2 : 400000 / (g.nx * g.nz);   // memory: about 7 MB at most
	int n = LAYERS * g.nx * g.nz;
	g.h = malloc(n * sizeof(float));
	g.blocked = malloc(n);
	int* parent = malloc(n * sizeof(int));
	float* cost = malloc(n * sizeof(float));
	int* heap = malloc(n * sizeof(int));
	int count = 0;
	if (!g.h || !g.blocked || !parent || !cost || !heap)
		goto done;
	build(&g, c);
	int si = (int)((start[0] - g.x0) / CELL), sj = (int)((start[2] - g.z0) / CELL);
	if (si < 0 || sj < 0 || si >= g.nx || sj >= g.nz)
		goto done;
	int sl = layer_near(&g, si, sj, start[1]);
	for (int r = 1; sl < 0 && r <= 2; r++)       // big cells may miss James' floor: a neighbour's
		for (int dj = -r; dj <= r && sl < 0; dj++)
			for (int di = -r; di <= r && sl < 0; di++)
				if (si + di >= 0 && sj + dj >= 0 && si + di < g.nx && sj + dj < g.nz &&
				    (sl = layer_near(&g, si + di, sj + dj, start[1])) >= 0)
					si += di, sj += dj;
	if (sl < 0)
		goto done;
	int sk = idx(&g, sl, si, sj);
	g.blocked[sk] = 0;          // James stands there, whatever the walls say
	for (int dj = -2; dj <= 2; dj++)     // and close round him (a cutscene may leave him by a wall)
		for (int di = -2; di <= 2; di++) {
			int i2 = si + di, j2 = sj + dj;
			if (i2 < 0 || j2 < 0 || i2 >= g.nx || j2 >= g.nz || hypotf(di * CELL, dj * CELL) > 200.0f)
				continue;
			int l2 = layer_near(&g, i2, j2, start[1]);
			if (l2 >= 0 && !wall_between(c, start[0], start[2], g.x0 + (i2 + 0.5f) * CELL, g.z0 + (j2 + 0.5f) * CELL, start[1]))
				g.blocked[idx(&g, l2, i2, j2)] &= (u8)~1;
		}
	int gi = (int)((goal[0] - g.x0) / CELL), gj = (int)((goal[2] - g.z0) / CELL);
	bool goal_in = gi >= 0 && gj >= 0 && gi < g.nx && gj < g.nz;
	// a goal height off every floor there (an event that ignores height): any floor of the cell
	bool goal_any = false;
	if (goal_in) {
		bool near = false, some = false;
		for (int l = 0; l < LAYERS; l++) {
			float h = g.h[idx(&g, l, gi, gj)];
			if (h != NOFLOOR) { some = true; if (fabsf(h - goal[1]) < 600.0f) near = true; }
		}
		goal_any = some && !near;
	}
	for (int k = 0; k < n; k++)
		parent[k] = -2, cost[k] = 1e30f;
	// A* (binary heap on cost + distance to the goal)
	int hn = 0;
#define F(k) (cost[k] + hypotf((float)((k) % g.nx) - gi, (float)(((k) / g.nx) % g.nz) - gj) * 1.0f)
#define PUSH(k) do { int _c = hn++; heap[_c] = (k); while (_c > 0) { int _p = (_c - 1) / 2; \
		if (F(heap[_p]) <= F(heap[_c])) { break; } int _t = heap[_p]; heap[_p] = heap[_c]; heap[_c] = _t; _c = _p; } } while (0)
	cost[sk] = 0.0f;
	parent[sk] = -1;
	PUSH(sk);
	int best = sk;
	float bestd = 1e30f;
	int reached = -1;
	while (hn > 0) {
		int k = heap[0];
		heap[0] = heap[--hn];
		for (int c2 = 0;;) {
			int l2 = 2 * c2 + 1, r2 = l2 + 1, m = c2;
			if (l2 < hn && F(heap[l2]) < F(heap[m])) m = l2;
			if (r2 < hn && F(heap[r2]) < F(heap[m])) m = r2;
			if (m == c2) break;
			int t = heap[m]; heap[m] = heap[c2]; heap[c2] = t; c2 = m;
		}
		int i = k % g.nx, j = (k / g.nx) % g.nz;
		float d = hypotf(g.x0 + (i + 0.5f) * CELL - goal[0], g.z0 + (j + 0.5f) * CELL - goal[2]);
		if (d < bestd) { bestd = d; best = k; }
		if (d < CELL * 1.5f && (goal_any || fabsf(g.h[k] - goal[1]) < 600.0f)) { reached = k; break; }
		float y = g.h[k];
		for (int dj = -1; dj <= 1; dj++)
			for (int di = -1; di <= 1; di++) {
				if (!di && !dj) continue;
				int ni = i + di, nj = j + dj;
				if (ni < 0 || nj < 0 || ni >= g.nx || nj >= g.nz) continue;
				int nl = layer_climb(&g, ni, nj, y);   // (cc28's ramps under a flat floor)
				if (nl < 0) continue;
				int nk = idx(&g, nl, ni, nj);
				if (!free_cell(&g, nk) || fabsf(g.h[nk] - y) > STEP) continue;
				int kl = k / (g.nx * g.nz);
				if (!di || !dj) {
					if (way_walled(&g, kl, i, j, di, dj)) continue;
				} else {                // no corner cutting
					int a = layer_near(&g, i + di, j, y), b = layer_near(&g, i, j + dj, y);
					if (a < 0 || b < 0 || !free_cell(&g, idx(&g, a, i + di, j)) || !free_cell(&g, idx(&g, b, i, j + dj)))
						continue;
					if (way_walled(&g, kl, i, j, di, 0) || way_walled(&g, a, i + di, j, 0, dj) ||
					    way_walled(&g, kl, i, j, 0, dj) || way_walled(&g, b, i, j + dj, di, 0))
						continue;
				}
				float nc = cost[k] + (di && dj ? 1.4142f : 1.0f);
				if (nc < cost[nk]) {
					cost[nk] = nc;
					parent[nk] = k;
					PUSH(nk);
				}
			}
	}
#undef PUSH
#undef F
	if (reached < 0) {
		*partial = true;
		reached = best;
#ifdef NAV_HOST
		printf("  partial: cell %.0f, nearest (%.0f %.0f %.0f)\n", CELL, g.x0 + (best % g.nx + 0.5f) * CELL, g.h[best],
		       g.z0 + ((best / g.nx) % g.nz + 0.5f) * CELL);
#endif
		if (!goal_in && reached == sk)
			goto done;
	}
	// the cells back to the start, then keep only the corners a straight walk can join
	static int chain[4096];
	int nc = 0;
	for (int k = reached; k >= 0 && nc < 4096; k = parent[k])
		chain[nc++] = k;
	int cur = nc - 1;
	while (cur > 0 && count < max) {
		int k0 = chain[cur], i0 = k0 % g.nx, j0 = (k0 / g.nx) % g.nz, l0 = k0 / (g.nx * g.nz);
		int nxt = cur - 1;
		for (int t = 0; t < cur; t++) {     // the farthest cell still in a straight line
			int k1 = chain[t];
			if (line_ok(&g, i0, j0, l0, k1 % g.nx, (k1 / g.nx) % g.nz, k1 / (g.nx * g.nz))) {
				nxt = t;
				break;
			}
		}
		int k1 = chain[nxt];
		out[count * 2] = g.x0 + (k1 % g.nx + 0.5f) * CELL;
		out[count * 2 + 1] = g.z0 + ((k1 / g.nx) % g.nz + 0.5f) * CELL;
		if (ys)
			ys[count] = g.h[k1];
		count++;
		cur = nxt;
	}
	if (!*partial && count < max) {     // the exact goal at the end
		out[count * 2] = goal[0];
		out[count * 2 + 1] = goal[2];
		if (ys)
			ys[count] = goal[1];
		count++;
	}
done:
	free(g.h);
	free(g.blocked);
	free(parent);
	free(cost);
	free(heap);
	return count;
}

static float s_last_ys[256];

int nav_plan(const Collision* c, const float start[3], const float goal[3], float* out, int max, bool* partial)
{
	CELL = 100.0f, MAXN = 300, BODY_R = 145.0f, s_edges = false, LAYERS = 6, STEP = 140.0f;   // (hp162 stacks five floors)
	MARGIN = fabsf(goal[1] - start[1]) > 500.0f ? 6000.0f : 3000.0f;   // stairs wind round
	int n = plan(c, start, goal, out, max, partial, s_last_ys);
	if (*partial) {
		// narrow ways (ap38's passage to its door, about 450 wide) leave no cell a full 145 from both
		// walls: again with less room to spare (James is 130; the walls slide him along)
		static float o2[2 * 128], y2[256];
		bool p2;
		// and a wider box for ways that wind far round (the labyrinth's ps125: down and back up)
		for (int k = 0; k < 3 && *partial; k++) {
			BODY_R = k == 1 ? 145.0f : 110.0f;
			MARGIN = k == 0 ? MARGIN : 9000.0f;
			LAYERS = 6;
			int n2 = plan(c, start, goal, o2, max < 128 ? max : 128, &p2, y2);
			if (n2 > 0 && !p2) {
				memcpy(out, o2, n2 * 2 * sizeof(float));
				memcpy(s_last_ys, y2, n2 * sizeof(float));
				*partial = false;
				n = n2;
			}
		}
	}
	return n;
}

const float* nav_last_heights(void) { return s_last_ys; }

// ---------------------------------------------------------------- area routes (the open areas)
//
// The town, the forest and the lake are many tiles; only those within reach of James are loaded,
// so a long walk first finds its way over every tile of the area (pack bg/<area>/*.s3l) on a
// coarser grid (at most 450 cells a side over the box of the walk), and then goes from one of its
// waypoints to the next with nav_plan on the loaded tiles.
#include <dirent.h>
#include <stdio.h>

static Collision s_ac;
static char s_area[8];
static float s_area_ext[4];      // the area's floors: min x, min z, max x, max z

static void load_s3l_append(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return;
	u32 h[4];
	if (fread(h, sizeof h, 1, f) == 1 && !memcmp(h, "S3L1", 4)) {
		FloorTri* nf = realloc(s_ac.floors, (s_ac.n_floors + h[2] + 1) * sizeof(FloorTri));
		WallSeg* nw = realloc(s_ac.walls, (s_ac.n_walls + h[3] + 1) * sizeof(WallSeg));
		if (nf) s_ac.floors = nf;
		if (nw) s_ac.walls = nw;
		if (nf && nw && fread(s_ac.floors + s_ac.n_floors, sizeof(FloorTri), h[2], f) == h[2] &&
		    fread(s_ac.walls + s_ac.n_walls, sizeof(WallSeg), h[3], f) == h[3]) {
			s_ac.n_floors += h[2];
			s_ac.n_walls += h[3];
		}
	}
	fclose(f);
}

static bool load_area(const char* area)
{
	if (!strcmp(area, s_area) && s_ac.n_floors)
		return true;
	s_ac.n_floors = s_ac.n_walls = 0;
	snprintf(s_area, sizeof s_area, "%s", area);
	// "ca,cb": two folders' tiles together (a walk from the Wood Side gate into town east)
	for (const char* f = area; *f;) {
		size_t fl = strcspn(f, ",");
		char dir[64];
		snprintf(dir, sizeof dir, "sdmc:/3ds/sh23ds/bg/%.*s", (int)fl, f);
		DIR* d = opendir(dir);
		if (d) {
			struct dirent* de;
			while ((de = readdir(d)) != NULL) {
				size_t n = strlen(de->d_name);
				if (n > 4 && !strcmp(de->d_name + n - 4, ".s3l")) {
					char path[128];
					snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
					load_s3l_append(path);
				}
			}
			closedir(d);
		}
		f += fl;
		if (*f == ',')
			f++;
	}
	s_area_ext[0] = s_area_ext[1] = 1e30f, s_area_ext[2] = s_area_ext[3] = -1e30f;
	for (u32 k = 0; k < s_ac.n_floors; k++)
		for (int v = 0; v < 3; v++) {
			const float* p = s_ac.floors[k].v + 3 * v;
			s_area_ext[0] = fminf(s_area_ext[0], p[0]), s_area_ext[1] = fminf(s_area_ext[1], p[2]);
			s_area_ext[2] = fmaxf(s_area_ext[2], p[0]), s_area_ext[3] = fmaxf(s_area_ext[3], p[2]);
		}
	return s_ac.n_floors > 0;
}


// The coarse plan over the area's tiles: about 200000 cells over the box of the walk with a wide
// margin (roads wander), then a box twice as wide on 300000 cells (town west's lower streets join its
// upper ones only by cc11's slope, far round to the south). Its ways are shut only by a wall across
// them, so narrow streets are not lost to the size of the cells; the legs are checked after.
static int coarse_plan(const float start[3], const float goal[3], float* xz, float* ys, int max)
{
	bool partial = true;
	int n = 0;
	// (a third pass takes in all of the area: from Heaven's Night to the Baldwin mansion the way runs
	// 70000 south of both)
	for (int pass = 0; pass < 3 && (n <= 0 || partial); pass++) {
		float span = fmaxf(fabsf(goal[0] - start[0]), fabsf(goal[2] - start[2]));
		WAY_CLEAR = 1.0f;            // only a wall across the way (the legs are checked after)
		float mx = fmaxf(fminf(start[0], goal[0]) - s_area_ext[0], s_area_ext[2] - fmaxf(start[0], goal[0]));
		float mz = fmaxf(fminf(start[2], goal[2]) - s_area_ext[1], s_area_ext[3] - fmaxf(start[2], goal[2]));
		MARGIN = pass == 2 ? fmaxf(mx, mz) + 1000.0f : pass == 1 ? fmaxf(20000.0f, 0.7f * span) : fmaxf(6000.0f, 0.3f * span);
		float sx = fabsf(goal[0] - start[0]) + 2 * MARGIN + 100.0f, sz = fabsf(goal[2] - start[2]) + 2 * MARGIN + 100.0f;
		CELL = fmaxf(100.0f, sqrtf(sx * sz / (pass ? 300000.0f : 200000.0f)));
		MAXN = (int)(fmaxf(sx, sz) / CELL) + 4;
		BODY_R = 0.0f;
		LAYERS = 2;
		STEP = fmaxf(140.0f, 1.2f * CELL);   // a ramp or a flight of steps between cells this far apart
		s_edges = true;              // the coarse grid only looks for the way; the walk keeps clear
		s_use_blk = true;
		n = plan(&s_ac, start, goal, xz, max, &partial, ys);
		s_use_blk = false;
	}
	return n > 0 && !partial ? n : 0;
}

// A leg of the coarse route planned as the walk will plan it (cells of 100, James' body kept 110
// from the walls, the narrow-way try of nav_plan).
static bool leg_ok(const float a[3], const float b[3])
{
	static float xz[2 * 64], ys[64];
	bool partial;
	CELL = 100.0f, MAXN = 120, MARGIN = 2500.0f, BODY_R = 110.0f, LAYERS = 4, STEP = 140.0f;
	s_edges = false;
	// (the points' heights are the coarse cells' in between: on the floor nearest them)
	float a2[3] = { a[0], a[1], a[2] }, b2[3] = { b[0], b[1], b[2] };
	col_floor(&s_ac, a2[0], a2[2], a[1], 3000.0f, &a2[1]);
	col_floor(&s_ac, b2[0], b2[2], b[1], 3000.0f, &b2[1]);
	int n = plan(&s_ac, a2, b2, xz, 64, &partial, ys);
	return n > 0 && !partial;
}

// The route is planned whole (up to 256 points) and handed out as far as max allows, the goal last:
// the door search asks only whether a way exists, with room for 8.
static int area_route(const float start[3], const float goal[3], float* out, int max);

int nav_area_route(const char* area, const float start[3], const float goal[3], float* out, int max)
{
	static float full[3 * 256];
	if (max < 1 || !load_area(area))
		return 0;
	int n = area_route(start, goal, full, 256);
	if (n <= 0)
		return 0;
	if (n > max)
		n = max;
	memcpy(out, full, 3 * (n - 1) * sizeof(float));
	memcpy(out + 3 * (n - 1), goal, 3 * sizeof(float));
	return n;
}

static int area_route(const float start[3], const float goal[3], float* out, int max)
{
	static float xz[2 * 256], ys[256];
	s_nblk = 0;
	for (int attempt = 0; attempt < 10; attempt++) {
		int n = coarse_plan(start, goal, xz, ys, 256);
		if (n <= 0)
			return 0;
		// waypoints about 2000 apart along the way (long straight stretches cut up, so every leg stays
		// within the tiles loaded around James), the goal always last
		int count = 0;
		float lx = start[0], ly = start[1], lz = start[2];
		for (int i = 0; i < n && count < max - 1; i++) {
			float dx = xz[2 * i] - lx, dz = xz[2 * i + 1] - lz, d = sqrtf(dx * dx + dz * dz);
			int parts = (int)(d / 2000.0f);
			for (int k = 1; k <= parts && count < max - 1; k++) {
				float f = (float)k / (parts + 1);
				out[count * 3] = lx + dx * f, out[count * 3 + 1] = ly + (ys[i] - ly) * f, out[count * 3 + 2] = lz + dz * f;
				count++;
			}
			if (count >= max - 1)
				break;
			out[count * 3] = xz[2 * i], out[count * 3 + 1] = ys[i], out[count * 3 + 2] = xz[2 * i + 1];
			lx = xz[2 * i], ly = ys[i], lz = xz[2 * i + 1];
			count++;
		}
		out[(count - 1) * 3] = goal[0], out[(count - 1) * 3 + 1] = goal[1], out[(count - 1) * 3 + 2] = goal[2];
		// every leg on the fine grid: where one cannot be walked, the coarse grid leaves that place
		// out and plans again (the sloping street west of Rosewater Park, the way from Heaven's Night)
		int bad = -1;
		float prev[3] = { start[0], start[1], start[2] };
		for (int k = 0; k < count && bad < 0; k++) {
			if (!leg_ok(prev, out + 3 * k)) {
				// a big cell's centre may sit in a wall or off the floor: straight on to the next point
				if (k + 1 < count && leg_ok(prev, out + 3 * (k + 1))) {
					memmove(out + 3 * k, out + 3 * (k + 1), (count - k - 1) * 3 * sizeof(float));
					count--;
				} else {
					bad = k;
					break;
				}
			}
			memcpy(prev, out + 3 * k, sizeof prev);
		}
		if (bad < 0)
			return count;
		if (s_nblk >= 16)
			return 0;
		const float* a = bad ? out + 3 * (bad - 1) : start;   // (the leg that failed)
		const float* b = out + 3 * bad;
		s_blk[s_nblk][0] = (a[0] + b[0]) * 0.5f, s_blk[s_nblk][1] = (a[2] + b[2]) * 0.5f;
		s_blk[s_nblk][2] = fmaxf(400.0f, 0.5f * hypotf(b[0] - a[0], b[2] - a[2]));
		s_nblk++;
#ifdef NAV_HOST
		printf("  leg %d failed at (%.0f %.0f), left out: (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)\n", bad, s_blk[s_nblk - 1][0],
		       s_blk[s_nblk - 1][1], a[0], a[1], a[2], b[0], b[1], b[2]);
#endif
	}
	return 0;
}

bool nav_area_floor(const char* area, float x, float z, float y_ref, float* y)
{
	return load_area(area) && col_floor(&s_ac, x, z, y_ref, 100000.0f, y);
}
