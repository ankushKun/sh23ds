#include "gameover.h"
#include "audio.h"
#include "sfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <tex3ds.h>

#define PACK_VOICE "sdmc:/3ds/sh23ds/sound/voice/143.adx"

static C3D_Tex s_tex;
static bool s_ok, s_on;
static int s_step;               // 0x9503bc
static float s_t, s_level;       // the step's timer; the text's brightness 0..0x60 (0x9503b8)
static int s_jitter[3];

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
	s_t = 0.0f;
	s_level = 0.0f;
}

bool gameover_active(void) { return s_on; }

bool gameover_update(float dt)
{
	if (!s_on)
		return false;
	float ticks = dt * 60.0f;
	s_t += dt;
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
			return false;
		}
	}
	// 0x496ee0: each copy moves by rand() % 30 - 15 lines a frame
	for (int i = 0; i < 3; i++)
		s_jitter[i] = rand() % 30 - 15;
	return true;
}

// one 160x48 copy: texture rows (0, v0) to (160, v0 + 47), centred on the screen
static void sprite(MapDrawFn draw, float v0, float dy, u32 rgba)
{
	// the PC's 640x448 screen to the 4:3 picture on the top screen (320x240)
	float sx = 0.5f, sy = 240.0f / 448.0f;
	float x0 = 200.0f - 80.0f * sx, x1 = 200.0f + 80.0f * sx;
	float y0 = 120.0f + (-24.0f + dy) * sy, y1 = 120.0f + (23.0f + dy) * sy;
	// the texture is the top left 256x256 of the 512x512 picture
	float u1 = 160.0f / 256.0f, t0 = v0 / 256.0f, t1 = (v0 + 47.0f) / 256.0f;
	MapVert v[6] = { { x0, y0, 0, t0 }, { x1, y0, u1, t0 }, { x1, y1, u1, t1 },
	                 { x0, y0, 0, t0 }, { x1, y1, u1, t1 }, { x0, y1, 0, t1 } };
	draw(&s_tex, v, 6, rgba);
}

void gameover_draw_top(MapDrawFn draw)
{
	if (!s_on || !s_ok || !draw || s_level <= 0.0f)
		return;
	// the brightness in PS2 units (0x80 full) as the colour, the copies as the steady one
	u32 g = (u32)(s_level * 2.0f);
	g = g > 255 ? 255 : g;
	u32 rgba = g << 24 | g << 16 | g << 8 | 0xff;
	for (int i = 0; i < 3; i++)
		sprite(draw, 48.0f * i, (float)s_jitter[i], rgba);
	sprite(draw, 0.0f, 0.0f, rgba);
}
