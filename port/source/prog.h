#pragma once
#include <3ds.h>

// Stage programs (ev_prog): the native code each PC stage overlay runs for result types 3 (program)
// and 5 (door, then program). They are ported by hand from sh2pc.exe (DC); every program names its PC
// address and its PS2 beta name. See PROGRAMS.md for the table and the open questions.
//
// The PC runs a program every frame through EventExecProgram (0x48c110) until it returns nonzero,
// then sets the event's rslt1 flag unless the program cleared ev_prog_flag_set. The runner keeps that
// rule; a program steps through ev_p_step / ev_s_step like on the PC, so it can wait for messages,
// fades, answers and cutscenes over several frames.

// Cutscenes the programs ask for. The port cannot play demos (.dds + .anm) or movies (.sfd -> Bink)
// yet, so prog_demo_request() hands the id to the engine's demo hook and the program waits until the
// hook says it finished (the default hook finishes at once). Names are the PC demo / movie files.
typedef enum {
	DEMO_NONE,
	DEMO_FIRST_TOILET,       // movie toilet.sfd + demo first_toilet (opening in the restroom)
	DEMO_TENBOU,             // observation deck: Mary's letter
	DEMO_END_MARIA,          // ending "Maria" at the observation deck (mar_isho, end_mria.sfd, credits)
	DEMO_HAKABA,             // movie hakaba.sfd + demo: Angela at the graveyard
	DEMO_ANGELA_GRAVE_2,     // talk to Angela again (graveyard)
	DEMO_ANGELA_GRAVE_3,
	DEMO_REBIRTH_FOREST,     // ending "Leave": Mary in the forest (mry_yarinaoshi, ending.sfd, credits)
	DEMO_SHADOW,             // town east: blood marks and the shadow
	DEMO_FIRST_MONSTER,      // town east: the first Lying Figure (saisho)
	DEMO_FIRST_MONSTER_CORPSE,
	DEMO_FIRST_MONSTER_END,
	DEMO_GERO_EDDIE,         // movie gero.sfd + demo: Eddie vomiting
	DEMO_GERO_EDDIE_AFTER,
	DEMO_CLOCK_OPEN,         // tokei 0a1: clock face opens with the clock key
	DEMO_CLOCK_CLOSE,        // tokei 0a2
	DEMO_CLOCK_PUSH,         // tokei 0b: the clock slides away
	DEMO_CLOCK_FAIL,         // tokei 0c: the clock does not move
	DEMO_HOLE,               // ana: the hand in the hole (clock key)
	DEMO_HOLE_EMPTY,         // ana_c
	DEMO_DUST,               // dust: the canned juice falls down the chute
	DEMO_NO_FACE_CORPSE,
	DEMO_KEY_KICK,           // kagikeri: Laura kicks the key away
	DEMO_PYRAMID_FIRST,      // sankaku1: first Pyramid Head sighting
	DEMO_KNIFE_ANGELA,       // movie knife.sfd + demo knife_agl
	DEMO_WALLET,             // kitanai: wallet in the toilet
	DEMO_APART_BOSS,         // ap_boss: Pyramid Head in the stairwell
	DEMO_TOBIRA_A,           // apartments east -> west through the fire escape
	DEMO_TOBIRA_B,
	DEMO_TOBIRA_C,           // west -> east
	DEMO_LAURA_FENCE,        // town west: movie hei.sfd, Laura climbs through the fence
	DEMO_MARIA_MEETING,      // movie deai.sfd: Maria in Rosewater Park
	DEMO_MARIA_NOT_HOTEL,
	DEMO_MARIA_BOWLING_1,    // bowl_mae_1: Maria waits outside Pete's Bowl-o-Rama
	DEMO_MARIA_BOWLING_2,
	DEMO_MARIA_ALLEY,
	DEMO_DIG,                // anahori: digging up the metal box
	DEMO_LAURA_HOSPITAL,
	DEMO_END_MARIA_TOWN,     // ending demo end_maria_e
	DEMO_EDDIE_LAURA,        // bowling: Eddie and Laura
	DEMO_EDDIE_PIZZA,        // bowling: Eddie eating pizza
	DEMO_EDDIE_PIZZA_AFTER,
	DEMO_LAURA_SIDE_ROAD,    // Laura runs down the side road
	DEMO_MARIA_UNLOCK_HEAVEN, // Maria opens Heaven's Night
	DEMO_JAMES_CANT_GO,
	DEMO_HOSPITAL_EXIT,      // Maria at the hospital exit
	DEMO_LAURA_DECEIVE_1,    // Laura in the hospital, first
	DEMO_NEEDLE_ANIMAL,
	DEMO_LOST_MARIA,         // Maria rests in the day room
	DEMO_LOST_MARIA_AFTER,
	DEMO_FISH_KEY,           // fishing the elevator key out of the drain
	DEMO_FROM_ROOF,          // James falls from the roof
	DEMO_ROOF_TRIHEAD,       // Pyramid Head on the roof
	DEMO_LAURA_DECEIVE_2,    // Laura locks James in
	DEMO_LAURA_DECEIVE_3,
	DEMO_HOSPITAL_BACK_1,    // the otherworld hospital
	DEMO_HOSPITAL_BACK_2,
	DEMO_UFO_HOSPITAL,       // DC: the UFO ending in the courtyard
	DEMO_ELEVATOR_DOWN,      // the hospital elevator after Maria
	DEMO_HOSPITAL_EXIT_LOBBY, // James leaves through the lobby
	DEMO_REFRIGERATOR,       // opening the refrigerator with Maria
	DEMO_REFRIGERATOR_ALONE,
	DEMO_MARIA_MEET_AGAIN,   // Maria in the basement
	DEMO_EMPTY_SHELF,        // moving the shelf
	DEMO_ELEVATOR_MARIA,     // the elevator ride with Maria
	DEMO_MARIA_KILLED,       // Maria is killed
	DEMO_JUMP1,              // down the hole to the third layer
	DEMO_BOTTOM_OF_WELL,
	DEMO_JUMP2,
	DEMO_EDDIE_KILL_HUMAN,   // Eddie and the corpse
	DEMO_JUMP3,
	DEMO_5F_START,
	DEMO_JUMP4,
	DEMO_6F_START,
	DEMO_JUMP5,
	DEMO_7F_START,
	DEMO_MARIA_ALIVE,        // Maria behind the bars
	DEMO_WIRE_CUTTER,
	DEMO_ANGELA_SCREAM,
	DEMO_PAPA_ATTACK,        // Angela and Abstract Daddy
	DEMO_ANGELA_ATTACK,
	DEMO_HANG_ROPE,          // a corpse drops
	DEMO_KAITEN,             // the false charge key and the handle
	DEMO_MARIA_REDEATH,
	DEMO_EDDIE_BOSS_1,       // Eddie
	DEMO_EDDIE_BOSS_2,
	DEMO_EDDIE_BOSS_3,
	DEMO_EDDIE_BOSS_4,
	DEMO_LAKE_BOAT_IN,       // the lake: James takes the boat
	DEMO_LAKE_BOAT_OUT,      // arriving at the hotel
	DEMO_END_REBIRTH,        // ending: Rebirth
	DEMO_UFO_LAKE,           // DC: the UFO at the lake
	DEMO_CAN,                // opening the can with the can opener
	DEMO_MUSIC_BOX,          // the music box plays
	DEMO_LAURA_PIANO,        // Laura's letter in the hotel
	DEMO_VIDEO,              // the videotape in room 312
	DEMO_UFO_HOTEL,          // DC: the UFO ending in room 312
	DEMO_TRIHEAD_BATTLE,     // the two Pyramid Heads
	DEMO_TRIHEAD_DEAD,
	DEMO_NOTIFY_DEATH,       // the doctor's tape
	DEMO_ANGELA_FIRE,        // Angela in the fire
	DEMO_MARY_BEFORE_BATTLE, // the last corridor
	DEMO_MARY_FALL,
	DEMO_END_LEAVE_ROOM,     // after the fight: Leave
	DEMO_END_WATER,          // ending: In Water
	DEMO_END_DOG,            // ending: Dog
	DEMO_WISH_START,         // Born From a Wish: Maria wakes up
	DEMO_WISH_ERNEST_1,
	DEMO_WISH_ERNEST_2,
	DEMO_WISH_CARD,          // Maria gives Ernest the birthday card
	DEMO_WISH_ERNEST_3,
	DEMO_WISH_LIQUID,
	DEMO_END_WISH,           // Born From a Wish: the end
	DEMO_LAURA_PIANO_END,    // Laura at the piano after her letter (piano_lau_e)
	DEMO_MUSIC_BOX_WRONG,    // the music box with the boxes in a wrong order (org_a)
	DEMO_COUNT
} ProgDemo;

// Engine services. main.c fills this in; any NULL entry falls back to a no-op stub.
typedef struct {
	void (*message)(const char* text);   // pageable message box ('\f' pages), A closes
	bool (*message_open)(void);
	void (*panel)(const char* text);     // persistent text for choices and puzzles, NULL hides it
	bool (*picture)(const char* name);   // full-screen still (PC pic file name), NULL hides; false = unsupported
	void (*fade)(int type);              // PC FadeStart types: 1/2 fade out, 3 black now, 4 fade in, 5 clear now
	bool (*fade_done)(void);
	void (*se)(int id);                  // sound effect by PC SE id
	void (*player_get)(float pos[3], float* yaw);
	void (*player_set)(const float pos[3], float yaw);   // CharaSetPos(jms): same room
	bool (*walk_to)(float x, float z);   // James walks there; true when arrived
	bool (*demo)(int id);                // play cutscene id, true when it finished
	void (*save_menu)(void);
	void (*flashlight)(bool on);
	// Ending reached (PC 0x48d7e0 with the ending's bit: 1 Leave, 2 Maria, 4 In Water, 8 Rebirth,
	// 0x10 Dog, 0x20 UFO, 0x40 Born From a Wish; 0 = the player backed out of Born From a Wish).
	// The PC records the bit in its clear data and goes to the results screen.
	void (*game_end)(int ending);
	void (*damage)(float fraction);   // hurt James by this part of his maximum health (the hotel fire)
	// Overlay sprites over a still (prog_overlay.c): the sheet is pack/sheet/<dir>/<name>.t3x (NULL
	// frees it, false when missing); sprite adds one sprite for this frame, corners top left, top
	// right, bottom left, bottom right in the PC's sprite units (-256..256 across the still, y
	// down) and texels u0 v0 u1 v1 of the sheet. sprite(NULL, NULL, 0) clears the frame's list.
	bool (*map_mark)(int mode);          // map marking animation (0x49c0f0 mode, 0x49dbd0); true when done
	bool (*sheet)(const char* name);
	void (*sprite)(const float xy[4][2], const float uv[4], float alpha);
} ProgHost;

void prog_init(const ProgHost* host);

// New game (PC FlagInit 0x48aed0): call after ev_reset(). Sets the difficulty flags and randomises
// the puzzle answers (safe code, clock start time, ...). riddle 0..3 easy..extra, action 0..3.
void prog_new_game(int riddle, int action);
// Born From a Wish (gChapterID 1): FlagInit only sets flags 5 and 24 (no puzzle answers to roll);
// the game starts in stage 53 (th09) with Maria as the player. Call after ev_reset().
void prog_new_game_wish(int riddle, int action);
int prog_riddle_level(void);
int prog_chapter(void);                // gChapterID: 0 James' game, 1 Born From a Wish

// Clear data (PC system data 0x1dbeb82 levels cleared, 0x1dbeb84 endings seen, 0x1dbeb85 clears).
// p_game_end records the ending; main.c loads and stores it in a file of its own so New Game+
// (FlagInit's flags for the extra items and endings) carries over between games.
void prog_clear_get(u8 out[3]);
void prog_clear_set(const u8 in[3]);
void prog_clear_merge(const u8 in[3]);   // a clear-data save loaded: its levels and endings added, the higher clear count
bool prog_extra_riddle(void);          // New Game on hard plays the extra riddle level
const char* prog_memo_text(int memo, const char* text);   // memo text with the codes put in

// True if the stage has a ported program with this ev_prog index.
bool prog_exists(int stage, int prog_no);
// Starts ev_prog[prog_no] of the stage; flag is the event's rslt1 & 0x3fff (0 = none).
void prog_start(int stage, int prog_no, int flag);
bool prog_running(void);
// While the program runs and has James in event mode the player does not move.
bool prog_player_locked(void);
// Runs one frame. down/held are this frame's keys (for choices and puzzles). Returns true while the
// game stays in event mode (the program has not finished).
bool prog_update(u32 down, u32 held, float dt);
void prog_abort(void);
int prog_choice_waiting(void);         // a choice is up: how many answers (0 none), for playtest.c

// Called by the program helpers; the engine hook decides what to show. True when finished.
bool prog_demo_request(int id);
const char* prog_demo_name(int id);

// Persistent state outside the game flags (randomised puzzle answers, puzzle progress, levels).
#define PROG_STATE_SIZE 64
void prog_state_get(void* buf);
void prog_state_set(const void* buf);

// Debug: "clock <minutes>", "safe <a> <b> <c> <d>" and "box <code> <wheels>" (debug.txt) set puzzle
// answers (box: the hospital box's dial code and wall symbols, wheels set to match); the status
// line names the running program for the HUD.
void prog_debug_line(const char* line);
const char* prog_debug_status(void);

// Per-frame stage code the programs depend on (the PC's Stage_Data per-frame functions): call it
// every frame. The hotel 2F weight check (flag 400, while no program runs) and the labyrinth lift
// (stage 34, flags 313 / 314).
void prog_stage_tick(int stage, float dt);

// The ending score (counters the PC keeps for 0x576660, the hotel's ending decision). Call
// prog_score_frame every game frame: hp is James' health as a part of his maximum, maria_dist the
// distance to Maria or < 0 when she is not with him. The others report single happenings.
void prog_score_frame(float dt, float hp, float maria_dist);
void prog_score_examine(int item);     // inventory examine (the photo / letter of Mary count)
void prog_score_overheal(float part);  // health items used over the maximum (0x49e7e0)
void prog_score_kill(void);
// The results screen's counters (PC 0x1dbeb8a..0x1dbebb0): moving (per frame, the distance and
// whether James runs), saves, items picked up (counted by ItemGet and the pickups) and kills by
// attack id (prog_stat_kill: guns or not, 0x539760).
void prog_stat_move(float dist, bool running);
void prog_stat_save(void);
void prog_stat_item(void);
// ItemGet's counting (0x49e310): the eight secret items (chainsaw, hyper spray, dog key, the two
// books, white chrism, obsidian goblet, blue gem) set a bit of gSecretItemsCollected (0x1dbeb88),
// Mary's photo and letter count for nothing, everything else is one more item.
void prog_stat_item_get(int item);
void prog_debug_item_get(int item);
// The memo list overlays (0x49a210): called every frame with the memo whose picture is up, or -1.
void prog_overlay_memo(int memo);           // ItemGet itself (debug.txt "itemget")
void prog_stat_kill(int attack_id);
typedef struct {
	int action, riddle, chapter, ending, endings, saves, items, secret, shot, beaten, rank;   // secret: how many of the 8
	float time, walk_km, run_km, damage, boat_time, boat_speed;
	// each category's points towards the rank (0x5398a0.. 0x539b70); the screen shows a value
	// yellow when it has its category's most points
	int pts_action, pts_riddle, pts_ends, pts_saves, pts_time, pts_items, pts_shot, pts_beaten, pts_boat, pts_damage;
} ProgResults;
void prog_results(ProgResults* r);            // an enemy killed (0x53a220)
