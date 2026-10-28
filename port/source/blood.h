#pragma once
#include <3ds.h>
#include <citro3d.h>

// The blood a fight leaves (the PC's Effect2 "impact queue" classes, 0x4ca720 registers them):
//   hit splash   0x4c7d40 posts class 5 (0x4eac00) at the hit enemy: for half a second it follows
//                the enemy and lets out spray (up to 8, every 1/160 s, along the hit) and drops (up
//                to 6, every 1/15 s, straight out) as class 11 (0x4cee80) sprites from
//                data/pic/effect/bloodtex0.tbn2; a drop that reaches the floor leaves a stain
//                (class 10, 0x4cf710) for 30 seconds
//   pool         enDyingExec (0x4a96c0) posts class 3 (0x4eb460): three class 12 (0x4ce260) pools
//                spread out round the body over the next seconds and grow for 16 seconds
// The stains and pools use a round blob made at start-up (0x507930). With blend-op support
// (0xa35f9c, D3DPMISCCAPS_BLENDOP) everything is subtracted from the picture in the Blood Color's
// colours (0x4c7bd0 hands the option to the classes), so floors darken towards the blood's colour.
// See PROGRAMS.md "Blood".

// Sets the program for world-space triangles with the vertex colour passed through (main.c).
typedef void (*BloodBindFn)(const C3D_Mtx* proj, const C3D_Mtx* view);
// The floor under (x, z) near y_ref (Y down) from the room's collision; false: none known.
typedef bool (*BloodFloorFn)(float x, float z, float y_ref, float* y);

bool blood_init(const char* pack_dir, BloodBindFn bind, BloodFloorFn floor);
void blood_exit(void);
void blood_clear(void);                  // a new room: nothing carries over
void blood_set_color(int color);         // Blood Color 0 Normal, 1 Green, 2 Violet, 3 Black

// A hit: pos the hit point, dir from the attacker to the enemy (any length), attack the PC attack id
// (it sets the spray's strength, 0x4c7d40), owner the enemy's x, y, z (the splash follows it for
// half a second), floor_y the floor under it (Y down).
void blood_hit(const float pos[3], const float dir[3], int attack, const float* owner, float floor_y);
// A death: the pool round owner (x, y, z, followed while it lies there). room is the PC room id
// (bgm_room_id): 0x4c73a0 leaves rooms 7 and 0x21 without and gives the rooms whose floor holds no
// pool six spurts instead (class 4, 0x4eafd0).
void blood_pool(const float* owner, int room);
// One of James' steps (main.c, move.c's footstep frames): a step in a pool starts five bloody
// footprints (class 1 0x4ebe70 -> class 8 0x4e95f0 -> class 9 0x4cb930), one a step after it.
void blood_footstep(const float foot[3], float yaw);
// The owner is going away (its slot will be reused): its effects keep their last place.
void blood_forget(const float* owner);

void blood_update(float dt);
// After the opaque scene, with the light environment unbound. fog_start, fog_end: the room's fog
// (D3DRS_FOGSTART / FOGEND), near, far: the depth range, for the blood's own fog table. It leaves the
// fog colour black and its table bound: the caller puts the room's back.
void blood_draw(const C3D_Mtx* proj, const C3D_Mtx* view, float fog_start, float fog_end, float near, float far);
const char* blood_status(void);          // debug HUD line
