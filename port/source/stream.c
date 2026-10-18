#include "stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOBS 6
#define FILES 64
enum { J_FREE, J_QUEUED, J_READING, J_DONE, J_TAKEN };

// Single files read ahead for a door (stage events, enemy models): stream_want_file queues one,
// stream_take_file hands its contents over. An .s3c queues the textures it lists as well.
typedef struct {
	char path[112];
	u8* data;
	long size;
	int state;
} FileJob;

static struct {
	char pack[96];
	StreamJob job[JOBS];
	FileJob file[FILES];
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
	if (j->s3m)
		room_prepare(j->s3m, j->s3m_size);   // the index rebase, off the main thread
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

static void want_locked(const char* path);

// An .s3c's texture ids (chr.c's S3C v2 header: textures at +0x18, their offset at +0x4c)
static void want_textures(const char* s3c, const u8* d, long size)
{
	if (size < 0x50 || memcmp(d, "S3C1", 4))
		return;
	u32 n, ofs;
	memcpy(&n, d + 0x18, 4);
	memcpy(&ofs, d + 0x4c, 4);
	if (n > 8 || ofs + n * 4 > (u32)size)
		return;
	char dir[112];
	snprintf(dir, sizeof dir, "%s", s3c);
	char* slash = strrchr(dir, '/');
	if (!slash)
		return;
	*slash = 0;
	for (u32 i = 0; i < n; i++) {
		u32 id;
		memcpy(&id, d + ofs + i * 4, 4);
		char path[160];
		snprintf(path, sizeof path, "%s/tex/%08lx.t3x", dir, (unsigned long)id);
		want_locked(path);
	}
}

static bool read_files(void)
{
	FileJob* f = NULL;
	LightLock_Lock(&W.lock);
	for (int i = 0; i < FILES && !f; i++)
		if (W.file[i].state == J_QUEUED)
			f = &W.file[i];
	if (f)
		f->state = J_READING;
	LightLock_Unlock(&W.lock);
	if (!f)
		return false;
	f->data = read_file(f->path, &f->size);
	LightLock_Lock(&W.lock);
	f->state = J_DONE;
	size_t l = strlen(f->path);
	if (f->data && l > 4 && !strcmp(f->path + l - 4, ".s3c"))
		want_textures(f->path, f->data, f->size);
	LightLock_Unlock(&W.lock);
	return true;
}

static void worker(void* arg)
{
	(void)arg;
	while (!W.quit) {
		LightEvent_Wait(&W.wake);
		while (!W.quit && read_files())
			;
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
			while (!W.quit && read_files())
				;
		}
		while (!W.quit && read_files())
			;
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
	for (int i = 0; i < FILES; i++)
		free(W.file[i].data);
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

// Lock held. Queues a file unless it is queued or held already.
static void want_locked(const char* path)
{
	FileJob* free_one = NULL;
	for (int i = 0; i < FILES; i++) {
		FileJob* f = &W.file[i];
		if (f->state != J_FREE && !strcmp(f->path, path))
			return;
		if (f->state == J_FREE && !free_one)
			free_one = f;
	}
	if (!free_one)
		return;
	snprintf(free_one->path, sizeof free_one->path, "%s", path);
	free_one->data = NULL;
	free_one->size = 0;
	free_one->state = J_QUEUED;
}

void stream_want_file(const char* path)
{
	if (!W.ready)
		return;
	LightLock_Lock(&W.lock);
	want_locked(path);
	LightLock_Unlock(&W.lock);
	LightEvent_Signal(&W.wake);
}

void* stream_take_file(const char* path, long* size)
{
	if (!W.ready)
		return NULL;
	for (;;) {
		FileJob* f = NULL;
		bool busy = false;
		LightLock_Lock(&W.lock);
		for (int i = 0; i < FILES && !f; i++) {
			FileJob* k = &W.file[i];
			if (k->state == J_FREE || strcmp(k->path, path))
				continue;
			if (k->state == J_DONE)
				f = k;
			else
				busy = true;
		}
		void* d = NULL;
		if (f) {
			d = f->data;
			*size = f->size;
			f->data = NULL;
			f->state = J_FREE;
		}
		LightLock_Unlock(&W.lock);
		if (f)
			return d;                    // NULL when the read failed: the caller tries itself
		if (!busy)
			return NULL;
		svcSleepThread(2000000);
	}
}

int stream_file_state(const char* path)
{
	if (!W.ready)
		return -2;
	int state = -1;
	LightLock_Lock(&W.lock);
	for (int i = 0; i < FILES && state < 0; i++) {
		const FileJob* f = &W.file[i];
		if (f->state != J_FREE && !strcmp(f->path, path))
			state = f->state == J_DONE ? 1 : 0;
	}
	LightLock_Unlock(&W.lock);
	return state;
}

bool stream_files_busy(void)
{
	bool busy = false;
	LightLock_Lock(&W.lock);
	for (int i = 0; i < FILES; i++)
		busy |= W.file[i].state == J_QUEUED || W.file[i].state == J_READING;
	LightLock_Unlock(&W.lock);
	return busy;
}

void stream_drop_files(void)
{
	LightLock_Lock(&W.lock);
	for (int i = 0; i < FILES; i++) {
		FileJob* f = &W.file[i];
		if (f->state == J_DONE || f->state == J_QUEUED) {
			free(f->data);
			memset(f, 0, sizeof *f);
		}
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
