#include "bgm.h"
#include "audio.h"
#include "events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define TRACKS 7
#define HALF_W 50.0f           // _SOUND_WORK.half_w: the player counts as a 100-unit square

typedef struct { s32 load_id, type; char path[32]; } BgmFile;
typedef struct { s32 load_id, start_id; } BgmPage;
typedef struct { u32 offset, count; } BgmStage;
// PS2 _SOUND_DATA: area x0..z2 (only x0, z0, x2, z2 are tested), Y window, flags (bit n: page n
// has its own 5 bits per track), chanstat per track (on, fade-in type, fade-out type)
typedef struct { float x0, z0, x1, z1, x2, z2, max_y, min_y; s32 flags; u32 chanstat[TRACKS]; } SoundArea;
typedef struct { u32 def, n_exact, n_block; const u16* cells; } GlbRooms;

enum { TR_OFF, TR_IN, TR_ON, TR_OUT };
typedef struct { u8 status, fade_in, fade_out; float vol; } Track;

static u8* s_blob;
static u32 s_nfiles, s_npages, s_nstages, s_nglb;
static const BgmFile* s_files;
static const BgmPage* s_pages;
static const BgmStage* s_stages;
static GlbRooms s_glb[32];
static bool s_ok;

static int s_stage = -1, s_room = -1;
static int s_page, s_playing_page;   // page wanted, page whose file is loaded (0 none)
static bool s_hold;                 // file chosen but the stage program starts it (stage 15)
static float s_master = 0.4f;
static int s_situation;
static Track s_tracks[TRACKS];
static char s_status[64] = "bgm off";

// 0x8a7764 fade times per type (4 is instant) and 0x8a7750 priorities when areas disagree
static const float fade_time[5] = { 0.0f, 3.0f, 1.5f, 6.0f, 0.0f };
static const int fade_prio[5] = { 0, 2, 3, 1, 4 };

bool bgm_init(void)
{
	FILE* f = fopen(PACK_DIR "/sound/bgm.bin", "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	s_blob = malloc(n);
	if (!s_blob || fread(s_blob, 1, n, f) != (size_t)n) {
		fclose(f);
		free(s_blob);
		s_blob = NULL;
		return false;
	}
	fclose(f);
	u32* h = (u32*)s_blob;
	if (memcmp(h, "S3BG", 4) || h[1] != 1) {
		free(s_blob);
		s_blob = NULL;
		return false;
	}
	s_nfiles = h[2], s_npages = h[3], s_nstages = h[4], s_nglb = h[5];
	u8* p = s_blob + 24;
	s_files = (const BgmFile*)p;
	p += s_nfiles * sizeof(BgmFile);
	s_pages = (const BgmPage*)p;
	p += s_npages * sizeof(BgmPage);
	s_stages = (const BgmStage*)p;
	p += s_nstages * sizeof(BgmStage);
	for (u32 i = 0; i < s_nglb && i < 32; i++) {
		u32* g = (u32*)p;
		s_glb[i].def = g[0], s_glb[i].n_exact = g[1], s_glb[i].n_block = g[2];
		s_glb[i].cells = (const u16*)(p + 12);
		p += 12 + 4 * (g[1] + g[2]);
	}
	s_ok = true;
	snprintf(s_status, sizeof s_status, "bgm ready");
	return true;
}

void bgm_exit(void)
{
	free(s_blob);
	s_blob = NULL;
	s_ok = false;
}

bool bgm_ready(void) { return s_ok; }
void bgm_set_situation(int page) { s_situation = page; }

// FUN_00494db0 via tools/sh2conv/sh2rooms.py: 20000-unit cells, exact cells first, then the
// even corner of a 2x2 block, else the glb's default room.
int bgm_room_id(int glb, float x, float z)
{
	if (!s_ok || glb < 0 || (u32)glb >= s_nglb)
		return 0;
	const GlbRooms* g = &s_glb[glb];
	int cell = (int)(x * 5e-5f + 16.0f) * 32 + (int)(z * 5e-5f + 16.0f);
	int block = (cell - (cell & 0x20)) - (cell & 1);
	for (u32 i = 0; i < g->n_exact; i++)
		if (g->cells[2 * i] == cell)
			return g->cells[2 * i + 1];
	for (u32 i = 0; i < g->n_block; i++)
		if (g->cells[2 * (g->n_exact + i)] == block)
			return g->cells[2 * (g->n_exact + i) + 1];
	return g->def;
}

// SeBgmChange (0x515c20). Flag n is game_flag word n >> 5, bit n & 31 (gGameFlag at 0x1f7d5e0).
static int pick_page(int stage, int room)
{
#define F(n) ev_flag(n)
	switch (stage) {
	case 2:
		return F(39) ? 0 : F(33) ? 0x2f : 0x2e;
	case 3:
	case 4:
		if (F(39) || F(618) || F(619))
			return 0;
		return F(36) ? 3 : 0x2f;
	case 5:
		return 3;
	case 6:
		if (!F(85) && room == 0x1e)
			return 7;
		return F(91) ? 9 : 4;
	case 7:
	case 8:
		return F(91) ? 9 : 5;
	case 9:
		return F(91) ? 9 : 6;
	case 10:
		if (!F(109) && room == 0x24)
			return 8;
		return 10;
	case 0xb:
	case 0x3a:
		return 10;
	case 0xc:
		return 0xc;
	case 0xe:
		if (F(619))
			return 0;
		return F(151) && !F(152) ? 0xd : 0xb;
	case 0xf:
		return 0xe;
	case 0x10:
	case 0x36:
		return 0xb;
	case 0x11:
	case 0x35:
		return 0xf;
	case 0x12:
	case 0x13:
	case 0x15:
		return 0x10;
	case 0x14:
		return 0x11;
	case 0x16:
		if (room == 0x34)
			return 0x12;
		if (room == 0x30)
			return 0x13;
		return 0x14;
	case 0x18:
	case 0x1a:
		return 0x14;
	case 0x17:
		return 0x17;
	case 0x19:
		return 0x15;
	case 0x1b:
		return 0x16;
	case 0x1d:
	case 0x1e:
		return 0x18;
	case 0x1f:
	case 0x20:
		return 0x19;
	case 0x21:
		return 0x1a;
	case 0x22:
		return 0x1b;
	case 0x23:
		return room == 0x8a && !F(335) ? 0x1c : 0x1b;
	case 0x24:
		return room == 0x89 || F(368) ? 0x1e : 0x1b;
	case 0x25:
		return 0x1f;
	case 0x26:
		return 0x20;
	case 0x27:
		return 0x23;
	case 0x28:
		return F(472) ? 0x22 : 0x23;
	case 0x29:
		return 0x24;
	case 0x2a:
		if (room != 0xa2)
			return 0x24;
		return F(527) ? 0x25 : 0;
	case 0x2b:
	case 0x2c:
		if (room == 0xac)
			return 0x27;
		return F(482) ? 0x2c : 0x26;
	case 0x2d:
	case 0x2e:
		return 0x28;
	case 0x2f:
		return 0x29;
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
		return 0x2a;
	case 0x37:
	case 0x38:
	case 0x39:
		return 0x2d;
	default:
		return 0;
	}
#undef F
}

// SeBgmManager's per-file master volume (0x8a73b4), keyed by the AIX/ADX start id.
static float master_for(int start_id)
{
	switch (start_id) {
	case 50115: case 50101: case 50106: return 0.5f;
	case 50021: case 50003: case 50017: return 0.25f;
	case 50013: return 0.3f;
	case 50102: case 50105: return 0.45f;
	case 50125: case 50124: case 50016: return 0.55f;
	case 50004: return 0.9f;
	case 50001: return 0.7f;
	case 50104: return 0.35f;
	default: return 0.4f;
	}
}

static const char* page_path(int page)
{
	if (page <= 0 || (u32)page >= s_npages)
		return NULL;
	for (u32 i = 0; i < s_nfiles; i++)
		if (s_files[i].load_id == s_pages[page].load_id)
			return s_files[i].path;
	return NULL;
}

static void start_page(int page)
{
	s_playing_page = 0;
	audio_stop_music();
	memset(s_tracks, 0, sizeof s_tracks);
	const char* rel = page_path(page);
	if (!rel || s_hold)
		return;
	char path[96];
	snprintf(path, sizeof path, PACK_DIR "/sound/adx/%s", rel);
	if (!audio_play_layers(path, 1.0f))       // tracks start silent and fade in from the areas
		return;
	s_master = master_for(s_pages[page].start_id);
	s_playing_page = page;
}

// sndGetSoundAryByCharaPos: which tracks the areas around the player switch on, and how they
// fade. A track's bits come from the page's own 5 bits when the area's flags have that page.
static void area_tracks(int stage, const float pos[3], u8 in[TRACKS], u8 out[TRACKS])
{
	memset(in, 0, TRACKS);
	memset(out, 0, TRACKS);
	if (stage < 0 || (u32)stage >= s_nstages || !s_stages[stage].count)
		return;
	const SoundArea* a = (const SoundArea*)(s_blob + s_stages[stage].offset);
	u32 n = s_stages[stage].count;
	int page = s_situation;
	bool any = false;
	for (u32 r = 0; r <= n; r++) {
		const SoundArea* s;
		if (r < n) {
			if (r == 0)
				continue;                          // record 0 is the default
			s = &a[r];
			if (s->x0 > pos[0] + HALF_W || pos[0] - HALF_W > s->x2 || s->z2 > pos[2] + HALF_W ||
			    pos[2] - HALF_W > s->z0 || pos[1] > s->max_y || pos[1] < s->min_y)
				continue;
			any = true;
		} else {
			if (any)
				break;
			s = &a[0];
		}
		bool own = (s->flags >> page) & 1;
		int sh = own ? page * 5 : 0;
		for (int t = 0; t < TRACKS; t++) {
			u32 c = s->chanstat[t];
			if (!((c >> sh) & 1))
				continue;
			u8 fi = ((c >> (sh + 1)) & 3) + 1, fo = ((c >> (sh + 3)) & 3) + 1;
			if (fade_prio[fi] > fade_prio[in[t]])
				in[t] = fi;
			if (fade_prio[fo] > fade_prio[out[t]])
				out[t] = fo;
		}
	}
}

void bgm_stop(void)
{
	audio_stop_music();
	s_stage = s_room = -1;
	s_page = s_playing_page = 0;
	memset(s_tracks, 0, sizeof s_tracks);
}

void bgm_update(int stage, int glb, const float pos[3], float dt)
{
	if (!s_ok)
		return;
	int room = bgm_room_id(glb, pos[0], pos[2]);
	if (stage != s_stage || room != s_room) {
		s_stage = stage;
		s_room = room;
		s_page = pick_page(stage, room);
		s_hold = stage == 0xf && !ev_flag(159);    // the stage program starts this one
		// FUN_005155e0: a new page fades every sounding track out over 3 s first
		if (s_page != s_playing_page)
			for (int t = 0; t < TRACKS; t++)
				if (s_tracks[t].status != TR_OFF && !(s_tracks[t].status == TR_OUT && s_tracks[t].fade_out != 3))
					s_tracks[t].status = TR_OUT, s_tracks[t].fade_out = 1;
	}
	if (s_page != s_playing_page) {
		bool silent = true;
		for (int t = 0; t < TRACKS; t++)
			if (s_tracks[t].status != TR_OFF)
				silent = false;
		if (silent || !s_playing_page)
			start_page(s_page);
	} else if (s_playing_page) {
		u8 in[TRACKS], out[TRACKS];
		area_tracks(stage, pos, in, out);
		for (int t = 0; t < TRACKS; t++) {
			Track* tr = &s_tracks[t];
			if (!in[t]) {
				if (tr->status != TR_OFF)
					tr->status = TR_OUT;
			} else if (tr->status == TR_OFF || tr->status == TR_OUT) {
				tr->status = TR_IN;
				tr->fade_in = in[t];
				tr->fade_out = out[t];
			}
		}
	}
	int nl = audio_layer_count();
	for (int t = 0; t < TRACKS; t++) {
		Track* tr = &s_tracks[t];
		if (tr->status == TR_IN) {
			tr->vol = tr->fade_in == 4 ? 1.0f : tr->vol + dt / fade_time[tr->fade_in];
			if (tr->vol >= 1.0f)
				tr->vol = 1.0f, tr->status = TR_ON;
		} else if (tr->status == TR_OUT) {
			tr->vol = tr->fade_out == 4 ? 0.0f : tr->vol - dt / fade_time[tr->fade_out];
			if (tr->vol <= 0.0f)
				tr->vol = 0.0f, tr->status = TR_OFF;
		}
		if (t < nl)
			audio_set_layer_volume(t, tr->vol * s_master);
	}
	snprintf(s_status, sizeof s_status, "bgm st%d rm%#x pg%d %.2f %.2f %.2f %.2f", stage, room, s_playing_page,
	         s_tracks[0].vol, s_tracks[1].vol, s_tracks[2].vol, s_tracks[3].vol);
}

const char* bgm_status(void) { return s_status; }
