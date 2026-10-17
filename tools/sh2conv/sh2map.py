"""Silent Hill 2 PC .map reader (geometry, materials, embedded DXT textures).

Layout (format notes: Polymega/SilentHillDatabase, alanm20/SH2_map; this is an independent
implementation):

  file      u32 magic 0x20010510, u32 size, u32 subfile_count, u32 0
  subfile   u32 type, u32 length, u32, u32, then the body
    type 1  geometry: u32 magic, u32 geo_count, u32 geo_size, u32 material_count
            geo_count x { u32 id, u32 size, u32 opaque_ofs, u32 transparent_ofs, u32 decal_ofs }
            material_count x { u16 mode, u16 texture_id, u32 color, u32 overlay, f32 specularity }
    type 2  textures: u32 x4 header, then { u32 id, ... sprites ... } until id == 0, then 12 pad
  mesh      f32[4] bbox_min, f32[4] bbox_max, u32 vtx_hdr_ofs, u32 idx_ofs, u32 idx_len, u32,
            u32 part_group_count, part_group_count x { u32 material, u32 section, u32 part_count,
            part_count x { u16 strip_len, u8, u8 prim (1 strip, 2 fan, 3 list), u16 first, u16 last } }
  vertices  sections of stride 0x14 (pos, uv), 0x18 (pos, color, uv), 0x20 (pos, normal, uv) or
            0x24 (pos, normal, color, uv)
"""
import struct
from dataclasses import dataclass, field

import numpy as np

import ee

MAP_MAGIC = 0x20010510


# ---------------------------------------------------------------- DXT decoding

def _565(c):
    r = ((c >> 11) & 31).astype(np.uint16)
    g = ((c >> 5) & 63).astype(np.uint16)
    b = (c & 31).astype(np.uint16)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def _color_blocks(blk, four_color_only):
    """blk: (n, 8) uint8 BC1 color blocks -> (n, 16, 4) RGBA."""
    c0 = blk[:, 0].astype(np.uint32) | (blk[:, 1].astype(np.uint32) << 8)
    c1 = blk[:, 2].astype(np.uint32) | (blk[:, 3].astype(np.uint32) << 8)
    idx = (blk[:, 4].astype(np.uint32) | (blk[:, 5].astype(np.uint32) << 8) |
           (blk[:, 6].astype(np.uint32) << 16) | (blk[:, 7].astype(np.uint32) << 24))
    p0, p1 = _565(c0), _565(c1)
    four = (c0 > c1) | four_color_only
    pal = np.empty((len(blk), 4, 4), np.int32)
    pal[:, 0, :3], pal[:, 1, :3] = p0, p1
    pal[:, :, 3] = 255
    pal[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal[:, 3, 3] = np.where(four, 255, 0)
    sel = (idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    return np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(4, 2), 1).astype(np.uint8)


def decode_dxt(data, width, height, kind):
    """kind: 1 (DXT1), 3 (DXT2/3), 5 (DXT4/5). Returns (h, w, 4) uint8 RGBA."""
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    bsize = 8 if kind == 1 else 16
    raw = np.frombuffer(data[:bw * bh * bsize], np.uint8).reshape(-1, bsize)
    if kind == 1:
        px = _color_blocks(raw, False)
    else:
        px = _color_blocks(raw[:, 8:], True)
        if kind == 3:
            a = raw[:, :8]
            nib = np.stack([a & 15, a >> 4], -1).reshape(-1, 16)
            px[:, :, 3] = (nib * 17).astype(np.uint8)
        else:
            a0, a1 = raw[:, 0].astype(np.int32), raw[:, 1].astype(np.int32)
            bits = np.zeros(len(raw), np.uint64)
            for i in range(6):
                bits |= raw[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
            sel = ((bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
            pal = np.empty((len(raw), 8), np.int32)
            pal[:, 0], pal[:, 1] = a0, a1
            eight = a0 > a1
            for i in range(1, 7):
                pal[:, i + 1] = np.where(eight, ((7 - i) * a0 + i * a1) // 7,
                                         ((5 - i) * a0 + i * a1) // 5 if i < 5 else 0)
            pal[:, 6] = np.where(eight, pal[:, 6], 0)
            pal[:, 7] = np.where(eight, pal[:, 7], 255)
            px[:, :, 3] = np.take_along_axis(pal, sel, 1).astype(np.uint8)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return np.ascontiguousarray(img[:height, :width])


# Embedded texture format codes in .map texture sub-files
MAP_TEX_KIND = {0x100: 1, 0x101: 3, 0x102: 3, 0x103: 5, 0x104: 5}


# ---------------------------------------------------------------- model

@dataclass
class Material:
    mode: int
    texture_id: int
    color: int
    overlay: int
    specularity: float


@dataclass
class Texture:
    id: int
    width: int
    height: int
    kind: int          # 1, 3 or 5 (DXT)
    data: bytes

    def rgba(self):
        return decode_dxt(self.data, self.width, self.height, self.kind)


@dataclass
class Batch:
    """Triangles of one material from one vertex section, ready for a single draw call."""
    group: str          # 'opaque', 'transparent' or 'decal'
    material: int
    positions: np.ndarray   # (n, 3) float32
    uvs: np.ndarray         # (n, 2) float32
    colors: np.ndarray      # (n, 4) uint8, white if the section has none
    normals: np.ndarray     # (n, 3) float32, zero if the section has none
    indices: np.ndarray     # (m,) uint32, triangle list into this batch's vertices


@dataclass
class MapFile:
    name: str
    materials: list = field(default_factory=list)
    textures: dict = field(default_factory=dict)
    batches: list = field(default_factory=list)


class _Reader:
    def __init__(self, data):
        self.d = data

    def u32(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def unpack(self, fmt, o):
        return struct.unpack_from('<' + fmt, self.d, o)


def _strip_to_list(idx, prim):
    if prim == 3:
        return idx[:len(idx) // 3 * 3]
    out = []
    if prim == 1:
        for i in range(len(idx) - 2):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            if a == b or b == c or a == c:
                continue
            out.extend((a, b, c) if i % 2 == 0 else (b, a, c))
    elif prim == 2:
        for i in range(1, len(idx) - 1):
            out.extend((idx[0], idx[i], idx[i + 1]))
    return np.array(out, np.uint32)


def _vertex_sections(r, base):
    _, count = r.unpack('II', base)
    info = [r.unpack('III', base + 8 + 12 * j) for j in range(count)]
    start = base + 8 + 12 * count
    sections = []
    for ofs, stride, length in info:
        raw = np.frombuffer(r.d, np.uint8, length, start + ofs).reshape(-1, stride)
        f = raw.view(np.float32) if stride % 4 == 0 else None
        n = len(raw)
        pos = f[:, 0:3].copy()
        nrm = np.zeros((n, 3), np.float32)
        col = np.full((n, 4), 255, np.uint8)
        if stride == 0x14:
            uv = f[:, 3:5].copy()
        elif stride == 0x20:
            nrm, uv = f[:, 3:6].copy(), f[:, 6:8].copy()
        elif stride == 0x18:
            # pos, colour, uv: the layout of material mode 3 (PC VS2 reads v5 and v7)
            uv = f[:, 4:6].copy()
            col = raw[:, 0x0c:0x10][:, [2, 1, 0, 3]].copy()   # stored BGRA
        elif stride >= 0x24:
            nrm, uv = f[:, 3:6].copy(), f[:, 7:9].copy()
            col = raw[:, 0x18:0x1c][:, [2, 1, 0, 3]].copy()   # stored BGRA
        else:
            uv = np.zeros((n, 2), np.float32)
        sections.append((pos, uv, col, nrm))
    return sections


def _collect(batches, group, sections, section, material, tris):
    pos, uv, col, nrm = sections[section]
    used, remap = np.unique(tris, return_inverse=True)
    batches.append(Batch(group, material, pos[used], uv[used], col[used], nrm[used],
                         remap.astype(np.uint32)))


def _mesh_group(r, base, group, batches):
    count = r.u32(base)
    for ofs in r.unpack('I' * count, base + 4):
        m = base + ofs
        vtx_ofs, idx_ofs, idx_len, _, pg_count = r.unpack('IIIII', m + 32)
        indices = np.frombuffer(r.d, np.uint16, idx_len // 2, m + idx_ofs).astype(np.uint32)
        sections = _vertex_sections(r, m + vtx_ofs)
        p = m + 52
        cursor = 0
        acc = {}
        for _ in range(pg_count):
            material, section, part_count = r.unpack('III', p)
            p += 12
            for _ in range(part_count):
                strip_len, _, prim, _, _ = r.unpack('HBBHH', p)
                p += 8
                n = strip_len * 3 if prim == 3 else strip_len
                tris = _strip_to_list(indices[cursor:cursor + n], prim)
                cursor += n
                acc.setdefault((material, section), []).append(tris)
        for (material, section), parts in acc.items():
            tris = np.concatenate(parts) if parts else np.zeros(0, np.uint32)
            if len(tris):
                _collect(batches, group, sections, section, material, tris)


def _decals(r, base, batches):
    count = r.u32(base)
    for ofs in r.unpack('I' * count, base + 4):
        g = base + ofs
        vtx_ofs, idx_ofs, idx_len, decal_count = r.unpack('IIII', g + 32)
        indices = np.frombuffer(r.d, np.uint16, idx_len // 2, g + idx_ofs).astype(np.uint32)
        sections = _vertex_sections(r, g + vtx_ofs)
        p, cursor = g + 48, 0
        for _ in range(decal_count):
            material, section, strip_len, strip_count = r.unpack('IIII', p)
            p += 16
            tris = []
            for _ in range(strip_count):
                tris.append(_strip_to_list(indices[cursor:cursor + strip_len], 1))
                cursor += strip_len
            tris = np.concatenate(tris) if tris else np.zeros(0, np.uint32)
            if len(tris):
                _collect(batches, 'decal', sections, section, material, tris)


def _textures(r, p, textures):
    p += 16
    while True:
        tex_id = r.u32(p)
        if tex_id == 0:
            return
        _, _, _, _, sprite_count = r.unpack('HHHHI', p + 4)
        p += 4 + 28
        for _ in range(sprite_count):
            _, _, _, w, h, fmt, length, _, _, _ = r.unpack('IHHHHIIIII', p)
            p += 32
            if length:
                textures[tex_id] = Texture(tex_id, w, h, MAP_TEX_KIND.get(fmt, 3), bytes(r.d[p:p + length]))
                p += length


def load(path):
    """A .map file; the Enhanced Edition's version when the player has it (ee.py)."""
    return ee.load(path, _load)


def _load(path):
    with open(path, 'rb') as f:
        data = f.read()
    r = _Reader(data)
    magic, _, sub_count, _ = r.unpack('IIII', 0)
    if magic != MAP_MAGIC:
        raise ValueError(f'{path}: not a PC map (magic {magic:#x})')
    m = MapFile(path)
    p = 16
    for _ in range(sub_count):
        sub_type, sub_len, _, _ = r.unpack('IIII', p)
        body = p + 16
        if sub_type == 1:
            _, geo_count, geo_size, mat_count = r.unpack('IIII', body)
            g = body + 16
            for _ in range(geo_count):
                _, size, o_ofs, t_ofs, d_ofs = r.unpack('IIIII', g)
                if o_ofs:
                    _mesh_group(r, g + o_ofs, 'opaque', m.batches)
                if t_ofs:
                    _mesh_group(r, g + t_ofs, 'transparent', m.batches)
                if d_ofs:
                    _decals(r, g + d_ofs, m.batches)
                g += size
            mp = body + geo_size
            for i in range(mat_count):
                m.materials.append(Material(*r.unpack('HHIIf', mp + 16 * i)))
        elif sub_type == 2:
            _textures(r, body, m.textures)
        p = body + sub_len
    return m
