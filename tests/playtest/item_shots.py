#!/usr/bin/env python3
"""A picture of every pickup from where James takes it (the item audit's companion).

  SLOT=2 tests/playtest/item_shots.py [stage ...]

Per stage: a new game with the flashlight and the behind camera (the virtual SD's options.txt is
set for the run and put back), then for each pickup a warp into its room and "pt pickup <i> shot".
The frame dumps go to .lr3ds/playtest/audit/shots/<stage>_<index>.jpg.
"""
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, '.lr3ds', 'playtest')
WISH = set(range(53, 59))


def main():
    from PIL import Image
    slot = os.environ.get('SLOT', '')
    sd = os.path.join(ROOT, '.lr3ds', 'save' + slot, 'Azahar', 'sdmc', '3ds', 'sh23ds')
    ev = json.load(open(os.path.join(ROOT, 'pack', 'events', 'events.json')))
    want = {int(a) for a in sys.argv[1:]}
    shots = os.path.join(OUT, 'audit', 'shots')
    os.makedirs(shots, exist_ok=True)
    os.makedirs(os.path.join(OUT, 'scripts'), exist_ok=True)
    opt = os.path.join(sd, 'options.txt')
    keep = open(opt).read() if os.path.exists(opt) else None
    lines = [l for l in (keep or '').splitlines() if not l.startswith(('camera ', 'setup '))]
    open(opt, 'w').write('\n'.join(lines + ['camera 1', 'setup 1']) + '\n')
    try:
        for st in ev['stages']:
            items = st.get('items') or []
            if not items or (want and st['id'] not in want):
                continue
            for f in glob.glob(os.path.join(sd, 'frame_*')):
                os.remove(f)
            sc = ['supersample 0', 'pt fast 1', 'pt god 1', 'pt noenemies 1', 'pt autoskip 1', 'pt prog clear 0 0 0',
                  'pt wish' if st['id'] in WISH else 'pt newgame', 'pt idle 120', 'pt item 18 1']
            for it in items:
                room = it.get('room') or {}
                if room.get('map'):
                    sc.append('pt goto %s %.0f %.0f %.0f 0 %d' % (room['map'], it['pos'][0], room.get('floor_y', 0),
                                                                  it['pos'][2], st['id']))
                    sc.append('pt pickup %d shot' % it['index'])
            name = 'shots_%02d' % st['id']
            open(os.path.join(OUT, 'scripts', name + '.pt'), 'w').write('\n'.join(sc) + '\n')
            subprocess.run([os.path.join(ROOT, 'tests', 'playtest', 'pt.sh'), name, '60000'], capture_output=True)
            order = [int(m.group(1)) for m in re.finditer(r'pickup (\d+): shot', open(os.path.join(OUT, 'logs', name + '.log')).read())]
            frames = sorted(glob.glob(os.path.join(sd, 'frame_*.bmp')), key=lambda p: int(re.search(r'frame_(\d+)', p).group(1)))
            for idx, fr in zip(order, frames):
                Image.open(fr).rotate(90, expand=True).transpose(Image.FLIP_LEFT_RIGHT).convert('RGB').save(
                    os.path.join(shots, '%02d_%02d.jpg' % (st['id'], idx)), quality=72)
            print('stage %d: %d shots of %d pickups' % (st['id'], min(len(order), len(frames)), len(items)), flush=True)
    finally:
        if keep is None:
            os.remove(opt)
        else:
            open(opt, 'w').write(keep)


if __name__ == '__main__':
    main()
