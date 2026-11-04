#pragma once
#include <3ds.h>

// Subtitles of the PC game's movies. The movie loop's subtitle step (0x43de10) shows message base + i
// of the loaded stage's message file while the movie's frame number lies between the two frames of
// entry i of a table (a start below the frame and an end not below it), at the movie's own 30 frames a
// second, when the Subtitles option is on (0x1dbec07). The tables are the PC's, handed over by each
// story program through 0x4a2b30 (movie name, table, first message) and 0x43dde0; the Movie Menu
// (0x43e900) clears them, so a movie shown from the title has none.

typedef struct {
	const char* movie;       // the name movie_play gets
	int base;                // message number of entry 0 in the stage's file
	int n;
	const u16 (*frames)[2];  // start, end per entry
} MovieSubs;

const MovieSubs* movsubs_find(const char* movie);
// The entry to show at this frame of the movie, -1 for none.
int movsubs_at(const MovieSubs* s, u32 frame);
