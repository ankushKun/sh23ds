#pragma once
#include <3ds.h>
#include <stdbool.h>

// One loudness per kind of streamed sound (3DS-only). The PC plays its music, scene voices and movies
// at the levels they were mastered at, which lie far apart (a stage's music 15 LU under the next, a
// movie 20 LU over another), and on the console's small speakers the quiet ones were hard to hear.
// The packer measures every file (tools/sh2conv/levels.py, <dir>/levels.txt) and these targets are
// what each kind is brought to, in LUFS at full option volume (port/SOUND.md, "Levels").
#define LEVEL_MUSIC  -11.0f      // an AIX file with all its layers on; fewer layers play quieter
#define LEVEL_VOICE  -10.0f      // scene dialogue, a little over the music under it
#define LEVEL_MOVIE  -11.0f

// How far over full scale each kind's peak may be lifted (dBFS); levels_limiter takes what goes over
// back down. Speech's loud consonants stand far over the voice around them, so it may go furthest.
#define LEVEL_PEAK_MUSIC   3.0f
#define LEVEL_PEAK_MOVIE   6.0f
#define LEVEL_PEAK_VOICE   9.0f

// The gain that brings <dir>/<name> (stream: a movie's audio track, -1 a file's streams together) to
// target, at most what puts its peak at max_peak. fallback when the pack has no measurement for it (a
// pack made before levels.py).
float levels_gain(const char* dir, const char* name, int stream, float target, float max_peak, float fallback);

// A peak limiter for the sound mixed in software: each block of 32 stereo frames is turned down at
// once as far as keeps its peak at 3/4 of full scale, and the gain comes back over about 80 ms.
// gain 0 (a zeroed struct) starts at 1.
typedef struct { float gain; } LevelLimiter;
void levels_limiter(LevelLimiter* l, s32* lr, int frames, u32 rate);

// A sample mixed in software, to 16 bits: as it is up to 3/4 of full scale, bent smoothly towards
// full scale above (the slope stays 1 at the knee), in place of clipping it
static inline s16 levels_limit(s32 v)
{
	const s32 knee = 24576, room = 32767 - knee;
	s32 a = v < 0 ? -v : v;
	if (a <= knee)
		return (s16)v;
	s32 e = a - knee;
	s32 y = knee + (s32)((s64)e * room / (e + room));
	return (s16)(v < 0 ? -y : y);
}
