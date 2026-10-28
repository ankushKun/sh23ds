#include "levels.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// <dir>/levels.txt as levels.py writes it: "name stream LUFS peak" per line, # comments. Each folder
// is read once, the first time one of its sounds is asked for.
typedef struct { char dir[64]; char name[40]; short stream; float lufs, peak; } Level;

#define MAX_LEVELS 256
#define MAX_DIRS 4
static Level s_lv[MAX_LEVELS];
static int s_n;
static char s_read[MAX_DIRS][64];
static int s_nread;

static void read_dir(const char* dir)
{
	for (int i = 0; i < s_nread; i++)
		if (!strcmp(s_read[i], dir))
			return;
	if (s_nread < MAX_DIRS)
		snprintf(s_read[s_nread++], sizeof s_read[0], "%s", dir);
	char path[96], line[128];
	snprintf(path, sizeof path, "%s/levels.txt", dir);
	FILE* f = fopen(path, "r");
	if (!f)
		return;
	while (s_n < MAX_LEVELS && fgets(line, sizeof line, f)) {
		Level* l = &s_lv[s_n];
		int stream;
		if (line[0] == '#' || sscanf(line, "%39s %d %f %f", l->name, &stream, &l->lufs, &l->peak) != 4)
			continue;
		snprintf(l->dir, sizeof l->dir, "%s", dir);
		l->stream = (short)stream;
		s_n++;
	}
	fclose(f);
}

float levels_gain(const char* dir, const char* name, int stream, float target, float max_peak, float fallback)
{
	read_dir(dir);
	for (int i = 0; i < s_n; i++) {
		const Level* l = &s_lv[i];
		if (l->stream != stream || strcmp(l->name, name) || strcmp(l->dir, dir))
			continue;
		if (l->lufs <= -69.0f)
			return fallback;                  // silence (or nothing the gates kept): as it was
		float db = target - l->lufs, cap = max_peak - l->peak;
		return powf(10.0f, (db < cap ? db : cap) / 20.0f);
	}
	return fallback;
}

void levels_limiter(LevelLimiter* l, s32* lr, int frames, u32 rate)
{
	const float ceil = 24576.0f;
	float back = 32.0f / (0.08f * (float)(rate ? rate : 32000));   // per block towards 1
	if (l->gain <= 0.0f || l->gain > 1.0f)
		l->gain = 1.0f;
	for (int i = 0; i < frames; i += 32) {
		int n = frames - i < 32 ? frames - i : 32;
		s32* b = lr + 2 * i;
		s32 peak = 0;
		for (int k = 0; k < 2 * n; k++) {
			s32 a = b[k] < 0 ? -b[k] : b[k];
			peak = a > peak ? a : peak;
		}
		float need = (float)peak > ceil ? ceil / (float)peak : 1.0f;
		float g0 = l->gain, g1 = g0 + (1.0f - g0) * (back > 1.0f ? 1.0f : back);
		if (need < g1) {
			l->gain = need;                   // down at once, for the whole block
			if (need >= 1.0f)
				continue;
			for (int k = 0; k < 2 * n; k++)
				b[k] = (s32)((float)b[k] * need);
		} else {
			l->gain = g1;                     // back up, ramped across the block
			if (g0 >= 1.0f && g1 >= 1.0f)
				continue;
			for (int j = 0; j < n; j++) {
				float g = g0 + (g1 - g0) * (float)(j + 1) / (float)n;
				b[2 * j] = (s32)((float)b[2 * j] * g);
				b[2 * j + 1] = (s32)((float)b[2 * j + 1] * g);
			}
		}
	}
}
