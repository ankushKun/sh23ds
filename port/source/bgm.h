#pragma once
#include <3ds.h>

// Per-stage background music, ported from the PC sound driver (sh_sound.c / snd_select.c):
// SeBgmChange (0x515c20) picks a BGM page from the stage, game flags and room; SeBgmManager
// (0x516080) fades every AIX layer in and out from the stage's sound areas around James
// (sndGetSoundAryByCharaPos, 0x5187e0), with the situation page from the stage's bgm_control
// (0x515690), which follows the flags and the scene playing. Data: <pack>/sound/bgm.bin from
// tools/sh2conv/sound.py.

bool bgm_init(void);           // false without bgm.bin (the caller can fall back to fixed tracks)
void bgm_exit(void);
bool bgm_ready(void);

// Once per frame in the game: stage id, its glb_crd (room lookup) and James' feet position.
void bgm_update(int stage, int glb_crd, const float pos[3], float dt);

// FUN_005155e0, which stage programs call: this page from now on (its tracks fade in by the areas
// once the old ones are out), until the next warp or stage change picks the stage's page again.
void bgm_set_page(int page);
bool bgm_page_pending(void);   // the page asked for has not started yet (0x1f837c8 != 0)
void bgm_warped(void);         // a warp: SeBgmChange picks the page again, as the PC does on a load
void bgm_set_music_box(bool on);   // 0x1d85fa4: the music box plays with the boxes in a wrong order

void bgm_stop(void);           // title screen: silence, forget the page
int bgm_room_id(int glb_crd, float x, float z);   // the PC room id (FUN_00494db0)
const char* bgm_status(void);
