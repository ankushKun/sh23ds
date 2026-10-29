#!/usr/bin/env python3
"""A picture of every model the stages place (pack/events/stagemdl.txt), the item audit's companion.

  SLOT=2 tests/playtest/model_shots.py [stage ...]

Each model's room comes from the converter's room lookup (tools/sh2conv/events.py Rooms.tag, the
game's room ids) over the game data next to the exe. Per stage: a new game with the flashlight and
the behind camera, then for each place a warp into its room, its flags set so it shows (flag_on set,
flag_off clear), and "pt view". Pictures go to .lr3ds/playtest/audit/models/<stage>_<line>.jpg, the
line being the model's line in stagemdl.txt; models.tsv lists them with their room.
"""
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sh2conv'))
OUT = os.path.join(ROOT, '.lr3ds', 'playtest')
WISH = set(range(53, 59))


def main():
    from PIL import Image
    from events import Rooms
    slot = os.environ.get('SLOT', '')
    sd = os.path.join(ROOT, '.lr3ds', 'save' + slot, 'Azahar', 'sdmc', '3ds', 'sh23ds')
    ev = json.load(open(os.path.join(ROOT, 'pack', 'events', 'events.json')))
    glb = {s['id']: s.get('glb_crd') for s in ev['stages']}
    rooms = Rooms(os.path.join(ROOT, 'Silent Hill 2', 'data'))
    want = {int(a) for a in sys.argv[1:]}
    lines = [l.split() for l in open(os.path.join(ROOT, 'pack', 'events', 'stagemdl.txt'))]
    pics = os.path.join(OUT, 'audit', 'models')
    os.makedirs(pics, exist_ok=True)
    tsv = open(os.path.join(OUT, 'audit', 'models.tsv'), 'a')
    opt = os.path.join(sd, 'options.txt')
    keep = open(opt).read() if os.path.exists(opt) else None
    base = [l for l in (keep or '').splitlines() if not l.startswith(('camera ', 'setup '))]
    open(opt, 'w').write('\n'.join(base + ['camera 1', 'setup 1']) + '\n')
    try:
        for stage in sorted({int(p[0]) for p in lines if len(p) >= 12}):
            if want and stage not in want:
                continue
            for f in glob.glob(os.path.join(sd, 'frame_*')):
                os.remove(f)
            sc = ['supersample 0', 'pt fast 1', 'pt god 1', 'pt noenemies 1', 'pt autoskip 1', 'pt prog clear 0 0 0',
                  'pt wish' if stage in WISH else 'pt newgame', 'pt idle 120', 'pt item 18 1']
            order = []
            for n, p in enumerate(lines):
                if len(p) < 12 or int(p[0]) != stage:
                    continue
                x, y, z = (float(v) for v in p[6:9])
                t = rooms.tag(glb.get(stage), x, y, z, any_height=True)
                if not t.get('map'):
                    tsv.write('%d\t%d\t%s\t%s\t\tno room\n' % (stage, n, p[1], p[3]))
                    continue
                off, on = int(p[4]), int(p[5])
                if on:
                    sc.append('pt flag %d 1' % on)
                if off:
                    sc.append('pt flag %d 0' % off)
                sc.append('pt goto %s %.0f %.0f %.0f 0 %d' % (t['map'], x, t.get('floor_y', y), z, stage))
                sc.append('pt view %.0f %.0f %.0f' % (x, y, z))
                order.append((n, p, t['map']))
            name = 'models_%02d' % stage
            open(os.path.join(OUT, 'scripts', name + '.pt'), 'w').write('\n'.join(sc) + '\n')
            subprocess.run([os.path.join(ROOT, 'tests', 'playtest', 'pt.sh'), name, '90000'], capture_output=True)
            log = open(os.path.join(OUT, 'logs', name + '.log'), errors='replace').read()
            results = re.findall(r'view: (shot|no place)', log)
            frames = sorted(glob.glob(os.path.join(sd, 'frame_*.bmp')), key=lambda q: int(re.search(r'frame_(\d+)', q).group(1)))
            fi = 0
            for (n, p, m), r in zip(order, results):
                if r == 'shot' and fi < len(frames):
                    Image.open(frames[fi]).rotate(90, expand=True).transpose(Image.FLIP_LEFT_RIGHT).convert('RGB').save(
                        os.path.join(pics, '%02d_%03d.jpg' % (stage, n)), quality=72)
                    fi += 1
                tsv.write('%d\t%d\t%s\t%s\t%s\t%s\n' % (stage, n, p[1], p[3], m, r))
            tsv.flush()
            print('stage %d: %d of %d pictured' % (stage, results.count('shot'), len(order)), flush=True)
    finally:
        if keep is None:
            os.remove(opt)
        else:
            open(opt, 'w').write(keep)


if __name__ == '__main__':
    main()
