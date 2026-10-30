#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "chr.h"
#include "collide.h"
#include "enemy.h"

// James' health, the wooden plank and melee attacks (port of the PC player attack code around
// 0x52fae0 / 0x531a00 and the sh2_attack_list table, see port/ENEMIES.md).
//
// Controls (game mode): hold R to ready the weapon (James turns to the nearest enemy), A swings or
// fires; a downed enemy at James' feet is kicked or stomped (bare hands, guns) or gets the plank's
// downward swing; an empty gun reloads on A; L switches to the next weapon James owns. With the
// Enhanced Edition timing (combat_set_options) a quick tap of A kicks or swings light and holding it
// for 0.2 s stomps or swings heavy.
// The plank starts equipped; combat_set_equipped is for the inventory once weapons are items.

// The player's full health: James 100 (200 on beginner and easy), Maria 80 (160) when she is the
// player in Born From a Wish. Kept as a macro so the existing callers follow the player.
float combat_hp_max(void);
#define JAMES_HP_MAX combat_hp_max()
// Ground speed in units per second for gait 0 (backwards), 1 (walk) or 2 (run): Maria is slower.
float combat_ground_speed(int gait);
// True while Maria is the player: combat.c plays her footsteps from her own walk and run, so main.c
// leaves out James' clip-timed ones.
bool combat_own_footsteps(void);

typedef struct {
	float* x;
	float* y;
	float* z;
	float* yaw;
} CombatBody;

// James' model must stay loaded while combat is used (the plank is drawn on his right hand bone).
// In Born From a Wish (prog_chapter() 1) combat_update swaps it for Maria's (chr/xmar/lxx_mar) in
// place, with her revolver and knife, and back to James' outside it.
bool combat_init(const char* pack, Character* james);
void combat_exit(void);
void combat_reset(void);                 // new game / continue: full health, plank equipped
// Enhanced Edition "EnableHoldToStomp" and "SwapLightHeavyAttack" (both on by default). Off gives the
// PC's own behaviour with a digital button: always the stomp and the heavy plank swing.
void combat_set_options(bool hold_to_stomp, bool swap_light_heavy);

// Per frame before player_update. Returns true while James is busy (readied, swinging, hurt or
// dead): the caller then skips the A button check, and moves the legs only when combat_legs says
// so. held_out gets the buttons that are still free for the normal controls.
bool combat_update(CombatBody* b, const circlePosition* cp, u32 down, u32 held, float dt,
                   const Collision* col, u32* held_out);

// Beginner and easy (0x52e110): Action held readies the weapon at an enemy ahead; the caller adds
// the Aim key when this is true. event_ahead: the Action press has an event in front of James.
bool combat_auto_ready(const CombatBody* b, u32 held, u32 down, bool event_ahead);

// Once per game frame, before combat_update and the camera (0x533c10): the candidates in James'
// sight, nearest first, and whether the lock still holds.
void combat_scan(const CombatBody* b);
// 0x1fba8d8 (main.c from 0x556490): 0 out of doors, 1 indoors, 2 a room whose 0x8bcce0 byte is 0.
void combat_set_place(int place);
// The candidates of the last scan (the camera's character slots, 0x1f991a0), the locked target
// (0x1fbacc4) and whether an attack was made since the weapon came up (0x1fbad16 != 0xff).
int combat_candidates(Enemy** out, int max);
const Enemy* combat_target(void);
bool combat_attack_seq(void);

// What the legs may do this frame. free: move.c runs them (combat's clip, when it has one, plays
// on the upper body). aim: the weapon is up (upper status 0x1a, or 0x1c while attacking), with
// the hold stance and the weapon's flags (0x6cf7cc) for walking and running; locked: combat
// turns James to the target; attacking: an attack the legs go on through (a shot, a reload, the
// spray, a light swing begun on the move), during which they keep their state. Returns free.
typedef struct { bool free, aim, locked, attack_seq, attacking, walk, run; } CombatLegs;
bool combat_legs(CombatLegs* out);

// When armed, James is animated from the plank animation set: returns the pose to use, and the
// clip's range in loop[] when it loops (else 0, 0), for chr_pose_loop.
bool combat_james_anim(const Anim** anim, float* frame, int loop[2]);
// gWeaponHandGrip of the weapon in hand: 0 none, 1 handgun, 2 shotgun, 3 rifle, 4 hyper spray, 5 plank,
// 6 steel pipe, 7 chainsaw, 8 great knife (9, 10 Maria's).
int combat_grip(void);
// True while combat leaves James' legs to move.c (nothing readied): his clips then come from
// move_last(), played on the anim combat_james_anim gives.
bool combat_james_free(void);
// The clip combat plays (an id that changes with it) and how long to crossfade into it.
int combat_james_clip(float* blend);
// After chr_draw(james): the plank on his right hand (same model view and skinning program).
void combat_draw_weapon(int uBones);
// After combat_draw_weapon: the weapon's shadow from James' light (0x5a5c70 copies his light block
// for it; shadow_cast_james), the plank's wp_kakuzai.kg1 and the rest that have one.
void combat_weapon_shadow(const float pos[3], float yaw, const float* light);
// Before chr_draw(james): which of the player model's hands and the flashlight on his chest show,
// from the weapon and what James is doing (move_state is move.c's MV_* when he is free).
void combat_set_parts(int move_state);

// Per frame from main.c: the room is dark, the flashlight is on (how far James sees enemies to lock
// on to them, 0x52f2c0).
void combat_set_sight(bool dark, bool flashlight);

// What James' head turns to (0x535720): the locked target, else the nearest enemy or point (people,
// items lying about) in his sight that the room does not hide. False: nothing, or he is busy
// (kicking, hurt, held, dying).
bool combat_look_target(const CombatBody* b, const float (*pts)[3], int npts, float out[3]);

// Testing (debug.txt "god 1"): enemy blows do not reach James.
void combat_set_god(bool on);

// Called by enemies when an attack reaches James.
void combat_damage_james(int attack_id, float ap, float from_x, float from_z);
// Grabs: James is held until he struggles free (the meter is in seconds of frame time, see
// ENEMIES.md); the hold attack drains `ap` per PC frame (1/30 s) meanwhile.
void combat_grab_james(float meter);
bool combat_held(void);
bool combat_has_ammo(void);              // any owned gun with rounds in it or spare (0x4a6340)
void combat_hold_damage(float ap, float dt);
// A Flesh Lip lifting James' body (its hold killed him): he hangs at (x, y, z) and the game over
// waits until the lift ends (on false).
void combat_carry_james(bool on, float x, float y, float z);
bool combat_dying(void);                       // the death animation, or dead
// James' push column for enemy_push_apart, once a frame: its radius (0: none) and how far ahead of
// his feet it stands, eased toward what his state and weapon ask for (the PC's 0x52ed60 / 0x534d70).
void combat_column(float dt, float* radius, float* ahead);
// Seconds since the killing blow while dying or dead, else 0 (main.c draws the screen bleeding to red).
float combat_dying_time(void);
// Seconds since the last blow that took health (main.c flashes the screen red), large when none.
float combat_hit_time(void);

float combat_hp(void);
float combat_low_health_alpha(float dt);   // per frame: the PC red cross's alpha, 0 when hidden
void combat_set_hp(float hp);         // continue from a save
void combat_test_god(bool on);        // playtest: blows take no health
void combat_heal(float amount);       // healing item: added to the recovery pool
void combat_set_equipped(bool on);
// Inventory: "use" on a weapon item equips it (true if it was a weapon); owned guns stay listed
// even when empty (their count is the rounds loaded).
bool combat_equip_item(int item);
bool combat_owns_item(int item);
const char* combat_weapon_line(void);   // weapon and rounds, for the help screen
int combat_weapon_item(void);            // the item of the weapon in hand, 0 for bare hands
void combat_set_infinite_ammo(bool on);  // Cheats: firing takes no rounds
bool combat_weapon_rounds(int* in_gun, int* spare_rounds);   // false for a weapon without rounds
// A loaded save's weapon in hand (its item, 0 for bare hands), once its items are back.
void combat_restore_weapon(int item);
bool combat_dead(void);
// The ap of an attack of James' that struck the room (a wall) during the last frame, 0 if none, and
// the item of the weapon (PC 0x544fd0 and the equipped weapon 0x1f7d409, for the stage code).
float combat_wall_hit(int* weapon_item);                  // true once the death animation has finished
EnPlayer combat_enplayer(const CombatBody* b, bool light);
const char* combat_status(void);         // one line for the debug HUD
