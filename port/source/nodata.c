#include "nodata.h"
#include "nodata_qr.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Files every finished pack has, one or more from each of the packer's main steps
// (tools/sh2conv/pack.py). The first is the one the folders below are searched for.
static const char* const s_need[] = {
	"ui/font.bin",            // ui (ui.py), read before anything is drawn
	"ui/font.t3x",
	"ui/title.t3x",
	"bg/ap/rooms.txt",        // rooms
	"chr/jms/hll_jms.s3c",    // james
	"sound/se/se.bnk",        // sound
};
#define NEED (int)(sizeof s_need / sizeof s_need[0])

// Where the folder ends up when it is copied one level off: the sh23ds folder alone at the root, the
// whole 3ds folder inside 3ds, the zip (writer.js: sh23ds-sd.zip) unzipped into a folder of its own,
// Docker's sd folder copied whole, or a make_pack.sh pack copied as it is.
static const char* const s_elsewhere[] = {
	"sdmc:/sh23ds",
	"sdmc:/3ds/3ds/sh23ds",
	"sdmc:/3ds/sh23ds/3ds/sh23ds",
	"sdmc:/3ds/sh23ds/sh23ds",
	"sdmc:/sh23ds-sd/3ds/sh23ds",
	"sdmc:/3ds/sh23ds-sd/3ds/sh23ds",
	"sdmc:/sd/3ds/sh23ds",
	"sdmc:/3ds/sd/3ds/sh23ds",
	"sdmc:/pack",
	"sdmc:/3ds/pack",
	"sdmc:/3ds/sh23ds/pack",
};
#define ELSEWHERE (int)(sizeof s_elsewhere / sizeof s_elsewhere[0])

#define GUIDE_URL "ankush.one/sh23ds/install.html"
#define PACKER_URL "ankush.one/sh23ds/packer.html"

// The QR code's colours, the website's (tools/make_site_qr.py), as RGB565
#define QR_LIGHT 0xef7e   // #e9edf0
#define QR_DARK 0x0883    // #0b1219
#define QR_QUIET 4        // modules of quiet zone around the code
#define QR_SCALE 5        // pixels per module: (29 + 8) * 5 = 185 of the touch screen's 240

#define RED "\x1b[31;1m"
#define CYAN "\x1b[36;1m"
#define DIM "\x1b[2m"
#define RESET "\x1b[0m"

static PrintConsole s_top, s_bottom;

static bool has_file(const char* dir, const char* file)
{
	char path[192];
	snprintf(path, sizeof path, "%s/%s", dir, file);
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void at(int row, int col, const char* text)
{
	printf("\x1b[%d;%dH%s", row, col, text);
}

// One line centred on the touch screen's 40 columns (the text without colour codes)
static void centred(int row, const char* colour, const char* text)
{
	int col = (40 - (int)strlen(text)) / 2 + 1;
	printf("\x1b[%d;%dH%s%s" RESET, row, col < 1 ? 1 : col, colour, text);
}

static void draw_qr(void)
{
	// the bottom screen's framebuffer is 240 x 320, a column per screen x, its rows bottom up
	u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	int side = (NODATA_QR_SIZE + 2 * QR_QUIET) * QR_SCALE;
	int x0 = (320 - side) / 2, y0 = 4;
	for (int y = 0; y < side; y++)
		for (int x = 0; x < side; x++) {
			int mx = x / QR_SCALE - QR_QUIET, my = y / QR_SCALE - QR_QUIET;
			bool dark = mx >= 0 && my >= 0 && mx < NODATA_QR_SIZE && my < NODATA_QR_SIZE &&
			            (nodata_qr[my] >> mx & 1);
			fb[(x0 + x) * 240 + 239 - (y0 + y)] = dark ? QR_DARK : QR_LIGHT;
		}
}

static void steps(int row)
{
	at(row, 2, "To set it up:");
	at(row + 1, 2, "1. On a computer, open the packer page");
	at(row + 2, 7, CYAN PACKER_URL RESET);
	at(row + 3, 5, "and choose your Silent Hill 2 folder, the");
	at(row + 4, 5, "one with sh2pc.exe. Save the zip it makes.");
	at(row + 5, 2, "2. Unzip it and copy its 3ds folder to the");
	at(row + 6, 5, "root of the SD card. It merges with the 3ds");
	at(row + 7, 5, "folder already there.");
	at(row + 8, 2, "3. Put the card back and start sh23ds again.");
}

bool nodata_check(const char* pack_dir)
{
	const char* missing[NEED];
	int nmissing = 0;
	for (int i = 0; i < NEED; i++)
		if (!has_file(pack_dir, s_need[i]))
			missing[nmissing++] = s_need[i];
	if (!nmissing)
		return true;
	const char* found = NULL;
	if (nmissing == NEED)
		for (int i = 0; i < ELSEWHERE && !found; i++)
			if (has_file(s_elsewhere[i], s_need[0]))
				found = s_elsewhere[i];

	consoleInit(GFX_TOP, &s_top);
	consoleInit(GFX_BOTTOM, &s_bottom);

	consoleSelect(&s_top);
	at(2, 2, DIM "SILENT HILL 2 for Nintendo 3DS" RESET);
	if (found) {
		at(4, 2, RED "Game data in the wrong folder" RESET);
		at(6, 2, "The game data is on the SD card, at");
		printf("\x1b[7;5H" CYAN "%s/" RESET, found);
		at(8, 2, "but sh23ds reads it from");
		printf("\x1b[9;5H" CYAN "%s/" RESET, pack_dir);
		at(11, 2, "Move that sh23ds folder so that it sits");
		at(12, 2, "directly inside the 3ds folder at the root of");
		at(13, 2, "the SD card. Use a computer, or a file manager");
		at(14, 2, "on the 3DS such as GodMode9.");
		at(16, 2, "If the right folder already holds save*.dat");
		at(17, 2, "files, keep them: they are your saves.");
	} else if (nmissing < NEED) {
		at(4, 2, RED "Game data incomplete" RESET);
		printf("\x1b[6;2H%s/ is there, but", pack_dir);
		at(7, 2, "these files are missing from it:");
		int row = 8;
		for (int i = 0; i < nmissing && i < 4; i++, row++)
			printf("\x1b[%d;5H" CYAN "%s" RESET, row, missing[i]);
		if (nmissing > 4)
			printf("\x1b[%d;5H" DIM "and %d more" RESET, row++, nmissing - 4);
		row++;
		at(row, 2, "The copy to the SD card may have stopped early,");
		at(row + 1, 2, "or the packer did not finish. Copy the 3ds");
		at(row + 2, 2, "folder from the packer to the SD card again,");
		at(row + 3, 2, "or run the packer again if the files are");
		at(row + 4, 2, "missing on the computer too.");
		at(row + 6, 2, "Copy your save*.dat files out first to keep");
		at(row + 7, 2, "your saves.");
	} else {
		at(4, 2, RED "Game data not found" RESET);
		at(6, 2, "sh23ds is the engine only. It plays the game");
		at(7, 2, "from your own copy of the PC Director's Cut,");
		at(8, 2, "converted on a computer by the packer.");
		printf("\x1b[10;2HIt looked in " CYAN "%s/" RESET, pack_dir);
		steps(12);
	}
	at(25, 2, "Full guide, with the Docker and ftpd options:");
	at(26, 4, CYAN GUIDE_URL RESET);
	at(27, 2, DIM "(or scan the code on the touch screen)" RESET);
	at(29, 2, "Press START to return to the HOME Menu.");

	consoleSelect(&s_bottom);
	centred(25, "", "Scan for the install guide");
	centred(26, CYAN, GUIDE_URL);
	centred(29, DIM, "START: back to the HOME Menu");
	draw_qr();

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
	return false;
}
