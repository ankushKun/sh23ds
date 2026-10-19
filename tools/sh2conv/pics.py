#!/usr/bin/env python3
"""Convert the stills the stage programs show (data/pic/<dir>/<name>.tex) for the 3DS.

  pics.py <game data dir> <pack dir> [dir ...]

Writes pack/pic/<dir>/<name>.t3x: RGB565 without mipmaps, 512x256. The PC shows a 512x512 still
stretched over the 4:3 screen, so the picture is squeezed to half height here and the app draws it
as 320x240 in the middle of the top screen. Overlay sprites (keys, buttons, needles) are converted
as well but the app only shows the base still. Besides the DXT stills there are two older kinds:
8-bit paletted PC files (htl/p_hanging*) and PS2-format files (the statue box and keys, the snake
coin and the hospital box's sprite sheet), which are decoded here as well. With the Enhanced
Edition's Image Enhancement Pack installed (sh2e/pic, see ee.py and EE_PACKS.md) its upscaled stills
are the source instead. It also writes the
puzzle overlays' sprite sheets and tables (see SHEETS). Usage: pics.py <data> <pack> [--exe sh2pc.exe]
[dir ...]
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert import encode_textures  # noqa: E402
import ee  # noqa: E402
from ui import decode_tex  # noqa: E402


def load_pc_paletted(d):
    """PC .tex with format 8: 8-bit indices after the 0x50-byte header, then a 0x20-byte palette
    header and 256 RGBA colours."""
    w, h, fmt, length = struct.unpack_from('<HHII', d, 0x38)
    if fmt != 8:
        raise ValueError(f'tex format {fmt:#x}')
    idx = np.frombuffer(d, np.uint8, w * h, 0x50).reshape(h, w)
    pal = np.frombuffer(d, np.uint8, 1024, 0x50 + length + 0x20).reshape(256, 4)
    return Image.fromarray(pal[idx][:, :, :3].copy())


def ps2_unswizzle8(buf, w, h):
    """PSMT8 indices as the GS stores them when uploaded as 32-bit pixels."""
    y, x = np.mgrid[0:h, 0:w]
    block = (y & ~0xf) * w + (x & ~0xf) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    posy = (((y & ~3) >> 1) + (y & 1)) & 7
    col = posy * w * 2 + ((x + swap) & 7) * 4
    byte = ((y >> 1) & 1) + ((x >> 2) & 2)
    return np.frombuffer(buf, np.uint8)[block + col + byte]


def load_ps2(d):
    """PS2-format .tex (0x40-byte file header, 0x80-byte sprite header): 32-bit pixels in the PS2's
    0..0x80 range, or GS-swizzled 8-bit indices with a 16x16 CLUT in a 64-wide block after them."""
    w, h, bpp = struct.unpack_from('<HHB', d, 0x48)
    if bpp == 0x18:
        rgb = np.frombuffer(d, np.uint8, w * h * 4, 0xc0).reshape(h, w, 4)[:, :, :3]
    else:
        idx = ps2_unswizzle8(d[0xc0:0xc0 + w * h], w, h)
        raw = np.frombuffer(d, np.uint8, 0x4000, 0xc0 + w * h + 0x30).reshape(64, 64, 4)
        pal = raw[:16, :16].reshape(256, 4)
        perm = [(i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1) for i in range(256)]   # CSM1 order
        rgb = pal[perm][idx][:, :, :3]
    return Image.fromarray(np.minimum(rgb.astype(int) * 2, 255).astype(np.uint8))


def load_still(path):
    """A still, the Enhanced Edition's when the player has the Image Enhancement Pack (ee.py): the
    same .tex files at 4x the size (2048x2048 for the 512x512 stills), all of them DXT, also where
    the original is one of the paletted or PS2-format files."""
    return ee.load(path, decode_still)


def decode_still(path):
    try:
        return decode_tex(path)
    except (KeyError, ValueError):
        d = open(path, 'rb').read()
        if d[0xc:0x10] == b'\xa7\xa7\xa7\xa7':
            return load_ps2(d)
        return load_pc_paletted(d)


# Sprite sheets the PC draws over puzzle stills (the second file of EvSubPictureDisplay), kept at
# their own size with alpha: pack/sheet/<dir>/<name>.t3x for port/source/prog_overlay.c.
SHEETS = ['apt/clock_hari', 'apt/p_safe_close2', 'apt/p_desk_coin_coin', 'hsp/pboxkey01', 'hsp/p_boxnumber_2',
          'hsp/p_quiz_btn', 'htl/dram', 'htl/egg', 'add/teien2', 'htl/maker', 'htl/p_video02',
          'hsp/p_h_elevator_botan', 'dls/p_manacles_off', 'hsp/p_hair_hair', 'hsp/p_female_ring',
          'dls/p_tablet_plate', 'out/p_incar_map']

# The tables those overlays read, from the exe (address, size): the clock hands' sprites (0x59a390),
# the desk coins' places (0x597ce0), the trunk letters and their places (0x579af0), the tombstone's
# boards (0x571f50), the box's locks and wheels (0x58e1f0), the wall symbols (0x58db10) and the
# hospital elevators' lit buttons (0x58cad0, 0x5888a0).
OVERLAY_TABLES = [('clock_uv', 0x8e5c10, 64), ('coin', 0x8e3208, 68), ('drum_abc', 0x88f6bc, 19),
                  ('drum_pos', 0x8cd270, 912), ('teien', 0x8c7320, 48), ('box', 0x8db678, 326),
                  ('wall', 0x8db448, 432), ('video', 0x8ceac4, 16), ('marker', 0x8cd600, 0x1d2),
                  ('elevator', 0x8da828, 0x60), ('elevator2', 0x8d83d0, 0x60)]


def write_overlays(data, pack, exe_path):
    from events import Exe
    exe = Exe(exe_path)
    blob = b'SHOV' + struct.pack('<I', len(OVERLAY_TABLES))
    for name, va, size in OVERLAY_TABLES:
        blob += name.encode().ljust(16, b'\0') + struct.pack('<I', size) + exe.read(va, size)
    os.makedirs(os.path.join(pack, 'pic'), exist_ok=True)
    with open(os.path.join(pack, 'pic', 'overlay.dat'), 'wb') as f:
        f.write(blob)
    jobs = []
    for s in SHEETS:
        os.makedirs(os.path.join(pack, 'sheet', os.path.dirname(s)), exist_ok=True)
        png = f'sheet/{s}.png'
        src = os.path.join(data, 'pic', s + '.tex')
        # the overlay tables address the sheets in the original's texels, so a larger sheet from
        # the Image Enhancement Pack is brought back to the original's size
        ee.like_original(load_still(src).convert('RGBA'), src).save(os.path.join(pack, png))
        jobs.append((png, f'sheet/{s}.t3x', True))
    encode_textures(jobs, pack, fmt='rgba4', mipmaps=False)
    for png, _, _ in jobs:
        os.remove(os.path.join(pack, png))
    print('wrote overlay.dat and', len(jobs), 'sprite sheets')


def main():
    args = sys.argv[1:]
    exe_path = None
    if '--exe' in args:
        i = args.index('--exe')
        exe_path = args[i + 1]
        del args[i:i + 2]
    if exe_path is None:
        from events import DEFAULT_EXE
        exe_path = DEFAULT_EXE
    data, pack = args[0], args[1]
    write_overlays(data, pack, exe_path)
    dirs = args[2:] or sorted(d for d in os.listdir(os.path.join(data, 'pic'))
                                  if os.path.isdir(os.path.join(data, 'pic', d)))
    jobs = []
    for d in dirs:
        src = os.path.join(data, 'pic', d)
        os.makedirs(os.path.join(pack, 'pic', d), exist_ok=True)
        for fn in sorted(os.listdir(src)):
            if not fn.endswith('.tex'):
                continue
            try:
                img = load_still(os.path.join(src, fn)).convert('RGB')
            except Exception as e:      # anything else
                print(f'  skip {d}/{fn}: {e}')
                continue
            png = f'pic/{d}/{fn[:-4]}.png'
            img.resize((512, 256), Image.LANCZOS).save(os.path.join(pack, png))
            jobs.append((png, f'pic/{d}/{fn[:-4]}.t3x', False))
    encode_textures(jobs, pack, fmt='rgb565', mipmaps=False)
    for png, _, _ in jobs:
        os.remove(os.path.join(pack, png))
    print('wrote', len(jobs), 'pictures')


if __name__ == '__main__':
    main()
