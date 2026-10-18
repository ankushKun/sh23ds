#pragma once
#include <3ds.h>
#include "savescreen.h"

// After the results of one of James' endings the PC says "Save clear data." and, on a button,
// opens its save screen (0x447650 cases 3..5). A clear-data save holds the system block at
// 0x1dbeb80 (the clear data and the results counters) with 0x1dbeb87 = 0xff; loading one starts a
// New Game whose FlagInit reads that clear data (0x402d00 case 10). The save screen is the PC's
// file list (savescreen.c) in its clear data mode.

typedef struct {
	const SaveScreenHost* files;     // the save files (write is asked for SS_CLEAR)
	const char* (*message)(int n);   // result_msg_e.mes
} ClearSaveHost;

void clearsave_begin(const ClearSaveHost* host);
bool clearsave_update(u32 down);     // false once it is over (the caller goes to the title)
void clearsave_draw_top(void);       // the PC text on the top screen (font.c), between font_flush calls
