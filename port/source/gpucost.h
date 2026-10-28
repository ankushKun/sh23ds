#pragma once
#include <3ds.h>
#include <citro3d.h>

// A cost model of the GPU's work, for tuning the 3D frames (port/PERFORMANCE.md, "3D"). Azahar does
// not time the PICA, so with debug.txt "gpucost 1" the passes count what the GPU will rasterize: the
// screen area of every triangle they send (clipped to the screen, after back face culling where the
// pass culls), in pixels, and the vertices. Every 60 frames perf.txt gets a "gpucost" line with each
// pass's area per eye in screens (400 x 240 pixels) and its vertices. 3DS-only: nothing ported.
//
// The CPU work of counting is large (every map triangle is projected), so CPU timings are taken with
// it off.

enum {
	GC_MAP,            // the map's opaque group (fragment lit, textured)
	GC_MAP_BLEND,      // the map's decals and transparent parts (blended)
	GC_CHR,            // characters and stage models (vertices only: skinned on the GPU)
	GC_SHADOW_VOL,     // the stencil shadow volumes, both passes (no colour writes)
	GC_SHADOW_QUAD,    // the shadow's darkening quad (stencil tested, blended)
	GC_DROP,           // drop shadows (blended)
	GC_WATER,          // the lake surfaces (opaque or blended, untextured)
	GC_FOG_SPRITE,     // the fog's sprites and puffs (blended, textured, procedural texture)
	GC_FOG_SCREEN,     // the fog's screen layer (blended, textured)
	GC_FULL,           // other full-screen quads: fades, the red cross, the noise, pictures
	GC_N
};

void gpucost_enable(bool on);
bool gpucost_on(void);
// The camera's far plane, so that triangles wholly beyond it count as not shown.
void gpucost_far(float far);
// One triangle in world space (or whatever clip maps from); clip: projection * modelview. cull: the
// pass culls back faces (GPU_CULL_BACK_CCW), so only counter-clockwise ones count. True when it has
// pixels on the screen.
bool gpucost_tri(int pass, const C3D_Mtx* clip, const float* a, const float* b, const float* c, bool cull);
// The map is counted on sampled frames only (gpucost_tri skips the rest); a map batch of which no
// triangle showed adds its triangles here, the share a per-batch cull would save.
bool gpucost_sampled(void);
void gpucost_idle_batch(u32 tris);
// Pixels drawn without triangles to project (a full-screen quad is 96000), and vertices.
void gpucost_px(int pass, float px, u32 verts);
// The sprites a pass drew (fog), for the line.
void gpucost_count(int pass, u32 n);
// After the frame is drawn: eyes, 1 or 2. Logs every 60 frames through perf_event.
void gpucost_frame(int eyes, const char* where);
