#include "perf.h"
#include "audio.h"
#include "profile.h"
#include <citro3d.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Memory split: profile.c sizes the malloc heap from the memory the process has (32 MB in the New
// 3DS's 124 MB mode, 15 MB in the Old 3DS's 64 MB mode) and gives the linear heap the rest.
extern u32 __ctru_heap_size, __ctru_linear_heap_size;

static struct {
	char pack[96];
	bool log_frames, log_mem;
	u64 t_begin, t_render;
	// sums over the current second
	u32 frames, total;
	float upd_sum, upd_max, ren_sum, ren_max, cpu_sum, gpu_sum, gpu_max, tot_max;
	char status[64], mem[64];
	// named parts of the current frame, for the slow frame lines
	struct { const char* name; float ms; } part[20];
	int nparts;
	// perf.txt lines wait here and go out with the once-a-second line: a file write costs tens of
	// ms, and a write in the middle of a frame would show up as the very hitch being measured
	struct { const char* name; float sum, max; } psum[24];   // the parts over the second
	int npsum;
	char log[4096];
	int loglen;
	PerfWindow win;
	float last_work;
	int pace_fps, pace_over, pace_fit, pace_cap;
	double win_upd, win_cpu;
	// low-water marks between two mem.txt lines (memlog): the area change lines alone miss the
	// peaks of streaming, cutscenes and movies
	u32 lin_min, heap_used_max;
} P;

static void log_add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_add(const char* fmt, ...)
{
	int room = (int)sizeof P.log - P.loglen;
	if (room <= 1)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(P.log + P.loglen, room, fmt, ap);
	va_end(ap);
	P.loglen += n < room ? n : room - 1;
}

static void log_flush(void)
{
	if (!P.loglen)
		return;
	char path[128];
	snprintf(path, sizeof path, "%s/perf.txt", P.pack);
	FILE* f = fopen(path, "a");
	if (f) {
		fwrite(P.log, 1, P.loglen, f);
		fclose(f);
	}
	P.loglen = 0;
}

void perf_init(const char* pack_dir)
{
	memset(&P, 0, sizeof P);
	snprintf(P.pack, sizeof P.pack, "%s", pack_dir);
}

void perf_set_log(bool frames, bool memory)
{
	P.log_frames = frames;
	P.log_mem = memory;
}

static float ms(u64 ticks) { return ticks * 1000.0f / SYSCLOCK_ARM11; }

void perf_frame_begin(void)
{
	P.t_begin = svcGetSystemTick();
	P.nparts = 0;
}

void perf_part(const char* name, u64 t0)
{
	perf_crumb(name);                    // done: the watchdog shows the step after it
	float t = ms(svcGetSystemTick() - t0);
	for (int i = 0; i < P.nparts; i++)
		if (P.part[i].name == name) {
			P.part[i].ms += t;
			return;
		}
	if (P.nparts < 20)
		P.part[P.nparts].name = name, P.part[P.nparts++].ms = t;
}
void perf_mark_render(void) { P.t_render = svcGetSystemTick(); }

static float s_cmd_max;
void perf_cmdbuf(float used) { s_cmd_max = used > s_cmd_max ? used : s_cmd_max; }

void perf_frame_end(const char* where)
{
	u64 now = svcGetSystemTick();
	if (!P.t_begin || !P.t_render)
		return;
	float upd = ms(P.t_render - P.t_begin), ren = ms(now - P.t_render);
	// citro3d: CPU time building the last frame's commands, GPU time drawing it (ms)
	float cpu = C3D_GetProcessingTime(), gpu = C3D_GetDrawingTime();
	P.frames++;
	P.total++;
	for (int i = 0; i < P.nparts; i++) {
		int k = 0;
		while (k < P.npsum && P.psum[k].name != P.part[i].name)
			k++;
		if (k == P.npsum) {
			if (P.npsum == 24)
				continue;
			P.psum[P.npsum].name = P.part[i].name, P.psum[P.npsum].sum = P.psum[P.npsum].max = 0.0f;
			P.npsum++;
		}
		P.psum[k].sum += P.part[i].ms;
		P.psum[k].max = fmaxf(P.psum[k].max, P.part[i].ms);
	}
	// single slow frames are what the player feels as a hitch, so they get a line of their own
	// slow: half a refresh over the frame's time (25 ms at 60 fps, 41.7 ms with the 30 fps cap)
	float slow = 1000.0f / (float)(P.pace_fps ? P.pace_fps : profile_frame_rate()) + 8.3f;
	if (P.log_frames && upd + ren > slow) {
		log_add("%-10s slow frame %lu: upd %5.2f render %5.2f", where, (unsigned long)P.total, upd, ren);
		for (int i = 0; i < P.nparts; i++)
			log_add("  %s %.2f", P.part[i].name, P.part[i].ms);
		log_add("\n");
	}
	if (P.log_mem) {
		u32 lf = linearSpaceFree();
		if (!P.lin_min || lf < P.lin_min)
			P.lin_min = lf;
		if ((P.total & 15) == 0) {
			struct mallinfo mi = mallinfo();
			if ((u32)mi.uordblks > P.heap_used_max)
				P.heap_used_max = (u32)mi.uordblks;
		}
	}
	float wait = 0.0f;
	for (int i = 0; i < P.nparts; i++)
		if (!strcmp(P.part[i].name, "r_wait"))
			wait += P.part[i].ms;
	// the frame's work: the CPU's (update and drawing, without the wait for the GPU and the vertical
	// blank) or the GPU's, whichever is longer. Pacing on the CPU's alone kept the game at 60 in the
	// fog, where the GPU takes 15 to 20 ms: uneven 22 to 25 ms frames. (Suspected, not yet confirmed,
	// as the cause of the console's fog flashes, which showed on the screen but not in the frame.)
	P.last_work = upd + ren - wait > gpu ? upd + ren - wait : gpu;
	P.win.frames++;
	P.win.over17 += upd + ren > 17.5f;
	P.win.over33 += upd + ren > 34.0f;
	P.win.max_ms = fmaxf(P.win.max_ms, upd + ren);
	P.win.max_work = fmaxf(P.win.max_work, upd + cpu);
	P.win_upd += upd, P.win_cpu += cpu;
	P.upd_sum += upd, P.ren_sum += ren, P.cpu_sum += cpu, P.gpu_sum += gpu;
	if (upd > P.upd_max) P.upd_max = upd;
	if (ren > P.ren_max) P.ren_max = ren;
	if (gpu > P.gpu_max) P.gpu_max = gpu;
	if (upd + ren > P.tot_max) P.tot_max = upd + ren;
	if (P.frames < 60)
		return;
	float n = (float)P.frames;
	snprintf(P.status, sizeof P.status, "upd %4.1f/%4.1f ren %4.1f gpu %4.1f ms", P.upd_sum / n, P.upd_max,
	         P.ren_sum / n, P.gpu_sum / n);
	if (P.log_frames) {
		log_add("%-10s upd %5.2f max %5.2f  render %5.2f max %5.2f  c3d cpu %5.2f gpu %5.2f max %5.2f  frame max %5.2f"
		        "  lin %lu  music %d%%  gaps %lu  cmdbuf %.0f%%\n",
		        where, P.upd_sum / n, P.upd_max, P.ren_sum / n, P.ren_max, P.cpu_sum / n, P.gpu_sum / n, P.gpu_max,
		        P.tot_max, (unsigned long)(linearSpaceFree() >> 10), audio_decode_load(),
		        (unsigned long)audio_underruns(), s_cmd_max * 100.0f);
		// the named parts: average over the second and the worst frame's share
		if (P.npsum) {
			log_add("%-10s parts", where);
			for (int i = 0; i < P.npsum; i++)
				if (P.psum[i].max >= 0.05f)
					log_add("  %s %.2f/%.2f", P.psum[i].name, P.psum[i].sum / n, P.psum[i].max);
			log_add("\n");
		}
		log_flush();
	}
	P.npsum = 0;
	P.frames = 0;
	s_cmd_max = 0.0f;
	P.upd_sum = P.upd_max = P.ren_sum = P.ren_max = P.cpu_sum = P.gpu_sum = P.gpu_max = P.tot_max = 0.0f;
}

const char* perf_status(void) { return P.status[0] ? P.status : "perf -"; }

int perf_pace(int cap)
{
	int want = cap == 30 || cap == 60 ? cap : 0;
	bool cap_changed = cap != P.pace_cap;   // the Options page (profile.c) may have set a rate meanwhile
	P.pace_cap = cap;
	if (!P.pace_fps)
		P.pace_fps = 60;
	if (!want) {
		// over budget in 4 of the last frames or so: 30; back to 60 after 2 s that would all fit
		if (P.last_work > 15.5f)
			P.pace_over += 2;
		else if (P.pace_over > 0)
			P.pace_over--;
		P.pace_fit = P.last_work < 11.0f ? P.pace_fit + 1 : 0;
		want = P.pace_fps;
		if (P.pace_fps == 60 && P.pace_over >= 8)
			want = 30;
		else if (P.pace_fps == 30 && P.pace_fit >= 120)
			want = 60;
	}
	if (want != P.pace_fps || cap_changed) {
		P.pace_fps = want;
		P.pace_over = P.pace_fit = 0;
		C3D_FrameRate((float)want);
	}
	return P.pace_fps;
}

void perf_window_reset(void)
{
	memset(&P.win, 0, sizeof P.win);
	P.win_upd = P.win_cpu = 0.0;
}

void perf_window_get(PerfWindow* w)
{
	*w = P.win;
	if (w->frames) {
		w->upd_avg = (float)(P.win_upd / w->frames);
		w->cpu_avg = (float)(P.win_cpu / w->frames);
	}
}

float perf_ms(u64 ticks) { return ms(ticks); }

void perf_event(const char* where, const char* fmt, ...)
{
	if (!P.log_frames)
		return;
	log_add("%-10s frame %lu: ", where, (unsigned long)P.total);
	int room = (int)sizeof P.log - P.loglen;
	if (room > 1) {
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(P.log + P.loglen, room, fmt, ap);
		va_end(ap);
		P.loglen += n < room ? n : room - 1;
	}
	log_add("\n");
}

const char* mem_log(const char* what)
{
	struct mallinfo mi = mallinfo();
	// heap free = never handed to malloc + free inside what malloc took from sbrk
	u32 heap_free = (__ctru_heap_size > (u32)mi.arena ? __ctru_heap_size - (u32)mi.arena : 0) + (u32)mi.fordblks;
	snprintf(P.mem, sizeof P.mem, "mem lin %lu vram %lu heap %lu KB", (unsigned long)(linearSpaceFree() >> 10),
	         (unsigned long)(vramSpaceFree() >> 10), (unsigned long)(heap_free >> 10));
	if (P.log_mem && what) {
		char path[128];
		snprintf(path, sizeof path, "%s/mem.txt", P.pack);
		FILE* f = fopen(path, "a");
		if (f) {
			u32 lf = linearSpaceFree();
			if (!P.lin_min || lf < P.lin_min)
				P.lin_min = lf;
			if ((u32)mi.uordblks > P.heap_used_max)
				P.heap_used_max = (u32)mi.uordblks;
			fprintf(f, "%-16s linear %6lu KB of %lu  vram %5lu KB  heap %6lu KB of %lu (malloc arena %lu, in use %lu)"
			        "  since last: linear min %lu, malloc max %lu\n",
			        what, (unsigned long)(lf >> 10), (unsigned long)(__ctru_linear_heap_size >> 10),
			        (unsigned long)(vramSpaceFree() >> 10), (unsigned long)(heap_free >> 10),
			        (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(mi.arena >> 10),
			        (unsigned long)(mi.uordblks >> 10), (unsigned long)(P.lin_min >> 10),
			        (unsigned long)(P.heap_used_max >> 10));
			P.lin_min = P.heap_used_max = 0;
			fclose(f);
		}
	}
	return P.mem;
}

// Breadcrumbs and the watchdog (perf.h).
#define CRUMBS 32
static struct {
	const char* volatile step[CRUMBS];
	volatile u32 frame[CRUMBS], ms[CRUMBS];
	volatile u32 n;                  // steps so far: the newest is n - 1
	Thread thread;
} C;

static u32 now_ms(void) { return (u32)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000)); }

void perf_crumb(const char* step)
{
	u32 i = C.n % CRUMBS;
	C.step[i] = step;
	C.frame[i] = P.total;
	C.ms[i] = now_ms();
	C.n++;
}

static void watchdog(void* arg)
{
	(void)arg;
	u32 seen = 0, since = now_ms(), written = 0;
	for (;;) {
		svcSleepThread(500 * 1000 * 1000LL);
		u32 n = C.n;
		if (n != seen) {
			seen = n, since = now_ms();
			continue;
		}
		if (n == written || now_ms() - since < 3000)
			continue;
		written = n;                     // once per stop
		char path[128];
		snprintf(path, sizeof path, "%s/stall.txt", P.pack);
		FILE* f = fopen(path, "a");
		if (!f)
			continue;
		fprintf(f, "main loop stopped %lu ms at frame %lu (%s; %s); its last steps, ms before the last:\n",
		        (unsigned long)(now_ms() - since), (unsigned long)P.total, P.status, P.mem);
		u32 first = n > CRUMBS ? n - CRUMBS : 0;
		for (u32 k = first; k < n; k++) {
			u32 i = k % CRUMBS;
			fprintf(f, "  %6ld  frame %lu  %s\n", -(long)(C.ms[(n - 1) % CRUMBS] - C.ms[i]), (unsigned long)C.frame[i],
			        C.step[i]);
		}
		fclose(f);
	}
}

void perf_watchdog_start(void)
{
	if (C.thread || !P.log_frames)
		return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// above the main thread, so a main thread spinning on its core does not keep it from writing;
	// it sleeps nearly all the time
	C.thread = threadCreate(watchdog, NULL, 16 * 1024, prio - 1, -2, true);
	perf_event("watchdog", "%s", C.thread ? "on" : "could not start");
}
