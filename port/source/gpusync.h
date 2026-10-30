#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

// Every wait before freeing GPU memory goes through here: it returns once the GPU has done all the
// work queued so far (the last frame's command list and transfers). citro3d 1.7.1 has no public
// call for that; C3Di_RenderQueueWaitDone is the wait C3D_FrameBegin makes (gxCmdQueueWait with no
// timeout, then the finished queue stopped and cleared), so it is called outside a frame only,
// which every caller is. This used C3D_FrameSync, which only waits for the next refresh of both
// screens: a frame whose GPU work outlasts a refresh (18.6 ms in Wood Side's 2F hall with Soft at
// 2x) was still drawing when a door freed the old room's buffers and the new room was read into
// them, and a New 3DS froze on the next frame. Once the app is being closed from the HOME Menu the
// GPU's events no longer come and the wait would never end (the console froze on HOME, Close), so it
// is skipped then (a movie stopping on the way out included); main.c also sets app_closing when its
// loop ends.
extern bool app_closing;
void C3Di_RenderQueueWaitDone(void);
void perf_crumb(const char* step);

static inline void gpu_sync(void)
{
	if (!app_closing && !aptShouldClose()) {
		perf_crumb("gpu_sync: waits for the GPU");
		C3Di_RenderQueueWaitDone();
		perf_crumb("gpu_sync: done");
	}
}
