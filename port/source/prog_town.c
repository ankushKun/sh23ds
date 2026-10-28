#include "prog_int.h"
#include <stddef.h>

// Stage programs of the town and the opening (stages 2..5 and 14), ported from the PC
// overlays. PS2 beta names come from Event/stage/stg_*.c; programs the Director's Cut added have
// none. Ending programs only hand their cutscene to the demo hook.

// ================================================================ stage 2 Stg_toilet (0x8eaaa8)

// 0x5a1670 EvProgPrologueInToilet: movie toilet.sfd and the mirror demo; James is put back at the
// sink facing west.
static int s02_prologue(Prog* p)
{
	p_demo_number(1);                   // DAT_01f7d3c4 = 1 every frame (the music's duck, bgm.c)
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_FIRST_TOILET))
		p_place(-19606.6f, 0.0f, 20429.55f, -1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x5a18d0 (DC): the hyper spray, offered by FlagInit's flag 631 on later playthroughs.
static int s02_get_hyper_spray(Prog* p) { return p_item_get(p, 0xc, 3); }

// 0x5a18e0 (DC): the memo on the sofa, "I'll wait at BAR Neely's"; the bar gets marked on the map.
static int s02_neely_memo(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 5))
	PROG_STEP(p, 0x1c, p_map_mark(p, 1))       // the bar goes onto the map
	PROG_STEP(p, 0x1e, p_message(p, 8))
		p_set(1499);                    // FLAGW_1472 |= 0x8000000
		p_lock(p, false);
	PROG_END(p)
}

// 0x5a1a00 (DC): the town map on the restroom wall; the symbol on Martin Street goes onto James'
// map unless that already happened.
static int s02_wall_map(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_message(p, 6))
	PROG_STEP(p, 2, p_picture(p, "out_p_outmape"))
		p_set(1512);                    // FLAGW_1504 |= 0x100
		p_set(1489);                    // FLAGW_1472 |= 0x320000
		p_set(1492);
		p_set(1493);
		p_picture_hide();
		if (p_flag(251) || p_flag(1498))
			PROG_JUMP(p, 0xd);
		if (p_flag(47))
			PROG_JUMP(p, 0x1c);
	PROG_STEP(p, 0x1f, p_message(p, 7))
	PROG_STEP(p, 0x1b, p_map_mark(p, 4))
		PROG_JUMP(p, 0x20);
	case 0x1c:                          // flag 47: only the restroom's mark
		if (!p_map_mark(p, 2))
			return 0;
	PROG_STEP(p, 0x20, p_message(p, 8))
	case 0xd:
		if (!p_flag(47))
			p_set(1510);                // FLAGW_1504 |= 0x40
		p_set(1498);                    // FLAGW_1472 |= 0x4000000
		p_lock(p, false);
	PROG_END(p)
}

// 0x5a1ce0 (DC): the blue gem (flag 723 from FlagInit on later playthroughs).
static int s02_get_blue_gem(Prog* p) { return p_item_get(p, 0x4e, 9); }

static const ProgFn s_stage02[6] = { NULL, s02_prologue, s02_get_hyper_spray, s02_neely_memo, s02_wall_map,
                                     s02_get_blue_gem };

// ================================================================ stage 3 Stg_observation (0x8ea6e8)

// 0x5a0a30 EvProgLetterFromMary: "In my restless dreams..." at the observation deck.
static int s03_letter_from_mary(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_TENBOU))
		p_set(517);                     // FLAGW_512 |= 0x20 once the demo passes 1400 frames (demo.c)
		p_place(19350.25f, 0.0f, -507.74f, 0.0f);
		p_set(1553);                    // FLAGW_1536 |= 0x20000
		p_set(1504);                    // FLAGW_1504 |= 1
		p_lock(p, false);
	PROG_END(p)
}

// 0x5a0b80 EvProgItemInCar: the map of Silent Hill on the driver's seat. EvSubPictureDisplay shows
// the empty seat (p_incar) with the map sprite over it (prog_overlay.c); a button takes it (SE
// 0x2b17), the message 0x1c is read over the map, then the map fades from the seat over a second
// (step 0x1b) and the picture goes.
static int s03_item_in_car(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		prog_overlay_incar(1.0f);
	PROG_STEP(p, 2, p_picture(p, "out_p_incar"))
		p_se(0x2b17);
	PROG_STEP(p, 0x1e, p_message(p, 0x1c))
		p->timer = 0.0f;
	PROG_STEP(p, 0x1b, (prog_overlay_incar(1.0f - p->timer), p_wait(p, 1.0f)))
		prog_overlay_incar(0.0f);
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x5a0e70 EvProgNothingTakeInCar
static int s03_nothing_in_car(Prog* p) { return p_picture_msg(p, "out_p_incar", 3); }

// Back off 500 units from where the player stands (the PC walks James to that point).
static int walk_back(Prog* p, float dx, float dz)
{
	if (p->sub == 0) {
		float pos[3], yaw;
		p_player(pos, &yaw);
		p->work[0] = (int)(pos[0] + dx);
		p->work[1] = (int)(pos[2] + dz);
		p->sub = 1;
	}
	return p_walk(p, (float)p->work[0], (float)p->work[1]);
}

// 0x5a0e80 EvProgHadBetterGetMap: leaving the car park without the map.
static int s03_better_get_map(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 2))
	PROG_STEP(p, 0x1b, walk_back(p, 0.0f, -500.0f))
		p_lock(p, false);
	PROG_END(p)
}

// 0x5a0fb0 EvProgCantGoBack: the road James came in on.
static int s03_cant_go_back(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 0))
	PROG_STEP(p, 0x1b, walk_back(p, -500.0f, 0.0f))
		p_lock(p, false);
	PROG_END(p)
}

// 0x5a10e0 EvProgLastScene: the ending at the observation deck (flag 512 from the last stage);
// the PC plays end_mria.sfd and the credits and leaves to the results screen.
static int s03_last_scene(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_END_MARIA))
		p_lock(p, false);
		p_game_end(2);                  // FUN_0048d7e0(2): the "Maria" ending is cleared
	PROG_END(p)
}

static const ProgFn s_stage03[7] = { NULL, s03_letter_from_mary, s03_item_in_car, s03_nothing_in_car,
                                     s03_better_get_map, s03_cant_go_back, s03_last_scene };

// ================================================================ stage 4 Stg_forest (0x8ea108)

// 0x59f480 EvProgFirstSaveWell: the red square in the well, then the save menu (0x4551d0).
static int s04_first_save_well(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "out_p_redpaper"))
	PROG_STEP(p, 10, p_message(p, 0))
		p_picture_hide();
		p_se(0x2743);
		p_set(5);                       // gGameFlag |= 0x20
		p_lock(p, false);
		p_save_menu();
	PROG_END(p)
}

// Angela talks: FUN_00448810() & 1 picks which reply the next talk gets (flag 37).
static void angela_next_talk(void) { ev_flag_set(37, (svcGetSystemTick() >> 4) & 1); }

// 0x5a0740 EvProgAngelaInGrave: movie hakaba.sfd and the graveyard demo, over BGM page 2
// (forest/bgm_022.adx); page 3 afterwards.
static int s04_angela_in_grave(Prog* p)
{
	p_demo_number(3);                   // DAT_01f7d3c4 = 3 every frame: the stage's bgm_control page 4
	PROG_BEGIN(p)
		p_bgm_page(2);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_HAKABA))
		p_bgm_page(3);
		p_place(330.845f, 2473.03f, -1332.315f, -2.3561945f);
		if ((svcGetSystemTick() >> 4) & 1)
			p_set(37);
		p_lock(p, false);
	PROG_END(p)
}

// 0x59f670 EvProgGraveSureQuiet / 0x59f790 EvProgGraveLookingFor: talking to Angela again.
static int s04_grave_sure_quiet(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_ANGELA_GRAVE_2))
		p_place(-179.005f, 2487.65f, -1286.69f, -1.3210f);
		angela_next_talk();
		p_lock(p, false);
	PROG_END(p)
}

static int s04_grave_looking_for(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_ANGELA_GRAVE_3))
		p_place(-493.0f, 2510.19f, -1093.55f, -1.7277f);
		angela_next_talk();
		p_lock(p, false);
	PROG_END(p)
}

// 0x59fc90 EvProgGetChainsaw
static int s04_get_chainsaw(Prog* p)
{
	if (!p_item_get(p, 0x10, 4))
		return 0;
	p_se(0x426b);
	return 1;
}

// 0x59f8b0 EvProgLastScene: the "Leave" ending in the forest (demo mry_yarinaoshi, movie ending.sfd,
// credits; FUN_0048d7e0(1)).
static int s04_last_scene(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_REBIRTH_FOREST))
		p_lock(p, false);
		p_game_end(1);                  // FUN_0048d7e0(1): the "Leave" ending is cleared
	PROG_END(p)
}

// Index 7 (0x59fcd0, DC) switches the game mode (gEventIndex 0xc); not ported.
static const ProgFn s_stage04[8] = { NULL, s04_first_save_well, s04_angela_in_grave, s04_grave_sure_quiet,
                                     s04_grave_looking_for, s04_get_chainsaw, s04_last_scene, NULL };

// ================================================================ stage 5 Stg_town_east (0x8e8358)

// 0x59d2a0 (EvProgGetWrench in the PS2 town west): the wrench, later in the game.
static int s05_get_wrench(Prog* p) { return p_item_get(p, 0x3a, 0x21); }

// 0x59d2b0 (DC): the note with the wrench picture.
static int s05_spana_note(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "out_p_spana"))
	PROG_STEP(p, 10, p_message(p, 0x20))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x59d4f0 EvProgShadowOfSomenthing: the blood marks, then a figure walks off up the road in the fog
// (a Lying Figure the program creates, status 0x11; the scene machi is the camera alone).
static int s05_shadow(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		p_place(5517.262f, 0.0f, -38671.51f, -1.5707964f);
		p_figure(-2, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);             // its model, read while James walks
	PROG_STEP(p, 0x1b, p_walk(p, 3144.163f, -38703.02f))
	PROG_STEP(p, 0x1e, p_message(p, 0x1c))
		// 0x48fcc0: the figure on the road (0x8e9880), a Lying Figure under event control that
		// walks off. The PC creates it as the scene starts; its footage has it still beside the
		// truck when the camera gets there, three seconds in, and walking off into the fog from
		// there, which the port's walk only matches when it starts then.
		p_figure(0x11, 2125.0f, 0.0f, -32300.0f, 0.0f, 3.0f);
	PROG_STEP(p, 0x17, p_demo(p, DEMO_SHADOW))
		p_place(2125.0f, 0.0f, -37000.0f, 0.0f);
	PROG_STEP(p, 0x20, p_message(p, 0x1d))
		p_figure(-1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);             // 0xd: 0x53a350(0, 0x200, 0)
		p_lock(p, false);
	PROG_END(p)
}

// 0x59d8f0 EvProgSandersBack: "That shadow just now bothers me", James turns back.
static int s05_sanders_back(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 0x1e))
	PROG_STEP(p, 0x1b, walk_back(p, 500.0f, 0.0f))
		p_lock(p, false);
	PROG_END(p)
}

// 0x59da20 EvProgFirstMonsterStart: the first Lying Figure; James picks up the plank and the
// radio and holds the plank.
static int s05_first_monster_start(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_FIRST_MONSTER))
		p_item_get_now(0xd);
		p_item_get_now(0x13);
		p_place_keep_yaw(47408.53f, 0.0f, 49275.65f);
		p_set(518);                     // FLAGW_512 |= 0x40
		p_lock(p, false);
	PROG_END(p)
}

// 0x59dbd0 EvProgFirstMonsterCorpse
static int s05_first_monster_corpse(Prog* p)
{
	PROG_BEGIN(p)
		p_clear(518);
		p_set(41);                      // FLAGW_32 |= 0x200
	PROG_STEP(p, 0x17, p_demo(p, DEMO_FIRST_MONSTER_CORPSE))
		p_place_keep_yaw(47207.766f, 0.0f, 50547.0f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x59dd40 EvProgFirstMonsterEnd
static int s05_first_monster_end(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x18, p_demo(p, DEMO_FIRST_MONSTER_END))
		p_place(47273.445f, 0.0f, 48630.1f, 3.1415927f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x59de50 EvProgBloodSwamp: the swamp monument; the second reading has the full text.
static int s05_blood_swamp(Prog* p)
{
	if (p->step == 0 && p->sub == 0 && p_flag(251))
		p_set(628);
	return p_picture_msg(p, p_flag(251) ? "out_p_swamp2" : "out_p_swamp", p_flag(44) ? 5 : 4);
}

// 0x59dea0 EvProgGetApartGateKey
static int s05_get_gate_key(Prog* p)
{
	if (p->sub == 0 && p_flag(1510))
		p_set(1511);
	return p_item_get(p, 0x1a, 0x12);
}

// 0x59dec0 EvProgUseApartGateKey
static int s05_use_gate_key(Prog* p) { return p_item_use(p, 0x1a, 0x13, 0x4076); }

// 0x59dee0 EvProgLookMemoFalled: the pages of the memo pad scattered in Neely's area. The event
// sets the "now reading page n" flag (51, 53, 55, 57, 59, 611) on the way in, the program shows
// the intro the first time and the page, then clears the reading flags again.
static int s05_memo_falled(Prog* p)
{
	static const int page_flag[6] = { 51, 53, 55, 57, 59, 611 };
	switch (p->step) {
	case 0:
		p_lock(p, true);
		if (p_flag(51) ? p_flag(50) : (p_flag(52) || p_flag(54) || p_flag(58) || p_flag(56) || p_flag(610)))
			PROG_JUMP(p, 0x1f);
		p->step = 0x1e, p->sub = 0;
		// fallthrough
	case 0x1e:
		if (!p_message(p, p_flag(51) ? 0x1a : 0x1b))
			return 0;
		p->step = 0x1f, p->sub = 0;
		// fallthrough
	case 0x1f:
		while (p->work[0] < 6) {
			if (p_flag(page_flag[p->work[0]]) && !p_message(p, 0x14 + p->work[0]))
				return 0;
			p->work[0]++;
			p->sub = 0;
		}
		// fallthrough
	default:
		for (int i = 0; i < 6; i++)
			p_clear(page_flag[i]);
		p_lock(p, false);
		return 1;
	}
}

static const ProgFn s_stage05[12] = {
	NULL, s05_get_wrench, s05_spana_note, s05_shadow, s05_sanders_back, s05_first_monster_start,
	s05_first_monster_corpse, s05_first_monster_end, s05_blood_swamp, s05_get_gate_key, s05_use_gate_key,
	s05_memo_falled,
};

// ================================================================ stage 14 Stg_town_west (0x8df5b8)

// Timed subtitles (the PC opens stage message first + i between the frame pairs of a table, at 30
// frames a second, while the camera runs). They go to the panel so they close on their own.
static int subtitles(Prog* p, const u16 (*t)[2], int n, int first, float end_frames)
{
	p->timer += p->dt * 30.0f;
	const char* text = NULL;
	for (int i = 0; i < n; i++)
		if (p->timer > t[i][0] && p->timer <= t[i][1])
			text = p_stage_text(first + i);
	p_panel(text);
	if (p->timer <= end_frames && !(p->down & KEY_START))
		return 0;
	p_panel(NULL);
	return 1;
}

// 0x591fb0 (no PS2 counterpart): arriving at Rosewater Park, "So this is that park...".
static int s14_park_arrival(Prog* p)
{
	static const u16 t[2][2] = { { 45, 150 }, { 150, 255 } };      // 0x8e0a3c
	PROG_BEGIN(p)
		p_lock(p, true);
		p_place(23445.4f, 1991.93f, 78547.12f, 0.4093992f);
	PROG_STEP(p, 10, subtitles(p, t, 2, 0x6f, 270.0f))     // _DAT_0069c5bc
		p_lock(p, false);
	PROG_END(p)
}

// 0x592160: the map board in the park; the hotel goes onto James' map.
static int s14_park_map(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_message(p, 0x5f))
	PROG_STEP(p, 2, p_picture(p, "out_p_outmapw"))
		p_picture_hide();
		if (p_flag(251) || p_flag(1536))
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x1c, p_map_mark(p, 3))       // the hotel goes onto the map
	PROG_STEP(p, 0x1f, p_message(p, 0x6e))
	case 0xd:
		p_set(1536);                    // FLAGW_1536 |= 1
		p_set(1530);                    // FLAGW_1504 |= 0x4000000
		p_lock(p, false);
	PROG_END(p)
}

// 0x592430: after Maria's death, James' thoughts over the camera (two subtitle tables).
static int s14_after_maria(Prog* p)
{
	static const u16 t1[6][2] = { { 60, 165 }, { 165, 270 }, { 270, 375 }, { 420, 510 }, { 540, 645 }, { 645, 720 } };
	static const u16 t2[1][2] = { { 30, 135 } };
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, subtitles(p, t1, 6, 0x61, 750.0f))
		p->timer = 0.0f;
	PROG_STEP(p, 0x20, subtitles(p, t2, 1, 0x67, 150.0f))
		p_lock(p, false);
	PROG_END(p)
}

// 0x592720: "I have to go after Laura", James turns back.
static int s14_after_laura(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 0x5a))
	PROG_STEP(p, 0x1b, walk_back(p, 0.0f, 500.0f))
		p_lock(p, false);
	PROG_END(p)
}

// 0x594850, probably EvProgLauraOnWall (movie hei.sfd, "hei" is the fence Laura climbs).
static int s14_laura_fence(Prog* p)
{
	PROG_BEGIN(p)
		p_set(659);                     // FLAGW_640 |= 0x80000
	PROG_STEP(p, 0x17, p_demo(p, DEMO_LAURA_FENCE))
		p_place(21950.0f, 1955.0f, 47284.0f, -0.9306f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x592830 EvProgMariaMeetingStandby: waits for the movie stream, puts Maria in the park.
static int s14_maria_standby(Prog* p)
{
	(void)p;
	p_set(519);                         // FLAGW_512 |= 0x80
	return 1;
}

// 0x5928e0 EvProgMariaMeeting: movie deai.sfd; Maria follows James from here (flag 15).
static int s14_maria_meeting(Prog* p)
{
	p_demo_number(0x15);                // DAT_01f7d3c4 = 0x15 every frame
	PROG_BEGIN(p)
		p_set(525);                     // FLAGW_512 |= 0x2000 while it plays
		// BGM page 0xd: EvProgMariaMeetingStandby (0x592830) asks for it and holds it (0x1f837d0),
		// this program lets it start; the hold is not ported, so the page is asked for here
		p_bgm_page(0xd);
	PROG_STEP(p, 0x17, p_demo(p, DEMO_MARIA_MEETING))
		p_set(15);                      // gGameFlag |= 0x8000
		p_place(4580.87f, 3000.0f, 103940.0f, -1.5707964f);
		p_set(1554);                    // FLAGW_1536 |= 0xc0000
		p_set(1555);
		p_set(1505);                    // FLAGW_1504 |= 2
		p_clear(525);
		p_clear(519);
		p_bgm_page(0xb);
		p_lock(p, false);
	PROG_END(p)
}

// 0x592c90 EvProgMariaNotHotel
static int s14_maria_not_hotel(Prog* p)
{
	PROG_BEGIN(p)
		p_set(655);                     // FLAGW_640 |= 0x8000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARIA_NOT_HOTEL))
		p_place(18913.77f, 3000.0f, 103259.0f, 2.7633948f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x592e00 EvProgBowlDemoDataLoad: loads the next demo only.
static int s14_bowl_demo_load(Prog* p) { (void)p; return 1; }

// 0x592e40 EvProgMariaFrontBowling1st: Maria stays behind at the bowling alley.
static int s14_maria_bowling_1(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARIA_BOWLING_1))
		p_clear(15);
		p_lock(p, false);
	PROG_END(p)
}

// 0x592fd0 EvProgMariaFrontBowling2nd: Maria comes along again.
static int s14_maria_bowling_2(Prog* p)
{
	PROG_BEGIN(p)
		p_set(658);                     // FLAGW_640 |= 0x40000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARIA_BOWLING_2))
		p_place(-90447.0f, 0.0f, 67931.0f, 2.3561945f);
		p_set(15);
		p_lock(p, false);
	PROG_END(p)
}

// 0x593100, probably EvProgLauraCantGo: a demo by the alley; James ends up where he stood, moved
// by the demo's offset (0x8e0ddc - 0x73c74c).
static int s14_alley_talk(Prog* p)
{
	PROG_BEGIN(p)
		p_set(656);                     // FLAGW_640 |= 0x10000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARIA_ALLEY))
		{
			float pos[3], yaw;
			p_player(pos, &yaw);
			p_place(pos[0] + (-88770.5f + 88717.0f), 0.0f, pos[2] + (70438.0f - 70440.0f), 1.8126431f);
		}
		p_lock(p, false);
	PROG_END(p)
}

// 0x593300 EvProgEnemyLoadWestTownStart
static int s14_enemy_load(Prog* p)
{
	(void)p;
	p_set(153);                         // FLAGW_128 |= 0x2000000
	return 1;
}

// 0x593330 EvProgEnemyLoadWestTownInit
static int s14_nothing(Prog* p) { (void)p; return 1; }

// 0x593340 EvProgLadyHoleMake: dig where the ground is raised (Heaven's Night lady's hint).
static int s14_dig(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_question(p, 1))
		if (p->answer)
			PROG_JUMP(p, 0xd);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_DIG))
		p_place(-17500.0f, 1900.0f, 87200.0f, 0.0f);
		p_set(252);                     // FLAGW_224 |= 0x10000000
	case 0xd:
		p_lock(p, false);
	PROG_END(p)
}

// 0x593570 EvProgUseOldBronzeKey
static int s14_use_bronze_key(Prog* p) { return p_item_use(p, 0x28, 0x4b, 0x4a41); }

// 0x594a90 EvProgUseWrench: the bolts of the buried box come off, the old bronze key is inside.
static int s14_use_wrench(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "out_statuebox"))
	PROG_STEP(p, 9, p_item_use(p, 0x3a, 0x49, 0))
	PROG_STEP(p, 0x12, p_item_get_quiet(p, 0x28, 0x4a))
		p_picture_hide();
		p_set(258);                     // FLAGW_256 |= 4
		p_lock(p, false);
	PROG_END(p)
}

// 0x5937c0 EvProgLookMetalBox
static int s14_look_metal_box(Prog* p)
{
	if (p_flag(257))
		return p_picture_msg(p, "out_statuekey", 0x5e);
	return p_picture_msg(p, "out_statuebox", p_flag(260) ? 2 : 3);
}

// 0x593800 EvProgLauraGoHospital
static int s14_laura_hospital(Prog* p)
{
	PROG_BEGIN(p)
		p_set(660);                     // FLAGW_640 |= 0x100000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LAURA_HOSPITAL))
		p_place(-81181.5f, 0.0f, -3599.675f, -2.3561945f);
		p_set(1537);                    // FLAGW_1536 |= 2
		p_lock(p, false);
	PROG_END(p)
}

// 0x593940 EvProgGetSteelPipe
static int s14_get_steel_pipe(Prog* p)
{
	if (!p_item_get(p, 0xe, 0x47))
		return 0;
	p_se(0x426a);
	return 1;
}

// 0x593980 EvProgGetLostMemory: the book in the bookstore, then its first lines.
static int s14_get_lost_memory(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_item_get_quiet(p, 0x4a, 10))
	PROG_STEP(p, 2, p_picture(p, "out_p_lostmemory"))
	PROG_STEP(p, 10, p_message(p, 0xb))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x593ba0 EvProgGetDogKey
static int s14_get_dog_key(Prog* p) { return p_item_get(p, 0x19, 0x4d); }

// 0x593bb0 EvProgEndMaria: the ending demo (end_maria_e).
static int s14_end_maria(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_END_MARIA_TOWN))
		p_lock(p, false);
	PROG_END(p)
}

// Index 24 (0x5947e0) is an empty function and 25 (0x5954b0) the stage's per-frame light and
// radio code; no event runs them.
static const ProgFn s_stage14[26] = {
	NULL, s14_park_arrival, s14_park_map, s14_after_maria, s14_after_laura, s14_laura_fence,
	s14_maria_standby, s14_maria_meeting, s14_maria_not_hotel, s14_bowl_demo_load, s14_maria_bowling_1,
	s14_maria_bowling_2, s14_alley_talk, s14_enemy_load, s14_nothing, s14_dig, s14_use_bronze_key,
	s14_use_wrench, s14_look_metal_box, s14_laura_hospital, s14_get_steel_pipe, s14_get_lost_memory,
	s14_get_dog_key, s14_end_maria, NULL, NULL,
};

const ProgStage prog_town_stages[] = {
	{ 6, s_stage02 }, { 7, s_stage03 }, { 8, s_stage04 }, { 12, s_stage05 },
};

const ProgStage prog_town_west_stage = { 26, s_stage14 };
