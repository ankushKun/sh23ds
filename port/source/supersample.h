#pragma once
#include <3ds.h>
#include <citro3d.h>

// Supersampling on the top screen. A 3DS-only system: the PC draws at the monitor's resolution and
// has nothing like it, so nothing here is ported from the exe.
//
// The top screen is 400 x 240, which leaves every edge in steps and the ground shimmering where its
// texture is seen at a slant. A frame drawn in 2D can go into a larger target, twice the height
// (2x: 480 x 400 as the GPU stores it, 1.5 MB of VRAM) or twice both ways (2x2: 480 x 800, 3 MB),
// one at a time. With the Picture on Sharp the display transfer halves it into the screen,
// averaging each pair of rows or each 2 x 2 block; on Soft the soft pass (picture.h) reads it at
// the screen's pixels and does the averaging itself.
//
// Each step doubles the pixel work, so the level follows the GPU (quality.h supersample 1). The
// frame's budget is 16.7 ms at 60 fps and 33.3 at 30 (the Frame Rate the profile chose): after 2
// seconds of frames whose GPU time doubled would still fit 0.84 of it, one step up; a frame over
// 0.84 of it, one step down at once, before the frame pacing would give up a frame. A place where
// it keeps stepping down waits longer each time. When the pacing has dropped a 60 fps game to 30
// it goes back to the screen's size. 3D frames and movies draw at the screen's own size; the Old
// 3DS profile's low quality level has none of it.

void supersample_init(C3D_RenderTarget* top, u32 transfer);
// Inside a frame, before the top screen is drawn: the target to draw the scene into. want: this
// frame may use a larger one (a frame drawn once, in 2D). Frames drawn elsewhere (movies, loading
// screens) ask with want false.
C3D_RenderTarget* supersample_target(bool want);
// The screen's target the 2D goes into after the scene drawn into t, and how many of t's pixels
// make one of its along the GPU's x and y: the screen's own target and 2 x 2 for a 2x2 scene, t and
// 1 x 1 for one drawn at the screen's size (or an eye's of a 3D frame).
C3D_RenderTarget* supersample_screen(C3D_RenderTarget* t, int* sx, int* sy);
// After C3D_FrameEnd: fps the rate perf_pace set, cap the chosen one (quality.h frame_cap).
void supersample_frame_end(int fps, int cap);
// The target shown last and its display transfer flags (the frame dump).
C3D_RenderTarget* supersample_shown(u32* transfer);
const char* supersample_status(void);
