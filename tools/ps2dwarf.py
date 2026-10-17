#!/usr/bin/env python3
"""Index the dwarf2cpp output of the SH2 PS2 beta into a function/global/type database.

  ps2dwarf.py <dwarf2cpp src dir> <out.json>
  e.g. ps2dwarf.py ".../SLUS_202_28/work/sh2(CVS全取得)/src" sh2-3ds-port-kit/data/ps2/ps2_functions.json

For each function: file (relative to src), name, signature, PS2 start/end address, the source line
range from its line table, and its local variable declarations. Also lists file-scope globals and
struct names per file. The dump is the reference for real names when mapping PC functions.
"""
import json
import os
import re
import sys

FUNC_START = re.compile(r'^// Start address: (0x[0-9a-f]+)$')
LINE = re.compile(r'^\t// Line (\d+), Address: (0x[0-9a-f]+)')
END = re.compile(r'^\t// Func End, Address: (0x[0-9a-f]+)')
STRUCT = re.compile(r'^struct (\w+)$')
GLOBAL = re.compile(r'^([A-Za-z_][\w \*]*?[\w\*])\s*\**(\w+)(\[[^\]]*\])*;$')


def parse_file(path, rel):
    funcs, globals_, structs = [], [], []
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = FUNC_START.match(ln)
        if m and i + 1 < len(lines):
            sig = lines[i + 1].strip()
            name_m = re.search(r'(\w+)\s*\(', sig)
            f = dict(file=rel, name=name_m.group(1) if name_m else sig, signature=sig,
                     start=m.group(1), end=None, lines=[], locals=[])
            j = i + 3          # skip signature and '{'
            while j < len(lines) and lines[j] != '}':
                lm, em = LINE.match(lines[j]), END.match(lines[j])
                if lm:
                    f['lines'].append(int(lm.group(1)))
                elif em:
                    f['end'] = em.group(1)
                elif lines[j].startswith('\t') and not lines[j].strip().startswith('//'):
                    f['locals'].append(lines[j].strip())
                j += 1
            if f['lines']:
                f['line_min'], f['line_max'] = min(f['lines']), max(f['lines'])
            del f['lines']
            funcs.append(f)
            i = j + 1
            continue
        sm = STRUCT.match(ln)
        if sm:
            structs.append(sm.group(1))
        elif not ln.startswith(('typedef', '//', '\t', ' ', '{', '}', '#')) and ln.endswith(';') and '(' not in ln:
            gm = GLOBAL.match(ln)
            if gm:
                globals_.append(ln)
        i += 1
    return funcs, globals_, structs


def main():
    src, out = sys.argv[1:3]
    db = dict(functions=[], globals={}, structs={})
    for root, _, files in os.walk(src):
        for fn in sorted(files):
            if not fn.endswith(('.c', '.cpp', '.h')):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, src).replace(os.sep, '/')
            f, g, s = parse_file(path, rel)
            db['functions'] += f
            if g:
                db['globals'][rel] = g
            if s:
                db['structs'][rel] = sorted(set(s))
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    json.dump(db, open(out, 'w'), indent=0)
    print(f"{len(db['functions'])} functions, {sum(len(v) for v in db['globals'].values())} globals, "
          f"{len(db['structs'])} files with structs")


if __name__ == '__main__':
    main()
