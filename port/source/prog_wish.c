#include "prog_int.h"

// Stage programs of Born From a Wish (stages 53..58, Director's Cut only): Maria in the Baldwin
// mansion and Blue Creek. Flags, items, messages and SE ids are the PC's. The PS2 beta has no
// names for these stages; the program names below follow the hotel's EvProg naming. Stages 57 and
// 58 run code that the PC keeps encrypted on disk (SecuROM), so those programs are reconstructed
// from their events and messages, and say so.

static void wish_end(Prog* p)
{
	p_lock(p, false);
}

// ================================================================ stage 53 (0x8c89d8)

// 0x5740c0 EvProgWishStart (auto, flag 668). The PC first shows the "additional scenario" notice
// (message 16) with Yes / No (messages 0 and 1) unless 0x1dbeb85 in the system data says it was
// seen; No goes back to the title. Then the opening scene, and Maria stands at the bar's door.
static int s53_wish_start(Prog* p)
{
	switch (p->step) {
	case 0: {
		const char* labels[2] = { ev_stage_message(0), ev_stage_message(1) };
		if (!labels[0]) labels[0] = "Yes.";
		if (!labels[1]) labels[1] = "No.";
		p_lock(p, true);
		if (!p_choice(p, 16, labels, 2))
			return 0;
		if (p->answer) {
			wish_end(p);
			p_game_end(0);
			return 1;
		}
		p->step = 0x16, p->sub = 0;
	}
		// fallthrough
	case 0x16:
		if (!p_demo(p, DEMO_WISH_START))
			return 0;
		p_place(59622.3f, 0.0f, 20308.75f, -1.5707964f);
		wish_end(p);
		// fallthrough
	default:
		return 1;
	}
}

// 0x574590 EvProgGetChineseCleaver
static int s53_get_cleaver(Prog* p) { return p_item_get(p, 0x11, 0xc); }

static const ProgFn s_stage53[3] = { NULL, s53_wish_start, s53_get_cleaver };

// ================================================================ stage 54 (0x8c7c60)

// 0x573a60 EvProgEndWish (auto, flag 721): the ending with James, movie end_wish.sfd, the
// credits; flag 728 and the clear bit 0x40 (FUN_0048d7e0).
static int s54_end_wish(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_END_WISH))
		p_set(728);                     // FLAGW_704 |= 0x1000000
		wish_end(p);
		p_game_end(0x40);
	PROG_END(p)
}

static const ProgFn s_stage54[2] = { NULL, s54_end_wish };

// ================================================================ stage 55 (0x8c6d10)

// 0x571cb0 EvProgGetBaldwinMap
static int s55_get_map(Prog* p) { return p_map_get(p, "add_mapboldwin", 0x48); }

// 0x571cc0 EvProgChimney: the ladder in the fireplace; yes sets 674 and the stage's door event
// takes Maria up.
static int s55_chimney(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "add_p_entotsu"))
	PROG_STEP(p, 10, p_question(p, 0x4a))
		if (!p->answer)
			p_set(674);                 // FLAGW_672 |= 4
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x571ea0 EvProgChimneyDown
static int s55_chimney_down(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_question(p, 0x4b))
		if (!p->answer)
			p_set(675);                 // FLAGW_672 |= 8
		p_lock(p, false);
	PROG_END(p)
}

// 0x571f30 EvProgGetBlackBoard / 0x571f40 EvProgGetRedBoard
static int s55_get_black_board(Prog* p) { return p_item_get(p, 0x50, 0x51); }
static int s55_get_red_board(Prog* p) { return p_item_get(p, 0x51, 0x52); }

// The tombstone (0x572f10 EvProgBoardPuzzle): three boards with holes (white 79, black 80,
// red 81) go into the square depression, each turned one of four ways. Per board b (white,
// black, red) the flags are: 678 + 9b "being put in" (from the use event), 679 + 9b + s "in slot
// s", 682 + 9b + r "turned r" (as is, right, 180, left). The first board goes in as it is; when
// all three are in and the holes line up (white and black turned the same way, red one quarter
// back) the Acacia key falls out (704).
static const short s_board_item[3] = { 0x4f, 0x50, 0x51 };

static int board_slot(int b)
{
	for (int s = 0; s < 3; s++)
		if (p_flag(679 + 9 * b + s))
			return s;
	return -1;
}

static int board_turn(int b)
{
	for (int r = 0; r < 4; r++)
		if (p_flag(682 + 9 * b + r))
			return r;
	return -1;
}

static bool boards_line_up(void)
{
	int w = board_turn(0), k = board_turn(1), r = board_turn(2);
	return w >= 0 && w == k && r == (w + 3) % 4;
}

static int s55_board_puzzle(Prog* p)
{
	static const char* const turns[4] = {
		"Insert as is.", "Turn 90 degrees to the right and insert.", "Turn 180 degrees and insert.",
		"Turn 90 degrees to the left and insert.",
	};
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p_picture_show("add_p_teien");
		p->work[1] = -1;
		for (int b = 0; b < 3; b++)
			if (p_flag(678 + 9 * b))
				p->work[1] = b;         // the board being put in
		if (p->work[1] < 0)
			PROG_JUMP(p, 8);
		// the lowest free slot (the PC checks slot 1 before slot 0)
		bool used1 = p_flag(680) || p_flag(689) || p_flag(698);
		bool used0 = p_flag(679) || p_flag(688) || p_flag(697);
		p->work[2] = used1 ? 2 : used0 ? 1 : 0;
		p_set(679 + 9 * p->work[1] + p->work[2]);
		if (p->work[2] == 0) {
			p_set(682 + 9 * p->work[1]);    // the first board goes in as it is
			PROG_JUMP(p, 0x1b);
		}
		PROG_JUMP(p, 0x11);
	case 0x11:                      // which way round (message 0x5b holds the four answers)
		if (!p_choice(p, -1, turns, 4))
			return 0;
		p_set(682 + 9 * p->work[1] + p->answer);
		p->step = 0x1b, p->sub = 0;
		// fallthrough
	case 0x1b:
		if (p->sub == 0) {
			p_se(0x4291);
			p->sub = 1;
		}
		if (!p_message(p, 0x5c + p->work[1]))   // "I put the ... board into the hole."
			return 0;
		PROG_JUMP(p, 8);
	case 8:
		if (!(p->down & (KEY_A | KEY_B)))   // the still stays up until a button
			return 0;
		if (p_flag(704))
			PROG_JUMP(p, 0x1f);
		if (p_flag(681) || p_flag(690) || p_flag(699)) {     // all three are in
			if (boards_line_up())
				PROG_JUMP(p, 9);
			PROG_JUMP(p, 0x14);
		}
		if (p->work[1] >= 0)
			PROG_JUMP(p, 0xd);
		if (board_slot(0) < 0 && board_slot(1) < 0 && board_slot(2) < 0)
			PROG_JUMP(p, 0x1e);
		PROG_JUMP(p, 0x20);
	case 9:
		if (!p_message(p, 0x57))
			return 0;
		p_se(0x4292);
		p_set(704);                     // FLAGW_704 |= 1
		p->step = 0x12, p->sub = 0;
		// fallthrough
	case 0x12:
		if (!p_item_get_quiet(p, 0x52, 0x53))
			return 0;
		PROG_JUMP(p, 0xd);
	case 0x14:                      // the holes overlap: take one out?
		if (!p_question(p, 0x4e))
			return 0;
		if (p->answer)
			PROG_JUMP(p, 0xd);
		p->work[1] = -1;
		p_clear(678);
		p_clear(687);
		p_clear(696);
		PROG_JUMP(p, 0x1c);
	case 0x20:
		if (!p_question(p, 0x4d))
			return 0;
		if (p->answer)
			PROG_JUMP(p, 0xd);
		PROG_JUMP(p, 0x1c);
	case 0x1c: {                    // the board in the highest slot comes out (white first)
		p_se(0x4290);
		int out = 2;                    // nothing found: the PC falls through to red in slot 0
		for (int s = 2; s >= 0; s--) {
			if (p_flag(679 + s)) { out = 0; p_clear(679 + s); break; }
			if (p_flag(688 + s)) { out = 1; p_clear(688 + s); break; }
			if (p_flag(697 + s)) { out = 2; p_clear(697 + s); break; }
		}
		for (int r = 0; r < 4; r++)
			p_clear(682 + 9 * out + r);
		p_item_get_now(s_board_item[out]);
		p->work[2] = out;
		p->step = 0x17, p->sub = 0;
	}
		// fallthrough
	case 0x17:
		if (!p_message(p, 0x58 + p->work[2]))   // "I removed the ... board."
			return 0;
		PROG_JUMP(p, 8);
	case 0x1e:
		if (!p_message(p, 0x4c))
			return 0;
		p_set(676);                     // FLAGW_672 |= 0x10
		PROG_JUMP(p, 0xd);
	case 0x1f:
		if (!p_message(p, 0x4f))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_clear(678);                   // FLAGW_672 &= 0xfeff7fbf
		p_clear(687);
		p_clear(696);
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x5725f0 EvProgLostMemories: a sound in the dark, then the book.
static int s55_lost_memories(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(0x428f);
	PROG_STEP(p, 10, p_message(p, 0x50))
		p_lock(p, false);
	PROG_END(p)
}

// 0x572800 EvProgErnestCard: Maria gives Ernest the birthday card.
static int s55_ernest_card(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_WISH_CARD))
		p_place(-18738.05f, 0.0f, 20767.3f, 0.7853982f);
		p_item_lose(0x54);
		wish_end(p);
	PROG_END(p)
}

// 0x572920 EvProgErnest3
static int s55_ernest_3(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_WISH_ERNEST_3))
		wish_end(p);
	PROG_END(p)
}

// 0x572a00 EvProgErnestLiquid: the white liquid; flag 722 partway through the scene.
static int s55_ernest_liquid(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_WISH_LIQUID))
		p_set(722);                     // FLAGW_704 |= 0x40000
		wish_end(p);
	PROG_END(p)
}

static const ProgFn s_stage55[11] = {
	NULL, s55_get_map, s55_chimney, s55_chimney_down, s55_get_black_board, s55_get_red_board,
	s55_board_puzzle, s55_lost_memories, s55_ernest_card, s55_ernest_3, s55_ernest_liquid,
};

// ================================================================ stage 56 (0x8c5f60)

// 0x571120 / 0x571220: Ernest behind the door, first and later talks.
static int s56_ernest_1(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_WISH_ERNEST_1))
		p_place(18031.8f, -2000.0f, 58766.5f, 1.8526269f);
		p_set(2436);                    // FLAGW_2432 |= 0x10
		wish_end(p);
	PROG_END(p)
}

static int s56_ernest_2(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_WISH_ERNEST_2))
		p_place(17853.1f, -2000.0f, 58846.5f, -2.3561945f);
		wish_end(p);
	PROG_END(p)
}

// 0x5712f0 EvProgGetWhiteBoard / 0x571300 EvProgUseAcaciaKey / 0x571320 EvProgGetMatches
static int s56_get_white_board(Prog* p) { return p_item_get(p, 0x4f, 0x20); }
static int s56_use_acacia_key(Prog* p) { return p_item_use(p, 0x52, 0x21, 0x4a3b); }
static int s56_get_matches(Prog* p) { return p_item_get(p, 0x53, 0x22); }

// 0x571330 EvProgLightCandle: the matches on the candle; flag 710 lights the room and Maria
// stands by the candle.
static int s56_light_candle(Prog* p)
{
	(void)p;
	p_se(0x4293);
	p_set(710);                         // FLAGW_704 |= 0x40
	p_place(58057.3f, -2000.0f, -17676.0f, 1.5707964f);
	return 1;
}

// 0x571370 EvProgGetBirthdayCard: under the chair (Maria crouches, 0x4e21).
static int s56_birthday_card(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "add_p_birthday"))
	PROG_STEP(p, 10, p_message(p, 0x24))
		p_picture_hide();
	PROG_STEP(p, 9, p_item_get_quiet(p, 0x54, 0x25))
		p_lock(p, false);
	PROG_END(p)
}

// 0x5715e0 EvProgCalendar: a sound in the dark, then the calendar.
static int s56_calendar(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(0x428f);
	PROG_STEP(p, 10, p_message(p, 0x26))
		p_lock(p, false);
	PROG_END(p)
}

// 0x5717f0 / 0x571810: sounds on entering (0x428d at the door, 0x428e at (20000, 0, 56700)).
static int s56_sound_1(Prog* p) { (void)p; p_se(0x428d); return 1; }
static int s56_sound_2(Prog* p) { (void)p; p_se(0x428e); return 1; }

// 0x571860 / 0x571880: BGM changes (FUN_0049fe30 0xeaf1, 0xeaf2); bgm.c follows the stage.
static int s56_bgm(Prog* p) { (void)p; return 1; }

// 0x5718a0: BGM 0xeaf3 and the camera hp246 for three seconds (_DAT_0062ede0).
static int s56_music_room(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p->timer = 0.0f;
	PROG_STEP(p, 10, p_wait(p, 3.0f) || (p->down & (KEY_A | KEY_B | KEY_START)))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage56[14] = {
	NULL, s56_ernest_1, s56_ernest_2, s56_get_white_board, s56_use_acacia_key, s56_get_matches,
	s56_light_candle, s56_birthday_card, s56_calendar, s56_sound_1, s56_sound_2, s56_bgm, s56_bgm,
	s56_music_room,
};

// ================================================================ stage 57 (0x8c5460)

// 0x570f70 (encrypted, reconstructed): two look events (7 and 8, ma77) a few units apart share
// the program; the stage's messages are Amy's grave plate (0) and the "Keep out" sign (1). The
// port shows the one whose event point is nearer to Maria.
static int s57_plate(Prog* p)
{
	if (p->step == 0) {
		float pos[3], yaw;
		p_player(pos, &yaw);
		float d7 = (pos[0] - 58479.26f) * (pos[0] - 58479.26f) + (pos[2] + 89000.31f) * (pos[2] + 89000.31f);
		float d8 = (pos[0] - 58457.7f) * (pos[0] - 58457.7f) + (pos[2] + 89038.66f) * (pos[2] + 89038.66f);
		p->work[1] = d8 < d7;
		p->step = 10;
	}
	return p_message(p, p->work[1]);
}

static const ProgFn s_stage57[2] = { NULL, s57_plate };

// ================================================================ stage 58 (0x8c4f90)

// 0x570e40 (encrypted, reconstructed): the west building map in ap108, the event sets 26 as in
// James' game (stage 12 program 3); message 4.
static int s58_get_map(Prog* p) { return p_map_get(p, "map_apartmapw", 4); }

// 0x570e50 (encrypted, reconstructed): the locked door in ap96 with Tim's note (message 3).
static int s58_locked_door(Prog* p) { return p_message(p, 3); }

// 0x570ef0 (encrypted, reconstructed): the white liquid (item 85, message 5); the event sets 720.
static int s58_get_white_liquid(Prog* p) { return p_item_get(p, 0x55, 5); }

static const ProgFn s_stage58[4] = { NULL, s58_get_map, s58_locked_door, s58_get_white_liquid };

const ProgStage prog_wish_stages[] = {
	{ 3, s_stage53 }, { 2, s_stage54 }, { 11, s_stage55 }, { 14, s_stage56 }, { 2, s_stage57 }, { 4, s_stage58 },
};
