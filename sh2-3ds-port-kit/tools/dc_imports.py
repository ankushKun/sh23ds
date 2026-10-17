#!/usr/bin/env python3
"""Rebuild import names behind SecuROM in the Director's Cut exe.

What SecuROM did to the DC's imports (all observed statically):
  - The original import descriptors are still in the first .idata (0x24a9000),
    with hint/name tables. Their IAT slots (0x24a9xxx) are named from those.
    MSVC code that loads an import into a register (mov edi,[__imp_X]; call edi)
    still reads these slots.
  - Most direct `call [__imp_X]` sites were rewritten to `call [0x24dc3f0]`, one
    slot in .cms_d that points at a SecuROM dispatcher. The dispatcher picks the
    API from the return address, so the import has to be named per call site.
  - Some CRT/D3D/DSound imports go through small stubs around 0x56e300..0x56ed06
    that game code reaches with `call rel32`.

Names come from v1.0 (same source, plain imports):
  pass 1  matched function pairs (v10_to_dc_functions.tsv) are byte-identical apart
          from addresses, so the v1.0 import site and the DC site sit at the same
          offset from the function entry.
  pass 2  unmatched functions between two matched anchors are paired by order when
          both builds have the same number of them; their import-call sequences are
          aligned 1:1 and accepted only if every already-known site agrees.

usage: dc_imports.py <dc sh2pc.exe> <kit data dir> <out dir>
writes  <out>/dc_import_sites.tsv   one row per call site
        <out>/dc_import_thunks.tsv  SecuROM stubs + jmp [original IAT] thunks, with exact names
        <out>/dc_import_labels.json thunks/stubs as functions + original IAT slots (ApplyLabels.java)
Then in Ghidra: ApplyLabels.java dc_import_labels.json, ApplyImportSites.java sites.tsv thunks.tsv
"""
import sys, struct, json, bisect, collections
import pefile, capstone

DISPATCH = 0x24dc3f0
STUB_LO, STUB_HI = 0x56e000, 0x571000  # two SecuROM stub tables: ~0x56e300 and ~0x5707c0

exe, data, out = sys.argv[1:4]
pe = pefile.PE(exe)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
X86_MEM, X86_IMM = capstone.x86.X86_OP_MEM, capstone.x86.X86_OP_IMM


def cstr(rva):
    return img[rva:img.index(b'\0', rva)].decode('latin1')


def ident(s):
    return ''.join(ch if ch.isalnum() or ch == '_' else '_' for ch in s).strip('_')


# Original import descriptors -> IAT slot names, keyed by slot address.
idata0 = next(base + s.VirtualAddress for s in pe.sections if s.Name.rstrip(b'\0') == b'.idata')
iat = {}
off = idata0 - base
while True:
    oft, _, _, dname, ft = struct.unpack_from('<5I', img, off)
    if not (oft or dname or ft):
        break
    dll = cstr(dname).upper()
    i = 0
    while (v := struct.unpack_from('<I', img, (oft or ft) + 4 * i)[0]):
        fn = '#%d' % (v & 0xffff) if v & 0x80000000 else cstr(v + 2)
        if dll == 'DSOUND.DLL' and fn == '#11':
            fn = 'DirectSoundCreate'  # dsound.dll ordinal 11
        iat[base + ft + 4 * i] = f'{dll}!{fn}'
        i += 1
    off += 20


def norm(name):
    """v1.0 Ghidra names -> DLL!symbol in the same spelling as the descriptors where possible."""
    dll, fn = name.split('!', 1)
    if dll.upper() == 'DSOUND.DLL' and fn in ('Ordinal_11', '#11'):
        fn = 'DirectSoundCreate'
    return f'{dll.upper() if dll.upper().endswith(".DLL") else dll}!{fn}'


def load_funcs(path):
    out = []
    for line in open(path):
        p = line.rstrip('\n').split('\t')
        if p[0] != 'entry' and p[6] != 'true':
            out.append((int(p[0], 16), int(p[2])))
    return sorted(out)


def classify(ins):
    """Import-call kinds in DC code: dispatch, stub, orig (named IAT slot), reg."""
    if ins.mnemonic not in ('call', 'jmp'):
        return None, None
    op = ins.operands[0]
    if op.type == X86_MEM and op.mem.base == 0 and op.mem.index == 0:
        a = op.mem.disp & 0xffffffff
        if a == DISPATCH:
            return 'dispatch', None
        if a in iat:
            return 'orig', iat[a]
        return None, None
    if op.type == X86_IMM and STUB_LO <= (op.imm & 0xffffffff) < STUB_HI:
        return 'stub', op.imm & 0xffffffff
    if ins.mnemonic == 'call' and op.type == capstone.x86.X86_OP_REG:
        return 'reg', None
    return None, None


def decode_at(a):
    o = a - base
    return next(md.disasm(bytes(img[o:o + 16]), a), None)


def import_calls(entry, size):
    """Linear sweep of one DC function; returns [(addr, kind, extra)]."""
    o = entry - base
    res = []
    for ins in md.disasm(bytes(img[o:o + size]), entry):
        k, x = classify(ins)
        if k:
            res.append((ins.address, k, x))
    return res


dc_funcs = load_funcs(f'{data}/dc/ghidra_functions.tsv')
v1_funcs = load_funcs(f'{data}/v10/ghidra_functions.tsv')
dc_size = dict(dc_funcs)
fmap = {}
for line in open(f'{data}/v10_to_dc_functions.tsv'):
    a, b, _ = line.split('\t')
    if a != 'a_entry':
        fmap[int(a, 16)] = int(b, 16)

v1_sites = collections.defaultdict(list)  # v1 function -> [(site, import)]
for line in open(f'{data}/v10/ghidra_import_xrefs.tsv'):
    imp, site, fn = line.rstrip('\n').split('\t')
    if fn != '?':
        v1_sites[int(fn, 16)].append((int(site, 16), norm(imp)))
for v in v1_sites.values():
    v.sort()

# Every dispatcher call in .text, with its containing Ghidra function (if any).
dc_entries = [e for e, _ in dc_funcs]


def containing(a):
    i = bisect.bisect_right(dc_entries, a) - 1
    if i >= 0 and a < dc_entries[i] + dc_size[dc_entries[i]]:
        return dc_entries[i]
    return None


all_dispatch = []
t_lo, t_hi = 0x401000, 0x629000
for o in range(t_lo - base, t_hi - base - 6):
    if img[o] == 0xFF and img[o + 1] in (0x15, 0x25) and struct.unpack_from('<I', img, o + 2)[0] == DISPATCH:
        all_dispatch.append(base + o)

site_name = {}   # dc site -> (import, method)
stub_votes = collections.defaultdict(collections.Counter)

# Pass 1: matched pairs, same offset.
for v1f, sites in v1_sites.items():
    dcf = fmap.get(v1f)
    if dcf is None:
        continue
    for s, imp in sites:
        ins = decode_at(dcf + (s - v1f))
        k, x = classify(ins) if ins else (None, None)
        if k == 'dispatch':
            site_name[ins.address] = (imp, 'pass1_same_offset')
        elif k == 'stub':
            stub_votes[x][imp] += 1

stubs = {a: c.most_common(1)[0][0] for a, c in stub_votes.items()}

# Pass 2: order-paired unmatched functions between matched anchors.
v1_entries = [e for e, _ in v1_funcs]
anchors = sorted(fmap.items())  # (v1, dc), monotonic in both builds?
pass2_pairs = pass2_named = 0
prev_v1, prev_dc = 0, 0
for v1a, dca in anchors + [(1 << 32, 1 << 32)]:
    if dca < prev_dc:  # not monotonic here; skip this gap
        prev_v1, prev_dc = v1a, dca
        continue
    gap_v1 = [e for e in v1_entries[bisect.bisect_right(v1_entries, prev_v1):bisect.bisect_left(v1_entries, v1a)] if e not in fmap]
    gap_dc = [e for e in dc_entries[bisect.bisect_right(dc_entries, prev_dc):bisect.bisect_left(dc_entries, dca)]
              if e < t_hi and e not in set()]
    gap_dc = [e for e in gap_dc if e not in fmap.values()]
    if len(gap_v1) == len(gap_dc):
        for e1, e2 in zip(gap_v1, gap_dc):
            seq_dc = import_calls(e2, dc_size[e2])
            seq_v1 = v1_sites.get(e1, [])
            if not seq_dc or len(seq_dc) != len(seq_v1):
                continue
            ok = True
            for (a, k, x), (_, imp) in zip(seq_dc, seq_v1):
                if k == 'orig' and x != imp:
                    ok = False
                if k == 'stub' and stubs.get(x) not in (None, imp):
                    ok = False
            if not ok:
                continue
            pass2_pairs += 1
            for (a, k, x), (_, imp) in zip(seq_dc, seq_v1):
                if k == 'dispatch' and a not in site_name:
                    site_name[a] = (imp, 'pass2_order_aligned')
                    pass2_named += 1
    prev_v1, prev_dc = v1a, dca

# Pass 3: orphan code (outside any Ghidra function in either build). v1.0 lists those
# import sites with function '?'. Measure each site from the nearest matched anchor
# at or below it; accept when v1.0 has an import site at exactly the same distance.
v1_orphan = {}
for line in open(f'{data}/v10/ghidra_import_xrefs.tsv'):
    imp, site, fn = line.rstrip('\n').split('\t')
    if fn == '?':
        v1_orphan[int(site, 16)] = norm(imp)
v1_any = dict(v1_orphan)
for v1f, sites in v1_sites.items():
    for s, imp in sites:
        v1_any[s] = imp
anc_dc = sorted((dc, v1) for v1, dc in fmap.items())
anc_keys = [d for d, _ in anc_dc]


def anchor_guess(a):
    i = bisect.bisect_right(anc_keys, a) - 1
    if i < 0:
        return None
    dc, v1 = anc_dc[i]
    return v1_any.get(v1 + (a - dc))


calib_ok = calib_bad = 0
for a, (imp, how) in site_name.items():
    g = anchor_guess(a)
    if g is not None:
        calib_ok += g == imp
        calib_bad += g != imp
pass3_named = 0
for a in all_dispatch:
    if a not in site_name and (g := anchor_guess(a)):
        site_name[a] = (g, 'pass3_anchor_offset')
        pass3_named += 1

# Stub call sites (named through the stub) and orig-slot sites, for the per-site table.
rows = []
for a in all_dispatch:
    imp, how = site_name.get(a, ('', 'unresolved'))
    rows.append((a, containing(a), 'dispatch', imp, how))
for o in range(t_lo - base, t_hi - base - 5):
    if img[o] == 0xE8:
        tgt = (base + o + 5 + struct.unpack_from('<i', img, o + 1)[0]) & 0xffffffff
        if tgt in stubs:
            rows.append((base + o, containing(base + o), 'stub', stubs[tgt], f'via stub {tgt:#x}'))
rows.sort()

with open(f'{out}/dc_import_sites.tsv', 'w') as f:
    f.write('site\tfunction\tkind\timport\tmethod\n')
    for a, fn, k, imp, how in rows:
        f.write(f'{a:#x}\t{fn and hex(fn) or "?"}\t{k}\t{imp}\t{how}\n')

# MSVC import thunks (jmp [__imp_X]) that read the original IAT, e.g. 0x43d52a -> DirectInput8Create.
jmp_thunks = {}
for o in range(t_lo - base, t_hi - base - 6):
    if img[o] == 0xFF and img[o + 1] == 0x25:
        slot = struct.unpack_from('<I', img, o + 2)[0]
        if slot in iat:
            jmp_thunks[base + o] = iat[slot]
with open(f'{out}/dc_import_thunks.tsv', 'w') as f:
    f.write('addr\tkind\timport\n')
    for a, imp in sorted(stubs.items()):
        f.write(f'{a:#x}\tstub\t{imp}\n')
    for a, imp in sorted(jmp_thunks.items()):
        f.write(f'{a:#x}\tjmp_thunk\t{imp}\n')

labels = []
for a, imp in sorted(jmp_thunks.items()):
    labels.append({'addr': hex(a), 'name': 'Imp_' + ident(imp.split('!', 1)[1]),
                   'source': f'jmp [original IAT] thunk -> {imp}'})
for a, imp in sorted(stubs.items()):
    labels.append({'addr': hex(a), 'name': 'stub_' + ident(imp.split('!', 1)[1]),
                   'source': f'SecuROM stub, {sum(stub_votes[a].values())} aligned v1.0 sites -> {imp}'})
for a, imp in sorted(iat.items()):
    labels.append({'addr': hex(a), 'name': '__imp_' + ident(imp.split('!', 1)[1]),
                   'source': f'original import descriptor -> {imp}'})
json.dump({'version': 'dc', 'md5': 'f3f5360298b6d0f6f2e09cf24a0925d1', 'globals': labels},
          open(f'{out}/dc_import_labels.json', 'w'), indent=1)

conflicts = {a: c for a, c in stub_votes.items() if len(c) > 1}
d = [r for r in rows if r[2] == 'dispatch']
print(f'original IAT slots named: {len(iat)}  jmp thunks: {len(jmp_thunks)}')
print(f'stubs named: {len(stubs)} (conflicting votes: {len(conflicts)})')
print(f'dispatcher sites: {len(d)}  pass1: {sum(r[4]=="pass1_same_offset" for r in d)}  '
      f'pass2: {pass2_named} from {pass2_pairs} order-paired functions  '
      f'pass3: {pass3_named} (calibration on known sites: {calib_ok} agree, {calib_bad} disagree)  '
      f'unresolved: {sum(r[4]=="unresolved" for r in d)}  outside functions: {sum(r[1] is None for r in d)}')
print(f'distinct imports behind the dispatcher: {len({r[3] for r in d if r[3]})}')
