#pragma once
#include <3ds.h>

// James' radio: static that rises when something dangerous is near. Ported from the PC game's
// RadioNoise (0x48cbc0, called every frame from the game loop), which drives four looping DirectSound
// buffers (sddata.bin ids 5360..5363, started by 0x5182d0) through 0x514400 (track, volume, pan).
// The tracks, the enemy table (0x88cce0), the easing and the ducking behind the map, the item screen
// and the memos are the PC's. See port/SOUND.md "The radio".

// The radio's switch and volume are the PC's 0x1f7d407 / 0x1f7d408 (the item screen's On/Off and its
// 0..15 volume, 0x471d09 / 0x471e96). They live in the saved state (events.c), so a save keeps them
// and a new game starts as 0x49e980 does: on, volume 10.
bool radio_is_on(void);
int radio_volume(void);                 // 0..15
void radio_set_on(bool on);
void radio_set_volume(int volume);

// The PC's DAT_01f7d584, which stage programs set: 0 the enemies' static (needs the radio item), 1 a
// fixed point's static by its distance (the first Lying Figure's street, 0x59e295), 2 steady static
// at 0.4 (the first Lying Figure's scene, 0x59db80), 3 steady full static even when the radio is off.
enum { RADIO_ENEMIES, RADIO_POINT, RADIO_STEADY, RADIO_FORCED };
void radio_set_mode(int mode);
void radio_set_point(const float pos[3]);   // the target of mode 1
int radio_mode(void);

// What the game loop says each frame.
typedef struct {
	bool playing;        // in the game (the PC's game mode 0xd), whatever menu is up
	bool silent;         // a screen on which the PC stops the radio: options, save screen, movie, results
	int duck;            // 0 none, 1 the map or the item screen (x0.5), 2 the memos (x0.25)
	bool has_radio;      // James has the radio item (ev_have_item(19))
	float x, y, z;       // James' feet
} RadioIn;

// Once per frame with the frame's real time, menus included (the PC keeps the static going behind them).
void radio_update(const RadioIn* in, float dt);
// Silence now (title screen, a movie, leaving the game).
void radio_stop(void);
