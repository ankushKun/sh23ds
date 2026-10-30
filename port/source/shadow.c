#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "events.h"
#include "stream.h"
#include "shadow_shbin.h"
#include "quality.h"
#include "stereo.h"
#include "gpucost.h"

#define PACK_DIR "sdmc:/3ds/sh23ds"

// How the PC does it (sh2pc.exe, Director's Cut):
//   0x5aad70  every frame: the shadow light (0x47e000: the flashlight, or an event's light), then
//             0x5a4ed0 for each loaded map's .kg2, at most four, light kind 0 = spot (the maps'
//             section below). The characters join from the draw loop (0x50b5d0 -> 0x5a5c70, at
//             most 16, a vertex budget 0x5a6220).
//   0x445390  walks the casters' bone blocks; 0x444b20 (spot) culls each .kg1 object and strip
//             against the light's range (5000, 0x5a8630) and cone, and 0x4449f0 turns the strips
//             into side walls: for a run of triangles whose face normal looks at the light, the
//             edges on the outside of the run, pulled away from the light (0x442ae0 per quad).
//   0x4426f0  draws them twice into the stencil buffer, culling one winding with D3DSTENCILOP_INCR
//             and the other with D3DSTENCILOP_DECR; colour writes off (blend ZERO / ONE).
//   0x442d30  a full-screen quad with stencil ref 0x81, func LESSEQUAL, blend ZERO / SRCALPHA and
//             a black vertex colour of alpha 0x80: every pixel with a count of one or more above
//             the 0x80 the buffer starts at is halved. Characters write stencil 0 when drawn, so
//             they never take a shadow.
// The port keeps all of that except the counting method: the PC counts on depth pass (z-pass) with
// open volumes, which goes wrong when the camera stands inside a volume, and the fixed cameras of
// SH2 often look back at James past a monster. Here the volumes get both caps (the lit triangles
// and their pulled-away copies) and are counted on depth fail (z-fail), which gives the same
// shadow from anywhere. The darkness is the Enhanced Edition's (EnableSoftShadows,
// IDirect3DDevice8.cpp GetShadowOpacity, zlib licence), faded with the flashlight as there.

#define RANGE 5000.0f            // 0x5a8630: light_param[2], objects beyond it cast nothing
#define EXTRUDE 6000.0f          // volume length from the light (past RANGE, so it never folds back)
// The PC's character list (0x5a5c70) takes 16 under a vertex budget (0x5a6220); four is the port's
// own limit (3DS-only). The four of 0x5a4ed0 are the maps' (shadow_cast_maps).
#define MAX_CASTERS 4
#define MAX_VERTS (3 * 12288)
#define DROP_VERTS (3 * 6000)
#define DROP_CASTERS 16          // the PC's character list (0x1dbe9bc, 16 slots)

typedef struct { float x, y, z; u8 c[4]; } DVtx;

typedef struct { float x, y, z; } V3;

static struct {
	bool ready, on;
	bool parallel;               // light kind 4 (shadow_begin_parallel): along dir, from far away
	DVLB_s* dvlb;
	shaderProgram_s prog;
	int uProj, uMv;
	V3* vbuf;                    // linear memory, triangles, world space
	int nv;
	int casters;
	u64 cast_ticks;              // CPU time building the volumes this frame
	float light[3], dir[3], cos_cone, sin_cone, strength;
	float len, eps;              // the volume's length from the light, and how far behind its casting
	bool back;                   // faces it starts (maps); back: the faces turned from the light cast
	int map_tris, maps;          // this frame's map volumes (shadow_cast_maps)
	int map_id;                  // the map James is in (shadow_begin_drop, shadow_cast_maps)
	int vmax;                    // the vertices emit may fill (the maps' share, shadow_cast_maps)
	void* quad;                  // linear memory, the darkening quad
	bool drop;                   // this frame draws drop shadows instead (shadow_begin_drop)
	DVtx* dbuf;                  // linear memory, drop shadow triangles
	int ndv, dcasters;
	float dlight[3];             // the direction drop shadows are cast along (towards the light)
	float da0, da1;              // the drop env's alpha at the top and at the floor (x2, 0x444780)
	u8 dcol;                     // their grey
	float dcam[3];
	char status[64];
	int debug;                   // shadow_debug(): 1 volumes in red, 2 darken every changed count
} S;

static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 scl(V3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

// 3x4 row-major affine helpers (Mtx34 as in chr.h)
static V3 xf(const Mtx34* m, V3 p)
{
	return v3(m->m[0][0] * p.x + m->m[0][1] * p.y + m->m[0][2] * p.z + m->m[0][3],
	          m->m[1][0] * p.x + m->m[1][1] * p.y + m->m[1][2] * p.z + m->m[1][3],
	          m->m[2][0] * p.x + m->m[2][1] * p.y + m->m[2][2] * p.z + m->m[2][3]);
}

static void mul34(Mtx34* out, const Mtx34* a, const Mtx34* b)
{
	Mtx34 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] +
			            (j == 3 ? a->m[i][3] : 0.0f);
	*out = r;
}

// Inverse of a rotation + translation (bone matrices carry no scale).
static void inv_rigid(Mtx34* out, const Mtx34* a)
{
	Mtx34 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i][j] = a->m[j][i];
	for (int i = 0; i < 3; i++)
		r.m[i][3] = -(r.m[i][0] * a->m[0][3] + r.m[i][1] * a->m[1][3] + r.m[i][2] * a->m[2][3]);
	*out = r;
}

bool shadow_init(void)
{
	memset(&S, 0, sizeof S);
	S.dvlb = DVLB_ParseFile((u32*)shadow_shbin, shadow_shbin_size);
	if (!S.dvlb)
		return false;
	shaderProgramInit(&S.prog);
	shaderProgramSetVsh(&S.prog, &S.dvlb->DVLE[0]);
	S.uProj = shaderInstanceGetUniformLocation(S.prog.vertexShader, "projection");
	S.uMv = shaderInstanceGetUniformLocation(S.prog.vertexShader, "modelView");
	S.vbuf = linearAlloc(MAX_VERTS * sizeof(V3));
	S.quad = linearAlloc(6 * sizeof(V3));
	S.dbuf = linearAlloc(DROP_VERTS * sizeof(DVtx));
	S.ready = S.vbuf && S.quad && S.dbuf;
	snprintf(S.status, sizeof S.status, "shadow %s", S.ready ? "ready" : "off");
	return S.ready;
}

void shadow_exit(void)
{
	if (S.vbuf)
		linearFree(S.vbuf);
	if (S.quad)
		linearFree(S.quad);
	if (S.dbuf)
		linearFree(S.dbuf);
	if (S.dvlb) {
		shaderProgramFree(&S.prog);
		DVLB_Free(S.dvlb);
	}
	memset(&S, 0, sizeof S);
}

void shadow_begin(const float pos[3], const float dir[3], float cone, float strength)
{
	S.nv = 0;
	S.casters = 0;
	S.drop = false;
	S.ndv = S.dcasters = 0;
	S.cast_ticks = 0;
	S.parallel = false;
	S.len = EXTRUDE, S.eps = 0.0f, S.back = false;
	S.map_tris = S.maps = 0;
	S.vmax = MAX_VERTS;
	S.on = S.ready && strength > 0.0f && S.debug != 6 && quality()->stencil_shadows;   // off on the low quality level (quality.h, set by profile.c)
	if (!S.on)
		return;
	memcpy(S.light, pos, sizeof S.light);
	float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	for (int i = 0; i < 3; i++)
		S.dir[i] = l > 0.0f ? dir[i] / l : 0.0f;
	S.cos_cone = cosf(cone);
	S.sin_cone = sinf(cone);
	S.strength = strength > 1.0f ? 1.0f : strength;
}

void shadow_begin_parallel(const float dir[3], float strength)
{
	static const float none[3] = { 0.0f, 0.0f, 0.0f };
	shadow_begin(none, dir, 0.0f, strength);
	S.parallel = S.on;
}

// One triangle (world space), wound so that its normal points along "out" (the GPU culls by screen
// winding, so every face of a volume must agree on which side is the outside).
static void emit(V3 a, V3 b, V3 c, V3 out)
{
	if (S.nv + 3 > S.vmax)
		return;
	if (dot(cross(sub(b, a), sub(c, a)), out) < 0.0f) {
		V3 t = b;
		b = c, c = t;
	}
	S.vbuf[S.nv++] = a;
	S.vbuf[S.nv++] = b;
	S.vbuf[S.nv++] = c;
}

// A corner pulled S.len from the light (a corner farther than that a little past itself, where the
// PC's volume would fold back), and *p moved S.eps away from it (map volumes, see shadow_cast_maps).
static V3 pull_from(V3* p, V3 L)
{
	if (S.parallel)
		return add(*p, scl(v3(S.dir[0], S.dir[1], S.dir[2]), S.len));
	V3 d = sub(*p, L);
	float l = sqrtf(dot(d, d));
	if (l <= 1.0f)
		return *p;
	V3 q = add(L, scl(d, (S.len > l + 1.0f ? S.len : l + 1.0f) / l));
	if (S.eps > 0.0f)
		*p = add(*p, scl(d, S.eps / l));
	return q;
}

static V3 s16v(const s16* s) { return v3(s[0], s[1], s[2]); }
static void maps_release(void);

// The side wall of one silhouette edge (vertices i, j) of a lit triangle whose third corner is k.
static void side(const V3* p, const V3* q, int i, int j, int k)
{
	V3 out = sub(scl(add(p[i], p[j]), 0.5f), p[k]);
	emit(p[i], p[j], q[j], out);
	emit(p[i], q[j], q[i], out);
}

// One TRIANGLE_STRIP_ADV geometry (prim 5 or 6): two vertices, then per triangle its face normal and
// the next vertex, all s16 x, y, z, w in the bone's space. The characters' volumes start at the
// triangles with (v - L) . n < 0 (L in the bone's space); the maps' at those with (v - L) . n >= 0,
// the runs 0x4449f0 walks (S.back), so a map object's lit side lies outside its own volume. Each
// vertex is moved to world space and pulled away from the light once.
static void strip(const Mtx34* w, const s16* g, int nv, V3 L, V3 Lw)
{
	if (nv < 3 || nv > 256)
		return;
	V3 v[256], p[256], q[256];
	bool lit[256];
	v[0] = s16v(g);
	v[1] = s16v(g + 4);
	const s16* t16 = g + 8;
	bool any = false;
	for (int t = 0; t < nv - 2; t++, t16 += 8) {
		V3 n = s16v(t16);
		v[t + 2] = s16v(t16 + 4);
		lit[t] = (dot(sub(v[t], L), n) < 0.0f) != S.back;
		any |= lit[t];
	}
	if (!any)
		return;
	for (int i = 0; i < nv; i++) {
		p[i] = xf(w, v[i]);
		q[i] = pull_from(&p[i], Lw);
	}
	int nt = nv - 2;
	for (int t = 0; t < nt; t++) {
		if (!lit[t])
			continue;
		int a = t, b = t + 1, c = t + 2;
		V3 ctr = scl(add(add(p[a], p[b]), p[c]), 1.0f / 3.0f);
		emit(p[a], p[b], p[c], sub(Lw, ctr));                  // near cap, facing the light
		emit(q[a], q[b], q[c], sub(ctr, Lw));                  // far cap
		side(p, q, a, c, b);                                   // the strip's outer edge
		if (t == 0 || !lit[t - 1])
			side(p, q, a, b, c);                               // where the lit run starts
		if (t == nt - 1 || !lit[t + 1])
			side(p, q, b, c, a);                               // and where it ends
	}
}

// 0x5a7c90 / 0x5a7730: the drop shadows of the lit rooms. The env (0x5ab750) and the light
// (0x5a7730's (1000, -10000, 1000), 0x5ab830) have a few per-map exceptions.
void shadow_begin_drop(int map_id, const float cam[3])
{
	S.nv = S.casters = 0;
	S.ndv = S.dcasters = 0;
	S.on = false;
	S.cast_ticks = 0;
	S.drop = S.ready && S.debug != 6;
	S.map_id = map_id;
	S.vmax = MAX_VERTS;
	maps_release();                  // the lit rooms cast no map shadows
	if (!S.drop)
		return;
	int glb = (map_id >> 16) & 0xfff, map = map_id & 0xffff;
	// DAT_008f1d48: { 1, 0.0, 32.0, 8 }: alpha 0 at the top, 32 at the floor (both doubled), grey 8
	float a0 = 0.0f, a1 = 32.0f;
	int col = 8;
	if (glb == 1 && (map == 10 || map == 11 || map == 13 || map == 14)) a1 = 64.0f, col = 0;
	else if (glb == 3 && map == 0x40) a1 = 16.0f;
	else if (glb == 5) a0 = 32.0f, a1 = 32.0f, col = 0;
	else if ((glb == 9 && map == 0x54) || (glb == 0xb && (map == 0xbd || map == 0xc1))) a0 = 32.0f, a1 = 32.0f;
	S.da0 = a0, S.da1 = a1, S.dcol = (u8)col;
	S.dlight[0] = 1000.0f, S.dlight[1] = -10000.0f, S.dlight[2] = 1000.0f;
	if (glb == 0xb && (map == 0xbd || map == 0xc1))
		S.dlight[0] = S.dlight[2] = 0.0f;      // 0x5ab830: straight down in these two rooms
	memcpy(S.dcam, cam, sizeof S.dcam);
}

// 0x444780 for one strip: the triangles that face the light (the PC culls the others after the
// projection), flattened along the light onto a plane 16 units above the floor (0x5a6ad0,
// 0x5a4d20); each corner's alpha runs from 2 x da0 700 units up to 2 x da1 at the floor, at most 128.
static void drop_strip(const Mtx34* w, const s16* g, int nv, float floor_y)
{
	if (nv < 3 || nv > 256)
		return;
	V3 D = v3(S.dlight[0], S.dlight[1], S.dlight[2]);
	float y0 = floor_y - 16.0f, h1 = floor_y, h0 = floor_y - 700.0f;
	float k = 2.0f * (S.da1 - S.da0) / (h1 - h0), base = 2.0f * S.da0;
	V3 p[256], q[256];
	u8 al[256];
	const s16* t16 = g + 8;
	p[0] = xf(w, s16v(g));
	p[1] = xf(w, s16v(g + 4));
	for (int t = 0; t < nv - 2; t++, t16 += 8)
		p[t + 2] = xf(w, s16v(t16 + 4));
	for (int i = 0; i < nv; i++) {
		float a = base + (p[i].y - h0) * k;
		al[i] = (u8)(a < 0.0f ? 0.0f : a > 128.0f ? 128.0f : a);
		float s = (y0 - p[i].y) / D.y;
		q[i] = add(p[i], scl(D, s));
	}
	t16 = g + 8;
	for (int t = 0; t < nv - 2; t++, t16 += 8) {
		V3 nl = s16v(t16);
		V3 n = v3(w->m[0][0] * nl.x + w->m[0][1] * nl.y + w->m[0][2] * nl.z,
		          w->m[1][0] * nl.x + w->m[1][1] * nl.y + w->m[1][2] * nl.z,
		          w->m[2][0] * nl.x + w->m[2][1] * nl.y + w->m[2][2] * nl.z);
		if (dot(n, D) <= 0.0f || S.ndv + 3 > DROP_VERTS)
			continue;
		int idx[3] = { t, t + 1, t + 2 };
		for (int j = 0; j < 3; j++) {
			DVtx* v = &S.dbuf[S.ndv++];
			v->x = q[idx[j]].x, v->y = q[idx[j]].y, v->z = q[idx[j]].z;
			v->c[0] = v->c[1] = v->c[2] = S.dcol;
			v->c[3] = al[idx[j]];
			if (S.debug == 7)
				v->c[0] = 255, v->c[3] = 200;   // shadowdebug 7: drop shadows in strong red
		}
	}
}

// A character's place and turn as a model matrix
static void c3d_to34(Mtx34* m, const C3D_Mtx* cm)
{
	for (int i = 0; i < 3; i++) {
		m->m[i][0] = cm->r[i].x, m->m[i][1] = cm->r[i].y;
		m->m[i][2] = cm->r[i].z, m->m[i][3] = cm->r[i].w;
	}
}

static void model_at(Mtx34* m, const float pos[3], float yaw)
{
	C3D_Mtx cm;
	Mtx_Identity(&cm);
	Mtx_Translate(&cm, pos[0], pos[1], pos[2], true);
	Mtx_RotateY(&cm, yaw, true);
	c3d_to34(m, &cm);
}

static void drop_cast(const Character* c, const Mtx34* model, const float pos[3])
{
	if (S.dcasters >= DROP_CASTERS)
		return;
	float dx = pos[0] - S.dcam[0], dz = pos[2] - S.dcam[2];
	if (dx * dx + dz * dz > 10000.0f * 10000.0f)
		return;                                    // 0x5a7730 fades them to 0.3 by then anyway
	const u8* d = c->kg1;
	u32 size = c->kg1_size;
	int nobj = *(const s16*)(d + 4);
	u32 o = 16;
	int before = S.ndv;
	for (int k = 0; k < nobj && o + 0x60 <= size; k++) {
		int bone = *(const s16*)(d + o + 4), ngeo = *(const s16*)(d + o + 6);
		u32 g = o + 0x60, next = g;
		for (int j = 0; j < ngeo && next + 16 <= size; j++)
			next += 16u * (u32)*(const s16*)(d + next + 6);
		if (bone >= 0 && (u32)bone < c->n_bones) {
			Mtx34 bind, b, w;
			inv_rigid(&bind, &c->inv_bind[bone]);
			mul34(&b, &c->skin[bone], &bind);
			mul34(&w, model, &b);
			for (int j = 0; j < ngeo && g + 16 <= size; j++) {
				const s16* h = (const s16*)(d + g);
				int nv = h[0], prim = h[1], qw = h[3];
				if ((prim == 5 || prim == 6 || prim == 8 || prim == 9) && g + 16u * (u32)qw <= size)
					drop_strip(&w, h + 8, nv, pos[1]);
				g += 16u * (u32)(qw > 0 ? qw : 1);
			}
		}
		o = next;
	}
	if (S.ndv > before)
		S.dcasters++;
}

// The volumes of c's .kg1 from the light Lw; cull: the spot's range and cone tests (0x444b20)
static void cast(const Character* c, const Mtx34* model, V3 Lw, bool cull, bool count)
{
	u64 t0 = svcGetSystemTick();
	const u8* d = c->kg1;
	u32 size = c->kg1_size;
	V3 D = v3(S.dir[0], S.dir[1], S.dir[2]);
	int nobj = *(const s16*)(d + 4);
	u32 o = 16;
	int before = S.nv;
	for (int k = 0; k < nobj && o + 0x60 <= size; k++) {
		int bone = *(const s16*)(d + o + 4), ngeo = *(const s16*)(d + o + 6);
		const s16* bnd = (const s16*)(d + o + 0x18);
		u32 g = o + 0x60;
		u32 next = g;
		for (int j = 0; j < ngeo && next + 16 <= size; j++)
			next += 16u * (u32)*(const s16*)(d + next + 6);
		if (bone >= 0 && (u32)bone < c->n_bones) {
			// bone -> world: model * pose * bind (S3C keeps inv_bind; the bones are rigid)
			Mtx34 bind, b, w, iw;
			inv_rigid(&bind, &c->inv_bind[bone]);
			mul34(&b, &c->skin[bone], &bind);
			mul34(&w, model, &b);
			inv_rigid(&iw, &w);
			V3 L = xf(&iw, Lw);
			// 0x444b20's object test: inside the range and the cone (with the bounding sphere)
			V3 cw = sub(xf(&w, s16v(bnd)), Lw);
			float r = bnd[3], dist = sqrtf(dot(cw, cw));
			bool in = S.parallel || !cull || dist <= RANGE + r;
			if (in && !S.parallel && cull && dist > r) {
				float ca = dot(cw, D) / dist, sa = sqrtf(fmaxf(0.0f, 1.0f - ca * ca));
				float sr = r / dist, cr = sqrtf(fmaxf(0.0f, 1.0f - sr * sr));
				// angle to the axis minus the sphere's half angle, against the cone
				in = ca * cr + sa * sr >= S.cos_cone;
			}
			for (int j = 0; in && j < ngeo && g + 16 <= size; j++) {
				const s16* h = (const s16*)(d + g);
				int nv = h[0], prim = h[1], qw = h[3];
				if ((prim == 5 || prim == 6 || prim == 8 || prim == 9) && g + 16u * (u32)qw <= size)
					strip(&w, h + 8, nv, L, Lw);
				g += 16u * (u32)(qw > 0 ? qw : 1);
			}
		}
		o = next;
	}
	if (S.nv > before && count)
		S.casters++;
	S.cast_ticks += svcGetSystemTick() - t0;
}

static void cast_at(const Character* c, const Mtx34* model, const float pos[3])
{
	if (stereo_eye())                // the second eye of a 3D frame draws the first one's (stereo.h)
		return;
	if (S.drop && c->kg1 && c->kg1_size >= 16) {
		u64 t0 = svcGetSystemTick();
		drop_cast(c, model, pos);
		S.cast_ticks += svcGetSystemTick() - t0;
		return;
	}
	if (!S.on || !c->kg1 || c->kg1_size < 16 || S.casters >= MAX_CASTERS)
		return;
	V3 Lw = v3(S.light[0], S.light[1], S.light[2]), D = v3(S.dir[0], S.dir[1], S.dir[2]);
	if (S.parallel)                  // the light tests see a point far back along the light's way
		Lw = sub(v3(pos[0], pos[1], pos[2]), scl(D, 100000.0f));
	V3 at = sub(v3(pos[0], pos[1], pos[2]), Lw);
	if (!S.parallel && dot(at, at) > (RANGE + 2000.0f) * (RANGE + 2000.0f))
		return;                                    // the whole character is out of reach
	cast(c, model, Lw, true, true);
}

void shadow_cast(const Character* c, const float pos[3], float yaw)
{
	Mtx34 m;
	model_at(&m, pos, yaw);
	cast_at(c, &m, pos);
}

// 0x5abcd0: the placed models the PC keeps out of the shadows by the map James is in
static bool placed_excluded(int kind)
{
	int glb = (S.map_id >> 16) & 0xffff, map = S.map_id & 0xffff;
	return (glb == 9 && map == 0x2a && kind == 0x50e) || (glb == 10 && map == 0x96 && kind == 0x420) ||
	       (glb == 0xb && map == 0xc1 && (kind == 0x420 || kind == 0x421)) ||
	       (glb == 0xc && map == 0x5b && kind == 0x412);
}

void shadow_cast_placed(const Character* c, const C3D_Mtx* model, int kind)
{
	// 0x50b5d0 / 0x50b770 add kinds 0x400..0x5ff through 0x50b210 when the model table gives a .kg1
	if (kind < 0x400 || kind > 0x5ff || !c->kg1 || placed_excluded(kind))
		return;
	Mtx34 m;
	c3d_to34(&m, model);
	for (int i = 0; i < 3; i++) {
		float l = m.m[0][i] * m.m[0][i] + m.m[1][i] * m.m[1][i] + m.m[2][i] * m.m[2][i];
		if (fabsf(l - 1.0f) > 0.01f)
			return;                  // a scaled one (stagemdl.c stay_scale): the volumes take rigid bones
	}
	float pos[3] = { m.m[0][3], m.m[1][3], m.m[2][3] };
	cast_at(c, &m, pos);
}

void shadow_cast_james(const Character* c, const float pos[3], float yaw, const float* light)
{
	if (stereo_eye() || !c->kg1 || c->kg1_size < 16)
		return;
	Mtx34 m;
	model_at(&m, pos, yaw);
	if (S.drop) {
		u64 t0 = svcGetSystemTick();
		drop_cast(c, &m, pos);
		S.cast_ticks += svcGetSystemTick() - t0;
		return;
	}
	if (!S.on || S.parallel || !light)
		return;
	cast(c, &m, v3(light[0], light[1], light[2]), false, false);
}

// ================================================================ the maps' shadow models (.kg2)
//
// stencil_shadow_main (0x5aad70) also hands the .kg2 of every loaded map
// (loadBgKG2_GetLoadedDataAddrList (0x512e40): 0x512d60's list over the eight map slots, file kind
// 1) to sh2shd_add_map (0x5a4ed0) with the same light: at most four (0x1dbe9fc, count 0x1dbe9b2),
// glb 9 map 7 skipped. sh2shd_init_outdoor_man2 (0x5a8630) copies each object's matrix and the
// light, its sine (sqrt(1 - cos^2)) and range 5000. Each frame sh2shd_renew_shadow_man (0x5a6bc0)
// sets the map's env (sh2shd_get_shadow_env_bg (0x5ac0a0): the clip kind and the volume length, by
// glb and map), and sh2shd_renew_outdoor_man (0x5a8d80) its light in each object's space, the
// length (at least 8000 for a billboard object, 10000 for type 4) and the cull position, 300 back
// from the light across the floor. sh2shd_make_reftag_pool_outdoor (0x5a8830) and
// sh2shd_add_outdoor_to_kick_packet (0x5a8a50) list the objects, dropping those switched off
// (sh2shd_add_map_to_shadow_off_work (0x5a5560), sh2shd_off_obj (0x5a55e0)), and 0x444b20 culls
// them for 0x4449f0: clip kind 999 casts every geometry, 666 none; otherwise the object's sphere
// within the range across the floor and inside the cone (its apex moved back by r / sin), then
// each geometry's sphere in front of the light, within the range and the cone. 0x5a8370 casts them
// only with James' flashlight on (0x50a190, 0x48c840) or in the rooms with a light of their own
// (check_bg_light_exist (0x5abfb0)), and not where the record's ShadowDarkNess is 2
// (sh2shd_bg_shadow_off (0x5a5860)).
//
// 3DS-only: the volumes are closed (both caps, counted on depth fail as the characters'), each
// starts MAP_EPS behind its casting face so that a face lying on the map's own surface does not
// shade it in speckles, and the maps' volumes stop at MAP_TRIS triangles a frame (the PC has no
// such limit), which leaves the characters theirs.

#define MAX_MAPS 4
#define MAP_CACHE 8
#define MAP_PULL 300.0f          // 0x69f3dc
#define MAP_EPS 4.0f             // 3DS-only (shadow_cast_maps)
#define MAX_OFF 64
#define MAP_WAIT 8               // frames the reader thread gets before the file is read here
#define MAP_TRIS 4096

typedef struct {
	char name[16];               // the room file, "ap104"
	int map;                     // its number (the PC's loader writes glb and map into the header)
	u8* d;
	u32 size;
	s8 state;                    // 0 asked of the reader thread, 1 read, -1 none
	bool want;
	u8 wait;                     // frames it has been on its way
} MapShd;

static MapShd s_map[MAP_CACHE];
static int s_order[MAP_CACHE], s_norder;
static char s_marea[8];
static struct { s16 map, obj; } s_off[MAX_OFF];
static int s_noff;

void shadow_maps_off_begin(void) { s_noff = 0; }

void shadow_map_off(int map, int obj)
{
	if (s_noff < MAX_OFF)
		s_off[s_noff].map = (s16)map, s_off[s_noff++].obj = (s16)obj;
}

static bool map_obj_off(int map, int obj)
{
	for (int i = 0; i < s_noff; i++)
		if (s_off[i].map == map && s_off[i].obj == obj)
			return true;
	return false;
}

static void map_path(const MapShd* m, char* out, size_t n)
{
	snprintf(out, n, PACK_DIR "/bg/%s/%s.kg2", s_marea, m->name);
}

static void map_free(MapShd* m)
{
	free(m->d);
	memset(m, 0, sizeof *m);
}

static void maps_release(void)
{
	for (int i = 0; i < MAP_CACHE && s_norder; i++)
		map_free(&s_map[i]);
	s_norder = 0;
}

void shadow_maps_set(const char* area, const char* const* rooms, int n)
{
	if (strncmp(area, s_marea, sizeof s_marea - 1)) {
		for (int i = 0; i < MAP_CACHE; i++)
			map_free(&s_map[i]);
		snprintf(s_marea, sizeof s_marea, "%s", area);
	}
	bool used[MAP_CACHE] = { false };
	s_norder = 0;
	for (int i = 0; i < n && s_norder < MAP_CACHE; i++) {
		int k = -1;
		for (int j = 0; j < MAP_CACHE && k < 0; j++)
			if (s_map[j].name[0] && !strcmp(s_map[j].name, rooms[i]))
				k = j;
		if (k < 0) {
			for (int j = 0; j < MAP_CACHE && k < 0; j++)
				if (!s_map[j].name[0])
					k = j;
			if (k < 0)
				continue;
			MapShd* m = &s_map[k];
			snprintf(m->name, sizeof m->name, "%s", rooms[i]);
			const char* num = m->name;
			while (*num && (*num < '0' || *num > '9'))
				num++;
			m->map = atoi(num);
			m->state = 0;
			m->want = true;
		}
		used[k] = true;
		s_order[s_norder++] = k;
	}
	for (int j = 0; j < MAP_CACHE; j++)
		if (!used[j] && s_map[j].name[0])
			map_free(&s_map[j]);
}

// Reads what the reader thread has brought in; a file it dropped or never took is read here.
static void map_poll(MapShd* m)
{
	if (m->state != 0)
		return;
	char path[96];
	map_path(m, path, sizeof path);
	int st = stream_file_state(path);
	if (m->want && st == -1) {
		stream_want_file(path);
		m->want = false;
		st = stream_file_state(path);
	}
	if (st == 0 && ++m->wait < MAP_WAIT)
		return;                          // the reader is busy with the room's textures: wait a little
	long size = 0;
	void* d = st == 1 ? stream_take_file(path, &size) : NULL;   // still queued: read here, the copy is
	                                                             // dropped with the door's read-ahead
	if (!d) {
		FILE* f = fopen(path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			size = ftell(f);
			fseek(f, 0, SEEK_SET);
			d = size >= 16 ? malloc(size) : NULL;
			if (d && fread(d, 1, size, f) != (size_t)size) {
				free(d);
				d = NULL;
			}
			fclose(f);
		}
	}
	m->d = d;
	m->size = d ? (u32)size : 0;
	m->state = d && *(const s16*)((const u8*)d + 4) > 0 ? 1 : -1;
}

typedef struct {
	V3 L, D, Lc;                 // the light, its way, the cull position (0x5a8d80)
	float cosc, sinc, range;
	int clip;                    // SHADOW_ENV clip_kind: 999 all, 666 none, 888 (type 2's clamp)
	float leng;                  // SHADOW_ENV leng
	bool camf;                   // spot_cam_flag (0x5a6bc0): the eye ahead of the light across the floor
	V3 cam;
} MapLight;

typedef struct { s16 map, clip; float leng; } EnvRow;
// sh2shd_get_shadow_env_bg's tables {map, clip kind, length} by glb: 0x8f1d68 (2), 0x8f1d78 (3), 0x8f1dc8 (9),
// 0x8f1de0 (0xb), 0x8f1e10 (0xc), 0x8f1e60 (0xd)
static const EnvRow k_env2[] = { { 30, 888, 8000 }, { 37, 888, 6000 } };
static const EnvRow k_env3[] = { { 4, 0, 8000 }, { 22, 0, 8000 }, { 24, 0, 8000 }, { 47, 999, 6000 },
                                 { 53, 0, 8000 }, { 71, 999, 6000 } };
static const EnvRow k_env9[] = { { 1, 0, 6000 }, { 30, 0, 3000 }, { 104, 999, 8000 } };
static const EnvRow k_envb[] = { { 33, 999, 6000 }, { 51, 999, 6000 }, { 93, 0, 8000 }, { 113, 999, 6000 },
                                 { 145, 0, 2500 }, { 189, 999, 6000 } };
static const EnvRow k_envc[] = { { 5, 999, 6000 }, { 9, 999, 6000 }, { 21, 666, 6000 }, { 25, 999, 8000 },
                                 { 29, 999, 8000 }, { 37, 999, 6000 }, { 51, 999, 6000 }, { 67, 0, 6000 },
                                 { 71, 999, 6000 }, { 87, 999, 6000 } };
static const EnvRow k_envd[] = { { 61, 999, 6000 } };

static void env_row(MapLight* m, const EnvRow* t, int n, int map)
{
	for (int i = 0; i < n; i++)
		if (t[i].map == map) {
			m->clip = t[i].clip, m->leng = t[i].leng;
			return;
		}
}

static void env_light(MapLight* m, float x, float y, float z, float cosc)
{
	m->L = v3(x, y, z), m->D = v3(0.0f, 1.0f, 0.0f);
	m->cosc = cosc, m->sinc = sqrtf(1.0f - cosc * cosc);
}

// sh2shd_get_shadow_env_bg (0x5ac0a0). Not ported: glb 0xb map 0x15 sets 0x1dbe9a2, whose use was
// not traced. ap42's test of RadioNoise() < 5 takes the radio as quiet (the port has no radio level).
static void map_env(MapLight* m, int glb, int map, const ShadowMapCtx* c)
{
	m->clip = 0, m->leng = 6000.0f;      // DAT_008f1d58
	const float* j = c->james;
	switch (glb) {
	case 2: env_row(m, k_env2, 2, map); break;
	case 3:
		env_row(m, k_env3, 6, map);
		if (map == 0x35 && j[0] < -84320.0f)
			m->clip = 666;
		break;
	case 6:
		if (map == 5)
			m->L.y -= 200.0f, m->clip = 999;
		break;
	case 7:
		if (map == 1)                    // a lamp straight down, cone 45 degrees
			env_light(m, 15850.0f, -1500.0f, 18650.0f, 0.70703f), m->leng = 3000.0f;
		break;
	case 8:
		if (map == 1)
			m->clip = 999;
		break;
	case 9:
		env_row(m, k_env9, 3, map);
		if (map == 0x2a && !c->flashlight)   // RadioNoise() < 5: see above
			m->clip = 666;
		break;
	case 10:
		switch (map) {
		case 5: case 9: case 0xd: case 0x11: case 0x1d: case 0x25: case 0x31: case 0x35: case 0x3d:
		case 0x41: case 0x69: case 0x79: case 0x7d: case 0x91: case 0x9a: case 0xae: case 0xc2: case 0xd2:
		case 0xde:
			m->clip = 999;
			break;
		case 0x2d:
			m->L.y -= 200.0f, m->clip = 999;
			break;
		case 0x49: case 0x71: case 0xb2: case 0xca: case 0xce: case 0xd6:
			m->leng = 8000.0f;
			break;
		case 0x96:
			if (j[2] > -222800.0f)
				m->clip = 999;
			m->leng = 8000.0f;
			break;
		case 0xbe:
			m->clip = 999, m->leng = 8000.0f;
			break;
		}
		break;
	case 0xb:
		env_row(m, k_envb, 6, map);
		if (map == 0x91 && c->cam[1] < -2000.0f)
			m->clip = 666;
		break;
	case 0xc:
		env_row(m, k_envc, 10, map);
		if (map == 0x11 && j[0] > -64500.0f && j[0] < -59800.0f && j[2] < -63500.0f && j[2] > -64600.0f &&
		    c->yaw < 0.0f)
			m->clip = 666;
		else if (map == 0x19 && !c->flashlight) {
			m->clip = 0;
			env_light(m, -20000.0f, -2000.0f, 25500.0f, m->cosc);
		}
		break;
	case 0xd: env_row(m, k_envd, 1, map); break;
	case 0x10:
		if (map == 0x45)
			m->clip = ev_flag(710) ? 999 : 666;   // 0x1f7d638 & 0x40
		break;
	}
}

// check_bg_light_exist (0x5abfb0): the rooms whose own light casts (0 or 4); 9 for the rest, which
// cast under the flashlight
static bool own_light(int glb, int map)
{
	return (glb == 7 && map == 1) || (glb == 9 && map == 0x2a) || (glb == 0xc && (map == 0x19 || map == 0x5b)) ||
	       (glb == 0x10 && map == 0x45);
}

// sh2shd_shadow_off (0x5ab8d0): the shadow code's own switches by the map James is in, his place
// and the eye. Returns false where it turns the maps' shadows off for the frame (0x5a5860). Its character switches
// (glb 10 map 0x2d, kind 0x104) are not ported.
static bool map_rules(const ShadowMapCtx* c)
{
	int glb = (c->map_id >> 16) & 0xffff, map = c->map_id & 0xffff;
	const float* j = c->james;
	switch (glb) {
	case 9:
		if (map == 1 && j[0] > -20300.0f && j[0] < -19620.0f && j[2] > -18820.0f && j[2] < -18200.0f)
			shadow_map_off(1, 8), shadow_map_off(1, 0x2b);
		break;
	case 10:
		if (map == 0x96 && j[2] > -222800.0f)
			shadow_map_off(0x96, 8);
		else if (map == 0xbe) {
			if (c->cam[0] < -22000.0f) {
				shadow_map_off(0xbe, 0xf);
				if (j[2] > -182000.0f)
					shadow_map_off(0xbe, 0x10);
			} else if (c->cam[0] > -17900.0f) {
				shadow_map_off(0xbe, 0xe);
				if (j[2] > -182000.0f)
					shadow_map_off(0xbe, 0xd);
			}
		} else if (map == 0xe6)
			return false;
		break;
	case 0xb:
		if (map == 0x71 && c->cam[1] > -1000.0f) {
			static const u8 o[] = { 2, 0x15, 0x17, 0x16, 0x18 };
			for (unsigned i = 0; i < sizeof o; i++)
				shadow_map_off(0x71, o[i]);
		}
		break;
	case 0xc:
		if (map == 9 && c->cam[1] > -2000.0f)
			shadow_map_off(9, 0x52);
		else if (map == 0x19 && j[2] < 18800.0f) {
			if (j[0] < -20600.0f && c->yaw > 0.0f)
				shadow_map_off(0x19, 0xa6);
			else if (j[0] > -19400.0f && c->yaw < 0.0f)
				shadow_map_off(0x19, 0xa5);
		}
		break;
	}
	return true;
}

// 0x444b20's cone test with the apex moved back by r / sin
static bool in_cone(const MapLight* m, V3 c, float r)
{
	V3 a = sub(c, sub(m->Lc, scl(m->D, m->sinc > 0.01f ? r / m->sinc : 0.0f)));
	float l = sqrtf(dot(a, a));
	return l <= 0.0f || dot(a, m->D) / l >= m->cosc;
}

// A D3D row-vector matrix (the .kg2's, translation in the last row) as an Mtx34
static void kg2_mtx(Mtx34* w, const float* M)
{
	for (int i = 0; i < 3; i++) {
		for (int k = 0; k < 3; k++)
			w->m[i][k] = M[k * 4 + i];
		w->m[i][3] = M[12 + i];
	}
}

// One polygon (n corners in world space) as a closed volume: itself, its pulled copy and a wall on
// each edge (0x4449f0's planes and billboards emit the walls, 0x442ae0 a quad each)
static void poly_volume(V3* p, int n, V3 Lw)
{
	V3 q[16], ctr = v3(0, 0, 0);
	for (int i = 0; i < n; i++) {
		q[i] = pull_from(&p[i], Lw);
		ctr = add(ctr, p[i]);
	}
	ctr = scl(ctr, 1.0f / n);
	V3 toL = sub(Lw, ctr), fromL = sub(ctr, Lw);
	for (int i = 1; i + 1 < n; i++) {
		emit(p[0], p[i], p[i + 1], toL);
		emit(q[0], q[i], q[i + 1], fromL);
	}
	for (int i = 0; i < n; i++) {
		int k = (i + 1) % n;
		V3 out = sub(scl(add(p[i], p[k]), 0.5f), ctr);
		emit(p[i], p[k], q[k], out);
		emit(p[i], q[k], q[i], out);
	}
}

// One geometry of a map object. L: the light in the object's space; s704: 0x935704 (the geometry
// and the eye on the same side of the light's way); dxz: 0x9356fc (the object across the floor
// from the cull position).
static void map_geom(const Mtx34* w, const s16* h, V3 L, V3 Lw, const MapLight* m, float s704, float dxz)
{
	int nv = h[0], prim = h[1], qw = h[3];
	const s16* g = h + 8;
	// the corners must lie inside the geometry: 8 bytes each after the normal, a strip's in pairs
	if (prim >= 5 && prim <= 9 && prim != 7 ? qw < nv : 3 + nv > 2 * qw)
		return;
	switch (prim) {
	case 1: case 2: case 7: {        // PLANE: the face normal, then the corners
		if (nv < 3 || nv > 16)
			return;
		V3 n = s16v(g);
		if (dot(sub(s16v(g + 4), L), n) < 0.0f)
			return;                      // cast only with the light behind it
		bool clamp = prim == 2 && m->camf && (m->clip == 888 || s704 >= 0.0f);
		V3 p[16];
		for (int i = 0; i < nv; i++) {
			V3 v = s16v(g + 4 + 4 * i);
			if (clamp && v.y < L.y)
				v.y = L.y;               // not above the light (y points down)
			p[i] = xf(w, v);
		}
		poly_volume(p, nv, Lw);
		return;
	}
	case 3: case 4: case 10: {       // BILLBOARD: turned about the object's up to face the light
		if (nv < 3 || nv > 16)
			return;
		V3 X = v3(L.z, 0.0f, -L.x);      // up x L, up = (0, 1, 0) (0x5aae90: +0x1b0)
		float lx = sqrtf(dot(X, X));
		if (lx < 1e-3f)
			return;
		X = scl(X, 1.0f / lx);
		V3 Z = v3(-X.z, 0.0f, X.x);      // X x up
		int mode = 0;
		float cy = 0.0f;
		if (prim == 4) {
			if (m->camf && s704 >= 0.0f)
				mode = 1, cy = L.y;
			else
				mode = 2, cy = L.y - dxz * 0.75f;   // 0x63b864
		}
		V3 p[16];
		for (int i = 0; i < nv; i++) {
			V3 v = s16v(g + 4 + 4 * i);
			if (mode && v.y < cy)
				v.y = cy;
			V3 r = add(add(scl(X, v.x), v3(0.0f, v.y, 0.0f)), scl(Z, v.z));
			p[i] = xf(w, r);
		}
		poly_volume(p, nv, Lw);
		return;
	}
	case 5: case 6: case 8: case 9:  // TRIANGLE_STRIP_ADV
		strip(w, g, nv, L, Lw);
		return;
	}
}

static void map_cast(const MapShd* ms, int glb, const ShadowMapCtx* c)
{
	MapLight m;
	m.L = v3(S.light[0], S.light[1], S.light[2]);
	m.D = v3(S.dir[0], S.dir[1], S.dir[2]);
	m.cosc = S.cos_cone, m.sinc = S.sin_cone, m.range = RANGE;   // 0x5a8630
	m.cam = v3(c->cam[0], c->cam[1], c->cam[2]);
	{   // 0x5a6bc0: before the env moves the light
		float dx = m.cam.x - m.L.x, dz = m.cam.z - m.L.z, l = sqrtf(dx * dx + dz * dz);
		float k = l > 0.0f ? 1.0f / l : 1.0f;
		m.camf = dx * k * m.D.x + dz * k * m.D.z > 0.0f;
	}
	map_env(&m, glb, ms->map, c);
	if (m.clip == 666)
		return;
	float hl = sqrtf(m.D.x * m.D.x + m.D.z * m.D.z);
	float hk = hl > 0.0f ? MAP_PULL / hl : 0.0f;
	m.Lc = v3(m.L.x - m.D.x * hk, m.L.y, m.L.z - m.D.z * hk);
	const u8* d = ms->d;
	u32 size = ms->size, o = 16;
	int nobj = *(const s16*)(d + 4);
	for (int k = 0; k < nobj && o + 0x60 <= size; k++) {
		const u8* oh = d + o;
		int ngeo = *(const s16*)(oh + 6);
		u32 g = o + 0x60, next = g;
		int walked = 0;
		for (; walked < ngeo && next + 16 <= size; walked++)
			next += 16u * (u32)(*(const s16*)(d + next + 6) > 0 ? *(const s16*)(d + next + 6) : 1);
		o = next;
		if (next > size || walked < ngeo || ngeo <= 0 || map_obj_off(ms->map, k))
			continue;
		Mtx34 w, iw;
		kg2_mtx(&w, (const float*)(oh + 0x20));
		inv_rigid(&iw, &w);
		V3 L = xf(&iw, m.L);
		int prim0 = *(const s16*)(d + g + 2);
		S.len = prim0 == 3 ? fmaxf(m.leng, 8000.0f) : prim0 == 4 ? fmaxf(m.leng, 10000.0f) : m.leng;
		const s16* bs = (const s16*)(oh + 0x18);
		V3 bc = xf(&w, s16v(bs));
		float dxz = sqrtf((bc.x - m.Lc.x) * (bc.x - m.Lc.x) + (bc.z - m.Lc.z) * (bc.z - m.Lc.z));
		bool out = dxz > m.range + bs[3] || !in_cone(&m, bc, bs[3]);
		if (out && m.clip != 999)
			continue;
		for (int j = 0; j < ngeo; j++) {
			const s16* h = (const s16*)(d + g);
			g += 16u * (u32)(h[3] > 0 ? h[3] : 1);
			V3 gc = xf(&w, s16v(h + 4));
			V3 cl = sub(gc, m.Lc);
			float s704 = (m.D.z * cl.x - cl.z * m.D.x) *
			             (m.D.z * (m.cam.x - m.Lc.x) - (m.cam.z - m.Lc.z) * m.D.x);
			if (m.clip != 999) {
				float r = h[7];
				if (dot(cl, m.D) < 0.0f || sqrtf(dot(cl, cl)) > m.range + r || !in_cone(&m, gc, r))
					continue;
			}
			map_geom(&w, h, L, m.L, &m, s704, dxz);
		}
	}
}

void shadow_cast_maps(const ShadowMapCtx* c)
{
	if (S.drop || stereo_eye())
		return;
	S.map_id = c->map_id;
	for (int i = 0; i < s_norder; i++)
		map_poll(&s_map[s_order[i]]);
	if (!S.on || S.parallel || S.debug == 8)   // room 312's light kind 4 (0x5a54d0 / 0x445030) is not ported
		return;
	int glb = (c->map_id >> 16) & 0xffff, map = c->map_id & 0xffff;
	// 0x5a8370 (0x5a5860 clears 0x1dbe9b8, which 0x5a7a00 reads as no map shadows)
	if (!own_light(glb, map) && (!c->flashlight || c->darkness == 2))
		return;
	u64 t0 = svcGetSystemTick();
	int off_from = s_noff;
	bool on = map_rules(c);
	int nv0 = S.nv, vmax = S.vmax;
	S.vmax = S.nv + 3 * MAP_TRIS < MAX_VERTS ? S.nv + 3 * MAP_TRIS : MAX_VERTS;
	for (int i = 0; on && i < s_norder && S.maps < MAX_MAPS; i++) {
		const MapShd* ms = &s_map[s_order[i]];
		if (ms->state != 1 || (glb == 9 && ms->map == 7))
			continue;
		S.maps++;
		S.eps = MAP_EPS, S.back = true;
		map_cast(ms, glb, c);
		S.len = EXTRUDE, S.eps = 0.0f, S.back = false;
	}
	S.vmax = vmax;
	s_noff = off_from;               // the rules' switches last this frame only
	S.map_tris = (S.nv - nv0) / 3;
	S.cast_ticks += svcGetSystemTick() - t0;
}

void shadow_mark_begin(void)
{
	if (!S.on)
		return;
	// characters overwrite the 0x80 the frame starts with by 0: the counts on them never reach 0x81
	C3D_StencilTest(true, GPU_ALWAYS, 0x00, 0xff, 0xff);
	C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
}

void shadow_mark_end(void)
{
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0x00);
	C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
}

// 0x442370 / 0x442810: the drop shadows as translucent grey triangles, fogged, no depth writes
static void drop_draw(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	GSPGPU_FlushDataCache(S.dbuf, S.ndv * sizeof(DVtx));
	C3D_BindProgram(&S.prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, S.dbuf, sizeof(DVtx), 2, 0x10);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, S.uProj, proj);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, S.uMv, view);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DrawArrays(GPU_TRIANGLES, 0, S.ndv);
	if (gpucost_on()) {                  // debug.txt gpucost (gpucost.h)
		C3D_Mtx pv;
		Mtx_Multiply(&pv, proj, view);
		for (int i = 0; i + 2 < S.ndv; i += 3)
			gpucost_tri(GC_DROP, &pv, &S.dbuf[i].x, &S.dbuf[i + 1].x, &S.dbuf[i + 2].x, false);
	}
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

void shadow_draw(const C3D_Mtx* proj, const C3D_Mtx* view, const C3D_Mtx* ortho)
{
	if (S.drop)
		snprintf(S.status, sizeof S.status, "shadow drop casters %d tris %d %.2f ms", S.dcasters, S.ndv / 3,
		         S.cast_ticks * 1000.0f / SYSCLOCK_ARM11);
	else
		snprintf(S.status, sizeof S.status, "shadow %s casters %d maps %d/%d tris %d %.2f ms", S.on ? "on" : "off",
		         S.casters, S.maps, S.map_tris, S.nv / 3, S.cast_ticks * 1000.0f / SYSCLOCK_ARM11);
	if (S.drop && S.ndv) {
		drop_draw(proj, view);
		return;
	}
	if (!S.on || !S.nv)
		return;
	GSPGPU_FlushDataCache(S.vbuf, S.nv * sizeof(V3));
	C3D_BindProgram(&S.prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	int fixed = AttrInfo_AddFixed(ai, 1);            // the colour input: 0
	C3D_FixedAttribSet(fixed, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, S.vbuf, sizeof(V3), 1, 0x0);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, S.uProj, proj);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, S.uMv, view);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
	// z-fail: faces behind the scene count, back faces up and front faces down
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);   // blend ZERO, ONE: the colour stays
	C3D_DepthMap(true, -1.0f, 0.0f);
	if (S.debug == 1) {
		C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
		C3D_TexEnvColor(env, 0x400000ff);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		C3D_DrawArrays(GPU_TRIANGLES, 0, S.nv);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);   // blend ZERO, ONE: the colour stays
		env = C3D_GetTexEnv(0);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	}
	C3D_StencilTest(true, GPU_ALWAYS, 0, 0xff, 0xff);
	bool zpass = S.debug == 5;
	C3D_CullFace(GPU_CULL_FRONT_CCW);
	if (zpass) C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_DECR);
	else C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_INCR, GPU_STENCIL_KEEP);
	C3D_DrawArrays(GPU_TRIANGLES, 0, S.nv);
	C3D_CullFace(GPU_CULL_BACK_CCW);
	if (zpass) C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_INCR);
	else C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_DECR, GPU_STENCIL_KEEP);
	C3D_DrawArrays(GPU_TRIANGLES, 0, S.nv);
	C3D_CullFace(GPU_CULL_NONE);
	if (gpucost_on()) {                  // debug.txt gpucost (gpucost.h): both passes together
		C3D_Mtx pv;
		Mtx_Multiply(&pv, proj, view);
		for (int i = 0; i + 2 < S.nv; i += 3)
			gpucost_tri(GC_SHADOW_VOL, &pv, &S.vbuf[i].x, &S.vbuf[i + 1].x, &S.vbuf[i + 2].x, false);
	}

	// the darkening quad: every pixel above 0x80 times (1 - strength)
	V3* q = S.quad;
	q[0] = v3(0, 0, 0.5f), q[1] = v3(400, 0, 0.5f), q[2] = v3(400, 240, 0.5f);
	q[3] = v3(0, 0, 0.5f), q[4] = v3(400, 240, 0.5f), q[5] = v3(0, 240, 0.5f);
	GSPGPU_FlushDataCache(S.quad, 6 * sizeof(V3));
	bi = C3D_GetBufInfo();               // re-fetched for the same reason as the texenv below
	BufInfo_Init(bi);
	BufInfo_Add(bi, S.quad, sizeof(V3), 1, 0x0);
	C3D_Mtx id;
	Mtx_Identity(&id);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, S.uProj, ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, S.uMv, &id);
	env = C3D_GetTexEnv(0);              // again: a texenv changed after a draw is only sent when re-fetched
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
	C3D_TexEnvColor(env, S.debug == 3 ? 0xff000000u : (u32)(S.strength * 255.0f + 0.5f) << 24);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_StencilTest(S.debug != 3, S.debug == 2 || S.debug >= 5 ? GPU_NOTEQUAL : S.debug == 4 ? GPU_EQUAL : GPU_LESS, 0x80, 0xff, 0x00);
	C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
	gpucost_px(GC_SHADOW_QUAD, 400.0f * 240.0f, 6);

	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0x00);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

// Enhanced Edition GetShadowOpacity (Wrappers/d3d8/IDirect3DDevice8.cpp, zlib licence).
int shadow_ee_opacity(int room, bool bfaw)
{
	if (!bfaw) {
		switch (room) {
		case 0x0f: return 40;                  // R_APT_E_STAIRCASE_N
		case 0xa2: case 0xbd: return 50;       // R_HTL_RM_312, R_END_DOG_RM
		case 0x89: return 70;                  // R_LAB_BOTTOM_D
		case 0x0b: case 0x21: return 110;      // R_BOWL_MAIN, R_APT_W_STAIRCASE_N
		case 0xbf: return 128;                 // R_END_BEDROOM_LEAVE
		}
	} else {
		switch (room) {
		case 0x27: return 30;                  // R_APT_W_RM_203
		case 0xc2: return 60;                  // R_MAN_LOUNGE_2F
		case 0x0c: case 0x0d: case 0x20: case 0x21: case 0x25: case 0x26: case 0xc0: case 0xc1: case 0xc3:
		case 0xc4: case 0xc5: case 0xc6: case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd:
		case 0xce: case 0xcf: case 0xd0: case 0xd1: case 0xd2: case 0xd5:
			return 90;
		case 0xc7: return 128;                 // R_MAN_ATTIC
		}
	}
	return 170;
}

const char* shadow_status(void) { return S.status; }

void shadow_debug(int mode) { S.debug = mode; }
