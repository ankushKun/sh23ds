#!/usr/bin/env python3
"""Reader for SH2 PC message files (data/etc/message/*.mes).

Layout: u16 count, u16 offset[count] (in 16-bit words from the file start). A message runs to the
next offset and holds one or more pages. Each page starts on a 16-bit boundary with control words
(0x1000, 0x8000, 0x9000: display style, the low byte is 0), then byte-coded text: a byte b < 0xff
is the character chr(b + 0x20) (so 0x00 is a space), 0xff is an escape: ff fd newline, ff ff end of
page; other escapes (ff fa, ff 01, ...) are display codes and are dropped. Pages are joined with \f.
"""
import struct
import sys

import ee


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
            if b == 0xff and p + 1 < end:
                c = d[p + 1]
                p += 2
                if c == 0xff:
                    pages.append(''.join(s).strip())
                    s = []
                elif c == 0xfd:
                    s.append('\n')
                elif 0xe0 <= c <= 0xe3:
                    s.append(chr(1 + c - 0xe0))     # inserted value 0..3 (MesSetInsert 0x47fd70)
                elif c == 0xf6:
                    s.append('\x05')               # yes / no question follows
                continue
            s.append(chr(b + 0x20) if b < 0x60 else '?')
            p += 1
        if s:
            pages.append(''.join(s).strip())
        out.append('\f'.join(x for x in pages if x))
    return out


if __name__ == '__main__':
    for i, m in enumerate(load(sys.argv[1])):
        print(i, repr(m))
