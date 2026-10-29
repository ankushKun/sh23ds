#!/usr/bin/env python3
"""Convert Silent Hill 2 (PC Director's Cut) in-engine cutscenes for sh23ds (port/source/demo.c).

  demos.py [--data <game data dir>] [--exe sh2pc.exe] [--out pack] [--only tenbou,haka_agl]
           [--no-models] [--no-voices] [--max-tex 1024]

How the PC plays a cutscene (static analysis of sh2pc.exe, see port/CUTSCENES.md)
  A stage program calls DramaDemoMain (0x4a1c80) every frame with a DramaDemo_PlayInfo record from
  the stage's data (PS2 beta Event/demoview.c names):
    +0x00 s32 demo_no        +0x04 ptr adr_dds_top   +0x08 ptr adr_anim (s16 motion ids)
    +0x0c ptr adr_msg_time ({u16 start, end} frames) +0x10 s16 msg_start (stage message number)
    +0x12 s16 voice_sd_no    +0x14 ptr adr_voice     +0x18 s32 stream_no (SE id 60000 + voice.afs file)
    +0x1c f32 stream_start (frames)                  +0x20 f32 add_pos_x   +0x24 f32 add_pos_z
  The .dds script ("dds\\0", s16 frame count at 0x10, light counts at 0x14..0x16, character count at
  0x18, then 16-byte character names such as "hhh_jms_pos") is a list of key records: u16 frame, then
  object sections (0 keys, 1 camera, 2.. lights, then characters) until 0xff, until frame 0xffff.
  Values are raw floats after a 0x10 key (a cut) and otherwise half floats added to the last raw
  value. Key 0x11 starts the next motion of every character from adr_anim; each motion id selects a
  12-byte record {id, frames, speed, start, end, loop} and a byte offset into the character's .anm in
  per-kind tables of the exe (DramaDemoAnimationStart 0x49f7e0 and the setters it calls).

What this writes
  <out>/demo/<key>.dem       one per PlayInfo found in the exe (key: the .dds name, plus a suffix
                             when two programs share one script), format DEM1 below
  <out>/demo/demos.txt       key, PlayInfo address, program, script, frames, characters, voice
  <out>/chr/<folder>/<model>.s3c + tex/   models the scenes need (chr.py's S3C v2; models that are
                             already in the pack, like James' hll_jms, are reused)
  <out>/sound/voice/<n>.adx  voice.afs file n (the scene's stream_no - 60000), copied as is (a
                            WAV from the Audio Enhancement Pack's voice.afs is encoded to ADX)

DEM1 (little-endian)
  char[4] "DEM1", u32 version (2), s32 demo_no, u32 total_frames, u32 n_chars, u32 n_anim, u32 n_msgs,
  s32 msg_start, s32 voice (voice.afs file, -1 none), f32 voice_start (frames), f32 add_x, add_z,
  f32 fov_scale_init, fov_scale_play (DramaDemoInit / DdsPlay scale scr_z for some demo_no),
  u32 ofs_dds, size_dds, ofs_anim, ofs_msgs, ofs_chars                       (76 bytes)
  dds        the original script bytes
  anim       s16 x n_anim: motion ids, n_chars per key 0x11 (adr_anim)
  msgs       {u16 start, u16 end} x n_msgs (frames of the elapsed timer)
  chars      n_chars x 64 bytes: char name[16] (script name), dir[16] (pack chr folder), model[16],
             u16 kind, u16 id (anim_info), s16 anim_lo, anim_hi (the ids this character takes),
             u32 n_motions, u32 ofs_motions
  motion     {s16 id, u16 frames, s16 speed, u8 loop, u8 pad, u32 ofs_data, u32 ofs_cls} x n_motions
             (ofs_cls: the motion's .cls track block, see S3K below; 0 for none)
  data       u32 frame_ofs[frames + 1] (relative to the bytes after the table), then the .anm bytes
             of the motion verbatim (same encoding chr.py / sh2mdl.load_anm read)
  cls        the original .cls block of the motion (clani.c Header and cluster keys)

S3K1 (pack/chr/<folder>/<model>.s3k, models with face clusters)
  char[4] "S3K1", u32 n_clusters, u32 n_verts, u32 n_entries
  u16 verts[n_verts] (S3C vertex indices the clusters move), padded to 4
  {u32 first, u32 count} x n_clusters (entries of each cluster)
  {u16 slot (index into verts), u16 pad, f32 dx, dy, dz (model space at weight 1)} x n_entries

VOC1 (pack/demo/voice_<program>.voc, SubtitlesExec calls: a voice with subtitles and no script)
  char[4] "VOC1", s32 voice (voice.afs file, -1 none), s32 first stage message (-1 none), u32 n,
  {u16 start, u16 end} x n (frames)

Born From a Wish scripts (data/demo2) take the DC models of data/chr2 first; those go to pack
folders with a 2 (item2, mar2).
"""
import argparse
import bisect
import glob
import json
import os
import re
import shutil
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ee  # noqa: E402
import sh2mdl  # noqa: E402
from chr import convert_model, textures_of_primitives  # noqa: E402
from convert import encode_textures, pow2_floor  # noqa: E402
from sound import Exe, afs_files, wav_to_adx  # noqa: E402
import host  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_DATA = os.path.join(ROOT, 'Silent Hill 2', 'data')
DEFAULT_EXE = os.path.join(ROOT, 'Silent Hill 2', 'sh2pc.exe')
DEFAULT_OUT = os.path.join(ROOT, 'pack')
FUNCS_TSV = os.path.join(ROOT, 'sh2-3ds-port-kit', 'data', 'dc', 'ghidra_functions_fixed.tsv')
STAGE_TABLE = os.path.join(ROOT, 'sh2-3ds-port-kit', 'data', 'dc', 'stage_table.json')

DRAMA_DEMO_MAIN = 0x4a1c80
SUBTITLES_EXEC = 0x49fe30         # voice and subtitles without a script
INLINE_VOICES = [(0x588e60, 0x8d82e8, 0x20, 60107)]   # (program, times, first message, stream)
LOAD_FILE = 0x492a50              # loads a file list entry into a new buffer
DEMO_BUFFER = 0x1dbec40          # where the stage programs load a script (FUN_00448f30)
# PlayInfos a program plays with more than one script: it loads one of them into DEMO_BUFFER first
# and the matching above keeps the best fit, so the others are written here as well under their
# own names. The music box (0x57c470) loads org_b for the right order and org_a for any other.
EXTRA_SCRIPTS = {0x8cea6c: ['demo/org/org_a.dds']}
ANIM_INFO = 0x891908             # DramaDemo_AnimInfo[79]: name[32], kind, id, start, end
ANIM_INFO_COUNT = 84            # the PS2 beta has 79; the DC adds Born From a Wish's props

# Motion tables per character kind, from the setters DramaDemoAnimationStart calls:
# kind -> (first id, record table {id, frames, speed, start, end, loop}, .anm offset table,
#          .cls offset table or None)
MOTION = {
    0x100: (950, 0x6d8398, 0x8bd158),    # FUN_0053b120 lll / hll James
    0x101: (950, 0x6d8398, 0x8bd158),
    0x102: (1000, 0x6e8288, 0x8be8d8, 0x8bebc8),   # FUN_00540f60 hhl / hhh James
    0x103: (1000, 0x6e8288, 0x8be8d8, 0x8bebc8),
    0x104: (2500, 0x6e6780, 0x8be5d8, 0x8be680),   # FUN_00540a20 Laura
    0x105: (40, 0x6d1ca0, None),         # FUN_0053a090 lll Maria: offsets are filled at run time
    0x106: (3000, 0x6e74a8, 0x8be728, 0x8be800),   # FUN_00540c60 hhh Maria
    0x107: (3500, 0x6e5aa0, 0x8be4d8, 0x8be558),   # FUN_00540730 Angela
    0x108: (4500, 0x6e4e08, 0x8be3f8, 0x8be468),   # FUN_005405c0 hhh Eddie
    0x109: (4000, 0x6d4fd0, 0x8bd098, 0x8bd0f0),   # FUN_0053a920 Mary
    0x10a: (4100, 0x6e4268, 0x8be39c, 0x8be3c8),   # FUN_005404e0 Mary (letter)
    0x10b: (55, 0x6e22c8, 0x8be380),     # FUN_005400d0 boat
    0x10d: (4900, 0x6e3728, 0x8be390),   # FUN_005402a0 dog
    0x200: (6000, 0x6e16f8, 0x8bdcb8),   # FUN_0053ddc0 Lying Figure
    0x201: (6100, 0x6df0f8, 0x8bdc5c),   # FUN_0053cef0 Mannequin
    0x203: (6500, 0x6de3a8, 0x8bdc2c),   # FUN_0053ca60 Pyramid Head (spear)
    0x204: (6600, 0x6e09d0, 0x8bdc98),   # FUN_0053d8d0 Abstract Daddy
    0x205: (6800, 0x6dcb28, 0x8bdbfc),   # FUN_0053bfb0 lll Eddie
    0x206: (6700, 0x6dbe08, 0x8bdba8),   # FUN_0053bb20 Mary boss
    0x208: (6300, 0x6dfd00, 0x8bdc7c),   # FUN_0053d3f0 Pyramid Head
    0x209: (6350, 0x6dd718, 0x8bdc04),   # FUN_0053c580 Pyramid Heads (end)
}
ITEM_MOTION = (10000, 0x6da4f8, 0x8bd740)   # FUN_0053b820: kinds 0x4xx (items, doors, props)

# DdsPlay (0x4a0f30) / DramaDemoInit (0x4a0b30) scale scr_z for these demo numbers
FOV_PLAY = {7: 0x89262c, 0x5a: 0x892630, 0x5d: 0x892630, 100: 0x892634}
FOV_INIT = {7: 0x892628}


def half(h):
    """DdsReadFloat2 (0x49f750): half float without zero / infinity cases."""
    v = ((((h & 0xffff8000) << 3) | (h & 0x3ff) | ((h & 0x7c00) + 0x1c000)) << 13) & 0xffffffff
    return struct.unpack('<f', struct.pack('<I', v))[0]


# ------------------------------------------------------------------ .dds scripts

def parse_dds(d):
    """Walks a script like DdsPlay; returns header facts and the frames of the 0x11 keys."""
    if d[:3] != b'dds':
        raise ValueError('not a dds script')
    total = struct.unpack_from('<h', d, 0x10)[0]
    lights = d[0x14] + d[0x15] + d[0x16]
    nchr = d[0x18]
    p = 0x19
    names = []
    for _ in range(nchr):
        names.append(d[p:p + 16].split(b'\0')[0].decode('latin-1'))
        p += 16
    starts = []
    while True:
        fr = struct.unpack_from('<H', d, p)[0]
        if fr == 0xffff:
            break
        obj = d[p + 2]
        p += 3
        raw = False
        while obj != 0xff:
            if obj == 0:
                while True:
                    c = d[p]
                    p += 1
                    if c == 0x0b:
                        break
                    if c == 0x10:
                        raw = True
                    if c == 0x11:
                        starts.append(fr)
            elif obj == 1:
                while True:
                    c = d[p]
                    p += 1
                    if not 3 <= c <= 7:
                        break
                    p += {3: 12 if raw else 6, 4: 12 if raw else 6, 5: 6, 6: 2, 7: 4}[c]
            elif obj - 2 < lights:
                while True:
                    c = d[p]
                    p += 1
                    if not 1 <= c <= 10 or c in (6, 7):
                        break
                    p += {1: 0, 2: 0, 3: 12 if raw else 6, 4: 12 if raw else 6, 5: 6, 8: 6, 9: 4, 10: 4}[c]
            else:
                if obj - 2 - lights >= nchr:
                    raise ValueError('object %d out of range at %#x' % (obj, p))
                while True:
                    c = d[p]
                    p += 1
                    if c not in (1, 2, 3):
                        break
                    if c == 3:
                        p += 12 if raw else 6
            obj = d[p]
            p += 1
    return dict(total=total, names=names, starts=starts)


# ------------------------------------------------------------------ tables in the exe

def anim_info(exe):
    out = []
    for i in range(ANIM_INFO_COUNT):
        r = exe.read(ANIM_INFO + 0x28 * i, 0x28)
        name = r[:32].split(b'\0')[0].decode('latin-1')
        kind, id_, lo, hi = struct.unpack_from('<4h', r, 32)
        out.append(dict(name=name, kind=kind & 0xffff, id=id_, lo=lo, hi=hi))
    return out


def motion_table(kind):
    t = MOTION.get(kind) or (ITEM_MOTION if kind & 0xff00 == 0x400 else None)
    return (t + (None,))[:4] if t else (None, None, None, None)


def motion_of(exe, kind, anim, with_cls=False):
    """(record, .anm byte offset or None) for a motion id, as the kind's setter finds them; with_cls
    adds the .cls byte offset (None for kinds without cluster animation)."""
    base, rec, off, cls = motion_table(kind)
    if base is None or anim < base:
        return (None, None, None) if with_cls else (None, None)
    r = struct.unpack('<6h', exe.read(rec + 12 * (anim - base), 12))
    # the PC indexes the table without checking the id (lll Maria's motion 46 says 45), so only
    # check that the record looks like one
    if not (0 < r[1] < 20000 and 0 <= r[3] <= r[4] and abs(r[0] - anim) <= 1):
        return (None, None, None) if with_cls else (None, None)
    o = exe.u32(off + 4 * (anim - base)) if off else None
    if with_cls:
        return r, o, exe.u32(cls + 4 * (anim - base)) if cls else None
    return r, o


def functions():
    f = []
    for line in open(FUNCS_TSV):
        p = line.rstrip('\n').split('\t')
        if p[0] != 'entry':
            f.append((int(p[3], 16), int(p[4], 16)))
    f.sort()
    return f


def playinfo_calls(exe):
    """Every `push imm32; call DramaDemoMain` in the code: (call address, PlayInfo, function)."""
    base = code_start(exe)
    funcs = functions()
    starts = [a for a, _ in funcs]
    out = []
    for va in exe.calls_to(DRAMA_DEMO_MAIN):
        # the PlayInfo is pushed as an immediate, or moved into EAX on two branches and pushed;
        # take every immediate in the instructions just before the call that looks like one
        pis = []
        lo = max(base, va - 40)
        pre = exe.read(lo, va - lo)
        for k in range(len(pre) - 4):
            if pre[k] in (0x68, 0xb8):
                v = struct.unpack_from('<I', pre, k + 1)[0]
                if 0x7b0000 <= v < 0x900000 and plausible_playinfo(exe, v) and v not in pis:
                    pis.append(v)
        i = bisect.bisect_right(starts, va) - 1
        fn = funcs[i] if i >= 0 and funcs[i][0] <= va <= funcs[i][1] else None
        lists = []
        if fn:
            seg = exe.read(fn[0], fn[1] - fn[0] + 1)
            for k in re.finditer(b'\x68', seg):
                if k.start() + 5 > len(seg):
                    continue
                v = struct.unpack_from('<I', seg, k.start() + 1)[0]
                if 0x600000 < v < 0x900000:
                    try:
                        s = exe.cstr(exe.u32(v))
                    except ValueError:
                        continue
                    if s.startswith('data/demo') and s.endswith('.dds'):
                        lists.append(s)
        for pi in pis:
            out.append((va, pi, fn[0] if fn else None, lists))
    return out


def plausible_playinfo(exe, a):
    try:
        v = struct.unpack('<iIIIhhIifff', exe.read(a, 40))
    except ValueError:
        return False
    def ptr(p):
        return p == 0 or 0x600000 <= p < 0x2000000
    return (0 <= v[0] < 200 and ptr(v[1]) and ptr(v[2]) and ptr(v[3]) and
            (v[7] == 0 or 60000 <= v[7] < 61000) and 0 <= v[8] < 20000 and
            abs(v[9]) < 1e6 and abs(v[10]) < 1e6)


def read_playinfo(exe, a):
    v = struct.unpack('<iIIIhhIifff', exe.read(a, 40))
    return dict(addr=a, demo_no=v[0], dds=v[1], anim=v[2], msgt=v[3], msg_start=v[4], voice_sd=v[5],
                stream=v[7], stream_start=v[8], add_x=v[9], add_z=v[10])


# ------------------------------------------------------------------ matching a PlayInfo to its script

def char_entry(info, name):
    for e in info:
        if e['name'] == name:
            return e
    return None


_models = {}


def bones_of(data, name, dc=False, stage=None):
    """(bone count, root flags) of a script character's model, cached."""
    k = (name, dc)
    if k not in _models:
        folder, model, src = model_for(data, name, dc)
        _models[k] = None
        if src:
            m = sh2mdl.load_mdl(src)
            _models[k] = (m.bone_count, list(m.parents == 255))
    return _models[k]


def anm_candidates(data, folder_dir, cname, stage=None):
    """.anm files a character's motions can be in, most likely first."""
    base = cname[:-4] if cname.endswith('_pos') else cname
    base = {'i_keyelevator_p': 'i_keyelevator'}.get(base, base)
    plain = re.sub(r'_[12]$', '', base)
    c = []
    # the in-game James kinds (lll / hll) play their demo motions from the stage's own file, which
    # the stage loads for James (jms_stg_<stage>.anm; EvProgPapaAttackAngela's motion 969 is there)
    if base in ('lll_jms', 'hll_jms') and stage:
        c.append(os.path.join(data, 'chr', 'jms', 'jms_stg_%s.anm' % stage))
    c += [os.path.join(folder_dir, base + '.anm'), os.path.join(folder_dir, plain + '.anm')]
    for chr_dir in ('chr', 'chr2'):
        folder = model_for(data, cname)[0]
        if folder:
            c.append(os.path.join(data, chr_dir, folder.rstrip('2'), plain + '.anm'))
    # scripts can use motions another folder holds (boat2 plays from boat1/hhh_jms.anm)
    for demo_dir in ('demo', 'demo2'):
        c += sorted(glob.glob(os.path.join(data, demo_dir, '*', base + '.anm')))
        c += sorted(glob.glob(os.path.join(data, demo_dir, '*', plain + '.anm')))
    if base in ('lll_jms', 'hll_jms'):
        c += sorted(glob.glob(os.path.join(data, 'chr', 'jms', 'jms_*.anm')))
    return [x for x in dict.fromkeys(c) if os.path.exists(x)]


_bounds = {}


def motion_bounds(exe, kind):
    """Every .anm offset in a kind's motion table: a motion ends where another starts (or at EOF)."""
    t = motion_table(kind)
    if not t[2]:
        return set()
    if t not in _bounds:
        out, i = set(), 0
        while i < 2000:
            r = struct.unpack('<6h', exe.read(t[1] + 12 * i, 12))
            if r[0] != t[0] + i and i > 0 and r[0] != 0:
                break
            out.add(exe.u32(t[2] + 4 * i))
            i += 1
        _bounds[t] = out
    return _bounds[t]


_anm_cache = {}


def anm_bytes(path):
    if path not in _anm_cache:
        _anm_cache[path] = ee.read(path)
    return _anm_cache[path]


def valid_ids(exe, info, pi, limit=64):
    """How many motion ids at the start of adr_anim name a real motion (the list has no count)."""
    k = 0
    while k < limit:
        a = struct.unpack('<h', exe.read(pi['anim'] + 2 * k, 2))[0]
        e = next((x for x in info if x['lo'] <= a <= x['hi']), None)
        if e is None or motion_of(exe, e['kind'], a)[0] is None:
            break
        k += 1
    return k


def score_script(exe, info, pi, sc):
    """How well a script fits a PlayInfo's motion list (lower is better), None if it cannot be it."""
    ents = [char_entry(info, n) for n in sc['names']]
    if any(e is None for e in ents):
        return None
    n = len(sc['starts']) * len(ents)
    if not n:
        return 0.0 if pi['anim'] == 0 else None
    if not pi['anim']:
        return 5.0                          # DramaDemoAnimationStart does nothing without a list
    ids = struct.unpack('<%dh' % n, exe.read(pi['anim'], 2 * n))
    # a list longer than the script uses means a script with more characters or keys
    err = 0.0 if valid_ids(exe, info, pi, n + 1) == n else 1000.0
    for s, start in enumerate(sc['starts']):
        end = sc['starts'][s + 1] if s + 1 < len(sc['starts']) else sc['total']
        for a in ids[s * len(ents):(s + 1) * len(ents)]:
            e = next((x for x in info if x['lo'] <= a <= x['hi']), None)
            if e is None or not any(x['kind'] == e['kind'] and x['id'] == e['id'] for x in ents):
                return None
            rec, off = motion_of(exe, e['kind'], a)
            if rec is None:
                return None
            if not rec[5]:                  # a one-shot motion should last about until the next key
                err += min(abs(rec[1] - (end - start)), 600) / len(ids)
    return err


# ------------------------------------------------------------------ .anm motions

def anm_frames(d, off, n, nbones, roots):
    """Byte offsets of n frames from off (sh2mdl.load_anm's layout), None if it runs off the end."""
    per = (nbones + 7) & ~7
    p, out = off, []
    for _ in range(n):
        out.append(p)
        for blk in range(per // 8):
            if p + 4 > len(d):
                return None
            h = struct.unpack_from('<I', d, p)[0]
            p += 4
            for j in range(8):
                t = (h >> (4 * j)) & 7
                bone = blk * 8 + j
                root = bone < nbones and roots[bone]
                if t == 1:
                    p += 6
                elif t == 2:
                    p += (12 if root else 6) + 6
                elif t == 3:
                    p += 16
                elif t in (4, 5):
                    p += (12 if root else 6) + 16
                elif t == 6:                       # two translations (0x54fc67), then the tail
                    p += (24 if root else 12) + 16
        if p > len(d):
            return None
    out.append(p)
    return out


def model_for(data, name, dc=False):
    """Script character name -> (pack chr folder, model name, .mdl path). Born From a Wish scripts
    (data/demo2) take the DC models of data/chr2 first; those go to <folder>2 in the pack."""
    n = name[:-4] if name.endswith('_pos') else name
    n = {'i_keyelevator_p': 'i_keyelevator'}.get(n, n)
    n = re.sub(r'_[12]$', '', n)                     # mkn_1, ike_2, oni_1, i_mtablet_2
    for chr_dir in (('chr2', 'chr') if dc else ('chr',)) + (('chr2',) if not dc else ()):
        suffix = '2' if chr_dir == 'chr2' else ''
        for folder in ('jms', 'mar', 'edi'):
            path = os.path.join(data, chr_dir, folder, n + '.mdl')
            if n.endswith('_' + folder) and os.path.exists(path):
                return folder + suffix, n, path
        path = os.path.join(data, chr_dir, n, n + '.mdl')
        if os.path.exists(path):
            return n + suffix, n, path
        path = os.path.join(data, chr_dir, 'item', n + '.mdl')
        if os.path.exists(path):
            return 'item' + suffix, n, path
    return None, None, None


def cluster_file(m, mdl_path, src, out_path):
    """The model's face clusters as S3K (see the top of this file) for the S3C vertex order src."""
    d = ee.read(mdl_path)                            # the file load_mdl read (ee.py)
    B = 64
    u = lambda o: struct.unpack_from('<I', d, B + o)[0]   # noqa: E731
    n_nodes, n_clusters, n_maps = u(0x44), u(0x4c), u(0x98)
    if not n_nodes or not n_clusters:
        return 0
    node_of = {}                                     # .mdl opaque vertex -> cluster node
    for i in range(n_maps):
        a, t, c = struct.unpack_from('<3H', d, B + u(0xa0) + 6 * i)
        for k in range(c):
            node_of[t + k] = a + k
    # the vertices a node feeds, by S3C index, and the bone each .mdl vertex is placed with
    vt = np.frombuffer(d, dtype=[('p', '<f4', 3), ('w', '<f4', 4), ('n', '<i2', 3), ('pad', '<u2'),
                                 ('uv', '<f4', 2), ('b', 'u1', 4)], count=u(128), offset=B + u(132))
    owner = {}
    for k, pr in enumerate(m.primitives):
        tris = m.vertices['groups'].get(pr.start)
        if tris is not None:
            for vi in np.unique(tris):
                owner[int(vi)] = pr.bone_indices[vt['b'][vi, 0]]
    verts = sorted(i for i, vi in enumerate(src) if vi in node_of)
    if not verts:
        return 0
    slot = {v: i for i, v in enumerate(verts)}
    by_node = {}
    for v in verts:
        by_node.setdefault(node_of[src[v]], []).append(v)
    ranges, entries = [], []
    for c in range(n_clusters):
        n, eo = struct.unpack_from('<II', d, B + u(0x50) + 8 * c)
        first = len(entries)
        for x, y, z, node in np.frombuffer(d, '<i2', 4 * n, B + eo).reshape(-1, 4).tolist():
            for v in by_node.get(node, []):
                R = m.initial[owner[src[v]]][:3, :3]
                dx, dy, dz = R @ (np.array([x, y, z], float) / 16.0)
                entries.append(struct.pack('<HH3f', slot[v], 0, dx, dy, dz))
        ranges.append(struct.pack('<II', first, len(entries) - first))
    vb = struct.pack('<%dH' % len(verts), *verts)
    vb += b'\0' * (-len(vb) % 4)
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4sIII', b'S3K1', n_clusters, len(verts), len(entries)) + vb +
                b''.join(ranges) + b''.join(entries))
    return n_clusters


def convert_model_files(pack, folder, model, mdl_path, max_tex):
    """chr.py's model conversion (S3C v2 and its textures) into pack/chr/<folder>/, plus the face
    clusters (<model>.s3k) when the model has any. Returns (model, cluster count)."""
    out = os.path.join(pack, 'chr', folder)
    dst = os.path.join(out, model + '.s3c')
    m = sh2mdl.load_mdl(mdl_path)
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    os.makedirs(os.path.join(out, 'png'), exist_ok=True)
    tex_of_prim, tex_alpha, jobs = textures_of_primitives(m), {}, []
    for tid in sorted(set(tex_of_prim.values())):
        t3x = f'chr/{folder}/tex/{tid:08x}.t3x'
        rgba = sh2mdl.texture_rgba(m, tid)
        tex_alpha[tid] = bool((rgba[:, :, 3] < 250).any())
        if os.path.exists(os.path.join(pack, t3x)) or os.path.exists(dst):
            continue                                 # shared with a model converted earlier
        h, w = rgba.shape[:2]
        tw, th = max(8, min(max_tex, pow2_floor(w))), max(8, min(max_tex, pow2_floor(h)))
        img = Image.fromarray(rgba, 'RGBA')
        if (tw, th) != img.size:
            img = img.resize((tw, th), Image.LANCZOS)
        png = f'chr/{folder}/png/{tid:08x}.png'
        img.save(os.path.join(pack, png))
        jobs.append((png, t3x, tex_alpha[tid]))
    encode_textures(jobs, pack)
    # the vertex order is the same for an S3C chr.py wrote earlier (it does not depend on textures),
    # so the cluster file can be made for models that are already in the pack too
    src = []
    convert_model(m, dst if not os.path.exists(dst) else dst + '.tmp', tex_of_prim, tex_alpha, src)
    if os.path.exists(dst + '.tmp'):
        os.remove(dst + '.tmp')
    n = cluster_file(m, mdl_path, src, os.path.join(out, model + '.s3k'))
    return m, n


def smoothness(d, offs, nbones):
    """Mean frame-to-frame change of the first rotation values; small for a real motion."""
    vals = []
    for o in offs[:min(len(offs) - 1, 20)]:
        h = struct.unpack_from('<I', d, o)[0]
        t = h & 7
        vals.append(struct.unpack_from('<3h', d, o + 4) if t == 1 else (0, 0, 0))
    if len(vals) < 2:
        return 0
    a = np.array(vals, dtype=float)
    return float(np.abs(np.diff(a, axis=0)).mean())


def cls_block(path, off, n_clusters):
    """The cluster animation block at off of a .cls file, None if it is not one for this model."""
    if off is None or not n_clusters or not os.path.exists(path):
        return None
    d = anm_bytes(path)
    if off + 16 > len(d) or struct.unpack_from('<I', d, off)[0] not in (0x29843918, 0x29853918):
        return None
    if struct.unpack_from('<H', d, off + 8)[0] != n_clusters:
        return None
    end = off + 16 + 4 * n_clusters
    for i in range(n_clusters):
        c = off + struct.unpack_from('<I', d, off + 16 + 4 * i)[0]
        end = max(end, c + 2 + 4 * struct.unpack_from('<H', d, c)[0])
    return d[off:end]


def build_motions(exe, data, folder_dir, ent, cname, ids, m, stage=None, n_clusters=0):
    """Motion records, .anm slices and .cls blocks for one character; ids are the motion ids it is
    given."""
    roots = list(m.parents == 255)
    nb = m.bone_count
    files = [(c, anm_bytes(c)) for c in anm_candidates(data, folder_dir, cname, stage)]
    out = []
    seq = 0                                          # fallback: motions back to back in id order
    bounds = motion_bounds(exe, ent['kind'])
    for a in sorted(set(ids)):
        rec, off, cls_off = motion_of(exe, ent['kind'], a, with_cls=True)
        if rec is None:
            print('    %s: no motion record for %d' % (cname, a))
            continue
        n = rec[1]                                   # stored frames; start..end is what plays
        best = None
        for rank, (path, d) in enumerate(files):
            for o in ([off] if off is not None else []) + [seq]:
                offs = anm_frames(d, o, n, nb, roots)
                if offs is None:
                    continue
                end = offs[-1]
                # a real motion ends at the end of the file or where another motion of the table starts
                fits = len(d) - end < 4 or any(abs(end - b) < 4 for b in bounds if b > o)
                key = (0 if fits else 1, rank, smoothness(d, offs, nb))
                if best is None or key < best[0]:
                    best = (key, path, d, offs)
        if best is None:
            print('    %s: motion %d (%d frames) not found in %s' % (cname, a, n, [os.path.basename(p) for p, _ in files]))
            continue
        key, path, d, offs = best
        if key[0]:
            print('    %s: motion %d: no file ends it cleanly, using %s' % (cname, a, os.path.relpath(path, data)))
        seq = offs[-1] + (offs[-1] & 1)
        last = max(0, min(rec[4], n - 1))
        offs = offs[:last + 2]
        blob = d[offs[0]:offs[-1]]
        table = struct.pack('<%dI' % len(offs), *[o - offs[0] for o in offs])
        # the face clusters' track for this motion, in the .cls next to the .anm it came from
        cls = cls_block(os.path.splitext(path)[0] + '.cls', cls_off, n_clusters)
        out.append((a, last + 1, rec[2], rec[5], table + blob, cls))
    return out


# ------------------------------------------------------------------ output

def write_dem(path, pi, sc_bytes, sc, ids, msgs, chars, voice, fov_init, fov_play):
    head_len = 76
    body = bytearray()
    ofs_dds = head_len
    body += sc_bytes
    body += b'\0' * (-len(body) % 4)
    ofs_anim = head_len + len(body)
    body += struct.pack('<%dh' % len(ids), *ids)
    body += b'\0' * (-len(body) % 4)
    ofs_msgs = head_len + len(body)
    for s, e in msgs:
        body += struct.pack('<HH', s, e)
    ofs_chars = head_len + len(body)
    char_recs_len = 64 * len(chars)
    motion_area = bytearray()
    recs = bytearray()
    mot_base = ofs_chars + char_recs_len
    for c in chars:
        tab = bytearray()
        data_blobs = bytearray()
        tab_len = 16 * len(c['motions'])
        for (a, n, speed, loop, blob, cls) in c['motions']:
            off = mot_base + len(motion_area) + tab_len + len(data_blobs)
            data_blobs += blob
            data_blobs += b'\0' * (-len(data_blobs) % 4)
            cls_ofs = 0
            if cls:
                cls_ofs = mot_base + len(motion_area) + tab_len + len(data_blobs)
                data_blobs += cls
                data_blobs += b'\0' * (-len(data_blobs) % 4)
            tab += struct.pack('<hHhBxII', a, n, speed, 1 if loop else 0, off, cls_ofs)
        recs += struct.pack('<16s16s16sHHhhII', c['name'].encode()[:15], (c['dir'] or '').encode()[:15],
                            (c['model'] or '').encode()[:15], c['kind'], c['id'] & 0xffff, c['lo'], c['hi'],
                            len(c['motions']), mot_base + len(motion_area))
        motion_area += tab + data_blobs
    body += recs + motion_area
    head = struct.pack('<4sIiIIIIiifffffIIIII', b'DEM1', 2, pi['demo_no'], sc['total'], len(chars), len(ids),
                       len(msgs), pi['msg_start'], voice, pi['stream_start'], pi['add_x'], pi['add_z'],
                       fov_init, fov_play, ofs_dds, len(sc_bytes), ofs_anim, ofs_msgs, ofs_chars)
    assert len(head) == head_len
    with open(path, 'wb') as f:
        f.write(head + body)
    return head_len + len(body)


def code_start(exe):
    """The code section's address. The code is searched with exe.calls_to and exe.find_all, and
    the bytes near what they find are read with exe.read, so pcexe.py sees every byte used."""
    return exe.secs[0][0]


def stage_programs(exe):
    """Program function -> stage name ("prison_n"), from the ev_prog tables (Stage_Data + 8)."""
    out = {}
    for st in json.load(open(STAGE_TABLE)):
        tab = exe.u32(int(st['addr'], 16) + 8)
        name = st['ps2_enum'][4:].lower()
        for i in range(128):
            try:
                f = exe.u32(tab + 4 * i) if tab else 0
            except ValueError:
                break
            if i and not 0x400000 <= f < 0x600000:
                break
            if f:
                out.setdefault(f, name)
    return out


def buffer_scripts(exe, data):
    """Stage buffers the programs load scripts into: FUN_00492a50(list, flags) with the result
    stored by `mov [buffer], eax` (0x593dc4: hei_lau_b.dds into 0x1d865a8)."""
    out = {}
    for va in exe.calls_to(LOAD_FILE):
        # the bytes from 15 before the call to 28 after it: the pushes and the store that follows
        code, o = exe.read(va - 15, 43), 15
        lst = next((struct.unpack_from('<I', code, k + 1)[0] for k in range(o - 5, o - 16, -1)
                    if code[k] == 0x68 and 0x600000 < struct.unpack_from('<I', code, k + 1)[0] < 0x900000), None)
        if lst is None:
            continue
        k = code.find(b'\xa3', o + 5, o + 24)
        if k < 0:
            continue
        buf = struct.unpack_from('<I', code, k + 1)[0]
        try:
            path = exe.cstr(exe.u32(lst))
        except ValueError:
            continue
        if path.endswith('.dds'):
            out.setdefault(buf, []).append(os.path.normpath(os.path.join(data, '..', path)))
    return out


def playinfo_buffer(exe, pa):
    """The stage buffer a program copies into PlayInfo.adr_dds_top before playing it, or None:
    `mov reg, [buffer]` ... `mov [PlayInfo + 4], reg` (0x5948a1 copies 0x1d865a0 through edx)."""
    base = code_start(exe)
    for va in exe.find_all(struct.pack('<I', pa + 4)):
        # the 40 bytes before the address: the store it is part of and the load before that
        lo = max(base, va - 40)
        code, i = exe.read(lo, va - lo), va - lo
        reg = 0 if code[i - 1] == 0xa3 else (code[i - 1] >> 3) & 7 if code[i - 2] == 0x89 and code[i - 1] & 0xc7 == 5 else None
        for k in range(i - 6, max(0, i - 40), -1) if reg is not None else ():
            load = None
            if code[k] == 0xa1 and reg == 0:
                load = struct.unpack_from('<I', code, k + 1)[0]
            elif code[k] == 0x8b and code[k + 1] & 0xc7 == 5 and (code[k + 1] >> 3) & 7 == reg:
                load = struct.unpack_from('<I', code, k + 2)[0]
            if load is not None:
                if 0x1d80000 <= load < 0x1e00000 and load != DEMO_BUFFER:
                    return load
                break
    return None


def voice_sequences(exe, programs):
    """SubtitlesExec (0x49fe30) calls: (program, times, first message, voice stream)."""
    funcs = functions()
    starts = [x for x, _ in funcs]
    out = []
    for va in exe.calls_to(SUBTITLES_EXEC):
        seg = exe.read(va - 28, 28)
        imm = [(k, struct.unpack_from('<I', seg, k + 1)[0]) for k in range(len(seg) - 4) if seg[k] == 0x68]
        times = next((v for k, v in reversed(imm) if 0x7b0000 <= v < 0x900000), 0)
        stream = next((v for k, v in imm if 60000 <= v < 61000), 0)
        kt = next((k for k, v in reversed(imm) if v == times), None)
        msg = struct.unpack('<b', seg[kt - 1:kt])[0] if kt and seg[kt - 2] == 0x6a else 0
        i = bisect.bisect_right(starts, va) - 1
        fn = funcs[i][0] if i >= 0 else None
        out.append((fn, times, msg, stream))
    # programs that run SubtitlesManager's loop inline instead of calling SubtitlesExec: the radio
    # quiz in the otherworld hospital elevator (0x588e60 EvProgElevatorButton step 0x30 starts
    # stream 0xeacb, the times at 0x8d82e8 show stage messages from 0x20)
    out += [x for x in INLINE_VOICES if x[0] not in {o[0] for o in out}]
    return out


def copy_voice(afs, data, out, voice, no_voices):
    """voice.afs file -> pack/sound/voice/<n>.adx; returns its length in frames (30 a second)."""
    if voice < 0 or no_voices:
        return 0.0, afs
    if afs is None:
        afs = afs_files(ee.file(os.path.join(data, 'sound', 'adx', 'voice', 'voice.afs')))
    f, toc = afs
    if voice >= len(toc):
        return 0.0, afs
    o, n = toc[voice]
    f.seek(o)
    blob = f.read(n)
    if blob[:4] == b'RIFF':
        # the Audio Enhancement Pack's voice.afs holds most lines as PCM WAV (the looping ones stay
        # ADX); the port streams ADX, so they are encoded to it here
        blob = wav_to_adx(blob)
    rate, samples = struct.unpack('>II', blob[8:16])
    voice_dir = os.path.join(out, 'sound', 'voice')
    os.makedirs(voice_dir, exist_ok=True)
    dst = os.path.join(voice_dir, '%d.adx' % voice)
    if not os.path.exists(dst) or os.path.getsize(dst) != len(blob):
        with open(dst, 'wb') as g:
            g.write(blob)
    return samples / rate * 30.0, afs


def read_script(path):
    d = open(path, 'rb').read()
    return d, parse_dds(d)


def times_list(exe, addr, limit):
    """{start, end} frame pairs; the PC lists have no count, so stop at 0xffff or where they stop
    rising."""
    out, prev = [], -1
    for i in range(64 if addr else 0):
        st, en = struct.unpack('<HH', exe.read(addr + 4 * i, 4))
        if st == 0xffff or en < st or st < prev or en > limit:
            break
        out.append((st, en))
        prev = st
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--only', help='comma separated keys')
    ap.add_argument('--no-models', action='store_true')
    ap.add_argument('--no-voices', action='store_true')
    ap.add_argument('--max-tex', type=int, default=1024)
    a = ap.parse_args()
    exe = Exe(a.exe)
    if exe.read(0x8a93c4, 8) != b'\0' * 8 or exe.u32(0x8a93cc) != 0x160c:
        sys.exit("%s does not look like the Director's Cut sh2pc.exe these addresses are for" % a.exe)
    info = anim_info(exe)
    programs = stage_programs(exe)
    buffers = buffer_scripts(exe, a.data)

    scripts = {}
    for demo_dir in ('demo', 'demo2'):
        for path in sorted(glob.glob(os.path.join(a.data, demo_dir, '*', '*.dds'))):
            try:                                     # Essential Files fixes a few scripts (ee.py)
                scripts[os.path.normpath(path)] = ee.load(path, read_script)
            except (ValueError, IndexError, struct.error) as e:
                print('skip %s: %s' % (path, e))

    calls = playinfo_calls(exe)
    order = {}                                       # PlayInfos of each program, in call order
    for call, pa, fn, lists in calls:
        if pa not in order.setdefault(fn, []):
            order[fn].append(pa)
    seen, cands = {}, {}
    for call, pa, fn, lists in calls:
        if pa in seen or pa in cands:
            continue
        pi = read_playinfo(exe, pa)
        listed = [os.path.normpath(os.path.join(a.data, '..', x)) for x in dict.fromkeys(lists)]
        # a program that copies a stage buffer into the PlayInfo plays one of the scripts the
        # stage loads into that buffer (EvProgHospitalBack1st: go_to_ura_a.dds into 0x1d8645c)
        buf = playinfo_buffer(exe, pa) if pi['dds'] != DEMO_BUFFER else None
        allowed = set(buffers.get(buf, [])) if buf else (set(listed) if pi['dds'] == DEMO_BUFFER else set())
        scored = []
        for path, (d, sc) in scripts.items():
            if allowed and path not in allowed:
                continue
            sv = score_script(exe, info, pi, sc)
            if sv is not None:
                scored.append((sv, path))
        if not scored and allowed:
            # the buffer is also filled elsewhere (teddy_b goes where teddy_a does with Maria along)
            for path, (d, sc) in scripts.items():
                sv = score_script(exe, info, pi, sc)
                if sv is not None:
                    scored.append((sv, path))
        cands[pa] = (pi, fn, listed, scored)
    # A script that fits one PlayInfo exactly is most likely not also another one's (kanzume and
    # agl_himei both have just hll_jms), so it costs the others extra. Exact fits are placed first.
    exact = {}
    for pa, (pi, fn, listed, scored) in cands.items():
        for sv, p in scored:
            if sv < 1.0:
                exact.setdefault(p, set()).add(pa)
    for pa, (pi, fn, listed, scored) in cands.items():
        scored = [(sv + (50.0 if p in exact and pa not in exact[p] else 0.0), p) for sv, p in scored]
        best = None
        if scored:
            low = min(sv for sv, _ in scored)
            ties = [p for sv, p in scored if sv - low < 1e-6]
            # equal fits (tobira_a / tobira_b): the program loads its scripts in the order it plays them
            k = order[fn].index(pa)
            pick = next((p for p in ties if k < len(listed) and p == listed[k]), ties[0])
            best = (low, pick)
        seen[pa] = (pi, fn, best[1] if best else None, best[0] if best else None)

    out_dir = os.path.join(a.out, 'demo')
    os.makedirs(out_dir, exist_ok=True)
    keys, index = {}, []
    only = set(a.only.split(',')) if a.only else None
    afs = None
    jobs = [(pa, pi, fn, path) for pa, (pi, fn, path, err) in sorted(seen.items(), key=lambda x: x[0])]
    for pa, extra in EXTRA_SCRIPTS.items():
        if pa in seen:
            pi, fn = seen[pa][0], seen[pa][1]
            for rel in extra:
                path = os.path.normpath(os.path.join(a.data, rel))
                if path in scripts and score_script(exe, info, pi, scripts[path][1]) is not None:
                    jobs.append((pa, pi, fn, path))
                else:
                    print('PlayInfo %#x: extra script %s does not fit' % (pa, rel))
    for k, (pa, pi, fn, path) in enumerate(jobs):
        host.progress(0.95 * k / len(jobs), k, len(jobs), 'scenes')
        if not path:
            print('PlayInfo %#x (program %s): no script fits (demo_no %d)' % (pa, fn and hex(fn), pi['demo_no']))
            continue
        d, sc = scripts[path]
        dc = os.sep + 'demo2' + os.sep in path
        stage = programs.get(fn)
        key = os.path.splitext(os.path.basename(path))[0]
        if key in keys:
            key = '%s_%x' % (key, pa)
        keys[key] = pa
        if only and key not in only:
            continue
        n_ids = len(sc['starts']) * len(sc['names']) if pi['anim'] else 0
        ids = list(struct.unpack('<%dh' % n_ids, exe.read(pi['anim'], 2 * n_ids))) if n_ids else []
        voice = pi['stream'] - 60000 if pi['stream'] >= 60000 else -1
        voice_frames, afs = copy_voice(afs, a.data, a.out, voice, a.no_voices)
        limit = max(sc['total'], pi['stream_start'] + voice_frames) + 300
        msgs = times_list(exe, pi['msgt'], limit)
        chars = []
        for cname in sc['names']:
            ent = char_entry(info, cname)
            folder, model, mdl_path = model_for(a.data, cname, dc)
            mine = [x for x in ids if any(e['lo'] <= x <= e['hi'] and e['kind'] == ent['kind'] and e['id'] == ent['id']
                                          for e in info)]
            c = dict(name=cname, dir=folder, model=model, kind=ent['kind'], id=ent['id'], lo=ent['lo'],
                     hi=ent['hi'], motions=[])
            if folder and not a.no_models:
                m, n_cl = convert_model_files(a.out, folder, model, mdl_path, a.max_tex)
                c['motions'] = build_motions(exe, a.data, os.path.dirname(path), ent, cname, mine, m, stage, n_cl)
            elif not folder:
                print('  %s: no model for %s' % (key, cname))
            chars.append(c)
        fi = struct.unpack('<f', exe.read(FOV_INIT[pi['demo_no']], 4))[0] if pi['demo_no'] in FOV_INIT else 1.0
        fp = struct.unpack('<f', exe.read(FOV_PLAY[pi['demo_no']], 4))[0] if pi['demo_no'] in FOV_PLAY else 1.0
        size = write_dem(os.path.join(out_dir, key + '.dem'), pi, d, sc, ids, msgs, chars, voice, fi, fp)
        line = '%-22s %#x %-10s %-12s %-44s %5d frames %-60s voice %4d @%-6g msgs %d+%d  %d KB' % (
            key, pa, fn and hex(fn), stage or '-', os.path.relpath(path, a.data), sc['total'], ','.join(sc['names']),
            voice, pi['stream_start'], pi['msg_start'], len(msgs), size // 1024)
        index.append(line)
        print(line)

    # voice and subtitles without a script (SubtitlesExec): voice_<program>.voc
    for fn, times, msg, stream in voice_sequences(exe, programs):
        key = 'voice_%x' % fn
        if only and key not in only:
            continue
        voice = stream - 60000 if stream >= 60000 else -1
        frames, afs = copy_voice(afs, a.data, a.out, voice, a.no_voices)
        tl = times_list(exe, times, frames + 300 if frames else 20000)
        with open(os.path.join(out_dir, key + '.voc'), 'wb') as f:
            f.write(struct.pack('<4siiI', b'VOC1', voice, msg, len(tl)) +
                    b''.join(struct.pack('<HH', st, en) for st, en in tl))
        line = '%-22s %-10s %-12s voice %4d  msgs %d+%d' % (key, hex(fn), programs.get(fn) or '-', voice, msg, len(tl))
        index.append(line)
        print(line)
    if not only:
        with open(os.path.join(out_dir, 'demos.txt'), 'w') as f:
            f.write('\n'.join(index) + '\n')


if __name__ == '__main__':
    main()
