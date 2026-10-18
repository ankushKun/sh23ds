#!/bin/sh
# The critical path by teleport (pt fast 1): c01..c10 in order, each loading the save the previous one
# made, then the six endings of James' game from c10's save and Born From a Wish from the title.
# Runs on one virtual SD; summary in .lr3ds/playtest/logs/chain_summary.txt.
D=$(cd "$(dirname "$0")" && pwd)
S="$(cd "$D/../.." && pwd)/.lr3ds/playtest/logs/chain_summary.txt"
mkdir -p "$(dirname "$S")"
: > "$S"
for c in ${CHAIN:-c01_prologue c02_town c03_apart c04_west c05_townwest c06_hospital c07_otherhospital c08_prison c09_hotel c10_otherhotel e_maria e_water e_leave e_dog e_ufo e_rebirth c11_wish}; do
	"$D/pt.sh" "$c" 90000 | tee -a "$S"
	case $c in c*) grep -q "^$c: .* fail 0 finished 1" "$S" || { echo "stopped: $c failed, and the next chapters load its save" | tee -a "$S"; exit 1; } ;; esac
done
grep -c ' fail 0 finished 1' "$S" | xargs printf '%s scripts passed\n'
! grep -q -v ' fail 0 finished 1' "$S"
