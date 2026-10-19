#include "prog_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The sprites the PC draws over a puzzle's still (the second file of EvSubPictureDisplay, a sprite
// sheet). Positions are the PC's sprite units: x and y -256..256 across the still, centred, y down
// (the PC keeps them in 1/16 steps); texture coordinates are the sheet's texels. The tables come
// from the exe through tools/sh2conv/pics.py (pack/pic/overlay.dat), the sheets from the same
// converter (pack/sheet/<dir>/<name>.t3x). Each overlay runs every frame while its still is up.

#define PACK_DIR "sdmc:/3ds/sh23ds"

static ProgHost s_ov_host;
static int s_overlay;            // OV_* of the still on screen
static u8* s_blob;               // overlay.dat, loaded the first time a still has an overlay
static bool s_blob_tried;

enum { OV_NONE, OV_CLOCK_CLOSED, OV_CLOCK_OPEN, OV_SAFE, OV_COINS, OV_BOX, OV_BOX_WALL, OV_QUIZ, OV_DRUMS,
       OV_EGG, OV_TOMBSTONE, OV_VIDEO, OV_MARKER, OV_ELEVATOR, OV_ELEVATOR_2, OV_MANACLES, OV_MEMO_SAFE,
       OV_MEMO_HAIR, OV_MEMO_RINGS, OV_MEMO_TABLETS, OV_INCAR };

static const struct { const char* pic; const char* sheet; int ov; } s_stills[] = {
	{ "apt_clock_close", "apt/clock_hari", OV_CLOCK_CLOSED },       // 0x59a390
	{ "apt_clock_open", "apt/clock_hari", OV_CLOCK_OPEN },
	{ "apt_p_safe_close", "apt/p_safe_close2", OV_SAFE },           // 0x596f20
	{ "apt_p_desk_coin", "apt/p_desk_coin_coin", OV_COINS },        // 0x597ce0
	{ "hsp_p_box", "hsp/pboxkey01", OV_BOX },                       // 0x58e1f0
	{ "hsp_p_boxnumber", "hsp/p_boxnumber_2", OV_BOX_WALL },        // 0x58db10
	{ "hsp_p_quiz", "hsp/p_quiz_btn", OV_QUIZ },                    // 0x589820
	{ "htl_p_trank", "htl/dram", OV_DRUMS },                        // 0x579af0
	{ "htl_p_egghollow", "htl/egg", OV_EGG },                       // 0x576510
	{ "add_p_teien", "add/teien2", OV_TOMBSTONE },                  // 0x571f50
	{ "htl_p_video", "htl/p_video02", OV_VIDEO },                   // 0x57bce0
	{ "htl_p_marker", "htl/maker", OV_MARKER },                     // 0x579f60
	{ "hsp_p_h_elevator", "hsp/p_h_elevator_botan", OV_ELEVATOR },  // 0x58cad0
	{ "hsp_p_h_elevator_2", "hsp/p_h_elevator_botan", OV_ELEVATOR_2 }, // 0x5888a0
	{ "dls_p_manacles_on", "dls/p_manacles_off", OV_MANACLES },     // 0x57fd50
	{ "out_p_incar", "out/p_incar_map", OV_INCAR },                 // 0x5a0b80
};

void prog_overlay_init(const ProgHost* host) { s_ov_host = *host; }

// pack/pic/overlay.dat: "SHOV", u32 count, then per table char name[16], u32 size, the bytes.
static const u8* table(const char* name, u32 need)
{
	if (!s_blob && !s_blob_tried) {
		s_blob_tried = true;
		FILE* f = fopen(PACK_DIR "/pic/overlay.dat", "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long n = ftell(f);
			fseek(f, 0, SEEK_SET);
			s_blob = malloc(n + 1);
			if (s_blob && (fread(s_blob, 1, n, f) != (size_t)n || n < 8 || memcmp(s_blob, "SHOV", 4))) {
				free(s_blob);
				s_blob = NULL;
			}
			fclose(f);
		}
	}
	if (!s_blob)
		return NULL;
	u32 count;
	memcpy(&count, s_blob + 4, 4);
	const u8* p = s_blob + 8;
	for (u32 i = 0; i < count; i++) {
		u32 size;
		memcpy(&size, p + 16, 4);
		if (!strncmp((const char*)p, name, 16))
			return size >= need ? p + 20 : NULL;
		p += 20 + size;
	}
	return NULL;
}

static s16 S16(const u8* t, int i) { s16 v; memcpy(&v, t + 2 * i, 2); return v; }
static u16 U16(const u8* t, int i) { u16 v; memcpy(&v, t + 2 * i, 2); return v; }

// One sprite: corners top left, top right, bottom left, bottom right in sprite units.
static void quad(const float x[4], const float y[4], float u0, float v0, float u1, float v1, float alpha)
{
	if (!s_ov_host.sprite)
		return;
	const float xy[4][2] = { { x[0], y[0] }, { x[1], y[1] }, { x[2], y[2] }, { x[3], y[3] } };
	const float uv[4] = { u0, v0, u1, v1 };
	s_ov_host.sprite(xy, uv, alpha);
}

static void rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float alpha)
{
	const float x[4] = { x0, x1, x0, x1 }, y[4] = { y0, y0, y1, y1 };
	quad(x, y, u0, v0, u1, v1, alpha);
}

// A rectangle (lx0..lx1, ly0..ly1) turned by a about (cx, cy); the PC squeezes x by 0.8 for its
// 4:3 screen (0x59a47f, 0x596fa6).
static void turned(float cx, float cy, float a, float lx0, float ly0, float lx1, float ly1, float u0, float v0,
                   float u1, float v1)
{
	float s = sinf(a), c = cosf(a), x[4], y[4];
	const float lx[4] = { lx0, lx1, lx0, lx1 }, ly[4] = { ly0, ly0, ly1, ly1 };
	for (int i = 0; i < 4; i++) {
		x[i] = 0.8f * (lx[i] * c - ly[i] * s) + cx;
		y[i] = lx[i] * s + ly[i] * c + cy;
	}
	quad(x, y, u0, v0, u1, v1, 1.0f);
}

// 0x59a390: the clock's hands about (0, -24). The minute hand turns once in 0xf00 (60 minutes in
// 1/64 steps), the hour hand once in 0xb400; the second hand stands at 15 seconds.
static void ov_clock(bool open)
{
	const u8* t = table("clock_uv", 64);
	if (!t)
		return;
	u16 clk = *(u16*)(p_puzzle() + PZ_CLOCK);
	static const int order[4] = { 2, 1, 0, 3 };     // priorities 3, 4, 5, 6
	for (int k = 0; k < 4; k++) {
		int i = order[k];
		float a = 0.0f, len = 16.0f, tail = 16.0f;
		if (i == 0) a = (float)M_PI / 2.0f, len = 144.0f, tail = 48.0f;
		if (i == 1) a = (float)(clk % 0xf00) * 0.0016362462f, len = 144.0f;
		if (i == 2) a = (float)clk * 0.00013635385f, len = 112.0f;
		int e = (i + (open ? 4 : 0)) * 4;
		turned(0.0f, -24.0f, a, -16.0f, -len, 16.0f, tail, U16(t, e) / 16.0f, U16(t, e + 1) / 16.0f,
		       U16(t, e + 2) / 16.0f, U16(t, e + 3) / 16.0f);
	}
}

// 0x596f20: the safe's dial, turned a tenth of a half turn per number (flags 103..107).
static void ov_safe(void)
{
	int d = 0;
	for (int b = 0; b < 5; b++)
		d |= p_flag(103 + b) << b;
	turned(-14.5f, -4.75f, -(float)(d + 1) * 0.31415927f, -80.0f, -64.0f, 80.0f, 64.0f, 0.0f, 0.0f, 127.0f, 127.0f);
}

// 0x597ce0: the coins in the desk's five holes (flag 126 + 5 * coin + hole).
static void ov_coins(void)
{
	const u8* t = table("coin", 68);
	if (!t)
		return;
	for (int hole = 0; hole < 5; hole++)
		for (int c = 0; c < 3; c++) {
			if (!p_flag(126 + 5 * c + hole))
				continue;
			// Enhanced Edition PuzzleAlignmentFixes (CoinPuzzleFix, zlib licence): each coin's whole
			// 64 texel cell, and one rectangle per hole that fills the slot, whichever coin is in it
			// (1/16 units; the PC's own are the table's: a 48 x 64 texel cell from each coin's corner)
			static const s16 EE_UV[3][4] = { { 0, 1055, 1024, 2075 }, { 0, 0, 1024, 1024 }, { -4, 2082, 1028, 3108 } };
			static const s16 EE_XY[5][4] = { { -3160, -708, -2090, 362 }, { -1790, -682, -720, 388 },
			                                 { -414, -650, 656, 420 }, { 962, -612, 2032, 458 },
			                                 { 2348, -580, 3418, 490 } };
			rect(EE_XY[hole][0] / 16.0f, EE_XY[hole][1] / 16.0f, EE_XY[hole][2] / 16.0f, EE_XY[hole][3] / 16.0f,
			     EE_UV[c][0] / 16.0f, EE_UV[c][1] / 16.0f, EE_UV[c][2] / 16.0f, EE_UV[c][3] / 16.0f, 1.0f);
			break;
		}
}

// 0x58e1f0: the box's locks. The PC fades each part out over a second once it opens; here a part
// shows while its lock is shut: the key locks (195), the wheel panel with its four symbols (197)
// and the button panel (198).
static void ov_box(void)
{
	const u8* t = table("box", 326);
	if (!t)
		return;
	if (!p_flag(195)) {
		const u8* r = t + 184;           // 0x8db730: u0 v0 u1 v1
		const u8* pos = t + 264;         // 0x8db780: x y (and a spare short)
		for (int pass = 0; pass < 2; pass++)
			for (int i = pass ? 3 : 0; i < (pass ? 10 : 3); i++) {
				float x = S16(pos, 3 * i) / 16.0f, y = S16(pos, 3 * i + 1) / 16.0f;
				float u0 = S16(r, 4 * i), v0 = S16(r, 4 * i + 1), u1 = S16(r, 4 * i + 2), v1 = S16(r, 4 * i + 3);
				rect(x, y, x + u1 - u0, y + v1 - v0, u0, v0, u1 - 1, v1 - 1, 1.0f);
			}
	}
	if (!p_flag(197)) {
		rect(-163.625f, -6.6875f, -83.625f, 153.3125f, 0, 0, 79, 159, 1.0f);
		const u8* w = p_puzzle() + PZ_WHEEL;
		for (int i = 0; i < 4; i++) {
			int s = w[i] % 9;
			const u8* r = t + 4 * s;     // 0x8db678
			float x = S16(t + 40, 2 * (i + 4 * s)) / 16.0f, y = S16(t + 40, 2 * (i + 4 * s) + 1) / 16.0f;
			rect(x, y, x + r[2] - r[0], y + r[3] - r[1], r[0], r[1], r[2] - 1, r[3] - 1, 1.0f);
		}
	}
	if (!p_flag(198))
		rect(24.1875f, 38.75f, 152.1875f, 198.75f, 224, 0, 351, 159, 1.0f);
}

// 0x58db10: the symbols on the wall that the box's wheels have to match.
static void ov_box_wall(void)
{
	const u8* t = table("wall", 432);
	if (!t)
		return;
	const u8* g = p_puzzle() + PZ_WHEEL_GOAL;
	for (int i = 0; i < 4; i++) {
		int e = i + 4 * (g[i] % 9);
		float x = S16(t + 288, 2 * e) / 16.0f, y = S16(t + 288, 2 * e + 1) / 16.0f;
		float u0 = S16(t, 4 * e), v0 = S16(t, 4 * e + 1), u1 = S16(t, 4 * e + 2), v1 = S16(t, 4 * e + 3);
		rect(x, y, x + u1 - u0, y + v1 - v0, u0, v0, u1 - 1, v1 - 1, 1.0f);
	}
}

// 0x589820: the pressed buttons, one per row (flags 234 + 2 * row: 1 0 first, 0 1 second, 1 1 third).
static void ov_quiz(void)
{
	static const s16 pos[9][2] = {
		{ -262, -2033 }, { 39, -1936 }, { 333, -1839 }, { -422, -1774 }, { -109, -1682 }, { 182, -1569 },
		{ -590, -1469 }, { -298, -1381 }, { 32, -1252 },
	};
	for (int r = 0; r < 3; r++) {
		bool a = p_flag(234 + 2 * r), b = p_flag(235 + 2 * r);
		if (!a && !b)
			continue;
		int c = a && b ? 2 : b ? 1 : 0, i = 3 * r + c;
		float x = pos[i][0] / 16.0f, y = pos[i][1] / 16.0f;
		rect(x, y, x + 30.0f, y + 30.0f, 30.0f * c, 30.0f * r, 30.0f * c + 30.0f, 30.0f * r + 30.0f, 1.0f);
	}
}

// 0x579af0: the trunk's four drums, each with the letter above, the one in the window and the one
// below (puzzle bytes 0x1c..0x1f, the drums' letters in hotel order).
static void ov_drums(void)
{
	static const char letters[4][9] = { "dhklmnot", "aeiopuvx", "celmrsvw", "aeklnprt" };
	const u8* abc = table("drum_abc", 19);
	const u8* t = table("drum_pos", 912);
	if (!abc || !t)
		return;
	const u8* drum = p_puzzle() + 0x1c;
	for (int d = 0; d < 4; d++)
		for (int k = -1; k <= 1; k++) {
			char ch = letters[d][(drum[d] + k + 8) & 7];
			int idx = 0;
			while (idx < 19 && abc[idx] != (u8)ch)
				idx++;
			if (idx == 19)
				continue;
			int e = (d + idx * 4) * 6 + (k + 1) * 2;     // 12 bytes per letter and drum
			float x = S16(t, e) / 16.0f, y = S16(t, e + 1) / 16.0f;
			float u = (idx & 7) * 32.0f, v = (idx >> 3) * 64.0f;
			float v0 = k < 0 ? 0.0f : k == 0 ? 16.0f : 48.0f, h = k == 0 ? 32.0f : 16.0f;
			rect(x, y, x + 32.0f, y + h, u, v + v0, u + 31.0f, v + v0 + h - 1.0f, 1.0f);
		}
}

// 0x576510: an egg in the hollow (flags 491..494 for the two doors, or the one being put in).
static void ov_egg(int putting)
{
	bool rust = (p_flag(485) && p_flag(491)) || (p_flag(486) && p_flag(493)) || putting == 1;
	bool scarlet = (p_flag(485) && p_flag(492)) || (p_flag(486) && p_flag(494)) || putting == 2;
	if (rust)
		rect(-18.6875f, -9.75f, 11.3125f, 50.25f, 30, 0, 60, 60, 1.0f);
	else if (scarlet)
		rect(-16.0625f, -10.0f, 13.9375f, 50.0f, 0, 0, 30, 60, 1.0f);
}

// 0x571f50: the boards in the tombstone. The first lies as it is, the others turned as they went in
// (flags 682 + 9b + r); the Acacia key shows until it is taken (704).
static void ov_tombstone(void)
{
	const u8* t = table("teien", 48);
	if (!t)
		return;
	if (!p_flag(704)) {
		float x0 = S16(t, 20) / 16.0f, y0 = S16(t, 21) / 16.0f, x1 = S16(t, 22) / 16.0f, y1 = S16(t, 23) / 16.0f;
		rect(x0, y0, x1, y1, U16(t, 12) / 16.0f, U16(t, 13) / 16.0f, U16(t, 14) / 16.0f, U16(t, 15) / 16.0f, 1.0f);
	}
	float X0 = S16(t, 16) / 16.0f, Y0 = S16(t, 17) / 16.0f, X1 = S16(t, 18) / 16.0f, Y1 = S16(t, 19) / 16.0f;
	for (int slot = 0; slot < 3; slot++) {
		int b = -1;
		for (int k = 0; k < 3; k++)
			if (p_flag(679 + 9 * k + slot))
				b = k;
		if (b < 0)
			continue;
		int r = 0;
		for (int k = 0; k < 4; k++)
			if (p_flag(682 + 9 * b + k))
				r = k;
		if (slot == 0)
			r = 0;
		// corners top left, top right, bottom left, bottom right of the board's picture
		static const int cx[4][4] = { { 0, 1, 0, 1 }, { 1, 1, 0, 0 }, { 1, 0, 1, 0 }, { 0, 0, 1, 1 } };
		static const int cy[4][4] = { { 0, 0, 1, 1 }, { 0, 1, 0, 1 }, { 1, 1, 0, 0 }, { 1, 0, 1, 0 } };
		float x[4], y[4];
		for (int i = 0; i < 4; i++)
			x[i] = cx[r][i] ? X1 : X0, y[i] = cy[r][i] ? Y1 : Y0;
		// Enhanced Edition PuzzleAlignmentFixes (Patches/PuzzleAlignmentFixes.cpp, GravestoneBoardsFix,
		// zlib licence): the three boards' texture rectangles matched to each other, and the turned
		// ones' corners moved to fill the stone's cavity (1/16 units as in the tables)
		static const s16 EE_UV[3][4] = { { 2581, -5, 5126, 3320 }, { 3, 1, 2548, 3320 }, { 3, 3316, 2542, 6636 } };
		static const s16 EE_XY[3][4][2] = {
			{ { 1355, -2030 }, { 1355, 1158 }, { -1313, -2030 }, { -1313, 1158 } },   // turned right
			{ { 1287, 1262 }, { -1267, 1262 }, { 1287, -2066 }, { -1267, -2066 } },   // upside down
			{ { -1353, 1176 }, { -1353, -2005 }, { 1315, 1176 }, { 1315, -2005 } },   // turned left
		};
		if (r > 0)
			for (int i = 0; i < 4; i++)
				x[i] = EE_XY[r - 1][i][0] / 16.0f, y[i] = EE_XY[r - 1][i][1] / 16.0f;
		quad(x, y, EE_UV[b][0] / 16.0f, EE_UV[b][1] / 16.0f, EE_UV[b][2] / 16.0f, EE_UV[b][3] / 16.0f, 1.0f);
	}
}

// 0x57bce0: the videotape and the can opener in the office safe until they are taken (the PC fades
// each out as it is picked up; flag 391 once the program has run).
static void ov_video(void)
{
	const u8* t = table("video", 16);
	if (!t)
		return;
	s32 p[4];
	memcpy(p, t, 16);
	if (!p_flag(391) && !ev_have_item(0x17))
		rect((s16)p[0] / 16.0f, (s16)p[1] / 16.0f, (s16)p[0] / 16.0f + 144.0f, (s16)p[1] / 16.0f + 112.0f, 0, 0, 143, 111,
		     1.0f);
	if (!p_flag(391) && !ev_have_item(0x46))
		rect((s16)p[2] / 16.0f, (s16)p[3] / 16.0f, (s16)p[2] / 16.0f + 80.0f, (s16)p[3] / 16.0f + 112.0f, 144, 0, 223,
		     111, 1.0f);
}

// 0x579f60: the photos on the bed. Under the marker is the trunk's word (four 48 texel letters from
// the same alphabet as the drums); the frame pieces, then the marker over the word until the thinner
// takes it off (411 / 412).
static void ov_marker(void)
{
	static const char words[] = "opendamnhelltowndarkmamadownlovelockmistlucklosedoseoverdusttimehelpkillnull";
	const u8* t = table("marker", 0x1d2);
	const u8* abc = table("drum_abc", 19);
	if (!t || !abc)
		return;
	int w = 0;
	for (int b = 0; b < 5; b++)
		w |= p_flag(406 + b) << b;
	w = w < 19 ? w : 0;
	for (int i = 0; i < 4; i++) {
		int idx = 0;
		while (idx < 19 && abc[idx] != (u8)words[w * 4 + i])
			idx++;
		if (idx == 19)
			continue;
		float x = S16(t, 2 * (i + idx * 4)) / 16.0f, y = S16(t, 2 * (i + idx * 4) + 1) / 16.0f;
		float u = S16(t + 0x130, 2 * idx), v = S16(t + 0x130, 2 * idx + 1);
		rect(x, y, x + 48.0f, y + 48.0f, u, v, u + 47.0f, v + 47.0f, 1.0f);
	}
	const bool gone = p_flag(411) || p_flag(412);
	for (int g = 0; g < 2; g++) {
		if (g == 1 && gone)
			break;
		float bx = S16(t, (g ? 0x180 : 0x17c) / 2) / 16.0f, by = S16(t, (g ? 0x180 : 0x17c) / 2 + 1) / 16.0f;
		int first = g ? 0x1b0 : 0x188, n = g ? 4 : 5;
		for (int k = 0; k < n; k++) {
			const u8* r = t + first + 8 * k;
			float u0 = S16(r, 0), v0 = S16(r, 1), u1 = S16(r, 2), v1 = S16(r, 3);
			rect(bx + u0, by + v0, bx + u1 + 1.0f, by + v1 + 1.0f, u0, v0, u1, v1, 1.0f);
		}
	}
}

// 0x58cad0 (front) and 0x5888a0 (otherworld): the button that was pressed lights up over a second.
// Flags 17..19 say where the car is and 21..23 where it was sent; the buttons are 3F, 2F and 1F
// from the top and the fourth is the floor the car is on. Each table holds four texel rectangles
// (s32 x0, y0, x1, y1) and then the four sprites' corners (s32, in sixteenths).
static u64 s_lit_tick;

static void ov_elevator(bool other)
{
	const u8* t = table(other ? "elevator2" : "elevator", 0x60);
	int b = p_flag(23) ? (p_flag(19) ? 3 : 0) : p_flag(22) ? (p_flag(18) ? 3 : 1) : p_flag(21) ? (p_flag(17) ? 3 : 2) : -1;
	if (!t || b < 0) {
		s_lit_tick = 0;
		return;
	}
	u64 now = svcGetSystemTick();
	if (!s_lit_tick)
		s_lit_tick = now;
	float a = (float)(now - s_lit_tick) / (float)SYSCLOCK_ARM11;
	s32 r[4], pos[2];
	memcpy(r, t + 16 * b, sizeof r);
	memcpy(pos, t + 0x40 + 8 * b, sizeof pos);
	float x = pos[0] / 16.0f, y = pos[1] / 16.0f;
	rect(x, y, x + (r[2] - r[0]), y + (r[3] - r[1]), r[0], r[1], r[2] - 1, r[3] - 1, a < 1.0f ? a : 1.0f);
}

// 0x57fd50: once the key turns, the open manacles (the still's second file) fade in over the
// closed ones in about a second (flag 367 is set when they are open).
static float s_manacles;

void prog_overlay_manacles(float alpha) { s_manacles = alpha; }

static void ov_manacles(void)
{
	if (s_manacles > 0.0f)
		rect(-256.0f, -256.0f, 256.0f, 256.0f, 0, 0, 511, 511, s_manacles < 1.0f ? s_manacles : 1.0f);
}

// 0x5a0b80: the map of Silent Hill on the driver's seat, a 256 unit square at (-774, -506) / 16
// (0x8ea818), drawn at 0x80 until the map is taken and then faded out over a second. Off (0) for
// the empty seat 0x5a0e70 shows once the map is gone.
static float s_incar;

void prog_overlay_incar(float alpha) { s_incar = alpha; }

static void ov_incar(void)
{
	if (s_incar > 0.0f)
		rect(-48.375f, -31.625f, 207.625f, 224.375f, 0, 0, 255, 255, s_incar < 1.0f ? s_incar : 1.0f);
}

// The memo list's overlays (0x49a210, by memo number): seven memos have a second picture. Four
// are the stills of puzzles above and draw the same parts (the PC keeps its own copies of their
// tables): the safe's dial (memo 0x12, once flag 607 is set), the wall symbols (0x20), the trunk's
// word on the photo (0x35) and the tombstone's boards (0x36). The other three are their own.
static const struct { u8 memo; u8 ov; const char* sheet; } s_memo_ov[] = {
	{ 0x12, OV_MEMO_SAFE, "apt/p_safe_close2" }, { 0x20, OV_BOX_WALL, "hsp/p_boxnumber_2" },
	{ 0x21, OV_MEMO_HAIR, "hsp/p_hair_hair" }, { 0x23, OV_MEMO_RINGS, "hsp/p_female_ring" },
	{ 0x29, OV_MEMO_TABLETS, "dls/p_tablet_plate" }, { 0x35, OV_MARKER, "htl/maker" },
	{ 0x36, OV_TOMBSTONE, "add/teien2" },
};

// Memo 0x21: the hair in the drain picture once flag 171 is set (the whole 256x64 sheet).
static void ov_memo_hair(void)
{
	if (p_flag(171))
		rect(-71.0f, -52.0f, 185.0f, 12.0f, 0, 0, 255, 63, 1.0f);
}

// Memo 0x23: the woman's two rings, flags 227 and 228 (16 texels each).
static void ov_memo_rings(void)
{
	if (p_flag(227))
		rect(147.5625f, 99.3125f, 163.5625f, 115.3125f, 0, 0, 15, 15, 1.0f);
	if (p_flag(228))
		rect(71.125f, 0.375f, 87.125f, 16.375f, 16, 0, 31, 15, 1.0f);
}

// Memo 0x29: the three tablets that have been put in, flags 301..303 (100 texel squares).
static void ov_memo_tablets(void)
{
	if (p_flag(301))
		rect(-173.5f, -7.0f, -73.5f, 93.0f, 0, 0, 100, 100, 1.0f);
	if (p_flag(302))
		rect(-44.3125f, -7.0f, 55.6875f, 93.0f, 100, 0, 200, 100, 1.0f);
	if (p_flag(303))
		rect(78.875f, -3.125f, 178.875f, 96.875f, 0, 100, 100, 200, 1.0f);
}

static int s_memo = -1;

void prog_overlay_memo(int memo)
{
	if (memo != s_memo) {
		s_memo = memo;
		int ov = OV_NONE;
		const char* sheet = NULL;
		for (u32 i = 0; memo >= 0 && i < sizeof s_memo_ov / sizeof *s_memo_ov; i++)
			if (s_memo_ov[i].memo == memo)
				ov = s_memo_ov[i].ov, sheet = s_memo_ov[i].sheet;
		s_overlay = ov;
		if (s_ov_host.sprite)
			s_ov_host.sprite(NULL, NULL, 0.0f);
		if (s_ov_host.sheet && !(ov && s_ov_host.sheet(sheet)))
			s_overlay = OV_NONE;
		if (!ov && s_ov_host.sheet)
			s_ov_host.sheet(NULL);
	}
	if (memo >= 0)
		prog_overlay_frame(0);
}

// Called with every still the programs show (NULL when it goes away).
void prog_overlay_picture(const char* pic)
{
	int ov = OV_NONE;
	const char* sheet = NULL;
	for (u32 i = 0; pic && i < sizeof s_stills / sizeof *s_stills; i++)
		if (!strcmp(pic, s_stills[i].pic))
			ov = s_stills[i].ov, sheet = s_stills[i].sheet;
	if (ov == s_overlay)
		return;
	s_overlay = ov;
	if (s_ov_host.sprite)
		s_ov_host.sprite(NULL, NULL, 0.0f);
	if (s_ov_host.sheet && !(ov && s_ov_host.sheet(sheet)))
		s_overlay = OV_NONE;
	if (!ov && s_ov_host.sheet)
		s_ov_host.sheet(NULL);
}

// Every frame a program runs: the sprites of the still on screen. extra is the egg being put in.
void prog_overlay_frame(int extra)
{
	if (!s_overlay || !s_ov_host.sprite)
		return;
	s_ov_host.sprite(NULL, NULL, 0.0f);
	switch (s_overlay) {
	case OV_CLOCK_CLOSED: ov_clock(false); break;
	case OV_CLOCK_OPEN: ov_clock(true); break;
	case OV_SAFE: ov_safe(); break;
	case OV_COINS: ov_coins(); break;
	case OV_BOX: ov_box(); break;
	case OV_BOX_WALL: ov_box_wall(); break;
	case OV_QUIZ: ov_quiz(); break;
	case OV_DRUMS: ov_drums(); break;
	case OV_EGG: ov_egg(extra); break;
	case OV_TOMBSTONE: ov_tombstone(); break;
	case OV_VIDEO: ov_video(); break;
	case OV_MARKER: ov_marker(); break;
	case OV_ELEVATOR: ov_elevator(false); break;
	case OV_ELEVATOR_2: ov_elevator(true); break;
	case OV_MANACLES: ov_manacles(); break;
	case OV_INCAR: ov_incar(); break;
	case OV_MEMO_SAFE: if (p_flag(607)) ov_safe(); break;
	case OV_MEMO_HAIR: ov_memo_hair(); break;
	case OV_MEMO_RINGS: ov_memo_rings(); break;
	case OV_MEMO_TABLETS: ov_memo_tablets(); break;
	}
}
