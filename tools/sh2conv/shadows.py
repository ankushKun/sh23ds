#!/usr/bin/env python3
"""Copy the characters' shadow models (.kg1) next to their converted S3C files.

  shadows.py <game data dir> <pack dir>
  e.g. shadows.py "Silent Hill 2/data" pack

For every pack/chr/<folder>/<model>.s3c, data/chr/<folder>/<model>.kg1 (data/chr2/... for the
folders the pack names <folder>2) is copied to pack/chr/<folder>/<model>.kg1 when it is not empty.
port/source/chr.c loads it with the model and shadow.c builds the stencil shadow volumes from it.

The .kg1 layout (PS2 beta DWARF names SHADOW_CHAR_HEAD / SHADOW_CHAR_OBJ_HEAD / SHADOW_GEOM_HEAD,
SilentHillDatabase SH2/Files/kg.md): a 16-byte header with the object count at +4, then per object
a 0x60-byte header (bone index at +4, geometry count at +6, bounding sphere s16 x, y, z, r at
+0x18), then its geometries: a 16-byte header (vertex count, primitive type, -, size in 16-byte
units including the header, bounding sphere), and for triangle strips (types 5, 6, 8, 9) two s16
x, y, z, w vertices followed by a face normal and the next vertex per triangle, in the bone's space.
The game ships empty .kg1 files for James' and Maria's gameplay models (hll_jms, lll_mar), so on
the PC they cast no such shadow; those are skipped.
"""
import argparse
import os
import shutil
import struct


def valid(path):
    """A non-empty .kg1 whose objects and geometries fit the file."""
    d = open(path, 'rb').read()
    if len(d) < 16:
        return False
    nobj = struct.unpack_from('<h', d, 4)[0]
    o = 16
    for _ in range(max(nobj, 0)):
        if o + 0x60 > len(d):
            return False
        ngeo = struct.unpack_from('<h', d, o + 6)[0]
        o += 0x60
        for _ in range(max(ngeo, 0)):
            if o + 16 > len(d):
                return False
            qw = struct.unpack_from('<h', d, o + 6)[0]
            o += 16 * max(qw, 1)
    return nobj > 0 and o <= len(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('data')
    ap.add_argument('pack')
    a = ap.parse_args()
    root = os.path.join(a.pack, 'chr')
    copied = skipped = 0
    for folder in sorted(os.listdir(root)):
        fdir = os.path.join(root, folder)
        if not os.path.isdir(fdir):
            continue
        srcs = [os.path.join(a.data, 'chr', folder)]
        if folder.endswith('2'):
            srcs.insert(0, os.path.join(a.data, 'chr2', folder[:-1]))
        for name in sorted(os.listdir(fdir)):
            if not name.endswith('.s3c'):
                continue
            stem = name[:-4]
            src = next((os.path.join(s, stem + '.kg1') for s in srcs
                        if os.path.exists(os.path.join(s, stem + '.kg1'))), None)
            if not src or os.path.getsize(src) < 16:
                continue
            if struct.unpack_from('<h', open(src, 'rb').read(16), 4)[0] == 0:
                continue                    # a header with no objects: as good as empty
            if not valid(src):
                print(f'  {folder}/{stem}.kg1: unexpected layout, skipped')
                skipped += 1
                continue
            shutil.copyfile(src, os.path.join(fdir, stem + '.kg1'))
            copied += 1
    print(f'{copied} shadow models copied, {skipped} skipped')


if __name__ == '__main__':
    main()
