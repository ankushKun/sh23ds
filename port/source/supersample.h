#pragma once
#include <3ds.h>
#include <citro3d.h>

// Supersampling on the top screen. A 3DS-only system: the PC draws at the monitor's resolution and
// has nothing like it, so nothing here is ported from the exe.
//
// The top screen is 400 x 240, which leaves every edge in steps and the ground shimmering where its
// texture is seen at a slant. A frame drawn in 2D can go into a target of twice the height (480 x
// 400 as the GPU stores it, 1.5 MB of VRAM made the first time it is used) that the display
// transfer halves into the screen, averaging each pair of rows: the edges that run across the
// screen (the ground's, the tops of walls and tables) are smoothed and the ground's texture is
// sampled twice as often down the screen, where it is squeezed most. The GPU does twice the pixel
// work for it, so it runs only while it has the time (quality.h supersample 1): from 2 seconds of
// frames whose GPU time would still fit in a 60 fps frame doubled, off as soon as one frame drawn
// this way takes more than 13.5 ms, before the frame pacing would drop to 30. A place where it keeps
// switching off waits longer each time. 3D frames, the 30 fps rate and the Old 3DS profile draw at
// the screen's own size.

void supersample_init(C3D_RenderTarget* top, u32 transfer);
// Inside a frame, before the top screen is drawn: the target to draw it into, which is also made
// the one shown. want: this frame may use the doubled target (a frame drawn once, at 60 fps).
// Frames drawn elsewhere (movies, loading screens) ask with want false.
C3D_RenderTarget* supersample_target(bool want);
// After C3D_FrameEnd, with the rate perf_pace set: decides the next frames from the GPU's time.
void supersample_frame_end(int fps);
// The target shown last and its display transfer flags (the frame dump).
C3D_RenderTarget* supersample_shown(u32* transfer);
const char* supersample_status(void);
