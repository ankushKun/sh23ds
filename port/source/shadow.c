#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "shadow_shbin.h"
#include "quality.h"

// How the PC does it (sh2pc.exe, Director's Cut):
//   0x5aad70  every frame: the shadow light (0x47e000: the flashlight, or an event's light), then
//             0x5a4ed0 for each character, at most four, light kind 0 = spot.
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
#define MAX_CASTERS 4            // 0x5a4ed0
#define MAX_VERTS (3 * 8192)
#define DROP_VERTS (3 * 6000)
#define DROP_CASTERS 16          // the PC's character list (0x1dbe9bc, 16 slots)

typedef struct { float x, y, z; u8 c[4]; } DVtx;

typedef struct { float x, y, z; } V3;

static struct {
	bool ready, on;
	DVLB_s* dvlb;
	shaderProgram_s prog;
	int uProj, uMv;
	V3* vbuf;                    // linear memory, triangles, world space
	int nv;
	int casters;
	u64 cast_ticks;              // CPU time building the volumes this frame
	float light[3], dir[3], cos_cone, sin_cone, strength;
	void* quad;                  // linear memory, the darkening quad
	bool drop;                   // this frame draws drop shadows instead (shadow_begin_drop)
	DVtx* dbuf;                  // linear memory, drop shadow triangles
	int ndv, dcasters;
	float dlight[3];             // the direction drop shadows are cast along (towards the light)
	float da0, da1;              // the drop env's alpha at the top and at the floor (x2, 0x444780)
	u8 dcol;                     // their grey
	float dcam[3];
	char status[48];
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

// One triangle (world space), wound so that its normal points along "out" (the GPU culls by screen
// winding, so every face of a volume must agree on which side is the outside).
static void emit(V3 a, V3 b, V3 c, V3 out)
{
	if (S.nv + 3 > MAX_VERTS)
		return;
	if (dot(cross(sub(b, a), sub(c, a)), out) < 0.0f) {
		V3 t = b;
		b = c, c = t;
	}
	S.vbuf[S.nv++] = a;
	S.vbuf[S.nv++] = b;
	S.vbuf[S.nv++] = c;
}

static V3 pull(V3 p, V3 L)
{
	V3 d = sub(p, L);
	float l = sqrtf(dot(d, d));
	return l > 1.0f ? add(L, scl(d, EXTRUDE / l)) : p;
}

static V3 s16v(const s16* s) { return v3(s[0], s[1], s[2]); }

// The side wall of one silhouette edge (vertices i, j) of a lit triangle whose third corner is k.
static void side(const V3* p, const V3* q, int i, int j, int k)
{
	V3 out = sub(scl(add(p[i], p[j]), 0.5f), p[k]);
	emit(p[i], p[j], q[j], out);
	emit(p[i], q[j], q[i], out);
}

// One TRIANGLE_STRIP_ADV geometry (prim 5 or 6): two vertices, then per triangle its face normal and
// the next vertex, all s16 x, y, z, w in the bone's space. 0x4449f0's light test: the triangle is lit
// when (v - L) . n < 0 (L in the bone's space). Each vertex is moved to world space and pulled away
// from the light once.
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
		lit[t] = dot(sub(v[t], L), n) < 0.0f;
		any |= lit[t];
	}
	if (!any)
		return;
	for (int i = 0; i < nv; i++) {
		p[i] = xf(w, v[i]);
		q[i] = pull(p[i], Lw);
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

static void drop_cast(const Character* c, const float pos[3], float yaw)
{
	if (S.dcasters >= DROP_CASTERS)
		return;
	float dx = pos[0] - S.dcam[0], dz = pos[2] - S.dcam[2];
	if (dx * dx + dz * dz > 10000.0f * 10000.0f)
		return;                                    // 0x5a7730 fades them to 0.3 by then anyway
	const u8* d = c->kg1;
	u32 size = c->kg1_size;
	C3D_Mtx cm;
	Mtx_Identity(&cm);
	Mtx_Translate(&cm, pos[0], pos[1], pos[2], true);
	Mtx_RotateY(&cm, yaw, true);
	Mtx34 model;
	for (int i = 0; i < 3; i++) {
		model.m[i][0] = cm.r[i].x, model.m[i][1] = cm.r[i].y;
		model.m[i][2] = cm.r[i].z, model.m[i][3] = cm.r[i].w;
	}
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
			mul34(&w, &model, &b);
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

void shadow_cast(const Character* c, const float pos[3], float yaw)
{
	if (S.drop && c->kg1 && c->kg1_size >= 16) {
		u64 t0 = svcGetSystemTick();
		drop_cast(c, pos, yaw);
		S.cast_ticks += svcGetSystemTick() - t0;
		return;
	}
	if (!S.on || !c->kg1 || c->kg1_size < 16 || S.casters >= MAX_CASTERS)
		return;
	u64 t0 = svcGetSystemTick();
	const u8* d = c->kg1;
	u32 size = c->kg1_size;
	V3 Lw = v3(S.light[0], S.light[1], S.light[2]), D = v3(S.dir[0], S.dir[1], S.dir[2]);
	V3 at = sub(v3(pos[0], pos[1], pos[2]), Lw);
	if (dot(at, at) > (RANGE + 2000.0f) * (RANGE + 2000.0f))
		return;                                    // the whole character is out of reach
	C3D_Mtx cm;
	Mtx_Identity(&cm);
	Mtx_Translate(&cm, pos[0], pos[1], pos[2], true);
	Mtx_RotateY(&cm, yaw, true);
	Mtx34 model;
	for (int i = 0; i < 3; i++) {
		model.m[i][0] = cm.r[i].x, model.m[i][1] = cm.r[i].y;
		model.m[i][2] = cm.r[i].z, model.m[i][3] = cm.r[i].w;
	}
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
			mul34(&w, &model, &b);
			inv_rigid(&iw, &w);
			V3 L = xf(&iw, Lw);
			// 0x444b20's object test: inside the range and the cone (with the bounding sphere)
			V3 cw = sub(xf(&w, s16v(bnd)), Lw);
			float r = bnd[3], dist = sqrtf(dot(cw, cw));
			bool in = dist <= RANGE + r;
			if (in && dist > r) {
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
	if (S.nv > before)
		S.casters++;
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
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

void shadow_draw(const C3D_Mtx* proj, const C3D_Mtx* view, const C3D_Mtx* ortho)
{
	if (S.drop)
		snprintf(S.status, sizeof S.status, "shadow drop casters %d tris %d %.2f ms", S.dcasters, S.ndv / 3,
		         S.cast_ticks * 1000.0f / SYSCLOCK_ARM11);
	else
		snprintf(S.status, sizeof S.status, "shadow %s casters %d tris %d %.2f ms", S.on ? "on" : "off",
		         S.casters, S.nv / 3, S.cast_ticks * 1000.0f / SYSCLOCK_ARM11);
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
