#!/usr/bin/env python3
"""Pack the decoded stage events (pack/events/stage_NN.json from events.py) into the binary
files the 3DS app reads: pack/events/sNN.s3e, plus pack/events/rooms.txt (map -> stage).

  events_pack.py <game data dir> <pack dir>

Also writes the English message text the events use: pack/events/sNN.msg (stage messages) and
pack/events/common.msg (system messages: locked doors, pickups, ...) and pack/events/item.msg (item n is
named by message 18 + 2n and described by 19 + 2n), from data/etc/message.

S3E v2 (little-endian)
  char[4] "S3E1", u32 version (2), u32 stage, u32 events, u32 items, u32 glb_crd, char name[24]  (48 bytes)
  event (128 bytes)
    u32 raw[4]           flag, cond, rslt0, rslt1 words exactly as in the exe (the app decodes them)
    u8  shape            0 none, 1 point, 2 segments (line / polyline), 3 rect (min, max), 4 polygon
    u8  points, u8 any_height, u8 dest_keeps_facing
    f32 y                trigger record height
    f32 pts[8][2]        x, z
    f32 dest_pos[3], f32 dest_facing
    u16 dest_stage       resolved (never 0), 0 when the event has no destination
    u16 pad
    char dest_map[8], char trigger_map[8]
    u32 pad
  item (32 bytes)
    f32 pos[3], f32 rot_y, u16 flag, u16 gives_item, u16 system_message, u16 pad, char map[8]
rooms.txt: one line per bg map, "<map> <stage>", the stage whose events use that map the most.
MSG v1: char[4] "MSG1", u32 count, u32 offset[count] (from the file start), then NUL-terminated
  ASCII strings; '\n' is a line break and '\f' a page break.
"""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sh2mes  # noqa: E402

# Message file per stage: the stage name without "Stg_"; the DC-only stages 53..58 have no name in
# the stage table and were matched by building and by the message numbers their events use.
DC_STAGE_MSG = {53: 'x_heaven', 54: 'x_town', 55: 'x_mansion_1f', 56: 'x_mansion_2f', 57: 'x_mansion_bf',
                58: 'x_apart'}

SHAPES = {'point': 1, 'line': 2, 'polyline': 2, 'rect': 3, 'triangle': 4, 'quad': 4}


def name8(s):
    return (s or '').encode()[:7].ljust(8, b'\0')


def pack_event(e, stage):
    raw = [int(x, 16) for x in e['raw']]
    sh = e['trigger'].get('shape') or {}
    kind = SHAPES.get(sh.get('shape'), 0)
    if sh.get('shape') == 'point':
        pts = [sh['point']]
    elif sh.get('shape') == 'rect':
        pts = [sh['min'], sh['max']]
    else:
        pts = sh.get('points') or []
    if len(pts) > 8:
        raise ValueError(f'stage {stage} event {e["index"]}: {len(pts)} points')
    flat = [c for p in pts for c in p] + [0.0] * (16 - 2 * len(pts))
    d = e['result'].get('dest') or {}
    dest_stage = d.get('stage') or 0
    if d and d.get('same_stage'):
        dest_stage = stage
    pos = d.get('pos') or [0.0, 0.0, 0.0]
    facing = d.get('facing')
    trig_map = (sh.get('room') or {}).get('map')
    dest_map = (d.get('room') or {}).get('map')
    rec = struct.pack('<4I4Bf16f4fHH8s8sI', *raw, kind, len(pts), 1 if sh.get('any_height') else 0,
                      1 if d and facing is None else 0, sh.get('y') or 0.0, *flat, *pos,
                      facing if facing is not None else 0.0, dest_stage, 0, name8(dest_map), name8(trig_map), 0)
    assert len(rec) == 128
    return rec, trig_map, dest_map


def pack_item(it):
    rec = struct.pack('<4f4H8s', *it['pos'], it.get('rot_y') or 0.0, it['flag'], it['gives_item'],
                      it.get('system_message') or 0, 0, name8((it.get('room') or {}).get('map')))
    assert len(rec) == 32
    return rec


def write_msg(path, messages):
    blobs = [m.encode('ascii', 'replace') + b'\0' for m in messages]
    o = 8 + 4 * len(blobs)
    offs = []
    for b in blobs:
        offs.append(o)
        o += len(b)
    with open(path, 'wb') as f:
        f.write(struct.pack(f'<4sI{len(offs)}I', b'MSG1', len(offs), *offs) + b''.join(blobs))


def main():
    data, pack = sys.argv[1], sys.argv[2]
    mdir = os.path.join(data, 'etc', 'message')
    write_msg(os.path.join(pack, 'events', 'common.msg'), sh2mes.load(os.path.join(mdir, 'common_msg_e.mes')))
    # item_x is the Director's Cut table (revolver, cleaver and the DC items); with it item n is
    # message 18 + 2n for every item id, item_msg_e.mes drifts from id 10 on
    write_msg(os.path.join(pack, 'events', 'item.msg'), sh2mes.load(os.path.join(mdir, 'item_x_msg_e.mes')))
    # the results screen after an ending (0x447650)
    write_msg(os.path.join(pack, 'events', 'result.msg'), sh2mes.load(os.path.join(mdir, 'result_msg_e.mes')))
    src = os.path.join(pack, 'events')
    uses = collections.defaultdict(collections.Counter)
    n_files = 0
    for stage in range(1, 64):
        p = os.path.join(src, f'stage_{stage:02d}.json')
        if not os.path.exists(p):
            continue
        st = json.load(open(p))
        recs = []
        for e in st['events']:
            rec, tm, dm = pack_event(e, stage)
            recs.append(rec)
            if tm:
                uses[tm][stage] += 2
            if dm:
                uses[dm][(e['result']['dest'].get('stage') or stage) if not e['result']['dest'].get('same_stage') else stage] += 1
        items = [pack_item(it) for it in st['items']]
        head = struct.pack('<4s5I24s', b'S3E1', 2, stage, len(recs), len(items), st['glb_crd'],
                           st['name'].encode()[:23])
        with open(os.path.join(src, f's{stage:02d}.s3e'), 'wb') as f:
            f.write(head + b''.join(recs) + b''.join(items))
        n_files += 1
        msg_name = DC_STAGE_MSG.get(stage) or st['name'][4:].lower()
        mp = os.path.join(mdir, f'stage_{msg_name}_msg_e.mes')
        if os.path.exists(mp):
            write_msg(os.path.join(src, f's{stage:02d}.msg'), sh2mes.load(mp))
        else:
            print(f'  stage {stage}: no message file {mp}')
    with open(os.path.join(src, 'rooms.txt'), 'w') as f:
        for m in sorted(uses):
            f.write(f'{m} {uses[m].most_common(1)[0][0]}\n')
    print(f'{n_files} stages, {len(uses)} maps indexed')


if __name__ == '__main__':
    main()
