#include "fog.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tex3ds.h>
#include "fog_shbin.h"

// The PC fog particle layer, ported from the Director's Cut exe (static analysis; the PS2 beta's
// Fog/fog.c gives the names and the FOG_WORK layout, which the PC keeps at 0x1f63b20..0x1f7d2cc).
// PC addresses in the comments. The PC runs it at 30 frames a second with SpeedLevel = 2; here it
// runs every frame with SpeedLevel = dt * 60, as the Enhanced Edition's 60 fps mode does.
//
// What the port leaves out: the wall test (0x4864a0). The PC builds the wall normals with an
// unnormalised cross product (0x485f00 -> 0x447f40) and compares the distance against a few
// hundred units, so walls only act on particles lying exactly in their plane; the port moves
// every particle like the PC does without walls. The fog blow points (0x48a320, flag 0x100) and
// the per-cell object masks (a broad phase for the object push) are left out too; the push itself
// is the PC's.
//
// The 3DS draws at most FOG_MAX particles. Where an area asks for more, the extra particles are
// not simulated and the drawn ones get the missing ones' share of alpha, so the fog stays as
// dense while the GPU fills fewer, slightly stronger sprites.
//
// Enhanced Edition fixes (Silent Hill 2: Enhanced Edition, zlib licence), on by default as in EE:
// - "FogFix" (Patches/FogFix.cpp): the screen layer uses alpha 128 and a 0.25 x 0.125 texture
//   window, the sprites alpha 112, a 0.055 texture window and PartSize * 1.4 + 100.
// - "FogSpeedFix" (Patches/FogAdjustments.cpp RunFogSpeed): GridRate (the push of the density
//   grid, 0x1f7d28c) 0.25 in the town, the cemetery, the final rooms and most of the mansion,
//   0.5 in the apartment courtyard and the two Eddie rooms; and the speed limit for drawing
//   (0x6a7920) 10 instead of 200 on the cemetery path, so the fog does not stick to James.
// - "ObservationDeckFogFix" (Patches/ObservationDeckFogFix.cpp): on the observation deck the
//   particles spawn, wrap and fade over x -24500..24500 instead of +-MaxPos, so the fog fills the
//   wide view behind the deck.

#define FOG_MAX      144        // sprites the 3DS draws (the PC allows 500)
#define FOG_MAX_OBJ  64
#define EE_FOG       1

typedef struct {
	float pos[4], mv[4], dd[4], deg[4];
	float tdx, tdy, alp;         // texture window centre, draw alpha
	float alp_now, alp_add;
	int bounce, erase;
} Part;

typedef struct {
	float pos[4];                // pos[3] = size
	float mv[4];
	float escape, r_escape;
	int type;                    // 0 sphere (0x488760), 1 cylinder (0x4888e0)
	int id;
} Obj;

// FOG_ENV_DATA, 0x18 bytes
typedef struct {
	s16 part, maxpos, size, escape, floory, limy, limh, watery;
	u8 wind, flag, dbl, alpha;
	float grid;
} Env;

// the PC's built-in records
static const Env ENV_DEFAULT = { 500, 2000, 300, 100, 0, 0, 0, 10000, 0, 0x00, 1, 16, 2.0f };    // 0x887ed0
static const Env ENV_DEMO34 = { 200, 2000, 300, 100, 2500, 0, 0, 10000, 15, 0x08, 2, 32, 2.0f };  // 0x887ee8
static const Env ENV_LEAVE = { 300, 3000, 350, 100, 3000, 0, 0, 10000, 15, 0x00, 1, 8, 2.0f };    // 0x887f00
static const Env ENV_EPILOGUE = { 300, 2000, 250, 100, 3000, 0, 0, 10000, 3, 0x08, 1, 8, 2.0f };  // 0x887f18

// FOG_WORK.Flag (0x1f7d2a8): the low byte is the environment's flag
enum {
	FL_LIMIT = 0x01,             // fade out above LimitY
	FL_LIGHT = 0x04,             // dim with the distance to LightPosV
	FL_FLOOR = 0x08,             // FloorY follows the target
	FL_TRACKX = 0x20,            // the centre slides along x with the camera (observation deck)
	FL_STAY = 0x40,              // WorldPosV is not taken from James
	FL_NOOBJ = 0x80,             // nothing pushes the fog
	FL_SKIP = 0x200,             // skip one draw (set when an interior fog loads)
	FL_SCREEN = 0x400,           // screen layer only, at twice the alpha
};

static struct {
	bool ready;
	char pack[96];
	C3D_Tex tex;
	DVLB_s* dvlb;
	shaderProgram_s prog;
	int uProj, uMv;
	u8* vbuf;                    // FOG_MAX * 5 vertices + the screen quad
	u16* ibuf;

	Part part[FOG_MAX];
	int nsim;                    // particles simulated (<= FOG_MAX)
	int npc;                     // the PC's PartNum (0x1f7d294), for the alpha share
	float grid_old[512], grid_new[512], grid_save[512];   // 0x1f63b20, 0x1f64320, 0x1f77ac0
	Obj obj[FOG_MAX_OBJ];
	int nobj;

	float camera[4], old_camera[4], world[4], local[4], map[4], wind[4], fewdense[4];
	float limit_y, limit_y2, water_y, speed, sc_tdx, sc_tdy;
	float maxpos, partsize, projection, escape, gridrate, floory;
	int alpha, winddef, dbl, global, sumw;
	float windtimer;
	u32 flag, color;             // color: 0xAABBGGRR as FUN_004879c0 packs it (a, r, g, b bytes)
	const Env* env_now;
	Env env_copy;

	u8* fcl;                     // ColisHead (0x1f7d2bc)
	const s16* areas;            // AreaTop (0x1f7d2c0)
	const u8* envs;              // EnvTop (0x1f7d2c4)
	int nareas, nenvs;
	int load_step;               // 0x1f7d2a3: 2 = no fog file for this place
	float draw_limit;            // 0x6a7920
	bool obsv_deck;
	bool dark_halve;             // a dark room halves the count (0x489573), outdoor modes only
	u32 seed;
	int drawn;
	char status[64];
} F;

// ---------------------------------------------------------------- PC helpers

// 0x448830 / 0x448870: the PC's linear congruential generator (its own seed here)
static float rnd(void)
{
	F.seed = (F.seed * 0xc1c64e6du + 0x3039u) & 0x7fffffffu;
	return (float)F.seed * 4.656612873e-10f;
}

static void rnd_vec(float* v, float scale)
{
	for (int i = 0; i < 3; i++)
		v[i] = (rnd() * 2.0f - 1.0f) * scale;
	v[3] = 0.0f;
}

// 0x448910: a small value with a long tail, random sign when a < 0
static float rnd_tail(float a, float b)
{
	if (a < 0.0f && (rnd() < 0.5f))
		a = -a;
	return ((b + 1.0f) * b / (rnd() + b) - b) * a;
}

static int ftol(float x) { return (int)x; }   // __ftol truncates

static float flen3(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// 0x488070
static void normalise(float* v)
{
	float l = flen3(v);
	float k = l != 0.0f ? 1.0f / l : 1.0f;
	if (k == 0.0f || k > 3.4e38f)
		k = 1.0f;
	v[0] *= k, v[1] *= k, v[2] *= k;
}

// x extent of the particle box: MaxPos, or EE's wide observation deck
static float max_x(void)
{
	return EE_FOG && F.obsv_deck ? 24500.0f : F.maxpos;
}

// ---------------------------------------------------------------- particles

// 0x485d50: a new particle anywhere in the box
static void part_init(Part* p)
{
	rnd_vec(p->pos, F.maxpos);
	if (EE_FOG && F.obsv_deck)
		p->pos[0] = (rnd() * 2.0f - 1.0f) * max_x();
	p->pos[3] = 1.0f;
	rnd_vec(p->mv, 1.0f);
	rnd_vec(p->dd, 0.5f);
	if (F.dbl)
		p->pos[1] = fabsf(p->pos[1]) / -(float)(1 << (F.dbl - 1));
	memset(p->deg, 0, sizeof p->deg);
	p->deg[2] = -1.0f;
	p->tdx = rnd(), p->tdy = rnd();
	p->alp = p->alp_now = (float)F.alpha;
	p->alp_add = 0.0f;
	p->bounce = p->erase = 0;
	for (int i = 0; i < 3; i++)
		p->pos[i] += F.local[i];
	p->pos[3] = 0.0f;
}

// 0x485e30: a particle reborn where the fog is thinnest, invisible at first
static void part_init_few(Part* p)
{
	rnd_vec(p->pos, F.maxpos);
	if (EE_FOG && F.obsv_deck)
		p->pos[0] = (rnd() * 2.0f - 1.0f) * max_x();
	rnd_vec(p->mv, 1.0f);
	rnd_vec(p->dd, 0.5f);
	if (F.dbl > 1)
		p->pos[1] /= (float)(1 << (F.dbl - 1));
	memset(p->deg, 0, sizeof p->deg);
	p->deg[2] = -1.0f;
	p->tdx = rnd(), p->tdy = rnd();
	p->alp = p->alp_now = p->alp_add = 0.0f;
	p->bounce = p->erase = 0;
	for (int i = 0; i < 3; i++)
		p->pos[i] += F.fewdense[i];
	p->pos[3] = 0.0f;
}

// 0x487a00: the PC caps the count at 500, halving everything above 200
static void set_partnum(int n)
{
	if (n > 200)
		n = (n - 200) / 2 + 200;
	if (n > 500)
		n = 500;
	if (n < 0)
		n = 0;
	F.npc = n;
	int sim = n < FOG_MAX ? n : FOG_MAX;
	for (int i = F.nsim > 0 ? F.nsim : 0; i < sim; i++)
		part_init(&F.part[i]);
	F.nsim = sim;
}

// 0x488700
static void reset_particles(void)
{
	for (int i = 0; i < F.nsim; i++)
		part_init(&F.part[i]);
	F.sc_tdx = rnd(), F.sc_tdy = rnd();
}

// 0x4880f0 fogChangeWind
static void change_wind(int wind)
{
	float d[4] = { 0, 0, 0, 0 }, r[4];
	float strength = 0.4f, random = 0.1f, ang;
	bool angle = false;
	switch (wind) {
	case 1: case 3:
		d[0] = rnd() - 0.5f, d[2] = rnd() - 0.5f;
		break;
	case 2:
		random = 0.0f;
		break;
	case 4:
		d[0] = (rnd() - 0.5f) * 0.1f, d[2] = 1.0f, strength = 0.5f, random = 0.0f;
		break;
	case 5: ang = (rnd() - 0.5f) * 0.785398f + 3.14159f; angle = true; break;
	case 6: ang = (rnd() - 0.5f) * 0.785398f; angle = true; break;
	case 7: ang = (rnd() - 0.5f) * 0.785398f - 1.5708f; angle = true; break;
	case 8: ang = (rnd() - 0.5f) * 0.785398f + 1.5708f; angle = true; break;
	case 9: ang = (rnd() - 0.5f) * 0.785398f + 2.35619f; angle = true; break;
	case 10: ang = (rnd() - 0.5f + 1.0f) * 0.785398f; angle = true; break;
	case 11: ang = (rnd() - 0.5f) * 0.785398f - 0.785398f; angle = true; break;
	case 12: ang = (rnd() - 0.5f) * 0.785398f - 2.35619f; angle = true; break;
	case 13:
		d[0] = 0.5f, d[1] = 0.1f, strength = 0.2f;
		break;
	case 14:
		d[0] = -(rnd() * 0.2f) - 0.2f, d[1] = rnd() * 0.2f + 0.1f, d[2] = -(rnd() * 0.5f) - 0.8f;
		strength = 1.0f;
		break;
	case 15:
		ang = (rnd() - 0.5f) * 0.0872665f + 2.61799f;
		d[0] = sinf(ang), d[2] = cosf(ang), d[1] = -0.1f, strength = 0.3f, random = 0.0f;
		break;
	case 16: case 17:
		d[0] = rnd() - 0.5f, d[2] = rnd() - 0.5f, strength = wind == 16 ? 0.1f : 0.2f, random = 0.0f;
		break;
	case 18:
		d[0] = rnd() + 1.0f, d[1] = rnd() * 0.1f, d[2] = (rnd() + 1.0f) * 0.1f, strength = 0.7f;
		break;
	default:
		d[0] = rnd() - 0.5f, d[1] = rnd() * 0.1f, d[2] = rnd() - 0.5f;
		break;
	}
	if (angle) {
		d[0] = sinf(ang), d[2] = cosf(ang);
		d[1] = rnd() * 0.1f;
	}
	normalise(d);
	for (int i = 0; i < 3; i++)
		d[i] *= strength;
	rnd_vec(r, 0.5f);
	normalise(r);
	for (int i = 0; i < 3; i++)
		d[i] += r[i] * random;
	// the wind turns at most 60 degrees at a time
	if (F.wind[0] != 0.0f || F.wind[1] != 0.0f || F.wind[2] != 0.0f) {
		float old = atan2f(F.wind[0], F.wind[2]);
		float diff = remainderf(old - atan2f(d[0], d[2]), 2.0f * (float)M_PI);
		if (fabsf(diff) > 1.0472f) {
			float s = diff > 0.0f ? 1.0f : diff < 0.0f ? -1.0f : 0.0f;
			float h = sqrtf(d[0] * d[0] + d[2] * d[2]);
			float a = s * -1.0472f + old;
			d[0] = sinf(a) * h, d[2] = cosf(a) * h;
		}
	}
	memcpy(F.wind, d, sizeof F.wind);
	F.windtimer += (float)ftol(rnd_tail(-300.0f, 0.1f));
	if (F.windtimer < 300.0f || F.windtimer > 900.0f)
		F.windtimer = 600.0f;
	for (int i = 0; i < F.nsim; i++)
		rnd_vec(F.part[i].dd, 0.5f);
}

// 0x4893f0 fogSetEnvironment
static void set_env(const Env* e)
{
	if (e == F.env_now) {
		F.windtimer -= F.speed;          // the PC counts frames (0x4479b0)
		if (F.windtimer < 1.0f)
			change_wind(F.winddef);
		return;
	}
	F.env_now = e;
	if (!e) {
		F.nsim = F.npc = 0;
		return;
	}
	float old_max = F.maxpos + 2000.0f;
	F.maxpos = e->maxpos;
	F.partsize = e->size;
	if (EE_FOG)
		F.partsize = F.partsize * 1.4f + 100.0f;     // EE FogFix layer 2 density
	F.escape = e->escape;
	F.floory = e->floory;
	F.water_y = e->watery;
	int old_wind = F.winddef;
	F.winddef = e->wind;
	if (F.winddef != old_wind)
		change_wind(F.winddef);
	if (!(e->flag & FL_LIMIT)) {
		F.limit_y = -3.4e38f;
	} else {
		F.limit_y = e->limy;
		F.limit_y2 = (float)(e->limy - e->limh);
	}
	F.dbl = e->dbl;
	F.alpha = e->alpha;
	F.gridrate = e->grid;
	set_partnum(F.dark_halve ? e->part / 2 : e->part);
	F.flag = (F.flag & 0xff00) | e->flag;
	if (old_max < F.maxpos)
		reset_particles();
}

// ---------------------------------------------------------------- the fog box and objects

// 0x487ca0: the box centre (LocalPosV) follows the target, at most 2000 from the camera
static void update_centre(void)
{
	float d[4], keep[3] = { F.local[0], F.local[1], F.local[2] };
	for (int i = 0; i < 4; i++)
		d[i] = F.world[i] - F.camera[i];
	float dist = flen3(d), slope = dist;
	if (F.flag & FL_TRACKX)
		slope = d[2] == 0.0f ? d[0] : d[0] / fabsf(d[2]);
	if (dist <= 2000.0f) {
		memcpy(F.local, F.world, sizeof F.local);
		if (F.dbl)
			F.local[1] = F.floory;
	} else {
		float k = 2000.0f / dist;
		for (int i = 0; i < 4; i++)
			F.local[i] = F.camera[i] + d[i] * k;
	}
	if (F.flag & FL_TRACKX) {
		// the point where the line of sight crosses the box's z plane
		float off = (keep[2] - F.world[2]) * slope;
		off = off > F.maxpos ? F.maxpos : off < -F.maxpos ? -F.maxpos : off;
		F.local[0] += off, F.local[1] = keep[1], F.local[2] = keep[2];
	}
}

static Obj* obj_find(int id)
{
	for (int i = 0; i < F.nobj; i++)
		if (F.obj[i].id == id)
			return &F.obj[i];
	return NULL;
}

// 0x488760 (sphere) / 0x4888e0 (cylinder)
static void obj_set(int id, int type, const float* pos, float size)
{
	Obj* o = obj_find(id);
	if (!o) {
		if (F.nobj >= FOG_MAX_OBJ)
			return;
		o = &F.obj[F.nobj++];
	}
	memset(o, 0, sizeof *o);
	o->pos[0] = pos[0], o->pos[1] = pos[1], o->pos[2] = pos[2], o->pos[3] = size;
	o->escape = F.escape;
	o->r_escape = F.escape > 0.0f ? 1.0f / F.escape : 0.0f;
	o->type = type;
	o->id = id;
}

// 0x488800: the object's velocity is its movement since the last frame
static void obj_move(int id, const float* pos)
{
	Obj* o = obj_find(id);
	if (!o)
		return;
	for (int i = 0; i < 3; i++) {
		o->mv[i] = pos[i] - o->pos[i];
		o->pos[i] = pos[i];
	}
}

// 0x4838a0 (objects part): the .fcl's poles and stones within 3000 of the target
static void gather_objects(void)
{
	int k = 0;
	for (int i = 0; i < F.nobj; i++)
		if (F.obj[i].id < 100)
			F.obj[k++] = F.obj[i];
	F.nobj = k;
	memcpy(F.map, F.world, sizeof F.map);
	if (!F.fcl)
		return;
	const u16* h = (const u16*)F.fcl;
	const u8* p = F.fcl + 12 + h[0] * 12 + h[1] * 24;
	float range = F.global == 5 ? 3.4e38f : 3000.0f;
	float ox = F.global == 0x65 ? 100000.0f : 0.0f, oz = F.global == 0x65 ? -100000.0f : 0.0f;
	for (int t = 0; t < 2; t++) {
		for (int i = 0; i < h[2 + t]; i++, p += 16) {
			float o[4];
			memcpy(o, p, 16);
			o[0] -= ox, o[2] -= oz;
			if (fabsf(o[0] - F.world[0]) <= range && fabsf(o[2] - F.world[2]) <= range)
				obj_set((t ? 500 : 100) + i, t, o, o[3]);
		}
	}
}

// ---------------------------------------------------------------- one particle

#define G(a, z, y, x) (a)[((z) << 6) | ((y) << 3) | (x)]

// 0x486710: add the particle to the density grid (world-anchored, 8 cells a side, wrapping) and
// push it away from where the fog was thick last frame
static void grid_part(Part* p)
{
	float k = 4.0f / F.maxpos, ky = F.dbl ? k * (float)(1 << F.dbl) : k;
	float px = k * p->pos[0], py = ky * p->pos[1], pz = k * p->pos[2];
	int ix = ftol(px), iy = ftol(py), iz = ftol(pz);
	float fx = px - ix, fy = py - iy, fz = pz - iz, gx = 1.0f - fx, gy = 1.0f - fy, gz = 1.0f - fz;
	int x0 = ix & 7, y0 = iy & 7, z0 = iz & 7, x1 = (x0 + 1) & 7, y1 = (y0 + 1) & 7, z1 = (z0 + 1) & 7;
	// the PC sums the three weights where trilinear weighting would multiply them
	G(F.grid_new, z0, y0, x0) += gz + gy + gx;
	G(F.grid_new, z0, y0, x1) += gz + gy + fx;
	G(F.grid_new, z0, y1, x0) += gz + gx + fy;
	G(F.grid_new, z0, y1, x1) += gz + fy + fx;
	G(F.grid_new, z1, y0, x0) += gy + gx + fz;
	G(F.grid_new, z1, y0, x1) += gy + fz + fx;
	G(F.grid_new, z1, y1, x0) += gx + fz + fy;
	G(F.grid_new, z1, y1, x1) += fz + fy + fx;

	float qx = px + p->dd[0], qy = py + p->dd[1], qz = pz + p->dd[2];
	int jx = ftol(qx), jy = ftol(qy), jz = ftol(qz);
	float hx = qx - jx, hy = qy - jy, hz = qz - jz;
	int X0 = jx & 7, X1 = (X0 + 1) & 7, X2 = (X0 + 2) & 7, Xm = (X0 - 1) & 7;
	int Y0 = jy & 7, Y1 = (Y0 + 1) & 7, Y2 = (Y0 + 2) & 7, Ym = (Y0 - 1) & 7;
	int Z0 = jz & 7, Z1 = (Z0 + 1) & 7, Z2 = (Z0 + 2) & 7, Zm = (Z0 - 1) & 7;
	const float* o = F.grid_old;
	float ex = (G(o, Z0, Y1, X2) + G(o, Z0, Y0, X2) + G(o, Z1, Y1, X2) + G(o, Z1, Y0, X2)
	          - G(o, Z0, Y0, X0) - G(o, Z0, Y1, X0) - G(o, Z1, Y0, X0) - G(o, Z1, Y1, X0)) * (1.0f - hx)
	         + (G(o, Z0, Y1, X1) + G(o, Z1, Y1, X1) + G(o, Z0, Y0, X1) + G(o, Z1, Y0, X1)
	          - G(o, Z0, Y0, Xm) - G(o, Z0, Y1, Xm) - G(o, Z1, Y0, Xm) - G(o, Z1, Y1, Xm)) * hx;
	float ey = (G(o, Z0, Y2, X1) + G(o, Z1, Y2, X1) + G(o, Z0, Y2, X0) + G(o, Z1, Y2, X0)
	          - G(o, Z0, Y0, X0) - G(o, Z0, Y0, X1) - G(o, Z1, Y0, X0) - G(o, Z1, Y0, X1)) * (1.0f - hy)
	         + (G(o, Z1, Y1, X0) + G(o, Z0, Y1, X0) + G(o, Z0, Y1, X1) + G(o, Z1, Y1, X1)
	          - G(o, Z0, Ym, X0) - G(o, Z0, Ym, X1) - G(o, Z1, Ym, X0) - G(o, Z1, Ym, X1)) * hy;
	float ez = (G(o, Z2, Y0, X1) + G(o, Z2, Y1, X1) + G(o, Z2, Y0, X0) + G(o, Z2, Y1, X0)
	          - G(o, Z0, Y0, X0) - G(o, Z0, Y1, X0) - G(o, Z0, Y0, X1) - G(o, Z0, Y1, X1)) * (1.0f - hz)
	         + (G(o, Z1, Y1, X0) + G(o, Z1, Y0, X0) + G(o, Z1, Y1, X1) + G(o, Z1, Y0, X1)
	          - G(o, Zm, Y0, X0) - G(o, Zm, Y1, X0) - G(o, Zm, Y0, X1) - G(o, Zm, Y1, X1)) * hz;
	float c = F.gridrate / (float)(F.nsim > 0 ? F.nsim : 1);
	p->mv[0] = (p->mv[0] - c * ex) * 0.9f;
	p->mv[1] = (p->mv[1] - c * ey) * 0.9f;
	p->mv[2] = (p->mv[2] - c * ez) * 0.9f;
}

// 0x4870e0: the alpha drifts towards Alpha, less above LimitY, full below WaterY
static void alpha_part(Part* p)
{
	float target = (float)F.alpha;
	if (p->pos[1] < F.limit_y)
		target = p->pos[1] >= F.limit_y2 ? (p->pos[1] - F.limit_y2) / (F.limit_y - F.limit_y2) * F.alpha : 0.0f;
	if (p->pos[1] > F.water_y)
		target = 127.0f;
	p->alp_add = (rnd_tail(F.alpha * -0.03125f, 0.02f) + p->alp_add - (p->alp_now - target) * 0.01f) * 0.9f;
	p->alp_now += p->alp_add;
	if (p->alp_now < 0.0f)
		p->alp_now = 0.0f;
	if (p->alp_now > 127.0f)
		p->alp_now = 127.0f;
}

// 0x4871c0: wrap around the box (mirrored, so the pattern does not repeat) and fade at its edges
static void wrap_part(Part* p)
{
	float M = F.maxpos, MX = max_x();
	float x = p->pos[0] - F.local[0], y = p->pos[1] - F.local[1], z = p->pos[2] - F.local[2];
	if (x < -MX) x += MX + MX, z = -z;
	else if (x > MX) x -= MX + MX, z = -z;
	if (F.dbl == 0) {
		if (y < -M) y += M + M;
		else if (y > M) p->alp_now = 0.0f, y -= M + M;
	} else if (F.dbl == 1) {
		if (y < -M) y += M;
		else if (y > 0.0f) p->alp_now = 0.0f, y -= M;
	} else {
		float s = (float)(1 << F.dbl);
		if (y < M * -2.0f / s) y += (M + M) / s;
		else if (y > 0.0f) p->alp_now = 0.0f, y -= (M + M) / s;
	}
	if (z < -M) z += M + M, x = -x;
	else if (z > M) z -= M + M, x = -x;
	float a = p->alp_now;
	if (x < 300.0f - MX) a *= (MX + x) * (1.0f / 300.0f);
	else if (x > MX - 300.0f) a *= (MX - x) * (1.0f / 300.0f);
	if (F.dbl == 0) {
		if (y < 300.0f - M) a *= (M + y) * (1.0f / 300.0f);
	} else {
		float t = (M + M) / (float)(1 << F.dbl);
		if (y < 300.0f - t) a *= (t + y) * (1.0f / 300.0f);
	}
	if (y > M - 300.0f) a *= (M - y) * (1.0f / 300.0f);
	if (z < 300.0f - M) a *= (M + z) * (1.0f / 300.0f);
	else if (z > M - 300.0f) a *= (M - z) * (1.0f / 300.0f);
	p->alp = a;
	p->pos[0] = F.local[0] + x, p->pos[1] = F.local[1] + y, p->pos[2] = F.local[2] + z;
}

// 0x486d70 (spheres) / 0x486f20 (cylinders): a moving object takes the fog along
static void push_part(Part* p, const Obj* list, int n)
{
	for (int i = 0; i < n; i++) {
		const Obj* o = &list[i];
		float dx = p->pos[0] - o->pos[0], dy = o->type ? 0.0f : p->pos[1] - o->pos[1], dz = p->pos[2] - o->pos[2];
		if (o->type && o->pos[1] > F.partsize + p->pos[1])
			continue;
		float inner = F.partsize + o->pos[3], outer = inner + o->escape;
		float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 >= outer * outer || d2 <= 0.0f)
			continue;
		float d = sqrtf(d2), inv = 1.0f / d, k;
		if (d >= inner) {
			k = (outer - d) * o->r_escape;
			if (o->type)
				p->bounce++;
		} else {
			k = 1.0f;
			p->bounce++;
		}
		float rel = ((p->mv[0] - o->mv[0]) * dx + (p->mv[1] - o->mv[1]) * dy + (p->mv[2] - o->mv[2]) * dz) * inv * k;
		if (rel >= 0.0f)
			rel *= 0.1f;
		p->mv[0] -= dx * inv * rel;
		p->mv[1] -= dy * inv * rel;
		p->mv[2] -= dz * inv * rel;
	}
}

// 0x4895b0 (without the per-place setup, done by the caller)
static void move_all(void)
{
	if (F.nsim <= 0 || (F.flag & FL_SCREEN))
		return;
	float dx = F.world[0] - F.part[0].pos[0], dz = F.world[2] - F.part[0].pos[2];
	if (sqrtf(dx * dx + dz * dz) > 30000.0f && F.global != 5) {
		memset(F.wind, 0, sizeof F.wind);      // 0x4893a0
		change_wind(F.winddef);
		memset(F.grid_save, 0, sizeof F.grid_save);
		reset_particles();
	}
	memcpy(F.grid_old, F.grid_save, sizeof F.grid_old);
	memset(F.grid_new, 0, sizeof F.grid_new);
	for (int i = 0; i < F.nsim; i++) {
		Part* p = &F.part[i];
		if (fabsf(p->pos[0]) > 1e8f || fabsf(p->pos[1]) > 1e8f || fabsf(p->pos[2]) > 1e8f)
			part_init_few(p);
		if (!p->erase) {
			if (p->bounce > 0)
				p->erase = 1;
		} else {
			p->alp_now *= 0.75f;
			if (p->alp_now <= 1.0f)
				part_init_few(p);
		}
		for (int k = 0; k < 3; k++)
			p->pos[k] += F.speed * p->mv[k];
		p->bounce = 0;
		for (int k = 0; k < 3; k++)
			p->mv[k] += F.wind[k];
		grid_part(p);
		alpha_part(p);
		if (p->mv[1] < -2.0f)
			p->erase = 1;
	}
	memcpy(F.grid_save, F.grid_new, sizeof F.grid_save);
	// the thinnest cell is where faded particles come back; the PC reads the index's top bits
	// as x and its low bits as z
	int best = 0;
	for (int i = 1; i < 512; i++)
		if (F.grid_new[i] < F.grid_new[best])
			best = i;
	int cx = best >> 6, cy = (best >> 3) & 7, cz = best & 7;
	cx -= cx > 3 ? 8 : 0, cy -= cy > 3 ? 8 : 0, cz -= cz > 3 ? 8 : 0;
	float cell = F.maxpos * 0.25f;
	F.fewdense[0] = F.local[0] + cx * cell;
	F.fewdense[1] = F.local[1] + cy * cell;
	F.fewdense[2] = F.local[2] + cz * cell;
	// 0x486080: only objects near the box can touch it
	int near = 0;
	Obj keep[FOG_MAX_OBJ];
	for (int i = 0; i < F.nobj; i++) {
		const Obj* o = &F.obj[i];
		float k = 2.0f / F.maxpos;
		if (fabsf(k * (o->pos[0] - F.local[0])) < 2.5f && fabsf(k * (o->pos[1] - F.local[1])) < 2.5f &&
		    fabsf(k * (o->pos[2] - F.local[2])) < 2.5f)
			keep[near++] = *o;
	}
	for (int i = 0; i < F.nsim; i++) {
		Part* p = &F.part[i];
		if (!(F.flag & FL_NOOBJ) && near)
			push_part(p, keep, near);
		wrap_part(p);
	}
	for (int i = 0; i < F.nobj; i++)
		F.obj[i].mv[0] = F.obj[i].mv[1] = F.obj[i].mv[2] = 0.0f;
}

// ---------------------------------------------------------------- places

static bool load_fcl(const char* name)
{
	free(F.fcl);
	F.fcl = NULL;
	F.areas = NULL;
	F.envs = NULL;
	F.nareas = F.nenvs = 0;
	char path[160];
	snprintf(path, sizeof path, "%s/fog/%s.fcl", F.pack, name);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	F.fcl = malloc(n);
	if (!F.fcl || fread(F.fcl, 1, n, f) != (size_t)n) {
		fclose(f);
		free(F.fcl);
		F.fcl = NULL;
		return false;
	}
	fclose(f);
	// FUN_004838a0: walls (12 and 24 bytes), objects (16 bytes each, two kinds), areas (10 bytes),
	// then the FOG_ENV_DATA records, 4-byte aligned
	const u16* h = (const u16*)F.fcl;
	size_t o = 12 + h[0] * 12 + h[1] * 24 + (h[2] + h[3]) * 16;
	F.areas = (const s16*)(F.fcl + o);
	F.nareas = h[4];
	o = (o + F.nareas * 10 + 3) & ~(size_t)3;
	F.envs = F.fcl + o;
	F.nenvs = (int)((n - (long)o) / 0x18);
	return true;
}

static const Env* fcl_env(int i)
{
	if (!F.envs || i < 0 || i >= F.nenvs)
		return &ENV_DEFAULT;
	memcpy(&F.env_copy, F.envs + i * 0x18, sizeof F.env_copy);
	// the same record keeps the same pointer, which set_env uses to tell "unchanged"
	static Env cache[24];
	if (i < 24) {
		cache[i] = F.env_copy;
		return &cache[i];
	}
	return &F.env_copy;
}

// 0x4849d0: which fog the place has. The PC keys it on the stage's glb_crd and, inside, the room id
static int place_mode(const FogFrame* f)
{
	int g = f->glb, r = f->room;
	if (g == 3) {
		float x = F.world[0], z = F.world[2];
		if (x <= -40000.0f || (z < 80000.0f && (z <= 65500.0f || F.global != 100))) {
			if ((x <= -55000.0f && z > 55000.0f) || (F.global == 0x65 && z > 40000.0f && x <= -50000.0f))
				return 0x65;           // ccnw.fcl
			return 3;
		}
		return 100;                    // park.fcl
	}
	if (g == 0xb) return r == 0x8e ? 0x66 : r == 0x8f ? 0x67 : r == 0x90 ? 0x68 : g;
	if (g == 0xc) return r == 0x98 ? 0x6a : r == 0x9b ? 0x6b : r == 0xa2 ? 0x6c : g;
	if (g == 9) return r == 0x28 ? 0x69 : g;
	if (g == 0xd) return r == 0xb0 ? 0x6b : (r == 0xbb && f->james[1] < -12450.0f) ? 0x6d : g;
	if (g == 0xe) return r == 0xbd ? 0x6e : g;
	if (g == 0x10) {
		static const u8 rooms[8] = { 0xc0, 0xc2, 0xc4, 0xc9, 0xca, 0xd0, 0xd3, 0xd4 };
		for (int i = 0; i < 8; i++)
			if (r == rooms[i])
				return 0x6f + i;
	}
	return g;
}

static void enter_mode(int mode)
{
	static const char* const outdoor[6] = { NULL, "caGB", "cbGB", "ccGB", "cdGB", "obGB" };
	F.nobj = 0;                        // 0x485ef0 / 0x486030
	F.areas = NULL;
	F.env_now = NULL;
	set_env(&ENV_DEFAULT);
	F.flag &= ~(u32)FL_LIMIT;
	F.global = mode;
	const char* name = NULL;
	if (mode >= 1 && mode <= 5) name = outdoor[mode];
	else if (mode == 100) name = "park";
	else if (mode == 0x65) name = "ccnw";
	else if (mode >= 0x66 && mode <= 0x68) name = "psGB";
	else if (mode >= 0x69 && mode <= 0x6e) name = "rrGB";
	else if (mode >= 0x6f && mode <= 0x76) name = "maGB";
	if (!name || !load_fcl(name)) {
		free(F.fcl);
		F.fcl = NULL;
		F.global = 0xff;
		set_partnum(0);
		F.load_step = 2;
		return;
	}
	F.load_step = 0;
	if (mode >= 0x69)
		set_partnum(0);
	if (mode >= 0x6f)
		fog_set_color(0x80, 0x80, 0x80, 0x80);
	if (mode != 0x65)
		gather_objects();
	if (mode > 0x68)
		F.flag |= FL_SKIP;
	if (mode >= 0x6f)
		memset(F.wind, 0, sizeof F.wind);
}

// the fixed fog centres of the interior places (0x484f86 switch)
static void place_centre(const FogFrame* f)
{
	static const struct { u8 mode; float x, y, z; } fixed[] = {
		{ 0x67, 100000, 0, -100000 }, { 0x69, 16000, 0, -99000 }, { 0x6a, -60000, 0, 56400 },
		{ 0x6b, 20000, 0, -63300 }, { 0x6c, 60000, 0, 16000 }, { 0x6e, 10000, 0, 9600 },
		{ 0x6f, 16300, 0, 20000 }, { 0x70, -23000, 0, -20000 }, { 0x71, 100500, -800, 18600 },
		{ 0x72, 20400, -1300, -61300 }, { 0x74, -66100, 0, 55200 },
	};
	if (F.global == 5) {             // observation deck: the centre slides along the deck
		F.world[0] = f->james[0], F.world[1] = f->james[1], F.world[2] = f->james[2];
		update_centre();
		F.local[1] = 1000.0f, F.local[2] = 7000.0f;
		return;
	}
	if (F.global == 0x6d) {
		F.world[1] = F.local[1] = -16000.0f;
		return;
	}
	if (F.global == 0x73) {
		F.world[0] = f->james[0], F.world[1] = f->james[1], F.world[2] = f->james[2];
		update_centre();
		F.flag |= FL_FLOOR;
		F.local[0] = 62000.0f, F.local[1] = 0.0f;
		return;
	}
	for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
		if (fixed[i].mode == F.global) {
			F.world[0] = F.local[0] = fixed[i].x;
			F.world[1] = F.local[1] = fixed[i].y;
			F.world[2] = F.local[2] = fixed[i].z;
			return;
		}
}

// 0x484760: the environment of the area the target stands in (or the scene's own)
static void place_env(const FogFrame* f)
{
	F.flag &= ~(u32)FL_SCREEN;
	if (f->demo_no >= 0) {
		switch (f->demo_no) {
		case 3: case 4: set_env(&ENV_DEMO34); return;
		case 0x10: set_partnum(0); return;
		case 0x4a: case 0x4b:
			set_env(fcl_env(0));
			F.world[0] = F.local[0] = 140000.0f;
			F.world[1] = F.local[1] = 0.0f;
			F.world[2] = F.local[2] = -101500.0f;
			return;
		case 0x4e: set_partnum(f->demo_timer >= 780.0f && f->demo_timer < 1420.0f ? 50 : 100); return;
		case 0x5a: set_env(&ENV_LEAVE); return;
		case 0x5c: set_env(&ENV_EPILOGUE); return;
		case 0x61: F.flag |= FL_SCREEN; return;
		}
	}
	if (f->flag508) {
		if (f->demo_no == 0x5a)
			set_env(&ENV_LEAVE);
		return;
	}
	if (!F.fcl) {
		set_env(&ENV_DEFAULT);
		return;
	}
	float x = F.world[0], z = F.world[2];
	if (F.global == 0x65)
		x += 100000.0f, z -= 100000.0f;
	int ax = ftol(x * 0.02f), az = ftol(z * 0.02f);
	for (int i = 0; i < F.nareas; i++) {
		const s16* a = F.areas + i * 5;
		if (ax >= a[0] && ax <= a[2] && az >= a[1] && az <= a[3]) {
			set_env(fcl_env((u16)a[4]));
			return;
		}
	}
	set_env(fcl_env(0));
}

// Enhanced Edition "FogSpeedFix": the slower fog of the PS2 in these rooms
static void ee_fog_speed(const FogFrame* f)
{
	if (!EE_FOG)
		return;
	switch (f->room) {
	case 0x03: case 0x04: case 0x08: case 0x8e: case 0xb0: case 0xbb: case 0xbd:
	case 0xc0: case 0xc2: case 0xca: case 0xc4: case 0xd3: case 0xd4:
		F.gridrate = 0.25f;
		break;
	case 0x07: case 0x8f: case 0x90:
		F.gridrate = 0.5f;
		break;
	}
	F.draw_limit = f->room == 0x03 && f->james[1] >= 1125.0f && f->james[1] <= 1575.0f ? 10.0f : 200.0f;
}

void fog_update(const FogFrame* f)
{
	if (!F.ready)
		return;
	F.speed = f->dt * 60.0f;
	F.projection = f->scr_z;
	F.camera[0] = f->eye[0], F.camera[1] = f->eye[1], F.camera[2] = f->eye[2], F.camera[3] = 1.0f;
	// 0x489b90: the target is the camera's watch point at James' height outside, James inside
	if (!(F.flag & FL_STAY)) {
		bool watch = F.global < 6 || F.global == 0x6d;
		F.world[0] = watch ? f->watch[0] : f->james[0];
		F.world[1] = f->james[1];
		F.world[2] = watch ? f->watch[2] : f->james[2];
		F.world[3] = 1.0f;
	}
	F.obsv_deck = f->room == 0x02;
	F.dark_halve = f->dark && f->glb <= 4;
	float body[3] = { f->james[0], f->james[1] - 750.0f, f->james[2] };
	float head[3] = { f->james[0], f->james[1] - 820.0f, f->james[2] };
	if (!obj_find(1)) {
		obj_set(1, 1, body, 110.0f);
		obj_set(2, 0, head, 60.0f);
	} else {
		obj_move(1, body);
		obj_move(2, head);
	}
	update_centre();
	if (f->flag508) {
		// the ending scenes (0x4849d0 with flag 508): the Leave scene's fog sits in the cemetery
		if (f->demo_no == 0x5a) {
			F.world[0] = F.local[0] = -6500.0f;
			F.world[1] = F.local[1] = 3000.0f;
			F.world[2] = F.local[2] = 1000.0f;
			if (F.winddef != 0xf) {
				F.winddef = 0xf;
				memset(F.wind, 0, sizeof F.wind);
				change_wind(F.winddef);
			}
		} else if (f->demo_no == 0x5d) {
			set_partnum(0);
		} else if (f->demo_no == 0x61 && F.winddef != 0xe) {
			F.winddef = 0xe;
			memset(F.wind, 0, sizeof F.wind);
			change_wind(F.winddef);
		}
	} else {
		int mode = place_mode(f);
		if (mode != F.global)
			enter_mode(mode);
		place_centre(f);
	}
	float dx = F.world[0] - F.map[0], dz = F.world[2] - F.map[2];
	if (F.global != 0x65 && F.fcl && dx * dx + dz * dz > 500.0f * 500.0f)
		gather_objects();
	place_env(f);
	ee_fog_speed(f);
	if (F.flag & FL_FLOOR)
		F.floory = F.world[1];
	if (F.speed > 0.0f)
		move_all();
	snprintf(F.status, sizeof F.status, "fog %02x n%d/%d a%d d%d sz%.0f m%.0f", F.global & 0xff, F.nsim, F.npc,
	         F.alpha, F.drawn, F.partsize, F.maxpos);
}

// ---------------------------------------------------------------- drawing

typedef struct { float x, y, z, u, v; u8 c[4]; } FogVtx;

static int cmp_depth(const void* a, const void* b)
{
	float da = ((const float*)a)[0], db = ((const float*)b)[0];
	return da < db ? 1 : da > db ? -1 : 0;           // far first
}

void fog_draw(const C3D_Mtx* proj, const C3D_Mtx* view, const C3D_Mtx* ortho)
{
	F.drawn = 0;
	if (!F.ready || F.global == 0xff)
		return;
	if (F.flag & FL_SKIP) {
		F.flag &= ~(u32)FL_SKIP;
		return;
	}
	FogVtx* vb = (FogVtx*)F.vbuf;
	u16* ib = F.ibuf;
	int nv = 0, ni = 0;
	u32 rgb = F.color & 0xffffff;     // r | g << 8 | b << 16
	float right[3] = { view->r[0].x, view->r[0].y, view->r[0].z };
	float down[3] = { -view->r[1].x, -view->r[1].y, -view->r[1].z };
	float fwd[3] = { -view->r[2].x, -view->r[2].y, -view->r[2].z };

	if (!(F.flag & FL_SCREEN) && F.nsim > 0) {
		// 0x488a80: the near limit grows with the zoom and is smoothed over frames
		float nearz = 64.0f;
		if (F.projection > 600.0f)
			nearz = (F.projection - 600.0f) * 1.2f + 64.0f;
		nearz = ((float)(F.sumw * 3) + nearz) * 0.25f;
		F.sumw = ftol(nearz);
		float dir[4] = { F.world[0] - F.camera[0], F.world[1] - F.camera[1], F.world[2] - F.camera[2], 0 };
		normalise(dir);
		float horiz = sqrtf(1.0f - (dir[1] * dir[1] >= 1.0f ? 1.0f : dir[1] * dir[1]));
		float abyte = EE_FOG ? 112.0f : (float)(F.color >> 24);
		float share = F.nsim > 0 ? (float)F.npc / (float)F.nsim : 1.0f;
		float P = F.partsize, w = EE_FOG ? 0.055f : 0.0488281f;
		float order[FOG_MAX][2];
		int n = 0;
		for (int i = 0; i < F.nsim; i++) {
			Part* p = &F.part[i];
			if (p->alp < 1.0f || F.floory - p->pos[1] <= 0.0f)
				continue;
			float rel[3] = { p->pos[0] - F.camera[0], p->pos[1] - F.camera[1], p->pos[2] - F.camera[2] };
			float z14 = (rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2]) * 1.4f;
			if (z14 < nearz || z14 > 32767.0f)
				continue;
			float a = abyte * p->alp * 0.0078125f, h = F.floory - p->pos[1], squash = 1.0f;
			if (h < P) {
				h /= P;
				squash = h < horiz ? h / horiz : 1.0f;
			} else {
				h = 1.0f;
			}
			a *= h;
			if (z14 < nearz + 512.0f)
				a = (z14 - nearz) * a * 0.001953125f;
			a *= share;
			if (a < 2.0f || flen3(p->mv) > F.draw_limit)
				continue;
			// texture window: scrolls with the particle's direction from the camera (parallax)
			float d[4] = { rel[0], rel[1], rel[2], 0 };
			normalise(d);
			float dd[3] = { d[0] - p->deg[0], d[1] - p->deg[1], d[2] - p->deg[2] };
			memcpy(p->deg, d, sizeof p->deg);
			p->tdx -= (dd[0] * right[0] + dd[1] * right[1] + dd[2] * right[2]) * 0.25f;
			p->tdy -= (dd[0] * down[0] + dd[1] * down[1] + dd[2] * down[2]) * 0.25f;
			p->tdx -= floorf(p->tdx), p->tdy -= floorf(p->tdy);
			// the sprite stretches along its movement on screen
			float vx = p->mv[0] * right[0] + p->mv[1] * right[1] + p->mv[2] * right[2];
			float vy = p->mv[0] * down[0] + p->mv[1] * down[1] + p->mv[2] * down[2];
			float s = vx * vy, sx = s >= 0.0f ? sqrtf(s) : -sqrtf(-s);
			float rx = sqrtf(fabsf(vx)) * 0.125f, ry = sqrtf(fabsf(vy)) * 0.125f;
			float lo = 1.0f - sx * 0.0078125f, hi = 1.0f + sx * 0.0078125f;
			float axn = fmaxf(lo - rx, 0.25f), ayn = fmaxf(lo - ry, 0.25f);
			float axp = fmaxf(hi - rx, 0.25f), ayp = fmaxf(hi - ry, 0.25f);
			float cx[4] = { -axn * P, axp * P, axn * P, -axp * P };
			float cy[4] = { -ayn * P, -ayp * P, ayn * P * squash, ayp * P * squash };
			float cu[4] = { p->tdx - w, p->tdx + w, p->tdx + w, p->tdx - w };
			float cv[4] = { p->tdy - w, p->tdy - w, p->tdy + w * squash, p->tdy + w * squash };
			FogVtx* v = &vb[n * 5];
			u8 alpha = a > 255.0f ? 255 : (u8)a;
			v[0] = (FogVtx){ p->pos[0], p->pos[1], p->pos[2], p->tdx, 1.0f - p->tdy,
			                 { rgb & 255, rgb >> 8 & 255, rgb >> 16 & 255, alpha } };
			for (int k = 0; k < 4; k++)
				v[1 + k] = (FogVtx){ p->pos[0] + right[0] * cx[k] + down[0] * cy[k],
				                     p->pos[1] + right[1] * cx[k] + down[1] * cy[k],
				                     p->pos[2] + right[2] * cx[k] + down[2] * cy[k], cu[k], 1.0f - cv[k],
				                     { rgb & 255, rgb >> 8 & 255, rgb >> 16 & 255, 0 } };
			order[n][0] = z14;
			order[n][1] = (float)n;
			n++;
		}
		qsort(order, n, sizeof order[0], cmp_depth);
		for (int i = 0; i < n; i++) {
			u16 b = (u16)((int)order[i][1] * 5);
			static const u8 fan[12] = { 0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1 };
			for (int k = 0; k < 12; k++)
				ib[ni++] = b + fan[k];
		}
		nv = n * 5;
		F.drawn = n;
	}

	// 0x4876c0: the screen layer scrolls with the camera's movement and the wind
	int screen_v = -1, screen_i = ni;
	if (F.global <= 5 || F.global == 0x6d) {
		float k = F.projection > 1.0f ? 0.1f / F.projection : 0.0f, ws = F.speed * 0.005f;
		float m[3] = { (F.camera[0] - F.old_camera[0]) * k + F.wind[0] * ws,
		               (F.camera[1] - F.old_camera[1]) * k + F.wind[1] * ws,
		               (F.camera[2] - F.old_camera[2]) * k + F.wind[2] * ws };
		F.sc_tdx -= (m[0] * right[0] + m[1] * right[1] + m[2] * right[2]) * 0.25f;
		F.sc_tdy -= (m[0] * down[0] + m[1] * down[1] + m[2] * down[2]) * 0.25f;
		F.sc_tdx -= floorf(F.sc_tdx), F.sc_tdy -= floorf(F.sc_tdy);
		int a = ((EE_FOG ? 128 : (int)(F.color >> 24)) * F.alpha) >> 9;
		if (a == 0)
			a = 1;
		if (F.flag & FL_SCREEN)
			a *= 2;
		float wx = EE_FOG ? 0.25f : 0.125f, wy = 0.125f;
		FogVtx* v = &vb[nv];
		const float xy[4][2] = { { 0, 0 }, { 400, 0 }, { 400, 240 }, { 0, 240 } };
		const float uv[4][2] = { { -wx, wy }, { wx, wy }, { wx, -wy }, { -wx, -wy } };
		for (int i = 0; i < 4; i++)
			v[i] = (FogVtx){ xy[i][0], xy[i][1], 0.5f, F.sc_tdx + uv[i][0], 1.0f - (F.sc_tdy + uv[i][1]),
			                 { rgb & 255, rgb >> 8 & 255, rgb >> 16 & 255, (u8)(a > 255 ? 255 : a) } };
		static const u8 quad[6] = { 0, 1, 2, 0, 2, 3 };
		for (int i = 0; i < 6; i++)
			ib[ni++] = (u16)(nv + quad[i]);
		screen_v = nv;
		nv += 4;
	}
	memcpy(F.old_camera, F.camera, sizeof F.old_camera);
	if (!nv)
		return;
	GSPGPU_FlushDataCache(F.vbuf, nv * sizeof(FogVtx));
	GSPGPU_FlushDataCache(F.ibuf, ni * sizeof(u16));

	C3D_BindProgram(&F.prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, F.vbuf, sizeof(FogVtx), 3, 0x210);
	C3D_TexBind(0, &F.tex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);         // D3DTOP_MODULATE2X
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(true, GPU_GREATER, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	if (screen_i > 0) {
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, F.uProj, proj);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, F.uMv, view);
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		C3D_DepthMap(true, -1.0f, 0.0f);
		C3D_DrawElements(GPU_TRIANGLES, screen_i, C3D_UNSIGNED_SHORT, F.ibuf);
	}
	if (screen_v >= 0) {
		C3D_Mtx id;
		Mtx_Identity(&id);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, F.uProj, ortho);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, F.uMv, &id);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, F.ibuf + screen_i);
	}
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

// ---------------------------------------------------------------- setup

bool fog_init(const char* pack_dir)
{
	memset(&F, 0, sizeof F);
	snprintf(F.pack, sizeof F.pack, "%s", pack_dir);
	F.seed = 0x1234567;
	F.global = 0xff;
	F.color = 0x80808080;
	F.draw_limit = 200.0f;
	char path[160];
	snprintf(path, sizeof path, "%s/fog/fog.t3x", pack_dir);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &F.tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(&F.tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&F.tex, GPU_REPEAT, GPU_REPEAT);
	F.dvlb = DVLB_ParseFile((u32*)fog_shbin, fog_shbin_size);
	shaderProgramInit(&F.prog);
	shaderProgramSetVsh(&F.prog, &F.dvlb->DVLE[0]);
	F.uProj = shaderInstanceGetUniformLocation(F.prog.vertexShader, "projection");
	F.uMv = shaderInstanceGetUniformLocation(F.prog.vertexShader, "modelView");
	F.vbuf = linearAlloc((FOG_MAX * 5 + 4) * sizeof(FogVtx));
	F.ibuf = linearAlloc((FOG_MAX * 12 + 6) * sizeof(u16));
	F.ready = F.vbuf && F.ibuf;
	return F.ready;
}

void fog_exit(void)
{
	if (!F.dvlb)
		return;
	C3D_TexDelete(&F.tex);
	shaderProgramFree(&F.prog);
	DVLB_Free(F.dvlb);
	linearFree(F.vbuf);
	linearFree(F.ibuf);
	free(F.fcl);
	F.ready = false;
}

// 0x489af0: on stage connect
void fog_reset(void)
{
	if (!F.ready)
		return;
	memset(F.part, 0, sizeof F.part);
	memset(F.grid_save, 0, sizeof F.grid_save);
	F.escape = 100.0f;
	F.nobj = 0;
	F.global = -1;                    // the next update loads the place's fog
	F.env_now = NULL;
	F.color = 0x80808080;
	memset(F.wind, 0, sizeof F.wind);
	change_wind(F.winddef);
	reset_particles();
}

void fog_set_color(int r, int g, int b, int a)
{
	if (r < 0)
		return;
	F.color = (u32)(a & 255) << 24 | (u32)(b & 255) << 16 | (u32)(g & 255) << 8 | (u32)(r & 255);
}

const char* fog_status(void) { return F.ready ? F.status : "fog off"; }
