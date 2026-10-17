#!/usr/bin/env python3
"""Convert an SH2 character (.mdl + .anm files) into sh23ds S3C/S3A for GPU skinning on the 3DS.

  chr.py <game data dir> <pack dir> <chr folder> <model.mdl> [anm ...] [--max-tex 512]
  e.g. chr.py "Silent Hill 2/data" pack jms hll_jms.mdl jms_basic1.anm

Writes pack/chr/<folder>/<model>.s3c, <anm>.s3a, tex/<id>.t3x.

S3C v2 (little-endian)
  char[4] "S3C1", u32 version (2), u32 bones, draws, vertices, indices, textures, f32 bbox[6],
  u32 ofs_parents, ofs_inv_bind, ofs_bind_local, ofs_draws, ofs_vertices, ofs_indices, ofs_textures
  parents     u8[bones], padded to 4
  inv_bind    bones x f32[12]   3x4 row-major, model space -> bone space
  bind_local  bones x f32[12]   3x4 row-major, parent-relative bind transform
  draw        u32 texture_slot (0xffffffff none), u32 first_index, u32 index_count, u32 flags
              (bit0 texture alpha, bit1 transparent part, bit2 cull back faces),
              u32 palette_count, u8 palette[28] (skeleton bone per slot), pad to 48 bytes
  vertex      f32 x, y, z (bind pose, model space), f32 u, v, u8 slot*3 [4], u8 weight [4],
              s8 normal x, y, z (bind pose, model space, x127), u8 pad (32 bytes)
  index       u16 into the shared vertex array (unique across draws)
  textures    u32 texture id per slot
S3A v1
  char[4] "S3A1", u32 version, u32 bones, u32 frames, then frames x bones x
  { s16 rx, ry, rz (4.12 fixed radians, ZYX order), u16 flags (bit0 rotation, bit1 translation),
    f32 tx, ty, tz }   (20 bytes)
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sh2mdl  # noqa: E402
from convert import encode_textures, pow2_floor  # noqa: E402

PALETTE_MAX = 28


def mat34(m):
    return struct.pack('<12f', *m[:3, :4].reshape(-1))


def split_palettes(tris, bones, weights):
    """Greedy split of a primitive's triangles into chunks needing <= PALETTE_MAX bones."""
    chunks, cur, cur_bones = [], [], set()
    for t in tris:
        need = set()
        for v in t:
            need |= {int(b) for b, w in zip(bones[v], weights[v]) if w > 0}
        if len(cur_bones | need) > PALETTE_MAX and cur:
            chunks.append((cur, cur_bones))
            cur, cur_bones = [], set()
        cur.append(t)
        cur_bones |= need
    if cur:
        chunks.append((cur, cur_bones))
    return chunks


def convert_model(m, out_path, tex_slot_of_prim, tex_alpha, src=None):
    """Writes the S3C. src, when a list, receives the .mdl vertex index of every S3C vertex in order
    (palette splitting duplicates vertices; demos.py needs this for the face clusters)."""
    v = m.vertices
    tex_ids = sorted(set(tex_slot_of_prim.values()))
    slot_of_id = {t: i for i, t in enumerate(tex_ids)}
    verts, indices, draws = [], [], []
    for k, pr in enumerate(m.primitives):
        tris = v['groups'].get(pr.start)
        if tris is None or not len(tris):
            continue
        for chunk, used in split_palettes(tris, v['bones'], v['weights']):
            palette = sorted(used) or [0]
            slot = {b: i for i, b in enumerate(palette)}
            remap = {}
            first = len(indices)
            for t in chunk:
                for vi in t:
                    if vi not in remap:
                        remap[vi] = len(verts)
                        if src is not None:
                            src.append(int(vi))
                        b = [slot.get(int(x), 0) * 3 for x in v['bones'][vi]]
                        w = np.round(v['weights'][vi] * 255).astype(int)
                        w[0] += 255 - w.sum()            # keep the sum exactly 255
                        nq = np.clip(np.round(v['nrm'][vi] * 127), -127, 127).astype(int)
                        verts.append(struct.pack('<5f4B4B3bx', *v['pos'][vi], *v['uv'][vi], *b,
                                                 *[int(np.clip(x, 0, 255)) for x in w], *nq))
                    indices.append(remap[vi])
            tid = tex_slot_of_prim.get(k)
            ts = slot_of_id[tid] if tid is not None else 0xFFFFFFFF
            flags = (1 if tex_alpha.get(tid) else 0) | (4 if pr.backface_culling else 0)
            draws.append(struct.pack('<5I', ts, first, len(indices) - first, flags, len(palette)) +
                         bytes(palette) + b'\0' * (PALETTE_MAX - len(palette)))
    if len(verts) > 65536:
        raise ValueError(f'{out_path}: {len(verts)} vertices after palette split')
    n = m.bone_count
    parents = bytes(int(p) for p in m.parents) + b'\0' * (-n % 4)
    inv_bind = b''.join(mat34(np.linalg.inv(x)) for x in m.initial)
    bind_local = b''.join(mat34(x) for x in sh2mdl.bind_locals(m))
    draw_bytes = b''.join(d + b'\0' * (48 - len(d)) for d in draws)
    vbytes = b''.join(verts)
    ibytes = struct.pack(f'<{len(indices)}H', *indices)
    ibytes += b'\0' * (-len(ibytes) % 4)
    texbytes = b''.join(struct.pack('<I', t) for t in tex_ids)
    head = 4 + 4 + 20 + 24 + 28
    o_par = head
    o_inv = o_par + len(parents)
    o_bl = o_inv + len(inv_bind)
    o_dr = o_bl + len(bind_local)
    o_v = o_dr + len(draw_bytes)
    o_i = o_v + len(vbytes)
    o_t = o_i + len(ibytes)
    P = m.vertices['pos']
    hdr = struct.pack('<4sI5I6f7I', b'S3C1', 2, n, len(draws), len(verts), len(indices), len(tex_ids),
                      *P.min(0), *P.max(0), o_par, o_inv, o_bl, o_dr, o_v, o_i, o_t)
    with open(out_path, 'wb') as f:
        f.write(hdr + parents + inv_bind + bind_local + draw_bytes + vbytes + ibytes + texbytes)
    return len(draws), len(verts), len(indices) // 3


def convert_anm(m, anm_path, out_path):
    frames = sh2mdl.load_anm(anm_path, m)
    out = [struct.pack('<4sIII', b'S3A1', 1, m.bone_count, len(frames))]
    for fr in frames:
        for e in fr:
            rot, trans = (e[0], e[1]) if e else (None, None)
            flags = (1 if rot is not None else 0) | (2 if trans is not None else 0)
            r = [int(round(x * 4096)) for x in rot] if rot is not None else [0, 0, 0]
            t = trans if trans is not None else (0.0, 0.0, 0.0)
            out.append(struct.pack('<3hH3f', *r, flags, *t))
    with open(out_path, 'wb') as f:
        f.write(b''.join(out))
    return len(frames)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('pack')
    ap.add_argument('folder')
    ap.add_argument('model')
    ap.add_argument('anims', nargs='*')
    ap.add_argument('--max-tex', type=int, default=512)
    a = ap.parse_args()
    src = os.path.join(a.data, 'chr', a.folder)
    out = os.path.join(a.pack, 'chr', a.folder)
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    os.makedirs(os.path.join(out, 'png'), exist_ok=True)
    m = sh2mdl.load_mdl(os.path.join(src, a.model))

    # primitive -> texture id (texture_pairs[texture_index].texture_index -> main_texture_ids)
    tex_of_prim, tex_alpha, jobs = {}, {}, []
    for k, pr in enumerate(m.primitives):
        if pr.texture_index < len(m.texture_pairs):
            ti = m.texture_pairs[pr.texture_index][0]
            if ti < len(m.main_texture_ids) and m.main_texture_ids[ti] in m.textures:
                tex_of_prim[k] = m.main_texture_ids[ti]
    for tid in sorted(set(tex_of_prim.values())):
        rgba = sh2mdl.texture_rgba(m, tid)
        alpha = bool((rgba[:, :, 3] < 250).any())
        tex_alpha[tid] = alpha
        h, w = rgba.shape[:2]
        tw, th = max(8, min(a.max_tex, pow2_floor(w))), max(8, min(a.max_tex, pow2_floor(h)))
        img = Image.fromarray(rgba, 'RGBA')
        if (tw, th) != img.size:
            img = img.resize((tw, th), Image.LANCZOS)
        png = f'chr/{a.folder}/png/{tid:08x}.png'
        t3x = f'chr/{a.folder}/tex/{tid:08x}.t3x'
        img.save(os.path.join(a.pack, png))
        jobs.append((png, t3x, alpha))
    encode_textures(jobs, a.pack)

    base = os.path.splitext(a.model)[0]
    draws, verts, tris = convert_model(m, os.path.join(out, base + '.s3c'), tex_of_prim, tex_alpha)
    print(f'{a.folder}/{base}: {m.bone_count} bones, {draws} draws, {verts} vertices, {tris} triangles, '
          f'{len(jobs)} textures')
    for anm in a.anims:
        n = convert_anm(m, os.path.join(src, anm), os.path.join(out, os.path.splitext(anm)[0] + '.s3a'))
        print(f'  {anm}: {n} frames')


if __name__ == '__main__':
    main()
