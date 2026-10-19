#!/usr/bin/env python3
"""Collect the PC function names the port's comments cite, for ApplyNames.java.

  port_names.py [port/source] [out.tsv]   (default: sh2-3ds-port-kit/data/dc/port_names.tsv)

Port comments name PC code as "0x4a96c0 enDyingExec" or "enDyingExec (0x4a96c0)". Only names the
PS2 beta's debug info knows (sh2-3ds-port-kit/data/ps2/ps2_functions.json) or stage programs
(EvProg*) are kept, and a PS2 name must be camel or snake case ("init" is a PS2 function too), so
English words and our own helper names never reach Ghidra. An address cited
with two different names is left out and printed. Writes addr, name, first place it is cited.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'port/source')
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'sh2-3ds-port-kit/data/dc/port_names.tsv')

ps2 = {f['name'] for f in json.load(open(os.path.join(ROOT, 'sh2-3ds-port-kit/data/ps2/ps2_functions.json')))['functions']}
ADDR = r'0x(?:00)?([4-5][0-9a-f]{5})\b'
NAME = r'([A-Za-z_][A-Za-z0-9_]{2,})'
forms = [re.compile(ADDR + r' \(?' + NAME + r'\b'), re.compile(NAME + r' \(' + ADDR + r'\)')]

seen = {}
for fn in sorted(os.listdir(src)):
    if not fn.endswith(('.c', '.h')):
        continue
    for ln, line in enumerate(open(os.path.join(src, fn), encoding='utf-8', errors='replace'), 1):
        for i, rx in enumerate(forms):
            for m in rx.finditer(line):
                a, n = (m.group(1), m.group(2)) if i == 0 else (m.group(2), m.group(1))
                if (n in ps2 and re.search(r'[A-Z0-9_]', n)) or n.startswith('EvProg'):
                    seen.setdefault(int(a, 16), {}).setdefault(n, f'port/source/{fn}:{ln}')

rows, clash = [], []
for a in sorted(seen):
    if len(seen[a]) > 1:
        clash.append((a, seen[a]))
        continue
    (n, where), = seen[a].items()
    rows.append(f'0x{a:x}\t{n}\t{where}')
with open(out, 'w') as f:
    f.write('addr\tname\tevidence\n' + '\n'.join(rows) + '\n')
print(f'{len(rows)} names -> {os.path.relpath(out, ROOT)}')
for a, ns in clash:
    print(f'left out 0x{a:x}: cited as ' + ', '.join(f'{n} ({w})' for n, w in ns.items()))
