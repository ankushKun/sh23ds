# SH2 camera (view module) porting notes

This directory holds a C99 translation of the Silent Hill 2 PC Director's Cut in-game camera, taken from the PC executable's view module (PC `0x00519430..0x00522410`) and named after the PS2 beta's DWARF (`view/vc_main.c`, `vc_calc.c`, `vc_newest.c`, `vw_calc.c`, `vb_main.c`).

| File | Purpose |
|---|---|
| `sh2cam.h` | Types (PS2 layouts, PC offsets), engine input/output structs, callbacks and the public API |
| `sh2cam.c` | The translation, one file, with each function tagged by its PC address |
| `PORTING_NOTES.md` | This document |

The module includes only `<stdint.h>`, `<stdbool.h>`, `<math.h>` and `<string.h>`, and it passes `clang -std=c99 -Wall -Wextra -fsyntax-only` with no warnings. It is also clean under `-pedantic` for an `armv6k-none-eabi` target, where the 32-bit layout asserts are active.

## How the translation was done

The Ghidra decompile was the starting point, but most functions in this module use LTCG register calling conventions, so the decompile often dropped arguments or showed garbage. I disassembled the matching executable (`Silent Hill 2/sh2pc.exe`, which matches the decompile byte for byte at `0x519430`) and checked every per-frame function against the instructions: argument registers, stack slots, comparison idioms (`test ah,41h / jp` and friends) and jump tables. All float constants were read from `.rdata`, including two the decompile hid (`4.0` at `0x006318A4` and `4096.0` at `0x006AEEF8`). No placeholder constants remain.

The D3DX math the module calls goes through the PSGP dispatch table at `0x007AF8E0`. I resolved the default implementations from the table at `0x007AF9C8`: slot 3 is `D3DXMatrixMultiply(out,a,b) = a*b`, slot 6 is `D3DXVec3TransformNormal`, slots 30/31/32 are `RotationX/Y/Z` and slot 37 is `Transpose`. The port keeps D3DX conventions exactly (row vectors, translation in row 3).

`FUN_00448470(a, b)` returns `atan2(b, a)` (fpatan with `b` in ST1). The port keeps the PC argument order through `shAtan2XY(a, b)` so each call can be checked against the decompile directly.

### The function names in the decompile are partly shifted

Several functions in `pc_view_decomp.c` carry real names that belong to a neighbouring function (they look like they were assigned by assert line ranges, and the PC source has more lines than the PS2 beta). `pc_names_aligned.json` is also off in a few places. I identified every function by behaviour and signature instead. The ones that differ from the labels:

| PC address | Label in decompile / alignment | Actual function (by behaviour) |
|---|---|---|
| `0x005195A0` | vcSetFirstCamWork (alignment) | vcUserWatchTarget |
| `0x00519650` | vcUserWatchTarget (alignment) | vcUserCamTarget |
| `0x005196F0` | vcUserCamTarget (alignment) | vcGetNowWatchPos |
| `0x005197E0` | vcGetNowWatchPos (alignment) | vcSetSubjChara |
| `0x00519AE0` | vcRetSelfViewEffectRate | vcRetFarWatchRate |
| `0x00519C40` | vcSetFlagsByCamMvType (alignment) | vcRetSelfViewEffectRate |
| `0x00519D70` | vcPreSetDataInVC_WORK | vcSetFlagsByCamMvType |
| `0x0051A150` | vcRetRoadUsePriority | correct |
| `0x0051B9F0` | vcFlipFromCamExclusionArea (alignment) | correct |
| `0x0051C230` | vcAdjCamOfsAngByCharaInScreen | vcMakeNewBaseCamAng |
| `0x0051C510` | vcAdjCamOfsAngByOfsAngSpd (alignment) | vcRenewalBaseCamAngAndAdjustOfsCamAng |
| `0x0051C670` | vcMakeCamMatAndCamAngByBaseAngAndOfsAng (alignment) | vcMakeOfsCam2CharaBottomAndTopAngByBaseMatT |
| `0x0051C710` | vcSetDataToVwSystem (alignment) | vcAdjCamOfsAngByCharaInScreen |
| `0x0051E280` | vcFlipFromCamExclusionArea | vcCamTgtMvVecIsFlipedFromCharaFront |
| `0x0051B5A0`, `0x0051EEA0`, `0x0051F660` | as labelled | correct |

## Per-frame pipeline

`sh2cam_exec()` copies the frame inputs, applies the PC driver's far-watch inhibit bit, calls `vcSetSubjChara()` and then `vcExecCamera()`, which runs:

1. `vcSetAllNpcDeadTimer` (enemy dead timers)
2. `vcPreSetDataInVC_WORK`: button state, screen half angles, through-door rail start, nearest enemy/item, and `vcSetNearRoadAryByCharaPos` (builds up to 128 candidate roads around James, with per-road world/local matrices)
3. `vcSetCurNearRoadInVC_WORK`: picks the current road (priority, hysteresis, direction test), starts projection blends and reports warps
4. Through-door end test, then the camera type for the frame
5. `vcRetFarWatchRate`, `vcRetSelfViewEffectRate`, `vcSetFlagsByCamMvType`, the PC-only special room check, and `vcGetUseWatchAndCamMvParam`
6. `vcAutoRenewalCamTgtPos`: ideal position per camera type (CHASE/SETTLE via `vcMakeIdealCamPosUseVC_ROAD_DATA`, FIX_ANG, LOCUS_CIRCLE, SELF_VIEW, THROUGH_DOOR), the move vector, the exclusion-area flip in front of James, the Y limits from `mv_y_type`, and `vcChangeProjByDist`
7. `vcRenewalCamData`: 60 Hz sub-stepped spring motion of `cam_pos` toward `cam_tgt_pos`
8. `vcChangeProjectionValue` (scr_z blend)
9. `vcAutoRenewalWatchTgtPosAndAngZ` (normal and far watch targets, first-person mix) and `vcAdjustWatchYLimitHighWhenFarView`
10. `vcRenewalCamMatAng`: base angle (SETTLE), offset angles with keep-James-in-frame correction and angular springs, the forced yaw limits, and the camera matrix
11. `vcSetDataToVwSystem` (first-person noise) and `vcEndProcessingVC_WORK`
12. `vcRetSmoothCamMvF` decides whether the move counts as smooth (no cut)

## Function table (all 100 PC functions)

Status: **ported** means translated with the PC behaviour, **replaced** means the job is done by the input/output structs, **stub** means a callback stands in for game systems, **skipped** means renderer or debug code outside the camera's job. Confidence reflects how well the asm pinned down the behaviour.

| PC address | Name used | Status | Conf. | Notes |
|---|---|---|---|---|
| `0x00519430` | vcRetZeroIfTiny | ported | high | PC helper (no PS2 name): \|x\| <= 5e-6 counts as 0 |
| `0x00519460` | vcInitVCSystem | ported | high | also clears cam_noise and the vcSelfViewTimer |
| `0x00519510` | vcStartCameraSystem | ported | high | |
| `0x005195A0` | vcUserWatchTarget | ported | high | |
| `0x00519650` | vcUserCamTarget | ported | high | |
| `0x005196F0` | vcGetNowWatchPos | ported | high | |
| `0x005197B0` | vcGetNowCamPos | ported | high | |
| `0x005197E0` | vcSetSubjChara | ported | high | called by `sh2cam_exec` from the input |
| `0x005198A0` | vcSetAllNpcDeadTimer | ported | high | writes `SH2CamChara.dead_timer` |
| `0x00519910` | vcRetSmoothCamMvF | ported | high | frame count clamped to [1,4] |
| `0x00519A20` | vcRetThroughDoorCamEndF | ported | high | |
| `0x00519AE0` | vcRetFarWatchRate | ported | high | FIX_ANG case may call vcWarpForFixAngCam |
| `0x00519C40` | vcRetSelfViewEffectRate | ported | high | |
| `0x00519D70` | vcSetFlagsByCamMvType | ported | high | |
| `0x00519EC0` | vcSetNearestEnemyDataInVC_WORK | ported | high | |
| `0x0051A060` | vcSetNearestItemDataInVC_WORK | ported | high | PC quirk: result written to the enemy fields |
| `0x0051A150` | vcRetRoadUsePriority | ported | high | EFFECT/EVENT use the prio callback |
| `0x0051A1D0` | vcAdvantageDistOfOldCurRoad | ported | high | |
| `0x0051A240` | vcMixSelfViewEffectToWatchTgtPos | ported | high | byte jump tables decoded; anim status semantics medium |
| `0x0051A670` | vcMakeFarWatchTgtPos | ported | high | |
| `0x0051AA10` | vcSetWatchTgtXzPos | ported | high | |
| `0x0051AAB0` | vcAdjustWatchYLimitHighWhenFarView | ported | high | |
| `0x0051ABC0` | vcRetMaxTgtMvXzLen | ported | high | |
| `0x0051AC80` | vcMakeIdealCamPosByHeadPos | ported | high | |
| `0x0051AD10` | vcMakeIdealCamPosForThroughDoorCam | ported | high | |
| `0x0051AE30` | vcMakeIdealCamPosForLocusCircleCam | ported | high | all quadrant branches checked against asm |
| `0x0051B3B0` | vcAdjustXzInLimAreaUsingMIN_IN_ROAD_DIST | ported | high | |
| `0x0051B4B0` | vcMakeBasicCamTgtMvVec | ported | high | |
| `0x0051B5A0` | vcAdjTgtMvVecYByCurNearRoad | ported | high | |
| `0x0051B9F0` | vcFlipFromCamExclusionArea | ported | high | |
| `0x0051BB50` | vcGetUseWatchAndCamMvParam | ported | high | |
| `0x0051BD50` | vcRenewalCamData | ported | high | PS2 vwRenewalXZVelocityToTargetPos etc. are inlined here on PC |
| `0x0051C230` | vcMakeNewBaseCamAng | ported | high | only SETTLE computes a base angle |
| `0x0051C510` | vcRenewalBaseCamAngAndAdjustOfsCamAng | ported | high | |
| `0x0051C610` | vcMakeOfsCamTgtAng | ported | high | |
| `0x0051C670` | vcMakeOfsCam2CharaBottomAndTopAngByBaseMatT | ported | high | |
| `0x0051C710` | vcAdjCamOfsAngByCharaInScreen | ported | high | PC assert on tiny half angle goes to the callback |
| `0x0051C850` | vcAdjCamOfsAngByOfsAngSpd | ported | high | PC quirk: yaw speed clamped by max_ang_spd_x |
| `0x0051CC70` | vcSetCamNoise | ported | high | PC only (name invented): int shake multiplier at +0x30 |
| `0x0051CC80` | vcCamMatNoise | ported | high | |
| `0x0051CCE0` | vcGetXZSumDistFromLimArea | ported | high | |
| `0x0051CED0` | vcSetEventCamParamRefView | ported | high | |
| `0x0051CF00` | vcSetProjectionValue | ported | high | |
| `0x0051CF90` | vcChangeProjectionValue | ported | high | the call to FUN_0051ff00 (copy of the engine projection matrix) is left to the renderer |
| `0x0051D030` | vcSetFirstCamWork | ported | high | |
| `0x0051D170` | vcReturnPreAutoCamWork | ported | high | |
| `0x0051D220` | vcSetNearRoadAryByCharaPos | ported | high | PC quirks kept (see below) |
| `0x0051D850` | vcGetNearestNEAR_ROAD_DATA | ported | high | |
| `0x0051D9E0` | vcMakeNormalWatchTgtPos | ported | high | vcSetWatchTgtYParam is inlined |
| `0x0051DC10` | vcMakeIdealCamPosForFixAngCam | ported | high | side effect on chara_top_y kept |
| `0x0051DE90` | vcMakeIdealCamPosUseVC_ROAD_DATA | ported | high | |
| `0x0051E280` | vcCamTgtMvVecIsFlipedFromCharaFront | ported | high | |
| `0x0051E570` | vcMakeCamMatAndCamAngByBaseAngAndOfsAng | ported | high | includes the PC-only shake |
| `0x0051E680` | vcSetDataToVwSystem | ported | high | writes `SH2CamOutput.view_mat` |
| `0x0051E820` | (vwInitViewInfo helper) | ported | high | inlined in `sh2cam_init` |
| `0x0051E880` | vcPreSetDataInVC_WORK | ported | high | vcSetTHROUGH_DOOR_CAM_PARAM_in_VC_WORK is inlined |
| `0x0051EA50` | vcGetBestNewCurNearRoad | ported | high | order EVENT, ROAD, EFFECT, PRIO_HIGH, PRIO_LOW, SV_ONLY |
| `0x0051ED10` | vcAutoRenewalWatchTgtPosAndAngZ | ported | high | |
| `0x0051EEA0` | vcAutoRenewalCamTgtPos | ported | high | PC passes a 6th arg (far_watch_rate) that is unused |
| `0x0051F110` | vcRenewalCamMatAng | ported | high | |
| `0x0051F300` | vcSetCurNearRoadInVC_WORK | ported | high | |
| `0x0051F660` | vcExecCamera | ported | high | |
| `0x0051F950` | vwGetViewPosition | skipped | n/a | use `view_mat[3]` or `cam_pos` |
| `0x0051F980` | vwGetViewAngle | skipped | n/a | use `cam_mat_ang` |
| `0x0051F9B0` | vwSetCoordRefAndEntou | skipped | n/a | debug/free camera |
| `0x0051FAA0` | vwSetViewInfo | skipped | n/a | renderer side (builds the view from the camera matrix) |
| `0x0051FAF0` | vwSetViewInfoDirectMatrix | replaced | high | becomes `SH2CamOutput.view_mat` |
| `0x0051FB20` | vwInitViewInfo | replaced | high | only the camera matrix seed matters, done in `sh2cam_init` |
| `0x0051FE50` | (vbSetRefView helper) | skipped | n/a | renderer |
| `0x0051FF00` | (copy projection matrix) | skipped | n/a | renderer |
| `0x0051FF20` | vbInitCoordinate | skipped | n/a | renderer |
| `0x0051FFF0` | vbGetLw | skipped | n/a | renderer |
| `0x00520150` | vbNormalizeRadianAngle | skipped | n/a | not used by the camera |
| `0x005201C0` | vbTransposeMatrixWithoutTr | ported | high | |
| `0x00520220` | vbApplyMatrixWithoutTr | ported | high | |
| `0x00520240` | vbSetRefView | skipped | n/a | renderer |
| `0x00520940` | vwLimitOverLimVector | ported | high | |
| `0x005209A0` | vwDecreaseSideOfVector | ported | high | |
| `0x00520E10` | vwAngleToVector | ported | high | |
| `0x00520E60` | vwVectorToAngle | ported | high | |
| `0x00520EE0` | vwOresenHokan | ported | high | integer math as on PC |
| `0x00520F70` | vwRotMatrixYXZ | ported | high | mat = RotZ * RotX * RotY * mat |
| `0x00521040` | vwMatrixToAngleYXZ | ported | high | |
| `0x00521130` | vwRet3DLength | ported | high | |
| `0x005214F0` | vcTransRotRoadArea | ported | high | |
| `0x00521530` | vcRotTransRoadArea | ported | high | |
| `0x00521570` | vcGetMinInRoadDist | ported | high | |
| `0x00521590` | vcConvertCamFile | ported | high | public as `sh2cam_convert_cam_file` |
| `0x00521680` | vcRetNearRatioSwitchAreaInXZPos | ported | high | PC passes cur_near_road by value |
| `0x00521810` | vcRetNearRatioSwitchAreaForCircleCam | ported | high | computes a transformed point it never uses |
| `0x00521960` | vcPreSetCharaDataForCamera | replaced | high | its outputs are fields of `SH2CamInput` |
| `0x00521A70` | vcPcSpecialRoomWatchY | ported | high | PC only (name invented) |
| `0x00521BB0` | vcPcSpecialRoom | ported | medium | the room/event condition comes from a callback |
| `0x00521C20` | vcEndProcessingVC_WORK | ported | high | |
| `0x00521C40` | vcEndProcessingOldNearRoad | ported | high | |
| `0x00521C60` | vcCorrectCamMatAngForcibly | ported | high | PC passes the road record by value |
| `0x00521D70` | vcWarpForFixAngCam | ported | high | |
| `0x00521EB0` | vcRetCirRadiusReduction | ported | high | |
| `0x00521FF0` | vcChangeProjByDist | ported | high | writes `road->projection` like the PC |
| `0x00522410` | vcRetPrioStgEvnt | stub | n/a | needs event flags and the room id; callback |

**Counts:** 86 ported, 4 replaced or stubbed (`0x0051FAF0`, `0x0051FB20`, `0x00521960`, `0x00522410`), 10 skipped.

One extra function sits in a gap between listed ones (`0x0051CE10`): an event helper that sets a user camera position plus an angle-derived watch target. It is not called from the per-frame path and is not ported; `vcSetEventCamParamRefView` covers the same need.

## Globals mapped

| PC address | Port location | Notes |
|---|---|---|
| `0x01F83DE0` | `g_vc.work` (`_VC_WORK`) | vcWork; field offsets in `sh2cam.h` match the PC exactly with 32-bit pointers (asserted) |
| `0x01F83E10` | `_VC_WORK.cam_noise` | PC-only int at +0x30; every later field is +4 from the PS2 layout |
| `0x01F83E40` | `_VC_WORK.cam_pos[1]` | gInGameCameraPosY |
| `0x01F8E3B8` | `_VC_WORK.cur_near_road` | +0xA5D8 |
| `0x01F906A8` | `g_vc.self_view_timer` | vcSelfViewTimer (noise phase) |
| `0x01F906B0..BC` | `g_vc.proj` | vcProjectionParam: new, old, delta, flg |
| `0x01F906C0` | `g_vc.watch_mv_prm_st` | vcWatchMvPrmSt |
| `0x01F90730` | `g_vc.cam_mv_prm_st` | vcCamMvPrmSt |
| `0x01F83BC0` | `g_vc.scr_z` | gCameraFOV = VbScreenInfo.scr_z |
| `0x01F83BC4/C8` | `g_vc.scr_sx/scr_sy` | VbScreenInfo.sx/sy, from the input |
| `0x01F83810/14` | `g_vc.pc_special_cam_h/watch_y` | PC-only special room heights |
| `0x01F83C2C` | `g_vc.view_mat` | vwViewPointInfo coord set by vwSetViewInfoDirectMatrix |
| `0x01F83860` | `SH2CamInput.hero_neck_wm` | vcPreInfo.hero_neck_wm |
| `0x01F83920/24/28` | `SH2CamInput.hero_head_ang_x/y, hero_eye_y` | vcPreInfo |
| `0x01F90704` | `SH2CamInput.james_rot_y` | James rot.y cached by vcPreSetCharaDataForCamera |
| `0x00935948/54/58` | `g_vc.frame_time/frame_count/frame_rate` | derived from `frame_ticks60` |
| `0x01F7D550 (+0x24)` | `SH2CamInput.stage_glb_crd` | stage->glb_crd |
| `0x01FBA880` (+0x04, +0x148, +0x1C) | `james_status`, `james_battle_status`, `james_pos_x` | player SubCharacter |
| `0x01FBA8E8` | `SH2CamInput.james_anim_status` | |
| `0x01FBACC4`, `0x01FBAD16` | `SH2CamChara.is_lock_target` | lock-on target and its flag byte |
| `0x01DBEB80` | `SH2CamInput.area_code` | only compared with 0x35 |
| `0x01DBEC08` | `SH2CamInput.control_type` | gControlType |
| `0x01D823C4`, `0x01F991A0` | `chara_slots_disabled`, `chara_slots` | character slot table (20 x 12 bytes on PC) |
| `0x01F7D5E4..0x01F7D638` | callbacks | event flag words used by vcRetPrioStgEvnt and the special room |
| `0x006C84A8..0x006C85B8`, `0x006CB480` | static tables in `sh2cam.c` | watch/cam move params and per-area distance tables |
| `0x008BC9E8`, `0x008BCA0C` | `excl_r_ary`, `mv_stl_ang_ary` | vwOresenHokan tables |
| `0x008BCA20`, `0x008BCB20`, `0x008BCB28` | `vcNullRoadArray`, `vcNullRoadArrayList`, `vcNullNearRoad` | the built-in "infinite" WIDE chase road |

## How to call it

```c
#include "game/view/sh2cam.h"

/* After loading a room's .cam file(s) */
sh2cam_convert_cam_file(cam_ary);              /* once per array */
static _VC_ROAD_DATA *cam_list[] = { cam_ary, NULL };

/* First frame in the game, or after a teleport */
SH2CamCallbacks cb = { 0 };                    /* all optional */
sh2cam_init(cam_list, james_pos, james_rot_y, /*through door*/ 0, &cb);

/* Room change without a reset (the PC room loader does exactly this) */
sh2cam_set_road_list(cam_list);

/* Every frame */
SH2CamInput in;   /* fill as described below */
SH2CamOutput out;
sh2cam_exec(&in, &out);
```

To build a view matrix from the result, take `out.view_mat`: row 0 is right, row 1 is local +Y (screen down, since world Y points down), row 2 is the look direction and row 3 is the eye position. With a look-at helper that is `eye = row3`, `target = row3 + row2`, `up = -row1`. Use the matrix instead of `watch_tgt_pos`, because the matrix includes the keep-James-in-frame correction, the angular smoothing and the first-person noise.

`out.scr_z` is the projection-plane distance in the same pixel units as `screen_sy`. A vertical field of view is `2 * atan((screen_sy / 2) / scr_z)`; with the PS2 frame of 640x448 and the default 448 that is about 53 degrees. Road data changes `scr_z` (blended over `proj_sec`), so read it every frame.

### What to put in `SH2CamInput` each frame

| Field | What the PC uses |
|---|---|
| `frame_ticks60` | elapsed 60 Hz ticks (2.0 at 30 fps, 0 when paused); the PC derives gFrametime = x/60, the int count and 60/x |
| `screen_sx`, `screen_sy` | VbScreenInfo.sx/sy; see the open questions, 640 and 448 reproduce PS2 framing |
| `stage_glb_crd` | stage number; 1..4 and 0x11 are the outdoor town stages, 5 and 2 also have special cases |
| `area_code` | byte at `0x01DBEB80`, only tested against 0x35 |
| `control_type` | gControlType; 1 turns off the chase side offset and changes far view |
| `transition_state` | gTransitionState; 0 and 2 count as idle |
| `far_view_button` | the look button (FUN_005342b0 returns 0 while James status has 0x6000 bits) |
| `inhibit_far_watch` | the PC driver sets this when `DAT_01F90768 == 0` |
| `james_status`, `james_battle_status`, `james_pos_x`, `james_rot_y`, `james_anim_status` | see the header comments |
| `hero_neck_wm`, `hero_head_ang_x/y`, `hero_eye_y` | vcPreInfo: neck bone world matrix, head angles (James +0x1DC/+0x1E0), eye height |
| `chara_*` | the vcSetSubjChara arguments, described below |
| `chara_slots` | 20 character slots in game order, empty ones with `kind = 0` |

The PC driver (`0x00518FF0`, the vcMoveAndSetCamera equivalent) fills the vcSetSubjChara arguments like this, so the port should do the same:

- `chara_pos` = James position (x, y, z, w).
- `chara_bottom_y` = `pos.y - k * 250`, where `k = DAT_01F90768` (1.0 normally; 0.0 also sets `inhibit_far_watch`).
- `chara_top_y` = `pos.y - 937.5` when `area_code < 0x35`, otherwise `pos.y - 812.5`.
- `chara_grnd_y` = `pos.y`.
- `chara_head_pos` = neck matrix translation plus `(0, -50, 0)` rotated by the neck matrix, then `y -= 150` (vcMakeHeroHeadPos `0x00518D60`).
- `chara_mv_spd` = `(60 / frame_ticks60) * sqrt(spd.x^2 + spd.z^2) * 500`, with `spd` the SubCharacter fields at +0x3C and +0x44.
- `chara_mv_ang_y` = `atan2(James+0x44, James+0x3C)` (that argument order is what the PC does).
- `chara_ang_spd_y` = James rot_spd.y (+0x50).
- `chara_eye_ang_y` = James rot.y, `chara_eye_ang_wy` = 2.0943952 (constant), `chara_watch_xz_r` = 5500 (constant).

### Collision and line of sight

The PC camera makes no collision or line-of-sight queries at all. Every constraint comes from the road and switch volumes in the .cam data, so there are no collision callbacks to implement. The callbacks that exist cover game-state questions:

- `ret_prio_stg_evnt(user, glb_crd)`: return non-zero to disable EFFECT and EVENT roads (priority 0). The real function reads story event flags and the room id per stage. Returning 0 keeps those roads at priorities 5 and 6, which matches the PC for every stage it does not special-case.
- `pc_special_room(user)`: only consulted on stage 0x0D. Return non-zero when the room id (FUN_00495220) is 0xBB and event word `0x01F7D61C` has bit 0x2000000 set and bit 0x8000000 clear.
- `on_assert(user, line)`: called where the PC would print an assert and hang. The port carries on.

## PC behaviour kept on purpose

These look like bugs or oddities, but they are what the PC does, so the port keeps them:

1. `vcSetNearestItemDataInVC_WORK` writes the found item into `nearest_enemy_p` and `nearest_enemy_xz_dist`. Only its early-out writes the item fields, so `nearest_item_p` effectively stays NULL. With no enemy and no item, the enemy distance ends up 5000 instead of 7500.
2. In `vcSetNearRoadAryByCharaPos` the character is transformed with the switch-area matrix only and that point is tested against both the switch and road bounds. The vertical distance above the ceiling uses `sw.min_hy` instead of `sw.max_hy`.
3. The road-local matrices translate by `+P1` and the inverse by `-P1`. That is self-consistent (local coordinates are just offset), so nothing changes, but it looks wrong at first glance.
4. `vcAdjCamOfsAngByOfsAngSpd` clamps the yaw speed with `max_ang_spd_x`.
5. `vcMakeIdealCamPosForFixAngCam` adds the road's `fix.ofs_hy` to `chara_top_y` for the rest of the frame.
6. `vcChangeProjByDist` writes the computed projection back into the road record, so .cam arrays must be writable.
7. In first person with anim status 6 or 7, the PC truncates James's yaw to an int and shifts it by 7, which is 0 for most angles. I reproduced the arithmetic shift exactly.
8. `vcGetBestNewCurNearRoad` and the hysteresis in `vcSetCurNearRoadInVC_WORK` follow the PC comparison idioms exactly, including `<=` versus `<` at the boundaries.

## Open questions and uncertainties

- **Screen size inputs.** The PC fills VbScreenInfo.sx/sy from the render size (`0x00A35498/9C`), while the road `projection` values are in PS2 pixel units (448). These feed the keep-James-in-frame half angles and the far-view Y limit. I recommend 640 and 448, which reproduce the PS2 framing; real PC values at other resolutions would change how tightly James is kept in frame.
- **`james_anim_status`** (dword at `0x01FBA8E8`). The case values (1, 3, 6, 7, 9, 0x0C..0x0E, 0x18) match `_JAMES_UPPER_STATUS` (RELAX, TIRED, LROUND, RROUND, WALK, RUN1..3, FALL), but I did not verify the PC `shPlayerWork` layout, so the exact source field is a best guess.
- **SubCharacter `battle.status` bits.** The camera uses 0x02 (dead, with the dead timer), 0x04 (ignored unless it is the lock target) and 0x20 (always ignored) on enemies, and 0x10/0x20 on James. The names are my reading of how they are used.
- **`__ftol`.** It lives in the protected `.cms` section, so I could not read it. I assumed MSVC truncation toward zero; it only affects the projection frame count, the smooth-move frame count and the two interpolation table lookups.
- **x87 precision.** The PC keeps intermediates in 80-bit registers. The port uses float (and double only inside the angle wrap), so results can differ in the last bits.
- **Uninitialised PC locals.** A few PC stack values are read without being set (the ideal position when the through-door camera is inactive, `ofs_tgt_ang[3]`). The port uses zero.
- **Near-road overflow.** The PC writes a 129th entry before its assert fires; the port stops at 128 and calls `on_assert`.
- **Pause.** With `frame_ticks60 = 0` the PC computes `0/0` and `1/0` in a couple of places whose results are never used (or only stored as target velocity). The port does the same arithmetic.
- **Event priority and the special room.** Both depend on systems the port does not have yet, so they are callbacks for now.

## Verification done

- Host build with `-std=c99 -Wall -Wextra`: no warnings. ARM 32-bit syntax check with `-pedantic`: clean, with the PC offset asserts active (a deliberately wrong offset makes it fail).
- A scratch harness (not in the repo) loaded all 375 `.cam` files from the game data (1232 road records), ran each road for 240 frames with James standing, walking, turning and using far view, and found no NaN or runaway values. The mean camera distance was about 1800 units and the maximum under 10000.
- On a sample outdoor chase road the camera settled 1250 units from James (the OUTDOOR radius) and 1125 units above the floor (top_y - 187.5), offset by the 0.209 rad side angle the PC uses at rest, and the view matrix's forward row pointed at the watch target. Through-door, first person, pause frames and the event camera API (set and return with warp) all stayed finite and behaved as the PC code describes.
