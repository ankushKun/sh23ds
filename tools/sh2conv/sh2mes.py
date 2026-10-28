#!/usr/bin/env python3
"""Reader for SH2 PC message files (data/etc/message/*.mes).

Layout: u16 count, u16 offset[count] (in 16-bit words from the file start). A message runs to the
next offset and holds one or more pages. Each page starts on a 16-bit boundary with control words
(0x1000, 0x8000, 0x9000: display style, the low byte is 0), then byte-coded text read as 0x47f400
reads it after 0x8000: a byte b < 0xe0 is the font code b, the Windows-1252 character
chr(b + 0x20) (so 0x00 is a space and the accents of the other languages come through); e0 lo hi
is the full-width code hi << 8 | lo (the titles' letters); a byte b > 0xe0 starts the two-byte
code b << 8 | next: ff fd newline, ff ff end of page, ff fe a full-width space, fc nn / fd nn move
the pen back / on by nn (the titles' kerning); other codes (ff fa, ff 01, f8 nn, ...) are display
codes and are dropped. Pages are joined with \f. A full-width code (and the kerning) becomes
FULL_WIDTH and the code as four hex digits, which port/source/font.c draws.
"""
import struct
import sys

import ee

FULL_WIDTH = '\x06'


def full_width(code):
    return '%s%04x' % (FULL_WIDTH, code)


def _is_control(d, p):
    return p + 1 < len(d) and d[p] == 0 and d[p + 1] and not d[p + 1] & 0x0f


def load(path):
    """A .mes file; the Enhanced Edition's version when the player has it (ee.py)."""
    return ee.load(path, _load)


def _load(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<H', d, 0)[0]
    offs = list(struct.unpack_from(f'<{n}H', d, 2))
    out = []
    for i, o in enumerate(offs):
        p, end = o * 2, offs[i + 1] * 2 if i + 1 < n else len(d)
        pages, s = [], []
        while p < end:
            if not s:
                p += p & 1
                while _is_control(d, p):
                    p += 2
                if p >= end:
                    break
            b = d[p]
            if b == 0xe0 and p + 2 < end:
                s.append(full_width(d[p + 1] | d[p + 2] << 8))
                p += 3
                continue
            if b > 0xe0 and p + 1 < end:
                c = d[p + 1]
                p += 2
                if b in (0xfc, 0xfd):
                    s.append(full_width(b << 8 | c))
                if b != 0xff:
                    continue
                if c == 0xff:
                    pages.append(''.join(s).strip())
                    s = []
                elif c == 0xfd:
                    s.append('\n')
                elif c == 0xfe:
                    s.append(full_width(0xfffe))
                elif 0xe0 <= c <= 0xe3:
                    s.append(chr(1 + c - 0xe0))     # inserted value 0..3 (MesSetInsert 0x47fd70)
                elif c == 0xf6:
                    s.append('\x05')               # yes / no question follows
                continue
            s.append(chr(b + 0x20) if b < 0xe0 else '?')
            p += 1
        if s:
            pages.append(''.join(s).strip())
        out.append('\f'.join(x for x in pages if x))
    return out


if __name__ == '__main__':
    for i, m in enumerate(load(sys.argv[1])):
        print(i, repr(m))
