#pragma once
#include <3ds.h>
#include <citro3d.h>

// Models a stage places in its rooms (the PC's mdl_list) that the port draws: so far the hotel
// lobby's music box (stage 40, model kind 0x524, data/chr/item/org.mdl converted by chr.py to
// pack/chr/item/org.s3c) with its cylinder and figures turning while it plays (0x57b5c0).

void stagemdl_update(int stage);                     // loads or frees the stage's models (stagemdl_draw does)
void stagemdl_draw(const C3D_Mtx* view, int uModelView, int uBones);   // skin program and lights bound
// 0x57b5c0 FUN(1, 15.0): one frame of the tune that opens the box (the right order); seconds is
// the time since it started, or < 0 to put the parts back.
void stagemdl_music_box(float seconds);
// Stage code turning a placed model (the cube in the labyrinth, 0x582bd0): the angles are added to
// the place's turn of every model of that kind, until set again (0, 0, 0 puts it back).
void stagemdl_turn(int kind, float rx, float ry, float rz);
