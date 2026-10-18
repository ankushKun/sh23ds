// James' legs while he is free: the PC's 3D-control lower-body states (see move.h).
//
// Sources (sh2pc.exe, Director's Cut):
//   0x52e110  pad read: the stick's cross dead zone (each axis is cut by half the other's tilt), full
//             tilt at 85 (x) and 100 (y) of 127, run held (0xc00 or the analog run option).
//   0x54e0f0  lower state choice: quick turn, forward (run3 / run2 / run1 or walk), back, strafe,
//             strafe at a run, turn on the spot, stand. A state may only follow the ones its row of
//             0x6cf878 allows (STATE_NEXT below); entering a state sets its start speed.
//   0x8bf6d8  the per-state movers (0x54bbb0 / 0x54bc90 turn on the spot at 2.4 rad/s, x1.5 with run
//             held; 0x54bd70 walk 1.5 m/s, turning 1.9; 0x54c0e0 back 1.2, turning 1.3; 0x54c470 /
//             0x54c810 strafe 1.3; 0x54cbb0 / 0x54cf10 / 0x54d300 run 3.5 / 4.0 / 4.8, turning 1.7;
//             0x54d670 / 0x54d8e0 strafe run 3.8; 0x54f160 / 0x54f240 the 180 degree turn at 5.5 rad/s;
//             0x557b20 stand, slowing at 10 m/s/s, 20 after a run).
//   0x54ef10  velocity = speed x 500 units per second along the facing plus the state's offset (back
//             pi, strafes pi/2), the offset easing back at 4 rad/s; Maria's x0.8.
//   0x531a00  clip per state from the base table 0x6ceac4 (ids 101..133) and how it starts: mode 2
//             cut, mode 4 an 8-frame crossfade from the old pose, mode 10 a 32-frame crossfade that
//             keeps the gait's phase (walk, run and strafe clips into each other).
//   0x533ad0 / 0x5350d0  stamina: running counts frames up to 600 at full health (fewer when hurt),
//             standing counts them down; run3 needs it below the limit, and standing with more than
//             a third of it plays the tired breathing (113, 114 past two thirds).
#include "move.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define UNIT 500.0f          // 0x54ef10: metres per second to world units
#define FRAME (1.0f / 30.0f) // the PC's frame counts (blends, stamina) are 30 fps frames

typedef struct { u16 id, start, end; bool loop; s16 speed; } Clip;   // speed 2048 = 30 frames/s

static const Clip C_STAND = { 101, 1, 10, 1, 256 };
static const Clip C_RROUND = { 102, 14, 28, 1, 1280 };
static const Clip C_LROUND = { 103, 34, 48, 1, 1280 };
static const Clip C_WALK = { 104, 51, 74, 1, 1408 };
static const Clip C_BACK = { 105, 75, 98, 1, 1536 };
static const Clip C_RUN1 = { 106, 99, 122, 1, 1536 };
static const Clip C_RUN2 = { 107, 123, 146, 1, 1792 };
static const Clip C_RUN3 = { 108, 147, 170, 1, 2048 };
static const Clip C_RTURN = { 109, 171, 190, 0, 2048 };
static const Clip C_LTURN = { 110, 191, 210, 0, 2048 };
static const Clip C_TIRED[2] = { { 113, 281, 300, 1, 1024 }, { 114, 301, 320, 1, 1024 } };
static const Clip C_RELAX[3] = { { 115, 321, 350, 0, 384 }, { 116, 351, 380, 0, 384 }, { 117, 381, 410, 0, 384 } };
static const Clip C_ALERT = { 118, 411, 440, 0, 384 };
static const Clip C_RSWALK = { 124, 516, 539, 1, 1536 };
static const Clip C_LSWALK = { 125, 540, 563, 1, 1536 };
static const Clip C_RSRUN = { 126, 564, 587, 1, 2048 };
static const Clip C_LSRUN = { 127, 588, 611, 1, 2048 };

// 0x6cf878: the states each state may change to (bit = MV_* index)
#define B(s) (1u << (s))
#define FREE_GO (B(MV_LROUND) | B(MV_RROUND) | B(MV_BACK) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RSWALK) | \
                 B(MV_RUN1) | B(MV_LSRUN) | B(MV_RSRUN) | B(MV_LTURN) | B(MV_RTURN))
static const u32 STATE_NEXT[MV_COUNT] = {
	[MV_STAND] = FREE_GO | B(MV_RELAX) | B(MV_TIRED) | B(MV_ALERT),
	[MV_RELAX] = FREE_GO | B(MV_STAND),
	[MV_TIRED] = FREE_GO | B(MV_STAND),
	[MV_LROUND] = FREE_GO | B(MV_STAND),
	[MV_RROUND] = FREE_GO | B(MV_STAND),
	[MV_BACK] = B(MV_STAND) | B(MV_BACK) | B(MV_LTURN) | B(MV_RTURN),
	[MV_WALK] = B(MV_STAND) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RSWALK) | B(MV_RUN1) | B(MV_RUN2) |
	            B(MV_LSRUN) | B(MV_RSRUN) | B(MV_LTURN) | B(MV_RTURN),
	[MV_LSWALK] = B(MV_STAND) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RUN1) | B(MV_LSRUN) | B(MV_LTURN) | B(MV_RTURN),
	[MV_RSWALK] = B(MV_STAND) | B(MV_WALK) | B(MV_RSWALK) | B(MV_RUN1) | B(MV_RSRUN) | B(MV_LTURN) | B(MV_RTURN),
	[MV_RUN1] = B(MV_STAND) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RSWALK) | B(MV_RUN1) | B(MV_LSRUN) | B(MV_RSRUN),
	[MV_RUN2] = B(MV_STAND) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RSWALK) | B(MV_RUN2) | B(MV_LSRUN) | B(MV_RSRUN),
	[MV_RUN3] = B(MV_STAND) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RSWALK) | B(MV_RUN3) | B(MV_LSRUN) | B(MV_RSRUN),
	[MV_LSRUN] = B(MV_STAND) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RUN1) | B(MV_LSRUN),
	[MV_RSRUN] = B(MV_STAND) | B(MV_WALK) | B(MV_RSWALK) | B(MV_RUN1) | B(MV_RSRUN),
	[MV_LTURN] = 0, [MV_RTURN] = 0,      // the turn runs to its end
	[MV_ALERT] = FREE_GO | B(MV_STAND),
};

static const char* const NAMES[MV_COUNT] = { "stand", "relax", "tired", "lround", "rround", "back", "walk",
                                             "lswalk", "rswalk", "run1", "run2", "run3", "lsrun", "rsrun",
                                             "lturn", "rturn", "alert" };

static struct {
	int state, prev;
	float spd;            // m/s, the PC's 0xa8
	float off;            // movement direction relative to the facing (0xb4)
	float stam;           // 30 fps frames run (0x1fbacf4)
	float still;          // seconds standing (0x1fbacd0, for the relax idles)
	float turned, turn_to;// the 180 turn so far, and the yaw it ends on
	float cam_lock, stick_at_lock;   // the PC 2D type's camera yaw and stick angle, held across cuts
	bool cam_locked;
	u32 allow;            // 0x1fba900: the states this one may change to (STATE_NEXT, then the runs)
	const Clip* clip;
	float frame;
	MoveOut out;
	bool interrupted;
	char status[48];
} M;

void move_reset(void)
{
	memset(&M, 0, sizeof M);
	M.clip = &C_STAND;
	M.frame = C_STAND.start;
	M.allow = STATE_NEXT[MV_STAND];
}

const MoveOut* move_last(void) { return &M.out; }
void move_interrupt(void) { M.interrupted = true; }

static bool running(int s) { return s >= MV_RUN1 && s <= MV_RSRUN; }
static bool moving(int s) { return s >= MV_BACK && s <= MV_RSRUN; }

// 0x531a00: the clips a gait change keeps the phase from (mode 10), per new state
static bool phase_kept(int to, u16 from)
{
	static const u16 keep[MV_COUNT][8] = {
		[MV_WALK] = { 106, 107, 108, 124, 125, 126, 127 },
		[MV_LSWALK] = { 104, 106, 107, 108, 127 },
		[MV_RSWALK] = { 104, 106, 107, 108, 126 },
		[MV_RUN1] = { 104, 107, 108, 124, 125, 126, 127 },
		[MV_RUN2] = { 104, 106, 108, 124, 125, 126, 127 },
		[MV_RUN3] = { 104, 106, 107, 124, 125, 126, 127 },
		[MV_LSRUN] = { 104, 106, 107, 108, 125 },
		[MV_RSRUN] = { 104, 106, 107, 108, 124 },
	};
	for (int i = 0; i < 8 && keep[to][i]; i++)
		if (keep[to][i] == from)
			return true;
	return false;
}

// Maria's anm (xmar_wp*) holds the same base moves at the same frames as James' (her feet move the
// same way through 104..127), and 0x531a00 picks them from the one table for both.
static const Clip* state_clip(int s)
{
	switch (s) {
	case MV_RELAX: return &C_RELAX[rand() % 3];
	case MV_ALERT: return &C_ALERT;
	case MV_TIRED: return &C_TIRED[0];
	case MV_LROUND: return &C_LROUND;
	case MV_RROUND: return &C_RROUND;
	case MV_BACK: return &C_BACK;
	case MV_WALK: return &C_WALK;
	case MV_LSWALK: return &C_LSWALK;
	case MV_RSWALK: return &C_RSWALK;
	case MV_RUN1: return &C_RUN1;
	case MV_RUN2: return &C_RUN2;
	case MV_RUN3: return &C_RUN3;
	case MV_LSRUN: return &C_LSRUN;
	case MV_RSRUN: return &C_RSRUN;
	case MV_LTURN: return &C_LTURN;
	case MV_RTURN: return &C_RTURN;
	default: return &C_STAND;
	}
}

static void set_clip(const Clip* c, int state, MoveOut* out)
{
	const Clip* old = M.clip;
	if (c == old && !M.interrupted)
		return;
	float rel = M.frame - old->start;
	M.clip = c;
	out->clip_changed = true;
	if (state == MV_LTURN || state == MV_RTURN) {
		M.frame = c->start;               // mode 2: a cut
		out->blend = 0.0f;
	} else if (!M.interrupted && phase_kept(state, old->id) && c->end - c->start == old->end - old->start) {
		M.frame = c->start + rel;         // mode 10: same step of the gait, 32 frames to mix
		out->blend = 32.0f * FRAME;
		out->sync = true;
		out->from_start = old->start;
	} else {
		M.frame = c->start;               // mode 4
		out->blend = 8.0f * FRAME;
	}
	M.interrupted = false;
}

static void enter(int s)
{
	if (s == M.state)
		return;
	M.prev = M.state;
	M.state = s;
	M.still = 0.0f;
	M.allow = STATE_NEXT[s];      // 0x52da80
	// start speeds (0x54e0f0)
	switch (s) {
	case MV_BACK: M.spd = 0.6f; break;
	case MV_WALK: case MV_LSWALK: case MV_RSWALK: if (M.spd < 0.6f) M.spd = 0.6f; break;
	case MV_RUN1: if (M.spd < 1.5f) M.spd = 1.5f; break;
	case MV_RUN2: if (M.spd < 2.5f) M.spd = 2.5f; break;
	case MV_RUN3: if (M.spd < 3.0f) M.spd = 3.0f; break;
	case MV_LSRUN: case MV_RSRUN: M.spd = 2.0f; break;
	default: break;
	}
}

static bool can(int to) { return to == M.state || (M.allow & B(to)) != 0; }

// speed towards target at rate up, down (m/s per second)
static void ramp(float target, float up, float down, float dt)
{
	if (M.spd < target) {
		M.spd += up * dt;
		if (M.spd > target) M.spd = target;
	} else if (M.spd > target) {
		M.spd -= down * dt;
		if (M.spd < target) M.spd = target;
	}
}

static float wrap(float a)
{
	while (a > PI) a -= 2.0f * PI;
	while (a < -PI) a += 2.0f * PI;
	return a;
}

void move_update(const MoveInput* in, float* yaw, float dt, MoveOut* out)
{
	memset(out, 0, sizeof *out);
	float yaw0 = *yaw;
	float limit = 600.0f * (in->hp_frac < 0.0f ? 0.0f : in->hp_frac > 1.0f ? 1.0f : in->hp_frac);

	// 0x52e110: cross dead zone, then full tilt short of the rim
	float ax = fabsf(in->x), ay = fabsf(in->y);
	float dzx = ay * 0.5f > 0.244f ? ay * 0.5f : 0.0f, dzy = ax * 0.5f > 0.244f ? ax * 0.5f : 0.0f;
	float tx = ax > dzx ? (ax - dzx) / (0.67f - dzx > 0.05f ? 0.67f - dzx : 0.05f) : 0.0f;
	float ty = ay > dzy ? (ay - dzy) / 0.79f : 0.0f;
	tx = tx > 1.0f ? 1.0f : tx;
	ty = ty > 1.0f ? 1.0f : ty;
	int turn = tx > 0.0f ? (in->x > 0.0f ? 1 : -1) : 0;    // +1 right
	bool fwd = ty > 0.0f && in->y > 0.0f, back = ty > 0.0f && in->y < 0.0f;
	bool run = in->run;

	if (in->control == MOVE_2D) {
		// Directional Style, the PC's 2D type (0x547bf0, 0x546760, 0x548bf0): the stick's direction
		// on screen, taken from the camera James saw when he pushed it. After a camera cut that direction holds
		// until the stick turns more than 30 degrees or comes back to the middle, so he does not
		// spin round at the cut. He turns towards it at 20 x 0.3235 of the difference a second
		// and walks or runs along the way he faces, as fast as the stick is tilted.
		float mx = in->x, my = in->y, mag = sqrtf(mx * mx + my * my);
		turn = 0;
		back = false;
		tx = 0.0f;
		fwd = mag > 0.2f;
		if (!fwd) {
			M.cam_locked = false;
		} else {
			float sa = atan2f(mx, my);
			if (!M.cam_locked || fabsf(wrap(sa - M.stick_at_lock)) >= 0.5236f) {
				M.cam_lock = in->camera_yaw;
				M.stick_at_lock = sa;
				M.cam_locked = true;
			}
			float want = M.cam_lock + sa, d = wrap(want - *yaw);
			float k = 20.0f * 0.32345787f * dt;
			*yaw += d * (k > 1.0f ? 1.0f : k);
			ty = mag / 0.79f > 1.0f ? 1.0f : mag / 0.79f;
		}
	}

	// 0x54e0f0: the state for this frame
	bool sl = in->strafe_l, sr = in->strafe_r;
	bool quick = in->turn180 || (sl && sr && (in->strafe_l_down || in->strafe_r_down));
	int want = -1;
	if (M.state == MV_LTURN || M.state == MV_RTURN) {
		want = M.state;
	} else if (quick && can(MV_LTURN)) {
		want = turn > 0 ? MV_RTURN : MV_LTURN;
		M.turned = 0.0f;
		M.turn_to = wrap(*yaw + PI);
	} else if (fwd) {
		if (run && !in->wall_ahead) {
			if (can(MV_RUN3)) want = MV_RUN3;
			else if (can(MV_RUN2)) want = MV_RUN2;
			else if (can(MV_RUN1)) want = MV_RUN1;
		}
		if (want < 0 && can(MV_WALK))
			want = MV_WALK;
	} else if (back && can(MV_BACK)) {
		want = MV_BACK;
	} else if (sl != sr) {
		int w = sl ? MV_LSWALK : MV_RSWALK, r = sl ? MV_LSRUN : MV_RSRUN;
		if (run && !in->wall_ahead && can(r)) want = r;
		else if (can(w)) want = w;
	}
	if (want < 0 && turn && !back && can(turn < 0 ? MV_LROUND : MV_RROUND))
		want = turn < 0 ? MV_LROUND : MV_RROUND;
	if (want < 0) {
		// stand, relax after 10 s still, or breathe hard after a long run
		if ((M.state == MV_RELAX || M.state == MV_ALERT) && M.clip->end > M.frame)
			want = M.state;
		else if (M.stam > limit / 3.0f)
			want = MV_TIRED;
		else if (M.state == MV_STAND && M.still >= 10.0f)
			want = in->enemy_near ? MV_ALERT : MV_RELAX;   // 0x54e0f0 (0x5422b0: an enemy about)
		else
			want = MV_STAND;
	}
	enter(want);

	// the state's mover
	float maria = in->maria ? 0.8f : 1.0f;
	float off_to = 0.0f, rate = 0.0f;
	switch (M.state) {
	case MV_STAND: case MV_RELAX: case MV_TIRED: case MV_ALERT: {
		M.still += dt;
		M.stam -= dt * 30.0f;
		if (M.stam < 0.0f) M.stam = 0.0f;
		M.spd -= (running(M.prev) ? 20.0f : 10.0f) * dt;      // 0x5567b0
		if (M.spd < 0.0f) M.spd = 0.0f;
		off_to = M.off;
		break;
	}
	case MV_LROUND: case MV_RROUND:
		M.spd -= 10.0f * dt;
		if (M.spd < 0.0f) M.spd = 0.0f;
		rate = 2.4f * tx * (run ? 1.5f : 1.0f);
		off_to = M.off;
		break;
	// the port reads the circle pad only, so these are the PC's analog branches: the tilt sets the
	// walking pace at once and the running pace James speeds up or slows down to
	case MV_WALK:
		M.spd = ty * 1.5f;
		rate = 1.9f * tx;
		break;
	case MV_BACK:
		M.spd = ty * 1.2f;
		rate = 1.3f * tx;
		off_to = PI;
		break;
	case MV_LSWALK: case MV_RSWALK:
		ramp(1.3f, 1.7f, 3.4f, dt);
		rate = 1.9f * tx;
		off_to = M.state == MV_LSWALK ? PI / 2.0f : -PI / 2.0f;
		break;
	case MV_RUN1: case MV_RUN2: case MV_RUN3: {
		static const float top[3] = { 3.5f, 4.0f, 4.8f };
		float t = top[M.state - MV_RUN1] * ty;
		if (M.state == MV_RUN1 && t < 2.0f)
			t = 2.0f;
		ramp(t, 3.0f, 3.0f, dt);
		rate = 1.7f * tx;
		M.stam += dt * 30.0f;
		// the next pace is earned (0x54cbb0, 0x54cf10, 0x54d300)
		if (M.state == MV_RUN1 && M.spd >= 3.5f)
			M.allow = (M.allow | B(MV_RUN2)) & ~B(MV_RUN1);
		if (M.state == MV_RUN2 && M.spd >= 4.0f && M.stam < limit)
			M.allow = (M.allow | B(MV_RUN3)) & ~B(MV_RUN2);
		if (M.state == MV_RUN3 && M.stam >= limit)
			M.allow = (M.allow | B(MV_RUN2)) & ~B(MV_RUN3);
		break;
	}
	case MV_LSRUN: case MV_RSRUN:
		ramp(3.8f, 6.0f, 6.0f, dt);
		rate = 1.7f * tx;
		M.stam += dt * 30.0f;
		off_to = M.state == MV_LSRUN ? PI / 2.0f : -PI / 2.0f;
		break;
	case MV_LTURN: case MV_RTURN: {
		M.spd = 0.0f;
		float step = 5.4977875f * dt;
		M.turned += step;
		if (M.turned >= PI) {
			*yaw = M.turn_to;
			enter(MV_STAND);
		} else {
			*yaw += M.state == MV_LTURN ? step : -step;
		}
		off_to = 0.0f;
		M.off = 0.0f;
		break;
	}
	}

	if (in->control == MOVE_TANK && turn)
		*yaw -= (float)turn * rate * dt;      // right turns lower the port's yaw
	// the direction of travel eases between the gaits' offsets (0x54ef10, 4 rad/s)
	if (moving(M.state) && moving(M.prev)) {
		float d = wrap(off_to - M.off), s = 4.0f * dt;
		M.off += d > s ? s : d < -s ? -s : d;
	} else if (moving(M.state)) {
		M.off = off_to;
	}
	*yaw = wrap(*yaw);

	float v = M.spd * UNIT * maria, a = *yaw + M.off;
	out->dx = sinf(a) * v * dt;
	out->dz = cosf(a) * v * dt;
	out->turn_speed = dt > 0.0f ? wrap(*yaw - yaw0) / dt : 0.0f;
	out->state = M.state;

	// the clip and its frame
	const Clip* c = state_clip(M.state);
	if (M.state == MV_RELAX && (M.clip->id < 115 || M.clip->id > 117))
		set_clip(c, M.state, out);
	else if (M.state == MV_TIRED)
		set_clip(&C_TIRED[M.stam >= limit * 2.0f / 3.0f], M.state, out);
	else if (M.state != MV_RELAX)
		set_clip(c, M.state, out);
	c = M.clip;
	float fps = c->speed / 2048.0f * 30.0f;
	if (M.state == MV_WALK || M.state == MV_BACK)
		fps *= ty;                            // 0x54bd70 / 0x54c0e0: the clip slows with the tilt
	bool rev = false;
	float f0 = M.frame;
	M.frame += (rev ? -fps : fps) * dt;
	float len = (float)(c->end - c->start + 1);
	// 0x54a9a0: a step on frames 5 and 17 of the walking, backing, strafing and running clips (at 0.5
	// up to the left strafe, 0.8 from the right strafe on, as the PC compares the state number) and
	// on 6 and 13 of turning on the spot
	{
		int a = -1, b2 = -1;
		if (M.state == MV_LROUND || M.state == MV_RROUND) a = 6, b2 = 13;
		else if (moving(M.state)) a = 5, b2 = 17;
		float r0 = f0 - c->start, r1 = M.frame - c->start;
		for (int i = 0; i < 2 && a >= 0; i++) {
			float k = i ? (float)b2 : (float)a;
			bool hit = rev ? (r1 <= k && r0 > k) || (r0 < r1 && (r1 <= k || r0 > k))
			               : (r0 < k && r1 >= k) || (r1 < r0 && (r0 < k || r1 >= k));
			if (hit && !out->clip_changed)
				out->step = true;
		}
		out->step_volume = M.state >= MV_RSWALK && M.state <= MV_RSRUN ? 0.8f : 0.5f;
	}
	if (c->loop) {
		while (M.frame >= c->end + 1.0f) M.frame -= len;
		while (M.frame < c->start) M.frame += len;
	} else if (M.frame > c->end) {
		M.frame = c->end;
	}
	out->clip.start = c->start;
	out->clip.end = c->end;
	out->clip.loop = c->loop;
	out->clip.rate = fps;
	out->frame = M.frame;
	out->backward = rev;
	M.out = *out;
}

const char* move_status(void)
{
	snprintf(M.status, sizeof M.status, "move %-6s %4.2f m/s st%3.0f", NAMES[M.state], M.spd, M.stam);
	return M.status;
}
