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
} Character;

bool chr_load(Character* c, const char* dir, const char* name, char* err, size_t errlen);
void chr_free(Character* c);
bool anim_load(Anim* a, const char* path);
void anim_free(Anim* a);
int clips_load(AnimClip* clips, int max, const char* path);

// Poses the character at a (fractional) frame; frames are blended linearly.
void chr_pose(Character* c, const Anim* a, float frame);

// Draws with the skinning shader bound; uBones is the uniform location of bones[]. Lit by the bound
// light environment (the shader outputs normal quaternions).
void chr_draw(Character* c, int uBones);
