#!/bin/sh
# Fails if any tracked (or, with --staged, staged) file must never be in the repository: hidden
# workspaces, game data or anything converted from it, system firmware, build outputs, or large
# binaries. Run by CI and by the local pre-commit hook (tools/install_hooks.sh).
cd "$(dirname "$0")/.."
if [ "$1" = "--staged" ]; then
	FILES=$(git diff --cached --name-only --diff-filter=ACMR)
else
	FILES=$(git ls-files)
fi
bad=$(printf '%s\n' "$FILES" | grep -E \
	-e '^\.[^/]+/' -e '(^|/)(pack|dist|build|decomp)/' -e '(^|/)Silent[^/]*Hill' \
	-e '\.(cdc|3dsx|cia|elf|smdh|3ds|t3x|s3m|s3l|s3c|s3a|s3e|kg1|idx|tex|tbn2|mdl|anm|map|cld|cam|afs|adx|aix|bik|mpg|dem|exe|dll|gpr|rep)$' \
	| grep -v -E '^\.(github/|gitignore$|dockerignore$)')
big=$(printf '%s\n' "$FILES" | while IFS= read -r f; do
	[ -f "$f" ] && [ "$(wc -c < "$f")" -gt 2000000 ] && echo "$f"; done)
if [ -n "$bad$big" ]; then
	echo "These files must not be committed:" >&2
	printf '%s\n' $bad $big | sed 's/^/  /' >&2
	exit 1
fi
