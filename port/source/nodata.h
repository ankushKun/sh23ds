#pragma once
#include <stdbool.h>

// The start-up check for the game data (a 3DS-only screen, no PC counterpart). sh23ds ships without
// the game: the player makes the pack from their own copy with the packer and copies it to the SD
// card, so a first start without it, or with it in the wrong folder or half copied, is common.
//
// True when pack_dir holds a finished pack. Otherwise shows on both screens what is wrong and how to
// make and copy the game data (with a QR code of the website's install guide), until START or the
// HOME Menu closes the app, and returns false: main() then exits. Call it after gfxInitDefault and
// before consoleInit and C3D_Init, since it takes both screens for libctru's console.
bool nodata_check(const char* pack_dir);
