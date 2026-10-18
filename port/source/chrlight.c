#include "chrlight.h"
#include "textcache.h"
#include "profile.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PC references (Director's Cut): light list 0xaac8f8 (12 x 0x98, PS2 LightWork.lights), setters
// 0x50cfa0 parallel, 0x50cff0 point, 0x50d0a0 fake point, 0x50d150 fake spot, 0x50d310 spot,
// scene setup 0x508a90, per-character fakes 0x5091c0, influence 0x50c3d0, extras 0x50ce10,
// constants 0x4fe4a0 (c47..c58 parallels, c63..c72 points, c73..c88 spots), material 0x501190
// (c43 diffuse, c44 ambient), ambient 0x50d950, shader 0x8a2ed8.
//
// What the port leaves out: per-primitive material colours (90% of the PC's character materials
// are diffuse 0.7 / ambient 0.3, which the port uses for all), James lighting himself with his own
// flashlight (0x508060), the reflection light and the shadow lights, event and scene lights
// (cutscenes have demo.c's own), and the light overrides (0x47adf0).

#define NSLOT 12
#define CL_LIGHTS 7                  // fragment lights 1..7
#define CL_PAR 3                     // lights 1..3: parallels
#define CL_EXTRA 4                   // lights 4..7: points and spots, the flashlight in the last

enum { K_NONE, K_PARALLEL, K_POINT, K_SPOT };
enum { F_NONE, F_POINT, F_SPOT };

typedef struct {
	int kind, fake;
	float pos[3], dir[3], col[3];
	float start, end;                // falloff
	float f_a, f_b, f_ra, f_rb;      // UpdateFParam: linear in d and in 1/d
	float s_a, s_b;                  // spread
	float cone;                      // fake spot: cos of the edge
	C3D_LightLutDA da;
	C3D_LightLut sp;
	bool luts;
} Slot;

static struct {
	C3D_LightEnv* env;
	C3D_Light light[CL_LIGHTS];
	Slot s[NSLOT];
	Slot fl;                         // the flashlight as a character spot (dark rooms)
	bool active, james_light;
	C3D_Mtx view;
	float amb[3], bright;
	ChrFlash flash;
	int used;
	char status[48];
} L;

static void falloff(Slot* s, float start, float end)
{
	// 0x50cff0: f_a / f_b fall linearly from 1 at start to 0 at end, f_ra / f_rb in 1/d
	s->start = start, s->end = end;
	float k = (start - end) != 0.0f ? 1.0f / (start - end) : 0.0f;
	s->f_a = k, s->f_b = -(k * end);
	s->f_rb = k * start, s->f_ra = -(k * start * end);
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static float da_func(float d, float ra, float rb)
{
	return d <= 1.0f ? clamp01(ra + rb) : clamp01(ra / d + rb);
}

// spot LUT: clamp(cos * s_a + s_b) over -1..1 (LightLut_FromFunc passes one parameter)
static float s_lut_sb;
static float spot_func(float x, float sa) { return clamp01(x * sa + s_lut_sb); }

static void spot_lut(C3D_LightLut* lut, float sa, float sb)
{
	s_lut_sb = sb;
	LightLut_FromFunc(lut, spot_func, sa, true);
}

static void slot_luts(Slot* s)
{
	if (s->kind != K_POINT && s->kind != K_SPOT)
		return;
	float end = s->end > s->start + 1.0f ? s->end : s->start + 1.0f;
	LightLutDA_Create(&s->da, da_func, 0.0f, end, s->f_ra, s->f_rb);
	if (s->kind == K_SPOT)
		spot_lut(&s->sp, s->s_a, s->s_b);
	s->luts = true;
}

static float intensity(const float* c) { return fabsf(0.3f * c[0] + 0.6f * c[1] + 0.1f * c[2]); }

void chrlight_init(C3D_LightEnv* env)
{
	memset(&L, 0, sizeof L);
	L.env = env;
	for (int i = 0; i < CL_LIGHTS; i++) {
		C3D_LightInit(&L.light[i], env);
		C3D_LightEnable(&L.light[i], false);
	}
	// the map's flashlight has no diffuse colour, so a diffuse material only affects these lights
	static const C3D_Material mat = {
		{ 1.0f, 1.0f, 1.0f },   // ambient (the environment ambient carries the PC's product)
		{ 0.7f, 0.7f, 0.7f },   // diffuse: the usual character material (c43)
		{ 1.0f, 1.0f, 1.0f },   // specular0 (the map's flashlight)
		{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
	};
	C3D_LightEnvMaterial(env, &mat);
	L.fl.kind = K_SPOT;
}

void chrlight_load(const char* pack_dir, const char* map, const char* variant)
{
	memset(L.s, 0, sizeof L.s);
	char path[160];
	snprintf(path, sizeof path, "%s/chrlight.txt", pack_dir);
	const char* text = text_cached(path);   // read once: this runs on every tile change
	if (!text)
		return;
	static char line[4096], best[4096];
	best[0] = 0;
	size_t maplen = strlen(map);
	while (text_next_line(&text, line, sizeof line)) {
		char name[16], var[16];
		if (line[0] == '#' || strncmp(line, map, maplen) || line[maplen] != ' ' ||
		    sscanf(line, "%15s %15s", name, var) != 2 || strcmp(name, map))
			continue;
		if (!strcmp(var, variant)) {
			strcpy(best, line);
			break;
		}
		if (!strcmp(var, "-"))
			strcpy(best, line);
	}
	if (!best[0])
		return;
	char* p = best;
	for (int i = 0; i < 3; i++) {           // skip name, variant, glb
		while (*p && *p != ' ') p++;
		while (*p == ' ') p++;
	}
	float v[512];
	int n = 0;
	while (n < 512) {
		char* e;
		v[n] = strtof(p, &e);
		if (e == p)
			break;
		p = e;
		n++;
	}
	if (n < 22)
		return;
	const float* q = v;
	for (int i = 1; i <= 3; i++, q += 6) {  // 0x50cfa0
		Slot* s = &L.s[i];
		if (q[3] == 0.0f && q[4] == 0.0f && q[5] == 0.0f)
			continue;
		s->kind = K_PARALLEL;
		memcpy(s->dir, q, 3 * sizeof(float));
		memcpy(s->col, q + 3, 3 * sizeof(float));
	}
	int ns = (int)q[0], np = (int)q[1], nfp = (int)q[2], nfs = (int)q[3];
	q += 4;
	int at = 5, fake_at = 2;
	for (int i = 0; i < ns && q + 14 <= v + n; i++, q += 14) {   // 0x50d310, from index 5
		if (at >= NSLOT)
			break;
		Slot* s = &L.s[at++];
		s->kind = K_SPOT;
		memcpy(s->pos, q, 3 * sizeof(float));
		memcpy(s->dir, q + 3, 3 * sizeof(float));
		memcpy(s->col, q + 6, 3 * sizeof(float));
		const float* sv = q + 10;
		falloff(s, sv[2], sv[3]);
		s->s_a = 1.0f, s->s_b = sv[0] * -0.8f;
	}
	for (int i = 0; i < np + nfp && q + 9 <= v + n; i++, q += 9) {
		bool fake = i >= np;
		int idx = fake ? fake_at++ : at++;           // fake points overwrite from index 2 (0x508f8d)
		if (idx >= NSLOT)
			continue;
		Slot* s = &L.s[idx];
		memset(s, 0, sizeof *s);
		s->kind = fake ? K_PARALLEL : K_POINT;
		s->fake = fake ? F_POINT : F_NONE;
		memcpy(s->pos, q, 3 * sizeof(float));
		memcpy(s->col, q + 3, 3 * sizeof(float));
		falloff(s, q[7], q[8]);
	}
	for (int i = 0; i < nfs && q + 12 <= v + n; i++, q += 12) {   // 0x50d150, after the fake points
		int idx = fake_at++;
		if (idx >= NSLOT)
			continue;
		Slot* s = &L.s[idx];
		memset(s, 0, sizeof *s);
		s->kind = K_PARALLEL;
		s->fake = F_SPOT;
		memcpy(s->pos, q, 3 * sizeof(float));
		memcpy(s->dir, q + 3, 3 * sizeof(float));
		memcpy(s->col, q + 6, 3 * sizeof(float));
		falloff(s, q[10], q[11]);
		float c = q[9];
		float k = (1.0f - c) != 0.0f ? 1.0f / (1.0f - c) : 0.0f;
		s->s_a = k, s->s_b = -(k * c);
	}
	for (int i = 0; i < NSLOT; i++)
		slot_luts(&L.s[i]);
}

void chrlight_begin(const C3D_Mtx* view, const float ambient[3], float level, float bright, const ChrFlash* fl)
{
	L.active = true;
	L.view = *view;
	L.bright = bright;
	float lv = level > 1.0f ? 1.0f : level;
	for (int i = 0; i < 3; i++)        // 0x50d950 x material ambient 0.3 (0x501190)
		L.amb[i] = (2.1f * ambient[i] + 0.3f * lv) * 0.3f * bright;
	L.flash = *fl;
	if (fl->on) {
		Slot* s = &L.fl;
		memcpy(s->pos, fl->pos, sizeof s->pos);
		memcpy(s->dir, fl->dir, sizeof s->dir);
		s->col[0] = s->col[1] = s->col[2] = 7.0f;    // SpotCol0, saturates after x0.7
		if (s->start != fl->start || s->end != fl->end) {
			falloff(s, fl->start, fl->end);
			s->s_a = 1.0f / (1.0f - cosf(fl->cone));
			s->s_b = -cosf(fl->cone) * s->s_a;
			slot_luts(s);
		}
	}
}

static void set_parallel(C3D_Light* g, const float* dir, const float* col)
{
	// the light travels along dir; a PICA directional light gives the direction towards the light
	C3D_FVec d = Mtx_MultiplyFVec4(&L.view, FVec4_New(-dir[0], -dir[1], -dir[2], 0.0f));
	C3D_LightPosition(g, &d);
	C3D_LightColor(g, col[0] * L.bright, col[1] * L.bright, col[2] * L.bright);
	C3D_LightSpotEnable(g, false);
	C3D_LightDistAttnEnable(g, false);
	C3D_LightEnable(g, true);
}

static void set_extra(C3D_Light* g, Slot* s, float k)
{
	C3D_FVec p = Mtx_MultiplyFVec4(&L.view, FVec4_New(s->pos[0], s->pos[1], s->pos[2], 1.0f));
	C3D_LightPosition(g, &p);
	C3D_LightColor(g, s->col[0] * k * L.bright, s->col[1] * k * L.bright, s->col[2] * k * L.bright);
	C3D_LightDistAttn(g, &s->da);
	C3D_LightDistAttnEnable(g, true);
	bool spot = s->kind == K_SPOT;
	C3D_LightSpotEnable(g, spot);
	if (spot) {
		C3D_FVec d = Mtx_MultiplyFVec4(&L.view, FVec4_New(s->dir[0], s->dir[1], s->dir[2], 0.0f));
		C3D_LightSpotDir(g, d.x, d.y, d.z);
		C3D_LightSpotLut(g, &s->sp);
	}
	C3D_LightEnable(g, true);
}

static void norm(float* v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0.0f)
		v[0] /= l, v[1] /= l, v[2] /= l;
}

// 0x5091c0: a fake light becomes a parallel light from its position towards the character
static bool fake_light(const Slot* s, const float* c, float* dir, float* col)
{
	float d[3] = { c[0] - s->pos[0], c[1] - s->pos[1], c[2] - s->pos[2] };
	float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	float k = clamp01(dist * s->f_a + s->f_b);
	bool vertical_dropped = false;
	memcpy(dir, d, sizeof d);
	if (s->fake == F_SPOT) {
		// the PC flattens the direction onto the dominant horizontal plane
		if (d[1] * d[1] <= d[0] * d[0] || d[1] * d[1] <= d[2] * d[2]) {
			dir[1] = 0.0f;
		} else {
			dir[2] = 0.0f;
			vertical_dropped = true;
		}
		norm(dir);
		float sd[3] = { s->dir[0], vertical_dropped ? s->dir[1] : 0.0f, vertical_dropped ? 0.0f : s->dir[2] };
		norm(sd);
		k *= clamp01((sd[0] * dir[0] + sd[1] * dir[1] + sd[2] * dir[2]) * s->s_a + s->s_b);
	} else {
		norm(dir);
	}
	if (k < 1e-5f)
		return false;
	col[0] = s->col[0] * k, col[1] = s->col[1] * k, col[2] = s->col[2] * k;
	return true;
}

typedef struct { Slot* s; float influence; } Pick;

static int by_influence(const void* a, const void* b)
{
	float x = ((const Pick*)a)->influence, y = ((const Pick*)b)->influence;
	return x < y ? 1 : x > y ? -1 : 0;
}

void chrlight_at(const float pos[3], bool james)
{
	if (!L.active)
		return;
	C3D_LightEnvAmbient(L.env, L.amb[0], L.amb[1], L.amb[2]);
	int g = 0;
	// parallels (UpdateParallels, at most three here): record lights 1..3 and the fakes
	for (int i = 1; i < NSLOT && g < CL_PAR; i++) {
		Slot* s = &L.s[i];
		if (s->kind != K_PARALLEL)
			continue;
		if (s->fake) {
			float dir[3], col[3];
			if (fake_light(s, pos, dir, col))
				set_parallel(&L.light[g++], dir, col);
		} else {
			set_parallel(&L.light[g++], s->dir, s->col);
		}
	}
	for (int i = g; i < CL_PAR; i++)
		C3D_LightEnable(&L.light[i], false);
	// points and spots by influence (0x50c3d0 with the character radius 800, 0x50ce10)
	Pick pick[NSLOT];
	int n = 0;
	for (int i = 0; i < NSLOT; i++) {
		Slot* s = &L.s[i];
		if (s->kind != K_POINT && s->kind != K_SPOT)
			continue;
		float d[3] = { pos[0] - s->pos[0], pos[1] - s->pos[1], pos[2] - s->pos[2] };
		float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		float f = clamp01((dist - 800.0f) * s->f_a + s->f_b);
		pick[n].s = s;
		pick[n].influence = f * intensity(s->col);
		n++;
	}
	qsort(pick, n, sizeof pick[0], by_influence);
	int room = CL_EXTRA - (L.flash.on && !james ? 1 : 0);
	if (room > profile_chr_point_lights())   // fewer on the low quality level (profile.c)
		room = profile_chr_point_lights();
	int e = 0;
	for (int i = 0; i < n && e < room; i++)
		if (pick[i].influence > 0.0f)
			set_extra(&L.light[CL_PAR + e++], pick[i].s, 1.0f);
	if (L.flash.on && !james)
		set_extra(&L.light[CL_PAR + e++], &L.fl, 1.0f);
	for (int i = CL_PAR + e; i < CL_LIGHTS; i++)
		C3D_LightEnable(&L.light[i], false);
	L.used = g + e;
	snprintf(L.status, sizeof L.status, "chr lights %d par %d extra", g, e);
}

void chrlight_ambient_scale(float k)
{
	if (L.active)
		C3D_LightEnvAmbient(L.env, L.amb[0] * k, L.amb[1] * k, L.amb[2] * k);
}

void chrlight_end(void)
{
	if (!L.active)
		return;
	for (int i = 0; i < CL_LIGHTS; i++)
		C3D_LightEnable(&L.light[i], false);
	C3D_LightEnvAmbient(L.env, 0.0f, 0.0f, 0.0f);
	L.active = false;
}

const char* chrlight_status(void) { return L.status[0] ? L.status : "chr lights -"; }
