#!/usr/bin/env python3
"""One character motion the stage code sets by id, as an .s3a the port's chr.c plays.

  motion.py [--data DIR] [--exe FILE] [--out DIR]

The stages place some characters themselves (CharaCreate 0x48fcc0, then a motion setter such as
0x540cf0 with an id), outside any scene, from a file list of their own: the motion's record (id,
frames, speed, start, end, loop) is in a table, and the motions lie back to back in the .anm. Writes
pack/chr/<folder>/<name>.s3a with the frames from the motion's start to its end (S3A1, chr.py
convert_anm's layout).

  Maria lying in the day room S3 (stage 20, room 0x41, 0x58ef90): kind 0x106 (hhh Maria) with
  the file list 0x8db88c (data/demo/mar_hagureru/hhh_mar.anm) and motion 0xd49, which 0x540cf0 finds
  in the table at 0x6e7728 (ids from 3400; the motions lie back to back in the .anm) ->
  pack/chr/mar/hhh_mar_s3.s3a

Whole files, played by the port's own code:
  The boat on the lake (stage 38, character 0x10b, 0x53fe40): data/chr/boat/boat.anm, 474 frames.
  The motions 51, 52 and 53 (table 0x6e2298: both oars, the left, the right) are frames 0..149,
  150..299 and 300..449; 54, the boat lying at the pier, is 450..473 -> pack/chr/boat/boat.s3a
  James rowing: data/chr/jms/jms_boat.anm (file record 0x7d87b8), 225 frames of hll_jms' 41 bones,
  three loops of 75 frames that go with the boat's three (James' motions 0x4e24..0x4e26 in the lake's
  table 0x8cf3d8 are 150 frames long, so they play at half the boat's frame rate)
  -> pack/chr/jms/jms_boat.s3a
"""
import argparse
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ee  # noqa: E402
import sh2mdl  # noqa: E402
from demos import DEFAULT_DATA, DEFAULT_EXE, DEFAULT_OUT, anm_frames  # noqa: E402
from sound import Exe  # noqa: E402

# (motion id, record table, first id, model, anm, output folder, output name)
MOTIONS = [
    (0xd49, 0x6e7728, 3400, 'chr/mar/hhh_mar.mdl', 'demo/mar_hagureru/hhh_mar.anm', 'mar', 'hhh_mar_s3'),
]

# (model, anm, output folder, output name): the whole .anm
WHOLE = [
    ('chr/boat/boat.mdl', 'chr/boat/boat.anm', 'boat', 'boat'),
    ('chr/jms/hll_jms.mdl', 'chr/jms/jms_boat.anm', 'jms', 'jms_boat'),
]


def write_s3a(m, frames, path):
    out = [struct.pack('<4sIII', b'S3A1', 1, m.bone_count, len(frames))]
    for fr in frames:
        for e in fr:
            rot, trans = (e[0], e[1]) if e else (None, None)
            flags = (1 if rot is not None else 0) | (2 if trans is not None else 0)
            r = [int(round(x * 4096)) for x in rot] if rot is not None else [0, 0, 0]
            t = trans if trans is not None else (0.0, 0.0, 0.0)
            out.append(struct.pack('<3hH3f', *r, flags, *t))
    with open(path, 'wb') as f:
        f.write(b''.join(out))


def convert(exe, data, pack, anim, table, base, mdl, anm, folder, name):
    m = sh2mdl.load_mdl(os.path.join(data, mdl))
    d = ee.read(os.path.join(data, anm))
    roots = list(m.parents == 255)
    off, rec = 0, None
    for i in range(1, anim - base + 1):          # the motions before it, back to back from 0
        rec = struct.unpack('<6h', exe.read(table + 12 * i, 12))
        if rec[0] != base + i or rec[1] <= 0:
            print('%s: no motion record for %#x' % (name, base + i))
            return False
        offs = anm_frames(d, off, rec[1], m.bone_count, roots)
        if offs is None:
            print('%s: motion %#x runs off the end of %s' % (name, base + i, anm))
            return False
        if i < anim - base:
            off = offs[-1] + (offs[-1] & 1)
    # the motion's bytes on their own, decoded by sh2mdl from their first frame
    with tempfile.NamedTemporaryFile(suffix='.anm', delete=False) as t:
        t.write(d[offs[0]:offs[-1]])
    try:
        frames = sh2mdl.load_anm(t.name, m)
    finally:
        os.unlink(t.name)
    first, last = max(0, rec[3]), max(0, min(rec[4], len(frames) - 1))
    frames = frames[first:last + 1] or frames[:1]
    out = os.path.join(pack, 'chr', folder, name + '.s3a')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_s3a(m, frames, out)
    print('%s: motion %#x, %d frames (loop %d) -> %s' % (name, anim, len(frames), rec[5], out))
    return True


def convert_whole(data, pack, mdl, anm, folder, name):
    m = sh2mdl.load_mdl(os.path.join(data, mdl))
    frames = sh2mdl.load_anm(os.path.join(data, anm), m)
    out = os.path.join(pack, 'chr', folder, name + '.s3a')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_s3a(m, frames, out)
    print('%s: %d frames -> %s' % (anm, len(frames), out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--out', default=DEFAULT_OUT)
    a = ap.parse_args()
    exe = Exe(a.exe)
    for anim, table, base, mdl, anm, folder, name in MOTIONS:
        convert(exe, a.data, a.out, anim, table, base, mdl, anm, folder, name)
    for mdl, anm, folder, name in WHOLE:
        convert_whole(a.data, a.out, mdl, anm, folder, name)


if __name__ == '__main__':
    main()
