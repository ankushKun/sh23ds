#include "sdsim.h"
#include <stdio.h>

static volatile int s_kbps, s_open_ms;
volatile int sdsim_legacy;
static char s_status[48];
// The console's FS service answers one request at a time, so a thread's read waits for the one
// before it to end. The simulated time is spent holding this lock to give the threads the same wait.
static LightLock s_fs;
__attribute__((constructor)) static void fs_lock_init(void) { LightLock_Init(&s_fs); }

void sdsim_set(int kb_per_s, int open_ms)
{
	s_kbps = kb_per_s > 0 ? kb_per_s : 0;
	s_open_ms = s_kbps && open_ms >= 0 ? open_ms : 0;
}

void sdsim_open(void)
{
	if (s_open_ms) {
		LightLock_Lock(&s_fs);
		svcSleepThread((s64)s_open_ms * 1000000);
		LightLock_Unlock(&s_fs);
	}
}

void sdsim_read(u32 bytes)
{
	int kbps = s_kbps;
	if (kbps && bytes) {
		LightLock_Lock(&s_fs);
		svcSleepThread((s64)bytes * 1000000000 / ((s64)kbps * 1024));
		LightLock_Unlock(&s_fs);
	}
}

const char* sdsim_status(void)
{
	if (!s_kbps)
		return "sd full speed";
	snprintf(s_status, sizeof s_status, "sd %d KB/s, open %d ms", s_kbps, s_open_ms);
	return s_status;
}
