#pragma once
#include "room.h"

// Rays against the rooms' drawn geometry, for the behind camera (tpcam.c), a 3DS-only system. The
// collision (collide.h) is where James can walk: it has no ceilings, and its walls go as high as
// the room's tallest, so a stairwell or a lowered ceiling let the camera rise through them. These
// rays meet the triangles the player sees: the opaque batches of the rooms set here, not the
// transparent ones (railings, bars, glass) nor the parts the stage code has switched off.

// Once a frame, before the camera: the rooms drawn this frame (they stay loaded until the next call).
void camray_set(const Room* const* rooms, int n);
// A room just loaded behind a door's black: its grid made now rather than over the next frames.
void camray_warm(const Room* r);
// The share of the way from a to b before the first opaque triangle: 1 when nothing is in between.
float camray_cast(const float a[3], const float b[3]);
