#!/usr/bin/env python3
"""Regenerates the website's install QR codes (site/qr/). They point at GitHub's "latest release"
download URLs, so they only change if the repository or the asset names change.

  pip install segno==1.6.1 && python3 tools/make_site_qr.py
"""
import os

import segno

REPO = 'ankushKun/sh23ds'
LATEST = f'https://github.com/{REPO}/releases/latest/download'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'site', 'qr')

for name, asset in (('install-cia', 'sh23ds.cia'), ('download-3dsx', 'sh23ds.3dsx')):
    qr = segno.make(f'{LATEST}/{asset}', error='m')
    path = os.path.join(OUT, f'{name}.svg')
    qr.save(path, kind='svg', scale=1, border=2, dark='#0b1219', light='#e9edf0', xmldecl=False,
            svgns=True, title=f'QR code for the latest {asset}', omitsize=True)
    print('wrote', os.path.relpath(path))
