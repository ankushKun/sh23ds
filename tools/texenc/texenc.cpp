// texenc: the pixel encoders of tex3ds (devkitPro, GPL-3.0, by Michael Theall) for the browser packer,
// which cannot run tex3ds itself. tools/sh2conv/t3x.py loads the PNG, builds the mipmaps and writes
// the .t3x header; this file turns one level of 8-bit RGBA pixels into the bytes tex3ds writes for it.
//
// The conversions follow tex3ds's encode.cpp, swizzle.cpp and quantum.h. tex3ds works on ImageMagick
// quanta: an 8-bit PNG value v becomes v * 257 at 16 bits, and quantum_to_bits<b>() of that is exactly
// v >> (8 - b), so plain shifts give the same bits. ETC1 goes through rg_etc1 (zlib licence, by Rich
// Geldreich) at high quality, as "tex3ds -q high" does.
//
// Built two ways (tools/texenc/build.sh): to WebAssembly for site/packer/, and as a shared library
// for tools/texenc/check.py, which compares t3x.py + this encoder with tex3ds's own output.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "rg_etc1.h"

namespace {

enum Format {
	RGBA8888 = 0x00, RGB888 = 0x01, RGBA5551 = 0x02, RGB565 = 0x03, RGBA4444 = 0x04, LA88 = 0x05,
	HILO88 = 0x06, L8 = 0x07, A8 = 0x08, LA44 = 0x09, L4 = 0x0A, A4 = 0x0B, ETC1 = 0x0C, ETC1A4 = 0x0D,
};

rg_etc1::etc1_quality quality = rg_etc1::cHighQuality;

inline unsigned bits(unsigned v, int b) { return v >> (8 - b); }

// quantum.h luminance(): BT.709 on linearised sRGB, back to sRGB, as a 16-bit quantum
double gamma_inverse(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double gamma(double v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }
unsigned luminance_bits(const uint8_t *p, int b)
{
	double v = gamma(0.212655 * gamma_inverse(257.0 * p[0] / 65535.0) +
	                 0.715158 * gamma_inverse(257.0 * p[1] / 65535.0) +
	                 0.072187 * gamma_inverse(257.0 * p[2] / 65535.0));
	v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
	unsigned q = (unsigned)(v * 65535.0);       // Magick::Quantum (16 bits)
	return ((1u << b) * q) / 65536u;
}

// swizzle.cpp: the 8x8 tile's pixels in the PICA's Morton order, by the same foursome rotations
void swizzle(uint32_t *t)
{
	static const unsigned char table[][4] = {
		{ 2, 8, 16, 4 }, { 3, 9, 17, 5 }, { 6, 10, 24, 20 }, { 7, 11, 25, 21 },
		{ 14, 26, 28, 22 }, { 15, 27, 29, 23 }, { 34, 40, 48, 36 }, { 35, 41, 49, 37 },
		{ 38, 42, 56, 52 }, { 39, 43, 57, 53 }, { 46, 58, 60, 54 }, { 47, 59, 61, 55 },
	};
	for (const auto &e : table) {
		uint32_t tmp = t[e[0]];
		t[e[0]] = t[e[1]];
		t[e[1]] = t[e[2]];
		t[e[2]] = t[e[3]];
		t[e[3]] = tmp;
	}
	uint32_t s;
	s = t[12]; t[12] = t[18]; t[18] = s;
	s = t[13]; t[13] = t[19]; t[19] = s;
	s = t[44]; t[44] = t[50]; t[50] = s;
	s = t[45]; t[45] = t[51]; t[51] = s;
}

uint8_t *put16(uint8_t *o, unsigned v) { o[0] = v; o[1] = v >> 8; return o + 2; }

// one pixel of an uncompressed format (encode.cpp)
uint8_t *pixel(int fmt, const uint8_t *c, uint8_t *o)
{
	switch (fmt) {
	case RGBA8888: o[0] = c[3]; o[1] = c[2]; o[2] = c[1]; o[3] = c[0]; return o + 4;
	case RGB888: o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; return o + 3;
	case RGBA5551:
		return put16(o, bits(c[0], 5) << 11 | bits(c[1], 5) << 6 | bits(c[2], 5) << 1 | bits(c[3], 1));
	case RGB565: return put16(o, bits(c[0], 5) << 11 | bits(c[1], 6) << 5 | bits(c[2], 5));
	case RGBA4444:
		return put16(o, bits(c[0], 4) << 12 | bits(c[1], 4) << 8 | bits(c[2], 4) << 4 | bits(c[3], 4));
	case LA88: o[0] = c[3]; o[1] = luminance_bits(c, 8); return o + 2;
	case HILO88: o[0] = c[1]; o[1] = c[0]; return o + 2;
	case L8: o[0] = luminance_bits(c, 8); return o + 1;
	case A8: o[0] = c[3]; return o + 1;
	case LA44: o[0] = luminance_bits(c, 4) << 4 | bits(c[3], 4); return o + 1;
	}
	return o;
}

// etc1_common(): the tile's four 4x4 blocks (left-right, then top-bottom), each optionally preceded
// by its 4-bit alpha (column-major nibbles) and stored with the ETC1 block's bytes reversed
uint8_t *etc1_tile(const uint8_t *rgba, int stride, bool alpha, uint8_t *o)
{
	rg_etc1::etc1_pack_params params;
	params.clear();
	params.m_quality = quality;
	for (int j = 0; j < 8; j += 4) {
		for (int i = 0; i < 8; i += 4) {
			uint8_t in[64], blk[8], a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
			for (int y = 0; y < 4; ++y) {
				for (int x = 0; x < 4; ++x) {
					const uint8_t *c = rgba + ((j + y) * stride + i + x) * 4;
					in[y * 16 + x * 4 + 0] = c[0];
					in[y * 16 + x * 4 + 1] = c[1];
					in[y * 16 + x * 4 + 2] = c[2];
					in[y * 16 + x * 4 + 3] = 0xff;
					if (alpha)
						a[2 * x + y / 2] |= bits(c[3], 4) << ((y & 1) ? 4 : 0);
				}
			}
			uint32_t px[16];
			std::memcpy(px, in, sizeof px);
			rg_etc1::pack_etc1_block(blk, px, params);
			if (alpha) {
				std::memcpy(o, a, 8);
				o += 8;
			}
			for (int k = 0; k < 8; ++k)
				*o++ = blk[7 - k];
		}
	}
	return o;
}

bool inited;

} // namespace

extern "C" {

// rg_etc1's effort for ETC1 / ETC1A4: 0 low, 1 medium, 2 high (tex3ds -q high, the default)
void texenc_set_quality(int q)
{
	quality = q <= 0 ? rg_etc1::cLowQuality : q == 1 ? rg_etc1::cMediumQuality : rg_etc1::cHighQuality;
}

// Bytes texenc_level() writes for a w x h level, or 0 for a format it doesn't know.
int texenc_size(int fmt, int w, int h)
{
	static const int bpp[] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };
	return fmt >= 0 && fmt <= ETC1A4 ? w * h * bpp[fmt] / 8 : 0;
}

// Encodes one level (w and h multiples of 8) of 8-bit RGBA pixels, row by row, into out the way
// tex3ds lays it out: 8x8 tiles left to right, top to bottom. Returns the bytes written.
int texenc_level(int fmt, const uint8_t *rgba, int w, int h, uint8_t *out)
{
	if (!inited) {
		rg_etc1::pack_etc1_block_init();
		inited = true;
	}
	uint8_t *o = out;
	for (int ty = 0; ty < h; ty += 8) {
		for (int tx = 0; tx < w; tx += 8) {
			if (fmt == ETC1 || fmt == ETC1A4) {
				o = etc1_tile(rgba + (ty * w + tx) * 4, w, fmt == ETC1A4, o);
				continue;
			}
			uint32_t t[64];
			for (int y = 0; y < 8; ++y)
				std::memcpy(t + y * 8, rgba + ((ty + y) * w + tx) * 4, 32);
			swizzle(t);
			if (fmt == L4 || fmt == A4) {   // two pixels a byte, low nibble first
				for (int k = 0; k < 64; k += 2) {
					const uint8_t *c0 = reinterpret_cast<const uint8_t *>(t + k);
					const uint8_t *c1 = reinterpret_cast<const uint8_t *>(t + k + 1);
					unsigned v0 = fmt == A4 ? bits(c0[3], 4) : luminance_bits(c0, 4);
					unsigned v1 = fmt == A4 ? bits(c1[3], 4) : luminance_bits(c1, 4);
					*o++ = v0 | v1 << 4;
				}
				continue;
			}
			for (int k = 0; k < 64; ++k)
				o = pixel(fmt, reinterpret_cast<const uint8_t *>(t + k), o);
		}
	}
	return (int)(o - out);
}

// For check.py: decodes one ETC1 / ETC1A4 level that texenc_level (or tex3ds) wrote back to 8-bit
// RGBA rows. Returns 0, or -1 for another format.
int texenc_decode_etc1(int fmt, const uint8_t *in, int w, int h, uint8_t *rgba)
{
	if (fmt != ETC1 && fmt != ETC1A4)
		return -1;
	for (int ty = 0; ty < h; ty += 8) {
		for (int tx = 0; tx < w; tx += 8) {
			for (int j = 0; j < 8; j += 4) {
				for (int i = 0; i < 8; i += 4) {
					uint8_t a[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
					if (fmt == ETC1A4) {
						std::memcpy(a, in, 8);
						in += 8;
					}
					uint8_t blk[8];
					for (int k = 0; k < 8; ++k)
						blk[k] = in[7 - k];
					in += 8;
					uint32_t px[16];
					rg_etc1::unpack_etc1_block(blk, px);
					for (int y = 0; y < 4; ++y) {
						for (int x = 0; x < 4; ++x) {
							uint8_t *o = rgba + ((ty + j + y) * w + tx + i + x) * 4;
							std::memcpy(o, px + y * 4 + x, 4);
							unsigned n = a[2 * x + y / 2] >> ((y & 1) ? 4 : 0) & 0xf;
							o[3] = fmt == ETC1A4 ? n * 17 : 0xff;
						}
					}
				}
			}
		}
	}
	return 0;
}

} // extern "C"
