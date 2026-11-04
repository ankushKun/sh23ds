#!/usr/bin/env python3
"""Checks the port's translation table (port/source/i18n.c, see port/UI.md "Language"). Fails when:

- a translation's printf specifiers (tr() output is used as a format) or {TOKENS} differ from the
  English, or it has another number of line breaks (panels and pages are laid out by line);
- a translation holds a byte the font has no glyph for (0x80..0x9f, control codes but the newline);
- an English key is in the table twice, or no longer appears in any string of port/source, which is
  what happens to its translations when the English wording changes (the panels' key verbs, such as
  "hole" in "D-pad: hole  A: put in", count as found).

  python3 tools/i18n_check.py
"""
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'port', 'source')
LIT = r'"((?:[^"\\]|\\.)*)"'
ENTRY = re.compile(r'\{\s*' + LIT + r'\s*,\s*\{\s*' + r'\s*,\s*'.join([LIT] * 4) + r'\s*\}\s*\}')
SPEC = re.compile(rb'%[-+ #0-9.*]*[a-zA-Z]')
TOKEN = re.compile(rb'\{[A-Z]+\}')
LANGS = ('French', 'German', 'Italian', 'Spanish')


def unescape(s):
    """A C string literal's bytes (octal and the usual escapes; the table uses no others)."""
    out, i = bytearray(), 0
    while i < len(s):
        if s[i] != '\\':
            out += s[i].encode('latin-1')
            i += 1
            continue
        m = re.match(r'[0-7]{1,3}', s[i + 1:])
        if m:
            out.append(int(m.group(), 8))
            i += 1 + len(m.group())
        else:
            out += {'n': b'\n', 't': b'\t', '"': b'"', '\\': b'\\'}.get(s[i + 1], s[i + 1].encode())
            i += 2
    return bytes(out)


def main():
    text = open(os.path.join(SRC, 'i18n.c'), encoding='latin-1').read()
    start = text.index('static const Entry TABLE[]')
    table = text[start:text.index('\n};', start)]
    rows = ENTRY.findall(table)
    errors = []
    lines = table.count('\t{ "')
    if len(rows) != lines:
        errors.append(f'{lines - len(rows)} entries do not have four translations')

    strings = []
    for path in glob.glob(os.path.join(SRC, '**', '*.c'), recursive=True):
        if os.path.basename(path) != 'i18n.c':
            strings += [unescape(m).lower() for m in re.findall(LIT, open(path, encoding='latin-1').read())]

    seen = set()
    for row in rows:
        en = unescape(row[0])
        name = en.decode('latin-1')
        if en in seen:
            errors.append(f'"{name}" is in the table twice')
        seen.add(en)
        if not any(en.lower() in s for s in strings):
            errors.append(f'"{name}" is not in any string of port/source any more')
        for lang, t in zip(LANGS, row[1:]):
            tb = unescape(t)
            if SPEC.findall(tb) != SPEC.findall(en):
                errors.append(f'"{name}" ({lang}): specifiers {SPEC.findall(tb)} for {SPEC.findall(en)}')
            if sorted(TOKEN.findall(tb)) != sorted(TOKEN.findall(en)):
                errors.append(f'"{name}" ({lang}): tokens differ')
            breaks, en_breaks = tb.count(b'\n'), en.count(b'\n')
            if breaks != en_breaks:
                errors.append(f'"{name}" ({lang}): {breaks} line breaks for {en_breaks}')
            bad = sorted({b for b in tb if 0x80 <= b < 0xa0 or (b < 0x20 and b != 0x0a)})
            if bad:
                errors.append(f'"{name}" ({lang}): no glyph for bytes {", ".join(hex(b) for b in bad)}')

    for e in errors:
        print(e, file=sys.stderr)
    print(f'i18n.c: {len(rows)} entries, {len(errors)} problems')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
