#!/usr/bin/env python3
"""Convert the models the stages place in their rooms (the stage data's mdl_list) for the 3DS.

  stagemodels.py [--data <game data dir>] [--exe sh2pc.exe] [--out pack] [--max-tex 256]
                 [--events pack/events/events.json]

How the PC places them
  Each stage's Model_List (40 bytes per entry, events.py decodes it into events.json "models")
  gives a kind, an id, two flags (off: not loaded while set; on: shown only while set), a place and
  a turn. The character loader (0x493430, Event/chara_data_load.c) finds the kind in the table at
  0x88d4f8 (16 bytes: s32 kind, then file records for the .mdl, an .anm and a .kg1) and loads the
  ones whose place is in the room James is in; the 0x3xx kinds (the town's cars and trailers) go
  to 0x53b510 instead in James' game, which places them the same way.

What this writes
  <out>/chr/item/<model>.s3c, chr/item2/... (data/chr2, Born From a Wish) with their textures,
  converted like the cutscenes' models (demos.py convert_model_files), and
  <out>/events/stagemdl.txt, one line per placed model:
      stage kind folder model flag_off flag_on x y z rx ry rz
  port/source/stagemdl.c reads it.

  The item screen's models too: the PC's item menu shows item i as the model of kind
  ITEM_KINDS[i] (u16 at 0x892640, 0x7xx kinds of the same table, or of the drinks', kits' and
  rounds' table at 0x88d2b8 {s32 kind, file record}), written to
  <out>/events/itemmdl.txt as "item kind folder model" for port/source/itemview.c.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import demos  # noqa: E402
from sound import Exe  # noqa: E402

MODEL_TABLE = 0x88d4f8
ITEM_KINDS, N_ITEMS = 0x892640, 86
PICKUP_TABLE, N_PICKUPS = 0x88d2b8, 7     # {s32 kind, file record}: the drinks, kits and rounds


def model_table(exe):
    out, i = {}, 0
    while True:
        kind = exe.u32(MODEL_TABLE + 16 * i)
        if kind == 0:
            return out
        rec = exe.u32(MODEL_TABLE + 16 * i + 4)
        out[kind & 0xffff] = exe.cstr(exe.u32(rec)) if rec else None
        i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=demos.DEFAULT_DATA)
    ap.add_argument('--exe', default=demos.DEFAULT_EXE)
    ap.add_argument('--out', default=demos.DEFAULT_OUT)
    ap.add_argument('--events', default=None)
    ap.add_argument('--max-tex', type=int, default=256)
    a = ap.parse_args()
    exe = Exe(a.exe)
    table = model_table(exe)
    events = json.load(open(a.events or os.path.join(a.out, 'events', 'events.json')))
    lines, done, missing = [], {}, set()
    for st in events['stages']:
        for m in st.get('models') or []:
            kind = int(m['kind'], 16)
            path = table.get(kind)
            if not path or not path.startswith(('data/chr/', 'data/chr2/')):
                missing.add(kind)
                continue
            dc = path.startswith('data/chr2/')
            sub = path[len('data/chr2/' if dc else 'data/chr/'):]
            folder, name = os.path.split(sub)
            folder += '2' if dc else ''
            model = os.path.splitext(name)[0]
            if (folder, model) not in done:
                src = os.path.join(a.data, 'chr2' if dc else 'chr', sub)
                if not os.path.exists(src):
                    missing.add(kind)
                    continue
                demos.convert_model_files(a.out, folder, model, src, a.max_tex)
                done[(folder, model)] = True
            p, r = m['pos'], m['rot']
            lines.append('%d %#x %s %s %d %d %g %g %g %g %g %g' % (
                st['id'], kind, folder, model, m['flag_off'], m['flag_on'], p[0], p[1], p[2], r[0], r[1], r[2]))
    for i in range(N_PICKUPS):
        kind, rec = struct.unpack('<iI', exe.read(PICKUP_TABLE + 8 * i, 8))
        table.setdefault(kind & 0xffff, exe.cstr(exe.u32(rec)) if rec else None)
    items = []
    for item in range(1, N_ITEMS):
        kind = struct.unpack('<H', exe.read(ITEM_KINDS + 2 * item, 2))[0]
        path = table.get(kind)
        if not kind or not path or not path.startswith(('data/chr/', 'data/chr2/')):
            continue
        dc = path.startswith('data/chr2/')
        sub = path[len('data/chr2/' if dc else 'data/chr/'):]
        folder, name = os.path.split(sub)
        folder += '2' if dc else ''
        model = os.path.splitext(name)[0]
        src = os.path.join(a.data, 'chr2' if dc else 'chr', sub)
        if not os.path.exists(src):
            continue
        if (folder, model) not in done:
            demos.convert_model_files(a.out, folder, model, src, a.max_tex)
            done[(folder, model)] = True
        items.append('%d %#x %s %s' % (item, kind, folder, model))
    with open(os.path.join(a.out, 'events', 'itemmdl.txt'), 'w') as f:
        f.write('\n'.join(items) + '\n')
    print('wrote events/itemmdl.txt: %d items' % len(items))
    with open(os.path.join(a.out, 'events', 'stagemdl.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote events/stagemdl.txt: %d placed models, %d models' % (len(lines), len(done)))
    if missing:
        print('no model file for kinds', ' '.join('%#x' % k for k in sorted(missing)))


if __name__ == '__main__':
    main()
