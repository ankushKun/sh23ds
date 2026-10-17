#include "audio.h"
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
} Stream;

// One ndsp channel streaming one ADX / AIX file: music on channel 0, cutscene voices on 1.
typedef struct {
	Stream s;
	int ch;
	bool playing;
	float volume;
	u64 played;                 // samples of the buffers the DSP has finished
	ndspWaveBuf wbuf[NBUF];
	s16* pcm;                   // NBUF buffers, linear memory
} Chan;

static Chan s_music = { .ch = MUSIC_CH }, s_voice = { .ch = VOICE_CH };
static float s_gain[2] = { 1.0f, 1.0f };   // the options' music and voice volumes (audio_set_gains)
static bool s_ok;
static char s_status[64] = "audio off";

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

enum { REQ_NONE, REQ_STOP, REQ_PLAY };
typedef struct {
	int op;
	char path[128];
	float volume, layer0;
} Req;
static Req s_req[2];                 // music, voice

static void chan_update(Chan* c);
static void chan_stop(Chan* c);
static bool play_unlocked(Chan* c, const char* path, float volume, float layer0);

static void refill_thread(void* arg)
{
	(void)arg;
	for (;;) {
		LightEvent_Wait(&s_wake);
		if (s_quit)
			break;
		LightLock_Lock(&s_lock);
		for (int i = 0; i < 2; i++) {
			Req r;
			LightLock_Lock(&s_req_lock);
			r = s_req[i];
			s_req[i].op = REQ_NONE;
			LightLock_Unlock(&s_req_lock);
			Chan* c = i ? &s_voice : &s_music;
			if (r.op == REQ_STOP)
				chan_stop(c);
			else if (r.op == REQ_PLAY && !play_unlocked(c, r.path, r.volume, r.layer0))
				chan_stop(c);
		}
		chan_update(&s_music);
		chan_update(&s_voice);
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
	LightEvent_Signal(&s_wake);
}

static bool pending_play(const Chan* c) { return s_req[c == &s_voice].op == REQ_PLAY; }

static void thread_start(void)
{
	LightLock_Init(&s_lock);
	LightLock_Init(&s_req_lock);
	memset(s_req, 0, sizeof s_req);
	LightEvent_Init(&s_wake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_quit = false;
	for (int core = 2; core >= -2 && !s_thread; core -= 4) {
		s_thread = threadCreate(refill_thread, NULL, 16 * 1024, prio + 1, core, false);
		s_core = core;
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

bool audio_init(void)
{
	if (R_FAILED(ndspInit())) {
		snprintf(s_status, sizeof s_status, "ndsp init failed (dspfirm.cdc?)");
		return false;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	s_music.pcm = linearAlloc(NBUF * BUF_SAMPLES * MAX_CH * sizeof(s16));
	s_voice.pcm = linearAlloc(NBUF * BUF_SAMPLES * MAX_CH * sizeof(s16));
	if (!s_music.pcm || !s_voice.pcm) {
		linearFree(s_music.pcm);
		linearFree(s_voice.pcm);
		ndspExit();
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
	audio_stop_music();
	audio_stop_voice();
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

// AIX: "AIXF" header, then "AIXP" chunks: u32 BE size, u8 layer, u8 layer count, u16 BE frames, then
// layer data. Each block has one chunk per layer in order; a layer's first chunk starts with its
// ADX header. Reads the next block into every layer's buffer. first: parse the headers.
static bool aix_fill(Stream* s, bool first)
{
	int got = 0;
	while (got < s->nlayers || first) {
		u8 hd[16];
		if (fseek(s->f, s->aix_next_chunk, SEEK_SET) || fread(hd, 1, 16, s->f) != 16)
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
		if (fread(l->buf, 1, payload, s->f) != payload)
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
		if (fseek(s->f, s->data_start + frame * need, SEEK_SET))
			return false;
		return fread(out[0], 1, need, s->f) == need;
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
static void decode_layer(Layer* l, const u8* fr, s32 acc[32][MAX_CH], s32 gain)
{
	for (int c = 0; c < l->channels; c++) {
		const u8* f = fr + 18 * c;
		int scale = (s16)be16(f) + 1;
		s32 h1 = l->hist1[c], h2 = l->hist2[c];
		for (int i = 0; i < 32; i++) {
			int nib = (i & 1) ? (f[2 + i / 2] & 0xF) : (f[2 + i / 2] >> 4);
			if (nib & 8)
				nib -= 16;
			s32 v = l->v3 ? nib * scale + ((l->coef1 * h1) >> 12) + ((l->coef2 * h2) >> 12)
			              : nib * scale + ((l->coef1 * h1 + l->coef2 * h2) >> 12);
			if (v > 32767) v = 32767;
			if (v < -32768) v = -32768;
			h2 = h1;
			h1 = v;
			s32 g = (s32)(((s64)v * gain) >> 16);
			acc[i][c] += g;
			if (l->channels == 1)
				acc[i][1] += g;
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
			decode_layer(l, fr[i], acc, (s32)(vol * volume * 65536.0f));
		}
		for (int i = 0; i < 32; i++)
			for (int c = 0; c < MAX_CH; c++) {
				s32 v = acc[i][c];
				out[(done + i) * MAX_CH + c] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			}
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
	memset(&c->s, 0, sizeof c->s);
	c->played = 0;
	for (int i = 0; i < MAX_LAYERS; i++) {          // chunk buffers are reused by the next stream
		c->s.layer[i].buf = keep[i].buf;
		c->s.layer[i].cap = keep[i].cap;
	}
	c->playing = false;
}

static void chan_stop_locked(Chan* c)
{
	if (s_thread)
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
	s->f = fopen(path, "rb");
	if (!s->f) {
		snprintf(s_status, sizeof s_status, "missing %s", path);
		return false;
	}
	u8 head[0x40];
	if (fread(head, 1, sizeof head, s->f) < 0x20) {
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
		snprintf(s_status, sizeof s_status, "%s %luHz %dch%s %d", name ? name + 1 : path, (unsigned long)s->rate,
		         s->layer[0].channels, s->aix ? " aix" : "", s->nlayers);
	}
	if (!s_thread)
		chan_update(c);                  // with the refill thread, it decodes the first buffers
	return true;
}

// With the refill thread this only asks for the stream and answers true; a file that turns out to
// be missing or unreadable then simply stays silent (audio_voice_playing turns false).
static bool play(Chan* c, const char* path, float volume, float layer0)
{
	if (s_thread) {
		request(c, REQ_PLAY, path, volume, layer0);
		return true;
	}
	return play_unlocked(c, path, volume, layer0);
}

// Layer 0 plays alone until the caller mixes in others.
bool audio_play_music(const char* path, float volume) { return play(&s_music, path, volume, 1.0f); }

// Every layer starts silent (the first buffers are decoded before the caller can set volumes).
bool audio_play_layers(const char* path, float volume) { return play(&s_music, path, volume, 0.0f); }

// Cutscene voices: no loop (the voice ADX files have no loop points), one at a time.
bool audio_play_voice(const char* path, float volume) { return play(&s_voice, path, volume, 1.0f); }

// Seconds of the voice heard so far, < 0 before the DSP has started on it (demo.c holds the scene
// clock until then, as DramaDemoMain waits for the stream).
float audio_voice_position(void)
{
	Chan* c = &s_voice;
	if (pending_play(c))
		return -1.0f;                    // the refill thread has not started it yet
	if (!c->s.rate)
		return -1.0f;
	u32 pos = 0;
	if (ndspChnIsPlaying(c->ch))
		pos = ndspChnGetSamplePos(c->ch);
	else if (!c->played)
		return -1.0f;
	return (float)(c->played + pos) / (float)c->s.rate;
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
	for (int i = 0; i < NBUF; i++) {
		ndspWaveBuf* w = &c->wbuf[i];
		if (w->status != NDSP_WBUF_DONE && w->status != NDSP_WBUF_FREE)
			continue;
		c->played += w->nsamples;   // finished (0 for a buffer that never played)
		w->nsamples = 0;
		s16* pcm = (s16*)w->data_vaddr;
		u32 n = decode(&c->s, pcm, BUF_SAMPLES, c->volume);
		if (!n) {
			c->playing = false;
			return;
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
	chan_update(&s_music);
	chan_update(&s_voice);
}

const char* audio_status(void) { return s_status; }
