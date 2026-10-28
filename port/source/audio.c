#include "audio.h"
#include "levels.h"
#include "sdsim.h"
#include "profile.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUSIC_CH 0
#define VOICE_CH 1                  // cutscene voices (demo.c); sfx.c uses 8..23
#define NBUF 4
#define BUF_SAMPLES 4096            // per channel per buffer
#define MAX_CH 2
#define MAX_LAYERS 8

// One ADX stream: the whole file for .adx, one interleaved layer for .aix. The PC game drives
// every AIX layer's volume on its own (SeBgmManager), so all layers decode in lockstep and mix.
typedef struct {
	int channels;
	int coef1, coef2;
	bool v3;
	s32 hist1[MAX_CH], hist2[MAX_CH];
	u8* buf;                    // AIX: this layer's bytes of the current chunk
	u32 cap, len, pos;
	float vol, target;          // gain now and the one asked for (ramped per buffer)
} Layer;

typedef struct {
	FILE* f;
	u8* rb;                     // read-ahead (sread_at): file bytes rb_off.. rb_off + rb_len
	u32 rb_off, rb_len, cur;
	bool rb_eof;
	bool aix;
	u32 data_start;             // ADX: first frame
	u32 rate;
	u32 total_samples;
	bool loop;
	u32 loop_start_sample, loop_end_sample;
	u32 sample_pos;             // next sample to decode
	int nlayers;
	Layer layer[MAX_LAYERS];
	u32 aix_next_chunk;         // file offset of the next AIXP chunk
	u32 aix_loop_offset;        // re-start offset for looping (first data chunk)
	LevelLimiter lim;           // the peaks the levels lift over full scale (levels.h)
} Stream;

// One ndsp channel streaming one ADX / AIX file: music on channel 0, cutscene voices on 1.
typedef struct {
	Stream s;
	int ch;
	bool playing;
	bool primed;                // its buffers have been queued once (an empty queue after is a gap)
	float volume;
	u64 played;                 // samples of the buffers the DSP has finished
	u32 seq;                    // odd while the refill thread moves a finished buffer into played
	ndspWaveBuf wbuf[NBUF];
	s16* pcm;                   // NBUF buffers, linear memory
	char path[128];             // the stream's file and its first layer's gain, to open it again
	float layer0;
} Chan;

static Chan s_music = { .ch = MUSIC_CH }, s_voice = { .ch = VOICE_CH };
static float s_gain[2] = { 1.0f, 1.0f };   // the options' music and voice volumes (audio_set_gains)
static bool s_ok;
static char s_status[64] = "audio off";
static volatile u32 s_underruns;     // times a stream's buffers had all played before the refill

// Decoding one buffer of layered AIX music costs 25-30 ms of CPU, which on the main loop dropped a
// frame every 110 ms (port/PERFORMANCE.md). A thread does the refills instead: on core 2, the New
// 3DS's spare core, or below the main thread on core 0 when the kernel refuses that, where it runs
// while the main thread waits for the vertical blank. Starting and stopping a stream are requests
// the thread carries out too (opening a file and reading its first AIX chunk cost 20..130 ms on the
// main loop, and waiting for the thread's lock up to a buffer's decoding), so the main thread only
// takes the small request lock. Volumes and positions are single words both sides read or write.
static Thread s_thread;
static LightLock s_lock;             // the refill thread's: streams and channels
static LightLock s_req_lock;         // requests from the main thread
static LightEvent s_wake;
static volatile bool s_quit;
static int s_core = -1;
static volatile u64 s_load_ticks, s_load_audio;   // music decoding cost (audio_decode_load)

enum { REQ_NONE, REQ_STOP, REQ_PLAY };
typedef struct {
	int op;
	char path[128];
	float volume, layer0;
} Req;
static Req s_req[2];                 // music, voice

// A stream that can't be opened or read is tried again a few times before it counts as missing: on a
// console a file open or read can fail while other threads hold the SD card (a room or a scene
// loading), which used to leave the music silent for the rest of the stage without a word. Azahar
// never fails them. The refill thread's own; pending_play() counts a retry as still starting.
#define RETRIES 4
#define RETRY_TICKS (SYSCLOCK_ARM11 / 4)
typedef struct {
	Req req;
	int fails;                       // failed tries of req since it last played
	bool waiting;                    // another try is due at tick "at"
	u64 at;
} Retry;
static Retry s_retry[2];
// A play request the refill thread has taken and is still opening: without it the stream looked
// stopped for as long as the file took to open (tens of ms on a console's SD card, none in Azahar),
// and demo.c ended a voice track that has no subtitles (Laura at the piano) before it began.
static volatile bool s_starting[2];
static char s_problem[64];           // audio_problem(): what the player should hear and does not

static void chan_update(Chan* c);
static void chan_stop(Chan* c);
static bool play_unlocked(Chan* c, const char* path, float volume, float layer0);

static void retry_after_failure(int i, const Req* r)
{
	Retry* t = &s_retry[i];
	if (strcmp(t->req.path, r->path))
		t->fails = 0;
	t->req = *r;
	t->req.op = REQ_PLAY;
	t->waiting = ++t->fails < RETRIES;
	t->at = svcGetSystemTick() + RETRY_TICKS;
	if (!t->waiting && i == 0) {     // the music: tell the player (touchui.c)
		const char* name = strrchr(r->path, '/');
		snprintf(s_problem, sizeof s_problem, "Music missing: %.48s", name ? name + 1 : r->path);
	}
}

// The requests, the retries and the refills: on the refill thread, or on the main loop without one.
static void service(void)
{
	for (int i = 0; i < 2; i++) {
		Req r;
		LightLock_Lock(&s_req_lock);
		r = s_req[i];
		if (r.op != REQ_NONE)
			s_retry[i].waiting = false;  // a new request replaces the one being retried
		else if (s_retry[i].waiting && svcGetSystemTick() >= s_retry[i].at)
			r = s_retry[i].req;
		s_starting[i] = r.op == REQ_PLAY;
		__dmb();                         // seen before the request goes (pending_play reads both unlocked)
		s_req[i].op = REQ_NONE;
		LightLock_Unlock(&s_req_lock);
		Chan* c = i ? &s_voice : &s_music;
		if (r.op == REQ_STOP) {
			chan_stop(c);
		} else if (r.op == REQ_PLAY) {
			if (play_unlocked(c, r.path, r.volume, r.layer0)) {
				s_retry[i].waiting = false;
			} else {
				chan_stop(c);
				retry_after_failure(i, &r);
			}
			s_starting[i] = false;
		}
	}
	chan_update(&s_music);
	chan_update(&s_voice);
}

static void refill_thread(void* arg)
{
	(void)arg;
	for (;;) {
		// woken by the main loop each frame, and every 20 ms on its own: a frame that takes longer
		// than the four buffers last (a scene or an area loading, seconds on a console's SD card)
		// used to leave the DSP with nothing to play until it ended
		LightEvent_WaitTimeout(&s_wake, 20 * 1000000LL);
		if (s_quit)
			break;
		LightLock_Lock(&s_lock);
		service();
		LightLock_Unlock(&s_lock);
	}
}

static void request(Chan* c, int op, const char* path, float volume, float layer0)
{
	Req* r = &s_req[c == &s_voice];
	LightLock_Lock(&s_req_lock);
	r->op = op;
	if (path)
		snprintf(r->path, sizeof r->path, "%s", path);
	r->volume = volume;
	r->layer0 = layer0;
	LightLock_Unlock(&s_req_lock);
	if (s_thread)
		LightEvent_Signal(&s_wake);
}

static bool pending_play(const Chan* c)
{
	int i = c == &s_voice;
	return s_req[i].op == REQ_PLAY || s_starting[i] || s_retry[i].waiting;
}

static void thread_start(void)
{
	LightLock_Init(&s_lock);
	LightLock_Init(&s_req_lock);
	memset(s_req, 0, sizeof s_req);
	LightEvent_Init(&s_wake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_quit = false;
	// core 2 on a New 3DS, core 0 on an Old 3DS (profile_music_core says why)
	for (int core = profile_music_core(); !s_thread; core = -2) {
		s_thread = threadCreate(refill_thread, NULL, 16 * 1024, prio + 1, core, false);
		s_core = core;
		if (core == -2)
			break;
	}
}

static void thread_stop(void)
{
	if (!s_thread)
		return;
	s_quit = true;
	LightEvent_Signal(&s_wake);
	threadJoin(s_thread, U64_MAX);
	threadFree(s_thread);
	s_thread = NULL;
}

static u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static ndspOutputMode s_output = NDSP_OUTPUT_STEREO;

// The Options' "Speaker Config.": 0 Mono, 1 Stereo, 2 Surround (the 3DS's virtual surround).
void audio_set_output(int mode)
{
	s_output = mode == 0 ? NDSP_OUTPUT_MONO : mode == 2 ? NDSP_OUTPUT_SURROUND : NDSP_OUTPUT_STEREO;
	if (s_ok)
		ndspSetOutputMode(s_output);
}

bool audio_init(void)
{
	if (R_FAILED(ndspInit())) {
		snprintf(s_status, sizeof s_status, "ndsp init failed (dspfirm.cdc?)");
		snprintf(s_problem, sizeof s_problem, "No sound: dspfirm.cdc missing");
		return false;
	}
	ndspSetOutputMode(s_output);
	s_music.pcm = linearAlloc(NBUF * BUF_SAMPLES * MAX_CH * sizeof(s16));
	s_voice.pcm = linearAlloc(NBUF * BUF_SAMPLES * MAX_CH * sizeof(s16));
	if (!s_music.pcm || !s_voice.pcm) {
		linearFree(s_music.pcm);
		linearFree(s_voice.pcm);
		s_music.pcm = s_voice.pcm = NULL;
		ndspExit();
		snprintf(s_status, sizeof s_status, "no linear memory for audio");
		snprintf(s_problem, sizeof s_problem, "No music: out of memory");
		return false;
	}
	s_ok = true;
	thread_start();
	snprintf(s_status, sizeof s_status, s_thread ? "audio ready, core %d" : "audio ready", s_core);
	return true;
}

static void chan_stop(Chan* c);

void audio_exit(void)
{
	thread_stop();
	chan_stop(&s_music);
	chan_stop(&s_voice);
	for (int i = 0; i < MAX_LAYERS; i++) {
		free(s_music.s.layer[i].buf);
		s_music.s.layer[i].buf = NULL;
		s_music.s.layer[i].cap = 0;
		free(s_voice.s.layer[i].buf);
		s_voice.s.layer[i].buf = NULL;
	}
	if (s_ok) {
		ndspExit();
		linearFree(s_music.pcm);
		linearFree(s_voice.pcm);
	}
	s_ok = false;
}

static void adx_coefs(Layer* l, u32 rate, u32 highpass)
{
	double z = cos(2.0 * M_PI * highpass / rate);
	double a = M_SQRT2 - z, b = M_SQRT2 - 1.0;
	double c = (a - sqrt((a + b) * (a - b))) / b;
	l->coef1 = (int)(c * 8192.0);
	l->coef2 = (int)(c * c * -4096.0);
}

// Parses an ADX header held in memory at h (len bytes) into layer l. The stream-wide fields
// (rate, length, loop, data start relative to h) come from the first layer.
static bool adx_header(Stream* s, Layer* l, const u8* h, u32 len)
{
	if (len < 0x20 || be16(h) != 0x8000)
		return false;
	u32 copyright_ofs = be16(h + 2);
	if (h[4] != 3 || h[5] != 18 || h[6] != 4)
		return false;                              // only standard 4-bit ADX
	l->channels = h[7];
	u32 rate = be32(h + 8);
	u32 highpass = be16(h + 0x10);
	u8 version = h[0x12];
	l->v3 = version == 3;
	if (l->channels < 1 || l->channels > MAX_CH || !rate)
		return false;
	adx_coefs(l, rate, highpass);
	if (l != &s->layer[0])
		return true;
	s->rate = rate;
	s->total_samples = be32(h + 0xC);
	s->data_start = copyright_ofs + 4;
	s->loop = false;
	if (version == 3 && copyright_ofs >= 0x2C - 4 && len >= 0x2C) {
		s->loop = be32(h + 0x18) != 0;
		s->loop_start_sample = be32(h + 0x1C);
		s->loop_end_sample = be32(h + 0x24);
	} else if (version == 4 && copyright_ofs >= 0x38 - 4 && len >= 0x38) {
		s->loop = be32(h + 0x24) != 0;
		s->loop_start_sample = be32(h + 0x28);
		s->loop_end_sample = be32(h + 0x30);
	}
	return true;
}

// The streams read through a read-ahead of their own, topped up here on the refill thread whenever
// less than half of it is left. On a console every thread's file reads go through the FS service one
// request at a time, so while the main loop or the tile reader read a large file the music's and the
// voice's small reads used to wait for it, and the four PCM buffers (about 0.4 s) ran dry: cutscene
// voices stuttered on hardware, never in Azahar, where reads take no time. With up to 192 KB held
// (seconds of a voice, about a second of layered music) a long read elsewhere goes unheard.
#define RB_SIZE (192 * 1024)

static void rb_fill(Stream* s)
{
	u32 want = RB_SIZE - s->rb_len;
	if (!want || s->rb_eof)
		return;
	u32 got = 0;
	if (fseek(s->f, s->rb_off + s->rb_len, SEEK_SET) == 0)
		got = fread(s->rb + s->rb_len, 1, want, s->f);
	sdsim_read(got);
	s->rb_len += got;
	if (got < want)
		s->rb_eof = true;
}

// Up to n bytes from file offset off into dst; the count read (short at the end of the file).
static u32 sread_at(Stream* s, u32 off, void* dst, u32 n)
{
	if (!s->rb)
		s->rb = malloc(RB_SIZE);
	if (!s->rb || n > RB_SIZE / 2 || sdsim_legacy) {   // no read-ahead: straight from the file
		s->cur = off;
		if (fseek(s->f, off, SEEK_SET))
			return 0;
		u32 got = fread(dst, 1, n, s->f);
		sdsim_read(got);
		s->cur += got;
		return got;
	}
	u32 end = s->rb_off + s->rb_len;
	if (off < s->rb_off || off + n > end) {
		// a seek, or the data runs on past what is held: keep what is held from off on, read the rest
		u32 keep = off >= s->rb_off && off < end ? end - off : 0;
		if (keep)
			memmove(s->rb, s->rb + (off - s->rb_off), keep);
		else
			s->rb_eof = false;
		s->rb_off = off;
		s->rb_len = keep;
		rb_fill(s);
		end = s->rb_off + s->rb_len;
	}
	u32 have = off + n <= end ? n : end > off ? end - off : 0;
	memcpy(dst, s->rb + (off - s->rb_off), have);
	s->cur = off + have;
	if (!s->rb_eof && end - s->cur < RB_SIZE / 2) {   // read ahead while there is time
		u32 keep = end - s->cur;
		memmove(s->rb, s->rb + (s->cur - s->rb_off), keep);
		s->rb_off = s->cur;
		s->rb_len = keep;
		rb_fill(s);
	}
	return have;
}

static u32 sread(Stream* s, void* dst, u32 n) { return sread_at(s, s->cur, dst, n); }

// AIX: "AIXF" header, then "AIXP" chunks: u32 BE size, u8 layer, u8 layer count, u16 BE frames, then
// layer data. Each block has one chunk per layer in order; a layer's first chunk starts with its
// ADX header. Reads the next block into every layer's buffer. first: parse the headers.
static bool aix_fill(Stream* s, bool first)
{
	int got = 0;
	while (got < s->nlayers || first) {
		u8 hd[16];
		if (sread_at(s, s->aix_next_chunk, hd, 16) != 16)
			return false;
		if (memcmp(hd, "AIXP", 4) != 0)
			return false;                          // "AIXE" ends the stream
		u32 size = be32(hd + 4);
		int li = hd[8];
		if (first) {
			s->nlayers = hd[9] < MAX_LAYERS ? hd[9] : MAX_LAYERS;
			first = false;
		}
		s->aix_next_chunk += 8 + size;
		if (li >= s->nlayers || size < 8)
			continue;
		Layer* l = &s->layer[li];
		u32 payload = size - 8;
		if (payload > l->cap) {
			u8* nb = realloc(l->buf, payload);     // grows a few times per stream, then reused
			if (!nb)
				return false;
			l->buf = nb;
			l->cap = payload;
		}
		if (sread(s, l->buf, payload) != payload)
			return false;
		l->len = payload;
		l->pos = 0;
		if (!l->channels) {
			if (!adx_header(s, l, l->buf, payload))
				return false;
			l->pos = be16(l->buf + 2) + 4;         // this layer's own copyright offset
		}
		got++;
	}
	return true;
}

// Reads one frame group (18 bytes per channel) of every layer. Returns false at end of data.
static bool read_frame_group(Stream* s, u8 out[MAX_LAYERS][18 * MAX_CH])
{
	if (!s->aix) {
		u32 need = 18 * s->layer[0].channels;
		u32 frame = s->sample_pos / 32;
		return sread_at(s, s->data_start + frame * need, out[0], need) == need;
	}
	while (s->layer[0].pos + 18 * s->layer[0].channels > s->layer[0].len)
		if (!aix_fill(s, false))
			return false;
	for (int i = 0; i < s->nlayers; i++) {
		Layer* l = &s->layer[i];
		u32 need = 18 * l->channels;
		if (l->pos + need > l->len) {
			memset(out[i], 0, need);              // a short layer chunk: silence, keep going
			continue;
		}
		memcpy(out[i], l->buf + l->pos, need);
		l->pos += need;
	}
	return true;
}

static void restart(Stream* s, u32 sample)
{
	s->sample_pos = sample - sample % 32;
	if (s->aix) {
		// AIX loops restart every layer from the first data chunk (whole-file loop)
		s->aix_next_chunk = s->aix_loop_offset;
		for (int i = 0; i < s->nlayers; i++)
			s->layer[i].len = s->layer[i].pos = 0;
		s->sample_pos = 0;
	}
	for (int i = 0; i < s->nlayers; i++) {
		memset(s->layer[i].hist1, 0, sizeof s->layer[i].hist1);
		memset(s->layer[i].hist2, 0, sizeof s->layer[i].hist2);
	}
}

// Decodes 32 samples of one layer's frame group and adds them, times gain (16.16), into acc.
// Two nibbles per byte and the version test outside the loop: this runs for every sample of every
// layer, which is most of the music thread's time on an Old 3DS.
static inline s32 adx_clamp(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

static void decode_layer(Layer* l, const u8* fr, s32 acc[32][MAX_CH], s32 gain)
{
	const s32 c1 = l->coef1, c2 = l->coef2;
	const bool v3 = l->v3, mono = l->channels == 1;
	for (int c = 0; c < l->channels; c++) {
		const u8* f = fr + 18 * c;
		const s32 scale = (s16)be16(f) + 1;
		s32 h1 = l->hist1[c], h2 = l->hist2[c];
		for (int i = 0; i < 32; i += 2) {
			s32 b = f[2 + i / 2];
			s32 nib[2] = { (s32)((u32)b << 24) >> 28, (s32)((u32)b << 28) >> 28 };
			for (int k = 0; k < 2; k++) {
				s32 v = v3 ? nib[k] * scale + ((c1 * h1) >> 12) + ((c2 * h2) >> 12)
				           : nib[k] * scale + ((c1 * h1 + c2 * h2) >> 12);
				v = adx_clamp(v);
				h2 = h1;
				h1 = v;
				s32 g = (s32)(((s64)v * gain) >> 16);
				acc[i + k][c] += g;
				if (mono)
					acc[i + k][1] += g;
			}
		}
		l->hist1[c] = h1;
		l->hist2[c] = h2;
	}
}

// Decodes up to n samples per channel into interleaved PCM. Returns samples produced.
static u32 decode(Stream* s, s16* out, u32 n, float volume)
{
	u32 done = 0, groups = n / 32;
	u8 fr[MAX_LAYERS][18 * MAX_CH];
	float from[MAX_LAYERS];
	for (int i = 0; i < s->nlayers; i++)
		from[i] = s->layer[i].vol;
	for (u32 g = 0; done + 32 <= n; g++) {
		u32 end = s->loop ? s->loop_end_sample : s->total_samples;
		if (end && s->sample_pos >= end) {
			if (!s->loop && !s->aix)
				break;
			restart(s, s->loop ? s->loop_start_sample : 0);
		}
		if (!read_frame_group(s, fr)) {
			if (s->aix || s->loop) {
				restart(s, s->loop ? s->loop_start_sample : 0);
				if (!read_frame_group(s, fr))
					break;
			} else {
				break;
			}
		}
		s32 acc[32][MAX_CH] = { { 0 } };
		float t = (float)(g + 1) / (float)groups;
		for (int i = 0; i < s->nlayers; i++) {
			Layer* l = &s->layer[i];
			float vol = from[i] + (l->target - from[i]) * t;    // ramp across the buffer
			s32 gain = (s32)(vol * volume * 65536.0f);
			if (gain == 0 && from[i] == 0.0f && l->target == 0.0f) {
				// A layer faded out for the whole buffer adds nothing: skip its decoding (most of the
				// cost with five-layer AIX music on an Old 3DS). Its predictor restarts from zero
				// when it fades back in, an error that dies out within a few dozen samples while
				// the fade is still near silence.
				l->hist1[0] = l->hist1[1] = l->hist2[0] = l->hist2[1] = 0;
				continue;
			}
			decode_layer(l, fr[i], acc, gain);
		}
		levels_limiter(&s->lim, &acc[0][0], 32, s->rate);
		for (int i = 0; i < 32; i++)
			for (int c = 0; c < MAX_CH; c++)
				out[(done + i) * MAX_CH + c] = levels_limit(acc[i][c]);
		s->sample_pos += 32;
		done += 32;
	}
	for (int i = 0; i < s->nlayers; i++)
		s->layer[i].vol = s->layer[i].target;
	return done;
}

static void chan_stop(Chan* c)
{
	if (s_ok)
		ndspChnReset(c->ch);
	if (c->s.f)
		fclose(c->s.f);
	Layer keep[MAX_LAYERS];
	memcpy(keep, c->s.layer, sizeof keep);
	u8* rb = c->s.rb;                                 // so is the read-ahead
	memset(&c->s, 0, sizeof c->s);
	c->s.rb = rb;
	__atomic_fetch_add(&c->seq, 1, __ATOMIC_SEQ_CST);
	c->played = 0;
	__atomic_fetch_add(&c->seq, 1, __ATOMIC_SEQ_CST);
	c->primed = false;
	for (int i = 0; i < MAX_LAYERS; i++) {          // chunk buffers are reused by the next stream
		c->s.layer[i].buf = keep[i].buf;
		c->s.layer[i].cap = keep[i].cap;
	}
	c->playing = false;
}

static void chan_stop_locked(Chan* c)
{
	if (s_ok)
		request(c, REQ_STOP, NULL, 0.0f, 0.0f);
	else
		chan_stop(c);
}

void audio_stop_music(void) { chan_stop_locked(&s_music); }

void audio_set_gains(float music, float voice)
{
	s_gain[0] = music, s_gain[1] = voice;
	for (int i = 0; i < 2; i++) {
		float mix[12] = { s_gain[i], s_gain[i] };
		ndspChnSetMix(i ? VOICE_CH : MUSIC_CH, mix);
	}
}
void audio_stop_voice(void) { chan_stop_locked(&s_voice); }

static bool play_unlocked(Chan* c, const char* path, float volume, float layer0)
{
	chan_stop(c);
	if (!s_ok)
		return false;
	Stream* s = &c->s;
	sdsim_open();
	s->f = fopen(path, "rb");
	if (!s->f) {
		snprintf(s_status, sizeof s_status, "missing %.55s", path);
		return false;
	}
	u8 head[0x40];
	if (sread_at(s, 0, head, sizeof head) < 0x20) {
		chan_stop(c);
		return false;
	}
	if (!memcmp(head, "AIXF", 4)) {
		s->aix = true;
		s->aix_next_chunk = 8 + be32(head + 4);    // skip the AIXF header block
		if (!aix_fill(s, true)) {
			chan_stop(c);
			snprintf(s_status, sizeof s_status, "bad AIX");
			return false;
		}
		s->aix_loop_offset = s->aix_next_chunk;    // loop restarts after the header block
	} else {
		s->nlayers = 1;
		if (!adx_header(s, &s->layer[0], head, sizeof head)) {
			chan_stop(c);
			snprintf(s_status, sizeof s_status, "unsupported ADX");
			return false;
		}
	}
	s->layer[0].vol = s->layer[0].target = layer0;
	c->volume = volume;
	ndspChnReset(c->ch);
	ndspChnSetInterp(c->ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(c->ch, (float)s->rate);
	ndspChnSetFormat(c->ch, NDSP_FORMAT_STEREO_PCM16);
	float g = s_gain[c == &s_voice];
	float mix[12] = { g, g };
	ndspChnSetMix(c->ch, mix);
	for (int i = 0; i < NBUF; i++) {
		memset(&c->wbuf[i], 0, sizeof c->wbuf[i]);
		c->wbuf[i].data_vaddr = c->pcm + i * BUF_SAMPLES * MAX_CH;
		c->wbuf[i].status = NDSP_WBUF_DONE;
	}
	c->playing = true;
	if (c == &s_music) {
		const char* name = strrchr(path, '/');
		snprintf(s_status, sizeof s_status, "%.24s %luHz %dch%s %d", name ? name + 1 : path, (unsigned long)s->rate,
		         s->layer[0].channels, s->aix ? " aix" : "", s->nlayers);
	}
	snprintf(c->path, sizeof c->path, "%s", path);
	c->layer0 = layer0;
	return true;
}

// This only asks for the stream (service() opens it, on the refill thread when there is one) and
// answers true; a file that turns out to be missing or unreadable is tried a few more times, then
// stays silent (audio_voice_playing turns false; audio_problem names a music file).
static bool play(Chan* c, const char* path, float volume, float layer0)
{
	if (!s_ok)
		return false;
	request(c, REQ_PLAY, path, volume, layer0);
	return true;
}

// Layer 0 plays alone until the caller mixes in others.
bool audio_play_music(const char* path, float volume) { return play(&s_music, path, volume, 1.0f); }

// Every layer starts silent (the first buffers are decoded before the caller can set volumes).
bool audio_play_layers(const char* path, float volume) { return play(&s_music, path, volume, 0.0f); }

// Cutscene voices: no loop (the voice ADX files have no loop points), one at a time. Each file is
// brought to the dialogue's loudness (levels.h); a pack with no measurement plays it 2 dB up.
bool audio_play_voice(const char* path, float volume)
{
	char dir[96];
	const char* slash = strrchr(path, '/');
	snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 0, path);
	float g = levels_gain(dir, slash ? slash + 1 : path, -1, LEVEL_VOICE, LEVEL_PEAK_VOICE, 1.26f);
	return play(&s_voice, path, volume * g, 1.0f);
}

// Seconds of the voice heard so far, < 0 before the DSP has started on it (demo.c holds the scene
// clock until then, as DramaDemoMain waits for the stream).
float audio_voice_position(void)
{
	Chan* c = &s_voice;
	if (pending_play(c))
		return -1.0f;                    // the refill thread has not started it yet
	if (!c->s.rate)
		return -1.0f;
	// The DSP's sample position counts from the start of the buffer it plays, and played grows only
	// when the refill thread next runs (up to 20 ms later), so a buffer the DSP has just finished is
	// counted here until then. Without it the clock fell back a whole buffer at every buffer change
	// (4096 samples, 186 ms of a 22 kHz voice) and the scenes that follow it jumped back and forth.
	// seq makes the read consistent with the refill thread's update on the other core.
	for (int tries = 0; tries < 16; tries++) {
		u32 seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
		if (seq & 1)
			continue;
		u64 done = c->played;
		for (int i = 0; i < NBUF; i++)
			if (c->wbuf[i].status == NDSP_WBUF_DONE)
				done += c->wbuf[i].nsamples;
		bool playing = ndspChnIsPlaying(c->ch);
		u32 pos = playing ? ndspChnGetSamplePos(c->ch) : 0;
		if (__atomic_load_n(&c->seq, __ATOMIC_SEQ_CST) != seq)
			continue;
		if (!playing && !done)
			return -1.0f;
		return (float)(done + pos) / (float)c->s.rate;
	}
	return -1.0f;
}

// Still sounding: decoding, or buffers the DSP has not finished yet.
bool audio_voice_playing(void)
{
	if (pending_play(&s_voice))
		return true;
	if (s_voice.playing)
		return true;
	for (int i = 0; i < NBUF; i++)
		if (s_voice.wbuf[i].status == NDSP_WBUF_QUEUED || s_voice.wbuf[i].status == NDSP_WBUF_PLAYING)
			return true;
	return false;
}

void audio_set_layer_volume(int layer, float volume)
{
	if (layer >= 0 && layer < s_music.s.nlayers)
		s_music.s.layer[layer].target = volume < 0.0f ? 0.0f : volume;
}

int audio_layer_count(void) { return s_music.playing && !pending_play(&s_music) ? s_music.s.nlayers : 0; }

static void chan_update(Chan* c)
{
	if (!c->playing)
		return;
	int empty = 0;
	for (int i = 0; i < NBUF; i++)
		empty += c->wbuf[i].status == NDSP_WBUF_DONE || c->wbuf[i].status == NDSP_WBUF_FREE;
	if (c->primed && empty == NBUF)
		s_underruns++;              // the DSP ran out: a gap the player hears
	c->primed = true;
	for (int i = 0; i < NBUF; i++) {
		ndspWaveBuf* w = &c->wbuf[i];
		if (w->status != NDSP_WBUF_DONE && w->status != NDSP_WBUF_FREE)
			continue;
		__atomic_fetch_add(&c->seq, 1, __ATOMIC_SEQ_CST);
		c->played += w->nsamples;   // finished (0 for a buffer that never played)
		w->nsamples = 0;
		__atomic_fetch_add(&c->seq, 1, __ATOMIC_SEQ_CST);
		s16* pcm = (s16*)w->data_vaddr;
		u64 t0 = svcGetSystemTick();
		u32 n = decode(&c->s, pcm, BUF_SAMPLES, c->volume);
		if (c == &s_music && c->s.rate) {
			// share of one core spent decoding music: decode time over the time the samples last
			s_load_ticks += svcGetSystemTick() - t0;
			s_load_audio += (u64)n * SYSCLOCK_ARM11 / c->s.rate;
		}
		if (!n) {
			c->playing = false;
			// A looping stream only runs dry when its file stops reading: open it again (from its
			// start) rather than leave the music off until the next page.
			if (c == &s_music && (c->s.loop || c->s.aix)) {
				Req r = { REQ_PLAY, "", c->volume, c->layer0 };
				memcpy(r.path, c->path, sizeof r.path);
				snprintf(s_status, sizeof s_status, "read error, again");
				retry_after_failure(0, &r);
			}
			return;
		}
		if (c == &s_music && s_retry[0].fails) {
			s_retry[0].fails = 0;            // it plays: earlier failures are forgotten
			s_problem[0] = '\0';
		}
		w->nsamples = n;
		DSP_FlushDataCache(pcm, n * MAX_CH * sizeof(s16));
		ndspChnWaveBufAdd(c->ch, w);
	}
}

void audio_update(void)
{
	if (s_thread) {
		LightEvent_Signal(&s_wake);     // the refill thread does it
		return;
	}
	if (s_ok)
		service();
}

const char* audio_status(void) { return s_status; }
const char* audio_problem(void) { return s_problem[0] ? s_problem : NULL; }
u32 audio_underruns(void) { return s_underruns; }

int audio_decode_load(void)
{
	u64 t = s_load_ticks, a = s_load_audio;
	s_load_ticks = s_load_audio = 0;
	return a ? (int)(t * 100 / a) : -1;
}
