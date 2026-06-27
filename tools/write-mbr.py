#!/usr/bin/env python3
"""Inject a single-partition MBR table into a StinkOS disk image.

Patches the partition-entry block of sector 0 (offsets 446..509) and the
0x55AA boot signature (510..511), preserving the existing bootstrap code at
offsets 0..445. The partition covers the StinkFS data region: type byte
0x53 (StinkOS), bootable flag set, first_lba/sector_count taken from the
arguments.

Why this script: at install time the kernel itself writes a partition
table via kernel/fs/mbr.c, but a freshly-built distribution image needs
the same table so that `dd` to a USB stick or `qemu -drive file=` boots
into a layout that other tooling (parted, fdisk) recognises.
"""

import argparse
import struct
import sys

MBR_PART_OFFSET = 446
MBR_SIGNATURE   = 0x55AA
PART_ENTRY_SIZE = 16
NUM_ENTRIES     = 4

TYPE_STINKOS = 0x53
TYPE_EMPTY   = 0x00
BOOTABLE     = 0x80


def chs_unused():
    """Three zero bytes -- we ignore CHS and rely on LBA fields."""
    return b"\x00\x00\x00"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="path to the disk image to patch")
    ap.add_argument("--start", type=int, default=1,
                    help="first LBA of the StinkOS partition (default 1)")
    ap.add_argument("--count", type=int, required=True,
                    help="sector count for the StinkOS partition")
    ap.add_argument("--type",  type=lambda x: int(x, 0), default=TYPE_STINKOS,
                    help="partition type byte (default 0x53 = StinkOS)")
    args = ap.parse_args()

    with open(args.image, "r+b") as f:
        f.seek(0, 2)
        size = f.tell()
        if size < 512:
            sys.exit(f"{args.image}: smaller than one sector")

        f.seek(MBR_PART_OFFSET)

        # Entry 1: bootable StinkOS partition spanning [start, start+count).
        entry = struct.pack(
            "<B3sB3sII",
            BOOTABLE,
            chs_unused(),
            args.type,
            chs_unused(),
            args.start,
            args.count,
        )
        f.write(entry)

        # Entries 2..4: empty.
        empty = struct.pack(
            "<B3sB3sII", 0, chs_unused(), TYPE_EMPTY, chs_unused(), 0, 0)
        for _ in range(NUM_ENTRIES - 1):
            f.write(empty)

        # 0x55AA boot signature at offsets 510-511.
        f.seek(510)
        f.write(struct.pack("<H", MBR_SIGNATURE))

    print(
        f"patched {args.image}: partition 1 type=0x{args.type:02X} "
        f"start={args.start} count={args.count} sectors"
    )


if __name__ == "__main__":
    sys.exit(main())
