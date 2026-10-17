#pragma once
// Internal interface between the program runner (prog.c) and the per-stage programs (prog_*.c).
#include "prog.h"
#include "events.h"

typedef struct Prog {
	int stage, no;
	int flag;             // event rslt1 & 0x3fff, set when the program returns nonzero
	bool flag_set;        // ev_prog_flag_set (PC 0x1f7d554): a program clears it to end without the flag
	bool locked;          // JmsEventMode(jms, 1): player control off
	int step, sub;        // ev_p_step (0x1f7d548), ev_s_step (0x1f7d544)
	float timer;          // 0x1f7d564
	int answer;           // last yes / no answer (0 yes), like the PC's MesState() after a question
	int work[4];          // per program scratch (the PC keeps these in stage globals)
	u32 down, held;       // this frame's keys
	float dt;
} Prog;

typedef int (*ProgFn)(Prog* p);

typedef struct {
	int count;            // ev_prog table length including the empty slot 0
	const ProgFn* fn;     // NULL entries are not ported
} ProgStage;

extern const ProgStage prog_town_stages[];      // stages 2..5 (prog_town.c)
extern const ProgStage prog_apart_stages[];     // stages 6..13 (prog_apart.c)
extern const ProgStage prog_town_west_stage;    // stage 14 (prog_town.c)
extern const ProgStage prog_hospital_stages[];  // stages 15..27 (prog_hospital.c)
extern const ProgStage prog_prison_stages[];    // stages 28..37 (prog_prison.c)
extern const ProgStage prog_hotel_stages[];     // stages 38..52 (prog_hotel.c)
extern const ProgStage prog_wish_stages[];      // stages 53..58 (prog_wish.c)
void prog_hotel_tick(int stage);
void prog_stagefn_tick(int stage, float dt);      // prog_stagefn.c: the stages' room and per-frame code
// prog_overlay.c: the sprites the PC draws over puzzle stills
void prog_overlay_init(const ProgHost* host);
void prog_overlay_picture(const char* pic);     // a still went up (NULL: down)
void prog_overlay_frame(int extra);             // every frame while a program runs                // prog_hotel.c: stage per-frame code
void prog_overlay_manacles(float alpha);        // the open manacles over the closed ones (0x57fd50)
void prog_wallet_insert(void);                  // prog_apart.c: the safe code as inserted values 0..3

// Coroutine style steps: PROG_STEP sets the step once when the code falls into it from above, then
// waits there until expr is true. Code between two steps runs once. Step numbers are local to a
// program; the ports keep the PC's numbers where that helps comparing with the decompile.
#define PROG_BEGIN(p) switch ((p)->step) { case 0:
#define PROG_STEP(p, n, expr) (p)->step = (n); (p)->sub = 0; /* fallthrough */ case (n): if (!(expr)) return 0;
#define PROG_JUMP(p, n) do { (p)->step = (n); (p)->sub = 0; return 0; } while (0)
#define PROG_END(p) default: break; } return 1;

// Sub steps (PC event_sub.c). Each returns 1 when finished and keeps returning 1 after that.
int p_message(Prog* p, int msg);                    // EvSubMessage 0x4a1e40: stage message
int p_message_text(Prog* p, const char* text);
int p_system_message(Prog* p, int msg);             // MesOpen(&system_mes, n) + wait
int p_question(Prog* p, int msg);                   // 0x4a1ea0 + MesState(): yes / no in p->answer
int p_choice(Prog* p, int msg, const char* const* labels, int n);   // up to four answers (msg -1: none)
int p_item_get(Prog* p, int item, int msg);         // EvSubItemGet 0x4a1fb0
int p_item_get_quiet(Prog* p, int item, int msg);   // 0x4a1f10: same without James' pick-up motion
int p_item_use(Prog* p, int item, int msg, int se); // EvSubItemUse0 0x4a2f20
int p_picture(Prog* p, const char* pic);            // EvSubPictureDisplay 0x4a21e0 + wait for a button
void p_picture_show(const char* pic);               // the same still under a puzzle: no wait
int p_picture_msg(Prog* p, const char* pic, int msg);   // 0x4a3170: picture, button, message
int p_map_get(Prog* p, const char* pic, int msg);
int p_map_mark(Prog* p, int mode);                  // 0x49c0f0 / 0x49dbd0: the map's marking animation   // EvSubMapGet 0x4a3370
int p_demo(Prog* p, int demo);                      // waits for prog_demo_request
int p_walk(Prog* p, float x, float z);              // 0x53b010 + 0x53b320: James walks to a point
int p_wait(Prog* p, float seconds);                 // timer on p->timer

void p_lock(Prog* p, bool on);                      // JmsEventMode
void p_picture_hide(void);                          // 0x4a24e0
void p_fade(int type);
void p_se(int id);
void p_place(float x, float y, float z, float yaw); // CharaSetPos(jms, pos, yaw)
void p_place_keep_yaw(float x, float y, float z);
void p_player(float pos[3], float* yaw);
void p_panel(const char* text);
void p_save_menu(void);
void p_flashlight(bool on);
void p_game_end(int ending);
void p_damage(float fraction);
float* p_damage_stats(void);                        // PC 0x1dbebb0, 0x1dbebac, gDamagePointsTaken
u8* p_flag_bytes(int pc_addr);                      // the game flag block at a PC address 0x1f7d5e0..

void p_item_get_now(int item);                      // ItemGet 0x49e310
void p_item_lose(int item);                         // ItemLose 0x49e4e0
static inline bool p_flag(int n) { return ev_flag(n); }
static inline void p_set(int n) { ev_flag_set(n, true); }
static inline void p_clear(int n) { ev_flag_set(n, false); }
int p_riddle(void);                                 // gRiddleDifficulty
int prog_action_level(void);                        // gActionDifficulty
const char* p_stage_text(int msg);
int p_rand(void);                                   // the PC's rand (0x448810)

// Puzzle state the PC keeps at 0x1f7da60..0x1f7da7f (FlagInit randomises it).
u8* p_puzzle(void);
#define PZ_SAFE_CODE 0x00     // u8[4] apartment w2f safe: dial positions 0..19
#define PZ_SAFE_PROG 0x04     // s8[4] dial progress, -1 = not reached
#define PZ_CLOCK 0x10         // u16 clock time in 1/64 minutes, 0..0xb400 (room 208)
#define PZ_WHEEL_GOAL 0x08    // u8[4] hospital box wheel lock: the symbols on the wall (0..8)
#define PZ_WHEEL 0x0c         // u8[4] the wheels as set on the box
#define PZ_BOX_CODE 0x12      // u16 hospital box dial code, four nibbles 1..9 (the carbon paper)
#define PZ_PANEL 0x14         // u8[3] delusion 3 panel lock: the three panels in order (0..8)
#define PZ_HANGMAN 0x18       // u16 labyrinth hangman: crime of corpse k is (v / 6^k) % 6
