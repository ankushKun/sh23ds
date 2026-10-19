#pragma once
#include <3ds.h>

// Frame timing and memory logging for tuning on real hardware (port/PERFORMANCE.md).
// debug.txt "perflog 1" appends a line per second to <pack>/perf.txt, "memlog 1" a line per room
// load to <pack>/mem.txt; the debug HUD shows the last second either way.

void perf_init(const char* pack_dir);
void perf_set_log(bool frames, bool memory);
void perf_frame_begin(void);          // top of the main loop
void perf_mark_render(void);          // just before C3D_FrameBegin
// Adds the time since t0 (svcGetSystemTick) to a named part of this frame; slow frames list them.
// name must be a string literal (compared by address).
void perf_part(const char* name, u64 t0);
void perf_frame_end(const char* where); // after C3D_FrameEnd
const char* perf_status(void);
// A measuring window (the autotest's perf mode): frames since perf_window_reset, the slowest frame
// (update + render, so a frame that missed the vertical blank shows 33 or more), how many went over
// 17 and 33 ms, and the averages of the update and of citro3d's CPU time.
typedef struct { int frames, over17, over33; float max_ms, upd_avg, cpu_avg, max_work; } PerfWindow;
void perf_window_reset(void);
void perf_window_get(PerfWindow* w);
// Frame pacing, after perf_frame_end: cap 30 or 60 fixes the rate; 0 runs at 60 while the frames'
// work (update and drawing, without the wait for the vertical blank) fits in one, and switches to an
// even 30 when it keeps missing, instead of alternating 16 and 33 ms frames. Returns the rate.
int perf_pace(int cap);
// A line of its own in perf.txt when frame logging is on (tile loads and the like).
void perf_event(const char* where, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
float perf_ms(u64 ticks);
// The frame's GPU command buffer use (0..1, C3D_GetCmdBufUsage before C3D_FrameEnd): the second's
// highest goes in the perf.txt line.
void perf_cmdbuf(float used);
// Free linear, VRAM and heap memory now, as a HUD line; logged to mem.txt with "what" when enabled.
const char* mem_log(const char* what);
