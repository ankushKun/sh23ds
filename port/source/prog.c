#include "prog_int.h"
#include "stagemdl.h"
#include "boat.h"
#include "combat.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Program runner (PC EventExecProgram 0x48c110) and the event_sub.c helpers the stage programs use.

static ProgHost s_host;
static Prog s_p;
static ProgFn s_fn;
static bool s_running;
static u8 s_puzzle[32] __attribute__((aligned(4)));    // PC 0x1f7da60..0x1f7da7f
static u8 s_riddle = 1, s_action = 2;
// Damage counters of the ending decision (PC 0x539820 / 0x539800, reset by 0x408020): [0] damage
// Maria took (0x1dbebb0, attack ids under 0x2a), [1] the same for ids 0x2a and up (0x1dbebac),
// [2] damage James took (gDamagePointsTaken 0x1dbeba8). Saved with the program state.
static float s_damage[3];
static float s_last_hp = -1.0f;
static u8 s_chapter;             // gChapterID: 1 in Born From a Wish (saved at byte 34)
// The PC's clear data (system data 0x1dbeb82, 0x1dbeb84, 0x1dbeb85): the riddle (bits 0..3) and
// action (bits 4..7) levels a game was finished on, the endings seen and the number of clears.
// main.c keeps it in its own file, outside the saves, as the PC keeps its system data.
static u8 s_cleared, s_endings, s_clears;
// The results screen's counters (PC 0x1dbeb8a.., cleared by 0x408020): play time, walking and
// running distance, saves, items picked up, enemies shot and beaten. Saved at bytes 48..63.
static float s_time, s_walk, s_run;
static u16 s_saves, s_items, s_shot, s_beaten;
static u8 s_secret;              // gSecretItemsCollected (0x1dbeb88), saved at byte 35
static int s_last_ending;
// The boat stage (gBoatStageTime 0x1dbeba0, gBoatMaxSpeed 0x1dbeba4, counted by 0x5397b0 and
// 0x5397d0 while James rows, cleared by 0x408020): seconds and m/s, saved in the save file's own
// field (prog_boat_get).
static float s_boat_time, s_boat_speed;

static void stats_reset(void)
{
	s_time = s_walk = s_run = 0.0f;
	s_saves = s_items = s_shot = s_beaten = 0;
	s_secret = 0;
	s_last_ending = 0;
	s_boat_time = s_boat_speed = 0.0f;
}
static u32 s_seed = 1;

static const char* const s_demo_names[] = {
	"", "toilet.sfd + first_toilet", "tenbou", "mar_isho + end_mria.sfd + credits.sfd", "hakaba.sfd + hakaba", "angela_grave_2",
	"angela_grave_3", "mry_yarinaoshi + ending.sfd + credits.sfd", "shadow", "saisho", "saisho_corpse", "saisho_end",
	"gero.sfd + gero_edi", "gero_edi_after", "tokei_0a1", "tokei_0a2", "tokei_0b", "tokei_0c", "ana",
	"ana_c", "dust", "no_face_corpse", "kagikeri", "sankaku1", "knife.sfd + knife_agl", "kitanai",
	"ap_boss", "tobira_a", "tobira_b", "tobira_c", "hei.sfd + laura fence", "deai.sfd + maria meeting",
	"maria_not_hotel", "bp_mar1 bowl_mae_1", "maria_bowling_2", "maria_alley", "anahori",
	"laura_hospital", "end_maria_e",
	"bp_edi_1",
	"bp_edi pizza",
	"bp_edi pizza 2",
	"to_heaven laura",
	"to_heaven maria",
	"to_heaven james",
	"hospital exit maria",
	"lau_damasu",
	"needle in the stuffed animal",
	"mar_hagureru",
	"mar_hagureru 2",
	"tsuri",
	"tenjou",
	"kanaami",
	"lau_damasu_m",
	"lau_damasu2",
	"go_to_ura a",
	"go_to_ura b",
	"ufoniwa",
	"hp_ele",
	"exit",
	"reizouko",
	"reizouko_a",
	"saikai.sfd",
	"kakushi",
	"quizu",
	"mar_dead",
	"jump1",
	"ido",
	"jump2",
	"korosu_a/b.sfd + edi_korosu",
	"jump3",
	"5f_start",
	"jump4",
	"6f_start",
	"jump5",
	"7f_start",
	"rouya.sfd + mar_ikiteruka",
	"harigane",
	"angela scream",
	"papa_agl",
	"papa_agl 2",
	"hanging rope",
	"kaiten",
	"maria redeath",
	"killer_edi",
	"killer_edi 2",
	"killer_edi_e",
	"killer_edi_e 2",
	"boat",
	"boat 2",
	"end_rbrt.sfd + credits.sfd",
	"ufomizu",
	"kanzume",
	"org_a / org_b",
	"laura piano",
	"murder.sfd",
	"ufo hotel",
	"red_ceo",
	"red_ceo 2",
	"notify death",
	"angela fire",
	"mary before battle",
	"mary fall",
	"end recovery",
	"water.sfd + end_scde.sfd + credits.sfd",
	"end_dog.sfd + credits.sfd",
	"wish start",
	"ernest 1",
	"ernest 2",
	"ernest card",
	"ernest 3",
	"ernest liquid",
	"end_wish.sfd + credits.sfd",
	"piano_lau_e",
	"org_a",
};
_Static_assert(sizeof s_demo_names / sizeof *s_demo_names == DEMO_COUNT, "one name per ProgDemo");

void prog_init(const ProgHost* host)
{
	s_host = *host;
	prog_overlay_init(host);
}

const char* prog_demo_name(int id) { return id > 0 && id < DEMO_COUNT ? s_demo_names[id] : ""; }

// The hook the engine turns into a cutscene later; without one the demo counts as finished.
bool prog_demo_request(int id) { return s_host.demo ? s_host.demo(id) : true; }

int prog_riddle_level(void) { return s_riddle; }
int p_riddle(void) { return s_riddle; }
int prog_action_level(void) { return s_action; }
u8* p_puzzle(void) { return s_puzzle; }

void prog_state_get(void* buf)
{
	u8* b = buf;
	memset(b, 0, PROG_STATE_SIZE);
	memcpy(b, s_puzzle, sizeof s_puzzle);
	b[32] = s_riddle;
	b[33] = s_action;
	b[34] = s_chapter;
	b[35] = s_secret;
	u16 st[8] = { 0, 0, (u16)(s_walk / 1000.0f), (u16)(s_run / 1000.0f), s_saves, s_items, s_shot, s_beaten };
	memcpy(st, &s_time, 4);
	memcpy(b + 48, st, 16);
	memcpy(b + 36, s_damage, sizeof s_damage);
}

void prog_state_summary(const void* buf, float* time, int* saves, int* chapter, int* action, int* riddle)
{
	const u8* b = buf;
	u16 st[8];
	memcpy(st, b + 48, 16);
	memcpy(time, st, 4);
	if (!(*time >= 0.0f && *time < 1e7f))
		*time = 0.0f;
	*saves = st[4];
	*riddle = b[32] < 4 ? b[32] : 1;
	*action = b[33] < 4 ? b[33] : 2;
	*chapter = b[34] == 1;
}

void prog_state_set(const void* buf)
{
	const u8* b = buf;
	memcpy(s_puzzle, b, sizeof s_puzzle);
	s_riddle = b[32] < 4 ? b[32] : 1;
	s_action = b[33] < 4 ? b[33] : 2;
	s_chapter = b[34] == 1;
	s_secret = b[35];
	u16 st[8];
	memcpy(st, b + 48, 16);
	memcpy(&s_time, st, 4);
	if (!(s_time >= 0.0f && s_time < 1e7f))
		s_time = 0.0f;
	s_walk = st[2] * 1000.0f, s_run = st[3] * 1000.0f;
	s_saves = st[4], s_items = st[5], s_shot = st[6], s_beaten = st[7];
	memcpy(s_damage, b + 36, sizeof s_damage);
	s_last_hp = -1.0f;
	s_running = false;
	s_boat_time = s_boat_speed = 0.0f;   // a save older than version 7 has none (prog_boat_set)
}

void prog_boat_get(float out[2]) { out[0] = s_boat_time, out[1] = s_boat_speed; }

void prog_boat_set(const float in[2])
{
	s_boat_time = in[0] >= 0.0f && in[0] < 1e7f ? in[0] : 0.0f;
	s_boat_speed = in[1] >= 0.0f && in[1] < 100.0f ? in[1] : 0.0f;
}

void prog_stat_boat(float dt, float speed)
{
	s_boat_time += dt;
	if (s_boat_speed < speed)
		s_boat_speed = speed;
}

void prog_stage_tick(int stage, float dt)
{
	if (!s_running)
		prog_hotel_tick(stage);
	prog_stagefn_tick(stage, dt);
}

void prog_debug_line(const char* line)
{
	int a, b, c, d;
	if (sscanf(line, "clock %d", &a) == 1)
		*(u16*)(s_puzzle + PZ_CLOCK) = (u16)((a % 720) << 6);
	if (sscanf(line, "box %d %d", &a, &b) == 2) {
		// hospital box: dial code (four digits 1..9) and the wall's wheel symbols (four digits 1..9)
		*(u16*)(s_puzzle + PZ_BOX_CODE) = (u16)((a / 1000 % 10) << 12 | (a / 100 % 10) << 8 | (a / 10 % 10) << 4 | a % 10);
		for (int i = 0; i < 4; i++) {
			int v = b;
			for (int k = 3 - i; k > 0; k--) v /= 10;
			s_puzzle[PZ_WHEEL_GOAL + i] = (u8)(v % 10 - 1);
			s_puzzle[PZ_WHEEL + i] = (u8)(v % 10 - 1);
		}
	}
	if (sscanf(line, "clear %d %d %d", &a, &b, &c) == 3) {   // clear data: levels, endings, clears
		const u8 v[3] = { (u8)a, (u8)b, (u8)c };
		prog_clear_set(v);
	}
	float sec;
	if (sscanf(line, "musicbox %f", &sec) == 1)    // the music box's parts as at this time of its tune
		stagemdl_music_box(sec);
	if (!strncmp(line, "boat", 4)) {   // "boat [x z yaw]": in the boat on the lake (stage 38), as after the embark scene
		float bx = -14970.1f, bz = -98254.0f, byaw = -1.5707964f;
		sscanf(line, "boat %f %f %f", &bx, &bz, &byaw);
		ev_flag_set(654, true);
		ev_flag_set(379, true);
		boat_embark_at(bx, bz, byaw);
		p_place(bx, 3592.5f, bz, byaw);
	}
	if (!strncmp(line, "wish", 4))      // "wish": Born From a Wish flags (with "stage 53" for the spawn)
		prog_new_game_wish(s_riddle, s_action);
	if (sscanf(line, "trunk %d", &a) == 1)  // hotel trunk word 0..18 (flags 406..410)
		for (int i = 0; i < 5; i++)
			ev_flag_set(406 + i, a >> i & 1);
	if (sscanf(line, "panel %d %d %d", &a, &b, &c) == 3) {   // delusion 3 panel lock: the lit panels in order, 0..8
		s_puzzle[PZ_PANEL] = (u8)a, s_puzzle[PZ_PANEL + 1] = (u8)b, s_puzzle[PZ_PANEL + 2] = (u8)c;
	}
	float fv;
	if (sscanf(line, "score %x %f", &a, &fv) == 2 && a >= 0x1f7d9c0 && a <= 0x1f7d9dc)   // an ending counter
		*(float*)p_flag_bytes(a) = fv;
	if (sscanf(line, "puzzle %x %d", &a, &b) == 2 && a >= 0 && a < (int)sizeof s_puzzle)   // 0x1f7da60 + a
		s_puzzle[a] = (u8)b;
	if (sscanf(line, "drums %d %d %d %d", &a, &b, &c, &d) == 4) {   // hotel trunk: the four drums (0..7)
		s_puzzle[0x1c] = (u8)a, s_puzzle[0x1d] = (u8)b, s_puzzle[0x1e] = (u8)c, s_puzzle[0x1f] = (u8)d;
		ev_flag_set(405, true);
	}
	if (sscanf(line, "hangman %d", &a) == 1)   // labyrinth hangman: corpse k's crime is (v / 6^k) % 6, 5 innocent
		*(u16*)(s_puzzle + PZ_HANGMAN) = (u16)a;
	if (sscanf(line, "safe %d %d %d %d", &a, &b, &c, &d) == 4) {
		s_puzzle[0] = (u8)(a - 1), s_puzzle[1] = (u8)(b - 1), s_puzzle[2] = (u8)(c - 1), s_puzzle[3] = (u8)(d - 1);
		memset(s_puzzle + PZ_SAFE_PROG, 0xff, 4);
	}
}

const char* prog_debug_status(void)
{
	static char buf[40];
	if (!s_running)
		return "-";
	snprintf(buf, sizeof buf, "s%02d #%d step %#x.%d%s", s_p.stage, s_p.no, s_p.step, s_p.sub,
	         s_p.locked ? " lock" : "");
	return buf;
}

void prog_new_game_wish(int riddle, int action)
{
	s_chapter = 1;
	s_seed = (u32)svcGetSystemTick() & 0x7fffffff;
	s_riddle = riddle < 0 ? 0 : riddle > 3 ? 3 : riddle;
	s_action = action < 0 ? 0 : action > 3 ? 3 : action;
	memset(s_puzzle, 0, sizeof s_puzzle);
	memset(s_damage, 0, sizeof s_damage);
	stats_reset();
	s_last_hp = -1.0f;
	p_set(5);                               // FlagInit: gChapterID 1 sets only 0x1000020
	p_set(24);
}

// ---------------------------------------------------------------- new game (FlagInit 0x48aed0)

// The PC's rand (0x448810): 31-bit LCG; 0x448830 scales it to 0..1.
static int rnd(void)
{
	s_seed = (s_seed * 1103515245u + 12345u) & 0x7fffffff;
	return (int)s_seed;
}

static float rndf(void) { return (float)rnd() * 4.656612873077393e-10f; }

void prog_new_game(int riddle, int action)
{
	// the clear data decides the New Game+ flags below (a first playthrough has none)
	const u8 cleared = s_cleared, endings = s_endings, clears = s_clears;
	s_chapter = 0;
	s_seed = (u32)svcGetSystemTick() & 0x7fffffff;
	memset(s_puzzle, 0, sizeof s_puzzle);
	memset(s_damage, 0, sizeof s_damage);
	stats_reset();
	s_last_hp = -1.0f;
	s_riddle = riddle < 0 ? 0 : riddle > 3 ? 3 : riddle;
	s_action = action < 0 ? 0 : action > 3 ? 3 : action;
	if (s_riddle == 2 && (cleared & 7) == 7 && (!(cleared & 8) || !(clears & 1)))
		s_riddle = 3;
	int n = (endings & 1) + (endings >> 1 & 1) + (endings >> 2 & 1);
	if (n == 1) {
		if (endings & 1) p_set(rnd() & 1 ? 8 : 9);
		if (endings & 2) p_set(rnd() & 1 ? 9 : 7);
		if (endings & 4) {
			if (rnd() & 1) p_set(7);
			p_set(8);
		}
	} else if (n == 2) {
		if (!(endings & 1)) p_set(7);
		if (!(endings & 2)) p_set(8);
		if (!(endings & 4)) p_set(9);
	}
	// Flag 12 offers the chainsaw at the grave (stage 4 event 20). The PC also sets it on a first
	// playthrough at beginner or easy action level (0x48b018 tests 0x1dbebf4 < 2).
	// Enhanced Edition "FixChainsawSpawn" (Patches/PreventChainsawSpawn.cpp, zlib licence) drops
	// that branch, since the chainsaw is meant to be a New Game+ reward.
	if (n > 0) {
		p_set(10);
		p_set(12);
	}
	if (n > 0 && (endings & 0x40)) p_set(723);
	if (n == 3 || (endings & 8)) p_set(11);
	if ((cleared & 0x80) || ((cleared & 0x40) && clears >= 2) || clears >= 3) p_set(631);
	// clock in room 208: a random time outside 8:40..9:40 (the answer is 9:10)
	int t = rnd() % 660;
	if (t > 520) t += 60;
	*(u16*)(s_puzzle + PZ_CLOCK) = (u16)(t << 6);
	// safe code in w2f: four dial positions 0..19, never the same twice in a row
	for (int i = 0; i < 4; i++) {
		int r = rnd() % 20;
		s_puzzle[i] = (u8)r;
		if (i > 0 && s_puzzle[i - 1] == r) s_puzzle[i] = (u8)((r + 10) % 20);
	}
	int dial = (rnd() % 19 + s_puzzle[0]) % 20;      // the dial starts off the first number
	for (int b = 0; b < 5; b++) ev_flag_set(103 + b, (dial >> b) & 1);
	memset(s_puzzle + PZ_SAFE_PROG, 0xff, 4);
	// later puzzles (hospital, hotel): kept for when those stages are ported
	u16 v72 = 0;
	for (int i = 0; i < 4; i++) {
		v72 = (u16)(rnd() % 9 + 1 + v72 * 16);
		s_puzzle[8 + i] = (u8)(rnd() % 9);
		s_puzzle[12 + i] = (u8)(((rnd() & 7) + 1 + s_puzzle[8 + i]) % 9);
	}
	*(u16*)(s_puzzle + 0x12) = v72;
	if (s_riddle == 0) {
		if (s_puzzle[8] == s_puzzle[9] && ++s_puzzle[9] == 9) s_puzzle[9] = 0;
		s_puzzle[10] = s_puzzle[8];
		s_puzzle[11] = s_puzzle[9];
	}
	int a = rnd() % 9, b = rnd() & 7, c = rnd() % 7;
	s_puzzle[0x14] = (u8)a;
	s_puzzle[0x15] = (u8)(b + (a <= b));
	s_puzzle[0x16] = (u8)(c + (a <= c) + (s_puzzle[0x15] <= c));
	int perm[6] = { 0, 1, 2, 3, 4, 5 };
	u16 v78 = 0;
	for (int i = 0, left = 6; left > 0; i++, left--) {
		int k = i + rnd() % left;
		v78 = (u16)(v78 * 6 + perm[k]);
		perm[k] = perm[i];
	}
	*(u16*)(s_puzzle + 0x18) = v78;
	int u = rnd() % 3 + 1;
	if (s_riddle < 2) u = 1;
	if (u & 1) { p_set(319); p_set(323); }
	if (u & 2) { p_set(320); p_set(324); }
	u = rnd() % 3 + 1;
	if (s_riddle < 2) u = 2;
	if (u & 1) { p_set(317); p_set(321); }
	if (u & 2) { p_set(318); p_set(322); }
	u = rnd() % 19;
	for (int i = 0; i < 5; i++)
		if (u >> i & 1) p_set(406 + i);
	if (s_riddle == 0) p_set(13);
	else if (s_riddle == 1) p_set(617);
	if ((rnd() & 3) == 0) p_set(543);
	if (rnd() & 1) p_set(547);
	if (rndf() < 0.7f) p_set(551);
}

// ---------------------------------------------------------------- runner

static ProgFn lookup(int stage, int no)
{
	const ProgStage* st = NULL;
	if (stage >= 2 && stage <= 5)
		st = &prog_town_stages[stage - 2];
	else if (stage >= 6 && stage <= 13)
		st = &prog_apart_stages[stage - 6];
	else if (stage == 14)
		st = &prog_town_west_stage;
	else if (stage >= 15 && stage <= 27)
		st = &prog_hospital_stages[stage - 15];
	else if (stage >= 28 && stage <= 37)
		st = &prog_prison_stages[stage - 28];
	else if (stage >= 38 && stage <= 52)
		st = &prog_hotel_stages[stage - 38];
	else if (stage >= 53 && stage <= 58)
		st = &prog_wish_stages[stage - 53];
	if (!st || no <= 0 || no >= st->count)
		return NULL;
	return st->fn[no];
}

bool prog_exists(int stage, int prog_no) { return lookup(stage, prog_no) != NULL; }

void prog_start(int stage, int prog_no, int flag)
{
	s_fn = lookup(stage, prog_no);
	s_running = s_fn != NULL;
	memset(&s_p, 0, sizeof s_p);
	s_p.stage = stage;
	s_p.no = prog_no;
	s_p.flag = flag;
	s_p.flag_set = true;
}

bool prog_running(void) { return s_running; }
bool prog_player_locked(void) { return s_running && s_p.locked; }

void prog_abort(void)
{
	s_running = false;
	p_panel(NULL);
	p_picture_hide();
}

static int s_choice_n;               // answers of the choice waiting last frame (playtest.c)
int prog_choice_waiting(void) { return s_running ? s_choice_n : 0; }

bool prog_update(u32 down, u32 held, float dt)
{
	s_choice_n = 0;
	if (!s_running)
		return false;
	s_p.down = down;
	s_p.held = held;
	s_p.dt = dt;
	if (!s_fn(&s_p)) {
		prog_overlay_frame(0);      // the sprites over the still the program shows
		return true;
	}
	// EventExecProgram: the rslt1 flag is set when the program returns nonzero
	if (s_p.flag_set && s_p.flag)
		ev_flag_set(s_p.flag, true);
	s_running = false;
	p_panel(NULL);
	p_picture_hide();
	return false;
}

// ---------------------------------------------------------------- engine services

void p_lock(Prog* p, bool on) { p->locked = on; }
void p_fade(int type) { if (s_host.fade) s_host.fade(type); }
void p_se(int id) { if (s_host.se) s_host.se(id); }
void p_panel(const char* text) { if (s_host.panel) s_host.panel(text); }
void p_picture_hide(void)
{
	if (s_host.picture)
		s_host.picture(NULL);
	prog_overlay_picture(NULL);
}
void p_save_menu(void) { if (s_host.save_menu) s_host.save_menu(); }
void p_flashlight(bool on) { if (s_host.flashlight) s_host.flashlight(on); }
void p_figure(int status, float x, float y, float z, float yaw, float delay) { if (s_host.figure) s_host.figure(status, x, y, z, yaw, delay); }
// 0x48d7e0: an ending's clear bit goes into the clear data before the game ends; the levels and the
// count only for James' endings (Born From a Wish passes 0x40).
static void record_clear(int ending)
{
	s_endings |= (u8)ending;
	if (ending == 0x40)
		return;
	s_cleared |= (u8)(s_action == 1 ? 0x20 : s_action == 2 ? 0x40 : s_action == 3 ? 0x80 : 0x10);
	s_cleared |= (u8)(1 << (s_riddle < 3 ? s_riddle : 3));
	if (++s_clears > 100)
		s_clears = 99;
}

void p_game_end(int ending)
{
	if (ending)
		record_clear(ending);
	s_last_ending = ending;
	if (s_host.game_end)
		s_host.game_end(ending);
}

void prog_clear_get(u8 out[3]) { out[0] = s_cleared, out[1] = s_endings, out[2] = s_clears; }
void prog_clear_set(const u8 in[3]) { s_cleared = in[0], s_endings = in[1], s_clears = in[2]; }
void prog_clear_merge(const u8 in[3])
{
	s_cleared |= in[0], s_endings |= in[1];
	s_clears = in[2] > s_clears ? in[2] : s_clears;
}

int prog_chapter(void) { return s_chapter; }

// FlagInit's extra riddle level (riddle 3): hard is replaced by it once Leave, Maria and In Water
// have been seen, unless the last clears were on extra already.
bool prog_extra_riddle(void)
{
	return (s_cleared & 7) == 7 && (!(s_cleared & 8) || !(s_clears & 1));
}

// Memo texts with the values the PC inserts (0x4994xx): the safe code in the wallet memo, the box
// code on the carbon paper and in the "i know it" notes.
const char* prog_memo_text(int memo, const char* text)
{
	if (!text)
		return NULL;
	if (memo == 18)
		prog_wallet_insert();
	if (memo == 29 || memo == 30) {
		u16 v = *(u16*)(s_puzzle + PZ_BOX_CODE);
		char code[16];
		snprintf(code, sizeof code, "%d%d%d%d", v >> 12 & 15, v >> 8 & 15, v >> 4 & 15, v & 15);
		ev_msg_set_insert(0, code);
	}
	return text;
}
void p_damage(float fraction) { if (s_host.damage) s_host.damage(fraction); }
float* p_damage_stats(void) { return s_damage; }

// The PC keeps these in the game flag block (0x1f7d9c0..0x1f7d9df), so they are saved with the
// flags here too: the hotel shelf's item counts and the ending score timers.
u8* p_flag_bytes(int pc_addr) { return (u8*)ev_flag_block() + (pc_addr - 0x1f7d5e0); }
#define SCORE(addr) (*(float*)p_flag_bytes(addr))

// 0x48ecb0 (EventMain): time at low health weighted by 1 - sin(pi * hp) (0x1f7d9d8), time near
// Maria (under 1000 units, 0x1f7d9cc) and far from her (over 5000, 0x1f7d9d0).
void prog_score_frame(float dt, float hp, float maria_dist)
{
	s_time += dt;                    // gInGameTime (0x5396c0)
	// James' damage (0x539800 from 0x5455be): every drop of his health in health points, healing
	// does not count
	if (s_last_hp >= 0.0f && hp < s_last_hp)
		s_damage[2] += (s_last_hp - hp) * combat_hp_max();
	s_last_hp = hp;
	if (s_running)
		return;                      // EventMain counts the rest only while no program runs
	if (hp < 0.5f)
		SCORE(0x1f7d9d8) += dt * (1.0f - sinf(hp * 3.1415927f));
	if (maria_dist >= 0.0f && maria_dist < 1000.0f)
		SCORE(0x1f7d9cc) += dt;
	if (maria_dist > 5000.0f)
		SCORE(0x1f7d9d0) += dt;
}

// The item menu (0x471590) counts looking at Mary's photo (20) and letter (21), up to two.
void prog_score_examine(int item)
{
	u8* n = p_puzzle() + 0x1a;              // 0x1f7da7a
	if ((item == 20 || item == 21) && *n < 2)
		(*n)++;
	if (item == 0x18)
		p_set(630);                         // Angela's knife: FLAGW_608 |= 0x400000 (In Water)
}

void prog_score_overheal(float part) { SCORE(0x1f7d9d4) += part; }

void prog_stat_move(float dist, bool running)
{
	if (dist < 200.0f) {             // 0x5396e0: not the jumps of a warp
		if (running) s_run += dist;
		else s_walk += dist;
	}
}

void prog_stat_save(void) { if (s_saves < 0xffff) s_saves++; }
void prog_stat_item(void) { if (s_items < 0xffff) s_items++; }

void prog_debug_item_get(int item) { p_item_get_now(item); }

void prog_stat_item_get(int item)
{
	static const struct { u8 item, bit; } secret[] = {
		{ 0x10, 1 }, { 0x0c, 2 }, { 0x19, 4 }, { 0x4a, 8 }, { 0x4b, 0x10 }, { 0x4c, 0x20 }, { 0x4d, 0x40 }, { 0x4e, 0x80 },
	};
	for (u32 i = 0; i < sizeof secret / sizeof *secret; i++)
		if (item == secret[i].item) {
			s_secret |= secret[i].bit;
			return;
		}
	if (item != 0x14 && item != 0x15)
		prog_stat_item();
}

// 0x539760: attack ids 1, 2, 4, 6, 0x19 and 0x1a are the guns.
void prog_stat_kill(int attack_id)
{
	bool gun = attack_id == 1 || attack_id == 2 || attack_id == 4 || attack_id == 6 || attack_id == 0x19 ||
	           attack_id == 0x1a;
	if (gun && s_shot < 0xffff) s_shot++;
	if (!gun && s_beaten < 0xffff) s_beaten++;
}

// EventMain (0x48ecb0): James running into Maria (0x52cf70) counts into 0x1f7da7b, up to 100; the
// ending decision takes a point from her past 30 and gives one under 3.
void prog_score_bump(void)
{
	u8* n = p_puzzle() + 0x1b;
	if (*n < 100)
		(*n)++;
}

// 0x539820 from Maria's damage code: enemies' blows (attack ids over 0x29) into 0x1dbebac.
void prog_score_maria_hurt(float d) { s_damage[1] += d; }

// The results screen after an ending (0x447650) and its rank (0x539bf0).
void prog_results(ProgResults* r)
{
	memset(r, 0, sizeof *r);
	r->action = s_action, r->riddle = s_riddle, r->chapter = s_chapter;
	r->ending = s_last_ending, r->endings = s_endings, r->saves = s_saves, r->time = s_time;
	r->walk_km = s_walk * 0.002f * 0.001f, r->run_km = s_run * 0.002f * 0.001f;
	r->items = s_items, r->shot = s_shot, r->beaten = s_beaten, r->damage = s_damage[2];
	r->boat_time = s_boat_time, r->boat_speed = s_boat_speed;
	int ends = 0;
	for (int b = 0; b < 6; b++)
		ends += s_endings >> b & 1 ? 4 : 0;
	ends = ends > 20 ? 20 : ends;
	static const int action_pts[4] = { 0, 1, 3, 5 }, riddle_pts[4] = { 1, 3, 5, 5 };
	int shot = s_shot > 75 ? 15 : s_shot / 5, beaten = s_beaten > 75 ? 15 : s_beaten / 5;
	float d = s_damage[2];
	int dmg = d <= 500.0f ? 5 : d <= 1000.0f ? 4 : d <= 1500.0f ? 3 : d <= 2000.0f ? 2 : d <= 2500.0f ? 1 : 0;
	float boat = r->boat_time;       // 0x539af0
	int boat_pts = boat <= 80.0f ? 5 : boat <= 160.0f ? 4 : boat <= 240.0f ? 3 : boat <= 480.0f ? 2 : boat <= 960.0f ? 1 : 0;
	float min = s_time / 60.0f;
	int time = min <= 180.0f ? 10 : min <= 240.0f ? 5 : min <= 360.0f ? 3 : min <= 480.0f ? 2 : min <= 960.0f ? 1 : 0;
	int secret = 0;
	for (int b = 0; b < 8; b++)
		secret += s_secret >> b & 1;
	r->secret = secret;
	int items = s_items / 20 + secret;      // 0x539970
	items = items > 15 ? 15 : items;
	r->rank = ends + action_pts[s_action & 3] + riddle_pts[s_riddle & 3] + shot + beaten + dmg + boat_pts + time + items + 5;
	// 0x539850: the saves only colour their line, they are not part of the rank
	int saves = s_saves <= 2 ? 5 : s_saves <= 5 ? 4 : s_saves <= 10 ? 3 : s_saves <= 20 ? 2 : s_saves <= 30 ? 1 : 0;
	r->pts_action = action_pts[s_action & 3], r->pts_riddle = riddle_pts[s_riddle & 3], r->pts_ends = ends;
	r->pts_saves = saves, r->pts_time = time, r->pts_items = items, r->pts_shot = shot, r->pts_beaten = beaten;
	r->pts_boat = boat_pts, r->pts_damage = dmg;
}

void prog_score_kill(void) { prog_stat_kill(0); }   // attack id unknown: counted as beaten

void p_player(float pos[3], float* yaw)
{
	pos[0] = pos[1] = pos[2] = 0.0f;
	*yaw = 0.0f;
	if (s_host.player_get)
		s_host.player_get(pos, yaw);
}

void p_place(float x, float y, float z, float yaw)
{
	float pos[3] = { x, y, z };
	if (s_host.player_set)
		s_host.player_set(pos, yaw);
}

void p_place_keep_yaw(float x, float y, float z)
{
	float pos[3], yaw;
	p_player(pos, &yaw);
	p_place(x, y, z, yaw);
}

const char* p_stage_text(int msg) { return ev_stage_message(msg); }
int p_rand(void) { return rnd(); }

static void message(const char* text)
{
	if (s_host.message)
		s_host.message(text && *text ? text : NULL);
}

static bool message_open(void) { return s_host.message_open && s_host.message_open(); }

int p_message_text(Prog* p, const char* text)
{
	if (p->sub == 0) {
		message(text);
		p->sub = 1;
		return 0;
	}
	if (p->sub == 1 && message_open())
		return 0;
	p->sub = 2;
	return 1;
}

int p_message(Prog* p, int msg) { return p_message_text(p, ev_stage_message(msg)); }
int p_system_message(Prog* p, int msg) { return p_message_text(p, ev_system_message(msg)); }

// The PC marks questions with a yes / no code at the end of the last page (ff f6) and reads the
// answer with MesState(). Here the text is shown first and the choice follows in the panel.
int p_question(Prog* p, int msg)
{
	static const char* const yes_no[2] = { "Yes", "No" };
	return p_choice(p, msg, yes_no, 2);
}

// A choice with up to four answers after the message (the PC's code ff f9 lists them in the text;
// the programs give the labels). B picks the last one, like "No".
int p_choice(Prog* p, int msg, const char* const* labels, int n)
{
	static char panel[256];
	switch (p->sub) {
	case 0:
	case 1:
		if (msg >= 0 && !p_message(p, msg))
			return 0;
		p->answer = 0;
		p->sub = 2;
		// fallthrough
	case 2:
		s_choice_n = n;
		if (p->down & (KEY_DLEFT | KEY_DUP))
			p->answer = (p->answer + n - 1) % n;
		if (p->down & (KEY_DRIGHT | KEY_DDOWN))
			p->answer = (p->answer + 1) % n;
		if (p->down & KEY_B)
			p->answer = n - 1;
		if (p->down & (KEY_A | KEY_B)) {
			p_panel(NULL);
			p->sub = 3;
			return 1;
		}
		{
			int k = snprintf(panel, sizeof panel, "\n");
			if (n == 2)
				k += snprintf(panel + k, sizeof panel - k, "   %c %s        %c %s\n\n\n\n\n\n", p->answer ? ' ' : '>',
				              labels[0], p->answer ? '>' : ' ', labels[1]);
			else
				for (int i = 0; i < n; i++)
					k += snprintf(panel + k, sizeof panel - k, "   %c %s\n", p->answer == i ? '>' : ' ', labels[i]);
			snprintf(panel + k, sizeof panel - k, "\n               D-pad: choose   A: OK");
		}
		p_panel(panel);
		return 0;
	default:
		return 1;
	}
}

// ItemGet (0x49e310): counted items gain a box (ev_item_get_amount, which applies the Bullet Adjust
// option 0x1dbec05 to ammo), the rest set their possession bit.
void p_item_get_now(int item)
{
	prog_stat_item_get(item);
	// counted items (health, ammo) take ItemGet's amount, with the Bullet Adjust option (events.c)
	bool counted = (item >= 1 && item <= 12);
	if (!counted) {
		if (!ev_have_item(item))
			ev_give_item(item, 1);
		return;
	}
	int n = ev_item_get_amount(item);
	if (ev_item_count(item) + n > 999)
		n = 999 - ev_item_count(item);
	ev_give_item(item, n);
}

// ItemLose (0x49e4e0): counted items lose one, everything else is gone.
void p_item_lose(int item)
{
	if (item >= 86 && item <= 96) {
		ev_take_item(item);          // a combination: its parts (cases 0x56..0x60)
		return;
	}
	int n = ev_item_count(item);
	if (n <= 0)
		return;
	bool counted = item < 13 && item != 4 && item != 6 && item != 8 && item != 10 && item != 12;
	ev_give_item(item, counted ? -1 : -n);
}

// EvSubItemGet: James bends down (motion 0x65), "I got ..." with SE 0x2b21, the item is added when
// the message closes. Player control is off for the whole sequence.
int p_item_get(Prog* p, int item, int msg)
{
	switch (p->sub) {
	case 0:
		p->locked = true;
		message(ev_stage_message(msg));
		p_se(0x2b21);
		p->sub = 3;
		return 0;
	case 3:
		if (message_open())
			return 0;
		p_item_get_now(item);
		p->locked = false;
		p->sub = 0xb;
		// fallthrough
	default:
		return 1;
	}
}

int p_item_get_quiet(Prog* p, int item, int msg)
{
	switch (p->sub) {
	case 0:
		message(ev_stage_message(msg));
		p_se(0x2b21);
		p->sub = 1;
		return 0;
	case 1:
		if (message_open())
			return 0;
		p_item_get_now(item);
		p->sub = 2;
		// fallthrough
	default:
		return 1;
	}
}

// EvSubItemUse0 (kind, message, se, stereo, pos, lock): the item is taken first, then the message;
// items 0x3b..0x3d play the sound after the message instead.
int p_item_use(Prog* p, int item, int msg, int se)
{
	switch (p->sub) {
	case 0:
		p->locked = true;
		p_item_lose(item);
		message(ev_stage_message(msg));
		if (se && (item < 0x3b || item > 0x3d))
			p_se(se);
		p->sub = 1;
		return 0;
	case 1:
		if (message_open())
			return 0;
		if (se && item >= 0x3b && item <= 0x3d)
			p_se(se);
		p->locked = false;
		p->sub = 2;
		// fallthrough
	default:
		return 1;
	}
}

// EvSubPictureDisplay + the program's own "wait for OK or cancel": without picture support in the
// engine the still is skipped and the program goes on with its message.
int p_picture(Prog* p, const char* pic)
{
	switch (p->sub) {
	case 0:
		p->locked = true;
		if (!s_host.picture || !s_host.picture(pic)) {
			p->sub = 2;
			return 1;
		}
		prog_overlay_picture(pic);
		p->sub = 1;
		return 0;
	case 1:
		if (!(p->down & (KEY_A | KEY_B)))
			return 0;
		p->sub = 2;
		// fallthrough
	default:
		return 1;
	}
}

void p_picture_show(const char* pic)
{
	if (s_host.picture && s_host.picture(pic))
		prog_overlay_picture(pic);
}

// 0x4a3170: picture, a button, then the message over the picture; the picture goes when it closes.
int p_picture_msg(Prog* p, const char* pic, int msg)
{
	if (p->sub < 10) {
		if (!p_picture(p, pic))
			return 0;
		p->sub = 10;
	}
	if (p->sub == 10) {
		message(ev_stage_message(msg));
		p->sub = 11;
		return 0;
	}
	if (p->sub == 11) {
		if (message_open())
			return 0;
		p_picture_hide();
		p->locked = false;
		p->sub = 12;
	}
	return 1;
}

// 0x49c0f0 + 0x49dbd0: the map screen opens by itself and fades the new marks in.
int p_map_mark(Prog* p, int mode)
{
	if (p->sub == 2)
		return 1;
	if (!s_host.map_mark || s_host.map_mark(mode)) {
		p->sub = 2;
		return 1;
	}
	return 0;
}

int p_map_get(Prog* p, const char* pic, int msg)
{
	if (!p_picture_msg(p, pic, msg))
		return 0;
	p_se(0x2b17);
	return 1;
}

int p_demo(Prog* p, int demo)
{
	p->locked = true;
	return prog_demo_request(demo);
}

int p_walk(Prog* p, float x, float z)
{
	p->locked = true;
	return !s_host.walk_to || s_host.walk_to(x, z);
}

int p_wait(Prog* p, float seconds)
{
	p->timer += p->dt;
	return p->timer >= seconds;
}
