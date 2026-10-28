#pragma once
#include <3ds.h>
#include <citro3d.h>

// Stereoscopic 3D on the top screen. A 3DS-only system: the PC game draws one view and has no
// counterpart, so nothing here is ported from the exe. The depth settings are in stereo.c.
//
// A frame that shows the world draws it twice, once per eye, each into its own render target, with
// Mtx_PerspStereoTilt in place of the PC camera's projection. The view matrix, the lights and every
// simulation stay as they are: only the projection differs, so the eyes always agree. The 2D layers
// (text, menus, fades, the noise, the fog's screen layer, pictures) are drawn into both eyes alike
// and sit at the screen plane. Frames without the world (title, movies, results) draw one image,
// which citro3d shows to both eyes.
//
// Off (the Options setting, the 3D slider at 0, a 2DS) the display stays in 2D mode, nothing extra
// is allocated or drawn and the frame is the same as before.

// After the top target is set up; transfer: its display transfer flags.
void stereo_init(u32 transfer);
// Once a frame, before C3D_FrameBegin. on: the Options setting; world: this frame draws the 3D
// world. Sets the display's 3D mode and returns how many eyes to draw (1 or 2).
int stereo_frame(bool on, bool world);
// The render target of one eye of a 2-eye frame; left: the top screen's own.
C3D_RenderTarget* stereo_target(int eye, C3D_RenderTarget* left);
// One eye's projection (0 left, 1 right) for the camera's fovy, near and far. focus: the distance
// from the eye to what the camera looks at; the screen plane goes a third of the way to it.
void stereo_projection(C3D_Mtx* out, int eye, float fovy, float near, float far, float focus);
// The eye being drawn: 1 while the right eye is, else 0. Code that advances state while drawing
// (animations, simulations, buffers filled once a frame) does it on eye 0 only.
int stereo_eye(void);
void stereo_set_eye(int eye);
// The eyes this frame draws (stereo_frame's answer).
int stereo_eyes(void);
const char* stereo_status(void);
