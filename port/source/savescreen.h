#pragma once
#include <3ds.h>

// The PC's save and load screens (0x453fc0, drawn by 0x4537b0): the save folder's files listed
// five to a page over James' picture (Maria's in Born From a Wish), each with the place it was
// saved at, the play time and the number of saves, an overwrite question and "Save completed.".
// Drawn on the top screen in the PC font, driven from the touch screen (touchui.c) and buttons.

#define SAVE_SLOTS 10

typedef struct {
	bool used, clear;        // a file is there; it is clear data
	int place;               // the save point (m_card_x_msg_e.mes place name), -1 unknown
	int stage, chapter, action, riddle, saves, clears;
	float time;              // play time in seconds
	char map[8];
} SaveSlotInfo;

enum { SS_SAVE, SS_LOAD, SS_CLEAR };

typedef struct {
	bool (*info)(int slot, SaveSlotInfo* out);
	bool (*write)(int slot, int mode);       // SS_SAVE: the game, SS_CLEAR: clear data
	void (*load)(int slot);                  // SS_LOAD: the chosen file (the screen closes first)
} SaveScreenHost;

void savescreen_open(int mode, const SaveScreenHost* host, int chapter);
bool savescreen_active(void);
bool savescreen_update(u32 down);           // true while it is open
void savescreen_draw_top(void);             // inside a C3D frame, after the scene (font.c draws)
