#!/usr/bin/env python3
"""Bundle a set of files into a .stinkpkg archive.

Wire format (matches kernel/sys/stinkpkg.h, all little-endian, no padding):

    magic         u32  "SPKG" = 0x474B5053
    format_ver    u16  1
    flags         u16  reserved
    name          char[32]
    version       char[16]
    description   char[128]
    dep_count     u32
    file_count    u32
    payload_off   u32
    payload_size  u32

    dep_count  x  { name[32] }
    file_count x  { name[32], size u32, offset u32 }
    payload bytes (file 0 || file 1 || ...)

Usage:
    make-stinkpkg.py --name NAME --version VER --desc DESC \\
                     [--dep NAME ...] [--out FILE] FILE [FILE ...]
"""

import argparse
import os
import struct
import sys

MAGIC          = 0x474B5053
FORMAT_VERSION = 1

NAME_LEN = 32
VER_LEN  = 16
DESC_LEN = 128
FILE_LEN = 32

HEADER_FMT = "<IHH32s16s128sIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)            # 196
DEP_SIZE    = NAME_LEN                                # 32
FILE_SIZE   = FILE_LEN + 4 + 4                        # 40


def fixed(s, n, label):
    """Encode `s` as ASCII NUL-padded to exactly `n` bytes."""
    raw = s.encode("ascii", errors="strict")
    if len(raw) >= n:
        raise SystemExit(f"{label} {s!r} is {len(raw)} bytes, max {n - 1}")
    return raw + b"\x00" * (n - len(raw))


def main():
    ap = argparse.ArgumentParser(
        description="Bundle files into a .stinkpkg archive.")
    ap.add_argument("--name",    required=True, help="package short name")
    ap.add_argument("--version", required=True, help="version string")
    ap.add_argument("--desc",    default="",     help="human-readable summary")
    ap.add_argument("--dep",     action="append", default=[],
                    help="dependency package name (repeatable)")
    ap.add_argument("--out",     help="output filename (default: NAME.stinkpkg)")
    ap.add_argument("inputs", nargs="+", metavar="FILE",
                    help="files to bundle into the payload")
    args = ap.parse_args()

    out_path = args.out or f"{args.name}.stinkpkg"

    name_field = fixed(args.name,    NAME_LEN, "package name")
    ver_field  = fixed(args.version, VER_LEN,  "version")
    desc_field = fixed(args.desc,    DESC_LEN, "description")
    deps = [fixed(d, NAME_LEN, "dep name") for d in args.dep]

    files = []
    payload = bytearray()
    cursor = 0
    for path in args.inputs:
        with open(path, "rb") as f:
            data = f.read()
        basename = os.path.basename(path)
        name_bytes = fixed(basename, FILE_LEN, "file name")
        files.append((name_bytes, len(data), cursor))
        payload += data
        cursor  += len(data)

    payload_off  = HEADER_SIZE + len(deps) * DEP_SIZE + len(files) * FILE_SIZE
    payload_size = len(payload)

    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        FORMAT_VERSION,
        0,                        # flags
        name_field,
        ver_field,
        desc_field,
        len(deps),
        len(files),
        payload_off,
        payload_size,
    )

    with open(out_path, "wb") as out:
        out.write(header)
        for d in deps:
            out.write(d)
        for name_bytes, size, off in files:
            out.write(name_bytes)
            out.write(struct.pack("<II", size, off))
        out.write(payload)

    print(
        f"wrote {out_path}: "
        f"{len(files)} files, {len(deps)} deps, "
        f"{payload_size} payload bytes "
        f"(total {payload_off + payload_size} bytes)"
    )


if __name__ == "__main__":
    sys.exit(main())
