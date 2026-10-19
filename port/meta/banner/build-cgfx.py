#!/usr/bin/env python3
"""Converts banner.glb to the CGFX model the HOME Menu banner holds.

pycgfx (https://github.com/skyfloogle/pycgfx) does the conversion; this
wrapper then fixes or adjusts what it leaves at defaults. It is 3dsDL's
(resources/banner/build-cgfx.py), whose fixes are the reason that banner
works on a console; only the lighting and the unlit materials differ.

- Each mesh must name the node (bone) it hangs from. pycgfx leaves the name
  blank, and a banner like that crashed the HOME Menu on a real console the
  moment 3dsDL was selected in its first test build. The same fix is in the pycgfx patch carried by
  CBC for YANBF (https://github.com/daaaaavia/CBC-for-YANBF), whose notes
  call it required for a banner that doesn't crash. Mesh and shape names
  are made unique at the same time (nodes sharing a mesh, like the
  sparkles, would otherwise all be called the same).
- pycgfx moves the bones of see-through (BLEND) materials, here the
  wordmark, to the end of the bone table so they draw last, but doesn't
  rebuild the table's search tree. The HOME Menu then finds the wrong bone
  when it looks one up by name and crashes (a null pointer read in "menu";
  this is what crashed 3dsDL's second test build). The tree is rebuilt, and
  BLEND materials are marked translucent so they draw in the right pass.
  Both fixes are described by CBC for YANBF too.
- The wordmark is a flat picture, drawn from the front only, so seen from
  behind it vanishes. Its bone is made a Y-axis billboard, which the 3DS
  keeps turned towards the viewer (CBC for YANBF does the same for logos).

- Its one light shines straight from the camera, and materials get no
  ambient light, so anything turned away from the viewer goes black. The
  light is moved up and to the left, like a studio key light, and every
  material gets a soft ambient fill.
- The wordmark is a picture, not a surface, so it gets full ambient light
  and shows its own colours whatever the angle.

scripts/make-banner.sh runs this in a pinned Python container with pycgfx
fetched at a pinned commit.

Usage: python3 build-cgfx.py PYCGFX_DIR banner.glb sh23ds.cgfx
"""

import math
import sys

LIMIT = 0x80000  # the HOME Menu refuses CGFX files above 512 KiB
AMBIENT = 0.36  # a little darker than 3dsDL's 0.42, for a gloomier mark
LIGHT = (0.35, -0.5, -0.8)  # direction the light travels: down, right, into the screen
UNLIT = ("Logo",)


def lookup_failures(info):
    """Names that a console lookup in this DICT would not find.

    Walks the Patricia tree the way NintendoWare does: from the root (whose
    reference bit counts as the highest), follow left or right by the bit of
    the name at each node's reference bit, until the bit index stops falling.
    """
    nodes = info.dict.nodes

    def bit(name, index):
        data = name.encode()
        return index // 8 < len(data) and (data[index // 8] >> (index & 7)) & 1

    def refbit(i):
        return float("inf") if i == 0 else nodes[i].refbit

    failures = []
    for entry in nodes[1:]:
        current, nxt = 0, nodes[0].left_index
        while refbit(current) > refbit(nxt):
            current = nxt
            nxt = nodes[current].right_index if bit(entry.name, nodes[current].refbit) else nodes[current].left_index
        if nodes[nxt] is not entry:
            failures.append(entry.name)
    return failures


def main(pycgfx_dir, gltf_path, out_path):
    sys.path.insert(0, pycgfx_dir)
    import gltflib
    import main as pycgfx
    from cgfx.shared import ColorFloat, Vector3
    from cgfx.sobj import BillboardMode

    gltf = gltflib.GLTF.load(gltf_path, load_file_resources=True)
    cgfx = pycgfx.convert_gltf(gltf)

    model = cgfx.data.models["COMMON"]

    # The bone order pycgfx chose stays; only the lookup tree is rebuilt.
    broken = lookup_failures(model.skeleton.bones)
    model.skeleton.bones.dict.regenerate()
    print("bone lookups broken by pycgfx's sort: %s" % (", ".join(broken) or "none"))

    model.skeleton.bones["Logo"].billboard_mode = BillboardMode.YAxial

    for material in gltf.model.materials:
        if material.alphaMode == "BLEND":
            model.materials[material.name].transluscency_kind = 1  # translucent

    bones = list(model.skeleton.bones)
    materials = list(model.materials)
    for mesh in model.meshes.data.contents:
        shape = model.shapes.data.contents[mesh.shape_index]
        bone = shape.primitive_sets.data.contents[0].related_bones.data.contents[0]
        mesh.mesh_node_name = bones[bone]
        mesh.name = shape.name = "%s_%s" % (bones[bone], materials[mesh.material_index])
    for name in model.materials:
        level = 1.0 if name in UNLIT else AMBIENT
        model.materials[name].material_color.ambient = ColorFloat(level, level, level, 1)

    length = math.sqrt(sum(c * c for c in LIGHT))
    for name in cgfx.data.lights:
        cgfx.data.lights[name].position_or_direction = Vector3(*(c / length for c in LIGHT))

    tables = {
        "models": cgfx.data.models, "textures": cgfx.data.textures, "luts": cgfx.data.lookup_tables,
        "lights": cgfx.data.lights, "scenes": cgfx.data.scenes, "animations": cgfx.data.skeletal_animations,
        "bones": model.skeleton.bones, "materials": model.materials,
        "animation groups": model.animation_group_descriptions,
    }
    for label, info in tables.items():
        failures = lookup_failures(info)
        if failures:
            sys.exit("lookup in %s would not find: %s" % (label, ", ".join(failures)))
    print("every name lookup resolves (%s)" % ", ".join(tables))

    data = pycgfx.write(cgfx)
    if len(data) > LIMIT:
        sys.exit("CGFX is %d bytes; the HOME Menu's limit is %d" % (len(data), LIMIT))
    with open(out_path, "wb") as out:
        out.write(data)
    print("%s: %d bytes (limit %d), materials: %s" % (out_path, len(data), LIMIT, ", ".join(model.materials)))
    for mesh in model.meshes.data.contents:
        print("  mesh %s on node %s" % (mesh.name, mesh.mesh_node_name))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
