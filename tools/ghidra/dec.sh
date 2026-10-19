#!/bin/sh
# Decompiles functions from the saved project, with all of its names and types, to stdout.
# Takes addresses (0x4a96c0), names (enDyingExec) or any address inside a function. Each run starts
# Ghidra headless (about 20 s), so pass every function you need in one call.
# Usage: tools/ghidra/dec.sh <addr|name> ...    (refs: tools/ghidra/dec.sh --refs <addr> ...)
. "$(dirname "$0")/env.sh"
[ $# -gt 0 ] || { echo "usage: $0 [--refs] <addr|name> ..." >&2; exit 2; }
out=$(mktemp "${TMPDIR:-/tmp}/sh2dec.XXXXXX")
if [ "$1" = "--refs" ]; then
	shift
	gh_read -postScript Refs.java "$out" "$@" >/dev/null
else
	gh_read -postScript Decompile.java "$out" "$@"
fi
cat "$out"
rm -f "$out"
