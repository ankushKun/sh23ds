#!/usr/bin/env python3
"""Compares the browser packer's textures (tools/sh2conv/t3x.py + texenc) with tex3ds's.

  check.py <pack> [--limit N] [-j processes]

For every .t3x in a pack that make_pack.sh built with its png folders kept, re-encodes the PNG it
came from (<dir>/png/<name>.png next to <dir>/tex/<name>.t3x, or in the same folder) in the format
and with the mipmaps the tex3ds file says, then compares: the header, level 0 byte for byte, and for
the mipmaps of ETC1 / ETC1A4 textures the error between the two decoded levels (they come from
different Lanczos filters, so they are not expected to match bytes; see mip_error). Needs build/libtexenc.so (build.sh native).
"""
import ctypes
import math
import os
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'sh2conv'))
os.environ.setdefault('TEXENC_LIB', os.path.join(HERE, 'build', 'libtexenc.so'))
import t3x  # noqa: E402

NAMES = {v: k for k, v in reversed(list(t3x.FORMATS.items()))}
LIB = ctypes.CDLL(os.environ['TEXENC_LIB'])


def levels(blob):
    """(format, [(w, h, bytes)], header) of a t3x file."""
    fmt, nmip = blob[3], blob[4]
    lw, lh = (blob[2] & 7) + 3, (blob[2] >> 3 & 7) + 3
    w, h = 1 << lw, 1 << lh
    p = 17 + (8 if blob[17] & 0x80 else 4)
    out = []
    for _ in range(nmip + 1):
        n = LIB.texenc_size(fmt, w, h)
        out.append((w, h, blob[p:p + n]))
        p += n
        w, h = w // 2, h // 2
    return fmt, out, blob[:17]


def decode(fmt, w, h, data):
    px = ctypes.create_string_buffer(w * h * 4)
    if LIB.texenc_decode_etc1(fmt, data, w, h, px) != 0:
        return None
    return np.frombuffer(px.raw, np.uint8).reshape(h, w, 4).astype(np.float64)


def psnr(a, b):
    mse = np.mean((a - b) ** 2)
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def mip_error(a, b):
    """(mean absolute error over the pixels that are off by at most 128, share of pixels off by more).
    tex3ds's ImageMagick resizes in floating point, and a Lanczos overshoot past white at a bright edge
    wraps round to black when it becomes 8 bits, so its mipmaps carry a few black pixels there that a
    plain PSNR would let dominate. Those count as 'wrapped'; the error is measured on the rest."""
    e = np.abs(a - b).max(axis=2)
    ok = e <= 128
    return (float(np.abs(a - b)[ok].mean()) if ok.any() else 255.0), float(1 - ok.mean())


def source_png(t3x_path):
    d, name = os.path.split(t3x_path)
    stem = os.path.splitext(name)[0]
    for cand in (os.path.join(os.path.dirname(d), 'png', stem + '.png'), os.path.join(d, 'png', stem + '.png'),
                 os.path.join(d, stem + '.png')):
        if os.path.exists(cand):
            return cand
    return None


def one(path):
    """(png found, header same, level 0 same, file same, [(mip PSNR, level width)]) for one t3x."""
    png = source_png(path)
    if not png:
        return None
    ref = open(path, 'rb').read()
    fmt, ref_lv, ref_head = levels(ref)
    fd, out = tempfile.mkstemp(suffix='.t3x')
    os.close(fd)
    try:
        t3x.encode(png, out, NAMES[fmt], mipmaps=len(ref_lv) > 1)
        mine = open(out, 'rb').read()
    finally:
        os.remove(out)
    _, my_lv, my_head = levels(mine)
    mips = []
    for (w, h, a), (_, _, b) in zip(ref_lv[1:], my_lv[1:]):
        da, db = decode(fmt, w, h, a), decode(fmt, w, h, b)
        if da is not None:
            mips.append(mip_error(da, db) + (w,))
    return my_head == ref_head, my_lv[0][2] == ref_lv[0][2], mine == ref, mips


def main():
    pack = sys.argv[1]
    limit = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else 0
    jobs = int(sys.argv[sys.argv.index('-j') + 1]) if '-j' in sys.argv else (os.cpu_count() or 1)
    files = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(pack) for f in fs if f.endswith('.t3x'))
    if limit:
        files = files[:limit]
    stats = {'t3x': len(files), 'no png': 0, 'checked': 0, 'header same': 0, 'level 0 same': 0, 'file same': 0}
    mips, worst, diffs = [], [], []
    import multiprocessing
    with multiprocessing.Pool(jobs) as pool:
        for path, r in zip(files, pool.imap(one, files, chunksize=8)):
            if r is None:
                stats['no png'] += 1
                continue
            hs, l0, fs, m = r
            stats['checked'] += 1
            stats['header same'] += hs
            stats['level 0 same'] += l0
            stats['file same'] += fs
            if not (hs and l0):
                diffs.append(os.path.relpath(path, pack))
            for err, wrapped, w in m:
                mips.append((err, wrapped))
                worst.append((err, os.path.relpath(path, pack), w))
    for k, v in stats.items():
        print('%-14s %d' % (k, v))
    if diffs:
        print('level 0 or header differs: %d, e.g. %s' % (len(diffs), ', '.join(diffs[:8])))
    if mips:
        m = np.array(mips)
        print('mipmaps vs tex3ds, decoded, over %d levels: mean abs error median %.2f, 99th percentile %.2f, '
              'worst %.2f (of 255); pixels tex3ds wrapped to black: %.3f%%'
              % (len(m), np.median(m[:, 0]), np.percentile(m[:, 0], 99), m[:, 0].max(), 100 * m[:, 1].mean()))
        for v, p, w in sorted(worst, reverse=True)[:5]:
            print('   %.2f  %s (level %d px wide)' % (v, p, w))
    sys.exit(1 if diffs else 0)


if __name__ == '__main__':
    main()
