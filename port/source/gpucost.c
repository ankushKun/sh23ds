#include "gpucost.h"
#include "perf.h"
#include <stdio.h>

// The GPU cost model (gpucost.h). 3DS-only: no PC counterpart.

#define SCREEN_PX (400.0f * 240.0f)

static const char* const NAMES[GC_N] = {
	"map", "map_blend", "chr", "shvol", "shquad", "drop", "water", "fog", "fogscr", "full",
};

static struct {
	bool on;
	double px[GC_N];
	u32 verts[GC_N], count[GC_N];
	u32 frames, eye_frames;
	u32 tick;                    // frames counted, for the map's sampling
	float far;                   // the projection's far plane (clip w), gpucost_far
	double tris[GC_N], shown[GC_N];   // triangles sent, and those with pixels on the screen
	double idle;                 // map triangles in batches with nothing on the screen
} G;

// The map has tens of thousands of triangles: it is counted on one frame in SAMPLE, times SAMPLE,
// so that the counting does not slow the emulator to a crawl
#define SAMPLE 8

void gpucost_enable(bool on) { G.on = on; }
void gpucost_far(float far) { G.far = far; }
bool gpucost_on(void) { return G.on; }

typedef struct { float x, y, w; } HV;

static HV xform(const C3D_Mtx* m, const float* p)
{
	const C3D_FVec* r = m->r;
	return (HV){ r[0].x * p[0] + r[0].y * p[1] + r[0].z * p[2] + r[0].w,
	             r[1].x * p[0] + r[1].y * p[1] + r[1].z * p[2] + r[1].w,
	             r[3].x * p[0] + r[3].y * p[1] + r[3].z * p[2] + r[3].w };
}

// distance to one of the planes w > 1 (in front of the eye), x <= w, -x <= w, y <= w, -y <= w
static float plane(const HV* v, int k)
{
	switch (k) {
	case 0: return v->w - 1.0f;
	case 1: return v->w - v->x;
	case 2: return v->w + v->x;
	case 3: return v->w - v->y;
	default: return v->w + v->y;
	}
}

bool gpucost_tri(int pass, const C3D_Mtx* clip, const float* a, const float* b, const float* c, bool cull)
{
	if (!G.on)
		return false;
	float weight = 1.0f;
	if (pass == GC_MAP || pass == GC_MAP_BLEND) {
		if (G.tick % SAMPLE)
			return false;
		weight = SAMPLE;
	}
	G.verts[pass] += (u32)(3 * weight);
	G.tris[pass] += weight;
	HV poly[2][12];
	int n = 3;
	poly[0][0] = xform(clip, a), poly[0][1] = xform(clip, b), poly[0][2] = xform(clip, c);
	int cur = 0;
	for (int k = 0; k < 5 && n >= 3; k++) {
		HV* in = poly[cur];
		HV* out = poly[cur ^ 1];
		int m = 0;
		for (int i = 0; i < n; i++) {
			const HV* p = &in[i];
			const HV* q = &in[(i + 1) % n];
			float dp = plane(p, k), dq = plane(q, k);
			if (dp >= 0.0f && m < 12)
				out[m++] = *p;
			if ((dp >= 0.0f) != (dq >= 0.0f) && m < 12) {
				float t = dp / (dp - dq);
				out[m++] = (HV){ p->x + (q->x - p->x) * t, p->y + (q->y - p->y) * t, p->w + (q->w - p->w) * t };
			}
		}
		n = m;
		cur ^= 1;
	}
	if (n < 3)
		return false;
	if (G.far > 0.0f) {          // wholly beyond the far plane
		bool beyond = true;
		for (int i = 0; i < n && beyond; i++)
			beyond = poly[cur][i].w > G.far;
		if (beyond)
			return false;
	}
	float area = 0.0f;
	const HV* p = poly[cur];
	for (int i = 0; i < n; i++) {
		const HV* q = &p[(i + 1) % n];
		area += (p[i].x / p[i].w) * (q->y / q->w) - (q->x / q->w) * (p[i].y / p[i].w);
	}
	area *= 0.5f;
	if (cull && area < 0.0f)
		return false;
	G.shown[pass] += weight;
	// clip units: 2 x 2 across the screen
	G.px[pass] += (double)(area < 0.0f ? -area : area) * (SCREEN_PX / 4.0f) * weight;
	return true;
}

bool gpucost_sampled(void) { return G.on && G.tick % SAMPLE == 0; }
void gpucost_idle_batch(u32 tris) { G.idle += (double)tris * SAMPLE; }

void gpucost_px(int pass, float px, u32 verts)
{
	if (!G.on)
		return;
	G.px[pass] += px;
	G.verts[pass] += verts;
}

void gpucost_count(int pass, u32 n)
{
	if (G.on)
		G.count[pass] += n;
}

void gpucost_frame(int eyes, const char* where)
{
	if (!G.on)
		return;
	G.frames++;
	G.tick++;
	G.eye_frames += eyes;
	if (G.frames < 60)
		return;
	char line[400];
	int len = snprintf(line, sizeof line, "eyes %.1f per eye: screens (k verts)", (float)G.eye_frames / G.frames);
	double total = 0.0;
	for (int i = 0; i < GC_N; i++) {
		double s = G.px[i] / SCREEN_PX / G.eye_frames;
		float kv = (float)G.verts[i] / G.eye_frames / 1000.0f;
		total += s;
		if ((s > 0.005 || kv > 0.05f) && len < (int)sizeof line - 40)
			len += snprintf(line + len, sizeof line - len, "  %s %.2f (%.1f)", NAMES[i], s, kv);
		if (G.tris[i] > 0.0 && i == GC_MAP && len < (int)sizeof line - 20)
			len += snprintf(line + len, sizeof line - len, " shown %.0f%% idle batches %.0f%%", 100.0 * G.shown[i] / G.tris[i],
			                100.0 * G.idle / G.tris[i]);
		if (G.count[i] && len < (int)sizeof line - 20)
			len += snprintf(line + len, sizeof line - len, " n%.0f", (float)G.count[i] / G.eye_frames);
	}
	if (len < (int)sizeof line - 20)
		snprintf(line + len, sizeof line - len, "  total %.2f", total);
	perf_event(where, "gpucost %s", line);
	for (int i = 0; i < GC_N; i++)
		G.px[i] = 0.0, G.verts[i] = 0, G.count[i] = 0, G.tris[i] = G.shown[i] = 0.0;
	G.frames = G.eye_frames = 0;
	G.idle = 0.0;
}
