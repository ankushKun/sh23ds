#include "radio.h"
#include "enemy.h"
#include "events.h"
#include "sfx.h"
#include <math.h>
#include <string.h>

// RadioNoise (0x48cbc0) and what it calls, as the PC runs it. Global addresses are the PC's.
//   0x1f7d580 s_active    the static is running (the four loops are started)
//   0x1f7d584 s_mode      RADIO_ENEMIES .. RADIO_FORCED (set by stage programs)
//   0x1f7d588 s_lvl[4]    each track's level now, eased toward its target
//   0x1f7d5a8 s_pan[4]    each track's pan now (-1 .. 1), eased
//   0x1f7d5c8 s_duck      the volume factor of the map, the item screen and the memos, eased
//   0x1f7d5cc s_point     mode 1's target
// The tracks are the PC's four DirectSound buffers; a track's volume is the byte 0x514400 hands to
// 0x518320 (twice it, 0..255, a DirectSound level of 20 log10(v / 255) dB), its pan the byte
// 64 - pan * 32 (0..127, 64 centre) that 0x517a00 turns into a source position.

static bool s_active;
static int s_mode;
static float s_lvl[4], s_pan[4], s_duck;
static float s_point[3];
static float s_retry;               // seconds before the loops are asked for again after a failed start

// ---------------------------------------------------------------- the radio's state (saved)

// 3DS-only: the PC keeps the switch and the volume in its save block (0x1f7d407 / 0x1f7d408); here
// they ride in the last slot of the saved item counts (events.c), which no item uses: bit 7 says
// they were set, bit 4 is the switch, bits 0..3 the volume. 0 (a new game, an old save) is the
// PC's start: on, volume 10 (0x49e9f0).
static int state_get(void) { return ev_radio_state(); }

bool radio_is_on(void)
{
	int s = state_get();
	return s ? (s >> 4 & 1) != 0 : true;
}

int radio_volume(void)
{
	int s = state_get();
	return s ? s & 15 : 10;
}

void radio_set_on(bool on) { ev_radio_state_set(0x80 | (on ? 0x10 : 0) | radio_volume()); }

void radio_set_volume(int volume)
{
	ev_radio_state_set(0x80 | (radio_is_on() ? 0x10 : 0) | (volume < 0 ? 0 : volume > 15 ? 15 : volume));
}

void radio_set_mode(int mode) { s_mode = mode >= RADIO_ENEMIES && mode <= RADIO_FORCED ? mode : RADIO_ENEMIES; }
int radio_mode(void) { return s_mode; }

void radio_set_point(const float pos[3]) { memcpy(s_point, pos, sizeof s_point); }

// ---------------------------------------------------------------- enemies

// The PC's enemy kind bytes (the index of the control table at 0x4a88e0), which the radio's table is
// indexed by. The creeper nests are 13 and 14 there (the same row as the creepers').
static int pc_type(const Enemy* e)
{
	switch (e->kind) {
	case EN_KIND_SCU: return 1;
	case EN_KIND_MKN: return 2;
	case EN_KIND_TYU: return 3;
	case EN_KIND_RED: return 4;
	case EN_KIND_ONI: return 5;
	case EN_KIND_NSE: return 6;
	case EN_KIND_IKE: return 7;
	case EN_KIND_PAP: return 8;
	case EN_KIND_EDI: return 9;
	case EN_KIND_ARM: return 10;
	case EN_KIND_BOS: return 11;
	case EN_KIND_NEST: return 13;
	default: return 0;
	}
}

// 0x88cce0: per kind 4 gains and 4 distances, one row of 32 bytes each (kinds 0..15).
static const float TABLE[16][8] = {
	{ 0 },
	{ 1.0f, 1.0f, 0.6f, 0.8f, 12500.0f, 5000.0f, 1250.0f, 7500.0f },
	{ 0.9f, 0.8f, 1.0f, 0.5f, 14000.0f, 6000.0f, 1500.0f, 5000.0f },
	{ 0.5f, 0.2f, 0.5f, 0.0f, 7500.0f, 1500.0f, 2500.0f, 0.0f },
	{ 0.5f, 0.5f, 0.5f, 1.0f, 10000.0f, 9000.0f, 8000.0f, 10000.0f },
	{ 0.3f, 0.3f, 0.3f, 1.0f, 10000.0f, 10000.0f, 10000.0f, 10000.0f },
	{ 1.0f, 0.8f, 1.0f, 0.0f, 8000.0f, 6000.0f, 4000.0f, 0.0f },
	{ 0 },
	{ 0.8f, 1.0f, 0.8f, 0.8f, 15000.0f, 7500.0f, 2500.0f, 7500.0f },
	{ 0 },
	{ 0.4f, 0.8f, 1.0f, 0.5f, 5000.0f, 3500.0f, 1500.0f, 5000.0f },
	{ 0 },
	{ 0 },
	{ 0.5f, 0.2f, 0.5f, 0.0f, 7500.0f, 1500.0f, 2500.0f, 0.0f },
	{ 0.5f, 0.2f, 0.5f, 0.0f, 7500.0f, 1500.0f, 2500.0f, 0.0f },
	{ 0 },
};

// 0x4a6ce0: how much an enemy counts for the radio (EnLOCAL +0x94). 0 leaves it out.
static float radio_factor(const Enemy* e, int type)
{
	if (!e->used)
		return 0.0f;
	if (type == 4)
		return e->type == 0 && e->ctrl == 1 ? 1.0f : 0.0f;       // Pyramid Head, only the plain one
	if (type == 5 || type == 7 || type == 9 || type == 11)
		return 0.0f;                                             // the spear one, Flesh Lip, Eddie, Mary
	if (type == 3 && e->type != 6)
		return 0.5f;
	return enemy_alive(e) ? 1.0f : 0.8f;                         // a body (+0x148 bit 2) counts less
}

// What an enemy adds to the crowd that decides whether the last track sounds (0x48cbc0 switch).
static int crowd_weight(int type)
{
	switch (type) {
	case 1: case 2: case 6: return 3;
	case 3: case 13: case 14: return 1;
	case 4: case 5: return 9;
	case 8: case 10: return 6;
	default: return 0;
	}
}

// Moves cur toward goal by at most step (the PC's FUN_0048ae60 / FUN_00485a20 pairs).
static float ease(float cur, float goal, float step)
{
	if (goal > cur) {
		cur += step;
		return cur > goal ? goal : cur;
	}
	if (goal < cur) {
		cur -= step;
		return cur < goal ? goal : cur;
	}
	return cur;
}

// Mode 0: the static of the enemies around James; fills the four tracks' target levels and pans.
static void enemy_targets(const RadioIn* in, float tl[4], float tp[4])
{
	// the PC's two 128-float arrays (0x200 bytes each): 32 enemies by 4 tracks. The port has at most
	// ENEMY_MAX, and the entry 5k read below stays inside them.
	float L[128], P[128];
	_Static_assert(ENEMY_MAX * 5 <= 128, "radio arrays");
	memset(L, 0, sizeof L);
	memset(P, 0, sizeof P);
	const Enemy* list[ENEMY_MAX];
	float dist[ENEMY_MAX], fac[ENEMY_MAX];
	int types[ENEMY_MAX];
	int n = 0, weight = 0;
	for (int i = 0; i < enemy_count(); i++) {
		const Enemy* e = enemy_get(i);
		if (!e)
			continue;
		int type = pc_type(e);
		float f = type ? radio_factor(e, type) : 0.0f;
		if (f == 0.0f)
			continue;
		float dx = e->x - in->x, dy = e->y - in->y, dz = e->z - in->z;
		float d = sqrtf(dx * dx + dy * dy + dz * dz);       // +0x90, 0x4a88e0
		const float* row = TABLE[type];
		if (!(d <= row[4] || d <= row[5] || d <= row[6] || d <= row[7]))
			continue;
		if (d < row[7])
			weight += crowd_weight(type);
		list[n] = e, dist[n] = d, fac[n] = f, types[n] = type;
		n++;
	}
	for (int k = 0; k < n; k++) {
		const float* row = TABLE[types[k]];
		float pos[3] = { list[k]->x, list[k]->y, list[k]->z };
		float pan = (float)sfx_source_angle(pos) * (1.0f / 90.0f);      // 0x515ab0 / 90
		for (int j = 0; j < 4; j++) {
			if ((j == 3 && weight < 9) || row[4 + j] < dist[k]) {
				L[k * 4 + j] = 0.0f;
				continue;
			}
			float f = dist[k] >= 1.5f ? cosf((dist[k] - 1.5f) / (row[4 + j] - 1.5f) * 1.5707964f) : 1.0f;
			L[k * 4 + j] = row[j] * fac[k] * f;
			P[k * 4 + j] = pan;
		}
	}
	for (int j = 0; j < 4; j++) {
		// the loudest first in this track
		for (int a = 0; a < n - 1; a++)
			for (int b = a + 1; b < n; b++)
				if (L[a * 4 + j] < L[b * 4 + j]) {
					float t = L[a * 4 + j];
					L[a * 4 + j] = L[b * 4 + j], L[b * 4 + j] = t;
					t = P[a * 4 + j];
					P[a * 4 + j] = P[b * 4 + j], P[b * 4 + j] = t;
				}
		// each further one counts a third of the one before it, and the sum stops once they are
		// quiet. The PC adds the entry 5k of its array (4k + k, which the decompiler shows, not 4k + j)
		// and so does this.
		float scale = 1.0f;
		for (int k = 1; k < n; k++) {
			scale *= 1.0f / 3.0f;
			float a = L[k * 4 + j] * scale, b = P[k * 4 + j] * scale;
			L[k * 4 + j] = a, P[k * 4 + j] = b;
			if (4 * k > 8 && a < 0.1f)
				break;
			L[j] += L[5 * k];
			P[j] += P[5 * k];
		}
		tl[j] = L[j] > 1.0f ? 1.0f : L[j];
		tp[j] = P[j] > 1.0f ? 1.0f : P[j] < -1.0f ? -1.0f : P[j];
	}
}

// ---------------------------------------------------------------- output

// 0x514400 -> 0x518320 -> 0x518140: the track's volume byte (0..127) doubles to 0..255 and sets the
// DirectSound level, a gain of v / 255; the pan byte moves the buffer left or right of the listener
// by (p - 64) / 128 on a metre in front of it. The port's 1.0 is the PC's full level (sfx.c).
static void track_out(int track, int vol, int pan)
{
	int v = vol * 2;
	v = v < 0 ? 0 : v > 255 ? 255 : v;
	int p = pan < 0 ? 0 : pan > 127 ? 127 : pan;
	sfx_radio_set(track, (float)v / 255.0f, (float)(64 - p) / 128.0f);
}

static void output(int first, int count, bool with_volume)
{
	for (int i = first; i < first + count; i++) {
		int pan = (int)(64.0f - s_pan[i] * 32.0f);
		float v = s_lvl[i] * s_duck;
		if (with_volume)
			v *= (float)radio_volume() * (1.0f / 15.0f);
		track_out(i, (int)(v * 127.0f), pan);
	}
}

void radio_stop(void)
{
	if (s_active)
		sfx_radio_stop();
	s_active = false;
}

static float duck_goal(int duck) { return duck == 2 ? 0.25f : duck == 1 ? 0.5f : 1.0f; }

void radio_update(const RadioIn* in, float dt)
{
	// the PC stops the radio outside the game, on its options / movie, save and result screens, with
	// the switch off (except mode 3), and in mode 0 without the radio item
	bool off = !in->playing || in->silent || (!radio_is_on() && s_mode != RADIO_FORCED) ||
	           (s_mode == RADIO_ENEMIES && !in->has_radio);
	if (off) {
		radio_stop();
		return;
	}
	if (dt > 0.1f)
		dt = 0.1f;
	if (s_retry > 0.0f)
		s_retry -= dt;
	if (!s_active || !sfx_radio_running()) {
		if (!s_active) {
			memset(s_lvl, 0, sizeof s_lvl);
			memset(s_pan, 0, sizeof s_pan);
			s_duck = 0.0f;
			s_active = true;
		}
		if (s_retry <= 0.0f) {
			sfx_radio_start();             // also after sfx_stop_all (a movie, a menu sound reset)
			if (!sfx_radio_running())
				s_retry = 2.0f;
		}
	}

	switch (s_mode) {
	case RADIO_POINT: {
		// a fixed spot: louder the nearer James is to it, panned toward it (0x48cc81)
		float near = floorf(0.8f * sfx_distance_level(s_point) * 255.0f) / 255.0f;
		int ang = sfx_source_angle(s_point);
		if (ang > 90)
			ang = 180 - ang;
		if (ang < -90)
			ang = -180 - ang;
		s_lvl[0] = TABLE[1][0] * near;
		s_lvl[1] = near * TABLE[1][1];
		s_pan[0] = s_pan[1] = (float)ang * (1.0f / 90.0f);
		s_duck = ease(s_duck, duck_goal(in->duck), dt * 0.5f);
		output(0, 2, false);
		break;
	}
	case RADIO_STEADY:
		for (int i = 0; i < 2; i++) {
			s_lvl[i] = ease(s_lvl[i], 0.4f, dt * 0.4f);
			s_pan[i] = ease(s_pan[i], 0.0f, dt * 0.5f);
		}
		s_duck = 1.0f;
		output(0, 2, false);
		break;
	case RADIO_FORCED:
		s_duck = ease(s_duck, duck_goal(in->duck), dt * 0.5f);
		for (int i = 0; i < 4; i++)
			s_lvl[i] = ease(s_lvl[i], 1.0f, dt * 0.2f);
		for (int i = 0; i < 2; i++)
			s_pan[i] = 0.0f;
		output(0, 2, true);
		break;
	default: {
		float tl[4], tp[4];
		enemy_targets(in, tl, tp);
		for (int j = 0; j < 4; j++) {
			s_lvl[j] = ease(s_lvl[j], tl[j], dt * 0.4f);
			s_pan[j] = ease(s_pan[j], tp[j], dt * 0.5f);
		}
		s_duck = ease(s_duck, duck_goal(in->duck), dt * 0.5f);
		output(0, 4, true);
		break;
	}
	}
}
