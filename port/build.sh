#!/bin/sh
# Builds sh23ds.3dsx (and sh23ds.elf for package.sh) in the pinned devkitARM container.
#
#   port/build.sh            build
#   port/build.sh clean      any other make arguments are passed through
#
# Needs Docker (Docker Desktop, or the CLI with Colima on macOS).
set -e
cd "$(dirname "$0")"
IMAGE=${SH23DS_BUILD_IMAGE:-devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5}
JOBS=${SH23DS_BUILD_JOBS:-4}
exec docker run --rm -v "$PWD/..":/work -w /work/port \
  -e DEVKITPRO=/opt/devkitpro -e DEVKITARM=/opt/devkitpro/devkitARM -e HOME=/tmp \
  --user "$(id -u):$(id -g)" "$IMAGE" make -j"$JOBS" "$@"
