#!/usr/bin/env python3
"""Write the stage enemy placements (Enemy_List, decoded by events.py) as one text file for the 3DS.

  enemies_pack.py <pack dir>        reads <pack>/events/stage_NN.json, writes <pack>/events/enemies.txt

One line per enemy: stage kind id x y z rot_y status condition (kind and condition in hex). rot_y is
the raw 4.12 fixed-point angle in radians (12867 = pi). The characters themselves are converted with
chr.py; see port/ENEMIES.md for the full list of commands.
"""
import glob
import json
import os
import sys


def main():
    pack = sys.argv[1]
    lines = []
    for path in sorted(glob.glob(os.path.join(pack, 'events', 'stage_*.json'))):
        stage = json.load(open(path))
        num = int(os.path.basename(path)[6:-5])
        for e in stage.get('enemies', []):
            x, y, z = e['pos']
            lines.append('%d %x %d %d %d %d %d %d %x' % (num, int(e['kind'], 16), e['id'], x, y, z,
                                                         e['rot_y_raw'], e['status'], int(e['condition'], 16)))
    with open(os.path.join(pack, 'events', 'enemies.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{len(lines)} enemies')


if __name__ == '__main__':
    main()
