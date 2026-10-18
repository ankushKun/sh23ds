#!/bin/sh
# The boss fights with James' own weapons (pt fight), from the teleport chain's saves (run run_chain.sh
# first): b*g with "pt god 1" (blows take no health; checks that each fight ends the PC's way) and b*
# without (balance; the driver plays crudely, so a loss there says little about the port).
# Summary in .lr3ds/playtest/logs/boss_summary.txt.
D=$(cd "$(dirname "$0")" && pwd)
S="$(cd "$D/../.." && pwd)/.lr3ds/playtest/logs/boss_summary.txt"
mkdir -p "$(dirname "$S")"
: > "$S"
for c in ${BOSS:-b02 b04g b07g b08g b10g bmaryg b04 b07 b08 b10 bmary}; do
	"$D/pt.sh" "$c" 200000 | tee -a "$S"
done
