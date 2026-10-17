#include "chr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tex3ds.h>
#include "stream.h"

typedef struct {
	char magic[4];
	u32 version, bones, draws, vertices, indices, textures;
	float bbox_min[3], bbox_max[3];
	u32 ofs_parents, ofs_inv_bind, ofs_bind_local, ofs_draws, ofs_vertices, ofs_indices, ofs_textures;
} S3CHeader;

static void* read_file(const char* path, long* size)
{
	void* pre = stream_take_file(path, size);   // read ahead for a door (main.c warp_prefetch)
	if (pre)
		return pre;
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* d = malloc(*size);
	if (d && fread(d, 1, *size, f) != (size_t)*size) {
		free(d);
		d = NULL;
	}
	fclose(f);
	return d;
}

bool chr_load(Character* c, const char* dir, const char* name, char* err, size_t errlen)
{
	memset(c, 0, sizeof *c);
	char path[256];
	snprintf(path, sizeof path, "%s/%s.s3c", dir, name);
	long size;
	u8* d = read_file(path, &size);
	if (!d) {
		snprintf(err, errlen, "cannot read %s", path);
		return false;
	}
	S3CHeader* h = (S3CHeader*)d;
	if (memcmp(h->magic, "S3C1", 4) || h->version != 2 || h->bones > CHR_MAX_BONES || h->textures > CHR_MAX_TEX) {
		free(d);
		snprintf(err, errlen, "%s: unsupported S3C", path);
		return false;
	}
	c->n_bones = h->bones;
	c->n_draws = h->draws;
	c->n_vertices = h->vertices;
	c->n_textures = h->textures;
	memcpy(c->bbox_min, h->bbox_min, sizeof c->bbox_min);
	memcpy(c->bbox_max, h->bbox_max, sizeof c->bbox_max);
	memcpy(c->parents, d + h->ofs_parents, h->bones);
	memcpy(c->inv_bind, d + h->ofs_inv_bind, h->bones * sizeof(Mtx34));
	memcpy(c->bind_local, d + h->ofs_bind_local, h->bones * sizeof(Mtx34));

	c->vertices = linearAlloc(h->vertices * CHR_VERTEX_SIZE);
	c->draws = calloc(h->draws ? h->draws : 1, sizeof(ChrDraw));
	if (!c->vertices || !c->draws) {
		free(d);
		chr_free(c);
		snprintf(err, errlen, "out of memory");
		return false;
	}
	memcpy(c->vertices, d + h->ofs_vertices, h->vertices * CHR_VERTEX_SIZE);
	const u16* idx = (const u16*)(d + h->ofs_indices);
	const u8* dr = d + h->ofs_draws;
	for (u32 i = 0; i < h->draws; i++, dr += 48) {
		ChrDraw* cd = &c->draws[i];
		memcpy(cd, dr, 20 + CHR_PALETTE_MAX);
		cd->indices = linearAlloc(cd->index_count * sizeof(u16));
		if (!cd->indices) {
			free(d);
			chr_free(c);
			snprintf(err, errlen, "out of memory (indices)");
			return false;
		}
		memcpy(cd->indices, idx + cd->first_index, cd->index_count * sizeof(u16));
	}
	const u32* tex_ids = (const u32*)(d + h->ofs_textures);
	for (u32 i = 0; i < h->textures; i++) {
		snprintf(path, sizeof path, "%s/tex/%08lx.t3x", dir, (unsigned long)tex_ids[i]);
		long tsize = 0;
		void* blob = stream_take_file(path, &tsize);
		Tex3DS_Texture t = NULL;
		if (blob) {
			t = Tex3DS_TextureImport(blob, tsize, &c->tex[i], NULL, false);
			free(blob);
		} else {
			FILE* f = fopen(path, "rb");
			if (!f)
				continue;
			t = Tex3DS_TextureImportStdio(f, &c->tex[i], NULL, false);
			fclose(f);
		}
		if (!t)
			continue;
		Tex3DS_TextureFree(t);
		C3D_TexSetFilter(&c->tex[i], GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetFilterMipmap(&c->tex[i], GPU_LINEAR);
		C3D_TexSetWrap(&c->tex[i], GPU_REPEAT, GPU_REPEAT);
		c->tex_ok[i] = true;
	}
	free(d);
	snprintf(path, sizeof path, "%s/%s.kg1", dir, name);   // shadow model, optional (shadow.c)
	long kg1_size = 0;
	c->kg1 = read_file(path, &kg1_size);
	c->kg1_size = c->kg1 ? (u32)kg1_size : 0;
	for (u32 b = 0; b < c->n_bones; b++)   // bind pose until the first chr_pose
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 4; k++)
				c->skin[b].m[r][k] = (r == k) ? 1.0f : 0.0f;
	return true;
}

void chr_free(Character* c)
{
	if (c->draws)
		for (u32 i = 0; i < c->n_draws; i++)
			if (c->draws[i].indices)
				linearFree(c->draws[i].indices);
	free(c->draws);
	if (c->vertices)
		linearFree(c->vertices);
	for (u32 i = 0; i < CHR_MAX_TEX; i++)
		if (c->tex_ok[i])
			C3D_TexDelete(&c->tex[i]);
	free(c->kg1);
	memset(c, 0, sizeof *c);
}

bool anim_load(Anim* a, const char* path)
{
	memset(a, 0, sizeof *a);
	long size;
	u8* d = read_file(path, &size);
	if (!d)
		return false;
	if (memcmp(d, "S3A1", 4) || size < 16) {
		free(d);
		return false;
	}
	u32 bones = ((u32*)d)[2], frames = ((u32*)d)[3];
	if ((long)(16 + (u64)bones * frames * sizeof(AnmKey)) > size) {
		free(d);
		return false;
	}
	a->bones = bones;
	a->frames = frames;
	a->keys = malloc((size_t)bones * frames * sizeof(AnmKey));
	if (a->keys)
		memcpy(a->keys, d + 16, (size_t)bones * frames * sizeof(AnmKey));
	free(d);
	return a->keys != NULL;
}

void anim_free(Anim* a)
{
	free(a->keys);
	memset(a, 0, sizeof *a);
}

int clips_load(AnimClip* clips, int max, const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f)
		return 0;
	int n = 0, loop;
	unsigned s, e;
	char name[16];
	while (n < max && fscanf(f, "%15s %u %u %d", name, &s, &e, &loop) == 4) {
		strcpy(clips[n].name, name);
		clips[n].start = s;
		clips[n].end = e;
		clips[n].loop = loop != 0;
		n++;
	}
	fclose(f);
	return n;
}

// R = Rz * Ry * Rx (Euler order ZYX), angles in 4.12 fixed-point radians
static void euler_zyx(float out[3][3], const s16 r[3])
{
	float x = r[0] / 4096.0f, y = r[1] / 4096.0f, z = r[2] / 4096.0f;
	float cx = cosf(x), sx = sinf(x), cy = cosf(y), sy = sinf(y), cz = cosf(z), sz = sinf(z);
	out[0][0] = cz * cy; out[0][1] = cz * sy * sx - sz * cx; out[0][2] = cz * sy * cx + sz * sx;
	out[1][0] = sz * cy; out[1][1] = sz * sy * sx + cz * cx; out[1][2] = sz * sy * cx - cz * sx;
	out[2][0] = -sy;     out[2][1] = cy * sx;                out[2][2] = cy * cx;
}

static void mul34(Mtx34* o, const Mtx34* a, const Mtx34* b)
{
	Mtx34 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
		r.m[i][3] += a->m[i][3];
	}
	*o = r;
}

static void local_of(Mtx34* L, const Character* c, const AnmKey* k, u32 b)
{
	*L = c->bind_local[b];
	if (k && (k->flags & 1)) {
		float R[3][3];
		euler_zyx(R, k->rot);
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				L->m[i][j] = R[i][j];
	}
	if (k && (k->flags & 2))
		for (int i = 0; i < 3; i++)
			L->m[i][3] = k->trans[i];
}

void chr_pose(Character* c, const Anim* a, float frame)
{
	static Mtx34 world[CHR_MAX_BONES];
	if (!a || !a->keys || a->bones != c->n_bones) {
		for (u32 b = 0; b < c->n_bones; b++) {
			Mtx34 L = c->bind_local[b];
			if (c->parents[b] == 255) world[b] = L;
			else mul34(&world[b], &world[c->parents[b]], &L);
			mul34(&c->skin[b], &world[b], &c->inv_bind[b]);
		}
		return;
	}
	if (frame < 0) frame = 0;
	u32 f0 = (u32)frame;
	if (f0 >= a->frames) f0 = a->frames - 1;
	u32 f1 = f0 + 1 < a->frames ? f0 + 1 : f0;
	float t = frame - (float)f0;
	for (u32 b = 0; b < c->n_bones; b++) {
		Mtx34 L0, L1, L;
		local_of(&L0, c, &a->keys[f0 * a->bones + b], b);
		if (t > 0.001f && f1 != f0) {
			// blend matrices then re-orthonormalise the rotation (cheap and adequate for 30 fps keys)
			local_of(&L1, c, &a->keys[f1 * a->bones + b], b);
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 4; j++)
					L.m[i][j] = L0.m[i][j] + (L1.m[i][j] - L0.m[i][j]) * t;
			for (int j = 0; j < 3; j++) {
				float n = sqrtf(L.m[0][j] * L.m[0][j] + L.m[1][j] * L.m[1][j] + L.m[2][j] * L.m[2][j]);
				if (n > 1e-6f)
					for (int i = 0; i < 3; i++)
						L.m[i][j] /= n;
			}
		} else {
			L = L0;
		}
		if (c->parents[b] == 255) world[b] = L;
		else mul34(&world[b], &world[c->parents[b]], &L);
		mul34(&c->skin[b], &world[b], &c->inv_bind[b]);
	}
}

void chr_draw(Character* c, int uBones)
{
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, c->vertices, CHR_VERTEX_SIZE, 5, 0x43210);
	for (u32 i = 0; i < c->n_draws; i++) {
		ChrDraw* d = &c->draws[i];
		C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, uBones, d->palette_count * 3);
		for (u32 s = 0; s < d->palette_count; s++) {
			const Mtx34* m = &c->skin[d->palette[s]];
			for (int r = 0; r < 3; r++)
				u[s * 3 + r] = FVec4_New(m->m[r][0], m->m[r][1], m->m[r][2], m->m[r][3]);
		}
		bool textured = d->texture_slot < c->n_textures && c->tex_ok[d->texture_slot];
		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		if (textured) {
			C3D_TexBind(0, &c->tex[d->texture_slot]);
			C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_FRAGMENT_PRIMARY_COLOR, 0);
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
			C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
			C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
		} else {
			C3D_TexEnvSrc(env, C3D_RGB, GPU_FRAGMENT_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		}
		C3D_TexEnvInit(C3D_GetTexEnv(1));
		C3D_AlphaTest(textured && (d->flags & 1), GPU_GREATER, 0x60);
		C3D_DrawElements(GPU_TRIANGLES, d->index_count, C3D_UNSIGNED_SHORT, d->indices);
	}
}
