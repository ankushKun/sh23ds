#include "supersample.h"
#include "quality.h"
#include <stdio.h>

// supersample.h says what this is. The numbers are the frame budget's: a 60 fps frame has 16.7 ms
// and perf_pace drops to 30 after frames of more than 15.5 ms of work.
#define ON_GPU_MS 7.0f           // a frame at the screen's size this fast leaves room for twice the pixels
#define ON_FRAMES 120            // for 2 seconds in a row
#define OFF_GPU_MS 14.0f         // a doubled frame slower than this switches it off at once
#define HOLD_FRAMES 300          // then 5 seconds before it may come back
#define HOLD_MAX 3600            // doubling up to a minute when it went off again within 10 seconds
#define SHORT_RUN 600

static struct {
	C3D_RenderTarget *top, *big, *shown;
	u32 flags, big_flags;
	bool failed;                 // no VRAM for the doubled target: stays off
	bool on;                     // the next frames that may are drawn doubled
	bool wanted, used;           // this frame asked for it, and drew doubled
	bool drew[2];                // the last two frames drew doubled (the GPU time read is the one before)
	int fit, hold, backoff, run;
	char status[48];
} S;

void supersample_init(C3D_RenderTarget* top, u32 transfer)
{
	S.top = S.shown = top;
	S.flags = transfer;
	S.big_flags = (transfer & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_X);
	S.backoff = HOLD_FRAMES;
}

C3D_RenderTarget* supersample_target(bool want)
{
	bool use = want && S.on && !S.failed;
	if (use && !S.big) {
		// the same formats as the screen's target, twice its 240 rows (the GPU's x)
		S.big = C3D_RenderTargetCreate(480, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		if (!S.big)
			S.failed = true, use = false;
	}
	C3D_RenderTarget* t = use ? S.big : S.top;
	if (t != S.shown) {
		C3D_RenderTargetSetOutput(t, GFX_TOP, GFX_LEFT, use ? S.big_flags : S.flags);
		S.shown = t;
	}
	S.wanted |= want;
	S.used |= use;
	return t;
}

void supersample_frame_end(int fps)
{
	float gpu = C3D_GetDrawingTime();    // the frame before this one's
	bool measured_big = S.drew[1];
	S.drew[1] = S.drew[0];
	S.drew[0] = S.used;
	bool wanted = S.wanted;
	S.wanted = S.used = false;
	int mode = quality()->supersample;
	if (mode >= 2 || mode <= 0) {
		S.on = mode >= 2;
		S.fit = 0;
		return;
	}
	if (S.hold > 0)
		S.hold--;
	if (fps != 60 || !wanted) {          // 30 fps, 3D or a movie: the screen's own size
		S.fit = 0;
		if (fps != 60)
			S.on = false;
		return;
	}
	if (S.on) {
		S.run++;
		if (measured_big && gpu > OFF_GPU_MS) {
			S.on = false;
			S.fit = 0;
			S.backoff = S.run < SHORT_RUN ? (S.backoff * 2 > HOLD_MAX ? HOLD_MAX : S.backoff * 2) : HOLD_FRAMES;
			S.hold = S.backoff;
		}
		return;
	}
	S.fit = !measured_big && gpu < ON_GPU_MS ? S.fit + 1 : 0;
	if (S.fit >= ON_FRAMES && !S.hold) {
		S.on = true;
		S.run = 0;
	}
}

C3D_RenderTarget* supersample_shown(u32* transfer)
{
	*transfer = S.shown == S.big ? S.big_flags : S.flags;
	return S.shown;
}

const char* supersample_status(void)
{
	snprintf(S.status, sizeof S.status, "2x %s%s", S.failed ? "no vram" : S.drew[0] ? "on" : "off",
	         S.hold ? " (wait)" : "");
	return S.status;
}
