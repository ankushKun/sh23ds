#!/bin/sh
# Builds the saved Ghidra project ($SH2_GHPROJ/sh2dc, default ~/ghproj) from your own Director's Cut
# exe and the text dump in sh2-3ds-port-kit/data/dc/project, which is what is in git instead of the
# project itself. Takes about 5 minutes. An existing project is moved aside, never deleted.
#   1. import + Ghidra auto-analysis
#   2. ApplyImportSites: the API names and call overrides SecuROM hides (dc_import_*.tsv)
#   3. ImportPs2Types: the PS2 beta's structs and enums, one category per source file, from the
#      dwarf2cpp dump (tools/ghidra/fetch_refs.sh; skipped with a warning when it is missing)
#   4. ImportProject: function bodies, names, tags, labels, typed globals, comments, bookmarks
# Usage: tools/ghidra/rebuild.sh
. "$(dirname "$0")/env.sh"
[ -f "$SH2_EXE" ] || { echo "No exe at $SH2_EXE (set SH2_EXE)" >&2; exit 2; }
sum=$(md5 -q "$SH2_EXE" 2>/dev/null || md5sum "$SH2_EXE" | cut -d' ' -f1)
[ "$sum" = f3f5360298b6d0f6f2e09cf24a0925d1 ] || { echo "$SH2_EXE is not the Director's Cut reference (md5 $sum); see sh2-3ds-port-kit/tools/sh2version.py" >&2; exit 2; }

TYPES=
HDR=$SH2_GHPROJ/ps2h
if [ -d "$PS2_SRC" ]; then
	mkdir -p "$HDR"
	( cd "$PS2_SRC" && find . -name '*.c' ) | while IFS= read -r f; do
		out=$(printf '%s' "${f#./}" | tr '/' '_').h
		python3 "$ROOT/tools/dwarf2h.py" "$PS2_SRC/$f" "$HDR/$out" >/dev/null || echo "dwarf2h failed on $f" >&2
	done
	TYPES="-postScript ImportPs2Types.java $HDR"
	echo "PS2 type headers: $(ls "$HDR" | wc -l | tr -d ' ') in $HDR"
else
	echo "warning: no PS2 dwarf2cpp dump at $PS2_SRC, the project gets no /ps2 types (run tools/ghidra/fetch_refs.sh)" >&2
fi

if [ -d "$SH2_GHPROJ/sh2dc.rep" ]; then
	old=$SH2_GHPROJ.old-$(date +%Y%m%d-%H%M%S)
	mv "$SH2_GHPROJ" "$old" && echo "moved the old project to $old"
fi
mkdir -p "$SH2_GHPROJ"
# shellcheck disable=SC2086
"$HEADLESS" "$SH2_GHPROJ" sh2dc -import "$SH2_EXE" -scriptPath "$K/ghidra_scripts" \
	-postScript ApplyImportSites.java "$K/data/dc/dc_import_sites.tsv" "$K/data/dc/dc_import_thunks.tsv" \
	$TYPES \
	-postScript ImportProject.java "$K/data/dc/project" 2>&1 \
	| grep -E '\.java> |ERROR' | sed -E 's/^INFO +//; s/ \(GhidraScript\) *$//'
echo "project: $SH2_GHPROJ/sh2dc (open it in the Ghidra GUI, or use tools/ghidra/dec.sh)"
