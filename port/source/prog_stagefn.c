// The stages' own code besides the event programs: each PC Stage_Data has a room function
// (+0x1c, run when James enters another room of the stage) and a per-frame function (+0x20).
// They mostly set up models, lights and sounds; the parts that set or clear game flags the story
// depends on are here (the hotel 2F weight check stays in prog_hotel.c). Room numbers are the PC's
// (FUN_00494db0, bgm_room_id in the port).
#include "prog_int.h"
#include "bgm.h"
#include "combat.h"
#include <math.h>

static int s_room = -1, s_room_stage = -1;

// ---------------------------------------------------------------- room functions

static void room_enter(int stage, int room)
{
	switch (stage) {
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
static float s_lift_gate = 450.0f, s_lift_gv, s_lift_t, s_lift_y, s_lift_step, s_lift_accel_end;

static void lift_tick(float dt)
{
	if (p_flag(314)) {
		s_lift_y = -45600.0f;
		return;
	}
	if (!p_flag(312)) {
		s_lift_gate = 450.0f, s_lift_gv = s_lift_t = s_lift_y = s_lift_step = s_lift_accel_end = 0.0f;
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
// endings. Both only advance while James stays in the corridor. The port does not play the voice
// (voice 60120 is not converted); it keeps the times and the flags.
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
		}
		return;
	}
	if (p_flag(624))
		return;
	s_hall_voice += dt;
	if (s_hall_voice * 30.0f > 2400.0f)
		p_set(624);                     // FLAGW_608 |= 0x10000
}

void prog_stagefn_tick(int stage, float dt)
{
	float pos[3], yaw;
	p_player(pos, &yaw);
	int room = bgm_room_id(ev_glb_crd(), pos[0], pos[2]);
	if (stage != s_room_stage || room != s_room) {
		s_room_stage = stage;
		s_room = room;
		s_hall_t = 0.0f;                // 0x576960: the corridor's timer starts again on entering
		room_enter(stage, room);
	}
	if (dt <= 0.0f)
		return;
	switch (stage) {
	case 30: well_wall_tick(room); break;
	case 34: lift_tick(dt); break;
	case 44: hall_voice_tick(room, dt); break;
	default: break;
	}
}
