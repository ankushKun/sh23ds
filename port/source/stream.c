#include "stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOBS 6
enum { J_FREE, J_QUEUED, J_READING, J_DONE, J_TAKEN };

static struct {
	char pack[96];
	StreamJob job[JOBS];
	Thread thread;
	LightLock lock;
	LightEvent wake;
	volatile bool quit;
	bool ready;
	u32 done_count;
	u32 last_ticks;
	char status[48];
} W;

static u8* read_file(const char* path, long* size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* d = n > 0 ? malloc(n) : NULL;
	if (d && fread(d, 1, n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	*size = d ? n : 0;
	return d;
}

static void read_job(StreamJob* j)
{
	u64 t0 = svcGetSystemTick();
	char path[192];
	snprintf(path, sizeof path, "%s/bg/%s/%s.s3m", W.pack, j->area, j->name);
	j->s3m = read_file(path, &j->s3m_size);
	snprintf(path, sizeof path, "%s/bg/%s/%s.s3l", W.pack, j->area, j->name);
	j->s3l = read_file(path, &j->s3l_size);
	snprintf(path, sizeof path, "%s/sound/floor/%s.sfm", W.pack, j->name);
	j->sfm = read_file(path, &j->sfm_size);
	j->ntex = 0;
	if (j->s3m) {
		u32 ids[STREAM_MAX_TEX];
		int n = room_texture_ids(j->s3m, j->s3m_size, ids, STREAM_MAX_TEX);
		for (int i = 0; i < n; i++) {
			bool cached = false;
			for (int k = 0; k < j->ncached && !cached; k++)
				cached = j->cached[k] == ids[i];
			if (cached)
				continue;
			snprintf(path, sizeof path, "%s/bg/%s/tex/%08lx.t3x", W.pack, j->area, (unsigned long)ids[i]);
			RoomTexBlob* b = &j->tex[j->ntex];
			long size = 0;
			b->data = read_file(path, &size);
			if (b->data) {
				b->id = ids[i];
				b->size = (size_t)size;
				j->ntex++;
			}
		}
	}
	j->read_ticks = (u32)(svcGetSystemTick() - t0);
}

static void worker(void* arg)
{
	(void)arg;
	while (!W.quit) {
		LightEvent_Wait(&W.wake);
		for (;;) {
			StreamJob* j = NULL;
			LightLock_Lock(&W.lock);
			for (int i = 0; i < JOBS && !j; i++)
				if (W.job[i].state == J_QUEUED)
					j = &W.job[i];
			if (j)
				j->state = J_READING;
			LightLock_Unlock(&W.lock);
			if (!j || W.quit)
				break;
			read_job(j);
			LightLock_Lock(&W.lock);
			j->state = J_DONE;
			LightLock_Unlock(&W.lock);
		}
	}
}

bool stream_init(const char* pack_dir)
{
	memset(&W, 0, sizeof W);
	snprintf(W.pack, sizeof W.pack, "%s", pack_dir);
	LightLock_Init(&W.lock);
	LightEvent_Init(&W.wake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// one step below the main thread: the reads block in the file system and the game keeps going
	W.thread = threadCreate(worker, NULL, 32 * 1024, prio + 1, -2, false);
	W.ready = W.thread != NULL;
	return W.ready;
}

static void free_job(StreamJob* j)
{
	free(j->s3m);
	free(j->s3l);
	free(j->sfm);
	for (int i = 0; i < j->ntex; i++)
		free(j->tex[i].data);
	int key = j->key;
	memset(j, 0, sizeof *j);
	j->key = key;
	j->state = J_FREE;
}

void stream_exit(void)
{
	if (!W.ready)
		return;
	W.quit = true;
	LightEvent_Signal(&W.wake);
	threadJoin(W.thread, U64_MAX);
	threadFree(W.thread);
	for (int i = 0; i < JOBS; i++)
		if (W.job[i].state != J_FREE)
			free_job(&W.job[i]);
	W.ready = false;
}

bool stream_pending(int key, const char* area)
{
	bool found = false;
	LightLock_Lock(&W.lock);
	for (int i = 0; i < JOBS; i++) {
		StreamJob* j = &W.job[i];
		if (j->state != J_FREE && j->key == key && !strcmp(j->area, area))
			found = true;
	}
	LightLock_Unlock(&W.lock);
	return found;
}

bool stream_request(int key, const char* area, const char* name)
{
	if (!W.ready || stream_pending(key, area))
		return W.ready;
	StreamJob* j = NULL;
	LightLock_Lock(&W.lock);
	for (int i = 0; i < JOBS && !j; i++)
		if (W.job[i].state == J_FREE)
			j = &W.job[i];
	if (j) {
		memset(j, 0, sizeof *j);
		j->key = key;
		snprintf(j->area, sizeof j->area, "%s", area);
		snprintf(j->name, sizeof j->name, "%s", name);
		j->ncached = texcache_ids(area, j->cached, STREAM_MAX_TEX * 2);
		j->state = J_QUEUED;
	}
	LightLock_Unlock(&W.lock);
	if (j)
		LightEvent_Signal(&W.wake);
	return j != NULL;
}

StreamJob* stream_take(void)
{
	StreamJob* j = NULL;
	LightLock_Lock(&W.lock);
	for (int i = 0; i < JOBS && !j; i++)
		if (W.job[i].state == J_DONE && W.job[i].key >= 0)   // not the warp prefetch
			j = &W.job[i];
	if (j)
		j->state = J_TAKEN;
	LightLock_Unlock(&W.lock);
	if (j) {
		W.done_count++;
		W.last_ticks = j->read_ticks;
	}
	return j;
}

StreamJob* stream_take_named(const char* area, const char* name)
{
	for (;;) {
		StreamJob* j = NULL;
		bool busy = false;
		LightLock_Lock(&W.lock);
		for (int i = 0; i < JOBS && !j; i++) {
			StreamJob* k = &W.job[i];
			if (k->state == J_FREE || k->state == J_TAKEN || strcmp(k->area, area) || strcmp(k->name, name))
				continue;
			if (k->state == J_DONE)
				j = k;
			else
				busy = true;
		}
		if (j)
			j->state = J_TAKEN;
		LightLock_Unlock(&W.lock);
		if (j || !busy)
			return j;
		svcSleepThread(2000000);          // still being read: that is quicker than reading it again
	}
}

void stream_release(StreamJob* j)
{
	LightLock_Lock(&W.lock);
	free_job(j);
	LightLock_Unlock(&W.lock);
}

void stream_flush(const char* area)
{
	LightLock_Lock(&W.lock);
	for (int i = 0; i < JOBS; i++) {
		StreamJob* j = &W.job[i];
		if ((j->state == J_QUEUED || j->state == J_DONE) && strcmp(j->area, area))
			free_job(j);
	}
	LightLock_Unlock(&W.lock);
}

bool stream_named_busy(const char* area, const char* name)
{
	bool busy = false;
	LightLock_Lock(&W.lock);
	for (int i = 0; i < JOBS; i++) {
		StreamJob* k = &W.job[i];
		if ((k->state == J_QUEUED || k->state == J_READING) && !strcmp(k->area, area) && !strcmp(k->name, name))
			busy = true;
	}
	LightLock_Unlock(&W.lock);
	return busy;
}

void stream_drop_prefetch(void)
{
	LightLock_Lock(&W.lock);
	for (int i = 0; i < JOBS; i++) {
		StreamJob* j = &W.job[i];
		if (j->key == STREAM_PREFETCH && (j->state == J_QUEUED || j->state == J_DONE))
			free_job(j);
	}
	LightLock_Unlock(&W.lock);
}

const char* stream_status(void)
{
	int busy = 0;
	for (int i = 0; i < JOBS; i++)
		busy += W.job[i].state == J_QUEUED || W.job[i].state == J_READING;
	snprintf(W.status, sizeof W.status, "stream %s jobs %d done %lu last %.0f ms", W.ready ? "on" : "off", busy,
	         (unsigned long)W.done_count, W.last_ticks * 1000.0f / SYSCLOCK_ARM11);
	return W.status;
}
