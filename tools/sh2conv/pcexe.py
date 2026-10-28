#!/usr/bin/env python3
"""sh2pc.exe as the converters read it, and the check that lets the packer take an exe that a no-CD
patch, a fix or a rip changed.

Every converter reads the exe through Exe: by virtual address through the section table (read, u32,
cstr), or by scanning the code for calls to a function or for a byte pattern (calls_to, find_all).
So this one file sees everything a pack is made from in the exe.

sh2-3ds-port-kit/data/dc/exe_reads.tsv lists all of it: every range of bytes the converters read and
every scan they run, and what they returned from the Director's Cut exe (DC_MD5). An exe with another
MD5 makes the same pack when each of those returns the same, wherever else it differs. `trace` writes
the list by running every step that reads the exe on the Director's Cut.

The ranges are compared in groups of at least GROUP_MIN bytes, one digest each over the group's bytes
in address order, so that no digest covers so few bytes that they could be found from it: the table
holds addresses, sizes and digests, never anything of the exe itself. A group keeps to one converter
(or one set of converters, for bytes several read) where it can, so a difference is still named by
the part it touches. A scan's digest is over the addresses it found.

  - matches(path) answers that for gameversion.py, and names the parts that differ when it doesn't.
  - Exe on an exe that isn't the Director's Cut by MD5 checks the same as it opens, and stops a
    converter that reads a byte or runs a scan the list doesn't have (a path through the converters
    the list wasn't made from), so no pack is made from bytes that weren't compared.

  pcexe.py check <sh2pc.exe>                    whether it matches the Director's Cut where read
  pcexe.py trace <Silent Hill 2 folder> <out>   rewrites exe_reads.tsv (the folder must hold the
                                                Director's Cut; out is a scratch folder for the steps)
"""
import bisect
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
TABLE = os.path.join(ROOT, 'sh2-3ds-port-kit', 'data', 'dc', 'exe_reads.tsv')
DC_MD5 = 'f3f5360298b6d0f6f2e09cf24a0925d1'
GROUP_MIN = 32      # bytes under one digest, at least

# the pack.py steps that pass the exe to their converters, which trace runs
STEPS = ('events', 'lighting', 'fog', 'ui', 'pics', 'maps', 'sound', 'demos', 'stagemodels')
# what each converter reads from the exe, for the messages
WHAT = {'events.py': 'stage events', 'lighting.py': 'room lighting', 'fog.py': 'fog',
        'ui.py': 'the font', 'pics.py': 'puzzle pictures', 'maps.py': 'maps and memos',
        'sound.py': 'sound effects and music', 'demos.py': 'cutscenes', 'motion.py': 'motions',
        'stagemodels.py': 'stage models'}

_trace = None       # trace(): {('range', va, n) | (kind, arg): set of converters}


class ExeChanged(RuntimeError):
    """The exe differs from the Director's Cut where a converter reads it."""


def _who():
    """The converter running: pack.py runs each as its own __main__ (runpy), with its own argv."""
    return os.path.basename(sys.argv[0]) if sys.argv and sys.argv[0] else '?'


def _digest(b):
    return hashlib.md5(b).hexdigest()[:16]


class Exe:
    def __init__(self, path):
        with open(path, 'rb') as f:
            d = f.read()
        self.path, self.d = path, d
        try:
            pe = struct.unpack_from('<I', d, 0x3c)[0]
            if d[:2] != b'MZ' or d[pe:pe + 4] != b'PE\0\0':
                raise ExeChanged('%s is not a Windows program' % path)
            nsec, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
            self.base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
            self.secs = []
            for i in range(nsec):
                vsize, va, rsize, raw = struct.unpack_from('<4I', d, pe + 24 + opt + 40 * i + 8)
                self.secs.append((self.base + va, max(vsize, rsize), raw, rsize))
        except struct.error:
            raise ExeChanged('%s is not a Windows program' % path)
        if not self.secs:
            raise ExeChanged('%s has no sections' % path)
        self.md5 = hashlib.md5(d).hexdigest()
        self._calls = None
        self.guard = None
        if self.md5 != DC_MD5:
            self.guard = _Guard(self)

    # ------------------------------------------------------------------ reads by address
    def _read(self, va, n):
        for base, vsize, raw, rsize in self.secs:
            if base <= va < base + vsize:
                o = va - base
                out = self.d[raw + o: raw + min(o + n, rsize)] if o < rsize else b''
                return out + b'\0' * (n - len(out))
        raise ValueError('address %#x not mapped' % va)

    def read(self, va, n):
        out = self._read(va, n)
        if _trace is not None:
            _trace.setdefault(('range', va, n), set()).add(_who())
        elif self.guard and n:
            self.guard.covered(va, n)
        return out

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def cstr(self, va, n=64):
        return self.read(va, n).split(b'\0')[0].decode('latin-1')

    # ------------------------------------------------------------------ scans of the code section
    def code(self):
        """(address, bytes) of the first section, the code."""
        base, _, raw, rsize = self.secs[0]
        return base, self.d[raw:raw + rsize]

    def _scan(self, kind, arg, result):
        if _trace is not None:
            _trace.setdefault((kind, arg), set()).add(_who())
        elif self.guard:
            self.guard.scanned(kind, arg, result)
        return result

    def _all_calls(self):
        """{target: [addresses of `call rel32` to it]} over the code section."""
        if self._calls is None:
            base, code = self.code()
            self._calls = {}
            o = code.find(b'\xe8', 5)
            while 0 <= o <= len(code) - 5:
                t = base + o + 5 + struct.unpack_from('<i', code, o + 1)[0]
                self._calls.setdefault(t, []).append(base + o)
                o = code.find(b'\xe8', o + 1)
        return self._calls

    def calls_to(self, target):
        """Addresses of every `call target` (e8 rel32) in the code, in order."""
        return self._scan('calls', '%#x' % target, list(self._all_calls().get(target, ())))

    def find_all(self, pattern):
        """Addresses of every occurrence of pattern in the code, overlapping ones included."""
        base, code = self.code()
        out, i = [], code.find(pattern)
        while i >= 0:
            out.append(base + i)
            i = code.find(pattern, i + 1)
        return self._scan('find', pattern.hex(), out)


def _scan_digest(exe, kind, arg):
    """The digest of a scan's result, the way exe_reads.tsv stores it."""
    if kind == 'calls':
        res = exe._all_calls().get(int(arg, 16), [])
    else:
        base, code = exe.code()
        pat, res, i = bytes.fromhex(arg), [], None
        i = code.find(pat)
        while i >= 0:
            res.append(base + i)
            i = code.find(pat, i + 1)
    return _digest(repr(res).encode())


# ------------------------------------------------------------------ the list and the comparison
def load_table(path=TABLE):
    """[(kind, key, ranges, md5, converters)] from exe_reads.tsv: ('group', id, [(address, size)...],
    md5, converters) for the reads, (kind, arg, [], md5, converters) for the scans."""
    rows, members = [], {}
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        f = line.rstrip('\n').split('\t')
        if f[0] == 'range':
            members.setdefault(f[3], []).append((int(f[1], 16), int(f[2])))
        elif f[0] == 'group':
            rows.append(('group', f[1], members.setdefault(f[1], []), f[3], f[4].split(',')))
        else:
            rows.append((f[0], f[1], [], f[3], f[4].split(',')))
    return rows


def _group_digest(exe, ranges):
    try:
        return _digest(b''.join(exe._read(a, n) for a, n in sorted(ranges)))
    except ValueError:
        return None


def differences(exe, rows=None, scans=True):
    """The rows of exe_reads.tsv whose bytes or scan result differ in this exe."""
    out = []
    for row in rows if rows is not None else load_table():
        kind, key, ranges, md5, _ = row
        if kind == 'group':
            got = _group_digest(exe, ranges)
        elif scans:
            got = _scan_digest(exe, kind, key)
        else:
            continue
        if got != md5:
            out.append(row)
    return out


def describe(rows):
    """'the font and cutscenes' for the rows that differ."""
    names = []
    for row in rows:
        for w in row[4]:
            name = WHAT.get(w, w)
            if name not in names:
                names.append(name)
    return names[0] if len(names) == 1 else ', '.join(names[:-1]) + ' and ' + names[-1]


def matches(path):
    """(True, '') when every read of exe_reads.tsv returns what it returns from the Director's Cut,
    else (False, the parts that differ in a few words)."""
    try:
        exe = Exe(path)
    except ExeChanged as e:
        return False, str(e)
    except (OSError, ValueError) as e:
        return False, str(e)
    if exe.md5 == DC_MD5:
        return True, ''
    return (True, '') if exe.guard.ok else (False, describe(exe.guard.bad))


_verified = {}      # md5 -> (rows that differ, ranges, scans), so each worker compares once


class _Guard:
    """The reads exe_reads.tsv allows, for an exe that isn't the Director's Cut by MD5."""

    def __init__(self, exe):
        if exe.md5 not in _verified:
            rows = load_table()
            starts, ends = [], []
            for a, n in sorted(rng for r in rows if r[0] == 'group' for rng in r[2]):
                if starts and a <= ends[-1]:
                    ends[-1] = max(ends[-1], a + n)
                else:
                    starts.append(a)
                    ends.append(a + n)
            scans = {(r[0], r[1]): r[3] for r in rows if r[0] != 'group'}
            # the ranges now; each scan when a converter first runs it (the code scans take a while)
            _verified[exe.md5] = (differences(exe, rows, scans=False), (starts, ends), scans, {})
        self.exe = exe
        self.bad, (self.starts, self.ends), self.scans, self.done = _verified[exe.md5]

    @property
    def ok(self):
        if self.bad:
            return False
        for kind, arg in self.scans:
            self.scanned(kind, arg, None, quiet=True)
        return not self.bad

    def covered(self, va, n):
        self.check()
        i = bisect.bisect_right(self.starts, va) - 1
        if i < 0 or va + n > self.ends[i]:
            raise ExeChanged("sh2pc.exe isn't the Director's Cut's own file, and %s read a part of it that "
                             "the packer has no record of (%#x, %d bytes), so it can't tell whether that part "
                             "matches. Please open an issue on GitHub with this message."
                             % (_who(), va, n))

    def scanned(self, kind, arg, result, quiet=False):
        key = (kind, arg)
        if key not in self.done:
            if key not in self.scans:
                raise ExeChanged("sh2pc.exe isn't the Director's Cut's own file, and %s searched its code in a "
                                 "way the packer has no record of (%s %s). Please open an issue on GitHub with "
                                 "this message." % (_who(), kind, arg))
            self.done[key] = _scan_digest(self.exe, kind, arg) == self.scans[key]
            if not self.done[key]:
                self.bad.append(next(r for r in load_table() if (r[0], r[1]) == key))
        if not quiet:
            self.check()

    def check(self):
        if self.bad:
            raise ExeChanged("sh2pc.exe differs from the Director's Cut's in what the packer reads for %s."
                             % describe(self.bad))


# ------------------------------------------------------------------ writing the list
def trace(game, out):
    """Runs every step that reads the exe on the Director's Cut and writes exe_reads.tsv."""
    global _trace
    sys.path.insert(0, HERE)
    import pack
    p = pack.Packer(game, out)
    p.check()
    exe = Exe(p.exe)
    if exe.md5 != DC_MD5:
        sys.exit("trace needs the Director's Cut's own sh2pc.exe")
    p.begin()
    _trace = {}
    try:
        for sid in STEPS:
            print('== %s' % sid, flush=True)
            p.run_step(sid)
    finally:
        found, _trace = _trace, None
    # the reads, merged into runs of bytes, each with the converters that read any of it
    runs = []
    for (kind, va, n), who in sorted((k, v) for k, v in found.items() if k[0] == 'range'):
        if runs and va <= runs[-1][1]:
            runs[-1][1] = max(runs[-1][1], va + n)
            runs[-1][2] |= who
        else:
            runs.append([va, va + n, set(who)])
    groups = group_runs([(a, e - a, ','.join(sorted(who))) for a, e, who in runs])
    with open(TABLE, 'w', newline='\n') as f:
        f.write("# Everything the converters read from the Director's Cut sh2pc.exe (MD5 %s).\n"
                "# tools/sh2conv/pcexe.py compares another exe with it; `pcexe.py trace` rewrites it whenever a\n"
                "# converter reads the exe differently. It holds addresses, sizes and digests only: each digest\n"
                "# (the first 16 hex digits of an MD5) covers at least %d bytes, too many to find from it.\n"
                "#   range  address  size  group        a run of bytes read by address, compared as part of its group\n"
                "#   group  id       size  md5  converters   the digest of its ranges' bytes, in address order\n"
                "#   calls  target   -     md5  converters   the addresses of every call to target in the code\n"
                "#   find   pattern  -     md5  converters   the addresses of every occurrence of pattern\n"
                % (DC_MD5, GROUP_MIN))
        for gid, (members, who) in enumerate(groups):
            for a, n, _ in members:
                f.write('range\t%#x\t%d\tg%d\n' % (a, n, gid))
            f.write('group\tg%d\t%d\t%s\t%s\n' % (gid, sum(n for _, n, _ in members),
                                                   _group_digest(exe, [(a, n) for a, n, _ in members]), who))
        for (kind, arg), who in sorted((k, v) for k, v in found.items() if k[0] != 'range'):
            f.write('%s\t%s\t-\t%s\t%s\n' % (kind, arg, _scan_digest(exe, kind, arg), ','.join(sorted(who))))
    print('%s: %d ranges (%d bytes) in %d groups, %d scans' % (
        os.path.relpath(TABLE, ROOT), len(runs), sum(e - a for a, e, _ in runs), len(groups),
        sum(1 for k in found if k[0] != 'range')))


def group_runs(runs):
    """[(members, converters)] for runs [(address, size, converters)]: consecutive runs read by the same
    converters, in address order, gathered until they hold GROUP_MIN bytes. A set of converters that
    reads fewer bytes than that in all joins the group of the run nearest it, whoever reads that."""
    by = {}
    for r in sorted(runs):
        by.setdefault(r[2], []).append(r)
    groups, small = [], []
    for who, rs in sorted(by.items()):
        if sum(n for _, n, _ in rs) < GROUP_MIN:
            small += rs
            continue
        cur = []
        for r in rs:
            cur.append(r)
            if sum(n for _, n, _ in cur) >= GROUP_MIN:
                groups.append(cur)
                cur = []
        if cur:
            groups[-1] += cur       # the short tail joins the last group of the same converters
    for r in small:
        g = min(groups, key=lambda g: min(abs(m[0] - r[0]) for m in g))
        g.append(r)
    out = []
    for g in groups:
        g.sort()
        out.append((g, ','.join(sorted({w for _, _, who in g for w in who.split(',')}))))
    return sorted(out)


def main():
    args = sys.argv[1:]
    if args[:1] == ['check'] and len(args) == 2:
        ok, why = matches(args[1])
        print('matches the Director\'s Cut where the converters read it' if ok else 'differs: ' + why)
        sys.exit(0 if ok else 1)
    if args[:1] == ['trace'] and len(args) == 3:
        return trace(args[1], args[2])
    sys.exit(__doc__)


if __name__ == '__main__':
    # run as the module the converters import, so trace() records their reads
    sys.path.insert(0, HERE)
    import pcexe
    pcexe.main()
