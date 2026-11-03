#!/usr/bin/env bash
#
# install-tools.sh - Install the STM32 build, flash and debug toolchain
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Target:  NUCLEO-F767ZI (STM32F767ZI, on-board ST-LINK/V2.1)
# Host:    Ubuntu 24.04 / Debian-based systems
#
# Installs:
#   - arm-none-eabi GCC, binutils and newlib (cross compiler + C library)
#   - gdb-multiarch (debugger for Cortex-M)
#   - cmake, ninja, make (build system)
#   - openocd, stlink-tools (flashing and on-chip debugging)
#   - picocom (serial console for the ST-LINK virtual COM port)
#
# It also adds the current user to the dialout and plugdev groups so the
# ST-LINK and /dev/ttyACM* can be used without sudo.
#
# Usage:
#   ./scripts/install-tools.sh            install everything, then probe the board
#   ./scripts/install-tools.sh --no-probe install only, skip the board check
#

set -euo pipefail

PACKAGES=(
    gcc-arm-none-eabi
    binutils-arm-none-eabi
    libnewlib-arm-none-eabi
    libstdc++-arm-none-eabi-newlib
    gdb-multiarch
    cmake
    ninja-build
    make
    openocd
    stlink-tools
    picocom
)

GROUPS_NEEDED=(dialout plugdev)

PROBE=1

info()  { printf '\033[1;34m[*]\033[0m %s\n' "$*"; }
ok()    { printf '\033[1;32m[+]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --no-probe) PROBE=0 ;;
            -h|--help)  usage; exit 0 ;;
            *)          die "Unknown option: $1 (try --help)" ;;
        esac
        shift
    done
}

check_host() {
    command -v apt-get >/dev/null 2>&1 \
        || die "apt-get not found; this script supports Debian/Ubuntu only."

    if [[ $EUID -eq 0 ]]; then
        SUDO=""
        TARGET_USER="${SUDO_USER:-root}"
    else
        command -v sudo >/dev/null 2>&1 || die "sudo is required."
        SUDO="sudo"
        TARGET_USER="$USER"
    fi
}

install_packages() {
    info "Updating package index"
    $SUDO apt-get update

    info "Installing: ${PACKAGES[*]}"
    $SUDO apt-get install -y --no-install-recommends "${PACKAGES[@]}"
    ok "Packages installed"
}

setup_permissions() {
    local added=0

    for grp in "${GROUPS_NEEDED[@]}"; do
        if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$grp"; then
            ok "$TARGET_USER is already in group $grp"
        else
            info "Adding $TARGET_USER to group $grp"
            $SUDO usermod -aG "$grp" "$TARGET_USER"
            added=1
        fi
    done

    # openocd and stlink-tools ship udev rules for the ST-LINK; reload them
    # so a board that is already plugged in picks up the new permissions.
    info "Reloading udev rules"
    $SUDO udevadm control --reload-rules
    $SUDO udevadm trigger

    if [[ $added -eq 1 ]]; then
        warn "Group membership changed: log out and back in (or run 'newgrp dialout')"
        warn "before using the serial port without sudo."
    fi
}

show_versions() {
    info "Installed tool versions"
    arm-none-eabi-gcc --version | head -1
    gdb-multiarch --version     | head -1
    cmake --version             | head -1
    printf 'ninja %s\n' "$(ninja --version)"
    openocd --version 2>&1      | head -1
    st-info --version
}

probe_board() {
    info "Probing ST-LINK"
    if ! st-info --probe; then
        warn "st-info could not reach the board. Check the USB cable and try again."
        return
    fi

    info "Reading target with OpenOCD"
    if openocd -f board/st_nucleo_f7.cfg -c "init; targets; exit" 2>&1 | tail -n 15; then
        ok "Board responded"
    else
        warn "OpenOCD could not connect; the board may need a replug after the udev reload."
    fi
}

main() {
    parse_args "$@"
    check_host
    install_packages
    setup_permissions
    show_versions

    if [[ $PROBE -eq 1 ]]; then
        probe_board
    fi

    ok "Done. Serial console: picocom -b 115200 /dev/ttyACM0"
}

main "$@"
