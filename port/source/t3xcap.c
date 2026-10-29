#include "t3xcap.h"
#include "sdio.h"
#include <stdlib.h>
#include <string.h>

// tex3ds's header for one sub-texture (tools/sh2conv/t3x.py header): u16 count, a byte with the
// sides as log2 - 3 (width in bits 0..2, height in 3..5), the format, the number of levels below
// the first; the sub-texture's u16 width and height and four u16 coordinates. Then the body's own
// header: type 0 with a 24-bit length, or 0x80 with a 32-bit one and three spare bytes.
#define HEAD 17
#define BODY_MAX 8

static int bits_per_texel(u8 fmt)
{
	switch (fmt) {
	case 0x00: return 32;                                            // RGBA8
	case 0x01: return 24;                                            // RGB8
	case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: return 16;
	case 0x07: case 0x08: case 0x09: case 0x0d: return 8;            // L8, A8, LA4, ETC1A4
	case 0x0a: case 0x0b: case 0x0c: return 4;                       // L4, A4, ETC1
	}
	return 0;
}

typedef struct {
	int levels;      // left out from the top
	u32 skip;        // their bytes
	u32 body;        // the whole chain's bytes
	int body_head;   // 4 or 8
} Plan;

static bool plan(const u8* h, long avail, int max_side, Plan* p)
{
	if (avail < HEAD + 4)
		return false;
	u16 count;
	memcpy(&count, h, 2);
	int lw = (h[2] & 7) + 3, lh = (h[2] >> 3 & 7) + 3, bpp = bits_per_texel(h[3]), mips = h[4];
	if (count != 1 || !bpp)
		return false;
	if (h[HEAD] == 0x00) {
		p->body = h[HEAD + 1] | h[HEAD + 2] << 8 | h[HEAD + 3] << 16;
		p->body_head = 4;
	} else if (h[HEAD] == 0x80 && avail >= HEAD + 8) {
		memcpy(&p->body, h + HEAD + 1, 4);
		p->body_head = 8;
	} else
		return false;                         // compressed: Tex3DS reads it whole
	p->levels = 0;
	p->skip = 0;
	while (p->levels < mips && ((1 << (lw - p->levels)) > max_side || (1 << (lh - p->levels)) > max_side)) {
		p->skip += ((u32)1 << (lw - p->levels + lh - p->levels)) * bpp / 8;
		p->levels++;
	}
	return p->levels > 0 && p->skip < p->body;
}

// The header without p's levels, into out (HEAD + BODY_MAX bytes); returns its length
static int new_header(const u8* h, const Plan* p, u8* out)
{
	memcpy(out, h, HEAD);
	int k = p->levels;
	int lw = (h[2] & 7) - k, lh = (h[2] >> 3 & 7) - k;
	out[2] = (u8)(lw | lh << 3);
	out[4] = (u8)(h[4] - k);
	u16 w, ht;
	memcpy(&w, h + 5, 2);
	memcpy(&ht, h + 7, 2);
	w >>= k;
	ht >>= k;
	memcpy(out + 5, &w, 2);
	memcpy(out + 7, &ht, 2);
	u32 n = p->body - p->skip;
	if (n < 0x1000000) {
		out[HEAD] = 0;
		out[HEAD + 1] = n & 0xff;
		out[HEAD + 2] = n >> 8 & 0xff;
		out[HEAD + 3] = n >> 16 & 0xff;
		return HEAD + 4;
	}
	out[HEAD] = 0x80;
	memcpy(out + HEAD + 1, &n, 4);
	memset(out + HEAD + 5, 0, 3);
	return HEAD + 8;
}

u8* t3x_read_capped(FILE* f, int max_side, long* size, int* dropped)
{
	*size = 0;
	if (dropped)
		*dropped = 0;
	long start = ftell(f);
	fseek(f, 0, SEEK_END);
	long n = ftell(f) - start;
	fseek(f, start, SEEK_SET);
	if (n <= 0)
		return NULL;
	u8 h[HEAD + BODY_MAX];
	long got = (long)sd_fread(h, n < (long)sizeof h ? (size_t)n : sizeof h, f);
	Plan p;
	if (plan(h, got, max_side, &p)) {
		u8 head[HEAD + BODY_MAX];
		int hl = new_header(h, &p, head);
		u32 rest = p.body - p.skip;
		u8* d = malloc(hl + rest);
		if (!d)
			return NULL;
		memcpy(d, head, hl);
		fseek(f, start + HEAD + p.body_head + p.skip, SEEK_SET);
		if (sd_fread(d + hl, rest, f) != rest) {
			free(d);
			return NULL;
		}
		*size = hl + rest;
		if (dropped)
			*dropped = p.levels;
		return d;
	}
	u8* d = malloc(n);
	if (!d)
		return NULL;
	memcpy(d, h, got);
	if (sd_fread(d + got, n - got, f) != (size_t)(n - got)) {
		free(d);
		return NULL;
	}
	*size = n;
	return d;
}
