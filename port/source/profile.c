#include "profile.h"
#include <citro3d.h>
#include <string.h>
#if __has_include("quality.h")
#include "quality.h"
#endif

// Hardware profile, quality level, frame rate and controls layout (profile.h, port/OLD3DS.md).

static struct {
	bool console_new;
	bool core1;              // the system granted the Old 3DS app a share of core 1
	int hw, quality, layout, fps; // CHOICE_*
	int helper_core;         // options.txt "helpercore" (testing): -1 auto, else that core
	int tex_half;            // options.txt "texhalf": 0 auto, 1 always, 2 never
	char status[80];
} P;

// ---------------------------------------------------------------- memory split
//
// libctru allocates the malloc heap and the linear heap before main() from what the process may
// still commit (the CIA's memory mode, or whatever the Homebrew Launcher's title had for a 3dsx).
// This replaces its weak __system_allocateHeaps (libctru 2.x, allocateHeaps.c: the same steps)
// to size the malloc heap from that amount instead of a fixed number. Every texture, vertex and
// index buffer and sound buffer lives in linear memory, so linear memory gets the most: malloc
// held at most 3.7 MB in any measurement (port/PERFORMANCE.md), plus the tile reader's buffers.
//
//   124 MB mode (New 3DS CIA): 32 MB malloc, 90.6 MB linear (as before)
//    80 MB mode (SH23DS_SYSTEM_MODE=80MB): 19 MB malloc, 59.4 MB linear
//    64 MB mode (Old 3DS CIA, a 3dsx from the Homebrew Launcher): 15 MB malloc, 47.4 MB linear
u32 __ctru_heap_size, __ctru_linear_heap_size;
extern u32 __ctru_heap, __ctru_linear_heap;
extern char* fake_heap_start;
extern char* fake_heap_end;
void mappableInit(u32 addrMin, u32 addrMax);

#define MB (1024u * 1024u)

static u32 heap_for(u32 remaining)
{
	if (remaining >= 100 * MB)
		return 32 * MB;
	u32 h = (remaining / 4) & ~(MB - 1);
	if (h < 12 * MB) h = 12 * MB;
	if (h > 20 * MB) h = 20 * MB;
	if (h > remaining / 2) h = (remaining / 2) & ~0xFFFu;
	return h;
}

void __system_allocateHeaps(void)
{
	Handle reslimit = 0;
	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
		svcBreak(USERBREAK_PANIC);
	ResourceLimitType type = RESLIMIT_COMMIT;
	s64 max_commit = 0, cur_commit = 0;
	svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
	svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
	svcCloseHandle(reslimit);
	u32 remaining = (u32)(max_commit - cur_commit) & ~0xFFFu;

	__ctru_heap_size = heap_for(remaining);
	__ctru_linear_heap_size = (remaining - __ctru_heap_size) & ~0xFFFu;

	if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC,
	                              MEMPERM_READ | MEMPERM_WRITE)))
		svcBreak(USERBREAK_PANIC);
	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
	                              MEMPERM_READ | MEMPERM_WRITE)))
		svcBreak(USERBREAK_PANIC);
	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = (char*)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}

// ---------------------------------------------------------------- start-up

static int clamp_choice(int v) { return v < 0 || v > 2 ? CHOICE_AUTO : v; }

bool profile_option(const char* key, int val)
{
	if (!strcmp(key, "hwprofile")) P.hw = clamp_choice(val);
	else if (!strcmp(key, "quality")) P.quality = clamp_choice(val);
	else if (!strcmp(key, "ctrllayout")) P.layout = clamp_choice(val);
	else if (!strcmp(key, "framerate")) P.fps = clamp_choice(val);
	else if (!strcmp(key, "helpercore")) P.helper_core = val >= -2 && val <= 2 ? val : -1;
	else if (!strcmp(key, "texhalf")) P.tex_half = clamp_choice(val);
	else return false;
	return true;
}

void profile_init(const char* options_path)
{
	P.helper_core = -1;
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	P.console_new = n3ds;
	FILE* f = fopen(options_path, "r");
	if (f) {
		char key[32];
		int val;
		while (fscanf(f, "%31s %d", key, &val) == 2)
			profile_option(key, val);
		fclose(f);
	}
	if (n3ds) {
		// 804 MHz and the L2 cache whatever the profile: the profile only picks the settings
		osSetSpeedupEnable(true);
	} else {
		// An Old 3DS application may only run threads on core 1 (the system core) after asking
		// for a share of its time; 3dbrew notes more than 30 % does not help on the Old 3DS.
		P.core1 = R_SUCCEEDED(APT_SetAppCpuTimeLimit(30));
	}
	profile_apply_quality();
}

bool profile_console_new(void) { return P.console_new; }

HwProfile profile_get(void)
{
	if (P.hw == CHOICE_LOW_OLD) return PROFILE_OLD3DS;
	if (P.hw == CHOICE_HIGH_NEW) return PROFILE_NEW3DS;
	return P.console_new ? PROFILE_NEW3DS : PROFILE_OLD3DS;
}

QualityLevel profile_quality(void)
{
	if (P.quality == CHOICE_LOW_OLD) return QUALITY_LOW;
	if (P.quality == CHOICE_HIGH_NEW) return QUALITY_HIGH;
	return profile_new() ? QUALITY_HIGH : QUALITY_LOW;
}

ControlsLayout controls_layout(void)
{
	if (P.layout == CHOICE_LOW_OLD) return CONTROLS_LAYOUT_OLD;
	if (P.layout == CHOICE_HIGH_NEW) return CONTROLS_LAYOUT_NEW;
	return P.console_new ? CONTROLS_LAYOUT_NEW : CONTROLS_LAYOUT_OLD;
}

// The Circle Pad Pro on an Old 3DS reaches the game through ir:rst like the New 3DS's ZL, ZR and
// C-Stick do; libctru only opens ir:rst on a New 3DS (weak hidShouldUseIrrst), so this opens it on
// both. Without the add-on ir:rst just reports nothing.
bool hidShouldUseIrrst(void) { return true; }

// ---------------------------------------------------------------- settings by profile and quality

int profile_helper_core(void)
{
	if (P.helper_core != -1)
		return P.helper_core;
	if (P.console_new) return 2;
	return P.core1 ? 1 : -2;
}

int profile_music_core(void)
{
	if (P.helper_core != -1)
		return P.helper_core;
	return P.console_new ? 2 : -2;
}

static bool low(void) { return profile_quality() == QUALITY_LOW; }

bool profile_tex_half(void)
{
	if (P.tex_half) return P.tex_half == 1;
	return __ctru_linear_heap_size < 32 * MB;
}
u32 profile_texcache_budget(void) { return __ctru_linear_heap_size >= 80 * MB ? 12 * MB : 6 * MB; }
int profile_max_neighbours(void) { return low() ? 2 : 4; }
float profile_stream_dist(void) { return low() ? 5000.0f : 7000.0f; }
int profile_fog_sprites(void) { return low() ? 72 : 144; }
bool profile_shadows(void) { return !low(); }
int profile_chr_point_lights(void) { return low() ? 2 : 4; }
int profile_movie_scale(void) { return low() ? 2 : 1; }

int profile_frame_rate(void)
{
	if (P.fps == CHOICE_LOW_OLD) return 30;
	if (P.fps == CHOICE_HIGH_NEW) return 60;
	return low() ? 30 : 60;
}

void profile_apply_frame_rate(void) { C3D_FrameRate((float)profile_frame_rate()); }

void profile_apply_quality(void)
{
#if __has_include("quality.h")
	Quality q = *quality();
	q.fog_sprites = profile_fog_sprites();
	q.stencil_shadows = q.drop_shadows = profile_shadows();
	q.tile_distance = profile_stream_dist();
	q.frame_cap = P.fps == CHOICE_HIGH_NEW ? 60 : profile_frame_rate() == 30 ? 30 : 0;
	quality_set(&q);
#endif
}

// ---------------------------------------------------------------- options

void profile_set_choices(int hw, int quality, int layout, int fps)
{
	P.hw = clamp_choice(hw), P.quality = clamp_choice(quality);
	P.layout = clamp_choice(layout), P.fps = clamp_choice(fps);
	profile_apply_frame_rate();
	profile_apply_quality();
}

const char* profile_status(void)
{
	snprintf(P.status, sizeof P.status, "%s 3DS, profile %s, %s quality, %s layout, %d fps%s",
	         P.console_new ? "New" : "Old", profile_new() ? "new" : "old", low() ? "low" : "high",
	         controls_layout() == CONTROLS_LAYOUT_NEW ? "new" : "old", profile_frame_rate(), P.core1 ? ", core 1" : "");
	return P.status;
}

