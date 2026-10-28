#pragma once
#include <3ds.h>
#include "collide.h"

// Playtest driver: runs a script of "pt ..." lines from debug.txt that plays the game the way a
// player would (stands at an event's trigger, presses A, uses items, answers questions, skips
// scenes, saves and loads), checks flags, items, stages and rooms, and logs everything that changes
// to sdmc:/3ds/sh23ds/ptlog.txt. The playthrough suite in tests/playtest/ is built on it. Off unless
// debug.txt has "pt" lines.

enum { PT_TITLE, PT_GAME, PT_OTHER, PT_RESULTS };

typedef struct {
	int state;               // PT_*
	int stage;
	const char* map;         // current room
	float pos[3], yaw;
	bool loading;            // a warp is pending, the screen is fading or tiles are still coming in
	bool message, save_prompt, panel, picture, inv, paused, screens;
	bool prog, prog_locked, demo;
	float hp;
	bool dead;
} PtState;

typedef struct {
	void (*place)(const float pos[3], float yaw);                           // same stage, floor snapped
	bool (*floor)(float x, float z, float y_ref, float max_step, float* y); // the current collision
	void (*warp)(int stage, const char* map, const float pos[3], float yaw); // through the door fade
	void (*use_item)(int item);                                             // as the item list's "use"
	bool (*save)(void);                                                     // writes save.dat
	bool (*load)(void);                                                     // Continue from save.dat
	void (*new_game)(int wish);
	void (*shot)(void);                                                     // dumpframe of this frame
	void (*quit)(void);
	int (*inv_index)(int item);   // the item's row in the item list (-1 if James does not have it)
	const Collision* (*collision)(void);   // the loaded collision James walks on, NULL if none
} PtHost;

void playtest_init(const PtHost* host);
bool playtest_line(const char* line);     // a debug.txt line; true if it was a "pt" line
bool playtest_active(void);
bool playtest_fast(void);                 // "pt fast 1": the game draws nothing (faster emulation)
// Every frame before the game reads its input: replaces the buttons while the script runs.
void playtest_frame(const PtState* st, u32* down, u32* held, float dt);
// The circle pad, read later in the frame.
void playtest_pad(circlePosition* cp);
// The walker is steering James: it turns him tank fashion, so play takes Rotational Style meanwhile.
bool playtest_steering(void);
void playtest_message(const char* text);  // a message went up (logged)
void playtest_log(const char* fmt, ...);
