#!/bin/sh
# Clones the outside references the port leans on into $SH2_REFSRC (default ~/Developer/refsrc), at
# the commits the repo's data was made from. They are documentation to read, never code to run or
# copy into the port; licences vary, so nothing from them is vendored here. See AGENTS.md.
# Usage: tools/ghidra/fetch_refs.sh [name ...]   (no names: all of them)
. "$(dirname "$0")/env.sh"
mkdir -p "$SH2_REFSRC"
while read -r name url commit; do
	case "$name" in ''|'#'*) continue ;; esac
	if [ $# -gt 0 ]; then
		want=0
		for n in "$@"; do [ "$n" = "$name" ] && want=1; done
		[ $want = 1 ] || continue
	fi
	dir=$SH2_REFSRC/$name
	[ -d "$dir/.git" ] || git clone -q "$url" "$dir" || { echo "clone failed: $url" >&2; continue; }
	git -C "$dir" fetch -q origin "$commit" 2>/dev/null
	git -C "$dir" -c advice.detachedHead=false checkout -q "$commit" && echo "$name at $commit"
done <<'LIST'
# name                 url                                                          commit
Silent-Hill            https://github.com/Sparagas/Silent-Hill.git                  f08ec73e32c0b0d5ddbb58f96eef51f0b8ee5b35
sh2ee                  https://github.com/elishacloud/Silent-Hill-2-Enhancements.git 9cf46217a76d8a2914189b4eb44cbe0d98f7c9a6
sh2_source             https://github.com/Palm-Studios/sh2_source.git               7c701e7067edacb685412e331b1632422c8cda03
sh2tex                 https://github.com/iOrange/sh2tex.git                        c48e787595b6b2bac513ad8d203b70b60bec14b1
SH2_map                https://github.com/alanm20/SH2_map.git                       43a44cc24c2c9a27837a87c47978814c528d5077
SH2MapTools            https://github.com/FrozenFish24/SH2MapTools.git              37398b5412fe0b361d575fa958d32f936cdf2449
SilentHillDatabase     https://github.com/Polymega/SilentHillDatabase.git           154cc94e6ea596b0fed2d9754b450fb738f21724
silent-hill-museum     https://github.com/dreamingmoths/silent-hill-museum.git      e12c2f1441d4b237ad7a9893a8463c5a96d91f52
Misc-Game-Research     https://github.com/Murugo/Misc-Game-Research.git             c088e878544f25d45ea7cf4215613fb5f9638dd4
LIST
