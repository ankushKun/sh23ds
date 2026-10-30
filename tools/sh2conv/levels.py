#!/usr/bin/env python3
"""Loudness of the sound the 3DS streams as it is: the music, the scene voices and the movies.

  levels.py <dir> [--each] [--from <source dir> [--skip <subdir>]] <pattern>...
                                            (pack.py's music, demos and movies steps run it)

Writes <dir>/levels.txt, one line per file (with --each, per audio stream: a movie's tracks play one
at a time): the name under <dir>, the stream (-1 for all of them together), the integrated loudness
in LUFS and the sample peak in dBFS. Without --each a file's streams count as one sound with all of
them on, as an AIX file's layers are. --from measures the game's own files in place of the pack's,
each the Enhanced Edition's version when there (ee.py), for files the pack only copies: the browser
packer streams those into the pack without Python seeing them. The port takes a gain from these (port/source/levels.c) that
brings each kind of sound to one loudness, since the files were mastered far apart (port/SOUND.md,
"Levels").

The loudness is ITU-R BS.1770's: K-weighted (ffmpeg's biquads with the standard's shelf and high
pass), mean square over 400 ms blocks every 100 ms, gated at -70 LUFS and then 10 LU under the mean
of the blocks left. Sounds are decoded as stereo at 16 kHz, the way the 3DS plays a mono one through
both speakers; that is enough for the weighting and the gates, and keeps the work small.
"""
import fnmatch
import os
import sys

import host

RATE = 16000
K_WEIGHT = 'highshelf=f=1681.97:g=4.0:t=q:w=0.7072,highpass=f=38.135:p=2:t=q:w=0.5003'


def _decode(path, stream, weighted):
    import numpy as np
    argv = ['ffmpeg', '-v', 'error', '-i', path, '-map', '0:a:%d' % stream, '-ac', '2', '-ar', str(RATE)]
    if weighted:
        argv += ['-af', K_WEIGHT]
    pcm = host.run(argv + ['-f', 's16le', 'pipe:'], capture=True)
    return np.frombuffer(pcm, '<i2').reshape(-1, 2).astype(np.float64) / 32768.0


def streams(path):
    out = host.run(['ffprobe', '-v', 'error', '-select_streams', 'a', '-show_entries', 'stream=index',
                    '-of', 'csv=p=0', path], capture=True)
    return len(out.split())


def _add(total, part):
    import numpy as np
    if total is None:
        return part.copy()
    n = max(len(total), len(part))
    if len(total) < n:
        total = np.concatenate([total, np.zeros((n - len(total), 2))])
    total[:len(part)] += part
    return total


def loudness(k):
    """BS.1770 integrated loudness of K-weighted stereo samples (n x 2)."""
    import numpy as np
    block, hop = int(0.4 * RATE), int(0.1 * RATE)
    if len(k) < block:
        return -70.0
    sq = np.concatenate([np.zeros((1, 2)), np.cumsum(k * k, axis=0)])
    starts = np.arange(0, len(k) - block + 1, hop)
    z = ((sq[starts + block] - sq[starts]) / block).sum(axis=1)
    with np.errstate(divide='ignore'):
        lk = -0.691 + 10 * np.log10(z)
    keep = lk > -70.0
    if not keep.any():
        return -70.0
    rel = -0.691 + 10 * np.log10(z[keep].mean()) - 10.0
    keep &= lk > rel
    return float(-0.691 + 10 * np.log10(z[keep].mean()))


def measure(path, stream_list):
    """(LUFS, peak dBFS) of the given streams played together."""
    import numpy as np
    raw = weighted = None
    for s in stream_list:
        raw = _add(raw, _decode(path, s, False))
        weighted = _add(weighted, _decode(path, s, True))
    if raw is None or not len(raw):
        return -70.0, -90.0
    peak = float(np.abs(raw).max())
    return loudness(weighted), 20 * np.log10(max(peak, 1e-6))


def main():
    args = sys.argv[1:]
    each = '--each' in args
    args = [a for a in args if a != '--each']
    src = skip = None
    for opt in ('--from', '--skip'):
        if opt in args:
            i = args.index(opt)
            if opt == '--from':
                src = args[i + 1]
            else:
                skip = args[i + 1]
            del args[i:i + 2]
    if len(args) < 2:
        sys.exit(__doc__)
    out, patterns = args[0], args[1:]
    root = src or out
    names = []
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if os.path.relpath(os.path.join(d, x), root) != skip)
        for f in files:
            rel = os.path.relpath(os.path.join(d, f), root).replace(os.sep, '/')
            if any(fnmatch.fnmatch(rel, p) or fnmatch.fnmatch(f, p) for p in patterns):
                names.append(rel)
    names.sort()
    if src:
        import ee

    done = [0]

    def one(rel):
        done[0] += 1
        host.progress(done[0] / max(1, len(names)), done[0], len(names), 'sounds measured')
        path = os.path.join(root, rel)
        if src:
            path = ee.file(path)
        try:
            n = streams(path)
            if not n:
                return []
            if each:
                return [(rel, s) + measure(path, [s]) for s in range(n)]
            return [(rel, -1) + measure(path, range(n))]
        except Exception as e:
            # a sound with no line plays at the level it was mastered at (levels.c), so one that can't
            # be measured costs only its evening out, not the pack
            print('levels.py: %s not measured, it keeps its own level (%s)' % (rel, str(e).splitlines()[0] if str(e) else type(e).__name__), flush=True)
            return []

    rows = [row for r in host.pmap(one, names, 4) for row in r]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'levels.txt'), 'w') as f:
        f.write('# name  stream  LUFS  peak dBFS  (tools/sh2conv/levels.py; stream -1: all of them together)\n')
        for rel, s, lufs, peak in rows:
            f.write('%s %d %.1f %.1f\n' % (rel, s, lufs, peak))
    print('levels.txt: %d sounds in %s' % (len(rows), out))


if __name__ == '__main__':
    main()
