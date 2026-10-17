#!/usr/bin/env python3
"""Host-side preview of an S3M room, using the same camera as the 3DS viewer's orbit mode.

  preview.py <pack dir> <area> <room> <out.png> [--angle RAD] [--size 400x240] [--mode tex|uv|group]

A small z-buffered rasterizer with perspective-correct UVs and nearest texture sampling.
It reads the converter's PNG staging (bg/<area>/png/<id>.png), so it checks the S3M data the
3DS loads without running the emulator. --mode uv colors by UV, --mode group by draw group.
"""
import argparse
import math
import os
import struct

import numpy as np
from PIL import Image


def load_s3m(path):
    d = open(path, 'rb').read()
    magic, ver = struct.unpack_from('<4sI', d, 0)
    assert magic == b'S3M1', path
    bmin = struct.unpack_from('<3f', d, 8)
    bmax = struct.unpack_from('<3f', d, 20)
    nm, nb, nv, ni, om, ob, ov, oi = struct.unpack_from('<8I', d, 32)
    mats = [struct.unpack_from('<4I', d, om + 16 * i) for i in range(nm)]
    batches = [struct.unpack_from('<6I', d, ob + 24 * i) for i in range(nb)]
    vt = np.frombuffer(d, dtype=[('p', '<f4', 3), ('uv', '<f4', 2), ('c', 'u1', 4)], count=nv, offset=ov)
    idx = np.frombuffer(d, '<u2', ni, oi)
    return np.array(bmin), np.array(bmax), mats, batches, vt, idx


def look_at(eye, target, up):
    f = target - eye
    f /= np.linalg.norm(f)
    s = np.cross(f, up)
    s /= np.linalg.norm(s)
    u = np.cross(s, f)
    m = np.eye(4)
    m[0, :3], m[1, :3], m[2, :3] = s, u, -f
    m[:3, 3] = -m[:3, :3] @ eye
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pack')
    ap.add_argument('area')
    ap.add_argument('room')
    ap.add_argument('out')
    ap.add_argument('--angle', type=float, default=2.5)
    ap.add_argument('--size', default='400x240')
    ap.add_argument('--mode', default='tex')
    ap.add_argument('--batch', type=int, default=-1)
    a = ap.parse_args()
    W, H = map(int, a.size.split('x'))
    bmin, bmax, mats, batches, vt, idx = load_s3m(os.path.join(a.pack, 'bg', a.area, a.room + '.s3m'))

    c = (bmin + bmax) / 2
    ext = bmax - bmin
    r = math.hypot(ext[0], ext[2]) * 0.55
    eye = np.array([c[0] + r * math.cos(a.angle), c[1] - r * 0.9, c[2] + r * math.sin(a.angle)])
    view = look_at(eye, c, np.array([0.0, -1.0, 0.0]))
    fy = 1 / math.tan(math.radians(60) / 2)
    fx = fy / (W / H)

    texcache = {}

    def tex(tid):
        if tid not in texcache:
            p = os.path.join(a.pack, 'bg', a.area, 'png', f'{tid:08x}.png')
            texcache[tid] = np.asarray(Image.open(p).convert('RGBA')) if os.path.exists(p) else None
        return texcache[tid]

    color = np.full((H, W, 3), (40, 36, 32), np.float32)
    zbuf = np.full((H, W), np.inf, np.float32)
    P = np.c_[vt['p'].astype(np.float64), np.ones(len(vt))] @ view.T
    zc = -P[:, 2]
    sx = (P[:, 0] * fx / np.maximum(zc, 1e-6) * 0.5 + 0.5) * W
    sy = (0.5 - P[:, 1] * fy / np.maximum(zc, 1e-6) * 0.5) * H
    gcol = {0: (90, 160, 255), 1: (255, 200, 60), 2: (255, 60, 60)}
    for pass_group in (0, 2, 1):
        for bi, (group, mat, fv, vc, fi, ic) in enumerate(batches):
            if group != pass_group or (a.batch >= 0 and bi != a.batch):
                continue
            tid, mode, mcol, flags = mats[mat]
            t = tex(tid) if tid else None
            tris = idx[fi:fi + ic].astype(np.int64).reshape(-1, 3) + fv
            for tri in tris:
                z = zc[tri]
                if (z < 20).any():
                    continue
                x, y = sx[tri], sy[tri]
                x0, x1 = max(int(x.min()), 0), min(int(math.ceil(x.max())), W - 1)
                y0, y1 = max(int(y.min()), 0), min(int(math.ceil(y.max())), H - 1)
                if x0 > x1 or y0 > y1:
                    continue
                gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
                d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
                if abs(d) < 1e-9:
                    continue
                w0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / d
                w1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / d
                w2 = 1 - w0 - w1
                inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
                if not inside.any():
                    continue
                iz = w0 / z[0] + w1 / z[1] + w2 / z[2]
                depth = 1 / np.where(inside, iz, 1)
                sub = zbuf[y0:y1 + 1, x0:x1 + 1]
                ok = inside & (depth < sub - (5 if group == 2 else 0))
                if not ok.any():
                    continue
                if a.mode == 'group':
                    rgb = np.broadcast_to(np.array(gcol[group], np.float32), ok.shape + (3,))
                    alpha = np.ones(ok.shape, np.float32)
                else:
                    uv = vt['uv'][tri].astype(np.float64)
                    u = (w0 * uv[0, 0] / z[0] + w1 * uv[1, 0] / z[1] + w2 * uv[2, 0] / z[2]) * depth
                    v = (w0 * uv[0, 1] / z[0] + w1 * uv[1, 1] / z[1] + w2 * uv[2, 1] / z[2]) * depth
                    if a.mode == 'uv' or t is None:
                        rgb = np.stack([np.mod(u, 1) * 255, np.mod(v, 1) * 255, np.full(u.shape, 128)], -1)
                        alpha = np.ones(ok.shape, np.float32)
                    else:
                        th, tw = t.shape[:2]
                        px = t[(np.mod(v, 1) * th).astype(int) % th, (np.mod(u, 1) * tw).astype(int) % tw]
                        rgb = px[..., :3].astype(np.float32)
                        alpha = px[..., 3] / 255.0
                        vc_rgb = vt['c'][tri].astype(np.float32).mean(0)[:3] / 255.0
                        rgb = rgb * vc_rgb
                if flags & 1 or group:
                    ok &= alpha > 0.25
                if group == 0:
                    zbuf[y0:y1 + 1, x0:x1 + 1] = np.where(ok, depth, sub)
                dst = color[y0:y1 + 1, x0:x1 + 1]
                aa = (alpha if group else np.ones_like(alpha))[..., None]
                color[y0:y1 + 1, x0:x1 + 1] = np.where(ok[..., None], dst * (1 - aa) + rgb * aa, dst)
    Image.fromarray(np.clip(color, 0, 255).astype(np.uint8)).save(a.out)


if __name__ == '__main__':
    main()
