FROM ubuntu:24.04

ARG TARGETPLATFORM

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install base build dependencies. The arm-none-eabi cross toolchain is
# deliberately NOT taken from apt: Ubuntu ships an older gcc that rejects some
# UB torture tests the codesize comparison compiles (e.g.
# gcc.c-torture/compile/pr111059-1, pr111059-2, pr111911-1 -- `1 << -1`,
# `0 / 0`, `INT_MAX + 1`). It is installed from Arm's official release below so
# the version is current and pinned.
RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    g++ \
    make \
    git \
    wget \
    curl \
    xz-utils \
    python3 \
    python3-pip \
    python3-venv \
    virtualenv \
    qemu-user \
    qemu-user-static \
    qemu-system-arm \
    gdb-multiarch \
    texinfo \
    && rm -rf /var/lib/apt/lists/*

# Arm GNU Toolchain (arm-none-eabi): gcc/binutils/newlib/libstdc++. Used as the
# GCC reference for the tcc-vs-gcc codesize comparison, and to link the QEMU
# test binaries (its libgcc + crt objects; libc is a separately built newlib).
# The qemu test Makefile discovers sysroot/libgcc/crt paths dynamically via
# `arm-none-eabi-gcc -print-*`, so it adapts to this layout automatically.
#
# Pinned by version URL. The x86_64 archive is also checked by sha256. Add the
# aarch64 sha256 when bumping or verifying that archive.
ARG ARM_TOOLCHAIN_VERSION=14.3.rel1
ARG ARM_TOOLCHAIN_SHA256_X86_64=8f6903f8ceb084d9227b9ef991490413014d991874a1e34074443c2a72b14dbd
ARG ARM_TOOLCHAIN_SHA256_AARCH64=
RUN set -eux; \
    if [ -z "${TARGETPLATFORM:-}" ]; then \
        case "$(uname -m)" in \
            "x86_64") arm_toolchain_arch="x86_64"; arm_toolchain_sha="$ARM_TOOLCHAIN_SHA256_X86_64" ;; \
            "aarch64"|"arm64") arm_toolchain_arch="aarch64"; arm_toolchain_sha="$ARM_TOOLCHAIN_SHA256_AARCH64" ;; \
            *) echo "Unsupported host architecture: $(uname -m)" >&2; exit 1 ;; \
        esac; \
    else \
        case "$TARGETPLATFORM" in \
            "linux/amd64") arm_toolchain_arch="x86_64"; arm_toolchain_sha="$ARM_TOOLCHAIN_SHA256_X86_64" ;; \
            "linux/arm64") arm_toolchain_arch="aarch64"; arm_toolchain_sha="$ARM_TOOLCHAIN_SHA256_AARCH64" ;; \
            *) echo "Unsupported TARGETPLATFORM: $TARGETPLATFORM" >&2; exit 1 ;; \
        esac; \
    fi; \
    url="https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_TOOLCHAIN_VERSION}/binrel/arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-${arm_toolchain_arch}-arm-none-eabi.tar.xz"; \
    curl -fSL "$url" -o /tmp/arm-toolchain.tar.xz; \
    if [ -n "$arm_toolchain_sha" ]; then \
        echo "${arm_toolchain_sha}  /tmp/arm-toolchain.tar.xz" | sha256sum -c -; \
    else \
        echo "WARNING: no sha256 configured for ${arm_toolchain_arch} Arm toolchain archive" >&2; \
    fi; \
    mkdir -p /opt/arm-gnu-toolchain; \
    tar -xf /tmp/arm-toolchain.tar.xz -C /opt/arm-gnu-toolchain --strip-components=1; \
    rm /tmp/arm-toolchain.tar.xz
ENV PATH="/opt/arm-gnu-toolchain/bin:${PATH}"

# Set working directory
WORKDIR /workspace

# Default command
CMD ["/bin/bash"]
