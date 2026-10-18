#pragma once
#include "collide.h"

// Playtest path planning (playnav.c): waypoints (x, z pairs into out, at most max) for walking from
// start to goal on the collision c with the player's body and step. Returns how many, 0 when there is
// no way at all; partial is set when the goal is not reachable within the loaded collision and the
// path ends at the reachable point nearest to it.
int nav_plan(const Collision* c, const float start[3], const float goal[3], float* out, int max, bool* partial);
// The floor heights of the last nav_plan's waypoints (for the driver's walk log).
const float* nav_last_heights(void);
// Open areas: a route over every tile of the area (a folder of bg, or two as "ca,cb") (x, y, z waypoints a few thousand units apart,
// the goal last), for walks longer than the loaded tiles reach. 0 when there is none.
int nav_area_route(const char* area, const float start[3], const float goal[3], float* out, int max);
// The floor of the whole open area under (x, z) nearest to y_ref.
bool nav_area_floor(const char* area, float x, float z, float y_ref, float* y);
