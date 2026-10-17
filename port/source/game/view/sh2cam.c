/*
 * sh2cam.c - Silent Hill 2 (PC Director's Cut) in-game camera, portable C99 translation.
 *
 * Source of truth: the PC executable (sh2pc.exe, view module 0x00519430..0x00522410), read
 * from the Ghidra decompile and checked instruction by instruction against the disassembly
 * wherever the decompile lost register arguments (most of this module uses LTCG custom
 * calling conventions).  Names and types come from the PS2 beta DWARF (view/vc_main.c etc.).
 *
 * Every function carries its PC address.  Every float constant is the exact .rdata value,
 * with its address in a comment.  Quirks of the PC code are kept on purpose and marked
 * "PC quirk"; see PORTING_NOTES.md.
 *
 * Matrix convention: identical to the PC build (D3DX): float m[4][4], row-vector math
 * (v' = v * M), translation in row 3, D3DXMatrixMultiply(out, a, b) = a * b.
 */
#include "sh2cam.h"

#include <math.h>
#include <string.h>

/* ===================================================================== */
/* Compile-time layout checks                                            */
/* ===================================================================== */

#define SH2CAM_STATIC_ASSERT(name, cond) typedef char sh2cam_assert_##name[(cond) ? 1 : -1]

SH2CAM_STATIC_ASSERT(road_data_is_128_bytes, sizeof(_VC_ROAD_DATA) == 128);
SH2CAM_STATIC_ASSERT(road_area_is_32_bytes, sizeof(_VC_ROAD_AREA) == 32);
#if UINTPTR_MAX == 0xffffffffu && (defined(__GNUC__) || defined(__clang__))
/* With 32-bit pointers (PC, 3DS) the work struct reproduces the PC offsets exactly. */
#define SH2CAM_OFS(t, m) __builtin_offsetof(t, m)
SH2CAM_STATIC_ASSERT(near_road_is_0x148, sizeof(_VC_NEAR_ROAD_DATA) == 0x148);
SH2CAM_STATIC_ASSERT(work_cur_near_road_at_a5d8, SH2CAM_OFS(_VC_WORK, cur_near_road) == 0xA5D8);
SH2CAM_STATIC_ASSERT(work_cam_noise_at_30, SH2CAM_OFS(_VC_WORK, cam_noise) == 0x30);
SH2CAM_STATIC_ASSERT(work_cam_pos_at_5c, SH2CAM_OFS(_VC_WORK, cam_pos) == 0x5C);
SH2CAM_STATIC_ASSERT(work_chara_pos_at_15c, SH2CAM_OFS(_VC_WORK, chara_pos) == 0x15C);
SH2CAM_STATIC_ASSERT(work_item_dist_at_a730, SH2CAM_OFS(_VC_WORK, nearest_item_xz_dist) == 0xA730);
#endif

/* ===================================================================== */
/* Static data (PC .data / .rdata)                                       */
/* ===================================================================== */

/* watch_mv_prm_* @0x006C84A8.. : ang_accel_x, ang_accel_y, max_ang_spd_x, max_ang_spd_y */
static const _VC_WATCH_MV_PARAM watch_mv_prm_user     = { 7.53982306f, 13.8230085f, 1.38230085f, 4.3982296f };   /* 0x006C84A8 */
static const _VC_WATCH_MV_PARAM watch_mv_prm_nrml     = { 6.03185797f, 11.0584068f, 1.10584068f, 3.51858377f };  /* 0x006C84B8 */
static const _VC_WATCH_MV_PARAM watch_mv_prm_outdoor  = { 8.48230076f, 15.5508833f, 1.5550884f, 4.94800854f };   /* 0x006C84C8 */
static const _VC_WATCH_MV_PARAM watch_mv_prm_on_boat  = { 0.879645944f, 2.82743335f, 1.10584068f, 3.53429174f }; /* 0x006C84D8 */
static const _VC_WATCH_MV_PARAM self_view_watch_mv_prm= { 10.5557518f, 19.352211f, 2.76460171f, 8.7964592f };    /* 0x006C84E8 */
/* cam_mv_prm_* : accel_xz, accel_y, max_spd_xz, max_spd_y */
static const _VC_CAM_MV_PARAM cam_mv_prm_user    = { 5000.0f, 1500.0f, 3000.0f, 1000.0f };  /* 0x006C84F8 */
static const _VC_CAM_MV_PARAM cam_mv_prm_nrml    = { 4000.0f, 1500.0f, 2400.0f, 1000.0f };  /* 0x006C8508 */
static const _VC_CAM_MV_PARAM cam_mv_prm_outdoor = { 5625.0f, 1687.5f, 3375.0f, 1125.0f };  /* 0x006C8518 */
static const _VC_CAM_MV_PARAM cam_mv_prm_on_boat = { 3600.0f, 1200.0f, 4500.0f, 1500.0f };  /* 0x006C8528 */

/* Enhanced Edition "PS2CameraSpeed" (ThirteenAG's WidescreenFixesPack, SilentHill2.WidescreenFix,
   MIT licence, bundled with the EE and on by default there). It writes 4.0 over max_ang_spd_x of
   watch_mv_prm_nrml and watch_mv_prm_outdoor (0x006C84C0 / 0x006C84D0 in this build, picked at
   0x0051BB81 / 0x0051BB95), so the camera pitches toward the watch target as fast as on the PS2, and
   in vcRenewalCamData (0x0051C080) it limits the vertical approach speed to 3 * |dy| instead of
   accel_y * |dy|, which removes the PC camera's vertical overshoot ("bounce"). */
#define PS2_WATCH_MAX_ANG_SPD_X 4.0f
#define PS2_CAM_EASE_Y 3.0f
static bool s_ps2_speed = true;
void sh2cam_set_ps2_speed(bool on) { s_ps2_speed = on; }

/* Tables indexed by _VC_AREA_SIZE_TYPE (TINY, SMALL, WIDE, OUTDOOR) */
static const float nml_tgt_watch_cir_r[4]     = { 400.0f, 600.0f, 1000.0f, 1250.0f };   /* 0x006C8538 */
static const float nml_cam2wth_min_dist[4]    = { 1400.0f, 1800.0f, 2800.0f, 3200.0f }; /* 0x006C8548 */
static const float far_tgt_watch_cir_r[4]     = { 1600.0f, 2800.0f, 4200.0f, 5000.0f }; /* 0x006C8558 */
static const float mv_nml_chr2cam_r[4]        = { 600.0f, 800.0f, 1000.0f, 1250.0f };   /* 0x006C8568 */
static const float mv_nml_no_adj_max_dist[4]  = { 400.0f, 600.0f, 800.0f, 1000.0f };    /* 0x006C8578 */
static const float mv_nml_full_adj_min_dist[4]= { 1200.0f, 1600.0f, 2400.0f, 2800.0f }; /* 0x006C8588 */
static const float extra_boundary_width[4]    = { 75.0f, 125.0f, 175.0f, 250.0f };      /* 0x006C8598 */
static const float excl_max_rate[4]           = { 0.699999988f, 0.850000024f, 0.949999988f, 1.0f }; /* 0x006C85A8 */
static const float cir_reduction_r[4]         = { 1600.0f, 2800.0f, 4200.0f, 5000.0f }; /* 0x006CB480 (vc_newest copy) */
/* Interpolation tables (vwOresenHokan), PC moved them to .data */
static const int32_t excl_r_ary[9]    = { 16384, 16384, 15155, 13107, 11059, 9420, 7782, 6144, 5324 }; /* 0x008BC9E8 */
static const int32_t mv_stl_ang_ary[5]= { 0, 1072, 4289, 450560, 450560 };                             /* 0x008BCA0C */

/* vcNullRoadArray @0x008BCA20: an "infinite" WIDE chase road + END record */
static _VC_ROAD_DATA vcNullRoadArray[2] = {
    {
        { -2000000.0f, 2000000.0f, 2000000.0f, 2000000.0f, 2000000.0f, -2000000.0f, 2000000.0f, -2000000.0f },
        { -2000000.0f, 2000000.0f, 2000000.0f, 2000000.0f, 2000000.0f, -2000000.0f, 2000000.0f, -2000000.0f },
        0,                    /* kind_id        */
        0,                    /* flags          */
        VC_AREA_WIDE,         /* area_size_type */
        VC_RD_TYPE_ROAD,      /* rd_type        */
        3,                    /* mv_y_type      */
        -300.0f,              /* ofs_watch_hy   */
        0.0f,                 /* trace_btm_hy   */
        0,                    /* rd_dir_type    */
        448.0f,               /* projection     */
        0.0f, 0.0f,           /* proj_volume, proj_sec */
        VC_MV_CHASE,          /* cam_mv_type    */
        { { 0 } }
    },
    { { 0 }, { 0 }, 0, VC_RD_END_DATA_F, 0, 0, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, 0.0f, 0, { { 0 } } }
};
static _VC_ROAD_DATA *vcNullRoadArrayList[2] = { &vcNullRoadArray[0], NULL };  /* 0x008BCB20 */

/* vcNullNearRoad @0x008BCB28 */
static _VC_NEAR_ROAD_DATA vcNullNearRoad = {
    &vcNullRoadArray[0], 0, 0, 0.0f, 0.0f, 0.0f,
    { -2000000.0f, 2000000.0f, -2000000.0f, 2000000.0f, 2000000.0f, -2000000.0f },
    { -2000000.0f, 2000000.0f, -2000000.0f, 2000000.0f, 2000000.0f, -2000000.0f },
    { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } },
    { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } },
    { { 0 } },
    { { 0 } }
};

static const float s_identity[4][4] = {   /* 0x007F5520 */
    { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
};

/* ===================================================================== */
/* Camera context: every PC global the module owns                       */
/* ===================================================================== */

typedef struct {               /* vcProjectionParam (_anon1) @0x01F906B0 */
    float   new_proj;          /* 0x01F906B0 */
    float   old_proj;          /* 0x01F906B4 */
    float   delta;             /* 0x01F906B8 */
    int32_t flg;               /* 0x01F906BC  0 idle, 1 blending, 2 snap next frame */
} VcProjParam;

static struct {
    _VC_WORK           work;              /* vcWork            @0x01F83DE0 */
    float              self_view_timer;   /* vcSelfViewTimer   @0x01F906A8 (noise phase) */
    VcProjParam        proj;              /* vcProjectionParam @0x01F906B0 */
    _VC_WATCH_MV_PARAM watch_mv_prm_st;   /* vcWatchMvPrmSt    @0x01F906C0 */
    _VC_CAM_MV_PARAM   cam_mv_prm_st;     /* vcCamMvPrmSt      @0x01F90730 */
    float              scr_z;             /* gCameraFOV = VbScreenInfo.scr_z @0x01F83BC0 */
    float              scr_sx;            /* VbScreenInfo.sx @0x01F83BC4 (from input)    */
    float              scr_sy;            /* VbScreenInfo.sy @0x01F83BC8 (from input)    */
    float              pc_special_cam_h;  /* @0x01F83810 (PC only) */
    float              pc_special_watch_y;/* @0x01F83814 (PC only) */
    float              view_mat[4][4];    /* vwViewPointInfo.vwcoord.coord @0x01F83C2C   */
    float              frame_time;        /* gFrametime  @0x00935948 */
    int32_t            frame_count;       /*             @0x00935954 */
    float              frame_rate;        /*             @0x00935958 */
    int                last_cam_mv_type;  /* port only */
    int                bss_cleared;
    SH2CamInput        in_default;
    const SH2CamInput *in;
    SH2CamCallbacks    cb;
} g_vc;

#define IN (g_vc.in)

static void vcAssert(int pc_line)
{
    /* PC: printf("assert error = %s / line: %d") then an infinite loop. */
    if (g_vc.cb.on_assert) g_vc.cb.on_assert(g_vc.cb.user, pc_line);
}

/* ===================================================================== */
/* Engine glue replaced by inputs (PC helpers outside the module)         */
/* ===================================================================== */

static float vcFrameTime(void)  { return g_vc.frame_time; }   /* FUN_004479d0 */
static int   vcFrameCount(void) { return g_vc.frame_count; }  /* FUN_004479b0 */
static float vcFrameRate(void)  { return g_vc.frame_rate; }   /* FUN_004479e0 */

/* PC 0x00447960: x = elapsed 60 Hz ticks.  Pause (0x00447a40) zeroes everything. */
static void vcSetFrameRate(float x)
{
    g_vc.frame_count = (int)x;
    g_vc.frame_time  = x / 60.0f;                        /* 0x00935934 = 60.0 */
    g_vc.frame_rate  = (x != 0.0f) ? 60.0f / x : 0.0f;
}

/* FUN_00494d80(0): stage->glb_crd is 0x11 or 1..4 (outdoor town stages) */
static int vcIsOutdoorStage(void)
{
    int s = IN->stage_glb_crd;
    return (s == 0x11) || (s >= 1 && s <= 4);
}

/* Repeated inline test: stage 1 and James.x >= -20000 ("wide" part of stage 1) */
static int vcIsStage1Wide(void)
{
    return IN->stage_glb_crd == 1 && !(IN->james_pos_x < -20000.0f);   /* 0x006A91D8 */
}

static int vcIsOnBoat(void) { return (IN->james_status & 0x20000u) != 0; }

/* FUN_004ee820: gTransitionState == 0 || == 2 */
static int vcTransitionIdle(void)
{
    return IN->transition_state == 0 || IN->transition_state == 2;
}

/* FUN_00542920(i, type): character slot i if its category byte matches (2 enemy, 7 item) */
static SH2CamChara *vcGetCharaSlot(int i, int type)
{
    SH2CamChara *c;
    int cat;
    if (IN->chara_slots_disabled || IN->chara_slots == NULL) return NULL;
    c = &IN->chara_slots[i];
    cat = (int)(int8_t)(uint8_t)((uint16_t)c->kind >> 8);
    if (type == 0) return (cat == 2) ? c : NULL;
    if (type == 1) return (cat == 7) ? c : NULL;
    return NULL;
}

/* FUN_00522410 vcRetPrioStgEvnt: game event flags -> callback */
static int vcRetPrioStgEvnt(int glb_crd)
{
    return g_vc.cb.ret_prio_stg_evnt ? g_vc.cb.ret_prio_stg_evnt(g_vc.cb.user, glb_crd) : 0;
}

/* ===================================================================== */
/* Math wrappers (PC 0x00401a60.., 0x00448420..)                         */
/* ===================================================================== */

/* FUN_00448470(x, y) = fpatan -> atan2(y, x).  Kept in PC argument order. */
static float shAtan2XY(float x, float y) { return atan2f(y, x); }

/* FUN_00448460(v) = atan2(v[0], v[2]) : yaw of a vector */
static float shVecAngY(const float *v) { return atan2f(v[0], v[2]); }

/* FUN_00448420: wrap to (-PI, PI] via floor (float-rounded product, double floor arg) */
static float shNormRad(float ang)
{
    double prod = (double)ang * (double)0.159154937f;        /* 0x00630DE8 1/(2PI) */
    float  yf   = (float)prod;
    double frac = (double)yf - floor(prod);
    if (frac > 0.5) frac -= 1.0;                              /* 0x0062FDEC, 0x0062EDF4 */
    return (float)(frac * (double)6.28318548f);              /* 0x00630DE4 */
}

/* Inline copy of the normalizer used by vc_calc/vc_newest (float rounded before floor) */
static float shNormRadInline(float ang)
{
    float  yf   = ang * 0.159154937f;                        /* 0x00630DE8 */
    double frac = (double)yf - floor((double)yf);            /* FUN_00401a80 */
    if (frac > 0.5) frac -= 1.0;
    return (float)(frac * (double)6.28318548f);
}

/* FUN_004484c0: (sin a, 0, cos a, 0) */
static void shSinCosVec(float *out, float a)
{
    out[1] = 0.0f; out[3] = 0.0f;
    out[0] = sinf(a);
    out[2] = cosf(a);
}

static float shXZLen(const float *v) { return sqrtf(v[0] * v[0] + v[2] * v[2]); }   /* FUN_00485bb0 */

static float shXZDist(const float *a, const float *b)                                /* FUN_00483840 */
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}

static float shMax(float a, float b) { return (b < a) ? a : b; }                     /* FUN_00485a20 */

static void v4copy(float *d, const float *s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }

/* ===================================================================== */
/* D3DX helpers (PSGP default table @0x007AF9C8)                         */
/* ===================================================================== */

typedef float VcMat[4][4];

static void d3dxIdentity(VcMat m) { memcpy(m, s_identity, sizeof(VcMat)); }

/* slot 3 (0x00419947): D3DXMatrixMultiply(out, a, b) = a * b, aliasing safe */
static void d3dxMatMul(VcMat out, const VcMat a, const VcMat b)
{
    VcMat t;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[i][j] = a[i][3] * b[3][j] + a[i][0] * b[0][j] + a[i][2] * b[2][j] + b[1][j] * a[i][1];
    memcpy(out, t, sizeof(VcMat));
}

static void d3dxRotX(VcMat m, float a)   /* slot 30 (0x0041a48a) */
{
    float s = sinf(a), c = cosf(a);
    d3dxIdentity(m);
    m[1][1] = c; m[1][2] = s; m[2][1] = -s; m[2][2] = c;
}

static void d3dxRotY(VcMat m, float a)   /* slot 31 (0x0041a526) */
{
    float s = sinf(a), c = cosf(a);
    d3dxIdentity(m);
    m[0][0] = c; m[0][2] = -s; m[2][0] = s; m[2][2] = c;
}

static void d3dxRotZ(VcMat m, float a)   /* slot 32 (0x0041a5c3) */
{
    float s = sinf(a), c = cosf(a);
    d3dxIdentity(m);
    m[0][0] = c; m[0][1] = s; m[1][0] = -s; m[1][1] = c;
}

static void d3dxTranspose(VcMat out, const VcMat in)   /* slot 37 (0x00419b90) */
{
    VcMat t;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[i][j] = in[j][i];
    memcpy(out, t, sizeof(VcMat));
}

/* slot 6 (0x00419240): D3DXVec3TransformNormal(out, v, m) */
static void d3dxVec3TransformNormal(float *out, const float *v, const VcMat m)
{
    float x = m[2][0] * v[2] + m[1][0] * v[1] + v[0] * m[0][0];
    float y = m[2][1] * v[2] + m[1][1] * v[1] + m[0][1] * v[0];
    float z = m[2][2] * v[2] + m[1][2] * v[1] + m[0][2] * v[0];
    out[0] = x; out[1] = y; out[2] = z;
}

/* FUN_00448570(out, in, ang): out = in * RotY(ang) */
static void shMatMulRotY(VcMat out, const VcMat in, float ang)
{
    VcMat r;
    d3dxRotY(r, ang);
    d3dxMatMul(out, in, r);
}

/* FUN_004486e0(out, a, b): out = b * a */
static void shMatMulRev(VcMat out, const VcMat a, const VcMat b)
{
    d3dxMatMul(out, b, a);
}

/* ===================================================================== */
/* vb_main / vw_calc (view math)                                         */
/* ===================================================================== */

/* vbTransposeMatrixWithoutTr  PC 0x005201c0 */
static void vbTransposeMatrixWithoutTr(VcMat m0, const VcMat m1)
{
    float tr[4];
    v4copy(tr, m1[3]);
    d3dxTranspose(m0, m1);
    m0[3][0] = tr[0]; m0[3][1] = tr[1]; m0[3][2] = tr[2]; m0[3][3] = tr[3];
    m0[0][3] = 0.0f; m0[1][3] = 0.0f; m0[2][3] = 0.0f; m0[3][3] = 1.0f;
}

/* vbApplyMatrixWithoutTr  PC 0x00520220 */
static void vbApplyMatrixWithoutTr(float *v0, const VcMat m0, const float *v1)
{
    d3dxVec3TransformNormal(v0, v1, m0);
    v0[3] = 1.0f;
}

/* vwLimitOverLimVector  PC 0x00520940 */
static void vwLimitOverLimVector(float *vec_xz, float lim_vec_len, float lim_vec_ang_y)
{
    float dir[4], over_spd;
    shSinCosVec(dir, lim_vec_ang_y);
    over_spd = (dir[2] * vec_xz[2] + dir[0] * vec_xz[0]) - lim_vec_len;
    if (over_spd > 0.0f) {
        vec_xz[0] = vec_xz[0] - dir[0] * over_spd;
        vec_xz[2] = vec_xz[2] - dir[2] * over_spd;
    }
}

/* vwDecreaseSideOfVector  PC 0x005209a0 */
static void vwDecreaseSideOfVector(float *vec_xz, float dec_val, float max_side_vec_len, float dir_ang_y)
{
    float side_ang = dir_ang_y + 1.57079637f;                       /* 0x006A14C4 */
    float sv_val = cosf(side_ang) * vec_xz[2] + sinf(side_ang) * vec_xz[0];
    float side_val = sv_val;
    if (side_val < -max_side_vec_len) side_val = -max_side_vec_len;
    else if (side_val > max_side_vec_len) side_val = max_side_vec_len;
    if (side_val > dec_val)       side_val = side_val - dec_val;
    else if (side_val < -dec_val) side_val = side_val + dec_val;
    else                          side_val = 0.0f;
    vec_xz[0] = sinf(side_ang) * (side_val - sv_val) + vec_xz[0];
    vec_xz[2] = cosf(side_ang) * (side_val - sv_val) + vec_xz[2];
}

/* vwAngleToVector  PC 0x00520e10 */
static void vwAngleToVector(float *vec, const float *ang, float r)
{
    float entou_r = cosf(ang[0]) * r;
    vec[1] = -(sinf(ang[0]) * r);
    vec[0] = sinf(ang[1]) * entou_r;
    vec[2] = cosf(ang[1]) * entou_r;
}

/* vwVectorToAngle  PC 0x00520e60: returns the vector length */
static float vwVectorToAngle(float *ang, const float *vec)
{
    float zz = vec[2] * vec[2], y = vec[1], xx = vec[0] * vec[0];
    ang[0] = shAtan2XY(sqrtf(xx + zz), -vec[1]);
    ang[1] = shVecAngY(vec);
    ang[2] = 0.0f;
    return sqrtf(y * y + xx + zz);
}

/* vwOresenHokan  PC 0x00520ee0: piecewise linear table lookup (integer math) */
static int vwOresenHokan(const int32_t *y_ary, int y_suu, int input_x, int min_x, int max_x)
{
    int kukan_w, kukan_no, amari;
    if (max_x <= input_x) return y_ary[y_suu - 1];
    if (input_x < min_x) return y_ary[0];
    kukan_w  = (max_x - min_x) / (y_suu - 1);
    kukan_no = (input_x - min_x) / kukan_w;
    amari    = (input_x - min_x) % kukan_w;
    if (y_suu - 1 <= kukan_no) return y_ary[y_suu - 1];
    if (kukan_no < 0) return y_ary[0];
    return ((kukan_w - amari) * y_ary[kukan_no] + y_ary[kukan_no + 1] * amari) / kukan_w;
}

/* vwRotMatrixYXZ  PC 0x00520f70: mat = RotZ(z) * RotX(x) * RotY(y) * mat */
static void vwRotMatrixYXZ(const float *rot, VcMat mat)
{
    float r[3];
    VcMat my, mx, t, mz;
    int i;
    for (i = 0; i < 3; i++)
        r[i] = (fabsf(rot[i]) < 1.00000002e+20f) ? rot[i] : 0.0f;   /* 0x006C9350 */
    d3dxRotY(my, r[1]);
    d3dxRotX(mx, r[0]);
    d3dxMatMul(t, mx, my);
    d3dxRotZ(mz, r[2]);
    d3dxMatMul(my, mz, t);
    d3dxMatMul(mat, my, mat);
}

/* vwMatrixToAngleYXZ  PC 0x00521040 (works on the transpose, like the PC, in place) */
static void vwMatrixToAngleYXZ(float *ang, VcMat mat)
{
    float r_xz;
    d3dxTranspose(mat, mat);
    r_xz = sqrtf(mat[2][2] * mat[2][2] + mat[0][2] * mat[0][2]);
    ang[0] = shAtan2XY(r_xz, -mat[1][2]);
    if (ang[0] == 1.57079637f) {                                     /* 0x006A14C4 */
        ang[2] = 0.0f;
        ang[1] = shAtan2XY(mat[2][1], mat[0][1]);
    } else if (ang[0] == -1.57079637f) {                             /* 0x006B61C4 */
        ang[2] = 0.0f;
        ang[1] = shAtan2XY(-mat[2][1], -mat[0][1]);
    } else {
        ang[2] = shAtan2XY(mat[1][1], mat[1][0]);
        ang[1] = shAtan2XY(mat[2][2], mat[0][2]);
    }
    d3dxTranspose(mat, mat);
}

/* vwRet3DLength  PC 0x00521130 */
static float vwRet3DLength(float x, float y, float z)
{
    return sqrtf(z * z + y * y + x * x);
}

/* ===================================================================== */
/* vc_calc                                                               */
/* ===================================================================== */

/* vcTransRotRoadArea  PC 0x005214f0: v0 = (v1 + m.row3) * m (rotation only) */
static void vcTransRotRoadArea(float *v0, const VcMat m, const float *v1)
{
    v0[0] = m[3][0] + v1[0];
    v0[1] = m[3][1] + v1[1];
    v0[2] = m[3][2] + v1[2];
    v0[3] = m[3][3] + v1[3];
    vbApplyMatrixWithoutTr(v0, m, v0);
}

/* vcRotTransRoadArea  PC 0x00521530: v0 = v1 * m (rotation only) + m.row3 */
static void vcRotTransRoadArea(float *v0, const VcMat m, const float *v1)
{
    vbApplyMatrixWithoutTr(v0, m, v1);
    v0[0] = m[3][0] + v0[0];
    v0[1] = m[3][1] + v0[1];
    v0[2] = m[3][2] + v0[2];
    v0[3] = m[3][3] + v0[3];
}

/* vcGetMinInRoadDist  PC 0x00521570 */
static float vcGetMinInRoadDist(void)
{
    return vcIsOutdoorStage() ? 500.0f : 400.0f;   /* 0x0062D230, 0x006A91C8 */
}

/* vcConvertCamFile  PC 0x00521590: replace authoring sentinels in a loaded .cam array */
static void vcConvertCamFile(_VC_ROAD_DATA *road_ary)
{
    _VC_ROAD_DATA *rd_p = road_ary;
    if (rd_p->flags & VC_RD_END_DATA_F) return;
    do {
        rd_p->kind_id = (int32_t)(int16_t)rd_p->kind_id;

        if (rd_p->ofs_watch_hy == 10000.0f)      rd_p->ofs_watch_hy = -300.0f;
        else if (rd_p->ofs_watch_hy == 10001.0f) rd_p->ofs_watch_hy = -375.0f;
        else if (rd_p->ofs_watch_hy == 10002.0f) rd_p->ofs_watch_hy = -600.0f;

        if (rd_p->projection == 0.0f)            rd_p->projection = 448.0f;
        else if (rd_p->projection == 10001.0f)   rd_p->projection = 384.0f;

        if (rd_p->trace_btm_hy == 10000.0f)      rd_p->trace_btm_hy = -300.0f;
        else if (rd_p->trace_btm_hy == 10001.0f) rd_p->trace_btm_hy = -150.0f;
        else if (rd_p->trace_btm_hy == 10002.0f) rd_p->trace_btm_hy = 150.0f;

        if (rd_p->cam_mv_type == VC_MV_CHASE) {
            if (rd_p->tmp.chs.ofs_hy == 10000.0f) rd_p->tmp.chs.ofs_hy = -500.0f;
            if (rd_p->tmp.chs.ofs_hy == 10001.0f) rd_p->tmp.chs.ofs_hy = -125.0f;
        } else if (rd_p->cam_mv_type == VC_MV_FIX_ANG) {
            if (rd_p->tmp.fix.ang_x == 10000.0f)  rd_p->tmp.fix.ang_x = -0.52359879f;
        }
        rd_p++;
    } while (!(rd_p->flags & VC_RD_END_DATA_F));
}

/* vcRetNearRatioSwitchAreaInXZPos  PC 0x00521680 (PC passes cur_near_road by value) */
static float vcRetNearRatioSwitchAreaInXZPos(_VC_NEAR_ROAD_DATA cur_near_road, const float *chr_pos,
                                             const float *cam_tgt_pos)
{
    float ppos[4], ofs_ang_y;
    ofs_ang_y = shNormRadInline(shAtan2XY(chr_pos[2] - cam_tgt_pos[2], chr_pos[0] - cam_tgt_pos[0]));
    vcTransRotRoadArea(ppos, cur_near_road.sw_rzm, chr_pos);
    if (cur_near_road.rd_dir_type == 0) {
        if (0.0f <= ofs_ang_y && ofs_ang_y < 3.14159274f)
            return (cur_near_road.sw.max_hx - ppos[0]) / (cur_near_road.sw.max_hx - cur_near_road.sw.min_hx);
        return (ppos[0] - cur_near_road.sw.min_hx) / (cur_near_road.sw.max_hx - cur_near_road.sw.min_hx);
    }
    if (cur_near_road.rd_dir_type == 1) {
        if (-1.57079637f <= ofs_ang_y && ofs_ang_y < 1.57079637f)
            return (cur_near_road.sw.max_hz - ppos[2]) / (cur_near_road.sw.max_hz - cur_near_road.sw.min_hz);
        return (ppos[2] - cur_near_road.sw.min_hz) / (cur_near_road.sw.max_hz - cur_near_road.sw.min_hz);
    }
    return 0.0f;
}

/* vcRetNearRatioSwitchAreaForCircleCam  PC 0x00521810 (cur_near_road and cir_man by value) */
static float vcRetNearRatioSwitchAreaForCircleCam(_VC_NEAR_ROAD_DATA cur_near_road, _VC_CIR_CAM_MANAGER cir_man,
                                                  const float *chr_pos)
{
    float ppos[4], ofs_ang_y;
    ofs_ang_y = shNormRad(shAtan2XY(chr_pos[2] - cir_man.origin[2], chr_pos[0] - cir_man.origin[0]));
    vcTransRotRoadArea(ppos, cur_near_road.sw_rzm, chr_pos);   /* PC computes it and never uses it */
    (void)ppos;
    if ((-2.3561945f <= ofs_ang_y && ofs_ang_y < -0.785398185f) ||   /* 0x006C9E8C, 0x006C9E88 */
        (0.785398185f <= ofs_ang_y && ofs_ang_y < 2.3561945f))        /* 0x0069DB50, 0x006A7914 */
        return fabsf((cir_man.origin[0] - chr_pos[0]) / cir_man.sw_l[0]);
    return fabsf((cir_man.origin[2] - chr_pos[2]) / cir_man.sw_l[2]);
}

/* ===================================================================== */
/* vc_newest (PC 0x00521c20..)                                           */
/* ===================================================================== */

/* vcEndProcessingVC_WORK  PC 0x00521c20 */
static void vcEndProcessingVC_WORK(void)
{
    g_vc.work.flags &= ~0x3cu;
    g_vc.work.through_door_activate_init_f = 0;
}

/* vcEndProcessingOldNearRoad  PC 0x00521c40 */
static void vcEndProcessingOldNearRoad(_VC_NEAR_ROAD_DATA *old_cur_p, _VC_WORK *w_p)
{
    (void)old_cur_p;
    if (w_p->fix_man.exception_f != 0) w_p->fix_man.exception_f = 0;
}

/* vcCorrectCamMatAngForcibly  PC 0x00521c60 (road record passed by value) */
static void vcCorrectCamMatAngForcibly(float *ofs_cam_ang, _VC_ROAD_DATA rd_data)
{
    float lr = rd_data.tmp.chs.lr_lim_ang_y;   /* +0x78: same slot as stl.lr_lim_ang_y */
    float rr = rd_data.tmp.chs.rr_lim_ang_y;   /* +0x7C */
    float ofs_lr, ofs_rr;
    if (rd_data.cam_mv_type == VC_MV_CHASE) {
        if (!(rd_data.flags & VC_RD_CAM_LIKE_SETTLE_F)) return;
    } else if (rd_data.cam_mv_type != VC_MV_SETTLE) {
        return;
    }
    if (lr == -17.4515476f && rr == 17.4515476f) return;            /* 0x006CB4E4, 0x006CB4E0: "no limit" */
    ofs_lr = shNormRad(ofs_cam_ang[1] - lr);
    ofs_rr = shNormRad(ofs_cam_ang[1] - rr);
    if (ofs_lr >= 0.0f && ofs_rr <= 0.0f) {
        ofs_cam_ang[1] = shNormRad(ofs_cam_ang[1]);
        return;
    }
    if (fabsf(ofs_lr) < fabsf(ofs_rr)) ofs_cam_ang[1] = shNormRad(lr);
    else                               ofs_cam_ang[1] = shNormRad(rr);
}

/* vcWarpForFixAngCam  PC 0x00521d70 */
static void vcWarpForFixAngCam(_VC_WORK *w_p)
{
    _VC_ROAD_DATA *rd = w_p->cur_near_road.road_p;
    float actual_ang_y = shNormRad(rd->tmp.fix.ang_y + w_p->fix_man.add_ang_y);
    float ofs_ang_y = fabsf(shNormRad(w_p->chara_eye_ang_y - actual_ang_y));
    if (ofs_ang_y >= 2.3561945f && rd->kind_id == 1) {               /* 0x006A7914 */
        w_p->flags |= VC_ALL_WARP_FLAGS;
        w_p->fix_man.exception_f = (w_p->fix_man.exception_f == 0);
        if (w_p->fix_man.exception_f == 0) w_p->fix_man.add_ang_y = w_p->fix_man.add_ang_y - 3.14159274f;
        else                               w_p->fix_man.add_ang_y = w_p->fix_man.add_ang_y + 3.14159274f;
        if (w_p->cur_near_road.rd_dir_type == 0) {
            if (-1.57079637f <= actual_ang_y && actual_ang_y < 1.57079637f)
                w_p->fix_man.add_rd_z = w_p->fix_man.add_rd_z + 800.0f;   /* 0x006C8890 */
            else
                w_p->fix_man.add_rd_z = w_p->fix_man.add_rd_z - 800.0f;
        } else if (w_p->cur_near_road.rd_dir_type == 1) {
            if (actual_ang_y > 0.0f) w_p->fix_man.add_rd_x = w_p->fix_man.add_rd_x + 800.0f;
            else                     w_p->fix_man.add_rd_x = w_p->fix_man.add_rd_x - 800.0f;
        }
    }
}

/* vcRetCirRadiusReduction  PC 0x00521eb0 */
static float vcRetCirRadiusReduction(_VC_WORK *w_p)
{
    float x = w_p->cam_pos[0] - w_p->chara_pos[0];
    float z = w_p->cam_pos[2] - w_p->chara_pos[2];
    float cam2chr_xz_dist = sqrtf(x * x + z * z);
    float half = 0.5f;
    float cam2chr_ang = shAtan2XY(w_p->chara_pos[2] - w_p->cam_pos[2], w_p->chara_pos[0] - w_p->cam_pos[0]);
    float deflt_r = cir_reduction_r[w_p->cur_near_road.road_p->area_size_type];
    float rx = sinf(cam2chr_ang) * (cam2chr_xz_dist * half) + sinf(w_p->chara_eye_ang_y) * deflt_r;
    float rz = cosf(cam2chr_ang) * (cam2chr_xz_dist * half) + cosf(w_p->chara_eye_ang_y) * deflt_r;
    float real_r = sqrtf(rx * rx + rz * rz);
    float ofs_ang_y = shNormRadInline(w_p->chara_eye_ang_y - cam2chr_ang);
    return (1.0f - fabsf(ofs_ang_y * 0.318309873f)) * real_r;        /* 0x006B34A8 1/PI */
}

/* vcChangeProjByDist  PC 0x00521ff0 (writes the road record's projection, like the PC) */
static void vcSetProjectionValueInternal(float new_proj, int framecnt);
static void vcChangeProjByDist(_VC_NEAR_ROAD_DATA *near_rd_p, float mv_vec_y)
{
    _VC_ROAD_DATA *rd = near_rd_p->road_p;
    float r;
    if (rd->area_size_type == VC_AREA_OUTDOOR && (rd->mv_y_type == 4 || rd->mv_y_type == 5)) {
        r = (IN->area_code < 0x35) ? 937.5f : 812.5f;                /* 0x006C7A80, 0x006C7A7C */
        rd->projection = fabsf((-437.5f - r) - mv_vec_y) * 1.20000005f + 448.0f;  /* 0x006CB4EC, 0x006A2AF4, 0x006CB4E8 */
        vcSetProjectionValueInternal(rd->projection, 0);
    }
}

/* PC 0x00521a70 (PC only): special watch height for one room (see vcPcSpecialRoom) */
static void vcPcSpecialRoomWatchY(_VC_WORK *w_p)
{
    float ofs_ang_y, dist, dx, dz;
    g_vc.pc_special_cam_h = w_p->chara_top_y + 625.0f;               /* 0x006BC1BC */
    w_p->ideal_cam_pos_h = g_vc.pc_special_cam_h;
    ofs_ang_y = fabsf(shNormRad(shAtan2XY(w_p->cam_pos[2] - w_p->chara_pos[2], w_p->cam_pos[0] - w_p->chara_pos[0])
                                - w_p->chara_eye_ang_y));
    dx = w_p->cam_pos[0] - w_p->chara_pos[0];
    dz = w_p->cam_pos[2] - w_p->chara_pos[2];
    dist = sqrtf(dx * dx + dz * dz);
    if (dist >= 1650.0f) {                                           /* 0x006A1460 */
        if ((w_p->flags & VC_PRS_F_VIEW_F) && ofs_ang_y < 0.785398185f) {
            g_vc.pc_special_watch_y = (w_p->chara_top_y -
                (45.0f - ofs_ang_y * 0.318309873f * 180.0f) * 12.5f) + 300.0f;   /* 0x006BC0AC, 0x0069F348, 0x006CA9A4, 0x0069F3DC */
            return;
        }
        g_vc.pc_special_watch_y = w_p->chara_top_y + 300.0f;
    } else {
        g_vc.pc_special_watch_y = (w_p->chara_top_y - (3.29999995f - 0.00200000009f * dist) * 250.0f) + 300.0f; /* 0x006CA9A8, 0x0063CFD4 */
        if (dist < 550.0f)                                           /* 0x006A4C58 */
            g_vc.pc_special_watch_y = w_p->chara_top_y - 250.0f;
    }
}

/* PC 0x00521bb0 (PC only): stage 0x0D, room 0xBB event camera height */
static void vcPcSpecialRoom(_VC_WORK *w_p, int glb_crd)
{
    if (glb_crd != 0xd) return;
    if (g_vc.cb.pc_special_room && g_vc.cb.pc_special_room(g_vc.cb.user)) {
        w_p->flags |= VC_PC_SPECIAL_WATCH_F;
        vcPcSpecialRoomWatchY(w_p);
        return;
    }
    if (w_p->flags & VC_PC_SPECIAL_WATCH_F) w_p->flags &= ~(uint32_t)VC_PC_SPECIAL_WATCH_F;
}

/* ===================================================================== */
/* vc_main: projection                                                    */
/* ===================================================================== */

/* vcSetProjectionValue  PC 0x0051cf00 */
static void vcSetProjectionValueInternal(float new_proj, int framecnt)
{
    VcProjParam *pp = &g_vc.proj;
    pp->new_proj = (new_proj == 0.0f) ? 448.0f : new_proj;
    pp->old_proj = g_vc.scr_z;
    if (g_vc.work.flags & VC_PROJ_MOMENT_CHANGE_F) {
        g_vc.work.flags &= ~(uint32_t)VC_PROJ_MOMENT_CHANGE_F;
        pp->flg = 2;
        return;
    }
    if (framecnt < 2) {
        pp->flg = 2;
        return;
    }
    pp->flg = 1;
    pp->delta = (pp->new_proj - g_vc.scr_z) / (float)framecnt;
}

/* vcChangeProjectionValue  PC 0x0051cf90 */
static void vcChangeProjectionValue(_VC_WORK *w_p)
{
    VcProjParam *pp = &g_vc.proj;
    if (pp->flg == 0) return;
    if (pp->flg != 2) {
        g_vc.scr_z = pp->delta + g_vc.scr_z;
        if (pp->new_proj <= pp->old_proj) {
            if (pp->new_proj <= g_vc.scr_z) goto set_ang;
        } else if (g_vc.scr_z <= pp->new_proj) {
            goto set_ang;
        }
    }
    pp->flg = 0;
    g_vc.scr_z = pp->new_proj;
set_ang:
    /* FUN_0051ff00 copies the engine projection matrix here; renderer side, not ported. */
    w_p->scr_half_ang_wx = shAtan2XY(g_vc.scr_z, g_vc.scr_sx) * 0.5f;
    w_p->scr_half_ang_wy = shAtan2XY(g_vc.scr_z, g_vc.scr_sy) * 0.5f;
}

/* vcCamMatNoise  PC 0x0051cc80 */
static float vcCamMatNoise(float noise_w, float ang_spd1, float ang_spd2)
{
    float a1 = shNormRad(g_vc.self_view_timer * ang_spd1);
    float a2 = shNormRad(g_vc.self_view_timer * ang_spd2);
    return (cosf(a2) + cosf(a1)) * 0.5f * noise_w;
}

/* vcGetXZSumDistFromLimArea  PC 0x0051cce0 */
static float vcGetXZSumDistFromLimArea(float *out_vec_x_p, float *out_vec_z_p, float chk_wld_x, float chk_wld_z,
                                       float lim_min_x, float lim_max_x, float lim_min_z, float lim_max_z,
                                       int can_ret_minus_dist_f)
{
    float x_dist, z_dist, ret_dist;
    if (!(chk_wld_x <= lim_max_x)) {
        *out_vec_x_p = lim_max_x - chk_wld_x;
        x_dist = -(lim_max_x - chk_wld_x);
    } else if (chk_wld_x < lim_min_x) {
        x_dist = lim_min_x - chk_wld_x;
        *out_vec_x_p = x_dist;
    } else {
        *out_vec_x_p = 0.0f;
        if ((lim_min_x + lim_max_x) * 0.5f > chk_wld_x) x_dist = lim_min_x - chk_wld_x;
        else                                           x_dist = chk_wld_x - lim_max_x;
    }
    if (!(chk_wld_z <= lim_max_z)) {
        *out_vec_z_p = lim_max_z - chk_wld_z;
        z_dist = -(lim_max_z - chk_wld_z);
    } else if (chk_wld_z < lim_min_z) {
        z_dist = lim_min_z - chk_wld_z;
        *out_vec_z_p = z_dist;
    } else {
        *out_vec_z_p = 0.0f;
        if ((lim_min_z + lim_max_z) * 0.5f > chk_wld_z) z_dist = lim_min_z - chk_wld_z;
        else                                           z_dist = chk_wld_z - lim_max_z;
    }
    if (x_dist < 0.0f) {
        if (z_dist >= 0.0f || x_dist <= z_dist) ret_dist = z_dist;   /* inside: the larger (closer to 0) */
        else                                    ret_dist = x_dist;
    } else {
        ret_dist = (z_dist >= 0.0f) ? z_dist + x_dist : x_dist;
    }
    if (!can_ret_minus_dist_f && ret_dist < 0.0f) ret_dist = 0.0f;
    return ret_dist;
}

/* ===================================================================== */
/* vc_main: small API (PC 0x00519430..0x005198a0)                         */
/* ===================================================================== */

/* PC 0x00519430 (inline helper): values within +-5e-6 count as zero */
static float vcRetZeroIfTiny(float x)
{
    if (-4.99999987e-06f <= x && x <= 4.99999987e-06f) return 0.0f;  /* 0x0069DB40, 0x0069DB3C */
    return x;
}

/* vcInitVCSystem  PC 0x00519460 */
void vcInitVCSystem(_VC_ROAD_DATA **vc_road_ary_list)
{
    _VC_WORK *w = &g_vc.work;
    w->view_cam_active_f = 0;
    w->cam_noise = 0;
    w->vc_road_ary_list = vc_road_ary_list ? vc_road_ary_list : vcNullRoadArrayList;
    w->cur_near_road = vcNullNearRoad;
    w->old_cam_excl_area_r = -1.0f;
    w->watch_tgt_max_y = 50000.0f;                                   /* 0x47435000 */
    g_vc.self_view_timer = 0.0f;
    memset(&g_vc.watch_mv_prm_st, 0, sizeof g_vc.watch_mv_prm_st);
    memset(&g_vc.cam_mv_prm_st, 0, sizeof g_vc.cam_mv_prm_st);
}

/* vcStartCameraSystem  PC 0x00519510 */
void vcStartCameraSystem(void)
{
    g_vc.work.view_cam_active_f = 1;
}

/* vcUserWatchTarget  PC 0x005195a0 */
void vcUserWatchTarget(const float *watch_tgt_pos, const _VC_WATCH_MV_PARAM *watch_prm_p, float rot_z, int warp_watch_f)
{
    _VC_WORK *w = &g_vc.work;
    uint32_t f = w->flags & ~(uint32_t)VC_VISIBLE_CHARA_F;
    w->flags = f | VC_USER_WATCH_F;
    if (warp_watch_f) w->flags = f | VC_USER_WATCH_F | VC_WARP_WATCH_F;
    w->watch_tgt_pos[0] = watch_tgt_pos[0];
    w->watch_tgt_pos[1] = watch_tgt_pos[1];
    w->watch_tgt_pos[2] = watch_tgt_pos[2];
    w->watch_tgt_ang_z = rot_z;
    w->watch_tgt_pos[3] = watch_tgt_pos[3];
    w->user_watch_mv_prm = watch_prm_p ? *watch_prm_p : watch_mv_prm_user;
}

/* vcUserCamTarget  PC 0x00519650 */
void vcUserCamTarget(const float *cam_tgt_pos, const _VC_CAM_MV_PARAM *cam_prm_p, int warp_cam_f)
{
    _VC_WORK *w = &g_vc.work;
    uint32_t f = w->flags & ~(uint32_t)VC_WARP_CAM_F;
    w->flags = f | VC_USER_CAM_F;
    if (warp_cam_f) w->flags = f | VC_USER_CAM_F | VC_WARP_CAM_F;
    v4copy(w->cam_tgt_pos, cam_tgt_pos);
    w->user_cam_mv_prm = cam_prm_p ? *cam_prm_p : cam_mv_prm_user;
}

/* vcGetNowWatchPos  PC 0x005196f0 */
void vcGetNowWatchPos(float *watch_pos)
{
    _VC_WORK *w = &g_vc.work;
    float cos_x = cosf(w->cam_mat_ang[0]), sin_x = sinf(w->cam_mat_ang[0]);
    float cos_y = cosf(w->cam_mat_ang[1]), sin_y = sinf(w->cam_mat_ang[1]);
    float dy = w->cam_pos[1] - w->watch_tgt_pos[1];
    float dz = w->cam_pos[2] - w->watch_tgt_pos[2];
    float dx = w->cam_pos[0] - w->watch_tgt_pos[0];
    float r = sqrtf(dy * dy + dz * dz + dx * dx);
    watch_pos[0] = r * sin_y * cos_x + w->cam_pos[0];
    watch_pos[2] = cos_y * r * cos_x + w->cam_pos[2];
    watch_pos[1] = w->cam_pos[1] - r * sin_x;
}

/* vcGetNowCamPos  PC 0x005197b0 */
void vcGetNowCamPos(float *cam_pos)
{
    v4copy(cam_pos, g_vc.work.cam_pos);
}

/* vcSetSubjChara  PC 0x005197e0 */
void vcSetSubjChara(const float *chara_pos, float chara_bottom_y, float chara_top_y, float chara_grnd_y,
                    const float *chara_head_pos, float chara_mv_spd, float chara_mv_ang_y,
                    float chara_ang_spd_y, float chara_eye_ang_y, float chara_eye_ang_wy,
                    float chara_watch_xz_r)
{
    _VC_WORK *w = &g_vc.work;
    v4copy(w->chara_pos, chara_pos);
    w->chara_bottom_y = chara_bottom_y;
    w->chara_top_y = chara_top_y;
    w->chara_grnd_y = chara_grnd_y;
    w->chara_center_y = (chara_bottom_y + chara_top_y) * 0.5f;
    v4copy(w->chara_head_pos, chara_head_pos);
    w->chara_mv_spd = chara_mv_spd;
    w->chara_mv_ang_y = chara_mv_ang_y;
    w->chara_ang_spd_y = chara_ang_spd_y;
    w->chara_eye_ang_y = chara_eye_ang_y;
    w->chara_eye_ang_wy = chara_eye_ang_wy;
    w->chara_watch_xz_r = chara_watch_xz_r;
}

/* vcSetAllNpcDeadTimer  PC 0x005198a0 */
static void vcSetAllNpcDeadTimer(void)
{
    int i;
    for (i = 0; i < SH2CAM_CHARA_SLOTS; i++) {
        SH2CamChara *sc_p = vcGetCharaSlot(i, 0);
        if (sc_p != NULL) {
            if (!(sc_p->battle_status & 2u)) sc_p->dead_timer = 0.0f;
            else                             sc_p->dead_timer = vcFrameTime() + sc_p->dead_timer;
            if (sc_p->dead_timer > 10.0f) sc_p->dead_timer = 10.0f;    /* 0x00638834 */
        }
    }
}

/* vcRetSmoothCamMvF  PC 0x00519910 */
static int vcRetSmoothCamMvF(const float *old_pos, const float *now_pos, const float *old_ang, const float *now_ang)
{
    float fn = (float)vcFrameCount();
    float intrpt, dx, dy, dz, mv_vec, rot_y;
    if (!(fn >= 1.0f)) fn = 1.0f;
    else if (fn > 4.0f) fn = 4.0f;                                   /* 0x006318A4 */
    intrpt = (float)(int)fn;
    dx = now_pos[0] - old_pos[0];
    dy = now_pos[1] - old_pos[1];
    dz = now_pos[2] - old_pos[2];
    mv_vec = sqrtf(dx * dx + dz * dz + dy * dy);
    if (!(mv_vec / intrpt <= 100.0f)) return 0;                      /* 0x006A91CC */
    if (!(fabsf(now_ang[0] - old_ang[0]) / intrpt <= 0.34906587f)) return 0;  /* 0x006C8888 */
    rot_y = fabsf(shNormRad(now_ang[1] - old_ang[1])) / intrpt;
    if (!(cosf(now_ang[0]) * rot_y <= 0.52359879f)) return 0;        /* 0x006A91C0 */
    return 1;
}

/* vcRetThroughDoorCamEndF  PC 0x00519a20 */
static int vcRetThroughDoorCamEndF(_VC_WORK *w_p)
{
    _VC_THROUGH_DOOR_CAM_PARAM *prm_p = &w_p->through_door;
    float rail2chara_dist = prm_p->rail_sta_to_chara_dist;
    float abs_ofs_ang_y;
    if (!prm_p->active_f) return 1;
    if (prm_p->timer > 1.20000005f && w_p->nearest_enemy_xz_dist < 600.0f) return 1;   /* 0x006A2AF4, 0x0063B830 */
    if (rail2chara_dist > 800.0f) return 1;                                          /* 0x006C8890 */
    if (rail2chara_dist <= 250.0f) return 0;                                         /* 0x006A91D4 */
    abs_ofs_ang_y = fabsf(shNormRad(w_p->chara_eye_ang_y -
        shAtan2XY(w_p->chara_pos[2] - prm_p->rail_sta_pos[2], w_p->chara_pos[0] - prm_p->rail_sta_pos[0])));
    return (abs_ofs_ang_y > 1.22173047f) ? 1 : 0;                                    /* 0x006C888C */
}

/* vcRetFarWatchRate  PC 0x00519ae0 */
static float vcRetFarWatchRate(int far_watch_button_prs_f, int cur_cam_mv_type, _VC_WORK *w_p)
{
    float dist_rate, abs_ofs_ang_y;
    if ((w_p->cur_near_road.road_p->flags & VC_RD_INVALID_SV_F) || (w_p->flags & (VC_USER_WATCH_F | VC_INHIBIT_FAR_WATCH_F)))
        return 0.0f;
    switch (cur_cam_mv_type) {
    case VC_MV_CHASE:
    case VC_MV_SETTLE:
    case VC_MV_LOCUS_CIRCLE:
        return far_watch_button_prs_f ? 1.0f : 0.0f;
    case VC_MV_SELF_VIEW:
        return 0.0f;
    case VC_MV_FIX_ANG:
        if (far_watch_button_prs_f && !vcIsOutdoorStage())
            vcWarpForFixAngCam(w_p);
        return 0.0f;
    case VC_MV_THROUGH_DOOR:
        if (far_watch_button_prs_f) return 1.0f;
        dist_rate = shMax(0.899999976f - w_p->through_door.rail_sta_to_chara_dist * 0.00112499995f, 0.0f); /* 0x0069F388, 0x006C8898 */
        if (w_p->through_door.rail_sta_to_chara_dist <= 250.0f) return dist_rate;
        abs_ofs_ang_y = fabsf(shNormRad(w_p->chara_eye_ang_y -
            shAtan2XY(w_p->chara_pos[2] - w_p->through_door.rail_sta_pos[2],
                      w_p->chara_pos[0] - w_p->through_door.rail_sta_pos[0])));
        return shMax((1.22173047f - abs_ofs_ang_y) * dist_rate * 0.818511128f, 0.0f);   /* 0x006C888C, 0x006C8894 */
    default:
        vcAssert(0x504);
        return 0.0f;
    }
}

/* vcRetSelfViewEffectRate  PC 0x00519c40 */
static float vcRetSelfViewEffectRate(int cur_cam_mv_type, _VC_WORK *w_p)
{
    float mul_rate, xyz_dist, dx, dy, dz, r1, r2, ret;
    if (cur_cam_mv_type != VC_MV_SELF_VIEW && cur_cam_mv_type != VC_MV_THROUGH_DOOR) return 0.0f;
    mul_rate = (cur_cam_mv_type == VC_MV_SELF_VIEW) ? 1.0f : 0.35f;
    dx = w_p->cam_pos[0] - w_p->chara_head_pos[0];
    dy = w_p->cam_pos[1] - w_p->chara_head_pos[1];
    dz = w_p->cam_pos[2] - w_p->chara_head_pos[2];
    xyz_dist = sqrtf(dy * dy + dz * dz + dx * dx);
    if (xyz_dist < 250.0f)       r1 = mul_rate;
    else if (xyz_dist <= 600.0f) r1 = (600.0f - xyz_dist) * mul_rate * 0.00285714259f;   /* 0x006C889C */
    else                         r1 = 0.0f;
    if (w_p->nearest_enemy_xz_dist > 2000.0f)        r2 = 1.0f;     /* 0x006A7900 */
    else if (w_p->nearest_enemy_xz_dist >= 1000.0f)  r2 = (w_p->nearest_enemy_xz_dist - 1000.0f) * 0.00100000005f;
    else                                             r2 = 0.0f;
    ret = r2 * r1;
    if (ret < 0.0f) return 0.0f;
    if (ret > mul_rate) ret = mul_rate;
    return ret;
}

/* vcSetFlagsByCamMvType  PC 0x00519d70 */
static void vcSetFlagsByCamMvType(int cam_mv_type, float far_watch_rate, int all_warp_f)
{
    _VC_WORK *w = &g_vc.work;
    if (far_watch_rate != 0.0f) {
        w->flags &= ~(uint32_t)VC_VISIBLE_CHARA_F;
    } else {
        switch (cam_mv_type) {
        case VC_MV_CHASE: case VC_MV_SETTLE: case VC_MV_LOCUS_CIRCLE:
            w->flags |= VC_VISIBLE_CHARA_F; break;
        case VC_MV_FIX_ANG: case VC_MV_SELF_VIEW: case VC_MV_THROUGH_DOOR:
            w->flags &= ~(uint32_t)VC_VISIBLE_CHARA_F; break;
        default:
            vcAssert(0x58e); break;
        }
    }
    if (cam_mv_type == VC_MV_SELF_VIEW) w->flags |= VC_WARP_CAM_F | VC_WARP_CAM_TGT_F;
    if (all_warp_f) w->flags |= VC_ALL_WARP_FLAGS;
    if (far_watch_rate != 0.0f) w->flags |= VC_WARP_CAM_TGT_F;
}

/* ===================================================================== */
/* vc_main: nearest enemy / item, road array                              */
/* ===================================================================== */

/* vcSetNearestEnemyDataInVC_WORK  PC 0x00519ec0 */
static void vcSetNearestEnemyDataInVC_WORK(_VC_WORK *w_p)
{
    SH2CamChara *active_min_sc_p = NULL, *all_min_sc_p = NULL, *sc_p;
    float active_min_dist = 7500.0f, all_min_dist = 7500.0f, ofs_xz[4] = { 0, 0, 0, 0 }, xz_dist;
    int i;
    if (IN->james_battle_status & 0x10u) {
        w_p->nearest_enemy_p = NULL;
        w_p->nearest_enemy_xz_dist = 7500.0f;                         /* 0x45EA6000 */
        return;
    }
    for (i = 0; i < SH2CAM_CHARA_SLOTS; i++) {
        sc_p = vcGetCharaSlot(i, 0);
        if (sc_p == NULL) break;
        if (((uint16_t)sc_p->kind & 0xff00u) != 0x200u) continue;
        if (!(sc_p->dead_timer <= 1.5f || !(sc_p->battle_status & 2u))) continue;   /* 0x0069F31C */
        if (sc_p->battle_status & 0x20u) continue;
        ofs_xz[0] = sc_p->pos[0] - w_p->chara_pos[0];
        ofs_xz[2] = sc_p->pos[2] - w_p->chara_pos[2];
        if (!(fabsf(ofs_xz[0]) < 7500.0f) || !(fabsf(ofs_xz[2]) < 7500.0f)) continue;  /* 0x006B34B4 */
        xz_dist = shXZLen(ofs_xz);
        if (xz_dist < all_min_dist) {
            all_min_dist = xz_dist;
            all_min_sc_p = sc_p;
        }
        if ((!(sc_p->battle_status & 4u) || sc_p->is_lock_target) && xz_dist < active_min_dist) {
            active_min_sc_p = sc_p;
            active_min_dist = xz_dist;
        }
    }
    if (active_min_sc_p != NULL) {
        w_p->nearest_enemy_p = active_min_sc_p;
        w_p->nearest_enemy_xz_dist = active_min_dist;
        return;
    }
    w_p->nearest_enemy_p = all_min_sc_p;
    w_p->nearest_enemy_xz_dist = all_min_dist;
}

/* vcSetNearestItemDataInVC_WORK  PC 0x0051a060 */
static void vcSetNearestItemDataInVC_WORK(_VC_WORK *w_p)
{
    SH2CamChara *all_min_sc_p = NULL, *sc_p;
    float all_min_dist = 5000.0f, dx, dz, xz_dist;
    int i;
    if (IN->james_battle_status & 0x20u) {
        w_p->nearest_item_p = NULL;
        w_p->nearest_item_xz_dist = 5000.0f;                          /* 0x459C4000 */
        return;
    }
    for (i = 0; i < SH2CAM_CHARA_SLOTS; i++) {
        sc_p = vcGetCharaSlot(i, 1);
        if (sc_p == NULL) break;
        dx = sc_p->pos[0] - w_p->chara_pos[0];
        dz = sc_p->pos[2] - w_p->chara_pos[2];
        if (fabsf(dx) < 5000.0f && fabsf(dz) < 5000.0f) {             /* 0x006A9224 */
            xz_dist = sqrtf(dx * dx + dz * dz);
            if (xz_dist < all_min_dist) {
                all_min_dist = xz_dist;
                all_min_sc_p = sc_p;
            }
        }
    }
    /* PC quirk: the result goes into the ENEMY fields (+0xA720/+0xA724), not the item fields. */
    w_p->nearest_enemy_p = all_min_sc_p;
    w_p->nearest_enemy_xz_dist = all_min_dist;
}

/* vcRetRoadUsePriority  PC 0x0051a150 */
static int vcRetRoadUsePriority(int rd_type)
{
    switch (rd_type) {
    case VC_RD_TYPE_ROAD:           return 2;
    case VC_RD_TYPE_EFFECT:         return vcRetPrioStgEvnt(IN->stage_glb_crd) ? 0 : 5;
    case VC_RD_TYPE_EVENT:          return vcRetPrioStgEvnt(IN->stage_glb_crd) ? 0 : 6;
    case VC_RD_TYPE_ROAD_PRIO_LOW:  return 1;
    case VC_RD_TYPE_ROAD_PRIO_HIGH: return 3;
    case VC_RD_TYPE_SV_ONLY:        return (int)((g_vc.work.flags >> 7) & 4u);   /* 4 while VC_PRS_F_VIEW_F */
    default:                        return 0;
    }
}

/* vcAdvantageDistOfOldCurRoad  PC 0x0051a1d0 */
static float vcAdvantageDistOfOldCurRoad(_VC_NEAR_ROAD_DATA *old_cur_p)
{
    _VC_ROAD_DATA *rd = old_cur_p->road_p;
    if (rd->flags & VC_RD_NO_EXTRA_AREA_F) return 0.0f;
    switch (rd->rd_type) {
    case VC_RD_TYPE_ROAD: case VC_RD_TYPE_ROAD_PRIO_LOW: case VC_RD_TYPE_ROAD_PRIO_HIGH: case VC_RD_TYPE_SV_ONLY:
        return extra_boundary_width[rd->area_size_type];
    case VC_RD_TYPE_EFFECT: case VC_RD_TYPE_EVENT:
        return 50.0f;                                                /* 0x006A6DAC */
    default:
        vcAssert(0x95c);
        return 0.0f;
    }
}

/* vcSetNearRoadAryByCharaPos  PC 0x0051d220 */
static void vcSetNearRoadAryByCharaPos(_VC_WORK *w_p, _VC_ROAD_DATA **road_ary_list, float half_w, int near_enemy_f)
{
    _VC_ROAD_DATA *road_ary, *rd_p;
    _VC_NEAR_ROAD_DATA *near_rd_p;
    VcMat rd_rm, sw_rm;
    float tmpvec0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float tmpvec1[4], tmpvec2[4], ppos[4];
    float rdrot, swrot;
    float rd_min_x, rd_max_x, rd_min_z, rd_max_z, sw_min_x, sw_max_x, sw_min_z, sw_max_z;
    float ydist;
    uint32_t skip_flag = near_enemy_f ? VC_RD_USE_NO_ENEMY_F : VC_RD_USE_NEAR_ENEMY_F;

    w_p->near_road_suu = 0;
    if (road_ary_list == NULL) return;
    for (; (road_ary = *road_ary_list) != NULL; road_ary_list++) {
        if (road_ary->flags & VC_RD_END_DATA_F) continue;
        rd_p = road_ary;
        do {
            if (!((uint32_t)rd_p->flags & skip_flag)) {
                /* road (camera) area in its own rotated frame */
                rdrot = shAtan2XY(rd_p->lim_rd.z0 - rd_p->lim_rd.z1, rd_p->lim_rd.x0 - rd_p->lim_rd.x1);
                d3dxIdentity(rd_rm);
                shMatMulRotY(rd_rm, rd_rm, shNormRad(-rdrot));
                rd_rm[3][0] = rd_p->lim_rd.x1;
                rd_rm[3][2] = rd_p->lim_rd.z1;
                tmpvec0[0] = rd_p->lim_rd.x0; tmpvec0[2] = rd_p->lim_rd.z0;
                vcTransRotRoadArea(tmpvec1, rd_rm, tmpvec0);
                tmpvec0[0] = rd_p->lim_rd.x2; tmpvec0[2] = rd_p->lim_rd.z2;
                vcTransRotRoadArea(tmpvec2, rd_rm, tmpvec0);
                if (tmpvec1[0] <= tmpvec2[0]) { rd_min_x = tmpvec1[0]; rd_max_x = tmpvec2[0]; }
                else                          { rd_max_x = tmpvec1[0]; rd_min_x = tmpvec2[0]; }
                if (tmpvec1[2] <= tmpvec2[2]) { rd_min_z = tmpvec1[2]; rd_max_z = tmpvec2[2]; }
                else                          { rd_max_z = tmpvec1[2]; rd_min_z = tmpvec2[2]; }

                /* switch area */
                swrot = shAtan2XY(rd_p->lim_sw.z0 - rd_p->lim_sw.z1, rd_p->lim_sw.x0 - rd_p->lim_sw.x1);
                d3dxIdentity(sw_rm);
                shMatMulRotY(sw_rm, sw_rm, shNormRad(-swrot));
                sw_rm[3][0] = rd_p->lim_sw.x1;
                sw_rm[3][2] = rd_p->lim_sw.z1;
                tmpvec0[0] = rd_p->lim_sw.x0; tmpvec0[2] = rd_p->lim_sw.z0;
                vcTransRotRoadArea(tmpvec1, sw_rm, tmpvec0);
                tmpvec0[0] = rd_p->lim_sw.x2; tmpvec0[2] = rd_p->lim_sw.z2;
                vcTransRotRoadArea(tmpvec2, sw_rm, tmpvec0);
                if (tmpvec1[0] <= tmpvec2[0]) { sw_min_x = tmpvec1[0]; sw_max_x = tmpvec2[0]; }
                else                          { sw_max_x = tmpvec1[0]; sw_min_x = tmpvec2[0]; }
                if (tmpvec1[2] <= tmpvec2[2]) { sw_min_z = tmpvec1[2]; sw_max_z = tmpvec2[2]; }
                else                          { sw_max_z = tmpvec1[2]; sw_min_z = tmpvec2[2]; }

                /* PC quirk: the character is transformed with the SWITCH matrix only and that
                   point is tested against both the switch and the road bounds. */
                tmpvec0[0] = w_p->chara_pos[0]; tmpvec0[2] = w_p->chara_pos[2];
                vcTransRotRoadArea(ppos, sw_rm, tmpvec0);

                if ((sw_min_x <= ppos[0] + half_w && ppos[0] - half_w <= sw_max_x &&
                     sw_min_z <= ppos[2] + half_w && ppos[2] - half_w <= sw_max_z) ||
                    (rd_min_x <= ppos[0] + half_w && ppos[0] - half_w <= rd_max_x &&
                     rd_min_z <= ppos[2] + half_w && ppos[2] - half_w <= rd_max_z)) {
                    if (w_p->near_road_suu >= 128) {   /* PC asserts after writing entry 128 (overflow) */
                        vcAssert(0x7a8);
                        return;
                    }
                    near_rd_p = &w_p->near_road_ary[w_p->near_road_suu];
                    near_rd_p->road_p = rd_p;
                    near_rd_p->chara2road_sum_dist = vcGetXZSumDistFromLimArea(
                        &near_rd_p->chara2road_vec_x, &near_rd_p->chara2road_vec_z, ppos[0], ppos[2],
                        sw_min_x, sw_max_x, sw_min_z, sw_max_z, rd_p->flags & VC_RD_MARGE_ROAD_F);
                    near_rd_p->sw.min_hy = rd_p->lim_sw.min_hy;
                    near_rd_p->sw.max_hy = rd_p->lim_sw.max_hy;
                    near_rd_p->rd.min_hy = rd_p->lim_rd.min_hy;
                    near_rd_p->rd.max_hy = rd_p->lim_rd.max_hy;
                    if (w_p->chara_pos[1] <= near_rd_p->sw.min_hy) {
                        if (near_rd_p->sw.max_hy < w_p->chara_pos[1]) ydist = 0.0f;
                        else ydist = fabsf(near_rd_p->sw.min_hy - w_p->chara_pos[1]);  /* PC quirk: min_hy, not max_hy */
                    } else {
                        ydist = fabsf(w_p->chara_pos[1] - near_rd_p->sw.min_hy);
                    }
                    near_rd_p->chara2road_sum_dist = ydist + near_rd_p->chara2road_sum_dist;
                    near_rd_p->rd_dir_type = rd_p->rd_dir_type;
                    near_rd_p->use_priority = vcRetRoadUsePriority(rd_p->rd_type);
                    near_rd_p->rd.min_hx = rd_min_x; near_rd_p->rd.max_hx = rd_max_x;
                    near_rd_p->rd.min_hz = rd_min_z; near_rd_p->rd.max_hz = rd_max_z;
                    near_rd_p->sw.min_hx = sw_min_x; near_rd_p->sw.max_hx = sw_max_x;
                    near_rd_p->sw.min_hz = sw_min_z; near_rd_p->sw.max_hz = sw_max_z;
                    memcpy(near_rd_p->rd_rzm, rd_rm, sizeof(VcMat));
                    d3dxIdentity(rd_rm);
                    shMatMulRotY(rd_rm, rd_rm, rdrot);
                    rd_rm[3][0] = -rd_p->lim_rd.x1;
                    rd_rm[3][2] = -rd_p->lim_rd.z1;
                    memcpy(near_rd_p->rd_rim, rd_rm, sizeof(VcMat));
                    memcpy(near_rd_p->sw_rzm, sw_rm, sizeof(VcMat));
                    d3dxIdentity(sw_rm);
                    shMatMulRotY(sw_rm, sw_rm, swrot);
                    sw_rm[3][0] = -rd_p->lim_sw.x1;
                    sw_rm[3][2] = -rd_p->lim_sw.z1;
                    memcpy(near_rd_p->sw_rim, sw_rm, sizeof(VcMat));
                    w_p->near_road_suu++;
                }
            }
            rd_p++;
        } while (!(rd_p->flags & VC_RD_END_DATA_F));
    }
}

/* vcGetNearestNEAR_ROAD_DATA  PC 0x0051d850 */
static float vcGetNearestNEAR_ROAD_DATA(_VC_NEAR_ROAD_DATA **out_nearest_p_addr, int chk_type, int rd_type,
                                        const float *pos, _VC_WORK *w_p, int chk_only_set_marge_f)
{
    _VC_NEAR_ROAD_DATA *n_rd_p, *nearest_p = NULL;
    float min_sum_dist = 3.40282347e+38f;
    float min_x, max_x, min_z, max_z, cppos[4], dummy, dist;
    for (n_rd_p = w_p->near_road_ary; n_rd_p < w_p->near_road_ary + w_p->near_road_suu; n_rd_p++) {
        if (n_rd_p->road_p->rd_type != rd_type) continue;
        if (chk_only_set_marge_f && !(n_rd_p->road_p->flags & VC_RD_MARGE_ROAD_F)) continue;
        if (chk_type == VC_CHK_NEAREST_ROAD_TYPE) {
            min_x = n_rd_p->rd.min_hx; max_x = n_rd_p->rd.max_hx; min_z = n_rd_p->rd.min_hz; max_z = n_rd_p->rd.max_hz;
        } else if (chk_type == VC_CHK_NEAREST_SWITCH_TYPE) {
            min_x = n_rd_p->sw.min_hx; max_x = n_rd_p->sw.max_hx; min_z = n_rd_p->sw.min_hz; max_z = n_rd_p->sw.max_hz;
        } else {
            vcAssert(0x922);
            continue;
        }
        vcTransRotRoadArea(cppos, n_rd_p->sw_rzm, pos);   /* always the switch matrix (PC) */
        dist = vcGetXZSumDistFromLimArea(&dummy, &dummy, cppos[0], cppos[2], min_x, max_x, min_z, max_z,
                                         n_rd_p->road_p->flags & VC_RD_MARGE_ROAD_F);
        if (dist <= min_sum_dist &&
            pos[1] <= n_rd_p->sw.min_hy + 0.5f &&
            n_rd_p->sw.max_hy - 0.5f <= pos[1]) {
            nearest_p = n_rd_p;
            min_sum_dist = dist;
        }
    }
    *out_nearest_p_addr = nearest_p;
    return min_sum_dist;
}

/* One step of the candidate scan in vcGetBestNewCurNearRoad (identical pattern 6 times) */
static void vcConsiderNewCur(_VC_NEAR_ROAD_DATA **new_cur_p, float *new_cur_dist, int *new_cur_priority,
                             _VC_NEAR_ROAD_DATA *cand_p, float cand_dist)
{
    int prio;
    if (cand_p == NULL) return;
    prio = cand_p->use_priority;
    if (*new_cur_priority < prio) {
        if (!(cand_dist <= 0.0f) && !(cand_dist < *new_cur_dist)) return;
    } else if (prio < *new_cur_priority) {
        if (!(*new_cur_dist > 0.0f)) return;
        if (!(cand_dist < *new_cur_dist)) return;
    } else {
        if (!(cand_dist < *new_cur_dist)) return;
    }
    *new_cur_p = cand_p;
    *new_cur_dist = cand_dist;
    *new_cur_priority = prio;
}

/* vcGetBestNewCurNearRoad  PC 0x0051ea50 */
static float vcGetBestNewCurNearRoad(_VC_NEAR_ROAD_DATA **new_cur_pp, int chk_type, const float *pos, _VC_WORK *w_p)
{
    _VC_NEAR_ROAD_DATA *evnt_p, *eff_p, *road_p, *rd_high_p, *rd_low_p, *sv_only_p, *new_cur_p = NULL;
    float evnt_d, eff_d, road_d, rd_high_d, rd_low_d, sv_only_d, dummy;
    float new_cur_dist = 3.40282347e+38f;                            /* 0x006315C0 */
    int new_cur_priority = 0;

    evnt_d    = vcGetNearestNEAR_ROAD_DATA(&evnt_p,    chk_type, VC_RD_TYPE_EVENT,          pos, w_p, 0);
    eff_d     = vcGetNearestNEAR_ROAD_DATA(&eff_p,     chk_type, VC_RD_TYPE_EFFECT,         pos, w_p, 0);
    road_d    = vcGetNearestNEAR_ROAD_DATA(&road_p,    chk_type, VC_RD_TYPE_ROAD,           pos, w_p, 0);
    rd_high_d = vcGetNearestNEAR_ROAD_DATA(&rd_high_p, chk_type, VC_RD_TYPE_ROAD_PRIO_HIGH, pos, w_p, 0);
    rd_low_d  = vcGetNearestNEAR_ROAD_DATA(&rd_low_p,  chk_type, VC_RD_TYPE_ROAD_PRIO_LOW,  pos, w_p, 0);
    sv_only_d = vcGetNearestNEAR_ROAD_DATA(&sv_only_p, chk_type, VC_RD_TYPE_SV_ONLY,        pos, w_p, 0);

    /* PC evaluation order: EVENT, ROAD, EFFECT, PRIO_HIGH, PRIO_LOW, SV_ONLY */
    vcConsiderNewCur(&new_cur_p, &new_cur_dist, &new_cur_priority, evnt_p, evnt_d);
    vcConsiderNewCur(&new_cur_p, &new_cur_dist, &new_cur_priority, road_p, road_d);
    vcConsiderNewCur(&new_cur_p, &new_cur_dist, &new_cur_priority, eff_p, eff_d);
    vcConsiderNewCur(&new_cur_p, &new_cur_dist, &new_cur_priority, rd_high_p, rd_high_d);
    vcConsiderNewCur(&new_cur_p, &new_cur_dist, &new_cur_priority, rd_low_p, rd_low_d);
    vcConsiderNewCur(&new_cur_p, &new_cur_dist, &new_cur_priority, sv_only_p, sv_only_d);

    if (new_cur_p != NULL) {
        *new_cur_pp = new_cur_p;
        return new_cur_dist;
    }
    *new_cur_pp = &vcNullNearRoad;
    return vcGetXZSumDistFromLimArea(&dummy, &dummy, pos[0], pos[2],
                                     vcNullNearRoad.rd.min_hx, vcNullNearRoad.rd.max_hx,
                                     vcNullNearRoad.rd.min_hz, vcNullNearRoad.rd.max_hz,
                                     vcNullNearRoad.road_p->flags & VC_RD_MARGE_ROAD_F);
}

/* switch part of vcSetCurNearRoadInVC_WORK (inline in the PC) */
static void vcSwitchCurNearRoad(_VC_WORK *w_p, _VC_NEAR_ROAD_DATA *new_cur_p, int *ret_warp_f)
{
    int proj_frame;
    if (new_cur_p->road_p->flags & VC_RD_WARP_IN_F) *ret_warp_f = 1;
    if (w_p->cur_near_road.road_p->flags & VC_RD_WARP_OUT_F) *ret_warp_f = 1;
    w_p->cur_near_road = *new_cur_p;
    if (new_cur_p->road_p->proj_sec == 0.0f) proj_frame = 0;
    else proj_frame = (int)(vcFrameRate() * new_cur_p->road_p->proj_sec);
    vcSetProjectionValueInternal(new_cur_p->road_p->projection, proj_frame);
    w_p->flags |= VC_SWITCH_NEAR_RD_DATA_F;
}

/* vcSetCurNearRoadInVC_WORK  PC 0x0051f300: returns the "warp" request */
static int vcSetCurNearRoadInVC_WORK(_VC_WORK *w_p)
{
    _VC_NEAR_ROAD_DATA *new_cur_p, *old_cur_p = NULL, *n_rd_p;
    float new_cur_sum_dist, adv_old_cur_dist, old_cur_sum_dist, old_cur_rd_ang_y, ofs_ang_y;
    int ret_warp_f = 0;

    new_cur_sum_dist = vcGetBestNewCurNearRoad(&new_cur_p, VC_CHK_NEAREST_SWITCH_TYPE, w_p->chara_pos, w_p);
    for (n_rd_p = w_p->near_road_ary; n_rd_p < w_p->near_road_ary + w_p->near_road_suu; n_rd_p++)
        if (n_rd_p->road_p == w_p->cur_near_road.road_p) old_cur_p = n_rd_p;

    if (old_cur_p == NULL) {
        vcSwitchCurNearRoad(w_p, new_cur_p, &ret_warp_f);
        return ret_warp_f;                  /* PC returns here directly */
    }

    adv_old_cur_dist = vcAdvantageDistOfOldCurRoad(old_cur_p);
    if (old_cur_p->use_priority > new_cur_p->use_priority) {
        if (!(adv_old_cur_dist + adv_old_cur_dist <= old_cur_p->chara2road_sum_dist)) {
            w_p->cur_near_road = *old_cur_p;
            goto end;
        }
    } else if (old_cur_p->use_priority < new_cur_p->use_priority) {
        if (new_cur_p->chara2road_sum_dist <= 0.0f) goto switch_new;
    }

    if ((old_cur_p->road_p->flags & VC_RD_NO_EXTRA_AREA_F) &&
        ((old_cur_p->road_p->mv_y_type == 5 && new_cur_p->road_p->mv_y_type == 0) ||
         (old_cur_p->road_p->mv_y_type == 0 && new_cur_p->road_p->mv_y_type == 5)))
        goto switch_new;

    old_cur_sum_dist = old_cur_p->chara2road_sum_dist;
    switch (old_cur_p->rd_dir_type) {
    case 0: case 2: old_cur_rd_ang_y = 0.0f; break;
    case 1:         old_cur_rd_ang_y = 1.57079637f; break;
    default:        vcAssert(0x846); old_cur_rd_ang_y = 0.0f; break;
    }
    ofs_ang_y = shNormRad(w_p->chara_mv_ang_y - old_cur_rd_ang_y);
    if (ofs_ang_y < 0.0f) ofs_ang_y = ofs_ang_y + 3.14159274f;
    if (ofs_ang_y > 1.57079637f) ofs_ang_y = 3.14159274f - ofs_ang_y;
    if (old_cur_sum_dist - adv_old_cur_dist <= new_cur_sum_dist) {
        w_p->cur_near_road = *old_cur_p;
        goto end;
    }
    if (old_cur_sum_dist < 0.0f && ofs_ang_y < 0.34906587f) {        /* 0x006C8888 */
        w_p->cur_near_road = *old_cur_p;
        goto end;
    }
switch_new:
    vcSwitchCurNearRoad(w_p, new_cur_p, &ret_warp_f);
end:
    if ((new_cur_p->road_p->flags & VC_RD_NOT_WARP_F) || (old_cur_p->road_p->flags & VC_RD_NOT_WARP_F))
        ret_warp_f = 0;
    if (w_p->flags & VC_SWITCH_NEAR_RD_DATA_F)
        vcEndProcessingOldNearRoad(old_cur_p, w_p);
    return ret_warp_f;
}

/* vcPreSetDataInVC_WORK  PC 0x0051e880 */
static void vcPreSetDataInVC_WORK(_VC_WORK *w_p, _VC_ROAD_DATA **vc_road_ary_list)
{
    _VC_THROUGH_DOOR_CAM_PARAM *td = &w_p->through_door;
    if (vcFrameTime() != 0.0f) {
        if (w_p->flags & VC_PRS_F_VIEW_F) w_p->flags |= VC_OLD_PRS_F_VIEW_F;
        else                              w_p->flags &= ~(uint32_t)VC_OLD_PRS_F_VIEW_F;
        if (IN->far_view_button)          w_p->flags |= VC_PRS_F_VIEW_F;
        else                              w_p->flags &= ~(uint32_t)VC_PRS_F_VIEW_F;
    }
    w_p->scr_half_ang_wx = shAtan2XY(g_vc.scr_z, g_vc.scr_sx) * 0.5f;
    w_p->scr_half_ang_wy = shAtan2XY(g_vc.scr_z, g_vc.scr_sy) * 0.5f;

    /* vcSetTHROUGH_DOOR_CAM_PARAM_in_VC_WORK (inlined on PC) */
    if (w_p->through_door_activate_init_f) {
        td->rail_ang_y = w_p->chara_eye_ang_y;
        td->rail_sta_pos[1] = w_p->chara_grnd_y - 975.0f;            /* 0x006C88A0 */
        td->active_f = 1;
        td->timer = 0.0f;
        td->rail_sta_pos[0] = w_p->chara_pos[0];
        td->rail_sta_pos[2] = w_p->chara_pos[2];
    }
    if (td->active_f) {
        td->rail_sta_to_chara_dist = shXZDist(w_p->chara_pos, td->rail_sta_pos);
        if (vcTransitionIdle()) {
            td->timer = vcFrameTime() + td->timer;
            if (td->timer <= 0.649999976f)                            /* 0x006C88A8 */
                td->rail_sta_pos[1] = td->rail_sta_pos[1] - vcFrameTime() * 115.0f;   /* 0x006C88A4 */
        }
    }

    vcSetNearestEnemyDataInVC_WORK(w_p);
    if (w_p->nearest_enemy_p == NULL) vcSetNearestItemDataInVC_WORK(w_p);
    vcSetNearRoadAryByCharaPos(w_p, vc_road_ary_list, 10000.0f, w_p->nearest_enemy_p != NULL);   /* 0x461C4000 */
    if (!vcIsOutdoorStage() && IN->stage_glb_crd != 5)
        w_p->ideal_cam_pos_h = w_p->chara_top_y - 187.5f;            /* 0x006C8928 */
    else
        w_p->ideal_cam_pos_h = w_p->chara_top_y - 437.5f;            /* 0x006C8924 */
}

/* ===================================================================== */
/* vc_main: watch target                                                  */
/* ===================================================================== */

/* vcSetWatchTgtXzPos  PC 0x0051aa10 */
static void vcSetWatchTgtXzPos(float *watch_pos, const float *center_pos, const float *cam_pos,
                               float tgt_chara2watch_cir_dist, float tgt_watch_cir_r, float watch_cir_ang_y)
{
    float cam2chr_ang = shAtan2XY(center_pos[2] - cam_pos[2], center_pos[0] - cam_pos[0]);
    float chr2watch_x = sinf(watch_cir_ang_y) * tgt_watch_cir_r + sinf(cam2chr_ang) * tgt_chara2watch_cir_dist;
    float chr2watch_z = cosf(watch_cir_ang_y) * tgt_watch_cir_r + cosf(cam2chr_ang) * tgt_chara2watch_cir_dist;
    watch_pos[0] = chr2watch_x + center_pos[0];
    watch_pos[2] = chr2watch_z + center_pos[2];
}

/* vcMakeNormalWatchTgtPos  PC 0x0051d9e0 (includes vcSetWatchTgtYParam) */
static void vcMakeNormalWatchTgtPos(float *watch_tgt_pos, float *watch_tgt_ang_z_p, _VC_WORK *w_p,
                                    int cam_mv_type, int cur_rd_area_size)
{
    _VC_ROAD_DATA *rd = w_p->cur_near_road.road_p;
    float ang[4], vec[4], cam2chara_dist, dx, dz;
    float tgt_watch_cir_r = 0.0f, min_cam2watch_dist, tgt_chara2watch_cir_dist, watch_y;

    *watch_tgt_ang_z_p = 0.0f;
    if (cam_mv_type == VC_MV_FIX_ANG) {
        ang[0] = rd->tmp.fix.ang_x;
        ang[1] = shNormRad(rd->tmp.fix.ang_y + w_p->fix_man.add_ang_y);
        ang[2] = 0.0f;
        ang[3] = 1.0f;
        vwAngleToVector(vec, ang, rd->tmp.fix.cam2wth_dist);
        watch_tgt_pos[0] = vec[0] + w_p->cam_pos[0];
        watch_tgt_pos[1] = vec[1] + w_p->cam_pos[1];
        watch_tgt_pos[3] = 1.0f;
        watch_tgt_pos[2] = vec[2] + w_p->cam_pos[2];
        return;
    }
    dx = w_p->chara_pos[0] - w_p->cam_pos[0];
    dz = w_p->chara_pos[2] - w_p->cam_pos[2];
    cam2chara_dist = sqrtf(dx * dx + dz * dz);
    switch (cam_mv_type) {
    case VC_MV_SETTLE:
        tgt_watch_cir_r = nml_tgt_watch_cir_r[cur_rd_area_size] * 0.5f;
        tgt_chara2watch_cir_dist = 0.0f;
        break;
    case VC_MV_CHASE:
    case VC_MV_LOCUS_CIRCLE:
    case VC_MV_THROUGH_DOOR:
        if (vcIsStage1Wide()) {
            tgt_watch_cir_r = nml_tgt_watch_cir_r[cur_rd_area_size] * 1.75f;          /* 0x006BF064 */
            min_cam2watch_dist = nml_cam2wth_min_dist[cur_rd_area_size] * 1.75f;
        } else if (vcIsOnBoat()) {
            min_cam2watch_dist = 5610.0f;                                             /* 0x006C8910 */
            tgt_watch_cir_r = 2550.0f;                                                /* 0x451F6000 */
        } else {
            tgt_watch_cir_r = nml_tgt_watch_cir_r[cur_rd_area_size];
            min_cam2watch_dist = nml_cam2wth_min_dist[cur_rd_area_size];
        }
        if (cam2chara_dist < min_cam2watch_dist + tgt_watch_cir_r)
            tgt_chara2watch_cir_dist = (min_cam2watch_dist + tgt_watch_cir_r) - cam2chara_dist;
        else
            tgt_chara2watch_cir_dist = 0.0f;
        break;
    default:
        vcAssert(0xa18);
        return;
    }
    watch_y = w_p->cur_near_road.road_p->ofs_watch_hy + w_p->chara_bottom_y;
    vcSetWatchTgtXzPos(watch_tgt_pos, w_p->chara_pos, w_p->cam_pos, tgt_chara2watch_cir_dist, tgt_watch_cir_r,
                       w_p->chara_eye_ang_y);
    /* vcSetWatchTgtYParam */
    if (cam_mv_type != VC_MV_SELF_VIEW) watch_tgt_pos[1] = watch_y;
    else                                watch_tgt_pos[1] = w_p->chara_center_y;
}

/* vcMixSelfViewEffectToWatchTgtPos  PC 0x0051a240 (head_mat = vcPreInfo.hero_neck_wm) */
static void vcMixSelfViewEffectToWatchTgtPos(float *watch_tgt_pos, float *watch_tgt_ang_z_p, float effect_rate,
                                             _VC_WORK *w_p, const float head_mat[4][4], int anim_status)
{
    float vec[4], cam_ang[4], eff_pos[4];
    float view_xyz_len, far_ang_x, chara2head_ofs_ang_y, add_ang_y, ofs, xz2;
    float james_rot_y = IN->james_rot_y;
    VcMat hm;
    int jouge_val;

    vec[0] = watch_tgt_pos[0] - w_p->cam_pos[0];
    vec[3] = 0.0f;
    vec[1] = watch_tgt_pos[1] - w_p->cam_pos[1];
    vec[2] = watch_tgt_pos[2] - w_p->cam_pos[2];
    xz2 = vec[0] * vec[0] + vec[2] * vec[2];
    view_xyz_len = sqrtf(vec[1] * vec[1] + xz2);
    far_ang_x = shAtan2XY(sqrtf(xz2), -vec[1]);
    if (far_ang_x < -1.22173047f) far_ang_x = -1.22173047f;          /* 0x006C88BC */
    else if (far_ang_x > 1.22173047f) far_ang_x = 1.22173047f;       /* 0x006C888C */
    (void)shNormRad(shVecAngY(vec) - james_rot_y);                   /* chara2far_ofs_ang_y: computed, unused (PC) */

    memcpy(hm, head_mat, sizeof hm);
    cam_ang[3] = 0.0f;
    vwMatrixToAngleYXZ(cam_ang, hm);
    chara2head_ofs_ang_y = shNormRad(cam_ang[1] - james_rot_y);

    /* roll */
    if (anim_status == 1) {
        if (w_p->nearest_enemy_p != NULL) cam_ang[2] = 0.0f;
    } else if (anim_status < 0xc || anim_status > 0xe) {
        cam_ang[2] = cam_ang[2] * 0.5f;
    }

    /* yaw: add_ang_y (kept in cam_ang[1]) */
    switch (anim_status) {
    case 1:
        if (w_p->nearest_enemy_p != NULL) add_ang_y = james_rot_y;
        else                              add_ang_y = cam_ang[1] + 0.52359879f;       /* 0x006A91C0 */
        break;
    case 3:
        ofs = fabsf(chara2head_ofs_ang_y);
        if (ofs <= 0.0698131695f) {                                  /* 0x006AD790 */
            add_ang_y = james_rot_y + chara2head_ofs_ang_y;
        } else {
            ofs = (ofs - 0.0698131695f) * 0.125f + 0.0698131695f;    /* 0x00631500 */
            if (chara2head_ofs_ang_y < 0.0f) ofs = ofs * -1.0f;
            add_ang_y = james_rot_y + ofs;
        }
        break;
    case 6: case 7: case 9: case 0xc: case 0xd: case 0xe:
        ofs = chara2head_ofs_ang_y * 0.125f;
        if (ofs < -0.174532935f) ofs = -0.174532935f;               /* 0x006C88B8 */
        else if (ofs > 0.174532935f) ofs = 0.174532935f;            /* 0x006B7818 */
        add_ang_y = james_rot_y + ofs;
        break;
    case 0x18:
        add_ang_y = cam_ang[1];
        break;
    default:
        add_ang_y = james_rot_y;
        break;
    }
    cam_ang[1] = add_ang_y;

    /* pitch */
    switch (anim_status) {
    case 1:
        if (w_p->nearest_enemy_p != NULL) ofs = -0.122173049f;      /* 0x006C88B0 */
        else                              ofs = cam_ang[0] - 0.139626339f;   /* 0x006C88B4 */
        break;
    case 3:
        ofs = cam_ang[0] - 0.139626339f;
        break;
    case 6: case 7:
        /* PC quirk: ftol(James rot.y) >> 7 & 0xF (always 0 for |rot.y| < 1, 0xF for rot.y <= -1) */
        {
            int iv = (int)james_rot_y;
            jouge_val = (iv < 0 ? ~((~iv) >> 7) : (iv >> 7)) & 0xf;
        }
        if (jouge_val == 0 || jouge_val == 5) ofs = (cam_ang[0] - 0.0174532924f) - 0.104719758f;  /* 0x006BC190, 0x006B7820 */
        else                                  ofs = cam_ang[0] - 0.104719758f;
        break;
    default:
        ofs = cam_ang[0];
        break;
    }
    switch (anim_status) {
    case 1: case 3: case 6: case 7: case 9: case 0xc: case 0xd: case 0xe:
        cam_ang[0] = far_ang_x * 0.5f + ofs;
        break;
    default:
        cam_ang[0] = far_ang_x * 0.699999988f;                       /* 0x0069F370 */
        break;
    }
    if (cam_ang[0] < -1.39626348f) cam_ang[0] = -1.39626348f;       /* 0x006C88AC */
    else if (cam_ang[0] > 1.39626348f) cam_ang[0] = 1.39626348f;    /* 0x006B61AC */

    *watch_tgt_ang_z_p = shNormRad(cam_ang[2] - *watch_tgt_ang_z_p) * effect_rate + *watch_tgt_ang_z_p;

    eff_pos[0] = cosf(cam_ang[0]) * (sinf(cam_ang[1]) * view_xyz_len) + w_p->cam_pos[0];
    eff_pos[2] = cosf(cam_ang[0]) * (cosf(cam_ang[1]) * view_xyz_len) + w_p->cam_pos[2];
    eff_pos[1] = w_p->cam_pos[1] - sinf(cam_ang[0]) * view_xyz_len;
    watch_tgt_pos[3] = 0.0f;
    watch_tgt_pos[0] = (eff_pos[0] - watch_tgt_pos[0]) * effect_rate + watch_tgt_pos[0];
    watch_tgt_pos[1] = (eff_pos[1] - watch_tgt_pos[1]) * effect_rate + watch_tgt_pos[1];
    watch_tgt_pos[2] = (eff_pos[2] - watch_tgt_pos[2]) * effect_rate + watch_tgt_pos[2];
}

/* vcMakeFarWatchTgtPos  PC 0x0051a670 */
static void vcMakeFarWatchTgtPos(float *watch_tgt_pos, _VC_WORK *w_p, int cur_rd_area_size)
{
    int glb = IN->stage_glb_crd;
    float use_dist, watch_y, actual_dist, adj_dist, lim_y;
    float cir_ang, dx, dz, cam2far_ang_y, cir_3d_r, real_ang_x, ofs_ang_x, tmpvec[4], search[4];
    SH2CamChara *sc_p;

    if (vcIsStage1Wide()) {
        use_dist = far_tgt_watch_cir_r[cur_rd_area_size] * 1.75f;
    } else if (glb == 9 || glb == 0xc || glb == 0xd || glb == 0x10) {
        use_dist = vcRetCirRadiusReduction(w_p);
    } else if (vcIsOnBoat()) {
        use_dist = 10200.0f;                                         /* 0x461F6000 */
    } else {
        use_dist = far_tgt_watch_cir_r[cur_rd_area_size];
    }

    sc_p = w_p->nearest_enemy_p;
    if (sc_p == NULL && w_p->nearest_item_p == NULL) {
        watch_y = w_p->chara_pos[1] - 450.0f;                        /* 0x006C88C8 */
    } else {
        if (sc_p != NULL) actual_dist = w_p->nearest_enemy_xz_dist;
        else { sc_p = w_p->nearest_item_p; actual_dist = w_p->nearest_item_xz_dist; }
        adj_dist = (actual_dist < 850.0f) ? -0.411764711f * actual_dist : -350.0f;   /* 0x006C88C4, 0x006C88C0, 0x006B3484 */
        if (adj_dist + actual_dist < use_dist) use_dist = adj_dist + actual_dist;
        if (use_dist < 200.0f) use_dist = 200.0f;                    /* 0x006A7920 */
        watch_y = sc_p->eye_y;
        lim_y = (actual_dist * 0.5f + w_p->chara_pos[1]) - 250.0f;
        if (watch_y > lim_y) watch_y = lim_y;
    }

    watch_tgt_pos[1] = watch_y;
    watch_tgt_pos[0] = sinf(w_p->chara_eye_ang_y) * use_dist + w_p->chara_pos[0];
    watch_tgt_pos[3] = 1.0f;
    watch_tgt_pos[2] = cosf(w_p->chara_eye_ang_y) * use_dist + w_p->chara_pos[2];

    cir_ang = shNormRad(IN->hero_head_ang_y + w_p->chara_eye_ang_y);
    dx = w_p->chara_pos[0] - w_p->cam_pos[0];
    dz = w_p->chara_pos[2] - w_p->cam_pos[2];
    (void)sqrtf(dx * dx + dz * dz);                                  /* chr2cam_dist: computed, unused (PC) */
    watch_tgt_pos[0] = sinf(cir_ang) * use_dist + w_p->chara_pos[0];
    watch_tgt_pos[2] = cosf(cir_ang) * use_dist + w_p->chara_pos[2];

    tmpvec[0] = w_p->cam_pos[0] - watch_tgt_pos[0];
    tmpvec[1] = w_p->cam_pos[1] - watch_tgt_pos[1];
    tmpvec[2] = w_p->cam_pos[2] - watch_tgt_pos[2];
    tmpvec[3] = w_p->cam_pos[3] - watch_tgt_pos[3];
    cam2far_ang_y = shVecAngY(tmpvec);
    cir_3d_r = sqrtf(use_dist * use_dist + (watch_y - IN->hero_eye_y) * (watch_y - IN->hero_eye_y));

    cir_ang = cam2far_ang_y + 3.14159274f;
    watch_tgt_pos[0] = sinf(cir_ang) * use_dist + w_p->chara_pos[0];
    watch_tgt_pos[1] = watch_y;
    watch_tgt_pos[3] = 1.0f;
    watch_tgt_pos[2] = cosf(cir_ang) * use_dist + w_p->chara_pos[2];

    search[0] = w_p->cam_pos[1] - watch_y;
    dx = w_p->cam_pos[0] - watch_tgt_pos[0];
    dz = w_p->cam_pos[2] - watch_tgt_pos[2];
    search[2] = sqrtf(dz * dz + dx * dx);
    real_ang_x = shVecAngY(search);                                  /* atan2(cam.y - y, xz) */
    ofs_ang_x = -(sinf(IN->hero_head_ang_x) * cir_3d_r);
    watch_tgt_pos[1] = ofs_ang_x + watch_tgt_pos[1];
    search[0] = w_p->cam_pos[1] - watch_tgt_pos[1];
    dx = w_p->cam_pos[0] - watch_tgt_pos[0];
    dz = w_p->cam_pos[2] - watch_tgt_pos[2];
    search[2] = sqrtf(dz * dz + dx * dx);
    ofs_ang_x = shVecAngY(search);
    watch_tgt_pos[1] = watch_y;
    tmpvec[0] = w_p->cam_pos[0] - watch_tgt_pos[0];
    tmpvec[1] = w_p->cam_pos[1] - watch_y;
    tmpvec[2] = w_p->cam_pos[2] - watch_tgt_pos[2];
    watch_tgt_pos[3] = 1.0f;
    watch_tgt_pos[1] = -(sqrtf(tmpvec[1] * tmpvec[1] + tmpvec[0] * tmpvec[0] + tmpvec[2] * tmpvec[2]) *
                         sinf(ofs_ang_x - real_ang_x)) + watch_y;
}

/* vcAdjustWatchYLimitHighWhenFarView  PC 0x0051aab0 */
static void vcAdjustWatchYLimitHighWhenFarView(float *watch_pos, const float *cam_pos)
{
    float max_cam_ang_x, dist, cam_ang_x, dx, dz;
    max_cam_ang_x = shAtan2XY(6500.0f, -(-2500.0f - cam_pos[1]))     /* 0x45CB2000, 0x006C88CC */
                  - shAtan2XY(g_vc.scr_z, g_vc.scr_sy * 0.5f);
    dx = watch_pos[0] - cam_pos[0];
    dz = watch_pos[2] - cam_pos[2];
    dist = sqrtf(dx * dx + dz * dz);
    cam_ang_x = shAtan2XY(dist, -(watch_pos[1] - cam_pos[1]));
    if (cam_ang_x > max_cam_ang_x)
        watch_pos[1] = cam_pos[1] - (sinf(max_cam_ang_x) * dist) / cosf(max_cam_ang_x);
    if (IN->stage_glb_crd == 9 && cam_ang_x > -0.392699093f)         /* 0x0069DB5C */
        watch_pos[1] = cam_pos[1] - (sinf(-0.392699093f) * dist) / cosf(-0.392699093f);
}

/* vcAutoRenewalWatchTgtPosAndAngZ  PC 0x0051ed10 */
static void vcAutoRenewalWatchTgtPosAndAngZ(_VC_WORK *w_p, int cam_mv_type, int cur_rd_area_size,
                                            float far_watch_rate, float self_view_eff_rate)
{
    float far_watch_pos[4], fx, fy, fz;
    vcMakeFarWatchTgtPos(far_watch_pos, w_p, cur_rd_area_size);
    switch (cam_mv_type) {
    case VC_MV_CHASE: case VC_MV_SETTLE: case VC_MV_FIX_ANG: case VC_MV_LOCUS_CIRCLE: case VC_MV_THROUGH_DOOR:
        vcMakeNormalWatchTgtPos(w_p->watch_tgt_pos, &w_p->watch_tgt_ang_z, w_p, cam_mv_type, cur_rd_area_size);
        if (far_watch_rate != 0.0f) {
            if (IN->control_type == 1 &&
                (w_p->cur_near_road.road_p->cam_mv_type == VC_MV_CHASE ||
                 w_p->cur_near_road.road_p->cam_mv_type == VC_MV_LOCUS_CIRCLE)) {
                v4copy(w_p->watch_tgt_pos, w_p->chara_pos);
                w_p->watch_tgt_pos[1] = w_p->chara_top_y + 225.0f;   /* 0x006B61B8 */
            }
            fx = (far_watch_pos[0] - w_p->watch_tgt_pos[0]) * far_watch_rate;
            fy = (far_watch_pos[1] - w_p->watch_tgt_pos[1]) * far_watch_rate;
            fz = (far_watch_pos[2] - w_p->watch_tgt_pos[2]) * far_watch_rate;
            w_p->watch_tgt_pos[0] = fx + w_p->watch_tgt_pos[0];
            w_p->watch_tgt_pos[1] = fy + w_p->watch_tgt_pos[1];
            w_p->watch_tgt_pos[3] = 0.0f;
            w_p->watch_tgt_pos[2] = fz + w_p->watch_tgt_pos[2];
        }
        break;
    case VC_MV_SELF_VIEW:
        v4copy(w_p->watch_tgt_pos, far_watch_pos);
        break;
    default:
        vcAssert(0x98d);
        break;
    }
    vcMixSelfViewEffectToWatchTgtPos(w_p->watch_tgt_pos, &w_p->watch_tgt_ang_z, self_view_eff_rate, w_p,
                                     (const float (*)[4])IN->hero_neck_wm, IN->james_anim_status);
    if (w_p->watch_tgt_pos[1] > 50000.0f) w_p->watch_tgt_pos[1] = 50000.0f;   /* 0x00638838 */
}

/* ===================================================================== */
/* vc_main: camera target                                                 */
/* ===================================================================== */

/* vcRetMaxTgtMvXzLen  PC 0x0051abc0 */
static float vcRetMaxTgtMvXzLen(_VC_WORK *w_p, const _VC_CAM_MV_PARAM *cam_mv_prm_p)
{
    float max_spd_xz = fabsf(w_p->chara_ang_spd_y * 8.0f) + w_p->chara_mv_spd + 500.0f;   /* 0x0069F3BC, 0x0062D230 */
    float max_spd_xz_stg;
    if (!vcIsOutdoorStage() && IN->stage_glb_crd != 5) max_spd_xz_stg = 1200.0f;          /* 0x006C88D8 */
    else if (!vcIsOnBoat())                            max_spd_xz_stg = 1980.0f;          /* 0x006C88D4 */
    else                                               max_spd_xz_stg = 2244.00024f;      /* 0x006C88D0 */
    if (max_spd_xz < max_spd_xz_stg) return vcFrameTime() * max_spd_xz_stg;
    if (max_spd_xz > cam_mv_prm_p->max_spd_xz) max_spd_xz = cam_mv_prm_p->max_spd_xz;
    return vcFrameTime() * max_spd_xz;
}

/* vcMakeIdealCamPosByHeadPos  PC 0x0051ac80 */
static void vcMakeIdealCamPosByHeadPos(float *ideal_pos, _VC_WORK *w_p)
{
    float chara2cam_ang_y;
    if (w_p->flags & VC_WARP_WATCH_F) {
        ideal_pos[0] = w_p->chara_pos[0];
        ideal_pos[1] = w_p->chara_pos[1];
        ideal_pos[2] = w_p->chara_pos[2];
        ideal_pos[1] = w_p->chara_top_y;
        return;
    }
    chara2cam_ang_y = shNormRad(w_p->chara_eye_ang_y + 2.96705985f); /* 0x006C88DC */
    ideal_pos[1] = w_p->chara_head_pos[1] + 50.0f;                   /* 0x006A6DAC */
    ideal_pos[0] = sinf(chara2cam_ang_y) * 90.0f + w_p->chara_head_pos[0];   /* 0x0069F354 */
    ideal_pos[2] = cosf(chara2cam_ang_y) * 90.0f + w_p->chara_head_pos[2];
}

/* vcMakeIdealCamPosForThroughDoorCam  PC 0x0051ad10 */
static void vcMakeIdealCamPosForThroughDoorCam(float *ideal_pos, _VC_WORK *w_p)
{
    _VC_THROUGH_DOOR_CAM_PARAM *prm_p = &w_p->through_door;
    float abs_ofs_ang_y, mv_forwd_dist, mv_right_dist;
    if (!prm_p->active_f) return;
    if (w_p->through_door_activate_init_f == 0 && (prm_p->timer != 0.0f || vcTransitionIdle())) {
        abs_ofs_ang_y = fabsf(shNormRad(w_p->chara_eye_ang_y - prm_p->rail_ang_y)) - 0.785398185f;  /* 0x0069DB50 */
        if (abs_ofs_ang_y < 0.0f) abs_ofs_ang_y = 0.0f;
        mv_forwd_dist = cosf(abs_ofs_ang_y * 1.33333337f) * -475.0f + 375.0f;   /* 0x006A2B0C, 0x006C88E8, 0x006C88E4 */
        mv_right_dist = sinf(w_p->chara_eye_ang_y - prm_p->rail_ang_y) * -425.0f; /* 0x006C88E0 */
    } else {
        mv_forwd_dist = -300.0f;
        mv_right_dist = 0.0f;
    }
    ideal_pos[0] = sinf(prm_p->rail_ang_y) * mv_forwd_dist + cosf(prm_p->rail_ang_y) * mv_right_dist + prm_p->rail_sta_pos[0];
    ideal_pos[2] = cosf(prm_p->rail_ang_y) * mv_forwd_dist + (prm_p->rail_sta_pos[2] - sinf(prm_p->rail_ang_y) * mv_right_dist);
    ideal_pos[1] = prm_p->rail_sta_pos[1];
}

/* vcMakeIdealCamPosForLocusCircleCam  PC 0x0051ae30 */
static void vcMakeIdealCamPosForLocusCircleCam(float *ideal_pos, _VC_WORK *w_p)
{
    _VC_ROAD_DATA *rd = w_p->cur_near_road.road_p;
    float cir_ang_y = shNormRad(rd->tmp.cir.ang_y);
    float cir_radius = rd->tmp.cir.radius;
    float origin_x, origin_z, swl_x, swl_z, a2, s, c;
    _VC_CIR_CAM_MANAGER *cm = &w_p->cir_man;

    if (w_p->flags & VC_SWITCH_NEAR_RD_DATA_F) {
        memset(cm->sw_l, 0, sizeof cm->sw_l);
        cm->origin[0] = 0.0f; cm->origin[1] = 0.0f; cm->origin[2] = 0.0f; cm->origin[3] = 1.0f;
        cm->sw_l[0] = fabsf(w_p->cur_near_road.sw.max_hx - w_p->cur_near_road.sw.min_hx);
        cm->sw_l[2] = fabsf(w_p->cur_near_road.sw.max_hz - w_p->cur_near_road.sw.min_hz);
        if (-3.14159274f <= cir_ang_y && cir_ang_y < -1.57079637f) {
            cm->origin[0] = (rd->lim_sw.x2 < rd->lim_sw.x0) ? rd->lim_sw.x0 : rd->lim_sw.x2;   /* max x */
            cm->origin[2] = (rd->lim_sw.z2 < rd->lim_sw.z0) ? rd->lim_sw.z0 : rd->lim_sw.z2;   /* max z */
        } else if (-1.57079637f <= cir_ang_y && cir_ang_y < 0.0f) {
            cm->origin[0] = (rd->lim_sw.x0 <= rd->lim_sw.x2) ? rd->lim_sw.x2 : rd->lim_sw.x0;  /* max x */
            cm->origin[2] = (rd->lim_sw.z0 >= rd->lim_sw.z2) ? rd->lim_sw.z2 : rd->lim_sw.z0;  /* min z */
        } else if (0.0f <= cir_ang_y && cir_ang_y < 1.57079637f) {
            cm->origin[0] = (rd->lim_sw.x0 >= rd->lim_sw.x2) ? rd->lim_sw.x2 : rd->lim_sw.x0;  /* min x */
            cm->origin[2] = (rd->lim_sw.z0 >= rd->lim_sw.z2) ? rd->lim_sw.z2 : rd->lim_sw.z0;  /* min z */
        } else {
            cm->origin[0] = (rd->lim_sw.x0 >= rd->lim_sw.x2) ? rd->lim_sw.x2 : rd->lim_sw.x0;  /* min x */
            cm->origin[2] = (rd->lim_sw.z0 <= rd->lim_sw.z2) ? rd->lim_sw.z2 : rd->lim_sw.z0;  /* max z */
        }
    }
    if (rd->tmp.cir.origin_x != 0.0f && rd->tmp.cir.origin_z != 0.0f) {
        origin_x = rd->tmp.cir.origin_x;
        origin_z = rd->tmp.cir.origin_z;
    } else {
        origin_x = cm->origin[0];
        origin_z = cm->origin[2];
    }
    swl_x = cm->sw_l[0];
    swl_z = cm->sw_l[2];
    a2 = shAtan2XY(w_p->chara_pos[2] - origin_z, w_p->chara_pos[0] - origin_x) - cir_ang_y;
    a2 = a2 + a2;
    s = sinf(a2);
    c = cosf(a2);

    if (-3.14159274f <= cir_ang_y && cir_ang_y < -1.57079637f) {
        if (a2 > 0.0f)       { ideal_pos[0] = (origin_x - swl_x) - s * cir_radius; ideal_pos[2] = origin_z - c * swl_z; }
        else if (a2 < 0.0f)  { ideal_pos[0] = origin_x - c * swl_x; ideal_pos[2] = (origin_z - swl_z) - -(s * cir_radius); }
        else                 { ideal_pos[0] = origin_x - swl_x;     ideal_pos[2] = origin_z - swl_z; }
    } else if (-1.57079637f <= cir_ang_y && cir_ang_y < 0.0f) {
        if (a2 > 0.0f)       { ideal_pos[0] = origin_x - c * swl_x; ideal_pos[2] = s * cir_radius + swl_z + origin_z; }
        else if (a2 < 0.0f)  { ideal_pos[0] = (origin_x - swl_x) - -(s * cir_radius); ideal_pos[2] = c * swl_z + origin_z; }
        else                 { ideal_pos[0] = origin_x - swl_x;     ideal_pos[2] = swl_z + origin_z; }
    } else if (0.0f < cir_ang_y && cir_ang_y < 1.57079637f) {
        if (a2 > 0.0f)       { ideal_pos[0] = s * cir_radius + swl_x + origin_x; ideal_pos[2] = c * swl_z + origin_z; }
        else if (a2 < 0.0f)  { ideal_pos[0] = c * swl_x + origin_x; ideal_pos[2] = (swl_z - s * cir_radius) + origin_z; }
        else                 { ideal_pos[0] = swl_x + origin_x;     ideal_pos[2] = swl_z + origin_z; }
    } else {   /* cir_ang_y == 0, >= PI/2, or < -PI */
        if (a2 > 0.0f)       { ideal_pos[0] = c * swl_x + origin_x; ideal_pos[2] = (origin_z - swl_z) - s * cir_radius; }
        else if (a2 < 0.0f)  { ideal_pos[0] = (swl_x - s * cir_radius) + origin_x; ideal_pos[2] = origin_z - c * swl_z; }
        else                 { ideal_pos[0] = swl_x + origin_x;     ideal_pos[2] = origin_z - swl_z; }
    }
    ideal_pos[1] = w_p->ideal_cam_pos_h;
}

/* vcAdjustXzInLimAreaUsingMIN_IN_ROAD_DIST  PC 0x0051b3b0 */
static void vcAdjustXzInLimAreaUsingMIN_IN_ROAD_DIST(float *x_p, float *z_p, const _VC_LIMIT_AREA *lim_p)
{
    float x = *x_p, z = *z_p;
    float min_in_road_dist = vcGetMinInRoadDist();
    float min_x = min_in_road_dist + lim_p->min_hx;
    float max_x = lim_p->max_hx - min_in_road_dist;
    float min_z = min_in_road_dist + lim_p->min_hz;
    float max_z = lim_p->max_hz - min_in_road_dist;
    if (max_x < min_x) { max_x = (max_x + min_x) * 0.5f; min_x = max_x; }
    if (max_z < min_z) { max_z = (max_z + min_z) * 0.5f; min_z = max_z; }
    if (x < min_x) x = min_x;
    else if (x > max_x) x = max_x;
    if (z < min_z) z = min_z;
    else if (z > max_z) z = max_z;
    *x_p = x;
    *z_p = z;
}

/* vcMakeIdealCamPosForFixAngCam  PC 0x0051dc10 */
static void vcMakeIdealCamPosForFixAngCam(float *ideal_pos, _VC_WORK *w_p)
{
    _VC_ROAD_DATA *rd;
    _VC_LIMIT_AREA lim_rd;
    float ang_y, cppos[4], abs_ofs_x, abs_ofs_z, out_x, out_z, base_front_r;
    float rate, full_front_dist, zero_front_dist, chara_front_r, chara2cam_dist, dx, dz, cam_ang;

    if (w_p->flags & VC_SWITCH_NEAR_RD_DATA_F) {
        w_p->fix_man.exception_f = 0;
        w_p->fix_man.add_ang_y = 0.0f;
        w_p->fix_man.add_rd_x = 0.0f;
        w_p->fix_man.add_rd_z = 0.0f;
    }
    rd = w_p->cur_near_road.road_p;
    ang_y = shNormRad(rd->tmp.fix.ang_y + w_p->fix_man.add_ang_y);
    lim_rd = w_p->cur_near_road.rd;
    lim_rd.min_hx = w_p->cur_near_road.rd.min_hx + w_p->fix_man.add_rd_x;
    lim_rd.max_hx = w_p->cur_near_road.rd.max_hx + w_p->fix_man.add_rd_x;
    lim_rd.min_hz = w_p->cur_near_road.rd.min_hz + w_p->fix_man.add_rd_z;
    lim_rd.max_hz = w_p->cur_near_road.rd.max_hz + w_p->fix_man.add_rd_z;
    vcTransRotRoadArea(cppos, w_p->cur_near_road.rd_rzm, w_p->chara_pos);
    (void)vcGetXZSumDistFromLimArea(&out_x, &out_z, cppos[0], cppos[2],
                                    w_p->cur_near_road.rd.min_hx, w_p->cur_near_road.rd.max_hx,
                                    w_p->cur_near_road.rd.min_hz, w_p->cur_near_road.rd.max_hz, 0);
    abs_ofs_x = fabsf(out_x);
    abs_ofs_z = fabsf(out_z);
    base_front_r = ((abs_ofs_x <= abs_ofs_z) ? abs_ofs_z : abs_ofs_x) * 0.5f + 1000.0f;   /* 0x006388A8 */

    rate = rd->tmp.fix.cam2wth_dist * 0.000500000024f;               /* 0x006C8914 */
    chara_front_r   = 350.0f * rate;                                 /* 0x006B61C8 */
    zero_front_dist = 3500.0f * rate;                                /* 0x006BB33C */
    full_front_dist = rate * 750.0f;                                 /* 0x006A8458 */
    dx = w_p->chara_pos[0] - w_p->cam_pos[0];
    dz = w_p->chara_pos[2] - w_p->cam_pos[2];
    chara2cam_dist = sqrtf(dx * dx + dz * dz);
    if (chara2cam_dist < zero_front_dist) {
        if (chara2cam_dist > full_front_dist)
            chara_front_r = ((chara2cam_dist - zero_front_dist) / (full_front_dist - zero_front_dist)) * chara_front_r;
    } else {
        chara_front_r = 0.0f;
    }

    cam_ang = ang_y + 3.14159274f;
    ideal_pos[0] = sinf(cam_ang) * base_front_r + sinf(w_p->chara_eye_ang_y) * chara_front_r + w_p->chara_pos[0];
    ideal_pos[2] = cosf(cam_ang) * base_front_r + cosf(w_p->chara_eye_ang_y) * chara_front_r + w_p->chara_pos[2];
    ideal_pos[1] = w_p->chara_pos[1];
    /* PC quirk/side effect: shifts this frame's chara_top_y by the road's fix.ofs_hy */
    w_p->chara_top_y = rd->tmp.fix.ofs_hy + w_p->chara_top_y;
    vcTransRotRoadArea(ideal_pos, w_p->cur_near_road.rd_rzm, ideal_pos);
    vcAdjustXzInLimAreaUsingMIN_IN_ROAD_DIST(&ideal_pos[0], &ideal_pos[2], &lim_rd);
    vcRotTransRoadArea(ideal_pos, w_p->cur_near_road.rd_rim, ideal_pos);
}

/* vcMakeIdealCamPosUseVC_ROAD_DATA  PC 0x0051de90 (CHASE and SETTLE) */
static void vcMakeIdealCamPosUseVC_ROAD_DATA(float *ideal_pos, _VC_WORK *w_p, int cur_rd_area_size)
{
    _VC_ROAD_DATA *rd;
    float behind_ang_y, ofs_ang_y, chara2ideal_r, abs_chr2cam_y, real_chr2cam_r;
    float no_adj_max_dist, full_adj_min_dist, cppos[4], adj_x, adj_z;

    behind_ang_y = w_p->chara_eye_ang_y + 3.14159274f;
    ofs_ang_y = shNormRad(w_p->cam_chara2ideal_ang_y - behind_ang_y);
    if (fabsf(w_p->chara_ang_spd_y) <= 0.34906587f) {               /* 0x006C8888 */
        ofs_ang_y = (ofs_ang_y < 0.0f) ? -0.209439516f : 0.209439516f;   /* 0x006C8920, 0x006B8E98 */
    } else {
        if (ofs_ang_y < -0.209439516f) ofs_ang_y = -0.209439516f;
        else if (ofs_ang_y > 0.209439516f) ofs_ang_y = 0.209439516f;
    }
    if (IN->control_type == 1 || vcIsOnBoat())
        w_p->cam_chara2ideal_ang_y = shNormRad(behind_ang_y);
    else
        w_p->cam_chara2ideal_ang_y = shNormRad(ofs_ang_y + behind_ang_y);

    if (vcIsStage1Wide())  chara2ideal_r = mv_nml_chr2cam_r[cur_rd_area_size] * 1.75f;
    else if (vcIsOnBoat()) chara2ideal_r = 2550.0f;                  /* 0x451F6000 */
    else                   chara2ideal_r = mv_nml_chr2cam_r[cur_rd_area_size];

    ideal_pos[0] = sinf(w_p->cam_chara2ideal_ang_y) * chara2ideal_r + w_p->chara_pos[0];
    ideal_pos[2] = cosf(w_p->cam_chara2ideal_ang_y) * chara2ideal_r + w_p->chara_pos[2];
    rd = w_p->cur_near_road.road_p;
    if (rd->cam_mv_type == VC_MV_CHASE)       ideal_pos[1] = rd->tmp.chs.ofs_hy + w_p->ideal_cam_pos_h;
    else if (rd->cam_mv_type == VC_MV_SETTLE) ideal_pos[1] = w_p->ideal_cam_pos_h;
    ideal_pos[3] = 1.0f;

    abs_chr2cam_y = fabsf(w_p->cam_pos[1] - w_p->chara_pos[1]) - 750.0f;   /* 0x006A8458 */
    if (abs_chr2cam_y <= 0.0f) abs_chr2cam_y = 0.0f;

    rd = w_p->cur_near_road.road_p;
    if (ideal_pos[1] < rd->lim_rd.max_hy)      ideal_pos[1] = rd->lim_rd.max_hy;
    else if (ideal_pos[1] > rd->lim_rd.min_hy) ideal_pos[1] = rd->lim_rd.min_hy;

    /* how far is the character outside the road area */
    vcTransRotRoadArea(cppos, w_p->cur_near_road.rd_rzm, w_p->chara_pos);
    adj_x = cppos[0];
    adj_z = cppos[2];
    vcAdjustXzInLimAreaUsingMIN_IN_ROAD_DIST(&adj_x, &adj_z, &w_p->cur_near_road.rd);
    cppos[0] = adj_x;
    cppos[1] = 0.0f;
    cppos[2] = adj_z;
    cppos[3] = 1.0f;
    vcRotTransRoadArea(cppos, w_p->cur_near_road.rd_rim, cppos);
    real_chr2cam_r = vwRet3DLength(cppos[0] - w_p->chara_pos[0], abs_chr2cam_y, cppos[2] - w_p->chara_pos[2]);

    if (vcIsStage1Wide()) {
        no_adj_max_dist   = mv_nml_no_adj_max_dist[cur_rd_area_size] * 1.75f;
        full_adj_min_dist = mv_nml_full_adj_min_dist[cur_rd_area_size] * 1.75f;
    } else if (vcIsOnBoat()) {
        no_adj_max_dist   = 2040.00012f;                             /* 0x006C891C */
        full_adj_min_dist = 5100.0f;                                 /* 0x006C8918 */
    } else {
        no_adj_max_dist   = mv_nml_no_adj_max_dist[cur_rd_area_size];
        full_adj_min_dist = mv_nml_full_adj_min_dist[cur_rd_area_size];
    }
    if (real_chr2cam_r > full_adj_min_dist)
        chara2ideal_r = 200.0f;                                      /* 0x43480000 */
    else if (real_chr2cam_r > no_adj_max_dist)
        chara2ideal_r = ((200.0f - chara2ideal_r) * (real_chr2cam_r - no_adj_max_dist)) /
                        (full_adj_min_dist - no_adj_max_dist) + chara2ideal_r;

    ideal_pos[0] = sinf(w_p->cam_chara2ideal_ang_y) * chara2ideal_r + w_p->chara_pos[0];
    ideal_pos[2] = cosf(w_p->cam_chara2ideal_ang_y) * chara2ideal_r + w_p->chara_pos[2];
    vcTransRotRoadArea(cppos, w_p->cur_near_road.rd_rzm, ideal_pos);
    vcAdjustXzInLimAreaUsingMIN_IN_ROAD_DIST(&cppos[0], &cppos[2], &w_p->cur_near_road.rd);
    vcRotTransRoadArea(cppos, w_p->cur_near_road.rd_rim, cppos);
    ideal_pos[0] = cppos[0];
    ideal_pos[2] = cppos[2];
}

/* vcMakeBasicCamTgtMvVec  PC 0x0051b4b0 */
static void vcMakeBasicCamTgtMvVec(float *tgt_mv_vec, const float *ideal_pos, _VC_WORK *w_p, float max_tgt_mv_xz_len)
{
    float xz_vec[4], sincos_y[4], now2ideal_tgt_dist, now2ideal_tgt_ang_y;
    xz_vec[3] = 0.0f;
    xz_vec[0] = ideal_pos[0] - w_p->cam_tgt_pos[0];
    xz_vec[1] = ideal_pos[1] - w_p->cam_tgt_pos[1];
    xz_vec[2] = ideal_pos[2] - w_p->cam_tgt_pos[2];
    now2ideal_tgt_dist = sqrtf(xz_vec[2] * xz_vec[2] + xz_vec[0] * xz_vec[0]);
    now2ideal_tgt_ang_y = shVecAngY(xz_vec);
    if (max_tgt_mv_xz_len <= now2ideal_tgt_dist) {
        shSinCosVec(sincos_y, now2ideal_tgt_ang_y);
        tgt_mv_vec[3] = 0.0f;
        tgt_mv_vec[0] = sincos_y[0] * max_tgt_mv_xz_len;
        tgt_mv_vec[1] = sincos_y[1] * max_tgt_mv_xz_len;
        tgt_mv_vec[2] = sincos_y[2] * max_tgt_mv_xz_len;
    } else {
        tgt_mv_vec[0] = xz_vec[0];
        tgt_mv_vec[1] = xz_vec[1];
        tgt_mv_vec[2] = xz_vec[2];
    }
    if (vcFrameTime() == 0.0f && !(w_p->flags & VC_WARP_CAM_TGT_F)) {
        tgt_mv_vec[1] = 0.0f;
        return;
    }
    tgt_mv_vec[1] = ideal_pos[1] - w_p->cam_tgt_pos[1];
}

/* vcAdjTgtMvVecYByCurNearRoad  PC 0x0051b5a0 */
static void vcAdjTgtMvVecYByCurNearRoad(float *tgt_mv_vec, _VC_WORK *w_p)
{
    _VC_ROAD_DATA *cur_rd_p = w_p->cur_near_road.road_p;
    float dx = w_p->chara_pos[0] - w_p->cam_tgt_pos[0];
    float dz = w_p->chara_pos[2] - w_p->cam_tgt_pos[2];
    float to_chara_dist = sqrtf(dz * dz + dx * dx);
    float near_ratio, min_dist, max_dist, dist, max_tgt_y, min_tgt_y, ofs_y, tgt_y, base_ofs_y, cam_h;

    if (cur_rd_p->mv_y_type == 4 || cur_rd_p->mv_y_type == 5) {
        if (cur_rd_p->cam_mv_type != VC_MV_LOCUS_CIRCLE) {
            near_ratio = vcRetNearRatioSwitchAreaInXZPos(w_p->cur_near_road, w_p->chara_pos, w_p->cam_tgt_pos);
        } else {
            near_ratio = vcRetNearRatioSwitchAreaForCircleCam(w_p->cur_near_road, w_p->cir_man, w_p->chara_pos);
            if (cur_rd_p->tmp.cir.ang_y < -3.14159274f || !(cur_rd_p->tmp.cir.ang_y < 3.14159274f))
                near_ratio = 1.0f - near_ratio;
        }
    } else {
        if (IN->stage_glb_crd == 2 || IN->stage_glb_crd == 5) { max_dist = 15500.0f; min_dist = 500.0f; }
        else                                                  { max_dist = 3500.0f;  min_dist = 600.0f; }
        dist = to_chara_dist;
        if (dist < min_dist) dist = min_dist;
        else if (dist > max_dist) dist = max_dist;
        near_ratio = (1.0f / (max_dist - min_dist)) * (max_dist - dist);
    }
    if (near_ratio < 0.0f) near_ratio = 0.0f;
    else if (near_ratio > 1.0f) near_ratio = 1.0f;

    switch (cur_rd_p->mv_y_type) {
    case 0:
        ofs_y = (to_chara_dist - 150.0f) * 0.25f;                    /* 0x006B564C, 0x0062BF7C */
        if (ofs_y < 0.0f) ofs_y = 0.0f;
        if (vcIsStage1Wide()) {
            max_tgt_y = (ofs_y + w_p->chara_top_y) - 750.0f;
            min_tgt_y = (w_p->chara_top_y - ofs_y) - 750.0f;
        } else {
            if (w_p->flags & VC_PC_SPECIAL_WATCH_F) max_tgt_y = g_vc.pc_special_watch_y;
            else max_tgt_y = (ofs_y + w_p->chara_top_y) - 250.0f;
            min_tgt_y = (w_p->chara_top_y - ofs_y) - 250.0f;
        }
        break;
    case 1:
        max_tgt_y = (1.0f - near_ratio) * cur_rd_p->lim_rd.min_hy + near_ratio * cur_rd_p->lim_rd.max_hy;
        min_tgt_y = max_tgt_y;
        break;
    case 2:
        max_tgt_y = (1.0f - near_ratio) * cur_rd_p->lim_rd.max_hy + near_ratio * cur_rd_p->lim_rd.min_hy;
        min_tgt_y = max_tgt_y;
        break;
    case 3:
        max_tgt_y = cur_rd_p->lim_rd.min_hy;
        min_tgt_y = cur_rd_p->lim_rd.max_hy;
        break;
    case 4:
    case 5:
        if (!vcIsOutdoorStage() && IN->stage_glb_crd != 5)
            base_ofs_y = (IN->area_code < 0x35) ? 0.0f : 125.0f;     /* 0x006BCD6C */
        else
            base_ofs_y = (IN->area_code < 0x35) ? -250.0f : -125.0f; /* 0x006C88F0, 0x006C88EC */
        cam_h = w_p->ideal_cam_pos_h;
        if (cur_rd_p->cam_mv_type == VC_MV_CHASE) {
            base_ofs_y = base_ofs_y + cur_rd_p->tmp.chs.ofs_hy;
            cam_h = cam_h + cur_rd_p->tmp.chs.ofs_hy;
        }
        max_tgt_y = cam_h * near_ratio + (1.0f - near_ratio) *
            (base_ofs_y + (w_p->chara_pos[1] + (cur_rd_p->mv_y_type == 4 ? cur_rd_p->lim_rd.min_hy
                                                                           : cur_rd_p->lim_rd.max_hy)));
        min_tgt_y = max_tgt_y;
        break;
    default:
        vcAssert(0x100d);
        return;
    }
    tgt_y = tgt_mv_vec[1] + w_p->cam_tgt_pos[1];
    if (tgt_y < min_tgt_y) tgt_y = min_tgt_y;
    else if (tgt_y > max_tgt_y) tgt_y = max_tgt_y;
    tgt_mv_vec[1] = tgt_y - w_p->cam_tgt_pos[1];
}

/* vcFlipFromCamExclusionArea  PC 0x0051b9f0 */
static float vcFlipFromCamExclusionArea(float *flip_ang_y_p, float *old_cam_excl_area_r_p, const float *in_pos,
                                        const float *chara_pos, float chara_eye_ang_y, int cur_rd_area_size)
{
    float cntr2pos_ang_y, ofs_ang_y, rate, cam_excl_area_r, min_add_dist, add_dist, dist, dx, dz;
    int r;
    cntr2pos_ang_y = shAtan2XY(in_pos[2] - chara_pos[2], in_pos[0] - chara_pos[0]);
    ofs_ang_y = shNormRad(cntr2pos_ang_y - chara_eye_ang_y);
    if (ofs_ang_y < 0.0f) ofs_ang_y = ofs_ang_y * -1.0f;
    if (vcIsStage1Wide()) rate = excl_max_rate[cur_rd_area_size] * 1.25f;   /* 0x006BCDC4 */
    else                  rate = excl_max_rate[cur_rd_area_size];
    r = vwOresenHokan(excl_r_ary, 9, (int)(ofs_ang_y * 4096.0f), 0, 0x3243);   /* 0x006AEEF8 */
    cam_excl_area_r = (float)r * 500.0f * 0.000244140625f * rate;           /* 0x0062D230, 0x006333F4 */
    if (*old_cam_excl_area_r_p != -1.0f) {
        min_add_dist = vcFrameTime() * -250.0f;                              /* 0x006C88F0 */
        add_dist = cam_excl_area_r - *old_cam_excl_area_r_p;
        if (add_dist < min_add_dist) add_dist = min_add_dist;
        cam_excl_area_r = add_dist + *old_cam_excl_area_r_p;
    }
    *old_cam_excl_area_r_p = cam_excl_area_r;
    dx = in_pos[0] - chara_pos[0];
    dz = in_pos[2] - chara_pos[2];
    dist = sqrtf(dx * dx + dz * dz);
    *flip_ang_y_p = cntr2pos_ang_y;
    if (dist < cam_excl_area_r) return cam_excl_area_r - dist;
    return 0.0f;
}

/* vcCamTgtMvVecIsFlipedFromCharaFront  PC 0x0051e280 */
static void vcCamTgtMvVecIsFlipedFromCharaFront(float *tgt_mv_vec, _VC_WORK *w_p, float max_tgt_mv_xz_len,
                                                int cur_rd_area_size)
{
    float pre_tgt_pos[4], post_tgt_pos[4], chk_pos[4], sincos_y[4];
    float flip_ang_y, flip_dist, chk_near_dist, min_in_road_dist, min_x, max_x, min_z, max_z, mv_len;
    _VC_NEAR_ROAD_DATA *use_nearest_p;

    pre_tgt_pos[0] = tgt_mv_vec[0] + w_p->cam_tgt_pos[0];
    pre_tgt_pos[2] = w_p->cam_tgt_pos[2] + tgt_mv_vec[2];
    pre_tgt_pos[1] = 0.0f;
    pre_tgt_pos[3] = 1.0f;
    flip_dist = vcFlipFromCamExclusionArea(&flip_ang_y, &w_p->old_cam_excl_area_r, pre_tgt_pos, w_p->chara_pos,
                                           w_p->chara_eye_ang_y, cur_rd_area_size);
    if (flip_dist <= 0.0f) return;
    sincos_y[0] = sincos_y[1] = sincos_y[2] = sincos_y[3] = 0.0f;
    shSinCosVec(sincos_y, flip_ang_y);

    if (w_p->cur_near_road.road_p->flags & VC_RD_MARGE_ROAD_F) {
        chk_near_dist = vcGetNearestNEAR_ROAD_DATA(&use_nearest_p, VC_CHK_NEAREST_ROAD_TYPE,
                                                   w_p->cur_near_road.road_p->rd_type, pre_tgt_pos, w_p, 1);
        if (use_nearest_p == NULL) use_nearest_p = &vcNullNearRoad;
        else if (chk_near_dist > 0.0f) use_nearest_p = &w_p->cur_near_road;
    } else {
        use_nearest_p = &w_p->cur_near_road;
    }
    post_tgt_pos[3] = 0.0f;
    post_tgt_pos[0] = pre_tgt_pos[0] + sincos_y[0] * flip_dist;
    post_tgt_pos[1] = sincos_y[1] * flip_dist + pre_tgt_pos[1];
    post_tgt_pos[2] = pre_tgt_pos[2] + sincos_y[2] * flip_dist;

    min_in_road_dist = vcGetMinInRoadDist();
    min_x = min_in_road_dist + use_nearest_p->rd.min_hx;
    max_x = use_nearest_p->rd.max_hx - min_in_road_dist;
    min_z = min_in_road_dist + use_nearest_p->rd.min_hz;
    max_z = use_nearest_p->rd.max_hz - min_in_road_dist;
    if (max_x < min_x) { min_x = (max_x + min_x) * 0.5f; max_x = min_x; }
    if (max_z < min_z) { min_z = (max_z + min_z) * 0.5f; max_z = min_z; }
    vcTransRotRoadArea(chk_pos, use_nearest_p->rd_rzm, post_tgt_pos);
    if (chk_pos[0] < min_x) chk_pos[0] = min_x;
    else if (chk_pos[0] > max_x) chk_pos[0] = max_x;
    if (chk_pos[2] < min_z) chk_pos[2] = min_z;
    else if (chk_pos[2] > max_z) chk_pos[2] = max_z;
    vcRotTransRoadArea(post_tgt_pos, use_nearest_p->rd_rim, chk_pos);

    tgt_mv_vec[0] = post_tgt_pos[0] - w_p->cam_tgt_pos[0];
    tgt_mv_vec[2] = post_tgt_pos[2] - w_p->cam_tgt_pos[2];
    mv_len = sqrtf(tgt_mv_vec[2] * tgt_mv_vec[2] + tgt_mv_vec[0] * tgt_mv_vec[0]);
    if (mv_len > max_tgt_mv_xz_len) {
        shSinCosVec(sincos_y, shVecAngY(tgt_mv_vec));
        tgt_mv_vec[3] = 0.0f;
        tgt_mv_vec[0] = sincos_y[0] * max_tgt_mv_xz_len;
        tgt_mv_vec[1] = sincos_y[1] * max_tgt_mv_xz_len;
        tgt_mv_vec[2] = sincos_y[2] * max_tgt_mv_xz_len;
    }
}

/* vcAutoRenewalCamTgtPos  PC 0x0051eea0 */
static void vcAutoRenewalCamTgtPos(_VC_WORK *w_p, int cam_mv_type, const _VC_CAM_MV_PARAM *cam_mv_prm_p,
                                   int cur_rd_flags, int cur_rd_area_size)
{
    float ideal_pos[4] = { 0, 0, 0, 0 }, tgt_vec[4] = { 0, 0, 0, 0 }, max_tgt_mv_xz_len, inv_dt;
    int make_type;

    switch (cam_mv_type) {
    case VC_MV_FIX_ANG:       vcMakeIdealCamPosForFixAngCam(ideal_pos, w_p); break;
    case VC_MV_SELF_VIEW:     vcMakeIdealCamPosByHeadPos(ideal_pos, w_p); break;
    case VC_MV_LOCUS_CIRCLE:  vcMakeIdealCamPosForLocusCircleCam(ideal_pos, w_p); break;
    case VC_MV_THROUGH_DOOR:  vcMakeIdealCamPosForThroughDoorCam(ideal_pos, w_p); break;
    default:                  vcMakeIdealCamPosUseVC_ROAD_DATA(ideal_pos, w_p, cur_rd_area_size); break;
    }
    if (w_p->flags & VC_WARP_CAM_TGT_F) v4copy(w_p->cam_tgt_pos, ideal_pos);

    switch (cam_mv_type) {
    case VC_MV_CHASE: case VC_MV_SETTLE: case VC_MV_FIX_ANG: case VC_MV_LOCUS_CIRCLE: case VC_MV_THROUGH_DOOR:
        make_type = MAKE_CAM_TGT_BY_ROAD; break;
    case VC_MV_SELF_VIEW:
        make_type = MAKE_CAM_TGT_BY_CHARA_HEAD; break;
    default:
        vcAssert(0xc77);
        make_type = MAKE_CAM_TGT_BY_ROAD; break;
    }
    if (make_type == MAKE_CAM_TGT_BY_CHARA_HEAD) {
        vcMakeBasicCamTgtMvVec(tgt_vec, ideal_pos, w_p, 500.0f);
    } else {
        max_tgt_mv_xz_len = vcRetMaxTgtMvXzLen(w_p, cam_mv_prm_p);
        vcMakeBasicCamTgtMvVec(tgt_vec, ideal_pos, w_p, max_tgt_mv_xz_len);
        if ((cam_mv_type == VC_MV_CHASE || cam_mv_type == VC_MV_LOCUS_CIRCLE) && !(cur_rd_flags & VC_RD_NO_FRONT_FLIP_F))
            vcCamTgtMvVecIsFlipedFromCharaFront(tgt_vec, w_p, max_tgt_mv_xz_len, cur_rd_area_size);
        if (cam_mv_type != VC_MV_THROUGH_DOOR)
            vcAdjTgtMvVecYByCurNearRoad(tgt_vec, w_p);
    }
    w_p->cam_tgt_mv_ang_y = shVecAngY(tgt_vec);
    if (vcFrameTime() == 0.0f && !(w_p->flags & VC_WARP_CAM_TGT_F)) {
        w_p->cam_tgt_velo[0] = 0.0f;
        w_p->cam_tgt_velo[2] = 0.0f;
        w_p->cam_tgt_spd = 0.0f;
    } else {
        w_p->cam_tgt_pos[0] = tgt_vec[0] + w_p->cam_tgt_pos[0];
        w_p->cam_tgt_pos[1] = tgt_vec[1] + w_p->cam_tgt_pos[1];
        w_p->cam_tgt_pos[3] = 0.0f;
        w_p->cam_tgt_pos[2] = tgt_vec[2] + w_p->cam_tgt_pos[2];
        inv_dt = 1.0f / vcFrameTime();
        w_p->cam_tgt_velo[3] = 0.0f;
        w_p->cam_tgt_velo[0] = tgt_vec[0] * inv_dt;
        w_p->cam_tgt_velo[1] = tgt_vec[1] * inv_dt;
        w_p->cam_tgt_velo[2] = tgt_vec[2] * inv_dt;
        w_p->cam_tgt_spd = sqrtf(w_p->cam_tgt_velo[2] * w_p->cam_tgt_velo[2] + w_p->cam_tgt_velo[0] * w_p->cam_tgt_velo[0]);
    }
    vcChangeProjByDist(&w_p->cur_near_road, w_p->cam_tgt_pos[1] - w_p->chara_bottom_y);
}

/* vcGetUseWatchAndCamMvParam  PC 0x0051bb50 */
static void vcGetUseWatchAndCamMvParam(_VC_WATCH_MV_PARAM **watch_mv_prm_pp, _VC_CAM_MV_PARAM **cam_mv_prm_pp,
                                       float self_view_eff_rate, float far_watch_rate, _VC_WORK *w_p)
{
    const _VC_WATCH_MV_PARAM *wth_mv_prm_stg_p;
    const _VC_CAM_MV_PARAM *cam_mv_prm_stg_p;
    _VC_WATCH_MV_PARAM *ws = &g_vc.watch_mv_prm_st;
    _VC_CAM_MV_PARAM *cs = &g_vc.cam_mv_prm_st;
    _VC_WATCH_MV_PARAM ps2_prm;
    float add_ang_accel_y, rate;

    if (w_p->flags & VC_USER_WATCH_F) {
        *watch_mv_prm_pp = &w_p->user_watch_mv_prm;
    } else {
        if (!vcIsOutdoorStage() && IN->stage_glb_crd != 5) wth_mv_prm_stg_p = &watch_mv_prm_nrml;
        else wth_mv_prm_stg_p = vcIsOnBoat() ? &watch_mv_prm_on_boat : &watch_mv_prm_outdoor;
        if (s_ps2_speed && wth_mv_prm_stg_p != &watch_mv_prm_on_boat) {
            ps2_prm = *wth_mv_prm_stg_p;
            ps2_prm.max_ang_spd_x = PS2_WATCH_MAX_ANG_SPD_X;
            wth_mv_prm_stg_p = &ps2_prm;
        }
        ws->ang_accel_x   = (self_view_watch_mv_prm.ang_accel_x   - wth_mv_prm_stg_p->ang_accel_x)   * self_view_eff_rate + wth_mv_prm_stg_p->ang_accel_x;
        ws->ang_accel_y   = (self_view_watch_mv_prm.ang_accel_y   - wth_mv_prm_stg_p->ang_accel_y)   * self_view_eff_rate + wth_mv_prm_stg_p->ang_accel_y;
        ws->max_ang_spd_x = (self_view_watch_mv_prm.max_ang_spd_x - wth_mv_prm_stg_p->max_ang_spd_x) * self_view_eff_rate + wth_mv_prm_stg_p->max_ang_spd_x;
        ws->max_ang_spd_y = (self_view_watch_mv_prm.max_ang_spd_y - wth_mv_prm_stg_p->max_ang_spd_y) * self_view_eff_rate + wth_mv_prm_stg_p->max_ang_spd_y;
        *watch_mv_prm_pp = ws;
        if (!vcIsOutdoorStage() && IN->stage_glb_crd != 5) {
            ws->ang_accel_x   = far_watch_rate * wth_mv_prm_stg_p->ang_accel_x   * 0.5f + ws->ang_accel_x;
            ws->ang_accel_y   = far_watch_rate * wth_mv_prm_stg_p->ang_accel_y   * 0.5f + ws->ang_accel_y;
            ws->max_ang_spd_x = far_watch_rate * wth_mv_prm_stg_p->max_ang_spd_x * 0.5f + ws->max_ang_spd_x;
            ws->max_ang_spd_y = far_watch_rate * wth_mv_prm_stg_p->max_ang_spd_y * 0.5f + ws->max_ang_spd_y;
        }
        add_ang_accel_y = w_p->chara_mv_spd;
        if (add_ang_accel_y < 0.0f) add_ang_accel_y = 0.0f;
        else if (add_ang_accel_y > 2.0f) add_ang_accel_y = 2.0f;     /* 0x0063158C */
        ws->ang_accel_y = ws->ang_accel_y + add_ang_accel_y;
    }

    if (w_p->flags & VC_USER_CAM_F) {
        *cam_mv_prm_pp = &w_p->user_cam_mv_prm;
        return;
    }
    if (!vcIsOutdoorStage() && IN->stage_glb_crd != 5) {
        cam_mv_prm_stg_p = &cam_mv_prm_nrml;
        rate = far_watch_rate + 1.0f;
    } else {
        rate = 1.0f;
        cam_mv_prm_stg_p = vcIsOnBoat() ? &cam_mv_prm_on_boat : &cam_mv_prm_outdoor;
    }
    cs->accel_xz   = rate * cam_mv_prm_stg_p->accel_xz;
    cs->accel_y    = rate * cam_mv_prm_stg_p->accel_y;
    cs->max_spd_xz = rate * cam_mv_prm_stg_p->max_spd_xz;
    cs->max_spd_y  = rate * cam_mv_prm_stg_p->max_spd_y;
    *cam_mv_prm_pp = cs;
}

/* vcRenewalCamData  PC 0x0051bd50: 60 Hz sub-stepped camera motion toward cam_tgt_pos */
static void vcRenewalCamData(_VC_WORK *w_p, const _VC_CAM_MV_PARAM *cam_mv_prm_p)
{
    float dt, sub_dt, side_dec, accel_y, d_x, d_y, d_z, accel_xz04, dx2, dz2, ang, sc[4], k, spd;
    float vec[4], lim, dyc, lim_y, ndx, ndy, ndz, over;
    int steps;
    float *velo = w_p->cam_velo, *cam = w_p->cam_pos, *tgt = w_p->cam_tgt_pos;

    if (w_p->flags & VC_WARP_CAM_F) {
        w_p->cam_mv_ang_y = shAtan2XY(tgt[2] - cam[2], tgt[0] - cam[0]);
        v4copy(cam, tgt);
        velo[0] = velo[1] = velo[2] = velo[3] = 0.0f;
        return;
    }
    dt = vcFrameTime();
    steps = (int)ceil((double)dt * (double)59.9988022f);           /* 0x006C88F8 */
    sub_dt = dt / (float)steps;
    if (steps <= 0) return;
    side_dec = sub_dt * 6000.0f;                                     /* 0x006C88F4 */
    do {
        accel_y = cam_mv_prm_p->accel_y;
        d_x = tgt[0] - cam[0];
        d_y = tgt[1] - cam[1];
        d_z = tgt[2] - cam[2];
        accel_xz04 = cam_mv_prm_p->accel_xz * 0.400000006f;          /* 0x0069DB44 */
        dx2 = tgt[0] * 0.00200000009f - cam[0] * 0.00200000009f;     /* 0x0063CFD4 */
        dz2 = tgt[2] * 0.00200000009f - cam[2] * 0.00200000009f;
        ang = shAtan2XY(dz2, dx2);
        shSinCosVec(sc, ang);
        (void)shVecAngY(velo);                                       /* computed, unused (PC) */
        k = sub_dt * cam_mv_prm_p->accel_xz;
        velo[0] = sc[0] * k + velo[0];
        velo[2] = sc[2] * k + velo[2];
        spd = sqrtf(velo[2] * velo[2] + velo[0] * velo[0]);
        if (spd > cam_mv_prm_p->max_spd_xz) {
            float a = shVecAngY(velo);
            over = spd - cam_mv_prm_p->max_spd_xz;
            velo[0] = velo[0] - over * sinf(a);
            velo[2] = velo[2] - over * cosf(a);
        }
        vec[0] = dx2; vec[1] = d_y; vec[2] = dz2; vec[3] = 0.0f;
        ang = shVecAngY(vec);
        lim = (sqrtf(vec[2] * vec[2] + vec[0] * vec[0]) - 0.100000001f) * accel_xz04;   /* 0x0069E7C8 */
        if (lim <= 0.0f) lim = 0.0f;
        vwLimitOverLimVector(velo, lim, ang);
        vwDecreaseSideOfVector(velo, side_dec, lim * 0.5f, ang);

        dyc = tgt[1] - cam[1];
        k = sub_dt * cam_mv_prm_p->accel_y;
        velo[1] = (dyc < 0.0f) ? velo[1] - k : k + velo[1];
        if (-cam_mv_prm_p->max_spd_y > velo[1]) velo[1] = -cam_mv_prm_p->max_spd_y;
        else if (velo[1] > cam_mv_prm_p->max_spd_y) velo[1] = cam_mv_prm_p->max_spd_y;
        lim_y = fabsf(dyc) * (s_ps2_speed ? PS2_CAM_EASE_Y : accel_y);
        if (dyc < 0.0f) { if (velo[1] <= -lim_y) velo[1] = -lim_y; }
        else            { if (!(velo[1] < lim_y)) velo[1] = lim_y; }

        w_p->cam_mv_ang_y = shVecAngY(velo);
        cam[0] = sub_dt * velo[0] + cam[0];
        cam[1] = sub_dt * velo[1] + cam[1];
        cam[3] = 0.0f;
        cam[2] = sub_dt * velo[2] + cam[2];
        ndx = tgt[0] - cam[0];
        ndy = tgt[1] - cam[1];
        ndz = tgt[2] - cam[2];
        if ((ndx <= 0.0f && d_x >= 0.0f) || (ndx >= 0.0f && d_x <= 0.0f)) cam[0] = tgt[0];
        if ((ndy <= 0.0f && d_y >= 0.0f) || (ndy >= 0.0f && d_y <= 0.0f)) cam[1] = tgt[1];
        if ((ndz <= 0.0f && d_z >= 0.0f) || (ndz >= 0.0f && d_z <= 0.0f)) cam[2] = tgt[2];
    } while (--steps != 0);
}

/* ===================================================================== */
/* vc_main: camera angle                                                  */
/* ===================================================================== */

/* vcMakeNewBaseCamAng  PC 0x0051c230 (only SETTLE produces a base angle) */
static void vcMakeNewBaseCamAng(float *new_base_ang, int cam_mv_type, _VC_WORK *w_p)
{
    float xyz_vec[4], cam2watch_ang_x, cam2watch_ang_y, ofs_sta_ang_y, ofs_end_ang_y;
    float sta, end, new_base_ang_y, mv_ang_y, max_ang_y_spd, new_base_ang_x;
    _VC_ROAD_DATA *rd;

    xyz_vec[3] = 0.0f;
    xyz_vec[0] = w_p->watch_tgt_pos[0] - w_p->cam_pos[0];
    xyz_vec[1] = w_p->watch_tgt_pos[1] - w_p->cam_pos[1];
    xyz_vec[2] = w_p->watch_tgt_pos[2] - w_p->cam_pos[2];
    if ((w_p->flags & VC_USER_WATCH_F) || cam_mv_type != VC_MV_SETTLE) {
        if (!(w_p->flags & VC_USER_WATCH_F) && (cam_mv_type < 0 || cam_mv_type > 5)) vcAssert(0x134e);
        new_base_ang[0] = new_base_ang[1] = new_base_ang[2] = new_base_ang[3] = 0.0f;
        return;
    }
    cam2watch_ang_x = shAtan2XY(shXZLen(xyz_vec), -xyz_vec[1]);
    cam2watch_ang_y = shVecAngY(xyz_vec);
    rd = w_p->cur_near_road.road_p;
    sta = rd->tmp.stl.sta_base_ang_y;
    end = rd->tmp.stl.end_base_ang_y;
    ofs_sta_ang_y = shNormRad(cam2watch_ang_y - sta);
    ofs_end_ang_y = shNormRad(cam2watch_ang_y - end);
    new_base_ang_y = cam2watch_ang_y;
    if (ofs_sta_ang_y < 0.0f || ofs_end_ang_y > 0.0f)
        new_base_ang_y = (fabsf(ofs_sta_ang_y) < fabsf(ofs_end_ang_y)) ? sta : end;

    if (!(w_p->flags & VC_WARP_WATCH_F)) {
        if (vcRetZeroIfTiny(w_p->chara_mv_spd) == 0.0f || fabsf(cam2watch_ang_x) >= 1.30899692f) {   /* 0x006C88FC */
            new_base_ang_y = w_p->base_cam_ang[1];
        } else {
            mv_ang_y = shNormRad(new_base_ang_y - w_p->base_cam_ang[1]);
            max_ang_y_spd = vcFrameTime() * 2.09439516f;              /* 0x006B4B28 */
            if (mv_ang_y < -max_ang_y_spd) mv_ang_y = -max_ang_y_spd;
            else if (mv_ang_y > max_ang_y_spd) mv_ang_y = max_ang_y_spd;
            new_base_ang_y = mv_ang_y + w_p->base_cam_ang[1];
        }
    }
    new_base_ang_x = (float)vwOresenHokan(mv_stl_ang_ary, 5, (int)(fabsf(cam2watch_ang_x) * 4096.0f), 0, 0x1921)
                   * 0.000244140625f;
    if (new_base_ang_x < 0.0f) new_base_ang_x = 0.0f;
    else if (new_base_ang_x > 1.57079637f) new_base_ang_x = 1.57079637f;
    if (cam2watch_ang_x < 0.0f) new_base_ang_x = new_base_ang_x * -1.0f;
    new_base_ang[0] = new_base_ang_x;
    new_base_ang[2] = 0.0f;
    new_base_ang[3] = 1.0f;
    new_base_ang[1] = new_base_ang_y;
}

/* vcRenewalBaseCamAngAndAdjustOfsCamAng  PC 0x0051c510 */
static void vcRenewalBaseCamAngAndAdjustOfsCamAng(_VC_WORK *w_p, const float *new_base_cam_ang)
{
    VcMat old_base_mat, new_base_mat, new_base_matT, ofs_mat, adj_ofs_mat;
    d3dxIdentity(adj_ofs_mat);
    d3dxIdentity(old_base_mat);
    d3dxIdentity(new_base_mat);
    d3dxIdentity(ofs_mat);
    vwRotMatrixYXZ(w_p->base_cam_ang, old_base_mat);
    vwRotMatrixYXZ(new_base_cam_ang, new_base_mat);
    vbTransposeMatrixWithoutTr(new_base_matT, new_base_mat);
    vwRotMatrixYXZ(w_p->ofs_cam_ang, ofs_mat);
    shMatMulRev(adj_ofs_mat, new_base_matT, old_base_mat);   /* old_base * new_base^T */
    shMatMulRev(ofs_mat, adj_ofs_mat, ofs_mat);              /* ofs * (old_base * new_base^T) */
    vwMatrixToAngleYXZ(w_p->ofs_cam_ang, ofs_mat);
    v4copy(w_p->base_cam_ang, new_base_cam_ang);
}

/* vcMakeOfsCamTgtAng  PC 0x0051c610 */
static void vcMakeOfsCamTgtAng(float *ofs_tgt_ang, const VcMat base_matT, _VC_WORK *w_p)
{
    float vec[4];
    vec[0] = w_p->watch_tgt_pos[0] - w_p->cam_pos[0];
    vec[3] = 0.0f;
    vec[1] = w_p->watch_tgt_pos[1] - w_p->cam_pos[1];
    vec[2] = w_p->watch_tgt_pos[2] - w_p->cam_pos[2];
    vbApplyMatrixWithoutTr(vec, base_matT, vec);
    (void)vwVectorToAngle(ofs_tgt_ang, vec);
    ofs_tgt_ang[2] = w_p->watch_tgt_ang_z;
}

/* vcMakeOfsCam2CharaBottomAndTopAngByBaseMatT  PC 0x0051c670 */
static void vcMakeOfsCam2CharaBottomAndTopAngByBaseMatT(float *ofs_cam2chara_btm_ang, float *ofs_cam2chara_top_ang,
                                                        const VcMat base_matT, const float *cam_pos,
                                                        const float *chara_pos, float chara_bottom_y, float chara_top_y)
{
    float vec[4];
    vec[0] = chara_pos[0] - cam_pos[0];
    vec[3] = 0.0f;
    vec[2] = chara_pos[2] - cam_pos[2];
    vec[1] = chara_bottom_y - cam_pos[1];
    vbApplyMatrixWithoutTr(vec, base_matT, vec);
    (void)vwVectorToAngle(ofs_cam2chara_btm_ang, vec);
    vec[0] = chara_pos[0] - cam_pos[0];
    vec[3] = 0.0f;
    vec[2] = chara_pos[2] - cam_pos[2];
    vec[1] = chara_top_y - cam_pos[1];
    vbApplyMatrixWithoutTr(vec, base_matT, vec);
    (void)vwVectorToAngle(ofs_cam2chara_top_ang, vec);
}

/* vcAdjCamOfsAngByCharaInScreen  PC 0x0051c710 */
static void vcAdjCamOfsAngByCharaInScreen(float *cam_ang, const float *ofs_cam2chara_btm_ang,
                                          const float *ofs_cam2chara_top_ang, _VC_WORK *w_p)
{
    float watch2chr_ofs_ang_y = shNormRad(ofs_cam2chara_top_ang[1] - cam_ang[1]);
    float watch2chr_bottom_ofs_ang_x = shNormRad(ofs_cam2chara_btm_ang[0] - cam_ang[0]);
    float watch2chr_top_ofs_ang_x = shNormRad(ofs_cam2chara_top_ang[0] - cam_ang[0]);
    float adj_cam_ang_y, adj_cam_ang_x, v;

    if (watch2chr_ofs_ang_y > w_p->scr_half_ang_wx)
        adj_cam_ang_y = shNormRad(watch2chr_ofs_ang_y - w_p->scr_half_ang_wx);
    else if (-w_p->scr_half_ang_wx > watch2chr_ofs_ang_y)
        adj_cam_ang_y = shNormRad(watch2chr_ofs_ang_y + w_p->scr_half_ang_wx);
    else
        adj_cam_ang_y = 0.0f;

    if (w_p->scr_half_ang_wy <= 0.0872664675f) vcAssert(0x1449);    /* 0x006A7910 */
    adj_cam_ang_x = 0.0f;
    if (-w_p->scr_half_ang_wy > watch2chr_bottom_ofs_ang_x)
        adj_cam_ang_x = watch2chr_bottom_ofs_ang_x + w_p->scr_half_ang_wy;
    v = watch2chr_top_ofs_ang_x - adj_cam_ang_x;
    if (v > w_p->scr_half_ang_wy)
        adj_cam_ang_x = (v - w_p->scr_half_ang_wy) + adj_cam_ang_x;
    if (adj_cam_ang_x < -0.52359879f) adj_cam_ang_x = -0.52359879f;  /* 0x006C8900 */
    else if (adj_cam_ang_x > 0.52359879f) adj_cam_ang_x = 0.52359879f;   /* 0x006A91C0 */
    cam_ang[1] = adj_cam_ang_y + cam_ang[1];
    cam_ang[0] = adj_cam_ang_x + cam_ang[0];
}

/* one axis of vcAdjCamOfsAngByOfsAngSpd (same pattern on x, y, z in the PC) */
static void vcAngSpdAxis(float *spd, float diff, float accel, float max_spd, float dec_per_dist)
{
    float s = diff * 0.318309873f;                                   /* 0x006B34A8 1/PI */
    float lim;
    *spd = (s < 0.0f) ? *spd - accel : accel + *spd;
    if (-max_spd > *spd) *spd = -max_spd;
    else if (*spd > max_spd) *spd = max_spd;
    lim = fabsf(s) * dec_per_dist;
    if (s < 0.0f) { if (*spd <= -lim) *spd = -lim; }
    else          { if (!(*spd < lim)) *spd = lim; }
}

/* vcAdjCamOfsAngByOfsAngSpd  PC 0x0051c850 */
static void vcAdjCamOfsAngByOfsAngSpd(float *ofs_ang, float *ofs_ang_spd, const float *ofs_tgt_ang,
                                      const _VC_WATCH_MV_PARAM *prm_p)
{
    float max_spd_dec_per_dist[2], dt, sub_dt, z_accel, d0, d1, d2, n0, n1, n2;
    int steps;
    max_spd_dec_per_dist[0] = prm_p->ang_accel_x * 8.0f;             /* 0x0069F3BC */
    max_spd_dec_per_dist[1] = prm_p->ang_accel_y * 3.0f;             /* 0x0062EDE0 */
    dt = vcFrameTime();
    steps = (int)ceil((double)dt * (double)59.9988022f);
    sub_dt = dt / (float)steps;
    if (steps <= 0) return;
    z_accel = sub_dt * 2.51327419f;                                  /* 0x006C890C */
    do {
        d0 = shNormRad(ofs_tgt_ang[0] - ofs_ang[0]);
        d1 = shNormRad(ofs_tgt_ang[1] - ofs_ang[1]);
        d2 = shNormRad(ofs_tgt_ang[2] - ofs_ang[2]);
        vcAngSpdAxis(&ofs_ang_spd[0], d0, sub_dt * prm_p->ang_accel_x, prm_p->max_ang_spd_x, max_spd_dec_per_dist[0]);
        /* PC quirk: the yaw speed is clamped with max_ang_spd_x, not max_ang_spd_y */
        vcAngSpdAxis(&ofs_ang_spd[1], d1, sub_dt * prm_p->ang_accel_y, prm_p->max_ang_spd_x, max_spd_dec_per_dist[1]);
        {
            /* roll: fixed accel/limits; the speed clamp uses the two .rdata bounds */
            float s = d2 * 0.318309873f, lim;
            ofs_ang_spd[2] = (s < 0.0f) ? ofs_ang_spd[2] - z_accel : z_accel + ofs_ang_spd[2];
            if (ofs_ang_spd[2] < -2.51327419f) ofs_ang_spd[2] = -2.51327419f;     /* 0x006C8908 */
            else if (ofs_ang_spd[2] > 2.51327419f) ofs_ang_spd[2] = 2.51327419f;  /* 0x006C890C */
            lim = fabsf(s) * 18.849556f;                                         /* 0x006C8904 */
            if (s < 0.0f) { if (ofs_ang_spd[2] <= -lim) ofs_ang_spd[2] = -lim; }
            else          { if (!(ofs_ang_spd[2] < lim)) ofs_ang_spd[2] = lim; }
        }
        ofs_ang[3] = 0.0f;
        ofs_ang[0] = sub_dt * ofs_ang_spd[0] + ofs_ang[0];
        ofs_ang[1] = sub_dt * ofs_ang_spd[1] + ofs_ang[1];
        ofs_ang[2] = sub_dt * ofs_ang_spd[2] + ofs_ang[2];
        n0 = shNormRad(ofs_tgt_ang[0] - ofs_ang[0]);
        n1 = shNormRad(ofs_tgt_ang[1] - ofs_ang[1]);
        n2 = shNormRad(ofs_tgt_ang[2] - ofs_ang[2]);
        if ((n0 <= 0.0f && d0 >= 0.0f) || (n0 >= 0.0f && d0 <= 0.0f)) ofs_ang[0] = ofs_tgt_ang[0];
        if ((n1 <= 0.0f && d1 >= 0.0f) || (n1 >= 0.0f && d1 <= 0.0f)) ofs_ang[1] = ofs_tgt_ang[1];
        if ((n2 <= 0.0f && d2 >= 0.0f) || (n2 >= 0.0f && d2 <= 0.0f)) ofs_ang[2] = ofs_tgt_ang[2];
    } while (--steps != 0);
}

/* vcMakeCamMatAndCamAngByBaseAngAndOfsAng  PC 0x0051e570 */
static void vcMakeCamMatAndCamAngByBaseAngAndOfsAng(float *cam_mat_ang, VcMat cam_mat, float *base_cam_ang,
                                                    const float *ofs_cam_ang, const float *cam_pos)
{
    VcMat base_mat, ofs_mat;
    _VC_WORK *w = &g_vc.work;
    d3dxIdentity(base_mat);
    d3dxIdentity(ofs_mat);
    if (w->cam_noise != 0) {   /* PC-only camera shake */
        g_vc.self_view_timer = vcFrameTime() * 30.0f + g_vc.self_view_timer;   /* 0x00638888 */
        base_cam_ang[0] = vcCamMatNoise(0.00261799409f, 1.83259583f, 1.04719758f) * (float)w->cam_noise + base_cam_ang[0];
        base_cam_ang[1] = vcCamMatNoise(0.00163624622f, 0.52359879f, -1.04719758f) * (float)w->cam_noise + base_cam_ang[1];
    }
    vwRotMatrixYXZ(base_cam_ang, base_mat);
    vwRotMatrixYXZ(ofs_cam_ang, ofs_mat);
    shMatMulRev(cam_mat, base_mat, ofs_mat);                 /* ofs_mat * base_mat */
    cam_mat[3][0] = cam_pos[0];
    cam_mat[3][1] = cam_pos[1];
    cam_mat[3][2] = cam_pos[2];
    cam_mat[3][3] = 1.0f;
    vwMatrixToAngleYXZ(cam_mat_ang, cam_mat);
}

/* vwSetViewInfoDirectMatrix(NULL, mat)  PC 0x0051faf0: the camera matrix handed to the renderer */
static void vcSetViewMatrix(const VcMat m)
{
    memcpy(g_vc.view_mat, m, sizeof(VcMat));
}

/* vcSetDataToVwSystem  PC 0x0051e680 */
static void vcSetDataToVwSystem(_VC_WORK *w_p, int cam_mv_type)
{
    float noise_ang[4];
    VcMat noise_mat, noise_cam_mat;
    if (cam_mv_type != VC_MV_SELF_VIEW) {
        vcSetViewMatrix(w_p->cam_mat);
        return;
    }
    g_vc.self_view_timer = vcFrameTime() + g_vc.self_view_timer;
    noise_ang[0] = vcCamMatNoise(0.00349065871f, 2.44346094f, 1.39626348f);
    noise_ang[1] = vcCamMatNoise(0.00218166155f, 0.69813174f, -1.39626348f);
    noise_ang[2] = 0.0f;
    noise_ang[3] = 1.0f;
    d3dxIdentity(noise_mat);
    vwRotMatrixYXZ(noise_ang, noise_mat);
    noise_mat[0][0] = vcCamMatNoise(0.00400000019f, -0.34906587f, 3.14159274f) + noise_mat[0][0];
    noise_mat[1][0] = vcCamMatNoise(0.00400000019f, -2.09439516f, -1.39626348f) + noise_mat[1][0];
    noise_mat[2][0] = vcCamMatNoise(0.00400000019f, -2.09439516f, 1.39626348f) + noise_mat[2][0];
    noise_mat[0][1] = vcCamMatNoise(0.00400000019f, 2.44346094f, 2.44346094f) + noise_mat[0][1];
    noise_mat[1][1] = vcCamMatNoise(0.00400000019f, 3.14159274f, 0.69813174f) + noise_mat[1][1];
    noise_mat[2][1] = vcCamMatNoise(0.00400000019f, -1.22173047f, -2.26892805f) + noise_mat[2][1];
    shMatMulRev(noise_cam_mat, w_p->cam_mat, noise_mat);    /* noise_mat * cam_mat */
    noise_cam_mat[3][0] = w_p->cam_mat[3][0];
    noise_cam_mat[3][1] = w_p->cam_mat[3][1];
    noise_cam_mat[3][2] = w_p->cam_mat[3][2];
    vcSetViewMatrix(noise_cam_mat);
}

/* vcRenewalCamMatAng  PC 0x0051f110 */
static void vcRenewalCamMatAng(_VC_WORK *w_p, const _VC_WATCH_MV_PARAM *watch_mv_prm_p, int cam_mv_type,
                               int visible_chara_f)
{
    float new_base_cam_ang[4], ofs_tgt_ang[4] = { 0, 0, 0, 0 }, ofs_cam2chara_btm_ang[4], ofs_cam2chara_top_ang[4];
    VcMat new_base_matT;

    vcMakeNewBaseCamAng(new_base_cam_ang, cam_mv_type, w_p);
    if (new_base_cam_ang[0] != w_p->base_cam_ang[0] || new_base_cam_ang[1] != w_p->base_cam_ang[1] ||
        new_base_cam_ang[2] != w_p->base_cam_ang[2])
        vcRenewalBaseCamAngAndAdjustOfsCamAng(w_p, new_base_cam_ang);
    d3dxIdentity(new_base_matT);
    vwRotMatrixYXZ(w_p->base_cam_ang, new_base_matT);
    vbTransposeMatrixWithoutTr(new_base_matT, new_base_matT);
    vcMakeOfsCamTgtAng(ofs_tgt_ang, new_base_matT, w_p);
    if (visible_chara_f) {
        vcMakeOfsCam2CharaBottomAndTopAngByBaseMatT(ofs_cam2chara_btm_ang, ofs_cam2chara_top_ang, new_base_matT,
            w_p->cam_pos, w_p->chara_pos,
            w_p->cur_near_road.road_p->trace_btm_hy + w_p->chara_bottom_y, w_p->chara_top_y);
        vcAdjCamOfsAngByCharaInScreen(ofs_tgt_ang, ofs_cam2chara_btm_ang, ofs_cam2chara_top_ang, w_p);
    }
    if (w_p->flags & VC_WARP_WATCH_F) {
        v4copy(w_p->ofs_cam_ang, ofs_tgt_ang);
        w_p->ofs_cam_ang_spd[0] = w_p->ofs_cam_ang_spd[1] = w_p->ofs_cam_ang_spd[2] = w_p->ofs_cam_ang_spd[3] = 0.0f;
    } else {
        vcAdjCamOfsAngByOfsAngSpd(w_p->ofs_cam_ang, w_p->ofs_cam_ang_spd, ofs_tgt_ang, watch_mv_prm_p);
    }
    if (!(w_p->flags & VC_USER_MODE_FLAGS))
        vcCorrectCamMatAngForcibly(w_p->ofs_cam_ang, *w_p->cur_near_road.road_p);
    vcMakeCamMatAndCamAngByBaseAngAndOfsAng(w_p->cam_mat_ang, w_p->cam_mat, new_base_cam_ang, w_p->ofs_cam_ang,
                                            w_p->cam_pos);
    w_p->ofs_cam_ang[0] = shNormRad(w_p->ofs_cam_ang[0]);
    w_p->ofs_cam_ang[1] = shNormRad(w_p->ofs_cam_ang[1]);
    w_p->ofs_cam_ang[2] = shNormRad(w_p->ofs_cam_ang[2]);
}

/* ===================================================================== */
/* vcExecCamera  PC 0x0051f660                                            */
/* ===================================================================== */

static int vcExecCamera(void)
{
    _VC_WORK *w = &g_vc.work;
    _VC_WATCH_MV_PARAM *watch_mv_prm_p;
    _VC_CAM_MV_PARAM *cam_mv_prm_p;
    float sv_old_cam_pos[4], sv_old_cam_mat_ang[4], far_watch_rate, self_view_eff_rate;
    int warp_f, cur_cam_mv_type, cur_rd_flags, cur_rd_area_size;
    _VC_ROAD_DATA *rd;

    v4copy(sv_old_cam_pos, w->cam_pos);
    v4copy(sv_old_cam_mat_ang, w->cam_mat_ang);
    if (w->view_cam_active_f == 0) return 0;

    vcSetAllNpcDeadTimer();
    vcPreSetDataInVC_WORK(w, w->vc_road_ary_list);
    warp_f = vcSetCurNearRoadInVC_WORK(w);
    rd = w->cur_near_road.road_p;
    if (rd == NULL) {
        vcAssert(0x369);
        return 0;
    }
    cur_rd_flags = rd->flags;
    cur_rd_area_size = rd->area_size_type;
    if (w->through_door.active_f) {
        if (!vcRetThroughDoorCamEndF(w)) {
            cur_cam_mv_type = VC_MV_THROUGH_DOOR;
            goto have_type;
        }
        w->through_door.active_f = 0;
        w->through_door.timer = 0.0f;
    }
    cur_cam_mv_type = w->cur_near_road.road_p->cam_mv_type;
have_type:
    far_watch_rate = vcRetFarWatchRate((w->flags & VC_PRS_F_VIEW_F) != 0, cur_cam_mv_type, w);
    self_view_eff_rate = vcRetSelfViewEffectRate(cur_cam_mv_type, w);
    if (!(w->flags & VC_USER_MODE_FLAGS))
        vcSetFlagsByCamMvType(cur_cam_mv_type, far_watch_rate, warp_f);
    if (w->flags & VC_WARP_CAM_TGT_F)
        w->old_cam_excl_area_r = -1.0f;
    if (!(w->flags & VC_USER_MODE_FLAGS))
        vcPcSpecialRoom(w, IN->stage_glb_crd);
    vcGetUseWatchAndCamMvParam(&watch_mv_prm_p, &cam_mv_prm_p, self_view_eff_rate, far_watch_rate, w);
    if (!(w->flags & VC_USER_CAM_F))
        vcAutoRenewalCamTgtPos(w, cur_cam_mv_type, cam_mv_prm_p, cur_rd_flags, cur_rd_area_size);
    vcRenewalCamData(w, cam_mv_prm_p);
    if (!(w->flags & VC_USER_CAM_F))
        vcChangeProjectionValue(w);
    if (!(w->flags & VC_USER_WATCH_F)) {
        vcAutoRenewalWatchTgtPosAndAngZ(w, cur_cam_mv_type, cur_rd_area_size, far_watch_rate, self_view_eff_rate);
        if ((w->cur_near_road.road_p->flags & VC_RD_LIM_UP_FAR_VIEW_F) &&
            (w->cur_near_road.road_p->cam_mv_type == VC_MV_CHASE || cur_cam_mv_type == VC_MV_SELF_VIEW))
            vcAdjustWatchYLimitHighWhenFarView(w->watch_tgt_pos, w->cam_pos);
    }
    vcRenewalCamMatAng(w, watch_mv_prm_p, cur_cam_mv_type, (w->flags & VC_VISIBLE_CHARA_F) != 0);
    vcSetDataToVwSystem(w, cur_cam_mv_type);
    vcEndProcessingVC_WORK();
    g_vc.last_cam_mv_type = cur_cam_mv_type;   /* port only: reported in SH2CamOutput */
    return vcRetSmoothCamMvF(sv_old_cam_pos, w->cam_pos, sv_old_cam_mat_ang, w->cam_mat_ang);
}

/* ===================================================================== */
/* Remaining PS2-named API                                                */
/* ===================================================================== */

/* vcSetFirstCamWork  PC 0x0051d030 */
void vcSetFirstCamWork(const float *cam_pos, float chara_eye_ang_y, int use_through_door_cam_f)
{
    _VC_WORK *w = &g_vc.work;
    memset(w->ofs_cam_ang_spd, 0, sizeof w->ofs_cam_ang_spd);
    w->flags = VC_INIT_FLAGS;
    v4copy(w->cam_pos, cam_pos);
    memset(w->cam_velo, 0, sizeof w->cam_velo);
    v4copy(w->cam_tgt_pos, cam_pos);
    memset(w->cam_tgt_velo, 0, sizeof w->cam_tgt_velo);
    w->cam_mv_ang_y = 0.0f;
    w->cam_tgt_spd = 0.0f;
    w->cam_chara2ideal_ang_y = shNormRad(chara_eye_ang_y + 3.14159274f);
    w->cur_near_road = vcNullNearRoad;
    w->through_door_activate_init_f = use_through_door_cam_f;
    w->through_door.active_f = 0;
    w->through_door.timer = 0.0f;
}

/* vcReturnPreAutoCamWork  PC 0x0051d170 */
void vcReturnPreAutoCamWork(int warp_f)
{
    _VC_WORK *w = &g_vc.work;
    uint32_t old;
    if (warp_f) w->flags |= VC_ALL_WARP_FLAGS;
    old = w->flags;
    w->flags &= ~(uint32_t)VC_USER_MODE_FLAGS;
    g_vc.proj.new_proj = w->cur_near_road.road_p->projection;
    if (g_vc.proj.new_proj == 0.0f) g_vc.proj.new_proj = 448.0f;
    g_vc.proj.old_proj = g_vc.scr_z;
    g_vc.proj.flg = 2;
    if (old & VC_PROJ_MOMENT_CHANGE_F)
        w->flags = old & ~(uint32_t)(VC_USER_MODE_FLAGS | VC_PROJ_MOMENT_CHANGE_F);
}

/* vcSetEventCamParamRefView  PC 0x0051ced0 */
void vcSetEventCamParamRefView(const float *vp, const _VC_CAM_MV_PARAM *cam_prm_p, const float *vr,
                               const _VC_WATCH_MV_PARAM *watch_prm_p, float rot_z, int warp_flg)
{
    vcUserCamTarget(vp, cam_prm_p, warp_flg);
    vcUserWatchTarget(vr, watch_prm_p, rot_z, warp_flg);
}

/* vcSetProjectionValue  PC 0x0051cf00 */
void vcSetProjectionValue(float new_proj, int framecnt)
{
    vcSetProjectionValueInternal(new_proj, framecnt);
}

/* PC 0x0051cc70 (PC only): camera shake multiplier used by vcMakeCamMat... */
void vcSetCamNoise(int32_t noise)
{
    g_vc.work.cam_noise = noise;
}

/* ===================================================================== */
/* Port entry points                                                      */
/* ===================================================================== */

void sh2cam_convert_cam_file(_VC_ROAD_DATA *road_ary)
{
    if (road_ary != NULL) vcConvertCamFile(road_ary);
}

static void sh2cam_default_input(SH2CamInput *in)
{
    memset(in, 0, sizeof *in);
    in->screen_sx = 640.0f;
    in->screen_sy = 448.0f;
    d3dxIdentity(in->hero_neck_wm);
}

/* PC 0x00518f70 (vcInitCamera) */
void sh2cam_init(_VC_ROAD_DATA **road_ary_list, const float chara_pos[3], float chara_rot_y,
                 int use_through_door_cam_f, const SH2CamCallbacks *cb)
{
    float cam_pos[4], ang[4], zero[4] = { 0, 0, 0, 0 };
    _VC_WORK *w = &g_vc.work;

    if (!g_vc.bss_cleared) {           /* the PC globals live in zeroed .bss */
        memset(&g_vc, 0, sizeof g_vc);
        g_vc.bss_cleared = 1;
        g_vc.scr_z = 448.0f;
        d3dxIdentity(g_vc.view_mat);
    }
    if (cb) g_vc.cb = *cb;
    else    memset(&g_vc.cb, 0, sizeof g_vc.cb);
    sh2cam_default_input(&g_vc.in_default);
    g_vc.in = &g_vc.in_default;
    g_vc.scr_sx = g_vc.in_default.screen_sx;
    g_vc.scr_sy = g_vc.in_default.screen_sy;

    /* vcSetCameraUseWarp  PC 0x00518ca0 */
    cam_pos[0] = chara_pos[0] - sinf(chara_rot_y) * 795.0f;          /* 0x006C7A78 */
    cam_pos[1] = chara_pos[1] - 900.0f;                              /* 0x006A7904 */
    cam_pos[2] = chara_pos[2] - cosf(chara_rot_y) * 795.0f;
    cam_pos[3] = 1.0f;
    vcSetFirstCamWork(cam_pos, chara_rot_y, use_through_door_cam_f);

    /* vwInitViewInfo  PC 0x0051fb20 -> FUN_0051e820: camera matrix from (0, chara_eye_ang_y, 0) */
    ang[0] = 0.0f; ang[1] = w->chara_eye_ang_y; ang[2] = 0.0f; ang[3] = 0.0f;
    vcMakeCamMatAndCamAngByBaseAngAndOfsAng(w->cam_mat_ang, w->cam_mat, zero, ang, w->cam_pos);
    vcSetViewMatrix(w->cam_mat);

    vcInitVCSystem(road_ary_list);
    vcStartCameraSystem();
    vcSetProjectionValueInternal(0.0f, 0);
    w->flags |= VC_PROJ_MOMENT_CHANGE_F;
}

void sh2cam_set_road_list(_VC_ROAD_DATA **road_ary_list)
{
    g_vc.work.vc_road_ary_list = road_ary_list ? road_ary_list : vcNullRoadArrayList;
}

int sh2cam_exec(const SH2CamInput *in, SH2CamOutput *out)
{
    _VC_WORK *w = &g_vc.work;
    int smooth_f;

    if (!g_vc.bss_cleared) {
        static const float zero3[3] = { 0, 0, 0 };
        sh2cam_init(NULL, zero3, 0.0f, 0, NULL);
    }
    g_vc.in = in ? in : &g_vc.in_default;
    vcSetFrameRate(g_vc.in->frame_ticks60);
    g_vc.scr_sx = g_vc.in->screen_sx;
    g_vc.scr_sy = g_vc.in->screen_sy;

    /* PC driver (0x00519228..0x00519280): far watch is inhibited while DAT_01F90768 == 0 */
    if (g_vc.in->inhibit_far_watch) w->flags |= VC_INHIBIT_FAR_WATCH_F;
    else                            w->flags &= ~(uint32_t)VC_INHIBIT_FAR_WATCH_F;

    vcSetSubjChara(g_vc.in->chara_pos, g_vc.in->chara_bottom_y, g_vc.in->chara_top_y, g_vc.in->chara_grnd_y,
                   g_vc.in->chara_head_pos, g_vc.in->chara_mv_spd, g_vc.in->chara_mv_ang_y,
                   g_vc.in->chara_ang_spd_y, g_vc.in->chara_eye_ang_y, g_vc.in->chara_eye_ang_wy,
                   g_vc.in->chara_watch_xz_r);
    smooth_f = vcExecCamera();

    if (out != NULL) {
        v4copy(out->cam_pos, w->cam_pos);
        v4copy(out->watch_tgt_pos, w->watch_tgt_pos);
        memcpy(out->view_mat, g_vc.view_mat, sizeof out->view_mat);
        v4copy(out->cam_mat_ang, w->cam_mat_ang);
        out->watch_tgt_ang_z = w->watch_tgt_ang_z;
        out->scr_z = g_vc.scr_z;
        out->scr_half_ang_wx = w->scr_half_ang_wx;
        out->scr_half_ang_wy = w->scr_half_ang_wy;
        out->smooth_f = smooth_f;
        out->cam_mv_type = g_vc.last_cam_mv_type;
        out->cur_road = w->cur_near_road.road_p;
        out->flags = w->flags;
    }
    g_vc.in = &g_vc.in_default;
    return smooth_f;
}

const _VC_WORK *sh2cam_work(void)
{
    return &g_vc.work;
}
