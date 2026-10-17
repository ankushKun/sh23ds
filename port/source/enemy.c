// Enemies: stage placement, the Lying Figure AI and drawing.
//
// The Lying Figure is kind 0x200 in the Enemy_List and uses chr/scu (the PC kind table at 0x88d408
// maps 0x200 to data/chr/scu/scu.mdl). Its control code is the PC port of the PS2 en_scu.c:
//   0x4c7070 enSCUCtrlMain        0x4c6f50 enSCUCtrlAutomatic   0x4c46c0 enSCUInitData
//   0x4c4b30 Wander  0x4c4f50 Precaution  0x4c5070 Chase  0x4c5230 Backward  0x4c5370 Ambush
//   0x4c5740 WaitCar 0x4c5800 Crawl       0x4c63f0 Attack 0x4c65d0 Damage    0x4c6840 Confuse
//   0x4c6900 Down    0x4a96c0 enDyingExec (en_common)
// Helpers come from en_common.c (0x4a5700..0x4aab00). Names follow the PS2 beta debug info; the
// PC addresses are given at each function. Things that need systems the port does not have yet
// (ambush tables, the waiting-for-car event, regeneration, path finding around furniture) are
// simplified and marked as such.
#include "chrlight.h"
#include "enemy.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chr.h"
#include "combat.h"
#include "events.h"
#include "maria.h"
#include "prog.h"
#include "sfx.h"

#define PI 3.14159265f
#define MAX_LIST 512

typedef struct { s16 stage, kind, id; int x, y, z, rot, status, cond; } EnListEntry;

static EnListEntry s_list[MAX_LIST];
static int s_nlist;
static EnListEntry s_debug[4];            // debug.txt "enemy" lines
static int s_ndebug;
static Enemy s_en[ENEMY_MAX];
static const Collision* s_col;
static u32 s_killed[MAX_LIST / 32];       // DAT_01f7d9e0: Enemy_List ids of dead enemies

_Static_assert(sizeof(u32) * (MAX_LIST / 32) == ENEMY_KILLED_BYTES, "killed bitfield size");
void enemy_killed_get(u8* out) { memcpy(out, s_killed, sizeof s_killed); }
void enemy_killed_set(const u8* in) { memcpy(s_killed, in, sizeof s_killed); }
static u8 s_regen[MAX_LIST];               // status + 1 a regenerated enemy comes back with (0: its own)
void enemy_killed_reset(void)
{
	memset(s_killed, 0, sizeof s_killed);
	memset(s_regen, 0, sizeof s_regen);
}
static int s_diff = 2;                    // gActionDifficulty (0 beginner .. 3 hard, 4 unused)
static char s_pack[64];
static float s_px, s_pz;                  // James this frame, for body separation

// Models by chr folder, loaded the first time a room needs them and kept until enemy_exit.
enum { MDL_SCU, MDL_MKN, MDL_NSE, MDL_XOO, MDL_TYU, MDL_RED, MDL_PAP, MDL_ONI, MDL_BOS, MDL_ARM, MDL_EDI, MDL_IKE,
       MDL_TY2, MDL_TY3, MDL_COUNT };
static const char* const MODEL_NAME[MDL_COUNT] = { "scu", "mkn", "nse", "xoo", "tyu", "red", "pap", "oni", "bos", "arm", "edi", "ike",
                                                   "item", "item" };
// File names inside the folder when they differ from it (Eddie's gameplay model is lll_edi).
// The creeper nests are props in chr/item (ty2, ty3) without an animation.
static const char* const MODEL_FILE[MDL_COUNT] = { [MDL_EDI] = "lll_edi", [MDL_TY2] = "ty2", [MDL_TY3] = "ty3" };
static int s_stage = -1;                  // stage of the loaded room (scripted enemies)
static bool load_model(int m);
static void scu_init(Enemy* e, int status);
static void edi_init(Enemy* e, int status);
static void edi_update(Enemy* e, const EnPlayer* p, float dt);
static void ike_init(Enemy* e, int status);
static void ike_update(Enemy* e, const EnPlayer* p, float dt);
static struct {
	Character chr;
	Anim anim;
	bool loaded, tried;
} s_mdl[MDL_COUNT];

// Communication queue (EnCOMMUNICATION, 0x4a7e60 add / 0x4a7f20 find): a chasing enemy calls
// the others to the player's position.
typedef struct { float x, z, radius; int kind, time; } Comm;
static Comm s_comm[8];

// ---------------------------------------------------------------- game random numbers

static u32 s_seed = 0x2f1a;

// 0x448830: LCG, uniform 0..1
static float rnd(void)
{
	s_seed = (s_seed * 0xc1c64e6du + 0x3039) & 0x7fffffff;
	return (float)s_seed * (1.0f / 2147483648.0f);
}

// 0x448910: magnitude in (0, |a|], biased toward small values by b; random sign when a < 0.
static float rnd_signed(float a, float b)
{
	float s = a;
	if (a < 0.0f) {
		s_seed = (s_seed * 0xc1c64e6du + 0x3039) & 0x7fffffff;
		if (s_seed & 0x1000)
			s = -a;
	}
	float r = rnd();
	return ((b + 1.0f) * b / (r + b) - b) * s;
}

// ---------------------------------------------------------------- angles

static float wrap(float a)
{
	while (a > PI) a -= 2.0f * PI;
	while (a < -PI) a += 2.0f * PI;
	return a;
}

// 0x407bc0: move angle *a toward target by at most step.
static void approach_angle(float* a, float target, float step)
{
	float d = wrap(target - *a);
	if (d > step) d = step;
	if (d < -step) d = -step;
	*a = wrap(*a + d);
}

static float dir_to(float fx, float fz, float tx, float tz)
{
	return atan2f(tx - fx, tz - fz);       // 0x448460: atan2(x, z), forward = (sin, cos)
}

// ---------------------------------------------------------------- animation (EnSCUAnime 0x893130)

typedef struct { u16 start, end; u8 loop; s16 speed; } ScuAnim;

// EnSCUAnime[i] gives an anime id 5001.. which indexes scu_anim (_AnimeInfo, live copy at
// 0x6e1554, used by shCharacterEnemySCUAnimeSetP 0x53de20). Frames are in scu.anm. Speed 2048 is
// the normal 30 frames per second.
static const ScuAnim SCU_ANIM[33] = {
	{ 15, 45, 0, 2048 },   //  0 5002 walk (no loop)
	{ 15, 45, 1, 2048 },   //  1 5002 walk
	{ 0, 14, 1, 2048 },    //  2 5001 stand
	{ 77, 100, 0, 2048 },  //  3 5005 attack: spits acid from the torso opening
	{ 77, 100, 0, 2048 },  //  4 5006 attack (same frames)
	{ 101, 120, 0, 2048 }, //  5 5007 flinch
	{ 121, 153, 0, 2048 }, //  6 5008 flinch front
	{ 154, 183, 0, 2048 }, //  7 5009 flinch back
	{ 184, 203, 0, 2048 }, //  8 5010 flinch, hit from the left
	{ 204, 224, 0, 2048 }, //  9 5011 flinch, hit from the right
	{ 225, 243, 0, 2048 }, // 10 5012 flinch, blow from above
	{ 244, 277, 0, 2048 }, // 11 5013 stagger
	{ 278, 314, 0, 2048 }, // 12 5014 stagger
	{ 101, 120, 0, 2048 }, // 13 5007 flinch
	{ 315, 337, 0, 2048 }, // 14 5015 knocked down
	{ 338, 352, 0, 2048 }, // 15 5016 knocked down onto the back (ends face up)
	{ 353, 368, 0, 2048 }, // 16 5017 knocked down forward
	{ 369, 397, 0, 2048 }, // 17 5018 knocked down, from the left
	{ 398, 426, 0, 2048 }, // 18 5019 knocked down, from the right
	{ 427, 441, 0, 1700 }, // 19 5020 knocked down, blow from above
	{ 442, 470, 0, 2048 }, // 20 5021 knocked down
	{ 471, 500, 0, 2048 }, // 21 5022 knocked down
	{ 537, 583, 0, 2048 }, // 22 5024 gets up (lying face down)
	{ 501, 536, 0, 2048 }, // 23 5023 gets up (lying face up)
	{ 675, 689, 1, 2048 }, // 24 5028 writhes on the floor, face down
	{ 660, 674, 1, 2048 }, // 25 5027 writhes on the floor, face up
	{ 367, 367, 0, 0 },    // 26 5033 lies still, face down
	{ 351, 351, 0, 0 },    // 27 5032 lies still, face up
	{ 46, 62, 1, 2048 },   // 28 5003 crawls
	{ 63, 76, 0, 2048 },   // 29 5004 drops from standing to crawling
	{ 714, 715, 0, 2048 }, // 30 5031 event pose
	{ 718, 778, 1, 2048 }, // 31 5035 event loop
	{ 716, 717, 0, 2048 }, // 32 5034 event pose
};
// EnMKNAnime (0x893070) and mkn_anim (0x6b1f9c, ids 5101..5134) for the Mannequin, frames in mkn.anm.
static const ScuAnim MKN_ANIM[32] = {
	{ 0, 23, 0, 2048 },    //  0 5101 stand (once)
	{ 24, 54, 1, 2048 },   //  1 5102 walk
	{ 0, 23, 1, 2048 },    //  2 5101 stand
	{ 55, 89, 0, 2048 },   //  3 5103 attack: swings an arm stump
	{ 90, 124, 0, 2048 },  //  4 5104 attack: kicks
	{ 125, 142, 0, 2048 }, //  5 5105 flinch
	{ 143, 167, 0, 2048 }, //  6 5106
	{ 168, 192, 0, 2048 }, //  7 5107
	{ 193, 212, 0, 2048 }, //  8 5108 flinch, from the left
	{ 213, 232, 0, 2048 }, //  9 5109 flinch, from the right
	{ 233, 259, 0, 2048 }, // 10 5110 flinch, from above
	{ 260, 291, 0, 2048 }, // 11 5111
	{ 292, 323, 0, 2048 }, // 12 5112
	{ 125, 142, 0, 2048 }, // 13 5105
	{ 324, 339, 0, 1500 }, // 14 5113 knocked down
	{ 340, 356, 0, 1500 }, // 15 5114 knocked down onto the back
	{ 357, 372, 0, 1500 }, // 16 5115
	{ 373, 402, 0, 1800 }, // 17 5116 knocked down, from the left
	{ 403, 432, 0, 1800 }, // 18 5117 knocked down, from the right
	{ 433, 442, 0, 1600 }, // 19 5118 knocked down, from above
	{ 443, 454, 0, 2048 }, // 20 5119
	{ 455, 470, 0, 2048 }, // 21 5120
	{ 515, 564, 0, 2048 }, // 22 5122 gets up (face down)
	{ 471, 514, 0, 2048 }, // 23 5121 gets up (face up)
	{ 662, 677, 1, 2048 }, // 24 5126 writhes, face down
	{ 646, 661, 1, 2048 }, // 25 5125 writhes, face up
	{ 371, 371, 0, 0 },    // 26 5134 lies still, face down
	{ 355, 355, 0, 0 },    // 27 5133 lies still, face up
	{ 694, 708, 0, 2048 }, // 28 5129 drops from above
	{ 709, 715, 0, 2048 }, // 29 5130 lands
	{ 716, 717, 0, 2048 }, // 30 5131 event pose
	{ 718, 719, 0, 2048 }, // 31 5132 event pose
};

// EnNSEAnime (0x892da8) and the nurse's _AnimeInfo (0x6ea24c, ids 5501..5526; anime set 0x541770),
// frames in nse.anm.
static const ScuAnim NSE_ANIM[28] = {
	{ 0, 16, 0, 2048 },    //  0 5501 stand (once)
	{ 17, 50, 1, 2048 },   //  1 5502 walk, dragging a leg
	{ 0, 16, 1, 2048 },    //  2 5501 stand
	{ 51, 82, 0, 2048 },   //  3 5503 attack: swings the pipe (attack 46)
	{ 83, 116, 0, 2048 },  //  4 5504 attack: lunging blow (attack 47)
	{ 117, 135, 0, 2048 }, //  5 5505 flinch
	{ 136, 168, 0, 2048 }, //  6 5506 flinch front
	{ 169, 204, 0, 2048 }, //  7 5507 flinch back
	{ 205, 228, 0, 2048 }, //  8 5508 flinch, from the left
	{ 229, 252, 0, 2048 }, //  9 5509 flinch, from the right
	{ 253, 277, 0, 2048 }, // 10 5510 flinch, from above
	{ 278, 310, 0, 2048 }, // 11 5511 stagger back
	{ 311, 346, 0, 2048 }, // 12 5512 stagger forward
	{ 117, 135, 0, 2048 }, // 13 5505
	{ 347, 367, 0, 2048 }, // 14 5513 knocked down
	{ 368, 396, 0, 2048 }, // 15 5514 knocked down onto the back
	{ 397, 419, 0, 2048 }, // 16 5515
	{ 420, 450, 0, 2048 }, // 17 5516 knocked down, from the left
	{ 451, 481, 0, 2048 }, // 18 5517 knocked down, from the right
	{ 482, 498, 0, 2048 }, // 19 5518 knocked down, from above
	{ 499, 528, 0, 2048 }, // 20 5519
	{ 529, 557, 0, 2048 }, // 21 5520
	{ 593, 631, 0, 2048 }, // 22 5522 gets up (face down)
	{ 558, 592, 0, 2048 }, // 23 5521 gets up (face up)
	{ 643, 657, 1, 2048 }, // 24 5524 writhes, face down
	{ 632, 642, 1, 2048 }, // 25 5523 writhes, face up
	{ 418, 418, 0, 0 },    // 26 5526 lies still, face down
	{ 395, 395, 0, 0 },    // 27 5525 lies still, face up
};

// Creeper (EnTYUAnime 0x6eaea4): 5201 lies still (one frame), 5202 scuttles.
static const ScuAnim TYU_ANIM[2] = {
	{ 0, 0, 0, 0 },
	{ 0, 7, 1, 2048 },
};

// Pyramid Head (EnREDAnime 0x892fa0, {id, loop} per anime; the ids index red_anim at 0x6dfc70).
static const ScuAnim RED_ANIM[11] = {
	{ 0, 0, 1, 2048 },       // 0 5301 stands
	{ 0, 29, 1, 1280 },      // 1 5302 walks
	{ 306, 323, 1, 2048 },   // 2 5310 idle loop
	{ 30, 96, 0, 2048 },     // 3 5303 swing (attack 50)
	{ 97, 216, 0, 2048 },    // 4 5304 the long great knife swing (attack 51)
	{ 217, 244, 0, 2048 },   // 5 5305 short swing to the side (attack 52)
	{ 245, 255, 0, 2048 },   // 6 5306 shove (attack 53)
	{ 256, 270, 0, 1024 },   // 7 5307 lifts James
	{ 271, 294, 1, 2048 },   // 8 5308 holds him (attack 54)
	{ 295, 305, 0, 2048 },   // 9 5309 lets go
	{ 324, 372, 1, 2048 },   // 10 5311 idle behind the bars
};

static const ScuAnim PAP_ANIM[33], ONI_ANIM[16], BOS_ANIM[10], ARM_ANIM[14], EDI_ANIM[22], IKE_ANIM[21];

static const ScuAnim* anim_tab(const Enemy* e)
{
	switch (e->kind) {
	case EN_KIND_PAP: return PAP_ANIM;
	case EN_KIND_EDI: return EDI_ANIM;
	case EN_KIND_IKE: return IKE_ANIM;
	case EN_KIND_ONI: return ONI_ANIM;
	case EN_KIND_BOS: return BOS_ANIM;
	case EN_KIND_ARM: return ARM_ANIM;
	case EN_KIND_RED: return RED_ANIM;
	case EN_KIND_MKN: return MKN_ANIM;
	case EN_KIND_NSE: return NSE_ANIM;
	case EN_KIND_TYU: return TYU_ANIM;
	default: return SCU_ANIM;
	}
}

enum {
	A_WALK = 1, A_STAND = 2, A_ATTACK = 3, A_DOWN0 = 14, A_GETUP = 22, A_WRITHE = 24, A_LIE = 26,
	A_CRAWL = 28, A_TO_CRAWL = 29
};

// Root motion of the walk (planted foot, skeleton pair 20/21 at 0x892770; measured from scu.anm):
// units per animation frame.
#define SCU_WALK_STRIDE 13.4f
#define MKN_WALK_STRIDE 19.2f      // skeleton pair 15/16 (0x892778), measured from mkn.anm

// enAnimeSet 0x4a9920 (or a resume when the anime is already playing, 0x4a7b80).
static void anim_set(Enemy* e, int a)
{
	if (e->anim != a) {
		e->anim = a;
		e->frame = anim_tab(e)[a].start;
	}
	e->rate = 1.0f;
	e->loops = 0;
	e->loop_limit = 0;
	e->anim_end = false;
}

// 0x4c42c0: the stand loop plays a random number of times before it counts as ended.
static void anim_random_loops(Enemy* e)
{
	float n = 6.0f - s_diff * 0.2f + rnd_signed(-2.0f, 0.5f);
	e->loop_limit = (int)(n * 0.5f + 0.5f);
	if (e->loop_limit < 1) e->loop_limit = 1;
	const ScuAnim* t = &anim_tab(e)[e->anim];
	e->frame = t->start + rnd() * (t->end - t->start);
}

// 0x4a77a0 (enAnimeExec) reduced to frame stepping; returns the animation frames advanced.
static float anim_step(Enemy* e, float dt)
{
	const ScuAnim* t = &anim_tab(e)[e->anim];
	float len = (float)(t->end - t->start + 1);
	float adv = dt * 30.0f * (t->speed / 2048.0f) * e->rate;
	if (e->anim_end || t->speed == 0) {
		if (t->speed == 0)
			e->anim_end = true;
		return 0.0f;
	}
	e->frame += adv;
	if (t->loop) {
		while (e->frame >= t->end + 1.0f) { e->frame -= len; e->loops++; }
		while (e->frame < t->start) { e->frame += len; e->loops++; }
		if (e->loop_limit && e->loops >= e->loop_limit)
			e->anim_end = true;
	} else if (e->frame >= t->end) {
		e->frame = t->end;
		e->anim_end = true;
	} else if (e->frame < t->start) {
		e->frame = t->start;
		e->anim_end = true;
	}
	return adv;
}

// ---------------------------------------------------------------- en_common helpers

static float hp_rate(const Enemy* e) { return e->hp_max > 0.0f ? e->hp / e->hp_max * 100.0f : 0.0f; }

// enSetSize 0x4a5ec0 / enSetNewSize 0x4a5f10 (eased by enMoveExec)
static void set_size(Enemy* e, float size, float tall, bool now)
{
	e->new_size = size;
	e->new_tall = tall;
	if (now) {
		e->size = size;
		e->tall = tall;
	}
}
static void size_upright(Enemy* e, bool now) { set_size(e, 150.0f, 800.0f, now); }
// Lying: 225 wide outside the special areas (0x4c5740), 200 tall.
static void size_lying(Enemy* e, bool now) { set_size(e, 225.0f, 200.0f, now); }

// enSetHP 0x4a5b50: kind 1 gets a random spread on both values.
static void set_hp(Enemy* e, float hp, float endurance)
{
	float r = rnd_signed(-0.5f, 1.0f);
	if (r < 0.0f && s_diff > 1 && (rnd() < 0.5f))
		r *= -2.0f;
	hp *= r + 1.0f;
	r = rnd_signed(-0.3f, 1.0f);
	if (r < 0.0f && s_diff > 1 && (rnd() < 0.5f))
		r *= -2.0f;
	endurance *= r + 1.0f;
	e->hp = e->hp_max = hp;
	e->endurance = e->endurance_max = endurance;
	e->damage = 0.0f;
}

// enReduceHP 0x4a5cd0: applies the pending damage, returns the endurance left.
static float reduce_hp(Enemy* e)
{
	e->hp -= e->damage;
	if (e->hp < 0.0f) e->hp = 0.0f;
	e->endurance -= e->damage;
	if (e->endurance < 0.0f) e->endurance = 0.0f;
	e->damage = 0.0f;
	return e->endurance;
}

// enAddHP 0x4a5d40
static void add_hp(Enemy* e, float n)
{
	e->hp += n;
	if (e->hp > e->hp_max) e->hp = e->hp_max;
	e->endurance = e->endurance_max < e->hp ? e->endurance_max : e->hp;
}

static bool is_dead(const Enemy* e) { return e->hp <= 0.0f; }     // 0x4a5e70

// enCheckDamage 0x4a8d80: a hit is pending. Hits are ignored while getting up (0x148 bit 0x40).
static bool invuln_flag(const Enemy* e) { return (e->flags & 0x10000) != 0; }
static bool check_damage(Enemy* e)
{
	if (e->damage == 0.0f)
		return false;
	if (invuln_flag(e)) {
		e->damage = 0.0f;
		e->shock = 0.0f;
		return false;
	}
	return true;
}
static void reset_damage(Enemy* e) { e->damage = 0.0f; e->shock = 0.0f; }   // 0x4a5e40

// 0x4a8e70: the hit pushes the enemy back (decays by half every 1/60 s in enMoveExec).
static void set_hitback(Enemy* e)
{
	e->hb = e->shock * 2.0f;
	e->shock = 0.0f;
}

// enSetTimer 0x4a6ba0 / enReduceTimer 0x4a6be0 (60 Hz ticks; bit 0x2000 marks each new second)
static void set_timer(Enemy* e, int t)
{
	e->timer = t - 1 < 0 ? 0 : t - 1;
	e->flags &= ~0x2000u;
}
static int reduce_timer(Enemy* e, int ticks)
{
	int old = e->timer;
	e->timer -= ticks;
	if (e->timer < 0) e->timer = 0;
	if (e->timer / 60 < old / 60) e->flags |= 0x2000;
	else e->flags &= ~0x2000u;
	return e->timer;
}

// Endurance recovery 0x4c3e30 (en_scu): {0,10,30,60,100} per second by difficulty.
static void recover_endurance(Enemy* e, float dt)
{
	static const float rate[5] = { 0, 10, 30, 60, 100 };
	float v = e->endurance + dt * rate[s_diff];
	if (v > e->endurance_max) v = e->endurance_max;
	if (v > e->hp) v = e->hp;
	e->endurance = v;
}

// ---------------------------------------------------------------- en_scu parameter tables

// 0x4c3ea0: walk anime speed. Light condition index DAT_01f7fa24: 0 lit place, 1/2 dark with the
// flashlight on, 3 dark with it off. The apartments are dark places.
static int light_index(const EnPlayer* p) { return p->light ? 1 : 3; }

static float walk_rate(const Enemy* e, const EnPlayer* p)
{
	static const float t[5] = { 0.7f, 0.8f, 1.0f, 1.2f, 1.5f };
	float r = t[s_diff] * (hp_rate(e) * 0.004f + 0.6f);
	int li = light_index(p);
	if (li <= 1 && s_diff >= 2) r *= 1.2f;
	else if (li == 3 && s_diff <= 2) r *= 0.8f;
	return r;
}

// 0x4c3f30: crawl anime speed; the crawl velocity is that rate * 1500 units/s.
static float crawl_rate(const Enemy* e)
{
	static const float t[5] = { 1.5f, 1.7f, 2.0f, 2.0f, 2.0f };
	float r = t[s_diff] * (hp_rate(e) * 0.0025f + 0.75f);
	if (e->type & 0x20) r *= 0.4f;
	return r;
}

// 0x4c3fb0: reach of the acid (EnSCU sight table attack range 0, rewritten before each check).
static float attack_range(const EnPlayer* p)
{
	static const float t[5] = { 750, 900, 1200, 1500, 1500 };
	float r = t[s_diff];
	int li = light_index(p);
	if (li < 2) r *= 1.5f;
	else if (li == 3) r *= 0.8f;
	return r;
}

// 0x4c4010: chance to spit when in range.
static float attack_probability(void)
{
	static const float t[5] = { 0.5f, 0.5f, 0.7f, 0.8f, 1.0f };
	return t[s_diff];
}

static float repeat_probability(void)     // 0x4c40a0
{
	static const float t[5] = { 0.0f, 0.0f, 0.2f, 0.5f, 0.99f };
	return t[s_diff];
}

static float attack_anim_rate(const Enemy* e)   // 0x4c40e0
{
	static const float t[5] = { 0.5f, 0.5f, 0.6f, 0.8f, 1.0f };
	return t[s_diff] * (hp_rate(e) * 0.0025f + 0.75f);
}

static float turn_rate(void)               // 0x4c4140: radians per 1/60 s
{
	static const float t[5] = { 0.5f, 0.7f, 1.0f, 1.2f, 1.5f };
	return t[s_diff] * 0.05235988f;
}

static float aim_rate(void)                // 0x4c4190 (indoor row): turning while spitting
{
	static const float t[5] = { 0.0f, 0.0f, 0.1f, 0.5f, 1.0f };
	return t[s_diff] * 0.05235988f;
}

// 0x4c4220 (Lying Figure) / 0x4c13a0 (Mannequin): ticks lying before getting up.
static int down_time(const Enemy* e)
{
	static const s16 scu[5] = { 900, 400, 300, 240, 180 };
	static const s16 mkn[5] = { 900, 500, 400, 300, 240 };
	static const s16 nse[5] = { 600, 480, 360, 300, 240 };      // 0x4b9fa0
	return e->kind == EN_KIND_MKN ? mkn[s_diff] : e->kind == EN_KIND_NSE ? nse[s_diff] : scu[s_diff];
}

static float stay_down_probability(void) { return 0.8f; }   // 0x4c4280

// EnSCU sight table at 0x8931b8: sight distance by light index, view half angle, hearing range by
// difficulty, attack range / angle pairs (range 0 is replaced by attack_range()).
static const float SIGHT_DIST[4] = { 5000, 4000, 4000, 750 };
static const float VIEW_ANGLE = 1.2217305f;
static const float HEAR_DIST[5] = { 0, 750, 1000, 1500, 1500 };
static const float ATTACK_ANGLE = 0.0872665f;
// EnMKN sight table at 0x8930f0 (same layout).
static const float MKN_SIGHT_DIST[4] = { 5000, 3000, 3000, 900 };
static const float MKN_VIEW_ANGLE = 1.0471976f;
static const float MKN_ATTACK_RANGE = 1100.0f;
static const float MKN_ATTACK_ANGLE = 0.2617994f;

// ---------------------------------------------------------------- senses

static bool line_of_sight(const Enemy* e, const EnPlayer* p)
{
	if (!s_col)
		return true;
	float cx = p->x, cz = p->z;
	col_clamp_ray(s_col, e->x, e->z, &cx, &cz, e->y - 500.0f);
	float dx = cx - p->x, dz = cz - p->z;
	return dx * dx + dz * dz < 40.0f * 40.0f;
}

// Flashlight on the enemy (0x542cd0 tests the light cone against the enemy position).
static bool lit_by_player(const Enemy* e, const EnPlayer* p)
{
	if (!p->light || e->p_dist > 6000.0f)
		return false;
	float a = wrap(dir_to(p->x, p->z, e->x, e->z) - p->yaw);
	return fabsf(a) < 0.42f;
}

// 0x4a9aa0 with the EnSCU table: 0 nothing, 1 sees James, 2 feels him near, 3 James in spitting
// range and straight ahead. The alert value (+0x98) builds up while lit and fades otherwise.
static int sight_check(Enemy* e, const EnPlayer* p, float dt)
{
	if (!p->alive)
		return 0;
	bool los = line_of_sight(e, p);
	float d = e->p_dist;
	float ang = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	int res = 1;
	bool lit = lit_by_player(e, p);
	bool mkn = e->kind == EN_KIND_MKN, nse = e->kind == EN_KIND_NSE;
	// EnNSE sight table at 0x892e18: sight 4000 / 4000 / 3000 / 1000, 80 degrees, hearing
	// {500, 750, 1200, 1500, 1500}, attack 1500 within 5 degrees or 900 within 30
	static const float NSE_SEE[4] = { 4000, 4000, 3000, 1000 }, NSE_HEAR[5] = { 500, 750, 1200, 1500, 1500 };
	float see = nse ? NSE_SEE[light_index(p)] : mkn ? MKN_SIGHT_DIST[light_index(p)] : SIGHT_DIST[light_index(p)];
	float view = nse ? 1.3962634f : mkn ? MKN_VIEW_ANGLE : VIEW_ANGLE;
	float hear = nse ? NSE_HEAR[s_diff] : HEAR_DIST[s_diff];
	if (!los || d >= see || ang >= view) {
		if (lit) {
			e->alert += dt;
			if (e->alert > 3.0f) e->alert = 3.0f;
		} else {
			e->alert -= dt;
			if (e->alert < 0.0f) e->alert = 0.0f;
			if (d > hear)
				return 0;
			res = 2;
		}
	} else if (!lit) {
		if (e->alert != 0.0f) {
			e->alert -= dt;
			if (e->alert < 0.0f) e->alert = 0.0f;
			return 0;
		}
	} else {
		e->alert += dt;
		if (e->alert > 3.0f) e->alert = 3.0f;
	}
	if (nse) {
		if (los && d < 1500.0f && ang < 0.0872665f) return 3;
		if (los && d < 900.0f && ang < 0.5235988f) return 4;
		return res;
	}
	if (los && d < (mkn ? MKN_ATTACK_RANGE : attack_range(p)) && ang < (mkn ? MKN_ATTACK_ANGLE : ATTACK_ANGLE))
		return 3;
	return res;
}

static void comm_add(float x, float z, float radius, int ticks, int kind)
{
	for (int i = 0; i < 8; i++)
		if (s_comm[i].time <= 0) {
			s_comm[i] = (Comm){ x, z, radius, kind, ticks };
			return;
		}
}

static bool comm_heard_kind(const Enemy* e, int kind)
{
	for (int i = 0; i < 8; i++) {
		const Comm* c = &s_comm[i];
		if (c->time > 0 && c->kind == kind) {
			float dx = e->x - c->x, dz = e->z - c->z;
			if (dx * dx + dz * dz < c->radius * c->radius)
				return true;
		}
	}
	return false;
}

static bool comm_heard(const Enemy* e) { return comm_heard_kind(e, 1); }

// The blow can catch Maria too when she stands in it (the PC tests the attack against every
// character in reach): once per attack.
static void blow_maria(Enemy* e, float reach, float arc, float ap)
{
	if (!e->maria_hit && maria_hit(e->x, e->z, e->yaw, reach, arc, ap))
		e->maria_hit = true;
}

// A blow that reached the target: James takes it (Maria's share goes through blow_maria).
static void hit_james(const EnPlayer* p, int id, float ap, const Enemy* e)
{
	if (!p->maria)
		combat_damage_james(id, ap, e->x, e->z);
}

static void play_at(const Enemy* e, int id)
{
	float pos[3] = { e->x, e->y, e->z };
	sfx_play_at(id, 1.0f, pos);
}

// enSetPath 0x4aa960: heads straight for the target when the way is clear for 500 units; when a
// wall is in the way it tries headings 15, 30 .. 75 degrees to either side (0x6b34ec) and takes the
// clear one closer to where it was going, or turns about when none is. A detour is kept for a short
// while (EnPATH +0x32) so the figure does not dither at a corner. Returns true while detouring.
static bool ray_clear(const Enemy* e, float a, float len)
{
	float ax = e->x + sinf(a) * len, az = e->z + cosf(a) * len;
	float cx = ax, cz = az;
	col_clamp_ray(s_col, e->x, e->z, &cx, &cz, e->y - 300.0f);
	return (cx - ax) * (cx - ax) + (cz - az) * (cz - az) <= 30.0f * 30.0f;
}

static bool steer(Enemy* e, float tx, float tz)
{
	float direct = dir_to(e->x, e->z, tx, tz);
	e->mark_angle = direct;
	if (!s_col)
		return false;
	float len = fminf(hypotf(tx - e->x, tz - e->z), 500.0f);
	if (len < 60.0f || ray_clear(e, direct, len)) {
		e->detour_t = 0.0f;
		return false;
	}
	if (e->detour_t > 0.0f && ray_clear(e, e->detour, 400.0f)) {
		e->detour_t -= 1.0f / 30.0f;
		e->mark_angle = e->detour;
		return true;
	}
	float pick = wrap(direct + PI);
	for (int k = 1; k <= 5; k++) {
		float a = wrap(direct + k * 0.2617994f), b = wrap(direct - k * 0.2617994f);
		bool ca = ray_clear(e, a, 400.0f), cb = ray_clear(e, b, 400.0f);
		if (ca && cb) {
			pick = fabsf(wrap(a - e->path_angle)) <= fabsf(wrap(b - e->path_angle)) ? a : b;
			break;
		}
		if (ca || cb) {
			pick = ca ? a : b;
			break;
		}
	}
	e->detour = pick;
	e->detour_t = 0.5f + rnd() * 0.5f;
	e->mark_angle = pick;
	return true;
}

// ---------------------------------------------------------------- states

// States shared by both kinds are written with the SCU_ names; the Mannequin's table (0x4c3790)
// has them at other indices.
static void go(Enemy* e, int mlv)
{
	static const s8 to_mkn[12] = { MKN_WANDER, MKN_PRECAUTION, MKN_CHASE, MKN_CHASE, MKN_WANDER, MKN_STAY,
	                               MKN_DOWN, MKN_ATTACK, MKN_DAMAGE, MKN_PRECAUTION, MKN_DOWN, MKN_DYING };
	static const s8 to_nse[12] = { NSE_WANDER, NSE_PRECAUTION, NSE_CHASE, NSE_CHASE, NSE_WANDER, NSE_WANDER,
	                               NSE_DOWN, NSE_ATTACK, NSE_DAMAGE, NSE_CONFUSE, NSE_DOWN, NSE_DYING };
	e->mlv = e->kind == EN_KIND_MKN ? to_mkn[mlv] : e->kind == EN_KIND_NSE ? to_nse[mlv] : mlv;
	e->slv = 0;
}
static void go_raw(Enemy* e, int mlv) { e->mlv = mlv; e->slv = 0; }

// 0x4c4620 / 0x4c4690: switch to anime a (the stand loop gets a random length).
static void pose(Enemy* e, int a)
{
	if (a < 0)
		return;
	anim_set(e, a);
	if (a == A_STAND)
		anim_random_loops(e);
}

static void stand_up(Enemy* e)
{
	pose(e, A_GETUP + e->lie);
	e->flags |= 0x10000;            // 0x4a6440: no damage while getting up
	size_upright(e, false);
}

static void start_dying(Enemy* e)
{
	go(e, SCU_DYING);
}


// Ambush (0x4c3a70 / 0x4c5370, hard only, type bit 4 which the buildings with glb_crd 9..13 give):
// when James stands in one of the rectangles of the glb's ambush_apart list (0x8935f4, counts at
// 0x89360c) and its spot is within 1250, the figure walks there (backwards if that is shorter),
// waits beside the doorway facing between James and the spot's angle, and spits once James is in
// reach. Rows: x0, z0, x1, z1, spot x, spot z, angle.
typedef struct { float x0, z0, x1, z1, x, z, angle; } Ambush;
static const Ambush AMB_9[] = {
	{ 56200, 10800, 64000, 12000, 60400, 12600, 3.142f }, { 56200, 10800, 60000, 12000, 56800, 10000, 0.0f },
	{ 99600, -20000, 100800, -15000, 98900, -17700, 1.571f }, { -106000, 10800, -100000, 12000, -103000, 10300, 0.0f },
	{ -102500, 10800, -96000, 12000, -99400, 12800, 3.142f }, { -84000, 10800, -80200, 12000, -80600, 13000, 3.142f },
	{ 99600, -20000, 100800, -15000, 99000, -17600, 1.571f }, { 99600, -61500, 100800, -56000, 99000, -58800, 1.571f },
	{ -50600, -65000, -49400, -57000, -48700, -60800, -1.571f },
};
static const Ambush AMB_10[] = {
	{ -18000, -24000, -16800, -16800, -18500, -22250, 1.571f }, { -25500, -24000, -16800, -22800, -18500, -22250, 3.142f },
	{ 54000, 59600, 63000, 60800, 58500, 61300, 3.142f }, { -66000, 99600, -56000, 100800, -61600, 101300, 3.142f },
	{ -64000, -140800, -56000, -139600, -60000, -139100, 3.142f }, { 18500, -181200, 23200, -178800, 22300, -181750, 0.0f },
	{ 56000, -180800, 65000, -179600, 60000, -179200, 3.142f }, { -24000, -224000, -16800, -222800, -18800, -222200, 3.142f },
	{ -18000, -224000, -16800, -216800, -18500, -222000, 1.571f },
};
static const Ambush AMB_11[] = {
	{ 58400, 107200, 108400, 65200, 62150, 106500, 0.0f }, { 61600, 99200, 62800, 108400, 63500, 103800, -1.571f },
	{ 139600, 97200, 140800, 103600, 141300, 98000, -1.571f }, { 56000, 60400, 63600, 61600, 62850, 59900, 0.0f },
	{ 142000, 54200, 143200, 66200, 141500, 60400, 1.571f }, { 138000, 65000, 143200, 66200, 140200, 66750, 3.142f },
	{ 59200, 17200, 60400, 28000, 61000, 21800, -1.571f }, { 137600, 16800, 142400, 19200, 139200, 18600, -1.571f },
	{ 137600, 18150, 144400, 20400, 138300, 19600, 3.142f },
};
static const Ambush AMB_12[] = {
	{ -18800, 98800, -16800, 107600, -16250, 102400, -1.571f }, { -18800, 98800, -16800, 107600, -16250, 99600, -1.571f },
	{ -60400, -64800, -59600, -59600, -60700, -64400, 1.571f }, { -60400, -64800, -59600, -59600, -59300, -63200, -1.571f },
};
static const Ambush AMB_13[] = {
	{ -22400, 60400, -12400, 61200, -12800, 60000, 0.0f }, { -17000, 58800, -22400, 61200, -22900, 59300, 1.571f },
	{ -102350, 20650, -98750, 23850, -101900, 22300, 3.142f }, { -102350, 17450, -98750, 23050, -101150, 21400, -1.571f },
	{ -63200, -64800, -59600, -59600, -59150, -63150, -1.571f },
};
static const struct { const Ambush* rows; int n; } AMBUSH[6] = {
	{ NULL, 0 }, { AMB_9, 9 }, { AMB_10, 9 }, { AMB_11, 9 }, { AMB_12, 4 }, { AMB_13, 5 },
};

static int glb_set(void)                   // 0x4a5860
{
	int g = ev_glb_crd();
	return g >= 9 && g <= 13 ? g - 8 : 0;
}

static bool ambush_find(Enemy* e, const EnPlayer* p)
{
	int set = glb_set();
	if (!(e->type & 4) || s_diff <= 2 || !set || p->maria)
		return false;
	for (int i = 0; i < AMBUSH[set].n; i++) {
		const Ambush* a = &AMBUSH[set].rows[i];
		if (a->x0 <= p->x && a->z0 <= p->z && p->x <= a->x1 && p->z <= a->z1 &&
		    hypotf(a->x - e->x, a->z - e->z) <= 1250.0f) {
			e->amb = (s8)i;
			return true;
		}
	}
	return false;
}

static void scu_ambush(Enemy* e, const EnPlayer* p, float dt)
{
	int set = glb_set();
	if (e->amb < 0 || !set) { go(e, SCU_WANDER); return; }
	const Ambush* a = &AMBUSH[set].rows[(int)e->amb];
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	switch (e->slv) {
	case 0:
		pose(e, A_WALK);
		e->path_angle = e->yaw;
		e->amb_n = 0;
		e->slv = 1;
		break;
	case 1: case 2: {
		steer(e, a->x, a->z);
		if (e->type & 2) {
			// backs into the spot when it lies behind (more than 90 degrees off its facing)
			float off = fabsf(wrap(e->mark_angle - e->yaw));
			if (e->slv == 1 && off > PI / 2.0f) { e->path_angle = wrap(e->yaw + PI); e->slv = 2; break; }
			if (e->slv == 2 && off < PI / 2.0f) { e->path_angle = e->yaw; e->slv = 1; break; }
		}
		approach_angle(&e->path_angle, e->mark_angle, turn_rate() * dt * 60.0f);
		e->yaw = e->slv == 1 ? e->path_angle : wrap(e->path_angle + PI);
		e->rate = walk_rate(e, p) * (e->slv == 2 ? -1.0f : 1.0f);
		float d = hypotf(a->x - e->x, a->z - e->z);
		if (d > 1250.0f) { go(e, SCU_WANDER); return; }
		if (d < 50.0f) {
			pose(e, A_STAND);
			e->x = a->x, e->z = a->z;
			e->path_angle = e->yaw;
			e->slv = 3;
		}
		break;
	}
	case 3: {
		float want = wrap(dir_to(e->x, e->z, p->x, p->z) - a->angle);
		approach_angle(&e->path_angle, wrap(want * 0.5f + a->angle), turn_rate() * dt * 60.0f);
		e->yaw = e->path_angle;
		if (p->x < a->x0 || p->z < a->z0 || p->x > a->x1 || p->z > a->z1) { go(e, SCU_WANDER); return; }
		if (e->anim_end) {
			pose(e, A_STAND);
			add_hp(e, 10.0f);
			if (e->amb_n < 255) e->amb_n++;
		}
		break;
	}
	}
	recover_endurance(e, dt);
	int seen = sight_check(e, p, dt);
	if (seen == 3) {
		if (e->amb_n) go(e, SCU_ATTACK);
	} else if (seen != 0 && e->slv < 3) {
		go(e, SCU_CHASE);
	}
}

// 0x4c4b30 enSCUCtrlWander
static void scu_wander(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (ambush_find(e, p)) { go(e, SCU_AMBUSH); return; }
	if (comm_heard(e)) { go(e, SCU_PRECAUTION); return; }
	int seen = sight_check(e, p, dt);
	switch (e->slv) {
	case 0:
		e->stx = e->x, e->stz = e->z;
		pose(e, A_WALK);
		e->rate = walk_rate(e, p);
		e->path_angle = e->yaw;
		e->tx = e->x + sinf(e->yaw) * 1000.0f;
		e->tz = e->z + cosf(e->yaw) * 1000.0f;
		e->slv = 1;
		// fall through
	case 1: {
		float ds = hypotf(e->stx - e->x, e->stz - e->z);
		if (!(e->type & 1) || ds <= 3500.0f) {
			float dt_ = hypotf(e->tx - e->x, e->tz - e->z);
			if (dt_ < 400.0f || rnd() < 0.002f) {
				float a = (rnd() - 0.5f) * (PI / 2.0f) + e->yaw;
				e->tx = e->x + sinf(a) * 1000.0f;
				e->tz = e->z + cosf(a) * 1000.0f;
				if ((e->type & 1) && hypotf(e->tx - e->stx, e->tz - e->stz) > 3500.0f) {
					a = e->yaw + ((rnd() < 0.5f) ? PI / 2.0f : -PI / 2.0f);
					e->tx = e->x + sinf(a) * 1000.0f;
					e->tz = e->z + cosf(a) * 1000.0f;
				}
			} else if (rnd() < 1e-4f) {
				pose(e, A_STAND);
				e->slv = 3;
			}
		} else {
			e->tx = e->stx, e->tz = e->stz;
			e->slv = 2;
			set_timer(e, 300);
		}
		break;
	}
	case 2:
		if (hypotf(e->stx - e->x, e->stz - e->z) >= 3500.0f) {
			if (reduce_timer(e, 1) > 0)
				break;
			e->tx = e->x, e->tz = e->z;
		}
		e->slv = 1;
		break;
	case 3:
		if (!e->anim_end)
			break;
		pose(e, A_WALK);
		e->rate = walk_rate(e, p);
		e->slv = 1;
		break;
	}
	if (e->slv != 3) {
		if (steer(e, e->tx, e->tz)) {
			e->tx = e->x + sinf(e->mark_angle) * 1000.0f;
			e->tz = e->z + cosf(e->mark_angle) * 1000.0f;
		}
		approach_angle(&e->path_angle, e->mark_angle, turn_rate() * dt * 60.0f);
		e->yaw = e->path_angle;
	}
	recover_endurance(e, dt);
	if (seen == 1) go(e, SCU_CHASE);
	else if (seen == 2) go(e, SCU_PRECAUTION);
	else if (seen == 3 && e->loops > 0) go(e, SCU_ATTACK);
}

// 0x4c4f50 enSCUCtrlPrecaution: stands and turns toward James.
static void scu_precaution(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		pose(e, A_STAND);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	int seen = sight_check(e, p, dt);
	if (e->anim_end) {
		if (seen == 0 && !comm_heard(e)) { go(e, SCU_WANDER); return; }
		if (seen == 1 && e->p_dist > 500.0f) { go(e, SCU_CHASE); return; }
		if (seen == 3) { go(e, SCU_ATTACK); return; }
		pose(e, A_STAND);
	}
	e->mark_angle = dir_to(e->x, e->z, p->x, p->z);
	approach_angle(&e->path_angle, e->mark_angle, turn_rate() * dt * 60.0f);
	e->yaw = e->path_angle;
	recover_endurance(e, dt);
}

// 0x4c5070 enSCUCtrlChase: walks at James, slowing down while it turns (0x4a66c0).
static void scu_chase(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		pose(e, A_WALK);
		e->path_angle = e->yaw;
		if (s_diff > 0)
			comm_add(p->x, p->z, 4000.0f, 120, 1);
		e->slv = 1;
	}
	steer(e, p->x, p->z);
	approach_angle(&e->path_angle, e->mark_angle, turn_rate() * dt * 60.0f);
	e->yaw = e->path_angle;
	float off = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	e->rate = walk_rate(e, p) * (((PI - off) / PI) + 1.0f) * 0.5f;
	recover_endurance(e, dt);
	int seen = sight_check(e, p, dt);
	if (seen == 3) {
		if (e->loops > 0) go(e, SCU_ATTACK);
	} else if (seen == 0) {
		go(e, comm_heard(e) ? SCU_PRECAUTION : SCU_WANDER);
	} else if ((e->type & 2) && e->p_dist < 750.0f && e->loops > 0 && s_diff > 0) {
		go(e, SCU_BACKWARD);
	}
}

// 0x4c5230 enSCUCtrlBackward: backs off with the walk played in reverse.
static void scu_backward(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		pose(e, A_WALK);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	e->mark_angle = dir_to(e->x, e->z, p->x, p->z);
	approach_angle(&e->path_angle, e->mark_angle, turn_rate() * dt * 60.0f);
	e->yaw = e->path_angle;
	e->rate = -walk_rate(e, p);
	recover_endurance(e, dt);
	int seen = sight_check(e, p, dt);
	if (seen == 3) {
		if (e->loops > 1 || s_diff > 2) go(e, SCU_ATTACK);
	} else if (seen == 0) {
		if (e->loops > 1) go(e, comm_heard(e) ? SCU_PRECAUTION : SCU_WANDER);
	} else if (e->loops > 1 && e->p_dist > 750.0f) {
		go(e, SCU_PRECAUTION);
	}
}

// 0x4c63f0 enSCUCtrlAttack: the acid spray (attack 42, frames 9..22 of the anime).
static void scu_attack(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { e->attacking = false; go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		if (rnd() < attack_probability()) {
			anim_set(e, A_ATTACK);
			e->rate = attack_anim_rate(e);
			play_at(e, 12000);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			e->slv = 1;
		} else {
			pose(e, A_STAND);
			e->slv = 2;
		}
		return;
	}
	if (e->slv == 1) {
		e->mark_angle = dir_to(e->x, e->z, p->x, p->z);
		approach_angle(&e->yaw, e->mark_angle, aim_rate() * dt * 60.0f);
		e->path_angle = e->yaw;
		// attack 42 (sh2_attack_list at 0x629000): kind 4 spray, ap 7, frames 9..22.
		float f = e->frame - SCU_ANIM[A_ATTACK].start;
		if (e->attacking && f >= 9.0f && f <= 22.0f)
			blow_maria(e, attack_range(p) + 150.0f, 0.4f, 7.0f);
		if (e->attacking && !e->attack_hit && f >= 9.0f && f <= 22.0f && p->alive) {
			float fx = sinf(e->yaw), fz = cosf(e->yaw);
			float dx = p->x - e->x, dz = p->z - e->z;
			float along = dx * fx + dz * fz, side = fabsf(dx * fz - dz * fx);
			if (along > 0.0f && along < attack_range(p) + 150.0f && side < 250.0f + 130.0f &&
			    line_of_sight(e, p)) {
				e->attack_hit = true;
				hit_james(p, 42, 7.0f, e);
			}
		}
		if (!e->anim_end)
			return;
		e->attacking = false;
		if (rnd() < repeat_probability() && sight_check(e, p, dt) == 3) {
			e->slv = 0;
			return;
		}
		if ((e->type & 2) && e->p_dist < 750.0f && s_diff > 0) {
			go(e, SCU_BACKWARD);
			return;
		}
		go(e, SCU_CHASE);
		return;
	}
	// slv 2: did not spit, waits out the stand loop
	if (!e->anim_end)
		return;
	if (e->type & 2) {
		e->slv = 0;
		if (e->p_dist >= 750.0f)
			go(e, SCU_CHASE);
		return;
	}
	go(e, SCU_CHASE);
}

// 0x4a7490: flinch anime for the attack that hit (plank 12/13: side of the blow, 14: from above).
static int flinch_anim(const Enemy* e, float side)
{
	switch (e->last_atk) {
	case 4: case 6: return fabsf(side) <= PI / 2.0f ? 7 : 6;   // shotgun / rifle: from behind / front
	case 12: case 13: case 15: case 16: return side < 0.0f ? 8 : 9;
	case 14: case 17: case 30: return 10;
	case 42: case 43: return 5;
	default: return 5;
	}
}

// 0x4a7620: knock-down anime.
static int down_anim(const Enemy* e, float side)
{
	switch (e->last_atk) {
	case 4: case 6: return fabsf(side) <= PI / 2.0f ? 16 : 15;  // thrown forward / onto the back
	case 12: case 13: case 15: case 16: return side < 0.0f ? 17 : 18;
	case 14: case 17: return 19;
	default: return A_DOWN0;
	}
}

static float blow_side(const Enemy* e)
{
	float a = wrap(atan2f(e->hbx, e->hbz) - e->yaw);
	return a;
}

// 0x4c65d0 enSCUCtrlDamage
static void scu_damage(Enemy* e, const EnPlayer* p, float dt)
{
	(void)p, (void)dt;
	if (e->slv == 0) {
		float side = blow_side(e);
		float endurance = reduce_hp(e);
		int a;
		if (endurance > 0.0f && (endurance >= 100.0f || rnd() <= endurance * 0.002f + 0.8f))
			a = flinch_anim(e, side);
		else
			a = down_anim(e, side);
		pose(e, a);
		set_hitback(e);
		if (a < 14 || a > 21) {
			e->slv = 1;
		} else {
			e->lie = (a == 15);          // 0x4a7760
			size_lying(e, false);
			e->slv = 2;
		}
		return;
	}
	if (e->slv == 1) {
		if (e->anim_end && e->hb == 0.0f) { go(e, SCU_PRECAUTION); return; }
		if (check_damage(e)) e->slv = 0;
		return;
	}
	// slv 2: falling
	if (check_damage(e))
		reduce_hp(e);
	if (!e->anim_end)
		return;
	go(e, SCU_DOWN);
}

static void death_throes(Enemy* e)          // 0x4c4310
{
	set_timer(e, 0);
	e->cnt = 0;
	e->cnt_max = (u8)(4 + (int)(rnd() * (s_diff + 2)));
}

static void hurt_on_floor(Enemy* e)
{
	pose(e, A_LIE + e->lie);
	reduce_hp(e);
	set_hitback(e);
}

// 0x4c6900 enSCUCtrlDown: lies on the floor, writhes, gets up or crawls.
static void scu_down(Enemy* e, const EnPlayer* p, float dt)
{
	int ticks = (int)(dt * 60.0f + 0.5f);
	switch (e->slv) {
	case 0:
		size_lying(e, false);
		if (!is_dead(e)) {
			set_timer(e, down_time(e));
			if (e->status == 3 || e->status == 4)
				e->timer += (int)(rnd() * 600.0f);
			e->slv = 1;
			return;
		}
		death_throes(e);
		e->slv = 4;
		return;
	case 1:
		if (check_damage(e)) {
			hurt_on_floor(e);
			if (!is_dead(e)) { e->slv = 3; set_timer(e, 90); return; }
			death_throes(e);
			e->slv = 4;
			return;
		}
		if (reduce_timer(e, ticks) > 0 || p->stomping) {
			if (!(e->flags & 0x2000) || p->stomping)
				return;
			if (e->anim != A_WRITHE + e->lie) {
				if (rnd() * 200.0f < hp_rate(e) + 100.0f)
					pose(e, A_WRITHE + e->lie);
			} else if (rnd() * 100.0f > hp_rate(e)) {
				pose(e, A_LIE + e->lie);
			}
			add_hp(e, 10.0f);
			return;
		}
		if (s_diff == 0) {
			pose(e, A_LIE + e->lie);
			e->hp = 0.0f;
			start_dying(e);
			return;
		}
		add_hp(e, 50.0f);
		if (e->kind == EN_KIND_SCU && (e->type == 0xff || (e->lie == 0 && rnd() < stay_down_probability()))) {
			go(e, SCU_CRAWL);
			return;
		}
		stand_up(e);
		e->slv = 2;
		return;
	case 2:
		if (!e->anim_end)
			return;
		e->flags &= ~0x10000u;
		size_upright(e, false);
		go(e, sight_check(e, p, dt) > 0 ? SCU_CHASE : SCU_PRECAUTION);
		return;
	case 3:
		if (check_damage(e)) {
			hurt_on_floor(e);
			if (is_dead(e)) { death_throes(e); e->slv = 4; return; }
			set_timer(e, 90);
			return;
		}
		if (e->hb != 0.0f || reduce_timer(e, ticks) > 0)
			return;
		if (s_diff == 0) { e->hp = 0.0f; start_dying(e); return; }
		if (e->type == 0xff) { e->slv = 1; return; }
		if (p->stomping) { e->slv = 6; return; }
		add_hp(e, 50.0f);
		stand_up(e);
		e->slv = 2;
		return;
	case 4:
		// death throes: a few twitches with the hit-back fading, then dead
		if (check_damage(e))
			reset_damage(e);
		if (reduce_timer(e, ticks) > 0)
			return;
		if (e->cnt < e->cnt_max) {
			e->cnt++;
			pose(e, (e->cnt & 1) ? A_WRITHE + e->lie : A_LIE + e->lie);
			set_timer(e, 40 + (int)(rnd() * 40.0f));
			return;
		}
		pose(e, A_LIE + e->lie);
		start_dying(e);
		return;
	case 6:
		if (check_damage(e)) { hurt_on_floor(e); e->slv = 3; set_timer(e, 90); return; }
		if (p->stomping)
			return;
		add_hp(e, 50.0f);
		stand_up(e);
		e->slv = 2;
		return;
	}
}

// 0x4c5800 enSCUCtrlCrawl: scuttles along the floor, grabbing at James' legs (attack 43).
static void scu_crawl(Enemy* e, const EnPlayer* p, float dt)
{
	int ticks = (int)(dt * 60.0f + 0.5f);
	if (check_damage(e)) {
		if (e->slv == 2 || e->slv <= 0) {
			reset_damage(e);
		} else {
			hurt_on_floor(e);
			e->vx = e->vz = 0.0f;
			if (is_dead(e)) { go(e, SCU_DOWN); e->slv = 4; death_throes(e); return; }
			e->slv = 5;
			set_timer(e, 10);
		}
	}
	int seen = sight_check(e, p, dt);
	switch (e->slv) {
	case 0:
		pose(e, A_CRAWL);
		size_lying(e, false);
		e->path_angle = e->yaw;
		{
			float a = rnd_signed(-PI / 2.0f, 0.5f) + e->yaw;
			e->tx = e->x + sinf(a) * 1000.0f;
			e->tz = e->z + cosf(a) * 1000.0f;
		}
		set_timer(e, (int)((1.0f + rnd_signed(-0.5f, 0.5f)) * 180.0f));
		if (s_diff < 2)
			e->flags |= 0x200;
		e->attacking = s_diff >= 2;
		e->slv = 1;
		// fall through
	case 1: {
		if (s_diff == 0 && !(e->type & 0x18))
			e->life -= dt;
		e->crawl_snd -= dt * 60.0f;
		if (e->crawl_snd <= 0.0f) {
			e->crawl_snd = 20.0f;
			play_at(e, 12009 + (int)(rnd() * 5.99f));
		}
		// attack 43: kind 3, ap 5, reach 250 in front, rearmed every frame on normal and up
		if (e->attacking && !e->maria_hit) {
			blow_maria(e, 250.0f + e->size, 0.6f, 5.0f);
			e->maria_hit = false;       // rearmed every frame, like the grab on James
		}
		if (e->attacking && p->alive) {
			float fx = sinf(e->yaw), fz = cosf(e->yaw);
			float dx = p->x - e->x, dz = p->z - e->z;
			float along = dx * fx + dz * fz, side = fabsf(dx * fz - dz * fx);
			if (along > 0.0f && along < 250.0f + e->size && side < 200.0f)
				hit_james(p, 43, 5.0f, e);
		}
		if (hypotf(e->tx - e->x, e->tz - e->z) < 400.0f || rnd() < 0.002f) {
			float a = rnd_signed(-PI / 2.0f, 0.5f) + e->yaw;
			e->tx = e->x + sinf(a) * 1000.0f;
			e->tz = e->z + cosf(a) * 1000.0f;
		}
		if (p->stomping && e->p_dist < 1000.0f) {
			e->vx = e->vz = 0.0f;
			set_timer(e, (int)((1.0f + rnd_signed(-0.5f, 0.5f)) * 180.0f));
			e->slv = 3;
			return;
		}
		if (reduce_timer(e, ticks) <= 0) {
			e->vx = e->vz = 0.0f;
			if ((e->type & 0x10) && !(e->type & 0x20) && e->p_dist < 2000.0f) {
				stand_up(e);
				e->slv = 2;
				return;
			}
			if ((seen != 3 && rnd() >= 0.02f) || (e->type & 0x30) || e->type == 0xff) {
				if (rnd() < 0.1f) {
					pose(e, A_WRITHE);
					set_timer(e, (int)((1.0f + rnd_signed(-0.5f, 0.5f)) * 180.0f));
					e->slv = 4;
					return;
				}
				if (e->life >= 0.0f) {
					set_timer(e, (int)((1.0f + rnd_signed(-0.5f, 0.5f)) * 180.0f));
					e->slv = 3;
					add_hp(e, 10.0f);
					return;
				}
			} else if (e->life >= 0.0f) {
				add_hp(e, 10.0f);
				stand_up(e);
				e->slv = 2;
				return;
			}
			pose(e, A_LIE);
			e->hp = 0.0f;
			start_dying(e);
			return;
		}
		if (!(e->flags & 0x200) && e->p_dist < 600.0f) {
			e->mark_angle = dir_to(e->x, e->z, p->x, p->z);
		} else if (steer(e, e->tx, e->tz)) {
			e->tx = e->x + sinf(e->mark_angle) * 1000.0f;
			e->tz = e->z + cosf(e->mark_angle) * 1000.0f;
		}
		approach_angle(&e->path_angle, e->mark_angle, turn_rate() * dt * 60.0f);
		e->yaw = e->path_angle;
		float r = crawl_rate(e);
		e->rate = r;
		e->vx = sinf(e->yaw) * r * 1500.0f;
		e->vz = cosf(e->yaw) * r * 1500.0f;
		return;
	}
	case 2:
		if (!e->anim_end)
			return;
		e->flags &= ~0x10000u;
		size_upright(e, false);
		e->flags &= ~0x200u;
		go(e, seen > 0 ? SCU_CHASE : SCU_PRECAUTION);
		return;
	case 3:
		// resting flat on the floor between dashes
		if (s_diff == 0 && !(e->type & 8))
			e->life -= dt;
		e->rate = 0.0f;
		if (reduce_timer(e, ticks) > 0 || p->stomping)
			return;
		if ((seen > 1 || e->hp >= e->hp_max * 0.5f) && e->type != 0xff && s_diff > 0 && rnd() < 0.3f) {
			add_hp(e, 50.0f);
			stand_up(e);
			e->slv = 2;
			return;
		}
		e->slv = 0;
		return;
	case 4:
		if (reduce_timer(e, ticks) > 0)
			return;
		add_hp(e, 10.0f);
		if (e->life < 0.0f) { pose(e, A_LIE); e->hp = 0.0f; start_dying(e); return; }
		e->slv = 0;
		return;
	case 5:
		if (e->hb == 0.0f && reduce_timer(e, ticks) <= 0)
			go(e, SCU_DOWN);
		return;
	case 0xff:
		pose(e, A_TO_CRAWL);
		size_lying(e, false);
		e->slv = 0xfe;
		return;
	case 0xfe:
		if (e->anim_end)
			e->slv = 0;
		return;
	}
}

// Dead for good: the Enemy_List id goes into the killed bits, and the results screen and the ending
// score count the kill by the attack that ended it (0x53a220, 0x539760).
// enDyingExec also rolls for a regeneration: Lying Figures, Mannequins, creepers, nurses (and the
// Mandarin) spawned alive come back with chance {0.1, 0.1, 0.2, 0.4} (control 6, 0x4aa760, which
// revives them once James has been 30000 away and comes within 15000 again); half the Lying
// Figures come back crawling (status 7). The port brings them back the next time the room loads.
static void mark_killed(const Enemy* e)
{
	static const float regen[5] = { 0.1f, 0.1f, 0.2f, 0.4f, 0.8f };
	if (e->status != 5)
		prog_stat_kill(e->last_atk);   // not the bodies placed dead
	if (e->list_id < 0 || e->list_id >= MAX_LIST)
		return;
	if (e->kind != EN_KIND_RED && e->status < 5 && rnd() < regen[s_diff]) {
		s_regen[e->list_id] = (u8)((e->kind == EN_KIND_SCU && rnd() < 0.5f ? 7 : e->status) + 1);
		return;
	}
	s_killed[e->list_id >> 5] |= 1u << (e->list_id & 31);
}

// 0x4a96c0 enDyingExec: the body stays where it fell. The PC can bring some back later (ctrl 6,
// enWaitRegenerate); the port marks the Enemy_List id as killed instead.
static void scu_dying(Enemy* e)
{
	if (e->slv == 0) {
		e->attacking = false;
		e->vx = e->vz = 0.0f;
		pose(e, A_LIE + e->lie);
		mark_killed(e);
		e->dead_flag = true;
		e->slv = 1;
	}
}

// ---------------------------------------------------------------- per-frame

// enMoveExec 0x4a9e60: size easing, root motion, crawl velocity, hit-back and the room.
static void move_exec(Enemy* e, float adv, float dt)
{
	float step = dt * 25.0f * 30.0f / 30.0f;    // size eases 25 units per 1/30 s (0x63887c)
	if (e->size != e->new_size) {
		float d = e->new_size - e->size;
		e->size += d > step ? step : d < -step ? -step : d;
	}
	float tstep = dt * 30.0f * 20.0f;            // tall eases 20 units per 1/30 s
	if (e->tall != e->new_tall) {
		float d = e->new_tall - e->tall;
		e->tall += d > tstep ? tstep : d < -tstep ? -tstep : d;
	}
	float mx = e->vx * dt, mz = e->vz * dt;
	// Foot root motion (0x4a7a20): the Lying Figure's walk and staggers (0x4c3c20 anime 1, 11, 12),
	// the Mannequin's walk, both attacks (a lunge of about 500) and staggers (0x4c1020 anime 1, 3, 4,
	// 11, 12). Average units per animation frame, measured from the .anm files.
	float stride = 0.0f;
	if (e->kind == EN_KIND_NSE)      // 0x4b9c80: anime 1, 3, 4, 11, 12 (pair 12/13 at 0x8927a0)
		stride = e->anim == A_WALK ? 10.8f : e->anim == 3 ? 6.9f : e->anim == 4 ? 13.0f :
		         e->anim == 11 ? -11.9f : e->anim == 12 ? 8.5f : 0.0f;
	else if (e->kind == EN_KIND_TYU)
		stride = 0.0f;                 // moves by its velocity (EnLOCAL +0x34)
	else if (e->kind == EN_KIND_RED)
		stride = e->anim == 1 ? 14.3f : 0.0f;   // feet 12/13 (0x892790), measured from red.anm
	else if (e->kind == EN_KIND_PAP)
		stride = e->anim == 1 ? 20.8f : 0.0f;   // feet 20 / 24 (0x8927b0)
	else if (e->kind == EN_KIND_ONI)
		stride = e->anim == 1 ? 9.6f : 0.0f;    // feet 12 / 13 (0x892798)
	else if (e->kind == EN_KIND_ARM)
		stride = e->anim >= 2 && e->anim <= 5 ? 34.7f : 0.0f;   // a planted hand: about 590 a step
	else if (e->anim == 0 || e->anim == A_WALK)
		stride = e->kind == EN_KIND_MKN ? MKN_WALK_STRIDE : SCU_WALK_STRIDE;
	else if (e->kind == EN_KIND_MKN)
		stride = e->anim == 3 || e->anim == 4 ? 14.7f : e->anim == 11 ? -12.5f : e->anim == 12 ? 13.8f : 0.0f;
	else
		stride = e->anim == 11 ? -10.7f : e->anim == 12 ? 7.1f : 0.0f;
	if (stride != 0.0f) {
		mx += sinf(e->yaw) * adv * stride;
		mz += cosf(e->yaw) * adv * stride;
	}
	if (e->hb != 0.0f) {
		float n = dt * 60.0f;
		float total = 0.0f;
		while (n > 0.0f && e->hb != 0.0f) {
			e->hb *= 0.5f;
			total += e->hb * (n >= 1.0f ? 1.0f : n);
			if (e->hb < 1.0f) e->hb = 0.0f;
			n -= 1.0f;
		}
		mx += e->hbx * total;
		mz += e->hbz * total;
	}
	if (mx == 0.0f && mz == 0.0f)
		return;
	float nx = e->x + mx, nz = e->z + mz;
	// bodies do not overlap: an upright enemy stops at James' collision radius (130) plus its own
	float mind = 130.0f + e->size * 0.8f;
	float dx = nx - s_px, dz = nz - s_pz, d = sqrtf(dx * dx + dz * dz);
	if (d < mind && d < hypotf(e->x - s_px, e->z - s_pz) && (!enemy_is_down(e) || e->kind == EN_KIND_TYU)) {
		if (d < 1.0f) return;
		nx = s_px + dx / d * mind;
		nz = s_pz + dz / d * mind;
	}
	if (s_col) {
		float ny;
		col_push(s_col, &nx, &nz, e->y, e->tall, e->size);
		if (col_floor(s_col, nx, nz, e->y, 200.0f, &ny)) {
			e->x = nx, e->z = nz, e->y = ny;
		}
	} else {
		e->x = nx, e->z = nz;
	}
}

// Footsteps on the walk cycle (0x4c3c20 / 0x4c1020: frames 2 and 18 of the Lying Figure's walk,
// 2 and 17 of the Mannequin's) and the idle noises (enSCUSoundLife 0x4c4350, every 5 s at the
// earliest, sooner while standing; Mannequin 0x4c1490).
static void sounds(Enemy* e, float f0, float dt)
{
	if (e->kind == EN_KIND_NSE) {
		// footsteps on walk frames 1 and 18 (0x4b9c80, SE 0x2f44); the radio-like breathing every
		// second at 0.6 volume (0x4ba090, 0x2f47..0x2f4a)
		if (e->anim == A_WALK) {
			float a = f0 - NSE_ANIM[1].start, b = e->frame - NSE_ANIM[1].start;
			if (b < a) b += NSE_ANIM[1].end - NSE_ANIM[1].start + 1;
			if ((a < 1.0f && b >= 1.0f) || (a < 18.0f && b >= 18.0f))
				play_at(e, 12100);
		}
		e->sound_wait += dt * 60.0f;
		if (e->sound_wait >= 60.0f && e->mlv != NSE_DYING) {
			e->sound_wait = 0.0f;
			float pos[3] = { e->x, e->y, e->z };
			sfx_play_at(12103 + (int)(rnd() * 3.99f), 0.6f, pos);
		}
		return;
	}
	bool mkn = e->kind == EN_KIND_MKN;
	if (e->anim == A_WALK || e->anim == 0) {
		const ScuAnim* t = &anim_tab(e)[e->anim];
		float a = f0 - t->start, b = e->frame - t->start;
		float s1 = 2.0f, s2 = mkn ? 17.0f : 18.0f;
		if (b < a) b += t->end - t->start + 1;     // wrapped
		if ((a < s1 && b >= s1) || (a < s2 && b >= s2))
			play_at(e, mkn ? 12500 : 12019);
	}
	e->sound_wait += dt * 60.0f;
	if (e->sound_wait < 300.0f || e->mlv == (mkn ? MKN_DYING : SCU_DYING) || (mkn && e->mlv == MKN_STAY))
		return;
	if (e->anim != A_STAND && rnd() >= dt * 0.2f)
		return;
	play_at(e, mkn ? 12505 + (int)(rnd() * 3.99f) : 12001 + (int)(rnd() * 7.99f));
	e->sound_wait = 0.0f;
}

static void scu_update(Enemy* e, const EnPlayer* p, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	if (e->ctrl != 1)
		return;
	switch (e->mlv) {
	case SCU_WANDER: scu_wander(e, p, dt); break;
	case SCU_PRECAUTION: scu_precaution(e, p, dt); break;
	case SCU_CHASE: scu_chase(e, p, dt); break;
	case SCU_BACKWARD: scu_backward(e, p, dt); break;
	case SCU_AMBUSH: scu_ambush(e, p, dt); break;
	case SCU_WAITCAR: if (e->p_dist < 2000.0f) go(e, SCU_CRAWL); break;   // event wait, simplified
	case SCU_CRAWL: scu_crawl(e, p, dt); break;
	case SCU_ATTACK: scu_attack(e, p, dt); break;
	case SCU_DAMAGE: scu_damage(e, p, dt); break;
	case SCU_CONFUSE: go(e, SCU_PRECAUTION); break;
	case SCU_DOWN: scu_down(e, p, dt); break;
	case SCU_DYING: scu_dying(e); break;
	}
	float f0 = e->frame;
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	sounds(e, f0, dt);
}


// ---------------------------------------------------------------- Mannequin (kind 0x201, chr/mkn)
//
// PC en_mkn code: 0x4c3890 control main, 0x4c3790 automatic, 0x4c15c0 init, states 0x4c18f0 stay,
// 0x4c1bb0 event, 0x4c1cd0 / 0x4c20f0 drop from above, 0x4c2420 wander, 0x4c2860 precaution,
// 0x4c2950 chase, 0x4c2a90 circle, 0x4c2c70 attack, 0x4c2e20 damage, 0x4c3030 confuse,
// 0x4c3250 down. Damage, down and dying share the Lying Figure code above (same en_common logic).

static float mkn_walk_rate(const Enemy* e, const EnPlayer* p)   // 0x4c11d0
{
	static const float t[5] = { 0.7f, 0.8f, 1.0f, 1.4f, 1.5f };
	float r = t[s_diff] * (hp_rate(e) * 0.004f + 0.6f);
	int li = light_index(p);
	if (li <= 1 && s_diff >= 2) r *= 1.2f;
	else if (li == 3 && s_diff <= 2) r *= 0.8f;
	return r;
}

static float mkn_turn_rate(const EnPlayer* p)                  // 0x4c1350
{
	static const float t[5] = { 0.5f, 0.8f, 1.0f, 1.2f, 1.5f };
	return t[s_diff] * (light_index(p) == 3 ? 0.5f : 1.0f) * 0.05235988f;
}

static float mkn_attack_rate(const Enemy* e)                   // 0x4c12f0
{
	static const float t[5] = { 0.5f, 0.8f, 1.0f, 1.2f, 1.5f };
	return t[s_diff] * (hp_rate(e) * 0.0025f + 0.75f);
}

// 0x4c0eb0: does a standing-still mannequin react? 0 no, 1 wake up, 2 strike at once. It needs a
// clear line and James within 500 in height; in the dark it reacts to the flashlight pointed at it
// within 2250 (40 degrees), and to anything within 900.
static int mkn_wake(const Enemy* e, const EnPlayer* p)
{
	if (fabsf(p->y - e->y) > 500.0f || !p->alive || !line_of_sight(e, p))
		return 0;
	float d = e->p_dist;
	int li = light_index(p);
	bool near = false;
	if (li == 0) {
		if (d > 3500.0f)
			return 0;
		near = true;
	} else if (!p->light) {
		float a = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
		near = d <= 1250.0f && a <= PI / 2.0f;
	} else {
		float a = fabsf(wrap(p->yaw + PI - dir_to(e->x, e->z, p->x, p->z)));
		near = d <= 2250.0f && a <= 0.6981317f;
	}
	if (!near)
		return d < 900.0f ? 1 : 0;
	return d < 1000.0f ? 2 : 1;
}

// attacks 44 (anime 3, arm swing) / 45 (anime 4, kick): kind 2, ap 18, sp 10, reach 400, frames 19..22
static void mkn_attack_hit(Enemy* e, const EnPlayer* p)
{
	float f = e->frame - MKN_ANIM[e->anim].start;
	if (!e->attacking || f < 19.0f || f > 22.99f)
		return;
	blow_maria(e, 700.0f, 1.0f, 18.0f);
	if (e->attack_hit || !p->alive)
		return;
	float dx = p->x - e->x, dz = p->z - e->z;
	float d = sqrtf(dx * dx + dz * dz);
	float a = fabsf(wrap(atan2f(dx, dz) - e->yaw));
	// reach 400 is measured from the striking limb (shGetEnemyMKNAttackPos, not ported): about 300
	// out from the body, plus James' radius
	if (d < 400.0f + 300.0f + 130.0f && a < 1.0f) {
		e->attack_hit = true;
		hit_james(p, e->anim == 3 ? 44 : 45, 18.0f, e);
	}
}

static void mkn_stay(Enemy* e, const EnPlayer* p, float dt)
{
	(void)dt;
	if (check_damage(e)) {
		size_upright(e, false);
		go(e, SCU_DAMAGE);
		return;
	}
	switch (e->slv) {
	case 0: {
		// frozen in one of the two attack poses (local table {3, 0..17}, {4, 0..17})
		int a = rnd() < 0.5f ? 3 : 4;
		anim_set(e, a);
		e->frame = MKN_ANIM[a].start + rnd() * 17.0f;
		e->rate = 0.0f;
		e->slv = 2;
		return;
	}
	case 1: case 2: {
		int w = mkn_wake(e, p);
		if (w == 2) {
			comm_add(e->x, e->z, 1000.0f, 2, 2);
			if (e->type == 0) e->type = 1;
			go(e, SCU_ATTACK);
			return;
		}
		if (w == 1) {
			set_timer(e, 0);
			e->slv = 3;
		} else if (comm_heard_kind(e, 2)) {
			set_timer(e, 90);
			e->slv = 3;
		}
		return;
	}
	case 3:
		if (reduce_timer(e, (int)(dt * 60.0f + 0.5f)) > 0)
			return;
		comm_add(e->x, e->z, 1000.0f, 2, 2);
		e->rate = 1.0f;
		e->path_angle = e->yaw;
		e->attacking = true;
		e->attack_hit = e->maria_hit = false;
		e->slv = 4;
		return;
	case 4:
		mkn_attack_hit(e, p);
		e->mark_angle = dir_to(e->x, e->z, p->x, p->z);
		approach_angle(&e->yaw, e->mark_angle, 0.01745f * dt * 60.0f);
		e->path_angle = e->yaw;
		if (!e->anim_end)
			return;
		e->attacking = false;
		if (e->type == 0) e->type = 1;
		go(e, SCU_PRECAUTION);
		return;
	}
}

// 0x4c2420: walks to points 1500 away, keeping within 5000 of its start.
static void mkn_wander(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	switch (e->slv) {
	case 0:
		e->stx = e->x, e->stz = e->z;
		pose(e, A_WALK);
		e->rate = mkn_walk_rate(e, p);
		e->path_angle = e->yaw;
		e->tx = e->x + sinf(e->yaw) * 1500.0f;
		e->tz = e->z + cosf(e->yaw) * 1500.0f;
		e->slv = 1;
		// fall through
	case 1: {
		if (e->type > 1 || hypotf(e->stx - e->x, e->stz - e->z) <= 5000.0f) {
			float r = rnd();
			if (hypotf(e->tx - e->x, e->tz - e->z) < 600.0f || r < 0.02f) {
				float a = (rnd() - 0.5f) * (PI / 2.0f) + e->yaw;
				e->tx = e->x + sinf(a) * 1500.0f;
				e->tz = e->z + cosf(a) * 1500.0f;
				if (e->type < 2 && hypotf(e->tx - e->stx, e->tz - e->stz) > 5000.0f) {
					a = e->yaw + ((rnd() < 0.5f) ? PI / 2.0f : -PI / 2.0f);
					e->tx = e->x + sinf(a) * 1500.0f;
					e->tz = e->z + cosf(a) * 1500.0f;
				}
			} else if (r < 0.021f) {
				pose(e, A_STAND);
				e->slv = 3;
			}
		} else {
			e->tx = e->stx, e->tz = e->stz;
			set_timer(e, 300);
			e->slv = 2;
		}
		break;
	}
	case 2:
		e->rate = mkn_walk_rate(e, p);
		if (hypotf(e->stx - e->x, e->stz - e->z) < 5000.0f || reduce_timer(e, 1) <= 0)
			e->slv = 1;
		break;
	case 3:
		if (!e->anim_end)
			break;
		pose(e, A_WALK);
		e->rate = mkn_walk_rate(e, p);
		e->slv = 1;
		break;
	}
	if (e->slv != 3) {
		if (steer(e, e->tx, e->tz)) {
			e->tx = e->x + sinf(e->mark_angle) * 1500.0f;
			e->tz = e->z + cosf(e->mark_angle) * 1500.0f;
		}
		approach_angle(&e->path_angle, e->mark_angle, mkn_turn_rate(p) * dt * 60.0f);
		e->yaw = e->path_angle;
	}
	recover_endurance(e, dt);
	int seen = sight_check(e, p, dt);
	if (seen == 3) { if (e->loops > 0) go(e, SCU_ATTACK); }
	else if (seen == 2) go(e, SCU_CHASE);
	else if (seen == 1) go(e, SCU_PRECAUTION);
}

static void mkn_precaution(Enemy* e, const EnPlayer* p, float dt)   // 0x4c2860
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		pose(e, A_STAND);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	int seen = sight_check(e, p, dt);
	if (e->anim_end) {
		if (seen == 3) { go(e, SCU_ATTACK); return; }
		if (seen == 1 && e->p_dist > 350.0f) { go(e, SCU_CHASE); return; }
		if (seen == 0) { go(e, SCU_WANDER); return; }
		pose(e, A_STAND);
	}
	e->mark_angle = dir_to(e->x, e->z, p->x, p->z);
	approach_angle(&e->path_angle, e->mark_angle, mkn_turn_rate(p) * dt * 60.0f);
	e->yaw = e->path_angle;
	recover_endurance(e, dt);
}

static void mkn_chase(Enemy* e, const EnPlayer* p, float dt)        // 0x4c2950
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		pose(e, A_WALK);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	steer(e, p->x, p->z);
	approach_angle(&e->path_angle, e->mark_angle, mkn_turn_rate(p) * dt * 60.0f);
	e->yaw = e->path_angle;
	float off = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	e->rate = mkn_walk_rate(e, p) * (((PI - off) / PI) + 1.0f) * 0.5f;
	recover_endurance(e, dt);
	int seen = sight_check(e, p, dt);
	if (seen == 3) {
		if (e->loops > 0) go(e, SCU_ATTACK);
	} else if (seen == 0) {
		if (e->type < 2 && s_diff > 1) go_raw(e, MKN_CIRCLE);
		else go(e, SCU_WANDER);
	} else if (seen == 2) {
		go(e, SCU_PRECAUTION);
	}
}

// 0x4c2a90: loses sight of James and walks round to come at him from 120 degrees off his facing.
static void mkn_circle(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		pose(e, A_WALK);
		e->rate = mkn_walk_rate(e, p);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	float side = wrap(dir_to(p->x, p->z, e->x, e->z) - p->yaw);
	float a = p->yaw + (side >= 0.0f ? 2.0943952f : -2.0943952f);
	float tx = p->x + sinf(a) * 1000.0f, tz = p->z + cosf(a) * 1000.0f;
	steer(e, tx, tz);
	if (hypotf(tx - e->x, tz - e->z) < 500.0f)
		go(e, SCU_CHASE);
	approach_angle(&e->path_angle, e->mark_angle, mkn_turn_rate(p) * dt * 60.0f);
	e->yaw = e->path_angle;
	recover_endurance(e, dt);
	int seen = sight_check(e, p, dt);
	if (seen == 3) { if (e->loops > 0) go(e, SCU_ATTACK); }
	else if (seen == 2) go(e, SCU_CHASE);
	else if (seen == 0 && e->p_dist > 3500.0f) go(e, SCU_WANDER);
}

static void mkn_attack(Enemy* e, const EnPlayer* p, float dt)       // 0x4c2c70
{
	static const float prob[5] = { 0.4f, 0.5f, 0.8f, 0.9f, 1.0f };   // 0x4c1260
	static const float again[5] = { 0.0f, 0.0f, 0.2f, 0.6f, 1.0f };  // 0x4c12b0
	if (check_damage(e)) { e->attacking = false; go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		if (rnd() >= prob[s_diff]) {
			pose(e, A_STAND);
			e->slv = 2;
		} else {
			int a = e->anim == 3 ? 4 : e->anim == 4 ? 3 : (rnd() < 0.5f ? 3 : 4);
			anim_set(e, a);
			e->frame = MKN_ANIM[a].start;
			e->rate = mkn_attack_rate(e);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			e->slv = 1;
		}
	} else if (e->slv == 1) {
		mkn_attack_hit(e, p);
		if (e->anim_end) {
			e->attacking = false;
			if (rnd() < again[s_diff] && sight_check(e, p, dt) == 3)
				e->slv = 0;
			else
				go(e, SCU_CHASE);
			return;
		}
	} else if (e->anim_end) {
		e->slv = 0;
		if (sight_check(e, p, dt) != 3)
			go(e, SCU_CHASE);
		return;
	}
	e->mark_angle = dir_to(e->x, e->z, p->x, p->z);
	approach_angle(&e->yaw, e->mark_angle, mkn_turn_rate(p) * dt * 60.0f);
	e->path_angle = e->yaw;
	recover_endurance(e, dt);
}

static void mkn_update(Enemy* e, const EnPlayer* p, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	if (e->ctrl != 1)
		return;
	switch (e->mlv) {
	case MKN_STAY: mkn_stay(e, p, dt); break;
	case MKN_EVENT: case MKN_FALL: case MKN_FALL2: go_raw(e, MKN_STAY); break;   // event drops, not ported
	case MKN_WANDER: mkn_wander(e, p, dt); break;
	case MKN_PRECAUTION: mkn_precaution(e, p, dt); break;
	case MKN_CHASE: mkn_chase(e, p, dt); break;
	case MKN_CIRCLE: mkn_circle(e, p, dt); break;
	case MKN_ATTACK: mkn_attack(e, p, dt); break;
	case MKN_DAMAGE:
		if (e->slv == 0)
			comm_add(e->x, e->z, 1000.0f, 2, 2);
		scu_damage(e, p, dt);
		break;
	case MKN_CONFUSE: go(e, SCU_PRECAUTION); break;   // fights other enemies, not ported
	case MKN_DOWN: scu_down(e, p, dt); break;
	case MKN_DYING: scu_dying(e); break;
	}
	float f0 = e->frame;
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	sounds(e, f0, dt);
}

// 0x4c15c0 enMKNInitData
static void mkn_init(Enemy* e, int status)
{
	static const float vitality[5] = { 400, 400, 700, 1500, 9999 };
	static const float endurance[5] = { 10, 190, 285, 380, 450 };
	e->ctrl = 1;
	e->mlv = MKN_STAY;
	e->slv = 0;
	e->path_angle = e->mark_angle = e->yaw;
	e->type = 0;
	size_upright(e, true);
	anim_set(e, A_WALK);
	e->stx = e->x, e->stz = e->z;
	float scale = 0.7f;
	switch (status) {
	case 1: scale = 1.2f; break;
	case 2: scale = 0.5f; break;
	case 3: case 4:
		if (status == 4) e->type = 5;
		go_raw(e, MKN_DOWN);
		anim_set(e, A_LIE);
		set_size(e, 250.0f, 200.0f, true);
		scale = 0.3f;
		break;
	case 5:
		go_raw(e, MKN_DYING);
		e->hp = 0.0f;
		anim_set(e, A_LIE);
		set_size(e, 250.0f, 200.0f, true);
		return;
	case 9:
		// lies in wait (event state 1 in the PC, it gets up when the story says so): wakes like a
		// standing mannequin here
		e->type = 6;
		scale = 1.0f;
		break;
	case 10: case 11: scale = 1.0f; break;   // drops from the ceiling (not ported): stands instead
	default: break;
	}
	set_hp(e, scale * vitality[s_diff], scale * endurance[s_diff]);
}

// ---------------------------------------------------------------- Bubble Head Nurse (kind 0x207, chr/nse)
//
// PC en_nse code (EnLOCAL kind 6, shared with kind 0x20b): 0x4bb470 control main, 0x4bb390
// automatic, 0x4ba1e0 init, states 0x4ba3f0 wander, 0x4ba6d0 precaution, 0x4ba7e0 chase,
// 0x4ba970 attack, 0x4bac00 damage, 0x4badc0 confuse, 0x4bae80 down and the common dying 0x4a96c0.
// Damage and down follow the same en_common template as the Lying Figure's and share its code.

static float nse_walk_rate(const Enemy* e)                     // 0x4b9e20, times the speed factor
{
	static const float t[5] = { 0.7f, 0.8f, 1.0f, 1.2f, 1.5f };
	return t[s_diff] * e->speed_mul;
}

static float nse_turn_rate(const EnPlayer* p)                  // 0x4b9f50: radians per 1/60 s
{
	static const float t[5] = { 0.5f, 0.8f, 1.0f, 1.2f, 1.5f };
	return t[s_diff] * (light_index(p) == 3 ? 0.5f : 1.0f) * 0.05235988f;
}

// The walk speed factor (+0xa8) moves toward its target (+0xac) by at most 0.01 a frame.
static void nse_speed_drift(Enemy* e, float dt)
{
	float d = e->speed_goal - e->speed_mul, m = 0.01f * dt * 60.0f;
	e->speed_mul += d < -m ? -m : d > m ? m : d;
	e->rate = nse_walk_rate(e);
}

static void nse_wander(Enemy* e, const EnPlayer* p, float dt)          // 0x4ba3f0
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	int seen = sight_check(e, p, dt);
	if (seen > 2) { go(e, SCU_ATTACK); return; }
	if (seen == 1) { go(e, SCU_CHASE); return; }
	if (seen == 2) { go(e, SCU_PRECAUTION); return; }
	float dir = e->yaw;
	if (e->slv == 0) {
		pose(e, A_WALK);
		e->path_angle = e->yaw;
		e->speed_mul = e->speed_goal = rnd() * 0.5f + 1.0f;
		e->slv = 1;
	} else if (e->slv == 1) {
		float r = rnd();
		if (r < 0.002f * dt * 60.0f) {
			// stops to twitch for a while, with a gurgle (0x4ba100, SE 0x2f4d / 0x2f4e)
			pose(e, A_STAND);
			play_at(e, 12109 + (int)(rnd() * 1.99f));
			e->slv = 2;
		} else if (r < 0.02f * dt * 60.0f) {
			e->speed_goal = rnd() * 0.5f + 1.0f;
			dir += (rnd() - 0.5f) * 0.2094395f;
		}
	} else if (e->slv == 2 && e->anim_end) {
		pose(e, A_WALK);
		e->speed_goal = rnd() * 0.5f + 1.0f;
		e->slv = 1;
	}
	if (e->anim == A_WALK)
		nse_speed_drift(e, dt);
	steer(e, e->x + sinf(dir) * 1000.0f, e->z + cosf(dir) * 1000.0f);
	approach_angle(&e->path_angle, e->mark_angle, nse_turn_rate(p) * dt * 60.0f);
	e->yaw = e->path_angle;
	recover_endurance(e, dt);
}

static void nse_precaution(Enemy* e, const EnPlayer* p, float dt)      // 0x4ba6d0
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	int seen = sight_check(e, p, dt);
	if (seen > 2) { go(e, SCU_ATTACK); return; }
	if (seen == 2) { go(e, SCU_CHASE); return; }
	if (e->p_dist > 1500.0f) { go(e, SCU_WANDER); return; }
	if (e->slv == 0 || e->anim_end) {
		pose(e, A_STAND);
		e->slv = 1;
	}
	// 0x4a6d90: turns the body to face James
	approach_angle(&e->yaw, dir_to(e->x, e->z, p->x, p->z), nse_turn_rate(p) * dt * 60.0f);
	e->path_angle = e->yaw;
	recover_endurance(e, dt);
}

static void nse_chase(Enemy* e, const EnPlayer* p, float dt)           // 0x4ba7e0
{
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	int seen = sight_check(e, p, dt);
	if (seen > 2) { go(e, SCU_ATTACK); return; }
	if (seen == 0) { go(e, SCU_WANDER); return; }
	if (e->slv == 0) {
		pose(e, A_WALK);
		e->path_angle = e->yaw;
		e->speed_mul = e->speed_goal = 1.0f;
		e->slv = 1;
	}
	steer(e, p->x, p->z);
	// 0x4a66c0: full speed when heading straight for James, half when he is behind
	float off = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	e->speed_goal = ((PI - off) / PI + 1.0f) * 0.5f;
	nse_speed_drift(e, dt);
	approach_angle(&e->path_angle, e->mark_angle, nse_turn_rate(p) * dt * 60.0f);
	e->yaw = e->path_angle;
	recover_endurance(e, dt);
}

// Attacks 46 (anime 3, overhead swing) and 47 (anime 4, short jab) from sh2_attack_list: ap 20 /
// 15 in frames 15..18 / 13..16. The reach is measured from the pipe in the PC; the port uses the
// body plus 450 for arm and pipe and James' radius.
static void nse_attack_hit(Enemy* e, const EnPlayer* p)
{
	bool a3 = e->anim == 3;
	float f = e->frame - NSE_ANIM[e->anim].start;
	if (!e->attacking || f < (a3 ? 15.0f : 13.0f) || f > (a3 ? 18.99f : 16.99f))
		return;
	blow_maria(e, 700.0f, 1.0f, a3 ? 20.0f : 15.0f);
	if (e->attack_hit || !p->alive)
		return;
	float dx = p->x - e->x, dz = p->z - e->z;
	float d = sqrtf(dx * dx + dz * dz);
	float a = fabsf(wrap(atan2f(dx, dz) - e->yaw));
	if (d < 250.0f + 450.0f + 130.0f && a < 1.0f) {
		e->attack_hit = true;
		hit_james(p, a3 ? 46 : 47, a3 ? 20.0f : 15.0f, e);
	}
}

static void nse_attack(Enemy* e, const EnPlayer* p, float dt)          // 0x4ba970
{
	static const float prob[5] = { 0.1f, 0.5f, 0.7f, 0.9f, 1.0f };     // 0x4b9eb0
	static const float rate[5] = { 0.5f, 0.8f, 1.0f, 1.1f, 1.2f };     // 0x4b9ef0
	if (check_damage(e)) { e->attacking = false; go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		if (rnd() >= prob[s_diff]) {
			pose(e, A_STAND);              // hesitates
			e->slv = 2;
		} else {
			int a = sight_check(e, p, dt) == 4 && rnd() < (s_diff + 1.0f) * 0.2f ? 4 : 3;
			anim_set(e, a);
			e->frame = NSE_ANIM[a].start;
			e->rate = rate[s_diff];
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			e->path_angle = e->yaw;
			play_at(e, 12107 + (int)(rnd() * 1.99f));     // 0x4ba130, SE 0x2f4b / 0x2f4c
			e->slv = 1;
		}
	} else if (e->slv == 1) {
		nse_attack_hit(e, p);
		if (s_diff > 2 || e->anim == 3) {
			approach_angle(&e->yaw, dir_to(e->x, e->z, p->x, p->z), nse_turn_rate(p) * dt * 60.0f);
			e->path_angle = e->yaw;
		}
		if (e->anim_end) {
			e->attacking = false;
			e->slv = 0;
			if (sight_check(e, p, dt) < 3)
				go(e, SCU_CHASE);
		}
	} else if (e->anim_end) {
		e->slv = 0;
		if (sight_check(e, p, dt) < 3)
			go(e, SCU_CHASE);
	}
	recover_endurance(e, dt);
}

// 0x4badc0: stands dazed. Only the hyper spray (attack ids 8 / 9) sends a nurse here, and that item
// is not in the port yet, so this is kept for completeness.
static void nse_confuse(Enemy* e, const EnPlayer* p, float dt)
{
	(void)p;
	if (check_damage(e)) { go(e, SCU_DAMAGE); return; }
	if (e->slv == 0) {
		reduce_hp(e);
		pose(e, A_STAND);
		e->slv = 1;
	}
	if (e->slv == 1 && e->anim_end) {
		set_timer(e, 600 - 90 * s_diff);
		e->slv = 2;
	} else if (e->slv == 2 && reduce_timer(e, (int)(dt * 60.0f + 0.5f)) <= 0) {
		go(e, SCU_PRECAUTION);
	}
}

static void nse_update(Enemy* e, const EnPlayer* p, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	if (e->ctrl != 1)
		return;
	switch (e->mlv) {
	case NSE_WANDER: nse_wander(e, p, dt); break;
	case NSE_PRECAUTION: nse_precaution(e, p, dt); break;
	case NSE_CHASE: nse_chase(e, p, dt); break;
	case NSE_ATTACK: nse_attack(e, p, dt); break;
	case NSE_DAMAGE: scu_damage(e, p, dt); break;
	case NSE_CONFUSE: nse_confuse(e, p, dt); break;
	case NSE_DOWN: scu_down(e, p, dt); break;
	case NSE_DYING: scu_dying(e); break;
	}
	float f0 = e->frame;
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	sounds(e, f0, dt);
}

// 0x4ba1e0 enNSEInitData
static void nse_init(Enemy* e, int status, bool tough)
{
	static const float vitality[5] = { 400, 500, 800, 1500, 9999 };
	static const float endurance[5] = { 10, 190, 285, 380, 450 };
	e->ctrl = 1;
	e->mlv = NSE_WANDER;
	e->slv = 0;
	e->path_angle = e->mark_angle = e->yaw;
	e->type = 0;
	e->speed_mul = e->speed_goal = 1.0f;
	size_upright(e, true);
	anim_set(e, A_WALK);
	float scale = 1.0f;
	switch (status) {
	case 1: scale = 1.2f; break;
	case 2: scale = 0.5f; break;
	case 3: case 4:
		if (status == 4) e->type = 1;
		e->mlv = NSE_DOWN;
		anim_set(e, A_LIE);
		set_size(e, 200.0f, 200.0f, true);
		scale = 0.3f;
		break;
	case 5:
		e->mlv = NSE_DYING;
		e->hp = 0.0f;
		anim_set(e, A_LIE);
		set_size(e, 200.0f, 200.0f, true);
		return;
	default: break;
	}
	if (tough)
		scale *= 1.2f;                // kind 0x20b
	set_hp(e, scale * vitality[s_diff], scale * endurance[s_diff]);
}

// ---------------------------------------------------------------- Creeper (kind 0x202, chr/tyu)
//
// PC en_tyu code (EnLOCAL kind 3): 0x4b0420 control main, 0x4b0370 automatic, 0x4ae1f0 init,
// states 0x4af6f0 wander, 0x4afa00 chase, 0x4afb90 go to a target, 0x4afd00 run off, 0x4aff00 /
// 0x4b00f0 walls and ceilings, 0x4ae360 bite, 0x4ae3e0 damage, 0x4ae730 dying, 0x4af450 event.
// The event creepers are left out; the nests' swarm spawning is under "creeper nests" below.
//
// Climbing (states 4 / 5). A creeper listed on a wall (status 1 or 3) holds on to the nearest wall
// segment at its height (the PC's wall probe 0x4ae7d0) and scuttles about on it (0x4aff00): its
// heading in the wall's plane turns away from what is ahead (0x4ad3a0 probes 500 ahead and turns
// by steps), it heads straight down when it is more than 2000 above James, and a status 3 one,
// within 100 of the floor, steps off onto it (100 out from the wall, facing away from it) and goes
// on as a floor creeper. A ceiling creeper (status 4, 0x4b00f0) runs about upside down at its
// height, turning now and then. A blow knocks either one off (state 7 sub 0xff): it falls at 1500
// units a second, lands on its back and is stunned as usual, a floor creeper from then on. The
// port's wall is one straight segment (the PC follows the room's mesh), so at the segment's ends
// and the wall's top it turns back.

static float tyu_speed(void)                   // 0x4ad9e0: units per second
{
	static const float t[5] = { 0.8f, 0.9f, 1.0f, 1.1f, 1.2f };
	return t[s_diff] * 1250.0f;
}

// Gap between the creeper's body and James' (the PC measures to the bodies, the port's p_dist is
// centre to centre).
static float tyu_gap(const Enemy* e) { return e->p_dist - 130.0f - e->size * 0.8f; }

// 0x4ad8b0: 2 James is right in front (bite), 1 close enough to chase, 0 neither. Creepers ignore
// James on beginner and in the dark without the flashlight.
static int tyu_sense(const Enemy* e, const EnPlayer* p)
{
	static const float chase[5] = { 0, 500, 600, 750, 1000 };
	if (s_diff == 0 || light_index(p) == 3 || !p->alive)
		return 0;
	float a = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	if (tyu_gap(e) < 150.0f && a < 1.0471976f)
		return 2;
	return tyu_gap(e) <= chase[s_diff] ? 1 : 0;
}

static void tyu_move(Enemy* e, float speed, float dt)
{
	approach_angle(&e->path_angle, e->mark_angle, 0.0872665f * dt * 60.0f);   // 5 degrees a frame
	e->yaw = e->path_angle;
	e->vx = sinf(e->yaw) * speed;
	e->vz = cosf(e->yaw) * speed;
	// skitters (0x4adb40): a random cockroach noise every third of a second or so
	e->sound_wait -= dt * 60.0f;
	if (e->sound_wait <= 0.0f) {
		e->sound_wait = rnd() * 20.0f;
		float pos[3] = { e->x, e->y, e->z };
		sfx_play_at(12200 + (int)(rnd() * 7.99f), 0.1f, pos);
	}
}

static void tyu_wander(Enemy* e, const EnPlayer* p, float dt)          // 0x4af6f0
{
	if (check_damage(e)) { go_raw(e, TYU_DAMAGE); return; }
	if (e->slv == 0) {
		anim_set(e, 1);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	if (rnd() < 0.003f * dt * 60.0f)
		e->path_angle = wrap(e->path_angle + rnd() * 1.0471976f);
	steer(e, e->x + sinf(e->path_angle) * 500.0f, e->z + cosf(e->path_angle) * 500.0f);
	tyu_move(e, tyu_speed(), dt);
	int s = tyu_sense(e, p);
	if (s == 2) go_raw(e, TYU_ATTACK);
	else if (s == 1) go_raw(e, TYU_CHASE);
}

static void tyu_chase(Enemy* e, const EnPlayer* p, float dt)           // 0x4afa00
{
	if (check_damage(e)) { go_raw(e, TYU_DAMAGE); return; }
	if (e->slv == 0) {
		anim_set(e, 1);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	steer(e, p->x, p->z);
	float off = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	tyu_move(e, tyu_speed() * ((PI - off) / PI + 1.0f) * 0.5f, dt);
	int s = tyu_sense(e, p);
	if (s == 2) go_raw(e, TYU_ATTACK);
	else if (s == 0) go_raw(e, TYU_WANDER);
	if (tyu_gap(e) < 0.0f)
		e->vx = e->vz = 0.0f;          // 0x4a5fb0: at James' feet, stop pushing
}

// 0x4afd00: after a bite it scuttles off sideways round James until it is out of chasing range.
static void tyu_flee(Enemy* e, const EnPlayer* p, float dt)
{
	if (check_damage(e)) { go_raw(e, TYU_DAMAGE); return; }
	if (e->slv == 0) {
		anim_set(e, 1);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	float a = dir_to(e->x, e->z, p->x, p->z) + PI / 2.0f;
	if (steer(e, e->x + sinf(a) * 500.0f, e->z + cosf(a) * 500.0f)) {
		go_raw(e, TYU_WANDER);          // cornered (path blocked within 250)
		return;
	}
	tyu_move(e, tyu_speed(), dt);
	if (tyu_sense(e, p) == 0)
		go_raw(e, TYU_WANDER);
}

// 0x4ae360: bites (attack 57: kind 3, ap 5, reach 300) for {60, 40, 30, 20, 10} ticks, then runs off.
static void tyu_attack(Enemy* e, const EnPlayer* p, float dt)
{
	static const s16 time[5] = { 60, 40, 30, 20, 10 };
	if (check_damage(e)) { e->attacking = false; go_raw(e, TYU_DAMAGE); return; }
	if (e->slv == 0) {
		set_timer(e, time[s_diff]);
		e->attacking = true;
		e->attack_hit = e->maria_hit = false;
		e->vx = e->vz = 0.0f;
		e->slv = 1;
	}
	blow_maria(e, 300.0f + e->size * 0.8f, PI, 5.0f);
	if (!e->attack_hit && p->alive && tyu_gap(e) < 300.0f) {
		e->attack_hit = true;
		hit_james(p, 57, 5.0f, e);
	}
	if (reduce_timer(e, (int)(dt * 60.0f + 0.5f)) <= 0) {
		e->attacking = false;
		go_raw(e, TYU_FLEE);
	}
}

// 0x4ae3e0: a hit stuns it ({60, 40, 30, 20, 10} ticks) or, once its endurance is gone, leaves it
// kicking on its back for {600, 500, 400, 300, 240}. It dies when the HP runs out.
static void tyu_stun(Enemy* e, float endurance)
{
	static const s16 stun[5] = { 60, 40, 30, 20, 10 };
	static const s16 over[5] = { 600, 500, 400, 300, 240 };
	set_timer(e, endurance > 0.0f ? stun[s_diff] : over[s_diff]);
	set_hitback(e);
}

// Nearest wall segment to (x, z) that spans height y, within 1500: the contact point, the inward
// normal and the segment's index (-1 if none).
static int nearest_wall(float x, float z, float y, float* cx, float* cz, float* nx, float* nz)
{
	int best = -1;
	float bd = 1500.0f;
	if (!s_col) return -1;
	for (u32 i = 0; i < s_col->n_walls; i++) {
		const WallSeg* w = &s_col->walls[i];
		if (y < w->y_top - 100.0f || y > w->y_bottom + 100.0f)
			continue;
		float vx = w->x1 - w->x0, vz = w->z1 - w->z0, l2 = vx * vx + vz * vz;
		if (l2 < 1.0f) continue;
		float t = ((x - w->x0) * vx + (z - w->z0) * vz) / l2;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		float px = w->x0 + vx * t, pz = w->z0 + vz * t, d = hypotf(x - px, z - pz);
		if (d < bd) {
			float l = sqrtf(l2), ux = -vz / l, uz = vx / l;
			if (ux * (x - px) + uz * (z - pz) < 0.0f) ux = -ux, uz = -uz;   // into the room
			bd = d, best = (int)i, *cx = px, *cz = pz, *nx = ux, *nz = uz;
		}
	}
	return best;
}

static float floor_below(float x, float z, float y)
{
	float fy;
	if (s_col && col_floor(s_col, x, z, y + 2500.0f, 3000.0f, &fy)) return fy;
	return y;
}

// Knocked off a wall or the ceiling (0x4ae3e0 sub 0xff / 0xfe): it drops at 1500 a second.
static void tyu_fall(Enemy* e)
{
	go_raw(e, TYU_DAMAGE);
	e->slv = 10;
	e->ival = -1;
	e->hb = 0.0f;
}

static void tyu_on_wall(Enemy* e, const EnPlayer* p, float dt)          // 0x4aff00
{
	e->vx = e->vz = 0.0f, e->hb = 0.0f;
	if (check_damage(e)) { tyu_fall(e); return; }
	if (!s_col || e->ival < 0 || e->ival >= (int)s_col->n_walls) { tyu_fall(e); return; }
	const WallSeg* w = &s_col->walls[e->ival];
	float a = e->aux[0], nx = sinf(a), nz = cosf(a), tx = nz, tz = -nx;
	float len = hypotf(w->x1 - w->x0, w->z1 - w->z0), ux = (w->x1 - w->x0) / len, uz = (w->z1 - w->z0) / len;
	float sx = (e->x - nx * 20.0f - w->x0) * ux + (e->z - nz * 20.0f - w->z0) * uz;   // along the segment
	float floor_y = floor_below(e->x + nx * 100.0f, e->z + nz * 100.0f, w->y_bottom);
	if (e->slv == 0) { anim_set(e, 1); e->aux[2] = 0.0f; e->slv = 1; }
	// the heading turns now and then; straight down when James is far below or it is a climber down
	e->aux[2] -= dt;
	if (e->aux[2] <= 0.0f) { e->aux[1] += (rnd() - 0.5f) * PI * 0.6f; e->aux[2] = 0.7f + rnd() * 1.5f; }
	if (e->type == 3 || e->y < p->y - 2000.0f) approach_angle(&e->aux[1], 0.0f, 0.0873f * dt * 60.0f);
	float h = e->aux[1], v = tyu_speed();
	float dir = tx * ux + tz * uz >= 0.0f ? 1.0f : -1.0f;      // the heading's side against the segment
	float ns = sx + sinf(h) * v * dt * dir, ny = e->y + cosf(h) * v * dt;
	if (ns < 100.0f || ns > len - 100.0f) { e->aux[1] = -h; ns = sx; }          // the segment's end
	if (ny < w->y_top + 150.0f) { e->aux[1] = PI - h; ny = e->y; }               // the top of the wall
	if (ny > floor_y - 100.0f) {
		if (e->type == 3) {
			// steps off onto the floor, facing away from the wall
			e->x += nx * 80.0f, e->z += nz * 80.0f, e->y = floor_y;
			e->yaw = e->path_angle = e->mark_angle = a;
			e->type = 2;
			go_raw(e, TYU_WANDER);
			return;
		}
		e->aux[1] = PI - h;
		ny = e->y;
	}
	e->x = w->x0 + ux * ns + nx * 20.0f, e->z = w->z0 + uz * ns + nz * 20.0f, e->y = ny;
	e->aux[1] = wrap(e->aux[1]);
	e->yaw = a;
	e->rate = 1.0f;
}

static void tyu_on_ceiling(Enemy* e, const EnPlayer* p, float dt)       // 0x4b00f0
{
	(void)p;
	e->vx = e->vz = 0.0f, e->hb = 0.0f;
	if (check_damage(e)) { tyu_fall(e); return; }
	if (e->slv == 0) { anim_set(e, 1); e->slv = 1; }
	// a new heading now and then (0.003 a tick, up to about 60 degrees)
	if (rnd() < 0.003f * dt * 60.0f) e->yaw = wrap(e->yaw + (rnd() - 0.5f) * 2.0f);
	float v = tyu_speed();
	float nx = e->x + sinf(e->yaw) * v * dt, nz = e->z + cosf(e->yaw) * v * dt, fy;
	if (s_col) {
		col_push(s_col, &nx, &nz, e->y, 150.0f, e->size);
		if (!col_floor(s_col, nx, nz, e->y + 2500.0f, 3000.0f, &fy)) { e->yaw = wrap(e->yaw + PI); return; }
	}
	if (hypotf(nx - e->x, nz - e->z) < v * dt * 0.3f) e->yaw = wrap(e->yaw + PI * (0.5f + rnd()));
	e->x = nx, e->z = nz;
	e->rate = 1.0f;
}

static void tyu_damage(Enemy* e, const EnPlayer* p, float dt)
{
	(void)p;
	e->vx = e->vz = 0.0f;
	if (e->slv == 10) {
		// falling off a wall or the ceiling; on the floor it is stunned as after any blow
		float fy = floor_below(e->x, e->z, e->y);
		e->y += 1500.0f * dt;
		e->rate = 0.0f;
		if (e->y >= fy) {
			e->y = fy;
			e->type = 2;
			e->slv = 0;
		}
		return;
	}
	if (e->slv == 0) {
		tyu_stun(e, reduce_hp(e));
		e->rate = 0.0f;                 // 0x4a7b60: the legs stop
		if (is_dead(e)) { go_raw(e, TYU_DYING); return; }
		e->slv = 1;
		return;
	}
	if (check_damage(e)) {
		tyu_stun(e, reduce_hp(e));
		if (is_dead(e)) { go_raw(e, TYU_DYING); return; }
	}
	if (reduce_timer(e, (int)(dt * 60.0f + 0.5f)) > 0 || e->hb != 0.0f)
		return;
	e->rate = 1.0f;
	if (e->endurance > 0.0f) {
		go_raw(e, TYU_CHASE);
	} else {
		add_hp(e, 100.0f);              // 0x4a5d40 on the endurance: rights itself
		go_raw(e, TYU_FLEE);
	}
}

static void tyu_dying(Enemy* e)                                        // 0x4ae730
{
	if (e->slv == 0) {
		anim_set(e, 0);
		e->vx = e->vz = 0.0f;
		e->attacking = false;
		mark_killed(e);
		e->dead_flag = true;
		e->slv = 1;
	}
}

static void tyu_update(Enemy* e, const EnPlayer* p, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	if (e->ctrl != 1)
		return;
	switch (e->mlv) {
	case TYU_WANDER: case TYU_TARGET: case TYU_EVENT:
		tyu_wander(e, p, dt);
		break;
	case TYU_WALL: tyu_on_wall(e, p, dt); break;
	case TYU_CEILING: tyu_on_ceiling(e, p, dt); break;
	case TYU_CHASE: tyu_chase(e, p, dt); break;
	case TYU_FLEE: tyu_flee(e, p, dt); break;
	case TYU_ATTACK: tyu_attack(e, p, dt); break;
	case TYU_DAMAGE: tyu_damage(e, p, dt); break;
	case TYU_DYING: tyu_dying(e); break;
	}
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
}

// 0x4ae1f0 enTYUInitData: HP {1, 250, 400, 600, 1000} and endurance {1, 100, 150, 240, 300}.
static void tyu_init(Enemy* e, int status)
{
	static const float vitality[5] = { 1, 250, 400, 600, 1000 };
	static const float endurance[5] = { 1, 100, 150, 240, 300 };
	e->ctrl = 1;
	e->mlv = TYU_WANDER;
	e->slv = 0;
	e->path_angle = e->mark_angle = e->yaw;
	set_size(e, 100.0f, 150.0f, true);
	anim_set(e, 1);
	e->type = 2;
	switch (status) {
	case 5:
		e->mlv = TYU_DYING;
		e->hp = 0.0f;
		return;
	case 1: case 3: {
		// on a wall at its listed height (e->aux[0], set by spawn_list); a status 3 one that finds
		// no wall is a floor creeper (0x4aff00), a status 1 one too in the port
		float y = e->aux[0], cx, cz, nx, nz;
		int seg = nearest_wall(e->x, e->z, y, &cx, &cz, &nx, &nz);
		if (seg >= 0 && y < e->y - 150.0f) {
			e->type = status;
			e->ival = seg;
			e->x = cx + nx * 20.0f, e->z = cz + nz * 20.0f, e->y = y;
			e->aux[0] = atan2f(nx, nz);
			e->aux[1] = PI * (rnd() - 0.5f);
			e->mlv = TYU_WALL;
		}
		break;
	}
	case 4:
		if (e->aux[0] < e->y - 150.0f) {
			e->type = 4;
			e->y = e->aux[0];
			e->mlv = TYU_CEILING;
		}
		break;
	default: break;                     // 6 event: on the floor here
	}
	set_hp(e, vitality[s_diff], endurance[s_diff]);
}

// ---------------------------------------------------------------- Pyramid Head (kind 0x208, chr/red)
//
// PC en_red code (EnLOCAL kind 4): 0x4c0c60 control main, 0x4c0b00 automatic, 0x4bf4f0 event
// control, 0x4bf2d0 init, states 0x4bf620 wander, 0x4bf790 chase, 0x4bf980 go for another enemy,
// 0x4bfb80 stair paths, 0x4bff10 attack, 0x4c00a0 shove and grab, 0x4c0370 confused, 0x4c05e0
// leave, 0x4c0ac0 stand. He cannot be killed. The port has the three places where the stage
// programs need him and nothing else: standing behind the bars (Enemy_List, status 12), the fight
// in the apartment stairwell (status 6, spawned by EvProgApartBoss 0x596220 once its demo is over)
// and the walk to the roof fence in the hospital (status 15, EvProgTriheadEntry 0x58cea0). The
// demos stay with the demo system. The hospital chase uses kind 0x209 (chr/oni) and is not here.

static float red_turn(void)                    // 0x4bec90: radians per 1/60 s
{
	static const float t[5] = { 0.5f, 0.7f, 1.0f, 1.5f, 2.0f };
	return t[s_diff] * 0.05235988f;
}

// 0x4beb60: walk speed falls with his endurance (the knife gets heavy).
static float red_walk_rate(const Enemy* e)
{
	static const float t[5] = { 0.5f, 0.6f, 0.8f, 1.0f, 1.2f };
	float k = e->endurance_max > 0.0f ? e->endurance / e->endurance_max : 1.0f;
	return t[s_diff] * (k * 0.75f + 0.25f);
}

// 0x4beeb0: 0 James out of reach, 1 follow him, 2 swing (anime 3), 3 the long swing (anime 4, after
// three attacks), 4 a short swing to the side (anime 5), 5 shove him away (stairwell only).
static int red_sense(Enemy* e, const EnPlayer* p)
{
	static const float range[5] = { 1000, 1500, 2000, 2500, 4000 };
	float r = range[s_diff] * (light_index(p) == 3 ? 0.75f : 1.0f);
	if (!p->alive || e->p_dist > r || !line_of_sight(e, p))
		return 0;
	if (e->anim == 1 && e->frame - RED_ANIM[1].start < 20.0f)
		return 1;                       // not in the first steps of a walk
	if (!p->maria && combat_held())
		return 1;                       // 0x4a5fc0: James is busy
	float a = wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw), d = e->p_dist;
	if (e->type == 2 && d < 500.0f && fabsf(a) < 0.5235988f) return 5;
	if (e->cnt > 2 && d < 1500.0f && fabsf(a) < 0.1221730f) return 3;
	if (d < 900.0f && fabsf(a) < 0.1745329f) return 2;
	if (d < 500.0f && a < 0.5235988f && a > -0.6981317f) return 4;
	return 1;
}

// Attacks (sh2_attack_list; the window is the frame within the playing anime, 0x539510): 50 with
// anime 3, ap 40 in frames 21..27; 51 with anime 4 (the long swing), ap 1000 in 67..71; 52 with
// anime 5, ap 25 in 7..10; 53 with the shove, ap 10 in 8..10. Reach 600 / 600 / 250 / 250 from the
// blade; the port adds 400 for arm and blade and James' radius.
static void red_attack_hit(Enemy* e, const EnPlayer* p)
{
	static const struct { s16 id, f0, f1, reach; float ap; } ATK[4] = {
		{ 50, 21, 27, 600, 40.0f }, { 51, 67, 71, 600, 1000.0f }, { 52, 7, 10, 250, 25.0f }, { 53, 8, 10, 250, 10.0f },
	};
	if (!e->attacking || e->anim < 3 || e->anim > 6)
		return;
	int k = e->anim - 3;
	float f = e->frame - RED_ANIM[e->anim].start;
	if (f < ATK[k].f0 || f > ATK[k].f1 + 0.99f)
		return;
	blow_maria(e, ATK[k].reach + 400.0f, 1.2f, ATK[k].ap);
	if (e->attack_hit || !p->alive)
		return;
	float dx = p->x - e->x, dz = p->z - e->z;
	float a = fabsf(wrap(atan2f(dx, dz) - e->yaw));
	if (sqrtf(dx * dx + dz * dz) < ATK[k].reach + 400.0f + 130.0f && a < 1.2f) {
		e->attack_hit = true;
		play_at(e, 16107);              // the blade meets something (0x4be9d0, SE 0x3eeb)
		hit_james(p, ATK[k].id, ATK[k].ap, e);
	}
}

static void red_start_attack(Enemy* e, int a)
{
	anim_set(e, a);
	e->attacking = true;
	e->attack_hit = e->maria_hit = false;
	e->cnt++;
	e->mlv = RED_ATTACK;
	e->slv = 1;
}

// 0x4bfb80, the stairwell (type 2): James on the upper flight (z > -99300) makes him climb by the
// points (98850, -99400) and (99200, -99000) and then go straight at him; James back below -99400
// with him above brings him down by (98850, -99400) and (100000, -101000). He swings on the way
// (the long swing after three attacks or when he is up at z >= -99500).
static void red_stairs(Enemy* e, const EnPlayer* p, float dt)
{
	static const float P1[2] = { 98850, -99400 }, P2[2] = { 99200, -99000 }, P3[2] = { 100000, -101000 };
	if (e->slv != 4 && p->z < -99400.0f && e->z < -99400.0f) { go_raw(e, RED_CHASE); return; }
	const float* t = NULL;
	switch (e->slv) {
	case 0:
		anim_set(e, 1);
		e->path_angle = e->yaw;
		e->slv = 1;
		// fall through
	case 1: t = P1; break;
	case 2: t = P2; break;
	case 4:
		red_attack_hit(e, p);
		if (!e->anim_end)
			return;
		e->attacking = false;
		anim_set(e, 1);
		e->slv = !e->attack_hit ? 1 : e->z < -99300.0f ? 6 : 5;
		return;
	case 5: t = P1; break;
	case 6: t = P3; break;
	}
	float tx = t ? t[0] : p->x, tz = t ? t[1] : p->z;
	if (t && hypotf(tx - e->x, tz - e->z) < 50.0f) {
		if (e->slv == 6) { go_raw(e, RED_CHASE); return; }
		e->slv++;                       // 1 -> 2 -> 3 (straight at James), 5 -> 6
	}
	if (e->slv > 4 && e->p_dist < 500.0f) { go_raw(e, RED_CHASE); return; }
	e->mark_angle = dir_to(e->x, e->z, tx, tz);
	approach_angle(&e->path_angle, e->mark_angle, red_turn() * dt * 60.0f);
	e->yaw = e->path_angle;
	e->rate = red_walk_rate(e);
	float a = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	if (e->slv <= 3 && e->p_dist < 1500.0f && a < 0.1221730f && e->frame - RED_ANIM[1].start >= 20.0f &&
	    p->alive) {
		int an = (e->cnt > 2 || e->z >= -99500.0f) ? 4 : 3;
		anim_set(e, an);
		e->attacking = true;
		e->attack_hit = e->maria_hit = false;
		e->cnt++;
		e->slv = 4;
	}
}

static void red_chase(Enemy* e, const EnPlayer* p, float dt)       // 0x4bf790
{
	if (e->slv == 0) {
		anim_set(e, 1);
		e->path_angle = e->yaw;
		e->slv = 1;
	}
	if (e->type == 2 && s_stage == 12) {
		if (p->z > -99300.0f) { go_raw(e, RED_STAIRS); return; }
		if (e->z > -99400.0f && p->z < -99400.0f) { go_raw(e, RED_STAIRS); e->slv = 5; anim_set(e, 1); return; }
	}
	steer(e, p->x, p->z);
	approach_angle(&e->path_angle, e->mark_angle, red_turn() * dt * 60.0f);
	e->yaw = e->path_angle;
	e->rate = red_walk_rate(e);
	int s = red_sense(e, p);
	if (s == 5) {
		red_start_attack(e, 6);
		e->mlv = RED_GRAB;
	} else if (s > 1) {
		red_start_attack(e, s == 3 ? 4 : s == 4 ? 5 : 3);
	}
}

static void red_attack(Enemy* e, const EnPlayer* p, float dt)      // 0x4bff10
{
	red_attack_hit(e, p);
	// keeps turning to follow James (1 degree a frame in the long swing on hard)
	float t = e->anim == 4 && s_diff > 2 ? 0.0174533f : red_turn();
	approach_angle(&e->yaw, dir_to(e->x, e->z, p->x, p->z), t * dt * 60.0f);
	e->path_angle = e->yaw;
	if (!e->anim_end)
		return;
	e->attacking = false;
	int s = red_sense(e, p);
	if (s >= 2 && s != 5)
		red_start_attack(e, s == 3 ? 4 : s == 4 ? 5 : 3);
	else
		go_raw(e, RED_CHASE);
}

// 0x4c00a0: the shove (attack 53); if it lands he lifts James (anime 7) and holds him up (anime 8)
// with attack 54 (kind 6, ap 0.2 every 1/60 s) until James struggles free; the meter is
// {0.35, 0.35, 0.7, 1.4} s of frame time (James' table for 0x35 at 0x533d00).
static void red_grab(Enemy* e, const EnPlayer* p, float dt)
{
	static const float meter[5] = { 0.35f, 0.35f, 0.7f, 1.4f, 1.4f };
	switch (e->slv) {
	case 1:
		approach_angle(&e->yaw, dir_to(e->x, e->z, p->x, p->z), 0.1745329f * dt * 60.0f);
		e->path_angle = e->yaw;
		red_attack_hit(e, p);
		if (!e->anim_end)
			return;
		e->attacking = false;
		if (e->attack_hit && !p->maria && p->alive) {
			combat_grab_james(meter[s_diff]);
			anim_set(e, 7);
			e->slv = 2;
		} else {
			anim_set(e, 9);
			e->slv = 4;
		}
		return;
	case 2:
		if (!combat_held()) { anim_set(e, 9); e->slv = 4; return; }
		if (e->anim_end) { anim_set(e, 8); e->slv = 3; }
		return;
	case 3:
		if (!combat_held()) { anim_set(e, 9); e->slv = 4; return; }
		combat_hold_damage(0.2f, dt);
		return;
	default:
		if (e->anim_end) go_raw(e, RED_CHASE);
		return;
	}
}

// 0x4c05e0: the fight is over (time or HP ran out). Flag 144 sounds the siren (event 8, program 6),
// he walks down the stairs through the points at 0x893000 and flag 146 at the last one ends it
// (event 9, program 7, which removes him; the door out unlocks with it).
static void red_leave(Enemy* e, const EnPlayer* p, float dt)
{
	static const float PATH[6][2] = {
		{ 98850, -99400 }, { 98800, -99000 }, { 101136, -98992 }, { 101136, -99820 }, { 98800, -99800 },
		{ 98800, -98620 },
	};
	e->attacking = false;
	if (e->slv == 0) {
		ev_flag_set(144, true);
		anim_set(e, 0);
		set_timer(e, 300);
		e->slv = 1;
		return;
	}
	if (e->slv == 1) {
		approach_angle(&e->yaw, dir_to(e->x, e->z, p->x, p->z), red_turn() * dt * 60.0f);
		e->path_angle = e->yaw;
		if (reduce_timer(e, (int)(dt * 60.0f + 0.5f)) > 0)
			return;
		anim_set(e, 1);
		e->rate = 1.0f;
		e->life = 0.0f;
		e->slv = e->z < -99400.0f ? 4 : e->z >= -99150.0f ? 6 : 5;
		return;
	}
	const float* t = PATH[e->slv - 4];
	e->life += dt;
	if (hypotf(t[0] - e->x, t[1] - e->z) < 50.0f || e->life > 8.0f) {
		e->life = 0.0f;                 // (the port gives up on a point it cannot reach)
		if (++e->slv > 9) {
			ev_flag_set(146, true);
			e->used = false;
			return;
		}
		t = PATH[e->slv - 4];
	}
	e->mark_angle = dir_to(e->x, e->z, t[0], t[1]);
	approach_angle(&e->path_angle, e->mark_angle, red_turn() * dt * 60.0f);
	e->yaw = e->path_angle;
}

// 0x4bf4f0: event control. Type 0 stands behind the bars; type 1 walks to the roof fence and raises
// flag 202 there (event 2, program 3: the demo where he pushes James through it).
static void red_event(Enemy* e, float dt)
{
	if (e->type == 0) {
		if (e->anim != 10) anim_set(e, 10);
		return;
	}
	if (e->anim != 1) {
		anim_set(e, 1);
		e->path_angle = e->yaw;
	}
	approach_angle(&e->path_angle, dir_to(e->x, e->z, 17800.0f, -23200.0f), red_turn() * dt * 60.0f);
	e->yaw = e->path_angle;
	if (hypotf(17800.0f - e->x, -23200.0f - e->z) < 50.0f && !ev_flag(202)) {
		ev_flag_set(202, true);
		anim_set(e, 0);
	}
}

static void red_sounds(Enemy* e, float f0, float dt)
{
	if (e->anim == 1) {
		// footsteps and the knife dragging on the floor (0x4be9d0, SE 0x3ef1 / 0x3ee4..)
		float a = f0 - RED_ANIM[1].start, b = e->frame - RED_ANIM[1].start;
		if (b < a) b += 30.0f;
		if ((a < 2.0f && b >= 2.0f) || (a < 17.0f && b >= 17.0f)) {
			play_at(e, 16113);
			play_at(e, 16100 + (int)(rnd() * 5.99f));
		}
	}
	// a groan every five seconds at the most (0x4bed70)
	e->sound_wait += dt * 60.0f;
	if (e->sound_wait >= 300.0f && rnd() < dt * 0.2f) {
		play_at(e, 16118 + (int)(rnd() * 1.99f));
		e->sound_wait = 0.0f;
	}
}

static void red_update(Enemy* e, const EnPlayer* p, float dt)       // 0x4c0b00
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	float f0 = e->frame;
	if (e->ctrl == 4) {
		reset_damage(e);
		red_event(e, dt);
	} else {
		// type 2 takes damage on its HP (0x4bede0); others only lose endurance
		if (check_damage(e)) {
			if (e->type == 2) {
				reduce_hp(e);
				if (is_dead(e) && e->mlv != RED_LEAVE) go_raw(e, RED_LEAVE);
			} else {
				e->endurance -= e->damage;
				if (e->endurance < 0.0f) e->endurance = 0.0f;
			}
			reset_damage(e);
			e->hb = 0.0f;
		}
		switch (e->mlv) {
		case RED_ATTACK: red_attack(e, p, dt); break;
		case RED_GRAB: red_grab(e, p, dt); break;
		case RED_STAIRS: red_stairs(e, p, dt); break;
		case RED_LEAVE: red_leave(e, p, dt); break;
		default: red_chase(e, p, dt); break;
		}
		if (!e->used)
			return;
		// endurance comes back at 200 a second
		e->endurance += dt * 200.0f;
		if (e->endurance > e->endurance_max) e->endurance = e->endurance_max;
		// the stairwell fight lasts {20, 90, 180, 300, 600} seconds (+0xac)
		if (e->type == 2 && e->mlv != RED_LEAVE) {
			e->clock -= dt;
			if (e->clock <= 0.0f && !e->attacking)
				go_raw(e, RED_LEAVE);
		}
	}
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	red_sounds(e, f0, dt);
}

// 0x4bf2d0 enREDInitData
static void red_init(Enemy* e, int status)
{
	static const float vitality[5] = { 600, 2400, 4000, 5000, 7000 };
	static const float endurance[5] = { 200, 700, 1000, 1200, 1500 };
	static const s16 fight[5] = { 20, 90, 180, 300, 600 };
	e->ctrl = 1;
	e->mlv = RED_CHASE;
	e->slv = 0;
	e->path_angle = e->mark_angle = e->yaw;
	set_size(e, 150.0f, 900.0f, true);
	anim_set(e, 1);
	e->hp = e->hp_max = 1000000.0f;      // no random spread for this kind (0x4a5b50)
	e->endurance = e->endurance_max = endurance[s_diff];
	switch (status) {
	case 6:                             // the stairwell
		e->type = 2;
		e->hp = e->hp_max = vitality[s_diff];
		e->endurance = e->endurance_max = endurance[s_diff] * 0.3f;
		e->clock = fight[s_diff];
		break;
	case 12: e->ctrl = 4; e->type = 0; anim_set(e, 10); break;   // behind the bars
	case 15: e->ctrl = 4; e->type = 1; break;                    // the roof
	default: e->type = 0; break;
	}
}


// ---------------------------------------------------------------- shared by the bosses

static void face(Enemy* e, float tx, float tz, float rate, float dt)
{
	approach_angle(&e->yaw, dir_to(e->x, e->z, tx, tz), rate * dt * 60.0f);
	e->path_angle = e->yaw;
}

static void walk_to(Enemy* e, float tx, float tz, float rate, float dt)
{
	steer(e, tx, tz);
	approach_angle(&e->path_angle, e->mark_angle, rate * dt * 60.0f);
	e->yaw = e->path_angle;
}

static float frame_in(const Enemy* e) { return e->frame - anim_tab(e)[e->anim].start; }

// A blow that reaches out `reach` from the body within `arc`: James (when he is the target) and
// Maria. Returns true when James was hit.
static bool swing_hit(Enemy* e, const EnPlayer* p, int id, float ap, float reach, float arc)
{
	blow_maria(e, reach, arc, ap);
	if (e->attack_hit || !p->alive)
		return false;
	float dx = p->x - e->x, dz = p->z - e->z;
	if (sqrtf(dx * dx + dz * dz) < reach + 130.0f && fabsf(wrap(atan2f(dx, dz) - e->yaw)) < arc) {
		e->attack_hit = true;
		if (!p->maria) {
			hit_james(p, id, ap, e);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------- Abstract Daddy (kind 0x204, chr/pap)
//
// PC en_pap code (EnLOCAL kind 8): control 0x4b7e90, automatic 0x4b7df0, init 0x4b6390; states
// 0 Wander 0x4b65e0, 1 Straight 0x4b6980, 2 Chase 0x4b6d10, 3 Rush 0x4b6f00, 4 Attack 0x4b7160,
// 5 Damage 0x4b7460, 6 Confuse 0x4b7680, 7 Down 0x4b7750, 8 LastMove 0x4b5690, 9 dying 0x4a96c0.
// The fight (status 6, type 1) starts in Chase and never gives up; ordinary spawns (type 0, the
// labyrinth rows) start in Straight. EnPAPAnime 0x892c58 ({id, loop}; pap_anim at 0x6e087c).
static const ScuAnim PAP_ANIM[33] = {
	{ 0, 21, 0, 2048 },      // 0 5701
	{ 22, 49, 1, 2048 },     // 1 5702 walk (20.8 units a frame, feet 20 / 24)
	{ 0, 21, 1, 2048 },      // 2 5701 idle
	{ 70, 93, 1, 2048 },     // 3 5704 holding James after a lunge from the front
	{ 642, 665, 1, 2048 },   // 4 5727 holding him after a lunge from behind
	{ 114, 126, 0, 2048 },   // 5 5706 flinch (handgun)
	{ 127, 155, 0, 2048 },   // 6 5707 flinch (shotgun / rifle)
	{ 156, 184, 0, 2048 },   // 7 5708
	{ 185, 202, 0, 2048 },   // 8 5709 flinch (swing from the left)
	{ 203, 220, 0, 2048 },   // 9 5710 (from the right)
	{ 221, 235, 0, 2048 },   // 10 5711 (from above)
	{ 236, 264, 0, 2048 },   // 11 5712
	{ 265, 293, 0, 2048 },   // 12 5713
	{ 185, 202, 0, 2048 },   // 13 5709
	{ 294, 325, 0, 2048 },   // 14 5714 knocked down
	{ 326, 343, 0, 2048 },   // 15 5715
	{ 344, 364, 0, 2048 },   // 16 5716
	{ 365, 396, 0, 2048 },   // 17 5717
	{ 397, 428, 0, 2048 },   // 18 5718
	{ 429, 465, 0, 2048 },   // 19 5719
	{ 466, 494, 0, 2048 },   // 20 5720
	{ 495, 521, 0, 2048 },   // 21 5721
	{ 0, 21, 0, 2048 },      // 22
	{ 522, 554, 0, 2048 },   // 23 5722 gets up
	{ 0, 21, 1, 2048 },      // 24
	{ 555, 572, 1, 2048 },   // 25 5723 writhes
	{ 621, 621, 0, 0 },      // 26 5725 the body (last frame)
	{ 604, 621, 0, 2048 },   // 27 5725 dies
	{ 50, 69, 0, 2048 },     // 28 5703 lunge when James faces it
	{ 94, 113, 0, 2048 },    // 29 5705 recovers after a front lunge
	{ 573, 603, 0, 2048 },   // 30 5724 turns round
	{ 622, 641, 0, 2048 },   // 31 5726 lunge from behind
	{ 666, 685, 0, 2048 },   // 32 5728 recovers after it
};

static float pap_walk_rate(const Enemy* e)                     // 0x4b58e0
{
	static const float t[5] = { 0.7f, 0.8f, 1.0f, 1.2f, 1.5f };
	return t[s_diff] * (0.5f + 0.005f * hp_rate(e)) * (e->type == 1 ? 0.5f : 1.0f);
}

static float pap_turn(const EnPlayer* p)                       // 0x4b5950: radians per 1/60 s
{
	static const float t[5] = { 0.5f, 0.8f, 1.0f, 2.0f, 2.0f };
	return t[s_diff] * 0.0261799f * (light_index(p) == 3 ? 0.5f : 1.0f);
}

// 0x4b56d0: 2 lunge, 1 sees James, 0 not. A narrow look: 10 degrees either side, 2500 away.
static int pap_sense(Enemy* e, const EnPlayer* p)
{
	if (!p->alive || e->p_dist > 2500.0f || !line_of_sight(e, p))
		return 0;
	float a = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	bool busy = !p->maria && combat_held();
	if (!busy && a < 0.174533f && e->p_dist < 130.0f + 405.0f - (e->mlv == PAP_RUSH ? 100.0f : 0.0f))
		return 2;
	return a <= 0.174533f ? 1 : 0;
}

static void pap_recover(Enemy* e, float dt)                    // 0x4b5870
{
	static const float r[5] = { 0, 10, 30, 60, 100 };
	e->endurance = fminf(fminf(e->endurance + r[s_diff] * dt, e->endurance_max), e->hp);
}

// Sounds (0x4b5f50): a groan 18305..18312 at 0.4 every two seconds while it moves about;
// footsteps 18300 on walk frames 2 and 16.
static void pap_sounds(Enemy* e, float f0, float dt)
{
	if (e->anim == 1) {
		float a = f0 - PAP_ANIM[1].start, b = e->frame - PAP_ANIM[1].start;
		if (b < a) b += 28.0f;
		if ((a < 2.0f && b >= 2.0f) || (a < 16.0f && b >= 16.0f))
			play_at(e, 18300);
	}
	if (e->mlv <= PAP_ATTACK) {
		e->sound_wait += dt * 60.0f;
		if (e->sound_wait >= 120.0f) {
			e->sound_wait = 0.0f;
			float pos[3] = { e->x, e->y, e->z };
			sfx_play_at(18305 + (int)(rnd() * 7.99f), 0.4f, pos);
		}
	}
}

static void pap_damage_check(Enemy* e)
{
	if (check_damage(e))
		go_raw(e, PAP_DAMAGE);
}

static void pap_chase(Enemy* e, const EnPlayer* p, float dt)   // 0x4b6d10
{
	pap_damage_check(e);
	if (e->mlv != PAP_CHASE) return;
	if (e->slv == 0) { anim_set(e, 1); e->path_angle = e->yaw; e->slv = 1; }
	walk_to(e, p->x, p->z, pap_turn(p), dt);
	float off = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	e->rate = pap_walk_rate(e) * (((PI - off) / PI) + 1.0f) * 0.5f * (e->mlv == PAP_RUSH ? 1.25f : 1.0f);
	int s = pap_sense(e, p);
	if (s == 2) { go_raw(e, PAP_ATTACK); return; }
	if (e->type == 0 && s == 0 && e->p_dist > 2500.0f) go_raw(e, PAP_WANDER);
	pap_recover(e, dt);
}

// 0x4b6980 / 0x4b65e0: the labyrinth ones walk about until James comes into their narrow view.
static void pap_wander(Enemy* e, const EnPlayer* p, float dt)
{
	pap_damage_check(e);
	if (e->mlv != PAP_WANDER && e->mlv != PAP_STRAIGHT) return;
	if (e->slv == 0) {
		anim_set(e, 1);
		e->path_angle = e->yaw;
		e->tx = e->x + sinf(e->yaw) * 1500.0f, e->tz = e->z + cosf(e->yaw) * 1500.0f;
		e->slv = 1;
	}
	if (hypotf(e->tx - e->x, e->tz - e->z) < 750.0f || rnd() < 0.01f) {
		float a = e->yaw + (rnd() - 0.5f) * (PI / 2.0f);
		e->tx = e->x + sinf(a) * 1500.0f, e->tz = e->z + cosf(a) * 1500.0f;
	}
	if (steer(e, e->tx, e->tz)) {
		e->tx = e->x + sinf(e->mark_angle) * 1500.0f, e->tz = e->z + cosf(e->mark_angle) * 1500.0f;
	}
	approach_angle(&e->path_angle, e->mark_angle, pap_turn(p) * dt * 60.0f);
	e->yaw = e->path_angle;
	e->rate = pap_walk_rate(e);
	int s = pap_sense(e, p);
	if (s == 2) go_raw(e, PAP_ATTACK);
	else if (s == 1 || (e->mlv == PAP_WANDER && e->p_dist < 1000.0f)) go_raw(e, PAP_CHASE);
	pap_recover(e, dt);
}

// 0x4b6f00: after a handgun hit it charges for five seconds at 1.25 x speed, shrugging off up to
// eight handgun bullets at 0.4 x.
static void pap_rush(Enemy* e, const EnPlayer* p, float dt)
{
	if (e->slv == 0) {
		anim_set(e, 1);
		set_size(e, 200.0f, 650.0f, false);
		set_timer(e, 300);
		e->amb_n = 0;
		e->slv = 1;
	}
	if (check_damage(e)) {
		if ((e->last_atk == 1 || e->last_atk == 2) && e->amb_n < 8 && e->hp > e->damage) {
			e->damage *= 0.4f;
			reduce_hp(e);
			e->amb_n++;
		} else {
			go_raw(e, PAP_DAMAGE);
			return;
		}
	}
	walk_to(e, p->x, p->z, pap_turn(p), dt);
	e->rate = pap_walk_rate(e) * 1.25f;
	if (pap_sense(e, p) == 2) { e->flags |= 0x40; go_raw(e, PAP_ATTACK); return; }
	if (reduce_timer(e, (int)(dt * 60.0f + 0.5f)) <= 0) {
		set_size(e, 400.0f, 650.0f, false);
		go_raw(e, PAP_CHASE);
	}
	pap_recover(e, dt);
}

// 0x4b7160: the lunge (attack 63: kind 2, ap 10, reach 400, frames 0..19). If it catches James it
// slides over him and holds him (attack 64, kind 6, ap 0.2 every 1/60 s) until he struggles free;
// his meter is {0.5, 0.5, 1.0, 2.0} s of frame time (0x533d00 for 0x3f). Then it backs off.
static void pap_attack(Enemy* e, const EnPlayer* p, float dt)
{
	static const float meter[5] = { 0.5f, 0.5f, 1.0f, 2.0f, 2.0f };
	if (e->slv < 2 || !combat_held()) {
		if (!(e->flags & 0x40) && check_damage(e)) {
			e->vx = e->vz = 0.0f;
			if (e->slv >= 2) combat_grab_james(0.0f);
			go_raw(e, PAP_DAMAGE);
			return;
		}
	} else {
		reset_damage(e);                // hits are lost while it holds James
	}
	switch (e->slv) {
	case 0: {
		// from behind when James has his back to it (0x4a6250)
		bool behind = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - p->yaw)) < PI / 2.0f;
		anim_set(e, behind ? 31 : 28);
		e->ival = behind;
		set_size(e, 150.0f, 650.0f, false);
		e->attacking = true;
		e->attack_hit = e->maria_hit = false;
		e->slv = 1;
		break;
	}
	case 1:
		face(e, p->x, p->z, 0.0261799f, dt);
		if (!e->attack_hit && frame_in(e) <= 19.0f && swing_hit(e, p, 63, 10.0f, 400.0f, 0.6f))
			combat_grab_james(meter[s_diff]);
		if (e->attack_hit && combat_held()) {
			// slides onto James (velocity 4 x the gap to a point 150 in front of him)
			float a = dir_to(e->x, e->z, p->x, p->z);
			e->vx = (p->x + sinf(a) * 150.0f - e->x) * 4.0f;
			e->vz = (p->z + cosf(a) * 150.0f - e->z) * 4.0f;
		}
		if (!e->anim_end)
			break;
		e->attacking = false;
		e->vx = e->vz = 0.0f;
		if (e->attack_hit && combat_held()) {
			anim_set(e, e->ival ? 4 : 3);
			e->slv = 2;
		} else {
			anim_set(e, e->ival ? 32 : 29);
			e->slv = 4;
		}
		break;
	case 2:
		combat_hold_damage(0.2f, dt);
		if (!combat_held()) {
			anim_set(e, e->ival ? 32 : 29);
			e->slv = 3;
		}
		break;
	case 3:
		e->vx = -sinf(e->yaw) * 500.0f, e->vz = -cosf(e->yaw) * 500.0f;   // backs off
		// fall through
	case 4:
		if (e->anim_end) {
			e->vx = e->vz = 0.0f;
			set_size(e, 400.0f, 650.0f, false);
			e->flags &= ~0x40u;
			go_raw(e, e->type == 1 ? PAP_CHASE : PAP_WANDER);
		}
		break;
	}
}

// 0x4b7460: no flinch while its endurance is full; otherwise a flinch, or a knock-down when the
// endurance is gone (or by a roll under 100). Health gone in the fight: the last move.
static void pap_damage(Enemy* e, const EnPlayer* p, float dt)
{
	(void)p, (void)dt;
	e->vx = e->vz = 0.0f;
	if (e->slv == 0) {
		if (e->endurance >= e->endurance_max && !e->attacking) {
			reduce_hp(e);
			if (is_dead(e) && e->type == 1) { go_raw(e, PAP_LAST); return; }
			set_timer(e, 10);
			e->slv = 3;
			return;
		}
		e->attacking = false;
		float side = blow_side(e);
		float en = reduce_hp(e);
		int a;
		if (en <= 0.0f || (en < 100.0f && rnd() > 0.8f + 0.002f * en)) {
			a = down_anim(e, side);
			if (e->type == 1 && is_dead(e)) {
				anim_set(e, a);
				go_raw(e, PAP_LAST);
				return;
			}
		} else {
			a = flinch_anim(e, side);
		}
		anim_set(e, a);
		if (a >= 14 && a <= 21) {
			e->flags |= 0x10000;
			set_size(e, 300.0f, 250.0f, false);
			e->slv = 2;
		} else {
			e->slv = 1;
		}
		return;
	}
	if (e->slv == 3) {
		if (check_damage(e)) { e->slv = 0; return; }
		if (reduce_timer(e, (int)(dt * 60.0f + 0.5f)) <= 0) go_raw(e, PAP_CHASE);
		return;
	}
	if (e->slv == 1) {
		if (check_damage(e)) { e->slv = 0; return; }
		if (!e->anim_end) return;
		go_raw(e, (e->type == 1 || s_diff >= 2) && (e->last_atk == 1 || e->last_atk == 2) ? PAP_RUSH : PAP_CHASE);
		return;
	}
	if (e->anim_end) {                  // slv 2: knocked down
		e->flags &= ~0x10000u;
		go_raw(e, PAP_DOWN);
	}
}

// 0x4b7750: the boss (type 1) gets straight back up (health + 100); the others lie a while,
// writhing, and can be stomped.
static void pap_down(Enemy* e, const EnPlayer* p, float dt)
{
	int ticks = (int)(dt * 60.0f + 0.5f);
	switch (e->slv) {
	case 0:
		reset_damage(e);
		anim_set(e, 25);
		if (is_dead(e)) {
			if (e->type == 1) { go_raw(e, PAP_LAST); return; }
			anim_set(e, 27);
			go_raw(e, PAP_DYING);
			return;
		}
		set_timer(e, e->type == 1 ? 0 : (int[5]){ 900, 480, 360, 300, 240 }[s_diff]);
		e->slv = 1;
		return;
	case 1:
		if (check_damage(e)) {
			if (e->last_atk >= 31 && e->last_atk <= 40 && e->type == 0) e->damage = e->hp;   // a stomp kills
			reduce_hp(e);
			if (is_dead(e)) { anim_set(e, 27); go_raw(e, e->type == 1 ? PAP_LAST : PAP_DYING); return; }
			set_timer(e, 60);
			return;
		}
		if (reduce_timer(e, ticks) > 0 || (p->stomping && e->type == 0))
			return;
		anim_set(e, 23);
		e->flags |= 0x10000;
		set_size(e, 400.0f, 650.0f, false);
		add_hp(e, 100.0f);
		e->slv = 2;
		return;
	case 2:
		if (!e->anim_end) return;
		e->flags &= ~0x10000u;
		go_raw(e, PAP_CHASE);
		return;
	}
}

// 0x4b5690 LastMove: falls (at 1/8 speed), then flag 334 runs the after-fight program (event 4,
// program 4 0x581090) and the body is the Enemy_List row with condition 0x240.
static void pap_last(Enemy* e)
{
	reset_damage(e);
	e->vx = e->vz = 0.0f;
	if (e->slv == 0) {
		e->rate = 0.125f;
		e->slv = 1;
	}
	if (frame_in(e) > 9.0f || e->anim_end) {
		ev_flag_set(334, true);
		go_raw(e, PAP_DYING);
	}
}

static void pap_update(Enemy* e, const EnPlayer* p, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	float f0 = e->frame;
	switch (e->mlv) {
	case PAP_WANDER: case PAP_STRAIGHT: pap_wander(e, p, dt); break;
	case PAP_CHASE: pap_chase(e, p, dt); break;
	case PAP_RUSH: pap_rush(e, p, dt); break;
	case PAP_ATTACK: pap_attack(e, p, dt); break;
	case PAP_DAMAGE: pap_damage(e, p, dt); break;
	case PAP_DOWN: pap_down(e, p, dt); break;
	case PAP_LAST: pap_last(e); break;
	case PAP_DYING:
		if (e->slv == 0) {
			e->vx = e->vz = 0.0f;
			if (e->type == 0) mark_killed(e);
			e->dead_flag = true;
			e->slv = 1;
		}
		break;
	}
	if (e->rate == 0.0f && e->mlv != PAP_DAMAGE) e->rate = 1.0f;
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	pap_sounds(e, f0, dt);
}

// 0x4b6390: the fight (status 6) has health {900, 2500, 5000, 7000} and endurance {300, 400, 600,
// 800}; the others {500, 750, 1000, 2000} / {150, 285, 380, 500}. No random spread for this kind.
static void pap_init(Enemy* e, int status)
{
	static const float hp6[5] = { 900, 2500, 5000, 7000, 10000 }, en6[5] = { 300, 400, 600, 800, 1000 };
	static const float hp0[5] = { 500, 750, 1000, 2000, 4000 }, en0[5] = { 150, 285, 380, 500, 600 };
	e->ctrl = 1;
	e->path_angle = e->mark_angle = e->yaw;
	set_size(e, 400.0f, 650.0f, true);
	anim_set(e, 2);
	if (status == 13 || status == 5) {
		anim_set(e, status == 13 ? 26 : 27);
		e->mlv = PAP_DYING;
		e->hp = 0.0f;
		set_size(e, 300.0f, 250.0f, true);
		e->dead_flag = true;
		e->slv = 1;
		return;
	}
	bool fight = status == 6;
	e->type = fight ? 1 : 0;
	e->mlv = fight ? PAP_CHASE : PAP_STRAIGHT;
	e->hp = e->hp_max = (fight ? hp6 : hp0)[s_diff];
	e->endurance = e->endurance_max = (fight ? en6 : en0)[s_diff];
}

// ---------------------------------------------------------------- the other Pyramid Head (kind 0x209, chr/oni)
//
// PC en_oni code (EnLOCAL kind 5): control 0x4be7f0 (1 automatic 0x4be550, 4 event 0x4be740),
// init 0x4bbe50. Four uses: the hospital chase (status 15, event control: a route of 14 points,
// James or Maria, whoever is nearer, as the target), the two in the hotel (status 6, type 2, the
// only ones that can be beaten), the labyrinth wanderers (status 0, invulnerable) and the hotel
// bodies (status 13). EnONIAnime 0x892e58 (oni_anim at 0x6b22bc). The spear is part of the model.
static const ScuAnim ONI_ANIM[16] = {
	{ 0, 17, 1, 2048 },      // 0 5401
	{ 18, 63, 1, 2048 },     // 1 5402 walk (9.6 units a frame, feet 12 / 13)
	{ 0, 17, 1, 2048 },      // 2 5401 idle
	{ 64, 102, 0, 2048 },    // 3 5403 thrust (attack 55)
	{ 236, 264, 1, 2048 },   // 4 5410 swings over and over (attack 56)
	{ 223, 235, 0, 2048 },   // 5 5409 into the swings
	{ 265, 277, 0, 2048 },   // 6 5411 out of them
	{ 103, 127, 0, 2048 },   // 7 5404 flinches (shot from behind)
	{ 128, 149, 0, 2048 },   // 8 5405 (from the front)
	{ 150, 171, 0, 2048 },   // 9 5406
	{ 172, 200, 0, 2048 },   // 10 5407
	{ 201, 222, 0, 2048 },   // 11 5408
	{ 278, 306, 0, 2048 },   // 12 5412
	{ 307, 335, 0, 2048 },   // 13 5413
	{ 336, 357, 0, 2048 },   // 14 5414
	{ 358, 359, 0, 0 },      // 15 5415 the body
};

static float oni_turn(void)
{
	static const float t[5] = { 0.5f, 0.7f, 1.0f, 1.2f, 1.5f };   // 0x4bbd50
	return t[s_diff] * 0.05236f;
}

static float oni_walk_rate(const Enemy* e)                     // 0x4bbc10
{
	static const float t[5] = { 0.6f, 0.8f, 1.0f, 1.0f, 1.0f };
	float k = e->endurance_max > 0.0f ? e->endurance / e->endurance_max : 1.0f;
	return (0.25f + 0.75f * k) * t[s_diff] * (e->type == 1 ? 1.5f : 1.0f);
}

static float oni_attack_rate(const Enemy* e)                   // 0x4bbc80
{
	static const float t[5] = { 0.75f, 0.8f, 1.0f, 1.0f, 1.0f };
	float k = e->endurance_max > 0.0f ? e->endurance / e->endurance_max : 1.0f;
	return (0.25f + 0.75f * k) * t[s_diff];
}

static Enemy* oni_partner(const Enemy* e);

// 0x4bc6b0: 2 James within 1250 and 10 degrees ahead, 1 aware of him, 0 not. The hotel pair take
// turns: neither may start an attack while the other is in its thrust (state 6 up to sub 1), its
// run of swings (7) or the thrust from a corner (3 sub 4); that is when the one waiting stands off.
static int oni_sense(const Enemy* e, const EnPlayer* p)
{
	static const float feel[5] = { 1000, 1500, 2000, 2500, 3000 };
	if (!p->alive) return 0;
	if (!line_of_sight(e, p)) return p->light ? 1 : 0;
	if (e->p_dist > feel[s_diff] * (light_index(p) == 3 ? 0.75f : 1.0f)) return 0;
	if (!p->maria && combat_held()) return 1;
	if (e->type == 2) {
		const Enemy* o = oni_partner(e);
		if (o && (o->mlv == ONI_THRUST || o->mlv == ONI_SWINGS)) return 1;
	}
	if (e->p_dist < 1250.0f && fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw)) < 0.1745f) return 2;
	return 1;
}

// Attack 55 (kind 3, ap 30, reach 700, frames 17..23) and 56 (kind 2, ap 50, reach 700, frames
// 0..28 of each swing loop). The oni do not hurt each other.
// The reach is 700 past the spear's skeleton node 20 (0x53c830); he thrusts from 1250, so the port
// counts 1150 from his body.
#define ONI_REACH 1150.0f

static void oni_attack_hit(Enemy* e, const EnPlayer* p)
{
	if (!e->attacking) return;
	float f = frame_in(e);
	if (e->anim == 3 && f >= 17.0f && f <= 23.99f) {
		if (swing_hit(e, p, 55, 30.0f, ONI_REACH, 0.8f))
			play_at(e, 16108);          // 0x3eec on contact
	} else if (e->anim == 4) {
		swing_hit(e, p, 56, 50.0f, ONI_REACH, 1.0f);
	}
}

static void oni_start_thrust(Enemy* e)
{
	anim_set(e, 3);
	e->rate = oni_attack_rate(e);
	e->attacking = true;
	e->attack_hit = e->maria_hit = false;
}

static void oni_sounds(Enemy* e, float f0)
{
	if (e->anim == 1) {
		float a = f0 - ONI_ANIM[1].start, b = e->frame - ONI_ANIM[1].start;
		if (b < a) b += 46.0f;
		if ((a < 4.0f && b >= 4.0f) || (a < 26.0f && b >= 26.0f))
			play_at(e, 16113);          // 0x3ef1 on keys 22 and 44
	} else if (e->anim == 4) {
		float a = f0 - ONI_ANIM[4].start, b = e->frame - ONI_ANIM[4].start;
		if (b < a) b += 29.0f;
		if ((a < 5.0f && b >= 5.0f) || (a < 20.0f && b >= 20.0f))
			play_at(e, 16109);          // 0x3eed, the swing
	}
}

// The hotel pair's damage (0x4bc420, type 2): x1.5 while it attacks or for the rifle, x5 for the
// great knife's heavy swings (17 / 18); what goes past 0 health is taken from the other one at
// x0.75. A spent endurance makes it flinch except for the handgun and the plank.
static Enemy* oni_partner(const Enemy* e)
{
	if (e->ival2 < 0 || e->ival2 >= ENEMY_MAX) return NULL;
	Enemy* o = &s_en[e->ival2];
	return o->used && o->kind == EN_KIND_ONI ? o : NULL;
}

static void oni_take_damage(Enemy* e)
{
	static const s16 stun[5] = { 180, 90, 60, 40, 30 };
	int id = e->last_atk;
	if (!(e->type == 2 && (id == 1 || id == 2 || id == 12 || id == 13)))
		e->timer2 = stun[s_diff];
	if (e->type != 2) {
		e->endurance = fmaxf(e->endurance - e->damage, 0.0f);
		reset_damage(e);
		return;
	}
	float d = e->damage * (e->attacking ? 1.5f : 1.0f) * (id == 6 ? 1.5f : 1.0f) * (id == 17 || id == 18 ? 5.0f : 1.0f);
	if (id == 14 || id == 17) e->endurance = 0.0f;
	e->hp -= d;
	if (e->hp < 0.0f) {
		Enemy* o = oni_partner(e);
		if (o) o->hp = fmaxf(o->hp + e->hp * (id >= 19 && id <= 22 ? 0.5f : 1.0f) * 0.75f, 0.0f);
		e->hp = 0.0f;
	}
	e->endurance -= d;
	reset_damage(e);
	if (e->endurance <= 0.0f && !(id == 1 || id == 2 || id == 12 || id == 13) && (e->mlv < ONI_FLINCH || e->mlv >= ONI_FLANK)) {
		e->attacking = false;
		go_raw(e, ONI_FLINCH);
	}
}

static int oni_flinch_anim(const Enemy* e)                     // 0x4bb910
{
	float side = blow_side(e);
	switch (e->last_atk) {
	case 4: case 6: return fabsf(side) <= PI / 2.0f ? 8 : 7;
	case 14: case 17: return 9;
	case 18: return fabsf(side) <= PI / 2.0f ? 11 : 10;
	case 19: case 20: return side < 0.0f ? 12 : 13;
	case 21: case 22: return 14;
	default: return 2;
	}
}

// The suicide spots in the hotel (0x892f7c / 0x892f8c): when both are beaten (or the clock ran
// out on beginner..normal) they walk there, turn, and flag 483 runs the demo (event 8, program 2).
static const float ONI_SPOT[2][3] = { { -19344.45f, 21723.15f, -0.5236f }, { -19622.80f, 22746.35f, 0.7854f } };

// The hotel pair's teamwork (type 2, room ru25). Once a second (0x4bcff0) the one farther from
// James, while the other chases or attacks, either flanks (state 2, 0x4bd230) or, with James in a
// corner of the room, guards the way out of it (state 3, 0x4bd780). The corner test (0x4bba10) is
// James outside x -21800..-18200 and outside z 19800..24200.
static bool oni_cornered(const EnPlayer* p)
{
	return (p->z < 19800.0f || p->z > 24200.0f) && (p->x < -21800.0f || p->x > -18200.0f);
}

static void oni_attack_or(Enemy* e, const Enemy* o, int s)
{
	// 0x4bd230: in range, the thrust, or the run of swings when the partner is more than 750 away
	if (s != 2) return;
	if (o && hypotf(o->x - e->x, o->z - e->z) > 750.0f) { go_raw(e, ONI_SWINGS); return; }
	oni_start_thrust(e);
	go_raw(e, ONI_THRUST);
	e->slv = 1;
}

// Flanking: the point at James' far side from the partner, 40 degrees off that line (the side is
// the sign of its own bearing less the partner's, chosen again every second), as far from him as
// it is now (at most 2500); once it is more
// than 100 degrees round from the partner it closes in to 500 behind him and waits there facing
// him. Back to the chase beyond 4000, or when it is 150 nearer to James than the partner.
static void oni_flank(Enemy* e, const EnPlayer* p, float dt)
{
	Enemy* o = oni_partner(e);
	if (!o) { go_raw(e, ONI_CHASE); return; }
	if (oni_cornered(p)) { go_raw(e, ONI_CORNER); return; }
	oni_attack_or(e, o, oni_sense(e, p));
	if (e->mlv != ONI_FLANK) return;
	if (e->p_dist > 4000.0f || e->p_dist < o->p_dist - 150.0f) { go_raw(e, ONI_CHASE); return; }
	float own = dir_to(e->x, e->z, p->x, p->z), part = dir_to(o->x, o->z, p->x, p->z);
	float apart = wrap(own - part);
	if (e->slv == 0) { anim_set(e, 1); e->aux[0] = 0.0f; e->slv = 1; }
	if (e->slv == 1) {
		e->aux[0] -= dt;
		if (e->aux[0] <= 0.0f) { e->aux[1] = apart < 0.0f ? -1.0f : 1.0f; e->aux[0] = 1.0f; }
		float r = fminf(e->p_dist, 2500.0f), a = part - e->aux[1] * 0.6981f;
		float tx = p->x + sinf(a) * r, tz = p->z + cosf(a) * r;
		walk_to(e, tx, tz, oni_turn(), dt);
		e->rate = oni_walk_rate(e);
		if (fabsf(apart) > 1.7453f) e->slv = 2;
		if (hypotf(tx - e->x, tz - e->z) < 250.0f || e->p_dist < 1000.0f) { anim_set(e, 2); e->slv = 3; }
	} else if (e->slv == 2) {
		float tx = p->x + sinf(part) * 500.0f, tz = p->z + cosf(part) * 500.0f;
		if (e->anim != 1) anim_set(e, 1);
		walk_to(e, tx, tz, oni_turn(), dt);
		e->rate = oni_walk_rate(e);
		if (e->p_dist < 500.0f) { anim_set(e, 2); e->slv = 3; }
	} else {
		face(e, p->x, p->z, oni_turn(), dt);
		if (e->p_dist > 2500.0f) { anim_set(e, 1); e->slv = 2; }
		else if (e->p_dist > 500.0f && fabsf(apart) <= 1.7453f) { anim_set(e, 1); e->slv = 1; }
	}
}

// Guarding a corner: the corner James is in (x -17200 or -22800, z 25200 or 18800), and of its two
// walls the one on the other side from the partner; it walks to the spot 1000 out along that wall
// and waits there facing him, and thrusts once he is within 1250 (normal and up). Back to the chase
// when he is out of the corners and more than 1250 away.
static void oni_corner(Enemy* e, const EnPlayer* p, float dt)
{
	Enemy* o = oni_partner(e);
	if (!oni_cornered(p) && e->p_dist > 1250.0f) { go_raw(e, ONI_CHASE); return; }
	bool partner_busy = o && (o->mlv == ONI_THRUST || o->mlv == ONI_SWINGS);
	if (e->p_dist < 1250.0f && s_diff > 1 && !partner_busy) {
		oni_start_thrust(e);
		go_raw(e, ONI_THRUST);
		e->slv = 1;
		return;
	}
	oni_attack_or(e, o, oni_sense(e, p));
	if (e->mlv != ONI_CORNER) return;
	bool hz = p->z >= 22000.0f, hx = p->x >= -20000.0f;
	float cx = hx ? -17200.0f : -22800.0f, cz = hz ? 25200.0f : 18800.0f;
	int k = hx ? 3 - hz : hz;
	float base = k * (PI / 2.0f);
	if (o && wrap(dir_to(o->x, o->z, p->x, p->z) - base - PI / 4.0f) > 0.0f) base += PI / 2.0f;
	float tx = cx + sinf(base) * 1000.0f, tz = cz + cosf(base) * 1000.0f;
	if (e->slv == 0) { anim_set(e, 1); e->slv = 1; }
	if (e->slv == 1) {
		walk_to(e, tx, tz, oni_turn(), dt);
		e->rate = oni_walk_rate(e);
		if (hypotf(tx - e->x, tz - e->z) < 150.0f) { anim_set(e, 2); e->slv = 2; }
	} else {
		face(e, p->x, p->z, oni_turn(), dt);
		if (hypotf(tx - e->x, tz - e->z) > 400.0f) { anim_set(e, 1); e->slv = 1; }
	}
}

// Standing off (state 4, 0x4bdb50), after a thrust that left James in reach: it attacks again
// while he stays in reach, holds its ground facing him while it is more than 100 degrees round
// from its partner (so James stays between them), and otherwise goes back to flanking, or to the
// corner when he is in one. Beyond 2500 it gives chase.
static void oni_standoff(Enemy* e, const EnPlayer* p, float dt)
{
	Enemy* o = oni_partner(e);
	if (!o) { go_raw(e, ONI_CHASE); return; }
	oni_attack_or(e, o, oni_sense(e, p));
	if (e->mlv != ONI_STANDOFF) return;
	if (e->p_dist > 2500.0f) { go_raw(e, ONI_CHASE); return; }
	float apart = wrap(dir_to(e->x, e->z, p->x, p->z) - dir_to(o->x, o->z, p->x, p->z));
	if (fabsf(apart) >= 1.7453f) {
		if (e->slv == 0) { anim_set(e, 2); e->slv = 1; }
		face(e, p->x, p->z, oni_turn(), dt);
		return;
	}
	go_raw(e, oni_cornered(p) ? ONI_CORNER : ONI_FLANK);
}

static void oni_automatic(Enemy* e, const EnPlayer* p, float dt)
{
	if (e->mlv != ONI_SUICIDE && check_damage(e)) oni_take_damage(e);
	switch (e->mlv) {
	case ONI_WANDER:
	case ONI_CHASE: {
		if (e->slv == 0) { anim_set(e, 1); e->path_angle = e->yaw; e->slv = 1; }
		int s = oni_sense(e, p);
		if (e->mlv == ONI_WANDER) {
			if (hypotf(e->tx - e->x, e->tz - e->z) < 600.0f || e->tx == 0.0f) {
				float a = e->yaw + (rnd() - 0.5f) * (PI / 2.0f);
				e->tx = e->x + sinf(a) * 1500.0f, e->tz = e->z + cosf(a) * 1500.0f;
			}
			walk_to(e, e->tx, e->tz, oni_turn(), dt);
			e->rate = oni_walk_rate(e);
			if (s == 1) go_raw(e, ONI_CHASE);
		} else {
			walk_to(e, p->x, p->z, oni_turn(), dt);
			float off = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
			e->rate = oni_walk_rate(e) * ((PI - off) / PI + 1.0f) * 0.5f;
			if (e->type == 2 && s_diff > 1 && off < 0.0873f) e->rate *= 1.5f;
			if (s == 0 && e->type != 2) go_raw(e, ONI_WANDER);
			if (e->type == 2 && s != 2) {
				e->aux[2] -= dt;
				Enemy* o = oni_partner(e);
				if (e->aux[2] <= 0.0f && o && o->p_dist < e->p_dist &&
				    (o->mlv == ONI_CHASE || o->mlv == ONI_THRUST || o->mlv == ONI_SWINGS)) {
					go_raw(e, oni_cornered(p) ? ONI_CORNER : ONI_FLANK);
					return;
				}
				if (e->aux[2] <= 0.0f) e->aux[2] = 1.0f;
			}
		}
		if (s == 2) {
			// the pair now and then goes into its run of swings when James is close (0x4be060)
			if (e->type == 2 && e->p_dist < 1100.0f && rnd() < 0.3f) { go_raw(e, ONI_SWINGS); return; }
			oni_start_thrust(e);
			go_raw(e, ONI_THRUST);
			e->slv = 1;
		}
		break;
	}
	case ONI_THRUST:
		oni_attack_hit(e, p);
		if (s_diff > 2 || e->p_dist < 500.0f) face(e, p->x, p->z, oni_turn(), dt);
		if (e->anim_end) {
			e->attacking = false;
			// 0x4bdea0: the hotel pair stand off after a thrust while James is still in reach
			if (e->type == 2 && oni_partner(e) && oni_sense(e, p) == 2) go_raw(e, ONI_STANDOFF);
			else go_raw(e, e->type == 0 ? ONI_WANDER : ONI_CHASE);
		}
		break;
	case ONI_STANDOFF:
		oni_standoff(e, p, dt);
		break;
	case ONI_SWINGS:
		if (e->slv == 0) {
			anim_set(e, 5);
			e->amb_n = (u8)(2 + (int)(rnd() * 3.99f));
			e->slv = 1;
		} else if (e->slv == 1 && e->anim_end) {
			anim_set(e, 4);
			e->loops = 0;
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			e->slv = 2;
		} else if (e->slv == 2) {
			int before = e->loops;
			oni_attack_hit(e, p);
			face(e, p->x, p->z, oni_turn(), dt);
			if (e->loops != before) e->attack_hit = e->maria_hit = false;   // rearmed every loop
			if (e->loops >= e->amb_n || (e->loops >= 1 && e->p_dist > 2500.0f)) {
				e->attacking = false;
				anim_set(e, 6);
				e->slv = 3;
			}
		} else if (e->slv == 3 && e->anim_end) {
			go_raw(e, ONI_CHASE);
		}
		break;
	case ONI_FLANK:
		oni_flank(e, p, dt);
		break;
	case ONI_CORNER:
		oni_corner(e, p, dt);
		break;
	case ONI_FLINCH:
		if (e->slv == 0) { anim_set(e, oni_flinch_anim(e)); e->slv = 1; }
		if (e->anim_end) {
			e->endurance = e->endurance_max;
			go_raw(e, ONI_CHASE);
		}
		break;
	case ONI_SUICIDE: {
		reset_damage(e);
		const float* s = ONI_SPOT[e->amb_n & 1];
		if (e->slv == 0) { anim_set(e, 1); e->slv = 1; }
		if (e->slv == 1) {
			walk_to(e, s[0], s[1], oni_turn(), dt);
			e->rate = 1.0f;
			if (hypotf(s[0] - e->x, s[1] - e->z) < 100.0f) { anim_set(e, 2); e->slv = 2; }
		} else if (e->slv == 2) {
			e->x += (s[0] - e->x) * fminf(dt, 1.0f), e->z += (s[1] - e->z) * fminf(dt, 1.0f);
			approach_angle(&e->yaw, s[2], oni_turn() * dt * 60.0f);
			if (fabsf(wrap(e->yaw - s[2])) < 0.01f) e->slv = 3;
		} else {
			Enemy* o = oni_partner(e);
			if (!o || (o->mlv == ONI_SUICIDE && o->slv == 3))
				ev_flag_set(483, true);
		}
		break;
	}
	}
	if (e->type == 2 && e->mlv != ONI_SUICIDE) {
		// the hotel clock (DAT_00951120): {60, 180, 300} s, none on hard
		if (s_diff < 3) {
			e->clock -= dt;
			if (e->clock <= 0.0f) e->hp = 0.0f;
		}
		Enemy* o = oni_partner(e);
		if (e->hp <= 0.0f && (!o || o->hp <= 0.0f) && e->mlv != ONI_FLINCH) {
			e->attacking = false;
			go_raw(e, ONI_SUICIDE);
			if (o && o->mlv != ONI_SUICIDE) { o->attacking = false; go_raw(o, ONI_SUICIDE); }
		}
	}
}

// 0x4be740 / 0x4bc930: the hospital chase. He walks the corridor route (0x892e98) at 1.5 x, going
// for whichever of James and Maria is nearer when one is within 750, then waits out a cool-down
// ({1800, 960, 540, 120} ticks) after a blow that lands. Behind James' back, or when a stage
// program has called him on (FUN_004a8750 (4, n) from programs 2..5, flags 244..247), he catches
// up at 1500 units a second. He cannot be hurt, only held up a moment.
static const float ONI_ROUTE[14][2] = {
	{ 16100, -260300 }, { 16000, -258800 }, { 16100, -258100 }, { 19200, -258000 }, { 19900, -257900 },
	{ 19900, -255700 }, { 19200, -255600 }, { 16100, -255500 }, { 16100, -254500 }, { 18300, -254300 },
	{ 18400, -252400 }, { 18500, -251600 }, { 19000, -251500 }, { 34000, -251600 },
};

static void oni_chase_event(Enemy* e, const EnPlayer* p, const EnPlayer* james, float dt)
{
	static const s16 cool[5] = { 1800, 960, 540, 120, 120 };
	static const u8 catchup_to[4] = { 1, 3, 6, 10 };
	int ticks = (int)(dt * 60.0f + 0.5f);
	if (check_damage(e)) {
		reset_damage(e);
		e->timer = (int)fminf(fmaxf((e->last_atk == 14 || e->last_atk == 17 ? 600 : e->timer) + 90, 180), 600);
	}
	if (e->timer > 0) e->timer = e->timer > ticks ? e->timer - ticks : 0;
	// the programs' calls, once each
	for (int n = 0; n < 4; n++)
		if (ev_flag(244 + n) && !(e->amb_n & (1 << n))) {
			e->amb_n |= (u8)(1 << n);
			if (e->ival <= catchup_to[n]) e->ival3 = n;
			e->attacking = false;
			e->slv = 0;
		}
	e->vx = e->vz = 0.0f;
	if (e->slv == 4) {                  // thrusting
		oni_attack_hit(e, p);
		face(e, p->x, p->z, oni_turn(), dt);
		if (e->anim_end) {
			e->attacking = false;
			if (e->attack_hit) e->timer = cool[s_diff];
			e->slv = 0;
		}
		return;
	}
	if (e->slv == 0) { anim_set(e, 1); e->path_angle = e->yaw; e->slv = 1; }
	if (e->ival < 14) {
		const float* w = ONI_ROUTE[e->ival];
		approach_angle(&e->path_angle, dir_to(e->x, e->z, w[0], w[1]), oni_turn() * dt * 60.0f);
		e->yaw = e->path_angle;
		if (hypotf(w[0] - e->x, w[1] - e->z) < 250.0f) {
			e->ival++;
			if (e->ival3 >= 0 && e->ival > catchup_to[e->ival3]) e->ival3 = -1;
		}
	}
	e->rate = oni_walk_rate(e);
	// James cannot see him: he is behind James (0x4a6950, guessed as that)
	bool unseen = fabsf(wrap(dir_to(james->x, james->z, e->x, e->z) - james->yaw)) > PI / 2.0f;
	if (e->ival3 >= 0 || (e->p_dist > 2500.0f && unseen) || (e->ival > 2 && unseen)) {
		e->vx = sinf(e->yaw) * 1500.0f;
		e->vz = cosf(e->yaw) * 1500.0f;
	}
	if (e->timer == 0 && e->p_dist < 750.0f && line_of_sight(e, p) && p->alive) {
		oni_start_thrust(e);
		if (s_diff == 2 && unseen) e->rate *= 2.0f;
		if (s_diff >= 3) e->rate *= unseen ? 3.0f : 1.5f;
		e->slv = 4;
	}
}

static void oni_update(Enemy* e, const EnPlayer* p, const EnPlayer* james, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	float f0 = e->frame;
	if (e->mlv == ONI_BODY) {
		reset_damage(e);
		return;
	}
	if (e->ctrl == 4) {
		oni_chase_event(e, p, james, dt);
	} else {
		if (e->timer2 > 0) e->timer2 -= (int)(dt * 60.0f + 0.5f);
		else e->endurance = fminf(e->endurance + dt * (s_diff < 2 ? 100.0f : 200.0f), e->endurance_max);
		oni_automatic(e, p, dt);
	}
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	oni_sounds(e, f0);
}

// 0x4bbe50: size 250 (350 for the chase), health 1e6 and invulnerable except the hotel pair
// ({1200, 5000, 9000, 20000}, endurance {300, 500, 1300, 1500}).
static void oni_init(Enemy* e, int status)
{
	static const float hp6[5] = { 1200, 5000, 9000, 20000, 30000 }, en[5] = { 300, 500, 1300, 1500, 2000 };
	static const float clock[5] = { 60, 180, 300, 3600, 3600 };
	e->ctrl = 1;
	e->path_angle = e->mark_angle = e->yaw;
	e->ival2 = e->ival3 = -1;
	set_size(e, 250.0f, 900.0f, true);
	anim_set(e, 2);
	e->hp = e->hp_max = 1e6f;
	e->endurance = e->endurance_max = en[s_diff];
	switch (status) {
	case 5: case 13:
		e->mlv = ONI_BODY;
		e->type = 4;
		anim_set(e, 15);
		e->hp = 1.0f;
		e->dead_flag = true;
		return;
	case 6:
		e->type = 2;
		e->mlv = ONI_CHASE;
		e->hp = e->hp_max = hp6[s_diff];
		e->clock = clock[s_diff];
		return;
	case 15:
		e->ctrl = 4;
		e->type = 1;
		e->mlv = ONI_WANDER;
		set_size(e, 350.0f, 900.0f, true);
		e->endurance = e->endurance_max = en[s_diff] * 0.5f;
		e->timer = 180;
		play_at(e, 16119);              // 0x3ef7
		return;
	default:
		e->mlv = ONI_WANDER;
		return;
	}
}

// ---------------------------------------------------------------- Mary, the last boss (kind 0x206, chr/bos)
//
// PC en_bos code (EnLOCAL kind 11): control 0x4b5540, automatic 0x4b5460, init 0x4b46f0; states
// 0 Approach 0x4b4c10, 1 Distance 0x4b4db0, 2 Generate 0x4b4ec0 (the moths), 3 Attack (grab)
// 0x4b5190, 4 Attack2 (swipe) 0x4b53b0, 5 Dead 0x4b4810. She hovers about 1500 above the floor
// (y -17500 in a room whose floor is -16005) and dips when James swings at her. EnBOSAnime
// 0x892c10 (bos_anim at 0x6dbd9c).
static const ScuAnim BOS_ANIM[10] = {
	{ 0, 58, 0, 2048 },      // 0 5801
	{ 0, 58, 1, 2048 },      // 1 5801 hovers
	{ 59, 73, 0, 2048 },     // 2 5802 lunges (attack 0x41)
	{ 74, 88, 0, 1536 },     // 3 5803 catches James
	{ 89, 112, 1, 2048 },    // 4 5804 chokes him (attack 0x42)
	{ 113, 136, 0, 2048 },   // 5 5805 lets go
	{ 137, 145, 0, 2048 },   // 6 5806 missed
	{ 173, 196, 0, 2048 },   // 7 5808 swipe (attack 0x43)
	{ 146, 172, 0, 2048 },   // 8 5807 flinches, and dies
	{ 197, 227, 1, 2048 },   // 9 5809 lying on the floor
};

static float bos_speed(void)                                   // 0x4b4590
{
	static const float t[5] = { 0.6f, 0.8f, 1.0f, 3.0f, 4.0f };
	return t[s_diff] * 500.0f;
}

// 0x4b4300: 2 grab (400..700 away, 5 degrees, James' back half turned), 1 swipe (within 600).
static int bos_can_attack(const Enemy* e, const EnPlayer* p)
{
	if (p->maria || combat_held() || !p->alive) return 0;
	float fa = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	float fb = fabsf(wrap(p->yaw - e->yaw));
	if (e->p_dist > 400.0f && e->p_dist < 700.0f && fa < 0.0873f && fb > 1.396f) return 2;
	if (e->p_dist < 600.0f && fa < 0.0873f) return 1;
	return 0;
}

static bool bos_special_spot(const Enemy* e)                   // 0x4b4420
{
	return hypotf(e->x - 58800.0f, e->z - 58700.0f) < 1000.0f || hypotf(e->x - 60900.0f, e->z - 55600.0f) < 1000.0f;
}

// Hits (0x4b4900): the third plank / pipe hits count x5; the handgun never makes her flinch, other
// weapons only while she hovers.
static int bos_check_damage(Enemy* e)
{
	if (!check_damage(e)) return 0;
	int id = e->last_atk;
	if (id == 0xe || id == 0x11) e->damage *= 5.0f;
	if (id == 0x15 || id == 0x16) e->damage *= 2.0f;
	reduce_hp(e);
	if (is_dead(e)) {
		e->attacking = false;
		go_raw(e, BOS_DEAD);
		return 2;
	}
	if ((e->anim == 1 && id != 1 && id != 2) || id == 0xe || id == 0x11 || id == 0x15 || id == 0x16) {
		anim_set(e, 8);
		return 1;
	}
	return 0;
}

// The moths (kind 15 under 0x4b4ec0): the port keeps them as one swarm point that flies at James
// ({0.4, 0.6, 1.0, 1.0} x a speed growing from 500), settles on him (attack 0x44) and holds him
// (0x45, 0.05 every 1/60 s) until he struggles free; then they flee.
//
// Drawing: Mary releases 33 of them (0x4b4ec0 case 0). Each is a camera-facing quad 100 units across
// (half size 50 from 0x4a5ec0) with a 2 x 2 sheet of wing beats, fly1.tbn2 (0x4a46f0); its frame
// runs through the sheet at 0x400 / 0x2580 of a turn per tick (0x4ac070). Every moth steers at the
// swarm's target with its own noise (0x4ab780), at chest height (y -16800) while it flies at James,
// on him while it holds him, and off towards one of three exits when it flees (0x4abcb0), where the
// PC removes them. The port gives each moth a fixed orbit around the swarm point instead of the
// steering, with the PC's look, count, size and flap rate.
#define MOTH_COUNT 33
static struct {
	bool on;
	float cx, cy, cz;        // swarm centre
	float spread;            // orbit radius
	float flee;              // seconds since they let go, 0 while they hunt or hold
	float idle;              // seconds since Mary last steered them
	float t;
} s_moths;
static Character s_moth_chr;
static bool s_moth_ok, s_moth_tried;

static void moths_load(void)
{
	if (s_moth_tried)
		return;
	s_moth_tried = true;
	char dir[96], err[96];
	snprintf(dir, sizeof dir, "%s/chr/moth", s_pack);
	s_moth_ok = chr_load(&s_moth_chr, dir, "moth", err, sizeof err) && s_moth_chr.n_bones == 4;
}

static void moths_update(float dt)
{
	if (!s_moths.on)
		return;
	s_moths.t += dt;
	s_moths.idle += dt;
	if (s_moths.flee == 0.0f && s_moths.idle > 0.1f)
		s_moths.flee = 0.001f;      // Mary moved on (they let go, gave up, or she was hit): they flee
	if (s_moths.flee > 0.0f) {
		s_moths.flee += dt;
		s_moths.cy -= 700.0f * dt;          // up and away (y points down)
		s_moths.spread += 400.0f * dt;
		if (s_moths.flee > 2.5f)
			s_moths.on = false;
	}
}

static void moths_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	if (!s_moths.on || !s_moth_ok)
		return;
	// the view's rotation transposed turns the quads to the camera; 100 units across
	float rot[3][3];
	for (int i = 0; i < 3; i++) {
		rot[i][0] = view->r[0].c[3 - i] * 100.0f;
		rot[i][1] = view->r[1].c[3 - i] * 100.0f;
		rot[i][2] = view->r[2].c[3 - i] * 100.0f;
	}
	float at[3] = { s_moths.cx, s_moths.cy, s_moths.cz };
	chrlight_at(at, false);
	for (int k = 0; k < MOTH_COUNT; k++) {
		// a fixed pseudo-random orbit per moth
		float a = k * 2.399963f, sp = 0.6f + (k % 7) * 0.12f, h = ((k * 37) % 11 - 5) * 0.12f;
		float ang = a + s_moths.t * sp * (k & 1 ? 1.0f : -1.0f);
		float r = s_moths.spread * (0.35f + ((k * 53) % 13) / 19.0f);
		float x = s_moths.cx + cosf(ang) * r;
		float z = s_moths.cz + sinf(ang) * r;
		float y = s_moths.cy + h * s_moths.spread + sinf(s_moths.t * 7.0f + k) * 40.0f;
		C3D_Mtx model, mv;
		Mtx_Identity(&model);
		Mtx_Translate(&model, x, y, z, true);
		Mtx_Multiply(&mv, view, &model);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
		// 0x400 per tick on a 0x2580 cycle of four frames, at 60 ticks a second
		int frame = (int)((s_moths.t * 60.0f * 1024.0f + k * 2400.0f) / 2400.0f) & 3;
		for (int b = 0; b < 4; b++) {
			Mtx34* m = &s_moth_chr.skin[b];
			memset(m, 0, sizeof *m);
			if (b == frame)
				for (int i = 0; i < 3; i++)
					m->m[i][0] = rot[i][0], m->m[i][1] = rot[i][1], m->m[i][2] = rot[i][2];
		}
		chr_draw(&s_moth_chr, uBones);
	}
}

static void bos_moths(Enemy* e, const EnPlayer* p, float dt)
{
	static const float k[5] = { 0.4f, 0.6f, 1.0f, 1.0f, 1.5f };
	static const float meter[5] = { 0.5f, 0.5f, 1.0f, 2.0f, 2.0f };
	switch (e->slv) {
	case 0:
		e->aux[0] = e->x, e->aux[1] = e->z, e->aux[2] = 500.0f;
		e->aux[3] = 0.0f;
		moths_load();
		s_moths.on = true, s_moths.flee = 0.0f;
		s_moths.cx = e->x, s_moths.cy = e->y, s_moths.cz = e->z, s_moths.spread = 150.0f;
		{ float pos[3] = { e->x, e->y, e->z }; sfx_play_at(18866, 1.0f, pos); }
		e->slv = 1;
		break;
	case 1: {
		float dx = p->x - e->aux[0], dz = p->z - e->aux[1], d = hypotf(dx, dz);
		e->aux[2] += 250.0f * dt;
		float v = k[s_diff] * sqrtf(d * 0.002f) * e->aux[2] * dt;
		if (d > 1.0f) e->aux[0] += dx / d * fminf(v, d), e->aux[1] += dz / d * fminf(v, d);
		e->aux[3] += dt;
		s_moths.cx = e->aux[0], s_moths.cz = e->aux[1];
		s_moths.cy += (-16800.0f - s_moths.cy) * fminf(1.0f, 2.0f * dt);   // down to chest height
		s_moths.spread += (300.0f - s_moths.spread) * fminf(1.0f, dt);
		if (d < 250.0f && !p->maria && p->alive) {
			combat_damage_james(0x44, 1.0f, e->aux[0], e->aux[1]);
			combat_grab_james(meter[s_diff]);
			e->slv = 2;
		} else if (e->aux[3] > (s_diff >= 3 ? 400.0f : 300.0f) / 60.0f) {
			go_raw(e, BOS_DISTANCE);
		}
		break;
	}
	case 2:
		combat_hold_damage(0.05f, dt);
		// on James: around his body (he stands on the floor, his chest about 900 above it)
		s_moths.cx += (p->x - s_moths.cx) * fminf(1.0f, 4.0f * dt);
		s_moths.cz += (p->z - s_moths.cz) * fminf(1.0f, 4.0f * dt);
		s_moths.cy += (p->y - 900.0f - s_moths.cy) * fminf(1.0f, 4.0f * dt);
		s_moths.spread += (110.0f - s_moths.spread) * fminf(1.0f, 3.0f * dt);
		if (!combat_held()) go_raw(e, BOS_DISTANCE);
		break;
	}
	s_moths.idle = 0.0f;
}

static void bos_move(Enemy* e, const EnPlayer* p, float dt)    // 0x4b4a10
{
	if (e->mlv != BOS_GRAB) {
		e->aux2 = fminf(e->aux2 + 250.0f * dt, bos_speed() * (e->slv == 2 ? 1.5f : 1.0f));
		if (e->anim == 8) e->aux2 = fmaxf(e->aux2 - 500.0f * dt, 0.0f);
		approach_angle(&e->path_angle, e->mark_angle, 0.0349f * dt * 60.0f);
		e->vx = sinf(e->path_angle) * e->aux2;
		e->vz = cosf(e->path_angle) * e->aux2;
	}
	if (e->anim != 4)                  // the body turns to James at 1 degree a tick
		approach_angle(&e->yaw, dir_to(e->x, e->z, p->x, p->z), 0.01745f * dt * 60.0f);
	// hovering about y -17500, and a dip when James swings at her within 750 (0x4b44b0)
	float term = e->anim == 8 ? (-17500.0f - e->y) * 0.2f : (rnd() - 0.5f) * 100.0f;
	e->aux3 = (term + e->aux3 - (e->y + 17500.0f) * 0.01f) * 0.98f;
	if (p->stomping && e->p_dist < 750.0f && e->y < -16950.0f) e->aux3 = 500.0f;
	if ((e->y < -17700.0f && e->aux3 < 0.0f) || (e->y >= -17300.0f && e->aux3 > 0.0f)) e->aux3 *= 0.5f;
	e->y += e->aux3 * dt;
	if (e->y > -16300.0f) e->y = -16300.0f;
}

static void bos_update(Enemy* e, const EnPlayer* p, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	if (e->mlv == BOS_DEAD) {
		e->vx = e->vz = 0.0f;
		if (e->status == 13) {
			// lying after the fall: one hit ends it (James is healed, flag 508 after two seconds)
			e->sound_wait += dt * 60.0f;
			if (e->sound_wait > 240.0f) { e->sound_wait = rnd() * 90.0f; play_at(e, 17015); }
			if (e->slv == 0 && check_damage(e)) {
				reset_damage(e);
				combat_set_hp(JAMES_HP_MAX);
				prog_stat_kill(e->last_atk);
				set_timer(e, 120);
				e->slv = 1;
			} else if (e->slv == 1 && reduce_timer(e, (int)(dt * 60.0f + 0.5f)) <= 0) {
				ev_flag_set(508, true);
				e->slv = 2;
			}
		} else {
			reset_damage(e);
			e->dead_flag = true;
			if (e->slv == 0) { anim_set(e, 8); set_timer(e, 60); e->slv = 1; }
			else if (e->slv == 1 && reduce_timer(e, (int)(dt * 60.0f + 0.5f)) <= 0) {
				ev_flag_set(506, true);    // event 7 runs EvProgMaryFall
				e->slv = 2;
			}
			e->y = fminf(e->y + 400.0f * dt, -16005.0f);
		}
		anim_step(e, dt);
		return;
	}
	int ticks = (int)(dt * 60.0f + 0.5f);
	switch (e->mlv) {
	case BOS_APPROACH: case BOS_DISTANCE: {
		if (bos_check_damage(e) == 1 && e->mlv == BOS_DISTANCE) go_raw(e, BOS_APPROACH);
		if (e->mlv == BOS_DEAD) break;
		if (e->anim != 1 && e->anim != 8) anim_set(e, 1);
		if (e->anim == 8 && e->anim_end) anim_set(e, 1);
		float tx, tz;
		if (e->mlv == BOS_APPROACH) {
			tx = p->x + sinf(p->yaw) * 550.0f, tz = p->z + cosf(p->yaw) * 550.0f;
		} else {
			float a = dir_to(e->x, e->z, p->x, p->z);
			tx = p->x - sinf(a) * 1500.0f, tz = p->z - cosf(a) * 1500.0f;
			if (e->slv == 0) { set_timer(e, 120); e->slv = 1; }
		}
		e->mark_angle = dir_to(e->x, e->z, tx, tz);
		int c = bos_can_attack(e, p);
		if (e->mlv == BOS_DISTANCE) {
			if (reduce_timer(e, ticks) <= 0 || c) go_raw(e, BOS_APPROACH);
			break;
		}
		if (c == 1 || (c == 2 && bos_special_spot(e))) go_raw(e, BOS_SWIPE);
		else if (c == 2) go_raw(e, BOS_GRAB);
		else if (e->p_dist >= 1500.0f && rnd() < 0.005f) go_raw(e, BOS_MOTHS);
		// out of ammunition (0x4b42b0): after 30 s (300 on hard) the fight ends all the same
		if (!combat_has_ammo()) {
			e->clock -= dt;
			if (e->clock <= 0.0f) { e->hp = 0.0f; go_raw(e, BOS_DEAD); }
		}
		break;
	}
	case BOS_MOTHS:
		if (bos_check_damage(e) == 2) break;
		bos_moths(e, p, dt);
		break;
	case BOS_GRAB: {
		static const float meter[5] = { 0.5f, 0.5f, 1.0f, 2.0f, 2.0f };
		if (combat_held()) reset_damage(e);
		else if (bos_check_damage(e) == 1) { combat_grab_james(0.0f); go_raw(e, BOS_DISTANCE); break; }
		if (e->mlv == BOS_DEAD) break;
		switch (e->slv) {
		case 0:
			anim_set(e, 2);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			play_at(e, 18852 + (int)(rnd() * 3.99f));
			e->slv = 1;
			break;
		case 1: {
			float tx = p->x + sinf(p->yaw) * 550.0f, tz = p->z + cosf(p->yaw) * 550.0f;
			e->vx = tx - e->x, e->vz = tz - e->z;
			float f = frame_in(e);
			if (f >= 11.0f && f <= 14.99f && swing_hit(e, p, 0x41, 10.0f, 300.0f + 250.0f, 0.8f))
				combat_grab_james(meter[s_diff]);
			if (!e->anim_end) break;
			e->attacking = false;
			e->vx = e->vz = 0.0f;
			if (e->attack_hit && combat_held()) { anim_set(e, 3); e->slv = 2; }
			else { anim_set(e, 6); e->slv = 4; }
			break;
		}
		case 2:
			if (e->anim_end) { anim_set(e, 4); e->slv = 3; }
			break;
		case 3:
			combat_hold_damage(0.2f, dt);
			if (!combat_held()) { anim_set(e, 5); e->slv = 4; }
			break;
		case 4:
			if (e->anim_end) { e->aux2 = 0.0f; go_raw(e, BOS_DISTANCE); }
			break;
		}
		break;
	}
	case BOS_SWIPE:
		if (bos_check_damage(e) == 1) { e->attacking = false; go_raw(e, BOS_DISTANCE); break; }
		if (e->mlv == BOS_DEAD) break;
		if (e->slv == 0) {
			anim_set(e, 7);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			play_at(e, 18850 + (int)(rnd() * 1.99f));
			e->slv = 1;
		}
		{
			float f = frame_in(e);
			if (f >= 7.0f && f <= 9.99f) swing_hit(e, p, 0x43, 20.0f, 300.0f + 250.0f, 0.8f);
		}
		if (e->anim_end) { e->attacking = false; go_raw(e, BOS_APPROACH); }
		break;
	}
	if (e->mlv == BOS_DEAD) return;
	bos_move(e, p, dt);
	anim_step(e, dt);
	// she flies over the floor: walls only
	float nx = e->x + e->vx * dt, nz = e->z + e->vz * dt;
	if (s_col) col_push(s_col, &nx, &nz, -16005.0f, 2500.0f, e->size);
	e->x = nx, e->z = nz;
	// a murmur every second (0x4b45e0, 18862..18865 at 0.4)
	e->sound_wait += dt * 60.0f;
	if (e->sound_wait >= 60.0f) {
		e->sound_wait = 0.0f;
		float pos[3] = { e->x, e->y, e->z };
		sfx_play_at(18862 + (int)(rnd() * 3.99f), 0.4f, pos);
	}
}

// 0x4b46f0: health {1200, 4000, 8000, 20000}, no spread.
static void bos_init(Enemy* e, int status)
{
	static const float hp[5] = { 1200, 4000, 8000, 20000, 30000 };
	e->ctrl = 1;
	e->hp = e->hp_max = e->endurance = e->endurance_max = hp[s_diff];
	e->clock = s_diff <= 2 ? 30.0f : 300.0f;
	e->path_angle = e->mark_angle = e->yaw;
	set_size(e, 250.0f, 800.0f, true);
	if (status == 13) {
		anim_set(e, 9);
		set_size(e, 350.0f, 150.0f, true);
		e->mlv = BOS_DEAD;
		return;
	}
	anim_set(e, 1);
	e->mlv = BOS_APPROACH;
	e->y = -17500.0f;
}

// ---------------------------------------------------------------- the Mandarin (kind 0x20a, chr/arm)
//
// PC en_arm code (EnLOCAL kind 10): control 0x4ad0e0, automatic 0x4acff0, init 0x4ac770; states
// 0 Wander 0x4acc00, 1 Turn 0x4ace40, 2 Damage 0x4ac890, 3 dying. It hangs under the floor and
// crawls on its two arms, planting one hand per step (0x4a6e40). The PC only lets it strike when
// both hands and the point ahead are over a grate (floor material 9 / 0xb, 0x4a72c0); the port
// reads the materials from the room's footstep floor (sound/floor/<map>.sfm, the same .cld
// materials). It still wanders within 2500 of where it was placed.
// EnARMAnime 0x892870 (arm_anim at 0x6e967c).
static const ScuAnim ARM_ANIM[14] = {
	{ 0, 48, 0, 2048 },      // 0 5251 recoils
	{ 0, 48, 1, 2048 },      // 1 5251 idle
	{ 49, 65, 0, 2048 },     // 2 5252 step, hand 0 planted (about 590 units a step)
	{ 66, 82, 0, 2048 },     // 3 5253 step, hand 1 planted
	{ 83, 99, 0, 2048 },     // 4 5254 strike (attack 0x46)
	{ 100, 116, 0, 2048 },   // 5 5255 strike (attack 0x47)
	{ 117, 132, 0, 2048 },   // 6 5256 hit
	{ 133, 148, 0, 2048 },   // 7 5257
	{ 149, 179, 0, 2048 },   // 8 5258
	{ 180, 210, 0, 2048 },   // 9 5259
	{ 211, 241, 0, 2048 },   // 10 5260
	{ 242, 272, 0, 2048 },   // 11 5261
	{ 273, 338, 0, 2048 },   // 12 5262 dies
	{ 339, 404, 0, 2048 },   // 13 5263 dies
};

static float arm_rate(const Enemy* e)                          // 0x4ac680
{
	static const float t[5] = { 0.7f, 0.8f, 1.0f, 1.2f, 1.5f };
	return t[s_diff] * (hp_rate(e) * 0.004f + 0.6f) * (e->p_dist > 5000.0f ? 4.0f : 1.0f);
}

// 0x4a72c0: is the floor at (x, z) a grate? The vertical probe there reports 2 for material 9 or
// 0xb, 1 for any other floor and 0 for none; the footstep floor answers 0 when it has nothing, so
// a missing floor counts as solid here (no strike), which errs on the safe side.
static bool arm_grate(float x, float z, float floor_y)
{
	int m = sfx_floor_material(x, floor_y, z);
	return m == 9 || m == 0xb;
}

// 0x4ac9d0: 0 unaware, 1 track James, 2 strike (within 1000, with both hands, 550 out from the
// shoulders, and the point 500 ahead all over a grate).
static int arm_decide(const Enemy* e, const EnPlayer* p)
{
	float a = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	if (!p->alive || !((e->p_dist <= 2500.0f && a <= 2.0944f) || e->anim >= 6))
		return 0;
	if (e->p_dist >= 1000.0f) return 1;
	float fx = sinf(e->yaw), fz = cosf(e->yaw), rx = fz, rz = -fx;
	for (int h = -1; h <= 1; h += 2)
		if (!arm_grate(e->x + fx * 550.0f + rx * 150.0f * h, e->z + fz * 550.0f + rz * 150.0f * h, p->y))
			return 1;
	return arm_grate(e->x + fx * 500.0f, e->z + fz * 500.0f, p->y) ? 2 : 1;
}

static void arm_update(Enemy* e, const EnPlayer* p, float dt)
{
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	int ticks = (int)(dt * 60.0f + 0.5f);
	if (e->timer > 0) e->timer = e->timer > ticks ? e->timer - ticks : 0;
	switch (e->mlv) {
	case ARM_WANDER:
		if (check_damage(e)) { go_raw(e, ARM_DAMAGE); break; }
		if (e->slv == 0) { e->anim_end = true; e->slv = 1; e->ival = -1; }
		if (e->attacking) {
			float f = frame_in(e);
			if (f <= 16.0f) swing_hit(e, p, e->anim == 4 ? 0x46 : 0x47, 10.0f, 450.0f + 300.0f, 1.0f);
		}
		if (e->anim_end) {
			int d = arm_decide(e, p);
			int s = d == 0 ? 1 : d == 1 ? (e->timer == 0 ? 2 : 1) : 3;
			if (s == 3) e->amb_n++;
			if ((e->slv > 1 && s != e->slv && s < 3) || e->amb_n > 4) {
				set_timer(e, 180);
				e->amb_n = 0;
				if (s == 3) s = 1;
			}
			e->slv = s;
			e->ival = e->ival == 0 ? 1 : 0;   // the hands take turns
			e->attacking = s == 3;
			e->attack_hit = e->maria_hit = false;
			anim_set(e, (s < 3 ? 3 : 5) - e->ival);
			e->frame = ARM_ANIM[e->anim].start;
			e->anim_end = false;
			if (s == 3) play_at(e, 12252 + (int)(rnd() * 1.99f));
			// the hand comes down (0x4acab0): 0x2fda on a grate, 0x2fdb elsewhere
			if (e->p_dist < 5000.0f)
				play_at(e, arm_grate(e->x + sinf(e->yaw) * 550.0f, e->z + cosf(e->yaw) * 550.0f, p->y) ? 12250 : 12251);
		}
		{
			float tx = e->slv == 1 ? e->x + sinf(e->yaw) * 1000.0f : p->x;
			float tz = e->slv == 1 ? e->z + cosf(e->yaw) * 1000.0f : p->z;
			if (e->slv == 1 && hypotf(e->x - e->stx, e->z - e->stz) > 2500.0f) tx = e->stx, tz = e->stz;
			walk_to(e, tx, tz, 0.05236f, dt);
		}
		e->rate = arm_rate(e);
		break;
	case ARM_DAMAGE:
		if (e->slv == 0) {
			reduce_hp(e);
			e->attacking = false;
			int id = e->last_atk;
			bool b = !(e->anim == 1 || e->anim == 6 || e->anim == 8 || e->anim == 9) &&
			         ((e->anim < 2 || e->anim > 5) || ((frame_in(e) > 8.0f) != (e->ival != 0)));
			bool front = fabsf(blow_side(e)) > PI / 2.0f;
			int a = (id == 1 || id == 2) ? 6 + b : (id == 4 || id == 6) ? 6 + 2 * b + front : 0;
			e->ival4 = a;
			anim_set(e, a);
			e->slv = 1;
			break;
		}
		if (e->slv == 1) {
			if (check_damage(e)) reduce_hp(e);
			if (!e->anim_end) break;
			if (!is_dead(e)) { set_timer(e, 0); e->amb_n = 0; go_raw(e, ARM_WANDER); break; }
			anim_set(e, (e->ival4 == 6 || e->ival4 == 8 || e->ival4 == 9) ? 12 : 13);
			mark_killed(e);
			e->dead_flag = true;
			e->slv = 2;
		} else if (e->anim_end) {
			go_raw(e, ARM_DYING);
		}
		break;
	case ARM_DYING:
		reset_damage(e);
		break;
	}
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	// a growl every second while it moves about (0x4ac700, 12258..12261)
	if (e->mlv == ARM_WANDER) {
		e->sound_wait += dt * 60.0f;
		if (e->sound_wait >= 60.0f) { e->sound_wait = 0.0f; play_at(e, 12258 + (int)(rnd() * 3.99f)); }
	}
}

// 0x4ac770: health {300, 800, 1200, 2500} with the usual spread.
static void arm_init(Enemy* e, int status)
{
	static const float hp[5] = { 300, 800, 1200, 2500, 4000 };
	e->ctrl = 1;
	e->path_angle = e->mark_angle = e->yaw;
	e->stx = e->x, e->stz = e->z;
	set_size(e, 300.0f, 500.0f, true);
	anim_set(e, 1);
	if (status == 5) {
		e->hp = 0.0f;
		e->mlv = ARM_DYING;
		anim_set(e, 13);
		e->frame = ARM_ANIM[13].end;
		e->dead_flag = true;
		return;
	}
	e->mlv = ARM_WANDER;
	set_hp(e, hp[s_diff], hp[s_diff]);
}

static EnKind model_kind(int m)
{
	switch (m) {
	case MDL_MKN: return EN_KIND_MKN;
	case MDL_TYU: return EN_KIND_TYU;
	case MDL_SCU: return EN_KIND_SCU;
	case MDL_RED: return EN_KIND_RED;
	case MDL_PAP: return EN_KIND_PAP;
	case MDL_ONI: return EN_KIND_ONI;
	case MDL_BOS: return EN_KIND_BOS;
	case MDL_ARM: return EN_KIND_ARM;
	case MDL_EDI: return EN_KIND_EDI;
	case MDL_IKE: return EN_KIND_IKE;
	case MDL_TY2: case MDL_TY3: return EN_KIND_NEST;
	default: return EN_KIND_NSE;
	}
}

// ---------------------------------------------------------------- creeper nests (kinds 0x20c, 0x20d)
//
// chr/item ty2 and ty3: creepers massed on the floor, a wall or the ceiling (EnLOCAL kinds 13 and
// 14: controls 0x4b0250 / 0x4b02e0, inits 0x4adc10 / 0x4ade20; rest states 0x4af230.. by status, 1 / 3
// a wall, 4 the ceiling). A nest cannot be killed: every blow that lands releases one creeper at its
// spot (0x4add40 / 0x4ae020), stunned at first (state 7), as long as there are fewer than twelve
// (DAT_01f7fa20 counts them). The port's creepers do not climb, so a released one drops to the
// floor under the nest. Nests that sit on a wall (statuses 1 and 3) are lined up with the nearest
// wall segment at their height, the way the PC's wall probe (0x4af260 -> 0x4ae7d0) stands them on
// the wall facing into the room (e->type 1, the wall's inward bearing in mark_angle); a ceiling nest
// (status 4, 0x4af2c0) is turned over (rotation pi about x) at its own height (type 2).
// Hit columns: radius 100 (ty2) and 250 (ty3), 150 tall.
static void nest_init(Enemy* e, int status)
{
	e->ctrl = 1;
	set_size(e, e->model == MDL_TY3 ? 250.0f : 100.0f, 150.0f, false);
	e->hp = e->hp_max = 1.0f;
	if (status == 4) {
		e->type = 2;
		e->y = e->aux[0];
	} else if ((status == 1 || status == 3) && s_col) {
		float y = e->aux[0], best = 1e30f, bx = 0.0f, bz = 0.0f, nx = 0.0f, nz = 0.0f;
		for (u32 i = 0; i < s_col->n_walls; i++) {
			const WallSeg* w = &s_col->walls[i];
			if (y < w->y_top - 100.0f || y > w->y_bottom + 100.0f)
				continue;
			float vx = w->x1 - w->x0, vz = w->z1 - w->z0, l2 = vx * vx + vz * vz;
			if (l2 < 1.0f) continue;
			float t = ((e->x - w->x0) * vx + (e->z - w->z0) * vz) / l2;
			t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
			float px = w->x0 + vx * t, pz = w->z0 + vz * t, d = hypotf(e->x - px, e->z - pz);
			if (d < best) {
				float l = sqrtf(l2);
				best = d, bx = px, bz = pz, nx = -vz / l, nz = vx / l;
				if (nx * (e->x - px) + nz * (e->z - pz) < 0.0f) nx = -nx, nz = -nz;   // into the room
			}
		}
		if (best < 1500.0f) {
			e->type = 1;
			e->y = y;
			e->x = bx + nx * 20.0f, e->z = bz + nz * 20.0f;
			e->mark_angle = atan2f(nx, nz);
		}
	}
}

static int creeper_count(void)
{
	int n = 0;
	for (int i = 0; i < ENEMY_MAX; i++)
		if (s_en[i].used && s_en[i].kind == EN_KIND_TYU && s_en[i].mlv != TYU_DYING)
			n++;
	return n;
}

static void nest_update(Enemy* e, const EnPlayer* p, float dt)
{
	(void)dt;
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	if (!check_damage(e))
		return;
	reset_damage(e);
	if (creeper_count() >= 12 || !load_model(MDL_TYU))
		return;
	for (int k = 0; k < ENEMY_MAX; k++) {
		if (s_en[k].used)
			continue;
		Enemy* c = &s_en[k];
		memset(c, 0, sizeof *c);
		c->used = true;
		c->kind = EN_KIND_TYU;
		c->model = MDL_TYU;
		c->list_id = -1;                  // not on the list: gone with the room
		c->amb = -1;
		c->x = e->x, c->z = e->z, c->y = e->y;
		c->yaw = e->type == 1 ? e->mark_angle : e->yaw;
		tyu_init(c, 0);
		if (e->type != 0) {
			// from a wall or ceiling nest it drops (state 7 sub 0xff), a little out from the wall
			if (e->type == 1) c->x += sinf(e->mark_angle) * 100.0f, c->z += cosf(e->mark_angle) * 100.0f;
			tyu_fall(c);
		} else {
			go_raw(c, TYU_DAMAGE);
		}
		return;
	}
}

static void kind_init(Enemy* e, int status)
{
	switch (e->kind) {
	case EN_KIND_RED: red_init(e, status); break;
	case EN_KIND_PAP: pap_init(e, status); break;
	case EN_KIND_ONI: oni_init(e, status); break;
	case EN_KIND_BOS: bos_init(e, status); break;
	case EN_KIND_ARM: arm_init(e, status); break;
	case EN_KIND_EDI: edi_init(e, status); break;
	case EN_KIND_IKE: ike_init(e, status); break;
	case EN_KIND_NEST: nest_init(e, status); break;
	default: scu_init(e, status); break;
	}
}


// ---------------------------------------------------------------- Eddie (kind 0x205, chr/edi lll_edi)
//
// PC en_edi code (EnLOCAL kind 9): control 0x4b41b0, automatic 0x4b4100, init 0x4b0ed0, damage
// 0x4b07d0; states 0 approach 0x4b2170, 1 walk about 0x4b1060, 2 to cover 0x4b2270, 3 round the
// pillars 0x4b2700, 4 patrol 0x4b2d90, 5 shoot 0x4b3010, 6 close in and punch 0x4b3660, 7 flinch
// 0x4b1480, 8 flee 0x4b3d90, 9 death 0x4b0590. The meat locker fight has two rounds: the first
// (status 0, stage 37 program 1) ends when his health is gone and he runs for the door at
// (100000, -99100), raising flag 373 (program 2 takes him away); the second (status 6, program 3)
// ends with his death and flag 374 (program 4). He walks and runs by velocity (1000 / 1400 units
// a second times {0.7, 0.8, 1.0, 1.2}), shoots (attack 58: kind 3, ap 24, a ray 5000 long) with
// six rounds before he stops to reload, and punches (attack 59: ap 15, frames 9..11) when James is
// within 550. The port folds the cover and pillar runs (2, 3) into closing in and backing off.
// EnEDIAnime 0x892bb8 (edi_anim at 0x6dca20).
static const ScuAnim EDI_ANIM[22] = {
	{ 0, 9, 1, 384 },        // 0 5901 stands
	{ 0, 9, 1, 384 },        // 1
	{ 10, 33, 1, 2048 },     // 2 5902 walks
	{ 34, 57, 1, 2048 },     // 3 5903 runs
	{ 58, 77, 0, 2048 },     // 4 5904 side step left
	{ 78, 97, 0, 2048 },     // 5 5905 side step right
	{ 98, 121, 0, 2048 },    // 6 5906 raises the gun
	{ 122, 163, 0, 2048 },   // 7 5907 fires
	{ 164, 196, 0, 1536 },   // 8 5908 punches
	{ 197, 256, 0, 2048 },   // 9 5909 falls dead
	{ 257, 271, 0, 2048 },   // 10.. flinches by weapon and side (0x4b06c0)
	{ 272, 286, 0, 2048 }, { 287, 301, 0, 2048 }, { 302, 316, 0, 2048 }, { 317, 331, 0, 2048 },
	{ 332, 346, 0, 2048 }, { 347, 361, 0, 2048 }, { 362, 376, 0, 2048 }, { 377, 391, 0, 2048 },
	{ 392, 406, 0, 2048 }, { 407, 421, 0, 2048 }, { 422, 436, 0, 2048 },
};

static float edi_speed(bool run)                               // 0x4b0bc0 / 0x4b0c20
{
	static const float walk[5] = { 0.7f, 0.8f, 1.0f, 1.2f, 1.5f }, rn[5] = { 0.7f, 0.8f, 1.0f, 1.4f, 1.5f };
	return run ? rn[s_diff] * 1400.0f : walk[s_diff] * 1000.0f;
}

static float edi_turn(void)                                    // 0x4b0d10
{
	static const float t[5] = { 0.5f, 0.6f, 0.85f, 1.2f, 1.5f };
	return t[s_diff] * 0.05235988f;
}

// 0x4b1810: 3 James within 550 and 30 degrees ahead (punch), 2 within 4000 and 30 degrees with a
// clear line (shoot), 1 within 80 degrees, 0 not.
static int edi_sense(const Enemy* e, const EnPlayer* p)
{
	if (!p->alive || !line_of_sight(e, p) || e->p_dist > 5000.0f)
		return 0;
	float a = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
	if (e->p_dist < 550.0f && a < 0.5236f) return 3;
	if (e->p_dist < 4000.0f && a < 0.5236f) return 2;
	return a <= 1.3963f ? 1 : 0;
}

static int edi_flinch_anim(const Enemy* e)                     // 0x4b06c0
{
	int front = fabsf(blow_side(e)) <= 2.0944f;
	switch (e->last_atk) {
	case 1: case 2: case 4: case 6: return 10 + front;
	case 0xc: case 0xd: case 0xe: case 0xf: case 0x10: case 0x11: case 0x17: return 14 + front;
	case 0x12: case 0x18: return 18 + front;
	default: return 10;
	}
}

static void edi_move(Enemy* e, float speed)
{
	e->vx = sinf(e->yaw) * speed;
	e->vz = cosf(e->yaw) * speed;
}

// Round two's cover (state 2, 0x4b2270 with the chooser 0x4b1640), taken after a shot once his
// health is under 30 % (0x4b3010): eleven crates and carcasses in the freezer (0x89294c, ids 6..16). The spot is 500 behind one of them as seen from James, at least
// 2000 from him and out of his sight, and not one of the last three he used (+0xc5..0xc7). He runs
// there, waits up to 600 ticks facing James, shoots when he has him in view, and moves on when James
// comes within 1500 (or closes in to punch within 1000). The PC prefers the spot whose cover is
// deepest (0x4aa800); the port takes the nearest valid one.
//
// With no cover to be had (0x4b1640 returns -1) he runs round the pillars instead (state 3,
// 0x4b2700): to one of the six at 0x8928c0 that he has not used lately (the port picks the nearest
// such), where he waits {3, 4, 5, 6} seconds by difficulty facing James, shooting when he has him
// in view and leaning out sideways (anime 4 / 5, towards James' side past 50 degrees) up to three
// times when he does not. On beginner and easy he goes back to looking for cover at once. James
// within 1000 makes him close in to punch.
static const float EDI_COVER_PT[11][2] = {
	{ 138900.0f, -97760.6f }, { 140200.0f, -97561.6f }, { 141500.0f, -96916.0f }, { 138900.0f, -98986.6f },
	{ 141500.0f, -98202.3f }, { 138900.0f, -100236.0f }, { 140200.0f, -99800.4f }, { 141500.0f, -101270.5f },
	{ 138900.0f, -102965.7f }, { 140200.0f, -102423.5f }, { 141500.0f, -102587.9f },
};

static const float EDI_PILLAR_PT[6][2] = {
	{ 142500.0f, -103750.0f }, { 142500.0f, -96250.0f }, { 138250.0f, -103750.0f },
	{ 138250.0f, -97500.0f }, { 139500.0f, -96250.0f }, { 142500.0f, -97000.0f },
};

static void edi_pick_pillar(Enemy* e)
{
	int best = -1;
	float bd = 1e30f;
	for (int k = 0; k < 6; k++) {
		bool recent = false;
		for (int j = 0; j < 3; j++)
			if (((e->ival4 >> (j * 8)) & 0xff) == k + 1) recent = true;
		float d = hypotf(EDI_PILLAR_PT[k][0] - e->x, EDI_PILLAR_PT[k][1] - e->z);
		if (!recent && d < bd) bd = d, best = k;
	}
	if (best < 0) best = 0;
	e->aux[0] = EDI_PILLAR_PT[best][0], e->aux[1] = EDI_PILLAR_PT[best][1];
	e->ival4 = ((e->ival4 << 8) | (best + 1)) & 0xffffff;
}

static bool edi_pick_cover(Enemy* e, const EnPlayer* p)
{
	int best = -1;
	float bd = 1e30f, bx = 0.0f, bz = 0.0f;
	for (int k = 0; k < 11; k++) {
		bool recent = false;
		for (int j = 0; j < 3; j++)
			if (((e->ival3 >> (j * 8)) & 0xff) == k + 1) recent = true;
		if (recent) continue;
		float px = EDI_COVER_PT[k][0], pz = EDI_COVER_PT[k][1];
		float dx = px - p->x, dz = pz - p->z, d = hypotf(dx, dz);
		if (d < 1.0f) continue;
		float sx = px + dx / d * 500.0f, sz = pz + dz / d * 500.0f;
		if (hypotf(sx - p->x, sz - p->z) < 2000.0f) continue;
		if (s_col) {
			float cx = sx, cz = sz;
			col_clamp_ray(s_col, p->x, p->z, &cx, &cz, p->y - 500.0f);
			if (hypotf(cx - sx, cz - sz) < 40.0f) continue;     // James would see him there
		}
		float de = hypotf(sx - e->x, sz - e->z);
		if (de < bd) bd = de, best = k, bx = sx, bz = sz;
	}
	if (best < 0) return false;
	e->aux[0] = bx, e->aux[1] = bz;
	e->ival3 = ((e->ival3 << 8) | (best + 1)) & 0xffffff;
	return true;
}

static void edi_update(Enemy* e, const EnPlayer* p, float dt)
{
	static const s8 stamina[5] = { 7, 15, 30, 30, 30 };    // 0x4b0d50: shots between rests
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	int ticks = (int)(dt * 60.0f + 0.5f);
	e->vx = e->vz = 0.0f;
	if ((e->mlv < EDI_FLINCH || e->mlv >= EDI_COVER) && check_damage(e)) {
		// 0x4b07d0: a spent endurance makes him flinch; the health running out ends the round
		e->attacking = false;
		float en = reduce_hp(e);
		if (is_dead(e) || en <= 0.0f) { go_raw(e, EDI_FLINCH); }
	}
	float f0 = e->frame;
	switch (e->mlv) {
	case EDI_CHASE: {
		int s = edi_sense(e, p);
		bool far = e->p_dist > 2500.0f;
		if (e->slv == 0 || (far && e->anim != 3) || (!far && e->anim != 2)) {
			anim_set(e, far ? 3 : 2);
			e->path_angle = e->yaw;
			e->slv = 1;
		}
		walk_to(e, p->x, p->z, edi_turn(), dt);
		// round two keeps his distance: he backs off when James comes within 1000
		float speed = edi_speed(far);
		if (e->type == 1 && e->p_dist < 1000.0f && s != 3) speed = -speed * 0.6f;
		edi_move(e, speed);
		if (e->timer > 0) e->timer = e->timer > ticks ? e->timer - ticks : 0;
		if (s == 3 && e->timer == 0) { go_raw(e, EDI_PUNCH); break; }
		if (s == 2 && e->timer == 0 && (e->p_dist > 1000.0f || e->type == 0)) { go_raw(e, EDI_SHOOT); break; }
		break;
	}
	case EDI_PILLAR: {
		int s = edi_sense(e, p);
		if (e->p_dist < 1000.0f) { go_raw(e, EDI_CHASE); break; }     // closes in to punch
		if (e->slv == 0) { anim_set(e, 3); e->path_angle = e->yaw; e->slv = 1; }
		if (e->slv == 1) {
			walk_to(e, e->aux[0], e->aux[1], edi_turn() * 2.0f, dt);
			edi_move(e, edi_speed(true));
			if (hypotf(e->aux[0] - e->x, e->aux[1] - e->z) < 150.0f) {
				anim_set(e, 0);
				e->aux[2] = (float)(s_diff + 3);
				e->aux[3] = 0.0f;
				e->amb_n = 0;
				e->slv = s_diff < 2 ? 4 : 2;
			}
		} else if (e->slv == 2 || e->slv == 3) {
			// waiting at the pillar; slv 3 while a lean out plays
			if (e->slv == 3) {
				// anime 4 steps to his left, 5 to his right
				float side = (e->anim == 4 ? -1.0f : 1.0f) * edi_speed(false) * 0.6f;
				e->vx = cosf(e->yaw) * side, e->vz = -sinf(e->yaw) * side;
				if (e->anim_end) { anim_set(e, 0); e->slv = 2; }
				break;
			}
			face(e, p->x, p->z, edi_turn(), dt);
			e->aux[2] -= dt;
			e->aux[3] += dt;
			if (e->timer > 0) e->timer = e->timer > ticks ? e->timer - ticks : 0;
			if (s == 2 && e->timer == 0) { go_raw(e, EDI_SHOOT); break; }
			if (!line_of_sight(e, p) && e->amb_n < 3 && e->aux[3] > 1.0f) {
				float a = wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw);
				if (fabsf(a) > 0.8727f) {
					anim_set(e, a > 0.0f ? 4 : 5);
					e->amb_n++;
					e->aux[3] = 0.0f;
					e->slv = 3;
					break;
				}
			}
			if (e->aux[2] <= 0.0f) e->slv = 4;
		} else {
			// done here: cover if there is some, else the next pillar
			if (edi_pick_cover(e, p)) go_raw(e, EDI_COVER);
			else { edi_pick_pillar(e); e->slv = 0; }
		}
		break;
	}
	case EDI_COVER: {
		int s = edi_sense(e, p);
		if (e->p_dist < 1000.0f) { go_raw(e, EDI_CHASE); break; }     // closes in to punch
		if (e->slv == 0) { anim_set(e, 3); e->path_angle = e->yaw; e->slv = 1; }
		if (e->slv == 1) {
			walk_to(e, e->aux[0], e->aux[1], edi_turn() * 2.0f, dt);
			edi_move(e, edi_speed(true));
			if (hypotf(e->aux[0] - e->x, e->aux[1] - e->z) < 150.0f) {
				anim_set(e, 0);
				e->aux[2] = 10.0f;      // 600 ticks
				e->slv = 2;
			}
		} else {
			face(e, p->x, p->z, edi_turn(), dt);
			e->aux[2] -= dt;
			if (s == 2 && e->timer == 0) { go_raw(e, EDI_SHOOT); break; }
			if (e->timer > 0) e->timer = e->timer > ticks ? e->timer - ticks : 0;
			if (e->aux[2] <= 0.0f || e->p_dist < 1500.0f) {
				if (edi_pick_cover(e, p)) { anim_set(e, 3); e->slv = 1; }
				else { edi_pick_pillar(e); go_raw(e, EDI_PILLAR); }
			}
		}
		break;
	}
	case EDI_SHOOT:
		face(e, p->x, p->z, edi_turn(), dt);
		if (e->slv == 0) { anim_set(e, 6); e->slv = 1; }
		else if (e->slv == 1 && e->anim_end) {
			anim_set(e, 7);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			play_at(e, 11048);          // the shot
			// the ray (0x544870): James or Maria if it reaches them, walls stop it
			if (p->alive && line_of_sight(e, p) && e->p_dist < 5000.0f &&
			    fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw)) < 0.12f) {
				if (p->maria) maria_hit(e->x, e->z, e->yaw, 5000.0f, 0.12f, 24.0f);
				else hit_james(p, 58, 24.0f, e);
			}
			e->ival--;
			e->slv = 2;
		} else if (e->slv == 2 && e->anim_end) {
			e->attacking = false;
			if (e->ival <= 0) {          // out of rounds: a breather while he reloads
				e->ival = 6;
				set_timer(e, stamina[s_diff] * 20);
			} else {
				set_timer(e, 30);
			}
			// 0x4b3010 case 3: in round two, once his health is under 30 % he goes for cover
			// (state 2) after a shot, or round the pillars when there is none; above it he keeps
			// coming (state 4)
			if (e->type == 1 && hp_rate(e) < 30.0f && e->p_dist > 1000.0f) {
				if (edi_pick_cover(e, p)) go_raw(e, EDI_COVER);
				else { edi_pick_pillar(e); go_raw(e, EDI_PILLAR); }
			} else {
				go_raw(e, EDI_CHASE);
			}
		}
		break;
	case EDI_PUNCH:
		if (e->slv == 2) {             // side-stepping away
			edi_move(e, -edi_speed(false));
			if (e->anim_end) { set_timer(e, 120); go_raw(e, EDI_CHASE); }
			break;
		}
		face(e, p->x, p->z, edi_turn(), dt);
		if (e->slv == 0) {
			anim_set(e, 8);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			e->slv = 1;
		}
		{
			float f = frame_in(e);
			if (f >= 9.0f && f <= 11.99f) swing_hit(e, p, 59, 15.0f, 250.0f + 300.0f, 0.8f);
		}
		if (e->anim_end) {
			// 0x4b3660: a couple of punches ({2, 10} by difficulty, 0x4b0da0), then he side-steps
			// away and keeps his distance for a while
			e->attacking = false;
			e->amb_n++;
			if (e->amb_n >= (s_diff < 2 ? 2 : 3)) {
				e->amb_n = 0;
				anim_set(e, rnd() < 0.5f ? 4 : 5);
				e->slv = 2;
			} else {
				go_raw(e, EDI_CHASE);
			}
		}
		break;
	case EDI_FLINCH:
		if (e->slv == 0) {
			anim_set(e, edi_flinch_anim(e));
			if (e->endurance <= 0.0f) e->endurance = e->endurance_max;
			e->slv = 1;
		}
		if (check_damage(e)) reduce_hp(e);
		if (!e->anim_end) break;
		if (is_dead(e)) {
			if (e->type == 0) go_raw(e, EDI_FLEE);
			else {
				float pos[3] = { e->x, e->y, e->z };
				sfx_play_at(18207, 1.0f, pos);   // 0x471f
				anim_set(e, 9);
				go_raw(e, EDI_DEATH);
			}
		} else {
			go_raw(e, EDI_CHASE);
		}
		break;
	case EDI_FLEE:
		// 0x4b3d90: runs for the door; within 500 of it, facing it, flag 373 ends round one
		reset_damage(e);
		if (e->slv == 0) { anim_set(e, 3); e->path_angle = e->yaw; e->slv = 1; }
		walk_to(e, 100000.0f, -99100.0f, edi_turn(), dt);
		edi_move(e, 1400.0f);
		if (hypotf(100000.0f - e->x, -99100.0f - e->z) < 500.0f) ev_flag_set(373, true);
		break;
	case EDI_DEATH:
		reset_damage(e);
		if (e->slv == 0) { set_timer(e, 90); e->rate = 0.25f; e->dead_flag = true; e->slv = 1; }
		if (reduce_timer(e, ticks) <= 0) ev_flag_set(374, true);
		break;
	}
	if (e->rate == 0.0f) e->rate = 1.0f;
	float adv = anim_step(e, dt);
	move_exec(e, adv, dt);
	if (e->anim == 2 || e->anim == 3) {
		// footsteps 0x4718 on frames 7 and 19 of the walk, 3 and 15 of the run
		const ScuAnim* t = &EDI_ANIM[e->anim];
		float a = f0 - t->start, b = e->frame - t->start, s1 = e->anim == 2 ? 7.0f : 3.0f;
		if (b < a) b += 24.0f;
		if ((a < s1 && b >= s1) || (a < s1 + 12.0f && b >= s1 + 12.0f)) play_at(e, 18200);
	}
}

// 0x4b0ed0: round one {100, 1800, 2500, 4000} health, round two {1000, 4000, 5000, 10000};
// endurance {200, 300, 350, 500}; no spread.
static void edi_init(Enemy* e, int status)
{
	static const float hp0[5] = { 100, 1800, 2500, 4000, 5000 }, hp6[5] = { 1000, 4000, 5000, 10000, 12000 };
	static const float en[5] = { 200, 300, 350, 500, 600 };
	e->ctrl = 1;
	e->path_angle = e->mark_angle = e->yaw;
	set_size(e, 200.0f, 850.0f, true);
	e->type = status == 6 ? 1 : 0;
	e->hp = e->hp_max = (e->type ? hp6 : hp0)[s_diff];
	e->endurance = e->endurance_max = en[s_diff];
	e->ival = 6;                        // rounds in the gun (+0xc4)
	e->mlv = EDI_CHASE;
	anim_set(e, 1);
}


// ---------------------------------------------------------------- Flesh Lip (kind 0x203, chr/ike)
//
// PC en_ike code (EnLOCAL kind 7): control 0x4b9b70, automatic 0x4b9ae0, init 0x4b8700; states
// 0 wait above 0x4b87e0, 1 drop 0x4b89b0, 2 wander 0x4b8ab0, 3 chase 0x4b8e00, 4 0x4b9190,
// 5 0x4b92c0, 6 grab 0x4b9460, 7 kick 0x4b96a0, 8 lift 0x4b7ff0, 9 damage 0x4b97a0, 10 death
// 0x4b9960. EvProgLauraDeceive3rd (0x58b750, stage 22) hangs three of them in the room: two drop
// to their working height (y -1640) at once, the third waits up at -3200 until the others are
// dead (or three minutes) and comes down at the drop point farthest from James. They glide on
// the ceiling rails at {0.5, 0.8, 1.0, 1.2} x 500 x (0.8 + 0.002 x health %); when the last one
// dies flag 210 runs EvProgHospitalBack1st (event 3). EnIKEAnime 0x892ce0 (ike_anim at 0x6de2a8).
static const ScuAnim IKE_ANIM[21] = {
	{ 0, 22, 1, 2048 },      // 0 5601
	{ 0, 22, 1, 2048 },      // 1 5601 hangs, legs swaying
	{ 23, 54, 0, 2048 },     // 2 5602 grabs (attack 60, frames 28..31)
	{ 55, 69, 0, 1024 },     // 3 5603
	{ 87, 110, 1, 2048 },    // 4 5605 holds James (attack 61)
	{ 111, 125, 0, 2048 },   // 5 5606 lets go
	{ 71, 86, 0, 2048 },     // 6 5604
	{ 466, 493, 0, 2048 },   // 7 5619 kicks (attack 62, frames 11..14)
	{ 126, 144, 0, 2048 },   // 8.. flinches (0x4b8360)
	{ 145, 163, 0, 2048 }, { 164, 182, 0, 2048 }, { 183, 201, 0, 2048 }, { 202, 234, 0, 2048 },
	{ 235, 267, 0, 2048 }, { 268, 300, 0, 2048 }, { 301, 333, 0, 2048 }, { 334, 366, 0, 2048 },
	{ 367, 399, 0, 2048 }, { 400, 432, 0, 2048 }, { 433, 465, 0, 2048 },
	{ 494, 516, 0, 2048 },   // 20 5620 collapses
};

static const float IKE_DROP[7][3] = {
	{ -141100, -20400, 1.571f }, { -141100, -19800, 1.571f }, { -140200, -18500, 3.142f },
	{ -139600, -18500, 3.142f }, { -138900, -19600, -1.571f }, { -138900, -20400, -1.571f },
	{ -140000, -21500, 0.0f },
};

static float ike_speed(const Enemy* e)                         // 0x4b8600
{
	static const float t[5] = { 0.5f, 0.8f, 1.0f, 1.2f, 1.5f };
	return (hp_rate(e) * 0.002f + 0.8f) * t[s_diff] * 500.0f;
}

static int ike_others_alive(const Enemy* self)                 // 0x4b8100
{
	int n = 0;
	for (int i = 0; i < ENEMY_MAX; i++) {
		const Enemy* o = &s_en[i];
		if (o != self && o->used && o->kind == EN_KIND_IKE && o->mlv != IKE_DEATH && o->mlv != IKE_WAIT)
			n++;
	}
	return n;
}

static bool ike_other_grabbing(const Enemy* self)              // 0x4b8260
{
	for (int i = 0; i < ENEMY_MAX; i++) {
		const Enemy* o = &s_en[i];
		if (o != self && o->used && o->kind == EN_KIND_IKE && o->mlv == IKE_GRAB)
			return true;
	}
	return false;
}

static int ike_flinch_anim(const Enemy* e)                     // 0x4b8360
{
	int b = fabsf(blow_side(e)) <= PI / 2.0f;
	switch (e->last_atk) {
	case 1: case 2: return 8 + b;
	case 4: case 6: return 12 + b;
	case 0xc: case 0xd: case 0xe: case 0xf: case 0x10: case 0x11: case 0x15: case 0x16: return 16 + b;
	default: return 16;
	}
}

static void ike_update(Enemy* e, const EnPlayer* p, float dt)
{
	static const float meter[5] = { 0.5f, 0.5f, 1.0f, 2.0f, 2.0f };
	int ticks = (int)(dt * 60.0f + 0.5f);
	e->p_dist = hypotf(p->x - e->x, p->z - e->z);
	e->vx = e->vz = 0.0f;
	if (e->mlv >= IKE_WANDER && e->mlv <= IKE_KICK && check_damage(e)) {
		if (e->mlv == IKE_GRAB && combat_held()) reset_damage(e);   // no hits while it holds James
		else {
			e->attacking = false;
			if (e->mlv == IKE_GRAB) combat_grab_james(0.0f);
			go_raw(e, IKE_DAMAGE);
		}
	}
	switch (e->mlv) {
	case IKE_WAIT:
		reset_damage(e);
		if (e->slv == 0) { set_timer(e, 0x2a30); e->slv = 1; }
		if (ike_others_alive(e) == 0 || reduce_timer(e, ticks) <= 0) {
			// comes down at the drop point farthest from James that nobody stands near
			int best = 0;
			float bd = -1.0f;
			for (int k = 0; k < 7; k++) {
				float d = hypotf(IKE_DROP[k][0] - p->x, IKE_DROP[k][1] - p->z);
				if (d > bd) bd = d, best = k;
			}
			e->x = IKE_DROP[best][0], e->z = IKE_DROP[best][1], e->yaw = IKE_DROP[best][2];
			e->path_angle = e->yaw;
			go_raw(e, IKE_DROP_DOWN);
		}
		break;
	case IKE_DROP_DOWN:
		reset_damage(e);
		if (e->y >= -1640.0f) {
			e->y = -1640.0f;
			if (e->slv == 0) { set_timer(e, 60); e->slv = 1; }
			if (reduce_timer(e, ticks) <= 0) go_raw(e, IKE_WANDER);
		} else {
			e->y += 250.0f * dt;
		}
		break;
	case IKE_WANDER:
	case IKE_CHASE: {
		if (e->slv == 0) { anim_set(e, 1); e->path_angle = e->yaw; e->slv = 1; }
		float tx = e->mlv == IKE_CHASE ? p->x : e->x + sinf(e->path_angle) * 1000.0f;
		float tz = e->mlv == IKE_CHASE ? p->z : e->z + cosf(e->path_angle) * 1000.0f;
		walk_to(e, tx, tz, e->mlv == IKE_CHASE ? 0.0174533f : 0.0087266f, dt);
		edi_move(e, ike_speed(e));
		// 0x4b84e0: touching James grabs, a clear line within 1000 and 10 degrees kicks (normal
		// and up), beyond 2500 it loses him
		float gap = e->p_dist - 130.0f - e->size * 0.8f;
		bool busy = p->maria || combat_held() || !p->alive;
		if (gap < 50.0f && !busy && !ike_other_grabbing(e)) { go_raw(e, IKE_GRAB); break; }
		float a = fabsf(wrap(dir_to(e->x, e->z, p->x, p->z) - e->yaw));
		if (e->timer > 0) e->timer = e->timer > ticks ? e->timer - ticks : 0;
		if (e->timer == 0 && e->p_dist < 1000.0f && s_diff > 1 && !busy && a < 0.1745f && line_of_sight(e, p)) {
			go_raw(e, IKE_KICK);
			break;
		}
		go_raw(e, e->p_dist > 2500.0f ? IKE_WANDER : IKE_CHASE);
		e->slv = 1;
		break;
	}
	case IKE_GRAB:
		switch (e->slv) {
		case 0:
			anim_set(e, 2);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			e->slv = 1;
			break;
		case 1: {
			float f = frame_in(e);
			if (f >= 28.0f && f <= 31.99f && swing_hit(e, p, 60, 7.0f, 250.0f + 200.0f, 1.2f))
				combat_grab_james(meter[s_diff]);
			if (!e->anim_end) break;
			e->attacking = false;
			if (e->attack_hit && combat_held()) { anim_set(e, 4); set_timer(e, 120); e->slv = 2; }
			else { anim_set(e, 5); e->slv = 3; }
			break;
		}
		case 2:
			combat_hold_damage(0.2f, dt);
			if (reduce_timer(e, ticks) <= 0) {
				play_at(e, 18504 + (int)(rnd() * 1.99f));   // 0x4848: the gasp
				set_timer(e, 120);
			}
			// the hold killed him (DAT_01fbad17 3): it carries him off (state 8)
			if (combat_dying()) {
				e->aux[0] = p->x - e->x, e->aux[1] = p->y - e->y, e->aux[2] = p->z - e->z;
				go_raw(e, IKE_LIFT);
				break;
			}
			if (!combat_held()) { anim_set(e, 5); e->slv = 3; }
			break;
		case 3:
			if (e->anim_end) go_raw(e, IKE_CHASE);
			break;
		}
		break;
	case IKE_LIFT:
		// 0x4b7ff0: it turns square to the room (yaw 0 or pi, whichever is nearer, 0.5 degrees a
		// tick), slides along the rail to z -21450 at 500 a second (through the wall, flag 8 from
		// z -21300) and then rises at 500 until it
		// is above y -4000, where the game is over; James' body hangs from it all the way. The PC
		// also stops the other Flesh Lips attacking (byte 5 of every kind 7 cleared).
		reset_damage(e);
		if (e->slv == 0) { e->mark_angle = fabsf(e->yaw) >= PI / 2.0f ? PI : 0.0f; e->slv = 1; }
		approach_angle(&e->yaw, e->mark_angle, 0.0087266f * dt * 60.0f);
		if (e->slv == 1) {
			e->z -= 500.0f * dt;
			if (e->z < -21450.0f) { e->z = -21500.0f; e->slv = 2; }
		} else if (e->slv == 2) {
			e->y -= 500.0f * dt;
			if (e->y < -4000.0f) e->slv = 3;
		}
		combat_carry_james(e->slv < 3, e->x + e->aux[0], e->y + e->aux[1], e->z + e->aux[2]);
		break;
	case IKE_KICK:
		if (e->slv == 0) {
			anim_set(e, 7);
			e->attacking = true;
			e->attack_hit = e->maria_hit = false;
			e->slv = 1;
		}
		{
			float f = frame_in(e);
			if (f >= 11.0f && f <= 14.99f) swing_hit(e, p, 62, 17.0f, 250.0f + 300.0f, 0.9f);
		}
		if (e->anim_end) {
			// a breather of 1.5 s before the next kick (the port's; the PC's kick test also asks
			// its wall probe 0x4aa800, which was not worked out)
			e->attacking = false;
			set_timer(e, 90);
			go_raw(e, IKE_CHASE);
		}
		break;
	case IKE_DAMAGE:
		if (e->slv == 0) {
			reduce_hp(e);
			anim_set(e, ike_flinch_anim(e));
			e->slv = 1;
		}
		if (check_damage(e)) reduce_hp(e);
		if (!e->anim_end) break;
		go_raw(e, is_dead(e) ? IKE_DEATH : IKE_CHASE);
		break;
	case IKE_DEATH:
		reset_damage(e);
		if (e->slv == 0) {
			anim_set(e, 20);
			e->dead_flag = true;
			mark_killed(e);
			e->slv = 1;
		}
		if (e->slv == 1 && e->anim_end) {
			bool last = true;
			for (int i = 0; i < ENEMY_MAX; i++)
				if (&s_en[i] != e && s_en[i].used && s_en[i].kind == EN_KIND_IKE && s_en[i].mlv != IKE_DEATH)
					last = false;
			set_timer(e, 10);
			e->slv = last ? 2 : 3;
		}
		if (e->slv == 2 && reduce_timer(e, ticks) <= 0) {
			ev_flag_set(210, true);
			e->slv = 3;
		}
		break;
	}
	if (e->mlv != IKE_WAIT && e->mlv != IKE_DROP_DOWN) {
		float adv = anim_step(e, dt);
		// it glides along the ceiling rails: walls only, no floor to follow
		float nx = e->x + e->vx * dt, nz = e->z + e->vz * dt;
		(void)adv;
		// the lift ignores the walls (the PC sets flag 8 past z -21300) to reach the rail's end
		if (s_col && e->mlv != IKE_LIFT) col_push(s_col, &nx, &nz, 0.0f, 2000.0f, e->size);
		e->x = nx, e->z = nz;
	}
	// a creak now and then (0x4b8690, 0x4850..0x4855 every second at the most)
	e->sound_wait += dt * 60.0f;
	if (e->sound_wait >= 60.0f && e->mlv != IKE_DEATH && e->mlv != IKE_WAIT) {
		e->sound_wait = rnd() * -60.0f;
		play_at(e, 18512 + (int)(rnd() * 5.99f));
	}
}

// 0x4b8700: health {500, 2000, 3000, 5000} with the usual spread; the one hung above -2140 waits.
static void ike_init(Enemy* e, int status)
{
	static const float hp[5] = { 500, 2000, 3000, 5000, 8000 };
	(void)status;
	e->ctrl = 1;
	e->path_angle = e->mark_angle = e->yaw;
	set_size(e, 150.0f, 710.0f, true);
	set_hp(e, hp[s_diff], hp[s_diff]);
	anim_set(e, 1);
	e->mlv = e->y < -2140.0f ? IKE_WAIT : IKE_DROP_DOWN;
}

// Enemies the stage programs create (FUN_0048fd40 / CharaCreate after their demos) while the story
// flags say they are there; removed again when the flags move on (CharaRemove / CharaDelete).
typedef struct { s8 stage; s16 on, off; u8 model, status; float x, y, z, yaw; } Script;
static const Script SCRIPTS[] = {
	{ 12, 143, 146, MDL_RED, 6, 99846.148f, -2202.74f, -100531.953f, -1.374f },     // EvProgApartBoss 0x596220
	{ 21, 201, 203, MDL_RED, 15, 18800.0f, 0.0f, -23600.0f, -1.5707964f },         // EvProgTriheadEntry 0x58cea0
	{ 22, 521, 210, MDL_IKE, 0, -140000.0f, -1650.0f, -19450.0f, 3.142f },          // EvProgLauraDeceive3rd 0x58b750
	{ 22, 521, 210, MDL_IKE, 0, -140500.0f, -1600.0f, -21400.0f, 0.0f },
	{ 22, 521, 210, MDL_IKE, 0, -139600.0f, -3200.0f, -20400.0f, 0.0f },
	{ 27, 243, 248, MDL_ONI, 15, 19600.0f, 0.0f, -260400.0f, -1.5707964f },        // EvProgChaseInHospital1st 0x587db0
	{ 35, 333, 334, MDL_PAP, 6, 20191.404f, 0.0f, 61010.0f, -0.785398f },          // EvProgAngelaAttackPapa 0x580e30
	{ 44, 2241, 483, MDL_ONI, 6, -21322.65f, -25.0f, 22699.85f, 2.7489f },          // EvProgTriheadBattle 0x576050
	{ 44, 2241, 483, MDL_ONI, 6, -18625.65f, -25.0f, 22746.05f, -2.7489f },
	{ 37, 375, 373, MDL_EDI, 0, 99969.0f, 0.0f, -99538.1f, 1.676f },               // EvProgEddieMurderer1st 0x57ef10
	{ 37, 377, 378, MDL_EDI, 6, 140025.45f, 0.0f, -101195.55f, 2.742f },           // EvProgEddieMurderer3rd 0x57f150
	{ 48, 505, 506, MDL_BOS, 0, 57098.6f, -16920.7f, 57825.5f, 0.0f },             // EvProgMaryBeforeBattle
	{ 49, 505, 506, MDL_BOS, 0, 57098.6f, -16920.7f, 57825.5f, 0.0f },
	{ 50, 505, 506, MDL_BOS, 0, 57098.6f, -16920.7f, 57825.5f, 0.0f },
	{ 51, 505, 506, MDL_BOS, 0, 57098.6f, -16920.7f, 57825.5f, 0.0f },
	{ 48, 507, 508, MDL_BOS, 13, 57098.6f, -16005.0f, 57825.5f, 0.0f },            // EvProgMaryFall
	{ 49, 507, 508, MDL_BOS, 13, 57098.6f, -16005.0f, 57825.5f, 0.0f },
	{ 50, 507, 508, MDL_BOS, 13, 57098.6f, -16005.0f, 57825.5f, 0.0f },
	{ 51, 507, 508, MDL_BOS, 13, 57098.6f, -16005.0f, 57825.5f, 0.0f },
};
#define SCRIPT_ID(i) (-2 - (i))

static void script_spawn(void)
{
	int first_oni = -1;
	for (u32 i = 0; i < sizeof SCRIPTS / sizeof SCRIPTS[0]; i++) {
		const Script* c = &SCRIPTS[i];
		bool want = s_stage == c->stage && ev_flag(c->on) && !ev_flag(c->off);
		int have = -1, free_slot = -1;
		for (int k = 0; k < ENEMY_MAX; k++) {
			if (s_en[k].used && s_en[k].list_id == SCRIPT_ID((int)i)) have = k;
			if (!s_en[k].used && free_slot < 0) free_slot = k;
		}
		if (have >= 0) {
			if (!want) s_en[have].used = false;
			else if (s_en[have].kind == EN_KIND_ONI && s_en[have].type == 2) {
				// the hotel pair know each other (+0xb0); the second takes the other suicide spot
				if (first_oni < 0) first_oni = have;
				else {
					s_en[have].ival2 = first_oni, s_en[first_oni].ival2 = have;
					s_en[have].amb_n = 1;
				}
			}
			continue;
		}
		float fy;
		if (!want || free_slot < 0 || !s_col || !col_floor(s_col, c->x, c->z, c->y, 3000.0f, &fy) || !load_model(c->model))
			continue;
		Enemy* e = &s_en[free_slot];
		memset(e, 0, sizeof *e);
		e->used = true;
		e->kind = model_kind(c->model);
		e->model = c->model;
		e->list_id = SCRIPT_ID((int)i);   // scripted: never in the killed bits
		e->status = c->status;
		e->x = c->x, e->y = c->model == MDL_IKE ? c->y : fy, e->z = c->z;   // Flesh Lips hang
		e->yaw = c->yaw;
		e->amb = -1;
		kind_init(e, c->status);
	}
}

// ---------------------------------------------------------------- spawning

// Enemy_List condition (0x48f5b0): bits 0..2 skip the enemy on easy / normal / hard; bits 6..15
// pick a story test on game flags (0x1f7d5e0 + n/8) or other state.
static bool condition_ok(int cond)
{
	if ((s_diff <= 1 && (cond & 1)) || (s_diff == 2 && (cond & 2)) || (s_diff == 3 && (cond & 4)))
		return false;
	switch (cond & 0xffc0) {
	case 0x000: return true;
	case 0x080: return ev_flag(67);
	case 0x100: return ev_flag(91);
	case 0x140: return ev_flag(146);
	case 0x180: return ev_flag(68) && !ev_flag(70);
	case 0x1c0: return !ev_flag(68) || ev_flag(70);
	case 0x200: return !(ev_flag(68) && !ev_flag(70));   // also needs the flashlight (always on here)
	case 0x380: return ev_flag(251) && !ev_flag(619);    // the town after the hospital (xoo nurses)
	case 0x3c0: return !ev_flag(482);
	case 0x040: return ev_flag(43) && !ev_flag(251);
	case 0x0c0: return ev_flag(483);                     // the hotel Pyramid Heads' bodies
	case 0x240: return ev_flag(335);                     // the Abstract Daddy's body
	case 0x280: return ev_flag(365);                     // (and James past an x the port ignores)
	case 0x2c0: return !ev_flag(329) || ev_flag(330);
	case 0x300: return ev_flag(154) && !ev_flag(251);
	case 0x400: return ev_flag(154) && (!ev_flag(156) || ev_flag(157)) && !ev_flag(162);
	case 0x440: return ev_flag(154) && !ev_flag(162);
	case 0x480: return !ev_flag(99) && !ev_flag(101);
	case 0x4c0: return ev_flag(395);
	case 0x500: return ev_flag(413);
	case 0x540: return ev_flag(446);
	case 0x5c0: return ev_flag(711);
	case 0x600: return ev_flag(720);
	default: return true;                                  // other tests are not ported yet
	}
}

// Loads a kind's model and animation the first time it is needed (kept until enemy_exit).
static bool load_model(int m)
{
	if (s_mdl[m].tried)
		return s_mdl[m].loaded;
	s_mdl[m].tried = true;
	const char* name = MODEL_NAME[m];
	const char* file = MODEL_FILE[m] ? MODEL_FILE[m] : name;
	Character* c = &s_mdl[m].chr;
	Anim* a = &s_mdl[m].anim;
	char dir[96], err[128], path[128];
	snprintf(dir, sizeof dir, "%s/chr/%s", s_pack, name);
	if (!chr_load(c, dir, file, err, sizeof err))
		return false;
	snprintf(path, sizeof path, "%s/%s.s3a", dir, file);
	if (m == MDL_TY2 || m == MDL_TY3) {
		memset(a, 0, sizeof *a);        // a still prop: drawn in its bind pose
	} else if (!anim_load(a, path) || a->bones != c->n_bones) {
		chr_free(c);
		return false;
	}
	s_mdl[m].loaded = true;
	return true;
}

// 0x4c46c0 enSCUInitData
static void scu_init(Enemy* e, int status)
{
	static const float vitality[5] = { 400, 400, 700, 1500, 9999 };
	static const float endurance[5] = { 10, 190, 285, 380, 450 };
	e->ctrl = 1;
	e->mlv = SCU_WANDER;
	e->slv = 0;
	e->path_angle = e->mark_angle = e->yaw;
	e->type = glb_set() ? 6 : 3;      // 0x4a5860: the buildings can ambush, elsewhere it stays near home
	e->amb = -1;
	size_upright(e, true);
	anim_set(e, A_WALK);
	e->stx = e->x, e->stz = e->z;
	float scale = 1.0f;
	switch (status) {
	case 1: scale = 1.2f; break;
	case 2: scale = 0.5f; break;
	case 3: case 4:
		if (status == 4) e->type = 0xff;
		e->mlv = SCU_DOWN;
		anim_set(e, A_LIE);
		size_lying(e, true);
		scale = 0.3f;
		break;
	case 5:
		e->mlv = SCU_DYING;
		e->hp = 0.0f;
		anim_set(e, A_LIE + e->lie);
		size_lying(e, true);
		return;
	case 6: e->type &= ~2; break;
	case 7: e->type |= 0x10; e->mlv = SCU_CRAWL; break;
	case 8: e->type |= 0x18; e->mlv = SCU_WAITCAR; anim_set(e, A_LIE); size_lying(e, true); break;
	case 0x12: e->type |= 0x20; e->type |= 0x10; e->mlv = SCU_CRAWL; break;
	default: break;
	}
	set_hp(e, scale * vitality[s_diff], scale * endurance[s_diff]);
	e->life = rnd() * 10.0f + 3.0f;
}

void enemy_set_difficulty(int level) { s_diff = level < 0 ? 0 : level > 4 ? 4 : level; }
int enemy_difficulty(void) { return s_diff; }

float enemy_nearest(float x, float z)
{
	float best = 1e9f;
	for (int i = 0; i < ENEMY_MAX; i++) {
		const Enemy* e = &s_en[i];
		if (enemy_alive(e) && e->kind != EN_KIND_RED) {
			float d = hypotf(e->x - x, e->z - z);
			if (d < best) best = d;
		}
	}
	return best;
}

void enemy_init(const char* pack)
{
	snprintf(s_pack, sizeof s_pack, "%s", pack);
	maria_init(pack);
	char path[128];
	// debug.txt "action <0..3>": the action level (beginner, easy, normal, hard) until the options
	// menu exists
	snprintf(path, sizeof path, "%s/debug.txt", pack);
	FILE* d = fopen(path, "r");
	if (d) {
		char line[128];
		int v;
		int kind, x, z, deg, st;
		while (fgets(line, sizeof line, d)) {
			if (sscanf(line, "action %d", &v) == 1)
				enemy_set_difficulty(v);
			// "enemy <kind hex> <x> <z> <yaw degrees> [status [y]]": an extra enemy in whatever room
			// has a floor there (testing; y places a wall or ceiling creeper)
			st = 0;
			int dy = 0;
			if (sscanf(line, "enemy %x %d %d %d %d %d", &kind, &x, &z, &deg, &st, &dy) >= 4 && s_ndebug < 4) {
				EnListEntry* l = &s_debug[s_ndebug++];
				l->stage = -1, l->kind = kind, l->id = -1, l->x = x, l->y = dy, l->z = z;
				l->rot = (int)(deg * 3.14159265f / 180.0f * 4096.0f), l->status = st, l->cond = 0;
			}
		}
		fclose(d);
	}
	snprintf(path, sizeof path, "%s/events/enemies.txt", pack);
	FILE* f = fopen(path, "r");
	s_nlist = 0;
	if (!f)
		return;
	int st, kind, id, x, y, z, rot, status, cond;
	while (s_nlist < MAX_LIST &&
	       fscanf(f, "%d %x %d %d %d %d %d %d %x", &st, &kind, &id, &x, &y, &z, &rot, &status, &cond) == 9) {
		EnListEntry* l = &s_list[s_nlist++];
		l->stage = st, l->kind = kind, l->id = id, l->x = x, l->y = y, l->z = z;
		l->rot = rot, l->status = status, l->cond = cond;
	}
	fclose(f);
}

void enemy_exit(void)
{
	enemy_clear();
	maria_exit();
	if (s_moth_ok)
		chr_free(&s_moth_chr);
	s_moth_ok = s_moth_tried = false;
	s_moths.on = false;
	for (int m = 0; m < MDL_COUNT; m++) {
		if (s_mdl[m].loaded) {
			chr_free(&s_mdl[m].chr);
			anim_free(&s_mdl[m].anim);
		}
		s_mdl[m].loaded = s_mdl[m].tried = false;
	}
}

void enemy_clear(void)
{
	memset(s_en, 0, sizeof s_en);
	memset(s_comm, 0, sizeof s_comm);
	s_col = NULL;
	s_moths.on = false;
}

// Spawns the stage's Enemy_List rows that stand on the loaded collision and are not there yet
// (debug rows are tracked by ids below -99).
static void spawn_list(int stage, const Collision* col)
{
	for (int i = 0; i < s_nlist + s_ndebug; i++) {
		const EnListEntry* l = i < s_nlist ? &s_list[i] : &s_debug[i - s_nlist];
		int id = i < s_nlist ? l->id : -100 - (i - s_nlist);
		int kind = l->kind & 0xffff;
		int m = kind == 0x200 ? MDL_SCU : kind == 0x201 ? MDL_MKN : kind == 0x202 ? MDL_TYU
		      : kind == 0x207 ? MDL_NSE : kind == 0x20b ? MDL_XOO : kind == 0x208 ? MDL_RED
		      : kind == 0x204 ? MDL_PAP : kind == 0x209 ? MDL_ONI : kind == 0x206 ? MDL_BOS : kind == 0x20a ? MDL_ARM
		      : kind == 0x20c ? MDL_TY2 : kind == 0x20d ? MDL_TY3 : -1;
		if ((l->stage != stage && l->stage != -1) || m < 0)
			continue;
		if (id >= 0 && id < MAX_LIST && (s_killed[id >> 5] >> (id & 31) & 1))
			continue;
		if (!condition_ok(l->cond))
			continue;
		int slot = -1;
		bool present = false;
		for (int k = 0; k < ENEMY_MAX; k++) {
			if (s_en[k].used && s_en[k].list_id == id) present = true;
			if (!s_en[k].used && slot < 0) slot = k;
		}
		if (present || slot < 0)
			continue;
		float fy;
		if (!col_floor(col, (float)l->x, (float)l->z, (float)l->y, 3000.0f, &fy))
			continue;                  // not in the loaded rooms
		if (!load_model(m))
			continue;
		Enemy* e = &s_en[slot];
		memset(e, 0, sizeof *e);
		e->used = true;
		e->kind = model_kind(m);
		e->model = (u8)m;
		e->list_id = id;
		e->amb = -1;
		int status = id >= 0 && id < MAX_LIST && s_regen[id] ? s_regen[id] - 1 : l->status;
		e->status = status;
		e->x = (float)l->x, e->y = fy, e->z = (float)l->z;
		e->yaw = wrap(l->rot / 4096.0f);   // Enemy_List rot_y: 4.12 fixed-point radians
		if (m == MDL_TY2 || m == MDL_TY3 || m == MDL_TYU) e->aux[0] = (float)l->y;   // walls and ceilings keep their height
		switch (e->kind) {
		case EN_KIND_MKN: mkn_init(e, status); break;
		case EN_KIND_NSE: nse_init(e, status, kind == 0x20b); break;   // 0x20b: chr/xoo, same code
		case EN_KIND_TYU: tyu_init(e, status); break;
		default: kind_init(e, status); break;
		}
	}
}

void enemy_room_enter(int stage, const Collision* col)
{
	enemy_clear();
	s_col = col;
	s_stage = stage;
	maria_room_enter(col);
	if (col)
		spawn_list(stage, col);
}

// Streamed open areas: the merged collision changed under the same pointer. Enemies whose floor
// left it are dropped (they come back from the list when their tile returns, alive unless they
// were killed); rows whose tile came in are spawned; the ones that stay keep going.
void enemy_stream_update(int stage, const Collision* col)
{
	s_col = col;
	s_stage = stage;
	if (!col)
		return;
	for (int k = 0; k < ENEMY_MAX; k++) {
		Enemy* e = &s_en[k];
		float fy;
		if (e->used && e->kind != EN_KIND_BOS && e->kind != EN_KIND_IKE &&
		    !col_floor(col, e->x, e->z, e->y, 3000.0f, &fy))
			e->used = false;
	}
	spawn_list(stage, col);
}

void enemy_update(const EnPlayer* p, float dt)
{
	s_px = p->x, s_pz = p->z;
	moths_update(dt);
	for (int i = 0; i < 8; i++)
		if (s_comm[i].time > 0)
			s_comm[i].time -= (int)(dt * 60.0f + 0.5f);
	EnPlayer mp;
	bool with_maria = maria_target(p, &mp);
	for (int i = 0; i < ENEMY_MAX; i++) {
		Enemy* e = &s_en[i];
		const EnPlayer* t = p;
		if (with_maria && e->used && e->kind != EN_KIND_RED &&
		    hypotf(mp.x - e->x, mp.z - e->z) < hypotf(p->x - e->x, p->z - e->z))
			t = &mp;                    // 0x4a5b20 / 0x542ad0: the closer of the two
		if (!e->used)
			continue;
		switch (e->kind) {
		case EN_KIND_MKN: mkn_update(e, t, dt); break;
		case EN_KIND_NSE: nse_update(e, t, dt); break;
		case EN_KIND_TYU: tyu_update(e, t, dt); break;
		case EN_KIND_RED: red_update(e, t, dt); break;
		case EN_KIND_PAP: pap_update(e, t, dt); break;
		case EN_KIND_ONI: oni_update(e, t, p, dt); break;
		case EN_KIND_BOS: bos_update(e, p, dt); break;
		case EN_KIND_ARM: arm_update(e, t, dt); break;
		case EN_KIND_EDI: edi_update(e, p, dt); break;
		case EN_KIND_IKE: ike_update(e, t, dt); break;
		case EN_KIND_NEST: nest_update(e, p, dt); break;
		default: scu_update(e, t, dt); break;
		}
	}
	script_spawn();
	maria_update(p, dt);
}

void enemy_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	for (int i = 0; i < ENEMY_MAX; i++) {
		Enemy* e = &s_en[i];
		if (!e->used || !s_mdl[e->model].loaded)
			continue;
		Character* c = &s_mdl[e->model].chr;
		C3D_Mtx model, mv;
		Mtx_Identity(&model);
		Mtx_Translate(&model, e->x, e->y, e->z, true);
		if (e->kind == EN_KIND_TYU && e->mlv == TYU_WALL) {
			// belly on the wall; its heading in the wall's plane (0 straight down)
			Mtx_RotateY(&model, e->aux[0], true);
			Mtx_RotateX(&model, -(float)M_PI / 2.0f, true);
			Mtx_RotateY(&model, e->aux[1], true);
		} else if (e->kind == EN_KIND_TYU && e->mlv == TYU_CEILING) {
			Mtx_RotateY(&model, e->yaw + (float)M_PI, true);
			Mtx_RotateX(&model, (float)M_PI, true);      // upside down, facing where it runs
		} else if (e->kind == EN_KIND_NEST && e->type == 1) {
			// on a wall: its top (-y) turned to face into the room
			Mtx_RotateY(&model, e->mark_angle, true);
			Mtx_RotateX(&model, -(float)M_PI / 2.0f, true);
		} else if (e->kind == EN_KIND_NEST && e->type == 2) {
			Mtx_RotateY(&model, e->yaw, true);
			Mtx_RotateX(&model, (float)M_PI, true);      // on the ceiling, upside down
		} else {
			Mtx_RotateY(&model, e->yaw, true);
		}
		Mtx_Multiply(&mv, view, &model);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
		const ScuAnim* t = &anim_tab(e)[e->anim];
		float f = e->frame;
		if (t->loop && f > t->end)
			f = t->start;               // no blend across the loop seam
		if (e->kind == EN_KIND_NEST) {
			for (int b = 0; b < c->n_bones && b < CHR_MAX_BONES; b++) {
				memset(&c->skin[b], 0, sizeof c->skin[b]);
				c->skin[b].m[0][0] = c->skin[b].m[1][1] = c->skin[b].m[2][2] = 1.0f;
			}
		} else {
			chr_pose(c, &s_mdl[e->model].anim, f);
		}
		float at[3] = { e->x, e->y, e->z };
		chrlight_at(at, false);         // the PC's per-character light list (chrlight.c)
		chr_draw(c, uBones);
	}
	moths_draw(view, uModelView, uBones);
	maria_draw(view, uModelView, uBones);
}

int enemy_count(void) { return ENEMY_MAX; }
Enemy* enemy_get(int i) { return (i >= 0 && i < ENEMY_MAX && s_en[i].used) ? &s_en[i] : NULL; }

bool enemy_alive(const Enemy* e)
{
	if (e && e->kind == EN_KIND_EDI)
		return e->used && !e->dead_flag && e->mlv != EDI_FLEE;
	if (e && e->kind == EN_KIND_IKE)
		return e->used && !e->dead_flag && e->mlv != IKE_WAIT && e->mlv != IKE_DROP_DOWN;
	if (e && (e->kind == EN_KIND_PAP || e->kind == EN_KIND_BOS || e->kind == EN_KIND_ARM))
		return e->used && !e->dead_flag && (e->hp > 0.0f || e->kind == EN_KIND_BOS);
	if (e && e->kind == EN_KIND_ONI)
		return e->used && e->mlv != ONI_BODY && e->mlv != ONI_SUICIDE;
	if (e && e->kind == EN_KIND_RED)
		return e->used && e->ctrl == 1 && e->mlv != RED_LEAVE;   // can be hit, never dies
	int dying = !e ? 0 : e->kind == EN_KIND_MKN ? MKN_DYING : e->kind == EN_KIND_NSE ? NSE_DYING
	          : e->kind == EN_KIND_TYU ? TYU_DYING : SCU_DYING;
	return e && e->used && e->mlv != dying && e->hp > 0.0f;
}

bool enemy_is_down(const Enemy* e)
{
	if (e && e->kind == EN_KIND_MKN)
		return e->mlv == MKN_DOWN || (e->mlv == MKN_DAMAGE && e->slv == 2);
	if (e && e->kind == EN_KIND_NSE)
		return e->mlv == NSE_DOWN || (e->mlv == NSE_DAMAGE && e->slv == 2);
	// a creeper on the floor counts as down, so stomps reach it; not on a wall, the ceiling or falling
	if (e && e->kind == EN_KIND_TYU)
		return e->mlv != TYU_WALL && e->mlv != TYU_CEILING && !(e->mlv == TYU_DAMAGE && e->slv == 10);
	if (e && e->kind == EN_KIND_RED)
		return false;
	if (e && e->kind == EN_KIND_PAP)
		return e->mlv == PAP_DOWN || (e->mlv == PAP_DAMAGE && e->slv == 2);
	if (e && e->kind == EN_KIND_BOS)
		return e->mlv == BOS_DEAD && e->status == 13;   // lying after the fall
	if (e && (e->kind == EN_KIND_ONI || e->kind == EN_KIND_ARM || e->kind == EN_KIND_EDI || e->kind == EN_KIND_IKE))
		return false;
	return e && (e->mlv == SCU_DOWN || e->mlv == SCU_CRAWL || e->mlv == SCU_WAITCAR ||
	             (e->mlv == SCU_DAMAGE && e->slv == 2));
}

// Hit column (0x4a91f0 clSetCharaHitColumn): radius = size * 0.8 for kind 1 (0x892800).
void enemy_hit_cylinder(const Enemy* e, float* x, float* z, float* r, float* top, float* bottom)
{
	*x = e->x, *z = e->z;
	*r = e->size * 0.8f;
	*top = e->y - e->tall;
	*bottom = e->y + 20.0f;
	if (e->kind == EN_KIND_BOS && e->status != 13) {
		*top = e->y - 785.0f;          // she hangs in the air (bos.mdl spans -785..401)
		*bottom = e->y + 401.0f;
		return;
	}
	if (e->kind == EN_KIND_IKE) {
		*top = e->y - 31.0f;           // hangs from the rails: ike.mdl spans 31 above to 832 below
		*bottom = e->y + 832.0f;
		return;
	}
	if (e->kind == EN_KIND_ARM) {
		*top = e->y - 100.0f;          // under the floor grate
		*bottom = e->y + 900.0f;
		return;
	}
	if (enemy_is_down(e) && e->kind != EN_KIND_TYU) {
		// lying bodies stretch along their facing: centre the column on the torso
		*x = e->x + sinf(e->yaw) * 250.0f;
		*z = e->z + cosf(e->yaw) * 250.0f;
		*r = 330.0f;
	}
}

void enemy_damage(Enemy* e, int attack_id, float ap, float sp, float dir_x, float dir_z)
{
	if (!enemy_alive(e))
		return;
	if (attack_id >= 31 && attack_id <= 40 && !enemy_is_down(e))
		return;          // stomps only count on a body on the floor (0x4a8d80)
	play_at(e, e->kind == EN_KIND_MKN ? 12503 + (int)(rnd() * 1.99f)
	         : e->kind == EN_KIND_NSE ? 12113 + (int)(rnd() * 3.99f)
	         : e->kind == EN_KIND_TYU ? 12208 + (int)(rnd() * 5.99f)
	         : e->kind == EN_KIND_RED || e->kind == EN_KIND_ONI ? 16110
	         : e->kind == EN_KIND_PAP ? 18301 : e->kind == EN_KIND_ARM ? 12255
	         : e->kind == EN_KIND_BOS ? 18863 : 12017 + (int)(rnd() * 1.99f));
	e->damage += ap;
	e->shock = sp;
	e->last_atk = attack_id;
	float n = sqrtf(dir_x * dir_x + dir_z * dir_z);
	if (n > 1e-3f) {
		e->hbx = dir_x / n;
		e->hbz = dir_z / n;
	}
}

const char* enemy_status(void)
{
	static char line[48];
	int n = 0, first = -1;
	for (int i = 0; i < ENEMY_MAX; i++)
		if (s_en[i].used) {
			if (first < 0 || s_en[i].p_dist < s_en[first].p_dist) first = i;
			n++;
		}
	if (first < 0) {
		snprintf(line, sizeof line, "%snone (%d listed)", maria_status(), s_nlist);
		return line;
	}
	const Enemy* e = &s_en[first];
	if (e->kind == EN_KIND_ONI && e->type == 2) {
		// the hotel pair: both states, nearest first (their teamwork is the point of watching)
		const Enemy* o = oni_partner(e);
		if (o) {
			snprintf(line, sizeof line, "%s%d s%d.%d|s%d.%d d%.0f|%.0f", maria_status(), n, e->mlv, e->slv,
			         o->mlv, o->slv, e->p_dist, o->p_dist);
			return line;
		}
	}
	snprintf(line, sizeof line, "%s%d s%d.%d a%d hp%.0f d%.0f", maria_status(), n, e->mlv, e->slv, e->anim,
	         e->hp, e->p_dist);
	return line;
}
