#!/usr/bin/env python3
"""Extract Silent Hill 2 (PC Director's Cut) sound effects and BGM selection data for sh23ds.

  sound.py [--data <game data dir>] [--exe sh2pc.exe] [--out pack/sound]

Writes
  <out>/se/se.bnk          every sound effect as mono PCM16, with the game's SE id table and the
                           door sound sets (format below)
  <out>/se_index.txt       one line per game SE id: id, sample, rate, length, loop, what it is
  <out>/floor/<map>.sfm    floor triangles with their collision material (footstep surface)
  <out>/steps.txt          heel-strike frames of James' walk and run clips (footstep timing)
  <out>/bgm.bin            BGM files, BGM pages, per-stage BGM areas and the room-id tables

Where the sounds live (static analysis of sh2pc.exe, see port/SOUND.md for the whole story)
  data/sound/sddata.bin    417 RIFF WAVE files back to back (mono PCM16, 8000..22050 Hz), each
                           followed by two bytes; the second one is 1 when the sound loops. The
                           exe never scans the file: it keeps a table of byte offsets indexed by
                           an internal id 5000..5698 (0x8a45a8 + 4 * id).
  voice.afs files 13..44   32 more effects stored as stereo ADX (internal ids 60013..60044,
                           0x8a4a40 maps game id -> 60000 + file)
  0x8a4ac0                 666 pairs {game SE id, internal id}. Game code calls the sound driver
                           (shSdCall, 0x5151d0) with ids 10000..19999 and 40000..40281.

se.bnk (little-endian)
  char[4] "S3SB", u32 version (1), u32 n_samples, u32 n_ids, u32 n_doors,
  u32 ids_offset, u32 doors_offset, u32 data_offset
  sample[n_samples]   u32 offset (from data_offset), u32 frames, u32 rate, u32 flags (1 = loop)
  id[n_ids]           u32 game id, u32 sample index          (sorted by game id)
  door[n_doors]       u16 open, close, unlock, jam, lock, pad (game SE ids, 0 = none;
                      door_se at 0x88cb70, indexed by event rslt1 bits 27..22)
  data                PCM16 mono, every sample starting on a 32-byte boundary

floor/<map>.sfm
  char[4] "SFM1", u32 n, then n x {f32 x0,y0,z0, x1,y1,z1, x2,y2,z2, u32 material}
  (every floor polygon of the .cld, quads split, Y down like the game)

bgm.bin
  char[4] "S3BG", u32 version (1), u32 n_files, u32 n_pages, u32 n_stages, u32 n_glb
  file[n_files]   i32 load id (5030..5083), i32 type (1 ADX, 0 AIX), char path[32] ("apart/bgm_101.aix")
  page[n_pages]   i32 load id, i32 start id (50000 + n)        (0x8a75d0, picked by SeBgmChange)
  stage[n_stages] u32 offset (from file start), u32 count     (sound areas, 0x8bc8c0[stage])
  glb[n_glb]      u32 default room, u32 n_exact, u32 n_block, then n_exact + n_block x {u16 cell, u16 room}
  areas           count x 64 bytes: f32 x0,z0,x1,z1,x2,z2,max_y,min_y, i32 flags, u32 chanstat[7]
                  (PS2 _SOUND_DATA; record 0 is the default used when no area matches)
"""
import argparse
import csv
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_DATA = os.path.join(ROOT, 'Silent Hill 2', 'data')
DEFAULT_EXE = os.path.join(ROOT, 'Silent Hill 2', 'sh2pc.exe')
DEFAULT_OUT = os.path.join(ROOT, 'pack', 'sound')
EE_MAP = os.path.expanduser('~/Developer/refsrc/sh2ee/AudioScripts/SFX/SFX-Map.csv')

SE_PAIRS = 0x8a4ac0          # {game id, internal id} x 666
SE_PAIR_COUNT = 0x29a
SE_OFFSETS = 0x8a45a8        # u32 sddata.bin offset per internal id (5000..5698)
SE_ADX = 0x8a4a40            # {u16 game id, u16 60000 + voice.afs file} x 32
SE_ADX_COUNT = 32
DOOR_SE = 0x88cb70           # 22 x {open, close, unlock, jam, lock, pad} (s16)
DOOR_COUNT = 22
BGM_FILES = 0x8a70b0         # {i32 id, i32 type, i32 ?, char* path} until id == -1
BGM_PAGES = 0x8a75d0         # {i32 load id, i32 start id} x 48
BGM_PAGE_COUNT = 48
BGM_AREAS = 0x8bc8c0         # _SOUND_DATA* per stage
BGM_STAGE_COUNT = 59

# What the port uses directly (ids from the callers in the exe; see SOUND.md)
KNOWN = {
    10000: 'menu: move cursor', 10002: 'menu: confirm (also the save screen)', 10003: 'menu: cancel',
    10005: 'menu: invalid', 10009: 'menu: volume tick',
    11000: 'footstep material 5/11 (metal)', 11001: 'footstep material 0 (default)',
    11002: 'footstep material 2 (grass)', 11003: 'footstep material 6', 11004: 'footstep material 1/4',
    11005: 'footstep material 7', 11006: 'footstep material 9', 11007: 'footstep material 3',
    11008: 'footstep material 10', 11009: 'footstep material 8', 11054: 'footstep material 8 (room 0x3e)',
    11041: 'item pickup (event.c 0x48c3a0)',
    18000: 'button click (flashlight off in sh23ds)', 18001: 'button click (flashlight on in sh23ds)',
}


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


class Exe:
    """Just enough PE parsing to read initialised data by virtual address."""

    def __init__(self, path):
        d = open(path, 'rb').read()
        pe = u32(d, 0x3c)
        nsec, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
        base = u32(d, pe + 24 + 28)
        self.d, self.secs = d, []
        for i in range(nsec):
            o = pe + 24 + opt + 40 * i
            vsize, va, rsize, raw = struct.unpack_from('<4I', d, o + 8)
            self.secs.append((base + va, max(vsize, rsize), raw, rsize))

    def read(self, va, n):
        for base, vsize, raw, rsize in self.secs:
            if base <= va < base + vsize:
                o = va - base
                out = self.d[raw + o: raw + min(o + n, rsize)] if o < rsize else b''
                return out + b'\0' * (n - len(out))
        raise ValueError('address %#x not mapped' % va)

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def cstr(self, va):
        return self.read(va, 64).split(b'\0')[0].decode('latin-1')


# ------------------------------------------------------------------ sddata.bin

def parse_sddata(blob):
    """Returns {file offset: (rate, pcm bytes, loop)} for every RIFF in sddata.bin."""
    out = {}
    pos = 0
    while True:
        o = blob.find(b'RIFF', pos)
        if o < 0:
            break
        if blob[o + 8:o + 16] != b'WAVEfmt ' or blob[o + 0x24:o + 0x28] != b'data':
            pos = o + 4
            continue
        fmt, ch, rate = struct.unpack_from('<HHI', blob, o + 0x14)
        bits = struct.unpack_from('<H', blob, o + 0x22)[0]
        size = u32(blob, o + 0x28)
        if fmt != 1 or ch != 1 or bits != 16:
            raise ValueError('sddata.bin entry at %#x is not mono PCM16' % o)
        # FUN_00517470 reads the loop flag at riff + 0x2d + data size
        loop = blob[o + 0x2d + size] == 1 if o + 0x2d + size < len(blob) else False
        out[o] = (rate, blob[o + 0x2c:o + 0x2c + (size & ~1)], loop)
        pos = o + 0x2c + size
    return out


# ------------------------------------------------------------------ ADX (voice.afs effects)

def adx_decode_mono(b):
    """Standard 4-bit CRI ADX (v3/v4) to mono PCM16 (stereo is averaged). Same maths as audio.c."""
    import math
    if struct.unpack_from('>H', b, 0)[0] != 0x8000 or b[4] != 3 or b[5] != 18:
        raise ValueError('not a standard ADX stream')
    cofs = struct.unpack_from('>H', b, 2)[0]
    ch, rate, total = b[7], struct.unpack_from('>I', b, 8)[0], struct.unpack_from('>I', b, 12)[0]
    hp = struct.unpack_from('>H', b, 16)[0]
    v3 = b[0x12] == 3
    z = math.cos(2 * math.pi * hp / rate)
    a, bb = math.sqrt(2) - z, math.sqrt(2) - 1
    c = (a - math.sqrt((a + bb) * (a - bb))) / bb
    c1, c2 = int(c * 8192), int(c * c * -4096)
    pos = cofs + 4
    hist = [[0, 0] for _ in range(ch)]
    chans = [[] for _ in range(ch)]
    n = 0
    while n < total and pos + 18 * ch <= len(b):
        for k in range(ch):
            f = b[pos + 18 * k: pos + 18 * k + 18]
            scale = struct.unpack_from('>h', f, 0)[0] + 1
            h1, h2 = hist[k]
            out = chans[k]
            for i in range(32):
                nib = f[2 + i // 2]
                nib = nib & 0xf if i & 1 else nib >> 4
                if nib & 8:
                    nib -= 16
                if v3:
                    v = nib * scale + ((c1 * h1) >> 12) + ((c2 * h2) >> 12)
                else:
                    v = nib * scale + ((c1 * h1 + c2 * h2) >> 12)
                v = -32768 if v < -32768 else 32767 if v > 32767 else v
                out.append(v)
                h2, h1 = h1, v
            hist[k] = [h1, h2]
        pos += 18 * ch
        n += 32
    m = min(total, len(chans[0]))
    if ch == 1:
        mono = chans[0][:m]
    else:
        mono = [sum(chans[k][i] for k in range(ch)) // ch for i in range(m)]
    return rate, struct.pack('<%dh' % m, *mono)


def afs_files(path):
    f = open(path, 'rb')
    head = f.read(8)
    if head[:4] != b'AFS\0':
        raise ValueError('%s is not an AFS archive' % path)
    n = u32(head, 4)
    toc = [struct.unpack('<II', f.read(8)) for _ in range(n)]
    return f, toc


# ------------------------------------------------------------------ bank

def ee_names():
    names = {}
    if os.path.exists(EE_MAP):
        for r in csv.reader(open(EE_MAP, encoding='utf-8')):
            if r and r[0].startswith('sddata'):
                names[int(r[0][6:])] = r[1].replace('\t', ' ')
    return names


def build_bank(exe, data, out):
    sd_path = os.path.join(data, 'sound', 'sddata.bin')
    blob = open(sd_path, 'rb').read()
    wavs = parse_sddata(blob)
    order = sorted(wavs)                       # sample index = position in sddata.bin
    wav_index = {o: i for i, o in enumerate(order)}
    samples = [wavs[o] for o in order]
    names = ee_names()
    desc = {i: 'sddata%03d %s' % (i, names.get(i, '')) for i in range(len(samples))}

    ids = {}
    for i in range(SE_PAIR_COUNT):
        gid, internal = struct.unpack('<ii', exe.read(SE_PAIRS + 8 * i, 8))
        off = exe.u32(SE_OFFSETS + 4 * internal)
        if off not in wav_index:
            raise ValueError('SE %d (internal %d) points at %#x, not a RIFF start' % (gid, internal, off))
        ids[gid] = wav_index[off]

    afs = os.path.join(data, 'sound', 'adx', 'voice', 'voice.afs')
    if os.path.exists(afs):
        f, toc = afs_files(afs)
        for i in range(SE_ADX_COUNT):
            gid, fid = struct.unpack('<HH', exe.read(SE_ADX + 4 * i, 4))
            o, n = toc[fid - 60000]
            f.seek(o)
            rate, pcm = adx_decode_mono(f.read(n))
            ids[gid] = len(samples)
            desc[len(samples)] = 'voice.afs #%d (ADX, mixed to mono)' % (fid - 60000)
            samples.append((rate, pcm, False))
    else:
        print('warning: %s missing, the 32 ADX effects are left out' % afs)

    doors = [struct.unpack('<6h', exe.read(DOOR_SE + 12 * i, 12)) for i in range(DOOR_COUNT)]

    se_dir = os.path.join(out, 'se')
    os.makedirs(se_dir, exist_ok=True)
    n_ids = len(ids)
    ids_off = 32 + 16 * len(samples)
    doors_off = ids_off + 8 * n_ids
    data_off = (doors_off + 12 * len(doors) + 31) & ~31
    table, body = [], bytearray()
    for rate, pcm, loop in samples:
        table.append(struct.pack('<4I', len(body), len(pcm) // 2, rate, 1 if loop else 0))
        body += pcm
        body += b'\0' * (-len(body) & 31)
    with open(os.path.join(se_dir, 'se.bnk'), 'wb') as f:
        f.write(struct.pack('<4s7I', b'S3SB', 1, len(samples), n_ids, len(doors), ids_off, doors_off, data_off))
        f.write(b''.join(table))
        f.write(b''.join(struct.pack('<2I', g, ids[g]) for g in sorted(ids)))
        f.write(b''.join(struct.pack('<6H', *[v & 0xffff for v in d]) for d in doors))
        f.write(b'\0' * (data_off - f.tell()))
        f.write(body)

    door_use = {}
    for i, d in enumerate(doors):
        for k, v in zip(('open', 'close', 'unlock', 'jam', 'lock'), d[:5]):
            if v:
                door_use.setdefault(v, []).append('door%d.%s' % (i, k))
    with open(os.path.join(out, 'se_index.txt'), 'w') as f:
        f.write('# game SE id -> sample in se.bnk (generated by tools/sh2conv/sound.py)\n')
        f.write('# id      sample  rate   seconds  loop  use / source\n')
        for g in sorted(ids):
            s = ids[g]
            rate, pcm, loop = samples[s]
            use = [KNOWN[g]] if g in KNOWN else []
            use += door_use.get(g, [])
            f.write('%-8d  %4d  %5d  %7.2f  %-4s  %s%s\n' % (g, s, rate, len(pcm) / 2 / rate, 'loop' if loop else '',
                                                           ('; '.join(use) + ' | ') if use else '', desc[s].strip()))
    total = sum(len(p) for _, p, _ in samples)
    print('se.bnk: %d samples (%.1f MB PCM), %d ids, %d door sets' % (len(samples), total / 1e6, n_ids, len(doors)))


# ------------------------------------------------------------------ footstep surfaces

def build_floors(data, out, areas):
    import sh2cld
    n_files = 0
    for bg in ('bg', 'bg2'):
        base = os.path.join(data, bg)
        if not os.path.isdir(base):
            continue
        for area in sorted(os.listdir(base)):
            if areas and area not in areas:
                continue
            d = os.path.join(base, area)
            if not os.path.isdir(d):
                continue
            for fn in sorted(os.listdir(d)):
                if not fn.endswith('.cld'):
                    continue
                _, polys, _ = sh2cld.load(os.path.join(d, fn))
                tris = []
                for p in polys:
                    if p.kind == 0 or p.group != 0:
                        continue
                    pts = p.p[:3] if p.shape == 0 else p.p[:4]
                    for t in ([pts[[0, 1, 2]]] if len(pts) == 3 else [pts[[0, 1, 2]], pts[[0, 2, 3]]]):
                        tris.append(struct.pack('<9fI', *t.reshape(-1), p.material))
                os.makedirs(os.path.join(out, 'floor'), exist_ok=True)
                with open(os.path.join(out, 'floor', fn[:-4] + '.sfm'), 'wb') as f:
                    f.write(struct.pack('<4sI', b'SFM1', len(tris)) + b''.join(tris))
                n_files += 1
    print('floor materials: %d rooms' % n_files)


# ------------------------------------------------------------------ footstep frames

ANKLES = (25, 26)            # hll_jms.mdl bones with the lowest mean height in the walk cycle


def build_steps(data, out, pack):
    """Heel strikes of James' walk and run clips, found from the animation itself.

    The PC triggers footsteps at fixed frames of its own walk / run motions (0x54a9a0: walk at 5
    and 17, run at 3 and 14 with a two-handed weapon), but the port plays jms_basic1 ranges from
    pack/chr/jms/clips.txt that hold several cycles. A foot lands on the frame its ankle drops
    back into the lowest 12 units it reaches in that clip (Y points down, so larger is lower).
    """
    import sh2mdl
    jms = os.path.join(data, 'chr', 'jms')
    clips = {'walk': (51, 91), 'run': (92, 170)}
    ctxt = os.path.join(pack, 'chr', 'jms', 'clips.txt')
    if os.path.exists(ctxt):
        for line in open(ctxt):
            f = line.split()
            if len(f) >= 3 and f[0] in clips:
                clips[f[0]] = (int(f[1]), int(f[2]))
    m = sh2mdl.load_mdl(os.path.join(jms, 'hll_jms.mdl'))
    anim = sh2mdl.load_anm(os.path.join(jms, 'jms_basic1.anm'), m)
    lines = ['# clip frame foot: heel strikes in jms_basic1 (generated by tools/sh2conv/sound.py)']
    for name, (a, b) in clips.items():
        ys = [[sh2mdl.pose(m, anim[f])[bone][1, 3] for bone in ANKLES] for f in range(a, b + 1)]
        hits = []
        for foot in range(2):
            y = [v[foot] for v in ys]
            band = max(y) - 12.0
            last = -100
            for i in range(len(y)):
                # i - 1 wraps (the clip loops); a planted foot that wobbles at the band edge
                # would land twice, so a foot needs 8 frames between landings
                if y[i] > band and y[i - 1] <= band and i - last >= 8:
                    hits.append((a + i, foot))
                    last = i
        # The clips do not loop seamlessly, so a foot can land at the seam just before the next
        # strike; of two strikes closer than 8 frames (going round the loop) the later one stays.
        hits.sort()
        n, length = len(hits), b - a + 1
        for i, (fr, foot) in enumerate(hits):
            if n == 1 or (hits[(i + 1) % n][0] - fr) % length >= 8:
                lines.append('%s %d %d' % (name, fr, foot))
    with open(os.path.join(out, 'steps.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('steps.txt: ' + ', '.join('%s %d' % (n, sum(1 for l in lines if l.startswith(n + ' '))) for n in clips))


# ------------------------------------------------------------------ BGM

def build_bgm(exe, out):
    import sh2rooms
    files = []
    a = BGM_FILES
    while True:
        fid, typ, _, ptr = struct.unpack('<iiiI', exe.read(a, 16))
        if fid == -1:
            break
        path = exe.cstr(ptr).lstrip('\\').replace('\\', '/')
        files.append(struct.pack('<ii32s', fid, typ, path.encode()))
        a += 16
    pages = [exe.read(BGM_PAGES + 8 * i, 8) for i in range(BGM_PAGE_COUNT)]
    stages = []
    for st in range(BGM_STAGE_COUNT):
        p = exe.u32(BGM_AREAS + 4 * st)
        recs = [exe.read(p, 64)]
        while True:
            r = exe.read(p + 64 * len(recs), 64)
            if struct.unpack_from('<i', r, 32)[0] < 0:
                break
            recs.append(r)
        stages.append(recs)
    glbs = []
    for g in range(max(sh2rooms.ROOM_TABLE) + 1):
        exact, block, default = sh2rooms.ROOM_TABLE.get(g, ({}, {}, 0))
        glbs.append(struct.pack('<3I', default, len(exact), len(block)) +
                    b''.join(struct.pack('<2H', c, r) for c, r in sorted(exact.items())) +
                    b''.join(struct.pack('<2H', c, r) for c, r in sorted(block.items())))
    head = 24 + 40 * len(files) + 8 * len(pages) + 8 * len(stages)
    glb_blob = b''.join(glbs)
    off = head + len(glb_blob)
    stage_tab = []
    for recs in stages:
        stage_tab.append(struct.pack('<2I', off, len(recs)))
        off += 64 * len(recs)
    with open(os.path.join(out, 'bgm.bin'), 'wb') as f:
        f.write(struct.pack('<4s5I', b'S3BG', 1, len(files), len(pages), len(stages), len(glbs)))
        f.write(b''.join(files) + b''.join(pages) + b''.join(stage_tab) + glb_blob)
        for recs in stages:
            f.write(b''.join(recs))
    print('bgm.bin: %d files, %d pages, %d stages, %d areas' % (len(files), len(pages), len(stages),
                                                               sum(len(r) for r in stages)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--areas', default='', help='limit floor material files to these bg areas (ap,hp)')
    ap.add_argument('--no-bank', action='store_true', help='skip se.bnk (the slow part is voice.afs)')
    a = ap.parse_args()
    exe = Exe(a.exe)
    if exe.read(0x8a93c4, 8) != b'\0' * 8 or exe.u32(0x8a93cc) != 0x160c:
        sys.exit('%s does not look like the Director\'s Cut sh2pc.exe these addresses are for' % a.exe)
    os.makedirs(a.out, exist_ok=True)
    if not a.no_bank:
        build_bank(exe, a.data, a.out)
    build_floors(a.data, a.out, set(a.areas.split(',')) if a.areas else None)
    build_steps(a.data, a.out, os.path.dirname(os.path.abspath(a.out)))
    build_bgm(exe, a.out)


if __name__ == '__main__':
    main()
