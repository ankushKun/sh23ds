#!/usr/bin/env python3
"""Name PC (Director's Cut) functions after PS2 beta functions using assert line numbers.

PC asserts pass the source path (\\CVSWork\\sh2pc\\src\\<file>) and a line number. The PS2 beta's
DWARF line tables give each function's source line range in the same file. A PC function whose
asserts all fall inside one PS2 function's range gets that function's name.

  map_ps2_names.py <sh2pc.exe> <graph dir> <ps2_functions.json> <out labels.json> <out report.tsv>
"""
import collections
import json
import re
import struct
import sys

import capstone
import pefile

exe, gdir, ps2json, out_json, out_tsv = sys.argv[1:6]
pe = pefile.PE(exe)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

funcs = {}
for line in list(open(f'{gdir}/functions.tsv'))[1:]:
    e, name, size, lo, hi, thunk = line.rstrip('\n').split('\t')
    funcs[int(e, 16)] = (name, int(lo, 16), int(hi, 16))
paths = {}   # function -> {string address: relative path}
SRC = re.compile(r'\\\\CVSWork\\\\sh2pc\\\\src\\\\(.+)$')
for line in list(open(f'{gdir}/strings.tsv'))[1:]:
    f, a, text = line.rstrip('\n').split('\t', 2)
    m = SRC.search(text)
    if m:
        paths.setdefault(int(f, 16), {})[int(a, 16)] = m.group(1).replace('\\\\', '/')

db = json.load(open(ps2json))
by_file = collections.defaultdict(list)
for f in db['functions']:
    if 'line_min' in f:
        by_file[f['file']].append(f)

labels, rows = [], []
for fa, strs in sorted(paths.items()):
    name, lo, hi = funcs[fa]
    ins = list(md.disasm(img[lo - base:hi - base + 1], lo))
    hits = []
    for k, i in enumerate(ins):
        if i.mnemonic != 'push' or not i.op_str.startswith('0x'):
            continue
        v = int(i.op_str, 16)
        if v not in strs:
            continue
        # the line number is the nearest preceding/following small immediate push or mov
        cand = None
        for j in list(range(k - 1, max(k - 6, -1), -1)) + list(range(k + 1, min(k + 4, len(ins)))):
            o = ins[j]
            if o.mnemonic == 'push' and re.fullmatch(r'0x[0-9a-f]+|\d+', o.op_str):
                n = int(o.op_str, 0)
                if 0 < n < 20000 and n not in strs:
                    cand = n
                    break
        if cand:
            hits.append((strs[v], cand))
    owners = set()
    detail = []
    for path, ln in hits:
        match = [f for f in by_file.get(path, []) if f['line_min'] <= ln <= f['line_max']]
        if len(match) == 1:
            owners.add(match[0]['name'])
            detail.append(f'{path}:{ln}->{match[0]["name"]}')
        else:
            detail.append(f'{path}:{ln}->?{len(match)}')
    status = 'ok' if len(owners) == 1 else ('none' if not owners else 'ambiguous')
    rows.append((fa, name, status, ' '.join(sorted(owners)), '; '.join(detail[:6])))
    if status == 'ok':
        labels.append({'addr': hex(fa), 'name': owners.pop(),
                       'source': 'PS2 beta DWARF line range of the assert(s): ' + '; '.join(detail[:3])})

# a PS2 name must be used by one PC function only
count = collections.Counter(l['name'] for l in labels)
labels = [l for l in labels if count[l['name']] == 1]
json.dump({'version': 'dc', 'md5': 'f3f5360298b6d0f6f2e09cf24a0925d1', 'globals': labels}, open(out_json, 'w'), indent=1)
with open(out_tsv, 'w') as f:
    f.write('pc_function\tpc_name\tstatus\tps2_name\tasserts\n')
    for r in rows:
        f.write(f'{r[0]:#x}\t{r[1]}\t{r[2]}\t{r[3]}\t{r[4]}\n')
st = collections.Counter(r[2] for r in rows)
print(f'PC functions with source asserts: {len(rows)}  {dict(st)}  unique names applied: {len(labels)}')
