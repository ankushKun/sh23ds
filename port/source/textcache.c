#include "textcache.h"
#include "sdio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES 4

static struct {
	char path[160];
	char* text;
} s_files[MAX_FILES];

const char* text_cached(const char* path)
{
	int slot = -1;
	for (int i = 0; i < MAX_FILES; i++) {
		if (s_files[i].text && !strcmp(s_files[i].path, path))
			return s_files[i].text;
		if (!s_files[i].text && slot < 0)
			slot = i;
	}
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* t = n >= 0 ? malloc(n + 1) : NULL;
	if (t && sd_fread(t, n, f) != (size_t)n) {
		free(t);
		t = NULL;
	}
	fclose(f);
	if (!t)
		return NULL;
	t[n] = 0;
	if (slot < 0) {      // full (more tables than expected): reuse the first slot
		slot = 0;
		free(s_files[0].text);
	}
	snprintf(s_files[slot].path, sizeof s_files[slot].path, "%s", path);
	s_files[slot].text = t;
	return t;
}

bool text_next_line(const char** cursor, char* line, size_t size)
{
	const char* p = *cursor;
	if (!p || !*p)
		return false;
	const char* e = strchr(p, '\n');
	size_t n = e ? (size_t)(e - p) : strlen(p);
	size_t k = n < size - 1 ? n : size - 1;
	memcpy(line, p, k);
	line[k] = 0;
	if (k && line[k - 1] == '\r')
		line[k - 1] = 0;
	*cursor = e ? e + 1 : p + n;
	return true;
}
