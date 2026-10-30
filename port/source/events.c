#include "events.h"
#include "stream.h"
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
	u16 flag, gives_item, system_message;
	u16 cond;                // bit 15 set, difficulty class bits 0..2, condition type 3..7 (0: older pack)
	char map[8];
} ItemRec;

typedef struct {
	u32 count;
	u32* offs;
	char* data;
	u32 size;
	bool tried;                  // a file loaded when first asked for: asked for once
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
static MsgFile s_msg, s_common, s_itemtext, s_memotext, s_resulttext, s_mcardtext, s_optiontext, s_pausetext;
static MsgFile* const MSG_FILES[] = { &s_msg, &s_common, &s_itemtext, &s_memotext, &s_resulttext, &s_mcardtext,
	&s_optiontext, &s_pausetext };
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

void ev_set_flag_lo(int index, u16 lo)
{
	if (index >= 0 && (u32)index < s_nev)
		s_ev[index].flag = (s_ev[index].flag & 0xffff0000u) | lo;
}
int ev_item_count(int item) { return item > 0 && item < 256 ? s_items_held[item] : 0; }

// Packs from before the item records kept the condition type: the pickups that have one, by their
// flag n (Item_List.st bits 24..20; every pickup's difficulty class is 0). Type 1: 21 in the town's
// east and 14 in its west, type 2: four boxes of rounds in ap64, type 3: hp210's shells and ampoules.
static const u16 OLD_COND1[] = { 901, 909, 911, 915, 933, 935, 943, 955, 961, 965, 973, 1209, 1211, 1221,
	1223, 1237, 1247, 1249, 1253, 1263, 1271, 1283, 1285, 1287, 1295, 1301, 1303, 1305, 1335, 1337, 1339,
	1341, 1343, 1367, 1393 };
static const u16 OLD_COND2[] = { 747, 749, 751, 753 };
static const u16 OLD_COND3[] = { 785, 787, 789, 791, 1079, 1081, 1083 };

static int old_cond_type(int flag)
{
	for (u32 i = 0; i < sizeof OLD_COND1 / sizeof *OLD_COND1; i++)
		if (OLD_COND1[i] == flag)
			return 1;
	for (u32 i = 0; i < sizeof OLD_COND2 / sizeof *OLD_COND2; i++)
		if (OLD_COND2[i] == flag)
			return 2;
	for (u32 i = 0; i < sizeof OLD_COND3 / sizeof *OLD_COND3; i++)
		if (OLD_COND3[i] == flag)
			return 3;
	return 0;
}

// EventItemConditionCheck (0x48cb20): whether a pickup is there. The room loader (0x490080) asks it
// for the pickup's model (pickup false), EventCheckLookLine (0x48e520) for the pickup itself. The
// action level leaves out classes 2, 3 and 6 on easy, 1, 3 and 5 on normal and 1, 2 and 4 on hard;
// condition type 1 needs flag 251 (the town at night), type 2's pickup flag 108 clear, and type 3
// flag 232 clear for its model and 240 for its pickup.
static bool item_there(const ItemRec* it, bool pickup)
{
	int cls = it->cond & 0x8000 ? it->cond & 7 : 0;
	int type = it->cond & 0x8000 ? it->cond >> 3 & 0x1f : old_cond_type(it->flag);
	switch (prog_action_level()) {
	case 1: if (cls == 2 || cls == 3 || cls == 6) return false; break;
	case 2: if (cls == 1 || cls == 3 || cls == 5) return false; break;
	case 3: if (cls == 1 || cls == 2 || cls == 4) return false; break;
	}
	switch (type) {
	case 1: return ev_flag(251);
	case 2: return !pickup || !ev_flag(108);
	case 3: return !ev_flag(pickup ? 240 : 232);
	default: return true;
	}
}

// Not taken yet (flags n and n + 1 clear) and there (item_there).
static bool item_lying(const ItemRec* it, bool pickup)
{
	return !ev_flag(it->flag) && !ev_flag(it->flag + 1) && item_there(it, pickup);
}

int ev_items_lying(float (*out)[3], int max)
{
	int n = 0;
	for (u32 i = 0; i < s_nitems && n < max; i++) {
		const ItemRec* it = &s_items[i];
		if (item_lying(it, false))
			memcpy(out[n++], it->pos, sizeof it->pos);
	}
	return n;
}

bool ev_pickup_place(int i, int* item, float pos[3], float* rot_y)
{
	if (i < 0 || (u32)i >= s_nitems)
		return false;
	*item = s_items[i].gives_item;
	memcpy(pos, s_items[i].pos, 3 * sizeof(float));
	*rot_y = s_items[i].rot_y;
	return true;
}

bool ev_pickup_shown(int i) { return i >= 0 && (u32)i < s_nitems && item_lying(&s_items[i], false); }

void ev_give_item(int item, int n)
{
	if (item > 0 && item < 256)
		s_items_held[item] = s_items_held[item] + n > 255 ? 255 : s_items_held[item] + n;
}

// The item screen's combinations (PC table at 0x891858, looked up by 0x49e900 with the up to three
// chosen items sorted, an unused slot 0): the result is "used" like an item (0x49ea00 runs the event
// check with it), and ItemLose of a result (0x49e4e0 cases 0x56..0x60) takes its parts away. The
// prison's horseshoe, lighter and wax doll pairs (93..95) keep their parts.
static const struct { u8 a, b, c, result; } COMBOS[] = {
	{ 0, 35, 36, 86 }, { 0, 53, 54, 87 }, { 0, 56, 57, 88 }, { 0, 59, 60, 89 }, { 0, 59, 61, 90 },
	{ 0, 60, 61, 91 }, { 59, 60, 61, 92 }, { 0, 62, 63, 93 }, { 0, 62, 64, 94 }, { 0, 63, 64, 95 },
	{ 62, 63, 64, 96 },
};

int ev_combine(int a, int b, int c)
{
	int v[3] = { a, b, c };
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2 - i; j++)
			if (v[j] > v[j + 1]) { int t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; }
	if (!v[1])
		return 0;                    // 0x49e92c: two items at least
	for (u32 i = 0; i < sizeof COMBOS / sizeof COMBOS[0]; i++)
		if (COMBOS[i].a == v[0] && COMBOS[i].b == v[1] && COMBOS[i].c == v[2])
			return COMBOS[i].result;
	return 0;
}

bool ev_combo_owned(int item)
{
	for (u32 i = 0; i < sizeof COMBOS / sizeof COMBOS[0]; i++)
		if (COMBOS[i].result == item)
			return (!COMBOS[i].a || ev_have_item(COMBOS[i].a)) && ev_have_item(COMBOS[i].b) && ev_have_item(COMBOS[i].c);
	return false;
}

static void take_item(int item)
{
	if (item >= 86 && item <= 96) {
		for (u32 i = 0; i < sizeof COMBOS / sizeof COMBOS[0]; i++)
			if (COMBOS[i].result == item && (item < 93 || item > 95)) {
				s_items_held[COMBOS[i].a] = 0;
				s_items_held[COMBOS[i].b] = 0;
				s_items_held[COMBOS[i].c] = 0;
			}
		s_items_held[0] = 0;
		return;
	}
	if (item > 0 && item < 256) s_items_held[item] = 0;
}

void ev_take_item(int item) { take_item(item); }

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
void ev_set_glb_crd(int glb) { s_glb_crd = glb; }
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

// The language (PC 0x1dbec06, set by the options' Language row, 0x4630c0): 1 English, 2 French,
// 3 German, 4 Italian, 5 Spanish. The PC loads each message file from a table indexed by it
// (0x445980: 0x7b4f50 common_msg_?.mes, 0x7b4f68 item_x, 0x7b4f80 option, 0x7b4f98 memo_x,
// 0x7b4fb0 m_card_x, 0x7b4fc8 result, 0x7b5028 the stages). The pack keeps English in events/ and
// the others in events/f, events/g, events/i and events/s (tools/sh2conv/events_pack.py). 0,
// Japanese, is only reachable through the PC's language.ini and is not packed.
static int s_lang = 1;
static const char LANG_DIR[6] = { 0, 0, 'f', 'g', 'i', 's' };
static s8 s_lang_ok[6];              // whether the pack has the language: 0 not checked, 1 yes, -1 no

void ev_msg_path(char* out, size_t size, const char* name)
{
	if (s_lang >= 2 && s_lang <= 5)
		snprintf(out, size, PACK_DIR "/events/%c/%s", LANG_DIR[s_lang], name);
	else
		snprintf(out, size, PACK_DIR "/events/%s", name);
}

static bool load_named(MsgFile* m, const char* name)
{
	char path[96];
	ev_msg_path(path, sizeof path, name);
	bool ok = msg_load(m, path);
	m->tried = true;
	return ok;
}

// A file loaded when first asked for (the results, the save screen, the options, the memos and
// the pause menu are not needed in play).
static const char* lazy(MsgFile* m, const char* name, int n)
{
	if (!m->tried)
		load_named(m, name);
	return n >= 0 ? msg_get(m, (u32)n) : NULL;
}

// The results screen's labels (result_msg_?.mes).
const char* ev_result_message(int n) { return lazy(&s_resulttext, "result.msg", n); }
// The save screen's texts (m_card_x_msg_?.mes).
const char* ev_mcard_message(int n) { return lazy(&s_mcardtext, "mcard.msg", n); }
// The options screens' texts (option_msg_?.mes).
const char* ev_option_message(int n) { return lazy(&s_optiontext, "option.msg", n); }
// The memo list's titles and texts (memo_x_msg_?.mes, written by tools/sh2conv/maps.py).
const char* ev_memo_message(int n) { return lazy(&s_memotext, "memo.msg", n); }
// The pause menu's strings, which the PC keeps in the exe by language (0x407310 picks the table:
// 0x7abd84 English, 0x7abda8 French, 0x7abdcc German, 0x7abe14 Italian, 0x7abdf0 Spanish; written
// by tools/sh2conv/ui.py): the title, the five rows, the quit question, Yes and No.
const char* ev_pause_message(int n) { return lazy(&s_pausetext, "pause.msg", n); }

int ev_language(void) { return s_lang; }

bool ev_language_available(int lang)
{
	if (lang == 1)
		return true;
	if (lang < 2 || lang > 5)
		return false;
	if (!s_lang_ok[lang]) {
		char path[96];
		snprintf(path, sizeof path, PACK_DIR "/events/%c/common.msg", LANG_DIR[lang]);
		FILE* f = fopen(path, "rb");
		s_lang_ok[lang] = f ? 1 : -1;
		if (f)
			fclose(f);
	}
	return s_lang_ok[lang] > 0;
}

// 0x4f6cc0 without a language.ini: the system's language (GetUserDefaultLangID there, the
// console's here, whose numbers 1..5 are the same five), English for any other.
int ev_system_language(void)
{
	u8 lang = CFG_LANGUAGE_EN;
	if (R_SUCCEEDED(cfguInit())) {
		if (R_FAILED(CFGU_GetSystemLanguage(&lang)))
			lang = CFG_LANGUAGE_EN;
		cfguExit();
	}
	return lang >= CFG_LANGUAGE_EN && lang <= CFG_LANGUAGE_ES ? lang : 1;
}

// Texts the game may still point at (a message on screen, an event's result) when the language
// changes: freed at the next stage load, the oldest first when there are too many.
static char* s_retired[24];
static int s_nretired;

static void free_retired(void)
{
	for (int i = 0; i < s_nretired; i++)
		free(s_retired[i]);
	s_nretired = 0;
}

int ev_set_language(int lang)
{
	if (!ev_language_available(lang))
		lang = 1;
	if (lang == s_lang)
		return lang;
	s_lang = lang;
	// 0x4630c0 reloads the option texts at once (0x445980(2)); the others follow here too, so
	// everything shown after the change is in the new language
	bool stage = s_msg.data != NULL, common = s_common.data != NULL, items = s_itemtext.data != NULL;
	for (size_t i = 0; i < sizeof MSG_FILES / sizeof *MSG_FILES; i++) {
		MsgFile* m = MSG_FILES[i];
		if (m->data) {
			if (s_nretired == (int)(sizeof s_retired / sizeof *s_retired)) {
				free(s_retired[0]);
				memmove(s_retired, s_retired + 1, sizeof s_retired - sizeof *s_retired);
				s_nretired--;
			}
			s_retired[s_nretired++] = m->data;
		}
		memset(m, 0, sizeof *m);
	}
	if (stage) {
		char name[16];
		snprintf(name, sizeof name, "s%02d.msg", s_stage);
		load_named(&s_msg, name);
	}
	if (common)
		load_named(&s_common, "common.msg");
	if (items)
		load_named(&s_itemtext, "item.msg");
	return lang;
}

const char* ev_item_message(int n) { return n >= 0 ? msg_get(&s_itemtext, (u32)n) : NULL; }
void* ev_flag_block(void) { return s_flags; }

static void* read_all(const char* path, u32* size)
{
	long pre_size = 0;
	void* pre = stream_take_file(path, &pre_size);   // read ahead for a door (main.c warp_prefetch)
	if (pre) {
		if (size)
			*size = (u32)pre_size;
		return pre;
	}
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
	free_retired();
	snprintf(path, sizeof path, "s%02d.msg", stage);
	load_named(&s_msg, path);
	if (!s_common.data)
		load_named(&s_common, "common.msg");
	if (!s_itemtext.data)
		load_named(&s_itemtext, "item.msg");
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
	// event 0), so they work without the flashlight (trigger_pass tests the bit).
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

static void fill_info(const EvRec* e, int stage, EvInfo* out);

bool ev_info(int i, EvInfo* out)
{
	if (i < 0 || (u32)i >= s_nev)
		return false;
	fill_info(&s_ev[i], s_stage, out);
	return true;
}

// Other stages' events for the playtest's door routing, read on demand and kept.
static struct { int stage; EvRec* ev; u32 n; } s_other[16];

static int other_index(int stage)
{
	for (int k = 0; k < 16; k++)
		if (s_other[k].ev && s_other[k].stage == stage)
			return k;
	int k = 0;
	while (k < 16 && s_other[k].ev)
		k++;
	if (k == 16) {
		k = stage % 16;
		free(s_other[k].ev);
		s_other[k].ev = NULL;
	}
	char path[96];
	snprintf(path, sizeof path, PACK_DIR "/events/s%02d.s3e", stage);
	u32 size;
	u8* d = read_all(path, &size);
	if (!d)
		return -1;
	u32* h = (u32*)d;
	if (size < 48 || memcmp(d, "S3E1", 4) || 48 + h[3] * sizeof(EvRec) > size) {
		free(d);
		return -1;
	}
	s_other[k].ev = malloc(h[3] * sizeof(EvRec) + 1);
	if (s_other[k].ev)
		memcpy(s_other[k].ev, d + 48, h[3] * sizeof(EvRec));
	s_other[k].n = h[3];
	s_other[k].stage = stage;
	free(d);
	return s_other[k].ev ? k : -1;
}

int ev_count_of(int stage)
{
	if (stage == s_stage)
		return (int)s_nev;
	int k = other_index(stage);
	return k < 0 ? 0 : (int)s_other[k].n;
}

bool ev_info_of(int stage, int i, EvInfo* out)
{
	if (stage == s_stage)
		return ev_info(i, out);
	int k = other_index(stage);
	if (k < 0 || i < 0 || (u32)i >= s_other[k].n)
		return false;
	fill_info(&s_other[k].ev[i], stage, out);
	return true;
}

static void fill_info(const EvRec* e, int stage, EvInfo* out)
{
	memset(out, 0, sizeof *out);
	out->kind = e->cond >> 28;
	out->item = (e->cond >> 4) & 0xff;
	out->needs_light = (e->cond & 4) != 0;
	out->shape = e->shape;
	out->npts = e->npts > 8 ? 8 : e->npts;
	out->result = e->rslt0 >> 28;
	out->prog = (e->rslt1 >> 14) & 0x7f;
	out->enabled = flags_pass(e->flag);
	out->any_height = e->any_height;
	out->y = e->y;
	memcpy(out->pts, e->pts, sizeof out->pts);
	memcpy(out->map, e->trig_map, sizeof out->map);
	out->map[7] = 0;
	memcpy(out->dest_map, e->dest_map, sizeof out->dest_map);
	out->dest_map[7] = 0;
	out->dest_stage = e->dest_stage ? e->dest_stage : stage;
	memcpy(out->dest_pos, e->dest_pos, sizeof out->dest_pos);
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
		ev_flag_set(flag, true);   // the map marks the door (and the prison's bars count the tries, 0x5847f0)
		break;
	case RT_JAMMED:
		out->kind = EV_MESSAGE;
		out->text = msg == 0xff ? msg_get(&s_common, 6) : msg == 0xfe ? msg_get(&s_common, 7) : stage_msg(e->rslt1);
		out->door_sound = (e->rslt1 >> 22) & 0x3f;
		out->door_slot = 3;
		ev_flag_set(flag, true);
		break;
	case RT_SAVE:
		// 0x48c680: system message 8 (James on the red square) only while game flag 5 (gGameFlag
		// 0x20) is clear, which it then sets: once in the whole game, not once per save point; every
		// later touch of any save point goes straight to the save prompt. The point's own flag is set
		// as well (case 7).
		out->kind = EV_SAVE;
		out->save_point = msg;
		out->text = ev_flag(5) ? NULL : msg_get(&s_common, 8);
		ev_flag_set(5, true);
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

// Item pickups (EventCheckLookLine 0x48e520, ItemCheckLookPoint 0x48b980): offered while flags n
// and n+1 are clear and item_there agrees, picking up sets n+1. Same 400 / 30 degree look test,
// height window py - 750 <= y <= py + 100. In a dark room only with the flashlight lit (0x47b1f0,
// 0x48c840: can_see), in stages 43..46, in Born From a Wish, while flag 625 is set, or when 0x507e00
// finds the item's model lit: the strongest room light it was drawn with is over 0.1
// (chrlight_lit), as the health drink at the foot of Wood Side's stairs is by the lobby's lamp.
static bool item_seen(const ItemRec* it, const EvPlayer* p)
{
	return p->can_see || (s_stage >= 43 && s_stage <= 46) || prog_chapter() || ev_flag(625) || (p->lit && p->lit(it->pos));
}

static bool item_reachable(const ItemRec* it, const EvPlayer* p)
{
	return item_lying(it, true) && it->pos[1] >= p->y - 750.0f && it->pos[1] <= p->y + 100.0f && item_seen(it, p);
}

static void take_pickup(ItemRec* it, EvAction* out)
{
	ev_flag_set(it->flag + 1, true);
	ev_give_item(it->gives_item, ev_item_get_amount(it->gives_item));
	prog_stat_item_get(it->gives_item);   // ItemGet counts the pickup (0x49e310)
	memset(out, 0, sizeof *out);
	out->kind = EV_MESSAGE;
	out->event = -1;
	out->text = msg_get(&s_common, it->system_message);
}

static bool items_update(const EvPlayer* p, EvAction* out)
{
	if (!p->action)
		return false;
	for (u32 i = 0; i < s_nitems; i++) {
		ItemRec* it = &s_items[i];
		if (item_reachable(it, p) && look_point(p->x, p->z, p->yaw, it->pos[0], it->pos[2], 400.0f)) {
			take_pickup(it, out);
			return true;
		}
	}
	return false;
}

// The reach assist, a 3DS-only system (port/STATUS.md): an Action press that the PC's own tests
// above meet with nothing takes the nearest pickup, or Action event at a point (the handgun, a key,
// a thing to look at), within REACH of James' feet and REACH_ANGLE of his facing, with nothing of
// the room between them. The PC's 400 units and 30 degrees from a point 50 ahead (0x48b980) are a
// fine aim on a 400 x 240 screen; its other tests stay: the light, the height window, the flags.
#define REACH 650.0f
#define REACH_COS 0.5f                   // 60 degrees either side

static bool reach_ok(const EvPlayer* p, float x, float z, float* d)
{
	float dx = x - p->x, dz = z - p->z;
	*d = sqrtf(dx * dx + dz * dz);
	if (*d > REACH)
		return false;
	if (*d > 1.0f && (dx * sinf(p->yaw) + dz * cosf(p->yaw)) / *d < REACH_COS)
		return false;
	return !p->clear || p->clear(x, z);
}

static bool reach_assist(const EvPlayer* p, EvAction* out)
{
	if (!p->action)
		return false;
	int best_ev = -1, best_item = -1;
	float bd = REACH + 1.0f, d;
	for (u32 i = 0; i < s_nev; i++) {
		EvRec* e = &s_ev[i];
		if (!e->cond && !e->rslt0)
			break;
		if ((e->cond >> 28) != 2 || e->shape != SH_POINT || !flags_pass(e->flag) || !height_ok(e, p->y))
			continue;
		if ((e->cond & 4) && !p->can_see)
			continue;
		if ((e->rslt0 >> 28) == RT_PROGRAM && !prog_exists(s_stage, (e->rslt1 >> 14) & 0x7f))
			continue;
		if (reach_ok(p, e->pts[0][0], e->pts[0][1], &d) && d < bd)
			bd = d, best_ev = (int)i;
	}
	for (u32 i = 0; i < s_nitems; i++) {
		ItemRec* it = &s_items[i];
		if (item_reachable(it, p) && reach_ok(p, it->pos[0], it->pos[2], &d) && d < bd)
			bd = d, best_item = (int)i, best_ev = -1;
	}
	if (best_item >= 0) {
		take_pickup(&s_items[best_item], out);
		return true;
	}
	if (best_ev >= 0) {
		s_last_fired = -1;
		return exec(best_ev, &s_ev[best_ev], out);
	}
	return false;
}

// Playtest driver: which of items_update's tests refuses pickup i to a player at p ("" if none).
const char* ev_pickup_why(int i, const EvPlayer* p)
{
	if (i < 0 || (u32)i >= s_nitems)
		return "no such pickup";
	const ItemRec* it = &s_items[i];
	if (!item_lying(it, true))
		return "not lying";
	if (it->pos[1] < p->y - 750.0f || it->pos[1] > p->y + 100.0f)
		return "height window";
	if (!look_point(p->x, p->z, p->yaw, it->pos[0], it->pos[2], 400.0f))
		return "look test";
	if (!item_seen(it, p))
		return "dark";
	for (u32 k = 0; k < i; k++)
		if (item_lying(&s_items[k], true) && s_items[k].pos[1] >= p->y - 750.0f && s_items[k].pos[1] <= p->y + 100.0f &&
		    look_point(p->x, p->z, p->yaw, s_items[k].pos[0], s_items[k].pos[2], 400.0f))
			return "an earlier pickup is taken first";
	return "";
}

// Playtest driver: the stage's item pickups (where, what, and whether it is still there).
int ev_pickup_count(void) { return (int)s_nitems; }
bool ev_pickup_info(int i, int* item, float pos[3], bool* available)
{
	if (i < 0 || (u32)i >= s_nitems)
		return false;
	const ItemRec* it = &s_items[i];
	*item = it->gives_item;
	memcpy(pos, it->pos, 3 * sizeof(float));
	*available = item_lying(it, true);
	return true;
}

// EventCheckLookLine (0x48e520) as 0x52e110 asks it, (1, 0, 1): the first event whose flags pass and
// whose trigger holds with Action pressed, the use-item kinds 5 and 6 left out and no item pickups.
// Nothing is run.
int ev_look_line(const EvPlayer* p)
{
	EvPlayer q = *p;
	q.action = true;
	q.use_item = 0;
	for (u32 i = 0; i < s_nev; i++) {
		const EvRec* e = &s_ev[i];
		if (!e->cond && !e->rslt0)
			break;
		int kind = e->cond >> 28;
		if (kind == 5 || kind == 6 || !flags_pass(e->flag))
			continue;
		if ((e->rslt0 >> 28) == RT_PROGRAM && !prog_exists(s_stage, (e->rslt1 >> 14) & 0x7f))
			continue;
		bool edge;
		if (trigger_pass(e, &q, &edge))
			return (int)i;
	}
	return -1;
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
	return items_update(p, out) || reach_assist(p, out);
}
