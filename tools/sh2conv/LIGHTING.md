# Silent Hill 2 PC per-room lighting and fog

This note documents what `lighting.py` extracts and how the PC Director's Cut (`sh2pc.exe`,
image base 0x400000) turns it into pixels. Everything comes from static analysis of the exe,
checked against the PS2 beta DWARF headers (`GFW/sh2_DrawEnvData.c`, `main.c`,
`GFW/sh2gfw_LightSet.c`) and the Enhanced Edition patches that touch the same globals.

```
.venv/bin/python -I tools/sh2conv/lighting.py      # writes pack/lighting.json + pack/lighting.txt
.venv/bin/python -I tools/sh2conv/lighting.py --pc # the same without the Enhanced Edition fixes
```

The short version: lighting and fog are not stored in the `.map` files. Each bg map has a static
496-byte record in the exe (PS2 name `DrawEnvData`), picked when the player arrives in a room.
The record feeds a small set of vertex shader constants, and the map is lit per vertex by
three directional lights plus a few ambient terms. In the "dark" rooms (the apartments, the
hospital, the prison, the hotel and so on) the only real light source is the flashlight, so
those rooms are almost black in the PC game when the flashlight is off.

## 1. Where the data lives

### The record table

`0x8063c8` holds 17 pointers, one per `glb_crd` (the world coordinate set of a stage, see
`sh2rooms.py`). Each points to an array of 0x1f0-byte records that ends with a record whose
`map_id` is -1; that terminator is a full record too and serves as the area default. glb 14 and
15 share one array. There are 129 records in total.

The record id is `glb << 16 | map number`, where the map number is the number in the bg file
name (`ap38.map` in glb 9 is 0x90026). The game derives it in `FUN_00494ef0`: indoors it takes
byte 0 of the `u8[4]` entry for the room id at `0x88e210` (room id from `FUN_00494db0`, the
function `sh2rooms.py` transcribes); outdoors (glb 1..4 and 17) it binary-searches tile tables
at 0x88e568 / 0x88e5c8 / 0x88e6a8 / 0x88e7d8 / 0x88e7f0 whose values are the `ca`/`cb`/`cc`/`cd`
tile numbers. In both cases the number matches the `.map` file name, which is what the converter
relies on.

| glb | area | notes |
|---|---|---|
| 1 | ca (forest) | record 0x1000e (ca14) and a default |
| 2 | cb (town east) | whole-area records, see selection below |
| 3 | cc (town west) | whole-area records; 0x30031/0x30032 exist but are never selected |
| 4 | cd (lake) | default only |
| 5 | ob | default only |
| 6 | er | records matched on id and on the dark byte |
| 7, 8 | bw, th | |
| 9 | ap | 17 records + default |
| 10..13 | hp, ps, rr, ru | |
| 14, 15 | qp, qt | glb from the room table (end_dog stage), medium confidence |
| 16 | ma (bg2) | Born From a Wish only |

### Record layout (PC, 0x1f0 bytes)

Field names in the second column are the PS2 `DrawEnvData` names; the PC layout matches the PS2
struct exactly (offsets confirmed by `FUN_0047be40` and the getters at 0x47baa0..0x47bbf0).

| offset | PS2 name | PC meaning |
|---|---|---|
| 0x00 | map_id_name | `glb << 16 \| map`, -1 = area default / end, -2 = town day record |
| 0x04 | parallelLNum | which parallel lights reach the map: 0 = all three, 1 = lights 0 and 1, >=2 = light 0 only (the others are multiplied by 0.0 at 0x47bf8f) |
| 0x05 | FakeSpotNum | count of fake spot lights in the Ld block |
| 0x06 | FakePointNum | count of fake point lights |
| 0x07 | pointLNum | count of point lights |
| 0x08 | spotLNum | count of spot lights, -1 for none; also read by the flashlight code (`FUN_0047dea0`) |
| 0x09 | NightOrDay | **dark room flag**. `FUN_0047b200` sets `gFlashlightAvailable` (0x9457f0, sh2ee name) to 1 + (flag != 0). Nonzero means the flashlight replaces light 0 and the dark fog defaults apply |
| 0x0a | ShadowColor (s16) | character shadow density into 0x9458ac; -1 = 80 (40 in Born From a Wish) |
| 0x0c | ShadowForceLightNo (u16) | low byte = light index, high byte 1 = parallel light, 4 = flag tested by `FUN_0047bb90` |
| 0x0e | ShadowDarkNess (s16) | `FUN_0047bb00` |
| 0x10 + 0x20*k | pLight_Dir k | direction the light travels (Y down, so (0,1,0) shines downward), normalised on load |
| 0x20 + 0x20*k | pLight_Col k | colour, scale 1.0 = 4x the shader value |
| 0x70 | SpotCol0 | flashlight colour (7,7,7 in every record) |
| 0x80 | SpotDecayParm0 | flashlight start (1500), slot 1 replaced by cos(slot 3), end (6000), cone half angle (0.785) |
| 0x90 | SpotParm | [0] unknown (0.0714 / 0.0429), [1] far clip (0 = 7500), [2] near clip (0 = 100), [3] fog colour scale |
| 0xa0 | FogParm | [0] fog end, [1] fog start, [2] fog factor at the end, [3] fog factor at the start (0..255). [0] == 0 means use the defaults |
| 0xb0..0x19f | Ld_0..Ld_E | point lights {Col, Decay(start,end), Pos} x pointLNum, then spots {Col, Decay, Pos, Dir}, then fake points, then fake spots. Used for characters, not for the map |
| 0x1a0 | MoveFogCol | RGBA 0..255: base of the fog colour and colour of the fog particle layer (`FUN_004879c0`) |
| 0x1b0 | Ambient | ambient for lit-material geometry (map modes 1/2/6) |
| 0x1c0 | BaseAmbient | ambient for vertex-coloured geometry (mode 4) |
| 0x1d0 | BaseColor | gain for vertex-coloured geometry (22.4 in almost every record) |
| 0x1e0 | VertexAmbient | gain for the vertex colours (4.0 in almost every record) |

### Selection (`FUN_0047c780`, PS2 `Set_DrawEnvData`)

Called from the stage connect code (0x401480 -> 0x476380) with the arrival position
(0x1dbc210/0x1dbc218), and again by the flashlight code when it changes mode. Walking between
rooms of one stage without a door does not reselect.

* glb 2 and 3 (town): Born From a Wish, or game flag 251 clear (bit 27 of `gGameFlag` word 7,
  0x1f7d5fc), takes the first record with id -2 (day). Flag 251 set takes the -1 record (dark
  night version). The map id is ignored.
* glb 6 (er): the first record with the map id and `dark == flag251`, else the default.
* everything else: the first record with the map id, else the -1 default.

### Applying it (`FUN_0047be40`)

The record is copied into frame globals. Where a field is zero the game substitutes:

| | lit room (dark byte 0) | dark room |
|---|---|---|
| fog end / start | 5500 / 3000 | 7500 / 3000 |
| fog factor at end / start | 20 / 235 | 0 / 255 |
| fog colour (SpotParm[3] == 0) | 70,70,70 | 0,0,0 |
| clear colour | 70,70,70 | 0,0,0 |

With SpotParm[3] != 0 the fog colour is `trunc(SpotParm[3] * MoveFogCol)` and the clear colour
(0x945850, sh2ee `WorldColor`, used by `FUN_00476d50` for `Clear`) is the same RGB. Far clip
defaults to 7500 and near clip to 100. Born From a Wish (`gChapterID` == 1, 0x1dbec0c) changes a
few things: in glb 9 the ambient becomes 0.4 grey and the base ambient 0.3 / 0.4 / 0.5 grey
(per room, switch at 0x47c1c4), in other glbs except 3, 8 and 16 the ambient keeps the previous
room's value, and the clear colour of the rooms listed in `FUN_00476870` becomes 70 or 90 grey.

Globals written (all confirmed by xrefs): 0x9455f0/600/610 light directions, 0x945620/630/640
light colours, 0x945650 Ambient, 0x945670 BaseAmbient, 0x945680 BaseColor, 0x945660
VertexAmbient, 0x9456b0 flashlight colour, 0x9456e0 flashlight decay, 0x945830 fog end,
0x945834 fog start, 0x945838 / 0x94583c fog factors, 0x945820 fog RGB, 0x945850 clear RGBA,
0x945558 far clip, 0x94554c near clip. Note that sh2ee calls 0x945830 the "front" fog; it is the
value that goes to `D3DRS_FOGEND`.

## 2. From globals to the GPU

`FUN_0047b870` (every frame) picks the light set:

* lit room: `FUN_0047a880` loads parallel lights 0, 1, 2 into the three light slots.
* dark room: `FUN_0047a8d0` builds the flashlight spot from 0x9456b0 (colour x 0.25) and James's
  light position, and loads parallel lights 1 and 2 into slots 0 and 1; slot 2 is black. Light 0
  of the record is not used.

Each slot colour is `record colour * 4 * 2^(8-12) = colour * 0.25` and its shader direction is
`-dir` (`FUN_0047a7b0`). Event code can override lights 0..2 and the ambient (0x945920 counters,
`FUN_0047b6c0`), which is how the "fake" fill light of a dark room with the flashlight off is done.

`FUN_0047a540` and `FUN_0047a250` then compute four colour constants through the intensity table
at 0x7f6d08 (`2^(8-i)`, indices set in `FUN_00479db0`), and `FUN_005b2560` stores them as vertex
shader constants c19..c22 (0x1dbb410..0x1dbb44f). It also sets `D3DRS_AMBIENT` to 2 x c19 for
the fixed-function path.

```
level = 0x8a2ba8   flashlight-off level: 0.3 normally, 1.0 before the flashlight is found,
                   0 while the flashlight is on (FUN_0050a010 / FUN_0050a7d0)
pulse = 0x8a2b88   0.75 for a moment after switching the flashlight, decays 0.15 per frame
c19 = 0.125  * (4*Ambient + 2*pulse + 2*level*Ambient)      = Ambient * (0.5 + 0.25*level)
c20 = 0.0625 * BaseColor                                    (1.4 for the usual 22.4)
c21 = 0.0625 * (BaseAmbient + 2*pulse + 2*level*BaseAmbient)
c22 = 0.25   * VertexAmbient                                (1.0 for the usual 4.0)
c8..c10  = -light dir (slot 0..2)
c15/c16/c17 = R/G/B of the three slot colours (one vector per channel)
```

The whole vertex shader constant block is uploaded by `FUN_005b2b00` as
`SetVertexShaderConstant(0, 0x1dbb2e0, 28)`:

| reg | source | use |
|---|---|---|
| c0..c3 | 0x1dbb2e0 | world-view-projection |
| c4..c6 | 0x1dbb320 | flashlight projection (cookie coordinates, depth along the beam) |
| c8..c10 | 0x1dbb360 | light directions |
| c11 | 0x1dbb390 | flashlight position |
| c12, c13 | | environment-map / eye vectors |
| c14 | `FUN_005b1d90` | (0, 0.5, 255, 1) |
| c15..c17 | 0x1dbb3d0 | light colours, one channel per register |
| c18 | 0x1dbb400 | flashlight attenuation (scale, bias) along the beam |
| c19..c22 | above | ambient terms |
| c23 | 0x1dbb450 | fog (A, B) |
| c26 | `FUN_005b23e0` | material colour of the current draw / 255 |
| c27 | `FUN_005b2490` | material overlay (specular) colour x flashlight colour / 255 |

### Fog

`FUN_0047a250` sets `D3DRS_FOGCOLOR` from 0x945820, `D3DRS_FOGSTART` = start (0x945834) and
`D3DRS_FOGEND` = end (0x945830), and `FUN_00458e50/e80/e90` + `FUN_00458de0` compute

```
fn = FogParm[3]/255 (factor at start), ff = FogParm[2]/255 (factor at end)
A = (ff*end - fn*start) / (end - start)
B = (fn - ff) * end * start / (end - start)
fog factor f = clamp(A + B / w, 0, 1)      (w = view depth, oFog in every map shader)
final = f * colour + (1 - f) * fog colour
```

So f is `fn` at the start distance and `ff` at the end distance, interpolated in 1/w rather than
linearly. Lit rooms by default keep 8% fog at 3000 and reach 92% at 5500. Dark rooms go from no
fog at 3000 to black at 7500. Units are world units (about 2 mm), Y down.

## 3. How the map is drawn (and what the vertex colours do)

The map draw list is built by `FUN_005b5670`; every draw record copies a 0x14-byte runtime
material: +0 colour (to c26), +4 overlay (to c27), +8/+0xc textures, and the byte at +0x10, which
becomes the draw "slot" (record byte 0x23) and is the `.map` material `mode`. `FUN_005b2f90` then
selects shaders from the table at 0x8f3008, indexed by `slot + 8*mode_set` (mode set 0 is the
normal one; it is set by `FUN_005b2f20(0)` in `FUN_0047a250`). The shaders live in the exe
(vertex shader table 0x8f2f48, pixel shaders 0x8f2fd8); this is their content, disassembled from
the D3D8 tokens.

Across all maps, the material mode lines up exactly with the vertex layout, which is what you
would expect if mode selects the shader:

| mode | vertex layout | triangles | shaders | per-vertex colour oD0 |
|---|---|---|---|---|
| 0 | pos, uv (0x14) | 35 k | VS1, PS1 | 1.0 (full bright, fog only) |
| 1, 2 | pos, normal, uv (0x20) | 4.27 M | VS5, PS1 | `2 * c26 * (sum_i max(N.L_i,0)*C_i + c19)` |
| 3 | pos, colour, uv (0x18) | 1.6 k | VS2 | vertex colour as is (the port draws it full bright, see section 7) |
| 4 | pos, normal, colour, uv (0x24) | 1.31 M | VS6, PS1 | `2 * (c20 * (sum_i max(N.L_i,0)*C_i + c21) + c22 * vcol)` |
| 6 | pos, normal, uv | 1.4 k | VS5, PS3 | like mode 1 |

(`L_i = -dir_i`, `C_i` = slot colour x 0.25, D3D clamps oD0 to 0..1.)

This answers the vertex colour question. **The `.map` vertex colour is not a diffuse factor; it
is added.** For mode 4 the colour is `2*c22*vcol = 2*vcol` with the stock VertexAmbient of 4, on
top of the lights scaled by `2*c20 = 2.8`. The material colour of mode-4 materials is 0 and the
shader never reads c26, so materials do not tint mode-4 geometry. For modes 1/2/6 the `.map`
material colour (usually 0xb2b2b2, 0.698) is the diffuse/ambient material colour: it multiplies
the light sum and the ambient. In D3D terms the PC treats the vertex colour like an emissive
source (and the fixed-function fallback in `FUN_005b2f90` does set
`D3DRS_EMISSIVEMATERIALSOURCE = D3DMCS_COLOR1` with a zero diffuse/ambient material for the
coloured cases, which agrees, though that path is only used without vertex shader support and I
read it with lower confidence).

The pixel shader for slot 0..5 (PS1, ps_1_1) is

```
t0 = base texture, t1 = environment (specular) map, t2 = flashlight cookie (projective), t3 = beam depth
spec  = t1.a * v1                      (v1 = oD1 = c27, material overlay * flashlight colour)
flash = (t0 * c1 + spec) * t2 * (t2.a * v0.a)    c1 = flashlight colour (ps clamps it to 1)
                                                  v0.a = clamp(depth*c18.x + c18.y) * N.(to flashlight)
out.rgb = t0 * v0 + (t3.z > 0.5 ? flash : 0)
out.a   = t0.a
```

so the flashlight is a per-pixel projected texture on top of the per-vertex light, and it does
not depend on the vertex colour or on c26. In ap38, 98% of the vertex colours are 0, the lights
in the dark-room set add about 0.03, c21 adds 0.013, so with the flashlight off a mode-4 wall is
about 0.04 x texture. That really is how dark the PC apartments are without the flashlight; the
flashlight term is what makes them readable.

## 4. Values that are only known at run time

The converter outputs the static values the game applies on room entry. These change later and
are documented here rather than extracted:

* the flashlight level (0x8a2ba8) and switch pulse (0x8a2b88) that feed c19/c21; `lighting.txt`
  gives c19/c21 for level 0.3 (flashlight off), the JSON also has the level-0 (flashlight on) set.
  In lit rooms the level is left over from the last dark room, so 0.3 or 0 are both possible.
* sh2ee's `RunRoomLighting` writes the same level (it calls it RoomLevels) for a few rooms.
* light overrides by events and cutscenes (0x945920.., `FUN_0047b6c0`), including the 0.3 x 0.3
  grey fill light the game adds in dark rooms while the flashlight is off.
* fog setters `FUN_0047cc10/20/30/40`, called only from cutscene code (0x5945d0..0x5a1590).
* `FUN_0047cc50`: in the town with flag 251 set, the fog start is re-derived from the camera
  height every frame between 3000 and 6500 (constants 0x80640c..0x806414).
* the brightness option scales the flashlight start distance (`FUN_0047bc20`).
* the fog particle layer (Fog/fog.c on PS2, 0x485xxx..0x489xxx on PC) is separate from D3D fog;
  only its colour comes from the record.

## 5. Doing it on the 3DS (PICA200)

Because the PC lights the map per vertex with three directional lights, the closest and cheapest
match is to do the same in a picasso vertex shader instead of using fragment lighting:

```
; uniforms: dirs[3] (= -dir), colR/colG/colB (= c15..c17), amb (c19 or c21), gain (c26 or c20),
;           vgain (= 2*c22, mode 4 only), fogA, fogB
d    = max(dp3(n, dirs[i]), 0)              (three of them)
lit  = (dp3(d, colR), dp3(d, colG), dp3(d, colB))
mode 1/2/6:  col = min(2 * gain * (lit + amb), 1)                 gain = material colour
mode 4:      col = min(2 * (c20 * (lit + c21) + c22 * vcol), 1)
mode 0:      col = 1
```

Then one texenv stage `texture0 * primary_color` gives the same result as PS1 without the
flashlight. Material colours come from the `.map` materials (`sh2map.Material.color`) for modes
1/2/6 and are ignored for mode 4. If you prefer fragment lighting instead, the equivalent setup
per draw is: three directional lights with diffuse `2*gain*C_i` (mode 1) or `2*c20*C_i` (mode 4),
global ambient `2*gain*c19` or `2*c20*c21`, no specular, then texenv stage 0 =
`fragment_primary + primary_color` (with the vertex colour pre-scaled by `2*c22` in the
converter or with the stage scale set to 2), stage 1 = `previous * texture0`. Either way the
vertex colour must be added, never multiplied.

The flashlight is a fragment spotlight (light 0 of the dark-room set) at the lamp on James' bone 3
(0x536190): colour `min(0.25*SpotCol0, 1)` = 1.0, distance attenuation LUT falling linearly from
`start` (1500) to `end` (6000), the PC cookie as the spot LUT (0x5b6360: `1 - tan^2(angle) /
tan^2(cone)`, inner radius 0, cone 0.785 rad), and N.L in the D0 LUT (LN input, signed), as VS5 and
VS6 put N.L into oD0.w. PS1 doubles the beam (`mul_x2`) before adding the base: stage 0
`2 * texture0 * fragment_secondary`, stage 1 `texture0 * primary_color + previous`, so
`t0*v0 + 2*t0*beam*cookie*ramp*N.L`. A room's own spot (record +0x08 spotLNum 0, position +0xd0,
direction +0xe0) takes the beam's place with the same colour and decay (0x47b100, 0x47b870); the
port has room 205's before the flashlight is taken (main.c room_spot).

Fog: the PICA fog LUT has 128 entries indexed by the depth-buffer value. Depth from any
perspective projection is affine in 1/w, and the PC fog factor `A + B/w` is also affine in 1/w,
so the LUT is a clamped straight line in depth. For each entry i, convert the depth `d = i/128`
back to w with the projection the port actually uses (for the D3D-style 0..1 mapping,
`w = n*f / (f - d*(f - n))`; check the sign convention of citro3d's tilted projection with
`zFlip`), store `clamp(A + B/w, 0, 1)`, and set the fog colour to the record's fog RGB. Set the
clear colour to `clear_r/g/b`, and the projection near/far to `clip_near`/`clip_far`.

## 6. Output format

`pack/lighting.txt`: one line per map and variant, whitespace separated, `#` lines are comments.
The default variant `-` comes first for every map; the others are `night` (town, flag 251 set),
`flag251` (er), `bfaw` (Born From a Wish; for `ma` the default line already is Born From a Wish).
Lines named `@1` to `@4` hold each open area's default for a tile of another area's grid: 0x494ef0
finds no cell in the stage's own table there and gives `glb << 16`, so FUN_0047c780 takes that area's
default (the whole-area record in the town). The port reads them when the stage's area is open and not
the tile's (`main.c` `env_room_changed`), which lights the Leave ending's road (cb03 in the forest
stage) as the forest.

```
name variant glb map_id dark
amb_r amb_g amb_b  bamb_r bamb_g bamb_b  bcol_r bcol_g bcol_b  vamb_r vamb_g vamb_b   (record values)
fog_r fog_g fog_b fog_start fog_end fog_near_f fog_far_f                               (factors 0..255)
clear_r clear_g clear_b clip_near clip_far
l0_dx l0_dy l0_dz l0_r l0_g l0_b  l1_...  l2_...     shader light slots: dir = direction the light
                                                     travels (use -dir in N.L), colour already x0.25
fl_r fl_g fl_b fl_start fl_end fl_cone               flashlight (colour x0.25, before the ps clamp)
c19_r c19_g c19_b c20_r c20_g c20_b c21_r c21_g c21_b c22_r c22_g c22_b   (flashlight off, level 0.3)
fogp_r fogp_g fogp_b fogp_a                         fog particle colour (ftol of MoveFogCol, 0x47c66e), -1 = keep
```

`pack/lighting.json` has, per map, every variant with the selected record address, how it was
selected, the room ids and sh2ee room names, the fog A/B, both constant sets, the character
point lights, and the raw records of every glb with their addresses.

Example values (default variant):

| map | room | dark | Ambient | fog start/end, factors | fog RGB | lights (dir, colour x0.25) |
|---|---|---|---|---|---|---|
| ap01 | APT_E_RM_210 (area default) | 1 | 0.15 | 3000/7500, 255/0 | 0 | (0,.71,.71) .0025; (-.87,.35,-.35) .0125; flashlight |
| ap06 | APT_E_HALLWAY_1F (default) | 1 | 0.15 | 3000/7500, 255/0 | 0 | same |
| ap38 | APT_E_RM_101_1 (0x7fa208) | 1 | 0.15 | 3000/7500, 255/0 | 0 | same |
| ap84 | APT_W_RM_109_2 (0x7fabb8) | 0 | 0.70 | 8000/12000, 255/0 | 1,1,1 | (0,1,0) .025; (0,.71,.71) .0025 |
| hp01 | HSP_LOBBY (default) | 1 | 0.15 | 3000/7500, 255/0 | 0 | as ap01 |
| cb03 | town east, day (-2 record) | 0 | 0.65 | 3000/5500, 235/20 | 70 | (0,1,0) .025; (0,.71,.71) .0025; (-.87,.35,-.35) .0125 |
| cc04 | town west, day (-2 record) | 0 | 0.65 | 1675/5525, 240/23 | 124 | same as cb03 |

## 7. Enhanced Edition fixes

`lighting.py` applies the Enhanced Edition fixes that change these records unless `--pc` is given
(the format stays the same; `port/EE_FIXES.md` has the full list). In the data: the default far clip
is 11000 instead of 7500 (IncreaseDrawDistance), the cemetery ca14 uses the forest default record
(RoomLightingFix), Blue Creek ap72 has fog start 4200 (FogParameterFix), room 312 (rr91) clears to
255,255,227 (LightingFix), the Eddie cold rooms ps189/ps193 get EE's VertexAmbient and light 0
(CatacombsMeatRoomFix), and the character point lights of six records are changed for
PS2FlashlightBrightness and LightingTransitionFix (`ee_patched` in the JSON records). The port
applies the rest at run time: the flashlight colour x0.3 and reach 8500 (`envlight.c`), the
flashlight level of FUN_0050a010 with EE's room levels, mode 3 full bright, and a few scene rules
in `main.c`.

Correction to the mode table: mode-3 vertices are 0x18 bytes (position, BGRA colour, uv), which
`sh2map.py` now reads; before, their uvs came out as 0 and their colour white.

## 8. The fog particle layer (fog.c)

Besides the D3D distance fog, the outdoor areas and a few rooms have the drifting fog of the PS2's
`Fog/fog.c`, which the PC keeps at 0x483820..0x48a0d0 (FOG_WORK at 0x1f63b20..0x1f7d2cc, same layout
as the PS2 struct). `port/source/fog.c` ports it; `tools/sh2conv/fog.py` copies its data into the
pack (`pack/fog/*.fcl`, `pack/fog/fog.t3x`).

* Where (0x4849d0): by the stage's glb_crd. ca, cb, cc (with `park.fcl` and `ccnw.fcl` for parts of
  town west), cd and ob load `<area>GB.fcl`; inside, only some rooms have fog: Eddie's rooms (ps,
  0x8e..0x90), Blue Creek 209, hotel rooms 0x98, 0x9b and 312, the final rooms (0xb0, 0xbb above
  y -12450, 0xbd) and eight mansion rooms, each with a fixed fog centre. Everything else has none.
* `.fcl` (FUN_004838a0): six u16 counts, then vertical walls (6 x s16, x50), free walls (12 x s16),
  spheres and cylinders (4 floats: x, y, z, size), fog areas (x0, z0, x1, z1 in units of 50, env
  index) and FOG_ENV_DATA records (0x18 bytes: PartNum, MaxPos, PartSize, EscapeRange, FloorY,
  LimitY, LimitHeight, WaterY, wind, flags, Double, Alpha, GridRate). 0x484760 picks the record of
  the area the camera's target stands in; scenes 3, 4, 0x5a and 0x5c have built-in records.
* Particles (0x4895b0): up to 500 (0x487a00: counts above 200 are halved; dark rooms halve again)
  in a box of +-MaxPos around the centre (0x487ca0: the camera's watch point, at most 2000 from the
  camera), wrapped mirrored at the box faces and faded over the last 300 units. Each frame: wind
  (0x4880f0, 18 wind kinds, changed every 300..900 frames, turning at most 60 degrees), an 8x8x8
  world-anchored density grid that pushes particles out of crowded cells by GridRate / PartNum
  (0x486710; the PC adds the three trilinear weights instead of multiplying them), a slow random
  alpha drift towards Alpha (0x4870e0), and James as a cylinder (r 110 at waist height) and a
  sphere (r 60) that take the fog along when he moves (0x486f20 / 0x486d70). Touched particles fade
  out and come back in the thinnest cell.
* Walls (0x4838a0, 0x485f00, 0x4864a0): the walls with a corner within 5000 of the fog centre are
  loaded (again whenever the centre moves 500), up to 188, each as a normal, the centre of its four
  corners and their box (10 wider in x and z). With walls about, a particle closer to a wall's plane
  than PartSize + 10 + EscapeRange and inside its box is pushed out, loses the speed across the wall
  and counts as touched (fading out from the next frame on). The PC's normal is the unnormalised
  cross product of two edges (0x447f40), so the distance is the real one times the wall's area: in
  practice only particles almost exactly in a wall's plane are affected, plus every particle that
  passes within 10 units of one of the zero-area walls in the data (they have |n| = 0, so the
  distance test always passes). The port does the same, quirk included.
* Blow points (0x48a320, 0x489ee0, 0x489f70, 0x48a0d0, 0x48a180, drawn by 0x48a500): flag 0x100,
  set for the three Eddie rooms (room ids 0x8e, 0x8f, 0x90). Each room has a table of vents (14, 10
  and 16; position, velocity, start size) and an interval (20, 30, 60 in 1/60 s); every vent puffs
  at that interval with a random spread, up to 200 puffs. A puff starts at its vent's size and
  grows to PartSize, falls slowly (4.9 x frame time per update, Y down: cold air sinks), slows by
  0.95 an update, its alpha wanders towards the fog's Alpha, and it fades out when it leaves the
  box (twice MaxPos) or sinks under the floor. They are drawn like the particles at their own size,
  with a fixed near limit of 64 and the PC's 0.049 texture window. `fog.py` takes the three tables
  from the exe into `pack/fog/blow.txt`. The port keeps the PC's 30 updates a second for them and
  draws at most 48. Their alpha byte is the one the port uses for the particles (EE FogFix's 112)
  where the PC reads the colour's alpha, so the puffs and the particles stay in step. They are
  faint either way: the puffs keep to the floor, where the floor fade takes most of their alpha.
* Drawing (0x488a80): each particle is a camera-facing fan, alpha at the centre and 0 at the
  corners, PartSize across, stretched along its screen movement, squashed near FloorY, faded near
  the camera, with a 0.1-wide window of the 256x256 fog texture (id 0x2b22) that scrolls with the
  view; D3D fog off, MODULATE2X, sorted by depth. Then (0x4876c0) one full-screen quad of the same
  texture, scrolled by the camera's movement and the wind, at alpha (Color.a x Alpha) >> 9.
* Colour: the record's MoveFogCol when it has one (0x47c66e), otherwise 0x80808080; the town at
  night takes it from 0x8a2c24 (0x50a5e0).
* 3DS budget: at most 144 particles are simulated and drawn (the PC allows 500). The missing ones'
  share of alpha goes to the drawn ones, so the fog is as dense with fewer sprites.

## 9. The fill light with the flashlight off

In a dark room with the flashlight off, 0x50a7d0 calls `FUN_0047b6c0(&0x8a2c00, 0.3 x level, 1)`:
light 1 of the record (shader slot 0 in the dark set) is replaced by a grey directional light
travelling along the camera's view (0x8a2c00 follows the camera vector from 0x9464f0), colour 0.3 x
level before the usual x0.25. The port does this in `main.c` `lighting_frame`.

## 10. Character lighting (chrlight.c)

Characters have their own light list, the PS2's `Chacter_Draw/light_n.c` (LightWork at 0xaac8f8:
twelve 0x98-byte `Light` records). FUN_00508a90 fills it from the room record when a room is set up:

* parallels: index 2 = record light 1, index 3 = record light 2 (with parallel_mode 2: record
  colour 1 and record colour 0 on light 2's direction), index 1 = record light 0 outside only
  (glb 1..4 and 0x11). Dark rooms have no index 1.
* the Ld block at +0xb0, read in this order: one spot when spotLNum >= 0 (0x50d310; in the mansion a
  count of 0 is an event light instead), then pointLNum points (0x50cff0), then fake points written
  from index 2 on (over the parallels, 0x50d0a0), then fake spots (0x50d150). LIGHTING's section 1
  listed points before spots; the code reads the spot first.
* the flashlight as a spot at index 0 in dark rooms (0x508060 / 0x5083f0 / 0x508690; James himself
  gets a parallel from his own beam instead, which the port leaves out).

Per character (0x50b5d0 -> 0x509aa0): fake lights turn into parallels from their position to the
character, scaled by the falloff (0x5091c0); points and spots get an influence = falloff at the
character's distance minus its radius 800, times the light's luminance (0.3 r + 0.6 g + 0.1 b), and
the six strongest are kept (0x50ce10). The vertex shader at 0x8a2ed8 then computes, per vertex,

```
lights = sum over 4 parallels   max(N . L, 0) * colour
       + sum over 4 points      clamp(f_ra / d + f_rb) * clamp(N . dir) * colour
       + sum over 4 spots       the same * clamp(cos * s_a + s_b)
colour = min(lights * c43 + c44, c45)
c43 = material diffuse (0.7 for 90% of the materials), c44 = (2.1 x Ambient + 0.3 x min(level, 1))
      x material ambient (0.3; 1.0 for material type 1), c45 = 1
```

with f_ra / f_rb from the falloff start s and end e (`-s e / (s - e)`, `s / (s - e)`) and for the
record spots s_a = 1, s_b = -0.8 x the spot's first decay value. The record colours are used as they
are (no x0.25 as for the map). The "movable object presets" that the Enhanced Edition changes at
0x4fee16 / 0x5027e6 are not about shadows, as an earlier version of this page said: 0x1f80314 picks
one of five pixel shaders for the per-pixel flashlight on characters (0x8a41e0, 0x8a42a8, 0x8a4370,
0x8a4438, 0x8a4500, made by 0x50f4c0). Presets 0 to 3 differ only in the output scale (x1, x2, x4,
x8), and 0x4fe4a0 picks the one that fits the largest flashlight colour channel (up to 1, 2, 4,
more) and scales the colour by 1, 0.5, 0.25 or 0.125 to match, which is a range trick of ps_1_1
and leaves the result the same. Preset 4 is a special case for a grey light. EE forces preset 3 and
puts 0.7 in place of the 0.125, so characters get 5.6 times the beam colour; with its 0.3 times
darker PS2 beam that is 1.7 times the PC's light on characters. The port's character flashlight
(colour 7, saturating after the x0.7 diffuse) is already at full brightness for any of these, so
there is nothing to add.

`lighting.py` writes the list per map and variant to `pack/chrlight.txt`; `port/source/chrlight.c`
rebuilds it and maps it onto the PICA: light 0 stays the map's flashlight (specular path), lights
1..3 are the parallels, lights 4..7 the strongest points and spots (and the flashlight for other
characters), as diffuse fragment lights with the falloff in the distance LUT and the spread in the
spot LUT, material diffuse 0.7, ambient as above. Left out: per-primitive materials, James' own
beam, reflections, scene lights (cutscenes have demo.c's), the light overrides of events. The
character shadows are in section 11.

## 11. Character shadows (shadow.c)

The PC draws the stencil shadows the PS2 beta calls `sh2shd` (`sh2shd_shadow_model.c`,
`sh2shd_char_jms.c`, `sh2shd_shadow_vu1_packet.c`); the PC code is a CPU version of the PS2's VU1
programs.

- **Data.** Every character, item and weapon model has a shadow model, `data/chr/<folder>/<name>.kg1`
  (layout in `tools/sh2conv/shadows.py`): per bone a few closed triangle strips with a face normal
  per triangle, in the bone's space. The gameplay models' own files (`hll_jms.kg1`,
  `lll_mar.kg1`) are empty, but the loader pairs James' `hll_jms` and `lll_jms` with the cutscene
  model's full `hhh_jms.kg1` (CharaDataLoadExec 0x493260, record 0x7d8688; the bones 0..40 match).
  James and his weapon get their own light (sh2shd_get_jms_shadow_env 0x5acaf0, Draw_Jms_Shadow
  0x509870): a room's own spot as a point light (kind 5, room 205), self shadows under his
  flashlight (kinds 1 to 3, not ported), never the flashlight's spot. Maria's pairing is unchecked.
- **Which shadows.** `0x5a8370` picks one of two kinds every frame. In a dark room
  (`gFlashlightAvailable` 2, set from the record's NightOrDay by `0x47b200`) whose record has
  ShadowDarkNess (+0x0e, `0x47bb00`) set, `0x5aad70` draws the stencil shadows below. The light is
  `0x47e000`: an event's light when one is set, otherwise James' flashlight; light kind 0 (spot)
  except in two rooms. Up to four characters are added (`0x5a4ed0`, `0x5a8630`: range 5000, the
  cone from the light's parameters). Everywhere else, the lit rooms included, it draws the drop
  shadows (`0x5a7c90`, see the end of this section).
- **Volumes.** `0x444b20` culls each object and strip against the range and cone, and `0x4449f0`
  walks a strip: a triangle is lit when `(v - L) . n < 0`, and for each run of lit triangles it emits
  the side walls of the run's outer edges, each edge pulled away from the light (`0x442ae0`).
- **Counting.** `0x445540` sets the states and `0x4426f0` draws the walls twice with colour writes off:
  culling one winding with `INCR` on depth pass, then the other with `DECR`. `0x442d30` then draws a
  full-screen quad with stencil ref 0x81, `LESSEQUAL`, blend `ZERO / SRCALPHA` and a vertex alpha of
  0x80: every pixel counted at least once above the 0x80 the buffer starts at is halved. The
  characters are drawn with stencil `REPLACE` 0 first, so they never take a shadow (the Xbox did;
  EE's EnableSoftShadows brings that back along with blurred edges).

The port (`port/source/shadow.c`) follows this with three changes. It counts on depth fail (z-fail)
with both caps added to each volume (the lit triangles and their pulled-away copies), because the PC's
open volumes counted on depth pass go wrong whenever the camera is inside one, and SH2's fixed
cameras often look back at James past a monster; the shadow looks the same otherwise. The darkness
is the Enhanced Edition's (`GetShadowOpacity`: 170 of 255 by default, per-room values in a few
rooms), faded in over about 33 frames when the flashlight comes on as EE does, where the PC halves
the colour. The blur of EE's soft shadows is left out (the PICA has no cheap way to do it). Volumes
are pulled out to 6000 units from the light, past the 5000 range, so they never fold back.

**Drop shadows** (`sh2shd_exe_drop_shadow` on the PS2; `0x5a7c90`, `0x5a7730`, `0x445780`,
`0x445220`, `0x444780`, states `0x442370`). The characters' .kg1 models again, flattened onto a
plane 16 units above the floor under each character (`0x5a6ad0`, `0x5a4d20`) along a light
direction of (1000, -10000, 1000), almost straight down (`0x5ab830`: exactly down in rooms 0xbd and
0xc1 of glb 0xb). Only the triangles facing the light remain (the PC culls the others after the
projection). Every corner gets the colour of the drop env (`0x5ab750`, grey 8 by default) and an
alpha of 2 x a0 at 700 units above the floor up to 2 x a1 at the floor, at most 128 (`0x444780`;
a0 0 and a1 32 by default, with overrides for some maps of glb 1, 3, 5, 9 and 0xb), blended over the
scene with D3D fog on and no depth writes. These are James' (from `hhh_jms.kg1`) and the enemies'
shadows in the town, the streets and the lit rooms: a soft dark patch around the
feet that fades out higher up the body. The port draws them the same way (`shadow_begin_drop`).
The distance factor `0x5a7730` computes per character (0.3..1) is stored but not used by the PC's
drawing code, so the port leaves it out.

Not ported: shadows from other light kinds (point, parallel, James' "self" shadow of `0x5a6560`),
shadow casting map objects (`.kg2`), items and weapons, the per-room exclusions (`0x5abcd0`), and
cutscene shadows (each scene has its own shadow light).

## 12. Confidence

| item | confidence | basis |
|---|---|---|
| record table, layout, selection, defaults | high | decompile + disassembly of 0x47c780/0x47be40, PS2 struct match, data sanity |
| record id = glb<<16 \| map number | high | 0x88e210 and the outdoor tile tables hold the bg file numbers |
| c19..c22 formulas, light x0.25, fog A + B/w | high | 0x47a250/0x47a540/0x5b2560/0x458de0 and the shader tokens |
| shader formulas per slot | high | disassembled vs_1_1 / ps_1_1 tokens from 0x8f1e70.. |
| slot = `.map` material mode | medium-high | `FUN_005b5670` copies byte +0x10 of the runtime material; mode/vertex layout correlation is exact over all maps; the loader that builds the runtime material from the `.map` material was not traced |
| c26 = `.map` material colour, c27 = overlay | medium-high | same runtime material; values (0xb2b2b2 for mode 1, 0 for mode 4) fit the shaders |
| flashlight attenuation (c18) as a linear start/end ramp | medium | c18 comes from the light list (`FUN_005b6360`), not traced |
| SpotParm[0], ShadowForceLightNo high byte | low | not used by the code read |
| character shadow volumes, counting and darkening | high for the method, medium for the light's cone test | 0x4449f0 / 0x4426f0 / 0x442d30 decompiled; the per-bone light block filled by code not traced |
| qp/qt glb assignment | medium | room table only (no .cld); both glbs share one record array anyway |
| fixed-function fallback states | medium-low | decompiler output of `FUN_005b2f90` is noisy; only matters without vertex shaders |

Open questions: the day town fog colour of 70 grey looks darker than the PC town; something
after the frame (gamma, the noise filter, or the fog particle layer, which is drawn with
`MoveFogCol`) probably brightens it, but I did not confirm which. The fixed-function pixel path
(no pixel shader support) was not traced.
