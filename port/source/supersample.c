#include "supersample.h"
#include "picture.h"
#include "quality.h"
#include <stdio.h>

// supersample.h says what this is. The numbers are fractions of the frame's budget: perf_pace drops
// a 60 fps frame to 30 after frames of more than 15.5 ms of work, 0.93 of its 16.7.
#define FIT 0.84f                // 14 ms at 60 fps: a step up wants its frames to double into this
#define ON_FRAMES 120            // for 2 seconds in a row
#define HOLD_FRAMES 300          // after a step down, 5 seconds before it may step up again
#define HOLD_MAX 3600            // doubling up to a minute when it stepped down again within 10 seconds
#define SHORT_RUN 600

// The levels: the scene's target as the GPU stores it (the screen's 240 rows run along x) and the
// display transfer's halving when it goes to the screen directly.
static const struct { s16 w, h; u8 scale; const char* name; } LEVEL[3] = {
	{ 240, 400, GX_TRANSFER_SCALE_NO, "1x" },
	{ 480, 400, GX_TRANSFER_SCALE_X, "2x" },
	{ 480, 800, GX_TRANSFER_SCALE_XY, "2x2" },
};

static struct {
	C3D_RenderTarget *top, *big, *shown;
	int big_level;               // the level big was made for, 0 none
	u32 flags, shown_flags;
	int level;                   // the level the next frames that may are drawn at
	int limit;                   // the levels below it could be made (VRAM, the soft pass's texture)
	bool wanted;                 // this frame asked for a larger target
	int used, drew[2];           // this frame's level, the last two frames'
	int fit, hold, backoff, run;
	char status[48];
} S;

void supersample_init(C3D_RenderTarget* top, u32 transfer)
{
	S.top = S.shown = top;
	S.flags = S.shown_flags = transfer;
	S.backoff = HOLD_FRAMES;
	S.limit = 3;
}

// The larger target for level lv, made in place of the other one (only one is kept: 3 MB of the
// 6 MB of VRAM at most). Called at a frame's start, when the GPU is done with the last one.
static bool make_big(int lv)
{
	if (S.big_level == lv)
		return true;
	if (S.big) {
		if (S.shown == S.big)
			S.shown = NULL;
		C3D_RenderTargetDelete(S.big);
		S.big = NULL;
		S.big_level = 0;
	}
	S.big = C3D_RenderTargetCreate(LEVEL[lv].w, LEVEL[lv].h, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!S.big)
		return false;
	S.big_level = lv;
	return true;
}

C3D_RenderTarget* supersample_target(bool want)
{
	bool soft = picture_soft();
	int lv = want ? S.level : 0;
	while (lv > 0 && (lv >= S.limit || (soft && !picture_reserve(LEVEL[lv].w, LEVEL[lv].h)) || !make_big(lv))) {
		if (lv < S.limit)
			S.limit = lv;            // could not be made: never asked for again
		lv--;
	}
	if (S.level > lv && want)
		S.level = lv;
	// on Soft the soft pass brings the scene to the screen's target, which is the one shown
	C3D_RenderTarget* draw = lv > 0 ? S.big : S.top;
	C3D_RenderTarget* show = lv > 0 && !soft ? S.big : S.top;
	u32 flags = show == S.big ? (S.flags & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(LEVEL[lv].scale) : S.flags;
	if (show != S.shown || flags != S.shown_flags) {
		C3D_RenderTargetSetOutput(show, GFX_TOP, GFX_LEFT, flags);
		S.shown = show;
		S.shown_flags = flags;
	}
	S.wanted |= want;
	if (lv > S.used)
		S.used = lv;
	return draw;
}

C3D_RenderTarget* supersample_screen(C3D_RenderTarget* t, int* sx, int* sy)
{
	if (t && t == S.big) {
		*sx = LEVEL[S.big_level].w / LEVEL[0].w;
		*sy = LEVEL[S.big_level].h / LEVEL[0].h;
		return S.top;
	}
	*sx = *sy = 1;
	return t;
}

void supersample_frame_end(int fps, int cap)
{
	float gpu = C3D_GetDrawingTime();    // the frame before this one's
	int measured = S.drew[1];
	S.drew[1] = S.drew[0];
	S.drew[0] = S.used;
	bool wanted = S.wanted;
	S.wanted = false, S.used = 0;
	int mode = quality()->supersample;
	if (mode != 1) {                     // 0 off, 2 always 2x, 3 always 2x2 (debug.txt)
		S.level = mode == 2 ? 1 : mode == 3 ? 2 : 0;
		S.fit = 0;
		return;
	}
	if (S.hold > 0)
		S.hold--;
	if (!wanted) {                       // 3D or a movie: the screen's own size
		S.fit = 0;
		return;
	}
	int rate = cap == 30 ? 30 : 60;
	if (fps < rate) {                    // the pacing gave up a frame: the screen's size again
		S.level = 0;
		S.fit = 0;
		return;
	}
	float budget = 1000.0f / rate;
	if (measured != S.level) {           // the time read belongs to another level
		S.fit = 0;
		return;
	}
	if (S.level > 0) {
		S.run++;
		if (gpu > FIT * budget) {
			S.level--;
			S.fit = 0;
			S.backoff = S.run < SHORT_RUN ? (S.backoff * 2 > HOLD_MAX ? HOLD_MAX : S.backoff * 2) : HOLD_FRAMES;
			S.hold = S.backoff;
			S.run = 0;
			return;
		}
	}
	int top = S.limit - 1 < 2 ? S.limit - 1 : 2;
	S.fit = S.level < top && gpu * 2.0f < FIT * budget ? S.fit + 1 : 0;
	if (S.fit >= ON_FRAMES && !S.hold) {
		S.level++;
		S.fit = 0;
		S.run = 0;
	}
}

C3D_RenderTarget* supersample_shown(u32* transfer)
{
	*transfer = S.shown_flags;
	return S.shown;
}

const char* supersample_status(void)
{
	snprintf(S.status, sizeof S.status, "%s%s%s", LEVEL[S.drew[0]].name, picture_soft() ? " soft" : "",
	         S.limit < 3 ? " (vram)" : S.hold ? " (wait)" : "");
	return S.status;
}
