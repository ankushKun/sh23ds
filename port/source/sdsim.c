#include "sdsim.h"
#include <stdio.h>

static volatile int s_kbps, s_open_ms;
static char s_status[48];

void sdsim_set(int kb_per_s, int open_ms)
{
	s_kbps = kb_per_s > 0 ? kb_per_s : 0;
	s_open_ms = s_kbps && open_ms >= 0 ? open_ms : 0;
}

void sdsim_open(void)
{
	if (s_open_ms)
		svcSleepThread((s64)s_open_ms * 1000000);
}

void sdsim_read(u32 bytes)
{
	int kbps = s_kbps;
	if (kbps && bytes)
		svcSleepThread((s64)bytes * 1000000000 / ((s64)kbps * 1024));
}

const char* sdsim_status(void)
{
	if (!s_kbps)
		return "sd full speed";
	snprintf(s_status, sizeof s_status, "sd %d KB/s, open %d ms", s_kbps, s_open_ms);
	return s_status;
}
