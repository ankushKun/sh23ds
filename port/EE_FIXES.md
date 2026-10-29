# Enhanced Edition fixes in sh23ds

The Silent Hill 2: Enhanced Edition project (github.com/elishacloud/Silent-Hill-2-Enhancements,
zlib licence, by Elisha Riedlinger and contributors) patches the PC exe in memory to fix bugs of the
PC port and restore PS2 behaviour. sh23ds is a native port of the same PC code, so many of those
bugs live on in our C. This file lists every EE option (from `Common/Settings.ini` and
`Common/Settings.h`, including the hidden ones) and what the 3DS port does with it. Code that ports
EE logic says so in a comment with the option name and the EE file.

Status:

- **ported**: the fix is in the port, on by default as in EE.
- **already covered**: the port never had the bug (it does not run the PC code in question).
- **left out**: the fix would apply, but the feature it touches is not in the port yet, or it needs
  EE's own extra assets. The note says what is missing.
- **not applicable**: Windows, Direct3D, mouse, file system or launcher matters.

The tuning values in `Settings.h` that only feed one option (`fog_layer*`, `fog_transparency_layer*`,
the `water_*` values, the font sizes, `closet_replacement_model_*_time`) and the legacy or wrapper
keys (`fog_custom_on`, `EnableWndMode`, `FullscreenWndMode`, `WrapperType`, `CustomModFolder`) go
with the option they belong to below.

None of the ported fixes needs an option on the 3DS: they are always on. The two EE options a 3DS
player might want to toggle are listed in the controls section.

## Data and tools

`tools/sh2conv/lighting.py` applies the EE record fixes by default (`--pc` writes the plain PC
values, and the output format is unchanged). Regenerate the SD pack's `lighting.txt` with it to get
the fog, far clip and colour changes below. `tools/sh2conv/sh2map.py` now reads the 0x18-byte
vertex layout of material mode 3 (position, colour, uv); rooms ap26, ap88, ap108, hp230, ps53,
ps145, qt01 and rr49 should be converted again so their light fittings get their texture
coordinates.

## Every option

### Gameplay, stage scripts, enemies and Maria

| Option | EE file | Status | Where / why |
| --- | --- | --- | --- |
| FixTownGateEvents (messages) | Patches/Langs.cpp (PatchTownGateEvents) | ported | `events.c` `apply_ee_fixes()`: once flag 251 is set the PC calls the Wood Side gate (stage 5 event 11) and the Heaven's Night back alley gate (stage 14 event 10) jammed with a broken lock, although James walked through both. They now show stage messages 0 and 78 without the jam sound |
| FixTownGateEvents (lock model) | same, 0x596090 | ported | `stageparts.c` stage 13: part 0x2e takes variant 0 where the PC sets 1 (the byte EE changes at 0x596090), the gate's lock model |
| FixChainsawSpawn | Patches/PreventChainsawSpawn.cpp | ported | `prog.c` `prog_new_game`: the PC also set flag 12 (the chainsaw at the grave) on a first playthrough at beginner or easy action level (0x48b018); that branch is gone, so the chainsaw is a New Game+ reward |
| ChangeClosetSpawn | Patches/ChangeClosetSpawn.cpp | ported | `prog_apart.c` `s09_first_meet_trihead`: after the room 307 scene James stands inside the closet (z -101525, yaw 0.3) so the key catches the eye. EE's FOV reset is not needed because `gamecam` takes the FOV from the roads every frame |
| FireEscapeKeyFix | Patches/FireEscapeKey.cpp | ported (data) | `events.c`: clears the needs-light bit of the fire escape key (stage 9 event 6) and the key behind the bars (stage 8 event 0). `trigger_pass` tests the bit (an event with it needs James to see, `cond & 4`), so with the fix both keys can be taken without the flashlight, as in EE |
| Hotel map without the flashlight (always on) | dllmain.cpp | ported (data) | Same bit on the hotel map pickup (stage 40 event 5) |
| WoodsideRoom205Fix | WoodsideMannequinState.cpp, RotatingMannequinGlitch.cpp | already covered | The PC bug is stale state on a resident mannequin; the port spawns it fresh on every room entry and has no type-6 "lies in wait" state |
| FixCreatureVehicleSpawn | CreatureVehicleSpawn.cpp | already covered | The PC sets a stale bit in the wait-under-car state (0x4c5740); the port's `SCU_WAITCAR` never sets it |
| MariaSpawnFix | MariaSpawnFix.cpp | already covered | The PC reused Maria's saved position (0x934c00); `maria.c` places her from the appear table or at James' side |
| ClosetCutsceneFix | ClosetCutscene.cpp | left out | It darkens and blurs the closet bars through the PC's blur object (the port has no blur pass) and turns the flashlight off while the scene's position word (read at 0x4a005c) is -22256.6, which the port's scene player does not keep |
| ClosetCutsceneBonusAudio, ClosetRoomEnhanced, DogRoomEnhanced, MariaCutsceneReplacement, CockroachesReplacement, EddieLauraCutscene | the files of the same names | left out | Need EE's own sounds, models or extended scene data |
| FixFinalBossRoom | FinalBossRoomFix.cpp | already covered | The PC stops drawing the lower walkway, floor and black box above a height (-8000, -4000, -12000) and EE moves the cutoffs out of the way while James is below -14600; the port draws the whole room with no such cutoffs |
| HospitalChaseFix | HospitalChase.cpp | ported | `enemy.c` `oni_update`: past x 33185 in the otherworld hallway (room 0x5b) the chase's Pyramid Head goes to control 5 (0x4bb600, which does nothing), as EE writes to the first enemy slot, so its elevator stabbing animation is not steered |
| TeddyBearLookFix | TeddyBearLookFix.cpp | already covered | EE stops James looking at the locker room teddy bear (kind 0x727) once he has the bent needle; the port's look targets (`combat_look_target`) are enemies, Maria and items lying about, never stage models |
| PistonRoomFix | PistonRoom.cpp | left out | The labyrinth pistons are not animated |
| PuzzleAlignmentFixes | PuzzleAlignmentFixes.cpp | ported | `prog_overlay.c`: the gravestone boards (still and memo 54) use EE's matched texture rectangles and, turned, EE's corners; the coins in the room 105 cabinet use each coin's whole cell and one rectangle per hole |
| OldManCoinFix | OldManCoinFix.cpp | ported | `stagemdl.c` `read_list`: the Old Man Coin in the garbage chute (stage 13, kind 0x71f) needs flag 0x5f (the canned juice thrown down) to show, as its pickup already does |
| RowboatAnimationFix, RowboatSpawnFix (with the Great Knife boat speed) | RowboatAnimation.cpp, RowboatSpawn.cpp, RowboatFix.cpp | left out | The ride is ported now (`boat.c`, programs batch 18), but these fixes are not applied to it yet and need checking against it; the Great Knife part needs a weapon test `boat.c` does not have |
| PyramidHeadRotationFix (always on) | PyramidHeadRotationFix.cpp | already covered | The PC's `s44_trihead_dead` creates the two bodies with no rotation and EE gives them -1.75 and 2.9; the port places them from their Enemy_List rows (stage 44 ids 15 and 16, flag 483) with the rows' own facing |
| PrisonerTimer, WaterMoveSpeedFix | the files of the same names | left out | The prisoner footsteps (0x4ae0f0) and the in-water walk speed are not ported; port logic is time based, so the 60 fps parts will not be needed |
| HoldDamage | HoldDamage.cpp | already covered | `combat.c` `combat_hold_damage` already drains by the attack power times dt times 30, the scaling EE adds |
| GameLoadFix, QuickSaveTweaks, QuickSaveCancelFix, GameLoadFlashFix | SaveGame.cpp, QuickLoadGame.cpp, QuickSaveTweaks.cpp, QuickSaveCancelFix.cpp | not applicable | Quick save and Game Results files; the port saves only at save points and loads Maria's model on demand |
| SpeedrunMode | (Settings) | left out | Off by default in EE |
| IncreaseBlood | BloodSize.cpp | ported (pool size) | `blood.c` `POOL_KEY`: the third key's size (0x8956e4) is 1 instead of 0.8, so a dead enemy's pools grow to their original size. EE's position tweaks for room 205 and the flesh room edit the PC's room effect tables (0x958f64 and the code at 0x4ce133), which the port does not have |

### Audio

| Option | EE file | Status | Where / why |
| --- | --- | --- | --- |
| DoubleFootstepFix (always on) | DoubleFootstepFix.cpp | ported | `sfx.c` `sfx_footsteps`: the PC plays two steps when walk and run switch as a step fires (0x54af80), and the port did too; a step closer than 6 animation frames to the last is dropped |
| PreserveSoundsOnLoad | PlayWavSound.cpp, SfxPatch.cpp | ported | The PC plays 15002 when a game starts (0x498201, 0x4978c4) and cuts it off while loading; the port did not play it. `main.c` `title_start()` plays it during a one second title fade-out; `movie.c` `movie_play` stops it so it does not run on after the opening movie |
| MenuSoundsFix | MenuSounds.cpp | ported | `main.c` `pause_update`: changing a value (brightness, controls, run) plays the cursor sound; moves, confirm and cancel already had sounds |
| FixInventoryBGM | PatchInventoryBGM.cpp | already covered | The PC refreshes its music areas (0x5187e0) only in some menu states; `bgm_update` recomputes them every frame |
| FlashlightToggleSFX | PlayWavSound.cpp | already covered | X plays SE 18001/18000 from the game's own data (EE ships WAVs) |
| SpecificSoundLoopFix | SpecificSoundLoopFix.cpp | already covered | The PC's wrong branch at 0x54a5b2 left the chainsaw sound running and its moth sound restarted after the item screen; the port stops the engine sounds (11044, 11045) when the saw is lowered (`combat.c`) and gives the moths no sound loop |
| FixLyingFigureFootsteps | PlayWavSound.cpp | left out | Needs EE's `machi.wav`; enemies are frozen during scenes in the port, so the scene has no steps to fix |
| AudioClipDetection | Wrappers/dsound | left out | Relies on DirectSound's volume ramp; on ndsp it would need a per-frame fade in the DSP callback |
| ChainsawSoundFix | ChainsawSoundFix.cpp | already covered (hit) / left out (idle loop) | The hit sound plays once per enemy per swing in the port (`combat.c` `s_hit`), which is EE's first-frame-only rule. The rest fades in and trims the 48 kHz idle loop (0x1642) and the crank (0x14e8); the port's chainsaw has no idle loop, only the rev (11044) and the running sound (11045) |
| LabyrinthElevatorVolumeFix | LabyrinthElevatorVolumeFix.cpp | left out | The lift's stage code is ported (`prog_stagefn.c`) without its sounds (0x4655 / 0x4656); when they are added, use dt and drop the 0.8 volume factor |
| MusicBoxVolume | MusicBoxVolume.cpp | left out | Needs EE's re-authored `hotel/bgm_112.aix` |
| PlayUnusedAudio, VHSAudioFix | PlayUnusedAudio.cpp, VHSAudio.cpp | left out | Off by default in EE; VHSAudioFix needs EE's FMV pack |
| EnableMasterVolume | dsound | not applicable | The 3DS has a hardware volume slider |

### Combat, controls and camera

| Option | EE file | Status | Where / why |
| --- | --- | --- | --- |
| EnableHoldToStomp | HoldToStomp.cpp | ported, option | `combat.c` `start_stomp`: the PC chooses kick or stomp by button pressure (0x534210), so a digital button always stomps. A tap of A now kicks (anim 132) and a 0.2 s hold stomps (133). `holdstomp 0` in `options.txt` gives the PC behaviour |
| SwapLightHeavyAttack | SwapLightHeavyAttack.cpp | ported, option | `combat.c` `start_swing`: on the PC every first plank press is the heavy attack (0x533ff0 tests pressure above 0xc0). A tap is now the light swing (12), a hold the downswing (14). `swapattack 0` gives the PC behaviour |
| PS2CameraSpeed | WidescreenFixesPack | ported, option | `sh2cam.c`: the PC limits the watch pitch speed to 1.106 indoors and 1.555 outdoors (0x51bb81/0x51bb95) and the vertical approach to accel_y x abs(dy) (0x51c080), which makes the camera float and bounce. The port uses 4.0 and 3 x abs(dy) as WSFix does; `ps2camera 0` restores the PC |
| FastTransitions | WidescreenFixesPack | already covered | Forces 0.5 s fades (PC default 1.2 s at 0x4ee620); the port's fades are already 0.25 s out and 0.33 s in |
| EnableToggleSprint | InputTweaks.cpp | already covered | Options, Controls tab, Walk/Run Control: Toggle (`togglerun`) |
| RestoreSearchCamMovement | ControllerTweaks.cpp | left out | It puts the search view on the right stick; on the 3DS the C-Stick already carries the quick turn, strafing and the map (`controls.c`), so this is for the controls work to decide |
| DPadMovementFix, Southpaw, GamepadControlsFix, ReplaceButtonText, RestoreVibration, PadNumber, RemoveForceFeedbackFilter | ControllerTweaks.cpp, dinput8 | not applicable | PC gamepad mapping and rumble |

### HUD, menus, movies and scenes

| Option | EE file | Status | Where / why |
| --- | --- | --- | --- |
| LowHealthIndicatorStyle | LowHealthIndicator.cpp, PatchBinary.cpp | ported (style 1, the PC cross) | The port had no indicator. `combat.c` `combat_low_health_alpha` and `main.c` `draw_low_health`: below half health a red cross pulses at the bottom right with alpha (1 - hp/max) x a 0.1..1 pulse, as the PC draws it (0x476730/0x4f6240); three red quads stand in for the texture. Style 2 needs EE's `LowHealthFade.png` |
| DisableRedCrossInCutScenes | DisableRedCrossDuringCutscenes.cpp | ported | The cross is hidden in scenes, stills, the inventory, the pause menu and while a stage program holds James. DisableRedCross (EE default off) has no option |
| CutsceneUnequip | RemoveWeaponFromCutscene.cpp | ported | `combat.c` `combat_draw_weapon` draws no weapon while a scene plays |
| ReduceCutsceneFOV | WidescreenFixesPack | ported, option | `demo.c` `demo_camera`: the PC's scr_z (plane x 1.14702, 0x6b0684) scaled by aspect / (4/3) = 1.25 for the 5:3 screen, so scenes keep their 4:3 framing; `cutscenefov 0` turns it off |
| DelayedFadeIn (always on) | DelayedFadeIn.cpp | ported | `main.c` `fade_update`: the PC fade-in starts at -0.133 (0x4788f0), so the first frames after a room load stay black |
| DisableCutsceneBorders | WidescreenFixesPack | already covered | No letterbox in the port (EE's Auto hides it on any screen wider than 4:3) |
| FixFMVSpeed | FMVPatches.cpp | already covered | `movie.c` is clocked by the audio |
| MainMenuInstantLoadOptions, DisableLoadingPressReturnMessages | MainMenu.cpp, dllmain.cpp | already covered | The title lists every entry at once; no loading screens |
| RestoreBrightnessSelector | | already covered | Pause menu and D-pad, levels 1..7 |
| FmvSubtitlesSyncFix, FmvSubtitlesNoiseFix | FmvSubtitles.cpp | left out | The port draws no movie subtitles (if added, time them in the movie's 29.97 fps frames) |
| FixMemoFading, MemoScreenFix | MemoFix.cpp | left out | The PC fades only the bottom layer of a memo (0x498cbd). The port draws the memo stills and overlays now (`prog_overlay.c`), but how its memo screen fades has not been checked against this |
| FixElevatorCursorColor | ElevatorCursorColorFix.cpp | already covered | The hotel employee elevator's buttons are picked in a text panel (`prog_hotel.c` `s41_elevator`), so there is no PC cursor sprite whose colour could go wrong |
| FixDrawingTextLine | d3d8 wrapper | not applicable | Thickens a 1-pixel line in the hospital diary at high PC resolutions; the port shows memo text, not the PC page |
| FixHangOnEsc, PauseScreenFix, FixSaveBGImage, FixFMVResetIssue | HangOnEsc.cpp, PauseScreen.cpp, SaveBGImage.cpp, d3d8 wrapper | not applicable | Screen grabs and D3D texture pools; the port renders the game behind its pause and has a text save prompt |
| DisableShadowCutscene | DisableShadowCutscene.cpp | not applicable | The port draws no shadows in cutscenes |

### Fog, lighting and rendering

| Option | EE file | Status | Where / why |
| --- | --- | --- | --- |
| PS2FlashlightBrightness | Patches/EnhancedFlashlight.cpp | ported (environment part), option | `envlight.c` `ee_flashlight()`: the PC multiplies the flashlight colour by 4 before the intensity table (0x47a9ee), which washes walls out; EE uses 1.2, so the beam on the map is 0.3 of the PC value. The reach goes from 6000 to 8500 (0x47be64). The Options' Flashlight row (Display tab, `ps2flashlight` in `options.txt`) switches back to the PC beam. The "movable object" presets and the 0.7 in the same patch (0x4ff1c6, 0x502b96) pick the range of the PC's per-pixel flashlight shader on characters (x8) and scale the beam colour to 0.7 for it, which makes characters bright under the dimmer PS2 beam; the port's character flashlight (`chrlight.c`) saturates at full brightness already, so nothing more is needed (LIGHTING.md section 10 has the details; an earlier version of this page wrongly called these shadow presets) |
| RoomLightingFix | Patches/LightingPatch.cpp | ported | `lighting.py` `EE_RECORD_REMAP`: the cemetery (ca14) uses the forest's area default, as when walking in on the PC (the port reselects the record per room, which reproduces the PC's load-from-save bug). `envlight.c` `env_ee_room_level()` + `main.c` `lighting_frame`: fixed flashlight levels (0x8a2ba8) in town east/west, the courtyard, the lake shore, the motorhome and two Born From a Wish rooms. This needed the PC's own level logic first: `env_flash_off_level()` is FUN_0050a010 (1.0 before the flashlight is found, 0.3 after, per-map exceptions), and lit rooms now keep the last level as on the PC instead of a fixed 0.3. The swinging lamp of the Otherworld hotel (0x576fa0, y -3000, start -1000) is left out: the port has no event lights (FUN_0047b040) |
| FogParameterFix | Patches/FogAdjustments.cpp | ported | Blue Creek 208/209 (ap72) fog start 4200 instead of 3000 (`lighting.py` `EE_FOG_START`); final boss room characters drawn without fog while the camera is above y -15750 (`main.c` `ee_final_boss_nofog`, PC 0x501e76 / 0x503507) |
| FogSpeedFix | Patches/FogAdjustments.cpp | ported | The Blue Creek 209 fog value is the one above. On the particle layer (`fog.c` `ee_fog_speed`): the PC pushes fog out of dense grid cells by GridRate (0x1f7d28c, the value EE patches through 0x486ced); EE uses 0.25 in the town, the cemetery, the final rooms and most of the mansion and 0.5 in the courtyard and the Eddie rooms, and on the cemetery path it hides particles moving faster than 10 instead of 200 (0x6a7920) so the fog does not stick to James |
| FogFix | Patches/FogFix.cpp | ported | `fog.c`: the screen layer uses alpha 128 and a 0.25 x 0.125 texture window (0x487819..0x48784b), the sprites alpha 112 (0x488e1e), a 0.055 texture window (0x6a791c) and PartSize x 1.4 + 100 (0x489485) |
| ObservationDeckFogFix | Patches/ObservationDeckFogFix.cpp | ported (range) | `fog.c` `max_x`: on the observation deck (room 2) particles spawn, wrap and fade over x -24500..24500 instead of +-MaxPos. EE also raises the PC cap from 500 to 2000; the deck asks for 700, which the PC rule already turns into 450, and the 3DS draws at most 144 (see LIGHTING.md) |
| SmokeFogFix | Wrappers/d3d8/IDirect3DDevice8.cpp | left out | Turns fog on for smoke sprites; the port draws no smoke sprites |
| LightingFix | Patches/TreeLighting.cpp | ported (two of three parts) | Room 312 (rr91) clears to 255,255,227 (`lighting.py` `EE_CLEAR_COLOR`); the Maria ending letter scene (demo 0x5d) uses fog colour 125,135,150 (`main.c` `lighting_frame`). The 3D tree lighting of the Leave/Maria endings (0x4a0f35) is left out: no 3D trees in the port |
| LightingTransitionFix | Patches/LightingTransition.cpp | ported | `main.c` `lighting_frame`: no flashlight during the bowling alley scene (demo 0x19) and the Maria epilogue (0x5c). The record light changes (room 205, the hotel kitchen) go into `lighting.txt` and `chrlight.txt`, and the rooms where 0x50d334 turns the record spot into a point light are handled by `lighting.py` (`EE_SPOT_AS_POINT`) for `chrlight.c` |
| EnemyRevealLighting | Wrappers/d3d8/IDirect3DDevice8.cpp | ported | `demo.c` `char_light()` + `main.c` `demo_light_scale`: the Lying Figure in the tunnel radio scene (demo 6) is lit at 0.065/0.7 until timer 705.5. The room 205 mannequin specular part is already covered (the port has no specular) |
| HalogenLightFix | Wrappers/d3d8/IDirect3DDevice8.cpp | ported | `main.c` `material_uniforms`: mode-3 materials (PC VS2 0x8f2490, which passes a vertex colour these meshes do not have) are full bright, as EE's shader patch makes them. Rooms: ap26, ap88, ap108, hp230, ps53, ps145, qt01, rr49 |
| CatacombsMeatRoomFix | Patches/CatacombsMeatRoom.cpp | ported | `lighting.py` `EE_RECORD_PATCHES`: VertexAmbient of ps189 (2.5, 2, 2) and ps193 (3.15, 2.8, 2.8) and light 0 of ps193 |
| IncreaseDrawDistance | Patches/DrawDistance.cpp | ported (far clip) | `lighting.py` `EE_FAR_CLIP_DEFAULT`: rooms without their own far clip use 11000 instead of 7500. The water chunk draw (0x4e3181..) and the object distance factor (0x806468) are already covered: the port draws whole rooms and has no water chunks |
| Leave ending draw distance | Patches/LeaveEndingDrawDistance.cpp | left out | The ending stage sets fog, far clip and ambient from data while the Leave scene plays (0x5a05c0); the port's scenes use the room record. EE value: 40000 instead of 30000 |
| RemoveEnvironmentFlicker | Patches/FogAdjustments.cpp (PatchFMV) | left out | Moves the fog key frames of the Maria meeting scene (968 to 965.5, 1463 to 1460, stage code 0x594750) and the closet scene's despawn frames; the port does not run the scenes' stage fog callbacks yet |
| FlashlightFlickerFix | Patches/FlashlightFlicker.cpp | already covered | The PC re-applies the flashlight state (0x48c870) when the pause menu closes (0x401722), which flashes James; the port's pause menu does not touch the light |
| Room 312 flashlight (always on in EE) | Patches/Room312FlashlightFix.cpp | ported | `main.c` X button: the flashlight switch does nothing in room 312 while James has the videotape (item 0x17), as EE's patch of 0x48f078 |
| ShowerRoomFlashlightFix | Patches/ShowerRoomFlashlightFix.cpp | already covered | The PC's spot jumps on the last frame of the shower drain scene; the port lights scenes from James' game position throughout, so there is no jump |
| FixAptClockFlashlight | Patches/FlashlightClockPush.cpp | left out | Keeps the event camera bits (0x934d94) through the failed clock push so the flashlight is drawn right; the port's flashlight has no event camera state |
| SpecularFix | Patches/Specular.cpp, d3d8 wrapper | left out | No specular or environment map pass in the port |
| EnableSoftShadows | Wrappers/d3d8/IDirect3DDevice8.cpp | ported (opacity and fading) | `shadow.c` `shadow_ee_opacity()` and `main.c` `shadow_frame()`: the stencil shadows (LIGHTING.md section 11) darken by EE's per-room opacity (170 of 255 by default) instead of the PC's halving, and fade in over about 33 frames when the flashlight comes on. The blur and the self shadows on characters are left out |
| Room312ShadowFix, DisableShadowCutscene, BFaWAtticFix (shadow part) | Room312Shadow.cpp, DisableShadowCutscene.cpp, AtticShadowFixes.cpp | left out | They fix shadows of map objects and cutscenes; the port's stencil and drop shadows are for characters in play only |
| RestoreSpecialFX | Patches/SpecialFX.cpp | left out | Blur, bloom and the room 312 fog volume are post effects the port does not have |
| RemoveEffectsFlicker | d3d8 wrapper | left out | Motion blur first frame; no motion blur |
| HotelWaterFix, WaterEnhancedRender, WaterDrawOrderFix | HotelWater.cpp, WaterEnhancement.cpp, WaterDrawOrderFix.cpp | left out | The PC water renderer (0x4d....) is not ported |
| RainParticles | Patches/RainParticles.cpp | left out | No rain |
| FlashlightReflection | Patches/FlashlightReflection.cpp | left out | Needs EE's reflection assets and a reflection pass |
| PS2StyleNoiseFilter | PS2NoiseFilter.cpp | ported | `noise.c` `noise_set`: with the grain style on PS2 (the default; `optmenu.c` "ps2noise", Noise Effect's PS2 value) the grey byte is divided by 7.4, 0x22 replaces 0xff past the ramp's knee and the alpha base is 0x1d7, as EE patches 0x47cb8a, 0x47cbaf and 0x47834d |
| IncreaseNoiseEffectRes | PS2NoiseFilter.cpp | already covered | The port's noise texture is drawn one texel per pixel over the 400 x 240 screen |
| FmvSubtitlesNoiseFix | FmvSubtitles.cpp | left out | The port draws no movie subtitles |
| JamesTextureFix | Patches/JamesTexture.cpp | left out | Needs EE's replacement jacket texture 9980 |
| MothDrawOrderFix | Patches/MothDrawOrder.cpp | already covered | The PC sorts the moths into its translucent depth list with a wrong index; the port draws the final boss's moths as opaque models in its own pass (`enemy.c` `moths_draw`) |
| UseBestGraphics | (dllmain) | already covered | The port has no graphics options; everything it draws is always on |
| FixMissingWallChunks, WhiteShaderFix | d3d8 wrapper | not applicable | NVIDIA / Direct3D 9 driver issues |

### Display, platform and PC-only options

| Option | EE file | Status | Where / why |
| --- | --- | --- | --- |
| SetSixtyFPS, LimitPerFrameFPS | SixtyFPSPatch.cpp | already covered | The port runs at 60 fps with a measured frame time; its logic is time based |
| DynamicResolution, LockResolution, ResX/ResY, ScaleWindowedResolution, ScreenMode, ForceTopMost, WndModeBorder, DisableMaximizedWindowedMode, DisableHighDPIScaling, DisplayModeOption | Resolution.cpp, dllmain.cpp | not applicable | Windows display modes; the 3DS has one fixed top screen |
| WidescreenFix, Fix2D, FullscreenImages, FullscreenVideos, SetBlackPillarBoxes, Lake moon size | WidescreenFixesPack, FullscreenImages.cpp, FullscreenVideos.cpp, Resolution.cpp | not applicable | Widescreen and image scaling for PC monitors; the port draws stills and movies at their own aspect over black |
| d3d8to9, FrontBufferControl, ForceHybridEnumeration, Direct3DCreate9On12, SetSwapEffectUpgradeShim, HookDirect3D, AntiAliasing, AnisotropicFiltering, LegacyFixGPUAntiAliasing, EnableSMAA, CRTShader | Wrappers, dllmain.cpp | not applicable | Direct3D wrapper and PC post shaders |
| EnableTexAddrHack, UsePS2LowResTextures, TexPatch | TexPatch.cpp | not applicable | Larger replacement textures from EE's packs |
| EnableSFXAddrHack, EnableCriWareReimplementation, UseDSOAL, HookDirectSound, AudioFadeOutDelayMS | SfxPatch.cpp, PatchCriware.cpp | not applicable | PC audio engines; the port has its own ADX/AIX decoder and mixer |
| EnableEnhancedMouse, EnableMouseWheelSwap, EnhanceMouseCursor, AutoHideMouseCursor, CommandWindowMouseFix, CenterPuzzleCursor, MainMenuFix | InputTweaks.cpp, CommandWindowMouseFix.cpp, MainMenu.cpp | not applicable | Mouse support |
| UseCustomFonts, DisableEnlargedText, font sizes, LetterSpacing, SpaceSize | Fonts.cpp | not applicable | Windows font rendering of the PC text |
| MainMenuTitlePerLang, UseCustomExeStr, UnlockJapLang, EnableLangPath | Langs.cpp | not applicable | The port reads the DC's own five European languages as the PC does (Options, Language; `events/<f,g,i,s>/`); Japanese is not packed, as the PC's options never offer it |
| CustomAdvancedOptions, FixAdvancedOptions, LockScreenPosition, LockSpeakerConfig, UseBestGraphics | CustomAdvancedOptions.cpp, AdvancedOptionsFix.cpp, OptionsMenuTweaks.cpp | not applicable | The PC options menu; the port's options are in the pause menu |
| ImproveStorageSupport | 2TBHardDriveFix.cpp | not applicable | Windows free-space check |
| AutoUpdateModule, UseCustomModFolder, CheckForAdminAccess, CheckCompatibilityMode, CreateLocalFix, DisableSafeMode, DisableScreenSaver, NoCDPatch, SteamCrashFix, DisableGameUX, SingleCoreAffinityLegacy, LoadPlugins, LoadFromScriptsOnly, LoadD3d8FromScriptsFolder, LoadModulesFromMemory, DisableLogging, EnableDebugOverlay, EnableInfoOverlay, EnableScreenshots, HookWndProc, HookDirectInput, DelayedStart, ValidateBinary, ReplaceWindowTitle, LaunchAsAdmin | dllmain.cpp and the files of the same names | not applicable | Launcher, Windows and module housekeeping |
| PatchBinary (always on) | PatchBinary.cpp | not applicable | Text spacing of the PC control options menu and a DC call fix; the health cross animation reversal is listed with the HUD options |
| MapMeshToggle | MapMeshToggle.cpp | ported | `stageparts.c`: the motorhome hint before the apartments (stage 2), Blue Creek 1F after meeting Eddie (10), the hospital garden while James has the blue gem (22), the otherworld lobby in the third chase scene (23), the 3F ring door (25), the lake after the blue gem and in the Rebirth epilogue (38) and the room 312 hint (42). While the room 312 hint shows, EE also writes flag 0x2d6 into the low half of that room's event 19 flag word, and `events.c` `ev_set_flag_lo` does the same |
| MapTranscription (always on) | MapTranscription.cpp | already covered | 60 fps timing of the map marker animation; `map.c` is time based |

## Options for the 3DS player

Five EE options change how the game plays or looks rather than fixing a bug, so `options.txt`
keeps them (all on by default, as in EE): `holdstomp`, `swapattack`, `ps2camera`, `cutscenefov` and
`ps2flashlight`. Set one to 0 for the PC behaviour. All of them are in the Options as well: Camera
Speed and Heavy Attack (hold to stomp and the swapped swings in one row) on the Controls tab,
Flashlight and Cutscene View on the Display tab, where the top screen shows what each changes.

## Where EE logic lives in the code

Every place below has a comment naming the EE option and file:

- `source/envlight.c`: `ee_flashlight` (PS2FlashlightBrightness), `env_ee_room_level`
  (RoomLightingFix); `env_flash_off_level` is the PC's own FUN_0050a010 that the room levels need
- `source/main.c`: `lighting_frame` (LightingTransitionFix flashlight, LightingFix fog colour,
  room levels), `material_uniforms` (HalogenLightFix), `ee_final_boss_nofog` (FogParameterFix),
  `demo_light_scale` (EnemyRevealLighting), the X button (room 312 flashlight), `title_start`
  (PreserveSoundsOnLoad), `pause_update` (MenuSoundsFix), `fade_update` (DelayedFadeIn),
  `draw_low_health` (LowHealthIndicatorStyle, DisableRedCrossInCutScenes)
- `source/shadow.c`: EnableSoftShadows opacity table; `main.c` `shadow_frame`: its fading
- `source/chrlight.c`: draws the character lights that `lighting.py` writes with the EE record
  changes and the LightingTransitionFix spot to point rooms
- `source/fog.c`: FogFix, FogSpeedFix and ObservationDeckFogFix on the ported PC fog particle layer
- `source/demo.c`: `char_light` (EnemyRevealLighting), `demo_camera` (ReduceCutsceneFOV)
- `source/combat.c`: kick or stomp, light or heavy swing, weapon hidden in scenes, low health alpha
- `source/game/view/sh2cam.c`: PS2CameraSpeed
- `source/sfx.c`: DoubleFootstepFix
- `source/events.c`: FireEscapeKeyFix, hotel map, FixTownGateEvents
- `source/prog.c`: FixChainsawSpawn; `source/prog_apart.c`: ChangeClosetSpawn
- `tools/sh2conv/lighting.py`: IncreaseDrawDistance, RoomLightingFix, FogParameterFix, LightingFix,
  CatacombsMeatRoomFix and the character light values of PS2FlashlightBrightness and
  LightingTransitionFix

## How it was checked

All in Azahar (software renderer, New 3DS), before (current port, plain PC `lighting.txt`) and after
(this port, EE `lighting.txt`), same spawn and frame:

- the flashlight on the ap06 door: the beam is visibly softer
- the cemetery ca14: forest fog and grey instead of the clear far view of the cemetery record
- town east cb03: slightly darker ambient from the fixed level 0
- the prison ps145 light fitting (mode 3): drawn full bright with its texture after the sh2map.py fix
- the Eddie cold room ps189: VertexAmbient applied (less red in the vertex light)
- gameplay fixes: the Wood Side and Heaven's Night gate messages with flag 251, no chainsaw on a
  first game at easy action level, James inside the room 307 closet after the scene
- combat: tap and hold for plank swings and kick or stomp; the low health cross pulsing at hp 20
  and hidden with the inventory open; scene framing with and without `cutscenefov`
- `tools/lr3ds/smoke.sh` reaches every last frame on the merged build. The "continue" shot now
  catches the title still fading out, because starting a game waits one second for the start sound

The before/after pictures were kept in a test workspace that has since been removed.

## Not done yet, and why

The options still marked "left out" wait on one of these: features the PC renderer has and the port
does not (water, specular and environment maps, blur, bloom and motion blur, rain and smoke, 3D trees,
event lights outside cutscenes, map-object and cutscene shadows, movie subtitles), PC stage code that
is not ported (the scenes' fog callbacks, the Leave ending's data, the prisoner footsteps, the in-water
walk speed, the labyrinth pistons), fixes not yet applied to code that now exists (the rowboat fixes,
the memo fade), or EE's own assets. STATUS.md lists the same gaps in order of weight.
