#include "noise.h"
#include "stereo.h"
#include <math.h>
#include <string.h>

#define SIZE 128

typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;   // main.c's 2D vertex (28 bytes)

static C3D_Tex s_tex[2];
static int s_cur;
static bool s_ready, s_on;
static V* s_quad;
static float* s_movie_quad;      // 4 corners of x, y, u, v for movie.c's program
static u32 s_color;              // TEV constant, 0xAABBGGRR
static u32 s_rand = 0x2545f491;
static u8 s_pair[16];            // four random bits -> two texels

// The PC's brightness levels as the gamma ramp's slope (0x47cb30: level 3 = 128, unchanged).
static const u8 s_ramp[8] = { 0x4b, 0x5a, 0x6e, 0x80, 0x8c, 0x9b, 0xb4, 0xdc };

bool noise_init(void)
{
	if (s_ready)
		return true;
	for (int i = 0; i < 2; i++) {
		if (!C3D_TexInit(&s_tex[i], SIZE, SIZE, GPU_L4)) {
			if (i)
				C3D_TexDelete(&s_tex[0]);
			return false;
		}
		C3D_TexSetFilter(&s_tex[i], GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&s_tex[i], GPU_REPEAT, GPU_REPEAT);
	}
	s_quad = linearAlloc(6 * sizeof(V));
	if (!s_quad) {
		C3D_TexDelete(&s_tex[0]);
		C3D_TexDelete(&s_tex[1]);
		return false;
	}
	// one texel per pixel over 400x240 (the PC tiles its 128 texels four times over 640 pixels)
	const float u = 400.0f / SIZE, v = 240.0f / SIZE;
	const V a = { 0, 0, 0.5f, 0, 0, { 255, 255, 255, 255 }, { 0, 0, 127, 0 } };
	const V b = { 400, 0, 0.5f, u, 0, { 255, 255, 255, 255 }, { 0, 0, 127, 0 } };
	const V c = { 400, 240, 0.5f, u, v, { 255, 255, 255, 255 }, { 0, 0, 127, 0 } };
	const V d = { 0, 240, 0.5f, 0, v, { 255, 255, 255, 255 }, { 0, 0, 127, 0 } };
	s_quad[0] = a, s_quad[1] = b, s_quad[2] = c, s_quad[3] = a, s_quad[4] = c, s_quad[5] = d;
	GSPGPU_FlushDataCache(s_quad, 6 * sizeof(V));
	// DXT1 with colour0 > colour1: index 0 colour0, 1 colour1 (black), 2 two thirds, 3 one third
	static const u8 level[4] = { 15, 0, 10, 5 };
	for (int i = 0; i < 16; i++)
		s_pair[i] = level[i & 3] | level[i >> 2] << 4;
	s_ready = true;
	noise_set(false, 3, false);
	return true;
}

void noise_exit(void)
{
	if (!s_ready)
		return;
	C3D_TexDelete(&s_tex[0]);
	C3D_TexDelete(&s_tex[1]);
	linearFree(s_quad);
	if (s_movie_quad)
		linearFree(s_movie_quad);
	s_movie_quad = NULL;
	s_ready = false;
}

void noise_set(bool on, int level, bool ps2)
{
	s_on = on;
	int k = s_ramp[level < 0 ? 0 : level > 7 ? 7 : level];
	// 0x47cb30: the grey is 350 - k up to 255; past that the alpha byte drops instead
	int v = 0x15e - k, grey, a2;
	if (v < 0xff) {
		grey = ps2 ? (int)lrintf(v / 7.4f) : v;      // the EE divides the byte by 7.4
		a2 = 0x60;
	} else {
		int t = 0x5f - k;
		grey = ps2 ? 0x22 : 0xff;
		a2 = 0x60 - t / 4;
	}
	// 0x4781e0: texture factor alpha 255 - ((base - 2 * a2) >> 2), base 0xff (the EE: 0x1d7)
	int e = a2 < 0x80 ? a2 * 2 : 0xff;
	int alpha = (0xff - (((ps2 ? 0x1d7 : 0xff) - e) >> 2)) & 0xff;
	// MODULATE2X of the grey and the texel: the texel's tint (0xde99 = 222, 210, 206) goes into
	// the constant here, the doubling into the TEV scale
	u32 r = grey * 222 / 255, g = grey * 210 / 255, b = grey * 206 / 255;
	s_color = (u32)alpha << 24 | b << 16 | g << 8 | r;
}

bool noise_on(void) { return s_on && s_ready; }

static inline u32 next(void)
{
	u32 x = s_rand;
	x ^= x << 13, x ^= x >> 17, x ^= x << 5;
	return s_rand = x;
}

// 0x478520 every frame: new random blocks (here: new random texels, the same four levels)
static void refill(C3D_Tex* t)
{
	u32* p = (u32*)t->data;
	for (int i = 0; i < SIZE * SIZE / 8; i += 2) {
		u32 r = next();
		p[i] = s_pair[r & 15] | s_pair[r >> 4 & 15] << 8 | s_pair[r >> 8 & 15] << 16 | (u32)s_pair[r >> 12 & 15] << 24;
		p[i + 1] = s_pair[r >> 16 & 15] | s_pair[r >> 20 & 15] << 8 | s_pair[r >> 24 & 15] << 16 |
		           (u32)s_pair[r >> 28] << 24;
	}
	C3D_TexFlush(t);
}

static void states(C3D_Tex* t)
{
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	// D3DBLEND_INVSRCALPHA / D3DBLEND_SRCALPHA: out = grain * (1 - a) + scene * a
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ZERO, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, s_color);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_TexBind(0, t);
}

void noise_draw(void)
{
	if (noise_on())
		noise_draw_always();
}

void noise_draw_always(void)
{
	if (!stereo_eye()) {             // the second eye of a 3D frame shows the same grain (stereo.h)
		s_cur ^= 1;
		refill(&s_tex[s_cur]);
	}
	C3D_Tex* t = &s_tex[s_cur];
	states(t);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_quad, sizeof(V), 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
}

void noise_draw_movie(float x0, float y0, float x1, float y1)
{
	// the PC draws its grain over the movies too (0x4781e0 runs in every frame); the Enhanced
	// Edition limits it to the picture (IDirect3DDevice8.cpp, the FMV vertex)
	if (!noise_on())
		return;
	if (!s_movie_quad && !(s_movie_quad = linearAlloc(16 * sizeof(float))))
		return;
	s_cur ^= 1;
	C3D_Tex* t = &s_tex[s_cur];
	refill(t);
	float u1 = (x1 - x0) / SIZE, v1 = (y1 - y0) / SIZE;
	const float q[16] = { x0, y1, 0, v1, x0, y0, 0, 0, x1, y1, u1, v1, x1, y0, u1, 0 };
	memcpy(s_movie_quad, q, sizeof q);
	GSPGPU_FlushDataCache(s_movie_quad, sizeof q);
	states(t);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_movie_quad, 4 * sizeof(float), 2, 0x10);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
}
