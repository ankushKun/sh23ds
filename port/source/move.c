// James' legs while he is free: the PC's 3D-control lower-body states (see move.h).
//
// Sources (sh2pc.exe, Director's Cut):
//   0x52e110  PlayerCheckKeyInput, the pad read: the stick's cross dead zone (an axis under half the
//             other's tilt, at most 64 of 127, reads 0; past 31 the turn also counts from there),
//             full turn at 85 of 127 and full forward at 100; the 2D type has no cross (0x52dd20:
//             the tilt is the length of both, at most 1). Run held (0xc00 or the analog run option).
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
//   0x54dcc0  lower status 0x1a (the weapon held up): turns at 2.4 rad/s, 3.6 with run held, unless
//             a target is locked; 0x54bd70 / 0x54c0e0 / 0x54c470: locked, the stick turns nothing and
//             run held quickens the walk 1.5 x (at most 2.25 m/s, 1.8 backing off or strafing).
//   0x52e110 / 0x52dd20 with a direction key (0x1fbac6a 0): full turns, and the movers ramp the speed
//             (0x54bd70 walk up 2.5 / down 5 to 1.5, 7.5 out of backing off; 0x54c0e0 back 5 to 1.2,
//             first stopping at 10 out of another gait; strafes 1.7 / 3.4 to 1.3, 5.1 out of backing off).
//   0x8bf460  the 2D type's movers: no turning on the spot or quick turn (0x547bf0 picks the state),
//             walk 0x546aa0, back 0x546cc0 (1.5 x with run held), strafes 0x546ea0 / 0x547050 (by the
//             stick, 1.95 at most with run held), strafe runs 0x547700 (3.8 by the stick, 0.9 x
//             indoors), the hold stance 0x547a30 (0x558200, no turn: 0x546760 turns him to the stick).
//             Locked on (0x1fbad07) the stick's way picks walk, back or strafe and he goes that way.
//   0x54e0f0 / 0x547bf0 under an attack (upper status 0x1c): the lower state is kept while the
//             weapon's flags allow its gait (keep_gait).
//   0x533ad0 / 0x5350d0  stamina: the limit is health percent x 6 (600 at full health, 0x5350d0);
//             run2, run3 and the strafe runs (0x54cf10, 0x54d300, 0x54d670, 0x54d8e0) add the 60 Hz
//             refreshes the frame took (0x1d824a4, 2 a frame at 30 fps), standing takes them off
//             (0x557b20, 0x557cd0 with 0x1d82514), so the limit is 10 seconds of running; run3 needs
//             it below the limit, and standing with more than a third of it plays the tired breathing
//             (113, 114 past two thirds).
#include "move.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define UNIT 500.0f          // 0x54ef10: metres per second to world units
#define FRAME (1.0f / 30.0f) // the PC's frame counts (the blends) are 30 fps frames

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

// The great knife's lurching drag: speed (m/s) by frame of the walking, backing and strafing
// clips (0x54b3b0, 0x54b580, 0x54b750), the blade scraping on 5 and 18 (0x549e20)
static const float DRAG_WALK[24] = { 0.9f, 1.05f, 0.45f, 0.25f, 0.3f, 0.9f, 1.05f, 1.2f, 1.425f, 1.5f, 1.425f, 1.35f,
                                     0.975f, 0.6f, 0.25f, 0.15f, 0.3f, 0.6f, 0.9f, 0.975f, 1.05f, 1.2f, 1.35f, 1.05f };
static const float DRAG_BACK[24] = { 0.9f, 1.05f, 1.2f, 0.0f, 0.3f, 0.9f, 1.05f, 1.2f, 1.35f, 1.425f, 1.35f, 1.2f,
                                     0.975f, 0.75f, 0.525f, 0.075f, 0.0f, 0.3f, 0.45f, 0.675f, 1.35f, 1.2f, 1.35f, 1.05f };
static const float DRAG_SIDE[24] = { 0.9f, 1.05f, 0.45f, 0.15f, 0.0f, 0.9f, 1.05f, 1.2f, 1.425f, 1.5f, 1.425f, 1.35f,
                                     0.975f, 0.75f, 0.525f, 0.075f, 0.0f, 0.0f, 0.225f, 0.675f, 0.9f, 1.2f, 1.35f, 1.05f };

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
	// row 26: from the hold stance the legs walk, back off, strafe and start running (no turning on
	// the spot and no quick turn: the stance turns itself)
	[MV_HOLD] = B(MV_BACK) | B(MV_WALK) | B(MV_LSWALK) | B(MV_RSWALK) | B(MV_RUN1) | B(MV_LSRUN) | B(MV_RSRUN) |
	            B(MV_HOLD),
};

static const char* const NAMES[MV_COUNT] = { "stand", "relax", "tired", "lround", "rround", "back", "walk",
                                             "lswalk", "rswalk", "run1", "run2", "run3", "lsrun", "rsrun",
                                             "lturn", "rturn", "alert", "hold" };

static struct {
	int state, prev;
	float spd;            // m/s, the PC's 0xa8
	float off;            // movement direction relative to the facing (0xb4)
	float stam;           // 60 Hz refreshes run (0x1fbacf4)
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

static bool s_inf_stamina;      // the Cheats page: running never tires
void move_set_infinite_stamina(bool on) { s_inf_stamina = on; }

static bool running(int s) { return s >= MV_RUN1 && s <= MV_RSRUN; }
static bool moving(int s) { return s >= MV_BACK && s <= MV_RSRUN; }

bool move_can_aim(void)
{
	int s = M.state;
	return s == MV_STAND || s == MV_RELAX || s == MV_TIRED || s == MV_ALERT || s == MV_LROUND || s == MV_RROUND ||
	       s == MV_HOLD;
}

int move_pc_status(int s)
{
	// _JAMES_LOWER_STATUS: 0 stand, 1 relax, 2 alert, 3 tired, 6 / 7 turning on the spot, 8 back,
	// 9 walk, 10 / 11 strafe, 12..14 run, 15 / 16 strafe at a run, 18 / 19 the quick turn, 0x1a hold
	static const u8 pc[MV_COUNT] = { 0, 1, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 2, 0x1a };
	return s >= 0 && s < MV_COUNT ? pc[s] : 0;
}

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
	if (s_inf_stamina)
		M.stam = 0.0f;               // Infinite Stamina: the run count never builds up

	// 0x52e110 reads the stick as bytes, -127..127. The 3D type's cross dead zone: an axis tilted
	// less than half the other (at most 64) reads 0, and past 31 the turn counts from that half.
	// The turn is full at 85, the forward tilt at 100 (x 0.01). The 2D type (control 1) skips the
	// cross. Aiming with the port's behind camera keeps the 3D reading: that camera turns with James
	// while he aims (tpcam.c, 3DS-only), so a direction taken from it would chase itself.
	bool tank = in->control == MOVE_TANK || (in->aim && in->cam_live);
	float px = in->x * 127.0f, py = in->y * 127.0f;
	float ax = fminf(fabsf(px), 127.0f), ay = fminf(fabsf(py), 127.0f);
	float base = 0.0f;
	if (tank) {
		float dzx = fminf(floorf(ay * 0.5f), 64.0f), dzy = fminf(floorf(ax * 0.5f), 64.0f);
		if (ax < dzx) ax = 0.0f;
		if (ay < dzy) ay = 0.0f;
		base = dzx > 31.0f ? dzx : 0.0f;
	}
	float tx = ax > 0.0f && ax >= base ? (fminf(ax, 85.0f) - base) / (85.0f - base) : 0.0f;
	float ty = fminf(ay, 100.0f) * 0.01f;
	if (in->digital) {
		// 0x52e110 with a direction key held (0x1fbac6a 0): the keys turn at the full rate (0x1fbab24
		// is -1, 0 or 1) and walk at no tilt; the movers ramp the speed themselves (0x54bd70)
		tx = tx > 0.0f ? 1.0f : 0.0f;
		ty = ty > 0.0f ? 1.0f : 0.0f;
	}
	int turn = tx > 0.0f ? (in->x > 0.0f ? 1 : -1) : 0;    // +1 right
	bool fwd = ty > 0.0f && in->y > 0.0f, back = ty > 0.0f && in->y < 0.0f;
	bool run = in->run && !in->drag;           // 0x52e110: no running with the great knife
	bool sl = in->strafe_l, sr = in->strafe_r;
	if (in->aim && in->locked && in->stick_strafe && turn) {
		// the port's behind camera (3DS-only): aiming at a lock, the circle pad's x strafes
		sl = turn < 0, sr = turn > 0;
		turn = 0;
	}

	bool dir_move = false;      // the 2D type locked on goes the stick's own way (0x548bf0)
	float dir_yaw = 0.0f;
	if (!tank) {
		// Directional Style, the PC's 2D type (0x547bf0, 0x546760, 0x548bf0): the stick's direction
		// on screen, taken from the camera James saw when he pushed it. After a camera cut that direction holds
		// until the stick turns more than 30 degrees or comes back to the middle, so he does not
		// spin round at the cut. He turns towards it at 20 x 0.3235 of the difference a second
		// and walks or runs along the way he faces, as fast as the stick is tilted.
		// 0x52dd20: the tilt is the length of the two axes read as above, at most 1
		float mx = in->x, my = in->y;
		float mag = fminf(sqrtf(fminf(ax, 85.0f) / 85.0f * (fminf(ax, 85.0f) / 85.0f) + ty * ty), 1.0f);
		if (in->digital && mag > 0.0f)
			mag = 1.0f;                // 0x52dd20: the keys' direction at full length
		turn = 0;
		back = false;
		tx = 0.0f;
		fwd = mag > 0.2f;
		if (!fwd) {
			M.cam_locked = false;
		} else {
			float sa = atan2f(mx, my);
			// the port's behind camera turns without cuts: its direction is taken every frame
			if (!M.cam_locked || in->cam_live || fabsf(wrap(sa - M.stick_at_lock)) >= 0.5236f) {
				M.cam_lock = in->camera_yaw;
				M.stick_at_lock = sa;
				M.cam_locked = true;
			}
			float want = M.cam_lock + sa, d = wrap(want - *yaw);
			ty = mag;
			if (in->aim && in->locked) {
				// 0x547bf0 with a target locked (0x1fbad07): the lock turns him (combat.c), and the
				// stick's way against his facing picks the gait (0x1fbab25): within 45 degrees of it
				// forward, past 135 back, else the strafe on that side. He goes the stick's way.
				dir_move = true;
				dir_yaw = want;
				if (fabsf(d) >= 2.3561945f)
					fwd = false, back = true;
				else if (d < -0.7853982f)
					fwd = false, sl = true, sr = false;
				else if (d >= 0.7853982f)
					fwd = false, sl = false, sr = true;
			} else {
				// 0x546760: unlocked, the weapon up or not, he turns to the stick's way; the 2D hold
				// stance (0x547a30, 0x558200) has no turn of its own
				float k = 20.0f * 0.32345787f * dt;
				*yaw += d * (k > 1.0f ? 1.0f : k);
			}
		}
	}

	// 0x557960: with the weapon up the legs stand in the hold stance whenever their row lets them
	// (the standing, idling and turning states); walking and running go on until they stop
	if (in->aim && !moving(M.state) && M.state != MV_LTURN && M.state != MV_RTURN)
		enter(MV_HOLD);
	else if (!in->aim && M.state == MV_HOLD)
		enter(MV_STAND);
	// 0x54e0f0: the state for this frame. Under the weapon (0x1fba8e4 0x1a or 0x1c) the gaits also
	// need the weapon's flags (combat_legs: 4 / 0x40 to walk, 8 / 0x80 to run, any run before the
	// first attack), and the quick turn waits for no attack since raising (0x1fbad16 == 0xff).
	bool can_walk = !in->aim || in->aim_walk;
	bool can_run = (!in->aim || in->aim_run) && run && !in->wall_ahead;
	bool quick = in->turn180 || (sl && sr && (in->strafe_l_down || in->strafe_r_down));
	int want = -1;
	if (in->autowalk) {
		// 0x53b010's gaits. Which lower state the PC's run takes is not traced: the run clip
		// whose pace the speed has reached (as the earned paces of 0x54cbb0 go)
		if (in->auto_gait == 2)
			want = M.spd >= 3.5f ? MV_RUN2 : MV_RUN1;
		else if (in->auto_gait == 1)
			want = MV_WALK;
		else
			want = MV_STAND;
	} else if (M.state == MV_LTURN || M.state == MV_RTURN) {
		want = M.state;
	} else if (in->keep_gait && !moving(M.state)) {
		// 0x54e0f0 / 0x547bf0 under an attack (upper status 0x1c) leave the lower state as it is:
		// standing in the stance he stays, walking or running he goes on while the weapon's flags
		// allow (0x6cf7cc 0x40 walking, 0x80 running); a gait they stop is chosen again below
		want = MV_HOLD;
	} else if (in->keep_gait && moving(M.state) && (running(M.state) ? in->aim_run : in->aim_walk)) {
		want = M.state;
	} else if (quick && can(MV_LTURN) && !(in->aim && in->attack_seq)) {
		want = turn > 0 ? MV_RTURN : MV_LTURN;
		M.turned = 0.0f;
		M.turn_to = wrap(*yaw + PI);
	} else if (fwd) {
		if (can_run) {
			if (can(MV_RUN3) && in->place == 0) want = MV_RUN3;   // run3 out of doors only
			else if (can(MV_RUN2)) want = MV_RUN2;
			else if (can(MV_RUN1)) want = MV_RUN1;
		}
		// run held with no run to go to (0x54e76e): no walking instead while no attack has been made
		bool skip = run && !in->wall_ahead && !(in->aim && in->attack_seq);
		if (want < 0 && !skip && can(MV_WALK) && can_walk)
			want = MV_WALK;
	} else if (back && can(MV_BACK) && can_walk) {
		want = MV_BACK;
	} else if (sl != sr) {
		int w = sl ? MV_LSWALK : MV_RSWALK, r = sl ? MV_LSRUN : MV_RSRUN;
		if (can_run && can(r)) want = r;
		else if (can(w) && can_walk) want = w;
	}
	if (want < 0 && turn && !back && !in->aim && can(turn < 0 ? MV_LROUND : MV_RROUND))
		want = turn < 0 ? MV_LROUND : MV_RROUND;
	if (want < 0 && in->aim) {
		want = MV_HOLD;                 // 0x557960: stopped under the weapon, the stance
	} else if (want < 0) {
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

	// the state's mover; pace is the speed James covers ground at (0xac) when it is not the gait's
	float maria = in->maria ? 0.8f : 1.0f, pace = -1.0f;
	bool lock_run = in->aim && in->locked && run;
	int drag_i = (int)(M.frame - M.clip->start);
	drag_i = drag_i < 0 ? 0 : drag_i > 23 ? 23 : drag_i;
	float off_to = 0.0f, rate = 0.0f;
	float spd_in = M.spd;            // a program's walk sets the speed itself, below
	switch (M.state) {
	case MV_STAND: case MV_RELAX: case MV_TIRED: case MV_ALERT: {
		M.still += dt;
		M.stam -= dt * 60.0f;
		if (M.stam < 0.0f) M.stam = 0.0f;
		M.spd -= (running(M.prev) ? 20.0f : 10.0f) * dt;      // 0x5567b0
		if (M.spd < 0.0f) M.spd = 0.0f;
		off_to = M.off;
		break;
	}
	case MV_HOLD:
		// 0x54dcc0 (lower status 0x1a): the stance slows like standing (0x5567b0) and turns with the
		// stick at 2.4 rad/s x tilt, 1.5 x that with run held, while nothing is locked
		M.spd -= (running(M.prev) ? 20.0f : 10.0f) * dt;
		if (M.spd < 0.0f) M.spd = 0.0f;
		rate = 2.4f * tx * (in->run ? 1.5f : 1.0f);
		off_to = M.off;
		break;
	case MV_LROUND: case MV_RROUND:
		M.spd -= 10.0f * dt;
		if (M.spd < 0.0f) M.spd = 0.0f;
		rate = in->drag ? 1.0f * tx : 2.4f * tx * (run ? 1.5f : 1.0f);   // 0x54bbb0 grip 8
		off_to = M.off;
		break;
	// the circle pad takes the PC's analog branches (the tilt sets the walking pace at once and the
	// running pace James speeds up or slows down to), the D-Pad the key branches (their ramps)
	case MV_WALK:
		if (in->drag) {
			M.spd = DRAG_WALK[drag_i];
		} else if (in->digital) {
			// 0x54bd70 / 0x546aa0 with the keys: from backing off he first slows at 7.5 m/s/s, then
			// speeds up at 2.5 (or slows at 5) to 1.5 m/s
			if (M.spd > 0.0f && M.prev == MV_BACK)
				M.spd = fmaxf(M.spd - 7.5f * dt, 0.0f);
			else
				ramp(1.5f, 2.5f, 5.0f, dt);
		} else {
			M.spd = ty * 1.5f;
		}
		rate = (in->drag ? 0.875f : 1.9f) * tx * (in->place == 2 ? 1.3f : 1.0f);   // 0x54bd70
		if (lock_run)
			pace = fminf(M.spd * 1.5f, 2.25f);
		break;
	case MV_BACK:
		if (in->drag) {
			M.spd = DRAG_BACK[drag_i];
		} else if (in->digital) {
			// 0x54c0e0 / 0x546cc0 with the keys: out of another gait he first stops (10 m/s/s), then
			// speeds up or slows at 5 m/s/s to 1.2 m/s
			if (M.spd > 0.0f && M.prev != MV_BACK) {
				M.spd -= 10.0f * dt;
				if (M.spd <= 0.0f)
					M.spd = 0.0f, M.prev = MV_BACK;
			} else {
				ramp(1.2f, 5.0f, 5.0f, dt);
			}
		} else {
			M.spd = ty * 1.2f * (!tank && run ? 1.5f : 1.0f);   // 0x546cc0: the 2D type's 1.5 x with run
		}
		rate = (in->drag ? 0.65f : 1.3f) * tx;
		off_to = PI;
		if (run)
			pace = fminf(M.spd * 1.5f, 1.8f);   // 0x54c0e0, 0x546cc0: run held, locked or not
		break;
	case MV_LSWALK: case MV_RSWALK:
		if (in->drag)
			M.spd = DRAG_SIDE[drag_i];
		else if (M.spd > 0.0f && M.prev == MV_BACK && (tank || in->digital))
			M.spd = fmaxf(M.spd - 5.1f * dt, 0.0f);   // 0x54c470 / 0x546ea0: out of backing off he slows first
		else if (!tank && !in->digital)
			M.spd = ty * 1.3f;                         // 0x546ea0: the 2D type's stick sets it
		else
			ramp(1.3f, 1.7f, 3.4f, dt);
		rate = 1.9f * tx;
		off_to = M.state == MV_LSWALK ? -PI / 2.0f : PI / 2.0f;   // +yaw is to the right
		if (tank ? lock_run : run)
			pace = fminf(M.spd * 1.5f, tank ? 1.8f : 1.95f);   // 0x54c470 (locked), 0x546ea0
		break;
	case MV_RUN1: case MV_RUN2: case MV_RUN3: {
		static const float top[3] = { 3.5f, 4.0f, 4.8f };
		float t = top[M.state - MV_RUN1] * ty;
		if (M.state == MV_RUN1 && t < 2.0f)
			t = 2.0f;
		ramp(t, 3.0f, 3.0f, dt);
		rate = 1.7f * tx;
		if (M.state != MV_RUN1)
			M.stam += dt * 60.0f;     // 0x54cbb0 (run1) counts none
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
		if (!tank && !in->digital)
			M.spd = ty * 3.8f * (in->place == 1 ? 0.9f : 1.0f);   // 0x547700: the 2D type's stick
		else
			ramp(3.8f, 6.0f, 6.0f, dt);
		rate = 1.7f * tx;
		M.stam += dt * 60.0f;
		off_to = M.state == MV_LSRUN ? -PI / 2.0f : PI / 2.0f;
		break;
	case MV_LTURN: case MV_RTURN: {
		M.spd = 0.0f;
		float step = (in->drag ? 4.712389f : 5.4977875f) * dt;   // 0x54f160: slower with the great knife
		M.turned += step;
		if (M.turned >= PI) {
			*yaw = M.turn_to;
			enter(MV_STAND);
		} else {
			*yaw += M.state == MV_LTURN ? -step : step;
		}
		off_to = 0.0f;
		M.off = 0.0f;
		break;
	}
	}

	if (in->autowalk) {
		// 0x53ae70 walking: the speed eases to 1.5 m/s (2.5 up, 5 down), 0x53af60 running: up at 3
		// to 4 m/s, 0x5567b0 stopping (10 m/s/s, 20 out of a run); 0x5343e0 turns him to the point
		// by 0.3234579 of the difference x 12 (walking) or 8 (running) a second
		if (in->auto_gait)
			M.spd = spd_in;
		if (in->auto_gait == 1)
			ramp(1.5f, 2.5f, 5.0f, dt);
		else if (in->auto_gait == 2)
			M.spd = fminf(M.spd + 3.0f * dt, 4.0f);
		if (in->auto_gait) {
			float k = 0.32345787f * dt * (in->auto_gait == 2 ? 8.0f : 12.0f);
			*yaw += wrap(in->auto_yaw - *yaw) * (k > 1.0f ? 1.0f : k);
		}
		ty = M.spd / 1.5f;                // the walk clip keeps pace with him (the tilt's place)
		pace = -1.0f;
		off_to = 0.0f;
	}
	if (s_inf_stamina)
		M.stam = 0.0f;
	// a locked target turns him instead (combat.c, 0x5568c0): the movers leave the yaw alone
	if (tank && turn && !(in->aim && in->locked))
		*yaw += (float)turn * rate * dt;      // right turns raise the yaw: forward is (sin, cos) and +x is screen right
	// the direction of travel eases between the gaits' offsets (0x54ef10, 4 rad/s)
	if (!tank && in->aim && in->locked) {
		// 0x548bf0: the 2D type locked on moves along the stick's way (0xb4), or along his facing
		// once the stick is let go (0x547bf0)
		M.off = dir_move ? wrap(dir_yaw - *yaw) : 0.0f;
	} else if (moving(M.state) && moving(M.prev)) {
		float d = wrap(off_to - M.off), s = 4.0f * dt;
		M.off += d > s ? s : d < -s ? -s : d;
	} else if (moving(M.state)) {
		M.off = off_to;
	}
	*yaw = wrap(*yaw);

	float v = (pace >= 0.0f ? pace : M.spd) * UNIT * maria, a = *yaw + M.off;
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
	// 0x538e00 (part 2, +0x244): the movers add a rate to the legs' clip, which 0x551820 steps by
	// (speed + rate) x the 60 Hz refreshes / 4096 frames, so the rate is in the clip speed's units.
	// With the stick the walk and the back-off slow with the tilt by a fixed share, not in
	// proportion: 0x54bd70 (1 - tilt) x -896, 0x54c0e0 x -1024; the 2D type's movers by the stick's
	// length, 0x546aa0 walk -896, 0x546cc0 back -512, 0x546ea0 / 0x547050 strafes -1001 / -1024,
	// 0x547700 strafe runs -1536. Run held with a target locked adds 512 with the keys (and to the
	// 3D strafes, 0x54c470 / 0x54c810); with the stick 0x54bd70 adds (1 - tilt) x 512, 0x54c0e0
	// tilt x 512, 0x546aa0 length x 512 (0x546cc0's own sum is left out: it takes the turn key's
	// byte x 10). A program's walk and the great knife keep the clip in step with the tilt as before.
	float arate = 0.0f, fps;
	if (in->autowalk || in->drag) {
		fps = c->speed / 2048.0f * 30.0f;
		if (M.state == MV_WALK || M.state == MV_BACK)
			fps *= ty;
	} else {
		float slack = 1.0f - (ty > 1.0f ? 1.0f : ty);
		bool strafe = M.state == MV_LSWALK || M.state == MV_RSWALK;
		if (in->digital) {
			if (lock_run && (M.state == MV_WALK || M.state == MV_BACK || strafe))
				arate = 512.0f;
		} else if (tank) {
			if (M.state == MV_WALK)
				arate = slack * -896.0f + (lock_run ? slack * 512.0f : 0.0f);
			else if (M.state == MV_BACK)
				arate = slack * -1024.0f + (lock_run ? ty * 512.0f : 0.0f);
			else if (strafe && lock_run)
				arate = 512.0f;
		} else {
			if (M.state == MV_WALK)
				arate = slack * -896.0f + (lock_run ? ty * 512.0f : 0.0f);
			else if (M.state == MV_BACK)
				arate = slack * -512.0f;
			else if (M.state == MV_LSWALK)
				arate = slack * -1001.0f;
			else if (M.state == MV_RSWALK)
				arate = slack * -1024.0f;
			else if (M.state == MV_LSRUN || M.state == MV_RSRUN)
				arate = slack * -1536.0f;
		}
		fps = (c->speed + arate) / 2048.0f * 30.0f;
		if (fps < 0.0f)
			fps = 0.0f;
	}
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
		else if (moving(M.state)) a = in->drag ? 3 : 5, b2 = in->drag ? 14 : 17;   // grip 8: 3 and 14
		float r0 = f0 - c->start, r1 = M.frame - c->start;
		for (int i = 0; i < 2 && a >= 0; i++) {
			float k = i ? (float)b2 : (float)a;
			bool hit = rev ? (r1 <= k && r0 > k) || (r0 < r1 && (r1 <= k || r0 > k))
			               : (r0 < k && r1 >= k) || (r1 < r0 && (r0 < k || r1 >= k));
			if (hit && !out->clip_changed)
				out->step = true;
			// the great knife scrapes along on 5 and 18 of the walking clips (0x549e20)
			float g = i ? 18.0f : 5.0f;
			if (in->drag && moving(M.state) && !out->clip_changed &&
			    ((r0 < g && r1 >= g) || (r1 < r0 && (r0 < g || r1 >= g))))
				out->scrape = true;
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
