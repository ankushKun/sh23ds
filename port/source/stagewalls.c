// The walls the stages' per-frame functions add to the collision each frame through
// clAddDynamicWall (0x5228e0, cl_main.c: a pointer to a list of _CL_HITPOLY_PLANE, 80 bytes each,
// ended by kind 0, put on the list 0x1f98680 that clCheckHitDynamicWallCollision 0x525030 and the
// line checks read on the next frame). The rooms' .cld files leave these places open: the town's
// police barricades and street ends, doors and gates that open on a flag, the boat's lake. The lists
// come from the exe by their address (tools/sh2conv/stagewalls.py writes events/stagewalls.bin, each
// plane as a wall the way collision.py makes a .cld wall); here is when each goes up, by the PC's
// room (0x495220) and flags. Like the rooms' walls they stop James only from their front
// (clCheckHitDynamicWallCollision 0x525030 tests them with clCheckColumn2WallHit 0x522b50; collide.h).
// The dynamic floors (clAddDynamicFloor 0x522910) of stages 7 and 34 are left out.
#include "stagewalls.h"
#include "collide.h"
#include "perf.h"
#include "prog_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define MAX_LISTS 96

// stagewalls.bin: "SWL1", u32 version, u32 n_lists; per list u32 address, u32 n_walls and
// n_walls x f32 x0, z0, x1, z1, y_top, y_bottom. Version 2 orders each wall's ends so that its front
// is where col_wall_side is >= 0; version 1 (packs made before) has no facing: two-sided walls.
static struct { u32 va, n; const float* w; } s_list[MAX_LISTS];
static int s_nlist, s_stage = -1;
static bool s_one_sided;
static u8* s_file;
static bool s_missing;

// Read again with each stage, as the other events files are.
static void load(void)
{
	free(s_file);
	s_file = NULL;
	s_nlist = 0;
	FILE* f = fopen(PACK_DIR "/events/stagewalls.bin", "rb");
	long size = -1;
	if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 12 && fseek(f, 0, SEEK_SET) == 0 &&
	    (s_file = malloc(size)) && fread(s_file, 1, size, f) == (size_t)size) {
		u32 head[3], o = 12;
		memcpy(head, s_file, sizeof head);
		bool ok = !memcmp(head, "SWL1", 4) && (head[1] == 1 || head[1] == 2) && head[2] <= MAX_LISTS;
		s_one_sided = head[1] >= 2;
		for (u32 i = 0; ok && i < head[2]; i++) {
			u32 ln[2];
			ok = o + 8 <= (u32)size;
			if (!ok)
				break;
			memcpy(ln, s_file + o, sizeof ln);
			o += 8;
			ok = ln[1] <= 64 && o + ln[1] * 24 <= (u32)size;
			if (ok) {
				s_list[s_nlist].va = ln[0], s_list[s_nlist].n = ln[1], s_list[s_nlist].w = (const float*)(s_file + o);
				s_nlist++;
				o += ln[1] * 24;
			}
		}
		if (!ok)
			s_nlist = 0;
	}
	if (f)
		fclose(f);
	bool missing = s_nlist == 0;
	if (missing && !s_missing)
		perf_event("stagewalls", "events/stagewalls.bin missing or unreadable: no stage walls (pack made before them)");
	s_missing = missing;
}

bool stagewalls_missing(void) { return s_missing; }

void stagewalls_add(u32 list)
{
	for (int i = 0; i < s_nlist; i++)
		if (s_list[i].va == list) {
			const float* w = s_list[i].w;
			for (u32 k = 0; k < s_list[i].n; k++, w += 6)
				col_extra_wall(w[0], w[1], w[2], w[3], w[4], w[5], s_one_sided);
			return;
		}
}

#define ADD(list) stagewalls_add(list)

static bool f(int n) { return p_flag(n); }

// The town's per-frame functions run each tile's code for the 3 x 3 tiles of 20000 units round
// James (tile x*32+z less a base, x and z ftol(pos / 20000 + 16)).
static int tile_base(const float* me, int base)
{
	int tx = (int)(me[0] * 5e-05f + 16.0f), tz = (int)(me[2] * 5e-05f + 16.0f);
	return tx * 32 + tz - base;
}

// 0x59ef10 (stage 5): its tile switch, the walls only
static void town_east_tile(int id)
{
	switch (id) {
	case 0x7e: if (f(251)) ADD(0x8e7db8); break;
	case 0x7f: if (f(251)) ADD(0x8e7a98); break;
	case 0x80: if (f(251)) ADD(0x8e7778); break;
	case 0x81: if (f(43)) ADD(0x8e7638); break;
	case 0x9f: if (!f(43)) ADD(0x8e7408); break;     // the barricade across the street at x -7600
	case 0xa0: if (!f(43)) ADD(0x8e7188); break;
	case 0xde: if (f(251)) ADD(0x8e82b8); break;     // 0x59e440
	case 0xe1:
		if (!f(251))
			ADD(0x8e7f98);
		else
			ADD(0x8e7e58);
		break;
	default: break;
	}
}

// 0x594e10 (stage 14): its tile switch, the walls only
static void town_west_tile(int id)
{
	switch (id) {
	case 0x3f: if (f(11)) ADD(0x8de7a8); break;
	case 0x5b: ADD(0x8de938); ADD(0x8deb68); break;
	case 0x5c: ADD(0x8dede8); ADD(0x8defc8); ADD(0x8df158); break;
	case 0x7b: ADD(0x8ded48); ADD(0x8deca8); break;
	case 0x7c: ADD(0x8df388); ADD(0x8df478); ADD(0x8df298); break;
	default: break;
	}
}

static bool s_all_tiles;
void stagewalls_all_tiles(bool on) { s_all_tiles = on; }

static void town_tiles(const float* me, int base, void (*tile)(int))
{
	if (s_all_tiles) {               // the playtest's planner: every tile's, wherever James is
		for (int id = -base; id < 32 * 32 - base; id++)
			tile(id);
		return;
	}
	int id = tile_base(me, base);
	for (int i = 0; i < 3; i++, id += 32)
		for (int j = 0; j < 3; j++)
			tile(id + j);
}

void stagewalls_tick(int stage, int room, const float* me)
{
	if (stage != s_stage) {
		s_stage = stage;
		load();
	}
	switch (stage) {
	case 4:   // 0x59fec0
		if (f(12)) {
			ADD(0x8e9e38);
			ADD(0x8e9f28);
		}
		break;
	case 5: town_tiles(me, 0x171, town_east_tile); break;
	case 6:   // 0x59d1e0
		if (room == 0x1e && !f(88))
			ADD(0x8e63a0);
		break;
	case 7:   // 0x59bca0
		if (room == 0x18) {
			if (!f(69)) {
				ADD(0x8e5268);
				ADD(0x8e53f8);
			} else {
				ADD(0x8e5088);
			}
			if (!f(82))
				ADD(0x8e4ef8);
			else
				ADD(0x8e4d68);
		}
		break;
	case 12:  // 0x596660
		if (room == 0x21 && !f(148))
			ADD(0x8e1d48);
		break;
	case 14: town_tiles(me, 0x1b4, town_west_tile); break;
	case 21:  // 0x58d2d0
		if (room == 0x2b)
			ADD(f(203) ? 0x8da5c0 : 0x8da430);   // the same walls, materials 0xc and 9
		break;
	case 26:  // 0x588bf0
		if (room == 0x59) {
			if (f(223))
				ADD(0x8d7f30);
			else
				ADD(0x8d7df0);
		}
		break;
	case 30:  // 0x587780
		if (room == 0x62) {
			if (f(268))
				ADD(0x8d69c8);
			else
				ADD(0x8d68d8);
		} else if (room == 0x64 && f(271)) {
			ADD(0x8d6838);
		}
		break;
	case 31:  // 0x585bb0
		if (room == 0x66 && f(283))
			ADD(0x8d5f88);
		break;
	case 35:  // 0x581580
		if (room == 0x8a) {
			if (f(335)) {
				ADD(0x8d2bc0);
			} else {
				ADD(0x8d29e0);
				ADD(0x8d2b20);
			}
		}
		break;
	case 36:  // 0x580560
		if (room == 0x8b) {
			// flags 343..348, the innocent man's rope
			if (!f(346)) ADD(0x8d0af0);
			if (!f(347)) ADD(0x8d0c80);
			if (!f(348)) ADD(0x8d0e10);
			if (!f(343)) ADD(0x8d0fa0);
			if (!f(344)) ADD(0x8d1130);
			if (!f(345)) ADD(0x8d12c0);
			if (!f(346) && !f(347)) ADD(0x8d15e0);
			if (!f(347) && !f(348)) ADD(0x8d1900);
			if (!f(343) && !f(344)) ADD(0x8d1770);
			if (!f(344) && !f(345)) ADD(0x8d1450);
		} else if (room == 0x86 && !f(367)) {
			ADD(0x8d0a50);
		} else if (room == 0x85 && !f(368)) {
			ADD(0x8d1a90);
		}
		break;
	case 38:  // 0x53fe40, the boat's update, each frame it is there: the lake's edge at the boat's height
		ADD(0x8bdce0);
		break;
	case 42:  // 0x578950
		if (room == 0xa3 && !f(473))
			ADD(0x8cbd30);
		break;
	case 43:  // 0x577690
		if (room == 0xb8 && !f(478))
			ADD(0x8cb7a0);
		break;
	default: break;
	}
}
