#include "picture.h"
#include "soft_shbin.h"
#include <string.h>

// picture.h says what this is.

typedef struct { float x, y; } V;

static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uProj, s_uTaps, s_uScale, s_uBase;
static bool s_ready, s_soft = true;
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

// One tap at (dx, dy) of the target's pixels, over what is there at alpha / 255.
static void draw(float dx, float dy, u8 alpha, int first, int count)
{
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uTaps, dx, dy, 0.0f, 0.0f);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvColor(env, (u32)alpha << 24);
	if (alpha == 255)
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
	C3D_Tex* tex = t->tex;
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
	// a target pixel is sx x sy texels of the scene, which starts at the texture's origin: the GPU
	// writes the viewport's row y to the buffer's row height - 1 - y, and the texture's t runs the
	// other way, so both axes start at 0
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uScale, (float)t->sx / tex->width, (float)t->sy / tex->height, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uBase, 0.0f, 0.0f, 0.0f, 0.0f);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_TexBind(0, tex);
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
		draw(0, 0, 255, 0, 30);                    // the scene as it is
	} else {
		// half a pixel off on each diagonal, each over the ones before at 1/2, 1/3 and 1/4: the four
		// taps' mean is the [1, 2, 1] filter in both directions
		draw(0.5f, 0.5f, 255, 0, 6);
		draw(-0.5f, -0.5f, 128, 0, 6);
		draw(0.5f, -0.5f, 85, 0, 6);
		draw(-0.5f, 0.5f, 64, 0, 6);
		draw(0, 0, 255, 6, 24);
	}
}
