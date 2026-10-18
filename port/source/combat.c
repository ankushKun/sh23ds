// James' health, his weapons (wooden plank, handgun, shotgun, hunting rifle, bare hands) and combat.
//
// Sources (sh2pc.exe, Director's Cut):
//   0x88d2f0  weapon file table, 20-byte rows {flags, anm, wp mdl, rwp mdl, kg1}; the row number is
//             gWeaponHandGrip: 0 none, 1 handgun, 2 shotgun, 3 rifle, 4 hyper spray, 5 wooden plank,
//             6 steel pipe, 7 chainsaw, 8 great knife (9, 10 Maria's in the DC).
//   0x534240  grip -> item: 1 -> 4 (handgun), 2 -> 6, 3 -> 8, 4 -> 12, 5 -> 13 (plank), 6 -> 14 ...
//   0x5569b0  attack request: the attack id per grip (handgun 1/2, reload 3; shotgun 4, reload 5;
//             rifle 6, reload 7; plank 12/13/14; kicks and stomps 31.. by kick type).
//   0x52fae0 / 0x531a00  upper/lower attack: animation entries per grip, e.g. the plank's 351..382
//             (table 0x6ceeb0), the handgun's 201..210 (0x6cec50), shotgun 251..266 (0x6cecd8), rifle
//             301..304 (0x6ceda4), base moves 101..133 (0x6ceac4).
//   0x558bd0  reload: the anime and the frame at which the rounds go in (handgun 209/210 frame 21,
//             shotgun 260 frame 12, rifle 304 frame 17).
//   0x543600  gunshot: one ray from the muzzle along the aim, min to max range of the attack; 0x543810
//             shotgun: 10 rays in a 30 degree cone (0x6a91c0), each a full hit.
//   0x6ec568  sh2_attack_list (the copy the battle code reads): ap, sp, ranges, frame windows.
//   Every weapon model shares its root bind matrix with James' bone 35 (right hand); bone 1 of the
//   guns is the muzzle (handgun 125, shotgun 294, rifle 483 units out), the plank's second bone is its
//   grip offset (frame 4 of wp/jms_weapon.anm). They are drawn rigidly on bone 35.
#include "combat.h"
#include "controls.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "demo.h"
#include "events.h"
#include "maria.h"
#include "prog.h"
#include "sfx.h"

#define PI 3.14159265f

typedef struct { u16 start, end; u8 loop; s16 speed; } JAnim;

// pjames base table (start, end, loop, speed; speed 2048 = 30 frames/s, negative plays backwards)
static const JAnim J_STAND = { 1, 10, 1, 256 };       // 101
static const JAnim J_WALK = { 51, 74, 1, 1408 };      // 104
static const JAnim J_RUN = { 147, 170, 1, 2048 };     // 108
static const JAnim J_HURT = { 453, 470, 0, 1024 };    // 121
static const JAnim J_FALL = { 256, 280, 0, 1024 };    // 112
// Lower-body state 0x1d (0x531a00) plays 132 for a kick (request 6) and 133 for a stomp (request 7).
// Both start and end on the standing pose: in 132 the right foot swings up and forward at knee
// height, in 133 it comes down on the floor in front of James.
static const JAnim J_KICK = { 682, 701, 0, 768 };     // 132 (hit frames 7..10)
static const JAnim J_STOMP = { 702, 719, 0, 512 };    // 133 (hit frames 3..9)

// Plank (grip 5)
static const JAnim P_RAISE[2] = { { 722, 733, 0, 2048 }, { 734, 745, 0, 2048 } };     // 351, 353
static const JAnim P_LOWER[2] = { { 722, 733, 0, -1794 }, { 734, 745, 0, -1794 } };   // 352, 354
static const JAnim P_READY[2] = { { 746, 755, 1, 320 }, { 756, 765, 1, 320 } };       // 359, 360
static const JAnim P_SWING[2] = { { 776, 797, 0, 1536 }, { 810, 831, 0, 1536 } };     // 363, 369
static const JAnim P_COMBO[2] = { { 780, 797, 0, 1536 }, { 814, 831, 0, 1536 } };     // 365, 371
static const JAnim P_REBOUND[2] = { { 776, 797, 0, -1024 }, { 810, 831, 0, -1024 } }; // 364, 370
// The plank and Maria's knife (grip 10, table 0x6cf454, 21201..21232) share this layout except
// for the follow-through and the downswing, which start two frames earlier with the knife.
static const JAnim P_RECOVERS[2][2] = {
	{ { 800, 809, 0, 1024 }, { 834, 843, 0, 1536 } },       // 367, 373
	{ { 798, 809, 0, 1024 }, { 832, 843, 0, 1536 } },       // 21217, 21223
};
static const JAnim P_DOWNSWINGS[2][2] = {
	{ { 846, 873, 0, 1024 }, { 876, 903, 0, 1024 } },       // 375, 379
	{ { 844, 873, 0, 1024 }, { 874, 903, 0, 1024 } },       // 21225, 21229
};
#define P_RECOVER (P_RECOVERS[s_maria])
#define P_DOWNSWING (P_DOWNSWINGS[s_maria])

typedef struct { int id; float ap, sp, min_range, max_range; u8 first, last; } AttackInfo;
// Swing, short swing and downswing: the plank's attacks 12..14 and the knife's 28..30 (same frame
// windows, less power, shorter reach and no push).
static const AttackInfo ATK_MELEE[2][3] = {
	{ { 12, 100.0f, 80.0f, -150.0f, 350.0f, 12, 15 }, { 13, 80.0f, 80.0f, -150.0f, 350.0f, 8, 11 },
	  { 14, 140.0f, 80.0f, -150.0f, 350.0f, 8, 10 } },
	{ { 28, 60.0f, 0.0f, -100.0f, 250.0f, 12, 15 }, { 29, 35.0f, 0.0f, -100.0f, 250.0f, 8, 11 },
	  { 30, 60.0f, 0.0f, -100.0f, 250.0f, 8, 10 } },
};
#define ATK_SWING (ATK_MELEE[s_maria][0])
#define ATK_COMBO (ATK_MELEE[s_maria][1])
#define ATK_DOWN (ATK_MELEE[s_maria][2])
// Kicks are attacks 31..35 and stomps 36..40 (0x5569b0 adds 0x1f or 0x24 to DAT_01fbacb4); every
// one is a kind 1 finish with the same power, only the frame window differs.
static const AttackInfo ATK_KICK = { 31, 9999.9f, 0.1f, 0.0f, 350.0f, 7, 10 };
static const AttackInfo ATK_STOMP = { 36, 9999.9f, 0.1f, 0.0f, 350.0f, 3, 9 };

// Enhanced Edition "EnableHoldToStomp" (Patches/HoldToStomp.cpp, zlib licence) and
// "SwapLightHeavyAttack" (Patches/SwapLightHeavyAttack.cpp, zlib licence), both by Murugo and on by
// default there. The PC picks kick or stomp (0x534210) and light or heavy melee (0x533ff0) from the
// pressure of the attack button, more than 0xc0 meaning the hard one; a digital button always reads
// 0xff, so on the PC every press stomps and every first plank swing is the heavy downswing. The EE
// times the press instead: released within 0.2 s is the light move (kick, light swing), held for
// 0.2 s the heavy one (stomp, downswing).
#define HOLD_SECONDS 0.2f
static bool s_hold_to_stomp = true, s_swap_light_heavy = true;
static float s_charge = -1.0f;    // seconds A has been held for a pending tap/hold choice, -1 none
static bool s_charge_stomp;       // the pending choice is kick/stomp (else light/heavy swing)

// Weapons. Guns: raise / lower / aim hold / shot / reload entries of their pjames table, the attack,
// the clip (the rounds a box gives in ItemGet 0x49e310) and the SE ids.
// James has W_NONE..W_RIFLE and Maria (Born From a Wish) W_MNONE..W_KNIFE, the rows 10..12 of the
// PC's weapon file table (0x88d2f0).
enum { W_NONE, W_PLANK, W_HANDGUN, W_SHOTGUN, W_RIFLE, W_MNONE, W_COLT, W_KNIFE, W_COUNT };
typedef struct {
	const char* name;
	const char* anm;          // chr/<player dir>/<anm>.s3a
	const char* mdl;          // chr/<player weapon dir>/<mdl>.s3c (NULL: bare hands)
	int item, ammo, owned_marker, clip, grip;
	JAnim raise, lower, aim, shot, reload;
	u8 reload_frame, pellets;
	AttackInfo atk;
	int snd_shot, snd_empty, snd_reload;
} WeaponDef;

static const WeaponDef WEAPONS[W_COUNT] = {
	[W_NONE] = { "hands", "jms_wpnone", NULL, 0, 0, 0, 0, 0 },
	[W_PLANK] = { "plank", "jms_wpkaku", "wp_kakuzai", 13, 0, 0, 0, 5 },
	[W_HANDGUN] = { "handgun", "jms_wphand", "wp_handgun", 4, 5, 250, 10, 1,
		{ 744, 756, 0, 3584 }, { 744, 756, 0, -2560 }, { 771, 771, 0, 0 },     // 203, 204, end of 205
		{ 772, 786, 0, 1792 }, { 792, 831, 0, 1792 }, 21, 1,                  // 207, 209
		{ 2, 120.0f, 60.0f, -100.0f, 7500.0f, 0, 10 }, 11048, 11030, 11029 },
	[W_SHOTGUN] = { "shotgun", "jms_wpshot", "wp_shotgun", 6, 7, 251, 6, 2,
		{ 722, 736, 0, 2048 }, { 722, 736, 0, -1792 }, { 736, 736, 0, 0 },     // 251, 252
		{ 736, 786, 0, 1792 }, { 917, 956, 0, 1536 }, 12, 10,                 // 253, 260
		{ 4, 100.0f, 120.0f, -100.0f, 5000.0f, 0, 10 }, 11051, 11052, 11053 },
	[W_RIFLE] = { "rifle", "jms_wprifl", "wp_riflgun", 8, 9, 252, 4, 3,
		{ 722, 741, 0, 2048 }, { 722, 741, 0, -1792 }, { 741, 741, 0, 0 },     // 301, 302
		{ 741, 771, 0, 1280 }, { 772, 811, 0, 1536 }, 17, 1,                  // 303, 304
		{ 6, 500.0f, 150.0f, -350.0f, 9000.0f, 0, 10 }, 11048, 11049, 11050 },
	// Maria's revolver (grip 9, table 0x6cf3cc: 21103 raise, 21105 aim, 21107 shot, 21109 reload)
	// fires attack 26 with shot SE 0x2b14 (0x543778). Its reload fills it to 10 like the handgun
	// (ItemEventCheck 0x49e6a0) and plays 0x2b15 on frame 21 of 21109 (0x549500).
	[W_MNONE] = { "hands", "xmar_wpnone", NULL, 0, 0, 0, 0, 0 },
	[W_COLT] = { "revolver", "xmar_wpcolt", "wp_colt", 10, 11, 253, 10, 9,
		{ 744, 756, 0, 3584 }, { 744, 756, 0, -2560 }, { 771, 771, 0, 0 },
		{ 771, 786, 0, 1792 }, { 792, 831, 0, 1792 }, 21, 1,
		{ 26, 150.0f, 0.0f, 0.0f, 7500.0f, 0, 10 }, 11028, 11030, 11029 },
	[W_KNIFE] = { "knife", "xmar_wpknif", "wp_chinanife", 17, 0, 0, 0, 10 },
};

// The two player characters. Maria's ground speeds are James' scaled by her shorter stride in the
// same walk and run entries (measured from the planted foot in xmar_wpnone: two thirds of his when
// walking, 0.6 when running). Her sounds come from 0x549500 (James' 0x549e20): 0x4a61 when she dies,
// and for a blow one of 0x4285..0x4289, the takes the sound list calls "Maria injured". The PC picks
// the blow's take by damage state; the port cycles through them, as it does for James.
typedef struct {
	const char* dir;          // chr/<dir>: model and animation sets
	const char* wdir;         // chr/<wdir>: weapon models
	const char* model;
	int hand;                 // bone the weapon follows
	int first, last;          // her or his weapons, first is bare hands
	float speed[3];           // backwards, walk, run (units per second)
	int snd_hurt, n_hurt, snd_die;
} PlayerDef;

static const PlayerDef PLAYERS[2] = {
	{ "jms", "wp", "hll_jms", 35, W_NONE, W_RIFLE, { 450.0f, 975.0f, 1550.0f }, 11020, 4, 11024 },
	{ "xmar", "xmar", "lxx_mar", 32, W_MNONE, W_KNIFE, { 300.0f, 650.0f, 930.0f }, 17029, 5, 19041 },
};

enum { M_FREE, M_RAISE, M_READY, M_SWING, M_REBOUND, M_RECOVER, M_LOWER, M_SHOT, M_RELOAD, M_STOMP,
       M_HURT, M_HELD, M_DYING, M_DEAD };

static Character* s_james;       // main's player model: James, or Maria in Born From a Wish
static bool s_maria;              // the model and weapons are Maria's
static bool s_fresh;              // reset for a new game: the first update sets health and weapons
static Character s_wmodel;        // the weapon in James' hand
static bool s_wmodel_ok;
static Anim s_anim;               // James' animation set for the current weapon
static bool s_anim_ok;
static char s_pack[64];
static int s_weapon = W_PLANK;
static bool s_ok;
static float s_hp = 100.0f;
static float s_hp_recover;        // shPlayerWork.hp_recover: healing still to come, drained over time
static int s_mode;
static int s_side;                // plank: 0 over the right shoulder (359), 1 left (360)
static const JAnim* s_cur;
static float s_frame;
static bool s_end;
static const AttackInfo* s_atk;
static bool s_hit_any, s_fired, s_reloaded;
// 0x544d43: the ap of James' attack that struck the room instead of a character this frame (PC
// 0x1d82478, read by stage code through 0x544fd0: the well wall at the bottom of the hole)
static float s_wall_ap, s_wall_ap_next;
static bool s_swing_wall;
static bool s_hit[ENEMY_MAX];
static bool s_combo_req;
static const AttackInfo* s_stomp_atk;   // ATK_KICK or ATK_STOMP while M_STOMP
static float s_hold;                    // grab meter (DAT_01fbacd4): frame times of struggling left
static int s_stick_dir;                 // last circle pad direction (0 centre, 1..4), for flicks
static float s_invuln, s_dead_t, s_flash;
static float s_tip[3], s_root[3];       // plank or knife segment in James' bind space
static float s_muzzle[3];               // gun muzzle in James' bind space
static void* s_flash_vbo;               // linear memory, muzzle flash quads (S3C vertex layout)
static char s_line[48];
static const Collision* s_col;          // the room, for shots
static Enemy* s_target;                 // the locked target (0x1fbacc4), see find_target
// A Flesh Lip that kills James in its hold carries the body up into the dark (see
// combat_carry_james)
static bool s_carried;
static float s_carry[3];

// How a clip starts (0x531a00 / 0x52fae0 and the modes of 0x538920): standing, walking, raising,
// aiming and lowering crossfade over 8 frames (mode 4); swings, shots, reloads, kicks, stomps, hits
// and falls cut in (mode 2).
static float clip_blend(const JAnim* a)
{
	if (a == &J_STAND || a == &J_WALK || a == &J_RUN)
		return 8.0f / 30.0f;
	for (int i = 0; i < 2; i++)
		if (a == &P_RAISE[i] || a == &P_LOWER[i] || a == &P_READY[i])
			return 8.0f / 30.0f;
	for (int w = 0; w < W_COUNT; w++)
		if (a == &WEAPONS[w].raise || a == &WEAPONS[w].lower || a == &WEAPONS[w].aim)
			return 8.0f / 30.0f;
	return 0.0f;
}

static void play(const JAnim* a)
{
	s_cur = a;
	s_frame = a->speed < 0 ? a->end : a->start;
	s_end = false;
}

static void play_from(const JAnim* a, float frame)
{
	s_cur = a;
	s_frame = frame;
	s_end = false;
}

static void step(float dt)
{
	const JAnim* a = s_cur;
	if (!a || s_end)
		return;
	if (a->speed == 0) {
		s_end = true;
		return;
	}
	float len = (float)(a->end - a->start + 1);
	s_frame += dt * 30.0f * (a->speed / 2048.0f);
	if (a->loop) {
		while (s_frame >= a->end + 1.0f) s_frame -= len;
		while (s_frame < a->start) s_frame += len;
	} else if (s_frame >= a->end) {
		s_frame = a->end;
		s_end = a->speed > 0;
	} else if (s_frame <= a->start) {
		s_frame = a->start;
		s_end = a->speed < 0;
	}
}

static const WeaponDef* wdef(void) { return &WEAPONS[s_weapon]; }
static const PlayerDef* pdef(void) { return &PLAYERS[s_maria]; }
static bool is_gun(void) { return wdef()->clip > 0; }
static bool is_bare(void) { return s_weapon == W_NONE || s_weapon == W_MNONE; }
static bool is_melee(void) { return s_weapon == W_PLANK || s_weapon == W_KNIFE; }
static int bare(void) { return pdef()->first; }

// ---------------------------------------------------------------- ammo and ownership
//
// The PC keeps a possession bit and a count per item; for the guns the count is the rounds in the
// gun (0x49e620), the ammo items hold the spare rounds (ItemEventCheck). The port has one count per
// item, so an empty gun would drop out of the item list: a marker item (250..252, never shown) keeps
// it owned.

static int loaded(void) { return ev_item_count(wdef()->item); }
static int spare(void) { return ev_item_count(wdef()->ammo); }

static bool owns(int w)
{
	const WeaponDef* d = &WEAPONS[w];
	if (w < pdef()->first || w > pdef()->last)
		return false;         // the other character's
	if (w == W_NONE || w == W_PLANK || w == W_MNONE)
		return true;          // the plank is not handed out by the ported events yet
	if (ev_item_count(d->item) > 0 && ev_item_count(d->owned_marker) == 0)
		ev_give_item(d->owned_marker, 1);
	return ev_item_count(d->item) > 0 || ev_item_count(d->owned_marker) > 0;
}

bool combat_owns_item(int item)
{
	for (int w = W_HANDGUN; w < W_COUNT; w++)
		if (WEAPONS[w].item == item && WEAPONS[w].clip > 0)
			return owns(w);
	return false;
}

// ---------------------------------------------------------------- loading

// Bind-space position of a bone of a converted model (inverse of its inv_bind 3x4).
static void bind_pos(const Character* c, int bone, float out[3])
{
	const Mtx34* m = &c->inv_bind[bone];
	for (int i = 0; i < 3; i++)
		out[i] = -(m->m[0][i] * m->m[0][3] + m->m[1][i] * m->m[1][3] + m->m[2][i] * m->m[2][3]);
}

static void plank_segment(void)
{
	bind_pos(&s_wmodel, 0, s_root);
	float best = 0.0f;
	const u8* v = (const u8*)s_wmodel.vertices;
	for (u32 i = 0; i < s_wmodel.n_vertices; i++) {
		const float* p = (const float*)(v + i * CHR_VERTEX_SIZE);
		float dx = p[0] - s_root[0], dy = p[1] - s_root[1], dz = p[2] - s_root[2];
		float d = dx * dx + dy * dy + dz * dz;
		if (d > best) {
			best = d;
			memcpy(s_tip, p, sizeof s_tip);
		}
	}
}

static void unload_weapon(void)
{
	if (s_wmodel_ok || s_anim_ok)
		C3D_FrameSync();      // the GPU may still read the old weapon this frame
	if (s_wmodel_ok)
		chr_free(&s_wmodel);
	if (s_anim_ok)
		anim_free(&s_anim);
	s_wmodel_ok = s_anim_ok = false;
}

static bool load_weapon(int w)
{
	unload_weapon();
	const WeaponDef* d = &WEAPONS[w];
	char path[128], dir[96], err[128];
	snprintf(path, sizeof path, "%s/chr/%s/%s.s3a", s_pack, pdef()->dir, d->anm);
	s_anim_ok = anim_load(&s_anim, path) && s_anim.bones == s_james->n_bones;
	if (!s_anim_ok)
		return false;
	if (d->mdl) {
		snprintf(dir, sizeof dir, "%s/chr/%s", s_pack, pdef()->wdir);
		s_wmodel_ok = chr_load(&s_wmodel, dir, d->mdl, err, sizeof err);
		if (s_wmodel_ok) {
			if (w == W_PLANK || w == W_KNIFE)
				plank_segment();
			else if (s_wmodel.n_bones > 1)
				bind_pos(&s_wmodel, 1, s_muzzle);
		}
	}
	s_weapon = w;
	s_mode = M_FREE;
	play(&J_STAND);
	return true;
}

static void flash_init(void)
{
	// two crossed quads, 140 units across, around the origin; moved to the muzzle when drawn
	typedef struct { float x, y, z, u, v; u8 s[4], w[4]; s8 n[4]; } V;
	static const float q[12][3] = {
		{ -70, -70, 0 }, { 70, -70, 0 }, { 70, 70, 0 }, { -70, -70, 0 }, { 70, 70, 0 }, { -70, 70, 0 },
		{ 0, -70, -70 }, { 0, -70, 70 }, { 0, 70, 70 }, { 0, -70, -70 }, { 0, 70, 70 }, { 0, 70, -70 },
	};
	s_flash_vbo = linearAlloc(12 * sizeof(V));
	if (!s_flash_vbo)
		return;
	V* v = s_flash_vbo;
	for (int i = 0; i < 12; i++) {
		memset(&v[i], 0, sizeof v[i]);
		v[i].x = q[i][0], v[i].y = q[i][1], v[i].z = q[i][2];
		v[i].w[0] = 255;
		v[i].n[2] = 127;
	}
}

bool combat_init(const char* pack, Character* james)
{
	s_james = james;
	snprintf(s_pack, sizeof s_pack, "%s", pack);
	if (!load_weapon(W_PLANK) && !load_weapon(W_NONE))
		return false;
	flash_init();
	s_ok = true;
	combat_reset();
	return true;
}

void combat_exit(void)
{
	if (!s_ok)
		return;
	unload_weapon();
	if (s_flash_vbo)
		linearFree(s_flash_vbo);
	s_flash_vbo = NULL;
	s_ok = false;
}

void combat_reset(void)
{
	s_hp = JAMES_HP_MAX;
	s_hp_recover = 0.0f;
	s_mode = M_FREE;
	s_side = 0;
	s_invuln = s_dead_t = s_flash = 0.0f;
	s_target = NULL;
	s_carried = false;
	int w = s_maria ? W_MNONE : W_PLANK;
	if (s_ok && s_weapon != w)
		load_weapon(w);
	play(&J_STAND);
	maria_reset();
	s_fresh = true;           // the chapter is set after this, see player_sync
}

// PlayerInit (0x533ad0): the health depends on the difficulty and on who plays, 200 / 100 for James
// on beginner and easy / normal and hard, 160 / 80 for Maria in Born From a Wish.
float combat_hp_max(void)
{
	bool easy = enemy_difficulty() <= 1;
	if (prog_chapter() == 1)
		return easy ? 160.0f : 80.0f;
	return easy ? 200.0f : 100.0f;
}

float combat_ground_speed(int gait) { return pdef()->speed[gait < 0 ? 0 : gait > 2 ? 2 : gait]; }

// Main's player model is James' outside Born From a Wish (gChapterID 1) and Maria's in it; the
// switch happens here on the first update of a chapter, so new game, continue and the title need
// no hook. A new game also starts at full health and, for Maria, with ItemInit's (0x49e980) revolver
// holding one round and drawn (DAT_01f7d409 = 10).
static void player_sync(void)
{
	static bool no_maria;     // her files were missing once: stay James
	bool maria = prog_chapter() == 1 && !no_maria;
	if (maria != s_maria) {
		char dir[96], err[128];
		unload_weapon();
		C3D_FrameSync();      // the GPU may still read the old model this frame
		chr_free(s_james);
		s_maria = maria;
		snprintf(dir, sizeof dir, "%s/chr/%s", s_pack, pdef()->dir);
		if (!chr_load(s_james, dir, pdef()->model, err, sizeof err) && maria) {
			s_maria = false;      // Maria's files are missing: carry on as James
			no_maria = true;
			snprintf(dir, sizeof dir, "%s/chr/%s", s_pack, pdef()->dir);
			chr_load(s_james, dir, pdef()->model, err, sizeof err);
		}
		s_mode = M_FREE;
		s_side = 0;
		if (!load_weapon(s_maria ? W_MNONE : W_PLANK))
			load_weapon(bare());
		if (s_hp > JAMES_HP_MAX)
			s_hp = JAMES_HP_MAX;
	}
	if (!s_fresh)
		return;
	s_fresh = false;
	s_hp = JAMES_HP_MAX;
	if (s_maria && ev_item_count(WEAPONS[W_COLT].item) == 0 &&
	    ev_item_count(WEAPONS[W_COLT].owned_marker) == 0) {
		ev_give_item(WEAPONS[W_COLT].item, 1);
		ev_give_item(WEAPONS[W_COLT].owned_marker, 1);
		load_weapon(W_COLT);
	}
}

float combat_hp(void) { return s_hp; }
void combat_set_hp(float hp)
{
	s_hp = hp > 0.0f && hp <= JAMES_HP_MAX ? hp : JAMES_HP_MAX;
	s_fresh = false;          // continue: keep the saved health and weapons
}
void combat_heal(float amount) { s_hp_recover += amount; }

// Healing is not instant on the PC: items fill hp_recover and it flows into hp. The flow rate here
// is provisional (the PC's recovery step was not traced).
static void recover_update(float dt)
{
	if (s_hp_recover <= 0.0f || s_hp <= 0.0f)
		return;
	float step_hp = 30.0f * dt;
	step_hp = step_hp > s_hp_recover ? s_hp_recover : step_hp;
	s_hp_recover -= step_hp;
	s_hp = s_hp + step_hp > JAMES_HP_MAX ? JAMES_HP_MAX : s_hp + step_hp;
	if (s_hp >= JAMES_HP_MAX)
		s_hp_recover = 0.0f;
}

void combat_set_equipped(bool on)
{
	if (s_mode != M_FREE || !s_ok)
		return;
	load_weapon(on ? (s_maria ? W_KNIFE : W_PLANK) : bare());
}

// Inventory "use" of a weapon item: equip it (using the equipped one again puts it away).
bool combat_equip_item(int item)
{
	if (!s_ok || s_mode != M_FREE)
		return false;
	for (int w = bare() + 1; w <= pdef()->last; w++)
		if (WEAPONS[w].item == item && owns(w)) {
			load_weapon(w == s_weapon ? bare() : w);
			return true;
		}
	return false;
}

// L: next owned weapon (the PC switches from the item menu only; this is a 3DS shortcut).
static void cycle_weapon(void)
{
	int first = pdef()->first, n = pdef()->last - first + 1;
	for (int i = 1; i <= n; i++) {
		int w = first + (s_weapon - first + i) % n;
		if (owns(w)) {
			if (w != s_weapon)
				load_weapon(w);
			return;
		}
	}
}

// James is dead, or Maria is (game over all the same).
bool combat_dead(void) { return (s_mode == M_DEAD && s_dead_t > 1.5f && !s_carried) || maria_dead(); }

EnPlayer combat_enplayer(const CombatBody* b, bool light)
{
	EnPlayer p = { *b->x, *b->y, *b->z, *b->yaw, light, s_mode < M_DYING,
	               (s_mode == M_SWING && s_atk == &ATK_DOWN) || s_mode == M_STOMP };
	return p;
}

// Low health indicator, PC 0x476730: below half health a red cross flashes in the bottom right
// corner. Its alpha is (1 - hp / max) times a pulse that runs between 0.1 and 1.0 by 0.03 a frame
// (gLowHealthIndicatorFlashSpeed 0x7f69f8, value 0x7f69fc); at half health or more the pulse
// restarts from 1. The Enhanced Edition's default "LowHealthIndicatorStyle" 2, a red vignette,
// needs its own LowHealthFade.png, and the EE falls back to this cross without it.
float combat_low_health_alpha(float dt)
{
	static float pulse = 1.0f, speed = -0.03f;
	if (s_hp >= JAMES_HP_MAX * 0.5f) {
		pulse = 1.0f;
		return 0.0f;
	}
	float a = (JAMES_HP_MAX - s_hp) / JAMES_HP_MAX * pulse;
	pulse += speed * dt * 30.0f;
	if (pulse < 0.1f) {
		pulse = 0.1f;
		speed = -speed;
	} else if (pulse > 1.0f) {
		pulse = 1.0f;
		speed = -speed;
	}
	return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

// Grabs (0x533d00 for attacks 0x35, 0x3c, 0x3f, 0x41, 0x44): James is held while the meter lasts.
// Each new button press or flick of the circle pad takes one frame time off it (0x5565c0); on
// beginner it also drains by itself.
void combat_grab_james(float meter)
{
	if (s_mode >= M_DYING)
		return;
	s_mode = M_HELD;
	s_hold = meter;
	s_stick_dir = 0;
	s_charge = -1.0f;
	play(&J_HURT);
}

bool combat_held(void) { return s_mode == M_HELD; }

// en_ike state 8 (0x4b7ff0): the game is over only when the lift has ended (DAT_01fbad17 = 2,
// 0x4a63b0).
void combat_carry_james(bool on, float x, float y, float z)
{
	s_carried = on && s_mode >= M_DYING;
	s_carry[0] = x, s_carry[1] = y, s_carry[2] = z;
}

bool combat_dying(void) { return s_mode == M_DYING || s_mode == M_DEAD; }

bool combat_has_ammo(void)
{
	for (int w = W_HANDGUN; w < W_COUNT; w++)
		if (WEAPONS[w].clip > 0 && owns(w) &&
		    (ev_item_count(WEAPONS[w].item) > 0 || ev_item_count(WEAPONS[w].ammo) > 0))
			return true;
	return false;
}

// The hold attacks (kind 6) are checked once per game frame and drain James by their ap each time;
// the port counts the PC's 30 frames a second (the struggle meter is in the same frame times). On
// beginner they cannot kill him.
// Playtest driver ("pt god 1"): blows still land (flinches, holds) but take no health, so a fight's
// own end (a boss's health or clock) can be checked with the real weapons.
static bool s_test_god;
void combat_test_god(bool on) { s_test_god = on; }

void combat_hold_damage(float ap, float dt)
{
	if (s_mode != M_HELD)
		return;
	s_hp -= s_test_god ? 0.0f : ap * dt * 30.0f;
	if (s_hp <= 0.0f) {
		if (enemy_difficulty() == 0) {
			s_hp = 1.0f;
			return;
		}
		s_hp = 0.0f;
		sfx_play(pdef()->snd_die, 1.0f, 0.0f);
		s_mode = M_DYING;
		play(&J_FALL);
	}
}

void combat_damage_james(int attack_id, float ap, float from_x, float from_z)
{
	(void)from_x, (void)from_z;
	if (s_mode >= M_DYING || s_mode == M_HELD || s_invuln > 0.0f)
		return;
	s_hp -= s_test_god ? 0.0f : ap;
	if (s_hp <= 0.0f) {
		s_hp = 0.0f;
		sfx_play(pdef()->snd_die, 1.0f, 0.0f);   // dying scream
		s_mode = M_DYING;
		play(&J_FALL);
		return;
	}
	if (attack_id == 42 && s_mode >= M_RAISE && s_mode <= M_RELOAD) {
		// The acid spray (kind 4, shBattleAttackByEnemyFog) burns without knocking the weapon down:
		// James keeps fighting. Blows (kinds 2 and 3) make him flinch.
		s_invuln = 1.0f;
		sfx_play(pdef()->snd_hurt + (int)(s_hp) % pdef()->n_hurt, 1.0f, 0.0f);
		return;
	}
	s_mode = M_HURT;
	s_invuln = 1.0f;
	sfx_play(pdef()->snd_hurt + (int)(s_hp) % pdef()->n_hurt, 1.0f, 0.0f);   // injured (four or five takes)
	play(&J_HURT);
}

// What James can see (0x52f2c0 sets his areas, 0x5420f0 tests them): a "look" sphere ahead of him
// and a "feel" sphere round him, in metres (x500 units) by the light (0 lit room, 1 dark with the
// flashlight, 2 dark without, 3 Maria in the dark) and the action level. An enemy in either is a
// candidate (0x542300 lists them nearest first).
static const float LOOK_AREA[4][5] = {        // centre ahead = radius
	{ 7.0f, 7.0f, 6.0f, 4.5f, 4.0f }, { 5.0f, 5.0f, 3.8f, 3.0f, 2.8f },
	{ 3.0f, 3.0f, 1.8f, 1.5f, 1.3f }, { 6.0f, 6.0f, 5.0f, 4.0f, 3.0f },
};
static const float FEEL_RADIUS[4][5] = {
	{ 6.5f, 6.5f, 5.0f, 3.0f, 2.5f }, { 4.5f, 4.5f, 3.8f, 3.3f, 2.8f },
	{ 3.0f, 3.0f, 2.3f, 2.0f, 1.5f }, { 5.0f, 5.0f, 4.0f, 3.0f, 2.0f },
};
static const float FEEL_CENTRE[4] = { 0.5f, 0.3f, 0.1f, 0.5f };
static bool s_dark, s_light;

void combat_set_sight(bool dark, bool flashlight)
{
	s_dark = dark;
	s_light = flashlight;
}

static bool in_sight_at(const CombatBody* b, float x, float z)
{
	int mode = !s_dark ? 0 : s_maria ? 3 : s_light ? 1 : 2;
	int lvl = enemy_difficulty();
	lvl = lvl < 0 ? 0 : lvl > 4 ? 4 : lvl;
	float fx = sinf(*b->yaw), fz = cosf(*b->yaw);
	float lc = LOOK_AREA[mode][lvl] * 500.0f, fr = FEEL_RADIUS[mode][lvl] * 500.0f, fc = FEEL_CENTRE[mode] * 500.0f;
	float d_look = hypotf(x - (*b->x + fx * lc), z - (*b->z + fz * lc));
	float d_feel = hypotf(x - (*b->x + fx * fc), z - (*b->z + fz * fc));
	return d_look <= lc || d_feel <= fr;
}

static bool in_sight(const CombatBody* b, const Enemy* e) { return in_sight_at(b, e->x, e->z); }

// 0x542060: nothing of the room between James' chest and the enemy
static bool clear_line_to(const CombatBody* b, float x, float z)
{
	if (!s_col)
		return true;
	float ex = x, ez = z, d = hypotf(ex - *b->x, ez - *b->z);
	col_clamp_ray(s_col, *b->x, *b->z, &ex, &ez, *b->y - 900.0f);
	return hypotf(ex - *b->x, ez - *b->z) >= d - 1.0f;
}

static bool clear_line(const CombatBody* b, const Enemy* e) { return clear_line_to(b, e->x, e->z); }

bool combat_look_target(const CombatBody* b, const float (*pts)[3], int npts, float out[3])
{
	if (s_mode == M_STOMP || s_mode >= M_HURT)
		return false;                    // 0x535720: not while kicking, hurt, held or dying
	if (s_target && enemy_alive(s_target) && in_sight(b, s_target)) {
		float cx, cz, r, top, bottom;
		enemy_hit_cylinder(s_target, &cx, &cz, &r, &top, &bottom);
		out[0] = cx, out[1] = top + 0.15f * (bottom - top), out[2] = cz;
		return true;
	}
	// 0x542860: the nearest candidate (0x542300 lists enemies, people and items in his look and feel
	// areas) that nothing of the room hides
	float bd = 1e9f;
	bool found = false;
	for (int i = 0; i < enemy_count(); i++) {
		Enemy* e = enemy_get(i);
		if (!enemy_alive(e) || !in_sight(b, e))
			continue;
		float d = hypotf(e->x - *b->x, e->z - *b->z);
		if (d >= bd || !clear_line(b, e))
			continue;
		float cx, cz, r, top, bottom;
		enemy_hit_cylinder(e, &cx, &cz, &r, &top, &bottom);
		out[0] = cx, out[1] = top + 0.15f * (bottom - top), out[2] = cz;
		bd = d;
		found = true;
	}
	for (int i = 0; i < npts; i++) {
		float d = hypotf(pts[i][0] - *b->x, pts[i][2] - *b->z);
		if (d >= bd || d < 100.0f || !in_sight_at(b, pts[i][0], pts[i][2]) || !clear_line_to(b, pts[i][0], pts[i][2]))
			continue;
		memcpy(out, pts[i], sizeof(float) * 3);
		bd = d;
		found = true;
	}
	return found;
}

static float angle_to(const CombatBody* b, const Enemy* e)
{
	float a = atan2f(e->x - *b->x, e->z - *b->z) - *b->yaw;
	while (a > PI) a -= 2 * PI;
	while (a < -PI) a += 2 * PI;
	return a;
}

// 0x542560: the nearest candidate in clear sight, one still on its feet before one on the floor.
// dir +1 / -1 (0x542740, changing target): the nearest other one to the left / right instead.
static Enemy* pick_target(const CombatBody* b, int dir)
{
	Enemy* best = NULL;
	float bd = 1e9f;
	bool best_down = true;
	for (int i = 0; i < enemy_count(); i++) {
		Enemy* e = enemy_get(i);
		if (!enemy_alive(e) || (dir && e == s_target) || !in_sight(b, e))
			continue;
		if (dir && (angle_to(b, e) > 0.0f) != (dir > 0))
			continue;
		float d = hypotf(e->x - *b->x, e->z - *b->z);
		bool down = enemy_is_down(e);
		if (dir)
			down = false;            // changing target takes the nearest, down or not
		if ((down && !best_down) || (down == best_down && d >= bd))
			continue;
		if (!clear_line(b, e))
			continue;
		best = e;
		bd = d;
		best_down = down;
	}
	return best;
}

// The target James turns to and shoots at (PlayerGetTarget): the one locked when he readied,
// while it lives and he can still see it (0x533c10), within reach.
static Enemy* find_target(const CombatBody* b, float* angle, float reach)
{
	if (s_target && (!enemy_alive(s_target) || !in_sight(b, s_target)))
		s_target = NULL;
	Enemy* t = s_target;
	if (!t || hypotf(t->x - *b->x, t->z - *b->z) > reach)
		return NULL;
	*angle = angle_to(b, t);
	return t;
}

static void to_world(const CombatBody* b, const float in[3], float out[3])
{
	const Mtx34* m = &s_james->skin[pdef()->hand];
	float p[3];
	for (int r = 0; r < 3; r++)
		p[r] = m->m[r][0] * in[0] + m->m[r][1] * in[1] + m->m[r][2] * in[2] + m->m[r][3];
	float c = cosf(*b->yaw), s = sinf(*b->yaw);
	out[0] = *b->x + c * p[0] + s * p[2];   // Mtx_RotateY: x' = c x + s z, z' = -s x + c z
	out[1] = *b->y + p[1];
	out[2] = *b->z - s * p[0] + c * p[2];
}

// Distance from point (px, pz) to segment a-b in the ground plane.
static float seg_dist2d(const float a[3], const float b[3], float px, float pz)
{
	float vx = b[0] - a[0], vz = b[2] - a[2];
	float wx = px - a[0], wz = pz - a[2];
	float l = vx * vx + vz * vz;
	float t = l > 0.0f ? (wx * vx + wz * vz) / l : 0.0f;
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	float dx = a[0] + vx * t - px, dz = a[2] + vz * t - pz;
	return sqrtf(dx * dx + dz * dz);
}

// shBattleAttackByHumanFightType (PS2 sh_character_battle.c), reduced: inside the attack's frame
// window the plank segment (hand to far end) is tested against each enemy's hit column.
static void swing_hits(const CombatBody* b)
{
	if (!s_atk)
		return;
	float f = s_frame - s_cur->start;
	if (f < s_atk->first || f > s_atk->last + 0.99f)
		return;
	chr_pose(s_james, &s_anim, s_frame);
	float a[3], t[3];
	to_world(b, s_root, a);
	to_world(b, s_tip, t);
	float fx = sinf(*b->yaw), fz = cosf(*b->yaw);
	float rx = fz, rz = -fx;                       // James' right
	if (!s_hit_any && !s_swing_wall && s_col) {
		// the weapon's far end inside a wall: the blow strikes the room
		float ex = t[0], ez = t[2];
		col_clamp_ray(s_col, *b->x, *b->z, &ex, &ez, t[1]);
		if (hypotf(ex - t[0], ez - t[2]) > 1.0f) {
			s_swing_wall = true;
			s_wall_ap_next = s_atk->ap;
		}
	}
	for (int i = 0; i < enemy_count(); i++) {
		Enemy* e = enemy_get(i);
		if (!enemy_alive(e) || s_hit[i])
			continue;
		float cx, cz, r, top, bottom;
		enemy_hit_cylinder(e, &cx, &cz, &r, &top, &bottom);
		float ylo = a[1] < t[1] ? a[1] : t[1], yhi = a[1] < t[1] ? t[1] : a[1];
		if (yhi < top || ylo > bottom)
			continue;
		if (seg_dist2d(a, t, cx, cz) > r)
			continue;
		s_hit[i] = true;
		s_hit_any = true;
		// the hit (0x5350d0): the plank's 11032, the knife's 17035, both at 0.8
		sfx_play(s_maria ? 17035 : 11032, 0.8f, 0.0f);
		float sgn = s_side == 0 ? -0.5f : 0.5f;    // the side the blow travels toward
		enemy_damage(e, s_atk->id, s_atk->ap, s_atk->sp, fx + rx * sgn, fz + rz * sgn);
	}
}

static void start_swing(const CombatBody* b, bool combo, bool heavy)
{
	float ang = 0.0f;
	Enemy* t = find_target(b, &ang, 4000.0f);
	memset(s_hit, 0, sizeof s_hit);
	s_hit_any = false;
	s_swing_wall = false;
	s_combo_req = false;
	// heavy: the plank's attack 14 (request 4); a downed target also gets it (port choice, as a
	// level swing passes over a body on the floor)
	if (heavy || (t && enemy_is_down(t) && t->p_dist < 1400.0f)) {
		s_atk = &ATK_DOWN;
		play(&P_DOWNSWING[s_side]);
	} else if (combo) {
		s_atk = &ATK_COMBO;
		play(&P_COMBO[s_side]);
	} else {
		s_atk = &ATK_SWING;
		play(&P_SWING[s_side]);
	}
	s_mode = M_SWING;
	if (s_maria) {
		// the knife's swish 0x428a with one of her two efforts 0x4286 / 0x4287 (0x549500)
		sfx_play(17034, 1.0f, 0.0f);
		sfx_play(17030 + (rand() & 1), 0.9f, 0.0f);
	} else {
		sfx_play(11027, 1.0f, 0.0f);     // melee swing
	}
}

// ---------------------------------------------------------------- guns

// First enemy hit column crossed by the segment o + d * t, t in [t0, t1] (d unit length); returns t.
static float ray_enemy(const float o[3], const float d[3], float t0, float t1, Enemy** hit)
{
	float best = t1;
	*hit = NULL;
	for (int i = 0; i < enemy_count(); i++) {
		Enemy* e = enemy_get(i);
		if (!enemy_alive(e))
			continue;
		float cx, cz, r, top, bottom;
		enemy_hit_cylinder(e, &cx, &cz, &r, &top, &bottom);
		// closest approach of the ray to the column axis in the ground plane
		float hx = d[0], hz = d[2], h2 = hx * hx + hz * hz;
		float wx = cx - o[0], wz = cz - o[2];
		float t = h2 > 1e-6f ? (wx * hx + wz * hz) / h2 : 0.0f;
		float px = o[0] + hx * t - cx, pz = o[2] + hz * t - cz;
		float miss2 = px * px + pz * pz;
		if (miss2 > r * r)
			continue;
		float back = h2 > 1e-6f ? sqrtf((r * r - miss2) / h2) : 0.0f;
		float te = t - back;                 // where the ray enters the column
		if (te < t0) te = t0;
		if (te > best)
			continue;
		float y = o[1] + d[1] * te;
		if (y < top - 50.0f || y > bottom + 50.0f)
			continue;
		best = te;
		*hit = e;
	}
	return best;
}

static void fire(const CombatBody* b)
{
	const WeaponDef* w = wdef();
	chr_pose(s_james, &s_anim, s_frame);
	float o[3];
	to_world(b, s_muzzle, o);
	// aim: at the locked target's body, else level along James' facing (0x5460b0)
	float ang = 0.0f;
	Enemy* t = find_target(b, &ang, w->atk.max_range);
	float aim[3] = { sinf(*b->yaw), 0.0f, cosf(*b->yaw) };
	if (t) {
		float cx, cz, r, top, bottom;
		enemy_hit_cylinder(t, &cx, &cz, &r, &top, &bottom);
		float tx = cx - o[0], ty = (top + bottom) * 0.5f - o[1], tz = cz - o[2];
		float n = sqrtf(tx * tx + ty * ty + tz * tz);
		if (n > 1.0f)
			aim[0] = tx / n, aim[1] = ty / n, aim[2] = tz / n;
	}
	// the room stops the ray: the first wall along the aim (col_clamp_ray stops short of it)
	float wall = w->atk.max_range;
	float hlen = sqrtf(aim[0] * aim[0] + aim[2] * aim[2]);
	if (s_col && hlen > 0.05f) {
		float ex = o[0] + aim[0] * wall, ez = o[2] + aim[2] * wall;
		col_clamp_ray(s_col, o[0], o[2], &ex, &ez, o[1]);
		// col_clamp_ray (a camera helper) stops 8 % of the segment short of the wall, which for a
		// 7500 shot is 600 units: add it back so things on or near the wall can be hit
		float hd = hypotf(ex - o[0], ez - o[2]) + 0.08f * wall * hlen + 40.0f;
		if (hd / hlen < wall)
			wall = hd / hlen;
	}
	for (int p = 0; p < w->pellets; p++) {
		float d[3] = { aim[0], aim[1], aim[2] };
		if (w->pellets > 1) {
			// shotgun: a random direction within 30 degrees of the aim (0x543810)
			float polar = ((float)rand() / RAND_MAX) * 0.5235988f;
			float az = (2.0f * rand() / RAND_MAX - 1.0f) * PI;
			float ux = -aim[2], uz = aim[0];                       // a side vector
			float un = sqrtf(ux * ux + uz * uz);
			if (un < 1e-4f) ux = 1, uz = 0, un = 1;
			ux /= un, uz /= un;
			float vx = aim[1] * uz, vy = aim[2] * ux - aim[0] * uz, vz = -aim[1] * ux;   // aim x side
			float s = sinf(polar), c = cosf(polar), sa = sinf(az), ca = cosf(az);
			d[0] = aim[0] * c + (ux * ca + vx * sa) * s;
			d[1] = aim[1] * c + (vy * sa) * s;
			d[2] = aim[2] * c + (uz * ca + vz * sa) * s;
		}
		Enemy* e;
		ray_enemy(o, d, w->atk.min_range > 0.0f ? w->atk.min_range : 0.0f, wall, &e);
		if (e)
			enemy_damage(e, w->atk.id, w->atk.ap, w->atk.sp, d[0], d[2]);
		else if (wall < w->atk.max_range)
			s_wall_ap_next = w->atk.ap;     // the shot went into the wall
	}
	ev_give_item(w->item, -1);
	sfx_play(w->snd_shot, 1.0f, 0.0f);
	s_flash = 0.06f;
}

// Downed enemy at James' feet (the PC stomps instead of shooting or swinging bare-handed).
static Enemy* stomp_target(const CombatBody* b)
{
	for (int i = 0; i < enemy_count(); i++) {
		Enemy* e = enemy_get(i);
		if (!enemy_alive(e) || !enemy_is_down(e))
			continue;
		float cx, cz, r, top, bottom;
		enemy_hit_cylinder(e, &cx, &cz, &r, &top, &bottom);
		float fx = *b->x + sinf(*b->yaw) * 250.0f, fz = *b->z + cosf(*b->yaw) * 250.0f;
		if (hypotf(cx - fx, cz - fz) < r + ATK_STOMP.max_range)
			return e;
	}
	return NULL;
}

static void start_stomp(bool stomp)
{
	s_mode = M_STOMP;
	s_stomp_atk = stomp ? &ATK_STOMP : &ATK_KICK;
	memset(s_hit, 0, sizeof s_hit);
	play(stomp ? &J_STOMP : &J_KICK);
}

float combat_wall_hit(int* weapon_item)
{
	if (weapon_item)
		*weapon_item = WEAPONS[s_weapon].item;
	return s_wall_ap;
}

void combat_set_options(bool hold_to_stomp, bool swap_light_heavy)
{
	s_hold_to_stomp = hold_to_stomp;
	s_swap_light_heavy = swap_light_heavy;
}

// ---------------------------------------------------------------- per frame

// Maria's footsteps (0x549150, her counterpart of James' 0x54a9a0): both feet strike on frames 5
// and 17 of her walk and run entries, at volume 0.5 walking and 0.8 running, with her own sound
// per floor material. James' steps stay with main.c's clips (combat_own_footsteps).
static const JAnim* s_step_anim;
static float s_step_f;

static int maria_step_id(int material)
{
	switch (material) {
	case 1: return 19035;
	case 2: return 19032;
	case 5: return 19038;
	case 6: return 19037;
	case 7: return 19036;
	case 8: return 19033;
	case 10: return 19034;
	default: return 19031;
	}
}

static void maria_steps(const CombatBody* b, const JAnim* a)
{
	if (a != &J_WALK && a != &J_RUN) {
		s_step_anim = NULL;
		return;
	}
	if (s_step_anim == a) {
		float f0 = s_step_f, f1 = s_frame, len = (float)(a->end - a->start + 1);
		bool back = f1 < f0 && f0 - f1 < len * 0.5f;
		float adv = back ? f0 - f1 : f1 >= f0 ? f1 - f0 : f1 + len - f0;
		for (int k = 0; k < 2 && adv < 4.0f; k++) {     // a bigger jump is a restart, not a step
			float fr = (float)(a->start + (k ? 17 : 5));
			bool hit = back ? (fr < f0 && fr >= f1)
			                : (f1 >= f0 ? fr > f0 && fr <= f1 : fr > f0 || fr <= f1);
			if (hit) {
				float pos[3] = { *b->x, *b->y, *b->z };
				sfx_play_at(maria_step_id(sfx_floor_material(pos[0], pos[1], pos[2])),
				            a == &J_RUN ? 0.8f : 0.5f, pos);
			}
		}
	}
	s_step_anim = a;
	s_step_f = s_frame;
}

bool combat_own_footsteps(void) { return s_ok && s_maria; }

static void free_moves(const CombatBody* b, const circlePosition* cp, u32 held, float dt)
{
	// weapon carried: pick the stand / walk / run anime like player_update does
	float fwd = cp->dy / 156.0f;
	bool run = (held & KEY_B) && fwd > 0.15f;
	const JAnim* want = fabsf(fwd) < 0.15f ? &J_STAND : run ? &J_RUN : &J_WALK;
	if (want != s_cur)
		play(want);
	if (fwd < -0.15f)
		s_frame -= 2.0f * dt * 30.0f * (want->speed / 2048.0f);   // walking backwards
	if (s_frame < want->start)
		s_frame += want->end - want->start + 1;
	if (s_maria)
		maria_steps(b, want);
}

static void turn_to_target(CombatBody* b, const circlePosition* cp, float dt, float reach)
{
	float ang = 0.0f;
	if (find_target(b, &ang, reach)) {
		float st = 4.0f * dt;
		*b->yaw += ang > st ? st : ang < -st ? -st : ang;
	} else {
		*b->yaw -= cp->dx / 156.0f * 2.0f * dt;
	}
}

bool combat_update(CombatBody* b, const circlePosition* cp, u32 down, u32 held, float dt,
                   const Collision* col, u32* held_out)
{
	recover_update(dt);
	*held_out = held;
	s_col = col;
	s_wall_ap = s_wall_ap_next;      // the last frame's wall hit, for the stage code (0x544f4c clears it)
	s_wall_ap_next = 0.0f;
	if (!s_ok)
		return false;
	player_sync();
	if (s_invuln > 0.0f)
		s_invuln -= dt;
	if (s_flash > 0.0f)
		s_flash -= dt;
	step(dt);
	const WeaponDef* w = wdef();
	switch (s_mode) {
	case M_FREE:
		if (down & KEY_L) {
			cycle_weapon();
			return false;
		}
		if (held & KEY_R) {
			s_target = pick_target(b, 0);    // 0x533ca0: locked as he readies
			if (is_bare()) {
				s_mode = M_READY;          // bare hands: nothing to raise
			} else if (is_melee()) {
				s_mode = M_RAISE;
				play(&P_RAISE[s_side]);
			} else {
				s_mode = M_RAISE;
				play(&w->raise);
			}
			return true;
		}
		free_moves(b, cp, held, dt);
		return false;
	case M_RAISE:
		if (s_end) {
			s_mode = M_READY;
			play(is_melee() ? &P_READY[s_side] : &w->aim);
		}
		break;
	case M_READY:
		if (!(held & KEY_R)) {
			s_charge = -1.0f;
			if (is_bare()) { s_mode = M_FREE; play(&J_STAND); return false; }
			s_mode = M_LOWER;
			play(is_melee() ? &P_LOWER[s_side] : &w->lower);
			break;
		}
		// 0x54f100: the strafe buttons move the lock to the nearest other enemy on that side
		if (down & (GK_STRAFE_L | GK_STRAFE_R)) {
			Enemy* t = pick_target(b, (down & GK_STRAFE_L) ? 1 : -1);
			if (t)
				s_target = t;
		}
		turn_to_target(b, cp, dt, is_melee() || is_bare() ? 4000.0f : w->atk.max_range);
		if (s_charge >= 0.0f) {
			// tap or hold (EE timers): decided on release, or once A has been held 0.2 s
			s_charge += dt;
			bool hold = (held & KEY_A) != 0;
			if (!hold || s_charge >= HOLD_SECONDS) {
				s_charge = -1.0f;
				if (s_charge_stomp)
					start_stomp(hold);
				else
					start_swing(b, false, hold);
			}
			break;
		}
		if (down & KEY_A) {
			if ((is_bare() || is_gun()) && stomp_target(b)) {
				if (s_hold_to_stomp)
					s_charge = 0.0f, s_charge_stomp = true;
				else
					start_stomp(true);
			} else if (is_melee()) {
				if (s_swap_light_heavy)
					s_charge = 0.0f, s_charge_stomp = false;
				else
					start_swing(b, false, true);
			} else if (is_gun()) {
				if (loaded() > 0) {
					s_mode = M_SHOT;
					s_fired = false;
					play(&w->shot);
				} else if (spare() > 0) {
					s_mode = M_RELOAD;
					s_reloaded = false;
					play(&w->reload);
				} else {
					sfx_play(w->snd_empty, 1.0f, 0.0f);   // click (0x533ff0)
				}
			}
			break;
		}
		if (is_melee()) {
			// James can step toward or away from the target with the weapon up (no running)
			float fwd = cp->dy / 156.0f;
			if (fabsf(fwd) < 0.15f) {
				if (s_cur != &P_READY[s_side])
					play(&P_READY[s_side]);
				break;
			}
			float speed = fwd > 0.0f ? 500.0f * fwd : 350.0f * fwd;
			float nx = *b->x + sinf(*b->yaw) * speed * dt, nz = *b->z + cosf(*b->yaw) * speed * dt, ny;
			if (col) {
				col_push(col, &nx, &nz, *b->y, 900.0f, 130.0f);
				if (col_floor(col, nx, nz, *b->y, 160.0f, &ny))
					*b->x = nx, *b->z = nz, *b->y = ny;
			} else {
				*b->x = nx, *b->z = nz;
			}
			if (s_cur != &J_WALK)
				play(&J_WALK);
			if (fwd < 0.0f)
				s_frame -= 2.0f * dt * 30.0f * (J_WALK.speed / 2048.0f);
			if (s_frame < J_WALK.start)
				s_frame += J_WALK.end - J_WALK.start + 1;
		} else if (is_bare()) {
			free_moves(b, cp, 0, dt);
		}
		break;
	case M_SHOT:
		if (!s_fired && s_frame - w->shot.start >= w->atk.first) {
			s_fired = true;
			fire(b);
		}
		turn_to_target(b, cp, dt, w->atk.max_range);
		if (s_end) {
			s_mode = M_READY;
			play(&w->aim);
		}
		break;
	case M_RELOAD:
		if (!s_reloaded && s_frame - w->reload.start >= w->reload_frame) {
			s_reloaded = true;
			int n = w->clip - loaded();
			if (n > spare()) n = spare();
			if (n > 0) {
				ev_give_item(w->item, n);
				ev_give_item(w->ammo, -n);
			}
			sfx_play(w->snd_reload, 1.0f, 0.0f);
		}
		if (s_end) {
			s_mode = (held & KEY_R) ? M_READY : M_LOWER;
			play(s_mode == M_READY ? &w->aim : &w->lower);
		}
		break;
	case M_STOMP: {
		const AttackInfo* a = s_stomp_atk;
		float f = s_frame - s_cur->start;
		if (f >= a->first && f <= a->last + 0.99f) {
			Enemy* e = stomp_target(b);
			if (e) {
				int i = 0;
				for (; i < enemy_count() && enemy_get(i) != e; i++)
					;
				if (i < ENEMY_MAX && !s_hit[i]) {
					s_hit[i] = true;
					sfx_play(11040, 1.0f, 0.0f);      // thud
					enemy_damage(e, a->id, a->ap, a->sp, sinf(*b->yaw), cosf(*b->yaw));
				}
			}
		}
		if (s_end) {
			s_mode = (held & KEY_R) ? M_READY : M_FREE;
			if (s_mode == M_READY && !is_bare()) {
				s_mode = M_RAISE;
				play(is_melee() ? &P_RAISE[s_side] : &w->raise);
			} else {
				play(&J_STAND);
			}
			if (s_mode == M_FREE)
				return false;
		}
		break;
	}
	case M_SWING:
		if (down & KEY_A)
			s_combo_req = true;
		swing_hits(b);
		if (s_hit_any && s_atk != &ATK_DOWN) {
			// 0x52fae0: a swing that connects plays back (entries 364 / 370 from the same frame)
			s_mode = M_REBOUND;
			play_from(&P_REBOUND[s_side], s_frame);
			break;
		}
		if (s_end) {
			s_atk = NULL;
			if (s_cur == &P_DOWNSWING[s_side]) {
				s_mode = M_READY;
				play(&P_READY[s_side]);
			} else if (s_combo_req && (held & KEY_R)) {
				// 0x5569b0: with the button pressed again during a swing (DAT_01fbad19) the next
				// attack is the short swing (attack 13) from the other shoulder, without the
				// follow-through in between
				s_side ^= 1;
				start_swing(b, true, false);
			} else {
				s_mode = M_RECOVER;
				play(&P_RECOVER[s_side]);
			}
		}
		break;
	case M_REBOUND:
		if (down & KEY_A)
			s_combo_req = true;
		if (s_end) {
			s_atk = NULL;
			if (s_combo_req && (held & KEY_R)) {
				start_swing(b, true, false);
			} else {
				s_mode = M_READY;
				play(&P_READY[s_side]);
			}
		}
		break;
	case M_RECOVER:
		if (down & KEY_A)
			s_combo_req = true;
		if (s_combo_req && (held & KEY_R)) {
			s_side ^= 1;
			start_swing(b, true, false);
			break;
		}
		if (s_end) {
			s_side ^= 1;
			s_mode = M_READY;
			play(&P_READY[s_side]);
		}
		break;
	case M_LOWER:
		if (held & KEY_R) {
			s_mode = M_RAISE;
			play(is_melee() ? &P_RAISE[s_side] : &w->raise);
			break;
		}
		if (s_end) { s_mode = M_FREE; play(&J_STAND); return false; }
		break;
	case M_HURT:
		if (s_end) { s_mode = M_FREE; play(&J_STAND); return false; }
		break;
	case M_HELD: {
		// hurt pose held in its middle while he struggles
		if (s_frame > J_HURT.start + 8.0f)
			s_frame = J_HURT.start + 4.0f;
		int dir = 0;
		if (cp->dx * cp->dx + cp->dy * cp->dy > 80 * 80)
			dir = abs(cp->dx) > abs(cp->dy) ? (cp->dx > 0 ? 1 : 2) : (cp->dy > 0 ? 3 : 4);
		int presses = __builtin_popcount(down & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R |
		                                         KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT));
		if (dir && dir != s_stick_dir)
			presses++;
		s_stick_dir = dir;
		// one PC frame time (1/30 s, as for the hold drain) per press, so it takes the PC's number
		// of presses whatever the port's frame rate
		s_hold -= presses / 30.0f + (enemy_difficulty() == 0 ? dt : 0.0f);
		if (s_hold <= 0.0f) {
			s_hold = 0.0f;
			s_mode = M_HURT;
			s_invuln = 1.0f;
		}
		break;
	}
	case M_DYING:
		if (s_carried) {
			// hanging from the Flesh Lip's legs: the hurt pose held as in the hold
			*b->x = s_carry[0], *b->y = s_carry[1], *b->z = s_carry[2];
			if (s_cur != &J_HURT) play(&J_HURT);
			if (s_frame > J_HURT.start + 8.0f) s_frame = J_HURT.start + 4.0f;
			break;
		}
		if (s_end) { s_mode = M_DEAD; s_dead_t = 0.0f; }
		break;
	case M_DEAD:
		s_dead_t += dt;
		break;
	}
	if (s_mode == M_READY)
		s_combo_req = false;
	else
		s_charge = -1.0f;
	*held_out = held & ~(KEY_A | KEY_B);
	return true;
}

bool combat_james_free(void) { return !s_ok || s_mode == M_FREE; }

int combat_james_clip(float* blend)
{
	*blend = s_cur ? clip_blend(s_cur) : 0.0f;
	return (int)(uintptr_t)s_cur;
}

bool combat_james_anim(const Anim** anim, float* frame)
{
	if (!s_ok || !s_anim_ok)
		return false;
	*anim = &s_anim;
	float f = s_frame;
	if (s_cur && s_cur->loop && f > s_cur->end)
		f = s_cur->start;
	*frame = f;
	return true;
}

static void draw_flash(int uBones)
{
	if (s_flash <= 0.0f || !s_flash_vbo || !is_gun())
		return;
	// the quads sit on the muzzle and follow the hand bone
	C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, uBones, 3);
	const Mtx34* m = &s_james->skin[pdef()->hand];
	for (int r = 0; r < 3; r++) {
		float t = m->m[r][0] * s_muzzle[0] + m->m[r][1] * s_muzzle[1] + m->m[r][2] * s_muzzle[2] + m->m[r][3];
		u[r] = FVec4_New(m->m[r][0], m->m[r][1], m->m[r][2], t);
	}
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_flash_vbo, CHR_VERTEX_SIZE, 5, 0x43210);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF60D0FF);               // ABGR: warm yellow
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 12);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

void combat_draw_weapon(int uBones)
{
	// Enhanced Edition "CutsceneUnequip" (Patches/RemoveWeaponFromCutscene.cpp, zlib licence): the
	// PC leaves the weapon in James' hand in scenes that use the gameplay James, where it hangs
	// "weightless" while he gestures. The port's scene characters never carry one; this covers the
	// scenes that draw the gameplay James instead (demo_has_james() false).
	if (!s_ok || !s_wmodel_ok || demo_active())
		return;
	for (u32 i = 0; i < s_wmodel.n_bones && i < CHR_MAX_BONES; i++)
		s_wmodel.skin[i] = s_james->skin[pdef()->hand];
	chr_draw(&s_wmodel, uBones);
	draw_flash(uBones);
}

const char* combat_status(void)
{
	static const char* names[] = { "free", "raise", "ready", "swing", "rebound", "recover", "lower",
	                               "shot", "reload", "stomp", "hurt", "held", "dying", "dead" };
	snprintf(s_line, sizeof s_line, "HP%3.0f %s", s_hp, names[s_mode]);
	return s_line;
}

// One line for the help screen: weapon and rounds.
const char* combat_weapon_line(void)
{
	static char line[40];
	const WeaponDef* w = wdef();
	if (is_gun())
		snprintf(line, sizeof line, "%-8s %2d / %-3d", w->name, loaded(), spare());
	else
		snprintf(line, sizeof line, "%-8s       ", w->name);
	return line;
}
