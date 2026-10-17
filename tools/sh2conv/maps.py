#!/usr/bin/env python3
"""Convert the map screen's data for the 3DS: map textures, marker sheets, the tables, the memos.

  maps.py <game data dir> <pack dir> [--exe sh2pc.exe]

Writes pack/map/:
  <name>.t3x   the maps (data/pic/map/*.tex and add/mapboldwin.tex, 1024x1024 DXT, squeezed to
               1024x512 RGB565 as the PC stretches them over its 4:3 screen) and the marker sheets
               (*mark*.tex, add/mapmark.tex, RGBA4 at their own size). With the Image Enhancement
               Pack (ee.py) its 5464x4096 maps are the source, and its 4x marker sheets are
               brought back to the original sheets' sizes.
  map.dat      the PC's tables (see below), read from sh2pc.exe
and pack/events/memo.msg from memo_x_msg_e.mes, the memo list's titles and texts.

map.dat (little endian), all from the exe:
  "SHMP" u32 version 1
  u32 room count, then per room id (0x88f700, 12 bytes each): s32 map, f32 x offset, f32 y offset
  u32 map count (37), then per map index:
      char tex[16], char mark[16]      (0x891670 and 0x891708: the file ids' base names)
      s8 nav[4]                         (0x890108: up, down and the two neighbours)
      u8 tables, then per marker table (0x891420 + 16 * i, a second one for some maps, 0x49afa0):
          u16 first flag, u16 count, then count records of u32 code (rect index in bits 0..5,
          y in bits 6..18 and x in bits 19..31, both 1/16 units offset by 0x1000) and s16 rect[4]
          (u0 v0 u1 v1 in the marker sheet's texels)
  u32 memo count (57), then per memo (0x88ef60, 16 bytes each):
      s16 flag, s16 kind (0 picture, 1 text, 2 picture then text), s16 title, s16 text,
      char pic[2][24] (data/pic names as "dir_name", empty for none)
  version 2 adds the marking animations (0x8917dc counts, 0x89183c lists), u32 count (6), then per
  mode u32 n and n records of u32 code (position as in the marker records) and u8 marker index
  (the marker in the map's first table it fades in).
"""
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert import encode_textures  # noqa: E402
from events import DEFAULT_EXE, Exe  # noqa: E402
from events_pack import write_msg  # noqa: E402
import sh2mes  # noqa: E402
import ee  # noqa: E402
from ui import load_tex  # noqa: E402

ROOMS = 0xd6
MAP_TEX, MARK_TEX, NAV, MARK_TABLES = 0x891670, 0x891708, 0x890108, 0x891420
MEMOS, N_MEMOS = 0x88ef60, 57

# 0x49afa0: maps that draw a second marker table (the apartment and hotel floors shared by two maps)
PAIRS = {7: (0x891490, 0x8914a0), 8: (0x891490, 0x8914a0), 9: (0x891460, 0x8914b0),
         10: (0x891470, 0x8914c0), 0x10: (0x891520, 0x891530), 0x11: (0x891520, 0x891530),
         0x1c: (0x8915e0, 0x891560), 0x1d: (0x8915f0, 0x891570), 0x20: (0x891620, 0x8915a0),
         0x21: (0x891630, 0x8915b0), 0x24: (0x891660,)}


def cstr(exe, va, n=96):
    b = exe.read(va, n)
    return b[:b.index(b'\0')].decode('latin1')


def file_name(exe, file_id):
    """A file id (pointer to a {char* path, ...} record) as 'dir/name' without data/pic and .tex."""
    if not file_id:
        return ''
    path = cstr(exe, exe.u32(file_id))
    assert path.startswith('data/pic/') and path.endswith('.tex'), path
    return path[len('data/pic/'):-4]


def marker_table(exe, va):
    rects, codes, first, end = struct.unpack('<4I', exe.read(va, 16))
    if not rects or end - first <= 1:
        return None
    out = []
    for i in range(1, end - first):
        code = exe.u32(codes + 4 * (i - 1))
        out.append((code, struct.unpack('<4h', exe.read(rects + 8 * (code & 0x3f), 8))))
    return first, out


def write_dat(exe, path):
    b = bytearray(b'SHMP' + struct.pack('<I', 2))
    b += struct.pack('<I', ROOMS)
    for r in range(ROOMS):
        b += exe.read(0x88f700 + 12 * r, 12)
    textures = set()
    b += struct.pack('<I', 37)
    for m in range(37):
        tex, mark = file_name(exe, exe.u32(MAP_TEX + 4 * m)), file_name(exe, exe.u32(MARK_TEX + 4 * m))
        textures.update(t for t in (tex, mark) if t)
        b += os.path.basename(tex).encode().ljust(16, b'\0') + os.path.basename(mark).encode().ljust(16, b'\0')
        b += exe.read(NAV + 4 * m, 4)
        tables = [t for t in (marker_table(exe, va) for va in PAIRS.get(m, (MARK_TABLES + 16 * m,))) if t]
        b += struct.pack('<B', len(tables))
        for first, recs in tables:
            b += struct.pack('<HH', first, len(recs))
            for code, rect in recs:
                b += struct.pack('<I4h', code, *rect)
    b += struct.pack('<I', N_MEMOS)
    for i in range(N_MEMOS):
        flag, kind, title, text, p1, p2 = struct.unpack('<4h2I', exe.read(MEMOS + 16 * i, 16))
        b += struct.pack('<4h', flag, kind, title, text)
        for p in (p1, p2):
            b += file_name(exe, p).replace('/', '_', 1).encode().ljust(24, b'\0')
    # 0x49c0f0 / 0x49c180: the marking animations programs start (modes 0..5, 5 follows 0)
    b += struct.pack('<I', 6)
    for mode in range(6):
        n = struct.unpack('<i', exe.read(0x8917dc + 4 * mode, 4))[0]
        lst = exe.u32(0x89183c + 4 * mode)
        b += struct.pack('<I', n)
        for k in range(n):
            code, _alpha, mark = struct.unpack('<IBB', exe.read(lst + 8 * k, 6))
            b += struct.pack('<IB', code, mark)
    with open(path, 'wb') as f:
        f.write(b)
    return sorted(textures)


def main():
    args = sys.argv[1:]
    exe_path = DEFAULT_EXE
    if '--exe' in args:
        i = args.index('--exe')
        exe_path = args[i + 1]
        del args[i:i + 2]
    data, pack = args[0], args[1]
    exe = Exe(exe_path)
    os.makedirs(os.path.join(pack, 'map'), exist_ok=True)
    textures = write_dat(exe, os.path.join(pack, 'map', 'map.dat'))
    jobs = {'rgb565': [], 'rgba4': []}
    for t in textures:
        src = os.path.join(data, 'pic', t + '.tex')
        img = load_tex(src)
        name = os.path.basename(t)
        png = f'map/{name}.png'
        if 'mark' in name or name.endswith('mk'):
            # map.dat's marker rectangles are in the original sheet's texels: a larger sheet from
            # the Image Enhancement Pack goes back to that size
            ee.like_original(img.convert('RGBA'), src).save(os.path.join(pack, png))
            jobs['rgba4'].append((png, f'map/{name}.t3x', True))
        else:
            img.convert('RGB').resize((1024, 512), Image.LANCZOS).save(os.path.join(pack, png))
            jobs['rgb565'].append((png, f'map/{name}.t3x', False))
    for fmt, j in jobs.items():
        encode_textures(j, pack, fmt=fmt, mipmaps=False)
        for png, _, _ in j:
            os.remove(os.path.join(pack, png))
    mdir = os.path.join(data, 'etc', 'message')
    write_msg(os.path.join(pack, 'events', 'memo.msg'), sh2mes.load(os.path.join(mdir, 'memo_x_msg_e.mes')))
    print('wrote map.dat,', len(textures), 'textures and memo.msg')


if __name__ == '__main__':
    main()
