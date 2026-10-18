#pragma once
#include <3ds.h>
#include <citro3d.h>

// Skinned character (S3C v2) and its animations (S3A v1), see tools/sh2conv/chr.py.

#define CHR_PALETTE_MAX 28
#define CHR_VERTEX_SIZE 32
#define CHR_MAX_BONES 96
#define CHR_MAX_TEX 8

typedef struct { float m[3][4]; } Mtx34;   // row-major 3x4 affine

typedef struct {
	u32 texture_slot, first_index, index_count, flags, palette_count;
	u8 palette[CHR_PALETTE_MAX];
	u16* indices;     // own linear allocation
} ChrDraw;

typedef struct {
	s16 rot[3];
	u16 flags;        // bit0 rotation, bit1 translation
	float trans[3];
} AnmKey;

typedef struct {
	u32 bones, frames;
	AnmKey* keys;     // frames * bones
} Anim;

typedef struct {
	char name[16];
	u32 start, end;
	bool loop;
} AnimClip;

typedef struct {
	u32 n_bones, n_draws, n_vertices, n_textures;
	float bbox_min[3], bbox_max[3];
	u8 parents[CHR_MAX_BONES];
	Mtx34 inv_bind[CHR_MAX_BONES];
	Mtx34 bind_local[CHR_MAX_BONES];
	ChrDraw* draws;
	void* vertices;   // linear memory
	C3D_Tex tex[CHR_MAX_TEX];
	bool tex_ok[CHR_MAX_TEX];
	Mtx34 skin[CHR_MAX_BONES];   // current pose: world * inv_bind
	void* kg1;                   // <name>.kg1 shadow model (shadow.c), NULL when there is none
	u32 kg1_size;
} Character;

bool chr_load(Character* c, const char* dir, const char* name, char* err, size_t errlen);
void chr_free(Character* c);
bool anim_load(Anim* a, const char* path);
void anim_free(Anim* a);
int clips_load(AnimClip* clips, int max, const char* path);

// Poses the character at a (fractional) frame; frames are blended linearly.
void chr_pose(Character* c, const Anim* a, float frame);

// Crossfades between clips, as the PC's anime interpolation does (0x551820 with the modes 0x538920
// sets): when a clip starts, chr_blend_start keeps the pose shown last and chr_pose_blend mixes it
// into the new clip's pose over `seconds`. Time based, so it is the same at any frame rate.
typedef struct {
	Mtx34 from[CHR_MAX_BONES], last[CHR_MAX_BONES];   // local transforms
	float t, dur;
	bool have_last;
	const Anim* sync;                                  // see chr_blend_start_sync
	float sync_from, sync_to, sync_len;
	// The head turned on top of the clip (James looking at what is near, 0x535720): yaw (left
	// positive) and pitch (up positive) in model space, the head bone taking all of it and the
	// neck bone `neck_share` more. Bones < 0: none.
	int look_head, look_neck;
	float look_yaw, look_pitch, neck_share;
} ChrBlend;
void chr_blend_start(ChrBlend* b, float seconds);
// The PC's mode 10 (walk, run and strafe clips into each other): the old clip, starting at frame
// from_start of anim a, keeps playing in step with the new one (starting at to_start, both len
// frames long) while it fades out, so the feet do not stop.
void chr_blend_start_sync(ChrBlend* b, const Anim* a, float from_start, float to_start, float len, float seconds);
void chr_pose_blend(Character* c, const Anim* a, float frame, ChrBlend* b, float dt);

// Draws with the skinning shader bound; uBones is the uniform location of bones[]. Lit by the bound
// light environment (the shader outputs normal quaternions), whose colours must come from the two
// setters below: chr_draw's combiner doubles the lighting and clamps it to 1.
void chr_draw(Character* c, int uBones);

// The PICA keeps 8 bits per light colour (255 is 1.0): a brighter light wraps round to a dim one,
// although Azahar reads 10 bits and shows it as meant. Characters' lights have colours up to about
// 2 (the PC's lights times the brightness), so they go to the GPU at half strength, clamped to
// what the register holds, and chr_draw doubles them back.
void chr_light_color(C3D_Light* l, float r, float g, float b, float mat_diffuse);
void chr_light_ambient(C3D_LightEnv* env, float r, float g, float b);
