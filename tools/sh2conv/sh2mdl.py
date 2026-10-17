"""Silent Hill 2 PC character model (.mdl) and animation (.anm) reader.

Format knowledge from the Silent Hill Museum Kaitai specs (laura-a-n-n/silent-hill-museum, MIT,
ksy/mdl.ksy and ksy/anm.ksy) and its viewer's skinning/animation code; this is an independent
Python implementation.

Model essentials
  * 64-byte file header: texture count/offset, model header offset.
  * Model header (magic 03 00 ff ff) at file offset 64; its offsets are relative to its start.
  * initial_matrices[bone]: bind pose in model space (column-major 4x4).
  * skeleton_tree[bone]: parent index, 255 for roots.
  * bone_pairs[i] = (parent, child); vertex weights 1..3 index pairs (offset by the primitive's
    bone count) and mean "blend toward the pair's child bone".
  * Opaque vertices (0x30 bytes) are stored in the local space of their primary bone
    (primitive.bone_indices[vertex.bone_index_0]); transform by that bone's initial matrix to get
    the bind pose in model space.
  * Primitives index a shared triangle-strip index list (degenerate triangles separate strips).
"""
import struct
from dataclasses import dataclass, field

import numpy as np

from sh2map import decode_dxt

TEX_KIND = {0: 1, 1: 3, 2: 3, 3: 5, 4: 5}


@dataclass
class Primitive:
    bone_indices: list          # u16 list into the skeleton
    pair_indices: list          # u16 list into bone_pairs
    texture_index: int          # index into texture_pairs
    start: int
    length: int
    pose_index: int
    material_type: int
    backface_culling: int
    diffuse: tuple
    ambient: tuple
    specular: tuple
    transparent: bool = False


@dataclass
class Model:
    bone_count: int
    parents: np.ndarray            # (n,) int
    initial: np.ndarray            # (n, 4, 4) model-space bind matrices (row-major, M @ v)
    pairs: list                    # [(parent, child)]
    primitives: list
    strip_indices: np.ndarray      # opaque u16 strip list
    vertices: dict                 # opaque: pos (bind, model space), uv, nrm, bones (n,4), weights (n,4)
    transparent: dict = None
    tstrip_indices: np.ndarray = None
    textures: dict = field(default_factory=dict)   # texture_id -> (w, h, kind, bytes)
    main_texture_ids: list = field(default_factory=list)
    texture_pairs: list = field(default_factory=list)   # (texture_index, sprite_id)


def _mat(d, o):
    m = np.frombuffer(d, '<f4', 16, o).reshape(4, 4).T.astype(np.float64)   # column-major on disk
    return m


def _strip_groups(strip, starts, total):
    """Split the shared strip list at primitive starts; return per-primitive triangle lists."""
    out = []
    bounds = sorted(set(starts)) + [total]
    for a, b in zip(bounds[:-1], bounds[1:]):
        seg = strip[a:b]
        tris = []
        for i in range(len(seg) - 2):
            x, y, z = int(seg[i]), int(seg[i + 1]), int(seg[i + 2])
            if x == y or y == z or x == z:
                continue
            tris.append((x, y, z) if i % 2 == 0 else (y, x, z))
        out.append((a, np.array(tris, np.int64).reshape(-1, 3)))
    return dict(out)


def load_mdl(path):
    d = open(path, 'rb').read()
    no_tex = d[0]
    _, tex_count, tex_ofs, _, mdl_ofs = struct.unpack_from('<IIIII', d, 4)
    B = mdl_ofs or 64                                     # model header base (0x40 in practice)
    u = lambda o: struct.unpack_from('<I', d, B + o)[0]   # noqa: E731
    if d[B:B + 4] != b'\x03\x00\xff\xff':
        raise ValueError(f'{path}: bad model magic')
    bone_count = u(12)
    pairs_count, pairs_ofs = u(20), u(24)
    prim_count, prim_ofs = u(32), u(36)
    tprim_count, tprim_ofs = u(40), u(44)
    tex_blocks_count = u(48)
    tex_id_count = u(56)
    vertex_count, vertex_ofs = u(128), u(132)
    tvertex_count, tvertex_ofs = u(136), u(140)
    tri_ofs, ttri_ofs = u(144), u(148)

    init_ofs, skel_ofs = u(8), u(16)
    initial = np.stack([_mat(d, B + init_ofs + 64 * i) for i in range(bone_count)])
    parents = np.frombuffer(d, np.uint8, bone_count, B + skel_ofs).astype(np.int64)
    pairs = [tuple(d[B + pairs_ofs + 2 * i:B + pairs_ofs + 2 * i + 2]) for i in range(pairs_count)]

    # texture metadata follows the default pcms matrices
    pcms_ofs = u(28)
    meta = B + pcms_ofs + 64 * pairs_count
    main_ids = list(struct.unpack_from(f'<{tex_blocks_count}I', d, meta))
    meta += 4 * tex_blocks_count
    if tex_blocks_count % 4:
        meta += 16 - (4 * tex_blocks_count) % 16
    tpairs = [struct.unpack_from('<II', d, meta + 8 * i) for i in range(tex_id_count)]

    prims = []
    p = B + prim_ofs
    for _ in range(prim_count):
        size = struct.unpack_from('<I', d, p)[0]
        h = p + 4
        bcount, bofs, pcount, pofs, tcount, tofs, _samp = struct.unpack_from('<7I', d, h + 4)
        mtype, _, pose, _ = d[h + 32:h + 36]
        cull = struct.unpack_from('<I', d, h + 36)[0]
        diffuse = struct.unpack_from('<3f', d, h + 64)
        ambient = struct.unpack_from('<3f', d, h + 80)
        specular = struct.unpack_from('<3f', d, h + 96)
        start, length = struct.unpack_from('<II', d, h + 112)
        bones = list(struct.unpack_from(f'<{bcount}H', d, h + 124))
        pidx = list(struct.unpack_from(f'<{pcount}H', d, p + pofs)) if pcount else []
        tidx = list(struct.unpack_from(f'<{tcount}H', d, p + tofs)) if tcount else [0]
        prims.append(Primitive(bones, pidx, tidx[0], start, length, pose, mtype, cull, diffuse, ambient, specular))
        p += size

    # opaque vertices
    vt = np.frombuffer(d, dtype=[('p', '<f4', 3), ('w', '<f4', 4), ('n', '<i2', 3), ('pad', '<u2'),
                                 ('uv', '<f4', 2), ('b', 'u1', 4)], count=vertex_count, offset=B + vertex_ofs)
    end_idx = min(x for x in (vertex_ofs, tprim_ofs if tprim_count else vertex_ofs) if x > tri_ofs)
    strip = np.frombuffer(d, '<u2', (end_idx - tri_ofs) // 2, B + tri_ofs)

    # map vertices to the primitive that uses them, then to skeleton bones
    total = max(pr.start + pr.length for pr in prims)
    groups = _strip_groups(strip, [pr.start for pr in prims], total)
    owner = np.full(vertex_count, -1, np.int64)
    for k, pr in enumerate(prims):
        tris = groups.get(pr.start, np.zeros((0, 3), np.int64))
        owner[np.unique(tris)] = k
    pos = np.zeros((vertex_count, 3))
    nrm = vt['n'].astype(np.float64) / -32768.0
    bones = np.zeros((vertex_count, 4), np.int64)
    weights = vt['w'].astype(np.float64)
    for i in range(vertex_count):
        k = owner[i]
        if k < 0:
            continue
        pr = prims[k]
        b0 = pr.bone_indices[vt['b'][i, 0]]
        M = initial[b0]
        pos[i] = (M @ np.r_[vt['p'][i], 1.0])[:3]
        nrm[i] = M[:3, :3] @ nrm[i]
        bones[i, 0] = b0
        for j in (1, 2, 3):
            x = int(vt['b'][i, j])
            if x and x != 255 and weights[i, j] > 0:
                pi = x - len(pr.bone_indices)
                if 0 <= pi < len(pr.pair_indices):
                    bones[i, j] = pairs[pr.pair_indices[pi]][1]
    s = weights.sum(1, keepdims=True)
    weights = np.where(s > 0, weights / np.where(s > 0, s, 1), np.array([1.0, 0, 0, 0]))
    verts = dict(pos=pos, uv=vt['uv'].astype(np.float64), nrm=nrm, bones=bones, weights=weights, groups=groups)

    textures = {}
    if tex_count and not no_tex:
        t = tex_ofs + 16
        for _ in range(tex_count):
            tid, w, h, _, _, sprites = struct.unpack_from('<IHHHHH', d, t)
            t += 4 + 8 + 2 + 18
            fmt = None
            for _ in range(sprites):
                fmt = d[t + 12]
                t += 32
            kind = TEX_KIND.get(fmt, 3)
            size = w * h // 2 if kind == 1 else w * h
            textures[tid] = (w, h, kind, d[t:t + size])
            t += size

    return Model(bone_count, parents, initial, pairs, prims, strip, verts,
                 textures=textures, main_texture_ids=main_ids, texture_pairs=tpairs)


def texture_rgba(model, tid):
    w, h, kind, data = model.textures[tid]
    return decode_dxt(data, w, h, kind)


# ---------------------------------------------------------------- animation

def _f16(h):
    return float(np.frombuffer(struct.pack('<H', h), np.float16)[0])


def load_anm(path, model):
    """Returns a list of frames; each frame is a list (per bone) of (rot_xyz or None, trans or None)."""
    d = open(path, 'rb').read()
    n = model.bone_count
    per_frame = (n + 7) & ~7
    roots = model.parents == 255
    frames, cur = [], [None] * n
    p, block = 0, 0
    while p + 4 <= len(d):
        header = struct.unpack_from('<I', d, p)[0]
        p += 4
        for j in range(8):
            t = (header >> (4 * j)) & 7
            bone = (block * 8) % per_frame + j
            root = bool(roots[bone % n])
            rot = trans = None
            if t == 1:
                rot = struct.unpack_from('<3h', d, p); p += 6
            elif t == 2:
                if root:
                    trans = struct.unpack_from('<3f', d, p); p += 12
                else:
                    trans = tuple(_f16(x) for x in struct.unpack_from('<3H', d, p)); p += 6
                rot = struct.unpack_from('<3h', d, p); p += 6
            elif t == 3:
                rot = struct.unpack_from('<3h', d, p); p += 6 + 8 + 2
            elif t in (4, 5):
                if t == 4 and root:
                    trans = struct.unpack_from('<3f', d, p); p += 12
                elif t == 5 and root:
                    trans = tuple(_f16(x) for x in struct.unpack_from('<HxxHxxH', d, p)); p += 12
                else:
                    trans = tuple(_f16(x) for x in struct.unpack_from('<3H', d, p)); p += 6
                rot = struct.unpack_from('<3h', d, p); p += 6 + 8 + 2
            elif t == 6:
                if root:
                    trans = struct.unpack_from('<3f', d, p); p += 24
                else:
                    # two half-float translations (6 + 6 bytes), PC 0x54fc67
                    trans = tuple(_f16(x) for x in struct.unpack_from('<3H', d, p)); p += 6 + 6
                rot = struct.unpack_from('<3h', d, p); p += 6 + 8 + 2
            if bone < n:
                cur[bone] = (None if rot is None else tuple(v / 4096.0 for v in rot), trans, t)
        block += 1
        if (block * 8) % per_frame == 0:
            frames.append(cur)
            cur = [None] * n
    return frames


def euler_zyx(rx, ry, rz):
    """three.js Euler order 'ZYX': R = Rz @ Ry @ Rx."""
    cx, sx, cy, sy, cz, sz = np.cos(rx), np.sin(rx), np.cos(ry), np.sin(ry), np.cos(rz), np.sin(rz)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def bind_locals(model):
    """Local (parent-relative) bind transforms, as the museum builds its three.js bones."""
    loc = np.empty_like(model.initial)
    for i in range(model.bone_count):
        p = model.parents[i]
        loc[i] = model.initial[i] if p == 255 else np.linalg.inv(model.initial[p]) @ model.initial[i]
    return loc


def pose(model, frame, locals_=None):
    """World matrices for one animation frame (bones without data keep their bind local)."""
    loc = bind_locals(model) if locals_ is None else locals_
    world = np.empty_like(loc)
    for i in range(model.bone_count):
        L = loc[i].copy()
        entry = frame[i] if frame else None
        if entry and entry[0] is not None:
            L[:3, :3] = euler_zyx(*entry[0])
        if entry and entry[1] is not None:
            L[:3, 3] = entry[1]
        p = model.parents[i]
        world[i] = L if p == 255 else world[p] @ L
    return world


def skin(model, world):
    """Linear blend skinning of the opaque vertices for given world matrices."""
    inv_bind = np.linalg.inv(model.initial)
    skinmats = world @ inv_bind
    v = model.vertices
    P = np.c_[v['pos'], np.ones(len(v['pos']))]
    out = np.zeros((len(P), 3))
    for j in range(4):
        M = skinmats[v['bones'][:, j]]
        out += v['weights'][:, j:j + 1] * np.einsum('nij,nj->ni', M, P)[:, :3]
    return out
