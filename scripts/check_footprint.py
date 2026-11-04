#!/usr/bin/env python3
#
# check_footprint.py - print and enforce the DEMO flash/RAM budget
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  NUCLEO-F767ZI
# SRS:     DNFR-001 (under 128 KB flash and 64 KB RAM)
#
# Usage:
#   scripts/check_footprint.py --size-tool arm-none-eabi-size \
#       --max-flash 131072 --max-ram 65536 build/app.elf

import argparse
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size-tool", default="arm-none-eabi-size")
    parser.add_argument("--max-flash", type=int, default=128 * 1024)
    parser.add_argument("--max-ram", type=int, default=64 * 1024)
    parser.add_argument("elf")
    args = parser.parse_args()

    result = subprocess.run([args.size_tool, args.elf],
                            check=False, text=True, capture_output=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        return result.returncode

    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        print("check_footprint: unexpected size output", file=sys.stderr)
        return 1

    fields = lines[1].split()
    if len(fields) < 3:
        print("check_footprint: unexpected size row", file=sys.stderr)
        return 1

    text, data, bss = (int(fields[0]), int(fields[1]), int(fields[2]))
    flash = text + data
    ram = data + bss

    print(f"footprint: flash {flash} / {args.max_flash} bytes "
          f"({flash * 100.0 / args.max_flash:.1f}%), "
          f"RAM {ram} / {args.max_ram} bytes "
          f"({ram * 100.0 / args.max_ram:.1f}%)")

    ok = True
    if flash > args.max_flash:
        print("check_footprint: flash budget exceeded", file=sys.stderr)
        ok = False
    if ram > args.max_ram:
        print("check_footprint: RAM budget exceeded", file=sys.stderr)
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
