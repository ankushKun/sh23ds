#!/usr/bin/env python3
"""Copy the fog particle layer's data into the 3DS pack.

  fog.py <game data dir> <pack dir> [sh2pc.exe]

The PC fog layer (Fog/fog.c on the PS2, 0x483820..0x48a0d0 in the Director's Cut exe, see
port/source/fog.c) reads two things from the game data:

  data/bg/<area>/*.fcl, data/bg2/ma/maGB.fcl   fog collision files: walls, round obstacles, the
                                               fog areas of the map and their FOG_ENV_DATA records
  data/etc/effect/fog.tex                      the fog texture (id 0x2b22, 256x256 with alpha)

The .fcl files are copied unchanged to pack/fog/<name>.fcl (the port parses them like
FUN_004838a0 does) and the texture becomes pack/fog/fog.t3x (ETC1A4 with mipmaps).

The blow points of the three Eddie rooms (0x489ee0: the cold puffs from the vents) are tables in
the exe; they go to pack/fog/blow.txt as "room interval count" lines, each followed by that many
"x y z 0 vx vy vz size" lines. The exe is taken from the third argument, or found next to the data
directory; without it the port simply has no puffs.
"""
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert import encode_textures  # noqa: E402
import ee  # noqa: E402
from ui import load_tex  # noqa: E402

# the files FUN_004849d0 loads (0x7c53c8.. name table)
FCL = [('bg', 'ca', 'caGB'), ('bg', 'cb', 'cbGB'), ('bg', 'cc', 'ccGB'), ('bg', 'cc', 'park'),
       ('bg', 'cc', 'ccnw'), ('bg', 'cd', 'cdGB'), ('bg', 'ob', 'obGB'), ('bg', 'ps', 'psGB'),
       ('bg', 'rr', 'rrGB'), ('bg2', 'ma', 'maGB')]


# 0x489ee0: room id -> (table VA, sources, spawn interval in 1/60 s)
BLOW = [(0x8e, 0x887fa0, 14, 20), (0x8f, 0x888160, 10, 30), (0x90, 0x8882a0, 16, 60)]


def write_blow(exe, out):
    try:
        import pefile
    except ImportError:
        print('fog: pefile missing, no blow.txt')
        return False
    pe = pefile.PE(data=open(exe, 'rb').read(), fast_load=True)
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    lines = []
    for room, va, n, interval in BLOW:
        lines.append('%d %d %d' % (room, interval, n))
        for i in range(n):
            v = struct.unpack_from('<8f', img, va - base + 32 * i)
            lines.append(' '.join('%g' % x for x in v))
    with open(os.path.join(out, 'blow.txt'), 'w') as f:
        f.write('# blow points (tools/sh2conv/fog.py from 0x489ee0): room interval count, then sources\n')
        f.write('\n'.join(lines) + '\n')
    return True


def main():
    data, pack = sys.argv[1], sys.argv[2]
    exe = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(data)), 'sh2pc.exe')
    out = os.path.join(pack, 'fog')
    os.makedirs(out, exist_ok=True)
    n = 0
    for bg, area, name in FCL:
        src = os.path.join(data, bg, area, name + '.fcl')
        if os.path.exists(src):
            shutil.copyfile(ee.file(src), os.path.join(out, name + '.fcl'))
            n += 1
        else:
            print('missing', src)
    fog = os.path.join(data, 'etc', 'effect', 'fog.tex')
    ee.like_original(load_tex(fog), fog).save(os.path.join(out, 'fog.png'))
    t3x = 'fog/fog.t3x'
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([('fog/fog.png', t3x, True)], pack)
    os.remove(os.path.join(out, 'fog.png'))
    blow = os.path.exists(exe) and write_blow(exe, out)
    print('fog: %d .fcl files, %s%s' % (n, t3x, ', blow.txt' if blow else ''))


if __name__ == '__main__':
    main()
