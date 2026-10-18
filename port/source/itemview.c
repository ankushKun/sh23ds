#include "itemview.h"
#include "chr.h"
#include "skin_shbin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_DIR "sdmc:/3ds/sh23ds"

// The item screen's model (the PC's item menu turns the item's model in the middle of the screen):
// pack/events/itemmdl.txt gives each item's model (tools/sh2conv/stagemodels.py), drawn with the
// skinning shader under one soft light, the last one kept loaded.

static shaderProgram_s s_prog;
static DVLB_s* s_dvlb;
static int s_uProj, s_uModelView, s_uBones;
static C3D_LightEnv s_env;
static C3D_Light s_light;
static bool s_ready;
static Character s_chr;
static int s_item = -1;
static bool s_chr_ok;

static float lut_one(float x, float unused) { (void)x, (void)unused; return 1.0f; }

static void init(void)
{
	s_ready = true;
	s_dvlb = DVLB_ParseFile((u32*)skin_shbin, skin_shbin_size);
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uProj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
	s_uModelView = shaderInstanceGetUniformLocation(s_prog.vertexShader, "modelView");
	s_uBones = shaderInstanceGetUniformLocation(s_prog.vertexShader, "bones");
	static const C3D_Material mat = {
		{ 0.8f, 0.8f, 0.8f },      // ambient
		{ 0.6f, 0.6f, 0.6f },      // diffuse
		{ 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
	};
	C3D_LightEnvInit(&s_env);
	C3D_LightEnvMaterial(&s_env, &mat);
	chr_light_ambient(&s_env, 1.0f, 1.0f, 1.0f);
	static C3D_LightLut d0;
	LightLut_FromFunc(&d0, lut_one, 0.0f, false);
	C3D_LightEnvLut(&s_env, GPU_LUT_D0, GPU_LUTINPUT_NH, false, &d0);
	C3D_LightInit(&s_light, &s_env);
	chr_light_color(&s_light, 1.0f, 1.0f, 1.0f, 0.6f);
	C3D_FVec dir = FVec4_New(-0.4f, 0.6f, 0.7f, 0.0f);     // view space, towards the light
	C3D_LightPosition(&s_light, &dir);
}

static bool model_of(int item, char* folder, char* name)
{
	FILE* f = fopen(PACK_DIR "/events/itemmdl.txt", "r");
	if (!f)
		return false;
	char line[96];
	bool found = false;
	while (!found && fgets(line, sizeof line, f)) {
		int it;
		unsigned kind;
		if (sscanf(line, "%d %x %7s %23s", &it, &kind, folder, name) == 4 && it == item)
			found = true;
	}
	fclose(f);
	return found;
}

bool itemview_has(int item)
{
	if (item != s_item) {
		if (s_chr_ok)
			chr_free(&s_chr);
		s_chr_ok = false;
		s_item = item;
		char folder[8], name[24], dir[64], err[64];
		if (model_of(item, folder, name)) {
			snprintf(dir, sizeof dir, PACK_DIR "/chr/%s", folder);
			s_chr_ok = chr_load(&s_chr, dir, name, err, sizeof err);
			if (s_chr_ok)
				chr_pose(&s_chr, NULL, 0.0f);
		}
	}
	return s_chr_ok;
}

void itemview_free(void)
{
	if (s_chr_ok)
		chr_free(&s_chr);
	s_chr_ok = false;
	s_item = -1;
}

void itemview_draw(int item, float x0, float y0, float x1, float y1, float angle)
{
	if (!itemview_has(item))
		return;
	if (!s_ready)
		init();
	// the bottom screen's target is 240 x 320 turned: screen x runs along the target's y
	int vx = 240 - (int)y1, vy = 320 - (int)x1, vw = (int)(y1 - y0), vh = (int)(x1 - x0);
	C3D_SetViewport(vx, vy, vw, vh);
	C3D_Mtx proj;
	Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(30.0f), (x1 - x0) / (y1 - y0), 5.0f, 20000.0f, false);
	// fit the model's bounds: its centre at the origin, far enough back for its size
	float c[3], r2 = 1.0f;
	for (int i = 0; i < 3; i++) {
		c[i] = (s_chr.bbox_min[i] + s_chr.bbox_max[i]) * 0.5f;
		float h = (s_chr.bbox_max[i] - s_chr.bbox_min[i]) * 0.5f;
		r2 += h * h;
	}
	float r = sqrtf(r2);                 // the bounding sphere, so any turn fits
	C3D_Mtx mv;
	Mtx_Identity(&mv);
	Mtx_Translate(&mv, 0.0f, 0.0f, -r * 4.0f, true);
	Mtx_RotateX(&mv, (float)M_PI + 0.35f, true);   // y points down in the game: turn it over, tilted
	Mtx_RotateY(&mv, angle, true);
	Mtx_Translate(&mv, -c[0], -c[1], -c[2], true);
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);
	AttrInfo_AddLoader(ai, 3, GPU_UNSIGNED_BYTE, 4);
	AttrInfo_AddLoader(ai, 4, GPU_BYTE, 4);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProj, &proj);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &mv);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_LightEnvBind(&s_env);
	// the characters' textures are drawn twice as bright as they are stored (the PC's 0..0x80
	// colour range): a third stage doubles what chr_draw's first stage gives
	C3D_TexEnv* e2 = C3D_GetTexEnv(2);
	C3D_TexEnvInit(e2);
	C3D_TexEnvSrc(e2, C3D_Both, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(e2, C3D_Both, GPU_REPLACE);
	C3D_TexEnvScale(e2, C3D_RGB, GPU_TEVSCALE_2);
	chr_draw(&s_chr, s_uBones);
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_LightEnvBind(NULL);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_SetViewport(0, 0, 240, 320);
}
