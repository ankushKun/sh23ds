#include "stagemdl.h"
#include "chr.h"
#include "chrlight.h"
#include "demo.h"
#include "events.h"
#include "prog_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"

// The placed models of the current stage (pack/events/stagemdl.txt, tools/sh2conv/stagemodels.py):
// the PC loads those in James' room (0x493430); the port draws the ones within SM_RANGE of him
// (fog hides the rest) and loads a model the first time one of its places comes that close, one a
// frame, keeping at most SM_LOADED models and dropping the one unused longest.
#define SM_MAX_PLACES 128
#define SM_MAX_MODELS 64
#define SM_LOADED 20
#define SM_RANGE 14000.0f

typedef struct {
	u16 kind, flag_off, flag_on;
	u8 model;
	float pos[3], rot[3];
} Place;

typedef struct {
	char folder[8], name[24];
	Character* chr;          // NULL until loaded
	bool failed;
	u32 used;                // frame it was last drawn
} Model;

static Place s_places[SM_MAX_PLACES];
static int s_nplaces;
static Model s_models[SM_MAX_MODELS];
static int s_nmodels;
static int s_stage = -1;
static u32 s_frame;
static float s_play = -1.0f;      // seconds into the music box's tune, < 0 when it is not playing

static void free_models(void)
{
	for (int i = 0; i < s_nmodels; i++)
		if (s_models[i].chr) {
			chr_free(s_models[i].chr);
			free(s_models[i].chr);
		}
	memset(s_models, 0, sizeof s_models);
	s_nmodels = 0;
}

void stagemdl_update(int stage)
{
	if (stage == s_stage)
		return;
	s_stage = stage;
	free_models();
	s_nplaces = 0;
	FILE* f = fopen(PACK_DIR "/events/stagemdl.txt", "r");
	if (!f)
		return;
	char line[160];
	while (fgets(line, sizeof line, f) && s_nplaces < SM_MAX_PLACES) {
		int st, off, on;
		unsigned kind;
		char folder[8], name[24];
		Place p;
		if (sscanf(line, "%d %x %7s %23s %d %d %f %f %f %f %f %f", &st, &kind, folder, name, &off, &on, &p.pos[0],
		           &p.pos[1], &p.pos[2], &p.rot[0], &p.rot[1], &p.rot[2]) != 12 || st != stage)
			continue;
		int m = 0;
		while (m < s_nmodels && (strcmp(s_models[m].folder, folder) || strcmp(s_models[m].name, name)))
			m++;
		if (m == s_nmodels) {
			if (s_nmodels == SM_MAX_MODELS)
				continue;
			snprintf(s_models[m].folder, sizeof s_models[m].folder, "%s", folder);
			snprintf(s_models[m].name, sizeof s_models[m].name, "%s", name);
			s_nmodels++;
		}
		p.kind = (u16)kind, p.flag_off = (u16)off, p.flag_on = (u16)on, p.model = (u8)m;
		s_places[s_nplaces++] = p;
	}
	fclose(f);
}

void stagemdl_music_box(float seconds) { s_play = seconds; }

static bool load_model(Model* m)
{
	int loaded = 0, oldest = -1;
	for (int i = 0; i < s_nmodels; i++)
		if (s_models[i].chr) {
			loaded++;
			if (s_models[i].used != s_frame && (oldest < 0 || s_models[i].used < s_models[oldest].used))
				oldest = i;
		}
	if (loaded >= SM_LOADED) {
		if (oldest < 0)
			return false;
		chr_free(s_models[oldest].chr);
		free(s_models[oldest].chr);
		s_models[oldest].chr = NULL;
	}
	Character* c = malloc(sizeof *c);
	char dir[64], err[64];
	snprintf(dir, sizeof dir, PACK_DIR "/chr/%s", m->folder);
	if (!c || !chr_load(c, dir, m->name, err, sizeof err)) {
		free(c);
		m->failed = true;
		return false;
	}
	m->chr = c;
	return true;
}

static void mul34(Mtx34* o, const Mtx34* a, const Mtx34* b)
{
	Mtx34 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
		r.m[i][3] += a->m[i][3];
	}
	*o = r;
}

// A bone's rotation as the PC keeps it, x, y and z applied as z * y * x (chr.c's order). The PC
// reads it, changes a component and writes it back (0x538b80 / 0x538c30); the port adds to the
// components of the bind rotation, which are 0 for the parts moved here except bone 2 of the box.
static void turn(Mtx34* L, float ax, float ay, float az)
{
	float (*R)[4] = L->m;
	float x, y, z;
	if (fabsf(R[2][0]) < 0.9999f) {
		y = -asinf(R[2][0]);
		x = atan2f(R[2][1], R[2][2]);
		z = atan2f(R[1][0], R[0][0]);
	} else {
		y = R[2][0] < 0.0f ? (float)M_PI / 2.0f : -(float)M_PI / 2.0f;
		z = 0.0f;
		x = R[2][0] < 0.0f ? atan2f(R[0][1], R[0][2]) : atan2f(-R[0][1], -R[0][2]);
	}
	x += ax, y += ay, z += az;
	float cx = cosf(x), sx = sinf(x), cy = cosf(y), sy = sinf(y), cz = cosf(z), sz = sinf(z);
	R[0][0] = cz * cy; R[0][1] = cz * sy * sx - sz * cx; R[0][2] = cz * sy * cx + sz * sx;
	R[1][0] = sz * cy; R[1][1] = sz * sy * sx + cz * cx; R[1][2] = sz * sy * cx - cz * sx;
	R[2][0] = -sy;     R[2][1] = cy * sx;                R[2][2] = cy * cx;
}

// 0x57b5c0 (called with T = 15 seconds while the right order plays) as a function of the time t.
// The music box (0x524): bones 0, 6 and 7 turn by -pi/2 about y over the first tenth and back over
// the last (bone 8 the other way); bone 2 turns at 11.78 / T rad/s in between and ends at pi. The
// figure boxes on the seats (0x718..0x71a): from 1.5 to 13.5 s bone 0 turns about y by
// theta = (t - 1.5) * 11.78 / 15 and goes round a circle of 135, x = 135 sin(theta), z =
// 135 (cos(theta) - 1), stopping at theta = 3 pi. The stairway key in the box (0x747): bone 0 turns
// about z by theta and goes round x = -135 sin(theta - pi/4) - 103, y = 135 cos(theta - pi/4) -
// 103, stopping at theta = 11 pi / 4.
static void pose(Character* c, int kind, float t)
{
	static Mtx34 world[CHR_MAX_BONES];
	const float T = 15.0f, k = 15.707964f / T, k2 = 11.780972f / T;
	bool playing = t >= 0.0f && t < T;
	float lid = playing ? -k * fminf(t, 0.1f * T) + k * fmaxf(0.0f, t - 0.9f * T) : 0.0f;
	float spin = !playing || t <= 1.5f ? 0.0f : t >= 13.5f ? 3.0f * (float)M_PI : (t - 1.5f) * k2;
	for (u32 b = 0; b < c->n_bones; b++) {
		Mtx34 L = c->bind_local[b];
		if (kind == 0x524 && playing) {
			float a = b == 0 || b == 6 || b == 7 ? lid : b == 8 ? -lid :
			          b == 2 ? (t <= 0.1f * T ? 0.0f : t >= 0.9f * T ? (float)M_PI : k2 * (t - 0.1f * T)) : 0.0f;
			if (a != 0.0f)
				turn(&L, 0.0f, a, 0.0f);
		} else if (kind >= 0x718 && kind <= 0x71a && b == 0 && spin != 0.0f) {
			turn(&L, 0.0f, spin, 0.0f);
			L.m[0][3] = 135.0f * sinf(spin);
			L.m[2][3] = 135.0f * (cosf(spin) - 1.0f);
		} else if (kind == 0x747 && b == 0 && playing && t > 1.5f) {
			float th = t >= 13.5f ? 11.0f * (float)M_PI / 4.0f : spin;
			turn(&L, 0.0f, 0.0f, th);
			L.m[0][3] = -135.0f * sinf(th - (float)M_PI / 4.0f) - 103.0f;
			L.m[1][3] = 135.0f * cosf(th - (float)M_PI / 4.0f) - 103.0f;
		}
		if (c->parents[b] == 255)
			world[b] = L;
		else
			mul34(&world[b], &world[c->parents[b]], &L);
		mul34(&c->skin[b], &world[b], &c->inv_bind[b]);
	}
}

void stagemdl_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	stagemdl_update(ev_stage());
	if (!s_nplaces)
		return;
	s_frame++;
	float me[3], yaw;
	p_player(me, &yaw);
	bool loaded_one = false;
	for (int i = 0; i < s_nplaces; i++) {
		const Place* p = &s_places[i];
		// The PC checks flag_off when it loads a room's models, so the figure boxes that flag 472
		// (the right order) takes away stay for the tune they are part of.
		bool tune = s_play >= 0.0f && p->kind >= 0x718 && p->kind <= 0x71a;
		if ((p->flag_off && ev_flag(p->flag_off) && !tune) || (p->flag_on && !ev_flag(p->flag_on)))
			continue;
		float dx = p->pos[0] - me[0], dz = p->pos[2] - me[2];
		if (dx * dx + dz * dz > SM_RANGE * SM_RANGE)
			continue;
		if (demo_has_kind(p->kind))
			continue;                // the cutscene moves its own copy
		Model* m = &s_models[p->model];
		if (!m->chr) {
			if (m->failed || loaded_one)
				continue;
			loaded_one = true;
			if (!load_model(m))
				continue;
		}
		m->used = s_frame;
		C3D_Mtx model, mv;
		Mtx_Identity(&model);
		Mtx_Translate(&model, p->pos[0], p->pos[1], p->pos[2], true);
		Mtx_RotateZ(&model, p->rot[2], true);
		Mtx_RotateY(&model, p->rot[1], true);
		Mtx_RotateX(&model, p->rot[0], true);
		Mtx_Multiply(&mv, view, &model);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
		pose(m->chr, p->kind, s_play);
		chrlight_at(p->pos, false);
		chr_draw(m->chr, uBones);
	}
}
