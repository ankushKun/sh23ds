#!/usr/bin/env python3
"""Extract the per-room lighting and fog parameters of Silent Hill 2 (PC Director's Cut).

  lighting.py [--exe sh2pc.exe] [--pack pack] [--out pack] [--pc]

The game keeps one static environment record per bg map (PS2 name DrawEnvData, 0x1f0 bytes on
PC) in per-area arrays reached through the pointer table at 0x8063c8. On every stage connect
(0x401480 -> 0x476380 -> FUN_0047c780) the record for the arrival room is selected and
FUN_0047be40 copies it into the frame globals that FUN_0047a250 / FUN_0047a540 turn into vertex
shader constants and D3D fog / clear state. This script reproduces that selection and the
derived values for every map listed in pack/bg/<area>/rooms.txt and writes:

  <out>/lighting.json   all records (raw, with addresses) plus the resolved values per map
  <out>/lighting.txt    one line per map and variant, whitespace separated (header comment)

Format, formulas and confidence notes are in LIGHTING.md next to this file.

The Enhanced Edition fixes that change these records or their selection are applied by default
(see EE_* below and port/EE_FIXES.md); --pc writes the unmodified PC values.
"""
import argparse
import hashlib
import json
import math
import os
import struct
import sys

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_EXE = os.path.join(ROOT, 'Silent Hill 2', 'sh2pc.exe')
DEFAULT_PACK = os.path.join(ROOT, 'pack')
SH2EE_HEADER = os.path.expanduser('~/Developer/refsrc/sh2ee/Patches/Patches.h')

EXE_MD5 = 'f3f5360298b6d0f6f2e09cf24a0925d1'
IMAGE_BASE = 0x400000

ENV_TABLE = 0x8063c8        # DrawEnvData* per glb_crd 0..16 (read in FUN_0047c780)
N_GLB = 17
REC_SIZE = 0x1f0            # FUN_0047c780 steps 0x7c dwords
ROOM_MAPS = 0x88e210        # u8[4] per room id; FUN_00494ef0 uses byte 0 as the map number
INTENSITY = 0x7f6d08        # float[] = 2^(8-i); indexed by the bytes at 0x9454e2..e7

# glb_crd (Stage_Data.glb_crd) of each bg area. ca/cb/cc are town tiles shared between the
# forest (1), town east (2) and town west (3); the prefix choice below follows the door
# destinations in pack/events (ca: forest, cb: east, cc: west). qp/qt have no .cld, so their
# glb comes from the room table (end_dog stage glb 14 rooms 0xbc/0xbd -> maps 1/5, glb 15
# room 0xbf -> map 1); both glbs share one record array, so the choice does not change values.
AREA_GLB = {'ca': 1, 'cb': 2, 'cc': 3, 'cd': 4, 'ob': 5, 'er': 6, 'bw': 7, 'th': 8, 'ap': 9,
            'hp': 10, 'ps': 11, 'rr': 12, 'ru': 13, 'qp': 14, 'qt': 15, 'ma': 16}
AREAS = ['ap', 'bw', 'ca', 'cb', 'cc', 'cd', 'er', 'hp', 'ma', 'ob', 'ps', 'qp', 'qt', 'rr',
         'ru', 'th']

# Defaults FUN_0047be40 uses when a record field is 0.
FOG_DEFAULT_LIT = (5500.0, 3000.0, 20.0, 235.0)      # 0x45abe000 0x453b8000 0x41a00000 0x436b0000
FOG_DEFAULT_DARK = (7500.0, 3000.0, 0.0, 255.0)      # 0x45ea6000 0x453b8000 0 0x437f0000
FAR_CLIP_DEFAULT = 7500.0                            # 0x45ea6000 -> 0x945558
NEAR_CLIP_DEFAULT = 100.0                            # 0x42c80000 -> 0x94554c

# Intensity table indices set in FUN_00479db0 (0x9454e2..0x9454e7) -> 2^(8-i)
K_AMBIENT = 2.0 ** (8 - 0xb)     # e2: c19 = 0x9454a0 * 0.125
K_BASECOLOR = 2.0 ** (8 - 0xc)   # e3: c20 = BaseColor * 0.0625
K_VTXAMB = 2.0 ** (8 - 0xa)      # e4: c22 = VertexAmbient * 0.25
K_BASEAMB = 2.0 ** (8 - 0xc)     # e5: c21 = 0x9454d0 * 0.0625
K_LIGHT = 4.0 * 2.0 ** (8 - 0xc)  # FUN_0047a7b0: colour * 4 * table[e6=0xc] = 0.25
K_FLASH = 4.0 * 2.0 ** (8 - 0xc)  # FUN_0047a8d0: SpotCol0 * 4 * table[e7=0xc] = 0.25

# Flashlight-off level (0x8a2ba8, FUN_0050a010): 0.3 with the flashlight acquired, 1.0 before.
LEVEL_FLASH_OFF = 0.3
LEVEL_FLASH_ON = 0.0

# ------------------------------------------------------------------ Enhanced Edition fixes
# Ported from Silent Hill 2: Enhanced Edition (github.com/elishacloud/Silent-Hill-2-Enhancements,
# zlib licence). EE patches the PC exe in memory; these are the same changes to the record data and
# its selection. Every record patch checks the original value first, like EE's CheckMemoryAddress.

# "IncreaseDrawDistance" (Patches/DrawDistance.cpp): the default far clip of FUN_0047be40
# (0x47c64d) and of the stage overlays becomes 11000 instead of 7500, so less of a room is cut off.
EE_FAR_CLIP_DEFAULT = 11000.0

# "RoomLightingFix" (Patches/LightingPatch.cpp, PatchRoomLighting): Set_DrawEnvData (0x47c7cc) looks
# up 0x1000d instead of 0x1000e, so the cemetery (ca14) keeps the forest's area default. On the PC
# the cemetery record was only picked when a save was loaded there, which EE calls wrong values.
EE_RECORD_REMAP = {0x1000e: 0x1000d}

# "FogParameterFix" + "FogSpeedFix" (Patches/FogAdjustments.cpp): in Blue Creek room 208/209 (ap72)
# the dark-room fog start written at 0x47c33e is 4200 instead of 3000.
EE_FOG_START = {0x90048: 4200.0}

# "LightingFix" (Patches/TreeLighting.cpp, RunTreeColor): hotel room 312 (rr91) clears to a warm
# white (255, 255, 227) instead of the record's grey, which is what shows behind the patio doors.
EE_CLEAR_COLOR = {0xc005b: (255, 255, 227)}

# "LightingTransitionFix" (Patches/LightingTransition.cpp, LightingTransitionASM at 0x50d334): in
# these rooms the record's spot light reaches characters as a point light (kind 2 instead of 3), so
# James brightens and darkens as he walks past it instead of only inside its cone. Blue Creek 203,
# the hotel 2F west hallway, three Otherworld hotel hallways and its staircase, the mansion 2F hall.
EE_SPOT_AS_POINT = {0x90040, 0xc003f, 0xd0009, 0xd0029, 0xd002d, 0xd0039, 0x10000d}

# (option, record address, record map id, field offset, original floats, new floats)
EE_RECORD_PATCHES = [
    # "CatacombsMeatRoomFix" (Patches/CatacombsMeatRoom.cpp): VertexAmbient of the two cold rooms
    # where Eddie is fought (ps189, ps193) and light 0 of ps193, for redder meat rooms.
    ('CatacombsMeatRoomFix', 0x7fe5d8, 0xb00bd, 0x1e0, (4.0, 4.0, 4.0), (2.5, 2.0, 2.0)),
    ('CatacombsMeatRoomFix', 0x7fe7c8, 0xb00c1, 0x1e0, (4.0, 4.0, 4.0), (3.15, 2.8, 2.8)),
    ('CatacombsMeatRoomFix', 0x7fe7c8, 0xb00c1, 0x020, (0.1, 0.1, 0.1), (0.0, -0.1, -0.1)),
    # Character point light 0 (Ld_0, +0xb0 colour, +0xc0 decay). The port does not light characters
    # with these yet; the values are here so lighting.json is right once it does.
    # "PS2FlashlightBrightness" (Patches/EnhancedFlashlight.cpp): no character light in Wood Side
    # room 205 (ap30) and in the Otherworld hotel kitchen (0xd0005).
    ('PS2FlashlightBrightness', 0x7f9e28, 0x9001e, 0x0b0, (7.0, 7.0, 7.0), (0.0, 0.0, 0.0)),
    ('PS2FlashlightBrightness', 0x801648, 0xd0005, 0x0b0, (0.6, 0.6, 0.6), (0.0, 0.0, 0.0)),
    # "LightingTransitionFix" (Patches/LightingTransition.cpp): Blue Creek room 203, the Baldwin
    # mansion 2F hallway, the hotel kitchen, hotel rooms 202/204.
    ('LightingTransitionFix', 0x7fa7d8, 0x90040, 0x0b0, (6.0, 6.0, 6.0), (1.0, 1.0, 1.0)),
    ('LightingTransitionFix', 0x803d08, 0x10000d, 0x0b0, (3.0, 3.0, 3.0), (1.0, 1.0, 1.0)),
    ('LightingTransitionFix', 0x7fed98, 0xc0005, 0x0b0, (2.0, 2.0, 2.0), (0.8, 0.8, 0.8)),
    ('LightingTransitionFix', 0x800e88, 0xc0053, 0x0b0, (1.4, 1.4, 1.4), (1.0, 1.0, 1.0)),
    ('LightingTransitionFix', 0x800e88, 0xc0053, 0x0c8, (500.0, 1500.0), (500.0, 500.0)),
]


def ee_patch_record(b, va):
    """Applies EE_RECORD_PATCHES to the raw bytes of the record at va; returns (bytes, notes)."""
    b = bytearray(b)
    notes = []
    for opt, addr, map_id, off, old, new in EE_RECORD_PATCHES:
        if addr != va or struct.unpack_from('<i', b, 0)[0] != map_id:
            continue
        cur = struct.unpack_from('<%df' % len(old), b, off)
        if any(abs(c - o) > 1e-4 for c, o in zip(cur, old)):
            print('warning: %s patch at %#x+%#x skipped, found %s' % (opt, va, off, cur), file=sys.stderr)
            continue
        struct.pack_into('<%df' % len(new), b, off, *new)
        notes.append('%s: +%#x %s -> %s' % (opt, off, list(old), list(new)))
    return bytes(b), notes


# Rooms FUN_00476870 lists (Born From a Wish clear-colour overrides)
BFAW_CLEAR_ROOMS = {0xd0049, 0xb00c1, 0xb00b9, 0x50001, 0x90048, 0xb00bd, 0xc005b, 0xc0029,
                    0xc0047, 0xd0035, 0x100031, 0x100035, 0x10003d, 0x100051, 0x100055,
                    0x100015, 0xe0005, 0x10000d, 0x10001d}


class Exe:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        md5 = hashlib.md5(raw).hexdigest()
        if md5 != EXE_MD5:
            print('warning: %s has md5 %s, expected %s (Director\'s Cut)' % (path, md5, EXE_MD5),
                  file=sys.stderr)
        self.data = raw
        pe = pefile.PE(path, fast_load=True)
        self.secs = [(s.VirtualAddress + IMAGE_BASE, max(s.Misc_VirtualSize, s.SizeOfRawData),
                      s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]

    def read(self, va, n):
        for base, vsize, raw, rsize in self.secs:
            if base <= va < base + vsize:
                o = va - base
                out = self.data[raw + o: raw + min(o + n, rsize)] if o < rsize else b''
                return out + b'\0' * (n - len(out))
        raise ValueError('address %#x not mapped' % va)

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]


def r5(v):
    return float('%.5g' % v)


def v4(b, o):
    return [r5(x) for x in struct.unpack_from('<4f', b, o)]


def norm3(v):
    """FUN_0047bd80: xyz normalise, 1.0 scale when the length is 0."""
    ln = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    k = 1.0 / ln if ln > 0 else 1.0
    return [r5(v[0] * k), r5(v[1] * k), r5(v[2] * k)]


def ftol(x):
    return int(x)  # __ftol (FUN_0056e3b0) truncates toward zero


# ------------------------------------------------------------------ record parsing
def parse_record(b, va):
    i32 = struct.unpack_from('<i', b, 0)[0]
    nums = struct.unpack_from('<6b3h', b, 4)
    rec = {
        'addr': '%#x' % va,
        'map_id': i32,
        'map_id_hex': '%#x' % (i32 & 0xffffffff),
        'parallel_mode': nums[0],     # +0x04 PS2 parallelLNum
        'fake_spot_num': nums[1],     # +0x05 PS2 FakeSpotNum
        'fake_point_num': nums[2],    # +0x06 PS2 FakePointNum
        'point_num': nums[3],         # +0x07 PS2 pointLNum
        'spot_num': nums[4],          # +0x08 PS2 spotLNum (-1 = none / normal flashlight room)
        'dark': nums[5],              # +0x09 PS2 NightOrDay; !=0 -> flashlight room
        'shadow_color': nums[6],      # +0x0a PS2 ShadowColor
        'shadow_force_light': nums[7] & 0xffff,  # +0x0c PS2 ShadowForceLightNo
        'shadow_darkness': nums[8],   # +0x0e PS2 ShadowDarkNess
        'parallel': [{'dir': v4(b, 0x10 + 0x20 * k), 'color': v4(b, 0x20 + 0x20 * k)}
                     for k in range(3)],
        'flashlight_color': v4(b, 0x70),     # SpotCol0
        'flashlight_decay': v4(b, 0x80),     # SpotDecayParm0: start, (cos), end, cone
        'spot_parm': v4(b, 0x90),            # [1] far clip, [2] near clip, [3] fog colour scale
        'fog_parm': v4(b, 0xa0),             # end, start, far factor, near factor (0..255)
        'move_fog_color': v4(b, 0x1a0),
        'ambient': v4(b, 0x1b0),
        'base_ambient': v4(b, 0x1c0),
        'base_color': v4(b, 0x1d0),
        'vertex_ambient': v4(b, 0x1e0),
    }
    # Ld_0..Ld_E: point lights (Col, Decay, Pos), spots (Col, Decay, Pos, Dir), then the fake
    # point and spot lights, packed in that order (the normalisation loops in FUN_0047be40).
    o = 0xb0
    pts, sps, fpts, fsps = [], [], [], []
    for _ in range(max(rec['point_num'], 0)):
        pts.append({'color': v4(b, o), 'decay': v4(b, o + 16), 'pos': v4(b, o + 32)})
        o += 0x30
    for _ in range(max(rec['spot_num'], 0)):
        sps.append({'color': v4(b, o), 'decay': v4(b, o + 16), 'pos': v4(b, o + 32),
                    'dir': v4(b, o + 48)})
        o += 0x40
    for _ in range(max(rec['fake_point_num'], 0)):
        fpts.append({'color': v4(b, o), 'decay': v4(b, o + 16), 'pos': v4(b, o + 32)})
        o += 0x30
    for _ in range(max(rec['fake_spot_num'], 0)):
        fsps.append({'color': v4(b, o), 'decay': v4(b, o + 16), 'pos': v4(b, o + 32),
                     'dir': v4(b, o + 48)})
        o += 0x40
    rec['point_lights'], rec['spot_lights'] = pts, sps
    rec['fake_point_lights'], rec['fake_spot_lights'] = fpts, fsps
    return rec


def read_records(exe, ee=True):
    out = {}
    for g in range(N_GLB):
        p = exe.u32(ENV_TABLE + 4 * g)
        if p == 0:
            continue
        recs = []
        for _ in range(64):
            raw, notes = ee_patch_record(exe.read(p, REC_SIZE), p) if ee else (exe.read(p, REC_SIZE), [])
            rec = parse_record(raw, p)
            rec['_raw'] = raw
            if notes:
                rec['ee_patched'] = notes
            recs.append(rec)
            p += REC_SIZE
            if rec['map_id'] == -1:
                break
        out[g] = recs
    return out


# ------------------------------------------------------------------ FUN_0047c780
def select_record(recs, glb, map_id, chapter=0, flag251=False, ee=True):
    """Returns (record, how). flag251 = game flag 251 (bit 27 of gGameFlag word 7, 0x1f7d5fc)."""
    if ee and map_id in EE_RECORD_REMAP:
        map_id = EE_RECORD_REMAP[map_id]
    if glb in (2, 3):
        want = -1 if (chapter != 1 and flag251) else -2
        for r in recs:
            if r['map_id'] == want:
                return r, 'whole-area record %d (%s)' % (want, 'night' if want == -1 else 'day')
    if glb == 6:
        dark = 1 if flag251 else 0
        for r in recs:
            if r['map_id'] == -1 or (r['map_id'] == map_id and r['dark'] == dark):
                return r, ('exact id, dark=%d' % dark) if r['map_id'] != -1 else 'area default'
    for r in recs:
        if r['map_id'] == -1 or r['map_id'] == map_id:
            return r, 'exact id' if r['map_id'] != -1 else 'area default (no record for this map)'
    return recs[-1], 'area default'


# ------------------------------------------------------------------ FUN_0047be40
def resolve(rec, room_id, chapter=0, ee=True):
    """Frame globals after FUN_0047be40, plus the vertex shader constants of FUN_0047a250."""
    dark = rec['dark'] != 0                      # FUN_0047b200: gFlashlightAvailable = 1 + dark
    pm = rec['parallel_mode']
    cols = [rec['parallel'][k]['color'] for k in range(3)]
    if pm == 0:
        keep = (1, 1, 1)
    elif pm == 1:
        keep = (1, 1, 0)
    else:
        keep = (1, 0, 0)                         # colours 1/2 multiplied by 0.0 (0x62eddc)
    par = []
    for k in range(3):
        d = norm3(rec['parallel'][k]['dir'])
        c = [r5(x * keep[k]) for x in cols[k]]
        par.append({'dir': d, 'color': c, 'zeroed_for_map': not keep[k]})

    amb = rec['ambient'][:]
    bamb = rec['base_ambient'][:]
    notes = []
    glb = (room_id >> 16) & 0xfff
    if chapter == 1:      # every chapter-1 path of FUN_0047be40 reaches the switch at 0x47c1a3
        if glb == 9:
            k = {0x90040: 0.3, 0x9006c: 0.3, 0x9005c: 0.5, 0x90060: 0.5}.get(room_id, 0.4)
            bamb = [r5(x * k / bamb[0]) for x in bamb] if bamb[0] else bamb
            amb = [r5(x * 0.4 / amb[0]) for x in amb] if amb[0] else amb
            notes.append('Born From a Wish: ambient forced to 0.4 grey, base ambient to %g' % k)
        elif glb not in (3, 8, 0x10):
            notes.append('Born From a Wish: ambient/base ambient keep the previous room values')

    # fog distances and factors
    fp = rec['fog_parm']
    if fp[0] != 0.0:
        fog_end, fog_start, f_far, f_near = fp
        fog_src = 'record'
    else:
        fog_end, fog_start, f_far, f_near = FOG_DEFAULT_DARK if dark else FOG_DEFAULT_LIT
        fog_src = 'default (%s)' % ('dark' if dark else 'lit')
    if ee and room_id in EE_FOG_START:
        fog_start = EE_FOG_START[room_id]
        notes.append('Enhanced Edition FogParameterFix: fog start %g' % fog_start)
    # fog / clear colour
    s = rec['spot_parm'][3]
    if s == 0.0:
        if dark:
            fog_rgb = [0, 0, 0]
            clear = [70, 70, 70, 0x7c] if (chapter == 1 and room_id in BFAW_CLEAR_ROOMS) \
                else [0, 0, 0, 0x7c]
        else:
            fog_rgb = [70, 70, 70]
            clear = [70, 70, 70, 0x40]
        fogc_src = 'default (%s)' % ('black' if dark else 'grey 70')
    else:
        mf = rec['move_fog_color']
        q = [ftol(s * mf[i]) & 0xff for i in range(4)]
        fog_rgb = q[:3]
        clear = q[:]
        if dark and chapter == 1 and room_id in BFAW_CLEAR_ROOMS and room_id != 0x100031:
            clear = [0x5a, 0x5a, 0x5a, q[3]]
        fogc_src = 'spot_parm[3] * move_fog_color'
    if ee and room_id in EE_CLEAR_COLOR:
        clear = list(EE_CLEAR_COLOR[room_id]) + clear[3:]
        notes.append('Enhanced Edition LightingFix: clear colour %s' % list(EE_CLEAR_COLOR[room_id]))
    far_clip = rec['spot_parm'][1] or (EE_FAR_CLIP_DEFAULT if ee else FAR_CLIP_DEFAULT)
    near_clip = rec['spot_parm'][2] or NEAR_CLIP_DEFAULT
    fog_particles = rec['move_fog_color'] if (rec['move_fog_color'][0] != 0.0
                                              and room_id != 0x10001d) else None

    # fog factor f(w) = A + B / w, f(start) = near/255, f(end) = far/255 (FUN_00458de0)
    fn, ff = f_near / 255.0, f_far / 255.0
    A = (ff * fog_end - fn * fog_start) / (fog_end - fog_start)
    B = (fn - ff) * fog_end * fog_start / (fog_end - fog_start)

    fl = rec['flashlight_decay']
    flash = {'color': rec['flashlight_color'][:3], 'start': fl[0], 'end': fl[2],
             'cone': fl[3], 'cos_cone': r5(math.cos(fl[3])),
             'vs_ps_color': [r5(x * K_FLASH) for x in rec['flashlight_color'][:3]]}

    def consts(level, flash_pulse=0.0):
        c19 = [r5(K_AMBIENT * (4.0 * a + 2.0 * flash_pulse + 2.0 * level * a)) for a in amb[:3]]
        c21 = [r5(K_BASEAMB * (b + 2.0 * flash_pulse + 2.0 * level * b)) for b in bamb[:3]]
        return {'level': level, 'c19_ambient': c19,
                'c20_base_color': [r5(K_BASECOLOR * x) for x in rec['base_color'][:3]],
                'c21_base_ambient': c21,
                'c22_vertex_ambient': [r5(K_VTXAMB * x) for x in rec['vertex_ambient'][:3]]}

    # light slots the map shaders see (c8..c10 = -dir, c15..c17 = colour * 0.25)
    if dark:
        slots = [par[1], par[2], None]
    else:
        slots = [par[0], par[1], par[2]]
    vs_lights = []
    for sl in slots:
        if sl is None:
            vs_lights.append({'dir': [0.0, 0.0, 0.0], 'color': [0.0, 0.0, 0.0]})
        else:
            vs_lights.append({'dir': sl['dir'], 'color': [r5(K_LIGHT * x) for x in sl['color'][:3]]})

    return {
        'dark': int(dark),
        'ambient': amb[:3], 'base_ambient': bamb[:3], 'base_color': rec['base_color'][:3],
        'vertex_ambient': rec['vertex_ambient'][:3],
        'parallel': par,
        'fog': {'start': fog_start, 'end': fog_end, 'near_factor': f_near, 'far_factor': f_far,
                'A': r5(A), 'B': r5(B), 'source': fog_src,
                'color': fog_rgb, 'color_source': fogc_src},
        'clear_color': clear,
        'clip': {'near': near_clip, 'far': far_clip},
        'fog_particle_color': fog_particles,
        'flashlight': flash,
        'shadow_color': shadow_level(rec, room_id, chapter),
        'shadow_darkness': rec['shadow_darkness'],   # 0x47bb00: 0 = no character shadows here
        'vs_lights': vs_lights,
        'vs_flashlight_off': consts(LEVEL_FLASH_OFF),
        'vs_flashlight_on': consts(LEVEL_FLASH_ON),
        'notes': notes,
    }


# ------------------------------------------------------------------ character lights (0x508a90)
def chr_lights(rec, glb, dark, map_id=None, ee=True):
    """The light list FUN_00508a90 builds from the record for characters (PS2 Chacter_Draw/light_n.c).
    Returns the parallels (index 1..3) and the Ld block in the order the PC reads it."""
    raw = rec['_raw']
    par = [rec['parallel'][k] for k in range(3)]
    d = [norm3(p['dir']) for p in par]
    raw_col = [p['color'][:3] for p in par]
    pm = rec['parallel_mode']
    keep = (1, 1, 1) if pm == 0 else (1, 1, 0) if pm == 1 else (1, 0, 0)
    gcol = [[c * keep[k] for c in raw_col[k]] for k in range(3)]
    lights = {}
    if pm == 2:
        lights[2] = (d[1], raw_col[1])
        lights[3] = (d[2], raw_col[0])
    else:
        lights[2] = (d[1], gcol[1])
        lights[3] = (d[2], gcol[2] if pm == 0 else raw_col[2])
    if not dark and (1 <= glb <= 4 or glb == 0x11):    # FUN_00494d80: outside, light 0 too
        lights[1] = (d[0], gcol[0])
    o = 0xb0
    f = lambda off, n: [r5(x) for x in struct.unpack_from('<%df' % n, raw, off)]
    spots, points, fpoints, fspots = [], [], [], []
    n = rec['spot_num']
    if glb == 0x10:
        if n == 0:
            o += 0x40                                   # an event light (FUN_0047b040), not for characters
    elif n >= 0:
        # (n < 1 ? n : 0) + 1 records: one spot whatever the count (0x508ea0)
        spots.append({'col': f(o, 4), 'sv': f(o + 16, 4), 'pos': f(o + 32, 3), 'dir': norm3(f(o + 48, 3))})
        o += 0x40
    for _ in range(max(rec['point_num'], 0)):
        points.append({'col': f(o, 4), 'start': f(o + 16, 1)[0], 'end': f(o + 20, 1)[0], 'pos': f(o + 32, 3)})
        o += 0x30
    for _ in range(max(rec['fake_point_num'], 0)):
        fpoints.append({'col': f(o, 4), 'start': f(o + 16, 1)[0], 'end': f(o + 20, 1)[0], 'pos': f(o + 32, 3)})
        o += 0x30
    for _ in range(max(rec['fake_spot_num'], 0)):
        fspots.append({'col': f(o, 4), 'cone': f(o + 16, 1)[0], 'start': f(o + 24, 1)[0], 'end': f(o + 28, 1)[0],
                       'pos': f(o + 32, 3), 'dir': norm3(f(o + 48, 3))})
        o += 0x40
    if ee and map_id in EE_SPOT_AS_POINT:
        points = [{'col': l['col'], 'start': l['sv'][2], 'end': l['sv'][3], 'pos': l['pos']} for l in spots] + points
        spots = []
    return {'parallels': lights, 'spots': spots, 'points': points, 'fake_points': fpoints, 'fake_spots': fspots}


def chr_line(name, var, glb, cl):
    v = [name, var, glb]
    for i in (1, 2, 3):
        dc = cl['parallels'].get(i)
        v += (list(dc[0]) + list(dc[1])) if dc else [0] * 6
    v += [len(cl['spots']), len(cl['points']), len(cl['fake_points']), len(cl['fake_spots'])]
    for l in cl['spots']:
        v += l['pos'] + l['dir'] + l['col'] + l['sv']
    for l in cl['points'] + cl['fake_points']:
        v += l['pos'] + l['col'] + [l['start'], l['end']]
    for l in cl['fake_spots']:
        v += l['pos'] + l['dir'] + l['col'] + [l['cone'], l['start'], l['end']]
    return ' '.join(x if isinstance(x, str) else fmt(x) for x in v)


def shadow_level(rec, room_id, chapter):
    """0x9458ac as set at the end of FUN_0047c780."""
    if rec['map_id'] == -1:
        return 50 if room_id == 0xa00e6 else 80
    if rec['shadow_color'] == -1:
        return 40 if chapter == 1 else 80
    return rec['shadow_color']


# ------------------------------------------------------------------ room names (optional)
def room_names():
    names = {}
    if os.path.exists(SH2EE_HEADER):
        import re
        for m in re.finditer(r'\b(R_[A-Z0-9_]+)\s*=\s*0x([0-9A-Fa-f]+)', open(SH2EE_HEADER).read()):
            names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def map_rooms(exe):
    """map number -> [room ids] from the u8[4] table at 0x88e210 (byte 0 is the map number)."""
    out = {}
    for r in range(0xd6):
        n = exe.read(ROOM_MAPS + 4 * r, 1)[0]
        out.setdefault(n, []).append(r)
    return out


def glb_rooms(glb):
    """Room ids FUN_00494db0 can return for this glb (sh2rooms.ROOM_TABLE)."""
    sys.path.insert(0, HERE)
    import sh2rooms
    t = sh2rooms.ROOM_TABLE.get(glb)
    if t is None:
        return {0xbc, 0xbd} if glb == 14 else ({0xbf} if glb == 15 else set())
    exact, even, default = t
    return set(exact.values()) | set(even.values()) | {default}


def pack_rooms(pack):
    rooms = []
    for area in AREAS:
        p = os.path.join(pack, 'bg', area, 'rooms.txt')
        if not os.path.exists(p):
            continue
        for line in open(p):
            name = line.strip()
            if name:
                rooms.append((area, name))
    return rooms


def variants_for(area):
    glb = AREA_GLB[area]
    if area == 'ma':
        return [('-', 1, False)]
    v = [('-', 0, False)]
    if glb in (2, 3):
        v.append(('night', 0, True))
    if glb == 6:
        v.append(('flag251', 0, True))
    if glb in (3, 8, 9):
        v.append(('bfaw', 1, False))
    return v


TXT_COLUMNS = [
    'name', 'variant', 'glb', 'map_id', 'dark',
    'amb_r', 'amb_g', 'amb_b', 'bamb_r', 'bamb_g', 'bamb_b', 'bcol_r', 'bcol_g', 'bcol_b',
    'vamb_r', 'vamb_g', 'vamb_b',
    'fog_r', 'fog_g', 'fog_b', 'fog_start', 'fog_end', 'fog_near_f', 'fog_far_f',
    'clear_r', 'clear_g', 'clear_b', 'clip_near', 'clip_far',
    'l0_dx', 'l0_dy', 'l0_dz', 'l0_r', 'l0_g', 'l0_b',
    'l1_dx', 'l1_dy', 'l1_dz', 'l1_r', 'l1_g', 'l1_b',
    'l2_dx', 'l2_dy', 'l2_dz', 'l2_r', 'l2_g', 'l2_b',
    'fl_r', 'fl_g', 'fl_b', 'fl_start', 'fl_end', 'fl_cone',
    'c19_r', 'c19_g', 'c19_b', 'c20_r', 'c20_g', 'c20_b', 'c21_r', 'c21_g', 'c21_b',
    'c22_r', 'c22_g', 'c22_b',
    'fogp_r', 'fogp_g', 'fogp_b', 'fogp_a', 'shadow',
]


def fmt(v):
    if isinstance(v, int):
        return str(v)
    return ('%.5g' % v) if v != int(v) or abs(v) >= 1e6 else str(int(v))


def txt_line(name, var, glb, map_id, res):
    vals = [name, var, glb, '%#x' % (map_id & 0xffffffff), res['dark']]
    vals += res['ambient'] + res['base_ambient'] + res['base_color'] + res['vertex_ambient']
    f = res['fog']
    vals += f['color'] + [f['start'], f['end'], f['near_factor'], f['far_factor']]
    vals += res['clear_color'][:3] + [res['clip']['near'], res['clip']['far']]
    for L in res['vs_lights']:
        vals += L['dir'] + L['color']
    fl = res['flashlight']
    vals += fl['vs_ps_color'] + [fl['start'], fl['end'], fl['cone']]
    c = res['vs_flashlight_off']
    vals += c['c19_ambient'] + c['c20_base_color'] + c['c21_base_ambient'] + c['c22_vertex_ambient']
    # fog particle colour (0x47c66e: ftol of MoveFogCol into FUN_004879c0), -1 = the record leaves it
    fp = res['fog_particle_color']
    vals += [ftol(x) for x in fp] if fp else [-1, -1, -1, -1]
    vals += [res['shadow_darkness']]      # record ShadowDarkNess, read by port/source/shadow.c
    return ' '.join(v if isinstance(v, str) else fmt(v) for v in vals)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--pack', default=DEFAULT_PACK, help='pack dir with bg/<area>/rooms.txt')
    ap.add_argument('--out', default=None, help='output dir (default: --pack)')
    ap.add_argument('--pc', action='store_true', help='leave out the Enhanced Edition fixes')
    a = ap.parse_args()
    ee = not a.pc
    out_dir = a.out or a.pack

    exe = Exe(a.exe)
    recs = read_records(exe, ee)
    names = room_names()
    m2r = map_rooms(exe)

    maps = []
    lines = []
    chr_lines = []
    for area, name in pack_rooms(a.pack):
        glb = AREA_GLB[area]
        num = int(name[len(area):])
        map_id = (glb << 16) | num
        own = glb_rooms(glb)
        rooms = [r for r in m2r.get(num, []) if r in own]
        entry = {'name': name, 'area': area, 'glb': glb, 'map_id': '%#x' % map_id,
                 'room_ids': ['%#x' % r for r in rooms] if glb not in (1, 2, 3, 4) else [],
                 'room_names': [names[r] for r in rooms if r in names] if glb not in (1, 2, 3, 4) else [],
                 'variants': {}}
        for var, chapter, flag in variants_for(area):
            rec, how = select_record(recs[glb], glb, map_id, chapter, flag, ee)
            res = resolve(rec, map_id, chapter, ee)
            res['record'] = rec['addr']
            res['record_map_id'] = rec['map_id_hex']
            res['selected_by'] = how
            res['chapter'] = chapter
            res['flag251'] = flag
            entry['variants'][var] = res
            lines.append(txt_line(name, var, glb, map_id, res))
            chr_lines.append(chr_line(name, var, glb, chr_lights(rec, glb, res['dark'], map_id, ee)))
        maps.append(entry)

    doc = {
        'source': os.path.basename(a.exe),
        'notes': 'see tools/sh2conv/LIGHTING.md',
        'enhanced_edition_fixes': ee,
        'env_table': '%#x' % ENV_TABLE,
        'record_size': REC_SIZE,
        'globals': {
            '0x945ae0': 'current DrawEnvData* (FUN_0047c780)',
            '0x945ad0': 'current map id glb<<16|map (FUN_00494ef0 byte 0)',
            '0x9455f0/0x945600/0x945610': 'parallel light directions (normalised)',
            '0x945620/0x945630/0x945640': 'parallel light colours',
            '0x945650': 'Ambient', '0x945670': 'BaseAmbient', '0x945680': 'BaseColor',
            '0x945660': 'VertexAmbient', '0x9456b0': 'flashlight colour', '0x9456e0': 'flashlight decay',
            '0x945820': 'fog colour RGB bytes', '0x945850': 'clear colour RGBA bytes',
            '0x945830/0x945834': 'fog end / start', '0x945838/0x94583c': 'fog far / near factor (0..255)',
            '0x945558/0x94554c': 'far / near clip',
            '0x1dbb2e0': 'vertex shader constants c0..c27 (SetVertexShaderConstant(0, ., 28))',
        },
        'txt_columns': TXT_COLUMNS,
        'maps': maps,
        'records': {str(g): r for g, r in recs.items()},
    }
    for rs in recs.values():
        for r in rs:
            r.pop('_raw', None)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'lighting.json'), 'w') as f:
        json.dump(doc, f, indent=1)
    with open(os.path.join(out_dir, 'chrlight.txt'), 'w') as f:
        f.write('# sh2 character lights per map (lighting.py, FUN_00508a90, see LIGHTING.md section 10): name '
                'variant glb, parallels 1..3 (dir xyz = direction the light travels, colour rgb), counts of '
                'spots points fake_points fake_spots, then spots (pos3 dir3 col4 sv4), points and fake points '
                '(pos3 col4 start end), fake spots (pos3 dir3 col4 cone start end)\n')
        for l in chr_lines:
            f.write(l + '\n')
    with open(os.path.join(out_dir, 'lighting.txt'), 'w') as f:
        f.write('# sh2 per-map lighting (tools/sh2conv/lighting.py, see LIGHTING.md). Columns: '
                + ' '.join(TXT_COLUMNS) + '\n')
        f.write('# variant "-" is the default (main scenario, flag 251 clear); others: night (town, '
                'flag 251 set), flag251 (er), bfaw (Born From a Wish). l0..l2 and fl_* are the '
                'vertex-shader light slots (dir = direction light travels, colour already x0.25); '
                'c19..c22 are the map shader constants with the flashlight off (level 0.3); fogp_* is the '
                'fog particle colour 0..255 (-1: keep the previous one); shadow is the record\'s ShadowDarkNess '
                '(0: no character shadows)\n')
        f.write('# %s\n' % ('Enhanced Edition fixes applied (lighting.py without --pc)' if ee
                            else 'unmodified PC values (lighting.py --pc)'))
        for l in lines:
            f.write(l + '\n')
    print('%d maps, %d lines, %d records -> %s' % (len(maps), len(lines),
                                                   sum(len(r) for r in recs.values()), out_dir))


if __name__ == '__main__':
    main()
