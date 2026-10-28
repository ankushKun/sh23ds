#!/usr/bin/env python3
"""Writes site/packer/sh2conv.zip: the converters and the data files they read, which the browser
packer unpacks into Pyodide at /sh23ds (the repository's layout, so the scripts find them as here).

  tools/packer-web/bundle.py [out.zip]

The Pages workflow runs it before publishing site/, so the page always packs with the converters of
the same commit. The zip is not committed (.gitignore).
"""
import os
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FILES = ['VERSION', 'sh2-3ds-port-kit/data/dc/stage_table.json',
         'sh2-3ds-port-kit/data/dc/ghidra_functions_fixed.tsv', 'sh2-3ds-port-kit/data/dc/game_files.tsv',
         'sh2-3ds-port-kit/data/dc/exe_reads.tsv']


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'site', 'packer', 'sh2conv.zip')
    conv = os.path.join(ROOT, 'tools', 'sh2conv')
    names = FILES + sorted('tools/sh2conv/' + f for f in os.listdir(conv) if f.endswith('.py'))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for n in names:
            # fixed dates, so the same sources give the same zip
            info = zipfile.ZipInfo(n, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, open(os.path.join(ROOT, n), 'rb').read())
    print('%s: %d files, %d KB' % (os.path.relpath(out, ROOT), len(names), os.path.getsize(out) // 1024))


if __name__ == '__main__':
    main()
