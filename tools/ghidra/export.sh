#!/bin/sh
# Writes the saved project's state to sh2-3ds-port-kit/data/dc/project (ExportProject.java), so the
# names, comments and function fixes you made in Ghidra can be committed. Run it after every session
# that changed the project, and check `git diff` on that folder before committing.
# Usage: tools/ghidra/export.sh
. "$(dirname "$0")/env.sh"
gh_read -postScript ExportProject.java "$K/data/dc/project"
