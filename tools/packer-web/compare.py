#!/usr/bin/env python3
"""Compares a pack from the browser packer with one from make_pack.sh.

  compare.py <browser pack: folder or zip> <native pack folder> [--movies N]

Every file the 3DS reads must be in both (the native pack's png folders and .json files are left
out, as the browser packer leaves them out). Then, by kind:
  - .t3x textures: the header must match; level 0 matches byte for byte when the browser used "Same
    as Docker" textures, otherwise it is compared decoded (mean absolute error). Mipmaps are always
    compared decoded (different Lanczos filters; check.mip_error).
  - .mpg movies come from another ffmpeg build: compared by size and, for the first N (default 3),
    by the PSNR of the decoded video (needs ffmpeg). movie/movies.txt lists their byte sizes, so it
    may differ in that column only.
  - .s3c / .s3a / .s3k models and animations may differ by float rounding only (float_noise): numpy's
    linear algebra differs between Pyodide and a computer.
  - everything else must match byte for byte.
Exits 1 when a file is missing or a file that must match doesn't.
"""
import io
import os
import subprocess
import sys
import tempfile
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'texenc'))
sys.path.insert(0, os.path.join(HERE, '..', 'sh2conv'))
import pack  # noqa: E402

PREFIX = '3ds/sh23ds/'


class Tree:
    def __init__(self, path):
        self.zip = zipfile.ZipFile(path) if path.endswith('.zip') else None
        self.root = path
        if self.zip:
            self.names = {n[len(PREFIX):] for n in self.zip.namelist() if n.startswith(PREFIX) and not n.endswith('/')}
        else:
            self.names = {os.path.relpath(os.path.join(dp, f), path).replace(os.sep, '/')
                          for dp, _, fs in os.walk(path) for f in fs}

    def read(self, rel):
        if self.zip:
            return self.zip.read(PREFIX + rel)
        return open(os.path.join(self.root, rel), 'rb').read()


def t3x_compare(a, b):
    """(header same, level 0 same, level 0 PSNR, [mip PSNRs]) for two t3x files."""
    import check
    fa, la, ha = check.levels(a)
    fb, lb, hb = check.levels(b)
    if fa != fb or len(la) != len(lb):
        return False, False, (255.0, 0.0), []
    out = []
    for (w, h, x), (_, _, y) in zip(la, lb):
        dx, dy = check.decode(fa, w, h, x), check.decode(fb, w, h, y)
        out.append(check.mip_error(dx, dy) if dx is not None else ((0.0 if x == y else 255.0), 0.0))
    return ha == hb, la[0][2] == lb[0][2], out[0], out[1:]


def float_noise(a, b):
    """For two model files of the same layout: the largest difference between 32-bit words that differ,
    read as floats, or None when one of them is not a float rounding apart (a different integer, a
    different size). numpy's linear algebra (LAPACK) differs between Pyodide and a computer, so bone
    matrices can differ in the last bit or as -0.0 against 0.0, and values that are zero in effect come
    out as 1e-17 on one side and 0 on the other."""
    if len(a) != len(b):
        return None
    n = len(a) // 4
    wa, wb = np.frombuffer(a, '<u4', n), np.frombuffer(b, '<u4', n)
    d = np.nonzero(wa != wb)[0]
    if len(d) == 0:
        return 0.0 if a == b else None
    fa, fb = wa[d].view('<f4').astype(np.float64), wb[d].view('<f4').astype(np.float64)
    gap = np.abs(fa - fb)
    ok = (gap <= 1e-5 * np.maximum(1.0, np.abs(fb))) & np.isfinite(fa) & np.isfinite(fb)
    return float(gap.max()) if ok.all() and a[4 * n:] == b[4 * n:] else None


def movie_psnr(a, b):
    with tempfile.TemporaryDirectory() as d:
        pa, pb = os.path.join(d, 'a.mpg'), os.path.join(d, 'b.mpg')
        open(pa, 'wb').write(a)
        open(pb, 'wb').write(b)
        r = subprocess.run(['ffmpeg', '-v', 'info', '-i', pa, '-i', pb, '-lavfi', '[0:v][1:v]psnr', '-f', 'null', '-'],
                           capture_output=True, text=True)
        for line in r.stderr.splitlines():
            if 'PSNR' in line and 'average:' in line:
                return float(line.split('average:')[1].split()[0].replace('inf', '99'))
    return 0.0


def main():
    web, native = Tree(sys.argv[1]), Tree(sys.argv[2])
    nmov = int(sys.argv[sys.argv.index('--movies') + 1]) if '--movies' in sys.argv else 3
    want = {n for n in native.names if not pack.is_extra(n) and not n.startswith('.steps')}
    missing, extra = sorted(want - web.names), sorted(web.names - want)
    same = 0
    bad, notes = [], []
    tex = {'files': 0, 'header same': 0, 'level 0 same': 0, 'l0': [], 'mips': []}
    movies = []
    noise = []
    for n in sorted(want & web.names):
        a, b = web.read(n), native.read(n)
        if a == b:
            same += 1
            if n.endswith('.t3x'):
                tex['files'] += 1
                tex['header same'] += 1
                tex['level 0 same'] += 1
            continue
        if n.endswith('.t3x'):
            hs, l0s, l0, mips = t3x_compare(a, b)
            tex['files'] += 1
            tex['header same'] += hs
            tex['level 0 same'] += l0s
            tex['l0'].append(l0)
            tex['mips'] += mips
            if not hs:
                bad.append(n + ' (t3x header)')
        elif n.endswith('.mpg'):
            movies.append((n, len(a), len(b), movie_psnr(a, b) if len(movies) < nmov else None))
        elif n.endswith(('.s3c', '.s3a', '.s3k')) and float_noise(a, b) is not None:
            noise.append(float_noise(a, b))
        elif n == 'sound/se_index.txt':
            # a listing for people; its notes come from the Enhanced Edition's SFX map when the developer
            # has it checked out (sound.py EE_MAP), so only the numbers must match
            cols = lambda t: [x.split()[:4] for x in t.decode().splitlines()]
            if cols(a) != cols(b):
                bad.append(n)
            else:
                notes.append('sound/se_index.txt differs only in its notes (sound.py EE_MAP)')
        elif n.endswith('.png'):
            # the same pixels can be compressed differently by another Pillow build
            from PIL import Image
            pa, pb = Image.open(io.BytesIO(a)), Image.open(io.BytesIO(b))
            if pa.size != pb.size or pa.mode != pb.mode or pa.tobytes() != pb.tobytes():
                bad.append(n)
            else:
                notes.append(n + ': same pixels, compressed differently')
        elif n == 'movie/movies.txt':
            la, lb = a.decode().splitlines(), b.decode().splitlines()
            cut = lambda s: s.split()[:5] + s.split()[6:]   # every column but the byte size
            if len(la) != len(lb) or any(cut(x) != cut(y) for x, y in zip(la, lb) if not x.startswith('#')):
                bad.append(n)
            else:
                notes.append('movie/movies.txt differs only in the byte sizes')
        else:
            bad.append(n)
    print('files the 3DS reads: %d in the native pack, %d in the browser one' % (len(want), len(web.names)))
    print('identical: %d' % same)
    if missing:
        print('MISSING from the browser pack (%d): %s' % (len(missing), ', '.join(missing[:10])))
    if extra:
        print('only in the browser pack (%d): %s' % (len(extra), ', '.join(extra[:10])))
    print('textures: %d, header same %d, level 0 byte-identical %d' % (tex['files'], tex['header same'], tex['level 0 same']))
    if tex['l0']:
        l0 = np.array(tex['l0'])
        print('  level 0 where it differs (%d): mean abs error vs tex3ds median %.2f, worst %.2f (of 255)'
              % (len(l0), np.median(l0[:, 0]), l0[:, 0].max()))
    if tex['mips']:
        m = np.array(tex['mips'])
        print('  mipmaps (%d levels): mean abs error median %.2f, 99th percentile %.2f, worst %.2f; '
              'pixels tex3ds wrapped to black %.3f%%' % (len(m), np.median(m[:, 0]), np.percentile(m[:, 0], 99),
                                                        m[:, 0].max(), 100 * m[:, 1].mean()))
    if movies:
        r = [w / n for _, w, n, _ in movies]
        print('movies that differ: %d, size browser/native %.2f..%.2f' % (len(movies), min(r), max(r)))
        for n, w, nn, p in movies:
            if p is not None:
                print('  %s: %.1f dB PSNR, %d vs %d bytes' % (n, p, w, nn))
    if noise:
        print('models and animations that differ only by float rounding: %d, largest gap %.2g'
              % (len(noise), max(noise)))
    for x in notes:
        print(x)
    if bad:
        print('DIFFERENT (%d): %s' % (len(bad), ', '.join(bad[:20])))
    ok = not missing and not bad
    print('OK' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
