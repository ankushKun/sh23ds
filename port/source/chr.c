#include "chr.h"
#include "sdio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tex3ds.h>
#include "stream.h"
#include "sdsim.h"

#define CHR_LIGHT_SCALE 0.5f

static float light_clamp(float v, float max) { return v < 0.0f ? 0.0f : v > max ? max : v; }

void chr_light_color(C3D_Light* l, float r, float g, float b, float mat_diffuse)
{
	float max = mat_diffuse > 0.0f ? 1.0f / mat_diffuse : 1.0f;   // the register holds material x light
	C3D_LightDiffuse(l, light_clamp(r * CHR_LIGHT_SCALE, max), light_clamp(g * CHR_LIGHT_SCALE, max),
	                 light_clamp(b * CHR_LIGHT_SCALE, max));
	C3D_LightSpecular0(l, 0.0f, 0.0f, 0.0f);     // the characters' combiner uses no specular
	C3D_LightSpecular1(l, 0.0f, 0.0f, 0.0f);
}

void chr_light_ambient(C3D_LightEnv* env, float r, float g, float b)
{
	C3D_LightEnvAmbient(env, r * CHR_LIGHT_SCALE, g * CHR_LIGHT_SCALE, b * CHR_LIGHT_SCALE);
}

typedef struct {
	char magic[4];
	u32 version, bones, draws, vertices, indices, textures;
	float bbox_min[3], bbox_max[3];
	u32 ofs_parents, ofs_inv_bind, ofs_bind_local, ofs_draws, ofs_vertices, ofs_indices, ofs_textures;
} S3CHeader;

static void* read_file(const char* path, long* size)
{
	bool asked = stream_file_state(path) >= 0;
	void* pre = stream_take_file(path, size);   // read ahead for a door (main.c warp_prefetch)
	if (pre || asked)
		return pre;                  // asked for and still NULL: the reader found no such file
	sdsim_open();
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* d = malloc(*size);
	if (d && sd_fread(d, *size, f) != (size_t)*size) {
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
			sdsim_open();
			t = Tex3DS_TextureImportStdio(f, &c->tex[i], NULL, false);
			sdsim_read((u32)ftell(f));
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

// The local (parent-relative) transforms of every bone at a fractional frame; frames are blended
// linearly. False when the animation does not fit the model (the bind pose is used then).
static bool pose_locals(const Character* c, const Anim* a, float frame, Mtx34* out)
{
	if (!a || !a->keys || a->bones != c->n_bones) {
		for (u32 b = 0; b < c->n_bones; b++)
			out[b] = c->bind_local[b];
		return false;
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
		out[b] = L;
	}
	return true;
}

// world = R(about the bone's joint) * world, so the bone and everything below it turn
static void turn_about_joint(Mtx34* w, float yaw, float pitch)
{
	float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
	// R = Ry(yaw) * Rx(pitch); Rx(p) tips +z towards -y (up, Y points down), Ry turns +z towards +x
	float R[3][3] = {
		{ cy, sy * sp, sy * cp },
		{ 0.0f, cp, -sp },
		{ -sy, cy * sp, cy * cp },
	};
	Mtx34 o = *w;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			o.m[i][j] = R[i][0] * w->m[0][j] + R[i][1] * w->m[1][j] + R[i][2] * w->m[2][j];
	*w = o;      // the joint (column 3) stays where it is
}

static void pose_finish_look(Character* c, const Mtx34* local, const ChrBlend* look)
{
	static Mtx34 world[CHR_MAX_BONES];
	for (u32 b = 0; b < c->n_bones; b++) {
		if (c->parents[b] == 255) world[b] = local[b];
		else mul34(&world[b], &world[c->parents[b]], &local[b]);
		if (look && (look->look_yaw != 0.0f || look->look_pitch != 0.0f)) {
			if ((int)b == look->look_neck)
				turn_about_joint(&world[b], look->look_yaw * look->neck_share, look->look_pitch * look->neck_share);
			else if ((int)b == look->look_head)
				turn_about_joint(&world[b], look->look_yaw * (1.0f - look->neck_share),
				                 look->look_pitch * (1.0f - look->neck_share));
		}
		mul34(&c->skin[b], &world[b], &c->inv_bind[b]);
	}
}

static void pose_finish(Character* c, const Mtx34* local) { pose_finish_look(c, local, NULL); }

void chr_pose(Character* c, const Anim* a, float frame)
{
	static Mtx34 local[CHR_MAX_BONES];
	pose_locals(c, a, frame, local);
	pose_finish(c, local);
}

// a + (b - a) * w for a rotation and translation, then Gram-Schmidt so the result stays a rotation
// even when the two are far apart (lerping matrices alone would shear and shrink the bone).
static void mix34(Mtx34* o, const Mtx34* a, const Mtx34* b, float w)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			o->m[i][j] = a->m[i][j] + (b->m[i][j] - a->m[i][j]) * w;
	float x[3] = { o->m[0][0], o->m[1][0], o->m[2][0] }, y[3] = { o->m[0][1], o->m[1][1], o->m[2][1] };
	float n = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
	if (n < 1e-6f) {
		*o = w < 0.5f ? *a : *b;
		return;
	}
	for (int i = 0; i < 3; i++)
		x[i] /= n;
	float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
	for (int i = 0; i < 3; i++)
		y[i] -= d * x[i];
	n = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
	if (n < 1e-6f) {
		*o = w < 0.5f ? *a : *b;
		return;
	}
	for (int i = 0; i < 3; i++)
		y[i] /= n;
	float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
	for (int i = 0; i < 3; i++)
		o->m[i][0] = x[i], o->m[i][1] = y[i], o->m[i][2] = z[i];
}

void chr_blend_start(ChrBlend* b, float seconds)
{
	b->sync = NULL;
	if (!b->have_last || seconds <= 0.0f) {
		b->t = b->dur = 0.0f;
		return;
	}
	// a blend that starts while another runs leaves from the mixed pose shown last
	memcpy(b->from, b->last, sizeof b->from);
	b->t = 0.0f;
	b->dur = seconds;
}

void chr_blend_start_sync(ChrBlend* b, const Anim* a, float from_start, float to_start, float len, float seconds)
{
	chr_blend_start(b, seconds);
	if (b->dur > 0.0f && len > 0.0f) {
		b->sync = a;
		b->sync_from = from_start;
		b->sync_to = to_start;
		b->sync_len = len;
	}
}

void chr_pose_blend(Character* c, const Anim* a, float frame, ChrBlend* b, float dt)
{
	static Mtx34 local[CHR_MAX_BONES];
	pose_locals(c, a, frame, local);
	if (b->t < b->dur) {
		b->t += dt;
		float w = b->t >= b->dur ? 1.0f : b->t / b->dur;
		if (b->sync) {
			float rel = fmodf(frame - b->sync_to, b->sync_len);
			if (rel < 0.0f)
				rel += b->sync_len;
			pose_locals(c, b->sync, b->sync_from + rel, b->from);
		}
		for (u32 i = 0; i < c->n_bones; i++)
			mix34(&local[i], &b->from[i], &local[i], w);
	}
	memcpy(b->last, local, sizeof(Mtx34) * c->n_bones);
	b->have_last = true;
	pose_finish_look(c, local, b);
}

void chr_draw(Character* c, int uBones)
{
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, c->vertices, CHR_VERTEX_SIZE, 5, 0x43210);
	// Consecutive draws often use the same palette, texture and texenv: only what changes is sent
	// (each palette slot is three uniform vectors, and citro3d re-sends whatever is written).
	const ChrDraw* prev = NULL;
	int env_mode = -1, tex_slot = -1, alpha = -1;
	for (u32 i = 0; i < c->n_draws; i++) {
		ChrDraw* d = &c->draws[i];
		bool same_palette = prev && prev->palette_count >= d->palette_count &&
		                    !memcmp(prev->palette, d->palette, d->palette_count);
		if (!same_palette) {
			C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, uBones, d->palette_count * 3);
			for (u32 s = 0; s < d->palette_count; s++) {
				const Mtx34* m = &c->skin[d->palette[s]];
				for (int r = 0; r < 3; r++)
					u[s * 3 + r] = FVec4_New(m->m[r][0], m->m[r][1], m->m[r][2], m->m[r][3]);
			}
			prev = d;
		}
		bool textured = d->texture_slot < c->n_textures && c->tex_ok[d->texture_slot];
		if (textured && (int)d->texture_slot != tex_slot) {
			C3D_TexBind(0, &c->tex[d->texture_slot]);
			tex_slot = (int)d->texture_slot;
		}
		if ((int)textured != env_mode) {
			// stage 0 doubles the half-strength lighting (chr_light_color) and clamps it to 1, as
			// the fragment lighting's own sum would be; stage 1 puts the texture on it
			C3D_TexEnv* env = C3D_GetTexEnv(0);
			C3D_TexEnvInit(env);
			C3D_TexEnvSrc(env, C3D_RGB, GPU_FRAGMENT_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvSrc(env, C3D_Alpha, textured ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
			C3D_TexEnv* e1 = C3D_GetTexEnv(1);
			C3D_TexEnvInit(e1);
			if (textured) {
				C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, 0);
				C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
				C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
				C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
			}
			env_mode = (int)textured;
		}
		int a = textured && (d->flags & 1);
		if (a != alpha) {
			C3D_AlphaTest(a, GPU_GREATER, 0x60);
			alpha = a;
		}
		C3D_DrawElements(GPU_TRIANGLES, d->index_count, C3D_UNSIGNED_SHORT, d->indices);
	}
	if (env_mode >= 0) {     // leave the combiner as the old one-stage setup did: stage 1 passes through
		C3D_TexEnvScale(C3D_GetTexEnv(0), C3D_RGB, GPU_TEVSCALE_1);
		C3D_TexEnvInit(C3D_GetTexEnv(1));
	}
}
