#include "movsubs.h"
#include <string.h>

// Tables copied from the PC's data (the address after each name), every one ending in the PC's 65535
// terminator, which is not kept here. Stage numbers are the stages whose files hold the texts
// (port/PROGRAMS.md numbering, the pack's sNN.msg).

// murder.sfd: set up at 0x5780f0, table 0x8cbf58
static const u16 T_MURDER[][2] = {
	{ 45, 195 }, { 258, 375 }, { 375, 459 }, { 459, 501 }, { 501, 636 }, { 636, 723 },
	{ 723, 807 }, { 807, 948 },
};

// rouya.sfd: set up at 0x5827dd, table 0x8d4240
static const u16 T_ROUYA[][2] = {
	{ 63, 174 }, { 174, 285 }, { 348, 495 }, { 570, 657 }, { 657, 759 }, { 795, 891 },
	{ 891, 960 }, { 960, 1104 }, { 1104, 1203 }, { 1203, 1296 }, { 1296, 1386 }, { 1386, 1500 },
	{ 1500, 1614 }, { 1647, 1770 }, { 1770, 1896 }, { 1941, 2049 }, { 2049, 2130 }, { 2130, 2265 },
};

// korosu_a.sfd: set up at 0x5850dd, table 0x8d6250
static const u16 T_KOROSU_A[][2] = {
	{ 249, 375 }, { 375, 480 },
};

// korosu_b.sfd: set up at 0x5851aa, table 0x8d6288
static const u16 T_KOROSU_B[][2] = {
	{ 24, 78 }, { 78, 237 }, { 237, 300 }, { 300, 420 }, { 420, 519 }, { 519, 570 },
	{ 570, 612 }, { 660, 744 },
};

// saikai.sfd: set up at 0x588216, table 0x8d81b8
static const u16 T_SAIKAI[][2] = {
	{ 0, 45 }, { 45, 90 }, { 90, 255 }, { 255, 300 }, { 348, 477 }, { 501, 528 },
	{ 528, 585 }, { 585, 678 }, { 678, 744 }, { 744, 819 }, { 819, 918 }, { 918, 1035 },
	{ 1035, 1188 }, { 1230, 1284 }, { 1284, 1344 }, { 1344, 1413 }, { 1413, 1572 }, { 1572, 1692 },
	{ 1692, 1800 }, { 1800, 1896 },
};

// deai.sfd: set up at 0x592977, table 0x8e0b74
static const u16 T_DEAI[][2] = {
	{ 12, 54 }, { 246, 327 }, { 327, 405 }, { 405, 522 }, { 522, 582 }, { 582, 708 },
	{ 708, 798 }, { 798, 873 }, { 873, 966 },
};

// hei.sfd: set up at 0x5948db, table 0x8e0ac4
static const u16 T_HEI[][2] = {
	{ 36, 174 }, { 174, 210 }, { 210, 306 }, { 306, 429 }, { 429, 522 }, { 522, 606 },
	{ 606, 708 }, { 708, 774 }, { 774, 804 }, { 804, 873 },
};

// knife.sfd: set up at 0x598201, table 0x8e3394
static const u16 T_KNIFE[][2] = {
	{ 201, 279 },
};

// hakaba.sfd: set up at 0x5a0808, table 0x8ea2c0
static const u16 T_HAKABA[][2] = {
	{ 564, 624 }, { 630, 720 }, { 720, 831 }, { 831, 921 }, { 987, 1026 },
};

static const MovieSubs TABLES[] = {
	{ "murder", 42, 5, 8, T_MURDER },
	{ "rouya", 34, 13, 18, T_ROUYA },
	{ "korosu_a", 31, 13, 2, T_KOROSU_A },
	{ "korosu_b", 31, 25, 8, T_KOROSU_B },
	{ "saikai", 26, 7, 20, T_SAIKAI },
	{ "deai", 14, 23, 9, T_DEAI },
	{ "hei", 14, 13, 10, T_HEI },
	{ "knife", 10, 12, 1, T_KNIFE },
	{ "hakaba", 4, 5, 5, T_HAKABA },
};

const MovieSubs* movsubs_find(const char* movie)
{
	for (unsigned i = 0; i < sizeof TABLES / sizeof *TABLES; i++)
		if (!strcmp(TABLES[i].movie, movie))
			return &TABLES[i];
	return NULL;
}

int movsubs_at(const MovieSubs* s, u32 frame)
{
	for (int i = 0; i < s->n; i++)
		if (s->frames[i][0] < frame && frame <= s->frames[i][1])
			return i;
	return -1;
}
