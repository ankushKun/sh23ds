#!/bin/sh
# The critical path on foot (pt walkmode 1): w01..w10 in order, each loading the save the previous one
# made, then Born From a Wish (w11). FROM=wNN starts later, WALK="..." picks scripts.
# Summary in .lr3ds/playtest/logs/walk_summary.txt.
D=$(cd "$(dirname "$0")" && pwd)
S="$(cd "$D/../.." && pwd)/.lr3ds/playtest/logs/walk_summary.txt"
mkdir -p "$(dirname "$S")"
: > "$S"
go=${FROM:-w01}; on=0
for c in ${WALK:-w01 w02 w03 w04 w05 w06 w07 w08 w09 w10 w11}; do
	[ "$c" = "$go" ] && on=1
	[ $on = 1 ] || continue
	"$D/pt.sh" "$c" 400000 | tee -a "$S"
	grep -q "^$c: .* fail 0 finished 1" "$S" || { echo "stopped: $c failed, and the next chapters load its save" | tee -a "$S"; exit 1; }
done
! grep -q -v ' fail 0 finished 1' "$S"
