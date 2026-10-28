#include "stereo.h"
#include <math.h>
#include <stdio.h>

// Stereoscopic 3D (stereo.h). 3DS-only: no PC function to port.
//
// citro3d's Mtx_PerspStereoTilt(fovy, aspect, near, far, iod, screen) puts the eyes at -iod / +iod
// and, with ta = tan(fovy / 2) * aspect, shifts a point at depth d by iod / (2 d) - iod / (2 screen ta)
// in clip units across the 400 pixels (200 pixels a unit): zero at d = screen * ta. So for a screen
// plane at the focus distance F and a separation of P pixels between the eyes for the farthest
// points, iod = P * F / 200 and screen = F / ta. A point at depth d is then P * (1 - F / d) pixels
// apart: behind the screen it tends to P, in front of it the separation grows as F / d.
//
// The screen plane goes a third of the way to what the camera looks at: the PC camera's watch
// target (gamecam_watch, usually James) in play, the scene camera's target in cutscenes. The subject
// then sits behind the screen (two thirds of P), the room goes back from it, and only what is nearer
// than a third of the way comes out: in SH2's fixed shots that is mostly the walls, fittings and
// ceilings right beside the camera, which came out by up to twice P at the screen's edges with the
// screen at the subject itself (and 13 to 20 pixels for the urinals beside the first camera of
// the restroom with the screen halfway). Following the subject keeps the depth alike in the wide
// shots and the close-ups, which a fixed screen distance cannot (the cameras run from a few hundred
// units to tens of thousands).
//
// The depth, P at the slider's top, is 11.5 pixels (10 at first, 15 % more after a console run):
// about half a degree at a normal viewing distance, inside the usual comfort limit of one degree, and
// small enough that the dark, high-contrast scenes (the flashlight on black) do not ghost much
// through the parallax barrier. The slider scales it down to nothing.
#define FAR_PX 11.5f
#define SCREEN_AT (1.0f / 3.0f) // the screen plane's share of the distance to the subject
#define FOCUS_MIN 200.0f         // the screen plane no nearer than this (close-ups of props)
#define FOCUS_MAX 6000.0f        // nor farther (the town's far shots)

static struct {
	u32 transfer;
	C3D_RenderTarget* right;
	bool failed;                 // the right target could not be made (VRAM): stays 2D
	bool mode3d;                 // the display is in 3D mode (gfxSet3D)
	int eye, eyes;
	bool on;
	float slider;
	char status[40];
} S;

void stereo_init(u32 transfer) { S.transfer = transfer; }

int stereo_frame(bool on, bool world)
{
	S.eye = 0;
	S.on = on;
	S.slider = on ? osGet3DSliderState() : 0.0f;   // 0 with the slider down and on a 2DS
	bool want = S.slider > 0.0f && !S.failed;
	if (want && !S.right) {
		// made the first time it is needed, so a 2DS or a player with 3D off never spends the VRAM
		// (750 KB: colour and depth, the same formats as the left target)
		S.right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		if (S.right)
			C3D_RenderTargetSetOutput(S.right, GFX_TOP, GFX_RIGHT, S.transfer);
		else
			S.failed = true, want = false;
	}
	if (want != S.mode3d) {
		gfxSet3D(want);
		S.mode3d = want;
	}
	S.eyes = want && world ? 2 : 1;
	return S.eyes;
}

C3D_RenderTarget* stereo_target(int eye, C3D_RenderTarget* left)
{
	return eye ? S.right : left;
}

void stereo_projection(C3D_Mtx* out, int eye, float fovy, float near, float far, float focus)
{
	float f = focus * SCREEN_AT;
	f = f < FOCUS_MIN ? FOCUS_MIN : f > FOCUS_MAX ? FOCUS_MAX : f;
	float ta = tanf(fovy * 0.5f) * C3D_AspectRatioTop;
	float iod = FAR_PX * S.slider * f / 200.0f;
	Mtx_PerspStereoTilt(out, fovy, C3D_AspectRatioTop, near, far, eye ? iod : -iod, f / ta, false);
}

int stereo_eye(void) { return S.eye; }
int stereo_eyes(void) { return S.eyes ? S.eyes : 1; }
void stereo_set_eye(int eye) { S.eye = eye; }
const char* stereo_status(void)
{
	snprintf(S.status, sizeof S.status, "3d %s slider %.2f", !S.on ? "off" : S.failed ? "no vram" :
	         S.eyes == 2 ? "2 eyes" : "1 eye", S.slider);
	return S.status;
}
