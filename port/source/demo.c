#include "chrlight.h"
#include "sdio.h"
#include "demo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "bgm.h"
#include "chr.h"
#include "events.h"
#include "movie.h"
#include "prog.h"
#include "stereo.h"

// In-engine cutscenes, ported from the PC's DramaDemo player (Event/demoview.c in the PS2 beta):
// DramaDemoMain 0x4a1c80, DramaDemoInit 0x4a0b30, DdsPlay 0x4a0f30 and the readers it calls
// (0x49fab0 keys, 0x4a0000 camera, 0x4a0270 lights, 0x49fb20 characters). The script (.dds) is
// read at run time exactly like the PC does; motions are the .anm frames of the PC's per-kind
// motion tables, decoded like FUN_0054fa10 (shExec). Formats: tools/sh2conv/demos.py.

#define DEMO_MAX_CHARS 8
#define DEMO_MAX_LIGHTS 16           // piano_lau has 15 point lights and a spot
#define GPU_LIGHTS 6                 // scene lights given to the PICA per frame (8 at most)
#define DEMO_FPS 30.0f
#define FADE_SECONDS 0.35f
#define SCREEN_SY 448.0f             // PS2 frame height, the unit of scr_z (gamecam.c)
#define SCR_Z_SCALE 1.1470200f       // DdsPlay: gCameraFOV = plane * 0x6b0684

// What each ProgDemo (prog.h) plays, in the order the PC's stage program plays it. Steps are
// separated by spaces:
//   key            a scene, pack/demo/<key>.dem (the PlayInfo it came from is in demos.txt)
//   @name          a movie (movie.c), played between scenes like FUN_004a2b30 / 0x4a2bb0 do
//   !key:frames    a voice with subtitles and no script (SubtitlesExec 0x49fe30), pack/demo/<key>.voc;
//                  the next step starts after that many frames or when the voice ends, and the
//                  voice and its subtitles carry on under it
//   a|b|...        the first choice whose condition holds: "f15:key" game flag 15 set, "s48:key"
//                  current stage 48, a key without a condition always holds
// NULL: nothing in-engine; the request counts as watched and main.c plays the movies of its name.
static const char* const s_seq[DEMO_COUNT] = {
	[DEMO_FIRST_TOILET] = "@toilet first_toilet",              // 0x5a1670
	[DEMO_TENBOU] = "tenbou",                                  // 0x5a0a30
	[DEMO_END_MARIA] = "end_maria_i @end_mria @credits",       // 0x5a10e0
	[DEMO_HAKABA] = "@hakaba haka_agl",                        // 0x5a0740
	[DEMO_ANGELA_GRAVE_2] = "haka_agl_h",                      // 0x59f670
	[DEMO_ANGELA_GRAVE_3] = "haka_agl_i",                      // 0x59f790
	[DEMO_REBIRTH_FOREST] = "mry_yarinaoshi_i @ending @credits", // 0x59f8b0
	[DEMO_SHADOW] = "machi",                                   // 0x59d4f0 (stage buffer 0x1d86704)
	[DEMO_FIRST_MONSTER] = "saisho",                           // 0x59da20
	[DEMO_FIRST_MONSTER_CORPSE] = "saisho_c",                  // 0x59dbd0
	[DEMO_FIRST_MONSTER_END] = "saisho_d",                     // 0x59dd40
	[DEMO_GERO_EDDIE] = "@gero gero_edi",                      // 0x59cbf0
	[DEMO_GERO_EDDIE_AFTER] = "f87:gero_edi_h|gero_edi_i",     // 0x59cdd0: flag 87 (0x1f7d5e8 bit 23)
	[DEMO_CLOCK_OPEN] = "clock_open",                          // 0x59c4b0
	[DEMO_CLOCK_CLOSE] = "clock_close",                        // 0x59c4b0
	[DEMO_CLOCK_PUSH] = "clock_push",                          // 0x59a6b0
	[DEMO_CLOCK_FAIL] = "clock_fail",                          // 0x59a6b0
	[DEMO_HOLE] = "ana",                                       // 0x59aa30
	[DEMO_HOLE_EMPTY] = "ana_c",                               // 0x59ad80
	[DEMO_DUST] = "dust",                                      // 0x59b1e0
	[DEMO_NO_FACE_CORPSE] = "kao",                             // 0x59b300
	[DEMO_KEY_KICK] = "kagikeri",                              // 0x599ce0
	[DEMO_PYRAMID_FIRST] = "sankaku",                          // 0x5991f0
	[DEMO_KNIFE_ANGELA] = "@knife knife_agl",                  // 0x598180
	[DEMO_WALLET] = "kitanai",                                 // 0x597710
	[DEMO_APART_BOSS] = "ap_boss",                             // 0x596220
	[DEMO_TOBIRA_A] = "tobira_a",                              // 0x595d90
	[DEMO_TOBIRA_B] = "tobira_b",                              // 0x595d90
	[DEMO_TOBIRA_C] = "tobira_c",                              // 0x595f50
	[DEMO_LAURA_FENCE] = "hei_lau_a @hei hei_lau_b",           // 0x594850
	[DEMO_MARIA_MEETING] = "@deai mar_deai_c @flash mar_deai_e", // 0x5928e0 (stage buffers 0x1d865a0 / a8)
	[DEMO_MARIA_NOT_HOTEL] = "mar_deai_00",                    // 0x592c90 (stage buffer 0x1d865a8)
	[DEMO_MARIA_BOWLING_1] = "bowl_mae_1",                     // 0x592e40
	[DEMO_MARIA_BOWLING_2] = "bowl_mae_2",                     // 0x592fd0
	[DEMO_MARIA_ALLEY] = "bp_mar2_00",                         // 0x593100
	[DEMO_DIG] = "anahori",                                    // 0x593340
	[DEMO_LAURA_HOSPITAL] = "hp_lau",                          // 0x593800
	[DEMO_END_MARIA_TOWN] = "end_maria_e",                     // 0x593bb0
	[DEMO_EDDIE_LAURA] = "bp_edi_a",                           // 0x591860
	[DEMO_EDDIE_PIZZA] = "bp_edi_c",                           // 0x591a40
	[DEMO_EDDIE_PIZZA_AFTER] = "bp_edi_h",                     // 0x591c10
	[DEMO_LAURA_SIDE_ROAD] = "gekijou_a",                      // 0x590f10
	[DEMO_MARIA_UNLOCK_HEAVEN] = "gekijou_b",                  // 0x591070
	[DEMO_JAMES_CANT_GO] = "gekijou_c",                        // 0x5911c0
	[DEMO_HOSPITAL_EXIT] = "hp_exit",                          // 0x590ab0
	[DEMO_LAURA_DECEIVE_1] = "lau_damasu",                     // 0x590c20
	[DEMO_NEEDLE_ANIMAL] = "f15:teddy_b|teddy_a",              // 0x590090: Maria along (gGameFlag 0x8000)
	[DEMO_LOST_MARIA] = "hagureru",                            // 0x58d7e0
	[DEMO_LOST_MARIA_AFTER] = "f187:hagureru_c|hagureru_d",    // 0x58d990: flag 187 (0x1f7d5f4 bit 27)
	[DEMO_FISH_KEY] = "tsuri",                                 // 0x58e0b0
	[DEMO_FROM_ROOF] = "tenjou",                               // 0x58ee60
	[DEMO_ROOF_TRIHEAD] = "kanaami",                           // 0x58cca0
	[DEMO_LAURA_DECEIVE_2] = "lau_damasu_1",                   // 0x58b670
	[DEMO_LAURA_DECEIVE_3] = "lau_damasu_2",                   // 0x58b750
	[DEMO_HOSPITAL_BACK_1] = "go_to_ura_a",                    // 0x58b910 (stage buffer 0x1d8645c)
	[DEMO_HOSPITAL_BACK_2] = "go_to_ura_b",                    // 0x58ba80
	[DEMO_ELEVATOR_DOWN] = "hp_ele",                           // 0x58af50
	[DEMO_HOSPITAL_EXIT_LOBBY] = "exit",                       // 0x58b060
	[DEMO_REFRIGERATOR] = "reizouko",                          // 0x58a8a0
	[DEMO_REFRIGERATOR_ALONE] = "reizouko_jms",                // 0x58ac20
	[DEMO_MARIA_MEET_AGAIN] = "@saikai mar_saikai",            // 0x5881a0
	[DEMO_EMPTY_SHELF] = "kakushi",                            // 0x5883f0
	[DEMO_ELEVATOR_MARIA] = "!voice_588e60:5175 quiz",        // 0x588e60: the radio quiz (0x72ab54 frames), then the scene
	[DEMO_MARIA_KILLED] = "mar_dead_a mar_dead_b",             // 0x587f10
	[DEMO_JUMP1] = "jump1",                                    // 0x587a2a
	[DEMO_BOTTOM_OF_WELL] = "ido",                             // 0x586040
	[DEMO_JUMP2] = "jump2",                                    // 0x586d40
	[DEMO_EDDIE_KILL_HUMAN] = "edi_korosu_a @korosu_a edi_korosu_c @korosu_b edi_korosu_e", // 0x584fc0
	[DEMO_JUMP3] = "jump3",                                    // 0x585950
	[DEMO_5F_START] = "5f_start",                              // 0x583c60
	[DEMO_JUMP4] = "jump4",                                    // 0x583db0
	[DEMO_6F_START] = "6f_start",                              // 0x583fb0
	[DEMO_JUMP5] = "jump5",                                    // 0x5841b0
	[DEMO_7F_START] = "7f_start",                              // 0x581cc0
	[DEMO_MARIA_ALIVE] = "ikiteruka_1 @rouya ikiteruka_2",     // 0x582750
	[DEMO_WIRE_CUTTER] = "harigane",                           // 0x5829e0
	[DEMO_ANGELA_SCREAM] = "!voice_580c00:105 agl_himei",      // 0x580c00: the scream, then the scene
	[DEMO_PAPA_ATTACK] = "papa_agl_a",                         // 0x580e30
	[DEMO_ANGELA_ATTACK] = "papa_agl",                         // 0x581090
	[DEMO_HANG_ROPE] = "kubitsuri",                            // 0x57f950
	[DEMO_KAITEN] = "kaiten",                                  // 0x580054
	[DEMO_MARIA_REDEATH] = "mar_2dead",                        // 0x5801c0
	[DEMO_EDDIE_BOSS_1] = "killer_edi",                        // 0x57ef10
	[DEMO_EDDIE_BOSS_2] = "killer_edi_2",                      // 0x57f060
	[DEMO_EDDIE_BOSS_3] = "killer_edi_3",                      // 0x57f150
	[DEMO_EDDIE_BOSS_4] = "killer_edi_4",                      // 0x57f2c0
	[DEMO_LAKE_BOAT_IN] = "boat1",                             // 0x57dd20
	[DEMO_LAKE_BOAT_OUT] = "boat2",                            // 0x57de40
	[DEMO_END_REBIRTH] = "end_rebirth_d @end_rbrt @credits",   // 0x57df90
	[DEMO_CAN] = "kanzume",                                    // 0x57d570
	[DEMO_MUSIC_BOX] = "org_b",                                // 0x57c470
	[DEMO_LAURA_PIANO] = "!voice_57beb0:212 piano_lau_b",    // 0x57beb0: then her letter
	[DEMO_LAURA_PIANO_END] = "piano_lau_e",                  // 0x57beb0 step 0x18
	[DEMO_MUSIC_BOX_WRONG] = "org_a",                        // 0x57c470 with a wrong order
	[DEMO_VIDEO] = "vtr @murder tomatta",                      // 0x577fa0
	[DEMO_TRIHEAD_BATTLE] = "red_taiketsu",                    // 0x576050
	[DEMO_TRIHEAD_DEAD] = "red_ceo",                           // 0x576200
	[DEMO_NOTIFY_DEATH] = "kokuchi",                           // 0x575b80
	[DEMO_ANGELA_FIRE] = "fire_agl",                           // 0x575630
	// the four endings' stages: 48 Leave, 49 Maria, 50 In Water, 51 Rebirth
	[DEMO_MARY_BEFORE_BATTLE] = "s48:mry_yarinaoshi_a|s49:end_maria_a|s50:end_suicide_a|end_rebirth_a",
	[DEMO_MARY_FALL] = "s48:mry_drop_7b3eb8|s49:mry_drop_7b4b98|s51:mry_drop_8c9738|mry_drop",
	[DEMO_END_LEAVE_ROOM] = "mry_yarinaoshi_e",                // 0x440230
	[DEMO_END_WATER] = "end_suicide_e @water @end_scde @credits", // 0x43eee0
	[DEMO_END_DOG] = "end_inu @end_dog @credits",              // 0x574600
	// Born From a Wish (data/demo2)
	[DEMO_WISH_START] = "geki",                                // 0x5740c0
	[DEMO_WISH_ERNEST_1] = "henjee",                           // 0x571120
	[DEMO_WISH_ERNEST_2] = "no_answer",                        // 0x571220
	[DEMO_WISH_CARD] = "what_amy",                             // 0x572800
	[DEMO_WISH_ERNEST_3] = "what_amy_ato",                     // 0x572920
	[DEMO_WISH_LIQUID] = "inai",                               // 0x572a00
	[DEMO_END_WISH] = "nageru @end_wish @credits",             // 0x573a80
};


// ---------------------------------------------------------------- pack file (DEM1)

typedef struct {
	char magic[4];
	u32 version;
	s32 demo_no;
	u32 total_frames, n_chars, n_anim, n_msgs;
	s32 msg_start, voice;
	float voice_start, add_x, add_z, fov_init, fov_play;
	u32 ofs_dds, size_dds, ofs_anim, ofs_msgs, ofs_chars;
} DemHeader;

typedef struct {
	char name[16], dir[16], model[16];
	u16 kind, id;
	s16 lo, hi;
	u32 n_motions, ofs_motions;
} DemChar;

typedef struct {
	s16 id;
	u16 frames;
	s16 speed;
	u8 loop, pad;
	u32 ofs;          // u32 frame_ofs[frames + 1], then the .anm bytes
	u32 ofs_cls;      // the face clusters' track for this motion (.cls block), 0 for none
} DemMotion;

// pack/chr/<folder>/<model>.s3k: the face clusters of a model (demos.py cluster_file)
typedef struct {
	char magic[4];
	u32 n_clusters, n_verts, n_entries;
} S3KHeader;

typedef struct {
	u16 slot, pad;    // index into the morphed vertices
	float d[3];       // model-space offset at weight 1
} S3KEntry;

// A model's morph data and the bind positions of the vertices it moves.
typedef struct {
	u8* file;
	u32 n_clusters, n_verts;
	const u16* verts;
	const u32* ranges;        // first entry, count per cluster
	const S3KEntry* entries;
	float (*base)[3];
	float (*acc)[3];
} Morph;

// ---------------------------------------------------------------- run-time state

// A scene light as DdsPlayLight (0x4a0270) keeps it (0x48 bytes on the PC).
typedef struct {
	float pos[3], at[3], col[3];
	float decay[2];           // full up to [0], none from [1] (0x50cff0's ramp)
	float cone[2];            // spot half angle and its second value
	bool on;
} SceneLight;

// What DdsPlay keeps per key frame (the PC copies 0x9a words from "next" to "last" each frame).
typedef struct {
	float pos[3], at[3], roll, plane;
	float chr_pos[DEMO_MAX_CHARS][3];
	bool visible[DEMO_MAX_CHARS];
	SceneLight light[DEMO_MAX_LIGHTS];
} KeyState;

// Per-bone state of the frame decoder. The PC keeps the translation slots in the skeleton from
// frame to frame (types 3 and 4 reuse the second one), and bones without data keep their values.
typedef struct {
	s16 rot[3];
	u8 has_rot, has_trans;
	float trans[3], trans2[3];
} BoneState;

typedef struct {
	const DemChar* rec;
	Character* model;          // shared by characters with the same model (mkn_1 / mkn_2)
	Morph* morph;              // face clusters of that model, NULL for none
	bool own_model;
	const DemMotion* mot;      // motion playing, NULL: bind pose
	float mot_start;           // scene frame it started at
	int dec_frame;             // frame held in cur (-1: none decoded yet)
	BoneState prev[CHR_MAX_BONES], cur[CHR_MAX_BONES];
} DemoChar;

static struct {
	bool active, closing;
	u8* file;
	const DemHeader* h;
	const u8* p;               // script read pointer (DAT_00950740)
	int n_lights, n_chars;
	int n_point, n_spot;       // the first n_point lights are character point lights, then spots,
	                           // then parallel lights (0x47ae60, 0x47b040, 0x47b6c0 in DdsPlay)
	float light_base[DEMO_MAX_LIGHTS][2][3];
	float face_peak;           // largest cluster weight drawn last frame (debug HUD)
	int lights_used;           // scene lights given to the PICA last frame (debug HUD)
	u32 flags;                 // DAT_01f7d3a4: 2 raw values follow (a cut), 4 motions start
	int loaded;                // integer frame "next" holds (-1 before the first record)
	int rec_frame;             // frame of the next unread record, 0xffff at the end
	bool cut;                  // "next" is a cut: show "last" until it is reached
	KeyState last, next;
	float cam_base[2][3];      // last raw camera position and interest (gCutscenePos ...)
	float chr_base[DEMO_MAX_CHARS][3];
	u32 anim_index;            // DAT_00950744: next entry of adr_anim
	float cur;                 // DAT_01f7d3c0: scene frame
	float elapsed;             // 0x9509e8: frames since the start, not clamped (subtitles)
	float fade_in, fade_out;   // seconds
	bool voice_started;
	u32 msg_index;
	bool msg_shown;
	int msg_serial;
	const char* msg_text;      // msg_buf while a subtitle shows, else NULL
	char msg_buf[320];
	DemoChar chars[DEMO_MAX_CHARS];
	char key[32];
	u32 cues;                  // the s_cues already done in this scene
	char status[64];
} S;

static float rd_f32(const u8* p)
{
	float f;
	memcpy(&f, p, 4);
	return f;
}

// DdsReadFloat2 (0x49f750): a half float without the zero case.
static float dds_half(const u8* p)
{
	u32 h = p[0] | p[1] << 8;
	u32 v = (((h & 0xffff8000u) << 3) | (h & 0x3ffu) | ((h & 0x7c00u) + 0x1c000u)) << 13;
	float f;
	memcpy(&f, &v, 4);
	return f;
}

// GetSF (0x54f850): the motion data's half float, zero kept.
static float anm_half(const u8* p)
{
	u32 h = p[0] | p[1] << 8;
	if (!(h & 0x7fff))
		return 0.0f;
	u32 v = ((h & 0x8000u) << 16) | ((h & 0x3ffu) << 13) | (((h & 0x7c00u) + 0x1c000u) << 13);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

static const DemMotion* motion_list(const DemChar* c) { return (const DemMotion*)(S.file + c->ofs_motions); }

// ---------------------------------------------------------------- motions

// DramaDemoAnimationStart (0x49f7e0): every character takes the next id of adr_anim; the id's
// range says which character it is for.
static void motions_start(int key_frame)
{
	const s16* ids = (const s16*)(S.file + S.h->ofs_anim);
	for (int i = 0; i < S.n_chars; i++) {
		if (S.anim_index >= S.h->n_anim)
			return;
		s16 a = ids[S.anim_index++];
		for (int c = 0; c < S.n_chars; c++) {
			DemoChar* dc = &S.chars[c];
			if (a < dc->rec->lo || a > dc->rec->hi)
				continue;
			const DemMotion* m = motion_list(dc->rec);
			for (u32 k = 0; k < dc->rec->n_motions; k++)
				if (m[k].id == a) {
					dc->mot = &m[k];
					// the record is read one frame ahead of the clock (DdsPlay reads up to cur + 1)
					dc->mot_start = key_frame > 0 ? (float)(key_frame - 1) : 0.0f;
					dc->dec_frame = -1;
				}
			break;
		}
	}
}

static void bone_rot(BoneState* b, const u8* p)
{
	memcpy(b->rot, p, 6);
	b->has_rot = 1;
}

// Translation slot: root bones store raw floats, the others half floats (0x54faf6).
static int bone_trans(float out[3], const u8* p, bool root)
{
	if (root) {
		for (int i = 0; i < 3; i++)
			out[i] = rd_f32(p + 4 * i);
		return 12;
	}
	for (int i = 0; i < 3; i++)
		out[i] = anm_half(p + 2 * i);
	return 6;
}

// The second slot of types 4 and 6: root bones keep half floats 4 bytes apart (0x54fd44).
static int bone_trans2(float out[3], const u8* p, bool root)
{
	for (int i = 0; i < 3; i++)
		out[i] = anm_half(p + (root ? 4 : 2) * i);
	return root ? 12 : 6;
}

// One frame of a motion into the bone states, like FUN_0054fa10 (shExec) with param_2 = 0. A u32
// header holds a 4-bit code per bone for 8 bones; bit 3 is a flag, bits 0..2 the layout.
static const u8* decode_frame(BoneState* st, const u8* p, const Character* c)
{
	u32 hdr = 0;
	for (u32 b = 0; b < c->n_bones; b++) {
		if ((b & 7) == 0) {
			memcpy(&hdr, p, 4);
			p += 4;
		}
		int t = (hdr >> (4 * (b & 7))) & 7;
		BoneState* s = &st[b];
		bool root = c->parents[b] == 255;
		switch (t) {
		case 1:                      // rotation
			bone_rot(s, p);
			p += 6;
			break;
		case 2:                      // translation, rotation
			p += bone_trans(s->trans, p, root);
			s->has_trans = 1;
			bone_rot(s, p);
			p += 6;
			break;
		case 3:                      // second slot as translation; rotation and 10 more bytes
			memcpy(s->trans, s->trans2, sizeof s->trans);
			s->has_trans = 1;
			bone_rot(s, p);
			p += 16;
			break;
		case 4:                      // the same, then a new second slot
			memcpy(s->trans, s->trans2, sizeof s->trans);
			s->has_trans = 1;
			p += bone_trans2(s->trans2, p, root);
			bone_rot(s, p);
			p += 16;
			break;
		case 5:                      // translation into both slots
			p += bone_trans(s->trans, p, root);
			memcpy(s->trans2, s->trans, sizeof s->trans);
			s->has_trans = 1;
			bone_rot(s, p);
			p += 16;
			break;
		case 6:                      // both slots
			p += bone_trans(s->trans, p, root);
			p += bone_trans2(s->trans2, p, root);
			s->has_trans = 1;
			bone_rot(s, p);
			p += 16;
			break;
		default:                     // 0 and 7: no data, the bone keeps its last values
			break;
		}
	}
	return p;
}

// Bone states of frames f and f + 1 of the character's motion (decoded forward from the last call;
// a jump back starts over from frame 0, which every motion stores in full).
static void motion_frames(DemoChar* dc, int f)
{
	const DemMotion* m = dc->mot;
	int last = m->frames - 1;
	if (f > last)
		f = last;
	int want = f + 1 > last ? last : f + 1;
	const u32* ofs = (const u32*)(S.file + m->ofs);
	const u8* data = (const u8*)(ofs + m->frames + 1);
	if (dc->dec_frame < 0 || dc->dec_frame > want) {
		memset(dc->cur, 0, sizeof dc->cur);         // start over: bind pose until frame 0 is read
		dc->dec_frame = -1;
	}
	while (dc->dec_frame < want) {
		int n = dc->dec_frame + 1;
		if (n == want)
			memcpy(dc->prev, dc->cur, sizeof dc->prev);
		decode_frame(dc->cur, data + ofs[n], dc->model);
		dc->dec_frame = n;
	}
	if (want == f)                   // last frame: nothing to blend towards
		memcpy(dc->prev, dc->cur, sizeof dc->prev);
}

// Poses the character; returns the motion frame (for its face track).
static float pose_char(DemoChar* dc)
{
	static AnmKey keys[2 * CHR_MAX_BONES];
	Character* c = dc->model;
	if (!dc->mot || !dc->mot->frames) {
		chr_pose(c, NULL, 0.0f);
		return 0.0f;
	}
	const DemMotion* m = dc->mot;
	float t = (S.cur - dc->mot_start) * (float)m->speed / 2048.0f;
	if (t < 0.0f) {                              // negative speed plays from the end
		t += (float)(m->frames - 1);
		if (t < 0.0f)
			t = m->loop ? fmodf(t, (float)m->frames) + (float)m->frames : 0.0f;
	}
	if (m->loop && t >= (float)m->frames)
		t = fmodf(t, (float)m->frames);
	if (t > (float)(m->frames - 1))
		t = (float)(m->frames - 1);
	int f = (int)t;
	motion_frames(dc, f);
	for (u32 b = 0; b < c->n_bones; b++) {
		const BoneState* s[2] = { &dc->prev[b], &dc->cur[b] };
		for (int k = 0; k < 2; k++) {
			AnmKey* o = &keys[k * c->n_bones + b];
			memcpy(o->rot, s[k]->rot, sizeof o->rot);
			memcpy(o->trans, s[k]->trans, sizeof o->trans);
			o->flags = (s[k]->has_rot ? 1 : 0) | (s[k]->has_trans ? 2 : 0);
		}
	}
	Anim a = { c->n_bones, 2, keys };
	chr_pose(c, &a, t - (float)f);
	return t;
}

// ---------------------------------------------------------------- script (DdsPlay)

static void read3(float out[3], float base[3], bool raw)
{
	if (raw) {
		for (int i = 0; i < 3; i++)
			base[i] = out[i] = rd_f32(S.p + 4 * i);
		S.p += 12;
	} else {
		for (int i = 0; i < 3; i++)
			out[i] = dds_half(S.p + 2 * i) + base[i];
		S.p += 6;
	}
}

// 0x49fab0: key codes until 0x0b. 0x10 a cut (raw values follow), 0x11 the next motions.
static void dds_keys(void)
{
	for (;;) {
		u8 c = *S.p++;
		if (c == 0x0b)
			return;
		if (c == 0x10)
			S.flags |= 3;
		else if (c == 0x11)
			S.flags |= 4;
	}
}

// RotationToInterest (0x49fd30): a camera aimed by angles instead of a point. No script uses it.
static void rotation_to_interest(const float pos[3], const float rot[3], float at[3])
{
	float cx = cosf(rot[0]), sx = sinf(rot[0]), cy = cosf(rot[1]), sy = sinf(rot[1]);
	at[0] = pos[0] + sy * cx * 1000.0f;
	at[1] = pos[1] - sx * 1000.0f;
	at[2] = pos[2] + cy * cx * 1000.0f;
}

// 0x4a0000: 3 position, 4 interest, 5 angles, 6 roll, 7 plane (projection distance).
static void dds_camera(void)
{
	bool raw = S.flags & 2;
	for (;;) {
		u8 c = *S.p++;
		switch (c) {
		case 3: read3(S.next.pos, S.cam_base[0], raw); break;
		case 4: read3(S.next.at, S.cam_base[1], raw); break;
		case 5: {
			float r[3];
			for (int i = 0; i < 3; i++)
				r[i] = dds_half(S.p + 2 * i);
			S.p += 6;
			rotation_to_interest(S.next.pos, r, S.next.at);
			S.next.roll = r[2];
			break;
		}
		case 6: S.next.roll = dds_half(S.p); S.p += 2; break;
		case 7: S.next.plane = rd_f32(S.p); S.p += 4; break;
		default: return;
		}
	}
}

// 0x4a0270: lights. 1 on, 2 off, 3 position, 4 interest, 5 angles, 8 colour, 9 decay, 10 cone.
static void dds_light(int i)
{
	bool raw = S.flags & 2;
	static SceneLight spare;           // lights past DEMO_MAX_LIGHTS are read and dropped
	static float spare_base[2][3];
	SceneLight* l = i < DEMO_MAX_LIGHTS ? &S.next.light[i] : &spare;
	float (*base)[3] = i < DEMO_MAX_LIGHTS ? S.light_base[i] : spare_base;
	for (;;) {
		u8 c = *S.p++;
		switch (c) {
		case 1: l->on = true; break;
		case 2: l->on = false; break;
		case 3: read3(l->pos, base[0], raw); break;
		case 4: read3(l->at, base[1], raw); break;
		case 5: {
			float r[3];
			for (int k = 0; k < 3; k++)
				r[k] = dds_half(S.p + 2 * k);
			S.p += 6;
			rotation_to_interest(l->pos, r, l->at);
			break;
		}
		case 8:
			for (int k = 0; k < 3; k++)
				l->col[k] = dds_half(S.p + 2 * k);
			S.p += 6;
			break;
		case 9: l->decay[0] = dds_half(S.p), l->decay[1] = dds_half(S.p + 2); S.p += 4; break;
		case 10: l->cone[0] = dds_half(S.p), l->cone[1] = dds_half(S.p + 2); S.p += 4; break;
		default: return;
		}
	}
}

// 0x49fb20: 1 show, 2 hide, 3 position.
static void dds_chara(int i)
{
	for (;;) {
		u8 c = *S.p++;
		if (c == 1)
			S.next.visible[i] = true;
		else if (c == 2)
			S.next.visible[i] = false;
		else if (c == 3)
			read3(S.next.chr_pos[i], S.chr_base[i], S.flags & 2);
		else
			return;
	}
}

// One record: u16 frame, then object sections until 0xff.
static void dds_record(void)
{
	int frame = S.p[0] | S.p[1] << 8;
	u8 obj = S.p[2];
	S.p += 3;
	S.flags &= ~2u;
	while (obj != 0xff) {
		if (obj == 0)
			dds_keys();
		else if (obj == 1)
			dds_camera();
		else if (obj - 2 < S.n_lights)
			dds_light(obj - 2);
		else if (obj - 2 - S.n_lights < S.n_chars)
			dds_chara(obj - 2 - S.n_lights);
		else
			break;                       // not in any script; stop rather than read garbage
		obj = *S.p++;
	}
	if (S.flags & 2)
		S.cut = true;
	if (S.flags & 4) {
		motions_start(frame);
		S.flags &= ~4u;
	}
	S.rec_frame = S.p[0] | S.p[1] << 8;
}

// Brings "next" to integer frame f and "last" to f - 1, applying every record up to f.
static void dds_advance(int f)
{
	while (S.loaded < f) {
		S.last = S.next;
		S.cut = false;
		S.loaded++;
		while (S.rec_frame != 0xffff && S.rec_frame <= S.loaded)
			dds_record();
	}
}

// ---------------------------------------------------------------- loading

static void* read_file(const char* path, long* size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* d = *size > 0 ? malloc(*size) : NULL;
	if (d && sd_fread(d, *size, f) != (size_t)*size) {
		free(d);
		d = NULL;
	}
	fclose(f);
	return d;
}

static void morph_free(Morph* m)
{
	if (!m)
		return;
	free(m->file);
	free(m->base);
	free(m->acc);
	free(m);
}

// The model's .s3k, if it has one, and the bind positions of the vertices the clusters move.
static Morph* morph_load(const char* dir, const char* model, const Character* c)
{
	char path[128];
	snprintf(path, sizeof path, "%s/%s.s3k", dir, model);
	long size;
	u8* d = read_file(path, &size);
	if (!d)
		return NULL;
	const S3KHeader* h = (const S3KHeader*)d;
	u32 vb = (h->n_verts * 2 + 3) & ~3u;
	if (size < (long)sizeof *h || memcmp(h->magic, "S3K1", 4) ||
	    (long)(sizeof *h + vb + 8 * h->n_clusters + sizeof(S3KEntry) * h->n_entries) > size) {
		free(d);
		return NULL;
	}
	Morph* m = calloc(1, sizeof *m);
	if (!m) {
		free(d);
		return NULL;
	}
	m->file = d;
	m->n_clusters = h->n_clusters;
	m->n_verts = h->n_verts;
	m->verts = (const u16*)(d + sizeof *h);
	m->ranges = (const u32*)(d + sizeof *h + vb);
	m->entries = (const S3KEntry*)(d + sizeof *h + vb + 8 * h->n_clusters);
	m->base = malloc(sizeof *m->base * m->n_verts);
	m->acc = malloc(sizeof *m->acc * m->n_verts);
	if (!m->base || !m->acc) {
		morph_free(m);
		return NULL;
	}
	for (u32 i = 0; i < m->n_verts; i++) {
		if (m->verts[i] >= c->n_vertices) {
			morph_free(m);
			return NULL;
		}
		memcpy(m->base[i], (const u8*)c->vertices + CHR_VERTEX_SIZE * m->verts[i], sizeof m->base[i]);
	}
	return m;
}

static Character* load_model(const char* pack_dir, int upto, const DemChar* rec, bool* own, Morph** morph)
{
	*own = false;
	*morph = NULL;
	if (!rec->dir[0] || !rec->model[0])
		return NULL;
	for (int i = 0; i < upto; i++)
		if (S.chars[i].model && !strncmp(S.chars[i].rec->dir, rec->dir, 16) &&
		    !strncmp(S.chars[i].rec->model, rec->model, 16)) {
			*morph = S.chars[i].morph;
			return S.chars[i].model;
		}
	Character* c = malloc(sizeof *c);
	if (!c)
		return NULL;
	char dir[96], err[128], model[17];
	snprintf(dir, sizeof dir, "%s/chr/%.16s", pack_dir, rec->dir);
	snprintf(model, sizeof model, "%.16s", rec->model);
	if (!chr_load(c, dir, model, err, sizeof err)) {
		free(c);
		return NULL;
	}
	*own = true;
	*morph = morph_load(dir, model, c);
	return c;
}

// What the PC programs do while their scene plays (they run every frame beside DramaDemoMain; the
// port's wait for the whole request): set a flag the stage's bgm_control reads once gCutsceneTimer
// passes a frame, or the BGM page between two scenes of one program (frame -1: at the start).
typedef struct { const char* key; float frame; short flag, page; } Cue;
static const Cue s_cues[] = {
	{ "tenbou", 1400.0f, 517, -1 },      // 0x5a0a30 EvProgLetterFromMary (_DAT_006b61a8)
	{ "piano_lau_b", 243.0f, 528, -1 },  // 0x57beb0 EvProgLauraPlayPiano state 0x17 (_DAT_00715f0c)
	{ "mar_dead_b", -1.0f, 0, 0x17 },    // 0x587f10 EvProgMariaKilled state 6
	{ "mar_dead_b", 380.0f, 522, -1 },   // its state 0x18 (_DAT_00729388)
	{ "tomatta", -1.0f, 527, 0x25 },     // 0x577fa0 EvProgVideoReplay state 6
};

static void scene_cues(void)
{
	for (u32 i = 0; i < sizeof s_cues / sizeof *s_cues; i++) {
		const Cue* c = &s_cues[i];
		if ((S.cues >> i & 1) || strcmp(S.key, c->key) || (c->frame >= 0.0f && !(S.cur - 1.0f > c->frame)))
			continue;
		S.cues |= 1u << i;
		if (c->flag)
			ev_flag_set(c->flag, true);
		if (c->page >= 0)
			bgm_set_page(c->page);
	}
}

// Frees the scene (models, motions, its voice); a sequence going on is kept.
static void scene_end(void)
{
	for (int i = 0; i < DEMO_MAX_CHARS; i++)
		if (S.chars[i].own_model && S.chars[i].model) {
			morph_free(S.chars[i].morph);
			chr_free(S.chars[i].model);
			free(S.chars[i].model);
		}
	if (S.voice_started)
		audio_stop_voice();
	free(S.file);
	memset(&S, 0, sizeof S);
}

static bool scene_start(const char* pack_dir, const char* key)
{
	scene_end();
	char path[128];
	snprintf(path, sizeof path, "%s/demo/%s.dem", pack_dir, key);
	long size;
	S.file = read_file(path, &size);
	S.h = (const DemHeader*)S.file;
	if (!S.file || size < (long)sizeof(DemHeader) || memcmp(S.h->magic, "DEM1", 4) || S.h->version != 2 ||
	    S.h->n_chars > DEMO_MAX_CHARS || S.h->ofs_dds + S.h->size_dds > (u32)size) {
		scene_end();
		return false;
	}
	snprintf(S.key, sizeof S.key, "%s", key);
	const u8* dds = S.file + S.h->ofs_dds;
	S.n_point = dds[0x14];
	S.n_spot = dds[0x15];
	S.n_lights = dds[0x14] + dds[0x15] + dds[0x16];
	S.n_chars = S.h->n_chars;
	const DemChar* recs = (const DemChar*)(S.file + S.h->ofs_chars);
	for (int i = 0; i < S.n_chars; i++) {
		S.chars[i].rec = &recs[i];
		S.chars[i].model = load_model(pack_dir, i, &recs[i], &S.chars[i].own_model, &S.chars[i].morph);
		S.chars[i].dec_frame = -1;
	}
	// DramaDemoInit: names were matched by the converter; the first record is read now
	S.p = dds + 0x19 + 16 * S.n_chars;
	S.rec_frame = S.p[0] | S.p[1] << 8;
	S.loaded = -1;
	dds_advance(0);
	S.last = S.next;
	S.cut = false;
	S.active = true;
	return true;
}

// ---------------------------------------------------------------- sequences and voice tracks

// The running ProgDemo's steps and the voice of a "!" step (SubtitlesExec / SubtitlesManager,
// 0x49fe30 / 0x49fe70): it runs on its own clock under whatever plays next.
static struct {
	const char* steps;         // what is left of s_seq[id], NULL when no sequence runs
	bool skipped;
	bool waiting;              // a "!" step holds the next step until hold frames have passed
	float hold;
	char pack[48];
	// voice track
	u8* voc;
	bool voice_on, voice_started;
	bool free;                 // started by stage code while the game goes on (demo_voice_free)
	s32 voice, msg_no;
	u32 n_times, index;
	const u16* times;
	float elapsed;
	bool shown;
	const char* text;          // textbuf while a subtitle shows, else NULL
	char textbuf[320];
	int stage;                 // the stage whose messages the track reads
	int serial;
} Q;

// A subtitle is kept as a copy: the stage's message file is freed when another stage loads (a door
// taken while a voice track plays), which left the old pointer showing garbage.
static const char* subtitle_copy(char* buf, size_t size, const char* text)
{
	if (!text)
		return NULL;
	snprintf(buf, size, "%s", text);
	return buf;
}

static void track_stop(void)
{
	if (Q.voice_started && !S.voice_started)
		audio_stop_voice();
	free(Q.voc);
	Q.voc = NULL;
	Q.voice_on = Q.voice_started = false;
	Q.free = false;
	Q.text = NULL;
	Q.serial++;
}

static bool track_start(const char* key)
{
	track_stop();
	char path[160];
	snprintf(path, sizeof path, "%s/demo/%s.voc", Q.pack, key);
	long size;
	Q.voc = read_file(path, &size);
	if (!Q.voc || size < 16 || memcmp(Q.voc, "VOC1", 4)) {
		free(Q.voc);
		Q.voc = NULL;
		return false;
	}
	memcpy(&Q.voice, Q.voc + 4, 4);
	memcpy(&Q.msg_no, Q.voc + 8, 4);
	memcpy(&Q.n_times, Q.voc + 12, 4);
	if (16 + 4 * (long)Q.n_times > size)
		Q.n_times = 0;
	Q.times = (const u16*)(Q.voc + 16);
	Q.index = 0;
	Q.elapsed = 0.0f;
	Q.shown = false;
	Q.voice_on = true;
	Q.stage = ev_stage();
	if (Q.voice >= 0) {
		snprintf(path, sizeof path, "%s/sound/voice/%ld.adx", Q.pack, (long)Q.voice);
		Q.voice_started = audio_play_voice(path, 1.0f);
	}
	return true;
}

// SubtitlesManager: the clock waits for the stream, then message msg_no + i shows between the
// start and end of entry i. Over when the voice and the list are both done.
static void track_update(float dt)
{
	if (!Q.voice_on)
		return;
	bool sounding = Q.voice_started && audio_voice_playing();
	if (!Q.voice_started || audio_voice_position() >= 0.0f || !sounding)
		Q.elapsed += dt * DEMO_FPS;
	int now = (int)Q.elapsed;
	if (Q.index < Q.n_times && Q.times[2 * Q.index + 1] < now) {
		Q.index++;
		Q.shown = false;
		Q.text = NULL;
		Q.serial++;
	}
	if (Q.index < Q.n_times && Q.times[2 * Q.index] < now && !Q.shown && Q.msg_no >= 0) {
		Q.shown = true;
		Q.text = subtitle_copy(Q.textbuf, sizeof Q.textbuf, ev_stage_message(Q.msg_no + (int)Q.index));
		Q.serial++;
	}
	if (!sounding && Q.index >= Q.n_times)
		track_stop();
}

// Reads the next step into buf (NULL at the end of the sequence).
static const char* next_token(char* buf, size_t n)
{
	const char* p = Q.steps;
	while (p && *p == ' ')
		p++;
	if (!p || !*p) {
		Q.steps = NULL;
		return NULL;
	}
	size_t k = 0;
	while (p[k] && p[k] != ' ')
		k++;
	snprintf(buf, n, "%.*s", (int)k, p);
	Q.steps = p + k;
	return buf;
}

// The sequence's next step is a movie: the scene before it then hands its last picture straight to
// the movie, which fades into its own first frame (movie.c), instead of going to black first.
static bool next_is_movie(void)
{
	const char* p = Q.steps;
	while (p && *p == ' ')
		p++;
	return p && *p == '@' && !Q.skipped;
}

// "f87:gero_edi_h|gero_edi_i": the first choice whose condition holds.
static void pick(const char* tok, char* out, size_t n)
{
	const char* p = tok;
	for (;;) {
		const char* bar = strchr(p, '|');
		size_t len = bar ? (size_t)(bar - p) : strlen(p);
		const char* colon = memchr(p, ':', len);
		bool ok = true;
		if (colon && (p[0] == 'f' || p[0] == 's')) {
			int v = atoi(p + 1);
			ok = p[0] == 'f' ? ev_flag(v) : ev_stage() == v;
			len -= (size_t)(colon + 1 - p);
			p = colon + 1;
		}
		if (ok || !bar) {
			snprintf(out, n, "%.*s", (int)len, p);
			return;
		}
		p = bar + 1;
	}
}

// Runs steps until a scene or a voice hold is going; false when the sequence has ended.
static bool seq_advance(void)
{
	char tok[96], key[48];
	while (!Q.skipped && next_token(tok, sizeof tok)) {
		if (tok[0] == '@') {                      // FUN_004a2b30: a movie between the scenes
			movie_play(tok + 1);
			continue;
		}
		if (tok[0] == '!') {
			char* colon = strchr(tok, ':');
			Q.hold = colon ? (float)atof(colon + 1) : 0.0f;
			if (colon)
				*colon = '\0';
			if (track_start(tok + 1)) {
				Q.waiting = true;
				return true;
			}
			continue;
		}
		pick(tok, key, sizeof key);
		if (scene_start(Q.pack, key))
			return true;
	}
	Q.steps = NULL;
	return false;
}

void demo_end(void)
{
	scene_end();
	track_stop();
	Q.steps = NULL;
	Q.waiting = Q.skipped = false;
}

bool demo_start_key(const char* pack_dir, const char* key)
{
	if (key[0] == '#')                     // debug: "#<ProgDemo id>" plays that request's sequence
		return demo_start(pack_dir, atoi(key + 1));
	demo_end();
	snprintf(Q.pack, sizeof Q.pack, "%s", pack_dir);
	return scene_start(pack_dir, key);
}

void demo_seek(float frame)
{
	if (!S.active || frame <= 0.0f)
		return;
	S.cur = S.elapsed = frame < (float)S.h->total_frames ? frame : (float)S.h->total_frames;
	S.fade_in = FADE_SECONDS;
	dds_advance((int)S.cur + 1);
	S.voice_started = S.h->voice >= 0 && S.cur - 1.0f >= S.h->voice_start;   // no seeking in the stream
}

bool demo_has_sequence(int id)
{
	return id > 0 && id < DEMO_COUNT && s_seq[id] && strchr(s_seq[id], '@');
}

bool demo_start(const char* pack_dir, int id)
{
	if (id <= 0 || id >= DEMO_COUNT || !s_seq[id])
		return false;
	demo_end();
	snprintf(Q.pack, sizeof Q.pack, "%s", pack_dir);
	Q.steps = s_seq[id];
	return seq_advance();
}

bool demo_active(void) { return S.active || Q.waiting || (Q.voice_on && !Q.free); }

// A voice with subtitles the stage code starts while James goes on walking (the PC's per-frame
// functions call SubtitlesExec 0x49fe30 themselves: the hotel corridor's 60120). A scene that starts
// meanwhile takes over the track.
bool demo_voice_free(const char* pack_dir, const char* key)
{
	if (S.active || Q.waiting || (Q.voice_on && !Q.free))
		return false;
	snprintf(Q.pack, sizeof Q.pack, "%s", pack_dir);
	if (!track_start(key))
		return false;
	Q.free = true;
	return true;
}

bool demo_voice_free_tick(float dt)
{
	if (!Q.free || !Q.voice_on)
		return false;
	// the stage function that drives it (0x5769b0 calls SubtitlesManager 0x49fe70 every frame) is
	// gone once James leaves its stage, so the track ends there
	if (ev_stage() != Q.stage) {
		track_stop();
		return false;
	}
	track_update(dt);
	return Q.voice_on;
}
bool demo_scene_shown(void) { return S.active; }

// ---------------------------------------------------------------- per frame

static void subtitles(void)
{
	const u16* t = (const u16*)(S.file + S.h->ofs_msgs);
	int now = (int)S.elapsed;
	// SubtitlesManager in DdsPlay: close at the end time, open at the start time (elapsed frames)
	if (S.msg_index < S.h->n_msgs && t[S.msg_index * 2 + 1] < now) {
		S.msg_index++;
		S.msg_shown = false;
		S.msg_text = NULL;
		S.msg_serial++;
	}
	if (S.msg_index < S.h->n_msgs && t[S.msg_index * 2] < now && !S.msg_shown) {
		S.msg_shown = true;
		S.msg_text = subtitle_copy(S.msg_buf, sizeof S.msg_buf, ev_stage_message(S.h->msg_start + (int)S.msg_index));
		S.msg_serial++;
	}
}

static bool s_hold;
void demo_debug_hold(bool on) { s_hold = on; }

// One frame of the scene; true once it has closed.
static bool scene_update(float dt)
{
	S.fade_in += dt;
	if (s_hold) {                          // debug: the scene stays on its frame
		dds_advance((int)S.cur + 1);
		snprintf(S.status, sizeof S.status, "demo %.10s %4d hold f%.2f l%d", S.key, (int)S.cur,
		         S.face_peak, S.lights_used);
		S.face_peak = 0.0f;
		return false;
	}
	if (!S.closing) {
		// DramaDemoMain holds the scene clock while the voice stream is starting (FUN_004479f0
		// until the stream state says it plays), so the lips and subtitles keep to the voice; after
		// that the clock follows the stream (the port resynchronises when they drift apart)
		float step = dt * DEMO_FPS;
		if (S.voice_started && audio_voice_playing()) {
			float pos = audio_voice_position();
			if (pos < 0.0f) {
				step = 0.0f;
			} else {
				float want = S.h->voice_start + 1.0f + pos * DEMO_FPS;
				// never backwards: a stream behind the scene (a slow card) holds it, as the PC's
				// clock does when its stream stalls
				if (fabsf(S.cur + step - want) > 2.0f && want < (float)S.h->total_frames)
					step = want > S.cur ? want - S.cur : 0.0f;
			}
		}
		S.elapsed += step;
		if (S.cur < (float)S.h->total_frames)
			S.cur += step;
		if (S.cur > (float)S.h->total_frames)
			S.cur = (float)S.h->total_frames;
		dds_advance((int)S.cur + 1);
		scene_cues();
		subtitles();
		// DramaDemoMain: the voice stream starts once gCutsceneTimer (cur - 1) passes stream_start
		if (S.h->voice >= 0 && !S.voice_started && S.cur - 1.0f >= S.h->voice_start) {
			char path[96];
			snprintf(path, sizeof path, "%s/sound/voice/%ld.adx", Q.pack, (long)S.h->voice);
			S.voice_started = audio_play_voice(path, 1.0f);   // false: no file, the scene plays silent
			if (S.voice_started)
				Q.voice_started = false;                         // the scene's voice replaced the track's
		}
		// over when the script has run out and the voice has finished (the stage programs wait
		// for the stream, e.g. EvProgLetterFromMary)
		bool voice_left = S.voice_started && audio_voice_playing();
		if (S.cur >= (float)S.h->total_frames && !voice_left)
			S.closing = true;
		if (S.elapsed > (float)S.h->total_frames + 120.0f * DEMO_FPS)
			S.closing = true;                // safety: never hang on a stream that does not end
	}
	if (S.closing)
		S.fade_out += dt;
	snprintf(S.status, sizeof S.status, "demo %.10s %4d/%4lu %s f%.2f l%d", S.key, (int)S.cur,
	         (unsigned long)S.h->total_frames, S.voice_started ? "vo" : "", S.face_peak, S.lights_used);
	S.face_peak = 0.0f;
	return S.closing && (S.fade_out >= FADE_SECONDS || next_is_movie());
}

bool demo_update(u32 down, float dt)
{
	if (!demo_active())
		return true;
	if ((down & KEY_START) && !Q.skipped) {      // skip: the PC programs end the whole sequence
		Q.skipped = true;
		Q.waiting = false;
		track_stop();
		if (S.active) {
			S.closing = true;
			if (S.voice_started)
				audio_stop_voice();
		}
	}
	track_update(dt);
	if (Q.waiting && (!Q.voice_on || Q.elapsed >= Q.hold)) {
		Q.waiting = false;
		if (!seq_advance() && !S.active && !Q.voice_on)
			return true;
	}
	if (S.active && scene_update(dt)) {
		scene_end();
		if (!seq_advance() && !Q.waiting && !Q.voice_on)
			return true;
	}
	if (Q.skipped && !S.active)
		return true;
	return !demo_active();
}

float demo_fade(void)
{
	if (!S.active)
		return 0.0f;
	float a = 1.0f - S.fade_in / FADE_SECONDS;
	float b = next_is_movie() ? 0.0f : S.fade_out / FADE_SECONDS;
	a = a > b ? a : b;
	return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Blend factor between "last" and "next" (no blending into a cut).
static float blend(void)
{
	if (S.cut)
		return 0.0f;
	float t = S.cur - (float)(S.loaded - 1);
	return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

static bool s_reduce_fov = true;
void demo_set_reduce_fov(bool on) { s_reduce_fov = on; }

bool demo_camera(C3D_Mtx* view, float* fovy, float eye_out[3], float* focus)
{
	if (!S.active)
		return false;
	float t = blend();
	float eye[3], at[3];
	for (int i = 0; i < 3; i++) {
		eye[i] = lerp(S.last.pos[i], S.next.pos[i], t);
		at[i] = lerp(S.last.at[i], S.next.at[i], t);
	}
	eye[0] += S.h->add_x, eye[2] += S.h->add_z;
	at[0] += S.h->add_x, at[2] += S.h->add_z;
	float roll = lerp(S.last.roll, S.next.roll, t);
	float plane = lerp(S.last.plane, S.next.plane, t);
	// up is -Y in the SH2 world; the roll (vcSetEventCamParamRefView rot_z) turns it about the view axis
	float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
	float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if (fl < 1e-3f)
		f[2] = fl = 1.0f;
	for (int i = 0; i < 3; i++)
		f[i] /= fl;
	float up[3] = { 0.0f, -1.0f, 0.0f };
	float r[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
	float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	if (rl > 1e-4f) {
		for (int i = 0; i < 3; i++)
			r[i] /= rl;
		float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
		float c = cosf(roll), s = sinf(roll);
		for (int i = 0; i < 3; i++)
			up[i] = u[i] * c + r[i] * s;
	}
	Mtx_LookAt(view, FVec3_New(eye[0], eye[1], eye[2]), FVec3_New(at[0], at[1], at[2]),
	           FVec3_New(up[0], up[1], up[2]), false);
	float scale = S.h->fov_play > 0.0f ? S.h->fov_play : 1.0f;
	float scr_z = plane * SCR_Z_SCALE * scale;
	// Enhanced Edition "ReduceCutsceneFOV" (WidescreenFixesPack, bundled with the EE, on by default):
	// the scene keeps the horizontal extent of its 4:3 framing (the PC constant 0x6b0684 times
	// aspect / (4/3), at most the 16:9 factor), so characters parked just outside the 4:3 frame
	// waiting for their entrance stay hidden. The 5:3 top screen gives 1.25 and crops a little height.
	if (s_reduce_fov)
		scr_z *= (400.0f / 240.0f) / (4.0f / 3.0f);
	if (scr_z < 50.0f)
		scr_z = SCREEN_SY;
	*fovy = 2.0f * atanf(SCREEN_SY * 0.5f / scr_z);
	if (eye_out)
		memcpy(eye_out, eye, sizeof eye);
	if (focus)
		*focus = fl;
	return true;
}

int demo_number(void) { return S.active ? S.h->demo_no : -1; }
float demo_timer(void) { return S.active ? S.cur - 1.0f : 0.0f; }

static void (*s_light_hook)(float);
void demo_light_hook(void (*set_scale)(float)) { s_light_hook = set_scale; }

// Enhanced Edition "EnemyRevealLighting" (Wrappers/d3d8/IDirect3DDevice8.cpp, zlib licence): in the
// tunnel radio scene (demo 6) the Lying Figure is lit with an ambient of 0.065 instead of the
// game's 0.7 until the timer passes 705.5, so it stays a dark shape until the radio reacts.
static float char_light(const DemoChar* dc)
{
	if (S.h->demo_no == 6 && demo_timer() < 705.5f && dc->rec->kind == 0x200)
		return 0.065f / 0.7f;
	return 1.0f;
}

// ---------------------------------------------------------------- faces (.cls clusters)

static int rd_u16(const u8* p) { return p[0] | p[1] << 8; }
static int rd_s16(const u8* p) { return (s16)(p[0] | p[1] << 8); }

// Calc1 (0x506af0, clani.c): a cluster's keys {u16 frame, s16 weight 4.12} blended at frame plus a
// 12-bit fraction; the last frame blends into the first key (the track loops).
static float cluster_weight(const u8* cl, int frame, int n_frames, int counter)
{
	int n = rd_u16(cl);
	const u8* k = cl + 2;
	if (n < 1)
		return 0.0f;
	if (n < 2)
		return rd_s16(k + 2) / 4096.0f;
	int i0, i1;
	float t;
	if (frame == n_frames - 1) {
		i0 = n - 1, i1 = 0;
		t = counter / 4096.0f;
	} else {
		if (frame >= rd_u16(k + 4 * (n - 1)))
			return rd_s16(k + 4 * (n - 1) + 2) / 4096.0f;
		int lo = 0, hi = n - 1;
		while (hi - lo > 1) {
			int mid = (lo + hi) / 2;
			if (rd_u16(k + 4 * mid) <= frame)
				lo = mid;
			else
				hi = mid;
		}
		i0 = lo, i1 = lo + 1;
		int f0 = rd_u16(k + 4 * i0), f1 = rd_u16(k + 4 * i1);
		if (frame - f0 == f1 - f0 && i1 + 1 < n) {
			i0 = i1, i1 = i0 + 1;
			f0 = rd_u16(k + 4 * i0), f1 = rd_u16(k + 4 * i1);
		}
		int span = f1 > f0 ? f1 - f0 : f0 - f1;
		t = span ? ((float)(frame - f0) + counter / 4096.0f) / (float)span : 0.0f;
	}
	return (rd_s16(k + 4 * i0 + 2) * (1.0f - t) + rd_s16(k + 4 * i1 + 2) * t) / 4096.0f;
}

// ClusterAnimeExec + Model3UpdateClusters: weights of the motion's .cls track at its current frame,
// applied to the face vertices of the model (bind pose; the skinning then moves them with the head).
static void morph_char(DemoChar* dc, float motion_frame)
{
	Morph* m = dc->morph;
	if (!m)
		return;
	memset(m->acc, 0, sizeof *m->acc * m->n_verts);
	const u8* cls = dc->mot && dc->mot->ofs_cls ? S.file + dc->mot->ofs_cls : NULL;
	if (cls && rd_u16(cls + 8) == (int)m->n_clusters) {
		int n_frames = rd_u16(cls + 12);
		int f = (int)motion_frame, counter = (int)((motion_frame - (float)f) * 4096.0f);
		if (f >= n_frames) {
			if (dc->mot->loop && n_frames)
				f %= n_frames;
			else
				f = n_frames - 1, counter = 0;
		}
		for (u32 c = 0; c < m->n_clusters; c++) {
			u32 ofs;
			memcpy(&ofs, cls + 16 + 4 * c, 4);
			float w = cluster_weight(cls + ofs, f, n_frames, counter);
			if (fabsf(w) > S.face_peak)
				S.face_peak = fabsf(w);
			if (w == 0.0f)
				continue;
			const S3KEntry* e = m->entries + m->ranges[2 * c];
			for (u32 i = 0; i < m->ranges[2 * c + 1]; i++, e++)
				for (int k = 0; k < 3; k++)
					m->acc[e->slot][k] += w * e->d[k];
		}
	}
	// the vertex buffer is linear memory the GPU reads at the end of the frame
	u8* vb = dc->model->vertices;
	u32 lo = ~0u, hi = 0;
	for (u32 i = 0; i < m->n_verts; i++) {
		float* p = (float*)(vb + CHR_VERTEX_SIZE * m->verts[i]);
		for (int k = 0; k < 3; k++)
			p[k] = m->base[i][k] + m->acc[i][k];
		lo = m->verts[i] < lo ? m->verts[i] : lo;
		hi = m->verts[i] > hi ? m->verts[i] : hi;
	}
	if (hi >= lo)
		GSPGPU_FlushDataCache(vb + CHR_VERTEX_SIZE * lo, CHR_VERTEX_SIZE * (hi - lo + 1));
}

// ---------------------------------------------------------------- scene lights

// The PC lights cutscene characters with the lights DdsPlay sets: point lights for characters
// (0x47ae60, read by the character light list 0x508a90 through 0x47af00), one spot (0x47b040) and
// overrides of parallel lights 1 and 2 (0x47b6c0). The port keeps James' room light as the ambient
// and adds those as PICA fragment lights (diffuse, N.L) in a light environment of its own.
static C3D_LightEnv s_lenv;
static C3D_Light s_light[GPU_LIGHTS];
static C3D_LightLutDA s_da[GPU_LIGHTS];
static float s_da_key[GPU_LIGHTS][2];
static C3D_LightLut s_spot;
static float s_spot_key = -1.0f;
static bool s_lenv_ready;
static C3D_LightEnv* s_game_env;
static float s_amb[3] = { 1.0f, 1.0f, 1.0f }, s_bright = 1.0f;

void demo_set_lighting(C3D_LightEnv* game_env, const float ambient[3], float bright)
{
	s_game_env = game_env;
	memcpy(s_amb, ambient, sizeof s_amb);
	s_bright = bright;
}

// 0x50cff0: full up to start, falling linearly to nothing at end.
static float decay_ramp(float d, float start, float end)
{
	if (d <= start)
		return 1.0f;
	return d >= end ? 0.0f : (end - d) / (end - start);
}

static float spot_shape(float x, float half_angle)
{
	float c_out = cosf(half_angle), c_in = cosf(half_angle * 0.7f);
	float t = (x - c_out) / (c_in - c_out);
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

static float lut_one(float x, float unused)
{
	(void)x, (void)unused;
	return 1.0f;
}

static void lights_init(void)
{
	static const C3D_Material mat = {
		{ 1.0f, 1.0f, 1.0f },   // ambient
		{ 1.0f, 1.0f, 1.0f },   // diffuse
		{ 0.0f, 0.0f, 0.0f },   // specular0
		{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
	};
	static C3D_LightLut d0;
	C3D_LightEnvInit(&s_lenv);
	C3D_LightEnvMaterial(&s_lenv, &mat);
	LightLut_FromFunc(&d0, lut_one, 0.0f, false);
	C3D_LightEnvLut(&s_lenv, GPU_LUT_D0, GPU_LUTINPUT_NH, false, &d0);
	for (int i = 0; i < GPU_LIGHTS; i++) {
		C3D_LightInit(&s_light[i], &s_lenv);
		C3D_LightEnable(&s_light[i], false);
		s_da_key[i][0] = s_da_key[i][1] = -1.0f;
	}
	s_lenv_ready = true;
}

static void light_lerp(SceneLight* o, const SceneLight* a, const SceneLight* b, float t)
{
	for (int k = 0; k < 3; k++) {
		o->pos[k] = lerp(a->pos[k], b->pos[k], t);
		o->at[k] = lerp(a->at[k], b->at[k], t);
		o->col[k] = lerp(a->col[k], b->col[k], t);
	}
	for (int k = 0; k < 2; k++) {
		o->decay[k] = lerp(a->decay[k], b->decay[k], t);
		o->cone[k] = lerp(a->cone[k], b->cone[k], t);
	}
	o->on = b->on;
}

// Sets up the scene's lights for this frame; false when it has none (James' lighting is used).
static bool lights_frame(const C3D_Mtx* view, float t)
{
	int used = 0;
	if (!S.n_lights)
		return false;
	if (!s_lenv_ready)
		lights_init();
	for (int i = 0; i < S.n_lights && i < DEMO_MAX_LIGHTS && used < GPU_LIGHTS; i++) {
		SceneLight l;
		light_lerp(&l, &S.last.light[i], &S.next.light[i], t);
		if (!l.on)
			continue;
		C3D_Light* g = &s_light[used];
		float k = 0.5f * s_bright;               // the scripts' colours go up to 2
		chr_light_color(g, l.col[0] * k, l.col[1] * k, l.col[2] * k, 1.0f);
		C3D_LightEnable(g, true);
		if (i >= S.n_point + S.n_spot) {          // parallel: the direction is -position
			float n = sqrtf(l.pos[0] * l.pos[0] + l.pos[1] * l.pos[1] + l.pos[2] * l.pos[2]);
			if (n < 1e-4f)
				n = 1.0f;
			C3D_FVec d = Mtx_MultiplyFVec4(view, FVec4_New(l.pos[0] / n, l.pos[1] / n, l.pos[2] / n, 0.0f));
			C3D_LightPosition(g, &d);
			C3D_LightSpotEnable(g, false);
			C3D_LightDistAttnEnable(g, false);
		} else {
			C3D_FVec wp = FVec4_New(l.pos[0] + S.h->add_x, l.pos[1], l.pos[2] + S.h->add_z, 1.0f);
			C3D_FVec vp = Mtx_MultiplyFVec4(view, wp);
			C3D_LightPosition(g, &vp);
			float st = l.decay[0], en = l.decay[1] > st + 1.0f ? l.decay[1] : st + 1.0f;
			if (fabsf(st - s_da_key[used][0]) > 1.0f || fabsf(en - s_da_key[used][1]) > 1.0f) {
				LightLutDA_Create(&s_da[used], decay_ramp, 0.0f, en, st, en);
				s_da_key[used][0] = st, s_da_key[used][1] = en;
			}
			C3D_LightDistAttn(g, &s_da[used]);
			C3D_LightDistAttnEnable(g, true);
			bool spot = i >= S.n_point;
			C3D_LightSpotEnable(g, spot);
			if (spot) {
				float dx = l.at[0] - l.pos[0], dy = l.at[1] - l.pos[1], dz = l.at[2] - l.pos[2];
				C3D_FVec vd = Mtx_MultiplyFVec4(view, FVec4_New(dx, dy, dz, 0.0f));
				C3D_LightSpotDir(g, vd.x, vd.y, vd.z);
				float cone = l.cone[0] > 0.05f ? l.cone[0] : 0.5f;
				if (fabsf(cone - s_spot_key) > 0.01f) {      // one spot per scene in every script
					LightLut_FromFunc(&s_spot, spot_shape, cone, true);
					s_spot_key = cone;
				}
				C3D_LightSpotLut(g, &s_spot);
			}
		}
		used++;
	}
	for (int i = used; i < GPU_LIGHTS; i++)
		C3D_LightEnable(&s_light[i], false);
	S.lights_used = used;
	return true;
}

void demo_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	if (!S.active)
		return;
	float t = blend();
	bool own = s_game_env && lights_frame(view, t);
	if (own)
		C3D_LightEnvBind(&s_lenv);
	for (int i = 0; i < S.n_chars; i++) {
		DemoChar* dc = &S.chars[i];
		if (!dc->model || !S.next.visible[i])
			continue;
		float p[3];
		for (int k = 0; k < 3; k++)
			p[k] = lerp(S.last.chr_pos[i][k], S.next.chr_pos[i][k], t);
		p[0] += S.h->add_x, p[2] += S.h->add_z;
		// DdsPlay sets every character's rotation to (0, pi, 0); the motion turns the root
		C3D_Mtx model, mv;
		Mtx_Identity(&model);
		Mtx_Translate(&model, p[0], p[1], p[2], true);
		Mtx_RotateY(&model, (float)M_PI, true);
		Mtx_Multiply(&mv, view, &model);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
		float mf = pose_char(dc);
		if (!stereo_eye())               // the second eye of a 3D frame keeps the face (stereo.h)
			morph_char(dc, mf);
		float k = char_light(dc);
		if (own) {
			chr_light_ambient(&s_lenv, s_amb[0] * k, s_amb[1] * k, s_amb[2] * k);
		} else {
			chrlight_at(p, dc->rec->kind >= 0x100 && dc->rec->kind <= 0x103);
			if (s_light_hook && k != 1.0f)
				s_light_hook(k);
		}
		// James' parts: hhl and hhh James (0x102, 0x103) by 0x540d80, lll and hll James by the
		// player's 0x52f6a0 in its event state. The port's scenes never show his weapon, and an
		// unshown weapon leaves both hands open (right 3, left 14; hhh has no hands to swap, hhl no
		// left ones). The flashlight (21) once he has it.
		int kind = dc->rec->kind;
		if (kind >= 0x100 && kind <= 0x103)
			chr_set_parts(dc->model, kind <= 0x101 ? 14 : 0, kind == 0x103 ? 0 : 3, ev_have_item(18) ? 21 : 0);
		chr_draw(dc->model, uBones);
		if (!own && s_light_hook && k != 1.0f)
			s_light_hook(1.0f);
	}
	if (own)
		C3D_LightEnvBind(s_game_env);
}

bool demo_has_kind(int kind)
{
	if (!S.active)
		return false;
	for (int i = 0; i < S.n_chars; i++)
		if (S.chars[i].model && S.chars[i].rec->kind == kind)
			return true;
	return false;
}

bool demo_has_james(void)
{
	if (!S.active)
		return false;
	for (int i = 0; i < S.n_chars; i++)
		if (S.chars[i].model && S.chars[i].rec->kind >= 0x100 && S.chars[i].rec->kind <= 0x103)
			return true;
	return false;
}

const char* demo_subtitle(int* serial)
{
	if (serial)
		*serial = S.msg_serial * 4096 + Q.serial;
	if (S.active && S.msg_text)
		return S.msg_text;
	return Q.text;
}

const char* demo_status(void)
{
	if (S.active)
		return S.status;
	return Q.voice_on ? "demo voice track" : "demo -";
}
