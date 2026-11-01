#pragma once
#include <3ds.h>

// Subtitles of the PC game's movies. SubtitlesExec-style code in the movie loop (0x43de41) shows the
// message base + i of the stage's message file while the movie's frame number lies between the two
// frames of entry i of a table (a start below the frame and an end not below it), at the movie's own
// 30 frames a second, when the Subtitles option is on (0x1dbec07). The tables are the PC's, set up by
// each story program with 0x4a2b30 (movie name, table, first message); the PC's Movie Menu has none, this
// port shows them there too, reading the stage's file for it.

typedef struct {
	const char* movie;       // the name movie_play gets
	int stage;               // the stage whose message file the texts are in
	int base;                // message number of entry 0
	int n;
	const u16 (*frames)[2];  // start, end per entry
} MovieSubs;

const MovieSubs* movsubs_find(const char* movie);
// The entry to show at this frame of the movie, -1 for none.
int movsubs_at(const MovieSubs* s, u32 frame);
