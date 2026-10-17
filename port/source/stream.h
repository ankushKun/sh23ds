#pragma once
#include <3ds.h>
#include "room.h"

// Background reader for streamed tiles: the main loop asks for a room, a worker thread reads its
// .s3m, .s3l and the textures the cache does not have into memory, and the main thread turns them
// into GPU data later (room_load_mem / col_load_mem). Walking through the town then never waits
// for the SD card. See port/PERFORMANCE.md.

#define STREAM_MAX_TEX 96

typedef struct {
	int key;                         // the caller's id (room index in the area)
	char area[8], name[16];
	u8* s3m;                         // owned until handed to room_load_mem
	long s3m_size;
	u8* s3l;
	long s3l_size;
	u8* sfm;                         // footstep floor table (sfx_floor_put takes it)
	long sfm_size;
	RoomTexBlob tex[STREAM_MAX_TEX];
	int ntex;
	u32 cached[STREAM_MAX_TEX * 2];  // ids the texture cache had when asked (not read again)
	int ncached;
	u32 read_ticks;                  // time the worker spent reading
	int state;                       // internal
} StreamJob;

bool stream_init(const char* pack_dir);
void stream_exit(void);
// Queues a room unless it is queued already; false when the queue is full.
bool stream_request(int key, const char* area, const char* name);
bool stream_pending(int key, const char* area);
// A finished job (main thread), or NULL. Hand it back with stream_release.
StreamJob* stream_take(void);
// Frees whatever the job still owns (s3m is set to NULL by the caller once room_load_mem has it).
void stream_release(StreamJob* j);
// Drops queued jobs and the results of jobs of other areas (area change).
void stream_flush(const char* area);
const char* stream_status(void);
