FROM ubuntu:22.04@sha256:2edbbc5dc405e9612ba3584ce95480277e3eb374407b5505fe26f17df77c7dbc

ARG MAKEROM_URL="https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.3/makerom-v0.18.3-ubuntu_x86_64.zip"
ARG MAKEROM_SHA256="6f36f77946742d2b8286a92d887e845e06d430998ed4a9c4856d8ea2d4eebd6f"
ARG BANNERTOOL_URL="https://github.com/diasurgical/bannertool/releases/download/1.2.0/bannertool.zip"
ARG BANNERTOOL_SHA256="69768596f836acb3e3aeaa66e47c6ba560dde813c6dfcd33c8afc25fe29b7524"

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl unzip && \
    rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "${MAKEROM_URL}" -o /tmp/makerom.zip && \
    echo "${MAKEROM_SHA256}  /tmp/makerom.zip" | sha256sum -c - && \
    unzip /tmp/makerom.zip -d /tmp/makerom && \
    install -m 0755 /tmp/makerom/makerom /usr/local/bin/makerom && \
    rm -rf /tmp/makerom /tmp/makerom.zip

RUN curl -fsSL "${BANNERTOOL_URL}" -o /tmp/bannertool.zip && \
    echo "${BANNERTOOL_SHA256}  /tmp/bannertool.zip" | sha256sum -c - && \
    unzip /tmp/bannertool.zip linux-x86_64/bannertool -d /tmp/bannertool && \
    install -m 0755 /tmp/bannertool/linux-x86_64/bannertool \
        /usr/local/bin/bannertool && \
    rm -rf /tmp/bannertool /tmp/bannertool.zip

RUN test -x /usr/local/bin/makerom && \
    test -x /usr/local/bin/bannertool
