# The sh23ds packer: everything tools/make_pack.sh needs (Python with numpy, Pillow and pefile, ffmpeg for
# the Bink movies, tex3ds from devkitPro) in one image, so the SD card data can be built from your own copy
# of the game on any computer with Docker. It contains only the port's converters, never game data.
#
#   docker build -f tools/packer/Dockerfile -t sh23ds-packer .
#   docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/sd:/out" sh23ds-packer
# tex3ds is the only devkitPro tool the converters use, and it is statically linked against ImageMagick,
# so it is copied out of the pinned devkitARM image onto a slim Debian.
FROM devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 AS devkitarm

FROM debian:bookworm-slim@sha256:7c7b2c966bc9ee8cedfeef67e0e279108992c77681fa595db4a9d65c06ccc587

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        python3 python3-numpy python3-pil python3-pefile ffmpeg && \
    rm -rf /var/lib/apt/lists/*

COPY --from=devkitarm /opt/devkitpro/tools/bin/tex3ds /usr/local/bin/tex3ds
COPY VERSION /opt/sh23ds/VERSION
COPY tools/make_pack.sh /opt/sh23ds/tools/make_pack.sh
COPY tools/sh2conv /opt/sh23ds/tools/sh2conv
# the stage table and function list that events.py and demos.py read
COPY sh2-3ds-port-kit/data/dc /opt/sh23ds/sh2-3ds-port-kit/data/dc
COPY tools/packer/entrypoint.sh /opt/sh23ds/tools/packer/entrypoint.sh

ENV PY="python3 -I" \
    HOME=/tmp

LABEL org.opencontainers.image.title="sh23ds packer" \
      org.opencontainers.image.description="Builds the sh23ds SD card data from your own Silent Hill 2 PC (Director's Cut) install" \
      org.opencontainers.image.source="https://github.com/ankushKun/sh23ds" \
      org.opencontainers.image.licenses="GPL-3.0-only"

ENTRYPOINT ["/opt/sh23ds/tools/packer/entrypoint.sh"]
