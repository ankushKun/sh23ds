#include "areaidx.h"
#include "stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	char name[16];
	float bb[6];
	u32 cam_size;
	const u8* cam;
} Entry;

static struct {
	char area[8];
	u8* data;
	Entry* e;
	u32 n;
	bool ok;
} I;

bool areaidx_load(const char* pack_dir, const char* area)
{
	if (!strcmp(I.area, area))
		return I.ok;
	free(I.data);
	free(I.e);
	memset(&I, 0, sizeof I);
	snprintf(I.area, sizeof I.area, "%s", area);
	char path[160];
	snprintf(path, sizeof path, "%s/bg/%s/area.idx", pack_dir, area);
	long size = 0;
	bool ok;
	I.data = stream_take_file(path, &size);   // read ahead for a door into this area
	if (I.data) {
		ok = size >= 16;
	} else {
		FILE* f = fopen(path, "rb");
		if (!f)
			return false;
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		I.data = size >= 16 ? malloc(size) : NULL;
		ok = I.data && fread(I.data, 1, size, f) == (size_t)size;
		fclose(f);
	}
	u32 h[4];
	if (ok)
		memcpy(h, I.data, sizeof h);
	if (!ok || memcmp(h, "SAI1", 4) || h[1] != 1 || h[2] > 4096) {
		free(I.data);
		I.data = NULL;
		return false;
	}
	I.e = calloc(h[2] ? h[2] : 1, sizeof *I.e);
	if (!I.e)
		return false;
	long o = 16;
	for (u32 i = 0; i < h[2] && o + 44 <= size; i++) {
		Entry* e = &I.e[I.n];
		memcpy(e->name, I.data + o, 16);
		e->name[15] = 0;
		memcpy(e->bb, I.data + o + 16, sizeof e->bb);
		memcpy(&e->cam_size, I.data + o + 40, 4);
		o += 44;
		if (o + (long)e->cam_size > size)
			break;
		e->cam = e->cam_size ? I.data + o : NULL;
		o += (e->cam_size + 3) & ~3u;
		I.n++;
	}
	I.ok = true;
	return true;
}

static const Entry* find(const char* name)
{
	for (u32 i = 0; I.ok && i < I.n; i++)
		if (!strcmp(I.e[i].name, name))
			return &I.e[i];
	return NULL;
}

bool areaidx_has(const char* name) { return find(name) != NULL; }

bool areaidx_bounds(const char* name, float bb[6])
{
	const Entry* e = find(name);
	if (!e)
		return false;
	memcpy(bb, e->bb, sizeof e->bb);
	return true;
}

const void* areaidx_cam(const char* name, u32* size)
{
	const Entry* e = find(name);
	*size = e ? e->cam_size : 0;
	return e ? e->cam : NULL;
}
