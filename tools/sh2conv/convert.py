#!/usr/bin/env python3
"""Convert the user's Silent Hill 2 PC data into the sh23ds pack for the 3DS.

  convert.py <game data dir> <pack out dir> [--areas ap,hp] [--max-tex 256] [--rooms ap01,ap06]

Writes, per area:
  bg/<area>/<room>.s3m        room geometry (format below)
  bg/<area>/tex/<id>.t3x      textures as ETC1 / ETC1A4 with mipmaps (tex3ds, run in the
                              devkitARM container)
  bg/<area>/rooms.txt         room list

S3M v2 (little-endian):
  char[4] "S3M1", u32 version (2), f32 bbox_min[3], f32 bbox_max[3],
  u32 n_materials, n_batches, n_vertices, n_indices,
  u32 ofs_materials, ofs_batches, ofs_vertices, ofs_indices
  material  u32 texture_id (0 = none), u32 mode, u32 color, u32 flags (bit0 texture has alpha)
  batch     u32 group (0 opaque, 1 transparent, 2 decal; bits 8..23: the map geometry's id, its
            variant in the low byte and its part group in the high one), u32 material,
            u32 first_vertex, u32 vertex_count, u32 first_index, u32 index_count
  vertex    f32 x, y, z, f32 u, v, u8 r, g, b, a, s8 nx, ny, nz, pad   (28 bytes)
            normals come from the map where its vertex section has them, otherwise from
            area-weighted face normals of the batch
  index     u16, relative to the batch's first_vertex

The .map and .cam files are the Enhanced Edition's where the player has its Essential Files (ee.py);
their textures go through the same size cap as the originals'.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ee  # noqa: E402
import host  # noqa: E402
import sh2map  # noqa: E402

GROUP = {'opaque': 0, 'transparent': 1, 'decal': 2}
DOCKER_IMAGE = 'devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5'


def pow2_floor(n):
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def prepare_texture(tex, max_size):
    """Decode, fit to a power of two within max_size (min 8), return (PIL image, has_alpha)."""
    rgba = tex.rgba()
    has_alpha = bool((rgba[:, :, 3] < 250).any())
    w = max(8, min(max_size, pow2_floor(tex.width)))
    h = max(8, min(max_size, pow2_floor(tex.height)))
    img = Image.fromarray(rgba, 'RGBA')
    if (w, h) != img.size:
        img = img.resize((w, h), Image.LANCZOS)
    return img, has_alpha


def batch_normals(b):
    """Unit normals for a batch: the map's own where present, else smoothed face normals."""
    n = np.nan_to_num(b.normals.astype(np.float64), nan=0.0, posinf=0.0, neginf=0.0)
    ln = np.linalg.norm(n, axis=1)
    if (ln > 0.5).mean() > 0.9:
        out = n / np.maximum(ln, 1e-6)[:, None]
        out[ln <= 0.5] = (0.0, -1.0, 0.0)
        return out
    tri = b.indices.reshape(-1, 3)
    p = b.positions.astype(np.float64)
    fn = np.cross(p[tri[:, 1]] - p[tri[:, 0]], p[tri[:, 2]] - p[tri[:, 0]])
    acc = np.zeros_like(p)
    for k in range(3):
        np.add.at(acc, tri[:, k], fn)
    ln = np.linalg.norm(acc, axis=1)
    return np.where(ln[:, None] > 1e-9, acc / np.maximum(ln, 1e-9)[:, None], np.array([0.0, -1.0, 0.0]))


def write_s3m(path, m, tex_alpha):
    batches = [b for b in m.batches if len(b.indices)]
    order = sorted(range(len(batches)), key=lambda i: (GROUP[batches[i].group], batches[i].material))
    verts, idx, brecs = [], [], []
    v0 = i0 = 0
    for k in order:
        b = batches[k]
        n = len(b.positions)
        if n > 65535:
            raise ValueError(f'{path}: batch with {n} vertices needs splitting')
        rec = np.zeros(n, dtype=[('p', '<f4', 3), ('uv', '<f4', 2), ('c', 'u1', 4), ('n', 'i1', 4)])
        rec['p'], rec['uv'], rec['c'] = b.positions, b.uvs, b.colors
        nrm = batch_normals(b)
        rec['n'][:, :3] = np.clip(np.round(nrm * 127), -127, 127).astype(np.int8)
        verts.append(rec.tobytes())
        idx.append(b.indices.astype('<u2').tobytes())
        # the group field's bits 8..23 carry the map geometry's id (map part switches, room.c)
        brecs.append(struct.pack('<6I', GROUP[b.group] | (b.geo_id & 0xffff) << 8, b.material, v0, n, i0,
                                 len(b.indices)))
        v0 += n
        i0 += len(b.indices)
    mats = b''.join(struct.pack('<4I', mt.texture_id, mt.mode, mt.color,
                                1 if tex_alpha.get(mt.texture_id) else 0) for mt in m.materials)
    allp = np.concatenate([b.positions for b in batches]) if batches else np.zeros((1, 3), np.float32)
    head_size = 4 + 4 + 24 + 16 + 16
    ofs_m = head_size
    ofs_b = ofs_m + len(mats)
    ofs_v = ofs_b + 24 * len(brecs)
    vbytes = b''.join(verts)
    ofs_i = ofs_v + len(vbytes)
    head = struct.pack('<4sI6f4I4I', b'S3M1', 2, *allp.min(0), *allp.max(0),
                       len(m.materials), len(brecs), v0, i0, ofs_m, ofs_b, ofs_v, ofs_i)
    ibytes = b''.join(idx)
    ibytes += b'\0' * (-len(ibytes) % 4)
    with open(path, 'wb') as f:
        f.write(head + mats + b''.join(brecs) + vbytes + ibytes)
    return v0, i0 // 3


def local_tex3ds():
    """tex3ds on PATH or in $DEVKITPRO/tools/bin, else None (then the devkitARM container runs it)."""
    found = shutil.which('tex3ds')
    if found:
        return found
    cand = os.path.join(os.environ.get('DEVKITPRO', '/opt/devkitpro'), 'tools', 'bin', 'tex3ds')
    return cand if os.access(cand, os.X_OK) else None


def encode_textures(jobs, pack_root, fmt=None, mipmaps=True, what=None):
    """jobs: list of (png path relative to pack_root, t3x path relative, has_alpha). fmt overrides the
    ETC1 / ETC1A4 choice (any tex3ds format name). The jobs are split over one subshell per CPU core
    that run side by side, since each tex3ds call uses a single core. The script goes to sh on stdin:
    a container on a macOS shared folder doesn't always see a file written just before it starts."""
    if not jobs:
        return
    if host.BROWSER:     # no tex3ds in the browser packer: t3x.py writes the same .t3x files
        import t3x
        t3x.encode_jobs(jobs, pack_root, fmt, mipmaps, what)
        return
    if os.environ.get('SH2_TEXENC') == 'native':
        # the browser packer's encoder built for this machine (tools/texenc/build.sh native), one
        # process per core: no tex3ds and no container
        import t3x
        from concurrent.futures import ProcessPoolExecutor
        full = [(os.path.join(pack_root, p), os.path.join(pack_root, t), fmt or ('etc1a4' if a else 'etc1'), mipmaps)
                for p, t, a in jobs]
        with ProcessPoolExecutor() as ex:
            list(ex.map(t3x.encode, *zip(*full), chunksize=8))
        return
    mip = '-m lanczos' if mipmaps else ''
    cmds = []
    for png, t3x, alpha in jobs:
        f = fmt or ('etc1a4' if alpha else 'etc1')
        cmds.append(f'tex3ds -f {f} -q high {mip} -z none -o "{t3x}" "{png}"')
    n = max(1, min(os.cpu_count() or 1, len(cmds)))
    lines = []
    for i in range(n):
        lines.append('( set -e')
        lines += cmds[i::n]
        lines.append(f') & p{i}=$!')
    lines.append('st=0')
    lines += [f'wait $p{i} || st=1' for i in range(n)]
    lines.append('exit $st')
    script = ('\n'.join(lines) + '\n').encode()
    local = local_tex3ds()
    if local:     # devkitPro installed on this machine, or the packer image
        env = dict(os.environ, PATH=os.path.dirname(local) + os.pathsep + os.environ.get('PATH', ''))
        cmd, cwd = ['sh', '-s'], pack_root
    else:
        env, cwd = None, None
        cmd = ['docker', 'run', '--rm', '-i', '-v', f'{os.path.abspath(pack_root)}:/pack', '-w', '/pack',
               '--user', f'{os.getuid()}:{os.getgid()}', '-e', 'PATH=/opt/devkitpro/tools/bin:/usr/bin:/bin',
               DOCKER_IMAGE, 'sh', '-s']
    for attempt in (1, 2):   # the second try covers a shared folder that was slow to show new PNGs
        r = subprocess.run(cmd, input=script, cwd=cwd, env=env)
        if r.returncode == 0:
            return
        if attempt == 1:
            import time
            time.sleep(2)
    raise subprocess.CalledProcessError(r.returncode, cmd)


def convert_area(data_dir, pack_root, area, max_tex, rooms_filter, bg='bg'):
    src = os.path.join(data_dir, bg, area)
    out = os.path.join(pack_root, 'bg', area)
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    os.makedirs(os.path.join(out, 'png'), exist_ok=True)
    maps = {}
    for fn in sorted(os.listdir(src)):
        if fn.endswith('.map'):
            try:
                maps[fn[:-4]] = sh2map.load(os.path.join(src, fn))
            except Exception as e:  # keep going; report at the end
                print(f'  ! {area}/{fn}: {e}')
    textures = {}
    for m in maps.values():
        for tid, t in m.textures.items():
            textures.setdefault(tid, t)
    rooms = [r for r in maps if r != area and (not rooms_filter or r in rooms_filter)]
    needed = {mt.texture_id for r in rooms for mt in maps[r].materials if mt.texture_id}
    tex_alpha, jobs, missing = {}, [], 0
    for n, tid in enumerate(sorted(needed)):
        host.progress(0.2 * n / max(1, len(needed)))
        t = textures.get(tid)
        if t is None:
            missing += 1
            continue
        img, alpha = prepare_texture(t, max_tex)
        tex_alpha[tid] = alpha
        png = f'bg/{area}/png/{tid:08x}.png'
        t3x = f'bg/{area}/tex/{tid:08x}.t3x'
        if not os.path.exists(os.path.join(pack_root, t3x)):
            img.save(os.path.join(pack_root, png))
            jobs.append((png, t3x, alpha))
    with host.part(0.2, 0.9):
        encode_textures(jobs, pack_root, what='textures in ' + area)
    tot_v = tot_t = 0
    for r in rooms:
        cam = os.path.join(src, f'{r}.cam')     # camera roads, used as-is by the camera port
        if os.path.exists(cam):
            shutil.copyfile(ee.file(cam), os.path.join(out, f'{r}.cam'))
        v, t = write_s3m(os.path.join(out, f'{r}.s3m'), maps[r], tex_alpha)
        tot_v += v
        tot_t += t
    gb = os.path.join(src, f'{area}GB.cam')   # area-wide camera roads of the open areas
    if os.path.exists(gb):
        shutil.copyfile(ee.file(gb), os.path.join(out, f'{area}GB.cam'))
    listed = rooms
    if rooms_filter:     # a partial run keeps the rooms already converted in the list
        old = os.path.join(out, 'rooms.txt')
        keep = set(open(old).read().split()) if os.path.exists(old) else set()
        listed = [r for r in maps if r != area and (r in keep or r in rooms)]
    with open(os.path.join(out, 'rooms.txt'), 'w') as f:
        f.write('\n'.join(listed) + '\n')
    print(f'{area}: {len(rooms)} rooms, {len(needed)} textures ({len(jobs)} encoded, {missing} missing), '
          f'{tot_v} vertices, {tot_t} triangles')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('pack')
    ap.add_argument('--areas', default='')
    ap.add_argument('--rooms', default='')
    ap.add_argument('--max-tex', type=int, default=256)
    ap.add_argument('--bg', default='bg', help='source folder under data (bg2 holds the DC-only area ma)')
    a = ap.parse_args()
    areas = a.areas.split(',') if a.areas else sorted(os.listdir(os.path.join(a.data, a.bg)))
    rooms = set(a.rooms.split(',')) if a.rooms else None
    # progress by each area's share of the map files
    weights = [sum(f.endswith('.map') for f in os.listdir(os.path.join(a.data, a.bg, x))) + 1 for x in areas]
    for area, (lo, hi) in zip(areas, host.parts(weights)):
        with host.part(lo, hi):
            convert_area(a.data, a.pack, area, a.max_tex, rooms, a.bg)


if __name__ == '__main__':
    main()
