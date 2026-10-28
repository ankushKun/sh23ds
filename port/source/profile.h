#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>

// Hardware profile: Old 3DS (268 MHz, no L2 cache, 64..96 MB for the application, no C-Stick, ZL
// or ZR) or New 3DS (804 MHz, L2 cache, 124 MB). The console is detected at start-up and the
// player can override it in options.txt (and Advanced Options). The profile sets the defaults for
// the quality level, the frame rate and the controls layout, each with its own Auto / override.
// See port/OLD3DS.md.

typedef enum { PROFILE_OLD3DS = 0, PROFILE_NEW3DS = 1 } HwProfile;
typedef enum { QUALITY_LOW = 0, QUALITY_HIGH = 1 } QualityLevel;   // quality.h has the Quality struct
typedef enum { CONTROLS_LAYOUT_OLD = 0, CONTROLS_LAYOUT_NEW = 1 } ControlsLayout;

// options.txt values of "hwprofile", "quality", "ctrllayout" and "framerate" (1: 30, 2: 60)
enum { CHOICE_AUTO = 0, CHOICE_LOW_OLD = 1, CHOICE_HIGH_NEW = 2 };

// First thing in main(), before gfxInitDefault and the audio and stream threads: detects the console,
// reads the three keys from options_path, turns on the New 3DS clock and L2 cache when the console
// has them, and on an Old 3DS asks for a share of the system core (APT_SetAppCpuTimeLimit) so a
// helper thread can run there.
void profile_init(const char* options_path);

bool profile_console_new(void);      // the hardware: APT_CheckNew3DS
HwProfile profile_get(void);           // the active profile (console or override)
static inline bool profile_new(void) { return profile_get() == PROFILE_NEW3DS; }
QualityLevel profile_quality(void);       // Low on the Old 3DS profile unless overridden

// ---- controls (the bindings, defaults and remap page are controls.c's, port/CONTROLS.md)
//
// The layout says which buttons the player has: CONTROLS_LAYOUT_OLD has only L, R, A, B, X, Y,
// the D-Pad, the Circle Pad, START, SELECT and the touch screen; CONTROLS_LAYOUT_NEW adds ZL, ZR and
// the C-Stick (a New 3DS, or an Old 3DS with the Circle Pad Pro, which libctru reads through ir:rst
// the same way). Auto follows the console. Read it every frame (it can change in Control Options).
// controls.h includes this header for these declarations.
ControlsLayout controls_layout(void);
static inline bool controls_have_cstick(void) { return controls_layout() == CONTROLS_LAYOUT_NEW; }

// The player's actions; controls.c binds each to buttons per layout (controls_default, controls_name).
typedef enum {
	ACT_RUN,            // hold
	ACT_ACTION,         // check, open, pick up, attack while aiming, next message
	ACT_AIM,            // hold: ready the weapon
	ACT_WEAPON_NEXT,
	ACT_FLASHLIGHT,
	ACT_ITEMS,
	ACT_LOOK,           // hold: look ahead / search view (the PC's view key)
	ACT_TURN180,        // quick turn
	ACT_STRAFE_LEFT,
	ACT_STRAFE_RIGHT,
	ACT_MAP,
	ACT_TARGET_LEFT,    // while aiming: the next enemy to the left / right (0x54f100)
	ACT_TARGET_RIGHT,
	ACT_PAUSE,
	ACT_CANCEL,
	ACT_COUNT
} ControlsAction;

// ---- what the profile and quality level decide (read by the subsystems)

// Light helper threads (a movie's sound): core 2 on a New 3DS, core 1 on an Old 3DS when the system
// granted its share of that core, else -2 (core 0, below the main thread).
int profile_helper_core(void);
// The music refill thread: core 2 on a New 3DS, core 0 below the main thread on an Old 3DS. Five-layer
// AIX music took 13 % of a core in Azahar on core 0 and 36 % of the time on core 1, whose 30 % share
// it outgrows once real hardware is slower than Azahar's count; core 0 has the time, since the main
// thread waits for the vertical blank most of each frame at 30 fps.
int profile_music_core(void);
// Room and tile textures load from their second mip level (half the width and height, a quarter of
// the memory) when true: when the linear heap is under 32 MB (a 3dsx started from a title with
// little memory) or "texhalf 1" asks for it. Room textures turned out to be well under 1 MB of a
// room's memory (port/OLD3DS.md), so the Old 3DS profile keeps them full size. Characters always do.
bool profile_tex_half(void);
u32 profile_texcache_budget(void);  // texture kept from earlier rooms: 12 MB, 6 MB under 80 MB of linear heap
int profile_max_neighbours(void);   // resident touching tiles in the open areas (main.c MAX_NB at most)
float profile_stream_dist(void);    // load a touching tile when James is this close to it
int profile_fog_sprites(void);      // fog sprite budget (fog.c FOG_MAX at most)
bool profile_shadows(void);         // character stencil shadows
int profile_chr_point_lights(void); // point/spot lights per character besides the flashlight (0..3)
int profile_movie_scale(void);      // 1: movies decode at full size, 2: the half-size set if the pack has it
// Frame rate cap: 30 on the low quality level (every frame lasts two vertical blanks, so frames
// that alternate between one and two blanks on an Old 3DS do not judder), 60 on the high one;
// "framerate" in options.txt overrides it. profile_apply_frame_rate() passes it to citro3d (after
// C3D_Init; profile_set_choices calls it again).
int profile_frame_rate(void);
void profile_apply_frame_rate(void);

// ---- options.txt
// profile_init reads the four keys itself (it runs before the rest of the options load); optmenu.c
// keeps them in its Options struct, writes them with the others and shows them in Advanced
// Options (Hardware, Quality, Frame Rate) and Control Options (Layout). options_apply hands them
// back here after every change.
bool profile_option(const char* key, int val);
void profile_set_choices(int hw, int quality, int layout, int fps);   // CHOICE_* values
// quality.h (the lighting batch's knobs), when it is in the tree, gets the profile's values from
// profile_init and every profile_set_choices; debug.txt can still override single ones.
void profile_apply_quality(void);
const char* profile_status(void);       // one line for perf.txt
