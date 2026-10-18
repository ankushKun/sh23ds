// lr3ds: headless libretro frontend for testing 3DS builds in Azahar's libretro core.
//
//   lr3ds <core.dylib> <app.3dsx|.cia|.3ds> [options]
//     --frames N            frames to run (default 600)
//     --shot F:path.bmp     save the frame shown at frame F (repeatable)
//     --press A-B:BUTTONS   hold buttons from frame A to B, e.g. 120-125:A or 300-400:UP+B
//     --touch A-B:X,Y       touch the bottom screen at (X,Y) in 320x240 coordinates
//     --analog A-B:X,Y      hold the circle pad (left stick) at X,Y in -1..1 (Y up positive)
//     --cstick A-B:X,Y      hold the C-Stick (the right stick) at X,Y in -1..1 (Y up positive)
//     --opt key=value       core option override (repeatable)
//     --list-options        print the core's options and values, then continue
//     --sys DIR / --save DIR  system and save directories (default ./lr3ds_sys, ./lr3ds_save)
//     --log                 print core log messages
//     --wav path.wav        write the core's audio output (16-bit stereo)
//     --stop-on FILE:TEXT   end the run early once the host file FILE contains TEXT (checked every
//                           60 frames; the playtest driver writes "# end" to its log on the SD)
//     --gl                  give the core an offscreen OpenGL 4.1 core context (CGL) so it can use its
//                           hardware renderer (also pass --opt citra_graphics_api=OpenGL)
// Buttons: A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT
// Without --gl hardware rendering is refused, so the core has to use its software renderer.
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#define ENV_EXP 0x10000
enum {
	ENV_GET_CAN_DUPE = 3, ENV_SET_MESSAGE = 6, ENV_SHUTDOWN = 7, ENV_SET_PERFORMANCE_LEVEL = 8,
	ENV_GET_SYSTEM_DIRECTORY = 9, ENV_SET_PIXEL_FORMAT = 10, ENV_SET_INPUT_DESCRIPTORS = 11,
	ENV_SET_HW_RENDER = 14, ENV_GET_VARIABLE = 15, ENV_SET_VARIABLES = 16, ENV_GET_VARIABLE_UPDATE = 17,
	ENV_GET_LOG_INTERFACE = 27, ENV_GET_SAVE_DIRECTORY = 31, ENV_SET_GEOMETRY = 37,
	ENV_GET_LANGUAGE = 39, ENV_GET_CORE_OPTIONS_VERSION = 52, ENV_SET_CORE_OPTIONS = 53,
	ENV_SET_CORE_OPTIONS_INTL = 54, ENV_SET_CORE_OPTIONS_DISPLAY = 55, ENV_GET_PREFERRED_HW_RENDER = 56,
	ENV_GET_MESSAGE_INTERFACE_VERSION = 59, ENV_SET_MESSAGE_EXT = 60, ENV_GET_INPUT_MAX_USERS = 61,
	ENV_SET_CORE_OPTIONS_V2 = 67, ENV_SET_CORE_OPTIONS_V2_INTL = 68,
	ENV_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK = 69, ENV_GET_INPUT_BITMASKS = 51 | ENV_EXP,
	ENV_GET_AUDIO_VIDEO_ENABLE = 47 | ENV_EXP, ENV_GET_FASTFORWARDING = 49 | ENV_EXP,
};
enum { PIX_0RGB1555 = 0, PIX_XRGB8888 = 1, PIX_RGB565 = 2 };
enum { DEV_JOYPAD = 1, DEV_ANALOG = 5, DEV_POINTER = 6 };
enum { ID_B, ID_Y, ID_SELECT, ID_START, ID_UP, ID_DOWN, ID_LEFT, ID_RIGHT, ID_A, ID_X, ID_L, ID_R, ID_L2, ID_R2 };
#define JOYPAD_MASK 256
#define MAX_VALUES 128

struct retro_variable { const char *key, *value; };
struct retro_option_value { const char *value, *label; };
struct retro_option_def_v1 { const char *key, *desc, *info; struct retro_option_value values[MAX_VALUES]; const char *default_value; };
struct retro_option_def_v2 { const char *key, *desc, *desc_cat, *info, *info_cat, *category_key;
	struct retro_option_value values[MAX_VALUES]; const char *default_value; };
struct retro_options_v2 { void *categories; struct retro_option_def_v2 *definitions; };
struct retro_options_v2_intl { struct retro_options_v2 *us, *local; };
struct retro_options_intl { struct retro_option_def_v1 *us, *local; };
struct retro_log_callback { void (*log)(int level, const char *fmt, ...); };
struct retro_message { const char *msg; unsigned frames; };
struct retro_message_ext { const char *msg; unsigned duration, priority; int level, target, type; int8_t progress; };
struct retro_game_geometry { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; };
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info { struct retro_game_geometry geometry; struct retro_system_timing timing; };
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_system_info { const char *library_name, *library_version, *valid_extensions; bool need_fullpath, block_extract; };
struct retro_hw_render_callback {
	int context_type;
	void (*context_reset)(void);
	uintptr_t (*get_current_framebuffer)(void);
	void *(*get_proc_address)(const char *sym);
	bool depth, stencil, bottom_left_origin;
	unsigned version_major, version_minor;
	bool cache_context;
	void (*context_destroy)(void);
	bool debug_context;
};
enum { HW_OPENGL = 1, HW_OPENGL_CORE = 3 };

// --gl: offscreen OpenGL context and framebuffer the core renders into
static bool use_gl;
static struct retro_hw_render_callback hw;
static bool hw_on;
static CGLContextObj gl_ctx;
static GLuint gl_fbo, gl_color, gl_depth;
static uintptr_t hw_fbo(void) { return gl_fbo; }
static void *hw_proc(const char *sym) { return dlsym(RTLD_DEFAULT, sym); }

static bool gl_create(unsigned w, unsigned h)
{
	CGLPixelFormatAttribute attrs[] = {
		kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, kCGLPFAAccelerated,
		kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		kCGLPFADepthSize, (CGLPixelFormatAttribute)24, kCGLPFAStencilSize, (CGLPixelFormatAttribute)8, 0 };
	CGLPixelFormatObj pf;
	GLint npix;
	if (CGLChoosePixelFormat(attrs, &pf, &npix) != kCGLNoError || !pf) return false;
	CGLError e = CGLCreateContext(pf, NULL, &gl_ctx);
	CGLDestroyPixelFormat(pf);
	if (e != kCGLNoError) return false;
	CGLSetCurrentContext(gl_ctx);
	glGenFramebuffers(1, &gl_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
	glGenRenderbuffers(1, &gl_color);
	glBindRenderbuffer(GL_RENDERBUFFER, gl_color);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gl_color);
	glGenRenderbuffers(1, &gl_depth);
	glBindRenderbuffer(GL_RENDERBUFFER, gl_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gl_depth);
	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	printf("[gl] %s, %s, fbo %ux%u %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), w, h, ok ? "ok" : "INCOMPLETE");
	return ok;
}

#define MAX_OPTS 256
static struct { char key[96]; char value[96]; char values[1024]; } opts[MAX_OPTS];
static int nopts;
static struct { char key[96]; char value[96]; } overrides[64];
static int noverrides;
static bool list_options, show_log;
static char sys_dir[1024] = "lr3ds_sys", save_dir[1024] = "lr3ds_save";
static int pixfmt = PIX_0RGB1555;
static unsigned frame_no;

#define MAX_EVENTS 128
static struct { unsigned a, b; uint32_t buttons; int tx, ty; bool touch, analog, cstick; float ax, ay; } events[MAX_EVENTS];
static int nevents;
static struct { unsigned frame; char path[1024]; } shots[64];
static int nshots;

static void set_opt(const char *key, const char *def, const char *values)
{
	if (nopts >= MAX_OPTS) return;
	snprintf(opts[nopts].key, sizeof opts[0].key, "%s", key);
	snprintf(opts[nopts].value, sizeof opts[0].value, "%s", def ? def : "");
	snprintf(opts[nopts].values, sizeof opts[0].values, "%s", values ? values : "");
	for (int i = 0; i < noverrides; i++)
		if (!strcmp(overrides[i].key, key))
			snprintf(opts[nopts].value, sizeof opts[0].value, "%s", overrides[i].value);
	if (list_options) printf("[opt] %s = %s   {%s}\n", key, opts[nopts].value, opts[nopts].values);
	nopts++;
}

static void opts_v2(struct retro_option_def_v2 *d)
{
	for (; d && d->key; d++) {
		char vals[1024] = "";
		for (int i = 0; i < MAX_VALUES && d->values[i].value; i++) {
			strncat(vals, d->values[i].value, sizeof vals - strlen(vals) - 2);
			strncat(vals, "|", sizeof vals - strlen(vals) - 1);
		}
		set_opt(d->key, d->default_value ? d->default_value : d->values[0].value, vals);
	}
}

static void opts_v1(struct retro_option_def_v1 *d)
{
	for (; d && d->key; d++) {
		char vals[1024] = "";
		for (int i = 0; i < MAX_VALUES && d->values[i].value; i++) {
			strncat(vals, d->values[i].value, sizeof vals - strlen(vals) - 2);
			strncat(vals, "|", sizeof vals - strlen(vals) - 1);
		}
		set_opt(d->key, d->default_value ? d->default_value : d->values[0].value, vals);
	}
}

static void core_log(int level, const char *fmt, ...)
{
	if (!show_log && level < 3) return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[core] ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool env(unsigned cmd, void *data)
{
	switch (cmd) {
	case ENV_GET_CAN_DUPE: *(bool *)data = true; return true;
	case ENV_SET_PERFORMANCE_LEVEL: case ENV_SET_INPUT_DESCRIPTORS: case ENV_SET_GEOMETRY:
	case ENV_SET_CORE_OPTIONS_DISPLAY: case ENV_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
		return true;
	case ENV_SHUTDOWN: return true;
	case ENV_SET_MESSAGE: fprintf(stderr, "[msg] %s\n", ((struct retro_message *)data)->msg); return true;
	case ENV_SET_MESSAGE_EXT: fprintf(stderr, "[msg] %s\n", ((struct retro_message_ext *)data)->msg); return true;
	case ENV_GET_MESSAGE_INTERFACE_VERSION: *(unsigned *)data = 1; return true;
	case ENV_GET_SYSTEM_DIRECTORY: *(const char **)data = sys_dir; return true;
	case ENV_GET_SAVE_DIRECTORY: *(const char **)data = save_dir; return true;
	case ENV_SET_PIXEL_FORMAT: pixfmt = *(int *)data; return true;
	case ENV_SET_HW_RENDER: {
		if (!use_gl) return false;   // force the software renderer
		struct retro_hw_render_callback *cb = data;
		if (cb->context_type != HW_OPENGL_CORE && cb->context_type != HW_OPENGL) return false;
		cb->get_current_framebuffer = hw_fbo;
		cb->get_proc_address = hw_proc;
		hw = *cb;
		hw_on = true;
		printf("[gl] core asks for OpenGL %s %u.%u\n", cb->context_type == HW_OPENGL_CORE ? "core" : "compat",
		       cb->version_major, cb->version_minor);
		return true;
	}
	case ENV_GET_PREFERRED_HW_RENDER:
		if (!use_gl) return false;
		*(unsigned *)data = HW_OPENGL_CORE;
		return true;
	case ENV_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = core_log; return true;
	case ENV_GET_LANGUAGE: *(unsigned *)data = 0; return true;
	case ENV_GET_INPUT_MAX_USERS: *(unsigned *)data = 1; return true;
	case ENV_GET_INPUT_BITMASKS: return true;
	case ENV_GET_AUDIO_VIDEO_ENABLE: *(int *)data = 3; return true;
	case ENV_GET_FASTFORWARDING: *(bool *)data = false; return true;
	case ENV_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 2; return true;
	case ENV_SET_CORE_OPTIONS_V2: opts_v2(((struct retro_options_v2 *)data)->definitions); return true;
	case ENV_SET_CORE_OPTIONS_V2_INTL: opts_v2(((struct retro_options_v2_intl *)data)->us->definitions); return true;
	case ENV_SET_CORE_OPTIONS: opts_v1((struct retro_option_def_v1 *)data); return true;
	case ENV_SET_CORE_OPTIONS_INTL: opts_v1(((struct retro_options_intl *)data)->us); return true;
	case ENV_SET_VARIABLES:
		for (struct retro_variable *v = data; v->key; v++) {
			const char *semi = strchr(v->value, ';');
			const char *vals = semi ? semi + 2 : v->value;
			char def[96] = "";
			size_t n = strcspn(vals, "|");
			snprintf(def, sizeof def, "%.*s", (int)n, vals);
			set_opt(v->key, def, vals);
		}
		return true;
	case ENV_GET_VARIABLE: {
		struct retro_variable *v = data;
		for (int i = 0; i < nopts; i++)
			if (!strcmp(opts[i].key, v->key)) { v->value = opts[i].value; return true; }
		for (int i = 0; i < noverrides; i++)
			if (!strcmp(overrides[i].key, v->key)) { v->value = overrides[i].value; return true; }
		v->value = NULL;
		return false;
	}
	case ENV_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
	default:
		if (show_log) fprintf(stderr, "[env] unhandled %u (%u%s)\n", cmd, cmd & 0xffff, cmd & ENV_EXP ? " exp" : "");
		return false;
	}
}

static void save_bmp(const char *path, const void *data, unsigned w, unsigned h, size_t pitch)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	unsigned rowsz = (w * 3 + 3) & ~3u;
	uint32_t size = 54 + rowsz * h;
	uint8_t hdr[54] = {'B', 'M'};
	memcpy(hdr + 2, &size, 4);
	uint32_t off = 54, ih = 40, planes_bpp = 1 | (24 << 16);
	int32_t sw = w, sh = h;
	memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &ih, 4); memcpy(hdr + 18, &sw, 4);
	memcpy(hdr + 22, &sh, 4); memcpy(hdr + 26, &planes_bpp, 4);
	fwrite(hdr, 1, 54, f);
	uint8_t *row = calloc(1, rowsz);
	for (int y = h - 1; y >= 0; y--) {
		const uint8_t *src = (const uint8_t *)data + y * pitch;
		for (unsigned x = 0; x < w; x++) {
			uint8_t r, g, b;
			if (pixfmt == PIX_XRGB8888) { uint32_t p = ((const uint32_t *)src)[x]; r = p >> 16; g = p >> 8; b = p; }
			else if (pixfmt == PIX_RGB565) { uint16_t p = ((const uint16_t *)src)[x]; r = (p >> 11) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3; }
			else { uint16_t p = ((const uint16_t *)src)[x]; r = ((p >> 10) & 31) << 3; g = ((p >> 5) & 31) << 3; b = (p & 31) << 3; }
			row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
		}
		fwrite(row, 1, rowsz, f);
	}
	free(row);
	fclose(f);
}

static unsigned last_w, last_h;
static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (data == (void *)-1 && hw_on) {   // RETRO_HW_FRAME_BUFFER_VALID: read the frame back from the FBO
		last_w = w; last_h = h;
		for (int i = 0; i < nshots; i++)
			if (shots[i].frame == frame_no) {
				uint32_t *px = malloc((size_t)w * h * 4), *out = malloc((size_t)w * h * 4);
				glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
				glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
				for (unsigned y = 0; y < h; y++)    // GL rows start at the bottom
					memcpy(out + (size_t)y * w, px + (size_t)(hw.bottom_left_origin ? h - 1 - y : y) * w, (size_t)w * 4);
				int saved = pixfmt;
				pixfmt = PIX_XRGB8888;
				save_bmp(shots[i].path, out, w, h, (size_t)w * 4);
				pixfmt = saved;
				free(px);
				free(out);
				printf("[shot] frame %u -> %s (%ux%u, gl)\n", frame_no, shots[i].path, w, h);
			}
		return;
	}
	if (!data || data == (void *)-1) return;
	last_w = w; last_h = h;
	for (int i = 0; i < nshots; i++)
		if (shots[i].frame == frame_no) {
			save_bmp(shots[i].path, data, w, h, pitch);
			printf("[shot] frame %u -> %s (%ux%u)\n", frame_no, shots[i].path, w, h);
		}
}
static FILE *wav_f;
static uint32_t wav_frames;
static double wav_rate = 32728.0;
static void audio(int16_t l, int16_t r) { if (wav_f) { int16_t s[2] = { l, r }; fwrite(s, 2, 2, wav_f); wav_frames++; } }
static size_t audio_batch(const int16_t *d, size_t n) { if (wav_f) { fwrite(d, 4, n, wav_f); wav_frames += n; } return n; }
static void wav_close(void)
{
	if (!wav_f) return;
	uint32_t data = wav_frames * 4, riff = 36 + data, rate = (uint32_t)wav_rate, brate = rate * 4, fmt = 16;
	uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
	fseek(wav_f, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, wav_f); fwrite(&riff, 4, 1, wav_f); fwrite("WAVEfmt ", 1, 8, wav_f); fwrite(&fmt, 4, 1, wav_f);
	fwrite(&pcm, 2, 1, wav_f); fwrite(&ch, 2, 1, wav_f); fwrite(&rate, 4, 1, wav_f); fwrite(&brate, 4, 1, wav_f);
	fwrite(&align, 2, 1, wav_f); fwrite(&bits, 2, 1, wav_f); fwrite("data", 1, 4, wav_f); fwrite(&data, 4, 1, wav_f);
	fclose(wav_f);
	wav_f = NULL;
}
static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port) return 0;
	uint32_t held = 0;
	int tx = -1, ty = -1;
	float ax = 0, ay = 0, cx = 0, cy = 0;
	for (int i = 0; i < nevents; i++)
		if (frame_no >= events[i].a && frame_no <= events[i].b) {
			held |= events[i].buttons;
			if (events[i].touch) { tx = events[i].tx; ty = events[i].ty; }
			if (events[i].analog) { ax = events[i].ax; ay = events[i].ay; }
			if (events[i].cstick) { cx = events[i].ax; cy = events[i].ay; }
		}
	if (device == DEV_ANALOG && index == 0) {
		if (id == 0) return (int16_t)(ax * 32767);
		if (id == 1) return (int16_t)(-ay * 32767);   // libretro: down is positive
		return 0;
	}
	if (device == DEV_ANALOG && index == 1) {     // RETRO_DEVICE_INDEX_ANALOG_RIGHT: Azahar's C-Stick
		if (id == 0) return (int16_t)(cx * 32767);
		if (id == 1) return (int16_t)(-cy * 32767);
		return 0;
	}
	if (device == DEV_JOYPAD) {
		if (id == JOYPAD_MASK) return (int16_t)held;
		return (held >> id) & 1;
	}
	if (device == DEV_POINTER && tx >= 0) {
		// libretro pointer space is -0x7fff..0x7fff over the whole frame (top 400x240 over bottom 320x240)
		if (id == 2) return 1;
		double fx = (40 + tx) / 400.0, fy = (240 + ty) / 480.0;
		if (id == 0) return (int16_t)(fx * 0xfffe - 0x7fff);
		if (id == 1) return (int16_t)(fy * 0xfffe - 0x7fff);
	}
	(void)index;
	return 0;
}

static uint32_t parse_buttons(const char *s)
{
	static const struct { const char *n; int id; } names[] = {
		{"A", ID_A}, {"B", ID_B}, {"X", ID_X}, {"Y", ID_Y}, {"L", ID_L}, {"R", ID_R}, {"ZL", ID_L2}, {"ZR", ID_R2},
		{"START", ID_START}, {"SELECT", ID_SELECT}, {"UP", ID_UP}, {"DOWN", ID_DOWN}, {"LEFT", ID_LEFT}, {"RIGHT", ID_RIGHT}};
	uint32_t m = 0;
	char buf[128];
	snprintf(buf, sizeof buf, "%s", s);
	for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
		for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
			if (!strcmp(t, names[i].n)) m |= 1u << names[i].id;
	return m;
}

#define SYM(name) do { *(void **)&name = dlsym(h, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 1; } } while (0)

static char stop_file[1024], stop_text[128];

// --stop-on: whether the file holds the text (the SD is a host folder, so the game's log is readable)
static bool file_has(const char *path, const char *text)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	static char buf[1 << 20];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return strstr(buf, text) != NULL;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 3) { fprintf(stderr, "usage: lr3ds core.dylib app.3dsx [options]\n"); return 2; }
	const char *core = argv[1], *content = argv[2];
	unsigned frames = 600;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshots < 64) {
			char *c = strchr(argv[++i], ':');
			if (!c) continue;
			shots[nshots].frame = atoi(argv[i]);
			snprintf(shots[nshots++].path, 1024, "%s", c + 1);
		} else if ((!strcmp(argv[i], "--press") || !strcmp(argv[i], "--touch") || !strcmp(argv[i], "--analog") ||
		            !strcmp(argv[i], "--cstick")) && i + 1 < argc && nevents < MAX_EVENTS) {
			bool touch = !strcmp(argv[i], "--touch"), analog = !strcmp(argv[i], "--analog");
			bool cstick = !strcmp(argv[i], "--cstick");
			char *spec = argv[++i];
			unsigned a = 0, b = 0;
			char rest[128] = "";
			if (sscanf(spec, "%u-%u:%127s", &a, &b, rest) != 3) { fprintf(stderr, "bad %s\n", spec); return 2; }
			events[nevents].a = a; events[nevents].b = b; events[nevents].touch = touch;
			events[nevents].analog = analog;
			events[nevents].cstick = cstick;
			if (analog || cstick) sscanf(rest, "%f,%f", &events[nevents].ax, &events[nevents].ay);
			else if (touch) sscanf(rest, "%d,%d", &events[nevents].tx, &events[nevents].ty);
			else events[nevents].buttons = parse_buttons(rest);
			nevents++;
		} else if (!strcmp(argv[i], "--opt") && i + 1 < argc && noverrides < 64) {
			char *eq = strchr(argv[++i], '=');
			if (!eq) continue;
			snprintf(overrides[noverrides].key, 96, "%.*s", (int)(eq - argv[i]), argv[i]);
			snprintf(overrides[noverrides++].value, 96, "%s", eq + 1);
		} else if (!strcmp(argv[i], "--list-options")) list_options = true;
		else if (!strcmp(argv[i], "--log")) show_log = true;
		else if (!strcmp(argv[i], "--gl")) use_gl = true;
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { wav_f = fopen(argv[++i], "wb"); if (wav_f) fseek(wav_f, 44, SEEK_SET); }
		else if (!strcmp(argv[i], "--stop-on") && i + 1 < argc) {
			char *c = strrchr(argv[++i], ':');
			if (c) { snprintf(stop_file, sizeof stop_file, "%.*s", (int)(c - argv[i]), argv[i]); snprintf(stop_text, sizeof stop_text, "%s", c + 1); }
		}
		else if (!strcmp(argv[i], "--sys") && i + 1 < argc) snprintf(sys_dir, sizeof sys_dir, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--save") && i + 1 < argc) snprintf(save_dir, sizeof save_dir, "%s", argv[++i]);
	}
	mkdir(sys_dir, 0755);
	mkdir(save_dir, 0755);

	void *h = dlopen(core, RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	void (*retro_set_environment)(bool (*)(unsigned, void *));
	void (*retro_set_video_refresh)(void (*)(const void *, unsigned, unsigned, size_t));
	void (*retro_set_audio_sample)(void (*)(int16_t, int16_t));
	void (*retro_set_audio_sample_batch)(size_t (*)(const int16_t *, size_t));
	void (*retro_set_input_poll)(void (*)(void));
	void (*retro_set_input_state)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
	void (*retro_init)(void);
	void (*retro_deinit)(void);
	void (*retro_get_system_info)(struct retro_system_info *);
	void (*retro_get_system_av_info)(struct retro_system_av_info *);
	bool (*retro_load_game)(const struct retro_game_info *);
	void (*retro_unload_game)(void);
	void (*retro_run)(void);
	SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
	SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
	SYM(retro_init); SYM(retro_deinit); SYM(retro_get_system_info); SYM(retro_get_system_av_info);
	SYM(retro_load_game); SYM(retro_unload_game); SYM(retro_run);

	struct retro_system_info si = {0};
	retro_get_system_info(&si);
	printf("[core] %s %s (%s)\n", si.library_name, si.library_version, si.valid_extensions);

	retro_set_environment(env);
	retro_set_video_refresh(video);
	retro_set_audio_sample(audio);
	retro_set_audio_sample_batch(audio_batch);
	retro_set_input_poll(input_poll);
	retro_set_input_state(input_state);
	retro_init();

	FILE *f = fopen(content, "rb");
	if (!f) { perror(content); return 1; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc(sz);
	fread(buf, 1, sz, f);
	fclose(f);
	struct retro_game_info gi = { content, buf, (size_t)sz, NULL };
	if (!retro_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	printf("[av] %ux%u @ %.2f fps, pixel format %d, audio %.0f Hz\n", av.geometry.base_width, av.geometry.base_height, av.timing.fps, pixfmt, av.timing.sample_rate);
	if (av.timing.sample_rate > 0) wav_rate = av.timing.sample_rate;
	if (hw_on) {
		unsigned mw = av.geometry.max_width ? av.geometry.max_width : 2048, mh = av.geometry.max_height ? av.geometry.max_height : 2048;
		if (!gl_create(mw, mh)) { fprintf(stderr, "[gl] context creation failed\n"); return 1; }
		if (hw.context_reset) hw.context_reset();
	}

	for (frame_no = 0; frame_no < frames; frame_no++) {
		retro_run();
		if (stop_file[0] && frame_no % 60 == 59 && file_has(stop_file, stop_text)) {
			printf("[stop] %s has \"%s\" at frame %u\n", stop_file, stop_text, frame_no + 1);
			frames = frame_no + 1;
			break;
		}
	}
	printf("[done] %u frames, last frame %ux%u\n", frames, last_w, last_h);
	wav_close();
	retro_unload_game();
	retro_deinit();
	fflush(stdout);
	fflush(stderr);
	_exit(0); // cores often crash in static destructors; the results are already written
}
