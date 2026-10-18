#include "sfx.h"
#include "sdsim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define BANK_PATH PACK_DIR "/sound/se/se.bnk"

#define FIRST_CH 8                  // ndsp channels 8..23; audio.c uses 0
#define NVOICES 16
#define CACHE_BYTES (4u << 20)      // linear memory for loaded samples
#define MAX_STEPS 32

typedef struct { u32 offset, frames, rate, flags; } BankSample;
typedef struct { u32 id, sample; } BankId;
typedef struct { u16 se[6]; } BankDoor;

typedef struct {
	s16* pcm;            // linear memory, NULL until loaded
	u32 last_use;        // frame of the last play, for eviction
	bool pinned;         // preloaded: never evicted
	volatile u8 state;   // L_READY, or L_QUEUED / L_DONE / L_FAILED while the reader thread has it
} Loaded;

enum { L_READY, L_QUEUED, L_DONE, L_FAILED };

typedef struct {
	bool active, positional;
	bool waiting;        // its sample is still being read; sfx_update starts it
	int id, sample;
	float volume, pos[3];
	u32 started;
	ndspWaveBuf wb;
} Voice;

typedef struct { char clip[8]; int frame, foot; } Step;

static bool s_ok;
static FILE* s_bank;
static u32 s_data_offset, s_nsamples, s_nids, s_ndoors;
static BankSample* s_samples;
static BankId* s_ids;
static BankDoor* s_doors;
static Loaded* s_loaded;
static u32 s_cache_used;
static Voice s_voices[NVOICES];
static u32 s_tick;
static float s_player[3], s_eye[3];
static bool s_have_listener;
static Step s_steps[MAX_STEPS];
static int s_nsteps;
static float* s_floor;               // 10 floats per triangle: 9 coordinates, material
static u32 s_nfloor;
static struct { int id, set; float pos[3]; u32 not_before; } s_pending_close;
static char s_status[64] = "sfx off";

// A sample's first play used to read se.bnk on the main loop, 1..30 ms for a sound and more for a
// long one (port/PERFORMANCE.md). Now the voice waits a frame or two while this thread reads the
// sample into the linear buffer the main thread allocated for it; sfx_update starts the voice.
// debug.txt "sfxsync 1" reads on the main loop as before.
#define QUEUE 32
static struct {
	Thread thread;
	LightLock lock;
	LightEvent wake;
	volatile bool quit;
	FILE* bank;          // its own handle: the main thread's s_bank keeps its file position
	int queue[QUEUE];
	int head, tail;
	bool sync;
	u32 async_loads, sync_loads;
	float sync_max_ms;
} R;

// ---------------------------------------------------------------- bank

static int find_sample(int id)
{
	u32 lo = 0, hi = s_nids;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (s_ids[mid].id < (u32)id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < s_nids && s_ids[lo].id == (u32)id ? (int)s_ids[lo].sample : -1;
}

static bool sample_in_use(int s)
{
	for (int i = 0; i < NVOICES; i++)
		if (s_voices[i].active && s_voices[i].sample == s)
			return true;
	return false;
}

// Frees the least recently used samples until need more bytes fit the budget.
static bool make_room(u32 need)
{
	while (s_cache_used + need > CACHE_BYTES) {
		int best = -1;
		for (u32 i = 0; i < s_nsamples; i++) {
			Loaded* l = &s_loaded[i];
			if (!l->pcm || l->pinned || l->state != L_READY || sample_in_use(i))
				continue;
			if (best < 0 || l->last_use < s_loaded[best].last_use)
				best = i;
		}
		if (best < 0)
			return false;
		linearFree(s_loaded[best].pcm);
		s_loaded[best].pcm = NULL;
		s_cache_used -= s_samples[best].frames * 2;
	}
	return true;
}

static void finish_load(int s);

static bool load_sample(int s)
{
	Loaded* l = &s_loaded[s];
	while (l->state == L_QUEUED)          // the reader thread has it: wait for that read instead
		svcSleepThread(1000000);
	if (l->state != L_READY)
		finish_load(s);
	if (l->pcm)
		return true;
	const BankSample* b = &s_samples[s];
	u32 bytes = b->frames * 2;
	if (!bytes || !make_room(bytes))
		return false;
	l->pcm = linearAlloc(bytes);
	if (!l->pcm)
		return false;
	u64 t0 = svcGetSystemTick();
	if (fseek(s_bank, s_data_offset + b->offset, SEEK_SET) || fread(l->pcm, 1, bytes, s_bank) != bytes) {
		linearFree(l->pcm);
		l->pcm = NULL;
		return false;
	}
	float ms = (svcGetSystemTick() - t0) * 1000.0f / SYSCLOCK_ARM11;
	R.sync_loads++;
	if (ms > R.sync_max_ms)
		R.sync_max_ms = ms;
	DSP_FlushDataCache(l->pcm, bytes);
	s_cache_used += bytes;
	return true;
}

static void reader(void* arg)
{
	(void)arg;
	while (!R.quit) {
		LightEvent_Wait(&R.wake);
		for (;;) {
			int s = -1;
			LightLock_Lock(&R.lock);
			if (R.head != R.tail) {
				s = R.queue[R.tail];
				R.tail = (R.tail + 1) % QUEUE;
			}
			LightLock_Unlock(&R.lock);
			if (s < 0 || R.quit)
				break;
			const BankSample* b = &s_samples[s];
			Loaded* l = &s_loaded[s];
			u32 bytes = b->frames * 2;
			bool ok = !fseek(R.bank, s_data_offset + b->offset, SEEK_SET) && fread(l->pcm, 1, bytes, R.bank) == bytes;
			sdsim_read(bytes);
			l->state = ok ? L_DONE : L_FAILED;
		}
	}
}

// Main thread: a read the reader thread finished becomes a usable sample (or is dropped).
static void finish_load(int s)
{
	Loaded* l = &s_loaded[s];
	if (l->state == L_DONE) {
		DSP_FlushDataCache(l->pcm, s_samples[s].frames * 2);
		l->state = L_READY;
	} else if (l->state == L_FAILED) {
		linearFree(l->pcm);
		l->pcm = NULL;
		s_cache_used -= s_samples[s].frames * 2;
		l->state = L_READY;
	}
}

// Queues a sample for the reader thread; false when it has to be read now.
static bool request_load(int s)
{
	Loaded* l = &s_loaded[s];
	if (R.sync || !R.thread || l->pcm)
		return false;
	const BankSample* b = &s_samples[s];
	u32 bytes = b->frames * 2;
	LightLock_Lock(&R.lock);
	bool full = (R.head + 1) % QUEUE == R.tail;
	LightLock_Unlock(&R.lock);
	if (!bytes || full || !make_room(bytes))
		return false;
	l->pcm = linearAlloc(bytes);
	if (!l->pcm)
		return false;
	s_cache_used += bytes;
	l->state = L_QUEUED;
	LightLock_Lock(&R.lock);
	R.queue[R.head] = s;
	R.head = (R.head + 1) % QUEUE;
	LightLock_Unlock(&R.lock);
	LightEvent_Signal(&R.wake);
	R.async_loads++;
	return true;
}

static void load_steps(void)
{
	FILE* f = fopen(PACK_DIR "/sound/steps.txt", "r");
	if (!f)
		return;
	char line[64];
	while (s_nsteps < MAX_STEPS && fgets(line, sizeof line, f)) {
		Step* st = &s_steps[s_nsteps];
		if (line[0] != '#' && sscanf(line, "%7s %d %d", st->clip, &st->frame, &st->foot) == 3)
			s_nsteps++;
	}
	fclose(f);
}

bool sfx_init(void)
{
	s_bank = fopen(BANK_PATH, "rb");
	if (!s_bank) {
		snprintf(s_status, sizeof s_status, "missing se.bnk");
		return false;
	}
	u32 h[8];
	if (fread(h, 4, 8, s_bank) != 8 || memcmp(h, "S3SB", 4) || h[1] != 1) {
		snprintf(s_status, sizeof s_status, "bad se.bnk");
		fclose(s_bank);
		s_bank = NULL;
		return false;
	}
	s_nsamples = h[2], s_nids = h[3], s_ndoors = h[4], s_data_offset = h[7];
	s_samples = malloc(s_nsamples * sizeof *s_samples);
	s_ids = malloc(s_nids * sizeof *s_ids);
	s_doors = malloc(s_ndoors * sizeof *s_doors);
	s_loaded = calloc(s_nsamples, sizeof *s_loaded);
	bool ok = s_samples && s_ids && s_doors && s_loaded &&
	          fread(s_samples, sizeof *s_samples, s_nsamples, s_bank) == s_nsamples &&
	          !fseek(s_bank, h[5], SEEK_SET) && fread(s_ids, sizeof *s_ids, s_nids, s_bank) == s_nids &&
	          !fseek(s_bank, h[6], SEEK_SET) && fread(s_doors, sizeof *s_doors, s_ndoors, s_bank) == s_ndoors;
	if (!ok) {
		s_ok = true;     // lets sfx_exit free what was allocated
		sfx_exit();
		snprintf(s_status, sizeof s_status, "bad se.bnk");
		return false;
	}
	memset(s_voices, 0, sizeof s_voices);
	for (int i = 0; i < NVOICES; i++)
		ndspChnReset(FIRST_CH + i);
	s_ok = true;
	load_steps();
	R.bank = fopen(BANK_PATH, "rb");
	if (R.bank) {
		LightLock_Init(&R.lock);
		LightEvent_Init(&R.wake, RESET_ONESHOT);
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		R.thread = threadCreate(reader, NULL, 16 * 1024, prio + 1, -2, false);
	}
	// Sounds that play all the time stay resident: footsteps, menus, pickup, flashlight.
	static const int resident[] = { 11000, 11001, 11002, 11003, 11004, 11005, 11006, 11007, 11008, 11009,
	                                SFX_MENU_MOVE, SFX_MENU_OK, SFX_MENU_CANCEL, SFX_ITEM_PICKUP,
	                                SFX_LIGHT_ON, SFX_LIGHT_OFF };
	for (u32 i = 0; i < sizeof resident / sizeof resident[0]; i++)
		sfx_preload(resident[i]);
	R.sync_loads = 0;                      // count the reads after the resident set
	R.sync_max_ms = 0.0f;
	snprintf(s_status, sizeof s_status, "sfx %lu ids, %lu KB resident", (unsigned long)s_nids,
	         (unsigned long)(s_cache_used >> 10));
	return true;
}

void sfx_exit(void)
{
	if (!s_ok)
		return;
	sfx_stop_all();
	if (R.thread) {
		R.quit = true;
		LightEvent_Signal(&R.wake);
		threadJoin(R.thread, U64_MAX);
		threadFree(R.thread);
	}
	if (R.bank)
		fclose(R.bank);
	memset(&R, 0, sizeof R);
	for (u32 i = 0; s_loaded && i < s_nsamples; i++)
		if (s_loaded[i].pcm)
			linearFree(s_loaded[i].pcm);
	free(s_samples);
	free(s_ids);
	free(s_doors);
	free(s_loaded);
	free(s_floor);
	s_samples = NULL, s_ids = NULL, s_doors = NULL, s_loaded = NULL, s_floor = NULL;
	s_nfloor = 0;
	s_cache_used = 0;
	if (s_bank)
		fclose(s_bank);
	s_bank = NULL;
	s_ok = false;
}

bool sfx_preload(int id)
{
	int s = s_ok ? find_sample(id) : -1;
	if (s < 0 || !load_sample(s))
		return false;
	s_loaded[s].pinned = true;
	return true;
}

// ---------------------------------------------------------------- volume and pan

// The PC turns a 0..1 level into a DirectSound 3D distance d = 255 * (1 - level) and places the
// buffer d / 25.5 units from the listener (min distance 1, rolloff 1: gain = 1 / r). Past 250 the
// distance is multiplied by 100 (0x517960), which is as good as silent.
static float level_gain(float level)
{
	float d = 255.0f * (1.0f - level);
	if (d < 1.0f)
		d = 1.0f;
	float r = d / 25.5f;
	float g = r <= 1.0f ? 1.0f : 1.0f / r;
	return d > 250.0f ? g * 0.01f : g;
}

// 0x515230: full volume up to 1500 units, silent at 11500, for both the source and the camera
// measured from the player.
static float distance_factor(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	float t = (sqrtf(dx * dx + dy * dy + dz * dz) - 1500.0f) * 0.0001f;
	return 1.0f - (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
}

// 0x515ab0: the source direction from the camera in view space gives an angle of
// 80 * (1 - forward) degrees toward its side (0 ahead, 80 abeam, 160 behind); DirectSound pans
// by the sine of it.
static float position_pan(const float pos[3])
{
	float fx = s_player[0] - s_eye[0], fz = s_player[2] - s_eye[2];
	float fl = sqrtf(fx * fx + fz * fz);
	if (fl < 1.0f)
		return 0.0f;
	fx /= fl, fz /= fl;
	float rx = pos[0] - s_eye[0], rz = pos[2] - s_eye[2];
	float side = rx * fz - rz * fx;      // screen right is (fz, -fx): turning right raises the yaw
	float ahead = rx * fx + rz * fz;
	float l = sqrtf(side * side + ahead * ahead);
	if (l < 1.0f)
		return 0.0f;
	float deg = 80.0f * (1.0f - ahead / l);
	float p = sinf(deg * (float)M_PI / 180.0f);
	return side < 0.0f ? -p : p;
}

static float s_gain = 1.0f;       // the options' effects volume (sfx_set_gain)

void sfx_set_gain(float g) { s_gain = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g; }

static void voice_mix(Voice* v, float gain, float pan)
{
	float mix[12] = { 0 };
	gain *= s_gain;
	if (pan > 1.0f) pan = 1.0f;
	if (pan < -1.0f) pan = -1.0f;
	// soft balance: the far side keeps 30%, which suits the 3DS's close speakers
	mix[0] = gain * (pan > 0.0f ? 1.0f - 0.7f * pan : 1.0f);
	mix[1] = gain * (pan < 0.0f ? 1.0f + 0.7f * pan : 1.0f);
	ndspChnSetMix(FIRST_CH + (v - s_voices), mix);
}

static void positional_mix(Voice* v)
{
	float level = v->volume;
	if (s_have_listener)
		level *= distance_factor(v->pos, s_player) * distance_factor(s_eye, s_player);
	voice_mix(v, level_gain(level), s_have_listener ? position_pan(v->pos) : 0.0f);
}

// ---------------------------------------------------------------- voices

static void voice_stop(Voice* v)
{
	if (!v->active)
		return;
	ndspChnWaveBufClear(FIRST_CH + (v - s_voices));
	v->active = false;
}

static Voice* voice_alloc(void)
{
	Voice* oldest = NULL;
	for (int i = 0; i < NVOICES; i++) {
		Voice* v = &s_voices[i];
		if (!v->active)
			return v;
		if (!(s_samples[v->sample].flags & 1) && (!oldest || v->started < oldest->started))
			oldest = v;
	}
	if (oldest)
		voice_stop(oldest);
	return oldest;
}

static Voice* start(int id, float volume)
{
	if (!s_ok || id <= 0)
		return NULL;
	int s = find_sample(id);
	if (s < 0)
		return NULL;
	for (int i = 0; i < NVOICES; i++)
		if (s_voices[i].active && s_voices[i].id == id)
			voice_stop(&s_voices[i]);
	finish_load(s);
	bool waiting = s_loaded[s].state == L_QUEUED || request_load(s);
	if (!waiting && !load_sample(s))
		return NULL;
	Voice* v = voice_alloc();
	if (!v)
		return NULL;
	const BankSample* b = &s_samples[s];
	int ch = FIRST_CH + (v - s_voices);
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(ch, (float)b->rate);
	ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
	memset(v, 0, sizeof *v);
	v->active = true;
	v->id = id;
	v->sample = s;
	v->volume = volume;
	v->started = s_tick;
	v->wb.data_vaddr = s_loaded[s].pcm;
	v->wb.nsamples = b->frames;
	v->wb.looping = (b->flags & 1) != 0;
	v->waiting = waiting;
	s_loaded[s].last_use = s_tick;
	return v;
}

int sfx_play(int id, float volume, float pan)
{
	Voice* v = start(id, volume);
	if (!v)
		return -1;
	voice_mix(v, level_gain(volume), pan);
	if (!v->waiting)
		ndspChnWaveBufAdd(FIRST_CH + (v - s_voices), &v->wb);
	return v - s_voices;
}

int sfx_play_at(int id, float volume, const float pos[3])
{
	Voice* v = start(id, volume);
	if (!v)
		return -1;
	v->positional = true;
	memcpy(v->pos, pos, sizeof v->pos);
	positional_mix(v);
	if (!v->waiting)
		ndspChnWaveBufAdd(FIRST_CH + (v - s_voices), &v->wb);
	return v - s_voices;
}

void sfx_stop(int id)
{
	for (int i = 0; i < NVOICES; i++)
		if (s_voices[i].active && s_voices[i].id == id)
			voice_stop(&s_voices[i]);
	if (s_pending_close.id == id)
		s_pending_close.id = 0;
}

void sfx_stop_all(void)
{
	for (int i = 0; i < NVOICES; i++)
		voice_stop(&s_voices[i]);
	s_pending_close.id = 0;
}

bool sfx_playing(int id)
{
	for (int i = 0; i < NVOICES; i++)
		if (s_voices[i].active && s_voices[i].id == id)
			return true;
	return false;
}

void sfx_listener(const float player[3], const float eye[3])
{
	memcpy(s_player, player, sizeof s_player);
	memcpy(s_eye, eye, sizeof s_eye);
	s_have_listener = true;
}

void sfx_update(void)
{
	if (!s_ok)
		return;
	s_tick++;
	for (int i = 0; i < NVOICES; i++) {
		Voice* v = &s_voices[i];
		if (!v->active)
			continue;
		if (v->waiting) {
			finish_load(v->sample);
			Loaded* l = &s_loaded[v->sample];
			if (l->state == L_QUEUED)
				continue;
			v->waiting = false;
			if (!l->pcm) {
				v->active = false;              // the read failed
				continue;
			}
			v->wb.data_vaddr = l->pcm;
			ndspChnWaveBufAdd(FIRST_CH + i, &v->wb);
			continue;
		}
		if (v->wb.status == NDSP_WBUF_DONE) {
			v->active = false;
			continue;
		}
		if (v->positional && v->wb.looping)
			positional_mix(v);
	}
	if (s_pending_close.id && s_tick >= s_pending_close.not_before) {
		int open = sfx_door_id(s_pending_close.set, SFX_DOOR_OPEN);
		if (open)
			sfx_stop(open);
		sfx_play_at(s_pending_close.id, 1.0f, s_pending_close.pos);
		s_pending_close.id = 0;
	}
}

// ---------------------------------------------------------------- doors

int sfx_door_id(int set, SfxDoor which)
{
	if (!s_ok || set <= 0 || (u32)set >= s_ndoors || which < 0 || which > SFX_DOOR_LOCK)
		return 0;
	return s_doors[set].se[which];
}

static u32 s_open_started;

void sfx_door_open(int set, const float pos[3])
{
	s_pending_close.id = 0;
	int id = sfx_door_id(set, SFX_DOOR_OPEN);
	if (id && sfx_play_at(id, 1.0f, pos) >= 0)
		s_open_started = s_tick;
	// the close sound is read now, while the screen fades, not when the new room appears
	int close = find_sample(sfx_door_id(set, SFX_DOOR_CLOSE));
	if (close >= 0)
		load_sample(close);
}

void sfx_door_close(int set, const float pos[3])
{
	int id = sfx_door_id(set, SFX_DOOR_CLOSE);
	if (!id)
		return;
	s_pending_close.id = id;
	s_pending_close.set = set;
	memcpy(s_pending_close.pos, pos, sizeof s_pending_close.pos);
	// at least 0.6 s of the open sound (or all of it, if shorter) before the close cuts it
	u32 min_ticks = 36;
	int open = sfx_door_id(set, SFX_DOOR_OPEN), s = open ? find_sample(open) : -1;
	if (s >= 0 && sfx_playing(open)) {
		u32 len = s_samples[s].frames * 60 / s_samples[s].rate;
		if (len < min_ticks)
			min_ticks = len;
		s_pending_close.not_before = s_open_started + min_ticks;
	} else {
		s_pending_close.not_before = s_tick;
	}
}

void sfx_door_result(int set, SfxDoor which, const float pos[3])
{
	int id = sfx_door_id(set, which);
	if (id)
		sfx_play_at(id, 1.0f, pos);
}

// ---------------------------------------------------------------- footsteps

// Floor tables the tile reader thread (stream.c) brought in with a neighbouring tile, so walking
// onto that tile does not read the SD card on the main loop (50 ms in Azahar, port/PERFORMANCE.md).
#define FLOOR_CACHE 8
static struct {
	char map[16];
	u8* data;
	long size;
	u32 age;
} s_fcache[FLOOR_CACHE];
static u32 s_fage;

void sfx_floor_put(const char* map, u8* data, long size)
{
	int slot = 0;
	for (int i = 0; i < FLOOR_CACHE; i++) {
		if (!strcmp(s_fcache[i].map, map)) {
			slot = i;
			break;
		}
		if (s_fcache[i].age < s_fcache[slot].age)
			slot = i;              // empty slots have age 0
	}
	free(s_fcache[slot].data);
	snprintf(s_fcache[slot].map, sizeof s_fcache[slot].map, "%s", map);
	s_fcache[slot].data = data;
	s_fcache[slot].size = size;
	s_fcache[slot].age = ++s_fage;
}

static void floor_from_mem(const u8* d, long size)
{
	u32 h[2];
	if (size < 8)
		return;
	memcpy(h, d, sizeof h);
	if (memcmp(h, "SFM1", 4) || h[1] >= 65536 || 8 + (long)h[1] * 40 > size)
		return;
	s_floor = malloc(h[1] * 40);
	if (s_floor) {
		memcpy(s_floor, d + 8, h[1] * 40);
		s_nfloor = h[1];
	}
}

void sfx_floor_load(const char* map)
{
	free(s_floor);
	s_floor = NULL;
	s_nfloor = 0;
	if (!s_ok)
		return;
	for (int i = 0; i < FLOOR_CACHE; i++)
		if (s_fcache[i].map[0] && !strcmp(s_fcache[i].map, map)) {
			floor_from_mem(s_fcache[i].data, s_fcache[i].size);   // no data: the tile has none
			return;
		}
	char path[96];
	snprintf(path, sizeof path, PACK_DIR "/sound/floor/%s.sfm", map);
	FILE* f = fopen(path, "rb");
	if (!f)
		return;
	u32 h[2];
	if (fread(h, 4, 2, f) == 2 && !memcmp(h, "SFM1", 4) && h[1] < 65536) {
		s_floor = malloc(h[1] * 40);
		if (s_floor && fread(s_floor, 40, h[1], f) == h[1])
			s_nfloor = h[1];
	}
	fclose(f);
}

// Material of the floor polygon under (x, z) whose height is nearest y (stairs and landings
// overlap in plan).
int sfx_floor_material(float x, float y, float z)
{
	int best = 0;
	float best_d = 300.0f;
	for (u32 i = 0; i < s_nfloor; i++) {
		const float* t = s_floor + i * 10;
		float ax = t[0], az = t[2], bx = t[3], bz = t[5], cx = t[6], cz = t[8];
		float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
		if (fabsf(d) < 1e-3f)
			continue;
		float u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
		float v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
		if (u < -0.01f || v < -0.01f || u + v > 1.02f)
			continue;
		float h = u * t[1] + v * t[4] + (1.0f - u - v) * t[7];
		float dy = fabsf(h - y);
		if (dy < best_d) {
			best_d = dy;
			u32 m;
			memcpy(&m, t + 9, 4);
			best = (int)m;
		}
	}
	return best;
}

// 0x54a9a0: floor material -> footstep SE. Some rooms override a material (room 0x78 uses 18803
// for 0, rooms 0x2a and 0x46 use 19908 for 5, room 0x3e uses 11054 for 8, room 0xb8 and a few
// boxes in rooms 0x92..0xa3 swap in 18807 / 19907); the port has no PC room ids yet.
static int step_id(int material)
{
	switch (material) {
	case 1: case 4: return 11004;
	case 2: return 11002;
	case 3: return 11007;
	case 5: case 11: return 11000;
	case 6: return 11003;
	case 7: return 11005;
	case 8: return 11009;
	case 9: return 11006;
	case 10: return 11008;
	default: return 11001;
	}
}

static bool passed(float f0, float f1, int frame, int first, int last, bool backward)
{
	float fr = (float)frame;
	if (!backward) {
		if (f1 >= f0)
			return fr > f0 && fr <= f1;
		return fr > f0 || (fr >= (float)first && fr <= f1);     // wrapped past the last frame
	}
	if (f1 <= f0)
		return fr < f0 && fr >= f1;
	return fr < f0 || (fr <= (float)last + 1.0f && fr >= f1);
}

// Enhanced Edition "DoubleFootstepFix" (Patches/DoubleFootstepFix.cpp, zlib licence, always on in
// the EE): the PC plays two steps at once when James switches between his walk and run motions just
// as the old one fires a step, because the new motion starts from its first frame and its first
// trigger comes at once (0x54af80; Maria's 0x549365 likewise). Here the run clip strikes 2 frames
// after its start, so walk to run gave the same double step. Strikes inside one clip are at least
// 8 animation frames apart (steps.txt), so a step closer than MIN_STEP_GAP frames to the previous
// one is the double and is dropped. Counted in animation frames, it follows game time.
#define MIN_STEP_GAP 6.0f
static float s_step_gap = MIN_STEP_GAP;

void sfx_footsteps(const char* clip, int first, int last, float f0, float f1, bool backward, const float pos[3])
{
	if (!s_ok)
		return;
	float adv = backward ? f0 - f1 : f1 - f0;
	if (adv < 0.0f)
		adv += (float)(last - first + 1);     // wrapped round the clip
	s_step_gap += adv;
	// walk 0.5, run 0.8 (0x54a9a0, lower-body states up to and above 10)
	float volume = !strcmp(clip, "run") ? 0.8f : 0.5f;
	for (int i = 0; i < s_nsteps; i++) {
		const Step* st = &s_steps[i];
		if (strcmp(st->clip, clip) || !passed(f0, f1, st->frame, first, last, backward))
			continue;
		if (s_step_gap < MIN_STEP_GAP)
			continue;
		s_step_gap = 0.0f;
		sfx_play_at(step_id(sfx_floor_material(pos[0], pos[1], pos[2])), volume, pos);
	}
}

void sfx_footstep(float volume, const float pos[3])
{
	if (s_ok)
		sfx_play_at(step_id(sfx_floor_material(pos[0], pos[1], pos[2])), volume, pos);
}

const char* sfx_status(void)
{
	if (s_ok)
		snprintf(s_status, sizeof s_status, "sfx %lu KB, read async %lu, now %lu (max %.1f ms)",
		         (unsigned long)(s_cache_used >> 10), (unsigned long)R.async_loads, (unsigned long)R.sync_loads,
		         R.sync_max_ms);
	return s_status;
}

void sfx_sync_loads(bool sync) { R.sync = sync; }
