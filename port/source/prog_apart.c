#include "prog_int.h"
#include <stdio.h>
#include <string.h>

// Stage programs of the Wood Side and Blue Creek apartments (stages 6..13), ported from the PC
// overlays (no symbols; PS2 beta names from Event/stage/stg_apart_*.c, which list the programs in
// the same order as the PC tables). Flags, items, messages and sound ids are the PC's numbers.
// Cutscenes go through prog_demo_request(); pictures through the engine's picture hook.


// ================================================================ stage 6 Stg_apart_e1f (0x8e64e0)

// 0x59cbf0 EvProgVomitEddie: movie gero.sfd and the Eddie demo in the kitchen of 105.
static int s06_vomit_eddie(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_GERO_EDDIE))
		p_set(87);                      // FLAGW_64 |= 0x800000: Eddie talked to
	PROG_END(p)
}

// 0x59cdd0 EvProgVomitEddieAfter (no function in the Ghidra project; read from the disassembly):
// Eddie's second scene, the demo variant depends on flag 87.
static int s06_vomit_eddie_after(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_GERO_EDDIE_AFTER))
	PROG_END(p)
}

// 0x59cf00 EvProgTouristGuideRead: the sightseeing brochure.
static int s06_tourist_guide(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_message(p, 1))
	PROG_STEP(p, 2, p_picture_msg(p, "apt_p_tourist", 0))
	PROG_END(p)
}

static const ProgFn s_stage06[4] = { NULL, s06_vomit_eddie, s06_vomit_eddie_after, s06_tourist_guide };

// ================================================================ stage 7 Stg_apart_e2f (0x8e5768)

#define CLOCK (*(u16*)(p_puzzle() + PZ_CLOCK))
#define CLOCK_OK (CLOCK >= 0x88c1 && CLOCK <= 0x8a3f)   // 9:07..9:13, "9:10"

// "The clock is stopped at <e0>:<e1>" (0x59c34c): the hour, and the minutes with two digits in the
// English text.
static const char* clock_text(int msg)
{
	int t = (CLOCK + 0x20) >> 6;
	char v[8];
	snprintf(v, sizeof v, "%d", t / 60);
	ev_msg_set_insert(0, v);
	snprintf(v, sizeof v, "%02d", t % 60);
	ev_msg_set_insert(1, v);
	return p_stage_text(msg);
}

// 0x599f60 EvProgThreeNameOnWall: "What's this...?", the names scratched on the wall of 208.
static int s07_three_names(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 7))
	PROG_STEP(p, 2, p_picture(p, "apt_clock_name"))
	PROG_STEP(p, 7, p_message(p, 8))
		p_picture_hide();
	PROG_END(p)
}

// 0x59a160 EvProgHintOfClockSet: the note with the clock riddle; the easy riddle has its own text.
static int s07_clock_hint(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 4))
	PROG_STEP(p, 2, p_picture(p, p_riddle() == 0 ? "apt_clock_memo_2" : "apt_clock_memo"))
		p_set(p_riddle() == 0 ? 75 : 78);
	PROG_STEP(p, 7, p_message(p, p_riddle() == 0 ? 9 : 5))
		p_picture_hide();
	PROG_END(p)
}

// 0x59c1c0 EvProgClockTime: looking at the clock face; the time is the puzzle's current setting.
static int s07_clock_time(Prog* p)
{
	PROG_BEGIN(p)
		p_set(77);
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "apt_clock_close"))
	PROG_STEP(p, 10, p_message_text(p, clock_text(p_flag(82) ? 10 : 11)))
		p_picture_hide();
	PROG_END(p)
}

// The needles on the open clock (case 8 of 0x59c4b0): left / right turn the time, B leaves. The PC
// plays SE 0x426c when the hands stop inside the 9:10 window, a hint kept here.
static int clock_needles(Prog* p)
{
	if (!p_flag(83)) {
		int d = 0;
		if (p->held & (KEY_DRIGHT | KEY_CPAD_RIGHT)) d = 32;
		if (p->held & (KEY_DLEFT | KEY_CPAD_LEFT)) d = -32;
		if (d) {
			int t = CLOCK + d;
			if (t < 0) t += 0xb400;
			if (t > 0xb400) t -= 0xb400;
			CLOCK = (u16)t;
			if (!p->work[0]) p_se(0x3e83);
			p->work[0] = 1;
		} else {
			if (p->work[0] && CLOCK_OK)
				p_se(0x426c);
			p->work[0] = 0;
		}
	}
	int t = (CLOCK + 0x20) >> 6;
	char buf[160];
	snprintf(buf, sizeof buf, "\n   The clock reads %d:%02d\n\n\n\n\n\n\n%s", t / 60, t % 60,
	         p_flag(83) ? "                           B: leave" : "      Left/Right: move hands  B: leave");
	p_panel(buf);
	if (p->down & KEY_B) {
		p_panel(NULL);
		p_se(0x3e83);
		return 1;
	}
	return 0;
}

// 0x59c4b0 EvProgUseClockKey (the PC folds EvProgClockNeedleMove into it): with flag 81 (set by the
// use-item event) James uses the clock key and the face opens, then the hands can be turned.
static int s07_use_clock_key(Prog* p)
{
	switch (p->step) {
	case 0:
		p_set(77);
		p_lock(p, true);
		if (!p_flag(81)) {
			p_se(0x3e82);
			PROG_JUMP(p, 2);
		}
		PROG_JUMP(p, 10);
	case 10:
		if (!p_item_use(p, 0x1c, 0x16, 0x3e81))
			return 0;
		p->step = 0x18, p->sub = 0;
		// fallthrough
	case 0x18:
		if (!p_demo(p, DEMO_CLOCK_OPEN))
			return 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("apt_clock_open");
		p->step = 8, p->sub = 0;
		// fallthrough
	case 8:
		if (!clock_needles(p))
			return 0;
		p_picture_hide();
		if (!p_flag(81)) {
			p_se(0x3e8b);
			PROG_JUMP(p, 0xd);
		}
		p->step = 0x19, p->sub = 0;
		// fallthrough
	case 0x19:
		if (!p_demo(p, DEMO_CLOCK_CLOSE))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_lock(p, false);
		p_clear(81);
		return 1;
	}
}

// 0x59a6b0 EvProgTryMoveClock: push the clock; it slides away only with the hands at 9:10.
static int s07_try_move_clock(Prog* p)
{
	PROG_BEGIN(p)
		p_set(77);
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_message(p, 6))
	PROG_STEP(p, 0x1f, p_question(p, 0xd))
		if (p->answer)
			PROG_JUMP(p, 0xd);
		if (!CLOCK_OK)
			PROG_JUMP(p, 0x20);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_CLOCK_PUSH))
		p_set(82);                      // FLAGW_64 |= 0x40000: the clock has moved
		PROG_JUMP(p, 0xd);
	case 0x20:
	PROG_STEP(p, 0x21, p_demo(p, DEMO_CLOCK_FAIL))
	PROG_STEP(p, 0x22, p_message(p, 0xe))
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x59aa10 EvProgUseEmergencyKey: the fire escape key on the door at the west end of 2F.
static int s07_use_emergency_key(Prog* p) { return p_item_use(p, 0x1e, 0x18, 0x4a51); }

// 0x59aa30 EvProgAnyoneInHole: reach into the hole in the wall of 202 for the clock key.
static int s07_anyone_in_hole(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_question(p, 2))
		if (p->answer)
			PROG_JUMP(p, 0xd);
		p_set(540);                     // FLAGW_512 bit 28 while the demo runs
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HOLE))
	PROG_STEP(p, 9, p_item_get(p, 0x1c, 0x13))
		p_set(79);                      // FLAGW_64 |= 0x8000: the hole is empty
		p_clear(540);
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x59ad80 EvProgNooneInHole: the hole again, nothing left.
static int s07_noone_in_hole(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HOLE_EMPTY))
	PROG_STEP(p, 9, p_message(p, 3))
		p_lock(p, false);
	PROG_END(p)
}

// 0x59afb0 EvProgLookDustChute: the trash chute; the easy riddle adds a hint.
static int s07_look_dust_chute(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "apt_p_dust_in"))
	PROG_STEP(p, 0x1e, p_message(p, 0xf))
		if (p_riddle() != 0)
			PROG_JUMP(p, 4);
	PROG_STEP(p, 0x1f, p_message(p, 0x10))
	case 4:
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x59b1e0 EvProgUseCannedJuice: the juice goes down the chute (the use event consumes it).
static int s07_use_canned_juice(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_DUST))
		p_se(0x3e85);
		p_lock(p, false);
	PROG_END(p)
}

// 0x59b300 EvProgNoFaceCorpse
static int s07_no_face_corpse(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_NO_FACE_CORPSE))
		p_lock(p, false);
	PROG_END(p)
}

// 0x59b3c0 EvProgAnyoneCry: a noise to the north, "What was that!?" after two seconds.
static int s07_anyone_cry(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(0x9c49);
	PROG_STEP(p, 0x1b, p_wait(p, 2.0f))      // _DAT_0063158c
	PROG_STEP(p, 0x1c, p_message(p, 0))
		p_lock(p, false);
	PROG_END(p)
}

// 0x59b4c0 EvProgGetLight: the flashlight on the mannequin; it comes on at once.
static int s07_get_light(Prog* p)
{
	if (!p_item_get(p, 0x12, 0x14))
		return 0;
	p_flashlight(true);                 // gFlashlightSwitch = 1, LightSpotOnOffSet
	if (p_flag(66) && p_flag(67))
		p_set(68);
	p_set(2457);                        // 0x1f7d710 |= 0x2000000
	return 1;
}

// 0x59b520 EvProgGetApart202Key
static int s07_get_202_key(Prog* p) { return p_item_get(p, 0x1b, 0x15); }

// 0x59b530 / 0x59b730 / 0x59b930 EvProgEndHint*Read: the cut-out article, one per ending hint.
static int endhint(Prog* p, int msg)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_message(p, 0x1a))
	PROG_STEP(p, 2, p_picture(p, "apt_p_endhint"))
	PROG_STEP(p, 10, p_message(p, msg))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

static int s07_endhint_recovery(Prog* p) { return endhint(p, 0x1b); }
static int s07_endhint_maria(Prog* p) { return endhint(p, 0x1c); }
static int s07_endhint_suicide(Prog* p) { return endhint(p, 0x1d); }

// 0x59bb30 EvProgUseApart202Key
static int s07_use_202_key(Prog* p) { return p_item_use(p, 0x1b, 0x17, 0x4a3b); }

// 0x59bb50 EvProgMonkeyKick206: a thump from 206 unless Laura's key scene ran.
static int s07_monkey_kick_206(Prog* p)
{
	(void)p;
	if (!p_flag(69))
		p_se(0x9c47);
	return 1;
}

// Index 4 (0x59a6a0, EvProgClockNeedleMove) only returns 0 on the PC and no event uses it.
static const ProgFn s_stage07[21] = {
	NULL, s07_three_names, s07_clock_hint, s07_clock_time, NULL, s07_use_clock_key, s07_try_move_clock,
	s07_use_emergency_key, s07_anyone_in_hole, s07_noone_in_hole, s07_look_dust_chute, s07_use_canned_juice,
	s07_no_face_corpse, s07_anyone_cry, s07_get_light, s07_get_202_key, s07_endhint_recovery,
	s07_endhint_maria, s07_endhint_suicide, s07_use_202_key, s07_monkey_kick_206,
};

// ================================================================ stage 8 Stg_apart_e3fw (0x8e4080)

// 0x599c30: the monsters in room 0x15 (3F west hall) go away and flag 68 starts the cry in 2F.
static void e3fw_clear_hall(void)
{
	p_set(68);                          // FLAGW_64 |= 0x10
}

// 0x599ce0 EvProgLauraKickKey: the key behind the bars of 301, Laura kicks it away.
static int s08_laura_kick_key(Prog* p)
{
	PROG_BEGIN(p)
		if (p_riddle() == 0)
			p_set(1655);
		p_lock(p, true);
		p_set(540);
	PROG_STEP(p, 10, p_question(p, 3))
		if (p->answer)
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_KEY_KICK))
		p_set(67);                      // FLAGW_64 |= 8
		if (p_flag(66))
			e3fw_clear_hall();
	case 0xd:
		p_clear(540);
		p_lock(p, false);
	PROG_END(p)
}

// 0x599f30 EvProgGetHandgun: the handgun in the shopping cart.
static int s08_get_handgun(Prog* p)
{
	if (!p_item_get(p, 4, 0))
		return 0;
	if (p_flag(67))
		e3fw_clear_hall();
	return 1;
}

static const ProgFn s_stage08[3] = { NULL, s08_laura_kick_key, s08_get_handgun };

// ================================================================ stage 9 Stg_apart_e3fe (0x8e3948)

// 0x5991f0 EvProgFirstMeetTrihead: Pyramid Head behind the bars; James ends up in the hallway.
static int s09_first_meet_trihead(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_PYRAMID_FIRST))
		p_se(0x3e86);
		// The PC puts James at z -100950 facing 0, outside the closet. Enhanced Edition
		// "ChangeClosetSpawn" (Patches/ChangeClosetSpawn.cpp, zlib licence) moves him back into
		// the closet (z -101525, yaw 0.3) so the player notices the key lying in there.
		p_place(-21300.0f, 0.0f, -101525.0f, 0.3f);
		p_set(91);                      // FLAGW_64 |= 0x8000000
		p_lock(p, false);
	PROG_END(p)
}

// 0x599490 EvProgGetYardKey
static int s09_get_yard_key(Prog* p) { return p_item_get(p, 0x1d, 1); }

// 0x5994a0 EvProgGetEmergencyKey
static int s09_get_emergency_key(Prog* p)
{
	if (p->sub == 0 && p_riddle() == 0)
		p_set(1656);
	return p_item_get(p, 0x1e, 2);
}

static const ProgFn s_stage09[4] = { NULL, s09_first_meet_trihead, s09_get_yard_key, s09_get_emergency_key };

// ================================================================ stage 10 Stg_apart_w1f (0x8e2d38)

// Coin puzzle in the Lyne apartment: coin c (0 snake, 1 old man, 2 prisoner = items 50..52) in hole
// h (0..4) is flag 126 + 5c + h. Flags 120..122 say which coin the player is putting in.
#define COIN_FLAG(c, h) (126 + 5 * (c) + (h))

static int coin_in_hole(int h)
{
	for (int c = 0; c < 3; c++)
		if (p_flag(COIN_FLAG(c, h)))
			return c;
	return -1;
}

// 0x598870 EvProgLookThreeCoin: the writing on the desk, one riddle per difficulty.
static int s10_look_three_coin(Prog* p)
{
	static const int msg[4] = { 0x3f, 1, 2, 3 }, flag[4] = { 609, 123, 124, 125 };
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "apt_p_desk_hint"))
	PROG_STEP(p, 0x1e, p_message(p, 4))
		p_set(flag[p_riddle()]);
	PROG_STEP(p, 0x1f, p_message(p, msg[p_riddle()]))
	PROG_STEP(p, 5, p_picture(p, "apt_p_desk_coin"))
	PROG_STEP(p, 0x20, p_message(p, 5))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

static void coin_panel(Prog* p, bool placing)
{
	static const char* const name[3] = { "Snake", "Old Man", "Prisoner" };
	char row[5][16];
	for (int h = 0; h < 5; h++) {
		int c = coin_in_hole(h);
		snprintf(row[h], sizeof row[h], "%s", c < 0 ? "(empty)" : name[c]);
	}
	char buf[400];
	int n = snprintf(buf, sizeof buf, "\n");
	for (int h = 0; h < 5; h++)
		n += snprintf(buf + n, sizeof buf - n, "  %c Hole %d   %s\n", h == p->work[1] ? '>' : ' ', h + 1, row[h]);
	snprintf(buf + n, sizeof buf - n, "\n\n  D-pad: hole  A: %s  B: leave", placing ? "put in" : "take out");
	p_panel(buf);
}

// 0x598c30 EvProgSetThreeCoin (helper EvProgSubCoinCursor 0x597ea0): put a coin into a hole or take
// coins out; on leaving, flag 141 says whether any coin is in the desk and the right three holes
// for the riddle level set flag 142 (the Lyne key appears).
static int s10_set_three_coin(Prog* p)
{
	bool placing = p_flag(120) || p_flag(121) || p_flag(122);
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("apt_p_desk_coin");
		p->step = 10, p->sub = 0;
		// fallthrough
	case 10:
		if (!p_message(p, placing ? 6 : 7))
			return 0;
		p->step = 8, p->sub = 0;
		// fallthrough
	case 8:
		if (p->down & (KEY_DUP | KEY_DLEFT)) p->work[1] = (p->work[1] + 4) % 5;
		if (p->down & (KEY_DDOWN | KEY_DRIGHT)) p->work[1] = (p->work[1] + 1) % 5;
		coin_panel(p, placing);
		if (p->down & KEY_B)
			PROG_JUMP(p, 4);
		if (!(p->down & KEY_A))
			return 0;
		{
			int h = p->work[1], c = coin_in_hole(h);
			if (placing) {
				if (c >= 0)
					return 0;           // hole taken
				c = p_flag(122) ? 2 : p_flag(121) ? 1 : 0;
				p_set(COIN_FLAG(c, h));
				p_se(0x3e89);
				p_item_lose(50 + c);
				p_clear(120 + c);       // one coin per use
				coin_panel(p, false);
				PROG_JUMP(p, 0x10);
			}
			if (c < 0)
				return 0;
			p_clear(COIN_FLAG(c, h));
			p_item_get_now(50 + c);
		}
		return 0;
	case 0x10:                      // the coin is in; any button leaves
		if (!(p->down & (KEY_A | KEY_B)))
			return 0;
		// fallthrough
	case 4:
	default: {
		bool any = false;
		for (int h = 0; h < 5; h++)
			any |= coin_in_hole(h) >= 0;
		ev_flag_set(141, any);
		static const int need[4][3] = { { 128, 131, 140 }, { 130, 132, 138 }, { 129, 132, 140 }, { 128, 131, 139 } };
		const int* n = need[p_riddle()];
		if (p_flag(n[0]) && p_flag(n[1]) && p_flag(n[2])) {
			p_set(142);
			p_se(0x3e8a);
		}
		p_panel(NULL);
		p_picture_hide();
		p_lock(p, false);
		p_clear(120);
		p_clear(121);
		p_clear(122);
		return 1;
	}
	}
}

// 0x598180 EvProgAngelaWithKnife: movie knife.sfd; James keeps Angela's knife.
static int s10_angela_with_knife(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_KNIFE_ANGELA))
		p_place(60185.0f, 0.0f, -98580.0f, 0.0269496f);
		p_item_get_now(0x18);
		p_lock(p, false);
	PROG_END(p)
}

// 0x5984a0 EvProgGetCoinOfPrisoner / 0x5984b0 EvProgGetWhiteChrism / 0x5984c0 EvProgGetLyneKey
static int s10_get_coin_prisoner(Prog* p) { return p_item_get(p, 0x34, 8); }
static int s10_get_white_chrism(Prog* p) { return p_item_get(p, 0x4c, 10); }
static int s10_get_lyne_key(Prog* p) { return p_item_get(p, 0x1f, 9); }

// 0x5984d0 EvProgLookFamilyPicture
static int s10_look_family_picture(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 0))
	PROG_STEP(p, 2, p_picture(p, "apt_p_family"))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage10[8] = {
	NULL, s10_look_three_coin, s10_set_three_coin, s10_angela_with_knife, s10_get_coin_prisoner,
	s10_get_white_chrism, s10_get_lyne_key, s10_look_family_picture,
};

// ================================================================ stage 11 Stg_apart_w2f (0x8e2620)

// The safe code as the wallet memo shows it (0x596dd0): two digits per number on easy and normal,
// partly hidden on hard, single characters on extra.
void prog_wallet_insert(void)
{
	const u8* code = p_puzzle() + PZ_SAFE_CODE;
	char v[4][4];
	for (int i = 0; i < 4; i++) {
		int n = code[i] + 1;
		if (p_riddle() < 2)
			snprintf(v[i], 4, "%02d", n);
		else if (p_riddle() == 2)
			snprintf(v[i], 4, n < 10 ? "%d" : n < 20 ? (i & 1 ? "X%d" : "VV%d") : "XX", n % 10);
		else
			snprintf(v[i], 4, "%c", n < 10 ? '0' + n : code[i] + 0x58);
	}
	for (int i = 0; i < 4; i++)
		ev_msg_set_insert(i, v[i]);     // "<e0> >><e1> <<<e2> >><e3>"
}

static const char* wallet_text(int msg)
{
	prog_wallet_insert();
	return p_stage_text(msg);
}

// 0x596c10 EvProgReadDearTim: the note on the locked door of 209.
static int s11_read_dear_tim(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_set(1698);                    // FLAGW_1696 |= 4
		p_se(0x4a3c);
	PROG_STEP(p, 0x1e, p_system_message(p, 4))
	PROG_STEP(p, 0x1f, p_message(p, 0))
	PROG_STEP(p, 0x20, p_message(p, 1))
		p_lock(p, false);
	PROG_END(p)
}

// 0x596d80 EvProgUseLyneKey / 0x596da0 EvProgGetApartStairKey / 0x596db0 EvProgUseApartStairKey
static int s11_use_lyne_key(Prog* p) { return p_item_use(p, 0x1f, 9, 0x4a51); }
static int s11_get_stair_key(Prog* p) { return p_item_get(p, 0x20, 8); }
static int s11_use_stair_key(Prog* p) { return p_item_use(p, 0x20, 10, 0x4a51); }

// 0x597710 EvProgWalletInToilet: reach into the toilet; the wallet holds the safe code.
static int s11_wallet_in_toilet(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_question(p, 2))
		if (p->answer)
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_WALLET))
	PROG_STEP(p, 0x20, p_message_text(p, wallet_text(3)))
		p_set(102);                     // FLAGW_96 |= 0x40
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x5979a0 EvProgWalletOutToilet: the memo again.
static int s11_wallet_out_toilet(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x10, p_message_text(p, wallet_text(4)))
		p_lock(p, false);
	PROG_END(p)
}

// Dial position 0..19 lives in flags 103..107 (FLAGW_96 bits 7..11), progress in PZ_SAFE_PROG.
static int safe_dial(void)
{
	int v = 0;
	for (int b = 0; b < 5; b++)
		v |= p_flag(103 + b) << b;
	return v;
}

// 0x5971a0: one dial step. Turn either way to the first number, left to the second, right to the
// third and left to the fourth; a turn the wrong way after a number starts over.
static void safe_step(int dir)
{
	u8* code = p_puzzle() + PZ_SAFE_CODE;
	s8* prog = (s8*)p_puzzle() + PZ_SAFE_PROG;
	int v = safe_dial();
	p_se(0x3e87);
	if (dir < 0) {
		v = v ? v - 1 : 19;
		if (prog[2] >= 0) {
			if (v == code[3])
				p_set(108);             // FLAGW_96 |= 0x1000: open
		} else if (prog[1] >= 0) {
			memset(prog, 0xff, 4);
		} else if (prog[0] >= 0) {
			if (v == code[1]) prog[1] = (s8)v;
		} else if (v == code[0]) {
			prog[0] = (s8)v;
		}
	} else {
		v = v == 19 ? 0 : v + 1;
		if (prog[2] >= 0) {
			memset(prog, 0xff, 4);
		} else if (prog[1] >= 0) {
			if (v == code[2]) prog[2] = (s8)v;
		} else if (prog[0] >= 0) {
			memset(prog, 0xff, 4);
		} else if (v == code[0]) {
			prog[0] = (s8)v;
		}
	}
	for (int b = 0; b < 5; b++)
		ev_flag_set(103 + b, (v >> b) & 1);
}

// 0x597b00 EvProgOpenSafe: the dial of the small safe in 208's neighbour.
static int s11_open_safe(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_picture_show("apt_p_safe_close");
		p->step = 8;
		// fallthrough
	case 8:
		if (!p_flag(108)) {
			// 0x5971a0: right (stick x > 0 or key 0x5000009) turns the dial down, left turns it up, as
			// the wallet's ">>" (right) and "<<" (left) say
			if (p->down & (KEY_DRIGHT | KEY_CPAD_RIGHT)) safe_step(-1);
			if (p->down & (KEY_DLEFT | KEY_CPAD_LEFT)) safe_step(1);
		}
		{
			char buf[160];
			snprintf(buf, sizeof buf, "\n   The dial points at %d\n\n%s\n\n\n\n\n%s", safe_dial() + 1,
			         p_flag(108) ? "   It opened." : "", p_flag(108) ? "" : "      Left/Right: turn  B: leave");
			p_panel(buf);
		}
		if (p_flag(108)) {
			if (!p_wait(p, 0.2f))   // _DAT_0063345c
				return 0;
		} else if (!(p->down & KEY_B)) {
			return 0;
		}
		p_panel(NULL);
		p_picture_hide();
		if (p_flag(108))
			p_se(0x3e88);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage11[8] = {
	NULL, s11_read_dear_tim, s11_use_lyne_key, s11_get_stair_key, s11_use_stair_key,
	s11_wallet_in_toilet, s11_wallet_out_toilet, s11_open_safe,
};

// ================================================================ stage 12 Stg_apart_stair (0x8e1e58)

// 0x5961d0 EvProgUseYardKey
static int s12_use_yard_key(Prog* p) { return p_item_use(p, 0x1d, 4, 0x4a51); }

// 0x5961f0 EvProgGetApartMap / 0x596200 EvProgGetApartWestMap (EvSubMapGet). The maps themselves
// are flags 25 / 26 set by the events.
static int s12_get_apart_map(Prog* p) { return p_map_get(p, "map_apartmape1f", 2); }
static int s12_get_apart_west_map(Prog* p) { return p_map_get(p, "map_apartmapw", 3); }

// 0x596210 EvProgGetCannedJuice
static int s12_get_canned_juice(Prog* p) { return p_item_get(p, 0x31, 1); }

// 0x596220 EvProgApartBoss: Pyramid Head comes down the stairs.
static int s12_apart_boss(Prog* p)
{
	PROG_BEGIN(p)
		p_set(529);                     // FLAGW_512 |= 0x20000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_APART_BOSS))
		p_lock(p, false);
	PROG_END(p)
}

// 0x5964c0 EvProgApartBossSiren
static int s12_apart_boss_siren(Prog* p)
{
	(void)p;
	p_se(0x2716);
	return 1;
}

// 0x5964e0 EvProgApartBossEnd: the siren, Pyramid Head leaves; four seconds of camera.
static int s12_apart_boss_end(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(0x4a4f);
		p_set(147);                     // FLAGW_128 |= 0x80000
	PROG_STEP(p, 0x16, p_wait(p, 4.0f))      // _DAT_006318a4
		p_set(148);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage12[8] = {
	NULL, s12_use_yard_key, s12_get_apart_map, s12_get_apart_west_map, s12_get_canned_juice,
	s12_apart_boss, s12_apart_boss_siren, s12_apart_boss_end,
};

// ================================================================ stage 13 Stg_apart_out (0x8e1590)

// 0x5959d0 EvProgGetCoinOfSnake: in the empty pool.
static int s13_get_coin_snake(Prog* p) { return p_item_get(p, 0x32, 5); }

// 0x5959e0 EvProgGetCoinOfOldman: the garbage bag under the chute, after the juice fell down.
static int s13_get_coin_oldman(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "apt_p_dust_out"))
	PROG_STEP(p, 10, p_message(p, 6))
	PROG_STEP(p, 0x1e, p_item_get_quiet(p, 0x33, 7))
	PROG_STEP(p, 0x20, p_message(p, 0))
		p_picture_hide();
		p_lock(p, false);
		p_set(96);                      // FLAGW_96 |= 1
	PROG_END(p)
}

// 0x595d80 EvProgMurderNewsRead
static int s13_murder_news(Prog* p) { return p_picture_msg(p, "apt_p_dust_out", 1); }

// 0x595d90 EvProgApartEastToWest: through the fire escape into the west building; the first time
// James remarks on the missing stairs between the two demos.
static int s13_east_to_west(Prog* p)
{
	PROG_BEGIN(p)
		if (p_flag(97))
			PROG_JUMP(p, 0x18);
	PROG_STEP(p, 0x17, p_demo(p, DEMO_TOBIRA_A))
	PROG_STEP(p, 10, p_message(p, 4))
	PROG_STEP(p, 0x18, p_demo(p, DEMO_TOBIRA_B))
		p_lock(p, false);
	PROG_END(p)
}

// 0x595f50 EvProgApartWestToEast
static int s13_west_to_east(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x18, p_demo(p, DEMO_TOBIRA_C))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage13[6] = {
	NULL, s13_get_coin_snake, s13_get_coin_oldman, s13_murder_news, s13_east_to_west, s13_west_to_east,
};

const ProgStage prog_apart_stages[] = {
	{ 4, s_stage06 }, { 21, s_stage07 }, { 3, s_stage08 }, { 4, s_stage09 },
	{ 8, s_stage10 }, { 8, s_stage11 }, { 8, s_stage12 }, { 6, s_stage13 },
};
