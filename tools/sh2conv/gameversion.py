#!/usr/bin/env python3
"""Which Silent Hill 2 PC release a game folder holds, and what to tell the player when it isn't the
one sh23ds is built from.

  gameversion.py <Silent Hill 2 folder>             report on the exe and the data folder
  gameversion.py --manifest <Silent Hill 2 folder>  rewrite game_files.tsv from a Director's Cut install

The converters read tables at fixed addresses of the Director's Cut sh2pc.exe (MD5 DC_MD5). Any exe
that holds the same bytes everywhere they read makes the same pack, so a Director's Cut exe that a
no-CD patch, a fix or a rip changed elsewhere is used as it is (pcexe.py, exe_reads.tsv). Any other exe
is named by its MD5 where we know the file, and otherwise by the code bytes at 0x401005 that Silent
Hill 2 Enhancements tells v1.0, v1.1 and the Director's Cut apart by (refsrc/sh2ee dllmain.cpp
GetGameVersion; sh2-3ds-port-kit/tools/sh2version.py does the same). Installers of the Enhanced
Edition and of fixes may keep the replaced exe next to the new one, so when sh2pc.exe can't be used, a
usable exe under another name in the folder or a backup folder (sh2pc.bak, backup/sh2pc.exe) is.

The data folder is compared with the file sizes of a Director's Cut install
(sh2-3ds-port-kit/data/dc/game_files.tsv, sizes only). That catches the case the exe check can't: the
right exe dropped onto another release's data, or a repack that shrank or removed files.
"""
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pcexe  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
MANIFEST = os.path.join(ROOT, 'sh2-3ds-port-kit', 'data', 'dc', 'game_files.tsv')

DC_MD5 = 'f3f5360298b6d0f6f2e09cf24a0925d1'
WANTED = ("Silent Hill 2: Director's Cut for PC, the 2002 release on two CDs (some stores list it as "
          "Restless Dreams). Its sh2pc.exe has MD5 " + DC_MD5 + ", or is that file changed only in places the "
          "packer doesn't read")

# exes we know by hash, and where that is written down
KNOWN = {
    DC_MD5: 'the Director\'s Cut',                                 # docs/RESEARCH.md
    '37caa31bdb473d9019be2491bd8dc914': 'version 1.0',             # docs/RESEARCH.md, data/v10/
    'c16ed8436fdcfc53a07ab21723994770': 'version 1.0, North American',      # sh2ee Resources/sh2pc.txt
    'f9ccf14b5e5a31d9fe50e5b5efe50ed7': 'ee',                      # the same file after sh2ee's sh2pc.ips
}
# 20 bytes at 0x401005, a table of jumps whose targets moved between builds (sh2version.py)
SIGNATURES = {
    bytes.fromhex('e956250000e971250000e9fc690000e977060000'): 'v10',
    bytes.fromhex('e956250000e971250000e99c6a0000e977060000'): 'v11',
    bytes.fromhex('e966250000e981250000e9ac6a0000e977060000'): 'dc',
}
FAMILY = {'v10': 'version 1.0 (the 2001 release)', 'v11': 'version 1.1 (the 2001 release, patched)',
          'dc': "the Director's Cut"}


def md5_of(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def family(path):
    """'v10', 'v11', 'dc', 'other' for a Windows program we can't place, or None for a file that
    isn't one. Reads the PE headers by hand, so it needs no pefile."""
    with open(path, 'rb') as f:
        d = f.read(64 << 20)
    try:
        if d[:2] != b'MZ':
            return None
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            return None
        nsec, = struct.unpack_from('<H', d, pe + 6)
        optsize, = struct.unpack_from('<H', d, pe + 20)
        base, = struct.unpack_from('<I', d, pe + 24 + 28)
        rva = 0x401005 - base
        for i in range(nsec):
            s = pe + 24 + optsize + 40 * i
            vsize, va, rsize, raw = struct.unpack_from('<IIII', d, s + 8)
            if va <= rva and rva + 20 <= va + min(vsize or rsize, rsize):
                return SIGNATURES.get(d[raw + rva - va:raw + rva - va + 20], 'other')
    except struct.error:
        return None
    return 'other'


def describe(path, md5=None):
    """A few words on what this exe is, for the messages."""
    md5 = md5 or md5_of(path)
    known = KNOWN.get(md5)
    if known == 'ee':
        return "the Enhanced Edition's own exe (a patched North American 1.0)"
    if known:
        return known
    fam = family(path)
    if fam in FAMILY:
        return "a Director's Cut exe that a patch or a mod changed" if fam == 'dc' else FAMILY[fam]
    return 'not a Windows program' if fam is None else 'a build sh23ds doesn\'t know'


def other_exes(game):
    """Files that may be the exe an installer or a fix replaced: next to sh2pc.exe, sh2pc.bak,
    sh2pc.exe.orig, sh2pc_original.exe, any other .exe and so on; in a subfolder named like a backup
    (backup, _bak, Original), the same. Sizes are bounded so nothing big gets hashed."""
    def candidates(d):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return []
        out = []
        for name in names:
            low, p = name.lower(), os.path.join(d, name)
            if (low != 'sh2pc.exe' or d != game) and (low.startswith('sh2pc') or
                                                      low.endswith(('.exe', '.bak', '.orig', '.old'))):
                if os.path.isfile(p) and 1 << 20 <= os.path.getsize(p) <= 16 << 20:
                    out.append(p)
        return out
    out = candidates(game)
    for name in sorted(os.listdir(game)):
        p = os.path.join(game, name)
        if os.path.isdir(p) and any(k in name.lower() for k in ('backup', 'bak', 'orig', 'old')):
            out += candidates(p)
    return out


def ee_installed(game):
    """The Enhanced Edition's module or its mod folder is in the game folder."""
    names = {n.lower() for n in os.listdir(game)}
    return 'sh2eeconfig.exe' in names or ('d3d8.dll' in names and 'd3d8.ini' in names) or 'sh2e' in names


def usable(path, md5=None):
    """None when the packer can read this exe, else a few words on why not."""
    if (md5 or md5_of(path)) == DC_MD5:
        return None
    if family(path) in ('v10', 'v11', None):
        return 'not the Director\'s Cut'
    ok, why = pcexe.matches(path)
    return None if ok else why


def check_exe(game):
    """(exe, notes, problem): the exe to read, lines to show the player, and the message that stops
    the packer (None when it can go on)."""
    exe = os.path.join(game, 'sh2pc.exe')
    md5 = md5_of(exe)
    if md5 == DC_MD5:
        return exe, ["sh2pc.exe: the Director's Cut (MD5 %s)" % md5], None
    differs = usable(exe, md5)
    if differs is None:
        return exe, ["sh2pc.exe: a Director's Cut exe that something changed (MD5 %s). Everything the packer "
                     "reads from it is the same as in the original, so it's used as it is." % md5], None
    for alt in other_exes(game):
        if usable(alt) is None:
            return alt, ["sh2pc.exe is %s, but %s is a Director's Cut exe the packer can read, so it uses that "
                         "one" % (describe(exe, md5), os.path.relpath(alt, game))], None

    known, fam = KNOWN.get(md5), family(exe)
    size = os.path.getsize(exe)
    if known == 'ee' or (fam in ('v10', 'v11', 'other') and ee_installed(game)):
        why = ("sh2pc.exe is the Enhanced Edition's own exe. Installing Silent Hill 2 Enhancements replaces "
               "the game's exe with a patched North American 1.0, which sh23ds can't read. Put the "
               "Director's Cut sh2pc.exe back while you pack, or next to this one as sh2pc.bak; the EE "
               "packs in the sh2e folder still get used.")
    elif fam in ('v10', 'v11') or (known and known != KNOWN[DC_MD5]):
        why = ("sh2pc.exe is Silent Hill 2 PC %s. sh23ds reads its tables out of the Director's Cut exe, "
               "where they sit at other addresses, so this release can't be used."
               % (known or FAMILY[fam]))
    elif fam == 'dc':
        why = ("sh2pc.exe is a Director's Cut exe that a patch or mod changed, and the change touches what "
               "the packer reads for %s. Put back the exe as the CDs install it, or leave that one next to "
               "this as sh2pc.bak and the packer finds it." % differs)
    elif fam is None:
        why = ("sh2pc.exe isn't a Windows program (%d bytes). It may be a shortcut or a placeholder left by "
               "a launcher; the packer needs the game's real exe." % size)
    else:
        why = ("sh2pc.exe is a build sh23ds doesn't know. If you think it is the Director's Cut, open an "
               "issue on GitHub with this MD5.")
    return None, [], '%s\n\nYou need %s.\nYour sh2pc.exe: MD5 %s, %d bytes.' % (why, WANTED, md5, size)


def read_manifest(path=MANIFEST):
    """{path under data/, lower case: size} of a Director's Cut install."""
    sizes = {}
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        rel, size = line.rstrip('\n').split('\t')
        sizes[rel.lower()] = int(size)
    return sizes


def scan(data):
    """{path under data/, lower case: size} of the files in a data folder."""
    have = {}
    for dirpath, dirnames, filenames in os.walk(data, followlinks=True):
        dirnames[:] = [d for d in dirnames if not d.startswith('.')]
        for fn in filenames:
            if fn.startswith('.'):
                continue
            p = os.path.join(dirpath, fn)
            have[os.path.relpath(p, data).replace(os.sep, '/').lower()] = os.path.getsize(p)
    return have


def check_data(data):
    """(notes, problem) for the data folder against the Director's Cut's. Files that are missing or
    have another size are reported by folder; the pack only stops when so much is missing that this
    can't be the game's data folder."""
    want, have = read_manifest(), scan(data)
    missing = sorted(r for r in want if r not in have)
    changed = sorted(r for r in want if r in have and have[r] != want[r])
    if not missing and not changed:
        return ["data: matches the Director's Cut (%d files)" % len(want)], None

    by_folder = {}
    for kind, rels in (('missing', missing), ('different', changed)):
        for r in rels:
            top = r.split('/')[0] if '/' in r else '.'
            by_folder.setdefault(top, {}).setdefault(kind, []).append(r)
    folders = ', '.join('%s (%s)' % (top, ', '.join('%d %s' % (len(v), k) for k, v in sorted(kinds.items())))
                        for top, kinds in sorted(by_folder.items()))
    examples = (missing + changed)[:6]
    notes = ['data: of the Director\'s Cut\'s %d files, %d %s missing and %d %s a different size, in %s. '
             'For example: %s' % (len(want), len(missing), 'is' if len(missing) == 1 else 'are', len(changed),
                                  'has' if len(changed) == 1 else 'have', folders, ', '.join(examples))]
    if 'movie' in by_folder:
        notes.append('Movies that are missing or different were most likely removed or shrunk by a repack, or '
                     'replaced by an older Enhanced Edition FMV pack. Each one is converted if ffmpeg can read it '
                     'and left out if not; the game plays on without it.')
    if [r for r in missing + changed if not r.startswith('movie/')]:
        notes.append('Other files that are missing or different may be missing or broken on the 3DS too, or '
                     'stop a step of the packer. A folder installed from the Director\'s Cut CDs avoids this.')
    if len(missing) > len(want) // 4:
        return notes, ("The data folder holds only %d of the Director's Cut's %d files. Either it is another "
                       "release's data or the folder isn't the game's.\n\nYou need %s."
                       % (len(want) - len(missing), len(want), WANTED))
    return notes, None


def check(game):
    """(exe, notes, problem) for a game folder: check_exe, then check_data when the exe is fine."""
    exe, notes, problem = check_exe(game)
    if problem:
        return exe, notes, problem
    dnotes, problem = check_data(os.path.join(game, 'data'))
    return exe, notes + dnotes, problem


def write_manifest(game):
    if md5_of(os.path.join(game, 'sh2pc.exe')) != DC_MD5:
        sys.exit("%s isn't a Director's Cut install" % game)
    have = scan(os.path.join(game, 'data'))
    with open(MANIFEST, 'w', newline='\n') as f:
        f.write("# Director's Cut data folder: every file (lower case, under data/) and its size in bytes.\n"
                "# Written by tools/sh2conv/gameversion.py --manifest; the packer compares a player's data with it.\n")
        for rel in sorted(have):
            f.write('%s\t%d\n' % (rel, have[rel]))
    print('%s: %d files' % (os.path.relpath(MANIFEST, ROOT), len(have)))


def main():
    args = sys.argv[1:]
    if args[:1] == ['--manifest'] and len(args) == 2:
        return write_manifest(args[1])
    if len(args) != 1:
        sys.exit(__doc__)
    exe, notes, problem = check(args[0])
    for n in notes:
        print(n)
    if problem:
        sys.exit(problem)


if __name__ == '__main__':
    main()
