#include "picture.h"
#include "movie_shbin.h"
#include <string.h>

// picture.h says what this is.

typedef struct { float x, y, u, v; } V;   // movie.c's QuadVertex: the target's pixels, the scene's texture

// The vertices, for each texture's own sizes: four taps' interiors, the edges, the interior untapped
#define TAPS 0
#define EDGES 24
#define PLAIN 48
#define NV 54

static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uProj;
static bool s_ready, s_soft = true;
static int s_debug;
// How much of the filtered picture is kept (percent): the rest is the scene's own pixel, drawn over
// the filter. The whole filter keeps a quarter of each pixel and looked out of focus on a console;
// at half, edges still lose their steps and the picture stays clear. debug.txt "softness".
static int s_softness = 50;
// linear, one set per scene texture width (256 and 512, supersample.h): the two eyes of a 3D frame
// read different ones, and the GPU reads both at C3D_FrameEnd
static struct { V* v; int key[6]; } s_set[2];

bool picture_init(void)
{
	if (s_ready)
		return true;
	// the movie's quad program (movie.v.pica), which consoles run: an earlier program of the pass's own
	// with a single vertex attribute froze a New 3DS on its first draw, though Azahar drew it
	s_dvlb = DVLB_ParseFile((u32*)movie_shbin, movie_shbin_size);
	if (!s_dvlb)
		return false;
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uProj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
	for (int i = 0; i < 2; i++)
		if (!(s_set[i].v = linearAlloc(NV * sizeof(V))))
			return false;
	s_ready = true;
	return true;
}

void picture_set_soft(bool on) { s_soft = on; }
bool picture_soft(void) { return s_ready && (s_soft || s_debug == 1 || s_debug == 3); }
bool picture_probe(void) { return s_ready && s_debug == 3; }
void picture_debug(int mode) { s_debug = mode; }
void picture_softness(int percent) { s_softness = percent < 0 ? 0 : percent > 100 ? 100 : percent; }

// The quads for a w x h target over a scene sx x sy times its size in tex. The interior's taps stay
// inside the scene; the edge pixels take it as it is. Along both axes the scene starts at the
// texture's origin: the GPU writes the viewport's row y to the buffer's row height - 1 - y, and the
// texture's t runs the other way.
static V* quads(int w, int h, int sx, int sy, const C3D_Tex* tex)
{
	int key[6] = { w, h, sx, sy, tex->width, tex->height };
	int i = tex->width > 256;
	if (!memcmp(key, s_set[i].key, sizeof key))
		return s_set[i].v;
	memcpy(s_set[i].key, key, sizeof key);
	const float W = (float)w, H = (float)h, a = W - 1.0f, b = H - 1.0f;
	const float su = (float)sx / tex->width, sv = (float)sy / tex->height;
	static const float tap[5][2] = { { 0.5f, 0.5f }, { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0, 0 } };
	static const float inner[6][2] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
	V* v = s_set[i].v;
	// the interior with each tap, then untapped
	for (int k = 0; k < 5; k++) {
		V* q = v + (k < 4 ? TAPS + 6 * k : PLAIN);
		for (int j = 0; j < 6; j++) {
			float x = inner[j][0] > 0 ? 1.0f : a, y = inner[j][1] > 0 ? 1.0f : b;
			q[j] = (V){ x, y, (x + tap[k][0]) * su, (y + tap[k][1]) * sv };
		}
	}
	const float e[24][2] = {
		{ 0, 0 }, { 1, 0 }, { 1, H }, { 0, 0 }, { 1, H }, { 0, H },
		{ a, 0 }, { W, 0 }, { W, H }, { a, 0 }, { W, H }, { a, H },
		{ 1, 0 }, { a, 0 }, { a, 1 }, { 1, 0 }, { a, 1 }, { 1, 1 },
		{ 1, b }, { a, b }, { a, H }, { 1, b }, { a, H }, { 1, H },
	};
	for (int j = 0; j < 24; j++)
		v[EDGES + j] = (V){ e[j][0], e[j][1], e[j][0] * su, e[j][1] * sv };
	GSPGPU_FlushDataCache(v, NV * sizeof(V));
	return v;
}

// One draw over what is there at alpha / 255 (0: nothing changes, the pass's work without its picture).
static void draw(u8 alpha, int first, int count)
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvColor(env, (u32)alpha << 24);
	if (s_debug == 3)
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
	else if (alpha == 255)
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
	C3D_DrawArrays(GPU_TRIANGLES, first, count);
}

void picture_soft_pass(const PictureTarget* t)
{
	if (!picture_soft() || !t->tex)
		return;
	C3D_RenderTarget* out = t->out;
	int W = out->frameBuf.width, H = out->frameBuf.height;
	V* v = quads(W, H, t->sx, t->sy, t->tex);
	C3D_FrameDrawOn(out);
	// as movie.c's bind_quad
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);     // position
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);     // texcoord
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, v, sizeof(V), 2, 0x10);
	C3D_Mtx proj;
	Mtx_Ortho(&proj, 0.0f, (float)W, 0.0f, (float)H, 0.0f, 1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProj, &proj);
	C3D_LightEnvBind(NULL);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
	C3D_TexBind(0, t->tex);
	// the tap's colour, the alpha from the constant
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	if (s_debug == 1) {
		draw(255, PLAIN, 6);                        // the scene as it is
	} else {
		// half a pixel off on each diagonal, each over the ones before at 1/2, 1/3 and 1/4: the four
		// taps' mean is the [1, 2, 1] filter in both directions
		draw(255, TAPS, 6);
		draw(128, TAPS + 6, 6);
		draw(85, TAPS + 12, 6);
		draw(64, TAPS + 18, 6);
		// then the pixel itself over it (from a larger scene, its box average), leaving softness %
		// of the filter
		if (s_softness < 100)
			draw((u8)((100 - s_softness) * 255 / 100), PLAIN, 6);
	}
	draw(255, EDGES, 24);
}
