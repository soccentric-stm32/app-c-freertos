#!/usr/bin/env python3
#
# mkimage.py - wrap an application binary in a BOOT image file
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  NUCLEO-F767ZI
# SRS:     BHOST-001, BHOST-002; SRS-BOOT-F767 section 5.1
#
# Usage:
#   tools/mkimage.py --input build/app.bin --output build/app.img \
#       --version 0.1.0 --commit <40-hex> --timestamp <unix> \
#       --name stm32-application
#
#   tools/mkimage.py --inspect build/app.img
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
SLOT_SIZE = 0x000F4000      # 976 KB
RAM_START = 0x20000000
RAM_END = 0x20080000

HEADER_FIELDS = (
    ("magic", "<I"),
    ("header_version", "<H"),
    ("header_size", "<H"),
    ("image_type", "<I"),
    ("version", "<I"),
    ("image_size", "<I"),
    ("image_crc32", "<I"),
    ("load_address", "<I"),
    ("entry_address", "<I"),
    ("build_timestamp", "<I"),
)


def semver(value: str) -> int:
    parts = value.split(".")
    if len(parts) != 3:
        raise ValueError(f"version must be major.minor.patch, got {value!r}")
    major, minor, patch = (int(p) & 0xFF for p in parts)
    return (major << 24) | (minor << 16) | patch


def version_text(value: int) -> str:
    return f"{(value >> 24) & 0xFF}.{(value >> 16) & 0xFF}.{value & 0xFF}"


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


def commit_text(raw: bytes) -> str:
    if all(b == 0 for b in raw):
        return ""
    return raw.hex()


def validate_binary(binary: bytes) -> list[str]:
    errors = []
    if len(binary) < 8:
        errors.append("input binary is too small for a vector table")
        return errors
    if len(binary) > SLOT_SIZE:
        errors.append(f"binary is {len(binary)} bytes; the slot is {SLOT_SIZE}")

    sp, entry = struct.unpack_from("<II", binary, 0)
    if not (RAM_START < sp <= RAM_END) or (sp & 0x7):
        errors.append(f"initial SP 0x{sp:08x} is not in internal RAM")
    entry_addr = entry & ~1
    if not (entry & 1) or not (LOAD_ADDRESS <= entry_addr <
                              LOAD_ADDRESS + SLOT_SIZE):
        errors.append(f"reset vector 0x{entry:08x} is not inside the slot")
    return errors


def build_header(binary: bytes, args) -> bytes:
    version = semver(args.version)
    image_crc = zlib.crc32(binary) & 0xFFFFFFFF
    timestamp = int(args.timestamp, 0) if args.timestamp else 0
    entry = struct.unpack_from("<I", binary, 4)[0]

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
    return bytes(header)


def inspect(path: pathlib.Path) -> int:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        print(f"{path}: shorter than a 256-byte header", file=sys.stderr)
        return 1

    header = data[:HEADER_SIZE]
    fields = {}
    offset = 0
    for name, fmt in HEADER_FIELDS:
        fields[name], = struct.unpack_from(fmt, header, offset)
        offset += struct.calcsize(fmt)
    git_commit = header[36:56]
    name = header[56:88].rstrip(b"\0").decode("ascii", "replace")
    header_crc, = struct.unpack_from("<I", header, 252)
    body = data[HEADER_SIZE:]

    errors = []
    if fields["magic"] != MAGIC:
        errors.append("bad magic")
    if fields["header_version"] != HEADER_VERSION:
        errors.append("bad header version")
    if fields["header_size"] != HEADER_SIZE:
        errors.append("bad header size")
    if fields["image_type"] != IMAGE_TYPE_APPLICATION:
        errors.append("bad image type")
    if fields["load_address"] != LOAD_ADDRESS:
        errors.append("bad load address")
    if fields["image_size"] > SLOT_SIZE:
        errors.append("image size exceeds the slot")
    if fields["image_size"] != len(body):
        errors.append("image_size does not match the file body")
    if zlib.crc32(header[:252]) & 0xFFFFFFFF != header_crc:
        errors.append("header CRC32 mismatch")
    if zlib.crc32(body) & 0xFFFFFFFF != fields["image_crc32"]:
        errors.append("image CRC32 mismatch")
    errors.extend(validate_binary(body))

    print(f"file          : {path}")
    print(f"magic         : 0x{fields['magic']:08x}")
    print(f"header        : version {fields['header_version']}, "
          f"{fields['header_size']} bytes")
    print(f"image type    : {fields['image_type']}")
    print(f"version       : {version_text(fields['version'])}")
    print(f"image size    : {fields['image_size']} bytes")
    print(f"image CRC32   : 0x{fields['image_crc32']:08x}")
    print(f"load address  : 0x{fields['load_address']:08x}")
    print(f"entry address : 0x{fields['entry_address']:08x}")
    print(f"build time    : {fields['build_timestamp']}")
    print(f"git commit    : {commit_text(git_commit)}")
    print(f"name          : {name}")
    print(f"header CRC32  : 0x{header_crc:08x}")

    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--version", default="0.1.0")
    parser.add_argument("--commit", default="")
    parser.add_argument("--timestamp", default="0")
    parser.add_argument("--name", default="stm32-application")
    parser.add_argument("--inspect", type=pathlib.Path,
                        help="print and check an existing image file")
    args = parser.parse_args()

    if args.inspect is not None:
        return inspect(args.inspect)

    if args.input is None or args.output is None:
        parser.error("--input and --output are required unless --inspect")

    binary = args.input.read_bytes()
    errors = validate_binary(binary)
    if errors:
        for error in errors:
            print(f"mkimage: {error}", file=sys.stderr)
        return 1

    header = build_header(binary, args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + binary)

    image_crc = zlib.crc32(binary) & 0xFFFFFFFF
    print(f"mkimage: {args.output} ({HEADER_SIZE + len(binary)} bytes, "
          f"version {args.version}, crc 0x{image_crc:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
