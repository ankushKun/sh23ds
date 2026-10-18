#pragma once

// The stage code's map part switches for this frame (stageparts.c); prog_stagefn_tick calls it
// with the PC room id James is in.
void stageparts_tick(int stage, int room, float dt);
