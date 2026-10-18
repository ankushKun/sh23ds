#!/usr/bin/env python3
"""Builds the sh23ds SD card pack from your own Silent Hill 2 PC Director's Cut install.

  pack.py <Silent Hill 2 folder> [output dir, default ./pack] [--strip-extras]

This is the packer's step list. tools/make_pack.sh and the Docker image run it with the computer's
Python; the browser packer (site/packer/) runs the same file in Pyodide, one step at a time through
Packer, so both build the same pack. Each converter runs in this process, as `python3 <script>`
would run it (runpy).

Environment, as for make_pack.sh: FRESH=1 starts over instead of resuming at the step that stopped
(each finished step leaves a stamp in <out>/.steps until the end), HALF_MOVIES=1 adds the half-size
movies for the Old 3DS profile, and SH2EE / SH2EE_SKIP / SH2EE_DIR choose the Enhanced Edition files
(ee.py, EE_PACKS.md). --strip-extras (the Docker image) removes the inspection files that the 3DS
doesn't read: the png folders and the .json files, about 0.6 GB.
"""
import hashlib
import os
import runpy
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

DC_MD5 = 'f3f5360298b6d0f6f2e09cf24a0925d1'   # sh2pc.exe of the Director's Cut (its addresses are baked in)
# seconds each step took in the browser engine (fast textures, one core): the browser packer's weights
STEP_SECONDS = {'rooms': 860, 'collision': 2, 'james': 3, 'characters': 36, 'events': 2, 'lighting': 1, 'fog': 1,
                'ui': 1, 'pics': 13, 'maps': 6, 'music': 2, 'movies': 360, 'halfmovies': 150, 'sound': 13,
                'demos': 530, 'stagemodels': 77, 'shadows': 1}

CHARACTERS = [
    ('nse', 'nse.mdl', 'nse.anm'), ('xoo', 'xoo.mdl', 'xoo.anm'), ('tyu', 'tyu.mdl', 'tyu.anm'),
    ('red', 'red.mdl', 'red.anm'), ('mar', 'lll_mar.mdl', 'lll_mar.anm'), ('pap', 'pap.mdl', 'pap.anm'),
    ('oni', 'oni.mdl', 'oni.anm'), ('bos', 'bos.mdl', 'bos.anm'), ('arm', 'arm.mdl', 'arm.anm'),
    ('ike', 'ike.mdl', 'ike.anm'), ('edi', 'lll_edi.mdl', 'lll_edi.anm'),
    ('wp', 'wp_handgun.mdl'), ('wp', 'wp_shotgun.mdl'), ('wp', 'wp_riflgun.mdl'), ('wp', 'wp_pipe.mdl'),
    ('wp', 'wp_nata.mdl'), ('wp', 'wp_csaw.mdl'), ('wp', 'wp_sp.mdl'),
]
JAMES_ANIMS = ['jms_basic1.anm', 'jms_wpkaku.anm', 'jms_wpnone.anm', 'jms_wphand.anm', 'jms_wpshot.anm',
               'jms_wprifl.anm', 'jms_wppipe.anm', 'jms_wpnata.anm', 'jms_wpcsaw.anm', 'jms_wpsp.anm']


def is_extra(rel):
    """Files the 3DS doesn't read: the png folders and the .json files (kept for inspection)."""
    parts = rel.replace(os.sep, '/').split('/')
    return 'png' in parts[:-1] or rel.endswith('.json')


def exe_md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


class Packer:
    def __init__(self, game, out, half=None):
        # absolute, so the links of the characters step point at the game from anywhere
        self.game = os.path.abspath(game)
        self.out = os.path.abspath(out)
        self.data = os.path.join(game, 'data')
        self.exe = os.path.join(game, 'sh2pc.exe')
        self.half = os.environ.get('HALF_MOVIES', '0') == '1' if half is None else half

    # ------------------------------------------------------------------ helpers
    def runs(self, *calls, what=None):
        """Runs (weight, script, args...) calls in order; the weights split the step's progress bar,
        and what names the calls for its count ('models')."""
        import host
        for k, ((lo, hi), call) in enumerate(zip(host.parts([c[0] for c in calls]), calls)):
            if what:
                host.progress(lo, k, len(calls), what)
            with host.part(lo, hi):
                self.run(*call[1:])
        if what:
            host.progress(1.0, len(calls), len(calls), what)

    def run(self, script, *args):
        """Runs tools/sh2conv/<script> with these arguments, as its own command line would."""
        path = os.path.join(HERE, script)
        argv = sys.argv
        sys.argv = [path] + [str(a) for a in args]
        try:
            runpy.run_path(path, run_name='__main__')
        except SystemExit as e:
            if e.code not in (None, 0):
                raise RuntimeError('%s %s failed: %s' % (script, ' '.join(map(str, args)), e.code))
        finally:
            sys.argv = argv

    def check(self):
        """The game folder holds the Director's Cut exe and the data folder."""
        if not os.path.isfile(self.exe) or not os.path.isdir(self.data):
            raise RuntimeError('the game folder must hold sh2pc.exe and the data folder')
        s = exe_md5(self.exe)
        if s != DC_MD5:
            raise RuntimeError("sh2pc.exe is not the Director's Cut build this port expects (md5 %s)" % s)

    def begin(self, report=True):
        os.makedirs(self.out, exist_ok=True)
        if report:      # the browser's other workers leave the report to the first
            import ee
            ee.report(self.data)
        os.environ['SH2EE_LOG'] = os.path.join(self.out, 'ee_used.txt')

    def steps(self):
        """(id, title, function) in order."""
        s = [
            ('rooms', 'rooms (geometry, textures)', self.step_rooms),
            ('collision', 'collision', self.step_collision),
            ('james', 'James', self.step_james),
            ('characters', "enemies and weapons (Lying Figure, Mannequin, James' weapons and their moves)",
             self.step_characters),
            ('events', 'stage events and messages', self.step_events),
            ('lighting', 'room lighting and fog (DrawEnvData records in the exe)', self.step_lighting),
            ('fog', 'fog particle layer (.fcl areas, fog texture)', self.step_fog),
            ('ui', 'title art', self.step_ui),
            ('pics', 'stills for the stage programs (data/pic)', self.step_pics),
            ('maps', 'maps and memos', self.step_maps),
            ('music', 'music', self.step_music),
            ('movies', 'movies (MPEG-1 + MP2 for movie.c; needs ffmpeg)', self.step_movies),
        ]
        if self.half:
            s.append(('halfmovies', 'half-size movies for the Old 3DS (needs ffmpeg)', self.step_halfmovies))
        s += [
            ('sound', 'sound effects, footstep materials, per-stage BGM tables', self.step_sound),
            ('demos', 'cutscenes (scripts, motions, the characters they need, voices from voice.afs)',
             self.step_demos),
            ('stagemodels', 'stage-placed models (cars, furniture, props, puzzle objects)', self.step_stagemodels),
            ('shadows', 'character shadow models (.kg1 next to the S3C files) and per-area room indexes',
             self.step_shadows),
        ]
        return s

    def tasks(self):
        """The steps as tasks for the browser packer, which runs them in several workers at once.

        Each task is a dict: id, step (the step it belongs to, for the progress list), lane ('main':
        in one worker, in this order, since these steps read each other's files; 'any': any worker,
        as soon as one is free), weight (relative time, to pick the longest first and to fill the
        bars), ffmpeg (needs ffmpeg.wasm), after (tasks that must be done first, '*' as a suffix
        matches all with that prefix) and needs (pack files, as globs, that other workers made and
        this task reads: the browser copies them in first). Every worker that converted rooms also
        runs 'areaindex' at the end, over its own areas. The tasks write what the steps write."""
        t = []
        for sid in ('james', 'characters', 'events', 'fog', 'ui', 'pics', 'maps', 'music', 'sound', 'demos',
                    'stagemodels'):
            t.append({'id': sid, 'step': sid, 'lane': 'main', 'weight': STEP_SECONDS.get(sid, 5),
                      'ffmpeg': sid in ('sound', 'demos')})
        t.append({'id': 'shadows', 'step': 'shadows', 'lane': 'main', 'weight': 1})
        for bg in ('bg', 'bg2'):
            for area in sorted(os.listdir(os.path.join(self.data, bg))):
                d = os.path.join(self.data, bg, area)
                if os.path.isdir(d):
                    maps = sum(f.lower().endswith('.map') for f in os.listdir(d))
                    t.append({'id': 'rooms:%s:%s' % (bg, area), 'step': 'rooms', 'lane': 'any', 'weight': 2 * maps + 1})
        t.append({'id': 'movies', 'step': 'movies', 'lane': 'any', 'weight': STEP_SECONDS['movies'], 'ffmpeg': True})
        if self.half:
            t.append({'id': 'halfmovies', 'step': 'halfmovies', 'lane': 'any', 'weight': STEP_SECONDS['halfmovies'],
                      'ffmpeg': True})
        t.append({'id': 'lighting', 'step': 'lighting', 'lane': 'main', 'weight': 1, 'after': ['rooms:*'],
                  'needs': ['bg/*/rooms.txt']})
        return t

    def run_task(self, tid):
        """Runs one task of tasks(), or 'areaindex'."""
        import host
        host.reset()
        if tid.startswith('rooms:'):
            _, bg, area = tid.split(':')
            # the rooms step for one area: geometry and textures, then collision
            self.runs((20, 'convert.py', self.data, self.out, '--areas', area, '--bg', bg),
                      (1, 'collision.py', self.data, self.out, '--areas', area, '--bg', bg))
        elif tid == 'areaindex':
            self.run('areaindex.py', self.out)
        elif tid == 'shadows':
            self.run('shadows.py', self.data, self.out)
        else:
            dict((i, fn) for i, _, fn in self.steps())[tid]()
        host.progress(1.0)

    def run_step(self, sid):
        """Runs one step by id (the browser packer goes step by step)."""
        import host
        for i, _, fn in self.steps():
            if i == sid:
                host.reset()
                fn()
                host.progress(1.0)
                return
        raise KeyError(sid)

    def finish(self):
        log = os.path.join(self.out, 'ee_used.txt')
        if os.path.exists(log) and os.path.getsize(log):
            lines = sorted(set(open(log).read().splitlines()))
            with open(log, 'w') as f:
                f.write('\n'.join(lines) + '\n')
            print('Enhanced Edition files used: %d (see %s)' % (len(lines), log))

    # ------------------------------------------------------------------ the steps
    def step_rooms(self):
        self.runs((16, 'convert.py', self.data, self.out),
                  (1, 'convert.py', self.data, self.out, '--bg', 'bg2'))

    def step_collision(self):
        self.runs((16, 'collision.py', self.data, self.out),
                  (1, 'collision.py', self.data, self.out, '--bg', 'bg2'))

    def step_james(self):
        self.run('chr.py', self.data, self.out, 'jms', 'hll_jms.mdl', 'jms_basic1.anm')
        # animation clips inside jms_basic1 (frame ranges, loop flag)
        with open(os.path.join(self.out, 'chr', 'jms', 'clips.txt'), 'w') as f:
            f.write('idle 0 50 1\nwalk 51 91 1\nrun 92 170 1\n')

    def step_characters(self):
        import host
        d, o = self.data, self.out
        with host.part(0.0, 0.88):
            self.characters_main(d, o)
        with host.part(0.88, 1.0):
            self.characters_maria(d, o)

    def characters_main(self, d, o):
        self.runs((3, 'chr.py', d, o, 'jms', 'hll_jms.mdl', *JAMES_ANIMS),
                  (1, 'chr.py', d, o, 'scu', 'scu.mdl', 'scu.anm'),
                  (1, 'chr.py', d, o, 'mkn', 'mkn.mdl', 'mkn.anm'),
                  (1, 'chr.py', d, o, 'wp', 'wp_kakuzai.mdl'),
                  *[(1, 'chr.py', d, o) + c for c in CHARACTERS],
                  # Mary's moths (camera-facing quads with the PC's wing-beat texture), creeper nests
                  (1, 'moths.py', d, o),
                  (1, 'chr.py', d, o, 'item', 'ty2.mdl'),
                  (1, 'chr.py', d, o, 'item', 'ty3.mdl'), what='models')

    def characters_maria(self, d, o):
        # Maria as the Born From a Wish player: her models live in data/chr2, which chr.py reads
        # through a folder of links laid out as chr/xmar
        x = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(x, 'chr', 'xmar'))
            for f in ('lxx_mar.mdl', 'xmar_wpnone.anm', 'xmar_wpcolt.anm', 'xmar_wpknif.anm'):
                os.symlink(os.path.join(d, 'chr2', 'mar', f), os.path.join(x, 'chr', 'xmar', f))
            for f in ('wp_colt.mdl', 'wp_chinanife.mdl'):
                os.symlink(os.path.join(d, 'chr2', 'wp', f), os.path.join(x, 'chr', 'xmar', f))
            self.run('chr.py', x, o, 'xmar', 'lxx_mar.mdl', 'xmar_wpnone.anm', 'xmar_wpcolt.anm', 'xmar_wpknif.anm')
            self.run('chr.py', x, o, 'xmar', 'wp_colt.mdl')
            self.run('chr.py', x, o, 'xmar', 'wp_chinanife.mdl')
        finally:
            shutil.rmtree(x, ignore_errors=True)

    def step_events(self):
        self.run('events.py', '--exe', self.exe, '--data', self.data, '--out', os.path.join(self.out, 'events'))
        self.run('events_pack.py', self.data, self.out)
        self.run('enemies_pack.py', self.out)

    def step_lighting(self):
        self.run('lighting.py', '--exe', self.exe, '--pack', self.out)

    def step_fog(self):
        self.run('fog.py', self.data, self.out, self.exe)

    def step_ui(self):
        self.run('ui.py', '--exe', self.exe, self.data, self.out)

    def step_pics(self):
        self.run('pics.py', self.data, self.out, '--exe', self.exe)

    def step_maps(self):
        self.run('maps.py', self.data, self.out, '--exe', self.exe)

    def step_music(self):
        # the BGM (the Audio Enhancement Pack's when there); voice/ is left out: the cutscene voices
        # are taken from voice.afs by the demos step and the effects in it by the sound step
        self.run('ee.py', 'copytree', self.data, 'sound/adx', os.path.join(self.out, 'sound', 'adx'), 'voice')

    def step_movies(self):
        self.run('movies.py', '--data', self.data, '--out', self.out)

    def step_halfmovies(self):
        # a half-size set in movie/half/ (about 75 MB more) that the Old 3DS profile plays (port/OLD3DS.md)
        self.run('movies.py', '--data', self.data, '--out', self.out, '--half')

    def step_sound(self):
        self.run('sound.py', '--data', self.data, '--exe', self.exe, '--out', os.path.join(self.out, 'sound'))

    def step_demos(self):
        # then the motions the stage code sets by id outside any scene (Maria lying in the day room in S3)
        self.runs((20, 'demos.py', '--data', self.data, '--exe', self.exe, '--out', self.out),
                  (1, 'motion.py', '--data', self.data, '--exe', self.exe, '--out', self.out))

    def step_stagemodels(self):
        self.run('stagemodels.py', '--data', self.data, '--exe', self.exe, '--out', self.out,
                 '--events', os.path.join(self.out, 'events', 'events.json'))

    def step_shadows(self):
        self.run('shadows.py', self.data, self.out)
        self.run('areaindex.py', self.out)


def trace(path, out):
    """SH2PACK_TRACE=<file>: logs every file of the pack each step opens (step, r or w, path), which
    shows what later steps read back (the browser packer keeps those in memory)."""
    out = os.path.abspath(out) + os.sep
    log = open(path, 'w', buffering=1)
    state = {'step': ''}

    def hook(event, args):
        if event == 'open' and isinstance(args[0], str):
            p = os.path.abspath(args[0])
            if p.startswith(out):
                mode = str(args[1] or '') if args[1] is not None else ('w' if args[2] & 3 else 'r')
                log.write('%s\t%s\t%s\n' % (state['step'], 'w' if set(mode) & set('wax+') else 'r', p[len(out):]))
        elif event in ('os.rename', 'os.replace') and isinstance(args[1], str):
            p = os.path.abspath(args[1])
            if p.startswith(out):
                log.write('%s\tw\t%s\n' % (state['step'], p[len(out):]))
    sys.addaudithook(hook)
    return state


def strip_extras(out):
    for dirpath, dirnames, filenames in os.walk(out, topdown=True):
        if 'png' in dirnames:
            shutil.rmtree(os.path.join(dirpath, 'png'))
            dirnames.remove('png')
        for fn in filenames:
            if fn.endswith('.json'):
                os.remove(os.path.join(dirpath, fn))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        sys.exit(__doc__)
    game = args[0]
    out = args[1] if len(args) > 1 else os.path.join(HERE, '..', '..', 'pack')
    p = Packer(game, out)
    try:
        p.check()
    except RuntimeError as e:
        sys.exit(str(e))
    p.begin()
    # Each step leaves a stamp in <out>/.steps when it finishes, so running the same command again
    # after a failure picks up at the step that stopped. FRESH=1 starts over.
    stamps = os.path.join(out, '.steps')
    if os.environ.get('FRESH', '0') == '1':
        shutil.rmtree(stamps, ignore_errors=True)
    os.makedirs(stamps, exist_ok=True)
    traced = trace(os.environ['SH2PACK_TRACE'], out) if os.environ.get('SH2PACK_TRACE') else {}
    for sid, title, fn in p.steps():
        if os.path.exists(os.path.join(stamps, sid)):
            print('== %s (already done, skipped)' % title, flush=True)
            continue
        print('== %s' % title, flush=True)
        traced['step'] = sid
        t0 = time.time()
        fn()
        open(os.path.join(stamps, sid), 'w').close()
        print('   %.0f s' % (time.time() - t0), flush=True)
    shutil.rmtree(stamps, ignore_errors=True)
    p.finish()
    if '--strip-extras' in sys.argv:
        strip_extras(out)
    print('done: %s' % out)


if __name__ == '__main__':
    main()
