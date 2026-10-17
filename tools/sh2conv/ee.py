#!/usr/bin/env python3
"""Silent Hill 2: Enhanced Edition packs: use the player's improved files where they have them.

The Enhanced Edition's d3d8.dll (Common/FileSystemHooks.cpp in its source) hooks every file open of
the game (fopen, CreateFileA/W, BinkOpen). For a path that starts with "data\\" it first tries the
same path under "lang\\" (only when EnableLangPath is set) and then under "sh2e\\" (or the folder
named by CustomModFolder in d3d8.ini), and opens the original only when neither exists. Every
package of the Enhanced Edition (Essential Files, Image, FMV and Audio Enhancement Packs, Credits)
installs into that one sh2e folder next to sh2pc.exe, as same-named files in the game's own
formats. See tools/sh2conv/EE_PACKS.md for what each pack replaces.

The converters call file() (or path()) for every game file they read, so a player's sh2e folder
takes the place of data/ the way it does in the PC game, file by file, falling back to data/.

  ee.file(path)          the replacement of a file under <game>/data, or path itself
  ee.path(data, rel)     the same for <data>/<rel>
  ee.load(path, loader)  loader(file(path)); if a replacement does not parse, the original
  ee.is_ee(path)         True for a path file() returned from the sh2e folder

Environment:
  SH2EE=0          ignore the sh2e folder (build from data/ only)
  SH2EE_DIR=dir    the mod folder (CustomModFolder): absolute, or relative to the game folder
  SH2EE_SKIP=list  comma separated categories to leave out: images (pic/, menu/, etc/effect/),
                   fmv (movie/), audio (sound/), essentials (everything else: maps, collision,
                   models, cutscene scripts, messages)
  SH2EE_LOG=file   append "<data path> <- <replacement>" for every replacement used

Command line (for tools/make_pack.sh):
  ee.py file <path>                            prints file(path)
  ee.py copytree <data> <rel> <dst> [skip ...] copies data/<rel> to dst, each file from sh2e when
                                               there, keeping the original names; skips the named
                                               subfolders
  ee.py report <data>                          lists the sh2e files the packer would use
"""
import os
import shutil
import struct
import sys

CATEGORIES = (('images', ('pic/', 'menu/', 'etc/effect/', 'effect/')),
              ('fmv', ('movie/',)),
              ('audio', ('sound/',)))
# The game asks for movie/end.bik; the FMV packs have shipped it as ending.bik too (UpdateModPath
# tries end.bik, then ending.bik, in sh2e first).
ALIASES = {'movie/end.bik': ('movie/end.bik', 'movie/ending.bik')}

_dir_cache = {}
_used = {}          # replacement path -> data-relative path
_rejected = set()   # replacements that failed to parse: the original is used from then on
_logged = set()


def category(rel):
    rel = rel.replace('\\', '/').lower()
    for name, prefixes in CATEGORIES:
        if rel.startswith(prefixes):
            return name
    return 'essentials'


def enabled():
    return os.environ.get('SH2EE', '1') not in ('0', 'no', 'off', '')


def _skipped(rel):
    skip = {s.strip().lower() for s in os.environ.get('SH2EE_SKIP', '').split(',') if s.strip()}
    return category(rel) in skip


def mod_root(game):
    """The sh2e folder (or SH2EE_DIR) of a game folder, or None."""
    name = os.environ.get('SH2EE_DIR') or 'sh2e'
    root = name if os.path.isabs(name) else _find_ci(game, name.replace('\\', '/'))
    return root if root and os.path.isdir(root) else None


def _listing(d):
    if d not in _dir_cache:
        try:
            _dir_cache[d] = {n.lower(): n for n in os.listdir(d)}
        except OSError:
            _dir_cache[d] = {}
    return _dir_cache[d]


def _find_ci(root, rel):
    """root/rel matched without regard to case (the PC game runs on a case-blind file system)."""
    cur = root
    for part in rel.replace('\\', '/').split('/'):
        if not part:
            continue
        name = _listing(cur).get(part.lower())
        if name is None:
            return None
        cur = os.path.join(cur, name)
    return cur


def _split(path):
    """(game folder, path relative to its data folder) for a path inside <game>/data, else None."""
    p = os.path.abspath(path)
    parts = []
    while True:
        head, tail = os.path.split(p)
        if not tail:
            return None
        if tail.lower() == 'data' and parts:
            return head, '/'.join(reversed(parts))
        parts.append(tail)
        p = head


def _candidates(path):
    """The path as given, then (for make_pack's folders of links) what a link points to."""
    yield path
    if os.path.islink(path):
        yield os.path.join(os.path.dirname(os.path.abspath(path)), os.readlink(path))
        yield os.path.realpath(path)


def file(path):
    """The Enhanced Edition replacement of a game file, or the path itself."""
    if not enabled():
        return path
    for cand in _candidates(path):
        hit = _split(cand)
        if not hit:
            continue
        game, rel = hit
        if _skipped(rel):
            return path
        root = mod_root(game)
        if not root:
            continue
        for alias in ALIASES.get(rel.lower(), (rel,)):
            repl = _find_ci(root, alias)
            if repl and os.path.isfile(repl) and repl not in _rejected:
                _note(rel, repl)
                return repl
    return path


def path(data, rel):
    return file(os.path.join(data, rel))


def is_ee(p):
    return p in _used


def _note(rel, repl):
    _used[repl] = rel
    if repl in _logged:
        return
    _logged.add(repl)
    print('  EE: data/%s <- %s' % (rel, repl), file=sys.stderr)
    log = os.environ.get('SH2EE_LOG')
    if log:
        with open(log, 'a') as f:
            f.write('data/%s <- %s\n' % (rel, repl))


def load(path, loader):
    """loader(replacement); when a replacement does not parse, a warning and loader(original)."""
    p = file(path)
    if p == path:
        return loader(path)
    try:
        return loader(p)
    except Exception as e:     # anything a format change can raise
        print('  EE: %s did not load (%s: %s), using the original' % (p, type(e).__name__, e), file=sys.stderr)
        _rejected.add(p)
        _used.pop(p, None)
        return loader(path)


def read(path):
    with open(file(path), 'rb') as f:
        return f.read()


# ------------------------------------------------------------------ textures

def tex_size(path):
    """(width, height) of a single-sprite .tex / .tbn2 file (the sprite header at 0x30), or of a
    PS2-format .tex (0x40-byte file header, the sprite's size at 0x48)."""
    with open(path, 'rb') as f:
        d = f.read(0x50)
    if d[0xc:0x10] == b'\xa7\xa7\xa7\xa7':
        return struct.unpack_from('<HH', d, 0x48)
    if struct.unpack_from('<I', d, 0)[0] != 0x19990901:
        raise ValueError('%s: not a .tex file' % path)
    return struct.unpack_from('<HH', d, 0x38)


def fit(img, size):
    """img resized to size (w, h) with Lanczos, when it is not that size already. Pillow resamples
    RGBA premultiplied, so transparent texels do not bleed into the edges."""
    return img if img.size == tuple(size) else img.resize(tuple(size), _lanczos())


def _lanczos():
    from PIL import Image
    return Image.LANCZOS


def like_original(img, path):
    """An image from a replacement brought back to the original file's size, for textures the port
    addresses in the original's texels (puzzle sprite sheets, map marker sheets); others pass."""
    if not is_ee(file(path)):
        return img
    try:
        return fit(img, tex_size(path))
    except (OSError, ValueError, struct.error):
        return img


# ------------------------------------------------------------------ command line

def copytree(data, rel, dst, skip=()):
    """data/<rel> to dst, each file replaced by its sh2e version when there (under the original's
    name). Returns (files, replaced)."""
    src = os.path.join(data, rel)
    n = r = 0
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = sorted(d for d in dirnames if os.path.relpath(os.path.join(dirpath, d), src) not in skip)
        out = os.path.join(dst, os.path.relpath(dirpath, src))
        os.makedirs(out, exist_ok=True)
        for fn in sorted(filenames):
            s = os.path.join(dirpath, fn)
            p = file(s)
            d = os.path.join(out, fn)
            if not (os.path.exists(d) and os.path.getsize(d) == os.path.getsize(p)
                    and os.path.getmtime(d) >= os.path.getmtime(p)):
                shutil.copy2(p, d)
            n += 1
            r += p != s
    return n, r


def report(data):
    game = os.path.dirname(os.path.abspath(data))
    root = mod_root(game) if enabled() else None
    if not root:
        print('no Enhanced Edition folder (%s) next to %s' % (os.environ.get('SH2EE_DIR') or 'sh2e', data))
        return
    count = {}
    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            rel = os.path.relpath(os.path.join(dirpath, fn), root).replace(os.sep, '/')
            orig = _find_ci(data, rel)
            key = category(rel) + (' (skipped)' if _skipped(rel) else '')
            c = count.setdefault(key, [0, 0])
            c[0 if orig else 1] += 1
    print('Enhanced Edition folder %s:' % root)
    for k in sorted(count):
        print('  %-22s %5d replacements, %4d files the game has no original of (not used)' % (k, *count[k]))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'file' and len(sys.argv) == 3:
        print(file(sys.argv[2]))
    elif cmd == 'copytree' and len(sys.argv) >= 5:
        n, r = copytree(sys.argv[2], sys.argv[3], sys.argv[4], set(sys.argv[5:]))
        print('%d files, %d from the Enhanced Edition folder' % (n, r))
    elif cmd == 'report' and len(sys.argv) == 3:
        report(sys.argv[2])
    else:
        sys.exit(__doc__)
