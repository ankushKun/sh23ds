// sh23ds: Silent Hill 2 (PC Director's Cut) on the New 3DS, from the user's own converted data.
//
// Title: D-pad choose, A select, START exit.
// Game: circle pad walk/turn, B run, A check / open / next message, X flashlight, START title,
// R (hold) ready the weapon and A attack (stomps a downed enemy at James' feet), L next weapon.
// Room viewer (debug): D-pad left/right room, up/down area, SELECT camera mode (orbit / fly /
// player), circle pad move, C-stick or Y/A/X/B look, L/R down/up, START title.
#include <3ds.h>
#include <citro3d.h>
#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tex3ds.h>
#include <malloc.h>
#include "audio.h"
#include "sfx.h"
#include "bgm.h"
#include "chr.h"
#include "demo.h"
#include "collide.h"
#include "combat.h"
#include "enemy.h"
#include "maria.h"
#include "events.h"
#include "envlight.h"
#include "fog.h"
#include "font.h"
#include "clearsave.h"
#include "savescreen.h"
#include "noise.h"
#include "blood.h"
#include "gameover.h"
#include "titlemenu.h"
#include "pausemenu.h"
#include "optmenu.h"
#include "stagemdl.h"
#include "touchui.h"
#include "controls.h"
#include "move.h"
#include "chrlight.h"
#include "shadow.h"
#include "quality.h"
#include "areaidx.h"
#include "stream.h"
#include "perf.h"
#include "profile.h"
#include "gamecam.h"
#include "game/view/sh2cam.h"
#include "map.h"
#include "movie.h"
#include "playtest.h"
#include "prog.h"
#include "room.h"
#include "skin_shbin.h"
#include "world_shbin.h"

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define CLEAR_COLOR 0x202428FF
#define MAX_AREAS 32
#define MAX_ROOMS 128

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uProjection, s_uModelView;
static DVLB_s* s_skin_dvlb;
static shaderProgram_s s_skin_prog;
static int s_uSkinProjection, s_uSkinModelView, s_uBones;

// Player (James)
static Character s_james;
static Anim s_james_anim;
static bool s_have_james;
static float s_px, s_pz, s_py, s_yaw;
static float s_anim_dt;                // this frame's time for James' crossfades (0 while paused)
static float s_vel[2], s_turn_speed;   // last frame's velocity (units/s) and yaw rate (rad/s), for the camera
static bool s_gamecam_reset = true;

static char s_areas[MAX_AREAS][8];
static int s_nareas, s_area;
static char s_rooms[MAX_ROOMS][16];
static int s_nrooms, s_room;

static Room s_cur;
static bool s_loaded;
static Collision s_col;
static bool s_have_col;
static int s_autotest;                // debug.txt "autotest" (autotest_frame)
static int s_debug_itemget[8], s_debug_nitemget;   // debug.txt "itemget <id>": ItemGet before "results"
static int s_debug_results;           // debug.txt "results 1": the results screen right after start (2: Born From a Wish)
static bool s_msg_top;                // options.txt "messagestop": messages and subtitles on the top screen
static Options s_opt;                 // options.txt, edited by the options screens (optmenu.c)
static char s_status[160];
static int s_render_mode;   // 0 textured, 1 flat color per group, 2 vertex color only, 3 texture test
static int s_only_batch = -1;   // debug: draw just this batch
static bool s_opaque_only;      // debug (X in orbit mode)
static float s_orbit_speed = 0.01f;
static int s_depth_variant;     // debug (Y in orbit mode): 0 GREATER/clear 0, 1 LESS/clear max, 2 ALWAYS
static void* s_testquad;    // linear memory, 6 ROOM_VERTEX_SIZE vertices

enum { CAM_ORBIT, CAM_FLY, CAM_PLAYER, CAM_MODES };
enum { ST_TITLE, ST_GAME, ST_VIEWER };
static int s_state = ST_TITLE;
static int s_spawn_stage;        // debug.txt "stage": overrides the spawn map's stage
static bool s_is_new3ds;
static float s_title_t;          // seconds on the title screen
static C3D_Tex s_title_tex;
static bool s_have_title;
static void* s_titlequad;        // linear memory, 6 ROOM_VERTEX_SIZE vertices
typedef struct {
	int mode;
	float eye[3], yaw, pitch, orbit;
} Camera;
static Camera s_cam;

static void env_room_changed(void);   // lighting section below
static bool james_can_see(void);       // lighting section below

// Events: pending door warp (applied at the darkest point of the fade) and message pages
static bool s_keep_player;       // load_current keeps James where a warp put him
static EvAction s_warp;
static bool s_warp_pending;
static float s_fade;             // 0 clear .. 1 black
static float s_fade_target;      // where the fade goes when no warp is pending (stage programs)
static const char* s_msg_page;   // current page of the shown message, NULL when none
static bool s_msg_dirty;
static int s_last_event = -1;
static bool s_save_after_msg;    // a save point's message is up; prompt when it closes
static bool s_save_prompt;
static int s_slot;               // the save file Continue loads and saves go to (savescreen.c)
static int s_save_place = -1;    // the save point last used (its place name in the file list)
static bool s_debug_hud, s_hud_shown;
#define SAVE_PATH PACK_DIR "/save.dat"
static void* s_fadequad;         // linear memory, 6 ROOM_VERTEX_SIZE vertices
static char s_panel[512];        // stage program panel (choices, puzzles), empty when none
static bool s_pt_shot, s_pt_quit;   // playtest driver: dump this frame, leave the main loop
static bool s_free_voice;        // demo_voice_free's track is playing (its subtitles show)
static int s_use_item;           // item chosen with "use" in the inventory, for the next event check
static int s_riddle = 1, s_action = 2;   // new game levels (debug.txt "riddle" / "action")
static char s_spawn_map[8];      // debug.txt "spawn"
static char s_debug_movie[24];   // debug.txt "movie <name>": played before the title
static char s_debug_demo[32];    // debug.txt "demo <key> [frame]": cutscene played after the spawn
static float s_debug_demo_at;
static float s_spawn[4];
static int s_spawn_mode;

static void list_areas(void)
{
	s_nareas = 0;
	DIR* d = opendir(PACK_DIR "/bg");
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)) && s_nareas < MAX_AREAS)
		if (e->d_name[0] != '.' && strlen(e->d_name) < 8)
			strcpy(s_areas[s_nareas++], e->d_name);
	closedir(d);
	// simple sort for a stable order
	for (int i = 0; i < s_nareas; i++)
		for (int j = i + 1; j < s_nareas; j++)
			if (strcmp(s_areas[j], s_areas[i]) < 0) {
				char t[8];
				strcpy(t, s_areas[i]);
				strcpy(s_areas[i], s_areas[j]);
				strcpy(s_areas[j], t);
			}
}

static void room_bounds_load(void);

static void list_rooms(void)
{
	s_nrooms = 0;
	char path[128];
	snprintf(path, sizeof path, PACK_DIR "/bg/%s/rooms.txt", s_areas[s_area]);
	long n = 0;
	char* text = stream_take_file(path, &n);   // read ahead for a door into another area
	if (text) {
		char* t2 = realloc(text, n + 1);
		if (t2) {
			text = t2;
			text[n] = 0;
			for (char* line = strtok(text, "\r\n"); line && s_nrooms < MAX_ROOMS; line = strtok(NULL, "\r\n"))
				if (line[0] && strlen(line) < 16)
					strcpy(s_rooms[s_nrooms++], line);
		}
		free(text);
	} else {
		FILE* f = fopen(path, "r");
		if (!f)
			return;
		char line[64];
		while (fgets(line, sizeof line, f) && s_nrooms < MAX_ROOMS) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[0] && strlen(line) < 16)
				strcpy(s_rooms[s_nrooms++], line);
		}
		fclose(f);
	}
	room_bounds_load();
}

// ---------------------------------------------------------------- neighbouring rooms (open areas)
//
// The town, the forest and the lake are continuous grids of map tiles with no doors between them;
// the PC keeps the blocks around James resident (up to 8, 0x512d60). The port keeps the touching
// tiles within fog distance resident too: drawn, and merged into one collision set. The primary
// room (s_cur) is the one James stands in; it gives the light record and footstep materials.

#define MAX_NB 4
// how near a touching tile has to be to load, and how many stay resident: profile.c (quality level)
static Room s_nb[MAX_NB];
static Collision s_nb_col[MAX_NB];
static int s_nb_idx[MAX_NB];         // index into s_rooms
static int s_nnb;
static Collision s_col_room;         // the primary room's own collision; s_col is the merged set
static float s_room_bb[MAX_ROOMS][6];

static void room_bounds_load(void)
{
	bool idx = areaidx_load(PACK_DIR, s_areas[s_area]);   // one file for the whole area
	for (int i = 0; i < s_nrooms; i++) {
		if (idx && areaidx_bounds(s_rooms[i], s_room_bb[i]))
			continue;
		char path[192];
		snprintf(path, sizeof path, PACK_DIR "/bg/%.7s/%.15s.s3m", s_areas[s_area], s_rooms[i]);
		memset(s_room_bb[i], 0, sizeof s_room_bb[i]);
		FILE* f = fopen(path, "rb");
		if (!f)
			continue;
		if (fseek(f, 8, SEEK_SET) || fread(s_room_bb[i], sizeof s_room_bb[i], 1, f) != 1)
			memset(s_room_bb[i], 0, sizeof s_room_bb[i]);
		fclose(f);
	}
}

static bool room_has_bounds(int i) { return s_room_bb[i][3] > s_room_bb[i][0] && s_room_bb[i][5] > s_room_bb[i][2]; }

static bool rooms_touch(int i, int j)
{
	const float* a = s_room_bb[i];
	const float* b = s_room_bb[j];
	if (!room_has_bounds(i) || !room_has_bounds(j))
		return false;
	float gx = fmaxf(a[0], b[0]) - fminf(a[3], b[3]), gz = fmaxf(a[2], b[2]) - fminf(a[5], b[5]);
	return gx < 100.0f && gz < 100.0f;
}

static float room_dist(int i, float x, float z)
{
	const float* b = s_room_bb[i];
	float dx = x < b[0] ? b[0] - x : x > b[3] ? x - b[3] : 0.0f;
	float dz = z < b[2] ? b[2] - z : z > b[5] ? z - b[5] : 0.0f;
	return sqrtf(dx * dx + dz * dz);
}

static void collision_rebuild(void)
{
	const Collision* parts[1 + MAX_NB];
	int n = 0;
	if (s_col_room.n_floors || s_col_room.n_walls)
		parts[n++] = &s_col_room;
	for (int i = 0; i < s_nnb; i++)
		parts[n++] = &s_nb_col[i];
	s_have_col = n > 0 && col_merge(&s_col, parts, n);
}

static void residents_keep(void)
{
	const Room* rooms[1 + MAX_NB];
	int n = 0;
	if (s_loaded)
		rooms[n++] = &s_cur;
	for (int i = 0; i < s_nnb; i++)
		rooms[n++] = &s_nb[i];
	texcache_keep(rooms, n);
}

static void neighbour_free(int k)
{
	room_free(&s_nb[k]);
	col_free(&s_nb_col[k]);
	s_nb[k] = s_nb[s_nnb - 1];
	s_nb_col[k] = s_nb_col[s_nnb - 1];
	s_nb_idx[k] = s_nb_idx[s_nnb - 1];
	s_nnb--;
}

// Tiles dropped while walking wait here until the next C3D_FrameBegin has waited for the GPU,
// which may still be drawing the last frame with them. Freeing them at once meant a C3D_FrameSync
// in the middle of the update, 13 ms of waiting and a dropped frame (port/PERFORMANCE.md). The
// texture cache trim waits with them, since it would delete their textures too.
static Room s_dead[MAX_NB];
static Collision s_dead_col[MAX_NB];
static int s_ndead;
static bool s_keep_due;

static void neighbour_retire(int k)
{
	if (s_ndead == MAX_NB) {             // cannot happen with MAX_NB tiles, but stay safe
		C3D_FrameSync();
		neighbour_free(k);
		return;
	}
	s_dead[s_ndead] = s_nb[k];
	s_dead_col[s_ndead++] = s_nb_col[k];
	s_nb[k] = s_nb[s_nnb - 1];
	s_nb_col[k] = s_nb_col[s_nnb - 1];
	s_nb_idx[k] = s_nb_idx[s_nnb - 1];
	s_nnb--;
}

// Right after the game's C3D_FrameBegin (SYNCDRAW: the previous frame is done).
static void neighbours_reap(void)
{
	while (s_ndead) {
		s_ndead--;
		room_free(&s_dead[s_ndead]);
		col_free(&s_dead_col[s_ndead]);
	}
	if (s_keep_due) {
		s_keep_due = false;
		residents_keep();
	}
}

// A tile from the reader thread whose textures go into the cache a few a frame before it joins
static StreamJob* s_join;
static bool s_join_warm;             // its textures are all in the cache

static void neighbours_clear(void)
{
	if (s_join) {
		stream_release(s_join);
		s_join = NULL;
		s_join_warm = false;
	}
	if (s_nnb || s_ndead)
		C3D_FrameSync();     // the GPU may still be reading them
	while (s_nnb)
		neighbour_free(s_nnb - 1);
	neighbours_reap();
}

// Whether a tile the reader thread has read is still wanted.
static bool neighbour_job_wanted(const StreamJob* j, const int* want, int nwant)
{
	bool wanted = !strcmp(j->area, s_areas[s_area]) && s_nnb < MAX_NB;
	for (int k = 0; k < s_nnb && wanted; k++)
		wanted = s_nb_idx[k] != j->key;
	bool listed = false;
	for (int i = 0; i < nwant; i++)
		listed |= want[i] == j->key;
	return wanted && listed;
}

// Adds a tile the reader thread has read (stream.c); false when it is not wanted any more.
static bool neighbour_add_job(StreamJob* j, const int* want, int nwant)
{
	bool wanted = neighbour_job_wanted(j, want, nwant), listed = wanted;
	char err[128];
	int k = s_nnb;
	u64 t0 = svcGetSystemTick();
	if (!wanted || !listed || !j->s3m ||
	    !room_load_mem(&s_nb[k], PACK_DIR, j->area, j->name, j->s3m, j->s3m_size, j->tex, j->ntex, err, sizeof err)) {
		if (wanted && listed)
			j->s3m = NULL;               // room_load_mem freed it on failure
		stream_release(j);
		return false;
	}
	j->s3m = NULL;                       // room_load_mem took it
	col_load_mem(&s_nb_col[k], j->s3l, j->s3l_size);
	sfx_floor_put(j->name, j->sfm, j->sfm_size);   // NULL too: a tile without one is not looked for again
	j->sfm = NULL;
	s_nb_idx[k] = j->key;
	s_nnb++;
	perf_event(j->name, "tile joined: read %.1f ms on the reader thread, %d textures; %.1f ms on the main loop",
	           perf_ms(j->read_ticks), j->ntex, perf_ms(svcGetSystemTick() - t0));
	stream_release(j);
	return true;
}

static bool s_stream_sync;           // debug.txt "streamsync 1": read tiles on the main loop (for comparison)
// A door warp's neighbouring tiles come in through the reader thread while the screen stays black
// (fade_update holds the fade until they are resident, at most ARRIVAL_MAX seconds), instead of
// being read in the one long frame of the room load (port/PERFORMANCE.md).
static bool s_async_arrival;         // set by apply_warp around its room load
static bool s_arrival;               // tiles of the last warp still on their way
static float s_arrival_t;
static int s_tiles_missing;          // wanted touching tiles that are not resident yet
#define ARRIVAL_MAX 3.0f

// Loads the touching tiles near James and drops the far ones. Returns true when the set changed.
// sync: read the files now (room loads); otherwise the reader thread fetches them and they join a
// few frames later, so walking never waits for the SD card.
static bool stream_update_ex(bool sync)
{
	if (s_state != ST_GAME || !s_loaded)
		return false;
	int want[MAX_NB], nwant = 0;
	float wdist[MAX_NB];
	for (int j = 0; j < s_nrooms; j++) {
		if (j == s_room || !rooms_touch(s_room, j))
			continue;
		float d = room_dist(j, s_px, s_pz);
		if (d > quality()->tile_distance)   // quality.h (profile.c sets it)
			continue;
		int k = nwant < profile_max_neighbours() ? nwant++ : -1;
		if (k < 0) {              // keep the nearest few (4 on the high quality level, profile.c)
			int far = 0;
			for (int i = 1; i < nwant; i++)
				if (wdist[i] > wdist[far]) far = i;
			if (wdist[far] <= d)
				continue;
			k = far;
		}
		want[k] = j, wdist[k] = d;
	}
	bool changed = false;
	for (int k = s_nnb - 1; k >= 0; k--) {
		bool keep = false;
		for (int i = 0; i < nwant; i++)
			keep |= want[i] == s_nb_idx[k];
		if (!keep) {
			neighbour_retire(k);         // freed after the next C3D_FrameBegin
			changed = true;
		}
	}
	if (!sync) {
		// A tile's textures go into the cache first, about 2 ms of them a frame, and its geometry
		// joins in the frame after the last one, so no single frame takes all of a tile.
		u64 t0 = svcGetSystemTick();
		while (s_join || (s_join = stream_take()) != NULL) {
			if (!neighbour_job_wanted(s_join, want, nwant) || !s_join->s3m) {
				neighbour_add_job(s_join, want, nwant);   // hands it back
				s_join = NULL;
				continue;
			}
			if (!s_join_warm) {
				s_join_warm = room_tex_warm(PACK_DIR, s_join->area, s_join->tex, s_join->ntex,
				                            t0 + SYSCLOCK_ARM11 / 500);
				break;                   // the geometry in a frame of its own
			}
			changed |= neighbour_add_job(s_join, want, nwant);
			s_join = NULL;
			s_join_warm = false;
			break;                       // one tile a frame
		}
		perf_part("join", t0);
	}
	s_tiles_missing = 0;
	for (int i = 0; i < nwant && s_nnb < MAX_NB; i++) {
		bool have = false;
		for (int k = 0; k < s_nnb; k++)
			have |= s_nb_idx[k] == want[i];
		if (have)
			continue;
		if (!sync && stream_request(want[i], s_areas[s_area], s_rooms[want[i]])) {
			s_tiles_missing++;
			continue;                    // the reader thread fetches it
		}
		char err[128], cpath[192];
		int k = s_nnb;
		u64 t0 = svcGetSystemTick();
		if (!room_load(&s_nb[k], PACK_DIR, s_areas[s_area], s_rooms[want[i]], err, sizeof err))
			continue;
		snprintf(cpath, sizeof cpath, PACK_DIR "/bg/%.7s/%.15s.s3l", s_areas[s_area], s_rooms[want[i]]);
		col_load(&s_nb_col[k], cpath);
		perf_event(s_rooms[want[i]], "tile loaded on the main loop: %.1f ms", perf_ms(svcGetSystemTick() - t0));
		s_nb_idx[k] = want[i];
		s_nnb++;
		changed = true;
	}
	if (changed) {
		u64 t0 = svcGetSystemTick();
		collision_rebuild();
		perf_part("col", t0);
		t0 = svcGetSystemTick();
		if (s_ndead)
			s_keep_due = true;           // with the dropped tiles, after the GPU is done
		else
			residents_keep();
		perf_part("keep", t0);
	}
	return changed;
}


// James walked out of the primary tile into a resident neighbour: swap their roles, no reload.
static void primary_update(void)
{
	if (s_state != ST_GAME || !s_loaded || !s_nnb)
		return;
	if (room_dist(s_room, s_px, s_pz) == 0.0f)
		return;
	for (int k = 0; k < s_nnb; k++) {
		if (room_dist(s_nb_idx[k], s_px, s_pz) > 0.0f)
			continue;
		Room r = s_cur;
		s_cur = s_nb[k], s_nb[k] = r;
		Collision c = s_col_room;
		s_col_room = s_nb_col[k], s_nb_col[k] = c;
		int idx = s_room;
		s_room = s_nb_idx[k], s_nb_idx[k] = idx;
		snprintf(s_status, sizeof s_status, "%s/%s", s_areas[s_area], s_rooms[s_room]);
		u64 t0 = svcGetSystemTick();
		sfx_floor_load(s_rooms[s_room]);
		perf_part("floor", t0);
		t0 = svcGetSystemTick();
		env_room_changed();
		perf_part("env", t0);
		stream_update_ex(s_stream_sync);   // the new far side comes in through the reader thread
		return;
	}
}

static void camera_reset(void)
{
	s_cam.orbit = s_orbit_speed == 0.0f ? 0.2f : 0.0f;
	s_cam.eye[0] = (s_cur.bbox_min[0] + s_cur.bbox_max[0]) * 0.5f;
	s_cam.eye[1] = s_cur.bbox_max[1] - 0.62f * (s_cur.bbox_max[1] - s_cur.bbox_min[1]);   // Y points down
	s_cam.eye[2] = (s_cur.bbox_min[2] + s_cur.bbox_max[2]) * 0.5f;
	s_cam.yaw = 0.0f;
	s_cam.pitch = 0.0f;
	if (s_keep_player)
		return;
	// spawn James on walkable floor near the middle of the room (Y down: floors have the largest y)
	s_px = (s_cur.bbox_min[0] + s_cur.bbox_max[0]) * 0.5f;
	s_pz = (s_cur.bbox_min[2] + s_cur.bbox_max[2]) * 0.5f;
	s_py = s_cur.bbox_max[1];
	if (s_have_col)
		col_spawn(&s_col, s_px, s_pz, &s_px, &s_py, &s_pz);
}

static const char* area_music(const char* area)
{
	// Placeholder until the stages' bgm_control code is ported: one track per building.
	if (!strcmp(area, "ap")) return PACK_DIR "/sound/adx/apart/bgm_101.aix";
	if (!strcmp(area, "hp")) return PACK_DIR "/sound/adx/hospital/bgm_100.aix";
	if (!strcmp(area, "ps")) return PACK_DIR "/sound/adx/prison/bgm_107.aix";
	if (!strcmp(area, "rr") || !strcmp(area, "ru")) return PACK_DIR "/sound/adx/hotel/bgm_108.aix";
	if (!strcmp(area, "ma")) return PACK_DIR "/sound/adx/mansion/bgm_104.aix";
	if (!strcmp(area, "cd") || !strcmp(area, "ob")) return PACK_DIR "/sound/adx/forest/bgm_022.adx";
	if (area[0] == 'c') return PACK_DIR "/sound/adx/town/bgm_005.adx";
	return NULL;
}

static void load_current(void)
{
	neighbours_clear();
	stream_flush(s_nareas ? s_areas[s_area] : "");
	if (s_loaded) {
		C3D_FrameSync();     // the GPU may still be reading the old buffers
		room_free(&s_cur);
		s_loaded = false;
	}
	if (!s_nrooms) {
		snprintf(s_status, sizeof s_status, "no rooms in area %s", s_nareas ? s_areas[s_area] : "-");
		return;
	}
	char err[128];
	if (s_have_col) {
		col_free(&s_col);
		s_have_col = false;
	}
	char cpath[160];
	snprintf(cpath, sizeof cpath, PACK_DIR "/bg/%s/%s.s3l", s_areas[s_area], s_rooms[s_room]);
	u64 t0 = svcGetSystemTick();
	// the files the reader thread fetched while the door faded out (warp_prefetch), if any
	StreamJob* pre = stream_take_named(s_areas[s_area], s_rooms[s_room]);
	stream_drop_prefetch();
	perf_part("ld_wait", t0);
	t0 = svcGetSystemTick();
	col_free(&s_col_room);
	if (pre && pre->s3l)
		col_load_mem(&s_col_room, pre->s3l, pre->s3l_size);
	else
		col_load(&s_col_room, cpath);
	collision_rebuild();
	if (pre && pre->sfm) {
		sfx_floor_put(s_rooms[s_room], pre->sfm, pre->sfm_size);
		pre->sfm = NULL;
	}
	sfx_floor_load(s_rooms[s_room]);
	perf_part("ld_col", t0);
	t0 = svcGetSystemTick();
	if (pre && pre->s3m) {
		s_loaded = room_load_mem(&s_cur, PACK_DIR, s_areas[s_area], s_rooms[s_room], pre->s3m, pre->s3m_size,
		                         pre->tex, pre->ntex, err, sizeof err);
		pre->s3m = NULL;                 // room_load_mem took it
	} else {
		s_loaded = room_load(&s_cur, PACK_DIR, s_areas[s_area], s_rooms[s_room], err, sizeof err);
	}
	if (pre)
		stream_release(pre);
	perf_part("ld_room", t0);
	if (s_loaded) {
		snprintf(s_status, sizeof s_status, "%s/%s", s_areas[s_area], s_rooms[s_room]);
		camera_reset();
		t0 = svcGetSystemTick();
		// touching tiles of open areas, before enemies are placed on them; after a door warp they
		// come in behind the black screen instead (s_arrival)
		bool async = s_async_arrival && !s_stream_sync;
		if (!stream_update_ex(!async))
			residents_keep();
		if (async && s_tiles_missing) {
			s_arrival = true;
			s_arrival_t = 0.0f;
		}
		perf_part("ld_tiles", t0);
		t0 = svcGetSystemTick();
		enemy_room_enter(ev_stage(), s_have_col ? &s_col : NULL);
		blood_clear();
		perf_part("ld_enemy", t0);
		t0 = svcGetSystemTick();
		gamecam_load_area(PACK_DIR, s_areas[s_area], (const char (*)[16])s_rooms, s_nrooms);
		perf_part("ld_cam", t0);
		t0 = svcGetSystemTick();
		env_room_changed();
		perf_part("ld_env", t0);
		mem_log(s_status);
		static char playing_area[8];
		if (!bgm_ready() && strcmp(playing_area, s_areas[s_area])) {
			snprintf(playing_area, sizeof playing_area, "%s", s_areas[s_area]);
			const char* m = area_music(s_areas[s_area]);
			if (m) audio_play_music(m, 0.8f);
			else audio_stop_music();
		}
	} else {
		snprintf(s_status, sizeof s_status, "%s", err);
	}
}

// Loads the room named by a bg map ("ap38" is area "ap"), keeping the player position when asked.
static bool goto_map(const char* map, bool keep_player)
{
	int area = -1;
	for (int i = 0; i < s_nareas; i++)
		if (!strncmp(s_areas[i], map, strlen(s_areas[i])))
			area = i;
	if (area < 0)
		return false;
	if (area != s_area) {
		s_area = area;
		u64 t0 = svcGetSystemTick();
		list_rooms();
		perf_part("ld_list", t0);
	}
	int room = -1;
	for (int i = 0; i < s_nrooms; i++)
		if (!strcmp(s_rooms[i], map))
			room = i;
	if (room < 0)
		return false;
	s_room = room;
	s_keep_player = keep_player;
	load_current();
	s_keep_player = false;
	return s_loaded;
}

// Stage for the room being shown (the viewer can jump anywhere; doors set it directly).
static void stage_for_room(void)
{
	if (!s_nrooms)
		return;
	int st = ev_stage_for_map(s_rooms[s_room]);
	if (st && st != ev_stage())
		ev_load_stage(st);
}

static void show_message(const char* text)
{
	text = ev_msg_expand(text);      // ff e0..ff e3 inserted values, ff f6 dropped
	s_msg_page = text && *text ? text : NULL;
	playtest_message(s_msg_page);
	s_msg_dirty = true;
}

// Page through the message with A; returns true while a message is up (player frozen).
static bool message_update(u32 down)
{
	if (!s_msg_page)
		return false;
	if (down & KEY_A) {
		const char* next = strchr(s_msg_page, '\f');
		show_message(next ? next + 1 : NULL);
		if (!s_msg_page && s_save_after_msg) {
			s_save_after_msg = false;
			s_save_prompt = true;
			sfx_play(SFX_SAVE_POINT, 1.0f, 0.0f);
			s_msg_dirty = true;
		}
	}
	return true;
}

static int room_at(float x, float z);

// A door warp's destination room: its files are read on the tile reader thread while the screen
// fades out, and load_current takes them from there (stream_take_named).
static int map_area(const char* map)
{
	int area = -1;
	for (int i = 0; i < s_nareas; i++)
		if (!strncmp(s_areas[i], map, strlen(s_areas[i])))
			area = i;
	return area;
}

static float s_warp_wait;            // time the black screen has waited for the prefetch

// ...and, when the stage changes, its event files and the enemy models it needs (they used to be
// read in the black frame of the load, 60..120 and up to 400 ms)
static void warp_prefetch(const char* map, int stage, const float* pos)
{
	s_warp_wait = 0.0f;
	if (s_stream_sync)
		return;
	int area = map[0] ? map_area(map) : -1;
	if (area >= 0)
		stream_request(STREAM_PREFETCH, s_areas[area], map);
	if (area >= 0 && area != s_area) {
		char path[96];
		snprintf(path, sizeof path, PACK_DIR "/bg/%s/rooms.txt", s_areas[area]);
		stream_want_file(path);
		snprintf(path, sizeof path, PACK_DIR "/bg/%s/area.idx", s_areas[area]);
		stream_want_file(path);
	}
	if (stage != ev_stage()) {
		char path[96];
		snprintf(path, sizeof path, PACK_DIR "/events/s%02d.s3e", stage);
		stream_want_file(path);
		snprintf(path, sizeof path, PACK_DIR "/events/s%02d.msg", stage);
		stream_want_file(path);
	}
	enemy_prefetch_stage(stage, stream_want_file);
	stagemdl_prefetch(stage, pos[0], pos[2], stream_want_file);   // the models placed near the arrival
}

// The black screen waits (frames keep coming) until the prefetch is in, at most 3 seconds, so the
// room load itself only does the GPU side.
static bool warp_ready(float dt)
{
	int area = s_warp.map[0] ? map_area(s_warp.map) : -1;
	bool busy = (area >= 0 && stream_named_busy(s_areas[area], s_warp.map)) || stream_files_busy();
	if (s_warp_wait >= 3.0f || !busy)
		return true;
	s_warp_wait += dt;
	return false;
}

// An open-area door's tile: the PC loads the destination stage's own blocks (table 0x8885a0 by
// Stage_Data.glb_crd: 1 ca, 2 cb, 3 cc, 4 and 17 cd) and stands James on the one under the arrival
// point. The pack's door records name the tile its converter found there in any folder (Blue Creek's
// way out to town west says cb02, town east's flat copy of the square walled at x 20000, where the
// PC has cc11's street): the tile of the stage's own folder under the point.
static void warp_area_tile(void)
{
	int glb = ev_glb_crd();
	const char* f = glb == 1 ? "ca" : glb == 2 ? "cb" : glb == 3 ? "cc" : glb == 4 || glb == 17 ? "cd" : NULL;
	const char* m = s_warp.map;
	if (!f || m[0] != 'c' || m[1] < 'a' || m[1] > 'd' || !strncmp(m, f, 2) || (s_warp.pos[0] == 0.0f && s_warp.pos[2] == 0.0f))
		return;
	char path[96], line[32];
	snprintf(path, sizeof path, PACK_DIR "/bg/%s/rooms.txt", f);
	FILE* rf = fopen(path, "r");
	if (!rf)
		return;
	while (fgets(line, sizeof line, rf)) {
		line[strcspn(line, "\r\n")] = 0;
		snprintf(path, sizeof path, PACK_DIR "/bg/%s/%.15s.s3m", f, line);
		FILE* mf = fopen(path, "rb");
		float bb[6];
		bool ok = mf && fseek(mf, 8, SEEK_SET) == 0 && fread(bb, sizeof bb, 1, mf) == 1;
		if (mf)
			fclose(mf);
		if (ok && s_warp.pos[0] >= bb[0] && s_warp.pos[0] <= bb[3] && s_warp.pos[2] >= bb[2] && s_warp.pos[2] <= bb[5]) {
			snprintf(s_warp.map, sizeof s_warp.map, "%.7s", line);
			break;
		}
	}
	fclose(rf);
}

static void apply_warp(void)
{
	s_gamecam_reset = true;
	s_async_arrival = true;
	u64 t0 = svcGetSystemTick();
	if (s_warp.stage != ev_stage())
		ev_load_stage(s_warp.stage);
	warp_area_tile();
	perf_part("ld_stage", t0);
	s_px = s_warp.pos[0];
	s_py = s_warp.pos[1];
	s_pz = s_warp.pos[2];
	if (!s_warp.keep_facing)
		s_yaw = s_warp.facing;
	char area_map[8];
	if (s_warp.map[0]) {
		if (!goto_map(s_warp.map, true))
			load_current();
	} else if (ev_map_of_stage(s_warp.stage, area_map) && goto_map(area_map, true)) {
		int r = room_at(s_warp.pos[0], s_warp.pos[2]);
		if (r >= 0 && r != s_room) {
			s_room = r;
			s_keep_player = true;
			load_current();
			s_keep_player = false;
		}
	} else {
		load_current();
	}
	if (s_have_col) {
		float ny;
		if (col_floor(&s_col, s_px, s_pz, s_py, 600.0f, &ny))
			s_py = ny;
		else
			col_spawn(&s_col, s_px, s_pz, &s_px, &s_py, &s_pz);
	}
	s_async_arrival = false;
	u64 ts = svcGetSystemTick();
	stagemdl_preload(ev_stage(), s_px, s_pz);   // the placed models in range, behind the fade
	perf_part("ld_stagemdl", ts);
	stream_drop_files();                 // read ahead for enemies that did not come after all
}

// debug.txt "memtest N": warps through ten rooms of different areas N times, one every 150 frames
// through the same fade and prefetch as a door, logging the free memory after each, so a leak
// across room changes shows as a falling column in mem.txt between rounds.
static int s_memtest, s_memtest_step;
static bool s_perflog, s_memlog;

static void memtest_step(u32 frame)
{
	static const struct { int stage; const char* map; float x, z, yaw; } dest[10] = {
		{ 6, "ap06", -49648, -66025, 90 }, { 5, "cb03", 17893, 18124, 166 }, { 2, "er05", 9950, 9100, 0 },
		{ 15, "bw05", -17537, 19000, -90 }, { 37, "ps189", 100000, -101140, 0 }, { 42, "rr91", 60000, 21975, 0 },
		{ 6, "ap38", -20024, 101015, 180 }, { 27, "hp230", 19600, -260505, 0 }, { 14, "ca14", -10000, -8000, 0 },
		{ 40, "rr49", -130000, 59250, 0 },
	};
	if (s_memtest <= 0 || s_state != ST_GAME || frame < 120 || frame % 150)
		return;
	if (s_memtest_step >= s_memtest * 10) {
		if (s_memtest_step++ == s_memtest * 10)
			mem_log("memtest done");
		return;
	}
	int i = s_memtest_step++ % 10;
	memset(&s_warp, 0, sizeof s_warp);
	s_warp.kind = EV_WARP;
	s_warp.stage = dest[i].stage;
	snprintf(s_warp.map, sizeof s_warp.map, "%s", dest[i].map);
	s_warp.pos[0] = dest[i].x, s_warp.pos[2] = dest[i].z;
	s_warp.facing = dest[i].yaw * (float)M_PI / 180.0f;
	warp_prefetch(s_warp.map, s_warp.stage, s_warp.pos);   // the way a door goes: prefetch, fade out, load, fade in
	s_warp_pending = true;
}

// ---------------------------------------------------------------- inventory (bottom screen)

static bool s_inv_open;
static int s_inv_sel;
static int s_inv_mark[2];        // items marked with R to combine with the next one (0 = none)
static bool s_inv_dirty;

static int inv_list(int* items, int max)
{
	int n = 0;
	for (int i = 1; i < 256 && n < max; i++)
		if (i < 250 && (ev_item_count(i) > 0 || combat_owns_item(i)))
			items[n++] = i;
	return n;
}

// Y opens and closes the item list; D-pad picks, A examines (the game's own description).
// The item list's "Use" (the touch menu calls it too): item alone, or with up to two marked items
// (0 = none) as the PC's "Combine" (0x46a790 / 0x49ea00): the combination's result id is used like
// an item by the next event check (trigger kinds 5, 6, 10), or "There's no use for just this." when
// the items do not combine. Closes the list.
static void inventory_use(int item, int mark0, int mark1)
{
	s_inv_open = false;
	s_inv_dirty = true;
	s_inv_mark[0] = s_inv_mark[1] = 0;
	if (mark0 || mark1) {
		int combo = ev_combine(item, mark0, mark1);
		if (combo)
			s_use_item = combo;
		else
			show_message(ev_item_message(17));
	} else {
		s_use_item = item;
	}
}

static bool inventory_update(u32 down)
{
	if (!s_inv_open) {
		if (down & KEY_Y) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			s_inv_open = true;
			s_inv_sel = 0;
			s_inv_mark[0] = s_inv_mark[1] = 0;
			s_inv_dirty = true;
		}
		return false;
	}
	int items[64];
	int n = inv_list(items, 64);
	if (down & (KEY_Y | KEY_B)) {
		sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		s_inv_open = false;
		s_inv_dirty = true;
		return true;
	}
	if (n && (down & (KEY_DUP | KEY_DDOWN))) {
		s_inv_sel = (s_inv_sel + (down & KEY_DDOWN ? 1 : n - 1)) % n;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		s_inv_dirty = true;
	}
	if (n && (down & KEY_X)) {
		s_inv_open = false;
		s_inv_dirty = true;
		prog_score_examine(items[s_inv_sel]);
		show_message(ev_item_desc(items[s_inv_sel]));
	}
	if (n && (down & KEY_R)) {
		// "Combine" (PC item screen 0x46a790): R marks up to two items, A then uses them together
		int it = items[s_inv_sel];
		if (s_inv_mark[0] == it || s_inv_mark[1] == it)
			s_inv_mark[s_inv_mark[0] == it ? 0 : 1] = 0;
		else if (!s_inv_mark[0] || !s_inv_mark[1])
			s_inv_mark[s_inv_mark[0] ? 1 : 0] = it;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		s_inv_dirty = true;
	}
	if (n && (down & KEY_A)) {
		int it = items[s_inv_sel];
		inventory_use(it, s_inv_mark[0] == it ? 0 : s_inv_mark[0], s_inv_mark[1] == it ? 0 : s_inv_mark[1]);
	}
	return true;
}

static void inventory_draw(void)
{
	if (!s_inv_dirty)
		return;
	s_inv_dirty = false;
	for (int r = 18; r < 30; r++)
		printf("\x1b[%d;1H%40s", r, "");
	if (!s_inv_open) {
		s_msg_dirty = true;      // give the message box its rows back
		touchui_menu_close(TOUI_ITEMS);
		return;
	}
	int items[64];
	int n = inv_list(items, 64);
	{
		static char rowtext[24][48];
		const char* rows[24];
		int k = n < 24 ? n : 24;
		for (int i = 0; i < k; i++) {
			const char* name = ev_item_name(items[i]);
			snprintf(rowtext[i], sizeof rowtext[i], "%s", name ? name : "?");
			char* nl = strchr(rowtext[i], '\n');
			if (nl)
				*nl = 0;
			if (ev_item_count(items[i]) > 1)
				snprintf(rowtext[i] + strlen(rowtext[i]), sizeof rowtext[i] - strlen(rowtext[i]), "   x%d",
				         ev_item_count(items[i]));
			if (s_inv_mark[0] == items[i] || s_inv_mark[1] == items[i]) {    // marked to combine
				memmove(rowtext[i] + 2, rowtext[i], sizeof rowtext[i] - 2);
				rowtext[i][0] = '+', rowtext[i][1] = ' ';
				rowtext[i][sizeof rowtext[i] - 1] = 0;
			}
			rows[i] = rowtext[i];
		}
		// Mark is R (up to two items); with items marked A uses them together with the chosen one
		static const TouiButton use[4] = { { "Use", KEY_A }, { "Examine", KEY_X }, { "Mark", KEY_R }, { "Close", KEY_B } };
		static const TouiButton join[4] = { { "Combine", KEY_A }, { "Examine", KEY_X }, { "Mark", KEY_R }, { "Close", KEY_B } };
		touchui_menu_items(items, k);
		touchui_menu(TOUI_ITEMS, "Items", rows, k, &s_inv_sel, s_inv_mark[0] || s_inv_mark[1] ? join : use, 4);
	}
	printf("\x1b[18;1H---------------- Items -----------------");
	if (!n)
		printf("\x1b[20;3H(nothing)");
	int first = s_inv_sel > 6 ? s_inv_sel - 6 : 0;
	for (int i = first; i < n && i < first + 8; i++) {
		const char* name = ev_item_name(items[i]);
		char line[40];
		snprintf(line, sizeof line, "%s", name ? name : "?");
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		printf("\x1b[%d;1H%c%c %-28.28s x%-3d", 20 + i - first,
		       s_inv_mark[0] == items[i] || s_inv_mark[1] == items[i] ? '+' : ' ', i == s_inv_sel ? '>' : ' ', line,
		       ev_item_count(items[i]));
	}
	printf("\x1b[29;1HA: use  X: examine  R: combine  Y/B: close");
}

static void events_update(u32 down)
{
	if (s_warp_pending || s_autotest)
		return;
	EvPlayer p = { s_px, s_py, s_pz, s_yaw, (down & KEY_A) != 0, s_use_item, james_can_see() };
	EvAction a;
	int used = s_use_item;
	s_use_item = 0;
	if (!ev_update(&p, &a)) {
		// Healing items work anywhere (0x49e7e0): the health drink adds a quarter of the maximum, the
		// first-aid kit half and the ampoule all of it, at once; what goes over the maximum counts
		// for the ending. The ampoule also starts a 600 second timer (0x1f7d40c) during which the PC
		// keeps James from tiring (his tired motions, which the port does not have).
		const float heal[4] = { 0.0f, JAMES_HP_MAX * 0.25f, JAMES_HP_MAX * 0.5f, JAMES_HP_MAX };
		if (used && combat_equip_item(used)) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		} else if (used >= 1 && used <= 3 && ev_item_count(used) > 0) {
			ev_give_item(used, -1);
			if (combat_hp() + heal[used] > JAMES_HP_MAX)
				prog_score_overheal((combat_hp() + heal[used] - JAMES_HP_MAX) / JAMES_HP_MAX);
			float hp = combat_hp() + heal[used];
			combat_set_hp(hp > JAMES_HP_MAX ? JAMES_HP_MAX : hp);
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		} else if (used) {
			show_message(ev_item_message(16));     // "It looks like I can't use that here."
		}
		return;
	}
	s_last_event = a.event;
	float feet[3] = { s_px, s_py, s_pz };
	if (a.kind == EV_WARP) {
		s_warp = a;
		s_warp_pending = true;
		warp_prefetch(a.map, a.stage, a.pos);
		sfx_door_open(a.door_sound, feet);
	} else if (a.kind == EV_MESSAGE) {
		show_message(a.text);
		if (a.door_sound)
			sfx_door_result(a.door_sound, (SfxDoor)a.door_slot, feet);
		else if (a.event < 0)
			sfx_play(SFX_ITEM_PICKUP, 1.0f, 0.0f);
	} else if (a.kind == EV_SAVE) {
		s_save_place = a.save_point;
		show_message(a.text);
		if (s_msg_page) {
			s_save_after_msg = true;
		} else {
			s_save_prompt = true;
			sfx_play(SFX_SAVE_POINT, 1.0f, 0.0f);
			s_msg_dirty = true;
		}
	}
}

// ---------------------------------------------------------------- save files

typedef struct {
	char magic[4];       // "SH3S"
	u32 version;         // 2 (item counts)
	u32 stage;
	char map[8];
	float pos[3], yaw;
	u8 events[EV_STATE_SIZE];
	u8 prog[PROG_STATE_SIZE];   // version 3: stage program state (puzzle answers, levels)
	u8 killed[ENEMY_KILLED_BYTES];   // version 4: dead enemies (Enemy_List ids)
	float hp;                        // version 4: James' health
	u8 clear_save;                   // version 5: 1 for clear data (the PC's 0x1dbeb87 = 0xff)
	u8 clear[3];                     // version 5: the clear data it carries (prog_clear_get)
	u8 place;                        // version 6: the save point (m_card_x_msg_e.mes place name)
	u8 pad[3];
} SaveFile;

// Bytes of each save version (older ones are prefixes of the current layout).
#define SAVE_V2_SIZE (offsetof(SaveFile, prog))
#define SAVE_V3_SIZE (offsetof(SaveFile, killed))
#define SAVE_V4_SIZE (offsetof(SaveFile, clear_save))
#define SAVE_V5_SIZE (offsetof(SaveFile, place))

// The save files (savescreen.c): file 1 is save.dat, as when there was one, the others save2.dat
// .. save10.dat (s_slot, s_save_place are with the other event state above).
static void slot_path(int slot, char* out, size_t size)
{
	if (slot <= 0)
		snprintf(out, size, "%s", SAVE_PATH);
	else
		snprintf(out, size, PACK_DIR "/save%d.dat", slot + 1);
}

static FILE* slot_open(int slot, const char* mode)
{
	char path[64];
	slot_path(slot, path, sizeof path);
	return fopen(path, mode);
}
static bool save_game(void)
{
	prog_stat_save();                // the results screen's save count
	static SaveFile sv;
	memset(&sv, 0, sizeof sv);
	memcpy(sv.magic, "SH3S", 4);
	sv.version = 6;
	sv.place = s_save_place >= 0 ? (u8)s_save_place : 0xff;
	sv.stage = ev_stage();
	snprintf(sv.map, sizeof sv.map, "%s", s_nrooms ? s_rooms[s_room] : "");
	sv.pos[0] = s_px, sv.pos[1] = s_py, sv.pos[2] = s_pz, sv.yaw = s_yaw;
	ev_state_get(sv.events);
	prog_state_get(sv.prog);
	enemy_killed_get(sv.killed);
	sv.hp = combat_hp();
	FILE* f = slot_open(s_slot, "wb");
	if (!f)
		return false;
	bool ok = fwrite(&sv, sizeof sv, 1, f) == 1;
	ok = fclose(f) == 0 && ok;
	return ok;
}

static bool read_save(SaveFile* sv)
{
	FILE* f = slot_open(s_slot, "rb");
	if (!f)
		return false;
	memset(sv, 0, sizeof *sv);
	size_t n = fread(sv, 1, sizeof *sv, f);
	bool ok = !memcmp(sv->magic, "SH3S", 4) &&
	          ((sv->version == 2 && n >= SAVE_V2_SIZE) || (sv->version == 3 && n >= SAVE_V3_SIZE) ||
	           (sv->version == 4 && n >= SAVE_V4_SIZE) || (sv->version == 5 && n >= SAVE_V5_SIZE) ||
	           (sv->version == 6 && n == sizeof *sv));
	if (ok && sv->version < 5)
		sv->clear_save = 0;
	if (ok && sv->version < 6)
		sv->place = 0xff;
	fclose(f);
	return ok;
}

// The clear data save (clearsave.c): the results counters with the program state, the clear data,
// no place to start from.
static bool save_clear_data(void)
{
	static SaveFile sv;
	memset(&sv, 0, sizeof sv);
	memcpy(sv.magic, "SH3S", 4);
	sv.version = 6;
	sv.clear_save = 1;
	sv.place = 0xff;
	prog_state_get(sv.prog);
	prog_clear_get(sv.clear);
	FILE* f = slot_open(s_slot, "wb");
	if (!f)
		return false;
	bool ok = fwrite(&sv, sizeof sv, 1, f) == 1;
	ok = fclose(f) == 0 && ok;
	return ok;
}

// The save screen's files (savescreen.c).
static bool files_info(int slot, SaveSlotInfo* out)
{
	static SaveFile sv;
	int keep = s_slot;
	s_slot = slot;
	bool ok = read_save(&sv);
	s_slot = keep;
	if (!ok)
		return false;
	out->clear = sv.clear_save != 0;
	out->place = sv.place == 0xff ? -1 : sv.place;
	out->stage = (int)sv.stage;
	out->clears = sv.clear[2];
	memcpy(out->map, sv.map, sizeof out->map);
	out->map[7] = 0;
	if (sv.version >= 3)
		prog_state_summary(sv.prog, &out->time, &out->saves, &out->chapter, &out->action, &out->riddle);
	return true;
}

static bool files_write(int slot, int mode)
{
	int keep = s_slot;
	s_slot = slot;
	bool ok = mode == SS_CLEAR ? save_clear_data() : save_game();
	if (mode == SS_CLEAR)
		s_slot = keep;
	return ok;
}

static void title_start(int what);
static void files_load(int slot)
{
	s_slot = slot;
	title_start(2);
}

static const SaveScreenHost s_files = { files_info, files_write, files_load };

static bool any_save(void)
{
	for (int i = 0; i < SAVE_SLOTS; i++) {
		FILE* f = slot_open(i, "rb");
		if (f) {
			fclose(f);
			return true;
		}
	}
	return false;
}

// A save point opens the PC's save screen (0x4551d0): the files, then the overwrite question and
// "Save completed."; s_save_prompt stays set while it is up (the game waits).
static bool save_prompt_update(u32 down)
{
	if (!s_save_prompt)
		return false;
	if (!savescreen_active()) {
		savescreen_open(SS_SAVE, &s_files, prog_chapter());
		return true;
	}
	if (!savescreen_update(down)) {
		s_save_prompt = false;
		s_msg_dirty = true;
	}
	return true;
}

// Enhanced Edition "DelayedFadeIn" (Patches/DelayedFadeIn.cpp, zlib licence, always on): the fade-in
// after a room change starts from -0.133 instead of 0 (0x4788f0, clamped at 0x47958a), so the screen
// stays black for 0.133 of the fade time and hides James' and the camera's first frames in the new
// room. The port's fade-in runs at 3 per second, which makes the hold 44 ms.
#define FADE_IN_HOLD (0.133334f / 3.0f)
static float s_fade_hold;

static void fade_update(float dt)
{
	if (s_warp_pending) {
		s_fade = s_fade + dt * 4.0f > 1.0f ? 1.0f : s_fade + dt * 4.0f;
		if (s_fade >= 1.0f && warp_ready(dt)) {
			s_fade = 1.0f;
			apply_warp();
			s_warp_pending = false;
			s_fade_hold = FADE_IN_HOLD;
			float feet[3] = { s_px, s_py, s_pz };
			sfx_door_close(s_warp.door_sound, feet);
		}
	} else if (s_arrival && s_arrival_t < ARRIVAL_MAX) {
		s_arrival_t += dt;                // stay black until the warp's tiles are in
	} else if (s_fade_hold > 0.0f) {
		s_fade_hold -= dt;
	} else if (s_fade > s_fade_target) {
		s_fade -= dt * 3.0f;
		if (s_fade < s_fade_target)
			s_fade = s_fade_target;
	} else if (s_fade < s_fade_target) {
		s_fade += dt * 3.0f;
		if (s_fade > s_fade_target)
			s_fade = s_fade_target;
	}
}

static C3D_FVec up_vec(void) { return FVec3_New(0.0f, -1.0f, 0.0f); }

static void view_matrix(C3D_Mtx* mv)
{
	float cx = (s_cur.bbox_min[0] + s_cur.bbox_max[0]) * 0.5f;
	float cy = (s_cur.bbox_min[1] + s_cur.bbox_max[1]) * 0.5f;
	float cz = (s_cur.bbox_min[2] + s_cur.bbox_max[2]) * 0.5f;
	float ex = s_cur.bbox_max[0] - s_cur.bbox_min[0];
	float ez = s_cur.bbox_max[2] - s_cur.bbox_min[2];
	float r = sqrtf(ex * ex + ez * ez) * 0.55f;
	C3D_FVec up = FVec3_New(0.0f, -1.0f, 0.0f);   // SH2 world: Y down
	if (s_cam.mode == CAM_ORBIT) {
		float a = s_cam.orbit;
		C3D_FVec eye = FVec3_New(cx + r * cosf(a), cy - r * 0.9f, cz + r * sinf(a));
		Mtx_LookAt(mv, eye, FVec3_New(cx, cy, cz), up, false);
	} else if (s_cam.mode == CAM_PLAYER) {
		// behind and above James, looking at his chest (Y down: smaller y is higher)
		// 1 unit ~ 2 mm: ceilings ~1260 units, James ~910 units tall
		float back = 1500.0f;
		float ex = s_px - sinf(s_yaw) * back, ez = s_pz - cosf(s_yaw) * back;
		if (s_have_col)
			col_clamp_ray(&s_col, s_px, s_pz, &ex, &ez, s_py - 1080.0f);
		C3D_FVec eye = FVec3_New(ex, s_py - 1080.0f, ez);
		Mtx_LookAt(mv, eye, FVec3_New(s_px, s_py - 620.0f, s_pz), up_vec(), false);
	} else {
		float dx = cosf(s_cam.pitch) * sinf(s_cam.yaw), dy = sinf(s_cam.pitch), dz = cosf(s_cam.pitch) * cosf(s_cam.yaw);
		C3D_FVec eye = FVec3_New(s_cam.eye[0], s_cam.eye[1], s_cam.eye[2]);
		Mtx_LookAt(mv, eye, FVec3_New(s_cam.eye[0] + dx, s_cam.eye[1] + dy, s_cam.eye[2] + dz), up, false);
	}
}

static void use_world_program(void)
{
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);          // position
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);          // texcoord
	AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);  // color
	AttrInfo_AddLoader(ai, 3, GPU_BYTE, 4);           // normal
}

// ---------------------------------------------------------------- lighting and fog

static C3D_LightEnv s_lenv;
static C3D_Light s_flash;
static C3D_LightLut s_spot_lut;
static C3D_LightLutDA s_flash_da;
static C3D_FogLut s_fog_lut;
static bool s_flash_on = true;
static float s_ambient = -1.0f;      // debug.txt "ambient" (percent): overrides the record Ambient
static int s_controls;              // move.h MOVE_*: 0 Rotational (3D, tank, PC default), 1 Directional (2D)
static bool s_toggle_run;           // B toggles running (Enhanced Edition option)
static bool s_ps2_flash = true;     // Enhanced Edition PS2 flashlight brightness (pause menu)
static bool s_run_pressed;          // B went down this frame (for the toggle)
static u32 s_gdown;                 // this frame's game keys going down (controls.c)
// Enhanced Edition options kept in options.txt, all on by default as in the EE (combat.c, sh2cam.c, demo.c)
static bool s_hold_stomp = true, s_swap_attack = true, s_ps2_camera = true, s_cutscene_fov = true;
static int s_bright = 3;             // Brightness Level 0..7 (Advanced Options)
static int s_bright_shown;           // frames left to show the level on the bottom screen
// The PC's brightness is a gamma ramp with the slope k / 128 (0x47cb30); here it scales the light,
// level 3 keeping the port's 1.2.
static float bright_scale(void)
{
	static const u8 k[8] = { 0x4b, 0x5a, 0x6e, 0x80, 0x8c, 0x9b, 0xb4, 0xdc };
	return 1.2f * k[s_bright & 7] / 128.0f;
}
static RoomEnv s_env;

// The room is lit, or the flashlight is on (events with the "needs light" bit want this).
static bool james_can_see(void) { return !s_env.dark || s_flash_on; }
static RoomEnvConst s_envc;
static bool s_env_dirty = true;      // fog and flashlight LUTs need rebuilding
static float s_flash_level = 1.0f;   // 0x8a2ba8: feeds c19 / c21; only dark rooms change it
static float s_slot_dir[3][3], s_slot_col[3][3];   // the three light slots of this frame (record + overrides)
static ChrFlash s_chrflash;          // the flashlight for chrlight.c (characters)
static bool s_chr_begun;             // draw_demo has set up chrlight for the scene
static int s_level_room = -1;        // PC room id the Enhanced Edition room levels were checked for
static bool s_chr_nofog;             // characters drawn without fog (final boss room, see below)
static int s_uLightDir, s_uLightCol, s_uAmb, s_uGain, s_uVgain, s_uBase;

// Record variant for the current state (LIGHTING.md): the town at night (game flag 251), the
// toilet after that flag, Born From a Wish stages.
static const char* env_variant(const char* area)
{
	if (ev_stage() >= 53)
		return "bfaw";
	if (ev_flag(251))
		return area[0] == 'c' ? "night" : !strcmp(area, "er") ? "flag251" : "-";
	return "-";
}

static void env_room_changed(void)
{
	if (!s_nrooms)
		return;
	env_load(PACK_DIR, s_rooms[s_room], env_variant(s_areas[s_area]), &s_env);
	chrlight_load(PACK_DIR, s_rooms[s_room], env_variant(s_areas[s_area]));
	if (s_ambient >= 0.0f)
		s_env.amb[0] = s_env.amb[1] = s_env.amb[2] = s_ambient;
	s_env_dirty = true;
	fog_set_color(s_env.fogp[0], s_env.fogp[1], s_env.fogp[2], s_env.fogp[3]);   // 0x47c6cd
}

// PC fog (0x458de0): factor = A + B / w, the near factor (/255) at fog start and the far factor
// at fog end; 1 means no fog. citro3d's LUT is indexed by depth, so sample w per entry.
static void fog_setup(const RoomEnv* e, float near, float far)
{
	float fs = e->fog_near_f / 255.0f, fe = e->fog_far_f / 255.0f;
	float den = 1.0f / e->fog_start - 1.0f / e->fog_end;
	float b = den != 0.0f ? (fs - fe) / den : 0.0f;
	float a = fs - b / e->fog_start;
	float data[256];
	for (int i = 0; i <= 128; i++) {
		float z = FogLut_CalcZ(i / 128.0f, near, far);
		float v = z > 1.0f ? a + b / z : fs;
		v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
		if (i < 128)
			data[i] = v;
		if (i > 0)
			data[i + 127] = v - data[i - 1];
	}
	FogLut_FromArray(&s_fog_lut, data);
}

// Flashlight profile: full in the centre, smooth falloff to zero at the cone edge (x = cos angle).
static float spot_soft(float x, float half_angle)
{
	float c_out = cosf(half_angle), c_in = cosf(half_angle * 0.3f);
	float t = (x - c_out) / (c_in - c_out);
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

// Beam attenuation: full up to start, linear to zero at end (the PC's c18 ramp).
static float flash_atten(float d, float start, float end)
{
	if (d <= start)
		return 1.0f;
	return d >= end ? 0.0f : (end - d) / (end - start);
}

static float lut_one(float x, float unused)
{
	(void)x, (void)unused;
	return 1.0f;
}

// The PC flashlight is a projected cookie texture times a beam ramp, with no N.L term. On the PICA
// the specular-0 path does that: specular0 * D0 (a constant 1 here) * spot * distance, with the
// diffuse term zeroed. It comes out as the fragment secondary colour, which the map texenv adds as
// texture * secondary. The ambient term (fragment primary) lights characters.
static void lighting_setup(void)
{
	static const C3D_Material mat = {
		{ 1.0f, 1.0f, 1.0f },   // ambient
		{ 0.0f, 0.0f, 0.0f },   // diffuse
		{ 1.0f, 1.0f, 1.0f },   // specular0
		{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
	};
	static C3D_LightLut d0;
	C3D_LightEnvInit(&s_lenv);
	C3D_LightEnvMaterial(&s_lenv, &mat);
	LightLut_FromFunc(&d0, lut_one, 0.0f, false);
	C3D_LightEnvLut(&s_lenv, GPU_LUT_D0, GPU_LUTINPUT_NH, false, &d0);
	C3D_LightInit(&s_flash, &s_lenv);
	C3D_LightSpotEnable(&s_flash, true);
	C3D_LightDistAttnEnable(&s_flash, true);
}

// Projection clip planes of the room record (fog is only used with them, in player mode).
static void env_clip(float* near, float* far)
{
	*near = s_env.clip_near >= 20.0f ? s_env.clip_near : 20.0f;
	*far = s_env.clip_far > *near + 100.0f ? s_env.clip_far : 100000.0f;
}

static void env_luts(void)
{
	float near, far;
	env_clip(&near, &far);
	float c = s_env.fl_col[0] * bright_scale();
	c = c > 1.0f ? 1.0f : c;                                     // the PC pixel shader clamps
	C3D_LightColor(&s_flash, 0.0f, 0.0f, 0.0f);      // no diffuse term: see lighting_setup
	C3D_LightSpecular0(&s_flash, c, c, c);
	float cone = s_env.fl_cone > 0.1f ? s_env.fl_cone : 0.785f;
	LightLut_FromFunc(&s_spot_lut, spot_soft, cone, true);     // record cone taken as the half angle
	C3D_LightSpotLut(&s_flash, &s_spot_lut);
	float st = s_env.fl_start > 0.0f ? s_env.fl_start : 1500.0f, en = s_env.fl_end > st ? s_env.fl_end : 6000.0f;
	LightLutDA_Create(&s_flash_da, flash_atten, 0.0f, en, st, en);
	C3D_LightDistAttn(&s_flash, &s_flash_da);
	fog_setup(&s_env, near, far);
}

// Per frame: map light uniforms (world space), the flashlight from James' chest along his facing
// (view space, only in dark rooms as on the PC), fog. Unlit viewer modes draw full bright.
static void lighting_frame(const C3D_Mtx* view, bool player)
{
	if (s_env_dirty) {
		env_luts();
		s_env_dirty = false;
	}
	// Enhanced Edition "LightingTransitionFix" (Patches/LightingTransition.cpp, zlib licence): no
	// flashlight in the bowling alley scene with Eddie and Laura (demo 0x19) and at the start of
	// the Maria ending epilogue (0x5c); the port lights scenes from James' position, so it stays off
	// for the whole scene.
	int dn = demo_number();
	bool flash = player && s_env.dark && s_flash_on && dn != 0x19 && dn != 0x5c;
	// 0x50a7d0: dark rooms set the level every frame, lit rooms keep the last one
	if (player && s_env.dark)
		s_flash_level = flash ? 0.0f : env_flash_off_level(&s_env, ev_have_item(0x12), ev_flag(625));
	if (player) {
		int room = bgm_room_id(ev_glb_crd(), s_px, s_pz);
		if (room != s_level_room) {
			s_level_room = room;
			env_ee_room_level(room, ev_stage() >= 53, &s_env, &s_flash_level);
		}
	}
	env_constants(&s_env, s_flash_level, &s_envc);
	memcpy(s_slot_dir, s_env.ldir, sizeof s_slot_dir);
	memcpy(s_slot_col, s_env.lcol, sizeof s_slot_col);
	// 0x50a7d0 -> 0x47b6c0(&0x8a2c00, 0.3 * level, 1): with the flashlight off a dark room gets a grey
	// fill light along the camera's view in place of its light 1 (shader slot 0 in the dark set),
	// 0.3 x level before the usual x0.25 (FUN_0047a7b0)
	if (player && s_env.dark && !flash && s_flash_level > 0.0f) {
		float k = 0.3f * s_flash_level * 0.25f;
		s_slot_dir[0][0] = -view->r[2].x, s_slot_dir[0][1] = -view->r[2].y, s_slot_dir[0][2] = -view->r[2].z;
		s_slot_col[0][0] = s_slot_col[0][1] = s_slot_col[0][2] = k;
	}
	for (int i = 0; i < 3; i++) {
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLightDir + i, -s_slot_dir[i][0], -s_slot_dir[i][1], -s_slot_dir[i][2], 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLightCol + i, s_slot_col[i][0], s_slot_col[i][1], s_slot_col[i][2], 0.0f);
	}
	C3D_LightEnvAmbient(&s_lenv, 0.0f, 0.0f, 0.0f);
	C3D_LightEnable(&s_flash, flash);
	s_chrflash.on = flash;
	if (flash) {
		float fx = sinf(s_yaw), fz = cosf(s_yaw);
		C3D_FVec wp = FVec4_New(s_px + fx * 80.0f, s_py - 780.0f, s_pz + fz * 80.0f, 1.0f);
		C3D_FVec wd = FVec4_New(fx, 0.3f, fz, 0.0f);        // aimed at the floor ahead (Y down)
		s_chrflash.pos[0] = wp.x, s_chrflash.pos[1] = wp.y, s_chrflash.pos[2] = wp.z;
		s_chrflash.dir[0] = wd.x, s_chrflash.dir[1] = wd.y, s_chrflash.dir[2] = wd.z;
		s_chrflash.start = s_env.fl_start > 0.0f ? s_env.fl_start : 1500.0f;
		s_chrflash.end = s_env.fl_end > s_chrflash.start ? s_env.fl_end : 6000.0f;
		s_chrflash.cone = s_env.fl_cone > 0.1f ? s_env.fl_cone : 0.785f;
		C3D_FVec vp = Mtx_MultiplyFVec4(view, wp);
		C3D_FVec vd = Mtx_MultiplyFVec4(view, wd);
		C3D_LightPosition(&s_flash, &vp);
		C3D_LightSpotDir(&s_flash, vd.x, vd.y, vd.z);
	}
	if (player) {
		C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
		u32 fog = (u32)s_env.fog_rgb[2] << 16 | (u32)s_env.fog_rgb[1] << 8 | (u32)s_env.fog_rgb[0];
		// Enhanced Edition "LightingFix" (Patches/TreeLighting.cpp, RunTreeColor, zlib licence): the
		// horizon of the Maria ending's letter scene (demo 0x5d) fades into blue grey, not the record's fog
		if (dn == 0x5d)
			fog = 150u << 16 | 135u << 8 | 125u;
		C3D_FogColor(fog);
		C3D_FogLutBind(&s_fog_lut);
	} else {
		C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	}
}

// Enhanced Edition "FogParameterFix" (Patches/FogAdjustments.cpp, FinalAreaBoss1/2ASM, zlib
// licence): in the final boss room the character renderer (0x501e70) turns fog off while the
// camera is above y -15750 (Y points down), so the figures on the bed are not lost in the fog
// seen from the high camera.
static bool ee_final_boss_nofog(float eye_y)
{
	return eye_y < -15750.0f && bgm_room_id(ev_glb_crd(), s_px, s_pz) == 0xbb;   // R_FINAL_BOSS_RM
}

// The PC fog particle layer (fog.c), run and drawn after the characters like the PC's 0x489b90.
static FogFrame s_fogf;
static int s_fog_stage = -1;

static void fog_frame(const C3D_Mtx* proj, const C3D_Mtx* view, const C3D_Mtx* ortho, float dt)
{
	if (ev_stage() != s_fog_stage) {        // 0x489af0 runs on every stage connect
		s_fog_stage = ev_stage();
		fog_reset();
	}
	s_fogf.glb = ev_glb_crd();
	s_fogf.room = bgm_room_id(s_fogf.glb, s_px, s_pz);
	s_fogf.stage = ev_stage();
	s_fogf.james[0] = s_px, s_fogf.james[1] = s_py, s_fogf.james[2] = s_pz;
	s_fogf.dark = s_env.dark;
	s_fogf.demo_no = demo_number();
	s_fogf.demo_timer = demo_timer();
	s_fogf.flag508 = ev_flag(508);
	s_fogf.dt = dt;
	fog_update(&s_fogf);
	fog_draw(proj, view, ortho);
}

// Vertex-shader light terms for one map material (see world.v.pica).
static void material_uniforms(const RoomMaterial* m, bool lit)
{
	float gain[3] = { 0, 0, 0 }, amb[3] = { 0, 0, 0 }, vgain[3] = { 0, 0, 0 }, base = 0.0f;
	// Mode 3 is the PC's VS2 (0x8f2490): it passes v5 through, but mode-3 vertices have no colour,
	// so the light fittings drawn with it come out black or random. Enhanced Edition
	// "HalogenLightFix" (Wrappers/d3d8/IDirect3DDevice8.cpp, zlib licence) patches the shader to
	// output c14.w (1.0), full bright like mode 0.
	if (!lit || s_render_mode == 2 || m->mode == 0 || m->mode == 3) {
		base = 1.0f;
	} else if (m->mode == 4) {
		for (int i = 0; i < 3; i++) {
			gain[i] = 2.0f * s_envc.c20[i];
			amb[i] = s_envc.c21[i];
			vgain[i] = 2.0f * s_envc.c22[i];
		}
	} else {
		float mc[3] = { (m->color >> 16 & 255) / 255.0f, (m->color >> 8 & 255) / 255.0f, (m->color & 255) / 255.0f };
		for (int i = 0; i < 3; i++) {
			gain[i] = 2.0f * mc[i];
			amb[i] = s_envc.c19[i];
		}
	}
	if (base == 0.0f) {
		float k = bright_scale();
		for (int i = 0; i < 3; i++)
			gain[i] *= k, vgain[i] *= k;
	}
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uGain, gain[0], gain[1], gain[2], 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uAmb, amb[0], amb[1], amb[2], 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uVgain, vgain[0], vgain[1], vgain[2], 1.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uBase, base, base, base, 0.0f);
}

static void use_skin_program(void)
{
	C3D_BindProgram(&s_skin_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);          // position (bind pose, model space)
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);          // texcoord
	AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);  // palette slot * 3
	AttrInfo_AddLoader(ai, 3, GPU_UNSIGNED_BYTE, 4);  // weights 0..255
	AttrInfo_AddLoader(ai, 4, GPU_BYTE, 4);           // normal (bind pose)
}

static void setup_pipeline(void)
{
	s_dvlb = DVLB_ParseFile((u32*)world_shbin, world_shbin_size);
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uProjection = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
	s_uModelView = shaderInstanceGetUniformLocation(s_prog.vertexShader, "modelView");
	s_uLightDir = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lightDir");
	s_uLightCol = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lightCol");
	s_uAmb = shaderInstanceGetUniformLocation(s_prog.vertexShader, "amb");
	s_uGain = shaderInstanceGetUniformLocation(s_prog.vertexShader, "gain");
	s_uVgain = shaderInstanceGetUniformLocation(s_prog.vertexShader, "vgain");
	s_uBase = shaderInstanceGetUniformLocation(s_prog.vertexShader, "base");

	s_skin_dvlb = DVLB_ParseFile((u32*)skin_shbin, skin_shbin_size);
	shaderProgramInit(&s_skin_prog);
	shaderProgramSetVsh(&s_skin_prog, &s_skin_dvlb->DVLE[0]);
	s_uSkinProjection = shaderInstanceGetUniformLocation(s_skin_prog.vertexShader, "projection");
	s_uSkinModelView = shaderInstanceGetUniformLocation(s_skin_prog.vertexShader, "modelView");
	s_uBones = shaderInstanceGetUniformLocation(s_skin_prog.vertexShader, "bones");

	use_world_program();
	C3D_CullFace(GPU_CULL_NONE);
	lighting_setup();
	chrlight_init(&s_lenv);
	shadow_init();
	fog_init(PACK_DIR);
	stream_init(PACK_DIR);
	perf_init(PACK_DIR);
}

// The Game Options' Weapon Control and Walk/Run Control (0x52e110) as the keys the movement and
// combat code read: Switch makes R a toggle for the ready stance, Run runs unless B is held,
// Analog runs on a full tilt of the circle pad.
static u32 move_keys(u32 held, u32 down, const circlePosition* cp)
{
	static bool ready;
	if (s_opt.weapon == 1) {
		if (down & KEY_R)
			ready = !ready;
		held = ready ? held | KEY_R : held & ~KEY_R;
	} else {
		ready = false;
	}
	if (s_opt.walk_run == 2)
		held ^= KEY_B;
	else if (s_opt.walk_run == 1)
		held = hypotf(cp->dx, cp->dy) > 0.9f * 156.0f ? held | KEY_B : held & ~KEY_B;
	return held;
}

// View Control (0x5342b0): Normal shows the search view while the look button is held (0x1fbab13),
// Switch turns it on and off with each press (0x52e110 flips 0x1fba8fb); neither while a stage
// program holds James (the PC's player flags 0x6000).
static bool search_view(u32 held, u32 down)
{
	static bool on;
	if (s_opt.view == 0) {
		on = false;
		return (held & GK_LOOK) != 0;
	}
	if (down & GK_LOOK)
		on = !on;
	return on && !(prog_running() && prog_player_locked());
}

// The blood a fight leaves (blood.c). The PC posts the splash from the hit (0x5431b1 calls
// 0x4c7d40 with the enemy and the attack); here a hit shows as the enemy's health going down once
// enemy.c has applied what combat.c handed it, with its last attack id and hit-back direction. A
// death starts the pool (enDyingExec 0x4a96c0 posts it at the start, not for Eddie).
static bool s_blood_test;       // debug.txt "bloodtest": a splash and a pool beside James every 2 s

static void blood_frame(float dt)
{
	if (s_blood_test && dt > 0.0f) {
		static float t, at[3];
		if ((t -= dt) <= 0.0f) {
			t = 2.0f;
			// 700 to one side of James as the camera sees him, the other side the next time
			const float* eye = gamecam_eye();
			float dx = s_px - eye[0], dz = s_pz - eye[2], d = sqrtf(dx * dx + dz * dz);
			d = d > 1.0f ? 1.0f / d : 0.0f;
			dx *= d, dz *= d;
			static float side = 700.0f;
			side = -side;
			at[0] = s_px - dz * side, at[1] = s_py, at[2] = s_pz + dx * side;
			float pos[3] = { at[0], at[1] - 500.0f, at[2] }, dir[3] = { dx, 0.0f, dz };
			blood_hit(pos, dir, 0x13, at, at[1]);
			blood_pool(at, bgm_room_id(ev_glb_crd(), at[0], at[2]));
		}
	}
	static float last_hp[ENEMY_MAX];
	static bool seen[ENEMY_MAX], dead[ENEMY_MAX];
	static const float* owner[ENEMY_MAX];
	for (int i = 0; i < ENEMY_MAX; i++) {
		Enemy* e = enemy_get(i);
		if (!e) {
			if (owner[i])
				blood_forget(owner[i]);
			owner[i] = NULL;
			seen[i] = false;
			continue;
		}
		bool d = e->hp <= 0.0f;
		if (seen[i] && e->hp < last_hp[i] - 0.001f) {
			float tall = e->tall > 0.0f ? e->tall : 800.0f;
			float pos[3] = { e->x, e->y - tall * 0.6f, e->z };
			float dir[3] = { e->hbx, 0.0f, e->hbz };
			if (fabsf(dir[0]) + fabsf(dir[2]) < 1e-4f)
				dir[0] = e->x - s_px, dir[2] = e->z - s_pz;
			blood_hit(pos, dir, e->last_atk, &e->x, e->y);
		}
		if (seen[i] && d && !dead[i] && e->kind != EN_KIND_EDI)
			blood_pool(&e->x, bgm_room_id(ev_glb_crd(), e->x, e->z));
		last_hp[i] = e->hp;
		dead[i] = d;
		seen[i] = true;
		owner[i] = &e->x;
	}
	blood_update(dt);
}

// James' legs (move.c, the PC's lower-body states): the circle pad walks and turns, the game keys
// (controls.c) run, strafe and turn round. held is the game keys still free (combat.c took its own).
static void player_update(const circlePosition* cp, u32 held, float dt)
{
	MoveInput in;
	memset(&in, 0, sizeof in);
	// the circle pad reads about +-156 at the rim; a small dead zone keeps James from creeping
	float x = cp->dx / 156.0f, y = cp->dy / 156.0f;
	in.x = fabsf(x) < 0.12f ? 0.0f : x;
	in.y = fabsf(y) < 0.12f ? 0.0f : y;
	// Enhanced Edition "EnableToggleSprint": B toggles running, which stops when James stops
	static bool s_run_latched;
	if (s_toggle_run) {
		if (s_run_pressed)
			s_run_latched = !s_run_latched;
		if (in.y <= 0.0f)
			s_run_latched = false;
	}
	s_run_pressed = false;
	static u32 s_prev_held;
	in.run = s_toggle_run ? s_run_latched : (held & KEY_B) != 0;
	in.strafe_l = (held & GK_STRAFE_L) != 0;
	in.strafe_r = (held & GK_STRAFE_R) != 0;
	in.strafe_l_down = in.strafe_l && !(s_prev_held & GK_STRAFE_L);
	in.strafe_r_down = in.strafe_r && !(s_prev_held & GK_STRAFE_R);
	in.turn180 = (held & GK_TURN180) && !(s_prev_held & GK_TURN180);
	s_prev_held = held;
	in.control = s_controls;
	in.camera_yaw = gamecam_yaw();
	in.hp_frac = combat_hp() / JAMES_HP_MAX;
	in.maria = combat_own_footsteps();
	in.drag = combat_grip() == 8;
	// 0x5422b0: an enemy is about (the PC's battle flag 0x400; here one that has seen or heard James)
	for (int i = 0; i < enemy_count() && !in.enemy_near; i++) {
		const Enemy* e = enemy_get(i);
		in.enemy_near = enemy_alive(e) && !enemy_is_down(e) && e->alert > 0.0f && e->p_dist < 8000.0f;
	}
	if (s_have_col) {
		// 0x536a20: a wall within 250 of a 900 probe ahead stops a run before it starts
		float ex = s_px + sinf(s_yaw) * 900.0f, ez = s_pz + cosf(s_yaw) * 900.0f;
		col_clamp_ray(&s_col, s_px, s_pz, &ex, &ez, s_py - 750.0f);
		in.wall_ahead = hypotf(ex - s_px, ez - s_pz) < 250.0f;
	}
	MoveOut mo;
	move_update(&in, &s_yaw, dt, &mo);
	s_turn_speed = mo.turn_speed;
	float ox = s_px, oz = s_pz;
	float nx = s_px + mo.dx, nz = s_pz + mo.dz;
	if (s_have_col) {
		float ny;
		col_push(&s_col, &nx, &nz, s_py, 900.0f, 130.0f);
		if (col_floor_climb(&s_col, nx, nz, s_py, 160.0f, 160.0f, &ny)) {
			s_px = nx;
			s_pz = nz;
			s_py = ny;
		} else if (nx != s_px || nz != s_pz) {
			// The open areas' tiles leave gaps of up to about 60 units between their floors (cb05 and
			// cb52 meet at x 19950 / 20000), which James walks over on the PC: with floor a little
			// further on at the same height he goes on at his height.
			float mx = nx - s_px, mz = nz - s_pz, ml = sqrtf(mx * mx + mz * mz);
			if (col_floor(&s_col, s_px + mx / ml * 100.0f, s_pz + mz / ml * 100.0f, s_py, 40.0f, &ny)) {
				s_px = nx;
				s_pz = nz;
			}
		}
	} else {
		s_px = nx;
		s_pz = nz;
	}
	s_vel[0] = dt > 0.0f ? (s_px - ox) / dt : 0.0f;
	s_vel[1] = dt > 0.0f ? (s_pz - oz) / dt : 0.0f;

	// James' footsteps on the PC's frames of his clips (move.c); Maria's come from combat.c
	if (mo.step && !combat_own_footsteps()) {
		float feet[3] = { s_px, s_py, s_pz };
		sfx_footstep(mo.step_volume, feet);
		blood_footstep(feet, s_yaw);    // a step in a pool leaves footprints after it (blood.c)
	}
	if (mo.scrape) {
		float feet[3] = { s_px, s_py, s_pz };
		sfx_play_at(11067, 0.3f, feet);     // 0x2b3b: the great knife dragged along the floor
	}
}

// James' head (0x535720): it turns to the locked target or to the nearest enemy, person or item he
// can see, and with the look key held it follows the circle pad. The angles ease towards their aim
// at 20 x 0.3235 of the gap a second (0x5343e0) and stop at 0.8 to either side, 0.5 up, 0.4 down.
static float s_look_yaw, s_look_pitch;

static void head_update(const circlePosition* cp, u32 gheld, float dt, bool on)
{
	float ty = 0.0f, tp = 0.0f;
	int st = move_last()->state;
	if (on && st != MV_RELAX && st != MV_ALERT) {
		if (gheld & GK_LOOK) {
			float x = cp->dx / 156.0f, y = cp->dy / 156.0f;
			ty = -x * 0.8f;                    // the stick right turns the head right (yaw down)
			tp = y * (ty >= 0.0f ? 0.4f : 0.5f);
		} else {
			static float pts[33][3];
			int n = ev_items_lying(pts, 32);
			CombatBody body = { &s_px, &s_py, &s_pz, &s_yaw };
			EnPlayer jp = combat_enplayer(&body, s_flash_on), mp;
			if (maria_target(&jp, &mp))
				pts[n][0] = mp.x, pts[n][1] = mp.y - 800.0f, pts[n][2] = mp.z, n++;
			float at[3];
			if (combat_look_target(&body, (const float (*)[3])pts, n, at)) {
				float dx = at[0] - s_px, dz = at[2] - s_pz;
				ty = remainderf(atan2f(dx, dz) - s_yaw, 2.0f * (float)M_PI);
				tp = atan2f((s_py - 800.0f) - at[1], hypotf(dx, dz));   // Y points down
			}
		}
	}
	ty = ty < -0.8f ? -0.8f : ty > 0.8f ? 0.8f : ty;
	tp = tp < -0.4f ? -0.4f : tp > 0.5f ? 0.5f : tp;
	float k = 20.0f * 0.32345787f * dt;
	k = k > 1.0f ? 1.0f : k;
	s_look_yaw += (ty - s_look_yaw) * k;
	s_look_pitch += (tp - s_look_pitch) * k;
}

// Character shadows this frame (shadow.c): in the dark rooms the PC casts stencil shadows from
// James' flashlight, where the record has ShadowDarkNess set (0x47bb00); elsewhere the drop
// shadows. None in cutscenes (their own shadow lights are not ported). The darkness is the Enhanced Edition's, faded in over about 33 frames when the
// flashlight comes on, as EE's SetShadowFading does (3 of 100 a frame).
static void shadow_frame(bool paused)
{
	static float fade;
	float k = 0.0f;
	if (!s_opt.shadows) {
		shadow_begin(s_chrflash.pos, s_chrflash.dir, s_chrflash.cone, 0.0f);
		return;
	}
	// 0x5a8370: the lit rooms (gFlashlightAvailable 1) and the dark ones whose record has no
	// ShadowDarkNess get the drop shadows instead
	if (s_cam.mode == CAM_PLAYER && s_state == ST_GAME && !demo_active() && (!s_env.dark || s_env.shadow == 0)) {
		fade = 0.0f;
		if (!quality()->drop_shadows) {
			shadow_begin(s_chrflash.pos, s_chrflash.dir, s_chrflash.cone, 0.0f);
			return;
		}
		const float* eye = gamecam_eye();
		shadow_begin_drop(s_env.map_id, eye);
		return;
	}
	if (s_cam.mode == CAM_PLAYER && s_state == ST_GAME && !demo_active() && s_chrflash.on && s_env.shadow != 0 &&
	    quality()->stencil_shadows) {
		int room = bgm_room_id(ev_glb_crd(), s_px, s_pz);
		if (!paused)
			fade = fade + 0.03f > 1.0f ? 1.0f : fade + 0.03f;
		k = shadow_ee_opacity(room, prog_chapter() == 1) / 255.0f * fade;
	} else {
		fade = 0.0f;
	}
	shadow_begin(s_chrflash.pos, s_chrflash.dir, s_chrflash.cone, k);
}

static void draw_james(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	if (!s_have_james)
		return;
	C3D_Mtx model, mv;
	Mtx_Identity(&model);
	Mtx_Translate(&model, s_px, s_py, s_pz, true);
	Mtx_RotateY(&model, s_yaw, true);
	Mtx_Multiply(&mv, view, &model);
	use_skin_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uSkinProjection, proj);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uSkinModelView, &mv);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_DepthMap(true, -1.0f, 0.0f);
	// The pose: combat.c's clip while it has James (readied, attacking, hurt), else his legs' (move.c),
	// crossfaded into at each change of clip the way the PC's anime code does (chr_pose_blend)
	const MoveOut* mo = move_last();
	const Anim* anim = &s_james_anim;
	float frame = mo->frame, blend = mo->blend;
	bool armed = combat_james_anim(&anim, &frame);   // armed: the weapon's animation set
	bool legs = !armed || combat_james_free();
	int clip = mo->clip.start;
	if (legs)
		frame = mo->frame;
	else
		clip = combat_james_clip(&blend);
	if (legs && frame >= mo->clip.end + 1.0f)
		frame = mo->clip.start;                  // the loop seam
	static ChrBlend s_jblend;
	static int s_jclip = -1;
	static bool s_jlegs;
	static const Anim* s_janim;
	if (clip != s_jclip || legs != s_jlegs || anim != s_janim) {
		if (legs != s_jlegs || anim != s_janim)
			chr_blend_start(&s_jblend, 8.0f / 30.0f);    // from combat's clip to the legs' or back
		else if (legs && mo->sync)
			chr_blend_start_sync(&s_jblend, anim, mo->from_start, mo->clip.start,
			                     mo->clip.end - mo->clip.start + 1, blend);
		else
			chr_blend_start(&s_jblend, blend);
		s_jclip = clip;
		s_jlegs = legs;
		s_janim = anim;
	}
	// the head and neck bones: James' hll_jms 15 and 8, Maria's lxx_mar 9 and 6
	bool jms = s_james.n_bones == 41, mar = s_james.n_bones == 36;
	s_jblend.look_head = jms ? 15 : mar ? 9 : -1;
	s_jblend.look_neck = jms ? 8 : mar ? 6 : -1;
	s_jblend.neck_share = 0.2f;
	s_jblend.look_yaw = demo_active() ? 0.0f : s_look_yaw;
	s_jblend.look_pitch = demo_active() ? 0.0f : s_look_pitch;
	chr_pose_blend(&s_james, anim, frame, &s_jblend, s_anim_dt);
	// Characters are lit like the PC's (chrlight.c: the room's character lights, the strongest
	// point and spot lights near each character, ambient 2.1 x Ambient + 0.3 x level). Full bright
	// in the viewer.
	bool own = s_cam.mode == CAM_PLAYER && !s_chr_begun;
	if (s_cam.mode == CAM_PLAYER) {
		if (own)
			chrlight_begin(view, s_env.amb, s_flash_level, bright_scale(), &s_chrflash);
		float at[3] = { s_px, s_py, s_pz };
		chrlight_at(at, true);
	} else {
		C3D_LightEnvAmbient(&s_lenv, 1.0f, 1.0f, 1.0f);
	}
	if (s_chr_nofog)
		C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	shadow_mark_begin();                  // characters take no shadow (shadow.c)
	float jat[3] = { s_px, s_py, s_pz };
	shadow_cast(&s_james, jat, s_yaw);    // the PC's hll_jms.kg1 is empty, so nothing yet
	chr_draw(&s_james, s_uBones);
	combat_draw_weapon(s_uBones);
	if (s_state == ST_GAME) {
		enemy_draw(view, s_uSkinModelView, s_uBones);
		stagemdl_draw(view, s_uSkinModelView, s_uBones);   // the stage's own models (stagemdl.c)
	}
	shadow_mark_end();
	if (s_chr_nofog)
		C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
	if (own)
		chrlight_end();
	C3D_LightEnvAmbient(&s_lenv, 0.0f, 0.0f, 0.0f);
}

// demo.c's per-character light factor (Enhanced Edition "EnemyRevealLighting")
static float s_demo_amb[3];
static void demo_light_scale(float k)
{
	chrlight_ambient_scale(k);
}

// Cutscene characters (demo.c) with James' lighting; James himself only when the scene has none.
static void draw_demo(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	use_skin_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uSkinProjection, proj);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_DepthMap(true, -1.0f, 0.0f);
	float a[3];
	for (int i = 0; i < 3; i++) {
		float lit = s_slot_col[0][i] + s_slot_col[1][i] + s_slot_col[2][i] + s_envc.c19[i];
		a[i] = 1.4f * lit * bright_scale();
		s_demo_amb[i] = a[i];
	}
	C3D_LightEnvAmbient(&s_lenv, a[0], a[1], a[2]);
	if (s_chr_nofog)
		C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	demo_set_lighting(&s_lenv, a, bright_scale());
	demo_light_hook(demo_light_scale);
	chrlight_begin(view, s_env.amb, s_flash_level, bright_scale(), &s_chrflash);
	s_chr_begun = true;
	shadow_mark_begin();
	demo_draw(view, s_uSkinModelView, s_uBones);
	shadow_mark_end();
	demo_light_hook(NULL);
	if (s_chr_nofog)
		C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
	if (!demo_has_james())
		draw_james(proj, view);
	s_chr_begun = false;
	chrlight_end();
	C3D_LightEnvAmbient(&s_lenv, 0.0f, 0.0f, 0.0f);
}

static void texenv(bool textured, int group)
{
	static const u32 group_color[3] = { 0xFFD0A060, 0xFF40C0FF, 0xFF4040FF };   // ABGR
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (s_render_mode == 1) {
		C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		C3D_TexEnvColor(env, group_color[group]);
	} else if (textured) {
		// PC map pixel shader: texture * vertex light, plus texture * flashlight (fragment light)
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		C3D_TexEnv* e1 = C3D_GetTexEnv(1);
		C3D_TexEnvInit(e1);
		C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_FRAGMENT_SECONDARY_COLOR, GPU_PREVIOUS);
		C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
		C3D_TexEnvFunc(e1, C3D_RGB, GPU_MULTIPLY_ADD);
		C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
		return;
	} else {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_FRAGMENT_SECONDARY_COLOR, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	}
	C3D_TexEnvInit(C3D_GetTexEnv(1));
}

// Texture orientation test: texture 0x746 (door top-left, shadow blobs in alpha) drawn on a
// 240x240 quad with SH2 UVs (top-left origin) over a grey background.
static void draw_texture_test(const C3D_Mtx* ortho)
{
	typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;
	static const V quad[6] = {
		{ 80, 240, 0.5f, 0, 0, {255,255,255,255}, {0,0,127,0} }, { 80, 0, 0.5f, 0, 1, {255,255,255,255}, {0,0,127,0} },
		{ 320, 0, 0.5f, 1, 1, {255,255,255,255}, {0,0,127,0} },  { 80, 240, 0.5f, 0, 0, {255,255,255,255}, {0,0,127,0} },
		{ 320, 0, 0.5f, 1, 1, {255,255,255,255}, {0,0,127,0} },  { 320, 240, 0.5f, 1, 0, {255,255,255,255}, {0,0,127,0} },
	};
	if (!s_testquad) {
		s_testquad = linearAlloc(sizeof quad);
		memcpy(s_testquad, quad, sizeof quad);
	}
	C3D_Tex* tex = NULL;
	for (u32 i = 0; i < s_cur.n_materials; i++)
		if (s_cur.materials[i].texture_id == 0x746 && s_cur.materials[i].tex)
			tex = s_cur.materials[i].tex;
	if (!tex)
		return;
	C3D_Mtx id;
	Mtx_Identity(&id);
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &id);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	texenv(true, 0);
	C3D_TexBind(0, tex);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_testquad, ROOM_VERTEX_SIZE, 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
}

// Full-screen black overlay for door transitions.
static void draw_fade(const C3D_Mtx* ortho, float alpha)
{
	typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;
	static const V quad[6] = {
		{ 0, 0, 0.5f, 0, 0, {0,0,0,255}, {0,0,127,0} },   { 400, 0, 0.5f, 0, 0, {0,0,0,255}, {0,0,127,0} },
		{ 400, 240, 0.5f, 0, 0, {0,0,0,255}, {0,0,127,0} }, { 0, 0, 0.5f, 0, 0, {0,0,0,255}, {0,0,127,0} },
		{ 400, 240, 0.5f, 0, 0, {0,0,0,255}, {0,0,127,0} }, { 0, 240, 0.5f, 0, 0, {0,0,0,255}, {0,0,127,0} },
	};
	if (!s_fadequad) {
		s_fadequad = linearAlloc(sizeof quad);
		memcpy(s_fadequad, quad, sizeof quad);
	}
	C3D_Mtx id;
	Mtx_Identity(&id);
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &id);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, (u32)(alpha * 255.0f) << 24);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_fadequad, ROOM_VERTEX_SIZE, 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
}

// One group of one room's batches (the vertex buffer is bound per room: indices are room-local).
// The PC's low health cross (0x4f6240): a square 0.1 of the screen width wide, right edge at 0.95
// of the width, bottom at 0.95 of the height, the red cross texture modulated by the alpha. The port
// draws the cross from three red quads instead of the texture.
static void* s_crossquad;        // linear memory, 18 ROOM_VERTEX_SIZE vertices

static void draw_low_health(const C3D_Mtx* ortho, float alpha)
{
	typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;
	if (!s_crossquad) {
		// 400 x 240: x 340..380, y 12..52 (y up); arms one third of the square thick
		static const float r[3][4] = { { 353.3f, 12.0f, 366.7f, 52.0f }, { 340.0f, 25.3f, 353.3f, 38.7f },
		                               { 366.7f, 25.3f, 380.0f, 38.7f } };
		V q[18];
		memset(q, 0, sizeof q);
		for (int i = 0; i < 3; i++) {
			float x0 = r[i][0], y0 = r[i][1], x1 = r[i][2], y1 = r[i][3];
			float xy[6][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y0 }, { x1, y1 }, { x0, y1 } };
			for (int k = 0; k < 6; k++) {
				V* v = &q[i * 6 + k];
				v->x = xy[k][0], v->y = xy[k][1], v->z = 0.5f;
				v->c[3] = 255;
				v->n[2] = 127;
			}
		}
		s_crossquad = linearAlloc(sizeof q);
		if (!s_crossquad)
			return;
		memcpy(s_crossquad, q, sizeof q);
	}
	C3D_Mtx id;
	Mtx_Identity(&id);
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &id);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, (u32)(alpha * 255.0f) << 24 | 0x1818D0);     // ABGR: red
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_crossquad, ROOM_VERTEX_SIZE, 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 18);
}

static u32 draw_room_group(const Room* r, int group)
{
	u32 calls = 0;
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, r->vertices, ROOM_VERTEX_SIZE, 4, 0x3210);
	// consecutive batches often share the texenv, the material constants, the texture and the
	// alpha test: only what changes is set again (citro3d re-sends whatever is touched)
	int env_mode = -1, alpha = -1;
	const RoomMaterial* last_m = NULL;
	const C3D_Tex* last_tex = NULL;
	bool lit = s_cam.mode == CAM_PLAYER;
	for (u32 i = 0; i < r->n_batches; i++) {
		RoomBatch* b = &r->batches[i];
		if ((int)b->group != group || b->hidden || (s_only_batch >= 0 && (int)i != s_only_batch))
			continue;
		RoomMaterial* m = &r->materials[b->material < r->n_materials ? b->material : 0];
		bool textured = m->tex != NULL && s_render_mode == 0;
		if ((int)textured != env_mode) {
			texenv(textured, group);
			env_mode = (int)textured;
		}
		if (!last_m || m->mode != last_m->mode || m->color != last_m->color)
			material_uniforms(m, lit);
		last_m = m;
		if (textured && m->tex != last_tex) {
			C3D_TexBind(0, m->tex);
			last_tex = m->tex;
		}
		int a = textured && (m->flags & 1);
		if (a != alpha) {
			C3D_AlphaTest(a, GPU_GREATER, 0x40);
			alpha = a;
		}
		C3D_DrawElements(GPU_TRIANGLES, b->index_count, C3D_UNSIGNED_SHORT, b->indices);
		calls++;
	}
	return calls;
}

// A neighbour tile whose bounding box is entirely outside one clip plane cannot show: skip it (the
// primary tile is always drawn). clip = projection * view.
static bool room_visible(const Room* r, const C3D_Mtx* clip)
{
	int out[5] = { 0 };
	for (int c = 0; c < 8; c++) {
		C3D_FVec p = FVec4_New(c & 1 ? r->bbox_max[0] : r->bbox_min[0], c & 2 ? r->bbox_max[1] : r->bbox_min[1],
		                       c & 4 ? r->bbox_max[2] : r->bbox_min[2], 1.0f);
		C3D_FVec q = Mtx_MultiplyFVec4(clip, p);
		out[0] += q.x < -q.w, out[1] += q.x > q.w;
		out[2] += q.y < -q.w, out[3] += q.y > q.w;
		out[4] += q.w <= 0.0f;     // behind the eye (depth is left alone: the tilted projection's sign)
	}
	for (int i = 0; i < 5; i++)
		if (out[i] == 8)
			return false;
	return true;
}

static C3D_Mtx s_clip_mtx;     // projection * view of the frame, for room_visible

// The primary room and the resident neighbours: every room's opaque pass first, then decals and
// transparent parts, so blending sees the whole scene behind it.
static u32 draw_room(void)
{
	u32 calls = 0;
	if (!s_loaded)
		return 0;
	bool vis[MAX_NB];
	for (int k = 0; k < s_nnb; k++)
		vis[k] = room_visible(&s_nb[k], &s_clip_mtx);
	for (int pass = GROUP_OPAQUE; pass <= GROUP_DECAL; pass++) {
		int group = pass == 1 ? GROUP_DECAL : pass == 2 ? GROUP_TRANSPARENT : GROUP_OPAQUE;
		if (s_opaque_only && group != GROUP_OPAQUE)
			continue;
		GPU_TESTFUNC test = s_depth_variant == 0 ? GPU_GREATER : s_depth_variant == 1 ? GPU_LESS : GPU_ALWAYS;
		GPU_TESTFUNC test_eq = s_depth_variant == 0 ? GPU_GEQUAL : s_depth_variant == 1 ? GPU_LEQUAL : GPU_ALWAYS;
		if (group == GROUP_OPAQUE) {
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
			C3D_DepthTest(true, test, GPU_WRITE_ALL);
			C3D_DepthMap(true, -1.0f, 0.0f);
		} else {
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
			               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
			C3D_DepthTest(true, test_eq, GPU_WRITE_COLOR);
			C3D_DepthMap(true, -1.0f, group == GROUP_DECAL ? 0.0002f : 0.0f);
		}
		calls += draw_room_group(&s_cur, group);
		for (int k = 0; k < s_nnb; k++)
			if (vis[k])
				calls += draw_room_group(&s_nb[k], group);
	}
	return calls;
}

// ---------------------------------------------------------------- stage program services (prog.h)

static void host_message(const char* text) { show_message(text); }
static bool host_message_open(void) { return s_msg_page != NULL; }

static void host_panel(const char* text)
{
	if (!text) text = "";
	if (strcmp(text, s_panel)) {
		snprintf(s_panel, sizeof s_panel, "%s", text);
		s_msg_dirty = true;
	}
}

// Stills from data/pic (tools/sh2conv/pics.py: pack/pic/<dir>/<name>.t3x, 512x256 RGB565). The
// programs name them like the PC's file ids ("apt_clock_memo" is pic/apt/clock_memo); shown 320x240
// in the middle of the top screen over black, as the PC stretches its 512x512 stills to 4:3.
static C3D_Tex s_pic_tex;
static bool s_pic_shown;
static void* s_picquad;          // linear memory, 6 ROOM_VERTEX_SIZE vertices

static bool host_picture(const char* name)
{
	if (s_pic_shown) {
		C3D_TexDelete(&s_pic_tex);
		s_pic_shown = false;
	}
	if (!name)
		return false;
	char path[96];
	snprintf(path, sizeof path, PACK_DIR "/pic/%s", name);
	char* sep = strchr(path + strlen(PACK_DIR "/pic/"), '_');
	if (!sep)
		return false;
	*sep = '/';
	strncat(path, ".t3x", sizeof path - strlen(path) - 1);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_pic_tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(&s_pic_tex, GPU_LINEAR, GPU_LINEAR);
	s_pic_shown = true;
	return true;
}

static void draw_picture(const C3D_Mtx* ortho)
{
	typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;
	static const V quad[6] = {
		{ 40, 240, 0.5f, 0, 0, {255,255,255,255}, {0,0,127,0} },  { 40, 0, 0.5f, 0, 1, {255,255,255,255}, {0,0,127,0} },
		{ 360, 0, 0.5f, 1, 1, {255,255,255,255}, {0,0,127,0} },   { 40, 240, 0.5f, 0, 0, {255,255,255,255}, {0,0,127,0} },
		{ 360, 0, 0.5f, 1, 1, {255,255,255,255}, {0,0,127,0} },   { 360, 240, 0.5f, 1, 0, {255,255,255,255}, {0,0,127,0} },
	};
	if (!s_picquad) {
		s_picquad = linearAlloc(sizeof quad);
		memcpy(s_picquad, quad, sizeof quad);
	}
	draw_fade(ortho, 1.0f);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexBind(0, &s_pic_tex);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_picquad, ROOM_VERTEX_SIZE, 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
}

// ---------------------------------------------------------------- map screen and memo list

// map.c's 2D service: triangles in top-screen pixels (y down), textured or flat, tinted by an
// 0xRRGGBBAA colour. The vertices go to a linear buffer filled anew every frame; the GPU reads
// them after C3D_FrameEnd, so nothing in it is reused within a frame.
#define UI_MAX_VERTS 8192      // the map screen and up to 1024 glyphs of PC text (font.c)
static void* s_ui_verts;
static int s_ui_used;
static const C3D_Mtx* s_ui_ortho;

static void ui_draw(C3D_Tex* tex, const MapVert* v, int n, u32 rgba)
{
	typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;
	if (!s_ui_verts)
		s_ui_verts = linearAlloc(UI_MAX_VERTS * sizeof(V));
	if (!s_ui_verts || !s_ui_ortho || s_ui_used + n > UI_MAX_VERTS)
		return;
	V* out = (V*)s_ui_verts + s_ui_used;
	for (int i = 0; i < n; i++)
		out[i] = (V){ v[i].x, 240.0f - v[i].y, 0.5f, v[i].u, v[i].v, { 255, 255, 255, 255 }, { 0, 0, 127, 0 } };
	C3D_Mtx id;
	Mtx_Identity(&id);
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, s_ui_ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &id);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (tex) {
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		C3D_TexBind(0, tex);
	} else {
		C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	C3D_TexEnvColor(env, (rgba & 0xff) << 24 | (rgba >> 8 & 0xff) << 16 | (rgba >> 16 & 0xff) << 8 | rgba >> 24);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, out, ROOM_VERTEX_SIZE, 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, n);
	s_ui_used += n;
}

// The 2D program as ui_draw_font sets it up (vertex colour straight through), for noise.c.
static void ui_program_2d(const C3D_Mtx* ortho)
{
	C3D_Mtx id;
	Mtx_Identity(&id);
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &id);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uGain, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uAmb, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uVgain, 1.0f, 1.0f, 1.0f, 1.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uBase, 0.0f, 0.0f, 0.0f, 0.0f);
}

// The floor under a pool or a stain (blood.c), from the room's collision.
static bool blood_floor(float x, float z, float y_ref, float* y)
{
	return s_have_col && col_floor(&s_col, x, z, y_ref, 600.0f, y);
}

// The same for world-space triangles (blood.c): the scene's projection and view.
static void ui_program_flat(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, proj);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, view);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uGain, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uAmb, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uVgain, 1.0f, 1.0f, 1.0f, 1.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uBase, 0.0f, 0.0f, 0.0f, 0.0f);
}

// font.c's glyphs: textured triangles tinted per vertex (the vertex colour passes straight through
// world.v.pica with gain and base 0 and vgain 1).
static void ui_draw_font(C3D_Tex* tex, const FontVert* v, int n)
{
	typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;
	if (!s_ui_verts)
		s_ui_verts = linearAlloc(UI_MAX_VERTS * sizeof(V));
	if (!s_ui_verts || !s_ui_ortho || s_ui_used + n > UI_MAX_VERTS)
		return;
	V* out = (V*)s_ui_verts + s_ui_used;
	for (int i = 0; i < n; i++) {
		u32 c = v[i].rgba;
		out[i] = (V){ v[i].x, 240.0f - v[i].y, 0.5f, v[i].u, v[i].v,
		              { c >> 24, c >> 16 & 0xff, c >> 8 & 0xff, c & 0xff }, { 0, 0, 127, 0 } };
	}
	C3D_Mtx id;
	Mtx_Identity(&id);
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, s_ui_ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &id);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uGain, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uAmb, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uVgain, 1.0f, 1.0f, 1.0f, 1.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uBase, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_TexBind(0, tex);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, out, ROOM_VERTEX_SIZE, 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, n);
	s_ui_used += n;
}

static bool s_memo_open;
static int s_memo_sel, s_memo_view, s_memo_cur;    // view: 0 the list, 1 a picture, 2 its text
static bool s_memo_dirty;

static bool screens_open(void) { return map_is_open() || s_memo_open; }

// The PC's map key (0x48ef03): in a dark room the map needs the flashlight (item 18, switched on,
// not taken away by flags 272 / 625), except in room 0xaa.
static void open_map(void)
{
	bool light = s_flash_on && ev_have_item(18) && !ev_flag(272) && !ev_flag(625);
	if (s_env.dark && !light && bgm_room_id(ev_glb_crd(), s_px, s_pz) != 0xaa) {
		show_message(ev_system_message(2));        // "It's too dark to read the map."
		return;
	}
	MapPlayer mp = { s_px, s_py, s_pz, s_yaw, ev_glb_crd(), prog_chapter() };
	int r = map_open(&mp);
	if (r != MAP_OPEN)
		show_message(ev_system_message(3));        // "I don't have a map of this area."
	if (r != MAP_NONE)
		sfx_play(0x2b17, 1.0f, 0.0f);
}

static void memo_panel(const int* list, int n)
{
	static char buf[500];
	int k = snprintf(buf, sizeof buf, " Memos (%d)\n", n);
	if (!n)
		k += snprintf(buf + k, sizeof buf - k, "\n   No memos yet.\n");
	int first = s_memo_sel - 3;
	first = first > n - 8 ? n - 8 : first;
	first = first < 0 ? 0 : first;
	for (int i = first; i < n && i < first + 8; i++) {
		char line[36];
		snprintf(line, sizeof line, "%s", memo_title(list[i]) ? memo_title(list[i]) : "?");
		for (char* c = line; *c; c++)
			if (*c == '\n' || *c == '\f') *c = ' ';
		k += snprintf(buf + k, sizeof buf - k, "  %c %s\n", i == s_memo_sel ? '>' : ' ', line);
	}
	snprintf(buf + k, sizeof buf - k, "\n   D-pad: choose  A: read  B: close");
	host_panel(buf);
	s_memo_dirty = false;
	{
		static char titles[64][48];
		const char* rows[64];
		for (int i = 0; i < n && i < 64; i++) {
			snprintf(titles[i], sizeof titles[i], "%s", memo_title(list[i]) ? memo_title(list[i]) : "?");
			for (char* c = titles[i]; *c; c++)
				if (*c == '\n' || *c == '\f') *c = ' ';
			rows[i] = titles[i];
		}
		static const TouiButton b[2] = { { "Read", KEY_A }, { "Close", KEY_B } };
		touchui_menu(TOUI_MEMOS, n ? "Memos" : "Memos: none yet", rows, n < 64 ? n : 64, &s_memo_sel, b, 2);
	}
}

static void open_memos(void)
{
	s_memo_open = true;
	s_memo_view = s_memo_sel = 0;
	s_memo_dirty = true;
	host_picture("etc_p_memo");
}

static void close_memos(void)
{
	touchui_menu_close(TOUI_MEMOS);
	touchui_box(NULL, NULL, 0);
	s_memo_open = false;
	host_picture(NULL);
	host_panel(NULL);
}

// Event/memo.c: the memo paper or the memo's own still on top, the list and the texts below.
static bool memo_update(u32 down)
{
	int list[64], n = memo_list(list, 64);
	if (s_memo_view == 2) {          // the text was closed (message_update ran first)
		s_memo_view = 0;
		s_memo_dirty = true;
		host_picture("etc_p_memo");
	}
	if (s_memo_view == 1) {
		if (!(down & (KEY_A | KEY_B)))
			return true;
		if (memo_kind(s_memo_cur) == 2) {
			show_message(memo_text(s_memo_cur));
			s_memo_view = 2;
			return true;
		}
		s_memo_view = 0;
		s_memo_dirty = true;
		host_picture("etc_p_memo");
		touchui_box(NULL, NULL, 0);
		return true;
	}
	if (down & (KEY_B | KEY_START)) {
		sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
		close_memos();
		return true;
	}
	if (n && (down & (KEY_DUP | KEY_DDOWN))) {
		s_memo_sel = (s_memo_sel + (down & KEY_DDOWN ? 1 : n - 1)) % n;
		s_memo_dirty = true;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
	}
	if (n && (down & KEY_A)) {
		s_memo_cur = list[s_memo_sel % n];
		touchui_menu_close(TOUI_MEMOS);
		const char* pic = memo_picture(s_memo_cur, 0);
		host_picture(pic ? pic : "etc_p_memo");
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		if (memo_kind(s_memo_cur) != 1 && pic) {
			s_memo_view = 1;
			host_panel("\n   A: next");
			static const TouiButton next[1] = { { "Next", KEY_A } };
			touchui_box(memo_title(s_memo_cur), next, 1);
		} else {
			host_panel(NULL);
			show_message(memo_text(s_memo_cur));
			s_memo_view = 2;
		}
		return true;
	}
	if (s_memo_dirty)
		memo_panel(list, n);
	return true;
}

// One frame of the map or the memo list; true while one of them is open (the game waits). A tap
// on the touch screen opens the map when the player is free, like the PC's map key.
static bool screens_update(u32 down, u32 held, const circlePosition* cp, float dt)
{
	prog_overlay_memo(s_memo_open && s_memo_view != 0 ? s_memo_cur : -1);   // 0x49a210 (prog_overlay.c)
	touchui_map(map_is_open());      // the map screen's buttons on the touch screen
	touchui_map_readable(!s_env.dark || (s_flash_on && ev_have_item(18) && !ev_flag(272) && !ev_flag(625)) ||
	                     bgm_room_id(ev_glb_crd(), s_px, s_pz) == 0xaa);   // as open_map
	if (!screens_open()) {
		bool free = !prog_running() && !s_inv_open && !s_msg_page && !s_save_prompt && !s_warp_pending;
		if (free && ((down & KEY_TOUCH) || (s_gdown & GK_MAP)))
			open_map();
		if (!screens_open())
			return false;
	}
	if (message_update(down))        // "no map of this area" first, and the memo texts
		return true;
	if (map_is_open()) {
		if (map_update(down, held, cp, dt))
			host_panel(map_legend());
		else
			host_panel(NULL);
		return true;
	}
	return memo_update(down);
}

// Clear data (prog.h): the PC keeps it in its system data, here in a file of its own.
#define CLEAR_PATH PACK_DIR "/clear.dat"
static void clear_load(void)
{
	u8 b[8];
	FILE* f = fopen(CLEAR_PATH, "rb");
	if (f && fread(b, 1, 7, f) == 7 && !memcmp(b, "SHCL", 4))
		prog_clear_set(b + 4);
	if (f)
		fclose(f);
}

static void clear_save(void)
{
	u8 b[7] = { 'S', 'H', 'C', 'L' };
	prog_clear_get(b + 4);
	FILE* f = fopen(CLEAR_PATH, "wb");
	if (f) {
		fwrite(b, 1, 7, f);
		fclose(f);
	}
}

// ---------------------------------------------------------------- puzzle overlays and map marking

// The sprite sheet of the still on screen (pack/sheet/<dir>/<name>.t3x, RGBA4) and this frame's
// sprites (prog_overlay.c fills the list while its program runs; it keeps it while paused).
static C3D_Tex s_sheet_tex;
static bool s_sheet_ok;
typedef struct { float xy[4][2], uv[4], alpha; } OvSprite;
static OvSprite s_sprites[48];
static int s_nsprites;

static bool host_sheet(const char* name)
{
	if (s_sheet_ok) {
		C3D_TexDelete(&s_sheet_tex);
		s_sheet_ok = false;
	}
	s_nsprites = 0;
	if (!name)
		return false;
	char path[96];
	snprintf(path, sizeof path, PACK_DIR "/sheet/%s.t3x", name);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_sheet_tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(&s_sheet_tex, GPU_LINEAR, GPU_LINEAR);
	s_sheet_ok = true;
	return true;
}

static void host_sprite(const float xy[4][2], const float uv[4], float alpha)
{
	if (!xy) {
		s_nsprites = 0;
		return;
	}
	if (s_nsprites >= (int)(sizeof s_sprites / sizeof *s_sprites))
		return;
	OvSprite* s = &s_sprites[s_nsprites++];
	memcpy(s->xy, xy, sizeof s->xy);
	memcpy(s->uv, uv, sizeof s->uv);
	s->alpha = alpha;
}

// Over the still: sprite units are -256..256 across the 320x240 picture area, y down.
static void draw_sprites(void)
{
	if (!s_sheet_ok || !s_pic_shown)
		return;
	for (int i = 0; i < s_nsprites; i++) {
		const OvSprite* s = &s_sprites[i];
		float u0 = s->uv[0] / s_sheet_tex.width, v0 = s->uv[1] / s_sheet_tex.height;
		float u1 = s->uv[2] / s_sheet_tex.width, v1 = s->uv[3] / s_sheet_tex.height;
		MapVert c[4];
		for (int k = 0; k < 4; k++)
			c[k] = (MapVert){ 200.0f + s->xy[k][0] * 0.625f, 120.0f + s->xy[k][1] * 0.46875f,
			                  k & 1 ? u1 : u0, k & 2 ? v1 : v0 };
		const MapVert v[6] = { c[0], c[1], c[3], c[0], c[3], c[2] };
		int a = (int)(s->alpha * 255.0f);
		ui_draw(&s_sheet_tex, v, 6, 0xffffff00 | (u32)(a < 0 ? 0 : a > 255 ? 255 : a));
	}
}

// 0x49c0f0 / 0x49dbd0: a program marks the map; the map screen plays the marks by itself and the
// program goes on once it has closed.
static int s_mark_mode = -1;
static bool host_map_mark(int mode)
{
	if (s_mark_mode < 0) {
		MapPlayer mp = { s_px, s_py, s_pz, s_yaw, ev_glb_crd(), prog_chapter() };
		if (!map_mark_open(mode, &mp))
			return true;
		s_mark_mode = mode;
		sfx_play(0x2b17, 1.0f, 0.0f);
		return false;
	}
	if (map_is_open())
		return false;
	s_mark_mode = -1;
	return true;
}

// ---------------------------------------------------------------- results screen (0x447650)

static bool s_results;
static bool s_results_dirty;
static bool s_clear_saving;      // clearsave.c runs after the results
static void enter_title(void);

static void results_print(void)
{
	ProgResults r;
	prog_results(&r);
	const char* (*m)(int) = ev_result_message;
	static const int action_msg[4] = { 25, 3, 4, 5 }, riddle_msg[4] = { 3, 4, 5, 24 };
	consoleClear();
	printf("\x1b[1;1H---------------- Result ----------------");
	int row = 3;
	static char sum[1400];          // the same lines for the touch screen
	int sn = 0;
#define LINE(label, ...) do { printf("\x1b[%d;2H%-27s", row, (label) ? (label) : ""); printf(__VA_ARGS__); row++; \
		sn += snprintf(sum + sn, sizeof sum - sn, "%s   ", (label) ? (label) : ""); \
		sn += snprintf(sum + sn, sizeof sum - sn, __VA_ARGS__); sn += snprintf(sum + sn, sizeof sum - sn, "\n"); } while (0)
	LINE(m(1), "%s", m(action_msg[r.action & 3]) ? m(action_msg[r.action & 3]) : "");
	LINE(m(2), "%s", m(riddle_msg[r.riddle & 3]) ? m(riddle_msg[r.riddle & 3]) : "");
	int e = r.ending == 1 ? 7 : r.ending == 2 ? 8 : r.ending == 4 ? 9 : r.ending == 8 ? 10 : r.ending == 0x10 ? 11 :
	        r.ending == 0x20 ? 27 : -1;
	LINE(m(6), "%s", e >= 0 && m(e) ? m(e) : "");
	int seen = 0;
	for (int b = 0; b < 7; b++)
		seen += r.endings >> b & 1;
	LINE(m(12), "%d", seen);
	LINE(m(13), "%d", r.saves);
	int t = (int)r.time;
	LINE(m(14), "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
	LINE(m(15), "%.1f km", r.walk_km);
	LINE(m(16), "%.1f km", r.run_km);
	if (r.secret)
		LINE(m(17), "%d(+%d)", r.items, r.secret);
	else
		LINE(m(17), "%d", r.items);
	LINE(m(18), "%d", r.shot);
	LINE(m(19), "%d", r.beaten);
	if (r.chapter != 1) {
		LINE(m(20), "%d:%02d", (int)r.boat_time / 60, (int)r.boat_time % 60);
		LINE(m(21), "%.1f", r.boat_speed);
	}
	LINE(m(22), "%.0f", r.damage);
	if (r.chapter != 1) {
		char stars[24];
		int n = r.rank < 20 ? r.rank : 20;
		memset(stars, '*', n);
		stars[n] = 0;
		LINE(m(23), "%d", r.rank);
		printf("\x1b[%d;4H%s", row, stars);
	}
#undef LINE
	printf("\x1b[29;12H%s", "A: continue");
	static const TouiButton cont[1] = { { "Continue", KEY_A } };
	touchui_box(sum, cont, 1);
	s_results_dirty = false;
}

// The results in the PC's layout on the top screen (0x447650, English positions): the title in the
// big font, the labels right-aligned at 240 in the small font fading from white to grey, the
// values from 256, a value yellow (colour 4) when it earns its category's full points (outside
// Born From a Wish), and the rank as stars, a big one per ten and a small one per point.
static void results_draw_top(void)
{
	if (s_clear_saving) {
		clearsave_draw_top();
		return;
	}
	if (!font_ready())
		return;
	ProgResults r;
	prog_results(&r);
	const char* (*m)(int) = ev_result_message;
	bool wish = r.chapter == 1;
	u32 white, label_top, label_bottom, yellow, dim, dummy;
	font_pc_color(0, &white, &dummy);
	font_pc_color(10, &label_top, &label_bottom);
	font_pc_color(4, &yellow, &dummy);
	dim = 0x606060ff;      // colour 0x11 is past the end of the PC's table: shown dim here
	static const u16 title[] = { 0x182, 0x17c, 0x188, 0x180, 0, 0x18d, 0x180, 0x18e, 0x190, 0x187, 0x18f };
	font_codes(FONT_BIG, 256, 40, title, sizeof title / sizeof *title, FONT_CENTER, white, white);  // GAME RESULT
	static const u8 labels[15] = { 1, 2, 6, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 };   // 0x7b566c
	for (int i = 0; i < 15; i++) {
		bool off = wish && (i == 1 || i == 2 || i == 3 || i == 11 || i == 12 || i == 14);
		font_text(FONT_SMALL, 240, 80 + 24 * i + (i == 14 ? 4 : 0), m(labels[i]), FONT_RIGHT,
		          off ? dim : label_top, off ? dim : label_bottom);
	}
#define VALUE(row, full, ...) do { char b_[48]; snprintf(b_, sizeof b_, __VA_ARGS__); \
		u32 c_ = (full) && !wish ? yellow : white; font_text(FONT_SMALL, 256, 80 + 24 * (row), b_, FONT_LEFT, c_, c_); } while (0)
	static const int action_msg[4] = { 25, 3, 4, 5 }, riddle_msg[4] = { 3, 4, 5, 24 };   // 0x7b567c, 0x7b5680
	const char* s = m(action_msg[r.action & 3]);
	VALUE(0, r.pts_action >= 5, "%s", s ? s : "");
	if (!wish) {
		s = m(riddle_msg[r.riddle & 3]);
		VALUE(1, r.pts_riddle >= 5, "%s", s ? s : "");
	}
	int e = r.ending == 1 ? 7 : r.ending == 2 ? 8 : r.ending == 4 ? 9 : r.ending == 8 ? 10 : r.ending == 0x10 ? 11 :
	        r.ending == 0x20 ? 27 : -1;
	s = e >= 0 ? m(e) : NULL;
	VALUE(2, r.ending == 8 || r.ending == 0x10, "%s", s ? s : "");
	if (!wish) {
		int main_ends = 0, extra = 0;
		for (int b = 0; b < 4; b++)
			main_ends += r.endings >> b & 1;
		for (int b = 4; b < 7; b++)
			extra += r.endings >> b & 1;
		if (extra)
			VALUE(3, r.pts_ends >= 20, "%d/4(+%d)", main_ends, extra);
		else
			VALUE(3, r.pts_ends >= 20, "%d/4", main_ends);
	}
	VALUE(4, r.pts_saves >= 5, "%d", r.saves);
	int t = (int)r.time;
	VALUE(5, r.pts_time >= 10, "%dh %02dm %02ds", t / 3600, t / 60 % 60, t % 60);
	VALUE(6, false, "%.2fkm", r.walk_km);
	VALUE(7, false, "%.2fkm", r.run_km);
	if (r.secret) {
		// "%d%s(+%d)": the secret items found, in yellow once all eight are (the \4 escape)
		char n8[12], p8[12];
		snprintf(n8, sizeof n8, "%d", r.items);
		snprintf(p8, sizeof p8, "(+%d)", r.secret);
		u32 c8 = r.pts_items >= 15 && !wish ? yellow : white, c9 = r.secret >= 8 ? yellow : white;
		font_text(FONT_SMALL, 256, 80 + 24 * 8, n8, FONT_LEFT, c8, c8);
		font_text(FONT_SMALL, 256 + font_width(FONT_SMALL, n8) + 2, 80 + 24 * 8, p8, FONT_LEFT, c9, c9);
	} else {
		VALUE(8, r.pts_items >= 15, "%d", r.items);
	}
	VALUE(9, r.pts_shot >= 15, "%d", r.shot);
	VALUE(10, r.pts_beaten >= 15, "%d", r.beaten);
	if (!wish) {
		int bt = (int)r.boat_time;
		int hund = (int)((r.boat_time - (float)bt) * 100.0f);
		if (bt < 3600)
			VALUE(11, r.pts_boat >= 5, "%dm %02ds %02d", bt / 60 % 60, bt % 60, hund);
		else
			VALUE(11, r.pts_boat >= 5, "%dh %02dm %02ds %02d", bt / 3600, bt / 60 % 60, bt % 60, hund);
		VALUE(12, false, "%.2fm/s", r.boat_speed);
	}
	VALUE(13, r.pts_damage >= 5, "%d pt%s", (int)r.damage, r.damage < 2.0f ? "" : "s");
#undef VALUE
	if (!wish) {
		u16 stars[20];
		u32 c = r.rank >= 100 ? yellow : white;
		int big = r.rank >= 10 ? r.rank / 10 : 0, small = r.rank >= 10 ? r.rank % 10 : r.rank;
		big = big > 20 ? 20 : big;
		for (int i = 0; i < 20; i++)
			stars[i] = 0x138;
		if (big)
			font_codes(FONT_BIG, 256, 416, stars, big, FONT_LEFT, c, c);
		if (small)
			font_codes(FONT_SMALL, 256, big ? 442 : 416, stars, small > 20 ? 20 : small, FONT_LEFT, c, c);
	}
	// "Press Return to continue." (message 0x1c at 35, 460), with the 3DS button
	s = m(28);
	char foot[64];
	const char* ret = s ? strstr(s, "Return") : NULL;
	if (ret)
		snprintf(foot, sizeof foot, "%.*sA%s", (int)(ret - s), s, ret + 6);
	else
		snprintf(foot, sizeof foot, "%s", s ? s : "");
	font_text(FONT_SMALL, 35, 460, foot, FONT_LEFT, white, white);
}

// The PC shows the results after the ending's credits, then saves the clear data (here it was
// written when the ending ran) and goes back to the title.
static bool results_update(u32 down)
{
	if (s_results_dirty)
		results_print();
	if (s_clear_saving) {
		if (!clearsave_update(down)) {
			s_clear_saving = false;
			s_results = false;
			enter_title();
		}
		return true;
	}
	if (down & (KEY_A | KEY_START)) {
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		if (prog_chapter() != 1) {
			// James' endings offer the clear data save (0x447650 case 3); Born From a Wish does not
			static const ClearSaveHost host = { &s_files, ev_result_message };
			clearsave_begin(&host);
			s_clear_saving = true;
			clearsave_update(0);
		} else {
			s_results = false;
			enter_title();
		}
	}
	return true;
}

// PC FadeStart types: 1, 2, 6, 7, 11 fade out; 3, 8 black at once; 4, 9, 12 fade in; 5, 10 clear.
static void host_fade(int type)
{
	switch (type) {
	case 3: case 8: s_fade = 1.0f; // fallthrough
	case 1: case 2: case 6: case 7: case 11: s_fade_target = 1.0f; break;
	case 5: case 10: s_fade = 0.0f; // fallthrough
	default: s_fade_target = 0.0f; break;
	}
}

static bool host_fade_done(void) { return s_fade == s_fade_target; }
static void host_se(int id) { sfx_play(id, 1.0f, 0.0f); }

static void host_player_get(float pos[3], float* yaw)
{
	pos[0] = s_px, pos[1] = s_py, pos[2] = s_pz;
	*yaw = s_yaw;
}

// Room of the current area whose bounds contain (x, z), from the .s3m headers only.
static int room_at(float x, float z)
{
	for (int i = 0; i < s_nrooms; i++) {
		char path[192];
		snprintf(path, sizeof path, PACK_DIR "/bg/%.7s/%.15s.s3m", s_areas[s_area], s_rooms[i]);
		FILE* f = fopen(path, "rb");
		if (!f)
			continue;
		float bb[6];
		bool ok = fseek(f, 8, SEEK_SET) == 0 && fread(bb, sizeof bb, 1, f) == 1;
		fclose(f);
		if (ok && x >= bb[0] && x <= bb[3] && z >= bb[2] && z <= bb[5])
			return i;
	}
	return -1;
}

static void host_player_set(const float pos[3], float yaw)
{
	// CharaSetPos can move James into another room of the stage (the prologue puts him at the
	// mirror in er01): load that room first when he leaves the current one
	bool inside = s_loaded && pos[0] >= s_cur.bbox_min[0] && pos[0] <= s_cur.bbox_max[0] &&
	              pos[2] >= s_cur.bbox_min[2] && pos[2] <= s_cur.bbox_max[2];
	if (!inside) {
		int r = room_at(pos[0], pos[2]);
		if (r >= 0 && r != s_room) {
			s_room = r;
			s_keep_player = true;
			load_current();
			s_keep_player = false;
		}
	}
	s_px = pos[0], s_py = pos[1], s_pz = pos[2], s_yaw = yaw;
	float ny;
	if (s_have_col && col_floor(&s_col, s_px, s_pz, s_py, 600.0f, &ny))
		s_py = ny;
	s_gamecam_reset = true;
}

// Scripted walk (PC 0x53b010): straight at walking speed, gives up after four seconds.
static int s_walk_frames;
static bool host_walk_to(float x, float z)
{
	float dx = x - s_px, dz = z - s_pz, d = sqrtf(dx * dx + dz * dz);
	if (d < 40.0f || ++s_walk_frames > 240) {
		s_walk_frames = 0;
		s_vel[0] = s_vel[1] = 0.0f;
		return true;
	}
	s_yaw = atan2f(dx, dz);
	float step = 975.0f / 60.0f < d ? 975.0f / 60.0f : d;
	float nx = s_px + dx / d * step, nz = s_pz + dz / d * step, ny;
	if (s_have_col) {
		col_push(&s_col, &nx, &nz, s_py, 900.0f, 130.0f);
		if (col_floor_climb(&s_col, nx, nz, s_py, 160.0f, 160.0f, &ny))
			s_px = nx, s_pz = nz, s_py = ny;
	} else {
		s_px = nx, s_pz = nz;
	}
	s_vel[0] = dx / d * 975.0f, s_vel[1] = dz / d * 975.0f;
	return false;
}

// prog_demo_request() lands here, every frame until it returns true. Movies the demo opens with
// ("toilet.sfd + first_toilet") play first (movie.c, blocking); the in-engine part then runs in the
// main loop (demo.c) and the program waits until it is over. A demo the pack has no data for
// counts as watched, so the program goes on with its flags and James' new position.
static int s_demo_req;           // ProgDemo id playing for the program, 0 when none
static bool s_demo_over;         // that scene has ended (or was skipped)

static bool host_demo(int id)
{
	if (s_demo_req == id) {
		if (!s_demo_over)
			return false;
		s_demo_req = 0;
		s_demo_over = false;
		return true;
	}
	// demo.c plays the movies of its sequences itself, between the scenes as the PC does
	const char* name = demo_has_sequence(id) ? "" : prog_demo_name(id);
	for (const char* sfd; (sfd = strstr(name, ".sfd")) != NULL; name = sfd + 4) {
		const char* start = sfd;
		while (start > name && start[-1] != ' ')
			start--;
		if (sfd - start < 24) {
			char movie[24];
			snprintf(movie, sizeof movie, "%.*s", (int)(sfd - start), start);
			movie_play(movie);
		}
	}
	if (!demo_start(PACK_DIR, id))
		return true;
	s_demo_req = id;
	s_demo_over = false;
	return false;
}

// The scene ended: free it, hand the end back to the program and fade the game back in.
static void demo_finish(void)
{
	demo_end();
	s_demo_over = s_demo_req != 0;
	s_gamecam_reset = true;
	s_msg_dirty = true;
	s_fade = 1.0f;               // the scene closed on black
}

static void host_save_menu(void)
{
	s_save_prompt = true;
	s_msg_dirty = true;
}

static void host_flashlight(bool on) { s_flash_on = on; }

// An ending's last program ran (PC 0x48d7e0 records the clear bit and shows the results); the main
// loop goes back to the title once the program has returned.
static int s_game_end = -1;
static void host_game_end(int ending)
{
	s_game_end = ending;
	clear_save();                    // p_game_end recorded the ending in the clear data
}

// EvProgFireDamage (stage 47): a part of James' maximum health.
static void host_damage(float fraction) { combat_damage_james(0x3e, JAMES_HP_MAX * fraction, s_px, s_pz); }

static const ProgHost s_prog_host = {
	host_message, host_message_open, host_panel, host_picture, host_fade, host_fade_done, host_se,
	host_player_get, host_player_set, host_walk_to, host_demo, host_save_menu, host_flashlight,
	host_game_end, host_damage, host_map_mark, host_sheet, host_sprite,
};

// New game: the PC version opens in the observation deck restroom (stage 2, er01). James stands
// inside the door the observation deck uses (its destination record), facing into the room.
static void new_game(void)
{
	prog_abort();
	ev_reset();
	combat_reset();
	enemy_killed_reset();
	prog_new_game(s_riddle, s_action);     // FlagInit: difficulty flags, puzzle answers
	memset(&s_warp, 0, sizeof s_warp);
	s_warp.kind = EV_WARP;
	s_warp.stage = 2;
	strcpy(s_warp.map, "er01");
	s_warp.pos[0] = -19250.0f;
	s_warp.pos[1] = 0.0f;
	s_warp.pos[2] = 21250.0f;
	s_warp.facing = (float)M_PI / 2.0f;
	apply_warp();
	s_cam.mode = CAM_PLAYER;
	s_state = ST_GAME;
	s_fade = 1.0f;               // fade in from black
	consoleClear();
}

// Born From a Wish (DC, gChapterID 1): Maria wakes up in the back room of Heaven's Night (stage 53,
// th09). Its stages are 53..58; the door records carry the stage number, so it stays in them.
static void new_game_wish(void)
{
	prog_abort();
	ev_reset();
	combat_reset();
	enemy_killed_reset();
	prog_new_game_wish(s_riddle, s_action);
	memset(&s_warp, 0, sizeof s_warp);
	s_warp.kind = EV_WARP;
	s_warp.stage = 53;
	strcpy(s_warp.map, "th09");
	s_warp.pos[0] = 59622.3f;
	s_warp.pos[1] = 0.0f;
	s_warp.pos[2] = 20308.75f;
	s_warp.facing = -(float)M_PI / 2.0f;
	apply_warp();
	s_cam.mode = CAM_PLAYER;
	s_state = ST_GAME;
	s_fade = 1.0f;
	consoleClear();
}

static void enter_title(void)
{
	titlemenu_reset(any_save());
	touchui_box(NULL, NULL, 0);
	touchui_panel("");
	map_close();
	s_memo_open = false;
	s_state = ST_TITLE;
	s_title_t = 0.0f;
	prog_abort();
	host_panel(NULL);
	show_message(NULL);
	bgm_stop();
	sfx_stop_all();
	consoleClear();
}

static void load_title(void)
{
	FILE* f = fopen(PACK_DIR "/ui/title.t3x", "rb");
	if (!f)
		return;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_title_tex, NULL, false);
	fclose(f);
	if (!t)
		return;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(&s_title_tex, GPU_LINEAR, GPU_LINEAR);
	s_have_title = true;
}

// The logo strip (512x128) scaled to 400x100 in the middle of the top screen, faded by brightness.
static void draw_title(const C3D_Mtx* ortho, float brightness)
{
	if (!s_have_title)
		return;
	typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;
	static const V quad[6] = {
		{ 0, 170, 0.5f, 0, 0, {255,255,255,255}, {0,0,127,0} },   { 0, 70, 0.5f, 0, 1, {255,255,255,255}, {0,0,127,0} },
		{ 400, 70, 0.5f, 1, 1, {255,255,255,255}, {0,0,127,0} },  { 0, 170, 0.5f, 0, 0, {255,255,255,255}, {0,0,127,0} },
		{ 400, 70, 0.5f, 1, 1, {255,255,255,255}, {0,0,127,0} },  { 400, 170, 0.5f, 1, 0, {255,255,255,255}, {0,0,127,0} },
	};
	if (!s_titlequad) {
		s_titlequad = linearAlloc(sizeof quad);
		memcpy(s_titlequad, quad, sizeof quad);
	}
	C3D_Mtx id;
	Mtx_Identity(&id);
	use_world_program();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, ortho);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &id);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	u8 b = (u8)(brightness * 255.0f);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_TexEnvColor(env, 0xFF000000 | b << 16 | b << 8 | b);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexBind(0, &s_title_tex);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_titlequad, ROOM_VERTEX_SIZE, 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
}

static int s_title_sub;          // the title's level screens (defined with them below)

// Returns false to exit the app.
static bool continue_game(void)
{
	static SaveFile sv;
	if (!read_save(&sv))
		return false;
	if (sv.clear_save) {
		// a clear data save (0x402d00 case 10): its clear data joins the system's and a New Game
		// starts from the level screens, so FlagInit gives the New Game+ items and endings
		prog_clear_merge(sv.clear);
		clear_save();
		s_title_sub = 1;
		consoleClear();
		return true;
	}
	prog_abort();
	ev_reset();
	combat_reset();
	ev_state_set(sv.events);
	if (sv.version >= 3)
		prog_state_set(sv.prog);
	enemy_killed_reset();
	if (sv.version >= 4) {
		enemy_killed_set(sv.killed);       // before the warp: entering the room spawns enemies
		combat_set_hp(sv.hp);
	}
	memset(&s_warp, 0, sizeof s_warp);
	s_warp.kind = EV_WARP;
	s_warp.stage = sv.stage;
	memcpy(s_warp.map, sv.map, sizeof s_warp.map);
	s_warp.map[7] = 0;
	memcpy(s_warp.pos, sv.pos, sizeof s_warp.pos);
	s_warp.facing = sv.yaw;
	apply_warp();
	s_cam.mode = CAM_PLAYER;
	s_state = ST_GAME;
	s_fade = 1.0f;
	consoleClear();
	return true;
}

#define OPTIONS_PATH PACK_DIR "/options.txt"

static void options_load(void)
{
	opt_defaults(&s_opt);
	opt_load(&s_opt, OPTIONS_PATH);
}

// s_opt into the game's settings
static void options_apply(void)
{
	s_bright = s_opt.bright;
	s_controls = s_opt.control_type;
	s_toggle_run = s_opt.toggle_run;
	s_ps2_flash = s_opt.ps2_flash;
	s_hold_stomp = s_opt.hold_stomp, s_swap_attack = s_opt.swap_attack;
	s_ps2_camera = s_opt.ps2_camera, s_cutscene_fov = s_opt.cutscene_fov;
	s_msg_top = s_opt.msg_top;
	ev_set_bullet_adjust(s_opt.bullet);
	combat_set_options(s_hold_stomp, s_swap_attack);
	sh2cam_set_ps2_speed(s_ps2_camera);
	demo_set_reduce_fov(s_cutscene_fov);
	env_set_ps2_flashlight(s_ps2_flash);
	// BGM and SE Volume 0..15 (the PC sends them as volume << 3, 0x515760)
	audio_set_gains(s_opt.bgm / 15.0f, s_opt.se / 15.0f);
	sfx_set_gain(s_opt.se / 15.0f);
	audio_set_output(s_opt.speaker);
	map_set_open_zoomed(s_opt.map_zoom == 0);
	noise_set(s_opt.noise, s_opt.bright, s_opt.noise_ps2);
	blood_set_color(s_opt.blood);
	profile_set_choices(s_opt.hwprofile, s_opt.quality, s_opt.ctrllayout, s_opt.framerate);   // profile.c
	s_env_dirty = true;
}

static void options_save(void)
{
	s_opt.bright = (u8)s_bright;
	s_opt.control_type = (u8)s_controls;
	s_opt.bullet = (u8)ev_bullet_adjust();
	opt_save(&s_opt, OPTIONS_PATH);
}

// The options screens' host: every change goes into the game at once (the flashlight's look and
// the light scale want the room's lights rebuilt), the file is written when the changes are kept.
static void opt_changed(const Options* o)
{
	(void)o;
	options_apply();
	env_room_changed();
}

static void opt_keep(const Options* o)
{
	(void)o;
	opt_save(&s_opt, OPTIONS_PATH);
}

static void options_open(void)
{
	u8 clear[3];
	prog_clear_get(clear);
	OptHost host = { opt_changed, opt_keep, clear[1] != 0 };
	optmenu_open(&s_opt, &host);
}

// The title entry being started (0 new game, 1 Born From a Wish, 2 continue) while the title fades
// out, -1 when none.
static int s_title_go = -1;
static float s_title_go_t;
#define TITLE_GO_SECONDS 1.0f

static void title_go(void)
{
	int what = s_title_go;
	s_title_go = -1;
	if (what == 0)
		new_game();
	else if (what == 1)
		new_game_wish();
	else if (!continue_game())
		printf("\x1b[25;9HNo save file yet.        ");
}

// Starting a game plays the PC's game start sound (15002: new game 0x498201, continue 0x4978c4)
// while the title fades out, and the game starts once it is black. Enhanced Edition
// "PreserveSoundsOnLoad" (Patches/PlayWavSound.cpp, zlib licence) exists because the PC cuts this
// sound off while it loads; the port never stops effects for a load, so a continue plays it out (a
// new game's opening movie ends it, see movie_play).
static void title_start(int what)
{
	sfx_play(SFX_GAME_START, 1.0f, 0.0f);
	s_title_go = what;
	s_title_go_t = 0.0f;
	// Continue: the saved room is read ahead during that second, as a door's would be
	static SaveFile sv;
	if (what == 2 && read_save(&sv) && !sv.clear_save) {
		char map[8];
		memcpy(map, sv.map, sizeof map);
		map[7] = 0;
		warp_prefetch(map, sv.stage, sv.pos);
	}
}

// New Game and Born From a Wish first ask for the action and riddle levels, as the PC does; hard
// riddles become the extra level once FlagInit allows it (prog_extra_riddle).
static int s_title_sub;          // 1: New Game levels, 2: Born From a Wish levels

static bool title_levels(u32 down)
{
	static const char* const action[4] = { "Beginner", "Easy", "Normal", "Hard" };
	static const char* const riddle[4] = { "Easy", "Normal", "Hard", "Extra" };
	static int sel;
	if (down & KEY_B) {
		s_title_sub = 0;
		consoleClear();
		return true;
	}
	if (down & (KEY_DUP | KEY_DDOWN)) {
		sel = (sel + (down & KEY_DDOWN ? 1 : 2)) % 3;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
	}
	if (s_riddle > 2)
		s_riddle = 2;
	if (down & (KEY_DLEFT | KEY_DRIGHT)) {
		int d = down & KEY_DRIGHT ? 1 : -1;
		if (sel == 0) s_action = (s_action + d + 4) % 4;
		if (sel == 1) s_riddle = (s_riddle + d + 3) % 3;
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
	}
	if ((down & KEY_A) && sel == 2) {
		int sub = s_title_sub;
		s_title_sub = 0;
		title_start(sub == 2 ? 1 : 0);
		return true;
	}
	const char* r = s_riddle == 2 && prog_extra_riddle() ? riddle[3] : riddle[s_riddle];
	printf("\x1b[10;12HSILENT HILL 2");
	printf("\x1b[12;9H%-24s", s_title_sub == 2 ? "Born From a Wish" : "New Game");
	printf("\x1b[15;7H%c Action level  < %-8s >", sel == 0 ? '>' : ' ', action[s_action & 3]);
	printf("\x1b[17;7H%c Riddle level  < %-8s >", sel == 1 ? '>' : ' ', r);
	printf("\x1b[19;7H%c Start                    ", sel == 2 ? '>' : ' ');
	printf("\x1b[27;2HD-pad: choose, change  A: go  B: back");
	char ra[40], rr[40];
	snprintf(ra, sizeof ra, "Action level   < %s >", action[s_action & 3]);
	snprintf(rr, sizeof rr, "Riddle level   < %s >", r);
	const char* rows[3] = { ra, rr, "Start" };
	static const TouiButton back[1] = { { "Back", KEY_B } };
	touchui_menu(TOUI_LEVELS, s_title_sub == 2 ? "Born From a Wish" : "New Game", rows, 3, &sel, back, 1);
	return true;
}

// The file CONTINUE loads: the one played longest (the saves carry no date; the load screen
// starts on the same one).
static int newest_slot(void)
{
	int best = 0;
	float t = -1.0f;
	for (int i = 0; i < SAVE_SLOTS; i++) {
		SaveSlotInfo in;
		memset(&in, 0, sizeof in);
		if (files_info(i, &in) && !in.clear && in.time > t)
			t = in.time, best = i;
	}
	return best;
}

static bool title_update(u32 down, float dt)
{
	if (s_title_go >= 0)
		return true;               // fading out to the game: no input
	if (savescreen_active()) {
		if (!savescreen_update(down))
			titlemenu_reset(any_save());
		return true;
	}
	if (optmenu_active()) {
		if (!optmenu_update(down))
			consoleClear();
		return true;
	}
	if (s_title_sub)
		return title_levels(down);
	if (down & KEY_START)
		return false;
	// the PC's title screens (titlemenu.c): the menu, the scenarios, the levels, the movies
	int action = s_action, riddle = s_riddle;
	const char* movie = NULL;
	switch (titlemenu_update(down, dt, &action, &riddle, &movie)) {
	case TM_LOAD:
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		savescreen_open(SS_LOAD, &s_files, 0);     // files_load starts the chosen one
		break;
	case TM_CONTINUE:
		files_load(newest_slot());                 // the game start sound and the fade
		break;
	case TM_NEW_GAME:
		s_action = action, s_riddle = riddle;
		title_start(0);
		break;
	case TM_WISH:
		s_action = action, s_riddle = riddle;
		title_start(1);
		break;
	case TM_OPTIONS:
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		touchui_menu_close(TOUI_TITLE);
		options_open();
		break;
	case TM_MOVIE:
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		touchui_menu_close(TOUI_TITLE);
		touchui_menu_close(TOUI_LEVELS);
		movie_play(movie);
		break;
	case TM_ATTRACT: {
		u8 clear[3];
		prog_clear_get(clear);
		touchui_menu_close(TOUI_TITLE);
		movie_play(clear[1] ? "open_voc" : "open_bgm");     // 0x43e750
		titlemenu_reset(any_save());
		break;
	}
	case TM_EXIT:
		return false;
	case TM_VIEWER:
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		touchui_menu_close(TOUI_TITLE);
		s_state = ST_VIEWER;
		s_cam.mode = CAM_ORBIT;
		consoleClear();
		load_current();
		stage_for_room();
		break;
	}
	return true;
}

// ---------------------------------------------------------------- pause menu (START in game)

static bool s_paused;
static bool s_pause_dirty;

static void pause_draw(void)
{
	if (!s_pause_dirty)
		return;
	s_pause_dirty = false;
	for (int r = 18; r < 30; r++)
		printf("\x1b[%d;1H%40s", r, "");
	if (!s_paused) {
		s_msg_dirty = true;
		pausemenu_close();
		return;
	}
	// the PC's pause menu is on the top screen and the touch screen (pausemenu.c); the settings and
	// the Old 3DS profile's rows are on the options screens
	printf("\x1b[18;1H---------------- Pause -----------------");
}

// 0x44c7d0: Save Game works anywhere but in stages 27, 37 and 48..52, and not while a stage
// program holds James (0x79b830); the file's place is the stage's name (0x4551d0: 0x9d + stage,
// m_card_x_msg_e.mes 157..)
static bool pause_can_save(void)
{
	int st = ev_stage();
	if (st == 27 || st == 37 || (st >= 48 && st <= 52))
		return false;
	return !prog_running();
}

// Returns true while the game is paused (the caller skips the game update).
static bool pause_update(u32 down)
{
	if (!s_paused) {
		if ((down & KEY_START) && !s_msg_page && !s_save_prompt && !s_inv_open && !s_warp_pending &&
		    !screens_open()) {
			s_paused = true;
			s_pause_dirty = true;
			pausemenu_open(pause_can_save());
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			return true;
		}
		return false;
	}
	if (optmenu_active()) {
		if (!optmenu_update(down)) {
			s_pause_dirty = true;            // back on the pause menu
			pausemenu_open(pause_can_save());
		}
		return true;
	}
	if (savescreen_active()) {
		if (!savescreen_update(down))
			pausemenu_open(pause_can_save());
		return true;
	}
	switch (pausemenu_update(down)) {
	case PM_RESUME:
		s_paused = false;
		s_pause_dirty = true;
		break;
	case PM_SAVE:
		pausemenu_close();
		s_save_place = 157 + ev_stage();
		savescreen_open(SS_SAVE, &s_files, prog_chapter());
		break;
	case PM_LOAD:
		// the PC's load screen (menu event 0x11); the port shows it over the title
		s_paused = false;
		enter_title();
		savescreen_open(SS_LOAD, &s_files, 0);
		break;
	case PM_OPTIONS:
		pausemenu_close();
		options_open();
		break;
	case PM_QUIT:
		s_paused = false;
		enter_title();
		break;
	case PM_MAP:                     // the port's own rows on the touch screen
		s_paused = false;
		s_pause_dirty = true;
		open_map();
		break;
	case PM_MEMOS:
		s_paused = false;
		s_pause_dirty = true;
		if (!prog_running())
			open_memos();
		break;
	}
	return true;
}

// ---------------------------------------------------------------- messages on the top screen

// The PC's message window (0x47ff50 without a position) in the big font: the block of lines
// centred on its widest line and left-aligned from there, the first line at 476 - (lines + 4) * 15
// (another half line higher for each line past three). The pack's messages no longer carry the
// PC's position words, so all of them sit there. A one-pixel dark shadow (not on the PC) keeps the
// text readable over bright rooms.
static void draw_message_top(const char* page)
{
	if (!page || !*page || !font_ready())
		return;
	char buf[640];
	int n = 0, lines = 1;
	for (; page[n] && page[n] != '\f' && n < (int)sizeof buf - 1; n++) {
		buf[n] = page[n];
		lines += page[n] == '\n';
	}
	buf[n] = 0;
	int h = font_line_height(FONT_BIG);
	float y = 476.0f - (float)((lines + 4) * h / 2);
	if (lines > 3)
		y -= (float)((lines - 3) * h / 2);
	float x = 256.0f - (font_width(FONT_BIG, buf) + 2.0f) * 0.5f;
	u32 white, dummy;
	font_pc_color(0, &white, &dummy);
	font_text(FONT_BIG, x + 1.6f, y + 2.1f, buf, FONT_LEFT, 0x000000c0, 0x000000c0);
	font_text(FONT_BIG, x, y, buf, FONT_LEFT, white, white);
}

static void draw_top_text(void)
{
	if (!s_msg_top || !font_ready() || s_paused)
		return;
	if (demo_active() || (s_free_voice && !s_msg_page && !s_save_prompt)) {
		int serial;
		const char* sub = demo_subtitle(&serial);
		draw_message_top(s_opt.subtitles ? sub : NULL);
	} else if (s_msg_page && !s_inv_open && !s_save_prompt) {
		draw_message_top(s_msg_page);
	}
}

// ---------------------------------------------------------------- frame dump (debug.txt "dumpframe N")
//
// Copies the top render target through the display transfer (as citro3d does for the screen) into
// memory and writes it to sdmc:/3ds/sh23ds/frame_<N>.bmp, with a line of state in frame_<N>.txt.
// It works under emulators' hardware renderers (the CPU read makes them flush the GPU surface) and
// on a console, so a black screen can be told apart from missing data.
static bool s_debug_newgame;
static int s_debug_weapon;           // debug.txt "weapon <item>": equipped on the first free frame
static u32 s_dump_frames[8];
static int s_ndump;

static void dump_frame(C3D_RenderTarget* top, u32 n)
{
	C3D_FrameSync();
	u8* buf = linearAlloc(240 * 400 * 3);
	if (!buf)
		return;
	GSPGPU_FlushDataCache(buf, 240 * 400 * 3);
	C3D_SyncDisplayTransfer((u32*)top->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), (u32*)buf,
	                        GX_BUFFER_DIM(240, 400), DISPLAY_TRANSFER_FLAGS);
	GSPGPU_InvalidateDataCache(buf, 240 * 400 * 3);
	char path[96];
	snprintf(path, sizeof path, PACK_DIR "/frame_%lu.bmp", (unsigned long)n);
	FILE* f = fopen(path, "wb");
	if (f) {
		// 240x400 BGR, rows as the GPU stores them (the screen is rotated; tools rotate it back)
		u32 size = 54 + 240 * 400 * 3;
		u8 h[54] = { 'B', 'M' };
		memcpy(h + 2, &size, 4);
		u32 v = 54; memcpy(h + 10, &v, 4);
		v = 40; memcpy(h + 14, &v, 4);
		v = 240; memcpy(h + 18, &v, 4);
		v = 400; memcpy(h + 22, &v, 4);
		u16 planes = 1, bpp = 24; memcpy(h + 26, &planes, 2); memcpy(h + 28, &bpp, 2);
		fwrite(h, 1, 54, f);
		fwrite(buf, 1, 240 * 400 * 3, f);
		fclose(f);
	}
	linearFree(buf);
	snprintf(path, sizeof path, PACK_DIR "/frame_%lu.txt", (unsigned long)n);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "state %d room %s loaded %d nb %d stage %d pos %.0f %.0f %.0f fade %.2f demo %d prog %d\n",
		        s_state, s_status, s_loaded, s_nnb, ev_stage(), s_px, s_py, s_pz, s_fade, demo_active(),
		        prog_running());
		fprintf(f, "env dark %d clear %.0f %.0f %.0f fog %.0f..%.0f clip %.0f..%.0f bright %d flash %d\n",
		        s_env.dark, s_env.clear_rgb[0], s_env.clear_rgb[1], s_env.clear_rgb[2], s_env.fog_start,
		        s_env.fog_end, s_env.clip_near, s_env.clip_far, s_bright, s_flash_on);
		fprintf(f, "linear free %lu KB, verts %lu\n", (unsigned long)(linearSpaceFree() / 1024),
		        (unsigned long)s_cur.n_vertices);
		fclose(f);
	}
}

// ---------------------------------------------------------------- autotest (debug.txt)

// "autotest warps [first] [count]": every door destination of every stage, in stage order. Each line
// of autotest.txt is written in two halves, the destination before the load and the result after
// eight frames, so a crash leaves the destination it died on as the last, unfinished line.
static int s_at_first, s_at_count = 100000, s_at_i, s_at_n, s_at_frame = -1;
static bool s_at_perf;               // "autotest warps first count perf": 1.5 s of frame times per room
static char s_at_seen[512][8];       // rooms measured already (perf mode)
static int s_at_nseen;
static u16 s_at_list[1024];          // stage << 10 | door index in the stage
static FILE* s_at_log;
static float s_at_ms;

static void autotest_begin(void)
{
	s_at_n = 0;
	for (int st = 1; st < 64 && s_at_n < 1024; st++) {
		if (!ev_load_stage(st))
			continue;
		int n = ev_warp_count();
		for (int k = 0; k < n && k < 1024 && s_at_n < 1024; k++)
			s_at_list[s_at_n++] = (u16)(st << 10 | k);
	}
	s_at_log = fopen(PACK_DIR "/autotest.txt", s_at_first ? "a" : "w");
	if (s_at_log) {
		fprintf(s_at_log, "# autotest warps: %d destinations, this run %d..%d\n", s_at_n, s_at_first,
		        s_at_first + s_at_count - 1);
		fflush(s_at_log);
	}
	s_at_i = s_at_first;
	s_at_frame = -1;
	if (s_at_perf) {
		FILE* f = fopen(PACK_DIR "/autoperf_seen.txt", "r");
		char line[32];
		while (f && fgets(line, sizeof line, f) && s_at_nseen < 512)
			if (sscanf(line, "%7s", s_at_seen[s_at_nseen]) == 1)
				s_at_nseen++;
		if (f)
			fclose(f);
	}
	new_game();
}

static void autotest_frame(u32* down, u32* held)
{
	*down = *held = 0;
	if (!s_at_log) {
		s_autotest = 0;
		return;
	}
	combat_set_hp(JAMES_HP_MAX);     // enemies in the rooms must not end the run
	if (s_at_frame < 0) {
		if (s_at_i >= s_at_n || s_at_i >= s_at_first + s_at_count) {
			fprintf(s_at_log, "# done, next %d\n", s_at_i);
			fclose(s_at_log);
			s_at_log = NULL;
			s_autotest = 0;
			return;
		}
		int st = s_at_list[s_at_i] >> 10, k = s_at_list[s_at_i] & 1023;
		if (st != ev_stage())
			ev_load_stage(st);
		EvAction a;
		if (!ev_warp_get(k, &a)) {
			fprintf(s_at_log, "%d stage %d door %d: no record\n", s_at_i, st, k);
			fflush(s_at_log);
			s_at_i++;
			return;
		}
		if (s_at_perf) {
			// perf mode measures each room once: the doors into a room it has seen are skipped,
			// across runs too (autoperf_seen.txt on the SD)
			bool dup = false;
			for (int i = 0; i < s_at_nseen && !dup; i++)
				dup = !strcmp(s_at_seen[i], a.map);
			if (dup) {
				s_at_i++;
				return;
			}
			if (s_at_nseen < 512)
				snprintf(s_at_seen[s_at_nseen++], sizeof s_at_seen[0], "%s", a.map);
		}
		fprintf(s_at_log, "%d st%d ev%d -> st%d %s (%.0f %.0f %.0f)", s_at_i, st, a.event, a.stage, a.map,
		        a.pos[0], a.pos[1], a.pos[2]);
		fflush(s_at_log);
		u64 t0 = svcGetSystemTick();
		s_warp = a;
		s_warp_pending = false;
		apply_warp();
		s_gamecam_reset = true;
		s_at_ms = (float)(svcGetSystemTick() - t0) * 1000.0f / (float)SYSCLOCK_ARM11;
		s_at_frame = 0;
		return;
	}
	++s_at_frame;
	if (s_at_perf && s_at_frame == 10)
		perf_window_reset();             // the load and its first frames are not counted
	if (s_at_frame < (s_at_perf ? 100 : 8))
		return;
	float ny;
	bool floor = s_have_col && col_floor(&s_col, s_px, s_pz, s_py, 600.0f, &ny);
	const char* room = s_nrooms && s_loaded ? s_rooms[s_room] : "-";
	struct mallinfo mi = mallinfo();
	fprintf(s_at_log, " | room %s%s loaded %d col %d floor %d moved %.0f roads %d type %d miss %lu linear %luK"
	        " heap %luK load %.0fms\n", room, s_warp.map[0] && strcmp(room, s_warp.map) ? " WRONG" : "", s_loaded, s_have_col, floor,
	        hypotf(s_px - s_warp.pos[0], s_pz - s_warp.pos[2]), gamecam_road_count(), gamecam_mv_type(),
	        (unsigned long)s_cur.missing_textures, (unsigned long)(linearSpaceFree() / 1024),
	        (unsigned long)(mi.uordblks / 1024), s_at_ms);
	if (s_at_perf) {
		PerfWindow w;
		perf_window_get(&w);
		fprintf(s_at_log, "  perf %s frames %d max %.1f over17 %d over33 %d upd %.2f cpu %.2f work %.1f\n", room,
		        w.frames, w.max_ms, w.over17, w.over33, w.upd_avg, w.cpu_avg, w.max_work);
		FILE* f = fopen(PACK_DIR "/autoperf_seen.txt", "a");   // measured: later runs skip it
		if (f) {
			fprintf(f, "%s\n", s_warp.map);
			fclose(f);
		}
	}
	fflush(s_at_log);
	s_at_i++;
	s_at_frame = -1;
}

// ---------------------------------------------------------------- playtest driver (debug.txt "pt" lines)

static bool pt_floor(float x, float z, float y_ref, float max_step, float* y)
{
	return s_have_col && col_floor(&s_col, x, z, y_ref, max_step, y);
}

static void pt_warp(int stage, const char* map, const float pos[3], float yaw)
{
	memset(&s_warp, 0, sizeof s_warp);
	s_warp.kind = EV_WARP;
	s_warp.stage = stage;
	snprintf(s_warp.map, sizeof s_warp.map, "%s", map);
	memcpy(s_warp.pos, pos, sizeof s_warp.pos);
	s_warp.facing = yaw;
	s_warp_pending = true;
	warp_prefetch(s_warp.map, s_warp.stage, s_warp.pos);
}

static void pt_use_item(int item) { s_use_item = item; }
static void pt_new_game(int wish) { if (wish) new_game_wish(); else new_game(); }
static void pt_shot(void) { s_pt_shot = true; }
static void pt_quit(void) { s_pt_quit = true; }
static const Collision* pt_collision(void) { return s_have_col ? &s_col : NULL; }
static int pt_inv_index(int item)
{
	int items[64], n = inv_list(items, 64);
	for (int i = 0; i < n; i++)
		if (items[i] == item)
			return i;
	return -1;
}

static const PtHost s_pt_host = {
	host_player_set, pt_floor, pt_warp, pt_use_item, save_game, continue_game, pt_new_game, pt_shot, pt_quit,
	pt_inv_index, pt_collision,
};

static void pt_frame(u32* down, u32* held, float dt)
{
	PtState st = {
		.state = s_results ? PT_RESULTS : s_state == ST_TITLE ? PT_TITLE : s_state == ST_GAME ? PT_GAME : PT_OTHER,
		.stage = ev_stage(), .map = s_nrooms ? s_rooms[s_room] : "", .pos = { s_px, s_py, s_pz }, .yaw = s_yaw,
		.loading = s_warp_pending || s_arrival || s_fade > 0.02f, .message = s_msg_page != NULL,
		.save_prompt = s_save_prompt, .panel = s_panel[0] != 0, .picture = s_pic_shown, .inv = s_inv_open,
		.paused = s_paused, .screens = screens_open(), .prog = prog_running(), .prog_locked = prog_player_locked(),
		.demo = demo_active(), .hp = combat_hp(), .dead = combat_dead(),
	};
	playtest_frame(&st, down, held, dt);
}

int main(void)
{
	profile_init(PACK_DIR "/options.txt");   // Old / New 3DS profile, clock, core 1 (profile.c)
	s_is_new3ds = profile_console_new();

	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
	profile_apply_frame_rate();          // 30 fps on the low quality level (profile.c)
	audio_init();
	sfx_init();
	bgm_init();
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	setup_pipeline();
	movie_init(top);
	touchui_init();                  // the touch screen is drawn from here on (touchui.c)

	C3D_Mtx proj;
	Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 20.0f, 100000.0f, false);
	C3D_Mtx ortho;
	Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
	s_ui_ortho = &ortho;
	map_init(ui_draw);
	font_init(PACK_DIR, ui_draw_font);
	noise_init();
	blood_init(PACK_DIR, ui_program_flat, blood_floor);
	gameover_init(PACK_DIR);
	titlemenu_init(PACK_DIR);
	titlemenu_reset(any_save());
	titlemenu_boot();

	options_load();     // debug.txt below may override
	controls_init(PACK_DIR);            // the player's button bindings (controls.txt)
	options_apply();
	clear_load();
	FILE* dbg = fopen(PACK_DIR "/debug.txt", "r");
	if (dbg) {
		char line[128], key[32];
		int val;
		while (fgets(line, sizeof line, dbg)) {
			if (sscanf(line, "%31s", key) != 1 || playtest_line(line))
				continue;
			if (!strcmp(key, "spawn")) {
				// spawn <map> <x> <y> <z> <yaw degrees> <camera mode>: start in player mode at a pose
				float x, y, z, deg;
				int mode;
				if (sscanf(line, "%*s %7s %f %f %f %f %d", s_spawn_map, &x, &y, &z, &deg, &mode) == 6) {
					s_spawn[0] = x, s_spawn[1] = y, s_spawn[2] = z, s_spawn[3] = deg * (float)M_PI / 180.0f;
					s_spawn_mode = mode;
				}
				continue;
			}
			if (!strcmp(key, "demo")) {
				sscanf(line, "%*s %31s %f", s_debug_demo, &s_debug_demo_at);
				demo_debug_hold(strstr(line, " hold") != NULL);
				continue;
			}
			if (!strcmp(key, "autotest")) {
				char what[16] = "";
				char mode[16] = "";
				sscanf(line, "%*s %15s %d %d %15s", what, &s_at_first, &s_at_count, mode);
				s_autotest = !strcmp(what, "warps");
				s_at_perf = !strcmp(mode, "perf");
				continue;
			}
			if (!strcmp(key, "movie")) {
				sscanf(line, "%*s %23s", s_debug_movie);
				continue;
			}
			if (sscanf(line, "%31s %d", key, &val) != 2)
				continue;
			if (!strcmp(key, "rendermode")) s_render_mode = val;
			if (!strcmp(key, "perflog")) s_perflog = val != 0;
			if (!strcmp(key, "memlog")) s_memlog = val != 0;
			if (!strcmp(key, "memtest")) s_memtest = val;
			if (!strcmp(key, "streamsync")) s_stream_sync = val != 0;
			if (!strcmp(key, "shadowdebug")) shadow_debug(val);
			if (!strcmp(key, "fogdebug")) fog_debug(val);
			if (!strcmp(key, "fogsprites") || !strcmp(key, "shadows") || !strcmp(key, "framecap") ||
			    !strcmp(key, "tiledist")) {
				Quality q = *quality(); // quality.h knobs, normally the hardware profile's
				if (!strcmp(key, "fogsprites")) q.fog_sprites = val;
				if (!strcmp(key, "shadows")) q.stencil_shadows = q.drop_shadows = val != 0;
				if (!strcmp(key, "framecap")) q.frame_cap = val;
				if (!strcmp(key, "tiledist")) q.tile_distance = (float)val;
				quality_set(&q);
			}
			if (!strcmp(key, "sfxsync")) sfx_sync_loads(val != 0);
			if (!strcmp(key, "batch")) s_only_batch = val;
			if (!strcmp(key, "freeze") && val) s_orbit_speed = 0.0f;
			if (!strcmp(key, "debughud")) s_debug_hud = val != 0;
			if (!strcmp(key, "god")) combat_set_god(val != 0);   // enemy blows do not reach James
			if (!strcmp(key, "logos") && !val) titlemenu_no_boot();   // no start-up screens
			if (!strcmp(key, "bloodtest")) s_blood_test = val != 0;
			if (!strcmp(key, "results")) s_debug_results = val;
			if (!strcmp(key, "itemget") && s_debug_nitemget < 8) s_debug_itemget[s_debug_nitemget++] = val;
			if (!strcmp(key, "ambient")) s_ambient = val / 100.0f;     // percent
			if (!strcmp(key, "brightness")) s_bright = val < 1 ? 1 : val > 7 ? 7 : val;
			if (!strcmp(key, "dumpframe") && s_ndump < 8) s_dump_frames[s_ndump++] = (u32)val;
			if (!strcmp(key, "newgame")) s_debug_newgame = val != 0;
			if (!strcmp(key, "controls")) s_controls = val == 1;
			if (!strcmp(key, "riddle")) s_riddle = val;
			if (!strcmp(key, "action")) s_action = val;
			if (!strcmp(key, "stage")) s_spawn_stage = val;
			if (!strcmp(key, "movielog")) movie_debug(val & 1, val & 2);   // 1: log times, 2: CPU YUV
		}
		fclose(dbg);
	}
	{
		char err[128];
		s_have_james = chr_load(&s_james, PACK_DIR "/chr/jms", "hll_jms", err, sizeof err);
		if (s_have_james) {
			anim_load(&s_james_anim, PACK_DIR "/chr/jms/jms_basic1.s3a");   // clips: move.c's PC table
			move_reset();
			combat_init(PACK_DIR, &s_james);
		}
		enemy_init(PACK_DIR);
		enemy_set_difficulty(s_action);
	}
	prog_init(&s_prog_host);
	playtest_init(&s_pt_host);
	movie_skip_all(playtest_active());
	list_areas();
	for (int i = 0; i < s_nareas; i++)
		if (!strcmp(s_areas[i], "ap"))
			s_area = i;
	list_rooms();
	if (s_spawn_map[0]) {
		s_warp.kind = EV_WARP;
		snprintf(s_warp.map, sizeof s_warp.map, "%s", s_spawn_map);
		memcpy(s_warp.pos, s_spawn, sizeof s_warp.pos);
		s_warp.facing = s_spawn[3];
		s_warp.stage = s_spawn_stage ? s_spawn_stage : ev_stage_for_map(s_spawn_map);
		prog_new_game(s_riddle, s_action);
		dbg = fopen(PACK_DIR "/debug.txt", "r");
		if (dbg) {
			// debug.txt "flag <n> <0|1>" and "item <id> <count>": test setups for the stage programs
			char line[128];
			int a, b;
			while (fgets(line, sizeof line, dbg)) {
				if (sscanf(line, "flag %d %d", &a, &b) == 2) ev_flag_set(a, b != 0);
				if (sscanf(line, "item %d %d", &a, &b) == 2) ev_give_item(a, b);
				if (sscanf(line, "weapon %d", &a) == 1) s_debug_weapon = a;   // equip this item once in play
				if (sscanf(line, "hp %d", &a) == 1) combat_set_hp((float)a);   // e.g. the low health cross
				prog_debug_line(line);
			}
			fclose(dbg);
		}
		apply_warp();
		s_cam.mode = s_spawn_mode;
		if (s_debug_demo[0] && s_spawn_mode == CAM_PLAYER && demo_start_key(PACK_DIR, s_debug_demo))
			demo_seek(s_debug_demo_at);
		s_state = s_spawn_mode == CAM_PLAYER ? ST_GAME : ST_VIEWER;
	} else {
		enter_title();
	}
	perf_set_log(s_perflog, s_memlog || s_memtest);
	mem_log("boot");
	perf_event("boot", "%s", profile_status());
	load_title();
	if (s_autotest)
		autotest_begin();
	if (s_debug_results) {
		if (s_debug_results == 2)
			prog_new_game_wish(s_riddle, s_action);
		for (int i = 0; i < s_debug_nitemget; i++)
			prog_debug_item_get(s_debug_itemget[i]);
		s_results = s_results_dirty = true;
	}
	if (s_debug_movie[0])
		movie_play(s_debug_movie);
	if (s_debug_newgame && !s_spawn_map[0])
		new_game();          // debug.txt "newgame 1": the full New Game path (movie, scene, prologue) without input

	u32 frame = 0, calls = 0;
	u64 last_tick = svcGetSystemTick();
	while (aptMainLoop()) {
		// Real frame time, so a slow frame on hardware does not slow the game down. Clamped so a
		// long room load does not teleport James; 1/60 when the frame made vsync.
		u64 now_tick = svcGetSystemTick();
		float dt = (float)(now_tick - last_tick) / (float)SYSCLOCK_ARM11;
		last_tick = now_tick;
		dt = dt < 1.0f / 60.0f ? 1.0f / 60.0f : dt > 1.0f / 15.0f ? 1.0f / 15.0f : dt;
		// Frames are shown on the screen's refresh (59.83 Hz), so a frame lasts a whole number of
		// refreshes; snapping dt to that removes the tick jitter that made motion uneven when the
		// frame rate sits between 60 and 30 or below (movement, the camera and the clips all use dt)
		{
			const float refresh = 4481136.0f / 268111856.0f;
			float n = roundf(dt / refresh);
			dt = (n < 1.0f ? 1.0f : n) * refresh;
		}
		perf_frame_begin();
		hidScanInput();
		s_ui_used = 0;
		memtest_step(frame);
		u64 t_part = svcGetSystemTick();
		audio_update();
		perf_part("audio", t_part);
		t_part = svcGetSystemTick();
		sfx_update();
		perf_part("sfx", t_part);
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (s_autotest)
			autotest_frame(&down, &held);
		touchui_input(&down, &held);
		pt_frame(&down, &held, dt);
		if (s_pt_quit)
			break;
		u32 gdown, gheld;                    // the game keys (controls.c): actions, not buttons
		controls_game(down, held, &gdown, &gheld);
		s_gdown = gdown;
		s_anim_dt = dt;
		touchui_debug(s_state == ST_VIEWER || s_debug_hud);
		if (s_state != ST_TITLE) {
			touchui_menu_close(TOUI_TITLE);
			touchui_menu_close(TOUI_LEVELS);
		}
		if (s_results && results_update(down)) {
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x000000FF, 0);
			C3D_FrameDrawOn(top);
			results_draw_top();
			font_flush();
			touchui_frame();
			C3D_FrameEnd(0);
			continue;
		}
		if (s_state == ST_TITLE) {
			s_title_t += dt;
			if (!title_update(down, dt))
				break;
			float bright = s_title_t < 1.5f ? s_title_t / 1.5f : 1.0f;
			if (s_title_go >= 0) {
				s_title_go_t += dt;
				if (s_title_go_t >= TITLE_GO_SECONDS) {
					u64 t0 = svcGetSystemTick();
					title_go();                // the title frames are not timed, so log this one
					perf_event("title", "game start frame %.1f ms", perf_ms(svcGetSystemTick() - t0));
				}
				else if (bright > 1.0f - s_title_go_t / TITLE_GO_SECONDS)
					bright = 1.0f - s_title_go_t / TITLE_GO_SECONDS;
			}
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x000000FF, 0);
			C3D_FrameDrawOn(top);
			if (s_state == ST_TITLE && !optmenu_active() && !savescreen_active() &&
			    !titlemenu_draw_top(ui_draw, bright))      // start00 and the PC's menus
				draw_title(&ortho, bright);
			savescreen_draw_top();       // Continue's file list over the title
			optmenu_draw_top(ui_draw);   // the options from the title, on black
			font_flush();
			touchui_frame();
			C3D_FrameEnd(0);
			continue;            // the button that left the title is not seen by the game
		}
		if (gameover_active()) {
			if (!gameover_update(dt)) {
				enter_title();
				continue;
			}
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x000000FF, 0);
			C3D_FrameDrawOn(top);
			gameover_draw_top(ui_draw);
			touchui_frame();
			C3D_FrameEnd(0);
			continue;
		}
		bool viewer = s_state == ST_VIEWER;
		if (viewer && (down & KEY_START)) {
			enter_title();
			continue;
		}
		if (!viewer && !demo_active() && pause_update(down)) {
			if (s_state == ST_TITLE)
				continue;
			down = held = gdown = gheld = s_gdown = 0;   // the menu took the buttons; the game is frozen below
			dt = s_anim_dt = 0.0f;
		}
		if (gdown & KEY_B)
			s_run_pressed = true;
		if (viewer && (down & (KEY_DRIGHT | KEY_DLEFT)) && s_nrooms) {
			s_room = (s_room + (down & KEY_DRIGHT ? 1 : s_nrooms - 1)) % s_nrooms;
			load_current();
			stage_for_room();
		}
		if (viewer && (down & (KEY_DUP | KEY_DDOWN)) && s_nareas) {
			s_area = (s_area + (down & KEY_DUP ? 1 : s_nareas - 1)) % s_nareas;
			s_room = 0;
			list_rooms();
			load_current();
			stage_for_room();
		}
		if (viewer && (down & KEY_SELECT)) {
			s_cam.mode = (s_cam.mode + 1) % CAM_MODES;
			s_gamecam_reset = true;
		}
		if (s_cam.mode == CAM_ORBIT && (down & KEY_X))
			s_opaque_only = !s_opaque_only;
		if (s_cam.mode == CAM_ORBIT && (down & KEY_Y))
			s_depth_variant = (s_depth_variant + 1) % 3;
		// Enhanced Edition Room312FlashlightFix (Patches/Room312FlashlightFix.cpp, zlib licence): the
		// flashlight switch (0x48f078) does nothing in hotel room 312 while James has the videotape
		if (s_cam.mode == CAM_PLAYER && (gdown & KEY_X) && !screens_open() && !s_inv_open &&
		    !(bgm_room_id(ev_glb_crd(), s_px, s_pz) == 0xa2 && ev_have_item(0x17))) {
			s_flash_on = !s_flash_on;
			sfx_play(s_flash_on ? SFX_LIGHT_ON : SFX_LIGHT_OFF, 0.8f, 0.0f);
		}
		if (!viewer && (gdown & (KEY_DUP | KEY_DDOWN)) && !s_inv_open && !screens_open()) {
			s_bright += gdown & KEY_DUP ? 1 : -1;
			s_bright = s_bright < 0 ? 0 : s_bright > 7 ? 7 : s_bright;
			s_bright_shown = 90;
			s_opt.bright = (u8)s_bright;
			options_apply();
			options_save();
		}
		if (!viewer && (down & KEY_SELECT) && touchui_select()) {     // SELECT twice: the debug page
			s_debug_hud = !s_debug_hud;
			for (int r = 1; r < 18; r++)
				printf("\x1b[%d;1H%40s", r, "");
		}
		if (viewer && (down & KEY_ZL))
			s_render_mode = (s_render_mode + 1) % 4;

		circlePosition cp, cs;
		hidCircleRead(&cp);
		hidCstickRead(&cs);
		playtest_pad(&cp);
		if (s_autotest)
			cp.dx = cp.dy = cs.dx = cs.dy = 0;
		fade_update(dt);
		t_part = svcGetSystemTick();
		if (s_paused) {
			cp.dx = cp.dy = 0;
		} else if (s_cam.mode == CAM_PLAYER && demo_active()) {
			if (demo_update(down, dt))
				demo_finish();
		} else if (s_cam.mode == CAM_PLAYER && screens_update(down, held, &cp, dt)) {
			// the map screen or the memo list has the buttons; the game waits
		} else if (s_cam.mode == CAM_PLAYER) {
			if (!save_prompt_update(down) && !message_update(down) && !s_warp_pending) {
				prog_stage_tick(ev_stage(), dt);
				s_free_voice = demo_voice_free_tick(dt);   // a voice the stage code started (the hotel corridor)
				prog_score_frame(dt, combat_hp() / JAMES_HP_MAX, maria_distance());
				{
					// 0x5396e0: walking and running distance (running is the faster stride)
					static float lx, lz;
					float d = hypotf(s_px - lx, s_pz - lz);
					lx = s_px, lz = s_pz;
					if (dt > 0.0f)
						prog_stat_move(d, d / dt > 1260.0f);
				}
				if (prog_running()) {
					// a stage program runs instead of the event check; James stands still while
					// it has him in event mode
					static const circlePosition still;
					bool locked = prog_player_locked();
					player_update(locked ? &still : &cp, locked ? 0 : move_keys(gheld, gdown, &cp), dt);
					prog_update(down, held, dt);
					if (s_game_end >= 0 && !prog_running()) {
						bool results = s_game_end > 0;      // backing out of Born From a Wish has none
						s_game_end = -1;
						enter_title();
						s_results = results;
						s_results_dirty = true;
						continue;
					}
				} else if (!inventory_update(s_inv_open ? down : gdown)) {   // Y opens, its own keys inside
					CombatBody body = { &s_px, &s_py, &s_pz, &s_yaw };
					u32 free_held;
					combat_set_sight(s_env.dark, s_flash_on);
					if (s_debug_weapon && combat_equip_item(s_debug_weapon))
						s_debug_weapon = 0;
					if (!combat_update(&body, &cp, gdown, move_keys(gheld, gdown, &cp), dt, s_have_col ? &s_col : NULL,
					                   &free_held)) {
						player_update(&cp, free_held, dt);
						events_update(gdown);
					}
				}
				if (!prog_running() || !prog_player_locked()) {
					CombatBody body = { &s_px, &s_py, &s_pz, &s_yaw };
					EnPlayer ep = combat_enplayer(&body, s_flash_on);
					enemy_update(&ep, dt);
					head_update(&cp, gheld, dt, !prog_running() || !prog_player_locked());
				}
				blood_frame(dt);
				if (combat_dead()) {     // game over: the PC's screen, then the title (0x401fc0)
					gameover_start();
					continue;
				}
			}
		} else if (s_cam.mode == CAM_FLY) {
			float look_x = cs.dx / 156.0f + ((held & KEY_A) ? 1 : 0) - ((held & KEY_Y) ? 1 : 0);
			float look_y = cs.dy / 156.0f + ((held & KEY_X) ? 1 : 0) - ((held & KEY_B) ? 1 : 0);
			s_cam.yaw -= look_x * 0.04f;
			s_cam.pitch -= look_y * 0.03f;
			if (s_cam.pitch > 1.4f) s_cam.pitch = 1.4f;
			if (s_cam.pitch < -1.4f) s_cam.pitch = -1.4f;
			float fwd = cp.dy / 156.0f * 40.0f, side = cp.dx / 156.0f * 40.0f;
			s_cam.eye[0] += sinf(s_cam.yaw) * fwd - cosf(s_cam.yaw) * side;
			s_cam.eye[2] += cosf(s_cam.yaw) * fwd + sinf(s_cam.yaw) * side;
			if (held & KEY_L) s_cam.eye[1] += 30.0f;
			if (held & KEY_R) s_cam.eye[1] -= 30.0f;
		} else {
			s_cam.orbit += s_orbit_speed;
		}

		perf_part("game", t_part);
		t_part = svcGetSystemTick();
		if (s_arrival && (s_tiles_missing == 0 || s_arrival_t >= ARRIVAL_MAX))
			s_arrival = false;
		if (s_state == ST_GAME && !s_paused && ((frame % 8) == 0 || s_arrival)) {
			primary_update();    // walked onto a neighbouring tile
			if (stream_update_ex(s_stream_sync)) {   // load / drop touching tiles around James
				u64 t0 = svcGetSystemTick();
				enemy_stream_update(ev_stage(), s_have_col ? &s_col : NULL);
				perf_part("enemy", t0);
			}
		}
		perf_part("stream", t_part);

		C3D_Mtx mv;
		if (s_cam.mode == CAM_PLAYER && demo_scene_shown()) {
			float fovy, eye[3], feet[3] = { s_px, s_py, s_pz };
			demo_camera(&mv, &fovy, eye);
			s_chr_nofog = ee_final_boss_nofog(eye[1]);
			memcpy(s_fogf.eye, eye, sizeof s_fogf.eye);
			memcpy(s_fogf.watch, feet, sizeof s_fogf.watch);
			s_fogf.scr_z = 224.0f / tanf(fovy * 0.5f);
			sfx_listener(feet, eye);
			u64 tb = svcGetSystemTick();
			bgm_update(ev_stage(), ev_glb_crd(), feet, dt);
			perf_part("bgm", tb);
			float near, far;
			env_clip(&near, &far);
			Mtx_PerspTilt(&proj, fovy, C3D_AspectRatioTop, near, far, false);
		} else if (s_cam.mode == CAM_PLAYER) {
			GameCamPlayer gp = { { s_px, s_py, s_pz }, s_yaw, { s_vel[0], s_vel[1] }, s_turn_speed,
			                     ev_stage(), ev_glb_crd(), s_controls };
			if (s_gamecam_reset) {
				gamecam_reset(&gp);
				s_gamecam_reset = false;
			}
			float fovy;
			u64 tb = svcGetSystemTick();
			gamecam_update(&gp, dt, search_view(gheld, gdown), &mv, &fovy);
			perf_part("cam", tb);
			s_chr_nofog = ee_final_boss_nofog(gamecam_eye()[1]);
			memcpy(s_fogf.eye, gamecam_eye(), sizeof s_fogf.eye);
			memcpy(s_fogf.watch, gamecam_watch(), sizeof s_fogf.watch);
			s_fogf.scr_z = gamecam_scr_z();
			sfx_listener(gp.pos, gamecam_eye());
			tb = svcGetSystemTick();
			bgm_update(gp.stage, gp.glb_crd, gp.pos, dt);
			perf_part("bgm", tb);
			float near, far;
			env_clip(&near, &far);
			Mtx_PerspTilt(&proj, fovy, C3D_AspectRatioTop, near, far, false);
		} else {
			view_matrix(&mv);
			s_chr_nofog = false;
			Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 20.0f, 100000.0f, false);
		}

		perf_mark_render();
		u64 tw = svcGetSystemTick();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		perf_part("r_wait", tw);          // the previous frame's GPU work and the vertical blank
		neighbours_reap();
		u32 clear = s_cam.mode == CAM_PLAYER ? ((u32)s_env.clear_rgb[0] << 24 | (u32)s_env.clear_rgb[1] << 16 |
		                                       (u32)s_env.clear_rgb[2] << 8 | 0xFF) : CLEAR_COLOR;
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, s_render_mode == 3 ? 0x808080FF : clear,
		                      (s_depth_variant == 1 ? 0xFFFFFF : 0) | SHADOW_STENCIL_CLEAR);
		C3D_FrameDrawOn(top);
		if (s_render_mode == 3) {
			draw_texture_test(&ortho);
			calls = 1;
		} else if (playtest_fast() && !s_pt_shot) {
			calls = 0;               // playtest "fast 1": nothing drawn, the emulator's GPU idles
		} else {
			use_world_program();
			C3D_LightEnvBind(&s_lenv);
			lighting_frame(&mv, s_cam.mode == CAM_PLAYER);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, &proj);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &mv);
			Mtx_Multiply(&s_clip_mtx, &proj, &mv);
			shadow_frame(s_paused);
			u64 tr = svcGetSystemTick();
			calls = draw_room();
			perf_part("r_map", tr);
			tr = svcGetSystemTick();
			if (demo_active())
				draw_demo(&proj, &mv);
			else
				draw_james(&proj, &mv);     // same light environment as the room
			C3D_LightEnvBind(NULL);
			perf_part("r_chr", tr);
			tr = svcGetSystemTick();
			shadow_draw(&proj, &mv, &ortho);
			blood_draw(&proj, &mv);      // the floor's pools and stains, the spray (blood.c)
			perf_part("r_shadow", tr);
			tr = svcGetSystemTick();
			if (s_cam.mode == CAM_PLAYER && s_state == ST_GAME && s_opt.fog)
				fog_frame(&proj, &mv, &ortho, s_paused ? 0.0f : dt);
			perf_part("r_fog", tr);
			tr = svcGetSystemTick();
			float black = s_fade > demo_fade() ? s_fade : demo_fade();
			// Enhanced Edition "DisableRedCrossInCutScenes" (Patches/DisableRedCrossDuringCutscenes.cpp,
			// zlib licence): no cross in scenes, still pictures, the item screen or while a stage
			// program holds James for a scripted shot; the pause menu hides it too
			float cross = combat_low_health_alpha(s_paused ? 0.0f : dt);
			if (cross > 0.0f && s_state == ST_GAME && s_cam.mode == CAM_PLAYER && !demo_active() && !s_pic_shown &&
			    !s_inv_open && !s_paused && !(prog_running() && prog_player_locked()))
				draw_low_health(&ortho, cross * (1.0f - black));
			float dim = s_paused && !optmenu_brightness() && black < 0.5f ? 0.5f : black;
			if (dim > 0.0f)
				draw_fade(&ortho, dim);
			if (noise_on() && !s_pic_shown) {
				ui_program_2d(&ortho);
				noise_draw();
			}
			if (s_pic_shown)
				draw_picture(&ortho);
			draw_sprites();
			map_draw();
			optmenu_draw_top(ui_draw);
			if (s_paused && !optmenu_active() && !savescreen_active())
				pausemenu_draw_top();
			draw_top_text();
			savescreen_draw_top();       // a save point's files over the room
			font_flush();
			perf_part("r_ui", tr);
		}
		u64 tt = svcGetSystemTick();
		touchui_frame();
		perf_part("r_touch", tt);
		tt = svcGetSystemTick();
		C3D_FrameEnd(0);
		perf_part("r_end", tt);
		perf_frame_end(s_nrooms ? s_rooms[s_room] : "-");
		perf_pace(quality()->frame_cap);   // 60 while it fits, an even 30 when it does not
		for (int i = 0; i < s_ndump; i++)
			if (s_dump_frames[i] == frame)
				dump_frame(top, frame);
		if (s_pt_shot) {
			s_pt_shot = false;
			dump_frame(top, frame);
			playtest_log("shot frame_%lu.bmp", (unsigned long)frame);
		}

		bool hud = s_state == ST_VIEWER || s_debug_hud;
		if ((frame++ % 30) == 0 && !hud && s_hud_shown) {
			s_hud_shown = false;
			for (int r = 1; r < 18; r++)
				printf("\x1b[%d;1H%40s", r, "");
		}
		if ((frame % 30) == 1 && !hud) {
			// the help: the live bindings of the layout in use (controls.c), then James' state
			char help[12][40];
			int nh = controls_help(help, 12);
			printf("\x1b[2;3HSILENT HILL 2  (%s)            ", controls_layout() == CONTROLS_LAYOUT_NEW ? "New 3DS" : "Old 3DS");
			printf("\x1b[3;3H%-37.37s", "Circle Pad  walk / turn");
			for (int i = 0; i < nh && i < 11; i++)
				printf("\x1b[%d;3H%-37.37s", 4 + i, help[i]);
			printf("\x1b[15;3HSTART pause, SELECT twice debug      ");
			float hp = combat_hp();
			printf("\x1b[16;3HHealth %3.0f %-8s flashlight %-3s  ", hp, hp > 70.0f ? "Fine" : hp > 35.0f ? "Caution" : "Danger",
			       s_flash_on ? "on" : "off");
			printf("\x1b[17;3HWeapon %-30.30s", combat_weapon_line());
		}
		if ((frame % 30) == 1 && hud) {
			s_hud_shown = true;
			printf("\x1b[1;1Hsh23ds %-12s            \n\n", s_state == ST_VIEWER ? "room viewer" : "debug");
			printf("room   %-20s +%d tiles  \n", s_status, s_nnb);
			printf("area   %d/%d  room %d/%d          \n", s_area + 1, s_nareas, s_room + 1, s_nrooms);
			printf("verts  %-6lu tris %-6lu      \n", (unsigned long)s_cur.n_vertices, (unsigned long)(s_cur.n_indices / 3));
			printf("draws  %-4lu textures %-4lu miss %lu  \n", (unsigned long)calls, (unsigned long)texcache_count(),
			       (unsigned long)s_cur.missing_textures);
			printf("col    %s floors %lu walls %lu      \n", s_have_col ? "yes" : "no ",
			       (unsigned long)s_col.n_floors, (unsigned long)s_col.n_walls);
			printf("%-38s\n", mem_log(NULL));
			if (s_have_james)
				printf("%-26.26s%5.2f %5.2f \n", move_status(), s_yaw, s_look_yaw);   // facing, head
			else
				printf("james  not loaded                     \n");
			printf("audio  %-30s\n", audio_status());
			printf("%-16.16s%-22.22s\n", combat_status(), enemy_status());
			printf("%-38s\n", bgm_status());
			printf("stage  %2d %-16.16s ev%3d #%-3d\n", ev_stage(), ev_stage_name(), ev_count(), s_last_event);
			printf("pos    %7.0f %6.0f %7.0f      \n", s_px, s_py, s_pz);
			printf("prog   %-32s\n", prog_debug_status());
			printf("%-38s\n", demo_status());
			printf("cam %2d t%-2d %6.0f %6.0f %6.0f f%2.0f\n", gamecam_road_count(), gamecam_mv_type(),
			       gamecam_eye()[0], gamecam_eye()[1], gamecam_eye()[2], gamecam_fov() * 57.3f);
			printf("%-38s\n", fog_status());
			printf("%-38s\n", perf_status());
			printf("%-38s\n", shadow_status());
			printf("%-38s\n", blood_status());
			printf("%-38.38s\n", sfx_status());
		}
		pause_draw();
		inventory_draw();
		static int s_sub_serial = -1;
		if (demo_active() || (s_free_voice && !s_msg_page && !s_save_prompt)) {
			int serial;
			const char* sub = demo_subtitle(&serial);
			if (!s_opt.subtitles)
				sub = NULL;
			if (serial != s_sub_serial) {
				s_sub_serial = serial;
				for (int r = 18; r < 30; r++)
					printf("\x1b[%d;1H%40s", r, "");
				printf("\x1b[18;1H----------------------------------------");
				int row = 20;
				printf("\x1b[%d;3H", row);
				for (const char* c = s_msg_top && font_ready() ? NULL : sub; c && *c && *c != '\f' && row < 28; c++) {
					if (*c == '\n')
						printf("\x1b[%d;3H", ++row);
					else
						putchar(*c);
				}
				static const TouiButton skip[1] = { { "Skip", KEY_START } };
				bool scene = demo_active();      // (a voice over the game cannot be skipped)
				if (scene)
					printf("\x1b[29;27HSTART: skip");
				touchui_box(s_msg_top && font_ready() ? "" : (sub ? sub : ""), skip, scene ? 1 : 0);
			}
		} else if (s_sub_serial != -1) {
			s_sub_serial = -1;               // the scene is over: the box below gets the screen back
			s_msg_dirty = true;
			touchui_box(NULL, NULL, 0);
		} else if (s_msg_dirty && !s_inv_open && !s_paused) {
			// message box on the lower half of the bottom screen (rows 18..29)
			s_msg_dirty = false;
			for (int r = 18; r < 30; r++)
				printf("\x1b[%d;1H%40s", r, "");
			static const TouiButton save[2] = { { "Save", KEY_A }, { "Cancel", KEY_B } };
			touchui_box(s_save_prompt ? "Do you want to save the game?" :
			            s_msg_page ? (s_msg_top && font_ready() ? "" : s_msg_page) : NULL,
			            save, s_save_prompt ? 2 : 0);
			touchui_panel(!s_save_prompt && !s_msg_page ? s_panel : "");
			if (s_save_prompt) {
				printf("\x1b[18;1H----------------------------------------");
				printf("\x1b[20;3HDo you want to save the game?");
				printf("\x1b[29;20HA: save   B: cancel");
			} else if (s_msg_page) {
				printf("\x1b[18;1H----------------------------------------");
				int row = 20;
				printf("\x1b[%d;3H", row);
				for (const char* c = s_msg_top && font_ready() ? "" : s_msg_page; *c && *c != '\f' && row < 28; c++) {
					if (*c == '\n')
						printf("\x1b[%d;3H", ++row);
					else
						putchar(*c);
				}
				printf("\x1b[29;30H%s", strchr(s_msg_page, '\f') ? "A: next" : "A: close");
			} else if (s_panel[0]) {
				printf("\x1b[18;1H----------------------------------------");
				int row = 19;
				printf("\x1b[%d;1H", row);
				for (const char* c = s_panel; *c && row < 30; c++) {
					if (*c == '\n')
						printf("\x1b[%d;1H", ++row);
					else
						putchar(*c);
				}
			}
		}
	}

	stream_exit();
	neighbours_clear();
	if (s_loaded)
		room_free(&s_cur);
	if (s_have_col)
		col_free(&s_col);
	col_free(&s_col_room);
	texcache_clear();
	movie_exit();
	enemy_exit();
	combat_exit();
	if (s_have_james) {
		chr_free(&s_james);
		anim_free(&s_james_anim);
	}
	bgm_exit();
	sfx_exit();
	audio_exit();
	fog_exit();
	shaderProgramFree(&s_skin_prog);
	DVLB_Free(s_skin_dvlb);
	shaderProgramFree(&s_prog);
	DVLB_Free(s_dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
