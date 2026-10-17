#pragma once
#include <stdbool.h>
#include <stddef.h>

// Text tables read once and kept in memory. lighting.txt and chrlight.txt are looked up every time
// James walks onto another town tile, and scanning them line by line from the SD card cost over
// 100 ms of the main loop each time (port/PERFORMANCE.md).

// The whole file as a string (cached by path, a few files at most); NULL when it is missing.
// Valid until the next call, so callers must not keep it.
const char* text_cached(const char* path);
// Copies the next line (without the newline) into line and advances the cursor; false at the end.
bool text_next_line(const char** cursor, char* line, size_t size);
