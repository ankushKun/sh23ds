#pragma once
#include <3ds.h>
#include <citro3d.h>

// The PC's "Noise Effect" (Advanced Options, Playing_Info byte 0x1dbec0b, off by default): a film
// grain over the picture. Every frame 0x478520 fills a 128x128 DXT1 texture with random 4x4 blocks
// whose two colours are 0xde99 (a warm light grey) and black, so each texel is one of four greys,
// and 0x4781e0 draws it over the screen four times across and down with the texture factor
// (grey 0x9458c5, alpha from 0x9458c2) as MODULATE2X, blended INVSRCALPHA / SRCALPHA so the
// scene keeps most of its colour. The brightness setting (0x47cb30) sets the grey and the alpha.
// The Enhanced Edition's "PS2StyleNoiseFilter" (Patches/PS2NoiseFilter.cpp, zlib licence) makes
// the grey 7.4 times darker and the grain stronger, which darkens the picture like the PS2.
//
// Here the texture is 4-bit luminance with the same four levels and tint, refilled every frame
// (two textures in turn so the GPU never reads one being written), and drawn one texel per
// pixel across the top screen.

bool noise_init(void);
void noise_exit(void);
// on: the option; level: brightness 0..7; ps2: the Enhanced Edition's PS2 style.
void noise_set(bool on, int level, bool ps2);
bool noise_on(void);
// Draws the grain over the top screen. The caller has bound the 2D program (vertices in
// top-screen pixels, y up, as main.c's ui_draw) and turned the depth test off.
void noise_draw(void);
// Over a movie (movie.c's program: float x, y and float u, v, an ortho projection, y up), on the
// picture only (x0, y0)-(x1, y1) as the Enhanced Edition keeps it off the black bars.
void noise_draw_movie(float x0, float y0, float x1, float y1);
