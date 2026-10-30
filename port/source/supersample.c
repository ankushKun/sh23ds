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
	// Soft's scenes (picture.h), in VRAM: [0] 256 x 512 for the screen's size and the left eye, [1]
	// 512 x 512 for 2x and the right eye. Made and freed after a frame that asked (need).
	struct { C3D_Tex tex; C3D_RenderTarget* rt; bool failed; } soft[2];
	int need;
	u32 flags, shown_flags;
	int level;                   // the level the next frames that may are drawn at
	int limit;                   // the levels below it could be made (VRAM, the soft pass's texture)
	bool swap;                   // big is the other level's: replaced after the frame (make_big)
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

// The larger target for level lv. Only one is kept (3 MB of the 6 MB of VRAM at most), and one made
// for the other level is replaced after the frame: citro3d stops the app (svcBreak) when a target
// is deleted inside a frame, so supersample_target draws that frame at the screen's size and
// supersample_frame_end, after C3D_FrameEnd, deletes the old one (citro3d waits for the GPU first).
static bool make_big(int lv)
{
	if (S.big)
		return S.big_level == lv;
	S.big = C3D_RenderTargetCreate(LEVEL[lv].w, LEVEL[lv].h, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!S.big)
		return false;
	S.big_level = lv;
	return true;
}

static bool make_soft(int i)
{
	C3D_Tex* tex = &S.soft[i].tex;
	if (!C3D_TexInitVRAM(tex, i ? 512 : 256, 512, GPU_RGBA8))
		return false;
	// the stencil is shadow.c's, as on the screen's target
	S.soft[i].rt = C3D_RenderTargetCreateFromTex(tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
	if (!S.soft[i].rt) {
		C3D_TexDelete(tex);
		return false;
	}
	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return true;
}

static void free_soft(int i)
{
	if (!S.soft[i].rt)
		return;
	C3D_RenderTargetDelete(S.soft[i].rt);   // the depth buffer; the colour is the texture's
	C3D_TexDelete(&S.soft[i].tex);
	S.soft[i].rt = NULL;
}

// The scene's texture i for this frame, or NULL: it is made after the frame, so the frame that
// first asks draws straight to the screen, sharp.
static C3D_RenderTarget* soft(int i)
{
	if (!S.soft[i].rt && !S.soft[i].failed)
		S.need |= 1 << i;
	return S.soft[i].rt;
}

static void show(C3D_RenderTarget* t, u32 flags)
{
	if (t != S.shown || flags != S.shown_flags) {
		C3D_RenderTargetSetOutput(t, GFX_TOP, GFX_LEFT, flags);
		S.shown = t;
		S.shown_flags = flags;
	}
}

C3D_RenderTarget* supersample_target(bool want)
{
	int lv = want ? S.level : 0;
	while (lv > 0) {
		if (S.big && S.big_level != lv && lv < S.limit) {
			S.swap = true;           // the other level's target: this frame at the screen's size
			lv = 0;
			break;
		}
		if (lv < S.limit && make_big(lv))
			break;
		if (lv < S.limit)
			S.limit = lv;            // could not be made: never asked for again
		lv--;
		if (want)
			S.level = lv;
	}
	C3D_RenderTarget* draw = lv > 0 ? S.big : S.top;
	show(draw, lv > 0 ? (S.flags & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(LEVEL[lv].scale) : S.flags);
	S.wanted |= want;
	if (lv > S.used)
		S.used = lv;
	return draw;
}

void supersample_top(bool want, PictureTarget* t)
{
	t->tex = NULL;
	t->sx = t->sy = 1;
	if (!picture_soft()) {
		t->scene = t->out = supersample_target(want);
		t->w = t->scene->frameBuf.width, t->h = t->scene->frameBuf.height;
		return;
	}
	show(S.top, S.flags);                // the pass brings the scene to the screen's own target
	S.wanted |= want;
	int lv = want && S.level > 0 ? 1 : 0;   // Soft goes up to 2x (VRAM: 3 MB for both scenes)
	if (lv && !soft(1))
		lv = 0;
	t->out = S.top;
	if (!soft(0) || picture_probe()) {
		t->scene = S.top;
		t->tex = S.soft[0].rt ? &S.soft[0].tex : NULL;   // picture_probe: the draws over the scene
		t->w = LEVEL[0].w, t->h = LEVEL[0].h;
		return;
	}
	t->scene = S.soft[lv].rt;
	t->tex = &S.soft[lv].tex;
	t->w = LEVEL[lv].w, t->h = LEVEL[lv].h;
	t->sx = t->w / LEVEL[0].w, t->sy = t->h / LEVEL[0].h;
	if (lv > S.used)
		S.used = lv;
}

void supersample_eye(int eye, C3D_RenderTarget* screen, PictureTarget* t)
{
	// one scene each, as both eyes' clears go ahead of the frame's drawing; neither eye when
	// either is missing, so the two never differ
	bool both = picture_soft() && soft(0) && soft(1);
	t->scene = both ? S.soft[eye].rt : screen;
	t->out = screen;
	t->tex = both ? &S.soft[eye].tex : NULL;
	t->w = LEVEL[0].w, t->h = LEVEL[0].h;
	t->sx = t->sy = 1;
}

static void drop_big(void)
{
	if (S.shown == S.big)
		show(S.top, S.flags);
	C3D_RenderTargetDelete(S.big);
	S.big = NULL;
	S.big_level = 0;
}

void supersample_frame_end(int fps, int cap)
{
	// outside the frame now: targets are only deleted here (make_big), and Soft's made here
	bool soft = picture_soft();
	if (S.swap || (soft && S.big)) {
		S.swap = false;
		if (S.big)
			drop_big();
	}
	for (int i = 0; i < 2; i++) {
		if (!soft)
			free_soft(i);
		else if ((S.need >> i & 1) && !S.soft[i].rt && !make_soft(i))
			S.soft[i].failed = true;
	}
	if (S.soft[1].failed && S.limit > 1)
		S.limit = 1;                     // no 2x on Soft (the right eye goes without too)
	S.need = 0;
	float gpu = C3D_GetDrawingTime();    // the frame before this one's
	int measured = S.drew[1];
	S.drew[1] = S.drew[0];
	S.drew[0] = S.used;
	bool wanted = S.wanted;
	S.wanted = false, S.used = 0;
	int mode = quality()->supersample;
	int most = soft ? 1 : 2;
	if (mode != 1) {                     // 0 off, 2 always 2x, 3 always 2x2 (debug.txt), Soft at most 2x
		S.level = mode == 2 ? 1 : mode == 3 ? most : 0;
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
	if (S.level > most)
		S.level = most;
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
	int top = S.limit - 1 < most ? S.limit - 1 : most;
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
