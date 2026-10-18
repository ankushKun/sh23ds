#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "collide.h"
#include "enemy.h"

// Maria as James' companion (chr/mar lll_mar), ported from the PC follower code (m3_maria_sub.c in
// the PS2 beta's names, 0x529480..0x52df40 in sh2pc.exe). She is there while game flag 15 is set
// (gGameFlag 0x8000, raised and cleared by the stage programs), walks after James along a trail of
// his recent positions, can be hurt by the enemies' blows and, if she dies, the game is over.
// enemy.c drives the whole module, so it needs no hooks of its own in main.c.

void maria_init(const char* pack);
void maria_exit(void);
void maria_reset(void);                       // new game or loaded save: full health, no room yet
void maria_room_enter(const Collision* col);  // a room was loaded (MariaInitOnConnect)
void maria_update(const EnPlayer* p, float dt);
void maria_draw(const C3D_Mtx* view, int uModelView, int uBones);

// An enemy blow from (x, z) facing yaw, reaching `reach` within `arc` radians to either side:
// true if it caught her (she takes `ap`).
bool maria_hit(float x, float z, float yaw, float reach, float arc, float ap);
bool maria_present(void);
// The ending counters' distance to James (0x1fb3b14): 0 before she first comes along, her last
// distance after she has left.
float maria_distance(void);
// Maria as an enemy's target (0x542ad0: an enemy goes for whichever of the two is closer). False
// when she is not following James or is down.
bool maria_target(const EnPlayer* james, EnPlayer* out);                    // to James, < 0 when she is not with him (ending score)
bool maria_dead(void);                         // her death has played out: game over
const char* maria_status(void);
