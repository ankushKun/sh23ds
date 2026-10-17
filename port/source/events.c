#include "events.h"
#include "prog.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"

typedef struct {
	u32 flag, cond, rslt0, rslt1;
	u8 shape, npts, any_height, keep_facing;
	float y;
	float pts[8][2];
	float dest_pos[3], dest_facing;
	u16 dest_stage, pad0;
	char dest_map[8], trig_map[8];
	u32 pad1;
} EvRec;

typedef struct {
	float pos[3], rot_y;
	u16 flag, gives_item, system_message, pad;
	char map[8];
} ItemRec;

typedef struct {
	u32 count;
	u32* offs;
	char* data;
	u32 size;
} MsgFile;

static const char* msg_get(const MsgFile* m, u32 i);
static void apply_ee_fixes(void);
static bool msg_load(MsgFile* m, const char* path);

enum { SH_NONE, SH_POINT, SH_SEGMENTS, SH_RECT, SH_POLYGON };
enum { RT_FLAG_SET = 1, RT_FLAG_CLEAR, RT_PROGRAM, RT_DOOR, RT_DOOR_PROGRAM, RT_MESSAGE, RT_UNLOCK, RT_LOCKED,
       RT_JAMMED, RT_SAVE };

static EvRec* s_ev;
static ItemRec* s_items;
static u32 s_nev, s_nitems;
static int s_stage;
static char s_name[24];
static int s_glb_crd;
static MsgFile s_msg, s_common, s_itemtext, s_memotext, s_resulttext, s_mcardtext;
static u32 s_flags[EV_MAX_FLAGS / 32];
static u8 s_items_held[256];        // count per item number (ammo and health items stack)
static int s_last_fired = -1;       // rising edge for non-button triggers

_Static_assert(sizeof(EvRec) == 128, "S3E event record");
_Static_assert(sizeof(ItemRec) == 32, "S3E item record");

bool ev_flag(int n) { return n > 0 && n < EV_MAX_FLAGS && (s_flags[n >> 5] >> (n & 31) & 1); }

void ev_flag_set(int n, bool v)
{
	if (n <= 0 || n >= EV_MAX_FLAGS)
		return;
	if (v) s_flags[n >> 5] |= 1u << (n & 31);
	else s_flags[n >> 5] &= ~(1u << (n & 31));
}

bool ev_have_item(int item) { return ev_item_count(item) > 0; }
int ev_item_count(int item) { return item > 0 && item < 256 ? s_items_held[item] : 0; }

void ev_give_item(int item, int n)
{
	if (item > 0 && item < 256)
		s_items_held[item] = s_items_held[item] + n > 255 ? 255 : s_items_held[item] + n;
}

static void take_item(int item) { if (item > 0 && item < 256) s_items_held[item] = 0; }

// Item n is named by item_msg 18 + 2n and described by 19 + 2n (the table lines up exactly with
// the pickups: 1 health drink, 5 handgun bullets, 7 shotgun shells, 9 rifle shells, ...).
const char* ev_item_name(int item) { return msg_get(&s_itemtext, 18 + 2 * item); }
const char* ev_item_desc(int item) { return msg_get(&s_itemtext, 19 + 2 * item); }

// Amount per pickup, as ItemGet (0x49e310) adds it: a health item and the revolver 1, the handgun
// and its bullets 10, the shotgun and its shells 6, the rifle and its shells 4, the hyper spray 8.
// The four ammo boxes (5, 7, 9, 11) are multiplied by the Extra Options "Bullet Adjust"
// (0x1dbec05, x1 to x3; the options page 0x4630c0 cycles it). Difficulty does not change the
// amount; EventItemConditionCheck (0x48cb20) only decides which pickups exist on which level.
static int s_bullet_adjust = 1;

void ev_set_bullet_adjust(int times) { s_bullet_adjust = times < 1 ? 1 : times > 3 ? 3 : times; }
int ev_bullet_adjust(void) { return s_bullet_adjust; }

int ev_item_get_amount(int item)
{
	switch (item) {
	case 1: case 2: case 3: case 10: return 1;
	case 4: return 10;
	case 5: case 11: return 10 * s_bullet_adjust;
	case 6: return 6;
	case 7: return 6 * s_bullet_adjust;
	case 8: return 4;
	case 9: return 4 * s_bullet_adjust;
	case 12: return 8;
	default: return 1;
	}
}

void ev_state_get(void* buf)
{
	memcpy(buf, s_flags, sizeof s_flags);
	memcpy((u8*)buf + sizeof s_flags, s_items_held, sizeof s_items_held);
}

void ev_state_set(const void* buf)
{
	memcpy(s_flags, buf, sizeof s_flags);
	memcpy(s_items_held, (const u8*)buf + sizeof s_flags, sizeof s_items_held);
	s_last_fired = -1;
}

void ev_reset(void)
{
	memset(s_flags, 0, sizeof s_flags);
	memset(s_items_held, 0, sizeof s_items_held);
	s_last_fired = -1;
}

int ev_stage(void) { return s_stage; }
int ev_glb_crd(void) { return s_glb_crd; }
const char* ev_stage_name(void) { return s_name; }
int ev_count(void) { return (int)s_nev; }
const char* ev_stage_message(int n) { return n >= 0 ? msg_get(&s_msg, (u32)n) : NULL; }
const char* ev_system_message(int n) { return n >= 0 ? msg_get(&s_common, (u32)n) : NULL; }
// Inserted values (PC MesSetInsert 0x47fd70): the converter keeps the PC's ff e0..ff e3 codes as
// bytes 1..4 and the question code ff f6 as byte 5 (tools/sh2conv/sh2mes.py); a message is expanded
// when it is shown, with the values set last, as on the PC.
static char s_insert[4][24];
static char s_expanded[1024];

void ev_msg_set_insert(int slot, const char* value)
{
	if (slot >= 0 && slot < 4)
		snprintf(s_insert[slot], sizeof s_insert[slot], "%s", value ? value : "");
}

const char* ev_msg_expand(const char* text)
{
	if (!text || (text >= s_expanded && text < s_expanded + sizeof s_expanded))
		return text;                // already expanded (the next page of a shown message)
	const char* c = text;
	while (*c && (u8)*c > 5)
		c++;
	if (!*c)
		return text;
	size_t n = 0;
	for (c = text; *c && n + 1 < sizeof s_expanded; c++) {
		u8 b = (u8)*c;
		if (b >= 1 && b <= 4)
			for (const char* v = s_insert[b - 1]; *v && n + 1 < sizeof s_expanded; v++)
				s_expanded[n++] = *v;
		else if (b != 5)
			s_expanded[n++] = *c;
	}
	s_expanded[n] = 0;
	return s_expanded;
}

// The results screen's labels (result_msg_e.mes), loaded when it first shows.
const char* ev_result_message(int n)
{
	static bool tried;
	if (!tried) {
		tried = true;
		msg_load(&s_resulttext, PACK_DIR "/events/result.msg");
	}
	return n >= 0 ? msg_get(&s_resulttext, (u32)n) : NULL;
}

// The save screen's texts (m_card_x_msg_e.mes), loaded when it first shows.
const char* ev_mcard_message(int n)
{
	static bool tried;
	if (!tried) {
		tried = true;
		msg_load(&s_mcardtext, PACK_DIR "/events/mcard.msg");
	}
	return n >= 0 ? msg_get(&s_mcardtext, (u32)n) : NULL;
}

// The memo list's titles and texts (memo_x_msg_e.mes, written by tools/sh2conv/maps.py), loaded
// the first time the list opens.
const char* ev_memo_message(int n)
{
	static bool tried;
	if (!tried) {
		tried = true;
		msg_load(&s_memotext, PACK_DIR "/events/memo.msg");
	}
	return n >= 0 ? msg_get(&s_memotext, (u32)n) : NULL;
}

const char* ev_item_message(int n) { return n >= 0 ? msg_get(&s_itemtext, (u32)n) : NULL; }
void* ev_flag_block(void) { return s_flags; }

static void* read_all(const char* path, u32* size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* d = n > 0 ? malloc(n) : NULL;
	if (d && fread(d, 1, n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	if (d && size)
		*size = (u32)n;
	return d;
}

static void msg_free(MsgFile* m)
{
	free(m->data);
	memset(m, 0, sizeof *m);
}

static bool msg_load(MsgFile* m, const char* path)
{
	msg_free(m);
	u32 size;
	char* d = read_all(path, &size);
	if (!d)
		return false;
	if (size < 8 || memcmp(d, "MSG1", 4)) {
		free(d);
		return false;
	}
	m->data = d;
	m->size = size;
	m->count = *(u32*)(d + 4);
	m->offs = (u32*)(d + 8);
	if (8 + m->count * 4 > size)
		m->count = 0;
	return true;
}

static const char* msg_get(const MsgFile* m, u32 i)
{
	if (i >= m->count || m->offs[i] >= m->size)
		return NULL;
	return m->data + m->offs[i];
}

bool ev_load_stage(int stage)
{
	char path[96];
	snprintf(path, sizeof path, PACK_DIR "/events/s%02d.s3e", stage);
	u32 size;
	u8* d = read_all(path, &size);
	if (!d)
		return false;
	u32* h = (u32*)d;
	if (size < 48 || memcmp(d, "S3E1", 4) || h[1] != 2 || 48 + h[3] * sizeof(EvRec) + h[4] * sizeof(ItemRec) > size) {
		free(d);
		return false;
	}
	free(s_ev);
	free(s_items);
	s_nev = h[3];
	s_nitems = h[4];
	s_ev = malloc(s_nev * sizeof(EvRec) + 1);
	s_items = malloc(s_nitems * sizeof(ItemRec) + 1);
	memcpy(s_ev, d + 48, s_nev * sizeof(EvRec));
	memcpy(s_items, d + 48 + s_nev * sizeof(EvRec), s_nitems * sizeof(ItemRec));
	s_glb_crd = (int)h[5];
	memcpy(s_name, d + 24, sizeof s_name);
	s_name[sizeof s_name - 1] = 0;
	free(d);
	s_stage = stage;
	s_last_fired = -1;
	apply_ee_fixes();
	snprintf(path, sizeof path, PACK_DIR "/events/s%02d.msg", stage);
	msg_load(&s_msg, path);
	if (!s_common.data)
		msg_load(&s_common, PACK_DIR "/events/common.msg");
	if (!s_itemtext.data)
		msg_load(&s_itemtext, PACK_DIR "/events/item.msg");
	return true;
}

// Enhanced Edition fixes to the PC event tables (zlib licence). The pack carries the tables as the
// exe has them, so the fixes are applied here after loading. Each one only applies while the record
// still holds the PC words.
typedef struct {
	u8 stage, index;
	u32 cond, rslt0, rslt1;              // PC values
	u32 new_cond, new_rslt0, new_rslt1;
} EvFix;

static const EvFix EE_FIXES[] = {
	// "FireEscapeKeyFix" (Patches/FireEscapeKey.cpp): clears the needs-light bit (cond bit 2) of the
	// fire escape key pickup in 3F (stage 9 event 6) and of the key behind the bars (stage 8
	// event 0), so they work without the flashlight. trigger_pass does not test that bit yet, so
	// this only matters once it does.
	{ 9, 6, 0x20340004, 0x30000000, 0x0000c05c, 0x20340000, 0x30000000, 0x0000c05c },
	{ 8, 0, 0x20004004, 0x30000000, 0x00004000, 0x20004000, 0x30000000, 0x00004000 },
	// dllmain.cpp "Patching hotel map to not require the flashlight": the same bit on the hotel
	// map pickup (stage 40 event 5).
	{ 40, 5, 0x20523004, 0x30000000, 0x0000401e, 0x20523000, 0x30000000, 0x0000401e },
	// "FixTownGateEvents" (Patches/Langs.cpp PatchTownGateEvents): once flag 251 is set (back in
	// town after the hospital) the PC calls the Heaven's Night back alley gate and the Wood Side
	// gate jammed with a broken lock, although James went through both. The alley gate shows
	// stage message 78 instead ("There's nothing more to do here.") and the Wood Side gate stage
	// message 0 ("There's no reason to go back."), both without the jam sound.
	{ 14, 10, 0x206e6000, 0x90000000, 0x023fc000, 0x206e6000, 0x60000000, 0x00138000 },
	{ 5, 11, 0x20a62000, 0x90000000, 0x03bfc000, 0x20a62000, 0x90000000, 0x00000000 },
};

static void apply_ee_fixes(void)
{
	for (u32 i = 0; i < sizeof EE_FIXES / sizeof EE_FIXES[0]; i++) {
		const EvFix* f = &EE_FIXES[i];
		if (f->stage != s_stage || f->index >= s_nev)
			continue;
		EvRec* e = &s_ev[f->index];
		if (e->cond == f->cond && e->rslt0 == f->rslt0 && e->rslt1 == f->rslt1) {
			e->cond = f->new_cond;
			e->rslt0 = f->new_rslt0;
			e->rslt1 = f->new_rslt1;
		}
	}
}

// The first map rooms.txt gives to a stage (doors that name no room: the lake of the Rebirth ending).
bool ev_map_of_stage(int stage, char out[8])
{
	FILE* f = fopen(PACK_DIR "/events/rooms.txt", "r");
	if (!f)
		return false;
	char name[16];
	int st;
	bool found = false;
	while (!found && fscanf(f, "%15s %d", name, &st) == 2)
		if (st == stage) {
			snprintf(out, 8, "%.7s", name);
			found = true;
		}
	fclose(f);
	return found;
}

int ev_stage_for_map(const char* map)
{
	FILE* f = fopen(PACK_DIR "/events/rooms.txt", "r");
	if (!f)
		return 0;
	char name[16];
	int st, found = 0;
	while (fscanf(f, "%15s %d", name, &st) == 2)
		if (!strcmp(name, map)) {
			found = st;
			break;
		}
	fclose(f);
	return found;
}

// ---------------------------------------------------------------- trigger tests

static float cross2(float ux, float uz, float vx, float vz) { return ux * vz - uz * vx; }

// Height window shared by look and inside tests: py - 500 <= y <= py + 100 (Y points down).
static bool height_ok(const EvRec* e, float py)
{
	return e->any_height || (e->y >= py - 500.0f && e->y <= py + 100.0f);
}

// Look at a point: within 400 units and within 30 degrees of the facing, measured from a point 50
// units in front of the player (0x48b490).
static bool look_point(float px, float pz, float yaw, float x, float z, float radius)
{
	float dx = x - px, dz = z - pz;
	if (fabsf(dx) > radius || fabsf(dz) > radius || dx * dx + dz * dz > radius * radius)
		return false;
	float fx = sinf(yaw), fz = cosf(yaw);
	float ox = x - (px + fx * 50.0f), oz = z - (pz + fz * 50.0f);
	float len = sqrtf(ox * ox + oz * oz);
	if (len < 1.0f)
		return true;
	return (ox * fx + oz * fz) / len >= cosf(30.0f * (float)M_PI / 180.0f);
}

// Look at a segment: the 400-unit facing ray crosses A-B and the player is on its front side,
// cross(B-A, P-A) >= 0 (0x48b570).
static bool look_segment(float px, float pz, float yaw, const float* a, const float* b)
{
	float bx = b[0] - a[0], bz = b[1] - a[1];
	if (cross2(bx, bz, px - a[0], pz - a[1]) < 0.0f)
		return false;
	float rx = sinf(yaw) * 400.0f, rz = cosf(yaw) * 400.0f;
	float den = cross2(rx, rz, bx, bz);
	if (fabsf(den) < 1e-6f)
		return false;
	float qx = a[0] - px, qz = a[1] - pz;
	float t = cross2(qx, qz, bx, bz) / den;     // along the ray
	float u = cross2(qx, qz, rx, rz) / den;     // along the segment
	return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

static bool test_look(const EvRec* e, const EvPlayer* p)
{
	if (!height_ok(e, p->y))
		return false;
	if (e->shape == SH_POINT)
		return look_point(p->x, p->z, p->yaw, e->pts[0][0], e->pts[0][1], 400.0f);
	if (e->shape == SH_SEGMENTS)
		for (int i = 0; i + 1 < e->npts; i++)
			if (look_segment(p->x, p->z, p->yaw, e->pts[i], e->pts[i + 1]))
				return true;
	return false;
}

static bool test_inside(const EvRec* e, const EvPlayer* p)
{
	if (!height_ok(e, p->y))
		return false;
	if (e->shape == SH_RECT)
		return p->x >= e->pts[0][0] && p->x <= e->pts[1][0] && p->z >= e->pts[0][1] && p->z <= e->pts[1][1];
	if (e->shape == SH_POLYGON) {
		for (int i = 0; i < e->npts; i++) {
			const float* a = e->pts[i];
			const float* b = e->pts[(i + 1) % e->npts];
			if ((p->x - a[0]) * (b[1] - a[1]) - (b[0] - a[0]) * (p->z - a[1]) < 0.0f)
				return false;
		}
		return true;
	}
	return false;
}

static bool test_position(const EvRec* e, const EvPlayer* p)
{
	return (e->shape == SH_RECT || e->shape == SH_POLYGON) ? test_inside(e, p) : test_look(e, p);
}

// Flag halves: number 14 bits, update bit, polarity bit. The event runs only while
// game_flag[n] == polarity.
static bool flags_pass(u32 w)
{
	int hi = (w >> 16) & 0x3fff, lo = w & 0x3fff;
	if (hi && ev_flag(hi) != (bool)(w >> 31))
		return false;
	if (lo && ev_flag(lo) != (bool)((w >> 15) & 1))
		return false;
	return true;
}

// EventExecSubFlagSet (0x48bee0): update bits make an event one-shot. The low half is only
// updated when the high half names a flag too (quirk kept from the PC code).
static void flags_update(u32 w)
{
	int hi = (w >> 16) & 0x3fff, lo = w & 0x3fff;
	if (!hi)
		return;
	if ((w >> 30) & 1)
		ev_flag_set(hi, !(w >> 31));
	if (lo && ((w >> 14) & 1))
		ev_flag_set(lo, !((w >> 15) & 1));
}

// Trigger kinds (cond >> 28). Kinds 5 and 6 need the item used from the inventory, kind 10 takes
// either the action button while holding the item or using it.
static bool trigger_pass(const EvRec* e, const EvPlayer* p, bool* edge)
{
	int kind = e->cond >> 28, item = (e->cond >> 4) & 0xff;
	if ((e->cond & 4) && !p->can_see)
		return false;            // cond bit 2: skipped while James cannot see (dark room, light off)
	*edge = false;
	switch (kind) {
	case 0:
	case 1:
		*edge = true;
		return true;
	case 2:
		return p->action && test_position(e, p);
	case 3:
	case 4:
		*edge = true;
		return test_position(e, p);
	case 5:
	case 6:
		return p->use_item == item && test_position(e, p);
	case 7:
		return p->action && ev_have_item(item) && test_position(e, p);
	case 10:
		return ((p->action && ev_have_item(item)) || p->use_item == item) && test_position(e, p);
	case 8:
	case 9:
		*edge = true;
		return ev_have_item(item) && test_position(e, p);
	default:
		return false;
	}
}

static const char* stage_msg(u32 rslt1)
{
	return msg_get(&s_msg, (rslt1 >> 14) & 0xff);
}

// The autotest's list of destinations: the current stage's door results (types 4 and 5) in list
// order, filled like exec() fills a warp but without setting flags or starting programs.
int ev_warp_count(void)
{
	int n = 0;
	for (u32 i = 0; i < s_nev; i++) {
		int type = s_ev[i].rslt0 >> 28;
		n += type == RT_DOOR || type == RT_DOOR_PROGRAM;
	}
	return n;
}

bool ev_warp_get(int k, EvAction* out)
{
	for (u32 i = 0; i < s_nev; i++) {
		const EvRec* e = &s_ev[i];
		int type = e->rslt0 >> 28;
		if ((type != RT_DOOR && type != RT_DOOR_PROGRAM) || k--)
			continue;
		memset(out, 0, sizeof *out);
		out->kind = EV_WARP;
		out->event = (int)i;
		out->stage = e->dest_stage ? e->dest_stage : s_stage;
		memcpy(out->map, e->dest_map, sizeof out->map);
		out->map[7] = 0;
		memcpy(out->pos, e->dest_pos, sizeof out->pos);
		out->facing = e->dest_facing;
		out->keep_facing = e->keep_facing;
		return true;
	}
	return false;
}

static bool exec(int i, const EvRec* e, EvAction* out)
{
	int type = e->rslt0 >> 28, flag = e->rslt1 & 0x3fff, msg = (e->rslt1 >> 14) & 0xff;
	memset(out, 0, sizeof *out);
	out->event = i;
	switch (type) {
	case RT_FLAG_SET:
	case RT_FLAG_CLEAR:
		ev_flag_set(flag, type == RT_FLAG_SET);
		break;
	case RT_PROGRAM:
		// EventExecProgram (0x48c110): the program runs over the next frames and sets the rslt1
		// flag itself when it returns nonzero
		out->kind = EV_PROGRAM;
		out->program = (e->rslt1 >> 14) & 0x7f;
		prog_start(s_stage, out->program, flag);
		break;
	case RT_DOOR:
	case RT_DOOR_PROGRAM:
		out->kind = EV_WARP;
		out->stage = e->dest_stage ? e->dest_stage : s_stage;
		memcpy(out->map, e->dest_map, sizeof out->map);
		out->map[7] = 0;
		memcpy(out->pos, e->dest_pos, sizeof out->pos);
		out->facing = e->dest_facing;
		out->keep_facing = e->keep_facing;
		out->door_sound = (e->rslt1 >> 22) & 0x3f;
		if (type == RT_DOOR_PROGRAM && prog_exists(s_stage, (e->rslt1 >> 14) & 0x7f)) {
			// warp first, then the program of this stage; its flag is set when it finishes
			out->program = (e->rslt1 >> 14) & 0x7f;
			prog_start(s_stage, out->program, flag);
		} else {
			ev_flag_set(flag, true);
		}
		break;
	case RT_MESSAGE:
		out->kind = EV_MESSAGE;
		out->text = stage_msg(e->rslt1);
		ev_flag_set(flag, true);
		break;
	case RT_UNLOCK:
		out->kind = EV_MESSAGE;
		out->text = msg == 0xff ? msg_get(&s_common, 5) : stage_msg(e->rslt1);
		out->door_sound = (e->rslt1 >> 22) & 0x3f;
		out->door_slot = 2;
		ev_flag_set(flag, true);
		break;
	case RT_LOCKED:
		out->kind = EV_MESSAGE;
		out->text = msg_get(&s_common, 4);
		out->door_sound = (e->rslt1 >> 22) & 0x3f;
		out->door_slot = 4;
		break;
	case RT_JAMMED:
		out->kind = EV_MESSAGE;
		out->text = msg == 0xff ? msg_get(&s_common, 6) : msg == 0xfe ? msg_get(&s_common, 7) : stage_msg(e->rslt1);
		out->door_sound = (e->rslt1 >> 22) & 0x3f;
		out->door_slot = 3;
		break;
	case RT_SAVE:
		// the first touch of a save point shows system message 8 (0x48c680), later ones go straight
		// to the save prompt
		out->kind = EV_SAVE;
		out->text = ev_flag(flag) ? NULL : msg_get(&s_common, 8);
		ev_flag_set(flag, true);
		break;
	default:
		return false;
	}
	flags_update(e->flag);
	if ((e->cond >> 28) >= 5 && (e->cond & 8))
		take_item((e->cond >> 4) & 0xff);
	return true;
}

// Item pickups (ItemCheckLookPoint 0x48b980): offered while flags n and n+1 are clear, picking up
// sets n+1. Same 400 / 30 degree look test, height window py - 750 <= y <= py + 100.
static bool items_update(const EvPlayer* p, EvAction* out)
{
	if (!p->action)
		return false;
	for (u32 i = 0; i < s_nitems; i++) {
		ItemRec* it = &s_items[i];
		if (ev_flag(it->flag) || ev_flag(it->flag + 1))
			continue;
		if (it->pos[1] < p->y - 750.0f || it->pos[1] > p->y + 100.0f)
			continue;
		if (!look_point(p->x, p->z, p->yaw, it->pos[0], it->pos[2], 400.0f))
			continue;
		ev_flag_set(it->flag + 1, true);
		ev_give_item(it->gives_item, ev_item_get_amount(it->gives_item));
		prog_stat_item_get(it->gives_item);   // ItemGet counts the pickup (0x49e310)
		memset(out, 0, sizeof *out);
		out->kind = EV_MESSAGE;
		out->event = -1;
		out->text = msg_get(&s_common, it->system_message);
		return true;
	}
	return false;
}

bool ev_update(const EvPlayer* p, EvAction* out)
{
	int fired = -1;
	for (u32 i = 0; i < s_nev; i++) {
		EvRec* e = &s_ev[i];
		if (!e->cond && !e->rslt0)
			break;
		if (!flags_pass(e->flag))
			continue;
		int type = e->rslt0 >> 28;
		if (type == RT_PROGRAM && !prog_exists(s_stage, (e->rslt1 >> 14) & 0x7f))
			continue;           // stage programs that are not ported yet (prog.c) stay skipped
		bool edge;
		if (!trigger_pass(e, p, &edge))
			continue;
		if (edge) {
			fired = (int)i;
			if ((int)i == s_last_fired)
				break;          // still standing in it
		}
		s_last_fired = fired;
		return exec((int)i, e, out);
	}
	s_last_fired = fired;
	return items_update(p, out);
}
