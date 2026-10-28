// The outdoor water surfaces (water.h): the PC's Effect2 water classes 42 (the lake) and 43 (the
// cemetery lake), their wave code and their draw, at the PC's 30 updates a second.
#include "water.h"
#include "gpucost.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;   // the world program's vertex

// Wave_Element (0x54 bytes; PS2 Effect2/hh_class_water_common.c): the fields the PC water reads
typedef struct {
	bool on;                     // Enable
	u32 ix, iz;                  // Impact_Grid_Index: the grid point it spreads from
	float maxd, lost, t;         // Max_Distance0, Lost_Time (s), Timer (s)
	float amp, omega, vel;       // Arrival (the height), Omega (rad/s), Verocity (units/s)
} Wave;

#define MAX_WAVE 20              // Wave_Info[20]: the classes pass 0x14 to the common code
#define MAX_NX 52
#define MAX_NZ 35
#define DT (1.0f / 30.0f)        // 0x6bc164: the classes' frame
#define DEG 0.0174532924f        // 0x6bc190

typedef struct {
	int cls;                     // the PC class: 42 lake, 43 cemetery
	int nx, nz;                  // WaveArea_Infomeation Grid_Index
	float cell;                  // Grid_WH[3], also the draw's cell (0x6a91c8 400, 0x69f3dc 300)
	float base[4];               // the shading's base RGBA
	float mult[3];               // times the flashlight colour
	bool blend;                  // the class's render states leave alpha blending on
} WaterDef;

// class 42: area 0x896778 (35 x 35 cells of 400), base 0x8968ec, light scale 0x89690c, states
// 0x4d4170 (alpha blending ends off); class 43: area 0x8966c0 (52 x 34 of 300 from -16800, 3500,
// 2100), base 0x89673c with its alpha set from 0x89674c in 0x4d2fe0, light scale 0x896750,
// states 0x4d2bd0 (blended, SRCALPHA / INVSRCALPHA). Both: no culling, alpha test > 0, Z test
// without Z writes, no texture, D3D fog off.
static const WaterDef LAKE = { 42, 35, 35, 400.0f, { 24, 24, 24, 70 }, { 16, 16, 16 }, false };
static const WaterDef CEMETERY = { 43, 52, 34, 300.0f, { 50, 50, 50, 40 }, { 18, 18, 18 }, true };

// the lake's three waves (0x8967f0, omega in degrees until posted)
static const Wave LAKE_WAVE[3] = {
	{ true, 0, 0, 28800.0f, 60.0f, 0.0f, 20.0f, 200.0f, 800.0f },
	{ true, 0, 0, 28800.0f, 60.0f, 0.0f, 12.0f, 400.0f, 1800.0f },
	{ true, 0, 0, 28800.0f, 60.0f, 0.0f, 22.0f, 300.0f, 1000.0f },
};

static WaterBindFn s_bind;
static V* s_verts[2];
static int s_buf;
static bool s_built;                 // s_verts[s_buf] holds this instance's grid
static char s_status[64] = "water off";

static struct {
	const WaterDef* def;         // NULL: the room has no water class
	int room;
	int step;                    // the instance's Step: 0 set up, 1 running, else gone
	float timer, last_post;      // Timer, Area_WavePostTime
	bool show;                   // Area_Enable_Table[0] this update
	float origin[3];             // the grid's corner (World_Location plus the class's offset)
	float saved[2];              // the lake's x and z the grid follows (0x96634c / 0x966354)
	Wave w[MAX_WAVE];
	float h[MAX_NX * MAX_NZ];    // pGrid_Y_Value
	float acc;
	u32 seed;
	int ticks, waves;
} S = { .room = -1, .seed = 1 };

// FUN_004483b0: the PC's random numbers (its seed 0x7b5690 is shared; this one is the water's own)
static u32 rnd(void)
{
	S.seed = (S.seed * 0xc1c64e6du + 0x3039u) & 0x7fffffffu;
	return S.seed;
}

// WaveElement_Addition (0x4d1500): the first free slot, else the oldest (0x4d1400, the highest
// Timer)
static void wave_add(const Wave* w)
{
	Wave* slot = NULL;
	for (int i = 0; i < MAX_WAVE && !slot; i++)
		if (!S.w[i].on)
			slot = &S.w[i];
	if (!slot) {
		float best = 0.0f;
		for (int i = 0; i < MAX_WAVE; i++)
			if (S.w[i].on && best < S.w[i].t)
				best = S.w[i].t, slot = &S.w[i];
	}
	if (slot)
		*slot = *w;
}

// HH_Class_WaterCommon_WaveElement_Time_Count (0x4d1620)
static void wave_time(void)
{
	for (int i = 0; i < MAX_WAVE; i++) {
		Wave* w = &S.w[i];
		if (w->on) {
			w->t += DT;
			if (w->lost < w->t)
				w->on = false;
		}
	}
}

// WaveArea_Calculator (0x4d1120): every wave adds sin((d / v + t) w) x height, falling off to 0 at
// the nearer of its front (v t) and its shrinking reach (Max_Distance0 x (1 - t / Lost_Time)).
static void wave_heights(const WaterDef* d)
{
	float dx2[MAX_NX];
	float c2 = d->cell * d->cell;
	S.waves = 0;
	for (int i = 0; i < MAX_WAVE; i++) {
		const Wave* w = &S.w[i];
		if (!w->on)
			continue;
		S.waves++;
		float reach = 1.0f - w->t / w->lost;
		reach = (reach < 0.0f ? 0.0f : reach) * w->maxd;
		float front = w->vel * w->t;
		float r = reach < front ? reach : front;
		float inv = r != 0.0f ? 1.0f / r : 0.0f;
		for (int x = 0; x < d->nx; x++) {
			int k = (int)w->ix - x;
			dx2[x] = (float)(k * k) * c2;
		}
		for (int z = 0; z < d->nz; z++) {
			int k = (int)w->iz - z;
			float dz2 = (float)(k * k) * c2;
			float* row = &S.h[z * d->nx];
			for (int x = 0; x < d->nx; x++) {
				float dist = sqrtf(dx2[x] + dz2);
				if (dist <= reach && dist <= front) {
					float fall = inv == 0.0f ? 0.0f : 1.0f - dist * inv;
					row[x] += sinf((dist / w->vel + w->t) * w->omega) * w->amp * fall;
				}
			}
		}
	}
}

// FUN_004d44a0: a lake wave from one of the three at a grid point
static void lake_post_at(u32 ix, u32 iz)
{
	Wave w = LAKE_WAVE[rnd() % 3];
	w.omega *= DEG;
	w.ix = ix, w.iz = iz;
	wave_add(&w);
	S.last_post = S.timer;
}

// class 42 (0x4d5210), one update
static void lake_tick(const WaterInput* in)
{
	const WaterDef* d = &LAKE;
	if (S.step == 0) {
		// 0x4d43f0, then gone for good once flag 621 is set
		memset(S.w, 0, sizeof S.w);
		S.last_post = 0.0f;
		if (in->flag621) {
			S.step = 2;
		} else {
			S.step = 1;
			S.timer = 20.0f;
		}
		S.saved[0] = in->player[0], S.saved[1] = in->player[2];
	}
	if (S.step != 1) {
		S.show = false;
		return;
	}
	memset(S.h, 0, sizeof S.h);
	S.show = true;
	if (-12100.0f < in->player[2])         // 0x6bc1a8: off the lake towards the town
		S.show = false;
	// every 20 s (0x966344) three of four waves from random points beyond the far corner
	if (S.show && 20.0f < S.timer - S.last_post) {
		u32 skip = rnd();
		u32 at[8];
		for (int i = 0; i < 8; i++)
			at[i] = rnd() % (u32)d->nz + (u32)d->nz;
		for (u32 a = 0; a < 4; a++) {
			u32 pick = rnd();
			if ((skip & 3) != a) {
				Wave w = LAKE_WAVE[pick % 3];
				w.omega *= DEG;
				w.ix = at[a * 2], w.iz = at[a * 2 + 1];
				wave_add(&w);
				S.last_post = S.timer;
			}
		}
	}
	float px = in->player[0], pz = in->player[2], py;
	if (!in->scene) {
		// the grid follows James in steps of a cell (0x6bc1a0 is the double 400) and its waves
		// move back a point; one pushed off the grid comes back in at the far side as a new wave
		int sx = 0, sz = 0;
		float dx = S.saved[0] - px, dz = S.saved[1] - pz;
		if (fabsf(dx) < 400.0f)
			px = S.saved[0];
		else
			sx = dx <= 0.0f ? -1 : 1, S.saved[0] = px;
		if (fabsf(dz) < 400.0f)
			pz = S.saved[1];
		else
			sz = dz <= 0.0f ? -1 : 1, S.saved[1] = pz;
		if (sx)
			for (int i = 0; i < MAX_WAVE; i++) {
				Wave* w = &S.w[i];
				u32 u = w->ix + (u32)sx;
				if (!w->on)
					continue;
				if (u < 35u) {
					w->ix = u % 35u;
				} else {
					w->on = false;
					lake_post_at((u + 35u) % 35u, w->iz);
				}
			}
		if (sz)
			for (int i = 0; i < MAX_WAVE; i++) {
				Wave* w = &S.w[i];
				u32 u = w->iz + (u32)sz;
				if (!w->on)
					continue;
				if (u < 35u) {
					w->iz = u % 35u;
				} else {
					w->on = false;
					lake_post_at(w->ix, (u + 35u) % 35u);
				}
			}
		py = 50.0f + 3500.0f;            // 0x89691c + 0x6bb33c
	} else {
		py = 100.0f + 3500.0f;           // 0x896928 + 0x6bb33c
		if (in->flag379)                 // 0x1f7d60c & 0x8000000: in the boat, a fixed grid
			px = -8520.0f, pz = -20800.0f;   // 0x896924, 0x896920
	}
	S.origin[0] = px - 7000.0f;          // 0x6bc19c: half the grid
	S.origin[1] = py;
	S.origin[2] = pz - 7000.0f;
	if (S.show)
		wave_heights(d);
	wave_time();
	S.timer += DT;
}

// class 43 (0x4d3c20), one update
static void cemetery_tick(const WaterInput* in)
{
	const WaterDef* d = &CEMETERY;
	if (S.step == 0) {
		// 0x4d2e60
		memset(S.w, 0, sizeof S.w);
		S.last_post = 0.0f;
		S.step = 1;
		S.timer = 10.0f;
		S.show = false;
		return;
	}
	if (S.step != 1) {
		S.show = false;
		return;
	}
	memset(S.h, 0, sizeof S.h);
	S.show = true;
	if (in->scene)
		S.show = in->flag618;
	if (!in->flag618) {
		// James away from the lake (0x6388d8, 0x6bc198); flag 618 keeps it for Angela's scene
		if (in->player[2] < -4000.0f)
			S.show = false;
		if (in->player[0] < -20200.0f)
			S.show = false;
	}
	if (S.show) {
		// every 10 s (0x96632c) one wave from the middle of the far edge (0x896760..0x896770)
		if (10.0f < S.timer - S.last_post) {
			for (int i = 0; i < 4; i++)
				rnd();
			Wave w = { true, (u32)d->nx >> 1, (u32)d->nz - 1, 28800.0f, 40.0f, 0.0f, 200.0f, 200.0f * DEG, 500.0f };
			wave_add(&w);
			S.last_post = S.timer;
		}
		S.origin[0] = -16800.0f, S.origin[1] = 3500.0f, S.origin[2] = 2100.0f;   // 0x8966c0
		wave_heights(d);
	}
	wave_time();
	S.timer += DT;
}

static void build(const WaterDef* d, const float light[3]);

bool water_init(WaterBindFn bind)
{
	s_bind = bind;
	int n = (MAX_NZ - 1) * MAX_NX * 2;
	for (int i = 0; i < 2; i++)
		if (!s_verts[i])
			s_verts[i] = linearAlloc(n * sizeof(V));
	return s_verts[0] && s_verts[1];
}

void water_exit(void)
{
	for (int i = 0; i < 2; i++) {
		if (s_verts[i])
			linearFree(s_verts[i]);
		s_verts[i] = NULL;
	}
}

void water_reset(void)
{
	s_built = false;
	S.def = NULL;
	S.room = -1;
	S.show = false;
}

void water_update(const WaterInput* in, float dt)
{
	if (in->room != S.room) {
		// HH_Effect_Object_AutoPost (0x4ca6d0): the room's classes from 0x8942c8. Room 0xe also
		// gets classes 22, 23 (the boat mask) and 56, room 3 class 55: not ported.
		S.room = in->room;
		S.def = in->room == 0xe ? &LAKE : in->room == 3 ? &CEMETERY : NULL;
		S.step = 0;
		S.show = false;
		s_built = false;
		S.acc = DT;                      // the class runs on the frame it is posted
	}
	if (!S.def)
		return;
	S.acc += dt;
	int n = 0;
	while (S.acc >= DT && n < 4) {
		S.acc -= DT;
		n++;
		S.ticks++;
		if (S.def == &LAKE)
			lake_tick(in);
		else
			cemetery_tick(in);
	}
	// the Old 3DS profile lays out the vertices on every other update (15 a second)
	if (n && S.show && (!in->low || !s_built || (S.ticks & 1) == 0))
		build(S.def, in->light);
	if (S.acc > DT)
		S.acc = DT;
}

bool water_active(void)
{
	return S.def && S.show;
}

// Object_Draw (lake 0x4d45e0, cemetery 0x4d2fe0), the vertices: row by row, each vertex lit by the
// facet at its grid point (towards the next column and the next row; the last column takes the
// first one's height, as the PC wraps the index), a row's far vertices keeping the near row's
// light. Built once per update (the heights only change there) into the buffer the GPU is not
// reading; the fade to grey is left to the GPU (water_draw).
static void build(const WaterDef* d, const float light[3])
{
	V* out = s_verts[s_buf ^ 1];
	int nx = d->nx, nz = d->nz;
	float cell = d->cell;
	// the light direction is fixed (0x966340 is never set), turned to point at the light
	static const float L[3] = { -0.776822507f, -0.28888759f, -0.559206009f };
	float lc[3] = { light[0] * d->mult[0], light[1] * d->mult[1], light[2] * d->mult[2] };
	static u8 col[MAX_NX * MAX_NZ][4];
	u8 shade[MAX_NX][4];                 // the light found at the row before, per column
	for (int z = 0; z < nz; z++) {
		const float* hz = &S.h[z * nx];
		for (int x = 0; x < nx; x++) {
			u8* c = col[z * nx + x];
			if (z == nz - 1) {
				memcpy(c, shade[x], 4);
				continue;
			}
			// the facet's normal at (x, z): from here to the next column and to the next row
			const float* hz1 = &S.h[(z + 1) * nx];
			int x1 = x + 1 < nx ? x + 1 : 0;
			float a0 = (float)(x1 - x) * cell, a1 = hz[x1] - hz[x];
			float b1 = hz1[x] - hz[x], b2 = cell;
			float la = 1.0f / sqrtf(a0 * a0 + a1 * a1), lb = 1.0f / sqrtf(b1 * b1 + b2 * b2);
			a0 *= la, a1 *= la, b1 *= lb, b2 *= lb;
			// a = (a0, a1, 0), b = (0, b1, b2): their cross product
			float n0 = a1 * b2, n1 = -a0 * b2, n2 = a0 * b1;
			float k = L[0] * n0 + L[1] * n1 + L[2] * n2;
			k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
			u8 lit[4];
			for (int i = 0; i < 3; i++) {
				float v = d->base[i];
				if (!(k >= -5e-6f && k <= 5e-6f))      // 0x4d3eb0 (0x69db40, 0x69db3c): base only
					v += lc[i] * k;
				lit[i] = (u8)(int)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
			}
			lit[3] = (u8)(int)d->base[3];
			// row 0 takes its own light, a later row the one found at the row before
			memcpy(c, z == 0 ? lit : shade[x], 4);
			memcpy(shade[x], lit, 4);
		}
	}
	// one strip per row: (x, z), (x, z + 1), ...
	V* v = out;
	for (int z = 0; z < nz - 1; z++)
		for (int x = 0; x < nx; x++)
			for (int j = 0; j < 2; j++) {
				int zz = z + j;
				const u8* c = col[zz * nx + x];
				*v++ = (V){ S.origin[0] + (float)x * cell, S.origin[1] + S.h[zz * nx + x], S.origin[2] + (float)zz * cell,
				            0.0f, 0.0f, { c[0], c[1], c[2], c[3] }, { 0, 0, 0, 0 } };
			}
	GSPGPU_FlushDataCache(out, (u32)((u8*)v - (u8*)out));
	s_buf ^= 1;
	s_built = true;
}

// The PC fades each vertex to grey 70 by its depth between the fog's start and end (D3D fog off);
// here the GPU's fog does the same per pixel from a table of that line.
static C3D_FogLut s_lut;
static float s_lut_key[4] = { -1.0f };

static void fog_lut(float fog_start, float fog_end, float near, float far)
{
	if (s_lut_key[0] == fog_start && s_lut_key[1] == fog_end && s_lut_key[2] == near && s_lut_key[3] == far)
		return;
	s_lut_key[0] = fog_start, s_lut_key[1] = fog_end, s_lut_key[2] = near, s_lut_key[3] = far;
	float k = fog_end - fog_start != 0.0f ? 1.0f / (fog_end - fog_start) : 1.0f;
	float data[256];
	for (int i = 0; i <= 128; i++) {
		float w = FogLut_CalcZ(i / 128.0f, near, far);
		float t = w < fog_start ? 0.0f : w >= fog_end ? 1.0f : (w - fog_start) * k;
		float v = 1.0f - t;              // the share of the water's own colour
		if (i < 128)
			data[i] = v;
		if (i > 0)
			data[i + 127] = v - data[i - 1];
	}
	FogLut_FromArray(&s_lut, data);
}

void water_draw(const C3D_Mtx* proj, const C3D_Mtx* view, float fog_start, float fog_end, float near, float far)
{
	const WaterDef* d = S.def;
	if (!d || !S.show || !s_built || !s_bind || !s_verts[0] || !s_verts[1])
		return;
	fog_lut(fog_start, fog_end, near, far);
	s_bind(proj, view);
	C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
	C3D_FogColor(70u << 16 | 70u << 8 | 70u);
	C3D_FogLutBind(&s_lut);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);   // D3DRS_ZWRITEENABLE 0
	C3D_AlphaTest(true, GPU_GREATER, 0);                 // ALPHAFUNC GREATER, ALPHAREF 0
	C3D_CullFace(GPU_CULL_NONE);
	if (d->blend)
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
		               GPU_ONE_MINUS_SRC_ALPHA);
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_TexEnv* env = C3D_GetTexEnv(0);                  // stage 0 SELECTARG1 DIFFUSE, no texture
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_verts[s_buf], sizeof(V), 4, 0x3210);
	for (int z = 0; z < d->nz - 1; z++)
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, z * d->nx * 2, d->nx * 2);
	if (gpucost_on()) {                  // debug.txt gpucost (gpucost.h)
		C3D_Mtx pv;
		Mtx_Multiply(&pv, proj, view);
		const V* v = s_verts[s_buf];
		for (int z = 0; z < d->nz - 1; z++)
			for (int i = 0; i + 2 < d->nx * 2; i++) {
				const V* t = &v[z * d->nx * 2 + i];
				gpucost_tri(GC_WATER, &pv, &t[0].x, &t[1].x, &t[2].x, false);
			}
	}
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

const char* water_status(void)
{
	if (!S.def)
		return "water off";
	snprintf(s_status, sizeof s_status, "water %d %s w%d t%.0f", S.def->cls, S.show ? "on" : "hid", S.waves,
	         S.timer);
	return s_status;
}
