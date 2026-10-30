#include "stagemdl.h"
#include "shadow.h"
#include "chr.h"
#include "chrlight.h"
#include "demo.h"
#include "events.h"
#include "prog_int.h"
#include "playtest.h"
#include "perf.h"
#include "stereo.h"
#include "stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"

// The placed models of the current stage (pack/events/stagemdl.txt, tools/sh2conv/stagemodels.py):
// the PC loads those in James' room (0x493430); the port draws the ones within SM_RANGE of him
// (fog hides the rest), keeping at most SM_LOADED models and dropping the one unused longest. The
// ones in range where James arrives load with the room, behind the fade (stagemdl_preload); one
// that comes into range later is read by the stream thread from SM_RANGE + SM_AHEAD on and set up
// once its files are in, one a frame, so no frame of play waits for the SD card. The whole list is
// read once, at the first stage, so a stage change does not read it again.
// The stage's pickups (drinks, kits, ampoules, rounds) are places too, as the room loader 0x490080
// puts their models down from the pickup list: the item's model from pack/events/itemmdl.txt, shown
// while the pickup lies there (ev_pickup_shown).
#define SM_MAX_PLACES 192
#define SM_MAX_MODELS 64
#define SM_LOADED 20
#define SM_RANGE 14000.0f
#define SM_AHEAD 3000.0f
#define SM_ALL 512

typedef struct {
	s16 stage;
	u16 kind, flag_off, flag_on;
	char folder[8], name[24];
	float pos[3], rot[3];
} Line;

typedef struct {
	u16 kind, flag_off, flag_on;
	u8 model;
	s16 pickup;              // the stage's pickup index, -1 for a placed model
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

// Maria lying in the day room S3 (stage 20, 0x58ef90): the PC places character 0x106 (hhh Maria) at
// 0x8db2cc facing pi with motion 0xd49 (72 frames, looping), pack/chr/mar/hhh_mar_s3.s3a (motion.py).
static struct {
	bool on, loaded, failed;
	Character chr;
	Anim anim;
	float t;
} s_bed;

static void bed_free(void)
{
	if (s_bed.loaded) {
		chr_free(&s_bed.chr);
		anim_free(&s_bed.anim);
	}
	s_bed.loaded = s_bed.failed = false;
}

void stagemdl_maria_bed(bool on) { s_bed.on = on; }

// Bones the stage code moves (the labyrinth lift's gates, 0x5831aa; the endings' falling stairs,
// 0x440470): added to the bone's place.
static struct { int kind, bone; float d[3]; } s_bone[12];

void stagemdl_bone_move(int kind, int bone, float dx, float dy, float dz)
{
	int k = 0;
	while (k < 11 && s_bone[k].kind && !(s_bone[k].kind == kind && s_bone[k].bone == bone))
		k++;
	s_bone[k].kind = kind, s_bone[k].bone = bone;
	s_bone[k].d[0] = dx, s_bone[k].d[1] = dy, s_bone[k].d[2] = dz;
}

static void free_models(void)
{
	for (int i = 0; i < s_nmodels; i++)
		if (s_models[i].chr) {
			chr_free(s_models[i].chr);
			free(s_models[i].chr);
		}
	memset(s_models, 0, sizeof s_models);
	s_nmodels = 0;
	bed_free();
}

static Line* s_all;
static int s_nall = -1;           // -1 until stagemdl.txt is read

static void read_list(void)
{
	if (s_nall >= 0)
		return;
	s_nall = 0;
	FILE* f = fopen(PACK_DIR "/events/stagemdl.txt", "r");
	if (!f)
		return;
	s_all = malloc(SM_ALL * sizeof *s_all);
	char line[160];
	while (s_all && fgets(line, sizeof line, f) && s_nall < SM_ALL) {
		int st, off, on;
		unsigned kind;
		Line* l = &s_all[s_nall];
		if (sscanf(line, "%d %x %7s %23s %d %d %f %f %f %f %f %f", &st, &kind, l->folder, l->name, &off, &on,
		           &l->pos[0], &l->pos[1], &l->pos[2], &l->rot[0], &l->rot[1], &l->rot[2]) != 12)
			continue;
		l->stage = (s16)st, l->kind = (u16)kind, l->flag_off = (u16)off, l->flag_on = (u16)on;
		// Enhanced Edition OldManCoinFix (Patches/OldManCoinFix.cpp, zlib licence): the Old Man Coin
		// in the garbage chute outside Wood Side (stage 13) shows only once James has thrown the
		// canned juice down it (flag 0x5f), as its pickup already requires
		if (st == 13 && kind == 0x71f && off == 0x76 && on == 0)
			l->flag_on = 0x5f;
		s_nall++;
	}
	fclose(f);
}

// Item -> its model (itemmdl.txt "item kind folder model"), read once.
static struct { u16 item, kind; char folder[8], name[24]; } s_itemmdl[96];
static int s_nitemmdl = -1;

static int item_model(int item)
{
	if (s_nitemmdl < 0) {
		s_nitemmdl = 0;
		FILE* f = fopen(PACK_DIR "/events/itemmdl.txt", "r");
		char line[96];
		while (f && fgets(line, sizeof line, f) && s_nitemmdl < 96) {
			int it;
			unsigned kind;
			if (sscanf(line, "%d %x %7s %23s", &it, &kind, s_itemmdl[s_nitemmdl].folder,
			           s_itemmdl[s_nitemmdl].name) == 4)
				s_itemmdl[s_nitemmdl].item = (u16)it, s_itemmdl[s_nitemmdl++].kind = (u16)kind;
		}
		if (f)
			fclose(f);
	}
	for (int i = 0; i < s_nitemmdl; i++)
		if (s_itemmdl[i].item == item)
			return i;
	return -1;
}

static int model_slot(const char* folder, const char* name)
{
	int m = 0;
	while (m < s_nmodels && (strcmp(s_models[m].folder, folder) || strcmp(s_models[m].name, name)))
		m++;
	if (m == s_nmodels) {
		if (s_nmodels == SM_MAX_MODELS)
			return -1;
		snprintf(s_models[m].folder, sizeof s_models[m].folder, "%s", folder);
		snprintf(s_models[m].name, sizeof s_models[m].name, "%s", name);
		s_nmodels++;
	}
	return m;
}

static void model_path(const char* folder, const char* name, char* path, size_t len)
{
	snprintf(path, len, PACK_DIR "/chr/%s/%s.s3c", folder, name);
}

void stagemdl_update(int stage)
{
	if (stage == s_stage)
		return;
	s_stage = stage;
	free_models();
	s_nplaces = 0;
	read_list();
	for (int i = 0; i < s_nall && s_nplaces < SM_MAX_PLACES; i++) {
		const Line* l = &s_all[i];
		if (l->stage != stage)
			continue;
		int m = model_slot(l->folder, l->name);
		if (m < 0)
			continue;
		Place p;
		memcpy(p.pos, l->pos, sizeof p.pos);
		memcpy(p.rot, l->rot, sizeof p.rot);
		p.kind = l->kind, p.flag_off = l->flag_off, p.flag_on = l->flag_on, p.model = (u8)m, p.pickup = -1;
		s_places[s_nplaces++] = p;
	}
	if (stage != ev_stage())
		return;
	for (int i = 0; i < ev_pickup_count() && s_nplaces < SM_MAX_PLACES; i++) {
		Place p = { .pickup = (s16)i };
		int item, k, m;
		if (!ev_pickup_place(i, &item, p.pos, &p.rot[1]) || (k = item_model(item)) < 0 ||
		    (m = model_slot(s_itemmdl[k].folder, s_itemmdl[k].name)) < 0)
			continue;
		p.kind = s_itemmdl[k].kind, p.model = (u8)m;
		s_places[s_nplaces++] = p;
	}
}

void stagemdl_prefetch(int stage, float x, float z, void (*want)(const char* path))
{
	read_list();
	char path[96];
	for (int i = 0; i < s_nall; i++) {
		const Line* l = &s_all[i];
		float dx = l->pos[0] - x, dz = l->pos[2] - z;
		if (l->stage != stage || dx * dx + dz * dz > SM_RANGE * SM_RANGE)
			continue;
		if (stage == s_stage) {          // loaded already?
			bool have = false;
			for (int m = 0; m < s_nmodels && !have; m++)
				have = s_models[m].chr && !strcmp(s_models[m].folder, l->folder) && !strcmp(s_models[m].name, l->name);
			if (have)
				continue;
		}
		model_path(l->folder, l->name, path, sizeof path);
		want(path);                      // the reader skips one it has queued already
		memcpy(path + strlen(path) - 4, ".kg1", 4);   // the shadow model chr_load also reads
		want(path);
	}
}

void stagemdl_music_box(float seconds) { s_play = seconds; }

static struct { int kind; float r[3]; } s_turn[4];

void stagemdl_turn(int kind, float rx, float ry, float rz)
{
	int k = 0;
	while (k < 3 && s_turn[k].kind && s_turn[k].kind != kind)
		k++;
	s_turn[k].kind = kind;
	s_turn[k].r[0] = rx, s_turn[k].r[1] = ry, s_turn[k].r[2] = rz;
}

static float s_dist;               // how far the place being loaded is from James (for the log)

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
		loaded--;
	}
	Character* c = malloc(sizeof *c);
	char dir[64], err[64];
	snprintf(dir, sizeof dir, PACK_DIR "/chr/%s", m->folder);   // chr_load takes what the reader read
	u64 t0 = svcGetSystemTick();
	if (!c || !chr_load(c, dir, m->name, err, sizeof err)) {
		free(c);
		m->failed = true;
		return false;
	}
	m->chr = c;
	perf_event("stagemdl", "%s/%s set up in %.1f ms, %d of %d loaded, %.0f away", m->folder, m->name,
	           perf_ms(svcGetSystemTick() - t0), loaded + 1, s_nmodels, s_dist);
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
		for (int k = 0; k < 12; k++)
			if (s_bone[k].kind == kind && s_bone[k].bone == (int)b)
				L.m[0][3] += s_bone[k].d[0], L.m[1][3] += s_bone[k].d[1], L.m[2][3] += s_bone[k].d[2];
		if (c->parents[b] == 255)
			world[b] = L;
		else
			mul34(&world[b], &world[c->parents[b]], &L);
		mul34(&c->skin[b], &world[b], &c->inv_bind[b]);
	}
}

static void bed_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	if (!s_bed.on || ev_stage() != 20 || s_bed.failed)
		return;
	if (!s_bed.loaded) {
		char err[64];
		if (!chr_load(&s_bed.chr, PACK_DIR "/chr/mar", "hhh_mar", err, sizeof err)) {
			s_bed.failed = true;
			return;
		}
		if (!anim_load(&s_bed.anim, PACK_DIR "/chr/mar/hhh_mar_s3.s3a")) {
			chr_free(&s_bed.chr);
			s_bed.failed = true;
			return;
		}
		s_bed.loaded = true;
		s_bed.t = 0.0f;
		if (playtest_active())
			playtest_log("S3: Maria in the bed (%lu frames)", (unsigned long)s_bed.anim.frames);
	}
	static const float pos[3] = { 99742.5625f, 203.282562f, 58623.1211f };   // 0x8db2cc
	if (!stereo_eye())               // once a frame, not once an eye (stereo.h)
		s_bed.t += 1.0f / 30.0f;
	float frame = fmodf(s_bed.t * 30.0f, (float)(s_bed.anim.frames ? s_bed.anim.frames : 1));
	C3D_Mtx model, mv;
	Mtx_Identity(&model);
	Mtx_Translate(&model, pos[0], pos[1], pos[2], true);
	Mtx_RotateY(&model, 3.1415927f, true);
	Mtx_Multiply(&mv, view, &model);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
	chr_pose_loop(&s_bed.chr, &s_bed.anim, frame, 0, (int)s_bed.anim.frames - 1);
	chrlight_at(pos, false);
	chr_draw(&s_bed.chr, uBones);
}

// The PC sets placed models up as "stay objects" (0x537cf0 picks the handler by kind): the 0x3xx and
// 0x5xx kinds (0x555120), the 0x4xx ones but for 0x421 and the mirrored copies 0x443, 0x444 and 0x460
// (0x53b780), and the items, 0x7xx (0x554840), go through 0x554710, which multiplies every bone's bind
// matrix by a turn of (0, pi, 0) (0x5546b0, 0x40490fdb) before the model's own place and turn, and
// scales the items by kind (0x554840: the keys and the like 2, the coins and some keys 1.5, ... into
// +0x29c, 0x538ac0). The 0x6xx kinds (0x554630) do neither.
static bool stay_turned(int kind)
{
	int k = kind >> 8;
	return k == 3 || k == 5 || k == 7 || (k == 4 && kind != 0x421 && kind != 0x443 && kind != 0x444 && kind != 0x460);
}

static float stay_scale(int kind)
{
	switch (kind) {
	case 0x706: case 0x707: case 0x709: case 0x70a: case 0x728: case 0x73f: case 0x74b: return 2.0f;
	case 0x712: case 0x717: return 1.3f;
	case 0x714: case 0x71c: case 0x72c: case 0x730: case 0x732: case 0x736: case 0x73d: case 0x747: return 1.5f;
	case 0x72f: return 2.25f;
	case 0x735: return 2.5f;
	case 0x73e: return 3.0f;
	}
	return 1.0f;
}

// Whether a place shows a model now (its flags, the cutscene), and its squared distance from x, z.
static bool shown(const Place* p, float x, float z, float* d2)
{
	if (p->pickup >= 0) {
		if (!ev_pickup_shown(p->pickup))
			return false;
		float dx = p->pos[0] - x, dz = p->pos[2] - z;
		*d2 = dx * dx + dz * dz;
		return true;
	}
	// The PC checks flag_off when it loads a room's models, so the figure boxes that flag 472
	// (the right order) takes away stay for the tune they are part of.
	bool tune = s_play >= 0.0f && p->kind >= 0x718 && p->kind <= 0x71a;
	if ((p->flag_off && ev_flag(p->flag_off) && !tune) || (p->flag_on && !ev_flag(p->flag_on)))
		return false;
	if (demo_has_kind(p->kind))
		return false;                // the cutscene moves its own copy
	float dx = p->pos[0] - x, dz = p->pos[2] - z;
	*d2 = dx * dx + dz * dz;
	return true;
}

void stagemdl_preload(int stage, float x, float z)
{
	stagemdl_update(stage);
	for (int i = 0; i < s_nplaces; i++) {
		const Place* p = &s_places[i];
		Model* m = &s_models[p->model];
		float d2;
		if (m->chr || m->failed || !shown(p, x, z, &d2) || d2 > SM_RANGE * SM_RANGE)
			continue;
		s_dist = sqrtf(d2);
		if (load_model(m))
			m->used = s_frame;
	}
}

void stagemdl_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	// the second eye of a 3D frame draws what the first one did and loads nothing (stereo.h)
	bool second = stereo_eye() != 0;
	if (!second)
		stagemdl_update(ev_stage());
	bed_draw(view, uModelView, uBones);
	if (!s_nplaces)
		return;
	if (!second)
		s_frame++;
	float me[3], yaw;
	p_player(me, &yaw);
	bool loaded_one = second;
	const float ahead = (SM_RANGE + SM_AHEAD) * (SM_RANGE + SM_AHEAD);
	// first the loaded models in range or about to be: none of them is the one a load drops
	for (int i = 0; i < s_nplaces && !second; i++) {
		float d2;
		if (s_models[s_places[i].model].chr && shown(&s_places[i], me[0], me[2], &d2) && d2 <= ahead)
			s_models[s_places[i].model].used = s_frame;
	}
	for (int i = 0; i < s_nplaces; i++) {
		const Place* p = &s_places[i];
		float d2;
		if (!shown(p, me[0], me[2], &d2) || d2 > ahead)
			continue;
		Model* m = &s_models[p->model];
		if (!m->chr) {
			if (m->failed || loaded_one)
				continue;
			// read by the stream thread first; set up (one a frame) once it and its textures are in
			char path[96];
			model_path(m->folder, m->name, path, sizeof path);
			int st = stream_file_state(path);
			if (st == -1) {
				stream_want_file(path);
				memcpy(path + strlen(path) - 4, ".kg1", 4);
				stream_want_file(path);
			}
			if (st == -1 || (st >= 0 && stream_files_busy()))
				continue;
			loaded_one = true;
			s_dist = sqrtf(d2);
			if (!load_model(m))
				continue;
		}
		m->used = s_frame;
		if (d2 > SM_RANGE * SM_RANGE)
			continue;
		C3D_Mtx model, mv;
		Mtx_Identity(&model);
		Mtx_Translate(&model, p->pos[0], p->pos[1], p->pos[2], true);
		float r[3] = { p->rot[0], p->rot[1], p->rot[2] };
		for (int k = 0; k < 4; k++)
			if (s_turn[k].kind == p->kind)
				r[0] += s_turn[k].r[0], r[1] += s_turn[k].r[1], r[2] += s_turn[k].r[2];
		Mtx_RotateZ(&model, r[2], true);
		Mtx_RotateY(&model, r[1], true);
		Mtx_RotateX(&model, r[0], true);
		float k = stay_scale(p->kind);
		if (k != 1.0f)
			Mtx_Scale(&model, k, k, k);
		if (stay_turned(p->kind))
			Mtx_RotateY(&model, 3.1415927f, true);
		Mtx_Multiply(&mv, view, &model);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
		pose(m->chr, p->kind, s_play);
		shadow_cast_placed(m->chr, &model, p->kind);   // its .kg1's shadow, as the PC's characters (shadow.c)
		chrlight_at(p->pos, false);
		chr_draw(m->chr, uBones);
	}
}
