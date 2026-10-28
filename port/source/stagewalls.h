#pragma once
#include <3ds.h>

// The walls the stage code adds for this frame (stagewalls.c, the PC's clAddDynamicWall callers,
// read from events/stagewalls.bin); prog_stagefn_tick calls it after col_extra_clear with the PC
// room id and James' place.
void stagewalls_tick(int stage, int room, const float* me);
// One list by its exe address, for stage code with its own conditions (the labyrinth's lift, wire
// and cube room); nothing when the pack has no such list.
void stagewalls_add(u32 list);
// The town's walls on every tile instead of the 3 x 3 round James, for the playtest's planner (walkmode),
// which plans across tiles not yet loaded: a plan made without them led into streets the PC closes.
void stagewalls_all_tiles(bool on);
// The pack has no stagewalls.bin (made before it): every such wall is missing.
bool stagewalls_missing(void);
