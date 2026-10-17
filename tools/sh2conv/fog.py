#!/usr/bin/env python3
"""Copy the fog particle layer's data into the 3DS pack.

  fog.py <game data dir> <pack dir>

The PC fog layer (Fog/fog.c on the PS2, 0x483820..0x48a0d0 in the Director's Cut exe, see
port/source/fog.c) reads two things from the game data:

  data/bg/<area>/*.fcl, data/bg2/ma/maGB.fcl   fog collision files: walls, round obstacles, the
                                               fog areas of the map and their FOG_ENV_DATA records
  data/etc/effect/fog.tex                      the fog texture (id 0x2b22, 256x256 with alpha)

The .fcl files are copied unchanged to pack/fog/<name>.fcl (the port parses them like
FUN_004838a0 does) and the texture becomes pack/fog/fog.t3x (ETC1A4 with mipmaps).
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert import encode_textures  # noqa: E402
from ui import load_tex  # noqa: E402

# the files FUN_004849d0 loads (0x7c53c8.. name table)
FCL = [('bg', 'ca', 'caGB'), ('bg', 'cb', 'cbGB'), ('bg', 'cc', 'ccGB'), ('bg', 'cc', 'park'),
       ('bg', 'cc', 'ccnw'), ('bg', 'cd', 'cdGB'), ('bg', 'ob', 'obGB'), ('bg', 'ps', 'psGB'),
       ('bg', 'rr', 'rrGB'), ('bg2', 'ma', 'maGB')]


def main():
    data, pack = sys.argv[1], sys.argv[2]
    out = os.path.join(pack, 'fog')
    os.makedirs(out, exist_ok=True)
    n = 0
    for bg, area, name in FCL:
        src = os.path.join(data, bg, area, name + '.fcl')
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(out, name + '.fcl'))
            n += 1
        else:
            print('missing', src)
    load_tex(os.path.join(data, 'etc', 'effect', 'fog.tex')).save(os.path.join(out, 'fog.png'))
    t3x = 'fog/fog.t3x'
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([('fog/fog.png', t3x, True)], pack)
    os.remove(os.path.join(out, 'fog.png'))
    print('fog: %d .fcl files, %s' % (n, t3x))


if __name__ == '__main__':
    main()
