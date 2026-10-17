#include "chrlight.h"
#include "maria.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bgm.h"
#include "chr.h"
#include "events.h"
#include "sfx.h"

// The PC keeps all of this in shMariaWork (sh2mar at 0x1fb3860). Main states (0x8bced0): stand,
// close to James, alert, discover, recover, boredom, damaged. Sub states (0x8bcef0) pick the
// motion: stand, relax, relax off, afraid, tired, one step, walk, run, damage. The port has all
// but the one step (her shuffle when James bumps into her) and the looking about.

#define PI 3.14159265f
#define TRAIL 12                  // DAT_01fb38b8: James' last positions, one every half second
#define RADIUS 150.0f             // her hit column (DAT_01fb3ae8)

enum { SUB_STAND, SUB_RELAX, SUB_RELAX_OFF, SUB_AFRAID, SUB_TIRED, SUB_WALK, SUB_RUN, SUB_DAMAGE, SUB_DYING };

// lll_mar.anm (pmaria_anim, ids from 11): {start, end, loop, speed (2048 = 30 fps)}
typedef struct { u16 start, end; u8 loop; s16 speed; } MarAnim;
static const MarAnim ANIM[] = {
	{ 0, 9, 1, 384 },        // 0 id 11 stands, breathing
	{ 10, 33, 1, 1536 },     // 1 id 12 walks
	{ 34, 57, 1, 1792 },     // 2 id 13 runs
	{ 58, 77, 1, 1024 },     // 3 id 14 tired, hands on knees
	{ 204, 218, 0, 1024 },   // 4 id 20 hit from the front
	{ 219, 233, 0, 1024 },   // 5 id 21 hit from behind
	{ 234, 248, 0, 1024 },   // 6 id 22 hit from the left
	{ 249, 263, 0, 1024 },   // 7 id 23 hit from the right
	{ 264, 293, 0, 512 },    // 8 id 24 falls
	{ 78, 108, 0, 512 },     // 9 id 15 afraid: backs off, arms up
	{ 109, 138, 0, 1024 },   // 10 id 16 settles into a relaxed stance
	{ 109, 138, 0, -3072 },  // 11 id 17 the same backwards, quickly: ready to go again
	{ 139, 148, 1, 128 },    // 12 id 18 relaxed, hand on hip
};
enum { MA_STAND, MA_WALK, MA_RUN, MA_TIRED, MA_HIT, MA_FALL = 8, MA_AFRAID, MA_RELAX_IN, MA_RELAX_OFF, MA_RELAX };

static char s_pack[64];
static Character s_chr;
static Anim s_anim;
static bool s_loaded, s_tried;
static const Collision* s_col;

static struct {
	bool here;               // in James' room and following (active type 3)
	bool waiting;            // placed by the appear table but not following (types 0 / 1)
	bool place;              // a room was loaded: put her somewhere on the next update
	bool with;               // flag 15 as of the last update
	int room, room_prev;     // PC room ids (FUN_00494db0) at James' feet
	int wait_room;           // active type 2: waits until James is in this room past x -86700
	float x, y, z, yaw;
	float speed;             // EnLOCAL-like spd (+0xa8): 1.4 walking, 3.5 running, 500 units each
	int sub;
	int anim;
	float frame;
	bool anim_end;
	float hp, hp_max;
	float invuln;            // DAT_01fb3b28: two seconds after a hit
	float sub_time;          // DAT_01fb3b24: time in the walk / run sub state
	float dead_time;
	float trail[TRAIL][3];
	float trail_time;        // DAT_01fb3b34
	float stand_time;        // DAT_01fb3b20: time standing about (relaxes after 6 s)
	float radius;            // her column: 150, 50 when resting (active type 1)
	float tx, tz;            // where she heads (DAT_01fb38a8)
	float step_snd;
	float last_px, last_pz;
	u32 drawn, updated;
} M;

// Appear points (maria_apeear_point_list, 0x6cd6d0): coming into room `now` from room `prev`, she
// stands at x/y/z facing yaw. Type 3 follows; 0 waits there; 1 rests there (the hospital bed; the
// port leaves her out of sight); 2 waits until James walks on (room 8 only). prev -1 is a loaded
// save. Rooms not listed put her 500 units off James' right shoulder, following.
typedef struct { s16 prev, now; u8 type; float x, y, z, yaw; } Appear;
static const Appear APPEAR[] = {
	{ 33, 51, 3, 4609, 3000, 104185, -1.61f }, { 8, 51, 3, -20426, 0, -16729, 0.70f },
	{ 51, 44, 0, -19305, 0, -59868, -2.98f }, { 44, 51, 3, -21330, 0, -17805, 1.48f },
	{ 44, 45, 0, -59966, 0, -19476, -2.88f }, { 45, 44, 0, -20744, 0, -60175, 0.90f },
	{ 51, 41, 3, -60750, 113, -100597, 1.68f }, { 41, 51, 3, -24139, 0, -23109, 1.39f },
	{ 41, 59, 3, -104104, 0, 16973, 1.65f }, { 59, 41, 3, -60762, -1879, -100598, 1.63f },
	{ 59, 53, 0, -20933, 0, 59853, 1.44f }, { 53, 59, 3, -102973, 0, 21799, -3.09f },
	{ 59, 54, 0, -19371, 0, 20200, -1.45f }, { 54, 59, 3, -102304, 0, 21099, -3.03f },
	{ 59, 60, 3, -51551, 0, 100445, -2.06f }, { 60, 59, 3, -105302, 0, 16401, -2.06f },
	{ 60, 55, 3, -60206, 0, 58799, 1.25f }, { 55, 60, 3, -53414, 0, 100586, -1.45f },
	{ 60, 56, 0, -139990, 0, 57214, 0.00f }, { 56, 60, 3, -56359, 0, 99946, -1.76f },
	{ 60, 57, 3, -60601, 0, 19323, 1.76f }, { 57, 60, 3, -59260, 0, 99893, -1.76f },
	{ 60, 58, 0, -138851, 0, 18407, -1.70f }, { 58, 60, 3, -68617, 0, 99864, 1.00f },
	{ 41, 68, 3, 56990, 0, 16987, -1.50f }, { 68, 41, 3, -60676, -3934, -100591, 1.91f },
	{ 68, 69, 3, 68455, 0, 60570, -1.55f }, { 69, 68, 3, 54600, 0, 16404, 1.59f },
	{ 69, 64, 0, 140133, 0, 59236, -2.46f }, { 64, 69, 3, 60322, 0, 60580, 1.77f },
	{ 69, 67, 1, 139570, 0, 98827, 1.67f }, { 67, 69, 3, 50819, 0, 60100, 1.92f },
	{ 69, 66, 1, 100120, 0, 18450, 0.00f }, { 66, 69, 3, 54266, 0, 59839, 2.19f },
	{ 51, 47, 1, -20350, 0, -100252, -1.44f }, { 47, 51, 3, -22173, 0, -20754, -0.04f },
	{ 47, 46, 1, -58361, 0, -61300, -0.80f }, { 46, 47, 1, -20350, 0, -100252, -1.44f },
	{ 51, 46, 1, -58361, 0, -61300, -0.80f }, { 46, 51, 3, -17830, 0, -20201, 2.91f },
	{ 71, 89, 0, 19479, 0, -221432, 1.44f }, { 89, 71, 3, -59404, 2000, -220010, -0.93f },
	{ 89, 90, 0, 19483, 0, -302019, 0.70f }, { 90, 89, 0, 19479, 0, -221432, 1.44f },
	{ 87, 88, 3, 14600, 0, -180870, 1.63f }, { 88, 87, 3, 70126, 0, -179830, -1.65f },
	{ 87, 85, 1, 59775, 0, -140650, 1.63f }, { 85, 87, 3, 66461, 0, -180568, -1.69f },
	{ 87, 86, 1, 99669, 0, -180657, 1.71f }, { 86, 87, 3, 55759, 0, -180573, 1.40f },
	{ 87, 72, 3, -100591, -4000, -219234, -3.13f }, { 72, 87, 3, 59767, 0, -179031, 2.23f },
	{ 72, 91, 3, 19793, 0, -260207, -2.34f }, { 91, 72, 3, -100213, 3400, -213986, -0.62f },
	{ 88, 71, 3, -60816, -3879, -220593, 1.85f }, { 71, 88, 3, 15954, 0, -180231, 1.39f },
	{ 87, 70, 3, -19683, 0, -299856, -2.42f }, { 70, 87, 3, 59409, 0, -179005, 2.46f },
	{ 80, 70, 3, -19683, 0, -299856, -2.42f }, { 70, 80, 3, -60580, 0, -139010, 2.58f },
	{ 78, 70, 3, -19683, 0, -299856, -2.42f }, { 70, 78, 3, -104838, 0, -259120, -1.41f },
	{ 88, 84, 0, 20017, 0, -137803, 2.84f }, { 84, 88, 3, 18952, 0, -180201, 2.98f },
	{ 78, 79, 3, -99502, 0, -302003, 3.05f }, { 79, 78, 3, -104888, 0, -260621, -0.66f },
	{ 80, 82, 0, -19434, 0, -141449, 1.92f }, { 82, 80, 3, -67027, 0, -140544, 1.59f },
	{ 80, 81, 0, -59253, 0, -180733, -2.33f }, { 81, 80, 3, -60456, 0, -140561, -1.68f },
	{ 80, 83, 3, -22306, 0, -180981, 3.06f }, { 83, 80, 3, -49819, 0, -140594, -0.22f },
	{ 79, 73, 3, -68526, -23, -300024, 2.42f }, { 73, 79, 3, -99600, 0, -300256, -2.42f },
	{ 78, 76, 0, -139394, 0, -221693, -1.73f }, { 76, 78, 3, -99568, 0, -260646, 0.56f },
	{ 78, 75, 0, -140861, 0, -261616, 1.55f }, { 75, 78, 3, -96298, 0, -260603, -1.36f },
	{ 9, 8, 3, -91589, 0, 66786, -1.36f }, { 8, 12, 3, 19630, 0, 21438, -3.05f },
	{ 12, 8, 3, -98778, 0, 32804, -0.09f }, { 12, 13, 3, -22675, 0, 22281, 2.35f },
	{ 13, 12, 3, 22616, -2125, 22398, -2.98f }, { 13, 8, 2, -86972, 0, 32485, -1.25f },
	{ 8, 13, 3, -17213, 0, 19587, -1.57f },
	{ -1, 44, 0, -19305, 0, -59868, -2.98f }, { -1, 66, 1, 100120, 0, 18450, 0.00f },
	{ -1, 71, 3, -60233, -4000, -219437, -2.29f }, { -1, 79, 3, -100561, 0, -302945, 0.62f },
	{ -1, 8, 3, -10570, 0, 45941, 1.11f },
};

static float wrap(float a)
{
	while (a > PI) a -= 2.0f * PI;
	while (a < -PI) a += 2.0f * PI;
	return a;
}

static bool clear_line(float x0, float z0, float x1, float z1, float y)
{
	if (!s_col)
		return true;
	float cx = x1, cz = z1;
	col_clamp_ray(s_col, x0, z0, &cx, &cz, y - 300.0f);
	return (cx - x1) * (cx - x1) + (cz - z1) * (cz - z1) < 1.0f;
}

static void set_anim(int a)
{
	if (M.anim != a) {
		M.anim = a;
		M.frame = ANIM[a].speed < 0 ? ANIM[a].end : ANIM[a].start;
		M.anim_end = false;
	}
}

static void step_anim(float rate, float dt)
{
	const MarAnim* t = &ANIM[M.anim];
	float len = (float)(t->end - t->start + 1);
	M.frame += dt * 30.0f * (t->speed / 2048.0f) * rate;
	if (t->loop) {
		while (M.frame >= t->end + 1.0f) M.frame -= len;
	} else if (M.frame >= t->end) {
		M.frame = t->end;
		M.anim_end = true;
	} else if (M.frame <= t->start && t->speed < 0) {
		M.frame = t->start;
		M.anim_end = true;
	}
}

void maria_init(const char* pack)
{
	snprintf(s_pack, sizeof s_pack, "%s", pack);
	maria_reset();
}

void maria_exit(void)
{
	if (s_loaded) {
		chr_free(&s_chr);
		anim_free(&s_anim);
	}
	s_loaded = s_tried = false;
}

static bool load(void)
{
	if (s_tried)
		return s_loaded;
	s_tried = true;
	char dir[96], err[128], path[128];
	snprintf(dir, sizeof dir, "%s/chr/mar", s_pack);
	if (!chr_load(&s_chr, dir, "lll_mar", err, sizeof err))
		return false;
	snprintf(path, sizeof path, "%s/lll_mar.s3a", dir);
	if (!anim_load(&s_anim, path) || s_anim.bones != s_chr.n_bones) {
		chr_free(&s_chr);
		return false;
	}
	s_loaded = true;
	return true;
}

// 0x53a170: 160 HP on beginner and easy, 80 on normal and hard.
void maria_reset(void)
{
	memset(&M, 0, sizeof M);
	M.room = M.room_prev = -1;
	M.hp = M.hp_max = enemy_difficulty() <= 1 ? 160.0f : 80.0f;
	M.radius = RADIUS;
	M.place = true;
}

void maria_room_enter(const Collision* col)
{
	s_col = col;
	M.place = true;
	M.here = M.waiting = false;
}

static void fill_trail(float x, float z)
{
	for (int i = 0; i < TRAIL; i++)
		M.trail[i][0] = x, M.trail[i][2] = z;
}

// MariaInitOnConnect (0x52d550): where she is after James came through a door.
static void place(const EnPlayer* p)
{
	M.here = M.waiting = false;
	M.wait_room = -1;
	if (!s_col || !load())
		return;
	const Appear* a = NULL;
	for (u32 i = 0; i < sizeof APPEAR / sizeof APPEAR[0]; i++)
		if (APPEAR[i].now == M.room && APPEAR[i].prev == M.room_prev) {
			a = &APPEAR[i];
			break;
		}
	float x, z, y = p->y, yaw;
	int type = 3;
	if (a) {
		x = a->x, z = a->z, y = a->y, yaw = a->yaw, type = a->type;
	} else {
		x = p->x + sinf(p->yaw + PI / 4.0f) * 500.0f;
		z = p->z + cosf(p->yaw + PI / 4.0f) * 500.0f;
		yaw = wrap(p->yaw - PI);
	}
	// Type 1 (the hospital rooms and two others) keeps her where she rests, with a 50-unit column
	// (0x52b330); lll_mar.anm has no lying pose, so she stands there.
	M.radius = type == 1 ? 50.0f : RADIUS;
	float fy;
	if (a && type != 3 && !col_floor(s_col, x, z, y, 3000.0f, &fy))
		return;                     // waiting in another part of the area
	if (!col_floor(s_col, x, z, y, 3000.0f, &fy)) {
		// The spot is outside the loaded map (the port loads the town in pieces) or in a wall:
		// the shoulder spot, else James' feet.
		x = p->x + sinf(p->yaw + PI / 4.0f) * 500.0f;
		z = p->z + cosf(p->yaw + PI / 4.0f) * 500.0f;
		yaw = wrap(p->yaw - PI);
		if (!col_floor(s_col, x, z, p->y, 3000.0f, &fy)) {
			if (!col_floor(s_col, p->x, p->z, p->y, 3000.0f, &fy))
				return;
			x = p->x, z = p->z;
		}
	}
	M.x = x, M.y = fy, M.z = z, M.yaw = yaw;
	M.speed = 0.0f;
	M.sub = SUB_STAND;
	set_anim(MA_STAND);
	fill_trail(p->x, p->z);
	M.tx = p->x, M.tz = p->z;
	if (type == 3) M.here = true;
	else if (type == 2) M.waiting = true, M.wait_room = 8;
	else M.waiting = true;
}

// MariaSetStraightRoot (0x52adc0): heads for James when nothing stands between them, else for the
// newest point of his trail she can see; the trail gets a point every half second he moves.
static void route(const EnPlayer* p, float dt)
{
	if (p->x != M.last_px || p->z != M.last_pz)
		M.trail_time += dt;
	M.last_px = p->x, M.last_pz = p->z;
	// a new point when half a second of walking has passed, or at once when James went round a
	// corner from the last one
	if (M.trail_time >= 0.5f || !clear_line(M.trail[0][0], M.trail[0][2], p->x, p->z, M.y)) {
		memmove(&M.trail[1], &M.trail[0], sizeof M.trail[0] * (TRAIL - 1));
		M.trail[0][0] = p->x, M.trail[0][2] = p->z;
		M.trail_time = M.trail_time < 0.5f ? 0.0f : M.trail_time - 0.5f;
	}
	if (clear_line(M.x, M.z, p->x, p->z, M.y)) {
		M.tx = p->x, M.tz = p->z;
		return;
	}
	int i = 0;
	while (i < TRAIL - 1 && !clear_line(M.x, M.z, M.trail[i][0], M.trail[i][2], M.y))
		i++;
	M.tx = M.trail[i][0], M.tz = M.trail[i][2];
	if (hypotf(M.tx - M.x, M.tz - M.z) < 150.0f) {
		M.x = M.tx, M.z = M.tz;     // reached a trail point
	}
}

// 0x529d60: turns toward a heading, 15 * 0.323 of the difference a second.
static void turn_to(float target, float dt)
{
	float d = wrap(target - M.yaw);
	float step = dt * 15.0f * 0.32345787f * d;
	if (fabsf(step) >= fabsf(d)) M.yaw = target;
	else M.yaw = wrap(M.yaw + step);
}

static void move(float dt)
{
	if (M.speed <= 0.0f || !s_col)
		return;
	float dir = atan2f(M.tx - M.x, M.tz - M.z);
	float v = M.speed * 500.0f * dt;
	float nx = M.x + sinf(dir) * v, nz = M.z + cosf(dir) * v;
	float ny;
	col_push(s_col, &nx, &nz, M.y, 1500.0f, M.radius);
	if (col_floor(s_col, nx, nz, M.y, 200.0f, &ny))
		M.x = nx, M.z = nz, M.y = ny;
}

// Footsteps on the walk and run cycles (MariaCheckSoundLower, SE 0x4a57 on the default floor).
static void footsteps(float f0)
{
	if (M.anim != MA_WALK && M.anim != MA_RUN)
		return;
	const MarAnim* t = &ANIM[M.anim];
	float half = (t->end - t->start + 1) * 0.5f;
	float a = f0 - t->start, b = M.frame - t->start;
	if (b < a) b += 2.0f * half;
	if ((a < 1.0f && b >= 1.0f) || (a < 1.0f + half && b >= 1.0f + half)) {
		float pos[3] = { M.x, M.y, M.z };
		sfx_play_at(19031, M.anim == MA_RUN ? 0.5f : 0.35f, pos);
	}
}

void maria_update(const EnPlayer* p, float dt)
{
	M.updated++;
	if (!ev_flag(15) || !bgm_ready()) {
		M.here = M.waiting = M.with = false;
		return;
	}
	int room = bgm_room_id(ev_glb_crd(), p->x, p->z);
	if (!M.with) {
		// a stage program just made her come along (the PC creates her in the program): she
		// starts next to James
		M.with = true;
		M.place = true;
		M.room = M.room_prev = room;
	}
	if (M.place) {
		if (room != M.room)
			M.room_prev = M.room;
		M.room = room;
		M.place = false;
		place(p);
	} else if (room != M.room) {
		M.room_prev = M.room, M.room = room;
	}
	if (M.waiting && M.wait_room >= 0 && M.room == M.wait_room && p->x > -86700.0f) {
		M.waiting = false, M.here = true;   // 0x52ba80: active type 2 becomes 3
		fill_trail(p->x, p->z);
	}
	if (!M.here && !M.waiting)
		return;
	float f0 = M.frame;
	if (M.invuln > 0.0f) M.invuln -= dt;
	float dist = hypotf(p->x - M.x, p->z - M.z);
	if (M.sub == SUB_DYING) {
		step_anim(1.0f, dt);
		if (M.anim_end) M.dead_time += dt;
		return;
	}
	if (M.sub == SUB_DAMAGE) {
		step_anim(1.0f, dt);
		M.speed = 0.0f;
		if (M.anim_end) {
			M.sub = SUB_STAND;
			M.invuln = 2.0f;
			set_anim(MA_STAND);
		}
		return;
	}
	if (M.waiting) {
		turn_to(atan2f(p->x - M.x, p->z - M.z), dt * 0.3f);
		step_anim(1.0f, dt);
		return;
	}
	route(p, dt);
	bool see = M.tx == p->x && M.tz == p->z;
	// 0x52b8d0 / 0x52a280: run when 2000 away, walk when 1000 away or out of sight, else stand
	// (catching her breath when hurt: tired, 0x52a810, half a point a second)
	// standing: tired when hurt (main state 4), afraid with an enemy about (2, alert: the PC asks
	// the radio's enemy flag, the port an enemy within 3000), relaxed after 6 s of nothing (5)
	int want = dist >= 2000.0f ? SUB_RUN : (dist > 1000.0f || !see) ? SUB_WALK
	         : M.hp < M.hp_max * 0.5f ? SUB_TIRED
	         : enemy_nearest(M.x, M.z) < 3000.0f ? SUB_AFRAID
	         : M.stand_time >= 6.0f ? SUB_RELAX : SUB_STAND;
	if (M.sub == SUB_RELAX && want != SUB_RELAX) {
		want = SUB_RELAX_OFF;       // 0x52a700: the relaxed stance unwinds first
	} else if (M.sub == SUB_RELAX_OFF) {
		want = M.anim_end ? (want == SUB_RELAX ? SUB_STAND : want) : SUB_RELAX_OFF;
	}
	if (want != M.sub) {
		// walking and running keep on for at least 1.06 / 0.91 s (0x6cdf80 / 0x6cdf84)
		bool hold = (M.sub == SUB_WALK && M.sub_time < 1.06f && want < SUB_WALK) ||
		            (M.sub == SUB_RUN && M.sub_time < 0.91f && want < SUB_WALK);
		if (!hold) {
			M.sub = want;
			M.sub_time = 0.0f;
		}
	}
	M.sub_time += dt;
	M.stand_time = (M.sub == SUB_STAND || M.sub == SUB_RELAX) ? M.stand_time + dt : 0.0f;
	float rate = 1.0f;
	switch (M.sub) {
	case SUB_WALK:
		// 0x52aa00: speeds up to 1.4 at 2 a second
		M.speed = M.speed < 1.4f ? fminf(M.speed + dt * 2.0f, 1.4f) : fmaxf(M.speed - dt * 4.0f, 1.4f);
		set_anim(MA_WALK);
		rate = M.speed / 1.12f;          // 561 units/s at rate 1 (measured from lll_mar.anm)
		break;
	case SUB_RUN: {
		// 0x52ab10: up to 3.5 at 3 a second, 5.0 once she is 6000 behind (after falling 7000)
		float top = dist >= 6000.0f ? 5.0f : 3.5f;
		M.speed = M.speed < top ? fminf(M.speed + dt * 3.0f, top) : fmaxf(M.speed - dt * 3.0f, top);
		set_anim(MA_RUN);
		rate = M.speed / 3.5f;
		break;
	}
	case SUB_RELAX:
		M.speed = fmaxf(M.speed - dt * 8.0f, 0.0f);
		if (M.anim != MA_RELAX_IN && M.anim != MA_RELAX) set_anim(MA_RELAX_IN);
		else if (M.anim == MA_RELAX_IN && M.anim_end) set_anim(MA_RELAX);
		break;
	case SUB_RELAX_OFF:
		M.speed = 0.0f;
		set_anim(MA_RELAX_OFF);
		break;
	case SUB_AFRAID:
		M.speed = fmaxf(M.speed - dt * 8.0f, 0.0f);
		set_anim(MA_AFRAID);         // plays once and holds
		break;
	case SUB_TIRED:
		M.speed = fmaxf(M.speed - dt * 8.0f, 0.0f);
		set_anim(MA_TIRED);
		M.hp = fminf(M.hp + dt * 0.5f, M.hp_max);
		break;
	default:
		M.speed = fmaxf(M.speed - dt * 8.0f, 0.0f);   // 0x529e10
		set_anim(MA_STAND);
		break;
	}
	float head = M.speed > 0.0f ? atan2f(M.tx - M.x, M.tz - M.z) : atan2f(p->x - M.x, p->z - M.z);
	turn_to(head, dt);
	move(dt);
	step_anim(rate, dt);
	footsteps(f0);
}

bool maria_hit(float x, float z, float yaw, float reach, float arc, float ap)
{
	if (!M.here || M.sub >= SUB_DAMAGE || M.invuln > 0.0f)
		return false;
	float dx = M.x - x, dz = M.z - z;
	if (sqrtf(dx * dx + dz * dz) > reach + M.radius || fabsf(wrap(atan2f(dx, dz) - yaw)) > arc)
		return false;
	M.hp -= ap;
	float pos[3] = { M.x, M.y, M.z };
	M.speed = 0.0f;
	if (M.hp <= 0.0f) {
		// 0x52c750 with the dead flag: the falling motion, then game over
		M.hp = 0.0f;
		M.sub = SUB_DYING;
		set_anim(MA_FALL);
		sfx_play_at(19041, 1.0f, pos);
		return true;
	}
	// MariaDamageMotionNo: front, back, left or right of her
	float side = wrap(atan2f(-dx, -dz) - M.yaw);
	int a = fabsf(side) < PI / 4.0f ? 0 : fabsf(side) > 3.0f * PI / 4.0f ? 1 : side > 0.0f ? 3 : 2;
	M.sub = SUB_DAMAGE;
	set_anim(MA_HIT + a);
	sfx_play_at(19040, 0.7f, pos);
	return true;
}

bool maria_present(void) { return M.here || M.waiting; }

bool maria_target(const EnPlayer* james, EnPlayer* out)
{
	if (!M.here || M.sub == SUB_DYING)
		return false;
	*out = (EnPlayer){ M.x, M.y, M.z, M.yaw, false, true, james->stomping, true };
	return true;
}
float maria_distance(void) { return M.here ? hypotf(M.last_px - M.x, M.last_pz - M.z) : -1.0f; }
bool maria_dead(void) { return M.sub == SUB_DYING && M.dead_time > 1.5f; }

void maria_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	// Stage programs stop the world (enemy_update is not called) while they hold James for a demo,
	// and the demos show their own Maria: she is left out when she was not updated since the last
	// frame.
	bool fresh = M.updated != M.drawn;
	M.drawn = M.updated;
	if (!fresh || !maria_present() || !s_loaded)
		return;
	C3D_Mtx model, mv;
	Mtx_Identity(&model);
	Mtx_Translate(&model, M.x, M.y, M.z, true);
	Mtx_RotateY(&model, M.yaw, true);
	Mtx_Multiply(&mv, view, &model);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
	chr_pose(&s_chr, &s_anim, M.frame);
	float at[3] = { M.x, M.y, M.z };
	chrlight_at(at, false);
	chr_draw(&s_chr, uBones);
}

const char* maria_status(void)
{
	static char line[24];
	if (!maria_present())
		line[0] = 0;
	else
		snprintf(line, sizeof line, "M%d.%d/%.0f ", M.room, M.sub, M.hp);
	return line;
}
