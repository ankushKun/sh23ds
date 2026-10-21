#!/usr/bin/env python3
"""Re-encode Silent Hill 2 (PC Director's Cut) Bink movies for sh23ds (port/source/movie.c).

  movies.py [--data <game data dir>] [--out pack] [--only toilet,open] [--trailers]
            [--fit pillarbox|crop] [--q 3] [--abr 128] [--half] [-j N]

Needs ffmpeg with the Bink decoder (Homebrew's ffmpeg has it). Writes
  <out>/movie/<name>.mpg   MPEG-1 program stream: mpeg1video at 29.97 fps plus one MP2 stream per
                           Bink audio track (32 kHz stereo). Plays in any player (ffplay) too.
  <out>/movie/movies.txt   name, picture size, seconds, audio tracks, bytes and the source (for
                           reference, and to re-encode a movie whose source changed)

With the Enhanced Edition's FMV Enhancement Pack (or its Credits package) installed, the source is
its sh2e/movie/<name>.bik (ee.py): Bink 1 like the originals, at 1920x1080 or 1920x1230 and partly
59.94 fps, with the same audio tracks. picture() fits any size and -r keeps 29.97 fps.
  <out>/movie/half/<name>.mpg   with --half: the same at half the width and height (200x120 or
                           160x120) for the Old 3DS profile (port/OLD3DS.md), which plays them at
                           twice the size and so decodes a quarter of the pixels

Why this format: the 3DS has no Bink decoder, and pl_mpeg (single header, MIT) decodes both MPEG-1
video and MP2 audio from one program stream with little code. MPEG-1 at 400x240 is cheap enough for
one New 3DS core, and MP2 at 128 kbps is half the size of IMA-ADPCM at the same rate.

Picture size. The top screen is 400x240 (5:3). Most story movies are 640x384, which is 5:3 as well,
so they scale to 400x240 exactly. The 640x480 (4:3) movies are full frame with credits text near
the edges, so by default they become 320x240 and the player centres them between black bars
(--fit pillarbox). --fit crop scales them to 400x300 and keeps the middle 240 lines instead.

Audio tracks. Movie_BinkPlay (0x43d770) opens some movies with BinkSetSoundTrack: open.bik track 0
is "open_bgm" and track 1 "open_voc"; end.bik tracks 0..3 are "end_mria", "end_rbrt", "end_scde"
and "ending" (the staff roll with each ending's song). Those files keep every track as its own MP2
stream (same order); movie.c maps the PC names onto file and stream. end_dog.bik has two identical
tracks, so only the first is kept. Every other file has one track.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ee  # noqa: E402
import host  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_DATA = os.path.join(ROOT, 'Silent Hill 2', 'data')
DEFAULT_OUT = os.path.join(ROOT, 'pack')

STORY = ['credits', 'deai', 'end', 'end_dog', 'end_wish', 'flash', 'gero', 'hakaba', 'hei', 'knife',
         'korosu_a', 'korosu_b', 'murder', 'open', 'rouya', 'saikai', 'toilet', 'water']
TRAILERS = ['sh2e3', 'sh3e3', 'sh3tgs']
TRACKS = {'open': 2, 'end': 4}      # audio tracks the game selects (see above); others use track 0

SCREEN_W, SCREEN_H = 400, 240
FPS = '30000/1001'                  # MPEG-1 only allows the standard rates; Bink's 29.94..29.97 map here


class Unusable(Exception):
    """A movie that cannot be converted; the pack goes on without it and the port skips it."""


def probe(path):
    """Width, height, seconds and audio track count, or None when there is no playable video in the
    file. ffprobe fails on some broken files and on others exits 0 and leaves out "streams" (a
    truncated file, or one encoded from a repack's emptied-out .bik), so both count as None."""
    try:
        out = host.run(['ffprobe', '-v', 'error', '-show_entries',
                        'format=duration:stream=codec_type,width,height', '-of', 'json', path],
                       capture=True)
    except subprocess.CalledProcessError:
        return None
    j = json.loads(out)
    streams = j.get('streams', [])
    v = next((s for s in streams if s.get('codec_type') == 'video'), None)
    if v is None or not v.get('width') or not v.get('height') or 'duration' not in j.get('format', {}):
        return None
    na = sum(1 for s in streams if s.get('codec_type') == 'audio')
    return v['width'], v['height'], float(j['format']['duration']), na


def picture(w, h, fit, half=False):
    """Encoded size and the ffmpeg filter that makes it (sizes stay multiples of 16 for MPEG-1)."""
    if half:
        # half of the full-size picture, rounded to whole 8 pixel blocks (pl_mpeg pads to 16)
        fw, fh, _ = picture(w, h, fit)
        hw, hh = fw // 2 // 8 * 8, fh // 2 // 8 * 8
        vf = 'scale=%d:%d:flags=lanczos' % (hw, hh)
        if fit == 'crop' and w * SCREEN_H < h * SCREEN_W:
            sh = round(h * hw / w / 2) * 2
            vf = 'scale=%d:%d:flags=lanczos,crop=%d:%d' % (hw, sh, hw, hh)
        return hw, hh, vf
    if w * SCREEN_H >= h * SCREEN_W:                 # 5:3 or wider: fill the width
        eh = min(SCREEN_H, round(h * SCREEN_W / w / 16) * 16)
        return SCREEN_W, eh, 'scale=%d:%d:flags=lanczos' % (SCREEN_W, eh)
    if fit == 'crop':
        sh = round(h * SCREEN_W / w / 2) * 2
        return SCREEN_W, SCREEN_H, 'scale=%d:%d:flags=lanczos,crop=%d:%d' % (SCREEN_W, sh, SCREEN_W, SCREEN_H)
    ew = min(SCREEN_W, round(w * SCREEN_H / h / 16) * 16)
    return ew, SCREEN_H, 'scale=%d:%d:flags=lanczos' % (ew, SCREEN_H)


def convert(name, src, dst, args, half=False):
    info = probe(src)
    if info is None:
        raise Unusable('%s has no video ffmpeg can read' % src)
    w, h, dur, na = info
    ew, eh, vf = picture(w, h, args.fit, half)
    tracks = min(TRACKS.get(name, 1), na)
    cmd = ['ffmpeg', '-v', 'error', '-y', '-i', src, '-map', '0:v:0']
    for t in range(tracks):
        cmd += ['-map', '0:a:%d' % t]
    cmd += ['-vf', vf + ',format=yuv420p', '-r', FPS,
            '-c:v', 'mpeg1video', '-q:v', str(args.q), '-mbd', 'rd', '-trellis', '2', '-cmp', '2',
            '-subcmp', '2', '-g', '60', '-bf', '2',
            '-c:a', 'mp2', '-ar', str(args.rate), '-ac', '2', '-b:a', '%dk' % args.abr,
            '-f', 'mpeg', dst + '.part']
    try:
        host.run(cmd)
        made = probe(dst + '.part')
    except subprocess.CalledProcessError:
        made = None
    if made is None:
        # ffmpeg exits 0 on a .bik with no frames and writes an empty or header-only file
        if os.path.exists(dst + '.part'):
            os.remove(dst + '.part')
        raise Unusable('%s did not convert to a playable movie' % src)
    os.replace(dst + '.part', dst)
    return name, ew, eh, dur, tracks, os.path.getsize(dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--data', default=DEFAULT_DATA, help='the game data folder (with movie/)')
    ap.add_argument('--out', default=DEFAULT_OUT, help='pack folder; movies go to <out>/movie')
    ap.add_argument('--only', default='', help='comma separated movie names')
    ap.add_argument('--trailers', action='store_true', help='also convert sh2e3, sh3e3, sh3tgs')
    ap.add_argument('--fit', choices=['pillarbox', 'crop'], default='pillarbox', help='4:3 movies')
    ap.add_argument('--q', type=int, default=3, help='MPEG-1 quantiser (2 best .. 31 worst)')
    ap.add_argument('--abr', type=int, default=128, help='MP2 kbit/s for the stereo pair')
    ap.add_argument('--rate', type=int, default=32000, choices=[32000, 44100, 48000])
    ap.add_argument('--force', action='store_true', help='re-encode even when up to date')
    ap.add_argument('--half', action='store_true',
                    help='also write movie/half/ at half size for the Old 3DS profile')
    ap.add_argument('-j', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    args = ap.parse_args()

    names = STORY + (TRAILERS if args.trailers else [])
    if args.only:
        names = [n.strip() for n in args.only.split(',') if n.strip()]
    src_dir = os.path.join(args.data, 'movie')
    out_dir = os.path.join(args.out, 'movie')
    os.makedirs(out_dir, exist_ok=True)

    # the source each movie was made from (the last column of movies.txt; "data" before it had one)
    index = os.path.join(out_dir, 'movies.txt')
    rows, made_from = {}, {}
    if os.path.exists(index):
        for line in open(index):
            p = line.split()
            if len(p) >= 5 and not line.startswith('#'):
                rows[p[0]] = line.rstrip('\n')
                made_from[p[0]] = p[6] if len(p) >= 7 else 'data'

    jobs, done = [], []
    for n in names:
        src = ee.file(os.path.join(src_dir, n + '.bik'))
        dst = os.path.join(out_dir, n + '.mpg')
        if not os.path.exists(src):
            print('missing', src, file=sys.stderr)
            continue
        origin = 'ee:%d' % os.path.getsize(src) if ee.is_ee(src) else 'data'
        if (not args.force and os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src)
                and made_from.get(n, 'data') == origin):
            info = probe(dst)
            if info is not None:
                done.append((n,) + info + (os.path.getsize(dst), origin))
                continue
            print('re-encoding %s, the earlier one is damaged' % dst, file=sys.stderr)
        jobs.append((n, src, dst, origin))
    def one(jp):
        k, (j, (lo, hi)) = jp
        host.progress(lo, k, len(jobs), 'movies, now ' + j[0])
        try:
            with host.part(lo, hi):      # the step's progress bar, by each movie's share of the bytes
                r = convert(j[0], j[1], j[2], args) + (j[3],)
        except Unusable as e:
            skipped.append((j[0], str(e)))
            return None
        print('%-10s %dx%d %6.1f s  %d track(s)  %5.1f MB  from %s' % (r[0], r[1], r[2], r[3], r[4], r[5] / 1e6, r[6]),
              flush=True)
        return r
    skipped = []
    done += [r for r in host.pmap(one, enumerate(zip(jobs, host.parts([os.path.getsize(j[1]) for j in jobs]))),
                                  args.j) if r]
    if args.half:
        half_dir = os.path.join(out_dir, 'half')
        os.makedirs(half_dir, exist_ok=True)
        hjobs = []
        for n in names:
            src = os.path.join(src_dir, n + '.bik')
            if 'ee' in globals():
                src = ee.file(src)      # the Enhanced Edition's movie when it is installed
            dst = os.path.join(half_dir, n + '.mpg')
            if os.path.exists(src) and (args.force or not os.path.exists(dst) or
                                        os.path.getmtime(dst) < os.path.getmtime(src)):
                hjobs.append((n, src, dst))
        def half(j):
            try:
                r = convert(j[0], j[1], j[2], args, True)
            except Unusable:
                return          # already reported by the full-size pass
            print('half %-10s %dx%d %5.1f MB' % (r[0], r[1], r[2], r[5] / 1e6), flush=True)
        host.pmap(half, hjobs, args.j)

    # A movie that could not be made is left out entirely, since a stale or broken .mpg would still
    # count as present to movie_exists() in the port
    for n, why in skipped:
        rows.pop(n, None)
        for f in (os.path.join(out_dir, n + '.mpg'), os.path.join(out_dir, 'half', n + '.mpg')):
            if os.path.exists(f):
                os.remove(f)
        print('skipped %s: %s. This copy of the game has a damaged or stripped movie file; the port '
              'plays on without it, or copy movie/%s.bik from a complete install and pack again'
              % (n, why, n), file=sys.stderr)

    # Keep lines for movies converted earlier that this run did not touch
    for n, w, h, dur, tracks, size, origin in done:
        rows[n] = '%-10s %3d %3d %7.2f %d %9d %s' % (n, w, h, dur, tracks, size, origin)
    with open(index, 'w') as f:
        f.write('# name width height seconds audio_tracks bytes source (data, or ee:<bytes> for the Enhanced Edition\'s)\n')
        for n in sorted(rows):
            f.write(rows[n] + '\n')
    total = sum(r[5] for r in done)
    print('%d movies, %.1f MB in %s' % (len(done), total / 1e6, out_dir))


if __name__ == '__main__':
    main()
