// The rowing boat on Toluca Lake, ported from the PC's boat character (kind 0x10b, update 0x53fe40;
// PS2 beta Chacter/m3_boat.c, m3_boat_jms.c and lake_wave.c). See boat.h.
//
// What the PC does each frame while James rows (0x53fe40 with 0x20000 in his status):
//   0x53e760  the stick (or the arrow keys) as one direction: turned 45 degrees it gives the two
//             oars' pulls a and b (0.5 each straight ahead, 0.25 and -0.25 straight to a side,
//             half that without the run key). Only the middle of a stroke (motion frames 17..79)
//             pushes: forward (a + b) x 1.8 x 0.5, turn (a - b) x 1.8. Which oars row: both, the
//             left or the right, re-picked at most every 1.5 s, both once the boat goes 2 m/s.
//   0x53ebf0  the boat stage's time and top speed (0x5397b0, 0x5397d0), the height on the water.
//   0x53ed50.. the status follows the pick; 0x53f5b0 starts its motion (51, 52, 53) in step with
//             the old one (mode 10) and James' (0x4e24..0x4e26, 0x533800) with it.
//   0x53ee20  drag (0.15 and 0.2 a second), the push, speed -3..3.5 m/s, turn rate +-1.5 rad/s
//             (less the faster it goes), the move along the heading (x 500 units a metre), and the
//             bob, roll and pitch waves.
//   0x53f3c0  the stroke speed: towards a x 32768 (b for the left oars) by 64 a frame, back by
//             16, at most 2048, played at twice that; with the stick let go the oars go on at 1024
//             until they rest at frames 40..42.
//   0x53f6b0 / 0x53f790  an oar blade going into the water (deeper than y 3500): SE 0x3cf1 /
//             0x3cf2 5000 units to its side.
//   0x53f9b0  the rocking: the stick and the lake's rings of waves (0x559b40: 20 rings from
//             (-10000, -60000), 3 m/s outwards, reborn at 240 m) push the roll and the pitch.
// Frames of the PC's game run at 30 a second; the per-frame steps here are scaled to that.
#include "boat.h"
#include "chrlight.h"
#include "events.h"
#include "gamecam.h"
#include "prog.h"
#include "sfx.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"
#define PI 3.14159265f
#define UNIT 500.0f                     // world units a metre (0x62d230)
#define FRAMES 150.0f                   // each rowing motion (table 0x6e2298)
#define DOCK_START 450.0f               // motion 54: the boat lying at a pier, frames 450..473
#define DOCK_LEN 24.0f
#define BLEND_S (32.0f / 30.0f)         // mode 10 mixes for 32 frames

enum { B_NONE, B_DOCK, B_ROW };
static int s_mode;
static bool s_loaded, s_failed, s_janim_ok;
static Character s_chr;
static Anim s_anim, s_janim;
static ChrBlend s_bblend, s_jblend;

static float s_pos[3], s_rot[3];        // +0x1c; +0x2c pitch, +0x30 turn, +0x34 roll
static float s_speed, s_rotv;           // +0xa8 (m/s), +0x38 (rad/s)
static float s_vel[2];
static float s_a, s_b, s_turn, s_fwd;   // 0x1d82254, 0x1d8228c, 0x1d821ec, 0x1d82288
static bool s_idle;                     // 0x1d82394 / 0x1d82250 bit 2: the stick let go
static int s_status, s_want, s_cur = -1;   // 0x1f99404, 0x1f99419, 0x8be37c
static float s_want_t, s_blend_t;       // 0x1f9941c; 0x1f99410 bit 4 while the motions mix
static float s_sp_a, s_sp_b, s_aspeed;  // 0x1d82360, 0x1d82358, 0x1f99420 (2048 = 30 frames/s)
static float s_frame;                   // the motion's frame, 0..150
static float s_dock_frame;
static bool s_switched;                 // 0x1f99418: James takes the new motion
static int s_jstatus = -1;
static float s_dt;                      // the time boat_draw's crossfade moves on
// the waves (0x53ee20): bob, roll, pitch
static float s_bob_amp, s_bob_t, s_bob_h;          // 0x1d82294, 0x1d822fc, 0x1d82300
static float s_roll_amp, s_roll_t, s_roll;         // 0x1d82390, 0x1d82310, 0x1d82248
static float s_pitch_amp, s_pitch_t, s_pitch;      // 0x1d822f8, 0x1d82370, 0x1d82364
static int s_roll_st, s_pitch_st;                  // 0x1d82290, 0x1d82308
static bool s_oar_in[2];                           // 0x1d82278, 0x1d821f0

// the lake's rings of waves (lake_wave.c _shLakeWaveInfo, 0x1d82528)
typedef struct { float r, w, e, t; int next; } Ring;
static Ring s_ring[20];
static int s_newest, s_hits;            // 0x1d826b8, 0x1d82520

static float wrap_pi(float a)
{
	if (a > PI) a -= 2.0f * PI;
	else if (a < -PI) a += 2.0f * PI;
	return a;
}

static float decay(float per_frame, float dt) { return powf(1.0f - per_frame, dt * 30.0f); }

// 0x5597c0
static void rings_init(void)
{
	for (int i = 0; i < 20; i++) {
		float t = (float)i * 4.0f;
		s_ring[i].t = t;
		s_ring[i].r = 3.0f * t;
		s_ring[i].w = 0.03f * t + 3.0f;
		s_ring[i].e = 0.5f - t * 0.015f;
		s_ring[i].next = (i + 1) % 20;
	}
	s_newest = s_hits = 0;
}

// 0x559b40: the rings grow; the first one (from the newest on) the boat is on pushes it.
static const Ring* rings_update(float dt, float* dir)
{
	for (int i = 0; i < 20; i++) {
		Ring* g = &s_ring[i];
		g->t += dt;
		g->r = 3.0f * g->t;
		g->w = g->t * 0.03f + 3.0f;
		g->e -= dt * 0.015f;
		if (g->r >= 240.0f) {
			s_newest = i;
			g->r = 0.0f;
			g->w = 3.0f;
			g->t = 0.0f;
			g->e = sinf((float)s_hits * PI * 0.0055555557f) * 0.25f + 0.5f;
		}
	}
	float dx = s_pos[0] * 0.002f + 20.0f, dz = s_pos[2] * 0.002f + 120.0f;
	float d = sqrtf(dx * dx + dz * dz);
	int i = s_newest;
	while (s_ring[i].next != s_newest) {
		if (s_ring[i].r - s_ring[i].w < d && d <= s_ring[i].w + s_ring[i].r)
			break;
		i = s_ring[i].next;
	}
	if (s_ring[i].next == s_newest)
		return NULL;
	*dir = atan2f(dx, dz);
	s_hits++;
	return &s_ring[i];
}

static void waves_reset(void)
{
	s_bob_amp = s_bob_t = s_bob_h = 0.0f;
	s_roll_amp = s_roll_t = s_roll = 0.0f;
	s_pitch_amp = s_pitch_t = s_pitch = 0.0f;
	s_roll_st = s_pitch_st = 0;
}

// 0x53e5c0
static void row_reset(void)
{
	waves_reset();
	s_a = s_b = s_turn = s_fwd = 0.0f;
	s_idle = true;
	s_sp_a = s_sp_b = s_aspeed = 0.0f;
	s_speed = s_rotv = 0.0f;
	s_vel[0] = s_vel[1] = 0.0f;
	s_oar_in[0] = s_oar_in[1] = false;
	s_status = s_want = 0;
	s_cur = -1;
	s_want_t = s_blend_t = 0.0f;
	s_frame = 0.0f;
	s_switched = true;
	s_jstatus = -1;
	rings_init();
}

static void unload(void)
{
	if (s_loaded) {
		chr_free(&s_chr);
		anim_free(&s_anim);
		if (s_janim_ok)
			anim_free(&s_janim);
	}
	s_loaded = s_janim_ok = false;
	memset(&s_bblend, 0, sizeof s_bblend);
	memset(&s_jblend, 0, sizeof s_jblend);
}

static bool load(void)
{
	if (s_loaded || s_failed)
		return s_loaded;
	char err[96];
	if (!chr_load(&s_chr, PACK_DIR "/chr/boat", "boat", err, sizeof err) ||
	    !anim_load(&s_anim, PACK_DIR "/chr/boat/boat.s3a")) {
		chr_free(&s_chr);
		s_failed = true;
		return false;
	}
	s_janim_ok = anim_load(&s_janim, PACK_DIR "/chr/jms/jms_boat.s3a");
	s_loaded = true;
	return true;
}

void boat_reset(void)
{
	s_mode = B_NONE;
	gamecam_on_boat(false);
}

bool boat_active(void) { return s_mode == B_ROW; }

static void place(float x, float y, float z, float yaw)
{
	s_pos[0] = x, s_pos[1] = y, s_pos[2] = z;
	s_rot[0] = s_rot[2] = 0.0f;
	s_rot[1] = yaw;
}

void boat_embark_at(float x, float z, float yaw)
{
	boat_embark();
	place(x, 3592.5f, z, yaw);
	s_pos[1] = 7.38f * UNIT;
}

void boat_embark(void)
{
	row_reset();
	place(-14970.1f, 3592.5f, -98254.0f, -1.5707964f);   // 0x8cf698
	s_pos[1] = 7.38f * UNIT;                              // 0x53fe40 step 0: on the water
	s_mode = B_ROW;
	gamecam_on_boat(true);
}

void boat_disembark(void)
{
	row_reset();
	place(-8915.9f, 3499.1f, -19412.7f, PI);            // 0x8cf6dc
	s_pos[1] = 7.38f * UNIT;
	s_dock_frame = 0.0f;
	s_mode = B_DOCK;
	gamecam_on_boat(false);
}

void boat_tick(int stage, float dt)
{
	s_dt = dt;
	if (stage != 38) {
		if (s_mode != B_NONE)
			boat_reset();
		unload();
		s_failed = false;
		return;
	}
	// 0x57e7a0, the lake's room function: until James has taken it (flag 379, EvProgEmbarkBoat's
	// event) the boat lies at the town pier, facing pi / 2
	if (s_mode == B_NONE && !ev_flag(379)) {
		row_reset();
		place(-5768.15f, 174.62f, -99725.3f, 1.5707964f);   // 0x8cf814
		s_pos[1] = 7.38f * UNIT;                          // 0x53fe40 step 0 puts it on the water
		s_dock_frame = 0.0f;
		s_mode = B_DOCK;
	}
	if (s_mode != B_NONE)
		load();
	if (s_mode == B_DOCK)
		s_dock_frame = fmodf(s_dock_frame + dt * 15.0f, DOCK_LEN);   // speed 1024: 15 frames a second
}

// 0x53e760: the keys. Writes s_a, s_b, s_turn, s_fwd, s_idle and the oars it would row with.
static void keys(const BoatInput* in)
{
	// the PC's stick bytes less 0x80, shifted down 4 (-7..7), the arrow keys as 7; its x grows to
	// the right and its y downwards
	int x = (int)(in->x * 127.0f) / 16, y = (int)(-in->y * 127.0f) / 16;
	if (in->down) y = 7;
	if (in->up) y = -7;
	if (in->right) x = 7;
	if (in->left) x = -7;
	float ang = atan2f((float)x, (float)-y), s = sinf(ang), c = cosf(ang);
	float m = sqrtf((s * x) * (s * x) + (c * y) * (c * y));    // 0x53e6c0: 0..1
	m = m < 0.0f ? 0.0f : m > 1.0f ? 1.0f : m;
	s_a = c * m * 0.5f + s * m * 0.25f;
	s_b = c * m * 0.5f - s * m * 0.25f;
	if (!in->run) {
		s_a *= 0.5f;
		s_b *= 0.5f;
	}
	int ox, oy;                         // table 0x53e760 local_24 [oy][ox]
	if (s_a < 0.0f && s_b >= 0.0f) ox = 1, oy = 0;
	else if (s_a > 0.0f && s_b <= 0.0f) ox = 0, oy = 1;
	else ox = 1, oy = 1;
	static const int pick[2][3] = { { 0, 1, 1 }, { 2, 0, 1 } };
	s_idle = s_a == 0.0f && s_b == 0.0f;
	float pa = 0.0f, pb = 0.0f;
	if (s_frame > 16.0f && s_frame < 80.0f)    // 0x10 < frame < 0x50
		pa = s_a * 1.8f, pb = s_b * 1.8f;
	s_turn = pa - pb;
	s_fwd = (pb + pa) * 0.5f;
	if (s_want_t == 0.0f) {
		int prev = s_want;
		s_want = pick[oy][ox];
		if (s_speed >= 2.0f)
			s_want = 0;
		if (prev != s_want)
			s_want_t = 1.5f;
	}
}

static float approach(float cur, float tgt, float k)
{
	float up = 64.0f * k, down = 16.0f * k, s;
	if (fabsf(cur) < fabsf(tgt)) {
		if (cur == tgt) return cur;
		if (tgt < cur) { s = cur - up; if (s < tgt) s = tgt; }
		else { s = cur + up; if (s > tgt) s = tgt; }
	} else {
		if (cur == tgt) return cur;
		if (cur <= tgt) { s = cur + down; if (s > tgt) s = tgt; }
		else { s = cur - down; if (s < tgt) s = tgt; }
	}
	return s;
}

// 0x53f9b0's switch: an energy swings the wave between its two sides
static void swing(int* st, float* amp, float* t, float push)
{
	switch (*st) {
	case 0: *amp += push; if (*amp < 0.0f) { *t = 0.5f; *st = 2; *amp = -*amp; } break;
	case 1: *amp += push; if (*amp < 0.0f) { *t = 1.0f; *st = 3; *amp = -*amp; } break;
	case 2: *amp -= push; if (*amp < 0.0f) { *t = 0.0f; *st = 0; *amp = -*amp; } break;
	case 3: *amp -= push; if (*amp < 0.0f) { *t = 1.5f; *st = 1; *amp = -*amp; } break;
	}
}

static void kick(bool start, float push, int* st, float* t, float v)
{
	if (!start)
		return;
	if (push <= 0.0f) {
		*t = 1.0f;
		*st = v <= 0.0f ? 3 : 2;
	} else {
		*t = 0.0f;
		*st = v >= 0.0f ? 0 : 1;
	}
}

// 0x53ee20's wave part: one swinging height (amp eased off by k a frame)
static float wave(float* amp, float* t, float dt, float k, bool root)
{
	*t += dt;
	if (*t > 2.0f)
		*t -= 2.0f;
	float a = root ? sqrtf(*amp * 0.0013509489f) : *amp;
	float h = sinf(*t * PI) * a;
	*amp -= *amp * (1.0f - decay(k, dt));
	if (*amp >= -0.0005f && *amp < 0.0005f) {
		*amp = *t = 0.0f;
		h = 0.0f;
	}
	return h;
}

// The world transform of a joint of the posed boat (bone b's origin)
static void joint_world(int b, float out[3])
{
	const Mtx34* ib = &s_chr.inv_bind[b];
	float bt[3];
	for (int i = 0; i < 3; i++)
		bt[i] = -(ib->m[0][i] * ib->m[0][3] + ib->m[1][i] * ib->m[1][3] + ib->m[2][i] * ib->m[2][3]);
	const Mtx34* s = &s_chr.skin[b];
	float m[3];
	for (int i = 0; i < 3; i++)
		m[i] = s->m[i][0] * bt[0] + s->m[i][1] * bt[1] + s->m[i][2] * bt[2] + s->m[i][3];
	C3D_Mtx w;
	Mtx_Identity(&w);
	Mtx_Translate(&w, s_pos[0], s_pos[1], s_pos[2], true);
	Mtx_RotateZ(&w, s_rot[2], true);
	Mtx_RotateY(&w, s_rot[1], true);
	Mtx_RotateX(&w, s_rot[0], true);
	C3D_FVec v = Mtx_MultiplyFVec3(&w, FVec3_New(m[0], m[1], m[2]));
	out[0] = v.x + w.r[0].w, out[1] = v.y + w.r[1].w, out[2] = v.z + w.r[2].w;
}

// 0x53f6b0 / 0x53f790: the blades (bones 3 and 4) going into the water
static void oar_sounds(void)
{
	if (!s_loaded || s_chr.n_bones < 5)
		return;
	float vol = fabsf(s_aspeed) * 9.765625e-05f;
	for (int k = 0; k < 2; k++) {
		float tip[3];
		joint_world(3 + k, tip);
		bool in = tip[1] > 3500.0f;
		if (in && !s_oar_in[k]) {
			float side = s_rot[1] + (k == 0 ? 1.5707964f : -1.5707964f);
			float at[3] = { tip[0] + sinf(side) * 5000.0f, tip[1], tip[2] + cosf(side) * 5000.0f };
			sfx_play_at(k == 0 ? 0x3cf1 : 0x3cf2, vol, at);
		}
		s_oar_in[k] = in;
	}
}

void boat_update(const BoatInput* in, const Collision* col, float dt, float pos[3], float* yaw,
                 float vel[2], float* turn_speed)
{
	if (s_mode != B_ROW)
		return;
	// James was put somewhere else (a warp, the playtest standing him at a trigger): the boat goes
	// with him, as the PC's boat takes him along (0x540190)
	if (fabsf(pos[0] - s_pos[0]) + fabsf(pos[2] - s_pos[2]) > 300.0f) {
		s_pos[0] = pos[0], s_pos[2] = pos[2];
		s_rot[1] = *yaw;
		s_speed = s_rotv = 0.0f;
	}
	float k = dt * 30.0f;
	keys(in);
	// 0x53ebf0
	prog_stat_boat(dt, s_speed);
	if (s_blend_t <= 0.0f && (s_want_t -= dt) < 0.0f)
		s_want_t = 0.0f;
	s_pos[1] = (s_bob_h + 7.38f) * UNIT;
	// 0x53ed50 / 0x53ed90 / 0x53edd0
	s_status = s_want;
	// 0x53f5b0: the oars' motion changes once the last change has mixed in
	s_switched = false;
	if (s_blend_t > 0.0f)
		s_blend_t -= dt;
	if (s_blend_t <= 0.0f && s_status != s_cur) {
		if (s_cur >= 0)
			chr_blend_start_sync(&s_bblend, &s_anim, s_cur * FRAMES, s_status * FRAMES, FRAMES, BLEND_S);
		s_cur = s_status;
		s_blend_t = BLEND_S;
		s_switched = true;
	}
	// 0x53ee20: speed and turn
	s_speed -= dt * s_speed * 0.15f;
	s_rotv -= dt * s_rotv * 0.2f;
	if (s_speed >= -0.05f && s_speed < 0.05f && s_fwd == 0.0f)
		s_speed = 0.0f;
	if (s_rotv >= -0.025f && s_rotv < 0.025f && s_turn == 0.0f)
		s_rotv = 0.0f;
	s_speed += s_fwd * 1.5f * dt;
	s_speed = s_speed < -3.0f ? -3.0f : s_speed > 3.5f ? 3.5f : s_speed;
	s_rotv += s_turn * 0.25f / (fabsf(s_speed) + 1.0f) * dt;
	s_rotv = s_rotv < -1.5f ? -1.5f : s_rotv > 1.5f ? 1.5f : s_rotv;
	float h = wrap_pi(s_rot[1]);
	float mx = dt * s_speed * sinf(h) * UNIT, mz = dt * s_speed * cosf(h) * UNIT;
	float yaw_step = dt * s_rotv;
	if (s_bob_amp != 0.0f)
		s_bob_h = -wave(&s_bob_amp, &s_bob_t, dt, 0.01f, false);
	if (s_roll_amp != 0.0f) {
		s_roll = wave(&s_roll_amp, &s_roll_t, dt, 0.02f, true);
		s_rot[2] = atan2f(s_roll, 0.5f);
	}
	if (s_pitch_amp != 0.0f) {
		s_pitch = wave(&s_pitch_amp, &s_pitch_t, dt, 0.02f, true);
		s_rot[0] = atan2f(s_pitch, 2.0f);
	}
	// 0x53f3c0: the stroke speed
	s_sp_a = approach(s_sp_a, truncf(s_a * 32768.0f), k);
	s_sp_b = approach(s_sp_b, truncf(s_b * 32768.0f), k);
	s_sp_a = s_sp_a < -2048.0f ? -2048.0f : s_sp_a > 2048.0f ? 2048.0f : s_sp_a;
	s_sp_b = s_sp_b < -2048.0f ? -2048.0f : s_sp_b > 2048.0f ? 2048.0f : s_sp_b;
	s_aspeed = (s_status == 1 ? s_sp_b : s_sp_a) * 2.0f;
	int fr = (int)s_frame;
	if (s_idle && (fr < 0x28 || fr > 0x2a))
		s_aspeed = s_speed >= 0.0f ? 1024.0f : -1024.0f;
	// 0x53f790: the oars' sounds (on the pose drawn last)
	oar_sounds();
	// 0x53f9b0: the rocking
	bool roll_go = s_roll_amp == 0.0f && s_turn != 0.0f;
	bool pitch_go = s_pitch_amp == 0.0f && s_fwd != 0.0f;
	float pr = s_turn * 0.05f, pp = s_fwd * 0.05f, dir;
	const Ring* g = rings_update(dt, &dir);
	if (g) {
		float a = wrap_pi(dir - s_rot[1]);
		pr += 2.0f * sinf(a) * g->e * dt;
		pp += cosf(a) * g->e * dt * 4.0f;
		roll_go = s_roll_amp == 0.0f;
		pitch_go = s_pitch_amp == 0.0f;
		if (s_bob_amp == 0.0f) {
			s_bob_t = 0.0f;
			s_bob_amp += g->e * 0.05f;
		}
	}
	kick(roll_go, pr, &s_roll_st, &s_roll_t, s_roll);
	kick(pitch_go, pp, &s_pitch_st, &s_pitch_t, s_pitch);
	swing(&s_roll_st, &s_roll_amp, &s_roll_t, pr);
	swing(&s_pitch_st, &s_pitch_amp, &s_pitch_t, pp);
	// 0x5375e0 / 0x5376c0: the move and the turn; the boat's column (1650 across) against walls
	float nx = s_pos[0] + mx, nz = s_pos[2] + mz;
	if (col)
		col_push(col, &nx, &nz, s_pos[1] + 50.0f, 100.0f, 1650.0f);
	s_vel[0] = dt > 0.0f ? (nx - s_pos[0]) / dt : 0.0f;
	s_vel[1] = dt > 0.0f ? (nz - s_pos[2]) / dt : 0.0f;
	s_pos[0] = nx, s_pos[2] = nz;
	s_rot[1] = wrap_pi(s_rot[1] + yaw_step);
	// the motion plays on at the stroke speed
	s_frame += s_aspeed / 2048.0f * 30.0f * dt;
	s_frame = fmodf(s_frame, FRAMES);
	if (s_frame < 0.0f)
		s_frame += FRAMES;
	// 0x540190: James sits where the boat is
	pos[0] = s_pos[0], pos[1] = s_pos[1], pos[2] = s_pos[2];
	*yaw = s_rot[1];
	vel[0] = s_vel[0], vel[1] = s_vel[1];
	*turn_speed = s_rotv;
}

static void model_matrix(C3D_Mtx* m)
{
	Mtx_Identity(m);
	Mtx_Translate(m, s_pos[0], s_pos[1], s_pos[2], true);
	Mtx_RotateZ(m, s_rot[2], true);
	Mtx_RotateY(m, s_rot[1], true);
	Mtx_RotateX(m, s_rot[0], true);
}

void boat_draw(const C3D_Mtx* view, int uModelView, int uBones)
{
	if (s_mode == B_NONE || !s_loaded)
		return;
	C3D_Mtx model, mv;
	model_matrix(&model);
	Mtx_Multiply(&mv, view, &model);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
	s_bblend.look_head = s_bblend.look_neck = -1;
	if (s_mode == B_ROW)
		chr_pose_blend(&s_chr, &s_anim, (float)s_cur * FRAMES + s_frame, &s_bblend, s_dt);
	else
		chr_pose(&s_chr, &s_anim, DOCK_START + s_dock_frame);
	s_dt = 0.0f;
	chrlight_at(s_pos, false);
	chr_draw(&s_chr, uBones);
}

bool boat_james_model(C3D_Mtx* model)
{
	if (s_mode != B_ROW)
		return false;
	model_matrix(model);
	return true;
}

// James' motions 0x4e24..0x4e26 (jms_boat: 75 frames each, half the boat's 150), set with the
// boat's (0x533800) and kept at its frame (0x559e40: 0x5390d0 with the boat's 0x5395e0).
bool boat_james_pose(Character* james, float dt)
{
	if (s_mode != B_ROW || !s_janim_ok || s_janim.bones != james->n_bones)
		return false;
	int st = s_cur < 0 ? 0 : s_cur;
	if (st != s_jstatus) {
		if (s_jstatus >= 0)
			chr_blend_start_sync(&s_jblend, &s_janim, s_jstatus * 75.0f, st * 75.0f, 75.0f, BLEND_S);
		else
			chr_blend_start(&s_jblend, 8.0f / 30.0f);
		s_jstatus = st;
	}
	s_jblend.look_head = s_jblend.look_neck = -1;
	chr_pose_blend(james, &s_janim, (float)st * 75.0f + s_frame * 0.5f, &s_jblend, dt);
	return true;
}

const char* boat_status(void)
{
	static char buf[80];
	if (s_mode == B_NONE)
		return "no boat";
	snprintf(buf, sizeof buf, "%s st%d f%.0f v%.2f r%.2f a%.0f", s_mode == B_ROW ? "row" : "dock", s_cur,
	         s_frame, s_speed, s_rotv, s_aspeed);
	return buf;
}
