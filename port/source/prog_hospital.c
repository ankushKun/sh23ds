#include "prog_int.h"
#include <stdio.h>
#include <string.h>

// Stage programs from Pete's Bowl-o-Rama to the end of Brookhaven Hospital (stages 15..27), ported
// from the PC overlays. PS2 beta names come from Event/stage/stg_bowling.c .. stg_hospital_pass.c,
// which list the programs in the PC tables' order. Flags, items, messages and SE ids are the PC's;
// cutscenes go to prog_demo_request(), positions are where the PC puts James after them.

// ================================================================ stage 15 Stg_bowling (0x8dd7b0)

// 0x591860 EvProgEddieAndLaura
static int s15_eddie_and_laura(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EDDIE_LAURA))
		p_lock(p, false);
	PROG_END(p)
}

// 0x591a40 EvProgEddieEatPizza
static int s15_eddie_pizza(Prog* p)
{
	PROG_BEGIN(p)
		p_set(548);                     // FLAGW_544 |= 0x10
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EDDIE_PIZZA))
		p_place(15918.216f, 0.0f, 16339.526f, 2.4103518f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x591c10 EvProgEddieEatPizzaAfter
static int s15_eddie_pizza_after(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EDDIE_PIZZA_AFTER))
		p_place(15700.0f, 0.0f, 16650.0f, -0.4526857f);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage15[4] = { NULL, s15_eddie_and_laura, s15_eddie_pizza, s15_eddie_pizza_after };

// ================================================================ stage 16 Stg_to_heaven (0x8dd358)

// 0x590f10 EvProgLauraWentSideRoad
static int s16_laura_side_road(Prog* p)
{
	PROG_BEGIN(p)
		p_set(657);                     // FLAGW_640 |= 0x20000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LAURA_SIDE_ROAD))
		p_place(-98934.0f, 0.0f, 30690.7f, 0.0675267f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x591070 EvProgMariaUnlockHeaven
static int s16_maria_unlock_heaven(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARIA_UNLOCK_HEAVEN))
		p_place(-98951.5f, 0.0f, 33388.4f, 2.3552065f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x5911c0 EvProgJamesCantGo
static int s16_james_cant_go(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_JAMES_CANT_GO))
		p_place_keep_yaw(-98843.0f, 0.0f, 30039.45f);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage16[4] = { NULL, s16_laura_side_road, s16_maria_unlock_heaven, s16_james_cant_go };

// Stage 17 Stg_heaven_night has no program table (its ev_prog points into bss).

// ================================================================ hospital elevators

// EvProgElevatorButton (0x58d480 front side, 0x588e60 otherworld). Flags 17, 18, 19 say on which
// floor the elevator stands (1F, 2F, 3F), the buttons set 21, 22, 23 for the floor wanted and the
// stage's auto door events do the ride. The PC maps its five buttons per floor (0x58c820 /
// 0x5885f0); the panel lists the floors the buttons lead to.
static const char* const s_floor_name[3] = { "1F", "2F", "3F" };

static bool elevator_same_floor(void)
{
	return (p_flag(17) && p_flag(21)) || (p_flag(18) && p_flag(22)) || (p_flag(19) && p_flag(23));
}

static bool elevator_pressed(void) { return p_flag(21) || p_flag(22) || p_flag(23); }

static int elevator(Prog* p, bool otherworld)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show(otherworld ? "hsp_p_h_elevator_2" : "hsp_p_h_elevator");
		p->step = 8, p->sub = 0;
		// fallthrough
	case 8: {
		bool here = p_flag(17) || p_flag(18) || p_flag(19);
		if (p->down & (KEY_DUP | KEY_DLEFT)) p->work[1] = (p->work[1] + 2) % 3;
		if (p->down & (KEY_DDOWN | KEY_DRIGHT)) p->work[1] = (p->work[1] + 1) % 3;
		char buf[200];
		int n = snprintf(buf, sizeof buf, "\n");
		for (int i = 2; i >= 0; i--)
			n += snprintf(buf + n, sizeof buf - n, "   %c %s\n", p->work[1] == i ? '>' : ' ', s_floor_name[i]);
		snprintf(buf + n, sizeof buf - n, "\n\n\n\n      D-pad: floor  A: press  B: leave");
		p_panel(buf);
		if (p->down & KEY_B) {
			p_panel(NULL);
			PROG_JUMP(p, 0xd);
		}
		if (!(p->down & KEY_A))
			return 0;
		p_se(0x4a49);
		if (here)
			p_set(21 + p->work[1]);
		if (!elevator_pressed())
			return 0;
		p_panel(NULL);
		p->timer = 0.0f;
		p->step = 7, p->sub = 0;
		return 0;
	}
	case 7:
		if (!p_wait(p, 1.0f))
			return 0;
		p_picture_hide();
		if (!elevator_same_floor())
			p_se(0x4a43);
		if (elevator_same_floor())
			PROG_JUMP(p, 0xd);
		// the otherworld elevator: the first ride down with Maria plays the "quizu" scene
		if (otherworld && p_flag(15) && !p_flag(232))
			PROG_JUMP(p, 0x12);
		p->timer = 0.0f;
		p->work[2] = (p_flag(19) && p_flag(21)) || (p_flag(17) && p_flag(23)) ? 45 : 30;   // 4.5 or 3 s
		p->step = 0x10, p->sub = 0;
		// fallthrough
	case 0x10:
		if (!p_wait(p, p->work[2] / 10.0f))
			return 0;
		p_se(0x4a44);
		PROG_JUMP(p, 0xd);
	case 0x12:
		if (!p_demo(p, DEMO_ELEVATOR_MARIA))
			return 0;
		p_set(232);                     // FLAGW_224 |= 0x100
		p_se(0x4a44);
		// fallthrough
	case 0xd:
	default:
		p_lock(p, false);
		if (elevator_pressed()) {
			p_clear(17);                // gGameFlag &= 0xfff1ffff
			p_clear(18);
			p_clear(19);
		}
		if (!otherworld && p_flag(21))
			p_set(176);                 // FLAGW_160 |= 0x10000
		p_picture_hide();
		return 1;
	}
}

// ================================================================ stage 18 Stg_hospital_1f_f (0x8dcc30)

// 0x590420 EvProgGetHospitalMap
static int s18_get_hospital_map(Prog* p) { return p_map_get(p, "map_hospitalmap01", 10); }

// 0x590430 EvProgDoctorMemo1st: the doctor's memo; the "Purple Bull" key is clipped to it.
static int s18_doctor_memo_1(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1a, p_message(p, 4))
	PROG_STEP(p, 2, p_picture(p, "hsp_p_doctormemo"))
	PROG_STEP(p, 7, p_message(p, 5))
		p->timer = 0.0f;
	PROG_STEP(p, 9, p_wait(p, 2.0f))        // _DAT_0063158c
	PROG_STEP(p, 10, p_item_get_quiet(p, 0x23, 8))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x590730 EvProgDoctorMemo2nd
static int s18_doctor_memo_2(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "hsp_p_doctormemo"))
	PROG_STEP(p, 7, p_message(p, 5))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x5908c0 EvProgUseExamKey
static int s18_use_exam_key(Prog* p) { return p_item_use(p, 0x21, 9, 0x4a3b); }

// 0x5908e0 EvProgReadWhiteBoard: the pin memo "T", "X", "Z".
static int s18_white_board(Prog* p) { return p_picture_msg(p, "hsp_whiteboard", 6); }

// 0x5908f0 EvProgCardsOfPatient
static int s18_patient_cards(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_message(p, 3))
	PROG_STEP(p, 2, p_picture(p, "hsp_p_patient"))
	PROG_STEP(p, 7, p_message(p, 2))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x590ab0 EvProgHospitalExit: Maria asks where James is going.
static int s18_hospital_exit(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HOSPITAL_EXIT))
		p_place_keep_yaw(-19991.2f, 0.0f, -16315.49f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x590c20 EvProgLauraDeceive1st
static int s18_laura_deceive_1(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LAURA_DECEIVE_1))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage18[9] = {
	NULL, s18_get_hospital_map, s18_doctor_memo_1, s18_doctor_memo_2, s18_use_exam_key, s18_white_board,
	s18_patient_cards, s18_hospital_exit, s18_laura_deceive_1,
};

// ================================================================ stage 19 Stg_hospital_2f_f (0x8dbff8)

// 0x58fcb0 EvProgGetLapisEyeKey
static int s19_get_lapis_eye_key(Prog* p) { return p_item_get(p, 0x24, 0x10); }

// The box code as the carbon paper shows it: four digits 1..9 (0x1f7da72 nibbles, high first).
static const char* box_code_text(int msg)
{
	u16 v = *(u16*)(p_puzzle() + PZ_BOX_CODE);
	char code[16];
	snprintf(code, sizeof code, "%d%d%d%d", v >> 12 & 15, v >> 8 & 15, v >> 4 & 15, v & 15);
	ev_msg_set_insert(0, code);         // 0x58fe64
	return p_stage_text(msg);
}

// 0x58fcc0 EvProgReadCarbonNumber: the carbon paper in the typewriter; the riddle level decides
// which ramblings frame the number.
static int s19_carbon_number(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "hsp_p_carbon"))
		if (p_flag(183))
			PROG_JUMP(p, 0x1f);
	PROG_STEP(p, 0x1e, p_message(p, 7))
		p->step = 0x1f, p->sub = 0;
		// fallthrough
	case 0x1f:
		if (p->sub == 0) {
			p_se(0x4a4a);
			if (p_riddle() < 2) p_set(613);     // FLAGW_608 |= 0x20
			else p_set(183);                     // FLAGW_160 |= 0x800000
		}
		if (!p_message_text(p, box_code_text(p_riddle() < 2 ? 0x17 : 8)))
			return 0;
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x58ff50 EvProgGetExamKey: the pocket of the white lab jacket.
static int s19_get_exam_key(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(0x4a48);
	PROG_STEP(p, 10, p_message(p, 4))
		p->timer = 0.0f;
	PROG_STEP(p, 0x10, p_wait(p, 0.5f))     // _DAT_00638830
	PROG_STEP(p, 9, p_item_get_quiet(p, 0x21, 0xf))
		p_lock(p, false);
	PROG_END(p)
}

// 0x590090 EvProgNeedleInAnimal: the bent needle in the stuffed animal (Maria says "Ow!" with her).
static int s19_needle_in_animal(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_NEEDLE_ANIMAL))
	PROG_STEP(p, 0x12, p_item_get_quiet(p, 0x36, 0xe))
		p_lock(p, false);
	PROG_END(p)
}

// 0x5902a0 EvProgUseElevatorKey
static int s19_use_elevator_key(Prog* p) { return p_item_use(p, 0x25, 0x11, 0x4a41); }

// 0x5902c0 EvProgGetShotgun: the shotgun in the locker.
static int s19_get_shotgun(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 0xc))
	PROG_STEP(p, 9, p_item_get_quiet(p, 6, 0xd))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage19[7] = {
	NULL, s19_get_lapis_eye_key, s19_carbon_number, s19_get_exam_key, s19_needle_in_animal,
	s19_use_elevator_key, s19_get_shotgun,
};

// ================================================================ stage 20 Stg_hospital_3f_f (0x8db0e0)

// 0x58d7e0 EvProgLostMaria: Maria lies down in the day room; flag 187 picks her next line.
static int s20_lost_maria(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LOST_MARIA))
		p_place(100265.0f, 0.0f, 58701.0f, -1.8386f);
		p_set(186);                     // FLAGW_160 |= 0x4000000
		if (p_rand() & 1)
			p_set(187);
		p_lock(p, false);
	PROG_END(p)
}

// 0x58d990 EvProgLostMariaAfter: talking to her again alternates the two talks.
static int s20_lost_maria_after(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LOST_MARIA_AFTER))
		p_place(100230.5f, 0.0f, 58692.0f, -1.8386f);
		ev_flag_set(187, !p_flag(187));
		p_lock(p, false);
	PROG_END(p)
}

// 0x58db00 EvProgGetRoofKey
static int s20_get_roof_key(Prog* p) { return p_item_get(p, 0x22, 0x10); }

static const char* symbols_text(const u8* s)
{
	static char buf[64];
	snprintf(buf, sizeof buf, "\n   The symbols read:  %d  %d  %d  %d", s[0] + 1, s[1] + 1, s[2] + 1, s[3] + 1);
	return buf;
}

// 0x58f090 EvProgGuruguruNumber: the writing on the wall with the box's four symbols (the picture
// overlays them; the panel lists them as 1..9).
static int s20_guruguru_number(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		if (p_flag(192))
			PROG_JUMP(p, 2);
	PROG_STEP(p, 10, p_message(p, 0))
	PROG_STEP(p, 2, p_picture(p, "hsp_p_boxnumber"))
		p_panel(symbols_text(p_puzzle() + PZ_WHEEL_GOAL));
	PROG_STEP(p, 7, p_message(p, 1))
		p_panel(NULL);
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x58dd70 EvProgOnlyNeedle / 0x58dc60 EvProgOnlyHair (using one of them at the drain)
static int s20_only_needle(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0xf, p_message(p, 0xb))
		p_lock(p, false);
	PROG_END(p)
}

static int s20_only_hair(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0xf, p_message(p, 0xc))
		p_lock(p, false);
	PROG_END(p)
}

// 0x58de80 EvProgInShowerDrain
static int s20_in_shower_drain(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "hsp_p_drainage"))
	PROG_STEP(p, 0xf, p_message(p, p_flag(172) ? 9 : 8))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x58e0b0 EvProgFishKey: hair and needle fish the elevator key out of the drain.
static int s20_fish_key(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_FISH_KEY))
	PROG_STEP(p, 9, p_item_get_quiet(p, 0x25, 0xf))
		p_lock(p, false);
	PROG_END(p)
}

// The box with four locks (0x58f2d0 EvProgBoxWithKey, cursor 0x58e600): the "Purple Bull" and
// "Lapis Eye" keys (flags 193, 195), four symbol wheels that must match the wall (197, only once
// flag 203 is set) and nine buttons whose last four presses must be the carbon paper's number
// (198). Flags 194 / 196 say a key is being used right now.
static bool box_open(void) { return p_flag(193) && p_flag(195) && p_flag(197) && p_flag(198); }

static void box_check(Prog* p)
{
	u8* z = p_puzzle();
	u16 code = *(u16*)(z + PZ_BOX_CODE);
	const u8* last = (const u8*)&p->work[2];        // 0x1d864bc: last four buttons, oldest first
	if (!p_flag(198) && p->work[3] >= 4 && (code >> 12) == last[0] + 1u && (code >> 8 & 15) == last[1] + 1u &&
	    (code >> 4 & 15) == last[2] + 1u && (code & 15) == last[3] + 1u) {
		p_set(198);                     // FLAGW_192 |= 0x40
		p_se(0x4a56);
	}
	if (!p_flag(197) && p_flag(203) && !memcmp(z + PZ_WHEEL_GOAL, z + PZ_WHEEL, 4)) {
		p_set(197);                     // FLAGW_192 |= 0x20
		p_se(0x4a46);
	}
}

static void box_panel(Prog* p)
{
	u8* w = p_puzzle() + PZ_WHEEL;
	const u8* last = (const u8*)&p->work[2];
	int cur = p->work[1];
	char buf[480];
	int n = snprintf(buf, sizeof buf, " Purple Bull %-6s  Lapis Eye %s\n Wheels      %-6s  Buttons   %s\n\n Wheels ",
	                 p_flag(193) ? "open" : "locked", p_flag(195) ? "open" : "locked",
	                 p_flag(197) ? "open" : "locked", p_flag(198) ? "open" : "locked");
	for (int i = 0; i < 4; i++)
		n += snprintf(buf + n, sizeof buf - n, "%c%d%c", cur == i ? '[' : ' ', w[i] + 1, cur == i ? ']' : ' ');
	n += snprintf(buf + n, sizeof buf - n, "\n Buttons ");
	for (int i = 0; i < 9; i++)
		n += snprintf(buf + n, sizeof buf - n, "%c%d%c", cur == 4 + i ? '[' : ' ', i + 1, cur == 4 + i ? ']' : ' ');
	n += snprintf(buf + n, sizeof buf - n, "\n Pressed ");
	for (int i = 4 - (p->work[3] < 4 ? p->work[3] : 4); i < 4; i++)
		n += snprintf(buf + n, sizeof buf - n, " %d", last[i] + 1);
	snprintf(buf + n, sizeof buf - n, "\n\n Left/Right: pick  Up/Down: turn\n A: press  B: leave");
	p_panel(buf);
}

static int s20_box_with_key(Prog* p)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p->work[2] = 0;
		p->work[3] = 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("hsp_p_box");
		if (!p_flag(194) && !p_flag(196))
			PROG_JUMP(p, 7);
		p->step = 9, p->sub = 0;
		// fallthrough
	case 9:                         // the keys being used: the "Lapis Eye" first, then the bull
		if (p_flag(196)) {
			if (!p_item_use(p, 0x24, 0x13, 0x4a4d))
				return 0;
			p_clear(196);
			p_set(195);
			p_se(0x4dbb);
			p->sub = 0;
			return 0;
		}
		if (p_flag(194)) {
			if (!p_item_use(p, 0x23, 0x12, 0x4a47))
				return 0;
			p_clear(194);
			p_set(193);
			p->sub = 0;
			return 0;
		}
		if (box_open())
			PROG_JUMP(p, 0xf);
		p->step = 7, p->sub = 0;
		// fallthrough
	case 7: {
		u8* w = p_puzzle() + PZ_WHEEL;
		int cur = p->work[1];
		if (p->down & KEY_DLEFT) p->work[1] = (cur + 12) % 13;
		if (p->down & KEY_DRIGHT) p->work[1] = (cur + 1) % 13;
		cur = p->work[1];
		if (cur < 4 && !p_flag(197)) {
			if (p->down & (KEY_DUP | KEY_A)) { w[cur] = w[cur] >= 8 ? 0 : w[cur] + 1; p_se(0x4a45); }
			if (p->down & KEY_DDOWN) { w[cur] = w[cur] == 0 ? 8 : w[cur] - 1; p_se(0x4a45); }
		}
		if (cur >= 4 && !p_flag(198) && (p->down & KEY_A)) {
			u8* last = (u8*)&p->work[2];
			memmove(last, last + 1, 3);
			last[3] = (u8)(cur - 4);
			if (p->work[3] < 4) p->work[3]++;
			p_se(0x4a4c);
		}
		box_check(p);
		box_panel(p);
		if (box_open()) {
			p_panel(NULL);
			PROG_JUMP(p, 0xf);
		}
		if (!(p->down & KEY_B))
			return 0;
		p_panel(NULL);
		PROG_JUMP(p, 0xd);
	}
	case 0xf:
		if (p->sub == 0)
			p_se(0x4dba);
		if (!p_picture(p, "hsp_p_hair"))
			return 0;
		p->step = 10, p->sub = 0;
		// fallthrough
	case 10:
		if (!p_message(p, 5))
			return 0;
		p->step = 0x12, p->sub = 0;
		// fallthrough
	case 0x12:
		if (!p_item_get_quiet(p, 0x35, 0xe))
			return 0;
		p_set(171);                     // FLAGW_160 |= 0x800
		// fallthrough
	case 0xd:
	default: {
		// leaving with the wheels right but flag 203 not set yet turns one wheel away again
		u8* z = p_puzzle();
		if (!p_flag(197) && !memcmp(z + PZ_WHEEL_GOAL, z + PZ_WHEEL, 4)) {
			int i = p_rand() & 3, v = p_rand() % 7;
			z[PZ_WHEEL + i] = (u8)(v == z[PZ_WHEEL_GOAL + i] ? v + 1 : v);
		}
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
	}
}

// 0x58ebd0 EvProgEmptyBox
static int s20_empty_box(Prog* p) { return p_picture_msg(p, "hsp_p_hair", 6); }

// 0x58fae0 EvProgSetPassNumberT (keypad 0x58ec10): the pin "T" on the 3F keypad, 7 3 3 5 on easy
// and normal, 1 3 2 8 on hard and extra; flag 181 opens the door.
static int s20_pass_number_t(Prog* p)
{
	static const u8 pin[2][4] = { { 7, 3, 3, 5 }, { 1, 3, 2, 8 } };
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p->work[2] = 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("hsp_p_panel");
		p->step = 8, p->sub = 0;
		// fallthrough
	case 8: {
		u8* last = (u8*)&p->work[2];            // 0x1d864c8: last four keys, 1..9
		if (p->down & KEY_DLEFT) p->work[1] = (p->work[1] + 8) % 9;
		if (p->down & KEY_DRIGHT) p->work[1] = (p->work[1] + 1) % 9;
		if (p->down & KEY_DUP) p->work[1] = (p->work[1] + 6) % 9;
		if (p->down & KEY_DDOWN) p->work[1] = (p->work[1] + 3) % 9;
		if (p->down & KEY_A) {
			memmove(last, last + 1, 3);
			last[3] = (u8)(p->work[1] + 1);
			p_se(0x4a55);
			if (!memcmp(last, pin[p_riddle() >= 2], 4)) {
				p_set(181);             // FLAGW_160 |= 0x200000
				p_se(0x4a41);
			}
		}
		char buf[200];
		int n = snprintf(buf, sizeof buf, "\n");
		for (int r = 0; r < 3; r++) {
			n += snprintf(buf + n, sizeof buf - n, "        ");
			for (int c = 0; c < 3; c++) {
				int k = r * 3 + c;
				n += snprintf(buf + n, sizeof buf - n, "%c%d%c ", p->work[1] == k ? '[' : ' ', k + 1, p->work[1] == k ? ']' : ' ');
			}
			n += snprintf(buf + n, sizeof buf - n, "\n");
		}
		snprintf(buf + n, sizeof buf - n, "\n\n\n   D-pad: key  A: press  B: leave");
		p_panel(buf);
		if (!(p->down & KEY_B) && !p_flag(181))
			return 0;
		// fallthrough
	}
	default:
		p_panel(NULL);
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x58ed40 EvProgLouiseTakecare: "Louise, I'll take care of you four ever."
static int s20_louise_takecare(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		if (p_flag(191))
			PROG_JUMP(p, 0xc);
	PROG_STEP(p, 0xb, p_message(p, 0))
	PROG_STEP(p, 0xc, p_message(p, 7))
		p_lock(p, false);
	PROG_END(p)
}

// 0x58ee40 EvProgUseElevatorKey
static int s20_use_elevator_key(Prog* p) { return p_item_use(p, 0x25, 0x11, 0x4a41); }

// 0x58ee60 EvProgFromRoof: James falls through the roof fence.
static int s20_from_roof(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_FROM_ROOF))
		p_place(21156.65f, 0.0f, 99800.0f, 3.1415927f);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage20[15] = {
	NULL, s20_lost_maria, s20_lost_maria_after, s20_get_roof_key, s20_guruguru_number, s20_only_needle,
	s20_only_hair, s20_in_shower_drain, s20_fish_key, s20_box_with_key, s20_empty_box, s20_pass_number_t,
	s20_louise_takecare, s20_use_elevator_key, s20_from_roof,
};

// ================================================================ stage 21 Stg_hospital_rf_f (0x8da750)

// 0x58d480 EvProgElevatorButton (front side)
static int s21_elevator(Prog* p) { return elevator(p, false); }

// 0x58cc80 EvProgUseRoofKey
static int s21_use_roof_key(Prog* p) { return p_item_use(p, 0x22, 6, 0x4a51); }

// 0x58cca0 EvProgTriheadOnRoof: Pyramid Head pushes James through the fence (and hurts him).
static int s21_trihead_on_roof(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_ROOF_TRIHEAD))
		p_clear(200);                   // FLAGW_192 &= ~0x100
		p_lock(p, false);
	PROG_END(p)
}

// 0x58cea0 EvProgTriheadEntry: puts Pyramid Head on the roof.
static int s21_trihead_entry(Prog* p) { (void)p; return 1; }

// 0x58ced0 EvProgReadDiary: the diary on the roof.
static int s21_read_diary(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_message(p, 0))
	PROG_STEP(p, 0x11, p_picture(p, "hsp_p_diary"))
	PROG_STEP(p, 10, p_message(p, 1))
		p_picture_hide();
	PROG_STEP(p, 0x1f, p_message(p, 2))
		p_lock(p, false);
	PROG_END(p)
}

// 0x58d1c0 EvProgMetalNetRusty
static int s21_metal_net_rusty(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(0x4a4b);
	PROG_STEP(p, 10, p_message(p, 4))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage21[7] = {
	NULL, s21_elevator, s21_use_roof_key, s21_trihead_on_roof, s21_trihead_entry, s21_read_diary,
	s21_metal_net_rusty,
};

// ================================================================ stage 22 Stg_hospital_1fw_b (0x8d9cd8)

// 0x58b670 EvProgLauraDeceive2nd / 0x58b750 EvProgLauraDeceive3rd: Laura locks James in room S3.
static int s22_laura_deceive_2(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LAURA_DECEIVE_2))
		p_lock(p, false);
	PROG_END(p)
}

static int s22_laura_deceive_3(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LAURA_DECEIVE_3))
		p_set(521);                     // FLAGW_512 |= 0x200
		p_se(0x2716);
		p_lock(p, false);
	PROG_END(p)
}

// 0x58b910 EvProgHospitalBack1st / 0x58ba80 EvProgHospitalBack2nd: the hospital turns.
static int s22_hospital_back_1(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HOSPITAL_BACK_1))
		p_set(213);                     // FLAGW_192 |= 0x200000 during the demo
		p_set(215);                     // FLAGW_192 |= 0x800000
		p_clear(521);
		p_lock(p, false);
	PROG_END(p)
}

static int s22_hospital_back_2(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HOSPITAL_BACK_2))
		p_set(214);                     // FLAGW_192 |= 0x400000
		p_place(-68146.0f, 0.0f, -299992.06f, -1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x58c4b0 (DC): the blue gem in the courtyard, the UFO ending's scene.
static int s22_ufo(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 1, p_demo(p, DEMO_UFO_HOSPITAL))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage22[6] = {
	NULL, s22_laura_deceive_2, s22_laura_deceive_3, s22_hospital_back_1, s22_hospital_back_2, s22_ufo,
};

// ================================================================ stage 23 Stg_hospital_1fe_b (0x8d9730)

// 0x58af50 EvProgMariaKilledAfter: the elevator down after Maria's death.
static int s23_maria_killed_after(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_ELEVATOR_DOWN))
		p_place(-19780.6f, 0.0f, -222189.2f, 1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x58b040 EvProgUseHospitalEnterKey
static int s23_use_lobby_key(Prog* p) { return p_item_use(p, 0x27, 1, 0x4a41); }

// 0x58b060 EvProgGetHospitalEnterKey: the town map in the Director's office with the lobby key;
// James marks the map, takes the key and leaves (flag 251: the town after the hospital).
static int s23_get_lobby_key(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "hsp_p_musiuminfo"))
	PROG_STEP(p, 10, p_message(p, 4))
		p_picture_hide();
	PROG_STEP(p, 7, p_map_mark(p, 0))          // the marks on both town maps
	PROG_STEP(p, 0x1e, p_message(p, 3))
	PROG_STEP(p, 0x24, p_item_get_quiet(p, 0x27, 2))
		p_picture_hide();
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HOSPITAL_EXIT_LOBBY))
		p_place(-20100.0f, 0.0f, -259683.9f, 0.9340296f);
		p_set(1514);                    // FLAGW_1504 |= 0x301400
		p_set(1516);
		p_set(1524);
		p_set(1525);
		p_set(251);                     // FLAGW_224 |= 0x8000000
		p_lock(p, false);
	PROG_END(p)
}

// 0x58b660 EvProgLookDirectorMap
static int s23_look_director_map(Prog* p) { return p_picture_msg(p, "hsp_p_musiuminfo", 4); }

static const ProgFn s_stage23[5] = {
	NULL, s23_maria_killed_after, s23_use_lobby_key, s23_get_lobby_key, s23_look_director_map,
};

// ================================================================ stage 24 Stg_hospital_2f_b (0x8d90e8)

// 0x58a8a0 EvProgRefrigeratorOpen: with Maria, the lead ring.
static int s24_refrigerator_open(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_question(p, 0))
		if (p->answer) {
			p_set(1956);                // FLAGW_1952 |= 0x10
			PROG_JUMP(p, 0xd);
		}
		if (p_flag(1956))
			p_set(1957);
	PROG_STEP(p, 0x18, p_demo(p, DEMO_REFRIGERATOR))
	PROG_STEP(p, 0x1f, p_item_get_quiet(p, 0x39, 5))
		p_place(-20400.0f, 0.0f, -182719.8f, -2.4099486f);
		p_set(219);                     // FLAGW_192 |= 0x8000000
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x58ac20 EvProgRefrigeratorNotOpen: alone, the door is too heavy.
static int s24_refrigerator_not_open(Prog* p)
{
	PROG_BEGIN(p)
		p_set(1956);
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_question(p, 0))
		if (p->answer)
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x18, p_demo(p, DEMO_REFRIGERATOR_ALONE))
	PROG_STEP(p, 0x1f, p_message(p, 2))
		p_place(-20418.6f, 0.0f, -183079.05f, -1.7589257f);
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x58aed0 EvProgGetDryCell / 0x58aee0 EvProgGetStoreroomKey
static int s24_get_dry_cell(Prog* p) { return p_item_get(p, 0x37, 3); }
static int s24_get_storeroom_key(Prog* p) { return p_item_get(p, 0x26, 4); }

static const ProgFn s_stage24[5] = {
	NULL, s24_refrigerator_open, s24_refrigerator_not_open, s24_get_dry_cell, s24_get_storeroom_key,
};

// ================================================================ stage 25 Stg_hospital_3f_b (0x8d8b38)

// 0x58a260 EvProgPictureOfAngel: the painted lady on the door; the copper and the lead ring go on
// her hands (flags 227, 228; 229 / 230 say which ring is being used now).
static int s25_picture_of_angel(Prog* p)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		if (p_flag(229) || p_flag(230))
			PROG_JUMP(p, 2);
		p_set(1948);                    // FLAGW_1920 |= 0x10000000
		p_se(0x4a53);
		p->step = 0x1e, p->sub = 0;
		// fallthrough
	case 0x1e:
		if (!p_system_message(p, 4))
			return 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		if (!p_picture(p, "hsp_p_female"))
			return 0;
		p->step = 0x21, p->sub = 0;
		// fallthrough
	case 0x21:
		if (p_flag(229) && !p_flag(227)) {
			if (!p_item_use(p, 0x38, 2, 0))
				return 0;
			p_set(227);                 // FLAGW_224 |= 8
			if (p_flag(228)) p_se(0x4a51);
		}
		p->step = 0x22, p->sub = 0;
		// fallthrough
	case 0x22:
		if (p_flag(230) && !p_flag(228)) {
			if (!p_item_use(p, 0x39, 3, 0))
				return 0;
			p_set(228);                 // FLAGW_224 |= 0x10
			if (p_flag(227)) p_se(0x4a51);
		}
		if (p_flag(229) || p_flag(230))
			PROG_JUMP(p, 0xd);
		p->step = 0x1f, p->sub = 0;
		// fallthrough
	case 0x1f:
		if (!p_message(p, 5))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_clear(229);
		p_clear(230);
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x589790 EvProgAngelNoOneKnows: the scrap of paper (the long version the first time).
static int s25_angel_no_one_knows(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0xf, p_message(p, p_flag(231) ? 1 : 0))
		p_lock(p, false);
	PROG_END(p)
}

// 0x58a6e0 EvProgTryQuizAnswer (logic 0x589820): three rows of three buttons. Each row's answer
// is two flags (234/235, 236/237, 238/239: 1 0 first, 0 1 second, 1 1 third). Third, first, third
// is right (flag 240), any other full answer is wrong (241); both end the quiz (233).
static int s25_try_quiz(Prog* p)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p->work[2] = 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("hsp_p_quiz");
		p->step = 8, p->sub = 0;
		// fallthrough
	case 8: {
		if (p->down & KEY_DUP) p->work[1] = (p->work[1] + 2) % 3;
		if (p->down & KEY_DDOWN) p->work[1] = (p->work[1] + 1) % 3;
		if (p->down & KEY_DLEFT) p->work[2] = (p->work[2] + 2) % 3;
		if (p->down & KEY_DRIGHT) p->work[2] = (p->work[2] + 1) % 3;
		if (p->down & KEY_A) {
			int row = p->work[1], ans = p->work[2] + 1, f = 234 + 2 * row;
			p_se(0x426f);
			ev_flag_set(f, ans & 1);
			ev_flag_set(f + 1, ans >> 1 & 1);
		}
		char buf[300];
		int n = snprintf(buf, sizeof buf, "\n");
		for (int r = 0; r < 3; r++) {
			int a = p_flag(234 + 2 * r) | p_flag(235 + 2 * r) << 1;
			n += snprintf(buf + n, sizeof buf - n, "  %c Row %d  ", p->work[1] == r ? '>' : ' ', r + 1);
			for (int c = 0; c < 3; c++)
				n += snprintf(buf + n, sizeof buf - n, "%c%c%c ", p->work[1] == r && p->work[2] == c ? '[' : ' ',
				              a == c + 1 ? '*' : 'o', p->work[1] == r && p->work[2] == c ? ']' : ' ');
			n += snprintf(buf + n, sizeof buf - n, "\n");
		}
		snprintf(buf + n, sizeof buf - n, "\n\n\n  D-pad: button  A: press  B: leave");
		p_panel(buf);
		bool all = (p_flag(234) || p_flag(235)) && (p_flag(236) || p_flag(237)) && (p_flag(238) || p_flag(239));
		bool right = p_flag(234) && p_flag(235) && p_flag(236) && !p_flag(237) && p_flag(238) && p_flag(239);
		if (right) {
			p_set(240);                 // FLAGW_224 |= 0x10200
			p_set(233);
			p_se(0x4db6);
		} else if (all) {
			p_set(241);                 // FLAGW_224 |= 0x20200
			p_set(233);
			p_se(0x4db7);
		}
		if (!all && !(p->down & KEY_B))
			return 0;
		// fallthrough
	}
	default:
		p_panel(NULL);
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x589f70 EvProgIncorrectQuizAnswer
static int s25_incorrect_quiz(Prog* p) { return p_picture_msg(p, "hsp_p_quiz", 6); }

static const ProgFn s_stage25[5] = {
	NULL, s25_picture_of_angel, s25_angel_no_one_knows, s25_try_quiz, s25_incorrect_quiz,
};

// ================================================================ stage 26 Stg_hospital_bf_b (0x8d8070)

// 0x5881a0 EvProgMariaMeetAgain: movie saikai.sfd, Maria is back (flag 15).
static int s26_maria_meet_again(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(19000);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARIA_MEET_AGAIN))
		p_place(19814.75f, 0.0f, -220911.1f, -1.718694f);
		p_set(15);                      // gGameFlag |= 0x8000
		p_lock(p, false);
	PROG_END(p)
}

// 0x5883f0 EvProgMoveEmptyShelf: the shelf with red handprints hides a ladder down.
static int s26_move_empty_shelf(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1f, p_question(p, 2))
		if (p->answer)
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EMPTY_SHELF))
		p_place_keep_yaw(19722.336f, 0.0f, -220675.05f);
		p_set(223);                     // FLAGW_192 |= 0x80000000
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x5885c0 EvProgGetCopperRing / 0x5885d0 EvProgUseStoreroomKey
static int s26_get_copper_ring(Prog* p) { return p_item_get(p, 0x38, 5); }
static int s26_use_storeroom_key(Prog* p) { return p_item_use(p, 0x26, 6, 0x4a3b); }

// 0x588e60 EvProgElevatorButton (otherworld)
static int s26_elevator(Prog* p) { return elevator(p, true); }

// 0x588a50 EvProgDirectorBronzeKey: the note about the museum key behind the praying woman.
static int s26_director_bronze_key(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0xf, p_message(p, p_flag(242) ? 3 : 4))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage26[7] = {
	NULL, s26_maria_meet_again, s26_move_empty_shelf, s26_get_copper_ring, s26_use_storeroom_key,
	s26_elevator, s26_director_bronze_key,
};

// ================================================================ stage 27 Stg_hospital_pass (0x8d77f0)

// 0x587db0 .. 0x587ed0 EvProgChaseInHospital1st..5th: set up and steer the chaser; the port has
// no enemies yet, so only the events' flags remain.
static int s27_chase(Prog* p) { (void)p; return 1; }

// 0x587f10 EvProgMariaKilled
static int s27_maria_killed(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x17, p_demo(p, DEMO_MARIA_KILLED))
		p_set(522);                     // FLAGW_512 |= 0x400
		p_clear(15);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage27[7] = { NULL, s27_chase, s27_chase, s27_chase, s27_chase, s27_chase, s27_maria_killed };

const ProgStage prog_hospital_stages[] = {
	{ 4, s_stage15 }, { 4, s_stage16 }, { 1, NULL }, { 9, s_stage18 }, { 7, s_stage19 }, { 15, s_stage20 },
	{ 7, s_stage21 }, { 6, s_stage22 }, { 5, s_stage23 }, { 5, s_stage24 }, { 5, s_stage25 }, { 7, s_stage26 },
	{ 7, s_stage27 },
};
