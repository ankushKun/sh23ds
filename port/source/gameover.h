#pragma once
#include <3ds.h>
#include "map.h"

// The PC's game over (event 0xd, 0x498780): the sound stops and voice.afs 143 plays (SE 60143),
// then on black the "GAME OVER" of data/pic/etc/gameover1.tex (0x496c90 / 0x496ee0): after a
// second it comes up over 0.8 s, stays two seconds and goes in 0.4 s, three copies of it jumping
// up and down by up to 15 lines round a steady one; then back to the title (0x401fc0).

bool gameover_init(const char* pack_dir);
void gameover_start(void);
bool gameover_active(void);
// Once a frame: false when it is over (the caller goes to the title).
bool gameover_update(float dt);
void gameover_draw_top(MapDrawFn draw);
