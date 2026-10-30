#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "picture.h"

// Supersampling on the top screen. A 3DS-only system: the PC draws at the monitor's resolution and
// has nothing like it, so nothing here is ported from the exe.
//
// The top screen is 400 x 240, which leaves every edge in steps and the ground shimmering where its
// texture is seen at a slant. A frame drawn in 2D can go into a larger target, twice the height
// (2x: 480 x 400 as the GPU stores it, 1.5 MB of VRAM) or twice both ways (2x2: 480 x 800, 3 MB),
// one at a time. With the Picture on Sharp the display transfer halves it into the screen,
// averaging each pair of rows or each 2 x 2 block. On Soft the scene always goes into a texture in
// VRAM (256 x 512, or 512 x 512 for 2x, 1 and 2 MB with their depth), which the soft pass (picture.h)
// reads at the screen's pixels, doing the averaging itself; Soft stops at 2x, as a 2x2 texture
// (4 MB) would not fit beside a 3D frame's right eye.
//
// Each step doubles the pixel work, so the level follows the GPU (quality.h supersample 1). The
// frame's budget is 16.7 ms at 60 fps and 33.3 at 30 (the Frame Rate the profile chose): after 2
// seconds of frames whose GPU time doubled would still fit 0.84 of it, one step up; a frame over
// 0.84 of it, one step down at once, before the frame pacing would give up a frame. A place where
// it keeps stepping down waits longer each time. When the pacing has dropped a 60 fps game to 30
// it goes back to the screen's size. 3D frames and movies draw at the screen's own size; the Old
// 3DS profile's low quality level has none of it.

void supersample_init(C3D_RenderTarget* top, u32 transfer);
// Inside a frame, before the top screen is drawn: the target to draw it into on Sharp, and for
// frames other than play's (title, menus, game over: want false, the screen's own target).
// want: this frame may use a larger one (a frame drawn once, in 2D).
C3D_RenderTarget* supersample_target(bool want);
// Inside a frame of play drawn in 2D: where its scene and its 2D go (picture.h), on Sharp or Soft.
void supersample_top(bool want, PictureTarget* t);
// The same for one eye of a 3D frame, whose 2D goes into screen; after supersample_target(false).
void supersample_eye(int eye, C3D_RenderTarget* screen, PictureTarget* t);
// After C3D_FrameEnd: fps the rate perf_pace set, cap the chosen one (quality.h frame_cap).
void supersample_frame_end(int fps, int cap);
// The target shown last and its display transfer flags (the frame dump).
C3D_RenderTarget* supersample_shown(u32* transfer);
const char* supersample_status(void);
