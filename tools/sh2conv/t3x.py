"""Writes .t3x textures the way "tex3ds -f <fmt> -q high [-m lanczos] -z none" does, for the browser
packer, which cannot run tex3ds (convert.encode_textures calls this there).

tex3ds (devkitPro) reads the PNG with ImageMagick, lays it over a transparent canvas (fully
transparent pixels come out black), widens it to powers of two, makes each mipmap from the full
image with a Lanczos filter, encodes every level in 8x8 tiles and writes its header and a
"no compression" header before the data. Here Pillow reads the PNG and makes the mipmaps, and
tools/texenc (tex3ds's encoders and rg_etc1, as WebAssembly in the browser) encodes the pixels.
Level 0 comes out byte for byte as tex3ds's; the smaller mipmaps differ slightly, since Pillow's
Lanczos is not ImageMagick's (tools/texenc/check.py measures both).
"""
import math
import os
import struct

import numpy as np
from PIL import Image

FORMATS = {
    'rgba': 0x00, 'rgba8': 0x00, 'rgba8888': 0x00, 'rgb': 0x01, 'rgb8': 0x01, 'rgb888': 0x01,
    'rgba5551': 0x02, 'rgb565': 0x03, 'rgba4': 0x04, 'rgba4444': 0x04, 'la': 0x05, 'la8': 0x05,
    'la88': 0x05, 'hilo': 0x06, 'hilo8': 0x06, 'hilo88': 0x06, 'l': 0x07, 'l8': 0x07, 'a': 0x08,
    'a8': 0x08, 'la4': 0x09, 'la44': 0x09, 'l4': 0x0a, 'a4': 0x0b, 'etc1': 0x0c, 'etc1a4': 0x0d,
}

_encoder = None


def _encode_level(fmt, rgba, w, h):
    """One level's bytes, from the browser's sh2host.texture or the native texenc library."""
    global _encoder
    if _encoder is None:
        import host
        if host.BROWSER:
            from js import sh2host
            from pyodide.ffi import to_js

            def enc(f, px, ww, hh):
                return sh2host.texture(f, to_js(px), ww, hh).to_bytes()
        else:
            import ctypes
            here = os.path.dirname(os.path.abspath(__file__))
            lib = os.environ.get('TEXENC_LIB') or os.path.join(here, '..', 'texenc', 'build', 'libtexenc.so')
            dll = ctypes.CDLL(lib)
            dll.texenc_size.restype = ctypes.c_int
            dll.texenc_level.restype = ctypes.c_int

            def enc(f, px, ww, hh):
                out = ctypes.create_string_buffer(dll.texenc_size(f, ww, hh))
                n = dll.texenc_level(f, px, ww, hh, out)
                return out.raw[:n]
        _encoder = enc
    return _encoder(fmt, rgba, w, h)


def pot_ceil(x):
    return 8 if x < 8 else int(2.0 ** math.ceil(math.log2(x)))


def load(path):
    """The PNG as tex3ds sees it: RGBA, with fully transparent pixels black (its composite over a
    transparent canvas), on a power-of-two canvas. Returns (image, its own width and height)."""
    im = Image.open(path)
    im.load()
    im = im.convert('RGBA')
    a = np.array(im)
    a[a[:, :, 3] == 0, :3] = 0
    w, h = im.size
    ow, oh = pot_ceil(w), pot_ceil(h)
    if (ow, oh) != (w, h):
        canvas = np.zeros((oh, ow, 4), np.uint8)
        canvas[:h, :w] = a
        a = canvas
    return Image.fromarray(a, 'RGBA'), w, h


def header(fmt, w, h, ow, oh, mipmaps):
    """tex3ds's write_tex3ds_header for one sub-image covering w x h of the ow x oh texture."""
    f32 = np.float32
    lw = int(math.log(ow) / math.log(2.0))
    lh = int(math.log(oh) / math.log(2.0))
    left, top = f32(0.0), f32(1.0) - f32(0) / f32(oh)
    right, bottom = f32(w) / f32(ow), f32(1.0) - f32(h) / f32(oh)
    sw = int(f32(right - left) * f32(ow))
    sh = int(f32(top - bottom) * f32(oh))
    nmip = min(lw, lh) - 3 if mipmaps else 0
    fixed = [int(f32(v) * f32(1024)) for v in (left, top, right, bottom)]   # tex3ds's encode<float>
    return (struct.pack('<HBBB', 1, (lw - 3) | (lh - 3) << 3, fmt, nmip) +
            struct.pack('<HH4H', sw, sh, *fixed))


def encode(png, out, fmt_name, mipmaps=True):
    fmt = FORMATS[fmt_name.lower()]
    im, w, h = load(png)
    ow, oh = im.size
    levels = [im]
    if mipmaps and ow > 8 and oh > 8:
        lw, lh = ow, oh
        while lw > 8 and lh > 8:
            lw, lh = lw // 2, lh // 2
            levels.append(im.resize((lw, lh), Image.LANCZOS))
    data = b''.join(_encode_level(fmt, lv.tobytes(), lv.size[0], lv.size[1]) for lv in levels)
    n = len(data)
    comp = bytes([0, n & 0xff, n >> 8 & 0xff, n >> 16 & 0xff])
    if n >= 0x1000000:
        comp = bytes([0x80, n & 0xff, n >> 8 & 0xff, n >> 16 & 0xff, n >> 24 & 0xff, 0, 0, 0])
    body = comp + data
    body += b'\0' * (-len(body) % 4)
    with open(out, 'wb') as f:
        f.write(header(fmt, w, h, ow, oh, mipmaps) + body)


def encode_jobs(jobs, pack_root, fmt=None, mipmaps=True, what=None):
    """convert.encode_textures's jobs: (png, t3x, has_alpha), relative to pack_root. what names them
    for the progress count ('textures in ap'), when the caller wants one shown."""
    import host
    for i, (png, t3x, alpha) in enumerate(jobs):
        encode(os.path.join(pack_root, png), os.path.join(pack_root, t3x),
               fmt or ('etc1a4' if alpha else 'etc1'), mipmaps)
        if what:     # only where the textures are the step's main work (convert.py's rooms)
            host.progress((i + 1) / len(jobs), i + 1, len(jobs), what)
