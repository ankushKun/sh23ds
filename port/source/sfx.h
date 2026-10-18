#pragma once
#include <3ds.h>

// Sound effects from the SD pack's se.bnk (tools/sh2conv/sound.py): the game's own SE ids, mono
// PCM16 loaded on first use into linear memory, mixed by ndsp on channels 8..23 (audio.c owns 0).
// Volumes follow the PC driver (sh_sound.c): 1.0 is full, and the curve below 0.9 is the 1/r of
// a DirectSound 3D buffer placed 255 * (1 - volume) / 25.5 units away.

typedef enum { SFX_DOOR_OPEN, SFX_DOOR_CLOSE, SFX_DOOR_UNLOCK, SFX_DOOR_JAM, SFX_DOOR_LOCK } SfxDoor;

#define SFX_MENU_MOVE     10000
#define SFX_MENU_OK       10002
#define SFX_MENU_CANCEL   10003
#define SFX_SAVE_POINT    10051     // save menu opened at a save point (0x48c680)
#define SFX_GAME_START    15002     // new game, continue, load (0x498201, 0x4978c4, 0x454cc4)
#define SFX_ITEM_PICKUP   11041     // item taken (0x48c3a0)
#define SFX_LIGHT_ON      18001     // the PC has no flashlight sound: a short button click stands in
#define SFX_LIGHT_OFF     18000

// After audio_init (ndsp must be up). False if se.bnk is missing; every call is then a no-op.
bool sfx_init(void);
void sfx_exit(void);

// Plays a game SE id. volume as the game passes it (0..1), pan -1 (left) .. 1 (right).
// Restarting an id that is still playing cuts the old instance, like the PC driver.
// Returns a handle >= 0, or -1 (unknown id, no free voice, out of memory).
int sfx_play(int id, float volume, float pan);
void sfx_set_gain(float g);             // the options' effects volume, 0..1 (sounds started after it)

// SeCallPosDistanceF: volume fades with the source's and the camera's distance to the player,
// pan from where the source is on screen. Looping sounds keep following the listener.
int sfx_play_at(int id, float volume, const float pos[3]);

void sfx_stop(int id);
void sfx_stop_all(void);
bool sfx_playing(int id);

// Listener for positional sounds: the player (feet) and the camera eye, world units, Y down.
void sfx_listener(const float player[3], const float eye[3]);

// Loads an id's sample now (no SD read later when it plays).
bool sfx_preload(int id);

// Call once per frame: frees finished voices, moves looping positional voices, starts delayed
// door closes.
void sfx_update(void);

// Door sound sets (Event_DoorSound door_se[22], picked by event rslt1 bits 27..22).
int sfx_door_id(int set, SfxDoor which);
// Door warps: open at the start of the fade, close once the new room shows. The PC stops the open
// sound when it starts the close one; here the close waits until the open one has played a little
// so a fast room load does not cut it to nothing.
void sfx_door_open(int set, const float pos[3]);
void sfx_door_close(int set, const float pos[3]);
// Locked / jammed / unlock results (0x48c1c0).
void sfx_door_result(int set, SfxDoor which, const float pos[3]);

// Footsteps (0x54a9a0). sfx_floor_load reads the room's floor materials (<pack>/sound/floor/<map>.sfm);
// sfx_footsteps plays a step for every heel strike of the clip (steps.txt) the animation passed
// going from frame f0 to f1 (the clip loops between first and last).
void sfx_floor_load(const char* map);
// Hands over a tile's .sfm file read ahead (stream.c); sfx_floor_load uses it instead of the SD card.
void sfx_floor_put(const char* map, u8* data, long size);
int sfx_floor_material(float x, float y, float z);
void sfx_footsteps(const char* clip, int first, int last, float f0, float f1, bool backward, const float pos[3]);
// One of James' steps on the floor under pos (move.c says when, from the PC's frames).
void sfx_footstep(float volume, const float pos[3]);

const char* sfx_status(void);
// debug.txt "sfxsync 1": read samples on the main loop at their first play, as before.
void sfx_sync_loads(bool sync);
