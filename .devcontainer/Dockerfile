FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Toolchain for building the RP2040 / Pico SDK firmware.
#   - gcc-arm-none-eabi + newlib: cross compiler for the target
#   - build-essential + python3: native host tools the Pico SDK builds
#     during configure (pioasm, elf2uf2, etc.)
#   - cmake/make/git: build system
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        cmake \
        make \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        build-essential \
        python3 \
        git \
        ca-certificates \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
