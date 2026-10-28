#!/usr/bin/env python3
"""Build the moth billboard for Mary's swarm (enemy.c) from the PC's moth sprite sheet.

  moths.py <game data dir> <pack dir>
  e.g. moths.py "Silent Hill 2/data" pack

The PC draws every moth (EnLOCAL kind 15, 0x4ac070) as one camera-facing quad (0x4a46f0) with the
effect texture 0x12 (0x4a5270 -> file id 0x52f3, data/pic/effect/fly1.tbn2): 64 x 64 DXT1, a 2 x 2
sheet of wing beats; the moth's frame byte picks the quarter, u = (frame & 1) / 2, v = (frame / 2) / 2.

The port has no sprite path, so this writes an ordinary S3C (pack/chr/moth/moth.s3c, tex/000052f3.t3x)
with four unit quads in the XY plane, one per frame, each on its own bone: enemy.c gives the frame's
bone the billboard matrix and collapses the other three.
"""
import os
import struct
import sys
from types import SimpleNamespace

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                              # in tools/sh2conv
sys.path.insert(1, os.path.join(HERE, '..', '..', 'tools', 'sh2conv'))  # or beside it while testing
import chr as chrconv  # noqa: E402
from convert import encode_textures  # noqa: E402
import ee  # noqa: E402
from ui import load_tex  # noqa: E402

TEX_ID = 0x52f3


def main():
    data, pack = sys.argv[1], sys.argv[2]
    src = os.path.join(data, 'pic', 'effect', 'fly1.tbn2')
    d = open(src, 'rb').read()
    tid, w, h = struct.unpack_from('<IHH', d, 0x10)
    assert tid == TEX_ID and (w, h) == (64, 64), (hex(tid), w, h)
    # DXT1 after the 0x50-byte header (an Enhanced Edition replacement is brought to 64x64)
    rgba = np.asarray(ee.like_original(load_tex(src).convert('RGBA'), src))
    out = os.path.join(pack, 'chr', 'moth')
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    os.makedirs(os.path.join(out, 'png'), exist_ok=True)
    png = f'chr/moth/png/{TEX_ID:08x}.png'
    Image.fromarray(rgba, 'RGBA').save(os.path.join(pack, png))
    encode_textures([(png, f'chr/moth/tex/{TEX_ID:08x}.t3x', True)], pack)

    pos, uv, bones, tris = [], [], [], []
    for f in range(4):
        u0, v0 = (f & 1) * 0.5, (f >> 1) * 0.5
        base = len(pos)
        # y points down in the game, so the sheet's top row goes to -y
        for (x, y), (u, v) in zip(((-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)),
                                  ((u0, v0), (u0 + 0.5, v0), (u0 + 0.5, v0 + 0.5), (u0, v0 + 0.5))):
            pos.append((x, y, 0.0))
            uv.append((u, v))
            bones.append((f, 0, 0, 0))
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    n = len(pos)
    weights = np.zeros((n, 4))
    weights[:, 0] = 1.0
    vertices = {'pos': np.array(pos, float), 'uv': np.array(uv, float), 'bones': np.array(bones),
                'weights': weights, 'nrm': np.tile([0.0, 0.0, -1.0], (n, 1)),
                'groups': {0: np.array(tris)}}
    m = SimpleNamespace(vertices=vertices, primitives=[SimpleNamespace(start=0, backface_culling=False, pose_index=0)],
                        bone_count=4, parents=[255] * 4, initial=[np.eye(4)] * 4)
    m.bind_locals = None
    # chr.convert_model reads the bind locals through sh2mdl.bind_locals(m): identity for root bones
    orig = chrconv.sh2mdl.bind_locals
    chrconv.sh2mdl.bind_locals = lambda model: [np.eye(4)] * model.bone_count
    try:
        draws, verts, ntris = chrconv.convert_model(m, os.path.join(out, 'moth.s3c'), {0: TEX_ID},
                                                    {TEX_ID: True})
    finally:
        chrconv.sh2mdl.bind_locals = orig
    print(f'moth: 4 bones, {draws} draws, {verts} vertices, {ntris} triangles, 1 texture')


if __name__ == '__main__':
    main()
