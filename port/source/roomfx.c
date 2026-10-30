#include "roomfx.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>
#include "demo.h"
#include "stereo.h"

// The PC's Effect2 classes for the burning stairs and the lake (see roomfx.h), from the Director's
// Cut exe. Class numbers are indexes into the class list at 0x893798 ({prefix, main, suffix,
// disable}, 57 classes, 0x4ca720); an instance is posted with an ImpactQueue_Element (0x4ca780):
// the class, a place (Vector[0]), two floats (a life time, 0 for no end, and a growth time) and an
// int (the parameter row). Every class runs once a game frame at 30 frames a second and adds
// 1/30 s (0x6bc164) to its own timers, so the port steps them in 1/30 s ticks.
//
// The flames (classes 47, 48 and 49, 0x4df190 / 0x4de770 / 0x4ddd60, the PS2 beta's
// Effect2/hh_class_flame_0x.c with its Flame_Initialize_Parameter) and the smoke (class 52,
// 0x4db0d0) are particle sprites: each particle starts somewhere in a box round the place, rises
// (a gravity term against Y down) and drifts, grows in alpha over the first quarter of its life
// and fades over the rest while it shrinks, then starts again (0x4de980, 0x4dec70). They are
// camera-facing quads (the inverse view rotation, 0x4ca050) drawn with alpha blending, the
// texture's colour doubled (D3DTOP_MODULATE2X) and fog off (0x4ec920, 0x4ecad0). Classes 47 and 49
// are byte-for-byte copies with copied tables; 48 grows to full size over its growth time.

#define TICK         (1.0f / 30.0f)        // 0x6bc164
#define EPS          4.9999999e-6f         // 0x69db3c: the classes' "is zero" test
#define MAX_INST     48
#define MAX_PART     100                   // HH_Object_Flame_03.Particle[100]
#define MAX_QUADS    2048
#define MAX_VERTS    (MAX_QUADS * 6 + 4)

typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;   // the world program's vertex

// Flame_Initialize_Parameter (0x5c bytes)
typedef struct {
	int n;                       // ParticleDraw_Num
	int flag[3];                 // which of the box's sides sets the radius the life is scaled by
	float conv, conv_sub;        // Convergence_Coff (sideways speed factor) and its change a tick
	float grav;                  // Gravity_Suppress_Coff (times -2450: the rise)
	float alpha, scale;          // Base_Alpha (128 = half, the vertex alpha), Base_Scale
	float base_time, rand_time;  // the life: base_time + rand_time nearer the box's centre
	int vel[3];                  // Verocity_0: start speeds -v/2..v/2
	int len[3];                  // Length_Max: the box
	float rgba[4];
} Param;

// 0x89d9c0 (class 47; 49's 0x89b180 and 48's 0x89c5a0 hold the same values), by Int_Value & 7
static const Param FLAME[8] = {
	{ 80, { 0, 0, 1 }, 1.0f, -0.03f, 0.18f, 80.0f, 190.0f, 0.4f, 1.8f, { 700, 100, 700 }, { 10, 250, 500 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
	{ 100, { 0, 0, 1 }, 1.0f, -0.035f, 0.3f, 90.0f, 210.0f, 0.4f, 1.8f, { 100, 100, 700 }, { 10, 350, 700 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
	{ 80, { 1, 0, 0 }, 1.0f, -0.03f, 0.18f, 80.0f, 190.0f, 0.4f, 1.8f, { 700, 100, 700 }, { 500, 250, 10 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
	{ 80, { 0, 1, 0 }, 1.0f, -0.03f, 0.2f, 100.0f, 170.0f, 0.4f, 1.8f, { 700, 100, 700 }, { 10, 600, 100 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
	{ 100, { 0, 1, 0 }, 1.0f, -0.03f, 0.2f, 80.0f, 180.0f, 0.4f, 2.0f, { 700, 100, 700 }, { 10, 700, 100 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
	{ 100, { 0, 1, 0 }, 1.0f, -0.03f, 0.25f, 90.0f, 190.0f, 0.4f, 2.0f, { 100, 100, 700 }, { 10, 600, 200 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
	{ 70, { 0, 0, 0 }, 1.0f, -0.03f, 0.15f, 128.0f, 120.0f, 0.4f, 1.8f, { 700, 100, 700 }, { 150, 200, 150 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
	{ 100, { 0, 0, 0 }, 1.0f, -0.03f, 0.3f, 100.0f, 200.0f, 0.4f, 1.8f, { 900, 300, 900 }, { 400, 300, 400 }, { 255.0f, 255.0f, 255.0f, 0.0f } },
};
// 0x8985b0 (class 52), by Int_Value & 3
static const Param SMOKE[4] = {
	{ 12, { 0, 0, 1 }, 1.0f, -0.01f, 0.09f, 90.0f, 600.0f, 1.5f, 1.8f, { 100, 100, 800 }, { 10, 250, 500 }, { 25.0f, 25.0f, 25.0f, 0.0f } },
	{ 12, { 1, 0, 0 }, 1.0f, -0.01f, 0.09f, 95.0f, 600.0f, 1.5f, 1.8f, { 800, 100, 100 }, { 500, 250, 10 }, { 20.0f, 20.0f, 20.0f, 0.0f } },
	{ 8, { 0, 1, 0 }, 1.0f, -0.03f, 0.1f, 80.0f, 600.0f, 0.4f, 1.8f, { 1000, 100, 1000 }, { 100, 150, 100 }, { 30.0f, 30.0f, 30.0f, 0.0f } },
	{ 12, { 0, 1, 0 }, 1.2f, -0.005f, 0.05f, 80.0f, 900.0f, 2.5f, 1.5f, { 500, 100, 500 }, { 100, 250, 100 }, { 30.0f, 30.0f, 30.0f, 0.0f } },
};

// The quads' corners (x right, y down on the screen): the flames' 0x89c880 stand on the particle,
// the smoke's 0x898060 are centred on it. Two triangles c0 c1 c3, c0 c3 c2 (0x4ded60).
static const float FLAME_CORNER[4][2] = { { -1, -2 }, { -1, 0 }, { 1, -2 }, { 1, 0 } };
static const float SMOKE_CORNER[4][2] = { { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 } };
// A sprite's four texture corners as the tables give them, by turn: 0 as drawn, 1 upside down,
// 2 mirrored across the diagonal, 3 turned half round. Values: which of (u0, u1) and (v0, v1).
static const u8 TURN[4][4][2] = {
	{ { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 } }, { { 0, 1 }, { 0, 0 }, { 1, 1 }, { 1, 0 } },
	{ { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } }, { { 1, 1 }, { 0, 1 }, { 1, 0 }, { 0, 0 } },
};

typedef struct {
	float loc[3];                // Location
	float vel[3], grav;          // Verocity_0 (its w: the rise term, Gravity_Suppress_Coff * -2450)
	float rgba[4];
	float time, scale, conv, life;
	int kind;                    // Texture_Kind
	float d[3];                  // this tick's drift (0x4deb30), kept for the draw
} Part;

typedef struct {
	bool on, ready;              // ready: stepped once (the PC draws from the second frame)
	int cls, st, id;
	float timer;
	float pos[3];                // Vector[0]
	float f0, f1;                // Float_Value: life time (0: none), growth time (class 48)
	int i0;                      // Int_Value[0]: the parameter row
	float ratio;                 // class 48's growth, 0..1
	float scale, bright;         // class 56
	int phase;
	Part part[MAX_PART];
} Inst;

// The director's rows (class 46): 0x89e130 is timed by the scene's frame (gCutsceneTimer), 0x89e808
// is posted when the scene is over. 19 dwords each: place, offset of the second instance, the two
// classes (0: none), start and end frame, the two floats, the two ints, then the row's state and
// the instances' handles.
typedef struct {
	float pos[3], off[3];
	int cls_a, cls_b;
	float start, end, f0, f1;
	int i_a, i_b;
} Row;

static const Row TIMED[23] = {
	{ { -100550.0f, -3600.0f, -74800.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 5, 2 },
	{ { -100550.0f, -3700.0f, -75600.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 1, 2 },
	{ { -100400.0f, 0.0f, -79400.0f }, { 0.0f, -700.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 7, 3 },
	{ { -100500.0f, -400.0f, -78700.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 1617.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100500.0f, -400.0f, -78700.0f }, { 0.0f, -500.0f, 0.0f }, 49, 52, 1615.0f, 2489.0f, 0.0f, 0.0f, 0, 2 },
	{ { -100500.0f, -400.0f, -78700.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 2488.0f, 10000.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100500.0f, -750.0f, -78450.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 1617.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100500.0f, -750.0f, -78450.0f }, { 0.0f, -500.0f, 0.0f }, 49, 52, 1615.0f, 2489.0f, 0.0f, 0.0f, 0, 2 },
	{ { -100500.0f, -750.0f, -78450.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 2488.0f, 10000.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100500.0f, -1200.0f, -78100.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 1617.0f, 0.0f, 0.0f, 4, 2 },
	{ { -100500.0f, -1200.0f, -77950.0f }, { 0.0f, -500.0f, 0.0f }, 49, 52, 1615.0f, 2489.0f, 0.0f, 0.0f, 4, 2 },
	{ { -100500.0f, -1200.0f, -78100.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 2488.0f, 10000.0f, 0.0f, 0.0f, 4, 2 },
	{ { -100500.0f, -2500.0f, -76800.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100550.0f, -2000.0f, -76400.0f }, { 100.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 4, 0 },
	{ { -99500.0f, -2800.0f, -76800.0f }, { 0.0f, -500.0f, 0.0f }, 47, 0, 0.0f, 10000.0f, 0.0f, 0.0f, 1, 0 },
	{ { -99500.0f, -2200.0f, -76100.0f }, { -250.0f, -500.0f, 0.0f }, 47, 0, 0.0f, 10000.0f, 0.0f, 0.0f, 5, 0 },
	{ { -99500.0f, -4000.0f, -74900.0f }, { 0.0f, -700.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 7, 3 },
	{ { -99800.0f, -2500.0f, -75700.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 6, 2 },
	{ { -100200.0f, -4500.0f, -73700.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 2, 1 },
	{ { -99750.0f, -5600.0f, -72600.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 10000.0f, 0.0f, 0.0f, 2, 1 },
	{ { -99500.0f, 0.0f, -78400.0f }, { 0.0f, -500.0f, 0.0f }, 48, 0, 0.0f, 10000.0f, 0.0f, 0.0f, 6, 2 },
	{ { -100300.0f, -910.0f, -77300.0f }, { 0.0f, -500.0f, 0.0f }, 48, 0, 4950.0f, 10000.0f, 0.0f, 3.5f, 6, 2 },
	{ { -99850.0f, -900.0f, -77300.0f }, { 0.0f, -500.0f, 0.0f }, 48, 0, 4950.0f, 10000.0f, 0.0f, 4.0f, 2, 2 },
};
static const Row FIXED[16] = {
	{ { -100550.0f, -3600.0f, -74800.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 5, 2 },
	{ { -100550.0f, -3700.0f, -75600.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 1, 2 },
	{ { -100500.0f, -400.0f, -78700.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100500.0f, -750.0f, -78450.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100500.0f, -1200.0f, -78100.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 4, 2 },
	{ { -100500.0f, -2500.0f, -76800.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0 },
	{ { -100550.0f, -2000.0f, -76400.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 4, 2 },
	{ { -99500.0f, -2800.0f, -76800.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 1, 2 },
	{ { -99500.0f, -2200.0f, -76100.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 5, 2 },
	{ { -99500.0f, -4000.0f, -74900.0f }, { 0.0f, -700.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 7, 3 },
	{ { -99800.0f, -2500.0f, -75700.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 6, 2 },
	{ { -100200.0f, -4500.0f, -73700.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 2, 1 },
	{ { -99750.0f, -5600.0f, -72600.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 2, 1 },
	{ { -99500.0f, 0.0f, -78400.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 6, 2 },
	{ { -100300.0f, -910.0f, -77300.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 6, 2 },
	{ { -99850.0f, -900.0f, -77300.0f }, { 0.0f, -500.0f, 0.0f }, 47, 52, 0.0f, 0.0f, 0.0f, 0.0f, 2, 1 },
};

// class 56 (0x4dbcd0): the line the boat crosses, from the near shore (0x8987d0) to the light
// (0x8987e0); the light is brightest (0x8987bc) between a tenth of the way (0x8987cc) and nine
// tenths (0x8987cc + 0x8987c8), fades in and out over the first and last tenth (0x8987c0), and
// grows from 1 to 4 times (0x8987c4) its quad (+-10, 0x898720) as the camera nears it.
static const float FLARE_FROM[3] = { -5426.0f, 1150.0f, -114753.0f };
static const float FLARE_AT[3] = { -12338.0f, 1150.0f, -19510.0f };

static RoomFxBindFn s_bind;
static C3D_Tex s_tex[3];                 // fire, smoke, flare
static bool s_tex_ok[3];
static V* s_verts[2];
static int s_buf, s_drawn;
static Inst s_inst[MAX_INST];
static int s_next_id = 1;
static int s_row_st[23], s_row_h[23][2], s_fix_st[16];
static float s_acc;
static int s_room = -1;
static bool s_reload = true;
static u32 s_seed = 1;

// the MSVC rand() the PC calls (0x4483b0)
static int pc_rand(void)
{
	s_seed = s_seed * 214013u + 2531011u;
	return (int)(s_seed >> 16 & 0x7fff);
}

static bool is_zero(float v) { return v >= -EPS && v <= EPS; }

// ---------------------------------------------------------------- instances

static int post(int cls, const float pos[3], float f0, float f1, int i0)
{
	if (cls == 0)
		return 0;
	for (int i = 0; i < MAX_INST; i++) {
		Inst* in = &s_inst[i];
		if (in->on)
			continue;
		in->on = true;
		in->ready = false;
		in->cls = cls, in->st = 0, in->id = s_next_id++;
		in->timer = 0.0f;
		memcpy(in->pos, pos, sizeof in->pos);
		in->f0 = f0, in->f1 = f1, in->i0 = i0;
		in->ratio = 1.0f;
		return in->id;
	}
	return 0;
}

static void remove_id(int id)            // 0x4ca7c0
{
	for (int i = 0; i < MAX_INST; i++)
		if (s_inst[i].on && s_inst[i].id == id)
			s_inst[i].on = false;
}

static void remove_class(int cls)        // 0x4ca7a0
{
	for (int i = 0; i < MAX_INST; i++)
		if (s_inst[i].on && s_inst[i].cls == cls)
			s_inst[i].on = false;
}

// ---------------------------------------------------------------- particles

// 0x4de980 (flames) / 0x4da930 (smoke): a new start in the box, a speed, and a life that is longer
// near the box's centre
static void respawn(Part* p, const Param* t, bool smoke)
{
	float r = (float)(t->len[0] + t->len[2]) * 0.5f;
	if (t->flag[0]) r = (float)t->len[0];
	if (t->flag[1]) r = (float)t->len[1];
	if (t->flag[2]) r = (float)t->len[2];
	r *= r;
	for (int k = 0; k < 3; k++)
		p->loc[k] = (float)(pc_rand() % t->len[k] - (t->len[k] >> 1));
	for (int k = 0; k < 3; k++)
		p->vel[k] = (float)(pc_rand() % t->vel[k] - (t->vel[k] >> 1));
	p->grav = t->grav * -2450.0f;    // 0x6bc1b8
	memcpy(p->rgba, t->rgba, sizeof p->rgba);
	p->time = 0.0f;
	p->scale = t->scale;
	p->conv = t->conv;
	p->kind = smoke ? (int)((u32)pc_rand() % 19u) : (pc_rand() & 0x3f);
	float q = 1.0f;
	if (!is_zero(r)) {
		q = (p->loc[0] * p->loc[0] + p->loc[1] * p->loc[1] + p->loc[2] * p->loc[2]) / r;
		if (q > 1.0f)
			q = 1.0f;
	}
	p->life = (1.0f - q) * t->rand_time + t->base_time;
}

// 0x4deb30: the drift so far, sideways at the start speed times the convergence factor, up and
// down at the start speed plus the rise
static void drift(Part* p)
{
	p->d[0] = p->vel[0] * p->conv * p->time;
	p->d[1] = p->vel[1] * p->time + p->grav * p->time * p->time;
	p->d[2] = p->vel[2] * p->conv * p->time;
}

// 0x4dec70 (47 / 49, r = 1) and 0x4de220 (48, r its growth): one tick of a flame particle
static void flame_step(Part* p, const Param* t, float r)
{
	if (p->time > p->life) {
		respawn(p, t, false);
		p->life *= r;                // 0x4de220 only: a growing flame starts its particles closer in
		for (int k = 0; k < 3; k++)
			p->loc[k] *= r;
	}
	float q = p->life * 0.25f;
	float inv = is_zero(p->life) ? 4294967296.0f : 1.0f / p->life;
	float es = 1.0f - inv * p->time;
	float ea = q <= p->time ? 1.0f - (p->time - q) * inv * 1.3333334f : p->time / q;
	drift(p);
	if (p->conv > 0.0f)
		p->conv += t->conv_sub;
	p->time += TICK;
	p->rgba[3] = ea * t->alpha * r;
	p->scale = es * t->scale * r;
}

// 0x4dac20: the smoke grows and darkens over its life, then starts again
static void smoke_step(Part* p, const Param* t)
{
	if (p->time > p->life)
		respawn(p, t, true);
	float f = is_zero(p->life) ? 1.0f : p->time / p->life;
	drift(p);
	if (p->conv > 0.0f)
		p->conv += t->conv_sub;
	p->time += TICK;
	p->rgba[3] = f * t->alpha;
	p->scale = f * t->scale;
}

static const Param* params(const Inst* in)
{
	return in->cls == 52 ? &SMOKE[in->i0 & 3] : &FLAME[in->i0 & 7];
}

// ---------------------------------------------------------------- classes

// 0x4debe0 / 0x4de190 / 0x4dd7b0 (flames), 0x4dab90 (smoke): the particles start at random points
// of their lives
static void particles_init(Inst* in)
{
	bool smoke = in->cls == 52;
	in->i0 &= smoke ? 3 : 7;
	const Param* t = params(in);
	for (int i = 0; i < t->n && i < MAX_PART; i++) {
		Part* p = &in->part[i];
		respawn(p, t, smoke);
		p->time = (float)(pc_rand() % 100) * 0.01f * p->life;
	}
}

// 0x4df190 / 0x4ddd60 / 0x4de770 / 0x4db0d0: state 0 sets up, 1 lives until its life time, 2 has
// none, anything else frees the instance
static void particle_main(Inst* in)
{
	if (in->st == 0) {
		in->timer = 0.0f;
		particles_init(in);
		in->st = is_zero(in->f0) ? 2 : 1;
		return;
	}
	if (in->st == 1) {
		if (in->f0 < in->timer)
			in->st = 3;
	} else if (in->st != 2) {
		in->on = false;
		return;
	}
	if (in->cls == 48)               // 0x4de770: Float_Value[1] is the time to full size
		in->ratio = (in->f1 < in->timer || is_zero(in->f1)) ? 1.0f : in->timer / in->f1;
	const Param* t = params(in);
	for (int i = 0; i < t->n && i < MAX_PART; i++) {
		if (in->cls == 52)
			smoke_step(&in->part[i], t);
		else
			flame_step(&in->part[i], t, in->cls == 48 ? in->ratio : 1.0f);
	}
	in->ready = true;
	in->timer += TICK;
}

// 0x4e1340 (class 46, room 0xaa): waits for a scene (0x934d94 & 0x40, set by DramaDemoMain while
// one runs), then posts and removes the rows of 0x89e130 by gCutsceneTimer (0x1f7d3b8); once the
// scene is over it removes classes 47, 48, 49 and 52 and posts the rows of 0x89e808 for good.
static void stairs_main(Inst* in)
{
	if (in->st == 0) {               // 0x4e1300
		memset(s_row_st, 0, sizeof s_row_st);
		memset(s_fix_st, 0, sizeof s_fix_st);
		in->st = 1;
		return;
	}
	bool scene = demo_number() >= 0;
	if (in->st == 1) {
		if (!scene)
			return;
		in->st = 2;
	} else if (in->st != 2) {
		in->on = false;
		return;
	}
	float t = demo_timer();
	for (int i = 0; i < 23; i++) {
		const Row* r = &TIMED[i];
		if (s_row_st[i] == 0 && r->start <= t) {
			float at[3] = { r->pos[0] + r->off[0], r->pos[1] + r->off[1], r->pos[2] + r->off[2] };
			s_row_h[i][0] = post(r->cls_a, r->pos, r->f0, r->f1, r->i_a);
			s_row_h[i][1] = post(r->cls_b, at, r->f0, r->f1, r->i_b);
			s_row_st[i] = 1;
		} else if (s_row_st[i] == 1 && r->end < t) {
			remove_id(s_row_h[i][0]);
			remove_id(s_row_h[i][1]);
			s_row_st[i] = 2;
		}
	}
	if (scene)
		return;
	remove_class(47);
	remove_class(48);
	remove_class(49);
	remove_class(52);
	for (int i = 0; i < 16; i++) {
		const Row* r = &FIXED[i];
		if (s_fix_st[i] != 0)
			continue;
		float at[3] = { r->pos[0] + r->off[0], r->pos[1] + r->off[1], r->pos[2] + r->off[2] };
		post(r->cls_a, r->pos, r->f0, r->f1, r->i_a);
		post(r->cls_b, at, r->f0, r->f1, r->i_b);
		s_fix_st[i] = 2;
	}
	in->st = 3;
}

// 0x4dbcd0 (class 56, room 0x0e), the part that runs every frame: how far along the line the camera
// is (0 at the light, 1 at the near shore) sets the light's brightness and size
static void flare_state(Inst* in, const float cam[3])
{
	float ab[3], ac[3];
	for (int k = 0; k < 3; k++)
		ab[k] = FLARE_AT[k] - FLARE_FROM[k], ac[k] = cam[k] - FLARE_AT[k];
	float len = sqrtf(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
	float dist = sqrtf(ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2]);
	float f = is_zero(len) ? 0.0f : dist / len;
	if (f >= 0.0f && f <= 0.1f)
		in->phase = 0;
	if (f > 0.1f && f < 0.9f)
		in->phase = 1;
	if (f >= 0.9f && f <= 1.0f)
		in->phase = 2;
	in->scale = (1.0f - f) * 3.0f + 1.0f;
	float k;
	switch (in->phase) {
	case 0:                          // the last tenth: fades out at the light
		k = 0.1f * len;
		k = is_zero(k) ? 1.0f : dist / k;
		in->bright = 100.0f * (k < 1.0f ? k : 1.0f);
		break;
	case 1:
		in->bright = 100.0f;
		break;
	case 2:                          // the first tenth: fades in
		k = 0.1f * len;
		k = is_zero(k) ? 1.0f : (len - dist) / k;
		in->bright = 100.0f * (k < 1.0f ? k : 1.0f);
		break;
	default:                         // beyond the near shore: no light (only its base alpha)
		in->bright = 0.0f;
		return;
	}
	in->phase = 5;
}

static void flare_main(Inst* in)
{
	if (in->st == 0) {
		in->scale = 2.2f;            // 0x8987a4
		in->phase = 0;
		in->bright = 0.0f;
		in->st = 1;
		return;
	}
	if (in->st != 1) {
		in->on = false;
		return;
	}
	in->ready = true;
}

// 0x4ca6d0 with the table at 0x8942c8: the rooms ported here
static void autopost(int room)
{
	static const float none[3];
	if (room == 0xaa)
		post(46, none, 0.0f, 0.0f, 0);
	if (room == 0x0e)
		post(56, none, 0.0f, 0.0f, 0);
}

// ---------------------------------------------------------------- setup

static bool load(C3D_Tex* tex, const char* pack_dir, const char* name)
{
	char path[128];
	snprintf(path, sizeof path, "%s/effect/%s.t3x", pack_dir, name);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return true;
}

// Port only, no PC counterpart (port/STATUS.md section 3): a soft additive glow that enemy.c posts
// for the frame (Pyramid Head behind the bars, red_bars_glow). A camera-facing quad of a 64x64
// alpha blob (1 - r^2)^3 made here, tested against the scene's depth without writing it, so the bars,
// the walls and the figure in front of it cut it, and fogged like the map.
static C3D_Tex s_glow_tex;
static bool s_glow_ok, s_glow_on;
static float s_glow_at[3], s_glow_r[2], s_glow_rgba[4];
static V* s_glow_v;                      // 4 sets of 4 vertices, one per draw in turn
static int s_glow_n;

static bool make_glow(void)
{
	if (!C3D_TexInit(&s_glow_tex, 64, 64, GPU_A8))
		return false;
	static const u8 m[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
	u8* d = (u8*)s_glow_tex.data;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			float u = (x + 0.5f) / 32.0f - 1.0f, v = (y + 0.5f) / 32.0f - 1.0f, a = 1.0f - (u * u + v * v);
			a = a < 0.0f ? 0.0f : a * a * a;
			d[((y >> 3) * 8 + (x >> 3)) * 64 + (m[x & 7] | m[y & 7] << 1)] = (u8)(a * 255.0f);
		}
	C3D_TexFlush(&s_glow_tex);
	C3D_TexSetFilter(&s_glow_tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_glow_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	s_glow_v = linearAlloc(16 * sizeof(V));
	return s_glow_v != NULL;
}

bool roomfx_init(const char* pack_dir, RoomFxBindFn bind)
{
	s_bind = bind;
	static const char* names[3] = { "fire", "smoke", "flare" };
	for (int i = 0; i < 3; i++)
		s_tex_ok[i] = load(&s_tex[i], pack_dir, names[i]);
	for (int i = 0; i < 2; i++)
		s_verts[i] = linearAlloc(MAX_VERTS * sizeof(V));
	s_glow_ok = make_glow();
	return s_tex_ok[0] && s_tex_ok[1] && s_tex_ok[2] && s_verts[0] && s_verts[1];
}

void roomfx_exit(void)
{
	for (int i = 0; i < 3; i++)
		if (s_tex_ok[i])
			C3D_TexDelete(&s_tex[i]);
	for (int i = 0; i < 2; i++)
		linearFree(s_verts[i]);
	memset(s_tex_ok, 0, sizeof s_tex_ok);
	s_verts[0] = s_verts[1] = NULL;
	if (s_glow_ok)
		C3D_TexDelete(&s_glow_tex);
	linearFree(s_glow_v);
	s_glow_v = NULL;
	s_glow_ok = s_glow_on = false;
}

void roomfx_glow(const float at[3], float half_w, float half_h, const float rgba[4])
{
	memcpy(s_glow_at, at, sizeof s_glow_at);
	memcpy(s_glow_rgba, rgba, sizeof s_glow_rgba);
	s_glow_r[0] = half_w, s_glow_r[1] = half_h;
	s_glow_on = true;
}

void roomfx_room_loaded(void) { s_reload = true; }

void roomfx_update(float dt, int room)
{
	if (s_reload || room != s_room) {    // 0x4c71b0: every instance goes, the room's classes come
		s_reload = false;
		s_room = room;
		memset(s_inst, 0, sizeof s_inst);
		s_acc = 0.0f;
		autopost(room);
	}
	if (dt <= 0.0f)
		return;
	s_acc += dt;
	int steps = 0;
	while (s_acc >= TICK * 0.999f && steps < 4) {
		s_acc -= TICK;
		steps++;
		for (int i = 0; i < MAX_INST; i++) {
			Inst* in = &s_inst[i];
			if (!in->on)
				continue;
			switch (in->cls) {
			case 46: stairs_main(in); break;
			case 47: case 48: case 49: case 52: particle_main(in); break;
			case 56: flare_main(in); break;
			default: in->on = false; break;
			}
		}
	}
	if (s_acc > TICK)
		s_acc = 0.0f;
}

// ---------------------------------------------------------------- drawing

static void begin(const C3D_Mtx* proj, const C3D_Mtx* view, C3D_Tex* tex, bool additive)
{
	s_bind(proj, view);
	C3D_DepthMap(true, -1.0f, 0.0f);
	// the flames are tested against the scene but leave the depth alone; the light lies in front
	// of everything
	C3D_DepthTest(!additive, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);    // D3DRS_FOGENABLE 0
	if (additive)                    // 0x4db440: SRCALPHA, ONE
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
	else                             // 0x4ec920 / 0x4ecad0: SRCALPHA, INVSRCALPHA
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
		               GPU_ONE_MINUS_SRC_ALPHA);
	// D3DTOP_MODULATE2X for the colour, D3DTOP_MODULATE for the alpha
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_TexBind(0, tex);
}

static void flush(V* first, V* end, GPU_Primitive_t prim)
{
	int n = (int)(end - first);
	if (n <= 0)
		return;
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, first, sizeof(V), 4, 0x3210);
	C3D_DrawArrays(prim, 0, n);
}

// the PC's colour bytes are ftol of the floats, low byte (0x56e3b0)
static u8 byte(float f) { return (u8)((int)f & 0xff); }

static V vert(const float p[3], float u, float v, const float rgba[4])
{
	return (V){ p[0], p[1], p[2], u, 1.0f - v, { byte(rgba[0]), byte(rgba[1]), byte(rgba[2]), byte(rgba[3]) },
	            { 0, -127, 0, 0 } };
}

// The texture corners of particle kind k: the flames' 64 kinds (0x89d8c0) are the sixteen cells of
// fire01's 4x4 grid in the four turns; the smoke's 19 (0x898560) are the four cells of
// playable0's lower right quarter in four turns and three plain puffs of its lower left.
static void sprite_uv(bool smoke, int k, float uv[4][2])
{
	float u0, v0, w = 0.248f;
	int turn;
	if (!smoke) {
		int c = k & 15;
		u0 = (float)(c >> 2) * 0.25f, v0 = (float)(c & 3) * 0.25f;
		turn = k >> 4 & 3;
	} else if (k < 16) {
		static const u8 order[4] = { 0, 1, 3, 2 };
		int c = k & 3;
		u0 = 0.5f + (float)(c >> 1) * 0.25f, v0 = 0.5f + (float)(c & 1) * 0.25f;
		turn = order[k >> 2];
	} else {
		static const float puff[3][2] = { { 0.25f, 0.875f }, { 0.375f, 0.75f }, { 0.375f, 0.875f } };
		u0 = puff[k - 16][0], v0 = puff[k - 16][1];
		w = 0.123f;
		turn = 0;
	}
	for (int i = 0; i < 4; i++) {
		uv[i][0] = u0 + (TURN[turn][i][0] ? w : 0.0f);
		uv[i][1] = v0 + (TURN[turn][i][1] ? w : 0.0f);
	}
}

// roomfx_glow's quad (port only): the colour is the vertex colour, the blob only its alpha, added
// as SRCALPHA, ONE
static void draw_glow(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	if (!s_glow_on || !s_glow_ok || !s_bind)
		return;
	s_glow_on = false;                   // posted again for the next frame (and the second eye)
	float rt[3] = { view->r[0].x, view->r[0].y, view->r[0].z };
	float dn[3] = { -view->r[1].x, -view->r[1].y, -view->r[1].z };
	static const float corner[4][2] = { { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 } };
	V* v = s_glow_v + 4 * (s_glow_n++ & 3);
	for (int n = 0; n < 4; n++) {
		float q[3];
		for (int k = 0; k < 3; k++)
			q[k] = s_glow_at[k] + corner[n][0] * rt[k] * s_glow_r[0] + corner[n][1] * dn[k] * s_glow_r[1];
		v[n] = vert(q, corner[n][0] * 0.5f + 0.5f, 0.5f - corner[n][1] * 0.5f, s_glow_rgba);
	}
	GSPGPU_FlushDataCache(v, 4 * sizeof(V));
	s_bind(proj, view);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	// the room's fog, still bound from the scene: black where the glow is used, so it fades the glow
	// with the distance as it does the map
	C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_TexBind(0, &s_glow_tex);
	flush(v, v + 4, GPU_TRIANGLE_STRIP);
}

void roomfx_draw(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	if (!s_bind || !s_verts[0] || !s_verts[1])
		return;
	s_buf ^= 1;
	V* base = s_verts[s_buf];
	V* v = base;
	V* end = base + MAX_QUADS * 6;
	// the inverse view's rows (0x4ca050): screen right and screen down in the world
	float rt[3] = { view->r[0].x, view->r[0].y, view->r[0].z };
	float dn[3] = { -view->r[1].x, -view->r[1].y, -view->r[1].z };
	for (int pass = 0; pass < 2; pass++) {       // the flames, then the smoke above them
		int tex = pass == 0 ? 0 : 1;
		if (!s_tex_ok[tex])
			continue;
		V* first = v;
		for (int i = 0; i < MAX_INST; i++) {
			const Inst* in = &s_inst[i];
			bool smoke = in->cls == 52;
			if (!in->on || !in->ready || (in->cls < 47 || in->cls > 52) || smoke != (pass == 1))
				continue;
			const Param* t = params(in);
			const float (*corner)[2] = smoke ? SMOKE_CORNER : FLAME_CORNER;
			for (int j = 0; j < t->n && j < MAX_PART && v + 6 <= end; j++) {
				const Part* p = &in->part[j];
				float c[3], q[4][3], uv[4][2];
				for (int k = 0; k < 3; k++)
					c[k] = in->pos[k] + p->loc[k] + p->d[k];
				for (int n = 0; n < 4; n++)
					for (int k = 0; k < 3; k++)
						q[n][k] = c[k] + (corner[n][0] * rt[k] + corner[n][1] * dn[k]) * p->scale;
				sprite_uv(smoke, p->kind, uv);
				v[0] = vert(q[0], uv[0][0], uv[0][1], p->rgba);
				v[1] = vert(q[1], uv[1][0], uv[1][1], p->rgba);
				v[2] = vert(q[3], uv[3][0], uv[3][1], p->rgba);
				v[3] = v[0];
				v[4] = v[2];
				v[5] = vert(q[2], uv[2][0], uv[2][1], p->rgba);
				v += 6;
			}
		}
		if (v > first) {
			begin(proj, view, &s_tex[tex], false);
			flush(first, v, GPU_TRIANGLES);
		}
	}
	s_drawn = (int)(v - base) / 6;
	if (v > base)
		GSPGPU_FlushDataCache(base, (u32)((u8*)v - (u8*)base));
	draw_glow(proj, view);
	C3D_DepthMap(true, -1.0f, 0.0f);
}

// 0x4db6d0: a quad of +-10 pixels times the size, turned with the camera's yaw (vc_work
// cam_mat_ang.y), at the light's place on the screen, at depth 0.01; colour (64, 64, 64), alpha the
// brightness plus 12 (0x6bc194). The PC's pixels are those of a 480-line picture; the port keeps the
// same share of the screen's height.
void roomfx_draw_flare(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	if (!s_bind || !s_tex_ok[2] || !s_verts[0])
		return;
	Inst* in = NULL;
	for (int i = 0; i < MAX_INST; i++)
		if (s_inst[i].on && s_inst[i].ready && s_inst[i].cls == 56)
			in = &s_inst[i];
	if (!in)
		return;
	// the camera's place and look direction from the view (rows: right, up, back; eye = -R^T t)
	float cam[3], fwd[3];
	for (int k = 0; k < 3; k++) {
		cam[k] = -(view->r[0].c[3 - k] * view->r[0].w + view->r[1].c[3 - k] * view->r[1].w +
		           view->r[2].c[3 - k] * view->r[2].w);
		fwd[k] = -view->r[2].c[3 - k];
	}
	flare_state(in, cam);
	C3D_Mtx clip;
	Mtx_Multiply(&clip, proj, view);
	C3D_FVec p = Mtx_MultiplyFVec4(&clip, FVec4_New(FLARE_AT[0], FLARE_AT[1], FLARE_AT[2], 1.0f));
	if (p.w < 0.0f)                  // behind the camera
		return;
	// The PC draws it on the screen, so the far plane never cuts it; the light is 80000 units away
	// at the start, so the port draws it at the same place on the screen 1000 units from the eye
	float at[3];
	float near = p.w > 1000.0f ? 1000.0f / p.w : 1.0f;
	for (int i = 0; i < 3; i++)
		at[i] = cam[i] + (FLARE_AT[i] - cam[i]) * near;
	if (stereo_eyes() == 2 && p.w > 1.0f) {
		// 3D (stereo.h, the port's own): 1000 units out the glow would float far in front of the
		// light, so in each eye it slides across the view (keeping its distance) onto the light's
		// own place in that eye's picture, which shows it at the light's depth
		const float rx[3] = { view->r[0].x, view->r[0].y, view->r[0].z };
		C3D_FVec a = Mtx_MultiplyFVec4(&clip, FVec4_New(at[0], at[1], at[2], 1.0f));
		C3D_FVec b = Mtx_MultiplyFVec4(&clip, FVec4_New(at[0] + rx[0], at[1] + rx[1], at[2] + rx[2], 1.0f));
		float gx = b.x / b.w - a.x / a.w, gy = b.y / b.w - a.y / a.w;    // one unit to the right
		float ex = p.x / p.w - a.x / a.w, ey = p.y / p.w - a.y / a.w;    // to the light
		float g2 = gx * gx + gy * gy, k = g2 > 0.0f ? (ex * gx + ey * gy) / g2 : 0.0f;
		for (int i = 0; i < 3; i++)
			at[i] += rx[i] * k;
	}
	p = Mtx_MultiplyFVec4(&clip, FVec4_New(at[0], at[1], at[2], 1.0f));
	// world units for one 480-line pixel at the light's distance: measure a step along the
	// screen's up direction in the 240-line picture
	float up[3] = { view->r[1].x, view->r[1].y, view->r[1].z };
	float d = fabsf(p.w) > 1.0f ? fabsf(p.w) : 1.0f;
	C3D_FVec p2 = Mtx_MultiplyFVec4(&clip, FVec4_New(at[0] + up[0] * d * 0.01f, at[1] + up[1] * d * 0.01f,
	                                                at[2] + up[2] * d * 0.01f, 1.0f));
	if (p2.w <= 0.0f)
		return;
	float dx = p2.x / p2.w - p.x / p.w, dy = p2.y / p2.w - p.y / p.w;
	float ndc = sqrtf(dx * dx + dy * dy);
	if (ndc <= 0.0f)
		return;
	float px = d * 0.01f / ndc / 240.0f;    // NDC 1 = 240 pixels of a 480-line picture
	float yaw = atan2f(fwd[0], fwd[2]);
	float cs = cosf(yaw), sn = sinf(yaw);
	float rt[3] = { view->r[0].x, view->r[0].y, view->r[0].z };
	float dn[3] = { -up[0], -up[1], -up[2] };
	// 0x4db670 holds the colour to 0..255
	float a = in->bright + 12.0f;
	a = a < 0.0f ? 0.0f : a > 255.0f ? 255.0f : a;
	const float rgba[4] = { 64.0f, 64.0f, 64.0f, a };
	static const float corner[4][2] = { { -10, -10 }, { -10, 10 }, { 10, -10 }, { 10, 10 } };
	static const float uv[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
	V* v = s_verts[s_buf] + MAX_QUADS * 6;   // the four spare vertices after the particles
	for (int n = 0; n < 4; n++) {
		// D3DXMatrixRotationZ on the scaled corner (row vector, Y down on the screen)
		float x = corner[n][0] * in->scale, y = corner[n][1] * in->scale;
		float rx = x * cs - y * sn, ry = x * sn + y * cs;
		float q[3];
		for (int k = 0; k < 3; k++)
			q[k] = at[k] + (rx * rt[k] + ry * dn[k]) * px;
		v[n] = vert(q, uv[n][0], uv[n][1], rgba);
	}
	GSPGPU_FlushDataCache(v, 4 * sizeof(V));
	begin(proj, view, &s_tex[2], true);
	flush(v, v + 4, GPU_TRIANGLE_STRIP);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
}

const char* roomfx_status(void)
{
	static char line[48];
	int n = 0;
	for (int i = 0; i < MAX_INST; i++)
		n += s_inst[i].on;
	snprintf(line, sizeof line, "roomfx room %d inst %d quads %d%s", s_room, n, s_drawn,
	         s_tex_ok[0] && s_tex_ok[1] && s_tex_ok[2] ? "" : " no texture");
	return line;
}
