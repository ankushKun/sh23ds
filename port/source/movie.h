#pragma once
#include <3ds.h>
#include <citro3d.h>

// Full-motion video: the game's Bink movies re-encoded by tools/sh2conv/movies.py as MPEG-1 program
// streams (<pack>/movie/<file>.mpg, MPEG-1 video up to 400x240 plus MP2 audio). pl_mpeg decodes video
// and audio on two threads (core 2 of the New 3DS when the system allows it), Y2R converts each frame
// into a tiled RGB565 texture, and the audio plays on ndsp channel MOVIE_CH (audio.c owns 0, sfx.c
// 8..23). Frames are shown by the audio clock, so picture and sound stay together. See port/MOVIES.md.

#define MOVIE_CH 2

// After C3D_Init and audio_init. top is the render target movies are drawn on.
bool movie_init(C3D_RenderTarget* top);
void movie_exit(void);

// Names are the ones the PC game asks for ("toilet", "open_voc", "ending", "end_mria", ...): some map
// onto another file's audio track the way Movie_BinkPlay (0x43d770) does it. Plain file names work too.
// Returns false if the file is missing or unreadable.
bool movie_start(const char* name);
bool movie_exists(const char* name);    // the pack has the movie (the title's Movie Menu)
// Once per frame, outside C3D_FrameBegin/End (stopping waits for the GPU). down: hidKeysDown().
// A or START skips. False once the movie is over (it has stopped itself then).
bool movie_update(u32 down);
// Draws the current frame to fill the top screen (black bars around smaller pictures). Call between
// C3D_FrameBegin and C3D_FrameEnd; it sets every GPU state it needs.
void movie_draw(void);
void movie_stop(void);
bool movie_active(void);

// Plays a movie to the end (or until skipped) with its own frame loop, then returns. Prints timing on
// the bottom screen. False if it could not start or the app is closing (aptMainLoop returned false).
bool movie_play(const char* name);

// Debug: log per-frame decode and conversion times to <pack>/movie_log.txt, and convert YUV to RGB on
// the CPU instead of Y2R (for emulators without Y2R; real hardware should keep Y2R).
void movie_debug(bool log, bool cpu_convert);
void movie_skip_all(bool on);        // the playtest driver: movie_play returns at once

const char* movie_status(void);
