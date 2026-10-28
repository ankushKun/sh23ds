#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "collide.h"

// Enemies placed by the stage Enemy_List (pack events/enemies.txt, tools/sh2conv/enemies_pack.py).
// The Lying Figure (kind 0x200, chr/scu) is ported from the PC game's en_scu code; see
// port/ENEMIES.md for the state machine and the addresses it comes from.

// Live enemies at once. The PC has 32 EnLOCAL slots; the stage 30 creeper room alone lists 14 (five
// creepers and nine nests), and the nests release up to twelve creepers (0x4add40).
#define ENEMY_MAX 24

// What the enemies need to know about James each frame.
typedef struct {
	float x, y, z, yaw;      // feet position (Y down), facing (forward = sin yaw, cos yaw)
	bool light;              // flashlight on
	bool alive;
	bool stomping;           // a downward weapon swing is under way (enemies stay down for it)
	bool maria;              // this is Maria (an enemy's target when she is closer than James)
} EnPlayer;

typedef enum {
	EN_KIND_NONE, EN_KIND_SCU, EN_KIND_MKN, EN_KIND_NSE, EN_KIND_TYU, EN_KIND_RED, EN_KIND_PAP, EN_KIND_ONI,
	EN_KIND_BOS, EN_KIND_ARM, EN_KIND_EDI, EN_KIND_IKE, EN_KIND_NEST
} EnKind;

// Lying Figure states (enCtrlSCUSubFunc order in the PC table built by 0x4c6f50).
enum {
	SCU_WANDER, SCU_PRECAUTION, SCU_CHASE, SCU_BACKWARD, SCU_AMBUSH, SCU_WAITCAR, SCU_CRAWL,
	SCU_ATTACK, SCU_DAMAGE, SCU_CONFUSE, SCU_DOWN, SCU_DYING
};

// Mannequin states (0x4c3790 table order).
enum {
	MKN_STAY, MKN_EVENT, MKN_FALL, MKN_FALL2, MKN_WANDER, MKN_PRECAUTION, MKN_CHASE, MKN_CIRCLE,
	MKN_ATTACK, MKN_DAMAGE, MKN_CONFUSE, MKN_DOWN, MKN_DYING
};

// Bubble Head Nurse states (0x4bb390 table order).
enum { NSE_WANDER, NSE_PRECAUTION, NSE_CHASE, NSE_ATTACK, NSE_DAMAGE, NSE_CONFUSE, NSE_DOWN, NSE_DYING };

// Pyramid Head states (0x4c0b00 table order).
enum { RED_WANDER, RED_CHASE, RED_HUNT, RED_STAIRS, RED_ATTACK, RED_GRAB, RED_CONFUSE, RED_LEAVE, RED_STAND };

// Abstract Daddy states (0x4b7df0 order; 6 Confuse is not ported).
enum { PAP_WANDER, PAP_STRAIGHT, PAP_CHASE, PAP_RUSH, PAP_ATTACK, PAP_DAMAGE, PAP_CONFUSE, PAP_DOWN, PAP_LAST, PAP_DYING };
// The other Pyramid Head (0x4be550 has 12 states; the port keeps these).
enum { ONI_WANDER, ONI_CHASE, ONI_THRUST, ONI_SWINGS, ONI_FLINCH, ONI_SUICIDE, ONI_BODY, ONI_FLANK, ONI_CORNER, ONI_STANDOFF };
// Mary (0x4b5460 order).
enum { BOS_APPROACH, BOS_DISTANCE, BOS_MOTHS, BOS_GRAB, BOS_SWIPE, BOS_DEAD };
// The Mandarin (0x4acff0 order; 1 Turn, away from another one close by, is folded into Wander).
enum { ARM_WANDER, ARM_TURN, ARM_DAMAGE, ARM_DYING };

// Eddie (the port's own grouping of 0x4b4100's ten states).
enum { EDI_CHASE, EDI_SHOOT, EDI_PUNCH, EDI_FLINCH, EDI_FLEE, EDI_DEATH, EDI_COVER, EDI_PILLAR };

// Flesh Lip (0x4b9ae0 order: 4 and 5 are folded into the others, 8 the lift into the grab).
enum { IKE_WAIT, IKE_DROP_DOWN, IKE_WANDER, IKE_CHASE, IKE_GRAB = 6, IKE_KICK, IKE_LIFT, IKE_DAMAGE, IKE_DEATH };

// Creeper states (0x4b0370 table order).
enum {
	TYU_WANDER, TYU_CHASE, TYU_TARGET, TYU_FLEE, TYU_WALL, TYU_CEILING, TYU_ATTACK, TYU_DAMAGE, TYU_DYING,
	TYU_EVENT
};

typedef struct {
	bool used;
	EnKind kind;
	u8 model;                // loaded model slot (chr folder)
	int list_id;             // Enemy_List id: the "killed" bit number
	int status;              // spawn status from the list (0 standing, 3/4 lying, 5 dead, ...)
	int ctrl;                // 1 automatic, 6 dead for good
	int mlv, slv;            // state and sub-state (EnLOCAL mlv / slv)
	u8 type;                 // EnLOCAL +4 behaviour bits (0x10 crawler, 0x08 wait, 0xff no recovery)
	u8 lie;                  // 0 face down, 1 face up (EnLOCAL +6)
	int last_atk;            // attack id of the last hit (EnLOCAL +8)
	u32 flags;               // EnLOCAL +0x10 bits used here: 0x200 straight crawl, 0x2000 timer tick
	float x, y, z, yaw;      // position (feet) and facing
	float path_angle, mark_angle;   // EnPATH angle / markangle
	float stx, stz, tx, tz;  // wander start point and target
	float hp, hp_max, endurance, endurance_max;
	float damage, shock;     // pending hit (shBattleInfo damage / shock)
	float hb, hbx, hbz;      // hit-back speed and direction
	float size, new_size, tall, new_tall;
	float p_dist;            // distance to James
	float alert;             // 0..3 seconds of being lit or seen (EnLOCAL +0x98)
	float life;              // crawl life in seconds (EnLOCAL +0xd0)
	float vx, vz;            // crawl velocity (units/s)
	int timer;               // 60 Hz ticks
	float sound_wait;        // ticks since the last idle noise (EnLOCAL +0xa0)
	float crawl_snd;         // ticks to the next crawling noise (EnSCU count, +0xcc)
	float speed_mul, speed_goal;   // nurse walk speed factor and its target (EnLOCAL +0xa8 / +0xac)
	s8 amb;                  // Lying Figure ambush spot (index into the glb's ambush_apart list), -1 none
	u8 amb_n;                // stand loops waited there (EnLOCAL +10)
	float detour, detour_t;  // enSetPath: the heading round an obstacle and how long to keep it
	int ival, ival2, ival3, ival4;   // per kind (oni: route point, partner slot, catch-up stage)
	int timer2;              // a second timer (the oni's stun)
	float aux[4], aux2, aux3;   // per kind (Mary: the moths, her speed and climb)
	float clock;             // seconds left of Pyramid Head's stairwell fight (EnLOCAL +0xac)
	u8 cnt, cnt_max;         // death throes (EnLOCAL +0xd4 / +0xd5)
	// animation (EnSCUAnime index, frame in the .anm, playback rate 1.0 = table speed)
	int anim;
	float frame, rate;
	int loops, loop_limit;   // loops played, and a loop count after which the anim ends (0 none)
	bool anim_end;
	bool attacking;          // attack 42 / 43 window armed
	bool attack_hit;         // already hit James during this attack
	bool maria_hit;          // already hit Maria during this attack
	bool dead_flag;
	// the crossfade into a new anim (enAnimeSet 0x4a9920): seconds the enemy has played (the PC's
	// +0xb count, no blend in its first 10 frames), a new anim waiting for enemy_draw to start the
	// blend, whether that one cuts in instead, and whether its blend history is set up
	float anim_age;
	bool anim_new, anim_cut, drawn;
	// 3DS-only: a body James killed takes stomps while he stays by it (enemy_corpse_stompable), and
	// the stomps it took (main.c sprays blood on each)
	bool corpse_open;
	u8 corpse_hits;
	float acid_acc;          // ticks of the Lying Figure's spit not yet spent on a puff (mist_poison)
} Enemy;

void enemy_init(const char* pack);
void enemy_exit(void);

// Spawns the current stage's enemies that stand in the loaded room (by its collision floors).
void enemy_room_enter(int stage, const Collision* col);
// After the streamed tiles of an open area changed (the merged collision is rebuilt in place):
// drops the enemies whose floor went away and spawns the rows that now have one.
void enemy_stream_update(int stage, const Collision* col);
void enemy_clear(void);

// Killed Enemy_List ids (the PC's bitfield at 0x1f7d9e0) for save files; reset on a new game.
#define ENEMY_KILLED_BYTES 64
void enemy_killed_get(u8* out);
void enemy_killed_set(const u8* in);
void enemy_killed_reset(void);

void enemy_update(const EnPlayer* p, float dt);
void enemy_spawn_list(void);              // the stage's list rows not present yet (0x586f40)
void enemy_event_create(int status, float x, float y, float z, float yaw, float delay);   // a program's own figure (0x48fcc0)
void enemy_event_remove(void);                                                // 0x53a350
void enemy_event_prefetch(void);          // start reading its model before it is needed
void enemy_event_tick(float dt);          // that figure, while a scene or the program holds the game

// Draws with the skinning program bound (James' draw state: same light environment).
// dt: the time the poses move on (0 for the second eye of a 3D frame, which draws the same poses)
void enemy_draw(const C3D_Mtx* view, int uModelView, int uBones, float dt);

int enemy_count(void);
Enemy* enemy_get(int i);
bool enemy_alive(const Enemy* e);
bool enemy_is_down(const Enemy* e);      // lying on the floor (a downward swing is the attack)
// Hit cylinder of an enemy: centre x/z, radius, top y (smaller, Y down) and bottom y.
void enemy_hit_cylinder(const Enemy* e, float* x, float* z, float* r, float* top, float* bottom);
// A weapon hit: attack id (sh2_attack_list), damage, shock, direction the blow travels.
void enemy_damage(Enemy* e, int attack_id, float ap, float sp, float dir_x, float dir_z);
// 3DS-only, the port's choice (not the PC's, whose 0x542a10 never offers a dead body: enDyingExec
// sets its +0x148 bit 2): a Lying Figure, mannequin or nurse that James killed can be stomped again
// and again until he walks away from it (CORPSE_LEAVE). A stomp on it only thuds and bleeds.
bool enemy_corpse_stompable(const Enemy* e);
void enemy_stomp_corpse(Enemy* e, float dir_x, float dir_z);

void enemy_set_difficulty(int level);    // gActionDifficulty 0..4 (2 = normal)
int enemy_difficulty(void);
// Distance from (x, z) to the nearest living enemy in the room (a large value when there is none).
float enemy_nearest(float x, float z);
const char* enemy_status(void);          // debug HUD line
// Whether the room is dark (the stage record's flashlight flag, main.c s_env.dark): the enemies'
// sight and pace depend on it (light_index). Set before each enemy_update.
void enemy_set_dark(bool dark);

// The model files a stage's enemies need and that are not loaded yet, one want(path) call each
// (main.c's door prefetch hands them to the reader thread).
void enemy_prefetch_stage(int stage, void (*want)(const char* path));
