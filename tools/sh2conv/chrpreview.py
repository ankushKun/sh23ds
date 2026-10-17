#!/usr/bin/env python3
"""Render an SH2 character on the host, posed by an animation frame (checks mdl/anm parsing).

  chrpreview.py <model.mdl> <out.png> [--anm file.anm --frames 0,100,200] [--yaw DEG]
"""
import argparse
import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sh2mdl  # noqa: E402


def raster(P, uv, tris, tex_of_tri, textures, W, H, bg=(36, 34, 40)):
    img = np.full((H, W, 3), bg, np.float32)
    zb = np.full((H, W), np.inf)
    for (a, b, c), tex in zip(tris, tex_of_tri):
        x, y, z = P[[a, b, c], 0], P[[a, b, c], 1], P[[a, b, c], 2]
        x0, x1 = max(int(x.min()), 0), min(int(math.ceil(x.max())), W - 1)
        y0, y1 = max(int(y.min()), 0), min(int(math.ceil(y.max())), H - 1)
        if x0 > x1 or y0 > y1:
            continue
        d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(d) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
        w0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / d
        w1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / d
        w2 = 1 - w0 - w1
        ins = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        depth = w0 * z[0] + w1 * z[1] + w2 * z[2]
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        ok = ins & (depth < sub)
        if not ok.any():
            continue
        t = textures.get(tex)
        u = w0 * uv[a, 0] + w1 * uv[b, 0] + w2 * uv[c, 0]
        v = w0 * uv[a, 1] + w1 * uv[b, 1] + w2 * uv[c, 1]
        if t is None:
            rgb = np.stack([np.mod(u, 1) * 255, np.mod(v, 1) * 255, np.full(u.shape, 90)], -1)
        else:
            th, tw = t.shape[:2]
            px = t[(np.mod(v, 1) * th).astype(int) % th, (np.mod(u, 1) * tw).astype(int) % tw]
            ok &= px[..., 3] > 60
            rgb = px[..., :3].astype(np.float32)
        zb[y0:y1 + 1, x0:x1 + 1] = np.where(ok, depth, sub)
        img[y0:y1 + 1, x0:x1 + 1] = np.where(ok[..., None], rgb, img[y0:y1 + 1, x0:x1 + 1])
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mdl')
    ap.add_argument('out')
    ap.add_argument('--anm')
    ap.add_argument('--frames', default='0')
    ap.add_argument('--yaw', type=float, default=20)
    a = ap.parse_args()
    m = sh2mdl.load_mdl(a.mdl)
    textures = {}
    for k, pr in enumerate(m.primitives):
        if pr.texture_index < len(m.texture_pairs):
            ti = m.texture_pairs[pr.texture_index][0]
            tid = m.main_texture_ids[ti] if ti < len(m.main_texture_ids) else None
            if tid in m.textures and k not in textures:
                textures[k] = sh2mdl.texture_rgba(m, tid)
    tris, tex_of = [], []
    for k, pr in enumerate(m.primitives):
        t = m.vertices['groups'].get(pr.start)
        if t is None:
            continue
        tris.append(t)
        tex_of += [k] * len(t)
    tris = np.concatenate(tris)
    frames = [int(f) for f in a.frames.split(',')]
    anim = sh2mdl.load_anm(a.anm, m) if a.anm else None
    W, H = 220, 300
    panels = []
    for f in frames:
        if anim:
            world = sh2mdl.pose(m, anim[min(f, len(anim) - 1)])
            P = sh2mdl.skin(m, world)
        else:
            P = m.vertices['pos'].copy()
        yaw = math.radians(a.yaw)
        R = np.array([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]])
        Q = P @ R.T
        c = Q.mean(0)
        s = 260.0 / 1000.0
        scr = np.c_[(Q[:, 0] - c[0]) * s + W / 2, (Q[:, 1] - c[1]) * s + H / 2, Q[:, 2]]   # Y down = screen down
        panels.append(raster(scr, m.vertices['uv'], tris, tex_of, textures, W, H))
    sheet = Image.new('RGB', (W * len(panels), H))
    for i, p in enumerate(panels):
        sheet.paste(p, (W * i, 0))
    sheet.save(a.out)


if __name__ == '__main__':
    main()
