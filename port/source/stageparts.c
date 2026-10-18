// The stage code's map part switches (room.h: room_parts_add / room_parts_apply), as the PC's
// per-frame stage functions list them through 0x4a2a80 and apply them through 0x4a2ad0. Each
// stage's switches are listed here by the room (0x495220) and the flags the PC tests; the walls the
// same functions add are in prog_stagefn.c.
//
// Enhanced Edition's MapMeshToggle (Patches/MapMeshToggle.cpp, zlib licence) adds switches the PC
// leaves out: the motorhome hint (stage 2), Blue Creek 1F (10), the hospital garden (22), the
// otherworld lobby (23), the hospital 3F ring door (25), the lake (38) and room 312 (42).
//
// "on" ids take a mask bit, "off" ids are listed with none (bit -1), which shows only variant 0.
// The town's per-tile code (stages 5, 14 and 54) runs for the 3 x 3 tiles around James; a switch
// keyed by a tile's id only reaches that tile, so listing every tile's switch each frame is the
// same for the tiles that are loaded.
#include "stageparts.h"
#include "demo.h"
#include "events.h"
#include "prog_int.h"
#include "room.h"

static RoomPartList L;
static int s_glb;

static void on(int id, int bit) { room_parts_add(&L, s_glb, id, bit); }
static void by(int id, bool flag, int bit_on, int bit_off) { on(id, flag ? bit_on : bit_off); }

static bool f(int n) { return p_flag(n); }
// 0x934d94 & 0x40: set by 0x4a1c80 while a scripted scene runs; 0x1f7d3c4 (0x49fe20) its number
static bool scene(void) { return demo_number() >= 0; }
static bool scene_no(int n) { return demo_number() == n; }

// ---------------------------------------------------------------- traffic lights

// The town's traffic lights (stage 5 0x59e490.. on cb27, cb28, cb29, cb35, cb36 and cb37, stage
// 14 0x594050 / 0x5940f0 on cc10 and cc20): a 3100 ms cycle over the frame ticks (0x4479b0, added
// up in 0x1d86710 / 0x1d86614), and on some a short flicker at random times (0x1d86714: the next
// one 400..1399 ms after the last).
static int s_t5, s_t14, s_flick_next;

static int flicker(int state)
{
	if (s_t5 > s_flick_next) {
		s_flick_next += 400 + p_rand() % 1000;
		return 0;
	}
	return state;
}

// 0x59e490 (0x1b), 0x59e580 (0x1c), 0x59e710 (0x23): green 1200, amber 300, red the rest, flicker
static void light_a(int id)
{
	int r = s_t5 % 3100;
	int s = flicker(r < 1200 ? 3 : r < 1500 ? 2 : 1);
	on(id, s == 1 ? 2 : s == 2 ? 1 : s == 3 ? 0 : 5);
	on(id, r >= 1800 && r < 2700 ? 4 : 3);
}

// 0x59e670 (0x1d), 0x59e800 (0x24): the same without the flicker
static void light_b(int id)
{
	int r = s_t5 % 3100;
	on(id, r < 1200 ? 0 : r < 1500 ? 1 : 2);
	on(id, r >= 1800 && r < 2700 ? 4 : 3);
}

// 0x59e8a0 (0x25)
static void light_c(void)
{
	if (f(43))
		on(0x25, 0);
	int r = s_t5 % 3100;
	int s = flicker(r < 1800 ? 1 : r < 2750 ? 3 : r < 3050 ? 2 : 1);
	on(0x25, s == 1 ? 3 : s == 2 ? 2 : s == 3 ? 1 : 6);
	on(0x25, r >= 1800 && r < 2700 ? 5 : 4);
}

// ---------------------------------------------------------------- stages

static int s_last_stage = -1, s_last_room = -1;
static bool s_knife_floor;            // 0x8e315c (stage 10 room 0x24)

void stageparts_tick(int stage, int room, float dt)
{
	bool entered = stage != s_last_stage || room != s_last_room;
	if (stage != s_last_stage)
		s_t5 = s_t14 = s_flick_next = 0;
	s_last_stage = stage, s_last_room = room;
	int ms = (int)(dt * 1000.0f + 0.5f);
	L.n = 0;
	s_glb = ev_glb_crd();
	switch (stage) {
	case 2:   // no display function on the PC; EE MapMeshToggle gives the motorhome one
		if (room == 0x05)
			on(0x05, f(0x31) ? -1 : 0);   // EE: the hint until James has been through the apartment gate
		break;
	case 3:   // 0x5a1520
		by(3, f(24), -1, 0);
		break;
	case 4:   // 0x59fec0
		by(0x14, f(12), 0, -1);
		by(0xa, scene(), 0, -1);
		break;
	case 5:   // 0x59ef10 and its tile helpers
		s_t5 += ms;
		by(0x31, f(251), 0, 1);
		by(0xb, f(43), 0, 1);
		by(0xc, f(43), 0, 1);
		on(0xd, -1);
		if (f(251))
			on(0xd, 0);
		by(0x12, f(43), 0, -1);
		by(0x13, f(251), 0, -1);
		by(0x14, f(251), 0, -1);
		by(0x15, f(251), 0, -1);
		by(0x34, f(251), 1, 0);           // 0x59e440
		light_a(0x1b);
		light_a(0x1c);
		light_b(0x1d);
		light_a(0x23);
		light_b(0x24);
		light_c();
		by(0x2e, f(49), 0, 1);
		break;
	case 7:   // 0x59bca0
		if (room == 0x17) {
			by(0x20, f(7) || f(8) || f(9), 0, -1);
		} else if (room == 0x18) {
			static const u8 on69[][2] = { { 42, 0 }, { 43, 0 }, { 43, 1 }, { 43, 2 }, { 43, 3 }, { 45, 0 }, { 45, 2 }, { 45, 4 } };
			static const u8 off69[][2] = { { 42, 1 }, { 43, 4 }, { 45, 1 }, { 45, 3 }, { 45, 5 } };
			if (f(69))
				for (unsigned i = 0; i < sizeof on69 / 2; i++)
					on(on69[i][0], on69[i][1]);
			else
				for (unsigned i = 0; i < sizeof off69 / 2; i++)
					on(off69[i][0], off69[i][1]);
		}
		break;
	case 9:   // 0x5994f0
		if (room == 0x13)
			by(0x13, f(91), 0, -1);
		break;
	case 10:  // 0x598770 (0x8e315c: set on entering room 0x24 by 0x598700, cleared by the knife scene)
		if (room == 0x24 && entered)
			s_knife_floor = true;
		if (room == 0x24 && scene())
			s_knife_floor = false;
		if (room == 0x22) {
			// EE MapMeshToggle replaces this room's list (the PC's 0x4e switch) with part 0x5c,
			// variant 1 once James has met Eddie (flag 0x55)
			on(0x5c, f(0x55) ? 1 : 0);
		} else if (room == 0x24) {
			on(0x57, -1);
			if (!f(85) || f(109))
				on(0x57, 0);
			if (s_knife_floor)
				on(0x57, 1);
		}
		break;
	case 11:  // 0x5974d0
		if (room == 0x27) {
			on(0x41, -1);
			if (f(102))
				on(0x41, 0);
			by(0x43, f(108), 0, 1);
		}
		break;
	case 12:  // 0x596660
		if (room == 0x10)
			by(0x19, f(25), -1, 0);
		else if (room == 0x20)
			by(0x6e, f(26), -1, 0);
		break;
	case 13:  // 0x596060
		if (room != 7) {
			if (f(95))
				on(0x2e, 2);
			// Enhanced Edition FixTownGateEvents (Patches/Langs.cpp PatchTownGateEvents, zlib licence)
			// changes this variant from 1 to 0 (the byte at 0x596090): the gate's lock model
			on(0x2e, 0);
		}
		break;
	case 14:  // 0x594e10 and its tile helpers
		s_t14 += ms;
		if (f(258)) {
			on(0x12, 0);
			on(0x12, 1);
		} else if (f(252)) {
			on(0x12, 3);
			on(0x12, 1);
		} else {
			on(0x12, 2);
		}
		on(0xb, 0);
		on(0x17, 0);
		on(0xe, 0);
		on(0xd, 0);
		{
			int r = s_t14 % 3100;      // 0x594050 (0xa), 0x5940f0 (0x14)
			on(0xa, r < 1200 ? 0 : r < 1500 ? 1 : 2);
			on(0xa, r >= 1800 && r < 2700 ? 4 : 3);
			on(0x14, r < 1200 ? 2 : r < 1500 ? 3 : 4);
			on(0x14, r >= 1800 && r < 2700 ? 1 : 0);
		}
		break;
	case 18:  // 0x590df0
		if (room == 0x33)
			by(4, f(27), -1, 0);
		break;
	case 21:  // 0x58d2d0
		if (room == 0x2a) {
			by(0x19, f(21), 2, 3);
			by(0x19, f(22), 1, 4);
			by(0x19, f(23), 0, 5);
		}
		break;
	case 22:  // 0x58c040
		if (room == 0x30)
			by(0x27, f(213), -1, 0);
		else if (room == 0x49) {
			by(0xab, f(214), 0, -1);
			on(0xaa, ev_have_item(0x4e) ? 0 : -1);   // EE MapMeshToggle: shown while James has the blue gem
		}
		break;
	case 23:  // no display function on the PC; EE MapMeshToggle gives the lobby one (room 0x4d)
		if (room == 0x4d)
			on(0x97, scene_no(0x32) ? 1 : 0);   // variant 1 during the third chase scene
		break;
	case 25:  // 0x589fa0
		if (room == 0x54)
			on(0xd3, !f(232) ? -1 : f(240) ? 0 : 1);
		else if (room == 0x57) {           // EE MapMeshToggle: the rings placed on the door (flags 0xe3, 0xe4)
			if (f(0xe3))
				on(0xd6, 0);
			if (f(0xe4))
				on(0xd6, 1);
			if (!f(0xe3) && !f(0xe4))
				on(0xd6, -1);
		}
		break;
	case 26:  // 0x588bf0
		if (room == 0x46) {
			by(0x9e, f(21), 5, 2);
			by(0x9e, f(22), 4, 1);
			by(0x9e, f(23), 3, 0);
		}
		break;
	case 30:  // 0x587780
		if (room == 0x62)
			by(0x37, f(268), 0, 1);
		else if (room == 0x64)
			by(0x35, f(271), 0, 1);
		break;
	case 31:  // 0x585bb0
		if (room == 0x66)
			by(0x2d, f(283), 0, 1);
		else if (room == 0x67)
			by(0x13, f(28), -1, 0);
		break;
	case 32:  // 0x5847f0: while a character of kind 0x70d is there (0x537950)
		if (room == 0x68)
			by(0x1d, demo_has_kind(0x70d), 0, -1);
		break;
	case 33:  // 0x5843b0
		if (room == 0x77) {
			on(0x39, -1);
			if (!f(309))
				on(0x39, 0);
		}
		break;
	case 34:  // 0x583960
		if (room == 0x84) {
			by(0x7d, f(330), 0, 1);
		} else if (room == 0x87) {
			on(0x79, -1);
			on(0x7b, -1);
			if (!f(325)) {
				on(0x79, 0);
				on(0x7b, 1);
			} else {
				on(0x7b, 0);
			}
			by(0x7a, f(327), 0, 1);
		} else if (room == 0x89) {
			on(0x56, 1);
		}
		break;
	case 35:  // 0x581580: the scene set pieces of scenes 0x43 and 0x44
		if (room == 0x8a) {
			if (scene_no(0x43) || (scene_no(0x44) && !f(328))) {
				on(0x63, 0);
				on(0x63, 1);
				for (int b = 0; b < 6; b++)
					on(0x64, b);
			} else {
				on(0x63, -1);
				on(0x64, -1);
			}
		}
		break;
	case 36:  // 0x580560
		if (room == 0x8b) {
			static const s8 bit[6] = { 2, 1, 0, 5, 4, 3 };   // flags 343..348 (the innocent man's rope)
			on(0x6d, -1);
			for (int j = 0; j < 6; j++)
				if (f(343 + j))
					on(0x6d, bit[j]);
		} else if (room == 0x86) {
			by(0x6a, f(367), 0, 1);
		} else if (room == 0x89) {
			// 0x1d86148 is set by the room's own code (a scene in progress); the port leaves it off
			on(0x55, 0);
		}
		break;
	case 38:  // 0x57eeb0 sets no parts on the PC; EE MapMeshToggle adds the lake's two
		on(0x01, f(0x2d5) ? 0 : -1);       // after the blue gem's first use
		on(0x0b, scene_no(0x61) ? 3 : -1); // the Rebirth epilogue
		break;
	case 39:  // 0x57db00
		if (room == 0xa5) {
			if (f(386)) {
				on(4, 3);
				on(4, 0);
			} else {
				on(4, 2);
				on(4, 1);
			}
		} else if (room == 0xa6) {
			by(7, f(385), 1, 0);
		}
		break;
	case 40:  // 0x57c370
		if (room == 0x98)
			by(0x2c, f(383), 0, 1);
		else if (room == 0x9a)
			by(0x23, f(31), -1, 0);
		else if (room == 0x99)
			by(0x20, f(30), -1, 0);
		break;
	case 41:  // 0x57a4a0
		if (room == 0x9d) {
			by(0x4b, f(402), -1, 0);
			by(0x4c, f(402), -1, 0);
			by(0x4e, f(402), 0, -1);
		} else if (room == 0x9e) {
			by(0x55, f(413), 1, 0);
			by(0x56, f(412), 0, 1);
		}
		break;
	case 42:  // 0x578950
		if (room == 0xa2) {
			by(0x5c, f(475), -1, 0);
			by(0x5d, f(475), 1, 0);
			// EE MapMeshToggle: the hint in room 312 after the blue gem's second use, until the tape;
			// while it shows, the room's event 19 also tests flag 0x2d6 (EE writes the PC's word)
			bool hint = f(0x2d6) && !f(0x1db);
			on(0x5b, hint ? 0 : -1);
			ev_set_flag_lo(19, hint ? 0x2d6 : 0);
		} else if (room == 0xa3) {
			by(0x5a, f(473), 1, 0);
		}
		break;
	case 43:  // 0x577690
		if (room == 0xb6)
			on(4, 0);
		else if (room == 0xb7)
			on(7, 1);
		break;
	case 45:  // 0x575f30
		by(0x37, scene_no(0x56), 0, -1);
		break;
	case 48: case 49: case 50: case 51: {   // 0x440ad0, 0x441ac0, 0x43fc80, 0x575450: by James' height
		float pos[3], yaw;
		p_player(pos, &yaw);
		float y = pos[1];
		if (y > -4000.0f) on(0x49, 3);
		if (y > -8000.0f) on(0x49, 2);
		if (y > -14000.0f && y < 0.0f) on(0x49, 1);
		if (y > -16000.0f && y < -4000.0f) on(0x49, 0);
		if (y < -8000.0f) on(0x49, 4);
		by(0x4c, y > -16000.0f && y < -13000.0f, 0, -1);
		break;
	}
	case 53:  // 0x5745c0
		if (room == 0xd)
			by(1, f(640), 1, 0);
		break;
	case 54:  // 0x573f50
		on(0xb, 1);
		on(0xc, 0);
		break;
	case 55:  // 0x572ce0
		if (room == 0xc6) {
			if (f(722)) {
				on(1, -1);
				on(2, 1);
				on(3, 0);
				on(4, 1);
			} else {
				on(1, 1);
				on(2, 3);
				on(3, 1);
				on(4, 0);
				on(2, 0);
			}
			if (scene_no(0x68)) {
				on(1, 0);
				on(2, 2);
				on(3, 2);
			}
		} else if (room == 0xc1) {
			by(0x2b, f(672), 0, 1);
			if (!f(670))
				on(0x2b, 2);
		}
		break;
	case 56:  // 0x571ac0
		if (room == 0xc7)
			by(0x48, f(709), 0, 1);
		break;
	default:
		return;
	}
	room_parts_apply(&L, s_glb);
}
