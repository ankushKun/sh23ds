#pragma once
#include <stdio.h>
#include "sdsim.h"

// fread in pieces of 64 KB. A console's FS service takes one request at a time, so a single read of
// a large file (a town tile, a scene, a character's textures) held up every other thread's reads,
// the music's and the voices' among them, until it ended; between pieces they get their turn.
static inline size_t sd_fread(void* dst, size_t n, FILE* f)
{
	size_t done = 0;
	if (sdsim_legacy) {
		done = fread(dst, 1, n, f);
		sdsim_read((u32)done);
		return done;
	}
	while (done < n) {
		size_t want = n - done > 0x10000 ? 0x10000 : n - done;
		size_t got = fread((char*)dst + done, 1, want, f);
		sdsim_read((u32)got);           // the slow-card test charges each piece on its own
		done += got;
		if (got < want)
			break;
	}
	return done;
}
