#include "picture.h"
#include "soft_shbin.h"
#include <string.h>

// picture.h says what this is.

#define PAD 8                    // the copy sits one tile in from the texture's edges on every side

typedef struct { float x, y; } V;

static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uProj, s_uTaps, s_uScale, s_uBase;
static C3D_Tex s_tex;
static bool s_tex_ok, s_ready, s_soft = true;
static int s_debug;
static V* s_quads;               // linear: the interior, then the four edges (30 vertices)
static int s_quad_w, s_quad_h;   // the target size they were made for

bool picture_init(void)
{
	if (s_ready)
		return true;
	s_dvlb = DVLB_ParseFile((u32*)soft_shbin, soft_shbin_size);
	if (!s_dvlb)
		return false;
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uProj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
	s_uTaps = shaderInstanceGetUniformLocation(s_prog.vertexShader, "taps");
	s_uScale = shaderInstanceGetUniformLocation(s_prog.vertexShader, "uvscale");
	s_uBase = shaderInstanceGetUniformLocation(s_prog.vertexShader, "uvbase");
	s_quads = linearAlloc(30 * sizeof(V));
	if (!s_quads)
		return false;
	s_ready = true;
	return true;
}

void picture_set_soft(bool on) { s_soft = on; }
bool picture_soft(void) { return s_ready && (s_soft || s_debug == 1); }
void picture_debug(int mode) { s_debug = mode; }

static int pow2(int n)
{
	int p = 8;
	while (p < n)
		p <<= 1;
	return p;
}

// The texture for a copy of a w x h scene and its padding, kept at the largest size asked for so
// far (2 MB of linear memory for a 2 x 2 scene, 512 KB at the screen's size).
static bool tex_fit(int w, int h)
{
	int tw = pow2(w + 2 * PAD), th = pow2(h + 2 * PAD);
	if (s_tex_ok && s_tex.width >= tw && s_tex.height >= th)
		return true;
	if (s_tex_ok) {
		tw = tw > s_tex.width ? tw : s_tex.width, th = th > s_tex.height ? th : s_tex.height;
		C3D_TexDelete(&s_tex);
		s_tex_ok = false;
	}
	if (tw > 1024 || th > 1024 || !C3D_TexInit(&s_tex, tw, th, GPU_RGBA8))
		return false;
	C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	s_tex_ok = true;
	return true;
}

bool picture_reserve(int w, int h) { return s_ready && tex_fit(w, h); }

// The interior, whose four taps stay inside the copy, then the edge pixels, which take it as it is.
static void quads(int w, int h)
{
	if (w == s_quad_w && h == s_quad_h)
		return;
	const float W = (float)w, H = (float)h, a = W - 1.0f, b = H - 1.0f;
	const V v[30] = {
		{ 1, 1 }, { a, 1 }, { a, b }, { 1, 1 }, { a, b }, { 1, b },
		{ 0, 0 }, { 1, 0 }, { 1, H }, { 0, 0 }, { 1, H }, { 0, H },
		{ a, 0 }, { W, 0 }, { W, H }, { a, 0 }, { W, H }, { a, H },
		{ 1, 0 }, { a, 0 }, { a, 1 }, { 1, 0 }, { a, 1 }, { 1, 1 },
		{ 1, b }, { a, b }, { a, H }, { 1, b }, { a, H }, { 1, H },
	};
	memcpy(s_quads, v, sizeof v);
	GSPGPU_FlushDataCache(s_quads, sizeof v);
	s_quad_w = w, s_quad_h = h;
}

static void draw(float x0, float y0, float x1, float y1, u8 alpha, int first, int count)
{
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uTaps, x0, y0, x1, y1);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvColor(env, (u32)alpha << 24 | 0x808080u);
	if (alpha == 255)
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
	C3D_DrawArrays(GPU_TRIANGLES, first, count);
}

void picture_soft_pass(C3D_RenderTarget* scene, int sx, int sy, C3D_RenderTarget* out)
{
	if (!picture_soft())
		return;
	int w = scene->frameBuf.width, h = scene->frameBuf.height;   // the GPU's x and y
	if (!tex_fit(w, h))
		return;
	// the scene's colours, tile row by tile row, into the texture one tile in: the copy waits for
	// the scene's commands, and the quads below wait for the copy
	u32 line = (u32)w * 8 * 4, stride = (u32)s_tex.width * 8 * 4;
	u8* dst = (u8*)s_tex.data + stride * (PAD / 8) + 8 * 8 * 4 * (PAD / 8);
	C3D_FrameSplit(0);
	GX_TextureCopy((u32*)scene->frameBuf.colorBuf, GX_BUFFER_DIM(line >> 4, 0), (u32*)dst,
	               GX_BUFFER_DIM(line >> 4, (stride - line) >> 4), line * (u32)(h / 8), GX_TRANSFER_RAW_COPY(1));

	int W = out->frameBuf.width, H = out->frameBuf.height;
	quads(W, H);
	C3D_FrameDrawOn(out);
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_quads, sizeof(V), 1, 0x0);
	C3D_Mtx proj;
	Mtx_Ortho(&proj, 0.0f, (float)W, 0.0f, (float)H, 0.0f, 1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProj, &proj);
	// a target pixel is sx x sy texels of the copy
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uScale, (float)sx / s_tex.width, (float)sy / s_tex.height, 0.0f, 0.0f);
	// along t the texture runs the other way from the target's rows, so there the copy starts PAD
	// and its own height in from the far end
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uBase, (float)PAD / s_tex.width, (float)(s_tex.height - PAD - h) / s_tex.height,
	              0.0f, 0.0f);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_TexBind(0, &s_tex);
	C3D_TexBind(1, &s_tex);
	// the two taps' mean (the constant's grey is the half), the alpha from the constant
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	if (s_debug == 1) {
		draw(0, 0, 0, 0, 255, 0, 30);              // the copy as it is
	} else {
		// half a pixel each way diagonally, then the other diagonal over it at half: the four
		// taps' mean is the [1, 2, 1] filter in both directions
		draw(0.5f, 0.5f, -0.5f, -0.5f, 255, 0, 6);
		draw(0.5f, -0.5f, -0.5f, 0.5f, 128, 0, 6);
		draw(0, 0, 0, 0, 255, 6, 24);
	}
	C3D_TexBind(1, NULL);
}
