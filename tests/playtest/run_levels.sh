#!/bin/sh
# The walk chain (w01..w11) and the endings on other difficulty levels:
#   TAG=beginner RIDDLE=0 ACTION=0 tests/playtest/run_levels.sh
#   TAG=hard RIDDLE=2 ACTION=3 SLOT=2 tests/playtest/run_levels.sh
#   TAG=extra RIDDLE=2 ACTION=2 CLEAR="7 1 2" SLOT=3 tests/playtest/run_levels.sh
#     (hard riddle becomes the extra riddle level once easy, normal and hard riddles have been cleared)
# The new game scripts (w01, w11) get the levels as debug.txt keys and the clear data; the endings load
# the walk's saves (wNN) instead of the teleport chain's (cNN); every save is named <tag>_<name>.
# Level copies of the scripts go to .lr3ds/playtest/scripts/L_<tag>_*, the summary to
# .lr3ds/playtest/logs/levels_<tag>.txt.
D=$(cd "$(dirname "$0")" && pwd)
OUT="$(cd "$D/../.." && pwd)/.lr3ds/playtest"
TAG=${TAG:?set TAG, RIDDLE and ACTION}; R=${RIDDLE:-1}; A=${ACTION:-2}; CLEAR=${CLEAR:-0 0 0}
S="$OUT/logs/levels_$TAG.txt"
mkdir -p "$OUT/scripts" "$OUT/logs"
: > "$S"
mk() {   # mk <script>: the level's copy, with saves of its own
	out="$OUT/scripts/L_${TAG}_$1.pt"
	case $1 in
	w01|w11) { echo "riddle $R"; echo "action $A"; sed "s/^pt prog clear 0 0 0/pt prog clear $CLEAR/" "$D/scripts/$1.pt"; } ;;
	e_*) sed 's/^pt load c/pt load w/' "$D/scripts/$1.pt" ;;
	*) cat "$D/scripts/$1.pt" ;;
	esac | sed -E "s/^pt (save|load) /pt \1 ${TAG}_/" > "$out"
}
for c in ${WALK:-w01 w02 w03 w04 w05 w06 w07 w08 w09 w10 e_maria e_water e_leave e_dog e_ufo e_rebirth w11}; do
	mk "$c"
	"$D/pt.sh" "L_${TAG}_$c" 400000 | sed "s/^L_${TAG}_//" | tee -a "$S"
	case $c in w*) grep -q "^$c: .* fail 0 finished 1" "$S" || { echo "stopped: $c failed, and the next chapters load its save" | tee -a "$S"; exit 1; } ;; esac
done
! grep -q -v ' fail 0 finished 1' "$S"
