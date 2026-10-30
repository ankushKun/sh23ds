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
// The scene is drawn into a texture in VRAM (supersample.h makes them), and four quads draw it into
// the screen's target with one bilinear tap each, half a pixel off diagonally, each blended over
// the ones before at 1/2, 1/3 and 1/4: the four taps' mean is the filter exactly. It is the item
// screen's way (itemview.c), all in the frame's one command list: a mid-frame C3D_FrameSplit with a
// GX_TextureCopy of the scene froze a New 3DS on the first frame of play, though Azahar ran it. All
// on texture unit 0: citro3d 1.7.1's C3D_TexBind reads the texture's type for units 1 and 2 without
// a NULL check, so a unit there could never be released again (binding NULL faults on a console).
// The far edges, where a tap would leave the scene, take it as it is. From a supersampled scene
// (supersample.h) the same taps are the downscale too: they read the larger texture at the
// screen's pixels, so the pass costs the same at any size.
// The 2D (messages, the grain, menus) is drawn after it and stays crisp, as the PC draws its text
// and noise after its own filters.

// Where a frame of play's top screen goes: the 3D into scene (viewport w x h from its origin), the
// 2D into out, the screen's target. tex: scene's texture, which the pass reads at sx x sy of its
// pixels per pixel of out; NULL when there is no pass, and then scene is out.
typedef struct {
	C3D_RenderTarget *scene, *out;
	C3D_Tex* tex;
	int w, h, sx, sy;
} PictureTarget;

bool picture_init(void);            // after C3D_Init; false: no shader, the picture stays sharp
void picture_set_soft(bool on);
bool picture_soft(void);            // the soft pass runs (chosen, and ready)
// Inside a frame, after the scene and before the 2D on top: t's scene drawn softened into t->out,
// which is left bound for the 2D.
void picture_soft_pass(const PictureTarget* t);
// debug.txt "picture N": 0 the option decides, 1 the scene drawn back as it is (a check that the
// pass reproduces it exactly).
void picture_debug(int mode);
