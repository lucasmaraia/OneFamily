# Build image for the OneFamily daemon payload.
#
# Holds clang/lld-18 plus the OpenOrbis PS4 toolchain, so a build needs nothing
# installed on the host but Docker. The upstream build_daemon.ps1 hardcodes the
# original author's toolchain path; this image makes the build reproducible.
FROM docker.io/library/ubuntu:24.04

# llvm-18 is here for llvm-config-18, which the toolchain's wrappers consult.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates clang-18 lld-18 llvm-18 curl tar make python3 \
    && rm -rf /var/lib/apt/lists/*

RUN ln -sf /usr/bin/clang-18 /usr/bin/clang \
    && ln -sf /usr/bin/clang++-18 /usr/bin/clang++ \
    && ln -sf /usr/bin/ld.lld-18 /usr/bin/ld.lld \
    && ln -sf /usr/bin/llvm-config-18 /usr/bin/llvm-config

# OpenOrbis supplies link.x, the Orbis headers and libc/libkernel stubs.
# llvm-18 release, to match clang-18 above.
ARG OO_VERSION=v0.5.4
RUN set -eux; \
    curl -fsSL -o /tmp/oo.tar.gz \
        "https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/${OO_VERSION}/toolchain-llvm-18.tar.gz"; \
    mkdir -p /opt/oo; \
    tar xzf /tmp/oo.tar.gz -C /opt/oo; \
    rm /tmp/oo.tar.gz; \
    # The archive's internal layout is not guaranteed, so locate the toolchain
    # root by its linker script and fail the build if it is not there.
    TC="$(dirname "$(find /opt/oo -name link.x -print -quit)")"; \
    test -n "$TC"; \
    test -d "$TC/include"; \
    test -d "$TC/lib"; \
    ln -s "$TC" /opt/ps4toolchain; \
    echo "OpenOrbis toolchain at $TC"

ENV OO_PS4_TOOLCHAIN=/opt/ps4toolchain
WORKDIR /src
