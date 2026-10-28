#!/usr/bin/env python3
"""Decode the Silent Hill 2 (PC Director's Cut) stage event tables from sh2pc.exe.

  events.py [--exe sh2pc.exe] [--stages stage_table.json] [--data <game data dir>]
            [--out pack/events] [--no-rooms]

Reads every Stage_Data record listed in stage_table.json, decodes its Event_List (ev_list),
the trigger / destination records in ev_pos, the item pickups (gi_list) and the model and
enemy placement lists, then writes <out>/events.json (all stages) and <out>/stage_NN.json
(one file per stage). Door / move events get a decoded destination (stage, position, facing)
and, when the game's collision files are available, the room (.cld / .map name) that the
trigger and the destination fall in.

Format notes are in EVENTS.md next to this file. Everything here comes from static analysis
of the PC event.c object (0x48aea0..0x48f4b0) and the PS2 beta debug info.
"""
import argparse
import json
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcexe import Exe  # noqa: E402,F401  (maps.py and pics.py take it from here)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_EXE = os.path.join(ROOT, 'Silent Hill 2', 'sh2pc.exe')
DEFAULT_STAGES = os.path.join(ROOT, 'sh2-3ds-port-kit', 'data', 'dc', 'stage_table.json')
DEFAULT_OUT = os.path.join(ROOT, 'pack', 'events')


# ------------------------------------------------------------------ constants from the exe
DOOR_OFFSET = 250.0          # _DAT_006a91d4: player placed this far in front of a door line
LOOK_RADIUS = 400.0          # _DAT_006a91c8 / sqrt(_DAT_006a91c4): point / line look distance
LOOK_CONE = math.radians(30)  # _DAT_006a91c0 (0.5236): max angle between facing and a look point
LOOK_AHEAD = 50.0            # _DAT_006a6dac: look origin is 50 units ahead of the player
Y_ANY = 45000.0              # _DAT_006a91d0: record y >= this means "any height"
Y_ABOVE = 500.0              # _DAT_0062d230: record y may be up to 500 above the feet (y down)
Y_BELOW = 100.0              # _DAT_006a91cc: ... and up to 100 below
ITEM_Y_ABOVE = 750.0         # _DAT_006a8458: item pickups use a 750 window above

# Event_List.cond >> 28 (trigger kind), see EventCheck @ 0x48e520
TRIGGER_KINDS = {
    0: ('auto', 'no position test; fires whenever the flag conditions hold'),
    1: ('auto', 'no position test; fires whenever the flag conditions hold'),
    2: ('action', 'action button; look at shape (types 0-10) or stand inside area (11-13)'),
    3: ('look', 'automatic when the player looks at the shape (no button)'),
    4: ('enter', 'automatic when the player stands inside the area'),
    5: ('use_item_look', 'item <item> used from the inventory while looking at the shape'),
    6: ('use_item_enter', 'item <item> used from the inventory while inside the area'),
    7: ('action_have_item', 'action button while holding item <item>; look or area'),
    8: ('have_item_look', 'automatic while holding item <item> and looking at the shape'),
    9: ('have_item_enter', 'automatic while holding item <item> and inside the area'),
    10: ('action_or_use_item', 'action button while holding item <item>, or using it; look or area'),
}
LOOK_KINDS = {3, 5, 8}       # must be a look shape (asserts otherwise)
AREA_KINDS = {4, 6, 9}       # must be an area shape (types 11-13)
EITHER_KINDS = {2, 7, 10}    # area if type 11-13, look otherwise
ITEM_KINDS = {5, 6, 7, 8, 9, 10}

# Event_List.rslt0 >> 28 (result type), see dispatcher @ 0x48eac0
RESULT_TYPES = {
    0: 'none',
    1: 'flag_set',        # 0x48bfc0 EventExecFlag: set flag rslt1&0x3fff
    2: 'flag_clear',      # 0x48bfc0 EventExecFlag: clear flag rslt1&0x3fff
    3: 'program',         # 0x48c110 EventExecProgram: ev_prog[(rslt1>>14)&0x7f]()
    4: 'door',            # 0x48e220 EventExecDoor/Move: teleport via 0x48bc30
    5: 'door_program',    # 0x48e220 then 0x48c110: teleport, then run ev_prog[...]
    6: 'message',         # 0x48c040 EventExecMessage: message (rslt1>>14)&0xff
    7: 'door_unlock',     # 0x48c1c0: message + door_se.unlock, sets flag (usually the door's)
    8: 'door_locked',     # 0x48c1c0: system message 4 ("locked") + door_se.lock
    9: 'door_jammed',     # 0x48c1c0: message or system message 6/7 + door_se.jam
    10: 'save_point',     # 0x48c680 EventExecSave
}
DOOR_RESULTS = {4, 5}

DOOR_SE_ADDR = 0x88cb70      # Event_DoorSound door_se[22] {open, close, unlock, jam, lock, pad}
ITEM_KIND_ADDR = 0x88cca0    # Item_List.st>>29 -> {s16 item given, s16 system message, s16 model, pad}


def half(h):
    """CharToFloat2 (0x48b8b0): the game's 16-bit float. Like IEEE half but exponent 0 and 31
    are ordinary exponents (no zero / subnormal / inf special cases)."""
    bits = ((h & 0x8000) << 16) | (((h >> 10) & 0x1f) + 112) << 23 | (h & 0x3ff) << 13
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def r6(v):
    return round(float(v), 3)


def wrap_angle(a):
    """Same normalisation as 0x48bc30: result in (-pi, pi]."""
    t = a / (2 * math.pi)
    t -= math.floor(t)
    if t > 0.5:
        t -= 1.0
    return t * 2 * math.pi


# ------------------------------------------------------------------ ev_pos records
def record_size(pt):
    if pt == 0:
        return 10
    if 1 <= pt <= 4:
        return 12
    if 5 <= pt <= 10:
        return 10 + 4 * (pt - 4)        # (pt-3) points: base + (pt-4) relative pairs
    if pt == 11:
        return 14
    if pt in (12, 13):
        return 10 + 4 * (pt - 10)       # triangle 2 extra points, quad 3
    return 10


def read_record(blob, off, pt):
    """Raw ev_pos record: f32 x @0, u16 half y @4, f32 z @6, u16 half extras from @10."""
    x, = struct.unpack_from('<f', blob, off)
    hy, = struct.unpack_from('<H', blob, off + 4)
    z, = struct.unpack_from('<f', blob, off + 6)
    n_extra = (record_size(pt) - 10) // 2
    extras = [half(v) for v in struct.unpack_from('<%dH' % n_extra, blob, off + 10)] if n_extra else []
    return x, half(hy), hy, z, extras


def axis_line(x, z, w, pt):
    """Types 1-4: segment from (x,z) along one axis. Returns the two end points."""
    return {1: [(x, z), (x + w, z)], 2: [(x, z), (x - w, z)],
            3: [(x, z), (x, z - w)], 4: [(x, z), (x, z + w)]}[pt]


def front_normal(a, b):
    """The side a line can be triggered from (EventCheckLookLine 0x48b570 requires
    cross(b-a, player-a) >= 0) and the side 0x48bc30 places the player on: n = (-dz, dx)."""
    dx, dz = b[0] - a[0], b[1] - a[1]
    ln = math.hypot(dx, dz) or 1.0
    return (-dz / ln, dx / ln)


def decode_shape(blob, off, pt):
    x, y, hy, z, ex = read_record(blob, off, pt)
    s = {'pos_type': pt, 'ev_pos_offset': off, 'y': r6(y), 'any_height': y >= Y_ANY}
    if y < Y_ANY:
        # EventCheckLook / EventCheckIn: py - 500 <= y <= py + 100 (y points down)
        s['player_y_range'] = [r6(y - Y_BELOW), r6(y + Y_ABOVE)]
    if pt == 0:
        s.update(shape='point', point=[r6(x), r6(z)], radius=LOOK_RADIUS,
                 facing_cone_deg=30.0)
        s['_stand'] = (x, z)
    elif 1 <= pt <= 10:
        if pt <= 4:
            pts = axis_line(x, z, ex[0], pt)
        else:
            pts = [(x, z)] + [(x + ex[i], z + ex[i + 1]) for i in range(0, len(ex), 2)]
        s.update(shape='line' if len(pts) == 2 else 'polyline',
                 points=[[r6(p[0]), r6(p[1])] for p in pts], look_distance=LOOK_RADIUS)
        a, b = pts[0], pts[1]
        n = front_normal(a, b)
        s['front_normal'] = [r6(n[0]), r6(n[1])]
        mx, mz = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
        s['_stand'] = (mx + n[0] * 150, mz + n[1] * 150)
    elif pt == 11:
        w, d = ex[0], ex[1]
        s.update(shape='rect', min=[r6(x), r6(z)], max=[r6(x + w), r6(z + d)])
        s['_stand'] = (x + w / 2, z + d / 2)
    elif pt in (12, 13):
        pts = [(x, z)] + [(x + ex[i], z + ex[i + 1]) for i in range(0, len(ex), 2)]
        s.update(shape='triangle' if pt == 12 else 'quad',
                 points=[[r6(p[0]), r6(p[1])] for p in pts])
        s['_stand'] = (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))
    else:
        s.update(shape='unknown', point=[r6(x), r6(z)])
        s['_stand'] = (x, z)
    s['_y'] = y
    return s


def decode_destination(blob, off, pt):
    """Mirror of 0x48bc30 (writes DAT_01dbc210..21c = x, y, z, rot)."""
    x, y, hy, z, ex = read_record(blob, off, pt)
    keep = False
    if pt == 1:
        px, pz, rot = x + ex[0] * 0.5, z + DOOR_OFFSET, 0.0
    elif pt == 2:
        px, pz, rot = x - ex[0] * 0.5, z - DOOR_OFFSET, math.pi
    elif pt == 3:
        px, pz, rot = x + DOOR_OFFSET, z - ex[0] * 0.5, math.pi / 2
    elif pt == 4:
        px, pz, rot = x - DOOR_OFFSET, z + ex[0] * 0.5, -math.pi / 2
    elif pt == 5:
        dx, dz = ex[0], ex[1]
        rot = math.atan2(dx, dz) - math.pi / 2
        px = x + dx * 0.5 + math.sin(rot) * DOOR_OFFSET
        pz = z + dz * 0.5 + math.cos(rot) * DOOR_OFFSET
    else:                               # 0 and 6..15: exact point, facing unchanged
        px, pz, rot, keep = x, z, None, True
    d = {'pos_type': pt, 'ev_pos_offset': off, 'pos': [r6(px), r6(y), r6(pz)]}
    if keep:
        d['facing'] = None
        d['facing_note'] = 'keeps the player rotation'
    else:
        rot = wrap_angle(rot)
        d['facing'] = r6(rot)
        d['facing_deg'] = round(math.degrees(rot), 2)
    if 1 <= pt <= 5:
        line = axis_line(x, z, ex[0], pt) if pt <= 4 else [(x, z), (x + ex[0], z + ex[1])]
        d['door_line'] = [[r6(p[0]), r6(p[1])] for p in line]
    return d


# ------------------------------------------------------------------ flags
def flag_half(v16):
    return v16 & 0x3fff, (v16 >> 14) & 1, (v16 >> 15) & 1


def decode_flags(w0):
    """Event_List.flag: two halves {u14 flag_no, u1 update, u1 polarity}. The event only
    runs while flag_no == 0 or game_flag[flag_no] == polarity; when it runs and update is
    set, game_flag[flag_no] becomes !polarity (0x48bee0, EventExecSubFlagSet)."""
    out = {'require': [], 'on_exec': []}
    high_no = flag_half(w0 >> 16)[0]
    for v in (w0 >> 16, w0 & 0xffff):
        n, upd, pol = flag_half(v)
        if n == 0:
            continue
        out['require'].append({'flag': n, 'state': bool(pol)})
        # 0x48bee0 returns early when the high half's flag number is 0, so a low-half
        # update only happens when the high half names a flag too
        if upd and high_no:
            out['on_exec'].append({'flag': n, 'value': not bool(pol)})
    return out


# ------------------------------------------------------------------ rooms
class Rooms:
    """Floor triangles and wall segments of the rooms in data/bg/<area>/<room>.cld, plus the
    game's own room id lookup (sh2rooms.room_id, FUN_00494db0)."""

    def __init__(self, data_dir):
        sys.path.insert(0, HERE)
        import sh2cld
        import sh2rooms
        self.sh2cld, self.sr = sh2cld, sh2rooms
        self.data_dir = data_dir
        self.files = {}                 # area -> [(room, tris (N,3,3), bbox, walls (M,4), cells)]
        self.by_glb = {}                # glb -> {room_id: [(area, room)]}

    def area(self, area):
        if area in self.files:
            return self.files[area]
        out = []
        d = os.path.join(self.data_dir, self.sr.AREA_DIRS.get(area, 'bg'), area)
        names = sorted(f for f in os.listdir(d) if f.endswith('.cld')) if os.path.isdir(d) else []
        for fn in names:
            _, polys, _ = self.sh2cld.load(os.path.join(d, fn))
            tris, walls = [], []
            for p in polys:
                if p.kind == 0:
                    continue
                pts = p.p[:3] if p.shape == 0 else p.p[:4]
                if p.group == 0:
                    tris.append(pts[[0, 1, 2]])
                    if len(pts) == 4:
                        tris.append(pts[[0, 2, 3]])
                else:
                    xz = pts[:, [0, 2]]
                    i, j = max(((i, j) for i in range(len(xz)) for j in range(i + 1, len(xz))),
                               key=lambda ij: np.sum((xz[ij[0]] - xz[ij[1]]) ** 2))
                    walls.append([xz[i][0], xz[i][1], xz[j][0], xz[j][1]])
            if not tris:
                continue
            t = np.array(tris)
            bb = (t[:, :, 0].min(), t[:, :, 2].min(), t[:, :, 0].max(), t[:, :, 2].max())
            cen = t.mean(axis=1)
            out.append((fn[:-4], t, bb, np.array(walls).reshape(-1, 4), cen))
        self.files[area] = out
        return out

    def glb_files(self, glb):
        """{room_id: [(area, room)]}: each .cld assigned to the room id most of its floor
        triangles map to."""
        if glb in self.by_glb:
            return self.by_glb[glb]
        m = {}
        for area in self.sr.GLB_AREAS.get(glb, []):
            for room, t, bb, w, cen in self.area(area):
                votes = {}
                for c in cen:
                    rid = self.sr.room_id(glb, c[0], c[2])
                    votes[rid] = votes.get(rid, 0) + 1
                rid = max(votes, key=votes.get)
                m.setdefault(rid, []).append((area, room))
        self.by_glb[glb] = m
        return m

    def _get(self, area, room):
        for f in self.area(area):
            if f[0] == room:
                return f
        return None

    @staticmethod
    def _floor_heights(t, x, z):
        a, b, c = t[:, 0], t[:, 1], t[:, 2]
        v0x, v0z = c[:, 0] - a[:, 0], c[:, 2] - a[:, 2]
        v1x, v1z = b[:, 0] - a[:, 0], b[:, 2] - a[:, 2]
        v2x, v2z = x - a[:, 0], z - a[:, 2]
        den = v0x * v1z - v1x * v0z
        ok = np.abs(den) > 1e-9
        den = np.where(ok, den, 1.0)
        u = (v2x * v1z - v1x * v2z) / den
        v = (v0x * v2z - v2x * v0z) / den
        inside = ok & (u >= -1e-4) & (v >= -1e-4) & (u + v <= 1 + 1e-4)
        h = a[:, 1] + u * (c[:, 1] - a[:, 1]) + v * (b[:, 1] - a[:, 1])
        return inside, h

    def floor(self, area, room, x, y, z):
        """(floor_y, |dy|) of the floor under (x, z) closest to y, or None."""
        f = self._get(area, room)
        if f is None:
            return None
        _, t, bb, _, _ = f
        if not (bb[0] - 1 <= x <= bb[2] + 1 and bb[1] - 1 <= z <= bb[3] + 1):
            return None
        inside, h = self._floor_heights(t, x, z)
        if not inside.any():
            return None
        hs = h[inside]
        i = int(np.argmin(np.abs(hs - y)))
        return float(hs[i]), float(abs(hs[i] - y))

    def wall_distance(self, area, room, x, z):
        f = self._get(area, room)
        if f is None or not len(f[3]):
            return None
        w = f[3]
        p = np.array([x, z])
        A, B = w[:, :2], w[:, 2:]
        AB = B - A
        L = np.maximum(np.sum(AB * AB, axis=1), 1e-9)
        tt = np.clip(np.sum((p - A) * AB, axis=1) / L, 0, 1)
        q = A + AB * tt[:, None]
        return float(np.min(np.hypot(q[:, 0] - x, q[:, 1] - z)))

    def tag(self, glb, x, y, z, any_height=False):
        rid = self.sr.room_id(glb, x, z)
        out = {'room_id': rid}
        m = self.glb_files(glb)
        cands = m.get(rid, []) if rid else []
        pool = cands or [(a, r) for a in self.sr.GLB_AREAS.get(glb, []) for r, *_ in self.area(a)]
        best = None
        for area, room in pool:
            fl = self.floor(area, room, x, y, z)
            if fl and (best is None or fl[1] < best[2]):
                best = (area, room, fl[1], fl[0])
        if best:
            out.update(area=best[0], map=best[1], floor_y=r6(best[3]), dy=r6(best[2]),
                       on_floor=any_height or best[2] <= 300.0)
        elif len(cands) == 1:
            out.update(area=cands[0][0], map=cands[0][1], on_floor=False)
        else:
            out['on_floor'] = False
        return out


# ------------------------------------------------------------------ stage decoding
def decode_stage(exe, st, names):
    sd = struct.unpack('<17I', exe.read(int(st['addr'], 16), 68))
    ev_list, ev_pos, ev_prog, gi_list, mdl_list, en_list = sd[:6]
    out = {'id': st['id'], 'name': names.get(st['id']), 'stage_data': '%#x' % int(st['addr'], 16),
           'glb_crd': sd[9], 'pc_model': sd[10],
           'ev_list': '%#x' % ev_list, 'ev_pos': '%#x' % ev_pos, 'ev_prog': '%#x' % ev_prog,
           'gi_list': '%#x' % gi_list, 'mdl_list': '%#x' % mdl_list, 'en_list': '%#x' % en_list}
    # ev_prog table length: count code pointers
    n_prog = 0
    if ev_prog:
        while n_prog < 256:
            p = exe.u32(ev_prog + 4 * n_prog)
            if not (0x401000 <= p < 0x629000) and not (p == 0 and n_prog == 0):
                break                   # slot 0 is normally NULL (program 0 = none)
            n_prog += 1
    out['ev_prog_count'] = n_prog       # table length including the NULL slot 0

    raw_events = []
    if ev_list:
        while len(raw_events) < 1024:
            w = struct.unpack('<4I', exe.read(ev_list + 16 * len(raw_events), 16))
            if w[1] == 0 and w[2] == 0:
                break
            raw_events.append(w)
    max_off = 0
    for w in raw_events:
        for word in (w[1], w[2]):
            o, pt = (word >> 16) & 0xfff, (word >> 12) & 0xf
            max_off = max(max_off, o + record_size(pt))
    blob = exe.read(ev_pos, max(max_off, 16)) if ev_pos else b'\0' * 16

    events = []
    for i, w in enumerate(raw_events):
        events.append(decode_event(i, w, blob, n_prog))
    out['events'] = events

    items = []
    if gi_list:
        while len(items) < 512:
            a = gi_list + 16 * len(items)
            px, pz, hy, hr, stw = struct.unpack('<ffHHI', exe.read(a, 16))
            kind = stw >> 29
            if kind == 0:
                break
            gives, smsg, model, _ = struct.unpack('<4h', exe.read(ITEM_KIND_ADDR + 8 * kind, 8))
            items.append({'index': len(items), 'pos': [r6(px), r6(half(hy)), r6(pz)],
                          'rot_y': r6(half(hr)), 'kind': kind, 'gives_item': gives,
                          'system_message': smsg, 'pickup_model': '%#x' % model,
                          'flag': stw & 0x3fff, 'picked_flag': (stw & 0x3fff) + 1,
                          'diff_class': (stw >> 26) & 7, 'cond_type': (stw >> 20) & 0x1f,
                          # ItemCheckLookPoint 0x48b980: py - 750 <= y <= py + 100,
                          # within 400 of the player, inside a 30 degree look cone
                          'player_y_range': [r6(half(hy) - Y_BELOW), r6(half(hy) + ITEM_Y_ABOVE)],
                          'raw_st': '%08x' % stw})
    out['items'] = items
    models = []
    for b in read_list(exe, mdl_list, 40):
        kind, mid, f_off, f_on = struct.unpack_from('<4h', b)
        pos = struct.unpack_from('<4f', b, 8)
        rot = struct.unpack_from('<4f', b, 24)
        models.append({'index': len(models), 'kind': '%#x' % (kind & 0xffff), 'id': mid,
                       'flag_off': f_off, 'flag_on': f_on,
                       'pos': [r6(v) for v in pos[:3]], 'rot': [r6(v) for v in rot[:3]]})
    enemies = []
    for b in read_list(exe, en_list, 20):
        kind, eid, px, pz, py, ry, status, cond = struct.unpack('<hhiihhhH', b)
        enemies.append({'index': len(enemies), 'kind': '%#x' % (kind & 0xffff), 'id': eid,
                        'pos': [px, py, pz], 'rot_y_raw': ry, 'status': status,
                        'condition': '%#x' % cond})
    out['models'] = models
    out['enemies'] = enemies
    return out


def read_list(exe, addr, size):
    """Model_List / Enemy_List entries up to the kind == 0 terminator (the loops in the
    chara loader 0x493430 stop there)."""
    out = []
    while addr and len(out) < 512:
        b = exe.read(addr + size * len(out), size)
        if struct.unpack_from('<h', b)[0] == 0:
            break
        out.append(b)
    return out


def decode_event(i, w, blob, n_prog):
    w0, w1, w2, w3 = w
    kind = w1 >> 28
    pt = (w1 >> 12) & 0xf
    toff = (w1 >> 16) & 0xfff
    ev = {'index': i, 'raw': ['%08x' % v for v in w], 'flags': decode_flags(w0)}
    kname, kdesc = TRIGGER_KINDS.get(kind, ('kind%d' % kind, 'unknown trigger kind'))
    trig = {'kind': kind, 'kind_name': kname, 'needs_light': bool(w1 & 4)}
    if kind in ITEM_KINDS:
        trig['item'] = (w1 >> 4) & 0xff
        trig['consume_item'] = bool(w1 & 8)
    unknown = w1 & (0x3 if kind in ITEM_KINDS else 0xffb)
    if unknown:
        trig['unknown_bits'] = '%#x' % unknown
    positional = kind >= 2
    if positional:
        sh = decode_shape(blob, toff, pt)
        if kind in AREA_KINDS or (kind in EITHER_KINDS and 11 <= pt <= 13):
            sh['test'] = 'inside'
        else:
            sh['test'] = 'look'
        if (kind in LOOK_KINDS and 11 <= pt <= 13) or (kind in AREA_KINDS and not 11 <= pt <= 13):
            sh['warning'] = 'shape type does not match trigger kind (game asserts)'
        trig['shape'] = sh
    ev['trigger'] = trig

    rtype = w2 >> 28
    res = {'type': rtype, 'type_name': RESULT_TYPES.get(rtype, 'type%d' % rtype)}
    flag_no = w3 & 0x3fff
    if rtype in (1, 2):
        res['flag'] = flag_no
    elif rtype == 3:
        res['program'] = (w3 >> 14) & 0x7f
        res['sets_flag'] = flag_no or None
    elif rtype == 6:
        res['message'] = (w3 >> 14) & 0xff
        res['sets_flag'] = flag_no or None
    elif rtype in (7, 8, 9):
        m = (w3 >> 14) & 0xff
        sysmsg = {7: {0xff: 5}, 8: {}, 9: {0xff: 6, 0xfe: 7}}[rtype]
        if rtype == 8:
            res['system_message'] = 4
        elif m in sysmsg:
            res['system_message'] = sysmsg[m]
        else:
            res['message'] = m
        res['door_sound'] = (w3 >> 22) & 0x3f
        res['sets_flag'] = flag_no or None
    elif rtype == 10:
        res['save_point'] = (w3 >> 14) & 0xff     # passed to the save menu (0x4551d0)
        res['sets_flag'] = flag_no or None
    elif rtype in DOOR_RESULTS:
        dpt = (w2 >> 12) & 0xf
        doff = (w2 >> 16) & 0xfff
        dest = decode_destination(blob, doff, dpt)
        stg = (w2 >> 5) & 0x7f
        dest['stage'] = stg or None
        dest['same_stage'] = stg == 0
        # rslt0 bit 4 -> DAT_00934d94 bit 2, handed to the camera reset after the warp
        # (0x518ca0 -> 0x51d030 third argument); exact effect not traced
        dest['camera_flag'] = bool(w2 & 0x10)
        res['dest'] = dest
        res['door_sound'] = (w3 >> 22) & 0x3f
        res['sets_flag'] = flag_no or None
        if rtype == 5:
            res['program'] = (w3 >> 14) & 0x7f
        if w2 & 0xf:
            res['unknown_bits'] = '%#x' % (w2 & 0xf)
    if res.get('program') is not None and res['program'] >= n_prog:
        res['warning'] = 'program index outside ev_prog table'
    ev['result'] = res
    return ev


# ------------------------------------------------------------------ room tagging
def tag_rooms(stages, rooms):
    by_id = {s['id']: s for s in stages}
    for s in stages:
        glb = s['glb_crd']
        for ev in s['events']:
            sh = ev['trigger'].get('shape')
            if sh:
                t = rooms.tag(glb, sh['_stand'][0], sh['_y'], sh['_stand'][1], sh['any_height'])
                if t.get('map') and sh['shape'] in ('line', 'polyline'):
                    pts = sh['points']
                    mx, mz = (pts[0][0] + pts[1][0]) / 2, (pts[0][1] + pts[1][1]) / 2
                    wd = rooms.wall_distance(t['area'], t['map'], mx, mz)
                    if wd is not None:
                        t['wall_dist'] = r6(wd)
                sh['room'] = t
            d = ev['result'].get('dest')
            if d:
                tgt = by_id.get(d['stage'], s)
                d['room'] = rooms.tag(tgt['glb_crd'], d['pos'][0], d['pos'][1], d['pos'][2])
        for it in s['items']:
            it['room'] = rooms.tag(glb, it['pos'][0], it['pos'][1], it['pos'][2])


def strip_private(o):
    if isinstance(o, dict):
        return {k: strip_private(v) for k, v in o.items() if not k.startswith('_')}
    if isinstance(o, list):
        return [strip_private(v) for v in o]
    return o


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--stages', default=DEFAULT_STAGES)
    ap.add_argument('--data', default=None, help='game data dir (default: <exe dir>/data)')
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--no-rooms', action='store_true')
    a = ap.parse_args()

    exe = Exe(a.exe)
    table = json.load(open(a.stages))
    names = {s['id']: s['ps2_enum'] for s in table}
    stages = [decode_stage(exe, st, names) for st in table]
    for s in stages:
        for ev in s['events']:
            d = ev['result'].get('dest')
            if d:
                d['stage_name'] = names.get(d['stage']) if d['stage'] else s['name']
                if d['stage'] is None:
                    d['stage'] = s['id']

    if not a.no_rooms:
        data_dir = a.data or os.path.join(os.path.dirname(os.path.abspath(a.exe)), 'data')
        if os.path.isdir(os.path.join(data_dir, 'bg')):
            rooms = Rooms(data_dir)
            tag_rooms(stages, rooms)
        else:
            print('no %s/bg, skipping room tagging' % data_dir, file=sys.stderr)

    os.makedirs(a.out, exist_ok=True)
    clean = [strip_private(s) for s in stages]
    links = door_links(clean)
    for s in clean:
        s['door_links'] = [l for l in links if l['stage'] == s['id']]
        with open(os.path.join(a.out, 'stage_%02d.json' % s['id']), 'w') as f:
            json.dump(s, f, indent=1)
    summary = {'source': os.path.basename(a.exe), 'notes': 'see tools/sh2conv/EVENTS.md',
               'result_types': RESULT_TYPES,
               'trigger_kinds': {k: v[0] for k, v in TRIGGER_KINDS.items()},
               'door_sounds': door_sounds(exe),
               'door_links': links,
               'stages': clean}
    with open(os.path.join(a.out, 'events.json'), 'w') as f:
        json.dump(summary, f, indent=1)

    on_floor = sum(1 for l in links if l['to'].get('on_floor'))
    print('%d stages, %d events, %d door/move events (%d land on a floor of the looked-up room),'
          ' %d items -> %s' % (len(stages), sum(len(s['events']) for s in stages), len(links),
                               on_floor, sum(len(s['items']) for s in stages), a.out))


def door_sounds(exe):
    out = []
    for i in range(22):
        v = struct.unpack('<6h', exe.read(DOOR_SE_ADDR + 12 * i, 12))
        out.append(dict(zip(('open', 'close', 'unlock', 'jam', 'lock'), v[:5])))
    return out


def door_links(stages):
    """Flat list of every door / move event: where it is and where it takes the player."""
    links = []
    for s in stages:
        for ev in s['events']:
            r = ev['result']
            if r['type'] not in DOOR_RESULTS:
                continue
            d = r['dest']
            sh = ev['trigger'].get('shape')
            fr = {'shape': sh['shape'], 'test': sh['test']} if sh else {'shape': None}
            if sh:
                for k in ('points', 'point', 'min', 'max', 'front_normal', 'y'):
                    if k in sh:
                        fr[k] = sh[k]
                fr.update({k: v for k, v in (sh.get('room') or {}).items()
                           if k in ('room_id', 'area', 'map')})
            to = {'stage': d['stage'], 'stage_name': d['stage_name'], 'pos': d['pos'],
                  'facing': d['facing']}
            to.update({k: v for k, v in (d.get('room') or {}).items()
                       if k in ('room_id', 'area', 'map', 'on_floor')})
            links.append({'stage': s['id'], 'event': ev['index'], 'type': r['type_name'],
                          'trigger': ev['trigger']['kind_name'], 'requires': ev['flags']['require'],
                          'item': ev['trigger'].get('item'), 'from': fr, 'to': to,
                          'program': r.get('program'), 'sets_flag': r.get('sets_flag'),
                          'door_sound': r['door_sound']})
    return links


if __name__ == '__main__':
    main()
