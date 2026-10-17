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
// The job for a room by name (the warp prefetch, key STREAM_PREFETCH), waiting while the reader
// thread is still on it; NULL when there is none. Hand it back with stream_release.
StreamJob* stream_take_named(const char* area, const char* name);
#define STREAM_PREFETCH (-1)
// True while the reader thread has not finished that room yet.
bool stream_named_busy(const char* area, const char* name);
// Drops prefetches nobody took (the warp went somewhere else).
void stream_drop_prefetch(void);
// Frees whatever the job still owns (s3m is set to NULL by the caller once room_load_mem has it).
void stream_release(StreamJob* j);
// Drops the queued jobs and the results of other areas (area change).
void stream_flush(const char* area);
// Single files read ahead (a door's stage events and enemy models). An .s3c brings the textures it
// lists. stream_take_file returns the contents (the caller frees them), waiting while the file is
// still being read, or NULL when it was not asked for.
void stream_want_file(const char* path);
void* stream_take_file(const char* path, long* size);
bool stream_files_busy(void);
void stream_drop_files(void);           // what nobody took
const char* stream_status(void);
