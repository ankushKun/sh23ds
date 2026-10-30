#include "lensflare.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>
#include "stereo.h"

// The Director's Cut's lens flare, the PS2 beta's Lens/lens_flare.c and kari_lf_draw.c. James'
// character code calls 0x511db0 shLensFlareExec once a frame while the option is on (0x538140 for
// kinds 0x100 and 0x101, 0x5383a0), and the end of the frame (0x476bf0, dark rooms only: 0x47b1f0)
// calls 0x511bb0 kari_shLensFlareDraw, which tests whether the lamp is seen, fades the flare and
// draws it (0x510040 kari_shLensFlareEffect_Draw).
//
//   0x5119e0 shLensFlareSetLightSeed  the lamp: the room's spot while its record has it on (0x47dea0;
//             the apartments' stage hook 0x59bca0 keeps room 205's on until the flashlight is taken)
//             at its place plus (0, dir.y * 50 - 3, dir.z * 50 - 20), in front of the lens on the
//             mannequin; otherwise James' lamp (0x533960, set by 0x536190)
//   0x511d00 shLensFlareMakeScreenInfo  the lamp in view space: its depth z, the angle between its
//             aim and the view (0 aimed away, pi straight at the camera) and the aim's turn on the
//             screen; 0x5118e0 shLensFlareMakeScreenPos its place on the screen
//   0x511db0  nothing unless the lamp lies beyond half the projection distance (gCameraFOV / 2); the
//             goal brightness (0x511850 shLensFlareMakeEffectTargetRate) min(2, b * d * a) from three
//             tables read by 0x5117a0 shLensFlareOresenHokan: b of the flashlight colour x 3/7, d of
//             the depth, a of the angle
//   0x511720  in view only in front of the camera with the angle at least 1.3614 (78 degrees), and
//             the lamp's depth on the screen for the glow while it is not seen
//   0x511170 shLensFlareLightCenterIsVisible  draws a small shape round the lamp (x +-5.1, y -6..+3,
//             z -3..+6) against the depth buffer and reads it back; seen from the fifth frame in a row
//   0x511bb0  brightness += ((seen ? goal : 0) - brightness) * 0.3 a frame
//   0x510040  the quads (0x50fc00 shLensFlareSpriteAddPacketGif, 0x50fd30
//             shLensFlarePolyFT4AddPacketGif, the ghosts 0x50fe60 shLensFlareDrawCommon): additive
//             (SRCALPHA, ONE), texture times vertex colour, colour bytes base x k with k held to 0..4
//             (0x50fb50), sizes in pixels of a 640 x 480 picture
//   0x511b80  the flashlight going off (0x50a4b0) clears the brightness and the glow's fade-in;
//   0x511ba0  it coming on (0x50a1a0), or the room's spot (0x47cdc0), lets the flare run again
//
// The port's own: the seen test is a ray to the lamp through the rooms' drawn triangles (camray.h)
// and, for the room's spot, a column round James (main.c), not a read of the depth buffer. No flare
// in the scenes, nor for the mirror's copy of James' lamp (0x546280, the second light), and bit 0 of
// 0x1f7d3a4 (set by a scenario script, 0x49fab0), which clears it on the PC, is not read. The PC's
// rates are per 30 fps frame; here they follow dt. The quads lie on the plane through the lamp
// facing the camera, sized by the share of the screen's height the PC gives them, so in 3D they
// stand at the lamp's depth.

#define MAX_QUADS 32
#define FPS       30.0f
#define SCR_H     480.0f                 // the PC's picture: its sizes are pixels of 480 lines
#define SCR_W     (SCR_H * 400.0f / 240.0f)

typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;   // the world program's vertex

// 0x5117a0's tables: 0x8a473c by the angle (1.3614..2.9147), 0x8a4720 by the depth (204.8..3328),
// 0x8a4718 by the flashlight colour x 3/7 (0..1)
static const float BY_ANGLE[7] = { 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.5f, 1.3f };
static const float BY_DEPTH[7] = { 4.0f, 1.3f, 0.9f, 0.7f, 0.5f, 0.35f, 0.2f };
static const float BY_LIGHT[2] = { 0.0f, 1.0f };
// texture rectangles u0, v0, u1, v1 (0x6c3340, 0x20 bytes each): the glow, the ring, the oval,
// the ghost
static const float RECT[4][4] = { { 0.0f, 0.0f, 0.5f, 0.5f }, { 0.5f, 0.0f, 1.0f, 0.5f },
                                  { 0.25f, 0.5f, 0.625f, 1.0f }, { 0.625f, 0.5f, 1.0f, 1.0f } };
// the streak's corners (0x6c33c0): the rays leave the first one
static const float STREAK_UV[4][2] = { { 0.0547f, 0.5547f }, { 0.25f, 0.5547f }, { 0.0547f, 1.0f }, { 0.25f, 1.0f } };
static const float STREAK4[4] = { -3.141593f, -1.570796f, 0.0f, 1.570796f };   // 0x8a46d8
static const float STREAK12[12] = { -2.932153f, -2.268928f, -1.047198f, -0.872665f, -0.261799f, 0.349066f,
                                    0.698132f, 1.22173f, 2.129302f, 2.565634f, 2.879793f, 0.0f };   // 0x8a46e8
// the six ghosts: along the line from the lamp through the screen's centre (0x8a45f8), size
// (0x8a4614), rectangle (0x8a4630), colour (0x8a4668); the tables' seventh entry is not drawn
static const float GHOST_T[6] = { 0.4f, 0.2f, -0.3f, -0.7f, -1.4f, -2.2f };
static const float GHOST_SIZE[6] = { 12.0f, 10.0f, 28.0f, 20.0f, 20.0f, 58.0f };
static const u8 GHOST_RECT[6] = { 3, 2, 2, 3, 2, 3 };
static const u8 GHOST_RGB[6][3] = { { 72, 48, 72 }, { 40, 44, 44 }, { 40, 46, 40 },
                                    { 36, 36, 36 }, { 40, 40, 32 }, { 64, 48, 40 } };

static LensFlareBindFn s_bind;
static LensFlareClearFn s_clear;
static C3D_Tex s_tex;
static bool s_tex_ok;
static V* s_verts[2];
static int s_buf, s_n_depth, s_n_top;    // this frame's quads: tested against the scene, then on top

static struct {
	int kind;
	float pos[3], dir[3], bright;
	float goal, lum, fade;               // +4, +8 (0xac97e0), +0x38 (the glow's fade-in)
	float seen_t;                        // 0xac985c, in time
	// this frame
	bool pending, facing, seen;
	float x, y, z, angle, roll, unit;    // unit: world units of one pixel at the lamp's depth
} F;

bool lensflare_init(const char* pack_dir, LensFlareBindFn bind, LensFlareClearFn clear)
{
	s_bind = bind, s_clear = clear;
	char path[128];
	snprintf(path, sizeof path, "%s/effect/lensflare.t3x", pack_dir);
	FILE* f = fopen(path, "rb");
	if (f) {
		Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_tex, NULL, false);
		fclose(f);
		if (t) {
			Tex3DS_TextureFree(t);
			C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			s_tex_ok = true;
		}
	}
	for (int i = 0; i < 2; i++)
		s_verts[i] = linearAlloc(MAX_QUADS * 6 * sizeof(V));
	return s_tex_ok && s_verts[0] && s_verts[1];
}

void lensflare_exit(void)
{
	if (s_tex_ok)
		C3D_TexDelete(&s_tex);
	s_tex_ok = false;
	for (int i = 0; i < 2; i++)
		linearFree(s_verts[i]), s_verts[i] = NULL;
}

void lensflare_light(int kind, const float pos[3], const float dir[3], float bright)
{
	F.kind = kind;
	if (kind == LF_NONE)
		return;
	memcpy(F.pos, pos, sizeof F.pos);
	memcpy(F.dir, dir, sizeof F.dir);
	F.bright = bright;
}

// 0x5117a0 shLensFlareOresenHokan: the table's n values spread evenly from lo to hi, linear in
// between, held at the ends
static float table(float x, float lo, float hi, const float* t, int n)
{
	if (!(x < hi))
		return t[n - 1];
	if (x < lo)
		return t[0];
	float step = (hi - lo) / (float)(n - 1), r = x - lo;
	int i = (int)(r / step);
	if (i >= n - 1)
		return t[n - 1];
	r -= (float)i * step;
	return ((step - r) * t[i] + r * t[i + 1]) / step;
}

// The camera's place: the view is R p + t, so it is -R^T t.
static void eye_of(const C3D_Mtx* v, float e[3])
{
	e[0] = -(v->r[0].x * v->r[0].w + v->r[1].x * v->r[1].w + v->r[2].x * v->r[2].w);
	e[1] = -(v->r[0].y * v->r[0].w + v->r[1].y * v->r[1].w + v->r[2].y * v->r[2].w);
	e[2] = -(v->r[0].z * v->r[0].w + v->r[1].z * v->r[1].w + v->r[2].z * v->r[2].w);
}

// One frame of 0x511db0 and 0x511bb0's test and fade.
static void step(const C3D_Mtx* view, float scr_z, float dt)
{
	F.pending = false;
	if (F.kind == LF_NONE) {             // 0x511b80 (0x511ba0 lets it run again)
		F.lum = 0.0f, F.fade = 0.0f, F.seen_t = 0.0f;
		return;
	}
	float p[3] = { F.pos[0], F.pos[1], F.pos[2] };
	if (F.kind == LF_ROOM)               // 0x5119e0 shLensFlareSetLightSeed
		p[1] += F.dir[1] * 50.0f - 3.0f, p[2] += F.dir[2] * 50.0f - 20.0f;
	// 0x511d00 shLensFlareMakeScreenInfo: the port's view looks down -z, the PC's down +z
	C3D_FVec vp = Mtx_MultiplyFVec4(view, FVec4_New(p[0], p[1], p[2], 1.0f));
	C3D_FVec vd = Mtx_MultiplyFVec4(view, FVec4_New(F.dir[0], F.dir[1], F.dir[2], 0.0f));
	float nx = vd.x, ny = vd.y, nz = -vd.z, len = sqrtf(nx * nx + ny * ny + nz * nz);
	float k = len > 0.0f ? 1.0f / len : 1.0f;         // 0x511c50
	nx *= k, ny *= k, nz *= k;
	F.z = -vp.z;
	F.angle = atan2f(sqrtf(nx * nx + ny * ny), nz);
	F.roll = atan2f(ny, nx);
	if (!(scr_z * 0.5f < F.z))           // 0x511db0
		return;
	// 0x5118e0 shLensFlareMakeScreenPos: its place in pixels of a picture SCR_H high, as wide as the
	// top screen's share
	float ty = scr_z > 0.0f ? 224.0f / scr_z : 0.57735f;    // tan of half the vertical view
	F.unit = 2.0f * F.z * ty / SCR_H;
	F.x = SCR_W * 0.5f + vp.x / F.unit;
	F.y = SCR_H * 0.5f - vp.y / F.unit;
	// 0x511850 shLensFlareMakeEffectTargetRate
	float goal = table(F.bright * 0.428571f, 0.0f, 1.0f, BY_LIGHT, 2) * table(F.z, 204.8f, 3328.0f, BY_DEPTH, 7) *
	             table(F.angle, 1.361357f, 2.9147f, BY_ANGLE, 7);
	F.goal = goal > 2.0f ? 2.0f : goal;
	F.facing = F.z >= 0.0f && F.angle >= 1.361357f;     // 0x511720
	F.pending = true;
	// 0x511bb0 kari_shLensFlareDraw, 0x511170 shLensFlareLightCenterIsVisible
	float eye[3];
	eye_of(view, eye);
	if (F.facing && (!s_clear || s_clear(eye, p, F.kind == LF_ROOM)))
		F.seen_t += dt;
	else
		F.seen_t = 0.0f;
	F.seen = F.seen_t > 4.5f / FPS;
	F.lum += ((F.seen ? F.goal : 0.0f) - F.lum) * (1.0f - powf(0.7f, dt * FPS));
}

// ---------------------------------------------------------------- the quads (0x510040 kari_shLensFlareEffect_Draw)

static V* s_out;
static V* s_end;

// 0x50fb50: the colour bytes base x k, k held to 0..4, each byte to 255; a black quad is skipped
// (0x50fc00 and 0x50fd30 leave out 0xff000000)
static bool colour(u8 c[4], int r, int g, int b, float k)
{
	k = k < 0.0f ? 0.0f : k > 4.0f ? 4.0f : k;
	int v[3] = { (int)((float)r * k), (int)((float)g * k), (int)((float)b * k) };
	for (int i = 0; i < 3; i++)
		c[i] = (u8)(v[i] > 255 ? 255 : v[i]);
	c[3] = 255;
	return c[0] | c[1] | c[2];
}

// A picture point on the plane through the lamp facing the camera (view space). The PC's texture
// coordinates go in as they are: v 0 samples the sheet's top row.
static V vert(float px, float py, float u, float v, const u8 c[4])
{
	return (V){ (px - SCR_W * 0.5f) * F.unit, (SCR_H * 0.5f - py) * F.unit, -F.z, u, v,
	            { c[0], c[1], c[2], c[3] }, { 0, 0, 127, 0 } };
}

// 0x50fc00 shLensFlareSpriteAddPacketGif: (x0, y0) to (x1, y1) with a rectangle of the texture
static void quad(float x0, float y0, float x1, float y1, const float* r, const u8 c[4])
{
	if (s_out + 6 > s_end)
		return;
	V a = vert(x0, y0, r[0], r[1], c), b = vert(x1, y0, r[2], r[1], c);
	V d = vert(x1, y1, r[2], r[3], c), e = vert(x0, y1, r[0], r[3], c);
	s_out[0] = a, s_out[1] = b, s_out[2] = d, s_out[3] = a, s_out[4] = d, s_out[5] = e;
	s_out += 6;
}

// 0x50fd30 shLensFlarePolyFT4AddPacketGif: a streak, the corners (a, b), (c, b), (a, d), (c, d)
// turned by th round the lamp
static void streak(float th, float a, float b, float c, float d, const u8 col[4])
{
	if (s_out + 6 > s_end)
		return;
	float s = sinf(th), k = cosf(th);
	const float lx[4] = { a, c, a, c }, ly[4] = { b, b, d, d };
	V q[4];
	for (int i = 0; i < 4; i++)
		q[i] = vert(k * lx[i] - s * ly[i] + F.x, s * lx[i] + k * ly[i] + F.y, STREAK_UV[i][0], STREAK_UV[i][1], col);
	s_out[0] = q[0], s_out[1] = q[1], s_out[2] = q[3], s_out[3] = q[0], s_out[4] = q[3], s_out[5] = q[2];
	s_out += 6;
}

static void build(void)
{
	V* base = s_verts[s_buf];
	s_out = base, s_end = base + MAX_QUADS * 6;
	u8 c[4];
	float S = 128.0f - sqrtf(F.z * 1.5f);                 // the flare's size in pixels
	S = S < 0.0f ? 0.0f : S;
	float sr = sinf(F.roll), cr = cosf(F.roll), sa = sinf(F.angle), I = F.lum;
	// the glow and its core, at the lamp's depth while it is not seen (tested against the scene)
	// and in front of everything once it is; the ring (all three only while the flashlight shines)
	V* tested = s_out;
	if (F.bright >= 0.002f) {
		float ox = sr * cr * 5.0f, oy = sr * sa * 5.0f, h = (I + 2.0f) * 0.25f * S;
		if (colour(c, 0x28, 0x28, 0x26, (I + 3.0f) * F.fade * 0.25f))
			quad(F.x - (h - ox), F.y - (h - oy), F.x + ox + h, F.y + h + oy, RECT[0], c);
		if (colour(c, 0x20, 0x20, 0x20, F.fade))
			quad(F.x - 11.0f, F.y - 11.0f, F.x + 11.0f, F.y + 11.0f, RECT[0], c);
	}
	s_n_depth = F.seen ? 0 : (int)(s_out - tested);
	if (F.bright >= 0.002f) {
		float ox = sr * cr * 9.0f, oy = sr * sa * 9.0f, h = (I + 3.0f) * 0.25f * S;
		if (colour(c, 0x20, 0x20, 0x20, F.fade * (2.0f * I < 1.0f ? 2.0f * I : 1.0f)))
			quad(F.x - (h - ox), F.y - (h - oy), F.x + ox + h, F.y + h + oy, RECT[1], c);
	}
	if (I > 0.001f) {
		// the streaks: four in a cross, then twelve whose brightness follows the lamp's turn
		float a = S * -0.061f, b = S * -0.03f, cc = S - S * 0.061f, d = S - S * 0.03f;
		float xy = F.x - (SCR_W - 640.0f) * 0.5f + F.y;    // the PC's x + y on its 640 x 480 picture
		float th0 = xy * 0.006136f + F.angle * -0.5f + 1.047198f;
		if (colour(c, 0x1c, 0x1c, 0x1c, I))
			for (int i = 0; i < 4; i++)
				streak(th0 + STREAK4[i], a, b, cc, d, c);
		for (int i = 0; i < 12; i++) {
			float th = th0 + STREAK12[i];
			float wave = cosf(th * 12.0f + F.z * 0.006136f + xy * 0.024544f);
			float f = (cosf(th - F.roll) + 0.85f) * ((wave + 0.5f) * 0.5f);
			f = f > 0.75f ? 0.75f : f;
			if (colour(c, 0x10, 0x10, 0x10, f * I))
				streak(th, a, b, cc, d, c);
		}
		// the ghosts (0x50fe60 shLensFlareDrawCommon)
		for (int i = 0; i < 6; i++) {
			float h = (I + 3.0f) * 0.25f * GHOST_SIZE[i];
			float gx = (F.x - SCR_W * 0.5f) * GHOST_T[i] + SCR_W * 0.5f;
			float gy = (F.y - SCR_H * 0.5f) * GHOST_T[i] + SCR_H * 0.5f;
			if (colour(c, GHOST_RGB[i][0], GHOST_RGB[i][1], GHOST_RGB[i][2], I))
				quad(gx - h, gy - h, gx + h, gy + h, RECT[GHOST_RECT[i]], c);
		}
		// the centre: a glow that shrinks with the depth, and a small violet one
		float g = 160.0f - sqrtf(F.z + F.z);
		if (colour(c, 0x20, 0x20, 0x20, I))
			quad(F.x - g, F.y - g, F.x + g, F.y + g, RECT[0], c);
		if (colour(c, 0x40, 0x18, 0x5e, I))
			quad(F.x - 10.0f, F.y - 10.0f, F.x + 10.0f, F.y + 10.0f, RECT[0], c);
	}
	s_n_top = (int)(s_out - tested) - s_n_depth;
	if (s_out > base)
		GSPGPU_FlushDataCache(base, (u32)((u8*)s_out - (u8*)base));
}

static void flush(V* first, int n)
{
	if (n <= 0)
		return;
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, first, sizeof(V), 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, n);
}

void lensflare_draw(const C3D_Mtx* proj, const C3D_Mtx* view, float scr_z, float dt)
{
	if (!s_bind || !s_tex_ok || !s_verts[0] || !s_verts[1])
		return;
	if (!stereo_eye()) {                 // the second eye draws what the first one built
		step(view, scr_z, dt);
		s_n_depth = s_n_top = 0;
		if (!F.pending)
			return;
		s_buf ^= 1;
		build();
		if (F.bright >= 0.002f)          // 0x510040: the glow fades in after the flare comes on
			F.fade = fminf(1.0f, F.fade + 0.2f * dt * FPS);
	}
	if (s_n_depth + s_n_top <= 0)
		return;
	C3D_Mtx id;
	Mtx_Identity(&id);                   // the vertices are in view space already
	s_bind(proj, &id);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_TexBind(0, &s_tex);
	V* base = s_verts[s_buf];
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	flush(base, s_n_depth);
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
	flush(base + s_n_depth, s_n_top);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
	               GPU_ONE_MINUS_SRC_ALPHA);
}

const char* lensflare_status(void)
{
	static char line[112];
	snprintf(line, sizeof line, "lens flare %s%s kind %d z %.0f angle %.2f goal %.2f lum %.2f fade %.2f quads %d+%d",
	         s_tex_ok ? "" : "(no texture) ", F.pending ? (F.seen ? "seen" : "hidden") : "off", F.kind, F.z, F.angle,
	         F.goal, F.lum, F.fade, s_n_depth / 6, s_n_top / 6);
	return line;
}
