#
# Dockerfile - STM32 build, flash and debug environment
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Target:  NUCLEO-F767ZI (STM32F767ZI, on-board ST-LINK/V2.1)
#
# Contains the arm-none-eabi toolchain, CMake/Ninja, OpenOCD, stlink-tools,
# stm32flash, dfu-util, gdb-multiarch, picocom and the full STM32CubeF7 SDK
# (HAL, LL, CMSIS, BSP, middleware and the NUCLEO-F767ZI example projects).
# Use it through scripts/docker-dev.sh, which passes the ST-LINK USB device
# and the workspace into the container.
#

FROM ubuntu:24.04

LABEL org.opencontainers.image.title="stm32-dev" \
      org.opencontainers.image.description="STM32 build, flash and debug environment" \
      org.opencontainers.image.authors="Sandesh Ghimire <sandesh@soccentric.com>"

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi \
        binutils-arm-none-eabi \
        clang-tidy \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        gdb-multiarch \
        cmake \
        ninja-build \
        make \
        openocd \
        stlink-tools \
        stm32flash \
        dfu-util \
        srecord \
        picocom \
        git \
        python3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Tools such as IDE plugins look for arm-none-eabi-gdb by name.
RUN ln -s /usr/bin/gdb-multiarch /usr/local/bin/arm-none-eabi-gdb

# STM32CubeF7 SDK: HAL/LL drivers, CMSIS core and device headers, startup
# files and linker scripts, board support packages, middleware (FreeRTOS,
# LwIP, FatFS, USB device/host, mbedTLS, LibJPEG, STemWin) and the example
# projects. The drivers and BSPs are git submodules pinned by the release tag.
ARG CUBE_F7_VERSION=v1.17.4
ENV STM32CUBE_F7=/opt/st/STM32CubeF7
RUN git clone --depth 1 --branch "${CUBE_F7_VERSION}" \
        --recurse-submodules --shallow-submodules \
        https://github.com/STMicroelectronics/STM32CubeF7.git "${STM32CUBE_F7}" \
    && rm -rf "${STM32CUBE_F7}/.git" "${STM32CUBE_F7}"/Drivers/*/.git \
    && find "${STM32CUBE_F7}" -name .git -prune -exec rm -rf {} + \
    && printf '%s\n' "${CUBE_F7_VERSION}" > "${STM32CUBE_F7}/VERSION"

LABEL org.opencontainers.image.version="cubef7-${CUBE_F7_VERSION}"

# The container runs as the host user (see docker-dev.sh), so give that user
# a writable home for GDB history and tool caches.
ENV HOME=/tmp/home
RUN mkdir -p /tmp/home && chmod 1777 /tmp/home

WORKDIR /work

CMD ["bash"]
