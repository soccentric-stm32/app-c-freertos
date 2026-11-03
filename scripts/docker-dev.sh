#!/usr/bin/env bash
#
# docker-dev.sh - Build, flash and debug STM32 firmware inside Docker
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Target:  NUCLEO-F767ZI (STM32F767ZI, on-board ST-LINK/V2.1)
#
# The project directory is mounted at /work and the container runs as the
# calling user, so build output is owned by you. The ST-LINK is passed in
# through /dev/bus/usb and the virtual COM port through /dev/ttyACM*.
# Host networking is used so a GDB or IDE on the host can reach the
# OpenOCD server on port 3333.
#
# Usage:
#   scripts/docker-dev.sh build                  build the Docker image
#   scripts/docker-dev.sh shell                  interactive shell in the container
#   scripts/docker-dev.sh run <cmd> [args...]    run one command in the container
#   scripts/docker-dev.sh probe                  detect the ST-LINK and target chip
#   scripts/docker-dev.sh flash <file> [addr]    flash .elf/.hex, or .bin at addr
#                                                (default 0x08000000), then reset
#   scripts/docker-dev.sh erase                  erase the whole flash
#   scripts/docker-dev.sh reset                  reset the target and let it run
#   scripts/docker-dev.sh gdbserver              run OpenOCD: GDB :3333, telnet :4444
#   scripts/docker-dev.sh debug <elf> [--load]   OpenOCD + GDB session, halted at reset;
#                                                --load flashes the ELF first
#   scripts/docker-dev.sh serial [baud]          serial console on the VCP (115200)
#                                                quit with Ctrl-A Ctrl-X
#
# Environment:
#   STM32_IMAGE    image name        (default: stm32-dev:latest)
#   OPENOCD_CFG    OpenOCD config    (default: board/st_nucleo_f7.cfg)
#   SERIAL_PORT    serial device     (default: first /dev/ttyACM*)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Resolve symlinks (pwd -P) so paths match realpath in container_path().
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd -P)"
DOCKER_DIR="$PROJECT_DIR/docker"

IMAGE="${STM32_IMAGE:-stm32-dev:latest}"
OPENOCD_CFG="${OPENOCD_CFG:-board/st_nucleo_f7.cfg}"

# Linux character device major numbers: 189 = USB devices, 166 = ttyACM.
USB_MAJOR=189
ACM_MAJOR=166

info() { printf '\033[1;34m[*]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

require_docker() {
    command -v docker >/dev/null 2>&1 || die "docker is not installed."
    docker info >/dev/null 2>&1 \
        || die "Cannot talk to the Docker daemon (is it running, and are you in the docker group?)."
}

ensure_image() {
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        info "Image $IMAGE not found, building it"
        cmd_build
    fi
}

# Build the docker run argument list in the global DOCKER_ARGS array.
docker_args() {
    DOCKER_ARGS=(
        --rm
        --init
        --network host
        --user "$(id -u):$(id -g)"
        --volume "$PROJECT_DIR:/work"
        --workdir /work
        --volume /dev/bus/usb:/dev/bus/usb
        --device-cgroup-rule "c $USB_MAJOR:* rmw"
        --device-cgroup-rule "c $ACM_MAJOR:* rmw"
    )

    # Give the container the host's plugdev and dialout groups so device
    # permissions match the host.
    local grp gid
    for grp in plugdev dialout; do
        gid="$(getent group "$grp" | cut -d: -f3 || true)"
        [[ -n "$gid" ]] && DOCKER_ARGS+=(--group-add "$gid")
    done

    local tty
    for tty in /dev/ttyACM*; do
        [[ -e "$tty" ]] && DOCKER_ARGS+=(--device "$tty")
    done

    # Always keep stdin open so piped GDB scripts work; add a TTY only when
    # attached to a terminal.
    DOCKER_ARGS+=(--interactive)
    if [[ -t 0 && -t 1 ]]; then
        DOCKER_ARGS+=(--tty)
    fi
}

in_container() {
    ensure_image
    docker_args
    docker run "${DOCKER_ARGS[@]}" "$IMAGE" "$@"
}

# Turn a host path into the matching path under /work in the container.
container_path() {
    local host_path
    host_path="$(realpath -m "$1")"
    case "$host_path" in
        "$PROJECT_DIR"/*) printf '/work/%s\n' "${host_path#"$PROJECT_DIR"/}" ;;
        *) die "$1 is outside the project directory ($PROJECT_DIR) and is not visible in the container." ;;
    esac
}

cmd_build() {
    info "Building image $IMAGE"
    docker build --tag "$IMAGE" "$DOCKER_DIR"
}

cmd_shell() {
    in_container bash
}

cmd_run() {
    [[ $# -gt 0 ]] || die "run needs a command."
    in_container "$@"
}

cmd_probe() {
    in_container bash -c "st-info --probe && openocd -f '$OPENOCD_CFG' -c 'init; targets; exit'"
}

cmd_flash() {
    [[ $# -ge 1 ]] || die "flash needs a firmware file."
    [[ -f "$1" ]] || die "File not found: $1"

    local file addr
    file="$(container_path "$1")"
    addr="${2:-0x08000000}"

    case "$file" in
        *.elf|*.hex) in_container openocd -f "$OPENOCD_CFG" -c "program $file verify reset exit" ;;
        *.bin)       in_container openocd -f "$OPENOCD_CFG" -c "program $file $addr verify reset exit" ;;
        *)           die "Unsupported file type: $1 (use .elf, .hex or .bin)" ;;
    esac
}

cmd_erase() {
    in_container openocd -f "$OPENOCD_CFG" -c "init; reset halt; flash erase_sector 0 0 last; exit"
}

cmd_reset() {
    in_container openocd -f "$OPENOCD_CFG" -c "init; reset run; exit"
}

cmd_gdbserver() {
    info "OpenOCD listening: GDB on :3333, telnet on :4444 (Ctrl-C to stop)"
    in_container openocd -f "$OPENOCD_CFG" -c "bindto 0.0.0.0"
}

cmd_debug() {
    [[ $# -ge 1 ]] || die "debug needs an ELF file."
    [[ -f "$1" ]] || die "File not found: $1"

    local elf load_cmd=""
    elf="$(container_path "$1")"
    if [[ "${2:-}" == "--load" ]]; then
        load_cmd="-ex load"
    fi

    # OpenOCD runs in the background inside the same container and is
    # stopped when GDB exits. Its log goes to /tmp so it does not clutter
    # the GDB console.
    in_container bash -c "
        openocd -f '$OPENOCD_CFG' -l /tmp/openocd.log &
        ocd=\$!
        trap 'kill \$ocd 2>/dev/null; wait \$ocd 2>/dev/null' EXIT
        for _ in \$(seq 50); do
            (exec 3<>/dev/tcp/127.0.0.1/3333) 2>/dev/null && break
            kill -0 \$ocd 2>/dev/null || { cat /tmp/openocd.log; exit 1; }
            sleep 0.1
        done
        gdb-multiarch -q '$elf' \
            -ex 'target extended-remote :3333' \
            -ex 'monitor reset halt' \
            $load_cmd
    "
}

cmd_serial() {
    local baud="${1:-115200}"
    local port="${SERIAL_PORT:-}"

    if [[ -z "$port" ]]; then
        port="$(ls /dev/ttyACM* 2>/dev/null | head -n 1 || true)"
    fi
    [[ -n "$port" && -e "$port" ]] || die "No serial port found (is the board connected?)."

    info "Opening $port at $baud baud (Ctrl-A Ctrl-X to quit)"
    in_container picocom -b "$baud" "$port"
}

main() {
    [[ $# -gt 0 ]] || { usage; exit 1; }

    local cmd="$1"
    shift

    case "$cmd" in
        -h|--help|help) usage; exit 0 ;;
    esac

    require_docker

    case "$cmd" in
        build)     cmd_build ;;
        shell)     cmd_shell ;;
        run)       cmd_run "$@" ;;
        probe)     cmd_probe ;;
        flash)     cmd_flash "$@" ;;
        erase)     cmd_erase ;;
        reset)     cmd_reset ;;
        gdbserver) cmd_gdbserver ;;
        debug)     cmd_debug "$@" ;;
        serial)    cmd_serial "$@" ;;
        *)         die "Unknown command: $cmd (try --help)" ;;
    esac
}

main "$@"
