#pragma once
#include <stdbool.h>

// Quality knobs that trade looks for frame time. The defaults are the New 3DS settings; the
// hardware profile sets them (profile.c profile_apply_quality, at start and whenever the System page
// changes the profile, quality level or frame rate), and debug.txt can override single ones (main.c).
// These are the only place fog.c, shadow.c, main.c and perf_pace read them from, so a change applies
// at once.
typedef struct {
	int fog_sprites;          // fog particles simulated and drawn (fog.c, at most 144)
	bool stencil_shadows;     // the flashlight's character shadows (shadow.c)
	bool drop_shadows;        // the drop shadows of the lit rooms (main.c shadow_frame)
	float tile_distance;      // how close James must be to a touching tile for it to load (main.c)
	int frame_cap;            // 0: 60 when the frame fits, an even 30 when it does not; 30 or 60 fixed
	float fog_fill_3d;        // in 3D, the fog sprites' area per eye in screens (fog.c stereo_thin); 0: no limit
} Quality;

const Quality* quality(void);
void quality_set(const Quality* q);
