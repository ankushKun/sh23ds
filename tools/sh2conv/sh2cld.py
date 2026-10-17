"""Silent Hill 2 .cld collision reader.

Layout from the PS2 beta debug info (_CL_CLDHEADER, _CL_HITPOLY_PLANE, _CL_HITPOLY_COLUMN in
Collision/cl_main.c), confirmed against the PC files:

  header   f32 sx, sz; i32 b0size, b1size, b2size, b3size, csize, disable;
           i32 b0ofs[16], b1ofs[16], b2ofs[16], b3ofs[16], clofs[16];   per-cell index lists (-1 ends)
           u32 fldofs, wldofs, cedofs, swdofs, cldofs                    polygon arrays
  plane    u8 kind, u8 shape, u16 pad, u32 weight, u32 material, i32 flg, f32 p[4][4]   (80 bytes)
  column   u8 kind, u8 shape, u16 pad, u32 weight, u32 material, i32 flg, f32 p[4][2]  (48 bytes)
Group sizes in bytes are b0size..b3size (planes) and csize (columns).
"""
import struct
from dataclasses import dataclass

import numpy as np


@dataclass
class Poly:
    group: int          # 0 b0/fld, 1 b1/wld, 2 b2/ced, 3 b3/swd
    kind: int
    shape: int
    weight: int
    material: int
    flg: int
    p: np.ndarray       # (4, 3) float


@dataclass
class Column:
    kind: int
    shape: int
    weight: int
    material: int
    flg: int
    p: np.ndarray       # (4, 2) float


def load(path):
    d = open(path, 'rb').read()
    sx, sz = struct.unpack_from('<2f', d, 0)
    sizes = struct.unpack_from('<6i', d, 8)
    fld, wld, ced, swd, cld = struct.unpack_from('<5I', d, 32 + 64 * 5)
    polys = []
    for g, (ofs, size) in enumerate(zip((fld, wld, ced, swd), sizes[:4])):
        for i in range(size // 80):
            o = ofs + 80 * i
            kind, shape, _, weight, mat, flg = struct.unpack_from('<BBHIIi', d, o)
            p = np.frombuffer(d, '<f4', 16, o + 16).reshape(4, 4)[:, :3].astype(np.float64)
            polys.append(Poly(g, kind, shape, weight, mat, flg, p))
    cols = []
    for i in range(sizes[4] // 48):
        o = cld + 48 * i
        kind, shape, _, weight, mat, flg = struct.unpack_from('<BBHIIi', d, o)
        cols.append(Column(kind, shape, weight, mat, flg, np.frombuffer(d, '<f4', 8, o + 16).reshape(4, 2).astype(np.float64)))
    return (sx, sz), polys, cols
