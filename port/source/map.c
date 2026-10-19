#include "map.h"
#include "bgm.h"
#include "events.h"
#include "prog.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tex3ds.h>

// The map screen of the PC (Event/map.c). Map indices are the PC's DAT_00950528 values: 1..3 the
// town, 4..11 the apartments, 12..15 the hospital, 16..19 the prison, 20..35 the hotel (20..27 the
// guest maps, 28..35 the employee ones), 36 the Baldwin mansion. Positions on a map are in the PC's
// map units: -256..256 across the map, y down, two texels of the 1024 texture per unit.

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define N_MAPS 37

typedef struct { s32 map; float ox, oy; } RoomRec;
typedef struct { u32 code; s16 rect[4]; } MarkRec;
typedef struct { u16 first, count; MarkRec* recs; } MarkTable;
typedef struct { char tex[16], mark[16]; s8 nav[4]; u8 ntab; MarkTable tab[2]; } MapRec;
typedef struct { s16 flag, kind, title, text; char pic[2][24]; } MemoRec;

static MapDrawFn s_draw;
static RoomRec* s_rooms;
static int s_nrooms;
static MapRec s_maps[N_MAPS];
static MemoRec* s_memos;
static int s_nmemos;
static bool s_ready;

static bool s_open;
static MapPlayer s_pl;
static int s_room;               // PC room id of the player (FUN_00495220)
static int s_shown, s_james;     // DAT_00950528, FUN_0049a7e0()
static int s_zoom_state;         // DAT_00950520: 0 out, 1 zooming in, 2 in, 3 zooming out
static float s_zoom;             // DAT_00950530
static bool s_open_in = true;    // the Game Options' Map: Zoom In (the PC's default) or Zoom Out
static float s_cx, s_cy;         // DAT_009504cc / 009504f4: the zoomed view's centre, map units
static C3D_Tex s_tex, s_mark;
static char s_tex_name[16], s_mark_name[16];
static bool s_tex_ok, s_mark_ok;
static char s_legend[200];

// The marking animation programs start (0x49c0f0 / 0x49dbd0 / 0x49c180): the map opens by itself,
// moves to each new mark, zooms in and fades it in. Modes 0..4 come from the programs, 5 follows 0.
typedef struct { u32 code; u8 mark; } MarkAnim;
static MarkAnim* s_anim[6];
static int s_nanim[6];
static int s_mode = -1;          // DAT_009504e8 while marking, -1 otherwise
static int s_mk, s_mstate;       // DAT_0095051c entry, DAT_009504dc state
static float s_mt;               // frames counted as time (the PC counts 16 and 24 frames of 1/30 s)
static u8 s_alpha[8];            // per entry, 0..0x80
static float s_fx, s_fy, s_tx, s_ty, s_scroll;   // scrolling between two marks (DAT_009504e4 / d0 / ec)

static bool F(int n) { return ev_flag(n); }
static bool s_mini_used;         // the touch screen's small map keeps the textures

// ---------------------------------------------------------------- data (pack/map/map.dat)

bool map_init(MapDrawFn draw)
{
	s_draw = draw;
	FILE* f = fopen(PACK_DIR "/map/map.dat", "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* b = malloc(n);
	bool ok = b && fread(b, 1, n, f) == (size_t)n && n > 16 && !memcmp(b, "SHMP", 4);
	fclose(f);
	if (!ok) {
		free(b);
		return false;
	}
	// parsed once into plain arrays; the file's records are not aligned
	const u8* p = b + 8;
	memcpy(&s_nrooms, p, 4), p += 4;
	s_rooms = malloc(sizeof(RoomRec) * s_nrooms);
	memcpy(s_rooms, p, sizeof(RoomRec) * s_nrooms), p += 12 * s_nrooms;
	int nmaps;
	memcpy(&nmaps, p, 4), p += 4;
	for (int m = 0; m < nmaps && m < N_MAPS; m++) {
		MapRec* r = &s_maps[m];
		memcpy(r->tex, p, 16), memcpy(r->mark, p + 16, 16), memcpy(r->nav, p + 32, 4);
		p += 36;
		r->ntab = *p++;
		for (int t = 0; t < r->ntab && t < 2; t++) {
			memcpy(&r->tab[t].first, p, 2), memcpy(&r->tab[t].count, p + 2, 2);
			p += 4;
			r->tab[t].recs = malloc(sizeof(MarkRec) * r->tab[t].count);
			for (int i = 0; i < r->tab[t].count; i++, p += 12)
				memcpy(&r->tab[t].recs[i].code, p, 4), memcpy(r->tab[t].recs[i].rect, p + 4, 8);
		}
	}
	memcpy(&s_nmemos, p, 4), p += 4;
	s_memos = malloc(sizeof(MemoRec) * s_nmemos);
	for (int i = 0; i < s_nmemos; i++, p += 56)
		memcpy(&s_memos[i], p, 56);
	u32 version;
	memcpy(&version, b + 4, 4);
	if (version >= 2) {
		int nmodes;
		memcpy(&nmodes, p, 4), p += 4;
		for (int m = 0; m < nmodes && m < 6; m++) {
			memcpy(&s_nanim[m], p, 4), p += 4;
			s_anim[m] = malloc(sizeof(MarkAnim) * (s_nanim[m] ? s_nanim[m] : 1));
			for (int k = 0; k < s_nanim[m]; k++, p += 5)
				memcpy(&s_anim[m][k].code, p, 4), s_anim[m][k].mark = p[4];
		}
	}
	free(b);
	s_ready = true;
	return true;
}

// ---------------------------------------------------------------- PC map logic

// 0x49adb0: does the player own map m?
static bool available(int m)
{
	switch (m) {
	case 0: return false;
	case 1: case 2: case 3: return F(24);
	case 4: case 5: case 6: return F(25);
	case 7: case 8: return F(26);
	case 9: case 10: case 11: return F(26) && F(25);
	case 12: case 13: return F(27);
	case 14: case 15: return F(215) && F(27);
	case 16: case 17: return F(28);
	case 18: case 19: return F(29);
	case 20: case 21: case 22: case 23: return F(30) || F(31);
	case 24: case 25: case 26: case 27: return F(476) && (F(30) || F(31));
	case 28: case 29: case 30: case 31: return F(31);
	case 32: case 33: case 34: case 35: return F(476) && F(31);
	case 36: return F(670);
	default: return true;
	}
}

// 0x49a7e0: the map index of the player's room (several rooms span floors or two maps).
static int james_map(int room, float x, float y, float z)
{
	int m = room >= 0 && room < s_nrooms ? s_rooms[room].map : 0;
	switch (room) {
	case 0x08:      // town west: the east map's corner
		m = x >= 40298.0f && x < 79561.0f && z >= -60828.848f && z < -59016.0f ? 2 : 3;
		break;
	case 0x0e:      // the lake: the town map once James is across
		m = F(381) ? 1 : !F(379) ? 3 : 1;
		break;
	case 0x0f:      // apartment stairs: the floor from the height (y down)
		if (y > -750.0f || (x < -60000.0f && y > -1000.0f)) m = 4;
		else if (y <= -2350.0f && (x >= -60000.0f || y <= -2600.0f)) m = 6;
		else m = 5;
		break;
	case 0x10:
		if (y > -900.0f || (z > -19300.0f && y > -1150.0f)) m = 4;
		else if (y <= -2700.0f && (z <= -19300.0f || y <= -3450.0f)) m = 6;
		else m = 5;
		break;
	case 0x11:
		if (y > -900.0f || (z > -59300.0f && y > -1150.0f)) m = 4;
		else if (y <= -2700.0f && (z <= -59300.0f || y <= -3450.0f)) m = 6;
		else m = 5;
		break;
	case 0x20: m = z < -19200.0f && y > -1050.0f ? 7 : 8; break;
	case 0x21: m = y >= -855.0f && (z <= -99431.0f || y >= -854.0f) ? 7 : 8; break;
	case 0x29: m = y <= -950.0f && (z <= -100000.0f || y <= -1200.0f) ? 13 : 12; break;
	case 0x2a:
		if (F(17)) m = 12;
		if (F(18) || F(19)) m = 13;
		break;
	case 0x46:
		if (F(17)) m = 14;
		if (F(18) || F(19)) m = 15;
		break;
	case 0x47: m = y >= -1000.0f || (z > -219978.0f && y > -1001.0f) ? 14 : 15; break;
	case 0x48:
		if (y < 645.0f)
			m = y <= -1000.0f ? 15 : 12;
		break;
	case 0x91: case 0xab: {
		bool up = y < -920.0f || (z < -20020.0f && y < -874.0f);
		m = room == 0x91 ? (up ? 0x1d : 0x1c) : (up ? 0x21 : 0x20);
		break;
	}
	case 0x92: m = y <= -1400.0f ? 0x16 : 0x15; break;
	case 0xb8: m = F(479) ? 0x18 : 0x1a; break;
	}
	if (F(25) && F(26)) {               // both apartment maps: the combined ones
		if (m >= 4 && m <= 6) m += 5;
		else if (m >= 7 && m <= 8) m += 2;
	}
	if (F(31) && m >= 20 && m < 28)     // the employee map adds the staff rooms
		m += 8;
	return m == 0x11 ? 0x10 : m;
}

// 0x49b510: the player's position on the map, in map units. False where the PC draws no marker.
static bool james_pos(float* X, float* Y)
{
	int r = s_room;
	float x = s_pl.x, y = s_pl.y, z = s_pl.z;
	float ox = r >= 0 && r < s_nrooms ? s_rooms[r].ox : 0.0f, oy = r >= 0 && r < s_nrooms ? s_rooms[r].oy : 0.0f;
	float kx = 0.01477f, kz = 0.019658f;      // _DAT_006aef8c / 006aef88, most rooms
	switch (r) {
	case 1: case 2: case 5: case 6: case 9: case 10: case 11: case 12: case 13: case 0x5c: case 0xbe: case 0xd5:
		*X = ox, *Y = oy;
		return true;
	case 3: kx = 0.00053f, kz = 0.00079f; break;
	case 4: kx = 0.00142f, kz = 0.00196f; break;
	case 7: case 0x12: case 0x15: case 0x1f: case 0x25: case 0x26: kx = 0.00801f, kz = 0.01037f; break;
	case 8:
		if (x > 30453.578f && x < 35961.0f && z >= -60828.848f && z < -59016.0f) {
			*X = 212.4619f, *Y = 183.76071f;
			return true;
		}
		if (x >= 40298.0f && x < 79561.0f && z >= -60828.848f && z < -59016.0f) {
			*X = x * 0.00071f - 217.0f, *Y = 67.0f - z * 0.00196f;
			return true;
		}
		kx = 0.00146f, kz = 0.00196f;
		break;
	case 0x0e:
		if (F(381)) { *X = -87.21007f, *Y = -134.06479f; return true; }
		if (!F(379)) { *X = -78.40884f, *Y = -178.77686f; return true; }
		return false;                       // on the boat
	case 0x29: {
		const float K = 0.013396f, KZ = 0.019375f;
		if (y > -950.0f || (z > -100000.0f && y > -1200.0f)) *X = x * K + 879.8f, *Y = -1988.8f - z * KZ;
		else if (y <= -2950.0f && (z <= -100000.0f || y <= -3200.0f)) {
			if (y <= -4950.0f && (z <= -100000.0f || y <= -5200.0f)) *X = x * K + 631.7003f, *Y = -1956.7996f - z * KZ;
			else *X = x * K + 916.4001f, *Y = -2086.5996f - z * KZ;
		} else *X = x * K + 916.2002f, *Y = -1847.7001f - z * KZ;
		return true;
	}
	case 0x2a: case 0x46: {
		static const float off[2][3][2] = {
			{ { 1421.101f, 531.0004f }, { 1421.101f, 289.9001f }, { 1385.2999f, 426.99988f } },
			{ { 240.9f, 6029.8965f }, { 238.69986f, 5789.196f }, { 203.69986f, 5931.196f } },
		};
		int i = F(19) ? 0 : F(18) ? 1 : F(17) ? 2 : -1;
		if (i < 0)
			return false;
		*X = x * 0.01477f + off[r == 0x46][i][0], *Y = z * -0.019658f - off[r == 0x46][i][1];
		return true;
	}
	case 0x3b: kx = 0.014716f, kz = 0.019583f; break;
	case 0x47: {
		const float K = 0.013396f, KZ = 0.019375f;
		float bx = 915.0f, by = -313.9f;
		if (y >= 1001.0f || (y >= 1000.0f && z > -220067.0f)) bx = 878.0f, by = -31.9f;
		else if (y >= -1000.0f || (z > -219978.0f && y > -1001.0f)) bx = 878.80005f, by = -211.9f;
		else if (y >= -2998.0f || (z > -220003.0f && y > -3000.0f)) bx = 915.0f, by = -73.9f;
		else if (y <= -5000.0f && (z <= -220011.0f || y < -5000.0f)) bx = 630.0f, by = -183.9f;
		*X = x * K + bx, *Y = by - z * KZ - 4096.0f;
		return true;
	}
	case 0x48:
		if (y >= 1700.0f) { *X = 0.0f, *Y = -1000.0f; return true; }
		if (y <= -1000.0f) {
			*X = x * 0.013396f + 1303.0f;
			*Y = (y <= -3000.0f ? 366.19952f : 607.19952f) - z * 0.016455f - 4096.0f;
		} else {
			*X = x * 0.013396f + 1267.3004f, *Y = z * -0.019375f - 177.0f - 4096.0f;
		}
		return true;
	case 0x66: case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e:
	case 0x6f: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: {
		// prison maps drawn turned a quarter: the axes swap
		float a = x * 0.01377f + ox, b = oy - z * (r == 0x72 ? 0.009988f : 0.010408f);
		*X = b, *Y = -a;
		return true;
	}
	default:
		if (r >= 0x78 && r <= 0x8d) {
			float a = x * 0.01007f + ox, b = oy - z * 0.0089f;
			*X = -b, *Y = a;
			return true;
		}
		if ((r >= 0x0f && r <= 0x1e) || (r >= 0x20 && r <= 0x24) || r == 0x27 || r == 0x28)
			kx = 0.0087f, kz = 0.0096f;
		else if (r == 0x91 || (r >= 0x93 && r <= 0x98) || (r >= 0x9b && r <= 0x9e) || (r >= 0xa5 && r <= 0xa7) ||
		         r == 0xab || r == 0xb0 || r == 0xb6 || r == 0xb7)
			kx = 0.0164f, kz = 0.01875f;
		else if (r == 0x92 || r == 0xa2 || r == 0xac)
			kx = 0.0164f, kz = 0.0209f;
		else if (r == 0x99)
			kx = 0.01597f, kz = 0.0207f;
		else if (r == 0x9a || r == 0x9f || r == 0xa0 || r == 0xa1 || r == 0xa3 || r == 0xa8 || r == 0xa9 ||
		         (r >= 0xad && r <= 0xaf) || (r >= 0xb1 && r <= 0xb5) || r == 0xb8 || r == 0xb9)
			kx = 0.01607f, kz = 0.0207f;
		break;
	}
	*X = x * kx + ox, *Y = oy - z * kz;
	// the west apartment building shown on its own map (no east map yet)
	if (!F(25) && F(26) && (s_shown == 7 || s_shown == 8)) {
		if (r >= 0x20 && r <= 0x28 && r != 0x25 && r != 0x26)
			*X += s_shown == 7 ? 24.965286f : 164.96529f;
		else if (r == 0x25 || r == 0x26)
			*X += s_shown == 7 ? 16.965286f : 158.96529f;
	}
	if (r == 0xc0 && y <= -1900.0f) *X += 191.0f;
	else if (r == 0xcb && y <= -1200.0f) *X += 192.0f;
	else if (r == 0xcd && y >= -400.0f) *X -= 189.8f;
	return true;
}

// The maps a direction key leads to and whether two maps are floors of one place (0x49b330).
static bool same_place(int next, int shown)
{
	switch (next) {
	case 1: return shown > 0 && shown < 4;
	case 9: return shown > 3 && shown < 12;
	case 12: return shown > 11 && shown < 16;
	case 16: return shown > 15 && shown < 20;
	case 21: return shown > 19 && shown < 24;
	case 25: return shown > 23 && shown < 28;
	case 29: return shown > 27 && shown < 32;
	case 33: return shown > 31 && shown < 36;
	case 36: return shown == 36;
	default: return false;
	}
}

static int nav(int dir)
{
	int cur = s_shown;
	for (int guard = 0; guard < N_MAPS; guard++) {
		int next = cur > 0 && cur < N_MAPS ? s_maps[cur].nav[dir] : 0;
		if (next <= 0 || next >= N_MAPS || (s_pl.chapter == 1 && dir < 2 && (next == 1 || next == 2)))
			return 0;
		if (dir < 2)
			return available(next) ? next : 0;
		if (same_place(next, s_shown))
			return 0;
		if (available(next))
			return next;
		if (next == 9) {
			if (F(25)) return 4;
			if (F(26)) return s_james == 8 ? 8 : 7;
		} else if (next == 16 && F(29)) {
			return 18;
		} else if (next == 29 && (F(30) || F(31))) {
			return 21;
		}
		cur = next;
	}
	return 0;
}

// ---------------------------------------------------------------- textures

static bool load_t3x(C3D_Tex* tex, const char* name)
{
	char path[64];
	snprintf(path, sizeof path, PACK_DIR "/map/%s.t3x", name);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, tex, NULL, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	return true;
}

// The map's and its marker sheet's textures, kept while they stay the same (the screen and the
// touch screen's small map share them; they are never drawn at the same time).
static void textures_for(int m)
{
	const MapRec* r = &s_maps[m];
	if (strcmp(s_tex_name, r->tex)) {
		if (s_tex_ok) C3D_TexDelete(&s_tex);
		s_tex_ok = r->tex[0] && load_t3x(&s_tex, r->tex);
		snprintf(s_tex_name, sizeof s_tex_name, "%s", r->tex);
	}
	if (strcmp(s_mark_name, r->mark)) {
		if (s_mark_ok) C3D_TexDelete(&s_mark);
		s_mark_ok = r->mark[0] && load_t3x(&s_mark, r->mark);
		snprintf(s_mark_name, sizeof s_mark_name, "%s", r->mark);
	}
}

// 0x49af20: load the map and its marker sheet unless they are the ones in memory.
static void show(int m)
{
	s_shown = m;
	textures_for(m);
	s_cx = s_cy = 0.0f;
	float X, Y;
	if (m == s_james && james_pos(&X, &Y))
		s_cx = 2.0f * X, s_cy = 2.0f * Y;
	s_cx = s_cx > 256.0f ? 256.0f : s_cx < -256.0f ? -256.0f : s_cx;
	s_cy = s_cy > 256.0f ? 256.0f : s_cy < -256.0f ? -256.0f : s_cy;
}

static void free_textures(void)
{
	if (s_tex_ok) C3D_TexDelete(&s_tex);
	if (s_mark_ok) C3D_TexDelete(&s_mark);
	s_tex_ok = s_mark_ok = false;
	s_tex_name[0] = s_mark_name[0] = 0;
}

// ---------------------------------------------------------------- the screen

void map_set_open_zoomed(bool in) { s_open_in = in; }

int map_open(const MapPlayer* p)
{
	if (!s_ready)
		return MAP_NONE;
	s_pl = *p;
	s_room = bgm_room_id(p->glb_crd, p->x, p->z);
	s_james = james_map(s_room, p->x, p->y, p->z);
	int m = s_james, ret = MAP_OPEN;
	if (m <= 0 || m >= N_MAPS || !available(m)) {
		if (!F(24))
			return MAP_NONE;
		m = p->chapter == 1 ? 3 : 1;
		ret = MAP_NONE_TOWN;
	}
	show(m);
	// the PC opens zoomed in, except on the town map when the player is not on it, and only with
	// the Game Options' Map on Zoom In (0x49de50 reads 0x1dbec0a)
	s_zoom = s_open_in && (m != 1 || m == s_james) ? 1.0f : 0.0f;
	s_zoom_state = s_zoom > 0.0f ? 2 : 0;
	s_open = true;
	return ret;
}

bool map_is_open(void) { return s_open; }

static float code_x(u32 code) { return (int)((code >> 19 & 0x1fff) - 0x1000) * 0.0625f; }
static float code_y(u32 code) { return (int)((code >> 6 & 0x1fff) - 0x1000) * 0.0625f; }

static void mark_begin(int mode)
{
	s_mode = mode;
	s_mk = s_mstate = 0;
	s_mt = 0.0f;
	memset(s_alpha, 0, sizeof s_alpha);
	show(mode == 3 || mode == 5 ? 3 : 2);
	s_zoom = 0.0f;
	s_zoom_state = 0;
}

bool map_mark_open(int mode, const MapPlayer* p)
{
	if (!s_ready || mode < 0 || mode > 4 || s_nanim[mode] <= 0)
		return false;
	s_pl = *p;
	s_room = bgm_room_id(p->glb_crd, p->x, p->z);
	s_james = -1;                    // no arrow while marking
	mark_begin(mode);
	s_open = true;
	return true;
}

// 0x49c180 and the zoom of 0x49d7a0: one frame of the marking. False when the last mark is in.
static bool mark_update(float dt)
{
	const MarkAnim* a = s_anim[s_mode];
	if (s_zoom_state == 1 && (s_zoom += dt) >= 1.0f)
		s_zoom = 1.0f, s_zoom_state = 2;
	if (s_zoom_state == 4) {
		s_scroll += dt;
		if (s_scroll >= 1.0f)
			s_scroll = 1.0f, s_zoom_state = 0;
		s_cx = s_fx + (s_tx - s_fx) * s_scroll, s_cy = s_fy + (s_ty - s_fy) * s_scroll;
	}
	s_mt += dt * 30.0f;
	switch (s_mstate) {
	case 0:
		s_cx = code_x(a[s_mk].code), s_cy = code_y(a[s_mk].code);
		s_mstate = 1, s_mt = 0.0f;
		break;
	case 1:
		if (s_mt > 16.0f)
			s_zoom_state = 1, s_mstate = 2, s_mt = 0.0f;
		break;
	case 2:
		if (s_zoom_state == 2)
			s_mstate = 3, s_mt = 0.0f;
		break;
	case 3:                          // eight steps of the 0x80 alpha a frame
		if (s_mk < 8) {
			int v = (int)(s_mt * 8.0f);
			s_alpha[s_mk] = v >= 0x80 ? 0x80 : (u8)v;
		}
		if (s_mt * 8.0f >= 128.0f)
			s_mstate = 4, s_mt = 0.0f;
		break;
	case 4:
		if (s_mt <= 24.0f)
			break;
		if (++s_mk >= s_nanim[s_mode])
			return false;
		s_fx = s_cx, s_fy = s_cy;
		s_tx = code_x(a[s_mk].code), s_ty = code_y(a[s_mk].code);
		s_scroll = 0.0f;
		s_zoom_state = 4;
		s_mstate = 5;
		break;
	case 5:
		if (s_zoom_state == 0)
			s_mstate = 0, s_mt = 0.0f;
		break;
	}
	return true;
}

bool map_marking(void) { return s_open && s_mode >= 0; }

void map_close(void)
{
	s_open = false;
	s_mode = -1;
	if (!s_mini_used)
		free_textures();
}

bool map_update(u32 down, u32 held, const circlePosition* cp, float dt)
{
	if (!s_open)
		return false;
	if (s_mode >= 0) {               // marking: no keys, the map closes by itself (0x49dbd0)
		if (mark_update(dt))
			return true;
		if (s_mode == 0) {           // the hospital's marks go on with the west map
			mark_begin(5);
			return true;
		}
		s_mode = -1;
		map_close();
		return false;
	}
	if (s_zoom_state == 1 && (s_zoom += dt * 2.0f) >= 1.0f)
		s_zoom = 1.0f, s_zoom_state = 2;
	if (s_zoom_state == 3 && (s_zoom -= dt * 2.0f) <= 0.0f)
		s_zoom = 0.0f, s_zoom_state = 0;
	if (down & (KEY_B | KEY_START)) {
		map_close();
		return false;
	}
	if (s_zoom_state == 0) {
		int m = 0;
		if (down & KEY_A)
			s_zoom_state = 1;
		else if (down & KEY_DUP) m = nav(0);
		else if (down & KEY_DDOWN) m = nav(1);
		else if (down & (KEY_DRIGHT | KEY_R)) m = nav(2);
		else if (down & (KEY_DLEFT | KEY_L)) m = nav(3);
		if (m) {
			if (s_pl.chapter == 1 && m == 1)
				m = 3;
			show(m);
		}
	} else if (s_zoom_state == 2) {
		if (down & KEY_A) {
			s_zoom_state = 3;
		} else {
			// 0x49c6b0: the stick scrolls 307.2 / 384 units a second, as does the D-pad
			float sx = cp->dx / 156.0f, sy = -cp->dy / 156.0f;
			if (held & KEY_DRIGHT) sx = 1.0f;
			if (held & KEY_DLEFT) sx = -1.0f;
			if (held & KEY_DDOWN) sy = 1.0f;
			if (held & KEY_DUP) sy = -1.0f;
			if (fabsf(sx) < 0.15f) sx = 0.0f;
			if (fabsf(sy) < 0.15f) sy = 0.0f;
			s_cx += sx * dt * 307.2f;
			s_cy += sy * dt * 384.0f;
			s_cx = s_cx > 256.0f ? 256.0f : s_cx < -256.0f ? -256.0f : s_cx;
			s_cy = s_cy > 256.0f ? 256.0f : s_cy < -256.0f ? -256.0f : s_cy;
		}
	}
	return true;
}

void map_drag(float dx, float dy)
{
	// the zoomed map follows the finger: zoomed in, map_draw shows 512 texels across 320 pixels and
	// 512 down 240, so a pixel of drag moves the map a pixel on the top screen
	if (!s_open || s_mode >= 0 || s_zoom_state != 2)
		return;
	s_cx -= dx * (512.0f / 320.0f);
	s_cy -= dy * (512.0f / 240.0f);
	s_cx = s_cx > 256.0f ? 256.0f : s_cx < -256.0f ? -256.0f : s_cx;
	s_cy = s_cy > 256.0f ? 256.0f : s_cy < -256.0f ? -256.0f : s_cy;
}

// Map units to top-screen pixels: the map fills 320x240 (4:3) in the middle of the screen.
static float sx(float u) { return 200.0f + u * (320.0f / 512.0f); }
static float sy(float u) { return 120.0f + u * (240.0f / 512.0f); }

static void quad(C3D_Tex* tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 rgba)
{
	MapVert v[6] = {
		{ x0, y0, u0, v0 }, { x1, y0, u1, v0 }, { x1, y1, u1, v1 },
		{ x0, y0, u0, v0 }, { x1, y1, u1, v1 }, { x0, y1, u0, v1 },
	};
	s_draw(tex, v, 6, rgba);
}

void map_draw(void)
{
	if (!s_open || !s_draw)
		return;
	float z = s_zoom, f = z / (2.0f - z);
	quad(NULL, 0, 0, 400, 240, 0, 0, 0, 0, 0x000000ff);
	if (s_tex_ok) {
		// 0x49d9d0: a (2 - zoom) * 512 texel window of the 1024 map
		float u0 = (s_cx + 256.0f) * z, v0 = (s_cy + 256.0f) * z, size = (2.0f - z) * 512.0f;
		float u1 = u0 + size > 1023.0f ? 1023.0f : u0 + size, v1 = v0 + size > 1023.0f ? 1023.0f : v0 + size;
		quad(&s_tex, 40, 0, 360, 240, u0 / 1024.0f, v0 / 1024.0f, u1 / 1024.0f, v1 / 1024.0f, 0xffffffff);
	}
	// 0x49afa0: the markers whose flags are set, anchored at their top left corner
	const MapRec* r = &s_maps[s_shown];
	for (int t = 0; t < r->ntab && s_mark_ok; t++)
		for (int i = 0; i < r->tab[t].count; i++) {
			// while marking, the marks of the animation fade in whatever their flags say
			u32 rgba = 0xffffffff;
			int k = -1;
			for (int j = 0; t == 0 && s_mode >= 0 && j < s_nanim[s_mode] && j < 8; j++)
				if (s_anim[s_mode][j].mark == i)
					k = j;
			if (k >= 0)
				rgba = 0xffffff00 | (s_alpha[k] >= 0x80 ? 0xff : s_alpha[k] * 2);
			else if (!F(r->tab[t].first + 1 + i))
				continue;
			const MarkRec* m = &r->tab[t].recs[i];
			float x = (int)((m->code >> 19 & 0x1fff) - 0x1000) * 0.0625f;
			float y = (int)((m->code >> 6 & 0x1fff) - 0x1000) * 0.0625f;
			float X0 = (x - s_cx) * f + x, Y0 = (y - s_cy) * f + y;
			float w = (m->rect[2] - m->rect[0]) * (f + 1.0f) * 0.5f, h = (m->rect[3] - m->rect[1]) * (f + 1.0f) * 0.5f;
			quad(&s_mark, sx(X0), sy(Y0), sx(X0 + w), sy(Y0 + h), m->rect[0] / (float)s_mark.width,
			     m->rect[1] / (float)s_mark.height, m->rect[2] / (float)s_mark.width, m->rect[3] / (float)s_mark.height,
			     rgba);
		}
	// the player's arrow, on the map he is on (the town map only around the town rooms)
	float X, Y;
	if (s_shown != s_james || !james_pos(&X, &Y))
		return;
	if (s_shown == 1 && !(s_room >= 1 && s_room <= 3) && !(s_room == 0x0e && F(381)))
		return;
	float a = s_pl.yaw;
	if (s_shown == 0x10 || s_shown == 0x11 || s_room == 1 || s_room == 5) a -= (float)M_PI / 2.0f;
	else if (s_shown == 0x12 || s_shown == 0x13) a += (float)M_PI / 2.0f;
	else if (s_room == 9 || s_room == 10 || s_room == 11 || s_room == 0x5c) a -= (float)M_PI;
	float s = sinf(a), c = cosf(a);
	static const float shape[6][2] = { { 0, -16 }, { -10, 12 }, { 0, 5 }, { 0, -16 }, { 0, 5 }, { 10, 12 } };
	for (int pass = 0; pass < 2; pass++) {
		MapVert v[6];
		for (int i = 0; i < 6; i++) {
			float lx = shape[i][0], ly = shape[i][1];
			float px = (lx * c - ly * s) * 0.8f + X + (pass ? 0.0f : 2.0f);
			float py = (lx * s + ly * c) * 0.8f + Y + (pass ? 0.0f : 0.5f);
			v[i] = (MapVert){ sx((px - s_cx) * f + px), sy((py - s_cy) * f + py), 0, 0 };
		}
		s_draw(NULL, v, 6, pass ? 0xd01818ff : 0x000000c0);
	}
}

// ---------------------------------------------------------------- the touch screen's small map

// The map James is on, as map_open would show it, or 0 when he has none of it.
static int mini_map(const MapPlayer* p, float* X, float* Y)
{
	if (!s_ready || s_open)
		return 0;
	s_pl = *p;
	s_room = bgm_room_id(p->glb_crd, p->x, p->z);
	int m = s_james = james_map(s_room, p->x, p->y, p->z);
	if (m <= 0 || m >= N_MAPS || !available(m))
		return 0;
	s_shown = m;
	if (!james_pos(X, Y))
		return 0;
	if (m == 1 && !(s_room >= 1 && s_room <= 3) && !(s_room == 0x0e && F(381)))
		return 0;
	return m;
}

u32 map_mini_key(const MapPlayer* p, float zoom)
{
	float X = 0, Y = 0;
	int m = mini_map(p, &X, &Y);
	if (!m)
		return 0;
	u32 marks = 0;
	const MapRec* r = &s_maps[m];
	for (int t = 0; t < r->ntab; t++)
		for (int i = 0; i < r->tab[t].count; i++)
			marks = marks * 31 + F(r->tab[t].first + 1 + i);
	u32 px = (u32)(int)(X * zoom), py = (u32)(int)(Y * zoom), a = (u32)(int)(p->yaw * 4.0f);
	return ((u32)m * 2654435761u) ^ (px * 40503u) ^ (py * 9973u) ^ (a * 131u) ^ marks ^ 1u;
}

bool map_mini(const MapPlayer* p, MapDrawFn fn, float x0, float y0, float x1, float y1, float zoom)
{
	float X, Y;
	int m = mini_map(p, &X, &Y);
	if (!m || !fn)
		return false;
	s_mini_used = true;
	textures_for(m);
	// map units to pixels: the PC's 4:3 shape (as the screen draws it), zoom times bigger
	float kx = 0.625f * zoom, ky = 0.46875f * zoom;
	float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
	// the units the rectangle shows, clipped to the map's -256..256
	float ux0 = X - (cx - x0) / kx, ux1 = X + (x1 - cx) / kx, uy0 = Y - (cy - y0) / ky, uy1 = Y + (y1 - cy) / ky;
	float cux0 = ux0 < -256.0f ? -256.0f : ux0, cux1 = ux1 > 256.0f ? 256.0f : ux1;
	float cuy0 = uy0 < -256.0f ? -256.0f : uy0, cuy1 = uy1 > 256.0f ? 256.0f : uy1;
	MapVert v[6];
#define PX(u) (cx + ((u) - X) * kx)
#define PY(u) (cy + ((u) - Y) * ky)
	if (s_tex_ok && cux1 > cux0 && cuy1 > cuy0) {
		float a0 = PX(cux0), b0 = PY(cuy0), a1 = PX(cux1), b1 = PY(cuy1);
		float u0 = (cux0 + 256.0f) / 512.0f, v0 = (cuy0 + 256.0f) / 512.0f;
		float u1 = (cux1 + 256.0f) / 512.0f, v1 = (cuy1 + 256.0f) / 512.0f;
		v[0] = (MapVert){ a0, b0, u0, v0 }, v[1] = (MapVert){ a1, b0, u1, v0 }, v[2] = (MapVert){ a1, b1, u1, v1 };
		v[3] = v[0], v[4] = v[2], v[5] = (MapVert){ a0, b1, u0, v1 };
		fn(&s_tex, v, 6, 0xffffffff);
	}
	// the red markings whose flags are set (0x49afa0), at the screen's zoomed-out size times zoom
	const MapRec* r = &s_maps[m];
	for (int t = 0; t < r->ntab && s_mark_ok; t++)
		for (int i = 0; i < r->tab[t].count; i++) {
			if (!F(r->tab[t].first + 1 + i))
				continue;
			const MarkRec* mk = &r->tab[t].recs[i];
			float x = code_x(mk->code), y = code_y(mk->code);
			float w = (mk->rect[2] - mk->rect[0]) * 0.5f, h = (mk->rect[3] - mk->rect[1]) * 0.5f;
			float a0 = PX(x), b0 = PY(y), a1 = PX(x + w), b1 = PY(y + h);
			if (a1 < x0 || a0 > x1 || b1 < y0 || b0 > y1)
				continue;
			float u0 = mk->rect[0] / (float)s_mark.width, v0 = mk->rect[1] / (float)s_mark.height;
			float u1 = mk->rect[2] / (float)s_mark.width, v1 = mk->rect[3] / (float)s_mark.height;
			v[0] = (MapVert){ a0, b0, u0, v0 }, v[1] = (MapVert){ a1, b0, u1, v0 }, v[2] = (MapVert){ a1, b1, u1, v1 };
			v[3] = v[0], v[4] = v[2], v[5] = (MapVert){ a0, b1, u0, v1 };
			fn(&s_mark, v, 6, 0xffffffff);
		}
#undef PX
#undef PY
	// James' arrow in the middle, turned as the screen turns it
	float a = p->yaw;
	if (m == 0x10 || m == 0x11 || s_room == 1 || s_room == 5) a -= (float)M_PI / 2.0f;
	else if (m == 0x12 || m == 0x13) a += (float)M_PI / 2.0f;
	else if (s_room == 9 || s_room == 10 || s_room == 11 || s_room == 0x5c) a -= (float)M_PI;
	float s = sinf(a), c = cosf(a);
	static const float shape[6][2] = { { 0, -16 }, { -10, 12 }, { 0, 5 }, { 0, -16 }, { 0, 5 }, { 10, 12 } };
	for (int pass = 0; pass < 2; pass++) {
		for (int k = 0; k < 6; k++) {
			float lx = shape[k][0] * 0.55f, ly = shape[k][1] * 0.55f;
			v[k] = (MapVert){ cx + lx * c - ly * s + (pass ? 0.0f : 1.0f), cy + lx * s + ly * c + (pass ? 0.0f : 1.0f), 0, 0 };
		}
		fn(NULL, v, 6, pass ? 0xd01818ff : 0x000000c0);
	}
	return true;
}

bool map_zoomed(void) { return s_open && s_zoom_state >= 1 && s_zoom_state <= 2; }

const char* map_name(void)
{
	map_legend();
	static char name[48];
	snprintf(name, sizeof name, "%.47s", s_legend);
	char* nl = strchr(name, '\n');
	if (nl)
		*nl = 0;
	for (char* e = name + strlen(name); e > name && e[-1] == ' '; )
		*--e = 0;
	return name;
}

const char* map_legend(void)
{
	static const char* const place[N_MAPS] = {
		"", "Silent Hill", "South Vale (east)", "South Vale (west)",
		"Wood Side Apartments 1F", "Wood Side Apartments 2F", "Wood Side Apartments 3F",
		"Blue Creek Apartments", "Blue Creek Apartments",
		"Apartments 1F", "Apartments 2F", "Apartments 3F",
		"Brookhaven Hospital", "Brookhaven Hospital", "Brookhaven Hospital", "Brookhaven Hospital",
		"Toluca Prison", "Toluca Prison", "Labyrinth", "Labyrinth",
	};
	static const char* const floor[4] = { "BF", "1F", "2F", "3F" };
	char name[48];
	if (s_shown >= 20 && s_shown < 36)
		snprintf(name, sizeof name, "Lakeview Hotel %s", floor[(s_shown - 20) & 3]);
	else if (s_shown == 36)
		snprintf(name, sizeof name, "Baldwin Mansion");
	else
		snprintf(name, sizeof name, "%s", s_shown > 0 && s_shown < 20 ? place[s_shown] : "");
	snprintf(s_legend, sizeof s_legend, "%-30s\n\n%s", name,
	         s_zoom_state >= 2 ? "Circle Pad / D-pad: look around\nA: whole map   B: close"
	                           : "D-pad: other floors and maps\nA: zoom in   B: close");
	return s_legend;
}

// ---------------------------------------------------------------- memos (0x4989c0..0x49a2c0)

int memo_count(void) { return s_nmemos; }

int memo_list(int* out, int max)
{
	int n = 0;
	for (int i = 0; i < s_nmemos && n < max; i++)
		if (F(s_memos[i].flag))
			out[n++] = i;
	return n;
}

const char* memo_title(int memo) { return memo >= 0 && memo < s_nmemos ? ev_memo_message(s_memos[memo].title) : NULL; }

const char* memo_text(int memo)
{
	if (memo < 0 || memo >= s_nmemos || s_memos[memo].kind == 0)
		return NULL;
	return prog_memo_text(memo, ev_memo_message(s_memos[memo].text));
}

const char* memo_picture(int memo, int which)
{
	if (memo < 0 || memo >= s_nmemos || which < 0 || which > 1 || !s_memos[memo].pic[which][0])
		return NULL;
	return s_memos[memo].pic[which];
}

int memo_kind(int memo) { return memo >= 0 && memo < s_nmemos ? s_memos[memo].kind : 1; }
