#include "envlight.h"
#include "textcache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCOLS 65

static void set3(float* d, const float* s) { d[0] = s[0], d[1] = s[1], d[2] = s[2]; }

// A lit room for maps the table does not know (also used when pack/lighting.txt is missing).
static void defaults(RoomEnv* e)
{
	static const float amb[3] = { 0.65f, 0.65f, 0.65f }, bcol[3] = { 22.4f, 22.4f, 22.4f };
	static const float vamb[3] = { 4, 4, 4 }, bamb[3] = { 0.045f, 0.045f, 0.045f };
	memset(e, 0, sizeof *e);
	set3(e->amb, amb), set3(e->bamb, bamb), set3(e->bcol, bcol), set3(e->vamb, vamb);
	e->fog_start = 3000.0f, e->fog_end = 5500.0f, e->fog_near_f = 235.0f, e->fog_far_f = 20.0f;
	e->fog_rgb[0] = e->fog_rgb[1] = e->fog_rgb[2] = 70.0f;
	e->clip_near = 100.0f, e->clip_far = 7500.0f;
	e->ldir[0][1] = 1.0f;
	e->lcol[0][0] = e->lcol[0][1] = e->lcol[0][2] = 0.025f;
	e->fogp[0] = e->fogp[1] = e->fogp[2] = e->fogp[3] = -1;
}

static void parse(RoomEnv* e, const float* v)
{
	// columns after "name variant glb map_id": see the header of lighting.txt
	memset(e, 0, sizeof *e);
	e->dark = v[0] != 0.0f;
	set3(e->amb, v + 1), set3(e->bamb, v + 4), set3(e->bcol, v + 7), set3(e->vamb, v + 10);
	set3(e->fog_rgb, v + 13);
	e->fog_start = v[16], e->fog_end = v[17], e->fog_near_f = v[18], e->fog_far_f = v[19];
	set3(e->clear_rgb, v + 20);
	e->clip_near = v[23], e->clip_far = v[24];
	for (int i = 0; i < 3; i++) {
		set3(e->ldir[i], v + 25 + 6 * i);
		set3(e->lcol[i], v + 28 + 6 * i);
	}
	set3(e->fl_col, v + 43);
	e->fl_start = v[46], e->fl_end = v[47], e->fl_cone = v[48];
}

// Enhanced Edition "PS2FlashlightBrightness" (Patches/EnhancedFlashlight.cpp, zlib licence). The PC
// scales the flashlight colour by 4 before the intensity table (0x47a9ee in FUN_0047a8d0), which
// makes the beam wash the walls out; EE uses 1.2 for the flashlight, as bright as on the PS2. The
// reach (0x47be64) goes from 6000 to 8500 so the dimmer beam still lights the far end of a room.
static bool s_ps2_flashlight = true;
void env_set_ps2_flashlight(bool on) { s_ps2_flashlight = on; }

static void ee_flashlight(RoomEnv* e)
{
	if (!s_ps2_flashlight)
		return;
	for (int i = 0; i < 3; i++)
		e->fl_col[i] *= 1.2f / 4.0f;
	if (e->fl_end == 6000.0f)
		e->fl_end = 8500.0f;
}

bool env_load(const char* pack_dir, const char* map, const char* variant, RoomEnv* out)
{
	char path[160];
	snprintf(path, sizeof path, "%s/lighting.txt", pack_dir);
	const char* text = text_cached(path);   // read once: this runs on every tile change
	defaults(out);
	if (!text)
		return false;
	size_t maplen = strlen(map);
	static char line[1024];
	bool found = false, found_default = false;
	float v[NCOLS - 4];
	RoomEnv def;
	while (text_next_line(&text, line, sizeof line)) {
		if (line[0] == '#' || strncmp(line, map, maplen) || line[maplen] != ' ')
			continue;
		char name[16], var[16], mapid[16];
		int glb, n = 0;
		if (sscanf(line, "%15s %15s %d %15s%n", name, var, &glb, mapid, &n) != 4 || strcmp(name, map))
			continue;
		char* p = line + n;
		int k = 0;
		while (k < NCOLS - 4) {
			char* end;
			v[k] = strtof(p, &end);
			if (end == p)
				break;
			p = end;
			k++;
		}
		if (k != NCOLS - 4)
			continue;
		// optional trailing columns: the fog particle colour (lighting.py fogp_*)
		int fp[4] = { -1, -1, -1, -1 };
		for (int i = 0; i < 4; i++) {
			char* end;
			long x = strtol(p, &end, 10);
			if (end == p)
				break;
			fp[i] = (int)x;
			p = end;
		}
		if (!strcmp(var, variant)) {
			parse(out, v);
			memcpy(out->fogp, fp, sizeof fp);
			out->map_id = (int)strtol(mapid, NULL, 16);
			found = true;
			break;
		}
		if (!strcmp(var, "-")) {
			parse(&def, v);
			memcpy(def.fogp, fp, sizeof fp);
			def.map_id = (int)strtol(mapid, NULL, 16);
			found_default = true;
		}
	}
	if (!found && found_default)
		*out = def;
	if (found || found_default)
		ee_flashlight(out);
	return found || found_default;
}

// 0x47a540 / 0x5b2560: c19 = Ambient * (0.5 + 0.25 * level), c20 = BaseColor / 16,
// c21 = BaseAmbient * (1 + 2 * level) / 16, c22 = VertexAmbient / 4. The level is 0x8a2ba8: 0 with
// the flashlight on, env_flash_off_level() with it off, and left as it was in lit rooms.
void env_constants(const RoomEnv* e, float level, RoomEnvConst* out)
{
	for (int i = 0; i < 3; i++) {
		out->c19[i] = e->amb[i] * (0.5f + 0.25f * level);
		out->c20[i] = e->bcol[i] / 16.0f;
		out->c21[i] = e->bamb[i] * (1.0f + 2.0f * level) / 16.0f;
		out->c22[i] = e->vamb[i] / 4.0f;
	}
}

float env_flash_off_level(const RoomEnv* e, bool have_flashlight, bool flag625)
{
	switch (e->map_id) {
	case 0xa0001: case 0xa0045: case 0xa0085: case 0xa00ca: case 0xa00d6: case 0xd0019:
		return 0.75f;                       // 0x8a2bb8
	case 0x60005: case 0xa003d: case 0xa007d: case 0xa0091: case 0xa00e6: case 0xc005b:
	case 0xd0015: case 0xd0049:
		return 0.0f;
	case 0xc0015: case 0xd0025:
		return 0.3f;                        // 0x8a2bb4
	case 0xd0029: case 0xd002d: case 0xd0031: case 0xd0039:
		return 1.0f;                        // 0x8a2bac
	}
	if (flag625)
		return 0.3f;                        // 0x8a2bb0
	return have_flashlight ? 0.3f : 1.0f;   // gFlashLightAcquired & 0x40000 (item 0x12)
}

// Enhanced Edition "RoomLightingFix" (Patches/LightingPatch.cpp, RunRoomLighting, zlib licence):
// the town, the apartment courtyard and the lake shore always use level 0, and the motorhome
// (lit, clear colour 70 grey) 1.0; in Born From a Wish town west and the mansion entrance use
// 1.0 and the Blue Creek entrance 0. EE writes 0x8a2ba8 one frame after the room changes.
bool env_ee_room_level(int room, bool bfaw, const RoomEnv* e, float* level)
{
	if (!bfaw) {
		if (room == 0x05 && e->clear_rgb[0] == 70.0f && e->clear_rgb[1] == 70.0f && e->clear_rgb[2] == 70.0f) {
			*level = 1.0f;              // R_MOTORHOME
			return true;
		}
		if (room == 0x04 || room == 0x07 || room == 0x08 || room == 0x0e) {
			*level = 0.0f;              // R_TOWN_EAST, R_APT_E_COURTYARD, R_TOWN_WEST, R_TOWN_LAKE
			return true;
		}
		return false;
	}
	if (room == 0x08 || room == 0xd3) {
		*level = 1.0f;                  // R_TOWN_WEST, R_MAN_OUTSIDE_ENTRANCE
		return true;
	}
	if (room == 0xd4) {
		*level = 0.0f;                  // R_MAN_BLUE_CREEK_ENTRANCE
		return true;
	}
	return false;
}
