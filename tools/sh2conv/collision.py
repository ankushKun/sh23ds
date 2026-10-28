#!/usr/bin/env python3
"""Convert .cld collision into sh23ds S3L files: floor triangles and wall segments.

  collision.py <game data dir> <pack dir> [--areas ap,hp]

S3L (little-endian)
  char[4] "S3L1", u32 version 2, u32 n_floors, u32 n_walls
  floor   f32 x0,y0,z0, x1,y1,z1, x2,y2,z2                     (triangles, quads split)
  wall    f32 x0, z0, x1, z1, y_top, y_bottom, u32 group, u32 material
          (XZ footprint of a near-vertical polygon from groups b1..b3; Y points down so
           y_top < y_bottom). The ends are in the order that puts the wall's front, the side the
           PC stops bodies on, where (x - x0) * (z1 - z0) - (z - z0) * (x1 - x0) >= 0. Version 1
           (no facing: the port reads its walls as two-sided) had the same layout.
  then, when the room has any, the columns (u32 count first): the poles and tree trunks
  column  f32 x, z, y_bottom, y_top, radius                     (_CL_HITPOLY_COLUMN p[0][0],
          p[1][0], p[0][1], p[2][1], p[3][1]; the PC pushes characters off them in 0x522de0)
  A runtime that knows only floors and walls ignores the column section.
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


def plane_wall(pts):
    """A near-vertical polygon (3 or 4 points, Y down) as a wall: the XZ footprint between its two
    points furthest apart, and its height (x0, z0, x1, z1, y_top, y_bottom); None when it is
    flat-ish or has no footprint. stagewalls.py makes the stage code's walls the same way.

    The PC stops a body only on a wall's front: clCheckColumn2WallHit (0x522b50) pushes while
    n . (c - p2) >= 0, n = (p2 - p1) x (p1 - p0) (0x529b10), and its line tests (0x528840, 0x528350)
    hit going from front to back. The ends come out in the order that puts that front on the side
    where (x - x0) * (z1 - z0) - (z - z0) * (x1 - x0) >= 0 (port/source/collide.h col_wall_side)."""
    n = np.cross(pts[1] - pts[0], pts[2] - pts[0])
    ln = np.linalg.norm(n)
    if ln < 1e-6 or abs(n[1]) / ln > 0.5:
        return None                        # flat-ish: not a wall
    xz = pts[:, [0, 2]]
    dmax, best = -1, (0, 1)
    for i in range(len(xz)):
        for j in range(i + 1, len(xz)):
            dd = np.sum((xz[i] - xz[j]) ** 2)
            if dd > dmax:
                dmax, best = dd, (i, j)
    if dmax < 1.0:
        return None
    a, b = xz[best[0]], xz[best[1]]
    front = np.cross(pts[2] - pts[1], pts[1] - pts[0])
    if front[0] * (b[1] - a[1]) - front[2] * (b[0] - a[0]) < 0:
        a, b = b, a
    return a[0], a[1], b[0], b[1], pts[:, 1].min(), pts[:, 1].max()


def convert(cld_path, out_path):
    _, polys, cols = sh2cld.load(cld_path)
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
        w = plane_wall(pts)
        if w:
            walls.append(struct.pack('<6f2I', *w, p.group, p.material))
    columns = [struct.pack('<5f', c.p[0][0], c.p[1][0], c.p[0][1], c.p[2][1], c.p[3][1])
               for c in cols if c.kind != 0 and c.p[3][1] > 0.0]
    tail = struct.pack('<I', len(columns)) + b''.join(columns) if columns else b''
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4s3I', b'S3L1', 2, len(floors), len(walls)) + b''.join(floors) + b''.join(walls)
                + tail)
    return len(floors), len(walls), len(columns)


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
        nf = nw = nc = rooms = 0
        for fn in sorted(os.listdir(src)):
            if fn.endswith('.cld'):
                f, w, c = convert(os.path.join(src, fn), os.path.join(out, fn[:-4] + '.s3l'))
                nf += f
                nw += w
                nc += c
                rooms += 1
        print(f'{area}: {rooms} collision files, {nf} floor triangles, {nw} wall segments, {nc} columns')


if __name__ == '__main__':
    main()
