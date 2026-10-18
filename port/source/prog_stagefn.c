// The stages' own code besides the event programs: each PC Stage_Data has a room function
// (+0x1c, run when James enters another room of the stage) and a per-frame function (+0x20).
// They mostly set up models, lights and sounds; the parts that set or clear game flags the story
// depends on are here (the hotel 2F weight check stays in prog_hotel.c). Room numbers are the PC's
// (FUN_00494db0, bgm_room_id in the port).
#include "demo.h"
#include "playtest.h"
#include "prog_int.h"
#include "stageparts.h"
#include "bgm.h"
#include "combat.h"
#include "collide.h"
#include "stagemdl.h"
#include "sfx.h"
#include <math.h>

static int s_room = -1, s_room_stage = -1;

// ---------------------------------------------------------------- stage 34, the labyrinth's first part

static float s_lift_gate = 450.0f, s_lift_gv, s_lift_t, s_lift_y, s_lift_step, s_lift_accel_end;

static int ring_outer(void) { return p_flag(317) | p_flag(318) << 1; }   // EvProgControlCube's rings
static int ring_inner(void) { return p_flag(319) | p_flag(320) << 1; }

// 0x582bd0, the room function: the cube room (0x87) turns its models to the rings (0x521 about x by
// the outer and about y by the inner ring, 0x522 about y by both) and works out which of its two
// doors are open (table 0x8d41a4: 325 the south one, 326 the north one, as EvProgControlCube does);
// the lift room (0x78) starts its timers again, or shows the lift at the bottom once it has gone down
// (flag 605); the room behind the bars (0x89) has Maria there once she is found alive (327, until
// 368; flag 328; the PC places character 0x106, which the port does not draw).
static void labyrinth_room(int room)
{
	static const u8 table[16] = { 2, 1, 2, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0 };
	if (room == 0x87) {
		int a = ring_outer(), b = ring_inner();
		const float q = 1.5707964f;
		stagemdl_turn(0x521, a * q, b * q, 0.0f);
		stagemdl_turn(0x522, 0.0f, b * q, 0.0f);
		ev_flag_set(325, table[a * 4 + b] != 0);
		ev_flag_set(326, table[a * 4 + b] > 1);
	} else if (room == 0x78) {
		s_lift_gv = s_lift_step = s_lift_accel_end = s_lift_t = 0.0f;
		if (p_flag(314)) {
			p_set(605);                 // FLAGW_576 |= 0x2000
			s_lift_y = -45600.0f;
			s_lift_gate = 450.0f;
		} else {
			s_lift_y = 0.0f;
		}
	} else if (room == 0x89) {
		if (p_flag(327) && !p_flag(368))
			p_set(328);                 // FLAGW_320 |= 0x100
	}
}

// 0x583960 case 0x87: the cube room's collision. The doors the rings leave shut get a wall
// (0x5228e0 with 0x8d3a28, the south door, while 325 is clear, and 0x8d3ac8, the north one, while
// 326 is), and of the cube's four arms only those the rings line up are open: the others get a
// ceiling at floor height (0x522910 with 0x8d3d48 north, 0x8d3c08 east, 0x8d3ca8 south, 0x8d3b68
// west), which the port turns into the arm's outline as walls.
// 0x583960, room 0x87: the walls across the cube room's two ways while they are shut (0x8d3a28 the
// south one without 325, 0x8d3ac8 the north one without 326). The arms of the cross are floors the
// PC adds by ring (0x522910 with 0x8d3d48 north, 0x8d3c08 east, 0x8d3ca8 south, 0x8d3b68 west) at
// y 0 over the room's own at y 75; the port has no added floors and leaves them at 75.
static void cube_room_collision(void)
{
	if (!p_flag(325))
		col_extra_wall(19795.0f, -21950.0f, 20205.0f, -21950.0f, -1000.0f, 0.0f);
	if (!p_flag(326))
		col_extra_wall(19795.0f, -18250.0f, 20205.0f, -18250.0f, -1000.0f, 0.0f);
}

// ---------------------------------------------------------------- stage inits (Stage_Data +0x18)
//
// Run once when a stage loads (the map marks one stage leaves for another, flags the stage expects).
static void stage_init(int stage)
{
	switch (stage) {
	case 5:                              // 0x59e190: map marks carried over from town west
		if (p_flag(1534)) p_set(1497);
		if (p_flag(1533)) p_set(1496);
		if (p_flag(1494)) p_set(1526);
		break;
	case 12:                             // 0x596650
		p_clear(87);
		break;
	case 14:                             // 0x593cc0: the marks back the other way, and Maria with James
		if (p_flag(1497)) p_set(1534);
		if (p_flag(1496)) p_set(1533);
		if (p_flag(1526)) p_set(1494);
		if (p_flag(152) && (!p_flag(162) || p_flag(163)) && !p_flag(251))
			p_set(15);
		break;
	case 15:                             // 0x591da0
		p_clear(15);
		break;
	case 49:                             // 0x4410a0
		p_set(45);
		p_set(168);
		break;
	}
}

static float frand(void) { return (float)p_rand() * 4.656612873e-10f; }   // 0x448830

// ---------------------------------------------------------------- one-shot sounds of the per-frame code
//
// Stages whose per-frame code plays a sound once where James passes (and a flag remembers it); the
// PC's sounds that move or fade after they start are kept where they start.
static float s_thr;                      // a room's threshold (picked by its room function)
static bool s_armed;

static void frame_sounds(int stage, int room, const float* me)
{
	switch (stage) {
	case 9:                              // 0x5994f0, room 0x1f: steps in the hallway west of James
		if (room == 0x1f && !p_flag(552) && me[2] < 17000.0f) {
			static const float at[3] = { 60000.0f, 0.0f, 11350.0f };
			sfx_play_at(0x9c51, 1.0f, at);
			p_set(552);
		}
		break;
	case 11:                             // 0x5974d0
		if (room == 0x28 && !p_flag(547) && me[0] < s_thr) {
			float a = frand() * 0.7853982f + 1.5707964f;   // somewhere round James, 2500 away
			float at[3] = { me[0] + sinf(a) * 2500.0f, me[1], me[2] + cosf(a) * 2500.0f };
			sfx_play_at(0x9c70, 1.0f, at);
			p_set(547);
		} else if (room == 0x26) {
			if (!s_armed && me[2] > -6500.0f)
				s_armed = true;
			else if (s_armed && me[2] < -6800.0f && !p_flag(553) && !p_flag(114)) {
				static const float at[3] = { 100807.1f, -500.0f, -6053.485f };   // 0x8e27f4
				sfx_play_at(0x9c6c, 1.0f, at);
				p_set(553);
			}
		}
		break;
	case 15:                             // 0x591f20, room 0xa: the voice through the wall
		if (room == 0xa && !p_flag(161)) {
			demo_voice_free("sdmc:/3ds/sh23ds", "voice_591f20");   // 0x49fe30: 60136, 0x8ddbbc
			p_set(161);
		}
		break;
	case 28:                             // 0x587c40, room 0x5c
		if (room == 0x5c && !p_flag(551) && me[0] > -19600.0f) {
			static const float at[3] = { -22000.0f, -500.0f, -18900.0f };   // 0x8d7664
			sfx_play_at(0x4982, 1.0f, at);
			p_set(551);
		}
		break;
	case 31:                             // 0x585bb0, room 0x73: a sound now and then past a spot
		if (room == 0x73 && p_flag(648) && !p_flag(649) && me[2] > s_thr) {
			if (frand() > 0.4f) {
				static const float at[3] = { -138700.0f, -50.0f, -21000.0f };   // 0x8d6424
				sfx_play_at(0x427d, 1.0f, at);
				p_set(649);
			} else {
				p_clear(648);
			}
		}
		break;
	case 39:                             // 0x57db00, room 0xa8
		if (room == 0xa8 && me[0] > -15455.0f) {
			p_set(2070);
			if (!p_flag(549) && !p_flag(384)) {
				float at[3] = { me[0] + frand() * 1500.0f, me[1], me[2] - frand() * 1500.0f };
				sfx_play_at(0x9cfe, 1.0f, at);
				p_set(549);
			}
		}
		break;
	case 55:                             // 0x572ce0: the present (model 0x752, after 721) is in James' room
		{
			bool here = p_flag(721) && room == bgm_room_id(ev_glb_crd(), -18827.5f, 20410.0f);
			ev_flag_set(729, here);
		}
		break;
	}
}

// ---------------------------------------------------------------- room functions

static void room_enter(int stage, int room)
{
	stagemdl_maria_bed(stage == 20 && room == 0x41);   // 0x58ef90 places her there (character 0x106)
	s_armed = false;
	switch (stage) {
	case 11:
		if (room == 0x28)                // 0x5974a0
			s_thr = 21200.0f - frand() * 1200.0f;
		break;
	case 15:                             // 0x591dd0: everywhere but room 9
		if (room != 9)
			p_set(530);
		break;
	case 31:                             // 0x585b70
		if (room == 0x73) {
			s_thr = frand() * 800.0f - 20000.0f;
			if (frand() > 0.3f) {
				p_clear(648);
				p_clear(649);
			}
		}
		break;
	case 41:                             // 0x57a410: one time in four
		if (room == 0x9f && (p_rand() & 3) == 0)
			p_set(541);
		break;
	case 16:
		// 0x591340: back at Heaven's Night after Maria's second wait at the bowling alley (163)
		// Laura has gone: 164 keeps the "Laura can't go" scene (stage 14 event 35) from coming
		if (p_flag(163))
			p_set(164);
		break;
	case 20:
		// 0x58ef90: the day room S3 (0x41). Maria lies down there: she stops following (15), the
		// PC places her in the bed (character 0x106); 188 is set, and once she has rested (186)
		// every visit sets 629, which counts for the Maria ending
		if (room == 0x41) {
			if (p_flag(15))
				p_clear(15);
			p_set(188);
			if (p_flag(186))
				p_set(629);
		}
		break;
	case 34:
		labyrinth_room(room);
		break;
	case 35:
		// 0x5814f0: room 0x79 before the Abstract Daddy (332): Angela's scream plays again (633)
		if (room == 0x79 && !p_flag(332))
			p_clear(633);
		break;
	case 56:
		// 0x571a30: leaving the candle's light (710) for another room puts it out
		if (p_flag(710))
			p_clear(710);
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------- per-frame functions

// Stage 30 (0x587780): at the bottom of the well (room 0x62) the soft part of the wall has 250
// points (the room function 0x587690 sets them); every blow of James' that strikes the room
// (0x544fd0) takes its ap from them, a handgun or shotgun shot a fiftieth and a rifle shot a tenth
// of it, and at 0 flag 267 lets EvProgBreakWellWall run. The PC does not look at where in the room
// the blow lands.
static float s_well_wall = 250.0f;

static void well_wall_tick(int room)
{
	if (p_flag(268) || p_flag(267)) {
		s_well_wall = 250.0f;
		return;
	}
	int weapon;
	float ap = combat_wall_hit(&weapon);
	if (ap <= 0.0f || room != 0x62)
		return;
	ap *= weapon == 4 || weapon == 6 ? 0.02f : weapon == 8 ? 0.1f : 1.0f;
	s_well_wall -= ap;
	if (s_well_wall <= 0.0f)
		p_set(267);                     // FLAGW_256 |= 0x800
}

// Stage 34 (0x582ee0 with the model code 0x5832f6): the lift in the labyrinth's first room. Once
// James has stepped onto it (flag 312, an enter event in ps66) the gate closes (450 units at a
// speed rising by 800 a second to 1000), two seconds later the ride starts (313) and goes down
// 45600 units, speeding up by 6 units a frame each second to 500 a second plus a random 0..300
// (600 at full speed) and slowing the same way near the bottom; at the bottom flag 314 opens the
// way out and the save point, and with the prison map the labyrinth map's flag 29 is set too. The
// PC moves the room's models; the port keeps the timing and the flags.
static float s_lift_arr_t, s_lift_open, s_lift_ov, s_fan;   // 0x1d8621c, 0x1d86210, 0x1d86214
static void lift_tick(float dt)
{
	if (p_flag(314)) {
		// 0x582f41: at the bottom, 2 s later the gate goes up again (accelerating 800 a second to
		// 1000) and lets James out (315) once it is 292.5 open, until 450
		s_lift_y = -45600.0f;
		if ((s_lift_arr_t += dt) >= 2.0f && s_lift_open < 450.0f) {
			s_lift_ov = s_lift_ov + dt * 800.0f > 1000.0f ? 1000.0f : s_lift_ov + dt * 800.0f;
			s_lift_open += dt * s_lift_ov;
			if (s_lift_open >= 292.5f)
				p_set(315);             // FLAGW_288 |= 0x8000000
		}
		return;
	}
	if (!p_flag(312)) {
		s_lift_gate = 450.0f, s_lift_gv = s_lift_t = s_lift_y = s_lift_step = s_lift_accel_end = 0.0f;
		s_lift_arr_t = s_lift_open = s_lift_ov = 0.0f;
		return;
	}
	if (!p_flag(313)) {
		if (s_lift_gate > 0.0f) {
			s_lift_gv = s_lift_gv + dt * 800.0f > 1000.0f ? 1000.0f : s_lift_gv + dt * 800.0f;
			s_lift_gate -= dt * s_lift_gv;
			if (s_lift_gate < 0.0f)
				s_lift_gate = 0.0f;
		} else if ((s_lift_t += dt) >= 2.0f) {
			p_set(313);                 // FLAGW_288 |= 0x2000000
		}
		return;
	}
	// 0x5834ec: the step per frame (in units per frame at the frame time dt)
	float jitter = (float)(p_rand() % (s_lift_step >= 500.0f * dt ? 600 : 300)) * dt;
	s_lift_y -= jitter + s_lift_step;
	if (-45600.0f - s_lift_accel_end < s_lift_y)
		s_lift_step += dt * 6.0f;
	else
		s_lift_step -= dt * 6.0f;
	if (s_lift_step >= 500.0f * dt) {
		s_lift_step = 500.0f * dt;
		if (s_lift_accel_end == 0.0f)
			s_lift_accel_end = s_lift_y;
	}
	if (s_lift_step < 0.0f)
		s_lift_step = 0.0f;
	if (s_lift_y <= -45600.0f) {
		s_lift_y = -45600.0f;
		p_set(314);                     // 0x583345: FLAGW_288 |= 0x4000000
		if (p_flag(28))
			p_set(29);
	}
}

// Stage 44 (0x5769b0): in the corridor before the last door (room 0xba) Mary's voice starts after
// 3.5 seconds (0x49fe30 with voice 60120 and its subtitle table 0x8cb100; flag 623); once it has
// played for 2400 frames (80 seconds, 0x49fe70) flag 624 counts for the In Water and Leave
// endings. Both only advance while James stays in the corridor. The voice plays while he walks
// (demo_voice_free, pack/demo/voice_5769b0.voc with its 26 subtitles).
static float s_hall_t, s_hall_voice;

static void hall_voice_tick(int room, float dt)
{
	if (room != 0xba)
		return;
	if (!p_flag(623)) {
		s_hall_t += dt;
		if (s_hall_t > 3.5f) {
			p_set(623);                 // FLAGW_608 |= 0x8000
			s_hall_voice = 0.0f;
			bool on = demo_voice_free("sdmc:/3ds/sh23ds", "voice_5769b0");   // 0x49fe30: 60120, table 0x8cb100
			if (playtest_active())
				playtest_log("corridor voice %s", on ? "started" : "could not start");
		}
		return;
	}
	if (p_flag(624))
		return;
	s_hall_voice += dt;
	if (s_hall_voice * 30.0f > 2400.0f)
		p_set(624);                     // FLAGW_608 |= 0x10000
}

// 0x5847f0, the south prison's per-frame function: the bars of the Oppressor's cell, shut by the
// tablet (288), open again 20 s after James first tries them (289, event 25); each later try (290,
// event 26) brings it 10 s closer (timer 0x1d86250).
static float s_bars_t;
static void prison_bars_tick(float dt)
{
	if (!p_flag(288) || !p_flag(289))
		return;
	s_bars_t += dt;
	if (p_flag(290)) {
		p_clear(290);
		s_bars_t += 10.0f;
	}
	if (s_bars_t > 20.0f) {
		p_clear(288);
		s_bars_t = 0.0f;
	}
}

void prog_stagefn_tick(int stage, float dt)
{
	float pos[3], yaw;
	p_player(pos, &yaw);
	int room = bgm_room_id(ev_glb_crd(), pos[0], pos[2]);
	if (stage != s_room_stage) {
		s_room_stage = -2;
		stage_init(stage);
	}
	if (stage != s_room_stage || room != s_room) {
		s_room_stage = stage;
		s_room = room;
		s_hall_t = 0.0f;                // 0x576960: the corridor's timer starts again on entering
		room_enter(stage, room);
	}
	col_extra_clear();
	if (dt <= 0.0f)
		return;
	frame_sounds(stage, room, pos);
	stageparts_tick(stage, room, dt);   // the map part switches (stageparts.c)
	switch (stage) {
	case 30: well_wall_tick(room); break;
	case 34:
		// 0x583a0d: the fan (model 0x523) turns half a turn a second
		s_fan = fmodf(s_fan + dt * 3.1415927f, 6.2831855f);
		stagemdl_turn(0x523, 0.0f, s_fan, 0.0f);
		// 0x583960: by room
		if (room == 0x78) {
			// the lift's gate behind James while it moves (0x8d3938: two planes), and the ride
			if (p_flag(312) && !p_flag(315)) {
				col_extra_wall(-19125.0f, -99525.0f, -19125.0f, -100450.0f, -1200.0f, 0.0f);
				col_extra_wall(-19000.0f, -100450.0f, -19000.0f, -99525.0f, -1200.0f, 0.0f);
			}
			lift_tick(dt);
			// 0x5831aa: the lift's two gate leaves (model 0x530, bones 0 and 1) apart by the opening
			// (450 open; the PC also shakes them while it rides, which the port leaves out)
			float g = !p_flag(312) ? 450.0f : !p_flag(314) ? s_lift_gate : s_lift_open;
			stagemdl_bone_move(0x530, 0, 0.0f, 0.0f, -g);
			stagemdl_bone_move(0x530, 1, 0.0f, 0.0f, g);
		} else if (room == 0x84 && !p_flag(330)) {
			// the wire across the way until the cutter takes it (0x8d3de8)
			col_extra_wall(63950.0f, 100026.5f, 63950.0f, 99572.5f, -1018.1f, 0.1f);
		} else if (room == 0x87) {
			cube_room_collision();
		}
		break;
	case 32: prison_bars_tick(dt); break;
	case 44: hall_voice_tick(room, dt); break;
	default: break;
	}
}
