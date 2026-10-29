#pragma once
#include <3ds.h>
#include <citro3d.h>

// The Display tab's Picture (port/VISUALS.md, "Picture"), a 3DS-only system: the PC draws at the
// monitor's resolution and has nothing like it, so nothing here is ported from the exe.
//
// Soft (the default) passes the finished scene through a [1, 2, 1] filter each way before the 2D
// goes on top: the edges and the texture grain of a 400 x 240 picture softened to about what the
// PC's frame looks like scaled down to this size. Sharp leaves every pixel as the GPU drew it.
//
// After the scene the GPU's transfer engine copies its colours into a texture (a raw copy of the
// tiled buffer, queued after the scene in the same frame), and four quads draw it back with one
// bilinear tap each, half a pixel off diagonally, each blended over the ones before at 1/2, 1/3 and
// 1/4: the four taps' mean is the filter exactly. All on texture unit 0: citro3d 1.7.1's
// C3D_TexBind reads the texture's type for units 1 and 2 without a NULL check, so a unit there
// could never be released again (binding NULL faults on a console).
// The far edges, where a tap would leave the copy, take the copy as it is. From a supersampled
// scene (supersample.h) the same taps are the downscale too: they read the larger copy at the
// screen's pixels, which for 2 x 2 averages a 4 x 4 block, so the pass costs the same at any size.
// The 2D (messages, the grain, menus) is drawn after it and stays crisp, as the PC draws its text
// and noise after its own filters.

bool picture_init(void);            // after C3D_Init; false: no shader, the picture stays sharp
void picture_set_soft(bool on);
bool picture_soft(void);            // the soft pass runs (chosen, and ready)
// The copy's texture for a w x h scene (the GPU's x and y) made ahead of the frame that needs it:
// supersample.c asks before it draws a larger scene that only this pass can bring to the screen.
bool picture_reserve(int w, int h);
// Inside a frame, after the scene and before the 2D on top: the scene in scene (sx, sy times the
// size of out along the target's x and y) copied out and drawn softened into out, the screen's
// target, which is left bound for the 2D.
void picture_soft_pass(C3D_RenderTarget* scene, int sx, int sy, C3D_RenderTarget* out);
// debug.txt "picture N": 0 the option decides, 1 the copy drawn back as it is (a check that the
// pass reproduces the scene exactly).
void picture_debug(int mode);
