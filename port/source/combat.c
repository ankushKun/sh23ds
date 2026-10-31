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
//   0x533c10  per frame: the candidates in James' sight (0x542300, nearest first), whether one is in
//             clear sight (0x542860), the downed one at his feet (0x542a10); the lock is dropped
//             when none is in clear sight or the target dies. 0x533ca0 locks the nearest in clear
//             sight as the weapon comes up (0x542560, one on its feet first), 0x54f100 / 0x542740
//             move it to the nearest other one on the left or right, 0x5568c0 turns James to it.
//   0x6cf7cc  per grip, what the legs may do under the weapon: 4 walk, 8 run while aiming, 0x40
//             walk, 0x80 run while attacking (0x54e0f0), 0x10 / 0x20 a light swing goes on walking /
//             running (0x556840, 0x556880); 0x52dac0 takes the runs off the spray, the plank and
//             Maria's knife indoors.
//   Every weapon model shares its root bind matrix with James' bone 35 (right hand); bone 1 of the
//   guns is the muzzle (handgun 125, shotgun 294, rifle 483 units out), the plank's second bone is its
//   grip offset (frame 4 of wp/jms_weapon.anm). They are drawn rigidly on bone 35.
#include "combat.h"
#include "shadow.h"
#include "gpusync.h"
#include "controls.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "demo.h"
#include "events.h"
#include "boat.h"
#include "maria.h"
#include "mist.h"
#include "move.h"
#include "prog.h"
#include "sfx.h"

#define PI 3.14159265f

typedef struct { u16 start, end; u8 loop; s16 speed; } JAnim;

// pjames base table (start, end, loop, speed; speed 2048 = 30 frames/s, negative plays backwards)
static const JAnim J_STAND = { 1, 10, 1, 256 };       // 101
static const JAnim J_WALK = { 51, 74, 1, 1408 };      // 104
static const JAnim J_RUN = { 147, 170, 1, 2048 };     // 108
static const JAnim J_HURT = { 453, 470, 0, 1024 };    // 121
// The fall James (or Maria) dies in. The base table's 111 and 112 (0x6ceac4: 221-245, 256-280) are
// the knockdown that lower state 0x18 plays (0x531a00 / 0x52fae0, which of the two by 0x52eb40's
// direction), and both end on their feet again: 112 lies flat from about frame 260 and is standing by
// 276. When the player is dead the PC never leaves the damage motion: 0x559100 sets 0x1fbad17 to 2 at
// its end instead of going back to standing, and EventMain starts the game over then (0x53ad20). Which
// motion the PC's damage code picks for the killing blow is behind the encryption (0x534700 ends in
// 0x56e456), so the port plays the knockdown from where the set falls from standing to its last frame
// on the floor (die_range), and holds it there. The sets differ by a few frames (James' falls start at
// 247..254 and rise at 264..271 by weapon, Maria's lie at 255..263), so each is measured when it loads.
static JAnim J_DIE = { 256, 268, 0, 1024 };
// Lower-body state 0x1d (0x531a00) plays 132 for a kick (request 6) and 133 for a stomp (request 7).
// Both start and end on the standing pose: in 132 the right foot swings up and forward at knee
// height, in 133 it comes down on the floor in front of James.
static const JAnim J_KICK = { 682, 701, 0, 768 };     // 132 (hit frames 7..10)
static const JAnim J_STOMP = { 702, 719, 0, 512 };    // 133 (hit frames 3..9)

typedef struct { int id; float ap, sp, min_range, max_range; u8 first, last; } AttackInfo;
// Kicks are attacks 31..35 and stomps 36..40 (0x5569b0 adds 0x1f or 0x24 to DAT_01fbacb4); every
// one is a kind 1 finish with the same power, only the frame window differs. The kick type follows
// the weapon (0x53b190): 0 bare hands, the handgun and the spray, 1 the long guns, 2 the plank and
// the pipe, 3 the great knife, 4 the chainsaw.
static const AttackInfo ATK_KICKS[5] = {
	{ 31, 9999.9f, 0.1f, 0.0f, 350.0f, 7, 10 }, { 32, 9999.9f, 0.1f, 0.0f, 350.0f, 7, 10 },
	{ 33, 9999.9f, 0.1f, 0.0f, 350.0f, 7, 10 }, { 34, 9999.9f, 0.1f, 0.0f, 350.0f, 9, 9 },
	{ 35, 9999.9f, 0.1f, 0.0f, 350.0f, 7, 11 },
};
static const AttackInfo ATK_STOMPS[5] = {
	{ 36, 9999.9f, 0.1f, 0.0f, 350.0f, 3, 9 }, { 37, 9999.9f, 0.1f, 0.0f, 350.0f, 3, 9 },
	{ 38, 9999.9f, 0.1f, 0.0f, 350.0f, 3, 9 }, { 39, 9999.9f, 0.1f, 0.0f, 350.0f, 3, 9 },
	{ 40, 9999.9f, 0.1f, 0.0f, 350.0f, 3, 9 },
};
#define ATK_KICK (ATK_KICKS[0])
#define ATK_STOMP (ATK_STOMPS[0])

// Melee weapons: the clips of each grip's table and its attacks (sh2_attack_list 0x6ec568). Index
// [s_side] is the shoulder. "light" is request 3 (0x533ff0: a light press), "heavy" request 4 (a
// hard press), "extra" request 5 (the pipe's lunge, the chainsaw's thrust). A clip with end 0 is
// not there.
enum { MK_PLANK, MK_PIPE, MK_NATA, MK_CSAW };
typedef struct {
	u8 kind, kick;
	JAnim raise[2], lower[2], ready[2];
	JAnim light[2], combo[2], rebound[2], recover[2], heavy[2], extra[2];
	AttackInfo a_light[2], a_combo, a_heavy[2], a_extra;
	int snd_swing, snd_hit;
	u8 f_light[2], f_combo, f_heavy[2], f_extra;    // frame of the swing sound and the effort (0x549e20)
	// A blow that meets an enemy plays back (0x5350d0, 0x52fae0): "rebound" for the light and the
	// combo swing, these for the heavy swing and the lunge; rb_skip is how far back the full light
	// swing's place is moved into its "rebound" entry (0x52fae0 takes 0x4000 off the plank's 363 /
	// 369, 0x6000 off the pipe's 409 / 415). Only the plank and the pipe play back (grips 5 and 6).
	JAnim rb_heavy[2], rb_extra[2];
	u8 rb_skip;
	bool rebounds;
} MeleeDef;

static const MeleeDef MELEE_PLANK = {
	MK_PLANK, 2,
	{ { 722, 733, 0, 2048 }, { 734, 745, 0, 2048 } }, { { 722, 733, 0, -1794 }, { 734, 745, 0, -1794 } },  // 351, 353 / 352, 354
	{ { 746, 755, 1, 320 }, { 756, 765, 1, 320 } },                                                      // 359, 360
	{ { 776, 797, 0, 1536 }, { 810, 831, 0, 1536 } }, { { 780, 797, 0, 1536 }, { 814, 831, 0, 1536 } },   // 363, 369 / 365, 371
	{ { 781, 797, 0, -1024 }, { 815, 831, 0, -1024 } }, { { 800, 809, 0, 1024 }, { 834, 843, 0, 1536 } }, // 366, 372 / 367, 373
	{ { 846, 873, 0, 1024 }, { 876, 903, 0, 1024 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },                // 375, 379
	{ { 12, 100.0f, 80.0f, -150.0f, 350.0f, 12, 15 }, { 12, 100.0f, 80.0f, -150.0f, 350.0f, 12, 15 } },
	{ 13, 80.0f, 80.0f, -150.0f, 350.0f, 8, 11 },
	{ { 14, 140.0f, 80.0f, -150.0f, 350.0f, 8, 10 }, { 14, 140.0f, 80.0f, -150.0f, 350.0f, 8, 10 } }, { 0 },
	11027, 11032, { 0, 0 }, 0, { 0, 0 }, 0,
	{ { 846, 873, 0, -1024 }, { 876, 903, 0, -1024 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }, 4, true,     // 376, 380
};
// Maria's knife (grip 10, table 0x6cf454, 21201..21232): the plank's layout, the follow-through and
// the downswing two frames earlier, attacks 28..30
static const MeleeDef MELEE_KNIFE = {
	MK_PLANK, 0,
	{ { 722, 733, 0, 2048 }, { 734, 745, 0, 2048 } }, { { 722, 733, 0, -1794 }, { 734, 745, 0, -1794 } },
	{ { 746, 755, 1, 320 }, { 756, 765, 1, 320 } },
	{ { 776, 797, 0, 1536 }, { 810, 831, 0, 1536 } }, { { 780, 797, 0, 1536 }, { 814, 831, 0, 1536 } },
	{ { 780, 797, 0, -1024 }, { 814, 831, 0, -1024 } }, { { 798, 809, 0, 1024 }, { 832, 843, 0, 1536 } },   // 21216, 21222
	{ { 844, 873, 0, 1024 }, { 874, 903, 0, 1024 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
	{ { 28, 60.0f, 0.0f, -100.0f, 250.0f, 12, 15 }, { 28, 60.0f, 0.0f, -100.0f, 250.0f, 12, 15 } },
	{ 29, 35.0f, 0.0f, -100.0f, 250.0f, 8, 11 },
	{ { 30, 60.0f, 0.0f, -100.0f, 250.0f, 8, 10 }, { 30, 60.0f, 0.0f, -100.0f, 250.0f, 8, 10 } }, { 0 },
	17034, 17035, { 0, 0 }, 0, { 0, 0 }, 0,
};
// Steel pipe (grip 6, table 0x6cf04c, 401..432): attacks 15 swing, 16 short swing, 17 overhead, 18
// lunge; swing sound and effort on frames 8, 2, 15 (13 the effort), 8 (7); hit 11040
static const MeleeDef MELEE_PIPE = {
	MK_PIPE, 2,
	{ { 722, 731, 0, 2048 }, { 732, 741, 0, 2048 } }, { { 722, 731, 0, -2048 }, { 732, 741, 0, -2048 } },  // 401, 403 / 402, 404
	{ { 742, 751, 1, 256 }, { 752, 761, 1, 256 } },                                                      // 405, 406
	{ { 772, 795, 0, 1536 }, { 807, 830, 0, 1536 } }, { { 778, 795, 0, 1536 }, { 813, 830, 0, 1536 } },   // 409, 415 / 411, 417
	{ { 778, 795, 0, -768 }, { 813, 830, 0, -768 } }, { { 796, 806, 0, 1536 }, { 831, 841, 0, 1536 } },   // 412, 418 / 413, 419
	{ { 842, 876, 0, 1792 }, { 877, 911, 0, 1792 } }, { { 912, 941, 0, 1024 }, { 942, 971, 0, 1024 } },   // 421, 425 / 429, 431
	{ { 15, 120.0f, 100.0f, -150.0f, 540.0f, 10, 12 }, { 15, 120.0f, 100.0f, -150.0f, 540.0f, 10, 12 } },
	{ 16, 96.0f, 100.0f, -150.0f, 540.0f, 4, 6 },
	{ { 17, 160.0f, 100.0f, -150.0f, 540.0f, 15, 17 }, { 17, 160.0f, 100.0f, -150.0f, 540.0f, 15, 17 } },
	{ 18, 130.0f, 50.0f, -150.0f, 540.0f, 8, 10 },
	11027, 11040, { 8, 8 }, 2, { 15, 15 }, 8,
	{ { 842, 876, 0, -1536 }, { 877, 911, 0, -1536 } }, { { 912, 941, 0, -896 }, { 942, 971, 0, -896 } }, 6, true,  // 422, 426 / 430, 432
};
// Great knife (grip 8, table 0x6cf1dc, 451..466): one swing from each shoulder in turn, attacks 19 /
// 20 and the heavier 21 / 22 by shoulder, reach 725, no combo; sound 11039 on frames 8 / 11 and
// 17 / 18; hit 11038
static const MeleeDef MELEE_NATA = {
	MK_NATA, 3,
	{ { 742, 753, 0, 1024 }, { 722, 741, 0, 1024 } }, { { 742, 753, 0, -1024 }, { 722, 741, 0, -1024 } },  // 453, 451 / 454, 452
	{ { 774, 783, 1, 256 }, { 784, 793, 1, 256 } },                                                      // 457, 458
	{ { 829, 863, 0, 768 }, { 794, 828, 0, 768 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },                  // 461, 459
	{ { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
	{ { 904, 943, 0, 768 }, { 864, 903, 0, 768 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },                  // 465, 463
	{ { 19, 600.0f, 0.0f, 0.0f, 725.0f, 10, 14 }, { 20, 600.0f, 0.0f, 0.0f, 725.0f, 13, 17 } }, { 0 },
	{ { 21, 1000.0f, 0.0f, 0.0f, 725.0f, 17, 18 }, { 22, 1000.0f, 0.0f, 0.0f, 725.0f, 18, 19 } }, { 0 },
	11039, 11038, { 8, 11 }, 0, { 17, 18 }, 0,
};
// Chainsaw (grip 7, table 0x6cf2ac, 551..573): the saw (attack 23, frames 12..27 of a 50 frame clip)
// and the thrust (24); the engine revs (11044) as James raises it and runs (11045) while he saws;
// the cut 11046. No fuel: the PC never runs it dry.
static const MeleeDef MELEE_CSAW = {
	MK_CSAW, 4,
	{ { 762, 786, 0, 1280 }, { 728, 746, 0, 1280 } }, { { 787, 798, 0, -2048 }, { 747, 758, 0, -2048 } },  // 555, 551 / 558, 554
	{ { 812, 821, 1, 2048 }, { 802, 811, 1, 2048 } },                                                    // 560, 559
	{ { 892, 941, 0, 1024 }, { 842, 891, 0, 1024 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },                // 567, 565
	{ { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
	{ { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }, { { 967, 991, 0, 1024 }, { 942, 966, 0, 1024 } },                // 571, 569
	{ { 23, 250.0f, 0.0f, 0.0f, 525.0f, 12, 27 }, { 23, 250.0f, 0.0f, 0.0f, 525.0f, 12, 27 } }, { 0 },
	{ { 0 }, { 0 } }, { 24, 300.0f, 0.0f, 0.0f, 525.0f, 6, 13 },
	11045, 11046, { 0, 0 }, 0, { 0, 0 }, 0,
};
static const MeleeDef* const MELEE_ALL[] = { &MELEE_PLANK, &MELEE_KNIFE, &MELEE_PIPE, &MELEE_NATA, &MELEE_CSAW };

// Hyper spray (grip 4, table 0x6cedec, 501..516): raise 501, aim 505, spray 508 (attack 8, frames
// 8..25), spraying on 509 (attack 9, frames 2..19) while the button stays down, 510 when the can is
// empty. The can holds 20 seconds of spray (PlayerInit 0x533ad0); spraying uses it up in real time,
// costs James a point of health a second on normal and hard (0x545260), and an empty can is gone.
static const JAnim S_SPRAY = { 742, 767, 0, 1536 };     // 508
static const JAnim S_AGAIN = { 748, 767, 0, 1536 };     // 509
static const JAnim S_EMPTY = { 768, 799, 0, 1536 };     // 510
static const AttackInfo ATK_SPRAY[2] = { { 8, 1.0f, 5.0f, 0.0f, 1000.0f, 8, 25 }, { 9, 1.0f, 5.0f, 0.0f, 1000.0f, 2, 19 } };
#define SPRAY_FULL 20.0f

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
// James has W_NONE..W_SPRAY and Maria (Born From a Wish) W_MNONE..W_KNIFE, the rows 10..12 of the
// PC's weapon file table (0x88d2f0). The grip -> item table (0x534240): 4 hyper spray 12, 5 plank
// 13, 6 steel pipe 14, 7 chainsaw 16, 8 great knife 15.
enum { W_NONE, W_PLANK, W_HANDGUN, W_SHOTGUN, W_RIFLE, W_PIPE, W_NATA, W_CSAW, W_SPRAY, W_MNONE, W_COLT, W_KNIFE,
       W_COUNT };
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
	[W_PIPE] = { "steel pipe", "jms_wppipe", "wp_pipe", 14, 0, 0, 0, 6 },
	[W_NATA] = { "great knife", "jms_wpnata", "wp_nata", 15, 0, 0, 0, 8 },
	[W_CSAW] = { "chainsaw", "jms_wpcsaw", "wp_csaw", 16, 0, 0, 0, 7 },
	// the spray's raise, lower and aim are 501, 502 and 505; it fires through M_SPRAY
	[W_SPRAY] = { "hyper spray", "jms_wpsp", "wp_sp", 12, 0, 0, 0, 4,
		{ 722, 731, 0, 1280 }, { 722, 731, 0, -1280 }, { 732, 741, 1, 256 } },
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
	{ "jms", "wp", "hll_jms", 35, W_NONE, W_SPRAY, { 450.0f, 975.0f, 1550.0f }, 11020, 4, 11024 },
	{ "xmar", "xmar", "lxx_mar", 32, W_MNONE, W_KNIFE, { 300.0f, 650.0f, 930.0f }, 17029, 5, 19041 },
};

enum { M_FREE, M_RAISE, M_READY, M_SWING, M_REBOUND, M_RECOVER, M_LOWER, M_SHOT, M_RELOAD, M_STOMP, M_SPRAY,
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
static bool s_had_plank;         // the plank was owned last frame (picking it up equips it)
static bool s_ok;
static float s_hp = 100.0f;
static float s_hp_recover;        // shPlayerWork.hp_recover: healing still to come, drained over time
static int s_mode;
static int s_side;                // plank: 0 over the right shoulder (359), 1 left (360)
static const JAnim* s_cur;
static float s_frame;
static bool s_end;
static const AttackInfo* s_atk;
static bool s_atk_heavy;                // the swing is the heavy one (a downed enemy's finish)
static int s_snd_frame = -1;            // the frame of the clip the swing sound waits for
static float s_spray = SPRAY_FULL;      // seconds of spray left in the can (0x1fbacf0)
static bool s_spray_again;              // the spray button stayed down: 509 follows
static float s_body_pos[3];             // James' feet this frame, for positional sounds
static bool s_run_held;                 // run held this frame (the pipe's swing on hard)
static bool s_hit_any, s_fired, s_reloaded;
// 0x544d43: the ap of James' attack that struck the room instead of a character this frame (PC
// 0x1d82478, read by stage code through 0x544fd0: the well wall at the bottom of the hole)
static float s_wall_ap, s_wall_ap_next;
static bool s_swing_wall;
static bool s_hit[ENEMY_MAX];
static bool s_combo_req;
static bool s_hit_down;                 // the enemy the blow met was down (0x5350d0: no play back)
static bool s_rb_light;                 // the rebound plays back a light or combo swing (presses count)
// The Enhanced Edition's SwapLightHeavyAttack keeps a released tap for 0.3 s
// (kLightAttackBufferTimerMax) and asks for the heavy swing once A has been held 0.2 s; the game
// reads either when it next asks for an attack (0x533ff0), here as the follow-through ends.
static int s_queue;                     // 0 none, 1 light, 2 heavy
static float s_queue_t, s_qhold = -1.0f;
static bool s_shot_queued;              // 0x544ac0: the next shot asked for during this one
// 3DS-only: an Action press while the weapon comes up (Aim and Action pressed together, or close)
// is kept and taken the frame it is ready, for every weapon. The PC drops it unless the Enhanced
// Edition's 0.3 s tap buffer (melee, Swap Light/Heavy) still holds it as the raise ends.
static bool s_raise_press, s_ready_press;
static const AttackInfo* s_stomp_atk;   // ATK_KICK or ATK_STOMP while M_STOMP
static float s_hold;                    // grab meter (DAT_01fbacd4): frame times of struggling left
static int s_stick_dir;                 // last circle pad direction (0 centre, 1..4), for flicks
static float s_invuln, s_dead_t, s_flash;
static float s_die_t;                    // since the killing blow (combat_dying_time)
static float s_hit_t = 99.0f;            // since the last blow that hurt (combat_hit_time)
static float s_tip[3], s_root[3];       // melee weapon's grip and far end in James' bind space
static float s_prev_end[3];             // the blow's far end the frame before (0x543b10 keeps it at +0x128)
static bool s_prev_ok;
static float s_muzzle[3];               // gun muzzle in James' bind space
static void* s_flash_vbo;               // linear memory, muzzle flash quads (S3C vertex layout)
static char s_line[48];
static const Collision* s_col;          // the room, for shots
static Enemy* s_target;                 // the locked target (0x1fbacc4), see combat_scan
static Enemy* s_cand[ENEMY_MAX];        // 0x542300: enemies in James' sight, nearest first
static int s_ncand;
static Enemy* s_vis;                    // 0x542860: the nearest candidate in clear sight
static Enemy* s_stompable;              // 0x542a10: a downed one ahead within 600 (kick / stomp)
static bool s_attack_seq;               // 0x1fbad16 != 0xff: an attack since the weapon came up
static bool s_swing_legs;               // this swing lets the legs walk or run on (0x5569b0, 0x556840)
static float s_action_held = -1.0f;     // seconds Action has been held, -1 up (0x52e110 key history)
static Enemy* s_stomp_e;                // the downed one a kick or stomp turns James to
static int s_place;                     // 0x1fba8d8 (combat_set_place)
static bool s_god;                      // debug.txt "god 1": blows do not reach James (for testing)
// A Flesh Lip that kills James in its hold carries the body up into the dark (see
// combat_carry_james)
static bool s_carried;
static float s_carry[3];
static float s_col_r = 200.0f, s_col_ahead;   // James' push column (combat_column)
static bool s_die_held;                       // the blow that killed him came in a hold

// How a clip starts (0x531a00 / 0x52fae0 and the modes of 0x538920): standing, walking, raising,
// aiming and lowering crossfade over 8 frames (mode 4); swings, shots, reloads, kicks, stomps, hits
// and falls cut in (mode 2).
static float clip_blend(const JAnim* a)
{
	if (a == &J_STAND || a == &J_WALK || a == &J_RUN)
		return 8.0f / 30.0f;
	for (unsigned m = 0; m < sizeof MELEE_ALL / sizeof MELEE_ALL[0]; m++)
		for (int i = 0; i < 2; i++)
			if (a == &MELEE_ALL[m]->raise[i] || a == &MELEE_ALL[m]->lower[i] || a == &MELEE_ALL[m]->ready[i])
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

static bool s_inf_ammo;          // Cheats: firing takes no rounds
static const WeaponDef* wdef(void) { return &WEAPONS[s_weapon]; }
static const PlayerDef* pdef(void) { return &PLAYERS[s_maria]; }
static bool is_gun(void) { return wdef()->clip > 0; }
static bool is_bare(void) { return s_weapon == W_NONE || s_weapon == W_MNONE; }
static bool is_melee(void)
{
	return s_weapon == W_PLANK || s_weapon == W_KNIFE || s_weapon == W_PIPE || s_weapon == W_NATA || s_weapon == W_CSAW;
}
static const MeleeDef* md(void)
{
	switch (s_weapon) {
	case W_PIPE: return &MELEE_PIPE;
	case W_NATA: return &MELEE_NATA;
	case W_CSAW: return &MELEE_CSAW;
	case W_KNIFE: return &MELEE_KNIFE;
	default: return &MELEE_PLANK;
	}
}
static int kick_type(void)
{
	switch (s_weapon) {
	case W_SHOTGUN: case W_RIFLE: return 1;
	case W_PLANK: case W_PIPE: case W_NATA: case W_CSAW: return md()->kick;
	default: return 0;
	}
}
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
	if (w == W_NONE || w == W_MNONE)
		return true;
	if (w == W_PLANK)
		return ev_item_count(d->item) > 0;   // picked up with the first Lying Figure (prog_town.c)
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

// 0x545c80: a melee weapon's blow runs from its bone 0 (the grip) toward its bone 1 (the far end);
// a model without the second bone points at its farthest vertex instead.
static void plank_segment(void)
{
	bind_pos(&s_wmodel, 0, s_root);
	if (s_wmodel.n_bones > 1) {
		bind_pos(&s_wmodel, 1, s_tip);
		return;
	}
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
		gpu_sync();      // the GPU may still read the old weapon this frame
	if (s_wmodel_ok)
		chr_free(&s_wmodel);
	if (s_anim_ok)
		anim_free(&s_anim);
	s_wmodel_ok = s_anim_ok = false;
}

// J_DIE for a set: the legs root (bone 1) on the floor is within 150 units of it, standing about 550
// above; the fall starts where it was last coming down from standing, the hold is the last frame on
// the floor before it rises. Looked for round the base table's knockdown (frames 240..285).
static void die_range(const Anim* a)
{
	J_DIE.start = 256, J_DIE.end = 268;
	if (a->bones < 2 || a->frames <= 285)
		return;
	float y[286];
	float cur = -550.0f;
	for (u32 f = 0; f < 286; f++) {
		const AnmKey* k = &a->keys[f * a->bones + 1];
		if (k->flags & 2)
			cur = k->trans[1];
		y[f] = cur;
	}
	int lo = -1, hi = -1;
	for (int f = 240; f < 286; f++) {
		if (y[f] > -150.0f) {
			if (lo < 0)
				lo = f;
			hi = f;
		} else if (lo >= 0) {
			break;
		}
	}
	if (lo < 0)
		return;
	int s = lo;
	while (s > 240 && y[s - 1] < y[s] - 5.0f)
		s--;
	J_DIE.start = (u16)s, J_DIE.end = (u16)hi;
}

static bool load_weapon(int w)
{
	s_target = NULL;                 // 0x534610 with every change of weapon
	s_attack_seq = false;
	unload_weapon();
	const WeaponDef* d = &WEAPONS[w];
	char path[128], dir[96], err[128];
	snprintf(path, sizeof path, "%s/chr/%s/%s.s3a", s_pack, pdef()->dir, d->anm);
	s_anim_ok = anim_load(&s_anim, path) && s_anim.bones == s_james->n_bones;
	if (!s_anim_ok)
		return false;
	die_range(&s_anim);
	if (d->mdl) {
		snprintf(dir, sizeof dir, "%s/chr/%s", s_pack, pdef()->wdir);
		s_wmodel_ok = chr_load(&s_wmodel, dir, d->mdl, err, sizeof err);
		if (s_wmodel_ok) {
			if (w == W_PLANK || w == W_KNIFE || w == W_PIPE || w == W_NATA || w == W_CSAW)
				plank_segment();       // the blade or shaft: the grip and its direction
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
	s_ncand = 0;
	s_vis = s_stompable = s_stomp_e = NULL;
	s_attack_seq = false;
	s_carried = false;
	s_col_r = 200.0f, s_col_ahead = 0.0f, s_die_held = false;   // 0x536f10
	int w = s_maria ? W_MNONE : W_NONE;    // bare hands until a weapon is picked up
	if (s_ok && s_weapon != w)
		load_weapon(w);
	s_had_plank = false;
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
		gpu_sync();      // the GPU may still read the old model this frame
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
		if (s_maria || !owns(W_PLANK) || !load_weapon(W_PLANK))
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
	               (s_mode == M_SWING && s_atk_heavy) || s_mode == M_STOMP };
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
	if (s_god)
		return;
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

// James' push column (the PC's DAT_01fbaaa0): 0x52ed60 asks for a radius and a distance ahead of
// his feet by the upper-body state (0x1fba8e4) and the grip, 0x534d70 eases both 5 units a 1/30 s
// toward them (from 200 and 0, 0x536f10), and 0x535010 registers it while 0x1fbad17 < 2 (none
// once the death motion is over). Standing, walking, kicking, hurt: 175, 25 ahead (Maria 150).
// HOLD (the port's ready) and ATTACK (swings, shots, reloads, the spray) take the grip's row of
// 0x52ed60's table, in metres; the handguns held after an attack on the left side reach 275, 115
// ahead, and the great knife holds 265 at 50 ahead (right side) or 50 behind. Dying (DAMAGE with
// 0x1fbad17 set): 375, or none when the blow that killed him was attack 54 or 61 (SC +0x114); the
// port takes a death in a hold for those. At the place 0x1fba8d8 == 2 the radius is the standing one.
void combat_column(float dt, float* radius, float* ahead)
{
	static const float ROW[10][4] = {   // per grip 1..10: HOLD radius, ahead, ATTACK radius, ahead
		{ 0.35f, 0.05f, 0.55f, 0.23f }, { 0.45f, 0.05f, 0.55f, 0.05f }, { 0.65f, 0.2f, 0.65f, 0.2f },
		{ 0.4f, 0.15f, 0.45f, 0.17f }, { 0.4f, 0.2f, 0.45f, 0.2f }, { 0.4f, 0.1f, 0.5f, 0.1f },
		{ 0.45f, 0.12f, 0.45f, 0.12f }, { 0.45f, 0.18f, 0.5f, 0.18f }, { 0.3f, 0.05f, 0.5f, 0.23f },
		{ 0.35f, 0.1f, 0.4f, 0.1f },
	};
	if (!s_ok || s_mode == M_DEAD) {
		*radius = *ahead = 0.0f;
		return;
	}
	float stand = s_maria ? 150.0f : 175.0f, r = stand, a = 25.0f;
	int grip = WEAPONS[s_weapon].grip;
	int side = s_attack_seq ? s_side : 0xff;        // 0x1fbad16
	switch (s_mode) {
	case M_READY:
		if (grip >= 1 && grip <= 10)
			r = ROW[grip - 1][0] * 500.0f, a = ROW[grip - 1][1] * 500.0f;
		if ((grip == 1 || grip == 9) && side == 1)
			r = 275.0f, a = 115.0f;
		if (grip == 8)
			r = 265.0f, a = side == 0 ? 50.0f : -50.0f;
		break;
	case M_SWING: case M_REBOUND: case M_RECOVER: case M_SHOT: case M_RELOAD: case M_SPRAY:
		if (grip >= 1 && grip <= 10)
			r = ROW[grip - 1][2] * 500.0f, a = ROW[grip - 1][3] * 500.0f;
		break;
	case M_DYING:
		r = s_die_held ? 0.0f : 375.0f;
		break;
	default:
		break;
	}
	if (s_place == 2)
		r = stand;
	float step = dt * 30.0f * 5.0f;
	s_col_r += r > s_col_r + step ? step : r < s_col_r - step ? -step : r - s_col_r;
	s_col_ahead += a > s_col_ahead + step ? step : a < s_col_ahead - step ? -step : a - s_col_ahead;
	*radius = s_col_r;
	*ahead = s_col_ahead;
}
float combat_dying_time(void) { return combat_dying() ? s_die_t : 0.0f; }
float combat_hit_time(void) { return s_hit_t; }

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
		s_die_t = 0.0f;
		s_die_held = true;
		play(&J_DIE);
	}
}

void combat_set_god(bool on) { s_god = on; }

void combat_damage_james(int attack_id, float ap, float from_x, float from_z)
{
	(void)from_x, (void)from_z;
	if (s_god)
		return;
	if (s_mode >= M_DYING || s_mode == M_HELD || s_invuln > 0.0f)
		return;
	s_hp -= s_test_god ? 0.0f : ap;
	s_hit_t = 0.0f;
	if (s_hp <= 0.0f) {
		s_hp = 0.0f;
		sfx_play(pdef()->snd_die, 1.0f, 0.0f);   // dying scream
		s_mode = M_DYING;
		s_die_t = 0.0f;
		s_die_held = false;
		play(&J_DIE);
		return;
	}
	if (attack_id == 42 && s_mode >= M_RAISE && s_mode <= M_RELOAD) {
		// The acid spray (kind 4, shBattleAttackByEnemyFog) burns without knocking the weapon down:
		// James keeps fighting. Blows (kinds 2 and 3) make him flinch.
		s_invuln = 1.0f;
		sfx_play(pdef()->snd_hurt + (int)(s_hp) % pdef()->n_hurt, 1.0f, 0.0f);
		return;
	}
	// 0x5350d0: a blow that lands while the damage motion still plays (0x1fbad18 set, cleared by the
	// damage state 0x547900 / 0x54db90 at the motion's end) takes its health and nothing more; the
	// motion is not started again, so a room of biting creepers cannot keep James from moving.
	if (s_mode == M_HURT)
		return;
	s_mode = M_HURT;
	s_invuln = 1.0f;     // a port stand-in: no such timer found in 0x5350d0 (STATUS section 3)
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

// 0x5420f0's look sphere alone (bits 0 and 1 of the candidate's flags): ahead of James
static bool in_look(const CombatBody* b, float x, float z)
{
	int mode = !s_dark ? 0 : s_maria ? 3 : s_light ? 1 : 2;
	int lvl = enemy_difficulty();
	lvl = lvl < 0 ? 0 : lvl > 4 ? 4 : lvl;
	float lc = LOOK_AREA[mode][lvl] * 500.0f;
	return hypotf(x - (*b->x + sinf(*b->yaw) * lc), z - (*b->z + cosf(*b->yaw) * lc)) <= lc;
}

static float dist3(const CombatBody* b, const Enemy* e)
{
	float dx = e->x - *b->x, dy = e->y - *b->y, dz = e->z - *b->z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// 0x533c10 PlayerGetTargetInfo, once a frame (combat_scan): the candidate list (0x542300: the enemies in James' look or
// feel area, sorted by distance; the PC's list also holds Maria and items, which the lock never
// takes), the nearest one in clear sight (0x542860) and the downed one to kick (0x542a10). The lock
// is dropped when no candidate is in clear sight or the target is dead; it is NOT dropped when only
// the target goes out of sight while another candidate stays in view.
void combat_scan(const CombatBody* b)
{
	s_ncand = 0;
	float d[ENEMY_MAX];
	for (int i = 0; i < enemy_count() && s_ncand < ENEMY_MAX; i++) {
		Enemy* e = enemy_get(i);
		if (!enemy_alive(e) || !in_sight(b, e))
			continue;
		float de = dist3(b, e);
		int k = s_ncand++;
		while (k > 0 && d[k - 1] > de) {
			d[k] = d[k - 1];
			s_cand[k] = s_cand[k - 1];
			k--;
		}
		d[k] = de;
		s_cand[k] = e;
	}
	s_vis = NULL;
	s_stompable = NULL;
	for (int i = 0; i < s_ncand; i++) {
		Enemy* e = s_cand[i];
		bool clear = clear_line(b, e);
		if (clear && !s_vis)
			s_vis = e;
		if (clear && !s_stompable && enemy_is_down(e) && in_look(b, e->x, e->z) && d[i] < 600.0f)
			s_stompable = e;
	}
	// 3DS-only: else a body he killed and has not walked away from (enemy_corpse_stompable)
	for (int i = 0; i < enemy_count() && !s_stompable; i++) {
		Enemy* e = enemy_get(i);
		if (enemy_corpse_stompable(e) && dist3(b, e) < 600.0f && in_look(b, e->x, e->z) && clear_line(b, e))
			s_stompable = e;
	}
	if (s_target && (!s_vis || !enemy_alive(s_target)))
		s_target = NULL;
}

// 0x542560 (dir 0): the first candidate in clear sight, one still on its feet before one on the floor.
// 0x542740 (dir +1 right, -1 left, by the angle from James' facing): the first other candidate on that
// side in clear sight, down or not.
static Enemy* pick_target(const CombatBody* b, int dir)
{
	Enemy* down = NULL;
	for (int i = 0; i < s_ncand; i++) {
		Enemy* e = s_cand[i];
		if (!enemy_alive(e) || (dir && e == s_target))
			continue;
		if (dir && (angle_to(b, e) > 0.0f) != (dir > 0))
			continue;
		if (!clear_line(b, e))
			continue;
		if (dir || !enemy_is_down(e))
			return e;
		if (!down)
			down = e;
	}
	return down;
}

// 0x533ca0 PlayerGetTarget: the lock as the weapon comes up, when there are candidates and none is locked
static void lock_on(const CombatBody* b)
{
	if (s_ncand && !s_target)
		s_target = pick_target(b, 0);
}

// 0x5568c0 PlayerCheckAimingToEnemy: James turns to e by 0.3235 x 15 of the gap a second, landing on it rather than past
static void face(CombatBody* b, const Enemy* e, float dt)
{
	if (!e)
		return;
	float to = atan2f(e->x - *b->x, e->z - *b->z);
	float gap = remainderf(*b->yaw - to, 2.0f * PI);
	float st = gap * 0.3234578690569946f * dt * 15.0f;
	if ((gap < 0.0f && gap - st >= 0.0f) || (gap >= 0.0f && gap - st <= 0.0f))
		*b->yaw = to;
	else
		*b->yaw -= st;
	*b->yaw = remainderf(*b->yaw, 2.0f * PI);
}

// 0x54f100 with 0x533cd0 PlayerChangeTarget: the target keys move the lock while one is held
static void change_target(const CombatBody* b, u32 down)
{
	if (!s_target)
		return;
	Enemy* t = NULL;
	if (down & GK_TARGET_R)
		t = pick_target(b, 1);
	if (down & GK_TARGET_L)
		t = pick_target(b, -1);
	if (t)
		s_target = t;
}

// The target fire() aims at (PlayerGetTarget): the locked one, within reach.
static Enemy* find_target(const CombatBody* b, float* angle, float reach)
{
	Enemy* t = s_target;
	if (!t || !enemy_alive(t) || hypotf(t->x - *b->x, t->z - *b->z) > reach)
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

// The looping clip s_frame is in, for chr_pose_loop (0, 0: none)
static void cur_loop(int loop[2])
{
	bool l = s_cur && s_cur->loop;
	loop[0] = l ? s_cur->start : 0;
	loop[1] = l ? s_cur->end : 0;
}

static void pose_james(void)
{
	int l[2];
	cur_loop(l);
	chr_pose_loop(s_james, &s_anim, s_frame, l[0], l[1]);
}

// shBattleAttackByHumanFightType (PS2 sh_character_battle.c): 0x543b10 builds the blow every frame
// of the attack, from the weapon's grip along it (0x545c80) out to the attack's min_range and
// max_range (the plank -150..350, so it reaches back past the hands to a body pressed against James),
// and keeps its far end (+0x128). Inside the frame window the check (0x527b60 for kind 2, 0x527cd0
// for kind 1) tries five blows from this frame's start: to this frame's far end, then to points
// 0.3, 0.5, 0.7 and all the way back to last frame's, until one meets something; a blow that
// starts inside a column counts (0x528f00). The plank's and the pipe's attacks (12..18) stop
// checking after their first hit (0x543b10 needs +0x124 clear). The checks run after the
// characters' push (0x50b1e0), so a body is never inside James.
static void swing_hits(const CombatBody* b)
{
	if (!s_atk)
		return;
	float f = s_frame - s_cur->start;
	bool window = f >= s_atk->first && f <= s_atk->last + 0.99f;
	if (!window && f > s_atk->last + 0.99f)
		return;
	pose_james();
	float a[3], t[3], s0[3], e0[3], pe[3];
	to_world(b, s_root, a);
	to_world(b, s_tip, t);
	float d[3] = { t[0] - a[0], t[1] - a[1], t[2] - a[2] };
	float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (dl < 1e-3f)
		return;
	for (int k = 0; k < 3; k++) {
		d[k] /= dl;
		s0[k] = a[k] + d[k] * s_atk->min_range;
		e0[k] = a[k] + d[k] * s_atk->max_range;
		pe[k] = s_prev_ok ? s_prev_end[k] : e0[k];
		s_prev_end[k] = e0[k];
	}
	s_prev_ok = true;
	if (!window || (s_hit_any && s_atk->id >= 12 && s_atk->id <= 18))
		return;
	float fx = sinf(*b->yaw), fz = cosf(*b->yaw);
	float rx = fz, rz = -fx;                       // James' right
	if (!s_hit_any && !s_swing_wall && s_col) {
		// the weapon's far end inside a wall: the blow strikes the room
		float ex = e0[0], ez = e0[2];
		col_clamp_ray(s_col, *b->x, *b->z, &ex, &ez, e0[1]);
		if (hypotf(ex - e0[0], ez - e0[2]) > 1.0f) {
			s_swing_wall = true;
			s_wall_ap_next = s_atk->ap;
		}
	}
	static const float BACK[5] = { 0.0f, 0.3f, 0.5f, 0.7f, 1.0f };   // 0x8bcdb8
	for (int k = 0; k < 5; k++) {
		float e[3];
		for (int c = 0; c < 3; c++)
			e[c] = e0[c] + (pe[c] - e0[c]) * BACK[k];
		bool met = false;
		for (int i = 0; i < enemy_count(); i++) {
			Enemy* en = enemy_get(i);
			if (!enemy_alive(en) || s_hit[i])
				continue;
			float cx, cz, r, top, bottom;
			enemy_hit_cylinder(en, &cx, &cz, &r, &top, &bottom);
			float ylo = s0[1] < e[1] ? s0[1] : e[1], yhi = s0[1] < e[1] ? e[1] : s0[1];
			if (yhi < top || ylo > bottom)
				continue;
			if (seg_dist2d(s0, e, cx, cz) > r)
				continue;
			s_hit[i] = true;
			s_hit_any = true;
			s_hit_down = enemy_is_down(en);
			met = true;
			// the hit (0x5350d0): the plank's 11032, the knife's 17035, both at 0.8
			sfx_play(md()->snd_hit, 0.8f, 0.0f);
			float sgn = s_side == 0 ? -0.5f : 0.5f;    // the side the blow travels toward
			enemy_damage(en, s_atk->id, s_atk->ap, s_atk->sp, fx + rx * sgn, fz + rz * sgn);
		}
		if (met)
			break;
	}
}

static u8 grip_flags(void);

static void start_swing(const CombatBody* b, bool combo, bool heavy)
{
	const MeleeDef* m = md();
	float ang = 0.0f;
	Enemy* t = find_target(b, &ang, 4000.0f);
	// 0x557960 sets 0x1fbad08 while the legs run (lower status 0xc..0x10), and 0x5569b0 then gives
	// the plank and Maria's knife the light swing (request 3) whatever was pressed
	int legs = move_last()->state;
	bool legs_run = legs >= MV_RUN1 && legs <= MV_RSRUN, legs_walk = legs >= MV_BACK && legs <= MV_RSWALK;
	if (m->kind == MK_PLANK && legs_run)
		heavy = false;
	s_attack_seq = true;             // 0x52fae0 sets the attack side byte with every swing
	memset(s_hit, 0, sizeof s_hit);
	s_hit_any = false;
	s_hit_down = false;
	s_prev_ok = false;
	s_swing_wall = false;
	s_combo_req = false;
	s_atk_heavy = false;
	s_snd_frame = -1;
	int side = s_side;
	if (m->kind == MK_PLANK) {
		// heavy: the plank's attack 14 (request 4); a downed target also gets it (port choice, as a
		// level swing passes over a body on the floor)
		if (heavy || (!legs_run && t && enemy_is_down(t) && t->p_dist < 1400.0f)) {
			s_atk = &m->a_heavy[side], s_atk_heavy = true;
			play(&m->heavy[side]);
		} else if (combo) {
			s_atk = &m->a_combo;
			play(&m->combo[side]);
		} else {
			s_atk = &m->a_light[side];
			play(&m->light[side]);
		}
	} else if (m->kind == MK_PIPE) {
		// 0x533ff0 grip 6: a hard press is the overhead (request 4, attack 17); a light one the lunge
		// (5, attack 18), except on hard with run held or a combo pressed, which is the swing (3,
		// attacks 15 / 16)
		if (heavy) {
			s_atk = &m->a_heavy[side], s_atk_heavy = true;
			play(&m->heavy[side]);
			s_snd_frame = m->f_heavy[side];
		} else if (combo) {
			s_atk = &m->a_combo;
			play(&m->combo[side]);
			s_snd_frame = m->f_combo;
		} else if (enemy_difficulty() >= 3 && s_run_held) {
			s_atk = &m->a_light[side];
			play(&m->light[side]);
			s_snd_frame = m->f_light[side];
		} else {
			s_atk = &m->a_extra;
			play(&m->extra[side]);
			s_snd_frame = m->f_extra;
		}
	} else if (m->kind == MK_NATA) {
		// 0x533ff0 grip 8: light 3 or hard 4, attacks 19 / 20 or 21 / 22 by shoulder (0x5569b0)
		s_atk = heavy ? &m->a_heavy[side] : &m->a_light[side];
		s_atk_heavy = heavy;
		play(heavy ? &m->heavy[side] : &m->light[side]);
		s_snd_frame = heavy ? m->f_heavy[side] : m->f_light[side];
	} else {
		// chainsaw, 0x533ff0 grip 7: light 3 saws (attack 23), hard or pressed again 5 thrusts (24)
		if (heavy || combo) {
			s_atk = &m->a_extra;
			play(&m->extra[side]);
		} else {
			s_atk = &m->a_light[side];
			play(&m->light[side]);
		}
		s_atk_heavy = heavy || combo;
		sfx_play_at(m->snd_swing, 0.7f, s_body_pos);   // the engine at work (0x52fae0)
	}
	s_mode = M_SWING;
	// 0x5569b0: a light swing (request 3) of the plank, Maria's knife or the pipe leaves the walking
	// and running states their row; the heavy and the lunge (requests 4, 5), the chainsaw and the
	// great knife take the moves off it and stop him (lower status 0x1a or 0x1c). The walking and
	// running movers then let the swing go on (0x556840 grip flag 0x10, 0x556880 flag 0x20; a run
	// without 0x20 drops to the walk, 0x54e0f0).
	s_swing_legs = (m->kind == MK_PLANK || m->kind == MK_PIPE) && !s_atk_heavy && s_atk != &m->a_extra &&
	               (legs_walk || legs_run) && (grip_flags() & 0x10);
	if (s_maria) {
		// the knife's swish 0x428a with one of her two efforts 0x4286 / 0x4287 (0x549500)
		sfx_play(17034, 1.0f, 0.0f);
		sfx_play(17030 + (rand() & 1), 0.9f, 0.0f);
	} else if (m->kind == MK_PLANK) {
		sfx_play(11027, 1.0f, 0.0f);     // melee swing
	}
}

// 0x549e20: the swing sound and James' effort (11018 or 11019 at up to 0.7) once the clip reaches
// the weapon's frame
static void swing_sound(void)
{
	if (s_snd_frame < 0 || !s_cur || s_frame - s_cur->start < s_snd_frame)
		return;
	s_snd_frame = -1;
	sfx_play_at(md()->snd_swing, 0.9f, s_body_pos);
	sfx_play_at(11018 + (rand() & 1), 0.7f * (float)rand() / (float)RAND_MAX, s_body_pos);
}

static float ray_enemy(const float o[3], const float d[3], float t0, float t1, Enemy** hit);

// 0x545260: the hyper spray's cone. Five rays from the can along James' aim, out to the attack's
// reach (shorter and weaker once less than ten seconds are left), each enemy struck once a spray.
// The centre ray also posts the mist (0x5454dc -> 0x4a4c00): a puff a tick toward its end plus 150.
static void spray_hits(const CombatBody* b, const AttackInfo* a, float dt)
{
	float f = s_frame - s_cur->start;
	if (f < a->first || f > a->last + 0.99f)
		return;
	float k = s_spray < 10.0f ? s_spray * 0.1f : 1.0f;
	float reach = a->max_range * k;
	pose_james();
	float o[3];
	to_world(b, s_muzzle, o);
	static const float fan[5][2] = { { 0, 0 }, { 0.12f, 0 }, { -0.12f, 0 }, { 0, 0.1f }, { 0, -0.1f } };
	for (int r = 0; r < 5; r++) {
		float yaw = *b->yaw + fan[r][0], d[3] = { sinf(yaw) * cosf(fan[r][1]), -sinf(fan[r][1]), cosf(yaw) * cosf(fan[r][1]) };
		float lim = reach;
		if (s_col) {
			float ex = o[0] + d[0] * reach, ez = o[2] + d[2] * reach;
			col_clamp_ray(s_col, o[0], o[2], &ex, &ez, o[1]);
			lim = hypotf(ex - o[0], ez - o[2]);
		}
		if (r == 0) {
			float v[3] = { d[0] * (lim + 150.0f), d[1] * (lim + 150.0f), d[2] * (lim + 150.0f) };
			mist_spray(o, v, dt);
		}
		Enemy* e;
		ray_enemy(o, d, 0.0f, lim, &e);
		if (!e)
			continue;
		int i = 0;
		while (i < enemy_count() && enemy_get(i) != e)
			i++;
		if (i >= ENEMY_MAX || s_hit[i])
			continue;
		s_hit[i] = true;
		enemy_damage(e, a->id, a->ap * k, a->sp * k, d[0], d[2]);
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
	pose_james();
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
	if (!s_inf_ammo)
		ev_give_item(w->item, -1);       // Infinite Ammo (Cheats): the round stays in the gun
	sfx_play(w->snd_shot, 1.0f, 0.0f);
	s_flash = 0.06f;
}

// Downed enemy at James' feet (the PC stomps instead of shooting or swinging bare-handed).
static Enemy* stomp_target(const CombatBody* b)
{
	for (int i = 0; i < enemy_count(); i++) {
		Enemy* e = enemy_get(i);
		if (!(enemy_alive(e) && enemy_is_down(e)) && !enemy_corpse_stompable(e))
			continue;
		float cx, cz, r, top, bottom;
		enemy_hit_cylinder(e, &cx, &cz, &r, &top, &bottom);
		float fx = *b->x + sinf(*b->yaw) * 250.0f, fz = *b->z + cosf(*b->yaw) * 250.0f;
		if (hypotf(cx - fx, cz - fz) < r + ATK_STOMP.max_range)
			return e;
	}
	return NULL;
}

static bool s_kick_voice;               // the kick's or stomp's effort still to come (0x549e20)

static void start_stomp(bool stomp)
{
	s_kick_voice = true;
	s_mode = M_STOMP;
	s_stomp_e = s_stompable;
	s_attack_seq = false;            // 0x5569b0: a kick or stomp sets 0x1fbad16 back to 0xff
	s_stomp_atk = stomp ? &ATK_STOMPS[kick_type()] : &ATK_KICKS[kick_type()];
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

// The upper hold and attack movers (0x5584e0, 0x558bd0): with a target locked (0x1fbad07) James is
// turned to it; without one the legs' hold stance turns him with the stick (move.c MV_HOLD) and an
// attack turns him not at all.
static void aim_turn(CombatBody* b, float dt)
{
	if (s_target)
		face(b, s_target, dt);
}

// The PC's grip flags (0x6cf7cc, set by 0x52dac0 as the weapon comes up): 0 none 0xff, 1 handgun
// 0x55, 2 shotgun 5, 3 rifle 0, 4 spray 0xff, 5 plank 0xff, 6 pipe 0x55, 7 chainsaw 5, 8 great
// knife 0, 9 Maria's revolver 0x55, 10 her knife 0xff
static u8 grip_flags(void)
{
	static const u8 F[11] = { 0xff, 0x55, 0x05, 0x00, 0xff, 0xff, 0x55, 0x05, 0x00, 0x55, 0xff };
	int g = wdef()->grip;
	u8 f = g >= 0 && g < 11 ? F[g] : 0;
	if ((g == 4 || g == 5 || g == 10) && s_place != 0)
		f &= 0x55;             // 0x52dac0: no running under these indoors
	return f;
}

void combat_set_place(int place) { s_place = place; }

int combat_candidates(Enemy** out, int max)
{
	int n = s_ncand < max ? s_ncand : max;
	memcpy(out, s_cand, sizeof(Enemy*) * n);
	return n;
}

const Enemy* combat_target(void) { return s_ok && s_target && enemy_alive(s_target) ? s_target : NULL; }
bool combat_attack_seq(void) { return s_attack_seq; }

bool combat_legs(CombatLegs* o)
{
	memset(o, 0, sizeof *o);
	if (!s_ok)
		return o->free = true;
	bool swing = s_mode == M_SWING || s_mode == M_REBOUND || s_mode == M_RECOVER;
	bool attacking = s_mode == M_SHOT || s_mode == M_RELOAD || s_mode == M_SPRAY || (swing && s_swing_legs);
	// the rifle's shot and reload hold the legs in the lower attack status (0x5569b0 grip 3); a
	// swing does unless it was a light one begun on the move (s_swing_legs)
	o->free = s_mode == M_FREE || s_mode == M_RAISE || s_mode == M_READY || s_mode == M_LOWER ||
	          (attacking && s_weapon != W_RIFLE);
	o->aim = o->free && s_mode != M_FREE && s_mode != M_LOWER;
	o->locked = o->aim && s_target != NULL;
	o->attack_seq = s_attack_seq;
	o->attacking = attacking;
	u8 f = grip_flags();
	o->walk = (f & (attacking ? 0x40 : 4)) != 0;
	o->run = !s_attack_seq || (f & (attacking ? 0x80 : 8)) != 0;
	return o->free;
}

// 0x52e110 on beginner and easy (gActionDifficulty < 2): Action held readies the weapon by itself
// (0x1fbab12 = 4) while 0x542620 (from 0x533c10) finds an enemy ahead: the first candidate in clear
// sight within 2500 of the point 2500 in front of James. Not on a press an event ahead takes
// (EventCheckLookLine 0x48e520), not with Aim held, nor with one on the floor to kick (0x1fbad00).
// The PC measures from James' 0xa0 height to the enemy's feet; the port takes both feet.
static const Enemy* enemy_ahead(const CombatBody* b)
{
	float cx = *b->x + sinf(*b->yaw) * 2500.0f, cz = *b->z + cosf(*b->yaw) * 2500.0f;
	for (int i = 0; i < s_ncand; i++) {
		const Enemy* e = s_cand[i];
		if (!enemy_alive(e) || !clear_line(b, e))
			continue;
		float dx = cx - e->x, dy = *b->y - e->y, dz = cz - e->z;
		if (sqrtf(dx * dx + dy * dy + dz * dz) <= 2500.0f)
			return e;
	}
	return NULL;
}

bool combat_auto_ready(const CombatBody* b, u32 held, u32 down, bool event_ahead)
{
	if (!s_ok || enemy_difficulty() >= 2 || !(held & KEY_A) || (held & KEY_R) || s_stompable)
		return false;
	if ((down & KEY_A) && event_ahead)
		return false;
	return enemy_ahead(b) != NULL;
}

// The EE's SwapLightHeavyAttack (HandleAttackInput, on in the hold and attack statuses 0x1a and
// 0x1c with a melee weapon) keeps a released tap 0.3 s (kLightAttackBufferTimerMax) and asks for the
// heavy swing once A has been held 0.2 s; the game takes either the next time it asks for an attack
// (its hook in 0x533ff0). In the hold status the PC asks only once the raise or the follow-through
// has ended, so a tap in their last 0.3 s still swings. Without the option a press there is lost
// and only A held on attacks again (M_READY's action_again).
static void ee_queue_update(u32 held, bool action_down, float dt)
{
	if (!s_swap_light_heavy)
		return;
	if (s_queue == 1 && (s_queue_t -= dt) <= 0.0f)
		s_queue = 0;
	if (action_down)
		s_qhold = 0.0f;
	else if (s_qhold >= 0.0f && (held & KEY_A))
		s_qhold += dt;
	if (s_qhold >= HOLD_SECONDS && (held & KEY_A)) {
		s_queue = 2;
		s_qhold = -1.0f;
	} else if (s_qhold >= 0.0f && !(held & KEY_A)) {
		s_queue = 1;
		s_queue_t = 0.3f;
		s_qhold = -1.0f;
	}
}

// The hold status asks for an attack again (0x5569b0): the kept tap or hold swings, a downed enemy
// at his feet taking the kick or stomp first (HoldToStomp: a tap kicks, a hold stomps); A still
// down undecided goes on being timed in M_READY.
static void ee_queue_fire(const CombatBody* b, u32 held)
{
	if (s_queue && (held & KEY_R)) {
		if (s_stompable && s_weapon != W_KNIFE)
			start_stomp(s_queue == 2 || !s_hold_to_stomp);
		else
			start_swing(b, false, s_queue == 2);
	} else if (s_qhold >= 0.0f && (held & KEY_A)) {
		bool stomp = s_stompable && s_weapon != W_KNIFE;
		if (stomp && !s_hold_to_stomp)
			start_stomp(true);
		else
			s_charge = s_qhold, s_charge_stomp = stomp;
	}
	s_queue = 0;
	s_qhold = -1.0f;
}

// 0x5569b0 grips 1 to 3 and 9: a shot, or the reload when the gun is empty and rounds are left;
// the handgun's and the revolver's lock again if the lock was lost. A press with neither clicks
// (0x533ff0, on the press only).
static void gun_attack(const CombatBody* b, bool press)
{
	const WeaponDef* w = wdef();
	if (s_weapon == W_HANDGUN || s_weapon == W_COLT)
		lock_on(b);
	if (loaded() > 0 || spare() > 0)
		s_attack_seq = true;
	if (loaded() > 0) {
		s_mode = M_SHOT;
		s_fired = false;
		s_shot_queued = false;
		play(&w->shot);
	} else if (spare() > 0) {
		s_mode = M_RELOAD;
		s_reloaded = false;
		play(&w->reload);
	} else if (press) {
		sfx_play(w->snd_empty, 1.0f, 0.0f);
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
	// EvProgFirstMonsterStart (0x59da20): James picks up the plank and holds it. Any first owning
	// of it with nothing in hand equips it (a save from before the weapon was kept loads this way)
	bool plank = !s_maria && owns(W_PLANK);
	if (plank && !s_had_plank && s_weapon == W_NONE && s_mode == M_FREE)
		load_weapon(W_PLANK);
	s_had_plank = plank;
	if (s_invuln > 0.0f)
		s_invuln -= dt;
	if (s_flash > 0.0f)
		s_flash -= dt;
	step(dt);
	s_body_pos[0] = *b->x, s_body_pos[1] = *b->y, s_body_pos[2] = *b->z;
	s_run_held = (held & KEY_B) != 0;
	// 0x52e110 keeps ten frames of the keys: Action held through all of them gives 0x533ff0 a
	// pressure again (0x1fbab0f), so holding it attacks again, light, as each attack ends. On
	// beginner and easy letting go of it drops a queued attack (0x1fbad13, 0x1fbad14).
	bool action_down = (down & KEY_A) != 0 || s_ready_press;
	s_ready_press = false;
	if (!(held & KEY_A))
		s_action_held = -1.0f;
	else if (action_down || s_action_held < 0.0f)
		s_action_held = 0.0f;
	else
		s_action_held += dt;
	bool action_again = !action_down && s_action_held >= 8.5f / 30.0f;
	if (enemy_difficulty() < 2 && !(held & KEY_A))
		s_combo_req = false;
	if (s_weapon == W_CSAW && (s_mode == M_FREE || s_mode == M_LOWER) && sfx_playing(11044)) {
		sfx_stop(11044);                // 0x549e20: the engine dies down once the saw is lowered
		sfx_stop(11045);
	}
	const WeaponDef* w = wdef();
	switch (s_mode) {
	case M_FREE:
		if (down & KEY_L) {
			cycle_weapon();
			return false;
		}
		// 0x557100: the weapon comes up only from the states whose upper row allows status 0x1a;
		// walking, running and the quick turn go on until they end
		if ((held & KEY_R) && move_can_aim()) {
			lock_on(b);                      // 0x533ca0: locked as he readies
			s_raise_press = action_down;
			if (is_bare()) {
				s_mode = M_READY;          // bare hands: nothing to raise
				s_ready_press = action_down;
			} else if (is_melee()) {
				s_mode = M_RAISE;
				play(&md()->raise[s_side]);
				if (s_weapon == W_CSAW)
					sfx_play_at(11044, 0.7f, s_body_pos);   // 0x52fae0: the engine starts as he raises it
			} else {
				s_mode = M_RAISE;
				play(&w->raise);
			}
			return true;
		}
		free_moves(b, cp, held, dt);
		return false;
	case M_RAISE:
		// the raise is the hold status' clip too (0x52fae0 case 0x1a): the attack waits for its end
		if (is_melee())
			ee_queue_update(held, action_down, dt);
		s_raise_press |= action_down;
		if (s_end) {
			s_mode = M_READY;
			play(is_melee() ? &md()->ready[s_side] : &w->aim);
			if (is_melee())
				ee_queue_fire(b, held);
			s_ready_press = s_raise_press && s_mode == M_READY;   // unless the EE's buffer took it
			s_raise_press = false;
		}
		break;
	case M_READY:
		if (!(held & KEY_R)) {
			s_charge = -1.0f;
			s_target = NULL;                 // 0x557100: upper status 0x1b drops the lock
			if (is_bare()) { s_mode = M_FREE; s_attack_seq = false; play(&J_STAND); return false; }
			s_mode = M_LOWER;
			play(is_melee() ? &md()->lower[s_side] : &w->lower);
			break;
		}
		change_target(b, down);           // 0x54f100
		aim_turn(b, dt);
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
		if (action_down || action_again) {
			// 0x5569b0 takes the kick / stomp branch before it looks at the grip: 0x533c10 sets
			// DAT_01fbad00 from 0x542a10 (a downed enemy at his feet) whatever James holds, so every
			// one of his weapons stomps, the plank too. Maria's knife keeps to her own attacks.
			// Held on, the low pressure of 0x1fbab0f makes the melee swing the light one (0x533ff0);
			// the stomp reads the key's own pressure and stays the stomp (0x534210).
			bool stomps = s_weapon <= W_SPRAY || is_bare() || is_gun();
			if (stomps && s_stompable) {
				if (s_hold_to_stomp && action_down)
					s_charge = 0.0f, s_charge_stomp = true;
				else
					start_stomp(true);
			} else if (is_melee()) {
				if (s_swap_light_heavy && action_down)
					s_charge = 0.0f, s_charge_stomp = false;
				else
					start_swing(b, false, action_down);
			} else if (s_weapon == W_SPRAY) {
				// 0x5569b0 grip 4: attack 8, or the empty can's pfft (10)
				s_mode = M_SPRAY;
				s_attack_seq = true;
				s_spray_again = false;
				memset(s_hit, 0, sizeof s_hit);
				play(s_spray > 0.0f ? &S_SPRAY : &S_EMPTY);
				if (s_spray > 0.0f)
					sfx_play_at(11047, s_spray < 10.0f ? s_spray * 0.05f : 0.5f, s_body_pos);
			} else if (is_gun()) {
				gun_attack(b, action_down);
			}
			break;
		}
		// the legs walk, back off, strafe and run under the weapon as its flags allow (move.c with
		// combat_legs); the upper body keeps the ready pose
		if (is_melee() && s_cur != &md()->ready[s_side])
			play(&md()->ready[s_side]);
		else if (is_bare() && s_cur != &J_STAND)
			play(&J_STAND);
		break;
	case M_SHOT:
		if (!s_fired && s_frame - w->shot.start >= w->atk.first) {
			s_fired = true;
			fire(b);
		}
		// 0x558bd0 asks 0x533ff0 for the next attack while 0x544ac0 allows it: for the handgun's
		// and the revolver's shots (attacks 1, 2, 25, 26) from 9 frames past the attack's first
		// frame to 9 past its last (frames 9..19 of attack 2, so from frame 9 to the shot's end).
		// 0x558bd0 then starts it as the shot ends, straight into the next shot (207, 0x52fae0) or
		// the reload. The shotgun's and the rifle's shots take none.
		if ((action_down || action_again) && (w->atk.id == 2 || w->atk.id == 26)) {
			float f = s_frame - w->shot.start;
			if (f >= w->atk.first + 9.0f && f <= w->atk.last + 9.0f + 0.99f)
				s_shot_queued = true;
		}
		aim_turn(b, dt);
		if (s_end) {
			s_mode = M_READY;
			play(&w->aim);
			if (s_shot_queued && (held & KEY_R))
				gun_attack(b, false);
			s_shot_queued = false;
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
		aim_turn(b, dt);
		if (s_end) {
			s_mode = (held & KEY_R) ? M_READY : M_LOWER;
			if (s_mode == M_LOWER)
				s_target = NULL;
			play(s_mode == M_READY ? &w->aim : &w->lower);
		}
		break;
	case M_STOMP: {
		const AttackInfo* a = s_stomp_atk;
		float f = s_frame - s_cur->start;
		face(b, s_stomp_e, dt);           // 0x558f40: the kick turns him to the one on the floor
		// 0x549e20 (upper state 0x1d, no motion change pending in 0x1fba904): James' effort 0x2b0d
		// (Maria's 0x4288, 0x549500) at 0.9 at his feet, frame 8 of the kick (request 6) or 4 of the
		// stomp (7), whatever the blow meets. The PC has no other stomp sound: no thud on a hit,
		// nothing by floor or enemy.
		if (s_kick_voice && f >= (a->id >= 36 ? 4.0f : 8.0f)) {
			s_kick_voice = false;
			sfx_play_at(s_maria ? 17032 : 11021, 0.9f, s_body_pos);
		}
		if (f >= a->first && f <= a->last + 0.99f) {
			Enemy* e = stomp_target(b);
			if (e) {
				int i = 0;
				for (; i < enemy_count() && enemy_get(i) != e; i++)
					;
				if (i < ENEMY_MAX && !s_hit[i]) {
					s_hit[i] = true;
					if (enemy_corpse_stompable(e))
						enemy_stomp_corpse(e, sinf(*b->yaw), cosf(*b->yaw));
					else
						enemy_damage(e, a->id, a->ap, a->sp, sinf(*b->yaw), cosf(*b->yaw));
				}
			}
		}
		if (s_end) {
			s_mode = (held & KEY_R) ? M_READY : M_FREE;
			if (s_mode == M_READY && !is_bare()) {
				s_mode = M_RAISE;
				play(is_melee() ? &md()->raise[s_side] : &w->raise);
			} else {
				play(&J_STAND);
			}
			if (s_mode == M_FREE)
				return false;
		}
		break;
	}
	case M_SWING: {
		const MeleeDef* m = md();
		if (action_down || action_again)
			s_combo_req = true;       // 0x558bd0 asks 0x533ff0 again through the swing
		swing_sound();
		swing_hits(b);
		// 0x5350d0: a plank or pipe blow that meets an enemy (+0x124 1) plays back on normal and hard
		// only, and not when the one it met is down (+0x148 & 4); on beginner and easy the swing goes
		// on through. 0x52fae0 then sets the attack's reversed entry in mode 6, which keeps the
		// place in the clip: the plank's 366 / 372 and the pipe's 412 / 418 for a light or combo
		// swing (the full swing's place less 4 or 6 frames), the heavy swing's and the lunge's own
		// (376 / 380, 422 / 426, 430 / 432). Maria's knife, the great knife and the chainsaw go on
		// through an enemy (grips 10, 8 and 7 play back only off a wall, which the port does not test).
		const JAnim* rb = NULL;
		if (s_hit_any && m->rebounds && enemy_difficulty() >= 2 && !s_hit_down) {
			rb = s_cur == &m->heavy[s_side] ? &m->rb_heavy[s_side]
			   : s_cur == &m->extra[s_side] ? &m->rb_extra[s_side] : &m->rebound[s_side];
			if (!rb->end)
				rb = NULL;
		}
		if (rb) {
			float f = rb->start + (s_frame - s_cur->start) - (s_cur == &m->light[s_side] ? m->rb_skip : 0);
			s_rb_light = rb == &m->rebound[s_side];
			s_mode = M_REBOUND;
			play_from(rb, f < rb->start ? rb->start : f > rb->end ? rb->end : f);
			break;
		}
		if (s_end) {
			s_atk = NULL;
			if (m->kind == MK_NATA) {
				// the great knife: the next swing comes from the other shoulder
				s_side ^= 1;
				s_mode = M_READY;
				play(&m->ready[s_side]);
			} else if (m->kind == MK_CSAW) {
				if (s_combo_req && (held & KEY_R) && s_cur == &m->light[s_side]) {
					start_swing(b, true, false);   // pressed again while sawing: the thrust
				} else {
					s_mode = M_READY;
					play(&m->ready[s_side]);
				}
			} else if (s_atk_heavy || s_cur == &m->extra[s_side] || !m->recover[s_side].end) {
				s_mode = M_READY;
				play(&m->ready[s_side]);
			} else if (s_combo_req && (held & KEY_R)) {
				// 0x5569b0: with the button pressed again during a swing (DAT_01fbad19) the next
				// attack is the short swing (attack 13 for the plank, 16 for the pipe) from the other
				// shoulder, without the follow-through in between
				s_side ^= 1;
				start_swing(b, true, false);
			} else {
				s_mode = M_RECOVER;
				play(&m->recover[s_side]);
			}
		}
		break;
	}
	case M_REBOUND:
		// 0x544ac0: while a light or combo swing plays back every press counts (its speed below 0 with
		// 0x1fbac7b set) and the combo follows from the same shoulder; the heavy swings' and the
		// lunge's attacks (14, 17, 18) take none
		if ((action_down || action_again) && s_rb_light)
			s_combo_req = true;
		if (s_end) {
			s_atk = NULL;
			if (s_combo_req && (held & KEY_R)) {
				start_swing(b, true, false);
			} else {
				s_mode = M_READY;
				play(&md()->ready[s_side]);
			}
		}
		break;
	case M_RECOVER:
		// The follow-through (367 / 373, the pipe's 413 / 419) is the hold status' clip: 0x5569b0 reads
		// the button there only once the clip has ended (0x1fba90c & 2) or loops, and 0x1fbad19 goes
		// back to 0, so the next swing is the light one (attack 12, 15) from the new shoulder; a tap
		// meanwhile is kept the EE's way (ee_queue_update)
		ee_queue_update(held, action_down, dt);
		if (s_end) {
			s_side ^= 1;
			s_mode = M_READY;
			play(&md()->ready[s_side]);
			ee_queue_fire(b, held);
		}
		break;
	case M_SPRAY: {
		// 0x545260: the can empties in real time while the spray is out; normal and hard cost James
		// health as well
		const AttackInfo* a = s_cur == &S_AGAIN ? &ATK_SPRAY[1] : &ATK_SPRAY[0];
		if (held & KEY_A)
			s_spray_again = true;
		else
			s_spray_again = false;
		if (s_cur != &S_EMPTY) {
			float f = s_frame - s_cur->start;
			if (f >= a->first && f <= a->last + 0.99f && s_spray > 0.0f) {
				spray_hits(b, a, dt);
				s_spray -= dt;
				if (enemy_difficulty() > 1 && s_hp > 1.0f)
					s_hp -= dt;
			}
		}
		aim_turn(b, dt);
		if (s_spray <= 0.0f && s_cur != &S_EMPTY) {
			// the can is spent: the PC takes the item away (0x49e620)
			sfx_stop(11047);
			s_spray = SPRAY_FULL;
			ev_give_item(12, -ev_item_count(12));
			load_weapon(bare());
			return false;
		}
		if (s_end) {
			if (s_spray_again && s_cur != &S_EMPTY && (held & KEY_R)) {
				memset(s_hit, 0, sizeof s_hit);
				play(&S_AGAIN);
			} else {
				sfx_stop(11047);
				s_mode = (held & KEY_R) ? M_READY : M_LOWER;
				if (s_mode == M_LOWER)
					s_target = NULL;
				play(s_mode == M_READY ? &w->aim : &w->lower);
			}
		}
		break;
	}
	case M_LOWER:
		if (held & KEY_R) {
			lock_on(b);
			s_mode = M_RAISE;
			play(is_melee() ? &md()->raise[s_side] : &w->raise);
			break;
		}
		// 0x5590e0: the weapon down, the attack side byte goes back to 0xff (0x534360)
		if (s_end) { s_mode = M_FREE; s_attack_seq = false; play(&J_STAND); return false; }
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
	if (s_mode == M_DYING || s_mode == M_DEAD)
		s_die_t += dt;
	s_hit_t += dt;
	if (s_mode == M_READY)
		s_combo_req = false;
	else
		s_charge = -1.0f;
	if (s_mode != M_RAISE && s_mode != M_RECOVER)
		s_queue = 0, s_qhold = -1.0f;    // a kept tap lives until the hold status asks (ee_queue_fire)
	if (s_mode == M_SWING || s_mode == M_REBOUND || s_mode == M_RECOVER)
		aim_turn(b, dt);                 // 0x558bd0: the upper attack keeps him on the lock
	// the run key stays the legs' while they walk under the weapon (combat_legs)
	CombatLegs lg;
	*held_out = held & ~(combat_legs(&lg) ? KEY_A : (KEY_A | KEY_B));
	return true;
}

int combat_grip(void) { return s_ok ? WEAPONS[s_weapon].grip : 0; }

bool combat_james_free(void) { return !s_ok || s_mode == M_FREE; }

int combat_james_clip(float* blend)
{
	*blend = s_cur ? clip_blend(s_cur) : 0.0f;
	return (int)(uintptr_t)s_cur;
}

bool combat_james_anim(const Anim** anim, float* frame, int loop[2])
{
	if (!s_ok || !s_anim_ok)
		return false;
	*anim = &s_anim;
	*frame = s_frame;
	cur_loop(loop);
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

void combat_weapon_shadow(const float pos[3], float yaw, const float* light)
{
	if (!s_ok || !s_wmodel_ok || demo_active())
		return;
	shadow_cast_james(&s_wmodel, pos, yaw, light);
}

// PlayerCheckModelParts (0x52f6a0) picks one right hand (poses 3..13), one left hand (14..20) and
// the flashlight on James' chest (21) for 0x50dfc0. Its left hand follows the upper-body state
// (0x1fba8e4, the PS2 beta's _JAMES_UPPER_STATUS) in four groups, read here off the port's states:
//   1  READY, READYOFF, the runs (RUN1..3, LSRUN, RSRUN), and RELAX with the great knife
//   2  RELAX, WALL_F, FALL, DAMAGE, TO_STAND, EVENT unless the motion is 101 (standing)
//   3  HOLD, RELEASE, ATTACK
//   0  the rest (standing, walking, turning, KICK)
// Port approximations: walking into a wall (WALL_F) counts as walking, being held by an enemy as
// DAMAGE, a reload as ATTACK, and the plank's two-handed frames 8..21 count from the start of the
// port's heavy swing clip. Born From a Wish Maria is 0x549050: left hand 6, right by grip, pose 7
// only with bare hands.
void combat_set_parts(int move_state)
{
	if (!s_ok || !s_james)
		return;
	int grip = WEAPONS[s_weapon].grip;
	if (s_maria) {
		chr_set_parts(s_james, 6, grip == 9 ? 4 : grip == 10 ? 5 : 3, grip ? 0 : 7);
		return;
	}
	// gFlashLightAcquired & 0x40000; the PC also hides it with James' status bit 0x8000, which the
	// port doesn't keep
	int light = ev_have_item(18) ? 21 : 0;
	int g = 0;
	switch (s_mode) {
	case M_FREE:
		if (demo_active())
			g = 2;
		else if (move_state == MV_RELAX)
			g = grip != 8 ? 2 : 1;
		else if (move_state >= MV_RUN1 && move_state <= MV_RSRUN)
			g = 1;
		break;
	case M_RAISE: case M_LOWER: g = 1; break;
	case M_READY: case M_SWING: case M_REBOUND: case M_RECOVER: case M_SHOT: case M_RELOAD: case M_SPRAY:
		g = 3;
		break;
	case M_HURT: case M_HELD: case M_DYING: case M_DEAD: g = 2; break;
	default: break;
	}
	int r = 3, l = 14;
	if (boat_active()) {                               // the boat (kind 0x10b) and status 0x20000
		r = 5, l = 16;
	} else {
		switch (grip) {
		case 0: case 1: case 4:
			l = g == 1 ? 15 : 14;
			r = grip == 1 ? 7 : grip == 4 ? 11 : g == 1 ? 4 : 3;
			break;
		case 2: case 3: l = g == 2 ? 14 : 17, r = 8; break;
		case 5:
			l = g <= 1 ? 19 : 14;
			if (g == 3 && s_atk_heavy && s_cur) {         // request 4 (0x1fbad12): both hands on it
				float f = s_frame - s_cur->start;
				l = f >= 8.0f && f <= 21.0f ? 19 : 14;
			}
			r = 10;
			break;
		case 6: l = g <= 1 ? 19 : g == 3 ? 18 : 14, r = 9; break;
		case 7: l = 14, r = 13; break;
		case 8: l = g == 2 ? 14 : 20, r = 12; break;
		}
	}
	// the weapon is in hand but not shown (0x53ac10 and the weapon's 0x10): both hands open
	if (grip && (!s_wmodel_ok || demo_active()))
		r = 3, l = 14;
	chr_set_parts(s_james, l, r, light);
}

const char* combat_status(void)
{
	static const char* names[] = { "free", "raise", "ready", "swing", "rebound", "recover", "lower",
	                               "shot", "reload", "stomp", "spray", "hurt", "held", "dying", "dead" };
	// the candidates in sight, L when one is locked (the debug page shows the first 16 characters)
	snprintf(s_line, sizeof s_line, "HP%3.0f %s %c%d g%d%s", s_hp, names[s_mode], s_target ? 'L' : 'c', s_ncand,
	         WEAPONS[s_weapon].grip, s_attack_seq ? " seq" : "");
	return s_line;
}

int combat_weapon_item(void) { return is_bare() ? 0 : wdef()->item; }

void combat_restore_weapon(int item)
{
	s_had_plank = !s_maria && owns(W_PLANK);
	if (item <= 0 || !s_ok || s_mode != M_FREE)
		return;
	for (int w = bare() + 1; w <= pdef()->last; w++)
		if (WEAPONS[w].item == item && owns(w)) {
			if (w != s_weapon)
				load_weapon(w);
			return;
		}
}

void combat_set_infinite_ammo(bool on) { s_inf_ammo = on; }

bool combat_weapon_rounds(int* in_gun, int* spare_rounds)
{
	*in_gun = is_gun() ? loaded() : 0;
	*spare_rounds = is_gun() ? spare() : 0;
	return is_gun();
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
