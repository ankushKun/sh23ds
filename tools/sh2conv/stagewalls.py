#!/usr/bin/env python3
"""The walls the stages' per-frame code adds to the collision each frame, read from sh2pc.exe.

  stagewalls.py --exe <sh2pc.exe> --out <pack>/events

The stage functions put these walls up with clAddDynamicWall (0x5228e0, cl_main.c): each call
passes a list of _CL_HITPOLY_PLANE (80 bytes: u8 kind, u8 shape, u16, u32 weight, u32 material,
i32 flg, f32 p[4][4]) ended by a plane of kind 0. They are the town's police barricades and street
ends, doors and gates that open on a flag, the boat's lake; the rooms' .cld files leave these places
open. Which list goes up when (the room, the flags, the town tile) is the port's
port/source/stagewalls.c, which asks for each list by its address.

Each plane becomes a wall the way collision.py makes a .cld wall (plane_wall: the XZ footprint
between its two points furthest apart, its height, and its ends in the order that gives its front,
the only side the PC stops a body on); near-flat planes are left out.

stagewalls.bin (little-endian)
  char[4] "SWL1", u32 version 2, u32 n_lists
  per list  u32 address, u32 n_walls, then n_walls x f32 x0, z0, x1, z1, y_top, y_bottom
  (version 1 had the same layout without the facing; the port reads its walls as two-sided)
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from collision import plane_wall  # noqa: E402
from pcexe import Exe  # noqa: E402

# The lists, by the function that puts them up (port/source/stagewalls.c has the conditions)
LISTS = (
    0x8e9e38, 0x8e9f28,                                         # 0x59fec0, stage 4
    0x8e7db8, 0x8e7a98, 0x8e7778, 0x8e7638, 0x8e7408, 0x8e7188,  # 0x59ef10, stage 5, by tile
    0x8e82b8,                                                   # 0x59e440 (stage 5 tile 0xde)
    0x8e7f98, 0x8e7e58,                                         # 0x59ef10, stage 5 tile 0xe1
    0x8e63a0,                                                   # 0x59d1e0, stage 6
    0x8e5268, 0x8e53f8, 0x8e5088, 0x8e4ef8, 0x8e4d68,           # 0x59bca0, stage 7
    0x8e1d48,                                                   # 0x596660, stage 12
    0x8de7a8, 0x8de938, 0x8deb68, 0x8dede8, 0x8defc8, 0x8df158,  # 0x594e10, stage 14, by tile
    0x8ded48, 0x8deca8, 0x8df388, 0x8df478, 0x8df298,
    0x8da430, 0x8da5c0,                                         # 0x58d2d0, stage 21
    0x8d7f30, 0x8d7df0,                                         # 0x588bf0, stage 26
    0x8d69c8, 0x8d68d8, 0x8d6838,                               # 0x587780, stage 30
    0x8d5f88,                                                   # 0x585bb0, stage 31
    0x8d3938, 0x8d3de8, 0x8d3a28, 0x8d3ac8,                     # 0x583960, stage 34
    0x8d2bc0, 0x8d29e0, 0x8d2b20,                               # 0x581580, stage 35
    0x8d0af0, 0x8d0c80, 0x8d0e10, 0x8d0fa0, 0x8d1130, 0x8d12c0,  # 0x580560, stage 36
    0x8d15e0, 0x8d1900, 0x8d1770, 0x8d1450, 0x8d0a50, 0x8d1a90,
    0x8bdce0,                                                   # 0x53fe40, the boat (stage 38)
    0x8cbd30,                                                   # 0x578950, stage 42
    0x8cb7a0,                                                   # 0x577690, stage 43
)
MAX_PLANES = 64     # no list has more than 20; a list running on would be a misread


def read_list(exe, va):
    walls = []
    for i in range(MAX_PLANES):
        b = exe.read(va + 80 * i, 80)
        kind, shape = b[0], b[1]
        if kind == 0:
            return walls
        p = np.array(struct.unpack_from('<16f', b, 16), dtype=np.float64).reshape(4, 4)[:, :3]
        w = plane_wall(p[:3] if shape == 0 else p[:4])
        if w:
            walls.append(w)
    raise ValueError('wall list 0x%x has no end' % va)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    exe = Exe(a.exe)
    out = [struct.pack('<4s2I', b'SWL1', 2, len(LISTS))]
    n = 0
    for va in LISTS:
        walls = read_list(exe, va)
        out.append(struct.pack('<2I', va, len(walls)))
        out += [struct.pack('<6f', *w) for w in walls]
        n += len(walls)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'stagewalls.bin'), 'wb') as f:
        f.write(b''.join(out))
    print('stage walls: %d lists, %d walls' % (len(LISTS), n))


if __name__ == '__main__':
    main()
