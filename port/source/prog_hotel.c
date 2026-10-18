#include "prog_int.h"
#include "stagemdl.h"
#include "boat.h"
#include <stdio.h>
#include <string.h>

// Stage programs of Toluca Lake, the Lakeview Hotel and the endings (stages 38..52), ported from
// the PC overlays; PS2 beta names from Event/stage/stg_lake.c .. stg_end_dog.c. The ending
// stages' code lives in the main exe (0x43eac0..0x440f40) on the PC. Flags, items, messages and SE
// ids are the PC's; cutscenes go to prog_demo_request(), endings to the game_end hook.

// ================================================================ stage 38 Stg_lake (0x8cf598)

// 0x57dd20 EvProgEmbarkBoat: the scene, then James in boat mode (0x20000 in his status) in the
// boat on the water, both at 0x8cf688 facing -pi / 2 (boat.c rows it). The event sets flag 379.
static int s38_embark_boat(Prog* p)
{
	PROG_BEGIN(p)
		p_set(654);                     // FLAGW_640 |= 0x4000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LAKE_BOAT_IN))
		boat_embark();
		p_place(-14970.1f, 3592.5f, -98254.0f, -1.5707964f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x57de40 EvProgDisembarkBoat: rowing into the hotel pier's area (event 3), the scene, James on the
// pier (0x8cf6ec) and the boat left at it (0x8cf6dc, facing pi, motion 54).
static int s38_disembark_boat(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_LAKE_BOAT_OUT))
		boat_disembark();
		p_place(-9560.6f, 3185.0f, -18665.95f, 0.0f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x57df80 EvProgGetLittleMermaid
static int s38_get_little_mermaid(Prog* p) { return p_item_get(p, 0x43, 1); }

// 0x57df90 EvProgEndRebirth: movie end_rbrt.sfd and the credits (FUN_0048d7e0(8)).
static int s38_end_rebirth(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_END_REBIRTH))
		p_lock(p, false);
		p_game_end(8);
	PROG_END(p)
}

// 0x57e9e0 (DC): the blue gem at the lake, the UFO scene.
static int s38_ufo(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 1, p_demo(p, DEMO_UFO_LAKE))
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage38[6] = { NULL, s38_embark_boat, s38_disembark_boat, s38_get_little_mermaid,
                                     s38_end_rebirth, s38_ufo };

// ================================================================ stage 39 Stg_hotel_bf_f (0x8cf198)

// 0x57d530 EvProgGetThinner / 0x57d540 EvProgGetBarKey
static int s39_get_thinner(Prog* p) { return p_item_get(p, 0x42, 7); }
static int s39_get_bar_key(Prog* p) { return p_item_get(p, 0x2e, 8); }

// 0x57d550 EvProgNoLabelCan
static int s39_no_label_can(Prog* p)
{
	if (p->step == 0 && p->sub == 0)
		p_set(2085);                    // FLAGW_2080 |= 0x20
	return p_picture_msg(p, "htl_p_can_close", 2);
}

// 0x57d570 EvProgGetLightbulb: the can opener on the can; a light bulb was inside.
static int s39_get_lightbulb(Prog* p)
{
	PROG_BEGIN(p)
		p_set(2085);                    // FLAGW_2080 |= 0x60
		p_set(2086);
	PROG_STEP(p, 0x16, p_demo(p, DEMO_CAN))
	PROG_STEP(p, 2, p_picture(p, "htl_p_can_open"))
	PROG_STEP(p, 10, p_message(p, 3))
	PROG_STEP(p, 0x12, p_item_get_quiet(p, 0x47, 9))
		p_picture_hide();
		p_place(-60256.0f, 0.0f, -18960.215f, 0.0f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x57da50 EvProgUseLightbulb / 0x57da80 EvProgUseBarKey
static int s39_use_lightbulb(Prog* p)
{
	if (p->sub == 0)
		p_set(386);                     // FLAGW_384 |= 4
	return p_item_use(p, 0x47, 0xc, 0x4dc1);
}

static int s39_use_bar_key(Prog* p) { return p_item_use(p, 0x2e, 0xb, 0x4a3b); }

static const ProgFn s_stage39[7] = { NULL, s39_get_thinner, s39_get_bar_key, s39_no_label_can, s39_get_lightbulb,
                                     s39_use_lightbulb, s39_use_bar_key };

// ================================================================ stage 40 Stg_hotel_1f_f (0x8ce608)

// 0x57b580 EvProgGetHotelGuestMap / 0x57b590 EvProgGetHotelEmployeeMap
static int s40_guest_map(Prog* p) { return p_map_get(p, "map_hotelmap3f01", 0x10); }
static int s40_employee_map(Prog* p) { return p_map_get(p, "map_hotelmap1f02", 0x12); }

// 0x57b5a0 EvProgGetHotel312Key / 0x57b5b0 EvProgReadReceptionMemo
static int s40_get_312_key(Prog* p) { return p_item_get(p, 0x2b, 0x11); }
static int s40_reception_memo(Prog* p) { return p_picture_msg(p, "htl_p_receipt", 2); }

// The music box (0x57c470 EvProgOrgel): three seats for the "Snow White" (69), "Little Mermaid"
// (67) and "Cinderella" (68) boxes. Per seat s: 448 + s is "James looks at it", 451 + 3s + b
// "box b is being put here" (from the use events), 460 + 4s "a box sits here" and 461 + 4s + b
// which one. The riddle level decides the order (0x1e messages, solution below); a right order
// plays the tune and sets 472 (the stairway key comes out, 0x57bcc0).
static const int s_box_item[3] = { 0x45, 0x43, 0x44 };   // Snow White, Little Mermaid, Cinderella
static const int s_box_put_msg[3] = { 0x18, 0x19, 0x1a };
static const int s_box_take_msg[3] = { 0x1b, 0x1c, 0x1d };

static bool music_box_solved(void)
{
	if (p_riddle() == 0)
		return p_flag(460) && p_flag(464) && p_flag(468);
	if (p_riddle() == 1)
		return p_flag(462) && p_flag(465) && p_flag(471);
	return p_flag(461) && p_flag(466) && p_flag(471);
}

static int s40_orgel(Prog* p)
{
	static const int riddle_msg[4][3] = { { 7, 7, 7 }, { 9, 13, 11 }, { 12, 8, 10 }, { 12, 8, 10 } };
	switch (p->step) {
	case 0: {
		p_set(2109);                    // FLAGW_2080 |= 0x20000000
		p_lock(p, true);
		// a box being put down
		for (int b = 0; b < 3; b++)
			for (int s = 0; s < 3; s++)
				if (p_flag(451 + 3 * s + b)) {
					p->work[1] = b;
					PROG_JUMP(p, 0x21);
				}
		// looking at a seat (448 / 449, otherwise the third): offer to take its box, or read its plate
		int s = p_flag(448) ? 0 : p_flag(449) ? 1 : 2;
		p->work[2] = s;
		for (int b = 0; b < 3; b++)
			if (p_flag(461 + 4 * s + b)) {
				p->work[1] = b;
				PROG_JUMP(p, 0x24);
			}
		PROG_JUMP(p, 0x1e);
	}
	case 0x1e:
		if (!p_message(p, 6))
			return 0;
		p->step = 0x1f, p->sub = 0;
		// fallthrough
	case 0x1f:
		if (!p_message(p, riddle_msg[p_riddle()][p->work[2]]))
			return 0;
		PROG_JUMP(p, 6);
	case 0x21: {                    // put the box down on every seat that asked for it
		int b = p->work[1];
		if (!p_item_use(p, s_box_item[b], s_box_put_msg[b], 0))
			return 0;
		p_lock(p, true);                // p_item_use hands control back
		for (int s = 0; s < 3; s++)
			if (p_flag(451 + 3 * s + b)) {
				p_clear(451 + 3 * s + b);
				p_set(460 + 4 * s);
				p_set(461 + 4 * s + b);
				p_se(0x4dc2);
			}
		PROG_JUMP(p, 6);
	}
	case 0x24:                      // "Take the music box?"
		if (!p_question(p, s_box_take_msg[p->work[1]]))
			return 0;
		if (p->answer)
			PROG_JUMP(p, 6);
		p_clear(460 + 4 * p->work[2]);
		p_clear(461 + 4 * p->work[2] + p->work[1]);
		p_item_get_now(s_box_item[p->work[1]]);
		PROG_JUMP(p, 0xd);
	case 6:                         // "Play the music box?"
		if (!p_question(p, 4))
			return 0;
		if (p->answer)
			PROG_JUMP(p, 0xd);
		p->work[3] = music_box_solved();
		if (p->work[3])
			p_set(472);                 // FLAGW_448 |= 0x1000000
		p->timer = 0.0f;
		p->step = 0x18, p->sub = 0;
		// fallthrough
	case 0x18:                      // the PC loads org_b for the right order, org_a otherwise
		if (p->work[3]) {
			p->timer += p->dt;          // 0x57b5c0(1, 15.0): the box opens and the tune turns it
			stagemdl_music_box(p->timer);
		}
		if (!p_demo(p, p->work[3] ? DEMO_MUSIC_BOX : DEMO_MUSIC_BOX_WRONG))
			return 0;
		stagemdl_music_box(-1.0f);
		if (p->work[3])
			PROG_JUMP(p, 0xd);
		p->step = 10, p->sub = 0;
		// fallthrough
	case 10:
		if (!p_message(p, 5))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_clear(448);                   // FLAGW_448 &= ~7
		p_clear(449);
		p_clear(450);
		p_lock(p, false);
		return 1;
	}
}

// 0x57bcc0 EvProgGetHotelStairKey: out of the music box.
static int s40_get_stair_key(Prog* p)
{
	if (p->sub == 0)
		p_set(2113);                    // FLAGW_2112 |= 2
	return p_item_get(p, 0x30, 0x13);
}

// 0x57d190 EvProgGetVideoTape: the videotape and the can opener in the office.
static int s40_get_video_tape(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 2, p_picture(p, "htl_p_video"))
	PROG_STEP(p, 0x24, p_item_get_quiet(p, 0x17, 0x14))
	PROG_STEP(p, 0x1e, p_message(p, 0x43))
	PROG_STEP(p, 0x26, p_item_get_quiet(p, 0x46, 0x15))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x57be80 EvProgNothingInSafe / 0x57be90 EvProgGetSnowWhite / 0x57bea0 EvProgGetFishKey
static int s40_nothing_in_safe(Prog* p) { return p_picture_msg(p, "htl_p_video", 0xf); }
static int s40_get_snow_white(Prog* p) { return p_item_get(p, 0x45, 0x16); }
static int s40_get_fish_key(Prog* p) { return p_item_get(p, 0x2f, 0x17); }

// 0x57beb0 EvProgLauraPlayPiano: Laura in the lounge (the voice, piano_lau_b), her letter on screen
// with message 0x2e, then the rest of the scene (piano_lau_e) and the letter (item 22).
static int s40_laura_piano(Prog* p)
{
	PROG_BEGIN(p)
		p_set(636);                     // FLAGW_608 |= 0x10000000
		p_lock(p, true);
	PROG_STEP(p, 0x17, p_demo(p, DEMO_LAURA_PIANO))
		p_set(528);                     // FLAGW_512 |= 0x10000 during the demo
	PROG_STEP(p, 2, p_picture(p, "htl_p_laura_letter"))
	PROG_STEP(p, 10, p_message(p, 0x2e))
		p_picture_hide();
		p_set(383);                     // FLAGW_352 |= 0x80000000
	PROG_STEP(p, 0x18, p_demo(p, DEMO_LAURA_PIANO_END))
		p_item_get_now(0x16);
		p_place(-59930.5f, 0.0f, 61014.5f, 0.0f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x57c280: returns 1.
static int s40_nothing(Prog* p) { (void)p; return 1; }

static const ProgFn s_stage40[13] = {
	NULL, s40_guest_map, s40_employee_map, s40_get_312_key, s40_reception_memo, s40_orgel, s40_get_stair_key,
	s40_get_video_tape, s40_nothing_in_safe, s40_get_snow_white, s40_get_fish_key, s40_laura_piano, s40_nothing,
};

// ================================================================ stage 41 Stg_hotel_2f_f (0x8ccf38)

// The shelf by the employee elevator (0x579220 EvProgItemPutForShelf): items and the flag each
// sets while it is on the shelf (table 0x8cd158). Counted items keep their count in
// 0x1f7d9c0..0x1f7d9ca (one short per item of 0x8cd1d4).
static const short s_shelf[][2] = {
	{ 21, 417 }, { 6, 418 }, { 8, 419 }, { 12, 420 }, { 14, 421 }, { 15, 422 }, { 16, 423 }, { 33, 424 },
	{ 43, 425 }, { 66, 426 }, { 67, 427 }, { 68, 428 }, { 74, 429 }, { 76, 430 }, { 77, 431 }, { 25, 432 },
	{ 1, 433 }, { 2, 434 }, { 3, 435 }, { 4, 436 }, { 5, 437 }, { 7, 438 }, { 9, 439 }, { 13, 440 },
	{ 18, 441 }, { 19, 442 }, { 20, 443 }, { 22, 444 }, { 24, 445 }, { 78, 730 },
};
#define SHELF_N (int)(sizeof s_shelf / sizeof *s_shelf)
static const short s_shelf_counted[6] = { 5, 7, 9, 1, 2, 3 };

static short* shelf_count(int i) { return (short*)p_flag_bytes(0x1f7d9c0) + i; }

static void shelf_put(int item)
{
	for (int i = 0; i < SHELF_N; i++)
		if (s_shelf[i][0] == item) {
			p_set(s_shelf[i][1]);
			p_set(446);                 // FLAGW_416 |= 0x40000000: something is on the shelf
			for (int k = 0; k < 6; k++)
				if (s_shelf_counted[k] == item)
					*shelf_count(k) += (short)ev_item_count(item);
			ev_give_item(item, -ev_item_count(item));
		}
	bool any = false;
	for (int i = 0; i < SHELF_N; i++)
		any |= ev_have_item(s_shelf[i][0]);
	if (!any)
		p_set(400);                     // FLAGW_384 |= 0x10000
}

// 0x579500 / 0x5796e0: everything comes back.
static void shelf_take_all(void)
{
	for (int i = 0; i < SHELF_N; i++)
		if (p_flag(s_shelf[i][1])) {
			bool counted = false;
			for (int k = 0; k < 6; k++)
				counted |= s_shelf_counted[k] == s_shelf[i][0];
			if (!counted)
				ev_give_item(s_shelf[i][0], 1);
			p_clear(s_shelf[i][1]);
		}
	for (int k = 0; k < 6; k++) {
		int n = *shelf_count(k);
		if (ev_item_count(s_shelf_counted[k]) + n > 999)
			n = 999 - ev_item_count(s_shelf_counted[k]);
		ev_give_item(s_shelf_counted[k], n);
		*shelf_count(k) = 0;
	}
	p_clear(446);
	p_se(0x2b21);
}

// The weight check of 0x57a4a0 (stage per-frame code, elevator room): with none of the heavy items
// James may ride (400); 401 is the buzzer set by the elevator's floor event.
static bool carrying_heavy(void)
{
	static const u8 heavy[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 18, 19, 22, 24, 25, 33, 43,
	                            66, 67, 68, 74, 76, 77, 78 };
	for (u32 i = 0; i < sizeof heavy; i++)
		if (ev_have_item(heavy[i]))
			return true;
	return false;
}

// The trunk's drums (0x1f7da7c..f): four drums of eight letters; the word is one of 19, picked
// by FlagInit's flags 406..410 (EvStageInit 0x57a340 puts every drum off its letter).
static const char s_drum_letters[4][9] = { "dhklmnot", "aeiopuvx", "celmrsvw", "aeklnprt" };
static const char s_trunk_words[] = "opendamnhelltowndarkmamadownlovelockmistlucklosedoseoverdusttimehelpkillnull";

static int trunk_word(void)
{
	int v = 0;
	for (int b = 0; b < 5; b++)
		v |= p_flag(406 + b) << b;
	return v < 19 ? v : 0;
}

static void trunk_init(void)
{
	if (p_flag(405))
		return;
	u8* drum = p_puzzle() + 0x1c;
	const char* w = s_trunk_words + trunk_word() * 4;
	for (int d = 0; d < 4; d++) {
		int k = (int)(strchr(s_drum_letters[d], w[d]) - s_drum_letters[d]);
		int v = p_rand() % 7 + k + 1;
		drum[d] = (u8)(v > 7 ? v - 8 : v);
	}
	p_set(405);                         // FLAGW_384 |= 0x200000
}

void prog_hotel_tick(int stage)
{
	if (stage != 41)
		return;
	trunk_init();
	if (!carrying_heavy()) {
		p_set(400);
		p_clear(401);
	} else {
		p_clear(400);
	}
}

// 0x579220 EvProgItemPutForShelf: the PC runs it from the item menu's "Put on shelf"; here the
// shelf panel of 0x5796e0 calls shelf_put directly.
static int s41_put_for_shelf(Prog* p) { (void)p; return 1; }

// 0x579420 EvProgUseHotel204Key / 0x579440 EvProgUseHotelElevatorKey
static int s41_use_204_key(Prog* p) { return p_item_use(p, 0x2c, 0xe, 0x4a3b); }
static int s41_use_elevator_key(Prog* p) { return p_item_use(p, 0x2d, 0xf, 0x4a3b); }

static void button_panel(int sel, int n)
{
	char buf[200];
	int k = snprintf(buf, sizeof buf, "\n   Buttons ");
	for (int i = 0; i < n; i++)
		k += snprintf(buf + k, sizeof buf - k, "%c%d%c", sel == i ? '[' : ' ', i + 1, sel == i ? ']' : ' ');
	snprintf(buf + k, sizeof buf - k, "\n\n\n\n\n\n\n   D-pad: button  A: press  B: leave");
	p_panel(buf);
}

// D-pad moves the cursor the PC's mouse / stick drives over the still (gPuzzleCursor*).
static void button_cursor(Prog* p, int n)
{
	if (p->down & (KEY_DUP | KEY_DLEFT)) p->work[1] = (p->work[1] + n - 1) % n;
	if (p->down & (KEY_DDOWN | KEY_DRIGHT)) p->work[1] = (p->work[1] + 1) % n;
	button_panel(p->work[1], n);
}

// 0x57a790 EvProgElvatorButton: the employee elevator. Of its six buttons (0x579460) the third
// goes down, unless the buzzer (401, too much weight) is on; leaving with the buzzer on says so.
static int s41_elevator(Prog* p)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p_picture_show("htl_p_htl_elevator");
		p->step = 7;
		// fallthrough
	case 7:
		button_cursor(p, 6);
		if (p->down & KEY_A) {
			p_se(0x4a49);
			if (p->work[1] == 2 && !p_flag(401)) {
				p_panel(NULL);
				p_picture_hide();
				p_se(0x4db9);
				p->timer = 0.0f;
				PROG_JUMP(p, 0x17);
			}
		}
		if (!(p->down & KEY_B))
			return 0;
		p_panel(NULL);
		if (p_flag(401))
			PROG_JUMP(p, 0x1e);
		PROG_JUMP(p, 0xd);
	case 0x17:                      // doors close, the car starts (about a second)
		if (!p_wait(p, 1.0f))
			return 0;
		p_se(0x4a43);
		p->timer = 0.0f;
		p->step = 0x18;
		// fallthrough
	case 0x18:                      // _DAT_0069f3bc: eight seconds down
		if (!p_wait(p, 8.0f))
			return 0;
		p_se(0x4a44);
		p->timer = 0.0f;
		p->step = 0x19;
		// fallthrough
	case 0x19:
		if (!p_wait(p, 1.0f))
			return 0;
		p_se(0x4a42);
		p->timer = 0.0f;
		p->step = 0x1b;
		// fallthrough
	case 0x1b:
		if (!p_wait(p, 1.0f))
			return 0;
		p_set(21);                      // gGameFlag |= 0x200000: the stage's door event goes down
		PROG_JUMP(p, 0xd);
	case 0x1e:
		if (!p_message(p, 6))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x579500 EvProgGetBaggageInShelf: take everything back.
static int s41_get_baggage(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_question(p, 10))
		if (!p->answer)
			shelf_take_all();
		p_lock(p, false);
	PROG_END(p)
}

static void shelf_panel(Prog* p, int* items, int n)
{
	char buf[400];
	int k = snprintf(buf, sizeof buf, " Put on the shelf:\n");
	int first = p->work[1] > 6 ? p->work[1] - 6 : 0;
	for (int i = first; i < n && i < first + 7; i++) {
		const char* name = ev_item_name(items[i]);
		char line[30];
		snprintf(line, sizeof line, "%s", name ? name : "?");
		char* nl = strchr(line, '\n');
		if (nl) *nl = 0;
		k += snprintf(buf + k, sizeof buf - k, "  %c %s\n", p->work[1] == i ? '>' : ' ', line);
	}
	if (!n)
		k += snprintf(buf + k, sizeof buf - k, "   (nothing left to put)\n");
	snprintf(buf + k, sizeof buf - k, "\n   D-pad: item  A: put  B: done");
	p_panel(buf);
}

// 0x5796e0 EvProgPutBaggageForShelf: "Put an item on the shelf?", then the PC's item menu in
// shelf mode (gEventIndex 6); here a panel lists what can go on the shelf.
static int s41_put_baggage(Prog* p)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->step = 10;
		// fallthrough
	case 10:
		if (!p_question(p, p_flag(446) ? 0x1f : 8))
			return 0;
		if (!p->answer) {
			p_set(416);                 // FLAGW_416 |= 1 while the menu is open
			p->work[1] = 0;
			PROG_JUMP(p, 0x20);
		}
		if (p_flag(446))
			PROG_JUMP(p, 9);
		PROG_JUMP(p, 0xd);
	case 9:
		if (!p_question(p, 10))
			return 0;
		if (!p->answer)
			shelf_take_all();
		PROG_JUMP(p, 0xd);
	case 0x20: {
		int items[SHELF_N], n = 0;
		for (int i = 0; i < SHELF_N; i++)
			if (ev_have_item(s_shelf[i][0]))
				items[n++] = s_shelf[i][0];
		if (n && (p->down & KEY_DUP)) p->work[1] = (p->work[1] + n - 1) % n;
		if (n && (p->down & KEY_DDOWN)) p->work[1] = (p->work[1] + 1) % n;
		if (p->work[1] >= n) p->work[1] = n ? n - 1 : 0;
		if (n && (p->down & KEY_A)) {
			shelf_put(items[p->work[1]]);
			p_se(0x2b21);
			return 0;
		}
		shelf_panel(p, items, n);
		if (!(p->down & KEY_B))
			return 0;
		p_panel(NULL);
	}
		// fallthrough
	case 0xd:
	default:
		p_clear(416);
		p_lock(p, false);
		return 1;
	}
}

// 0x57ac90 EvProgUseFishKey: the "Fish" key opens the bag, the key to 204 is inside.
static int s41_use_fish_key(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
		if (p_flag(2189))
			p_set(2190);                // FLAGW_2176 |= 0x4000
		p_picture_show("htl_p_bag_close");
	PROG_STEP(p, 9, p_item_use(p, 0x2f, 0x10, 0x44c0))
		p_lock(p, true);
		p_se(0x44c1);
		p_picture_show("htl_p_bag_open");
	PROG_STEP(p, 0x12, p_item_get_quiet(p, 0x2c, 0xc))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

// 0x579ab0 EvProgFishCaseClose / 0x579ae0 EvProgFishCaseOpen
static int s41_fish_case_close(Prog* p)
{
	if (p->step == 0 && p->sub == 0)
		p_set(2189);                    // FLAGW_2176 |= 0x2000
	return p_picture_msg(p, "htl_p_bag_close", 2);
}

static int s41_fish_case_open(Prog* p) { return p_picture_msg(p, "htl_p_bag_open", 3); }

// 0x57afc0 EvProgTrunkUnlockChallenge (drums 0x579db0): turn the four drums to the word written
// under the marker in the photo; 413 opens the trunk (the "Cinderella" box is inside).
static int s41_trunk(Prog* p)
{
	switch (p->step) {
	case 0:
		p_set(2187);                    // FLAGW_2176 |= 0x800
		p_lock(p, true);
		trunk_init();
		p->work[1] = 0;
		p_picture_show("htl_p_trank");
		p->step = 8;
		// fallthrough
	case 8: {
		u8* drum = p_puzzle() + 0x1c;
		int c = p->work[1];
		if (p->down & KEY_DLEFT) p->work[1] = (c + 3) & 3;
		if (p->down & KEY_DRIGHT) p->work[1] = (c + 1) & 3;
		c = p->work[1];
		if (!p_flag(413) && (p->down & (KEY_DUP | KEY_A))) { drum[c] = (drum[c] + 1) & 7; p_se(0x4a45); }
		if (!p_flag(413) && (p->down & KEY_DDOWN)) { drum[c] = (drum[c] - 1) & 7; p_se(0x4a45); }
		const char* w = s_trunk_words + trunk_word() * 4;
		int ok = 0;
		for (int d = 0; d < 4; d++)
			ok += s_drum_letters[d][drum[d]] == w[d];
		if (ok == 4 && !p_flag(413)) {
			p_set(413);                 // FLAGW_384 |= 0x20000000
			p_se(0x44c3);
		}
		char buf[200];
		int k = snprintf(buf, sizeof buf, "\n     ");
		for (int d = 0; d < 4; d++)
			k += snprintf(buf + k, sizeof buf - k, "%c%c%c ", c == d ? '[' : ' ', s_drum_letters[d][drum[d]] - 32,
			              c == d ? ']' : ' ');
		snprintf(buf + k, sizeof buf - k, "\n\n\n\n\n\n   Left/Right: drum  Up/Down: turn\n   B: leave");
		p_panel(buf);
		if (!p_flag(413) && !(p->down & KEY_B))
			return 0;
	}
		// fallthrough
	default:
		p_panel(NULL);
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x57b1f0 EvProgPictureWithMarker: the photos on the bed; the thinner (411 from the use event)
// takes the marker off and shows the trunk's word.
static int s41_picture_with_marker(Prog* p)
{
	switch (p->step) {
	case 0:
		p_set(2187);
		p_lock(p, true);
		if (!p_flag(411) && !p_flag(412))
			PROG_JUMP(p, 10);
		PROG_JUMP(p, 2);
	case 10:
		if (!p_message(p, 4))
			return 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("htl_p_marker");
		if (p_flag(411) && !p_flag(412)) {
			if (!p_item_use(p, 0x42, 0x11, 0x44c2))
				return 0;
			p_lock(p, true);
		}
		p->step = 0x10, p->sub = 0;
		// fallthrough
	case 0x10:
		if (p_flag(411) || p_flag(412)) {
			char buf[80];
			snprintf(buf, sizeof buf, "\n   Under the marker:  %.4s", s_trunk_words + trunk_word() * 4);
			p_panel(buf);
		}
		if (!(p->down & (KEY_A | KEY_B)))
			return 0;
		p_panel(NULL);
		if (p_flag(411) || p_flag(412))
			PROG_JUMP(p, 0xd);
		p->step = 0x1a, p->sub = 0;
		// fallthrough
	case 0x1a:
		if (!p_message(p, 5))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x57a310 EvProgGetElvHallKey / 0x57a320 EvProgGetCinderella
static int s41_get_elevator_key(Prog* p) { return p_item_get(p, 0x2d, 0xd); }

static int s41_get_cinderella(Prog* p)
{
	if (p->sub == 0) {
		p_set(2187);                    // FLAGW_2176 |= 0x1800
		p_set(2188);
	}
	return p_item_get(p, 0x44, 0xb);
}

static const ProgFn s_stage41[14] = {
	NULL, s41_put_for_shelf, s41_use_204_key, s41_use_elevator_key, s41_elevator, s41_get_baggage,
	s41_put_baggage, s41_use_fish_key, s41_fish_case_close, s41_fish_case_open, s41_trunk,
	s41_picture_with_marker, s41_get_elevator_key, s41_get_cinderella,
};

// ================================================================ stage 42 Stg_hotel_3f_f (0x8cbdd0)

// 0x577e80 EvProgUseHotelStairKey / 0x577f80 EvProgUseHotel312Key
static int s42_use_stair_key(Prog* p)
{
	if (p->sub == 0)
		p_set(550);                     // FLAGW_544 |= 0x40
	if (!p_item_use(p, 0x30, 3, 0x4a51))
		return 0;
	p_se(0x4980);
	return 1;
}

static int s42_use_312_key(Prog* p) { return p_item_use(p, 0x2b, 4, 0x4a3b); }

// 0x577fa0 EvProgVideoReplay: the videotape in room 312 (movie murder.sfd).
static int s42_video_replay(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x17, p_demo(p, DEMO_VIDEO))
		p_set(475);                     // FLAGW_448 |= 0x8000000
		p_set(625);                     // FLAGW_608 |= 0x20000
		p_set(527);                     // FLAGW_512 |= 0x8000
		p_set(627);                     // FLAGW_608 |= 0x80000
		p_place(59947.35f, 0.0f, 19132.5f, 3.1415927f);
		p_lock(p, false);
	PROG_END(p)
}

// 0x578440 EvProgMemoryOfMurder slot: returns 1.
static int s42_nothing(Prog* p) { (void)p; return 1; }

// 0x578e00 (DC): the blue gem in 312, the UFO ending (FUN_0048d7e0(0x20)).
static int s42_ufo_ending(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 1, p_demo(p, DEMO_UFO_HOTEL))
		p_set(727);                     // FLAGW_704 |= 0x800000
		p_lock(p, false);
		p_game_end(0x20);
	PROG_END(p)
}

static const ProgFn s_stage42[6] = { NULL, s42_use_stair_key, s42_use_312_key, s42_video_replay, s42_nothing,
                                     s42_ufo_ending };

// ================================================================ stage 43 Stg_hotel_bf_b (0x8cb840)

// 0x5777f0 EvProgOpenElevatorWater: the flooded elevator (buttons 0x577540). The fourth button
// starts the car with James inside and the water drains (478, then 501, 502, 2219, and 479 once the
// level is 0); the fifth sets 22 and the stage's door event takes him up.
static int s43_elevator_water(Prog* p)
{
	switch (p->step) {
	case 0:
		p_lock(p, true);
		p->work[1] = 0;
		p_picture_show("htl_p_htl_elevator_2");
		p->step = 7;
		// fallthrough
	case 7: {
		button_cursor(p, 6);
		if (p->down & KEY_B) {
			p_panel(NULL);
			PROG_JUMP(p, 0xd);
		}
		if (!(p->down & KEY_A))
			return 0;
		p_se(0x4a49);
		if (p->work[1] == 4) {
			p_set(22);                  // gGameFlag |= 0x400000
			p_se(0x4a42);
			p_panel(NULL);
			PROG_JUMP(p, 0xd);
		}
		if (p->work[1] != 3)
			return 0;
		p_panel(NULL);
		p_picture_hide();
		p_se(0x4a43);
		float pos[3], yaw;              // the PC sets x, z and yaw only (jms + 0x1c, 0x24, 0x30)
		p_player(pos, &yaw);
		p_place(-15046.85f, pos[1], -56599.02f, -1.525869f);
		p_set(478);                     // FLAGW_448 |= 0x40000000
		p->timer = 0.0f;
		p->step = 0x10, p->sub = 0;
	}
		// fallthrough
	case 0x10:
		if (!p_wait(p, 6.0f + 4.5f))    // _DAT_0069f324, _DAT_007117a4
			return 0;
		p_se(0x4a44);
		p_set(501);                     // FLAGW_480 |= 0x600000
		p_set(502);
		p_set(2219);                    // FLAGW_2208 |= 0x800
		p_set(479);                     // FLAGW_448 |= 0x80000000: the water is gone
		// fallthrough
	case 0xd:
	default:
		p_picture_hide();
		p_lock(p, false);
		return 1;
	}
}

// 0x5775f0: returns 1.
static int s43_nothing(Prog* p) { (void)p; return 1; }

static const ProgFn s_stage43[3] = { NULL, s43_elevator_water, s43_nothing };

// ================================================================ stage 44 Stg_hotel_1f_b (0x8cad68)

// 0x576050 EvProgTriheadBattle: the two Pyramid Heads (the letter from Mary is gone after it).
static int s44_trihead_battle(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_TRIHEAD_BATTLE))
		p_place(-20022.0f, -25.0f, 21740.25f, 0.0f);
		p_item_lose(0x15);
		p_lock(p, false);
	PROG_END(p)
}

// 0x576200 EvProgTriheadDead
static int s44_trihead_dead(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_TRIHEAD_DEAD))
		p_lock(p, false);
	PROG_END(p)
}

// 0x576370 EvProgGetEggRustColored / 0x576440 EvProgGetEggScarlet: from the Pyramid Heads.
static int s44_get_egg(Prog* p, int item, int msg)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 10, p_message(p, 0x2b))
	PROG_STEP(p, 9, p_item_get_quiet(p, item, msg))
		p_lock(p, false);
	PROG_END(p)
}

static int s44_get_egg_rust(Prog* p) { return s44_get_egg(p, 0x48, 4); }
static int s44_get_egg_scarlet(Prog* p) { return s44_get_egg(p, 0x49, 5); }

// 0x576a90 EvProgEggHoleOnDoor: two doors with a round hollow each. 485 / 486 James looks at
// door 1 / 2; 487..490 an egg is being put in (rust or scarlet, door 1 then door 2); 491..494
// the egg sits there.
static int s44_egg_hole(Prog* p)
{
	bool look = p_flag(485) || p_flag(486);
	switch (p->step) {
	case 0:
		p_lock(p, true);
		if (look) {
			if (p_flag(485)) p_set(2243);   // FLAGW_2240 |= 8
			else p_set(2245);               // FLAGW_2240 |= 0x20
			p_se(0x4a3c);
			PROG_JUMP(p, 0x10);
		}
		PROG_JUMP(p, 2);
	case 0x10:
		if (!p_system_message(p, 4))
			return 0;
		p->step = 2, p->sub = 0;
		// fallthrough
	case 2:
		p_picture_show("htl_p_egghollow");
		PROG_JUMP(p, look ? 8 : 9);
	case 9: {
		bool rust = p_flag(487) || p_flag(489);
		if (!p_item_use(p, rust ? 0x48 : 0x49, rust ? 6 : 7, 0))
			return 0;
		p_lock(p, true);
		p_se(0x44c7);
		static const short move[4][2] = { { 487, 491 }, { 488, 492 }, { 489, 493 }, { 490, 494 } };
		for (int i = 0; i < 4; i++)
			if (p_flag(move[i][0])) {
				p_clear(move[i][0]);
				p_set(move[i][1]);
			}
		PROG_JUMP(p, 7);
	}
	case 7:                         // the egg sits in the door until a button
		if (!(p->down & (KEY_A | KEY_B)))
			return 0;
		PROG_JUMP(p, 0xd);
	case 8: {
		if (!(p->down & (KEY_A | KEY_B)))
			return 0;
		bool filled = (p_flag(485) && (p_flag(491) || p_flag(492))) || (p_flag(486) && (p_flag(493) || p_flag(494)));
		if (filled)
			PROG_JUMP(p, 0xd);
		p->step = 10, p->sub = 0;
	}
		// fallthrough
	case 10:
		if (!p_message(p, 1))
			return 0;
		// fallthrough
	case 0xd:
	default:
		p_picture_hide();
		p_clear(485);                   // FLAGW_480 &= ~0x60
		p_clear(486);
		p_lock(p, false);
		return 1;
	}
}

// 0x576660: the ending decision at the last door. Four scores: the Rebirth items (74..77 all held
// gives 621), and the rest weigh flags and the ending counters (prog_score_*) into 618, 619 or
// 620, which the stage's auto doors turn into stages 48 (Leave), 49 (Maria), 50 (In Water) and 51
// (Rebirth). The comparison order is the PC's; the endings seen (0x1dbeb84) come from the clear
// data, 0 on a first game.
static int s44_ending_decision(Prog* p)
{
	(void)p;
	u8 clear[3];
	prog_clear_get(clear);
	const u8 seen = clear[1];           // PC 0x1dbeb84: endings seen before (the clear data)
	const float* dmg = p_damage_stats();
	int books = ev_have_item(74) + ev_have_item(75) + ev_have_item(76) + ev_have_item(77);
	int c = p_flag(655) ? 4 : 5;        // uVar6
	int a = p_flag(656) ? 2 : 3;        // iVar2
	int b = p_flag(204) ? 3 : 2;        // uVar5
	if (p_flag(371)) a++;
	if (p_flag(500)) b++;
	if (p_flag(615)) b++;
	if (p_flag(630)) b++;
	if (p_flag(629)) a++;
	if (p_flag(624)) { c++; b++; }
	if (p_puzzle()[0x1a] > 1) a--;
	float low = *(float*)p_flag_bytes(0x1f7d9d8), heal = *(float*)p_flag_bytes(0x1f7d9d4);
	float near = *(float*)p_flag_bytes(0x1f7d9cc), far = *(float*)p_flag_bytes(0x1f7d9d0);
	if (low > 60.0f) b++;
	if (low > 240.0f) b++;
	if (heal > 2.0f) c++;
	if (near > 600.0f) a++;
	if (far > 300.0f) a--;
	bool s1 = seen & 1, s2 = seen & 2, s4 = seen & 4;
	if (s1) { if (!s2) a++; if (!s4) b++; }
	if (s2) { if (!s1) c++; if (!s4) b++; }
	if (s4) { if (!s1) c++; if (!s2) a++; }
	float limit = prog_action_level() < 2 ? 160.0f : 80.0f;   // _DAT_0069f368 / _DAT_0069f3b0
	if (dmg[0] > 0.0f) a--;
	if (dmg[0] > 0.5f * limit) a--;
	if (p_puzzle()[0x1b] > 30) a--;
	if (p_puzzle()[0x1b] < 3) a++;
	if (dmg[2] <= 1.0f || dmg[1] / dmg[2] < 0.03f) a++;
	if (books == 4) {
		p_set(621);                     // FLAGW_608 |= 0x2000
		return 1;
	}
	if (a < c && b < c) goto leave;
	if (!s1) {
		if (a <= c) {
			if (b <= c) goto leave;
			goto check_c;
		}
		goto check_b;
	}
check_c:
	if (c < a) goto check_b;
	goto rest;
check_b:
	if (b < a) goto maria;
rest:
	if (s2 || a < c || a < b) {
		if ((b <= c || b <= a) && s4 && (b < c || b < a)) {
			if (a <= c && b <= c) goto leave;
			if (b <= a) goto maria;
		}
		p_set(620);                     // FLAGW_608 |= 0x1000
		return 1;
	}
maria:
	p_set(619);                         // FLAGW_608 |= 0x800
	return 1;
leave:
	p_set(618);                         // FLAGW_608 |= 0x400
	return 1;
}

static const ProgFn s_stage44[7] = { NULL, s44_trihead_battle, s44_trihead_dead, s44_get_egg_rust,
                                     s44_get_egg_scarlet, s44_egg_hole, s44_ending_decision };

// ================================================================ stage 45 Stg_hotel_2f_b (0x8ca5d0)

// 0x575b80 EvProgNotifyDeath
static int s45_notify_death(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_NOTIFY_DEATH))
		p_lock(p, false);
	PROG_END(p)
}

// 0x575c70 EvProgGetCrimsonCeremony (New Game+)
static int s45_crimson_ceremony(Prog* p)
{
	PROG_BEGIN(p)
		p_lock(p, true);
	PROG_STEP(p, 0x1e, p_item_get_quiet(p, 0x4b, 2))
	PROG_STEP(p, 2, p_picture(p, "htl_p_redreling"))
	PROG_STEP(p, 10, p_message(p, 1))
		p_picture_hide();
		p_lock(p, false);
	PROG_END(p)
}

static const ProgFn s_stage45[3] = { NULL, s45_notify_death, s45_crimson_ceremony };

// ================================================================ stage 46 Stg_hotel_3f_b (0x8c9e88)

// 0x575ae0 EvProgUseDogKey (disassembly: EvSubItemUse0(0x19, 3, 0x4270, 0, pos, 1)); flag 516
// sends James to the dog ending.
static int s46_use_dog_key(Prog* p) { return p_item_use(p, 0x19, 3, 0x4270); }

static const ProgFn s_stage46[2] = { NULL, s46_use_dog_key };

// ================================================================ stage 47 Stg_hotel_fire (0x8c99e0)

// 0x575630 EvProgAngelaInFire / 0x575750 EvProgFireDamage (5% of James' health)
static int s47_angela_in_fire(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_ANGELA_FIRE))
		p_lock(p, false);
	PROG_END(p)
}

static int s47_fire_damage(Prog* p)
{
	(void)p;
	p_damage(0.05f);                    // FUN_00544f80(0x3e, max * _DAT_0069f3b4)
	return 1;
}

static const ProgFn s_stage47[3] = { NULL, s47_angela_in_fire, s47_fire_damage };

// ================================================================ stages 48..51: the endings

// EvProgMaryBeforeBattle (0x43fe30, 0x440c80, 0x43eac0, 0x574860): the last corridor, flag 531.
static int end_before_battle(Prog* p)
{
	PROG_BEGIN(p)
		p_set(531);                     // FLAGW_512 |= 0x80000
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARY_BEFORE_BATTLE))
		p_lock(p, false);
	PROG_END(p)
}

// end_maria's version also sets flag 33 when it ends (0x440c80).
static int end_before_battle_maria(Prog* p)
{
	if (!end_before_battle(p))
		return 0;
	p_set(33);                          // FLAGW_32 |= 2
	return 1;
}

// EvProgMaryFall (0x4400c0, 0x440f40, 0x43ed60, 0x574b00)
static int end_mary_fall(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_MARY_FALL))
		p_place(57945.0f, -16005.0f, 57523.2f, -0.566514f);
		p_lock(p, false);
	PROG_END(p)
}

// EvProgChangeMaryRoom (0x440210, 0x43eeb0): the end stage swaps the room (0x4ed6f0).
static int end_change_room(Prog* p) { (void)p; return 1; }

// EvProgMaryAfterBattle, Leave (0x440230): the scene with Mary; the ending runs on in the forest.
static int s48_after_battle(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_END_LEAVE_ROOM))
		p_lock(p, false);
	PROG_END(p)
}

// EvProgMaryAfterBattle, In Water (0x43eee0): movies water.sfd, end_scde.sfd, the credits
// (FUN_0048d7e0(4)).
static int s50_after_battle(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_END_WATER))
		p_lock(p, false);
		p_game_end(4);
	PROG_END(p)
}

static const ProgFn s_stage48[5] = { NULL, end_before_battle, end_mary_fall, end_change_room, s48_after_battle };
static const ProgFn s_stage49[3] = { NULL, end_before_battle_maria, end_mary_fall };
static const ProgFn s_stage50[5] = { NULL, end_before_battle, end_mary_fall, end_change_room, s50_after_battle };
static const ProgFn s_stage51[3] = { NULL, end_before_battle, end_mary_fall };

// ================================================================ stage 52 Stg_end_dog (0x8c8c0c)

// 0x574600 EvProgEndingDog (disassembly): movie end_dog.sfd and the credits, flag 622,
// FUN_0048d7e0(0x10).
static int s52_ending_dog(Prog* p)
{
	PROG_BEGIN(p)
	PROG_STEP(p, 0x16, p_demo(p, DEMO_END_DOG))
		p_set(622);                     // FLAGW_608 |= 0x4000
		p_lock(p, false);
		p_game_end(0x10);
	PROG_END(p)
}

static const ProgFn s_stage52[2] = { NULL, s52_ending_dog };

const ProgStage prog_hotel_stages[] = {
	{ 6, s_stage38 }, { 7, s_stage39 }, { 13, s_stage40 }, { 14, s_stage41 }, { 6, s_stage42 },
	{ 3, s_stage43 }, { 7, s_stage44 }, { 3, s_stage45 }, { 2, s_stage46 }, { 3, s_stage47 },
	{ 5, s_stage48 }, { 3, s_stage49 }, { 5, s_stage50 }, { 3, s_stage51 }, { 2, s_stage52 },
};
