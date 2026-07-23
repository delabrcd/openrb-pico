FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# One image for both building and debugging the RP2040 / Pico SDK firmware.
# Build toolchain:
#   - ARM GNU Toolchain (arm-none-eabi) 15.2.rel1: cross compiler for the target,
#     installed from ARM's official bundle below (NOT apt: Ubuntu 24.04's
#     gcc-arm-none-eabi is stuck at 13.2, which lacks C++23 std::start_lifetime_as).
#   - build-essential + python3: native host tools the Pico SDK builds
#     during configure (pioasm, elf2uf2, etc.)
#   - cmake/make/git: build system
#   - curl + xz-utils: fetch/extract the ARM toolchain bundle at build time
# Debug toolchain (used by the privileged dbg/monitor compose services):
#   - openocd: SWD flash / reset via the CMSIS-DAP probe
#   - gdb-multiarch: on-target debugging
#   - gawk + procps: timestamped UART monitor (docker/uart-monitor.sh)
# Code-intelligence toolchain (used by the `mcp` compose service):
#   - clangd: C/C++ language server. Reads <build dir>/compile_commands.json and
#     uses --query-driver=arm-none-eabi-* to learn the cross system include paths.
#   - nodejs + npm: runtime for the clangd MCP server (built below).
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        cmake \
        make \
        build-essential \
        python3 \
        git \
        ca-certificates \
        curl \
        xz-utils \
        openocd \
        gdb-multiarch \
        gawk \
        procps \
        clangd \
        clang-tidy \
        nodejs \
        npm \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# ARM GNU Toolchain (arm-none-eabi) -- pinned to 15.2.rel1 from ARM's official release
# bundle. Ubuntu 24.04's apt gcc-arm-none-eabi is stuck at GCC 13.2, which lacks the C++23
# std::start_lifetime_as the firmware relies on for well-defined typed views over wire
# buffers (no unions, no -fno-strict-aliasing). Integrity-checked against the SHA-256 we
# computed from ARM's bundle (their published .sha256 sibling is actually an MD5). Extracted
# to /opt and symlinked onto /usr/bin so `arm-none-eabi-*` and clangd's
# --query-driver=/usr/bin/arm-none-eabi-* glob resolve. Bump both ARGs together to upgrade.
ARG ARM_TOOLCHAIN_VERSION=15.2.rel1
ARG ARM_TOOLCHAIN_SHA256=597893282ac8c6ab1a4073977f2362990184599643b4c5ee34870a8215783a16
RUN curl -fsSL -o /tmp/arm-toolchain.tar.xz \
        "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_TOOLCHAIN_VERSION}/binrel/arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi.tar.xz" \
    && echo "${ARM_TOOLCHAIN_SHA256}  /tmp/arm-toolchain.tar.xz" | sha256sum -c - \
    && mkdir -p /opt/arm-gnu-toolchain \
    && tar -xJf /tmp/arm-toolchain.tar.xz -C /opt/arm-gnu-toolchain --strip-components=1 \
    && rm /tmp/arm-toolchain.tar.xz \
    && ln -sf /opt/arm-gnu-toolchain/bin/arm-none-eabi-* /usr/bin/
ENV PATH="/opt/arm-gnu-toolchain/bin:${PATH}"

# clangd MCP server (felipeerias/clangd-mcp-server): bridges clangd's LSP to the
# Model Context Protocol so Claude Code gets semantic C/C++ navigation
# (find_definition / find_references / diagnostics / call-hierarchy). Built from
# source and `npm link`ed onto PATH as `clangd-mcp-server`. Pin via the build arg
# for reproducibility; bump CLANGD_MCP_REF to upgrade.
ARG CLANGD_MCP_REF=main
RUN git clone https://github.com/felipeerias/clangd-mcp-server.git /opt/clangd-mcp-server \
    && cd /opt/clangd-mcp-server \
    && git checkout "${CLANGD_MCP_REF}" \
    && npm install \
    && npm run build \
    && npm link \
    && npm cache clean --force
