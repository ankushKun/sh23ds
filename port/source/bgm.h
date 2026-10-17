#pragma once
#include <3ds.h>

// Per-stage background music, ported from the PC sound driver (sh_sound.c / snd_select.c):
// SeBgmChange (0x515c20) picks a BGM page from the stage, game flags and room; SeBgmManager
// (0x516080) fades every AIX layer in and out from the stage's sound areas around James
// (sndGetSoundAryByCharaPos, 0x5187e0). Data: <pack>/sound/bgm.bin from tools/sh2conv/sound.py.

bool bgm_init(void);           // false without bgm.bin (the caller can fall back to fixed tracks)
void bgm_exit(void);
bool bgm_ready(void);

// Once per frame in the game: stage id, its glb_crd (room lookup) and James' feet position.
void bgm_update(int stage, int glb_crd, const float pos[3], float dt);

// Situation page for the area tables: 0 calm, 2 James badly hurt, 3 enemy near (0x515690).
void bgm_set_situation(int page);

void bgm_stop(void);           // title screen: silence, forget the page
int bgm_room_id(int glb_crd, float x, float z);   // the PC room id (FUN_00494db0)
const char* bgm_status(void);
