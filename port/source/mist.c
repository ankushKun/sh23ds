#include "mist.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>

// The PC's spray puff (0x4a4c00 spawn, 0x4a3f60 update, 0x4a4170 draw). The spray (0x545260) posts
// one a 60 Hz tick from the can toward the end of its reach plus 150, with the life argument 12:
//  - the vector gets up to 50 of jitter on each axis (0x4489a0, small values far likelier), its
//    length is the puff's speed and it is then normalised;
//  - phase 0 lasts 0.3 * 12 = 3 ticks: 0.3 speed of travel, the alpha rises 0 -> 32 and the size
//    0 -> 0.2;
//  - phase 1 runs on to 0.6 * 12 = 7 ticks: another 0.3 speed per 3 ticks, sinking 100 a second,
//    alpha 32, size 0.2 * (1 + f) with f over 3 ticks;
//  - phase 2 runs to 14 ticks: 0.5 speed over 7 ticks, still sinking, the alpha falls 32 -> 0 and
//    the size grows 0.4 -> 0.8; then the puff is freed.
// The disc's radius is speed * size * 0.25; its centre carries the alpha (colour alpha * a / 64)
// and its 16-point rim none, so each puff is a soft round cloud. Its texture is effect texture 2
// (poison.tbn2) sampled around a random point with radius 0.125.
//
// The Lying Figure's acid is the same kind of puff (0x4a4580 spawn, 0x4a3a30 update, 0x4a3d00 draw,
// effect 0x1e, the PS2 beta's enEfctSetPoisonFog / enEfctMovePoisonFog / enEfctDrawPoisonFog), drawn
// through the same packet with the same texture. Its vector gets up to 100 of jitter on each axis
// (0x448910 (-100, 10)); it lives 6 + 6 + 10 ticks:
//  - phase 0, 6 ticks: 0.2 speed of travel, the alpha rises 0 -> 32, the size 0 -> 0.5;
//  - phase 1, 6 ticks: another 0.25 speed, sinking 100 a second, alpha 32, size 0.5 -> 1;
//  - phase 2, 10 ticks: another 0.3 speed, still sinking, the alpha falls 32 -> 0, size 1 -> 1.2.
// Its disc's radius is speed * size * 0.25 with the texture round a random point by 0.25, its colour
// a grey (0x80, darker in the dark: 0x4a4580) and its centre's alpha twice the puff's.

#define MAX_MIST 96
#define SEGS 16
#define MAX_VERTS (MAX_MIST * SEGS * 3)

typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;   // the world program's vertex

typedef struct {
	bool on;
	float pos[3], dir[3], speed;
	float t;                     // ticks since the spawn
	float u, v;                  // texture centre
	bool acid;                   // the Lying Figure's acid (else the hyper spray's mist)
	u8 grey;                     // the acid's colour
} Puff;

static BloodBindFn s_bind;
static C3D_Tex s_tex;
static bool s_tex_ok;
static V* s_verts[2];
static int s_buf;
static Puff s_puff[MAX_MIST];
static int s_next;
static float s_acc;              // ticks of spraying not yet spent on a puff
static u8 s_rgb[3] = { 192, 216, 240 };
static u32 s_seed = 0x2468ace;

// 0x448830: the PC's generator, 0..1
static float rnd(void)
{
	s_seed = (s_seed * 0xc1c64e6du + 0x3039u) & 0x7fffffff;
	return (float)s_seed * 4.656613e-10f;
}

// 0x4489a0 (a, 1): a random sign when a < 0, times 2 / (r + 1) - 1
static float jitter(float a)
{
	if (a < 0.0f && (rnd() < 0.5f))
		a = -a;
	return (2.0f / (rnd() + 1.0f) - 1.0f) * a;
}

// 0x448910 (a, b): a random sign when a < 0, times (b + 1) * b / (r + b) - b, in 0..1 with small
// values the likelier the larger b is
static float jitter_b(float a, float b)
{
	if (a < 0.0f && (rnd() < 0.5f))
		a = -a;
	return ((b + 1.0f) * b / (rnd() + b) - b) * a;
}

// 0x5454e6: the spray's colour by the tier the last clear's rank gave (0x539dd0: rank <= 20 -1,
// < 80 0, < 100 1, else 2), as 0xAARRGGBB; 0x4a4170 then scales its bytes by 1, 144/128 and
// 160/128 (clamped to 0xcc, 0xe2, 0xff) and draws them as R, G, B.
void mist_set_color(int tier)
{
	static const u32 COL[4] = { 0xff400040, 0xffc0c0c0, 0xff40c0c0, 0xff40ff40 };
	u32 c = COL[(tier < -1 ? -1 : tier > 2 ? 2 : tier) + 1];
	int b0 = c & 0xff, b1 = (c >> 8) & 0xff, b2 = (c >> 16) & 0xff;
	int r = b0, g = b1 * 144 / 128, b = b2 * 160 / 128;
	s_rgb[0] = (u8)(r > 0xcc ? 0xcc : r);
	s_rgb[1] = (u8)(g > 0xe2 ? 0xe2 : g);
	s_rgb[2] = (u8)(b > 0xff ? 0xff : b);
}

bool mist_init(const char* pack_dir, BloodBindFn bind)
{
	s_bind = bind;
	char path[128];
	snprintf(path, sizeof path, "%s/effect/mist.t3x", pack_dir);
	FILE* f = fopen(path, "rb");
	if (f) {
		Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_tex, NULL, false);
		fclose(f);
		if (t) {
			Tex3DS_TextureFree(t);
			C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&s_tex, GPU_REPEAT, GPU_REPEAT);
			s_tex_ok = true;
		}
	}
	for (int i = 0; i < 2; i++)
		s_verts[i] = linearAlloc(MAX_VERTS * sizeof(V));
	mist_set_color(0);
	return s_tex_ok && s_verts[0] && s_verts[1];
}

void mist_exit(void)
{
	if (s_tex_ok)
		C3D_TexDelete(&s_tex);
	for (int i = 0; i < 2; i++) {
		linearFree(s_verts[i]);
		s_verts[i] = NULL;
	}
	s_tex_ok = false;
}

void mist_clear(void)
{
	memset(s_puff, 0, sizeof s_puff);
	s_acc = 0.0f;
}

int mist_count(void)
{
	int n = 0;
	for (int i = 0; i < MAX_MIST; i++)
		n += s_puff[i].on;
	return n;
}

void mist_spray(const float pos[3], const float vel[3], float dt)
{
	s_acc += dt * 60.0f;
	while (s_acc >= 1.0f) {
		s_acc -= 1.0f;
		Puff* p = &s_puff[s_next];
		s_next = (s_next + 1) % MAX_MIST;
		float d[3];
		for (int k = 0; k < 3; k++)
			d[k] = vel[k] + jitter(-50.0f);
		float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (len < 1.0f)
			continue;
		// the ticks of this frame after its spawn; mist_update adds the frame's dt later on
		*p = (Puff){ .on = true, .speed = len, .t = s_acc - dt * 60.0f, .u = rnd(), .v = rnd() };
		for (int k = 0; k < 3; k++) {
			p->pos[k] = pos[k];
			p->dir[k] = d[k] / len;
		}
	}
}

void mist_poison(const float pos[3], const float vec[3], u8 grey)
{
	Puff* p = &s_puff[s_next];
	s_next = (s_next + 1) % MAX_MIST;
	float d[3];
	for (int k = 0; k < 3; k++)
		d[k] = vec[k] + jitter_b(-100.0f, 10.0f);
	float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len < 1.0f)
		return;
	*p = (Puff){ .on = true, .speed = len, .u = rnd(), .v = rnd(), .acid = true, .grey = grey };
	for (int k = 0; k < 3; k++) {
		p->pos[k] = pos[k];
		p->dir[k] = d[k] / len;
	}
}

// 0x4a3a30 summed over its ticks, as puff_at for the spray.
static void acid_at(const Puff* p, float t, float out[3], float* size, float* alpha)
{
	const float T0 = 6.0f, T1 = 12.0f, T2 = 10.0f;
	float travel, sink;
	if (t < T0) {
		float f = t / T0;
		travel = 0.2f * p->speed * f;
		sink = 0.0f;
		*alpha = 32.0f * f;
		*size = 0.5f * f;
	} else if (t < T1) {
		float f = (t - T0) / T0;
		travel = p->speed * (0.2f + 0.25f * f);
		sink = (t - T0) * (100.0f / 60.0f);
		*alpha = 32.0f;
		*size = 0.5f * (1.0f + f);
	} else {
		float f = (t - T1) / T2;
		travel = p->speed * (0.45f + 0.3f * f);
		sink = (t - T0) * (100.0f / 60.0f);
		*alpha = 32.0f * (1.0f - f);
		*size = 1.0f + 0.2f * f;
	}
	for (int k = 0; k < 3; k++)
		out[k] = p->pos[k] + p->dir[k] * travel;
	out[1] += sink;                              // +y is down
}

// Where a puff is after t ticks, with its size and alpha (0..32): the PC's per-tick steps summed.
static void puff_at(const Puff* p, float t, float out[3], float* size, float* alpha)
{
	if (p->acid) {
		acid_at(p, t, out, size, alpha);
		return;
	}
	const float T0 = 3.0f, T1 = 7.0f;            // ftol(0.3 * 12), ftol(0.6 * 12)
	float travel, sink, f;
	if (t < T0) {
		f = t / T0;
		travel = 0.3f * p->speed * f;
		sink = 0.0f;
		*alpha = 32.0f * f;
		*size = 0.2f * f;
	} else if (t < T1) {
		f = (t - T0) / T0;
		travel = 0.3f * p->speed * (1.0f + f);
		sink = (t - T0) * (100.0f / 60.0f);
		*alpha = 32.0f;
		*size = 0.2f * (1.0f + f);
	} else {
		f = (t - T1) / T1;
		travel = 0.3f * p->speed * (1.0f + (T1 - T0) / T0) + 0.5f * p->speed * f;
		sink = (t - T0) * (100.0f / 60.0f);
		*alpha = 32.0f * (1.0f - f);
		*size = 0.4f * (1.0f + f);
	}
	for (int k = 0; k < 3; k++)
		out[k] = p->pos[k] + p->dir[k] * travel;
	out[1] += sink;                              // +y is down
}

void mist_update(float dt)
{
	for (int i = 0; i < MAX_MIST; i++) {
		Puff* p = &s_puff[i];
		if (!p->on)
			continue;
		p->t += dt * 60.0f;
		if (p->t >= (p->acid ? 22.0f : 14.0f))
			p->on = false;
	}
}

void mist_draw(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	if (!s_bind || !s_tex_ok || !s_verts[0] || !s_verts[1])
		return;
	static float ring[SEGS + 1][2];
	if (ring[1][0] == 0.0f)
		for (int k = 0; k <= SEGS; k++) {
			float a = k * (6.2831853f / SEGS);
			ring[k][0] = sinf(a), ring[k][1] = cosf(a);
		}
	s_buf ^= 1;
	V* base = s_verts[s_buf];
	V* v = base;
	float rx = view->r[0].x, ry = view->r[0].y, rz = view->r[0].z;
	float ux = view->r[1].x, uy = view->r[1].y, uz = view->r[1].z;
	for (int i = 0; i < MAX_MIST; i++) {
		const Puff* p = &s_puff[i];
		if (!p->on)
			continue;
		if (p->t <= 0.0f)
			continue;
		float c[3], size, alpha;
		puff_at(p, p->t, c, &size, &alpha);
		float r = p->speed * size * 0.25f;
		int a = p->acid ? (int)(alpha * 2.0f) : (int)(255.0f * alpha / 64.0f);   // 0x4a3d00: alpha << 25
		if (r < 1.0f || a <= 0)
			continue;
		// the acid's colour: the PC's grey (0x80 lit, 0x18 in the dark) scales a bile green, the
		// port's choice: the PC modulates the poison texture's own tan with it, a yellowish haze, and
		// the pack keeps only that texture's alpha (pack/effect/mist.t3x)
		static const u8 BILE[3] = { 150, 172, 46 };
		u8 cr = p->acid ? (u8)(BILE[0] * p->grey / 0x80) : s_rgb[0];
		u8 cg = p->acid ? (u8)(BILE[1] * p->grey / 0x80) : s_rgb[1];
		u8 cb = p->acid ? (u8)(BILE[2] * p->grey / 0x80) : s_rgb[2];
		float uvr = p->acid ? 0.25f : 0.125f;
		V mid = { c[0], c[1], c[2], p->u, p->v, { cr, cg, cb, (u8)(a > 255 ? 255 : a) }, { 0, -127, 0, 0 } };
		V rim[SEGS + 1];
		for (int k = 0; k <= SEGS; k++) {
			float cx = ring[k][0] * r, cy = ring[k][1] * r;
			rim[k] = (V){ c[0] + rx * cx + ux * cy, c[1] + ry * cx + uy * cy, c[2] + rz * cx + uz * cy,
			              p->u + ring[k][0] * uvr, p->v + ring[k][1] * uvr,
			              { cr, cg, cb, 0 }, { 0, -127, 0, 0 } };
		}
		for (int k = 0; k < SEGS; k++) {
			v[0] = mid, v[1] = rim[k], v[2] = rim[k + 1];
			v += 3;
		}
	}
	int n = (int)(v - base);
	if (n <= 0)
		return;
	GSPGPU_FlushDataCache(base, (u32)((u8*)v - (u8*)base));
	s_bind(proj, view);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_TexBind(0, &s_tex);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, base, sizeof(V), 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, n);
}
