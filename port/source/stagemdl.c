#include "stagemdl.h"
#include "chr.h"
#include "chrlight.h"
#include "events.h"
#include <math.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"

// The music box's place in stage 40 (mdl_list entry 0, kind 0x524, no flags, not turned).
static const float s_box_pos[3] = { -20000.0f, 0.0f, 24000.0f };

static Character s_box;
static bool s_box_ok;
static int s_stage = -1;
static float s_play = -1.0f;      // seconds into the tune, < 0 when it is not playing

void stagemdl_update(int stage)
{
	if (stage == s_stage)
		return;
	s_stage = stage;
	if (s_box_ok) {
		chr_free(&s_box);
		s_box_ok = false;
		s_play = -1.0f;
	}
	if (stage == 40) {
		char err[64];
		s_box_ok = chr_load(&s_box, PACK_DIR "/chr/item", "org", err, sizeof err);
	}
}

void stagemdl_music_box(float seconds) { s_play = seconds; }

static void mul34(Mtx34* o, const Mtx34* a, const Mtx34* b)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++) {
			float v = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
			o->m[i][j] = v + (j == 3 ? a->m[i][3] : 0.0f);
		}
}

// 0x57b5c0 with T = 15 seconds, as a function of the time t: bones 0, 6 and 7 turn by -pi/2 about
// y over the first tenth and back over the last (bone 8 the other way), bone 2 turns by 11.78 / T
// rad/s in between and ends at pi (that is where 3 pi lands); everything is 0 again at T.
static float box_angle(int bone, float t)
{
	const float T = 15.0f, k = 15.707964f / T, k2 = 11.780972f / T;
	if (t < 0.0f || t >= T)
		return 0.0f;
	float lid = -k * fminf(t, 0.1f * T) + k * fmaxf(0.0f, t - 0.9f * T);
	switch (bone) {
	case 0: case 6: case 7: return lid;
	case 8: return -lid;
	case 2: return t <= 0.1f * T ? 0.0f : t >= 0.9f * T ? (float)M_PI : k2 * (t - 0.1f * T);
	default: return 0.0f;
	}
}

// The bone's rotation as the PC keeps it, x, y and z applied as z * y * x (chr.c's order); the
// PC reads it, sets y and writes it back (0x538b80 / 0x538c30). Here y gets the angle on top of the
// bind pose's y, which is 0 for the doors and keeps bone 2's own turn.
static void turn_y(Mtx34* L, float angle)
{
	float (*R)[4] = L->m;
	float x, y, z;
	if (fabsf(R[2][0]) < 0.9999f) {
		y = -asinf(R[2][0]);
		x = atan2f(R[2][1], R[2][2]);
		z = atan2f(R[1][0], R[0][0]);
	} else {
		y = R[2][0] < 0.0f ? (float)M_PI / 2.0f : -(float)M_PI / 2.0f;
		z = 0.0f;
		x = R[2][0] < 0.0f ? atan2f(R[0][1], R[0][2]) : atan2f(-R[0][1], -R[0][2]);
	}
	y += angle;
	float cx = cosf(x), sx = sinf(x), cy = cosf(y), sy = sinf(y), cz = cosf(z), sz = sinf(z);
	R[0][0] = cz * cy; R[0][1] = cz * sy * sx - sz * cx; R[0][2] = cz * sy * cx + sz * sx;
	R[1][0] = sz * cy; R[1][1] = sz * sy * sx + cz * cx; R[1][2] = sz * sy * cx - cz * sx;
	R[2][0] = -sy;     R[2][1] = cy * sx;                R[2][2] = cy * cx;
}

static void pose_box(void)
{
	static Mtx34 world[CHR_MAX_BONES];
	for (u32 b = 0; b < s_box.n_bones; b++) {
		Mtx34 L = s_box.bind_local[b];
		float a = box_angle((int)b, s_play);
		if (a != 0.0f)
			turn_y(&L, a);
		if (s_box.parents[b] == 255)
			world[b] = L;
		else
			mul34(&world[b], &world[s_box.parents[b]], &L);
		mul34(&s_box.skin[b], &world[b], &s_box.inv_bind[b]);
	}
}

void stagemdl_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	stagemdl_update(ev_stage());
	if (!s_box_ok)
		return;
	C3D_Mtx model, mv;
	Mtx_Identity(&model);
	Mtx_Translate(&model, s_box_pos[0], s_box_pos[1], s_box_pos[2], true);
	Mtx_Multiply(&mv, view, &model);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
	pose_box();
	chrlight_at(s_box_pos, false);
	chr_draw(&s_box, uBones);
}
