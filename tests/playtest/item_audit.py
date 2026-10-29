#!/usr/bin/env python3
"""Item audit: every pickup the stages lay down (the PC's gi_list, pack/events/events.json "items"),
taken in Azahar the way a player takes it (playtest.c "pt pickup").

  tests/playtest/item_audit.py [stage ...]      all stages with pickups, or these
  SLOT=2 tests/playtest/item_audit.py           on the second virtual SD (pt.sh)

For each stage: a new game, then for each pickup a warp into its room and "pt pickup": James stands
where its height window allows, faces it and presses A, first without the flashlight (the dark-room
rule, 0x48e520 / 0x507e00), then the ones left again with the flashlight on. Born From a Wish stages
start as Born From a Wish. FLAGS="251" sets story flags first (the pickups that lie only later).
Writes .lr3ds/playtest/audit/items.tsv (AUDIT_OUT another file):

  stage  index  item  name  x y z  room  dark_result  light_result  lit_by_room_light
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, '.lr3ds', 'playtest')
WISH = set(range(53, 59))          # Born From a Wish stages (prog_chapter)


def script(stage, items):
    lines = ['# item audit, stage %d' % stage, 'pt fast 1', 'pt god 1', 'pt noenemies 1', 'pt autoskip 1',
             'pt prog clear 0 0 0', 'pt wish' if stage in WISH else 'pt newgame', 'pt idle 120']
    # FLAGS=251 ...: story flags set first, for the pickups that only lie later (cond type 1: flag 251)
    lines += ['pt flag %s 1' % f for f in os.environ.get('FLAGS', '').split()]
    for flashlight in (False, True):
        if flashlight:
            lines += ['pt item 18 1']          # the light is switched on already (main.c s_flash_on)
        for it in items:
            room = it.get('room') or {}
            m = room.get('map')
            if not m:
                continue
            y = room.get('floor_y', it['pos'][1])
            lines.append('pt goto %s %.0f %.0f %.0f 0 %d' % (m, it['pos'][0], y, it['pos'][2], stage))
            lines.append('pt pickup %d' % it['index'])
    return '\n'.join(lines) + '\n'


def main():
    ev = json.load(open(os.path.join(ROOT, 'pack', 'events', 'events.json')))
    want = {int(a) for a in sys.argv[1:]}
    rows = []
    os.makedirs(os.path.join(OUT, 'scripts'), exist_ok=True)
    os.makedirs(os.path.join(OUT, 'audit'), exist_ok=True)
    for st in ev['stages']:
        items = st.get('items') or []
        if not items or (want and st['id'] not in want):
            continue
        name = 'audit_%02d' % st['id']
        open(os.path.join(OUT, 'scripts', name + '.pt'), 'w').write(script(st['id'], items))
        r = subprocess.run([os.path.join(ROOT, 'tests', 'playtest', 'pt.sh'), name, '60000'], capture_output=True,
                           text=True)
        print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else name + ': no output', flush=True)
        log = os.path.join(OUT, 'logs', name + '.log')
        res = {}
        for line in open(log, errors='replace') if os.path.exists(log) else []:
            m = re.search(r'pickup (\d+): (.*)$', line)
            if m and not m.group(2).startswith(('James at', 'refused')):
                res.setdefault(int(m.group(1)), []).append(m.group(2))
        for it in items:
            got = res.get(it['index'], [])
            dark = got[0] if got else 'no result'
            light = got[1] if len(got) > 1 else ''
            lit = re.search(r'lit by a room light (\d)', dark)
            rows.append([st['id'], it['index'], it['gives_item'], it.get('pickup_model', ''),
                         '%.0f %.0f %.0f' % tuple(it['pos']), (it.get('room') or {}).get('map', ''),
                         dark, light, lit.group(1) if lit else ''])
    tsv = os.environ.get('AUDIT_OUT', os.path.join(OUT, 'audit', 'items.tsv'))
    with open(tsv, 'w') as f:
        f.write('stage\tindex\titem\tmodel\tpos\troom\twithout_flashlight\twith_flashlight\tlit\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    taken = sum('TAKEN' in r[6] and 'NOT TAKEN' not in r[6] for r in rows)
    print('%d pickups, %d taken without the flashlight' % (len(rows), taken))


if __name__ == '__main__':
    main()
