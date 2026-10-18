#pragma once
#include <3ds.h>

// Stage events (doors, warps, messages, flags, item pickups) from the SD pack (S3E, see
// tools/sh2conv/events_pack.py). The checks follow the PC game's EventCheck (0x48e520): events are
// tested in list order and the first one whose flags, trigger and position test pass runs.

#define EV_MAX_FLAGS 16384      // 14-bit game flag numbers

// EV_PROGRAM: a stage program was started (prog.c runs it); EV_WARP may carry one to start after the
// warp (door + program events).
typedef enum { EV_NONE, EV_WARP, EV_MESSAGE, EV_SAVE, EV_PROGRAM } EvKind;

typedef struct {
	EvKind kind;
	int event;               // index in the stage list (-1 for item pickups)
	// EV_WARP
	int stage;               // destination stage (may equal the current one)
	char map[8];             // destination room (bg map name, e.g. "ap38")
	float pos[3];
	float facing;
	bool keep_facing;
	int door_sound;          // door_se set (0 none), also on EV_MESSAGE for locked / jammed / unlock
	int door_slot;           // which sound of the set: 0 open, 2 unlock, 3 jam, 4 lock
	// EV_MESSAGE, EV_SAVE (text shown first, may be NULL)
	const char* text;        // pages separated by '\f'
	// EV_PROGRAM, EV_WARP of a door + program event: ev_prog index (0 = none)
	int program;
	int save_point;          // EV_SAVE: the save point's id (rslt1 >> 14 & 0xff, its place name)
} EvAction;

typedef struct {
	float x, y, z, yaw;      // feet position, facing (forward = sin yaw, cos yaw)
	bool action;             // action button pressed this frame
	int use_item;            // item used from the inventory this frame (trigger kinds 5, 6, 10), 0 = none
	bool can_see;            // the room is lit or the flashlight is on (for the cond "needs light" bit)
} EvPlayer;

void ev_reset(void);          // new game: all flags and items cleared
bool ev_load_stage(int stage);
int ev_stage(void);
int ev_glb_crd(void);        // building / map group of the stage (Stage_Data.glb_crd)
const char* ev_stage_name(void);
int ev_stage_for_map(const char* map);     // from rooms.txt, 0 if unknown
bool ev_map_of_stage(int stage, char out[8]);   // a map of that stage from rooms.txt

// Tests the current stage's events and item pickups. Non-button triggers fire once per entry
// (rising edge), so standing in an area does not repeat a message every frame.
bool ev_update(const EvPlayer* p, EvAction* out);
// Autotest: the current stage's door destinations (no side effects).
int ev_warp_count(void);
bool ev_warp_get(int k, EvAction* out);

// Playtest driver (playtest.c): event i of the current stage as data, to stand where it triggers.
typedef struct {
	int kind, item, shape, npts, result, prog;   // cond >> 28, cond item, shape, rslt0 >> 28, ev_prog index
	bool enabled, any_height, needs_light;      // enabled: its flag test passes now
	float y, pts[8][2];
	char map[8];                                 // the room the trigger is in ("" when the record has none)
	char dest_map[8];                            // doors: where they lead
	int dest_stage;
	float dest_pos[3];
} EvInfo;
bool ev_info(int i, EvInfo* out);
// The same for another stage's events (read from the pack on demand), for routes through doors.
int ev_count_of(int stage);
bool ev_info_of(int stage, int i, EvInfo* out);

bool ev_flag(int n);
void ev_flag_set(int n, bool v);
bool ev_have_item(int item);
int ev_item_count(int item);
// Where the stage's items still lying about are (for James' head turning to them), up to max.
int ev_items_lying(float (*out)[3], int max);
// Item combinations (PC 0x891858 / 0x49e900): the result id for up to three items (0 unused), 0 if
// they do not combine; whether James has a result's parts; ItemLose of an item or a result's parts.
int ev_combine(int a, int b, int c);
bool ev_combo_owned(int item);
void ev_take_item(int item);
void ev_give_item(int item, int n);
// ItemGet's amount for one pickup of the item (ammo boxes times the Bullet Adjust option, 1..3)
int ev_item_get_amount(int item);
void ev_set_bullet_adjust(int times);
int ev_bullet_adjust(void);
const char* ev_item_name(int item);      // NULL if unknown
// Playtest driver: the current stage's item pickups (available: not taken and offered).
int ev_pickup_count(void);
bool ev_pickup_info(int i, int* item, float pos[3], bool* available);
const char* ev_item_desc(int item);
int ev_count(void);
const char* ev_stage_message(int n);     // current stage's message n, NULL if none
const char* ev_system_message(int n);    // common_msg n
const char* ev_memo_message(int n);     // memo list message n (titles, then texts)
const char* ev_result_message(int n);   // the results screen's labels
const char* ev_mcard_message(int n);    // the save screen's texts
const char* ev_option_message(int n);   // the options screens' texts
// Inserted values: bytes 1..4 in a message stand for the PC's ff e0..ff e3, byte 5 for its question
// code ff f6. Set the values, then show the message through ev_msg_expand (main.c does).
void ev_msg_set_insert(int slot, const char* value);
const char* ev_msg_expand(const char* text);   // static buffer; returns text when it has no codes
const char* ev_item_message(int n);      // item_msg n (16: "It looks like I can't use that here.")
// The game flag words (PC gGameFlag 0x1f7d5e0); the PC keeps a few counters in the same block.
void* ev_flag_block(void);

// Flags and possessions for save files.
#define EV_STATE_SIZE (EV_MAX_FLAGS / 8 + 256)
void ev_state_get(void* buf);
void ev_state_set(const void* buf);
