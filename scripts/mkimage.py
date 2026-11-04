#!/usr/bin/env python3
#
# mkimage.py - wrap an application binary in a BOOT image file
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  NUCLEO-F767ZI
# SRS:     DGEN-001, DGEN-002; SRS-BOOT-F767 section 5.1
#
# Usage:
#   scripts/mkimage.py --input build/demo.bin --output build/demo.img \
#       --version 0.1.0 --commit <40-hex> --timestamp <unix> \
#       --name stm32-application
#
# The header is 256 bytes; the binary follows unchanged.  BOOT stores this
# header in the slot metadata sector (IMG-006).

import argparse
import pathlib
import struct
import sys
import zlib

MAGIC = 0x48574249          # "IBWH" little-endian
HEADER_VERSION = 1
HEADER_SIZE = 256
IMAGE_TYPE_APPLICATION = 1
LOAD_ADDRESS = 0x0800C000


def semver(value: str) -> int:
    parts = value.split(".")
    if len(parts) != 3:
        raise ValueError(f"version must be major.minor.patch, got {value!r}")
    major, minor, patch = (int(p) & 0xFF for p in parts)
    return (major << 24) | (minor << 16) | patch


def commit_bytes(value: str) -> bytes:
    raw = bytes(20)
    text = value.strip()
    if len(text) >= 40:
        try:
            raw = bytes.fromhex(text[:40])
        except ValueError:
            raw = text[:20].encode("ascii", "replace").ljust(20, b"\0")
    elif text:
        raw = text[:20].encode("ascii", "replace").ljust(20, b"\0")
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", default="")
    parser.add_argument("--timestamp", default="0")
    parser.add_argument("--name", default="stm32-application")
    args = parser.parse_args()

    binary = args.input.read_bytes()
    if not binary:
        print("mkimage: input binary is empty", file=sys.stderr)
        return 1
    if len(binary) > (0x08100000 - LOAD_ADDRESS):
        print("mkimage: binary exceeds the 976 KB slot", file=sys.stderr)
        return 1

    entry = struct.unpack_from("<I", binary, 4)[0]
    version = semver(args.version)
    image_crc = zlib.crc32(binary) & 0xFFFFFFFF
    timestamp = int(args.timestamp, 0) if args.timestamp else 0

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<I", header, 0, MAGIC)
    struct.pack_into("<H", header, 4, HEADER_VERSION)
    struct.pack_into("<H", header, 6, HEADER_SIZE)
    struct.pack_into("<I", header, 8, IMAGE_TYPE_APPLICATION)
    struct.pack_into("<I", header, 12, version)
    struct.pack_into("<I", header, 16, len(binary))
    struct.pack_into("<I", header, 20, image_crc)
    struct.pack_into("<I", header, 24, LOAD_ADDRESS)
    struct.pack_into("<I", header, 28, entry)
    struct.pack_into("<I", header, 32, timestamp)
    header[36:56] = commit_bytes(args.commit)
    header[56:88] = args.name.encode("ascii", "replace")[:32].ljust(32, b"\0")
    struct.pack_into("<I", header, 252, zlib.crc32(header[:252]) & 0xFFFFFFFF)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bytes(header) + binary)

    print(f"mkimage: {args.output} ({HEADER_SIZE + len(binary)} bytes, "
          f"version {args.version}, crc 0x{image_crc:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
