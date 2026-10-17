#!/usr/bin/env python3
"""Write one index file per map area so the 3DS opens a single file when it enters an area.

  areaindex.py <pack dir>
  e.g. areaindex.py pack

Entering an area used to open every room's .s3m (for its bounds) and every room's .cam (camera
roads) one by one: 1 to 4 seconds in Azahar for the town or the hospital (port/PERFORMANCE.md).
pack/bg/<area>/area.idx holds the same data in one file:

  char[4] "SAI1", u32 version (1), u32 entries, u32 reserved
  entries x { char name[16] (zero padded), f32 bounds[6] (the .s3m header's bbox, zeros when
              there is no .s3m), u32 cam_size, u8 cam[cam_size] padded to 4 bytes }

The entries are the rooms of rooms.txt plus <area>GB when the area has the area-wide camera file.
Run it again whenever the area's .s3m or .cam files change (port/source/areaidx.c falls back to
the single files for names the index does not have).
"""
import argparse
import os
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pack')
    a = ap.parse_args()
    bg = os.path.join(a.pack, 'bg')
    total = 0
    for area in sorted(os.listdir(bg)):
        adir = os.path.join(bg, area)
        rooms_txt = os.path.join(adir, 'rooms.txt')
        if not os.path.isfile(rooms_txt):
            continue
        names = [l.strip() for l in open(rooms_txt) if l.strip() and len(l.strip()) < 16]
        if os.path.exists(os.path.join(adir, area + 'GB.cam')):
            names.append(area + 'GB')
        out = bytearray()
        for name in names:
            bb = [0.0] * 6
            s3m = os.path.join(adir, name + '.s3m')
            if os.path.exists(s3m):
                with open(s3m, 'rb') as f:
                    h = f.read(32)
                if len(h) == 32:
                    bb = list(struct.unpack_from('<6f', h, 8))
            cam = b''
            cpath = os.path.join(adir, name + '.cam')
            if os.path.exists(cpath):
                cam = open(cpath, 'rb').read()
            out += name.encode().ljust(16, b'\0') + struct.pack('<6fI', *bb, len(cam)) + cam
            out += b'\0' * (-len(cam) % 4)
        with open(os.path.join(adir, 'area.idx'), 'wb') as f:
            f.write(struct.pack('<4sIII', b'SAI1', 1, len(names), 0) + out)
        total += 1
    print(f'{total} area indexes written')


if __name__ == '__main__':
    main()
