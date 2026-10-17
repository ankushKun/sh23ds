#include "perf.h"
#include <citro3d.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Memory split. The CIA runs in the New 3DS's 124 MB mode (package.sh, APP_SYSTEM_MODE_EXT), and
// libctru's default split of it gives the linear heap 32 MB and malloc the rest, about 91 MB. The
// port is the other way round: textures, vertex and index buffers and sound buffers all live in
// linear memory, while malloc held at most 3.3 MB in the measurements (port/PERFORMANCE.md). The
// tile reader thread can add a few MB per queued tile on top (an .s3m of up to 1.6 MB and the
// textures the cache lacks), so malloc gets a fixed 32 MB and libctru gives the linear heap
// whatever is left, about 90 MB in 124 MB mode. A 3dsx started in a smaller mode gets a smaller
// linear heap the same way; libctru only stops if the 32 MB themselves do not fit.
u32 __ctru_heap_size = 32 * 1024 * 1024;
extern u32 __ctru_linear_heap_size;

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
	char log[4096];
	int loglen;
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
	// single slow frames are what the player feels as a hitch, so they get a line of their own
	if (P.log_frames && upd + ren > 25.0f) {
		log_add("%-10s slow frame %lu: upd %5.2f render %5.2f", where, (unsigned long)P.total, upd, ren);
		for (int i = 0; i < P.nparts; i++)
			log_add("  %s %.2f", P.part[i].name, P.part[i].ms);
		log_add("\n");
	}
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
		log_add("%-10s upd %5.2f max %5.2f  render %5.2f max %5.2f  c3d cpu %5.2f gpu %5.2f max %5.2f  frame max %5.2f\n",
		        where, P.upd_sum / n, P.upd_max, P.ren_sum / n, P.ren_max, P.cpu_sum / n, P.gpu_sum / n, P.gpu_max,
		        P.tot_max);
		log_flush();
	}
	P.frames = 0;
	P.upd_sum = P.upd_max = P.ren_sum = P.ren_max = P.cpu_sum = P.gpu_sum = P.gpu_max = P.tot_max = 0.0f;
}

const char* perf_status(void) { return P.status[0] ? P.status : "perf -"; }

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
			fprintf(f, "%-16s linear %6lu KB of %lu  vram %5lu KB  heap %6lu KB of %lu (malloc arena %lu, in use %lu)\n",
			        what, (unsigned long)(linearSpaceFree() >> 10), (unsigned long)(__ctru_linear_heap_size >> 10),
			        (unsigned long)(vramSpaceFree() >> 10), (unsigned long)(heap_free >> 10),
			        (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(mi.arena >> 10),
			        (unsigned long)(mi.uordblks >> 10));
			fclose(f);
		}
	}
	return P.mem;
}
