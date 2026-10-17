#!/usr/bin/env python3
"""Convert .cld collision into sh23ds S3L files: floor triangles and wall segments.

  collision.py <game data dir> <pack dir> [--areas ap,hp]

S3L v1 (little-endian)
  char[4] "S3L1", u32 version, u32 n_floors, u32 n_walls
  floor   f32 x0,y0,z0, x1,y1,z1, x2,y2,z2                     (triangles, quads split)
  wall    f32 x0, z0, x1, z1, y_top, y_bottom, u32 group, u32 material
          (XZ footprint of a near-vertical polygon from groups b1..b3; Y points down so
           y_top < y_bottom)
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sh2cld  # noqa: E402


def polygon_points(p):
    return p.p[:3] if p.shape == 0 else p.p[:4]


def convert(cld_path, out_path):
    _, polys, _ = sh2cld.load(cld_path)
    floors, walls = [], []
    for p in polys:
        if p.kind == 0:
            continue
        pts = polygon_points(p)
        if p.group == 0:
            tris = [pts[[0, 1, 2]]] if len(pts) == 3 else [pts[[0, 1, 2]], pts[[0, 2, 3]]]
            for t in tris:
                floors.append(struct.pack('<9f', *t.reshape(-1)))
            continue
        n = np.cross(pts[1] - pts[0], pts[2] - pts[0])
        ln = np.linalg.norm(n)
        if ln < 1e-6 or abs(n[1]) / ln > 0.5:
            continue                       # flat-ish: not a wall
        xz = pts[:, [0, 2]]
        dmax, best = -1, (0, 1)
        for i in range(len(xz)):
            for j in range(i + 1, len(xz)):
                dd = np.sum((xz[i] - xz[j]) ** 2)
                if dd > dmax:
                    dmax, best = dd, (i, j)
        if dmax < 1.0:
            continue
        a, b = xz[best[0]], xz[best[1]]
        walls.append(struct.pack('<6f2I', a[0], a[1], b[0], b[1], pts[:, 1].min(), pts[:, 1].max(),
                                 p.group, p.material))
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4s3I', b'S3L1', 1, len(floors), len(walls)) + b''.join(floors) + b''.join(walls))
    return len(floors), len(walls)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('pack')
    ap.add_argument('--areas', default='')
    ap.add_argument('--bg', default='bg')
    a = ap.parse_args()
    areas = a.areas.split(',') if a.areas else sorted(os.listdir(os.path.join(a.data, a.bg)))
    for area in areas:
        src = os.path.join(a.data, a.bg, area)
        out = os.path.join(a.pack, 'bg', area)
        os.makedirs(out, exist_ok=True)
        nf = nw = rooms = 0
        for fn in sorted(os.listdir(src)):
            if fn.endswith('.cld'):
                f, w = convert(os.path.join(src, fn), os.path.join(out, fn[:-4] + '.s3l'))
                nf += f
                nw += w
                rooms += 1
        print(f'{area}: {rooms} collision files, {nf} floor triangles, {nw} wall segments')


if __name__ == '__main__':
    main()
