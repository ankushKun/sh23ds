#pragma once
#include <3ds.h>
#include "map.h"

// The PC's options screens (0x469ad0 runs them, 0x461600 / 0x464eb0 draw them, the texts are
// data/etc/message/option_msg_e.mes, pack/events/option.msg), drawn on the top screen in the PC's
// layout and driven from the touch screen (touchui.c) and the buttons:
//
//   OPTIONS           Game Options, Advanced Options, Control Options, Control Type, Subtitles,
//                     Speaker Config., BGM Volume, SE Volume (the PC's Vibration and Language
//                     rows have nothing to set on the 3DS)
//   GAME OPTIONS      Weapon Control, Map, Walk/Run Control, View Control, Blood Color, and
//                     Bullet Adjust once a game has been cleared
//   ADVANCED OPTIONS  Brightness Level (its own screen), Noise Effect, Shadows, Fog, then the
//                     port's display settings (messages, Enhanced Edition looks)
//   Control Options   the PC's key setup has no 3DS counterpart: the port's control settings
//
// Values change at once; leaving the screen with changes asks "Change the settings?" as the PC
// does, and No puts the old values back. See PROGRAMS.md "Options".

typedef struct {
	// OPTIONS (Playing_Info 0x1dbeb80 + offsets, defaults from 0x4f6ae0 / 0x4f6c70)
	u8 control_type;     // gControlType: 0 Rotational Style (3D tank), 1 Directional Style
	u8 subtitles;        // 0x1dbec07: 0 Off, 1 On
	u8 speaker;          // 0x1dbebfb on the PC; here 0 Mono, 1 Stereo, 2 Surround (the 3DS's modes)
	u8 bgm, se;          // 0x1dbebfc / 0x1dbebfd: 0..15, 12 by default
	// ADVANCED OPTIONS
	u8 bright;           // 0x1dbebf6: 0..7, 3 by default
	u8 noise;            // 0x1dbec0b: Noise Effect, 0 Off (the PC's default), 1 On
	u8 shadows;          // 0xa35ffc: 0 Off, 1 On
	u8 fog;              // 0xa35ff8: 0 Simple (no fog particles), 1 Complex
	// GAME OPTIONS
	u8 weapon;           // 0x1dbebfe: 0 Normal (hold R to aim), 1 Switch (R toggles)
	u8 map_zoom;         // 0x1dbec0a: the map opens 0 Zoom In, 1 Zoom Out
	u8 walk_run;         // gRunOption: 0 Walk, 1 Analog, 2 Run
	u8 view;             // 0x1dbec00: View Control 0 Normal, 1 Switch
	u8 blood;            // 0x1dbebff: 0 Normal, 1 Green, 2 Violet, 3 Black
	u8 bullet;           // 0x1dbec05: Bullet Adjust 1..3
	// the port's own: display and Enhanced Edition settings (all EE ones on by default as in the EE)
	u8 msg_top;          // messages and subtitles on the top screen
	u8 ps2_flash;        // EE "PS2FlashlightBrightness"
	u8 noise_ps2;        // EE "PS2StyleNoiseFilter"
	u8 ps2_camera;       // EE PS2 camera speed
	u8 cutscene_fov;     // EE "ReduceCutsceneFOV"
	u8 toggle_run;       // EE "EnableToggleSprint"
	u8 hold_stomp;       // EE hold A to stomp
	u8 swap_attack;      // EE light and heavy attacks swapped
} Options;

typedef struct {
	void (*apply)(const Options* o);   // after every change (and after No puts the old ones back)
	void (*save)(const Options* o);    // when the settings are kept
	bool cleared;                      // a game has been cleared: Bullet Adjust shows (0x1dbeb85)
} OptHost;

void opt_defaults(Options* o);
void opt_load(Options* o, const char* path);    // options.txt; keys it lacks keep their value
void opt_save(const Options* o, const char* path);

void optmenu_open(Options* live, const OptHost* host);
bool optmenu_active(void);
bool optmenu_update(u32 down);           // true while it is open
// The brightness screen is up: the caller draws the scene undimmed so the level can be judged.
bool optmenu_brightness(void);
// Inside a C3D frame on the top screen, after the scene: the panels through draw (main.c's 2D
// service), the text through font.c (the caller flushes it).
void optmenu_draw_top(MapDrawFn draw);
