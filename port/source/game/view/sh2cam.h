/*
 * sh2cam.h - Silent Hill 2 (PC Director's Cut) in-game camera ("view" module), portable C99.
 *
 * Translated from the PC executable's view module (PC 0x00519430..0x00522410), using the
 * PS2 beta's DWARF names/types (view/vc_main.c, vc_calc.c, vc_newest.c, vw_calc.c, vb_main.c).
 * See PORTING_NOTES.md for the function-by-function map, open questions and usage.
 *
 * World conventions (unchanged from the game): units ~2 mm, Y axis points DOWN, angles in
 * radians, yaw 0 looks along +Z, yaw = atan2(x, z).
 *
 * This module has no dependency on the rest of the port: only <stdint.h>/<stdbool.h> here and
 * <math.h>/<string.h> in sh2cam.c.  Everything the camera reads from the game is passed in
 * SH2CamInput each frame; everything it produces comes back in SH2CamOutput.
 */
#ifndef SH2CAM_H
#define SH2CAM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Enums (PS2 view/vc_main.c)                                            */
/* ===================================================================== */

typedef unsigned char _VC_AREA_SIZE_TYPE;
enum { VC_AREA_TINY, VC_AREA_SMALL, VC_AREA_WIDE, VC_AREA_OUTDOOR, VC_AREA_SUU };

typedef unsigned char _VC_ROAD_TYPE;
enum { VC_RD_TYPE_ROAD, VC_RD_TYPE_EFFECT, VC_RD_TYPE_EVENT, VC_RD_TYPE_ROAD_PRIO_LOW,
       VC_RD_TYPE_ROAD_PRIO_HIGH, VC_RD_TYPE_SV_ONLY, VC_RD_TYPE_SUU };

typedef unsigned char _VC_CAM_MV_TYPE;
enum { VC_MV_CHASE, VC_MV_SETTLE, VC_MV_FIX_ANG, VC_MV_SELF_VIEW, VC_MV_LOCUS_CIRCLE,
       VC_MV_THROUGH_DOOR, VC_MV_SUU };

/* _VC_ROAD_DATA.flags */
typedef unsigned short _VC_ROAD_FLAGS;
enum {
    VC_RD_NOFLAG            = 0x000,
    VC_RD_END_DATA_F        = 0x001,  /* terminator record of a .cam array            */
    VC_RD_WARP_IN_F         = 0x002,
    VC_RD_WARP_OUT_F        = 0x004,
    VC_RD_WARP_IN_OUT_FS    = 0x006,
    VC_RD_NO_FRONT_FLIP_F   = 0x008,
    VC_RD_LIM_UP_FAR_VIEW_F = 0x010,
    VC_RD_USE_NO_ENEMY_F    = 0x020,
    VC_RD_USE_NEAR_ENEMY_F  = 0x040,
    VC_RD_MARGE_ROAD_F      = 0x080,
    VC_RD_NO_EXTRA_AREA_F   = 0x100,
    VC_RD_CAM_LIKE_SETTLE_F = 0x200,
    VC_RD_NOT_WARP_F        = 0x400,
    VC_RD_INVALID_SV_F      = 0x800
};

/* _VC_WORK.flags.  0x800 is a PC-only bit (special watch height, see vcPcSpecialRoom). */
enum {
    VC_USER_CAM_F           = 0x001,
    VC_USER_WATCH_F         = 0x002,
    VC_WARP_CAM_F           = 0x004,
    VC_WARP_WATCH_F         = 0x008,
    VC_WARP_CAM_TGT_F       = 0x010,
    VC_SWITCH_NEAR_RD_DATA_F= 0x020,
    VC_PROJ_MOMENT_CHANGE_F = 0x040,
    VC_VISIBLE_CHARA_F      = 0x080,
    VC_INHIBIT_FAR_WATCH_F  = 0x100,
    VC_PRS_F_VIEW_F         = 0x200,
    VC_OLD_PRS_F_VIEW_F     = 0x400,
    VC_PC_SPECIAL_WATCH_F   = 0x800,  /* PC only */
    VC_INIT_FLAGS           = 0x01c,
    VC_ALL_CAM_FLAGS        = 0x005,
    VC_ALL_WATCH_FLAGS      = 0x082,
    VC_USER_MODE_FLAGS      = 0x003,
    VC_ALL_WARP_FLAGS       = 0x01c
};

enum { VC_CHK_NEAREST_ROAD_TYPE, VC_CHK_NEAREST_SWITCH_TYPE };   /* _enum_0 */
enum { MAKE_CAM_TGT_BY_ROAD, MAKE_CAM_TGT_BY_CHARA_HEAD };      /* _enum_1 */

/* ===================================================================== */
/* On-disk .cam record (layout must stay exact: 128 bytes, no pointers)  */
/* ===================================================================== */

typedef struct _VC_ROAD_AREA {
    float x0, z0;        /* rectangle corner 0                                   */
    float x1, z1;        /* pivot corner (adjacent to 0 and 2)                   */
    float x2, z2;        /* rectangle corner 2                                   */
    float min_hy;        /* lower Y limit in world terms = numerically LARGER (Y down) */
    float max_hy;        /* upper Y limit = numerically smaller                  */
} _VC_ROAD_AREA;

typedef struct _VC_CHASE_CAM_PARAM        { float ofs_hy, ratio_r_xz, lr_lim_ang_y, rr_lim_ang_y; } _VC_CHASE_CAM_PARAM;
typedef struct _VC_SETTLE_CAM_PARAM       { float sta_base_ang_y, end_base_ang_y, lr_lim_ang_y, rr_lim_ang_y; } _VC_SETTLE_CAM_PARAM;
typedef struct _VC_FIX_ANG_CAM_PARAM      { float ang_x, ang_y, ofs_hy, cam2wth_dist; } _VC_FIX_ANG_CAM_PARAM;
typedef struct _VC_LOCUS_CIRCLE_CAM_PARAM { float origin_x, origin_z, ang_y, radius; } _VC_LOCUS_CIRCLE_CAM_PARAM;

typedef union _VC_ROAD_TMP {               /* PS2 "_anon5" */
    unsigned char              data[16];
    _VC_CHASE_CAM_PARAM        chs;
    _VC_SETTLE_CAM_PARAM       stl;
    _VC_FIX_ANG_CAM_PARAM      fix;
    _VC_LOCUS_CIRCLE_CAM_PARAM cir;
} _VC_ROAD_TMP;

typedef struct _VC_ROAD_DATA {
    _VC_ROAD_AREA lim_sw;          /* +0x00 switch area                 */
    _VC_ROAD_AREA lim_rd;          /* +0x20 road (camera) area          */
    int32_t  kind_id;              /* +0x40                             */
    int32_t  flags;                /* +0x44 _VC_ROAD_FLAGS              */
    int32_t  area_size_type;       /* +0x48 _VC_AREA_SIZE_TYPE          */
    int32_t  rd_type;              /* +0x4C _VC_ROAD_TYPE               */
    int32_t  mv_y_type;            /* +0x50 0..5                        */
    float    ofs_watch_hy;         /* +0x54                             */
    float    trace_btm_hy;         /* +0x58                             */
    int32_t  rd_dir_type;          /* +0x5C 0 = along X, 1 = along Z    */
    float    projection;           /* +0x60 scr_z for this road         */
    float    proj_volume;          /* +0x64 (unused by the camera)      */
    float    proj_sec;             /* +0x68 projection blend time (s)   */
    int32_t  cam_mv_type;          /* +0x6C _VC_CAM_MV_TYPE             */
    _VC_ROAD_TMP tmp;              /* +0x70                             */
} _VC_ROAD_DATA;

/* ===================================================================== */
/* Runtime camera types (PS2 view/vc_main.c)                             */
/* ===================================================================== */

typedef struct _VC_LIMIT_AREA { float min_hx, max_hx, min_hz, max_hz, min_hy, max_hy; } _VC_LIMIT_AREA;

typedef struct _VC_NEAR_ROAD_DATA {
    _VC_ROAD_DATA *road_p;         /* +0x00 */
    int32_t  rd_dir_type;          /* +0x04 */
    int32_t  use_priority;         /* +0x08 */
    float    chara2road_sum_dist;  /* +0x0C */
    float    chara2road_vec_x;     /* +0x10 */
    float    chara2road_vec_z;     /* +0x14 */
    _VC_LIMIT_AREA rd;             /* +0x18 road area bounds in road-local space   */
    _VC_LIMIT_AREA sw;             /* +0x30 switch area bounds in switch-local space */
    float    rd_rzm[4][4];         /* +0x48 world -> road-local                     */
    float    rd_rim[4][4];         /* +0x88 road-local -> world                     */
    float    sw_rzm[4][4];         /* +0xC8 world -> switch-local                   */
    float    sw_rim[4][4];         /* +0x108 switch-local -> world                  */
} _VC_NEAR_ROAD_DATA;              /* 0x148 bytes with 32-bit pointers              */

typedef struct _VC_CAM_MV_PARAM   { float accel_xz, accel_y, max_spd_xz, max_spd_y; } _VC_CAM_MV_PARAM;
typedef struct _VC_WATCH_MV_PARAM { float ang_accel_x, ang_accel_y, max_ang_spd_x, max_ang_spd_y; } _VC_WATCH_MV_PARAM;

typedef struct _VC_THROUGH_DOOR_CAM_PARAM {
    int32_t active_f;
    float   timer;
    float   rail_ang_y;
    float   rail_sta_pos[4];
    float   rail_sta_to_chara_dist;
} _VC_THROUGH_DOOR_CAM_PARAM;

typedef struct _VC_FIX_CAM_MANAGER { int32_t exception_f; float add_ang_y, add_rd_x, add_rd_z; } _VC_FIX_CAM_MANAGER;
typedef struct _VC_CIR_CAM_MANAGER { float origin[4]; float sw_l[4]; } _VC_CIR_CAM_MANAGER;

struct SH2CamChara;

/*
 * Camera work struct.  Field order is the PS2 one plus the PC-only int at +0x30; with
 * 32-bit pointers (PC, 3DS) every offset equals the PC build (base 0x01F83DE0, vcWork).
 */
typedef struct _VC_WORK {
    int32_t  view_cam_active_f;              /* +0x000 */
    _VC_ROAD_DATA **vc_road_ary_list;        /* +0x004 NULL-terminated list of .cam arrays */
    uint32_t flags;                          /* +0x008 _VC_FLAGS (PC stores 32 bits) */
    int32_t  through_door_activate_init_f;   /* +0x00C */
    _VC_THROUGH_DOOR_CAM_PARAM through_door; /* +0x010 */
    int32_t  cam_noise;                      /* +0x030 PC only: camera shake multiplier (int) */
    float    scr_half_ang_wy;                /* +0x034 */
    float    scr_half_ang_wx;                /* +0x038 */
    _VC_CAM_MV_PARAM user_cam_mv_prm;        /* +0x03C */
    float    cam_tgt_pos[4];                 /* +0x04C */
    float    cam_pos[4];                     /* +0x05C (gInGameCameraPosY = cam_pos[1]) */
    float    cam_mv_ang_y;                   /* +0x06C */
    float    cam_velo[4];                    /* +0x070 */
    float    old_cam_excl_area_r;            /* +0x080 */
    float    ideal_cam_pos_h;                /* +0x084 */
    _VC_WATCH_MV_PARAM user_watch_mv_prm;    /* +0x088 */
    float    watch_tgt_pos[4];               /* +0x098 */
    float    watch_tgt_max_y;                /* +0x0A8 */
    float    watch_tgt_ang_z;                /* +0x0AC */
    float    cam_mat_ang[4];                 /* +0x0B0 */
    float    cam_mat[4][4];                  /* +0x0C0 */
    float    ofs_cam_ang[4];                 /* +0x100 */
    float    ofs_cam_ang_spd[4];             /* +0x110 */
    float    base_cam_ang[4];                /* +0x120 */
    float    old_base_cam_ang[4];            /* +0x130 */
    float    cam_chara2ideal_ang_y;          /* +0x140 */
    float    cam_tgt_velo[4];                /* +0x144 */
    float    cam_tgt_mv_ang_y;               /* +0x154 */
    float    cam_tgt_spd;                    /* +0x158 */
    float    chara_pos[4];                   /* +0x15C */
    float    chara_bottom_y;                 /* +0x16C */
    float    chara_top_y;                    /* +0x170 */
    float    chara_center_y;                 /* +0x174 */
    float    chara_grnd_y;                   /* +0x178 */
    float    chara_head_pos[4];              /* +0x17C */
    float    chara_mv_spd;                   /* +0x18C */
    float    chara_mv_ang_y;                 /* +0x190 */
    float    chara_ang_spd_y;                /* +0x194 */
    float    chara_eye_ang_y;                /* +0x198 */
    float    chara_eye_ang_wy;               /* +0x19C */
    float    chara_watch_xz_r;               /* +0x1A0 */
    _VC_FIX_CAM_MANAGER fix_man;             /* +0x1A4 */
    _VC_CIR_CAM_MANAGER cir_man;             /* +0x1B4 */
    _VC_NEAR_ROAD_DATA near_road_ary[128];   /* +0x1D4 */
    int32_t  near_road_suu;                  /* +0xA5D4 */
    _VC_NEAR_ROAD_DATA cur_near_road;        /* +0xA5D8 */
    struct SH2CamChara *nearest_enemy_p;     /* +0xA720 */
    float    nearest_enemy_xz_dist;          /* +0xA724 */
    float    enemy_input_watch_y;            /* +0xA728 (unused on PC) */
    struct SH2CamChara *nearest_item_p;      /* +0xA72C */
    float    nearest_item_xz_dist;           /* +0xA730 */
} _VC_WORK;

/* ===================================================================== */
/* Engine interface                                                      */
/* ===================================================================== */

#define SH2CAM_CHARA_SLOTS 20

/*
 * One entry of the game's character slot table (PC 0x01F991A0, 20 slots, read through
 * FUN_00542920).  The camera scans enemies and items in slot order and STOPS at the first
 * slot that is not of the requested category (that is how the PC loop works), so pass the
 * slots in the game's order, with empty slots as kind == 0.
 */
typedef struct SH2CamChara {
    int16_t  kind;            /* SubCharacter.kind (+0x10). High byte = category: 0x02 enemy, 0x07 item */
    float    pos[3];          /* SubCharacter.pos (+0x1C)                                       */
    float    eye_y;           /* SubCharacter.eye_y (+0xA0): used as far-watch height            */
    uint32_t battle_status;   /* SubCharacter.battle.status (+0x148): 0x02 dead, 0x04 not-active, 0x20 ignore */
    float    dead_timer;      /* SubCharacter.battle.dead_timer (+0x118): WRITTEN by the camera   */
    uint8_t  is_lock_target;  /* 1 if this is sh2jms.target (0x01FBACC4) and lock byte (0x01FBAD16) != 0xFF */
} SH2CamChara;

/* Values the PC camera reads from the rest of the game each frame. */
typedef struct SH2CamInput {
    /* --- timing.  PC SetFrameRate(x) (0x00447960) gets x = elapsed 60 Hz ticks (1.0 at 60 fps,
       2.0 at 30 fps, 0 when paused).  The camera derives gFrametime = x/60, (int)x and 60/x. */
    float    frame_ticks60;

    /* --- screen (VbScreenInfo): sx/sy in the same pixel units as the road "projection" value. */
    float    screen_sx;               /* 0x01F83BC4 */
    float    screen_sy;               /* 0x01F83BC8 */

    /* --- global game state */
    int32_t  stage_glb_crd;           /* stage->glb_crd  (Stage_Data* @0x01F7D550, +0x24)       */
    uint8_t  area_code;               /* byte @0x01DBEB80 (camera only tests "< 0x35")           */
    uint8_t  control_type;            /* gControlType byte @0x01DBEC08 (1 = the "2D/radio" type)  */
    int32_t  transition_state;        /* gTransitionState (camera treats 0 and 2 as "idle")      */
    uint8_t  far_view_button;         /* FUN_005342b0: look/far-view button held                  */
    uint8_t  inhibit_far_watch;       /* PC driver: DAT_01F90768 == 0 -> VC_INHIBIT_FAR_WATCH_F   */
    uint8_t  chara_slots_disabled;    /* DAT_01D823C4 == 0x14 (no characters)                     */

    /* --- James (player SubCharacter at *(0x01FBA880)) */
    uint32_t james_status;            /* SubCharacter.status (+0x04): 0x20000 = on the boat       */
    uint32_t james_battle_status;     /* +0x148: 0x10 = ignore enemies, 0x20 = ignore items       */
    float    james_pos_x;             /* SubCharacter.pos.x (stage 1 "x >= -20000" tests)         */
    float    james_rot_y;             /* rot.y as cached at 0x01F90704                             */
    int32_t  james_anim_status;       /* dword @0x01FBA8E8 (James upper/allbody status, 1..0x18)   */

    /* --- vcPreInfo (PC vcPreSetCharaDataForCamera 0x00521960) */
    float    hero_neck_wm[4][4];      /* 0x01F83860: neck bone world matrix (D3DX row layout)    */
    float    hero_head_ang_x;         /* 0x01F83920 (James +0x1DC)                                */
    float    hero_head_ang_y;         /* 0x01F83924 (James +0x1E0)                                */
    float    hero_eye_y;              /* 0x01F83928 (James eye_y, +0xA0): ABSOLUTE world y (Y down) */

    /* --- vcSetSubjChara() arguments (see PORTING_NOTES for how the PC derives them) */
    float    chara_pos[4];
    float    chara_bottom_y;
    float    chara_top_y;
    float    chara_grnd_y;
    float    chara_head_pos[4];
    float    chara_mv_spd;            /* world units per second (0 when standing)                  */
    float    chara_mv_ang_y;          /* atan2f(vel_z, vel_x): math convention, NOT yaw (PC 0x5219cd) */
    float    chara_ang_spd_y;         /* yaw rate, rad/s (James rot_spd.y, +0x50)                  */
    float    chara_eye_ang_y;
    float    chara_eye_ang_wy;
    float    chara_watch_xz_r;

    /* --- characters (may be NULL = none) */
    SH2CamChara *chara_slots;         /* array of SH2CAM_CHARA_SLOTS entries */
} SH2CamInput;

/* Game-state queries.  All optional; NULL means "return 0". */
typedef struct SH2CamCallbacks {
    void *user;
    /* vcRetPrioStgEvnt (PC 0x00522410): event-flag check per stage; non-zero DISABLES
       EFFECT/EVENT road types (priority 0).  Needs the game's event flags and room id. */
    int (*ret_prio_stg_evnt)(void *user, int glb_crd);
    /* PC 0x00521bb0 (stage 0x0D only): return non-zero when FUN_00495220() == 0xBB and
       event word 0x01F7D61C has bit 0x2000000 set and bit 0x8000000 clear. */
    int (*pc_special_room)(void *user);
    /* Called where the PC would hit an assert (infinite loop).  Execution then continues. */
    void (*on_assert)(void *user, int pc_line);
} SH2CamCallbacks;

/* What the renderer needs after each frame. */
typedef struct SH2CamOutput {
    float    cam_pos[4];          /* camera position (world)                                   */
    float    watch_tgt_pos[4];    /* look-at target (world)                                    */
    /* Camera-to-world matrix handed to vwSetViewInfoDirectMatrix, D3DX row-vector layout:
       row0 = right, row1 = local +Y (screen down, world Y is down), row2 = forward/look,
       row3 = position.  Includes the SELF_VIEW (first person) noise. View = inverse(this). */
    float    view_mat[4][4];
    float    cam_mat_ang[4];      /* x = pitch (+ looks up), y = yaw, z = roll                 */
    float    watch_tgt_ang_z;     /* roll target                                              */
    float    scr_z;               /* gCameraFOV (VbScreenInfo.scr_z): projection distance in
                                     screen units; vertical half FOV = atan((screen_sy/2)/scr_z) */
    float    scr_half_ang_wx;
    float    scr_half_ang_wy;
    int32_t  smooth_f;            /* vcExecCamera() result: 1 = continuous move, 0 = cut      */
    int32_t  cam_mv_type;         /* _VC_CAM_MV_TYPE used this frame                            */
    const _VC_ROAD_DATA *cur_road;
    uint32_t flags;
} SH2CamOutput;

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/* Converts the sentinel values of a freshly loaded .cam array in place (PC vcConvertCamFile,
   0x00521590).  Call once per array right after loading, before handing it to the camera. */
void sh2cam_convert_cam_file(_VC_ROAD_DATA *road_ary);

/* Full PC camera bring-up (PC 0x00518f70 = vcInitCamera): places the camera 795 units behind
   and 900 above the character (vcSetCameraUseWarp), then vcInitVCSystem + vcStartCameraSystem
   + vcSetProjectionValue(0,0) + VC_PROJ_MOMENT_CHANGE_F.  road_ary_list is a NULL-terminated
   list of .cam arrays (may be NULL: built-in "null road").  cb may be NULL. */
void sh2cam_init(_VC_ROAD_DATA **road_ary_list, const float chara_pos[3], float chara_rot_y,
                 int use_through_door_cam_f, const SH2CamCallbacks *cb);

/* Room change: the PC room loader writes vcWork.vc_road_ary_list directly (0x00512ed4). */
void sh2cam_set_road_list(_VC_ROAD_DATA **road_ary_list);

/* Enhanced Edition "PS2CameraSpeed" (default on): PS2 pitch speed and no vertical overshoot.
   Off keeps the PC constants. */
void sh2cam_set_ps2_speed(bool on);

/* One frame: vcSetSubjChara(in->chara_*) then vcExecCamera().  Returns smooth_f. */
int  sh2cam_exec(const SH2CamInput *in, SH2CamOutput *out);

/* Read-only access to the camera work (debugging). */
const _VC_WORK *sh2cam_work(void);

/* --- PS2-named game API (same behaviour as the PC functions) --- */
void vcInitVCSystem(_VC_ROAD_DATA **vc_road_ary_list);                                  /* 0x00519460 */
void vcStartCameraSystem(void);                                                         /* 0x00519510 */
void vcSetFirstCamWork(const float *cam_pos, float chara_eye_ang_y, int use_through_door_cam_f); /* 0x0051d030 */
void vcUserWatchTarget(const float *watch_tgt_pos, const _VC_WATCH_MV_PARAM *watch_prm_p,
                       float rot_z, int warp_watch_f);                                  /* 0x005195a0 */
void vcUserCamTarget(const float *cam_tgt_pos, const _VC_CAM_MV_PARAM *cam_prm_p, int warp_cam_f); /* 0x00519650 */
void vcGetNowWatchPos(float *watch_pos);                                                /* 0x005196f0 */
void vcGetNowCamPos(float *cam_pos);                                                    /* 0x005197b0 */
void vcReturnPreAutoCamWork(int warp_f);                                                /* 0x0051d170 */
void vcSetSubjChara(const float *chara_pos, float chara_bottom_y, float chara_top_y, float chara_grnd_y,
                    const float *chara_head_pos, float chara_mv_spd, float chara_mv_ang_y,
                    float chara_ang_spd_y, float chara_eye_ang_y, float chara_eye_ang_wy,
                    float chara_watch_xz_r);                                            /* 0x005197e0 */
void vcSetEventCamParamRefView(const float *vp, const _VC_CAM_MV_PARAM *cam_prm_p, const float *vr,
                               const _VC_WATCH_MV_PARAM *watch_prm_p, float rot_z, int warp_flg); /* 0x0051ced0 */
void vcSetProjectionValue(float new_proj, int framecnt);                                /* 0x0051cf00 */
void vcSetCamNoise(int32_t noise);                                                      /* 0x0051cc70 (PC only) */

#ifdef __cplusplus
}
#endif

#endif /* SH2CAM_H */
