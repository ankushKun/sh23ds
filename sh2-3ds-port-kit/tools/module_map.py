#!/usr/bin/env python3
"""First-pass module map for the Director's Cut .text, from ExportGraph.java output.

MSVC links each .obj (and each static library member) as one contiguous run of
functions, so a few anchored functions pin a module to an address range.

Anchors
  game/<Folder>/<file>  references a \\CVSWork\\sh2pc\\src\\ assert path
  lib/cri               CRI middleware error strings (E-codes, adxf_/mfCi/wxCi/... names), CRI UT version
  lib/libpng            libpng messages (1.0.5)
  lib/zlib              zlib internal messages (1.1.3)
  lib/libjpeg           IJG messages, the jpeg_std_message_table pointer table (6a), or the
                        ERREXIT/WARNMS pattern: err = cinfo->err; err->msg_code = N; err->method(cinfo)
  lib/d3dx8             D3DX8 CPU detection / PSGP strings (DisableD3DXPSGP, GenuineIntel, ...)
  import                Imp_* thunks and SecuROM stubs
Spreading
  between   unlabeled functions whose nearest labeled neighbours on both sides agree
  owners    every function that calls it or takes its address (directly or through a pointer
            table in .data) is in one module, and the function lies within 64 KB of a
            function already in that module (C static helpers can only be called from
            their own file; modules are compact, so a shared helper far away stays open)
Also flags functions whose first instructions decode to privileged/junk opcodes: code that
SecuROM keeps encrypted until runtime, which cannot be decompiled from this exe.
Repeats until nothing changes. Everything else is reported as gaps, with hints.

usage: module_map.py <graph dir> <sh2pc.exe> <out dir>
writes  <out>/module_map.tsv      entry, size, module, method
        <out>/module_ranges.tsv   module, first, last, functions, bytes
        <out>/module_gaps.tsv     unlabeled runs between modules, with string hints
        <out>/cri_labels.json     CRI function names taken from their own error strings
"""
import sys, re, json, bisect, struct, collections
import pefile, capstone

gdir, exe, out = sys.argv[1:4]
TEXT_LO, TEXT_HI = 0x401000, 0x629000
JPEG_MSG_TABLE = 0x631908  # jpeg_std_message_table (pointer to "Bogus message code %d")


def rows(name):
    with open(f'{gdir}/{name}') as f:
        next(f)
        for line in f:
            yield line.rstrip('\n').split('\t')


funcs = {}
for e, name, size, lo, hi, thunk in rows('functions.tsv'):
    a = int(e, 16)
    if TEXT_LO <= a < TEXT_HI:
        funcs[a] = dict(name=name, size=int(size), thunk=thunk == 'true')
order = sorted(funcs)

strings = collections.defaultdict(list)
for f, _, text in rows('strings.tsv'):
    strings[int(f, 16)].append(text.replace('\\\\', '\\'))
calls = collections.defaultdict(set)
callers = collections.defaultdict(set)
for a, b in rows('calls.tsv'):
    a = int(a, 16)
    if re.fullmatch(r'[0-9a-f]{8}', b):
        b = int(b, 16)
        calls[a].add(b)
        callers[b].add(a)
globals_ = collections.defaultdict(set)
for f, g in rows('globals.tsv'):
    if re.fullmatch(r'[0-9a-f]{8}', g):
        globals_[int(f, 16)].add(int(g, 16))

# Who takes each function's address: direct code pointers, and pointer tables in data
# (a referenced global that starts a run of function entries, e.g. D3DX's PSGP table).
pe = pefile.PE(exe)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
init_hi = max(base + s.VirtualAddress + s.SizeOfRawData for s in pe.sections if s.Name.startswith(b'.data'))
takers = collections.defaultdict(set)
for f, t in rows('codeptrs.tsv'):
    takers[int(t, 16)].add(int(f, 16))
for f, gs in globals_.items():
    for g in gs:
        a = g
        while a + 4 <= init_hi and a - base + 4 <= len(img):
            v = struct.unpack_from('<I', img, a - base)[0]
            if v not in funcs:
                break
            takers[v].add(f)
            a += 4
refs_in = collections.defaultdict(set)
for b, cs in callers.items():
    refs_in[b] |= cs
for b, cs in takers.items():
    refs_in[b] |= cs

# libjpeg ERREXIT/WARNMS/TRACEMS: mov rE,[rC]; mov dword ptr [rE+0x14], imm; ... call dword ptr [rX]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
ERR_SET = re.compile(r'^dword ptr \[(e\w\w) \+ 0x14\], (0x[0-9a-f]+|\d+)$')


def ijg_errexit(a):
    lo = a - base
    ins = list(md.disasm(img[lo:lo + funcs[a]['size']], a))
    loaded = {}
    for k, i in enumerate(ins):
        if i.mnemonic == 'mov':
            m = re.match(r'^(e\w\w), dword ptr \[(e\w\w)\]$', i.op_str)
            if m:
                loaded[m.group(1)] = m.group(2)
            m = ERR_SET.match(i.op_str)
            if m and m.group(1) in loaded:
                cinfo = loaded[m.group(1)]
                for j in ins[k + 1:k + 8]:
                    if j.mnemonic == 'call' and j.op_str.startswith('dword ptr [e') and (
                            f'push {cinfo}' in ' '.join(x.mnemonic + ' ' + x.op_str for x in ins[k + 1:k + 8])):
                        return True
    return False

SRC = re.compile(r'\\CVSWork\\sh2pc\\src\\(.+)$')
PATHLIKE = re.compile(r'[\\/].*\.\w{2,4}$')
CRI = re.compile(r'^E[0-9A-F]{4,}|\((adx|ADX|mf|wx|cv|sj|mw|SFD|sfd|ahx|AHX|ADXF|ADXT|ADXB|ADXSJD|SJ|svm|SVM)\w*\)'
                 r'|CRI UT|cvFs|adxt_|adxf_|ADXT_|ADXF_|mwPly|mwSfd|IDirectSoundBuffer_\w+ return error')
PNG = re.compile(r'libpng|\bPNG\b|png_|IHDR|IDAT|IEND|PLTE|tRNS|gAMA|cHRM|sBIT|bKGD|hIST|pHYs|oFFs|tIME|tEXt|zTXt|'
                 r'Unknown zlib error|zlib (memory|version)|Too many IDAT|Invalid (bit depth|color type)|CRC error')
ZLIB = re.compile(r'invalid (stored block lengths|block type|distance|bit length repeat|literal/length)|'
                  r'too many length or distance|incorrect (header|data) check|oversubscribed|incomplete (dynamic|literal)|'
                  r'^need dictionary$|^stream end$|^insufficient memory$|^incompatible version$|^buffer error$|'
                  r'^1\.1\.3$|deflate 1\.1\.3|inflate 1\.1\.3|unknown compression method|invalid window size')
JPEG = re.compile(r'JPEG|Huffman|JFIF|quantization table|Bogus (marker|message|JPEG|virtual|sampling|Huffman|DQT|DAC|input)|'
                  r'Thomas G\. Lane|Invalid (SOS|component|crop|scan|progressive)|Sampling factors|DCT')
D3DX = re.compile(r'DisableD3DXPSGP|GenuineIntel|AuthenticAMD|UnknownVendr|CyrixInstead|Software\\Microsoft\\Direct3D')
NAME_IN_MSG = re.compile(r'\(([A-Za-z_][A-Za-z0-9_]*)\)(\\n|\n)?$')

label, method = {}, {}


def set_label(a, mod, how):
    if a not in label:
        label[a] = mod
        method[a] = how


# Anchors
cri_names = collections.defaultdict(set)
for a in order:
    f = funcs[a]
    if f['name'].startswith(('Imp_', 'stub_')) or f['thunk']:
        set_label(a, 'import', 'anchor')
        continue
    for s in strings.get(a, []):
        m = SRC.search(s)
        if m:
            set_label(a, 'game/' + m.group(1).replace('\\', '/'), 'anchor')
    ss = [s for s in strings.get(a, []) if not PATHLIKE.search(s)]
    if any(CRI.search(s) for s in ss):
        set_label(a, 'lib/cri', 'anchor')
        for s in ss:
            m = NAME_IN_MSG.search(s)
            if m:
                cri_names[m.group(1)].add(a)
    elif any(ZLIB.search(s) for s in ss):
        set_label(a, 'lib/zlib', 'anchor')
    elif any(PNG.search(s) for s in ss):
        set_label(a, 'lib/libpng', 'anchor')
    elif any(JPEG.search(s) for s in ss) or JPEG_MSG_TABLE in globals_.get(a, ()):
        set_label(a, 'lib/libjpeg', 'anchor')
    elif any(D3DX.search(s) for s in ss):
        set_label(a, 'lib/d3dx8', 'anchor')
    elif TEXT_LO + 0x1e000 <= a < TEXT_LO + 0x3d000 and ijg_errexit(a):
        # only inside the static-library area; game objects use +0x14 stores too
        set_label(a, 'lib/libjpeg', 'anchor')
anchors = sum(1 for v in method.values() if v == 'anchor')


def spans():
    sp = {}
    for a, m in label.items():
        lo, hi = sp.get(m, (a, a))
        sp[m] = (min(lo, a), max(hi, a))
    return sp


changed = True
rounds = 0
while changed and rounds < 20:
    changed = False
    rounds += 1
    # between: nearest labeled neighbours agree (an 'import' thunk does not break a run)
    lab = [a for a in order if a in label and label[a] != 'import']
    for a in order:
        if a in label:
            continue
        i = bisect.bisect_left(lab, a)
        if 0 < i < len(lab) and label[lab[i - 1]] == label[lab[i]]:
            set_label(a, label[lab[i - 1]], 'between')
            changed = True
    # owners: everything that calls it or takes its address is in one module
    members = collections.defaultdict(list)
    for x in order:
        if x in label:
            members[label[x]].append(x)
    for a in order:
        if a in label:
            continue
        # libraries: calls + taken addresses, within 64 KB. Game modules: direct calls only
        # (game code dispatches across files through tables), within 16 KB, and no other
        # module's function in between, since an .obj is one contiguous run.
        for m in ({label.get(c) for c in refs_in.get(a, ())}, {label.get(c) for c in callers.get(a, ())}):
            if len(m) != 1 or None in m:
                continue
            m = next(iter(m))
            if m == 'import':
                break
            ms = members[m]
            i = bisect.bisect_left(ms, a)
            nb = [ms[j] for j in (i - 1, i) if 0 <= j < len(ms)]
            nearest = min(nb, key=lambda x: abs(a - x))
            if m.startswith('lib/'):
                ok = abs(a - nearest) <= 0x10000 and refs_in.get(a)
            else:
                lo, hi = min(a, nearest), max(a, nearest)
                k0, k1 = bisect.bisect_left(order, lo), bisect.bisect_right(order, hi)
                ok = (callers.get(a) and abs(a - nearest) <= 0x4000 and
                      all(label.get(x) in (None, m, 'import') for x in order[k0:k1]))
            if ok:
                set_label(a, m, 'owners')
                changed = True
            break

# CRI names: one function per name and one name per function
by_func = collections.defaultdict(set)
for n, fs in cri_names.items():
    for a in fs:
        by_func[a].add(n)
cri_labels = [{'addr': hex(a), 'name': n, 'source': 'CRI error string names its function'}
              for n, fs in sorted(cri_names.items()) if len(fs) == 1
              for a in fs if len(by_func[a]) == 1 and funcs[a]['name'].startswith('FUN_')]

# Outputs
JUNK = {'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'hlt', 'cli', 'sti', 'into', 'iretd', 'lds', 'les',
        'arpl', 'bound', 'aam', 'aad', 'daa', 'das', 'aaa', 'aas', 'salc', 'int1', 'retf', 'lcall', 'ljmp'}


def encrypted(a):
    lo = a - base
    return any(i.mnemonic in JUNK for i in list(md.disasm(img[lo:lo + 64], a))[:12])


enc = {a for a in order if not funcs[a]['name'].startswith('Imp_') and encrypted(a)}
with open(f'{out}/module_map.tsv', 'w') as f:
    f.write('entry\tsize\tmodule\tmethod\tencrypted\n')
    for a in order:
        f.write(f'{a:#x}\t{funcs[a]["size"]}\t{label.get(a, "")}\t{method.get(a, "")}\t{"yes" if a in enc else ""}\n')

stat = collections.defaultdict(lambda: [None, None, 0, 0])
for a in order:
    m = label.get(a)
    if m:
        s = stat[m]
        s[0] = a if s[0] is None else min(s[0], a)
        s[1] = a if s[1] is None else max(s[1], a)
        s[2] += 1
        s[3] += funcs[a]['size']
with open(f'{out}/module_ranges.tsv', 'w') as f:
    f.write('module\tfirst\tlast\tfunctions\tbytes\n')
    for m, (lo, hi, n, b) in sorted(stat.items(), key=lambda kv: kv[1][0]):
        f.write(f'{m}\t{lo:#x}\t{hi:#x}\t{n}\t{b}\n')

# Gaps: runs of unlabeled functions, with neighbours and string hints
DATA = re.compile(r'data[\\/](\w+)[\\/](\w+)')
gaps, run = [], []
prev_label = None
for a in order + [None]:
    if a is not None and a not in label:
        run.append(a)
        continue
    if run:
        hints = collections.Counter()
        for r in run:
            for s in strings.get(r, []):
                m = DATA.search(s)
                if m:
                    hints[f'{m.group(1)}/{m.group(2)}'] += 1
        gaps.append((run[0], run[-1], len(run), sum(funcs[r]['size'] for r in run),
                     prev_label or '', label.get(a, '') if a else '',
                     ', '.join(f'{k}:{v}' for k, v in hints.most_common(4))))
        run = []
    if a is not None:
        prev_label = label[a]
with open(f'{out}/module_gaps.tsv', 'w') as f:
    f.write('first\tlast\tfunctions\tbytes\tafter\tbefore\tdata_hints\n')
    for g in sorted(gaps, key=lambda g: -g[3]):
        f.write(f'{g[0]:#x}\t{g[1]:#x}\t{g[2]}\t{g[3]}\t{g[4]}\t{g[5]}\t{g[6]}\n')
json.dump({'version': 'dc', 'md5': 'f3f5360298b6d0f6f2e09cf24a0925d1', 'globals': cri_labels},
          open(f'{out}/cri_labels.json', 'w'), indent=1)

tot = sum(funcs[a]['size'] for a in order)
lab_b = sum(funcs[a]['size'] for a in order if a in label)
print(f'.text functions {len(order)}, anchors {anchors}, labeled {len(label)} '
      f'({100 * lab_b / tot:.1f}% of bytes) after {rounds} rounds; gaps {len(gaps)}; CRI names {len(cri_labels)}; '
      f'SecuROM-encrypted {len(enc)}')
for m, (lo, hi, n, b) in sorted(stat.items(), key=lambda kv: kv[1][0]):
    print(f'  {m:40} {lo:#x}..{hi:#x} {n:5} fns {b:8} B')
