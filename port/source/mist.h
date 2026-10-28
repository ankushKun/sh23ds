#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "blood.h"

// The hyper spray's mist (the PC's effect 0x1f, spawned by 0x4a4c00 and moved by 0x4a3f60): puffs
// that leave the can along the spray, slow down, sink, grow and fade, drawn as soft camera-facing
// discs with the poison cloud texture (pack/effect/mist.t3x).

bool mist_init(const char* pack_dir, BloodBindFn bind);
void mist_exit(void);
void mist_clear(void);                   // a new room
void mist_set_color(int tier);           // the spray's colour tier (0x1dbeb89): -1, 0 (grey), 1, 2

// One frame of spraying from the can at pos along vel (the spray's reach as a vector): the PC
// spawns a puff every 60 Hz tick, so a longer frame spawns more.
void mist_spray(const float pos[3], const float vel[3], float dt);

// One puff of a Lying Figure's acid (0x4a4580) from pos along vec, in the colour grey.
void mist_poison(const float pos[3], const float vec[3], u8 grey);

void mist_update(float dt);
void mist_draw(const C3D_Mtx* proj, const C3D_Mtx* view);
int mist_count(void);                    // puffs alive (debug HUD)
