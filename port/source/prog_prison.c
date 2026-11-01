#include "prog_int.h"
#include "i18n.h"
#include <stdio.h>
#include <string.h>

// Stage programs of the Historical Society, the prison and the Labyrinth (stages 28..37), ported
// from the PC overlays; PS2 beta names from Event/stage/stg_society.c .. stg_eddie_boss.c. Flags,
// items, messages and SE ids are the PC's; cutscenes go to prog_demo_request().

// ================================================================ stage 28 Stg_society (0x8d75c0)

// 0x587c00 EvProgTriheadPicture / 0x587c10 EvProgGetGoblet (no functions in the Ghidra project;
// read from the disassembly: EvSubPictureMsg(dls/p_sankaku, 2) and EvSubItemGet(0x4d, 7))
static int s28_trihead_picture(Prog* p) { return p_picture_msg(p, "dls_p_sankaku", 2); }
static int s28_get_goblet(Prog* p) { return p_item_get(p, 0x4d, 7); }

static const ProgFn s_stage28[3] = { NULL, s28_trihead_picture, s28_get_goblet };

// ================================================================ stage 29 Stg_delusion_2 (0x8d72a8)

// A hole: "Will you jump down?" then the jump demo and the layer flag.
static int jump_hole(Prog* p, int msg, int demo, int flag)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_question(p, msg))
		if (p->answer)
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x18, p_demo(p, demo))
		p_set(flag);
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x587920 EvProgHoleTo3rdLayer (disassembly; flag 265 = FLAGW_256 |= 0x200)
static int s29_hole_to_3rd(Prog* p) { return jump_hole(p, 0, DEMO_JUMP1, 265); }

static const ProgFn s_stage29[2] = { NULL, s29_hole_to_3rd };

// ================================================================ stage 30 Stg_delusion_3 (0x8d6ce8)

// 0x586040 EvProgStartBottomOfWell
static int s30_bottom_of_well(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_BOTTOM_OF_WELL))
		p_place(-49967.75f, 0.0f, -30079.145f, 1.91986f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x586160 EvProgBreakWellWall: the wall gives way.
static int s30_break_well_wall(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p->timer = 0.0f;
	PROG_STEP(p, 4, p_wait(p, 3.0f))     // FadeStart(1, 3.0)
		p_se(0x497f);
		p_set(268);                     // FLAGW_256 |= 0x1000
		p_place(-50000.0f, 0.0f, -29250.0f, 0.0f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x586210 EvProgGetSpiralKey: also unlocks the panel lock (269) and the battery (272).
static int s30_get_spiral_key(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0xf, p_item_get_quiet(p, 0x29, 5))
		p_set(269);                     // FLAGW_256 |= 0x12000
		p_set(272);
		p_lock(p, false);
	PROG_END(p)
}

// 0x587470 EvProgCocRoomLock (logic 0x586350 / 0x586ae0): nine panels, three of them light up
// (0x1f7da74..76); pressing those three in order clears flag 269, the lock.
static int s30_coc_room_lock(Prog* p)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p->work[2] = 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("dls_p_panelrun");
		p->step = 8, p->sub = 0;
		// fallthrough
	case 8: {
		const u8* goal = p_puzzle() + PZ_PANEL;
		u8* got = (u8*)&p->work[3];
		if (p->down & KEY_DLEFT) p->work[1] = (p->work[1] + 8) % 9;
		if (p->down & KEY_DRIGHT) p->work[1] = (p->work[1] + 1) % 9;
		if (p->down & KEY_DUP) p->work[1] = (p->work[1] + 6) % 9;
		if (p->down & KEY_DDOWN) p->work[1] = (p->work[1] + 3) % 9;
		if ((p->down & KEY_A) && p->work[2] < 3 && p_flag(269)) {
			got[p->work[2]++] = (u8)p->work[1];
			p_se(10000);
		}
		char buf[300];
		int n = snprintf(buf, sizeof buf, "\n");
		for (int r = 0; r < 3; r++) {
			n += snprintf(buf + n, sizeof buf - n, "        ");
			for (int c = 0; c < 3; c++) {
				int k = r * 3 + c, lit = 0;
				for (int i = 0; i < 3; i++)
					if (goal[i] == k) lit = i + 1;
				n += snprintf(buf + n, sizeof buf - n, "%c%c%c ", p->work[1] == k ? '[' : ' ',
				              lit ? '0' + lit : '.', p->work[1] == k ? ']' : ' ');
			}
			n += snprintf(buf + n, sizeof buf - n, "\n");
		}
		char pressed[64];
		snprintf(pressed, sizeof pressed, tr("Pressed %d of 3"), p->work[2]);
		snprintf(buf + n, sizeof buf - n, "\n %s\n\n D-pad: panel  A: press  B: leave", pressed);
		p_panel(buf);
		if (p->work[2] >= 3) {
			if (got[0] == goal[0] && got[1] == goal[1] && got[2] == goal[2])
				p_clear(269);           // FLAGW_256 &= ~0x2000
			p_se(p_flag(269) ? 18000 : 0x4651);
		} else if (!(p->down & KEY_B)) {
			return 0;
		}
		// fallthrough
	}
	default:
		p_panel(NULL);
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x586bb0 EvProgUseSpiralKey
static int s30_use_spiral_key(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_item_use(p, 0x29, 6, 0x2b39))
		p_se(0x2b36);
		p_set(271);                     // FLAGW_256 |= 0x8000
		p_place(-59625.0f, 0.0f, -10050.0f, 1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x586d40 EvProgHoleTo4thLayer (flag 274 = FLAGW_256 |= 0x40000)
static int s30_hole_to_4th(Prog* p) { return jump_hole(p, 4, DEMO_JUMP2, 274); }

// 0x586f40 EvProgUseDryCell: a new battery for the flashlight; the room's creepers and nests come
// out. The PC creates them (0x48fcc0) before the item message, setting their 0x148 bit 0x400; here
// the list spawns them once the dry cell is gone (enemy.c, condition 0x340).
static int s30_use_dry_cell(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x10, p_item_use(p, 0x37, 7, 0))
		p_enemy_list();
		p_clear(272);                   // FLAGW_256 &= ~0x10000
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage30[8] = {
	NULL, s30_bottom_of_well, s30_break_well_wall, s30_get_spiral_key, s30_coc_room_lock, s30_use_spiral_key,
	s30_hole_to_4th, s30_use_dry_cell,
};

// ================================================================ stage 31 Stg_prison_n (0x8d6028)

// 0x584fc0 EvProgEddieKillHuman: movies korosu_a / korosu_b.
static int s31_eddie_kill_human(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x17, p_demo(p, DEMO_EDDIE_KILL_HUMAN))
		p_place(-60117.0f, 0.0f, 61186.5f, -2.7617927f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x585350 EvProgGetPrisonMap / 0x585360 EvProgGetTabletPig / 0x585370 EvProgGetTabletSeduct /
// 0x585380 EvProgGetRifle / 0x585390 EvProgGetLighter
static int s31_get_prison_map(Prog* p) { return p_map_get(p, "map_prisonmap03", 8); }
static int s31_get_tablet_pig(Prog* p) { return p_item_get(p, 0x3c, 9); }
static int s31_get_tablet_seduct(Prog* p) { return p_item_get(p, 0x3d, 10); }
static int s31_get_rifle(Prog* p) { return p_item_get(p, 8, 0xb); }
static int s31_get_lighter(Prog* p) { return p_item_get(p, 0x3f, 0xc); }

// 0x5853a0 EvProgReadLakeMagazine
static int s31_lake_magazine(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		if (p_flag(279))
			PROG_JUMP(p, 2);
	PROG_STEP(p, 0x1e, p_message(p, 1))
	PROG_STEP(p, 2, p_picture(p, "dls_p_magazin"))
	PROG_STEP(p, 0x1f, p_message(p, 2))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x5855c0 EvProgTryOpenFloor: the cover on the floor; flags 280..282 say what was tried.
static int s31_try_open_floor(Prog* p)
{
	PROG_BEGIN(p)
		p_set(2038);                    // FLAGW_2016 |= 0x400000
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "dls_p_pull_off"))
	PROG_STEP(p, 10, p_message(p, p_flag(280) ? 5 : p_flag(281) ? 6 : p_flag(282) ? 4 : 7))
		p_clear(280);                   // FLAGW_256 &= 0xf8ffffff
		p_clear(281);
		p_clear(282);
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x585d00 EvProgSuccessOpenFloor: the horseshoe with melted wax lifts the cover.
static int s31_success_open_floor(Prog* p)
{
	PROG_BEGIN(p)
		p_set(283);                     // FLAGW_256 |= 0x8000000
		p_set(2038);                    // FLAGW_2016 |= 0xc00000
		p_set(2039);
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "dls_p_pull_on"))
		p_picture_hide();
		p_se(0x4658);
		p_place(-102900.0f, 0.0f, 89000.0f, -1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x585950 EvProgHoleTo5thLayer (flag 284 = FLAGW_256 |= 0x10000000)
static int s31_hole_to_5th(Prog* p) { return jump_hole(p, 3, DEMO_JUMP3, 284); }

static const ProgFn s_stage31[11] = {
	NULL, s31_eddie_kill_human, s31_get_prison_map, s31_get_tablet_pig, s31_get_tablet_seduct, s31_get_rifle,
	s31_get_lighter, s31_lake_magazine, s31_try_open_floor, s31_success_open_floor, s31_hole_to_5th,
};

// ================================================================ stage 32 Stg_prison_s (0x8d5218)

// 0x5844f0 EvProgGetTabletOppressor / 0x584520 EvProgGetWaxDoll / 0x584530 EvProgGetHorseshoe
static int s32_get_tablet_oppressor(Prog* p)
{
	if (p->sub == 0)
		p_set(288);                     // FLAGW_288 |= 1
	return p_item_get(p, 0x3b, 2);
}

static int s32_get_wax_doll(Prog* p) { return p_item_get(p, 0x40, 7); }

static int s32_get_horseshoe(Prog* p)
{
	if (p->sub == 0)
		p_set(2037);                    // FLAGW_2016 |= 0x200000
	return p_item_get(p, 0x3e, 6);
}

// 0x584980 EvProgTabletSetScaffold: the three tablets go into the gallows. The use events set
// 294 (Oppressor), 295 (Pig), 296 (Seductress) or one of 297..300 for the combined items, which
// the table at 0x8d54d4 turns into those three. Placed tablets are 301..303; all three set 304.
static int s32_tablet_scaffold(Prog* p)
{
	static const short combo[4][4] = { { 300, 294, 295, 296 }, { 297, 294, 295, 0 }, { 298, 294, 296, 0 },
	                                    { 299, 295, 296, 0 } };
	switch (p->step) {
	case 0:
		p_set(2036);                    // FLAGW_2016 |= 0x100000
		p_lock(p, true);
		for (int i = 0; i < 4; i++)
			if (p_flag(combo[i][0])) {
				for (int j = 1; j < 4; j++)
					if (combo[i][j]) p_set(combo[i][j]);
				p_clear(combo[i][0]);
			}
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("dls_p_tablet");
		if (!p_flag(294) && !p_flag(295) && !p_flag(296))
			PROG_JUMP(p, 10);
		p->step = 0x21, p->sub = 0;
		// fallthrough
	case 0x21:
		if (p_flag(294)) {
			if (!p_item_use(p, 0x3b, 3, 0x4659))
				return 0;
			p_clear(294);
			p_set(301);                 // FLAGW_288 |= 0x2000
			p->sub = 0;
		}
		if (p_flag(295)) {
			if (!p_item_use(p, 0x3c, 4, 0x4659))
				return 0;
			p_clear(295);
			p_set(302);                 // FLAGW_288 |= 0x4000
			p->sub = 0;
		}
		if (p_flag(296)) {
			if (!p_item_use(p, 0x3d, 5, 0x4659))
				return 0;
			p_clear(296);
			p_set(303);                 // FLAGW_288 |= 0x8000
			p->sub = 0;
		}
		if (p_flag(301) && p_flag(302) && p_flag(303) && !p_flag(304)) {
			p_set(304);                 // FLAGW_288 |= 0x10000
			p_se(0x426e);
		}
		PROG_JUMP(p, 0xd);
	case 10:
		if (!p_message(p, p_flag(301) || p_flag(302) || p_flag(303) ? 1 : 0))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x584790 / 0x5847a0 / 0x5847b0: the three paintings ("Woman in Flight", "436 People at a
// Recital", "Burning Man")
static int s32_painting_woman(Prog* p) { return p_picture_msg(p, "dls_p_flingwoman", 0xb); }
static int s32_painting_436(Prog* p) { return p_picture_msg(p, "dls_p_436", 10); }
static int s32_painting_fireman(Prog* p) { return p_picture_msg(p, "dls_p_fireman", 9); }

static const ProgFn s_stage32[8] = {
	NULL, s32_get_tablet_oppressor, s32_get_wax_doll, s32_get_horseshoe, s32_tablet_scaffold,
	s32_painting_woman, s32_painting_436, s32_painting_fireman,
};

// ================================================================ stage 33 Stg_prison_bf (0x8d4898)

// 0x583c60 EvProg5thLayerStart / 0x583fb0 EvProg6thLayerStart
static int s33_5th_layer_start(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_5F_START))
		p_place(-100714.5f, 0.0f, 110025.5f, -1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

static int s33_6th_layer_start(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_6F_START))
		p_place(-20025.2f, 0.0f, -48801.734f, 3.1415927f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x583db0 EvProgHoleTo6thLayer (306 = FLAGW_288 |= 0x40000), 0x5841b0 EvProgHoleTo7thLayer (310)
static int s33_hole_to_6th(Prog* p) { return jump_hole(p, 1, DEMO_JUMP4, 306); }
static int s33_hole_to_7th(Prog* p) { return jump_hole(p, 2, DEMO_JUMP5, 310); }

// 0x584100 EvProgDoorOpen6Layer: the door opens with a bang and James steps through (z + 80).
static int s33_door_open_6(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_se(0x4a4f);
		p->timer = 0.0f;
	PROG_STEP(p, 4, p_wait(p, 1.2f))     // FadeStart(2) default length
		{
			float pos[3], yaw;
			p_player(pos, &yaw);
			p_place(pos[0], pos[1], pos[2] + 80.0f, yaw);   // _DAT_0069f3b0
		}
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage33[6] = {
	NULL, s33_5th_layer_start, s33_hole_to_6th, s33_6th_layer_start, s33_door_open_6, s33_hole_to_7th,
};

// ================================================================ stage 34 Stg_labyrinth_w (0x8d3efc)

// 0x581cc0 EvProg7thLayerStart
static int s34_7th_layer_start(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_7F_START))
		p_place(-17647.85f, 0.0f, -100067.65f, -1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x581e10 EvProgGetGreatKnife
static int s34_get_great_knife(Prog* p) { return p_item_get(p, 0xf, 0x26); }

// 0x581e20 EvProgControlCube: the cube room. Two rings turn in quarter steps: the outer one's
// position is flags 317 / 318, the inner one's 319 / 320; the table at 0x8d41a4 then opens the
// ways out (flag 325 one door, 326 the other).
static int s34_control_cube(Prog* p)
{
	static const u8 table[16] = { 2, 1, 2, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0 };
	int a = p_flag(317) | p_flag(318) << 1;
	int b = p_flag(319) | p_flag(320) << 1;
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->step = 8;
		// fallthrough
	case 8:
		if (p->down & KEY_DUP) { a = (a + 1) & 3; p_se(0x4970); p_se(0x4972); }
		if (p->down & KEY_DDOWN) { a = (a + 3) & 3; p_se(0x4970); p_se(0x4972); }
		if (p->down & KEY_DRIGHT) { b = (b + 1) & 3; p_se(0x4971); p_se(0x4972); }
		if (p->down & KEY_DLEFT) { b = (b + 3) & 3; p_se(0x4971); p_se(0x4972); }
		ev_flag_set(317, a & 1);
		ev_flag_set(318, a >> 1 & 1);
		ev_flag_set(319, b & 1);
		ev_flag_set(320, b >> 1 & 1);
		ev_flag_set(325, table[a * 4 + b] != 0);
		ev_flag_set(326, table[a * 4 + b] > 1);
		{
			char buf[200];
			snprintf(buf, sizeof buf, "\n   %s  %d / 4\n   %s  %d / 4\n\n\n\n\n"
			         "   Up/Down: outer  Left/Right: inner\n   B: leave", tr("Outer ring"), a + 1, tr("Inner ring"), b + 1);
			p_panel(buf);
		}
		if (!(p->down & KEY_B))
			return 0;
		// fallthrough
	default:
		p_panel(NULL);
		p_lock(p, false);
		return 1;
	}
}

// 0x582750 EvProgMariaIsAlive: movie rouya.sfd, Maria behind the bars.
static int s34_maria_is_alive(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x17, p_demo(p, DEMO_MARIA_ALIVE))
		p_place(20000.0f, 0.0f, 18600.0f, 3.1415927f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x5829c0 EvProgAfterMariaIsAlive
static int s34_after_maria_alive(Prog* p) { (void)p; return 1; }

// 0x5829d0 EvProgGetWireCutter
static int s34_get_wire_cutter(Prog* p) { return p_item_get(p, 0x41, 0x25); }

// 0x5829e0 EvProgUseWireCutter: the wire across the path (the item stays).
static int s34_use_wire_cutter(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_WIRE_CUTTER))
		p_place(63790.15f, 0.0f, 99798.0f, 1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage34[8] = {
	NULL, s34_7th_layer_start, s34_get_great_knife, s34_control_cube, s34_maria_is_alive,
	s34_after_maria_alive, s34_get_wire_cutter, s34_use_wire_cutter,
};

// ================================================================ stage 35 Stg_labyrinth_e (0x8d2cb0)

// 0x580bf0 EvProgScreamOfAngela (returns 1; the scream is a sound event)
static int s35_scream_of_angela(Prog* p) { (void)p; return 1; }

// 0x580c00 EvProgPapaAttackAngela: "No daddy! Please! Don't!"
static int s35_papa_attack_angela(Prog* p)
{
	PROG_BEGIN(p)
		p_set(606);                     // FLAGW_576 |= 0x40000000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_ANGELA_SCREAM))
		p_lock(p, false);
	PROG_END(p)
}

// 0x580e30 EvProgAngelaAttackPapa: Abstract Daddy appears.
static int s35_angela_attack_papa(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_PAPA_ATTACK))
		p_place(20071.975f, 0.0f, 59529.0f, 0.579536f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x581090 (no PS2 counterpart): after the fight, Angela leaves.
static int s35_after_papa(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_ANGELA_ATTACK))
		p_place(20007.475f, 0.0f, 60215.0f, -1.9300585f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x581260 EvProgNewsOfPapa: the newspaper about Thomas Orosco.
static int s35_news_of_papa(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		if (p_flag(331))
			PROG_JUMP(p, 2);
	PROG_STEP(p, 10, p_message(p, 1))
	PROG_STEP(p, 2, p_picture(p, "dls_p_newspaper"))
	PROG_STEP(p, 7, p_message(p, 0))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage35[6] = {
	NULL, s35_scream_of_angela, s35_papa_attack_angela, s35_angela_attack_papa, s35_after_papa,
	s35_news_of_papa,
};

// ================================================================ stage 36 Stg_labyrinth_n (0x8d1b90)

// The hangman room: six corpses with six ropes. Corpse k's crime is (0x1f7da78 / 6^k) % 6; the
// innocent one has crime 5. Reading a corpse (flags 337..342 from its event) shows its crime, in
// a wording that depends on the riddle level (table in 0x57f730).
static int hangman_crime(int k)
{
	int v = *(u16*)(p_puzzle() + PZ_HANGMAN);
	for (int i = 0; i < k; i++)
		v /= 6;
	return v % 6;
}

// 0x57f730 EvProgHangingCorpse
static int s36_hanging_corpse(Prog* p)
{
	static const int crime_msg[4][6] = { { 7, 3, 6, 5, 1, 2 }, { 0, 2, 1, 6, 3, 7 }, { 5, 3, 2, 4, 0, 6 },
	                                     { 0, 5, 7, 2, 3, 1 } };
	PROG_BEGIN(p)
		p_lock(p, true);
		if (p_flag(336))
			PROG_JUMP(p, 10);
	PROG_STEP(p, 0x1a, p_message(p, 8))
		p->step = 10, p->sub = 0;
		// fallthrough
	case 10: {
		int k = 0;
		while (k < 6 && !p_flag(337 + k))
			k++;
		if (!p_message(p, crime_msg[p_riddle()][hangman_crime(k < 6 ? k : 5)]))
			return 0;
	}
		for (int i = 337; i <= 342; i++)
			p_clear(i);                 // FLAGW_320 &= 0xff81ffff
		p_lock(p, false);
	PROG_END(p)
}

// 0x57f950 EvProgHangRope: pull one of the six ropes (flags 349..354 from the events). Every
// rope's 343 + j is cleared, the pulled one's corpse drops (355 + j) and, if it was the innocent
// man, 343 + j lets the key event run.
static int s36_hang_rope(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_question(p, 0xd))
		if (p->answer)
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HANG_ROPE))
		for (int j = 0; j < 6; j++) {
			p_clear(343 + j);
			if (p_flag(349 + j)) {
				p_set(355 + j);
				if (hangman_crime(j) == 5)
					p_set(343 + j);
			}
		}
	case 0xd:
		for (int j = 349; j <= 354; j++)
			p_clear(j);                 // FLAGW_320 &= 0x1fffffff, FLAGW_352 &= ~7
		p_lock(p, false);
	PROG_END(p)
}

// 0x57fc80 EvProgReadHangHint: the hint for the riddle level.
static int s36_read_hang_hint(Prog* p)
{
	static const char* const pic[4] = { "dls_p_hanging_easy", "dls_p_hanging_normal", "dls_p_hanging_hard",
	                                    "dls_p_hanging_extra" };
	static const int msg[4] = { 9, 10, 0xb, 0xc }, flag[4] = { 361, 362, 363, 612 };
	if (p->step == 0 && p->sub == 0)
		p_set(flag[p_riddle()]);
	return p_picture_msg(p, pic[p_riddle()], msg[p_riddle()]);
}

// 0x57fd20 EvProgReadPullOnce
static int s36_read_pull_once(Prog* p) { return p_picture_msg(p, "dls_p_hanging_2", 0xe); }

// 0x57fd30 EvProgGetFalseChargeKey: the key of the Persecuted.
static int s36_get_false_charge_key(Prog* p) { return p_item_get(p, 0x2a, 0x1c); }

// 0x57fd40 EvProgCanNotRotate
static int s36_cannot_rotate(Prog* p) { return p_picture_msg(p, "dls_p_manacles_on", 0x11); }

// 0x57fd50 EvProgUseFalseChargeKey: the manacles come off and the handle turns.
static int s36_use_false_charge_key(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "dls_p_manacles_on"))
	PROG_STEP(p, 9, p_item_use(p, 0x2a, 0x1d, 0x465c))
		p->timer = 0.0f;
	// the open manacles fade in over two seconds (prog_overlay.c) and stay until a button
	PROG_STEP(p, 0x10, (prog_overlay_manacles(p->timer * 0.5f), p_wait(p, 2.0f)))
	PROG_STEP(p, 8, (p->down & (KEY_A | KEY_B)) != 0)
		p_set(367);                     // FLAGW_352 |= 0x8000
		prog_overlay_manacles(0.0f);
		p_picture_hide();
	PROG_STEP(p, 0x16, p_demo(p, DEMO_KAITEN))
		p_lock(p, false);
	PROG_END(p)
}

// 0x5801c0 EvProgMariaRedeath
static int s36_maria_redeath(Prog* p)
{
	PROG_BEGIN(p)
		p_set(524);                     // FLAGW_512 |= 0x1000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARIA_REDEATH))
		p_lock(p, false);
	PROG_END(p)
}

// 0x580330 EvProgHoleTo9thLayer: "Will you go down?" sets 372 (FLAGW_352 |= 0x100000).
static int s36_hole_to_9th(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_question(p, 0x25))
		if (!p->answer)
			p_set(372);
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage36[10] = {
	NULL, s36_hanging_corpse, s36_hang_rope, s36_read_hang_hint, s36_read_pull_once, s36_get_false_charge_key,
	s36_cannot_rotate, s36_use_false_charge_key, s36_maria_redeath, s36_hole_to_9th,
};

// ================================================================ stage 37 Stg_eddie_boss (0x8cfa5c)

// 0x57ef10 .. 0x57f2c0 EvProgEddieMurderer1st..4th: the fight with Eddie's scenes.
static int s37_eddie_1(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EDDIE_BOSS_1))
		p_place(100032.55f, 0.0f, -101094.85f, 0.209439f);
		p_lock(p, false);
	PROG_END(p)
}

static int s37_eddie_2(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EDDIE_BOSS_2))
		p_lock(p, false);
	PROG_END(p)
}

static int s37_eddie_3(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EDDIE_BOSS_3))
		p_place(139880.95f, 0.0f, -102912.5f, 0.0759f);
		p_lock(p, false);
	PROG_END(p)
}

static int s37_eddie_4(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_EDDIE_BOSS_4))
		p_place(140215.55f, 0.0f, -100404.25f, 0.9993049f);
		p_set(626);                     // FLAGW_608 |= 0x40000
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage37[5] = { NULL, s37_eddie_1, s37_eddie_2, s37_eddie_3, s37_eddie_4 };

const ProgStage prog_prison_stages[] = {
	{ 3, s_stage28 }, { 2, s_stage29 }, { 8, s_stage30 }, { 11, s_stage31 }, { 8, s_stage32 },
	{ 6, s_stage33 }, { 8, s_stage34 }, { 6, s_stage35 }, { 10, s_stage36 }, { 5, s_stage37 },
};
