#include "gameover.h"
#include "audio.h"
#include "font.h"
#include "sfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <tex3ds.h>

#define PACK_VOICE "sdmc:/3ds/sh23ds/sound/voice/143.adx"

static C3D_Tex s_tex;
static bool s_ok, s_on;
static int s_step;               // 0x9503bc
static float s_t, s_level;       // the step's timer; the text's brightness 0..0x60 (0x9503b8)
static int s_jitter[3];
static float s_all;              // since the start
static C3D_Tex s_still;          // the last frame of play (gameover_capture)
static bool s_still_ok, s_have_still;
static int s_still_w;

bool gameover_init(const char* pack_dir)
{
	char path[128];
	snprintf(path, sizeof path, "%s/ui/gameover.t3x", pack_dir);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
	s_ok = true;
	return true;
}

void gameover_start(void)
{
	// 0x498780: the sound stops (0x3f4) and SE 60143 plays (voice.afs 143)
	audio_stop_music();
	audio_stop_voice();
	sfx_stop_all();
	audio_play_voice(PACK_VOICE, 1.0f);
	s_on = true;
	s_step = 0;
	s_t = s_all = 0.0f;
	s_level = 0.0f;
	s_have_still = false;
}

void gameover_capture(const void* color, int w, int h)
{
	int tw = w <= 256 ? 256 : 512;
	if (s_still_ok && s_still.width != tw) {
		C3D_TexDelete(&s_still);
		s_still_ok = false;
	}
	if (!s_still_ok) {
		if (!C3D_TexInit(&s_still, tw, 512, GPU_RGBA8))
			return;
		C3D_TexSetFilter(&s_still, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&s_still, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		s_still_ok = true;
	}
	// both tiled in 8x8 blocks: a row of blocks at a time, the texture's wider rows leaving a gap
	u32 line = (u32)w * 8 * 4, gap = (u32)(tw - w) * 8 * 4;
	// (flags bit 3: the transfer engine's texture copy mode, which GX_TextureCopy leaves to the caller)
	C3D_SyncTextureCopy((u32*)color, GX_BUFFER_DIM(line >> 4, 0), (u32*)s_still.data, GX_BUFFER_DIM(line >> 4, gap >> 4),
	                    line * (u32)(h / 8), 8);
	s_still_w = w;
	s_have_still = true;
}

bool gameover_active(void) { return s_on; }

bool gameover_update(float dt)
{
	if (!s_on)
		return false;
	float ticks = dt * 60.0f;
	s_t += dt;
	s_all += dt;
	switch (s_step) {
	case 0:                      // a second of black
		if (s_t >= 1.0f)
			s_step++, s_t = 0.0f;
		break;
	case 1:                      // up by 2 a tick to 0x60
		if ((s_level += 2.0f * ticks) >= 96.0f)
			s_level = 96.0f, s_step++, s_t = 0.0f;
		break;
	case 2:                      // two seconds
		if (s_t >= 2.0f)
			s_step++, s_t = 0.0f;
		break;
	case 3:                      // down by 4 a tick
		if ((s_level -= 4.0f * ticks) <= 0.0f)
			s_level = 0.0f, s_step++, s_t = 0.0f;
		break;
	default:                     // the fade (already black) and back to the title
		if (s_t >= 0.5f) {
			s_on = false;
			audio_stop_voice();
			if (s_still_ok)
				C3D_TexDelete(&s_still);
			s_still_ok = s_have_still = false;
			return false;
		}
	}
	// 0x496ee0: each copy moves by rand() % 30 - 15 lines a frame
	for (int i = 0; i < 3; i++)
		s_jitter[i] = rand() % 30 - 15;
	return true;
}

// one 160x48 copy: texture rows (0, v0) to (160, v0 + 47), centred on the screen. The PC's 640x448
// screen maps to the 4:3 picture as 0.5; the port draws it at twice that, 160 pixels across.
static void sprite(MapDrawFn draw, float v0, float dy, u32 rgba)
{
	float sx = 1.0f, sy = 480.0f / 448.0f;
	float x0 = 200.0f - 80.0f * sx, x1 = 200.0f + 80.0f * sx;
	float y0 = 120.0f + (-24.0f + dy) * sy, y1 = 120.0f + (23.0f + dy) * sy;
	// the texture is the top left 256x256 of the 512x512 picture
	float u1 = 160.0f / 256.0f, t0 = v0 / 256.0f, t1 = (v0 + 47.0f) / 256.0f;
	MapVert v[6] = { { x0, y0, 0, t0 }, { x1, y0, u1, t0 }, { x1, y1, u1, t1 },
	                 { x0, y0, 0, t0 }, { x1, y1, u1, t1 }, { x0, y1, 0, t1 } };
	draw(&s_tex, v, 6, rgba);
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float smooth(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

static u32 rgba_of(float r, float g, float b, float a)
{
	return (u32)(clamp01(r) * 255.0f) << 24 | (u32)(clamp01(g) * 255.0f) << 16 | (u32)(clamp01(b) * 255.0f) << 8 |
	       (u32)(clamp01(a) * 255.0f);
}

// The still's texture coordinates for a point of the screen, z the push-in
static void still_uv(float x, float y, float z, float* u, float* v)
{
	x = 200.0f + (x - 200.0f) / z;
	y = 120.0f + (y - 120.0f) / z;
	// the GPU's x runs up the screen and its y across it, as the top screen's buffers are turned
	*u = (240.0f - y) * ((float)s_still_w / 240.0f) / (float)s_still.width;
	*v = x / 512.0f;
}

void gameover_draw_back(void)
{
	if (!s_on)
		return;
	// The frame James fell in, already bled red by the dying veil (main.c draw_dying), keeps sinking
	// under the text with a slow beat and goes to black as the text leaves. It closes in all along.
	float bleed = smooth(s_all / 4.0f);
	float bright = 1.0f - 0.4f * bleed + 0.04f * sinf(s_all * 5.7f) * bleed;   // about a beat a second
	if (s_step == 3)
		bright *= 0.5f + 0.5f * clamp01(s_level / 96.0f);   // half way down with the text
	else if (s_step > 3)
		bright *= 0.5f * clamp01(1.0f - s_t / 0.5f);          // and to black before the title
	if (s_have_still) {
		float z = 1.0f + 0.07f * smooth(s_all / 6.0f);
		float uv[8];
		still_uv(0.0f, 0.0f, z, &uv[0], &uv[1]);
		still_uv(400.0f, 0.0f, z, &uv[2], &uv[3]);
		still_uv(0.0f, 240.0f, z, &uv[4], &uv[5]);
		still_uv(400.0f, 240.0f, z, &uv[6], &uv[7]);
		u32 c = rgba_of(bright, bright * (1.0f - 0.35f * bleed), bright * (1.0f - 0.35f * bleed), 1.0f);
		const u32 col[4] = { c, c, c, c };
		font_quad_uv_px(&s_still, 0.0f, 0.0f, 400.0f, 240.0f, uv, col);
	}
	// the dark closing in from the edges
	float e = 0.5f * bleed;
	u32 k = rgba_of(0.0f, 0.0f, 0.0f, e), o = 0x00000000;
	const u32 l[4] = { k, o, k, o }, r[4] = { o, k, o, k }, t[4] = { k, k, o, o }, b[4] = { o, o, k, k };
	font_quad_px(NULL, 0.0f, 0.0f, 150.0f, 240.0f, 0, 0, 0, 0, l);
	font_quad_px(NULL, 250.0f, 0.0f, 400.0f, 240.0f, 0, 0, 0, 0, r);
	font_quad_px(NULL, 0.0f, 0.0f, 400.0f, 90.0f, 0, 0, 0, 0, t);
	font_quad_px(NULL, 0.0f, 150.0f, 400.0f, 240.0f, 0, 0, 0, 0, b);
}

void gameover_draw_top(MapDrawFn draw)
{
	if (!s_on || !s_ok || !draw || s_level <= 0.0f)
		return;
	// the brightness in PS2 units (0x80 full): the steady copy pale as the PC draws it, the jumping
	// ones in blood red behind it
	float a = s_level / 96.0f;
	u32 g = (u32)(s_level * 2.0f);
	g = g > 255 ? 255 : g;
	u32 steady = g << 24 | g << 16 | g << 8 | 0xff;
	u32 red = rgba_of(0.75f, 0.06f, 0.05f, 0.85f * a);
	for (int i = 0; i < 3; i++)
		sprite(draw, 48.0f * i, (float)s_jitter[i], red);
	sprite(draw, 0.0f, 0.0f, steady);
}
