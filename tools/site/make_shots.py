#!/usr/bin/env python3
"""Builds the website's screenshots (site/assets/shots/) from a folder of captures.

  make_shots.py <captures dir> [mapping.txt]

Each capture is a pair <name>_top.png (400x240) and <name>_bottom.png (320x240), as tools/lr3ds writes
them when split. The mapping file has one line per site picture:

  <site name> <capture name> [bottom capture name]

and writes <site name>-top.jpg and <site name>-bottom.jpg. A site name starting with "f-" is a feature
picture: the top screen only, or with a bottom capture, both screens side by side on one 400x240 frame.
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'site', 'assets', 'shots')


def load(src, name, half):
    path = os.path.join(src, f'{name}_{half}.png')
    return Image.open(path).convert('RGB')


def side_by_side(top, bottom):
    """Both screens on one 5:3 frame: the top screen scaled to the left, the touch screen to the right."""
    frame = Image.new('RGB', (800, 480), (8, 9, 10))
    t = top.resize((480, 288), Image.LANCZOS)
    b = bottom.resize((288, 216), Image.LANCZOS)
    frame.paste(t, (8, 96))
    frame.paste(b, (800 - 288 - 8, 132))
    return frame


def main():
    src = sys.argv[1]
    mapping = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'shots.txt')
    os.makedirs(OUT, exist_ok=True)
    for line in open(mapping):
        parts = line.split('#')[0].split()
        if not parts:
            continue
        site, cap = parts[0], parts[1]
        bottom_cap = parts[2] if len(parts) > 2 else cap
        top = load(src, cap, 'top')
        if site.startswith('f-'):
            img = side_by_side(top, load(src, bottom_cap, 'bottom')) if len(parts) > 2 else top
            img.save(os.path.join(OUT, f'{site}.jpg'), quality=90)
            print('wrote', site)
            continue
        top.save(os.path.join(OUT, f'{site}-top.jpg'), quality=90)
        load(src, bottom_cap, 'bottom').save(os.path.join(OUT, f'{site}-bottom.jpg'), quality=90)
        print('wrote', site)


if __name__ == '__main__':
    main()
