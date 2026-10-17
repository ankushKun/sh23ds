#!/bin/sh
# Installs a pre-commit hook that runs tools/check_repo.sh on the staged files.
cd "$(dirname "$0")/.."
printf '#!/bin/sh\nexec tools/check_repo.sh --staged\n' > .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
echo "installed .git/hooks/pre-commit"
