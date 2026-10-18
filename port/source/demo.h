#pragma once
#include <3ds.h>
#include <citro3d.h>

// In-engine cutscenes (the PC's DramaDemo player, Event/demoview.c), from pack/demo/<key>.dem written
// by tools/sh2conv/demos.py. See port/CUTSCENES.md.
//
// A stage program asks for a cutscene with prog_demo_request(id); main.c's demo hook calls
// demo_start() once and then reports the scene as running until demo_update() says it is over.
// While a scene runs, main.c takes the camera from demo_camera(), draws the scene's characters with
// demo_draw() instead of James (when the scene has its own James) and shows demo_subtitle() on
// the bottom screen. START skips the scene, as on the PC.

// Loads the scene for a ProgDemo id (prog.h): script, motions, character models, voice. False when
// the pack has nothing for it (the scene then counts as watched, as before).
bool demo_start(const char* pack_dir, int prog_demo_id);
// True while anything of a request runs: a scene, or the voice of a voice-and-subtitles step.
bool demo_active(void);
// True while a scene is on screen (its camera and characters); a voice step alone keeps the game view.
bool demo_scene_shown(void);
// The demo's sequence plays its movies itself, in the PC's order (main.c then skips the name's).
bool demo_has_sequence(int prog_demo_id);

// One frame: advances the scene clock (30 frames a second), starts motions, the voice stream and
// subtitles. down: keys pressed this frame (START skips). True once the scene is over; the caller
// then calls demo_end().
bool demo_update(u32 down, float dt);

// Frees the models, motions and the voice stream.
void demo_end(void);

// Camera of the current frame (world space, Y down) and vertical field of view in radians.
bool demo_camera(C3D_Mtx* view, float* fovy, float eye[3]);
// Enhanced Edition "ReduceCutsceneFOV" (default on): keep the 4:3 horizontal framing on the 5:3 screen.
void demo_set_reduce_fov(bool on);

// Lighting for demo_draw: the game's light environment (bound again afterwards), the ambient James
// would get in this room and the brightness scale. Scenes with lights of their own add them on top.
void demo_set_lighting(C3D_LightEnv* game_env, const float ambient[3], float bright);

// Draws every visible character of the scene. The skinning program, projection and light
// environment must be bound; uModelView / uBones are the skin shader's uniform locations.
void demo_draw(const C3D_Mtx* view, int uModelView, int uBones);

// Scene number of the running scene (DramaDemo_PlayInfo.demo_no, the PC's cutscene id), -1 when none.
int demo_number(void);
// gCutsceneTimer (0x1f7d3b8): the scene frame minus one, as the stage code and the Enhanced Edition
// compare it.
float demo_timer(void);
// Called by demo_draw before each character with a factor for the character light (1 = normal),
// so main.c can scale its light environment. NULL: no hook.
void demo_light_hook(void (*set_scale)(float scale));

// True if the scene brings its own James (the player model is hidden while it plays).
bool demo_has_james(void);
bool demo_has_kind(int kind);      // a character of the running scene is of this kind (stagemdl.c)

// Text of the subtitle on screen now (the stage message the PC shows), NULL when none. *serial
// changes whenever the subtitle changes, so the caller redraws only then.
const char* demo_subtitle(int* serial);

// A voice-and-subtitles track (pack/demo/<key>.voc) that plays while the game goes on, as the PC's
// stage code starts some (demo_active stays false); tick it every game frame, true while it runs.
bool demo_voice_free(const char* pack_dir, const char* key);
bool demo_voice_free_tick(float dt);

// 0..1 black over the picture: short fades at the start and the end of a scene.
float demo_fade(void);

// Status line for the debug HUD.
const char* demo_status(void);

// Debug: the pack key ("tenbou") played for "demo <key>" in debug.txt, or NULL.
bool demo_start_key(const char* pack_dir, const char* key);
// Debug: jumps the scene to a frame (the voice is left out when the jump passes its start).
void demo_seek(float frame);
// Debug: "demo <key> <frame> hold" keeps the scene on that frame (faces and lights can be compared).
void demo_debug_hold(bool on);
