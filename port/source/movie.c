#include "movie.h"
#include "gpusync.h"
#include "noise.h"
#include "touchui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_shbin.h"
#include "sfx.h"
#include "profile.h"

// pl_mpeg is plain C without SIMD; -O3 lets GCC unroll its IDCT and motion compensation loops.
#pragma GCC push_options
#pragma GCC optimize("O3")
#define PL_MPEG_IMPLEMENTATION
// File reads happen in chunks of this size. When the audio instance still read on the main thread,
// one 128 KB SD read (the default) took long enough to miss two vblanks; short reads also keep the
// two decoder threads from waiting long on each other's file access.
#define PLM_BUFFER_DEFAULT_SIZE (16 * 1024)
#include "third_party/pl_mpeg.h"
#pragma GCC pop_options

#define MOVIE_DIR "sdmc:/3ds/sh23ds/movie"
#define LOG_PATH "sdmc:/3ds/sh23ds/movie_log.txt"
#define TEX_W 512                   // smallest power-of-two texture holding a 400x240 picture
#define TEX_H 256
#define NSLOT 8                     // decoded frames waiting for display (absorbs runs of slow frames)
#define MIN_SLOTS 3                 // fewer when linear memory is short, down to this
#define NABUF 6                     // ndsp wave buffers
#define ABUF_SAMPLES (2 * PLM_AUDIO_SAMPLES_PER_FRAME)      // per buffer, 72 ms at 32 kHz
#define LOG_FRAMES 16384
#define DISPLAY_LAG (1.0 / 60.0)    // a frame drawn now is on screen from the next vblank

// The PC asks for PS2 names; Movie_BinkPlay (0x43d770) plays these from another file's audio track.
static const struct { const char* name; const char* file; int track; } s_alias[] = {
	{ "open_bgm", "open", 0 }, { "open_voc", "open", 1 },
	{ "end_mria", "end", 0 }, { "end_rbrt", "end", 1 }, { "end_scde", "end", 2 }, { "ending", "end", 3 },
};

typedef struct {
	u8 *y, *cb, *cr;            // linear memory, read by Y2R's DMA
	double time;
} Slot;

typedef struct { float x, y, u, v; } QuadVertex;

static C3D_RenderTarget* s_top;
static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uProjection;
static QuadVertex* s_quad;          // linear memory, 4 vertices (triangle strip)
static bool s_ready, s_y2r, s_ndsp;
static Handle s_y2r_event;
static bool s_log, s_cpu_convert;
static char s_status[64] = "no movie";

// Current movie
static bool s_active;
static char s_name[24];
static plm_t* s_vplm;               // video, owned by the decoder thread while it runs
static plm_t* s_aplm;               // audio, owned by the audio thread while it runs
static int s_w, s_h, s_tracks;
static double s_fps, s_duration;
static C3D_Tex s_tex[2];
static int s_front;                 // texture shown; Y2R writes the other one
static u32 s_front_idx;             // its frame number
static bool s_have_frame;
static Slot s_slot[NSLOT];
static u32 s_nslot;                 // slots allocated for this movie
static u32 s_wr, s_rd;              // frames produced / consumed (decoder writes s_wr, main s_rd)
static volatile bool s_quit, s_vdone, s_go;   // s_go: both threads exist, start working
static Thread s_vthread, s_athread;
static int s_core = -1;             // svcGetProcessorID in the thread (Azahar does not implement it)
static int s_core_req;              // core the thread was created on (-2: the default, core 0)

// Audio
static ndspWaveBuf s_wbuf[NABUF];
static s16* s_pcm;
// Written by the audio thread before ndspChnWaveBufAdd (whose lock orders the stores), read by
// clock_now on the main thread once ndsp reports the buffer queued or playing.
static u64 s_buf_start[NABUF];      // first sample of each queued buffer within the stream
static bool s_buf_used[NABUF];
static u64 s_queued;                // samples handed to ndsp so far
static u32 s_rate;
static bool s_has_audio, s_audio_over, s_started;
static volatile bool s_audio_eof;   // set by the audio thread
static volatile u32 s_abufs;        // wave buffers queued so far
static double s_clock;              // seconds of movie shown so far (monotonic)
static u64 s_tick_base;             // tick clock (no audio, or after the audio ended)
static double s_tick_from;
static u64 s_start_tick;

// Timing (svcGetSystemTick, 268 MHz)
static u64 s_dec_sum, s_dec_max, s_copy_sum;
static u32 s_dec_n, s_dec_over;     // frames decoded, of those slower than one frame period
static u64 s_conv_sum, s_conv_max, s_aud_sum;
static u32 s_conv_n, s_aud_n;
static u32 s_shown, s_dropped;
static u32* s_frame_ticks;          // per-frame decode ticks for the log
static u32 s_drop_at[64];           // the first dropped frames, for the log
static u32 s_updates, s_depth_sum, s_starved;   // queue depth seen by movie_update

static inline u32 load_acq(u32* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_rel(u32* p, u32 v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static double ms(u64 ticks) { return ticks / CPU_TICKS_PER_MSEC; }

bool movie_init(C3D_RenderTarget* top)
{
	s_top = top;
	s_dvlb = DVLB_ParseFile((u32*)movie_shbin, movie_shbin_size);
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uProjection = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
	s_quad = linearAlloc(4 * sizeof(QuadVertex));
	s_pcm = linearAlloc(NABUF * ABUF_SAMPLES * 2 * sizeof(s16));
	// ndspInit only counts a reference when audio.c already started it, and fails the same way
	// without dspfirm.cdc; movies then run on the tick clock.
	s_ndsp = R_SUCCEEDED(ndspInit());
	s_y2r = R_SUCCEEDED(y2rInit());
	if (s_y2r) {
		Y2RU_SetTransferEndInterrupt(true);
		if (R_FAILED(Y2RU_GetTransferEndEvent(&s_y2r_event)))
			s_y2r = false;
	}
	s_ready = s_quad && s_pcm;
	snprintf(s_status, sizeof s_status, "ready (y2r %s, ndsp %s)", s_y2r ? "ok" : "off", s_ndsp ? "ok" : "off");
	return s_ready;
}

void movie_exit(void)
{
	movie_stop();
	if (s_y2r)
		y2rExit();
	if (s_ndsp)
		ndspExit();
	shaderProgramFree(&s_prog);
	DVLB_Free(s_dvlb);
	linearFree(s_quad);
	linearFree(s_pcm);
	s_ready = s_y2r = s_ndsp = false;
}

void movie_debug(bool log, bool cpu_convert)
{
	s_log = log;
	s_cpu_convert = cpu_convert;
}

const char* movie_status(void) { return s_status; }
bool movie_active(void) { return s_active; }

// Decoder thread: keeps up to s_nslot frames decoded ahead of the display. It decodes every frame
// (P and B frames need their references); the main thread drops frames that come too late.
static void decode_thread(void* arg)
{
	(void)arg;
	s_core = svcGetProcessorID();
	u32 ysize = s_w * s_h, csize = ysize / 4;
	while (!s_go && !s_quit)
		svcSleepThread(1000000);
	while (!s_quit) {
		if (s_wr - load_acq(&s_rd) >= s_nslot) {
			svcSleepThread(2000000);
			continue;
		}
		u64 t0 = svcGetSystemTick();
		plm_frame_t* f = plm_decode_video(s_vplm);
		if (!f)
			break;
		u64 t1 = svcGetSystemTick();
		Slot* s = &s_slot[s_wr % s_nslot];
		// planes are padded to whole macroblocks; movies.py encodes multiples of 16, but be safe
		for (int r = 0; r < s_h; r++)
			memcpy(s->y + r * s_w, f->y.data + r * f->y.width, s_w);
		for (int r = 0; r < s_h / 2; r++) {
			memcpy(s->cb + r * s_w / 2, f->cb.data + r * f->cb.width, s_w / 2);
			memcpy(s->cr + r * s_w / 2, f->cr.data + r * f->cr.width, s_w / 2);
		}
		// cleaned on this core so Y2R's DMA sees the planes
		svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)s->y, ysize + 2 * csize);
		s->time = f->time;
		u64 t2 = svcGetSystemTick();
		u64 d = t1 - t0;
		if (s_frame_ticks && s_dec_n < LOG_FRAMES)
			s_frame_ticks[s_dec_n] = (u32)d;
		s_dec_sum += d;
		s_copy_sum += t2 - t1;
		if (d > s_dec_max)
			s_dec_max = d;
		if (d > SYSCLOCK_ARM11 / s_fps)
			s_dec_over++;
		s_dec_n++;
		store_rel(&s_wr, s_wr + 1);
	}
	s_vdone = true;
}

// Refills every free wave buffer with decoded MP2.
static void audio_feed(void)
{
	for (int i = 0; i < NABUF && !s_audio_eof; i++) {
		ndspWaveBuf* w = &s_wbuf[i];
		if (w->status != NDSP_WBUF_FREE && w->status != NDSP_WBUF_DONE)
			continue;
		u64 t0 = svcGetSystemTick();
		s16* out = (s16*)w->data_vaddr;
		u32 n = 0;
		while (n + PLM_AUDIO_SAMPLES_PER_FRAME <= ABUF_SAMPLES) {
			plm_samples_t* smp = plm_decode_audio(s_aplm);
			if (!smp) {
				s_audio_eof = true;
				break;
			}
			const float* in = smp->interleaved;
			s16* o = out + n * 2;
			for (u32 k = 0; k < smp->count * 2; k++) {
				float v = in[k] * 32767.0f;
				o[k] = (s16)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
			}
			n += smp->count;
		}
		if (!n)
			break;
		w->nsamples = n;
		DSP_FlushDataCache(out, n * 2 * sizeof(s16));
		s_buf_start[i] = s_queued;
		s_buf_used[i] = true;
		s_queued += n;
		ndspChnWaveBufAdd(MOVIE_CH, w);
		s_abufs++;
		s_aud_sum += svcGetSystemTick() - t0;
		s_aud_n++;
	}
}

// Audio thread: keeps the wave buffers full. The audio instance reads through the whole file to
// find its packets; those SD reads and the MP2 decoding stay off the main thread, where a slow read
// would make the display miss vblanks.
static void audio_thread(void* arg)
{
	(void)arg;
	while (!s_go && !s_quit)
		svcSleepThread(1000000);
	while (!s_quit && !s_audio_eof) {
		audio_feed();
		svcSleepThread(8000000);
	}
}

static void free_movie(void)
{
	if (s_vplm)
		plm_destroy(s_vplm);
	if (s_aplm)
		plm_destroy(s_aplm);
	s_vplm = s_aplm = NULL;
	for (int i = 0; i < NSLOT; i++) {
		if (s_slot[i].y)
			linearFree(s_slot[i].y);   // one block per slot: y, then cb, then cr
		s_slot[i].y = NULL;
	}
	for (int i = 0; i < 2; i++)
		if (s_tex[i].data)
			C3D_TexDelete(&s_tex[i]);
	memset(s_tex, 0, sizeof s_tex);
	free(s_frame_ticks);
	s_frame_ticks = NULL;
}

static void resolve(const char* name, char* file, size_t size, int* track)
{
	// accept "d:\data\movie\toilet.sfd" style paths as the PC passes them
	const char* b = name;
	for (const char* p = name; *p; p++)
		if (*p == '/' || *p == '\\')
			b = p + 1;
	char base[24];
	snprintf(base, sizeof base, "%s", b);
	char* dot = strrchr(base, '.');
	if (dot)
		*dot = 0;
	*track = 0;
	snprintf(file, size, "%s", base);
	for (size_t i = 0; i < sizeof s_alias / sizeof s_alias[0]; i++)
		if (!strcmp(base, s_alias[i].name)) {
			snprintf(file, size, "%s", s_alias[i].file);
			*track = s_alias[i].track;
		}
	snprintf(s_name, sizeof s_name, "%s", base);
}

bool movie_exists(const char* name)
{
	char file[24], path[96];
	int track;
	resolve(name, file, sizeof file, &track);
	snprintf(path, sizeof path, MOVIE_DIR "/%s.mpg", file);
	FILE* f = fopen(path, "rb");
	if (f)
		fclose(f);
	return f != NULL;
}

bool movie_start(const char* name)
{
	movie_stop();
	if (!s_ready)
		return false;
	char file[24], path[96];
	int track;
	resolve(name, file, sizeof file, &track);
	snprintf(path, sizeof path, MOVIE_DIR "/%s.mpg", file);
	if (profile_movie_scale() > 1) {
		// the low quality level plays the half-size set when the pack has it (movies.py --half):
		// a quarter of the pixels to decode, drawn at twice the size
		char lo[96];
		snprintf(lo, sizeof lo, MOVIE_DIR "/half/%s.mpg", file);
		FILE* f = fopen(lo, "rb");
		if (f) {
			fclose(f);
			snprintf(path, sizeof path, "%s", lo);
		}
	}

	s_vplm = plm_create_with_filename(path);
	s_aplm = s_vplm ? plm_create_with_filename(path) : NULL;
	if (!s_vplm || !s_aplm || !plm_get_num_video_streams(s_vplm)) {
		snprintf(s_status, sizeof s_status, "missing movie/%s.mpg", file);
		free_movie();
		return false;
	}
	plm_set_audio_enabled(s_vplm, false);
	plm_set_video_enabled(s_aplm, false);
	s_w = plm_get_width(s_vplm) & ~7;
	s_h = plm_get_height(s_vplm) & ~7;
	s_fps = plm_get_framerate(s_vplm);
	s_tracks = plm_get_num_audio_streams(s_aplm);
	if (s_w < 8 || s_h < 8 || s_w > TEX_W || s_h > TEX_H || s_fps <= 0.0) {
		snprintf(s_status, sizeof s_status, "bad movie %dx%d", s_w, s_h);
		free_movie();
		return false;
	}
	s_duration = plm_get_duration(s_aplm);
	s_has_audio = s_ndsp && s_tracks > 0;
	if (s_has_audio) {
		// The audio decoder read its first frames from stream 0 when it was created; starting over
		// keeps them out of another track (open_voc began with 108 ms of open_bgm).
		plm_set_audio_stream(s_aplm, track < s_tracks ? track : 0);
		plm_rewind(s_aplm);
		s_rate = plm_get_samplerate(s_aplm);
		s_has_audio = s_rate > 0;
	}

	bool ok = true;
	for (int i = 0; i < 2 && ok; i++) {
		ok = C3D_TexInit(&s_tex[i], TEX_W, TEX_H, GPU_RGB565);
		if (ok) {
			memset(s_tex[i].data, 0, s_tex[i].size);         // black outside the picture
			GSPGPU_FlushDataCache(s_tex[i].data, s_tex[i].size);
			bool scaled = s_w * 2 <= 400 && s_h * 2 <= 240;          // a half-size movie
			C3D_TexSetFilter(&s_tex[i], scaled ? GPU_LINEAR : GPU_NEAREST, scaled ? GPU_LINEAR : GPU_NEAREST);
			C3D_TexSetWrap(&s_tex[i], GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
			s_tex[i].border = 0;
		}
	}
	u32 frame_bytes = s_w * s_h * 3 / 2;
	for (s_nslot = 0; ok && s_nslot < NSLOT; s_nslot++) {
		u8* m = linearAlloc(frame_bytes);
		if (!m)
			break;
		s_slot[s_nslot].y = m;
		s_slot[s_nslot].cb = m + s_w * s_h;
		s_slot[s_nslot].cr = m + s_w * s_h * 5 / 4;
	}
	ok = ok && s_nslot >= MIN_SLOTS;
	if (!ok) {
		snprintf(s_status, sizeof s_status, "out of linear memory");
		free_movie();
		return false;
	}
	if (s_log)
		s_frame_ticks = malloc(LOG_FRAMES * sizeof(u32));

	s_wr = s_rd = 0;
	s_quit = s_vdone = false;
	s_have_frame = false;
	s_front = 0;
	s_front_idx = 0;
	s_dec_sum = s_dec_max = s_copy_sum = s_conv_sum = s_conv_max = s_aud_sum = 0;
	s_dec_n = s_dec_over = s_conv_n = s_aud_n = s_shown = s_dropped = 0;
	s_core = -1;
	s_updates = s_depth_sum = s_starved = 0;
	s_queued = 0;
	s_audio_eof = s_audio_over = s_started = false;
	s_clock = 0.0;
	memset(s_buf_used, 0, sizeof s_buf_used);

	if (s_has_audio) {
		ndspChnReset(MOVIE_CH);
		ndspChnSetInterp(MOVIE_CH, NDSP_INTERP_LINEAR);
		ndspChnSetRate(MOVIE_CH, (float)s_rate);
		ndspChnSetFormat(MOVIE_CH, NDSP_FORMAT_STEREO_PCM16);
		float mix[12] = { 1.0f, 1.0f };
		ndspChnSetMix(MOVIE_CH, mix);
		ndspChnSetPaused(MOVIE_CH, true);        // starts once the first frames are decoded
		for (int i = 0; i < NABUF; i++) {
			memset(&s_wbuf[i], 0, sizeof s_wbuf[i]);
			s_wbuf[i].data_vaddr = s_pcm + i * ABUF_SAMPLES * 2;
			s_wbuf[i].status = NDSP_WBUF_FREE;
		}
	}

	// Core 2 is free for applications on the New 3DS. Both threads sit below the main thread's
	// priority so that the display loop wins wherever they end up sharing a core (core 0 when core 2
	// is refused), and audio comes before video.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_abufs = 0;
	s_audio_eof = !s_has_audio;
	s_go = false;
	// On an Old 3DS the picture decodes on core 0, where the main thread only waits for the vertical
	// blank during a movie, and the sound on core 1 (the share profile.c asked for), whose 30 % would
	// be tight for the picture (port/OLD3DS.md).
	for (int pass = 0; pass < 2 && !s_vthread; pass++) {
		int core = pass ? -2 : profile_console_new() ? 2 : -2;
		int acore = pass ? -2 : profile_console_new() ? 2 : profile_helper_core();
		s_core_req = core;
		s_vthread = threadCreate(decode_thread, NULL, 64 * 1024, prio + 2, core, false);
		if (s_vthread && s_has_audio) {
			s_athread = threadCreate(audio_thread, NULL, 32 * 1024, prio + 1, acore, false);
			if (!s_athread) {
				s_quit = true;               // both or neither on this core
				threadJoin(s_vthread, U64_MAX);
				threadFree(s_vthread);
				s_vthread = NULL;
				s_quit = false;
			}
		}
	}
	s_go = true;
	if (!s_vthread) {
		snprintf(s_status, sizeof s_status, "no decoder thread");
		if (s_has_audio)
			ndspChnReset(MOVIE_CH);
		free_movie();
		return false;
	}
	s_active = true;
	s_start_tick = svcGetSystemTick();
	snprintf(s_status, sizeof s_status, "%s %dx%d %s", s_name, s_w, s_h, s_has_audio ? "" : "(no audio)");
	return true;
}

static void write_log(void)
{
	FILE* f = fopen(LOG_PATH, "a");
	if (!f)
		return;
	fprintf(f, "movie %s %dx%d %.3f fps thread core %d (id %d) convert %s\n", s_name, s_w, s_h, s_fps, s_core_req, s_core,
	        s_y2r && !s_cpu_convert ? "y2r" : "cpu");
	fprintf(f, "decoded %lu avg %.2f ms max %.2f ms over %lu copy %.2f ms convert avg %.2f ms max %.2f ms "
	        "audio %.2f ms/buffer shown %lu dropped %lu\n",
	        (unsigned long)s_dec_n, s_dec_n ? ms(s_dec_sum) / s_dec_n : 0.0, ms(s_dec_max), (unsigned long)s_dec_over,
	        s_dec_n ? ms(s_copy_sum) / s_dec_n : 0.0, s_conv_n ? ms(s_conv_sum) / s_conv_n : 0.0, ms(s_conv_max),
	        s_aud_n ? ms(s_aud_sum) / s_aud_n : 0.0, (unsigned long)s_shown, (unsigned long)s_dropped);
	fprintf(f, "updates %lu, average queue %.2f of %lu frames, empty %lu times, linear free %lu KB\n",
	        (unsigned long)s_updates, s_updates ? (double)s_depth_sum / s_updates : 0.0, (unsigned long)s_nslot,
	        (unsigned long)s_starved, (unsigned long)(linearSpaceFree() / 1024));
	if (s_dropped) {
		fprintf(f, "dropped:");
		for (u32 i = 0; i < s_dropped && i < 64; i++)
			fprintf(f, " %lu", (unsigned long)s_drop_at[i]);
		fprintf(f, "\n");
	}
	if (s_frame_ticks) {
		u32 n = s_dec_n < LOG_FRAMES ? s_dec_n : LOG_FRAMES;
		for (u32 i = 0; i < n; i++)
			fprintf(f, "%.2f%c", ms(s_frame_ticks[i]), (i % 16) == 15 || i + 1 == n ? '\n' : ' ');
	}
	fclose(f);
}

void movie_stop(void)
{
	if (!s_active)
		return;
	s_quit = true;
	threadJoin(s_vthread, U64_MAX);
	threadFree(s_vthread);
	s_vthread = NULL;
	if (s_athread) {
		threadJoin(s_athread, U64_MAX);
		threadFree(s_athread);
		s_athread = NULL;
	}
	if (s_has_audio)
		ndspChnReset(MOVIE_CH);
	if (s_log)
		write_log();
	gpu_sync();             // the GPU may still be reading the last frame's texture
	free_movie();
	s_active = false;
	snprintf(s_status, sizeof s_status, "%s: %lu shown, %lu dropped", s_name, (unsigned long)s_shown,
	         (unsigned long)s_dropped);
}

// Seconds of movie that should be on screen now: the sample ndsp is playing, or the tick clock when
// there is no audio (or it ended before the picture).
static double clock_now(void)
{
	double t = s_clock;
	if (s_has_audio && !s_audio_over) {
		u16 seq = ndspChnGetWaveBufSeq(MOVIE_CH);
		u32 pos = ndspChnGetSamplePos(MOVIE_CH);
		for (int i = 0; i < NABUF; i++)
			if (s_buf_used[i] && s_wbuf[i].sequence_id == seq &&
			    (s_wbuf[i].status == NDSP_WBUF_PLAYING || s_wbuf[i].status == NDSP_WBUF_QUEUED)) {
				t = (double)(s_buf_start[i] + pos) / s_rate;
				break;
			}
		bool idle = s_audio_eof;
		for (int i = 0; i < NABUF; i++)
			if (s_wbuf[i].status == NDSP_WBUF_PLAYING || s_wbuf[i].status == NDSP_WBUF_QUEUED)
				idle = false;
		if (idle) {
			s_audio_over = true;
			t = (double)s_queued / s_rate;
			s_tick_from = t > s_clock ? t : s_clock;
			s_tick_base = svcGetSystemTick();
		}
	} else {
		t = s_tick_from + (double)(svcGetSystemTick() - s_tick_base) / SYSCLOCK_ARM11;
	}
	if (t > s_clock)
		s_clock = t;
	return s_clock;
}

static inline u32 morton(u32 x, u32 y)
{
	return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

// CPU fallback with the layout Y2R's 8x8 block mode produces: tiles of 8x8 pixels in Z-order,
// rows of TEX_W / 8 tiles, the picture's top row first. BT.601 with the video (16..235) range.
static void cpu_convert(const Slot* s, u16* dst)
{
	for (int y = 0; y < s_h; y += 2)
		for (int x = 0; x < s_w; x += 2) {
			int cb = s->cb[(y / 2) * (s_w / 2) + x / 2] - 128;
			int cr = s->cr[(y / 2) * (s_w / 2) + x / 2] - 128;
			int rv = 409 * cr + 128, gu = -100 * cb - 208 * cr + 128, bu = 516 * cb + 128;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++) {
					int px = x + dx, py = y + dy;
					int l = (s->y[py * s_w + px] - 16) * 298;
					int r = (l + rv) >> 8, g = (l + gu) >> 8, b = (l + bu) >> 8;
					r = r < 0 ? 0 : r > 255 ? 255 : r;
					g = g < 0 ? 0 : g > 255 ? 255 : g;
					b = b < 0 ? 0 : b > 255 ? 255 : b;
					u32 o = ((py >> 3) * (TEX_W >> 3) + (px >> 3)) * 64 + morton(px & 7, py & 7);
					dst[o] = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
				}
		}
	GSPGPU_FlushDataCache(dst, TEX_W * TEX_H * sizeof(u16));
}

static bool y2r_convert(const Slot* s, void* dst)
{
	Y2RU_ConversionParams p = {
		.input_format = INPUT_YUV420_INDIV_8,
		.output_format = OUTPUT_RGB_16_565,
		.rotation = ROTATION_NONE,
		.block_alignment = BLOCK_8_BY_8,
		.input_line_width = s_w,
		.input_lines = s_h,
		.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING,   // MPEG-1 luma is 16..235
		.alpha = 0xFF,
	};
	if (R_FAILED(Y2RU_SetConversionParams(&p)))
		return false;
	Y2RU_SetSendingY(s->y, s_w * s_h, s_w, 0);
	Y2RU_SetSendingU(s->cb, s_w * s_h / 4, s_w / 2, 0);
	Y2RU_SetSendingV(s->cr, s_w * s_h / 4, s_w / 2, 0);
	// one unit is a row of 8x8 tiles; the gap skips the texture's columns right of the picture
	Y2RU_SetReceiving(dst, s_w * s_h * 2, s_w * 8 * 2, (TEX_W - s_w) * 8 * 2);
	svcClearEvent(s_y2r_event);
	if (R_FAILED(Y2RU_StartConversion()))
		return false;
	if (svcWaitSynchronization(s_y2r_event, 100000000LL) != 0) {
		Y2RU_StopConversion();
		return false;
	}
	return true;
}

// The next decoded frame into the texture not on screen, which then is.
static void show_next(void)
{
	const Slot* s = &s_slot[s_rd % s_nslot];
	int back = s_front ^ 1;
	u64 t0 = svcGetSystemTick();
	bool ok = false;
	if (s_y2r && !s_cpu_convert)
		ok = y2r_convert(s, s_tex[back].data);
	if (!ok)
		cpu_convert(s, s_tex[back].data);
	u64 d = svcGetSystemTick() - t0;
	s_conv_sum += d;
	s_conv_n++;
	if (d > s_conv_max)
		s_conv_max = d;
	s_front = back;
	s_front_idx = s_rd;
	s_have_frame = true;
	s_shown++;
	store_rel(&s_rd, s_rd + 1);
}

bool movie_update(u32 down)
{
	if (!s_active)
		return false;
	if (down & (KEY_A | KEY_START)) {
		movie_stop();
		return false;
	}
	if (!s_started) {
		// a short head start for the decoders, so the first frames are not dropped
		bool ready = (load_acq(&s_wr) >= 2 || s_vdone) && (s_abufs >= 2 || s_audio_eof);
		ready = ready || svcGetSystemTick() - s_start_tick > SYSCLOCK_ARM11 / 2;
		if (!ready)
			return true;
		s_started = true;
		if (s_has_audio)
			ndspChnSetPaused(MOVIE_CH, false);
		else
			s_tick_base = svcGetSystemTick(), s_tick_from = 0.0;
	}
	// the frame nearest to what the speaker plays when this one reaches the screen
	double t = clock_now() + DISPLAY_LAG + 0.5 / s_fps;
	u32 wr = load_acq(&s_wr);
	s_updates++;
	s_depth_sum += wr - s_rd;
	if (wr == s_rd && !s_vdone)
		s_starved++;
	while (s_rd < wr && s_slot[s_rd % s_nslot].time <= t) {
		// a later frame is due as well: this one is late, skip it
		if (s_rd + 1 < wr && s_slot[(s_rd + 1) % s_nslot].time <= t) {
			if (s_dropped < 64)
				s_drop_at[s_dropped] = s_rd;
			store_rel(&s_rd, s_rd + 1);
			s_dropped++;
			continue;
		}
		show_next();
		break;
	}
	bool video_over = s_vdone && s_rd == load_acq(&s_wr);
	bool audio_over = !s_has_audio || s_audio_over;
	if (video_over && audio_over) {
		movie_stop();
		return false;
	}
	return true;
}

// The screen-filling quad's program and vertex state (movie_draw and the fades).
static void bind_quad(void)
{
	C3D_Mtx ortho;
	Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
	C3D_BindProgram(&s_prog);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, &ortho);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);     // position
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);     // texcoord
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_quad, sizeof(QuadVertex), 2, 0x10);
	C3D_LightEnvBind(NULL);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

// Black at alpha a over whatever the top screen's target holds (the fade out after a movie).
static void draw_black(float a)
{
	QuadVertex q[4] = { { 0, 240, 0, 0 }, { 0, 0, 0, 0 }, { 400, 240, 0, 0 }, { 400, 0, 0, 0 } };
	memcpy(s_quad, q, sizeof q);
	GSPGPU_FlushDataCache(s_quad, sizeof q);
	bind_quad();
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, (u32)(a * 255.0f) << 24);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
}

static void draw_frame(float alpha);

void movie_draw(void) { draw_frame(1.0f); }

// The current frame; alpha below 1 lays it over what the target already holds (the fade in from
// the game's last picture), without the noise.
static void draw_frame(float alpha)
{
	if (!s_active || !s_have_frame)
		return;
	// one quad over the whole screen; outside the picture the texture is black (cleared texels on
	// the right and bottom, the black border colour on the left and top)
	// a half-size movie (Old 3DS profile) is drawn at twice its size, filtered
	int k = 1;
	while ((k + 1) * s_w <= 400 && (k + 1) * s_h <= 240)
		k++;
	float x0 = (400 - k * s_w) / 2, y0 = (240 - k * s_h) / 2;
	float u0 = -x0 / k / TEX_W, u1 = (400.0f - x0) / k / TEX_W;
	// The start of texture memory (the picture's first row) is the top, near t = 1. A test pattern in
	// Azahar showed screen line k sampling memory row k - 1 with the plain mapping, so it is nudged by
	// one texel: lines 1..239 now show rows 1..239 exactly and line 0 samples a cleared (black) row.
	float v0 = 1.0f + (y0 - 1.0f) / k / TEX_H, v1 = 1.0f - (241.0f - y0) / k / TEX_H;
	QuadVertex q[4] = {
		{ 0.0f, 240.0f, u0, v0 }, { 0.0f, 0.0f, u0, v1 },
		{ 400.0f, 240.0f, u1, v0 }, { 400.0f, 0.0f, u1, v1 },
	};
	memcpy(s_quad, q, sizeof q);
	GSPGPU_FlushDataCache(s_quad, sizeof q);

	bind_quad();
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (alpha < 1.0f) {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, 0, 0);
		C3D_TexEnvColor(env, (u32)(alpha * 255.0f) << 24);
	} else {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
	}
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexBind(0, &s_tex[s_front]);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	// the Noise Effect over the picture (noise.c), y up as the quad above
	if (alpha >= 1.0f)
		noise_draw_movie(x0, y0, 400.0f - x0, 240.0f - y0);
}

static void print_stats(void)
{
	// 40 columns on the bottom screen
	printf("\x1b[2;2HMovie   %-10s %dx%d %d track%s  ", s_name, s_w, s_h, s_tracks, s_tracks == 1 ? "" : "s");
	printf("\x1b[4;2Htime    %6.1f / %6.1f s      ", s_clock, s_duration);
	printf("\x1b[6;2Hdecode  avg %5.2f  max %5.2f ms  ", s_dec_n ? ms(s_dec_sum) / s_dec_n : 0.0, ms(s_dec_max));
	printf("\x1b[7;2H        over %.1f ms: %lu of %lu    ", 1000.0 / s_fps, (unsigned long)s_dec_over,
	       (unsigned long)s_dec_n);
	printf("\x1b[8;2H        core %d (id %d), copy %4.2f ms  ", s_core_req, s_core,
	       s_dec_n ? ms(s_copy_sum) / s_dec_n : 0.0);
	printf("\x1b[10;2H%-6s  avg %5.2f  max %5.2f ms  ", s_y2r && !s_cpu_convert ? "y2r" : "cpu",
	       s_conv_n ? ms(s_conv_sum) / s_conv_n : 0.0, ms(s_conv_max));
	printf("\x1b[11;2Haudio   %5.2f ms per %d samples%s  ", s_aud_n ? ms(s_aud_sum) / s_aud_n : 0.0, ABUF_SAMPLES,
	       s_has_audio ? "" : " (off)");
	printf("\x1b[13;2Hshown %-5lu dropped %-4lu queue %.1f/%lu  ", (unsigned long)s_shown, (unsigned long)s_dropped,
	       s_updates ? (double)s_depth_sum / s_updates : 0.0, (unsigned long)s_nslot);
	printf("\x1b[17;2Hlinear memory free %lu KB   ", (unsigned long)(linearSpaceFree() / 1024));
	printf("\x1b[28;2HA / START: skip");
}

static bool s_skip_all;
void movie_skip_all(bool on) { s_skip_all = on; }

// Transitions (port/MOVIES.md, "Fades"): a movie no longer cuts in over the game. The game's last
// picture stays in the top target (nothing clears it), and the movie's first frame is laid over it
// at a growing alpha, so the room fades into the movie's first picture (white for the Laura scenes,
// black for most) before the movie's clock and sound start. After the movie, or when it is skipped,
// its last picture fades to black the same way. Each frame blends over the last one, so the alpha
// of a step is what is left to cover: (f - f_prev) / (1 - f_prev).
#define FADE_IN 0.75f
#define FADE_OUT 0.4f

typedef void (*FadeDraw)(float a);

// Runs one fade of `secs` drawing `draw` at each step's alpha; false when the app is closing. A
// skip key during it ends it at once (skip true: the caller's movie stops too).
static bool fade(float secs, FadeDraw draw, bool* skip)
{
	u64 t0 = svcGetSystemTick();
	float prev = 0.0f;
	while (prev < 1.0f) {
		if (!aptMainLoop())
			return false;
		hidScanInput();
		u32 kd = hidKeysDown(), kh = 0;
		touchui_input(&kd, &kh);
		if (skip && (kd & (KEY_A | KEY_START))) {
			*skip = true;
			return true;
		}
		float f = (float)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11 / secs;
		f = f > 1.0f ? 1.0f : f;
		f = f * f * (3.0f - 2.0f * f);          // eased at both ends
		float a = prev >= 1.0f ? 1.0f : (f - prev) / (1.0f - prev);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_FrameDrawOn(s_top);                  // no clear: blend over the last picture
		draw(a > 1.0f ? 1.0f : a);
		touchui_frame();
		C3D_FrameEnd(0);
		prev = f;
	}
	return true;
}

static void fade_in_step(float a) { draw_frame(a); }

// Waits (up to a second) for the first frame, shows it into the game's picture, then lets the movie
// run from it: the clock starts on the first movie_update after this.
static bool fade_in(void)
{
	u64 t0 = svcGetSystemTick();
	while (load_acq(&s_wr) < 1 && !s_vdone && svcGetSystemTick() - t0 < SYSCLOCK_ARM11)
		svcSleepThread(2000000);
	if (load_acq(&s_wr) < 1)
		return aptMainLoop();
	show_next();                             // frame 0, on screen from the start
	bool skip = false;
	if (!fade(FADE_IN, fade_in_step, &skip))
		return false;
	if (skip)
		movie_stop();                        // the loop then ends at once, into the fade out
	s_start_tick = svcGetSystemTick();       // the decoders' head start counts from here
	return true;
}

static bool fade_out(void) { return fade(FADE_OUT, draw_black, NULL); }

bool movie_play(const char* name)
{
	if (s_skip_all)
		return true;                 // playtest.c: movies count as watched
	if (!movie_start(name))
		return false;
	// The title's game start sound (main.c) belongs to the title: when New Game goes straight into
	// the opening movie it ends there instead of resuming mid-sound a minute later.
	sfx_stop(SFX_GAME_START);
	// Music and effects stop for the movie: their channels are paused (nothing refills them while
	// this loop runs) and continue afterwards from where they were.
	bool paused[24] = { false };
	if (s_ndsp)
		for (int ch = 0; ch < 24; ch++)
			if (ch != MOVIE_CH && !ndspChnIsPaused(ch)) {
				ndspChnSetPaused(ch, true);
				paused[ch] = true;
			}
	consoleClear();
	touchui_movie(true);
	bool alive = fade_in();
	u32 frame = 0;
	while (alive && (alive = aptMainLoop())) {
		hidScanInput();
		u32 kd = hidKeysDown(), kh = 0;
		touchui_input(&kd, &kh);        // the touch screen's Skip button
		if (!movie_update(kd))
			break;
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(s_top, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(s_top);
		movie_draw();
		touchui_frame();
		C3D_FrameEnd(0);
		if ((frame++ % 15) == 0)
			print_stats();
		// every frame: lets a screenshot tell which frame the player thinks is up, and when
		printf("\x1b[15;2Hframe %5lu at %8.3f s  ", (unsigned long)s_front_idx, s_clock);
	}
	movie_stop();
	if (alive)
		alive = fade_out();
	touchui_movie(false);
	for (int ch = 0; ch < 24; ch++)
		if (paused[ch])
			ndspChnSetPaused(ch, false);
	consoleClear();
	if (alive) {
		// black while the caller loads what comes next, instead of the last movie frame
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(s_top, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(s_top);
		C3D_FrameEnd(0);
	}
	return alive;
}
