#!/usr/bin/env bash
#
# serial.sh - Open a serial console on the ST-LINK virtual COM port
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Target:  NUCLEO-F767ZI, USART3 (PD8 TX / PD9 RX) via ST-LINK -> /dev/ttyACM0
#
# Runs picocom on the host. Quit with Ctrl-A Ctrl-X.
#
# Usage:
#   scripts/serial.sh                       /dev/ttyACM0 at 115200 baud
#   scripts/serial.sh -b 921600             another baud rate
#   scripts/serial.sh -p /dev/ttyACM1       another port
#   scripts/serial.sh -l logs/uart.log      also write everything to a log file
#   scripts/serial.sh -e                    echo typed characters locally
#
# Environment:
#   SERIAL_PORT    default port  (default: /dev/ttyACM0)
#   SERIAL_BAUD    default baud  (default: 115200)
#

set -euo pipefail

PORT="${SERIAL_PORT:-/dev/ttyACM0}"
BAUD="${SERIAL_BAUD:-115200}"
LOGFILE=""
ECHO=0

# ST-LINK virtual COM ports, used when the default port is missing.
STLINK_GLOB="/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_*"

info() { printf '\033[1;34m[*]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

parse_args() {
    while getopts ":p:b:l:eh" opt; do
        case "$opt" in
            p) PORT="$OPTARG" ;;
            b) BAUD="$OPTARG" ;;
            l) LOGFILE="$OPTARG" ;;
            e) ECHO=1 ;;
            h) usage; exit 0 ;;
            :) die "option -$OPTARG needs a value (try -h)" ;;
            *) die "unknown option: -$OPTARG (try -h)" ;;
        esac
    done
}

find_port() {
    [[ -e "$PORT" ]] && return

    # The board may have come back as ttyACM1 after a replug; look it up by
    # its ST-LINK USB name instead.
    local link
    for link in $STLINK_GLOB; do
        if [[ -e "$link" ]]; then
            info "$PORT not found, using ST-LINK port $(readlink -f "$link")"
            PORT="$(readlink -f "$link")"
            return
        fi
    done

    die "$PORT not found and no ST-LINK serial port detected. Is the board plugged in?"
}

check_port() {
    if [[ ! -r "$PORT" || ! -w "$PORT" ]]; then
        die "no permission to open $PORT. Add yourself to the dialout group (sudo usermod -aG dialout \$USER) and log in again."
    fi

    if command -v fuser >/dev/null 2>&1 && fuser "$PORT" >/dev/null 2>&1; then
        die "$PORT is already in use by another program (check: fuser -v $PORT)."
    fi
}

main() {
    parse_args "$@"
    command -v picocom >/dev/null 2>&1 \
        || die "picocom is not installed (run scripts/install-tools.sh, or sudo apt install picocom)."

    find_port
    check_port

    local args=(--baud "$BAUD" --imap lfcrlf)
    [[ $ECHO -eq 1 ]] && args+=(--echo)
    if [[ -n "$LOGFILE" ]]; then
        mkdir -p "$(dirname "$LOGFILE")"
        args+=(--logfile "$LOGFILE")
        info "Logging to $LOGFILE"
    fi

    info "Connecting to $PORT at $BAUD baud (Ctrl-A Ctrl-X to quit)"
    exec picocom "${args[@]}" "$PORT"
}

main "$@"
