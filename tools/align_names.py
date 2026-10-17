#!/usr/bin/env python3
"""Name PC functions by aligning them with the PS2 beta's source-ordered function list.

Within one .obj, MSVC (PC) and the PS2 compiler both emit functions in source order. Given anchors
(PC functions already named, e.g. from assert line numbers) the PC functions between two anchors
correspond to the PS2 functions between them, minus functions the PC compiler inlined or dropped.
A small DP aligns each gap by size (x86 bytes vs MIPS bytes, ratio calibrated on the anchors)
and allows skips on the PS2 side.

  align_names.py <graph functions.tsv> <ps2_functions.json> <anchors.json> <file> <pc_lo> <pc_hi> <out.json>
  e.g. align_names.py graph/functions.tsv ps2_functions.json ps2_assert_labels.json view/vc_main.c 0x519400 0x51d000 out.json
"""
import json
import math
import sys

fun_tsv, ps2_json, anchors_json, src_file, lo, hi, out = sys.argv[1:8]
lo, hi = int(lo, 16), int(hi, 16)

pc = []
for line in list(open(fun_tsv))[1:]:
    e, name, size, mn, mx, thunk = line.rstrip('\n').split('\t')
    a = int(e, 16)
    if lo <= a < hi and thunk != 'true':
        pc.append((a, int(size), name))
pc.sort()

ps2 = sorted([f for f in json.load(open(ps2_json))['functions'] if f['file'] == src_file and f.get('end')],
             key=lambda f: int(f['start'], 16))
ps2 = [(f['name'], int(f['end'], 16) - int(f['start'], 16)) for f in ps2]
ps2_index = {n: i for i, (n, _) in enumerate(ps2)}

named = {int(l['addr'], 16): l['name'] for l in json.load(open(anchors_json))['globals']}
anchors = [(i, ps2_index[named[a]]) for i, (a, _, _) in enumerate(pc) if named.get(a) in ps2_index]
# keep the longest chain of anchors that is increasing on both sides
best = []
for k in range(len(anchors)):
    chain = [anchors[k]]
    for x in anchors[k + 1:]:
        if x[1] > chain[-1][1]:
            chain.append(x)
    if len(chain) > len(best):
        best = chain
dropped = [pc[i][0] for i, _ in anchors if (i, _) not in best]
anchors = best
print('anchors:', [(hex(pc[i][0]), ps2[j][0]) for i, j in anchors], 'dropped:', [hex(x) for x in dropped])
if not anchors:
    sys.exit('no anchors in range')
ratio = math.exp(sum(math.log(max(pc[i][1], 1) / max(ps2[j][1], 1)) for i, j in anchors) / len(anchors))


def cost(pc_size, ps2_size):
    r = (pc_size + 8) / (ratio * ps2_size + 8)
    return abs(math.log(r))


SKIP_PS2, SKIP_PC = 0.25, 0.7


def align(pcs, ps2s):
    """DP alignment keeping order; PS2 functions may be skipped (inlined/dropped on PC) and, at a
    higher cost, PC functions too (PC-only code or fragments split off by the orphan pass)."""
    n, m = len(pcs), len(ps2s)
    INF = float('inf')
    dp = [[INF] * (m + 1) for _ in range(n + 1)]
    bt = [[None] * (m + 1) for _ in range(n + 1)]
    dp[0][0] = 0
    for i in range(n + 1):
        for j in range(m + 1):
            if i == 0 and j == 0:
                continue
            opts = []
            if i and j:
                opts.append((dp[i - 1][j - 1] + cost(pcs[i - 1][1], ps2s[j - 1][1]), 'take'))
            if j:
                opts.append((dp[i][j - 1] + SKIP_PS2, 'skip2'))
            if i:
                opts.append((dp[i - 1][j] + SKIP_PC, 'skippc'))
            dp[i][j], bt[i][j] = min(opts)
    i, j, pairs = n, m, []
    while i > 0 or j > 0:
        b = bt[i][j]
        if b == 'take':
            pairs.append((i - 1, j - 1))
            i, j = i - 1, j - 1
        elif b == 'skip2':
            j -= 1
        else:
            i -= 1
    return pairs[::-1]


labels = []
bounds = [(-1, -1)] + anchors + [(len(pc), len(ps2))]
for (pi0, si0), (pi1, si1) in zip(bounds[:-1], bounds[1:]):
    pcs = pc[pi0 + 1:pi1]
    ps2s = ps2[si0 + 1:si1]
    if not pcs or not ps2s:
        continue
    for a, b in align(pcs, ps2s):
        c = cost(pcs[a][1], ps2s[b][1])
        if c < 0.6:      # size within ~1.8x of expectation
            labels.append({'addr': hex(pcs[a][0]), 'name': ps2s[b][0],
                           'source': f'aligned with PS2 {src_file} between anchors (size {pcs[a][1]} vs '
                                     f'{ps2s[b][1]}, ratio {ratio:.2f}, cost {c:.2f})'})
json.dump({'version': 'dc', 'globals': labels}, open(out, 'w'), indent=1)
print(f'{src_file}: {len(pc)} PC functions in range, {len(ps2)} PS2 functions, {len(anchors)} anchors, '
      f'size ratio {ratio:.2f}, {len(labels)} names aligned')
for l in labels:
    print(f"  {l['addr']} {l['name']}  ({l['source'].split('(')[1]}")
