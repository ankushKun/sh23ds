#!/usr/bin/env python3
"""Re-encode Silent Hill 2 (PC Director's Cut) Bink movies for sh23ds (port/source/movie.c).

  movies.py [--data <game data dir>] [--out pack] [--only toilet,open] [--trailers]
            [--fit pillarbox|crop] [--q 3] [--abr 128] [-j N]

Needs ffmpeg with the Bink decoder (Homebrew's ffmpeg has it). Writes
  <out>/movie/<name>.mpg   MPEG-1 program stream: mpeg1video at 29.97 fps plus one MP2 stream per
                           Bink audio track (32 kHz stereo). Plays in any player (ffplay) too.
  <out>/movie/movies.txt   name, picture size, seconds, audio tracks, bytes and the source (for
                           reference, and to re-encode a movie whose source changed)

With the Enhanced Edition's FMV Enhancement Pack (or its Credits package) installed, the source is
its sh2e/movie/<name>.bik (ee.py): Bink 1 like the originals, at 1920x1080 or 1920x1230 and partly
59.94 fps, with the same audio tracks. picture() fits any size and -r keeps 29.97 fps.

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
import concurrent.futures
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ee  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_DATA = os.path.join(ROOT, 'Silent Hill 2', 'data')
DEFAULT_OUT = os.path.join(ROOT, 'pack')

STORY = ['credits', 'deai', 'end', 'end_dog', 'end_wish', 'flash', 'gero', 'hakaba', 'hei', 'knife',
         'korosu_a', 'korosu_b', 'murder', 'open', 'rouya', 'saikai', 'toilet', 'water']
TRAILERS = ['sh2e3', 'sh3e3', 'sh3tgs']
TRACKS = {'open': 2, 'end': 4}      # audio tracks the game selects (see above); others use track 0

SCREEN_W, SCREEN_H = 400, 240
FPS = '30000/1001'                  # MPEG-1 only allows the standard rates; Bink's 29.94..29.97 map here


def probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                          'format=duration:stream=codec_type,width,height', '-of', 'json', path],
                         check=True, capture_output=True, text=True).stdout
    j = json.loads(out)
    v = next(s for s in j['streams'] if s['codec_type'] == 'video')
    na = sum(1 for s in j['streams'] if s['codec_type'] == 'audio')
    return v['width'], v['height'], float(j['format']['duration']), na


def picture(w, h, fit):
    """Encoded size and the ffmpeg filter that makes it (sizes stay multiples of 16 for MPEG-1)."""
    if w * SCREEN_H >= h * SCREEN_W:                 # 5:3 or wider: fill the width
        eh = min(SCREEN_H, round(h * SCREEN_W / w / 16) * 16)
        return SCREEN_W, eh, 'scale=%d:%d:flags=lanczos' % (SCREEN_W, eh)
    if fit == 'crop':
        sh = round(h * SCREEN_W / w / 2) * 2
        return SCREEN_W, SCREEN_H, 'scale=%d:%d:flags=lanczos,crop=%d:%d' % (SCREEN_W, sh, SCREEN_W, SCREEN_H)
    ew = min(SCREEN_W, round(w * SCREEN_H / h / 16) * 16)
    return ew, SCREEN_H, 'scale=%d:%d:flags=lanczos' % (ew, SCREEN_H)


def convert(name, src, dst, args):
    w, h, dur, na = probe(src)
    ew, eh, vf = picture(w, h, args.fit)
    tracks = min(TRACKS.get(name, 1), na)
    cmd = ['ffmpeg', '-v', 'error', '-y', '-i', src, '-map', '0:v:0']
    for t in range(tracks):
        cmd += ['-map', '0:a:%d' % t]
    cmd += ['-vf', vf + ',format=yuv420p', '-r', FPS,
            '-c:v', 'mpeg1video', '-q:v', str(args.q), '-mbd', 'rd', '-trellis', '2', '-cmp', '2',
            '-subcmp', '2', '-g', '60', '-bf', '2',
            '-c:a', 'mp2', '-ar', str(args.rate), '-ac', '2', '-b:a', '%dk' % args.abr,
            '-f', 'mpeg', dst + '.part']
    subprocess.run(cmd, check=True)
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
            w, h, dur, na = probe(dst)
            done.append((n, w, h, dur, na, os.path.getsize(dst), origin))
            continue
        jobs.append((n, src, dst, origin))
    with concurrent.futures.ThreadPoolExecutor(args.j) as ex:
        for r in ex.map(lambda j: convert(j[0], j[1], j[2], args) + (j[3],), jobs):
            print('%-10s %dx%d %6.1f s  %d track(s)  %5.1f MB  from %s' % (r[0], r[1], r[2], r[3], r[4], r[5] / 1e6, r[6]))
            done.append(r)

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
