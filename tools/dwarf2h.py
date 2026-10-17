#!/usr/bin/env python3
"""Turn one dwarf2cpp file (SH2 PS2 beta) into a C header that Ghidra's C parser accepts.

  dwarf2h.py <dwarf2cpp .c file> <out.h>

Layouts stay byte-exact:
  * "enum X : unsigned char { A, B };" -> "typedef unsigned char X;" plus "enum X__values { A, B };"
  * every struct/union is forward-declared with a typedef so bare names resolve
  * declarations are emitted in dependency order (by-value uses only; pointers don't need a body)
  * function bodies (line tables, locals) are dropped; signatures become prototypes
  * globals become extern declarations
"""
import re
import sys

PRIMS = {'void', 'char', 'short', 'int', 'long', 'float', 'double', 'unsigned', 'signed', 'const',
         'volatile', 'struct', 'union', 'enum', 'bool'}


def type_tokens(decl):
    """Named types used by value in a field/typedef declaration."""
    d = re.sub(r'\[[^\]]*\]', '', decl)
    if re.search(r'\(\s*\*', d):           # function pointer: nothing by value
        return set()
    m = re.match(r'^\s*([\w ]+?)\s*(\**)\s*\w*\s*(:\s*\d+)?\s*;?\s*$', d)
    if not m or m.group(2):
        return set()
    return {t for t in m.group(1).split() if t not in PRIMS}


def convert(src):
    text = open(src, encoding='utf-8', errors='replace').read()
    # PS2 128-bit integers (u_long128): opaque 16 bytes by value, void* through pointers
    text = re.sub(r'<unknown fundamental type \(0x[0-9a-f]+\)>\s*\*', 'void*', text)
    text = re.sub(r'<unknown fundamental type \(0x[0-9a-f]+\)>', 'u128', text)
    lines = text.split('\n')
    items = []          # (kind, name, text, deps)
    records = set()
    protos, globals_ = [], []
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = re.match(r'^enum (\w+)(?: : ([\w ]+))?$', ln)
        if m:
            name, under = m.group(1), (m.group(2) or 'int').strip()
            vals = []
            i += 1
            while i < len(lines) and not lines[i].startswith('};'):
                v = lines[i].strip().rstrip(',')
                if v and v != '{':
                    vals.append(v)
                i += 1
            text = f'typedef {under} {name};'
            if vals:
                text += f'\nenum {name}__values {{ ' + ', '.join(vals) + ' };'
            items.append(('enum', name, text, set()))
            i += 1
            continue
        if re.match(r'^typedef enum \w+( : [\w ]+)?;$', ln):
            i += 1
            continue
        m = re.match(r'^typedef (struct|union) (\w+);$', ln)
        if m:
            records.add((m.group(1), m.group(2)))
            i += 1
            continue
        m = re.match(r'^(struct|union) (\w+)$', ln)
        if m:
            kind, name = m.groups()
            records.add((kind, name))
            body = [ln]
            deps = set()
            i += 1
            depth = 0
            while i < len(lines):
                cur = lines[i]
                if depth > 0 and (not cur.strip() or re.match(r'^\s+[\w \*]+\s\**\w+\(.*\);$', cur)):
                    i += 1             # blank lines and member-function declarations
                    continue
                body.append(cur)
                depth += cur.count('{') - cur.count('}')
                if lines[i].strip() not in ('{', '};', '}'):
                    deps |= type_tokens(lines[i])
                i += 1
                if depth <= 0 and lines[i - 1].startswith('}'):
                    break
            items.append(('record', name, '\n'.join(body), deps - {name}))
            continue
        m = re.match(r'^typedef (.+?);$', ln)
        if m and not re.match(r'^typedef (struct|union|enum) \w+;$', ln):
            nm = re.search(r'(\w+)\s*(\[[^\]]*\])*\s*$', re.sub(r'\(.*', '', m.group(1)))
            fp = re.search(r'\(\s*\*\s*(\w+)\s*\)', m.group(1))
            name = fp.group(1) if fp else (nm.group(1) if nm else ln)
            items.append(('typedef', name, ln, type_tokens(m.group(1).replace(name, 'X', 1) + ';')))
            i += 1
            continue
        if ln.startswith('// Start address'):
            sig = lines[i + 1].strip() if i + 1 < len(lines) else ''
            if '(' in sig:
                protos.append(sig + ';')
            i += 2
            if i < len(lines) and lines[i] == '{':
                depth = 0
                while i < len(lines):
                    depth += lines[i].count('{') - lines[i].count('}')
                    i += 1
                    if depth <= 0:
                        break
            continue
        if ln and not ln.startswith(('//', '\t', ' ', '{', '}', '#')) and ln.endswith(';'):
            if '(' in ln and not re.search(r'\(\s*\*', ln):
                protos.append(ln)
            else:
                globals_.append('extern ' + ln)
        i += 1

    by_name = {}
    for it in items:
        by_name.setdefault(it[1], it)
    emitted, order = set(), []

    def emit(name, stack=()):
        if name in emitted or name not in by_name or name in stack:
            return
        for d in by_name[name][3]:
            emit(d, stack + (name,))
        emitted.add(name)
        order.append(by_name[name][2])

    for it in items:
        if it[0] == 'enum':
            emit(it[1])
    for it in items:
        emit(it[1])
    proto_names = {re.search(r'(\w+)\s*\(', p).group(1) for p in protos if re.search(r'(\w+)\s*\(', p)}
    globals_ = [g for g in globals_ if not any(re.search(r'\b' + n + r'\b', g) for n in proto_names
                                               if re.search(r'\(\s*\*\s*' + n + r'\s*\)', g))]
    head = ['/* generated by tools/dwarf2h.py from ' + src.split('/src/')[-1] + ' */',
            'typedef struct { unsigned int w[4]; } u128;']
    head += [f'typedef {k} {n} {n};' for k, n in sorted(records)]
    return '\n'.join(head + order + [''] + globals_ + [''] + protos) + '\n'


FNPTR = re.compile(r'^(\s*)[\w \*]+?\(\s*\*\s*(\w+)\s*((?:\[[^\]]*\])*)\s*\)\s*\(.*\)\s*;$')


def simplify_fnptrs(text):
    """Function pointers (and arrays of them) in fields/typedefs become void* of the same shape."""
    out = []
    for ln in text.split('\n'):
        m = FNPTR.match(ln)
        if m and not ln.lstrip().startswith(('extern', 'typedef')) and ln.startswith((' ', '\t')):
            ln = f'{m.group(1)}void* {m.group(2)}{m.group(3)};'
        elif m and ln.startswith('typedef'):
            ln = f'typedef void* {m.group(2)}{m.group(3)};'
        out.append(ln)
    return '\n'.join(out)


def repair(text, tries=8):
    """Drop top-level declarations clang still rejects (rare dwarf2cpp oddities)."""
    import subprocess
    import tempfile
    for _ in range(tries):
        with tempfile.NamedTemporaryFile('w', suffix='.h', delete=False) as f:
            f.write(text)
            path = f.name
        r = subprocess.run(['clang', '-fsyntax-only', '-x', 'c', '-Wno-everything', '-ferror-limit=0', path],
                           capture_output=True, text=True)
        bad = sorted({int(m.group(1)) for m in re.finditer(re.escape(path) + r':(\d+):\d+: error', r.stderr)})
        if not bad:
            return text, 0
        lines = text.split('\n')
        for n in bad:
            ln = lines[n - 1]
            if ln.startswith((' ', '\t')):
                lines[n - 1] = '\t/* dropped: ' + ln.strip().replace('*/', '') + ' */ int _dropped_' + str(n) + ';'
            else:
                lines[n - 1] = '/* dropped: ' + ln.replace('*/', '') + ' */'
        text = '\n'.join(lines)
    return text, len(bad)


if __name__ == '__main__':
    src, out = sys.argv[1:3]
    text, left = repair(simplify_fnptrs(convert(src)))
    open(out, 'w').write(text)
    if left:
        print(f'{out}: {left} errors left')
