#!/usr/bin/env python3
"""Convert the user's title art into the 3DS pack.

  ui.py <game data dir> <pack dir>

Writes pack/ui/title.t3x: the Director's Cut logo (data/pic/etc/sh2dc.tex, 512x512 DXT3) cropped to
a 512x128 strip around the lettering, RGB565 without mipmaps.

Also writes the save screen's backgrounds (data/menu/mc/savebg.tbn2, James, and savebg2.tbn2,
Maria; 512x512 DXT3 squeezed to 512x256 RGB565 like the stills: pack/ui/savebg.t3x, savebg2.t3x)
and its texts (data/etc/message/m_card_x_msg_e.mes as pack/events/mcard.msg), and the options
screens' texts (option_msg_e.mes as pack/events/option.msg).

Also writes the PC's text font (pack/ui/font.t3x and font.bin, see write_font), which the exe carries
compressed; pass --exe to read another sh2pc.exe than the default one.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ee  # noqa: E402
import sh2map  # noqa: E402
from convert import encode_textures  # noqa: E402


def load_tex(path):
    """A single-sprite .tex or .tbn2 file, the Enhanced Edition's version when the player has it
    (ee.py; its images are the same files at up to 4x the size). See decode_tex."""
    return ee.load(path, decode_tex)


def decode_tex(path):
    """Single-sprite .tex / .tbn2 file: 0x30-byte header, then the sprite header (u16 width and
    height at 0x38, u32 format at 0x3c, u32 length at 0x40) and the pixels at 0x50: DXT1..5
    (0x100..0x104) or 32-bit BGRA (0x20: menu/mc/mu_alpha.tbn2, the Image Enhancement Pack's
    start01*.tex)."""
    d = open(path, 'rb').read()
    if struct.unpack_from('<I', d, 0)[0] != 0x19990901:
        raise ValueError(f'{path}: not a .tex file')
    _, _, _, w, h, fmt, length, _, _, _ = struct.unpack_from('<IHHHHIIIII', d, 0x30)
    if fmt == 0x20:
        bgra = np.frombuffer(d, np.uint8, w * h * 4, 0x50).reshape(h, w, 4)
        return Image.fromarray(np.ascontiguousarray(bgra[:, :, [2, 1, 0, 3]]), 'RGBA')
    kind = sh2map.MAP_TEX_KIND[fmt]
    return Image.fromarray(sh2map.decode_dxt(d[0x50:0x50 + length], w, h, kind))


# The PC's two text fonts (0x480ea0 draws a glyph into its glyph cache): font 0 is 20x30, font 1
# 16x24 (the u16 pairs at 0x806480), the font data pointers are at 0x806488 and 0x806490 holds
# the eight alpha levels of the 3-bit pixel codes. Each font's data starts with u16 bounds (ended
# by 0) where the glyph offsets wrap past 64K dwords, the widths of codes below 0xe0 at +0x10 and
# the u16 dword offsets of every code's bitstream at +0xf0. Codes below 0xe0 are the text
# characters (Windows-1252 minus 0x20) and are as wide as their width byte; the codes from 0xe0
# on are the full-width set (Japanese, symbols, the 0x138 star and the 0x172.. Latin letters).
FONT_SIZES = 0x806480
FONT_DATA = 0x806488
FONT_LEVELS = 0x806490
FONT_CODES = list(range(0xe0)) + [0x138] + list(range(0x172, 0x1b0))


def font_glyph(img, va, f, code):
    """Decodes one glyph: 3-bit codes read LSB first from dwords (0x480e00); 7 starts a run of
    transparent pixels whose length follows in up to five more codes."""
    def u16(a):
        return struct.unpack_from('<H', img, a)[0]
    fw, fh = u16(va(FONT_SIZES + 4 * f)), u16(va(FONT_SIZES + 4 * f + 2))
    fa = va(struct.unpack_from('<I', img, va(FONT_DATA + 4 * f))[0])
    levels = img[va(FONT_LEVELS):va(FONT_LEVELS) + 8]
    bounds = []
    while u16(fa + 2 * len(bounds)):
        bounds.append(u16(fa + 2 * len(bounds)))
    w = img[fa + 0x10 + code] if code < 0xe0 else fw
    off = u16(fa + 0xf0 + 2 * code)
    px = np.zeros((fh, max(w, 1)), np.uint8)
    if not off or not w:
        return w, px
    p = fa + (sum(1 for b in bounds if b < code) * 0x10000 + off) * 4
    bits = nb = run = 0

    def sym():
        nonlocal bits, nb, p
        if nb < 3:
            bits |= struct.unpack_from('<I', img, p)[0] << nb
            p += 4
            nb += 32
        s = bits & 7
        bits >>= 3
        nb -= 3
        return s
    for y in range(fh):
        for x in range(w):
            if run:
                run -= 1
                continue
            s = sym()
            if s == 7:
                run = sym()
                if run == 0:
                    run = sym() + 7
                    if run == 7:
                        run = sym() + 14
                        if run == 14:
                            run = sym() + 0x15 + sym() * 8
                            if run == 0x15:
                                run = sym() + 0x54 + sym() * 8 + sym() * 64
                continue
            px[y, x] = levels[s]
    return w, px


# The 3DS draws the PC's 512x448 text space as 320x210 pixels (see port/source/font.h), so the
# glyphs are stored at that size and drawn one texel per pixel.
FONT_SX, FONT_SY = 0.625, 0.46875


def write_font(pack, exe_path):
    """pack/ui/font.t3x: both fonts' glyphs, shrunk to the 3DS size, packed into one LA4 atlas
    (white, the glyph as alpha). pack/ui/font.bin: "FNT1", u16 font count, then per font u16 cell
    width, cell height and glyph count and per glyph u16 code, u16 x, u16 y (the atlas rectangle's
    corner), u8 width, u8 height (the rectangle, in 3DS pixels), u8 advance (PC units), u8 0. The
    advance is the PC's: the width plus 2, 7 for the space and the cell width plus 2 for the
    full-width codes (0x47f400)."""
    import pefile
    pe = pefile.PE(exe_path, fast_load=True)
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase

    def va(a):
        return a - base
    atlas = np.zeros((512, 512), np.uint8)
    out = [b'FNT1', struct.pack('<H', 2)]
    x = y = 0
    pad = 1
    for f in (0, 1):
        fw, fh = struct.unpack_from('<HH', img, va(FONT_SIZES + 4 * f))
        sh = int(round(fh * FONT_SY))
        recs = []
        for code in FONT_CODES:
            w, px = font_glyph(img, va, f, code)
            adv = 7 if code == 0 else w + 2 if code < 0xe0 else fw + 2
            if code == 0 or not w or not px.any():
                recs.append(struct.pack('<HHHBBBB', code, 0, 0, 0, 0, adv, 0))
                continue
            sw = max(1, int(round(w * FONT_SX)))
            small = np.asarray(Image.fromarray(px[:, :w]).resize((sw, sh), Image.LANCZOS))
            if x + sw + pad > 512:
                x, y = 0, y + sh + pad
            if y + sh + pad > 512:
                raise ValueError('font atlas full')
            atlas[y:y + sh, x:x + sw] = small
            recs.append(struct.pack('<HHHBBBB', code, x, y, sw, sh, adv, 0))
            x += sw + pad
        x, y = 0, y + sh + pad
        out.append(struct.pack('<HHH', fw, fh, len(recs)))
        out += recs
    height = 64
    while height < y:
        height *= 2
    os.makedirs(os.path.join(pack, 'ui'), exist_ok=True)
    with open(os.path.join(pack, 'ui', 'font.bin'), 'wb') as fp:
        fp.write(b''.join(out))
    rgba = np.zeros((height, 512, 4), np.uint8)
    rgba[:, :, :3] = 255
    rgba[:, :, 3] = atlas[:height]
    Image.fromarray(rgba, 'RGBA').save(os.path.join(pack, 'ui', 'font.png'))
    t3x = 'ui/font.t3x'
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([('ui/font.png', t3x, True)], pack, fmt='la4', mipmaps=False)
    os.remove(os.path.join(pack, 'ui', 'font.png'))
    print(f'wrote {t3x} (512x{height}) and ui/font.bin')


def write_save_screen(data, pack):
    """The save screen's pictures and texts (0x453fc0 / 0x4537b0, the PC's file list)."""
    import sh2mes
    from events_pack import write_msg
    jobs = []
    for name in ('savebg', 'savebg2'):
        # 512x512 DXT3; the Image Enhancement Pack's are 2732x2048 (4:3 at square pixels), which
        # squeeze to the same 512x256 since both fill the PC's 4:3 screen
        img = load_tex(os.path.join(data, 'menu', 'mc', name + '.tbn2')).convert('RGB')
        png = 'ui/%s.png' % name
        img.resize((512, 256), Image.LANCZOS).save(os.path.join(pack, png))
        t3x = 'ui/%s.t3x' % name
        if os.path.exists(os.path.join(pack, t3x)):
            os.remove(os.path.join(pack, t3x))
        jobs.append((png, t3x, False))
    encode_textures(jobs, pack, fmt='rgb565', mipmaps=False)
    for png, _, _ in jobs:
        os.remove(os.path.join(pack, png))
    os.makedirs(os.path.join(pack, 'events'), exist_ok=True)
    write_msg(os.path.join(pack, 'events', 'mcard.msg'),
              sh2mes.load(os.path.join(data, 'etc', 'message', 'm_card_x_msg_e.mes')))
    print('wrote ui/savebg.t3x, ui/savebg2.t3x and events/mcard.msg')


def write_options(data, pack):
    """The options screens' texts (0x461600 / 0x464eb0 draw option_msg_e.mes by number)."""
    import sh2mes
    from events_pack import write_msg
    os.makedirs(os.path.join(pack, 'events'), exist_ok=True)
    write_msg(os.path.join(pack, 'events', 'option.msg'),
              sh2mes.load(os.path.join(data, 'etc', 'message', 'option_msg_e.mes')))
    print('wrote events/option.msg')


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
    write_font(pack, exe_path)
    write_save_screen(data, pack)
    write_options(data, pack)
    os.makedirs(os.path.join(pack, 'ui'), exist_ok=True)
    logo = os.path.join(data, 'pic', 'etc', 'sh2dc.tex')
    img = ee.like_original(load_tex(logo), logo).convert('RGB')
    img.crop((0, 136, 512, 264)).save(os.path.join(pack, 'ui', 'title.png'))
    t3x = 'ui/title.t3x'
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([('ui/title.png', t3x, False)], pack, fmt='rgb565', mipmaps=False)
    print('wrote', t3x)


if __name__ == '__main__':
    main()
