#include "figures.h"
#include "font.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265f
#define MAX_V 3000                    // shape vertices a frame (two drawings and the explainer's bars)

static FontVert s_v[MAX_V];
static int s_nv;
static float s_alpha = 1.0f;          // fig_begin's fade
static bool s_measure;
static FILE* s_log;
static const char* s_ctx;
static int s_cuts;

// a drawing's place: its box at (s_ox, s_oy), faded by s_fa when its value is not the chosen one
static float s_ox, s_oy, s_fa = 1.0f;

static u32 fade(u32 rgba, float a)
{
	float k = (rgba & 0xff) * a * s_alpha;
	return (rgba & 0xffffff00u) | (u32)(k < 0.0f ? 0.0f : k > 255.0f ? 255.0f : k);
}

static void flush(void)
{
	if (s_nv && !s_measure)
		font_tris_px(NULL, s_v, s_nv);
	s_nv = 0;
}

// a triangle in screen pixels, a colour per corner
static void tri3(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float x2, float y2, u32 c2)
{
	if (s_measure)
		return;
	if (s_nv + 3 > MAX_V)
		flush();
	s_v[s_nv++] = (FontVert){ x0, y0, 0, 0, c0 };
	s_v[s_nv++] = (FontVert){ x1, y1, 0, 0, c1 };
	s_v[s_nv++] = (FontVert){ x2, y2, 0, 0, c2 };
}

// a quad with a colour per corner (top left, top right, bottom left, bottom right)
static void quad4(float x0, float y0, float x1, float y1, u32 tl, u32 tr, u32 bl, u32 br)
{
	tri3(x0, y0, tl, x0, y1, bl, x1, y1, br);
	tri3(x0, y0, tl, x1, y1, br, x1, y0, tr);
}

void fig_begin(float alpha)
{
	s_alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
	s_nv = 0;
}

void fig_end(void) { flush(); }

void fig_measure(FILE* log, const char* context)
{
	s_measure = true;
	s_log = log;
	s_ctx = context;
}

void fig_measure_end(void)
{
	s_measure = false;
	s_log = NULL;
}

int fig_cuts(void) { return s_cuts; }

void fig_rect(float x0, float y0, float x1, float y1, u32 rgba)
{
	u32 c = fade(rgba, 1.0f);
	quad4(x0, y0, x1, y1, c, c, c, c);
}

void fig_band(float x0, float y0, float x1, float y1)
{
	float f = (x1 - x0) * 0.18f;
	u32 c = fade(FIGC_BAND, 1.0f), o = c & 0xffffff00u;
	quad4(x0, y0, x0 + f, y1, o, c, o, c);
	quad4(x0 + f, y0, x1 - f, y1, c, c, c, c);
	quad4(x1 - f, y0, x1, y1, c, o, c, o);
}

// ---------------------------------------------------------------- text

// font.c's text space from top-screen pixels (font.h: x 0..512 over the 4:3 middle, y 16..496)
static float pcx(float x) { return 256.0f + (x - 200.0f) / 0.625f; }
static float pcy(float y) { return 256.0f + (y - 120.0f) / 0.46875f; }

static void put_text(int font, float x, float y, const char* s, int align, u32 rgba)
{
	if (!s_measure)
		font_text(font, pcx(x), pcy(y), s, align, rgba, rgba);
}

float fig_text(const char* s, float x, float y, float w, int max, bool big, int align, u32 rgba)
{
	FontFit f;
	if (!s || !*s)
		return 0.0f;
	if (!font_fit_px(s, w, max, big, &f)) {
		s_cuts++;
		if (s_log)
			fprintf(s_log, "cut %s: %s\n", s_ctx ? s_ctx : "?", s);
	}
	float lh = font_line_px(f.font);
	u32 c = fade(rgba, 1.0f);
	for (int i = 0; i < f.n; i++)
		put_text(f.font, x, y + i * lh, f.line[i], align, c);
	return f.n * lh;
}

// one character of s into one (a full-width code is five bytes), the next one returned
static const char* one_char(const char* s, char one[6])
{
	int n = *s == FONT_FULL_WIDTH ? 5 : 1, k = 0;
	for (; k < n && s[k]; k++)
		one[k] = s[k];
	one[k] = 0;
	return s + k;
}

void fig_title(const char* s, float y)
{
	if (!s || !*s)
		return;
	const float gap = 1.0f;
	char one[6];
	float w = 0.0f;
	for (const char* c = s; *c;) {
		c = one_char(c, one);
		w += font_width_px(FONT_BIG, one) + (*c ? gap : 0.0f);
	}
	if (w > 360.0f) {                    // a long name in another language: plain, then the small font
		fig_text(s, 200.0f, y, 380.0f, 1, true, FONT_CENTER, FIGC_WHITE);
		return;
	}
	float x = 200.0f - w * 0.5f;
	u32 shadow = fade(0x000000c0u, 1.0f), c = fade(FIGC_WHITE, 1.0f);
	for (const char* p = s; *p;) {
		p = one_char(p, one);
		put_text(FONT_BIG, x + 1, y + 1, one, FONT_LEFT, shadow);
		put_text(FONT_BIG, x, y, one, FONT_LEFT, c);
		x += font_width_px(FONT_BIG, one) + gap;
	}
	float mid = y + font_line_px(FONT_BIG) * 0.5f, l = 200.0f - w * 0.5f - 12.0f, r = 200.0f + w * 0.5f + 12.0f;
	u32 rule = fade(FIGC_RULE, 1.0f), none = rule & 0xffffff00u;
	if (l > 30.0f) {
		quad4(14, mid, l, mid + 1, none, rule, none, rule);
		quad4(r, mid, 386, mid + 1, rule, none, rule, none);
	}
}

// ---------------------------------------------------------------- shapes in a drawing's box

static float X(float x) { return s_ox + x; }
static float Y(float y) { return s_oy + y; }

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 rgba)
{
	u32 c = fade(rgba, s_fa);
	tri3(X(x0), Y(y0), c, X(x1), Y(y1), c, X(x2), Y(y2), c);
}

static void box(float x0, float y0, float x1, float y1, u32 rgba)
{
	tri(x0, y0, x0, y1, x1, y1, rgba);
	tri(x0, y0, x1, y1, x1, y0, rgba);
}

static void seg(float x0, float y0, float x1, float y1, float w, u32 rgba)
{
	float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
	if (l < 0.01f)
		return;
	float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
	tri(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny, rgba);
	tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, rgba);
}

static void frame(float x0, float y0, float x1, float y1, u32 rgba)
{
	box(x0, y0, x1, y0 + 1, rgba), box(x0, y1 - 1, x1, y1, rgba);
	box(x0, y0 + 1, x0 + 1, y1 - 1, rgba), box(x1 - 1, y0 + 1, x1, y1 - 1, rgba);
}

static void disc(float cx, float cy, float r, u32 rgba)
{
	const int n = 14;
	for (int i = 0; i < n; i++) {
		float a0 = i * 2.0f * PI / n, a1 = (i + 1) * 2.0f * PI / n;
		tri(cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, rgba);
	}
}

static void circ(float cx, float cy, float r, u32 rgba)
{
	const int n = 18;
	for (int i = 0; i < n; i++) {
		float a0 = i * 2.0f * PI / n, a1 = (i + 1) * 2.0f * PI / n;
		seg(cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, 1.0f, rgba);
	}
}

// facing a: 0 up the box, turning clockwise
static float dirx(float a) { return sinf(a); }
static float diry(float a) { return -cosf(a); }

// James as the map draws him: a red arrowhead with a pale edge
static void james(float x, float y, float a, float s)
{
	float px[4], py[4];
	const float k[4] = { 0.0f, 2.45f, PI, -2.45f }, r[4] = { 1.0f, 0.85f, 0.25f, 0.85f };
	for (int i = 0; i < 4; i++)
		px[i] = x + dirx(a + k[i]) * s * r[i], py[i] = y + diry(a + k[i]) * s * r[i];
	tri(px[0], py[0], px[1], py[1], px[2], py[2], FIGC_RED);
	tri(px[0], py[0], px[2], py[2], px[3], py[3], FIGC_RED);
	for (int i = 0; i < 4; i++)
		seg(px[i], py[i], px[(i + 1) & 3], py[(i + 1) & 3], 0.8f, FIGC_WHITE);
}

// a camera: a small body with its lens towards a
static void cam(float x, float y, float a, u32 rgba)
{
	float fx = dirx(a), fy = diry(a), sx = -fy, sy = fx;
	float c[4][2] = { { -4, -2 }, { 4, -2 }, { 4, 4 }, { -4, 4 } };   // across, back (lens at -y)
	float p[4][2];
	for (int i = 0; i < 4; i++)
		p[i][0] = x + sx * c[i][0] - fx * c[i][1], p[i][1] = y + sy * c[i][0] - fy * c[i][1];
	tri(p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1], 0x0b0a0aff);
	tri(p[0][0], p[0][1], p[2][0], p[2][1], p[3][0], p[3][1], 0x0b0a0aff);
	for (int i = 0; i < 4; i++)
		seg(p[i][0], p[i][1], p[(i + 1) & 3][0], p[(i + 1) & 3][1], 1.0f, rgba);
	float l0x = x + sx * -2.5f + fx * 2, l0y = y + sy * -2.5f + fy * 2, l1x = x + sx * 2.5f + fx * 2, l1y = y + sy * 2.5f + fy * 2;
	float l2x = x + sx * 3.5f + fx * 5, l2y = y + sy * 3.5f + fy * 5, l3x = x + sx * -3.5f + fx * 5, l3y = y + sy * -3.5f + fy * 5;
	tri(l0x, l0y, l1x, l1y, l2x, l2y, rgba);
	tri(l0x, l0y, l2x, l2y, l3x, l3y, rgba);
}

static float aim(float x, float y, float tx, float ty) { return atan2f(tx - x, -(ty - y)); }

// what a camera at (x, y) sees of (tx, ty): a pale wedge a little past it
static void cone(float x, float y, float tx, float ty, float half, float extra, float alpha)
{
	float a = aim(x, y, tx, ty), d = sqrtf((tx - x) * (tx - x) + (ty - y) * (ty - y)) + extra;
	u32 c = (FIGC_WHITE & 0xffffff00u) | (u32)(alpha * 255.0f);
	tri(x, y, x + dirx(a - half) * d, y + diry(a - half) * d, x + dirx(a + half) * d, y + diry(a + half) * d, c);
}

static void head(float x, float y, int dx, int dy, float s, u32 rgba)   // an arrowhead pointing dx, dy
{
	if (dx)
		tri(x + dx * s, y, x - dx * s * 0.6f, y - s, x - dx * s * 0.6f, y + s, rgba);
	else
		tri(x, y + dy * s, x - s, y - dy * s * 0.6f, x + s, y - dy * s * 0.6f, rgba);
}

static void ring_at(float cx, float cy, const char* label)
{
	fig_ring(X(cx), Y(cy), label, (FIGC_GREY & 0xffffff00u) | (u32)(255.0f * s_fa));
}

void fig_ring(float cx, float cy, const char* label, u32 rgba)
{
	if (!label || !*label)
		return;
	float ox = s_ox, oy = s_oy, fa = s_fa;
	s_ox = s_oy = 0.0f, s_fa = (rgba & 0xff) / 255.0f;
	u32 c = rgba | 0xff;
	float w = font_width_px(FONT_SMALL, label);
	if (strlen(label) > 1) {
		float hw = w * 0.5f + 5.0f;
		circ(cx - hw + 6, cy, 6.0f, c), circ(cx + hw - 6, cy, 6.0f, c);
		seg(cx - hw + 6, cy - 6, cx + hw - 6, cy - 6, 1.0f, c);
		seg(cx - hw + 6, cy + 6, cx + hw - 6, cy + 6, 1.0f, c);
	} else {
		circ(cx, cy, 6.5f, c);
	}
	put_text(FONT_SMALL, cx, cy - font_line_px(FONT_SMALL) * 0.5f - 0.5f, label, FONT_CENTER, fade(c, s_fa));
	s_ox = ox, s_oy = oy, s_fa = fa;
}

void fig_drop(float cx, float cy, float s, u32 fill, bool lit)
{
	float ox = s_ox, oy = s_oy, fa = s_fa;
	s_ox = s_oy = 0.0f, s_fa = lit ? 1.0f : 0.5f;
	disc(cx, cy + s * 0.3f, s, fill);
	tri(cx, cy - s * 1.5f, cx - s * 0.93f, cy, cx + s * 0.93f, cy, fill);
	if (lit) {
		const int n = 18;
		for (int i = 0; i <= n / 2; i++) {   // the lower rim lit, as the drop catches the light
			float a0 = i * PI / (n / 2), a1 = (i + 1) * PI / (n / 2);
			if (i < n / 2)
				seg(cx + cosf(a0) * s, cy + s * 0.3f + sinf(a0) * s, cx + cosf(a1) * s, cy + s * 0.3f + sinf(a1) * s,
				    1.0f, FIGC_GREY);
		}
	}
	s_ox = ox, s_oy = oy, s_fa = fa;
}

// ---------------------------------------------------------------- the drawings

// Camera: an L-shaped corridor seen from above with James walking it. Original: two fixed cameras
// frame him as he comes towards them and the shot cuts at the corner. Behind: one camera trails him.
static void camera(int v, float t)
{
	const float L1 = 66.0f, L2 = 118.0f;
	float k = fmodf(t, 7.2f), s = (k / 5.6f < 1.0f ? k / 5.6f : 1.0f) * (L1 + L2);
	float jx, jy, ja;
	if (s <= L1)
		jx = 34, jy = 94 - s, ja = 0.0f;
	else
		jx = 34 + (s - L1), jy = 28, ja = PI * 0.5f;
	seg(18, 104, 18, 12, 1, FIGC_RULE), seg(18, 12.5f, 170, 12.5f, 1, FIGC_RULE);
	seg(50, 104, 50, 44, 1, FIGC_RULE), seg(50, 44.5f, 170, 44.5f, 1, FIGC_RULE);
	if (v == 0) {
		const float cx[2] = { 44, 164 }, cy[2] = { 18, 36 };
		int on = s < 56.0f ? 0 : 1;
		for (int i = 0; i < 2; i++) {
			if (i == on)
				cone(cx[i], cy[i], jx, jy, 0.36f, 16, 0.12f);
			cam(cx[i], cy[i], aim(cx[i], cy[i], jx, jy), i == on ? FIGC_WHITE : FIGC_DIM);
		}
	} else {
		float b = s - 30.0f, cx, cy;
		if (b >= L1)
			cx = 34 + (b - L1), cy = 28;
		else
			cx = 34, cy = 94 - b > 100 ? 100 : 94 - b;
		cone(cx, cy, jx, jy, 0.36f, 18, 0.12f);
		cam(cx, cy, aim(cx, cy, jx, jy), FIGC_WHITE);
	}
	james(jx, jy, ja, 6);
}

// Movement: the same pushes of the Circle Pad (right, then up) with the camera at the bottom.
// Rotational turns James on the spot, then walks him the way he faces; Directional sends him
// right, then up the screen.
static void movement(int v, float t)
{
	const float dt = 1.0f / 60.0f;
	float end = fmodf(t, 6.4f), x = 74, y = 60, a = 0.0f;
	float trail[2][64];
	int nt = 0, step = 0;
	for (float q = 0.0f; q <= end; q += dt, step++) {
		int ix = q >= 0.6f && q < 1.6f, iy = q >= 1.9f && q < 3.5f;
		if (v == 1 && (ix || iy)) {
			float want = atan2f((float)ix, (float)iy), d = fmodf(want - a + PI * 3.0f, PI * 2.0f) - PI;
			float turn = 8.0f * dt;
			a += d > turn ? turn : d < -turn ? -turn : d;
			if (fabsf(d) < 0.6f)
				x += dirx(a) * 26 * dt, y += diry(a) * 26 * dt;
		} else if (v == 0) {
			a += ix * 1.6f * dt;
			if (iy)
				x += dirx(a) * 26 * dt, y += diry(a) * 26 * dt;
		}
		if (step % 6 == 0 && nt < 64)
			trail[0][nt] = x, trail[1][nt++] = y;
	}
	int ix = end >= 0.6f && end < 1.6f, iy = end >= 1.9f && end < 3.5f;
	tri(74, 98, 40, 8, 108, 8, 0xf0ece60a);
	cam(74, 98, 0.0f, FIGC_GREY);
	for (int i = 0; i < nt; i++)
		box(trail[0][i] - 0.8f, trail[1][i] - 0.8f, trail[0][i] + 0.8f, trail[1][i] + 0.8f, FIGC_LABEL);
	circ(150, 80, 12, FIGC_GREY);
	disc(150 + ix * 6.0f, 80 - iy * 6.0f, 5, FIGC_GREY);
	if (ix || iy)
		head(150 + ix * 18.0f, 80 - iy * 18.0f, ix, -iy, 3.0f, FIGC_RED);
	james(x, y, a, 6);
}

// Hold or press: the button's presses over time above what they do (the weapon up, the view
// ahead), a line passing over them.
static void hold(int v, float t, const char* key, bool look)
{
	const float x0 = 34, W = 168 - 34, yb = 34, ys = 66;
	float ph = fmodf(t, 4.6f) / 4.6f;
	const float hold_p[1][2] = { { 0.18f, 0.62f } }, tap_p[2][2] = { { 0.18f, 0.22f }, { 0.62f, 0.66f } };
	const float(*press)[2] = v == 0 ? hold_p : tap_p;
	int np = v == 0 ? 1 : 2;
	float act0 = 0.18f, act1 = v == 0 ? 0.62f : 0.64f;
	ring_at(16, yb, key);
	box(x0, yb - 0.5f, x0 + W, yb + 0.5f, FIGC_FAINT);
	if (look) {
		tri(12, ys - 4, 12, ys + 4, 22, ys, FIGC_GREY);
	} else {
		circ(16, ys, 4.5f, FIGC_GREY);
		box(15.5f, ys - 0.5f, 16.5f, ys + 0.5f, FIGC_GREY);
	}
	box(x0, ys - 2, x0 + W, ys + 2, FIGC_FAINT);
	for (int i = 0; i < np; i++) {
		float a = press[i][0], b = press[i][1];
		box(x0 + a * W, yb - 3, x0 + b * W, yb + 3, FIGC_DIM);
		if (ph > a)
			box(x0 + a * W, yb - 3, x0 + (ph < b ? ph : b) * W, yb + 3, FIGC_WHITE);
	}
	box(x0 + act0 * W, ys - 2, x0 + act1 * W, ys + 2, 0x3a1414ff);
	if (ph > act0)
		box(x0 + act0 * W, ys - 2, x0 + (ph < act1 ? ph : act1) * W, ys + 2, FIGC_RED);
	box(x0 + ph * W, yb - 10, x0 + ph * W + 1, ys + 10, 0xb4aea680);
}

// Messages: the two screens, the message on the top one or the touch screen.
static void screens(int v, float t)
{
	bool blink = (int)(t * 1.5f) % 2 == 0;
	box(52, 4, 124, 47, 0x141110ff), frame(52, 4, 124, 47, FIGC_GREY);
	box(59, 54, 117, 97, 0x141110ff), frame(59, 54, 117, 97, FIGC_GREY);
	const float hx[4] = { 53, 74, 92, 123 }, hy[4] = { 36, 26, 32, 22 };   // the game's picture: a skyline
	for (int i = 0; i < 3; i++) {
		tri(hx[i], hy[i], hx[i], 46, hx[i + 1], 46, 0x221d1bff);
		tri(hx[i], hy[i], hx[i + 1], 46, hx[i + 1], hy[i + 1], 0x221d1bff);
	}
	if (v == 1) {
		box(58, 31, 118, 44, 0x000000b0), frame(58, 31, 118, 44, FIGC_LABEL);
		box(62, 35, 102, 36.6f, FIGC_WHITE), box(62, 39, 92, 40.6f, FIGC_WHITE);
		if (blink)
			head(113, 39, 0, 1, 1.8f, FIGC_RED);
	} else {
		box(63, 60, 107, 61.8f, FIGC_WHITE), box(63, 65, 99, 66.8f, FIGC_WHITE), box(63, 70, 103, 71.8f, FIGC_WHITE);
		if (blink)
			ring_at(104, 88, "A");
	}
}

// Flashlight: James' beam from above, its reach to scale (the PC's 6000 units, the PS2's 8500)
// and its strength: the PC's bright, the PS2's soft.
static void beam(int v, float t)
{
	(void)t;
	float reach = v == 1 ? 140.0f : 99.0f, op = v == 1 ? 0.32f : 0.62f, mid = v == 1 ? 0.8f : 0.55f;
	for (float gx = 30; gx < 172; gx += 10)
		for (float gy = 10; gy < 96; gy += 10)
			box(gx - 0.6f, gy - 0.6f, gx + 0.6f, gy + 0.6f, FIGC_FAINT);
	float ax = 20, ay = 50, h = 0.33f;
	float mx = ax + reach * mid, my = reach * mid * h, ex = ax + reach, ey = reach * h;
	u32 c0 = (FIGC_WHITE & 0xffffff00u) | (u32)(op * 255), c1 = (FIGC_WHITE & 0xffffff00u) | (u32)(op * 0.35f * 255);
	u32 c2 = FIGC_WHITE & 0xffffff00u;
	c0 = fade(c0, s_fa), c1 = fade(c1, s_fa), c2 = fade(c2, s_fa);
	tri3(X(ax), Y(ay), c0, X(mx), Y(ay - my), c1, X(mx), Y(ay + my), c1);
	tri3(X(mx), Y(ay - my), c1, X(ex), Y(ay - ey), c2, X(ex), Y(ay + ey), c2);
	tri3(X(mx), Y(ay - my), c1, X(ex), Y(ay + ey), c2, X(mx), Y(ay + my), c1);
	box(ax, 83, ax + reach, 84, FIGC_LABEL);
	box(ax + 99, 80, ax + 100, 86, FIGC_LABEL);
	box(ax + 140, 80, ax + 141, 86, FIGC_LABEL);
	james(ax, ay, PI * 0.5f, 6);
}

// Cutscene View: one shot on the wide screen. Full Width shows more at the sides than the 4:3
// frame it was made for (marked); 4:3 comes closer so that frame fills the width.
static void framing(int v, float t)
{
	(void)t;
	const float sx = 13, sy = 6, sw = 150, sh = 90, k = v == 1 ? 1.25f : 1.0f, cx = sx + sw * 0.5f, cy = sy + sh * 0.5f + 4;
	box(sx, sy, sx + sw, sy + sh, 0x141110ff);
	float hl = cx - 90 * k, hr = cx + 90 * k;
	box(hl < sx ? sx : hl, cy + 10 * k, hr > sx + sw ? sx + sw : hr, cy + 10 * k + 1, FIGC_LABEL);
	float dx = cx + 30 * k, dy = cy - 22 * k;
	frame(dx, dy, dx + 14 * k, dy + 32 * k, FIGC_LABEL);
	disc(cx - 6 * k, cy - 30 * k, 5 * k, FIGC_GREY);
	box(cx - 11 * k, cy - 24 * k, cx - 1 * k, cy + 10 * k, FIGC_GREY);
	box(cx - 52 * k, cy - 12 * k, cx - 48 * k, cy + 10 * k, FIGC_DIM);
	frame(sx, sy, sx + sw, sy + sh, FIGC_GREY);
	if (v == 0) {
		float w43 = sh * 4.0f / 3.0f, l = cx - w43 * 0.5f, r = cx + w43 * 0.5f;
		box(sx + 1, sy + 1, l, sy + sh - 1, 0xc4161f1e), box(r, sy + 1, sx + sw - 1, sy + sh - 1, 0xc4161f1e);
		for (float y = sy + 1; y < sy + sh - 1; y += 6)
			box(l, y, l + 1, y + 3, FIGC_RED), box(r - 1, y, r, y + 3, FIGC_RED);
	}
}

// Map: the same floor opening zoomed in on James, or whole.
static void map(int v, float t)
{
	(void)t;
	const float k = v == 0 ? 2.1f : 1.0f, ox = 88, oy = 50, bx0 = 8, by0 = 4, bx1 = 168, by1 = 96;
	box(bx0, by0, bx1, by1, 0x1a1613ff);
	static const float rooms[9][4] = { { -60, -30, 30, 22 }, { -28, -30, 26, 22 }, { 0, -30, 34, 22 }, { 36, -30, 24, 22 },
	                                   { -60, -6, 120, 10 }, { -60, 6, 22, 24 }, { -36, 6, 30, 24 }, { -4, 6, 26, 24 },
	                                   { 24, 6, 36, 24 } };
	for (int i = 0; i < 9; i++) {
		float x0 = ox + rooms[i][0] * k, y0 = oy + rooms[i][1] * k, x1 = x0 + rooms[i][2] * k, y1 = y0 + rooms[i][3] * k;
		float cx0 = x0 < bx0 ? bx0 : x0, cx1 = x1 > bx1 ? bx1 : x1, cy0 = y0 < by0 ? by0 : y0, cy1 = y1 > by1 ? by1 : y1;
		if (cx0 >= cx1 || cy0 >= cy1)
			continue;
		if (i == 4)
			box(cx0, cy0, cx1, cy1, 0xc4161f30);
		if (y0 >= by0) box(cx0, y0, cx1, y0 + 0.8f, 0x6b5e50ff);
		if (y1 <= by1) box(cx0, y1 - 0.8f, cx1, y1, 0x6b5e50ff);
		if (x0 >= bx0) box(x0, cy0, x0 + 0.8f, cy1, 0x6b5e50ff);
		if (x1 <= bx1) box(x1 - 0.8f, cy0, x1, cy1, 0x6b5e50ff);
	}
	frame(bx0, by0, bx1, by1, FIGC_GREY);
	james(ox - 14 * k, oy - 1 * k, PI * 0.5f, 5);
}

// Stamina: what is left of it over a run (the run button held along the bottom). The PC's runs
// out and James walks until he gets it back; the infinite one stays full.
static void stamina(int v, float t, const char* key)
{
	const float x0 = 22, W = 166 - 22, top = 18, bot = 66;
	float ph = fmodf(t, 6.0f) / 6.0f;
	box(x0, top, x0 + W, top + 1, FIGC_FAINT), box(x0, bot, x0 + W, bot + 1, FIGC_FAINT);
	float px = 0, py = 0;
	for (int i = 0; i <= 60; i++) {
		float q = i / 60.0f;
		float l = v == 1 || q < 0.1f ? top : q < 0.5f ? top + (q - 0.1f) / 0.4f * (bot - top) : q < 0.75f ? bot
		                                                    : bot - (q - 0.75f) / 0.25f * (bot - top);
		float x = x0 + q * W;
		if (i)
			seg(px, py, x, l, 1.5f, q <= ph ? FIGC_WHITE : FIGC_DIM);
		px = x, py = l;
	}
	float tired = v == 0 ? 0.5f : 0.75f;
	ring_at(12, 86, key);
	box(x0, 84, x0 + W, 88, FIGC_FAINT);
	box(x0 + 0.1f * W, 84, x0 + tired * W, 88, ph > 0.1f ? FIGC_RED : 0x3a1414ff);
	if (v == 0)
		box(x0 + tired * W, 84, x0 + 0.75f * W, 88, FIGC_DIM);
	box(x0 + ph * W, 12, x0 + ph * W + 1, 92, 0xb4aea680);
}

// Picture: a slanted edge between the dark and the light, close up in the screen's pixels. Sharp
// gives each pixel one side or the other; Soft shades it by how much of it each side covers.
static void picture(int v, float t)
{
	(void)t;
	const float px = 8, x0 = 16, y0 = 8;
	const int nx = 18, ny = 11;
	for (int j = 0; j < ny; j++)
		for (int i = 0; i < nx; i++) {
			int in = 0;                  // of 4 x 4 points in the pixel, those under the edge
			for (int sj = 0; sj < 4; sj++)
				for (int si = 0; si < 4; si++) {
					float x = i + (si + 0.5f) / 4.0f, y = j + (sj + 0.5f) / 4.0f;
					in += y > 1.5f + x * 0.42f;
				}
			float k = v == 0 ? in / 16.0f : in >= 8 ? 1.0f : 0.0f;
			u32 c = (u32)(0x22 + k * (0xb4 - 0x22)) << 24 | (u32)(0x1d + k * (0xae - 0x1d)) << 16 |
			        (u32)(0x1b + k * (0xa6 - 0x1b)) << 8 | 0xffu;
			box(x0 + i * px, y0 + j * px, x0 + (i + 1) * px, y0 + (j + 1) * px, c);
		}
	frame(x0, y0, x0 + nx * px, y0 + ny * px, FIGC_GREY);
}

void fig_draw(int fig, int value, float x, float y, float t, bool lit, const char* key)
{
	if (s_measure || fig == FIG_NONE)
		return;
	s_ox = x, s_oy = y, s_fa = lit ? 1.0f : 0.34f;
	switch (fig) {
	case FIG_CAMERA: camera(value, t); break;
	case FIG_MOVEMENT: movement(value, t); break;
	case FIG_AIM: hold(value, t, key, false); break;
	case FIG_LOOK: hold(value, t, key, true); break;
	case FIG_SCREENS: screens(value, t); break;
	case FIG_BEAM: beam(value, t); break;
	case FIG_FRAMING: framing(value, t); break;
	case FIG_MAP: map(value, t); break;
	case FIG_STAMINA: stamina(value, t, key); break;
	case FIG_PICTURE: picture(value, t); break;
	}
	s_ox = s_oy = 0.0f, s_fa = 1.0f;
}
