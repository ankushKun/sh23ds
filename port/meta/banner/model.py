#!/usr/bin/env python3
"""Builds sh23ds's 3D HOME Menu banner as a binary glTF (banner.glb).

The banner is what the 3DS shows on the top screen while sh23ds is selected on the HOME Menu: the
nine rusted red squares of the sh23ds mark on a plate of black iron, hanging in fog and swaying
slowly, with a heartbeat that makes it pulse twice, ash drifting down through the scene, and the
SILENT HILL 2 wordmark in front. Every shape is generated here, so the model can be reviewed and
changed as code, and needs no 3D editor.

The pipeline and every rule below come from 3dsDL's banner (github.com/ankushKun/3dsDL,
resources/banner), which took five test builds on a console to get right, and from 3dsFM's, whose
first three builds froze the HOME Menu. The scene keeps to the shape of the 3dsDL banner that works:

- Only rigid node animation (translation, rotation and scale keys), no skinning.
- One unanimated rig node holding the placement, one animated bone under it with the sway, bob
  and pulse (3dsDL's card motion, smaller), flat parts under that with no transform of their own.
- The wordmark is a Y-axis billboard (build-cgfx.py) with a hard-edged cut-out (alpha MASK),
  never a blend.
- Nothing scales to (nearly) nothing; the pulse stays within 3dsDL's squash range.
- Fewer bones, meshes and materials than the 3dsDL banner, one texture as there, under 2,000
  triangles.

Units follow the HOME Menu banner camera (pycgfx's banner-camera.gltf): the camera sits at
(0, 1, 44.786) looking down -z with a 30 degree vertical field of view, so the plane z = 0 shows x
from -20 to 20 and y from -11 to 13, and one unit is ten pixels of the 400 x 240 top screen.
Things with a larger z sit in front of the screen when the 3D slider is up. The loop is four
seconds and ends where it starts.

The one texture is atlas.png (render.sh), 256 x 128 like 3dsDL's: the wordmark on the left, the
rust and the fog stacked on the right. Colours are given as the 3DS will show them, not as linear values.

Usage: python3 model.py atlas.png banner.glb [--static SECONDS]

--static writes the scene as it is at that moment, with no animation.
"""

import json
import math
import struct
import sys

LOOP = 4.0  # seconds


# --------------------------------------------------------------- 2D shapes


def norm2(x, y):
    length = math.hypot(x, y)
    return (x / length, y / length) if length else (0.0, 0.0)


def rounded(points, radius, segments=6):
    """Rounds every corner of a polygon with an arc of the given radius.

    radius is one number or one per corner. Works for convex and concave
    corners alike (a concave corner gets a fillet)."""
    radii = radius if isinstance(radius, (list, tuple)) else [radius] * len(points)
    out = []
    n = len(points)
    for i, (px, py) in enumerate(points):
        r = radii[i]
        if r <= 0:
            out.append((px, py))
            continue
        ax, ay = points[i - 1]
        bx, by = points[(i + 1) % n]
        ux, uy = norm2(ax - px, ay - py)
        vx, vy = norm2(bx - px, by - py)
        theta = math.acos(max(-1.0, min(1.0, ux * vx + uy * vy)))
        t = r / math.tan(theta / 2)
        sx, sy = px + ux * t, py + uy * t
        ex, ey = px + vx * t, py + vy * t
        bx_, by_ = norm2(ux + vx, uy + vy)
        d = r / math.sin(theta / 2)
        cx, cy = px + bx_ * d, py + by_ * d
        a0 = math.atan2(sy - cy, sx - cx)
        a1 = math.atan2(ey - cy, ex - cx)
        sweep = (a1 - a0 + math.pi) % (2 * math.pi) - math.pi
        for k in range(segments + 1):
            a = a0 + sweep * k / segments
            out.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return out


def signed_area(poly):
    return 0.5 * sum(
        poly[i - 1][0] * poly[i][1] - poly[i][0] * poly[i - 1][1] for i in range(len(poly))
    )


def triangulate(poly):
    """Ear clipping for a simple counter-clockwise polygon."""

    def cross(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

    def inside(p, a, b, c):
        return cross(a, b, p) >= 0 and cross(b, c, p) >= 0 and cross(c, a, p) >= 0

    idx = list(range(len(poly)))
    tris = []
    while len(idx) > 3:
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            turn = cross(a, b, c)
            if abs(turn) < 1e-9:
                idx.pop(k)  # collinear: drop the middle point
                break
            if turn < 0:
                continue  # reflex corner
            if any(inside(poly[j], a, b, c) for j in idx if j not in (i0, i1, i2)):
                continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            break
        else:
            raise RuntimeError("could not triangulate the outline (is a corner rounded less than its bevel?)")
    tris.append(tuple(idx))
    return tris


def miters(poly):
    """Per-corner outward direction and the length factor that keeps an
    offset edge parallel to the original."""
    out = []
    n = len(poly)
    for i in range(n):
        (ax, ay), (px, py), (bx, by) = poly[i - 1], poly[i], poly[(i + 1) % n]
        n1 = norm2(py - ay, -(px - ax))
        n2 = norm2(by - py, -(bx - px))
        mx, my = norm2(n1[0] + n2[0], n1[1] + n2[1])
        scale = 1 / max(0.35, mx * n1[0] + my * n1[1])
        out.append((mx, my, scale))
    return out


# --------------------------------------------------------------- 3D meshes


class Mesh:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []

    def vertex(self, p, n, uv=None):
        self.positions.append(p)
        self.normals.append(n)
        if uv is not None:
            self.uvs.append(uv)
        return len(self.positions) - 1


def norm3(x, y, z):
    length = math.sqrt(x * x + y * y + z * z)
    return (x / length, y / length, z / length)


def extrude(outline, depth, bevel):
    """A chunky solid: the outline extruded to depth, with a rounded-looking
    bevel around the front and back faces and smooth side normals.

    Convex corners need rounding at least as large as the bevel, or the
    inset face folds over itself and cannot be triangulated."""
    if signed_area(outline) < 0:
        outline = outline[::-1]
    m = miters(outline)
    inset = [(x - mx * bevel * s, y - my * bevel * s) for (x, y), (mx, my, s) in zip(outline, m)]
    h = depth / 2
    mesh = Mesh()
    n = len(outline)

    def loop(points, z, normal_of):
        return [mesh.vertex((x, y, z), normal_of(i)) for i, (x, y) in enumerate(points)]

    def band(top, bottom):
        for i in range(n):
            j = (i + 1) % n
            mesh.indices += [top[i], bottom[i], bottom[j], top[i], bottom[j], top[j]]

    # Front and back faces.
    front = loop(inset, h, lambda i: (0.0, 0.0, 1.0))
    back = loop(inset, -h, lambda i: (0.0, 0.0, -1.0))
    for a, b, c in triangulate(inset):
        mesh.indices += [front[a], front[b], front[c]]
        mesh.indices += [back[a], back[c], back[b]]

    # Bevels: normals swing from the face to the side, so highlights roll
    # round the edge like moulded plastic.
    f_in = loop(inset, h, lambda i: norm3(m[i][0] * 0.45, m[i][1] * 0.45, 1))
    f_out = loop(outline, h - bevel, lambda i: norm3(m[i][0], m[i][1], 0.3))
    band(f_in, f_out)
    s_top = loop(outline, h - bevel, lambda i: (m[i][0], m[i][1], 0.0))
    s_bot = loop(outline, -(h - bevel), lambda i: (m[i][0], m[i][1], 0.0))
    band(s_top, s_bot)
    b_out = loop(outline, -(h - bevel), lambda i: norm3(m[i][0], m[i][1], -0.3))
    b_in = loop(inset, -h, lambda i: norm3(m[i][0] * 0.45, m[i][1] * 0.45, -1))
    band(b_out, b_in)
    return mesh


def plane(width, height, uv=(0.0, 0.0, 1.0, 1.0)):
    """A camera-facing rectangle showing the atlas region uv (u0, v0, u1, v1; glTF puts the
    image's top-left at 0, 0)."""
    mesh = Mesh()
    w, h = width / 2, height / 2
    u0, v0, u1, v1 = uv
    z = (0.0, 0.0, 1.0)
    a = mesh.vertex((-w, -h, 0.0), z, (u0, v1))
    b = mesh.vertex((w, -h, 0.0), z, (u1, v1))
    c = mesh.vertex((w, h, 0.0), z, (u1, v0))
    d = mesh.vertex((-w, h, 0.0), z, (u0, v0))
    mesh.indices += [a, b, c, a, c, d]
    return mesh


def merge(*meshes_and_offsets):
    out = Mesh()
    for mesh, (ox, oy, oz) in meshes_and_offsets:
        base = len(out.positions)
        out.positions += [(x + ox, y + oy, z + oz) for x, y, z in mesh.positions]
        out.normals += mesh.normals
        out.uvs += mesh.uvs
        out.indices += [i + base for i in mesh.indices]
    return out


# ------------------------------------------------------------------ Shapes

# Where each picture sits in atlas.png, as (u0, v0, u1, v1) with v down (glTF's convention). The
# atlas is 256 x 128: the wordmark at 192 x 96 top left, the rust and the fog at 64 x 64 down the
# right. Each region is inset by a texel so filtering never picks up the neighbour.
TU, TV = 1.0 / 256, 1.0 / 128
LOGO_UV = (TU, TV, 0.75 - TU, 0.75 - TV)
RUST_UV = (0.75 + TU, TV, 1.0 - TU, 0.5 - TV)
FOG_UV = (0.75 + TU, 0.5 + TV, 1.0 - TU, 1.0 - TV)


def planar_uv(mesh, rect, uv):
    """Texture coordinates for every vertex from its x and y: rect (x0, y0, x1, y1) in model
    units maps onto the atlas region uv, the image's top at y1."""
    x0, y0, x1, y1 = rect
    u0, v0, u1, v1 = uv
    mesh.uvs = [(u0 + (u1 - u0) * (x - x0) / (x1 - x0), v0 + (v1 - v0) * (y1 - y) / (y1 - y0)) for x, y, _ in mesh.positions]
    return mesh


def square(size, radius):
    h = size / 2
    return rounded([(-h, -h), (h, -h), (h, h), (-h, h)], radius, 2)


PLATE = 10.4      # the iron plate, as the favicon's 16 units scaled by 0.65
TILE = 3 * 0.65   # each square: 3 units of the favicon
STEP = 4 * 0.65   # square plus gap


def plate():
    return extrude(square(PLATE, 1.1), 1.0, 0.3)


def tiles():
    """The nine squares as one solid, standing proud of the plate, each with its own patch of
    rust (the atlas's rust picture is spread over the whole grid)."""
    parts = []
    for row in range(3):
        for col in range(3):
            parts.append((extrude(square(TILE, 0.22), 0.6, 0.16), ((col - 1) * STEP, (1 - row) * STEP, 0.75)))
    grid = merge(*parts)
    h = STEP + TILE / 2
    return planar_uv(grid, (-h, -h, h, h), RUST_UV)


def ash():
    """A flake of ash: a small, uneven, thin plate."""
    corners = [(0.0, 0.55), (0.5, 0.3), (0.42, -0.35), (-0.1, -0.5), (-0.55, -0.12), (-0.38, 0.4)]
    return extrude(rounded(corners, 0.08, 1), 0.08, 0.02)


def backdrop():
    """The fog behind everything, as wide as the view at z = -9 and a little more."""
    m = plane(50.0, 31.0, FOG_UV)
    m.positions = [(x, y + 1.0, -9.0) for x, y, _ in m.positions]
    return m


def euler_quat(x, y, z):
    """Quaternion for rotations about x, then y, then z (radians)."""
    cx, sx = math.cos(x / 2), math.sin(x / 2)
    cy, sy = math.cos(y / 2), math.sin(y / 2)
    cz, sz = math.cos(z / 2), math.sin(z / 2)
    return (
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz,
    )


def sampled(fn, step=0.1):
    """Keys every step seconds over the whole loop, from a function of time."""
    count = int(round(LOOP / step))
    return [(k * step, fn(k * step)) for k in range(count + 1)]


def wave(t, period=LOOP, phase=0.0):
    return math.sin(2 * math.pi * t / period + phase)


def mark_tracks():
    """Sways slowly like a sign on a chain and bobs, and pulses twice on the jingle's heartbeat
    (0.45 s and 0.75 s): 3dsDL's card motion, with a smaller sway and a squash kept inside its range."""
    rotation = sampled(lambda t: euler_quat(math.radians(-6), math.radians(11) * wave(t), math.radians(2) * wave(t, phase=1.2)))
    scale = [
        (0.0, (1, 1, 1)), (0.35, (1, 1, 1)), (0.45, (1.05, 1.05, 1.05)), (0.6, (0.99, 0.99, 0.99)),
        (0.75, (1.035, 1.035, 1.035)), (0.95, (1, 1, 1)), (LOOP, (1, 1, 1)),
    ]
    translation = sampled(lambda t: (0.0, 0.18 * wave(t, phase=math.pi / 2), 0.0))
    return translation, rotation, scale


def ash_tracks(x, z, phase, drift, spin):
    """Falls from above the screen to below it once a loop, drifting side to side and turning a
    little, starting part way down (phase, 0..1) so the flakes don't fall together. At the bottom
    it jumps back above the screen between two keys 0.01 s apart, the way 3dsDL's arrow does."""
    top, bottom = 15.5, -13.5

    def at(t):
        f = (t / LOOP + phase) % 1.0
        return (x + drift * wave(t, LOOP / 2, phase * 6.0), top + (bottom - top) * f, z)

    wrap = (1.0 - phase) * LOOP          # when it reaches the bottom
    keys = [(t, at(t)) for t, _ in sampled(lambda t: None) if abs(t - wrap) > 0.03]
    if 0.0 < wrap < LOOP:
        keys += [(wrap, (at(wrap - 1e-6)[0], bottom, z)), (wrap + 0.01, (at(wrap + 0.02)[0], top, z))]
    translation = sorted(keys)
    rotation = sampled(lambda t: euler_quat(math.radians(30) * wave(t, LOOP, phase * 5.0), 0, math.radians(spin) * wave(t, LOOP, phase * 3.0)))
    return translation, rotation, None


def logo_tracks():
    """Bobs gently. It never turns: build-cgfx.py makes it a billboard, so the 3DS keeps it facing
    the viewer (a flat plane seen from behind would vanish)."""
    translation = sampled(lambda t: (7.6, 1.6 + 0.3 * wave(t, phase=math.pi / 2), 2.4))
    return translation, None, None


# ---------------------------------------------------------------- glTF out


def sample(keys, t):
    """A track's value at time t, interpolated linearly (rotations are
    normalised after blending, which is close enough between dense keys)."""
    if t <= keys[0][0]:
        return keys[0][1]
    for (t0, a), (t1, b) in zip(keys, keys[1:]):
        if t0 <= t <= t1:
            f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
            if len(a) == 4 and sum(x * y for x, y in zip(a, b)) < 0:
                b = tuple(-x for x in b)
            v = tuple(x + (y - x) * f for x, y in zip(a, b))
            if len(v) == 4:
                n = math.sqrt(sum(x * x for x in v))
                v = tuple(x / n for x in v)
            return v
    return keys[-1][1]


class Gltf:
    def __init__(self, static=None):
        self.static = static
        self.doc = {
            "asset": {"version": "2.0", "generator": "sh23ds port/meta/banner/model.py"},
            "scene": 0,
            "scenes": [{"name": "Banner", "nodes": []}],
            "nodes": [], "meshes": [], "materials": [], "accessors": [],
            "bufferViews": [], "buffers": [], "animations": [],
        }
        self.blob = bytearray()
        self.channels = []
        self.samplers = []

    def view(self, data, target=None):
        while len(self.blob) % 4:
            self.blob.append(0)
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.blob += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, kind, component=5126, target=None, bounds=False):
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
        flat = [v for value in values for v in (value if width > 1 else (value,))]
        fmt = {5126: "f", 5123: "H"}[component]
        acc = {
            "bufferView": self.view(struct.pack("<%d%s" % (len(flat), fmt), *flat), target),
            "componentType": component, "count": len(values), "type": kind,
        }
        if bounds:
            acc["min"] = [min(v[i] for v in values) for i in range(width)] if width > 1 else [min(values)]
            acc["max"] = [max(v[i] for v in values) for i in range(width)] if width > 1 else [max(values)]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def material(self, name, colour, roughness=0.35, texture=None):
        mat = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": [*colour, 1.0], "metallicFactor": 0.0, "roughnessFactor": roughness,
            },
        }
        if texture is not None:
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture}
            # A cut-out, not a blend: blended materials make pycgfx reorder
            # bones and need a separate drawing pass.
            mat["alphaMode"] = "MASK"
            mat["alphaCutoff"] = 0.5
        self.doc["materials"].append(mat)
        return len(self.doc["materials"]) - 1

    def texture(self, png, name):
        self.doc.setdefault("images", []).append({"name": name, "bufferView": self.view(png), "mimeType": "image/png"})
        self.doc.setdefault("samplers", []).append({"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071})
        self.doc.setdefault("textures", []).append({"source": len(self.doc["images"]) - 1, "sampler": 0})
        return len(self.doc["textures"]) - 1

    def mesh(self, name, mesh, material):
        attributes = {
            "POSITION": self.accessor(mesh.positions, "VEC3", target=34962, bounds=True),
            "NORMAL": self.accessor(mesh.normals, "VEC3", target=34962),
        }
        if mesh.uvs:
            attributes["TEXCOORD_0"] = self.accessor(mesh.uvs, "VEC2", target=34962)
        assert len(mesh.positions) < 65536
        indices = self.accessor(mesh.indices, "SCALAR", component=5123, target=34963)
        self.doc["meshes"].append({"name": name, "primitives": [{"attributes": attributes, "indices": indices, "material": material}]})
        return len(self.doc["meshes"]) - 1

    def node(self, name, mesh=None, parent=None, translation=None, scale=None):
        node = {"name": name}
        if mesh is not None:
            node["mesh"] = mesh
        if translation:
            node["translation"] = list(translation)
        if scale:
            node["scale"] = list(scale)
        self.doc["nodes"].append(node)
        index = len(self.doc["nodes"]) - 1
        if parent is None:
            self.doc["scenes"][0]["nodes"].append(index)
        else:
            self.doc["nodes"][parent].setdefault("children", []).append(index)
        return index

    def animate(self, node, translation=None, rotation=None, scale=None):
        for path, keys, kind in (("translation", translation, "VEC3"), ("rotation", rotation, "VEC4"), ("scale", scale, "VEC3")):
            if not keys:
                continue
            if self.static is not None:
                self.doc["nodes"][node][path] = list(sample(keys, self.static))
                continue
            times = self.accessor([t for t, _ in keys], "SCALAR", bounds=True)
            values = self.accessor([v for _, v in keys], kind)
            self.samplers.append({"input": times, "output": values, "interpolation": "LINEAR"})
            self.channels.append({"sampler": len(self.samplers) - 1, "target": {"node": node, "path": path}})

    def write(self, path):
        if self.channels:
            self.doc["animations"].append({"name": "Banner", "channels": self.channels, "samplers": self.samplers})
        while len(self.blob) % 4:
            self.blob.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.blob)}]
        text = json.dumps(self.doc, separators=(",", ":")).encode()
        text += b" " * (-len(text) % 4)
        with open(path, "wb") as out:
            out.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(self.blob)))
            out.write(struct.pack("<I4s", len(text), b"JSON") + text)
            out.write(struct.pack("<I4s", len(self.blob), b"BIN\0") + self.blob)


def build(atlas_png, out_path, static=None):
    g = Gltf(static)
    atlas = g.texture(atlas_png, "atlas")
    iron = g.material("Iron", (0.09, 0.085, 0.085), 0.55)
    rust = g.material("Rust", (1.0, 1.0, 1.0), 0.6, texture=atlas)
    flake = g.material("Ash", (0.42, 0.4, 0.38), 0.8)
    fog = g.material("Fog", (1.0, 1.0, 1.0), 1.0, texture=atlas)
    logo = g.material("Logo", (1.0, 1.0, 1.0), 1.0, texture=atlas)

    root = g.node("Banner")

    # Behind everything, unanimated, its placement in its vertices (like the mark's parts).
    g.node("Fog", g.mesh("Fog", backdrop(), fog), parent=root)

    # The mark: an unanimated rig places it, one animated bone moves it, its two parts hang
    # under that with no transform of their own (3dsDL's CardRig, Card and card parts).
    rig = g.node("MarkRig", parent=root, translation=(-10.2, 0.6, -1.0), scale=(1.1, 1.1, 1.1))
    mark = g.node("Mark", parent=rig)
    g.node("MarkPlate", g.mesh("MarkPlate", plate(), iron), parent=mark)
    g.node("MarkTiles", g.mesh("MarkTiles", tiles(), rust), parent=mark)
    g.animate(mark, *mark_tracks())

    wordmark = g.node("Logo", g.mesh("Logo", plane(20.0, 10.0, LOGO_UV), logo), parent=root)
    g.animate(wordmark, *logo_tracks())

    # Ash, drifting down at different depths, two of them in front of the wordmark.
    flake_mesh = g.mesh("Ash", ash(), flake)
    for k, (x, z, phase, drift, spin) in enumerate([
        (-15.5, 1.5, 0.05, 0.8, 40), (-3.2, 3.6, 0.42, 1.0, -55), (3.8, 0.8, 0.71, 0.7, 35),
        (12.5, 4.2, 0.24, 0.9, -45), (17.0, -2.0, 0.88, 0.6, 50),
    ]):
        node = g.node("Ash%d" % (k + 1), flake_mesh, parent=root)
        g.animate(node, *ash_tracks(x, z, phase, drift, spin))

    g.write(out_path)


def stats(path):
    """Triangles and vertices as the console draws them (a mesh used by two nodes counts twice),
    and the bone, mesh and material counts, against the 3dsDL banner's."""
    with open(path, "rb") as f:
        data = f.read()
    n = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + n])
    tris = verts = drawn = 0
    for node in doc["nodes"]:
        if "mesh" in node:
            prim = doc["meshes"][node["mesh"]]["primitives"][0]
            tris += doc["accessors"][prim["indices"]]["count"] // 3
            verts += doc["accessors"][prim["attributes"]["POSITION"]]["count"]
            drawn += 1
    out = dict(triangles=tris, vertices=verts, bones=len(doc["nodes"]), meshes=drawn, materials=len(doc["materials"]),
               textures=len(doc.get("textures", [])))
    limits = dict(triangles=2000, vertices=2048, bones=14, meshes=11, materials=7, textures=1)
    over = [k for k in limits if out[k] > limits[k]]
    print("model: " + ", ".join("%s %d (max %d)" % (k, out[k], limits[k]) for k in limits))
    if over:
        sys.exit("over the limits of the banner that works: " + ", ".join(over))


if __name__ == "__main__":
    args = sys.argv[1:]
    static = None
    if "--static" in args:
        i = args.index("--static")
        static = float(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    with open(args[0], "rb") as source:
        build(source.read(), args[1], static)
    stats(args[1])
