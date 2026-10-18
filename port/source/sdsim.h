#pragma once
#include <3ds.h>

// A slower SD card for tests (debug.txt "sdthrottle KB/s [open ms]"): Azahar reads the host's disk
// far faster than a console reads its SD card (roughly 5 to 10 MB/s, less with small files), which
// hides loads that would not be in when the fade ends. The readers call these around their file
// access and sleep for the time a console would have taken on top of what Azahar charges. Off (0)
// unless the switch is given. Thread-safe: they only read two words.
void sdsim_set(int kb_per_s, int open_ms);
void sdsim_open(void);                // a file opened (the directory lookup)
void sdsim_read(u32 bytes);           // bytes read
const char* sdsim_status(void);
