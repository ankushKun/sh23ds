#pragma once
#include <3ds.h>
#include <stdio.h>

// The pack keeps textures at the PC's sizes (512 for the map, up to 1024 x 512 for characters) with
// their whole mip chain. A console that keeps smaller ones reads a .t3x from the level whose largest
// side is at most max_side, leaving the levels above it out, and the result is an ordinary .t3x for
// Tex3DS_TextureImport. Files it can't trim (several sub-textures, a compressed body) come back whole.

// The file from f's current position, malloc'ed, without the levels above max_side; those are
// skipped on the card, not read. dropped (may be NULL): how many. NULL on a read error.
u8* t3x_read_capped(FILE* f, int max_side, long* size, int* dropped);
