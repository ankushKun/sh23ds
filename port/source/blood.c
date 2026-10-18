#include "blood.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <tex3ds.h>

#define MAX_SPLASH 8
#define MAX_PART 96
#define MAX_STAIN 64
#define MAX_POOL 24
#define MAX_VERTS ((MAX_PART + MAX_STAIN + 16) * 6 + MAX_POOL * 24)

typedef struct { float x, y, z, u, v; u8 c[4]; s8 n[4]; } V;   // the world program's vertex

typedef struct {
	bool on;
	float pos[3], dir[3];        // the element's two vectors (hit point, spray direction)
	const float* owner;          // followed for the splash's half second
	float last[3];
	float floor_y;
	float t, next[2];
	int posted[2];
} Splash;

typedef struct {
	bool on;
	int mode;                    // 0 spray (fades out), 1 drop (falls, leaves a stain), 2 spurt (class 14:
	                             // falls and is gone, the floors that hold no blood)
	int kind;                    // one of the eight sprite placements (rand & 7)
	float p0[3], vel[3], t;
	float floor_y;
} Part;

typedef struct {
	bool on;
	float x, y, z, rot, t, sx, sz;
	float alpha;                 // 1 until the 30 s are up, then fades
} Stain;

typedef struct {
	bool on;
	const float* owner;
	float off[2];                // from the body (x, z)
	float x, y, z, rot, t;
	int mesh, size;              // rand & 3, rand % 5
} Pool;

typedef struct {
	bool on;
	const float* owner;
	float t;
	int row, posted;
} PoolSet;

static BloodBindFn s_bind;
static BloodFloorFn s_floor;
static C3D_Tex s_sprite, s_blob;
static bool s_sprite_ok, s_blob_ok;
static V* s_verts[2];
static int s_buf, s_drawn;
static int s_color;
static u32 s_seed = 0x1234567;
static Splash s_splash[MAX_SPLASH];
static Part s_part[MAX_PART];
static Stain s_stain[MAX_STAIN];
static Pool s_pool[MAX_POOL];
static PoolSet s_set[MAX_POOL / 3];

typedef struct {
	bool on;
	float x, y, z, rot, t, alpha;
	int foot;                    // 0 left, 1 right print
} Print;
#define MAX_PRINT 16
static Print s_print[MAX_PRINT];
static int s_prints, s_foot;     // footprints still to leave (class 8's count, 5), the next foot

// ---------------------------------------------------------------- the PC's tables

// 0x8959f0: the sprites' colours subtracted from the picture, per Blood Color
static const u8 PART_COL[4][3] = { { 40, 255, 230 }, { 180, 4, 150 }, { 24, 230, 23 }, { 194, 210, 180 } };
// 0x8955a0 / 0x8955e0 (and the stains' 0x895c80 / 0x895cc0): start and end colours
static const u8 DECAL_A[4][3] = { { 64, 180, 150 }, { 180, 4, 150 }, { 24, 180, 23 }, { 194, 210, 180 } };
static const u8 DECAL_B[4][3] = { { 96, 255, 230 }, { 255, 20, 230 }, { 24, 230, 23 }, { 226, 255, 230 } };
// 0x8957a8..: the sprite's four corners (x, y of the +-25 quad at 0x895728) and their texture
// coordinates in bloodtex0 (a 256x256 quarter: the spray clusters top left or top right)
static const float CORNER[4][2] = { { -25, -25 }, { -25, 25 }, { 25, -25 }, { 25, 25 } };
static const float SPRITE_UV[8][4][2] = {
	{ { 0, 0 }, { 0, .5f }, { .5f, 0 }, { .5f, .5f } },     { { 0, 0 }, { .5f, 0 }, { 0, .5f }, { .5f, .5f } },
	{ { .5f, .5f }, { .5f, 0 }, { 0, .5f }, { 0, 0 } },     { { .5f, .5f }, { 0, .5f }, { .5f, 0 }, { 0, 0 } },
	{ { .5f, 0 }, { .5f, .5f }, { 1, 0 }, { 1, .5f } },     { { .5f, 0 }, { 1, 0 }, { .5f, .5f }, { 1, .5f } },
	{ { 1, .5f }, { 1, 0 }, { .5f, .5f }, { .5f, 0 } },     { { 1, .5f }, { .5f, .5f }, { 1, 0 }, { .5f, 0 } },
};
// 0x895a74 / 0x895a7c (size, times the quad), 0x8959dc / 0x8959e4 (alpha, 128 = full), curve
// 0x895a84 / 0x895aa8 (the time to the end values), per mode
static const float PART_SIZE[3][2] = { { 0.8f, 7.0f }, { 0.8f, 2.0f }, { 0.8f, 2.0f } };
static const float PART_ALPHA[3][2] = { { 80.0f, 0.0f }, { 128.0f, 128.0f }, { 128.0f, 128.0f } };
static const float PART_TIME[3] = { 0.375f, 0.6f, 0.6f };
// 0x4c7d40: the spray's speed per attack id; melee hits throw it back towards the attacker
static float attack_speed(int id, bool* melee)
{
	*melee = false;
	switch (id) {
	case 1: case 2: return 700.0f;
	case 4: return 2000.0f;
	case 6: case 0x3a: return 3000.0f;
	case 0x1f: case 0x20: case 0x21: case 0x22: case 0x23: return 600.0f;
	case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: return 800.0f;
	}
	*melee = true;
	switch (id) {
	case 0xc: case 0xd: case 0xe: return 500.0f;
	case 0xf: case 0x10: case 0x11: case 0x12: return 800.0f;
	case 0x13: case 0x14: case 0x15: case 0x16: case 0x18: return 2000.0f;
	case 0x17: return 1500.0f;
	}
	*melee = false;
	return 500.0f;
}
// 0x8a133c: when the three pools start, five rows to pick from (seconds)
static const float POOL_ROW[5][3] = { { 0, 1, 2 }, { 0, 2, 4 }, { 1, 2, 4 }, { 2, 3, 4 }, { 1.6f, 3, 5 } };
// 0x895678: the pools' full size (x, z) per size variant; keys 0x8956c8 (time, size, colour)
static const float POOL_SIZE[5][2] = { { 400, 400 }, { 350, 400 }, { 400, 350 }, { 300, 400 }, { 400, 300 } };
// Enhanced Edition IncreaseBlood (Patches/BloodSize.cpp, zlib licence) sets the third key's size
// (0x8956e4) from 0.8 to 1: a dead enemy's pools grow back to their original size
static const float POOL_KEY[4][3] = { { 0, 0, 0 }, { 2, 0.3f, 0 }, { 8, 1.0f, 0.35f }, { 16, 1, 1 } };
// 0x895e08 and keys 0x895e88 for the stains (their start colour fraction is 0, size 0.7)
static const float STAIN_SIZE[5][2] = { { 40, 40 }, { 30, 35 }, { 35, 30 }, { 35, 32 }, { 30, 30 } };
static const float STAIN_KEY[4][3] = { { 0, 0.7f, 0 }, { 1, 0.85f, 0 }, { 3, 0.95f, 0.35f }, { 7, 1, 1 } };
// 0x8951d0 / 0x895310 / 0x8953b0: the fan-shaped pools (centre, then nine rim points); 0x895150 is a
// plain square. Texture coordinates of the fans: the blob's circle (0x895270).
static const float FAN[3][10][2] = {
	{ { 0, 0 }, { 1, 0 }, { .707f, .707f }, { 0, 1 }, { -.707f, .707f }, { -1, 0 }, { -.707f, -.707f }, { 0, -1 }, { .707f, -.707f }, { 1, 0 } },
	{ { 0, 0 }, { .8f, 0 }, { 1.008f, 1.008f }, { 0, 1.675f }, { -1.096f, 1.096f }, { -.925f, 0 }, { -.919f, -.919f }, { 0, -1.3f }, { .566f, -.566f }, { .8f, 0 } },
	{ { 0, 0 }, { .4f, 0 }, { .725f, .725f }, { 0, 1.275f }, { -.813f, .813f }, { -.525f, 0 }, { -.636f, -.636f }, { 0, -.9f }, { .283f, -.283f }, { .4f, 0 } },
};
static const float FAN_UV[10][2] = {
	{ .5f, .5f }, { 1, .5f }, { .854f, .854f }, { .5f, 1 }, { .146f, .854f }, { 0, .5f }, { .146f, .146f }, { .5f, 0 }, { .854f, .146f }, { 1, .5f },
};
#define GRAVITY (4900.0f * 1.0989f)   // 0x4ce740: offset = v t + 2 * 2692.31 * t^2 (Y down)

static u32 rnd(void)
{
	s_seed = s_seed * 1103515245u + 12345u;
	return s_seed >> 8;
}
static float frand(void) { return (rnd() & 0xffff) / 65535.0f; }

// the floor under (x, z), as the PC lays its pools on the ground (0x4cb510)
static float floor_at(float x, float z, float y)
{
	float out;
	return s_floor && s_floor(x, z, y, &out) ? out : y;
}

// keyframes as the PC's 0x4cdc10: cumulative (time, size, colour)
static void keys(const float (*k)[3], float t, float* size, float* col)
{
	if (t >= k[3][0]) {
		*size = k[3][1], *col = k[3][2];
		return;
	}
	int i = 0;
	while (i < 2 && t >= k[i + 1][0])
		i++;
	float f = (t - k[i][0]) / (k[i + 1][0] - k[i][0]);
	*size = k[i][1] + (k[i + 1][1] - k[i][1]) * f;
	*col = k[i][2] + (k[i + 1][2] - k[i][2]) * f;
}

// ---------------------------------------------------------------- setup

// 0x507930: 64x64, value 255 * (1 - u^2 - v^2)^4 over u, v in -1..1, in every channel
static bool make_blob(void)
{
	if (!C3D_TexInit(&s_blob, 64, 64, GPU_A8))
		return false;
	static const u8 m[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
	u8* d = (u8*)s_blob.data;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			float u = x / 32.0f - 1.0f, v = y / 32.0f - 1.0f, a = 1.0f - (u * u + v * v);
			a = a < 0.0f ? 0.0f : a;
			a = a * a * a * a;
			d[((y >> 3) * 8 + (x >> 3)) * 64 + (m[x & 7] | m[y & 7] << 1)] = (u8)(a * 255.0f);
		}
	C3D_TexFlush(&s_blob);
	C3D_TexSetFilter(&s_blob, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_blob, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return true;
}

bool blood_init(const char* pack_dir, BloodBindFn bind, BloodFloorFn floor)
{
	s_bind = bind;
	s_floor = floor;
	char path[128];
	snprintf(path, sizeof path, "%s/effect/blood.t3x", pack_dir);
	FILE* f = fopen(path, "rb");
	if (f) {
		Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_sprite, NULL, false);
		fclose(f);
		if (t) {
			Tex3DS_TextureFree(t);
			C3D_TexSetFilter(&s_sprite, GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&s_sprite, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			s_sprite_ok = true;
		}
	}
	s_blob_ok = make_blob();
	for (int i = 0; i < 2; i++)
		s_verts[i] = linearAlloc(MAX_VERTS * sizeof(V));
	return s_sprite_ok && s_blob_ok && s_verts[0] && s_verts[1];
}

void blood_exit(void)
{
	if (s_sprite_ok)
		C3D_TexDelete(&s_sprite);
	if (s_blob_ok)
		C3D_TexDelete(&s_blob);
	for (int i = 0; i < 2; i++)
		linearFree(s_verts[i]);
	s_sprite_ok = s_blob_ok = false;
}

void blood_clear(void)
{
	memset(s_splash, 0, sizeof s_splash);
	memset(s_part, 0, sizeof s_part);
	memset(s_stain, 0, sizeof s_stain);
	memset(s_pool, 0, sizeof s_pool);
	memset(s_set, 0, sizeof s_set);
	memset(s_print, 0, sizeof s_print);
	s_prints = 0;
}

void blood_set_color(int color) { s_color = color & 3; }

// ---------------------------------------------------------------- posting

void blood_hit(const float pos[3], const float dir[3], int attack, const float* owner, float floor_y)
{
	int i = 0;
	while (i < MAX_SPLASH && s_splash[i].on)
		i++;
	if (i == MAX_SPLASH)
		return;
	Splash* s = &s_splash[i];
	memset(s, 0, sizeof *s);
	// 0x4c7d40: the hit direction normalised, times the attack's speed; melee throws it back, and
	// it always leaves upwards
	bool melee;
	float k = attack_speed(attack, &melee);
	float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	n = n > 1e-6f ? 1.0f / n : 0.0f;
	s->dir[0] = dir[0] * n * k * (melee ? -1.0f : 1.0f);
	s->dir[1] = dir[1] * n * k;
	s->dir[2] = dir[2] * n * k * (melee ? -1.0f : 1.0f);
	if (s->dir[1] > 0.0f)
		s->dir[1] = -s->dir[1];
	memcpy(s->pos, pos, sizeof s->pos);
	s->owner = owner;
	if (owner)
		memcpy(s->last, owner, sizeof s->last);
	s->floor_y = floor_y;
	s->on = true;
}

static void post_part(const Splash* s, int ch)
{
	int i = 0;
	while (i < MAX_PART && s_part[i].on)
		i++;
	if (i == MAX_PART)
		return;
	Part* p = &s_part[i];
	// 0x4ea6b0 turns the direction by a random amount and moves the start a little; channel 1
	// leaves straight out (0, -500, 0)
	float d[3] = { s->dir[0], s->dir[1], s->dir[2] };
	if (ch == 1)
		d[0] = 0.0f, d[1] = -500.0f, d[2] = 0.0f;
	float yaw = (frand() - 0.5f) * 0.785f, c = cosf(yaw), sn = sinf(yaw), sp = 0.6f + 0.4f * frand();
	float rx = d[0] * c - d[2] * sn, rz = d[0] * sn + d[2] * c;
	// 0x4ce6f0: the speed scaled (0.549, 1.099, 0.549)
	p->vel[0] = rx * 0.549451f * sp;
	p->vel[1] = d[1] * 1.0989f * (0.8f + 0.4f * frand());
	p->vel[2] = rz * 0.549451f * sp;
	for (int k = 0; k < 3; k++)
		p->p0[k] = s->pos[k] + (frand() - 0.5f) * 20.0f;
	p->mode = ch;
	p->kind = rnd() & 7;
	p->t = 0.0f;
	p->floor_y = s->floor_y;
	p->on = true;
}

static void post_stain(float x, float y, float z)
{
	int i = 0;
	while (i < MAX_STAIN && s_stain[i].on)
		i++;
	if (i == MAX_STAIN) {
		// the oldest goes (EFCTDeleteOldBloodDropTask on the PS2)
		float best = -1.0f;
		for (int k = 0; k < MAX_STAIN; k++)
			if (s_stain[k].t > best)
				best = s_stain[k].t, i = k;
	}
	Stain* st = &s_stain[i];
	int v = rnd() % 5;
	*st = (Stain){ true, x, y, z, frand() * 6.2831853f, 0.0f, STAIN_SIZE[v][0], STAIN_SIZE[v][1], 1.0f };
}

static void spurts(const float* owner)
{
	// 0x4eafd0: six drops (class 14) at random angles, 500 strong, a little upwards; on these
	// floors they go when they land
	for (int k = 0; k < 6; k++) {
		int i = 0;
		while (i < MAX_PART && s_part[i].on)
			i++;
		if (i == MAX_PART)
			return;
		Part* p = &s_part[i];
		float a = frand() * 6.2831853f, up = (frand() * 10.0f - 5.0f) * 0.01745f;
		float sp = 500.0f * (0.6f + 0.4f * frand());
		p->vel[0] = sinf(a) * cosf(up) * sp * 0.549451f;
		p->vel[2] = cosf(a) * cosf(up) * sp * 0.549451f;
		p->vel[1] = -fabsf(sinf(up)) * sp * 1.0989f - 200.0f;
		p->p0[0] = owner[0], p->p0[1] = owner[1] - 300.0f, p->p0[2] = owner[2];
		p->floor_y = owner[1];
		p->mode = 2;
		p->kind = rnd() & 7;
		p->t = 0.0f;
		p->on = true;
	}
}

void blood_pool(const float* owner, int room)
{
	if (!owner || room == 7 || room == 0x21)
		return;
	static const u8 SPURT_ROOMS[] = { 0x62, 0x7a, 0x7c, 0x7e, 0x80, 0x82, 0x85, 0xab, 0xb6, 0xb7, 0xb8, 0xb9, 0xbb };
	for (size_t k = 0; k < sizeof SPURT_ROOMS; k++)
		if (room == SPURT_ROOMS[k]) {
			spurts(owner);
			return;
		}
	if (room == 0x63) {
		if (owner[1] < -128.0f)
			spurts(owner);
		return;
	}
	int i = 0;
	while (i < MAX_POOL / 3 && s_set[i].on)
		i++;
	if (i == MAX_POOL / 3)
		return;
	s_set[i] = (PoolSet){ true, owner, 0.0f, (int)(rnd() % 5), 0 };
}

void blood_footstep(const float foot[3], float yaw)
{
	if (s_prints <= 0) {
		// 0x4ebe70: the step lands in a pool (one of the class 12 instances, 0x4ce2c0)
		for (int i = 0; i < MAX_POOL; i++) {
			const Pool* p = &s_pool[i];
			if (!p->on)
				continue;
			float sz, cf;
			keys(POOL_KEY, p->t, &sz, &cf);
			float r = 0.6f * sz * (POOL_SIZE[p->size][0] + POOL_SIZE[p->size][1]) * 0.5f;
			float dx = foot[0] - p->x, dz = foot[2] - p->z;
			if (dx * dx + dz * dz < r * r && fabsf(foot[1] - p->y) < 200.0f) {
				s_prints = 5;            // class 8's count (5.0)
				break;
			}
		}
		return;
	}
	// 0x4e94c0: a print a step, turned along the walk, left and right in turn
	int i = 0;
	while (i < MAX_PRINT && s_print[i].on)
		i++;
	if (i == MAX_PRINT) {
		float best = -1.0f;
		for (int k = 0; k < MAX_PRINT; k++)
			if (s_print[k].t > best)
				best = s_print[k].t, i = k;
	}
	float side = s_foot ? 60.0f : -60.0f;
	float x = foot[0] + cosf(yaw) * side, z = foot[2] - sinf(yaw) * side;
	s_print[i] = (Print){ true, x, floor_at(x, z, foot[1]), z, yaw, 0.0f, s_prints / 5.0f, s_foot };
	s_foot ^= 1;
	s_prints--;
}

static void post_pool(const float* owner)
{
	int i = 0;
	while (i < MAX_POOL && s_pool[i].on)
		i++;
	if (i == MAX_POOL)
		return;
	Pool* p = &s_pool[i];
	// 0x4eb460: a random angle, 50 to 200 from the body
	float a = frand() * 6.2831853f, r = 50.0f + (float)(rnd() % 150);
	p->owner = owner;
	p->off[0] = sinf(a) * r, p->off[1] = cosf(a) * r;
	p->x = owner[0] + p->off[0], p->z = owner[2] + p->off[1];
	p->y = floor_at(p->x, p->z, owner[1]);
	p->rot = frand() * 6.2831853f;
	p->t = 0.0f;
	p->mesh = rnd() & 3;
	p->size = rnd() % 5;
	p->on = true;
}

void blood_forget(const float* owner)
{
	for (int i = 0; i < MAX_SPLASH; i++)
		if (s_splash[i].owner == owner)
			s_splash[i].owner = NULL;
	for (int i = 0; i < MAX_POOL; i++)
		if (s_pool[i].owner == owner)
			s_pool[i].owner = NULL;
	for (int i = 0; i < MAX_POOL / 3; i++)
		if (s_set[i].owner == owner)
			s_set[i].on = false;
}

// ---------------------------------------------------------------- update

void blood_update(float dt)
{
	if (dt <= 0.0f)
		return;
	static const float INTERVAL[2] = { 0.00625f, 0.0666667f };   // 0x8a1324
	static const int COUNT[2] = { 8, 6 };                        // 0x8a1314
	for (int i = 0; i < MAX_SPLASH; i++) {
		Splash* s = &s_splash[i];
		if (!s->on)
			continue;
		if (s->owner) {
			// 0x4eac00: the hit point moves with the enemy
			for (int k = 0; k < 3; k++) {
				s->pos[k] += s->owner[k] - s->last[k];
				s->last[k] = s->owner[k];
			}
		}
		s->t += dt;
		for (int ch = 0; ch < 2; ch++)
			while (s->posted[ch] < COUNT[ch] && s->next[ch] < s->t) {
				post_part(s, ch);
				s->posted[ch]++;
				s->next[ch] += INTERVAL[ch];
			}
		if (s->t > 0.5f)
			s->on = false;
	}
	for (int i = 0; i < MAX_PART; i++) {
		Part* p = &s_part[i];
		if (!p->on)
			continue;
		p->t += dt;
		if (p->mode == 0) {
			if (p->t >= PART_TIME[0])
				p->on = false;           // faded out
			continue;
		}
		float y = p->p0[1] + p->vel[1] * p->t + GRAVITY * p->t * p->t;
		if (p->mode == 2) {
			if (y >= p->floor_y && p->vel[1] + 2.0f * GRAVITY * p->t > 0.0f)
				p->on = false;           // 0x4ccd20: gone when it reaches the floor
			continue;
		}
		if (y >= p->floor_y && p->vel[1] + 2.0f * GRAVITY * p->t > 0.0f) {
			// 0x4cee80: a drop that reaches the floor leaves a stain (class 10) there
			float x = p->p0[0] + p->vel[0] * p->t, z = p->p0[2] + p->vel[2] * p->t;
			post_stain(x, floor_at(x, z, p->floor_y), z);
			p->on = false;
		} else if (p->t > 3.0f) {
			p->on = false;
		}
	}
	for (int i = 0; i < MAX_STAIN; i++) {
		Stain* st = &s_stain[i];
		if (!st->on)
			continue;
		st->t += dt;
		// 0x4cf710: thirty seconds, then the alpha runs out one step a frame
		if (st->t > 30.0f && (st->alpha -= dt * 60.0f / 128.0f) <= 0.0f)
			st->on = false;
	}
	for (int i = 0; i < MAX_PRINT; i++) {
		Print* pr = &s_print[i];
		if (!pr->on)
			continue;
		pr->t += dt;
		// 0x4cb930: ten seconds, then the alpha goes by 0.01 a tick
		if (pr->t > 10.0f && (pr->alpha -= dt * 0.6f) <= 0.0f)
			pr->on = false;
	}
	for (int i = 0; i < MAX_POOL / 3; i++) {
		PoolSet* ps = &s_set[i];
		if (!ps->on)
			continue;
		ps->t += dt;
		while (ps->posted < 3 && ps->t >= POOL_ROW[ps->row][ps->posted]) {
			post_pool(ps->owner);
			ps->posted++;
		}
		if (ps->posted == 3)
			ps->on = false;
	}
	for (int i = 0; i < MAX_POOL; i++) {
		Pool* p = &s_pool[i];
		if (!p->on)
			continue;
		p->t += dt;
		if (p->owner && (p->owner[0] + p->off[0] != p->x || p->owner[2] + p->off[1] != p->z)) {
			p->x = p->owner[0] + p->off[0];
			p->z = p->owner[2] + p->off[1];
			p->y = floor_at(p->x, p->z, p->owner[1]);
		}
	}
}

// ---------------------------------------------------------------- draw

static V* put(V* v, float x, float y, float z, float u, float t, const u8* rgb, float a)
{
	int al = (int)(a * 255.0f);
	*v = (V){ x, y, z, u, t, { rgb[0], rgb[1], rgb[2], (u8)(al < 0 ? 0 : al > 255 ? 255 : al) }, { 0, -127, 0, 0 } };
	return v + 1;
}

static V* decal(V* v, float x, float y, float z, float rot, float sx, float sz, const u8* rgb, float a)
{
	float c = cosf(rot), s = sinf(rot);
	float cx[4] = { 1, 1, -1, -1 }, cz[4] = { 1, -1, 1, -1 }, cu[4] = { 0, 0, 1, 1 }, cv[4] = { 0, 1, 0, 1 };
	V q[4];
	for (int k = 0; k < 4; k++) {
		float lx = cx[k] * sx, lz = cz[k] * sz;
		put(&q[k], x + c * lx - s * lz, y, z + s * lx + c * lz, cu[k], cv[k], rgb, a);
	}
	v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[2], v[4] = q[1], v[5] = q[3];
	return v + 6;
}

static void begin(const C3D_Mtx* proj, const C3D_Mtx* view, C3D_Tex* tex, bool decal)
{
	s_bind(proj, view);
	// the room's depth mapping; the floor decals get the offset of its decal group (main.c)
	C3D_DepthMap(true, -1.0f, decal ? 0.0002f : 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	// D3DBLENDOP_REVSUBTRACT: the picture minus the blood's colour times its alpha
	C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_TexBind(0, tex);
}

static void flush(V* first, V* end)
{
	int n = (int)(end - first);
	if (n <= 0)
		return;
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, first, sizeof(V), 4, 0x3210);
	C3D_DrawArrays(GPU_TRIANGLES, 0, n);
}

void blood_draw(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	if (!s_bind || !s_verts[0] || !s_verts[1])
		return;
	s_buf ^= 1;
	V* base = s_verts[s_buf];
	V* v = base;
	// the floor decals: stains and pools (the blob), a little above the floor
	if (s_blob_ok) {
		for (int i = 0; i < MAX_POOL; i++) {
			const Pool* p = &s_pool[i];
			if (!p->on)
				continue;
			float sz, cf;
			keys(POOL_KEY, p->t, &sz, &cf);
			u8 rgb[3];
			for (int k = 0; k < 3; k++)
				rgb[k] = (u8)(DECAL_A[s_color][k] + (DECAL_B[s_color][k] - DECAL_A[s_color][k]) * cf);
			float sx = POOL_SIZE[p->size][0] * sz, szz = POOL_SIZE[p->size][1] * sz;
			if (p->mesh == 0) {
				v = decal(v, p->x, p->y - 6.0f, p->z, p->rot, sx, szz, rgb, 1.0f);
				continue;
			}
			const float (*fan)[2] = FAN[p->mesh - 1];
			float c = cosf(p->rot), s = sinf(p->rot);
			for (int k = 1; k < 9; k++) {
				int idx[3] = { 0, k, k + 1 };
				for (int j = 0; j < 3; j++) {
					float lx = fan[idx[j]][0] * sx, lz = fan[idx[j]][1] * szz;
					v = put(v, p->x + c * lx - s * lz, p->y - 6.0f, p->z + s * lx + c * lz, FAN_UV[idx[j]][0],
					        FAN_UV[idx[j]][1], rgb, 1.0f);
				}
			}
		}
		for (int i = 0; i < MAX_STAIN; i++) {
			const Stain* st = &s_stain[i];
			if (!st->on)
				continue;
			float sz, cf;
			keys(STAIN_KEY, st->t, &sz, &cf);
			u8 rgb[3];
			for (int k = 0; k < 3; k++)
				rgb[k] = (u8)(DECAL_A[s_color][k] + (DECAL_B[s_color][k] - DECAL_A[s_color][k]) * cf);
			v = decal(v, st->x, st->y - 7.0f, st->z, st->rot, st->sx * sz, st->sz * sz, rgb, st->alpha);
		}
		if (v > base) {
			begin(proj, view, &s_blob, true);
			flush(base, v);
		}
	}
	// the footprints: bloodtex0's prints (64x64 cells at 0 / 64, 384), flat on the floor
	V* prints = v;
	if (s_sprite_ok) {
		for (int i = 0; i < MAX_PRINT; i++) {
			const Print* pr = &s_print[i];
			if (!pr->on)
				continue;
			float c = cosf(pr->rot), s = sinf(pr->rot), h = 187.0f;
			float u0 = pr->foot ? 0.125f : 0.0f, u1 = u0 + 0.125f;
			// the toe (the cell's top, v 0.75) ahead along the walk
			float lx[4] = { -h, h, -h, h }, lz[4] = { h, h, -h, -h }, cu[4] = { u0, u1, u0, u1 };
			float cv[4] = { 1.0f - 0.75f, 1.0f - 0.75f, 1.0f - 0.875f, 1.0f - 0.875f };
			V q[4];
			for (int k = 0; k < 4; k++)
				put(&q[k], pr->x + c * lx[k] + s * lz[k], pr->y - 6.0f, pr->z - s * lx[k] + c * lz[k], cu[k], cv[k],
				    DECAL_B[s_color], pr->alpha);
			v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[2], v[4] = q[1], v[5] = q[3];
			v += 6;
		}
		if (v > prints) {
			begin(proj, view, &s_sprite, true);
			flush(prints, v);
		}
	}
	// the spray and the drops: sprites facing the camera
	V* first = v;
	if (s_sprite_ok) {
		float rx = view->r[0].x, ry = view->r[0].y, rz = view->r[0].z;
		float ux = view->r[1].x, uy = view->r[1].y, uz = view->r[1].z;
		for (int i = 0; i < MAX_PART; i++) {
			const Part* p = &s_part[i];
			if (!p->on)
				continue;
			float f = p->t / PART_TIME[p->mode];
			f = f > 1.0f ? 1.0f : f;
			float size = PART_SIZE[p->mode][0] + (PART_SIZE[p->mode][1] - PART_SIZE[p->mode][0]) * f;
			float a = (PART_ALPHA[p->mode][0] + (PART_ALPHA[p->mode][1] - PART_ALPHA[p->mode][0]) * f) / 128.0f;
			float x = p->p0[0] + p->vel[0] * p->t;
			float y = p->p0[1] + p->vel[1] * p->t + GRAVITY * p->t * p->t;
			float z = p->p0[2] + p->vel[2] * p->t;
			V q[4];
			for (int k = 0; k < 4; k++) {
				float cx = CORNER[k][0] * size, cy = CORNER[k][1] * size;
				put(&q[k], x + rx * cx + ux * cy, y + ry * cx + uy * cy, z + rz * cx + uz * cy,
				    SPRITE_UV[p->kind][k][0], 1.0f - SPRITE_UV[p->kind][k][1], PART_COL[s_color], a);
			}
			v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[2], v[4] = q[1], v[5] = q[3];
			v += 6;
		}
		if (v > first) {
			begin(proj, view, &s_sprite, false);
			flush(first, v);
		}
	}
	s_drawn = (int)(v - base);
	if (v > base)
		GSPGPU_FlushDataCache(base, (u32)((u8*)v - (u8*)base));
	C3D_DepthMap(true, -1.0f, 0.0f);
}

const char* blood_status(void)
{
	static char line[56];
	int n[4] = { 0 };
	for (int i = 0; i < MAX_SPLASH; i++) n[0] += s_splash[i].on;
	for (int i = 0; i < MAX_PART; i++) n[1] += s_part[i].on;
	for (int i = 0; i < MAX_STAIN; i++) n[2] += s_stain[i].on;
	for (int i = 0; i < MAX_POOL; i++) n[3] += s_pool[i].on;
	int prints = 0;
	for (int i = 0; i < MAX_PRINT; i++) prints += s_print[i].on;
	snprintf(line, sizeof line, "blood %d %d %d %d %d verts %d%s", n[0], n[1], n[2], n[3], prints, s_drawn,
	         s_sprite_ok && s_blob_ok ? "" : " no texture");
	return line;
}
